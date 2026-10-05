#include "Player.hpp"

#include <cmath>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>

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
        bool movementOff = false;
        bool hostMenu = false;
        double halfHeight = 90.0;
        FVec lastSet{};
        // ponytail: temporary flat floor under the player until Collision.cpp streams the real level.
        bool floorValid = false;
        double floorTop = 0, floorX = 0, floorZ = 0;

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

        // Minecraft moves its player to sky.pos (kept current while we aren't puppeting) when this bumps.
        void Teleport()
        {
            ++sky.teleportSeq;
            puppeting = false;
        }

        // One 8-block collision region of the temporary floor: its top at floorTop.
        void SendFloorRegion(int rx, int ry, int rz)
        {
            const int x0 = rx * 8, y0 = ry * 8, z0 = rz * 8;
            const int topBlock = static_cast<int>(std::floor(floorTop));
            const int layers = static_cast<int>(std::lround((floorTop - topBlock) * 8.0));  // 1/8ths in the top block
            const int surfaceBlock = layers ? topBlock : topBlock - 1;
            proto::ColRegion region{x0, y0, z0, x0 + 7, y0 + 7, z0 + 7, sky.collisionEpoch, 0};

            std::vector<std::uint8_t> tris(sizeof(region));
            if (surfaceBlock >= y0 && surfaceBlock <= y0 + 7)
            {
                const float y = static_cast<float>(floorTop), ax = float(x0), az = float(z0), bx = float(x0 + 8), bz = float(z0 + 8);
                const proto::ColTri t[2] = {{{ax, y, az, bx, y, az, bx, y, bz}, 0}, {{ax, y, az, bx, y, bz, ax, y, bz}, 0}};
                tris.insert(tris.end(), reinterpret_cast<const std::uint8_t*>(t), reinterpret_cast<const std::uint8_t*>(t) + sizeof(t));
                region.count = 2;
            }
            std::memcpy(tris.data(), &region, sizeof(region));
            Link::Get().WriteCollision(proto::kColTris, tris.data(), static_cast<std::uint32_t>(tris.size()));

            std::vector<proto::ColBlock> blocks;
            for (int y = y0; y <= y0 + 7; ++y)
            {
                std::uint64_t bits[8]{};
                if (y < topBlock)
                {
                    std::fill(std::begin(bits), std::end(bits), ~0ull);
                }
                else if (y == topBlock)
                {
                    std::fill(bits, bits + layers, ~0ull);
                }
                if (!bits[0])
                {
                    continue;
                }
                for (int z = z0; z <= z0 + 7; ++z)
                {
                    for (int x = x0; x <= x0 + 7; ++x)
                    {
                        proto::ColBlock b{x, y, z, 0, {}};
                        std::memcpy(b.bits, bits, sizeof(bits));
                        blocks.push_back(b);
                    }
                }
            }
            region.count = static_cast<std::uint32_t>(blocks.size());
            std::vector<std::uint8_t> payload(sizeof(region) + blocks.size() * sizeof(proto::ColBlock));
            std::memcpy(payload.data(), &region, sizeof(region));
            std::memcpy(payload.data() + sizeof(region), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
            Link::Get().WriteCollision(proto::kColRegion, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        // Re-centres the floor when the player has walked far from where it was last sent.
        void UpdateFloor(const Vec3& feetMc)
        {
            if (!floorValid)
            {
                floorTop = std::floor(feetMc.y * 8.0) / 8.0;
            }
            else if (std::abs(feetMc.x - floorX) < 16 && std::abs(feetMc.z - floorZ) < 16)
            {
                return;
            }
            floorValid = true;
            floorX = feetMc.x;
            floorZ = feetMc.z;
            const int cx = static_cast<int>(std::floor(floorX / 8)), cz = static_cast<int>(std::floor(floorZ / 8));
            const int topBlock = static_cast<int>(std::floor(floorTop));
            const int ryLow = static_cast<int>(std::floor((topBlock - 1) / 8.0)), ryHigh = static_cast<int>(std::floor(topBlock / 8.0));
            for (int rz = cz - 4; rz <= cz + 4; ++rz)
            {
                for (int rx = cx - 4; rx <= cx + 4; ++rx)
                {
                    for (int ry = ryLow; ry <= ryHigh; ++ry)
                    {
                        SendFloorRegion(rx, ry, rz);
                    }
                }
            }
            Log(L"temporary floor at y {:.3f} around ({:.1f}, {:.1f})", floorTop, floorX, floorZ);
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
                floorValid = false;
            }
            Teleport();
            Log(L"player character {} (capsule half height {:.1f} cm), world {:08X}", pawn->GetName(), halfHeight, sky.worldId);
        }
        if (haveMc && link.McPid() != lastMcPid)
        {
            lastMcPid = link.McPid();
            Teleport();
            Log(L"Minecraft connected (pid {})", lastMcPid);
        }

        // The game moved the player itself (respawn, portal, cutscene): Minecraft follows.
        if (puppeting && std::hypot(loc.X - lastSet.X, loc.Y - lastSet.Y, loc.Z - lastSet.Z) > kTeleportCm)
        {
            Log(L"game teleported the player; re-syncing Minecraft");
            Teleport();
        }
        if (!mcInWorld)
        {
            puppeting = false;
        }
        if (!puppeting)
        {
            const Vec3 feet = UeToMc({loc.X, loc.Y, loc.Z - halfHeight});
            sky.posX = feet.x;
            sky.posY = feet.y;
            sky.posZ = feet.z;
        }
        else if (!puppeting && mc.teleportAck == sky.teleportSeq)
        {
            puppeting = true;
            Log(L"Minecraft now drives the player");
        }

        if (puppeting)
        {
            if (!movementOff)
            {
                SetMovementMode(pawn, kMoveNone);
                movementOff = true;
            }
            const Vec3 feet = McToUe({mc.x, mc.y, mc.z});
            lastSet = {feet.x, feet.y, feet.z + halfHeight};
            SetLocation(pawn, lastSet);
            UpdateFloor({mc.x, mc.y, mc.z});
        }
        else if (movementOff)
        {
            SetMovementMode(pawn, kMoveWalking);
            movementOff = false;
            Log(L"Minecraft gone; the game drives the player again");
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
