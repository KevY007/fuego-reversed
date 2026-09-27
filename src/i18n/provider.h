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
    // Sampling. temp 0.3 is the sweet spot found in live testing: Hy-MT2 stays
    // coherent and NEVER loops (its runaway only happens at temperature 0/greedy),
    // while general models (gemma3, ...) are far more accurate + consistent at 0.3
    // than at 0.7 (which made gemma echo the source / drift). Configurable via the
    // menu (config.translate_temp). maxTokens caps output -> bounds latency + stops
    // any runaway. (top_k / repeat_penalty aren't honoured by the OpenAI /v1 endpoint.)
    double temperature = 0.3;
    double topP = 0.6;
    int maxTokens = 512;
    // Minimum gap (ms) between outgoing requests, enforced globally across worker
    // threads (mirrors config::Settings::request_min_gap_ms). 0 = no pacing. Used
    // to stay under a cloud provider's RPM limit (free tiers). Separate from the
    // automatic 429/503 backoff-retry, which always runs.
    int minRequestGapMs = 0;
    // Provider preset (mirrors config::Settings::provider_kind) -- selects
    // per-provider request quirks discovered by live testing against each API:
    //   0 Custom / 4 Ollama-Local : permissive -> send top_p, no special headers.
    //   1 Google (Gemini)         : permissive -> same as above (accepts anything).
    //   2 Anthropic (Claude)      : REJECTS temperature+top_p together (HTTP 400)
    //                               -> omit top_p; also needs an "anthropic-version"
    //                               header on both /chat/completions and /models.
    //   3 OpenAI                  : REJECTS reasoning_effort (HTTP 400) -> we never
    //                               send it anyway; top_p omitted for cloud.
    // (reasoning_effort is no longer sent for ANY provider: OpenAI 400s on it,
    //  Ollama silently ignores it, and chain-of-thought is handled by strip_think
    //  plus recommending non-thinking models.)
    int providerKind = 0;
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
