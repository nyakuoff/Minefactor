#include "Link.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <immintrin.h>
#include <windows.h>

#include "Log.hpp"

namespace mf
{
    namespace
    {
        template <class T>
        std::atomic_ref<T> Atomic(T& value)
        {
            return std::atomic_ref<T>(value);
        }

        constexpr std::uint64_t kMcTimeoutMs = 3000;
    }

    Link& Link::Get()
    {
        static Link link;
        return link;
    }

    bool Link::Create()
    {
        if (base_)
        {
            return true;
        }
        constexpr auto size = proto::kMappingBytes;
        HANDLE file = ::CreateFileW(proto::kMappingPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            Log(L"can't create {} (error {}); is /dev/shm visible inside Proton?", proto::kMappingPath, ::GetLastError());
            return false;
        }
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(size);
        if (!::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !::SetEndOfFile(file))
        {
            Log(L"can't size {} to {} bytes (error {})", proto::kMappingPath, size, ::GetLastError());
            ::CloseHandle(file);
            return false;
        }
        HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        ::CloseHandle(file);
        if (!mapping)
        {
            Log(L"CreateFileMapping failed ({})", ::GetLastError());
            return false;
        }
        base_ = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        ::CloseHandle(mapping);  // the view keeps the section alive
        if (!base_)
        {
            Log(L"MapViewOfFile failed ({})", ::GetLastError());
            return false;
        }

        // Minecraft may still have the file mapped from a previous run: reset everything the host
        // owns so rings and the overlay swap start from a known state.
        auto* header = At<proto::Header>(proto::kOffHeader);
        std::memset(base_ + proto::kOffSkyState, 0, sizeof(proto::SkyState));
        std::memset(base_ + proto::kOffOverlayCtl, 0, 0x100);
        std::memset(base_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
        std::memset(base_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
        std::memset(base_ + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
        std::memset(base_ + proto::kOffEventRing, 0, proto::kEventRingDataOff);
        std::memset(base_ + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
        std::memset(base_ + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
        header->version = proto::kVersion;
        header->skyrimPid = ::GetCurrentProcessId();
        header->skyrimHeartbeatMs = ::GetTickCount64();
        Atomic(header->magic).store(proto::kMagic, std::memory_order_release);

        LARGE_INTEGER freq{};
        ::QueryPerformanceFrequency(&freq);
        Log(L"shared memory {} ready ({} MB); QPC frequency {} Hz", proto::kMappingPath, size >> 20, freq.QuadPart);
        return true;
    }

    bool Link::McAlive()
    {
        if (!base_)
        {
            return false;
        }
        const auto beat = Atomic(At<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
        const auto now = ::GetTickCount64();
        if (beat != lastMcBeat_)
        {
            lastMcBeat_ = beat;
            lastMcBeatChange_ = now;
        }
        return beat != 0 && now - lastMcBeatChange_ < kMcTimeoutMs;
    }

    std::uint32_t Link::McPid() const
    {
        return base_ ? Atomic(At<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire) : 0;
    }

    void Link::Heartbeat()
    {
        if (base_)
        {
            Atomic(At<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs).store(::GetTickCount64(), std::memory_order_release);
        }
    }

    void Link::WriteSkyState(const proto::SkyState& state)
    {
        if (!base_)
        {
            return;
        }
        auto* dst = At<proto::SkyState>(proto::kOffSkyState);
        auto seq = Atomic(dst->seq);
        const auto s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(reinterpret_cast<std::uint8_t*>(dst) + 4, reinterpret_cast<const std::uint8_t*>(&state) + 4, sizeof(proto::SkyState) - 4);
        seq.store(s + 2, std::memory_order_release);
    }

    bool Link::ReadMcState(proto::McState& out) const
    {
        if (!base_)
        {
            return false;
        }
        auto* src = At<proto::McState>(proto::kOffMcState);
        auto seq = Atomic(src->seq);
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const auto s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1)
            {
                _mm_pause();
                continue;
            }
            std::memcpy(&out, src, sizeof(proto::McState));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1)
            {
                return true;
            }
        }
        return false;
    }

    void Link::PushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c)
    {
        if (!base_)
        {
            return;
        }
        auto* ring = base_ + proto::kOffInputRing;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
        const auto head = Atomic(headRef).load(std::memory_order_relaxed);
        const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
        if (head - tail >= proto::kInputRingEntries)
        {
            return;
        }
        auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (head & (proto::kInputRingEntries - 1));
        *entry = {static_cast<std::uint16_t>(type), code, a, b, c};
        Atomic(headRef).store(head + 1, std::memory_order_release);
    }

    bool Link::WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes)
    {
        if (!base_)
        {
            return false;
        }
        auto* ring = base_ + proto::kOffCollisionRing;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
        auto* data = ring + proto::kColRingDataOff;
        constexpr auto size = proto::kColRingDataBytes;

        const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
        if (msgBytes > size / 2)
        {
            Log(L"collision message too large ({} bytes)", msgBytes);
            return false;
        }
        auto head = Atomic(headRef).load(std::memory_order_relaxed);
        const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
        auto pos = head % size;
        const auto padBytes = (pos + msgBytes > size) ? size - pos : 0;
        if (size - (head - tail) < msgBytes + padBytes)
        {
            return false;
        }
        if (padBytes)
        {
            *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {proto::kColPad, 0};
            head += padBytes;
            pos = 0;
        }
        *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {type, bytes};
        std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
        Atomic(headRef).store(head + msgBytes, std::memory_order_release);
        return true;
    }

    void Link::WriteActors(const proto::ActorRecord* records, std::uint32_t count)
    {
        if (!base_)
        {
            return;
        }
        auto* table = At<proto::ActorTable>(proto::kOffActorTable);
        auto seq = Atomic(table->seq);
        const auto s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        count = std::min(count, proto::kMaxActors);
        table->count = count;
        if (count)
        {
            std::memcpy(table->actors, records, sizeof(proto::ActorRecord) * count);
        }
        seq.store(s + 2, std::memory_order_release);
    }

    bool Link::PopEvent(proto::McEvent& out)
    {
        if (!base_)
        {
            return false;
        }
        auto* ring = base_ + proto::kOffEventRing;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
        const auto head = Atomic(headRef).load(std::memory_order_acquire);
        auto tail = Atomic(tailRef).load(std::memory_order_relaxed);
        if (tail >= head)
        {
            return false;
        }
        if (head - tail > proto::kEventRingEntries)
        {
            tail = head - proto::kEventRingEntries;
        }
        out = reinterpret_cast<const proto::McEvent*>(ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
        Atomic(tailRef).store(tail + 1, std::memory_order_release);
        return true;
    }

    bool Link::AcquireOverlayFrame()
    {
        if (!base_)
        {
            return false;
        }
        auto& state = At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
        if (!(Atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
        {
            return false;
        }
        overlayFront_ = Atomic(state).exchange(overlayFront_, std::memory_order_acq_rel) & 3;
        return true;
    }

    void Link::ResetOverlay()
    {
        if (base_)
        {
            Atomic(At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
            overlayFront_ = 2;
        }
    }

    const std::uint8_t* Link::FrontPixels() const
    {
        return base_ + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront_;
    }

    const proto::OverlaySlotHdr* Link::FrontHeader() const
    {
        return At<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront_);
    }
}
