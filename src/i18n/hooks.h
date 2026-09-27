#pragma once
// i18n/hooks.h -- the chat seam (Task 11).
//
// Detours SA:MP's CChat::AddEntry (samp::addr::FUNC_ADD_TO_CHAT_WND): an
// incoming server chat/info line with a translation-memory hit is shown already
// translated; any other line is shown in the ORIGINAL at once (both through the
// trampoline -- never the hooked entry point, so nothing re-triggers
// translation) and queued. The main-thread pump (via render::on_frame) then
// writes a landed translation IN PLACE over the chat rows that line became
// (i18n::chatbuf) -- lines above/below never move -- and drives the other
// surfaces' per-frame work (held dialogs, reply/chat delivery deadlines).
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
// the worker engine when ANY translation surface is enabled (inbound chat,
// outgoing translation, dialogs/labels, HUD), or stops it when all are off.
// Safe to call repeatedly at runtime -- this is what the menu's Save button
// calls so settings changes apply live, with no game restart. A call that finds
// nothing actually changed (rebuilt config/prompt/target/codepage are identical
// to the running engine's) returns WITHOUT restarting it: engine::start()
// internally stops the previous engine first, which JOINS the worker threads on
// this (the main/game) thread and can stall it for as long as an in-flight HTTP
// call, and drops every job still queued (held dialog units, chat flips, outbound
// chat, dialog-input replies) -- unacceptable for something called on every menu
// Save and every outbound-toggle hotkey press. Before a real stop/restart, every
// swallowed dialog-input reply and outgoing chat line is sent as typed. Never
// throws (it runs inside hkEndScene): if engine::start()/stop() fails, the engine
// is left OFF and the next call retries. Main thread only.
void apply_config();

// True once install() has wired the AddEntry detour + registered the per-frame
// pump + the engine's result applier AND render::install() has hooked EndScene,
// which is what actually calls pump_tick() every frame -- i.e. once a submitted
// job is guaranteed to eventually be delivered to apply_result(), and held
// dialogs / deadlines are ticked. A surface that HOLDS or SWALLOWS something
// pending a translated result (a held dialog, outgoing chat, a dialog INPUT reply)
// must check this first: without a running pump nothing will ever call back or
// release it, so it must pass the original through instead.
bool pump_ready();

} // namespace i18n::hooks
