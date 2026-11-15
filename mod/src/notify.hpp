// Messages for the player, in the game's own UI. Why a request failed goes into the Storefront's error popup in
// place of its generic message. A failure no popup showed goes to a pop-up on the driving HUD, through the mod
// platform's show_toast (API 10): the platform shares the game's notification with the game and every other mod,
// and the player can turn it off (the game's Notifications setting, and the Pop-ups row on the mod's page).
#pragma once

#include "fh2mod.h"

#include <cstdint>

// Hooks the Storefront's error text. Call once the game's code is ready, before enable_hooks.
bool notify_install(const FH2ModApi* api, FH2Mod* mod, uintptr_t base);

// A request to the community server failed (any thread). For the next few seconds the Storefront's error popup shows
// `text` in place of the game's generic message, when the popup's code is `hr` (0x8051xxxx for the server's error reply
// with code xxxx, the HRESULT a failed connection was reported with) or `hr` is 0 (not known). If no popup asks for
// it, `line` (or `text`) shows as a HUD pop-up, unless `toast` is false (a reason for an error that may not happen).
// The pop-up's line holds 45 characters, so `line` is a short form of `text`; the same line within half a minute is
// shown once, so a failing background request doesn't repeat itself.
void notify_request_failed(uint32_t hr, const char* text, bool toast = true, const char* line = nullptr);
