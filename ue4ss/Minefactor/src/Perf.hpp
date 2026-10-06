#pragma once
// Tiny frame-cost accounting: Perf::Scope s(Perf::Collision); ... Perf::Report() logs averages.
#include <atomic>
#include <cstdint>
#include <windows.h>

namespace mf::perf
{
    enum Slot
    {
        Tick,       // whole PlayerTick
        Collision,
        Actors,
        Position,   // K2_SetActorLocation of the puppet
        Overlay,    // Draw() before Present (render thread)
        Render,     // Minecraft world content into the game (render ring, meshes)
        Count
    };
    inline const wchar_t* kNames[Count] = {L"tick", L"collision", L"actors", L"position", L"overlay", L"render"};
    inline std::atomic<std::uint64_t> total[Count]{};  // QPC ticks
    inline std::atomic<std::uint64_t> calls[Count]{};

    struct Scope
    {
        Slot slot;
        LARGE_INTEGER start{};
        explicit Scope(Slot s) : slot(s) { ::QueryPerformanceCounter(&start); }
        ~Scope()
        {
            LARGE_INTEGER end{};
            ::QueryPerformanceCounter(&end);
            total[slot] += static_cast<std::uint64_t>(end.QuadPart - start.QuadPart);
            ++calls[slot];
        }
    };
}
