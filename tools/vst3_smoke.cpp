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
#include "pluginterfaces/gui/iplugview.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;

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
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        requests++;
        if (calls_back) view->onSize(r);
        return kResultOk;
    }
};

static void test_scale(IComponent* component) {
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
    view->removed();
    view->release();
    IPlugView* again = controller->createView(ViewType::kEditor);
    again->getSize(&size);
    printf("reopens at %d x %d, %d requests\n", size.getWidth(), size.getHeight(), frame.requests);
    again->release();
    DestroyWindow(parent);
    controller->release();
}

int main(int, char** argv) {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    SetEnvironmentVariableA("LOCALAPPDATA", temp);  // keep the user's saved size out of this
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
    test_scale(component);
    audio->setProcessing(false);
    component->setActive(false);
    component->terminate();
    component->release();
    return 0;
}
