#include "save_state.h"
// OSSleepTicks / OSSleepThread parking plus the host-side sleep-timer table.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "det_clock.h"
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "timebase_contract.h"
#include "runtime_log.h"
#include "os_internal.h"

uint64_t PPC_ReadGuestTimeBase();  // ppc_helpers.cpp

namespace {
// Sleep deadlines live on the virtual timeline in deterministic mode (see det_clock.h), so a
// sleeper wakes at the same point of guest execution on every device. Otherwise on the guest's
// time base: the host clock, except that it stands still while Options pauses the game and runs
// with the game during fast-forward (guest_clock.h), so a sleep lasts as long as the game's other
// clocks say.
std::chrono::steady_clock::time_point SleepClockNow()
{
    if (DetClock::Enabled()) return DetClock::NowTimePoint();
    return std::chrono::steady_clock::time_point{TimeBaseContract::TicksToDuration(PPC_ReadGuestTimeBase())};
}
} // namespace

namespace OsHleInternal {
std::mutex gSleepTimerMutex;
std::vector<SleepTimerEntry> gSleepTimers;

void CancelSleepTimer(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    std::erase_if(gSleepTimers, [threadPtr](const SleepTimerEntry& entry) {
        return entry.threadPtr == threadPtr;
    });
}

uint64_t NextSleepTimerTicks()
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    uint64_t next = DetClock::kNever;
    for (const SleepTimerEntry& entry : gSleepTimers) {
        next = std::min(next, DetClock::TicksAtOrAfter(entry.deadline));
    }
    return next;
}

bool SleepTimerIsPending(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    for (const SleepTimerEntry& entry : gSleepTimers) {
        if (entry.threadPtr == threadPtr) {
            return true;
        }
    }
    return false;
}

// Threads with an outstanding OSSleepTicks park (timer armed, wake not yet delivered). If one ends
// up park-shaped with no pending timer, the reconciler in ProcessSleepTimers heals it; nothing
// outside this set is ever force-started.
std::mutex gOutstandingParkMutex;
std::unordered_set<uint32_t> gOutstandingParks;

void ClearOutstandingPark(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.erase(threadPtr);
}

bool ProcessSleepTimers(CpuContext* cpu)
{
    using Clock = std::chrono::steady_clock;

    // Pop and process ONE due timer at a time, straight from the shared table. Resuming a
    // sleeper re-enters the scheduler (OSResumeThread -> SelectThread) and can switch fibers
    // away from this call. Timers that had already been popped into a private list would then
    // sit on the suspended fiber's stack with their threads parked and no entry in the table:
    // exactly the "park-shaped with no pending wake timer" strand the reconciler below heals
    // 100ms late, followed by a "sleep-timer stale" drop when this fiber finally resumes.
    // Leaving unprocessed timers in the table keeps them visible to every other pump (idle
    // loop, other threads' SelectThread) while this one is switched away.
    bool processedAny = false;
    constexpr size_t kMaxTimersPerCall = 64;
    size_t processedCount = 0;
    const auto now = SleepClockNow();
    while (processedCount < kMaxTimersPerCall) {
        SleepTimerEntry timer{0, {}};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(gSleepTimerMutex);
            for (auto it = gSleepTimers.begin(); it != gSleepTimers.end(); ++it) {
                if (it->deadline <= now) {
                    timer = *it;
                    gSleepTimers.erase(it);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            break;
        }
        ++processedCount;
        processedAny = true;

        const uint32_t threadPtr = timer.threadPtr;
        if (threadPtr == 0 ||
            !Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t))) {
            continue;
        }

        // A sleep park always leaves the thread Ready with a suspension. A Running thread with a due
        // timer is mid-park (fire raced the self-suspend), so defer instead of consuming it there or
        // the sleeper strands. Any other shape means the sleeper already woke; drop the stale timer
        // without touching the thread.
        const uint16_t stateAtFire = Memory::Read16(threadPtr + kThreadStateOffset);
        const int32_t countAtFire =
            static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
        if (stateAtFire == kThreadStateRunning) {
            std::lock_guard<std::mutex> lock(gSleepTimerMutex);
            bool present = false;
            for (const SleepTimerEntry& entry : gSleepTimers) {
                if (entry.threadPtr == threadPtr) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                gSleepTimers.push_back({threadPtr, now + std::chrono::milliseconds(2)});
            }
            continue;
        }
        if (stateAtFire != kThreadStateReady || countAtFire < 1) {
            RT_LOG(RT_TAG_OS) << "sleep-timer stale for thread 0x" << std::hex << threadPtr
                      << std::dec << " (state=" << stateAtFire << " suspends=" << countAtFire
                      << "); dropped without consuming a suspension" << std::endl;
            ClearOutstandingPark(threadPtr);
            continue;
        }

        ClearOutstandingPark(threadPtr);
        cpu->gpr[3] = threadPtr;
        OSResumeThread_HLE_801134b0(cpu);

        // OSResumeThread only peels one suspension level, so if the sleeper's count is inflated
        // (failed park, overlapping suspend) dropping the timer here would strand it. Keep it armed
        // instead of consuming it; remaining suspensions after the park's own are foreign ones woken
        // by their own OSResumeThread. Rearming here used to steal those foreign suspensions, which
        // is how the DWC re-init thread got stranded.
    }

    // Stranded-sleeper reconciler: heals a thread whose park has no pending wake timer (lost to a
    // race) by resuming it once that shape persists for 100ms, sampled every 50ms so no strand is
    // missed. The atomic CAS below lets exactly one caller run the scan when ProcessSleepTimers
    // executes on more than one host thread; the rest skip it.
    static std::atomic<Clock::rep> strandScanDueAt{0};
    auto strandScanClaim = strandScanDueAt.load(std::memory_order_relaxed);
    const bool runStrandReconciler =
        now.time_since_epoch().count() >= strandScanClaim &&
        strandScanDueAt.compare_exchange_strong(
            strandScanClaim,
            static_cast<Clock::rep>(
                (now + std::chrono::milliseconds(50)).time_since_epoch().count()),
            std::memory_order_relaxed);
    if (runStrandReconciler) {
        static std::mutex strandMutex;
        static std::unordered_map<uint32_t, Clock::time_point> strandFirstSeen;

        std::vector<uint32_t> outstanding;
        {
            std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
            outstanding.assign(gOutstandingParks.begin(), gOutstandingParks.end());
        }
        // Decide who to heal under the lock, but resume OUTSIDE it: the resume
        // re-enters the scheduler and thus this function, and holding the lock
        // across it is a guaranteed self-deadlock.
        std::vector<uint32_t> toHeal;
        {
            std::lock_guard<std::mutex> strandLock(strandMutex);
            // Drop debounce records for threads whose park was satisfied elsewhere.
            for (auto it = strandFirstSeen.begin(); it != strandFirstSeen.end();) {
                if (std::find(outstanding.begin(), outstanding.end(), it->first) ==
                    outstanding.end()) {
                    it = strandFirstSeen.erase(it);
                } else {
                    ++it;
                }
            }
            const auto reconcileNow = SleepClockNow();
            for (const uint32_t threadPtr : outstanding) {
                if (SleepTimerIsPending(threadPtr)) {
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                if (!Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t)) ||
                    Fiber::GuestFiberManager::IsTerminated(threadPtr)) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const uint16_t state = Memory::Read16(threadPtr + kThreadStateOffset);
                const int32_t count =
                    static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
                if (state != kThreadStateReady || count < 1) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const auto seen = strandFirstSeen.find(threadPtr);
                if (seen == strandFirstSeen.end()) {
                    strandFirstSeen.emplace(threadPtr, reconcileNow);
                    continue;
                }
                if (reconcileNow - seen->second < std::chrono::milliseconds(100)) {
                    continue;
                }
                strandFirstSeen.erase(seen);
                ClearOutstandingPark(threadPtr);
                toHeal.push_back(threadPtr);
            }
        }
        for (const uint32_t threadPtr : toHeal) {
            RT_LOG(RT_TAG_OS) << "reconciler: thread 0x" << std::hex << threadPtr
                      << std::dec << " park-shaped with no pending wake timer for 100ms; "
                      << "resuming lost sleeper" << std::endl;
            cpu->gpr[3] = threadPtr;
            OSResumeThread_HLE_801134b0(cpu);
        }
    }

    return processedAny;
}
} // namespace OsHleInternal

namespace {
// OSSleepThread must reach SelectThread with the scheduler-disable count at zero, or the thread
// relinks into the wait queue a second time and OSWakeupThread livelocks. The SDK guarantees this;
// deferred guest-callback batches (VI retrace, alarms) can raise the count and violate it.
bool SchedulerCanSwitchAway()
{
    return ::Memory::Read32(kSchedulerIdleFlagAddr) == 0;
}

// A blocking call from a non-switchable region can't park, so the caller's retry loop spins; that's
// survivable if host-side work (alarm, IOS completion) eventually satisfies the wait, terminal if
// not. Report once per wait queue and escalate if a site stays wedged long enough to read as a freeze.
void ReportUnparkableSleep(uint32_t queuePtr, uint32_t thread)
{
    using Clock = std::chrono::steady_clock;

    // thread_local rather than static: guest scheduling normally runs on one
    // host thread, but this is reachable from the host frame loop too and a
    // diagnostic must not be the thing that introduces a data race.
    thread_local std::unordered_set<uint32_t> reportedQueues;
    thread_local uint32_t wedgedQueue = 0;
    thread_local Clock::time_point wedgedSince{};
    thread_local bool escalated = false;

    const uint32_t disableCount = ::Memory::Read32(kSchedulerIdleFlagAddr);

    if (reportedQueues.insert(queuePtr).second) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: thread 0x" << std::hex << thread
                  << " tried to block on wait queue 0x" << queuePtr
                  << std::dec << " but the scheduler could not switch away";
        if (disableCount != 0) {
            std::cerr << " (OSDisableScheduler nesting count " << disableCount << ")";
        }
        std::cerr << ". Not parking - the caller retries instead of corrupting"
                     " the wait queue." << std::endl;
    }

    const auto now = Clock::now();
    if (queuePtr != wedgedQueue) {
        wedgedQueue = queuePtr;
        wedgedSince = now;
        escalated = false;
        return;
    }

    constexpr auto kWedgeThreshold = std::chrono::seconds(5);
    if (!escalated && now - wedgedSince >= kWedgeThreshold) {
        escalated = true;
        RT_LOG(RT_TAG_OS) << "OSSleepThread: wait queue 0x" << std::hex << queuePtr
                  << " has been unsatisfiable for " << std::dec
                  << std::chrono::duration_cast<std::chrono::seconds>(now - wedgedSince).count()
                  << "s from a non-switchable region. The guest callback that is"
                     " blocking here must not block, or the deferred-callback"
                     " scope around it is wrong." << std::endl;
    }
}
} // namespace

// OSSleepThread (0x801b5c90)
// Puts the current thread to sleep on a specified wait queue.
extern "C" void SelectThread_ResumeIdle(CpuContext* cpu, uint32_t runningContext);

// Save states: the rest of a sleep for a restored thread. Its SelectThread had parked it (finish the
// switch that brings it back) or, for the thread that was idling, continues the idle loop.
void OS_HLE_SleepResume(CpuContext* cpu, bool running, int32_t irqState)
{
    const uint32_t thread = Fiber::GuestFiberManager::GetCurrentGuestThread();
    if (running) {
        SelectThread_ResumeIdle(cpu, thread);
    } else {
        Fiber::GuestFiberManager::CompleteSwitchIn(thread, cpu);
    }
    OS__RestoreInterrupts_8010f1a0(irqState);
}

extern "C" void OSSleepThread_HLE_801138e0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    bool resumedRunning = false;
    if (const SaveState::YieldRecord* resumed = SaveState::TakeYield(SaveState::YieldKind::Sleep, resumedRunning)) {
        SaveState::YieldScope scope(*resumed);
        OS_HLE_SleepResume(cpu, resumedRunning, static_cast<int32_t>(resumed->extra[1]));
        return;
    }
    const uint32_t queuePtr = cpu->gpr[3];
    
    if (queuePtr == 0) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: null queue pointer!" << std::endl;
        return;
    }

    const CpuContext entry = *cpu;
    const int32_t irqState = OS__DisableInterrupts_8010f160();
    // A sleep the game calls directly (inside the HLE waits this is not the outermost wait).
    SaveState::YieldScope yieldScope(SaveState::YieldKind::Sleep, entry, {queuePtr, static_cast<uint32_t>(irqState)});
    
    try {
        uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        
        // If no current thread is set but we have a default thread, use that
        // This can happen during early boot before the scheduler has switched threads
        if (currentThread == 0) {
            currentThread = kDefaultThreadContextAddr;
            if (!Memory::Contains(currentThread, 4)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: no current thread and default thread not valid!" << std::endl;
                OS__RestoreInterrupts_8010f1a0(irqState);
                return;
            }
            // Set the default thread as current (both running and context)
            ::Memory::Write32(kOSRunningContextAddr, currentThread);
            OS__SetCurrentContext_8010aca0(currentThread);
            
            // Also register as a fiber if not already
            if (Fiber::GuestFiberManager::IsInitialized() && 
                !Fiber::GuestFiberManager::HasFiber(currentThread)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: registering default thread 0x"
                          << std::hex << currentThread << " as fiber" << std::dec << std::endl;
                Fiber::GuestFiberManager::RegisterMainThreadAsFiber(currentThread, cpu);
            }
        }
        
        // See SchedulerCanSwitchAway: parking is only safe when SelectThread is
        // permitted to switch. Otherwise leave the thread RUNNING and the wait
        // queue untouched and let the caller's retry loop re-test its condition.
        if (!SchedulerCanSwitchAway()) {
            ReportUnparkableSleep(queuePtr, currentThread);
            OS__RestoreInterrupts_8010f1a0(irqState);
            return;
        }

        // Mark thread as WAITING
        ::Memory::Write16(currentThread + 0x2C8u, 4);

        // Store the queue we're waiting on
        ::Memory::Write32(currentThread + 0x2DCu, queuePtr);

        // Insert into wait queue (sorted by priority). The shared helper also
        // re-writes the queue pointer stored above and, for a run-queue
        // address, publishes the pending bit - neither applies to a wait queue.
        const int32_t ourPrio = static_cast<int32_t>(::Memory::Read32(currentThread + 0x2D0u));
        InsertThreadIntoQueueByPriority(queuePtr, currentThread, ourPrio);

        // Mark fiber as waiting
        if (Fiber::GuestFiberManager::IsInitialized()) {
            Fiber::GuestFiberManager::SuspendGuestThread(currentThread);
        }

        // Set reschedule flag and switch threads
        ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        
        // Match the original SDK behavior: sleep yields via SelectThread(0).
        cpu->gpr[3] = 0;
        SelectThread_80112b90(cpu);

        // Defence in depth: SelectThread has other paths that return without switching (context
        // mismatch, uninitialised thread system, a run queue that drained mid-enqueue). None of
        // them may leave this thread WAITING and linked, or the caller's retry loop inserts the same
        // node twice; a real switch resumes here already RUNNING with the queue pointer cleared, so
        // this block never fires there.
        if (::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateWaiting &&
            ::Memory::Read32(currentThread + kThreadQueueOffset) == queuePtr) {
            // Reverse of the priority-sorted insert above, so a thread is never
            // left WAITING and linked while its caller keeps running on the
            // same stack.
            UnlinkGuestListNode(currentThread, kThreadNextOffset, kThreadPrevOffset, queuePtr,
                                queuePtr + 4u);
            ::Memory::Write32(currentThread + kThreadQueueOffset, 0);
            ::Memory::Write16(currentThread + kThreadStateOffset, kThreadStateRunning);
            if (Fiber::GuestFiberManager::IsInitialized() &&
                Fiber::GuestFiberManager::HasFiber(currentThread)) {
                Fiber::GuestFiberManager::ResumeGuestThread(currentThread);
            }
            ReportUnparkableSleep(queuePtr, currentThread);
        }

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSleepThread", e);
    }
    
    OS__RestoreInterrupts_8010f1a0(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801138E0, OSSleepThread_HLE_801138e0, (CpuContext* ctx), (ctx));

// Save states: the host side of the OS HLE (sleep timers, parks, interrupt state); the rest is guest memory.
void OS_HLE_SaveState(SaveState::Writer& out)
{
    using namespace OsHleInternal;
    out.Pod(g_interrupts_enabled.load());
    out.Pod(g_interrupt_mask.load());
    {
        std::lock_guard<std::mutex> lock(gSleepTimerMutex);
        // Deadlines relative to now: the windowed game's sleep clock is the host's, which a state loaded
        // in a later session does not share (a frontend restores its virtual clock before this is read).
        const auto now = SleepClockNow();
        out.Pod(uint32_t(gSleepTimers.size()));
        for (const auto& entry : gSleepTimers) {
            out.Pod(entry.threadPtr);
            out.Pod(int64_t((entry.deadline - now).count()));
        }
    }
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    std::vector<uint32_t> parks(gOutstandingParks.begin(), gOutstandingParks.end());
    std::sort(parks.begin(), parks.end());
    out.Pod(uint32_t(parks.size()));
    for (const uint32_t thread : parks) out.Pod(thread);
}

void OS_HLE_LoadState(SaveState::Reader& in)
{
    using namespace OsHleInternal;
    g_interrupts_enabled.store(in.Get<bool>());
    g_interrupt_mask.store(in.Get<uint32_t>());
    {
        std::lock_guard<std::mutex> lock(gSleepTimerMutex);
        gSleepTimers.clear();
        const auto now = SleepClockNow();
        for (uint32_t i = 0, n = in.Get<uint32_t>(); i < n && in.Ok(); ++i) {
            const uint32_t thread = in.Get<uint32_t>();
            const auto remaining = in.Get<int64_t>();
            gSleepTimers.push_back({thread, now + std::chrono::steady_clock::duration(remaining)});
        }
    }
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.clear();
    for (uint32_t i = 0, n = in.Get<uint32_t>(); i < n && in.Ok(); ++i) gOutstandingParks.insert(in.Get<uint32_t>());
}
