#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include "live_io.h"
#pragma comment(lib, "winmm.lib")

static HWAVEOUT wave = nullptr;
static std::vector<WAVEHDR> headers;
static std::vector<std::vector<int16_t>> blocks;
static HANDLE freed = nullptr;
static int frames_each = 0, next_header = 0;
static bool quiet = false;
static std::atomic<int> queued{0};
static long underruns = 0, sent = 0;

static void CALLBACK wave_done(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (message != WOM_DONE) return;
    queued.fetch_sub(1);
    SetEvent(freed);
}

bool live_audio_open(int buffers, int frames_per_buffer, bool silent) {
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 48000 * 4;
    freed = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&wave, WAVE_MAPPER, &format, (DWORD_PTR)wave_done, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return false;
    frames_each = frames_per_buffer;
    quiet = silent;
    headers.resize(buffers);
    blocks.resize(buffers);
    for (int i = 0; i < buffers; i++) {
        blocks[i].assign(frames_per_buffer * 2, 0);
        memset(&headers[i], 0, sizeof(WAVEHDR));
        headers[i].lpData = (LPSTR)blocks[i].data();
        headers[i].dwBufferLength = frames_per_buffer * 4;
        waveOutPrepareHeader(wave, &headers[i], sizeof(WAVEHDR));
    }
    return true;
}

void live_audio_write(const int16_t* frames) {
    while (queued.load() >= (int)headers.size()) WaitForSingleObject(freed, 2);
    if (sent > (long)headers.size() && queued.load() == 0) underruns++;
    WAVEHDR& header = headers[next_header];
    if (!quiet) memcpy(blocks[next_header].data(), frames, frames_each * 4);
    queued.fetch_add(1);
    waveOutWrite(wave, &header, sizeof(WAVEHDR));
    next_header = (next_header + 1) % (int)headers.size();
    sent++;
}

long live_audio_underruns() { return underruns; }
long live_audio_buffers_sent() { return sent; }

void live_audio_close() {
    if (!wave) return;
    waveOutReset(wave);
    for (auto& header : headers) waveOutUnprepareHeader(wave, &header, sizeof(WAVEHDR));
    waveOutClose(wave);
    wave = nullptr;
}

static HMIDIIN midi = nullptr;
static std::mutex midi_lock;
static std::vector<uint8_t> midi_bytes;

static void CALLBACK midi_in(HMIDIIN, UINT message, DWORD_PTR, DWORD_PTR packed, DWORD_PTR) {
    if (message != MIM_DATA) return;
    uint8_t status = packed & 0xff;
    if (status >= 0xf0) return;  // clock and active sensing stay out
    int length = (status & 0xe0) == 0xc0 ? 2 : 3;
    std::lock_guard<std::mutex> hold(midi_lock);
    for (int i = 0; i < length; i++) midi_bytes.push_back((packed >> (8 * i)) & 0xff);
}

int live_midi_devices() { return (int)midiInGetNumDevs(); }

const char* live_midi_name(int device) {
    static MIDIINCAPSA caps;
    if (midiInGetDevCapsA(device, &caps, sizeof caps) != MMSYSERR_NOERROR) return "";
    return caps.szPname;
}

bool live_midi_open(int device) {
    if (midiInOpen(&midi, device, (DWORD_PTR)midi_in, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return false;
    midiInStart(midi);
    return true;
}

int live_midi_read(uint8_t* bytes, int capacity) {
    std::lock_guard<std::mutex> hold(midi_lock);
    int count = (int)midi_bytes.size() < capacity ? (int)midi_bytes.size() : capacity;
    if (count) {
        memcpy(bytes, midi_bytes.data(), count);
        midi_bytes.erase(midi_bytes.begin(), midi_bytes.begin() + count);
    }
    return count;
}

void live_midi_close() {
    if (!midi) return;
    midiInStop(midi);
    midiInClose(midi);
    midi = nullptr;
}
