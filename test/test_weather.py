import io
import json
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pi"))
from weather import WeatherCache


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
                "weather.urllib.request.urlopen", side_effect=OSError("offline")) as network:
            self.weather.refresh_if_due()
            self.assertEqual(self.weather.snapshot()["temperature"], -2)
            self.assertEqual(self.weather.snapshot()["status"], "stale")
            self.assertEqual(self.weather.snapshot()["age_seconds"], 900)
            self.assertFalse(self.weather.refresh_if_due())
            self.assertEqual(network.call_count, 1)

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


if __name__ == "__main__":
    unittest.main()
