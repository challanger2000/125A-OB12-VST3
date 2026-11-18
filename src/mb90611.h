// Peripherals shared by the OB-12 Master and Slave MB90611A models.
#pragma once
#include <deque>
#include "emu.h"
#include <vector>

struct ReloadTimer {
    int base = 0;
    int vector = 0;
    u16 tmcsr = 0, reload = 0, count = 0;
    s64 prescale = 0;
    bool running = false;
    int divider() const { int csl = (tmcsr >> 10) & 3; return csl == 0 ? 2 : csl == 1 ? 8 : csl == 2 ? 32 : 0; }
    void trigger() { count = reload; running = true; prescale = 0; }
    void advance(int cycles) {
        if (!running || !(tmcsr & 2) || !divider()) return;
        prescale += cycles;
        int div = divider();
        // One count per div cycles: down to 0, and the count after 0 sets UF and reloads or stops.
        // Taken in runs instead of one by one (up to 83 counts a call at the usual 167 cycles).
        s64 steps = prescale / div;
        while (steps > 0) {
            if (count >= steps) { count = (u16)(count - steps); prescale -= steps * div; break; }
            s64 used = (s64)count + 1;  // down to 0, then the underflow
            prescale -= used * div;
            steps -= used;
            tmcsr |= 4;  // UF
            if (tmcsr & 0x10) count = reload; else { count = 0; running = false; return; }
        }
    }
    bool pending() const { return (tmcsr & 0x0c) == 0x0c; }
};

// The interrupt sources asked one by one, in the order source_pending() asks them, instead of
// asking every source for each of the 32 vectors. A vector two sources share is answered by the
// first, as there; best() then goes up the pending vectors: lowest level wins, on a tie the
// lowest vector.
struct PendingScan {
    u64 claimed = 0, pending = 0;
    void add(int vector, bool is_pending) {
        if (vector < 0 || vector > 63) return;
        u64 bit = (u64)1 << vector;
        if (claimed & bit) return;
        claimed |= bit;
        if (is_pending) pending |= bit;
    }
    int best(const u8* io) const {
        u64 left = pending & ((((u64)1 << 0x2b) - 1) & ~(((u64)1 << 0x0b) - 1));  // vectors 0B to 2A
        int best = 0, level = 7;
        while (left) {
            int vector = 0;
            while (!(left >> vector & 1)) vector++;
            left &= left - 1;
            int icr_level = io[0xb0 + (vector - 11) / 2] & 7;
            if (icr_level < level) { level = icr_level; best = vector; }
        }
        return best;
    }
};

struct Uart {
    int base = 0, tx_vector = 0, rx_vector = 0;
    const char* name = "";
    u8 scr = 0, smr = 0, ssr = 0x08;  // TDRE set
    int tx_timer = 0, rx_timer = 0;
    long sent = 0;
    u8 rx_data = 0;
    std::deque<u8> rx_queue;  // a whole .syx can wait here: drained from the front
    std::vector<u8> head;
    void advance(int cycles) {
        if (tx_timer > 0 && (tx_timer -= cycles) <= 0) { tx_timer = 0; ssr |= 0x08; }
        if (!rx_queue.empty() && (rx_timer -= cycles) <= 0 && !(ssr & 0x10)) {
            rx_data = rx_queue.front();
            rx_queue.erase(rx_queue.begin());
            ssr |= 0x10;  // RDRF
            rx_timer = 5120;  // one MIDI byte at 31250 baud
        }
    }
    u8 read() { ssr &= ~0x10; return rx_data; }
    void write(u8 data) {
        sent++;
        head.push_back(data);
        ssr &= ~0x08;
        tx_timer = 2000;
    }
    bool tx_pending() const { return (ssr & 0x08) && (ssr & 0x01); }
    bool rx_pending() const { return (ssr & 0x10) && (ssr & 0x02); }
};

