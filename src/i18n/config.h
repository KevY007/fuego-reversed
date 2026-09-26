#pragma once
#include <string>

namespace i18n::config {

struct Settings {
    bool inbound_chat = false;
    int target_lang = 1033;  // LANG_ENGLISH default
    int source_cp = 0;       // 0 = auto-detect
    std::string base_url = "http://127.0.0.1:11434/v1";
    std::string api_key;     // empty = no key (local Ollama)
    std::string model = "hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M";
    int marker_style = 0;
};

Settings& get();
void load();
void load(const char* path);
void save();
void save(const char* path);
std::string build_system_prompt(int targetLang);

}
