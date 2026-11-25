#include "save_state.h"
#include <optional>
// OSMessageQueue HLE.

#include <cstdint>
#include <iostream>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"

// ============================================================================
// OSMessageQueue HLE
// ============================================================================
extern "C" void OS__InitMessageQueue_HLE_801103a0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t queuePtr = cpu->gpr[3];
    const uint32_t msgArrayPtr = cpu->gpr[4];
    const uint32_t msgCount = cpu->gpr[5];

    if (queuePtr == 0) {
        return;
    }

    try {
        // Clear send/recv thread queues
        ::Memory::Write32(queuePtr + kMsgQueueSendOffset + 0u, 0);
        ::Memory::Write32(queuePtr + kMsgQueueSendOffset + 4u, 0);
        ::Memory::Write32(queuePtr + kMsgQueueRecvOffset + 0u, 0);
        ::Memory::Write32(queuePtr + kMsgQueueRecvOffset + 4u, 0);

        ::Memory::Write32(queuePtr + kMsgQueueArrayOffset, msgArrayPtr);
        ::Memory::Write32(queuePtr + kMsgQueueCountOffset, msgCount);
        ::Memory::Write32(queuePtr + kMsgQueueFirstOffset, 0);
        ::Memory::Write32(queuePtr + kMsgQueueUsedOffset, 0);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__InitMessageQueue", e);
    }
}
REGISTER_NATIVE_FUNCTION(0x801103A0, OS__InitMessageQueue_HLE_801103a0);

static bool MsgQueueIsFull(uint32_t queuePtr)
{
    const uint32_t used = ::Memory::Read32(queuePtr + kMsgQueueUsedOffset);
    const uint32_t count = ::Memory::Read32(queuePtr + kMsgQueueCountOffset);
    return count != 0 && used >= count;
}

static bool MsgQueueIsEmpty(uint32_t queuePtr)
{
    const uint32_t used = ::Memory::Read32(queuePtr + kMsgQueueUsedOffset);
    return used == 0;
}

static void MsgQueueEnqueue(uint32_t queuePtr, uint32_t msg)
{
    const uint32_t arrayPtr = ::Memory::Read32(queuePtr + kMsgQueueArrayOffset);
    const uint32_t count = ::Memory::Read32(queuePtr + kMsgQueueCountOffset);
    uint32_t first = ::Memory::Read32(queuePtr + kMsgQueueFirstOffset);
    uint32_t used = ::Memory::Read32(queuePtr + kMsgQueueUsedOffset);

    if (count == 0) {
        return;
    }

    const uint32_t index = (first + used) % count;
    ::Memory::Write32(arrayPtr + index * 4u, msg);
    ::Memory::Write32(queuePtr + kMsgQueueUsedOffset, used + 1u);
}

static uint32_t MsgQueueDequeue(uint32_t queuePtr)
{
    const uint32_t arrayPtr = ::Memory::Read32(queuePtr + kMsgQueueArrayOffset);
    const uint32_t count = ::Memory::Read32(queuePtr + kMsgQueueCountOffset);
    uint32_t first = ::Memory::Read32(queuePtr + kMsgQueueFirstOffset);
    uint32_t used = ::Memory::Read32(queuePtr + kMsgQueueUsedOffset);

    if (count == 0 || used == 0) {
        return 0;
    }

    const uint32_t msg = ::Memory::Read32(arrayPtr + first * 4u);
    first = (first + 1u) % count;
    ::Memory::Write32(queuePtr + kMsgQueueFirstOffset, first);
    ::Memory::Write32(queuePtr + kMsgQueueUsedOffset, used - 1u);
    return msg;
}

namespace {
// Shared blocking-loop body for OSSendMessage/OSJamMessage/OSReceiveMessage: with interrupts
// off, wait for the queue shape the op needs, apply it, wake the opposite queue, and return 1,
// or return 0 immediately if non-blocking. Only the predicate/action/offsets differ per caller.
template <typename Ready, typename Apply>
int32_t MsgQueueOp(CpuContext* cpu, const char* who, uint32_t queuePtr, bool block,
                   Ready ready, Apply apply, uint32_t wakeOffset, uint32_t blockOffset,
                   const CpuContext* recordEntry = nullptr, uint32_t recordExtra = 0,
                   const SaveState::YieldRecord* resumed = nullptr, bool resumedRunning = false)
{
    const int32_t irqState = resumed ? static_cast<int32_t>(resumed->extra[3]) : OS__DisableInterrupts_8010f160();
    // Save states: a recorded blocking op (OSReceiveMessage) continues after its sleep when restored.
    std::optional<SaveState::YieldScope> yieldScope;
    if (resumed) {
        yieldScope.emplace(*resumed);
        OS_HLE_SleepResume(cpu, resumedRunning, 0);  // the sleep ran with interrupts already off
    } else if (recordEntry && block) {
        yieldScope.emplace(SaveState::YieldKind::MessageQueue, *recordEntry,
                           std::initializer_list<uint32_t>{queuePtr, recordExtra, recordEntry->gpr[5], static_cast<uint32_t>(irqState)});
    }

    while (true) {
        try {
            if (ready(queuePtr)) {
                apply(queuePtr);
                cpu->gpr[3] = queuePtr + wakeOffset;
                OSWakeupThread_HLE_801139d0(cpu);
                cpu->gpr[3] = 1;
                OS__RestoreInterrupts_8010f1a0(irqState);
                return 1;
            }
        } catch (const ::Memory::AccessViolation& e) {
            LogMemoryError(RT_TAG_OS, who, e);
            cpu->gpr[3] = 0;
            OS__RestoreInterrupts_8010f1a0(irqState);
            return 0;
        }

        if (!block) {
            cpu->gpr[3] = 0;
            OS__RestoreInterrupts_8010f1a0(irqState);
            return 0;
        }

        cpu->gpr[3] = queuePtr + blockOffset;
        OSSleepThread_HLE_801138e0(cpu);
    }
}
} // namespace

extern "C" int32_t OS__SendMessage_HLE_80110400(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t queuePtr = cpu->gpr[3];
    const uint32_t msg = cpu->gpr[4];
    const uint32_t flags = cpu->gpr[5];
    const bool block = (flags & 1u) != 0;

    if (queuePtr == 0) {
        cpu->gpr[3] = 0;
        return 0;
    }

    // Append, then wake receivers blocked on the receive queue (offset 0x08);
    // a full queue parks this thread on the send queue.
    return MsgQueueOp(
        cpu, "OS__SendMessage", queuePtr, block,
        [](uint32_t queue) { return !MsgQueueIsFull(queue); },
        [msg](uint32_t queue) { MsgQueueEnqueue(queue, msg); },
        kMsgQueueRecvOffset, kMsgQueueSendOffset);
}
REGISTER_NATIVE_FUNCTION(0x80110400, OS__SendMessage_HLE_80110400);

extern "C" int32_t OS__ReceiveMessage_HLE_801104d0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    bool resumedRunning = false;
    const SaveState::YieldRecord* resumed = SaveState::TakeYield(SaveState::YieldKind::MessageQueue, resumedRunning);
    const CpuContext entry = resumed ? resumed->entry : *cpu;
    const uint32_t queuePtr = resumed ? resumed->extra[0] : cpu->gpr[3];
    const uint32_t outMsgPtr = resumed ? resumed->extra[1] : cpu->gpr[4];
    const uint32_t flags = resumed ? resumed->extra[2] : cpu->gpr[5];
    const bool block = (flags & 1u) != 0;

    if (queuePtr == 0) {
        cpu->gpr[3] = 0;
        return 0;
    }

    // Pop, then wake senders now that there is space; an empty queue parks this
    // thread on the receive queue.
    return MsgQueueOp(
        cpu, "OS__ReceiveMessage", queuePtr, block,
        [](uint32_t queue) { return !MsgQueueIsEmpty(queue); },
        [outMsgPtr](uint32_t queue) {
            const uint32_t msg = MsgQueueDequeue(queue);
            if (outMsgPtr != 0) {
                ::Memory::Write32(outMsgPtr, msg);
            }
        },
        kMsgQueueSendOffset, kMsgQueueRecvOffset, &entry, outMsgPtr, resumed, resumedRunning);
}
REGISTER_NATIVE_FUNCTION(0x801104D0, OS__ReceiveMessage_HLE_801104d0);

