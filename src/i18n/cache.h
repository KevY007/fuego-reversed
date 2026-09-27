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

    /// One-time move of the TM from an old file name to a new one, leaving one file
    /// and no leftovers. Call before load(newPath). An old file WITH the v2 header
    /// holds good entries: it is renamed to newPath, or -- when newPath already exists
    /// (e.g. an earlier rename failed and load() then started a fresh file) -- MERGED
    /// into it (the new file's entry wins on a clash) and deleted only after the
    /// merged file is safely in place. An old file WITHOUT the header (master's --
    /// its entries were damaged by the old cap bug) is deleted; so is
    /// "<oldPath>.v1.bak" (only ever held those damaged entries). If a rename or
    /// merge fails, the old file stays and the next start tries again. No-op when
    /// there is nothing old. Returns what it did, for the caller to log.
    struct Migration { bool moved = false; bool merged = false; bool removedOld = false; bool removedBak = false; };
    Migration migrate_legacy(const char* oldPath, const char* newPath);

    /// Clear in-memory map (test-only)
    void clear_for_test();
}
