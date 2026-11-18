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
    // After overload reject all new events until the feeder has delivered every
    // already-accepted event and queued its all-notes-off recovery. Accepting
    // new notes before the recovery could leave them sounding indefinitely.
    bool push(uint64_t frame, const uint8_t* bytes, std::size_t length) noexcept {
        if (recovering_.load(std::memory_order_acquire)) return false;
        if (!bytes || !length || length > MaxBytes) return false;
        const uint32_t w = write_.load(std::memory_order_relaxed);
        const uint32_t r = read_.load(std::memory_order_acquire);
        if ((uint32_t)(w - r) >= Capacity) {
            request_recovery();
            return false;
        }
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

    // RT producer may also request recovery after a truncated, excessively
    // large, or invalid host event. This is a latch, not a stream message.
    void request_recovery() noexcept { recovering_.store(true, std::memory_order_release); }
    bool recovery_pending() const noexcept {
        return recovering_.load(std::memory_order_acquire);
    }
    // Feeder thread ONLY. Must run after draining the accepted queue.
    bool finish_recovery_after_drain() noexcept {
#if defined(P007_TEST_EARLY_RECOVERY_MUTANT)
        // Test-only historical bug: release voices before prior queued notes.
        return recovering_.exchange(false, std::memory_order_acq_rel);
#else
        if (!empty()) return false;
        return recovering_.exchange(false, std::memory_order_acq_rel);
#endif
    }
    void reset_after_stop() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        recovering_.store(false, std::memory_order_relaxed);
    }
private:
    std::array<Entry, Capacity> items_{};
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    std::atomic<bool> recovering_{false};
};
