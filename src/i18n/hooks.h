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
// The seam is wired ONCE at startup via install() (whether or not translation
// is currently enabled), so nothing is ever appended to render's frame-callback
// list after the render hook goes live -- that would race the game thread.
// Whether a line is actually queued is then a cheap runtime check of
// config::get().inbound_chat inside the detour, and the worker engine is
// started/stopped live by apply_config().

namespace i18n::hooks {

// Installs the AddEntry detour + the per-frame pump + the engine's result
// applier. Idempotent. Wire this ONCE, alongside the other
// bring_up_subsystems() installs and BEFORE render::install(). Does NOT start
// the engine -- call apply_config() for that. Main thread only.
void install();

// (Re)builds the provider config + system prompt from i18n::config and starts
// the worker engine when inbound_chat is enabled, or stops it when disabled.
// Safe to call repeatedly at runtime -- this is what the menu's Save button
// calls so settings changes apply live, with no game restart. Main thread only.
void apply_config();

} // namespace i18n::hooks
