#include <chrono>
#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <SDL3/SDL_init.h>

namespace {
// Audio waiting for the output device that the regulation (RegulateLocked) keeps. Enough to ride out a slow game
// frame (the guest thread makes audio between frames) and the device's own pull size (~10 ms).
constexpr double kTargetMs = 60.0;
// Fast-forward keeps more: its frames are heavier and its delay does not matter (the queue dipped under 10 ms in 5-9 of
// 50 seconds at 60 ms, boots aq6/aq7). Afterwards the speed regulation drains back to kTargetMs.
constexpr double kFastForwardTargetMs = 120.0;
// Beyond this the new block is dropped (a catch-up after a long stall can add up to 100 ms on top of the target).
constexpr double kLimitMs = 250.0;
// Largest playback speed change: 0.5% is about a twelfth of a semitone, not heard as pitch. Above twice the target the
// queue drains faster, up to 1%.
constexpr double kMaxSlower = 0.005;
constexpr double kMaxFaster = 0.010;
}  // namespace

AudioBackend& AudioBackend::Instance() {
    static AudioBackend instance;
    return instance;
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

// The device plays unless Options holds it or the queue is still filling up to the target.
void AudioBackend::ApplyDeviceStateLocked() {
    if (!m_stream) {
        return;
    }
    const bool run = !m_paused && !m_buffering;
    if (run == m_deviceRunning) {
        return;
    }
    if (run ? !SDL_ResumeAudioStreamDevice(m_stream) : !SDL_PauseAudioStreamDevice(m_stream)) {
        RT_LOG(RT_TAG_AUDIO) << (run ? "SDL_ResumeAudioStreamDevice" : "SDL_PauseAudioStreamDevice")
                             << " failed: " << SDL_GetError() << std::endl;
        return;
    }
    m_deviceRunning = run;
}

void AudioBackend::SetFastForward(bool on) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fastForward = on;
    if (on && m_stream && !m_buffering) {
        // The speed regulation would take ~12 s to build the larger cushion: wait for it once instead.
        m_buffering = true;
        ApplyDeviceStateLocked();
    }
}

void AudioBackend::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paused = paused;
    ApplyDeviceStateLocked();
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    // With no usable output device (none plugged in, or it went away) every 3 ms AI DMA block asked for one again:
    // Other M boot 46 logged 112,620 failed opens in 6 minutes and fast-forward slowed to a crawl (boot 48). Wait
    // between attempts, report the first failure only, and say when a device is back.
    static std::chrono::steady_clock::time_point s_retryAt{};
    static bool s_failing = false;
    const auto now = std::chrono::steady_clock::now();
    if (s_failing && now < s_retryAt) {
        return false;
    }
    const auto fail = [&](const char* what) {
        if (!s_failing) {
            RT_LOG(RT_TAG_AUDIO) << what << " failed: " << SDL_GetError() << " (retrying every 2 s, silent until then)" << std::endl;
        }
        s_failing = true;
        s_retryAt = now + std::chrono::seconds(2);
        return false;
    };

    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return fail("SDL_InitSubSystem(SDL_INIT_AUDIO)");
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

    // The device opens paused; ApplyDeviceStateLocked starts it once the target amount is queued.
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        return fail("SDL_OpenAudioDeviceStream");
    }

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        fail("SDL_SetAudioStreamGain");
        SDL_DestroyAudioStream(stream);
        return false;
    }

    if (s_failing) {
        RT_LOG(RT_TAG_AUDIO) << "audio output device available again" << std::endl;
        s_failing = false;
    }
    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    m_deviceRunning = false;
    m_buffering = true;
    m_levelMs = 0.0;
    m_ratio = 1.0f;
    RT_LOGF(RT_TAG_AUDIO, "audio output opened: %u Hz, %u channels\n", sampleRate, channels);
    // Which device the sound goes to and its own format (a TV over HDMI, a headset, ...): bug reports about sound
    // need it. A device the player renamed ("<name>'s headphones") keeps only what follows the name.
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
    const char* rawName = SDL_GetAudioDeviceName(device);
    std::string name = rawName != nullptr ? rawName : "unknown";
    for (const char* mark : {"'s ", "\xE2\x80\x99s "}) {
        if (const size_t at = name.find(mark); at != std::string::npos) name = name.substr(at + std::strlen(mark));
    }
    SDL_AudioSpec deviceSpec{};
    int deviceFrames = 0;
    if (SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames)) {
        m_deviceBufferMs = deviceSpec.freq > 0 && deviceFrames > 0 ? 1000.0 * deviceFrames / deviceSpec.freq : 0.0;
        RT_LOGF(RT_TAG_AUDIO, "device: %s (driver %s), %d Hz, %d channels, %d frames a buffer; keeping %.0f ms queued\n",
                name.c_str(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", deviceSpec.freq,
                deviceSpec.channels, deviceFrames, TargetMsLocked(false));
    } else {
        RT_LOGF(RT_TAG_AUDIO, "device: %s (driver %s), format unknown: %s\n", name.c_str(),
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", SDL_GetError());
    }
    return true;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_reportedDroppedBlock = false;
    m_deviceRunning = false;
    m_convertBuffer.clear();
}

double AudioBackend::BytesPerMsLocked() const {
    return static_cast<double>(m_spec.freq) * m_spec.channels * sizeof(int16_t) / 1000.0;
}

// Puts one block in the device's queue, unless that would pass kLimitMs: then the block is dropped and the continuous
// audio already queued is kept, as Dolphin's FIFO does (clearing SDL's whole stream made a severe crackle).
bool AudioBackend::PutLocked(const void* data, int lenBytes) {
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued >= 0 && queued + lenBytes > kLimitMs * BytesPerMsLocked()) {
        ++m_dropped;
        if (!m_reportedDroppedBlock) {  // a drop is audible: report the first one so it is not invisible
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full (" << queued << " bytes); dropping blocks to preserve continuity"
                                 << std::endl;
        }
        RegulateLocked(queued, queued);
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, data, lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }
    // OTHERM_AUDIO_OUT_DUMP=<file>: what goes to the device, as queued (16-bit little-endian stereo, left first).
    static std::FILE* outDump = [] {
        const char* path = std::getenv("OTHERM_AUDIO_OUT_DUMP");
        return path != nullptr && path[0] != '\0' ? std::fopen(path, "wb") : nullptr;
    }();
    if (outDump != nullptr) {
        std::fwrite(data, 1, static_cast<size_t>(lenBytes), outDump);
        std::fflush(outDump);
    }
    if (queued >= 0) {
        RegulateLocked(queued, queued + lenBytes);
    }
    return true;
}

// The game makes its audio on the wall clock and the device plays it on its own clock, so what waits in between was
// whatever startup left and then drifted: 0-11 ms after a fresh start (the device starved: crackling) until a loading
// stall's catch-up raised it, and ~1 ms lower every two seconds after that (OTHERM_AUDIO_QUEUE_LOG). Now the device
// waits until kTargetMs is queued, at the start and whenever it ran dry, and then plays a little faster or slower
// (SDL resamples) to hold that level.
// kTargetMs assumes a device that takes ~10 ms at a time. A Steam Deck under PipeWire takes 1024 frames at 44.1 kHz
// (23.2 ms), sometimes two at once, while the game hands over ~16 ms once a frame (32-52 ms after a slow frame): up
// to 70 ms went between two pushes and the 60 ms queue ran dry several times a minute, each time a gap and a refill
// (OTHERM_AUDIO_QUEUE_TRACE, NOTES.md 2026-11-23). The target grows with the device's pull: 3 pulls plus 30 ms (100 ms
// on the Deck; a ~10 ms device keeps 60).
double AudioBackend::TargetMsLocked(bool fastForward) const {
    const double target = std::max(kTargetMs, 3.0 * m_deviceBufferMs + 30.0);
    return fastForward ? std::max(kFastForwardTargetMs, target + 60.0) : target;
}

void AudioBackend::RegulateLocked(int queuedBefore, int queuedAfter) {
    const double bytesPerMs = BytesPerMsLocked();
    const double targetMs = TargetMsLocked(m_fastForward);
    const double afterMs = queuedAfter / bytesPerMs;
    if (m_buffering) {
        if (afterMs >= targetMs) {
            m_buffering = false;
            m_levelMs = afterMs;
            ApplyDeviceStateLocked();
        }
    } else if (queuedBefore < bytesPerMs && m_deviceRunning) {
        // Under 1 ms left: the device ran dry since the last block, or is about to (it pulls ~10 ms at a time). One
        // short wait now instead of a crackle at every block.
        ++m_starved;
        m_buffering = true;
        ApplyDeviceStateLocked();
    } else {
        m_levelMs += 0.02 * (afterMs - m_levelMs);  // about 150 ms of blocks
        const double change = std::clamp(kMaxSlower * (m_levelMs - targetMs) / targetMs, -kMaxSlower, kMaxFaster);
        const float ratio = static_cast<float>(1.0 + change);
        if (std::fabs(ratio - m_ratio) >= 0.0002f) {
            if (SDL_SetAudioStreamFrequencyRatio(m_stream, ratio)) {
                m_ratio = ratio;
            }
        }
    }

    // OTHERM_AUDIO_QUEUE_TRACE=<from>:<count>: one line per push, pushes <from>..<from>+<count>: the time since the
    // first traced push, the queue before and after it (equal: the block was dropped), and whether the device waits.
    static const auto trace = [] {
        unsigned from = 0, count = 0;
        if (const char* v = std::getenv("OTHERM_AUDIO_QUEUE_TRACE")) std::sscanf(v, "%u:%u", &from, &count);
        return std::pair<unsigned, unsigned>{from, count};
    }();
    if (trace.second != 0) {
        static unsigned push = 0;
        static auto first = std::chrono::steady_clock::now();
        if (push == trace.first) first = std::chrono::steady_clock::now();
        if (push >= trace.first && push - trace.first < trace.second) {
            const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first).count();
            RT_LOGF(RT_TAG_AUDIO, "[aqt] %u t %.2f before %.1f after %.1f level %.1f%s\n", push, t,
                    queuedBefore / bytesPerMs, queuedAfter / bytesPerMs, m_levelMs, m_buffering ? " filling" : "");
        }
        ++push;
    }

    // OTHERM_AUDIO_QUEUE_LOG: once a second, the least and most audio waiting for the device, the playback speed,
    // and how often it ran dry or a block was dropped.
    static const bool logQueue = std::getenv("OTHERM_AUDIO_QUEUE_LOG") != nullptr;
    if (logQueue) {
        static int least = INT32_MAX, most = 0, pushes = 0;
        static auto since = std::chrono::steady_clock::now();
        least = std::min(least, queuedBefore);
        most = std::max(most, queuedAfter);
        ++pushes;
        if (const auto now = std::chrono::steady_clock::now(); now - since >= std::chrono::seconds(1)) {
            RT_LOGF(RT_TAG_AUDIO, "[audio] queue %.0f..%.0f ms over %d pushes, speed %.4f, ran dry %u, dropped %u%s\n",
                    least / bytesPerMs, most / bytesPerMs, pushes, m_ratio, m_starved, m_dropped,
                    m_buffering ? " (filling)" : "");
            least = INT32_MAX;
            most = pushes = 0;
            since = now;
        }
    }
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    if (m_convertBuffer.size() < sampleCount) {
        m_convertBuffer.resize(sampleCount);
    }

    const size_t frameCount = sampleCount / 2;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        m_convertBuffer[frame * 2] = static_cast<int16_t>(left);
        m_convertBuffer[frame * 2 + 1] = static_cast<int16_t>(right);
    }

    return PutLocked(m_convertBuffer.data(), static_cast<int>(frameCount * 2 * sizeof(int16_t)));
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    return PutLocked(samples, static_cast<int>(sampleCount * sizeof(int16_t)));
}
