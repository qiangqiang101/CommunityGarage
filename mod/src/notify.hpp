// Messages for the player, in the game's own UI. Why a request failed goes into the Storefront's error popup in
// place of its generic message. Anything else, and a failure no popup showed, goes to the HUD notification
// (HUDMinorNotificationControl: a title line over a text line, for about four seconds; the barn-find and club-tier
// pop-ups use it). That control is part of the driving HUD, and the game skips it when the player has turned off
// Minor Notifications in the HUD options.
#pragma once

#include "fh2mod.h"

#include <cstdint>

// Hooks the notification component's constructor and destructor (to know the live one) and a game-thread tick
// that shows queued messages. Call once the game's code is ready, before enable_hooks.
bool notify_install(const FH2ModApi* api, FH2Mod* mod, uintptr_t base);

// A request to the community server failed (any thread). For the next few seconds the Storefront's error popup shows
// `text` in place of the game's generic message, when the popup's code is `hr` (0x8051xxxx for the server's error reply
// with code xxxx, the HRESULT a failed connection was reported with) or `hr` is 0 (not known). If no popup asks for
// it, it shows as a HUD notification, unless `toast` is false (a reason for an error that may not happen).
void notify_request_failed(uint32_t hr, const char* text, bool toast = true);

// Queues a message, from any thread. Text is shown as ASCII (anything else becomes '?') and cut to fit. The same
// text again within half a minute is dropped, so a failing background request doesn't repeat itself.
void notify_player(const char* title, const char* text);
