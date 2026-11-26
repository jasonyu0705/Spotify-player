#!/usr/bin/env python3
"""Poll Spotify on a Pi and serve cached state/art to the ESP32. Python 3.9+."""

import argparse
import base64
from collections import OrderedDict
from datetime import datetime
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http.client import HTTPException
import json
import logging
import math
import os
from pathlib import Path
import shlex
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from weather import WeatherCache

LOG = logging.getLogger("spotify-bridge")
MAX_ART_BYTES = 64 * 1024
MAX_JSON_BYTES = 512 * 1024
DEFAULT_POLL_INTERVAL = 1.0


def load_env(path):
    """Read quoted KEY=value pairs without executing shell code."""
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.replace("_", "").isalnum():
            raise ValueError("Invalid environment file entry")
        parts = shlex.split(value, comments=True)
        if len(parts) > 1:
            raise ValueError("Quote environment values containing spaces")
        os.environ.setdefault(key, parts[0] if parts else "")


def write_private_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(data, stream)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class SpotifyError(Exception):
    def __init__(self, status):
        self.status = status
        super().__init__(status)


def status_for(code):
    if 200 <= code < 300:
        return "ok"
    return {401: "not_authorized", 400: "unsupported", 403: "unsupported",
            404: "no_active_device", 429: "rate_limited"}.get(code, "network_error")


class SpotifyClient:
    """Accessed under Bridge.spotify_lock; all rate limits apply to commands too."""

    def __init__(self, client_id, client_secret, refresh_token, token_file):
        self.client_id = client_id
        self.client_secret = client_secret
        self.refresh_token = refresh_token
        self.token_file = Path(token_file)
        self.seed = hashlib.sha256((client_id + ":" + refresh_token).encode()).hexdigest()
        self.access_token = ""
        self.expires_at = 0
        self.retry_at = 0
        self.retry_at_utc = 0
        self.rate_limit_failures = 0
        self.rate_limit_reason = ""
        self.cooldown_file = self.token_file.with_suffix(".ratelimit.json")
        self.app_fingerprint = hashlib.sha256(client_id.encode()).hexdigest()
        self.auth_retry_at = 0
        try:
            saved = json.loads(self.token_file.read_text())
            if saved.get("seed") == self.seed:
                self.refresh_token = saved.get("refresh_token") or refresh_token
        except FileNotFoundError:
            pass
        try:
            saved = json.loads(self.cooldown_file.read_text())
            if saved.get("app") == self.app_fingerprint:
                deadline = float(saved["retry_at_utc"])
                if not math.isfinite(deadline):
                    raise ValueError("Invalid cooldown deadline")
                self.retry_at_utc = deadline
                self.rate_limit_failures = max(0, min(12, int(saved.get("failures", 1))))
                self.rate_limit_reason = str(saved.get("reason", "RATE_LIMITED"))[:64]
                if self.cooldown_remaining():
                    LOG.warning("Resuming Spotify cooldown (%d seconds remaining)", self.cooldown_remaining())
        except FileNotFoundError:
            pass
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            # A damaged state file must not turn a restart into rapid retries.
            self.retry_at_utc = time.time() + 900
            self.rate_limit_reason = "COOLDOWN_STATE_UNREADABLE"
            LOG.warning("Cannot read Spotify cooldown state; waiting 15 minutes")

    def cooldown_remaining(self):
        # A live process uses monotonic time. Restored cooldowns use wall time
        # so the remaining wait survives a restart or reboot.
        remaining = self.retry_at - time.monotonic() if self.retry_at else self.retry_at_utc - time.time()
        return max(0, math.ceil(remaining))

    def _save_cooldown(self):
        write_private_json(self.cooldown_file, {"version": 1, "app": self.app_fingerprint,
                           "retry_at_utc": self.retry_at_utc, "failures": self.rate_limit_failures,
                           "reason": self.rate_limit_reason})

    def record_cooldown_until(self, deadline):
        """Migrate an older bridge's logged deadline without calling Spotify."""
        if not math.isfinite(deadline):
            raise ValueError("Invalid cooldown deadline")
        self.retry_at_utc = max(self.retry_at_utc, deadline)
        self.retry_at = time.monotonic() + max(0, self.retry_at_utc - time.time())
        self.rate_limit_failures = max(1, self.rate_limit_failures)
        self.rate_limit_reason = self.rate_limit_reason or "RATE_LIMITED"
        self._save_cooldown()

    def _rate_limited(self, headers, body):
        headers = headers or {}
        try:
            error = json.loads(body).get("error", {})
            quota_exceeded = isinstance(error, dict) and error.get("reason") == "QUOTA_EXCEEDED"
        except (ValueError, TypeError, AttributeError):
            quota_exceeded = False
        quota_exceeded = quota_exceeded or self.rate_limit_reason == "QUOTA_EXCEEDED"
        self.rate_limit_reason = "QUOTA_EXCEEDED" if quota_exceeded else "RATE_LIMITED"
        base = 900 if quota_exceeded else 60
        fallback = min(3600, base * (2 ** min(self.rate_limit_failures, 6)))
        try:
            requested = float(headers.get("Retry-After", ""))
            if not math.isfinite(requested) or requested < 0:
                raise ValueError("Invalid retry delay")
            delay = max(1, math.ceil(requested))
        except (ValueError, TypeError):
            delay = fallback
        self.rate_limit_failures += 1
        self.retry_at = time.monotonic() + delay
        self.retry_at_utc = time.time() + delay
        try:
            self._save_cooldown()
        except OSError:
            LOG.error("Could not save Spotify cooldown; avoid restarting until it expires")
        LOG.warning("Spotify rate limited; waiting %d seconds (%s)", delay, self.rate_limit_reason)

    def _clear_cooldown(self):
        had_cooldown = bool(self.retry_at_utc or self.rate_limit_failures)
        self.retry_at = self.retry_at_utc = self.rate_limit_failures = 0
        self.rate_limit_reason = ""
        if had_cooldown:
            try:
                self.cooldown_file.unlink(missing_ok=True)
            except OSError:
                LOG.warning("Could not remove expired Spotify cooldown file")

    def _request(self, method, url, authorization, data=None):
        headers = {"Authorization": authorization, "User-Agent": "SpotifyESP-Pi/1"}
        if data is not None:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        request = urllib.request.Request(url, headers=headers, data=data, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                body = response.read(MAX_JSON_BYTES + 1)
                if len(body) > MAX_JSON_BYTES:
                    raise SpotifyError("network_error")
                parsed = json.loads(body) if body else None
                if url.startswith("https://api.spotify.com/"):
                    self._clear_cooldown()
                return response.status, parsed
        except urllib.error.HTTPError as error:
            if error.code == 429:
                try:
                    error_body = error.read(8192)
                except (OSError, HTTPException):
                    error_body = b""
                self._rate_limited(error.headers, error_body)
            code = error.code
            error.close()
            return code, None
        except (OSError, urllib.error.URLError, HTTPException, ValueError) as error:
            LOG.warning("Spotify request failed (%s)", type(error).__name__)
            raise SpotifyError("network_error") from error

    def _refresh(self):
        now = time.monotonic()
        if now < self.auth_retry_at:
            raise SpotifyError("not_authorized")
        authorization = base64.b64encode(
            (self.client_id + ":" + self.client_secret).encode()).decode()
        payload = urllib.parse.urlencode({"grant_type": "refresh_token",
                                          "refresh_token": self.refresh_token}).encode()
        code, body = self._request("POST", "https://accounts.spotify.com/api/token",
                                   "Basic " + authorization, payload)
        if code in (400, 401):
            self.auth_retry_at = time.monotonic() + 30
            raise SpotifyError("not_authorized")
        if code != 200:
            raise SpotifyError(status_for(code))
        if not isinstance(body, dict) or not body.get("access_token"):
            raise SpotifyError("not_authorized")
        self.access_token = body["access_token"]
        self.expires_at = time.monotonic() + max(1, int(body.get("expires_in", 3600)) - 60)
        rotated = body.get("refresh_token")
        if rotated and rotated != self.refresh_token:
            # Persist before using the new token so restarting the service keeps it.
            write_private_json(self.token_file, {"seed": self.seed, "refresh_token": rotated})
            self.refresh_token = rotated

    def call(self, method, path):
        if self.cooldown_remaining():
            raise SpotifyError("rate_limited")
        if not self.access_token or time.monotonic() >= self.expires_at:
            self._refresh()
        for attempt in range(2):
            code, body = self._request(method, "https://api.spotify.com/v1/me/player" + path,
                                       "Bearer " + self.access_token,
                                       b"" if method != "GET" else None)
            if code != 401 or attempt:
                return code, body
            self.access_token = ""
            self._refresh()


def pick_image(images):
    candidates = [image for image in images or [] if image.get("url", "").startswith("https://")]
    return min(candidates, key=lambda image: abs((image.get("width") or 0) - 300))["url"] if candidates else ""


def playback_state(body):
    """Keep Spotify's large response on the Pi; expose only display fields."""
    item = body.get("item")
    progress = max(0, body.get("progress_ms") or 0)
    state = {"device_name": (body.get("device") or {}).get("name", ""),
             "is_playing": bool(body.get("is_playing")), "progress_ms": progress,
             "shuffle": bool(body.get("shuffle_state")),
             "repeat": body.get("repeat_state", "off")}
    if not item:
        if body.get("currently_playing_type") != "ad":
            return None, ""
        state.update(track_id="ad", title="Advertisement", artist="Spotify", album="",
                     duration_ms=max(progress, 30000))
        return state, ""
    show = item.get("show")
    album = item.get("album") or {}
    state.update(track_id=item.get("id") or item.get("uri") or item.get("name") or "unknown",
                 title=item.get("name", ""), duration_ms=max(0, item.get("duration_ms") or 0),
                 artist=show.get("name", "") if show else ", ".join(
                     artist.get("name", "") for artist in item.get("artists", [])),
                 album=show.get("name", "") if show else album.get("name", ""))
    return state, pick_image(item.get("images") if show else album.get("images"))


def art_path(url):
    return "/art/" + hashlib.sha256(url.encode()).hexdigest() + ".jpg" if url else ""


def command_request(command, value=None):
    simple = {"play": ("PUT", "/play"), "pause": ("PUT", "/pause"),
              "next": ("POST", "/next"), "previous": ("POST", "/previous")}
    if command in simple:
        return simple[command]
    if command == "shuffle" and isinstance(value, bool):
        return "PUT", "/shuffle?state=" + str(value).lower()
    if command == "repeat" and value in ("off", "context", "track"):
        return "PUT", "/repeat?state=" + value
    raise ValueError("Unknown command or invalid value")


class Bridge:
    def __init__(self, spotify, poll_interval=DEFAULT_POLL_INTERVAL, weather=None, polling_file=None):
        self.spotify = spotify
        self.weather = weather
        self.poll_interval = poll_interval
        self.spotify_lock = threading.Lock()
        self.state_lock = threading.Lock()
        self.stop = threading.Event()
        self.poll_wakeup = threading.Event()
        self.art_wakeup = threading.Event()
        self.display_timeout = 60
        self.last_display_seen = None
        self.polling_enabled = True
        self.polling_file = Path(polling_file) if polling_file else None
        if self.polling_file:
            try:
                saved = json.loads(self.polling_file.read_text())
                if not isinstance(saved.get("enabled"), bool):
                    raise ValueError("Invalid polling preference")
                self.polling_enabled = saved["enabled"]
            except FileNotFoundError:
                pass
            except (OSError, ValueError, TypeError, AttributeError):
                self.polling_enabled = False
                LOG.warning("Cannot read polling preference; Spotify updates disabled until enabled manually")
        self.status = "network_error"
        self.state = None
        self.observed_at = 0
        self.desired_art = ""
        self.art_cache = OrderedDict()
        self.threads = []

    def _polling_snapshot_locked(self):
        online = self.last_display_seen is not None and time.monotonic() - self.last_display_seen < self.display_timeout
        active = self.polling_enabled and online
        return {"enabled": self.polling_enabled, "active": active, "display_online": online,
                "reason": "manual_off" if not self.polling_enabled else "enabled" if online else "display_offline",
                "display_timeout_seconds": self.display_timeout}

    def polling_snapshot(self):
        with self.state_lock:
            return self._polling_snapshot_locked()

    def display_seen(self):
        # Authenticated /state reads are the check-in. Health checks, artwork,
        # and terminal polling controls never keep an absent display awake.
        with self.state_lock:
            was_active = self._polling_snapshot_locked()["active"]
            self.last_display_seen = time.monotonic()
            active = self.polling_enabled
        if active and not was_active:
            LOG.info("Display connected; Spotify polling may resume")
            self.poll_wakeup.set()

    def set_polling(self, enabled):
        if not isinstance(enabled, bool):
            raise ValueError("enabled must be a boolean")
        # Finish an in-flight Spotify request before acknowledging OFF. Save the
        # preference before changing it, so a failed write isn't reported as OK.
        with self.spotify_lock:
            if self.polling_file:
                write_private_json(self.polling_file, {"enabled": enabled})
            with self.state_lock:
                self.polling_enabled = enabled
                self.state = None
                self.desired_art = ""
                self.status = "network_error" if enabled else "polling_disabled"
            self.art_wakeup.set()
        LOG.info("Spotify updates manually %s", "enabled" if enabled else "disabled")
        self.poll_wakeup.set()
        return self.polling_snapshot()

    def snapshot(self):
        with self.state_lock:
            polling = self._polling_snapshot_locked()
            status = self.status
            state = dict(self.state) if self.state else None
            if not polling["active"]:
                status, state = "polling_disabled", None
            age = max(0, time.monotonic() - self.observed_at)
            if status == "ok" and age > 30:
                status, state = "network_error", None
            if state:
                if state["is_playing"]:
                    state["progress_ms"] = min(state["duration_ms"], state["progress_ms"] + int(age * 1000))
                path = art_path(self.desired_art)
                state["art_path"] = path if path in self.art_cache else ""
            result = {"version": 1, "status": status, "now_playing": state, "polling": polling}
        # Weather uses a separate lock/thread and is present even when Spotify
        # reports idle, authorization errors, or unsupported playback.
        result["weather"] = self.weather.snapshot() if self.weather else {"status": "not_configured"}
        result["retry_after_seconds"] = self.spotify.cooldown_remaining()
        result["rate_limit_reason"] = self.spotify.rate_limit_reason if result["retry_after_seconds"] else ""
        return result

    def _poll_locked(self):
        try:
            code, body = self.spotify.call("GET", "?additional_types=episode&market=from_token")
            status = status_for(code)
            state, url = None, ""
            if code == 204:
                status = "nothing_playing"
            elif code == 200:
                if not isinstance(body, dict):
                    raise SpotifyError("network_error")
                state, url = playback_state(body)
                if state is None:
                    status = "nothing_playing"
        except SpotifyError as error:
            status, state, url = error.status, None, ""
        with self.state_lock:
            if self.status != status:
                LOG.info("Playback: %s", status)
            self.status, self.state = status, state
            self.observed_at = time.monotonic()
            changed = self.desired_art != url
            self.desired_art = url
        if changed:
            self.art_wakeup.set()

    def poll_once(self):
        with self.spotify_lock:
            if not self.polling_snapshot()["active"]:
                with self.state_lock:
                    if self.status != "polling_disabled":
                        LOG.info("Spotify polling suspended: %s", self._polling_snapshot_locked()["reason"])
                    self.status, self.state, self.desired_art = "polling_disabled", None, ""
                self.art_wakeup.set()
                return
            self._poll_locked()

    def command(self, command, value=None):
        method, path = command_request(command, value)
        with self.spotify_lock:
            if not self.polling_snapshot()["active"]:
                return "polling_disabled"
            try:
                code, _ = self.spotify.call(method, path)
                status = status_for(code)
            except SpotifyError as error:
                status = error.status
            if status == "ok":
                # Refresh the cache before replying so a local read after a tap
                # doesn't restore the state from before the command.
                self.stop.wait(0.15)
                self._poll_locked()
            return status

    def cached_art(self, path):
        with self.state_lock:
            return self.art_cache.get(path)

    def next_poll_delay(self):
        with self.state_lock:
            if not self._polling_snapshot_locked()["active"]:
                return self.display_timeout
            status = self.status
            paused = status == "ok" and self.state is not None and not self.state["is_playing"]
            presence_remaining = max(0, self.display_timeout - (time.monotonic() - self.last_display_seen))
        delay = max(self.poll_interval, {"nothing_playing": 10, "no_active_device": 10,
                    "network_error": 15, "not_authorized": 60, "unsupported": 60}.get(status, 0))
        if paused:
            delay = max(delay, 5)
        remaining = self.spotify.cooldown_remaining()
        if remaining:
            # Wake occasionally to refresh diagnostics and handle wall-clock
            # synchronization after boot, without making a Spotify request.
            delay = max(delay, min(remaining, 60))
        return min(delay, presence_remaining)

    def _poll_loop(self):
        while not self.stop.is_set():
            self.poll_wakeup.clear()
            try:
                self.poll_once()
            except (OSError, ValueError, TypeError, KeyError) as error:
                LOG.error("Playback update failed (%s)", type(error).__name__)
                with self.state_lock:
                    self.status, self.state = "network_error", None
            self.poll_wakeup.wait(self.next_poll_delay())

    def _art_loop(self):
        failed_url, failures = "", 0
        while not self.stop.is_set():
            self.art_wakeup.clear()
            with self.state_lock:
                url = self.desired_art if self._polling_snapshot_locked()["active"] else ""
                cached = art_path(url) in self.art_cache
            if not url or cached:
                self.art_wakeup.wait(1)
                continue
            if url != failed_url:
                failures = 0
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "SpotifyESP-Pi/1"})
                with urllib.request.urlopen(request, timeout=5) as response:
                    image = response.read(MAX_ART_BYTES + 1)
                if not 4 <= len(image) <= MAX_ART_BYTES or not image.startswith(b"\xff\xd8"):
                    raise ValueError("Artwork is too large or isn't a JPEG")
                with self.state_lock:
                    self.art_cache[art_path(url)] = image
                    while len(self.art_cache) > 4:
                        self.art_cache.popitem(last=False)
                failed_url, failures = "", 0
                LOG.info("Cached artwork (%d bytes)", len(image))
            except (OSError, urllib.error.URLError, ValueError) as error:
                if isinstance(error, urllib.error.HTTPError):
                    error.close()
                LOG.warning("Artwork unavailable (%s)", type(error).__name__)
                failed_url = url
                delay = (2, 5, 15, 30)[min(failures, 3)]
                failures += 1
                self.art_wakeup.wait(delay)

    def start(self):
        if self.weather:
            self.weather.start()
        for target in (self._poll_loop, self._art_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)

    def close(self):
        self.stop.set()
        self.poll_wakeup.set()
        self.art_wakeup.set()
        if self.weather:
            self.weather.close()


def make_server(address, bridge, api_token):
    if len(api_token) < 24 or not api_token.isascii():
        raise ValueError("PI_API_TOKEN must contain at least 24 ASCII characters")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            self.connection.settimeout(8)

        def reply(self, code, data, content_type="application/json"):
            body = data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            self.wfile.write(body)

        def authorized(self):
            provided = self.headers.get("Authorization", "")
            if not hmac.compare_digest(provided.encode(), ("Bearer " + api_token).encode()):
                self.reply(401, {"error": "Invalid Pi API token"})
                return False
            return True

        def do_GET(self):
            if self.path == "/health":
                self.reply(200, {"ok": True, "version": 1})
                return
            if not self.authorized():
                return
            if self.path == "/state":
                bridge.display_seen()
                self.reply(200, bridge.snapshot())
            elif self.path == "/polling":
                self.reply(200, {"version": 1, "status": "ok", "polling": bridge.polling_snapshot()})
            elif self.path.startswith("/art/"):
                image = bridge.cached_art(self.path)
                self.reply(200, image, "image/jpeg") if image else self.reply(404, {"error": "No artwork"})
            else:
                self.reply(404, {"error": "Unknown endpoint"})

        def do_POST(self):
            if not self.authorized():
                return
            if self.path not in ("/command", "/polling"):
                self.reply(404, {"error": "Unknown endpoint"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 1024 or self.headers.get("Transfer-Encoding"):
                    raise ValueError("Invalid body length")
                data = json.loads(self.rfile.read(length))
                if self.path == "/polling":
                    if not isinstance(data, dict) or not isinstance(data.get("enabled"), bool):
                        raise ValueError("Missing enabled boolean")
                    polling = bridge.set_polling(data["enabled"])
                    self.reply(200, {"version": 1, "status": "ok", "polling": polling})
                    return
                if not isinstance(data, dict) or not isinstance(data.get("command"), str):
                    raise ValueError("Missing command")
                command_request(data["command"], data.get("value"))
            except (ValueError, TypeError):
                self.reply(400, {"error": "Invalid command"})
                return
            except OSError:
                self.reply(500, {"error": "Could not save polling preference"})
                return
            try:
                status = bridge.command(data["command"], data.get("value"))
            except (OSError, ValueError, TypeError, KeyError):
                status = "network_error"
            self.reply(200, {"version": 1, "status": status})

        def log_message(self, *args):
            pass

    return ThreadingHTTPServer(address, Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", default=str(Path(__file__).with_name("spotify.env")))
    parser.add_argument("--record-cooldown-until", metavar="ISO_TIMESTAMP",
                        help="save a logged Spotify retry deadline, then exit without starting the service")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        load_env(args.env_file)
        names = ("SPOTIFY_CLIENT_ID", "SPOTIFY_CLIENT_SECRET", "SPOTIFY_REFRESH_TOKEN", "PI_API_TOKEN")
        missing = [name for name in names if not os.environ.get(name) or os.environ[name].startswith("your-")]
        if missing:
            raise ValueError("Missing configuration: " + ", ".join(missing))
        interval = float(os.environ.get("SPOTIFY_POLL_INTERVAL", str(DEFAULT_POLL_INTERVAL)))
        if not 1 <= interval <= 60:
            raise ValueError("SPOTIFY_POLL_INTERVAL must be between 1 and 60 seconds")
        client = SpotifyClient(*(os.environ[name] for name in names[:3]),
                               os.environ.get("SPOTIFY_TOKEN_FILE", str(Path(args.env_file).with_suffix(".tokens.json"))))
        if args.record_cooldown_until:
            deadline = datetime.fromisoformat(args.record_cooldown_until)
            if deadline.tzinfo is None:
                raise ValueError("Cooldown timestamp must include a timezone, e.g. -08:00")
            client.record_cooldown_until(deadline.timestamp())
            print("Saved Spotify cooldown; %d seconds remaining. Service was not started." % client.cooldown_remaining())
            return
        bridge = Bridge(client, interval, WeatherCache.from_env(), client.token_file.with_suffix(".polling.json"))
        address = (os.environ.get("PI_BIND", "0.0.0.0"), int(os.environ.get("PI_PORT", "8765")))
        server = make_server(address, bridge, os.environ["PI_API_TOKEN"])
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, "Configuration failed: " + str(error) + "\n")
    bridge.start()
    LOG.info("Listening on %s:%d", *server.server_address)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.close()
        server.server_close()


if __name__ == "__main__":
    main()
