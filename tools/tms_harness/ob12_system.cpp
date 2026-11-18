// Whole-instrument test: Master and Slave CPUs linked over UART2, five TMS57070 models
// driven by the Slave's host bytes, DSP chain wired per sheets 09/15 and 10/15.
// Needs a local copy of the public TMS57070 emulator; see docs/HARDWARE.md.
//
// usage: ob12_system <rom dir> <seconds> <note on s> <note off s> <out.wav>
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include <algorithm>
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

#include "TMS57070.h"
using namespace TMS57070;

void unk_hit(int, const char*) {}
void dmem_touch(int, int) {}
static void on_out(Channel, int32_t) {}
static int32_t on_in(uint32_t) { return 0; }
static void on_ext(int32_t, uint32_t) {}

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    Emulator* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false;
    long updates = 0, programs = 0, unknown = 0;

    void deselect() { if (state == Update) { state = Idle; buffer.clear(); } }
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
            if (buffer.size() == 12 + 2048) load_program();
            break;
        case Coefficients:
            buffer.push_back(data);
            if (buffer.size() == 1536) load_coefficients();
            break;
        case Update:
            buffer.push_back(data);
            if (buffer.size() == 5) {
                int address = (buffer[0] << 8 | buffer[1]) & 0x1ff;
                dsp->CMEM[address].value = buffer[2] << 16 | buffer[3] << 8 | buffer[4];
                updates++;
                buffer.clear();
            }
            break;
        }
    }
    void load_program() {
        const u8* h = buffer.data();
        running = false;
        dsp->reset();
        dsp->CR0.value = h[0] << 16 | h[1] << 8 | h[2];
        dsp->CR1.value = h[3] << 16 | h[4] << 8 | h[5];
        dsp->CR2.value = h[6] << 16 | h[7] << 8 | h[8];
        dsp->CR3.value = h[9] << 16 | h[10] << 8 | h[11];
        dsp->update_mac_modes();
        dsp->CR2.FREE = 0;
        for (int i = 0; i < 512; i++) {
            const u8* w = h + 12 + i * 4;
            dsp->PMEM[i] = w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3];
        }
        programs++;
        state = Idle;
    }
    void load_coefficients() {
        for (int i = 0; i < 512; i++) {
            const u8* w = buffer.data() + i * 3;
            dsp->CMEM[i].value = w[0] << 16 | w[1] << 8 | w[2];
        }
        running = true;
        state = Idle;
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

int main(int argc, char** argv) {
    std::string rom = argv[1];
    double seconds = atof(argv[2]), note_on = atof(argv[3]), note_off = atof(argv[4]);
    std::string wav = argv[5];

    auto* m = new master::Master();
    auto* s = new slave::Slave();
    m->load(rom);
    s->load(rom);
    m->attach(m); s->attach(s);
    m->reset(); s->reset();

    Host host[5];
    for (auto& h : host) {
        h.dsp = new Emulator();
        h.dsp->reset();
        h.dsp->register_sample_out_callback(on_out);
        h.dsp->register_external_bus_in_callback(on_in);
        h.dsp->register_external_bus_out_callback(on_ext);
    }
    s->on_dsp_byte = [&](u8 selected, u8 data) {
        for (int chip = 0; chip < 5; chip++) if (selected & (1 << chip)) host[chip].byte(data);
    };
    s->on_dsp_select = [&]() { for (auto& h : host) h.deselect(); };

    size_t m_sent = 0, s_sent = 0;
    std::vector<int16_t> out;
    double sum[4] = {0, 0, 0, 0};
    double chip_sum[5] = {0, 0, 0, 0, 0};
    std::vector<int32_t> bus[2];
    int direct_limit = getenv("OB12_DIRECT") ? atoi(getenv("OB12_DIRECT")) : 0;
    long frames = 0, count = 0;
    long total_half_frames = (long)(seconds * 96000);
    bool on_done = false, off_done = false, phase_set = false;
    bool direct = getenv("OB12_DIRECT") != nullptr;
    int direct_channel = 0;
    if (direct) on_done = off_done = true;
    try {
        for (long k = 0; k < total_half_frames; k++) {
            double now = k / 96000.0;
            if (!on_done && now >= note_on) { m->midi_in({0x90, 0x3c, 0x64}); on_done = true; }
            if (!off_done && now >= note_off) { m->midi_in({0x80, 0x3c, 0x40}); off_done = true; }

            // Direct test: note on each channel, straight into the Slave, bypassing the Master.
            if (direct && direct_channel < direct_limit && now >= note_on + direct_channel * 1.0) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), {(u8)(0x90 | direct_channel), 0x3c, 0x64});
                direct_channel++;
            }
            const int slice = 167;
            m->run(slice); m->tick(slice - m->cycles_left());
            s->run(slice); s->tick(slice - s->cycles_left());
            auto& mu = m->link();
            auto& su = s->link();
            for (; m_sent < mu.head.size(); m_sent++) su.rx_queue.push_back(mu.head[m_sent]);
            for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);

            if (host[4].running) {
                int32_t ax2l[5], ax2r[5], ax3l[5], ax3r[5];
                for (int n = 0; n < 5; n++) {
                    auto* d = host[n].dsp;
                    ax2l[n] = d->AX2L.value; ax2r[n] = d->AX2R.value;
                    ax3l[n] = d->AX3L.value; ax3r[n] = d->AX3R.value;
                }
                host[0].dsp->AR1L.value = host[4].dsp->AX1L.value;
                host[0].dsp->AR1R.value = host[4].dsp->AX1R.value;
                host[0].dsp->AR2L.value = 0;
                host[0].dsp->AR2R.value = 0;
                for (int n = 1; n < 5; n++) {
                    auto* d = host[n].dsp;
                    d->AR1L.value = ax2l[n - 1]; d->AR1R.value = ax2r[n - 1];
                    d->AR2L.value = ax3l[n - 1]; d->AR2R.value = ax3r[n - 1];
                }
                // Chip start phase is not modelled. DASP4 reads the voice bus in the half frame
                // where its program expects it, so offset it by one half frame once.
                if (!phase_set) { phase_set = true; for (int i = 0; i < 256; i++) host[4].dsp->step(); }
                for (int n = 0; n < 5; n++) {
                    if (!host[n].running) continue;
                    for (int i = 0; i < 256; i++) host[n].dsp->step();
                }
                // The voice bus as DASP4 receives it, one stream per half frame slot.
                bus[k & 1].push_back(host[4].dsp->AR1L.value);
                bus[k & 1].push_back(host[4].dsp->AR1R.value);
                if (k & 1) {
                    auto* d = host[4].dsp;
                    int32_t v[4] = {d->AX2L.value, d->AX2R.value, d->AX3L.value, d->AX3R.value};
                    for (int c = 0; c < 4; c++) sum[c] += (double)v[c] * v[c];
                    for (int n = 0; n < 5; n++) {
                        double l = host[n].dsp->AX2L.value, r = host[n].dsp->AX2R.value;
                        chip_sum[n] += l * l + r * r;
                    }
                    out.push_back((int16_t)(v[0] >> 8));
                    out.push_back((int16_t)(v[1] >> 8));
                    frames++; count++;
                }
            }
            if (k % 96000 == 95999) {
                printf("t=%.0f s master %ld slave %ld steps; link M>S %zu S>M %zu; host", (k + 1) / 96000.0, m->steps, s->steps, m_sent, s_sent);
                for (auto& h : host) printf(" %ld/%ld%s", h.programs, h.updates, h.running ? "r" : "");
                printf("; AX2 rms");
                for (int n = 0; n < 5; n++) { printf(" %.0f", count ? sqrt(chip_sum[n] / count / 2) : 0); chip_sum[n] = 0; }
                printf("; rms MOUT %.0f %.0f AOUT %.0f %.0f\n",
                       count ? sqrt(sum[0] / count) : 0, count ? sqrt(sum[1] / count) : 0,
                       count ? sqrt(sum[2] / count) : 0, count ? sqrt(sum[3] / count) : 0);
                fflush(stdout);
                sum[0] = sum[1] = sum[2] = sum[3] = 0; count = 0;
            }
        }
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    write_wav(wav, out);
    for (int slot = 0; slot < 2; slot++) {
        std::ofstream raw("build/voice_bus_" + std::to_string(slot) + ".raw", std::ios::binary);
        raw.write((const char*)bus[slot].data(), bus[slot].size() * 4);
    }
    for (int n = 0; n < 5; n++) {
        auto* d = host[n].dsp;
        char name[32];
        sprintf(name, "build/dasp%d_state.bin", n);
        std::ofstream state(name, std::ios::binary);
        for (int i = 0; i < 512; i++) { int32_t v = d->CMEM[i].value; state.write((char*)&v, 4); }
        for (int i = 0; i < 512; i++) { int32_t v = d->DMEM[i].value; state.write((char*)&v, 4); }
    }
    printf("frames %ld\n", frames);
    for (int n = 0; n < 5; n++) printf("DASP%d unknown host bytes %ld\n", n, host[n].unknown);
    m->lcd.save_pbm("build/lcd.pbm");
    m->report();
    s->report();
    return 0;
}
