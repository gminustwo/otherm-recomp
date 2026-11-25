// Controllers are updated on an input thread, the way Dolphin's ControllerInterface refreshes its devices off the
// emulation thread. SDL detects controllers inside SDL_UpdateJoysticks, which every SDL_PumpEvents used to run on the
// game thread: when any device comes or goes (a Bluetooth mouse waking, a USB stick), SDL re-enumerates every HID
// device there (HIDAPI at once, DirectInput 300 ms and 2 s later), holding its joystick lock for 0.25 to 3.4 s
// (NOTES.md 2026-11-18). SDL_HINT_AUTO_UPDATE_JOYSTICKS is off, this thread calls SDL_UpdateJoysticks under the
// controller gate, and the game's per-frame readers try the gate first: while a scan holds it they reuse their last
// reading instead of waiting on SDL's lock (Dolphin's UpdateInput does the same with try_lock).
#include "input.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace aurora::input {
namespace {
using Clock = std::chrono::steady_clock;

std::recursive_timed_mutex g_controllerGate;
std::atomic_bool g_running{false};
std::thread g_thread;
// When the thread's current SDL_UpdateJoysticks began (steady clock, ns), 0 between updates. An ordinary update takes
// tens of microseconds and a reader waits it out (up to kGateWait); one past kScanAfter is a device scan, and readers
// then go without the controllers at once rather than wait a millisecond each.
std::atomic<int64_t> g_updateSince{0};
constexpr auto kGateWait = std::chrono::milliseconds(1);
constexpr auto kScanAfter = std::chrono::milliseconds(2);

int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
// About a millisecond between updates (SDL polls joysticks that often when it updates them itself).
constexpr auto kUpdateInterval = std::chrono::microseconds(1000);

void run() noexcept {
#ifdef _WIN32
  // SDL's DirectInput and Windows.Gaming.Input calls want COM on the calling thread.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // Sleep(1) lasts 15.6 ms at the default timer resolution: a high-resolution timer keeps updates ~1 ms apart.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
  int slowLogged = 0;
  while (g_running.load(std::memory_order_acquire)) {
    const Clock::time_point start = Clock::now();
    Clock::time_point updateStart;
    {
      std::lock_guard gate(g_controllerGate);
      updateStart = Clock::now();
      g_updateSince.store(now_ns(), std::memory_order_release);
      SDL_UpdateJoysticks();
      g_updateSince.store(0, std::memory_order_release);
    }
    const Clock::time_point end = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - updateStart).count();
    if (ms > 50.0 && slowLogged < 30) {
      ++slowLogged;
      int hour = 0, minute = 0, second = 0, milli = 0;  // to line it up with device changes in other logs
#ifdef _WIN32
      SYSTEMTIME now{};
      GetLocalTime(&now);
      hour = now.wHour, minute = now.wMinute, second = now.wSecond, milli = now.wMilliseconds;
#endif
      Log.info("Controller update took {:.1f} ms on the input thread (a device scan; the gate waited {:.1f} ms first), "
               "ended {:02}:{:02}:{:02}.{:03}",
               ms, std::chrono::duration<double, std::milli>(updateStart - start).count(), hour, minute, second, milli);
    }
#ifdef _WIN32
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(kUpdateInterval).count() / 100);
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      std::this_thread::sleep_for(kUpdateInterval);
    }
#else
    std::this_thread::sleep_for(kUpdateInterval);
#endif
  }
#ifdef _WIN32
  if (timer != nullptr) CloseHandle(timer);
  if (SUCCEEDED(com)) CoUninitialize();
#endif
}
} // namespace

void prepare_input_thread() noexcept {
  // Before SDL_INIT_JOYSTICK: SDL_PumpEvents leaves controllers alone; the thread updates them.
  SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
}

void start_input_thread() noexcept {
  if (g_running.exchange(true)) return;
  g_thread = std::thread(run);
}

void stop_input_thread() noexcept {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

bool controllers_try_lock() noexcept {
  if (g_controllerGate.try_lock()) {
    return true;
  }
  const int64_t since = g_updateSince.load(std::memory_order_acquire);
  if (since != 0 && now_ns() - since > std::chrono::duration_cast<std::chrono::nanoseconds>(kScanAfter).count()) {
    return false;  // a device scan: it can take seconds
  }
  return g_controllerGate.try_lock_for(kGateWait);
}

void controllers_unlock() noexcept { g_controllerGate.unlock(); }
} // namespace aurora::input
