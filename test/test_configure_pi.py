import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pi"))
from configure_pi import configure, read_define
from spotify_bridge import load_env


class ConfigureTests(unittest.TestCase):
    def test_regenerating_credentials_preserves_weather_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            header, env = Path(directory) / "secrets.h", Path(directory) / "spotify.env"
            header.write_text('#define SPOTIFY_CLIENT_ID "id"\n#define SPOTIFY_CLIENT_SECRET "secret"\n'
                              '#define SPOTIFY_REFRESH_TOKEN "refresh"\n')
            env.write_text("WEATHER_REFRESH_SECONDS=3600\nWEATHER_LOCATION='Martinez, CA'\nSPOTIFY_POLL_INTERVAL=5\n"
                           "TELEMETRY_SSD_PATH='/mnt/My SSD'\n")
            configure("http://10.0.0.248:8765", header, env)
            self.assertIn("WEATHER_REFRESH_SECONDS=3600", env.read_text())
            self.assertIn("WEATHER_LOCATION='Martinez, CA'", env.read_text())
            self.assertIn("SPOTIFY_POLL_INTERVAL=5", env.read_text())
            self.assertIn("TELEMETRY_SSD_PATH='/mnt/My SSD'", env.read_text())

    def test_credentials_wifi_and_key_survive_rerun(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "secrets.h"
            env = Path(directory) / "spotify.env"
            original = ('#pragma once\n#define WIFI_SSID "Home"\n'
                        '#define WIFI_PASSWORD "s$ecret"\n#define SPOTIFY_CLIENT_ID "id"\n'
                        '#define SPOTIFY_CLIENT_SECRET "a secret with \' and $ characters"\n'
                        '#define SPOTIFY_REFRESH_TOKEN "refresh-token"\n')
            header.write_text(original)
            configure("http://10.0.0.248:8765", header, env)
            first_header, first_env = header.read_text(), env.read_text()
            self.assertTrue(first_header.startswith(original))
            configure("http://10.0.0.248:8765", header, env)
            self.assertEqual(header.read_text(), first_header)
            self.assertEqual(env.read_text(), first_env)
            self.assertEqual(env.stat().st_mode & 0o777, 0o600)
            with patch.dict(os.environ, {}, clear=True):
                load_env(env)
                self.assertEqual(os.environ["SPOTIFY_CLIENT_SECRET"], "a secret with ' and $ characters")
                self.assertEqual(os.environ["PI_API_TOKEN"], read_define(first_header, "PI_API_TOKEN"))

    def test_invalid_address_does_not_change_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "secrets.h"
            env = Path(directory) / "spotify.env"
            header.write_text("untouched")
            for url in ("https://host", "http://user:secret@host", "http://host/path", "http://host?key=secret"):
                with self.assertRaises(ValueError):
                    configure(url, header, env)
            self.assertEqual(header.read_text(), "untouched")
            self.assertFalse(env.exists())


if __name__ == "__main__":
    unittest.main()
