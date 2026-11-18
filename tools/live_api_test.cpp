// The live control interface, end to end, offline: Master Volume by pot, BUS by keys and encoder.
// usage: live_api_test <rom dir>
#include "ob12_engine.h"
#include <cmath>
#include <cstdio>
#include <vector>

static double rms(Engine& e, double seconds) {
    int n = (int)(seconds * 48000);
    std::vector<int32_t> audio(n * 2 + 4);
    e.run(audio.data(), n);
    double sum = 0;
    for (int i = 0; i < n * 2; i++) sum += (double)audio[i] * audio[i];
    return sqrt(sum / (n * 2));
}

int main(int, char** argv) {
    Engine e(argv[1]);
    rms(e, 12);
    const uint8_t on[3] = {0x90, 0x3c, 0x64}, off[3] = {0x80, 0x3c, 0x40};
    e.midi(on, 3);
    rms(e, 1.2);
    printf("note, start volume: rms %.0f\n", rms(e, 0.3));
    e.set_pot(7, 2, 0x40);
    rms(e, 0.3);
    printf("Master Volume pot 40: rms %.0f\n", rms(e, 0.3));
    e.set_pot(7, 2, 0xff);
    rms(e, 0.3);
    printf("Master Volume pot FF: rms %.0f\n", rms(e, 0.3));
    e.midi(off, 3);
    rms(e, 3);
    auto press = [&](int bar, int bit) { e.key(bar, bit, true); rms(e, 0.2); e.key(bar, bit, false); rms(e, 0.6); };
    press(10, 3);  // PROGRAM
    press(9, 4);   // F4, EDIT
    press(6, 2);   // 2, BUS SETTINGS
    e.turn(3);     // AUX STEREO
    rms(e, 0.5);
    e.midi(on, 3);
    rms(e, 1.2);
    printf("BUS AUX STEREO by keys and encoder, MOUT: rms %.0f\n", rms(e, 0.3));
    uint8_t bitmap[30 * 64];
    int lit = 0;
    if (e.lcd(bitmap)) for (uint8_t b : bitmap) lit += __builtin_popcount(b);
    printf("LCD pixels on: %d\n", lit);
    return 0;
}
