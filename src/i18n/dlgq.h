#pragma once
// Ordered, timing-faithful release queue for HELD server dialogs.
//
// PURE (no game types, injected clock) so the ordering/timing rules are
// host-tested (tests/dlgq_test.cpp). surfaces.cpp holds a ShowDialog while it is
// translated and releases it through this queue:
//  * strictly in arrival order -- a fast translation never overtakes a slow one;
//  * each release no sooner than the previous release + the ORIGINAL gap between
//    the two arrivals, so spammed "pages" keep the reading time the server gave;
//  * an item still untranslated after maxHoldMs is released as its original;
//  * a push onto an empty queue starts a fresh burst (no inherited gap).
// Payloads are opaque bytes; the caller decides how to show them.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace i18n::dlgq {

struct Release {
    uint64_t             seq = 0;
    std::vector<uint8_t> payload;    // what to show: the translation, or the original
    std::vector<uint8_t> original;   // always the original payload
    bool                 translated = false;
};

class Queue {
public:
    explicit Queue(uint32_t maxHoldMs = 10000, size_t maxItems = 16)
        : maxHoldMs_(maxHoldMs), maxItems_(maxItems) {}

    bool   empty() const { return q_.empty(); }
    size_t size() const { return q_.size(); }

    // Hold a dialog (its original payload) that arrived at nowMs. False if full.
    bool push(uint64_t seq, uint32_t nowMs, std::vector<uint8_t> original);

    // Translation finished for `seq`: show `translated` (empty = show the original).
    void resolve(uint64_t seq, std::vector<uint8_t> translated);

    // Call every frame: everything due now, in order.
    std::vector<Release> tick(uint32_t nowMs);

    // Release everything immediately as originals, in order (feature switched off).
    std::vector<Release> flush();

    // Release everything immediately, in order, without waiting for gaps or
    // translations: the translation where one is ready, else the original.
    std::vector<Release> drain();

private:
    struct Item {
        uint64_t             seq = 0;
        uint32_t             arrivalMs = 0;
        std::vector<uint8_t> original;
        std::vector<uint8_t> translated;
        bool                 ready = false;
    };
    std::deque<Item> q_;
    uint32_t maxHoldMs_;
    size_t   maxItems_;
    bool     chain_ = false;       // a previous release in the current burst exists
    uint32_t lastReleaseMs_ = 0;
    uint32_t lastArrivalMs_ = 0;
};

} // namespace i18n::dlgq
