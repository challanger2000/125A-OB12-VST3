// Minimal stand-in for MAME's emu.h, so src/f2mc16 compiles unchanged outside MAME.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#define ATTR_COLD
#define NAME(x) x, #x
#define DECLARE_DEVICE_TYPE(Type, Class) extern const device_type Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) const device_type Type = nullptr;

enum { AS_PROGRAM = 0 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };
enum endianness_t { ENDIANNESS_LITTLE };

using device_type = const void*;
class machine_config {};
class device_t {};

namespace util { class disasm_interface { public: virtual ~disasm_interface() = default; }; }

class address_space_config {
public:
    address_space_config(const char*, endianness_t, int, int, int) {}
};

class address_space {
public:
    virtual ~address_space() = default;
    // What the machine does for an address: every access outside the direct windows.
    virtual u8 slow_read(u32 address) = 0;
    virtual void slow_write(u32 address, u8 data) = 0;

    // Host addition: plain memory can be reached without the virtual call. A window covers
    // addresses base .. base + size - 1 (after the 24 bit mask) and maps them onto data[offset &
    // mask]; size 0 is no window. Two for reading (code and RAM), one for writing (RAM). A machine
    // opens them only when nothing watches those accesses: see go_lean in ob12_master.cpp.
    struct Window { u32 base = 0, size = 0, mask = 0; u8* data = nullptr; };
    Window read_window[2], write_window;
    u8 read_byte(u32 address) {
        u32 a = address & 0xffffff;
        u32 offset = a - read_window[0].base;
        if (offset < read_window[0].size) return read_window[0].data[offset & read_window[0].mask];
        offset = a - read_window[1].base;
        if (offset < read_window[1].size) return read_window[1].data[offset & read_window[1].mask];
        return slow_read(address);
    }
    void write_byte(u32 address, u8 data) {
        u32 offset = (address & 0xffffff) - write_window.base;
        if (offset < write_window.size) { write_window.data[offset & write_window.mask] = data; return; }
        slow_write(address, data);
    }
    u16 read_word(u32 address) { return read_byte(address) | read_byte(address + 1) << 8; }
    u32 read_dword(u32 address) { return read_word(address) | (u32)read_word(address + 2) << 16; }
    void write_word(u32 address, u16 data) { write_byte(address, data & 0xff); write_byte(address + 1, data >> 8); }
    void write_dword(u32 address, u32 data) { write_word(address, data & 0xffff); write_word(address + 2, data >> 16); }
};

class device_state_entry {
public:
    int index() const { return 0; }
    device_state_entry& callimport() { return *this; }
    device_state_entry& callexport() { return *this; }
    device_state_entry& noshow() { return *this; }
    device_state_entry& formatstr(const char*) { return *this; }
};

class device_memory_interface {
public:
    using space_config_vector = std::vector<std::pair<int, const address_space_config*>>;
};

struct machine_stub {
    const char* describe_context() const { return "ob12"; }
};

inline u16 swapendian_int16(u16 value) { return (u16)(value << 8 | value >> 8); }

inline std::string string_format(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return buffer;
}

[[noreturn]] inline void fatalerror(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    throw std::runtime_error(buffer);
}

class cpu_device : public device_t, public device_memory_interface {
public:
    cpu_device(const machine_config&, device_type, const char*, device_t*, u32) {}
    virtual ~cpu_device() = default;

    // Host interface, not part of MAME.
    void attach(address_space* space) { m_space = space; device_start(); *m_icountptr = 0; }
    void reset() { device_reset(); }
    // The last instruction overruns the budget; the debt carries into the next run, so over time
    // the core runs exactly the cycles it is given. executed() is what this run used.
    void run(int cycles) {
        s32 debt = *m_icountptr < 0 ? *m_icountptr : 0;
        *m_icountptr = m_run_from = debt + cycles;
        if (m_run_from > 0) execute_run();
    }
    int executed() const { return m_run_from - *m_icountptr; }
    int cycles_left() const { return *m_icountptr; }

protected:
    virtual void device_start() = 0;
    virtual void device_reset() = 0;
    virtual void state_import(const device_state_entry&) {}
    virtual void state_export(const device_state_entry&) {}
    virtual void state_string_export(const device_state_entry&, std::string&) const {}
    virtual void execute_run() = 0;
    virtual void execute_set_input(int, int) {}
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() = 0;
    virtual space_config_vector memory_space_config() const = 0;

    // Overridden by the machine to observe execution and deliver interrupts.
    virtual void debugger_instruction_hook(u32) {}
    virtual int standard_irq_callback(int, u32) { return 0; }

    address_space& space(int) { return *m_space; }
    void set_icountptr(s32& icount) { m_icountptr = &icount; }
    template <typename T> device_state_entry& state_add(int, const char*, T&) { return m_entry; }
    template <typename... Args> void save_item(Args&&...) {}
    machine_stub machine() const { return {}; }
    // An unimplemented instruction must stop the run: continuing would fake the result.
    void logerror(const char* format, ...) {
        char buffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof buffer, format, args);
        va_end(args);
        throw std::runtime_error(buffer);
    }

private:
    address_space* m_space = nullptr;
    s32* m_icountptr = nullptr;
    s32 m_run_from = 0;
    device_state_entry m_entry;
};
