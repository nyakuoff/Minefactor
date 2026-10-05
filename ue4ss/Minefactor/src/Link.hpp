#pragma once
// The host end of the shared memory (ported from SkyCraft skse/src/Link.h). We create it as a file
// under /dev/shm (Z:\dev\shm\... from inside Proton): Wine maps file-backed sections MAP_SHARED, so
// native Minecraft mapping the same file sees the same pages.
#include <cstdint>
#include <functional>

#include "minefactor_protocol.h"

namespace mf
{
    namespace proto = minefactor::proto;

    class Link
    {
    public:
        static Link& Get();

        bool Create();
        [[nodiscard]] bool Valid() const { return base_ != nullptr; }

        // True if Minecraft's heartbeat changed within the last few seconds (clocks needn't match).
        [[nodiscard]] bool McAlive();
        void Heartbeat();
        [[nodiscard]] std::uint32_t McPid() const;

        void WriteSkyState(const proto::SkyState& state);
        bool ReadMcState(proto::McState& out) const;

        // Input ring (producer; game thread only). Drops the event if MC is a full ring behind.
        void PushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);
        // Collision ring (producer; one thread only). False if the ring is full.
        bool WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes);
        // Event ring (consumer). False when empty.
        bool PopEvent(proto::McEvent& out);

    private:
        template <class T>
        T* At(std::uint64_t off) const { return reinterpret_cast<T*>(base_ + off); }

        std::uint8_t* base_{nullptr};
        std::uint64_t lastMcBeat_{0};
        std::uint64_t lastMcBeatChange_{0};
    };
}
