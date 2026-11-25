#pragma once

#include <aurora/aurora.h>

// Controller reads on the game thread. Aurora's input thread reads and detects controllers under a gate
// (aurora/lib/input_thread.cpp): a device scan there holds SDL's joystick lock for up to seconds whenever any
// device comes or goes (NOTES.md 2026-11-18). A per-frame reader holds this gate while it calls SDL's controller
// functions; when it is not given (within a millisecond), the reader uses its last result instead of waiting.
class ControllerGate {
public:
    ControllerGate() : m_held(aurora_controllers_try_lock()) {}
    ~ControllerGate() {
        if (m_held) aurora_controllers_unlock();
    }
    ControllerGate(const ControllerGate&) = delete;
    ControllerGate& operator=(const ControllerGate&) = delete;
    explicit operator bool() const { return m_held; }

private:
    bool m_held;
};
