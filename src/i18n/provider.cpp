#include "i18n/provider.h"
#include "i18n/breaker.h"
#include "i18n/json_min.h"
#include "i18n/url_util.h"

#include <windows.h>
#include <winhttp.h>

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

    // Builds the OpenAI-style chat request body.
    std::string build_request_body(const std::vector<std::string>& units, const std::string& systemPrompt,
                                    const std::string& rollingCtx) const {
        std::string userContent = rollingCtx + "\n" + json::build_units(units);

        std::string body = "{";
        body += "\"model\":\"" + json::escape(cfg_.model) + "\",";
        body += "\"temperature\":0,";
        body += "\"stream\":false,";
        // Disables extended thinking/reasoning output so the reply is just the
        // translated JSON, not a chain-of-thought preamble. Ollama / most
        // OpenAI-compatible servers accept "reasoning_effort":"none"; some
        // providers use a different key (e.g. "think":false) instead -- this
        // is the single place to change if a target provider needs that.
        body += "\"reasoning_effort\":\"none\",";
        body += "\"messages\":[";
        body += "{\"role\":\"system\",\"content\":\"" + json::escape(systemPrompt) + "\"},";
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

        std::vector<json::Unit> parsed;
        if (!json::parse_units(content, parsed)) {
            err = "failed to parse translated units from reply";
            return false;
        }

        // Fill by id so out[i] always corresponds to units[i], regardless of
        // the order the model emitted them in.
        out.assign(units.size(), std::string());
        std::vector<bool> filled(units.size(), false);
        for (const auto& u : parsed) {
            if (u.id >= 0 && (size_t)u.id < out.size()) {
                out[(size_t)u.id] = u.text;
                filled[(size_t)u.id] = true;
            }
        }
        for (size_t i = 0; i < filled.size(); ++i) {
            if (!filled[i]) {
                err = "reply missing unit id " + std::to_string(i);
                return false;
            }
        }

        return true;
    }
};

} // namespace

std::unique_ptr<IProvider> make_openai_compatible(const ProviderConfig& config) {
    return std::make_unique<OpenAiCompatibleProvider>(config);
}

} // namespace i18n
