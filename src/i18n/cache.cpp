#include "cache.h"
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace i18n::cache {
    // FNV-1a parameters
    constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
    constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;

    // Global cache state (protected by mutex)
    static std::unordered_map<uint64_t, std::string> g_map;
    static std::mutex g_mutex;
    static std::string g_save_path = "fuego_i18n_tm.txt";

    /// FNV-1a hash for a single buffer
    static uint64_t fnv1a(const uint8_t* data, size_t len) {
        uint64_t h = FNV_OFFSET;
        for (size_t i = 0; i < len; ++i) {
            h ^= data[i];
            h *= FNV_PRIME;
        }
        return h;
    }

    /// Combine hashes using FNV-1a
    static uint64_t fnv1a_combine(uint64_t h, const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            h ^= data[i];
            h *= FNV_PRIME;
        }
        return h;
    }

    uint64_t key(const std::string& normText, int srcLang, int tgtLang, const std::string& providerProfile) {
        uint64_t h = FNV_OFFSET;

        // Hash normText
        h = fnv1a_combine(h, (const uint8_t*)normText.data(), normText.size());

        // Hash srcLang
        uint8_t src_bytes[4];
        src_bytes[0] = (srcLang >> 0) & 0xFF;
        src_bytes[1] = (srcLang >> 8) & 0xFF;
        src_bytes[2] = (srcLang >> 16) & 0xFF;
        src_bytes[3] = (srcLang >> 24) & 0xFF;
        h = fnv1a_combine(h, src_bytes, 4);

        // Hash tgtLang
        uint8_t tgt_bytes[4];
        tgt_bytes[0] = (tgtLang >> 0) & 0xFF;
        tgt_bytes[1] = (tgtLang >> 8) & 0xFF;
        tgt_bytes[2] = (tgtLang >> 16) & 0xFF;
        tgt_bytes[3] = (tgtLang >> 24) & 0xFF;
        h = fnv1a_combine(h, tgt_bytes, 4);

        // Hash providerProfile
        h = fnv1a_combine(h, (const uint8_t*)providerProfile.data(), providerProfile.size());

        return h;
    }

    bool get(uint64_t k, std::string& out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_map.find(k);
        if (it == g_map.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    /// Escape newlines, tabs, and backslashes in value for flat-file storage
    static std::string escape_value(const std::string& value) {
        std::string result;
        for (char c : value) {
            if (c == '\n') {
                result += "\\n";
            } else if (c == '\t') {
                result += "\\t";
            } else if (c == '\\') {
                result += "\\\\";
            } else {
                result += c;
            }
        }
        return result;
    }

    /// Unescape value read from flat-file
    static std::string unescape_value(const std::string& escaped) {
        std::string result;
        for (size_t i = 0; i < escaped.size(); ++i) {
            if (escaped[i] == '\\' && i + 1 < escaped.size()) {
                char next = escaped[i + 1];
                if (next == 'n') {
                    result += '\n';
                    ++i;
                } else if (next == 't') {
                    result += '\t';
                    ++i;
                } else if (next == '\\') {
                    result += '\\';
                    ++i;
                } else {
                    result += escaped[i];
                }
            } else {
                result += escaped[i];
            }
        }
        return result;
    }

    void put(uint64_t k, const std::string& value) {
        std::lock_guard<std::mutex> lock(g_mutex);

        // Update in-memory map
        g_map[k] = value;

        // Append to disk (under lock to prevent data race on g_save_path and file interleaving)
        std::ofstream ofs(g_save_path, std::ios_base::app);
        if (ofs.is_open()) {
            ofs << k << '\t' << escape_value(value) << '\n';
            ofs.close();
        }
    }

    void load(const char* path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            // File doesn't exist; just return
            return;
        }

        std::string line;
        while (std::getline(ifs, line)) {
            // Parse key\tvalue
            size_t tab_pos = line.find('\t');
            if (tab_pos == std::string::npos) {
                // Malformed line; skip
                continue;
            }

            std::string key_str = line.substr(0, tab_pos);
            std::string value_str = line.substr(tab_pos + 1);

            // Parse key as uint64_t
            char* end;
            uint64_t k = std::strtoull(key_str.c_str(), &end, 10);
            if (*end != '\0') {
                // Failed to parse key; skip
                continue;
            }

            // Unescape value and store
            g_map[k] = unescape_value(value_str);
        }
        ifs.close();
    }

    void save_path(const char* path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_save_path = path;
    }

    void clear_for_test() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_map.clear();
    }
}
