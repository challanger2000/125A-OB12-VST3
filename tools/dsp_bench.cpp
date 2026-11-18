// Speed of the TMS57070 core alone: a carved program from rom/dsp, noise on the input, word by
// word and through the translated program; the two must leave the same state.
// usage: dsp_bench <rom dir>/dsp <program number> [million steps]
#include "tms57070/tms57070.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    std::string base = std::string(argv[1]) + "/prog" + argv[2];
    long steps = (argc > 3 ? atol(argv[3]) : 100) * 1000000L;
    tms::Options o;
    o.audio_port_writes = o.true_dmem_ring = o.lock_delay = o.maclo_multiply = o.divide_1fc = o.noise_generator = true;
    o.late_mode_change = o.compare_3e = o.maclo_shift = o.maclo_unsigned = o.queued_pointer_load = o.bus_sum_saturates = true;
    o.reuse_coefficient = o.late_saturation_mode = o.xread_flush = true;
    if (argv[2][0] == '4') { o.xmem_words = 0x20000; o.xread_cycles = 5; }
    double ns[2]; uint32_t sum[2];
    for (int fast = 0; fast < 2; fast++) {  // word by word, then through run() with the translated program
        tms::Core c(o);
        auto h = read(base + ".hdr.bin"), p = read(base + ".pmem.bin"), k = read(base + ".cmem.bin");
        c.load_control(h.data());
        for (int i = 0; i < 512; i++) c.pmem[i] = p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (size_t i = 0; i + 2 < k.size() && i / 3 < 512; i += 3) c.cmem[i / 3] = (int32_t)((uint32_t)(k[i] << 16 | k[i + 1] << 8 | k[i + 2]) << 8) >> 8;
        if (argv[2][0] == '4') c.dcirc = 0xCD;  // what the effects chip's start-up program (prog9, C70CD000) leaves
        if (fast) c.select_fast();
        uint32_t noise = 1;
        auto t0 = std::chrono::steady_clock::now();
        for (long done = 0; done < steps; done += 256) {
            noise = noise * 1664525u + 1013904223u;
            c.ar[0][0] = c.ar[0][1] = (int32_t)(noise >> 12) - 0x80000;
            c.run(256);
        }
        ns[fast] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / steps;
        uint32_t s = 0;  // a change that keeps the behaviour keeps this number
        for (int i = 0; i < 512; i++) s = s * 31 + (uint32_t)c.dmem[i] + (uint32_t)c.cmem[i];
        for (int32_t x : c.xmem) s = s * 31 + (uint32_t)x;
        sum[fast] = s + c.ax[1][0] + c.ax[1][1] + c.ax[0][0] + c.pc + c.cr[1] + c.acc[0] + c.acc[1];
        printf("%s: %.2f ns a step, state %08X, translated halves %ld, single steps %ld\n", fast ? "translated" : "word by word", ns[fast], sum[fast], c.fast_halves, c.slow_steps);
    }
    printf("%s, %.2f x\n", sum[0] == sum[1] ? "same state" : "STATES DIFFER", ns[0] / ns[1]);
    return sum[0] != sum[1];
}
