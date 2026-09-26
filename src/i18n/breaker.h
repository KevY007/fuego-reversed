#pragma once

namespace i18n {

// Consecutive-failure circuit breaker for IProvider implementations.
//
// After K consecutive failures, allow() returns false (the caller must not
// touch the network) for T milliseconds; a success resets the failure count
// and closes the circuit. The clock is injected by the caller as `now`
// (any monotonically-increasing millisecond counter, e.g. GetTickCount())
// so this can be unit-tested without touching the system clock or WinHTTP.
//
// Usage: call allow(now) before attempting a request; call on_success() or
// on_failure(now) after the attempt completes.
struct Breaker {
    int K;                      // consecutive-failure threshold
    int T;                      // open duration in ms once tripped
    int consecutiveFailures = 0;
    bool tripped = false;
    unsigned long openedAt = 0; // `now` at the moment the breaker tripped

    explicit Breaker(int k = 3, int t = 15000) : K(k), T(t) {}

    // False while the breaker is open and T ms haven't elapsed since it
    // tripped. Once T ms have elapsed this returns true again (half-open:
    // the next on_success()/on_failure() decides whether it stays closed).
    bool allow(unsigned long now) const {
        if (!tripped) return true;
        return (now - openedAt) >= static_cast<unsigned long>(T);
    }

    // A successful call closes the circuit and resets the failure count.
    void on_success() {
        consecutiveFailures = 0;
        tripped = false;
    }

    // A failed call increments the failure count; once it reaches K the
    // breaker trips (or re-trips, restarting the T ms window) at `now`.
    void on_failure(unsigned long now) {
        ++consecutiveFailures;
        if (consecutiveFailures >= K) {
            tripped = true;
            openedAt = now;
        }
    }
};

} // namespace i18n
