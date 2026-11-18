// OB-12 Slave CPU harness.
// The resident Slave boot block is not in any firmware file, so this harness replaces it:
// a short start-up stub sets the banks and stack and jumps to the firmware's main at FC0500.
// The four resident routines the firmware calls are logged and return at once.
// Memory map from sheets 06/15 and 07/15. RAM bank EF comes from the firmware's own pointers.
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>

static std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(file)), {});
}

struct Stat { long reads = 0, writes = 0; u32 first_pc = 0; u8 last = 0; };

class Slave : public f2mc16_device, public address_space {
public:
    Slave() : f2mc16_device(machine_config(), nullptr, nullptr, 16000000) {
        flash.assign(0x40000, 0xff);
        ram.assign(0x10000, 0);
        memset(internal, 0, sizeof internal);
        memset(io, 0, sizeof io);
        io[0xa1] = 0xfb;
        for (int i = 0xb0; i < 0xc0; i++) io[i] = 7;
        timer[0].base = 0x38; timer[0].vector = 0x1d;
        timer[1].base = 0x3c; timer[1].vector = 0x1e;
        uart[0].base = 0x20; uart[0].tx_vector = 0x18; uart[0].rx_vector = 0x27; uart[0].name = "UART0 debug";
        uart[1].base = 0x24; uart[1].tx_vector = 0x1a; uart[1].rx_vector = 0x25; uart[1].name = "UART1 DAC";
        uart[2].base = 0x44; uart[2].tx_vector = 0x21; uart[2].rx_vector = 0x23; uart[2].name = "UART2 Master";
    }
    void load(const std::string& rom) {
        auto image = read_file(rom + "/12dsp111-DSP-v1.11-FINAL.bin");
        std::copy(image.begin(), image.end(), flash.begin());
        // Every vector defaults to IsrNull at FC0002 until mapped.
        for (int vector = 0; vector < 256; vector++) set_vector(vector, 0xfc0002);
        set_vector(0x23, 0xfc0100);  // UART2 receive, identified from the code at FC0100
        set_vector(0x21, 0xfc0200);  // UART2 transmit, identified from the code at FC0200
        // FC0300 clears PUF0 and PUF1, and the firmware masks it through ICR08: it is the PPG tick.
        set_vector(0x1b, 0xfc0300);
        // FC0400 clears UF of reload timer 1 and counts D148; every second call asks for a glide step.
        set_vector(0x1e, 0xfc0400);
        io[0xb9] = 6;
        timer[1].tmcsr = 0x08;  // INTE, set by the missing resident boot
        io[0xb8] = 6;  // the level the firmware restores after masking
        io[0x30] = 0x10;  // PIE0, set by the missing resident boot
        // The resident routines evidently set up the serial ports and interrupt levels;
        // the firmware never touches UART2 itself. Levels here are assumptions.
        uart[2].ssr |= 0x02;  // RIE
        io[0xbb] = 3;
        io[0xbc] = 2;
        // Reset stub at FFF000.
        static const u8 stub[] = {
            0x42, 0xef, 0x6f, 0x12,        // SSB = EF
            0x42, 0xef, 0x6f, 0x13,        // USB = EF
            0x42, 0xef, 0x6f, 0x10,        // DTB = EF
            0x42, 0xef, 0x6f, 0x11,        // ADB = EF
            0x4a, 0xf0, 0xff, 0x47,        // SP = FFF0
            0x0a, 0x00,                    // RP = 0
            0x1a, 0x07,                    // ILM = 7
            0x63, 0x00, 0x05, 0xfc,        // JMPP FC0500
        };
        std::copy(stub, stub + sizeof stub, flash.begin() + 0x3f000);
        poke_vector_reset(0xfff000);
        for (u32 address : {0xffd3a0u, 0xffd215u, 0xffd12eu, 0xffd1bdu}) flash[address - 0xfc0000] = 0x66;  // RETP
    }
    Uart& link() { return uart[2]; }
    std::function<void(u8, u8)> on_dsp_byte;   // selected mask, data
    std::function<void()> on_dsp_select;       // NDCS changed
    void feed(const std::vector<u8>& bytes) { uart[2].rx_queue.insert(uart[2].rx_queue.end(), bytes.begin(), bytes.end()); }

    u8 slow_read(u32 address) override {
        address &= 0xffffff;
        if (byte_log && ((pc >= 0xfc29b0 && pc < 0xfc29f8) || pc == 0xfd1791)) fprintf(byte_log, "read %06X at %06X\n", address, pc);  // study: where the DSP sender reads
        if (address < 0x100) { if (!lean) { Stat& s = touch(iostat, address); s.reads++; } return io_read(address); }
        if (address < 0x500) return internal[address];
        if (address >= 0xfc0000) return flash[address - 0xfc0000];
        if (address >= 0xe00000 && address < 0xf00000) { if (trace_file) fprintf(trace_file, "R %06X %02X\n", address, ram[address & 0xffff]); return ram[address & 0xffff]; }
        Stat& s = touch(ext, address & 0xfffff0);
        s.reads++;
        return 0xff;
    }
    void slow_write(u32 address, u8 data) override {
        address &= 0xffffff;
        if (address < 0x100) { if (!lean) { Stat& s = touch(iostat, address); s.writes++; s.last = data; } io_write(address, data); return; }
        if (address < 0x500) { if (address >= watch_lo && address < watch_hi && cycles_total >= watch_from) printf("slave watch %.4f idx %02X pc %06X W %06X %02X\n", (double)cycles_total / 16e6, ram[0x7b9e], pc, address, data); internal[address] = data; return; }
        if (address >= 0xfc0000) { Stat& s = touch(flashw, address & 0xffff00); s.writes++; s.last = data; return; }
        if (address >= watch_lo && address < watch_hi && cycles_total >= watch_from) printf("slave watch %.4f idx %02X pc %06X W %06X %02X\n", (double)cycles_total / 16e6, ram[0x7b9e], pc, address, data);
        if (address >= 0xe00000 && address < 0xf00000) { if (trace_file) fprintf(trace_file, "W %06X %02X\n", address, data); ram[address & 0xffff] = data; return; }
        if (address == 0x68ff80) { dsp_byte(data); return; }
        Stat& s = touch(ext, address & 0xfffff0);
        s.writes++;
        s.last = data;
    }

    u32 watch_lo = 0, watch_hi = 0;
    s64 watch_from = 0;  // study: watch only from this cycle on (OB12_SLAVE_WATCH_FROM, seconds)  // study: print writes in this range
    FILE* trace_file = nullptr;  // study: PCs and RAM accesses
    bool lean = false;  // skip the per instruction statistics
    // Lean running for the engine: see go_lean in ob12_master.cpp.
    void go_lean() {
        lean = true;
        ob12_idle_skip(true);
        if (byte_log || trace_file || watch_hi) return;
        ob12_quiet_hook(true);  // also leaves out the call log at FFD3A0, FFD215, FFD12E, FFD1BD
        read_window[0] = {0xfc0000, 0x40000, 0x3ffff, flash.data()};
        read_window[1] = {0xe00000, 0x100000, 0xffff, ram.data()};
        write_window = {0xe00000, 0x100000, 0xffff, ram.data()};
    }
    void tick(int cycles) {
        cycles_total += cycles;
        for (auto& t : timer) t.advance(cycles);
        for (auto& u : uart) u.advance(cycles);
        static const int shift[4] = {12, 14, 16, 19};
        timebase += cycles;
        s64 interval = (s64)4 << shift[io[0xa9] & 3];
        if (timebase >= interval) { timebase -= interval; io[0xa9] |= 0x08; }
        update_irq();
    }
    void reload_tick() { timer[1].tmcsr |= 0x04; }  // UF; rate unknown, taken as 1 ms
    void glide_tick() { ram[0xd15a] = 1; }  // what FC0400 does every second call, for OB12_NO_RELOAD
    void ppg_tick() { io[0x30] |= 0x08; }  // PUF0; the rate is set by the missing boot

    long steps = 0;
    s64 cycles_total = 0;

    void report() {
        printf("instructions %ld, cycles %lld (%.2f s), last PC %06X PS %04X\n", steps, cycles_total, cycles_total / 16e6, pc, ob12_ps());
        std::vector<std::pair<long, u32>> top;
        for (auto& entry : hot) top.push_back({entry.second, entry.first});
        std::sort(top.rbegin(), top.rend());
        printf("hottest PCs:");
        for (size_t i = 0; i < top.size() && i < 10; i++) printf(" %06X x%ld", top[i].second, top[i].first);
        printf("\ndistinct PCs %zu\nlast PCs:", hot.size());
        for (int i = 0; i < 16; i++) printf(" %06X", trail[(trail_at + i) % 16]);
        puts("\nresident calls:");
        for (auto& line : calls) puts(line.c_str());
        printf("debug text: %s\n", text.substr(0, 1500).c_str());
        for (auto& u : uart) {
            printf("%s: %ld bytes:", u.name, u.sent);
            for (size_t i = 0; i < u.head.size() && i < 48; i++) printf(" %02X", u.head[i]);
            puts("");
        }
        printf("vectors taken:");
        for (auto& entry : vectors) printf(" %02X x%ld", entry.first, entry.second);
        puts("\nI/O registers (addr reads writes last firstPC):");
        for (auto& entry : iostat)
            printf("  %02X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        puts("external (16-byte block reads writes last firstPC):");
        for (auto& entry : ext)
            printf("  %06X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        printf("host bytes with no DASP selected: %ld\n", unselected_bytes);
        for (int chip = 0; chip < 5; chip++) {
            printf("DASP%d: %zu bytes:", chip, dsp_stream[chip].size());
            for (size_t i = 0; i < dsp_stream[chip].size() && i < 24; i++) printf(" %02X", dsp_stream[chip][i]);
            puts("");
            std::ofstream file("build/dsp_stream_" + std::to_string(chip) + ".bin", std::ios::binary);
            file.write((const char*)dsp_stream[chip].data(), dsp_stream[chip].size());
        }
        puts("flash writes:");
        for (auto& entry : flashw)
            printf("  %06X w%ld last %02X pc %06X\n", entry.first, entry.second.writes, entry.second.last, entry.second.first_pc);
    }
    void save_hot(const std::string& path) {
        std::map<u32, long> sorted(hot.begin(), hot.end());
        std::ofstream file(path);
        for (auto& entry : sorted) file << std::hex << entry.first << ' ' << std::dec << entry.second << '\n';
    }

protected:
    void debugger_instruction_hook(u32 address) override {
        if (address < 0xe00000) fatalerror("PC left memory: %06X", address);
        if (lean && address != 0xffd3a0 && address != 0xffd215 && address != 0xffd12e && address != 0xffd1bd) { pc = address; steps++; return; }
        if (address == 0xffd3a0) debug_print();
        else if (address == 0xffd215 || address == 0xffd12e || address == 0xffd1bd) log_call(address);
        pc = address;
        if (trace_file) fprintf(trace_file, "P %06X\n", address);
        steps++;
        hot[address]++;
        trail[trail_at] = address;
        trail_at = (trail_at + 1) % 16;
    }
    int standard_irq_callback(int level, u32) override {
        int vector = best_vector();
        int icr = (vector - 11) / 2;
        if (!vector || (io[0xb0 + icr] & 7) != (level >> 13)) { update_irq(); return 0; }
        vectors[vector]++;
        return vector;
    }

private:
    void set_vector(int vector, u32 target) {
        u32 at = 0xfffffc - vector * 4 - 0xfc0000;
        flash[at] = target & 0xff; flash[at + 1] = (target >> 8) & 0xff; flash[at + 2] = (target >> 16) & 0xff; flash[at + 3] = 0;
    }
    void poke_vector_reset(u32 target) {
        u32 at = 0xffffdc - 0xfc0000;
        flash[at] = target & 0xff; flash[at + 1] = (target >> 8) & 0xff; flash[at + 2] = (target >> 16) & 0xff; flash[at + 3] = 0x88;
    }
    // After CALLP the stack holds PC, PCB, then the arguments.
    u16 argument(int index) { return read_word(ob12_sp24() + 4 + index * 2); }
    void log_call(u32 address) {
        char line[96];
        snprintf(line, sizeof line, "  %06X from %06X args %04X %04X", address, pc, argument(0), argument(1));
        calls.push_back(line);
    }
    void debug_print() {
        u32 string = argument(0) | argument(1) << 16;
        for (int i = 0; i < 120; i++) {
            u8 c = read_byte(string + i);
            if (!c) break;
            text.push_back(c >= 32 && c < 127 ? (char)c : ' ');
        }
        text += " | ";
    }

    bool source_pending(int vector) const {
        for (auto& t : timer) if (t.vector == vector) return t.pending();
        for (auto& u : uart) {
            if (u.tx_vector == vector) return u.tx_pending();
            if (u.rx_vector == vector) return u.rx_pending();
        }
        if (vector == 0x1b) return (io[0x30] & 0x18) == 0x18;
        if (vector == 0x22) return (io[0xa9] & 0x18) == 0x18;
        if (vector == 0x2a) return io[0x9f] & 1;  // delayed interrupt module, DIRR
        if (vector >= 0x0b && vector <= 0x19 && (vector & 1)) {
            u8 bit = 1 << ((vector - 0x0b) / 2);
            return io[0x28] & io[0x29] & bit;
        }
        return false;
    }
    int best_vector() const {  // the same answer as going through source_pending() for 0B to 2A
        PendingScan p;
        for (auto& t : timer) p.add(t.vector, t.pending());
        for (auto& u : uart) { p.add(u.tx_vector, u.tx_pending()); p.add(u.rx_vector, u.rx_pending()); }
        p.add(0x1b, (io[0x30] & 0x18) == 0x18);
        p.add(0x22, (io[0xa9] & 0x18) == 0x18);
        p.add(0x2a, io[0x9f] & 1);
        for (int vector = 0x0b; vector <= 0x19; vector += 2) p.add(vector, io[0x28] & io[0x29] & (1 << ((vector - 0x0b) / 2)));
        return p.best(io);
    }
    void update_irq() {
        int vector = best_vector();
        set_irq_level(vector ? io[0xb0 + (vector - 11) / 2] & 7 : 7);
    }

    // Host bytes go through the PLD to whichever DASP is selected by NDCS, port 7 bits 0-4.
    void dsp_byte(u8 data) {
        u8 selected = ~io[0x07] & 0x1f;
        for (int chip = 0; chip < 5; chip++)
            if (selected & (1 << chip)) dsp_stream[chip].push_back(data);
        if (!selected) unselected_bytes++;
        if (byte_log) fprintf(byte_log, "%06X %02X %02X %lld\n", pc, selected, data, (long long)cycles_total);  // study: OB12_DSP_BYTE_LOG
        if (on_dsp_byte) on_dsp_byte(selected, data);
    }

    u8 io_read(u32 address) {
        if (address >= 0x01 && address <= 0x0a) {
            u8 ddr = io[address + 0x10];
            u8 pins = 0xff;
            if (address == 0x06) pins = 0xfe;  // P60 EMPTY low: DSP accepts host data. P61 RDY high.
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
        if (address == 0x29 && rmw()) return 0xff;
        return io[address];
    }
    void io_write(u32 address, u8 data) {
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
        case 0x29: io[address] &= data; break;
        case 0xa1: io[address] = (data & 0xbf) | (data & 0x04 ? 0x40 : 0); break;
        case 0xa9: io[address] = (data & ~0x08) | (data & io[address] & 0x08); break;
        case 0x07:
            if (((io[address] ^ data) & 0x1f) && on_dsp_select) on_dsp_select();
            io[address] = data;
            break;
        default: io[address] = data; break;
        }
    }

public:
    Stat& touch(std::map<u32, Stat>& table, u32 key) {
        auto found = table.find(key);
        if (found != table.end()) return found->second;
        Stat& s = table[key];
        s.first_pc = pc;
        return s;
    }

    std::vector<u8> flash, ram;
    u8 internal[0x500];
    u8 io[0x100];
    ReloadTimer timer[2];
    Uart uart[3];
    s64 timebase = 0;
    u32 pc = 0;
    FILE* byte_log = nullptr;  // study: Slave PC of each DSP host byte
    u32 trail[16] = {};
    int trail_at = 0;
    std::unordered_map<u32, long> hot;
    std::map<u32, Stat> iostat, ext, flashw;
    std::map<int, long> vectors;
    std::vector<u8> dsp_stream[5];
    long unselected_bytes = 0;
    std::vector<std::string> calls;
    std::string text;
};

#ifndef OB12_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: ob12_slave <rom dir> <instructions> [master stream]"); return 2; }
    auto* cpu = new Slave();
    cpu->load(argv[1]);
    long limit = atol(argv[2]);
    cpu->attach(cpu);
    cpu->reset();
    if (argc > 3) cpu->feed(read_file(argv[3]));
    try {
        while (cpu->steps < limit) {
            const int slice = 200;
            cpu->run(slice);
            cpu->tick(cpu->executed());
        }
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    cpu->report();
    cpu->save_hot("build/hot_slave.txt");
    return 0;
}
#endif
