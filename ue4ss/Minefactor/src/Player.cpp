#include "Player.hpp"

#include <cmath>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>

#include "Collision.hpp"
#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Reflect.hpp"

using namespace RC::Unreal;

namespace mf
{
    namespace
    {
        struct FVec
        {
            double X, Y, Z;
        };
        struct FRot
        {
            double Pitch, Yaw, Roll;
        };

        constexpr std::uint8_t kMoveNone = 0;
        constexpr std::uint8_t kMoveWalking = 1;
        constexpr double kTeleportCm = 200.0;  // the game moved the player further than this: re-sync MC

        FWeakObjectPtr controllerRef;
        UObject* lastPawn = nullptr;
        std::uint64_t nextFind = 0;
        std::uint64_t nextSlow = 0;
        proto::SkyState sky{};
        std::uint32_t lastMcPid = 0;
        bool puppeting = false;
        bool enabled = false;        // F8: Minecraft drives the player
        bool togglePending = false;  // set by the window hook, handled on the next tick
        bool movementOff = false;
        bool hostMenu = false;
        double halfHeight = 90.0;
        FVec lastSet{};

        UObject* Controller()
        {
            if (auto* pc = controllerRef.Get())
            {
                return pc;
            }
            const auto now = ::GetTickCount64();
            if (now < nextFind)
            {
                return nullptr;
            }
            nextFind = now + 1000;
            UObject* pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
            controllerRef = pc;
            return pc;
        }

        UObject* Pawn(UObject* pc)
        {
            auto call = reflect::Begin(pc, STR("K2_GetPawn"));
            if (!call)
            {
                return nullptr;
            }
            reflect::Invoke(call);
            return reflect::Get<UObject*>(call, STR("ReturnValue"));
        }

        FVec Location(UObject* actor)
        {
            auto call = reflect::Begin(actor, STR("K2_GetActorLocation"));
            if (!call)
            {
                return {};
            }
            reflect::Invoke(call);
            return reflect::Get<FVec>(call, STR("ReturnValue"));
        }

        void SetLocation(UObject* actor, FVec where)
        {
            auto call = reflect::Begin(actor, STR("K2_SetActorLocation"));
            if (!call)
            {
                return;
            }
            reflect::Set(call, STR("NewLocation"), where);
            reflect::Set(call, STR("bSweep"), false);
            reflect::Set(call, STR("bTeleport"), true);
            reflect::Invoke(call);
        }

        FRot ControlRotation(UObject* pc)
        {
            auto call = reflect::Begin(pc, STR("GetControlRotation"));
            if (!call)
            {
                return {};
            }
            reflect::Invoke(call);
            return reflect::Get<FRot>(call, STR("ReturnValue"));
        }

        UObject* Component(UObject* actor, const TCHAR* property)
        {
            auto** ptr = actor->GetValuePtrByPropertyName<UObject*>(property);
            return ptr ? *ptr : nullptr;
        }

        void SetMovementMode(UObject* pawn, std::uint8_t mode)
        {
            auto call = reflect::Begin(Component(pawn, STR("CharacterMovement")), STR("SetMovementMode"));
            if (!call)
            {
                return;
            }
            reflect::Set(call, STR("NewMovementMode"), mode);
            reflect::Invoke(call);
        }

        double CapsuleHalfHeight(UObject* pawn)
        {
            auto call = reflect::Begin(Component(pawn, STR("CapsuleComponent")), STR("GetScaledCapsuleHalfHeight"));
            if (!call)
            {
                return 90.0;
            }
            reflect::Invoke(call);
            return reflect::Get<float>(call, STR("ReturnValue"));
        }

        bool BoolProperty(UObject* object, const TCHAR* name)
        {
            auto* prop = CastField<FBoolProperty>(object->GetClassPrivate()->FindProperty(FName(name, FNAME_Find)));
            return prop && prop->GetPropertyValueInContainer(object);
        }

        void ViewportSize(UObject* pc, std::uint32_t& w, std::uint32_t& h)
        {
            auto call = reflect::Begin(pc, STR("GetViewportSize"));
            if (!call)
            {
                return;
            }
            reflect::Invoke(call);
            const auto x = reflect::Get<std::int32_t>(call, STR("SizeX"));
            const auto y = reflect::Get<std::int32_t>(call, STR("SizeY"));
            if (x > 0 && y > 0)
            {
                w = static_cast<std::uint32_t>(x);
                h = static_cast<std::uint32_t>(y);
            }
        }

        // Abiotic Factor's HUD warning line. Level: 0 green, 1 gray, 2 yellow, 3 red, 4 purple.
        void Message(UObject* pawn, const wchar_t* text, std::uint8_t level)
        {
            auto call = reflect::Begin(pawn, STR("Client_DisplayWarningMessage"));
            if (!call)
            {
                return;
            }
            // Params in declaration order: Message (FText), CriticalityLevel (byte enum), WarningBeep (bool).
            // ponytail: the FText is never destructed (a few bytes per message); fine for rare toggles.
            int i = 0;
            for (FProperty* prop : call.function->ForEachProperty())
            {
                void* at = &call.params[prop->GetOffset_Internal()];
                if (i == 0)
                {
                    new (at) FText(text);
                }
                else if (i == 1)
                {
                    *static_cast<std::uint8_t*>(at) = level;
                }
                ++i;
            }
            reflect::Invoke(call);
        }

        std::uint32_t Hash(const std::wstring& s)
        {
            std::uint32_t h = 2166136261u;  // FNV-1a
            for (wchar_t c : s)
            {
                h = (h ^ static_cast<std::uint32_t>(c)) * 16777619u;
            }
            return h ? h : 1;
        }

        double Normalize(double degrees)
        {
            degrees = std::fmod(degrees, 360.0);
            return degrees > 180.0 ? degrees - 360.0 : degrees < -180.0 ? degrees + 360.0 : degrees;
        }


        // Minecraft moves its player to sky.pos (kept current while we aren't puppeting) when this
        // bumps, and holds it there until it has collision underneath.
        void Teleport(const Vec3&)
        {
            ++sky.teleportSeq;
            puppeting = false;
        }

        void ClockCheck(const proto::McState& mc)
        {
            if (!mc.tickQpc)
            {
                return;
            }
            LARGE_INTEGER now{}, freq{};
            ::QueryPerformanceCounter(&now);
            ::QueryPerformanceFrequency(&freq);
            // MC writes 100 ns ticks of clock_gettime; convert our QPC to the same unit.
            const double hostTicks = static_cast<double>(now.QuadPart) * (1.0e7 / static_cast<double>(freq.QuadPart));
            Log(L"clock check: host QPC - MC tick time = {:.1f} ms (same clock if 0..~60)", (hostTicks - static_cast<double>(mc.tickQpc)) / 1.0e4);
        }
    }

    bool Puppeting()
    {
        return puppeting;
    }

    bool HostMenuOpen()
    {
        return hostMenu;
    }

    void ToggleMinecraft()
    {
        togglePending = true;
    }

    void PlayerTick()
    {
        auto& link = Link::Get();
        link.Heartbeat();
        proto::McState mc{};
        const bool haveMc = link.McAlive() && link.ReadMcState(mc);
        const bool mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);

        UObject* pc = Controller();
        UObject* pawn = pc ? Pawn(pc) : nullptr;
        const bool inGame = pawn && pawn->GetClassPrivate()->GetName().starts_with(STR("Abiotic_PlayerCharacter"));
        if (!inGame)
        {
            if (lastPawn)
            {
                Log(L"player character gone (menu or loading)");
            }
            lastPawn = nullptr;
            puppeting = movementOff = false;
            sky.flags = proto::kSkyLoading;
            link.WriteSkyState(sky);
            return;
        }

        const FVec loc = Location(pawn);
        if (pawn != lastPawn)
        {
            lastPawn = pawn;
            halfHeight = CapsuleHalfHeight(pawn);
            movementOff = false;
            const auto world = pawn->GetWorld();
            const auto worldId = world ? Hash(world->GetName()) : 1u;
            if (worldId != sky.worldId)
            {
                sky.worldId = worldId;
                ++sky.collisionEpoch;
            }
            Teleport(UeToMc({loc.X, loc.Y, loc.Z - halfHeight}));
            Log(L"player character {} (capsule half height {:.1f} cm), world {:08X}", pawn->GetName(), halfHeight, sky.worldId);
        }
        if (haveMc && link.McPid() != lastMcPid)
        {
            lastMcPid = link.McPid();
            Teleport(UeToMc({loc.X, loc.Y, loc.Z - halfHeight}));
            Log(L"Minecraft connected (pid {}); press F8 in game to hand it the controls", lastMcPid);
        }

        if (togglePending)
        {
            togglePending = false;
            enabled = !enabled;
            if (enabled && !mcInWorld)
            {
                enabled = false;
                Message(pawn, L"Minefactor: Minecraft isn't running (start it with tools/play.sh)", 3);
            }
            else
            {
                Message(pawn, enabled ? L"Minefactor: Minecraft controls ON (F8 to turn off)" : L"Minefactor: Minecraft controls OFF", enabled ? 0 : 1);
                if (enabled)
                {
                    SetMovementMode(pawn, kMoveNone);  // hold still while Minecraft comes to us
                    movementOff = true;
                    Teleport(UeToMc({loc.X, loc.Y, loc.Z - halfHeight}));
                }
            }
            Log(L"F8: Minecraft controls {}", enabled ? L"on" : L"off");
        }
        if (!enabled || !mcInWorld)
        {
            if (enabled)
            {
                enabled = false;
                Message(pawn, L"Minefactor: lost Minecraft; controls OFF", 3);
            }
            puppeting = false;
        }

        // The game moved the player itself (respawn, portal, cutscene): Minecraft follows.
        if (puppeting && std::hypot(loc.X - lastSet.X, loc.Y - lastSet.Y, loc.Z - lastSet.Z) > kTeleportCm)
        {
            Log(L"game teleported the player; re-syncing Minecraft");
            Teleport(UeToMc({loc.X, loc.Y, loc.Z - halfHeight}));
        }
        if (!puppeting)
        {
            const Vec3 feet = UeToMc({loc.X, loc.Y, loc.Z - halfHeight});
            sky.posX = feet.x;
            // Up to the next 1/8 block: the floor's top voxel may poke above the real surface, and
            // Minecraft never pushes you out of collision you start inside.
            sky.posY = std::ceil(feet.y * 8.0) / 8.0 + 0.001;
            sky.posZ = feet.z;
            if (enabled)
            {
                if (mc.teleportAck == sky.teleportSeq)
                {
                    puppeting = true;
                    Log(L"Minecraft now drives the player");
                }
            }
        }

        if (enabled && !movementOff)  // e.g. a new character after respawning
        {
            SetMovementMode(pawn, kMoveNone);
            movementOff = true;
        }
        if (puppeting)
        {
            const Vec3 feet = McToUe({mc.x, mc.y, mc.z});
            lastSet = {feet.x, feet.y, feet.z + halfHeight};
            SetLocation(pawn, lastSet);
        }
        else if (movementOff && !enabled)
        {
            SetMovementMode(pawn, kMoveWalking);
            movementOff = false;
            Log(L"Minecraft gone; the game drives the player again");
        }

        if (mcInWorld)
        {
            const Vec3 here = puppeting ? Vec3{mc.x, mc.y, mc.z} : UeToMc({loc.X, loc.Y, loc.Z - halfHeight});
            CollisionTick(pawn->GetWorld(), here, sky.collisionEpoch);
        }

        // Look stays the game's own (its mouse handling, zero latency); Minecraft follows it.
        const FRot rot = ControlRotation(pc);
        sky.yaw = static_cast<float>(Normalize(UeYawToMc(rot.Yaw)));
        sky.pitch = static_cast<float>(UePitchToMc(Normalize(rot.Pitch)));
        hostMenu = BoolProperty(pc, STR("bShowMouseCursor"));
        sky.flags = proto::kSkyInGame | (hostMenu ? proto::kSkyMenuOpen : 0u);
        sky.gameHour = 12.0f;

        const auto now = ::GetTickCount64();
        if (now >= nextSlow)
        {
            nextSlow = now + 5000;
            ViewportSize(pc, sky.viewportW, sky.viewportH);
            if (haveMc)
            {
                ClockCheck(mc);
            }
        }
        link.WriteSkyState(sky);
    }
}
