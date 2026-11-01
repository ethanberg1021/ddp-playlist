/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

/* Outside control of the player:
 *  - OBS hotkeys (Settings > Hotkeys, and Companion's "Trigger hotkey")
 *  - obs-websocket vendor requests under the vendor name "DDPPlaylist"
 *    (Companion's OBS module: "Custom - Send Vendor Request")
 *  - a "StateChanged" vendor event for Companion button feedback */
namespace RemoteControl {
void registerHotkeys();
void unregisterHotkeys();
void saveHotkeys();
void registerWebsocketVendor(); // call from obs_module_post_load
void shutdown();
} // namespace RemoteControl
