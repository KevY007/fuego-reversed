#ifndef FUEGO_I18N_DETECT_H
#define FUEGO_I18N_DETECT_H

#include <string>

namespace i18n::detect {

enum class Script { Latin, Cyrillic, Other };

// Analyze script composition of the text (Latin/Cyrillic/Other)
Script script_of(const std::wstring& text);

// Coarse check: Latin script + mostly ASCII + hits common English stopwords
bool is_probably_english(const std::wstring& text);

// Short English UI text: pure ASCII, 1-6 words, and EVERY word on a curated list of
// common English interface/game words ("Select", "Log in", "Withdraw", "Bank of San
// Andreas"). Catches the buttons, titles and list cells the stopword test misses.
// The list leaves out words that mean something else in another Latin-script
// language the game is played in (German "Gift", Roman Urdu "main", Spanish
// "sale"), so a foreign label is never mistaken for English. {RRGGBB} colour codes,
// digits and punctuation separate words and are not words themselves.
bool is_english_ui_text(const std::wstring& text);

// Skip if empty, all digits/punct, or (English && target is English) -- "English"
// meaning is_probably_english() OR is_english_ui_text().
bool worth_translating(const std::wstring& text, bool targetIsEnglish);

}

#endif // FUEGO_I18N_DETECT_H
