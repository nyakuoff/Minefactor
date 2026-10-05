#include "Collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "Link.hpp"
#include "Log.hpp"
#include "Symbols.hpp"

namespace mf
{
    namespace
    {
        constexpr int kRegion = 8;          // blocks per region edge (SkyCollision.REGION_SIZE)
        constexpr int kUnits = kRegion * 8; // 1/8-block voxels per region edge
        constexpr int kRadius = 4;          // regions around the player horizontally
        constexpr int kBelow = 2, kAbove = 1;
        constexpr double kBudgetMs = 3.0;   // per tick
        constexpr std::uint64_t kRefreshMs = 5000;  // regions next to the player (doors, lifts)

        // Engine types, layouts from the game's PDB.
        struct FVector { double X, Y, Z; };
        struct FQuat { double X, Y, Z, W; };
        struct FCollisionShape
        {
            std::int32_t type;  // ECollisionShape::Box = 1
            float halfX, halfY, halfZ;
        };
        static_assert(sizeof(FCollisionShape) == 16);
        struct FCollisionResponseParams
        {
            std::uint8_t response[32];  // per ECollisionChannel: 0 ignore, 1 overlap, 2 block
        };
        using OverlapFn = bool (*)(const void* world, const FVector& pos, const FQuat& rot, std::int32_t channel,
                                   const FCollisionShape& shape, const void* params, const FCollisionResponseParams& response);

        constexpr std::int32_t kChannelPawn = 2;
        constexpr std::int32_t kChannelPhysicsBody = 5;

        OverlapFn overlap = nullptr;
        const void* defaultParams = nullptr;
        FCollisionResponseParams response{};
        bool resolved = false, usable = false;

        bool Resolve()
        {
            if (resolved)
            {
                return usable;
            }
            resolved = true;
            auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != sym::kExeTimestamp)
            {
                Log(L"collision OFF: the game was updated (exe timestamp {:08X}, symbols for {:08X}); re-run tools/gen_symbols.py",
                    nt->FileHeader.TimeDateStamp, sym::kExeTimestamp);
                return false;
            }
            overlap = reinterpret_cast<OverlapFn>(base + sym::UWorld_OverlapBlockingTestByChannel);
            defaultParams = base + sym::FCollisionQueryParams_DefaultQueryParam;
            // What a walking player bumps into, minus pawns (you, NPCs: Minecraft has those as
            // entities) and loose physics props.
            std::fill(std::begin(response.response), std::end(response.response), std::uint8_t{2});
            response.response[kChannelPawn] = 0;
            response.response[kChannelPhysicsBody] = 0;
            usable = true;
            return true;
        }

        struct Cell
        {
            std::uint8_t x, y, z, size;  // in 1/8 blocks, within the region
        };

        struct Job
        {
            bool active = false;
            int rx = 0, ry = 0, rz = 0;
            std::vector<Cell> stack;
            std::array<std::array<std::uint64_t, 8>, kRegion * kRegion * kRegion> bits{};  // [bx + 8bz + 64by][layer]
            std::uint32_t queries = 0;
            double ms = 0;
        };

        Job job;
        std::unordered_map<std::int64_t, std::uint64_t> done;  // region -> when sent
        std::uint32_t lastEpoch = ~0u;
        std::uint64_t regionsSent = 0;
        std::uint64_t nextForget = 0;

        std::int64_t Key(int rx, int ry, int rz)
        {
            return (std::int64_t(rx & 0x1FFFFF) << 42) | (std::int64_t(ry & 0x1FFFFF) << 21) | std::int64_t(rz & 0x1FFFFF);
        }

        int RegionOf(double v)
        {
            return static_cast<int>(std::floor(v / kRegion));
        }

        double NowMs()
        {
            LARGE_INTEGER t{}, f{};
            ::QueryPerformanceCounter(&t);
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        }

        bool Solid(void* world, const Job& j, const Cell& c)
        {
            const double half = c.size / 16.0;  // blocks
            const Vec3 centerMc{j.rx * kRegion + c.x / 8.0 + half, j.ry * kRegion + c.y / 8.0 + half, j.rz * kRegion + c.z / 8.0 + half};
            const Vec3 ue = McToUe(centerMc);
            // A hair smaller than the cell, so a surface exactly on a cell boundary only fills one side.
            const float h = static_cast<float>(half * kCm - 0.1);
            return overlap(world, {ue.x, ue.y, ue.z}, {0, 0, 0, 1}, kChannelPawn, {1, h, h, h}, defaultParams, response);
        }

        void Start(int rx, int ry, int rz)
        {
            job.active = true;
            job.rx = rx;
            job.ry = ry;
            job.rz = rz;
            job.stack.assign(1, Cell{0, 0, 0, kUnits});
            for (auto& b : job.bits)
            {
                b.fill(0);
            }
            job.queries = 0;
            job.ms = 0;
        }

        // Region subdivided down to 1/8 block wherever the engine says something is there.
        bool Work(void* world, double deadline)
        {
            while (!job.stack.empty())
            {
                if (NowMs() > deadline)
                {
                    return false;
                }
                const Cell c = job.stack.back();
                job.stack.pop_back();
                ++job.queries;
                if (!Solid(world, job, c))
                {
                    continue;
                }
                if (c.size == 1)
                {
                    const int bx = c.x / 8, by = c.y / 8, bz = c.z / 8;
                    job.bits[bx + 8 * bz + 64 * by][c.y % 8] |= 1ull << ((c.z % 8) * 8 + (c.x % 8));
                    continue;
                }
                const std::uint8_t s = c.size / 2;
                for (int i = 0; i < 8; ++i)
                {
                    job.stack.push_back({static_cast<std::uint8_t>(c.x + (i & 1) * s), static_cast<std::uint8_t>(c.y + ((i >> 1) & 1) * s),
                                         static_cast<std::uint8_t>(c.z + ((i >> 2) & 1) * s), s});
                }
            }
            return true;
        }

        bool Send(std::uint32_t epoch)
        {
            const int x0 = job.rx * kRegion, y0 = job.ry * kRegion, z0 = job.rz * kRegion;
            proto::ColRegion region{x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, 0};
            std::vector<std::uint8_t> payload(sizeof(region));
            for (int by = 0; by < kRegion; ++by)
            {
                for (int bz = 0; bz < kRegion; ++bz)
                {
                    for (int bx = 0; bx < kRegion; ++bx)
                    {
                        const auto& b = job.bits[bx + 8 * bz + 64 * by];
                        if (std::none_of(b.begin(), b.end(), [](std::uint64_t v) { return v != 0; }))
                        {
                            continue;
                        }
                        proto::ColBlock block{x0 + bx, y0 + by, z0 + bz, 0, {}};
                        std::memcpy(block.bits, b.data(), sizeof(block.bits));
                        const auto* p = reinterpret_cast<const std::uint8_t*>(&block);
                        payload.insert(payload.end(), p, p + sizeof(block));
                        ++region.count;
                    }
                }
            }
            std::memcpy(payload.data(), &region, sizeof(region));
            return Link::Get().WriteCollision(proto::kColRegion, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        // Nearest region never sent; else the stalest one next to the player.
        bool Next(const Vec3& p, std::uint64_t now)
        {
            const int px = RegionOf(p.x), py = RegionOf(p.y), pz = RegionOf(p.z);
            int best[3]{};
            double bestDist = 1e18;
            for (int dy = -kBelow; dy <= kAbove; ++dy)
            {
                for (int dz = -kRadius; dz <= kRadius; ++dz)
                {
                    for (int dx = -kRadius; dx <= kRadius; ++dx)
                    {
                        const auto it = done.find(Key(px + dx, py + dy, pz + dz));
                        const bool adjacent = std::abs(dx) <= 1 && std::abs(dz) <= 1 && (dy == 0 || dy == -1);
                        double dist;
                        if (it == done.end())
                        {
                            dist = dx * dx + dz * dz + dy * dy * 4.0;  // the floor you're on beats the one below
                        }
                        else if (adjacent && now - it->second > kRefreshMs)
                        {
                            dist = 1000.0 + double(now - it->second) * -1e-6;  // after all new ones, stalest first
                        }
                        else
                        {
                            continue;
                        }
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            best[0] = px + dx;
                            best[1] = py + dy;
                            best[2] = pz + dz;
                        }
                    }
                }
            }
            if (bestDist >= 1e18)
            {
                return false;
            }
            Start(best[0], best[1], best[2]);
            return true;
        }

        void Forget(const Vec3& p)
        {
            const int px = RegionOf(p.x), pz = RegionOf(p.z);
            std::erase_if(done, [&](const auto& kv) {
                const auto unpack = [](std::int64_t v) { const int i = int(v & 0x1FFFFF); return i & 0x100000 ? i - 0x200000 : i; };
                const int rx = unpack(kv.first >> 42), rz = unpack(kv.first);
                return std::abs(rx - px) > kRadius + 2 || std::abs(rz - pz) > kRadius + 2;
            });
        }
    }

    void CollisionTick(void* world, const Vec3& playerMc, std::uint32_t epoch)
    {
        if (!world || !Resolve())
        {
            return;
        }
        if (epoch != lastEpoch)
        {
            lastEpoch = epoch;
            done.clear();
            job.active = false;
        }
        const double start = NowMs(), deadline = start + kBudgetMs;
        const auto now = ::GetTickCount64();
        while (NowMs() < deadline)
        {
            if (!job.active && !Next(playerMc, now))
            {
                break;
            }
            const double t0 = NowMs();
            const bool finished = Work(world, deadline);
            job.ms += NowMs() - t0;
            if (!finished)
            {
                break;
            }
            if (!Send(epoch))
            {
                break;  // ring full: try again next tick
            }
            done[Key(job.rx, job.ry, job.rz)] = now;
            job.active = false;
            if (++regionsSent % 50 == 1)
            {
                Log(L"collision: region ({}, {}, {}) took {} queries, {:.1f} ms; {} regions sent", job.rx, job.ry, job.rz, job.queries, job.ms, regionsSent);
            }
        }
        if (now >= nextForget)
        {
            nextForget = now + 2000;
            Forget(playerMc);  // regions left behind get scanned again when you come back
        }
    }

    bool CollisionReady(const Vec3& mc)
    {
        return done.contains(Key(RegionOf(mc.x), RegionOf(mc.y), RegionOf(mc.z)));
    }
}
