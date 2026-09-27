#pragma once
#include <string>
#include <vector>

// Batched translation wire format (pure, host-tested in tests/i18n_tests.cpp).
//
// Several short texts (the cells of one dialog) go to the model in ONE request
// as a numbered list, and the reply is split back into items. The parser is
// STRICT: a reply is accepted only if it holds exactly the numbers 1..n, once
// each, in order, one per line. Anything else (a merged, split, dropped, extra or
// reordered line, a chatty preamble) is rejected as a whole, so a batch can never
// put one item's translation on another item's row. The caller then falls back
// to translating the items one at a time.

namespace i18n::batch {

// "[1] first\n[2] second\n..." -- the list the model is asked to translate.
std::string numbered_block(const std::vector<std::string>& items);

// The instruction added after the normal translation prompt for an n-item list.
std::string batch_instruction(size_t n);

// Split a numbered-list reply back into exactly n items (see the header comment).
// Accepted markers: "1." "1)" "1:" "[1]", after optional spaces; blank lines and
// CRLF are ignored; a marker with nothing after it gives an empty item. Returns
// false and leaves `out` empty on any mismatch.
bool parse_numbered(const std::string& reply, size_t n, std::vector<std::string>& out);

} // namespace i18n::batch
