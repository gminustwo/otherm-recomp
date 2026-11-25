#include "settings_overlay.h"
#include "save_state.h"
#include "frame_input.h"
#include "hle_stubs.h"
#include "input_bindings.h"
#include "memory.h"
#include "wii_remote_input.h"
#include "runtime_log.h"

#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// KPAD HLE fed by a real Bluetooth Wii Remote. The game calls KPADRead once per
// frame with room for 16 KPADStatus entries and only looks at entry 0; with a
// Classic Controller it also calls KPADGetUnifiedWpadStatus for the raw
// WPADCLStatus (buttons, sticks and triggers of the extension).
namespace {

constexpr uint32_t kKpadStatusSize = 0x84;

// KPADStatus field offsets (RVL SDK).
constexpr uint32_t kHold = 0x00, kTrig = 0x04, kRelease = 0x08, kAcc = 0x0C, kAccValue = 0x18,
                   kAccSpeed = 0x1C, kPos = 0x20, kAccVertical = 0x54, kDevType = 0x5C, kWpadErr = 0x5D,
                   kDpdValidFg = 0x5E, kDataFormat = 0x5F, kFsStick = 0x60, kFsAcc = 0x68, kFsAccValue = 0x74,
                   kFsAccSpeed = 0x78;
// KPADStatus.ex_status.cl (KPADEXStatus, Classic Controller view).
constexpr uint32_t kClHold = 0x60, kClTrig = 0x64, kClRelease = 0x68, kClLStick = 0x6C, kClRStick = 0x74,
                   kClLTrigger = 0x7C, kClRTrigger = 0x80;

// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsStickX = 0x2A, kUFsStickY = 0x2B, kUFsAccX = 0x2C, kUFsAccY = 0x2E,
                   kUFsAccZ = 0x30, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
                   kUClRStickY = 0x32, kUClTriggerL = 0x34, kUClTriggerR = 0x35, kUFmt = 0x36;

// WPAD device types (WPAD_DEV_*) and the data formats KPAD runs each of them
// in (WPAD_FMT_*_ACC_DPD): the values KPADStatus.dev_type / data_format and
// KPADUnifiedWpadStatus.dev / fmt carry on the console.
constexpr uint8_t kDevCore = 0;
constexpr uint8_t kDevFreestyle = 1;
constexpr uint8_t kDevClassic = 2;
constexpr uint8_t kFmtCoreAccDpd = 2;
constexpr uint8_t kFmtFreestyleAccDpd = 5;
constexpr uint8_t kFmtClassicAccDpd = 8;
constexpr int8_t kWpadErrNone = 0;
constexpr int8_t kWpadErrNoController = -1;

// Raw accelerometer as WPADStatus carries it: 10 bits, 0x200 at 0 g, 100 per g.
constexpr float kRawAccZero = 512.0f;
constexpr float kRawAccPerG = 100.0f;

struct ChannelState {
    uint32_t prevHold = 0;
    float repeatLeft = 0.0f;  // seconds to the next KPAD_BUTTON_RPT pulse while the same buttons stay held
    uint32_t prevClHold = 0;
    float prevAcc[3] = {0.0f, -1.0f, 0.0f};
    float prevFsAcc[3] = {0.0f, -1.0f, 0.0f};
    float accVertical[2] = {1.0f, 0.0f};  // KPADStatus.acc_vertical, level at rest
};

// KPAD runs its accelerometer filters once per report from the remote, about 200 a second, so
// one 60 Hz read stands for about three of them.
constexpr int kReportsPerRead = 3;

// KPAD's calc_acc_vertical: the remote's attitude in the vertical plane,
// (|acc.xy|, -acc.z) / |acc|, eased toward each report with weight kp_acc_horizon_pw (0.05) times
// w^2, where w is |acc| folded around 1 g; readings of 2 g or more (swings) are ignored. Kept at
// unit length.
void UpdateAccVertical(ChannelState& state, const float* acc) {
    constexpr float kHorizonPower = 0.05f;
    const float xy2 = acc[0] * acc[0] + acc[1] * acc[1];
    const float length = std::sqrt(xy2 + acc[2] * acc[2]);
    if (length == 0.0f || length >= 2.0f) {
        return;
    }
    const float targetX = std::sqrt(xy2) / length;
    const float targetY = -acc[2] / length;
    const float weight = length > 1.0f ? 2.0f - length : length;
    const float k = weight * weight * kHorizonPower;
    for (int report = 0; report < kReportsPerRead; ++report) {
        const float x = state.accVertical[0] + k * (targetX - state.accVertical[0]);
        const float y = state.accVertical[1] + k * (targetY - state.accVertical[1]);
        const float norm = std::sqrt(x * x + y * y);
        if (norm == 0.0f) {
            return;
        }
        state.accVertical[0] = x / norm;
        state.accVertical[1] = y / norm;
    }
}

std::array<ChannelState, 4> g_channels{};

// Euclidean length of a 3-vector.
float Length(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Euclidean distance between two 3-vectors.
float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length(d);
}

// Writes three big-endian floats to guest memory.
void WriteVec3(uint32_t addr, const float* v) {
    Memory::WriteFloat32(addr, v[0]);
    Memory::WriteFloat32(addr + 4, v[1]);
    Memory::WriteFloat32(addr + 8, v[2]);
}

// Zeroes `count` consecutive floats in guest memory.
void WriteZeroFloats(uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Memory::WriteFloat32(addr + i * 4, 0.0f);
    }
}

// The real KPADRead also keeps the latest buttons at the start of its own work for the channel (kpadWork[chan] at
// 0x802DBEC0 + chan * 0x688: hold, trig, release). The game reads its own copy, but Gecko codes that act on a button
// ("button activators", Other M Redux's third-person missiles and quick title read the halfword at 0x802DBEC2) read
// this one, so it gets the same values.
void MirrorButtonsToWork(uint32_t chan, uint32_t hold, uint32_t trig, uint32_t release) {
    constexpr uint32_t kKpadWorkBase = 0x802DBEC0u, kKpadWorkSize = 0x688u;
    const uint32_t work = kKpadWorkBase + chan * kKpadWorkSize;
    if (chan >= 4 || !Memory::Contains(work, 12)) return;
    Memory::Write32(work + 0, hold);
    Memory::Write32(work + 4, trig);
    Memory::Write32(work + 8, release);
}

// Fills one KPADStatus at `addr` and returns the number of valid entries (1),
// or writes an "unplugged" status and returns 0.
int32_t WriteStatus(uint32_t chan, uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    ChannelState& state = g_channels[chan];
    if (sample == nullptr) {
        state = {};
        Memory::Write32(addr + kHold, 0);
        Memory::Write32(addr + kTrig, 0);
        Memory::Write32(addr + kRelease, 0);
        MirrorButtonsToWork(chan, 0, 0, 0);
        Memory::Write8(addr + kDevType, kDevCore);
        Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kDpdValidFg, 0);
        return 0;
    }

    const uint32_t hold = sample->hold;
    // KPAD button repeat as KPADSetBtnRepeat(chan, 0.375 s, 0.125 s) sets it (unverified for Other M): menus can read
    // directions only while KPAD_BUTTON_RPT (bit 31 of hold) is set, which KPAD sets on a new press and then every
    // pulse. shortcut: the game's delay/pulse are fixed here (one KPADRead per 60 Hz frame); hook KPADSetBtnRepeat
    // if the game sets other values.
    constexpr uint32_t kButtonRpt = 0x80000000u;
    constexpr float kRepeatDelay = 0.375f, kRepeatPulse = 0.125f, kReadPeriod = 1.0f / 60.0f;
    bool repeat = false;
    if ((hold & ~state.prevHold) != 0) {
        repeat = true;
        state.repeatLeft = kRepeatDelay;
    } else if (hold != 0) {
        state.repeatLeft -= kReadPeriod;
        if (state.repeatLeft <= 0.0f) {
            repeat = true;
            state.repeatLeft += kRepeatPulse;
        }
    }
    {   // OTHERM_LOG_KPAD: every hold/trig change the game is handed (input debugging)
        static const bool logKpad = std::getenv("OTHERM_LOG_KPAD") != nullptr;
        if (logKpad && (hold != state.prevHold || repeat))
            RT_LOGF(RT_TAG_RUNTIME, "[kpad] ch%u hold %08X trig %08X rpt %d\n", chan, hold | (repeat ? kButtonRpt : 0u),
                    hold & ~state.prevHold, repeat ? 1 : 0);
    }
    Memory::Write32(addr + kHold, hold | (repeat ? kButtonRpt : 0u));
    Memory::Write32(addr + kTrig, hold & ~state.prevHold);
    Memory::Write32(addr + kRelease, state.prevHold & ~hold);
    MirrorButtonsToWork(chan, hold | (repeat ? kButtonRpt : 0u), hold & ~state.prevHold, state.prevHold & ~hold);
    state.prevHold = hold;

    WriteVec3(addr + kAcc, sample->acc);
    Memory::WriteFloat32(addr + kAccValue, Length(sample->acc));
    Memory::WriteFloat32(addr + kAccSpeed, Distance(sample->acc, state.prevAcc));
    for (int i = 0; i < 3; ++i) state.prevAcc[i] = sample->acc[i];

    // No IR pointer: pos .. dist_speed zeroed and dpd_valid_fg clear, which
    // the game treats as "pointing away from the screen". acc_vertical comes
    // from the accelerometer alone, as on the console.
    WriteZeroFloats(addr + kPos, (kAccVertical - kPos) / 4);
    UpdateAccVertical(state, sample->acc);
    Memory::WriteFloat32(addr + kAccVertical, state.accVertical[0]);
    Memory::WriteFloat32(addr + kAccVertical + 4, state.accVertical[1]);
    Memory::Write8(addr + kDpdValidFg, 0);
    if (sample->pointerValid) {  // emulated pointer: pos, a level remote (horizon 1,0) about 1 m away, both marks seen
        Memory::WriteFloat32(addr + kPos, sample->pointer[0]);
        Memory::WriteFloat32(addr + kPos + 4, sample->pointer[1]);
        Memory::WriteFloat32(addr + 0x34, 1.0f);   // horizon.x
        Memory::WriteFloat32(addr + 0x48, 1.0f);   // dist
        Memory::Write8(addr + kDpdValidFg, 2);
    }

    const uint8_t devType = sample->hasClassic ? kDevClassic : sample->hasNunchuk ? kDevFreestyle : kDevCore;
    const uint8_t dataFormat =
        sample->hasClassic ? kFmtClassicAccDpd : sample->hasNunchuk ? kFmtFreestyleAccDpd : kFmtCoreAccDpd;
    Memory::Write8(addr + kDevType, devType);
    Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNone));
    Memory::Write8(addr + kDataFormat, dataFormat);

    if (sample->hasClassic) {
        const uint32_t clHold = sample->clHold;
        Memory::Write32(addr + kClHold, clHold);
        Memory::Write32(addr + kClTrig, clHold & ~state.prevClHold);
        Memory::Write32(addr + kClRelease, state.prevClHold & ~clHold);
        state.prevClHold = clHold;
        Memory::WriteFloat32(addr + kClLStick, sample->clLStick[0]);
        Memory::WriteFloat32(addr + kClLStick + 4, sample->clLStick[1]);
        Memory::WriteFloat32(addr + kClRStick, sample->clRStick[0]);
        Memory::WriteFloat32(addr + kClRStick + 4, sample->clRStick[1]);
        Memory::WriteFloat32(addr + kClLTrigger, sample->clTriggerL / 255.0f);
        Memory::WriteFloat32(addr + kClRTrigger, sample->clTriggerR / 255.0f);
    } else if (sample->hasNunchuk) {
        state.prevClHold = 0;
        Memory::WriteFloat32(addr + kFsStick, sample->stick[0]);
        Memory::WriteFloat32(addr + kFsStick + 4, sample->stick[1]);
        WriteVec3(addr + kFsAcc, sample->nunchukAcc);
        Memory::WriteFloat32(addr + kFsAccValue, Length(sample->nunchukAcc));
        Memory::WriteFloat32(addr + kFsAccSpeed, Distance(sample->nunchukAcc, state.prevFsAcc));
        for (int i = 0; i < 3; ++i) state.prevFsAcc[i] = sample->nunchukAcc[i];
    } else {
        state.prevClHold = 0;
        WriteZeroFloats(addr + kFsStick, (kKpadStatusSize - kFsStick) / 4);
    }
    return 1;
}

// Neutral sticks and no buttons while an overlay owns input; the remote stays connected.
bool ReadSample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (FrameInput::Active()) {
        // Deterministic mode: the frame's latched state (already neutral while an overlay owns input).
        const FrameInput::Remote& remote = FrameInput::Get(chan);
        if (!remote.connected) {
            return false;
        }
        sample = remote.sample;
        return true;
    }
    if (!WiiRemoteInput::ReadKpadSample(chan, sample)) {
        return false;
    }
    settings_overlay::FilterMenuInput(chan, sample);
    // A or 2 during the startup cover (settings_overlay) also starts its lift; input is blocked while it is up.
    constexpr uint32_t kWpadA = 0x0800u, kWpadTwo = 0x0100u;
    if (settings_overlay::StartupScreenVisible() && (sample.hold & (kWpadA | kWpadTwo)) != 0) {
        settings_overlay::NotifyStrapInputAccepted();
    }
    if (InputBindings::InputBlocked()) {
        sample.hold = 0;
        sample.clHold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clLStick[0] = sample.clLStick[1] = 0.0f;
        sample.clRStick[0] = sample.clRStick[1] = 0.0f;
        sample.clLStickRaw[0] = sample.clLStickRaw[1] = 0;
        sample.clRStickRaw[0] = sample.clRStickRaw[1] = 0;
        sample.clTriggerL = sample.clTriggerR = 0;
    }
    return true;
}

} // namespace

// Save states: KPAD's per-channel filters and button-repeat timers.
void KPAD_HLE_SaveState(SaveState::Writer& out) { out.Pod(g_channels); }
void KPAD_HLE_LoadState(SaveState::Reader& in) { in.Pod(g_channels); }

// KPADRead: fills KPADStatus[0] for `chan` from the Bluetooth remote, returns the entry count.
extern "C" int32_t KPAD__Read_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    g_gxBringupCounters[5].fetch_add(1, std::memory_order_relaxed);
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        return WriteStatus(chan, statusPtr, have ? &sample : nullptr);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
}
// Other M: KPADRead (0x800FA350) is "li r6,0; b KPADReadEx"; KPADReadEx (0x800FA360, 0x810 bytes, right before
// KPADInit) takes (chan, status, count, err), checks WPADGetStatus() == 3 and the KPAD init flag at r13-0x73BC.
PPC_NATIVE_OVERRIDE(800FA350, KPAD__Read_HLE, int32_t, (uint32_t chan, uint32_t statusPtr, uint32_t count),
         (chan, statusPtr, count));

// KPADReadEx: KPADRead plus an optional s32* error out-parameter. KPADRead (0x800FA350) is a thin wrapper
// that branches here, and the game reads the remote through this one (NOTES.md 2026-11-05), so it needs
// the same native or the remote would read as disconnected.
// Other M (KPAD of RVL_SDK 2009): the per-channel work is kpadWork[chan] at 0x802DBEC0 + chan * 0x688, with the
// application's connect callback at +0x63C (KPADSetConnectCallback, 0x800FB1F0: the game installs 0x8013FB60,
// which takes (chan, reason), sets its own per-channel "connected" flag for reason 0 and clears it for -1) and the
// sampling callback at +0x638 (KPADSetSamplingCallback, 0x800FBE80; the game passes none). On a console WPAD's
// connection report reaches that callback through KPAD; here each channel the virtual remote presents is
// announced once as connected (reason 0) and once as gone (reason -1) when it disappears. The game never polls
// KPADRead for an unconnected channel (boot 17: 0 calls in 2,400 frames), so without this it sees no controller.
void KPAD_HLE_FireSamplingCallbacks(CpuContext* ctx)
{
    constexpr uint32_t kKpadWork = 0x802DBEC0u, kKpadWorkStride = 0x688u, kConnectCallbackOffset = 0x63Cu;
    static bool s_announced[4] = {};
    if (ctx == nullptr) return;
    for (uint32_t chan = 0; chan < 4; ++chan) {
        const bool present = WiiRemoteInput::EffectiveKind(chan) != WiiRemoteInput::Kind::NotWii;
        if (present == s_announced[chan]) continue;
        uint32_t callback = 0;
        if (!Memory::TryRead32(kKpadWork + chan * kKpadWorkStride + kConnectCallbackOffset, callback) || callback == 0) continue;
        s_announced[chan] = present;
        RT_LOGF(RT_TAG_RUNTIME, "[kpad] connect callback 0x%08X: channel %u %s\n", callback, chan, present ? "connected" : "disconnected");
        ctx->gpr[3] = chan;
        ctx->gpr[4] = present ? 0u : 0xFFFFFFFFu;
        InvokeIndirectCpu(callback, ctx);
    }
}

extern "C" int32_t KPAD__ReadEx_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count, uint32_t errPtr)
{
    g_gxBringupCounters[5].fetch_add(1, std::memory_order_relaxed);
    constexpr int32_t kKpadReadErrNone = 0;
    constexpr int32_t kKpadReadErrNoController = -2;
    const int32_t read = KPAD__Read_HLE(chan, statusPtr, count);
    if (errPtr != 0) {
        try {
            Memory::Write32(errPtr, static_cast<uint32_t>(read > 0 ? kKpadReadErrNone : kKpadReadErrNoController));
        } catch (const Memory::AccessViolation&) {
        }
    }
    return read;
}
PPC_NATIVE_OVERRIDE(800FA360, KPAD__ReadEx_HLE, int32_t,
         (uint32_t chan, uint32_t statusPtr, uint32_t count, uint32_t errPtr), (chan, statusPtr, count, errPtr));

