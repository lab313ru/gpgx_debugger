#include "AudioOutput.h"

#include <algorithm>
#include <cstring>

// ===========================================================================
// Windows: waveOut
// ===========================================================================
#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

namespace { constexpr int kBufFrames = 2048; constexpr int kNumBufs = 4; }

struct AudioOutput::Impl {
    HWAVEOUT hwo    = nullptr;
    WAVEHDR  hdr[kNumBufs]{};
    int16_t  buf[kNumBufs][kBufFrames * AudioOutput::kChannels]{};
    HANDLE   event  = nullptr;
    int      head   = 0;
    volatile LONG queued = 0;

    static void CALLBACK waveProc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR, DWORD_PTR)
    {
        if (msg != WOM_DONE) return;
        auto* self = reinterpret_cast<Impl*>(inst);
        InterlockedDecrement(&self->queued);
        SetEvent(self->event);
    }
};

AudioOutput::AudioOutput() : impl_(new Impl)
{
    impl_->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = kChannels;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = kChannels * 2;
    wfx.nAvgBytesPerSec = kSampleRate * kChannels * 2;

    if (waveOutOpen(&impl_->hwo, WAVE_MAPPER, &wfx,
                    reinterpret_cast<DWORD_PTR>(&Impl::waveProc),
                    reinterpret_cast<DWORD_PTR>(impl_.get()),
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        impl_->hwo = nullptr;
        return;
    }

    for (int i = 0; i < kNumBufs; ++i) {
        impl_->hdr[i].lpData         = reinterpret_cast<LPSTR>(impl_->buf[i]);
        impl_->hdr[i].dwBufferLength = sizeof(impl_->buf[i]);
        waveOutPrepareHeader(impl_->hwo, &impl_->hdr[i], sizeof(WAVEHDR));
    }
}

AudioOutput::~AudioOutput()
{
    if (impl_->hwo) {
        waveOutReset(impl_->hwo);
        for (int i = 0; i < kNumBufs; ++i)
            waveOutUnprepareHeader(impl_->hwo, &impl_->hdr[i], sizeof(WAVEHDR));
        waveOutClose(impl_->hwo);
    }
    if (impl_->event) CloseHandle(impl_->event);
}

bool AudioOutput::isOpen() const { return impl_->hwo != nullptr; }

void AudioOutput::write(const int16_t* stereo, int frameCount)
{
    if (!impl_->hwo || !stereo) return;
    int remaining = frameCount;
    const int16_t* src = stereo;

    while (remaining > 0) {
        while (impl_->queued >= kNumBufs)          // wait for a free slot
            WaitForSingleObject(impl_->event, INFINITE);

        const int idx   = impl_->head;
        const int chunk = std::min(remaining, kBufFrames);
        std::memcpy(impl_->buf[idx], src, size_t(chunk) * kChannels * sizeof(int16_t));

        impl_->hdr[idx].dwBufferLength = DWORD(chunk * kChannels * sizeof(int16_t));
        InterlockedIncrement(&impl_->queued);
        waveOutWrite(impl_->hwo, &impl_->hdr[idx], sizeof(WAVEHDR));

        impl_->head = (impl_->head + 1) % kNumBufs;
        src       += chunk * kChannels;
        remaining -= chunk;
    }
}

void AudioOutput::pause(bool p)
{
    if (!impl_->hwo) return;
    if (p) waveOutPause(impl_->hwo);
    else   waveOutRestart(impl_->hwo);
}

// ===========================================================================
// Everywhere else: SDL2
//
// SDL2 is already a hard dependency of this build and the SDL frontend already
// opens a device at the same rate, so this adds no dependency — it only stops
// Linux and macOS being silent, which is the first thing a user notices and
// the least defensible gap in a project whose first requirement was to be
// cross-platform.
//
// SDL pulls from a callback on its own thread while the emulation thread
// pushes, so the two meet in a ring buffer. write() drops the oldest audio
// rather than blocking: the emulation thread must never be parked on the sound
// card, and a debugger that is single-stepping produces samples in bursts that
// no device pacing can follow.
// ===========================================================================
#else

#include <SDL.h>
#include <mutex>

namespace {
// Four frames at 60 Hz. Enough to ride out scheduling jitter, short enough
// that resuming from a breakpoint does not replay a stale half-second.
constexpr int kRingFrames = 8192;
} // namespace

struct AudioOutput::Impl {
    SDL_AudioDeviceID dev = 0;
    bool ownsSdl = false;

    std::mutex mx;
    int16_t ring[kRingFrames * AudioOutput::kChannels]{};
    size_t  head = 0;          // next write
    size_t  tail = 0;          // next read
    size_t  fill = 0;          // frames available

    static void SDLCALL feed(void* userdata, Uint8* stream, int len)
    {
        auto* self = static_cast<Impl*>(userdata);
        auto* out = reinterpret_cast<int16_t*>(stream);
        const size_t want = size_t(len) / sizeof(int16_t) / AudioOutput::kChannels;

        std::lock_guard<std::mutex> lk(self->mx);
        const size_t give = want < self->fill ? want : self->fill;
        for (size_t i = 0; i < give; ++i) {
            for (int ch = 0; ch < AudioOutput::kChannels; ++ch)
                out[i * AudioOutput::kChannels + ch] =
                    self->ring[self->tail * AudioOutput::kChannels + ch];
            self->tail = (self->tail + 1) % kRingFrames;
        }
        self->fill -= give;
        // Underrun: silence, not the previous buffer again. A repeated buffer
        // is a recognisable buzz and sounds like a bug in the emulator.
        std::memset(out + give * AudioOutput::kChannels, 0,
                    (want - give) * AudioOutput::kChannels * sizeof(int16_t));
    }
};

AudioOutput::AudioOutput() : impl_(new Impl)
{
    // The host may already have initialised SDL for video; init the subsystem
    // we need and only quit the part we started.
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        impl_->ownsSdl = true;
    }

    SDL_AudioSpec want{}, have{};
    want.freq     = kSampleRate;
    want.format   = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples  = 1024;
    want.callback = &Impl::feed;
    want.userdata = impl_.get();

    impl_->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (impl_->dev != 0) SDL_PauseAudioDevice(impl_->dev, 0);
}

AudioOutput::~AudioOutput()
{
    if (impl_->dev != 0) SDL_CloseAudioDevice(impl_->dev);
    if (impl_->ownsSdl) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioOutput::isOpen() const { return impl_->dev != 0; }

void AudioOutput::write(const int16_t* stereo, int frameCount)
{
    if (impl_->dev == 0 || !stereo || frameCount <= 0) return;

    std::lock_guard<std::mutex> lk(impl_->mx);
    for (int i = 0; i < frameCount; ++i) {
        if (impl_->fill == kRingFrames) {            // full: drop the oldest
            impl_->tail = (impl_->tail + 1) % kRingFrames;
            --impl_->fill;
        }
        for (int ch = 0; ch < kChannels; ++ch)
            impl_->ring[impl_->head * kChannels + ch] = stereo[i * kChannels + ch];
        impl_->head = (impl_->head + 1) % kRingFrames;
        ++impl_->fill;
    }
}

void AudioOutput::pause(bool p)
{
    if (impl_->dev == 0) return;
    SDL_PauseAudioDevice(impl_->dev, p ? 1 : 0);
    if (p) {                                          // drop what was queued
        std::lock_guard<std::mutex> lk(impl_->mx);
        impl_->head = impl_->tail = impl_->fill = 0;
    }
}

#endif
