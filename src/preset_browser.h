// 125A OB-12: a non-modal Windows preset browser. The original front panel and firmware
// remain the authority; selection uses the documented OB-12 bank select / program change MIDI.
#pragma once
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

class PresetBrowser {
public:
    explicit PresetBrowser(Engine& engine, const std::string& panel_file)
        : engine_(engine), rom_dir_(panel_file.substr(0, panel_file.find_last_of("\\/"))) {
        background_ = CreateSolidBrush(RGB(23, 27, 34));
        field_brush_ = CreateSolidBrush(RGB(34, 40, 50));
        font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
        load_names(0, rom_dir_ + "\\presets\\12prgFS.bin");
        load_names(1, rom_dir_ + "\\presets\\12tmbFS.bin");
        load_favorites();
    }
    ~PresetBrowser() {
        close();
        if (font_) DeleteObject(font_);
        if (field_brush_) DeleteObject(field_brush_);
        if (background_) DeleteObject(background_);
    }
    void close() { if (window_ && IsWindow(window_)) DestroyWindow(window_); }
    void show(HWND panel) {
        if (window_ && IsWindow(window_)) {
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
            return;
        }
        HINSTANCE instance = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = &PresetBrowser::proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"125A_OB12_PresetBrowser";
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
            registered = true;
        }
        HWND owner = GetAncestor(panel, GA_ROOT);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"125A_OB12_PresetBrowser",
             L"125A OB-12 | Preset Browser", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
             CW_USEDEFAULT, CW_USEDEFAULT, 700, 600, owner, nullptr, instance, this);
        if (window_) SetForegroundWindow(window_);
    }

private:
    enum : int {
        id_type = 3101, id_bank, id_search, id_list, id_favorite,
        id_only_favorites, id_import, id_close, id_status
    };
    Engine& engine_;
    std::string rom_dir_;
    HWND window_ = nullptr, type_ = nullptr, bank_ = nullptr, search_ = nullptr;
    HWND list_ = nullptr, status_ = nullptr, favorite_ = nullptr, only_favorites_ = nullptr;
    HBRUSH background_ = nullptr, field_brush_ = nullptr;
    HFONT font_ = nullptr;
    std::array<std::array<std::string, 256>, 2> names_{};
    std::set<int> favorites_;
    int selected_slot_ = -1;
    int selected_key_ = -1;

    static std::wstring wide(const std::string& text) {
        if (text.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      (int)text.size(), nullptr, 0);
        UINT encoding = CP_UTF8;
        if (!size) {
            encoding = CP_ACP;
            size = MultiByteToWideChar(encoding, 0, text.data(), (int)text.size(), nullptr, 0);
        }
        if (!size) return {};
        std::wstring result(size, L'\0');
        MultiByteToWideChar(encoding, 0, text.data(), (int)text.size(), result.data(), size);
        return result;
    }
    static std::wstring lower(std::wstring text) {
        for (wchar_t& c : text) c = (wchar_t)towlower(c);
        return text;
    }
    static bool ascii_name_char(unsigned char c) {
        return (c >= ' ' && c <= '~');
    }
    static std::string extract(const std::vector<unsigned char>& bytes, size_t at, size_t end) {
        if (at > 0 && ascii_name_char(bytes[at - 1])) return {};
        std::string value;
        for (size_t i = at; i < end && value.size() < 24 && ascii_name_char(bytes[i]); ++i)
            value.push_back((char)bytes[i]);
        while (!value.empty() && value.back() == ' ') value.pop_back();
        size_t first = value.find_first_not_of(' ');
        if (first == std::string::npos) return {};
        value.erase(0, first);
        if (value.size() < 4 || value.size() > 20) return {};
        int letters = 0;
        for (unsigned char c : value) if (std::isalpha(c)) ++letters;
        return letters >= 3 ? value : std::string{};
    }
    // The stock image contains 256 fixed-size slots. The position of the ASCII name
    // is NOT assumed: find a common offset with readable, distinctive names.
    // Fail closed (display slot numbers) if the image does not meet confidence checks.
    void load_names(int type, const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.empty() || bytes.size() % 256 != 0) return;
        size_t stride = bytes.size() / 256;
        if (stride < 16 || stride > 4096) return;
        int best_score = -1;
        std::array<std::string, 256> best{};
        for (size_t offset = 0; offset + 4 < stride; ++offset) {
            std::array<std::string, 256> candidate{};
            std::set<std::string> unique;
            int good = 0;
            for (int i = 0; i < 256; ++i) {
                size_t start = i * stride + offset;
                std::string name = extract(bytes, start, (i + 1) * stride);
                if (name.empty()) continue;
                candidate[i] = std::move(name);
                unique.insert(candidate[i]);
                ++good;
            }
            int score = good * 2 + (int)unique.size();
            if (good >= 160 && unique.size() >= 64 && score > best_score) {
                best_score = score;
                best = std::move(candidate);
            }
        }
        if (best_score >= 0) names_[type] = std::move(best);
    }
    std::string favorites_file() const {
        const char* appdata = getenv("LOCALAPPDATA");
        if (!appdata) return {};
        std::string dir = std::string(appdata) + "\\OB12";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\preset-favorites.txt";
    }
    void load_favorites() {
        const std::string path = favorites_file();
        if (path.empty()) return;
        std::ifstream file(path);
        int key;
        while (file >> key) if (key >= 0 && key < 512) favorites_.insert(key);
    }
    void save_favorites() {
        const std::string path = favorites_file();
        if (path.empty()) return;
        std::ofstream file(path, std::ios::trunc);
        for (int key : favorites_) file << key << '\n';
    }
    int type_value() const { return (int)SendMessageW(type_, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0; }
    int bank_value() const { return (int)SendMessageW(bank_, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0; }
    int slot_key(int slot) const { return type_value() * 256 + bank_value() * 128 + slot; }
    void text_status(const std::wstring& message) {
        if (status_) SetWindowTextW(status_, message.c_str());
    }
    void fill() {
        if (!list_) return;
        std::wstring query;
        int length = GetWindowTextLengthW(search_);
        if (length) {
            std::vector<wchar_t> buffer((size_t)length + 1, L'\0');
            GetWindowTextW(search_, buffer.data(), (int)buffer.size());
            query = lower(buffer.data());
        }
        int ty = type_value(), bank = bank_value();
        bool only = SendMessageW(only_favorites_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (int slot = 0; slot < 128; ++slot) {
            int key = slot_key(slot);
            if (only && !favorites_.count(key)) continue;
            const std::string& name = names_[ty][bank * 128 + slot];
            wchar_t number[24] = {};
            swprintf(number, 24, L"%lc-%03d", bank ? L'B' : L'A', slot + 1);
            std::wstring label = std::wstring(favorites_.count(key) ? L"\u2605  " : L"    ") +
                                 number + L"    " + (name.empty() ? L"(name not indexed)" : wide(name));
            if (!query.empty() && lower(label).find(query) == std::wstring::npos) continue;
            LRESULT index = SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)label.c_str());
            if (index != LB_ERR && index != LB_ERRSPACE) {
                SendMessageW(list_, LB_SETITEMDATA, (WPARAM)index, slot);
                if (selected_key_ == key) SendMessageW(list_, LB_SETCURSEL, (WPARAM)index, 0);
            }
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
    }
    void select_current() {
        LRESULT row = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (row == LB_ERR) return;
        LRESULT slot = SendMessageW(list_, LB_GETITEMDATA, (WPARAM)row, 0);
        if (slot < 0 || slot >= 128) return;
        selected_slot_ = (int)slot;
        selected_key_ = slot_key(selected_slot_);
        // Routes through the original Master CPU firmware. The MIDI Program Map must be off
        // for standard bank/slot addressing, per the OB-12 owner's manual.
        engine_.select_preset(type_value() == 0, bank_value(), selected_slot_);
        wchar_t label[130];
        swprintf(label, 130, L"Sent %ls %lc-%03d through MIDI Bank Select + Program Change",
                 type_value() ? L"Timbre" : L"Program", bank_value() ? L'B' : L'A',
                 selected_slot_ + 1);
        text_status(label);
    }
    void toggle_favorite() {
        LRESULT row = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (row == LB_ERR) return;
        LRESULT slot = SendMessageW(list_, LB_GETITEMDATA, (WPARAM)row, 0);
        if (slot < 0 || slot >= 128) return;
        int key = slot_key((int)slot);
        if (!favorites_.erase(key)) favorites_.insert(key);
        save_favorites();
        fill();
    }
    void import_bank() {
        wchar_t path[32768] = {};
        OPENFILENAMEW dlg = {sizeof dlg};
        dlg.hwndOwner = window_;
        dlg.lpstrFile = path;
        dlg.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
        dlg.lpstrFilter = L"OB-12 SysEx / MIDI bank (*.syx;*.mid)\0*.syx;*.mid\0All files\0*.*\0";
        dlg.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&dlg)) return;
        std::vector<uint8_t> bytes;
        std::string error = Engine::read_sysex(path, bytes);
        if (!error.empty()) {
            MessageBoxW(window_, wide(error).c_str(), L"Bank import failed", MB_ICONERROR);
            return;
        }
        if (MessageBoxW(window_,
             L"Importing this bank into the original OB-12 flash may overwrite existing "
             L"Programs/Timbres. This is NOT a separate read-only browser library.\n\nContinue?",
             L"Confirm bank import", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        engine_.send_sysex(bytes.data(), (int)bytes.size());
        text_status(L"Bank queued for original MIDI SysEx import. Existing memory may be replaced.");
    }
    void create_widgets() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        auto control = [&](LPCWSTR kind, LPCWSTR label, DWORD style, int id) -> HWND {
            HWND hwnd = CreateWindowExW(0, kind, label, style | WS_CHILD | WS_VISIBLE,
                       0, 0, 10, 10, window_, (HMENU)(INT_PTR)id, inst, nullptr);
            if (font_ && hwnd) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font_, TRUE);
            return hwnd;
        };
        control(L"STATIC", L"125A  /  OB-12 PRESET BROWSER", SS_LEFT, 0);
        type_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_type);
        bank_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, id_bank);
        search_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id_search);
        list_ = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL |
                                            WS_BORDER | WS_TABSTOP, id_list);
        favorite_ = control(L"BUTTON", L"Toggle favorite", BS_PUSHBUTTON | WS_TABSTOP, id_favorite);
        only_favorites_ = control(L"BUTTON", L"Only favorites", BS_AUTOCHECKBOX | WS_TABSTOP, id_only_favorites);
        control(L"BUTTON", L"Import .syx / .mid", BS_PUSHBUTTON | WS_TABSTOP, id_import);
        control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, id_close);
        status_ = control(L"STATIC", L"Select an entry. Program Map must be disabled on the OB-12.", SS_LEFT, id_status);
        SendMessageW(type_, CB_ADDSTRING, 0, (LPARAM)L"Programs");
        SendMessageW(type_, CB_ADDSTRING, 0, (LPARAM)L"Timbres");
        SendMessageW(type_, CB_SETCURSEL, 0, 0);
        SendMessageW(bank_, CB_ADDSTRING, 0, (LPARAM)L"Bank A");
        SendMessageW(bank_, CB_ADDSTRING, 0, (LPARAM)L"Bank B");
        SendMessageW(bank_, CB_SETCURSEL, 0, 0);
        fill();
    }
    void layout(int width, int height) {
        width = std::max(width, 430);
        height = std::max(height, 340);
        HWND child = GetWindow(window_, GW_CHILD);
        // Creation-order layout: title, type, bank, search, list, favorite, only, import, close, status.
        if (!child) return;
        MoveWindow(child, 20, 15, width - 40, 28, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 20, 52, 165, 260, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 195, 52, 135, 260, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 340, 52, width - 360, 27, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 20, 90, width - 40, height - 196, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 20, height - 92, 150, 30, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 180, height - 92, 155, 30, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, width - 305, height - 92, 160, 30, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, width - 135, height - 92, 115, 30, TRUE);
        if (!(child = GetWindow(child, GW_HWNDNEXT))) return;
        MoveWindow(child, 20, height - 51, width - 40, 40, TRUE);
    }
    static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<PresetBrowser*>(((CREATESTRUCTW*)l)->lpCreateParams);
            SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)self);
            self->window_ = wnd;
            return TRUE;
        }
        auto* self = reinterpret_cast<PresetBrowser*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(wnd, msg, w, l);
        switch (msg) {
        case WM_CREATE: self->create_widgets(); SetTimer(wnd, 1, 500, nullptr); return 0;
        case WM_SIZE: self->layout(LOWORD(l), HIWORD(l)); return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = (MINMAXINFO*)l;
            info->ptMinTrackSize.x = 510;
            info->ptMinTrackSize.y = 390;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == id_type || LOWORD(w) == id_bank) {
                if (HIWORD(w) == CBN_SELCHANGE) self->fill();
                return 0;
            }
            if (LOWORD(w) == id_search && HIWORD(w) == EN_CHANGE) { self->fill(); return 0; }
            if (LOWORD(w) == id_list && HIWORD(w) == LBN_SELCHANGE) { self->select_current(); return 0; }
            if (LOWORD(w) == id_only_favorites && HIWORD(w) == BN_CLICKED) { self->fill(); return 0; }
            if (LOWORD(w) == id_favorite && HIWORD(w) == BN_CLICKED) { self->toggle_favorite(); return 0; }
            if (LOWORD(w) == id_import && HIWORD(w) == BN_CLICKED) { self->import_bank(); return 0; }
            if (LOWORD(w) == id_close && HIWORD(w) == BN_CLICKED) { DestroyWindow(wnd); return 0; }
            break;
        case WM_TIMER: {
            int waiting = self->engine_.sysex_waiting();
            if (waiting) {
                wchar_t text[100];
                swprintf(text, 100, L"SysEx import: %d messages pending", waiting);
                self->text_status(text);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)w, RGB(230, 235, 244));
            SetBkColor((HDC)w, RGB(34, 40, 50));
            return (LRESULT)self->field_brush_;
        case WM_ERASEBKGND: {
            RECT rect;
            GetClientRect(wnd, &rect);
            FillRect((HDC)w, &rect, self->background_);
            return 1;
        }
        case WM_CLOSE: DestroyWindow(wnd); return 0;
        case WM_DESTROY:
            KillTimer(wnd, 1);
            self->window_ = nullptr;
            self->type_ = self->bank_ = self->search_ = self->list_ = self->status_ = nullptr;
            return 0;
        }
        return DefWindowProcW(wnd, msg, w, l);
    }
};
