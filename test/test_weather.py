import io
import http.client
import json
from pathlib import Path
import sys
import ssl
import threading
import unittest
from unittest.mock import Mock, patch
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pi"))
from weather import WeatherCache, read_fallback, WEATHER_HOST, WEATHER_FALLBACK_IP


def response(temperature=12.5, code=2, is_day=1):
    return io.BytesIO(json.dumps({"current": {"temperature_2m": temperature,
                                             "weather_code": code, "is_day": is_day}}).encode())


class WeatherTests(unittest.TestCase):
    def setUp(self):
        self.weather = WeatherCache(38.01937, -122.13413, "Martinez, CA")

    def test_cache_reads_do_not_fetch_and_refresh_only_after_15_minutes(self):
        with patch("weather.time.monotonic", return_value=100), patch(
                "weather.urllib.request.urlopen", return_value=response()) as network:
            self.assertTrue(self.weather.refresh_if_due())
            for _ in range(20):
                snapshot = self.weather.snapshot()
                self.assertFalse(self.weather.refresh_if_due())
            self.assertEqual(snapshot["temperature"], 12.5)
            self.assertEqual(snapshot["temperature_unit"], "celsius")
            self.assertEqual(snapshot["condition"], "Partly cloudy")
            self.assertIn("temperature_unit=celsius", network.call_args.args[0].full_url)
            self.assertEqual(network.call_count, 1)
        with patch("weather.time.monotonic", return_value=999):
            self.assertFalse(self.weather.refresh_if_due())
        with patch("weather.time.monotonic", return_value=1000), patch(
                "weather.urllib.request.urlopen", return_value=response(13)) as network:
            self.assertTrue(self.weather.refresh_if_due())
            self.assertEqual(self.weather.snapshot()["temperature"], 13)
            self.assertEqual(network.call_count, 1)

    def test_hourly_setting_is_not_stale_after_30_minutes(self):
        weather = WeatherCache(38, -122, "Martinez", interval=3600)
        with patch("weather.time.monotonic", return_value=100), patch(
                "weather.urllib.request.urlopen", return_value=response()):
            weather.refresh_if_due()
        with patch("weather.time.monotonic", return_value=1900):
            self.assertFalse(weather.refresh_if_due())
            self.assertEqual(weather.snapshot()["status"], "ok")
        with patch("weather.time.monotonic", return_value=7300):
            self.assertEqual(weather.snapshot()["status"], "stale")

    def test_failed_update_preserves_weather_and_does_not_retry_rapidly(self):
        with patch("weather.time.monotonic", return_value=100), patch(
                "weather.urllib.request.urlopen", return_value=response(-2, 71, 0)):
            self.weather.refresh_if_due()
        with patch("weather.time.monotonic", return_value=1000), patch(
                "weather.urllib.request.urlopen", side_effect=OSError("offline")) as network, patch(
                "weather.read_fallback", side_effect=OSError("fallback offline")) as fallback:
            self.weather.refresh_if_due()
            self.assertEqual(self.weather.snapshot()["temperature"], -2)
            self.assertEqual(self.weather.snapshot()["status"], "stale")
            self.assertEqual(self.weather.snapshot()["age_seconds"], 900)
            self.assertFalse(self.weather.refresh_if_due())
            self.assertEqual(network.call_count, 1)
            self.assertEqual(fallback.call_count, 1)

    def test_primary_timeout_uses_one_fallback_and_preserves_refresh_schedule(self):
        error = urllib.error.URLError(TimeoutError("TLS handshake timed out"))
        with patch("weather.time.monotonic", return_value=100), patch(
                "weather.urllib.request.urlopen", side_effect=error) as primary, patch(
                "weather.read_fallback", return_value=response(15).getvalue()) as fallback:
            self.weather.refresh_if_due()
            state = self.weather.snapshot()
            self.assertEqual(state["status"], "ok")
            self.assertEqual(state["temperature"], 15)
            self.assertFalse(self.weather.refresh_if_due())
            self.assertEqual(primary.call_count, 1)
            self.assertEqual(fallback.call_count, 1)
            request = fallback.call_args.args[0]
            self.assertTrue(request.full_url.startswith("https://" + WEATHER_HOST + "/v1/forecast?"))
            self.assertIn("temperature_unit=celsius", request.full_url)
        with patch("weather.time.monotonic", return_value=999):
            self.assertFalse(self.weather.refresh_if_due())

    def test_http_rejections_never_fail_over_to_another_node(self):
        for code in (400, 403, 429, 503):
            weather = WeatherCache(38, -122, "Martinez")
            error = urllib.error.HTTPError("https://" + WEATHER_HOST, code, "Rejected", {}, None)
            with patch("weather.urllib.request.urlopen", side_effect=error), patch("weather.read_fallback") as fallback:
                weather.refresh_if_due()
            fallback.assert_not_called()
            self.assertEqual(weather.snapshot()["status"], "unavailable")

    def test_interrupted_response_falls_back_instead_of_killing_weather_worker(self):
        broken = Mock()
        broken.__enter__ = Mock(return_value=broken)
        broken.__exit__ = Mock(return_value=False)
        broken.read.side_effect = http.client.IncompleteRead(b"partial", 100)
        with patch("weather.urllib.request.urlopen", return_value=broken), patch(
                "weather.read_fallback", return_value=response(16).getvalue()):
            self.weather.refresh_if_due()
        self.assertEqual(self.weather.snapshot()["status"], "ok")
        self.assertEqual(self.weather.snapshot()["temperature"], 16)

    def test_failed_attempts_log_the_underlying_connection_reason(self):
        error = urllib.error.URLError(TimeoutError("TLS handshake timed out"))
        with patch("weather.urllib.request.urlopen", side_effect=error), patch(
                "weather.read_fallback", side_effect=error), self.assertLogs("spotify-bridge.weather", level="WARNING") as logs:
            self.weather.refresh_if_due()
        self.assertTrue(any("TLS handshake timed out" in line for line in logs.output))


    def test_invalid_data_never_becomes_a_zero_degree_forecast(self):
        for temperature in (None, True, "12", float("nan")):
            weather = WeatherCache(38, -122, "Martinez")
            with patch("weather.urllib.request.urlopen", return_value=response(temperature)):
                weather.refresh_if_due()
            self.assertEqual(weather.snapshot()["status"], "unavailable")
            self.assertNotIn("temperature", weather.snapshot())

    def test_weather_reads_are_immediate_during_slow_fetch(self):
        entered, release = threading.Event(), threading.Event()

        def slow_fetch(*args, **kwargs):
            entered.set()
            release.wait(2)
            return response()

        with patch("weather.urllib.request.urlopen", side_effect=slow_fetch):
            worker = threading.Thread(target=self.weather.refresh_if_due)
            worker.start()
            try:
                self.assertTrue(entered.wait(1))
                self.assertEqual(self.weather.snapshot()["status"], "loading")
            finally:
                release.set()
                worker.join()


class FallbackTransportTests(unittest.TestCase):
    def setUp(self):
        self.request = urllib.request.Request("https://" + WEATHER_HOST + "/v1/forecast?latitude=38&longitude=-122",
                                              headers={"User-Agent": "SpotifyESP-Pi/1"})

    def test_fallback_keeps_verified_tls_sni_and_http_host_for_original_hostname(self):
        tcp, tls = Mock(), Mock()
        body = response().getvalue()
        tls.makefile.return_value = io.BytesIO(b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode() +
                                              b"\r\nConnection: close\r\n\r\n" + body)

        def verified_wrap(context, sock, **kwargs):
            self.assertTrue(context.check_hostname)
            self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
            self.assertIs(sock, tcp)
            self.assertEqual(kwargs["server_hostname"], WEATHER_HOST)
            return tls

        with patch("weather.socket.create_connection", return_value=tcp) as connect, patch.object(
                ssl.SSLContext, "wrap_socket", autospec=True, side_effect=verified_wrap):
            self.assertEqual(read_fallback(self.request), body)
        connect.assert_called_once_with((WEATHER_FALLBACK_IP, 443), 15, None)
        sent = b"".join(call.args[0] for call in tls.sendall.call_args_list)
        self.assertIn(("Host: " + WEATHER_HOST + "\r\n").encode(), sent)
        self.assertIn(b"GET /v1/forecast?latitude=38&longitude=-122 HTTP/1.1", sent)
        tls.close.assert_called()

    def test_invalid_certificate_is_not_accepted_and_connection_is_closed(self):
        tcp = Mock()
        with patch("weather.socket.create_connection", return_value=tcp), patch.object(
                ssl.SSLContext, "wrap_socket", side_effect=ssl.SSLCertVerificationError("bad certificate")):
            with self.assertRaises(ssl.SSLCertVerificationError):
                read_fallback(self.request)
        tcp.close.assert_called()

    def test_fallback_http_error_is_reported_without_another_retry(self):
        connection = Mock()
        reply = Mock()
        reply.status, reply.reason, reply.headers = 429, "Too many requests", {}
        connection.getresponse.return_value.__enter__ = Mock(return_value=reply)
        connection.getresponse.return_value.__exit__ = Mock(return_value=False)
        with patch("weather.http.client.HTTPSConnection", return_value=connection):
            with self.assertRaises(urllib.error.HTTPError) as result:
                read_fallback(self.request)
        self.assertEqual(result.exception.code, 429)
        result.exception.close()
        connection.request.assert_called_once()
        connection.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
