# Spotify ESP controller

A Spotify now-playing screen and swipe-to-open photo frame with touch controls on the Hosyond 4.0" ESP32 board
(ESP32-WROOM-32E, ST7796 320x480, XPT2046 resistive touch). The ESP32 talks to the
Spotify Web API directly by default. A Raspberry Pi can instead handle Spotify
polling, authentication, artwork, and commands; see [Pi setup](pi/README.md).

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
America/Los_Angeles time (including daylight saving time); change `CLOCK_TIMEZONE`
in `src/config.h` for another timezone. Until NTP syncs, it shows `--:--`.
In Pi mode, the idle screen also shows small weather details for Martinez, CA
in Celsius and separate Wi-Fi/Pi connection indicators. Weather is fetched and
cached on the Pi every 15 minutes; see [weather configuration](pi/README.md#idle-weather-and-connection-indicators).
Idle polling and connection errors keep the clock visible, and only the time
area redraws when the minute changes. Album art loads for both playing and paused
tracks. The player returns automatically when Spotify reports a track again.
If Spotify cannot load a track because of an authorization or Premium error,
the clock stays visible with a brief error notice. The clock and photo frame
do not require Spotify Premium; time synchronization only needs internet access.
The controller reads Spotify's playback state; closing the app switches to the
clock once Spotify stops reporting that device/track.

## Photo frame (internal storage, no microSD needed)

Swipe horizontally anywhere on the display to switch between Spotify and your
photo album. Spotify keeps syncing in the background; swiping back restores its
latest state. Taps register on release so swipes do not trigger playback buttons.

Photos advance every **5 minutes**. Tap a photo to advance immediately; tap the
clock badge in the top-right corner to cycle brightness. The clock uses the
same America/Los_Angeles timezone as the idle clock and shows `--:--` until NTP syncs.
Change `PHOTO_INTERVAL_MS` or `CLOCK_TIMEZONE` in `src/config.h` to customize them.

### Add your photos

1. Put around **10 original photos** in the project's `photos/` folder. Use JPG,
   PNG, WebP, or BMP; export HEIC images to JPEG first. Files play in filename order,
   so use names such as `01-family.jpg`, `02-vacation.jpg`.
2. Set up the preparation helper once (from the project root):

   ```bash
   python3 -m venv .venv
   .venv/bin/python -m pip install -r tools/requirements-photos.txt
   ```

3. Prepare the images:

   ```bash
   .venv/bin/python tools/prepare_photos.py
   ```

   Originals stay untouched. The helper corrects EXIF orientation, removes image
   metadata, and writes 480x320 baseline JPEGs to `data/photos/`. It preserves the
   whole photo with dark borders when needed. To fill the screen by cropping:

   ```bash
   .venv/bin/python tools/prepare_photos.py --crop
   ```

4. Connect the display over USB. Upload the updated firmware **once**, then the
   photo filesystem:

   ```bash
   pio run -e controller -t upload
   pio run -e controller -t uploadfs
   ```

   You can also use PlatformIO's **Upload** and **Upload Filesystem Image** tasks
   under the `controller` environment. Close the serial monitor before uploading.

When changing the album later, add/remove originals in `photos/`, rerun the helper,
and run only `pio run -e controller -t uploadfs`. This replaces the previous uploaded
album. Ordinary firmware uploads keep the album with this unchanged partition map.
WiFi credentials, brightness, and touch calibration live in a separate NVS partition.
See [PlatformIO's filesystem upload documentation](https://docs.platformio.org/en/latest/platforms/espressif32.html#uploading-files-to-file-system).

The existing flash layout reserves **896 KiB** for files. The helper caps each
prepared image at 60 KiB and all data at 700 KiB, leaving room for filesystem
metadata; ten photos fit within that budget. Up to 32 photos are supported if they
fit. Photos stream directly from LittleFS without loading an entire photo into RAM.
Original and generated photo files are git-ignored. Only files under `data/` are
included in the filesystem upload; keep originals and documentation in `photos/`.

If the photo screen says **Add your photos**, prepare the images and upload the
filesystem. A failed mount never automatically formats or erases the photo storage.
The album remains available without WiFi; after a cold boot, the clock needs WiFi
once to obtain the time. No photo-upload webpage or microSD card is required.

## Envs

| Env | What it builds |
| --- | --- |
| `controller` | Spotify player (default) |
| `controller-pi` | Spotify player reading the Raspberry Pi's local cache |
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
- `src/ui/`: player screen, photo frame, swipe recognition, icons, theme/layout, text helpers
- `photos/`: original personal photos (git-ignored)
- `data/photos/`: prepared JPEGs uploaded to LittleFS (git-ignored)
- `tools/prepare_photos.py`: desktop resizing, orientation correction, and JPEG conversion
- `src/net/`: WiFi and clock sync
- `lib/TouchCalibration/`: touch calibration shared with the bring-up test
- `tools/get_refresh_token.py`: one-time Spotify login

## Local checks

Photo preparation tests (after installing the helper requirements):

```bash
.venv/bin/python -m unittest discover -s test -p 'test_*.py'
```

Gesture regression check (host C++ compiler):

```bash
c++ -std=c++11 test/host_touch_gesture.cpp -o /tmp/host_touch_gesture
/tmp/host_touch_gesture
```

Build firmware and the filesystem without flashing:

```bash
pio run -e controller
pio run -e controller -t buildfs
```
