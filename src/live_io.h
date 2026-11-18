// Sound card and MIDI input for live running. Kept apart from the emulator so windows.h stays out.
#pragma once
#include <cstdint>

// 48 kHz stereo 16 bit. `silent` keeps the same timing but sends zeros to the device.
bool live_audio_open(int buffers, int frames_per_buffer, bool silent);
// Blocks until a buffer is free: this is what paces the emulator.
void live_audio_write(const int16_t* frames);
long live_audio_underruns();
long live_audio_buffers_sent();
void live_audio_close();

int live_midi_devices();
const char* live_midi_name(int device);
bool live_midi_open(int device);
int live_midi_read(uint8_t* bytes, int capacity);  // complete short messages only
void live_midi_close();
