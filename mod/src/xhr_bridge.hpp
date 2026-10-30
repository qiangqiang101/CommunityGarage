// Real HTTP for the game's IXMLHTTPRequest2 requests.
//
// Azimuth answers every game request with an "offline" stub (class
// azimuth::services::OfflineRequest, which fails with "network unreachable").
// This swaps that stub's vtable slots for ones that carry out requests to one
// chosen host with WinHTTP and report the result through the game's own
// IXMLHTTPRequest2Callback. Requests to any other host still get Azimuth's
// offline behaviour, so the game never talks to Xbox Live or anything else.
#pragma once

#include "fh2mod.h"

#include <string>

struct BridgeConfig {
    std::wstring host;              // requests to this host are carried out; all others stay offline
    int          scheme = 0;        // 0 = auto (https on port 443, else http), 1 = http, 2 = https
    bool         insecure_tls = false; // accept self-signed/mismatched certificates (local testing only)
    bool         debug = false;         // log every request and response (off for normal play)
    bool         fail_economy = false;  // answer GetEconomySettings with an error, so the game keeps its built-in values
    std::wstring path_prefix;       // prepended to every request path, e.g. /fh2 when the server app lives under a sub-URL
    std::string  key;               // this install's account key, sent as X-CT-Key to the community server
    std::string  name;              // display name (UTF-8), sent URL-encoded as X-CT-Name
};

// The Azimuth release this azimuth.exe is ("1.0.4"), or NULL when the bridge wasn't written for it. Without the
// bridge no request reaches the server, so the mod stays off on such a build.
const char* xhr_bridge_known_build();

// Returns true when the stub's vtable was recognised and all slots are hooked.
bool xhr_bridge_install(const FH2ModApi* api, FH2Mod* mod, const BridgeConfig& cfg);
