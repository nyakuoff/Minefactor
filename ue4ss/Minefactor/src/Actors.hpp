#pragma once
#include "minefactor_protocol.h"

namespace RC::Unreal
{
    class UObject;
}

namespace mf
{
    // Combat bridge (game thread, every tick): mirrors nearby NPCs into Minecraft, applies Minecraft's
    // hits to them through the game's damage path, and turns damage the player takes into Minecraft
    // hurts (the game's limbs are put back: Minecraft's health is the one that counts).
    void ActorsTick(RC::Unreal::UObject* pc, RC::Unreal::UObject* pawn, bool puppeting, const minefactor::proto::McState& mc);
    // Catches damage events on the player while Minecraft drives it (call once at startup).
    void InstallDamageHook();
    // Once, ~10 s after startup (main menu is fine): load a few NPC classes and log their members.
    void ActorsDiscoverOnce();
}
