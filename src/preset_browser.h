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
        find_banks();
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
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
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
        id_only_favorites, id_import, id_close, id_status, id_library
    };
    Engine& engine_;
    std::string rom_dir_;
    HWND window_ = nullptr, type_ = nullptr, bank_ = nullptr, search_ = nullptr;
    HWND list_ = nullptr, status_ = nullptr, favorite_ = nullptr, only_favorites_ = nullptr;
    HWND title_ = nullptr, import_ = nullptr, close_ = nullptr, library_ = nullptr;
    struct BankPair { std::wstring title, program_path, timbre_path; };
    std::vector<BankPair> bank_pairs_;
    HBRUSH background_ = nullptr, field_brush_ = nullptr;
    HFONT font_ = nullptr;
    std::array<std::array<std::string, 256>, 2> names_{};
    std::array<std::array<std::string, 256>, 2> preview_names_{};
    bool preview_active_ = false;
    std::set<int> favorites_;
    int selected_slot_ = -1;
    int selected_key_ = -1;
    bool names_live_ = false, observed_ready_ = false;
    long last_index_writes_ = -1;
    bool transferring_ = false;
    ULONGLONG transfer_started_ = 0, last_flash_activity_ = 0;
    long first_transfer_writes_ = 0, observed_transfer_writes_ = 0;

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
        // The verified factory Program (158 B) and Timbre (167 B) records start
        // directly with an ASCII name. The preceding byte belongs to another record.
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
    bool scan_names(int type, const std::vector<unsigned char>& bytes,
                    std::array<std::string, 256>* destination = nullptr) {
        if (type < 0 || type > 1 || bytes.empty() || bytes.size() % 256 != 0)
            return false;
        size_t stride = bytes.size() / 256;
        if (stride < 16 || stride > 4096) return false;
        // Verified against the upstream v0.10.0 firmware assets: names begin at byte 0.
        // Do not let a shifted one-character-short candidate outrank the actual name.
        if ((type == 0 && stride == 158) || (type == 1 && stride == 167)) {
            std::array<std::string, 256> actual{};
            std::set<std::string> unique;
            int good = 0;
            for (int i = 0; i < 256; ++i) {
                actual[i] = extract(bytes, i * stride, (i + 1) * stride);
                if (!actual[i].empty()) { ++good; unique.insert(actual[i]); }
            }
            if (good >= 160 && unique.size() >= 64) {
                (destination ? *destination : names_[type]) = std::move(actual);
                return true;
            }
        }
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
        if (best_score < 0) return false;
        (destination ? *destination : names_[type]) = std::move(best);
        return true;
    }
    void load_names(int type, const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
        scan_names(type, bytes);
    }
    void sync_live_names() {
        // Read actual original-emulator flash, not a stale factory .bin when a
        // DAW project has restored a private bank or a user edited patches.
        const bool ready = engine_.sounding_ready();
        if (!ready) { observed_ready_ = false; return; }
        if (!observed_ready_) { last_index_writes_ = -1; observed_ready_ = true; }
        const long before = engine_.flash_writes();
        if (before == last_index_writes_) return;
        // Capture on the engine thread at a millisecond boundary; do not access its
        // mutable flash vector directly from the Win32 editor/UI thread.
        std::vector<uint8_t> flash;
        if (!engine_.snapshot_flash(flash) || flash.size() != 0x100000 ||
            engine_.flash_writes() != before) return;
        names_[0].fill({});
        names_[1].fill({});
        const std::vector<unsigned char> programs(flash.begin() + 0xD0000,
                                                   flash.begin() + 0xD0000 + 40448);
        const std::vector<unsigned char> timbres(flash.begin() + 0xC0000,
                                                  flash.begin() + 0xC0000 + 42752);
        const bool p = scan_names(0, programs);
        const bool t = scan_names(1, timbres);
        // Unknown/invalid changed flash is shown using slot IDs, never the
        // names of a different Factory bank.
        names_live_ = p || t;
        last_index_writes_ = before;
        fill();
    }

    // Decode the standard 7+1 byte packaging in Viscount's OB-12 update
    // records. Works on an Engine::read_sysex stream (F0...F7 messages).
    // This is for *catalogue titles only*. The firmware remains responsible
    // for interpreting, validating and storing the actual bank data.
    static bool decode_bank(const std::vector<uint8_t>& sysex,
                            std::vector<unsigned char>& image) {
        image.clear();
        size_t at = 0;
        bool any = false;
        while (at < sysex.size()) {
            if (sysex[at] != 0xf0) return false;
            size_t end = at + 1;
            while (end < sysex.size() && sysex[end] != 0xf7) ++end;
            if (end >= sysex.size() || end < at + 7) return false;
            if (sysex[at + 1] != 0x31 || sysex[at + 2] != 0x30 ||
                sysex[at + 3] != 0x00) return false;
            int check = 0;
            for (size_t i = at + 1; i < end - 1; ++i) check += sysex[i];
            if (((~check) & 0x7f) != sysex[end - 1]) return false;
            if (sysex[at + 4] == 0x7f) {
                if (end < at + 13) return false;
                int expected = sysex[at + 10];
                std::vector<unsigned char> record;
                for (size_t p = at + 11; p < end - 1;) {
                    size_t remaining = end - 1 - p;
                    size_t group = std::min((size_t)8, remaining);
                    if (group < 2) return false;
                    unsigned char upper = sysex[p + group - 1];
                    for (size_t i = 0; i + 1 < group; ++i)
                        record.push_back((unsigned char)(sysex[p + i] |
                                      ((upper & (1u << i)) ? 0x80 : 0)));
                    p += group;
                }
                if (expected > (int)record.size()) return false;
                if (image.size() + (size_t)expected > 8u * 1024u * 1024u) return false;
                image.insert(image.end(), record.begin(), record.begin() + expected);
                any = true;
            }
            at = end + 1;
        }
        return any;
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
            const std::string& name = (preview_active_ ? preview_names_ : names_)[ty][bank * 128 + slot];
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
        if (preview_active_) {
            text_status(L"This soundbank is only being previewed. Use Load Bank Pair to activate it.");
            return;
        }
        if (transferring_) {
            text_status(L"Soundbank transfer in progress. Preset selection is temporarily disabled.");
            return;
        }
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
    static void enumerate_banks(const std::wstring& directory, std::vector<BankPair>& into) {
        std::wstring mask = directory + L"\\12prg*.mid";
        WIN32_FIND_DATAW info = {};
        HANDLE search = FindFirstFileW(mask.c_str(), &info);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring program = info.cFileName;
            std::wstring filename_lower = lower(program);
            if (program.size() < 11 || program.size() > 17 ||
                filename_lower.substr(0, 5) != L"12prg" ||
                filename_lower.substr(program.size() - 4) != L".mid") continue;
            const size_t suffix_len = program.size() - 9; // prefix length 5, suffix, .mid length 4
            if (suffix_len < 2 || suffix_len > 8) continue;
            std::wstring suffix = program.substr(5, suffix_len);
            if (!std::all_of(suffix.begin(), suffix.end(), [](wchar_t c) {
                    return iswalnum(c) != 0 || c == L'_';
                })) continue;
            std::wstring timbre = L"12tmb" + suffix + L".mid";
            std::wstring timbre_path = directory + L"\\" + timbre;
            if (GetFileAttributesW(timbre_path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::wstring id = lower(suffix);
            std::wstring title = suffix;
            if (id == L"fs") title = L"Factory Presets (Viscount)";
            else if (id == L"jr") title = L"JR Sounds";
            else if (id == L"va2") title = L"Virtual Analog 2 (VA2)";
            else if (id == L"jx1") title = L"Jexus / WC Olo Garb — Set 1";
            else if (id == L"jx2") title = L"Jexus / WC Olo Garb — Set 2";
            else if (id == L"sp") title = L"Syntpelle — 39 Patches (Bank B)";
            else if (id == L"ip") title = L"Individual POB / TOB Patches";
            BankPair pair{title + L" — " + directory, directory + L"\\" + program, timbre_path};
            bool duplicate = false;
            for (const BankPair& old : into)
                if (lower(old.program_path) == lower(pair.program_path)) duplicate = true;
            if (!duplicate) into.push_back(std::move(pair));
        } while (FindNextFileW(search, &info));
        FindClose(search);
    }
    void find_banks() {
        bank_pairs_.clear();
        enumerate_banks(wide(rom_dir_) + L"\\presets", bank_pairs_);
        if (const char* appdata = getenv("LOCALAPPDATA"))
            enumerate_banks(wide(appdata) + L"\\OB12\\SoundBanks", bank_pairs_);
        std::sort(bank_pairs_.begin(), bank_pairs_.end(),
                  [](const BankPair& a, const BankPair& b) { return lower(a.title) < lower(b.title); });
        if (library_) {
            SendMessageW(library_, CB_RESETCONTENT, 0, 0);
            SendMessageW(library_, CB_ADDSTRING, 0, (LPARAM)L"Choose a soundbank pair, or browse for .mid/.syx...");
            for (const BankPair& entry : bank_pairs_)
                SendMessageW(library_, CB_ADDSTRING, 0, (LPARAM)entry.title.c_str());
            SendMessageW(library_, CB_SETCURSEL, 0, 0);
        }
    }
    void preview_bank() {
        const int index = library_ ? (int)SendMessageW(library_, CB_GETCURSEL, 0, 0) - 1 : -1;
        preview_active_ = index >= 0 && index < (int)bank_pairs_.size();
        preview_names_[0].fill({});
        preview_names_[1].fill({});
        if (!preview_active_) {
            if (title_) SetWindowTextW(title_, L"125A  /  OB-12 PRESET BROWSER  |  ACTIVE MEMORY");
            if (list_) EnableWindow(list_, !transferring_);
            fill();
            return;
        }
        const BankPair& bank = bank_pairs_[index];
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> image;
        int count = 0;
        for (int type = 0; type < 2; ++type) {
            const std::wstring& path = type == 0 ? bank.program_path : bank.timbre_path;
            bytes.clear();
            image.clear();
            uint32_t flash_address = 0;
            if (Engine::read_sysex(path, bytes).empty() &&
                Engine::decode_bank(bytes, image, flash_address).empty() &&
                flash_address == (type == 0 ? 0xD0000u : 0xC0000u) &&
                scan_names(type, image, &preview_names_[type])) ++count;
        }
        if (title_) SetWindowTextW(title_, L"125A  /  OB-12 PRESET BROWSER  |  LIBRARY PREVIEW (NOT LOADED)");
        if (list_) EnableWindow(list_, FALSE); // preview entries must not recall a different bank
        fill();
        text_status(count == 2
            ? L"Preview only. Both Program and Timbre names identified. Load Bank Pair to activate."
            : L"Preview only. Some names are unavailable; load the pair to use its sounds.");
    }
    void import_bank() {
        if (transferring_ || engine_.sysex_waiting() > 0) {
            text_status(L"A soundbank transfer is already in progress.");
            return;
        }
        std::wstring program_file, timbre_file;
        int chosen_pair = library_ ? (int)SendMessageW(library_, CB_GETCURSEL, 0, 0) - 1 : -1;
        if (chosen_pair >= 0 && chosen_pair < (int)bank_pairs_.size()) {
            program_file = bank_pairs_[chosen_pair].program_path;
            timbre_file = bank_pairs_[chosen_pair].timbre_path;
        } else {
            wchar_t path[32768] = {};
            OPENFILENAMEW dlg = {sizeof dlg};
            dlg.hwndOwner = window_;
            dlg.lpstrFile = path;
            dlg.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
            dlg.lpstrFilter = L"OB-12 SysEx / MIDI bank (*.syx;*.mid)\0*.syx;*.mid\0All files\0*.*\0";
            dlg.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&dlg)) return;
            std::wstring chosen(path);
            std::wstring filename = chosen.substr(chosen.find_last_of(L"\\/") + 1);
            std::wstring file_lower = lower(filename);
            bool is_program = file_lower.size() == 11 && file_lower.substr(0, 5) == L"12prg" &&
                              file_lower.substr(7) == L".mid";
            bool is_timbre = file_lower.size() == 11 && file_lower.substr(0, 5) == L"12tmb" &&
                             file_lower.substr(7) == L".mid";
            if (is_program || is_timbre) {
                std::wstring dir = chosen.substr(0, chosen.find_last_of(L"\\/") + 1);
                std::wstring id = filename.substr(5, 2);
                program_file = dir + L"12prg" + id + L".mid";
                timbre_file = dir + L"12tmb" + id + L".mid";
                if (GetFileAttributesW(program_file.c_str()) == INVALID_FILE_ATTRIBUTES ||
                    GetFileAttributesW(timbre_file.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    MessageBoxW(window_,
                        L"A matching 12prgXX.mid and 12tmbXX.mid pair is required. "
                        L"Both files must be in the same folder.",
                        L"Incomplete OB-12 soundbank", MB_OK | MB_ICONWARNING);
                    return;
                }
            } else {
                program_file = chosen;
            }
        }

        std::vector<uint8_t> programs, timbres;
        auto read = [&](const std::wstring& file, std::vector<uint8_t>& bytes) {
            if (file.empty()) return true;
            std::string error = Engine::read_sysex(file, bytes);
            if (error.empty()) return true;
            MessageBoxW(window_, wide(error).c_str(), L"Invalid SysEx bank", MB_ICONERROR);
            return false;
        };
        if (!read(program_file, programs) || !read(timbre_file, timbres)) return;

        if (!timbres.empty()) {
            std::vector<uint8_t> program_image, timbre_image;
            uint32_t program_at = 0, timbre_at = 0;
            const std::string pe = Engine::decode_bank(programs, program_image, program_at);
            const std::string te = Engine::decode_bank(timbres, timbre_image, timbre_at);
            if (!pe.empty() || !te.empty() || program_at != 0xD0000 ||
                timbre_at != 0xC0000) {
                std::wstring why = wide(!pe.empty() ? pe : !te.empty() ? te :
                                        "Bank file targets an unexpected flash address");
                MessageBoxW(window_, why.c_str(), L"Unsupported OB-12 bank data", MB_OK | MB_ICONERROR);
                return;
            }
            if (MessageBoxW(window_,
                 L"Load the selected 256 Programs and 256 Timbres into THIS plugin instance's "
                 L"private Flash memory? Existing patches in these two banks will be replaced. "
                 L"The original CPU/DSP sound engine remains unchanged.\n\n"
                 L"Save a backup of your project before proceeding.",
                 L"Confirm 125A soundbank installation",
                 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            if (!engine_.apply_bank_pair(program_image, timbre_image)) {
                MessageBoxW(window_, L"Unable to change flash during another hardware update.",
                            L"Soundbank not installed", MB_ICONERROR);
                return;
            }
            // Apply is atomic with respect to the emulation thread. The firmware
            // reads the freshly installed data on the next MIDI preset change.
            preview_active_ = false;
            if (library_) SendMessageW(library_, CB_SETCURSEL, 0, 0);
            if (title_) SetWindowTextW(title_,
                L"125A  /  OB-12 PRESET BROWSER  |  ACTIVE MEMORY");
            if (list_) EnableWindow(list_, TRUE);
            last_index_writes_ = -1;
            sync_live_names();
            int selected = engine_.selected_preset();
            if (selected < 0) selected = 256; // default Program A-001
            engine_.select_preset(selected >= 256, (selected % 256) / 128, selected % 128);
            text_status(L"Both complete banks installed into private flash. Presets are ready.");
            return;
        }
        // Keep support for standalone original .syx files through the original UART.
        if (MessageBoxW(window_,
             L"Send this standalone SysEx file through the emulated OB-12 MIDI input? "
             L"The original firmware controls this transfer and may overwrite memory.\n\n"
             L"Proceed only if you know the purpose of this file.",
             L"Original OB-12 SysEx transfer",
             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        first_transfer_writes_ = observed_transfer_writes_ = engine_.flash_writes();
        transfer_started_ = last_flash_activity_ = GetTickCount64();
        transferring_ = true;
        engine_.send_sysex(programs.data(), (int)programs.size());
        text_status(L"Original SysEx transfer queued. Do not change presets or save yet.");
    }
    void create_widgets() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        auto control = [&](LPCWSTR kind, LPCWSTR label, DWORD style, int id) -> HWND {
            HWND hwnd = CreateWindowExW(0, kind, label, style | WS_CHILD | WS_VISIBLE,
                       0, 0, 10, 10, window_, (HMENU)(INT_PTR)id, inst, nullptr);
            if (font_ && hwnd) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font_, TRUE);
            return hwnd;
        };
        title_ = control(L"STATIC", L"125A  /  OB-12 PRESET BROWSER  |  ACTIVE MEMORY", SS_LEFT, 0);
        library_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_library);
        type_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_type);
        bank_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, id_bank);
        search_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id_search);
        list_ = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL |
                                            WS_BORDER | WS_TABSTOP, id_list);
        favorite_ = control(L"BUTTON", L"Toggle favorite", BS_PUSHBUTTON | WS_TABSTOP, id_favorite);
        only_favorites_ = control(L"BUTTON", L"Only favorites", BS_AUTOCHECKBOX | WS_TABSTOP, id_only_favorites);
        import_ = control(L"BUTTON", L"Load Bank Pair", BS_PUSHBUTTON | WS_TABSTOP, id_import);
        close_ = control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, id_close);
        status_ = control(L"STATIC", L"Select an entry. Program Map must be disabled on the OB-12.", SS_LEFT, id_status);
        SendMessageW(type_, CB_ADDSTRING, 0, (LPARAM)L"Programs");
        SendMessageW(type_, CB_ADDSTRING, 0, (LPARAM)L"Timbres");
        SendMessageW(type_, CB_SETCURSEL, 0, 0);
        SendMessageW(bank_, CB_ADDSTRING, 0, (LPARAM)L"Bank A");
        SendMessageW(bank_, CB_ADDSTRING, 0, (LPARAM)L"Bank B");
        SendMessageW(bank_, CB_SETCURSEL, 0, 0);
        find_banks();
        fill();
    }
    void layout(int width, int height) {
        width = std::max(width, 510);
        height = std::max(height, 390);
        // Never enumerate sibling HWNDs: creation/Z-order can differ across Windows hosts.
        if (title_) MoveWindow(title_, 20, 10, width - 40, 25, TRUE);
        if (library_) MoveWindow(library_, 20, 38, width - 40, 350, TRUE);
        if (type_) MoveWindow(type_, 20, 76, 165, 200, TRUE);
        if (bank_) MoveWindow(bank_, 195, 76, 135, 200, TRUE);
        if (search_) MoveWindow(search_, 340, 76, width - 360, 27, TRUE);
        if (list_) MoveWindow(list_, 20, 115, width - 40, height - 221, TRUE);
        if (favorite_) MoveWindow(favorite_, 20, height - 92, 150, 30, TRUE);
        if (only_favorites_) MoveWindow(only_favorites_, 180, height - 92, 155, 30, TRUE);
        if (import_) MoveWindow(import_, width - 305, height - 92, 160, 30, TRUE);
        if (close_) MoveWindow(close_, width - 135, height - 92, 115, 30, TRUE);
        if (status_) MoveWindow(status_, 20, height - 51, width - 40, 40, TRUE);
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
        case WM_CREATE: {
            self->create_widgets();
            RECT rc = {};
            GetClientRect(wnd, &rc);
            self->layout(rc.right - rc.left, rc.bottom - rc.top);
            SetTimer(wnd, 1, 500, nullptr);
            return 0;
        }
        case WM_SIZE: self->layout(LOWORD(l), HIWORD(l)); return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = (MINMAXINFO*)l;
            info->ptMinTrackSize.x = 510;
            info->ptMinTrackSize.y = 390;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == id_library && HIWORD(w) == CBN_SELCHANGE) {
                self->preview_bank();
                return 0;
            }
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
            const ULONGLONG now = GetTickCount64();
            if (self->transferring_) {
                const long writes = self->engine_.flash_writes();
                if (writes != self->observed_transfer_writes_) {
                    self->observed_transfer_writes_ = writes;
                    self->last_flash_activity_ = now;
                }
                if (waiting == 0 && now - self->transfer_started_ > 1500 &&
                    now - self->last_flash_activity_ >= 2000) {
                    self->transferring_ = false;
                    self->preview_active_ = false;
                    if (self->library_) SendMessageW(self->library_, CB_SETCURSEL, 0, 0);
                    if (self->title_) SetWindowTextW(self->title_, L"125A  /  OB-12 PRESET BROWSER  |  ACTIVE MEMORY");
                    if (self->list_) EnableWindow(self->list_, TRUE);
                    self->last_index_writes_ = -1;
                    self->sync_live_names();
                    self->text_status(writes > self->first_transfer_writes_
                        ? L"Soundbank transfer finished; current emulated flash names refreshed."
                        : L"Transfer queue ended, but no firmware flash writes were observed.");
                } else {
                    wchar_t text[128] = {};
                    swprintf(text, 128, L"Transferring SysEx: %d messages pending. Do not change presets.", waiting);
                    self->text_status(text);
                }
            } else if (waiting == 0 && !self->preview_active_) self->sync_live_names();
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
            self->preview_active_ = false;
            self->type_ = self->bank_ = self->search_ = self->list_ = self->status_ = nullptr;
            self->title_ = self->favorite_ = self->only_favorites_ = self->import_ = self->close_ = self->library_ = nullptr;
            return 0;
        }
        return DefWindowProcW(wnd, msg, w, l);
    }
};
