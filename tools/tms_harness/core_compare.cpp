// Differential test: runs src/tms57070 and the public TMS57070 emulator on the same OB-12
// programs and input, with all inferred options off, and compares state after every step.
// The public emulator is built from a local, unmodified copy; it is not in this repo.
//
// usage: core_compare <rom/dsp dir> <frames> [dump file from OB12_DUMP]
// With a dump, programs 0, 1 and 4 run again with the CMEM of a playing instrument.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <iostream>
#include <vector>
// The test reads the emulator's registers directly.
#define _ALLOW_KEYWORD_MACROS
#define private public
#include "TMS57070.h"
#undef private
#include "tms57070/tms57070.h"

static void on_out(TMS57070::Channel, int32_t) {}
static int32_t on_in(uint32_t) { return 0; }
static void on_ext(int32_t, uint32_t) {}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

static long compare(TMS57070::Emulator* a, tms::Core* b, const char* name, int frames) {
    long steps = 0;
    for (int f = 0; f < frames; f++) {
        for (int half = 0; half < 2; half++) {
            int k = f * 2 + half;
            int32_t l = (int32_t)(0.5 * sin(k * 0.031) * 0x7FFFFF), r = (int32_t)(0.4 * sin(k * 0.047) * 0x7FFFFF);
            a->AR1L.value = l; a->AR1R.value = r; a->AR2L.value = r; a->AR2R.value = l;
            b->ar[0][0] = l; b->ar[0][1] = r; b->ar[1][0] = r; b->ar[1][1] = l;
            for (int i = 0; i < 256; i++) {
                int pc = b->pc;
                a->step();
                b->step();
                steps++;
                const char* bad = nullptr;
                int where = 0;
                if ((int)a->PC.value != b->pc) bad = "PC";
                else if (a->ACC1.value != b->acc[0]) bad = "ACC1";
                else if (a->ACC2.value != b->acc[1]) bad = "ACC2";
                else if (a->MACC1.getUpper().value != b->mac_upper(b->mac[0])) bad = "MACC1";
                else if (a->MACC2.getUpper().value != b->mac_upper(b->mac[1])) bad = "MACC2";
                else if ((uint32_t)a->CR1.value != b->cr[1]) bad = "CR1";
                else if (a->XRD.value != b->xrd) bad = "XRD";
                else if (a->DA.one.value != b->da[0] || a->DA.two.value != b->da[1]) bad = "DA";
                else if (a->CA.one.value != b->ca[0] || a->CA.two.value != b->ca[1]) bad = "CA";
                else for (int c = 0; c < 512 && !bad; c++) {
                    if (a->DMEM[c].value != b->dmem[c]) { bad = "DMEM"; where = c; }
                    else if (a->CMEM[c].value != b->cmem[c]) { bad = "CMEM"; where = c; }
                }
                if (bad) {
                    printf("%s: MISMATCH %s[%03X] at step %ld PC %03X word %08X\n", name, bad, where, steps, pc, b->pmem[pc]);
                    return -1;
                }
            }
        }
    }
    printf("%s: %ld steps identical, %zu unknown encodings\n", name, steps, b->unknown.size());
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
        std::string addr, d, c;
        while (dump >> chip >> addr >> d >> c) live[chip].push_back((int32_t)strtol(c.c_str(), nullptr, 16));
    }
    struct Case { const char* prog; int chip; };
    std::vector<Case> cases = {{"prog0", -1}, {"prog1", -1}, {"prog4", -1}, {"prog5", -1}, {"prog9", -1}};
    if (argc > 3) cases.insert(cases.end(), {{"prog0", 0}, {"prog1", 1}, {"prog4", 4}});
    for (auto& one : cases) {
        const char* prog = one.prog;
        auto pmem = read_file(dir + "/" + prog + ".pmem.bin");
        auto cmem = read_file(dir + "/" + prog + ".cmem.bin");
        auto hdr = read_file(dir + "/" + prog + ".hdr.bin");
        auto* a = new TMS57070::Emulator();
        tms::Options options;
        if (getenv("OB12_CMP_PR")) {  // the options a patched public emulator should match
            options.late_mode_change = options.queued_pointer_load = options.compare_3e = true;
            options.reuse_coefficient = true;
        }
        if (getenv("OB12_BENCH_ALL")) {  // the set the instrument runs; only for timing, run with 0 frames
            options.audio_port_writes = options.true_dmem_ring = options.lock_delay = true;
            options.divide_1fc = options.noise_generator = true;
            options.late_mode_change = options.queued_pointer_load = options.compare_3e = true;
            options.reuse_coefficient = true;
        }
        auto* b = new tms::Core(options);
        a->reset();
        a->register_sample_out_callback(on_out);
        a->register_external_bus_in_callback(on_in);
        a->register_external_bus_out_callback(on_ext);
        a->CR0.value = hdr[0] << 16 | hdr[1] << 8 | hdr[2];
        a->CR1.value = hdr[3] << 16 | hdr[4] << 8 | hdr[5];
        a->CR2.value = hdr[6] << 16 | hdr[7] << 8 | hdr[8];
        a->CR3.value = hdr[9] << 16 | hdr[10] << 8 | hdr[11];
        a->update_mac_modes();
        a->CR2.FREE = 0;
        b->load_control(hdr.data());
        b->cr[2] &= ~(1u << 17);
        for (size_t i = 0; i < pmem.size() / 4; i++) {
            uint32_t w = pmem[i * 4] << 24 | pmem[i * 4 + 1] << 16 | pmem[i * 4 + 2] << 8 | pmem[i * 4 + 3];
            a->PMEM[i] = w;
            b->pmem[i] = w;
        }
        for (size_t i = 0; i < cmem.size() / 3; i++) {
            int32_t w = cmem[i * 3] << 16 | cmem[i * 3 + 1] << 8 | cmem[i * 3 + 2];
            a->CMEM[i].value = w;
            b->cmem[i] = (w << 8) >> 8;
        }
        if (one.chip >= 0) for (int i = 0; i < 512; i++) {
            int32_t w = live[one.chip][i];
            a->CMEM[i].value = w;
            b->cmem[i] = (w << 8) >> 8;
        }
        std::string label = std::string(prog) + (one.chip >= 0 ? " live" : "");
        if (compare(a, b, label.c_str(), frames) < 0) failures++;
        // Best of five rounds each, taken in turn, so other load hits both alike.
        double own = 1e9, pub = 1e9;
        for (int round = 0; round < 5; round++) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (long i = 0; i < 5000000; i++) b->step();
            auto t1 = std::chrono::high_resolution_clock::now();
            for (long i = 0; i < 5000000; i++) a->step();
            auto t2 = std::chrono::high_resolution_clock::now();
            own = std::min(own, std::chrono::duration<double, std::nano>(t1 - t0).count() / 5000000);
            pub = std::min(pub, std::chrono::duration<double, std::nano>(t2 - t1).count() / 5000000);
        }
        printf("   ns per step: own %.1f, public %.1f, ratio %.2f\n", own, pub, pub / own);
        delete a;
        delete b;
    }
    printf("failures %d\n", failures);
    return failures ? 1 : 0;
}
