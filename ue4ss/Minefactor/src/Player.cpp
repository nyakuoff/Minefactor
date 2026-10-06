#include "Player.hpp"

#include <algorithm>
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

#include "Actors.hpp"
#include "Collision.hpp"
#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Overlay.hpp"
#include "Perf.hpp"
#include "Render.hpp"
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
            auto** ptr = actor->GetValuePtrByPropertyNameInChain<UObject*>(property);
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

        // The game's own fall damage stays off while Minecraft (which has its own) moves the player:
        // our small per-frame position steps on stairs otherwise count as falls.
        bool fallDamageOff = false;
        bool fallDamageWas = true;

        void GameFallDamage(UObject* pawn, bool on)
        {
            auto* flag = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("TakeFallDamage"));
            if (!flag || on == !fallDamageOff)
            {
                return;
            }
            if (!on)
            {
                fallDamageWas = *flag;
            }
            *flag = on ? fallDamageWas : false;
            fallDamageOff = !on;
        }

        // Shown height: Minecraft steps up stairs and voxel edges instantly; ease small changes in so
        // the camera doesn't jump (visual only, Minecraft's physics are untouched).
        bool haveShown = false;
        double shownZ = 0;
        LARGE_INTEGER lastTick{};

        double SmoothZ(double target, bool onGround)
        {
            LARGE_INTEGER now{}, freq{};
            ::QueryPerformanceCounter(&now);
            ::QueryPerformanceFrequency(&freq);
            const double dt = lastTick.QuadPart ? double(now.QuadPart - lastTick.QuadPart) / double(freq.QuadPart) : 0.0;
            lastTick = now;
            if (!haveShown || std::abs(target - shownZ) > 60.0)  // more than a step: teleport or fast fall
            {
                shownZ = target;
                haveShown = true;
            }
            else
            {
                // On the ground: ~70 ms to settle a stair step. In the air: catch up quickly (jumps).
                shownZ += (target - shownZ) * std::min(1.0, dt * (onGround ? 15.0 : 40.0));
            }
            return shownZ;
        }

        // Minecraft's feet at this moment on our clock, from its last two 20 Hz physics ticks (what its
        // own renderer does with partial ticks; the clocks match, see ClockCheck), so the two games'
        // frame rates don't beat against each other.
        Vec3 InterpolatedFeet(const proto::McState& mc)
        {
            if (!mc.tickQpc || mc.tickMs <= 0)
            {
                return {mc.x, mc.y, mc.z};
            }
            LARGE_INTEGER now{}, freq{};
            ::QueryPerformanceCounter(&now);
            ::QueryPerformanceFrequency(&freq);
            const double nowTicks = double(now.QuadPart) * (1.0e7 / double(freq.QuadPart));
            const double a = std::clamp((nowTicks - double(mc.tickQpc)) / (double(mc.tickMs) * 1.0e4), 0.0, 1.0);
            return {mc.prevX + (mc.curX - mc.prevX) * a, mc.prevY + (mc.curY - mc.prevY) * a, mc.prevZ + (mc.curZ - mc.prevZ) * a};
        }

        // Minecraft's F5 views through a camera actor of our own (the game's first-person camera stays
        // untouched); the game's character is hidden meanwhile (Minecraft draws the body).
        FWeakObjectPtr cameraRef;
        bool thirdPerson = false;

        void ThirdPerson(UObject* pc, UObject* pawn, const proto::McState& mc, bool on)
        {
            UObject* camera = cameraRef.Get();
            if (on && !camera)
            {
                UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CameraActor"));
                auto spawn = reflect::Begin(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics")),
                                            STR("BeginDeferredActorSpawnFromClass"));
                if (!cls || !spawn)
                {
                    return;
                }
                reflect::Set(spawn, STR("WorldContextObject"), pawn);
                reflect::Set(spawn, STR("ActorClass"), static_cast<UObject*>(cls));
                if (auto* prop = CastField<FStructProperty>(spawn.function->FindProperty(FName(STR("SpawnTransform"), FNAME_Find))))
                {
                    auto* st = prop->GetStruct().Get();
                    auto* at = &spawn.params[prop->GetOffset_Internal()];
                    reinterpret_cast<double*>(at + st->FindProperty(FName(STR("Rotation"), FNAME_Find))->GetOffset_Internal())[3] = 1.0;
                    auto* sc = reinterpret_cast<double*>(at + st->FindProperty(FName(STR("Scale3D"), FNAME_Find))->GetOffset_Internal());
                    sc[0] = sc[1] = sc[2] = 1.0;
                }
                reflect::Set<std::uint8_t>(spawn, STR("CollisionHandlingOverride"), 1);
                reflect::Invoke(spawn);
                camera = reflect::Get<UObject*>(spawn, STR("ReturnValue"));
                if (auto finish = reflect::Begin(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics")),
                                                 STR("FinishSpawningActor")))
                {
                    reflect::Set(finish, STR("Actor"), camera);
                    std::memcpy(reflect::ParamAddr(finish, STR("SpawnTransform")), reflect::ParamAddr(spawn, STR("SpawnTransform")),
                                reflect::ParamSize(spawn, STR("SpawnTransform")));
                    reflect::Invoke(finish);
                }
                cameraRef = camera;
            }
            if (on && camera)
            {
                // Minecraft's own third-person camera: behind (1) or in front looking back (2), at the
                // distance it settled on after its own collision.
                constexpr double kDeg = 3.14159265358979 / 180.0;
                const double yaw = mc.yaw * kDeg, pitch = mc.pitch * kDeg;
                const Vec3 fwd{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                const double d = (mc.cameraMode == 2 ? 1.0 : -1.0) * mc.cameraDistance;
                const Vec3 eye = McToUe({mc.eyeX + fwd.x * d, mc.eyeY + fwd.y * d, mc.eyeZ + fwd.z * d});
                FRot rot{McPitchToUe(mc.pitch), McYawToUe(mc.yaw), 0};
                if (mc.cameraMode == 2)
                {
                    rot.Yaw += 180.0;
                    rot.Pitch = -rot.Pitch;
                }
                if (auto call = reflect::Begin(camera, STR("K2_SetActorLocationAndRotation")))
                {
                    reflect::Set(call, STR("NewLocation"), FVec{eye.x, eye.y, eye.z});
                    reflect::Set(call, STR("NewRotation"), rot);
                    reflect::Set(call, STR("bTeleport"), true);
                    reflect::Invoke(call);
                }
            }
            if (on == thirdPerson)
            {
                return;
            }
            thirdPerson = on;
            if (auto call = reflect::Begin(pc, STR("SetViewTargetWithBlend")))
            {
                reflect::Set(call, STR("NewViewTarget"), on && camera ? camera : pawn);
                reflect::Invoke(call);
            }
            if (auto call = reflect::Begin(pawn, STR("SetActorHiddenInGame")))
            {
                reflect::Set(call, STR("bNewHidden"), on);
                reflect::Invoke(call);
            }
            Log(L"third person {}", on ? L"on" : L"off");
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

        // Abiotic Factor's own HUD, hidden while Minecraft's is drawn instead.
        FWeakObjectPtr hudRef;
        bool hudHidden = false;
        std::uint8_t hudVisibility = 0;

        void HideHostHud(bool hide)
        {
            if (hide == hudHidden)
            {
                return;
            }
            UObject* hud = hudRef.Get();
            static std::uint64_t nextLookup = 0;
            if (!hud && ::GetTickCount64() >= nextLookup)
            {
                nextLookup = ::GetTickCount64() + 2000;  // a full object scan: never every frame
                hud = UObjectGlobals::FindFirstOf(STR("W_PlayerHUD_Main_C"));
                hudRef = hud;
            }
            if (!hud)
            {
                return;
            }
            if (hide)
            {
                auto get = reflect::Begin(hud, STR("GetVisibility"));
                if (get)
                {
                    reflect::Invoke(get);
                    hudVisibility = reflect::Get<std::uint8_t>(get, STR("ReturnValue"));
                }
            }
            auto set = reflect::Begin(hud, STR("SetVisibility"));
            if (set)
            {
                reflect::Set<std::uint8_t>(set, STR("InVisibility"), hide ? 2 : hudVisibility);  // 2: ESlateVisibility::Hidden
                reflect::Invoke(set);
                hudHidden = hide;
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


        // Minecraft moves its player to sky.pos (kept current while we aren't puppeting) when
        // sky.teleportSeq bumps. We only bump once our own scan has ground under the player: the
        // level streams in after a load or long move, and Minecraft drops you if it isn't there.
        bool teleportPending = false;
        bool waitLogged = false;
        std::uint64_t teleportAsked = 0;

        void Teleport(const Vec3&)
        {
            teleportPending = true;
            waitLogged = false;
            teleportAsked = ::GetTickCount64();
            puppeting = false;
        }

        void TeleportWhenGrounded(const Vec3& feet)
        {
            if (!teleportPending)
            {
                return;
            }
            if (CollisionGroundBelow(feet, 2.0))
            {
                teleportPending = false;
                ++sky.teleportSeq;
                Log(L"Minecraft goes to ({:.2f}, {:.2f}, {:.2f})", feet.x, feet.y, feet.z);
            }
            else if (::GetTickCount64() - teleportAsked > 5000 && !waitLogged)
            {
                waitLogged = true;
                Log(L"still waiting for ground under the player at ({:.2f}, {:.2f}, {:.2f}) (level streaming in?)", feet.x, feet.y, feet.z);
            }
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
        perf::Scope scope(perf::Tick);
        static std::uint64_t nextReport = 0;
        if (const auto t = ::GetTickCount64(); t >= nextReport)
        {
            nextReport = t + 10000;
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            std::wstring line;
            for (int i = 0; i < perf::Count; ++i)
            {
                const auto n = perf::calls[i].exchange(0), ticks = perf::total[i].exchange(0);
                line += std::format(L" {} {:.2f} ms avg ({} calls, {:.0f} ms total);", perf::kNames[i], n ? ticks * 1000.0 / f.QuadPart / n : 0.0, n, ticks * 1000.0 / f.QuadPart);
            }
            Log(L"perf (10 s):{}", line);
        }
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
            RenderTick(nullptr, sky.collisionEpoch);  // drains the render ring (Minecraft stalls on a full one)
            if (lastPawn)
            {
                Log(L"player character gone (menu or loading)");
            }
            lastPawn = nullptr;
            puppeting = movementOff = fallDamageOff = false;
            thirdPerson = false;
            overlay::show = false;
            hudHidden = false;
            hudRef = FWeakObjectPtr{};
            sky.flags = proto::kSkyLoading;
            link.WriteSkyState(sky);
            return;
        }

        const FVec loc = Location(pawn);
        if (pawn != lastPawn)
        {
            lastPawn = pawn;
            fallDamageOff = false;
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
        {
            perf::Scope r(perf::Render);
            const Vec3 feet = puppeting ? InterpolatedFeet(mc) : Vec3{};
            RenderTick(pawn, sky.collisionEpoch, puppeting ? &feet : nullptr);  // after the map/epoch bookkeeping above
        }
        if (haveMc && link.McPid() != lastMcPid)
        {
            lastMcPid = link.McPid();
            Teleport(UeToMc({loc.X, loc.Y, loc.Z - halfHeight}));
            overlay::reset = true;
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
                TeleportWhenGrounded(feet);
                if (!teleportPending && mc.teleportAck == sky.teleportSeq)
                {
                    puppeting = true;
                    Log(L"Minecraft now drives the player");
                }
            }
        }

        if (!puppeting)
        {
            haveShown = false;
        }
        if (pawn)
        {
            GameFallDamage(pawn, !enabled);
        }
        if (enabled && !movementOff)  // e.g. a new character after respawning
        {
            SetMovementMode(pawn, kMoveNone);
            movementOff = true;
        }
        if (puppeting)
        {
            const Vec3 feet = McToUe(InterpolatedFeet(mc));
            lastSet = {feet.x, feet.y, SmoothZ(feet.z + halfHeight, mc.flags & proto::kMcOnGround)};
            perf::Scope pos(perf::Position);
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
            perf::Scope col(perf::Collision);
            CollisionTick(pawn->GetWorld(), here, sky.collisionEpoch);
        }

        {
            perf::Scope act(perf::Actors);
            ActorsTick(pc, pawn, puppeting, mc);
        }

        ThirdPerson(pc, pawn, mc, puppeting && mc.cameraMode != 0);

        // Look stays the game's own (its mouse handling, zero latency); Minecraft follows it.
        const FRot rot = ControlRotation(pc);
        sky.yaw = static_cast<float>(Normalize(UeYawToMc(rot.Yaw)));
        sky.pitch = static_cast<float>(UePitchToMc(Normalize(rot.Pitch)));
        const bool menu = BoolProperty(pc, STR("bShowMouseCursor"));
        if (menu != hostMenu && puppeting)
        {
            Log(L"game menu {}", menu ? L"open (input to the game)" : L"closed (input to Minecraft)");
        }
        hostMenu = menu;
        // The game loads zones when you walk into their level volumes; until they're in, the floor
        // ahead may not exist. Minecraft parks its player while we say "loading" (and only lets go
        // once our scan has ground under it), like the game holds its own player.
        static std::uint64_t nextLevelCheck = 0;
        static bool levelLoaded = true;
        if (const auto t = ::GetTickCount64(); t >= nextLevelCheck)
        {
            nextLevelCheck = t + 100;
            if (auto call = reflect::Begin(pawn, STR("IsLevelLoaded")))
            {
                reflect::Set(call, STR("RequireAllOverlapped"), true);
                reflect::Invoke(call);
                const bool loaded = reflect::Get<bool>(call, STR("ReturnValue"));
                if (loaded != levelLoaded)
                {
                    levelLoaded = loaded;
                    Log(L"zone {}", loaded ? L"loaded: Minecraft may move again" : L"loading: holding Minecraft's player");
                }
            }
        }
        if (puppeting)
        {
            // Minecraft re-teleports to sky.pos after a hold: make that a no-op.
            sky.posX = mc.x;
            sky.posY = mc.y;
            sky.posZ = mc.z;
        }
        sky.flags = proto::kSkyInGame | (hostMenu ? proto::kSkyMenuOpen : 0u) | (levelLoaded ? 0u : proto::kSkyLoading);
        const bool mcScreen = puppeting && (mc.flags & proto::kMcScreenOpen);
        if (mcScreen && !overlay::screenOpen)
        {
            overlay::cursorX = static_cast<int>(sky.viewportW / 2);  // Minecraft screens open with the cursor centred
            overlay::cursorY = static_cast<int>(sky.viewportH / 2);
        }
        overlay::screenOpen = mcScreen;
        overlay::show = puppeting;  // Minecraft's HUD stays up behind the game's menus
        overlay::crosshair = puppeting && mc.cameraMode == 0 && !mcScreen && !hostMenu;
        overlay::guiScale = static_cast<int>(mc.guiScale);
        HideHostHud(puppeting);
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
        // Minecraft renders one overlay frame per SkyState update (SkyClient paces on its seq): 60 Hz
        // is plenty for HUD and hand, and keeps it from rendering + reading back at the game's 160 fps.
        static std::uint64_t lastSkyWrite = 0;
        if (now - lastSkyWrite >= 16)
        {
            lastSkyWrite = now;
            link.WriteSkyState(sky);
        }
    }
}
