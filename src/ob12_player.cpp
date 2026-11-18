// Standalone OB-12: the panel in a window, sound on the default device, notes from a MIDI input.
// usage: ob12_player [MIDI input number]. OB12_ROM names the ROM folder, default "rom".
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include "live_io.h"
#include "ob12_engine.h"
#include "panel_view.h"

static PanelView* view = nullptr;
static float art_aspect = 3.75f;

static LRESULT CALLBACK frame_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (view) view->resize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int main(int argc, char** argv) {
    SetProcessDPIAware();
    const char* rom_env = getenv("OB12_ROM");
    std::string rom = rom_env ? rom_env : "rom";
    Engine engine(rom);
    view = new PanelView(engine, rom + "/panel.ob12p");
    art_aspect = view->aspect();

    if (!live_audio_open(6, 480, getenv("OB12_LIVE_SILENT") != nullptr)) { puts("no sound device"); return 1; }
    bool midi = false;
    for (int d = 0; d < live_midi_devices(); d++) printf("MIDI in %d: %s\n", d, live_midi_name(d));
    if (argc > 1) midi = live_midi_open(atoi(argv[1]));

    std::atomic<bool> stop{false};
    std::thread audio([&]() {
        std::vector<int32_t> frames(480 * 2);
        std::vector<int16_t> block(480 * 2);
        uint8_t bytes[64];
        while (!stop) {
            if (midi) {
                int n = live_midi_read(bytes, sizeof bytes);
                if (n) engine.midi(bytes, n);
            }
            engine.run(frames.data(), 480);
            for (int i = 0; i < 960; i++) block[i] = (int16_t)(frames[i] >> 8);
            if (engine.sounding_ready()) live_audio_write(block.data());
        }
    });

    WNDCLASSW wc = {};
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"OB12Player";
    RegisterClassW(&wc);
    int width = 1500, height = (int)(width / art_aspect);
    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND frame = CreateWindowW(L"OB12Player", L"OB-12", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    view->attach(frame);
    view->on_width_request([frame](int w) {
        RECT r = {0, 0, w, (int)(w / art_aspect)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(frame, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    });
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    stop = true;
    audio.join();
    printf("buffers %ld, underruns %ld\n", live_audio_buffers_sent(), live_audio_underruns());
    live_midi_close();
    live_audio_close();
    delete view;
    return 0;
}
