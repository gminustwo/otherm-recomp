#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL3/SDL_audio.h>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();
    // Wii AI DMA frames are big-endian and ordered right, left. SDL expects
    // native-endian interleaved left, right samples.
    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);

    // Applied to the final host output, covering both AX and direct AI DMA.
    void SetMasterVolume(float volume);
    void SetMuted(bool muted);
    // Holds the output device (Options pauses the game): the audio already queued stays queued and plays on resume.
    void SetPaused(bool paused);
    // Fast-forward: keep a larger cushion queued (its frames are heavier and its delay does not matter).
    void SetFastForward(bool on);

private:
    AudioBackend() = default;
    ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels);
    bool PutLocked(const void* data, int lenBytes);
    void RegulateLocked(int queuedBefore, int queuedAfter);
    void ApplyDeviceStateLocked();
    double BytesPerMsLocked() const;
    double TargetMsLocked(bool fastForward) const;  // audio kept queued, from the device's pull size
    float EffectiveGainLocked() const;
    void ApplyGainLocked();

    mutable std::mutex m_mutex;
    SDL_AudioStream* m_stream = nullptr;
    SDL_AudioSpec m_spec{};
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    bool m_initialized = false;
    float m_masterVolume = 1.0f;
    bool m_muted = false;
    bool m_paused = false;
    bool m_fastForward = false;
    bool m_buffering = true;      // the device waits until the target amount is queued (start, or after running dry)
    bool m_deviceRunning = false;
    double m_levelMs = 0.0;       // smoothed audio waiting for the device
    double m_deviceBufferMs = 0.0;  // how much the device takes at a time (SDL_GetAudioDeviceFormat), 0 = unknown
    float m_ratio = 1.0f;         // playback speed SDL resamples to
    uint32_t m_starved = 0;       // times the device ran dry
    uint32_t m_dropped = 0;       // blocks dropped at the limit
    bool m_reportedDroppedBlock = false;
    std::vector<int16_t> m_convertBuffer;
};
