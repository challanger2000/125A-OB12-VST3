// Fixed-capacity single-producer/single-consumer MIDI transfer from VST3 audio thread to emulator.
// Never allocates, blocks or calls the firmware on the producer path.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

template<std::size_t Capacity, std::size_t MaxBytes>
class MidiRtQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be power of two");
    static_assert(MaxBytes >= 3 && MaxBytes <= UINT16_MAX, "invalid MIDI message capacity");
public:
    struct Entry {
        uint64_t frame = 0; // absolute 48-kHz output-producer frame
        uint16_t length = 0;
        std::array<uint8_t, MaxBytes> bytes{};
    };

    // Audio thread only. Retains contents independently of host-owned Event memory.
    bool push(uint64_t frame, const uint8_t* bytes, std::size_t length) noexcept {
        if (!bytes || !length || length > MaxBytes) return false;
        const uint32_t w = write_.load(std::memory_order_relaxed);
        const uint32_t r = read_.load(std::memory_order_acquire);
        if ((uint32_t)(w - r) >= Capacity) return false;
        Entry& slot = items_[w & (Capacity - 1)];
        slot.frame = frame;
        slot.length = (uint16_t)length;
        std::memcpy(slot.bytes.data(), bytes, length);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Emulator/feeder thread only. The pointer stays owned by this consumer until pop().
    const Entry* front() const noexcept {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return nullptr;
        return &items_[r & (Capacity - 1)];
    }

    // Number of 48-kHz frames that may be rendered before the next MIDI boundary.
    int frames_before_next(uint64_t frame, int maximum) const noexcept {
        if (maximum <= 0) return 0;
        const Entry* event = front();
        if (!event) return maximum;
        if (event->frame <= frame) return 0;
#if defined(P007_TEST_TIMING_MUTANT)
        // Test executable only: emulate the original block-wide timing bug.
        return maximum;
#else
        const uint64_t gap = event->frame - frame;
        return gap < (uint64_t)maximum ? (int)gap : maximum;
#endif
    }

    void pop() noexcept { read_.store(read_.load(std::memory_order_relaxed) + 1,
                                       std::memory_order_release); }
    bool empty() const noexcept { return front() == nullptr; }
    void reset_after_stop() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }
private:
    std::array<Entry, Capacity> items_{};
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
};
