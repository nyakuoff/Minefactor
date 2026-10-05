#include "Actors.hpp"

#include <set>
#include <string>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Log.hpp"

using namespace RC::Unreal;

namespace mf
{
    namespace
    {
        std::uint64_t nextScan = 0;
        std::set<std::wstring> logged;
        std::size_t lastCount = ~std::size_t(0);

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

    void ActorsTick(UObject* pawn)
    {
        const auto now = ::GetTickCount64();
        if (now < nextScan)
        {
            return;
        }
        nextScan = now + 3000;
        if (pawn)
        {
            Describe(pawn, L"player");
        }
        std::vector<UObject*> npcs;
        UObjectGlobals::FindAllOf(STR("NPC_Base_ParentBP_C"), npcs);
        if (npcs.size() != lastCount)
        {
            lastCount = npcs.size();
            Log(L"discover: {} NPCs loaded", npcs.size());
        }
        for (UObject* npc : npcs)
        {
            Describe(npc, L"NPC");
        }
    }
}
