// Amplitude envelope of one note through the engine: optional pot moves, note on, note off,
// then a level per 10 ms and the times read from it.
// usage: env_test <rom dir> <note on s> <hold s> <tail s> [channel,mux,value ...]
// The pots move at 12.5 s, before the note. Prints peak, time to 10/90% of peak, the level 0.3 s
// before note off, and the time from note off to -20 and -40 dB of that level.
#include "ob12_engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    Engine e(argv[1]);
    double on = atof(argv[2]), hold = atof(argv[3]), tail = atof(argv[4]);
    struct Pot { int channel, mux, value; };
    std::vector<Pot> pots;
    for (int i = 5; i < argc; i++) {
        Pot p;
        if (sscanf(argv[i], "%d,%d,%x", &p.channel, &p.mux, &p.value) == 3) pots.push_back(p);
    }
    const int block = 480;  // 10 ms
    std::vector<int32_t> audio(block * 2);
    std::vector<double> level;  // RMS of left per 10 ms after note on
    long frames = 0, pot_at = (long)(12.5 * 48000), on_at = (long)(on * 48000), off_at = (long)((on + hold) * 48000);
    long end = (long)((on + hold + tail) * 48000);
    bool pots_sent = false, on_sent = false, off_sent = false;
    while (frames < end) {
        if (!pots_sent && frames >= pot_at) { for (auto& p : pots) e.set_pot(p.channel, p.mux, p.value); pots_sent = true; }
        if (!on_sent && frames >= on_at) { const uint8_t m[3] = {0x90, 0x3c, 0x64}; e.midi(m, 3); on_sent = true; }
        if (!off_sent && frames >= off_at) { const uint8_t m[3] = {0x80, 0x3c, 0x40}; e.midi(m, 3); off_sent = true; }
        e.run(audio.data(), block);
        if (frames >= on_at) {
            double sum = 0;
            for (int i = 0; i < block; i++) sum += (double)audio[i * 2] * audio[i * 2];
            level.push_back(sqrt(sum / block));
        }
        frames += block;
    }
    double peak = 0;
    size_t peak_i = 0;
    size_t off_i = (size_t)(hold * 100);
    for (size_t i = 0; i < off_i && i < level.size(); i++) if (level[i] > peak) { peak = level[i]; peak_i = i; }
    auto first_above = [&](double v) { for (size_t i = 0; i < off_i; i++) if (level[i] >= v) return (double)i / 100; return -1.0; };
    double sustain = level[off_i - 30];
    auto after_off_below = [&](double v) { for (size_t i = off_i; i < level.size(); i++) if (level[i] <= v) return (double)(i - off_i) / 100; return -1.0; };
    printf("peak %.0f at %.2f s, 10%% at %.2f s, 90%% at %.2f s, held %.0f (%.1f dB of peak), release -20 dB %.2f s, -40 dB %.2f s, end %.0f\n",
           peak, peak_i / 100.0, first_above(peak * 0.1), first_above(peak * 0.9), sustain, 20 * log10(sustain / peak + 1e-12),
           after_off_below(sustain * 0.1), after_off_below(sustain * 0.01), level.back());
    if (getenv("ENV_CURVE")) for (size_t i = 0; i < level.size(); i += 5) printf("%.2f %.0f\n", i / 100.0, level[i]);
    return 0;
}
