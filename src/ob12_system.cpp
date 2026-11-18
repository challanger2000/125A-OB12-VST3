// Whole instrument: Master and Slave CPUs linked over UART2, five TMS57070 cores driven by
// the Slave's host bytes, DSP chain wired per service manual sheets 09/15 and 10/15.
//
// usage: ob12_system <rom dir> <seconds> <note seconds> <out.wav>
//   OB12_DIRECT=n    send one note per channel 0..n-1 straight to the Slave, 1 s apart
//   OB12_NOTE_CH=c   send one note on channel c straight to the Slave. It breaks running
//                    status: the Master's next data bytes then read as notes. Use for study only
//   OB12_MIDI_EVENTS=t:on:nn,t:off:nn,t:pc:nn  timed note on, off, program change, hex, channel 0, to the Master
//   OB12_MIDI_NOTES=3C,40  send these notes on MIDI channel 0 to the Master, 0.5 s apart
//   OB12_VOICE_TRACE=chip,first,last  print one sample of a chip for a PC range, hex
//   OB12_MIDI=1      send the note to the Master's MIDI input instead
//   OB12_HOP=1|2     half frames a word takes from one chip to the next (default 2)
//   OB12_WATCH_C=a   log the first changes of DASP0 CMEM cell a, hex, with their writer
//   OB12_WATCH_D=a,b count which PCs change DASP0 DMEM cells after the note, hex
//   OB12_PHASE=0|1 start DASP4 on the even or odd half frame (default 0)
//   OB12_NO_1FC=1    disable the 1FC divide step
//   OB12_NO_NOISE=1  disable the noise secondary, 0x34
//   OB12_EARLY_MODE=1  apply secondaries 28-2B before the primary, as the public emulator does
//   OB12_POT_SWEEP=t move all pots from t seconds on
//   OB12_FORCE_GAIN=1  old diagnostic, not needed: forces DASP4 cells C00D, C00F, C0CB
//   OB12_SLAVE_SEQ=t,step,status,value  send status n value to the Slave for n = 0..127, hex
//   OB12_SLAVE_FILE=f  lines of seconds and hex bytes, sent straight to the Slave
//   OB12_AUX_TONE=1 or amplitude  put a 440 Hz tone (default amplitude 1000000) on the even bus slot into DASP4 from the note time
//   OB12_AOUT_CELLS=l,r  study: force the AOUT pointers C07F, C080, hex
//   OB12_SLAVE_TICK=n  Slave PPG tick every n half frames, default 96 = 1 ms, 0 turns it off
//   OB12_KEYS=from:to:row:mask,...  hold panel keys, row and mask hex, active low
//   OB12_ENC=t:detents,...  turn the data encoder
//   OB12_SERIAL=1    run on one thread; diagnostics that look inside a step do this anyway
//   OB12_LIVE=t      from t seconds on play MOUT on the sound card, paced by it
//   OB12_LIVE_SILENT=1  same timing, zeros to the device
//   OB12_LIVE_MS=n   buffer length in ms, default 10; OB12_LIVE_BUFFERS=n, default 6
//   OB12_LIVE_MIDI=d  take notes from MIDI input device d; OB12_LIVE_MIDI=list prints them
//   OB12_NO_RELOAD=1  no reload timer 1: dry sound, glide only
//   OB12_HOST_SINGLE=1  host update words take effect one by one, not together when the select ends
//   OB12_XREAD_CANCEL=1  a new XMEM read cancels the pending one (old model)
//   OB12_XMEM_CR3=1  DASP4 XMEM size from CR3 (32K words) instead of the board's 128K
//   OB12_TRACE_MAC=1  with OB12_CREADS: both MACs and CR1 after each word instead of the XMEM columns
//   OB12_XREAD_CYCLES=n  words from a DASP4 XMEM read to its data in XRD (default 5; 0 = the old table, 9)
//   OB12_EARLY_2D=1  secondary 2D before the primary: all LFOs stand still
//   OB12_MACLO=1     primary 43 as low MAC word multiply, the old model
//   OB12_POT_REST=v  value every pot reads when not moved, hex
//   OB12_POT_SWEEP_TO=t  pots stop moving at t seconds and stay there
//   OB12_POT_STEPS=t:channel:mux:value,...  one pot jumps at t seconds, value hex
//   OB12_HOST_LOG=file  log Master to Slave bytes and host CMEM updates with their time
//   OB12_PORT_LOG=1  write all DASP4 output ports per half frame to build/ports.raw
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include "tms57070/tms57070.h"
#include "live_io.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <thread>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>

#define OB12_NO_MAIN
namespace master {
#include "ob12_master.cpp"
}
namespace slave {
#include "ob12_slave.cpp"
}

// A spinning gate: the caller opens it, every worker does its part once, the caller waits.
struct Gate {
    alignas(64) std::atomic<long> go{0};
    alignas(64) std::atomic<int> left{0};
    void open(int workers) { left.store(workers, std::memory_order_relaxed); go.fetch_add(1, std::memory_order_release); }
    void wait() { while (left.load(std::memory_order_acquire)) _mm_pause(); }
    void done() { left.fetch_sub(1, std::memory_order_release); }
};

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    tms::Core* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false, start_pending = false;
    long updates = 0, programs = 0, unknown = 0;
    FILE* log = nullptr;
    int chip = 0;
    const double* clock = nullptr;

    // An update transaction takes effect when the Slave releases the chip select, all its words at
    // once (same as the engine; see "Host bursts" in docs/HARDWARE.md). OB12_HOST_SINGLE=1: one by one.
    bool burst = getenv("OB12_HOST_SINGLE") == nullptr;
    std::vector<std::pair<int, int32_t>> pending, ready;

    // A released burst goes in at the start of the chip's next sample, so no sample reads half of it.
    void sample_start() {
        for (auto& w : ready) commit(w.first, w.second);
        ready.clear();
    }

    void commit(int address, int32_t value) {
        dsp->cmem[address] = value;
        updates++;
        if (log) fprintf(log, "%.6f W %d C%03X %06X\n", *clock, chip, address, value & 0xffffff);
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
            else if (data != 0x00) unknown++;
            break;
        case Program:
            buffer.push_back(data);
            if (buffer.size() == 12 + 2048) {
                const u8* h = buffer.data();
                running = false;
                dsp->reset();  // keeps DCIRC, DIR and CIR set by the start-up program
                dsp->load_control(h);
                for (int i = 0; i < 512; i++) {
                    const u8* w = h + 12 + i * 4;
                    dsp->pmem[i] = w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3];
                }
                programs++;
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
                if (burst) pending.push_back({address, value}); else commit(address, value);
                buffer.clear();
            }
            break;
        }
    }
};

static void write_wav(const std::string& path, const std::vector<int16_t>& samples) {
    std::ofstream f(path, std::ios::binary);
    auto u32le = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16le = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t bytes = (uint32_t)samples.size() * 2;
    f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
    u32le(48000); u32le(48000 * 4); u16le(4); u16le(16);
    f.write("data", 4); u32le(bytes);
    f.write((const char*)samples.data(), bytes);
}

// DMEM by program address, for the true ring.
static int32_t logical_d(const tms::Core* d, int a) {
    return d->dmem[a <= d->dcirc ? (a + d->doff) % (d->dcirc + 1) : a];
}

// One sample of one chip, with the machine state after each word in a PC range.
static void voice_trace(tms::Core* d, int first, int last) {
    int samples = getenv("OB12_TRACE_SAMPLES") ? atoi(getenv("OB12_TRACE_SAMPLES")) : 1;
    for (int i = 0; i < 512 * samples; i++) {
        int pc = d->pc;
        uint32_t w = d->pmem[pc];
        d->step();
        if (pc >= first && pc <= last)
            printf("%03X %08X A %06X %06X M %06X %06X DA %03X %03X CA %03X %03X D00 %06X D02 %06X D42 %06X %06X %06X DB6 %06X d2 %06X %06X CR1 %06X lo %06X\n", pc, w, d->acc[0] & 0xffffff, d->acc[1] & 0xffffff,
                   d->mac_upper(d->mac[0]) & 0xffffff, d->mac_upper(d->mac[1]) & 0xffffff, d->da[0], d->da[1], d->ca[0], d->ca[1], logical_d(d, 0) & 0xffffff, logical_d(d, 2) & 0xffffff, logical_d(d, 0x42) & 0xffffff, logical_d(d, 0x43) & 0xffffff, logical_d(d, 0x44) & 0xffffff, logical_d(d, 0xb6) & 0xffffff, d->mac_upper(d->mac_d2[0]) & 0xffffff, d->mac_upper(d->mac_d2[1]) & 0xffffff, d->cr[1], d->mac_lower(d->mac[1]));
    }
}

int main(int argc, char** argv) {
    if (argc < 5) { puts("usage: ob12_system <rom dir> <seconds> <note seconds> <out.wav>"); return 2; }
    std::string rom = argv[1];
    double seconds = atof(argv[2]), note_at = atof(argv[3]);
    std::string wav = argv[4];
    int direct = getenv("OB12_DIRECT") ? atoi(getenv("OB12_DIRECT")) : 0;
    bool midi = getenv("OB12_MIDI") != nullptr;
    int phase = getenv("OB12_PHASE") ? atoi(getenv("OB12_PHASE")) : 0;
    bool force_gain = getenv("OB12_FORCE_GAIN") != nullptr;

    tms::Options options;
    options.audio_port_writes = true;
    options.true_dmem_ring = true;
    options.lock_delay = true;
    options.maclo_multiply = true;
    options.divide_1fc = getenv("OB12_NO_1FC") == nullptr;
    options.noise_generator = getenv("OB12_NO_NOISE") == nullptr;
    options.late_mode_change = getenv("OB12_EARLY_MODE") == nullptr;
    if (const char* m = getenv("OB12_SHIFT_PRODUCT")) options.shift_with_product = atoi(m);  // study
    if (const char* m = getenv("OB12_SHIFT_PATHS")) options.shift_paths = atoi(m);  // study
    if (const char* m = getenv("OB12_LATE_MASK")) options.late_mode_mask = (unsigned)strtol(m, nullptr, 16);  // study
    options.compare_3e = getenv("OB12_NO_3E") == nullptr;
    options.bus_sum_saturates = getenv("OB12_BUS_WRAP") == nullptr;
    options.reuse_coefficient = getenv("OB12_MACLO") == nullptr;
    options.late_saturation_mode = getenv("OB12_EARLY_2D") == nullptr;
    options.xread_flush = getenv("OB12_XREAD_CANCEL") == nullptr;
    options.maclo_shift = getenv("OB12_MACLO_NO_SHIFT") == nullptr;
    options.maclo_unsigned = getenv("OB12_MACLO_SIGNED") == nullptr;
    options.queued_pointer_load = getenv("OB12_EARLY_POINTER") == nullptr;

    auto* m = new master::Master();
    if (getenv("OB12_POT_SWEEP")) m->pot_sweep_from = atof(getenv("OB12_POT_SWEEP"));
    if (getenv("OB12_POT_REST")) m->pot_rest = (u16)strtol(getenv("OB12_POT_REST"), nullptr, 16);
    if (const char* list = getenv("OB12_KEYS")) {  // from:to:row:mask,... times in seconds, hex row and mask
        double from, to; int row, mask, used;
        while (sscanf(list, "%lf:%lf:%x:%x%n", &from, &to, &row, &mask, &used) == 4) {
            m->key_steps.push_back({from, to, row, (u8)mask});
            list += used;
            if (*list == ',') list++;
        }
    }
    if (getenv("OB12_POT_SWEEP_TO")) m->pot_sweep_to = atof(getenv("OB12_POT_SWEEP_TO"));
    if (const char* list = getenv("OB12_POT_STEPS")) {
        double at; int channel, mux, value, used;
        while (sscanf(list, "%lf:%d:%d:%x%n", &at, &channel, &mux, &value, &used) == 4) {
            m->pot_steps.push_back({at, channel, mux, (u16)value});
            list += used;
            if (*list == ',') list++;
        }
    }
    auto* s = new slave::Slave();
    m->load(rom);
    s->load(rom);
    m->attach(m); s->attach(s);
    m->reset(); s->reset();

    Host host[5];
    // DASP4 has 3 x 44C256 DRAM, 256K x 12 bits: 128K words of 24 bits in two accesses.
    tms::Options options4 = options;
    if (!getenv("OB12_XMEM_CR3")) options4.xmem_words = 0x20000;
    // An XMEM read's data is in XRD 5 words later, see "XMEM read time" in docs/HARDWARE.md.
    // OB12_XREAD_CYCLES=n tries another time; 0 is the old table (9 words here).
    options4.xread_cycles = getenv("OB12_XREAD_CYCLES") ? atoi(getenv("OB12_XREAD_CYCLES")) : 5;
    for (int n = 0; n < 5; n++) host[n].dsp = new tms::Core(n == 4 ? options4 : options);
    if (getenv("OB12_SHIFT_PRODUCT") || getenv("OB12_SHIFT_MISMATCH") || getenv("OB12_SHIFT_PCS"))
        for (auto& h : host) h.dsp->study_shift = true;  // the shift studies, off in the engine
    // study: OB12_SHIFT_PCS=chip:pc,pc,... product-time shift (OB12_SHIFT_PRODUCT) only for reads there
    if (const char* t = getenv("OB12_SHIFT_PCS")) {
        int chip = atoi(t);
        for (int n = 0; n < 5; n++) host[n].dsp->shift_pcs.push_back(-1);  // a chip not named keeps the old rule
        const char* c = strchr(t, ':');
        while (c && *++c) {  // chip 9: the four voice chips
            int at = (int)strtol(c, (char**)&c, 16);
            for (int n = 0; n < 5; n++) if (n == chip || (chip == 9 && n < 4)) host[n].dsp->shift_pcs.push_back(at);
            if (*c != ',') break;
        }
    }
    double clock = 0;
    FILE* host_log = getenv("OB12_HOST_LOG") ? fopen(getenv("OB12_HOST_LOG"), "w") : nullptr;
    FILE* port_log = getenv("OB12_PORT_LOG") ? fopen("build/ports.raw", "wb") : nullptr;
    FILE* chip_log = getenv("OB12_CHIP_LOG") ? fopen("build/chips.raw", "wb") : nullptr;
    FILE* cell_probe = getenv("OB12_PORT_LOG") && getenv("OB12_AUX_TONE") ? fopen("build/cells4.raw", "wb") : nullptr;
    m->text_log = host_log;
    for (int n = 0; n < 5; n++) { host[n].log = host_log; host[n].chip = n; host[n].clock = &clock; }
    // While the DSPs run the next half frame's CPU slice is already going, so host bytes wait here.
    // They reach the chips before those chips' next run, the same order as one thread would give.
    bool hold_bytes = false;
    std::vector<std::pair<u8, u8>> held;
    auto host_byte = [&](u8 selected, u8 data) {
        for (int chip = 0; chip < 5; chip++) if (selected & (1 << chip)) host[chip].byte(data);
    };
    if (const char* path = getenv("OB12_DSP_BYTE_LOG")) s->byte_log = fopen(path, "w");  // study: Slave PC per DSP byte
    s->on_dsp_byte = [&](u8 selected, u8 data) {
        if (hold_bytes) held.push_back({selected, data}); else host_byte(selected, data);
    };
    s->on_dsp_select = [&]() {
        if (host_log) fprintf(host_log, "%.6f SEL\n", clock);  // study: chip select edges between host words
        if (hold_bytes) held.push_back({0x80, 0}); else for (auto& h : host) h.deselect();
    };

    size_t m_sent = 0, s_sent = 0;
    std::vector<int16_t> out;
    std::vector<int32_t> bus;
    double sum[4] = {0, 0, 0, 0}, chip_sum[5] = {0, 0, 0, 0, 0};
    long count = 0;
    long total = (long)(seconds * 96000);
    int sent_notes = 0;
    int midi_channel = 0;
    int note_channel = getenv("OB12_NOTE_CH") ? atoi(getenv("OB12_NOTE_CH")) : -1;
    bool note_sent = false;
    std::vector<u8> midi_notes;
    size_t next_midi = 0;
    if (const char* list = getenv("OB12_MIDI_NOTES")) {  // hex, comma separated
        for (char* p = (char*)list; *p;) {
            midi_notes.push_back((u8)strtol(p, &p, 16));
            if (*p == ',') p++;
        }
    }
    struct TimedMidi { double at; u8 status, note, velocity; };
    std::vector<TimedMidi> midi_events;  // OB12_MIDI_EVENTS=t:on:nn,t:off:nn,...  note hex, into the Master
    size_t next_event = 0;
    double creads_at = getenv("OB12_CREADS") ? atof(getenv("OB12_CREADS")) : 0;  // study: list DASP4 multiply operands once at t
    bool creads_done = false;
    bool creads_on = false, trace_all = getenv("OB12_TRACE4_ALL") != nullptr;
    int trace_chip = getenv("OB12_TRACE_CHIP") ? atoi(getenv("OB12_TRACE_CHIP")) : 4;  // which DSP OB12_CREADS traces
    // study: OB12_SLAVE_SYSEX=t:F07E04...F7 sends these hex bytes straight to the Slave at t
    double slave_sysex_at = -1;
    std::vector<u8> slave_sysex;
    if (const char* text = getenv("OB12_SLAVE_SYSEX")) {
        const char* colon = strchr(text, ':');
        if (colon) {
            slave_sysex_at = atof(text);
            for (const char* p = colon + 1; p[0] && p[1]; p += 2) slave_sysex.push_back((u8)strtol(std::string(p, 2).c_str(), nullptr, 16));
        }
    }
    struct Poke { double at; int chip, address; int32_t value; };
    std::vector<Poke> pokes;  // study: OB12_POKE=t:chip:cmem:value,... hex address and value, held from t on
    if (const char* list = getenv("OB12_POKE")) {
        double at; int chip, address, used; unsigned value;
        while (sscanf(list, "%lf:%d:%x:%x%n", &at, &chip, &address, &value, &used) == 4) {
            pokes.push_back({at, chip, address, (int32_t)(value << 8) >> 8});
            list += used;
            if (*list == ',') list++;
        }
    }
    if (const char* list = getenv("OB12_MIDI_EVENTS")) {
        double at; char kind[4]; int note, used;
        while (sscanf(list, "%lf:%3[a-z]:%x%n", &at, kind, &note, &used) == 3) {
            u8 status = (u8)(kind[0] == 'p' ? 0xC0 : kind[1] == 'n' ? 0x90 : 0x80);  // t:pc:nn program change
            list += used;
            int velocity = status == 0x90 ? 0x64 : 0x40, more = 0;  // t:on:nn:vv, velocity hex
            if (*list == ':' && sscanf(list, ":%x%n", &velocity, &more) == 1) list += more;
            midi_events.push_back({at, status, (u8)note, (u8)velocity});
            if (*list == ',') list++;
        }
    }
    int32_t d04_peak[5] = {};
    // Study: send status, n, value straight to the Slave for n = 0..127.
    int seq_status = 0, seq_value = 0, seq_next = 0;
    double seq_from = 0, seq_step = 0;
    if (const char* text = getenv("OB12_SLAVE_SEQ")) sscanf(text, "%lf,%lf,%x,%x", &seq_from, &seq_step, &seq_status, &seq_value);
    // Study: a file of lines "seconds hex hex ...", sent straight to the Slave.
    std::vector<std::pair<double, std::vector<u8>>> script;
    size_t next_script = 0;
    if (const char* name = getenv("OB12_SLAVE_FILE")) {
        std::ifstream file(name);
        std::string line;
        while (std::getline(file, line)) {
            char* cursor = nullptr;
            double at = strtod(line.c_str(), &cursor);
            std::vector<u8> bytes;
            for (;;) {
                char* end = nullptr;
                long value = strtol(cursor, &end, 16);
                if (end == cursor) break;
                bytes.push_back((u8)value);
                cursor = end;
            }
            script.push_back({at, bytes});
        }
    }
    std::vector<std::pair<int, int>> force_cells;  // study: OB12_FORCE_C=addr:value,... on DASP4, hex
    if (const char* list = getenv("OB12_FORCE_C")) {
        int a, v, used;
        while (sscanf(list, "%x:%x%n", &a, &v, &used) == 2) {
            force_cells.push_back({a, v});
            list += used;
            if (*list == ',') list++;
        }
    }
    bool aux_tone = getenv("OB12_AUX_TONE") != nullptr;
    double aux_amp = aux_tone && atof(getenv("OB12_AUX_TONE")) > 1 ? atof(getenv("OB12_AUX_TONE")) : 1000000;  // OB12_AUX_TONE=amplitude, default 1000000
    int aout_left = -1, aout_right = -1;  // study: point AOUT at other DMEM cells
    if (const char* text = getenv("OB12_AOUT_CELLS")) sscanf(text, "%x,%x", &aout_left, &aout_right);
    // Study: OB12_SLAVE_WIN=from,to,from,to writes Slave PCs and RAM accesses to build/slave_N.txt.
    struct SlaveWindow { double from, to; FILE* file; };
    std::vector<SlaveWindow> slave_windows;
    if (const char* list = getenv("OB12_SLAVE_WIN")) {
        double a, b; int used;
        while (sscanf(list, "%lf,%lf%n", &a, &b, &used) == 2) {
            char name[64];
            snprintf(name, sizeof name, "build/slave_%zu.txt", slave_windows.size());
            slave_windows.push_back({a, b, fopen(name, "w")});
            list += used;
            if (*list == ',') list++;
        }
    }
    if (const char* text = getenv("OB12_SLAVE_WATCH")) sscanf(text, "%x,%x", &s->watch_lo, &s->watch_hi);
    if (const char* text = getenv("OB12_MASTER_WATCH")) sscanf(text, "%x,%x", &m->mwatch_lo, &m->mwatch_hi);
    if (const char* text = getenv("OB12_MASTER_WATCH_PC")) sscanf(text, "%x,%x", &m->mwatch_pc_lo, &m->mwatch_pc_hi);
    if (const char* text = getenv("OB12_SLAVE_WATCH_FROM")) s->watch_from = (s64)(atof(text) * 16e6);
    bool full_reload = getenv("OB12_NO_RELOAD") == nullptr;  // timer 1 starts the effects; off leaves glide only
    int slave_tick = getenv("OB12_SLAVE_TICK") ? atoi(getenv("OB12_SLAVE_TICK")) : 96;
    std::vector<int> cell_pick;  // study: OB12_CELL_PICK=10,42 logs only these DMEM cells
    if (const char* list = getenv("OB12_CELL_PICK")) for (const char* c = list; c; c = strchr(c, ','), c = c ? c + 1 : c) cell_pick.push_back((int)strtol(c, nullptr, 16));
    // Study: OB12_ENC=t:detents,... turns the data encoder, negative the other way, 16 ms per detent.
    struct EncMove { double at; int detents; };
    std::vector<EncMove> enc_moves;
    if (const char* list = getenv("OB12_ENC")) {
        double at; int detents, used;
        while (sscanf(list, "%lf:%d%n", &at, &detents, &used) == 2) {
            enc_moves.push_back({at, detents});
            list += used;
            if (*list == ',') list++;
        }
    }
    int hop = getenv("OB12_HOP") ? atoi(getenv("OB12_HOP")) : 2;
    static int32_t wire[4][5][3][2];
    int watch = getenv("OB12_WATCH_C") ? strtol(getenv("OB12_WATCH_C"), nullptr, 16) : 0;
    int32_t watch_value = 0;
    int watch_log = getenv("OB12_WATCH_C") ? 0 : 24;
    int start_trace = getenv("OB12_START_TRACE") ? atoi(getenv("OB12_START_TRACE")) : 0;
    struct DWatch { int32_t last = 0, peak = 0; std::map<int, std::pair<long, int32_t>> writers; };
    std::map<int, DWatch> watch_d;
    if (const char* list = getenv("OB12_WATCH_D")) {  // hex, comma separated
        for (char* p = (char*)list; *p;) {
            watch_d[(int)strtol(p, &p, 16)];
            if (*p == ',') p++;
        }
    }
    // study: OB12_PC_RMS=chip,from,to (seconds): per program address, the rms of the D cell each
    // instruction addresses, after it ran, and of both accumulators. Printed at the end.
    int pcr_chip = -1; double pcr_from = 0, pcr_to = 0;
    static double pcr_d[512], pcr_a0[512], pcr_a1[512], pcr_ax[512]; static long pcr_n[512];
    if (const char* t = getenv("OB12_PC_RMS")) sscanf(t, "%d,%lf,%lf", &pcr_chip, &pcr_from, &pcr_to);
    FILE* cell_log = nullptr;  // per sample: DMEM 256 and CMEM 512 words of one chip
    double cell_from = 0, cell_to = 0;
    int cell_chip = 0;
    long cell_count = 0, cell_every = getenv("OB12_CELL_EVERY") ? atol(getenv("OB12_CELL_EVERY")) : 1;  // study: every nth frame
    if (const char* range = getenv("OB12_CELL_LOG")) {  // chip,from,to in seconds after the note
        if (sscanf(range, "%d,%lf,%lf", &cell_chip, &cell_from, &cell_to) == 3) cell_log = fopen("build/cells.bin", "wb");
    }
    static double port_sum[5][3][2][2];  // chip, AX1-3, left/right, half frame
    long port_count[2] = {};
    try {
        int slice = 167;  // set per half frame below, 16 MHz
        bool threaded = !getenv("OB12_SERIAL") && !cell_log && !start_trace && watch_d.empty() && !getenv("OB12_WATCH_C") && !getenv("OB12_SLAVE_WIN") && !getenv("OB12_CREADS") && pcr_chip < 0;
        long long chip_tsc[5] = {};
        Gate cpu_gate, dsp_gate;
        std::atomic<bool> stop_workers{false};
        std::string worker_error;
        std::vector<std::thread> workers;
        if (threaded) {
            m->lean = s->lean = true;
            hold_bytes = true;
            workers.emplace_back([&]() {
                long seen = 0;
                for (;;) {
                    while (cpu_gate.go.load(std::memory_order_acquire) == seen) { if (stop_workers) return; _mm_pause(); }
                    seen++;
                    try { m->run(slice); m->tick(m->executed()); }
                    catch (const std::exception& error) { worker_error = error.what(); }
                    cpu_gate.done();
                }
            });
            for (int n = 0; n < 5; n++) workers.emplace_back([&, n]() {
                long seen = 0;
                for (;;) {
                    while (dsp_gate.go.load(std::memory_order_acquire) == seen) { if (stop_workers) return; _mm_pause(); }
                    seen++;
                    long long t0 = __rdtsc();
                    if (host[n].running) for (int i = 0; i < 256; i++) host[n].dsp->step();
                    chip_tsc[n] += __rdtsc() - t0;
                    dsp_gate.done();
                }
            });
        }
        struct Joiner { std::atomic<bool>& stop; std::vector<std::thread>& list; ~Joiner() { stop = true; for (auto& t : list) t.join(); } } joiner{stop_workers, workers};
        bool live_on = getenv("OB12_LIVE") != nullptr;
        double live_from = live_on ? atof(getenv("OB12_LIVE")) : 0;
        int live_ms = getenv("OB12_LIVE_MS") ? atoi(getenv("OB12_LIVE_MS")) : 10;
        int live_buffers = getenv("OB12_LIVE_BUFFERS") ? atoi(getenv("OB12_LIVE_BUFFERS")) : 6;
        std::vector<int16_t> live_block(live_ms * 48 * 2);
        int live_fill = 0;
        bool live_midi = false;
        if (live_on) {
            if (!live_audio_open(live_buffers, live_ms * 48, getenv("OB12_LIVE_SILENT") != nullptr)) throw std::runtime_error("no sound device");
            printf("live: %d x %d ms buffers\n", live_buffers, live_ms);
            if (const char* device = getenv("OB12_LIVE_MIDI")) {
                if (!strcmp(device, "list")) { for (int d = 0; d < live_midi_devices(); d++) printf("MIDI in %d: %s\n", d, live_midi_name(d)); }
                else {
                    live_midi = live_midi_open(atoi(device));
                    printf("MIDI in %s: %s\n", device, live_midi ? live_midi_name(atoi(device)) : "failed");
                }
            }
            fflush(stdout);
        }
        auto post = [&](long k, double now) {
            if (now >= note_at + 0.2) {
                for (int n = 0; n < 5; n++) for (int p = 0; p < 3; p++) for (int c = 0; c < 2; c++) {
                    double v = host[n].dsp->ax[p][c];
                    port_sum[n][p][c][k & 1] += v * v;
                }
                port_count[k & 1]++;
            }
            if (k & 1) {
                auto* d = host[4].dsp;
                int32_t v[4] = {d->ax[1][0], d->ax[1][1], d->ax[2][0], d->ax[2][1]};
                for (int c = 0; c < 4; c++) sum[c] += (double)v[c] * v[c];
                for (int n = 0; n < 5; n++) {
                    int32_t v04 = abs(logical_d(host[n].dsp, 4));
                    if (v04 > d04_peak[n]) d04_peak[n] = v04;
                    double l = host[n].dsp->ax[1][0], r = host[n].dsp->ax[1][1];
                    chip_sum[n] += l * l + r * r;
                }
                if (live_on) {
                    if (k / 96000.0 >= live_from) {
                        int a = v[0] >> 8, b = v[1] >> 8;
                        live_block[live_fill++] = (int16_t)a;
                        live_block[live_fill++] = (int16_t)b;
                        if (live_fill == (int)live_block.size()) { live_audio_write(live_block.data()); live_fill = 0; }
                    }
                } else {
                    out.push_back((int16_t)(v[0] >> 8));
                    out.push_back((int16_t)(v[1] >> 8));
                }
                count++;
            }
            if (port_log) {
                int32_t v[6];
                for (int p = 0; p < 3; p++) for (int c = 0; c < 2; c++) v[p * 2 + c] = host[4].dsp->ax[p][c];
                fwrite(v, 4, 6, port_log);
                if (chip_log) { int32_t c[30]; for (int n = 0; n < 5; n++) for (int q = 0; q < 3; q++) for (int ch = 0; ch < 2; ch++) c[n * 6 + q * 2 + ch] = host[n].dsp->ax[q][ch]; fwrite(c, 4, 30, chip_log); }  // chip, AX1-3, L/R
                if (cell_probe) { int32_t c[6]; int list[6] = {0x0b, 0x0f, 0xd0, 0xd1, 0x08, 0x17}; for (int i = 0; i < 6; i++) c[i] = logical_d(host[4].dsp, list[i]); fwrite(c, 4, 6, cell_probe); }
            }
            if (!live_on) {
                bus.push_back(host[4].dsp->ar[0][0]);
                bus.push_back(host[4].dsp->ar[0][1]);
            }
            if (k % 96000 == 95999) {
                printf("t=%.0f M>S %zu S>M %zu host", (k + 1) / 96000.0, m_sent, s_sent);
                for (auto& h : host) printf(" %ld/%ld%s", h.programs, h.updates, h.running ? "r" : "");
                printf("; AX2 rms");
                for (int n = 0; n < 5; n++) { printf(" %.0f", count ? sqrt(chip_sum[n] / count / 2) : 0); chip_sum[n] = 0; }
                printf("; MOUT %.0f %.0f AOUT %.0f %.0f\n", sqrt(sum[0] / count), sqrt(sum[1] / count), sqrt(sum[2] / count), sqrt(sum[3] / count));
                fflush(stdout);
                sum[0] = sum[1] = sum[2] = sum[3] = 0;
                count = 0;
            }
        };
        long long spent[4] = {};  // cycles: Slave, wait for Master, wait for DSPs, post
        long long tsc_start = __rdtsc();
        auto wall_start = std::chrono::steady_clock::now();
        for (long k = 0; k < total; k++) {
            slice = k % 3 == 2 ? 166 : 167;
            double now = k / 96000.0;
            clock = now;
            for (auto& e : enc_moves) if (e.detents && now >= e.at) {
                int phase = (int)((now - e.at) / 0.004);
                static const int a_first[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
                int i = phase & 3;
                if (e.detents > 0) m->enc_set(a_first[i][0], a_first[i][1]);
                else m->enc_set(a_first[i][1], a_first[i][0]);
                if (phase >= 4) { e.detents += e.detents > 0 ? -1 : 1; e.at = now; }
                break;
            }
            m->pot_clock = now;
            if (slave_tick && k % slave_tick == 0) { s->ppg_tick(); if (full_reload) s->reload_tick(); else if (k % (2 * slave_tick) == 0) s->glide_tick(); }
            if (live_midi && k % 96 == 0) {
                uint8_t bytes[64];
                int n = live_midi_read(bytes, sizeof bytes);
                if (n) m->midi_in(std::vector<u8>(bytes, bytes + n));
            }
            for (auto& w : slave_windows) {
                if (now >= w.from && now < w.to && !s->trace_file) s->trace_file = w.file;
                else if (now >= w.to && s->trace_file == w.file) s->trace_file = nullptr;
            }
            // One note per MIDI channel, half a second apart, into the Master's MIDI input.
            if (midi && midi_channel < 16 && now >= note_at + midi_channel * 0.5) {
                m->midi_in({(u8)(0x90 | midi_channel), 0x3c, 0x64});
                midi_channel++;
            }
            for (auto& p : pokes) if (now >= p.at) host[p.chip].dsp->cmem[p.address & 0x1ff] = p.value;
            if (slave_sysex_at >= 0 && now >= slave_sysex_at) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), slave_sysex.begin(), slave_sysex.end());
                slave_sysex_at = -1;
            }
            while (next_event < midi_events.size() && now >= midi_events[next_event].at) {
                auto& ev = midi_events[next_event++];
                if (ev.status == 0xC0) m->midi_in({ev.status, ev.note});
                else m->midi_in({ev.status, ev.note, ev.velocity});
            }
            if (next_midi < midi_notes.size() && now >= note_at + next_midi * 0.5) {
                m->midi_in({0x90, midi_notes[next_midi], 0x64});
                next_midi++;
            }
            if (note_channel >= 0 && !note_sent && now >= note_at) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), {(u8)(0x90 | note_channel), 0x3c, 0x64});
                note_sent = true;
            }
            while (next_script < script.size() && now >= script[next_script].first) {
                auto& bytes = script[next_script++].second;
                s->link().rx_queue.insert(s->link().rx_queue.end(), bytes.begin(), bytes.end());
            }
            if (seq_status && seq_next < 128 && now >= seq_from + seq_next * seq_step) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), {(u8)seq_status, (u8)seq_next, (u8)seq_value});
                seq_next++;
            }
            if (sent_notes < direct && now >= note_at + sent_notes) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), {(u8)(0x90 | sent_notes), 0x3c, 0x64});
                sent_notes++;
            }
            if (threaded) {
                long long t0 = __rdtsc();
                cpu_gate.open(1);
                s->run(slice); s->tick(s->executed());
                long long t1 = __rdtsc();
                cpu_gate.wait();
                long long t2 = __rdtsc();
                spent[0] += t1 - t0;
                spent[1] += t2 - t1;
                if (!worker_error.empty()) throw std::runtime_error(worker_error);
                if (k > 0) {
                    long long t3 = __rdtsc();
                    dsp_gate.wait();
                    long long t4 = __rdtsc();
                    post(k - 1, (k - 1) / 96000.0);
                    spent[2] += t4 - t3;
                    spent[3] += __rdtsc() - t4;
                }
                for (auto& b : held) {
                    if (b.first == 0x80) { for (auto& h : host) h.deselect(); }
                    else host_byte(b.first, b.second);
                }
                held.clear();
            } else {
                m->run(slice); m->tick(m->executed());
                s->run(slice); s->tick(s->executed());
            }
            auto& mu = m->link();
            auto& su = s->link();
            for (; m_sent < mu.head.size(); m_sent++) {
                su.rx_queue.push_back(mu.head[m_sent]);
                if (host_log) fprintf(host_log, "%.4f M %02X\n", now, mu.head[m_sent]);
            }
            for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);

            // Snapshot the serial links, so every chip sees the previous half frame.
            int32_t ax2[5][2], ax3[5][2];
            for (int n = 0; n < 5; n++) for (int c = 0; c < 2; c++) {
                ax2[n][c] = host[n].dsp->ax[1][c];
                ax3[n][c] = host[n].dsp->ax[2][c];
            }
            // A word sent in one slot arrives hop slots later.
            int slot = k % 4, from = (k + 5 - hop) % 4;
            for (int n = 0; n < 5; n++) for (int c = 0; c < 2; c++) {
                wire[slot][n][0][c] = host[n].dsp->ax[0][c];
                wire[slot][n][1][c] = ax2[n][c];
                wire[slot][n][2][c] = ax3[n][c];
            }
            for (int c = 0; c < 2; c++) {
                host[0].dsp->ar[0][c] = wire[from][4][0][c];  // LFO return
                host[0].dsp->ar[1][c] = 0;
                for (int n = 1; n < 5; n++) {
                    host[n].dsp->ar[0][c] = wire[from][n - 1][1][c];
                    host[n].dsp->ar[1][c] = wire[from][n - 1][2][c];
                }
            }
            // Study: a 440 Hz tone on the empty bus slot, into DASP4 only.
            // OB12_BUS_TONE=1 puts it on the odd slot instead, where the voices arrive.
            if (aux_tone && (k & 1) == (getenv("OB12_BUS_TONE") ? 1 : 0) && now >= note_at) for (int c = 0; c < 2; c++)
                host[4].dsp->ar[0][c] = (int32_t)(aux_amp * sin(now * 440 * 6.283185307));
            for (auto& f : force_cells) host[4].dsp->cmem[f.first] = f.second;
            if (aout_left >= 0) { host[4].dsp->cmem[0x7f] = aout_left; host[4].dsp->cmem[0x80] = aout_right; }
            // Diagnostic only: DASP4 output gains the firmware has not been seen to write.
            if (force_gain) for (int index : {0x0d, 0x0f, 0xcb}) host[4].dsp->cmem[index] = 0x200000;
            if (threaded) {
                for (int n = 0; n < 5; n++) {
                    Host& h = host[n];
                    if (((k + (n == 4 ? phase : 0)) & 1) == 0) h.sample_start();
                    if (h.start_pending && ((k + (n == 4 ? phase : 0)) & 1) == 0) { h.start_pending = false; h.running = true; }
                }
                dsp_gate.open(5);
            } else
            for (int n = 0; n < 5; n++) {
                Host& h = host[n];
                if (((k + (n == 4 ? phase : 0)) & 1) == 0) h.sample_start();
                    if (h.start_pending && ((k + (n == 4 ? phase : 0)) & 1) == 0) { h.start_pending = false; h.running = true; }
                if (n == 0 && h.dsp->cmem[watch] != watch_value && watch_log < 24) {  // a host write
                    printf("C%03X host %06X -> %06X at %.4f s\n", watch, watch_value & 0xffffff, h.dsp->cmem[watch] & 0xffffff, now);
                    watch_value = h.dsp->cmem[watch];
                    watch_log++;
                }
                if (h.running) for (int i = 0; i < 256; i++) {
                    // study: one whole sample of DASP4, from PC 0 after t. OB12_TRACE4_ALL=1 prints every
                    // word with the D cell after it ran and the XMEM read address; else multiplies only.
                    if (creads_at > 0 && n == trace_chip && now >= creads_at && !creads_done && h.dsp->pc == 0) creads_on = true;
                    uint32_t trace_w = 0, trace_d = 0;
                    if (creads_on && n == trace_chip) {
                        trace_w = h.dsp->pmem[h.dsp->pc];
                        uint32_t op = (trace_w >> 24) & 0xff;
                        trace_d = h.dsp->study_dmem(trace_w);
                        if (trace_all || (op >= 0x40 && op <= 0x71 && !(op >= 0x68 && op <= 0x6b))) {
                            printf("creads %03X %08X C%03X %06X D%02X %06X", h.dsp->pc, trace_w, h.dsp->study_cmem(trace_w), h.dsp->cmem[h.dsp->study_cmem(trace_w)] & 0xffffff, trace_d, h.dsp->dmem[trace_d] & 0xffffff);
                            if (op == 0x39 && (trace_w & 0x400000))  // XMEM write: the index it lands on
                                printf(" XW %05X", h.dsp->study_xmem((uint32_t)h.dsp->cmem[h.dsp->study_cmem(trace_w)]));
                        }
                        else trace_w = 0;
                    }
                    int pc = h.dsp->pc;
                    uint32_t pcr_cell = (n == pcr_chip && now >= pcr_from && now < pcr_to) ? h.dsp->study_dmem(h.dsp->pmem[pc]) : 0x10000;
                    h.dsp->step();
                    if (pcr_cell < 0x10000) {
                        auto norm = [](int32_t v) { return (double)(((int64_t)v << 40) >> 40) / 8388608.0; };
                        double d = norm(h.dsp->dmem[pcr_cell]), a0 = norm(h.dsp->acc[0]), a1 = norm(h.dsp->acc[1]);
                        pcr_d[pc] += d * d; pcr_a0[pc] += a0 * a0; pcr_a1[pc] += a1 * a1; pcr_n[pc]++;
                        uint32_t w = h.dsp->pmem[pc], sop = (w >> 16) & 0x3f;
                        if ((w >> 24) < 0x80 && sop >= 0x18 && sop <= 0x1a) { double x = norm(h.dsp->ax[sop - 0x18][(w >> 15) & 1]); pcr_ax[pc] += x * x; }
                    }
                    if (trace_w && getenv("OB12_TRACE_MAC")) printf(" -> D %06X A %06X %06X M %013llX %013llX CR1 %06X\n", h.dsp->dmem[trace_d] & 0xffffff, h.dsp->acc[0] & 0xffffff, h.dsp->acc[1] & 0xffffff, (unsigned long long)h.dsp->mac[0] & 0xfffffffffffffULL, (unsigned long long)h.dsp->mac[1] & 0xfffffffffffffULL, h.dsp->cr[1]);  // study: both MACs (52 bits) and CR1 instead of the XMEM columns
                    else if (trace_w) printf(" -> D %06X A %06X %06X XR %05X xrd %06X\n", h.dsp->dmem[trace_d] & 0xffffff, h.dsp->acc[0] & 0xffffff, h.dsp->acc[1] & 0xffffff, h.dsp->study_xread(), h.dsp->xrd & 0xffffff);
                    if (creads_on && n == trace_chip && pc == 511) { creads_on = false; creads_done = true; }
                    if (n == cell_chip && cell_log && pc == 0 && now >= note_at + cell_from && now < note_at + cell_to && ++cell_count % cell_every == 0) {
                        if (!cell_pick.empty()) { for (int c : cell_pick) fwrite(c < 256 ? &h.dsp->dmem[c] : &h.dsp->cmem[c - 256], 4, 1, cell_log); }
                        else { fwrite(h.dsp->dmem, 4, 256, cell_log); fwrite(h.dsp->cmem, 4, 512, cell_log); }
                    }
                    if (n == 0 && start_trace > 0 && h.programs >= 2) { start_trace--; printf("S %03X %08X A %06X %06X DA %03X %03X CA %03X %03X\n", pc, h.dsp->pmem[pc], h.dsp->acc[0] & 0xffffff, h.dsp->acc[1] & 0xffffff, h.dsp->da[0], h.dsp->da[1], h.dsp->ca[0], h.dsp->ca[1]); }
                    if (n == 0 && !watch_d.empty() && now >= note_at) for (auto& cell : watch_d) {
                        int32_t v = h.dsp->dmem[cell.first];
                        if (v != cell.second.last) {
                            cell.second.last = v;
                            auto& writer = cell.second.writers[pc];
                            writer.first++;
                            writer.second = v;
                        }
                        cell.second.peak = std::max(cell.second.peak, abs(v));
                    }
                    if (n == 0 && h.dsp->cmem[watch] != watch_value) {
                        if (watch_log < 24) printf("C%03X dsp pc %03X word %08X %06X -> %06X at %.4f s\n", watch, pc, h.dsp->pmem[pc], watch_value & 0xffffff, h.dsp->cmem[watch] & 0xffffff, now);
                        watch_value = h.dsp->cmem[watch];
                        watch_log++;
                    }
                }
            }
            if (threaded) continue;
            post(k, now);
        }
        if (threaded) {
            dsp_gate.wait();
            post(total - 1, (total - 1) / 96000.0);
        }
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        if (live_on) {
            printf("live: %ld buffers, %ld underruns\n", live_audio_buffers_sent(), live_audio_underruns());
            live_midi_close();
            live_audio_close();
        }
        if (threaded) {
            double all = (double)(__rdtsc() - tsc_start);
            printf("time share: Slave %.1f%%, wait Master %.1f%%, wait DSP %.1f%%, post %.1f%%\n", 100 * spent[0] / all, 100 * spent[1] / all, 100 * spent[2] / all, 100 * spent[3] / all);
            printf("chip busy:"); for (long long c : chip_tsc) printf(" %.1f%%", 100 * c / all); puts("");
        }
        printf("speed: %.2f s of sound in %.2f s, %.2f x real time, %s\n", total / 96000.0, wall, total / 96000.0 / wall, threaded ? "threads" : "serial");
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    write_wav(wav, out);
    if (port_log) fclose(port_log);
    if (host_log) fclose(host_log);
    {
        std::ofstream link("build/live_master_to_slave.bin", std::ios::binary);
        link.write((const char*)m->link().head.data(), m->link().head.size());
    }
    printf("DASP4 C00D %06X C00E %06X C00F %06X C010 %06X C077 %06X C0CB %06X\n", host[4].dsp->cmem[0xd] & 0xffffff, host[4].dsp->cmem[0xe] & 0xffffff,
           host[4].dsp->cmem[0xf] & 0xffffff, host[4].dsp->cmem[0x10] & 0xffffff, host[4].dsp->cmem[0x77] & 0xffffff, host[4].dsp->cmem[0xcb] & 0xffffff);
    std::ofstream raw("build/voice_bus.raw", std::ios::binary);
    raw.write((const char*)bus.data(), bus.size() * 4);
    for (int n = 0; n < 5; n++) {
        printf("DASP%d DCIRC %X DOFF %X DIR %X %X CIR %X %X unknown host %ld, unimplemented:", n, host[n].dsp->dcirc, host[n].dsp->doff,
               host[n].dsp->dir[0], host[n].dsp->dir[1], host[n].dsp->cir[0], host[n].dsp->cir[1], host[n].unknown);
        for (auto& entry : host[n].dsp->unknown) printf(" %08X x%ld", entry.first, entry.second);
        puts("");
    }
    if (port_count[0] && port_count[1]) {
        puts("port rms after the note, even/odd half frame: AX1 L R, AX2 L R, AX3 L R");
        for (int n = 0; n < 5; n++) {
            printf("DASP%d", n);
            for (int p = 0; p < 3; p++) for (int c = 0; c < 2; c++)
                printf(" %.0f/%.0f", sqrt(port_sum[n][p][c][0] / port_count[0]), sqrt(port_sum[n][p][c][1] / port_count[1]));
            puts("");
        }
    }
    for (int n = 0; n < 4; n++) {
        auto* d = host[n].dsp;
        printf("DASP%d D04 now %06X peak %06X; C048:", n, logical_d(d, 4) & 0xffffff, d04_peak[n]);
        for (int a = 0x48; a < 0x52; a++) printf(" %06X", d->cmem[a] & 0xffffff);
        printf("\n  D00:");
        for (int a = 0; a < 0x30; a++) printf(" %06X", logical_d(d, a) & 0xffffff);
        puts("");
    }
    for (auto& cell : watch_d) {
        printf("D%03X peak %06X writers:", cell.first, cell.second.peak);
        for (auto& w : cell.second.writers) printf(" %03X x%ld last %06X;", w.first, w.second.first, w.second.second & 0xffffff);
        puts("");
    }
    if (const char* name = getenv("OB12_DUMP")) {  // whole DMEM by program address, and CMEM
        std::ofstream dump(name);
        char text[64];
        for (int n = 0; n < 5; n++) for (int a = 0; a < 512; a++) {
            snprintf(text, sizeof text, "%d %03X %06X %06X %08X\n", n, a, a < 256 ? logical_d(host[n].dsp, a) & 0xffffff : 0, host[n].dsp->cmem[a] & 0xffffff, host[n].dsp->pmem[a]);  // chip, address, DMEM, CMEM, PMEM
            dump << text;
        }
    }
    if (const char* list = getenv("OB12_CELL_WRITERS")) {  // chip,cell,cell in hex: who changes these logical cells
        int chip = (int)strtol(list, nullptr, 16);
        std::vector<int> cells;
        for (const char* c = strchr(list, ','); c; c = strchr(c + 1, ',')) cells.push_back((int)strtol(c + 1, nullptr, 16));
        tms::Core* d = host[chip].dsp;
        std::map<int, std::map<int, long>> writers;
        for (int i = 0; i < 512 * 64; i++) {
            int pc = d->pc;
            std::vector<int32_t> before;
            uint16_t off = d->doff;
            for (int c : cells) before.push_back(logical_d(d, c));
            d->step();
            if (d->doff == off) for (size_t j = 0; j < cells.size(); j++)
                if (logical_d(d, cells[j]) != before[j]) writers[cells[j]][pc]++;
        }
        for (int c : cells) {
            printf("D%03X now %06X writers:", c, logical_d(d, c) & 0xffffff);
            for (auto& w : writers[c]) printf(" %03X x%ld", w.first, w.second);
            puts("");
        }
    }
    if (const char* trace = getenv("OB12_VOICE_TRACE")) {  // chip,first,last in hex
        int chip, first, last;
        if (sscanf(trace, "%x,%x,%x", &chip, &first, &last) == 3) voice_trace(host[chip].dsp, first, last);
    }
    for (auto& entry : s->ext) printf("slave ext %06X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
    for (auto& entry : s->iostat) printf("slave io %02X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
    printf("slave vectors:"); for (auto& entry : s->vectors) printf(" %02X x%ld", entry.first, entry.second); printf(" ENIR %02X EIRR %02X ELVR %02X %02X TBTC %02X", s->io[0x28], s->io[0x29], s->io[0x2a], s->io[0x2b], s->io[0xa9]); puts("");
    for (auto& entry : m->iostat) if (entry.first < 0x12 || (entry.first >= 0x28 && entry.first < 0x30)) printf("master io %02X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
    printf("font ids:"); for (int i = 0; i < 64; i++) if (m->font_reads[i]) printf(" %d x%ld", i, m->font_reads[i]); puts("");
    for (int n = 0; n < 5; n++) { printf("wraps DASP%d:", n); for (int i = 0; i < 512; i++) if (host[n].dsp->wraps[i]) printf(" %03X %08X x%ld", i, host[n].dsp->pmem[i], host[n].dsp->wraps[i]); puts(""); }
    printf("key reads:"); for (long r : m->key_reads) printf(" %ld", r); puts("");
    m->lcd.save_pbm("build/lcd.pbm");
    m->save_hot("build/hot.txt");
    m->save_ram("build/ram.bin");
    if (getenv("OB12_SHIFT_MISMATCH")) for (int n = 0; n < 5; n++) for (int i = 0; i < 512; i++)
        if (host[n].dsp->shift_mismatch[i]) printf("mismatch DASP%d %03X %08X x%ld\n", n, i, host[n].dsp->pmem[i], host[n].dsp->shift_mismatch[i]);
    if (pcr_chip >= 0) for (int i = 0; i < 512; i++) if (pcr_n[i])
        printf("pcrms %03X %08X D %.4f A0 %.4f A1 %.4f AX %.4f\n", i, host[pcr_chip].dsp->pmem[i], sqrt(pcr_d[i] / pcr_n[i]), sqrt(pcr_a0[i] / pcr_n[i]), sqrt(pcr_a1[i] / pcr_n[i]), sqrt(pcr_ax[i] / pcr_n[i]));
    return 0;
}
