// The instrument as a library: two CPUs and five DSPs behind a small live interface.
// One thread calls run(). Any thread may call the input functions; they take effect at the next
// millisecond. ob12_system.cpp stays the research tool; this is what the player and plugin use.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Engine {
public:
    explicit Engine(const std::string& rom_dir);
    ~Engine();

    // Runs the machine until n frames at 48 kHz are out. 24 bit values, left and right interleaved.
    void run(int32_t* frames, int n);

    void midi(const uint8_t* bytes, int count);          // into the Master's MIDI input  (31250 baud, a .syx whole)

    // The Master's flash (29F800, 1 MB) is the unit's only memory that survives switching off:
    // the SRAM has no battery. Call before the first run(): sectors stored in the file replace
    // the factory image; after the firmware writes flash and goes quiet for a second, the
    // sectors that differ from the factory image are saved there again, and at the end.
    void set_flash_file(const std::string& path);
    long flash_writes() const;                           // erases and words programmed so far

    // A .syx (or the sysex of a .mid) as a sequencer plays it into MIDI IN: one message at a time,
    // the next after the previous has gone out on the line and gap_ms has passed. The factory
    // files carry about 100 ms between messages; the firmware writes flash in those gaps.
    void send_sysex(const uint8_t* bytes, int count, int gap_ms = 100);
    int sysex_waiting() const;                           // messages not yet sent
    // The sysex of a .syx file, or of a Standard MIDI File's F0 events, as F0 ... F7 messages.
    static std::string read_sysex(const std::wstring& path, std::vector<uint8_t>& out);
    // Back to the factory memory: the stored flash is deleted and no longer written this session.
    void forget_flash();
    // MIDI OUT's system exclusive since the last take, as F0 ... F7 messages: a bulk dump from
    // SYSTEM, BULK DUMP lands here. With take, the bytes move into it and the store empties.
    size_t sysex_sent(std::vector<uint8_t>* take = nullptr);
    void set_pot(int channel, int mux, int value);       // ADC channel 0-7, mux 0-7, value 0-255
    void key(int bar, int bit, bool down);               // button bar 0-11, BUT bit 0-7
    void turn(int detents);                              // data encoder

    bool sounding_ready() const;                         // the Slave has reported in
    // 240 x 64, one bit per pixel, 30 bytes a row. False until the Master has set up the LCD.
    bool lcd(uint8_t* bitmap) const;
    // The 16 LED latch bytes the Master last wrote (CS2, 68FF80-8F).
    void leds(uint8_t* latches) const;

private:
    struct Impl;
    Impl* impl;
};
