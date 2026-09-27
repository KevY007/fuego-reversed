#include "detect.h"
#include <cwctype>
#include <algorithm>
#include <cctype>
#include <vector>

namespace i18n::detect {

Script script_of(const std::wstring& text) {
    int latin_count = 0, cyrillic_count = 0, other_count = 0;

    for (wchar_t ch : text) {
        // Latin: A-Z, a-z, and other Latin extended characters
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) {
            latin_count++;
        }
        // Cyrillic: U+0400 to U+04FF
        else if (ch >= 0x0400 && ch <= 0x04FF) {
            cyrillic_count++;
        }
        // Other letter-like characters
        else if (std::iswalpha(ch)) {
            other_count++;
        }
    }

    if (cyrillic_count > 0 && cyrillic_count >= latin_count && cyrillic_count >= other_count) {
        return Script::Cyrillic;
    } else if (latin_count > 0 && latin_count >= cyrillic_count && latin_count >= other_count) {
        return Script::Latin;
    } else {
        return Script::Other;
    }
}

bool is_probably_english(const std::wstring& text) {
    // Must be Latin script
    if (script_of(text) != Script::Latin) {
        return false;
    }

    // Must be mostly ASCII
    int ascii_count = 0, non_ascii_count = 0;
    for (wchar_t ch : text) {
        if (ch < 128) {
            ascii_count++;
        } else {
            non_ascii_count++;
        }
    }

    if (non_ascii_count > 0 && ascii_count < non_ascii_count * 2) {
        // Too many non-ASCII characters
        return false;
    }

    // Convert to lowercase for comparison
    std::wstring lower_text;
    for (wchar_t ch : text) {
        lower_text += std::towlower(ch);
    }

    // Common English stopwords
    const char* stopwords[] = {
        "the", "you", "is", "to", "and", "a", "i", "in", "that", "it",
        "of", "for", "on", "are", "as", "was", "be", "this", "by", "or",
        "an", "with", "at", "from", "he", "she", "we", "they", "what",
        "which", "who", "when", "where", "why", "how", "can", "will",
        "would", "could", "should", "may", "do", "does", "did", "have",
        "has", "had", "but", "not", "no", "yes", "if", "then", "else",
        "all", "each", "every", "both", "neither", "nor", "some", "most",
        "any", "many", "few", "more", "much", "get", "go", "make", "know",
        "think", "see", "say", "find", "give", "tell", "ask", "work",
        "need", "try", "use", "help", "turn", "show", "hear", "let",
        "mean", "leave", "seem", "feel", "want", "meet", "call", "talk"
    };

    // Check for stopwords
    int stopword_hits = 0;
    for (const char* word : stopwords) {
        std::wstring wword;
        for (char c : std::string(word)) {
            wword += (wchar_t)c;
        }

        // Simple word boundary check
        size_t pos = 0;
        while ((pos = lower_text.find(wword, pos)) != std::wstring::npos) {
            // Check word boundaries
            bool start_ok = (pos == 0 || !std::iswalpha(lower_text[pos - 1]));
            bool end_ok = (pos + wword.length() == lower_text.length() ||
                          !std::iswalpha(lower_text[pos + wword.length()]));

            if (start_ok && end_ok) {
                stopword_hits++;
                break;  // count each stopword once
            }
            pos++;
        }
    }

    // Consider it probably English if we found at least 2 stopwords
    return stopword_hits >= 2;
}

namespace {

// Common English interface / game words (lowercase). Deliberately LEFT OUT: words
// that mean something else in another Latin-script language players use, so a
// foreign label or one-word chat line made only of "English-looking" words is still
// sent for translation --
//   German   gift art also fast kind arm brand boot rat hut bad hell die was war man
//            hat tag rot gang(gear) last(load) fee(fairy)
//   French   chat(cat) or(gold) store(blind) location(rental) resume(summary)
//            car(coach/because) bail(lease)
//   Spanish  sale once pan red con sin solo fin real los/las(the)
//   Italian  come dove camera fine(end)                 Dutch map(folder)
//   Polish/Czech list(letter) ty(you) no(yeah) los(fate) Romanian an(year)
//   Indonesian ban(tyre)    Turkish sat(sell) al(take) at(horse)    Norwegian rent(clean)
//   Roman Urdu/Hindi main par bas log(people) to(so)    Russian translit net da by on(he)
const char* const kUiWords[] = {
    // buttons / actions
    "ok", "okay", "yes", "cancel", "close", "back", "next", "previous", "prev",
    "select", "choose", "confirm", "accept", "decline", "reject", "submit", "send",
    "continue", "done", "finish", "exit", "quit", "leave", "enter", "open", "save",
    "load", "delete", "remove", "edit", "change", "reset", "refresh", "return", "skip",
    "retry", "login", "logout", "sign", "register", "play", "spawn", "respawn",
    "start", "stop", "pause", "join", "invite", "kick", "mute",
    "unmute", "report", "help", "info", "information", "settings", "setting",
    "options", "option", "menu", "page", "more", "less", "show", "hide", "view",
    "search", "find", "go", "use", "drop", "give", "take", "get", "pick", "equip",
    "upgrade", "repair", "refill", "lock", "unlock", "buy", "sell", "purchase",
    "rental", "pay", "withdraw", "deposit", "transfer", "claim", "collect", "redeem",
    "vote", "apply", "create", "add", "set", "clear", "toggle", "enable", "disable",
    "enabled", "disabled", "off",
    // money / shops / property
    "balance", "account", "money", "cash", "bank", "price", "cost", "total", "amount",
    "tax", "taxes", "salary", "payday", "shop", "market", "item", "items",
    "inventory", "house", "houses", "home", "property", "properties", "business",
    "businesses", "apartment", "apartments", "hotel", "motel", "garage", "door", "doors",
    "key", "keys",
    // gear / vehicles
    "weapon", "weapons", "gun", "guns", "ammo", "armour", "armor", "health", "vehicle",
    "vehicles", "bike", "bikes", "boat", "boats", "plane", "planes",
    "bus", "taxi", "fuel", "engine", "lights", "trunk", "hood", "park", "parking",
    "impound", "tow", "tuning", "paint", "nitro", "wheels",
    // players / server / social
    "player", "players", "admin", "admins", "staff", "owner", "leader", "member",
    "members", "vip", "server", "rules", "rule", "commands", "command", "name",
    "username", "password", "email", "phone", "call", "message", "messages",
    "friend", "friends", "gangs", "faction", "factions", "family", "team",
    "teams", "clan", "clans", "rank", "ranks", "skin", "skins", "color", "colour",
    "colors", "online", "offline", "welcome", "hello", "hi", "thanks", "thank",
    "please", "sorry", "police", "cop", "cops", "medic", "mechanic", "hospital", "jail",
    "prison", "wanted", "fines",
    // game systems / places
    "job", "jobs", "work", "level", "levels", "score", "stats", "statistics", "status",
    "skill", "skills", "experience", "exp", "xp", "respect", "points", "point", "reward",
    "rewards", "bonus", "daily", "weekly", "event", "events", "mission", "missions",
    "quest", "quests", "race", "races", "lottery", "casino", "license", "licence",
    "licenses", "driving", "flying", "fishing", "mining", "delivery", "time", "weather",
    "hours", "minutes", "seconds", "day", "days", "gps",
    "teleport", "destination", "closest", "nearest", "zone", "zones",
    "area", "city", "interior", "general", "tutorial", "guide", "faq", "support",
    "discord", "forum", "website", "achievement", "achievements", "topic", "topics",
    "system", "overview", "first", "new", "old", "other", "all", "none",
    "santos", "san", "fierro", "venturas", "andreas",
    // glue words that only ever pass when EVERY other word is on this list too
    "my", "your", "the", "a", "of", "for", "and", "in", "with", "from",
    // universal gaming chat
    "lol", "lmao", "rofl", "gg", "wp", "brb", "afk", "np", "thx", "xd", "omg",
    "haha", "hahaha", "hehe", "kk",
};

bool is_ui_word(const std::string& w) {
    for (const char* u : kUiWords)
        if (w == u) return true;
    return false;
}

bool is_hex(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

} // namespace

bool is_english_ui_text(const std::wstring& text) {
    std::vector<std::string> words;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c >= 128) return false; // pure ASCII only: an accented/Cyrillic letter is not English UI text
        if (c == L'{' && i + 7 < text.size() && text[i + 7] == L'}') { // {RRGGBB} colour code
            bool hex = true;
            for (size_t k = 1; k <= 6; ++k) hex = hex && is_hex(text[i + k]);
            if (hex) {
                if (!cur.empty()) { words.push_back(cur); cur.clear(); }
                i += 7;
                continue;
            }
        }
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))
            cur += (char)std::tolower((int)c);
        else if (!cur.empty()) {
            words.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty() || words.size() > 6) return false;
    for (const auto& w : words)
        if (!is_ui_word(w)) return false;
    return true;
}

bool worth_translating(const std::wstring& text, bool targetIsEnglish) {
    // Skip if empty
    if (text.empty()) {
        return false;
    }

    // Check if already English and target is English (a sentence with stopwords, or
    // a short UI label made only of common English interface words)
    if (targetIsEnglish && (is_probably_english(text) || is_english_ui_text(text))) {
        return false;
    }

    // Skip if all digits and punctuation
    bool has_letter = false;
    for (wchar_t ch : text) {
        if (std::iswalpha(ch)) {
            has_letter = true;
            break;
        }
    }

    if (!has_letter) {
        return false;
    }

    return true;
}

}
