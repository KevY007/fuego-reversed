#include "i18n/provider.h"
#include "i18n/breaker.h"
#include "i18n/json_min.h"
#include "i18n/url_util.h"

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace i18n {

namespace {

// ---- small helpers --------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring w(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), needed);
    return w;
}

// RAII wrapper for WinHTTP handles so nothing leaks on an early return.
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
};

// Trims ASCII whitespace from both ends.
std::string trim_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strips a leading ```/```lang fence and a trailing ``` fence (models often wrap
// JSON that way), returning the inner text trimmed.
std::string strip_code_fences(const std::string& in) {
    std::string s = trim_ws(in);
    if (s.size() >= 3 && s.compare(0, 3, "```") == 0) {
        size_t nl = s.find('\n');
        s = (nl != std::string::npos) ? s.substr(nl + 1) : s.substr(3);
        size_t f = s.rfind("```");
        if (f != std::string::npos) s = s.substr(0, f);
        s = trim_ws(s);
    }
    return s;
}

// Some "thinking" models (Qwen3, DeepSeek-R1, ...) prefix the answer with a
// <think>...</think> chain-of-thought block. Drop everything up to and including the
// closing tag so only the actual translation remains. (No tag -> returned unchanged;
// note some models emit untagged reasoning, which can't be stripped -- use a
// non-thinking model for those.)
std::string strip_think(const std::string& in) {
    size_t end = in.find("</think>");
    if (end != std::string::npos) return in.substr(end + 8);
    // A stray opening tag with no close (rare) -> nothing usable.
    if (in.find("<think>") != std::string::npos) return std::string();
    return in;
}

// Trims whitespace, then one matching pair of surrounding quotes.
std::string trim_ws_quotes(const std::string& in) {
    std::string s = trim_ws(in);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// Formats a double with a '.' decimal point regardless of C locale (the app
// never setlocale()s, so the default "C" locale already uses '.', but be
// explicit) for embedding in the JSON request body.
std::string fmt_double(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", d);
    return b;
}

// Extracts model ids from an OpenAI-style /models reply
// ({"data":[{"id":"..."},...]}). A targeted scan for "id" string values --
// robust enough for the /models shape (the only "id" keys there are the model
// ids) without pulling in a full JSON parser.
void parse_model_ids(const std::string& body, std::vector<std::string>& out) {
    const std::string key = "\"id\"";
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t pos = 0;
    while ((pos = body.find(key, pos)) != std::string::npos) {
        pos += key.size();
        while (pos < body.size() && is_ws(body[pos])) ++pos;
        if (pos >= body.size() || body[pos] != ':') continue;
        ++pos;
        while (pos < body.size() && is_ws(body[pos])) ++pos;
        if (pos >= body.size() || body[pos] != '"') continue;
        ++pos; // opening quote
        std::string id;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                char n = body[pos + 1];
                if (n == '"' || n == '\\' || n == '/') { id += n; pos += 2; continue; }
                if (n == 'n') { id += '\n'; pos += 2; continue; }
                if (n == 't') { id += '\t'; pos += 2; continue; }
            }
            id += body[pos];
            ++pos;
        }
        if (!id.empty()) out.push_back(id);
        if (pos < body.size()) ++pos; // step past the closing quote
    }
}

// ---- provider ---------------------------------------------------------

class OpenAiCompatibleProvider : public IProvider {
public:
    explicit OpenAiCompatibleProvider(const ProviderConfig& cfg) : cfg_(cfg) {}

    const char* name() const override { return "openai-compatible"; }

    bool translate(const std::vector<std::string>& units, const std::string& systemPrompt,
                   const std::string& rollingCtx, std::vector<std::string>& out, std::string& err) override {
        out.clear();
        err.clear();

        unsigned long now = GetTickCount();
        if (!breaker_.allow(now)) {
            err = "circuit open";
            return false;
        }

        bool ok = do_translate(units, systemPrompt, rollingCtx, out, err);
        if (ok) {
            breaker_.on_success();
        } else {
            breaker_.on_failure(GetTickCount());
            out.clear();
        }
        return ok;
    }

private:
    ProviderConfig cfg_;
    Breaker breaker_; // K=3, T=15000ms defaults (see breaker.h)

    // Builds the OpenAI-style chat request body as a NATIVE single-message
    // translation prompt: `instruction` (the translate directive + do-not-touch
    // rules from config::build_system_prompt) as the user message, the text to
    // translate after a blank line, optionally preceded by rolling context. No
    // system role -- Hy-MT2 has no default system prompt and dedicated MT models
    // translate a single user turn; a "return JSON" instruction just makes them
    // parrot the example, so we ask for plain text and parse plain text back.
    std::string build_request_body(const std::vector<std::string>& units, const std::string& instruction,
                                    const std::string& rollingCtx) const {
        std::string userContent;
        if (!rollingCtx.empty()) { userContent += rollingCtx; userContent += "\n"; }
        userContent += instruction;
        userContent += "\n\n";
        if (units.size() == 1) {
            userContent += units[0];
        } else {
            // Multi-unit batching isn't used by the chat path; number them so a
            // future caller at least gets a deterministic layout.
            for (size_t i = 0; i < units.size(); ++i)
                userContent += "[" + std::to_string(i) + "] " + units[i] + "\n";
        }

        std::string body = "{";
        body += "\"model\":\"" + json::escape(cfg_.model) + "\",";
        body += "\"temperature\":" + fmt_double(cfg_.temperature) + ",";
        body += "\"top_p\":" + fmt_double(cfg_.topP) + ",";
        body += "\"max_tokens\":" + std::to_string(cfg_.maxTokens) + ",";
        body += "\"stream\":false,";
        // Suppress chain-of-thought on reasoning-capable OpenAI models; Ollama
        // and MT models ignore it harmlessly.
        body += "\"reasoning_effort\":\"none\",";
        body += "\"messages\":[";
        body += "{\"role\":\"user\",\"content\":\"" + json::escape(userContent) + "\"}";
        body += "]}";
        return body;
    }

    bool do_translate(const std::vector<std::string>& units, const std::string& systemPrompt,
                       const std::string& rollingCtx, std::vector<std::string>& out, std::string& err) {
        std::string body = build_request_body(units, systemPrompt, rollingCtx);

        // baseUrl should already end in /v1, but tolerate a stray trailing
        // slash (a common config typo, e.g. "http://127.0.0.1:11434/v1/")
        // by normalizing the seam through join_url instead of naive
        // concatenation -- a doubled '/' 404s on many servers.
        std::wstring url = widen(join_url(cfg_.baseUrl, "/chat/completions"));

        URL_COMPONENTS uc{};
        wchar_t hostBuf[256] = {};
        wchar_t pathBuf[2048] = {};
        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = hostBuf;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = pathBuf;
        uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
            err = "invalid baseUrl";
            return false;
        }
        bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        WinHttpHandle hSession(WinHttpOpen(L"fuego-i18n/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!hSession.h) {
            err = "WinHttpOpen failed";
            return false;
        }
        if (!WinHttpSetTimeouts(hSession, cfg_.connectMs, cfg_.connectMs, cfg_.readMs, cfg_.readMs)) {
            // If this silently fails, WinHTTP falls back to its own (much
            // longer) default timeouts -- a worker could then stall well past
            // what the breaker's T assumes. Treat it like every other WinHTTP
            // call here: a hard failure of translate().
            err = "WinHttpSetTimeouts failed";
            return false;
        }

        WinHttpHandle hConnect(WinHttpConnect(hSession, hostBuf, uc.nPort, 0));
        if (!hConnect.h) {
            err = "WinHttpConnect failed";
            return false;
        }

        DWORD reqFlags = https ? WINHTTP_FLAG_SECURE : 0;
        WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"POST", pathBuf, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags));
        if (!hRequest.h) {
            err = "WinHttpOpenRequest failed";
            return false;
        }

        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!cfg_.apiKey.empty()) {
            headers += L"Authorization: Bearer " + widen(cfg_.apiKey) + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
                                        body.empty() ? nullptr : (LPVOID)body.data(), (DWORD)body.size(),
                                        (DWORD)body.size(), 0);
        if (!sent) {
            err = "WinHttpSendRequest failed";
            return false;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            err = "WinHttpReceiveResponse failed";
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200) {
            err = "HTTP status " + std::to_string(statusCode);
            return false;
        }

        std::string responseBody;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                err = "WinHttpQueryDataAvailable failed";
                return false;
            }
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) {
                err = "WinHttpReadData failed";
                return false;
            }
            chunk.resize(read);
            responseBody += chunk;
        }

        std::string content;
        if (!json::extract_message_content(responseBody, content)) {
            err = "failed to extract message content from reply";
            return false;
        }

        // Native MT reply is plain text -- drop any <think> block, then strip code
        // fences / surrounding quotes the model may have added around it.
        std::string translation = trim_ws_quotes(strip_code_fences(strip_think(content)));
        if (translation.empty()) {
            err = "empty translation in reply";
            return false;
        }

        // The chat path always sends exactly one unit; put the reply there.
        // (A multi-unit batch can't be split out of one plain-text reply, and
        // nothing uses multi-unit batching, so out[0] is what the engine reads.)
        out.assign(units.size(), std::string());
        if (!out.empty()) out[0] = translation;
        return true;
    }
};

} // namespace

std::unique_ptr<IProvider> make_openai_compatible(const ProviderConfig& config) {
    return std::make_unique<OpenAiCompatibleProvider>(config);
}

bool list_models(const ProviderConfig& config, std::vector<std::string>& out, std::string& err) {
    out.clear();
    err.clear();

    // baseUrl already ends in /v1; GET /v1/models is the OpenAI-standard list
    // endpoint (Ollama, OpenAI, DeepSeek, Groq, OpenRouter, LM Studio, ... and
    // Anthropic via its OpenAI-compat base URL all expose it in the same shape).
    std::wstring url = widen(join_url(config.baseUrl, "/models"));

    URL_COMPONENTS uc{};
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        err = "invalid baseUrl";
        return false;
    }
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    WinHttpHandle hSession(WinHttpOpen(L"fuego-i18n/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession.h) {
        err = "WinHttpOpen failed";
        return false;
    }
    // A menu convenience (off the frame path), so slightly more generous than a
    // translate call but still bounded: 1s connect, 4s read.
    WinHttpSetTimeouts(hSession, 1000, 1000, 4000, 4000);

    WinHttpHandle hConnect(WinHttpConnect(hSession, hostBuf, uc.nPort, 0));
    if (!hConnect.h) {
        err = "WinHttpConnect failed";
        return false;
    }

    DWORD reqFlags = https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"GET", pathBuf, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags));
    if (!hRequest.h) {
        err = "WinHttpOpenRequest failed";
        return false;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!config.apiKey.empty()) {
        headers += L"Authorization: Bearer " + widen(config.apiKey) + L"\r\n";
    }

    if (!WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = "WinHttpSendRequest failed";
        return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        err = "WinHttpReceiveResponse failed";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        err = "HTTP status " + std::to_string(statusCode);
        return false;
    }

    std::string responseBody;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            err = "WinHttpQueryDataAvailable failed";
            return false;
        }
        if (avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) {
            err = "WinHttpReadData failed";
            return false;
        }
        chunk.resize(read);
        responseBody += chunk;
    }

    parse_model_ids(responseBody, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) {
        err = "no models found in reply";
        return false;
    }
    return true;
}

} // namespace i18n
