# Stream Deck / Bitfocus Companion setup

DDP Playlist Player can be driven two ways. Use whichever fits the button.

| | Simple hotkeys | Vendor requests (recommended) |
|---|---|---|
| Setup | Pick an ID in Companion | Type a request into a Companion button |
| Can do | Play/pause, stop, next, previous, restart, take/end break | Everything, including *load "Break 2" and take it* |
| Button lights up when playing | No | Yes |

Both need Companion's **OBS Studio** connection (module `obs-studio`) pointed at OBS's
WebSocket server: in OBS go to *Tools → WebSocket Server Settings*, tick *Enable*, and copy the
port/password into the Companion connection.

---

## Option A – Vendor requests

Add a button → **Press actions** → *OBS: Custom - Send Vendor Request*.

- **vendorName:** `DDPPlaylist`
- **requestType:** one from the table below
- **requestData:** JSON, or leave empty

| requestType | requestData example | What it does |
|---|---|---|
| `TakeBreak` | *(empty)* | Cut to break scene, play selected playlist from the top |
| `TakeBreak` | `{"playlist":"Break 2"}` | Select "Break 2", then take the break |
| `EndBreak` | | Cut back to the show scene now |
| `ToggleBreak` | | Take break / end break on one button |
| `PlayPause` | | Play or pause |
| `Play` / `Pause` / `Stop` | | |
| `Next` / `Previous` / `Restart` | | Previous restarts the clip if it's more than 3 s in |
| `PlayItem` | `{"index":3}` | Play clip #3 of the selected playlist |
| `PlayItem` | `{"name":"Ford Spot"}` | Play a clip by its display name |
| `LoadPlaylist` | `{"project":"Walleye Tour","playlist":"Break 1"}` | Select only (cue it up) |
| `LoadPlaylist` | `{"playlist":"Sponsor Reel","play":true}` | Select and play (no scene cut) |
| `LoadPlaylist` | `{"playlist":"Break 3","break":true}` | Select and take the break |
| `SelectProject` | `{"name":"Walleye Tour"}` | Switch project |
| `SelectPlaylist` | `{"name":"Break 2"}` | Switch playlist in the current project |
| `SetLoop` | `{"enabled":true}` | Loop on/off for the selected playlist |
| `AddClips` | `{"path":"C:/Ads/Ford.mp4"}` | Append a clip (`paths` takes `a.mp4\|b.mp4`) |
| `AddPlayerToScene` | | Put the player source in the current scene |
| `GetState` / `ListProjects` / `ListItems` | | Read-only, for scripts |

Names are matched without caring about upper/lower case. Numbers (`index`, or `project`/`playlist`
as a number) count from 1.

### Button feedback (light up while on air)

Add a **Feedback** → *OBS: Custom - Vendor Event*:

- **vendorName:** `DDPPlaylist`
- **eventType:** `StateChanged`
- **eventData Key / Value** – pick one:

| Key | Value | Lights when |
|---|---|---|
| `onBreak` | `true` | A break is on air (great for the TAKE BREAK button – make it red) |
| `state` | `playing` | Anything is playing |
| `state` | `paused` | Paused |
| `playlist` | `Break 2` | "Break 2" is the playlist playing |
| `item` | `Ford Spot` | That clip is on air |

Every change sends one `StateChanged` event with all of these fields
(`state`, `onBreak`, `project`, `selectedPlaylist`, `playlist`, `item`, `index`, `count`), all as text.

### A good starter page

| Button | Action | Feedback |
|---|---|---|
| TAKE BREAK | `ToggleBreak` | `onBreak` = `true` → red |
| BREAK 1 | `LoadPlaylist` `{"playlist":"Commercial Break 1","break":true}` | `playlist` = `Commercial Break 1` → gold |
| BREAK 2 | `LoadPlaylist` `{"playlist":"Commercial Break 2","break":true}` | `playlist` = `Commercial Break 2` → gold |
| ⏯ | `PlayPause` | `state` = `playing` → green |
| ⏭ | `Next` | |
| ⏹ | `Stop` | |

---

## Option B – OBS hotkeys

No key binding needed. In Companion use *OBS: Hotkey - Trigger by ID* with one of these IDs:

| ID | Does |
|---|---|
| `ddp_playlist.play_pause` | Play / pause |
| `ddp_playlist.stop` | Stop |
| `ddp_playlist.next` | Next clip |
| `ddp_playlist.previous` | Previous clip |
| `ddp_playlist.restart` | Restart clip |
| `ddp_playlist.take_break` | Take break (cut to commercials) |
| `ddp_playlist.end_break` | End break (back to show) |
| `ddp_playlist.toggle_break` | Toggle break |

The same actions are also in OBS *Settings → Hotkeys* (search **DDP Playlist**) if you want
keyboard shortcuts or a plain Stream Deck hotkey button.
