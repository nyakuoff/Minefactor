#pragma once
#include <cstdint>

#include "Coords.hpp"

namespace RC::Unreal
{
    class UObject;
}

namespace mf
{
    // Minecraft's world content drawn inside the game's world: placed blocks (with collision the
    // game's NPCs bump into), block entities, mobs, particles (render ring) and arrows / dropped
    // items (world entities). Game thread, every tick. epoch changes (new map) start over.
    // feet: the player's (interpolated) feet in Minecraft coords while Minecraft drives it, for the
    // third-person body; null otherwise.
    void RenderTick(RC::Unreal::UObject* pawn, std::uint32_t epoch, const Vec3* feet = nullptr);
}
