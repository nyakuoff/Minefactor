#pragma once

namespace RC::Unreal
{
    class UObject;
}

namespace mf
{
    // Phase 3 groundwork: logs, once per class, the health/damage-looking members of the game's NPCs
    // and of the player, so combat can target the right ones. Call every tick (game thread).
    void ActorsTick(RC::Unreal::UObject* pawn);
}
