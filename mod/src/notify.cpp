#include "notify.hpp"

#include "fh2game.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace {

const FH2ModApi* g_api  = nullptr;
FH2Mod*          g_mod  = nullptr;
uintptr_t        g_base = 0;

// HUDMinorNotificationComponent's vtable: the constructor and destructor store it just before the instructions
// hooked below, so it tells a real component from anything else in rbx.
constexpr uintptr_t kComponentVtable = 0x2CD7B70;

// ShowNotification(component, title, text, icon): each a display string (a ref-counted object, vtable +0x10 AddRef,
// +0x18 Release) passed by value, so the callee releases them. Title and text are the control's Maintext and
// Subtext; icon is its IconName ("" hides the icon). MakeDisplayString(out, text, -1) makes one, with a reference
// in *out. It allocates from the calling thread's game heap, so both run only on the game's thread (on_tick).
using Fn_MakeDisplayString = void** (*)(void** out, const char* text, int len);
using Fn_ShowNotification  = void (*)(void* component, void** title, void** text, void** icon);

// StorefrontErrorText(out, hr): the message the Storefront's error popups show for hr. It has its own text for the
// Storefront's codes (0x80514101, 0x805143xx) and hands every other code to the online services' table, which falls
// back to IDS_ERROR_GENERIC; a community server's error reply (0x8051xxxx, xxxx its code) always ends up there.
// It writes a display string with one reference to *out and returns out. Called on the UI thread.
constexpr uintptr_t kStorefrontErrorText = 0xB4D7C0;
using Fn_StorefrontErrorText = void** (*)(void** out, uint32_t hr);
Fn_StorefrontErrorText g_error_text_orig = nullptr;

constexpr size_t kMaxText     = 100;      // one line of the HUD's medium font
constexpr size_t kMaxPopup    = 300;      // the popup wraps its message
constexpr DWORD  kFailureFor  = 10000;    // a failed request explains the Storefront's error popups this long after it
constexpr DWORD  kToastAfter  = 3000;     // and if no popup asked for it by then, it shows on the HUD instead
constexpr DWORD  kShowEvery   = 5000;     // the control stays up about 4.5 s; one at a time
constexpr DWORD  kRepeatAfter = 30000;    // the same text again within this is dropped
constexpr DWORD  kKeepFor     = 120000;   // a message not shown by then (no HUD) is dropped
constexpr size_t kMaxQueued   = 6;

struct Msg { std::string title, text; DWORD queued; };

std::mutex       g_mu;              // the queue, the components, and every call into the game below
std::deque<Msg>  g_queue;
void*            g_live[4] = {};    // live components, newest last
std::string      g_last_text;
DWORD            g_last_queued = 0;
DWORD            g_last_shown  = 0;

// The last request that failed: the code the game is expected to see (0 when unknown) and what to tell the player.
struct Failure { uint32_t hr = 0; std::string text; DWORD at = 0; bool toast = true, used = false, toasted = false; };
Failure          g_failure;

void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, line);
}

std::string ascii(const char* s, size_t max = kMaxText) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
        if (*p < 0x80) out += (*p < 0x20) ? ' ' : static_cast<char>(*p);
        else if ((*p & 0xC0) != 0x80) out += '?';   // one '?' per UTF-8 character, none for its continuation bytes
    }
    if (out.size() > max) out.replace(max - 3, std::string::npos, "...");
    return out;
}

// At both hooked instructions the constructor or destructor has just written the vtable through rbx.
bool is_component(void* p) {
    return p && *static_cast<uintptr_t*>(p) == g_base + kComponentVtable;
}

void on_component_made(FH2Context* ctx, void*) {
    void* c = reinterpret_cast<void*>(ctx->rbx);
    if (!is_component(c)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (void*& slot : g_live) if (slot == c) return;
    std::memmove(g_live, g_live + 1, sizeof g_live - sizeof g_live[0]);
    g_live[3] = c;
}

void on_component_gone(FH2Context* ctx, void*) {
    void* c = reinterpret_cast<void*>(ctx->rbx);
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 3; i >= 0; i--) {
        if (g_live[i] != c) continue;
        std::memmove(g_live + 1, g_live, i * sizeof g_live[0]);
        g_live[0] = nullptr;
        break;
    }
}

void queue_locked(Msg m);

void on_tick(FH2Context*, void*) {
    DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_failure.at && g_failure.toast && !g_failure.used && !g_failure.toasted && now - g_failure.at >= kToastAfter) {
            g_failure.toasted = true;
            queue_locked(Msg{"Community Garage", ascii(g_failure.text.c_str()), now});
        }
    }
    if (g_last_shown && now - g_last_shown < kShowEvery) return;
    std::lock_guard<std::mutex> lk(g_mu);
    while (!g_queue.empty() && now - g_queue.front().queued > kKeepFor) {
        logf("notification dropped (the HUD wasn't shown in time): %s", g_queue.front().text.c_str());
        g_queue.pop_front();
    }
    void* component = g_live[3];
    if (g_queue.empty() || !component) return;
    Msg m = std::move(g_queue.front());
    g_queue.pop_front();

    auto make = reinterpret_cast<Fn_MakeDisplayString>(g_base + FH2_GAME_MAKE_DISPLAY_STRING);
    void* title = nullptr;
    void* text  = nullptr;
    void* icon  = nullptr;
    make(&title, m.title.c_str(), -1);
    make(&text, m.text.c_str(), -1);
    make(&icon, "", -1);
    reinterpret_cast<Fn_ShowNotification>(g_base + FH2_GAME_SHOW_NOTIFICATION)(component, &title, &text, &icon);
    g_last_shown = now ? now : 1;
    logf("notification shown: %s: %s", m.title.c_str(), m.text.c_str());
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
    reinterpret_cast<Fn_MakeDisplayString>(g_base + FH2_GAME_MAKE_DISPLAY_STRING)(out, ascii(text.c_str(), kMaxPopup).c_str(), -1);
    logf("Storefront error popup for 0x%08lX: \"%s\"", (unsigned long)hr, text.c_str());
    return out;
}

bool code_is(uintptr_t off, const char* bytes, size_t n) {
    return std::memcmp(reinterpret_cast<const void*>(g_base + off), bytes, n) == 0;
}
#define CODE_IS(name) code_is(name, name##_BYTES, sizeof name##_BYTES - 1)

// "notify <text>" from fh2mod.exe or the command pipe: shows <text> the way an error would be, to try the pop-up.
void on_notify_command(void*, const char*, const char* args, char* reply, size_t reply_size) {
    if (!args || !*args) { std::snprintf(reply, reply_size, "error: usage: notify <text>"); return; }
    notify_player("Community Garage", args);
    std::snprintf(reply, reply_size, "queued; it shows on the driving HUD (Minor Notifications on)");
}

} // namespace

bool notify_install(const FH2ModApi* api, FH2Mod* mod, uintptr_t base) {
    g_api  = api;
    g_mod  = mod;
    g_base = base;
    // Only the functions called here are checked in memory. The hooked places may already carry another mod's hook
    // (the Achievements mod hooks the same three instructions), and the platform checks those against the exe on disk.
    if (!CODE_IS(FH2_GAME_MAKE_DISPLAY_STRING)) {
        logf("error messages: the game code isn't as expected; they stay in the log only");
        return false;
    }
    bool popup = api->hook_function(mod, "Storefront error text", reinterpret_cast<void*>(base + kStorefrontErrorText),
                                    reinterpret_cast<void*>(&error_text_detour), reinterpret_cast<void**>(&g_error_text_orig)) != nullptr;
    if (!popup) logf("Storefront error text: not hooked; its popups keep the game's own messages");
    bool hud = CODE_IS(FH2_GAME_SHOW_NOTIFICATION) &&
               api->hook_instruction(mod, "HUD notification made", reinterpret_cast<void*>(base + FH2_GAME_HUD_NOTIFICATION_CTOR_AT),
                                     on_component_made, nullptr) &&
               api->hook_instruction(mod, "HUD notification gone", reinterpret_cast<void*>(base + FH2_GAME_HUD_NOTIFICATION_DTOR_AT),
                                     on_component_gone, nullptr) &&
               api->hook_instruction(mod, "notification tick", reinterpret_cast<void*>(base + FH2_GAME_ACHIEVEMENTSMODULE_TICK_AT),
                                     on_tick, nullptr);
    if (!hud) logf("HUD notifications: not all hooks could be made; messages without a popup stay in the log");
    if (api->api_version >= 9) api->add_command(mod, "notify", "notify <text>: show <text> as a HUD notification", on_notify_command, nullptr);
    return popup && hud;
}

namespace {
void queue_locked(Msg m) {
    if (m.text == g_last_text && m.queued - g_last_queued < kRepeatAfter) return;
    g_last_text   = m.text;
    g_last_queued = m.queued;
    if (g_queue.size() >= kMaxQueued) g_queue.pop_front();
    logf("notification queued: %s: %s", m.title.c_str(), m.text.c_str());
    g_queue.push_back(std::move(m));
}
} // namespace

void notify_player(const char* title, const char* text) {
    if (!g_api || !text || !*text) return;
    std::lock_guard<std::mutex> lk(g_mu);
    queue_locked(Msg{ascii(title ? title : ""), ascii(text), GetTickCount()});
}

void notify_request_failed(uint32_t hr, const char* text, bool toast) {
    if (!g_api || !text || !*text) return;
    std::lock_guard<std::mutex> lk(g_mu);
    DWORD now = GetTickCount();
    g_failure = Failure{hr, text, now ? now : 1, toast};
    logf("request failed (0x%08lX): %s", (unsigned long)hr, text);
}
