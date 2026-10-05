#pragma once
#include <cstdint>

#include "Coords.hpp"

namespace mf
{
    // Streams the level's collision around the player to Minecraft as 1/8-block voxels, asking the
    // engine's own physics what blocks a player. Call once per tick (game thread) while Minecraft
    // is in its world; epoch changes (new map) start over.
    void CollisionTick(void* world, const Vec3& playerMc, std::uint32_t epoch);
    // True if a scanned solid voxel lies under these feet (Minecraft coords), within depth blocks.
    bool CollisionGroundBelow(const Vec3& feet, double depth);
}
