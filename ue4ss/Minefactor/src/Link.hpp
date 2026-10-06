#pragma once
// The host end of the shared memory (ported from SkyCraft skse/src/Link.h). We create it as a file
// under /dev/shm (Z:\dev\shm\... from inside Proton): Wine maps file-backed sections MAP_SHARED, so
// native Minecraft mapping the same file sees the same pages.
#include <cstdint>
#include <functional>
#include <string>

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
        void WriteWaterGrid(const proto::WaterGrid& grid);
        // Which save is loaded (UTF-8, "" while unknown) and whether we're the server.
        void WriteHostInfo(const std::string& worldName, std::uint32_t flags);

        // Input ring (producer; game thread only). Drops the event if MC is a full ring behind.
        void PushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);
        // Collision ring (producer; one thread only). False if the ring is full.
        bool WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes);
        // Actor table (producer; game thread): nearby NPCs Minecraft mirrors as hittable stand-ins.
        void WriteActors(const proto::ActorRecord* records, std::uint32_t count);
        // Render ring (consumer): calls fn(type, payload, bytes) for each pending message, up to about
        // maxBytes of payload. Minecraft stalls its render thread when this ring is full.
        void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes);
        // World entities (arrows, dropped items, ...) + block outline: seqlock read.
        bool ReadWorldEntities(proto::WorldEntities& out) const;
        // Event ring (consumer). False when empty.
        bool PopEvent(proto::McEvent& out);

        // Overlay triple buffer (consumer; render thread). True if a newer frame was swapped into the
        // front slot; FrontPixels/FrontHeader describe the current front slot.
        bool AcquireOverlayFrame();
        // Minecraft (re)connected: its writer starts at slot 1, so restart the swap from scratch.
        void ResetOverlay();
        [[nodiscard]] const std::uint8_t* FrontPixels() const;
        [[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

    private:
        template <class T>
        T* At(std::uint64_t off) const { return reinterpret_cast<T*>(base_ + off); }

        std::uint8_t* base_{nullptr};
        std::uint32_t overlayFront_{2};
        std::uint64_t lastMcBeat_{0};
        std::uint64_t lastMcBeatChange_{0};
    };
}
