// Regression test: the core of commit 87d17c3, proved identical to the public emulator by
// core_compare, against the current core. Every inferred option is off unless named.
// The public emulator is no longer on this machine, so this carries that proof forward.
//
// usage: core_regress <rom/dsp dir> <frames> [dump file from OB12_DUMP]
// With a dump, each program also runs with the live CMEM of a playing instrument.
#define tms tms_old
#include "oldcore/tms57070.h"
#undef tms
#include "tms57070/tms57070.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

struct Program {
    std::vector<uint8_t> hdr;
    std::vector<uint32_t> pmem;
    std::vector<int32_t> cmem;
};

template <class C> static void load(C* core, const Program& p) {
    core->load_control(p.hdr.data());
    core->cr[2] &= ~(1u << 17);
    for (size_t i = 0; i < p.pmem.size(); i++) core->pmem[i] = p.pmem[i];
    for (size_t i = 0; i < p.cmem.size(); i++) core->cmem[i] = p.cmem[i];
}

template <class A, class B> static const char* differs(A* a, B* b, int& where) {
    if (a->pc != b->pc) return "PC";
    if (a->acc[0] != b->acc[0] || a->acc[1] != b->acc[1]) return "ACC";
    if (a->mac[0] != b->mac[0] || a->mac[1] != b->mac[1]) return "MAC";
    for (int i = 0; i < 4; i++) if (a->cr[i] != b->cr[i]) return "CR";
    if (a->xrd != b->xrd) return "XRD";
    if (a->da[0] != b->da[0] || a->da[1] != b->da[1]) return "DA";
    if (a->ca[0] != b->ca[0] || a->ca[1] != b->ca[1]) return "CA";
    for (int p = 0; p < 3; p++) for (int c = 0; c < 2; c++) if (a->ax[p][c] != b->ax[p][c]) return "AX";
    for (int c = 0; c < 512; c++) {
        if (a->dmem[c] != b->dmem[c]) { where = c; return "DMEM"; }
        if (a->cmem[c] != b->cmem[c]) { where = c; return "CMEM"; }
    }
    return nullptr;
}

template <class A, class B> static long run(A* a, B* b, const char* name, int frames) {
    long steps = 0;
    for (int f = 0; f < frames; f++) for (int half = 0; half < 2; half++) {
        int k = f * 2 + half;
        int32_t l = (int32_t)(0.5 * sin(k * 0.031) * 0x7FFFFF), r = (int32_t)(0.4 * sin(k * 0.047) * 0x7FFFFF);
        a->ar[0][0] = l; a->ar[0][1] = r; a->ar[1][0] = r; a->ar[1][1] = l;
        b->ar[0][0] = l; b->ar[0][1] = r; b->ar[1][0] = r; b->ar[1][1] = l;
        for (int i = 0; i < 256; i++) {
            int pc = b->pc, where = 0;
            a->step();
            b->step();
            steps++;
            if (const char* bad = differs(a, b, where)) {
                printf("%s: differs in %s[%03X] at step %ld PC %03X word %08X\n", name, bad, where, steps, pc, b->pmem[pc]);
                return -steps;
            }
        }
    }
    printf("%s: %ld steps identical\n", name, steps);
    return steps;
}

int main(int argc, char** argv) {
    std::string dir = argv[1];
    int frames = atoi(argv[2]);
    int failures = 0;
    std::vector<std::vector<int32_t>> live(5);
    if (argc > 3) {
        std::ifstream dump(argv[3]);
        int chip;
        std::string a, d, c;
        while (dump >> chip >> a >> d >> c) {
            int32_t v = (int32_t)strtol(c.c_str(), nullptr, 16);
            live[chip].push_back((v << 8) >> 8);
        }
    }
    const char* names[] = {"prog0", "prog1", "prog4", "prog5", "prog9"};
    int live_chip[] = {0, 1, 4, -1, -1};
    std::vector<std::pair<const char*, std::function<void(tms::Options&)>>> options = {
        {"audio_port_writes", [](tms::Options& o) { o.audio_port_writes = true; }},
        {"true_dmem_ring", [](tms::Options& o) { o.true_dmem_ring = true; }},
        {"lock_delay", [](tms::Options& o) { o.lock_delay = true; }},
        {"maclo_multiply", [](tms::Options& o) { o.maclo_multiply = true; }},
        {"divide_1fc", [](tms::Options& o) { o.divide_1fc = true; }},
        {"noise_generator", [](tms::Options& o) { o.noise_generator = true; }},
        {"late_mode_change", [](tms::Options& o) { o.late_mode_change = true; }},
        {"queued_pointer_load", [](tms::Options& o) { o.queued_pointer_load = true; }},
        {"compare_3e", [](tms::Options& o) { o.compare_3e = true; }},
    };
    for (int n = 0; n < 5; n++) {
        std::string prog = names[n];
        Program p;
        p.hdr = read_file(dir + "/" + prog + ".hdr.bin");
        auto pm = read_file(dir + "/" + prog + ".pmem.bin");
        auto cm = read_file(dir + "/" + prog + ".cmem.bin");
        for (size_t i = 0; i < pm.size() / 4; i++) p.pmem.push_back(pm[i * 4] << 24 | pm[i * 4 + 1] << 16 | pm[i * 4 + 2] << 8 | pm[i * 4 + 3]);
        for (size_t i = 0; i < cm.size() / 3; i++) {
            int32_t w = cm[i * 3] << 16 | cm[i * 3 + 1] << 8 | cm[i * 3 + 2];
            p.cmem.push_back((w << 8) >> 8);
        }
        auto* a = new tms_old::Core();
        auto* b = new tms::Core();
        load(a, p); load(b, p);
        if (run(a, b, prog.c_str(), frames) < 0) failures++;
        delete a; delete b;
        if (live_chip[n] >= 0 && live[live_chip[n]].size() == 512) {
            Program q = p;
            q.cmem = live[live_chip[n]];
            a = new tms_old::Core();
            b = new tms::Core();
            load(a, q); load(b, q);
            std::string label = prog + " live";
            if (run(a, b, label.c_str(), frames) < 0) failures++;
            delete a; delete b;
        }
        // Where each inferred option first leaves the plain core, on this program.
        for (auto& option : options) {
            tms::Options on;
            option.second(on);
            auto* c = new tms::Core();
            auto* d = new tms::Core(on);
            const Program& use = live_chip[n] >= 0 && live[live_chip[n]].size() == 512 ? [&]() -> const Program& { static Program q; q = p; q.cmem = live[live_chip[n]]; return q; }() : p;
            load(c, use); load(d, use);
            std::string label = prog + " +" + option.first;
            run(c, d, label.c_str(), frames < 500 ? frames : 500);
            delete c; delete d;
        }
    }
    printf("failures %d\n", failures);
    return failures ? 1 : 0;
}
