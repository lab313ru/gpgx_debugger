#include "AudioOutput.h"

#include <algorithm>
#include <cstdlib>
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
    // A run driven over the bridge has no listener. Without a device nothing
    // reaches the system mixer, and the host goes on silently (isOpen false).
    if (std::getenv("DGX_NO_AUDIO") != nullptr)
        return;

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
// Everywhere else: SDL2, loaded at run time
//
// The library is dlopen()ed rather than linked. The IDA plugin links this
// file, and IDA does not ship SDL: a hard dependency would stop the plugin
// loading at all on a machine without it. Without the library the hosts run
// silently, exactly as when no device can be opened. Only the headers are
// needed to build.
//
// SDL pulls from a callback on its own thread while the emulation thread
// pushes, so the two meet in a ring buffer. write() drops the oldest audio
// rather than blocking: the emulation thread must never be parked on the sound
// card, and a debugger that is single-stepping produces samples in bursts that
// no device pacing can follow.
// ===========================================================================
#else

#include <SDL.h>
#include <dlfcn.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>

namespace {
// Four frames at 60 Hz. Enough to ride out scheduling jitter, short enough
// that resuming from a breakpoint does not replay a stale half-second.
constexpr int kRingFrames = 8192;

// The SDL entry points this file uses. decltype keeps each pointer's type tied
// to the header's declaration without referencing the symbol itself.
struct SdlApi {
    decltype(&SDL_WasInit)          WasInit          = nullptr;
    decltype(&SDL_InitSubSystem)    InitSubSystem    = nullptr;
    decltype(&SDL_QuitSubSystem)    QuitSubSystem    = nullptr;
    decltype(&SDL_OpenAudioDevice)  OpenAudioDevice  = nullptr;
    decltype(&SDL_PauseAudioDevice) PauseAudioDevice = nullptr;
    decltype(&SDL_CloseAudioDevice) CloseAudioDevice = nullptr;
    decltype(&SDL_SetHint)          SetHint          = nullptr;   // optional

    bool ok() const
    {
        return WasInit && InitSubSystem && QuitSubSystem && OpenAudioDevice
            && PauseAudioDevice && CloseAudioDevice;
    }
};

// Loaded once and never unloaded: SDL keeps an audio thread alive until its
// subsystem quits, and an unmapped library under a live thread is a crash.
//
// A failure is said once on stderr. Silence alone does not tell a missing SDL
// from an SDL that is there but cannot load because a library it needs
// (libgbm, libdrm, libwayland-client...) is not, and only the dynamic
// linker's message names which.
const SdlApi* loadSdl()
{
    static const SdlApi api = [] {
        SdlApi a;
        static const char* const names[] = {
#ifdef __APPLE__
            "libSDL2-2.0.0.dylib", "libSDL2.dylib",
#else
            "libSDL2-2.0.so.0", "libSDL2.so",
#endif
        };
        void* lib = nullptr;
        std::string why;                    // about the first name: the real one
        for (const char* n : names) {
            if ((lib = dlopen(n, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
            if (why.empty())
                if (const char* e = dlerror()) why = e;
        }
        if (!lib) {
            std::fprintf(stderr, "smd_dgx: no sound, SDL2 did not load: %s\n", why.c_str());
            return a;
        }

        auto bind = [lib](auto& fn, const char* sym) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, sym));
        };
        bind(a.WasInit,          "SDL_WasInit");
        bind(a.InitSubSystem,    "SDL_InitSubSystem");
        bind(a.QuitSubSystem,    "SDL_QuitSubSystem");
        bind(a.OpenAudioDevice,  "SDL_OpenAudioDevice");
        bind(a.PauseAudioDevice, "SDL_PauseAudioDevice");
        bind(a.CloseAudioDevice, "SDL_CloseAudioDevice");
        bind(a.SetHint,          "SDL_SetHint");
        if (!a.ok())
            std::fprintf(stderr, "smd_dgx: no sound, %s lacks SDL2 audio functions\n", names[0]);
        return a;
    }();
    return api.ok() ? &api : nullptr;
}
} // namespace

struct AudioOutput::Impl {
    const SdlApi* sdl = nullptr;   // null: no library, or audio disabled
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
    // The library is loaded even for a silenced run: loading opens no device,
    // and it keeps two things true when DGX_NO_AUDIO is set. A run still says
    // on stderr why SDL would not load, and a package check, which runs
    // silent, can see in the process's maps which libSDL2 was picked up.
    const SdlApi* sdl = loadSdl();

    // Same switch as the waveOut path: a scripted run stays off the mixer.
    if (!sdl || std::getenv("DGX_NO_AUDIO") != nullptr)
        return;

    // The host may already have initialised SDL for video; init the subsystem
    // we need and only quit the part we started.
    if (sdl->WasInit(SDL_INIT_AUDIO) == 0) {
        // SDL2 brings the events subsystem up with audio, and that first init
        // hooks SIGINT and SIGTERM wherever they were left at the default,
        // turning them into an SDL_QUIT event. Nothing here reads events, so
        // the standalone app would ignore kill and Ctrl+C for as long as the
        // device is open, and the IDA plugin would rewire its host's signals.
        // If the host already runs SDL events, its handlers are its own choice.
        if (sdl->SetHint && sdl->WasInit(SDL_INIT_EVENTS) == 0)
            sdl->SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        if (sdl->InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        impl_->ownsSdl = true;
    }
    impl_->sdl = sdl;

    SDL_AudioSpec want{}, have{};
    want.freq     = kSampleRate;
    want.format   = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples  = 1024;
    want.callback = &Impl::feed;
    want.userdata = impl_.get();

    impl_->dev = sdl->OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (impl_->dev != 0) sdl->PauseAudioDevice(impl_->dev, 0);
}

AudioOutput::~AudioOutput()
{
    if (!impl_->sdl) return;
    if (impl_->dev != 0) impl_->sdl->CloseAudioDevice(impl_->dev);
    if (impl_->ownsSdl) impl_->sdl->QuitSubSystem(SDL_INIT_AUDIO);
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
    impl_->sdl->PauseAudioDevice(impl_->dev, p ? 1 : 0);
    if (p) {                                          // drop what was queued
        std::lock_guard<std::mutex> lk(impl_->mx);
        impl_->head = impl_->tail = impl_->fill = 0;
    }
}

#endif
