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
    // Co-op, on the host, once a second for each other player driven by their own Minefactor: their
    // needs held full and their limbs healed back (Minecraft's health counts; damage still replicates
    // to them first, which is how their Minecraft feels it).
    void RemoteMinefactorPlayer(RC::Unreal::UObject* pawn);
    // Catches damage events on the player while Minecraft drives it (call once at startup).
    void InstallDamageHook();
}
