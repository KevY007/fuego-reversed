#include "names.h"

#include "protect.h"
#include "../samp/pools.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace i18n::names {

namespace {
constexpr uint32_t kScanMs   = 2000;
constexpr uint32_t kForgetMs = 10 * 60 * 1000;
std::unordered_map<std::string, uint32_t> g_seen; // name -> last seen tick
uint32_t g_lastScan = 0;
bool     g_scanned  = false;
} // namespace

void tick(uint32_t nowMs) {
    if (g_scanned && nowMs - g_lastScan < kScanMs) return;
    g_scanned  = true;
    g_lastScan = nowMs;

    bool changed = false;
    for (int id = 0; id < samp::SAMP_MAX_PLAYERS; ++id) {
        std::string nm = samp::player_name(id);
        if (nm.empty()) continue;
        auto it = g_seen.find(nm);
        if (it == g_seen.end()) { g_seen.emplace(std::move(nm), nowMs); changed = true; }
        else it->second = nowMs;
    }
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        if (nowMs - it->second > kForgetMs) { it = g_seen.erase(it); changed = true; }
        else ++it;
    }
    if (g_seen.size() > 3000) { g_seen.clear(); changed = true; }
    if (!changed) return;

    std::vector<std::string> names;
    names.reserve(g_seen.size());
    for (const auto& kv : g_seen) names.push_back(kv.first);
    i18n::protect::set_names(i18n::protect::name_variants(names));
}

} // namespace i18n::names
