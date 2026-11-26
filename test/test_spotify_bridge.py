import base64
from contextlib import contextmanager
import copy
import http.client
import io
import json
from pathlib import Path
import sys
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


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.spotify = FakeSpotify()
        self.bridge = Bridge(self.spotify)
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


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.token_file = Path(self.temp.name) / "tokens.json"
        self.client = SpotifyClient("id", "secret", "original-token", self.token_file)

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


if __name__ == "__main__":
    unittest.main()
