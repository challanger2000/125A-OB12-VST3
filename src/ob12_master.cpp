// OB-12 Master CPU boot harness.
// Memory map from service manual sheets 03/15 and 04/15; peripherals from the MB90611A
// data sheet. Every I/O register has storage and every access is recorded: an unmapped
// register is a gap on the record, never a neutral read.
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include "t6963_cgrom.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

static std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(file)), {});
}

struct Stat { long reads = 0, writes = 0; u32 first_pc = 0; u8 last = 0; };

// Toshiba T6963C, the controller named in the MGLS24064 data sheet.
class T6963 {
public:
    T6963() : vram(0x10000, 0) {}
    u8 status() const { return 0x0f; }
    u8 read_data() { u8 value = vram[pointer]; if (auto_read) pointer++; return value; }
    void write_data(u8 value) {
        if (auto_write) { vram[pointer++] = value; writes++; return; }
        args[0] = args[1];
        args[1] = value;
    }
    void command(u8 code) {
        commands[code]++;
        u16 value = args[0] | args[1] << 8;
        switch (code) {
        case 0x24: pointer = value; break;
        case 0x40: text_home = value; break;
        case 0x41: text_area = args[0]; break;
        case 0x42: graphic_home = value; break;
        case 0x43: graphic_area = args[0]; break;
        case 0xb0: auto_write = true; break;
        case 0xb1: auto_read = true; break;
        case 0xb2: auto_write = auto_read = false; break;
        case 0xc0: vram[pointer++] = args[1]; writes++; break;
        case 0xc2: vram[pointer--] = args[1]; writes++; break;
        case 0xc4: vram[pointer] = args[1]; writes++; break;
        case 0xc1: pointer++; break;
        case 0xc3: pointer--; break;
        default:
            if ((code & 0xf0) == 0x80) mode = code;
            if ((code & 0xf0) == 0x90) display = code;
            if ((code & 0xf0) == 0xf0) {
                u8 bit = 1 << (code & 7);
                if (code & 8) vram[pointer] |= bit; else vram[pointer] &= ~bit;
                writes++;
            }
            break;
        }
    }
    void report() const {
        printf("LCD: %ld writes, mode %02X display %02X text home %04X area %d graphic home %04X area %d\n",
               writes, mode, display, text_home, text_area, graphic_home, graphic_area);
        printf("LCD commands:");
        for (auto& entry : commands) printf(" %02X x%ld", entry.first, entry.second);
        puts("");
        if (text_area) {
            for (int row = 0; row < 8; row++) {
                printf("| ");
                for (int column = 0; column < text_area && column < 40; column++) {
                    u8 code = vram[(u16)(text_home + row * text_area + column)];
                    char c = code + 0x20;
                    putchar(c >= 32 && c < 127 ? c : '.');
                }
                puts(" |");
            }
        }
    }
    // 240 x 64, 30 bytes a row: the graphic layer and the text layer (the built in character ROM,
    // 8 x 8 cells as the 30 byte graphic rows show the font width is 8), combined by the mode
    // command (80: OR, 81: EXOR, 83: AND) as the display command (90-9F) turns them on.
    // The updating procedure writes its page in text.
    bool bitmap(u8* out) const {
        bool graphic = (display & 0x08) || !(display & 0x0c), text = (display & 0x04) && text_area;
        if (graphic_area != 30 && !text) return false;
        for (int row = 0; row < 64; row++) for (int i = 0; i < 30; i++) {
            u8 g = graphic && graphic_area == 30 ? vram[(u16)(graphic_home + row * 30 + i)] : 0;
            if (text && i < text_area) {
                u8 code = vram[(u16)(text_home + (row / 8) * text_area + i)];
                u8 t = code < 0x80 ? t6963_cgrom[code * 8 + row % 8] : 0;  // 80-FF: CG RAM, not used here
                switch (mode & 7) {
                case 1: g ^= t; break;
                case 3: g &= t; break;
                default: g |= t; break;
                }
            }
            out[row * 30 + i] = g;
        }
        return true;
    }
    void save_pbm(const std::string& path) const {
        u8 image[30 * 64];  // what the panel shows: both layers
        if (!bitmap(image)) return;
        std::ofstream file(path, std::ios::binary);
        file << "P4\n240 64\n";
        file.write((const char*)image, sizeof image);
    }
    long writes = 0;

private:
    std::vector<u8> vram;
    u8 args[2] = {0, 0};
    u16 pointer = 0, text_home = 0, graphic_home = 0;
    int text_area = 0, graphic_area = 0;
    u8 mode = 0, display = 0;
    bool auto_write = false, auto_read = false;
    std::map<int, long> commands;
};

class Master : public f2mc16_device, public address_space {
public:
    Master() : f2mc16_device(machine_config(), nullptr, nullptr, 16000000) {
        flash.assign(0x100000, 0xff);
        ram.assign(0x40000, 0);
        memset(internal, 0, sizeof internal);
        memset(pot_live, 0xff, sizeof pot_live);
        memset(io, 0, sizeof io);
        io[0xa1] = 0xfb;  // CKSCR reset value
        for (int i = 0xb0; i < 0xc0; i++) io[i] = 7;  // ICR: interrupts disabled
        timer[0].base = 0x38; timer[0].vector = 0x1d;
        timer[1].base = 0x3c; timer[1].vector = 0x1e;
        // Vectors from the data sheet's interrupt table.
        uart[0].base = 0x20; uart[0].tx_vector = 0x18; uart[0].rx_vector = 0x27; uart[0].name = "UART0 LED";
        uart[1].base = 0x24; uart[1].tx_vector = 0x1a; uart[1].rx_vector = 0x25; uart[1].name = "UART1 MIDI";
        uart[2].base = 0x44; uart[2].tx_vector = 0x21; uart[2].rx_vector = 0x23; uart[2].name = "UART2 Slave";
    }
    void load(const std::string& rom) {
        place(rom + "/12cpu152-CPU-v1.52-FINAL.bin", 0x00000);
        place(rom + "/system-parameters.bin", 0xfa000);
        place(rom + "/boot_152-emergency-boot.bin", 0xfc000);
        place(rom + "/presets/12tmbFS.bin", 0xc0000);
        place(rom + "/presets/12prgFS.bin", 0xd0000);
        // The font sector is not among the supplied files. tools/make_font.py builds a replacement.
        if (std::ifstream(rom + "/font_sector.bin").good()) place(rom + "/font_sector.bin", 0xf0000);
    }

    // Test probe: runs of printable bytes read from ascending flash addresses, for what the panel shows.
    FILE* text_log = nullptr;
    // Several streams, because the text routine reads the font between two characters.
    struct TextRun { u32 next, start; long seen; std::string text; };
    std::vector<TextRun> text_runs;
    long text_reads = 0;
    void note_text(u32 address, u8 data) {
        if (address < 0xf20000 || address >= 0xf54000) return;
        text_reads++;
        bool printable = data >= 0x20 && data < 0x7f;
        for (size_t i = 0; i < text_runs.size(); i++) {
            auto& r = text_runs[i];
            if (r.next == address) {
                if (printable) { r.text.push_back((char)data); r.next++; r.seen = text_reads; return; }
                if (r.text.size() >= 3) fprintf(text_log, "%.4f S %06X %s\n", pot_clock, r.start, r.text.c_str());
                text_runs.erase(text_runs.begin() + i);
                return;
            }
        }
        for (size_t i = 0; i < text_runs.size();) {
            if (text_reads - text_runs[i].seen > 400) text_runs.erase(text_runs.begin() + i); else i++;
        }
        if (printable && text_runs.size() < 16) text_runs.push_back({address + 1, address, text_reads, std::string(1, (char)data)});
    }
    u8 slow_read(u32 address) override {
        address &= 0xffffff;
        if (address < 0x100) {
            if (!lean) { Stat& s = touch(iostat, address); s.reads++; }
            return io_read(address);
        }
        if (address < 0x500) return internal[address];
        if (address >= 0xf00000) {
            if (flash[address - 0xf00000] == 0xff && address >= 0xf54000 && address < 0xffc000) {
                Stat& s = touch(blank, address & 0xfff000);
                s.reads++;
            }
            if (address >= 0xff0002 && address < 0xff0102) font_reads[(address - 0xff0002) / 4]++;
            if (text_log) note_text(address, flash[address - 0xf00000]);
            return flash[address - 0xf00000];
        }
        if (address >= 0xe00000) return ram[address & 0x3ffff];
        if (address == 0x68ff00) return lcd.read_data();
        if (address == 0x68ff01) return lcd.status();
        Stat& s = touch(ext, address & 0xfffff0);
        s.reads++;
        return 0xff;
    }
    void slow_write(u32 address, u8 data) override {
        address &= 0xffffff;
        if (address < 0x100) {
            if (!lean) { Stat& s = touch(iostat, address); s.writes++; s.last = data; }
            io_write(address, data);
            return;
        }
        if (address < 0x500) { internal[address] = data; return; }
        if (address >= 0xf00000) { Stat& s = touch(flashw, address & 0xff0000); s.writes++; s.last = data; flash_write(address - 0xf00000, data); return; }
        if (address >= 0xe00000) {
            u32 cell = address & 0x3ffff;
            if (watch_from && steps > watch_from && cell >= watch_lo && cell < watch_hi && watch_log.size() < 24) {
                char line[96];
                snprintf(line, sizeof line, "watch step %ld PC %06X write %06X = %02X PS %04X", steps, pc, address, data, ob12_ps());
                watch_log.push_back(line);
            }
            if (mwatch_hi && cell >= mwatch_lo && cell < mwatch_hi && pc >= mwatch_pc_lo && pc < mwatch_pc_hi) printf("master watch %.4f pc %06X W %06X %02X\n", cycles_total / 16e6, pc, address, data);  // study: OB12_MASTER_WATCH
            ram[cell] = data;
            return;
        }
        if (address == 0x68ff00) { lcd.write_data(data); return; }
        if (address == 0x68ff01) { lcd.command(data); return; }
        if ((address & 0xfffff0) == 0x68ff80) led[address & 15] = led_shift;  // CS2 strobe: latch n takes the shifted byte
        if ((address & 0xfffff0) == 0x68ff80 && led_log) fprintf(led_log, "%.4f %06X %02X %06X img %02X blink %02X phase %02X\n", cycles_total / 16e6, address, led_shift, pc,
            ram[(0xec5b09 + (address & 15)) & 0x3ffff], ram[(0xec5afc + (address & 15)) & 0x3ffff], ram[0xec5b30 & 0x3ffff]);  // study: OB12_LED_LOG, with the firmware's two LED images
        Stat& s = touch(ext, address & 0xfffff0);
        s.writes++;
        s.last = data;
    }

    long font_reads[64] = {};  // which fonts the firmware asks for
    bool lean = false;  // skip the per instruction statistics
    // Lean running for the engine: no statistics, code and RAM read and RAM written through the
    // direct windows (emu.h), and the idle loop taken in one go. Not while a study watches memory.
    void go_lean() {
        lean = true;
        ob12_idle_skip(true);
        if (text_log || watch_from || mwatch_hi) return;
        ob12_quiet_hook(true);  // pc and steps are then not kept: only the studies above read them
        read_window[0] = {0xf00000, 0x100000, 0xfffff, flash.data()};
        read_window[1] = {0xe00000, 0x100000, 0x3ffff, ram.data()};
        write_window = {0xe00000, 0x100000, 0x3ffff, ram.data()};
    }
    void tick(int cycles) {
        cycles_total += cycles;
        for (auto& t : timer) t.advance(cycles);
        for (auto& u : uart) u.advance(cycles);
        // Time-base timer: counts the 4 MHz oscillator, 4 machine cycles per count.
        static const int shift[4] = {12, 14, 16, 19};
        timebase += cycles;
        s64 interval = (s64)4 << shift[io[0xa9] & 3];
        if (timebase >= interval) { timebase -= interval; io[0xa9] |= 0x08; }
        if (int_period && (int_timer -= cycles) <= 0) { int_timer = int_period; io[0x29] |= int_mask; }
        if (adc_busy > 0 && (adc_busy -= cycles) <= 0) {
            adc_busy = 0;
            io[0x2d] |= 0x40;  // INT
            adc_channel++;
        } else if (!adc_busy && (io[0x2d] & 0x80) && !(io[0x2d] & 0x40)) {
            // Single mode scans ANS..ANE; the next channel starts once the request is served.
            if (adc_channel <= (io[0x2c] & 7)) adc_busy = 500; else io[0x2d] &= ~0x80;
        }
        update_irq();
    }

    Uart& link() { return uart[2]; }
    // Test input: from this time on, every pot moves slowly and differently.
    double pot_sweep_from = 0;
    u16 pot_rest = 0x80;  // where pots sit; the firmware reads only the low byte, 8 bits
    u8 led[16] = {};  // each LED latch, the byte as the firmware shifted it, bit 0 first
    u8 led_shift = 0;
    FILE* led_log = getenv("OB12_LED_LOG") ? fopen(getenv("OB12_LED_LOG"), "w") : nullptr;  // last byte written to each CS2 LED latch, 68FF80-8F
    double pot_clock = -1;  // sample time from the system, the CPU clock runs a little ahead of it
    struct KeyStep { double from, to; int row; u8 mask; };
    std::vector<KeyStep> key_steps;  // test input: a key held between two times
    long key_reads[16] = {};
    u8 key_live[16] = {};        // live input: keys held now
    short pot_live[8][8];        // live input: pot values, -1 when never moved
    u8 key_value(int row) {
        key_reads[row]++;
        u8 value = (u8)~key_live[row];
        for (auto& k : key_steps) if (k.row == row && pot_clock >= k.from && pot_clock < k.to) value &= ~k.mask;
        return value;
    }
    // Test input: the data encoder. Falling edges raise INT0 and INT1, as ELVR is set by the firmware.
    u8 enc_lines = 3;
    void enc_set(int a, int b) {
        u8 next = (u8)(a | b << 1);
        if ((enc_lines & 1) && !a) io[0x29] |= 1;
        if ((enc_lines & 2) && !b) io[0x29] |= 2;
        enc_lines = next;
    }
    double pot_sweep_to = 0;  // pots stay where they are from this time on
    struct PotStep { double at; int channel, mux; u16 value; };
    std::vector<PotStep> pot_steps;  // test input: one pot jumps to a value
    u16 pot_value() const {
        double now = pot_clock >= 0 ? pot_clock : cycles_total / 16e6;
        int channel = (adc_channel - 1) & 7;
        int mux = io[0x05] & 7;  // POTA[2:0], sheet 02/15
        // The mod wheel (ADC 0, mux 6) rests at its bottom, not mid way like a panel pot: half a wheel
        // put A-015's amp LFO on from power on. The pitch wheel (mux 4) rests in the middle.
        u16 stepped = channel == 0 && mux == 6 ? 0 : pot_rest;
        if (pot_live[channel][mux] >= 0) return (u16)pot_live[channel][mux];
        for (auto& step : pot_steps) if (now >= step.at && step.channel == channel && step.mux == mux) stepped = step.value;
        if (pot_sweep_from <= 0 || now < pot_sweep_from) return stepped;
        if (pot_sweep_to > 0 && now > pot_sweep_to) now = pot_sweep_to;
        double phase = (now - pot_sweep_from) * 1.3 + channel * 0.7 + mux * 0.31;
        return (u16)(128 + 120 * sin(phase));
    }
    // Test input: queue bytes on the MIDI input.
    void midi_in(const std::vector<u8>& bytes) { uart[1].rx_queue.insert(uart[1].rx_queue.end(), bytes.begin(), bytes.end()); }
    bool midi_idle() const { return uart[1].rx_queue.empty(); }  // every byte handed to the UART
    std::vector<u8>& midi_sent() { return uart[1].head; }  // MIDI OUT, for the engine to drain
    // Test input: raise external interrupts periodically.
    int int_period = 0, int_timer = 0;
    u8 int_mask = 0;
    long watch_from = 0;
    u32 watch_lo = 0, watch_hi = 0;
    u32 mwatch_lo = 0, mwatch_hi = 0, mwatch_pc_lo = 0, mwatch_pc_hi = 0x1000000;  // study: print RAM writes to these cells (address & 3FFFF)
    std::vector<std::string> watch_log;
    long steps = 0;
    s64 cycles_total = 0;
    long irqs_taken = 0;
    T6963 lcd;

    // Execution count of every PC, for offline coverage analysis.
    void save_slave_stream(const std::string& path) {
        std::ofstream file(path, std::ios::binary);
        file.write((const char*)uart[2].head.data(), uart[2].head.size());
    }
    void save_ram(const std::string& path) {
        std::ofstream file(path, std::ios::binary);
        file.write((const char*)ram.data(), ram.size());
    }
    void save_hot(const std::string& path) {
        std::map<u32, long> sorted(hot.begin(), hot.end());
        std::ofstream file(path);
        for (auto& entry : sorted) file << std::hex << entry.first << ' ' << std::dec << entry.second << '\n';
    }

    void report() {
        printf("instructions %ld, cycles %lld (%.2f s), interrupts %ld, last PC %06X\n", steps, cycles_total, cycles_total / 16e6, irqs_taken, pc);
        std::vector<std::pair<long, u32>> top;
        for (auto& entry : hot) top.push_back({entry.second, entry.first});
        std::sort(top.rbegin(), top.rend());
        printf("hottest PCs:");
        for (size_t i = 0; i < top.size() && i < 10; i++) printf(" %06X x%ld", top[i].second, top[i].first);
        printf("\ndistinct PCs %zu\nlast PCs:", hot.size());
        for (int i = 0; i < 16; i++) printf(" %06X", trail[(trail_at + i) % 16]);
        puts("");
        for (auto& line : switches) puts(line.c_str());
        for (auto& line : watch_log) puts(line.c_str());
        for (auto& line : jumps) puts(line.c_str());
        // RTOS view: TCB table at EC0E6E, 8 bytes per task, first word is the saved SP.
        printf("RTOS current %02X ready %04X %04X\n", read_word(0xec0e5e), read_word(0xec0e6a), read_word(0xec0e6c));
        for (int task = 0; task < 9; task++) {
            u32 tcb = 0xec0e6e + task * 8;
            u16 sp = read_word(tcb);
            printf("task %d tcb", task);
            for (int i = 0; i < 8; i += 2) printf(" %04X", read_word(tcb + i));
            printf(" sp %04X code addresses on stack:", sp);
            int shown = 0;
            for (u32 at = sp; at < (u32)sp + 0x60 && shown < 12; at += 2) {
                u32 low = read_word(0xec0000 | at), high = read_word(0xec0000 | (at + 2)) & 0xff;
                if (high >= 0xf0 && high <= 0xf5) { printf(" %02X%04X", high, low); shown++; }
            }
            puts("");
        }
        lcd.report();
        for (auto& u : uart) {
            printf("%s: %ld bytes:", u.name, u.sent);
            for (size_t i = 0; i < u.head.size() && i < 48; i++) printf(" %02X", u.head[i]);
            puts("");
        }
        printf("vectors taken:");
        for (auto& entry : vectors) printf(" %02X x%ld", entry.first, entry.second);
        printf("\nEI2OS transfers:");
        for (auto& entry : ei2os_count) printf(" %02X x%ld", entry.first, entry.second);
        puts("\nI/O registers (addr reads writes last firstPC):");
        for (auto& entry : iostat)
            printf("  %02X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        puts("external (16-byte block reads writes last firstPC):");
        for (auto& entry : ext)
            printf("  %06X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        puts("reads of blank flash (4 KB block reads firstPC):");
        for (auto& entry : blank)
            printf("  %06X r%ld pc %06X\n", entry.first, entry.second.reads, entry.second.first_pc);
        printf("flash: %ld sector erases, %ld words programmed\n", flash_erases, flash_programs);
        puts("flash writes:");
        for (auto& entry : flashw)
            printf("  %06X w%ld last %02X pc %06X\n", entry.first, entry.second.writes, entry.second.last, entry.second.first_pc);
    }

protected:
    void debugger_instruction_hook(u32 address) override {
        if (address < 0xe00000) fatalerror("PC left memory: %06X", address);
        if (lean) { pc = address; steps++; return; }
        if (address == 0xf01c76 || address == 0xf00899) {
            char line[160];
            u32 sp = ob12_ssb() << 16 | ob12_ssp();
            int n = snprintf(line, sizeof line, "%06X step %ld PS %04X SSP %02X:%04X USP %02X:%04X DTB %02X task %02X frame:", address, steps, ob12_ps(), ob12_ssb(), ob12_ssp(), ob12_usb(), ob12_usp(), ob12_dtb(), read_byte(0xec0e5e));
            for (int i = 0; i < 12; i += 2) n += snprintf(line + n, sizeof line - n, " %04X", read_word(sp + i));
            switches.push_back(line);
            if (switches.size() > 14) switches.erase(switches.begin());
        }
        if (abs((int)ob12_ssp() - (int)last_ssp) > 0x200) {
            char line[96];
            snprintf(line, sizeof line, "SSP jump step %ld at %06X after %06X: %04X -> %04X PS %04X", steps, address, pc, last_ssp, ob12_ssp(), ob12_ps());
            jumps.push_back(line);
            if (jumps.size() > 12) jumps.erase(jumps.begin());
        }
        last_ssp = ob12_ssp();
        pc = address;
        steps++;
        hot[address]++;
        trail[trail_at] = address;
        trail_at = (trail_at + 1) % 16;
    }
    int standard_irq_callback(int level, u32) override {
        int vector = best_vector();
        int icr = (vector - 11) / 2;
        // The core captured the level earlier; retry if the winning source has changed.
        if (!vector || (io[0xb0 + icr] & 7) != (level >> 13)) { update_irq(); return 0; }
        if ((io[0xb0 + icr] & 0x08) && !ei2os_transfer(vector, icr)) { update_irq(); return 0; }
        irqs_taken++;
        vectors[vector]++;
        return vector;
    }

private:
    // Extended intelligent I/O service: one transfer per request, descriptor at 0x100 + 8 * ICS.
    // Returns true when the interrupt itself must now be taken.
    bool ei2os_transfer(int vector, int icr) {
        ei2os_count[vector]++;
        u32 isd = 0x100 + ics[icr] * 8;
        u8 iscs = read_byte(isd + 3);
        u32 bap = read_word(isd) | read_byte(isd + 2) << 16;
        u16 ioa = read_word(isd + 4);
        u16 dc = read_word(isd + 6);
        bool word = iscs & 0x08, to_io = iscs & 0x02;
        u32 from = to_io ? bap : ioa, to = to_io ? ioa : bap;
        if (word) write_word(to, read_word(from)); else write_byte(to, read_byte(from));
        if (!(iscs & 0x04)) write_word(isd, bap + (word ? 2 : 1));
        if (!(iscs & 0x10)) write_word(isd + 4, ioa + (word ? 2 : 1));
        dc--;
        write_word(isd + 6, dc);
        if (vector == 0x1f) io[0x2d] &= ~0x40;  // I2OSCLR clears the A/D request
        if (dc == 0) {
            io[0xb0 + icr] = (io[0xb0 + icr] & ~0x38) | 0x10;  // ISE off, S = count complete
            return true;
        }
        return false;
    }

    bool source_pending(int vector) const {
        for (auto& t : timer) if (t.vector == vector) return t.pending();
        for (auto& u : uart) {
            if (u.tx_vector == vector) return u.tx_pending();
            if (u.rx_vector == vector) return u.rx_pending();
        }
        if (vector == 0x22) return (io[0xa9] & 0x18) == 0x18;  // time base
        if (vector == 0x2a) return io[0x9f] & 1;  // delayed interrupt module, DIRR
        // External interrupts INT0-7: vectors 0B, 0D ... 19, enabled by ENIR, latched in EIRR.
        if (vector >= 0x0b && vector <= 0x19 && (vector & 1)) {
            u8 bit = 1 << ((vector - 0x0b) / 2);
            return io[0x28] & io[0x29] & bit;
        }
        if (vector == 0x1f) return (io[0x2d] & 0x60) == 0x60;  // A/D
        return false;
    }
    // The lowest level number wins; on a tie the lowest vector does.
    int best_vector() const {  // the same answer as going through source_pending() for 0B to 2A
        PendingScan p;
        for (auto& t : timer) p.add(t.vector, t.pending());
        for (auto& u : uart) { p.add(u.tx_vector, u.tx_pending()); p.add(u.rx_vector, u.rx_pending()); }
        p.add(0x22, (io[0xa9] & 0x18) == 0x18);
        p.add(0x2a, io[0x9f] & 1);
        for (int vector = 0x0b; vector <= 0x19; vector += 2) p.add(vector, io[0x28] & io[0x29] & (1 << ((vector - 0x0b) / 2)));
        p.add(0x1f, (io[0x2d] & 0x60) == 0x60);
        return p.best(io);
    }
    void update_irq() {
        int vector = best_vector();
        set_irq_level(vector ? io[0xb0 + (vector - 11) / 2] & 7 : 7);
    }

    u8 io_read(u32 address) {
        // Ports 1-A: output latches where DDR is set, pin levels elsewhere.
        // Inputs idle high: sheet 02/15 pulls the button lines up through AR1.
        if (address >= 0x01 && address <= 0x0a) {
            u8 ddr = io[address + 0x10];
            // P20-27 read BUT[7:0]; P30-33 select the button bar, sheets 02/15, 04/15.
            u8 pins = address == 0x02 ? key_value(io[0x03] & 15) : 0xff;
            if (address == 0x07) pins = (pins & 0xfc) | enc_lines;  // P70, P71: encoder, sheet 02/15
            return (io[address] & ddr) | (pins & ~ddr);
        }
        for (auto& t : timer) {
            if (address == (u32)t.base) return t.tmcsr & 0xfe;
            if (address == (u32)t.base + 1) return t.tmcsr >> 8;
            if (address == (u32)t.base + 2) return t.count & 0xff;
            if (address == (u32)t.base + 3) return t.count >> 8;
        }
        for (auto& u : uart) {
            if (address == (u32)u.base) return u.smr;
            if (address == (u32)u.base + 1) return u.scr;
            if (address == (u32)u.base + 2) return u.read();
            if (address == (u32)u.base + 3) return u.ssr;
        }
        if (address == 0x29 && rmw()) return 0xff;  // EIRR: bit operations must not clear other flags
        if (address >= 0xc0 && address <= 0xcf) return 0xff;  // keyboard, sheet 04/15, no keys down
        if (address == 0x2e) return pot_value() & 0xff;  // A/D result
        if (address == 0x2f) return pot_value() >> 8;
        return io[address];
    }
    void io_write(u32 address, u8 data) {
        // LED data is shifted out on port 8, bit 5 data, bit 6 clock, LSB first (F0F1D9); a write to
        // 68FF80+n then loads the byte into latch n. Rising clock edges shift in the data bit.
        if (address == 0x08) {
            if ((data & 0x40) && !(io[0x08] & 0x40)) led_shift = (u8)((led_shift >> 1) | ((data & 0x20) ? 0x80 : 0));
        }
        for (auto& t : timer) {
            if (address == (u32)t.base) {
                bool trg = data & 1;
                t.tmcsr = (t.tmcsr & 0xff00) | (data & 0xfe);
                if (trg && (t.tmcsr & 2)) t.trigger();
                return;
            }
            if (address == (u32)t.base + 1) { t.tmcsr = (t.tmcsr & 0x00ff) | data << 8; return; }
            if (address == (u32)t.base + 2) { t.reload = (t.reload & 0xff00) | data; return; }
            if (address == (u32)t.base + 3) { t.reload = (t.reload & 0x00ff) | data << 8; return; }
        }
        for (auto& u : uart) {
            if (address == (u32)u.base) { u.smr = data; return; }
            if (address == (u32)u.base + 1) { u.scr = data; return; }
            if (address == (u32)u.base + 2) { u.write(data); return; }
            if (address == (u32)u.base + 3) { u.ssr = (u.ssr & 0xfc) | (data & 3); return; }
        }
        switch (address) {
        case 0x29:  // EIRR: writing 0 clears a request.
            io[address] &= data;
            break;
        case 0xa1:  // CKSCR: MCM follows MCS once the clock has switched.
            io[address] = (data & 0xbf) | (data & 0x04 ? 0x40 : 0);
            break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            ics[address & 15] = data >> 4;
            io[address] = (io[address] & 0xf0) | (data & 0x0f);
            break;
        case 0xa9:  // TBTC: writing 0 to TBOF clears it.
            io[address] = (data & ~0x08) | (data & io[address] & 0x08);
            break;
        case 0x2d:  // ADCS high: BUSY and INT can only be cleared; STRT starts a conversion.
            io[address] = (data & 0x3d) | (data & io[address] & 0xc0);
            if (data & 0x02) { adc_busy = 500; adc_channel = (io[0x2c] >> 3) & 7; io[address] |= 0x80; }
            break;
        default:
            io[address] = data;
            break;
        }
    }

    // 29F800T in word mode, sheet 03/15. Commands complete at once, so status polls see done.
    // Word address 5555/2AAA appear as byte address AAAA/5554 because A0 is unconnected.
    FILE* flash_log = getenv("OB12_FLASH_LOG") ? fopen(getenv("OB12_FLASH_LOG"), "w") : nullptr;  // study
    void flash_write(u32 offset, u8 data) {
        if (flash_log) fprintf(flash_log, "%.6f pc %06X %05X %02X state %d\n", cycles_total / 16e6, pc, offset, data, flash_state);
        if (flash_high) {  // second byte of a word program
            flash_high = false;
            flash[offset] &= data;
            flash_state = 0;
            return;
        }
        if (offset & 1) return;  // high byte of a command word
        u32 low = offset & 0xffff;
        // F0 is the reset command only outside a word program: there it is data. Taking it as reset
        // dropped every program word whose low byte is F0 (the factory programs stopped at D1332).
        if (data == 0xf0 && flash_state != 6) { flash_state = 0; return; }
        switch (flash_state) {
        case 0: case 3: flash_state = (low == 0xaaaa && data == 0xaa) ? flash_state + 1 : 0; break;
        case 1: case 4: flash_state = (low == 0x5554 && data == 0x55) ? flash_state + 1 : 0; break;
        case 2:
            if (data == 0xa0) flash_state = 6;
            else if (data == 0x80) flash_state = 3;
            else flash_state = 0;
            break;
        case 5:
            if (data == 0x30) {
                u32 start = offset & 0xf0000, size = 0x10000;
                if (offset >= 0xfc000) { start = 0xfc000; size = 0x4000; }
                else if (offset >= 0xfa000) { start = 0xfa000; size = 0x2000; }
                else if (offset >= 0xf8000) { start = 0xf8000; size = 0x2000; }
                else if (offset >= 0xf0000) { start = 0xf0000; size = 0x8000; }
                std::fill(flash.begin() + start, flash.begin() + start + size, 0xff);
                flash_erases++;
            }
            flash_state = 0;
            break;
        case 6:
            flash[offset] &= data;
            flash_programs++;
            flash_high = true;
            break;
        }
    }

    void place(const std::string& path, u32 offset) {
        auto data = read_file(path);
        std::copy(data.begin(), data.end(), flash.begin() + offset);
    }
    Stat& touch(std::map<u32, Stat>& table, u32 key) {
        auto found = table.find(key);
        if (found != table.end()) return found->second;
        Stat& s = table[key];
        s.first_pc = pc;
        return s;
    }

public:  // the engine keeps the flash between sessions (the unit's only lasting memory)
    std::vector<u8> flash;
    long flash_erases = 0, flash_programs = 0;
private:
    std::vector<u8> ram;
    u8 internal[0x500];
    u8 io[0x100];
    ReloadTimer timer[2];
    Uart uart[3];
    s64 timebase = 0;
    int adc_busy = 0, adc_channel = 0;
    int flash_state = 0;
    bool flash_high = false;
    u8 ics[16] = {};
    std::map<int, long> ei2os_count;
    std::vector<std::string> switches, jumps;
    u16 last_ssp = 0;
    u32 pc = 0;
    u32 trail[16] = {};
    int trail_at = 0;
    std::unordered_map<u32, long> hot;
public:
    std::map<u32, Stat> iostat, ext, flashw, blank;
    std::map<int, long> vectors;
};

#ifndef OB12_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: ob12_master <rom dir> <instructions> [lcd.pbm]"); return 2; }
    auto* cpu = new Master();
    cpu->load(argv[1]);
    long limit = atol(argv[2]);
    if (argc > 6) {  // watch: from-step low-address high-address (24-bit)
        cpu->watch_from = atol(argv[4]);
        cpu->watch_lo = strtoul(argv[5], nullptr, 16) & 0x3ffff;
        cpu->watch_hi = strtoul(argv[6], nullptr, 16) & 0x3ffff;
    }
    if (getenv("OB12_INT_MASK")) {
        cpu->int_mask = strtoul(getenv("OB12_INT_MASK"), nullptr, 16);
        cpu->int_period = getenv("OB12_INT_PERIOD") ? atoi(getenv("OB12_INT_PERIOD")) : 80000;
    }
    cpu->attach(cpu);
    cpu->reset();
    try {
        long midi_at = getenv("OB12_MIDI_AT") ? atol(getenv("OB12_MIDI_AT")) : 0;
        while (cpu->steps < limit) {
            if (midi_at && cpu->steps >= midi_at) { cpu->midi_in({0x90, 0x3c, 0x64}); midi_at = 0; }
            const int slice = 200;
            cpu->run(slice);
            cpu->tick(cpu->executed());
        }
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    cpu->report();
    if (argc > 3) cpu->lcd.save_pbm(argv[3]);
    cpu->save_hot("build/hot.txt");
    cpu->save_slave_stream("build/master_to_slave.bin");
    cpu->save_ram("build/ram.bin");
    return 0;
}
#endif
