# Spotify ESP controller

A Spotify now-playing screen with touch controls on the Hosyond 4.0" ESP32 board
(ESP32-WROOM-32E, ST7796 320x480, XPT2046 resistive touch). The ESP32 talks to the
Spotify Web API directly; no server needed.

Actual album covers download over HTTPS in the background and are cached until
the cover URL changes. Transfers yield to queued playback commands between
chunks; an in-progress HTTPS connection setup still has to finish first.
The obsolete cover cache is released when the artwork URL changes to leave room
for the next HTTPS transfer. Failed downloads retry after 2, 5, 15, then 30 seconds;
transfers interrupted by playback commands retry promptly. Body downloads have
a separate timeout from connection setup, and stalls are logged.
JPEGs are decoded directly into the 264px artwork area
without a full framebuffer. A placeholder appears while loading or if the image
is unavailable (downloads are limited to 64 KB on this board without PSRAM).

## Setup

### 1. Create a Spotify developer app (once)

1. Go to <https://developer.spotify.com/dashboard> and click **Create app**.
2. Fill in any name and description.
3. Under **Redirect URIs** add exactly `http://127.0.0.1:8888/callback`
   (use `127.0.0.1`, not `localhost`).
4. Under **Which API/SDKs are you planning to use?** check **Web API**, then save.
5. Open the app's **Settings** and note the **Client ID** and **Client Secret**.

Controlling playback needs Spotify **Premium** on the account you log in with.

### 2. Get a refresh token (once, on your laptop)

```bash
python3 tools/get_refresh_token.py
```

Paste the Client ID and Secret when asked. Your browser opens Spotify's login;
approve it and the script prints three `#define` lines.

### 3. Fill in `include/secrets.h`

Copy `include/secrets.h.example` to `include/secrets.h` if it isn't there, then set:

- `WIFI_SSID` / `WIFI_PASSWORD`: a 2.4 GHz network (the ESP32 can't see 5 GHz)
- `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET`: from step 1
- `SPOTIFY_REFRESH_TOKEN`: from step 2

`secrets.h` is git-ignored. Don't share it: the refresh token controls your account's playback.

### 4. Flash

```bash
pio run -e controller -t upload -t monitor
```

Start music in Spotify on your phone or computer. The screen should
show it within a second or two after connecting.

The bottom-right repeat button replaces volume control. Tap to cycle through
Off (gray), Repeat all (green), and Repeat one (green with a `1`). The label
below the icon shows OFF, ALL, or ONE. Changes made in Spotify sync back to the
controller on the next playback poll.

The sun button at the bottom center controls display brightness locally:
100% → 75% → 50% → 25% → 10% → 100%. It works on the player and status screens.
Your setting is saved one second after the last tap and restored on restart.

Paused music keeps the Spotify screen, artwork, and controls visible. When
Spotify reports no active track or device, the screen instead shows a large
12-hour clock with AM/PM and the date, with "Nothing is playing" underneath. The clock uses
America/Chicago time (including daylight saving time); change `CLOCK_TIMEZONE`
in `src/config.h` for another timezone. Until NTP syncs, it shows `--:--`.
Idle polling and connection errors keep the clock visible, and only the time
area redraws when the minute changes. Album art loads for both playing and paused
tracks. The player returns automatically when Spotify reports a track again.
The controller reads Spotify's playback state; closing the app switches to the
clock once Spotify stops reporting that device/track.

## Envs

| Env | What it builds |
| --- | --- |
| `controller` | Spotify player (default) |
| `bringup` | Screen + touch hardware test (`testing/bringup/`) |

## Touch calibration

Calibration is saved in flash, one per screen rotation: the player (landscape) and
the bring-up test (portrait) each calibrate once. To redo it: hold BOOT for 1 second,
send `c` in the serial monitor, or hold the screen while pressing RESET.

If the player shows upside down, set `SCREEN_ROTATION` to 3 in `src/config.h`.

## Troubleshooting

The serial monitor (115200 baud) logs every failed Spotify call with its reason.

| Screen | Cause |
| --- | --- |
| WiFi not connected | Wrong SSID/password, or a 5 GHz-only network |
| Can't connect | No internet, clock not synced, or a certificate error (see `src/backend/SpotifyCerts.h`) |
| Spotify not linked | Refresh token missing, revoked or for a different app; rerun step 2 |
| Clock / Nothing is playing | Spotify reports no active track/device; open Spotify and load a track to return to the player |
| "Not supported" notice on a tap | Spotify refused the command: usually Premium required, or the device restricts it |

## Code layout

- `src/backend/`: `PlayerBackend` interface, `SpotifyBackend`, and
  `BackendWorker` (runs the backend on core 0 so network calls never freeze the UI)
- `src/ui/`: the player screen, icons, theme/layout, text helpers
- `src/net/`: WiFi and clock sync
- `lib/TouchCalibration/`: touch calibration shared with the bring-up test
- `tools/get_refresh_token.py`: one-time Spotify login

