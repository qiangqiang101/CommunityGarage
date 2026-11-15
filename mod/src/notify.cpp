#include "notify.hpp"

#include "fh2game.h"
#include "fh2mod_ui.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

const FH2ModApi* g_api  = nullptr;
FH2Mod*          g_mod  = nullptr;
uintptr_t        g_base = 0;

// MakeDisplayString(out, text, -1) makes a display string (a ref-counted object) with a reference in *out. Its
// text goes through the Windows code page, so only ASCII is sure to come out as written. The string holds UTF-16,
// and SetDisplayStringWide(string, text, -1) (the wchar_t* constructor's setter; it regrows the buffer from the
// calling thread's game heap) puts any text in it: the popup is made empty, then given its text that way, as
// FH2-ModPlatform.NET shows its notifications in Chinese.
using Fn_MakeDisplayString = void** (*)(void** out, const char* text, int len);
constexpr uintptr_t kSetDisplayStringWide = 0x12467B0;
constexpr char      kSetDisplayStringWide_BYTES[] = "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x10\x48\x89\x74\x24\x18\x57\x48\x83\xEC\x20";
using Fn_SetDisplayStringWide = void (*)(void* string, const wchar_t* text, int len);
bool g_wide = false;   // SetDisplayStringWide's code checked

// StorefrontErrorText(out, hr): the message the Storefront's error popups show for hr. It has its own text for the
// Storefront's codes (0x80514101, 0x805143xx) and hands every other code to the online services' table, which falls
// back to IDS_ERROR_GENERIC; a community server's error reply (0x8051xxxx, xxxx its code) always ends up there.
// It writes a display string with one reference to *out and returns out. Called on the UI thread.
constexpr uintptr_t kStorefrontErrorText = 0xB4D7C0;
using Fn_StorefrontErrorText = void** (*)(void** out, uint32_t hr);
Fn_StorefrontErrorText g_error_text_orig = nullptr;

constexpr size_t kMaxPopup    = 300;      // the popup wraps its message
constexpr DWORD  kFailureFor  = 10000;    // a failed request explains the Storefront's error popups this long after it
constexpr DWORD  kToastAfter  = 3000;     // and if no popup asked for it by then, it shows on the HUD instead
constexpr DWORD  kRepeatAfter = 30000;    // the same line again within this isn't shown

// The last request that failed: the code the game is expected to see (0 when unknown) and what to tell the player.
struct Failure { uint32_t hr = 0; std::string text, line; DWORD at = 0; bool toast = true, used = false, toasted = false; };

std::mutex  g_mu;        // g_failure and the repeat check
Failure     g_failure;
std::string g_last_line;
DWORD       g_last_shown = 0;

void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, line);
}

std::string ascii(const char* s, size_t max) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
        if (*p < 0x80) out += (*p < 0x20) ? ' ' : static_cast<char>(*p);
        else if ((*p & 0xC0) != 0x80) out += '?';   // one '?' per UTF-8 character, none for its continuation bytes
    }
    if (out.size() > max) out.replace(max - 3, std::string::npos, "...");
    return out;
}

// UTF-8 to UTF-16, one line, cut at `max` characters; empty if it isn't UTF-8.
std::wstring utf16(const std::string& s, size_t max) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L' ');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
    for (wchar_t& c : w) if (c < 0x20) c = L' ';
    if (w.size() > max) {
        size_t cut = max - 3;
        if (cut && w[cut - 1] >= 0xD800 && w[cut - 1] <= 0xDBFF) cut--;   // never half a surrogate pair
        w.replace(cut, std::wstring::npos, L"...");
    }
    return w;
}

// A HUD pop-up, unless the same line was shown less than half a minute ago. g_mu held.
void toast_locked(const std::string& line, uint32_t flags) {
    DWORD now = GetTickCount();
    if (line == g_last_line && now - g_last_shown < kRepeatAfter) return;
    g_last_line  = line;
    g_last_shown = now;
    FH2Toast t{};
    t.size     = sizeof t;
    t.key      = "server";       // a newer failure takes the place of one still waiting
    t.title    = "Community Garage";
    t.subtitle = line.c_str();
    t.flags    = flags;
    if (!fh2ui_show_toast(&t)) logf("pop-up not shown (pop-ups are off, or the platform has none): %s", line.c_str());
}

// Shows the last failure as a pop-up once no Storefront popup has used it for kToastAfter.
DWORD WINAPI toast_thread(LPVOID) {
    for (;;) {
        Sleep(250);
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_failure.at && g_failure.toast && !g_failure.used && !g_failure.toasted && GetTickCount() - g_failure.at >= kToastAfter) {
            g_failure.toasted = true;
            toast_locked(g_failure.line, 0);
        }
    }
}

// The game's own message for the Storefront's codes; the server's reason for anything else, when a request failed
// moments ago with that code (or with one that isn't known).
bool own_storefront_code(uint32_t hr) {
    return hr == 0x80514101 || (hr >= 0x80514304 && hr <= 0x8051430B);
}

void** error_text_detour(void** out, uint32_t hr) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        bool recent = g_failure.at && GetTickCount() - g_failure.at < kFailureFor;
        if (recent && !own_storefront_code(hr) && (!g_failure.hr || g_failure.hr == hr)) {
            text = g_failure.text;
            g_failure.used = true;
        }
    }
    if (text.empty()) {
        logf("Storefront error popup for 0x%08lX: the game's own message", (unsigned long)hr);
        return g_error_text_orig(out, hr);
    }
    *out = nullptr;
    auto make = reinterpret_cast<Fn_MakeDisplayString>(g_base + FH2_GAME_MAKE_DISPLAY_STRING);
    std::wstring wide = g_wide ? utf16(text, kMaxPopup) : std::wstring();
    if (!wide.empty()) {
        make(out, "", -1);
        if (*out) reinterpret_cast<Fn_SetDisplayStringWide>(g_base + kSetDisplayStringWide)(*out, wide.c_str(), -1);
    } else {
        make(out, ascii(text.c_str(), kMaxPopup).c_str(), -1);
    }
    logf("Storefront error popup for 0x%08lX: \"%s\"", (unsigned long)hr, text.c_str());
    return out;
}

bool code_is(uintptr_t off, const char* bytes, size_t n) {
    return std::memcmp(reinterpret_cast<const void*>(g_base + off), bytes, n) == 0;
}
#define CODE_IS(name) code_is(name, name##_BYTES, sizeof name##_BYTES - 1)

// "notify <text>" from fh2mod.exe or the command pipe: shows <text> the way a failure would be, to try the pop-up.
void on_notify_command(void*, const char*, const char* args, char* reply, size_t reply_size) {
    if (!args || !*args) { std::snprintf(reply, reply_size, "error: usage: notify <text>"); return; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_last_line.clear();
        toast_locked(args, FH2_TOAST_FLAG_REPLY);
    }
    std::snprintf(reply, reply_size, "queued; it shows on the driving HUD (Minor Notifications on)");
}

} // namespace

bool notify_install(const FH2ModApi* api, FH2Mod* mod, uintptr_t base) {
    g_api  = api;
    g_mod  = mod;
    g_base = base;
    if (HANDLE t = CreateThread(nullptr, 0, toast_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (api->api_version >= 9) api->add_command(mod, "notify", "notify <text>: show <text> as a HUD pop-up", on_notify_command, nullptr);
    // MakeDisplayString is only called, never hooked, so its bytes are checked in memory. Achievements and FH2MP check
    // them too, and the platform keeps them as they are.
    if (!CODE_IS(FH2_GAME_MAKE_DISPLAY_STRING)) {
        logf("Storefront error text: the game code isn't as expected; its popups keep the game's own messages");
        return false;
    }
    g_wide = CODE_IS(kSetDisplayStringWide);
    if (!g_wide) logf("Storefront error text: SetDisplayStringWide isn't as expected; popups show plain letters only");
    bool popup = api->hook_function(mod, "Storefront error text", reinterpret_cast<void*>(base + kStorefrontErrorText),
                                    reinterpret_cast<void*>(&error_text_detour), reinterpret_cast<void**>(&g_error_text_orig)) != nullptr;
    if (!popup) logf("Storefront error text: not hooked; its popups keep the game's own messages");
    return popup;
}

void notify_request_failed(uint32_t hr, const char* text, bool toast, const char* line) {
    if (!g_api || !text || !*text) return;
    std::lock_guard<std::mutex> lk(g_mu);
    DWORD now = GetTickCount();
    g_failure = Failure{hr, text, line && *line ? line : text, now ? now : 1, toast};
    logf("request failed (0x%08lX): %s", (unsigned long)hr, text);
}
