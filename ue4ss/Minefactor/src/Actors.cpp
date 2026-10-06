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

    namespace
    {
        constexpr double kMcToGameDamage = 6.0;   // Minecraft hit -> game damage (diamond sword 7 -> 42)
        constexpr double kGameToMcDamage = 15.0;  // must match SkyCombat.SKYRIM_TO_MC_DAMAGE (pest bite 35 -> 2.3)
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
            auto** ptr = actor->GetValuePtrByPropertyNameInChain<UObject*>(property);
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
                const auto* dead = actor->GetValuePtrByPropertyNameInChain<bool>(STR("IsDead"));
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
                const auto* dead = npc->GetValuePtrByPropertyNameInChain<bool>(STR("IsDead"));
                Log(L"hit {} (#{}) for {:.1f} ({:.1f} game damage, weapon {}): health {:.1f} -> {:.1f}, IsDead {}",
                    std::wstring(it->second.name, it->second.name + std::strlen(it->second.name)), e.formId, e.a, e.a * kMcToGameDamage, e.weapon,
                    before, CallGet<float>(npc, STR("GetTotalHealth")), dead ? (*dead ? L"yes" : L"no") : L"?");
            }
        }

        bool dying = false;  // Minecraft's player died; the game's is being killed (cleared per character)
        UObject* dyingFor = nullptr;

        // While Minecraft drives the player the game's own "Invincible" flag is on (as the cheat mods
        // do it): the game's health can't drop, but damage events still fire and become Minecraft hurts.
        UObject* invincibleFor = nullptr;
        bool invincibleWas = false;

        void SetInvincible(UObject* pawn, bool on)
        {
            if (on == (invincibleFor == pawn && pawn))
            {
                return;
            }
            if (invincibleFor && !on)
            {
                if (auto* flag = invincibleFor->GetValuePtrByPropertyNameInChain<bool>(STR("Invincible")))
                {
                    *flag = invincibleWas;
                }
                invincibleFor = nullptr;
                return;
            }
            if (on && pawn)
            {
                if (auto* flag = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Invincible")))
                {
                    invincibleWas = *flag;
                    *flag = true;
                    invincibleFor = pawn;
                }
            }
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
            // Discovery: every distinct event the game sends the player while Minecraft drives it.
            static std::set<std::uint32_t> seen;
            if (seen.size() < 400 && seen.insert(fn->GetNamePrivate().GetComparisonIndex()).second)
            {
                Log(L"player event: {}", fn->GetName());
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
            // The engine sends ReceivePointDamage/RadialDamage first, then (sometimes) ReceiveAnyDamage
            // for the same hit: count the first one we see per hit.
            static UObject* lastCauser = nullptr;
            static std::uint64_t lastAt = 0;
            UObject* hitCauser = ObjectParam(fn, parms, STR("DamageCauser"));
            const auto nowMs = ::GetTickCount64();
            const bool sameHit = name == any && hitCauser == lastCauser && nowMs - lastAt < 50;
            lastCauser = hitCauser;
            lastAt = nowMs;
            if (!sameHit && *damage > 0)
            {
                UObject* causer = ObjectParam(fn, parms, STR("DamageCauser"));
                UObject* type = ObjectParam(fn, parms, STR("DamageType"));
                const auto attacker = causer ? static_cast<std::uint32_t>(causer->GetInternalIndex()) : 0u;
                Link::Get().PushInput(proto::kInHurt, HurtKind(type), static_cast<std::int32_t>(*damage * 100.0f), static_cast<std::int32_t>(attacker), 0);
                Log(L"player hit for {:.1f} game damage ({}) -> {:.1f} Minecraft", *damage, type ? type->GetName() : std::wstring(L"?"), *damage / kGameToMcDamage);
            }
            *damage = 0.0f;
        }

        // A number property of any float type (Blueprint variables may be float or double).
        FProperty* NumberProp(UObject* o, const wchar_t* name)
        {
            return o ? o->GetPropertyByNameInChain(name) : nullptr;
        }

        double GetNumber(UObject* o, const wchar_t* name)
        {
            FProperty* p = NumberProp(o, name);
            if (!p)
            {
                return 0;
            }
            const auto type = p->GetClass().GetName();
            auto* at = reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal();
            return type == STR("DoubleProperty") ? *reinterpret_cast<double*>(at) : type == STR("FloatProperty") ? *reinterpret_cast<float*>(at) : 0.0;
        }

        void SetNumber(UObject* o, const wchar_t* name, double value)
        {
            FProperty* p = NumberProp(o, name);
            if (!p)
            {
                return;
            }
            const auto type = p->GetClass().GetName();
            auto* at = reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal();
            if (type == STR("DoubleProperty"))
            {
                *reinterpret_cast<double*>(at) = value;
            }
            else if (type == STR("FloatProperty"))
            {
                *reinterpret_cast<float*>(at) = static_cast<float>(value);
            }
        }

        void Call(UObject* o, const wchar_t* fn)
        {
            if (auto call = reflect::Begin(o, fn))
            {
                reflect::Invoke(call);
            }
        }

        // The game's own hunger, thirst, fatigue and bladder are held full while Minecraft drives the
        // player (Minecraft's hunger is the one that counts), the way the cheat mods do it.
        void FreezeNeeds(UObject* pawn)
        {
            static std::uint64_t next = 0;
            const auto now = ::GetTickCount64();
            if (now < next)
            {
                return;
            }
            next = now + 1000;
            SetNumber(pawn, STR("CurrentHunger"), GetNumber(pawn, STR("MaxHunger")));
            SetNumber(pawn, STR("CurrentThirst"), GetNumber(pawn, STR("MaxThirst")));
            SetNumber(pawn, STR("CurrentFatigue"), 0.0);
            SetNumber(pawn, STR("CurrentContinence"), GetNumber(pawn, STR("MaxContinence")));
            Call(pawn, STR("OnRep_CurrentHunger"));
            Call(pawn, STR("OnRep_CurrentThirst"));
            Call(pawn, STR("OnRep_CurrentFatigue"));
            Call(pawn, STR("OnRep_CurrentContinence"));
        }

        // Minecraft actions train the game's skills (CharacterSkills enum values).
        enum Skill : std::uint8_t
        {
            kSprinting = 1,
            kAccuracy = 2,
            kSneaking = 4,
            kSharpMelee = 5,
            kBluntMelee = 6,
            kCrafting = 8,
            kFortitude = 14,
        };

        void AddXp(UObject* pawn, std::uint8_t skill, double amount)
        {
            UObject* progression = nullptr;
            if (auto** p = pawn->GetValuePtrByPropertyNameInChain<UObject*>(STR("CharacterProgressionComponent")))
            {
                progression = *p;
            }
            auto call = reflect::Begin(progression, STR("Server_AddXPToSkill"));
            if (!call)
            {
                return;
            }
            // Params in declaration order: skill (enum byte), amount (number), a bool (true, as the cheat mods pass), out success.
            int i = 0;
            for (FProperty* prop : call.function->ForEachProperty())
            {
                auto* at = &call.params[prop->GetOffset_Internal()];
                const auto type = prop->GetClass().GetName();
                if (i == 0)
                {
                    *at = skill;
                }
                else if (i == 1)
                {
                    if (type == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = amount;
                    else if (type == STR("FloatProperty")) *reinterpret_cast<float*>(at) = static_cast<float>(amount);
                    else if (type == STR("IntProperty")) *reinterpret_cast<std::int32_t*>(at) = static_cast<std::int32_t>(std::lround(amount));
                }
                else if (i == 2 && type == STR("BoolProperty"))
                {
                    *reinterpret_cast<bool*>(at) = true;
                }
                ++i;
            }
            reflect::Invoke(call);
        }

        // Sprinting and sneaking while moving: a little XP per second, like the game's own trickle.
        void MovementXp(UObject* pawn, const proto::McState& mc)
        {
            static std::uint64_t next = 0;
            static double lastX = 0, lastZ = 0;
            const auto now = ::GetTickCount64();
            if (now < next)
            {
                return;
            }
            next = now + 2000;
            const bool moved = std::hypot(mc.x - lastX, mc.z - lastZ) > 1.0;
            lastX = mc.x;
            lastZ = mc.z;
            if (moved && (mc.flags & proto::kMcSprinting))
            {
                AddXp(pawn, kSprinting, 2.0);
            }
            if (moved && (mc.flags & proto::kMcSneaking))
            {
                AddXp(pawn, kSneaking, 2.0);
            }
        }

        // Minecraft's player died: so does the game's (its own death and respawn run).
        void KillPlayer(UObject* pawn)
        {
            dying = true;
            dyingFor = pawn;
            SetInvincible(pawn, false);
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
        if (pawn != dyingFor)
        {
            dying = false;  // a new character (respawned): damage counts again
        }
        guardedPawn = puppeting ? pawn : nullptr;
        SetInvincible(pawn, puppeting && !dying);
        if (!puppeting)
        {
            Link::Get().WriteActors(nullptr, 0);
            proto::McEvent drop;
            while (Link::Get().PopEvent(drop))
            {
            }
            return;
        }
        FreezeNeeds(pawn);
        MovementXp(pawn, mc);
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
                const auto skill = e.weapon == proto::kWeaponArrow ? kAccuracy
                                   : (e.weapon == proto::kWeaponBlade || e.weapon == proto::kWeaponAxe || e.weapon == proto::kWeaponPierce) ? kSharpMelee
                                                                                                                                              : kBluntMelee;
                AddXp(pawn, skill, 3.0);
            }
            else if (e.type == proto::kEvSkillUse)
            {
                // Skyrim skill ids Minecraft reports: 9 Block, 10 Smithing, 11/12 Heavy/Light Armor.
                AddXp(pawn, e.formId == 10 ? kCrafting : kFortitude, std::max(1.0f, e.a) * 2.0);
            }
            else if (e.type == proto::kEvPlayerDied && !dying)
            {
                KillPlayer(pawn);
            }
        }
    }

    void InstallDamageHook()
    {
        Hook::RegisterProcessEventPreCallback([](auto&, UObject* context, UFunction* fn, void* parms) { OnProcessEvent(context, fn, parms); },
                                              {false, false, STR("Minefactor"), STR("PlayerDamage")});
    }
}
