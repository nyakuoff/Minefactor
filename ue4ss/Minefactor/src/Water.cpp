#include "Water.hpp"

#include <cmath>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Link.hpp"
#include "Log.hpp"
#include "Reflect.hpp"

using namespace RC::Unreal;

namespace mf
{
    namespace
    {
        struct FVec
        {
            double X, Y, Z;
        };

        struct Volume
        {
            FWeakObjectPtr actor, brush;
            FVec min, max;  // game-space bounds
        };
        std::vector<Volume> volumes;
        constexpr double kVerticalMargin = 250.0;  // cm
        std::uint64_t nextList = 0, nextGrid = 0;
        std::size_t lastCount = ~std::size_t(0);

        bool IsWater(UObject* volume)
        {
            auto* prop = CastField<FBoolProperty>(volume->GetPropertyByNameInChain(STR("bWaterVolume")));
            return prop && prop->GetPropertyValueInContainer(volume);
        }

        // Every couple of seconds (sublevels stream in and out): the water volumes and their bounds.
        void List(UObject* pawn)
        {
            static UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.PhysicsVolume"));
            static UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            auto call = reflect::Begin(statics, STR("GetAllActorsOfClass"));
            if (!cls || !call)
            {
                return;
            }
            reflect::Set(call, STR("WorldContextObject"), pawn);
            reflect::Set(call, STR("ActorClass"), static_cast<UObject*>(cls));
            auto* out = reflect::SetArrayParam<UObject*>(call, STR("OutActors"));
            reflect::Invoke(call);
            volumes.clear();
            for (UObject* actor : *out)
            {
                if (!actor || !IsWater(actor))
                {
                    continue;
                }
                auto bounds = reflect::Begin(actor, STR("GetActorBounds"));
                if (!bounds)
                {
                    continue;
                }
                reflect::Set(bounds, STR("bOnlyCollidingComponents"), false);
                reflect::Invoke(bounds);
                const FVec o = reflect::Get<FVec>(bounds, STR("Origin")), e = reflect::Get<FVec>(bounds, STR("BoxExtent"));
                UObject* brush = nullptr;
                if (auto** b = actor->GetValuePtrByPropertyNameInChain<UObject*>(STR("BrushComponent")))
                {
                    brush = *b;
                }
                if (brush)
                {
                    volumes.push_back({FWeakObjectPtr(actor), FWeakObjectPtr(brush), {o.X - e.X, o.Y - e.Y, o.Z - e.Z}, {o.X + e.X, o.Y + e.Y, o.Z + e.Z}});
                }
            }
            out->~TArray();
            if (volumes.size() != lastCount)
            {
                lastCount = volumes.size();
                Log(L"water: {} water volumes loaded", volumes.size());
            }
        }

        // Surface height (game Z) where a vertical line at (x, y) meets this volume's brush, from above.
        bool Surface(UObject* brush, const Volume& v, double x, double y, double& z)
        {
            auto call = reflect::Begin(brush, STR("K2_LineTraceComponent"));
            if (!call)
            {
                return false;
            }
            reflect::Set(call, STR("TraceStart"), FVec{x, y, v.max.Z + 50.0});
            reflect::Set(call, STR("TraceEnd"), FVec{x, y, v.min.Z - 50.0});
            reflect::Set(call, STR("bTraceComplex"), false);
            reflect::Invoke(call);
            if (!reflect::Get<bool>(call, STR("ReturnValue")))
            {
                return false;
            }
            z = reflect::Get<FVec>(call, STR("HitLocation")).Z;
            return true;
        }
    }

    void WaterTick(UObject* pawn, const Vec3& playerMc, std::uint32_t worldId)
    {
        const auto now = ::GetTickCount64();
        if (now >= nextList)
        {
            nextList = now + 2000;
            List(pawn);
        }
        if (now < nextGrid)
        {
            return;
        }
        nextGrid = now + 100;

        constexpr int N = proto::kWaterGridSize;
        proto::WaterGrid grid{};
        grid.originX = static_cast<std::int32_t>(std::floor(playerMc.x)) - N / 2;
        grid.originZ = static_cast<std::int32_t>(std::floor(playerMc.z)) - N / 2;
        grid.worldId = worldId;
        const double playerZ = McToUe(playerMc).z;
        for (int i = 0; i < N * N; ++i)
        {
            grid.surface[i] = proto::kNoWater;
        }
        for (const auto& v : volumes)
        {
            UObject* brush = v.brush.Get();
            // Minecraft fills water down through any air below the surface (the game's floors are only
            // collision to it), so only volumes at the player's own height count: a pool upstairs
            // mustn't flood the room below.
            if (!brush || playerZ < v.min.Z - kVerticalMargin || playerZ > v.max.Z + kVerticalMargin)
            {
                continue;
            }
            for (int gz = 0; gz < N; ++gz)
            {
                for (int gx = 0; gx < N; ++gx)
                {
                    const Vec3 c = McToUe({grid.originX + gx + 0.5, 0, grid.originZ + gz + 0.5});
                    if (c.x < v.min.X || c.x > v.max.X || c.y < v.min.Y || c.y > v.max.Y)
                    {
                        continue;
                    }
                    double z;
                    if (!Surface(brush, v, c.x, c.y, z))
                    {
                        continue;
                    }
                    // Stacked bodies on different floors: the surface nearest the player wins.
                    float& cell = grid.surface[gz * N + gx];
                    const float mcY = static_cast<float>(z / kCm);
                    if (cell == proto::kNoWater || std::abs(z - playerZ) < std::abs(cell * kCm - playerZ))
                    {
                        cell = mcY;
                    }
                }
            }
        }
        Link::Get().WriteWaterGrid(grid);
    }
}
