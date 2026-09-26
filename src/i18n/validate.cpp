#include "i18n/validate.h"
#include "i18n/protect.h"

namespace i18n::validate {

bool result_ok(const std::string& maskedSource, const std::string& modelOutput, size_t tokenCount) {
    // Check 1: Placeholders intact
    if (!protect::placeholders_intact(modelOutput, tokenCount)) {
        return false;
    }

    // Check 2: Output non-empty
    if (modelOutput.empty()) {
        return false;
    }

    // Check 3: Output not wildly longer (output length <= 4x source length)
    if (modelOutput.length() > 4 * maskedSource.length()) {
        return false;
    }

    return true;
}

} // namespace i18n::validate
