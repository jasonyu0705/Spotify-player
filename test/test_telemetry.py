import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pi"))
from telemetry import TelemetryCache


class TelemetryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.proc, self.sys = self.root / "proc", self.root / "sys"
        self.write("proc/stat", "cpu 100 0 50 850 0 0 0 0 0 0\n")
        self.write("proc/meminfo", "MemTotal: 1024000 kB\nMemAvailable: 512000 kB\n")
        self.write("proc/uptime", "90061.50 100.00\n")
        self.write("proc/self/mountinfo", "1 0 0:1 / / rw - ext4 /dev/mmcblk0p2 rw\n")
        self.write("sys/class/thermal/thermal_zone0/temp", "45600\n")
        self.cache = TelemetryCache(self.root / "ssd", self.proc, self.sys)
        self.clock = patch("telemetry.time.monotonic", return_value=100)
        self.now = self.clock.start()
        self.addCleanup(self.clock.stop)

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)

    def test_cached_readings_only_sample_once_per_minute_and_cpu_is_interval_average(self):
        self.assertEqual(self.cache.snapshot()["status"], "loading")
        with patch.object(self.cache, "temperature", wraps=self.cache.temperature) as sensor:
            self.assertTrue(self.cache.refresh_if_due())
            state = self.cache.snapshot()
            self.assertEqual(state["cpu_temperature_c"], 45.6)
            self.assertIsNone(state["cpu_percent"])
            self.assertEqual(state["memory"]["used_bytes"], 512000 * 1024)
            self.assertEqual(state["uptime_seconds"], 90061)
            self.write("sys/class/thermal/thermal_zone0/temp", "47000\n")
            self.write("proc/stat", "cpu 150 0 100 1750 0 0 0 0 0 0\n")
            self.now.return_value = 159
            for _ in range(10):
                self.assertFalse(self.cache.refresh_if_due())
                self.assertEqual(self.cache.snapshot()["cpu_temperature_c"], 45.6)
            self.assertEqual(sensor.call_count, 1)
            self.assertEqual(self.cache.snapshot()["age_seconds"], 59)
            self.now.return_value = 160
            self.assertTrue(self.cache.refresh_if_due())
            self.assertEqual(self.cache.snapshot()["cpu_percent"], 10)
            self.assertEqual(self.cache.snapshot()["cpu_temperature_c"], 47)
            self.assertEqual(sensor.call_count, 2)

    def test_unmounted_ssd_does_not_report_the_root_filesystem_space(self):
        with patch("telemetry.os.statvfs") as disk:
            self.cache.refresh_if_due()
        disk.assert_not_called()
        self.assertEqual(self.cache.snapshot()["ssd"], {"status": "unmounted", "mounted": False})

    def test_mounted_ssd_with_escaped_space_reports_its_available_space(self):
        target = self.root / "SSD mount"
        self.cache.ssd_path = target
        escaped = str(target).replace(" ", "\\040")
        self.write("proc/self/mountinfo", "2 1 8:1 / %s rw - exfat /dev/sda1 rw\n" % escaped)
        with patch("telemetry.os.statvfs", return_value=SimpleNamespace(f_blocks=1000000, f_bavail=250000, f_frsize=4096)) as disk:
            self.cache.refresh_if_due()
        disk.assert_called_once_with(str(target.resolve()))
        state = self.cache.snapshot()["ssd"]
        self.assertTrue(state["mounted"])
        self.assertEqual(state["total_bytes"], 4096000000)
        self.assertEqual(state["available_bytes"], 1024000000)

    def test_missing_sensor_does_not_hide_other_readings(self):
        (self.sys / "class/thermal/thermal_zone0/temp").unlink()
        self.cache.refresh_if_due()
        state = self.cache.snapshot()
        self.assertEqual(state["status"], "ok")
        self.assertIsNone(state["cpu_temperature_c"])
        self.assertIsNotNone(state["memory"])

    def test_complete_collection_failure_retains_previous_values_as_stale(self):
        self.cache.refresh_if_due()
        self.cache.proc = self.root / "missing-proc"
        self.cache.sys = self.root / "missing-sys"
        self.now.return_value = 160
        self.cache.refresh_if_due()
        state = self.cache.snapshot()
        self.assertEqual(state["status"], "stale")
        self.assertEqual(state["age_seconds"], 60)
        self.assertEqual(state["cpu_temperature_c"], 45.6)

    def test_missing_linux_counters_report_unavailable_and_allow_later_recovery(self):
        self.cache.proc = self.root / "missing-proc"
        self.cache.sys = self.root / "missing-sys"
        self.cache.refresh_if_due()
        self.assertEqual(self.cache.snapshot()["status"], "unavailable")
        self.cache.proc, self.cache.sys = self.proc, self.sys
        self.now.return_value = 160
        self.cache.refresh_if_due()
        self.assertEqual(self.cache.snapshot()["status"], "ok")

    def test_invalid_or_nonfinite_readings_do_not_escape_into_json(self):
        self.write("sys/class/thermal/thermal_zone0/temp", "nan\n")
        self.write("proc/uptime", "inf\n")
        self.write("proc/meminfo", "MemTotal: 1000 kB\nMemAvailable: 2000 kB\n")
        self.cache.refresh_if_due()
        self.assertEqual(self.cache.snapshot()["status"], "unavailable")
        json.dumps(self.cache.snapshot(), allow_nan=False)

    def test_memory_available_fallback_and_cpu_counter_reset(self):
        self.write("proc/meminfo", "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 150 kB\n")
        self.cache.refresh_if_due()
        self.assertEqual(self.cache.snapshot()["memory"]["used_bytes"], 700 * 1024)
        self.write("proc/stat", "cpu 1 0 1 8 0 0 0 0\n")
        self.now.return_value = 160
        self.cache.refresh_if_due()
        self.assertIsNone(self.cache.snapshot()["cpu_percent"])

    def test_missing_mount_information_is_unavailable_not_unmounted(self):
        (self.proc / "self/mountinfo").unlink()
        self.cache.refresh_if_due()
        self.assertEqual(self.cache.snapshot()["ssd"]["status"], "unavailable")

    def test_old_samples_are_marked_stale_without_new_collection(self):
        self.cache.refresh_if_due()
        self.now.return_value = 220
        self.assertEqual(self.cache.snapshot()["status"], "stale")


if __name__ == "__main__":
    unittest.main()
