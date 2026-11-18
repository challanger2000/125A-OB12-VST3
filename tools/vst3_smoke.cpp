// Loads the bundle as a host would and plays one note through process().
// usage: vst3_smoke <path to OB12.vst3 dll>
#define NOMINMAX
#include <windows.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <fstream>
#include <cstring>
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;

struct MemoryState final : IBStream {
    std::vector<uint8_t> storage;
    size_t pos = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { if (out) *out = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API read(void* dst, int32 count, int32* done = nullptr) override {
        if (!dst || count < 0) return kInvalidArgument;
        size_t n = std::min((size_t)count, storage.size() - std::min(pos,storage.size()));
        if (n) memcpy(dst,storage.data()+pos,n);
        pos += n;
        if (done) *done = (int32)n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* src, int32 count, int32* done = nullptr) override {
        if (!src || count < 0 || (size_t)count > (2u << 20) || pos > (2u << 20) - (size_t)count)
            return kInvalidArgument;
        if (pos + count > storage.size()) storage.resize(pos + count);
        memcpy(storage.data()+pos,src,(size_t)count);
        pos += (size_t)count;
        if (done) *done = count;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset,int32 mode,int64* result=nullptr) override {
        int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? (int64)pos :
                     mode == kIBSeekEnd ? (int64)storage.size() : -1;
        if (base < 0 || offset < -base || base + offset > (2LL << 20)) return kInvalidArgument;
        pos=(size_t)(base + offset);
        if (result) *result = (int64)pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* result) override {
        if (!result) return kInvalidArgument;
        *result=(int64)pos;
        return kResultOk;
    }
};

struct Events final : IEventList {
    std::vector<Event> list;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return (int32)list.size(); }
    tresult PLUGIN_API getEvent(int32 i, Event& e) override { e = list[i]; return kResultOk; }
    tresult PLUGIN_API addEvent(Event& e) override { list.push_back(e); return kResultOk; }
};

// One parameter with one point per block, as a host sends a mapped MIDI controller.
struct Queue final : IParamValueQueue {
    ParamID id = 0;
    ParamValue value = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32, int32& offset, ParamValue& v) override { offset = 0; v = value; return kResultOk; }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kResultFalse; }
};
struct Changes final : IParameterChanges {
    std::vector<Queue> list;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return (int32)list.size(); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override { return &list[i]; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }
};

// A host frame; calls back onSize when it is that kind of host.
struct Frame final : IPlugFrame {
    bool calls_back = true;
    int requests = 0;
    HWND saved_container = nullptr;
    int last_host_width = 0; // simulate a host that caches width before attach()
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        requests++;
        if (saved_container && r->getWidth() != last_host_width) {
            SetWindowPos(saved_container, nullptr, 0, 0,
                         r->getWidth(), r->getHeight(),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            last_host_width = r->getWidth();
        }
        if (calls_back) view->onSize(r);
        return kResultOk;
    }
};

static bool test_scale(IComponent* component, MemoryState* saved) {
    IEditController* controller = nullptr;
    component->queryInterface(IEditController::iid, (void**)&controller);
    IPlugView* view = controller->createView(ViewType::kEditor);
    ViewRect size;
    view->getSize(&size);
    printf("editor opens at %d x %d\n", size.getWidth(), size.getHeight());
    HWND parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, size.getWidth(), size.getHeight(), nullptr, nullptr, nullptr, nullptr);
    Frame frame;
    view->setFrame(&frame);
    view->attached(parent, kPlatformTypeHWND);
    // Studio One can restore an outdated container height when reloading
    // an instrument: onSize must enforce the art's aspect and resize the
    // Win32 child, rather than leaving a black band below the controls.
    ViewRect wrong_height = size;
    wrong_height.bottom += 96;
    view->onSize(&wrong_height);
    ViewRect corrected_height{};
    view->getSize(&corrected_height);
    HWND sized_child = FindWindowExW(parent, nullptr, L"125A_OB12_PanelView_v2", nullptr);
    RECT sized_rect{};
    if (sized_child) GetClientRect(sized_child, &sized_rect);
    const bool editor_height_ok = sized_child &&
        corrected_height.getHeight() == size.getHeight() &&
        sized_rect.bottom == size.getHeight();
    printf("EDITOR_RELOAD_HEIGHT_TEST=%s host_height=%d corrected=%d child=%ld\n",
        editor_height_ok ? "PASS" : "FAIL",
        wrong_height.getHeight(), corrected_height.getHeight(), sized_rect.bottom);
    bool state_open_editor_ok = (saved == nullptr);
    if (saved && !saved->storage.empty()) {
        saved->pos = 0;
        state_open_editor_ok = component->setState(saved) == kResultOk;
    }
    if (saved) printf("PROJECT_RECALL_WITH_EDITOR=%s\n", state_open_editor_ok ? "PASS" : "FAIL");
    HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr);
    for (int percent : {50, 67, 200, 100}) {
        frame.calls_back = percent != 67;
        SendMessageW(child, WM_COMMAND, 1000 + percent, 0);
        RECT rc;
        GetClientRect(child, &rc);
        view->getSize(&size);
        printf("%d%%%s: view %d x %d, window %ld x %ld\n", percent, frame.calls_back ? "" : " (host silent)", size.getWidth(), size.getHeight(), rc.right, rc.bottom);
    }
    if (const char* p = getenv("OB12_PANEL_SHOT_PERCENT")) SendMessageW(child, WM_COMMAND, 1000 + atoi(p), 0);
    if (getenv("OB12_PANEL_SHOT")) SendMessageW(child, WM_COMMAND, 1999, 0);  // the panel saves itself, off screen
    SendMessageW(child, WM_COMMAND, 1000 + 75, 0);
    // Open 125A's real browser through the original panel's right-click menu handler.
    // Test the actual Win32 controls rather than a mocked list or a screenshot.
    bool browser_ok = false;
    SendMessageW(child, WM_COMMAND, MAKEWPARAM(1503, 0), 0);
    HWND browser = FindWindowW(L"125A_OB12_PresetBrowser_v2", nullptr);
    if (browser) {
        HWND ty = GetDlgItem(browser, 3101);
        HWND bk = GetDlgItem(browser, 3102);
        HWND list = GetDlgItem(browser, 3104);
        HWND library = GetDlgItem(browser, 3110);
        wchar_t program_label[256] = {};
        if (list) SendMessageW(list, LB_GETTEXT, 0, (LPARAM)program_label);
        bool factory_program_ok = wcsstr(program_label, L"OB Fat Pad") != nullptr;
        int initial = list ? (int)SendMessageW(list, LB_GETCOUNT, 0, 0) : -1;
        int types = ty ? (int)SendMessageW(ty, CB_GETCOUNT, 0, 0) : -1;
        int banks = bk ? (int)SendMessageW(bk, CB_GETCOUNT, 0, 0) : -1;
        int collections = library ? (int)SendMessageW(library, LB_GETCOUNT, 0, 0) : -1;
        // An alternate complete bank is selected from the dropdown. It must
        // immediately change the real private flash and let notes be played:
        // no preview-only state and no Load Bank Pair button click.
        // All library collections are visible without opening a dropdown.
        // The list control's client height must show >= 7 rows at 17px.
        RECT library_bounds{};
        if (library) GetClientRect(library, &library_bounds);
        const bool sidebar_visible = library && IsWindowVisible(library) &&
             library_bounds.bottom - library_bounds.top >= 7 * 17;
        bool library_switch_ok = false;
        int alternate = -1;
        for (int row = 1; library && row < collections; ++row) {
            wchar_t name[512] = {};
            SendMessageW(library, LB_GETTEXT, row, (LPARAM)name);
            if (wcscmp(name, L"UI") == 0 || wcsncmp(name, L"UI ", 3) == 0) { alternate = row; break; }
        }
        if (alternate >= 1 && list) {
            SendMessageW(library, LB_SETCURSEL, alternate, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3110, LBN_SELCHANGE),
                         (LPARAM)library);
            wchar_t in_other_bank[256] = {};
            SendMessageW(list, LB_GETTEXT, 0, (LPARAM)in_other_bank);
            MemoryState switched_state;
            const bool has_state = component->getState(&switched_state) == kResultOk &&
                                   switched_state.storage.size() == 0x100000 + 24;
            const bool flash_changed = has_state &&
                memcmp(switched_state.storage.data() + 24 + 0xD0000, "UI Browser", 10) == 0;
            const bool selectable = IsWindowEnabled(list) &&
                wcsstr(in_other_bank, L"UI Browser") != nullptr &&
                SendMessageW(list, LB_GETCOUNT, 0, 0) == 128;
            SendMessageW(library, LB_SETCURSEL, 0, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3110, LBN_SELCHANGE),
                         (LPARAM)library);
            wchar_t restored_label[256] = {};
            SendMessageW(list, LB_GETTEXT, 0, (LPARAM)restored_label);
            MemoryState restored_state;
            const bool restore_data = component->getState(&restored_state) == kResultOk &&
                restored_state.storage.size() == 0x100000 + 24 &&
                memcmp(restored_state.storage.data() + 24 + 0xD0000, "OB Fat Pad", 10) == 0;
            library_switch_ok = sidebar_visible && flash_changed && selectable && restore_data &&
                                wcsstr(restored_label, L"OB Fat Pad") != nullptr;
            printf("LIBRARY_SWITCH_TEST=%s direct_playable=%d flash_changed=%d project_restore=%d\n",
                   library_switch_ok?"PASS":"FAIL",selectable,flash_changed,restore_data);
        } else puts("LIBRARY_SWITCH_TEST=FAIL: valid alternate bank not found");

        if (ty) {
            SendMessageW(ty, CB_SETCURSEL, 1, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3101, CBN_SELCHANGE), (LPARAM)ty);
        }
        wchar_t timbre_label[256] = {};
        if (list) SendMessageW(list, LB_GETTEXT, 0, (LPARAM)timbre_label);
        bool factory_timbre_ok = wcsstr(timbre_label, L"OldStyle Pad") != nullptr;
        if (bk) {
            SendMessageW(bk, CB_SETCURSEL, 1, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3102, CBN_SELCHANGE), (LPARAM)bk);
        }
        if (ty) {
            SendMessageW(ty, CB_SETCURSEL, 1, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3101, CBN_SELCHANGE), (LPARAM)ty);
        }
        int after = list ? (int)SendMessageW(list, LB_GETCOUNT, 0, 0) : -1;
        if (list && after == 128) {
            SendMessageW(list, LB_SETCURSEL, 6, 0);
            SendMessageW(browser, WM_COMMAND, MAKEWPARAM(3104, LBN_SELCHANGE), (LPARAM)list);
        }
        wchar_t status[160] = {};
        if (HWND label = GetDlgItem(browser, 3109)) GetWindowTextW(label, status, 160);
        browser_ok = initial == 128 && after == 128 && types == 2 && banks == 2 &&
                     collections >= 4 && library_switch_ok &&
                     factory_program_ok && factory_timbre_ok &&
                     wcsstr(status, L"Timbre") != nullptr;
        printf("BROWSER_TEST=%s slots=%d/%d types=%d banks=%d soundbank_options=%d names=%s/%s\n",
               browser_ok ? "PASS" : "FAIL", initial, after, types, banks, collections,
               factory_program_ok ? "PASS" : "FAIL", factory_timbre_ok ? "PASS" : "FAIL");
        DestroyWindow(browser);
    } else {
        puts("BROWSER_TEST=FAIL (window not created)");
    }
    view->removed();
    view->release();
    IPlugView* again = controller->createView(ViewType::kEditor);
    again->getSize(&size);
    printf("reopens at %d x %d, %d requests\n", size.getWidth(), size.getHeight(), frame.requests);
    again->release();
    DestroyWindow(parent);
    controller->release();
    return browser_ok && state_open_editor_ok && editor_height_ok;
}

// Regression for Studio One: remove an instrument, reinstantiate it,
// unload/reload the DLL, then open the native browser again.  A host-owned
// registered WNDPROC formerly remained after FreeLibrary, jumping to the old DLL.
static bool test_second_plugin_instance(const char* dll_path) {
    for (int cycle = 0; cycle < 3; ++cycle) {
        HMODULE dll = LoadLibraryA(dll_path);
        if (!dll) { puts("INSTANCE_RELOAD_TEST=FAIL LoadLibrary"); return false; }
        auto get = (IPluginFactory * (PLUGIN_API*)())GetProcAddress(dll, "GetPluginFactory");
        if (!get) { FreeLibrary(dll); return false; }
        IPluginFactory* factory = get();
        PClassInfo info{};
        bool ok = factory->getClassInfo(0, &info) == kResultOk;
        IComponent* component = nullptr;
        if (ok) ok = factory->createInstance(info.cid, IComponent::iid, (void**)&component) == kResultOk && component;
        if (ok) ok = component->initialize(nullptr) == kResultOk;
        IEditController* controller = nullptr;
        IPlugView* view = nullptr;
        HWND parent = nullptr;
        if (ok) {
            component->queryInterface(IEditController::iid, (void**)&controller);
            ok = controller != nullptr;
        }
        if (ok) {
            view = controller->createView(ViewType::kEditor);
            ok = view != nullptr;
        }
        ViewRect wanted{};
        if (ok) {
            view->getSize(&wanted);
            parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0,
                                   wanted.getWidth(), wanted.getHeight(),
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ok = parent && view->attached(parent, kPlatformTypeHWND) == kResultOk;
        }
        if (ok) {
            HWND panel = FindWindowExW(parent, nullptr, L"125A_OB12_PanelView_v2", nullptr);
            if (!panel) ok = false;
            else {
                SendMessageW(panel, WM_COMMAND, MAKEWPARAM(1503,0), 0);
                HWND browser = FindWindowW(L"125A_OB12_PresetBrowser_v2", nullptr);
                ok = browser && GetDlgItem(browser, 3104) && GetDlgItem(browser, 3110);
                if (browser) DestroyWindow(browser);
            }
        }
        if (view) { view->removed(); view->release(); }
        if (parent) DestroyWindow(parent);
        if (controller) controller->release();
        if (component) { component->terminate(); component->release(); }
        FreeLibrary(dll);
        printf("INSTANCE_RELOAD_CYCLE_%d=%s\n", cycle + 1, ok ? "PASS" : "FAIL");
        if (!ok) return false;
    }
    puts("INSTANCE_RELOAD_TEST=PASS iterations=3 native_panel_and_browser_after_dll_unload");
    return true;
}


static bool test_studio_one_delayed_frame(IComponent* component) {
    IEditController* controller = nullptr;
    if (component->queryInterface(IEditController::iid, (void**)&controller) != kResultOk || !controller) return false;
    IPlugView* view = controller->createView(ViewType::kEditor);
    if (!view) { controller->release(); return false; }
    ViewRect expected{};
    view->getSize(&expected);
    const int width = expected.getWidth(), height = expected.getHeight();
    // WS_POPUP makes the window's client area exactly the requested dimensions.
    HWND host = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, width, height + 96,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Frame frame;
    frame.saved_container = host;
    frame.last_host_width = width; // host ignores same-width requests
    view->setFrame(&frame);
    bool ok = host && view->attached(host, kPlatformTypeHWND) == kResultOk;
    RECT before{};
    GetClientRect(host, &before);
    ok = ok && before.bottom == height + 96;
    HWND panel = host ? FindWindowExW(host, nullptr, L"125A_OB12_PanelView_v2", nullptr) : nullptr;
    ok = ok && panel != nullptr;
    if (panel) {
        // Explicit message injection: test does not depend on the runner's
        // foreground window or real timer scheduling.
        for (int tick = 0; tick < 3; ++tick)
            SendMessageW(panel, WM_TIMER, 1, 0);
    }
    RECT after{}, child{};
    if (host) GetClientRect(host, &after);
    if (panel) GetClientRect(panel, &child);
    ViewRect returned{};
    view->getSize(&returned);
    ok = ok && after.right == width && after.bottom == height &&
         child.right == width && child.bottom == height &&
         returned.getWidth() == width && returned.getHeight() == height &&
         frame.requests >= 3;
    printf("STUDIO_ONE_DEFERRED_FRAME_TEST=%s before=%ldx%ld after=%ldx%ld panel=%ldx%ld resize_calls=%d\n",
           ok?"PASS":"FAIL",before.right,before.bottom,after.right,after.bottom,
           child.right,child.bottom,frame.requests);
    view->removed();
    view->release();
    if (host) DestroyWindow(host);
    controller->release();
    return ok;
}

int main(int, char** argv) {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    SetEnvironmentVariableA("LOCALAPPDATA", temp);  // keep the user's saved size out of this
    const std::string probe_path=std::string(temp)+"\\p007-midi-offsets.txt";
    SetEnvironmentVariableA("OB12_MIDI_PROBE", probe_path.c_str());
    // Test discovery of an external matching soundbank pair without writing to firmware.
    std::string user_library = std::string(temp) + "\\OB12";
    CreateDirectoryA(user_library.c_str(), nullptr);
    user_library += "\\SoundBanks";
    CreateDirectoryA(user_library.c_str(), nullptr);
    { std::ofstream(user_library + "\\12prgTT.mid", std::ios::binary).put('X'); }
    { std::ofstream(user_library + "\\12tmbTT.mid", std::ios::binary).put('X'); }
    { std::ofstream(user_library + "\\12prgVA2.mid", std::ios::binary).put('X'); }
    { std::ofstream(user_library + "\\12tmbVA2.mid", std::ios::binary).put('X'); }
    // A real, checksum-valid MIDI bank generated from original factory
    // firmware data with a distinct first patch name for this test only.
    if (!CopyFileA("build\\bank-tests\\12prgUI.mid",
                   (user_library + "\\12prgUI.mid").c_str(), FALSE) ||
        !CopyFileA("build\\bank-tests\\12tmbUI.mid",
                   (user_library + "\\12tmbUI.mid").c_str(), FALSE)) {
        puts("LIBRARY_FIXTURE_TEST=FAIL: missing generated bank");
        return 1;
    }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { puts("load failed"); return 1; }
    auto get = (IPluginFactory * (PLUGIN_API*)())GetProcAddress(dll, "GetPluginFactory");
    IPluginFactory* factory = get();
    PClassInfo info;
    factory->getClassInfo(0, &info);
    printf("class %s\n", info.name);
    IComponent* component = nullptr;
    factory->createInstance(info.cid, IComponent::iid, (void**)&component);
    component->initialize(nullptr);
    IAudioProcessor* audio = nullptr;
    component->queryInterface(IAudioProcessor::iid, (void**)&audio);
    ProcessSetup setup{kRealtime, kSample32, 512, 44100};  // not the machine's rate, on purpose
    audio->setupProcessing(setup);
    component->setActive(true);
    audio->setProcessing(true);

    std::vector<float> l(512), r(512);
    float* channels[2] = {l.data(), r.data()};
    AudioBusBuffers bus{};
    bus.numChannels = 2;
    bus.channelBuffers32 = channels;
    Events events;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;
    data.inputEvents = &events;
    Changes changes;
    data.inputParameterChanges = &changes;
    // Pitch bend through the controller's MIDI mapping, as a host finds it.
    ParamID bend_id = 0;
    bool mapped = false;
    {
        IEditController* controller = nullptr;
        component->queryInterface(IEditController::iid, (void**)&controller);
        IMidiMapping* mapping = nullptr;
        if (controller && controller->queryInterface(IMidiMapping::iid, (void**)&mapping) == kResultOk && mapping) {
            mapped = mapping->getMidiControllerAssignment(0, 0, kPitchBend, bend_id) == kResultTrue;
            mapping->release();
        }
        printf("pitch bend mapping: %s, id %u, parameters %d\n", mapped ? "yes" : "no", bend_id, controller ? controller->getParameterCount() : -1);
        if (controller) controller->release();
    }
    std::vector<int16_t> recording;

    // Blocks are paced by the wall clock, as a sound card would.
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    auto seconds = [&]() { QueryPerformanceCounter(&now); return (double)(now.QuadPart - start.QuadPart) / freq.QuadPart; };
    double block = 512 / 44100.0, next = 0, sum = 0, first_sound = -1;
    long frames = 0, zero_blocks = 0, blocks = 0;
    bool sent = false;
    while (next < 17) {
        while (seconds() < next) Sleep(1);
        next += block;
        events.list.clear();
        changes.list.clear();
        static bool bent = false;
        if (mapped && !bent && next >= 14.5) { Queue q; q.id = bend_id; q.value = 1.0; changes.list.push_back(q); bent = true; }
        if (!sent && next >= 11) {
            Event e{};
            e.type = Event::kNoteOnEvent;
            e.noteOn.pitch = 72;
            e.noteOn.velocity = 0.8f;
            events.list.push_back(e);
            sent = true;
        }
        audio->process(data);
        if (next > 12.5 && next < 16) for (float v : l) recording.push_back((int16_t)std::max(-32767.f, std::min(32767.f, v * 32767)));
        double s = 0;
        for (float v : l) s += v * v;
        if (s > 0 && first_sound < 0) first_sound = next;
        { static int nz = 0, n = 0; n++; if (s > 0) nz++; if (n == 43) { printf("%.1f:%d ", next, nz); n = nz = 0; } }
        if (next > 13) {
            sum += s;
            frames += 512;
            blocks++;
            if (s == 0) zero_blocks++;
        }
    }
    printf("first sound at %.2f s; rms %.4f over %ld frames, %ld of %ld blocks empty\n", first_sound, sqrt(sum / frames), frames, zero_blocks, blocks);
    if (const char* name = getenv("OB12_SMOKE_WAV")) {  // mono, 44.1 kHz, 12.5 to 16 s; the bend comes at 14.5 s
        std::ofstream f(name, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
        auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
        uint32_t bytes = (uint32_t)recording.size() * 2;
        f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
        f.write("data", 4); u32(bytes); f.write((const char*)recording.data(), bytes);
    }
    const bool audio_ok = first_sound > 10.0 && first_sound < 17.0 && frames > 0 && sum / frames > 1e-9;
    printf("AUDIO_TEST=%s\n", audio_ok ? "PASS" : "FAIL");
    // First choose Timbre B-007 through the actual browser while the machine
    // is fully booted. Then ensure the exact selection is serialized/recalled.
    const bool preselect_browser_ok = test_scale(component, nullptr);
    MemoryState state;
    const tresult state_result = component->getState(&state);
    bool state_ok = state_result == kResultOk && state.storage.size() == 0x100000 + 24;
    int32_t captured_program = -1;
    if (state_ok) memcpy(&captured_program, state.storage.data() + 20, sizeof captured_program);
    state_ok = state_ok && captured_program == 134; // Timbre B-007, index 128+6
    printf("PROJECT_SELECTED_PRESET=%d expected=134\n", captured_program);
    if (state_ok) {
        state.pos = 0;
        state_ok = component->setState(&state) == kResultOk;
    }
    printf("PROJECT_STATE_TEST=%s bytes=%zu\n", state_ok ? "PASS" : "FAIL", state.storage.size());
    const bool browser_ok = test_scale(component, &state);
    const bool deferred_frame_ok = test_studio_one_delayed_frame(component);

    // P007 independent host-boundary regression: a single raw channel event may
    // suppress the host's mirrored parameter mapping in THAT block, but never
    // disable mapped CCs on subsequent callbacks.
    // The original sticky raw_channel_messages field fails this test (263 vs 7).
    const bool midi_bridge_ok = [&]() -> bool {
        auto raw = [&](const uint8_t* bytes, uint32_t size) {
            Event e{};
            e.type = Event::kDataEvent;
            e.data.bytes = bytes;
            e.data.size = size;
            events.list.push_back(e);
        };
        auto selected = [&]() -> int32_t {
            MemoryState state;
            if (component->getState(&state) != kResultOk ||
                state.storage.size() != 0x100000 + 24) return -100;
            int32_t slot = -100;
            std::memcpy(&slot, state.storage.data() + 20, sizeof slot);
            return slot;
        };
        const uint8_t bank_program[] = {0xB0,0x00,0x40};
        const uint8_t bank_slot[] = {0xB0,0x20,0x00};
        const uint8_t program[] = {0xC0,0x07};
        changes.list.clear();
        events.list.clear();
        raw(bank_program, 3); raw(bank_slot, 3); raw(program, 2);
        const bool injected = audio->process(data) == kResultOk;
        const int32_t before = selected();  // Program A-008 => 256 + 7
        events.list.clear();
        changes.list.clear();
        Queue msb; msb.id = 0; msb.value = 0.0;
        Queue lsb; lsb.id = 32; lsb.value = 0.0;
        changes.list.push_back(msb); changes.list.push_back(lsb);
        const bool mapped_ok = audio->process(data) == kResultOk;
        changes.list.clear();
        events.list.clear();
        raw(program, 2);
        const bool after_ok = audio->process(data) == kResultOk;
        const int32_t after = selected();  // Timbre A-008 => 7
        changes.list.clear();
        events.list.clear();
        const bool ok = injected && mapped_ok && after_ok &&
                        before == 263 && after == 7;
        std::printf("P007_MIDI_RAW_THEN_MAPPED_CC=%s first=%d last=%d\n",
                    ok ? "PASS" : "FAIL", (int)before, (int)after);
        return ok;
    }();
    // A separate timed native MIDI ingress probe follows the ordinary VST3
    // project and browser tests; it checks the true feeder's 48-kHz dispatch.
    const bool timing_ok = [&]() -> bool {
        events.list.clear();
        changes.list.clear();
        Event first{};
        first.type=Event::kNoteOnEvent; first.sampleOffset=64;
        first.noteOn.pitch=59; first.noteOn.velocity=0.7f;
        Event second{};
        second.type=Event::kNoteOnEvent; second.sampleOffset=320;
        second.noteOn.pitch=61; second.noteOn.velocity=0.7f;
        events.list.push_back(first); events.list.push_back(second);
        bool calls = audio->process(data) == kResultOk;
        events.list.clear();
        for (int n=0;n<16;++n) {
            calls = calls && audio->process(data)==kResultOk;
            Sleep(4);
        }
        std::ifstream probe(probe_path);
        unsigned long long targets[2]={}, actuals[2]={};
        bool seen[2]={};
        std::string line;
        while (std::getline(probe,line)) {
            unsigned long long target=0,actual=0;
            unsigned status=0,pitch=0;
            if (std::sscanf(line.c_str(),
                  "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u",
                  &target,&actual,&status,&pitch)!=4) continue;
            const int slot = pitch==59 ? 0 : pitch==61 ? 1 : -1;
            if (slot<0 || status!=0x90) continue;
            targets[slot]=target; actuals[slot]=actual; seen[slot]=true;
        }
        const long long separation =
            std::llround(320.0*48000.0/44100.0) -
            std::llround(64.0*48000.0/44100.0);
        const bool ok=calls && seen[0] && seen[1] &&
            targets[0]==actuals[0] && targets[1]==actuals[1] &&
            (long long)(actuals[1]-actuals[0])==separation;
        std::printf("P007_MIDI_OFFSET_DISPATCH=%s diff=%lld expected=%lld\\n",
                    ok?"PASS":"FAIL",
                    seen[0]&&seen[1]?(long long)(actuals[1]-actuals[0]):-1LL,
                    separation);
        return ok;
    }();
    audio->setProcessing(false);
    component->setActive(false);
    component->terminate();
    component->release();
    FreeLibrary(dll);
    SetEnvironmentVariableA("OB12_MIDI_PROBE", nullptr);
    const bool reload_ok = test_second_plugin_instance(argv[1]);
    return audio_ok && preselect_browser_ok && state_ok && browser_ok &&
           deferred_frame_ok && midi_bridge_ok && timing_ok && reload_ok ? 0 : 2;
}
