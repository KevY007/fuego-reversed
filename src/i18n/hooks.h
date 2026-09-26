#pragma once
// i18n/hooks.h -- the chat seam (Task 11).
//
// Detours SA:MP's CChat::AddEntry (samp::addr::FUNC_ADD_TO_CHAT_WND) so every
// incoming server chat/info line also gets queued for translation, and
// registers the main-thread pump (via render::on_frame) that applies a
// finished translation by re-emitting it (marked) through the SAME
// trampoline used to show the original line -- never through the hooked
// entry point itself, so applying a result can never re-trigger translation.
//
// Inert unless i18n::config::get().inbound_chat is true at install() time.

namespace i18n::hooks {

// Installs the AddEntry detour + the per-frame pump + the engine's result
// applier. No-op if already installed, or if inbound_chat is off (Phase 0:
// checked once here; see core/main.cpp for the call site). Call after
// i18n::engine::start() and after samp::base()/is_in_game() are valid (i.e.
// alongside the other bring_up_subsystems() installs). Main thread only.
void install();

} // namespace i18n::hooks
