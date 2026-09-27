#pragma once
#include <cstdint>

namespace i18n::names {

// Main thread, every frame (cheap: rescans every 2s). Collects every connected
// player's name from SA:MP's player pool (by id, so they are real names -- incl.
// the local player), keeps names seen in the last 10 minutes (so "X left the
// server" still protects X), and republishes protect::set_names(name_variants())
// when the set changes. Names are then masked before any text reaches the model.
void tick(uint32_t nowMs);

} // namespace i18n::names
