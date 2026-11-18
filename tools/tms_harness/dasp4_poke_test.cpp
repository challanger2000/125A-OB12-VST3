// Finds which zero CMEM words gate DASP4's outputs.
// Feeds a sine into AR1, then for each zero CMEM word sets 0x400000 and measures
// the four outputs. The Slave normally writes these words; here they are forced.
// Needs a local copy of the public TMS57070 emulator; see docs/HARDWARE.md.
#include "TMS57070.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace TMS57070;

void unk_hit(int, const char*) {}
static void on_out(Channel, int32_t) {}
static int32_t on_in(uint32_t) { return 0; }
static void on_ext(int32_t, uint32_t) {}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

struct Result { double out[4]; };

static Result run(const std::string& dir, const std::vector<int>& pokes, int frames) {
    auto pmem = read_file(dir + "/prog4.pmem.bin");
    auto cmem = read_file(dir + "/prog4.cmem.bin");
    auto hdr = read_file(dir + "/prog4.hdr.bin");
    auto* dsp = new Emulator();
    dsp->reset();
    dsp->register_sample_out_callback(on_out);
    dsp->register_external_bus_in_callback(on_in);
    dsp->register_external_bus_out_callback(on_ext);
    for (size_t i = 0; i < pmem.size() / 4; i++)
        dsp->PMEM[i] = pmem[i * 4] << 24 | pmem[i * 4 + 1] << 16 | pmem[i * 4 + 2] << 8 | pmem[i * 4 + 3];
    for (size_t i = 0; i < cmem.size() / 3; i++)
        dsp->CMEM[i].value = cmem[i * 3] << 16 | cmem[i * 3 + 1] << 8 | cmem[i * 3 + 2];
    dsp->CR0.value = hdr[0] << 16 | hdr[1] << 8 | hdr[2];
    dsp->CR1.value = hdr[3] << 16 | hdr[4] << 8 | hdr[5];
    dsp->CR2.value = hdr[6] << 16 | hdr[7] << 8 | hdr[8];
    dsp->CR3.value = hdr[9] << 16 | hdr[10] << 8 | hdr[11];
    dsp->update_mac_modes();
    dsp->CR2.FREE = 0;
    for (int index : pokes) dsp->CMEM[index].value = 0x400000;
    double sum[4] = {0, 0, 0, 0};
    long count = 0;
    for (int k = 0; k < frames * 2; k++) {
        int32_t left = (int32_t)(0.5 * sin(k * 0.031) * 0x7FFFFF);
        int32_t right = (int32_t)(0.4 * sin(k * 0.047 + 1) * 0x7FFFFF);
        dsp->AR1L.value = left;
        dsp->AR1R.value = right;
        for (int i = 0; i < 256; i++) dsp->step();
        if (k >= frames) {
            int32_t value[4] = {dsp->AX2L.value, dsp->AX2R.value, dsp->AX3L.value, dsp->AX3R.value};
            for (int c = 0; c < 4; c++) sum[c] += (double)value[c] * value[c];
            count++;
        }
    }
    delete dsp;
    Result result;
    for (int c = 0; c < 4; c++) result.out[c] = sqrt(sum[c] / count);
    return result;
}

int main(int argc, char** argv) {
    std::string dir = argv[1];
    int frames = atoi(argv[2]);
    auto cmem = read_file(dir + "/prog4.cmem.bin");
    int words = (int)(cmem.size() / 3);
    std::vector<int> hits;
    int zero_words = 0;
    for (int i = 0; i < words; i++) {
        if (cmem[i * 3] | cmem[i * 3 + 1] | cmem[i * 3 + 2]) continue;
        zero_words++;
        Result r = run(dir, {i}, frames);
        if (r.out[0] + r.out[1] + r.out[2] + r.out[3] > 1) {
            printf("CMEM %03X: MOUT %.0f %.0f AOUT %.0f %.0f\n", i, r.out[0], r.out[1], r.out[2], r.out[3]);
            hits.push_back(i);
        }
    }
    printf("%d zero words tested, %zu open an output\n", zero_words, hits.size());
    if (hits.empty()) {
        // No single word opens a path: start from all of them and remove what is not needed.
        std::vector<int> set;
        for (int i = 0; i < words; i++)
            if (!(cmem[i * 3] | cmem[i * 3 + 1] | cmem[i * 3 + 2])) set.push_back(i);
        auto loud = [](const Result& r) { return r.out[0] + r.out[1] + r.out[2] + r.out[3] > 1; };
        Result all = run(dir, set, frames);
        printf("all zero words set: MOUT %.0f %.0f AOUT %.0f %.0f\n", all.out[0], all.out[1], all.out[2], all.out[3]);
        if (loud(all)) {
            for (size_t i = set.size(); i-- > 0;) {
                std::vector<int> trial = set;
                trial.erase(trial.begin() + i);
                if (loud(run(dir, trial, frames))) set = trial;
            }
            printf("minimal set of %zu:", set.size());
            for (int index : set) printf(" %03X", index);
            Result r = run(dir, set, frames);
            printf("\nminimal set: MOUT %.0f %.0f AOUT %.0f %.0f\n", r.out[0], r.out[1], r.out[2], r.out[3]);
        }
    } else {
        Result r = run(dir, hits, frames);
        printf("all hits together: MOUT %.0f %.0f AOUT %.0f %.0f (input rms L 2965821 R 2372657)\n", r.out[0], r.out[1], r.out[2], r.out[3]);
    }
    return 0;
}
