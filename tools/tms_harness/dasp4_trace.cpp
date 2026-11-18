// Differential signal trace through DASP4: two copies run the same program and the same
// Slave coefficient stream, one with a tone on AR1, one with silence. Wherever their state
// differs, the signal is present. The point where the difference ends is where it stops.
// Needs a local copy of the public TMS57070 emulator; see docs/HARDWARE.md.
//
// usage: dasp4_trace <rom/dsp dir> <dsp_stream_4.bin> <frames>
#include "TMS57070.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>
using namespace TMS57070;

void unk_hit(int, const char*) {}
static std::set<int> touched[256];
void dmem_touch(int addr, int pc) { if (addr < 256) touched[addr].insert((pc - 1) & 511); }
static void on_out(Channel, int32_t) {}
static int32_t on_in(uint32_t) { return 0; }
static void on_ext(int32_t, uint32_t) {}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

// Replays load and update commands. Group first bytes are 0 or 1, so 03 marks a new batch.
static void replay(Emulator* d, const std::vector<uint8_t>& s) {
    size_t i = 0;
    long updates = 0;
    while (i < s.size()) {
        uint8_t c = s[i];
        if (c == 0x0c && i + 1 + 12 + 2048 <= s.size()) {
            const uint8_t* h = &s[i + 1];
            d->reset();
            d->CR0.value = h[0] << 16 | h[1] << 8 | h[2];
            d->CR1.value = h[3] << 16 | h[4] << 8 | h[5];
            d->CR2.value = h[6] << 16 | h[7] << 8 | h[8];
            d->CR3.value = h[9] << 16 | h[10] << 8 | h[11];
            d->update_mac_modes();
            d->CR2.FREE = 0;
            for (int w = 0; w < 512; w++) {
                const uint8_t* p = h + 12 + w * 4;
                d->PMEM[w] = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
            }
            i += 1 + 12 + 2048;
        } else if (c == 0x05 && i + 1 + 1536 <= s.size()) {
            for (int w = 0; w < 512; w++) {
                const uint8_t* p = &s[i + 1 + w * 3];
                d->CMEM[w].value = p[0] << 16 | p[1] << 8 | p[2];
            }
            i += 1 + 1536;
        } else if (c == 0x03) {
            i++;
            while (i + 5 <= s.size() && s[i] <= 1) {
                d->CMEM[(s[i] << 8 | s[i + 1]) & 0x1ff].value = s[i + 2] << 16 | s[i + 3] << 8 | s[i + 4];
                i += 5;
                updates++;
            }
        } else i++;
    }
    if (getenv("OB12_VERBOSE")) printf("replayed %ld CMEM updates, CR1 %06X CR3 %06X\n", updates, (unsigned)d->CR1.value, (unsigned)d->CR3.value);
}

static std::set<std::string> differences(Emulator* a, Emulator* b) {
    std::set<std::string> out;
    char name[16];
    for (int i = 0; i < 512; i++) {
        if (a->DMEM[i].value != b->DMEM[i].value) { sprintf(name, "D%03X", i); out.insert(name); }
        if (a->CMEM[i].value != b->CMEM[i].value) { sprintf(name, "C%03X", i); out.insert(name); }
    }
    if (a->ACC1.value != b->ACC1.value) out.insert("ACC1");
    if (a->ACC2.value != b->ACC2.value) out.insert("ACC2");
    if (a->MACC1.getUpper().value != b->MACC1.getUpper().value) out.insert("M1");
    if (a->MACC2.getUpper().value != b->MACC2.getUpper().value) out.insert("M2");
    if (a->XRD.value != b->XRD.value) out.insert("XRD");
    if (a->AX1L.value != b->AX1L.value) out.insert("AX1L");
    if (a->AX2L.value != b->AX2L.value) out.insert("AX2L");
    if (a->AX2R.value != b->AX2R.value) out.insert("AX2R");
    if (a->AX3L.value != b->AX3L.value) out.insert("AX3L");
    if (a->AX3R.value != b->AX3R.value) out.insert("AX3R");
    return out;
}

// Gain search on the real coefficient state: which zero CMEM words open an output?
static double measure(const std::vector<uint8_t>& stream, const std::vector<int>& pokes, int frames) {
    static Emulator* d = new Emulator();
    memset(d->DMEM, 0, sizeof d->DMEM);
    memset(d->XMEM, 0, 1 << 20);
    d->register_sample_out_callback(on_out);
    d->register_external_bus_in_callback(on_in);
    d->register_external_bus_out_callback(on_ext);
    replay(d, stream);
    d->PC.value = 0;
    for (int index : pokes) d->CMEM[index].value = 0x400000;
    double sum = 0;
    long count = 0;
    for (int k = 0; k < frames * 2; k++) {
        int32_t tone = (k & 1) == 0 ? (int32_t)(0.3 * sin(k * 0.02) * 0x7FFFFF) : 0;
        d->AR1L.value = tone; d->AR1R.value = tone;
        for (int i = 0; i < 256; i++) d->step();
        if (k > frames) {
            double v[4] = {(double)d->AX2L.value, (double)d->AX2R.value, (double)d->AX3L.value, (double)d->AX3R.value};
            for (double x : v) sum += x * x;
            count += 4;
        }
    }
    return sqrt(sum / count);
}

static int search(const std::vector<uint8_t>& stream, int frames) {
    Emulator* base = new Emulator();
    base->register_sample_out_callback(on_out);
    base->register_external_bus_in_callback(on_in);
    base->register_external_bus_out_callback(on_ext);
    replay(base, stream);
    std::vector<int> zero;
    for (int i = 0; i < 512; i++) if (!base->CMEM[i].value) zero.push_back(i);
    printf("%zu zero words; baseline output rms %.0f\n", zero.size(), measure(stream, {}, frames));
    double all = measure(stream, zero, frames);
    printf("all zero words set: rms %.0f\n", all);
    int hits = 0;
    for (int index : zero) {
        double r = measure(stream, {index}, frames);
        if (r > 1) { printf("CMEM %03X: rms %.0f\n", index, r); hits++; }
    }
    printf("%d single words open an output\n", hits);
    if (all > 1 && !hits) {
        std::vector<int> set = zero;
        for (size_t i = set.size(); i-- > 0;) {
            auto trial = set;
            trial.erase(trial.begin() + i);
            if (measure(stream, trial, frames) > 1) set = trial;
        }
        printf("minimal set:");
        for (int index : set) printf(" %03X", index);
        printf(" rms %.0f\n", measure(stream, set, frames));
    }
    return 0;
}

// Silent input: where does the first nonzero data come from, and how does the output grow?
static int origin(const std::vector<uint8_t>& stream, int frames) {
    Emulator* d = new Emulator();
    d->register_sample_out_callback(on_out);
    d->register_external_bus_in_callback(on_in);
    d->register_external_bus_out_callback(on_ext);
    replay(d, stream);
    d->PC.value = 0;
    static bool seen[512];
    int logged = 0;
    double sum = 0;
    long count = 0, xmem_nonzero = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < 512; i++) {
            int pc = d->PC.value;
            uint32_t word = d->PMEM[pc];
            d->step();
            for (int a = 0; a < 512; a++) {
                if (!seen[a] && d->DMEM[a].value) {
                    seen[a] = true;
                    if (logged++ < 40)
                        printf("frame %d PC %03X %08X: D%03X = %06X  ACC %06X %06X M %06X %06X XRD %06X\n", f, pc, word, a,
                               d->DMEM[a].value & 0xffffff, d->ACC1.value & 0xffffff, d->ACC2.value & 0xffffff,
                               d->MACC1.getUpper().value & 0xffffff, d->MACC2.getUpper().value & 0xffffff, d->XRD.value & 0xffffff);
                }
            }
        }
        double v = d->AX2L.value;
        sum += v * v;
        count++;
        if (count == frames / 8 || f == frames - 1) {
            printf("frame %d AX2L rms %.0f last AX2 %06X %06X AX3 %06X %06X\n", f, sqrt(sum / count), d->AX2L.value & 0xffffff,
                   d->AX2R.value & 0xffffff, d->AX3L.value & 0xffffff, d->AX3R.value & 0xffffff);
            sum = 0; count = 0;
        }
    }
    for (int i = 0; i < 0x10000; i++) if (d->XMEM[i].value) xmem_nonzero++;
    printf("nonzero D cells %d, XMEM nonzero in first 64K %ld, XOFF %X DOFF %X DCIRC %X\n", logged, xmem_nonzero, d->XOFF, d->DOFF.value, d->DCIRC.value);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 5 && !strcmp(argv[5], "origin")) return origin(read_file(argv[2]), atoi(argv[3]));
    if (argc > 5) return search(read_file(argv[2]), atoi(argv[3]));
    auto stream = read_file(argv[2]);
    int frames = atoi(argv[3]);
    int slot = argc > 4 ? atoi(argv[4]) : 1;
    Emulator* dsp[2] = {new Emulator(), new Emulator()};
    for (auto* d : dsp) {
        d->register_sample_out_callback(on_out);
        d->register_external_bus_in_callback(on_in);
        d->register_external_bus_out_callback(on_ext);
        replay(d, stream);
        d->PC.value = 0;
    }
    for (int f = 0; f <= frames; f++) {
        for (int half = 0; half < 2; half++) {
            int32_t tone = half == slot ? (int32_t)(0.3 * sin((f * 2 + half) * 0.02) * 0x7FFFFF) : 0;
            dsp[0]->AR1L.value = tone; dsp[0]->AR1R.value = tone;
            dsp[1]->AR1L.value = 0; dsp[1]->AR1R.value = 0;
            if (f < frames) {
                for (int i = 0; i < 256; i++) { dsp[0]->step(); dsp[1]->step(); }
            }
        }
    }
    puts("final frame, per PC: changes in the differing set");
    std::set<std::string> before = differences(dsp[0], dsp[1]);
    printf("start: %zu differing:", before.size());
    int shown = 0;
    for (auto& x : before) if (shown++ < 30) printf(" %s", x.c_str());
    puts("");
    for (int i = 0; i < 512; i++) {
        if (i == 0 || i == 256) {
            int32_t tone = (i != 0) == (slot != 0) ? (int32_t)(0.3 * sin((frames * 2 + 1) * 0.02) * 0x7FFFFF) : 0;
            dsp[0]->AR1L.value = tone; dsp[0]->AR1R.value = tone;
        }
        int pc = dsp[0]->PC.value;
        uint32_t word = dsp[0]->PMEM[pc];
        dsp[0]->step(); dsp[1]->step();
        auto now = differences(dsp[0], dsp[1]);
        std::string added, removed;
        for (auto& x : now) if (!before.count(x)) added += " +" + x;
        for (auto& x : before) if (!now.count(x)) removed += " -" + x;
        if (!added.empty() || !removed.empty())
            printf("%03X %08X%s%s\n", pc, word, added.c_str(), removed.c_str());
        before = now;
    }
    for (int addr : {0x00, 0x04, 0x03, 0x11, 0x17, 0x18, 0x0b, 0x0f}) {
        printf("D%02X touched at:", addr);
        for (int pc : touched[addr]) printf(" %03X", pc);
        puts("");
    }
    return 0;
}
