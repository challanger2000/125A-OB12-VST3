// CPU load at real time. Boots flat out, then feeds a ring against the wall clock the way the
// plugin does, with notes held, and reports process CPU per second of sound and how late it got.
// usage: pace_test <rom dir> [seconds]
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include "ob12_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#pragma comment(lib, "winmm.lib")

static double process_cpu() {
    FILETIME a, b, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &kernel, &user);
    auto v = [](FILETIME t) { return (((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime) / 1e7; };
    return v(kernel) + v(user);
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    Engine e(argv[1]);
    double seconds = argc > 2 ? atof(argv[2]) : 20;
    std::vector<int32_t> block(240 * 2);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    auto wall = [&]() { QueryPerformanceCounter(&now); return (double)(now.QuadPart - start.QuadPart) / freq.QuadPart; };
    while (!e.sounding_ready()) e.run(block.data(), 240);
    double boot = wall();
    for (int i = 0; i < 400; i++) e.run(block.data(), 240);  // the Master finishes its start-up
    const uint8_t notes[][3] = {{0x90, 0x30, 0x64}, {0x90, 0x3c, 0x64}, {0x90, 0x40, 0x64}, {0x90, 0x43, 0x64}, {0x90, 0x48, 0x64}, {0x90, 0x4c, 0x64}};
    for (auto& n : notes) e.midi(n, 3);

    double t0 = wall(), cpu0 = process_cpu(), produced = 0, late = 0;
    const double lead = 0.048;  // the ring of the plugin
    long blocks = 0, sleeps = 0, over = 0;
    while (wall() - t0 < seconds) {
        double played = wall() - t0;
        double fill = lead + produced - played;
        if (fill < 0) { over++; late = std::max(late, -fill); produced = played - lead; }
        if (fill >= lead) { Sleep(1); sleeps++; continue; }
        e.run(block.data(), 240);
        produced += 0.005;
        blocks++;
    }
    double used = process_cpu() - cpu0, span = wall() - t0;
    printf("boot %.2f s; %.1f s at real time: %.2f cores busy, %ld blocks, %ld sleeps, %ld ring empties, worst %.1f ms\n",
           boot, span, used / span, blocks, sleeps, over, late * 1000);
    return 0;
}
