#include "xhr_bridge.hpp"

#include "notify.hpp"

#include <windows.h>
#include <winhttp.h>
#include <objidl.h>
#include <msxml6.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod*          g_mod = nullptr;
BridgeConfig     g_cfg;

constexpr size_t kMaxBody = 64u * 1024 * 1024;

// The OfflineRequest vtable and the RVA each of its 13 slots points at (IUnknown, then IXMLHTTPRequest2 in
// order), per azimuth.exe build (PE timestamp). Found through the class's RTTI (.?AVOfflineRequest@...).
struct AzimuthBuild { DWORD timestamp; const char* name; uint32_t vtable; uint32_t slot[13]; };
constexpr AzimuthBuild kBuilds[] = {
    {0x6AD14833, "0.9.10", 0x2CCC60, {0x1982D0, 0x198340, 0x198350, 0x1983B0, 0x198470, 0x1985A0, 0x198610,
                                      0x198630, 0x198640, 0x198650, 0x198660, 0x198690, 0x1986D0}},
    {0x6ADFF280, "1.0.1", 0x81E928, {0x66BFF0, 0x66BFE0, 0x66BFA0, 0x66BC10, 0x66BCD0, 0x66BDF0, 0x66BE60,
                                     0x66BE80, 0x66BE90, 0x66BEA0, 0x66BEB0, 0x66BEE0, 0x66BF20}},
    {0x6AE05DD1, "1.0.2", 0x822A58, {0x670730, 0x670720, 0x6706E0, 0x670350, 0x670410, 0x670530, 0x6705A0,
                                     0x6705C0, 0x6705D0, 0x6705E0, 0x6705F0, 0x670620, 0x670660}},
    {0x6AE0F17A, "1.0.3", 0x8493F8, {0x67C410, 0x67C400, 0x67C3C0, 0x67C030, 0x67C0F0, 0x67C210, 0x67C280,
                                     0x67C2A0, 0x67C2B0, 0x67C2C0, 0x67C2D0, 0x67C300, 0x67C340}},
    {0x6AE393EB, "1.0.4", 0x7159D8, {0x54BD90, 0x54BD80, 0x54BD40, 0x54B9B0, 0x54BA70, 0x54BB90, 0x54BC00,
                                     0x54BC20, 0x54BC30, 0x54BC40, 0x54BC50, 0x54BC80, 0x54BCC0}},
};
enum Slot { kQueryInterface, kAddRef, kRelease, kOpen, kSend, kAbort, kSetCookie, kSetCustomResponseStream,
            kSetProperty, kSetRequestHeader, kGetAllResponseHeaders, kGetCookie, kGetResponseHeader };

void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, line);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool iequals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// ---- A readable in-memory ISequentialStream for the response body.

struct MemStream final : ISequentialStream {
    LONG                 refs = 1;
    std::vector<uint8_t> data;
    size_t               pos = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream) {
            *ppv = static_cast<ISequentialStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        size_t n = std::min<size_t>(cb, data.size() - pos);
        if (n) std::memcpy(pv, data.data() + pos, n);
        pos += n;
        if (pcbRead) *pcbRead = (ULONG)n;
        return n < cb ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* pcbWritten) override {
        if (pcbWritten) *pcbWritten = 0;
        return E_NOTIMPL;
    }
};

// ---- Per-request state, keyed by the stub object's address.

struct Req {
    std::wstring method, url;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    IXMLHTTPRequest2Callback* cb = nullptr;     // we hold a reference
    ISequentialStream*        custom = nullptr; // we hold a reference
    volatile LONG aborted = 0;
    bool synthetic = false;                     // answered locally (Xbox Live privacy checks), never leaves the machine

    DWORD        status = 0;
    std::wstring statusText, rawHeaders;
    std::vector<uint8_t> respBody;
};

std::mutex g_mu;
std::unordered_map<void*, std::shared_ptr<Req>> g_reqs;

std::shared_ptr<Req> find_req(void* self) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_reqs.find(self);
    return it == g_reqs.end() ? nullptr : it->second;
}

void drop_refs(Req& r) {
    if (r.cb) { r.cb->Release(); r.cb = nullptr; }
    if (r.custom) { r.custom->Release(); r.custom = nullptr; }
}

// Original slot functions, saved by hook_pointer.
void* g_orig[13] = {};
using Fn_Release  = ULONG (STDMETHODCALLTYPE*)(void*);
using Fn_AddRef   = ULONG (STDMETHODCALLTYPE*)(void*);
using Fn_Open     = HRESULT (STDMETHODCALLTYPE*)(void*, LPCWSTR, LPCWSTR, IXMLHTTPRequest2Callback*, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
using Fn_Send     = HRESULT (STDMETHODCALLTYPE*)(void*, ISequentialStream*, ULONGLONG);
using Fn_Abort    = HRESULT (STDMETHODCALLTYPE*)(void*);
using Fn_SetCookie = HRESULT (STDMETHODCALLTYPE*)(void*, const XHR_COOKIE*, DWORD*);
using Fn_SetCustom = HRESULT (STDMETHODCALLTYPE*)(void*, ISequentialStream*);
using Fn_SetProp  = HRESULT (STDMETHODCALLTYPE*)(void*, XHR_PROPERTY, ULONGLONG);
using Fn_SetHdr   = HRESULT (STDMETHODCALLTYPE*)(void*, LPCWSTR, LPCWSTR);
using Fn_GetAll   = HRESULT (STDMETHODCALLTYPE*)(void*, LPWSTR*);
using Fn_GetCookie = HRESULT (STDMETHODCALLTYPE*)(void*, LPCWSTR, LPCWSTR, DWORD, ULONG*, XHR_COOKIE**);
using Fn_GetHdr   = HRESULT (STDMETHODCALLTYPE*)(void*, LPCWSTR, LPWSTR*);

template <class F> F orig(Slot s) { return reinterpret_cast<F>(g_orig[s]); }

// ---- The HTTP work.

LPWSTR co_string(const std::wstring& s) {
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    auto* p = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (p) std::memcpy(p, s.c_str(), bytes);
    return p;
}

// Case-insensitive lookup in the CRLF raw header block.
bool header_value(const std::wstring& raw, const std::wstring& name, std::wstring& out) {
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find(L"\r\n", pos);
        if (end == std::wstring::npos) end = raw.size();
        size_t colon = raw.find(L':', pos);
        if (colon != std::wstring::npos && colon < end && iequals(raw.substr(pos, colon - pos), name)) {
            size_t v = colon + 1;
            while (v < end && (raw[v] == L' ' || raw[v] == L'\t')) v++;
            out = raw.substr(v, end - v);
            return true;
        }
        pos = end + 2;
    }
    return false;
}

struct Closer {
    HINTERNET h;
    ~Closer() { if (h) WinHttpCloseHandle(h); }
};

// A WinHTTP call said no: log which one and how far the response had got, and turn its error into an HRESULT.
// Last error is cleared before every WinHTTP call, so the code seen here is that call's own and not a leftover
// (query_str provokes ERROR_INSUFFICIENT_BUFFER on purpose, for instance).
HRESULT http_failed(const Req& r, const char* step) {
    DWORD e = GetLastError();
    logf("%s %s: %s failed, win32 error %lu (0x%08lx), HTTP status %lu, %zu body bytes read", narrow(r.method).c_str(),
         narrow(r.url).c_str(), step, (unsigned long)e, (unsigned long)e, (unsigned long)r.status, r.respBody.size());
    return e ? HRESULT_FROM_WIN32(e) : E_FAIL;
}

// The one refusal that means "end of body" rather than "the transfer broke" is Wine 11's ERROR_INSUFFICIENT_BUFFER at
// the end of a chunked reply (dlls/winhttp/request.c: refill_buffer, read_more_data and start_next_chunk answer it once
// read_chunked_eof is set). Every other code is a lost response: WinHTTP's own (12xxx), Winsock's (10xxx), and the raw
// SEC_E_* status Wine hands back when a TLS record is cut short (dlls/winhttp/net.c read_ssl_chunk returns
// SEC_E_INCOMPLETE_MESSAGE 0x80090318 when the peer closes mid-record, or DecryptMessage's own failure), which is how a
// dropped HTTPS connection shows up on the Deck. Windows never reports ERROR_INSUFFICIENT_BUFFER from these two calls.
bool chunked_eof_quirk(DWORD e) { return e == ERROR_INSUFFICIENT_BUFFER; }

// Performs the request; fills r.status/statusText/rawHeaders/respBody. Returns S_OK or an HRESULT.
HRESULT do_http(Req& r, const std::vector<uint8_t>& body) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    r.status = 0;
    SetLastError(0);
    if (!WinHttpCrackUrl(r.url.c_str(), (DWORD)r.url.size(), 0, &uc)) return http_failed(r, "WinHttpCrackUrl");

    std::wstring objectName = g_cfg.path_prefix + std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    std::wstring hostName(uc.lpszHostName, uc.dwHostNameLength);
    // The game's URLs carry no port (they always arrive as http://<host>/...), so the configured port is used for the
    // community server; another host keeps the URL's.
    INTERNET_PORT port = iequals(hostName, g_cfg.host) && g_cfg.port > 0 ? (INTERNET_PORT)g_cfg.port : uc.nPort;
    bool secure = g_cfg.scheme == 2 || (g_cfg.scheme == 0 && port == 443);

    std::wstring ua = L"Xls Client";
    std::wstring hdrs;
    for (auto& h : r.headers) {
        if (iequals(h.first, L"User-Agent")) { ua = h.second; continue; }
        if (iequals(h.first, L"Host") || iequals(h.first, L"Content-Length") || iequals(h.first, L"Connection")) continue;
        if (h.second.empty()) continue;   // ADD|REPLACE with an empty value means "delete this header"
        hdrs += h.first + L": " + h.second + L"\r\n";
    }
    if (iequals(std::wstring(uc.lpszHostName, uc.dwHostNameLength), g_cfg.host) && !g_cfg.key.empty()) {
        // Identify this install to the community server (account key + display name).
        auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
        std::string enc;
        for (unsigned char c : g_cfg.name) {
            char tmp[4];
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.') { enc += (char)c; }
            else { std::snprintf(tmp, sizeof tmp, "%%%02X", c); enc += tmp; }
        }
        hdrs += L"X-CT-Key: " + wide(g_cfg.key) + L"\r\n";
        // An empty value with ADD|REPLACE means "delete this header" and fails with ERROR_WINHTTP_HEADER_NOT_FOUND
        // (0x80072f76), so a player with no gamertag configured must not send the header at all.
        if (!enc.empty()) hdrs += L"X-CT-Name: " + wide(enc) + L"\r\n";
    }

    bool local = iequals(hostName, L"localhost") || hostName == L"127.0.0.1" || hostName == L"::1";
    SetLastError(0);
    Closer session{WinHttpOpen(ua.c_str(), local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return http_failed(r, "WinHttpOpen");
    SetLastError(0);
    WinHttpSetTimeouts(session.h, 5000, 5000, 15000, 30000);

    SetLastError(0);
    Closer conn{WinHttpConnect(session.h, hostName.c_str(), port, 0)};
    if (!conn.h) return http_failed(r, "WinHttpConnect");

    SetLastError(0);
    Closer req{WinHttpOpenRequest(conn.h, r.method.empty() ? L"POST" : r.method.c_str(), objectName.c_str(), nullptr,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!req.h) return http_failed(r, "WinHttpOpenRequest");

    if (secure && g_cfg.insecure_tls) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        SetLastError(0);
        WinHttpSetOption(req.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    }
    SetLastError(0);
    if (!hdrs.empty() && !WinHttpAddRequestHeaders(req.h, hdrs.c_str(), (DWORD)hdrs.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return http_failed(r, "WinHttpAddRequestHeaders");

    DWORD len = (DWORD)body.size();
    SetLastError(0);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, len ? (LPVOID)body.data() : WINHTTP_NO_REQUEST_DATA, len, len, 0))
        return http_failed(r, "WinHttpSendRequest");
    SetLastError(0);
    if (!WinHttpReceiveResponse(req.h, nullptr)) return http_failed(r, "WinHttpReceiveResponse");

    DWORD size = sizeof r.status;
    SetLastError(0);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size, WINHTTP_NO_HEADER_INDEX);

    auto query_str = [&](DWORD what) {
        DWORD bytes = 0;
        SetLastError(0);
        WinHttpQueryHeaders(req.h, what, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);   // sizes: fails with ERROR_INSUFFICIENT_BUFFER by design
        std::wstring s(bytes / sizeof(wchar_t), L'\0');
        SetLastError(0);
        if (bytes && WinHttpQueryHeaders(req.h, what, WINHTTP_HEADER_NAME_BY_INDEX, s.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
            s.resize(bytes / sizeof(wchar_t));
        else
            s.clear();
        return s;
    };
    r.statusText = query_str(WINHTTP_QUERY_STATUS_TEXT);
    r.rawHeaders = query_str(WINHTTP_QUERY_RAW_HEADERS_CRLF);

    // Body. The header block is complete here. One refusal from WinHttpQueryDataAvailable/WinHttpReadData is the end
    // of the body (chunked_eof_quirk); every other one fails the request. Wine 11's winhttp
    // (dlls/winhttp/request.c: get_chunk_size sets read_chunked_eof inside query_data_available, after the
    // end_of_read_data check, so refill_buffer answers ERROR_INSUFFICIENT_BUFFER) fails the first
    // WinHttpQueryDataAvailable of every chunked reply whose closing "0" chunk arrived with the headers, i.e. every
    // empty chunked reply such as the community server's 401 behind Cloudflare; Windows reports 0 bytes there.
    bool more = true;
    while (more) {
        DWORD avail = 0;
        SetLastError(0);
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            DWORD e = GetLastError();
            if (!chunked_eof_quirk(e)) return http_failed(r, "WinHttpQueryDataAvailable");
            if (g_cfg.debug) logf("WinHttpQueryDataAvailable refused with error %lu after a complete header block (HTTP %lu, %zu body bytes): end of body",
                                  (unsigned long)e, (unsigned long)r.status, r.respBody.size());
            break;
        }
        if (!avail) break;
        if (r.respBody.size() + avail > kMaxBody) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        // WinHttpReadData may hand back less than asked for: keep reading until it has nothing more.
        DWORD want = avail;
        while (want) {
            size_t at = r.respBody.size();
            r.respBody.resize(at + want);
            DWORD got = 0;
            SetLastError(0);
            BOOL ok = WinHttpReadData(req.h, r.respBody.data() + at, want, &got);
            r.respBody.resize(at + (ok ? got : 0));
            if (!ok) {
                DWORD e = GetLastError();
                if (!chunked_eof_quirk(e)) return http_failed(r, "WinHttpReadData");
                if (g_cfg.debug) logf("WinHttpReadData refused with error %lu after a complete header block (HTTP %lu, %zu body bytes): end of body",
                                      (unsigned long)e, (unsigned long)r.status, r.respBody.size());
                more = false;
                break;
            }
            if (!got) { more = false; break; }
            want -= got;
        }
    }
    return S_OK;
}

// Xbox Live's privacy service decides whether the player may share user-created content. There is no Xbox Live
// to ask, so every check is answered "allowed". The JSON follows XSAPI's PermissionCheckResult deserializer.
// ---- Drivatar names. The game asks the profile service for every other driver's gamertag by user id; each id gets
// the next name in the list the first time it's asked for, and keeps it.
std::mutex                                   g_names_mu;
std::vector<std::string>                     g_names;
std::unordered_map<std::string, std::string> g_name_of;   // user id -> name

// Azimuth's local player is always user 2533274790395904 (0x9000000000000, the saves' User_9000000000000): that lookup
// is the player's own gamertag, so it gets the player's name, never a driver's.
constexpr const char* kLocalUser = "2533274790395904";

std::string drivatar_name(const std::string& user) {
    if (user == kLocalUser) return g_cfg.name.empty() ? "Racer" : g_cfg.name;
    std::lock_guard<std::mutex> lk(g_names_mu);
    if (g_names.empty()) return "Racer";
    auto it = g_name_of.find(user);
    if (it != g_name_of.end()) return it->second;
    std::string name = g_names[g_name_of.size() % g_names.size()];
    g_name_of.emplace(user, name);
    return name;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char u[8]; std::snprintf(u, sizeof u, "\\u%04x", c); out += u; }
        else out += (char)c;
    }
    return out;
}

HRESULT do_synthetic(Req& r, const std::vector<uint8_t>& body) {
    std::string reply;
    if (r.url.find(L"/users/batch/profile/settings") != std::wstring::npos) {
        // Profile service (gamertags for other players / drivatars). Echo every requested user with each requested
        // name setting set to the driver's name (drivatar_name), so name plates have text instead of boxes.
        std::string in(body.begin(), body.end());
        if (g_cfg.debug) logf("profile settings request body: %.300s", in.c_str());
        auto strings_after = [&](const char* key) {
            std::vector<std::string> out;
            size_t at = in.find(key);
            if (at == std::string::npos) return out;
            size_t lb = in.find('[', at), rb = in.find(']', at);
            if (lb == std::string::npos || rb == std::string::npos) return out;
            for (size_t i = lb; i < rb; i++) {
                if (in[i] != '"') continue;
                size_t e = in.find('"', i + 1);
                if (e == std::string::npos || e > rb) break;
                out.push_back(in.substr(i + 1, e - i - 1));
                i = e;
            }
            return out;
        };
        auto users = strings_after("userIds");
        auto settings = strings_after("settings");
        reply = "{" + std::string(1, '"') + "profileUsers" + std::string(1, '"') + ":[";
        for (size_t u = 0; u < users.size(); u++) {
            reply += std::string(u ? "," : "") + "{\"id\":\"" + users[u] + "\",\"hostId\":\"" + users[u] + "\",\"settings\":[";
            const std::string who = json_escape(drivatar_name(users[u]));
            for (size_t s = 0; s < settings.size(); s++) {
                bool name = settings[s].find("amertag") != std::string::npos || settings[s].find("Name") != std::string::npos;
                reply += std::string(s ? "," : "") + "{\"id\":\"" + settings[s] + "\",\"value\":\"" + (name ? who : "") + "\"}";
            }
            reply += "],\"isSponsoredUser\":false}";
        }
        reply += "]}";
    } else if (r.url.find(L"/system/strings/validate") != std::wstring::npos) {
        // String verification (profanity filter) for the description/keywords: one "OK" result per string sent.
        // The request body's first JSON array holds the strings; count its elements.
        std::string in(body.begin(), body.end());
        if (g_cfg.debug) logf("string validate request body: %.400s", in.c_str());
        int count = 0, depth = 0;
        bool inStr = false, started = false;
        for (size_t i = 0; i < in.size(); i++) {
            char c = in[i];
            if (inStr) { if (c == '\\') i++; else if (c == '"') inStr = false; continue; }
            if (c == '"') { inStr = true; if (started && depth == 1) count++; }
            else if (c == '[') { depth++; started = true; }
            else if (c == '{' && started && depth == 1) { count++; depth++; }
            else if (c == '{') depth++;
            else if (c == ']' || c == '}') { depth--; if (started && depth == 0) break; }
        }
        reply = "{\"verifyStringResult\":[";
        for (int i = 0; i < count; i++) reply += std::string(i ? "," : "") + "{\"resultCode\":0,\"firstOffendingSubstring\":\"\"}";
        reply += "]}";
    } else if (r.method == L"GET") {
        reply = "{\"isAllowed\":true,\"permissionRequested\":\"\",\"reasons\":[]}";
    } else {
        // Batch check: echo each requested user back with every permission allowed. The exact shape is a guess;
        // the request body is logged so it can be corrected.
        std::string in(body.begin(), body.end());
        if (g_cfg.debug) logf("privacy batch request body: %.600s", in.c_str());
        reply = "{\"responses\":[]}";
    }
    r.status = 200;
    r.statusText = L"OK";
    r.rawHeaders = L"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_wstring(reply.size()) + L"\r\n\r\n";
    r.respBody.assign(reply.begin(), reply.end());
    return S_OK;
}

struct Job {
    void*                self;
    std::shared_ptr<Req> req;
    std::vector<uint8_t> body;
};

// The car the tune menus were last opened for. The game asks for the Suggested list (GetCuratedContent {ContentType u32, CarId i32,
// ...}) and searches (SearchParameters {MakeId, ModelId, ...}) with the current car, but lets a My Files tune made for another
// car be applied to it: that tune's parts don't exist on this car (crash, or stock parts under the car-builder guard). So a
// downloaded tune (RetrieveVersionedTuneById reply: 12-byte header, then CarId u32) for a different car is answered with an error.
volatile LONG g_menu_car = 0;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void refuse_foreign_tune(Req& r) {
    LONG car = g_menu_car;
    if (car <= 0 || r.status != 200 || r.respBody.size() < 16 || be32(r.respBody.data()) != 0) return;   // kind 0: a success reply
    uint32_t tune_car = be32(r.respBody.data() + 12);
    if (tune_car == (uint32_t)car) return;
    logf("a downloaded tune is for car %lu, not the current car %ld: refused, so the game leaves the car as it is", (unsigned long)tune_car, car);
    static const char kText[] = "This tune was made for a different car.";
    uint8_t head[12] = {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, (uint8_t)(sizeof kText - 1)};    // error reply: kind 2, code 1, text length
    r.respBody.assign(head, head + sizeof head);
    r.respBody.insert(r.respBody.end(), kText, kText + sizeof kText - 1);
}

// Tell the player why a request failed: the server's own text from an error reply (kind 2), or why the server couldn't
// be reached. It goes into the Storefront's error popup in place of its generic message, or on the HUD when there's
// no popup. Errors that are expected stay in the log.
//
// A livery that downloaded fine can still be refused by the game: it won't load one bigger than about 337 KB, and says
// only "Design Unavailable" (popup code 0x80500001). The largest reply seen to load was 345,013 bytes, the smallest
// seen to fail 389,086; one above the first gets that reason ready, without a HUD notification, as it may still load.
constexpr size_t kLargestLiveryLoaded = 345013;
// The server's error code for an upload over that size (validate.js TOO_LARGE).
constexpr uint32_t kTooLarge = 413;

void tell_player(const std::string& xmethod, const Req& r, HRESULT hr) {
    static const char* const kQuiet[] = {"GetDrivatarsForCache", "GetDrivatarInformationForRace"};   // "drivatars unavailable" is normal
    if (xmethod.empty()) return;
    for (const char* q : kQuiet) if (xmethod == q) return;
    // The server answers every Clubs request with 503, as an offline game gets, so the Clubs tab stays locked.
    for (auto& h : r.headers)
        if (iequals(h.first, L"X-ClassName") && h.second.rfind(L"Forza.WebServices.CarClub", 0) == 0) return;
    char text[400];
    if (SUCCEEDED(hr) && r.status == 200 && xmethod == "RetrieveVersionedLiveryById" && r.respBody.size() > kLargestLiveryLoaded &&
        be32(r.respBody.data()) == 0) {
        std::snprintf(text, sizeof text, "This livery is too big for the game to load (%zu KB). Liveries over about 337 KB can't be downloaded.",
                      r.respBody.size() / 1024);
        notify_request_failed(0x80500001, text, false);
        return;
    }
    uint32_t seen = 0;   // the code the game will see, when known
    if (FAILED(hr)) {
        seen = (uint32_t)hr;
        if (hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) return;
        switch (HRESULT_CODE(hr)) {
        case ERROR_WINHTTP_TIMEOUT: std::snprintf(text, sizeof text, "The server took too long to answer. Try again in a moment."); break;
        case ERROR_WINHTTP_SECURE_FAILURE: std::snprintf(text, sizeof text, "Couldn't make a secure connection to the server."); break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR: std::snprintf(text, sizeof text, "Can't reach the server. Check your internet connection."); break;
        default: std::snprintf(text, sizeof text, "Can't reach the server (error 0x%08lX).", (unsigned long)hr); break;
        }
    } else if (r.status >= 400) {
        std::snprintf(text, sizeof text, "The server had a problem (HTTP %lu). Try again later.", (unsigned long)r.status);
    } else if (r.respBody.size() >= 12 && be32(r.respBody.data()) == 2) {
        size_t len = std::min<size_t>(be32(r.respBody.data() + 8), r.respBody.size() - 12);
        std::string said(reinterpret_cast<const char*>(r.respBody.data()) + 12, len);
        uint32_t code = be32(r.respBody.data() + 4);
        // The game reports it as 0x8051xxxx, but the popup it ends up in may carry another code, so any Storefront
        // error popup right after it gets this text.
        seen = 0;
        if (said.empty()) std::snprintf(text, sizeof text, "The server turned this down (code %lu).", (unsigned long)code);
        else std::snprintf(text, sizeof text, "%s", said.c_str());
        // 413: the livery is too big to share. LiveryLayerPatch raises the game's layer limits well past that.
        if (code == kTooLarge && xmethod == "UpdateLivery" && g_api->mod_version("LiveryLayerPatch"))
            std::snprintf(text + std::strlen(text), sizeof text - std::strlen(text),
                          " LiveryLayerPatch lets you add more layers than can be shared.");
    } else {
        return;
    }
    notify_request_failed(seen, text);
}

// With the detailed log on, every Scoreboard call's request and reply bodies go to communitygarage/captures/, to work
// out the leaderboard layouts: <n>-<method>-req.bin and -reply.bin.
void save_scoreboard_capture(const Req& r, const std::string& xmethod, const std::vector<uint8_t>& body) {
    bool scoreboard = false;
    for (auto& h : r.headers)
        if (iequals(h.first, L"X-ClassName") && h.second.rfind(L"Forza.WebServices.Scoreboard", 0) == 0) scoreboard = true;
    if (!scoreboard || xmethod.empty()) return;
    static volatile LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    if (k > 500) return;
    wchar_t dir[MAX_PATH];
    if (GetModuleFileNameW(nullptr, dir, MAX_PATH) == 0) return;
    std::wstring folder = dir;
    folder = folder.substr(0, folder.find_last_of(L"\\/")) + L"\\communitygarage\\captures";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring stem = folder + L"\\" + std::to_wstring(GetTickCount()) + L"-" + std::to_wstring(k) + L"-" +
                        std::wstring(xmethod.begin(), xmethod.end());
    for (int part = 0; part < 2; part++) {
        const std::vector<uint8_t>& data = part ? r.respBody : body;
        if (FILE* f = _wfopen((stem + (part ? L"-reply.bin" : L"-req.bin")).c_str(), L"wb")) {
            if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
        }
    }
    logf("scoreboard capture #%ld: %s (%zu request bytes, %zu reply bytes)", k, xmethod.c_str(), body.size(), r.respBody.size());
}

DWORD WINAPI worker(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    Req& r = *job->req;
    void* self = job->self;
    auto* xhr = reinterpret_cast<IXMLHTTPRequest2*>(self);

    std::string xmethod;
    for (auto& h : r.headers) if (iequals(h.first, L"X-Method")) xmethod = narrow(h.second);
    if ((xmethod == "GetCuratedContent" || xmethod == "SearchTuningFiles") && job->body.size() >= 8) {
        LONG car = (LONG)be32(job->body.data() + 4);
        if (car > 0) InterlockedExchange(&g_menu_car, car);
    }
    {
        static LONG shown = 0;
        LONG nth = InterlockedIncrement(&shown);
        if (g_cfg.debug && nth <= 600) logf("request #%ld %s (%zu body bytes)", nth, xmethod.c_str(), job->body.size());
        if (g_cfg.debug && nth <= 6) {
            std::string hs;
            for (auto& h : r.headers) hs += narrow(h.first) + "=[" + narrow(h.second) + "] ";
            logf("request %s %s, %zu body bytes, headers: %s", narrow(r.method).c_str(), narrow(r.url).c_str(),
                 job->body.size(), hs.c_str());
            if (!job->body.empty()) {
                char hex[3 * 48 + 8] = {};
                int len = 0;
                for (size_t i = 0; i < job->body.size() && i < 48; i++)
                    len += std::snprintf(hex + len, sizeof hex - len, "%02x ", job->body[i]);
                logf("  body starts: %s", hex);
            }
        }
    }
    DWORD t0 = GetTickCount();
    bool probe = false;
    if (g_cfg.fail_economy) {
        for (auto& h : r.headers) if (iequals(h.first, L"X-Method") && iequals(h.second, L"GetEconomySettings")) probe = true;
        if (probe) logf("not asking the server for economy values (opted out or economy_probe), the game keeps its built-in economy values");
    }
    HRESULT hr = InterlockedCompareExchange(&r.aborted, 0, 0) ? HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)
                                                               : (probe ? HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE) : (r.synthetic ? do_synthetic(r, job->body) : do_http(r, job->body)));

    if (SUCCEEDED(hr) && xmethod == "RetrieveVersionedTuneById") refuse_foreign_tune(r);
    if (!probe && !r.synthetic) tell_player(xmethod, r, hr);
    IXMLHTTPRequest2Callback* cb = r.cb;
    if (SUCCEEDED(hr)) {
        // The game reads Content-Length from the request object in OnHeadersAvailable
        // (GetResponseHeader), then keeps the stream it gets in OnDataAvailable and
        // signals completion from OnResponseReceived.
        if (g_cfg.debug || r.status >= 400) logf("%s %s -> %lu, %zu bytes, %lu ms", narrow(r.method).c_str(), narrow(r.url).c_str(), (unsigned long)r.status,
             r.respBody.size(), (unsigned long)(GetTickCount() - t0));
        if (g_cfg.debug) save_scoreboard_capture(r, xmethod, job->body);
        if (cb) {
            MemStream* ms = nullptr;
            if (r.custom) {
                size_t off = 0;
                HRESULT whr = S_OK;
                ULONG wrote = 0;
                while (off < r.respBody.size()) {
                    wrote = 0;
                    whr = r.custom->Write(r.respBody.data() + off, (ULONG)(r.respBody.size() - off), &wrote);
                    if (FAILED(whr) || !wrote) break;
                    off += wrote;
                }
                if (off < r.respBody.size())
                    logf("response stream stopped taking data: wrote %zu of %zu bytes (Write hr=0x%08lx, last write %lu bytes)", off,
                         r.respBody.size(), (unsigned long)whr, (unsigned long)wrote);
            } else {
                ms = new MemStream();
                ms->data = r.respBody;
            }
            cb->OnHeadersAvailable(xhr, r.status, r.statusText.c_str());
            cb->OnDataAvailable(xhr, ms);
            cb->OnResponseReceived(xhr, ms);
            if (ms) ms->Release();
        }
    } else {
        logf("%s %s FAILED: 0x%08lx (%lu ms)", narrow(r.method).c_str(), narrow(r.url).c_str(), (unsigned long)hr,
             (unsigned long)(GetTickCount() - t0));
        if (cb) cb->OnError(xhr, hr);
    }

    // Done with the callback; drop our references, then the one Send took on the stub.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        drop_refs(r);
    }
    reinterpret_cast<Fn_Release>((*reinterpret_cast<void***>(self))[kRelease])(self);
    return 0;
}

// ---- Detours. Requests we don't own fall through to the stub's own behaviour.

ULONG STDMETHODCALLTYPE d_Release(void* self) {
    ULONG n = orig<Fn_Release>(kRelease)(self);
    if (n == 0) {
        std::shared_ptr<Req> r;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_reqs.find(self);
            if (it != g_reqs.end()) { r = it->second; g_reqs.erase(it); }
        }
        if (r) { std::lock_guard<std::mutex> lk(g_mu); drop_refs(*r); }
    }
    return n;
}

HRESULT STDMETHODCALLTYPE d_Open(void* self, LPCWSTR method, LPCWSTR url, IXMLHTTPRequest2Callback* cb,
                                 LPCWSTR u, LPCWSTR p, LPCWSTR pu, LPCWSTR pp) {
    if (method && url && cb) {
        URL_COMPONENTSW uc{};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256];
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        std::wstring h;
        bool cracked = WinHttpCrackUrl(url, 0, 0, &uc);
        if (cracked) h.assign(uc.lpszHostName, uc.dwHostNameLength);
        bool privacy = cracked && (iequals(h, L"privacy.xboxlive.com") || iequals(h, L"client-strings.xboxlive.com") || iequals(h, L"profile.xboxlive.com"));
        if (cracked && (privacy || iequals(h, g_cfg.host))) {
            auto r = std::make_shared<Req>();
            r->synthetic = privacy;
            r->method = method;
            r->url = url;
            r->cb = cb;
            cb->AddRef();
            std::lock_guard<std::mutex> lk(g_mu);
            g_reqs[self] = r;
            return S_OK;
        }
    }
    {
        // Not ours: it stays offline. Log what the game tried, to spot calls (uploads, downloads) that need a host.
        static LONG others = 0;
        if (g_cfg.debug && url && InterlockedIncrement(&others) <= 80)
            logf("not forwarded (other host): %s %s", method ? narrow(method).c_str() : "?", narrow(url).c_str());
    }
    return orig<Fn_Open>(kOpen)(self, method, url, cb, u, p, pu, pp);
}

HRESULT STDMETHODCALLTYPE d_Send(void* self, ISequentialStream* body, ULONGLONG cbBody) {
    auto r = find_req(self);
    if (!r) return orig<Fn_Send>(kSend)(self, body, cbBody);

    auto job = std::make_unique<Job>();
    job->self = self;
    job->req = r;
    if (body) {
        uint8_t buf[4096];
        for (;;) {
            ULONG got = 0;
            HRESULT hr = body->Read(buf, sizeof buf, &got);
            if (got) job->body.insert(job->body.end(), buf, buf + got);
            if (FAILED(hr) || !got || (cbBody && job->body.size() >= cbBody) || job->body.size() > kMaxBody) break;
        }
    }
    // Keep the stub object alive until the worker is done with it.
    reinterpret_cast<Fn_AddRef>((*reinterpret_cast<void***>(self))[kAddRef])(self);
    Job* raw = job.release();
    if (!QueueUserWorkItem(worker, raw, WT_EXECUTELONGFUNCTION)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        delete raw;
        reinterpret_cast<Fn_Release>((*reinterpret_cast<void***>(self))[kRelease])(self);
        return hr;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_Abort(void* self) {
    auto r = find_req(self);
    if (!r) return orig<Fn_Abort>(kAbort)(self);
    InterlockedExchange(&r->aborted, 1);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_SetCookie(void* self, const XHR_COOKIE* c, DWORD* state) {
    if (!find_req(self)) return orig<Fn_SetCookie>(kSetCookie)(self, c, state);
    if (state) *state = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_SetCustomResponseStream(void* self, ISequentialStream* s) {
    auto r = find_req(self);
    if (!r) return orig<Fn_SetCustom>(kSetCustomResponseStream)(self, s);
    std::lock_guard<std::mutex> lk(g_mu);
    if (r->custom) r->custom->Release();
    r->custom = s;
    if (s) s->AddRef();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_SetProperty(void* self, XHR_PROPERTY prop, ULONGLONG value) {
    if (!find_req(self)) return orig<Fn_SetProp>(kSetProperty)(self, prop, value);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_SetRequestHeader(void* self, LPCWSTR name, LPCWSTR value) {
    auto r = find_req(self);
    if (!r) return orig<Fn_SetHdr>(kSetRequestHeader)(self, name, value);
    if (!name) return E_INVALIDARG;
    std::lock_guard<std::mutex> lk(g_mu);
    r->headers.emplace_back(name, value ? value : L"");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_GetAllResponseHeaders(void* self, LPWSTR* out) {
    auto r = find_req(self);
    if (!r) return orig<Fn_GetAll>(kGetAllResponseHeaders)(self, out);
    if (!out) return E_POINTER;
    *out = co_string(r->rawHeaders);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE d_GetCookie(void* self, LPCWSTR url, LPCWSTR name, DWORD flags, ULONG* count, XHR_COOKIE** cookies) {
    if (!find_req(self)) return orig<Fn_GetCookie>(kGetCookie)(self, url, name, flags, count, cookies);
    if (count) *count = 0;
    if (cookies) *cookies = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE d_GetResponseHeader(void* self, LPCWSTR name, LPWSTR* out) {
    auto r = find_req(self);
    if (!r) return orig<Fn_GetHdr>(kGetResponseHeader)(self, name, out);
    if (!name || !out) return E_POINTER;
    *out = nullptr;
    std::wstring v;
    if (!header_value(r->rawHeaders, name, v)) {
        // The game asks for Content-Length before it has the body; we read the whole body first.
        if (iequals(name, L"Content-Length")) v = std::to_wstring(r->respBody.size());
        else return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    *out = co_string(v);
    return *out ? S_OK : E_OUTOFMEMORY;
}

} // namespace

void xhr_bridge_set_drivatar_names(std::vector<std::string> names) {
    std::lock_guard<std::mutex> lk(g_names_mu);
    g_names = std::move(names);
    g_name_of.clear();
}

bool xhr_bridge_fetch_drivatar_names(std::vector<std::string>* out) {
    out->clear();
    if (!g_api) return false;
    const BridgeConfig& cfg = g_cfg;   // set by xhr_bridge_install, read-only from then on (do_http uses it too)
    bool secure = cfg.scheme == 2 || (cfg.scheme == 0 && cfg.port == 443);
    Req r;
    r.method = L"GET";
    r.url    = std::wstring(secure ? L"https://" : L"http://") + cfg.host + L":" + std::to_wstring(cfg.port) + L"/drivatar-names";
    HRESULT hr = do_http(r, {});
    if (FAILED(hr) || r.status != 200) {
        logf("drivatar names: the server didn't send them (hr 0x%08lx, HTTP %lu)", (unsigned long)hr, (unsigned long)r.status);
        return false;
    }
    std::string text(r.respBody.begin(), r.respBody.end());
    size_t at = 0;
    while (at < text.size() && out->size() < 26) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out->push_back(line);
    }
    return true;
}

namespace {
const AzimuthBuild* known_build() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    for (const AzimuthBuild& b : kBuilds)
        if (nt->FileHeader.TimeDateStamp == b.timestamp) return &b;
    return nullptr;
}
} // namespace

const char* xhr_bridge_known_build() {
    const AzimuthBuild* b = known_build();
    return b ? b->name : nullptr;
}

bool xhr_bridge_install(const FH2ModApi* api, FH2Mod* mod, const BridgeConfig& cfg) {
    g_api = api;
    g_mod = mod;
    g_cfg = cfg;
    if (api->api_version < 3) {
        logf("HTTP bridge needs platform API 3 (hook_pointer)");
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const AzimuthBuild* build = known_build();
    if (!build) {
        logf("HTTP bridge: this azimuth.exe isn't one it was written for; not hooking");
        return false;
    }
    auto** vt = reinterpret_cast<void**>(base + build->vtable);
    for (int i = 0; i < 13; i++) {
        if (reinterpret_cast<uint8_t*>(vt[i]) != base + build->slot[i]) {
            logf("HTTP bridge: OfflineRequest vtable slot %d isn't where expected (Azimuth %s); not hooking", i, build->name);
            return false;
        }
    }

    struct H { Slot slot; void* detour; const char* name; };
    const H hooks[] = {
        {kRelease, (void*)d_Release, "OfflineRequest::Release"},
        {kOpen, (void*)d_Open, "OfflineRequest::Open"},
        {kSend, (void*)d_Send, "OfflineRequest::Send"},
        {kAbort, (void*)d_Abort, "OfflineRequest::Abort"},
        {kSetCookie, (void*)d_SetCookie, "OfflineRequest::SetCookie"},
        {kSetCustomResponseStream, (void*)d_SetCustomResponseStream, "OfflineRequest::SetCustomResponseStream"},
        {kSetProperty, (void*)d_SetProperty, "OfflineRequest::SetProperty"},
        {kSetRequestHeader, (void*)d_SetRequestHeader, "OfflineRequest::SetRequestHeader"},
        {kGetAllResponseHeaders, (void*)d_GetAllResponseHeaders, "OfflineRequest::GetAllResponseHeaders"},
        {kGetCookie, (void*)d_GetCookie, "OfflineRequest::GetCookie"},
        {kGetResponseHeader, (void*)d_GetResponseHeader, "OfflineRequest::GetResponseHeader"},
    };
    // AddRef (slot 1) is left alone; d_Send/worker call it through the vtable.
    g_orig[kAddRef] = vt[kAddRef];

    bool ok = true;
    for (const H& h : hooks) {
        FH2Hook* hook = api->hook_pointer(mod, h.name, &vt[h.slot], h.detour, &g_orig[h.slot]);
        ok = ok && hook && api->enable_hooks(mod, hook);
    }
    logf("HTTP bridge %s: requests to %s go over WinHTTP (scheme %s%s)", ok ? "installed" : "FAILED",
         narrow(cfg.host).c_str(), cfg.scheme == 0 ? "auto" : cfg.scheme == 1 ? "http" : "https",
         cfg.insecure_tls ? ", insecure TLS allowed" : "");
    return ok;
}
