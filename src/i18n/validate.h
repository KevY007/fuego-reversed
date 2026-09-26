#pragma once
#include <string>

namespace i18n::validate {

// Result validation: checks that the translation model's output is sensible.
// Returns true iff:
//   1. Placeholders are intact (none lost, duplicated, or corrupted)
//   2. Output is non-empty
//   3. Output length is not wildly larger (e.g. <= 4x source length)
bool result_ok(const std::string& maskedSource, const std::string& modelOutput, size_t tokenCount);

} // namespace i18n::validate
