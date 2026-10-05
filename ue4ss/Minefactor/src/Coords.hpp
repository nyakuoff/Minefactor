#pragma once
// Unreal (cm, Z up, left-handed: X forward, Y right) <-> Minecraft (blocks, Y up, right-handed:
// X east, Z south). 1 block = 100 cm. Swapping Unreal's Y and Z flips handedness, so no negation.
#include "minefactor_protocol.h"

namespace mf
{
    struct Vec3
    {
        double x, y, z;
        constexpr bool operator==(const Vec3&) const = default;
    };

    constexpr double kCm = minefactor::proto::kUnitsPerBlock;

    constexpr Vec3 UeToMc(const Vec3& ue) { return {ue.x / kCm, ue.z / kCm, ue.y / kCm}; }
    constexpr Vec3 McToUe(const Vec3& mc) { return {mc.x * kCm, mc.z * kCm, mc.y * kCm}; }

    // Yaw: Unreal 0 faces +X and turns towards +Y; Minecraft 0 faces +Z (south) and 90 faces -X.
    constexpr double UeYawToMc(double yaw) { return yaw - 90.0; }
    constexpr double McYawToUe(double yaw) { return yaw + 90.0; }
    // Pitch: Unreal positive looks up, Minecraft positive looks down.
    constexpr double UePitchToMc(double pitch) { return -pitch; }
    constexpr double McPitchToUe(double pitch) { return -pitch; }

    // Unreal forward (+X) is Minecraft east (+x), which MC calls yaw -90 (= 270).
    static_assert(UeToMc({100, 0, 0}) == Vec3{1, 0, 0} && UeYawToMc(0) == -90.0);
    // Unreal right (+Y) is Minecraft south (+z), MC yaw 0.
    static_assert(UeToMc({0, 100, 0}) == Vec3{0, 0, 1} && UeYawToMc(90) == 0.0);
    // Unreal up (+Z) is Minecraft up (+y).
    static_assert(UeToMc({0, 0, 100}) == Vec3{0, 1, 0});
    static_assert(McToUe(UeToMc({125, -450, 775})) == Vec3{125, -450, 775});  // binary-exact values
    static_assert(McYawToUe(UeYawToMc(37)) == 37 && McPitchToUe(UePitchToMc(-12)) == -12);
}
