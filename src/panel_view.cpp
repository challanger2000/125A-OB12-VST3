#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>
#include <atomic>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include "ob12_engine.h"
#include "panel_controls.h"
#include "panel_view.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Run {
    uint32_t argb;
    uint8_t mode;
    float width;
    std::vector<uint8_t> kinds;
    std::vector<float> points;
    ID2D1PathGeometry* geometry = nullptr;
    bool moving = false;  // drawn per frame by its control, not in the static art
};

// The parts of a control that move with its value: a slider's cap, a knob's face marks.
// Found in the vector art at load. Units are the art's own.
struct Moving {
    std::vector<int> runs;
    float cx = 0, cy = 0;   // knob centre
    float rest = 0;         // knob: the art's pointer angle, degrees clockwise from 12 o'clock
    float travel = 0;       // slider: art units from value 0 to full
    float lo = -135, hi = 135;  // knob: angles of its end marks, degrees clockwise from 12 o'clock
    float radius = 0;       // knob: its body's radius, art units
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // slider: the cap's box at rest, art units
    bool found = false;
};

constexpr int scale_command = 1000;  // menu id is this plus the percentage
constexpr int shot_command = 1999;
constexpr int syx_command = 1500;    // load a .syx or .mid into MIDI IN, as a sequencer would
constexpr int factory_command = 1501;  // forget the stored memory
constexpr int gain_command = 1600;     // plus 0-24: output gain in dB
constexpr int dump_command = 1502;     // save what MIDI OUT sent as system exclusive (a bulk dump)   // hidden: save the panel to OB12_PANEL_SHOT as PNG
constexpr int scale_percents[] = {50, 67, 75, 100, 125, 150, 200};

D2D1_COLOR_F color(uint32_t argb) {
    return D2D1::ColorF(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f);
}

// The unit's colours, from Thor's photos: a royal blue panel, cream print, black knobs and keys.
const uint32_t panel_blue = 0xff2251b0, panel_cream = 0xffece5c6;

const wchar_t* class_name = L"OB12PanelView";

std::atomic<int> gain_db{-1};  // -1: not read yet
std::string gain_file(bool make_folder) {
    const char* base = getenv("LOCALAPPDATA");
    if (!base) return "";
    std::string folder = std::string(base) + "\\OB12";
    if (make_folder) CreateDirectoryA(folder.c_str(), nullptr);
    return folder + "\\output_gain.txt";
}
int read_gain() {
    int db = 0;
    if (FILE* f = fopen(gain_file(false).c_str(), "r")) { if (fscanf(f, "%d", &db) != 1) db = 0; fclose(f); }
    return std::max(0, std::min(24, db));
}
void write_gain(int db) {
    gain_db = db;
    if (FILE* f = fopen(gain_file(true).c_str(), "w")) { fprintf(f, "%d\n", db); fclose(f); }
}
const UINT_PTR frame_timer = 1;

}  // namespace

struct PanelView::Impl {
    Engine& engine;
    HWND window = nullptr;
    ID2D1Factory* factory = nullptr;
    ID2D1RenderTarget* target = nullptr;        // the window's, or a WIC bitmap's for save_image
    ID2D1HwndRenderTarget* hwnd_target = nullptr;
    ID2D1Bitmap* art = nullptr;       // the panel at the current size
    ID2D1Bitmap* lcd = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::vector<Run> runs;
    float art_w = 472, art_h = 126;
    std::function<void(int)> width_request;
    int width = 0, height = 0;

    int pot_value[sizeof panel_pots / sizeof panel_pots[0]];
    Moving moving[sizeof panel_pots / sizeof panel_pots[0]];
    Moving dial;           // the data entry dial: its dimple turns about the centre
    float dial_angle = 0;  // degrees, 15 per detent turned
    std::vector<ID2D1Layer*> live_layers;
    int drag_pot = -1, drag_start_y = 0, drag_start_value = 0, down_key = -1;
    bool drag_dial = false;
    float dial_grab = 0, dial_rest = 0;  // mouse angle at the last move, degrees not yet a detent
    uint8_t lcd_bits[30 * 64];

    explicit Impl(Engine& e) : engine(e) {
        for (int& v : pot_value) v = 0x80;
        memset(lcd_bits, 0, sizeof lcd_bits);
    }

    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
        if (data.size() < 16 || memcmp(data.data(), "OB12", 4)) return;
        const char* p = data.data() + 4;
        auto u32 = [&]() { uint32_t v; memcpy(&v, p, 4); p += 4; return v; };
        auto f32 = [&]() { float v; memcpy(&v, p, 4); p += 4; return v; };
        uint32_t count = u32();
        art_w = f32();
        art_h = f32();
        runs.resize(count);
        for (auto& r : runs) {
            r.argb = u32();
            r.mode = (uint8_t)*p++;
            r.width = f32();
            uint32_t commands = u32();
            r.kinds.reserve(commands);
            for (uint32_t i = 0; i < commands; i++) {
                uint8_t kind = (uint8_t)*p++;
                r.kinds.push_back(kind);
                int n = kind == 2 ? 3 : kind == 3 ? 0 : 1;
                for (int j = 0; j < n * 2; j++) r.points.push_back(f32());
            }
        }
        fix_art();
    }

    // A gradient band (mode 1) is drawn without antialiasing: the bands then tile without the half
    // transparent seams that let the blue panel through a knob or a cap.
    static void fill(ID2D1RenderTarget* t, const Run& run, ID2D1Brush* paint) {
        if (run.mode == 1) t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        t->FillGeometry(run.geometry, paint);
        if (run.mode == 1) t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    // The art is printed black on grey; the unit is cream on blue with black controls.
    void recolour() {
        const float k = art_w / panel_frame_w;
        for (Run& r : runs) {
            if (r.mode >= 4 || r.points.empty()) continue;
            int cr = (r.argb >> 16) & 255, cg = (r.argb >> 8) & 255, cb = r.argb & 255;
            if (std::abs(cr - cg) > 12 || std::abs(cg - cb) > 12) continue;  // LEDs and anything coloured
            int v = (cr + cg + cb) / 3;
            float x0, y0, x1, y1;
            box(r, x0, y0, x1, y1);
            float w = (x1 - x0) / k, h = (y1 - y0) / k;
            int figures = (int)std::count(r.kinds.begin(), r.kinds.end(), (uint8_t)0);
            uint32_t a = r.argb & 0xff000000;
            if (r.argb == 0xff6d6e71 && r.mode == 2) { r.argb = a | 0xd6d6d4; continue; }  // a knob's pointer
            if (r.argb == 0xffbcbec0 && r.mode == 2 && w < 30 && r.moving) { r.argb = a | 0xd6d6d4; continue; }  // its light fill
            if (v > 0xf0 && figures == 1 && w < 1 && h > 40) { r.argb = a | 0x0c0c0e; continue; }  // a slot's highlight: black on the unit
            if (v > 0xf0) { r.argb = a | 0xdcdcd8; continue; }
            if (v < 0x40 && r.mode == 1 && !r.moving && w < 8 && h < 3) { r.argb = a | (panel_cream & 0xffffff); continue; }  // a slider's tick  // index lines and highlights on caps and knobs
            if (v < 0x40 && r.mode != 1) {  // print; mode 1 is a gradient band, shading
                if (r.mode == 3 && figures >= 60) { r.argb = a | 0x3a3a3c; continue; }  // a knob's knurled ring
                bool plain_block = r.mode != 3 && r.kinds.size() <= 6 && (w > 40 || h > 40);  // slots, the LCD bezel
                r.argb = plain_block ? (a | 0x0c0c0e) : (a | (panel_cream & 0xffffff));
                continue;
            }
            int d = 0x12 + std::max(0, v - 0x30) * 30 / 100;  // greys of knobs, caps and keys, kept as shading on black
            if (d > 0x58) d = 0x58;
            r.argb = a | (uint32_t)(d << 16 | d << 8 | d);
        }
    }

    // Two faults of the service drawing the art comes from (Thor's screenshots):
    // - its numbered callouts sit outside the panel, their leader lines reach in: short ticks at the
    //   edges, and one (x 1036) up to the Phrase REC key. A line that starts outside the panel is dropped.
    // - the LFO1 frame has no top edge between its corners (frame units 40.5 to 186.1 at y 20.1).
    void fix_art() {
        const float k = art_w / panel_frame_w;
        auto outside = [&](float x, float y) { return x < 0 || y < 0 || x > art_w || y > art_h; };
        for (Run& r : runs) {
            if (r.mode != 3) continue;
            std::vector<uint8_t> kinds;
            std::vector<float> points;
            bool drop = false;
            size_t q = 0;
            for (uint8_t kind : r.kinds) {
                int n = kind == 2 ? 3 : kind == 3 ? 0 : 1;
                if (kind == 0) drop = outside(r.points[q], r.points[q + 1]);
                if (!drop) {
                    kinds.push_back(kind);
                    points.insert(points.end(), r.points.begin() + q, r.points.begin() + q + n * 2);
                }
                q += n * 2;
            }
            r.kinds.swap(kinds);
            r.points.swap(points);
        }
        for (Run& r : runs) {
            // the frame line itself (width 0.49), not the thin twin drawn under its corners
            if (r.mode != 3 || r.width < 0.4f || r.points.size() < 2) continue;
            if (std::fabs(r.points[0] / k - 40.5f) > 0.5f || std::fabs(r.points[1] / k - 20.1f) > 0.5f) continue;
            r.kinds.push_back(0); r.points.push_back(40.5f * k); r.points.push_back(20.1f * k);
            r.kinds.push_back(1); r.points.push_back(186.1f * k); r.points.push_back(20.1f * k);
            break;
        }
    }

    void box(const Run& r, float& x0, float& y0, float& x1, float& y1) const {
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (size_t i = 0; i + 1 < r.points.size(); i += 2) {
            x0 = std::min(x0, r.points[i]); x1 = std::max(x1, r.points[i]);
            y0 = std::min(y0, r.points[i + 1]); y1 = std::max(y1, r.points[i + 1]);
        }
    }

    // The data dial: a large gradient disc and a dimple drawn in its own clip. The dimple's clip run and
    // everything up to its pop turn with the encoder.
    void find_dial() {
        const float k = art_w / panel_frame_w;
        float sx = 0, sy = 0; int n = 0;
        for (const Run& run : runs) {
            if (run.mode != 1 || run.points.empty()) continue;
            float x0, y0, x1, y1;
            box(run, x0, y0, x1, y1);
            float w = (x1 - x0) / k, h = (y1 - y0) / k;
            if (w < 140 || w > 160 || h < 140 || h > 160) continue;
            if (std::fabs((x0 + x1) / 2 / k - 1757) > 40 || std::fabs((y0 + y1) / 2 / k - 383) > 40) continue;
            sx += (x0 + x1) / 2; sy += (y0 + y1) / 2; n++;
        }
        if (!n) return;
        dial.cx = sx / n; dial.cy = sy / n;
        for (int r = 0; r < (int)runs.size(); r++) {
            if (runs[r].mode != 4) continue;
            float x0, y0, x1, y1;
            box(runs[r], x0, y0, x1, y1);
            float w = (x1 - x0) / k;
            if (w < 28 || w > 55 || std::hypot((x0 + x1) / 2 - dial.cx, (y0 + y1) / 2 - dial.cy) > 45 * k) continue;
            int depth = 0;
            for (int q = r; q < (int)runs.size(); q++) {
                if (runs[q].mode == 4) depth++;
                if (runs[q].mode == 5) depth--;
                dial.runs.push_back(q);
                runs[q].moving = true;
                if (depth == 0) break;
            }
            dial.found = true;
            break;
        }
    }

    void find_moving() {
        const float k = art_w / panel_frame_w;  // frame units to art units
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            Moving& m = moving[i];
            float px = p.x * k, py = p.y * k;
            int first = -1, last = -1;
            std::vector<int> picked;
            if (p.slider) {
                // The cap: below the track, within the cap's width, not the ticks left of the track.
                for (int r = 0; r < (int)runs.size(); r++) {
                    if (runs[r].mode >= 4 || runs[r].points.empty()) continue;
                    float x0, y0, x1, y1;
                    box(runs[r], x0, y0, x1, y1);
                    if (x0 >= px - 14 * k && x1 <= px + 14 * k && y0 >= py + 36 * k && y1 <= py + 92 * k) picked.push_back(r);
                }
                m.travel = 88 * k;
            } else {
                // The body: a grey fill whose box centre is the knob centre.
                int body = -1;
                float best = 1e9f;
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (run.mode != 2 || run.argb != 0xffbcbec0) continue;
                    float x0, y0, x1, y1;
                    box(run, x0, y0, x1, y1);
                    float w = (x1 - x0) / k, h = (y1 - y0) / k;
                    if (w < 40 || w > 60 || h < 30 || h > 60) continue;
                    float d = std::hypot((x0 + x1) / 2 - px, (y0 + y1) / 2 - py);
                    if (d < best) { best = d; body = r; m.cx = (x0 + x1) / 2; m.cy = (y0 + y1) / 2; }
                }
                if (body < 0 || best > 25 * k) {
                    // Some knobs (Osc2 Range) have no body fill: take the centre from the knurled ring.
                    body = -1; best = 1e9f;
                    for (int r = 0; r < (int)runs.size(); r++) {
                        const Run& run = runs[r];
                        int figures = (int)std::count(run.kinds.begin(), run.kinds.end(), (uint8_t)0);
                        if (run.mode != 3 || figures < 60 || figures > 72) continue;
                        float x0, y0, x1, y1;
                        box(run, x0, y0, x1, y1);
                        if ((y1 - y0) / k < 30 || (y1 - y0) / k > 42) continue;
                        float d = std::hypot((x0 + x1) / 2 - px, (y0 + y1) / 2 - py);
                        if (d < best) { best = d; body = r; m.cx = (x0 + x1) / 2; m.cy = (y0 + y1) / 2; }
                    }
                    if (body < 0 || best > 25 * k) continue;
                }
                float bx0, by0, bx1, by1;
                box(runs[body], bx0, by0, bx1, by1);
                float radius = (by1 - by0) / 2;  // the height: Bezier handles widen the box sideways
                m.radius = radius;
                double sx = 0, sy = 0;  // the face marks' fill centroid points where the knob points
                long sn = 0;
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (r == body || run.mode >= 4 || run.points.empty()) continue;  // the light pointer line turns too
                    int figures = (int)std::count(run.kinds.begin(), run.kinds.end(), (uint8_t)0);
                    if (figures > 8) continue;  // the knurled ring
                    bool inside = true;  // every point of the shape on the knob's face
                    for (size_t j = 0; j + 1 < run.points.size() && inside; j += 2)
                        if (std::hypot(run.points[j] - m.cx, run.points[j + 1] - m.cy) > radius * 1.02f) inside = false;
                    if (!inside) continue;
                    picked.push_back(r);
                    // the pointer's dark fill only
                    if (run.mode == 2 && run.argb == 0xff6d6e71) for (size_t j = 0; j + 1 < run.points.size(); j += 2) { sx += run.points[j] - m.cx; sy += run.points[j + 1] - m.cy; sn++; }
                }
                // The scale's end marks: the small dark fills just outside the knob, below its centre.
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (run.mode != 2 || run.argb != 0xff231f20 || run.points.empty()) continue;
                    float x0, y0, x1, y1;
                    box(run, x0, y0, x1, y1);
                    if ((x1 - x0) / k > 9 || (y1 - y0) / k > 9) continue;
                    float dx = (x0 + x1) / 2 - m.cx, dy = (y0 + y1) / 2 - m.cy, d = std::hypot(dx, dy);
                    if (d < radius * 1.1f || d > radius * 1.9f || dy <= 0) continue;
                    float a = atan2f(dx, -dy) * 180 / 3.14159265f;
                    if (a < -120) m.lo = std::max(m.lo == -135 ? -180.f : m.lo, a);
                    if (a > 120) m.hi = std::min(m.hi == 135 ? 180.f : m.hi, a);
                }
                if (m.lo <= -180 || m.hi >= 180) { m.lo = -135; m.hi = 135; }
                if (!sn) for (int r : picked) if (runs[r].mode == 2)  // no dark pointer: any face fill
                    for (size_t j = 0; j + 1 < runs[r].points.size(); j += 2) { sx += runs[r].points[j] - m.cx; sy += runs[r].points[j + 1] - m.cy; sn++; }
                if (sn) m.rest = (float)(atan2(sx, -sy) * 180 / 3.14159265);
            }
            if (picked.empty()) continue;
            first = picked.front(); last = picked.back();
            // Clip pairs nested between them belong to the part too; an unmatched push or pop belongs to
            // an outer clip and stays in the static art, so its layers stay balanced.
            for (int r : picked) { runs[r].moving = true; m.runs.push_back(r); }
            std::vector<int> open;
            for (int r = first; r <= last; r++) {
                if (runs[r].mode == 4) open.push_back(r);
                else if (runs[r].mode == 5 && !open.empty()) {
                    int push = open.back();
                    open.pop_back();
                    bool whole = true;  // only a clip whose whole content moves goes with it
                    for (int q = push + 1; q < r; q++) if (!runs[q].moving && runs[q].mode < 4) whole = false;
                    if (!whole) continue;
                    runs[push].moving = runs[r].moving = true;
                    m.runs.push_back(push);
                    m.runs.push_back(r);
                }
            }
            std::sort(m.runs.begin(), m.runs.end());
            if (p.slider) {
                m.bx0 = m.by0 = 1e9f; m.bx1 = m.by1 = -1e9f;
                for (int r : picked) {
                    float x0, y0, x1, y1;
                    box(runs[r], x0, y0, x1, y1);
                    m.bx0 = std::min(m.bx0, x0); m.by0 = std::min(m.by0, y0); m.bx1 = std::max(m.bx1, x1); m.by1 = std::max(m.by1, y1);
                }
            }
            m.found = true;
            if (getenv("OB12_PANEL_DEBUG")) fprintf(stderr, "%-18s runs %3zu centre %.1f %.1f rest %.0f range %.0f..%.0f\n", p.name, m.runs.size(), m.cx / k, m.cy / k, m.rest, m.lo, m.hi);
        }
    }

    // A control's moving parts at its value, drawn over the static art.
    void draw_moving(float scale) {
        int count = sizeof panel_pots / sizeof panel_pots[0];
        ID2D1SolidColorBrush* paint = brush;
        // The art's rails stop where the resting cap hides them; carry each down to as far below the
        // bottom tick as the rail reaches above the top one, with its slot. Frame units, then scaled.
        const float f = scale * art_w / panel_frame_w;
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (!p.slider || !moving[i].found) continue;
            // On the unit a slider is a plain black slit: cover the art's drawn slot frame (x +-3.7, the
            // art's slot from y -43.7, carried down to the bottom tick) with the panel and lay the slit.
            float top = p.y - 44.6f, bottom = p.y + 81;
            paint->SetColor(color(panel_blue));
            target->FillRectangle(D2D1::RectF((p.x - 5.2f) * f, top * f, (p.x + 5.2f) * f, (bottom + 0.9f) * f), paint);
            paint->SetColor(color(0xff0c0c0e));
            target->FillRectangle(D2D1::RectF((p.x - 0.9f) * f, (p.y - 43.6f) * f, (p.x + 0.8f) * f, (bottom - 2.1f) * f), paint);
        }
        for (int i = 0; i < count; i++) {
            const Moving& m = moving[i];
            if (!m.found) continue;
            float value = pot_value[i] / 255.f;
            D2D1::Matrix3x2F place = panel_pots[i].slider
                ? D2D1::Matrix3x2F::Translation(0, -value * m.travel)
                : D2D1::Matrix3x2F::Rotation(m.lo + value * (m.hi - m.lo) - m.rest, D2D1::Point2F(m.cx, m.cy));
            target->SetTransform(place * D2D1::Matrix3x2F::Scale(scale, scale) * shift());
            size_t depth = 0;
            for (int r : m.runs) {
                const Run& run = runs[r];
                if (run.mode == 4) {
                    if (depth == live_layers.size()) { ID2D1Layer* l = nullptr; target->CreateLayer(&l); live_layers.push_back(l); }
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), run.geometry), live_layers[depth++]);
                } else if (run.mode == 5) {
                    if (depth) { target->PopLayer(); depth--; }
                } else {
                    paint->SetColor(color(run.argb));
                    if (run.mode == 3) target->DrawGeometry(run.geometry, paint, run.width);
                    else fill(target, run, paint);
                }
            }
            while (depth) { target->PopLayer(); depth--; }
        }
        if (dial.found) {  // the dial's dimple, turned by the encoder
            target->SetTransform(D2D1::Matrix3x2F::Rotation(dial_angle, D2D1::Point2F(dial.cx, dial.cy)) * D2D1::Matrix3x2F::Scale(scale, scale) * shift());
            size_t depth = 0;
            for (int r : dial.runs) {
                const Run& run = runs[r];
                if (run.mode == 4) {
                    if (depth == live_layers.size()) { ID2D1Layer* l = nullptr; target->CreateLayer(&l); live_layers.push_back(l); }
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), run.geometry), live_layers[depth++]);
                } else if (run.mode == 5) {
                    if (depth) { target->PopLayer(); depth--; }
                } else {
                    paint->SetColor(color(run.argb));
                    if (run.mode == 3) target->DrawGeometry(run.geometry, paint, run.width);
                    else fill(target, run, paint);
                }
            }
            while (depth) { target->PopLayer(); depth--; }
        }
        target->SetTransform(shift());
    }

    void build_geometry() {
        for (auto& r : runs) {
            if (r.geometry || r.mode == 5) continue;
            factory->CreatePathGeometry(&r.geometry);
            ID2D1GeometrySink* sink = nullptr;
            r.geometry->Open(&sink);
            sink->SetFillMode(r.mode == 2 ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
            const float* q = r.points.data();
            bool open = false;
            D2D1_FIGURE_BEGIN begin = r.mode == 3 ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED;
            for (uint8_t kind : r.kinds) {
                switch (kind) {
                case 0:
                    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->BeginFigure(D2D1::Point2F(q[0], q[1]), begin);
                    open = true;
                    q += 2;
                    break;
                case 1:
                    sink->AddLine(D2D1::Point2F(q[0], q[1]));
                    q += 2;
                    break;
                case 2:
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(q[0], q[1]), D2D1::Point2F(q[2], q[3]), D2D1::Point2F(q[4], q[5])));
                    q += 6;
                    break;
                case 3:
                    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open = false;
                    break;
                }
            }
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
        }
    }

    void render_art() {
        release(art);
        if (!target || width < 8 || height < 8) return;
        ID2D1BitmapRenderTarget* layer = nullptr;
        if (FAILED(target->CreateCompatibleRenderTarget(D2D1::SizeF((float)width, (float)height), &layer))) return;
        ID2D1SolidColorBrush* paint = nullptr;
        layer->CreateSolidColorBrush(D2D1::ColorF(0), &paint);
        layer->BeginDraw();
        layer->Clear(color(panel_blue));
        float scale = art_scale();
        layer->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * shift());
        std::vector<ID2D1Layer*> clips;
        size_t depth = 0;
        for (auto& r : runs) {
            if (r.moving) continue;
            if (r.mode == 4) {
                if (depth == clips.size()) {
                    ID2D1Layer* l = nullptr;
                    layer->CreateLayer(&l);
                    clips.push_back(l);
                }
                layer->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), r.geometry), clips[depth++]);
            } else if (r.mode == 5) {
                layer->PopLayer();
                depth--;
            } else {
                paint->SetColor(color(r.argb));
                if (r.mode == 3) layer->DrawGeometry(r.geometry, paint, r.width);
                else fill(layer, r, paint);
            }
        }
        draw_panel_c(layer, paint);
        layer->EndDraw();
        for (auto* l : clips) l->Release();
        layer->GetBitmap(&art);
        paint->Release();
        layer->Release();
    }

    // Panel C, drawn in the art's style (cream frames and print, black keys) from the service
    // manual's sheet and Thor's photos: WHEEL MODE, KEYBOARD SENSE, and the ribbon's two keys.
    void draw_panel_c(ID2D1RenderTarget* t, ID2D1SolidColorBrush* paint) {
        float s = sx();
        t->SetTransform(shift());
        auto P = [&](float x, float y) { return D2D1::Point2F(x * s, y * s); };
        const float x0 = -226, x1 = -12, line = 2.7f * s;
        paint->SetColor(color(panel_cream));
        struct Box { float y0, y1, bar; bool title_top; const wchar_t* title; };
        const Box boxes[] = {{20, 230, 38, true, L"WHEEL MODE"}, {250, 440, 422, false, L"KEYBOARD SENSE"}, {460, 667, 649, false, L"RIBBON"}};
        IDWriteFactory* dw = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
        IDWriteTextFormat* title = nullptr, *label = nullptr;
        if (dw) {
            dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, 13.5f * s, L"", &title);
            dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, 10.5f * s, L"", &label);
            for (auto* f : {title, label}) if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
        }
        auto text = [&](IDWriteTextFormat* f, const wchar_t* str, float cx, float cy) {
            if (f) t->DrawText(str, (UINT32)wcslen(str), f, D2D1::RectF((cx - 110) * s, (cy - 10) * s, (cx + 110) * s, (cy + 10) * s), paint);
        };
        for (const Box& b : boxes) {
            t->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 * s, b.y0 * s, x1 * s, b.y1 * s), 15 * s, 15 * s), paint, line);
            t->DrawLine(P(x0, b.bar), P(x1, b.bar), paint, line);
            text(title, b.title, (x0 + x1) / 2, b.title_top ? (b.y0 + b.bar) / 2 : (b.bar + b.y1) / 2);
        }
        text(label, L"STND", -195, 82);
        text(label, L"M.MORPH", -145, 82);
        text(label, L"AFTERTOUCH", -68, 96);
        text(label, L"EXCHANGE", -68, 111);
        text(label, L"VELOCITY", -170, 372);
        text(label, L"AFTERTOUCH", -68, 372);
        text(label, L"RELATIVE", -170, 592);
        text(label, L"HOLD", -68, 592);
        for (const PanelLed& led : panel_leds) {  // the small indicators' unlit dots
            if (led.x >= 0 || led.lamp) continue;
            paint->SetColor(color(0xff0c0c0e));
            t->FillEllipse(D2D1::Ellipse(P(led.x, led.y), 3.4f * s, 3.4f * s), paint);
            paint->SetColor(color(panel_cream));
            t->DrawEllipse(D2D1::Ellipse(P(led.x, led.y), 3.6f * s, 3.6f * s), paint, 0.9f * s);
        }
        for (const PanelKey& k : panel_keys) {  // black keys like the art's
            if (k.x >= 0) continue;
            paint->SetColor(color(0xff141416));
            t->FillEllipse(D2D1::Ellipse(P(k.x, k.y), 21 * s, 12 * s), paint);
            paint->SetColor(color(0xff3a3a3e));
            t->FillEllipse(D2D1::Ellipse(P(k.x - 3, k.y - 4), 13 * s, 5 * s), paint);
        }
        if (label) label->Release();
        if (title) title->Release();
        if (dw) dw->Release();
    }

    void make_target() {
        if (target || !window) return;
        RECT rc;
        GetClientRect(window, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = 96;  // one unit is one pixel
        if (FAILED(factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(rc.right, rc.bottom)), &hwnd_target))) return;
        target = hwnd_target;
        target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
        lcd_valid = false;  // the LCD bitmap is made at its device size in draw_lcd
        render_art();
    }

    void drop_target() {
        for (auto* l : live_layers) l->Release();
        live_layers.clear();
        release(art); release(lcd); release(brush); release(target);
        hwnd_target = nullptr;
    }

    void scale_menu(LPARAM at) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, syx_command, L"Load .syx / .mid into MIDI IN...");
        size_t sent = engine.sysex_sent();
        wchar_t dump_text[96];
        swprintf(dump_text, 96, L"Save MIDI OUT sysex as .syx (%zu bytes)...", sent);
        AppendMenuW(menu, MF_STRING | (sent ? 0 : MF_GRAYED), dump_command, dump_text);
        AppendMenuW(menu, MF_STRING, factory_command, L"Reset memory to factory...");
        HMENU gain = CreatePopupMenu();
        int now_db = PanelView::output_gain_db();
        for (int db = 0; db <= 24; db++) {
            wchar_t text[16];
            swprintf(text, 16, db ? L"+%d dB" : L"0 dB", db);
            AppendMenuW(gain, MF_STRING | (db == now_db ? MF_CHECKED : 0), gain_command + db, text);
        }
        wchar_t gain_text[48];
        swprintf(gain_text, 48, L"Output gain (now %ls%d dB)", now_db ? L"+" : L"", now_db);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)gain, gain_text);
        if (width_request) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (width_request) for (int percent : scale_percents) {
            wchar_t text[16];
            swprintf(text, 16, L"%d%%", percent);
            bool now = std::abs(width - PanelView::base_width * percent / 100) < 4;
            AppendMenuW(menu, MF_STRING | (now ? MF_CHECKED : 0), scale_command + percent, text);
        }
        POINT p = {GET_X_LPARAM(at), GET_Y_LPARAM(at)};
        ClientToScreen(window, &p);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
        DestroyMenu(menu);
    }

    void load_sysex() {
        wchar_t name[MAX_PATH] = {};
        OPENFILENAMEW ofn = {sizeof ofn};
        ofn.hwndOwner = window;
        ofn.lpstrFilter = L"System exclusive (*.syx, *.mid)\0*.syx;*.mid\0All files\0*.*\0";
        ofn.lpstrFile = name;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        std::vector<uint8_t> data;
        std::string problem = Engine::read_sysex(name, data);
        if (!problem.empty()) { MessageBoxA(window, problem.c_str(), "OB-12", MB_ICONWARNING); return; }
        engine.send_sysex(data.data(), (int)data.size());
    }
    void save_dump() {
        wchar_t name[MAX_PATH] = L"OB-12 dump.syx";
        OPENFILENAMEW ofn = {sizeof ofn};
        ofn.hwndOwner = window;
        ofn.lpstrFilter = L"System exclusive (*.syx)\0*.syx\0";
        ofn.lpstrFile = name;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"syx";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;
        std::vector<uint8_t> data;
        engine.sysex_sent(&data);
        std::ofstream f(name, std::ios::binary);
        f.write((const char*)data.data(), data.size());
    }
    void reset_memory() {
        if (MessageBoxW(window, L"Delete the stored memory (user programs, timbres, settings)?\n"
                                L"The factory memory comes back the next time the plug-in loads.",
                        L"OB-12", MB_OKCANCEL | MB_ICONWARNING) == IDOK)
            engine.forget_flash();
    }

    // The LCD as a dot matrix, like the EII panel: every pixel a dot of the same size with a gap,
    // computed at the exact device size and drawn 1:1, so no column is doubled by scaling.
    // A pixel's colour is the area of each dot it covers, so dots stay even at any scale.
    std::vector<uint32_t> lcd_pixels;
    uint8_t lcd_shown[30 * 64] = {};
    int lcd_w = 0, lcd_h = 0;
    bool lcd_valid = false;

    // Per device column (or row): which LCD cells it overlaps and by how much of a dot.
    static void dot_cover(int pixels, int cells, std::vector<int>& first, std::vector<float>& cover) {
        const float pitch = (float)pixels / cells, dot = 0.86f;
        first.assign(pixels, 0);
        cover.assign(pixels * 3, 0.f);
        for (int p = 0; p < pixels; p++) {
            int c0 = std::min(cells - 1, (int)(p / pitch));
            first[p] = c0;
            for (int k = 0; k < 3 && c0 + k < cells; k++) {
                float a = (c0 + k) * pitch, b = a + dot * pitch;
                float lo = std::max(a, (float)p), hi = std::min(b, (float)p + 1);
                cover[p * 3 + k] = std::max(0.f, hi - lo);
            }
        }
    }

    void draw_lcd(float s) {
        int x0 = (int)lroundf(panel_lcd_x0 * s), y0 = (int)lroundf(panel_lcd_y0 * s);
        int w = (int)lroundf(panel_lcd_x1 * s) - x0, h = (int)lroundf(panel_lcd_y1 * s) - y0;
        if (w < 8 || h < 4) return;
        bool fresh = engine.lcd(lcd_bits);
        if (!fresh && !lcd_valid) return;
        bool changed = fresh && memcmp(lcd_bits, lcd_shown, sizeof lcd_bits) != 0;
        if (!lcd || w != lcd_w || h != lcd_h) {
            release(lcd);
            if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &lcd))) return;
            lcd_w = w; lcd_h = h;
            changed = true;
        }
        if (changed || !lcd_valid) {
            if (fresh) memcpy(lcd_shown, lcd_bits, sizeof lcd_bits);
            std::vector<int> fx, fy;
            std::vector<float> cx, cy;
            dot_cover(w, 240, fx, cx);
            dot_cover(h, 64, fy, cy);
            // A set bit is background. Gap, unlit dot and lit dot colours, R G B.
            // The unit's blue-white backlight with blue-violet dots (Thor's photo of the SYSTEM READY page).
            const float gap[3] = {0xea, 0xec, 0xfa}, off[3] = {0xdc, 0xdf, 0xf4}, on[3] = {0x48, 0x46, 0xc0};
            lcd_pixels.assign((size_t)w * h, 0);
            for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
                float lit = 0, unlit = 0;
                for (int j = 0; j < 3; j++) {
                    float wy = cy[py * 3 + j];
                    if (wy <= 0) continue;
                    int row = fy[py] + j;
                    for (int i = 0; i < 3; i++) {
                        float wx = cx[px * 3 + i];
                        if (wx <= 0) continue;
                        int col = fx[px] + i;
                        bool background = lcd_shown[row * 30 + (col >> 3)] & (0x80 >> (col & 7));
                        (background ? unlit : lit) += wx * wy;
                    }
                }
                float rest = std::max(0.f, 1 - lit - unlit);
                uint32_t c = 0xff000000;
                for (int k = 0; k < 3; k++) {
                    float v = gap[k] * rest + off[k] * unlit + on[k] * lit;
                    c |= (uint32_t)std::min(255.f, v + 0.5f) << (16 - 8 * k);
                }
                lcd_pixels[(size_t)py * w + px] = c;
            }
            lcd->CopyFromMemory(nullptr, lcd_pixels.data(), w * 4);
            lcd_valid = true;
        }
        target->DrawBitmap(lcd, D2D1::RectF((float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    // The panel drawn into a WIC bitmap and saved as PNG, off screen: for checking it by eye.
    bool save_image(const char* path) {
        if (const char* a = getenv("OB12_PANEL_SHOT_DIAL")) dial_angle = (float)atof(a);  // study: dial angle for a shot
        if (const char* v = getenv("OB12_PANEL_SHOT_VALUE"))  // study: every pot at this value, 0-255
            for (int& value : pot_value) value = atoi(v);
        drop_target();
        IWICImagingFactory* wic = nullptr;
        IWICBitmap* bitmap = nullptr;
        bool ok = false;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap, props, &target))) {
                target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
                lcd_valid = false;
                render_art();
                paint();
                drop_target();
                wchar_t wide[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
                IWICStream* stream = nullptr;
                IWICBitmapEncoder* encoder = nullptr;
                IWICBitmapFrameEncode* frame = nullptr;
                if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide, GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
                    frame->SetSize(width, height);
                    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
                    frame->SetPixelFormat(&format);
                    ok = SUCCEEDED(frame->WriteSource(bitmap, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
                }
                release(frame); release(encoder); release(stream);
            }
        }
        release(bitmap); release(wic);
        if (window) InvalidateRect(window, nullptr, FALSE);
        return ok;
    }

    // The canvas: Panel C's column (panel_c_w frame units), then the art. art_scale is pixels per art
    // unit, sx pixels per frame unit, left the column's width in whole pixels.
    float art_scale() const { return std::min(width / (art_w * (1 + panel_c_w / panel_frame_w)), height / art_h); }
    float sx() const { return art_scale() * art_w / panel_frame_w; }
    float left() const { return roundf(panel_c_w * sx()); }
    D2D1::Matrix3x2F shift() const { return D2D1::Matrix3x2F::Translation(left(), 0); }

    void paint() {
        make_target();
        if (!target) return;
        target->BeginDraw();
        target->Clear(color(panel_blue));
        if (art) target->DrawBitmap(art);
        float s = sx();
        target->SetTransform(shift());  // all below in frame units from the art's origin; Panel C at x < 0

        draw_lcd(s);

        if (getenv("OB12_PANEL_BARE")) { target->EndDraw(); return; }  // study: the art alone, for finding controls
        // Caps and knob faces move with their values; a red mark only where the art had none found.
        draw_moving(art_scale());
        // Lamps: lit from the Master's LED latches. A soft glow, then the lamp, then a highlight.
        uint8_t latches[16];
        engine.leds(latches);
        for (const PanelLed& led : panel_leds) {
            if (!(latches[led.latch] >> led.bit & 1)) continue;
            D2D1_POINT_2F c = D2D1::Point2F(led.x * s, led.y * s);
            float r = led.lamp ? 5.6f : 3.1f;  // a key's lamp, or a small indicator beside a label
            brush->SetColor(D2D1::ColorF(1.f, 0.15f, 0.08f, 0.28f));
            target->FillEllipse(D2D1::Ellipse(c, r * 1.6f * s, r * 1.6f * s), brush);
            brush->SetColor(D2D1::ColorF(1.f, 0.16f, 0.1f));
            target->FillEllipse(D2D1::Ellipse(c, r * s, r * s), brush);
            brush->SetColor(D2D1::ColorF(1.f, 0.75f, 0.6f, 0.85f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - r * 0.29f * s, c.y - r * 0.32f * s), r * 0.32f * s, r * 0.25f * s), brush);
        }
        brush->SetColor(D2D1::ColorF(0.85f, 0.1f, 0.1f));
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (moving[i].found) continue;
            float value = pot_value[i] / 255.f;
            if (p.slider) {
                float y = (p.y + 44 - value * 88) * s;  // the art's track is 88 units long
                target->DrawLine(D2D1::Point2F((p.x - 12) * s, y), D2D1::Point2F((p.x + 12) * s, y), brush, std::max(1.5f, 4 * s));
            } else {
                float angle = (-135 + value * 270) * 3.14159265f / 180;
                float r = 22 * s;
                target->DrawLine(D2D1::Point2F(p.x * s, p.y * s), D2D1::Point2F(p.x * s + r * sinf(angle), p.y * s - r * cosf(angle)), brush, std::max(1.5f, 3.5f * s));
            }
        }
        if (down_key >= 0) {
            const PanelKey& k = panel_keys[down_key];
            brush->SetColor(D2D1::ColorF(0.85f, 0.1f, 0.1f, 0.45f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k.x * s, k.y * s), 21 * s, 12 * s), brush);
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        if (target->EndDraw() == D2DERR_RECREATE_TARGET) drop_target();
    }

    void set_pot(int i, int value) {
        value = std::max(0, std::min(255, value));
        pot_value[i] = value;
        engine.set_pot(panel_pots[i].channel, panel_pots[i].mux, value);
    }

    void mouse_down(int x, int y) {
        float s = sx();
        float fx = (x - left()) / s, fy = y / s;
        SetCapture(window);
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            bool hit = p.slider ? fabsf(fx - p.x) < 20 && fabsf(fy - p.y) < 62 : hypotf(fx - p.x, fy - p.y) < 30;
            if (hit) { drag_pot = i; drag_start_y = y; drag_start_value = pot_value[i]; return; }
        }
        int keys = sizeof panel_keys / sizeof panel_keys[0];
        for (int i = 0; i < keys; i++) {
            const PanelKey& k = panel_keys[i];
            if (fabsf(fx - k.x) < 24 && fabsf(fy - k.y) < 15) {
                down_key = i;
                engine.key(k.bar, k.bit, true);
                return;
            }
        }
        if (hypotf(fx - panel_dial_x, fy - panel_dial_y) < panel_dial_r) {
            drag_dial = true;
            dial_grab = atan2f(fx - panel_dial_x, -(fy - panel_dial_y)) * 180 / 3.14159265f;
            dial_rest = 0;
        }
    }

    // The dial turns with the mouse around its centre, all the way round; every 15 degrees is one
    // encoder detent, clockwise up.
    void mouse_move(int x, int y) {
        if (drag_pot >= 0) set_pot(drag_pot, drag_start_value + (drag_start_y - y) * 256 / std::max(60, height / 3));
        if (drag_dial) {
            float s = sx();
            float a = atan2f((x - left()) / s - panel_dial_x, -(y / s - panel_dial_y)) * 180 / 3.14159265f;
            float d = a - dial_grab;
            if (d > 180) d -= 360;
            if (d < -180) d += 360;
            dial_grab = a;
            dial_angle += d;
            dial_rest += d;
            int detents = (int)(dial_rest / 15);
            if (detents) { engine.turn(detents); dial_rest -= detents * 15.f; }
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    void mouse_up() {
        if (down_key >= 0) engine.key(panel_keys[down_key].bar, panel_keys[down_key].bit, false);
        down_key = drag_pot = -1;
        drag_dial = false;
        ReleaseCapture();
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = (Impl*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (self) switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            self->paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: self->mouse_down(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
        case WM_MOUSEMOVE: self->mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
        case WM_LBUTTONUP: self->mouse_up(); return 0;
        case WM_RBUTTONUP: self->scale_menu(lparam); return 0;
        case WM_COMMAND: {
            // Only our own menu's commands (lParam 0, not a control's notification): a host's command
            // reaching this window must not be taken for a size or a gain.
            if (lparam != 0 || HIWORD(wparam) != 0) break;
            int percent = (int)LOWORD(wparam) - scale_command;
            if (LOWORD(wparam) == syx_command) { self->load_sysex(); return 0; }
            if (LOWORD(wparam) == factory_command) { self->reset_memory(); return 0; }
            if (LOWORD(wparam) == dump_command) { self->save_dump(); return 0; }
            if (LOWORD(wparam) >= gain_command && LOWORD(wparam) <= gain_command + 24) { write_gain(LOWORD(wparam) - gain_command); return 0; }
            if (LOWORD(wparam) == shot_command) { if (const char* path = getenv("OB12_PANEL_SHOT")) self->save_image(path); return 0; }
            bool listed = false;
            for (int p : scale_percents) listed |= p == percent;
            if (listed && self->width_request) self->width_request(PanelView::base_width * percent / 100);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int detents = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            self->engine.turn(detents);
            self->dial_angle += 15.f * detents;  // the dial turns with the encoder
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

PanelView::PanelView(Engine& engine, const std::string& art_file) : impl(new Impl(engine)) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl->factory);
    impl->load(art_file);
    impl->find_moving();
    impl->find_dial();
    impl->recolour();  // after the finders: they know the parts by the art's own colours
    impl->build_geometry();
}

PanelView::~PanelView() {
    detach();
    for (auto& r : impl->runs) release(r.geometry);
    release(impl->factory);
    delete impl;
}

bool PanelView::attach(void* parent) {
    static bool registered = false;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Impl::proc, &module);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Impl::proc;
        wc.hInstance = module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = class_name;
        RegisterClassW(&wc);
        registered = true;
    }
    RECT rc;
    GetClientRect((HWND)parent, &rc);
    impl->window = CreateWindowExW(0, class_name, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom, (HWND)parent, nullptr, module, nullptr);
    if (!impl->window) return false;
    SetWindowLongPtrW(impl->window, GWLP_USERDATA, (LONG_PTR)impl);
    impl->width = rc.right;
    impl->height = rc.bottom;
    SetTimer(impl->window, frame_timer, 33, nullptr);
    return true;
}

void PanelView::detach() {
    if (!impl->window) return;
    KillTimer(impl->window, frame_timer);
    impl->drop_target();
    DestroyWindow(impl->window);
    impl->window = nullptr;
}

void PanelView::resize(int width, int height) {
    impl->width = width;
    impl->height = height;
    if (!impl->window) return;
    SetWindowPos(impl->window, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (impl->hwnd_target) {
        impl->hwnd_target->Resize(D2D1::SizeU(width, height));
        impl->render_art();
    }
    InvalidateRect(impl->window, nullptr, FALSE);
}

int PanelView::output_gain_db() {
    int db = gain_db.load();
    if (db < 0) { db = read_gain(); gain_db = db; }
    return db;
}
float PanelView::output_gain() { return powf(10.f, output_gain_db() / 20.f); }

float PanelView::aspect() const { return impl->art_w * (1 + panel_c_w / panel_frame_w) / impl->art_h; }

void PanelView::on_width_request(std::function<void(int)> request) { impl->width_request = std::move(request); }
