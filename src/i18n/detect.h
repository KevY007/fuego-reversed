#ifndef FUEGO_I18N_DETECT_H
#define FUEGO_I18N_DETECT_H

#include <string>

namespace i18n::detect {

enum class Script { Latin, Cyrillic, Other };

// Analyze script composition of the text (Latin/Cyrillic/Other)
Script script_of(const std::wstring& text);

// Coarse check: Latin script + mostly ASCII + hits common English stopwords
bool is_probably_english(const std::wstring& text);

// Skip if empty, all digits/punct, or (English && target is English)
bool worth_translating(const std::wstring& text, bool targetIsEnglish);

}

#endif // FUEGO_I18N_DETECT_H
