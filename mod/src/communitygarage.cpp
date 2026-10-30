// CommunityGarage: points FH2's online services at a community server so the
// game's own Storefront (liveries, tunes) can work again.
//
// The game keeps its service endpoints in a static table in .data: one entry
// per environment, each {section, key, value} triples for the host
// ("TestLspProvider"/"unsecureAddress"), the port and the URL path
// ("HttpProtocolFactory"). The retail environment is "FH2Release" (and its
// ".Trial" twin). We swap the host and port pointers of those two entries for
// strings of our own, from communitygarage\server.ini next to azimuth.exe.
#include "fh2mod.h"
#include "notify.hpp"
#include "xhr_bridge.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod          = nullptr;

constexpr uintptr_t kImageBase = 0x140000000;

struct Entry {
    const char* name;
    uintptr_t   host_slot;   // VA of the pointer to the host string
    uintptr_t   port_slot;   // VA of the pointer to the port string
};

// Table at 0x142F65F50, 0x50 bytes per environment; host at +0x10, port at +0x28.
const Entry kEntries[] = {
    {"FH2Release",       0x142F66280, 0x142F66298},
    {"FH2Release.Trial", 0x142F662D0, 0x142F662E8},
};
const char kHostExpect[] = "gameservices.FH2.forzamotorsport.net";
const char kPortExpect[] = "443";

// Must outlive the game's use of the pointers: static storage.
char g_host[128];
char g_port[16];
BridgeConfig g_bridge;
volatile LONG g_share_tick = 0;     // tick count when the last share attempt started (0 = none)
bool g_fake_token = true;       // experiment: Azimuth token requests complete successfully (see on_az_token_op)
bool g_debug      = false;
bool g_server_names = true;     // [drivatars] names=server: Drivatar names from the community server (see load_drivatar_names)
bool drivatar_names_from_server(const std::wstring& ini);      // [debug] trace=1: diagnostic hooks and verbose logging (off for normal play)
bool g_econ_scan  = false;      // experiment: scan memory for the economy object (see scan_economy)
bool g_econ_probe = false;      // experiment: [experiments] economy_probe=1, also runs the scan
bool g_token_text = false;      // experiment: non-empty Xbox token (see on_token_strings)
int  g_skip_streams = -1;       // [azimuth] skip_livery_texture_streams: 1 on, 0 off, unset = on (install_pack_skip)

void logf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, line);
}

bool string_at_pointer(uintptr_t base, uintptr_t slot_va, const char* expect) {
    auto* slot = reinterpret_cast<const uintptr_t*>(base + (slot_va - kImageBase));
    auto* str  = reinterpret_cast<const char*>(*slot);
    if (!str) return false;
    return std::strncmp(str, expect, std::strlen(expect) + 1) == 0;
}

bool redirect(uintptr_t base, uintptr_t slot_va, const char* what, const char* expect, const char* value) {
    if (!string_at_pointer(base, slot_va, expect)) {
        logf("%s: the table doesn't look as expected, leaving it alone", what);
        return false;
    }
    void* at           = reinterpret_cast<void*>(base + (slot_va - kImageBase));
    uint64_t new_value = reinterpret_cast<uint64_t>(value);
    FH2Hook* h         = g_api->patch_bytes(g_mod, what, at, nullptr, &new_value, sizeof new_value);
    return h && g_api->enable_hooks(g_mod, h);
}

bool load_config(const wchar_t* dir) {
    const std::wstring ini = std::wstring(dir) + L"/communitygarage/server.ini";
    wchar_t host[128] = {}, port[16] = {};
    GetPrivateProfileStringW(L"server", L"host", L"", host, 128, ini.c_str());
    GetPrivateProfileStringW(L"server", L"port", L"443", port, 16, ini.c_str());
    WideCharToMultiByte(CP_UTF8, 0, host, -1, g_host, sizeof g_host, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, port, -1, g_port, sizeof g_port, nullptr, nullptr);
    if (!g_host[0]) {
        logf("no host in communitygarage/server.ini; not redirecting (add [server] host=...)");
        return false;
    }
    wchar_t scheme[16] = {};
    GetPrivateProfileStringW(L"server", L"scheme", L"auto", scheme, 16, ini.c_str());
    {
        wchar_t prefix[128] = {};
        GetPrivateProfileStringW(L"server", L"path_prefix", L"", prefix, 128, ini.c_str());
        std::wstring p = prefix;
        while (!p.empty() && p.back() == L'/') p.pop_back();
        if (!p.empty() && p.front() != L'/') p.insert(p.begin(), L'/');
        g_bridge.path_prefix = p;
    }
    g_bridge.host         = host;
    g_bridge.port         = _wtoi(port) > 0 ? _wtoi(port) : 443;
    g_bridge.scheme       = !_wcsicmp(scheme, L"https") ? 2 : !_wcsicmp(scheme, L"http") ? 1 : 0;
    g_bridge.insecure_tls = GetPrivateProfileIntW(L"server", L"insecure_tls", 0, ini.c_str()) != 0;
    // Account: a random key made on first run identifies this install to the server; the display name is the
    // gamertag Azimuth already knows (data/azimuth.ini player_name) unless [account] name= says otherwise.
    // They live in communitygarage/account.ini, a file no release ships, so an update that replaces server.ini can't
    // wipe them. An account.ini without a key adopts the [account] key/name of an older server.ini.
    {
        const std::wstring acc = std::wstring(dir) + L"/communitygarage/account.ini";
        wchar_t key[80] = {}, name[64] = {};
        GetPrivateProfileStringW(L"account", L"key", L"", key, 80, acc.c_str());
        if (wcslen(key) < 32) {
            wchar_t old_key[80] = {}, old_name[64] = {};
            GetPrivateProfileStringW(L"account", L"key", L"", old_key, 80, ini.c_str());
            GetPrivateProfileStringW(L"account", L"name", L"", old_name, 64, ini.c_str());
            if (wcslen(old_key) >= 32) {
                WritePrivateProfileStringW(L"account", L"key", old_key, acc.c_str());
                if (old_name[0]) WritePrivateProfileStringW(L"account", L"name", old_name, acc.c_str());
                GetPrivateProfileStringW(L"account", L"key", L"", key, 80, acc.c_str());
                if (!wcscmp(key, old_key)) {      // only drop the old copy once the new file reads back the same
                    WritePrivateProfileStringW(L"account", L"key", nullptr, ini.c_str());
                    WritePrivateProfileStringW(L"account", L"name", nullptr, ini.c_str());
                    logf("moved the account key and name from communitygarage/server.ini to communitygarage/account.ini");
                } else {
                    logf("couldn't write communitygarage/account.ini; leaving the key in server.ini");
                    wcscpy_s(key, old_key);
                }
            } else {
                uint8_t raw[16] = {};
                BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                for (int i = 0; i < 16; i++) std::swprintf(key + i * 2, 3, L"%02x", raw[i]);
                WritePrivateProfileStringW(L"account", L"key", key, acc.c_str());
                logf("made a new account key (saved in communitygarage/account.ini; keep it private)");
            }
        }
        GetPrivateProfileStringW(L"account", L"name", L"", name, 64, acc.c_str());
        if (!name[0]) GetPrivateProfileStringW(L"account", L"name", L"", name, 64, ini.c_str());   // migration fallback
        if (!name[0]) {
            const std::wstring az = std::wstring(dir) + L"/data/azimuth.ini";
            GetPrivateProfileStringW(L"launcher", L"player_name", L"", name, 64, az.c_str());
        }
        char key8[80] = {}, name8[200] = {};
        WideCharToMultiByte(CP_UTF8, 0, key, -1, key8, sizeof key8, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, name, -1, name8, sizeof name8, nullptr, nullptr);
        g_bridge.key  = key8;
        g_bridge.name = name8;
    }
    g_debug               = GetPrivateProfileIntW(L"debug", L"trace", 0, ini.c_str()) != 0;
    g_bridge.debug        = g_debug;
    g_econ_scan           = GetPrivateProfileIntW(L"experiments", L"economy_scan", 0, ini.c_str()) != 0;
    g_econ_probe          = GetPrivateProfileIntW(L"experiments", L"economy_probe", 0, ini.c_str()) != 0;
    // Mods page "Use server economy values": off makes the mod fail GetEconomySettings, so the game keeps its own numbers.
    g_bridge.fail_economy = g_econ_probe || GetPrivateProfileIntW(L"economy", L"use_server_values", 1, ini.c_str()) == 0;
    g_fake_token          = GetPrivateProfileIntW(L"experiments", L"fake_token", 1, ini.c_str()) != 0;
    g_token_text          = GetPrivateProfileIntW(L"experiments", L"token_text", 1, ini.c_str()) != 0;
    g_skip_streams        = static_cast<int>(GetPrivateProfileIntW(L"azimuth", L"skip_livery_texture_streams", static_cast<UINT>(-1), ini.c_str()));
    g_server_names        = drivatar_names_from_server(ini);
    return true;
}

// ---- Drivatar names: [drivatars] names=server (the default) takes the community's players' names from the server at
// startup; names=off (or the older "own") keeps the game's own AI driver names, as does a server that can't be reached
// (the few drivers the game looks up by Xbox id are then "Racer").
bool drivatar_names_from_server(const std::wstring& ini) {
    wchar_t v[16] = {};
    GetPrivateProfileStringW(L"drivatars", L"names", L"server", v, 16, ini.c_str());
    return _wcsicmp(v, L"off") != 0 && _wcsicmp(v, L"own") != 0;
}

// ---- Putting the names on the AI drivers, in memory only: no file is written.
// The Drivatars are the 125 records of media\drivatarglobalcache\DefaultCache.bin (u32 version 4, u32 count, records of
// 0x2658 bytes, each starting with the driver's gamertag as UTF-16 in a 32-byte field). The loader (game+0xA2D0E0) reads
// the records straight into its list at this+0x4D78 {begin, end}; at game+0xA2D2C2 the read is done, and the 25 gamertags
// (five records each, in the file in IDS_Name_1..25 order) are renamed there. Car meets and some road-trip traffic are
// named from AIPlayers.str (IDS_Name_1..25, and IDS_Name_313 "HRZN Ben"); every string lookup goes through
// game+0x123D780 (manager, table index, key) -> text, and the lookups of those 26 keys in that table get the same names.
// That table's index is set at runtime, so it's recognised by its text: the game's own name for the key.
struct AiName { uint16_t key; const wchar_t* name; };
constexpr AiName kAiNames[] = {   // IDS_Name_1..25, then IDS_Name_313
    {0x290B, L"Nocomply76"}, {0x288B, L"Super Final"}, {0x280B, L"FlawlessC0wboy"}, {0x2B8B, L"Lord of Ward86"},
    {0x2B0B, L"Richiiieee"}, {0x2A8B, L"AcidCatFish"}, {0x2A0B, L"Van182"}, {0x2D8B, L"Rofelloswow"}, {0x2D0B, L"adetheterrible"},
    {0x9D94, L"D1LL0W88"}, {0x9D14, L"geekmonkey13"}, {0x9C94, L"jay8ee"}, {0x9C14, L"Atomique Banana"}, {0x9F94, L"Novatide"},
    {0x9F14, L"ton3s"}, {0x9E94, L"ajmiles"}, {0x9E14, L"JDLOKI"}, {0x9994, L"Womgoose"}, {0x9914, L"Detune"}, {0x5D94, L"TPeza"},
    {0x5D14, L"czphillips"}, {0x5C94, L"T4tty T3ddy"}, {0x5C14, L"Tolomandy"}, {0x5F94, L"TheMonsterMash"}, {0x5F14, L"TheCraigFace"},
    {0x938E, L"HRZN Ben"}};
constexpr size_t kAiCount = sizeof kAiNames / sizeof kAiNames[0];
constexpr const char* kDrivatarCache = "drivatarglobalcache/DefaultCache.bin";
constexpr size_t      kCacheRecord = 0x2658, kCacheName = 32;

std::wstring g_ai_text[kAiCount];    // the community name for each slot; set once before the hooks are made
bool         g_ai_names_on = false;
volatile signed char g_ai_table[1024];   // per string table index: 0 not yet seen, 1 AIPlayers.str, -1 another table

int ai_slot(uint32_t key) {
    switch (key & 0xFF) { case 0x0B: case 0x8B: case 0x14: case 0x94: case 0x8E: break; default: return -1; }   // cheap reject
    for (size_t i = 0; i < kAiCount; i++) if (kAiNames[i].key == (key & 0xFFFF)) return (int)i;
    return -1;
}

using Fn_LocLookup = const wchar_t* (*)(void* manager, uint32_t table, uint32_t key);
Fn_LocLookup g_loc_lookup_orig = nullptr;
const wchar_t* loc_lookup_detour(void* manager, uint32_t table, uint32_t key) {
    int slot = ai_slot(key);
    uint32_t t = table & 0xFFFF;
    if (slot < 0 || t >= sizeof g_ai_table) return g_loc_lookup_orig(manager, table, key);
    signed char known = g_ai_table[t];
    if (known < 0) return g_loc_lookup_orig(manager, table, key);
    if (known == 0) {
        const wchar_t* text = g_loc_lookup_orig(manager, table, key);
        bool ai = text && !std::wcscmp(text, kAiNames[slot].name);
        g_ai_table[t] = ai ? 1 : -1;
        if (ai) logf("drivatar names: string table %u is AIPlayers.str", t);
        if (!ai) return text;
    }
    return g_ai_text[slot].c_str();
}

bool safe_read(const void* p, void* out, size_t n);

// game+0xA2D2C2, in the Drivatar cache loader just after the records were read: r14 = &list {begin, end}.
void on_drivatars_loaded(FH2Context* ctx, void*) {
    uint8_t** list = reinterpret_cast<uint8_t**>(ctx->r14);
    uint8_t *begin = nullptr, *end = nullptr;
    if (!safe_read(list, &begin, sizeof begin) || !safe_read(list + 1, &end, sizeof end) || !begin || end <= begin) return;
    size_t bytes = static_cast<size_t>(end - begin);
    if (bytes % kCacheRecord || bytes / kCacheRecord > 1000) { logf("drivatar names: the Drivatar list isn't as expected (%zu bytes)", bytes); return; }
    size_t count = bytes / kCacheRecord;
    std::vector<std::wstring> seen;   // the gamertags in the order they first appear: slot 0..24
    for (size_t i = 0; i < count; i++) {
        wchar_t* field = reinterpret_cast<wchar_t*>(begin + i * kCacheRecord);
        std::wstring name(field, wcsnlen(field, kCacheName / 2));
        auto it = std::find(seen.begin(), seen.end(), name);
        size_t slot = static_cast<size_t>(it - seen.begin());
        if (it == seen.end()) seen.push_back(name);
        const std::wstring& to = g_ai_text[slot < kAiCount - 1 ? slot : slot % (kAiCount - 1)];
        std::memset(field, 0, kCacheName);
        std::wmemcpy(field, to.c_str(), std::min<size_t>(to.size(), kCacheName / 2 - 1));
    }
    logf("drivatar names: %zu Drivatars (%zu records) renamed in memory", seen.size(), count);
}

// The names for the AI drivers (repeated when there are fewer than 26), or none: the game keeps its own. Also removes
// what an earlier version wrote to files (a DefaultCache.bin override and AIPlayers.str edits).
void set_ai_names(const std::vector<std::string>& names) {
    g_api->clear_file(g_mod, kDrivatarCache);
    g_api->clear_strings(g_mod, "AIPlayers.str");
    g_api->apply_strings(g_mod);
    if (names.empty()) return;
    for (size_t i = 0; i < kAiCount; i++) {
        const std::string& n = names[i % names.size()];
        int len = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), (int)n.size(), nullptr, 0);
        g_ai_text[i].assign(len, L' ');
        MultiByteToWideChar(CP_UTF8, 0, n.c_str(), (int)n.size(), g_ai_text[i].data(), len);
        if (g_ai_text[i].size() > kCacheName / 2 - 1) g_ai_text[i].resize(kCacheName / 2 - 1);
    }
    g_ai_names_on = true;
}

// ---- Rivals. The game only opens Rivals when RivalsModule (game+0x34D6348) has its flag at +0xF0 set: States::
// IsRivalsAccessible (game+0x8A4200) and some 20 other places read it, and nothing in an offline game ever sets it, so
// Rivals says "cannot connect to Xbox Live". With the community server answering the Rivals calls (and its Logon reply
// enabling the features 0x56-0x5B), the mod sets it, and puts it back if the game clears it.
DWORD WINAPI rivals_unlock_thread(LPVOID param) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(param);   // the game's base
    bool logged = false;
    for (;;) {
        uint8_t* module = nullptr;
        uint8_t flag = 1;
        if (safe_read(reinterpret_cast<void*>(base + 0x34D6348), &module, sizeof module) && module &&
            safe_read(module + 0xF0, &flag, 1) && flag == 0) {
            *reinterpret_cast<volatile uint8_t*>(module + 0xF0) = 1;
            if (!logged) { logf("Rivals: switched on (RivalsModule+0xF0)"); logged = true; }
        }
        Sleep(1000);
    }
}

// The two hooks, made and switched on the moment the game's code is ready, before anything else the mod sets up: the
// game reads the Drivatar list within its first seconds (only when there are names to put on).
void install_ai_name_hooks(uintptr_t base) {
    if (!g_ai_names_on) return;
    FH2Hook* loaded = g_api->hook_instruction(g_mod, "Drivatar list loaded", reinterpret_cast<void*>(base + (0x140A2D2C2 - kImageBase)),
                                              on_drivatars_loaded, nullptr);
    FH2Hook* lookup = g_api->hook_function(g_mod, "string lookup (AI driver names)", reinterpret_cast<void*>(base + (0x14123D780 - kImageBase)),
                                           reinterpret_cast<void*>(&loc_lookup_detour), reinterpret_cast<void**>(&g_loc_lookup_orig));
    if (loaded) g_api->enable_hooks(g_mod, loaded);
    if (lookup) g_api->enable_hooks(g_mod, lookup);
}

void load_drivatar_names() {
    std::vector<std::string> names;
    if (g_server_names && xhr_bridge_fetch_drivatar_names(&names)) {
        // The server leaves out the player's own name already; this also catches an older server, or a name that was
        // changed in account.ini since. Compared without a "#N" suffix and without regard to case.
        auto base = [](std::string s) {
            size_t hash = s.rfind('#');
            if (hash != std::string::npos && hash + 1 < s.size() && s.find_first_not_of("0123456789", hash + 1) == std::string::npos) s.erase(hash);
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string self = base(g_bridge.name);
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& n) { return !self.empty() && base(n) == self; }),
                    names.end());
    }
    if (!names.empty()) logf("drivatar names: %zu from the server", names.size());
    else logf("drivatar names: %s", g_server_names ? "none from the server; the game's own names" : "off; the game's own names");
    set_ai_names(names);
    xhr_bridge_set_drivatar_names(std::move(names));
}

// ---- Azimuth (0.9.10 through 1.0.3) stops ("CreatePlacementTexture2D requires GPU-write/CPU-map/unknown placement semantics") when
// CLiveryRenderManager::SaveRenderToStream (game+0x122E2C0) packs the texture streams of a fully composed livery: its
// packer game+0x122E450 asks for a 1024x1024 CPU-readable placement texture that Azimuth doesn't implement. The job runs
// after every full layer composition (modes 2 and 4 always, mode 5 once all layer groups are composed), and once the
// game is logged on, the drivatar livery loader composes the liveries it gets from the server, so a session dies a few
// minutes after logon. The packer is the only step that needs that texture and it already has an early-out for a
// resource without a stream ([res+0x328] == 0): skipping it leaves the rest untouched, the job still posts event
// 0x10013 and CCarLivery moves on at once. Manual Share packs through slot 21 (game+0x122DF20) and is not affected.
// server.ini [azimuth] skip_livery_texture_streams=0 turns this off (e.g. on an Azimuth build that supports it).
bool safe_read(const void* p, void* out, size_t n);
void (*g_pack_orig)(void*) = nullptr;
void pack_skip(void* res) {
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 5)
        logf("livery texture streams: pack skipped for resource %p (Azimuth has no CPU-readable placement textures) (#%ld)", res, n);
}
// Every Azimuth build so far lacks CPU-readable placement textures (0.9.10 asked for 1024x1024; 1.0.3 stops the same
// way on a 4096x4096 13-mip one, from this packer, while composing drivatar liveries), so the pack is skipped on all
// builds unless server.ini says otherwise.
void install_pack_skip(uintptr_t base) {
    bool on = g_skip_streams != 0;
    if (!on) { logf("livery texture streams: left as the game does it ([azimuth] skip_livery_texture_streams=0)"); return; }
    static const uint8_t expect[] = {0x40, 0x55, 0x56, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x70};   // push rbp; push rsi; push rdi; mov rbp,rsp; sub rsp,0x70
    uint8_t got[sizeof expect] = {};
    void* at = reinterpret_cast<void*>(base + (0x14122E450 - kImageBase));
    if (!safe_read(at, got, sizeof got) || std::memcmp(got, expect, sizeof expect) != 0) {
        logf("livery texture streams: game+0x122E450 doesn't look as expected; not hooking (the game keeps packing them)");
        return;
    }
    FH2Hook* h = g_api->hook_function(g_mod, "CLiveryRenderManager::SaveRenderToStream pack (skipped: Azimuth has no CPU-readable placement textures)", at,
                                      reinterpret_cast<void*>(&pack_skip), reinterpret_cast<void**>(&g_pack_orig));
    if (!h) logf("livery texture streams: hook failed; the game keeps packing them");
}

// ---- Diagnostics: log-only hooks, to learn why the Storefront says "server not available".

uintptr_t g_base = 0;

// MSVC std::string (x64): 16-byte SSO buffer, size at +0x10, capacity at +0x18.
std::string read_std_string(const uint8_t* s) {
    size_t size = *reinterpret_cast<const size_t*>(s + 0x10);
    size_t cap  = *reinterpret_cast<const size_t*>(s + 0x18);
    if (size > 256) return "<?>";
    const char* data = cap >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    return std::string(data, size);
}

// HttpProtocolFactory-style provider: Configure(self, config) at 0x141B7C0E0.
// Fields it fills (see docs/re-notes.md): port u16 +0x28, url string +0x08,
// UseHTTPS u8 +0x2A, host string +0x30.
bool (*g_configure_orig)(uint8_t*, void*) = nullptr;
bool configure_detour(uint8_t* self, void* config) {
    bool r = g_configure_orig(self, config);
    if (self) {
        logf("HTTP provider configured: host=%s port=%u url=%s https=%d (ok=%d)",
             read_std_string(self + 0x30).c_str(), *reinterpret_cast<uint16_t*>(self + 0x28),
             read_std_string(self + 0x08).c_str(), self[0x2A], r);
    }
    return r;
}

// "Feature unavailable" popup builder at 0x140DDD630: (page, reason).
void (*g_popup_orig)(void*, uint32_t) = nullptr;
void popup_detour(void* page, uint32_t reason) {
    logf("feature-locked popup: reason=%u, called from game+0x%llX", reason,
         (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_base));
    g_popup_orig(page, reason);
}

// Hex-dump 0x60 bytes behind an argument that looks like a pointer, read safely.
void dump_arg(const char* fn, const char* reg, uint64_t v) {
    if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) return;
    uint8_t buf[0x60];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(v), buf, sizeof buf, &got) || got < 16) return;
    for (SIZE_T off = 0; off < got; off += 32) {
        char line[160];
        int len = std::snprintf(line, sizeof line, "  %s %s+%02llx:", fn, reg, (unsigned long long)off);
        for (SIZE_T i = off; i < off + 32 && i < got; i++) len += std::snprintf(line + len, sizeof line - len, " %02x", buf[i]);
        g_api->log(g_mod, line);
    }
}

// Instruction hooks at the three "GetAuthToken failed" log sites, to see which fires and why.
void on_token_failure(FH2Context* ctx, void* user) {
    const char* what = static_cast<const char*>(user);
    logf("GetAuthToken FAILED: %s (r9=%08x edx=%08x)", what, (unsigned)ctx->r9, (unsigned)ctx->rdx);
}

// The Xbox token request fails here (Azimuth can't reach Xbox Live: 0x800704CF), and the
// handler then calls the completion callback with (error, "", ""). Turn the error into
// success so the game's LSP authenticator carries on with an empty token; our server
// doesn't check it. Hooked just before `mov rcx,[rcx+0x10]` at game+0x1C541A5, where edx
// holds the code that the call at +0x1C541EE passes to the callback.
void on_token_callback(FH2Context* ctx, void*) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    if (k <= 5) logf("token request failed with %08x: reporting success with an empty token (#%ld)",
                     (unsigned)ctx->rdx, k);
    ctx->rdx = 0;
}

// ---- Azimuth's token request. Its user object answers every GetTokenAndSignatureAsync with an async operation
// that is already failed with ERROR_NETWORK_UNREACHABLE (Azimuth.OfflineTokenRequest). The game's own token
// callback tolerates that (on_token_callback), but the Xbox Services library the share goes through does not: its
// privacy check (can this player share user content?) needs a token first. Turn that failure into a completed
// operation holding a result object whose every property reads "CTUNE" (an HSTRING).
//
// Azimuth builds the operation in a constructor (azimuth+0xD6E50: this, type info, hr, result*, name); the offline
// producer calls it with hr = 0x800704CF and a null result at azimuth+0xD3EAD. The hook swaps in hr = 0 and a
// pointer to our result object, which the constructor stores as an interface pointer (kind 3).
using Fn_WindowsCreateString    = HRESULT (WINAPI*)(const wchar_t*, UINT32, void**);
using Fn_WindowsDuplicateString = HRESULT (WINAPI*)(void*, void**);
void* g_hstr = nullptr;
Fn_WindowsDuplicateString g_dup_hstr = nullptr;

HRESULT STDMETHODCALLTYPE fr_qi(void* self, const void*, void** out) { if (!out) return E_POINTER; *out = self; return S_OK; }
ULONG STDMETHODCALLTYPE fr_addref(void*) { return 2; }
ULONG STDMETHODCALLTYPE fr_release(void*) { return 2; }
HRESULT STDMETHODCALLTYPE fr_iids(void*, ULONG* n, void** iids) { if (n) *n = 0; if (iids) *iids = nullptr; return S_OK; }
HRESULT STDMETHODCALLTYPE fr_class(void*, void** out) { if (out) *out = nullptr; return S_OK; }
HRESULT STDMETHODCALLTYPE fr_trust(void*, int* level) { if (level) *level = 0; return S_OK; }
template <int I>
HRESULT STDMETHODCALLTYPE fr_prop(void*, void** out) {
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 3) logf("fake token result: property slot %d read", I);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (g_dup_hstr && g_hstr) g_dup_hstr(g_hstr, out);
    return S_OK;
}
template <size_t... I>
void fill_props(void** vt, std::index_sequence<I...>) { ((vt[6 + I] = reinterpret_cast<void*>(&fr_prop<static_cast<int>(I)>)), ...); }

void* g_fake_vtbl[6 + 40];
void* g_fake_obj[2] = {g_fake_vtbl, nullptr};
void* g_fake_result = g_fake_obj;                   // the interface pointer Azimuth's operation will hold

void init_fake_token() {
    // Azimuth has its own HSTRING implementation (it rejects handles that did not come from it, "invalid local
    // string handle"), so combase strings crash the game. Properties read as the empty string (null handle).
    if (HMODULE cb = nullptr) {
        auto create = reinterpret_cast<Fn_WindowsCreateString>(GetProcAddress(cb, "WindowsCreateString"));
        g_dup_hstr  = reinterpret_cast<Fn_WindowsDuplicateString>(GetProcAddress(cb, "WindowsDuplicateString"));
        if (create) create(L"CTUNE", 5, &g_hstr);
    }
    g_fake_vtbl[0] = reinterpret_cast<void*>(&fr_qi);
    g_fake_vtbl[1] = reinterpret_cast<void*>(&fr_addref);
    g_fake_vtbl[2] = reinterpret_cast<void*>(&fr_release);
    g_fake_vtbl[3] = reinterpret_cast<void*>(&fr_iids);
    g_fake_vtbl[4] = reinterpret_cast<void*>(&fr_class);
    g_fake_vtbl[5] = reinterpret_cast<void*>(&fr_trust);
    fill_props(g_fake_vtbl, std::make_index_sequence<40>{});
}

void on_az_token_op(FH2Context* ctx, void*) {
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 5) logf("Azimuth token request: completing it with a fake result (#%ld, hr was %08x)", n, (unsigned)ctx->r8);
    ctx->r8 = 0;
    ctx->r9 = reinterpret_cast<uint64_t>(&g_fake_result);
}

// Where does the parser raise 0x8050005C? Hook the unconditional "mov reg, 0x8050005C" sites
// (the begin/end type checks use conditional moves and aren't included).
const uintptr_t kRaiseSites[] = {
    0x140EE3D6B, 0x140EE3DE1, 0x140EE3EDD, 0x140EE3F6F, 0x140EE3FF2, 0x140EE4071, 0x140EE40F4, 0x140EE41E2,
    0x140EE41FD, 0x140EE4291, 0x140EE43AD, 0x140EE442E, 0x140EE450E, 0x140EE5654, 0x140EE5862, 0x140EE5AAD,
    0x140EE5BB0, 0x140EE6365, 0x140EEF1DA, 0x140EEFE01, 0x140EFCBD4, 0x140F01B9E, 0x140F03733, 0x140F037CF,
    0x140F04464, 0x14198ECFC, 0x14198F585,
};
void on_raise_5c(FH2Context* ctx, void* user) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    if (k <= 20)
        logf("0x8050005C raised at game+0x%llX (rcx=%llx rdx=%llx rsi=%llx rdi=%llx)",
             (unsigned long long)(reinterpret_cast<uintptr_t>(user) - kImageBase), (unsigned long long)ctx->rcx,
             (unsigned long long)ctx->rdx, (unsigned long long)ctx->rsi, (unsigned long long)ctx->rdi);
}

// DateTime fields travel as ISO-8601 strings. The reader (game+0xEE5A30) reads the string into a
// UTF-16 buffer at reader+0x290 via game+0xEE5FD0, then parses it; an empty string (as in an all-zero
// body) fails with 0x8050005C. Hooked right after that read (game+0xEE5A57, rbx = the buffer): if the
// string is empty, give it a valid default date. Real replies carry real dates and are left alone.
void on_empty_datetime(FH2Context* ctx, void*) {
    if (static_cast<int32_t>(ctx->rax) < 0) return;           // the read itself failed
    auto* buf = reinterpret_cast<wchar_t*>(ctx->rbx);
    if (buf && buf[0] == 0) {
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 5) logf("empty DateTime in a reply: substituting a default date (#%ld)", n);
        std::memcpy(buf, L"2015-11-24T00:00:00Z", sizeof L"2015-11-24T00:00:00Z");
    }
}

// The car builder (game+0x1056210) takes a car setup record in r8: u32 carId, two u32 swap fields, then 43 u32 upgrade part ids,
// and at +0x168 a byte "use stock parts". Car-level part ids are carId*1000 + n, so a tune made on another car (the game will
// apply one from My Files to the current car) names parts this car doesn't have; the drivetrain lookup then comes back empty and
// the game crashes at game+0x1056BAE. Hooked at `mov rdi, r9` (game+0x1056263), before the record is read: if a car-level part
// belongs to another car, hand the builder a copy with the stock-parts flag set. The function reads only those fields of it.
bool safe_read(const void* p, void* out, size_t n);
void on_car_build(FH2Context* ctx, void*) {
    const auto* rec = reinterpret_cast<const uint8_t*>(ctx->r8);
    uint32_t car = 0;
    uint8_t stock = 0;
    if (!rec || !safe_read(rec, &car, 4) || !safe_read(rec + 0x168, &stock, 1) || stock || !car) return;
    uint32_t parts[43];
    if (!safe_read(rec + 12, parts, sizeof parts)) return;
    uint32_t foreign = 0;
    for (int i : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 34, 35, 36, 37, 38, 39, 40, 41})   // car-level slots (12..33: engine, drivetrain)
        if (parts[i] != 0xFFFFFFFFu && parts[i] / 1000 != car) { foreign = parts[i] / 1000; break; }
    if (!foreign) return;
    thread_local uint8_t copy[0x170];
    std::memcpy(copy, rec, sizeof copy);
    copy[0x168] = 1;
    ctx->r8 = reinterpret_cast<uint64_t>(copy);
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 10)
        logf("car %u was given upgrade parts of car %u (a tune made for another car); building it with stock parts instead", car, foreign);
}

// ---- Reply-layout probes. The reader walks the reply field by field; log where each array and
// date is read (stream offset, count) so we can see where fields like notificationNonce sit.

bool safe_read(const void* p, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

// CMemStream: base at +8, cursor at +0x10.
bool stream_pos(uint8_t* reader, uint32_t* pos, uint32_t* next_be) {
    uint8_t* stream = nullptr;
    if (!safe_read(reader + 0x42B0, &stream, sizeof stream) || !stream) return false;
    uint8_t *base = nullptr, *cur = nullptr;
    if (!safe_read(stream + 8, &base, sizeof base) || !safe_read(stream + 0x10, &cur, sizeof cur)) return false;
    *pos = static_cast<uint32_t>(cur - base);
    uint32_t raw = 0;
    *next_be = safe_read(cur, &raw, sizeof raw) ? _byteswap_ulong(raw) : 0xFFFFFFFF;
    return true;
}

uint64_t (*g_array_orig)(uint8_t*, uint8_t*, void*) = nullptr;
uint64_t array_detour(uint8_t* reader, uint8_t* desc, void* out) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    uint32_t pos = 0, cnt = 0;
    if (k <= 400 && stream_pos(reader, &pos, &cnt)) {
        uint8_t* elem = nullptr;
        uint32_t esz = 0;
        if (safe_read(desc + 0x50, &elem, sizeof elem) && elem) safe_read(elem + 0x40, &esz, sizeof esz);
        logf("reply array #%ld at offset %u: count=%u elemSize=%u desc=%p", k, pos, cnt, esz, desc);
    }
    uint64_t hr = g_array_orig(reader, desc, out);
    // Keyword arrays have 136-byte elements. Dump what the game stored, to learn how the wire bytes map to fields.
    static LONG dumped = 0;
    if (static_cast<int32_t>(hr) >= 0 && out && dumped < 6) {
        uint8_t* elem = nullptr;
        uint32_t esz = 0;
        if (safe_read(desc + 0x50, &elem, sizeof elem) && elem) safe_read(elem + 0x40, &esz, sizeof esz);
        uint8_t* arr = nullptr;
        if (esz == 136 && InterlockedIncrement(&dumped) && safe_read(out, &arr, sizeof arr) && arr) {
            for (int i = 0; i < 3; i++) {
                uint8_t e[136];
                if (!safe_read(arr + i * 136, e, sizeof e)) break;
                wchar_t w[24] = {};
                std::memcpy(w, e, sizeof(w) - sizeof(wchar_t));
                char text[64] = {};
                WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text, nullptr, nullptr);
                logf("stored keyword[%d]: text=\"%s\" tail bytes @0x7c: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     i, text, e[0x7c], e[0x7d], e[0x7e], e[0x7f], e[0x80], e[0x81], e[0x82], e[0x83], e[0x84], e[0x85], e[0x86], e[0x87]);
            }
        }
    }
    return hr;
}

uint64_t (*g_date_orig)(uint8_t*, void*) = nullptr;
uint64_t date_detour(uint8_t* reader, void* out) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    uint32_t pos = 0, len = 0;
    if (k <= 100 && stream_pos(reader, &pos, &len)) logf("reply DateTime #%ld at offset %u (string length %u)", k, pos, len);
    return g_date_orig(reader, out);
}

// The logon-response listener at game+0x9C88E0 looks the response's notificationNonce up in a map and
// writes through a null entry when the nonce isn't one it issued. Skip it until replies carry real data.
void (*g_listener_orig)(void*, void*) = nullptr;
void listener_skip(void*, void*) {
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 3) logf("skipping the logon-response listener at game+0x9C88E0 (#%ld)", n);
}

// The logon response is broadcast to every listener by game+0x126C5E0 (this, response). A reply built
// from zeros leaves the notification poll intervals at 0, so the game asks for notifications every
// ~100 ms. The intervals live in the parsed response (UserLogonResponse offsets 0x1350 u16,
// 0x1354 u32, 0x1358 u32); fill sane values in before the listeners read them.
void (*g_broadcast_orig)(void*, uint8_t*) = nullptr;
void broadcast_detour(void* self, uint8_t* response) {
    static LONG n = 0;
    uint16_t poll16 = 0;
    uint32_t poll_def = 0, poll_max = 0;
    if (response && safe_read(response + 0x1350, &poll16, 2) && safe_read(response + 0x1354, &poll_def, 4) &&
        safe_read(response + 0x1358, &poll_max, 4)) {
        if (poll16 == 0) *reinterpret_cast<uint16_t*>(response + 0x1350) = 30;
        if (poll_def == 0) *reinterpret_cast<uint32_t*>(response + 0x1354) = 60;
        if (poll_max == 0) *reinterpret_cast<uint32_t*>(response + 0x1358) = 300;
        if (InterlockedIncrement(&n) <= 3)
            logf("logon response: poll intervals were %u/%u/%u s, now 30/60/300", poll16, poll_def, poll_max);
    }
    g_broadcast_orig(self, response);
}

// ---- First-call timeline of the share popup's functions: every entry point in its code range gets an
// instruction hook; each function logs once, the first time it runs, with a sequence number. The log then
// reads as the order in which the popup's code ran, which shows what happens when SHARE is pressed.
#include "popup_funcs.inc"

volatile LONG g_entry_seen[sizeof(kPopupFuncs) / sizeof(kPopupFuncs[0])];
volatile LONG g_entry_seq = 0;

void on_popup_enter(FH2Context* ctx, void* user) {
    size_t i = reinterpret_cast<size_t>(user);
    if (InterlockedExchange(&g_entry_seen[i], 1)) return;
    LONG seq = InterlockedIncrement(&g_entry_seq);
    if (seq <= 700)
        logf("popup fn #%ld: game+0x%X (rcx=%llx rdx=%llx r8=%llx)", seq, kPopupFuncs[i],
             (unsigned long long)ctx->rcx, (unsigned long long)ctx->rdx, (unsigned long long)ctx->r8);
}

// The share dispatcher (game+0x32E540) walks a chain from a singleton: three virtual calls, and fails at once
// if the last returns null. Log what each step returns (rax right after the call), and its vtable.
void on_share_chain(FH2Context* ctx, void* user) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    if (k > 24) return;
    uint64_t vptr = 0;
    if (ctx->rax > 0x10000) safe_read(reinterpret_cast<void*>(ctx->rax), &vptr, sizeof vptr);
    logf("share chain step %s: returned %llx (vtable %s game+0x%llX)", static_cast<const char*>(user),
         (unsigned long long)ctx->rax, vptr >= g_base && vptr < g_base + 0x3A18000 ? "at" : "?",
         (unsigned long long)(vptr - g_base));
}

// After each interface call in the share dispatcher, log the HRESULT and which function was called
// (as an offset into whichever module owns it: the objects come from Azimuth).
void on_share_call(FH2Context* ctx, void* user) {
    static LONG n = 0;
    if (InterlockedIncrement(&n) > 16) return;
    const auto* c = static_cast<const char*>(user);
    uint64_t obj = c[0] == 'A' ? ctx->r15 : ctx->rsi;          // 'A' = call [obj+0xb0], 'B' = call [obj+0x40]
    uint64_t slot = c[0] == 'A' ? 0xb0 : 0x40;
    uint64_t vptr = 0, fn = 0;
    if (obj > 0x10000 && safe_read(reinterpret_cast<void*>(obj), &vptr, sizeof vptr) && vptr)
        safe_read(reinterpret_cast<void*>(vptr + slot), &fn, sizeof fn);
    HMODULE owner = nullptr;
    char name[MAX_PATH] = "?";
    uint64_t off = 0;
    if (fn && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(fn), &owner) && owner) {
        GetModuleFileNameA(owner, name, sizeof name);
        off = fn - reinterpret_cast<uint64_t>(owner);
    }
    const char* base_name = std::strrchr(name, '\\');
    logf("share call %s: hr=%08x, function = %s+0x%llX (vtable %llx)", c, (unsigned)ctx->rax, base_name ? base_name + 1 : name,
         (unsigned long long)off, (unsigned long long)vptr);
    // The target is a heap thunk; show its bytes (and where a jmp/mov+jmp leads) to identify the real function.
    uint8_t code[24] = {};
    if (fn && safe_read(reinterpret_cast<void*>(fn), code, sizeof code)) {
        char hex[100] = {};
        for (int i = 0; i < 24; i++) std::snprintf(hex + i * 3, sizeof hex - i * 3, "%02x ", code[i]);
        static const uint64_t azb = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        uint64_t target = 0;
        if (code[0] == 0x48 && code[1] == 0xB8) std::memcpy(&target, code + 2, 8);               // mov rax, imm64 ; jmp rax
        else if (code[0] == 0xE9) target = fn + 5 + *reinterpret_cast<int32_t*>(code + 1);        // jmp rel32
        else if (code[0] == 0xFF && code[1] == 0x25) {                                            // jmp [rip+disp]
            uint64_t slot = fn + 6 + *reinterpret_cast<int32_t*>(code + 2);
            safe_read(reinterpret_cast<void*>(slot), &target, 8);
        }
        logf("  thunk at %llx: %s-> leads to %llx (azimuth+0x%llX, game+0x%llX)", (unsigned long long)fn, hex,
             (unsigned long long)target, (unsigned long long)(target - azb), (unsigned long long)(target - g_base));
    }
}

// MSVC std::wstring: 8-wchar inline buffer at +0 (or a pointer there when capacity >= 8), size at +0x10, capacity at +0x18.
std::string read_wstring(const void* p) {
    uint64_t size = 0, cap = 0;
    if (!p || !safe_read(static_cast<const uint8_t*>(p) + 0x10, &size, 8) || !safe_read(static_cast<const uint8_t*>(p) + 0x18, &cap, 8) ||
        size > 400)
        return "<?>";
    const wchar_t* data = reinterpret_cast<const wchar_t*>(p);
    if (cap >= 8) {
        uint64_t ptr = 0;
        if (!safe_read(p, &ptr, 8)) return "<?>";
        data = reinterpret_cast<const wchar_t*>(ptr);
    }
    wchar_t buf[401] = {};
    if (!safe_read(data, buf, size * sizeof(wchar_t))) return "<?>";
    char out[801] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)size, out, 800, nullptr, nullptr);
    return out;
}

// Experiment switch (server.ini [experiments] token_text=1): give the Xbox token callback a non-empty token and
// signature instead of two empty strings. The share's task chain waits on this callback, and an empty token may
// be what makes it give up. Hooked at `lea r8,[rbp+0x98]` (game+0x1C541E7), where the two std::wstring
// objects at rbp+0x98 (token) and rbp+0xB8 (signature) are already built (inline buffer, capacity 7).
void on_token_strings(FH2Context* ctx, void*) {
    if (!g_token_text) return;
    auto* rbp = reinterpret_cast<uint8_t*>(ctx->rbp);
    static const wchar_t kText[] = L"CTUNE";
    for (uint32_t off : {0x98u, 0xB8u}) {
        std::memcpy(rbp + off, kText, sizeof kText);                       // 5 characters + NUL, fits the inline buffer
        *reinterpret_cast<uint64_t*>(rbp + off + 0x10) = 5;                // size
    }
}

// ---- C++ exceptions: every `throw` ends in RaiseException(0xE06D7363, ...). Log the thrown type and a stack
// scan, so a task that fails by exception shows what was thrown and from where.
void (WINAPI* g_raise_orig)(DWORD, DWORD, DWORD, const ULONG_PTR*) = nullptr;
void WINAPI raise_detour(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
    static LONG n = 0;
    LONG tick = g_share_tick;
    bool in_window = tick && (LONG)GetTickCount() - tick < 3000;     // within 3 s of a share attempt starting
    if (code == 0xE06D7363 && nargs >= 4 && args && (in_window ? InterlockedIncrement(&n) <= 300 : InterlockedIncrement(&n) <= 8)) {
        // args: [0]=magic, [1]=object, [2]=ThrowInfo*, [3]=image base (x64)
        char type[120] = "?";
        uint32_t tinfo[4] = {};
        if (safe_read(reinterpret_cast<void*>(args[2]), tinfo, sizeof tinfo) && tinfo[3]) {
            uint32_t catchable_count = 0, first = 0;
            uint8_t* arr = reinterpret_cast<uint8_t*>(args[3] + tinfo[3]);
            if (safe_read(arr, &catchable_count, 4) && catchable_count && safe_read(arr + 4, &first, 4)) {
                uint32_t props[3] = {};      // CatchableType: properties, pType RVA, ...
                if (safe_read(reinterpret_cast<void*>(args[3] + first), props, sizeof props)) {
                    char name[110] = {};
                    if (safe_read(reinterpret_cast<void*>(args[3] + props[1] + 0x10), name, sizeof name - 1)) std::snprintf(type, sizeof type, "%s", name);
                }
            }
        }
        char line[900];
        // The thrown value is a handle (pointer to the exception object); show its first bytes: the HRESULT is in there.
        char obj[320] = "";
        uint8_t* handle = nullptr;
        if (safe_read(reinterpret_cast<void*>(args[1]), &handle, sizeof handle) && handle) {
            uint32_t w[28] = {};
            if (safe_read(handle, w, sizeof w)) {
                static const uint64_t azb = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
                uint64_t vt = (uint64_t)w[1] << 32 | w[0];
                int o = std::snprintf(obj, sizeof obj, "vtable=azimuth+0x%llX data:", (unsigned long long)(vt - azb));
                for (int i = 8; i < 28 && o < (int)sizeof obj - 12; i++) o += std::snprintf(obj + o, sizeof obj - o, " %08x", w[i]);
            }
        }
        int len = std::snprintf(line, sizeof line, "C++ exception #%ld%s thrown: type %s; object: %s; stack:", n, in_window ? " [during share]" : "", type, obj);
        uint64_t* sp = static_cast<uint64_t*>(__builtin_frame_address(0));
        int found = 0;
        static const uint64_t az_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        uint64_t az_size = 0;
        if (az_base) az_size = reinterpret_cast<const IMAGE_NT_HEADERS*>(az_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(az_base)->e_lfanew)->OptionalHeader.SizeOfImage;
        for (int i = 0; i < 400 && found < 16 && len < (int)sizeof line - 28; i++) {
            uint64_t v = 0;
            if (!safe_read(sp + i, &v, sizeof v)) break;
            bool in_game = v >= g_base + 0x1000 && v < g_base + 0x1DB4000;
            bool in_az = az_base && v >= az_base + 0x1000 && v < az_base + az_size;
            if (!in_game && !in_az) continue;
            uint8_t pre[6] = {};
            if (!safe_read(reinterpret_cast<void*>(v - 6), pre, sizeof pre)) continue;
            bool after_call = pre[1] == 0xE8 || (pre[4] == 0xFF && (pre[5] & 0x38) == 0x10) || (pre[3] == 0xFF && (pre[4] & 0x38) == 0x10) ||
                              (pre[2] == 0xFF && (pre[3] & 0x38) == 0x10);
            if (!after_call) continue;
            if (in_game) len += std::snprintf(line + len, sizeof line - len, " game+%llX", (unsigned long long)(v - g_base));
            else len += std::snprintf(line + len, sizeof line - len, " azimuth+%llX", (unsigned long long)(v - az_base));
            found++;
        }
        g_api->log(g_mod, line);
    }
    g_raise_orig(code, flags, nargs, args);
}

// ---- Azimuth's three "offline" error producers (all return ERROR_NETWORK_UNREACHABLE, 0x800704CF):
// OfflinePicture (azimuth+0x41D20), OfflineTokenRequest (+0xD3E20) and a larger one at +0x16AF10.
// Log when each is called, with a stack scan, to see which one the share operation runs into.
struct AzSite { const char* name; uint64_t (*orig)(uint64_t, uint64_t, uint64_t, uint64_t); };
AzSite g_az[] = {
    {"Azimuth.OfflinePicture", nullptr},
    {"Azimuth.OfflineTokenRequest", nullptr},
    {"Azimuth offline op", nullptr},
};

// Addresses inside azimuth.exe, per build (PE timestamp). token_call is the call that constructs the failed
// OfflineTokenRequest op (mov r8d,0x800704CF just before it). Patching these on any other build would corrupt
// unrelated code, so an unknown build gets no Azimuth hooks.
struct AzBuild { DWORD timestamp; uint32_t site[3]; uint32_t token_call; };
const AzBuild kAzBuilds[] = {
    {0x6AD14833, {0x41D20, 0xD3E20, 0x16AF10}, 0xD3EAD},      // 0.9.10
    {0x6ADFF280, {0x421D0, 0x2A6A50, 0x5DA0F0}, 0x2A6ADD},    // 1.0.1
    {0x6AE05DD1, {0x413D0, 0x2AA480, 0x5DE870}, 0x2AA50D},    // 1.0.2
    {0x6AE0F17A, {0x42110, 0x2B4FE0, 0x5EAC10}, 0x2B506D},    // 1.0.3
    {0x6AE393EB, {0x41680, 0x1A43F0, 0x4C5390}, 0x1A447D},    // 1.0.4
};
const AzBuild* az_build() {
    static const AzBuild* found = [] () -> const AzBuild* {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        for (const AzBuild& b : kAzBuilds)
            if (b.timestamp == nt->FileHeader.TimeDateStamp) return &b;
        logf("azimuth.exe build 0x%08lx is unknown: Azimuth token/diagnostic hooks skipped", (unsigned long)nt->FileHeader.TimeDateStamp);
        return nullptr;
    }();
    return found;
}
template <int K>
uint64_t az_detour(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    static LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    LONG tick = g_share_tick;
    bool in_window = tick && (LONG)GetTickCount() - tick < 3000;
    if (in_window || k <= 3) {
        char line[900];
        int len = std::snprintf(line, sizeof line, "%s called #%ld%s; stack:", g_az[K].name, k, in_window ? " [during share]" : "");
        uint64_t* sp = static_cast<uint64_t*>(__builtin_frame_address(0));
        static const uint64_t azb = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        uint64_t azs = reinterpret_cast<const IMAGE_NT_HEADERS*>(azb + reinterpret_cast<const IMAGE_DOS_HEADER*>(azb)->e_lfanew)->OptionalHeader.SizeOfImage;
        int found = 0;
        for (int i = 0; i < 400 && found < 16 && len < (int)sizeof line - 28; i++) {
            uint64_t v = 0;
            if (!safe_read(sp + i, &v, sizeof v)) break;
            bool in_game = v >= g_base + 0x1000 && v < g_base + 0x1DB4000;
            bool in_az = v >= azb + 0x1000 && v < azb + azs;
            if (!in_game && !in_az) continue;
            uint8_t pre[6] = {};
            if (!safe_read(reinterpret_cast<void*>(v - 6), pre, sizeof pre)) continue;
            bool after_call = pre[1] == 0xE8 || (pre[4] == 0xFF && (pre[5] & 0x38) == 0x10) || (pre[3] == 0xFF && (pre[4] & 0x38) == 0x10) ||
                              (pre[2] == 0xFF && (pre[3] & 0x38) == 0x10);
            if (!after_call) continue;
            len += in_game ? std::snprintf(line + len, sizeof line - len, " game+%llX", (unsigned long long)(v - g_base))
                           : std::snprintf(line + len, sizeof line - len, " azimuth+%llX", (unsigned long long)(v - azb));
            found++;
        }
        g_api->log(g_mod, line);
    }
    return g_az[K].orig(a, b, c, d);
}
template <int K> void install_az() {
    const AzBuild* b = az_build();
    if (!b) return;
    uintptr_t azb = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_api->hook_function(g_mod, g_az[K].name, reinterpret_cast<void*>(azb + b->site[K]), reinterpret_cast<void*>(&az_detour<K>),
                         reinterpret_cast<void**>(&g_az[K].orig));
}

// Log-only trace hooks on the login/transport path. The detours forward six
// integer-class arguments, which is enough for these (all take a few pointers/ints).
// Only the first few calls of each are logged, then every 1000th.
struct Trace {
    const char* name;
    uintptr_t   va;
    uint64_t  (*orig)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    volatile LONG count;
    int         state_off;    // if >= 0, log the int at rcx+state_off whenever it changes
    volatile LONG last_state;
    LONG        limit;        // how many calls to log in full (0 = 5)
};

template <int N>
struct TraceSlot {
    static Trace* t;
    static uint64_t detour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
        LONG n = InterlockedIncrement(&t->count);
        if (t->va == 0x140E44510) InterlockedExchange(&g_share_tick, (LONG)GetTickCount() | 1);
        if (t->va == 0x140E44510 && n <= 40) {      // share starter: what the popup holds (attempt marker)
            uint8_t* popup = reinterpret_cast<uint8_t*>(a);
            uint8_t* desc_owner = nullptr;
            std::string desc = "<?>";
            if (safe_read(popup + 0xE05C0, &desc_owner, 8) && desc_owner) desc = read_wstring(desc_owner + 0x10);
            logf("SHARE ATTEMPT #%ld: description=\"%s\" keyword A=\"%s\" keyword B=\"%s\"", n, desc.c_str(),
                 read_wstring(popup + 0xE0588).c_str(), read_wstring(popup + 0xE0568).c_str());
        }
        if (t->va == 0x140EF88B0 && n <= 40)       // StsAuthTokenManager::GetAuthToken(this, verb, url, ...)
            logf("token request: verb=%s url=%s", read_wstring(reinterpret_cast<void*>(b)).c_str(),
                 read_wstring(reinterpret_cast<void*>(c)).c_str());
        if (t->state_off >= 0 && a > 0x10000) {
            LONG st = *reinterpret_cast<const LONG*>(a + t->state_off);
            if (InterlockedExchange(&t->last_state, st) != st)
                logf("trace %s: state is now %ld (call #%ld)", t->name, st, n);
        }
        if (n <= (t->limit ? t->limit : 5) || n % 1000 == 0) {
            logf("trace %s #%ld (rcx=%llx rdx=%llx r8=%llx r9=%llx)", t->name, n,
                 (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
            if (n <= 3 && !t->limit) {
                const uint64_t args[4] = {a, b, c, d};
                static const char* const regs[4] = {"rcx", "rdx", "r8", "r9"};
                for (int i = 0; i < 4; i++) dump_arg(t->name, regs[i], args[i]);
            }
        }
        uint64_t ret = t->orig(a, b, c, d, e, f);
        if (t->limit == 20 && n <= 3) {        // the share-path traces: also log a heuristic call stack
            // The game's code has no unwind data, so scan the stack for words that look like return
            // addresses: inside the game's code, and directly after a CALL instruction.
            char line[700];
            int len = std::snprintf(line, sizeof line, "  stack for %s #%ld:", t->name, n);
            uint64_t* sp = static_cast<uint64_t*>(__builtin_frame_address(0));
            int found = 0;
            for (int i = 0; i < 400 && found < 18 && len < (int)sizeof line - 24; i++) {
                uint64_t v = 0;
                if (!safe_read(sp + i, &v, sizeof v)) break;
                if (v < g_base + 0x1000 || v >= g_base + 0x1DB4000) continue;
                uint8_t pre[6] = {};
                if (!safe_read(reinterpret_cast<void*>(v - 6), pre, sizeof pre)) continue;
                bool after_call = pre[1] == 0xE8 || (pre[4] == 0xFF && (pre[5] & 0x38) == 0x10) || (pre[3] == 0xFF && (pre[4] & 0x38) == 0x10) ||
                                  (pre[2] == 0xFF && (pre[3] & 0x38) == 0x10) || (pre[4] == 0x41 && pre[5] >= 0xD0 && pre[5] <= 0xD7);
                if (!after_call) continue;
                len += std::snprintf(line + len, sizeof line - len, " game+%llX", (unsigned long long)(v - g_base));
                found++;
            }
            g_api->log(g_mod, line);
        }
        if (n <= (t->limit ? t->limit : 5))
            logf("trace %s #%ld returned %llx (hr=%08x), called from game+0x%llX", t->name, n, (unsigned long long)ret,
                 (unsigned)ret, (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_base));
        return ret;
    }
};
template <int N> Trace* TraceSlot<N>::t = nullptr;

Trace g_traces[] = {
    {"TSTransportClient::OnLogonChanged",    0x140EEB490, nullptr, 0, -1, -1},
    {"TSConnection::EnqueSends",             0x140EF3BD0, nullptr, 0, -1, -1},
    {"TSConnection::Send",                   0x140EF5500, nullptr, 0, -1, -1},
    {"TSConnection::Pump",                   0x140EF4D90, nullptr, 0, -1, -1},
    {"LspAuthenticator (state query)",       0x140EFA6B0, nullptr, 0, -1, -1},
    {"LspAuthenticator::OnUserLogout",       0x140EFA9D0, nullptr, 0, -1, -1},
    {"StsAuthTokenManager::GetAuthToken",    0x140EF88B0, nullptr, 0, -1, -1},
    {"GetAuthToken caller",                  0x140EEA6D0, nullptr, 0, -1, -1},
    {"LspAuthenticator::OnUserLogin",        0x140EFA780, nullptr, 0, -1, -1},
    {"LspAuthenticator::Update",             0x140EFA900, nullptr, 0, 0x70, -1},
    {"HttpProtocol2::OnRequestComplete",     0x141B7DD00, nullptr, 0, -1, -1, 400},
    {"HttpCallback::OnHeadersAvailable",     0x141B7F790, nullptr, 0, -1, -1},
    {"HttpCallback::OnDataAvailable",        0x141B7F700, nullptr, 0, -1, -1},
    {"HttpCallback::OnResponseReceived",     0x141B7F810, nullptr, 0, -1, -1},
    {"HttpProtocol2 feed response",          0x141B7F830, nullptr, 0, -1, -1},
    {"Share: completion callback (bool*)",   0x140E45980, nullptr, 0, -1, -1, 20},
    {"Share: reset",                         0x140E459D0, nullptr, 0, -1, -1, 20},
    {"UploadStarter (Livery) vfunc",         0x140A4B780, nullptr, 0, -1, -1, 20},
    {"CStorefrontUploadManager vfunc A",     0x140A439F0, nullptr, 0, -1, -1, 20},
    {"CStorefrontUploadManager vfunc B",     0x140A438E0, nullptr, 0, -1, -1, 20},
    {"CStorefrontUploadManager vfunc C",     0x140A43500, nullptr, 0, -1, -1, 20},
    {"CStorefrontUploadManager vfunc D",     0x140A43400, nullptr, 0, -1, -1, 20},
    {"Share starter (builds keywords)",      0x140E44510, nullptr, 0, -1, -1, 20},
    {"Share status handler",                 0x140E39EB0, nullptr, 0, -1, -1, 20},
    {"Share op 2CE80",                       0x140E2CE80, nullptr, 0, -1, -1, 20},
    {"Share op 2D0A0",                       0x140E2D0A0, nullptr, 0, -1, -1, 20},
    {"Share op 2D530",                       0x140E2D530, nullptr, 0, -1, -1, 20},
    {"Share op 2D8B0",                       0x140E2D8B0, nullptr, 0, -1, -1, 20},
    {"Share op 2D980",                       0x140E2D980, nullptr, 0, -1, -1, 20},
    {"Share op 2DBD0",                       0x140E2DBD0, nullptr, 0, -1, -1, 20},
    {"Share op 2DD90",                       0x140E2DD90, nullptr, 0, -1, -1, 20},
    {"Share op 37E40",                       0x140E37E40, nullptr, 0, -1, -1, 20},
    {"Share op 37FB0",                       0x140E37FB0, nullptr, 0, -1, -1, 20},
    {"Share op 3AC60",                       0x140E3AC60, nullptr, 0, -1, -1, 20},
    {"Share op 42A30",                       0x140E42A30, nullptr, 0, -1, -1, 20},
    {"Share op 44C10",                       0x140E44C10, nullptr, 0, -1, -1, 20},
    {"ThrowHRESULT (game's throw helper)",   0x14022B8F0, nullptr, 0, -1, -1, 20},
};

template <int N>
void install_trace(uintptr_t base) {
    TraceSlot<N>::t = &g_traces[N];
    g_api->hook_function(g_mod, g_traces[N].name,
        reinterpret_cast<void*>(base + (g_traces[N].va - kImageBase)),
        reinterpret_cast<void*>(&TraceSlot<N>::detour), reinterpret_cast<void**>(&g_traces[N].orig));
}

template <size_t... I>
void install_traces(uintptr_t base, std::index_sequence<I...>) {
    (install_trace<static_cast<int>(I)>(base), ...);
}

// ---- Reflection dump: the request/reply layouts live in runtime-built type objects. Walk the pointers from each
// method's two descriptor objects and write the raw memory to communitygarage/typedump.txt for offline decoding.
// The Scoreboard service's descriptors come first (game+0x354D310..0x354D4A0: DownloadFile, GetBestScore, GetLadderRows,
// GetRival, GetRows, GetRowsAnthem, GetTimeTrials, NotifyRival, SubmitScore, SubmitXpScore, UploadFile), then the
// Storefront ones decoded earlier.
size_t dump_types(uintptr_t base) {
    std::vector<uint64_t> roots;
    for (uint64_t a = 0x14354d300; a <= 0x14354d4b0; a += 0x10) roots.push_back(a);
    for (uint64_t a = 0x14354cc80; a <= 0x14354d000; a += 0x10) roots.push_back(a);
    for (uint64_t a = 0x14354e100; a <= 0x14354e200; a += 0x10) roots.push_back(a);
    std::wstring path = std::wstring(g_api->azimuth_dir) + L"/communitygarage/typedump.txt";
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return 0;
    std::vector<uint64_t> queue;
    std::unordered_set<uint64_t> seen;
    for (uint64_t r : roots) queue.push_back(base + (r - kImageBase));
    size_t nodes = 0;
    for (size_t qi = 0; qi < queue.size() && nodes < 40000; qi++) {
        uint64_t a = queue[qi];
        if (!seen.insert(a).second) continue;
        uint8_t buf[0x100];
        size_t got = 0;
        for (size_t n : {sizeof buf, (size_t)0x80, (size_t)0x40, (size_t)0x10, (size_t)8})
            if (safe_read(reinterpret_cast<void*>(a), buf, n)) { got = n; break; }
        if (!got) continue;
        nodes++;
        std::fprintf(f, "%llx:", (unsigned long long)(a - base + kImageBase));
        for (size_t i = 0; i < got; i++) std::fprintf(f, "%02x", buf[i]);
        std::fputc('\n', f);
        for (size_t i = 0; i + 8 <= got; i += 8) {
            uint64_t v;
            std::memcpy(&v, buf + i, 8);
            uint8_t probe;
            if (v > 0x10000 && v < 0x7fffffffffffull && !seen.count(v) && safe_read(reinterpret_cast<void*>(v), &probe, 1)) queue.push_back(v);
        }
    }
    std::fclose(f);
    logf("type dump written: %zu nodes", nodes);
    return nodes;
}

// "features" from the command pipe: the online-services manager's feature table, as the Logon reply filled it
// (manager at game+0x367A6D0; table at +0x80: entries of {methodId, tier, isEnabled, access} from +0xA8, count at
// +0x10A8, valid flag +0x10B1; the manager's state at +0x74, 2 = connected). A feature's status is its `access`; one
// that isn't listed counts as unavailable (4).
void on_features_command(void*, const char*, const char*, char* reply, size_t reply_size) {
    if (!g_base) { std::snprintf(reply, reply_size, "error: the game isn't running yet"); return; }
    uint8_t* mgr = nullptr;
    if (!safe_read(reinterpret_cast<void*>(g_base + 0x367A6D0), &mgr, sizeof mgr) || !mgr) { std::snprintf(reply, reply_size, "error: no manager yet"); return; }
    uint32_t state = 0, count = 0;
    uint8_t valid = 0, valid2 = 0;
    safe_read(mgr + 0x74, &state, 4);
    safe_read(mgr + 0x80 + 0x10A8, &count, 4);
    safe_read(mgr + 0x80 + 0x10B1, &valid, 1);
    safe_read(mgr + 0x80 + 0x10B2, &valid2, 1);
    // The two halves of the Rivals access check (States::IsRivalsAccessible, game+0x8A4200), and what they read.
    uint8_t* online = nullptr;   // game+0x34D6AF0
    uint8_t* rivals = nullptr;   // game+0x34D6348 (RivalsModule)
    uint8_t online11 = 0xFF, rivalsF0 = 0xFF;
    safe_read(reinterpret_cast<void*>(g_base + 0x34D6AF0), &online, sizeof online);
    safe_read(reinterpret_cast<void*>(g_base + 0x34D6348), &rivals, sizeof rivals);
    if (online) safe_read(online + 0x11, &online11, 1);
    if (rivals) safe_read(rivals + 0xF0, &rivalsF0, 1);
    int a = -1, b = -1, user = -1;
    if (online) {
        a    = reinterpret_cast<bool (*)(void*, bool)>(g_base + 0xA41C90)(online, false);
        user = reinterpret_cast<bool (*)(void*)>(g_base + 0xA42090)(online);
    }
    if (rivals) b = reinterpret_cast<bool (*)()>(g_base + 0x666F40)();
    int len = std::snprintf(reply, reply_size, "rivals access: A41C90=%d 666F40=%d (online+user A42090=%d, online+0x11=%u, rivals+0xF0=%u); "
                            "manager state %u, table valid %u/%u, %u entries:", a, b, user, online11, rivalsF0, state, valid, valid2, count);
    for (uint32_t i = 0; i < count && i < 256 && len < (int)reply_size - 40; i++) {
        uint32_t e[4] = {};
        if (!safe_read(mgr + 0x80 + 0xA8 + i * 16, e, sizeof e)) break;
        len += std::snprintf(reply + len, reply_size - len, " %#x(tier %u, enabled %u, access %u)", e[0], e[1], e[2] & 0xFF, e[3]);
    }
}

// "typedump" from fh2mod.exe or the command pipe, while the game runs (the type objects are built at runtime).
void on_typedump_command(void*, const char*, const char*, char* reply, size_t reply_size) {
    if (!g_base) { std::snprintf(reply, reply_size, "error: the game isn't running yet"); return; }
    size_t n = dump_types(g_base);
    std::snprintf(reply, reply_size, n ? "wrote communitygarage\\typedump.txt (%zu nodes)" : "error: couldn't write communitygarage\\typedump.txt", n);
}

// ---- The game's economy values. They live in one object, a property every 0x20 bytes from +0x208 (u32 each). Hook the
// property getters, and the first time the game reads one, log the whole set. Run with [experiments] economy_probe=1 to see
// the game's own built-in numbers (the mod then fails the server request instead of letting server values replace them).
struct EconSite { const char* name; uintptr_t va; uint32_t off; uint32_t (*orig)(void*, uint8_t*); };
EconSite g_econ[] = {
    {"CreditDestinationBonus", 0x14060EDF0, 0x208, nullptr}, {"CreditDestinationFalloff", 0x140543700, 0x228, nullptr},
    {"XPPerKmOffline", 0x140542AA0, 0x248, nullptr},         {"XPPerKmOnline", 0x1405441A0, 0x268, nullptr},
    {"CreditsPerKm", 0x1403EE230, 0x288, nullptr},           {"MinimumFinishlineXP", 0x14060EB40, 0x2A8, nullptr},
    {"UseServerValues", 0x140543A70, 0x1E8, nullptr},
};
volatile LONG g_econ_logged = 0;
uint64_t g_econ_last = 0;
void log_economy(uint8_t* obj, const char* why) {
    uint32_t v[7] = {};
    for (int i = 0; i < 7; i++) {
        uint32_t x = 0;
        safe_read(obj + g_econ[i].off, &x, i == 6 ? 1 : 4);
        v[i] = x;
    }
    float tombola = 0, promo = 0;
    safe_read(obj + 0x308, &tombola, 4);
    safe_read(obj + 0x2E8, &promo, 4);
    uint64_t sig = 1469598103934665603ull;
    for (uint32_t x : v) sig = (sig ^ x) * 1099511628211ull;
    if (sig == g_econ_last || InterlockedIncrement(&g_econ_logged) > 12) return;
    g_econ_last = sig;
    logf("ECONOMY (%s) object %p: CreditDestinationBonus=%u CreditDestinationFalloff=%u XPPerKmOffline=%u XPPerKmOnline=%u "
         "CreditsPerKm=%u MinimumFinishlineXP=%u UseServerValues=%u TombolaPrizeScale=%g HorizonPromoBonusScale=%g",
         why, obj, v[0], v[1], v[2], v[3], v[4], v[5], v[6], tombola, promo);
}
template <int K>
uint32_t econ_detour(void* a, uint8_t* obj) {
    uint32_t r = g_econ[K].orig(a, obj);
    if (obj) log_economy(obj, g_econ[K].name);
    return r;
}
template <int K> void install_econ(uintptr_t base) {
    g_api->hook_function(g_mod, g_econ[K].name, reinterpret_cast<void*>(base + (g_econ[K].va - kImageBase)),
                         reinterpret_cast<void*>(&econ_detour<K>), reinterpret_cast<void**>(&g_econ[K].orig));
}

// ---- Find the economy object in memory. Its values are properties 0x20 bytes apart: CreditDestinationBonus at +0x208, then
// Falloff, XPPerKmOffline, XPPerKmOnline, CreditsPerKm, MinimumFinishlineXP; UseServerValues at +0x1E8, HorizonPromoBonusScale
// (float) at +0x2E8 and TombolaPrizeScale (float) at +0x308. Scan the process for that shape and log what matches.
void scan_economy() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<uint8_t> buf(1 << 20);
    int found = 0;
    uint64_t scanned = 0;
    for (uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress); p < static_cast<uint8_t*>(si.lpMaximumApplicationAddress);) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        uint8_t* end = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
        if (ok) {
            for (uint8_t* q = static_cast<uint8_t*>(mbi.BaseAddress); q < end; q += buf.size() - 0x400) {
                size_t n = std::min<size_t>(buf.size(), end - q);
                if (n < 0x320 || !safe_read(q, buf.data(), n)) continue;
                scanned += n;
                for (size_t o = 0x20; o + 0x320 <= n; o += 8) {
                    const uint8_t* b = buf.data() + o;      // candidate: b = start of the CreditDestinationBonus property value
                    uint32_t v[6];
                    bool good = true;
                    int vtables = 0, nonzero = 0;
                    for (int i = 0; i < 6 && good; i++) {
                        std::memcpy(&v[i], b + i * 0x20, 4);
                        good = v[i] <= 50000000;
                        nonzero += v[i] != 0;
                        uint64_t head;                      // each property object starts 8 bytes before its value, with a vtable pointer
                        std::memcpy(&head, b + i * 0x20 - 8, 8);
                        vtables += head >= g_base && head < g_base + 0x1DB4000;
                    }
                    if (!good || vtables < 5 || nonzero < 3) continue;
                    float promo, tombola;
                    uint8_t useServer = b[-0x20];
                    std::memcpy(&promo, b + 0xE0, 4);
                    std::memcpy(&tombola, b + 0x100, 4);
                    if (useServer > 1 || !(promo >= 0.f && promo <= 1000.f) || !(tombola >= 0.f && tombola <= 1000.f)) continue;
                    if (++found <= 30)
                        logf("ECONOMY scan: object %p: CreditDestinationBonus=%u CreditDestinationFalloff=%u XPPerKmOffline=%u XPPerKmOnline=%u "
                             "CreditsPerKm=%u MinimumFinishlineXP=%u UseServerValues=%u HorizonPromoBonusScale=%g TombolaPrizeScale=%g",
                             q + o - 0x208, v[0], v[1], v[2], v[3], v[4], v[5], useServer, promo, tombola);
                }
            }
        }
        if (end <= p) break;
        p = end;
    }
    logf("ECONOMY scan finished: %d candidate(s), %llu MB looked at", found, (unsigned long long)(scanned >> 20));
}

DWORD WINAPI econ_scan_thread(LPVOID) {
    for (int delay : {60, 120, 240}) {          // after login; again later in case the values arrive or change
        Sleep(delay * 1000);
        scan_economy();
    }
    return 0;
}

DWORD WINAPI worker(LPVOID) {
    // Azimuth fails every game HTTP request as "offline"; put real HTTP behind the
    // requests to our host before the game makes its first one.
    xhr_bridge_install(g_api, g_mod, g_bridge);
    load_drivatar_names();
    // wait_for_code, not wait_for_game: Extra Options has to patch first.
    void* code = g_api->wait_for_code(0xFFFFFFFFu);
    if (!code) return 0;
    auto base = reinterpret_cast<uintptr_t>(code);
    g_base    = base;
    install_ai_name_hooks(base);
    if (HANDLE t = CreateThread(nullptr, 0, rivals_unlock_thread, reinterpret_cast<void*>(base), 0, nullptr)) CloseHandle(t);
    for (const Entry& e : kEntries) {
        std::string what = std::string(e.name) + " host";
        bool ok = redirect(base, e.host_slot, what.c_str(), kHostExpect, g_host);
        what    = std::string(e.name) + " port";
        ok     &= redirect(base, e.port_slot, what.c_str(), kPortExpect, g_port);
        logf("%s -> %s:%s %s", e.name, g_host, g_port, ok ? "redirected" : "FAILED");
    }
    {
        // The Clubs tab: the menu's UI state sets map["IsClubsEnabled"] = 1 with `mov byte ptr [rax], 1` (game+0x6F0897, bytes
        // C6 00 01). The community server has no club service, so the tab is switched off by making that value 0.
        const uint8_t was = 1, now = 0;
        FH2Hook* h = g_api->patch_bytes(g_mod, "Clubs tab off", reinterpret_cast<void*>(base + (0x1406F0899 - kImageBase)), &was, &now, 1);
        if (!h || !g_api->enable_hooks(g_mod, h)) logf("Clubs tab: the game code didn't look as expected, leaving it alone");
    }
    install_pack_skip(base);
    notify_install(g_api, g_mod, base);
    // What changes the game's behaviour: the date default, the Xbox token callback and strings, the login-listener skip, the poll
    // intervals, and Azimuth's token request. Everything else (below, under g_debug) only logs.
    if (g_api->api_version >= 7) {
        g_api->hook_instruction(g_mod, "empty DateTime default", reinterpret_cast<void*>(base + (0x140EE5A57 - kImageBase)),
                                on_empty_datetime, nullptr);
        g_api->hook_instruction(g_mod, "token strings", reinterpret_cast<void*>(base + (0x141C541E7 - kImageBase)),
                                on_token_strings, nullptr);
        g_api->hook_instruction(g_mod, "token callback result", reinterpret_cast<void*>(base + (0x141C541A5 - kImageBase)),
                                on_token_callback, nullptr);
        g_api->hook_instruction(g_mod, "car builder: foreign tune parts", reinterpret_cast<void*>(base + (0x141056263 - kImageBase)),
                                on_car_build, nullptr);
    }
    g_api->hook_function(g_mod, "logon-response listener (skipped)", reinterpret_cast<void*>(base + (0x1409C88E0 - kImageBase)),
                         reinterpret_cast<void*>(&listener_skip), reinterpret_cast<void**>(&g_listener_orig));
    g_api->hook_function(g_mod, "logon response broadcast", reinterpret_cast<void*>(base + (0x14126C5E0 - kImageBase)),
                         reinterpret_cast<void*>(&broadcast_detour), reinterpret_cast<void**>(&g_broadcast_orig));
    if (g_api->api_version >= 7 && g_fake_token) {
        init_fake_token();
        // The call to the operation constructor, right after "mov r8d, 0x800704CF" (14 bytes before it).
        auto* az = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        const AzBuild* ab = az_build();
        static const uint8_t mov_r8d[] = {0x41, 0xB8, 0xCF, 0x04, 0x07, 0x80};
        uint8_t got[sizeof mov_r8d] = {}, call = 0;
        if (ab && safe_read(az + ab->token_call - 14, got, sizeof got) && safe_read(az + ab->token_call, &call, 1) &&
            std::memcmp(got, mov_r8d, sizeof got) == 0 && call == 0xE8)
            g_api->hook_instruction(g_mod, "Azimuth token request -> success", az + ab->token_call, on_az_token_op, nullptr);
        else if (ab)
            logf("Azimuth token request: the call isn't where expected; not hooked");
    }
    // Switch these on now: with [debug] trace=1 installing the hooks below takes seconds, and the logon reply can
    // reach the listener before then and crash the game (null write at game+0x9C898F).
    g_api->enable_hooks(g_mod, nullptr);
    if (g_econ_probe || g_econ_scan) CreateThread(nullptr, 0, econ_scan_thread, nullptr, 0, nullptr);

    if (g_debug) {
        logf("debug tracing is on ([debug] trace=1): extra hooks and verbose logging");
        g_api->hook_function(g_mod, "HTTP provider Configure", reinterpret_cast<void*>(base + (0x141B7C0E0 - kImageBase)),
                             reinterpret_cast<void*>(&configure_detour), reinterpret_cast<void**>(&g_configure_orig));
        g_api->hook_function(g_mod, "Feature-locked popup", reinterpret_cast<void*>(base + (0x140DDD630 - kImageBase)),
                             reinterpret_cast<void*>(&popup_detour), reinterpret_cast<void**>(&g_popup_orig));
        if (g_api->api_version >= 7) {
            struct Site { uintptr_t va; const char* what; };
            static const Site sites[] = {
                {0x140EF8BE3, "no primary Live user"},
                {0x141C54654, "exception in User::GetTokenAndSignatureAsync()"},
                {0x141C5415A, "exception in GetTokenAndSignatureResult::get()"},
            };
            static const struct { uintptr_t va; const char* what; } chain[] = {
                {0x14032E57F, "1 ([singleton]->vtbl+0x48)"}, {0x14032E58C, "2 (->vtbl+0xa8)"}, {0x14032E598, "3 (->vtbl+0x128, null = fail)"},
            };
            for (const auto& c : chain)
                g_api->hook_instruction(g_mod, "share chain", reinterpret_cast<void*>(base + (c.va - kImageBase)), on_share_chain,
                                        const_cast<char*>(c.what));
            g_api->hook_instruction(g_mod, "share call A", reinterpret_cast<void*>(base + (0x14032E86C - kImageBase)), on_share_call,
                                    const_cast<char*>("A [r15+0xb0]"));
            g_api->hook_instruction(g_mod, "share call B", reinterpret_cast<void*>(base + (0x14032E8B5 - kImageBase)), on_share_call,
                                    const_cast<char*>("B [rsi+0x40]"));
            for (uintptr_t va : kRaiseSites)
                g_api->hook_instruction(g_mod, "raise 0x8050005C", reinterpret_cast<void*>(base + (va - kImageBase)),
                                        on_raise_5c, reinterpret_cast<void*>(va));
            for (const Site& st : sites)
                g_api->hook_instruction(g_mod, st.what, reinterpret_cast<void*>(base + (st.va - kImageBase)),
                                        on_token_failure, const_cast<char*>(st.what));
        }
        g_api->hook_function(g_mod, "reply array reader", reinterpret_cast<void*>(base + (0x140EE5450 - kImageBase)),
                             reinterpret_cast<void*>(&array_detour), reinterpret_cast<void**>(&g_array_orig));
        g_api->hook_function(g_mod, "reply DateTime reader", reinterpret_cast<void*>(base + (0x140EE5A30 - kImageBase)),
                             reinterpret_cast<void*>(&date_detour), reinterpret_cast<void**>(&g_date_orig));
        if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) {
            if (void* raise = reinterpret_cast<void*>(GetProcAddress(kb, "RaiseException")))
                g_api->hook_function(g_mod, "RaiseException (C++ throws)", raise, reinterpret_cast<void*>(&raise_detour),
                                     reinterpret_cast<void**>(&g_raise_orig));
        }
        install_az<0>(); install_az<1>(); install_az<2>();
        install_econ<0>(base); install_econ<1>(base); install_econ<2>(base); install_econ<3>(base); install_econ<4>(base); install_econ<5>(base); install_econ<6>(base);
        install_traces(base, std::make_index_sequence<sizeof(g_traces) / sizeof(g_traces[0])>{});
    }
    g_api->enable_hooks(g_mod, nullptr);
    if (g_api->api_version >= 9)   // writes communitygarage/typedump.txt on request (see docs/re-notes.md)
        g_api->add_command(g_mod, "typedump", "typedump: write the game's request/reply layouts to communitygarage\\typedump.txt",
                           on_typedump_command, nullptr);
    if (g_api->api_version >= 9)
        g_api->add_command(g_mod, "features", "features: list the online features the Logon reply enabled", on_features_command, nullptr);
    return 0;
}

} // namespace

// Whether the platform may look for a newer release of this mod on GitHub (about every six hours; it then shows
// "CommunityGarage x.y.z is available" on the title screen). Players can turn it off with [updates] check=0 in server.ini.
static bool updates_enabled(const wchar_t* dir) {
    const std::wstring ini = std::wstring(dir) + L"/communitygarage/server.ini";
    return GetPrivateProfileIntW(L"updates", L"check", 1, ini.c_str()) != 0;
}

// ---- The MODS page (Pause > Settings > MODS): the switches live in server.ini, so a change is written back there.
static std::wstring g_ini_path;
static void on_setting_changed(void*, const char* key, int value) {
    const wchar_t* section = nullptr;
    const wchar_t* name = nullptr;
    if (!std::strcmp(key, "CheckUpdates")) { section = L"updates"; name = L"check"; }
    else if (!std::strcmp(key, "DebugLog")) { section = L"debug"; name = L"trace"; }
    else if (!std::strcmp(key, "ServerEconomy")) { section = L"economy"; name = L"use_server_values"; }
    else if (!std::strcmp(key, "ServerDrivatarNames")) {
        WritePrivateProfileStringW(L"drivatars", L"names", value ? L"server" : L"off", g_ini_path.c_str());
        logf("setting %s is now %s (applies the next time the game starts)", key, value ? "server" : "off");
        return;
    }
    else return;
    WritePrivateProfileStringW(section, name, value ? L"1" : L"0", g_ini_path.c_str());
    logf("setting %s is now %s (applies the next time the game starts)", key, value ? "on" : "off");
}

static void add_mods_page_rows(const FH2ModApi* api) {
    if (api->api_version >= 4) {
        FH2Setting s{};
        s.size          = sizeof s;
        s.key           = "CheckUpdates";
        s.label         = "Check for updates";
        s.description   = "Let the mod platform look on GitHub, about every six hours, for a newer Community Garage and tell you on the "
                          "title screen. Nothing is downloaded or installed for you. Nothing else is sent to GitHub.";
        s.value         = GetPrivateProfileIntW(L"updates", L"check", 1, g_ini_path.c_str()) != 0;
        s.needs_restart = 1;
        s.on_change     = on_setting_changed;
        api->add_setting(g_mod, &s);

        FH2Setting e{};
        e.size          = sizeof e;
        e.key           = "ServerEconomy";
        e.label         = "Use server economy values";
        e.description   = "Take race payouts (credits and XP) from the Community Garage server. Turn it off to keep the game's own "
                          "built-in values; the mod then doesn't ask the server for them.";
        e.value         = GetPrivateProfileIntW(L"economy", L"use_server_values", 1, g_ini_path.c_str()) != 0;
        e.needs_restart = 1;
        e.on_change     = on_setting_changed;
        api->add_setting(g_mod, &e);

        FH2Setting n{};
        n.size          = sizeof n;
        n.key           = "ServerDrivatarNames";
        n.label         = "Drivatar names from the server";
        n.description   = "Give the AI drivers the names of Community Garage players, a new random pick each time you start the game. "
                          "Turn it off to keep the game's own AI driver names.";
        n.value         = drivatar_names_from_server(g_ini_path);
        n.needs_restart = 1;
        n.on_change     = on_setting_changed;
        api->add_setting(g_mod, &n);

        FH2Setting d{};
        d.size          = sizeof d;
        d.key           = "DebugLog";
        d.label         = "Detailed log (troubleshooting)";
        d.description   = "Writes much more about what the mod does to fh2-mods\\platform.log. Turn it on only if asked to when "
                          "reporting a problem; it adds extra hooks to the game and makes the log large.";
        d.value         = GetPrivateProfileIntW(L"debug", L"trace", 0, g_ini_path.c_str()) != 0;
        d.needs_restart = 1;
        d.on_change     = on_setting_changed;
        api->add_setting(g_mod, &d);
    }
    if (api->api_version >= 6) {
        FH2Setting l{};
        l.size        = sizeof l;
        l.key         = "Website";
        l.label       = "Community Garage on GitHub";
        l.description = "Releases, what's new, and the player guide.";
        l.kind        = FH2_SETTING_LINK;
        l.url         = "https://github.com/qiangqiang101/CommunityGarage";
        api->add_setting(g_mod, &l);
    }
}

// The version azimuth.exe says it is ("1.0.5"), from its version resource; empty if it has none.
static std::string azimuth_version() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n == MAX_PATH) return {};
    DWORD size = GetFileVersionInfoSizeW(path, nullptr);
    if (!size) return {};
    std::vector<uint8_t> info(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path, 0, size, info.data()) || !VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) ||
        !fixed || len < sizeof *fixed)
        return {};
    char v[32];
    std::snprintf(v, sizeof v, "%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS));
    return v;
}

// An Azimuth release the HTTP bridge wasn't written for: no request would reach the server, and the mod's sign-in hooks
// assume they do (they can leave the game stuck signing in). So the mod stays off, the game plays as Azimuth does
// offline, and the title screen says why. The notice is kept in a file by the platform, so a known build clears it.
static bool azimuth_supported(const FH2ModApi* api) {
    if (const char* known = xhr_bridge_known_build()) {
        api->set_notice(g_mod, "");
        logf("Azimuth %s", known);
        return true;
    }
    std::string version = azimuth_version();
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    logf("Azimuth %s (azimuth.exe build 0x%08lx) isn't supported by this version; the mod is off, so the game plays offline",
         version.empty() ? "?" : version.c_str(), (unsigned long)nt->FileHeader.TimeDateStamp);
    std::string notice = "Community Garage doesn't support\nAzimuth " + (version.empty() ? std::string("this version") : version) +
                         " yet, so sharing is off.\nLook for a Community Garage update.";
    api->set_notice(g_mod, notice.c_str());
    return false;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "CommunityGarage";
    info.version          = COMMUNITYGARAGE_VERSION;
    info.github_repo      = "qiangqiang101/CommunityGarage";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates    = updates_enabled(api->azimuth_dir) ? 1 : 0;
    info.description      = "Share and download Forza Horizon 2 liveries, tunes and group vinyls from the game's own menus, through the "
                            "Community Garage server. Needs an internet connection. Your account key, name and what you share go to the "
                            "community server only.";   // [updates] check= in server.ini, on by default
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    if (api->api_version < 7) {
        api->log(g_mod, "needs platform API 7 (byte patches); reinstall with a newer DWrite.dll");
        return 1;
    }
    g_ini_path = std::wstring(api->azimuth_dir) + L"/communitygarage/server.ini";
    add_mods_page_rows(api);
    if (!azimuth_supported(api)) return 0;
    if (!load_config(api->azimuth_dir)) return 0;
    if (HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
