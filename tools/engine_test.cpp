// The engine against ob12_system: same notes, same frames, written as the same WAV.
// usage: engine_test <rom dir> <seconds> <out.wav>
#include "ob12_engine.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    Engine engine(argv[1]);
    double seconds = atof(argv[2]);
    long frames = (long)(seconds * 48000);
    std::vector<int32_t> audio(frames * 2 + 4);
    const uint8_t notes[3][3] = {{0x90, 0x3c, 0x64}, {0x90, 0x40, 0x64}, {0x90, 0x43, 0x64}};
    double at[3] = {13, 13.5, 14};  // notes play from about 12.2 s, after the boot
    long done = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i <= 3; i++) {
        long until = i < 3 ? (long)(at[i] * 48000) - 24 : frames;  // inputs land on the next millisecond
        engine.run(audio.data() + done * 2, (int)(until - done));
        done = until;
        if (i < 3) engine.midi(notes[i], 3);
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%.2f s of sound in %.2f s, %.2f x, ready %d\n", seconds, wall, seconds / wall, engine.sounding_ready());
    std::ofstream f(argv[3], std::ios::binary);
    auto u32le = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16le = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t bytes = (uint32_t)(frames * 4);
    f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
    u32le(48000); u32le(48000 * 4); u16le(4); u16le(16);
    f.write("data", 4); u32le(bytes);
    for (long i = 0; i < frames * 2; i++) u16le((uint16_t)(int16_t)(audio[i] >> 8));
    return 0;
}
