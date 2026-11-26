# Raspberry Pi polling service

The Pi owns Spotify polling, authentication, playback commands, and artwork
downloads. The ESP32 reads small cached responses over LAN HTTP and still runs
the screen, clock, brightness, and photo frame locally. Python 3.9+ is required;
there are no pip dependencies. Use this service on your private LAN.

## Configure on the laptop

From the project root, with your existing Spotify credentials in
`include/secrets.h`, run:

```bash
python3 tools/configure_pi.py --url http://10.0.0.248:8765
```

This keeps your Wi-Fi settings, adds the Pi address and a random shared API key
to `include/secrets.h`, and writes `pi/spotify.env` with the matching key and your
Spotify credentials. Both files are git-ignored and written with private file
permissions. It never prints the credentials. Rerunning keeps the same API key.
Keep the Pi at this address using a DHCP reservation on your router.

## Copy to the Pi

In your existing **Pi SSH session**, create the destination:

```bash
mkdir -p ~/projects/Spotify_ESP
```

Then in a **laptop terminal**, from this project's root:

```bash
scp -r pi jasonyu0705@10.0.0.248:~/projects/Spotify_ESP/
```

Only this small directory is needed on the Pi; no PlatformIO installation or
firmware source is required. On a different Pi, change the username/address.
If the direct ESP32 backend saved a newer refresh token in flash and this token
is rejected, rerun `tools/get_refresh_token.py` on the laptop, update the token in
`include/secrets.h`, rerun configuration, and copy `spotify.env` again.

## Start in the Pi SSH session

```bash
cd ~/projects/Spotify_ESP/pi
chmod 600 spotify.env
python3 spotify_bridge.py
```

Start Spotify on your phone/computer. Logs should show `Playback: ok` and
`Cached artwork`. If no device has a track loaded, `nothing_playing` is normal.
The service listens on port **8765** by default.

In another terminal, check reachability (this reveals no playback or credentials):

```bash
curl http://10.0.0.248:8765/health
```

It should return `{"ok": true, "version": 1}`. Playback and commands additionally
require the generated API key; health alone does not confirm Spotify is linked.
If health fails from the laptop but works on the Pi, check LAN isolation and any
Pi firewall. Allow TCP 8765 from the local network; do not forward it on the router.

## Flash the ESP32 from the laptop

```bash
pio run -e controller-pi -t upload -t monitor
```

Use **controller-pi**, which excludes the direct Spotify backend from the firmware.
The normal **controller** environment remains available for direct Spotify access.
Both use the same flash layout and LittleFS photo partition. No filesystem upload
is needed for this change. The existing clock and photo frame remain available
when the Pi is offline. NTP still synchronizes the ESP32 clock independently.

## Run on boot

After verifying it in the foreground, press Ctrl-C on the Pi and run:

```bash
mkdir -p ~/.config/systemd/user
cp ~/projects/Spotify_ESP/pi/spotify-bridge.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now spotify-bridge.service
sudo loginctl enable-linger "$USER"
```

The supplied unit assumes the installation path `~/projects/Spotify_ESP/pi`.
Change `WorkingDirectory` and `ExecStart` if you use another path.
Linger starts the user service at boot and keeps it running after SSH disconnects.
Inspect it with:

```bash
systemctl --user status spotify-bridge.service
journalctl --user -u spotify-bridge.service -n 40 --no-pager
```

After replacing code or configuration, run
`systemctl --user restart spotify-bridge.service`.

## Idle weather and connection indicators

The Pi fetches current weather from [Open-Meteo](https://open-meteo.com/en/docs)
on its own background thread, **once every 15 minutes** by default. The ESP32
reads this cache in `/state`; its regular reads never trigger weather-provider
requests. Weather is included even when Spotify is idle, unlinked, or unsupported.
The display keeps the large clock and adds a small weather icon, temperature,
conditions, and location. Wi-Fi and Pi connectivity have separate indicators.
"Pi online" means a recent authenticated Pi response, regardless of Spotify status.
"Wi-Fi online" means the ESP32 is connected to its router; it is not a separate
internet-connectivity check.

Defaults are Martinez, California 94553 (`38.01937, -122.13413`), in Celsius.
This is a fixed configured location, not the Pi's detected location. Optional
settings in the private `spotify.env` file override the defaults:

```text
WEATHER_LATITUDE=38.01937
WEATHER_LONGITUDE=-122.13413
WEATHER_LOCATION='Martinez, CA'
WEATHER_UNIT=celsius
WEATHER_REFRESH_SECONDS=900
```

Use `WEATHER_REFRESH_SECONDS=3600` for hourly updates. Restart the Pi service
after changing these values; no ESP32 reflash is needed for location, units, or
refresh interval changes. The Pi sends the configured coordinates to Open-Meteo.
The free endpoint requires no API key for this personal, non-commercial project.
The idle screen shows the Pi server address at the bottom left. Tap the weather
row to view the provider credit in the status bar. Weather data is provided by
[Open-Meteo](https://open-meteo.com/) under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/);
the display rounds temperatures and maps weather codes to concise labels/icons.

Failed requests keep the previous weather with a `(cached)` label and wait until
the next scheduled refresh. With no successful reading, the display says weather
is unavailable. Pi connection failures also mark retained weather as cached.
The existing clock and photo album continue working independently.

### Update an existing Pi installation

From the project root in a **Mac terminal**, copy both Python files:

```bash
scp pi/spotify_bridge.py pi/weather.py jasonyu0705@10.0.0.248:~/projects/Spotify_ESP/pi/
```

Then, in the **Pi SSH session**, restart the systemd service:

```bash
systemctl --user restart spotify-bridge.service
journalctl --user -u spotify-bridge.service -n 30 --no-pager
```

If running it manually instead, stop it with Ctrl-C and start
`python3 ~/projects/Spotify_ESP/pi/spotify_bridge.py` again.
Look for `Weather updated for Martinez, CA`. Then flash the matching UI from
the Mac with `pio run -e controller-pi -t upload`.
An older Pi service still works for Spotify; the new UI prompts to update the
bridge when the weather field is missing. The direct `controller` environment
shows Wi-Fi status; weather requires `controller-pi`.

## Behavior and local API

The Pi polls once a second during active/paused playback, once every three seconds
when nothing is loaded, and backs off on connection/authentication errors. It
honors Spotify's `Retry-After` response before sending more polls or commands.
Access tokens refresh automatically; any rotated refresh token is saved in
`spotify.tokens.json`. An expired/revoked refresh token needs a new login.
See Spotify's [rate limits](https://developer.spotify.com/documentation/web-api/concepts/rate-limits)
and [refresh token documentation](https://developer.spotify.com/documentation/web-api/tutorials/refreshing-tokens).

Album art downloads on a separate thread so it cannot block commands or polls.
The Pi caches up to four 300px JPEGs, each capped at 64 KiB to fit the ESP32's
existing decoder. Failed downloads back off; missing/oversized art shows the
existing placeholder. The ESP32 never fetches artwork directly from Spotify.

All endpoints except health require `Authorization: Bearer <PI_API_TOKEN>`:

| Endpoint | Behavior |
| --- | --- |
| `GET /health` | Service reachability and API version; no Spotify request |
| `GET /state` | Cached status and display metadata; no Spotify request |
| `GET /art/<hash>.jpg` | Cached JPEG only; no outbound fetch |
| `POST /command` | JSON `{"command":"play"}`; also pause, next, previous |
| `POST /command` | `{"command":"shuffle","value":true}` |
| `POST /command` | `{"command":"repeat","value":"off"}`; also context, track |

State and command responses use `version: 1` and a string status: `ok`,
`nothing_playing`, `no_active_device`, `not_authorized`, `network_error`,
`rate_limited`, or `unsupported`. Cached progress advances using the Pi's monotonic
clock. Successful commands refresh the cache before replying. Reads continue to
work during Spotify requests; playback older than 30 seconds is reported as an
error rather than displayed indefinitely as current.

Optional `spotify.env` settings: `PI_BIND` (default `0.0.0.0`), `PI_PORT` (8765),
`SPOTIFY_POLL_INTERVAL` (1–60 seconds), and `SPOTIFY_TOKEN_FILE` (default beside the
env file). Keep the API token and these files private.
