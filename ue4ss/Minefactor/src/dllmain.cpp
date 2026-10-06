// Minefactor: play Abiotic Factor as a Minecraft player. UE4SS C++ mod entry point.
// Loaded from ue4ss/Mods/Minefactor/dlls/main.dll (+ "Minefactor : 1" in mods.txt).
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "Actors.hpp"
#include "Input.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Overlay.hpp"
#include "Player.hpp"

using namespace RC;
using namespace RC::Unreal;

class MinefactorMod : public CppUserModBase
{
public:
    MinefactorMod()
    {
        ModName = STR("Minefactor");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Play Abiotic Factor as a Minecraft player (SkyCraft-style).");
        ModAuthors = STR("Minefactor");
    }

    void on_unreal_init() override
    {
        if (!mf::Link::Get().Create())
        {
            mf::Log(L"no shared memory; Minefactor is off for this session");
            return;
        }
        mf::overlay::Install();
        mf::InstallDamageHook();
        // Engine tick runs on the game thread, where reflected calls are safe.
        Hook::RegisterEngineTickPreCallback(
            [](auto&, ...) {
                mf::InputInstall();
                mf::PlayerTick();
            },
            {false, false, STR("Minefactor"), STR("Tick")});
        mf::Log(L"ready; start Minecraft (./gradlew runClient in fabric/) and load a save");
    }
};

#define MINEFACTOR_API __declspec(dllexport)
extern "C"
{
    MINEFACTOR_API RC::CppUserModBase* start_mod()
    {
        return new MinefactorMod();
    }

    MINEFACTOR_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
