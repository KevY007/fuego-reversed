#pragma once
#include <string>
#include <cstdint>

namespace i18n::cache {
    /// Generate FNV-1a hash key from normText, srcLang, tgtLang, providerProfile
    uint64_t key(const std::string& normText, int srcLang, int tgtLang, const std::string& providerProfile);

    /// Retrieve cached translation; returns false if not found
    bool get(uint64_t k, std::string& out);

    /// Cache translation: updates in-memory map and appends key\tvalue to disk.
    /// An empty value is ignored (never stored, never a hit).
    void put(uint64_t k, const std::string& value);

    /// Load cached translations from the flat-file TM, whose first line is the
    /// header "#fuego-tm 2". A file that exists but lacks that header predates the
    /// format (master stored empty/truncated values): it is renamed to
    /// "<path>.v1.bak" (replacing an older backup) and the cache starts empty. A
    /// missing (or just-renamed) file is created holding only the header. Empty
    /// values are skipped. The user's old file is never deleted -- the rename is
    /// the backup.
    void load(const char* path);

    /// Set the path for put() to append to
    void save_path(const char* path);

    /// Clear in-memory map (test-only)
    void clear_for_test();
}
