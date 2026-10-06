#pragma once
#include <cstdint>

namespace RC::Unreal
{
    class UObject;
}

namespace mf
{
    // Minecraft's world content drawn inside the game's world: placed blocks (with collision the
    // game's NPCs bump into), block entities, mobs, particles (render ring) and arrows / dropped
    // items (world entities). Game thread, every tick. epoch changes (new map) start over.
    void RenderTick(RC::Unreal::UObject* pawn, std::uint32_t epoch);
}
