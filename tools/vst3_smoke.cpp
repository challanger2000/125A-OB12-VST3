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
#include <string>
#include <cstring>
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
    int32 virtual_count=-1;
    int32 accessed=0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override {
        return virtual_count>=0 ? virtual_count : (int32)list.size();
    }
    tresult PLUGIN_API getEvent(int32 i, Event& e) override {
        ++accessed;
        if (i<0 || (size_t)i>=list.size()) return kInvalidArgument;
        e=list[(size_t)i];return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) override { list.push_back(e); return kResultOk; }
};

// One parameter with one point per block, as a host sends a mapped MIDI controller.
struct Queue final : IParamValueQueue {
    ParamID id = 0;
    ParamValue value = 0;
    int32 sample_offset = 0;
    int32 virtual_point_count=-1;
    int32 point_reads=0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override {
        return virtual_point_count>=0 ? virtual_point_count : 1;
    }
    tresult PLUGIN_API getPoint(int32 i, int32& offset, ParamValue& v) override {
        ++point_reads;
        if (i!=0) return kInvalidArgument;
        offset=sample_offset;v=value;return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kResultFalse; }
};
struct Changes final : IParameterChanges {
    std::vector<Queue> list;
    int32 virtual_count=-1;
    int32 accessed=0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override {
        return virtual_count>=0 ? virtual_count : (int32)list.size();
    }
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override {
        ++accessed;
        return i>=0 && (size_t)i<list.size() ? &list[(size_t)i] : nullptr;
    }
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
    std::string probe_path=std::string(temp)+"\\p007-midi-offsets.txt";
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
    const uint32_t latency_at_44100=audio->getLatencySamples();
    // Verify host-PDC follows the known 2,784-frame producer lead.
    setup.sampleRate=48000;
    const bool setup_48=audio->setupProcessing(setup)==kResultOk;
    const uint32_t latency_at_48000=audio->getLatencySamples();
    setup.sampleRate=96000;
    const bool setup_96=audio->setupProcessing(setup)==kResultOk;
    const uint32_t latency_at_96000=audio->getLatencySamples();
    setup.sampleRate=44100;
    const bool setup_restore=audio->setupProcessing(setup)==kResultOk;
    const bool latency_ok=setup_48&&setup_96&&setup_restore&&
        latency_at_44100==2558&&latency_at_48000==2784&&latency_at_96000==5568;
    std::printf("P007_MIDI_SCHEDULER_PDC=%s 44100=%u 48000=%u 96000=%u\n",
                latency_ok?"PASS":"FAIL",latency_at_44100,latency_at_48000,latency_at_96000);
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
    int64_t audio_frame_cursor = 0;
    // Host-side measurement: wall time spent inside each actual VST3 callback.
    // The Windows runner is not an ASIO device; record this as diagnostics,
    // not as a real-driver no-dropout certification.
    std::vector<double> audio_callback_us;
    audio_callback_us.reserve(2048);
    const double block_period_us = 1e6 * 512.0 / 44100.0;
    int over_budget_callbacks = 0;
    int64_t note_requested_frame = -1;
    int64_t note_first_pcm_frame = -1;
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
            note_requested_frame = audio_frame_cursor;
            sent = true;
        }
        LARGE_INTEGER callback_begin{}, callback_end{};
        QueryPerformanceCounter(&callback_begin);
        const tresult processed = audio->process(data);
        QueryPerformanceCounter(&callback_end);
        const double callback_us = 1e6 *
            double(callback_end.QuadPart - callback_begin.QuadPart) / freq.QuadPart;
        audio_callback_us.push_back(callback_us);
        if (callback_us > block_period_us) ++over_budget_callbacks;
        if (processed != kResultOk) {
            std::printf("P007_CALLBACK_HOST_RETURN=FAIL code=%d\n",(int)processed);
            return 2;
        }
        if (sent && note_first_pcm_frame < 0) {
            for (int i=0; i<data.numSamples; ++i) {
                // First meaningful PCM frame from the real firmware/audio
                // chain; prior silence alone cannot be treated as a PDC oracle.
                if (std::fabs(l[i]) > 1e-5f || std::fabs(r[i]) > 1e-5f) {
                    note_first_pcm_frame = audio_frame_cursor + i;
                    break;
                }
            }
        }
        audio_frame_cursor += data.numSamples;
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
    std::sort(audio_callback_us.begin(), audio_callback_us.end());
    const auto percentile_us=[&](double p) -> double {
        if (audio_callback_us.empty()) return -1;
        const size_t at=std::min(audio_callback_us.size()-1,
            (size_t)std::floor(p * (audio_callback_us.size()-1)));
        return audio_callback_us[at];
    };
    std::printf("P007_CALLBACK_512_44100_MEASURED blocks=%zu p95_us=%.1f "
                "p99_us=%.1f max_us=%.1f deadline_us=%.1f misses=%d\n",
                audio_callback_us.size(), percentile_us(0.95), percentile_us(0.99),
                percentile_us(1.0), block_period_us, over_budget_callbacks);
    const int64_t pcm_onset_delta = note_first_pcm_frame >= 0 && note_requested_frame >= 0
        ? note_first_pcm_frame - note_requested_frame : -1;
    // Diagnostic for the ACTUAL stereo waveform. The reported scheduler PDC
    // does not cover analog/firmware envelope response; do not label this
    // as exact hardware equivalence or enforce an arbitrary onset tolerance.
    std::printf("P007_PCM_ONSET_MEASURED requested_frame=%lld actual_frame=%lld delay_samples=%lld "
                "reported_scheduler_latency=%u host_rate=44100 threshold=0.00001\n",
                (long long)note_requested_frame,(long long)note_first_pcm_frame,
                (long long)pcm_onset_delta,latency_at_44100);

    // The original main smoke leaves the sustained Pad note on for six seconds.
    // Measure the true firmware response to CC123+CC120, then a matching NoteOff
    // positive control. Recording alone (not a preset label or MIDI log) decides
    // whether the audio actually becomes quiet.
    const auto release_pcm = [&](bool send_all_sound_off, bool send_note_off) -> double {
        events.list.clear(); changes.list.clear();
        const uint8_t cc123[3]={0xB0,123,0};
        const uint8_t cc120[3]={0xB0,120,0};
        if (send_all_sound_off) for (const auto* bytes : {cc123,cc120}) {
            Event e{}; e.type=Event::kDataEvent; e.sampleOffset=0;
            e.data.bytes=bytes; e.data.size=3; events.list.push_back(e);
        }
        if (send_note_off) {
            Event off{}; off.type=Event::kNoteOffEvent;
            off.noteOff.pitch=72; off.noteOff.channel=0; off.sampleOffset=0;
            events.list.push_back(off);
        }
        double late_energy=0;
        uint64_t late_samples=0;
        for (int i=0;i<160;++i) {
            if (audio->process(data)!=kResultOk) return -1;
            // Exclude initial device polls, all effect tails and the known
            // bridge lookahead. Compare the last 35 host blocks only.
            if (i>=125) for(int n=0;n<data.numSamples;++n) {
                late_energy += double(l[n])*l[n]+double(r[n])*r[n];
                late_samples += 2;
            }
            events.list.clear();
            Sleep(12); // paced 512/44100 host callback
        }
        return late_samples ? std::sqrt(late_energy/late_samples) : -1;
    };
    const double held_rms=std::sqrt(sum/frames); // left channel, 13..17s held reference
    // Essential adversarial counterfactual: no MIDI is sent; the same Pad
    // must still be sounding or its natural decay could masquerade as panic.
    const double continued_held_rms=release_pcm(false,false);
    const double panic_tail_rms=release_pcm(true,false);
    const double noteoff_tail_rms=release_pcm(false,true);
    const bool release_probe_valid=audio_ok && held_rms>1e-4 &&
        continued_held_rms>=0 && panic_tail_rms>=0 && noteoff_tail_rms>=0;
    const bool sustained_control=release_probe_valid &&
        continued_held_rms>=0.3*held_rms;
    const bool panic_quiet=sustained_control && panic_tail_rms<continued_held_rms*0.1;
    const double panic_reduction_db=continued_held_rms>1e-8 && panic_tail_rms>=0
        ? 20.0 * std::log10(std::max(panic_tail_rms,1e-12) / continued_held_rms) : 0.0;
    std::printf("P007_PANIC_PCM_COUNTERFACTUAL=%s held_L_rms=%.8f "
                "untouched_held_LR_rms=%.8f panic_LR_late_rms=%.8f "
                "noteoff_LR_late_rms=%.8f panic_delta_db=%.2f "
                "sustained_control=%d panic_below_-20dB=%d\n",
                release_probe_valid?"MEASURED":"NOT_VALID",
                held_rms,continued_held_rms,panic_tail_rms,noteoff_tail_rms,
                panic_reduction_db,(int)sustained_control,(int)panic_quiet);
    // Do not certify acoustic panic unless the ORIGINAL FIRMWARE's Pad
    // survived the matching no-command baseline and then became quieter.
    // Keep this diagnostic separate from the host-load/protocol gates.

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

    // The panel+project tests above use real setState(), which reboots the
    // asynchronous original CPU. A fixed 16×Sleep(4) is not proof that the
    // second firmware boot completed. Start a fresh, uniquely-named worker
    // probe, reload the same owned state, then require an explicit original
    // Engine::sounding_ready transition before timing/overload assertions.
    // The new filename prevents a READY marker from an older feeder from
    // making the new worker appear booted.
    const bool firmware_ready = [&]() -> bool {
        const std::string fresh=std::string(temp)+"\\p007-ready-current-boot.txt";
        DeleteFileA(fresh.c_str());
        const bool exported=SetEnvironmentVariableA("OB12_MIDI_PROBE",fresh.c_str())!=0;
        char observed[MAX_PATH]={};
        const DWORD reported=GetEnvironmentVariableA("OB12_MIDI_PROBE",observed,MAX_PATH);
        if (!exported || reported!=fresh.size() || fresh!=observed) {
            puts("P007_FIRMWARE_READY_GATE=FAIL fresh probe env not installed");
            return false;
        }
        state.pos=0;
        if (component->setState(&state)!=kResultOk) {
            puts("P007_FIRMWARE_READY_GATE=FAIL state reload");
            return false;
        }
        probe_path=fresh;
        for (int check=0; check<1500; ++check) {
            std::ifstream file(probe_path);
            std::string line;
            while (std::getline(file,line))
                if (line.find("P007_WORKER_READY native_frame=")!=std::string::npos) {
                    std::printf("P007_FIRMWARE_READY_GATE=PASS polls=%d\n",check);
                    return true;
                }
            Sleep(20);
        }
        puts("P007_FIRMWARE_READY_GATE=FAIL timeout after 30s");
        return false;
    }();

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

    // Critical mixed-source host path: raw channel Program Change must NOT
    // silence unrelated mapped bank-select CC in the same audio callback.
    // A second fixture verifies that raw CC values win over true mirrored
    // mappings for those exact controller IDs (no duplicate applied).
    const bool midi_mixed_raw_mapped_ok = [&]() -> bool {
        auto state_preset=[&]() -> int {
            MemoryState image;
            if (component->getState(&image)!=kResultOk ||
                image.storage.size()!=0x100000+24) return -100;
            int32_t value=-100;
            std::memcpy(&value,image.storage.data()+20,sizeof value);
            return value;
        };
        auto raw_at=[&](const uint8_t* bytes,uint32_t size,int offset) {
            Event event{};
            event.type=Event::kDataEvent;
            event.sampleOffset=offset;
            event.data.bytes=bytes;
            event.data.size=size;
            events.list.push_back(event);
        };
        auto mapped_cc=[&](int id,double value) {
            Queue queue{};
            queue.id=(ParamID)id;
            queue.value=value;
            changes.list.push_back(queue);
        };
        // Establish unambiguous Timbre-A bank baseline.
        events.list.clear();changes.list.clear();
        mapped_cc(0,0.0); mapped_cc(32,0.0);
        bool calls=audio->process(data)==kResultOk;
        events.list.clear();changes.list.clear();
        // Bank MSB at host sample 0, raw Program at sample 64:
        // this MUST select Program A-006 = 256 + 5.
        const uint8_t program5[]={0xc0,0x05};
        raw_at(program5,2,64);
        mapped_cc(0,64.0/127.0);
        mapped_cc(32,0.0);
        calls=calls && audio->process(data)==kResultOk;
        const int mixed_selected=state_preset();
        events.list.clear();changes.list.clear();
        // Raw bank CCs and their mirrored mapped CCs must NOT compete.
        // If the zero-valued mapped MSB were applied, this would
        // incorrectly select Timbre B-010 (137) instead of Program A-010 (265).
        const uint8_t bank64[]={0xb0,0,64};
        const uint8_t bank0[]={0xb0,32,0};
        const uint8_t program9[]={0xc0,9};
        raw_at(bank64,3,0);
        raw_at(bank0,3,0);
        raw_at(program9,2,64);
        mapped_cc(0,0.0);
        mapped_cc(32,1.0);
        calls=calls && audio->process(data)==kResultOk;
        const int duplicate_selected=state_preset();
        events.list.clear();changes.list.clear();
        const bool result=calls && mixed_selected==261 && duplicate_selected==265;
        std::printf("P007_MIDI_MIXED_RAW_AND_MAPPED=%s "
                    "mixed_selected=%d expected=261 mirrored_selected=%d expected=265\n",
                    result?"PASS":"FAIL",mixed_selected,duplicate_selected);
        return result;
    }();

    // Same CC ID != duplicate unless the exact host-frame offset also
    // collides. This is a full loaded-VST3 counterexample to the previous
    // block-wide "same controller present" suppression.
    const bool midi_controller_frame_identity_ok = [&]() -> bool {
        auto state_program=[&]() -> int {
            MemoryState image;
            if (component->getState(&image)!=kResultOk ||
                image.storage.size()!=0x100000+24) return -100;
            int32_t selected=-100;
            std::memcpy(&selected,image.storage.data()+20,sizeof selected);
            return selected;
        };
        auto push_raw=[&](const uint8_t* bytes,uint32_t count,int offset) {
            Event e{};e.type=Event::kDataEvent;e.sampleOffset=offset;
            e.data.bytes=bytes;e.data.size=count;events.list.push_back(e);
        };
        auto push_mapped=[&](int controller,int offset,int value) {
            Queue point{};
            point.id=(ParamID)controller;
            point.sample_offset=offset;
            point.value=(double)value/127.0;
            changes.list.push_back(point);
        };
        const uint8_t cc0_64[]={0xB0,0x00,0x40};
        const uint8_t cc0_0[]={0xB0,0x00,0x00};
        const uint8_t cc32_0[]={0xB0,0x20,0x00};
        const uint8_t program24[]={0xC0,0x18};
        const uint8_t program25[]={0xC0,0x19};
        bool processed=true;
        events.list.clear();changes.list.clear();
        // Raw Program-bank at t32; later mapped Timbre-bank at t212.
        // The old block-wide CC-ID mask incorrectly DROPPED mapped t212.
        push_raw(cc0_64,3,32);
        push_mapped(0,212,0);
        push_raw(cc32_0,3,0);
        push_raw(program24,2,300);
        processed=processed&&audio->process(data)==kResultOk;
        const int first=state_program(); // Timbre A 025 => 24.
        events.list.clear();changes.list.clear();
        // Invert the values. Old mask drops the later mapped Program bank.
        push_raw(cc0_0,3,32);
        push_mapped(0,212,64);
        push_raw(cc32_0,3,0);
        push_raw(program25,2,300);
        processed=processed&&audio->process(data)==kResultOk;
        const int second=state_program(); // Program A 026 => 281.
        events.list.clear();changes.list.clear();
        // Exact same-offset conflict remains raw-wins; this must not
        // invert the previous behavior under simultaneous host mirroring.
        push_raw(cc0_64,3,32);
        push_mapped(0,32,0);
        push_raw(cc32_0,3,0);
        push_raw(program24,2,300);
        processed=processed&&audio->process(data)==kResultOk;
        const int simultaneous=state_program(); // Program A 025 => 280.
        events.list.clear();changes.list.clear();
        for(int n=0;n<20;++n) {
            processed=processed&&audio->process(data)==kResultOk;
            Sleep(4);
        }
        // Read the real NON-RT firmware worker log; show the two accepted
        // CC0 events per distinct-offset case, including their precise
        // original 48-kHz dispatch delta, not merely the state mirror.
        std::ifstream log(probe_path);
        std::string line;
        struct Dispatch{unsigned long long frame=0;unsigned status=0,pitch=0,value=0;};
        std::vector<Dispatch> seen;
        while(std::getline(log,line)) {
            unsigned long long frame=0,actual=0;
            unsigned status=0,pitch=0,value=0;
            if(std::sscanf(line.c_str(),
                    "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u value=%u",
                    &frame,&actual,&status,&pitch,&value)==5 &&
                frame==actual)
                seen.push_back({actual,status,pitch,value});
        }
        const int expected_delta=(int)(
            std::llround(212.0*48000.0/44100.0)-
            std::llround(32.0*48000.0/44100.0));
        // Each unique Program Change ends the associated controller case.
        // Two distinct raw CC0+mapped CC0 must precede Program24/25,
        // while an exact same-frame clash contributes one CC0.
        bool first_trace=false,second_trace=false,same_trace=false;
        int case_number=0;
        std::vector<Dispatch> cc0;
        for(const auto& x:seen) {
            if(x.status==0xB0 && x.pitch==0) cc0.push_back(x);
            if(x.status!=0xC0)continue;
            if(x.pitch==24 && case_number==0) {
                // First Program24 may appear in other earlier tests; take
                // only cases with our exact ordered distinct CC0 signatures.
                if(cc0.size()>=2) {
                    const auto& a=cc0[cc0.size()-2];
                    const auto& z=cc0.back();
                    if(a.value==64 && z.value==0 &&
                       (long long)(z.frame-a.frame)==expected_delta) {
                        first_trace=true;case_number=1;cc0.clear();continue;
                    }
                }
            }
            if(x.pitch==25 && case_number==1) {
                if(cc0.size()>=2) {
                    const auto& a=cc0[cc0.size()-2];
                    const auto& z=cc0.back();
                    if(a.value==0 && z.value==64 &&
                       (long long)(z.frame-a.frame)==expected_delta) {
                        second_trace=true;case_number=2;cc0.clear();continue;
                    }
                }
            }
            if(x.pitch==24 && case_number==2) {
                if(cc0.size()==1 && cc0.back().value==64) {
                    same_trace=true;case_number=3;cc0.clear();continue;
                }
            }
        }
        const bool ok=processed&&first==24&&second==281&&simultaneous==280&&
                      first_trace&&second_trace&&same_trace;
        std::printf("P007_MIDI_PER_EVENT_CC_OFFSET_IDENTITY=%s "
                    "raw_then_mapped=%d expected=24 mapped_later=%d expected=281 "
                    "same_frame_raw_wins=%d expected=280 "
                    "native_traces=%d/%d/%d expected_delta=%d\n",
                    ok?"PASS":"FAIL",first,second,simultaneous,
                    (int)first_trace,(int)second_trace,(int)same_trace,expected_delta);
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
        std::printf("P007_MIDI_OFFSET_DISPATCH=%s diff=%lld expected=%lld\n",
                    ok?"PASS":"FAIL",
                    seen[0]&&seen[1]?(long long)(actuals[1]-actuals[0]):-1LL,
                    separation);
        return ok;
    }();
    // 513th MIDI message forces a queue recovery. The note-off is deliberately
    // rejected: the worker must first deliver the 512 accepted messages and THEN
    // inject CC123/CC120. A premature panic can leave the note sounding.
    const bool recovery_ok=[&]() -> bool {
        events.list.clear();
        changes.list.clear();
        Event on{};
        on.type=Event::kNoteOnEvent;
        on.sampleOffset=64;
        on.noteOn.pitch=102;
        on.noteOn.velocity=0.75f;
        events.list.push_back(on);
        const uint8_t clock=0xF8;
        for(int n=0;n<520;++n) {
            Event tick{};
            tick.type=Event::kDataEvent;
            tick.sampleOffset=64;
            tick.data.bytes=&clock;
            tick.data.size=1;
            events.list.push_back(tick);
        }
        Event discarded_off{};
        discarded_off.type=Event::kNoteOffEvent;
        discarded_off.sampleOffset=400;
        discarded_off.noteOff.pitch=102;
        discarded_off.noteOff.velocity=0.0f;
        events.list.push_back(discarded_off);
        const bool published=audio->process(data)==kResultOk;
        events.list.clear();
        bool recovered=false;
        for(int n=0;n<260&&!recovered;++n) {
            if(audio->process(data)!=kResultOk)break;
            Sleep(4);
            std::ifstream probe(probe_path);
            std::string line;
            while(std::getline(probe,line))
                if(line.find("P007_RECOVERY frame=")!=std::string::npos)recovered=true;
        }
        // After recovery a new note-on and a matching note-off must be
        // accepted again. Verify that both reached the original feeder.
        Event again{};
        again.type=Event::kNoteOnEvent;
        again.sampleOffset=64;
        again.noteOn.pitch=103;
        again.noteOn.velocity=0.7f;
        events.list.push_back(again);
        bool restart=audio->process(data)==kResultOk;
        events.list.clear();
        Event off{};
        off.type=Event::kNoteOffEvent;
        off.sampleOffset=320;
        off.noteOff.pitch=103;
        events.list.push_back(off);
        restart=restart&&audio->process(data)==kResultOk;
        events.list.clear();
        for(int n=0;n<35;++n) {restart=restart&&audio->process(data)==kResultOk;Sleep(3);}
        std::ifstream probe(probe_path);
        std::string line;
        int ticks=0;
        bool before=false,marker=false,after_on=false,after_off=false,early=false;
        while(std::getline(probe,line)) {
            unsigned long long target=0,actual=0;
            unsigned status=0,pitch=0;
            if(line.find("P007_RECOVERY frame=")!=std::string::npos) {
                if(!marker) marker=true;
                continue;
            }
            if(std::sscanf(line.c_str(),
                "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u",
                &target,&actual,&status,&pitch)!=4)continue;
            if(status==248 && !marker)++ticks;
            if(status==144&&pitch==102) {
                before=!marker;
                early=early||marker;
            }
            if(status==144&&pitch==103)after_on=marker;
            if(status==128&&pitch==103)after_off=marker;
        }
        const bool ok=published&&recovered&&restart&&marker&&!early&&before&&
                      ticks==511&&after_on&&after_off;
        std::printf("P007_MIDI_SATURATION_RECOVERY=%s accepted_clocks=%d expected=511 "
                    "note_before=%d marker=%d note_after=%d off_after=%d\n",
                    ok?"PASS":"FAIL",ticks,(int)before,(int)marker,(int)after_on,(int)after_off);
        return ok;
    }();

    // Separate native VST3 sample-offset stress: verify that NoteOn and
    // its matching NoteOff are not collapsed at small host block sizes
    // or when switching among 44.1/48/96k host rates. This is the same
    // original firmware-linked VST3 DLL, not a mocked queue-only test.
    const bool small_block_ok = [&]() -> bool {
        struct Case {
            int host_rate, block, pitch, offset_on, offset_off;
            bool on_seen=false, off_seen=false, finite=true;
            unsigned long long on_target=0, on_actual=0, off_target=0, off_actual=0;
            double p99_us=0, max_us=0, rms=0;
            int deadline_misses=0;
        };
        std::vector<Case> cases;
        cases.reserve(9);
        bool calls_ok=true;
        LARGE_INTEGER perf_freq{};
        QueryPerformanceFrequency(&perf_freq);
        for (int host_rate : {44100,48000,96000}) {
            for (int block_size : {64,128,256}) {
                Case c{};
                c.host_rate=host_rate;
                c.block=block_size;
                c.pitch=36+(int)cases.size();
                c.offset_on=3;
                c.offset_off=block_size-2;
                calls_ok = calls_ok && audio->setProcessing(false)==kResultOk &&
                    component->setActive(false)==kResultOk;
                ProcessSetup current{kRealtime,kSample32,512,(double)host_rate};
                calls_ok = calls_ok && audio->setupProcessing(current)==kResultOk &&
                    component->setActive(true)==kResultOk &&
                    audio->setProcessing(true)==kResultOk;
                if (!calls_ok) break;
                const int expected_pdc=(int)std::llround(2784.0*host_rate/48000.0);
                if ((int)audio->getLatencySamples()!=expected_pdc) calls_ok=false;
                data.numSamples=block_size;
                events.list.clear(); changes.list.clear();
                Event on{};
                on.type=Event::kNoteOnEvent;
                on.sampleOffset=c.offset_on;
                on.noteOn.pitch=c.pitch;
                on.noteOn.channel=0;
                on.noteOn.velocity=0.85f;
                Event off{};
                off.type=Event::kNoteOffEvent;
                off.sampleOffset=c.offset_off;
                off.noteOff.pitch=c.pitch;
                off.noteOff.channel=0;
                off.noteOff.velocity=0;
                events.list.push_back(on);
                events.list.push_back(off);
                std::vector<double> durations;
                durations.reserve(160);
                double energy=0;
                int64_t finite_count=0;
                const double deadline_us=1e6*double(block_size)/host_rate;
                for (int n=0;n<150 && calls_ok;++n) {
                    LARGE_INTEGER started{},finished{};
                    QueryPerformanceCounter(&started);
                    calls_ok=audio->process(data)==kResultOk;
                    QueryPerformanceCounter(&finished);
                    const double us=1e6*double(finished.QuadPart-started.QuadPart)/
                        double(perf_freq.QuadPart);
                    durations.push_back(us);
                    if (us > deadline_us) ++c.deadline_misses;
                    for (int frame=0;frame<block_size;++frame) {
                        if (!std::isfinite(l[frame])||!std::isfinite(r[frame])) {
                            c.finite=false;
                        } else {
                            energy+=double(l[frame])*l[frame]+double(r[frame])*r[frame];
                            finite_count+=2;
                        }
                    }
                    events.list.clear();
                    const DWORD pacing_ms=(DWORD)std::max(1,
                        (int)std::ceil(1000.0*block_size/host_rate));
                    Sleep(pacing_ms);
                }
                if (!durations.empty()) {
                    std::sort(durations.begin(),durations.end());
                    c.p99_us=durations[(size_t)(0.99*(durations.size()-1))];
                    c.max_us=durations.back();
                }
                c.rms=finite_count>0 ? std::sqrt(energy/finite_count) : -1;
                cases.push_back(c);
            }
            if (!calls_ok) break;
        }
        // The worker may be finishing the last 240-frame chunk; every host
        // process call above was paced and flushed after an entire buffer.
        Sleep(8);
        std::ifstream dispatch(probe_path);
        std::string line;
        while (std::getline(dispatch,line)) {
            unsigned long long target=0,actual=0;
            unsigned status=0,pitch=0;
            if (std::sscanf(line.c_str(),
                    "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u",
                    &target,&actual,&status,&pitch)!=4)
                continue;
            for (Case& c:cases) if (pitch==(unsigned)c.pitch) {
                if (status==0x90 && !c.on_seen) {
                    c.on_seen=true;c.on_target=target;c.on_actual=actual;
                }
                if (status==0x80 && !c.off_seen) {
                    c.off_seen=true;c.off_target=target;c.off_actual=actual;
                }
            }
        }
        bool valid=calls_ok && cases.size()==9;
        for (const Case& c:cases) {
            const long long expected=
                std::llround(double(c.offset_off)*48000.0/c.host_rate)-
                std::llround(double(c.offset_on)*48000.0/c.host_rate);
            const long long measured=c.on_seen&&c.off_seen ?
                (long long)(c.off_actual-c.on_actual) : -1;
            const bool case_ok=c.finite && c.on_seen && c.off_seen &&
                c.on_actual==c.on_target && c.off_actual==c.off_target &&
                measured==expected;
            std::printf("P007_SMALL_BLOCK_CASE=%s rate=%d block=%d "
                "on=%d off=%d native_delta=%lld expected=%lld "
                "pcm_rms=%.8f p99_us=%.2f max_us=%.2f misses=%d\n",
                case_ok?"PASS":"FAIL",c.host_rate,c.block,c.on_seen,c.off_seen,
                measured,expected,c.rms,c.p99_us,c.max_us,c.deadline_misses);
            valid = valid && case_ok;
        }
        // Return the fixture to the previously established 44.1-kHz/512
        // processing contract before the normal instance teardown.
        calls_ok = audio->setProcessing(false)==kResultOk &&
            component->setActive(false)==kResultOk && calls_ok;
        ProcessSetup old{kRealtime,kSample32,512,44100};
        calls_ok = audio->setupProcessing(old)==kResultOk &&
            component->setActive(true)==kResultOk &&
            audio->setProcessing(true)==kResultOk && calls_ok;
        data.numSamples=512;
        events.list.clear(); changes.list.clear();
        valid = valid && calls_ok;
        std::printf("P007_SMALL_BLOCK_9X_NATIVE_OFFSET_SWEEP=%s cases=%zu\n",
                    valid?"PASS":"FAIL",cases.size());
        return valid;
    }();


    // Host normalization adversary: NaN / Infinity must never flow through
    // lround/float-to-int into valid-looking bank changes or audible notes.
    // High but finite values must be clamped before multiplying.
    const bool midi_nonfinite_host_values_ok=[&]() -> bool {
        auto selected=[&]() -> int {
            MemoryState image;
            if(component->getState(&image)!=kResultOk ||
               image.storage.size()!=0x100000+24) return -100;
            int32_t value=-100;
            std::memcpy(&value,image.storage.data()+20,sizeof value);
            return value;
        };
        auto raw=[&](const uint8_t* bytes,uint32_t n,int off=0) {
            Event event{};event.type=Event::kDataEvent;
            event.sampleOffset=off;event.data.bytes=bytes;event.data.size=n;
            events.list.push_back(event);
        };
        const uint8_t bank64[]={0xB0,0,64};
        const uint8_t bank0[]={0xB0,32,0};
        const uint8_t program22[]={0xC0,22},program23[]={0xC0,23};
        const uint8_t program24[]={0xC0,24},program25[]={0xC0,25};
        auto submit_program=[&](const uint8_t* prog,
                                bool bad_mapped,double bad_value) -> bool {
            events.list.clear();changes.list.clear();
            raw(prog,2,256);
            if(bad_mapped) {
                Queue bad{};
                bad.id=0;bad.value=bad_value;bad.sample_offset=0;
                changes.list.push_back(bad);
            }
            const bool ok=audio->process(data)==kResultOk;
            events.list.clear();changes.list.clear();
            // Let original firmware apply queued/panic input between cases.
            for(int i=0;i<20;++i) {
                if(audio->process(data)!=kResultOk)return false;
                Sleep(4);
            }
            return ok;
        };
        events.list.clear();changes.list.clear();
        raw(bank64,3);raw(bank0,3);raw(program22,2,256);
        bool calls=audio->process(data)==kResultOk;
        events.list.clear();
        for(int i=0;i<20;++i){calls=calls&&audio->process(data)==kResultOk;Sleep(4);}
        const int start=selected();
        calls=calls&&submit_program(program23,true,
            std::numeric_limits<double>::quiet_NaN());
        const int nan_state=selected();
        calls=calls&&submit_program(program24,true,
            std::numeric_limits<double>::infinity());
        const int inf_state=selected();
        calls=calls&&submit_program(program25,true,1e100);
        const int bounded_state=selected();

        // Invalid NoteOn is rejected, but a subsequent valid NoteOn and a
        // NoteOff with NaN release velocity still reach original firmware.
        Event invalid{};
        invalid.type=Event::kNoteOnEvent;invalid.sampleOffset=10;
        invalid.noteOn.pitch=119;
        invalid.noteOn.velocity=std::numeric_limits<float>::quiet_NaN();
        Event legitimate{};
        legitimate.type=Event::kNoteOnEvent;legitimate.sampleOffset=20;
        legitimate.noteOn.pitch=120;legitimate.noteOn.velocity=0.75f;
        Event malformed_off{};
        malformed_off.type=Event::kNoteOffEvent;
        malformed_off.sampleOffset=300;malformed_off.noteOff.pitch=120;
        malformed_off.noteOff.velocity=std::numeric_limits<float>::quiet_NaN();
        events.list={invalid,legitimate,malformed_off};
        changes.list.clear();
        calls=calls&&audio->process(data)==kResultOk;
        events.list.clear();
        for(int i=0;i<30;++i) {calls=calls&&audio->process(data)==kResultOk;Sleep(4);}
        std::ifstream trace(probe_path);
        std::string line;
        bool invalid_note_dispatched=false,valid_note=false,valid_off=false;
        while(std::getline(trace,line)) {
            unsigned long long target=0,actual=0;
            unsigned status=0,pitch=0;
            if(std::sscanf(line.c_str(),
                "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u",
                &target,&actual,&status,&pitch)!=4)continue;
            if(status==144 && pitch==119)invalid_note_dispatched=true;
            if(status==144 && pitch==120)valid_note=true;
            if(status==128 && pitch==120)valid_off=true;
        }
        const bool ok=calls && start==278 && nan_state==279 &&
            inf_state==280 && bounded_state==281 &&
            !invalid_note_dispatched && valid_note && valid_off;
        std::printf("P007_HOST_NONFINITE_MIDI_VALUES=%s "
            "baseline=%d nan=%d inf=%d huge_finite=%d "
            "invalid_on_dispatched=%d valid_on=%d invalid_vel_off_delivered=%d\n",
            ok?"PASS":"FAIL",start,nan_state,inf_state,bounded_state,
            (int)invalid_note_dispatched,(int)valid_note,(int)valid_off);
        return ok;
    }();

    // Malformed host MIDI must not be truncated into a different valid
    // message (nor must out-of-range offsets be silently converted into
    // valid NoteOn/CC automation). A short raw NoteOff is salvaged with
    // zero velocity, and a valid subsequent note must remain playable.
    const bool midi_raw_packet_and_offset_ok=[&]() -> bool {
        const uint8_t cc64[]={0xb0,0,64};
        const uint8_t cc32[]={0xb0,32,0};
        const uint8_t prog30[]={0xc0,30};
        const uint8_t prog31[]={0xc0,31};
        const uint8_t truncated_prog[]={0xc0};
        const uint8_t truncated_cc[]={0xb0,0};
        const uint8_t bad_status[]={0x40,0,0};
        const uint8_t status_as_data[]={0x90,119,0x80};
        const uint8_t fragmented_off[]={0x80,118};
        const uint8_t bad_sysex[]={0xf0,0x7d,0x01}; // no F7 terminator
        const uint8_t clock[]={0xf8};
        const uint8_t malformed_clock[]={0xf8,0xf8};
        auto raw=[&](const uint8_t* bytes,uint32_t len,int offset) {
            Event e{};e.type=Event::kDataEvent;e.sampleOffset=offset;
            e.data.bytes=bytes;e.data.size=len;events.list.push_back(e);
        };
        auto current=[&]() -> int {
            MemoryState state;
            if (component->getState(&state)!=kResultOk ||
                state.storage.size()!=0x100000+24)return -100;
            int32_t selected=-100;
            std::memcpy(&selected,state.storage.data()+20,sizeof selected);
            return selected;
        };
        events.list.clear();changes.list.clear();
        raw(cc64,3,0);raw(cc32,3,0);raw(prog30,2,128);
        bool calls=audio->process(data)==kResultOk;
        events.list.clear();
        const int baseline=current();
        // All malformed messages in the *same* host block are rejected,
        // while genuine Program31 at the end remains valid (no phantom bank).
        raw(truncated_prog,1,0);
        raw(truncated_cc,2,0);
        raw(bad_status,3,0);
        raw(status_as_data,3,0);
        raw(bad_sysex,3,0);
        raw(malformed_clock,2,0);
        raw(cc64,3,-1); // invalid raw controller offset must not be remapped to 0
        raw(prog31,2,128);
        calls=calls&&audio->process(data)==kResultOk;
        events.list.clear();
        const int after_bad=current();
        // A malformed short NoteOff and out-of-block NoteOff still need
        // safe pitch/channel releases. Both should reach the feeder and
        // trigger recovery, while a malformed NoteOn never reaches it.
        Event on{};
        on.type=Event::kNoteOnEvent;on.sampleOffset=40;
        on.noteOn.pitch=118;on.noteOn.velocity=0.75f;
        Event malformed_on{};
        malformed_on.type=Event::kNoteOnEvent;malformed_on.sampleOffset=-5;
        malformed_on.noteOn.pitch=117;malformed_on.noteOn.velocity=0.75f;
        Event release{};
        release.type=Event::kNoteOffEvent;release.sampleOffset=1000000;
        release.noteOff.pitch=118;release.noteOff.velocity=0;
        events.list.push_back(on);
        raw(fragmented_off,2,400);
        events.list.push_back(release);
        events.list.push_back(malformed_on);
        raw(clock,1,256);
        Queue invalid_cc{};
        invalid_cc.id=0; invalid_cc.value=0; invalid_cc.sample_offset=-32;
        changes.list.push_back(invalid_cc);
        calls=calls&&audio->process(data)==kResultOk;
        events.list.clear();changes.list.clear();
        for(int n=0;n<55;++n){calls=calls&&audio->process(data)==kResultOk;Sleep(4);}
        std::ifstream trace(probe_path);
        std::string line;
        bool on118=false,off118=false,short_repaired=false,malformed117=false;
        bool raw_bad_data=false,prog31_delivered=false,clock_valid=false;
        int off_count=0;
        while(std::getline(trace,line)) {
            unsigned long long target=0,actual=0;
            unsigned status=0,pitch=0,value=0;
            if(std::sscanf(line.c_str(),
                "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u value=%u",
                &target,&actual,&status,&pitch,&value)!=5)continue;
            if(status==0x90 && pitch==118)on118=true;
            if(status==0x90 && pitch==117)malformed117=true;
            if(status==0x80 && pitch==118) {
                off118=true;++off_count;
                if(value==0)short_repaired=true;
            }
            if(status==0xc0 && pitch==31)prog31_delivered=true;
            if(status==0xf8)clock_valid=true;
            if((status==0xc0 && pitch==0) || status==0x40 ||
               (status==0x90 && pitch==119))raw_bad_data=true;
        }
        const bool ok=calls&&baseline==286&&after_bad==287&&
             on118&&off118&&short_repaired&&!malformed117&&
             !raw_bad_data&&prog31_delivered&&clock_valid&&off_count>=2;
        std::printf("P007_MALFORMED_RAW_AND_OFFSETS=%s "
            "base=%d expected=286 after=%d expected=287 "
            "note_on=%d note_off=%d short_off_zero=%d off_count=%d "
            "invalid_on=%d malformed_raw_dispatched=%d program31=%d clock=%d\n",
            ok?"PASS":"FAIL",baseline,after_bad,(int)on118,
            (int)off118,(int)short_repaired,off_count,
            (int)malformed117,(int)raw_bad_data,(int)prog31_delivered,(int)clock_valid);
        return ok;
    }();
    // Adversarial host iterator budget: the VST3 callback must finish even
    // if a malicious/buggy IEventList / IParameterChanges reports INT_MAX
    // events. The returned pointers are a real host fixture, while the
    // reported counts are intentionally absurd. Previous implementation
    // walked the entire counts even when its 2,048-message list was full.
    const bool midi_unbounded_host_count_ok=[&]() -> bool {
        LARGE_INTEGER freq{},started{},finished{};
        QueryPerformanceFrequency(&freq);
        auto invoke=[&]() -> double {
            QueryPerformanceCounter(&started);
            const bool processed=audio->process(data)==kResultOk;
            QueryPerformanceCounter(&finished);
            return processed ?
                1e6*double(finished.QuadPart-started.QuadPart)/freq.QuadPart : -1;
        };
        events.list.clear();changes.list.clear();
        Event first{};
        first.type=Event::kNoteOffEvent;
        first.noteOff.pitch=52;first.noteOff.channel=0;
        events.list.push_back(first);
        events.virtual_count=2147483647;
        events.accessed=0;
        const double raw_us=invoke();
        const int raw_reads=events.accessed;
        events.virtual_count=-1;
        events.list.clear();
        changes.list.clear();
        Queue point{};
        point.id=7;point.value=0.75;point.virtual_point_count=2147483647;
        changes.list.push_back(point);
        changes.virtual_count=2147483647;
        changes.accessed=0;
        const double mapped_us=invoke();
        const int queues_queried=changes.accessed;
        const int points_queried=changes.list[0].point_reads;
        changes.virtual_count=-1;
        changes.list.clear();
        const bool ok=raw_us>=0 && mapped_us>=0 &&
            raw_reads>0 && raw_reads<=2048 &&
            queues_queried>0 && queues_queried<=2048 &&
            points_queried>0 && points_queried<=2048;
        std::printf("P007_HOST_REPORTED_INT_MAX_BOUNDS=%s raw_reads=%d "
            "queues_queried=%d points_queried=%d "
            "raw_callback_us=%.1f mapped_callback_us=%.1f\n",
            ok?"PASS":"FAIL",raw_reads,queues_queried,
            points_queried,raw_us,mapped_us);
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
           deferred_frame_ok && firmware_ready && midi_bridge_ok && midi_mixed_raw_mapped_ok &&
           midi_controller_frame_identity_ok && timing_ok && latency_ok && recovery_ok &&
           small_block_ok && midi_nonfinite_host_values_ok &&
           midi_raw_packet_and_offset_ok && midi_unbounded_host_count_ok &&
           reload_ok ? 0 : 2;
}
