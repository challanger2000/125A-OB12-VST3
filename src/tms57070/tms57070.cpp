#include "tms57070.h"

namespace tms {

namespace {
enum { CR0, CR1, CR2, CR3 };
// CR1 bits
enum { AOV = 0, AOVL = 1, ACCZ = 2, ACCN = 3, MOV = 4, MOVL = 5, MOVR = 6, AOVM = 8, MOVM = 16, EXTMEM = 19, EXT = 20, LDMEM = 21, LCMEM = 23 };
// CR2 bits
enum { FREE = 17 };
// CR3 bits
enum { LXMEM = 15, XWORD = 16 };

enum { OpLoad, OpLoadUnsigned, OpOnes, OpTwos, OpInc, OpDec };
enum { OpAdd, OpSub, OpAnd, OpOr, OpXor, OpCmp };

inline int32_t s24(uint32_t value) { return (int32_t)(value << 8) >> 8; }
inline int64_t s52(int64_t value) { return (int64_t)((uint64_t)value << 12) >> 12; }
inline uint32_t u24(int32_t value) { return (uint32_t)value & 0xffffff; }
const uint64_t LOW24 = 0xffffff;
}  // namespace

#if defined(__clang__) || defined(__GNUC__)
#define TMS_INLINE __attribute__((always_inline)) inline
#else
#define TMS_INLINE inline
#endif

enum { PDA = 0, PDIR = 2, PCA = 4, PCIR = 6 };  // indices into Regs::preg

// The registers on the stack with the memories beside them: what a run works on.
struct Machine : Regs {
    uint32_t* pmem;
    int32_t* cmem;
    int32_t* dmem;
    int32_t* xmem;
    long* wraps;
    Core* core;
    Options options;
    bool study_shift;
    // CR1's status flags (bits 0-6: AOV, AOVL, ACCZ, ACCN, MOV, MOVL, MOVR) live here during a run
    // and cr[1] keeps only the mode bits. Nearly every word changes a flag; kept in one word with
    // them, the mode bits stopped being constants to the compiler a few words into a translation.
    uint32_t status;
    enum { STATUS = 0x7f };

    TMS_INLINE explicit Machine(Core& c) : Regs(c), pmem(c.pmem), cmem(c.cmem), dmem(c.dmem), xmem(c.xmem.data()), wraps(c.wraps), core(&c), options(c.options), study_shift(c.study_shift) {
        status = cr[1] & STATUS;
        cr[1] &= ~(uint32_t)STATUS;
    }
    TMS_INLINE void store() {
        cr[1] |= status;
        static_cast<Regs&>(*core) = *this;
        cr[1] &= ~(uint32_t)STATUS;
    }

    TMS_INLINE bool flag(int reg, int bit) const { return ((reg == 1 && bit < 7 ? status : cr[reg]) >> bit) & 1; }
    TMS_INLINE void set_flag(int reg, int bit, bool on) {
        if (reg == 1 && bit < 7) status = (status & ~(1u << bit)) | ((uint32_t)on << bit);
        else cr[reg] = (cr[reg] & ~(1u << bit)) | ((uint32_t)on << bit);
    }
    TMS_INLINE int field(int reg, int shift, int bits) const { return (cr[reg] >> shift) & ((1 << bits) - 1); }  // mode fields only
    TMS_INLINE uint32_t get_cr(int reg) const { return reg == 1 ? cr[1] | status : cr[reg]; }
    TMS_INLINE void set_cr(int reg, uint32_t value) {
        if (reg == 1) { status = value & STATUS; cr[1] = value & ~(uint32_t)STATUS; }
        else cr[reg] = value;
    }

    int output_shift() const;
    int round_bits() const;
    TMS_INLINE int32_t mac_upper(int64_t value) const { return mac_upper_s(value, output_shift()); }
    int32_t mac_upper_s(int64_t value, int shift) const;
    TMS_INLINE uint32_t mac_lower(int64_t value) const { return mac_lower_s(value, output_shift()); }
    uint32_t mac_lower_s(int64_t value, int shift) const;
    // The study switches cost nothing when off: the plain read is the model (see "The voice level
    // hangs on when the output shift takes effect" in docs/HARDWARE.md).
    TMS_INLINE int32_t upper_d2(int unit, int path = 1) const {
        if (!study_shift) return mac_upper(mac_d2[unit]);
        return mac_upper_s(mac_d2[unit], core->study_pick_shift(shift_d2[unit], output_shift(), pc, path, true));
    }
    TMS_INLINE uint32_t lower_d2(int unit) const {
        if (!study_shift) return mac_lower(mac_d2[unit]);
        return mac_lower_s(mac_d2[unit], core->study_pick_shift(shift_d2[unit], output_shift(), pc, 1, false));
    }

    uint32_t cmem_index(uint32_t address) const;
    uint32_t dmem_index(uint32_t address) const;
    uint32_t cmem_operand(uint32_t word) const;
    uint32_t dmem_operand(uint32_t word) const;
    uint32_t xmem_index(uint32_t address) const;

    void step_word(uint32_t word, bool fast);
    void primary(uint32_t word);
    void secondary(uint32_t word);
    void post_increment(uint32_t word);
    void jump(uint32_t word);
    void pipeline();
    // A register by a number known only at run time, without indexing the struct: an index the
    // compiler cannot see through would keep every register of a translated half in memory.
    TMS_INLINE void put_preg(int index, uint16_t value) {
        switch (index) {
        case 0: preg[0] = value; break; case 1: preg[1] = value; break; case 2: preg[2] = value; break; case 3: preg[3] = value; break;
        case 4: preg[4] = value; break; case 5: preg[5] = value; break; case 6: preg[6] = value; break; case 7: preg[7] = value; break;
        }
    }
    void queue_single(int target, uint16_t value);
    void queue_dual(int target, uint32_t value);
    void set_pointer(int target, uint16_t value);

    int32_t settle_acc(int64_t value);
    void load_acc(uint32_t word, int operation);
    void alu(uint32_t word, int operation);
    void multiply(int unit, int64_t lhs, int64_t rhs, bool negate);
    void accumulate(int unit, int64_t lhs, int64_t rhs, bool negate);
    void mac_shift(int unit, int amount);
    void mac_set_upper(int unit, int32_t value);
    void mac_set_lower(int unit, uint32_t value);
};

TMS_INLINE int Machine::output_shift() const {
    static const int table[4] = {0, 2, 4, -8};
    return table[field(CR1, 12, 2)];
}

TMS_INLINE int Machine::round_bits() const {
    static const int table[8] = {0, 24, 28, 30, 32, 32, 28, 30};
    return table[field(CR1, 9, 3)];
}

TMS_INLINE int32_t Machine::mac_upper_s(int64_t value, int shift) const {
    int64_t v = shift >= 0 ? value << shift : value >> -shift;
    int bits = round_bits();
    int64_t mask = 0;
    if (bits) {
        int64_t carry = v & ((int64_t)1 << (bits - 1));
        mask = ((int64_t)1 << bits) - 1;
        v += carry << 1;
    }
    int64_t upper = v >> 24;
    if (flag(CR1, MOVM)) {
        if (upper > 0x7fffff) upper = 0x7fffff;
        else if (upper < -0x800000) upper = -0x800000;
    }
    if (bits) upper &= ~(mask >> 24);
    return s24((uint32_t)upper);
}

TMS_INLINE uint32_t Machine::mac_lower_s(int64_t value, int shift) const {
    int64_t v = shift >= 0 ? value << shift : value >> -shift;
    uint32_t lower = (uint32_t)(v & LOW24);
    if (flag(CR1, MOVM)) {
        int64_t upper = v >> 24;
        if (upper > 0x7fffff) lower = 0xffffff;
        else if (upper < -0x800000) lower = 0;
    }
    return lower;
}

TMS_INLINE uint32_t Machine::cmem_index(uint32_t address) const {
    uint32_t a = address & 0xffff;
    if (!flag(CR1, LCMEM)) a = (a + coff) & 0xffff;
    return a & ((flag(CR1, EXT) && flag(CR1, EXTMEM)) ? 0x1ff : 0xff);
}

TMS_INLINE uint32_t Machine::dmem_index(uint32_t address) const {
    uint32_t a = address & 0xffff;
    uint32_t size_mask = (flag(CR1, EXT) && !flag(CR1, EXTMEM)) ? 0x1ff : 0xff;
    if (!flag(CR1, LDMEM)) {
        if (options.true_dmem_ring) {
            a &= size_mask;
            if (a <= dcirc) { a += doff; if (a > dcirc) { a -= dcirc + 1u; if (a > dcirc) a %= dcirc + 1u; } }
            return a;
        }
        a = (a + doff) & 0xffff;
    }
    return a & size_mask;
}

TMS_INLINE uint32_t Machine::cmem_operand(uint32_t word) const {
    uint32_t address = 0;
    switch ((word >> 12) & 3) {
    case 1: address = ca[(word & 0x800) ? 1 : 0]; break;
    case 2: address = word; break;
    case 3: address = ca[(word & 0x100) ? 1 : 0]; break;
    }
    return cmem_index(address);
}

TMS_INLINE uint32_t Machine::dmem_operand(uint32_t word) const {
    uint32_t address = 0;
    switch ((word >> 12) & 3) {
    case 1: address = word; break;
    case 2: case 3: address = da[(word & 0x800) ? 1 : 0]; break;
    }
    return dmem_index(address);
}

TMS_INLINE uint32_t Machine::xmem_index(uint32_t address) const {
    static const uint32_t sizes[4] = {0x4000, 0x8000, 0x10000, 0x10000};
    uint32_t size = sizes[field(CR3, 17, 2)];
    if (flag(CR3, XWORD)) size >>= 1;
    if (options.xmem_words) size = options.xmem_words;
    return (address + xoff) & (size - 1);
}

TMS_INLINE int32_t Machine::settle_acc(int64_t value) {
    if (flag(CR1, AOVM)) {
        if (value < -0x800000) { value = -0x800000; set_flag(CR1, AOV, true); }
        else if (value > 0x7fffff) { value = 0x7fffff; set_flag(CR1, AOV, true); }
    } else if (value < -0x800000 || value > 0x7fffff) {
        set_flag(CR1, AOV, true);
        wraps[(pc - 1) & 0x1ff]++;
    }
    int32_t result = s24((uint32_t)value);
    set_flag(CR1, ACCZ, result == 0);
    set_flag(CR1, ACCN, result < 0);
    return result;
}

TMS_INLINE void Machine::load_acc(uint32_t word, int operation) {
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    int32_t value = 0;
    switch ((word >> 24) & 3) {
    case 0: value = dmem[dmem_operand(word)]; break;
    case 1: value = cmem[cmem_operand(word)]; break;
    case 2: value = acc[flag8]; break;
    case 3: value = upper_d2(flag8, 2); break;
    }
    switch (operation) {
    case OpLoadUnsigned: if (value < 0) value = -value; break;
    case OpTwos: value = -value; break;
    case OpOnes: value = ~value; break;
    case OpInc: value++; break;
    case OpDec: value--; break;
    }
    acc[flag4] = settle_acc(value);
}

TMS_INLINE void Machine::alu(uint32_t word, int operation) {
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    uint32_t top = word >> 24;
    int32_t lhs, rhs;
    if (top >= 0x3c && top <= 0x3e) {
        lhs = dmem[dmem_operand(word)];
        rhs = cmem[cmem_operand(word)];
    } else {
        int source = top & 3;
        lhs = source < 2 ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        rhs = (source & 1) ? upper_d2(flag8, 2) : acc[flag8];
    }
    if (operation == OpAdd || operation == OpSub || operation == OpCmp) alu_last = u24(lhs);
    int64_t result = 0;
    switch (operation) {
    case OpAdd: result = (int64_t)lhs + rhs; break;
    case OpSub: case OpCmp: result = (int64_t)lhs - rhs; break;
    case OpAnd: result = lhs & rhs; break;
    case OpOr: result = lhs | rhs; break;
    case OpXor: result = lhs ^ rhs; break;
    }
    if (operation == OpCmp) {
        set_flag(CR1, AOV, result < -0x800000 || result > 0x7fffff);
        set_flag(CR1, ACCZ, result == 0);
        set_flag(CR1, ACCN, result < 0);
        return;
    }
    acc[flag4] = settle_acc(result);
}

// Product of two 24-bit fractions, aligned as a MAC value: a * b * 2.
TMS_INLINE void Machine::multiply(int unit, int64_t lhs, int64_t rhs, bool negate) {
    int64_t product = lhs * rhs * 2;
    mac[unit] = s52(negate ? -product : product);
}

TMS_INLINE void Machine::accumulate(int unit, int64_t lhs, int64_t rhs, bool negate) {
    int64_t current = mac[unit];
    switch (field(CR1, 14, 2)) {
    case 1: current <<= 2; break;
    case 2: current <<= 4; break;
    case 3: current >>= 24; break;
    }
    multiply(unit, lhs, rhs, negate);
    mac[unit] = s52(mac[unit] + current);
}

TMS_INLINE void Machine::mac_shift(int unit, int amount) {
    mac[unit] = s52(amount < 0 ? mac[unit] >> -amount : mac[unit] << amount);
}

TMS_INLINE void Machine::mac_set_upper(int unit, int32_t value) {
    uint64_t raw = (uint64_t)mac[unit] & LOW24;
    raw |= (uint64_t)((int64_t)value << 24);
    mac[unit] = s52((int64_t)raw);
}

TMS_INLINE void Machine::mac_set_lower(int unit, uint32_t value) {
    uint64_t raw = (uint64_t)mac[unit] & ~LOW24;
    raw |= value & LOW24;
    mac[unit] = s52((int64_t)raw);
}

TMS_INLINE void Machine::queue_single(int target, uint16_t value) { single_reg[0] = target; single_value[0] = value & 0xfff; }
TMS_INLINE void Machine::queue_dual(int target, uint32_t value) { dual_reg[0] = target; dual_value[0] = value & 0xffffff; }
TMS_INLINE void Machine::set_pointer(int target, uint16_t value) {
    if (options.queued_pointer_load) queue_single(target, value);
    else preg[target] = value;
}

TMS_INLINE void Machine::pipeline() {
    if (dual_reg[1] >= 0) { put_preg(dual_reg[1], dual_value[1] & 0xfff); put_preg(dual_reg[1] + 1, (dual_value[1] >> 12) & 0xfff); }
    if (single_reg[1] >= 0) put_preg(single_reg[1], single_value[1]);
    dual_reg[1] = dual_reg[0]; dual_value[1] = dual_value[0];
    single_reg[1] = single_reg[0]; single_value[1] = single_value[0];
    dual_reg[0] = -1;
    single_reg[0] = -1;
}

static inline int64_t as_unsigned(int32_t value) { return (int64_t)(uint32_t)u24(value); }

TMS_INLINE void Machine::primary(uint32_t word) {
    uint32_t op = word >> 24;
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    int unit = flag4;  // MAC unit for multiply groups
    bool negate = flag8;

    auto pick = [](int32_t value, bool is_unsigned) { return is_unsigned ? as_unsigned(value) : (int64_t)value; };

    if (op < 0x04) return;
    if (op < 0x08) { load_acc(word, OpLoadUnsigned); return; }
    if (op < 0x0c) { load_acc(word, OpTwos); return; }
    if (op < 0x10) { load_acc(word, OpOnes); return; }
    if (op < 0x14) { load_acc(word, OpLoad); return; }
    if (op < 0x18) { load_acc(word, OpInc); return; }
    if (op < 0x1c) { load_acc(word, OpDec); return; }
    if (op >= 0x20 && op < 0x24) { alu(word, OpAdd); return; }
    if (op >= 0x24 && op < 0x28) { alu(word, OpSub); return; }
    if (op >= 0x28 && op < 0x2c) { alu(word, OpAnd); return; }
    if (op >= 0x2c && op < 0x30) { alu(word, OpOr); return; }
    if (op >= 0x30 && op < 0x34) { alu(word, OpXor); return; }
    if (op >= 0x34 && op < 0x38) { alu(word, OpCmp); return; }

    switch (op) {
    case 0x1c: {
        int64_t value = acc[flag4];
        value = flag8 ? value << 1 : value >> 1;
        acc[flag4] = settle_acc(value);
        break;
    }
    case 0x1d:
        if (flag8) { core->note_unknown(word & 0xffc00000); break; }
        acc[flag4] = 0;
        settle_acc(0);
        break;
    case 0x1e:
        if (flag4) { core->note_unknown(word & 0xffc00000); break; }
        acc[1] = mac_upper(mac[flag8]);
        acc[0] = s24(mac_lower(mac[flag8]));
        break;
    case 0x1f:
        if (flag8) {
            uint32_t divisor;
            if (flag4) {
                if (!options.divide_1fc) { core->note_unknown(word & 0xffc00000); break; }
                divisor = alu_last;
            } else {
                divisor = u24(dmem[dmem_operand(word)]);
                alu_last = divisor;
            }
            uint32_t low = u24(acc[0]), high = u24(acc[1]);
            uint32_t difference = (high - divisor) & 0xffffff;
            bool subtract = !(difference & 0x800000);
            if (options.divide_1fc && divisor == 0) subtract = false;  // hypothesis, see docs
            if (subtract) high = difference;
            acc[1] = s24((high << 1) | (low >> 23));
            acc[0] = s24((low << 1) | (subtract ? 1 : 0));
        } else if (!flag4) {
            acc[0] = acc[1] = 0;
            settle_acc(0);
        }
        break;
    case 0x38: {
        int unit2 = flag8;
        int32_t upper = mac_upper(mac[unit2]);
        if (!(upper >= 0x400000 || upper < -0x400000)) {
            mac_shift(unit2, 1);
            acc[flag4] = s24((uint32_t)(acc[flag4] - 1));
        }
        break;
    }
    case 0x39:
        if (flag4) {
            xmem[xmem_index((uint32_t)cmem[cmem_operand(word)])] = dmem[dmem_operand(word)];
        } else {
            static const int with_word[4] = {21, 12, 9, 0};
            static const int without[4] = {15, 9, 0, 6};
            if (options.xread_flush && xmem_read_cycles != 0) {
                xrd = xmem[xmem_read_address];
                if (!flag(CR3, XWORD)) xrd = s24(u24(xrd) & 0xffff00);
            }
            xmem_read_address = xmem_index((uint32_t)cmem[cmem_operand(word)]);
            int bus = field(CR3, 17, 2);
            xmem_read_cycles = options.xread_cycles ? options.xread_cycles : flag(CR3, XWORD) ? with_word[bus] : without[bus];
        }
        break;
    case 0x3a: case 0x3b: case 0x3f: break;
    case 0x3c: alu(word, flag8 ? OpSub : OpAdd); break;
    case 0x3d: alu(word, flag8 ? OpOr : OpAnd); break;
    case 0x3e:
        if (flag8 && !options.compare_3e) core->note_unknown(word & 0xffc00000);
        else alu(word, flag8 ? OpCmp : OpXor);
        break;

    case 0x40: case 0x41: case 0x44: case 0x45: case 0x48: case 0x49: case 0x4c: case 0x4d: {
        int32_t a = acc[op & 1];
        int32_t c = cmem[cmem_operand(word)];
        last_coefficient = c;
        multiply(unit, pick(c, op & 4), pick(a, op & 8), negate);
        break;
    }
    case 0x42: case 0x46: case 0x4a: case 0x4e: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        multiply(unit, pick(c, op & 4), pick(d, op & 8), negate);
        break;
    }
    case 0x43: case 0x47: case 0x4b: case 0x4f:
        if (options.reuse_coefficient) {
            // Always paired with a secondary that takes the C bus; the multiplier keeps its last C.
            multiply(unit, pick(last_coefficient, op & 4), pick(dmem[dmem_operand(word)], op & 8), negate);
        } else if (options.maclo_multiply) {
            int32_t d = dmem[dmem_operand(word)];
            bool is_unsigned = op & 8;
            int64_t low = options.maclo_unsigned ? (int64_t)mac_lower(mac[unit]) : (int64_t)s24(mac_lower(mac[unit]));
            multiply(unit, low, pick(d, is_unsigned), negate);
            if (options.maclo_shift) mac[unit] >>= 24;
        } else {
            core->note_unknown(word & 0xff000000);
        }
        break;

    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        int32_t a = acc[op & 1];
        bool from_dmem = op & 2;
        int32_t w = from_dmem ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        bool acc_unsigned = false, word_unsigned = false;
        switch (op & 0xc) {
        case 0x4: if (from_dmem) acc_unsigned = true; else word_unsigned = true; break;
        case 0x8: if (from_dmem) word_unsigned = true; else acc_unsigned = true; break;
        case 0xc: acc_unsigned = word_unsigned = true; break;
        }
        accumulate(unit, pick(a, acc_unsigned), pick(w, word_unsigned), negate);
        break;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: {
        int32_t a = acc[op & 1];
        bool from_dmem = op & 2;
        int32_t w = from_dmem ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        bool acc_unsigned = (op & 4) && from_dmem, word_unsigned = (op & 4) && !from_dmem;
        if (field(CR1, 14, 2) == 0) mac_shift(unit, -24);
        accumulate(unit, pick(a, acc_unsigned), pick(w, word_unsigned), negate);
        break;
    }
    case 0x68: case 0x69: case 0x6a: case 0x6b: break;
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        accumulate(unit, pick(c, op & 1), pick(d, op & 2), negate);
        break;
    }
    case 0x70: case 0x71: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        if (field(CR1, 14, 2) == 0) mac_shift(unit, -24);
        accumulate(unit, pick(c, op & 1), d, negate);
        break;
    }
    case 0x72: mac_shift(unit, flag8 ? 1 : -1); break;
    case 0x73:
        if (flag8) core->note_unknown(word & 0xffc00000);
        else mac[unit] = 0;
        break;
    case 0x74:
        if (flag8 || flag4) { core->note_unknown(word & 0xffc00000); break; }
        mac[0] = mac[1] = 0;
        mac_d1[0] = mac[0];
        mac_d1[1] = mac[1];
        shift_d1[0] = shift_d1[1] = output_shift();
        break;
    case 0x75: case 0x76: case 0x77: case 0x7e: case 0x7f: break;
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: {
        if (op < 0x7a) mac_set_lower(unit, 0);
        int32_t value;
        if ((op & 1) == 0) value = flag8 ? acc[0] : dmem[dmem_operand(word)];
        else value = flag8 ? acc[1] : cmem[cmem_operand(word)];
        if (op < 0x7c) mac_set_upper(unit, value);
        else mac_set_lower(unit, (uint32_t)value);
        mac_d1[unit] = mac[unit];
        shift_d1[unit] = output_shift();
        break;
    }

    case 0xc1: {
        uint16_t value = word & 0xfff;
        switch ((word >> 16) & 0xff) {
        case 0x00: queue_single(PDA, value); break;
        case 0x08: queue_single(PDA + 1, value); break;
        case 0x10: queue_single(PDIR, value); break;
        case 0x18: queue_single(PDIR + 1, value); break;
        case 0x20: queue_single(PCA, value); break;
        case 0x28: queue_single(PCA + 1, value); break;
        case 0x30: queue_single(PCIR, value); break;
        case 0x38: queue_single(PCIR + 1, value); break;
        case 0x40: if (!flag(CR1, ACCN)) queue_single(PCA, value); break;
        case 0x48: if (!flag(CR1, ACCN)) queue_single(PCA + 1, value); break;
        default: core->note_unknown(word & 0xffff0000); break;
        }
        break;
    }
    case 0xc2: queue_dual(PDA, word); break;
    case 0xc3: queue_dual(PDIR, word); break;
    case 0xc4: queue_dual(PCA, word); break;
    case 0xc5: queue_dual(PCIR, word); break;
    case 0xc6: if (!flag(CR1, ACCN)) queue_dual(PCA, word); break;
    case 0xc7: doff = word & 0xfff; dcirc = (word >> 12) & 0xfff; break;
    case 0xc8: coff = word & 0xfff; ccirc = (word >> 12) & 0xfff; break;
    case 0xca: acc[0] = s24(word); settle_acc(acc[0]); break;
    case 0xcb: acc[1] = s24(word); settle_acc(acc[1]); break;
    case 0xcc: cr[CR0] = word & 0xffffff; break;
    case 0xcd: set_cr(CR1, word & 0xffffff); break;
    case 0xce: {
        uint32_t value = word & 0xffffff;
        uint32_t common = value & cr[CR2] & 0xff;
        cr[CR2] = (value & 0xffff00) | ((cr[CR2] & 0xff) ^ common);
        break;
    }
    case 0xcf: cr[CR3] = word & 0xffffff; break;
    case 0xe0: rptc = (word >> 16) & 0xff; rep_start = rep_end = pc; insn_cycles = rptc ? 1 : 2; break;
    case 0xe2: rptc = (uint8_t)acc[0]; rep_start = rep_end = pc; insn_cycles = rptc ? 3 : 4; break;
    case 0xe3: rptc = (uint8_t)acc[1]; rep_start = rep_end = pc; insn_cycles = rptc ? 3 : 4; break;
    case 0xe4:
        rptc = (word >> 16) & 0xff;
        rep_start = pc;
        rep_end = word & 0x1ff;
        if (rep_end == pc) rptc = 0;
        break;
    case 0xec: sp--; pc = stack[sp & 3]; insn_cycles = 2; break;
    case 0xee: sp--; pc = stack[sp & 3]; set_flag(CR2, FREE, true); rptc = 0; insn_cycles = 2; break;
    default:
        if (op >= 0xf0) jump(word);
        else core->note_unknown(word & 0xff000000);
        break;
    }
}

TMS_INLINE void Machine::jump(uint32_t word) {
    uint32_t condition = (word >> 20) & 0x7c;
    bool is_call = (word >> 24) >= 0xf8;
    bool taken = false;
    uint16_t target = (uint16_t)word;
    switch (condition) {
    case 0x00: taken = true; break;
    case 0x08: taken = true; target = (uint16_t)acc[0]; break;
    case 0x0c: taken = true; target = (uint16_t)acc[1]; break;
    case 0x10: taken = flag(CR1, ACCZ); break;
    case 0x18: taken = !flag(CR1, ACCZ); break;
    case 0x20: taken = !(flag(CR1, ACCZ) || flag(CR1, ACCN)); break;
    case 0x28: taken = flag(CR1, ACCN); break;
    case 0x30: taken = flag(CR1, AOV); break;
    case 0x38: taken = flag(CR1, AOVL); break;
    case 0x40: taken = flag(CR1, MOV); break;
    case 0x48: taken = flag(CR1, MOVL); break;
    case 0x50: taken = flag(CR1, MOVR); break;
    case 0x58: taken = bio; break;
    default: core->note_unknown(word & 0xfff00000); break;
    }
    insn_cycles = condition == 0 ? 2 : (condition == 8 || condition == 0xc) ? 4 : 3;
    if (taken) {
        if (is_call) { stack[sp & 3] = pc; sp++; }
        pc = target & 0x1ff;
    }
}

TMS_INLINE void Machine::secondary(uint32_t word) {
    uint32_t op = (word >> 16) & 0x3f;
    bool flag4 = word & 0x4000, flag8 = word & 0x8000;
    int args = (word >> 14) & 3;

    switch (op) {
    case 0x00: break;
    case 0x01:
        if (flag8) cmem[cmem_operand(word)] = acc[flag4];
        else dmem[dmem_operand(word)] = acc[flag4];
        break;
    case 0x02:
        if (flag8) cmem[cmem_operand(word)] = upper_d2(flag4);
        else dmem[dmem_operand(word)] = upper_d2(flag4);
        break;
    case 0x03:
        if (flag8) cmem[cmem_operand(word)] = s24(lower_d2(flag4));
        else dmem[dmem_operand(word)] = s24(lower_d2(flag4));
        break;
    case 0x04: set_pointer(PDA + flag8, acc[flag4] & 0xfff); break;
    case 0x05: set_pointer(PCA + flag8, acc[flag4] & 0xfff); break;
    case 0x06: {
        uint32_t value = (uint32_t)cmem[cmem_operand(word)];
        int pair = args * 2;  // da, dir, ca, cir in preg order
        if (options.queued_pointer_load) queue_dual(pair, value);
        else { preg[pair] = value & 0xfff; preg[pair + 1] = (value >> 12) & 0xfff; }
        break;
    }
    case 0x07: {
        int pair = args * 2;
        cmem[cmem_operand(word)] = s24((uint32_t)preg[pair + 1] << 12 | preg[pair]);
        break;
    }
    case 0x08: set_pointer((flag4 ? PDIR : PDA) + flag8, cmem[cmem_operand(word)] & 0xfff); break;
    case 0x09: set_pointer((flag4 ? PCIR : PCA) + flag8, cmem[cmem_operand(word)] & 0xfff); break;
    case 0x0a: case 0x0b: {
        static const int regs[4][2] = {{PDA, PCA}, {PDIR, PCIR}, {PDA + 1, PCA + 1}, {PDIR + 1, PCIR + 1}};
        cmem[cmem_operand(word)] = preg[regs[args][op == 0x0b]];
        break;
    }
    case 0x0c: case 0x0d: {
        int port = op == 0x0d;
        if (flag4 && options.audio_port_writes) {
            ax[port][flag8] = dmem[dmem_operand(word)];
        } else {
            dmem[dmem_operand(word)] = ar[port][flag8];
        }
        break;
    }
    case 0x0e: case 0x0f:
        if (op == 0x0e && flag4 && options.audio_port_writes) ax[2][flag8] = dmem[dmem_operand(word)];
        else dmem[dmem_operand(word)] = 0;
        break;
    case 0x18: case 0x19: case 0x1a:
        ax[op - 0x18][flag8] = upper_d2(flag4);
        break;
    case 0x20:
        switch (args) {
        case 0: t_reg = dmem[dmem_operand(word)]; break;
        case 2: dmem[dmem_operand(word)] = xrd; break;
        default: core->note_unknown(word & 0x3fc000); break;
        }
        break;
    case 0x21:
        if (args == 0) { doff = (doff - 1) & 0xfff; goff = (goff + 1) & 0xfff; }
        else if (args == 2) goff = 0;
        break;
    case 0x22:
        set_cr(args, u24(cmem[cmem_operand(word)]));
        break;
    case 0x23: cmem[cmem_operand(word)] = s24(get_cr(args)); break;
    case 0x26: hir = u24(flag8 ? cmem[cmem_operand(word)] : dmem[dmem_operand(word)]); break;
    case 0x27:
        if (!flag(CR1, LCMEM)) {
            int32_t end = cmem[cmem_index(ccirc)];
            coff = (coff - 1) & 0xfff;
            cmem[cmem_index(0)] = end;
        }
        if (!flag(CR1, LDMEM) || lock_pending) {
            if (options.true_dmem_ring) {
                doff = (doff + dcirc) % (dcirc + 1u);
            } else {
                int32_t end = dmem[dmem_index(dcirc)];
                doff = (doff - 1) & 0xfff;
                dmem[dmem_index(0)] = end;
            }
            lock_pending = false;
        }
        if (!flag(CR3, LXMEM)) xoff--;
        goff = (goff + 1) & 0xfff;
        ca[1] = 0;
        da[1] = 0;
        break;
    case 0x28: cr[CR1] = (cr[CR1] & ~(3u << 14)) | ((uint32_t)args << 14); break;
    case 0x29: cr[CR1] = (cr[CR1] & ~(3u << 12)) | ((uint32_t)args << 12); break;
    case 0x2a: case 0x2b: {
        uint32_t mode = args + (op == 0x2b ? 4 : 0);
        cr[CR1] = (cr[CR1] & ~(7u << 9)) | (mode << 9);
        break;
    }
    case 0x2c:
        if (flag8) {
            if (flag4) { set_flag(CR1, MOV, false); set_flag(CR1, MOVL, false); }
            else { set_flag(CR1, AOV, false); set_flag(CR1, AOVL, false); }
        } else {
            set_flag(CR2, FREE, flag4);
        }
        break;
    case 0x2d: set_flag(CR1, flag8 ? MOVM : AOVM, flag4); break;
    case 0x2e:
        if (flag8) {
            set_flag(CR1, LCMEM, flag4);
        } else {
            lock_pending = options.lock_delay && flag4 && !flag(CR1, LDMEM);
            set_flag(CR1, LDMEM, flag4);
        }
        break;
    case 0x30: case 0x31: case 0x32: case 0x33:
        dmem[dmem_operand(word)] = xrd;
        break;
    case 0x34:
        if (options.noise_generator) {
            noise_state ^= noise_state << 13;
            noise_state ^= noise_state >> 17;
            noise_state ^= noise_state << 5;
            dmem[dmem_operand(word)] = s24(noise_state);
        } else {
            core->note_unknown(0x80000000u | (op << 16));
        }
        break;
    case 0x38: case 0x39: case 0x3a: case 0x3b:
        break;
    default:
        core->note_unknown(0x80000000u | (op << 16));
        break;
    }
}

TMS_INLINE void Machine::post_increment(uint32_t word) {
    int mode = (word >> 12) & 3;
    int high = (word >> 8) & 0xf, low = (word >> 4) & 0xf;
    if (mode & 2) {
        uint16_t& reg = da[(high & 8) ? 1 : 0];
        if (high & 4) reg = (reg + dir[(high & 2) ? 1 : 0]) & 0xfff;
        else if (high & 2) reg = (reg + 1) & 0xfff;
    }
    if (mode == 1) {
        uint16_t& reg = ca[(high & 8) ? 1 : 0];
        if (high & 4) reg = (reg + cir[(high & 2) ? 1 : 0]) & 0xfff;
        else if (high & 2) reg = (reg + 1) & 0xfff;
    } else if (mode == 3) {
        uint16_t& reg = ca[(high & 1) ? 1 : 0];
        if (low & 8) reg = (reg + cir[(low & 4) ? 1 : 0]) & 0xfff;
        else if (low & 4) reg = (reg + 1) & 0xfff;
    }
}

// One word. In the translated code (fast) the word is a constant and the options are the engine's.
TMS_INLINE void Machine::step_word(uint32_t word, bool fast) {
    // The voice output adds of the OB-12 programs, see "Bus sum saturates" in docs/HARDWARE.md.
    bool bus_sum = options.bus_sum_saturates && (word == 0xa13b2122 || word == 0xa13b2139 || word == 0x2186a136 || word == 0x2186a14d || word == 0x210921e2 || word == 0x21a6b000);
    bool plain = flag(CR1, AOVM);
    if (bus_sum) set_flag(CR1, AOVM, true);
    if (!fast && rptc && pc == rep_end) { pc = rep_start; rptc--; }  // the translated programs have no repeat
    else pc = (pc + 1) & 0x1ff;

    pipeline();
    insn_cycles = 1;
    int shift_start = study_shift ? output_shift() : 0;

    uint32_t top = word >> 24;
    if (top >= 0xc0) {
        primary(word);
    } else if (top >= 0x80) {
        uint32_t paired = (word & 0x3fff) | (((word & 0x3fc000) + 0x400000) << 8);
        primary(paired);
        word &= 0x7fffffff;
        primary(word);
        post_increment(word);
    } else {
        uint32_t op = (word >> 16) & 0x3f;
        if (options.late_mode_change && ((op >= 0x28 && op <= 0x2b && (options.late_mode_mask >> (op - 0x28) & 1)) || (options.late_saturation_mode && op == 0x2d))) {
            primary(word);
            secondary(word);
        } else {
            secondary(word);
            primary(word);
        }
        post_increment(word);
    }

    if (bus_sum) set_flag(CR1, AOVM, plain);

    for (int unit = 0; unit < 2; unit++) {
        mac_d2[unit] = mac_d1[unit];
        mac_d1[unit] = mac[unit];
        // 1: the shift at the end of the product's word; 2: at its start; 3: one word later.
        if (study_shift) {
            shift_d2[unit] = options.shift_with_product == 3 ? output_shift() : shift_d1[unit];
            shift_d1[unit] = options.shift_with_product == 2 ? shift_start : output_shift();
        }
    }

    if (xmem_read_cycles != 0) {
        xmem_read_cycles -= insn_cycles;
        if (xmem_read_cycles <= 0) {
            xmem_read_cycles = 0;
            xrd = xmem[xmem_read_address];
            if (!flag(CR3, XWORD)) xrd = s24(u24(xrd) & 0xffff00);
        }
    }
}


Core::Core(const Options& chosen) : xmem(chosen.xmem_words > 0x10000 ? chosen.xmem_words : 0x10000, 0), options(chosen) { reset(); }

void Core::reset() {
    pc = 0;
    sp = 0;
    rptc = 0;
    xmem_read_address = 0;
    xmem_read_cycles = 0;
    cr[CR0] = 0xAA9BAD;
    cr[CR1] = 0x810100;
    cr[CR2] = 0x30FF00;
    cr[CR3] = 0xE50000;
    for (int i = 0; i < 2; i++) { single_reg[i] = -1; dual_reg[i] = -1; }
}

void Core::load_control(const uint8_t* h) {
    for (int i = 0; i < 4; i++) cr[i] = h[i * 3] << 16 | h[i * 3 + 1] << 8 | h[i * 3 + 2];
}

void Core::note_unknown(uint32_t key) { unknown[key]++; }

int Core::study_pick_shift(int product_shift, int now_shift, int at_pc, int path, bool count) const {
    int here = (at_pc - 1) & 0x1ff;
    if (count && product_shift != now_shift) shift_mismatch[here]++;
    if (!options.shift_with_product || !(options.shift_paths & path)) return now_shift;
    if (!shift_pcs.empty()) {
        bool listed = false;
        for (int p : shift_pcs) if (p == here) listed = true;
        if (!listed) return now_shift;
    }
    return product_shift;
}

int32_t Core::mac_upper(int64_t value) const { return Machine(const_cast<Core&>(*this)).mac_upper(value); }
uint32_t Core::mac_lower(int64_t value) const { return Machine(const_cast<Core&>(*this)).mac_lower(value); }
uint32_t Core::study_cmem(uint32_t word) const { return Machine(const_cast<Core&>(*this)).cmem_operand(word); }
uint32_t Core::study_dmem(uint32_t word) const { return Machine(const_cast<Core&>(*this)).dmem_operand(word); }
uint32_t Core::study_xmem(uint32_t address) const { return Machine(const_cast<Core&>(*this)).xmem_index(address); }

// Any program, word by word.
void Core::run_steps(int steps) {
    Machine m(*this);
    while (steps-- > 0) m.step_word(m.pmem[m.pc], false);
    m.store();
}

// What a translated half takes for granted at its first word: nothing queued for the address
// registers, no XMEM read on its way, no repeat, the CR1 mode bits its translation assumed, and
// CR3 and the DMEM ring size the program was translated for (no word of it changes those).
bool Core::fast_ready(int half) const {
    return !study_shift && rptc == 0 && xmem_read_cycles == 0 && single_reg[0] < 0 && single_reg[1] < 0 && dual_reg[0] < 0 && dual_reg[1] < 0
        && (cr[CR1] & fast_mask[half]) == fast_value[half] && cr[CR3] == fast_cr3 && dcirc == fast_dcirc && doff <= dcirc;
}

void Core::run(int steps) {
    while (steps > 0) {
        if (fast_program >= 0 && steps >= 256 && (pc & 0xff) == 0) {
            int half = pc >> 8;
            if (fast_ready(half)) { (this->*fast_table[fast_program][half])(); steps -= 256; fast_halves++; continue; }
        }
        int n = fast_program >= 0 ? 1 : steps;  // with a translation, look again after each word
        run_steps(n);
        steps -= n;
        slow_steps += n;
    }
}

// The engine's options, which the translated code is compiled for.
static bool engine_options(const Options& o) {
    return o.shift_paths == 3 && o.audio_port_writes && o.true_dmem_ring && o.lock_delay && o.maclo_multiply && o.divide_1fc && o.noise_generator
        && o.late_mode_change && o.compare_3e && o.maclo_shift && o.maclo_unsigned && o.queued_pointer_load && o.bus_sum_saturates
        && o.reuse_coefficient && o.late_saturation_mode && o.xread_flush && o.late_mode_mask == 0xf && o.shift_with_product == 0;
}

static TMS_INLINE Options fast_options(int xread, uint32_t xwords) {
    Options o;
    o.audio_port_writes = o.true_dmem_ring = o.lock_delay = o.maclo_multiply = o.divide_1fc = o.noise_generator = true;
    o.late_mode_change = o.compare_3e = o.maclo_shift = o.maclo_unsigned = o.queued_pointer_load = o.bus_sum_saturates = true;
    o.reuse_coefficient = o.late_saturation_mode = o.xread_flush = true;
    o.xread_cycles = xread;
    o.xmem_words = xwords;
    return o;
}

// One translated half: the registers come onto the stack, 256 constant words run in a line, the
// registers go back. FAST_ASSUME tells the compiler what fast_ready() and select_fast() checked.
#if defined(__clang__)
#define FAST_ASSUME(x) __builtin_assume(x)
#else
#define FAST_ASSUME(x) (void)0
#endif
#define FAST_ENTER(mask, value, cr3, ring, xread, xwords) \
    Machine m(*this); \
    m.options = fast_options(xread, xwords);  /* written out, not assumed: select_fast() checked they are these */ \
    m.study_shift = false; \
    FAST_ASSUME(m.rptc == 0 && m.xmem_read_cycles == 0 && m.single_reg[0] < 0 && m.single_reg[1] < 0 && m.dual_reg[0] < 0 && m.dual_reg[1] < 0); \
    FAST_ASSUME((m.cr[1] & (mask)) == (value)); \
    FAST_ASSUME(m.cr[3] == (cr3) && m.dcirc == (ring) && m.doff <= m.dcirc); \
    (void)0;
#define W(word) m.step_word(word, true);
#define FAST_LEAVE m.store();

// The OB-12's DSP programs translated, made by tools/gen_fast.py from rom/dsp into build/
// (firmware stays out of git). Without the file every program runs word by word.
#if defined(__has_include) && __has_include("tms_fast.inc") && !defined(TMS_NO_FAST)
#include "tms_fast.inc"
#else
void Core::fast_0a() {} void Core::fast_0b() {} void Core::fast_1a() {} void Core::fast_1b() {} void Core::fast_2a() {} void Core::fast_2b() {}
const Core::FastHalf Core::fast_table[][2] = {{&Core::fast_0a, &Core::fast_0b}};
void Core::select_fast() { fast_program = -1; }
#endif

}  // namespace tms
