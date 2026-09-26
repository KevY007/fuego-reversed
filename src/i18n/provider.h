#pragma once
#include <string>
#include <vector>
#include <memory>

// WinHTTP-free by design: Task 9's host engine test includes this header and
// defines a FakeProvider : IProvider without linking WinHTTP. All WinHTTP
// code lives in provider.cpp only -- do not add <windows.h>/<winhttp.h> here.

namespace i18n {

struct ProviderConfig {
    std::string baseUrl;   // e.g. "http://127.0.0.1:11434/v1" (ends in /v1, no trailing slash)
    std::string apiKey;    // sent as "Authorization: Bearer <apiKey>" if non-empty
    std::string model;
    int connectMs = 2000;
    // Generous by default: a LOCAL LLM cold-loads the model on the first request
    // (several seconds to tens of seconds) and even warm inference of a chat line
    // can take a few seconds on CPU / a small GPU. 2.5s was far too short and made
    // every call time out. Overridable per-config (menu: Response timeout).
    int readMs = 30000;
    // Sampling. Hy-MT2's recommended params: temp 0.7, top_p 0.6. IMPORTANT: at
    // temperature 0 (greedy) Hy-MT2 runs away/loops (verified), so never send 0.
    // maxTokens caps output -> bounds latency and stops any runaway generation.
    // (top_k / repeat_penalty aren't honoured by the OpenAI /v1 endpoint, so we
    // rely on temp + top_p + the maxTokens cap.)
    double temperature = 0.7;
    double topP = 0.6;
    int maxTokens = 512;
};

struct IProvider {
    virtual ~IProvider() = default;

    // Translates `units` (already masked/protected source text) as a batch.
    // On success, fills `out` with one translation per input unit (same
    // order/size as `units`) and returns true. On any failure (network,
    // non-200, malformed reply, open circuit breaker), returns false and
    // sets `err` to a short reason; `out` is left empty.
    virtual bool translate(const std::vector<std::string>& units, const std::string& systemPrompt,
                            const std::string& rollingCtx, std::vector<std::string>& out, std::string& err) = 0;

    virtual const char* name() const = 0;
};

// A provider that speaks the OpenAI /v1/chat/completions wire format --
// works against a local Ollama instance or any OpenAI-compatible cloud
// endpoint. Implemented with WinHTTP in provider.cpp.
std::unique_ptr<IProvider> make_openai_compatible(const ProviderConfig& config);

// Fetch the list of model ids the endpoint advertises via GET <baseUrl>/models
// (the OpenAI-compatible endpoint every target here exposes -- Ollama, OpenAI,
// DeepSeek, ... -- returning {"data":[{"id":"..."},...]}). Fills `out` with the
// ids and returns true; on any failure returns false and sets `err`. Blocking
// (WinHTTP) -- call it off the main thread. Used by the menu's model dropdown.
bool list_models(const ProviderConfig& config, std::vector<std::string>& out, std::string& err);

} // namespace i18n
