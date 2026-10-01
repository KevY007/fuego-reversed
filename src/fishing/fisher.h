#pragma once
/*
	fishing/fisher.h  --  the auto fisher's brain (pure, host-tested).

	Mike's CnR fishing, as recorded with the packet recorder (fuego_packets.log,
	2026-10-02, ~20 casts):
	  - rod out: attached object model 18632 on slot 0 (/fish equips it);
	  - cast:    hold AIM (KEY_HANDBRAKE) -> the server plays GRENADE/WEAPON_START_THROW
	             -> press FIRE while still aiming -> GRENADE/WEAPON_THROW -> let go of
	             both ~0.6-1 s later -> chat "The bar on-screen shows line length ...";
	  - bite:    ~6-8 s later, chat "Catch weight: ... (X lbs)" + "... Keep tapping
	             SPRINT ...";
	  - reel:    every SPRINT press reels a fixed step (8/7/6/4 % by weight); presses
	             closer together than the weight allows cost 10 % line health each
	             (huge fish: under ~0.25 s; medium: under ~0.20 s; small: ~0.15 s).
	             A stats textdraw "<line>%~N~<health>%~N~<weight>lbs~N~" shows both;
	  - end:     chat "You caught ..." (a rotten fish: ~10 s stunned), "The line
	             snapped ...", or "The catch got away.".

	The fisher never acts blind: every step waits for the server's answer to the
	last one (with a timeout + retry), so it can't get out of step, and it starts
	from whatever point the cycle is at. Reeling starts at a safe pace for the
	weight and slows down by itself whenever the line takes damage. Every delay is
	randomised within human ranges.

	It only decides which of FIRE / SPRINT / HANDBRAKE to hold; autofish.cpp feeds
	it the server's messages and puts those keys into our syncs.
*/

#include <cstdint>
#include <string>

namespace fish {

constexpr uint16_t KEY_FIRE      = 4;
constexpr uint16_t KEY_SPRINT    = 8;
constexpr uint16_t KEY_HANDBRAKE = 128;
constexpr uint16_t kControlled   = KEY_FIRE | KEY_SPRINT | KEY_HANDBRAKE; // what the fisher owns
constexpr int32_t  kRodModel     = 18632;

enum class State {
	Off,
	NoRod,     // enabled, waiting for the rod to be out (/fish)
	Ready,     // about to cast (after a short pause)
	Aiming,    // AIM held, waiting for the throw to start
	Throwing,  // AIM + FIRE held, waiting for the throw
	Casting,   // keys let go, waiting for "line is out"
	WaitBite,  // line out, waiting for a fish
	Reeling,   // tapping SPRINT
	Cooldown,  // a short break after a catch (long after a rotten fish)
	Paused,    // a dialog is open: hands off until it's closed
};

struct Stats {
	unsigned caught = 0, rotten = 0, other = 0, snapped = 0, gotAway = 0;
	double   pounds = 0;      // total weight of the fish caught
	double   biggest = 0;
	std::string biggestName;
};

class Fisher {
public:
	explicit Fisher( uint32_t seed = 0x5EEDF15u ) : rng_( seed ? seed : 1 ) {}

	void enable( bool on, uint32_t nowMs );
	bool enabled() const { return state_ != State::Off; }
	State state() const { return state_; }

	// --- what the server tells us -----------------------------------------
	void on_rod( bool out );                                             // our slot-0 rod attached / removed
	void on_anim( const std::string &lib, const std::string &name, uint32_t nowMs ); // ApplyAnimation on us
	void on_chat( const std::string &text, uint32_t nowMs );            // a chat line (colour codes allowed)
	void on_textdraw( const std::string &text, uint32_t nowMs );        // any textdraw text (finds the stats one)
	void on_dialog_shown( uint32_t nowMs );
	void on_dialog_closed( uint32_t nowMs );

	// Each frame: the keys (within kControlled) to hold right now.
	uint16_t tick( uint32_t nowMs );

	// True while it's using the keys (casting, reeling): only then are AIM / FIRE /
	// SPRINT taken from the player -- waiting for a bite, they're the player's.
	bool holds_keys() const;

	// "reeling 26.3 lbs -- line 64%, health 90%" etc.
	std::string status( uint32_t nowMs ) const;
	const Stats &stats() const { return stats_; }

	// Something the player should be told in chat (stopped, why); false when none.
	bool pop_notice( std::string &out );

	// Helpers (exposed for tests).
	static std::string strip_colors( const std::string &s );                  // "{FFFFFF}" codes out
	static bool parse_stats( const std::string &td, int &line, int &health, double &lbs );
	static bool parse_weight( const std::string &chat, double &lbs );         // "... (26.3 lbs) ..."
	static uint32_t base_gap_ms( double lbs );                                // safe time between presses

private:
	uint32_t rand_between( uint32_t lo, uint32_t hi ); // inclusive
	uint16_t step( uint32_t nowMs );                   // tick() without the hold cap
	void go( State s, uint32_t nowMs );
	void ready_in( uint32_t nowMs, uint32_t lo, uint32_t hi );
	void cast_failed( uint32_t nowMs, const char *why );
	void stop( const std::string &why );
	void start_reeling( double lbs, uint32_t nowMs );

	uint32_t rng_;
	State    state_ = State::Off;
	State    resumeTo_ = State::Ready; // after a dialog closes
	bool     rodOut_ = false;
	uint16_t keys_ = 0;
	uint32_t stateAt_ = 0;   // when the current state began
	uint32_t nextAt_ = 0;    // the next scheduled step (state-specific)
	uint32_t deadline_ = 0;  // waiting for the server: give up at this time
	double   pendingLbs_ = 0; // from "Catch weight: ..." (the bite line follows)
	bool     rottenWait_ = false;
	uint32_t releaseAt_ = 0; // Throwing: when FIRE goes up; Reeling: when the press ends
	uint32_t aimUpAt_ = 0;   // Throwing: when AIM goes up
	uint32_t keysSince_ = 0; // when the current key hold began (a hard cap on any hold)
	int      castFails_ = 0;
	bool     fireScheduled_ = false;    // Aiming: the throw started, FIRE is due at nextAt_
	bool     releaseScheduled_ = false; // Throwing: the throw happened, keys up at releaseAt_ / aimUpAt_

	// Reeling.
	double   lbs_ = 0;
	int      line_ = -1, health_ = -1;
	uint32_t gapMs_ = 0;      // current time between presses (base pace for the weight + backoff)
	uint32_t backoffMs_ = 0;
	uint32_t lastPressAt_ = 0;
	unsigned presses_ = 0;

	Stats    stats_;
	std::string notice_;
};

const char *state_name( State s );

} // namespace fish
