#include "abi_bridge.h"
#include "isa/big_endian.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>


#if defined(__clang__)
// PowerPC uses discrete fmuls/fadds; a fused multiply-add would change sample rounding.
#pragma clang fp contract(off)
#endif

namespace {
namespace ReverbStd {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;

// .sdata2 constants the guest function loads through r2.
constexpr uint32_t kOneConstantAddr = 0x803334C0u;   // 1.0f (.sdata2, loaded by AXFXReverbStdExpCallback)
constexpr uint32_t kScaleConstantAddr = 0x803334C4u; // 0.6f send pre-scale

// AXFX_REVERBSTD_EXP field offsets (byte offsets into the struct in r4).
constexpr uint32_t kFieldPreDelayCoef = 0x18;
constexpr uint32_t kFieldEarlyLength = 0x2C;
constexpr uint32_t kFieldComb1Coef = 0x64;
constexpr uint32_t kFieldComb2Coef = 0x68;
constexpr uint32_t kFieldAllpassCoef = 0x9C;
constexpr uint32_t kFieldLastAllpass = 0xA0; // + channel * 4
constexpr uint32_t kFieldDamping = 0xAC;
constexpr uint32_t kFieldFlags = 0xB0;
constexpr uint32_t kFieldDryPreScale = 0xD0;
constexpr uint32_t kFieldWetPreScale = 0xD4;
constexpr uint32_t kFieldAuxInputBuffers = 0xD8;
constexpr uint32_t kFieldAuxOutputBuffers = 0xDC;
constexpr uint32_t kFieldMainOutGain = 0xE0;
constexpr uint32_t kFieldAuxOutGain = 0xE4;
constexpr uint32_t kStateStructBytes = 0xE8;

enum RingId : uint32_t {
    kRingPreDelay = 0,
    kRingEarly,
    kRingComb1,
    kRingComb2,
    kRingAllpass1,
    kRingAllpass2,
    kRingCount,
};

struct RingLayout {
    uint32_t bufferField;   // Channel 0 buffer pointer.
    uint32_t channelStride; // Byte stride between channel buffer pointers.
    uint32_t indexField;
    uint32_t lengthField;
};

constexpr RingLayout kRingLayout[kRingCount] = {
    {0x00, 4, 0x0C, 0x10}, // Pre-delay comb.
    {0x1C, 4, 0x28, 0x2C}, // Early reflection tap (optional).
    {0x34, 8, 0x4C, 0x54}, // Comb 1 (per-channel pointers interleave with comb 2).
    {0x38, 8, 0x50, 0x58}, // Comb 2.
    {0x6C, 8, 0x84, 0x8C}, // Allpass 1 (interleaves with allpass 2).
    {0x70, 8, 0x88, 0x90}, // Allpass 2.
};

struct Frame {
    uint8_t* ring[kRingCount][kChannels]{};
    uint32_t ringIndex[kRingCount]{};
    uint32_t ringLength[kRingCount]{};
    uint8_t* main[kChannels]{};
    const uint8_t* auxIn[kChannels]{};
    uint8_t* auxOut[kChannels]{};
    float lastAllpass[kChannels]{};
    float preDelayCoef = 0.0f;
    float comb1Coef = 0.0f;
    float comb2Coef = 0.0f;
    float allpassCoef = 0.0f;
    float damping = 0.0f;
    float oneMinusDamping = 0.0f;
    float dryScale = 0.0f;
    float wetScale = 0.0f;
    float mainGain = 0.0f;
    float auxGain = 0.0f;
    bool hasEarly = false;
    bool hasAuxIn = false;
    bool hasAuxOut = false;
};

inline float LoadFloat(const uint8_t* host) {
    return BigEndian::ReadFloat32(host);
}

inline void StoreFloat(uint8_t* host, float value) {
    BigEndian::WriteFloat32(host, value);
}

inline int32_t LoadS32(const uint8_t* host) {
    return static_cast<int32_t>(BigEndian::Read32(host));
}

inline void StoreS32(uint8_t* host, int32_t value) {
    BigEndian::Write32(host, static_cast<uint32_t>(value));
}

// PowerPC fctiwz: round toward zero, saturating out-of-range and NaN exactly the
// way runtime/src/fpu_helpers.cpp does for the translated form.
inline int32_t ConvertToIntegerWord(float value) {
    const double wide = static_cast<double>(value);
    if (std::isnan(wide)) {
        return static_cast<int32_t>(0x80000000u);
    }
    if (wide >= 2147483647.0) {
        return 2147483647;
    }
    if (wide <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000u);
    }
    return static_cast<int32_t>(wide);
}

void Render(uint32_t stateAddr, Frame& frame) {
    uint32_t index[kRingCount];
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        index[ring] = frame.ringIndex[ring];
    }

    for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
        const uint32_t preDelayOffset = index[kRingPreDelay] * 4u;
        const uint32_t earlyOffset = index[kRingEarly] * 4u;
        const uint32_t comb1Offset = index[kRingComb1] * 4u;
        const uint32_t comb2Offset = index[kRingComb2] * 4u;
        const uint32_t allpass1Offset = index[kRingAllpass1] * 4u;
        const uint32_t allpass2Offset = index[kRingAllpass2] * 4u;
        const uint32_t frameOffset = sample * 4u;

        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            uint8_t* const mainSlot = frame.main[channel] + frameOffset;
            int32_t rawInput = LoadS32(mainSlot);
            if (frame.hasAuxIn) {
                rawInput = static_cast<int32_t>(
                    static_cast<uint32_t>(rawInput) +
                    static_cast<uint32_t>(LoadS32(frame.auxIn[channel] + frameOffset)));
            }
            const float input = static_cast<float>(rawInput);

            // Pre-delay comb: the tap that leaves the buffer also feeds the dry
            // (early) send. Every product is its own statement so the host
            // compiler cannot fuse a multiply into the following add.
            uint8_t* const preDelaySlot = frame.ring[kRingPreDelay][channel] + preDelayOffset;
            const float preDelayTap = LoadFloat(preDelaySlot);
            const float preDelayFeedback = preDelayTap * frame.preDelayCoef;
            StoreFloat(preDelaySlot, input + preDelayFeedback);

            float excite = input;
            if (frame.hasEarly) {
                uint8_t* const earlySlot = frame.ring[kRingEarly][channel] + earlyOffset;
                excite = LoadFloat(earlySlot);
                StoreFloat(earlySlot, input);
            }

            const float dry = preDelayTap * frame.dryScale;

            uint8_t* const comb1Slot = frame.ring[kRingComb1][channel] + comb1Offset;
            const float comb1Tap = LoadFloat(comb1Slot);
            const float comb1Feedback = comb1Tap * frame.comb1Coef;
            StoreFloat(comb1Slot, excite + comb1Feedback);

            uint8_t* const comb2Slot = frame.ring[kRingComb2][channel] + comb2Offset;
            const float comb2Tap = LoadFloat(comb2Slot);
            const float comb2Feedback = comb2Tap * frame.comb2Coef;
            const float combSum = comb1Tap + comb2Tap;
            StoreFloat(comb2Slot, excite + comb2Feedback);

            uint8_t* const allpass1Slot = frame.ring[kRingAllpass1][channel] + allpass1Offset;
            const float allpass1Tap = LoadFloat(allpass1Slot);
            const float allpass1Feedback = allpass1Tap * frame.allpassCoef;
            const float allpass1Store = combSum + allpass1Feedback;
            StoreFloat(allpass1Slot, allpass1Store);
            const float allpass1Feedforward = allpass1Store * frame.allpassCoef;
            const float allpass1Out = allpass1Tap - allpass1Feedforward;

            const float dampedNew = frame.oneMinusDamping * allpass1Out;
            const float dampedOld = frame.damping * frame.lastAllpass[channel];
            const float damped = dampedNew + dampedOld;
            frame.lastAllpass[channel] = damped;

            uint8_t* const allpass2Slot = frame.ring[kRingAllpass2][channel] + allpass2Offset;
            const float allpass2Tap = LoadFloat(allpass2Slot);
            const float allpass2Feedback = allpass2Tap * frame.allpassCoef;
            const float allpass2Store = damped + allpass2Feedback;
            StoreFloat(allpass2Slot, allpass2Store);
            const float allpass2Feedforward = allpass2Store * frame.allpassCoef;
            const float allpass2Out = allpass2Tap - allpass2Feedforward;

            const float wet = allpass2Out * frame.wetScale;
            const float mixed = dry + wet;
            const float mainSample = mixed * frame.mainGain;
            StoreS32(mainSlot, ConvertToIntegerWord(mainSample));
            if (frame.hasAuxOut) {
                const float auxSample = mixed * frame.auxGain;
                StoreS32(frame.auxOut[channel] + frameOffset, ConvertToIntegerWord(auxSample));
            }
        }

        for (uint32_t ring = 0; ring < kRingCount; ++ring) {
            if (ring == kRingEarly && !frame.hasEarly) {
                continue;
            }
            const uint32_t next = index[ring] + 1u;
            index[ring] = next < frame.ringLength[ring] ? next : 0u;
        }
    }

    // The guest writes these back every sample; nothing can observe the
    // intermediate values, so one store per field at the end is equivalent.
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        Memory::Write32(stateAddr + kRingLayout[ring].indexField, index[ring]);
    }
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        Memory::WriteFloat32(stateAddr + kFieldLastAllpass + channel * 4,
                             static_cast<double>(frame.lastAllpass[channel]));
    }
}

} // namespace ReverbStd
} // namespace

