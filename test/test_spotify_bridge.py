import base64
from contextlib import contextmanager
import copy
import http.client
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pi"))
from spotify_bridge import Bridge, SpotifyClient, SpotifyError, art_path, make_server, playback_state


TRACK = {"is_playing": True, "progress_ms": 10000, "repeat_state": "context",
         "shuffle_state": True, "device": {"name": "Kitchen"},
         "item": {"id": "track-1", "name": "A song", "duration_ms": 180000,
                  "artists": [{"name": "First"}, {"name": "Second"}],
                  "album": {"name": "An album", "images": [
                      {"width": 640, "url": "https://i.scdn.co/large"},
                      {"width": 300, "url": "https://i.scdn.co/medium"}]}}}
KEY = "test-key-longer-than-twenty-four-characters"


class FakeSpotify:
    retry_at = 0
    rate_limit_reason = ""

    def cooldown_remaining(self):
        return max(0, int(self.retry_at - time.monotonic()))

    def __init__(self):
        self.body = copy.deepcopy(TRACK)
        self.calls = []
        self.error = None
        self.block = None

    def call(self, method, path):
        self.calls.append((method, path))
        if self.block:
            self.block.wait(2)
        if self.error:
            raise SpotifyError(self.error)
        if method == "GET":
            return (200, copy.deepcopy(self.body)) if self.body else (204, None)
        if path == "/pause":
            self.body["is_playing"] = False
        return 204, None


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.spotify = FakeSpotify()
        self.bridge = Bridge(self.spotify)
        self.bridge.display_seen()

    def test_playing_paused_and_idle_have_independent_poll_intervals(self):
        self.bridge.poll_once()
        self.assertEqual(self.bridge.next_poll_delay(), 1)
        self.spotify.body["is_playing"] = False
        self.bridge.poll_once()
        self.assertEqual(self.bridge.next_poll_delay(), 5)
        self.spotify.body = None
        self.bridge.poll_once()
        self.assertEqual(self.bridge.next_poll_delay(), 10)

    def test_cooldown_diagnostics_count_down_without_calling_spotify(self):
        self.spotify.error = "rate_limited"
        self.spotify.rate_limit_reason = "QUOTA_EXCEEDED"
        self.spotify.retry_at = 1000
        with patch("spotify_bridge.time.monotonic", return_value=100):
            self.bridge.poll_once()
            calls = len(self.spotify.calls)
            self.assertEqual(self.bridge.snapshot()["retry_after_seconds"], 900)
            self.assertEqual(self.bridge.next_poll_delay(), 60)
        with patch("spotify_bridge.time.monotonic", return_value=160):
            snapshot = self.bridge.snapshot()
            self.assertEqual(snapshot["retry_after_seconds"], 840)
            self.assertEqual(snapshot["rate_limit_reason"], "QUOTA_EXCEEDED")
        self.assertEqual(len(self.spotify.calls), calls)

    def test_cached_progress_advances_without_calling_spotify(self):
        with patch("spotify_bridge.time.monotonic", return_value=100):
            self.bridge.poll_once()
        self.spotify.calls.clear()
        with patch("spotify_bridge.time.monotonic", return_value=102.5):
            snapshot = self.bridge.snapshot()
        self.assertEqual(snapshot["now_playing"]["progress_ms"], 12500)
        self.assertEqual(snapshot["now_playing"]["artist"], "First, Second")
        self.assertEqual(self.spotify.calls, [])
        self.assertEqual(self.bridge.state["progress_ms"], 10000)

    def test_pause_refreshes_cache_before_command_returns(self):
        self.bridge.poll_once()
        self.assertEqual(self.bridge.command("pause"), "ok")
        snapshot = self.bridge.snapshot()
        self.assertFalse(snapshot["now_playing"]["is_playing"])
        self.assertEqual(snapshot["now_playing"]["progress_ms"], 10000)
        self.assertEqual(self.spotify.calls[-2][0], "PUT")
        self.assertEqual(self.spotify.calls[-1][0], "GET")

    def test_reads_do_not_wait_for_a_spotify_request(self):
        self.bridge.poll_once()
        self.spotify.block = threading.Event()
        worker = threading.Thread(target=self.bridge.poll_once)
        worker.start()
        try:
            started = time.monotonic()
            self.assertEqual(self.bridge.snapshot()["status"], "ok")
            self.assertLess(time.monotonic() - started, 0.1)
        finally:
            self.spotify.block.set()
            worker.join()

    def test_stale_and_failed_polls_do_not_publish_old_playback_as_current(self):
        with patch("spotify_bridge.time.monotonic", return_value=100):
            self.bridge.poll_once()
        with patch("spotify_bridge.time.monotonic", return_value=131):
            snapshot = self.bridge.snapshot()
        self.assertEqual(snapshot["status"], "network_error")
        self.assertIsNone(snapshot["now_playing"])
        self.spotify.error = "rate_limited"
        self.bridge.poll_once()
        self.assertEqual(self.bridge.snapshot()["status"], "rate_limited")
        self.assertIsNone(self.bridge.snapshot()["now_playing"])

    def test_idle_ad_and_episode(self):
        self.spotify.body = None
        self.bridge.poll_once()
        self.assertEqual(self.bridge.snapshot()["status"], "nothing_playing")
        state, url = playback_state({"currently_playing_type": "ad", "progress_ms": 100})
        self.assertEqual(state["title"], "Advertisement")
        self.assertEqual(url, "")
        state, url = playback_state({"item": {"uri": "spotify:episode:1", "name": "Episode",
                                             "show": {"name": "Podcast"}, "images": []}})
        self.assertEqual(state["artist"], "Podcast")
        self.assertEqual(state["track_id"], "spotify:episode:1")

    def test_art_is_only_advertised_when_cached(self):
        self.bridge.poll_once()
        self.assertEqual(self.bridge.snapshot()["now_playing"]["art_path"], "")
        path = art_path("https://i.scdn.co/medium")
        self.bridge.art_cache[path] = b"\xff\xd8jpeg"
        self.assertEqual(self.bridge.snapshot()["now_playing"]["art_path"], path)

    def test_weather_survives_a_spotify_error_without_network_requests(self):
        class CachedWeather:
            def snapshot(self):
                return {"status": "ok", "temperature": 14, "temperature_unit": "celsius"}

        self.bridge.weather = CachedWeather()
        self.spotify.error = "unsupported"
        self.bridge.poll_once()
        calls = len(self.spotify.calls)
        snapshot = self.bridge.snapshot()
        self.assertEqual(snapshot["status"], "unsupported")
        self.assertEqual(snapshot["weather"]["temperature"], 14)
        self.assertIsNone(snapshot["now_playing"])
        self.assertEqual(len(self.spotify.calls), calls)


class PresenceTests(unittest.TestCase):
    def setUp(self):
        self.clock = patch("spotify_bridge.time.monotonic", return_value=100)
        self.now = self.clock.start()
        self.addCleanup(self.clock.stop)
        self.spotify = FakeSpotify()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.preference = Path(self.temp.name) / "spotify.tokens.polling.json"
        self.bridge = Bridge(self.spotify, polling_file=self.preference)

    def test_startup_waits_for_display_and_expiry_stops_requests_and_stale_playback(self):
        self.bridge.poll_once()
        self.assertEqual(self.spotify.calls, [])
        self.assertEqual(self.bridge.polling_snapshot()["reason"], "display_offline")
        self.bridge.display_seen()
        self.bridge.poll_once()
        self.assertEqual(self.bridge.snapshot()["status"], "ok")
        self.now.return_value = 159
        self.assertTrue(self.bridge.polling_snapshot()["active"])
        self.now.return_value = 160
        calls = len(self.spotify.calls)
        self.bridge.poll_once()
        self.assertEqual(len(self.spotify.calls), calls)
        self.assertIsNone(self.bridge.snapshot()["now_playing"])
        self.assertEqual(self.bridge.snapshot()["status"], "polling_disabled")
        self.assertEqual(self.bridge.command("next"), "polling_disabled")
        self.assertEqual(len(self.spotify.calls), calls)

    def test_only_returning_display_wakes_polling_not_each_cache_read(self):
        self.bridge.display_seen()
        self.assertTrue(self.bridge.poll_wakeup.is_set())
        self.bridge.poll_wakeup.clear()
        self.now.return_value = 130
        self.bridge.display_seen()
        self.assertFalse(self.bridge.poll_wakeup.is_set())
        self.now.return_value = 190
        self.bridge.display_seen()
        self.assertTrue(self.bridge.poll_wakeup.is_set())
        self.assertTrue(self.bridge.polling_snapshot()["active"])
        self.assertEqual(self.spotify.calls, [])

    def test_background_poller_wakes_on_return_and_stops_when_presence_expires(self):
        completed = threading.Event()
        poll_once = self.bridge.poll_once

        def observed_poll():
            poll_once()
            completed.set()

        self.bridge.poll_once = observed_poll
        thread = threading.Thread(target=self.bridge._poll_loop, daemon=True)
        thread.start()
        try:
            self.assertTrue(completed.wait(1))
            self.assertEqual(self.spotify.calls, [])
            completed.clear()
            self.bridge.display_seen()
            self.assertTrue(completed.wait(1))
            self.assertEqual(len(self.spotify.calls), 1)
            completed.clear()
            self.now.return_value = 160
            self.bridge.poll_wakeup.set()  # reach the presence deadline without a real minute's wait
            self.assertTrue(completed.wait(1))
            self.assertEqual(len(self.spotify.calls), 1)
            completed.clear()
            self.bridge.display_seen()
            self.assertTrue(completed.wait(1))
            self.assertEqual(len(self.spotify.calls), 2)
        finally:
            self.bridge.close()
            thread.join(1)
        self.assertFalse(thread.is_alive())

    def test_manual_off_survives_restart_and_display_return_until_enabled(self):
        self.bridge.display_seen()
        self.bridge.poll_once()
        self.bridge.set_polling(False)
        self.assertEqual(self.preference.stat().st_mode & 0o777, 0o600)
        restarted = Bridge(self.spotify, polling_file=self.preference)
        restarted.display_seen()
        calls = len(self.spotify.calls)
        restarted.poll_once()
        self.assertFalse(restarted.polling_snapshot()["active"])
        self.assertEqual(restarted.polling_snapshot()["reason"], "manual_off")
        self.assertIsNone(restarted.snapshot()["now_playing"])
        self.assertEqual(len(self.spotify.calls), calls)
        restarted.set_polling(True)
        restarted.poll_once()
        self.assertEqual(restarted.snapshot()["status"], "ok")
        self.assertEqual(len(self.spotify.calls), calls + 1)

    def test_enabling_without_display_does_not_start_polling(self):
        self.bridge.set_polling(False)
        self.bridge.set_polling(True)
        self.bridge.poll_once()
        self.assertEqual(self.spotify.calls, [])
        self.assertEqual(self.bridge.polling_snapshot()["reason"], "display_offline")

    def test_disabling_does_not_pause_music_or_reset_a_spotify_cooldown(self):
        self.spotify.retry_at = 1000
        self.spotify.rate_limit_reason = "RATE_LIMITED"
        self.bridge.display_seen()
        self.bridge.set_polling(False)
        self.assertEqual(self.spotify.calls, [])
        self.assertEqual(self.bridge.snapshot()["retry_after_seconds"], 900)
        self.bridge.set_polling(True)
        self.assertEqual(self.spotify.retry_at, 1000)
        self.assertEqual(self.spotify.calls, [])

    def test_weather_is_available_when_display_absent_or_manually_disabled(self):
        class CachedWeather:
            def snapshot(self):
                return {"status": "ok", "temperature": 14}

        self.bridge.weather = CachedWeather()
        for enabled in (True, False):
            self.bridge.set_polling(enabled)
            self.bridge.poll_once()
            self.assertEqual(self.bridge.snapshot()["weather"]["temperature"], 14)
        self.assertEqual(self.spotify.calls, [])

    def test_failed_preference_save_leaves_existing_setting_unchanged(self):
        with patch("spotify_bridge.write_private_json", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.bridge.set_polling(False)
        self.assertTrue(self.bridge.polling_enabled)

    def test_unreadable_preference_defaults_to_no_requests(self):
        self.preference.write_text("[]")
        restarted = Bridge(self.spotify, polling_file=self.preference)
        restarted.display_seen()
        restarted.poll_once()
        self.assertFalse(restarted.polling_enabled)
        self.assertEqual(self.spotify.calls, [])


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.spotify = FakeSpotify()
        self.bridge = Bridge(self.spotify)
        self.bridge.display_seen()
        self.bridge.poll_once()
        self.server = make_server(("127.0.0.1", 0), self.bridge, KEY)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, method, path, body=None, key=KEY):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        headers = {"Authorization": "Bearer " + key} if key else {}
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            data = response.read()
            self.assertEqual(len(data), int(response.getheader("Content-Length")))
            return response.status, data
        finally:
            connection.close()

    def test_authenticated_cache_reads_and_art_never_call_spotify(self):
        calls = len(self.spotify.calls)
        self.assertEqual(self.request("GET", "/health", key=None)[0], 200)
        self.assertEqual(self.request("GET", "/state", key=None)[0], 401)
        code, data = self.request("GET", "/state")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(data)["status"], "ok")
        self.assertEqual(self.request("GET", "/art/missing.jpg")[0], 404)
        path = art_path("https://i.scdn.co/medium")
        self.bridge.art_cache[path] = b"\xff\xd8test-image"
        self.assertEqual(self.request("GET", path)[1], b"\xff\xd8test-image")
        self.assertEqual(len(self.spotify.calls), calls)

    def test_command_validation_and_execution(self):
        self.assertEqual(self.request("POST", "/command", '{"command":"pause"}', key="wrong")[0], 401)
        for body in ('[]', '{"command":"anything"}', '{"command":"shuffle","value":"true"}',
                     '{"command":"repeat","value":"all"}', '{"command":[]}'):
            self.assertEqual(self.request("POST", "/command", body)[0], 400)
        code, body = self.request("POST", "/command", '{"command":"pause"}')
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["status"], "ok")
        self.assertFalse(self.bridge.snapshot()["now_playing"]["is_playing"])

    def test_authenticated_state_includes_weather_when_spotify_is_unsupported(self):
        class CachedWeather:
            def snapshot(self):
                return {"status": "ok", "temperature": 14.5, "temperature_unit": "celsius"}

        self.bridge.weather = CachedWeather()
        self.spotify.error = "unsupported"
        self.bridge.poll_once()
        calls = len(self.spotify.calls)
        code, body = self.request("GET", "/state")
        self.assertEqual(code, 200)
        state = json.loads(body)
        self.assertEqual(state["status"], "unsupported")
        self.assertEqual(state["weather"]["temperature"], 14.5)
        self.assertEqual(len(self.spotify.calls), calls)

    def test_only_authenticated_state_reads_renew_display_presence(self):
        self.bridge.last_display_seen = None
        for path, key in (("/health", None), ("/state", None), ("/polling", KEY), ("/art/missing.jpg", KEY)):
            self.request("GET", path, key=key)
            self.assertFalse(self.bridge.polling_snapshot()["display_online"])
        self.request("GET", "/state")
        self.assertTrue(self.bridge.polling_snapshot()["display_online"])

    def test_cached_telemetry_survives_disabled_spotify_and_never_calls_it(self):
        class CachedTelemetry:
            def snapshot(self):
                return {"status": "ok", "cpu_temperature_c": 45.6, "refresh_seconds": 60}

        self.bridge.telemetry = CachedTelemetry()
        self.bridge.set_polling(False)
        calls = len(self.spotify.calls)
        code, body = self.request("GET", "/state")
        self.assertEqual(code, 200)
        state = json.loads(body)
        self.assertEqual(state["status"], "polling_disabled")
        self.assertEqual(state["telemetry"]["cpu_temperature_c"], 45.6)
        self.assertEqual(len(self.spotify.calls), calls)

    def test_polling_controls_require_auth_validate_boolean_and_do_not_call_spotify(self):
        calls = len(self.spotify.calls)
        self.assertEqual(self.request("POST", "/polling", '{"enabled":false}', key="wrong")[0], 401)
        self.assertTrue(self.bridge.polling_enabled)
        for body in ('[]', '{}', '{"enabled":"false"}', '{"enabled":0}'):
            self.assertEqual(self.request("POST", "/polling", body)[0], 400)
        code, body = self.request("POST", "/polling", '{"enabled":false}')
        self.assertEqual(code, 200)
        self.assertFalse(json.loads(body)["polling"]["enabled"])
        self.assertEqual(json.loads(self.request("GET", "/state")[1])["status"], "polling_disabled")
        self.assertEqual(json.loads(self.request("POST", "/command", '{"command":"play"}')[1])["status"],
                         "polling_disabled")
        self.assertEqual(len(self.spotify.calls), calls)

    def test_terminal_controls_change_the_running_service_without_renewing_presence(self):
        self.bridge.last_display_seen = None
        calls = len(self.spotify.calls)
        script = str(Path(__file__).resolve().parents[1] / "pi/polling.py")
        with tempfile.TemporaryDirectory() as directory:
            env_file = Path(directory) / "spotify.env"
            env_file.write_text("PI_API_TOKEN=%s\nPI_PORT=%d\n" % (KEY, self.server.server_port))
            environment = {key: value for key, value in os.environ.items() if key not in ("PI_API_TOKEN", "PI_PORT")}
            for action, expected in (("off", "OFF"), ("on", "ON"), ("status", "ON")):
                result = subprocess.run([sys.executable, script, action, "--env-file", str(env_file)],
                                        env=environment, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Spotify updates: " + expected, result.stdout)
        self.assertFalse(self.bridge.polling_snapshot()["display_online"])
        self.assertEqual(len(self.spotify.calls), calls)


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.token_file = Path(self.temp.name) / "tokens.json"
        self.client = SpotifyClient("id", "secret", "original-token", self.token_file)

    def test_long_retry_after_survives_restart_and_cannot_be_shortened_by_migration(self):
        with patch("spotify_bridge.time.monotonic", return_value=100), patch(
                "spotify_bridge.time.time", return_value=1000):
            self.client._rate_limited({"Retry-After": "17354"}, b"")
            self.assertEqual(self.client.cooldown_remaining(), 17354)
        with patch("spotify_bridge.time.monotonic", return_value=500), patch(
                "spotify_bridge.time.time", return_value=1100), patch(
                "spotify_bridge.urllib.request.urlopen") as network:
            restarted = SpotifyClient("id", "secret", "original-token", self.token_file)
            self.assertEqual(restarted.cooldown_remaining(), 17254)
            restarted.record_cooldown_until(1500)
            self.assertEqual(restarted.cooldown_remaining(), 17254)
            with self.assertRaises(SpotifyError) as caught:
                restarted.call("GET", "")
            self.assertEqual(caught.exception.status, "rate_limited")
            network.assert_not_called()
        self.assertEqual(self.client.cooldown_file.stat().st_mode & 0o777, 0o600)

    def test_missing_retry_header_uses_increasing_backoff(self):
        with patch("spotify_bridge.time.monotonic", return_value=100):
            self.client._rate_limited({}, b"")
            self.assertEqual(self.client.cooldown_remaining(), 60)
        with patch("spotify_bridge.time.monotonic", return_value=161):
            self.client._rate_limited({"Retry-After": "invalid"}, b"")
            self.assertEqual(self.client.cooldown_remaining(), 120)

    def test_retry_header_is_honored_even_if_error_body_cannot_be_read(self):
        class BrokenBody(io.BytesIO):
            def read(self, *args):
                raise OSError("connection closed")

        rate_error = urllib.error.HTTPError("https://api.spotify.com", 429, "limited",
                                           {"Retry-After": "120"}, BrokenBody())
        with patch("spotify_bridge.time.monotonic", return_value=100), patch(
                "spotify_bridge.urllib.request.urlopen", side_effect=rate_error):
            self.assertEqual(self.client._request("GET", "https://api.spotify.com/v1/me/player", "Bearer test")[0], 429)
            self.assertEqual(self.client.cooldown_remaining(), 120)

    def test_quota_error_without_retry_header_uses_longer_backoff(self):
        with patch("spotify_bridge.time.monotonic", return_value=100):
            self.client._rate_limited({}, b'{"error":{"reason":"QUOTA_EXCEEDED"}}')
            self.assertEqual(self.client.cooldown_remaining(), 900)
        with patch("spotify_bridge.time.monotonic", return_value=1001):
            self.client._rate_limited(None, b"")
            self.assertEqual(self.client.cooldown_remaining(), 1800)
            self.assertEqual(self.client.rate_limit_reason, "QUOTA_EXCEEDED")

    def test_successful_api_response_clears_expired_cooldown(self):
        class Reply(io.BytesIO):
            status = 204

        with patch("spotify_bridge.time.monotonic", return_value=100), patch(
                "spotify_bridge.time.time", return_value=1000):
            self.client.record_cooldown_until(1010)
        self.client.access_token, self.client.expires_at = "valid", 10000
        with patch("spotify_bridge.time.monotonic", return_value=111), patch(
                "spotify_bridge.urllib.request.urlopen", return_value=Reply(b"")):
            self.assertEqual(self.client.call("GET", "")[0], 204)
            self.assertEqual(self.client.cooldown_remaining(), 0)
            self.assertEqual(self.client.rate_limit_failures, 0)
        self.assertFalse(self.client.cooldown_file.exists())

    def test_corrupt_cooldown_file_does_not_trigger_an_immediate_retry(self):
        self.client.cooldown_file.write_text("invalid JSON")
        with patch("spotify_bridge.time.time", return_value=1000), patch(
                "spotify_bridge.urllib.request.urlopen") as network:
            restarted = SpotifyClient("id", "secret", "original-token", self.token_file)
            self.assertEqual(restarted.cooldown_remaining(), 900)
            with self.assertRaises(SpotifyError):
                restarted.call("PUT", "/play")
            network.assert_not_called()

    def test_refresh_rotation_survives_restart_and_new_seed_overrides_it(self):
        with patch.object(self.client, "_request", side_effect=[
                (200, {"access_token": "access", "expires_in": 3600, "refresh_token": "rotated"}),
                (204, None)]):
            self.client.call("GET", "")
        restarted = SpotifyClient("id", "secret", "original-token", self.token_file)
        self.assertEqual(restarted.refresh_token, "rotated")
        self.assertEqual(self.token_file.stat().st_mode & 0o777, 0o600)
        replaced = SpotifyClient("id", "secret", "new-login", self.token_file)
        self.assertEqual(replaced.refresh_token, "new-login")

    def test_rate_limit_blocks_polls_commands_and_refresh_until_retry_after(self):
        self.client.access_token = "valid"
        self.client.expires_at = 1000
        rate_error = urllib.error.HTTPError("https://api.spotify.com", 429, "limited",
                                           {"Retry-After": "25"}, io.BytesIO(b""))
        with patch("spotify_bridge.time.monotonic", return_value=100), patch(
                "spotify_bridge.urllib.request.urlopen", side_effect=rate_error) as network:
            self.assertEqual(self.client.call("GET", "")[0], 429)
            for method, path in (("GET", ""), ("PUT", "/pause")):
                with self.assertRaises(SpotifyError) as caught:
                    self.client.call(method, path)
                self.assertEqual(caught.exception.status, "rate_limited")
            self.assertEqual(network.call_count, 1)
        with patch("spotify_bridge.time.monotonic", return_value=126), patch.object(
                self.client, "_request", return_value=(204, None)):
            self.assertEqual(self.client.call("PUT", "/pause")[0], 204)

    def test_unauthorized_token_refreshes_once_and_retries(self):
        self.client.access_token = "expired-early"
        self.client.expires_at = time.monotonic() + 1000
        with patch.object(self.client, "_request", side_effect=[
                (401, None), (200, {"access_token": "fresh"}), (204, None)]) as network:
            self.assertEqual(self.client.call("PUT", "/play")[0], 204)
        self.assertEqual(network.call_count, 3)

    def test_revoked_refresh_token_backs_off_without_exposing_credentials(self):
        with patch.object(self.client, "_request", return_value=(400, None)) as network:
            for _ in range(2):
                with self.assertRaises(SpotifyError) as caught:
                    self.client.call("GET", "")
                self.assertEqual(str(caught.exception), "not_authorized")
        self.assertEqual(network.call_count, 1)


class CooldownMigrationTests(unittest.TestCase):
    def test_cli_records_deadline_without_starting_the_http_server(self):
        with tempfile.TemporaryDirectory() as directory:
            env_file = Path(directory) / "spotify.env"
            env_file.write_text("SPOTIFY_CLIENT_ID=id\nSPOTIFY_CLIENT_SECRET=secret\n"
                                "SPOTIFY_REFRESH_TOKEN=refresh\nPI_API_TOKEN=" + KEY + "\n")
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(("SPOTIFY_", "PI_", "WEATHER_"))}
            script = Path(__file__).resolve().parents[1] / "pi/spotify_bridge.py"
            result = subprocess.run([sys.executable, str(script), "--env-file", str(env_file),
                                     "--record-cooldown-until", "2099-01-01T00:00:00+00:00"],
                                    env=environment, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Saved Spotify cooldown", result.stdout)
            self.assertTrue(env_file.with_suffix(".tokens.ratelimit.json").exists())


if __name__ == "__main__":
    unittest.main()
