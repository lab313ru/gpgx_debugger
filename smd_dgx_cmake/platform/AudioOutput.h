#pragma once
#include <cstdint>
#include <memory>

// Audio device for the emulator hosts.
//
// The interface is deliberately platform-neutral: this header used to pull in
// <windows.h>, so every host that merely wanted sound stopped compiling
// anywhere else. The backend lives entirely in the .cpp behind a pimpl —
// waveOut on Windows, SDL2 elsewhere.
class AudioOutput
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels   = 2;

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&)            = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Interleaved stereo frames, written from the emulation thread.
    void write(const int16_t* stereo, int frameCount);
    void pause(bool p);

    // False when no device could be opened, or the platform has no backend;
    // hosts keep running, just silently.
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
