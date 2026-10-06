#pragma once
#include <cstdint>

#include "Coords.hpp"

namespace RC::Unreal
{
    class UObject;
}

namespace mf
{
    // The game's water (PhysicsVolume with bWaterVolume: pools, flooded rooms, toxic waste, lava...)
    // around the player, as Minecraft's 16x16 water-surface grid: Minecraft swims in it. Game thread.
    void WaterTick(RC::Unreal::UObject* pawn, const Vec3& playerMc, std::uint32_t worldId);
}
