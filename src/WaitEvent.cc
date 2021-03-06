#include "WaitEvent.h"

using namespace std::literals;

namespace Chili {

void WaitEvent::Reset() {
    _mutex.lock();
    _signalled = false;
    _mutex.unlock();
}

void WaitEvent::Signal() {
    _mutex.lock();
    _signalled = true;
    _mutex.unlock();
    OnSignalled();
    _cv.notify_all();
}

void WaitEvent::Wait() const {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [this] { return _signalled; });
}

bool WaitEvent::TryWait() const {
    return Wait(0us);
}

bool WaitEvent::Wait(std::chrono::microseconds timeout) const {
    std::unique_lock lock(_mutex);
    return _cv.wait_for(lock, timeout, [this] { return _signalled; });
}

bool WaitEvent::WaitUntil(Clock::TimePoint t) const {
    std::unique_lock lock(_mutex);
    return _cv.wait_until(lock, t, [this] { return _signalled; });
}

void WaitEvent::WaitAndReset() {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [this] { return _signalled; });
    _signalled = false;
}

bool WaitEvent::TryWaitAndReset() {
    return WaitAndReset(0us);
}

bool WaitEvent::WaitAndReset(std::chrono::microseconds timeout) {
    std::unique_lock lock(_mutex);

    if (_cv.wait_for(lock, timeout, [this] { return _signalled; })) {
        _signalled = false;
        return true;
    }

    return false;
}

bool WaitEvent::WaitUntilAndReset(Clock::TimePoint t) {
    std::unique_lock lock(_mutex);

    if (_cv.wait_until(lock, t, [this] { return _signalled; })) {
        _signalled = false;
        return true;
    }

    return false;
}

} // namespace Chili

