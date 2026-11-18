// The front panel as a child window, drawn by Direct2D. Shared by the player and the VST3 editor.
#pragma once
#include <functional>
#include <string>

class Engine;

class PanelView {
public:
    PanelView(Engine& engine, const std::string& art_file);
    ~PanelView();
    bool attach(void* parent_hwnd);
    void detach();
    void resize(int width, int height);
    float aspect() const;  // width over height of the art
    // The right click menu offers sizes; the owner resizes the window and calls resize().
    void on_width_request(std::function<void(int width)> request);
    static constexpr int base_width = 1500;  // the 100% size
    // Output gain, 0 to +24 dB in whole dB, after the analog stage at the very end of the chain. A user
    // setting (%LOCALAPPDATA%\OB12\output_gain.txt), set from the panel menu, read by the plug-in.
    static int output_gain_db();
    static float output_gain();  // as a factor

private:
    struct Impl;
    Impl* impl;
};
