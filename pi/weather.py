"""Small Open-Meteo cache. Only its background thread makes weather requests."""

import http.client
import json
import logging
import math
import os
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

LOG = logging.getLogger("spotify-bridge.weather")
WEATHER_HOST = "api.open-meteo.com"
# Alternate free-API node documented in Open-Meteo issue #2329. Only the
# socket destination changes: HTTP Host, TLS SNI, and certificate checks retain
# api.open-meteo.com. No process-wide DNS or Spotify connections are modified.
WEATHER_FALLBACK_IP = "167.114.211.86"
REQUEST_TIMEOUT = 15


def error_detail(error):
    reason = error.reason if isinstance(error, urllib.error.URLError) else error
    return str(reason).replace("\n", " ").replace("\r", " ")[:180]


def fallback_socket(address, timeout=REQUEST_TIMEOUT, source_address=None):
    if address != (WEATHER_HOST, 443):
        raise ValueError("Unexpected weather fallback destination")
    return socket.create_connection((WEATHER_FALLBACK_IP, 443), timeout, source_address)


def read_fallback(request):
    connection = http.client.HTTPSConnection(WEATHER_HOST, timeout=REQUEST_TIMEOUT)
    # HTTPSConnection.connect() still wraps this socket with its default verified
    # TLS context and the original hostname. This hook affects this instance only.
    connection._create_connection = fallback_socket
    try:
        connection.request("GET", request.selector, headers=dict(request.header_items()))
        with connection.getresponse() as response:
            if response.status != 200:
                raise urllib.error.HTTPError(request.full_url, response.status, response.reason, response.headers, None)
            return response.read(32769)
    finally:
        connection.close()


def read_weather(request):
    try:
        with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT) as response:
            return response.read(32769)
    except urllib.error.HTTPError:
        # Don't retry API rejections (especially 429) against another node.
        raise
    except (OSError, http.client.HTTPException) as error:
        LOG.warning("Weather primary connection failed (%s: %s); trying fallback server",
                    type(error).__name__, error_detail(error))
    raw = read_fallback(request)
    LOG.info("Weather fallback server succeeded (%s)", WEATHER_FALLBACK_IP)
    return raw


def condition_name(code, is_day=True):
    if code == 0:
        return "Clear" if is_day else "Clear night"
    return {1: "Mostly clear", 2: "Partly cloudy", 3: "Overcast", 45: "Fog", 48: "Freezing fog",
            51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle", 56: "Freezing drizzle",
            57: "Freezing drizzle", 61: "Light rain", 63: "Rain", 65: "Heavy rain",
            66: "Freezing rain", 67: "Freezing rain", 71: "Light snow", 73: "Snow",
            75: "Heavy snow", 77: "Snow grains", 80: "Rain showers", 81: "Rain showers",
            82: "Heavy showers", 85: "Snow showers", 86: "Snow showers",
            95: "Thunderstorm", 96: "Thunder / hail", 99: "Thunder / hail"}.get(code, "Weather")


class WeatherCache:
    def __init__(self, latitude, longitude, location, unit="celsius", interval=900):
        if not math.isfinite(latitude) or not -90 <= latitude <= 90:
            raise ValueError("WEATHER_LATITUDE must be between -90 and 90")
        if not math.isfinite(longitude) or not -180 <= longitude <= 180:
            raise ValueError("WEATHER_LONGITUDE must be between -180 and 180")
        if unit not in ("celsius", "fahrenheit"):
            raise ValueError("WEATHER_UNIT must be celsius or fahrenheit")
        if not 900 <= interval <= 86400:
            raise ValueError("WEATHER_REFRESH_SECONDS must be between 900 and 86400")
        self.latitude, self.longitude = latitude, longitude
        self.location, self.unit, self.interval = location[:48], unit, interval
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.next_fetch_at = 0
        self.fetched_at = None
        self.data = None
        self.failed = False
        self.thread = None

    @classmethod
    def from_env(cls):
        # Martinez, CA 94553, as requested. Override these for another location.
        return cls(float(os.environ.get("WEATHER_LATITUDE", "38.01937")),
                   float(os.environ.get("WEATHER_LONGITUDE", "-122.13413")),
                   os.environ.get("WEATHER_LOCATION", "Martinez, CA"),
                   os.environ.get("WEATHER_UNIT", "celsius"),
                   int(os.environ.get("WEATHER_REFRESH_SECONDS", "900")))

    def snapshot(self):
        with self.lock:
            if self.data is None:
                return {"status": "unavailable" if self.failed else "loading",
                        "location": self.location, "temperature_unit": self.unit}
            age = max(0, int(time.monotonic() - self.fetched_at))
            return dict(self.data, location=self.location, temperature_unit=self.unit,
                        age_seconds=age,
                        status="stale" if self.failed or age >= 2 * self.interval else "ok")

    def refresh_if_due(self):
        with self.lock:
            if time.monotonic() < self.next_fetch_at:
                return False
            # Reserve the next slot before I/O; errors do not cause rapid retries.
            self.next_fetch_at = time.monotonic() + self.interval
        query = urllib.parse.urlencode({"latitude": self.latitude, "longitude": self.longitude,
                                       "current": "temperature_2m,weather_code,is_day",
                                       "temperature_unit": self.unit, "forecast_days": 1})
        request = urllib.request.Request("https://" + WEATHER_HOST + "/v1/forecast?" + query,
                                         headers={"User-Agent": "SpotifyESP-Pi/1"})
        try:
            raw = read_weather(request)
            if len(raw) > 32768:
                raise ValueError("Weather response too large")
            current = json.loads(raw)["current"]
            temperature, code = current["temperature_2m"], current["weather_code"]
            if (isinstance(temperature, bool) or not isinstance(temperature, (int, float))
                    or not math.isfinite(temperature) or not -200 <= temperature <= 200
                    or isinstance(code, bool) or not isinstance(code, int) or not 0 <= code <= 99
                    or current.get("is_day") not in (0, 1)):
                raise ValueError("Invalid current weather")
            is_day = bool(current["is_day"])
            data = {"temperature": temperature, "weather_code": code, "is_day": is_day,
                    "condition": condition_name(code, is_day)}
            with self.lock:
                self.data, self.fetched_at, self.failed = data, time.monotonic(), False
            LOG.info("Weather updated for %s (next request in %d minutes)", self.location, self.interval // 60)
        except (OSError, http.client.HTTPException, ValueError, KeyError, TypeError) as error:
            if isinstance(error, urllib.error.HTTPError):
                error.close()
            with self.lock:
                self.failed = True
            LOG.warning("Weather update failed (%s: %s); keeping cache, retry in %d minutes",
                        type(error).__name__, error_detail(error), self.interval // 60)
        return True

    def _run(self):
        while not self.stop.is_set():
            self.refresh_if_due()
            self.stop.wait(max(1, self.next_fetch_at - time.monotonic()))

    def start(self):
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
