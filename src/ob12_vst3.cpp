// OB-12 VST3 instrument: stereo out, MIDI in, the panel as editor. Written against pluginterfaces
// alone. One COM object is both component and, through a facet, edit controller.
// The engine runs on its own thread and fills a ring; process() only reads from the ring.
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include "ob12_engine.h"
#include "midi_rt_queue.h"
#include <array>
#include "panel_view.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vsttypes.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// VST3 hands controllers to a plug-in as parameters, found through IMidiMapping: ids 0..127 are
// CC 0..127, 128 channel pressure, 129 pitch bend. They are hidden from the host's parameter list.
const int midi_params = kCountCtrlNumber;  // 130

const TUID class_id = INLINE_UID(0x0B120B12, 0x6F1C4A3E, 0x9D27C5A8, 0x12DA5F70);
HINSTANCE module_handle = nullptr;
const int ring_size = 1 << 15;   // frames, a power of two
const int lead_frames = 2304;    // 48 ms in the ring

void copy_ascii(String128 out, const char* in) {
    int i = 0;
    for (; in[i] && i < 127; i++) out[i] = (TChar)in[i];
    out[i] = 0;
}

std::string rom_folder() {
    if (const char* env = getenv("OB12_ROM")) return env;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module_handle, path, MAX_PATH);
    std::string here = path;
    here = here.substr(0, here.find_last_of("\\/"));
    std::string beside = here + "\\rom";
    if (GetFileAttributesA((beside + "\\12cpu152-CPU-v1.52-FINAL.bin").c_str()) != INVALID_FILE_ATTRIBUTES) return beside;
    std::string above = here + "\\..\\rom";
    if (GetFileAttributesA((above + "\\12cpu152-CPU-v1.52-FINAL.bin").c_str()) != INVALID_FILE_ATTRIBUTES) return above;
    // Return the bundle-relative location even when missing; Instrument::start reports failure.
    // Never silently load the original author's machine-specific F: path.
    return beside;
}

// The analog output channel of the main board (service manual, "OUTPUT STEREO CHANNEL" and "ANALOG
// OUTPUTs", sheets 1/15 and 11/15): PCM1716 DAC, 3.1 Vpp at digital full scale; an MFB low pass with
// gain 1 (R150 / R143 = 2K55 / 2K55); an inverting stage with gain 2.375 (R149 / R146 = 47K5 / 20K0),
// 7.35 Vpp; C80 10u into R141 47K and C25 10u into R54 10K couple it. The op amps run on +-12 V, far
// from 7.35 Vpp, so the path is linear: a gain and two DC blocks. The low passes (47 kHz, 70.5 kHz)
// take about 0.5 dB at 20 kHz and are left out. The plug-in's full scale stays the DAC's, so the jack
// level comes out 7.5 dB above it.
// Saved inside the DAW project, not in %LOCALAPPDATA%\OB12\flash.bin.
// Fixed-width little-endian header, followed by the exact 1-MiB Master flash image.
struct ProjectStateHeader {
    char magic[8];
    uint32_t version;
    uint32_t length;
    uint32_t checksum;
    int32_t selected_preset; // -1 unknown, otherwise type*256 + bank*128 + slot
};
static constexpr uint32_t flash_image_bytes = 0x100000;
static uint32_t project_checksum(const std::vector<uint8_t>& bytes) {
    uint32_t value = 2166136261u;
    for (uint8_t byte : bytes) value = (value ^ byte) * 16777619u;
    return value;
}
static bool stream_write_exact(IBStream* stream, const void* bytes, int32 count) {
    int32 written = 0;
    return stream->write(const_cast<void*>(bytes), count, &written) == kResultOk && written == count;
}

struct AnalogOut {
    static constexpr float gain = 47.5f / 20.0f;
    float x1[2] = {}, y1[2] = {}, x2[2] = {}, y2[2] = {};
    float a1 = 0, a2 = 0;  // one pole high pass coefficients at 48 kHz
    AnalogOut() {
        a1 = 1.f / (1.f + 6.2831853f * (1.f / (6.2831853f * 10e-6f * 47e3f)) / 48000.f);  // 0.34 Hz
        a2 = 1.f / (1.f + 6.2831853f * (1.f / (6.2831853f * 10e-6f * 10e3f)) / 48000.f);  // 1.6 Hz
    }
    float run(int c, float x) {
        float h1 = a1 * (y1[c] + x - x1[c]); x1[c] = x; y1[c] = h1;
        float g = h1 * gain;
        float h2 = a2 * (y2[c] + g - x2[c]); x2[c] = g; y2[c] = h2;
        return h2;
    }
};

// The machine and its feeding thread.
struct Instrument {
    std::unique_ptr<Engine> engine;
    std::thread feeder;
    std::atomic<bool> stop{false};
    std::vector<float> ring = std::vector<float>(ring_size * 2);
    std::atomic<uint64_t> written{0}, read{0};
    MidiRtQueue<512,4096> midi_queue;
    std::atomic<bool> midi_panic_pending{false};
    std::atomic<uint64_t> midi_queue_drops{0};
    std::atomic<long> underruns{0};
    AnalogOut analog;
    double position = 0, step = 1;  // resampling, frames of 48 kHz per host frame
    float last[2] = {}, next[2] = {};

    void start_feeder(int preset) {
        feeder = std::thread([this, preset]() {
            bool preset_restored = false;
            std::vector<int32_t> block(240 * 2);
            // Diagnostics are deliberately owned by this NON-realtime worker.
            FILE* dispatch_probe = nullptr;
            if (const char* file = getenv("OB12_MIDI_PROBE"))
                dispatch_probe = fopen(file, "w");
            while (!stop.load(std::memory_order_acquire)) {
                const uint64_t fill = written.load(std::memory_order_relaxed) -
                                      read.load(std::memory_order_acquire);
                const bool ready = engine->sounding_ready();
                if (ready && !preset_restored && preset >= 0 && preset < 512) {
                    engine->select_preset(preset >= 256, (preset % 256) / 128, preset % 128);
                    preset_restored = true;
                }
                if (ready && fill >= (uint64_t)lead_frames) { Sleep(1); continue; }
                const uint64_t base = written.load(std::memory_order_relaxed);
                int generated = 0;
                while (generated < 240 && !stop.load(std::memory_order_relaxed)) {
                    const uint64_t current = base + (uint64_t)generated;
                    // Release due messages before advancing the original CPUs/DSPs past their time.
                    while (const auto* message = midi_queue.front()) {
                        if (message->frame > current) break;
                        if (dispatch_probe) {
                            fprintf(dispatch_probe,
                                    "P007_DISPATCH target=%llu actual=%llu status=%u pitch=%u\\n",
                                    (unsigned long long)message->frame,
                                    (unsigned long long)current,
                                    (unsigned)message->bytes[0],
                                    (unsigned)(message->length > 1 ? message->bytes[1] : 0));
                            fflush(dispatch_probe);
                        }
                        engine->midi_deferred(message->bytes.data(), (int)message->length);
                        midi_queue.pop();
                    }
                    if (midi_panic_pending.exchange(false, std::memory_order_acq_rel)) {
                        // Overflow recovery never runs on the host's audio thread.
                        for (int ch=0; ch<16; ++ch) {
                            const uint8_t off[3] = {(uint8_t)(0xB0|ch), 123, 0};
                            const uint8_t sound_off[3] = {(uint8_t)(0xB0|ch), 120, 0};
                            engine->midi_deferred(off, 3);
                            engine->midi_deferred(sound_off, 3);
                        }
                    }
                    const int n = midi_queue.frames_before_next(current, 240 - generated);
                    if (n == 0) continue; // an event arrived at this exact boundary
                    engine->run(block.data() + generated * 2, n);
                    generated += n;
                }
                if (!ready || generated != 240) continue;
                uint64_t w = written.load(std::memory_order_relaxed);
                const float gain = PanelView::output_gain();
                for (int i = 0; i < 240; i++, w++) {
                    ring[(w & (ring_size - 1)) * 2] =
                        analog.run(0, block[i * 2] / 8388608.f) * gain;
                    ring[(w & (ring_size - 1)) * 2 + 1] =
                        analog.run(1, block[i * 2 + 1] / 8388608.f) * gain;
                }
                written.store(w, std::memory_order_release);
            }
            if (dispatch_probe) fclose(dispatch_probe);
        });
    }
    bool reload_project(const std::vector<uint8_t>& saved, int preset) {
        if (!engine || saved.size() != flash_image_bytes) return false;
        stop.store(true, std::memory_order_release);
        if (feeder.joinable()) feeder.join();
        if (!engine->reload(rom_folder(), saved)) {
            stop.store(false, std::memory_order_release);
            start_feeder(-1);
            return false;
        }
        midi_queue.reset_after_stop();
        midi_panic_pending.store(false, std::memory_order_relaxed);
        std::fill(ring.begin(), ring.end(), 0.0f);
        written.store(0, std::memory_order_relaxed);
        read.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        position = 0;
        step = 1;
        last[0] = last[1] = next[0] = next[1] = 0;
        analog = AnalogOut{};
        stop.store(false, std::memory_order_release);
        start_feeder(preset);
        return true;
    }
    bool start(const std::vector<uint8_t>* saved = nullptr, int preset = -1) {
        std::string rom = rom_folder();
        if (GetFileAttributesA((rom + "\\12cpu152-CPU-v1.52-FINAL.bin").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        timeBeginPeriod(1);
        engine.reset(new Engine(rom));
        // A saved project owns a private flash image. The legacy shared flash is only
        // a fallback for a newly instantiated instrument without a DAW state.
        if (saved && saved->size() == flash_image_bytes) {
            if (!engine->restore_flash(*saved)) return false;
        } else if (const char* base = getenv("LOCALAPPDATA")) {
            std::string folder = std::string(base) + "\\OB12";
            CreateDirectoryA(folder.c_str(), nullptr);
            engine->set_flash_file(folder + "\\flash.bin");
            engine->detach_flash_file(); // copy legacy bank; never share mutable global memory
        }
        start_feeder(preset);
        return true;
    }
    ~Instrument() {
        timeEndPeriod(1);
        stop = true;
        if (feeder.joinable()) feeder.join();
        if (getenv("OB12_VST_LOG")) printf("written %llu read %llu underruns %ld midi_drops %llu\n", (unsigned long long)written.load(), (unsigned long long)read.load(), underruns.load(), (unsigned long long)midi_queue_drops.load());
    }
    bool pull() {
        uint64_t r = read.load(std::memory_order_relaxed);
        if (written.load(std::memory_order_acquire) == r) return false;
        last[0] = next[0];
        last[1] = next[1];
        next[0] = ring[(r & (ring_size - 1)) * 2];
        next[1] = ring[(r & (ring_size - 1)) * 2 + 1];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
    void render(float* l, float* r, int n, bool wait) {
        for (int i = 0; i < n; i++) {
            position += step;
            bool held = false;
            while (position >= 1) {
                while (!pull()) {
                    if (wait && engine->sounding_ready()) { Sleep(1); continue; }
                    held = true;
                    break;
                }
                if (held) break;
                position -= 1;
            }
            if (held) { underruns++; position = 0; l[i] = 0; if (r) r[i] = 0; continue; }
            float a = (float)position;
            l[i] = last[0] + (next[0] - last[0]) * a;
            if (r) r[i] = last[1] + (next[1] - last[1]) * a;
        }
    }
};

class Plugin;
static FILE* midi_log();
// Diagnostic: the plug-in and editor life cycle, into the same opt-in log.
static void trace(const char* format, ...) {
    FILE* f = midi_log();
    if (!f) return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc(10, f);
    fflush(f);
}

// Diagnostic: what the host sends, the first 2000 lines, in %LOCALAPPDATA%\OB12\midi_log.txt.
// Written only when that folder holds a file named midi_log.on.
static FILE* midi_log() {
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    static FILE* f = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const char* base = getenv("LOCALAPPDATA")) {
            std::string folder = std::string(base) + "\\OB12";
            if (GetFileAttributesA((folder + "\\midi_log.on").c_str()) != INVALID_FILE_ATTRIBUTES)
                f = fopen((folder + "\\midi_log.txt").c_str(), "w");
        }
    }
    static int lines = 0;
    // Past the limit the file stays open (another thread may hold it) but takes no more.
    if (f && ++lines > 20000) return nullptr;
    return f;
}

constexpr int min_editor_width = 600;

// The chosen size is the user's, so it lives per user, not in the project.
static std::string editor_width_file(bool make_folder) {
    const char* base = getenv("LOCALAPPDATA");
    if (!base) return "";
    std::string folder = std::string(base) + "\\OB12";
    if (make_folder) CreateDirectoryA(folder.c_str(), nullptr);
    return folder + "\\editor_width.txt";
}

static int load_editor_width() {
    int width = PanelView::base_width;
    if (FILE* f = fopen(editor_width_file(false).c_str(), "r")) {
        if (fscanf(f, "%d", &width) != 1) width = PanelView::base_width;
        fclose(f);
    }
    return std::clamp(width, min_editor_width, 6000);
}

static void save_editor_width(int width) {
    if (FILE* f = fopen(editor_width_file(true).c_str(), "w")) {
        fprintf(f, "%d\n", width);
        fclose(f);
    }
}

class Editor final : public IPlugView {
public:
    Editor(Plugin& owner, Engine& engine);
    ~Editor();
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPlugView::iid)) { addRef(); *obj = this; return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { uint32 n = --refs; trace("editor %p release -> %u", this, n); if (!n) delete this; return n; }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return !strcmp(type, kPlatformTypeHWND) ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API attached(void* parent, FIDString) override {
        trace("editor %p attached parent %p", this, parent);
        if (!view.attach(parent)) return kResultFalse;
        // Studio One may restore an old parent height without calling onSize.
        // Do not size our art from that stale parent rectangle: use the actual
        // VST3 editor dimensions and ask the host to synchronize its frame.
        view.resize(size.getWidth(), size.getHeight());
        RECT parent_size = {};
        GetClientRect((HWND)parent, &parent_size);
        if (frame && (parent_size.right != size.getWidth() ||
                      parent_size.bottom != size.getHeight())) {
            ViewRect desired = size;
            frame->resizeView(this, &desired);
        }
        return kResultOk;
    }
    tresult PLUGIN_API removed() override { trace("editor %p removed", this); view.detach(); return kResultOk; }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* r) override { if (!r) return kInvalidArgument; *r = size; return kResultOk; }
    tresult PLUGIN_API onSize(ViewRect* r) override {
        if (!r) return kInvalidArgument;
        // Preserve the panel's aspect when a DAW supplies a stale editor height.
        const int width = std::max(min_editor_width, r->getWidth());
        const int height = (int)std::lround(width / view.aspect());
        const bool mismatch = width != r->getWidth() || height != r->getHeight();
        size = ViewRect{r->left, r->top, r->left + width, r->top + height};
        view.resize(width, height);
        save_editor_width(width);
        if (mismatch && frame && !correcting_host_size) {
            correcting_host_size = true;
            ViewRect corrected = size;
            frame->resizeView(this, &corrected);
            correcting_host_size = false;
        }
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* f) override { frame = f; return kResultOk; }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
        if (!r) return kInvalidArgument;
        int w = std::max(min_editor_width, r->getWidth());
        r->right = r->left + w;
        r->bottom = r->top + (int)std::lround(w / view.aspect());
        return kResultTrue;
    }

private:
    void request_width(int width) {
        ViewRect r{0, 0, width, (int)std::lround(width / view.aspect())};
        if (!frame || frame->resizeView(this, &r) != kResultOk) return;
        if (size.getWidth() != width) onSize(&r);  // not every host calls back
    }

    void reconcile_delayed_host_frame(HWND panel) {
        if (!frame || !IsWindow(panel)) return;
        HWND parent = GetParent(panel);
        if (!parent || !IsWindow(parent)) return;
        RECT client{};
        if (!GetClientRect(parent, &client)) return;
        const int actual_width = client.right - client.left;
        const int actual_height = client.bottom - client.top;
        const int expected_width = size.getWidth();
        const int expected_height = size.getHeight();
        if (actual_width <= 0 || actual_height <= 0 ||
            (std::abs(actual_width - expected_width) <= 2 &&
             std::abs(actual_height - expected_height) <= 2)) return;
        trace("editor %p deferred frame mismatch host=%dx%d plugin=%dx%d",
              this, actual_width, actual_height, expected_width, expected_height);
        // Many hosts ignore resizeView() when the requested width is unchanged:
        // an updated height alone does not invalidate their cached layout.
        // The SAME width change used by the working Zoom menu forces their
        // real editor frame to be recomputed. Restore the exact user width
        // immediately afterwards. Never change the plug-in's project state.
        request_width(expected_width + 1);
        request_width(expected_width);
    }

    std::atomic<uint32> refs{1};
    Plugin& owner;
    PanelView view;
    ViewRect size{0, 0, 1500, 400};
    IPlugFrame* frame = nullptr;
    bool correcting_host_size = false;
};

class Facet final : public IEditController {
public:
    explicit Facet(Plugin& p) : plugin(p) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override;
    uint32 PLUGIN_API addRef() override;
    uint32 PLUGIN_API release() override;
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return midi_params; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index < 0 || index >= midi_params) return kInvalidArgument;
        info = {};
        info.id = index;
        char name[32];
        if (index == kPitchBend) snprintf(name, sizeof name, "Pitch Bend");
        else if (index == kAfterTouch) snprintf(name, sizeof name, "Aftertouch");
        else snprintf(name, sizeof name, "MIDI CC %d", index);
        copy_ascii(info.title, name);
        copy_ascii(info.shortTitle, name);
        info.stepCount = index == kPitchBend ? 0 : 127;
        info.defaultNormalizedValue = index == kPitchBend ? 0.5 : 0;
        info.flags = ParameterInfo::kIsHidden;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kInvalidArgument; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kInvalidArgument; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id < (ParamID)midi_params ? values[id] : 0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override { if (id < (ParamID)midi_params) values[id] = v; return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString name) override;

private:
    Plugin& plugin;
    ParamValue values[midi_params] = {};
};

class Plugin final : public IComponent, public IAudioProcessor, public IProcessContextRequirements, public IMidiMapping {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) || FUnknownPrivate::iidEqual(iid, IComponent::iid)) { addRef(); *obj = static_cast<IComponent*>(this); return kResultOk; }
        if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid)) { addRef(); *obj = static_cast<IAudioProcessor*>(this); return kResultOk; }
        if (FUnknownPrivate::iidEqual(iid, IProcessContextRequirements::iid)) { addRef(); *obj = static_cast<IProcessContextRequirements*>(this); return kResultOk; }
        if (FUnknownPrivate::iidEqual(iid, IEditController::iid)) { addRef(); *obj = &facet; return kResultOk; }
        if (FUnknownPrivate::iidEqual(iid, IMidiMapping::iid)) { addRef(); *obj = static_cast<IMidiMapping*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { uint32 n = --refs; if (!n) { trace("plugin %p deleted", this); delete this; } return n; }

    tresult PLUGIN_API initialize(FUnknown*) override { trace("plugin %p initialize", this); return kResultOk; }
    tresult PLUGIN_API terminate() override {
        trace("plugin %p terminate", this);
        reloading.store(true, std::memory_order_release);
        while (active_callbacks.load(std::memory_order_acquire)) Sleep(1);
        {
            std::lock_guard<std::mutex> hold(instrument_lock);
            instrument.reset();
        }
        reloading.store(false, std::memory_order_release);
        return kResultOk;
    }

    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type == kAudio && dir == kOutput) return 1;
        if (type == kEvent && dir == kInput) return 1;
        return 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (index != 0 || getBusCount(type, dir) == 0) return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        bus.channelCount = type == kAudio ? 2 : 16;
        copy_ascii(bus.name, type == kAudio ? "Main Out" : "MIDI In");
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool) override { return index == 0 && getBusCount(type, dir) ? kResultOk : kInvalidArgument; }
    tresult PLUGIN_API setActive(TBool state) override {
        trace("plugin %p setActive %d", this, (int)state);
        if (state) ensure();  // a plug-in scan never activates, so it never boots the machine
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* stream) override {
        if (!stream) return kInvalidArgument;
        ProjectStateHeader header{};
        int32 read = 0;
        tresult result = stream->read(&header, sizeof header, &read);
        // Historical versions stored no component state at all.
        if (read == 0) return kResultOk;
        if (result != kResultOk || read != (int32)sizeof header ||
            memcmp(header.magic, "125AOB12", 8) != 0 || header.version != 2 ||
            header.length != flash_image_bytes) return kResultFalse;
        std::vector<uint8_t> image(header.length);
        read = 0;
        result = stream->read(image.data(), (int32)image.size(), &read);
        if (result != kResultOk || read != (int32)image.size() ||
            project_checksum(image) != header.checksum) return kResultFalse;

        if (header.selected_preset < -1 || header.selected_preset >= 512) return kResultFalse;
        // Keep the DAW's audio callback out of the buffer/engine while reloading.
        reloading.store(true, std::memory_order_release);
        while (active_callbacks.load(std::memory_order_acquire)) Sleep(1);
        bool ok = true;
        {
            std::lock_guard<std::mutex> hold(instrument_lock);
            project_flash = std::move(image);
            project_preset = header.selected_preset;
            // Keep the Instrument and Engine addresses stable while the editor is open.
            if (instrument) ok = instrument->reload_project(project_flash, project_preset);
        }
        reloading.store(false, std::memory_order_release);
        return ok ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getState(IBStream* stream) override {
        if (!stream) return kInvalidArgument;
        std::vector<uint8_t> image;
        {
            std::lock_guard<std::mutex> hold(instrument_lock);
            if (instrument && instrument->engine) {
                if (!instrument->engine->snapshot_flash(image)) return kResultFalse;
                project_flash = image;
                project_preset = instrument->engine->selected_preset();
            } else {
                image = project_flash;
            }
        }
        // No instance was started; legacy empty-state behaviour is valid.
        if (image.empty()) return kResultOk;
        if (image.size() != flash_image_bytes) return kResultFalse;
        ProjectStateHeader header{};
        memcpy(header.magic, "125AOB12", 8);
        header.version = 2;
        header.length = (uint32_t)image.size();
        header.checksum = project_checksum(image);
        header.selected_preset = project_preset;
        return stream_write_exact(stream, &header, (int32)sizeof header) &&
               stream_write_exact(stream, image.data(), (int32)image.size()) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32 inputs, SpeakerArrangement* outputs, int32 count) override {
        return inputs == 0 && count == 1 && outputs[0] == SpeakerArr::kStereo ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arrangement) override {
        if (dir != kOutput || index != 0) return kInvalidArgument;
        arrangement = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override { rate = setup.sampleRate; return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return kInfiniteTail; }
    uint32 PLUGIN_API getProcessContextRequirements() override { return 0; }

    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16, CtrlNumber number, ParamID& id) override {
        if (FILE* f = midi_log()) { fprintf(f, "mapping bus %d controller %d\n", bus, number); fflush(f); }
        if (bus != 0 || number < 0 || number >= midi_params) return kResultFalse;
        id = (ParamID)number;
        return kResultTrue;
    }

    static tresult quiet(ProcessData& data) {
        if (data.numSamples > 0 && data.numOutputs > 0 && data.outputs) {
            auto& out = data.outputs[0];
            for (int c = 0; c < out.numChannels; ++c)
                if (out.channelBuffers32 && out.channelBuffers32[c])
                    memset(out.channelBuffers32[c], 0, (size_t)data.numSamples * sizeof(float));
            out.silenceFlags = 3;
        }
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (reloading.load(std::memory_order_acquire)) return quiet(data);
        active_callbacks.fetch_add(1, std::memory_order_acq_rel);
        struct Finish { std::atomic<int>& counter; ~Finish() { counter.fetch_sub(1, std::memory_order_release); } };
        Finish finish{active_callbacks};
        if (reloading.load(std::memory_order_acquire)) return quiet(data);
        Instrument* inst = instrument.get();
        if (inst && inst->engine) {
            // Fixed-size stack scratch. All allocation, mutex and file logging of the
            // original firmware now happen on the feeder worker, NOT process().
            struct Message {
                int32 offset = 0, order = 0;
                uint8_t bytes[3] = {};
                uint16_t count = 3;
                const uint8_t* raw = nullptr;
            };
            std::array<Message,2048> list{};
            int used = 0, order = 0;
            bool raw_channel_messages = false;
            auto add = [&](const Message& message) {
                if (used >= (int)list.size()) {
                    inst->midi_queue_drops.fetch_add(1, std::memory_order_relaxed);
                    inst->midi_panic_pending.store(true, std::memory_order_release);
                } else list[used++] = message;
            };
            if (data.inputEvents) {
                const int32 count = data.inputEvents->getEventCount();
                for (int32 i = 0; i < count; ++i) {
                    Event e{};
                    if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
                    Message m{};
                    m.offset = e.sampleOffset;
                    m.order = order++;
                    if (e.type == Event::kNoteOnEvent) {
                        m.bytes[0] = 0x90 | (e.noteOn.channel & 15);
                        m.bytes[1] = (uint8_t)std::clamp<int>(e.noteOn.pitch, 0, 127);
                        m.bytes[2] = (uint8_t)std::max(1, (int)std::lround(e.noteOn.velocity * 127));
                    } else if (e.type == Event::kNoteOffEvent) {
                        m.bytes[0] = 0x80 | (e.noteOff.channel & 15);
                        m.bytes[1] = (uint8_t)std::clamp<int>(e.noteOff.pitch, 0, 127);
                        m.bytes[2] = (uint8_t)std::clamp<int>((int)std::lround(e.noteOff.velocity * 127),0,127);
                    } else if (e.type == Event::kPolyPressureEvent) {
                        m.bytes[0] = 0xA0 | (e.polyPressure.channel & 15);
                        m.bytes[1] = (uint8_t)std::clamp<int>(e.polyPressure.pitch, 0, 127);
                        m.bytes[2] = (uint8_t)std::clamp<int>((int)std::lround(e.polyPressure.pressure * 127), 0, 127);
                    } else if (e.type == Event::kDataEvent && e.data.bytes && e.data.size) {
                        if (e.data.size > 4096) {
                            inst->midi_queue_drops.fetch_add(1, std::memory_order_relaxed);
                            inst->midi_panic_pending.store(true, std::memory_order_release);
                            continue;
                        }
                        const uint8_t status = e.data.bytes[0];
                        uint32 length = e.data.size;
                        if (status >= 0xf8 || status == 0xf6) length = 1;
                        else if (status == 0xf1 || status == 0xf3 ||
                                 (status >= 0xc0 && status < 0xe0)) length = 2;
                        else if (status == 0xf2 ||
                                 (status >= 0x80 && status < 0xc0) ||
                                 (status >= 0xe0 && status < 0xf0)) length = 3;
                        m.count = (uint16_t)std::min(length,e.data.size);
                        m.raw = e.data.bytes;
                        if (status >= 0x80 && status < 0xf0) raw_channel_messages = true;
                    } else continue;
                    add(m);
                }
            }
            // Only suppress mirrored controller mapping inside the same host callback.
            IParameterChanges* changes = raw_channel_messages ? nullptr : data.inputParameterChanges;
            if (changes) for (int32 i=0; i<changes->getParameterCount(); ++i) {
                IParamValueQueue* queue = changes->getParameterData(i);
                if (!queue) continue;
                const ParamID id = queue->getParameterId();
                if (id >= (ParamID)midi_params) continue;
                for (int32 p=0; p<queue->getPointCount(); ++p) {
                    int32 offset=0;
                    ParamValue v=0;
                    if (queue->getPoint(p,offset,v)!=kResultOk) continue;
                    Message m{};
                    m.offset=offset; m.order=order++;
                    if (id==kPitchBend) {
                        const int bend=std::clamp((int)std::lround(v*16383),0,16383);
                        m.bytes[0]=0xE0; m.bytes[1]=bend&0x7f; m.bytes[2]=bend>>7;
                    } else if (id==kAfterTouch) {
                        m.bytes[0]=0xD0;
                        m.bytes[1]=(uint8_t)std::clamp((int)std::lround(v*127),0,127);
                        m.count=2;
                    } else {
                        m.bytes[0]=0xB0;
                        m.bytes[1]=(uint8_t)id;
                        m.bytes[2]=(uint8_t)std::clamp((int)std::lround(v*127),0,127);
                    }
                    add(m);
                }
            }
            std::sort(list.begin(), list.begin()+used, [](const Message& a,const Message& b) {
                if (a.offset != b.offset) return a.offset < b.offset;
                return a.order < b.order;
            });
            // 240 extra frames provide a boundary margin above the existing lead
            // while the VST3 host block is being submitted. This is an experimental
            // producer timeline and NOT a measured audio latency/PDC claim.
            const uint64_t origin = inst->read.load(std::memory_order_acquire) +
                                    (uint64_t)(lead_frames + 240);
            const double native_per_host = rate > 0 ? 48000.0 / rate : 1.0;
            for (int i=0; i<used; ++i) {
                const Message& m = list[i];
                const uint8_t* bytes = m.raw ? m.raw : m.bytes;
                const int32 offset = std::clamp<int32>(m.offset, 0, std::max<int32>(0,data.numSamples));
                const uint64_t target = origin +
                    (uint64_t)std::llround((double)offset * native_per_host);
                if (!inst->midi_queue.push(target,bytes,m.count)) {
                    inst->midi_queue_drops.fetch_add(1, std::memory_order_relaxed);
                    inst->midi_panic_pending.store(true, std::memory_order_release);
                    continue;
                }
                // The DAW may getState immediately after process without the feeder
                // having reached this future MIDI frame yet.
                inst->engine->observe_host_midi(bytes, m.count);
            }
        }
        if (data.numSamples <= 0 || data.numOutputs < 1 || !data.outputs || data.outputs[0].numChannels < 1) return kResultOk;
        auto& out = data.outputs[0];
        float* l = out.channelBuffers32[0];
        float* r = out.numChannels > 1 ? out.channelBuffers32[1] : nullptr;
        if (!inst || !inst->engine) {
            memset(l, 0, data.numSamples * 4);
            if (r) memset(r, 0, data.numSamples * 4);
            out.silenceFlags = 3;
            return kResultOk;
        }
        inst->step = 48000.0 / rate;
        inst->render(l, r, data.numSamples, data.processMode == kOffline);
        out.silenceFlags = 0;
        return kResultOk;
    }

    // setActive and createView may come on different threads; one machine only.
    Instrument* ensure() {
        std::lock_guard<std::mutex> hold(instrument_lock);
        if (!instrument) {
            auto* fresh = new Instrument;
            if (fresh->start(project_flash.empty() ? nullptr : &project_flash, project_preset)) instrument.reset(fresh);
            else delete fresh;
        }
        return instrument.get();
    }

private:
    std::atomic<uint32> refs{1};
    Facet facet{*this};
    std::unique_ptr<Instrument> instrument;
    std::atomic<bool> reloading{false};
    std::atomic<int> active_callbacks{0};
    std::vector<uint8_t> project_flash; // exact serialized original memory
    int project_preset = -1;
    std::mutex instrument_lock;
    double rate = 48000;
};

Editor::Editor(Plugin& p, Engine& engine) : owner(p), view(engine, rom_folder() + "\\panel.ob12p") {
    static_cast<IComponent&>(owner).addRef();
    size.right = load_editor_width();
    size.bottom = (int)std::lround(size.right / view.aspect());
    view.on_width_request([this](int width) { request_width(width); });
    view.on_host_size_check([this](void* panel_hwnd) {
        reconcile_delayed_host_frame((HWND)panel_hwnd);
    });
}
Editor::~Editor() {
    trace("editor %p deleted", this); view.detach(); static_cast<IComponent&>(owner).release(); }

tresult PLUGIN_API Facet::queryInterface(const TUID iid, void** obj) { return static_cast<IComponent&>(plugin).queryInterface(iid, obj); }
uint32 PLUGIN_API Facet::addRef() { return static_cast<IComponent&>(plugin).addRef(); }
uint32 PLUGIN_API Facet::release() { return static_cast<IComponent&>(plugin).release(); }
IPlugView* PLUGIN_API Facet::createView(FIDString name) {
    trace("createView %s", name ? name : "(null)");
    if (strcmp(name, ViewType::kEditor)) return nullptr;
    Instrument* inst = plugin.ensure();
    Editor* editor = inst ? new Editor(plugin, *inst->engine) : nullptr;
    trace("createView -> editor %p", editor);
    return editor;
}

class Factory final : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory2::iid)) { *obj = this; return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        *info = {};
        strncpy(info->vendor, "Tamas (private)", PFactoryInfo::kNameSize - 1);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index) return kInvalidArgument;
        *info = {};
        memcpy(info->cid, class_id, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
        strncpy(info->name, "OB-12", PClassInfo::kNameSize - 1);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (index) return kInvalidArgument;
        *info = {};
        memcpy(info->cid, class_id, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
        strncpy(info->name, "OB-12", PClassInfo::kNameSize - 1);
        strncpy(info->subCategories, PlugType::kInstrumentSynth, PClassInfo2::kSubCategoriesSize - 1);
        strncpy(info->vendor, "Tamas (private)", PClassInfo2::kVendorSize - 1);
        strncpy(info->version, "0.1.0", PClassInfo2::kVersionSize - 1);
        strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        *obj = nullptr;
        if (!FUnknownPrivate::iidEqual(cid, class_id)) return kNoInterface;
        auto* p = new Plugin;
        tresult result = static_cast<IComponent*>(p)->queryInterface(iid, obj);
        static_cast<IComponent*>(p)->release();
        return result;
    }
};
Factory factory;

}  // namespace

extern "C" {
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module_handle = instance;
    return TRUE;
}
__declspec(dllexport) bool PLUGIN_API InitDll() { return true; }
__declspec(dllexport) bool PLUGIN_API ExitDll() { return true; }
__declspec(dllexport) IPluginFactory* PLUGIN_API GetPluginFactory() { return &factory; }
}
