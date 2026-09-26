#pragma once
#include <string>
#include <cstdint>

namespace i18n::cache {
    /// Generate FNV-1a hash key from normText, srcLang, tgtLang, providerProfile
    uint64_t key(const std::string& normText, int srcLang, int tgtLang, const std::string& providerProfile);

    /// Retrieve cached translation; returns false if not found
    bool get(uint64_t k, std::string& out);

    /// Cache translation: updates in-memory map and appends key\tvalue to disk
    void put(uint64_t k, const std::string& value);

    /// Load cached translations from flat-file TM
    void load(const char* path);

    /// Set the path for put() to append to
    void save_path(const char* path);

    /// Clear in-memory map (test-only)
    void clear_for_test();
}
