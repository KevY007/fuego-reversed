#include "batch.h"

namespace i18n::batch {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// "<marker> <text>" -> number + text. False if the line doesn't start with a marker.
bool parse_line(const std::string& line, size_t& num, std::string& text) {
    size_t i = 0;
    const bool bracket = (i < line.size() && line[i] == '[');
    if (bracket) ++i;
    const size_t digits = i;
    num = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        if (i - digits >= 4) return false; // no list is that long
        num = num * 10 + (size_t)(line[i] - '0');
        ++i;
    }
    if (i == digits) return false; // no number
    if (bracket) {
        if (i >= line.size() || line[i] != ']') return false;
        ++i;
    } else {
        if (i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':')) return false;
        ++i;
    }
    text = trim(line.substr(i));
    return true;
}

} // namespace

std::string numbered_block(const std::vector<std::string>& items) {
    std::string s;
    for (size_t i = 0; i < items.size(); ++i) {
        s += std::to_string(i + 1);
        s += ". ";
        s += items[i];
        s += '\n';
    }
    return s;
}

std::string batch_instruction(size_t n) {
    const std::string count = std::to_string(n);
    return "The text below is a numbered list of " + count +
           " separate lines from one SA-MP game menu or dialog (a title, buttons, list rows or table "
           "cells). Translate each line on its own and reply with exactly " + count +
           " lines in the same order, each starting with its number and a period, like the input. "
           "Never merge, split, drop, reorder or add lines.";
}

bool parse_numbered(const std::string& reply, size_t n, std::vector<std::string>& out) {
    out.clear();
    if (n == 0) return false;
    std::vector<std::string> items;
    size_t pos = 0;
    while (pos <= reply.size()) {
        size_t nl = reply.find('\n', pos);
        if (nl == std::string::npos) nl = reply.size();
        const std::string line = trim(reply.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        size_t num = 0;
        std::string text;
        if (!parse_line(line, num, text) || num != items.size() + 1) return false;
        items.push_back(std::move(text));
        if (items.size() > n) return false;
    }
    if (items.size() != n) return false;
    out = std::move(items);
    return true;
}

} // namespace i18n::batch
