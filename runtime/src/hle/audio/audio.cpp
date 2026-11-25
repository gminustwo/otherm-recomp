#include "timebase_contract.h"
#include "det_clock.h"
#include "guest_clock.h"
#include "memory.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "guest_flat_memory.h"
#include "ppc_runtime.h"
#include "audio_backend.h"
#include "ax_dsp.h"
#include "runtime_log.h"
#include "save_state.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

uint64_t PPC_ReadGuestTimeBase();  // ppc_helpers.cpp

namespace {
constexpr uint32_t kDefaultSampleRate = 32000u;
constexpr uint32_t kAudioChannels = 2u;
constexpr uint32_t kBytesPerSample = 2u;
constexpr uint32_t kAIInitializedAddr = 0x80332258u;          // __AI_init_flag
constexpr uint32_t kAICallbackBusyAddr = 0x8033225Cu;         // __AID_Active
constexpr uint32_t kAICallbackStackSwitchAddr = 0x8033228Cu;  // __CallbackStack
constexpr uint32_t kAIDmaCallbackAddr = 0x80332290u;          // __AID_Callback

// Max completed 3 ms DMA blocks delivered per tick. Draining several at once catches up
// backlog from a long frame without letting a large stall spiral into an unbounded loop.
// 8, not 4: a 60 Hz frame is 5.6 blocks, and on a Steam Deck the guest thread (busy ~14 of 16.7 ms) often polls only
// once a frame, so 4 delivered audio at 72% speed for ~0.8 s, then the backlog in a burst whenever it idled: the
// queue ran dry and overflowed about once a second (OTHERM_AUDIO_QUEUE_TRACE, NOTES.md 2026-11-23).
constexpr int kMaxBlocksPerTick = 8;

struct AIDmaState {
    std::mutex mutex;
    uint32_t startAddr = 0;
    uint32_t registerStartAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    bool enabled = false;
    uint32_t sampleRate = kDefaultSampleRate;
    uint32_t bytesLeft = 0;
    double accumulatorSeconds = 0.0;
    // Deterministic mode: the virtual tick at which the DMA block in flight completes (0 = no
    // block started yet). Integer ticks, not the floating accumulator, so every device delivers each
    // block at exactly the same point.
    uint64_t detBlockDueTicks = 0;
    bool tickActive = false;
    bool loggedBackendFailure = false;
    bool loggedMissingCallback = false;
    bool loggedAccessFailure = false;
};

AIDmaState g_ai{};
void PublishAiRegisters();

// Audio degradation is invisible to the player except as silence, so every
// notice below reaches stderr unconditionally. The ones that sit on the
// per-DMA-frame path keep their one-shot latch in g_ai.
void ReportAudioProblem(const char* who, const char* what) {
    RT_LOGF(RT_TAG_AUDIO, "%s: %s\n", who, what);
    std::fflush(stderr);
}

bool EnsureAudioBackend(uint32_t sampleRate) {
    return AudioBackend::Instance().Init(sampleRate, kAudioChannels);
}

uint32_t EncodeAIDmaStartRegister(uint32_t startAddr) {
    return startAddr & 0x1fffffe0u;
}

uint32_t EncodeAIDmaLengthRegister(uint32_t length) {
    return length & 0x000fffe0u;
}

// Bring-up aid: the loudest sample of the AI output, reported every ~15 s of 32 kHz stereo for the
// first minute, so a game that runs silently shows up in the log of an automated run.
void MeterAudioBlock(const uint8_t* src, uint32_t bytes) {
    constexpr uint64_t kReportBytes = 32000ull * 2 * kBytesPerSample * 15;  // frames * channels
    static uint64_t s_bytes = 0;
    static int s_peak = 0;
    static int s_reports = 0;
    if (s_reports >= 4) {
        return;
    }
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        const int value = static_cast<int16_t>(static_cast<uint16_t>((src[i] << 8) | src[i + 1]));
        s_peak = std::max(s_peak, value < 0 ? -value : value);
    }
    s_bytes += bytes;
    if (s_bytes >= kReportBytes) {
        RT_LOGF(RT_TAG_AUDIO, "[meter] output peak %d of 32767 over the last ~15 s\n", s_peak);
        s_bytes = 0;
        s_peak = 0;
        ++s_reports;
    }
}

// Fast-forward: audio blocks reach the game as fast as its virtual clock runs (ConsumeAudioPollDeltaMicros,
// guest_clock.h), so its streams stay in step with the picture (an in-engine cutscene's music ends with the
// cutscene). The output keeps one stereo frame per `speed` the game actually reaches (the guest time base against
// the host clock, measured over a tenth of a second), which keeps it at real time instead of overflowing or starving
// the queue: the sound plays faster and higher, as in Dolphin's unthrottled mode. The phase carries across blocks so
// the kept frames stay evenly spaced.
double ReachedSpeed(bool restart) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_since = Clock::now();
    static uint64_t s_guestSince = PPC_ReadGuestTimeBase();
    static double s_speed = 1.0;
    const Clock::time_point now = Clock::now();
    if (restart) {  // fast-forward starts: the chosen speed until the first measurement
        s_since = now;
        s_guestSince = PPC_ReadGuestTimeBase();
        s_speed = std::clamp(static_cast<double>(VI_HLE_Speed()), 1.0, 8.0);
        return s_speed;
    }
    const double seconds = std::chrono::duration<double>(now - s_since).count();
    if (seconds >= 0.1) {
        const uint64_t guest = PPC_ReadGuestTimeBase();
        const double guestSeconds =
            guest > s_guestSince ? static_cast<double>(guest - s_guestSince) / TimeBaseContract::kTicksPerSecond : 0.0;
        s_speed = std::clamp(guestSeconds / seconds, 1.0, 64.0);
        s_since = now;
        s_guestSince = guest;
    }
    return s_speed;
}

const uint8_t* DecimateForSpeed(const uint8_t* src, uint32_t& bytes, double speed) {
    static std::vector<uint8_t> s_kept;
    static double s_phase = 0.0;
    constexpr uint32_t kFrameBytes = kAudioChannels * kBytesPerSample;
    const uint32_t frames = bytes / kFrameBytes;
    s_kept.resize(static_cast<size_t>(frames) * kFrameBytes);
    uint32_t kept = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        s_phase += 1.0;
        if (s_phase >= speed) {
            std::memcpy(&s_kept[static_cast<size_t>(kept++) * kFrameBytes], src + f * kFrameBytes, kFrameBytes);
            s_phase -= speed;
        }
    }
    bytes = kept * kFrameBytes;
    return s_kept.data();
}

// OTHERM_AUDIO_DUMP=<file>: every block the game hands the output, raw, as the DMA buffer holds it (32 kHz 16-bit
// stereo, big-endian, right then left), to find glitches offline. vi.cpp's [pause] lines give the byte offset.
std::FILE* AudioDumpFile() {
    static std::FILE* file = [] {
        const char* path = std::getenv("OTHERM_AUDIO_DUMP");
        return path != nullptr && path[0] != '\0' ? std::fopen(path, "wb") : nullptr;
    }();
    return file;
}
uint64_t g_audioDumpedBytes = 0;

bool PushAudioBlock(uint32_t startAddr, uint32_t length) {
    if (startAddr == 0 || length == 0) {
        return false;
    }
    const uint32_t bytes = length;
    const uint8_t* src = nullptr;
    try {
        src = static_cast<const uint8_t*>(Memory::GetPointer(startAddr, bytes));
    } catch (const Memory::AccessViolation&) {
        src = nullptr;
    }

    if (src) {
        if (std::FILE* dump = AudioDumpFile()) {
            std::fwrite(src, 1, bytes, dump);
            std::fflush(dump);
            g_audioDumpedBytes += bytes;
        }
        MeterAudioBlock(src, bytes);
        static bool s_fastForward = false;
        const bool fastForward = VI_HLE_Speed() > 1;
        const bool fastForwardStarts = fastForward && !s_fastForward;
        if (fastForward != s_fastForward) {
            s_fastForward = fastForward;
            AudioBackend::Instance().SetFastForward(fastForward);
            if (AudioDumpFile() != nullptr)
                RT_LOGF(RT_TAG_AUDIO, "[audio] fast-forward %s at audio dump byte %llu\n", fastForward ? "on" : "off",
                        static_cast<unsigned long long>(g_audioDumpedBytes));
        }
        if (fastForward) {
            uint32_t keptBytes = bytes;
            const uint8_t* kept = DecimateForSpeed(src, keptBytes, ReachedSpeed(fastForwardStarts));
            return keptBytes == 0 || AudioBackend::Instance().PushWiiAiSamplesBE16(kept, keptBytes);
        }
        return AudioBackend::Instance().PushWiiAiSamplesBE16(src, bytes);
    }

    const uint32_t sampleCount = bytes / kBytesPerSample;
    if (sampleCount == 0) {
        return false;
    }
    std::vector<int16_t> samples(sampleCount);
    try {
        for (uint32_t i = 0; i < sampleCount; ++i) {
            const uint32_t addr = startAddr + i * kBytesPerSample;
            samples[i] = static_cast<int16_t>(Memory::Read16(addr));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    // Memory::Read16 has converted endianness, but the Wii AI frame order is
    // still right, left. Convert it to the host's left, right convention.
    for (uint32_t i = 0; i + 1 < sampleCount; i += 2) {
        std::swap(samples[i], samples[i + 1]);
    }
    return AudioBackend::Instance().PushSamplesLE16(samples.data(), samples.size());
}

} // namespace

extern "C" void AIClockInit_80109F10(uint32_t clock_mode)
{
    // clock_mode is unused: AID/DSP rate is controlled separately by AI state, and
    // treating it as a sample-rate switch would break Wii AX's normal 32 kHz cadence.
    (void)clock_mode;
    uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        rate = g_ai.sampleRate;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("__AIClockInit", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(80109F10, AIClockInit_80109F10, (uint32_t clock_mode), (clock_mode));

extern "C" void OSInitAudioSystem_8010A130()
{
    AIClockInit_80109F10(1);
    AxDspHle::InitAram();
    AxDspHle::Init();
    if (!EnsureAudioBackend(kDefaultSampleRate)) {
        ReportAudioProblem("__OSInitAudioSystem", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(8010A130, OSInitAudioSystem_8010A130, (), ());

extern "C" void OSStopAudioSystem_8010A300()
{
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.enabled = false;
        g_ai.registerStartAddr = 0;
        g_ai.bytesLeft = 0;
        g_ai.accumulatorSeconds = 0.0;
        g_ai.detBlockDueTicks = 0;
    }
    AxDspHle::Stop();
}

PPC_NATIVE_OVERRIDE_VOID(8010A300, OSStopAudioSystem_8010A300, (), ());



extern "C" void AIInit_800aa130(uint32_t callback_stack_switch)
{
    const uint32_t rate = kDefaultSampleRate;
    uint32_t initialized = 0;
    const bool alreadyInitialized = Memory::TryRead32(kAIInitializedAddr, initialized) && initialized == 1u;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        if (!alreadyInitialized) {
            g_ai.callback = 0;
            g_ai.loggedMissingCallback = false;
            g_ai.loggedAccessFailure = false;
        }
    }
    if (!alreadyInitialized) {
        Memory::TryWrite32(kAIDmaCallbackAddr, 0);
        Memory::TryWrite32(kAICallbackBusyAddr, 0);
        Memory::TryWrite32(kAICallbackStackSwitchAddr, callback_stack_switch);
        Memory::TryWrite32(kAIInitializedAddr, 1);
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIInit", "audio backend init failed");
    } else {
        RT_LOG(RT_TAG_AUDIO) << "AIInit_800aa130 called: Audio subsystem initialized (HLE)" << std::endl;
    }
}

PPC_NATIVE_OVERRIDE_VOID(800aa130, AIInit_800aa130, (uint32_t callback_stack_switch), (callback_stack_switch));



extern "C" void DSPInit_800df6a0()
{
    AxDspHle::Init();
    RT_LOG(RT_TAG_AUDIO) << "DSPInit_800df6a0 called: DSP hardware boundary initialized (HLE)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(800df6a0, DSPInit_800df6a0, (), ());

extern "C" uint32_t DSPCheckInit_800df760()
{
    return AxDspHle::CheckInit();
}
REGISTER_NATIVE_FUNCTION(0x800DF760, DSPCheckInit_800df760);

extern "C" uint32_t DSPAddTask_800df770(uint32_t task_ptr)
{
    return AxDspHle::AddTask(task_ptr);
}
REGISTER_NATIVE_FUNCTION(0x800DF770, DSPAddTask_800df770);

extern "C" void __DSP_boot_task_800dff10(uint32_t task_ptr)
{
    AxDspHle::AssertTask(task_ptr);
    RT_LOG(RT_TAG_AUDIO) << "__DSP_boot_task called: booted DSP task at 0x"
              << std::hex << task_ptr << std::dec << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(800DFF10, __DSP_boot_task_800dff10, (uint32_t task_ptr), (task_ptr));


extern "C" void __AXOutInitDSP_800acd40(CpuContext* ctx)
{
    AxDspHle::InitForAXOut(ctx);
    RT_LOG(RT_TAG_AUDIO) << "__AXOutInitDSP called: native AX/DSP HLE initialized." << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(800acd40, __AXOutInitDSP_800acd40, (CpuContext* ctx), (ctx));



extern "C" void AIInitDMA_800aa020(uint32_t start_addr, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.startAddr = start_addr;
    g_ai.registerStartAddr = EncodeAIDmaStartRegister(start_addr);
    g_ai.length = EncodeAIDmaLengthRegister(length);
    g_ai.bytesLeft = g_ai.length;
    PublishAiRegisters();
}

PPC_NATIVE_OVERRIDE_VOID(800aa020, AIInitDMA_800aa020, (uint32_t start_addr, uint32_t length), (start_addr, length));



// AIRegisterDMACallback stores the callback in the guest global __AID_Callback (0x80332290).
// Returns the old callback pointer.
extern "C" uint32_t AIRegisterDMACallback_800a9fd0(uint32_t callback)
{
    uint32_t old_callback = 0;
    Memory::TryRead32(kAIDmaCallbackAddr, old_callback);
    Memory::TryWrite32(kAIDmaCallbackAddr, callback);
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.callback = callback;
        g_ai.loggedMissingCallback = false;
    }
    return old_callback;
}
PPC_NATIVE_OVERRIDE(800a9fd0, AIRegisterDMACallback_800a9fd0, uint32_t, (uint32_t callback), (callback));

// AIStartDMA toggles the AI DMA control register on hardware. Keep the guest-visible
// DMA state here and let the VI tick advance the hardware boundary.
extern "C" void AIStartDMA_800AA0A0()
{
    const uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        g_ai.enabled = true;
        g_ai.bytesLeft = g_ai.length;
        g_ai.detBlockDueTicks = 0;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIStartDMA", "audio backend init failed");
    }
    PublishAiRegisters();
}

PPC_NATIVE_OVERRIDE_VOID(800AA0A0, AIStartDMA_800AA0A0, (), ());

extern "C" uint32_t AIGetDMABytesLeft_800aa110()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.bytesLeft;
}
PPC_NATIVE_OVERRIDE(800AA110, AIGetDMABytesLeft_800aa110, uint32_t, (), ());

// Other M's sound engine polls the AI DMA registers itself (0xCC00503A, bytes left, read from one of the six
// per-frame sound callbacks under fn_80045F94) instead of calling the SDK, so memory.cpp routes reads of the
// DSP/AI interface block (0xCC005000..0xCC0051FF) and the AI control block (0xCC006C00..0xCC006C3F) here. The
// register encodings follow the SDK: DMA start as hi/lo halves of (addr & 0x1FFFFFE0), DMA control as enable
// bit 15 | length / 32, bytes left as bytes / 32, AICR bit 6 = AIDFR (0 = 48 kHz).
// The AI DMA registers as the guest sees them when it reads the hardware directly: the flat memory's MMIO
// window gets a real page for 0xCC005000 (DSP/AI interface) and 0xCC006000 (AI control at 0xCC006C00), written
// here after every change of the DMA state. Values are big-endian halfwords, the register encodings as below.
namespace {  // the declaration beside g_ai is in this namespace too
void PublishAiRegisters()
{
    static uint8_t* s_dsp = nullptr;
    static uint8_t* s_ai = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        s_dsp = GuestFlat::BackMmioRange(0xCC005000u, 0x1000u);
        s_ai = GuestFlat::BackMmioRange(0xCC006000u, 0x1000u);
        if (s_dsp == nullptr || s_ai == nullptr) {
            RT_LOGF(RT_TAG_AUDIO, "AI registers: no MMIO backing (flat memory inactive); direct reads will fault\n");
        }
    }
    const auto put16 = [](uint8_t* base, uint32_t offset, uint32_t value) {
        if (base == nullptr) return;
        base[offset] = static_cast<uint8_t>(value >> 8);
        base[offset + 1] = static_cast<uint8_t>(value);
    };
    put16(s_dsp, 0x30, (g_ai.registerStartAddr >> 16) & 0x1FFFu);                           // AI_DMA_START_HI
    put16(s_dsp, 0x32, g_ai.registerStartAddr & 0xFFE0u);                                   // AI_DMA_START_LO
    put16(s_dsp, 0x36, (g_ai.enabled ? 0x8000u : 0u) | ((g_ai.length >> 5) & 0x7FFFu));     // AI_DMA_CONTROL
    put16(s_dsp, 0x3A, (g_ai.bytesLeft >> 5) & 0x7FFFu);                                    // AI_DMA_BYTES_LEFT
    put16(s_ai, 0xC00, (g_ai.sampleRate == 48000u) ? 0u : 0x40u);                           // AICR: AIDFR
}
}  // namespace

bool Audio_HLE_IsAiRegister(uint32_t addr) noexcept
{
    return (addr >= 0xCC005000u && addr < 0xCC005200u) || (addr >= 0xCC006C00u && addr < 0xCC006C40u);
}

uint32_t Audio_HLE_ReadAiRegister(uint32_t addr, size_t size) noexcept
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    auto reg16 = [&](uint32_t at) -> uint32_t {
        switch (at) {
        case 0xCC005030u: return (g_ai.registerStartAddr >> 16) & 0x1FFFu;  // AI_DMA_START_HI
        case 0xCC005032u: return g_ai.registerStartAddr & 0xFFE0u;          // AI_DMA_START_LO
        case 0xCC005036u: return (g_ai.enabled ? 0x8000u : 0u) | ((g_ai.length >> 5) & 0x7FFFu);  // AI_DMA_CONTROL
        case 0xCC00503Au: return (g_ai.bytesLeft >> 5) & 0x7FFFu;           // AI_DMA_BYTES_LEFT
        case 0xCC006C00u: return (g_ai.sampleRate == 48000u) ? 0u : 0x40u;  // AICR: AIDFR
        case 0xCC006C02u: return 0u;
        default: {
            static uint32_t s_logged = 0;
            if (s_logged++ < 16) RT_LOGF(RT_TAG_AUDIO, "AI/DSP register read 0x%08X (%zu bytes): answering 0\n", at, size);
            return 0u;
        }
        }
    };
    if (size == 4) return (reg16(addr & ~1u) << 16) | reg16((addr & ~1u) + 2u);
    if (size == 2) return reg16(addr & ~1u);
    const uint32_t half = reg16(addr & ~1u);
    return (addr & 1u) ? (half & 0xFFu) : (half >> 8);
}

extern "C" uint32_t AIGetDSPSampleRate_800aa120()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    // SDK AIGetDSPSampleRate returns AIDFR^1: 0 for 32 kHz, 1 for 48 kHz.
    return (g_ai.sampleRate == 48000u) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(800AA120, AIGetDSPSampleRate_800aa120, uint32_t, (), ());  // Other M: signature match, after AIGetDMABytesLeft

extern "C" void DSPSendMailToDSP_800df680(uint32_t mail)
{
    AxDspHle::SendMailToDSP(mail);
}
PPC_NATIVE_OVERRIDE_VOID(800DF680, DSPSendMailToDSP_800df680, (uint32_t mail), (mail));

extern "C" uint32_t DSPCheckMailToDSP_800df640()
{
    return AxDspHle::CheckMailToDSP();
}
PPC_NATIVE_OVERRIDE(800DF640, DSPCheckMailToDSP_800df640, uint32_t, (), ());

extern "C" uint32_t DSPCheckMailFromDSP_800df650()
{
    return AxDspHle::CheckMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x800DF650, DSPCheckMailFromDSP_800df650);

extern "C" uint32_t DSPReadMailFromDSP_800df660()
{
    return AxDspHle::ReadMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x800DF660, DSPReadMailFromDSP_800df660);

extern "C" uint32_t DSPAssertTask_800df820(uint32_t taskPtr)
{
    return AxDspHle::AssertTask(taskPtr);
}
PPC_NATIVE_OVERRIDE(800DF820, DSPAssertTask_800df820, uint32_t, (uint32_t taskPtr), (taskPtr));

// Each delivered block runs the AI DMA callback and deferred AX task callbacks before the
// next block, preserving the SoundThread/DSP interleave order real hardware provides.
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros)
{
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    bool enabled = false;

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        enabled = g_ai.enabled;
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;

        if (enabled && startAddr != 0 && length != 0 && sampleRate != 0) {
            // SoundThread can hit the idle scheduler before the outer AXOut frame finishes;
            // retain elapsed time here rather than recursively entering the singleton AI/AX device.
            g_ai.accumulatorSeconds += static_cast<double>(deltaMicros) / 1'000'000.0;
            // Fast-forward runs the audio clock at its speed, and a busy scene can leave it hundreds of ms behind
            // (612 ms seen at 8x). At normal speed that backlog would arrive unthinned on top of the output queue;
            // when fast-forward ends it is dropped instead (the guest's audio just doesn't advance for it, as across
            // an Options pause).
            static uint32_t s_speed = 1;
            static double s_most = 0.0;
            s_most = std::max(s_most, g_ai.accumulatorSeconds);
            if (const uint32_t speed = VI_HLE_Speed(); speed != s_speed) {
                static const bool logBacklog = std::getenv("OTHERM_AUDIO_QUEUE_LOG") != nullptr;
                if (logBacklog)
                    RT_LOGF(RT_TAG_AUDIO, "[audio] speed %u -> %u: audio backlog %.0f ms (most %.0f ms)\n", s_speed, speed,
                            g_ai.accumulatorSeconds * 1000.0, s_most * 1000.0);
                if (speed < s_speed) {
                    const double block = static_cast<double>(length) / (static_cast<double>(sampleRate) * kAudioChannels * kBytesPerSample);
                    g_ai.accumulatorSeconds = std::min(g_ai.accumulatorSeconds, block);
                }
                s_speed = speed;
                s_most = 0.0;
            }
            if (g_ai.tickActive) {
                return;
            }
            g_ai.tickActive = true;
        }
    }

    if (!enabled || startAddr == 0 || length == 0 || sampleRate == 0) {
        return;
    }

    // Resets tickActive on early return; the normal exit path disarms this and clears the
    // flag itself while already holding the mutex, avoiding a redundant lock acquisition.
    struct ActiveTickReset {
        bool armed = true;
        ~ActiveTickReset()
        {
            if (!armed) {
                return;
            }
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    const double bytesPerSecond = static_cast<double>(sampleRate) * kAudioChannels * kBytesPerSample;
    const double blockDuration = static_cast<double>(length) / bytesPerSecond;
    if (blockDuration <= 0.0) {
        return;
    }

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);

    int blocksCompleted = 0;
    while (true) {
        // Claim the block in one critical section; sample-then-consume separately gains
        // nothing since the callback below (the only reentrancy point) runs mutex-released.
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (g_ai.accumulatorSeconds < blockDuration) {
                break;
            }
            g_ai.accumulatorSeconds -= blockDuration;
            g_ai.bytesLeft = 0;
        }

        // Everything below reads what the AX mix wrote (PushAudioBlock) or runs guest code
        // that reads its PB write-back and aux buffers (__AXOutNewFrame via the AI DMA
        // callback), so the mix worker must finish first; the join also publishes its
        // aux-out shadow.
        AxDspHle::JoinMixWorker();

        if (!EnsureAudioBackend(sampleRate)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedBackendFailure) {
                g_ai.loggedBackendFailure = true;
                ReportAudioProblem("Audio", "audio backend unavailable; dropping samples");
            }
        } else {
            const bool pushed = PushAudioBlock(startAddr, length);
            if (!pushed) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedAccessFailure) {
                    g_ai.loggedAccessFailure = true;
                    ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
                }
                g_ai.enabled = false;
                return;
            }
        }

        if (callback != 0) {
            // Pointer lookup avoids copying the registry record per audio block.
            const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(callback);
            if (!info || !info->rawCpuInvoker) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedMissingCallback) {
                    g_ai.loggedMissingCallback = true;
                    ReportAudioProblem("Audio", "AI DMA callback not registered; skipping");
                }
            } else {
                Memory::TryWrite32(kAICallbackBusyAddr, 1);
                InvokeIndirectCpu(callback, cpu);
                Memory::TryWrite32(kAICallbackBusyAddr, 0);
                AxDspHle::ServiceDeferredCallbacks();
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            startAddr = g_ai.startAddr;
            length = g_ai.length;
            callback = g_ai.callback;
            sampleRate = g_ai.sampleRate;
            g_ai.bytesLeft = g_ai.length;
        }

        {   // OTHERM_AUDIO_RATE: audio blocks the game received per presented frame, every 2000 blocks (fast-forward
            // keeps this ratio when the audio clock follows the game's).
            static const bool logRate = std::getenv("OTHERM_AUDIO_RATE") != nullptr;
            static uint32_t s_blocks = 0, s_frame0 = 0;
            if (logRate && ++s_blocks == 2000) {
                const uint32_t frame = VI_HLE_PresentedFrames();
                if (s_frame0 != 0 && frame > s_frame0)
                    RT_LOGF(RT_TAG_AUDIO, "[audio] rate: 2000 blocks over %u presented frames (%.2f per frame) at speed %u\n",
                            frame - s_frame0, 2000.0 / (frame - s_frame0), VI_HLE_Speed());
                s_frame0 = frame;
                s_blocks = 0;
            }
        }
        if (++blocksCompleted >= kMaxBlocksPerTick * static_cast<int>(VI_HLE_Speed())) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (g_ai.length != 0) {
            const double bytesRemaining = g_ai.length * (g_ai.accumulatorSeconds / blockDuration);
            if (bytesRemaining < static_cast<double>(g_ai.length)) {
                g_ai.bytesLeft = g_ai.length - static_cast<uint32_t>(bytesRemaining);
            }
        }
        g_ai.tickActive = false;
        activeTickReset.armed = false;
    }
}

namespace {

// Wall-clock delta since the previous poll, from whichever pump ran it. All
// pumps live on the guest thread, so the one thread_local cursor is shared and
// no interval is ever counted twice or dropped between them.
std::atomic<bool> g_dropPausedInterval{false};  // Audio_HLE_SetMenuPause: the time Options was up is not audio time

int64_t ConsumeAudioPollDeltaMicros()
{
    using Clock = std::chrono::steady_clock;
    static thread_local Clock::time_point lastPoll = Clock::now();
    static thread_local uint64_t lastGuest = PPC_ReadGuestTimeBase();

    const Clock::time_point now = Clock::now();
    const uint64_t guest = PPC_ReadGuestTimeBase();
    const uint64_t guestElapsed = guest > lastGuest ? guest - lastGuest : 0;
    lastGuest = guest;
    if (g_dropPausedInterval.exchange(false, std::memory_order_acq_rel)) {
        lastPoll = now;
        return 0;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastPoll).count();
    lastPoll = now;

    if (elapsed < 0) {
        elapsed = 0;
    }

    // Cap the catch-up interval after a debugger pause or host stall; backlog drains via
    // kMaxBlocksPerTick per pass. Never return early on a zero delta: two scheduler passes
    // can land in the same microsecond and the backlog still needs servicing.
    constexpr int64_t kMaxPollDeltaMicros = 100'000;
    // microseconds::rep is long long; int64_t is long on LP64 Linux/Android.
    // Fast-forward: the game's audio clock is its virtual time base, which runs with its retraces (guest_clock.h);
    // see DecimateForSpeed.
    if (GuestClock::FastForwarding()) {
        const auto guestMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                                     TimeBaseContract::TicksToDuration(guestElapsed))
                                     .count();
        return std::min<int64_t>(guestMicros, kMaxPollDeltaMicros * 8);
    }
    return std::min<int64_t>(elapsed, kMaxPollDeltaMicros);
}

} // namespace

namespace {

// One DMA block's length on the virtual timeline: 0x180 bytes of 32 kHz stereo (3 ms) is exactly
// 182250 ticks.
uint64_t DetBlockTicks(uint32_t length, uint32_t sampleRate)
{
    const uint64_t bytesPerSecond = static_cast<uint64_t>(sampleRate) * kAudioChannels * kBytesPerSample;
    return bytesPerSecond == 0 ? 0 : static_cast<uint64_t>(length) * TimeBaseContract::kTicksPerSecond / bytesPerSecond;
}

// Deterministic counterpart of Audio_HLE_Tick: delivers every block whose virtual completion time
// has passed, in the same order (mix join, output push, AI DMA callback, deferred AX callbacks).
void AudioTickDeterministic(CpuContext* ctx)
{
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (!g_ai.enabled || g_ai.startAddr == 0 || g_ai.length == 0 || g_ai.sampleRate == 0 || g_ai.tickActive) {
            return;
        }
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;
        const uint64_t blockTicks = DetBlockTicks(length, sampleRate);
        if (blockTicks == 0) {
            return;
        }
        if (g_ai.detBlockDueTicks == 0) {
            g_ai.detBlockDueTicks = DetClock::Now() + blockTicks;
        }
        if (DetClock::Now() < g_ai.detBlockDueTicks) {
            return;
        }
        g_ai.tickActive = true;
    }

    struct ActiveTickReset {
        ~ActiveTickReset()
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);

    for (int blocksCompleted = 0; blocksCompleted < kMaxBlocksPerTick; ++blocksCompleted) {
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (DetClock::Now() < g_ai.detBlockDueTicks) {
                break;
            }
            g_ai.detBlockDueTicks += DetBlockTicks(length, sampleRate);
            g_ai.bytesLeft = 0;
        }

        AxDspHle::JoinMixWorker();
        if (EnsureAudioBackend(sampleRate) && !PushAudioBlock(startAddr, length)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedAccessFailure) {
                g_ai.loggedAccessFailure = true;
                ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
            }
            g_ai.enabled = false;
            return;
        }

        if (callback != 0) {
            const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(callback);
            if (info && info->rawCpuInvoker) {
                Memory::TryWrite32(kAICallbackBusyAddr, 1);
                InvokeIndirectCpu(callback, cpu);
                Memory::TryWrite32(kAICallbackBusyAddr, 0);
                AxDspHle::ServiceDeferredCallbacks();
            }
        }

        std::lock_guard<std::mutex> lock(g_ai.mutex);
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;
        g_ai.bytesLeft = g_ai.length;
        if (!g_ai.enabled || startAddr == 0 || length == 0 || DetBlockTicks(length, sampleRate) == 0) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_ai.mutex);
    const uint64_t blockTicks = DetBlockTicks(g_ai.length, g_ai.sampleRate);
    const uint64_t now = DetClock::Now();
    if (blockTicks != 0 && g_ai.detBlockDueTicks > now) {
        // The guest polls AIGetDMABytesLeft to size its work; report what is left of this block.
        const uint64_t remaining = std::min<uint64_t>(g_ai.detBlockDueTicks - now, blockTicks);
        g_ai.bytesLeft = static_cast<uint32_t>(remaining * g_ai.length / blockTicks) & ~0x1Fu;
    }
    PublishAiRegisters();
}

} // namespace

uint64_t Audio_HLE_DetNextEventTicks()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    if (!g_ai.enabled || g_ai.startAddr == 0 || g_ai.length == 0 || g_ai.sampleRate == 0) {
        return DetClock::kNever;
    }
    if (g_ai.detBlockDueTicks == 0) {
        return DetClock::Now();  // not armed yet: the next poll arms it
    }
    return g_ai.detBlockDueTicks;
}

void Audio_HLE_Poll(CpuContext* ctx)
{
    if (DetClock::Enabled()) {
        AudioTickDeterministic(ctx);
        return;
    }
    Audio_HLE_Tick(ctx, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
    PublishAiRegisters();
}

uint64_t Audio_HLE_DumpedBytes() { return g_audioDumpedBytes; }

// Options pauses the game (vi.cpp RunPausedFrames). The output device is held, so the audio already queued for it
// (60-100 ms) plays on after the menu, and the time the menu was up is not counted as audio time. Counting it (capped
// at 100 ms) made the resumed AX voices run ahead of CRI's ADX streaming, which refills a 128 ms loop buffer from a
// guest thread: the music then replayed the previous 128 ms (boot tpause2: two pauses, two 128 ms repeats).
void Audio_HLE_SetMenuPause(bool paused)
{
    if (!paused) {
        g_dropPausedInterval.store(true, std::memory_order_release);
    }
    AudioBackend::Instance().SetPaused(paused);
}

void Audio_HLE_PollDeferred()
{
    // A wall-clock pump; deterministic mode delivers audio blocks only from the scheduler.
    if (DetClock::Enabled()) {
        return;
    }
    if (!OS_HLE_InterruptsEnabled()) {
        // Leave the elapsed interval unconsumed so the next poll still sees it.
        return;
    }

    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        Audio_HLE_Tick(cpu, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
    PublishAiRegisters();
}

// Save states: the AI DMA registers and the deterministic block timer.
void AI_HLE_SaveState(SaveState::Writer& out)
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    out.Pod(g_ai.startAddr);
    out.Pod(g_ai.registerStartAddr);
    out.Pod(g_ai.length);
    out.Pod(g_ai.callback);
    out.Pod(g_ai.enabled);
    out.Pod(g_ai.sampleRate);
    out.Pod(g_ai.bytesLeft);
    out.Pod(g_ai.accumulatorSeconds);
    out.Pod(g_ai.detBlockDueTicks);
    out.Pod(g_ai.tickActive);
}

void AI_HLE_LoadState(SaveState::Reader& in)
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    in.Pod(g_ai.startAddr);
    in.Pod(g_ai.registerStartAddr);
    in.Pod(g_ai.length);
    in.Pod(g_ai.callback);
    in.Pod(g_ai.enabled);
    in.Pod(g_ai.sampleRate);
    in.Pod(g_ai.bytesLeft);
    in.Pod(g_ai.accumulatorSeconds);
    in.Pod(g_ai.detBlockDueTicks);
    in.Pod(g_ai.tickActive);
}
