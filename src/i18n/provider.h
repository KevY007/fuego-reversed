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
    int connectMs = 300;
    int readMs = 2500;
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

} // namespace i18n
