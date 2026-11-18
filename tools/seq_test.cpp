// A timed event list through the engine, MOUT to a WAV.
// usage: seq_test <rom dir> <seconds> <out.wav> event...
// events: t:on:nn  t:off:nn  (note hex, velocity 64)  t:enc:detents  t:pot:channel:mux:value(hex)
//         t:key:bar:bit  (panel key down, up 80 ms later)  t:dump  (print time and the LED latches)
//         t:midi:hexbytes  (any MIDI message, e.g. 2.0:midi:E07F7F pitch bend up)
#include "ob12_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Event { long frame; int kind, a, b, c; uint8_t bytes[256]; int count; };  // kind 0 on, 1 off, 2 encoder, 3 pot, 4 key down, 5 key up

int main(int argc, char** argv) {
    Engine e(argv[1]);
    if (const char* path = getenv("SEQ_FLASH")) e.set_flash_file(path);  // the unit's memory
    double seconds = atof(argv[2]);
    std::vector<Event> events;
    std::vector<std::string> syx_files;
    for (int i = 4; i < argc; i++) {
        double t; char kind[8]; int a = 0, b = 0, c = 0;
        if (sscanf(argv[i], "%lf:%7[a-z]:%x:%d:%x", &t, kind, &a, &b, &c) < 2) continue;
        Event ev{(long)(t * 48000), 0, a, b, c};
        if (!strcmp(kind, "off")) ev.kind = 1;
        else if (!strcmp(kind, "enc")) { ev.kind = 2; sscanf(argv[i], "%lf:%7[a-z]:%d", &t, kind, &ev.a); }
        else if (!strcmp(kind, "pot")) { ev.kind = 3; sscanf(argv[i], "%lf:%7[a-z]:%d:%d:%x", &t, kind, &ev.a, &ev.b, &ev.c); }
        else if (!strcmp(kind, "dump")) ev.kind = 6;
        else if (!strcmp(kind, "syx")) {  // t:syx:file  a whole .syx onto MIDI IN
            ev.kind = 8;
            syx_files.push_back(strchr(strchr(argv[i], ':') + 1, ':') + 1);
            ev.a = (int)syx_files.size() - 1;
        }
        else if (!strcmp(kind, "midi")) {
            ev.kind = 7;
            const char* hex = strchr(strchr(argv[i], ':') + 1, ':') + 1;
            ev.count = 0;
            while (ev.count < 256 && hex[0] && hex[1]) { unsigned v; sscanf(hex, "%2x", &v); ev.bytes[ev.count++] = (uint8_t)v; hex += 2; }
        }
        else if (!strcmp(kind, "key")) {
            ev.kind = 4; sscanf(argv[i], "%lf:%7[a-z]:%d:%d", &t, kind, &ev.a, &ev.b);
            Event up = ev; up.kind = 5; up.frame += 48000 * 8 / 100;
            events.push_back(up);
        }
        events.push_back(ev);
    }
    std::sort(events.begin(), events.end(), [](const Event& x, const Event& y) { return x.frame < y.frame; });
    long frames = (long)(seconds * 48000), done = 0;
    std::vector<int32_t> audio(frames * 2 + 960);
    size_t next = 0;
    bool clock = getenv("SEQ_CLOCK") != nullptr;  // MIDI clock every 21 ms from 1 s, as VSTHost sends
    long clock_next = 48000;
    while (done < frames) {
        if (clock && done >= clock_next) { uint8_t f8 = 0xf8; e.midi(&f8, 1); clock_next += 1008; }
        while (next < events.size() && events[next].frame <= done) {
            const Event& ev = events[next++];
            if (ev.kind == 0) { uint8_t m[3] = {0x90, (uint8_t)ev.a, 0x64}; e.midi(m, 3); }
            else if (ev.kind == 1) { uint8_t m[3] = {0x80, (uint8_t)ev.a, 0x40}; e.midi(m, 3); }
            else if (ev.kind == 2) e.turn(ev.a);
            else if (ev.kind == 3) e.set_pot(ev.a, ev.b, ev.c);
            else if (ev.kind == 7) e.midi(ev.bytes, ev.count);
            else if (ev.kind == 8) {
                std::vector<uint8_t> data;  // through the plug-in's own reader (.syx or .mid)
                std::string path = syx_files[ev.a];
                std::string problem = Engine::read_sysex(std::wstring(path.begin(), path.end()), data);
                if (!problem.empty()) fprintf(stderr, "read_sysex: %s\n", problem.c_str());
                fprintf(stderr, "syx %s: %zu bytes at %.2f s\n", syx_files[ev.a].c_str(), data.size(), ev.frame / 48000.0);
                e.send_sysex(data.data(), (int)data.size());
            }
            else if (ev.kind == 6) { uint8_t l[16]; e.leds(l); printf("leds %.2f flash %ld sysex %d", ev.frame / 48000.0, e.flash_writes(), e.sysex_waiting()); for (int j = 0; j < 16; j++) printf(" %02X", l[j]); printf("|"); }
            else e.key(ev.a, ev.b, ev.kind == 4);
        }
        int n = (int)std::min<long>(240, frames - done);
        e.run(audio.data() + done * 2, n);
        done += n;
    }
    if (const char* path = getenv("SEQ_DUMP")) {  // what MIDI OUT sent as system exclusive
        std::vector<uint8_t> dump;
        e.sysex_sent(&dump);
        std::ofstream(path, std::ios::binary).write((const char*)dump.data(), dump.size());
        fprintf(stderr, "dump %zu bytes\n", dump.size());
    }
    fprintf(stderr, "sysex messages left %d\n", e.sysex_waiting());
    if (getenv("SEQ_FLASH")) fprintf(stderr, "flash writes %ld\n", e.flash_writes());
    if (getenv("SEQ_LEDS")) {  // the LED latches at the end, hex
        uint8_t l[16];
        e.leds(l);
        for (int i = 0; i < 16; i++) printf("%02X ", l[i]);
        printf("\n");
    }
    if (const char* path = getenv("SEQ_LCD")) {  // the display at the end, 240 x 64 PBM
        uint8_t bitmap[30 * 64];
        if (e.lcd(bitmap)) { std::ofstream p(path, std::ios::binary); p << "P4\n240 64\n"; p.write((const char*)bitmap, sizeof bitmap); }
    }
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
