// TMS57070 DSP core for the OB-12 emulator.
//
// Written for this project with a flat integer design. Instruction behaviour was learned
// from the public Prehistoricman/TMS57070 reverse engineering (wiki, IDA module, emulator)
// and is checked against that emulator by tools/tms_harness/core_compare.cpp. See
// docs/HARDWARE.md for the licence position.
//
// Options switch between the public emulator's behaviour, used for that comparison, and
// behaviour inferred from the OB-12 firmware. Inferred behaviour is not checked on silicon.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace tms {

struct Options {
    bool audio_port_writes = false;  // secondary 0C/0D/0E with flag 4 write AX1/2/3
    bool true_dmem_ring = false;     // DMEM ring is 00..DCIRC, cells above stay fixed
    bool lock_delay = false;         // a DMEM lock from secondary 2E misses the next 27
    bool maclo_multiply = false;     // primary 43 multiplies by the low MAC word
    bool divide_1fc = false;         // primary 1FC divides by the last ALU operand
    bool noise_generator = false;    // secondary 34 stores a noise word; real sequence unknown
    bool late_mode_change = false;   // secondaries 28-2B act after the primary of the same word
    // study: 1 = the output shift travels with the product (shift at the end of the word that made it),
    // 2 = the shift in effect at the start of that word; 0 = applied when the MAC is read (old model)
    int shift_with_product = 0;
    int shift_paths = 3;  // with shift_with_product: 1 = stores and port writes, 2 = ALU and accumulator loads
    unsigned late_mode_mask = 0xf;   // which of 28, 29, 2A, 2B (bits 0-3) act late, with late_mode_change
    bool queued_pointer_load = false;  // secondaries 04-06, 08, 09 land two steps later, like C1-C5
    bool maclo_unsigned = false;     // primary 43 takes the low MAC word as unsigned
    bool maclo_shift = false;        // primary 43 product sits 24 bits lower, for double precision
    bool compare_3e = false;         // primary 3E flag 8 compares DMEM with CMEM, flags only
    uint32_t xmem_words = 0;  // the board's external RAM in words, a power of two; 0 = from CR3
    bool xread_flush = false;  // a new XMEM read lands the pending one in XRD first (read pipeline)
    // Words from an XMEM read to its data in XRD; 0 = the public emulator's table by CR3. The OB-12
    // effects program takes XRD 5 words after a read and expects the old value 2 words after one.
    int xread_cycles = 0;
    bool late_saturation_mode = false;  // secondary 2D acts after the primary, like 28-2B
    bool reuse_coefficient = false;  // primary 43 multiplies DMEM by the last multiplier C word
    bool bus_sum_saturates = false;  // the OB-12 voice output adds clip instead of wrapping
};

// The registers: everything a step reads and writes except the memories. Core is a Regs, so
// core.acc, core.pc and the rest read as before. A run copies them into a Machine on the stack,
// where the compiler keeps them in machine registers, and copies them back at the end.
struct Regs {
    uint32_t cr[4] = {};     // control registers, 24 bits each
    int32_t acc[2] = {};
    int64_t mac[2] = {}, mac_d1[2] = {}, mac_d2[2] = {};  // 52-bit, sign extended
    int shift_d1[2] = {}, shift_d2[2] = {};  // output shift that goes with mac_d1, mac_d2 (shift_with_product)
    int32_t ar[2][2] = {};   // [port][0 left, 1 right]
    int32_t ax[3][2] = {};
    int32_t xrd = 0, t_reg = 0;
    uint32_t hir = 0;
    uint16_t pc = 0, rep_start = 0, rep_end = 0;
    uint32_t rptc = 0;
    uint16_t stack[4] = {};
    uint8_t sp = 0;
    // The eight address registers, also as one array for the queued loads: da 0-1, dir 2-3, ca 4-5, cir 6-7.
    union {
        struct { uint16_t da[2], dir[2], ca[2], cir[2]; };
        uint16_t preg[8];
    };
    uint16_t coff = 0, ccirc = 0, doff = 0, dcirc = 0, goff = 0;
    uint32_t xoff = 0;
    bool bio = false;

    int32_t last_coefficient = 0;
    uint32_t alu_last = 0;
    uint32_t noise_state = 0x1234567;
    bool lock_pending = false;
    int insn_cycles = 1;
    uint32_t xmem_read_address = 0;
    int xmem_read_cycles = 0;
    // Address register writes land two steps later: the register's index in preg, -1 for none.
    int single_reg[2] = {-1, -1};
    uint16_t single_value[2] = {};
    int dual_reg[2] = {-1, -1};
    uint32_t dual_value[2] = {};

    Regs() : preg{} {}
};

struct Machine;

class Core : public Regs {
public:
    explicit Core(const Options& options = Options());

    void reset();
    void step() { run_steps(1); }
    // steps words. On a program select_fast() knows, whole half frames (256 words from PC 0 or
    // 100) run from code translated for that program, bit for bit the same as step().
    void run(int steps);
    void select_fast();  // call after PMEM is loaded
    bool fast_selected() const { return fast_program >= 0; }
    long fast_halves = 0, slow_steps = 0;  // how run() went, for measurement
    void load_control(const uint8_t* header);  // 12 bytes, CR0-CR3, big-endian

    uint32_t pmem[512] = {};
    int32_t cmem[512] = {};
    int32_t dmem[512] = {};
    std::vector<int32_t> xmem;

    int32_t mac_upper(int64_t value) const;
    uint32_t mac_lower(int64_t value) const;
    // study: per program address, how often a read sees a shift other than the one of its product's word
    mutable long shift_mismatch[512] = {};
    std::vector<int> shift_pcs;  // study: product-time shift only for reads at these addresses (empty: all)
    bool study_shift = false;  // the shift studies are on: set by the harness (OB12_SHIFT_*)
    // study: the shift a read of mac_d2 takes; counts the mismatches when count is set
    int study_pick_shift(int product_shift, int now_shift, int at_pc, int path, bool count) const;

    // Encodings the core does not implement, for measurement.
    std::map<uint32_t, long> unknown;
    void note_unknown(uint32_t key);
    long wraps[512] = {};  // ACC overflows without saturation, by word

    // Study access: the CMEM and DMEM index a word would use now.
    uint32_t study_cmem(uint32_t word) const;
    uint32_t study_dmem(uint32_t word) const;
    uint32_t study_xmem(uint32_t address) const;
    uint32_t study_xread() const { return xmem_read_address; }

private:
    friend struct Machine;
    void run_steps(int steps);
    Options options;
    int fast_program = -1;
    uint32_t fast_mask[2] = {}, fast_value[2] = {};  // CR1 mode bits each translated half expects at its start
    uint32_t fast_cr3 = 0;    // CR3 as the program's header loads it
    uint16_t fast_dcirc = 0;  // the DMEM ring size the start-up program leaves
    bool fast_ready(int half) const;
    typedef void (Core::*FastHalf)();
    static const FastHalf fast_table[][2];
    void fast_0a(); void fast_0b(); void fast_1a(); void fast_1b(); void fast_2a(); void fast_2b();
};

}  // namespace tms
