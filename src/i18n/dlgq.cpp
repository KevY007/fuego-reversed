#include "dlgq.h"

#include <utility>

namespace i18n::dlgq {

namespace {
// Wrap-safe "a is at or after b" for GetTickCount()-style clocks.
bool at_or_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }
} // namespace

bool Queue::push(uint64_t seq, uint32_t nowMs, std::vector<uint8_t> original) {
    if (q_.size() >= maxItems_) return false;
    if (q_.empty()) chain_ = false; // a new burst: no gap to honour yet
    Item it;
    it.seq       = seq;
    it.arrivalMs = nowMs;
    it.original  = std::move(original);
    q_.push_back(std::move(it));
    return true;
}

void Queue::resolve(uint64_t seq, std::vector<uint8_t> translated) {
    for (Item& it : q_) {
        if (it.seq != seq) continue;
        it.translated = std::move(translated);
        it.ready = true;
        return;
    }
}

std::vector<Release> Queue::tick(uint32_t nowMs) {
    std::vector<Release> out;
    while (!q_.empty()) {
        Item& h = q_.front();
        const bool expired = at_or_after(nowMs, h.arrivalMs + maxHoldMs_);
        if (!h.ready && !expired) break;                          // still translating
        if (chain_) {
            const int32_t  d   = (int32_t)(h.arrivalMs - lastArrivalMs_);
            const uint32_t gap = d > 0 ? (uint32_t)d : 0u;
            if (!at_or_after(nowMs, lastReleaseMs_ + gap)) break;  // not its turn yet
        }
        const bool useT = h.ready && !h.translated.empty();
        Release r;
        r.seq        = h.seq;
        r.translated = useT;
        r.payload    = useT ? h.translated : h.original;
        r.original   = std::move(h.original);
        out.push_back(std::move(r));
        chain_         = true;
        lastReleaseMs_ = nowMs;
        lastArrivalMs_ = h.arrivalMs;
        q_.pop_front();
    }
    return out;
}

std::vector<Release> Queue::flush() {
    std::vector<Release> out;
    for (Item& h : q_) {
        Release r;
        r.seq        = h.seq;
        r.translated = false;
        r.payload    = h.original;
        r.original   = std::move(h.original);
        out.push_back(std::move(r));
    }
    q_.clear();
    chain_ = false;
    return out;
}

std::vector<Release> Queue::drain() {
    std::vector<Release> out;
    for (Item& h : q_) {
        const bool useT = h.ready && !h.translated.empty();
        Release r;
        r.seq        = h.seq;
        r.translated = useT;
        r.payload    = useT ? h.translated : h.original;
        r.original   = std::move(h.original);
        out.push_back(std::move(r));
    }
    q_.clear();
    chain_ = false;
    return out;
}

} // namespace i18n::dlgq
