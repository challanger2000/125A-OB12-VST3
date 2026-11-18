// Five-DSP chain test, wired per service manual sheets 09/15 and 10/15:
//   DASPn AX2 -> DASPn+1 AR1, DASPn AX3 -> DASPn+1 AR2 (n = 0..3)
//   DASP4 AX1 -> DASP0 AR1 (LFO), DASP0 AR2 grounded
// Serial links are modelled as one copy per half frame (256 DSP cycles).
// Needs a local copy of the public TMS57070 emulator; see docs/HARDWARE.md.
#include "TMS57070.h"
#include <chrono>
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

static Emulator* load(const std::string& dir, const std::string& prog) {
    auto* dsp = new Emulator();
    dsp->reset();
    dsp->register_sample_out_callback(on_out);
    dsp->register_external_bus_in_callback(on_in);
    dsp->register_external_bus_out_callback(on_ext);
    auto pmem = read_file(dir + "/" + prog + ".pmem.bin");
    auto cmem = read_file(dir + "/" + prog + ".cmem.bin");
    auto hdr = read_file(dir + "/" + prog + ".hdr.bin");
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
    return dsp;
}

using Seq = std::vector<int32_t>;

static double rms(const Seq& seq) {
    double sum = 0;
    for (int32_t value : seq) sum += (double)value * value;
    return seq.empty() ? 0 : sqrt(sum / seq.size());
}

// Best bit-exact match of out against ref over lags, on all, even or odd half frames.
static void report(const char* name, const Seq& out, const Seq& ref) {
    double best = 0;
    int best_lag = -1, best_parity = -1;
    for (int parity = 0; parity < 3; parity++) {
        for (int lag = 0; lag < 40; lag++) {
            long equal = 0, total = 0;
            for (size_t k = 64; k < out.size(); k++) {
                if (parity < 2 && (int)(k & 1) != parity) continue;
                total++;
                equal += out[k] == ref[k - lag];
            }
            double rate = total ? (double)equal / total : 0;
            if (rate > best + 1e-9) { best = rate; best_lag = lag; best_parity = parity; }
        }
    }
    const char* parity_name[3] = {"even", "odd", "all"};
    printf("  %-22s rms %9.0f  best exact %5.1f%% lag %d (%s)\n", name, rms(out), best * 100, best_lag,
           best_lag >= 0 ? parity_name[best_parity] : "-");
}

int main(int argc, char** argv) {
    std::string dir = argv[1];
    int frames = atoi(argv[2]);
    Emulator* dsp[5] = {load(dir, "prog0"), load(dir, "prog1"), load(dir, "prog1"), load(dir, "prog1"), load(dir, "prog4")};
    Seq sum_l, sum_r, lfo_l, lfo_r;
    Seq in4_ar1l, in4_ar1r, in4_ar2l, in4_ar2r, mout_l, mout_r, aout_l, aout_r, lfo_out_l, lfo_out_r;
    Seq stage_ax2l[4], stage_ax2r[4], stage_ax3l[4];
    auto start = std::chrono::high_resolution_clock::now();
    for (int k = 0; k < frames * 2; k++) {
        // Test tones replace the two sources the chain cannot produce alone.
        int32_t test_sum_l = (int32_t)(0.5 * sin(k * 0.031) * 0x7FFFFF);
        int32_t test_sum_r = (int32_t)(0.4 * sin(k * 0.047 + 1) * 0x7FFFFF);
        int32_t test_lfo_l = (int32_t)(0.3 * sin(k * 0.071 + 2) * 0x7FFFFF);
        int32_t test_lfo_r = (int32_t)(0.2 * sin(k * 0.113 + 3) * 0x7FFFFF);
        sum_l.push_back(test_sum_l); sum_r.push_back(test_sum_r);
        lfo_l.push_back(test_lfo_l); lfo_r.push_back(test_lfo_r);

        // Snapshot all links first so every DSP sees the previous half frame.
        int32_t ax2l[5], ax2r[5], ax3l[5], ax3r[5];
        for (int n = 0; n < 5; n++) {
            ax2l[n] = dsp[n]->AX2L.value; ax2r[n] = dsp[n]->AX2R.value;
            ax3l[n] = dsp[n]->AX3L.value; ax3r[n] = dsp[n]->AX3R.value;
        }
        dsp[0]->AR1L.value = test_lfo_l; dsp[0]->AR1R.value = test_lfo_r;
        dsp[0]->AR2L.value = 0; dsp[0]->AR2R.value = 0;
        for (int n = 1; n < 5; n++) {
            dsp[n]->AR1L.value = n == 1 ? test_sum_l : ax2l[n - 1];
            dsp[n]->AR1R.value = n == 1 ? test_sum_r : ax2r[n - 1];
            dsp[n]->AR2L.value = ax3l[n - 1];
            dsp[n]->AR2R.value = ax3r[n - 1];
        }
        for (int n = 0; n < 5; n++)
            for (int i = 0; i < 256; i++) dsp[n]->step();

        for (int n = 0; n < 4; n++) {
            stage_ax2l[n].push_back(dsp[n]->AX2L.value);
            stage_ax2r[n].push_back(dsp[n]->AX2R.value);
            stage_ax3l[n].push_back(dsp[n]->AX3L.value);
        }
        in4_ar1l.push_back(dsp[4]->AR1L.value); in4_ar1r.push_back(dsp[4]->AR1R.value);
        in4_ar2l.push_back(dsp[4]->AR2L.value); in4_ar2r.push_back(dsp[4]->AR2R.value);
        mout_l.push_back(dsp[4]->AX2L.value); mout_r.push_back(dsp[4]->AX2R.value);
        aout_l.push_back(dsp[4]->AX3L.value); aout_r.push_back(dsp[4]->AX3R.value);
        lfo_out_l.push_back(dsp[4]->AX1L.value); lfo_out_r.push_back(dsp[4]->AX1R.value);
    }
    auto stop = std::chrono::high_resolution_clock::now();
    double ns = std::chrono::duration<double, std::nano>(stop - start).count() / ((double)frames * 512 * 5);
    printf("frames %d, %.1f ns per DSP step\n", frames, ns);
    puts("summing bus, test tone into DASP1 AR1:");
    for (int n = 1; n < 4; n++) { char name[32]; sprintf(name, "DASP%d AX2L", n); report(name, stage_ax2l[n], sum_l); }
    report("DASP4 AR1L", in4_ar1l, sum_l);
    report("DASP4 AR1R", in4_ar1r, sum_r);
    puts("LFO bus, test tone into DASP0 AR1:");
    for (int n = 0; n < 4; n++) { char name[32]; sprintf(name, "DASP%d AX3L", n); report(name, stage_ax3l[n], lfo_l); }
    report("DASP4 AR2L", in4_ar2l, lfo_l);
    report("DASP4 AR2R", in4_ar2r, lfo_r);
    puts("DASP4 outputs:");
    report("MOUT L vs sum L", mout_l, sum_l);
    report("MOUT R vs sum R", mout_r, sum_r);
    report("AOUT L vs sum L", aout_l, sum_l);
    report("AOUT R vs sum R", aout_r, sum_r);
    report("AX1L vs lfo L", lfo_out_l, lfo_l);
    report("AX1R vs lfo R", lfo_out_r, lfo_r);

    puts("DASP1 AX2R mismatches against the input, lag 2, first 12 even half frames that differ:");
    int shown = 0;
    long mismatches = 0, evens = 0;
    for (size_t k = 64; k < stage_ax2r[1].size(); k += 2) {
        evens++;
        if (stage_ax2r[1][k] == sum_r[k - 2]) continue;
        mismatches++;
        if (shown++ < 12) printf("  k=%zu (mod 32 = %zu) got %06X want %06X\n", k, k % 32, stage_ax2r[1][k] & 0xFFFFFF, sum_r[k - 2] & 0xFFFFFF);
    }
    printf("  %ld of %ld differ\n", mismatches, evens);

    puts("DASP4 nonzero DMEM:");
    for (int i = 0; i < 512; i++)
        if (dsp[4]->DMEM[i].value) printf(" %03X=%06X", i, dsp[4]->DMEM[i].value & 0xFFFFFF);
    puts("");
    puts("DASP4 one frame, words writing or near the outputs:");
    for (int i = 0; i < 512; i++) {
        int pc = dsp[4]->PC.value;
        bool near_out = (pc >= 0x60 && pc <= 0x76) || (pc >= 0xFC && pc <= 0x10A);
        if (near_out)
            printf(" %03X %08X ACC1 %06X ACC2 %06X M1 %06X M2 %06X DA %03X %03X CA %03X %03X\n", pc, dsp[4]->PMEM[pc],
                   dsp[4]->ACC1.value & 0xFFFFFF, dsp[4]->ACC2.value & 0xFFFFFF,
                   dsp[4]->MACC1.getUpper().value & 0xFFFFFF, dsp[4]->MACC2.getUpper().value & 0xFFFFFF,
                   dsp[4]->DA.one.value, dsp[4]->DA.two.value, dsp[4]->CA.one.value, dsp[4]->CA.two.value);
        dsp[4]->step();
    }
    return 0;
}
