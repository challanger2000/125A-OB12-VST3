#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include "tms57070/tms57070.h"
#include "ob12_engine.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#pragma comment(lib, "synchronization.lib")

#define OB12_NO_MAIN
namespace master {
#include "ob12_master.cpp"
}
namespace slave {
#include "ob12_slave.cpp"
}

namespace {

// Workers spin through the gaps of a burst and sleep on the address between bursts.
struct Gate {
    alignas(64) std::atomic<long> go{0};
    alignas(64) std::atomic<int> left{0};
    alignas(64) std::atomic<int> sleepers{0};
    void open(int workers) {
        left.store(workers, std::memory_order_relaxed);
        go.fetch_add(1, std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst)) WakeByAddressAll(&go);
    }
    void wait() { while (left.load(std::memory_order_acquire)) _mm_pause(); }
    void done() { left.fetch_sub(1, std::memory_order_release); }
};

static int spin_limit = getenv("OB12_SPIN") ? atoi(getenv("OB12_SPIN")) : 2000;

static void wait_for(Gate& gate, long seen, const std::atomic<bool>& stop) {
    for (int spins = 0; spins < spin_limit; spins++) {
        if (gate.go.load(std::memory_order_acquire) != seen) return;
        _mm_pause();
    }
    gate.sleepers.fetch_add(1, std::memory_order_seq_cst);
    while (gate.go.load(std::memory_order_seq_cst) == seen && !stop.load(std::memory_order_relaxed))
        WaitOnAddress(&gate.go, &seen, sizeof seen, 5);
    gate.sleepers.fetch_sub(1, std::memory_order_seq_cst);
}

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    tms::Core* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false, start_pending = false;

    // An update transaction (03, then address and value words, under one chip select) takes effect
    // when the Slave releases the select, all words at once: no DSP sample sees half of a burst.
    // The Slave writes a voice's coupled words (oscillator rate and its partner, a glide stage's
    // A and A+1) inside one burst; taken in one by one, a sample between two of them clicked.
    // See "Host bursts" in docs/HARDWARE.md. OB12_HOST_SINGLE=1 is the old model.
    bool burst = getenv("OB12_HOST_SINGLE") == nullptr;
    std::vector<std::pair<int, int32_t>> pending, ready;

    // A released burst goes in at the start of the chip's next sample, so no sample reads half of it.
    void sample_start() {
        for (auto& w : ready) dsp->cmem[w.first] = w.second;
        ready.clear();
    }

    void deselect() {
        if (state != Update) return;
        ready.insert(ready.end(), pending.begin(), pending.end());
        pending.clear();
        state = Idle;
        buffer.clear();
    }
    void byte(u8 data) {
        switch (state) {
        case Idle:
            if (data == 0x0c) { state = Program; buffer.clear(); }
            else if (data == 0x05) { state = Coefficients; buffer.clear(); }
            else if (data == 0x03) { state = Update; buffer.clear(); }
            break;
        case Program:
            buffer.push_back(data);
            if (buffer.size() == 12 + 2048) {
                const u8* h = buffer.data();
                running = false;
                dsp->reset();
                dsp->load_control(h);
                for (int i = 0; i < 512; i++) {
                    const u8* w = h + 12 + i * 4;
                    dsp->pmem[i] = w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3];
                }
                dsp->select_fast();  // a known program runs from its translation, see tms57070.h
                state = Idle;
            }
            break;
        case Coefficients:
            buffer.push_back(data);
            if (buffer.size() == 1536) {
                for (int i = 0; i < 512; i++) {
                    const u8* w = buffer.data() + i * 3;
                    dsp->cmem[i] = (int32_t)((uint32_t)(w[0] << 16 | w[1] << 8 | w[2]) << 8) >> 8;
                }
                start_pending = true;
                state = Idle;
            }
            break;
        case Update:
            buffer.push_back(data);
            if (buffer.size() == 5) {
                int address = (buffer[0] << 8 | buffer[1]) & 0x1ff;
                int32_t value = (int32_t)((uint32_t)(buffer[2] << 16 | buffer[3] << 8 | buffer[4]) << 8) >> 8;
                if (burst) pending.push_back({address, value}); else dsp->cmem[address] = value;
                buffer.clear();
            }
            break;
        }
    }
};

}  // namespace

struct Engine::Impl {
    master::Master* m = nullptr;
    slave::Slave* s = nullptr;
    Host host[5];
    // Chips read each other two half frames late, so two half frames can run per gate. Host bytes
    // stay per chip and per half, so each lands between the same two half frames as on one thread.
    struct Byte { u8 data; bool deselect; };
    std::vector<Byte> held[2][5][2];  // batch parity, chip, half
    int32_t snap[2][2][5][3][2] = {};  // batch parity, half, chip, port, channel: ports after that run
    int filling = 0, half = 0;         // where the CPUs put bytes now
    long batch = 0;
    size_t m_sent = 0, s_sent = 0;
    long k = 0;
    int slice = 167;  // CPU cycles in this half frame
    Gate cpu_gate, dsp_gate;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::string error;
    // Which thread runs what. groups[0] is the caller's thread: it runs both CPUs, then its own
    // chips; every other group is a worker. Chips of one batch do not read each other, so any
    // grouping gives the same output. The work is about 6.6 us a sample per chip and 2.8 us for
    // both CPUs; a thread that finishes early spins for the others, so fewer, fuller threads
    // cost less at real time and leave less margin. See "Threads" in docs/HARDWARE.md.
    // OB12_THREADS=4/0/1/2/3 style: groups separated by /, chips by commas; "cpu" as a group
    // gives the Master its own thread as before (OB12_THREADS=/cpu/0/1/2/3/4 is the old layout).
    std::vector<std::vector<int>> groups;
    bool master_thread = false;
    int32_t* out = nullptr;
    int made = 0;

    // Inputs from other threads, taken in at each millisecond.
    std::mutex input_lock;
    std::atomic<bool> input_waiting{false};
    std::vector<u8> midi_bytes;
    std::vector<std::vector<u8>> pending_sysex;
    int pending_gap = 100;
    bool forget = false;
    struct KeyEvent { int bar, bit; bool down; };
    std::vector<KeyEvent> key_events;
    struct PotEvent { int channel, mux, value; };
    std::vector<PotEvent> pot_events;
    int turns = 0, enc_detents = 0;
    long enc_start = 0;

    mutable std::mutex lcd_lock;
    uint8_t lcd_copy[30 * 64] = {};
    uint8_t led_copy[16] = {};
    bool lcd_ready = false;
    std::atomic<bool> ready{false};

    // MIDI OUT: its system exclusive, kept for sysex_sent (a bulk dump).
    mutable std::mutex out_lock;
    std::vector<u8> sysex_out;
    bool out_in_sysex = false;
    void drain_out() {
        auto& sent = m->midi_sent();
        if (sent.empty()) return;
        std::lock_guard<std::mutex> hold(out_lock);
        for (u8 b : sent) {
            if (b >= 0xf8) continue;  // real time bytes may sit inside a message
            if (b == 0xf0) out_in_sysex = true;
            else if (b & 0x80 && b != 0xf7) out_in_sysex = false;
            if (out_in_sysex && sysex_out.size() < (16u << 20)) sysex_out.push_back(b);
            if (b == 0xf7) out_in_sysex = false;
        }
        sent.clear();
    }

    // send_sysex: messages waiting, and the half frame the next may go.
    std::deque<std::vector<u8>> sysex_queue;
    long sysex_next = 0, sysex_gap = 9600;
    std::atomic<int> sysex_left{0};
    // The next message waits for this one's own time on the line (320 us a byte at 31250 baud) and
    // the gap. Waiting for an empty input instead starved it: the host's MIDI clock (F8 every 21 ms
    // in VSTHost) kept the input busy, and the update stopped at "Waiting for MIDI data".
    void sysex_step() {  // every millisecond, after the inputs
        if (sysex_queue.empty() || k < sysex_next) return;
        const std::vector<u8>& message = sysex_queue.front();
        m->midi_in(message);
        sysex_next = k + (long)(message.size() * 30.72) + sysex_gap;
        sysex_queue.pop_front();
        sysex_left.store((int)sysex_queue.size(), std::memory_order_relaxed);
    }

    // The unit's memory: see set_flash_file in ob12_engine.h.
    std::string flash_path;
    std::vector<u8> factory;
    long flash_seen = 0, flash_changed_at = 0;
    bool flash_dirty = false;
    std::atomic<long> flash_count{0};

    static void sector_of(u32 offset, u32& start, u32& size) {  // 29F800 top boot block map
        if (offset >= 0xfc000) { start = 0xfc000; size = 0x4000; }
        else if (offset >= 0xfa000) { start = 0xfa000; size = 0x2000; }
        else if (offset >= 0xf8000) { start = 0xf8000; size = 0x2000; }
        else if (offset >= 0xf0000) { start = 0xf0000; size = 0x8000; }
        else { start = offset & 0xf0000; size = 0x10000; }
    }
    void load_flash() {
        std::ifstream f(flash_path, std::ios::binary);
        char magic[8];
        if (!f.read(magic, 8) || memcmp(magic, "OB12FLSH", 8)) return;
        uint32_t start, size;
        while (f.read((char*)&start, 4) && f.read((char*)&size, 4)) {
            if (start + size > m->flash.size()) break;
            std::vector<char> data(size);
            if (!f.read(data.data(), size)) break;
            memcpy(m->flash.data() + start, data.data(), size);
        }
    }
    void save_flash() {
        if (flash_path.empty()) return;
        std::string temp = flash_path + ".new";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            f.write("OB12FLSH", 8);
            for (u32 at = 0; at < m->flash.size();) {
                u32 start, size;
                sector_of(at, start, size);
                if (memcmp(m->flash.data() + start, factory.data() + start, size)) {
                    f.write((const char*)&start, 4);
                    f.write((const char*)&size, 4);
                    f.write((const char*)m->flash.data() + start, size);
                }
                at = start + size;
            }
            if (!f) return;
        }
        MoveFileExA(temp.c_str(), flash_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        flash_dirty = false;
    }
    void watch_flash() {  // every millisecond, on the engine's thread
        long now = m->flash_erases + m->flash_programs;
        flash_count.store(now, std::memory_order_relaxed);
        if (now != flash_seen) { flash_seen = now; flash_changed_at = k; flash_dirty = true; }
        else if (flash_dirty && k - flash_changed_at > 96000) save_flash();
    }
    long ready_at = 0;  // half frame the Slave had answered

    void take_inputs() {
        if (!input_waiting.exchange(false)) return;
        std::lock_guard<std::mutex> hold(input_lock);
        if (!midi_bytes.empty()) { m->midi_in(midi_bytes); midi_bytes.clear(); }
        if (forget) {
            forget = false;
            if (!flash_path.empty()) DeleteFileA(flash_path.c_str());
            flash_path.clear();
            flash_dirty = false;
        }
        if (!pending_sysex.empty()) {
            for (auto& message : pending_sysex) sysex_queue.push_back(std::move(message));
            pending_sysex.clear();
            sysex_gap = pending_gap * 96L;
            sysex_left.store((int)sysex_queue.size(), std::memory_order_relaxed);
        }
        for (auto& e : key_events) {
            u8 bit = (u8)(1 << e.bit);
            if (e.down) m->key_live[e.bar] |= bit; else m->key_live[e.bar] &= ~bit;
        }
        key_events.clear();
        for (auto& e : pot_events) m->pot_live[e.channel][e.mux] = (short)e.value;
        pot_events.clear();
        enc_detents += turns;
        turns = 0;
    }

    void encoder_step() {
        if (!enc_detents) return;
        if (!enc_start) enc_start = k;
        int phase = (int)((k - enc_start) / 384);  // 4 ms per quarter
        static const int a_first[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
        int i = phase & 3;
        if (enc_detents > 0) m->enc_set(a_first[i][0], a_first[i][1]);
        else m->enc_set(a_first[i][1], a_first[i][0]);
        if (phase >= 4) {
            enc_detents += enc_detents > 0 ? -1 : 1;
            enc_start = enc_detents ? k : 0;
        }
    }

    void cpu_slice() {
        double now = k / 96000.0;
        m->pot_clock = now;
        encoder_step();
        if (k % 96 == 0) {
            s->ppg_tick();
            s->reload_tick();
            take_inputs();
            if (!flash_path.empty()) watch_flash();
            sysex_step();
            drain_out();
            if (k % 3072 == 0) {
                std::lock_guard<std::mutex> hold(lcd_lock);
                lcd_ready = m->lcd.bitmap(lcd_copy);
                memcpy(led_copy, m->led, sizeof led_copy);
            }
        }
        slice = k % 3 == 2 ? 166 : 167;  // 16 MHz: 500 cycles per three half frames
        if (!master_thread) {
            uint64_t t0 = __rdtsc();
            m->run(slice); m->tick(m->executed());
            uint64_t t1 = __rdtsc();
            s->run(slice); s->tick(s->executed());
            spent_master.ticks += t1 - t0;
            spent_slave.ticks += __rdtsc() - t1;
        } else {
            cpu_gate.open(1);
            s->run(slice); s->tick(s->executed());
            cpu_gate.wait();
        }
        if (!error.empty()) throw std::runtime_error(error);
        auto& mu = m->link();
        auto& su = s->link();
        for (; m_sent < mu.head.size(); m_sent++) su.rx_queue.push_back(mu.head[m_sent]);
        for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);
        if (s_sent >= 18 && !ready_at) { ready_at = k; ready.store(true, std::memory_order_relaxed); }
        k++;
    }

    // One chip through both half frames of a batch, on its own worker.
    void chip_batch(int n) {
        long b = dsp_batch;
        uint64_t began = __rdtsc();
        int now = b & 1, before = now ^ 1;
        Host& h = host[n];
        tms::Core* d = h.dsp;
        for (int part = 0; part < 2; part++) {
            auto& list = held[now][n][part];
            for (auto& byte : list) {
                if (byte.deselect) h.deselect(); else h.byte(byte.data);
            }
            list.clear();
            // A word sent in one slot arrives two slots later, see "Two-slot audio bus".
            for (int c = 0; c < 2; c++) {
                if (n == 0) {
                    d->ar[0][c] = snap[before][part][4][0][c];
                    d->ar[1][c] = 0;
                } else {
                    d->ar[0][c] = snap[before][part][n - 1][1][c];
                    d->ar[1][c] = snap[before][part][n - 1][2][c];
                }
            }
            if (part == 0) h.sample_start();
            if (h.start_pending && part == 0) { h.start_pending = false; h.running = true; }
            if (h.running) d->run(256);
            memcpy(snap[now][part][n], d->ax, sizeof d->ax);
        }
        spent_chip[n].ticks += __rdtsc() - began;
    }

    long dsp_batch = 0;
    // Study (OB12_TIME_LOG): time stamp counter ticks spent in each part, summed over the run.
    struct alignas(64) Spent { uint64_t ticks = 0; } spent_chip[5], spent_cpu, spent_wait, spent_master, spent_slave;
    long time_from = getenv("OB12_TIME_LOG") ? (long)(atof(getenv("OB12_TIME_LOG")) * 48000) : -1;  // OB12_TIME_LOG=seconds: count from there (1 or less: from the start)

    void next_batch() {
        filling = batch & 1;
        if (batch == time_from) { for (auto& c : spent_chip) c.ticks = 0; spent_cpu.ticks = spent_wait.ticks = spent_master.ticks = spent_slave.ticks = 0; }
        uint64_t began = __rdtsc();
        half = 0;
        cpu_slice();
        half = 1;
        cpu_slice();
        uint64_t slices = __rdtsc();
        spent_cpu.ticks += slices - began;
        if (batch > 0) {
            if (groups.size() > 1) dsp_gate.wait();
            spent_wait.ticks += __rdtsc() - slices;
            const int32_t* frame = snap[(batch - 1) & 1][1][4][1];  // MOUT after the odd half frame
            out[made * 2] = frame[0];
            out[made * 2 + 1] = frame[1];
            made++;
        }
        dsp_batch = batch;
        if (groups.size() > 1) dsp_gate.open((int)groups.size() - 1);
        for (int chip : groups[0]) chip_batch(chip);
        batch++;
    }
};

Engine::Engine(const std::string& rom_dir) : impl(new Impl) {
    Impl& e = *impl;
    tms::Options options;
    options.audio_port_writes = true;
    options.true_dmem_ring = true;
    options.lock_delay = true;
    options.maclo_multiply = true;
    options.divide_1fc = true;
    options.noise_generator = true;
    options.late_mode_change = true;
    options.compare_3e = true;
    options.maclo_shift = true;
    options.maclo_unsigned = true;
    options.queued_pointer_load = true;
    options.bus_sum_saturates = true;
    options.reuse_coefficient = true;
    options.late_saturation_mode = true;
    options.xread_flush = true;

    e.m = new master::Master();
    e.s = new slave::Slave();
    e.m->load(rom_dir);
    e.s->load(rom_dir);
    e.m->attach(e.m);
    e.s->attach(e.s);
    e.m->reset();
    e.s->reset();
    e.m->go_lean();
    e.s->go_lean();
    // DASP4 has 3 x 44C256 DRAM, 256K x 12 bits: 128K words of 24 bits in two accesses.
    tms::Options options4 = options;
    options4.xmem_words = 0x20000;
    // An XMEM read's data is in XRD 5 words later, see "XMEM read time" in docs/HARDWARE.md.
    options4.xread_cycles = 5;
    for (int n = 0; n < 5; n++) e.host[n].dsp = new tms::Core(n == 4 ? options4 : options);
    if (getenv("OB12_STUDY_SHIFT")) for (auto& h : e.host) h.dsp->study_shift = true;  // to time its cost
    e.s->on_dsp_byte = [&e](u8 selected, u8 data) {
        for (int chip = 0; chip < 5; chip++) if (selected & (1 << chip)) e.held[e.filling][chip][e.half].push_back({data, false});
    };
    e.s->on_dsp_select = [&e]() {
        for (int chip = 0; chip < 5; chip++) e.held[e.filling][chip][e.half].push_back({0, true});
    };

    // Default: everything on the caller's thread. With the DSP programs translated a sample is
    // about 9 us of work (CPUs 3.3, voice chips 0.8 each, effects chip 2.2) in a 20.8 us sample:
    // 0.48 of a core and 2.2 times real time on one thread, nothing spinning. Workers buy margin:
    // OB12_THREADS=4/0,1,2,3 is 3.7 times at 0.6 of a core, /0/1/2/3/4 is 6 times at 1.1.
    const char* layout = getenv("OB12_THREADS");
    if (!layout) layout = "0,1,2,3,4";
    e.groups.assign(1, {});
    for (const char* c = layout; *c; c++) {
        if (*c == '/') e.groups.emplace_back();
        else if (!strncmp(c, "cpu", 3)) { e.master_thread = true; e.groups.pop_back(); c += 2; if (c[1] == '/') c++; e.groups.emplace_back(); }
        else if (*c >= '0' && *c <= '4') e.groups.back().push_back(*c - '0');
    }
    {
        bool seen_chip[5] = {};
        for (auto& g : e.groups) for (int chip : g) seen_chip[chip] = true;
        for (int chip = 0; chip < 5; chip++) if (!seen_chip[chip]) e.groups[0].push_back(chip);  // every chip runs somewhere
        for (size_t g = e.groups.size(); g-- > 1;) if (e.groups[g].empty()) e.groups.erase(e.groups.begin() + g);
    }
    if (e.master_thread) e.workers.emplace_back([&e]() {
        long seen = 0;
        for (;;) {
            wait_for(e.cpu_gate, seen, e.stop);
            if (e.stop) return;
            seen++;
            try { e.m->run(e.slice); e.m->tick(e.m->executed()); }
            catch (const std::exception& error) { e.error = error.what(); }
            e.cpu_gate.done();
        }
    });
    // Study (OB12_SPREAD=1 hint, 2 hard): each worker on a physical core of its own. Two of our
    // threads on the two halves of one core slow each other, the spinning one as much as a working one.
    static int spread = getenv("OB12_SPREAD") ? atoi(getenv("OB12_SPREAD")) : 0;
    for (size_t g = 1; g < e.groups.size(); g++) e.workers.emplace_back([&e, g]() {
        if (spread) {
            SYSTEM_INFO info; GetSystemInfo(&info);
            DWORD logical = 2 * (DWORD)g % info.dwNumberOfProcessors;
            if (spread == 2) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << logical);
            else SetThreadIdealProcessor(GetCurrentThread(), logical);
        }
        long seen = 0;
        for (;;) {
            wait_for(e.dsp_gate, seen, e.stop);
            if (e.stop) return;
            seen++;
            for (int chip : e.groups[g]) e.chip_batch(chip);
            e.dsp_gate.done();
        }
    });
}

void Engine::set_flash_file(const std::string& path) {
    Impl& e = *impl;
    e.flash_path = path;
    e.factory = e.m->flash;
    e.load_flash();
    e.flash_seen = e.m->flash_erases + e.m->flash_programs;
}

void Engine::send_sysex(const uint8_t* bytes, int count, int gap_ms) {
    std::vector<std::vector<u8>> messages;
    for (int i = 0; i < count; i++) {
        if (bytes[i] != 0xf0) continue;
        int j = i;
        while (j < count && bytes[j] != 0xf7) j++;
        if (j >= count) break;
        messages.emplace_back(bytes + i, bytes + j + 1);
        i = j;
    }
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->pending_sysex.insert(impl->pending_sysex.end(), messages.begin(), messages.end());
    impl->pending_gap = gap_ms;
    impl->input_waiting = true;
}

void Engine::forget_flash() {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->forget = true;
    impl->input_waiting = true;
}

std::string Engine::read_sysex(const std::wstring& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    if (b.empty()) return "cannot read the file";
    out.clear();
    if (b.size() > 14 && !memcmp(b.data(), "MThd", 4)) {  // a Standard MIDI File: its F0 events
        size_t pos = 8 + (b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]);
        auto vlq = [&](size_t& p) { uint32_t v = 0; while (p < b.size()) { uint8_t c = b[p++]; v = v << 7 | (c & 0x7f); if (!(c & 0x80)) break; } return v; };
        while (pos + 8 <= b.size()) {
            size_t end = pos + 8 + (b[pos + 4] << 24 | b[pos + 5] << 16 | b[pos + 6] << 8 | b[pos + 7]);
            bool track = !memcmp(&b[pos], "MTrk", 4);
            pos += 8;
            uint8_t running = 0;
            while (track && pos < end && pos < b.size()) {
                vlq(pos);
                uint8_t status = b[pos];
                if (status == 0xff) { pos += 2; uint32_t n = vlq(pos); pos += n; continue; }
                if (status == 0xf0 || status == 0xf7) {
                    pos++;
                    uint32_t n = vlq(pos);
                    if (status == 0xf0) out.push_back(0xf0);
                    out.insert(out.end(), b.begin() + pos, b.begin() + std::min(b.size(), pos + n));
                    pos += n;
                    continue;
                }
                if (status & 0x80) { running = status; pos++; }
                pos += (running & 0xf0) == 0xc0 || (running & 0xf0) == 0xd0 ? 1 : 2;
            }
            pos = end;
        }
    } else {
        out = b;
    }
    if (std::count(out.begin(), out.end(), (uint8_t)0xf0) == 0) return "no system exclusive in the file";
    return "";
}

size_t Engine::sysex_sent(std::vector<uint8_t>* take) {
    std::lock_guard<std::mutex> hold(impl->out_lock);
    size_t n = impl->sysex_out.size();
    if (take) { take->swap(impl->sysex_out); impl->sysex_out.clear(); }
    return n;
}

int Engine::sysex_waiting() const { return impl->sysex_left.load(std::memory_order_relaxed); }

long Engine::flash_writes() const { return impl->flash_count.load(std::memory_order_relaxed); }

Engine::~Engine() {
    if (getenv("OB12_CLOCK_LOG")) printf("clock: %.6f s of sound, Master %.6f s, Slave %.6f s at 16 MHz, ready at %.4f s\n", impl->k / 96000.0, impl->m->cycles_total / 16e6, impl->s->cycles_total / 16e6, impl->ready_at / 96000.0);
    if (getenv("OB12_TIME_LOG")) {
        // us per sample of sound, taking the counter at its nominal rate (the first 2 s calibrate it)
        static double rate = 0;
        if (!rate) { uint64_t a = __rdtsc(); Sleep(200); rate = (__rdtsc() - a) / 0.2; }
        double samples = impl->batch > impl->time_from && impl->time_from > 48000 ? (double)(impl->batch - impl->time_from) : impl->batch ? (double)impl->batch : 1;
        auto us = [&](uint64_t ticks) { return ticks / rate * 1e6 / samples; };
        printf("us a sample: CPUs (both slices, with the Master wait if it has a thread) %.2f, wait for chips %.2f, chips", us(impl->spent_cpu.ticks), us(impl->spent_wait.ticks));
        for (auto& c : impl->spent_chip) printf(" %.2f", us(c.ticks));
        printf("; of the CPUs: Master %.2f, Slave %.2f", us(impl->spent_master.ticks), us(impl->spent_slave.ticks));
        puts("");
    }
    impl->stop = true;
    for (auto& t : impl->workers) t.join();
    if (impl->flash_dirty) impl->save_flash();
    delete impl->m;
    delete impl->s;
    for (auto& h : impl->host) delete h.dsp;
    delete impl;
}

void Engine::run(int32_t* frames, int n) {
    Impl& e = *impl;
    e.out = frames;
    e.made = 0;
    while (e.made < n) e.next_batch();
}

void Engine::midi(const uint8_t* bytes, int count) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->midi_bytes.insert(impl->midi_bytes.end(), bytes, bytes + count);
    impl->input_waiting = true;
}

void Engine::set_pot(int channel, int mux, int value) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->pot_events.push_back({channel, mux, value});
    impl->input_waiting = true;
}

void Engine::key(int bar, int bit, bool down) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->key_events.push_back({bar, bit, down});
    impl->input_waiting = true;
}

void Engine::turn(int detents) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->turns += detents;
    impl->input_waiting = true;
}

bool Engine::sounding_ready() const { return impl->ready.load(std::memory_order_relaxed); }

void Engine::leds(uint8_t* latches) const {
    std::lock_guard<std::mutex> hold(impl->lcd_lock);
    memcpy(latches, impl->led_copy, sizeof impl->led_copy);
}

bool Engine::lcd(uint8_t* bitmap) const {
    std::lock_guard<std::mutex> hold(impl->lcd_lock);
    if (!impl->lcd_ready) return false;
    memcpy(bitmap, impl->lcd_copy, sizeof impl->lcd_copy);
    return true;
}
