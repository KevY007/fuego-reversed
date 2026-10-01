#include "fisher.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace fish {

namespace {

constexpr uint32_t kAimTimeoutMs   = 1500;  // AIM held, the throw didn't start
constexpr uint32_t kThrowTimeoutMs = 1500;  // FIRE held, no throw
constexpr uint32_t kCastTimeoutMs  = 4000;  // keys up, the line isn't out
constexpr uint32_t kBiteTimeoutMs  = 60000; // line out, no fish (bites came 6-8 s after a cast)
constexpr uint32_t kReelTimeoutMs  = 90000; // reeling, no result
constexpr uint32_t kMaxHoldMs      = 3500;  // no key is ever held longer than this, whatever happens
constexpr uint32_t kBackoffMs      = 80;    // slower presses after each hit to the line
constexpr uint32_t kMaxBackoffMs   = 400;
constexpr int      kCastTries      = 3;

// The server's own lines start with these words; player chat relayed through the same
// RPC starts with the player's name ("[R2AW]Panzer_ (34): ..."), so a player quoting a
// fishing line can never be taken for the server.
bool starts( const std::string &s, const char *prefix ) {
	return s.compare( 0, std::char_traits<char>::length( prefix ), prefix ) == 0;
}

// Has `at` come? Wrap-safe: GetTickCount wraps every 49.7 days.
bool due( uint32_t nowMs, uint32_t at ) { return (int32_t) ( nowMs - at ) >= 0; }
// The later of two times, wrap-safe.
uint32_t later( uint32_t a, uint32_t b ) { return (int32_t) ( a - b ) >= 0 ? a : b; }

} // namespace

const char *state_name( State s ) {
	switch ( s ) {
	case State::Off:      return "off";
	case State::NoRod:    return "no rod";
	case State::Ready:    return "ready";
	case State::Aiming:   return "aiming";
	case State::Throwing: return "throwing";
	case State::Casting:  return "casting";
	case State::WaitBite: return "waiting for a bite";
	case State::Reeling:  return "reeling";
	case State::Cooldown: return "break";
	case State::Paused:   return "paused";
	}
	return "?";
}

// --- parsing -------------------------------------------------------------------

std::string Fisher::strip_colors( const std::string &s ) {
	std::string r;
	r.reserve( s.size() );
	for ( size_t i = 0; i < s.size(); ) {
		if ( s[i] == '{' && s.size() - i >= 8 && s[i + 7] == '}' ) {
			bool code = true;
			for ( size_t k = 1; k <= 6; ++k )
				if ( !std::isxdigit( (unsigned char) s[i + k] ) ) { code = false; break; }
			if ( code ) { i += 8; continue; }
		}
		r += s[i++];
	}
	return r;
}

bool Fisher::parse_stats( const std::string &td, int &line, int &health, double &lbs ) {
	int l = 0, h = 0, end = 0;
	double w = 0;
	if ( std::sscanf( td.c_str(), "%d%%~N~%d%%~N~%lflbs~N~%n", &l, &h, &w, &end ) != 3 || end != (int) td.size() )
		return false;
	line = l;
	health = h;
	lbs = w;
	return true;
}

bool Fisher::parse_weight( const std::string &chat, double &lbs ) {
	const std::string s = strip_colors( chat );
	const size_t e = s.find( " lbs)" );
	if ( e == std::string::npos ) return false;
	const size_t b = s.rfind( '(', e );
	if ( b == std::string::npos ) return false;
	const std::string num = s.substr( b + 1, e - b - 1 );
	char *end = nullptr;
	const double v = std::strtod( num.c_str(), &end );
	if ( end == num.c_str() || *end != '\0' || v <= 0 ) return false;
	lbs = v;
	return true;
}

// Safe time between SPRINT presses. From the recording: huge fish (>100 lbs) lost line
// health under ~0.25 s, medium under ~0.20 s, small/very small not at 0.15 s; each
// pace keeps a ~40-60 ms margin for network jitter (the server times its arrivals).
uint32_t Fisher::base_gap_ms( double lbs ) {
	if ( lbs <= 10.0 )  return 190;
	if ( lbs <= 25.0 )  return 205;
	if ( lbs <= 100.0 ) return 255;
	return 310;
}

// --- state -----------------------------------------------------------------------

uint32_t Fisher::rand_between( uint32_t lo, uint32_t hi ) {
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	return hi > lo ? lo + rng_ % ( hi - lo + 1 ) : lo;
}

void Fisher::go( State s, uint32_t nowMs ) {
	if ( state_ == State::Paused && s != State::Off ) { // behind a dialog: where to pick up again
		resumeTo_ = s;
		return;
	}
	state_ = s;
	stateAt_ = nowMs;
}

void Fisher::ready_in( uint32_t nowMs, uint32_t lo, uint32_t hi ) {
	keys_ = 0;
	go( State::Cooldown, nowMs );
	nextAt_ = nowMs + rand_between( lo, hi );
}

void Fisher::stop( const std::string &why ) {
	state_ = State::Off;
	keys_ = 0;
	notice_ = why;
}

void Fisher::cast_failed( uint32_t nowMs, const char *why ) {
	keys_ = 0;
	if ( ++castFails_ >= kCastTries ) {
		stop( std::string( "Stopped: the server didn't answer " ) + std::to_string( kCastTries ) +
		      " casts in a row (" + why + ") -- is the rod out and are you at the water?" );
		return;
	}
	ready_in( nowMs, 1800, 2600 );
}

void Fisher::start_reeling( double lbs, uint32_t nowMs ) {
	lbs_ = lbs > 0 ? lbs : 10.0;
	pendingLbs_ = 0;
	line_ = health_ = -1;
	backoffMs_ = 0;
	gapMs_ = base_gap_ms( lbs_ );
	presses_ = 0;
	keys_ = 0;
	castFails_ = 0;
	go( State::Reeling, nowMs );
	nextAt_ = nowMs + rand_between( 280, 520 ); // a human reaction to the bite
	releaseAt_ = 0;
}

void Fisher::enable( bool on, uint32_t nowMs ) {
	keys_ = 0;
	state_ = State::Off;
	if ( !on ) return;
	castFails_ = 0;
	rottenWait_ = false;
	if ( rodOut_ ) {
		state_ = State::Ready;
		stateAt_ = nowMs;
		nextAt_ = nowMs + rand_between( 300, 800 );
	} else {
		go( State::NoRod, nowMs );
	}
}

bool Fisher::holds_keys() const {
	return state_ == State::Aiming || state_ == State::Throwing || state_ == State::Casting ||
	       state_ == State::Reeling;
}

void Fisher::on_rod( bool out ) { rodOut_ = out; }

void Fisher::on_anim( const std::string &lib, const std::string &name, uint32_t nowMs ) {
	if ( lib != "GRENADE" ) return;
	if ( name == "WEAPON_START_THROW" ) {
		if ( state_ == State::Aiming && !fireScheduled_ ) {
			fireScheduled_ = true;
			nextAt_ = nowMs + rand_between( 300, 650 ); // then FIRE, still aiming
		}
	} else if ( name == "WEAPON_THROW" ) {
		if ( state_ == State::Throwing && !releaseScheduled_ ) {
			releaseScheduled_ = true;
			releaseAt_ = nowMs + rand_between( 470, 820 ); // FIRE held ~0.6-0.95 s in all
			aimUpAt_ = releaseAt_ + rand_between( 5, 60 ); // AIM a moment later
		}
	}
}

void Fisher::on_chat( const std::string &text, uint32_t nowMs ) {
	if ( state_ == State::Off ) return;
	const std::string s = strip_colors( text );
	const bool paused = state_ == State::Paused;
	const State cur = paused ? resumeTo_ : state_;
	const bool fishOn = cur == State::Reeling || cur == State::WaitBite;

	if ( starts( s, "The bar on-screen shows line length" ) ) { // the line is out
		if ( cur != State::Reeling ) {
			keys_ = 0;
			castFails_ = 0;
			go( State::WaitBite, nowMs );
		}
		return;
	}
	if ( starts( s, "Catch weight:" ) ) {
		double w = 0;
		if ( parse_weight( s, w ) ) pendingLbs_ = w;
		return;
	}
	if ( starts( s, "You caught something!" ) ) { // a bite ("... Keep tapping SPRINT ...")
		if ( cur != State::Reeling ) {
			if ( paused ) {
				resumeTo_ = State::Reeling;
				lbs_ = pendingLbs_ > 0 ? pendingLbs_ : 10.0;
				backoffMs_ = 0;
				gapMs_ = base_gap_ms( lbs_ );
				line_ = health_ = -1;
			} else {
				start_reeling( pendingLbs_, nowMs );
			}
		}
		return;
	}
	if ( !fishOn ) return; // results only count for a fish we were on

	if ( starts( s, "The line snapped" ) ) {
		++stats_.snapped;
		ready_in( nowMs, 1200, 2200 );
		return;
	}
	if ( starts( s, "The catch got away" ) ) {
		++stats_.gotAway;
		ready_in( nowMs, 1200, 2200 );
		return;
	}
	if ( starts( s, "You caught " ) && s.find( "Catch of the Day" ) == std::string::npos ) {
		if ( s.find( "rotten fish" ) != std::string::npos ) {
			++stats_.rotten;
			rottenWait_ = true;
			ready_in( nowMs, 11000, 12500 ); // stunned ~10 s
			return;
		}
		const size_t w = s.find( " weighing " );
		if ( w != std::string::npos ) {
			++stats_.caught;
			// "You caught a Sea Bass weighing 1.1lbs!" -> "Sea Bass", 1.1
			size_t a = 11;
			if ( s.compare( a, 3, "an " ) == 0 ) a += 3;
			else if ( s.compare( a, 2, "a " ) == 0 ) a += 2;
			const std::string name = s.substr( a, w > a ? w - a : 0 );
			const double lbs = std::strtod( s.c_str() + w + 10, nullptr );
			if ( lbs > 0 ) stats_.pounds += lbs;
			if ( lbs > stats_.biggest ) {
				stats_.biggest = lbs;
				stats_.biggestName = name;
			}
		} else {
			++stats_.other; // a wallet, a boot, ...
		}
		ready_in( nowMs, 900, 2000 );
	}
}

void Fisher::on_textdraw( const std::string &text, uint32_t nowMs ) {
	int line = 0, health = 0;
	double lbs = 0;
	if ( state_ == State::Off || state_ == State::Paused || !parse_stats( text, line, health, lbs ) ) return;
	if ( state_ == State::WaitBite && lbs > 0 && line == 100 ) start_reeling( lbs, nowMs ); // a bite (chat may lag)
	if ( state_ != State::Reeling ) return;
	if ( lbs > lbs_ ) { // heavier than we assumed (the weight line was missed): its pace
		lbs_ = lbs;
		gapMs_ = base_gap_ms( lbs_ ) + backoffMs_;
	}
	if ( health_ >= 0 && health < health_ ) { // the line took a hit: slower from now on
		backoffMs_ = backoffMs_ + kBackoffMs > kMaxBackoffMs ? kMaxBackoffMs : backoffMs_ + kBackoffMs;
		gapMs_ = base_gap_ms( lbs_ ) + backoffMs_;
		if ( presses_ ) nextAt_ = later( nextAt_, lastPressAt_ + gapMs_ );
	}
	line_ = line;
	health_ = health;
}

void Fisher::on_dialog_shown( uint32_t nowMs ) {
	if ( state_ == State::Off || state_ == State::Paused ) return;
	// A cast in progress can't be resumed half-way: start it over afterwards.
	resumeTo_ = ( state_ == State::Aiming || state_ == State::Throwing ) ? State::Ready : state_;
	keys_ = 0;
	state_ = State::Paused;
	stateAt_ = nowMs;
}

void Fisher::on_dialog_closed( uint32_t nowMs ) {
	if ( state_ != State::Paused ) return;
	state_ = resumeTo_;
	stateAt_ = nowMs;
	keys_ = 0;
	switch ( state_ ) {
	case State::Ready: case State::NoRod:
		nextAt_ = nowMs + rand_between( 800, 1500 );
		break;
	case State::Cooldown: // never shorter than the break it was on (a rotten fish's stun)
		nextAt_ = later( nextAt_, nowMs + rand_between( 800, 1500 ) );
		break;
	case State::Reeling:
		releaseAt_ = 0;
		nextAt_ = nowMs + rand_between( 300, 500 );
		break;
	case State::Casting:
		deadline_ = nowMs + kCastTimeoutMs;
		break;
	default:
		break;
	}
}

uint16_t Fisher::tick( uint32_t nowMs ) {
	const uint16_t before = keys_;
	const uint16_t keys = step( nowMs );
	// A hard cap on any hold, whatever the server does or doesn't say.
	if ( keys && !before ) keysSince_ = nowMs;
	if ( keys && due( nowMs, keysSince_ + kMaxHoldMs ) ) {
		if ( state_ == State::Aiming || state_ == State::Throwing ) cast_failed( nowMs, "a key was held too long" );
		else keys_ = 0;
		return keys_;
	}
	return keys;
}

uint16_t Fisher::step( uint32_t nowMs ) {
	switch ( state_ ) {
	case State::Off:
	case State::Paused:
		return keys_ = 0;

	case State::NoRod:
		keys_ = 0;
		if ( rodOut_ ) {
			state_ = State::Cooldown;
			stateAt_ = nowMs;
			nextAt_ = nowMs + rand_between( 400, 900 );
		}
		return 0;

	case State::Ready:
	case State::Cooldown:
		if ( !due( nowMs, nextAt_ ) ) return keys_ = 0;
		rottenWait_ = false;
		if ( !rodOut_ ) {
			go( State::NoRod, nowMs );
			return keys_ = 0;
		}
		keys_ = KEY_HANDBRAKE; // aim
		fireScheduled_ = releaseScheduled_ = false;
		go( State::Aiming, nowMs );
		deadline_ = nowMs + kAimTimeoutMs;
		return keys_;

	case State::Aiming:
		if ( fireScheduled_ ) {
			if ( due( nowMs, nextAt_ ) ) { // the throw started: FIRE, still aiming
				keys_ = KEY_HANDBRAKE | KEY_FIRE;
				releaseScheduled_ = false;
				go( State::Throwing, nowMs );
				deadline_ = nowMs + kThrowTimeoutMs;
			}
		} else if ( due( nowMs, deadline_ ) ) {
			cast_failed( nowMs, "aiming started no throw" );
		}
		return keys_;

	case State::Throwing:
		if ( releaseScheduled_ ) {
			if ( due( nowMs, releaseAt_ ) ) keys_ &= (uint16_t) ~KEY_FIRE;
			if ( due( nowMs, aimUpAt_ ) ) keys_ &= (uint16_t) ~KEY_HANDBRAKE;
			if ( keys_ == 0 ) {
				go( State::Casting, nowMs );
				deadline_ = nowMs + kCastTimeoutMs;
			}
		} else if ( due( nowMs, deadline_ ) ) {
			cast_failed( nowMs, "FIRE threw nothing" );
		}
		return keys_;

	case State::Casting:
		keys_ = 0;
		if ( due( nowMs, deadline_ ) ) cast_failed( nowMs, "the line didn't go out" );
		return 0;

	case State::WaitBite:
		keys_ = 0;
		if ( nowMs - stateAt_ >= kBiteTimeoutMs ) stop( "Stopped: no bite for 60 s." );
		return 0;

	case State::Reeling:
		if ( keys_ & KEY_SPRINT ) { // a press: let go after a finger-tap
			if ( due( nowMs, releaseAt_ ) ) keys_ = 0;
			return keys_;
		}
		if ( nowMs - stateAt_ >= kReelTimeoutMs ) {
			stop( "Stopped: reeling got no result for 90 s." );
			return 0;
		}
		if ( line_ == 0 ) return 0; // all in: the result is on its way
		if ( due( nowMs, nextAt_ ) ) {
			keys_ = KEY_SPRINT;
			lastPressAt_ = nowMs;
			++presses_;
			releaseAt_ = nowMs + rand_between( 70, 120 );
			nextAt_ = nowMs + gapMs_ * rand_between( 98, 106 ) / 100;
		}
		return keys_;
	}
	return keys_ = 0;
}

std::string Fisher::status( uint32_t nowMs ) const {
	char b[160];
	switch ( state_ ) {
	case State::Off:      return "off";
	case State::NoRod:    return "waiting for the rod -- type /fish";
	case State::Ready:    return "about to cast";
	case State::Aiming:
	case State::Throwing:
	case State::Casting:  return "casting";
	case State::WaitBite:
		std::snprintf( b, sizeof b, "waiting for a bite (%u s)", ( nowMs - stateAt_ ) / 1000 );
		return b;
	case State::Reeling:
		if ( line_ >= 0 && health_ >= 0 )
			std::snprintf( b, sizeof b, "reeling %.1f lbs -- line %d%%, health %d%%", lbs_, line_, health_ );
		else
			std::snprintf( b, sizeof b, "reeling %.1f lbs", lbs_ );
		return b;
	case State::Cooldown: {
		const uint32_t left = due( nowMs, nextAt_ ) ? 0 : nextAt_ - nowMs;
		std::snprintf( b, sizeof b, rottenWait_ ? "stunned by a rotten fish -- next cast in %.0f s" : "next cast in %.1f s",
		               left / 1000.0 );
		return b; }
	case State::Paused:   return "paused -- a dialog is open";
	}
	return "?";
}

bool Fisher::pop_notice( std::string &out ) {
	if ( notice_.empty() ) return false;
	out.swap( notice_ );
	notice_.clear();
	return true;
}

} // namespace fish
