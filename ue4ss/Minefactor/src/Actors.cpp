#include "Actors.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Reflect.hpp"
#include "Symbols.hpp"

using namespace RC::Unreal;

namespace mf
{
    namespace
    {
        std::uint64_t nextScan = 0;
        std::set<std::wstring> logged;

        bool Interesting(const std::wstring& name)
        {
            static const wchar_t* kWords[] = {L"Health", L"HP", L"Damage", L"Hurt", L"Hit", L"Kill", L"Die", L"Dead", L"Death",
                                              L"Hostil", L"Faction", L"Team", L"Aggro", L"Limb", L"Stun", L"Knock"};
            for (const auto* w : kWords)
            {
                if (name.find(w) != std::wstring::npos)
                {
                    return true;
                }
            }
            return false;
        }

        void Describe(UObject* object, const wchar_t* what)
        {
            UClass* cls = object->GetClassPrivate();
            const std::wstring className = cls->GetName();
            if (!logged.insert(className).second)
            {
                return;
            }
            std::wstring props, funcs;
            for (FProperty* p : cls->ForEachPropertyInChain())
            {
                const auto name = p->GetName();
                if (Interesting(name))
                {
                    props += std::format(L" {}:{}", name, p->GetClass().GetName());
                }
            }
            for (UFunction* f : cls->ForEachFunctionInChain())
            {
                const auto name = f->GetName();
                if (!Interesting(name))
                {
                    continue;
                }
                std::wstring params;
                for (FProperty* p : f->ForEachProperty())
                {
                    params += std::format(L"{}{}:{}", params.empty() ? L"" : L", ", p->GetName(), p->GetClass().GetName());
                }
                funcs += std::format(L" {}({})", name, params);
            }
            Log(L"discover {} {} ({}):\n    properties:{}\n    functions:{}", what, className, cls->GetPathName(), props, funcs);
        }
    }

    void ActorsDiscoverOnce()
    {
        static std::uint64_t at = ::GetTickCount64() + 10000;  // let the engine finish starting up
        if (!at || ::GetTickCount64() < at)
        {
            return;
        }
        at = 0;
        auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != sym::kExeTimestamp)
        {
            return;
        }
        using LoadClassFn = UClass* (*)(UClass*, UObject*, const wchar_t*, const wchar_t*, std::uint32_t, void*);
        auto load = reinterpret_cast<LoadClassFn>(base + sym::StaticLoadClass);
        static const wchar_t* kClasses[] = {
            L"/Game/Blueprints/Characters/Abiotic_PlayerCharacter.Abiotic_PlayerCharacter_C",
            L"/Game/Blueprints/Characters/NPCs/NPC_Base_ParentBP.NPC_Base_ParentBP_C",
            L"/Game/Blueprints/Characters/NPCs/NPC_Monster_Peccary.NPC_Monster_Peccary_C",
            L"/Game/Blueprints/Characters/NPCs/NPC_Monster_Exor.NPC_Monster_Exor_C",
            L"/Game/Blueprints/Characters/NPCs/NPC_Grunt_Guard.NPC_Grunt_Guard_C",
        };
        for (const auto* path : kClasses)
        {
            UClass* cls = load(UObject::StaticClass(), nullptr, path, nullptr, 0, nullptr);
            if (!cls)
            {
                Log(L"discover: couldn't load {}", path);
                continue;
            }
            Describe(cls->GetClassDefaultObject(), L"class");
        }
    }

    namespace
    {
        constexpr double kMcToGameDamage = 6.0;   // Minecraft hit -> game damage (diamond sword 7 -> 42)
        constexpr double kGameToMcDamage = 5.0;   // must match SkyCombat.SKYRIM_TO_MC_DAMAGE
        constexpr double kMirrorRadius = 64.0;    // blocks

        struct FVec
        {
            double X, Y, Z;
        };
        struct FRot
        {
            double Pitch, Yaw, Roll;
        };

        UClass* LoadClass(const wchar_t* path)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != sym::kExeTimestamp)
            {
                return nullptr;
            }
            using LoadClassFn = UClass* (*)(UClass*, UObject*, const wchar_t*, const wchar_t*, std::uint32_t, void*);
            return reinterpret_cast<LoadClassFn>(base + sym::StaticLoadClass)(UObject::StaticClass(), nullptr, path, nullptr, 0, nullptr);
        }

        template <class T>
        T CallGet(UObject* obj, const wchar_t* fn)
        {
            auto call = reflect::Begin(obj, fn);
            if (!call)
            {
                return T{};
            }
            reflect::Invoke(call);
            return reflect::Get<T>(call, STR("ReturnValue"));
        }

        UObject* Component(UObject* actor, const wchar_t* property)
        {
            auto** ptr = actor->GetValuePtrByPropertyName<UObject*>(property);
            return ptr ? *ptr : nullptr;
        }

        UObject* Statics()
        {
            static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            return cdo;
        }

        struct Npc
        {
            FWeakObjectPtr ref;
            float radius = 0.4f, halfHeight = 0.9f;  // blocks
            char name[24]{};
        };
        std::unordered_map<std::uint32_t, Npc> npcs;  // by object index (the protocol's formId)
        UClass* npcClass = nullptr;
        std::uint64_t nextList = 0, nextMirror = 0;

        void ShortName(const std::wstring& cls, char (&out)[24])
        {
            std::wstring n = cls;
            for (const wchar_t* cut : {L"NPC_", L"Monster_"})
            {
                if (n.starts_with(cut))
                {
                    n = n.substr(std::wcslen(cut));
                }
            }
            if (n.ends_with(L"_C"))
            {
                n.resize(n.size() - 2);
            }
            std::size_t i = 0;
            for (; i < n.size() && i < sizeof(out) - 1; ++i)
            {
                out[i] = n[i] < 128 ? static_cast<char>(n[i]) : '?';
            }
            out[i] = 0;
        }

        // Every second: which NPC actors exist (cheap actor iteration, not an object-array scan).
        void ListNpcs(UObject* pawn)
        {
            if (!npcClass)
            {
                npcClass = LoadClass(L"/Game/Blueprints/Characters/NPCs/NPC_Base_ParentBP.NPC_Base_ParentBP_C");
                if (!npcClass)
                {
                    return;
                }
            }
            auto call = reflect::Begin(Statics(), STR("GetAllActorsOfClass"));
            if (!call)
            {
                return;
            }
            reflect::Set(call, STR("WorldContextObject"), pawn);
            reflect::Set(call, STR("ActorClass"), static_cast<UObject*>(npcClass));
            auto* out = reflect::SetArrayParam<UObject*>(call, STR("OutActors"));
            reflect::Invoke(call);
            std::unordered_map<std::uint32_t, Npc> next;
            for (UObject* actor : *out)
            {
                if (!actor)
                {
                    continue;
                }
                const auto id = static_cast<std::uint32_t>(actor->GetInternalIndex());
                if (auto it = npcs.find(id); it != npcs.end() && it->second.ref.Get() == actor)
                {
                    next.emplace(id, std::move(it->second));
                    continue;
                }
                Npc n;
                n.ref = actor;
                if (UObject* capsule = Component(actor, STR("CapsuleComponent")))
                {
                    n.radius = CallGet<float>(capsule, STR("GetScaledCapsuleRadius")) / 100.0f;
                    n.halfHeight = CallGet<float>(capsule, STR("GetScaledCapsuleHalfHeight")) / 100.0f;
                }
                ShortName(actor->GetClassPrivate()->GetName(), n.name);
                Describe(actor, L"NPC");
                next.emplace(id, std::move(n));
            }
            npcs = std::move(next);
            out->~TArray();  // engine-allocated, constructed in our param buffer
        }

        // 20 Hz: the NPCs near the player into Minecraft's actor table.
        void Mirror(const Vec3& playerMc)
        {
            std::vector<proto::ActorRecord> records;
            for (auto& [id, n] : npcs)
            {
                UObject* actor = n.ref.Get();
                if (!actor)
                {
                    continue;
                }
                const FVec loc = CallGet<FVec>(actor, STR("K2_GetActorLocation"));
                const Vec3 feet = UeToMc({loc.X, loc.Y, loc.Z - n.halfHeight * 100.0});
                if (std::abs(feet.x - playerMc.x) > kMirrorRadius || std::abs(feet.z - playerMc.z) > kMirrorRadius ||
                    std::abs(feet.y - playerMc.y) > kMirrorRadius)
                {
                    continue;
                }
                const FRot rot = CallGet<FRot>(actor, STR("K2_GetActorRotation"));
                const auto* dead = actor->GetValuePtrByPropertyName<bool>(STR("IsDead"));
                const float health = CallGet<float>(actor, STR("GetTotalHealth"));
                const float maxHealth = CallGet<float>(actor, STR("GetMaxTotalHealth"));
                proto::ActorRecord r{};
                r.formId = id;
                r.flags = (dead && *dead) ? proto::kActorDead : std::strstr(n.name, "Ally") ? 0u : proto::kActorHostile;
                r.x = static_cast<float>(feet.x);
                r.y = static_cast<float>(feet.y);
                r.z = static_cast<float>(feet.z);
                r.yaw = static_cast<float>(UeYawToMc(rot.Yaw));
                r.width = n.radius * 2.0f;
                r.height = n.halfHeight * 2.0f;
                r.healthFrac = maxHealth > 0 ? std::clamp(health / maxHealth, 0.0f, 1.0f) : 1.0f;
                std::memcpy(r.name, n.name, sizeof(r.name));
                records.push_back(r);
                if (records.size() == proto::kMaxActors)
                {
                    break;
                }
            }
            Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
        }

        UClass* DamageType(std::uint32_t weapon, std::uint32_t flags)
        {
            static std::unordered_map<std::wstring, UClass*> cache;
            const wchar_t* name = L"DamageType_Blunt";
            switch (weapon)
            {
            case proto::kWeaponUnarmed: name = L"DamageType_Blunt_Punching"; break;
            case proto::kWeaponBlade:
            case proto::kWeaponAxe:
            case proto::kWeaponPierce: name = L"DamageType_Sharp"; break;
            case proto::kWeaponArrow: name = L"DamageType_Sharp_Crossbow"; break;
            default: break;
            }
            if (flags & proto::kHitFire)
            {
                name = L"DamageType_Fire";
            }
            auto [it, fresh] = cache.try_emplace(name, nullptr);
            if (fresh)
            {
                it->second = LoadClass(std::format(L"/Game/Blueprints/DamageTypes/{0}.{0}_C", name).c_str());
            }
            return it->second;
        }

        int hitsLogged = 0;

        // Minecraft hit an NPC's stand-in: the same hit, through the game's own damage path.
        void HitNpc(UObject* pc, UObject* pawn, const proto::McEvent& e, const Vec3& eyeMc)
        {
            auto it = npcs.find(e.formId);
            UObject* npc = it != npcs.end() ? it->second.ref.Get() : nullptr;
            if (!npc)
            {
                return;
            }
            const FVec loc = CallGet<FVec>(npc, STR("K2_GetActorLocation"));
            const Vec3 eye = McToUe(eyeMc);
            const FVec dir = [&] {
                const double dx = loc.X - eye.x, dy = loc.Y - eye.y, dz = loc.Z - eye.z, len = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0);
                return FVec{dx / len, dy / len, dz / len};
            }();
            const float before = CallGet<float>(npc, STR("GetTotalHealth"));

            // A trace against just this NPC's mesh gives a real bone (the game's damage is per limb).
            auto damage = reflect::Begin(Statics(), STR("ApplyPointDamage"));
            if (!damage)
            {
                return;
            }
            if (auto trace = reflect::Begin(Component(npc, STR("Mesh")), STR("K2_LineTraceComponent")))
            {
                reflect::Set(trace, STR("TraceStart"), FVec{eye.x, eye.y, eye.z});
                reflect::Set(trace, STR("TraceEnd"), FVec{loc.X + dir.X * 200, loc.Y + dir.Y * 200, loc.Z + dir.Z * 200});
                reflect::Invoke(trace);
                if (reflect::Get<bool>(trace, STR("ReturnValue")))
                {
                    std::memcpy(reflect::ParamAddr(damage, STR("HitInfo")), reflect::ParamAddr(trace, STR("OutHit")),
                                std::min(reflect::ParamSize(damage, STR("HitInfo")), reflect::ParamSize(trace, STR("OutHit"))));
                }
            }
            reflect::Set(damage, STR("DamagedActor"), npc);
            reflect::Set(damage, STR("BaseDamage"), static_cast<float>(e.a * kMcToGameDamage));
            reflect::Set(damage, STR("HitFromDirection"), dir);
            reflect::Set(damage, STR("EventInstigator"), pc);
            reflect::Set(damage, STR("DamageCauser"), pawn);
            reflect::Set(damage, STR("DamageTypeClass"), static_cast<UObject*>(DamageType(e.weapon, e.flags)));
            reflect::Invoke(damage);

            // Minecraft's knockback, as a shove.
            if (e.d > 0)
            {
                if (auto launch = reflect::Begin(npc, STR("LaunchCharacter")))
                {
                    reflect::Set(launch, STR("LaunchVelocity"), FVec{e.b * e.d * 600.0, e.c * e.d * 600.0, 150.0});
                    reflect::Invoke(launch);
                }
            }
            if (hitsLogged++ < 10)
            {
                const auto* dead = npc->GetValuePtrByPropertyName<bool>(STR("IsDead"));
                Log(L"hit {} (#{}) for {:.1f} ({:.1f} game damage, weapon {}): health {:.1f} -> {:.1f}, IsDead {}",
                    std::wstring(it->second.name, it->second.name + std::strlen(it->second.name)), e.formId, e.a, e.a * kMcToGameDamage, e.weapon,
                    before, CallGet<float>(npc, STR("GetTotalHealth")), dead ? (*dead ? L"yes" : L"no") : L"?");
            }
        }

        // The player's limbs as they were when Minecraft took over: anything taken off them is a hit
        // Minecraft should feel, and they're put back (Minecraft's health is the one that counts).
        constexpr const wchar_t* kLimbs[] = {STR("CurrentHealth_Head"), STR("CurrentHealth_Torso"), STR("CurrentHealth_LeftArm"),
                                             STR("CurrentHealth_RightArm"), STR("CurrentHealth_LeftLeg"), STR("CurrentHealth_RightLeg")};
        float savedLimbs[6]{};
        UObject* savedFor = nullptr;
        bool dying = false;

        void GuardPlayerHealth(UObject* pawn)
        {
            float* limbs[6];
            for (int i = 0; i < 6; ++i)
            {
                limbs[i] = pawn->GetValuePtrByPropertyName<float>(kLimbs[i]);
                if (!limbs[i])
                {
                    return;
                }
            }
            if (savedFor != pawn)
            {
                savedFor = pawn;
                dying = false;
                for (int i = 0; i < 6; ++i)
                {
                    savedLimbs[i] = *limbs[i];
                }
                Log(L"guarding player health: head {:.0f} torso {:.0f} arms {:.0f}/{:.0f} legs {:.0f}/{:.0f}", savedLimbs[0], savedLimbs[1],
                    savedLimbs[2], savedLimbs[3], savedLimbs[4], savedLimbs[5]);
                return;
            }
            if (dying)
            {
                return;
            }
            float lost = 0;
            for (int i = 0; i < 6; ++i)
            {
                if (*limbs[i] < savedLimbs[i])
                {
                    lost += savedLimbs[i] - *limbs[i];
                    *limbs[i] = savedLimbs[i];
                }
            }
            if (lost <= 0)
            {
                return;
            }
            if (auto rep = reflect::Begin(pawn, STR("OnRep_CurrentHealth")))
            {
                reflect::Invoke(rep);
            }
            std::uint32_t attacker = 0;
            if (auto** by = pawn->GetValuePtrByPropertyName<UObject*>(STR("LastHitBy")); by && *by)
            {
                attacker = static_cast<std::uint32_t>((*by)->GetInternalIndex());
            }
            Link::Get().PushInput(proto::kInHurt, proto::kHurtMelee, static_cast<std::int32_t>(lost * 100.0f), static_cast<std::int32_t>(attacker), 0);
            Log(L"player took {:.1f} game damage -> {:.1f} Minecraft", lost, lost / kGameToMcDamage);
        }

        // Damage events on the guarded player, caught before the game's own handlers run: the amount
        // goes to Minecraft, and the game gets 0 (Minecraft's health is the one that counts).
        UObject* guardedPawn = nullptr;

        float* FloatParam(UFunction* fn, void* parms, const wchar_t* name)
        {
            FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
            return p ? reinterpret_cast<float*>(static_cast<std::uint8_t*>(parms) + p->GetOffset_Internal()) : nullptr;
        }

        UObject* ObjectParam(UFunction* fn, void* parms, const wchar_t* name)
        {
            FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
            return p ? *reinterpret_cast<UObject**>(static_cast<std::uint8_t*>(parms) + p->GetOffset_Internal()) : nullptr;
        }

        std::uint16_t HurtKind(UObject* damageType)
        {
            if (!damageType)
            {
                return proto::kHurtMelee;
            }
            const auto name = damageType->GetName();
            for (const wchar_t* w : {L"Bullet", L"Bolt", L"Crossbow", L"Quill", L"Throwing", L"Dart", L"Rocket"})
            {
                if (name.find(w) != std::wstring::npos)
                {
                    return proto::kHurtProjectile;
                }
            }
            for (const wchar_t* w : {L"Fire", L"Acid", L"Plasma", L"Electro", L"Radiation", L"Laser", L"Holy"})
            {
                if (name.find(w) != std::wstring::npos)
                {
                    return proto::kHurtMagic;
                }
            }
            return proto::kHurtMelee;
        }

        void OnProcessEvent(UObject* context, UFunction* fn, void* parms)
        {
            if (!guardedPawn || context != guardedPawn || dying || !parms)
            {
                return;
            }
            static const FName any(STR("ReceiveAnyDamage"), FNAME_Find), point(STR("ReceivePointDamage"), FNAME_Find),
                radial(STR("ReceiveRadialDamage"), FNAME_Find);
            const FName name = fn->GetNamePrivate();
            if (name != any && name != point && name != radial)
            {
                return;
            }
            float* damage = FloatParam(fn, parms, STR("Damage"));
            if (!damage)
            {
                return;
            }
            if (name == any && *damage > 0)
            {
                UObject* causer = ObjectParam(fn, parms, STR("DamageCauser"));
                UObject* type = ObjectParam(fn, parms, STR("DamageType"));
                const auto attacker = causer ? static_cast<std::uint32_t>(causer->GetInternalIndex()) : 0u;
                Link::Get().PushInput(proto::kInHurt, HurtKind(type), static_cast<std::int32_t>(*damage * 100.0f), static_cast<std::int32_t>(attacker), 0);
                Log(L"player hit for {:.1f} game damage ({}) -> {:.1f} Minecraft", *damage, type ? type->GetName() : std::wstring(L"?"), *damage / kGameToMcDamage);
            }
            *damage = 0.0f;
        }

        // Minecraft's player died: so does the game's (its own death and respawn run).
        void KillPlayer(UObject* pawn)
        {
            dying = true;
            if (auto call = reflect::Begin(Statics(), STR("ApplyDamage")))
            {
                reflect::Set(call, STR("DamagedActor"), pawn);
                reflect::Set(call, STR("BaseDamage"), 100000.0f);
                reflect::Set(call, STR("DamageTypeClass"), static_cast<UObject*>(DamageType(proto::kWeaponBlunt, 0)));
                reflect::Invoke(call);
            }
            Log(L"Minecraft player died; killing the game's player");
        }
    }

    void ActorsTick(UObject* pc, UObject* pawn, bool puppeting, const proto::McState& mc)
    {
        const auto now = ::GetTickCount64();
        if (now >= nextScan)
        {
            nextScan = now + 30000;
            Describe(pawn, L"player");
        }
        if (now >= nextList)
        {
            nextList = now + 1000;
            ListNpcs(pawn);
        }
        guardedPawn = puppeting ? pawn : nullptr;
        if (!puppeting)
        {
            savedFor = nullptr;
            Link::Get().WriteActors(nullptr, 0);
            proto::McEvent drop;
            while (Link::Get().PopEvent(drop))
            {
            }
            return;
        }
        if (now >= nextMirror)
        {
            nextMirror = now + 50;
            Mirror({mc.x, mc.y, mc.z});
        }
        proto::McEvent e;
        while (Link::Get().PopEvent(e))
        {
            if (e.type == proto::kEvHitActor)
            {
                HitNpc(pc, pawn, e, {mc.eyeX, mc.eyeY, mc.eyeZ});
            }
            else if (e.type == proto::kEvPlayerDied && !dying)
            {
                KillPlayer(pawn);
            }
        }
        GuardPlayerHealth(pawn);
    }

    void InstallDamageHook()
    {
        Hook::RegisterProcessEventPreCallback([](auto&, UObject* context, UFunction* fn, void* parms) { OnProcessEvent(context, fn, parms); },
                                              {false, false, STR("Minefactor"), STR("PlayerDamage")});
    }
}
