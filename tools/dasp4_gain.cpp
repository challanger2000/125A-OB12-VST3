// DASP4 signal study on the own core. Replays the Slave's host stream, running each loaded
// program as the instrument does, then compares a run with a tone against a silent one.
//
// usage: dasp4_gain <dsp_stream_4.bin> <frames> [search]
#include "tms57070/tms57070.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

static tms::Options options() {
    tms::Options o;
    o.audio_port_writes = o.true_dmem_ring = o.lock_delay = o.maclo_multiply = true;
    return o;
}

static int32_t s24(uint32_t v) { return (int32_t)(v << 8) >> 8; }

static void setup(tms::Core* d, const std::vector<uint8_t>& s) {
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = s[i];
        if (c == 0x0c && i + 1 + 12 + 2048 <= s.size()) {
            const uint8_t* h = &s[i + 1];
            d->reset();
            d->load_control(h);
            for (int w = 0; w < 512; w++) {
                const uint8_t* p = h + 12 + w * 4;
                d->pmem[w] = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
            }
            i += 1 + 12 + 2048;
        } else if (c == 0x05 && i + 1 + 1536 <= s.size()) {
            for (int w = 0; w < 512; w++) {
                const uint8_t* p = &s[i + 1 + w * 3];
                d->cmem[w] = s24(p[0] << 16 | p[1] << 8 | p[2]);
            }
            i += 1 + 1536;
            for (int n = 0; n < 2048; n++) d->step();  // the loaded program runs before the next load
            d->pc = 0;
        } else if (c == 0x03) {
            i++;
            while (i + 5 <= s.size() && s[i] <= 1) {
                d->cmem[(s[i] << 8 | s[i + 1]) & 0x1ff] = s24(s[i + 2] << 16 | s[i + 3] << 8 | s[i + 4]);
                i += 5;
            }
        } else i++;
    }
}

static int32_t tone(int k) { return (int32_t)(0.3 * sin(k * 0.02) * 0x7fffff); }

static double measure(const std::vector<uint8_t>& stream, const std::vector<int>& pokes, int frames, int value) {
    tms::Core d(options());
    setup(&d, stream);
    d.pc = 0;
    for (int index : pokes) d.cmem[index] = value;
    double sum = 0;
    long count = 0;
    for (int f = 0; f < frames; f++) {
        d.ar[0][0] = d.ar[0][1] = tone(f);
        for (int i = 0; i < 256; i++) d.step();
        d.ar[0][0] = d.ar[0][1] = 0;
        for (int i = 0; i < 256; i++) d.step();
        if (f > frames / 2) {
            for (int p = 1; p < 3; p++) for (int c = 0; c < 2; c++) { double v = d.ax[p][c]; sum += v * v; count++; }
        }
    }
    return sqrt(sum / count);
}

static std::set<std::string> differences(tms::Core& a, tms::Core& b) {
    std::set<std::string> out;
    char name[16];
    for (int i = 0; i < 256; i++) {
        if (a.dmem[i] != b.dmem[i]) { sprintf(name, "D%02X", i); out.insert(name); }
        if (a.cmem[i] != b.cmem[i]) { sprintf(name, "C%02X", i); out.insert(name); }
    }
    if (a.acc[0] != b.acc[0]) out.insert("ACC1");
    if (a.acc[1] != b.acc[1]) out.insert("ACC2");
    if (a.mac[0] != b.mac[0]) out.insert("M1");
    if (a.mac[1] != b.mac[1]) out.insert("M2");
    if (a.xrd != b.xrd) out.insert("XRD");
    for (int p = 0; p < 3; p++) for (int c = 0; c < 2; c++) if (a.ax[p][c] != b.ax[p][c]) { sprintf(name, "AX%d%c", p + 1, c ? 'R' : 'L'); out.insert(name); }
    return out;
}

int main(int argc, char** argv) {
    auto stream = read_file(argv[1]);
    int frames = atoi(argv[2]);
    if (argc > 3) {
        tms::Core base(options());
        setup(&base, stream);
        printf("DCIRC %X, baseline output rms %.0f\n", base.dcirc, measure(stream, {}, frames, 0));
        printf("C00D %06X C00F %06X C0CB %06X\n", base.cmem[0xd] & 0xffffff, base.cmem[0xf] & 0xffffff, base.cmem[0xcb] & 0xffffff);
        if (!strcmp(argv[3], "base")) return 0;
        std::vector<int> zero;
        for (int i = 0; i < 512; i++) if (!base.cmem[i]) zero.push_back(i);
        printf("%zu zero words; all set: rms %.0f\n", zero.size(), measure(stream, zero, frames, 0x400000));
        int hits = 0;
        for (int index : zero) {
            double r = measure(stream, {index}, frames, 0x400000);
            if (r > 1) { printf("CMEM %03X opens rms %.0f\n", index, r); hits++; }
        }
        printf("%d single words open an output\n", hits);
        std::vector<int> set = zero;
        double level = measure(stream, set, frames, 0x400000);
        for (size_t i = set.size(); i-- > 0;) {
            auto trial = set;
            trial.erase(trial.begin() + i);
            double r = measure(stream, trial, frames, 0x400000);
            if (r > level * 0.25) set = trial;
        }
        printf("minimal set of %zu keeping a quarter of the level:", set.size());
        for (int index : set) printf(" %03X", index);
        printf("\nrms %.0f\n", measure(stream, set, frames, 0x400000));
        return 0;
    }
    auto* a = new tms::Core(options());
    auto* b = new tms::Core(options());
    setup(a, stream);
    setup(b, stream);
    a->pc = b->pc = 0;
    for (int f = 0; f < frames; f++) {
        a->ar[0][0] = a->ar[0][1] = tone(f);
        for (int i = 0; i < 256; i++) { a->step(); b->step(); }
        a->ar[0][0] = a->ar[0][1] = 0;
        for (int i = 0; i < 256; i++) { a->step(); b->step(); }
    }
    auto before = differences(*a, *b);
    printf("differing at start of final frame: %zu:", before.size());
    for (auto& x : before) printf(" %s", x.c_str());
    puts("");
    a->ar[0][0] = a->ar[0][1] = tone(frames);
    for (int i = 0; i < 512; i++) {
        if (i == 256) a->ar[0][0] = a->ar[0][1] = 0;
        int pc = a->pc;
        uint32_t word = a->pmem[pc];
        a->step(); b->step();
        auto now = differences(*a, *b);
        std::string added, removed;
        for (auto& x : now) if (!before.count(x)) added += " +" + x;
        for (auto& x : before) if (!now.count(x)) removed += " -" + x;
        if (!added.empty() || !removed.empty()) printf("%03X %08X%s%s\n", pc, word, added.c_str(), removed.c_str());
        before = now;
    }
    printf("CMEM 70-79:");
    for (int i = 0x70; i < 0x7a; i++) printf(" %06X", a->cmem[i] & 0xffffff);
    puts("");
    return 0;
}
