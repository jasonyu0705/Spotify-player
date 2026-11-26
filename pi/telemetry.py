"""Read local Linux health counters once a minute; serve cached values only."""

import math
import os
from pathlib import Path
import re
import threading
import time


class TelemetryCache:
    interval = 60

    def __init__(self, ssd_path="/mnt/jasons-ssd", proc_root="/proc", sys_root="/sys"):
        self.ssd_path = Path(ssd_path)
        self.proc = Path(proc_root)
        self.sys = Path(sys_root)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.next_sample_at = 0
        self.sampled_at = None
        self.previous_cpu = None
        self.data = None
        self.failed = False

    @classmethod
    def from_env(cls):
        return cls(os.environ.get("TELEMETRY_SSD_PATH", "/mnt/jasons-ssd"))

    @staticmethod
    def optional(read):
        try:
            return read()
        except (OSError, ValueError, IndexError, KeyError, OverflowError):
            return None

    def temperature(self):
        value = float((self.sys / "class/thermal/thermal_zone0/temp").read_text()) / 1000
        return round(value, 1) if math.isfinite(value) and 0 <= value <= 150 else None

    def cpu(self):
        line = (self.proc / "stat").read_text().splitlines()[0].split()
        if line[0] != "cpu" or len(line) < 5:
            raise ValueError("Missing aggregate CPU counters")
        # guest counters are already included in user/nice; don't count twice.
        values = [int(value) for value in line[1:9]]
        if min(values) < 0:
            raise ValueError("Invalid CPU counter")
        total, idle = sum(values), values[3] + (values[4] if len(values) > 4 else 0)
        previous, self.previous_cpu = self.previous_cpu, (total, idle)
        if previous is None:
            return None  # need two samples for an interval average
        elapsed, inactive = total - previous[0], idle - previous[1]
        if elapsed <= 0 or not 0 <= inactive <= elapsed:
            return None
        return round(100 * (elapsed - inactive) / elapsed, 1)

    def memory(self):
        counters = {}
        for line in (self.proc / "meminfo").read_text().splitlines():
            key, value = line.split(":", 1)
            counters[key] = int(value.split()[0]) * 1024
        total = counters["MemTotal"]
        available = counters.get("MemAvailable")
        if available is None:
            available = sum(counters.get(key, 0) for key in ("MemFree", "Buffers", "Cached"))
        if total <= 0 or not 0 <= available <= total:
            raise ValueError("Invalid memory counters")
        return {"total_bytes": total, "used_bytes": total - available}

    def uptime(self):
        value = float((self.proc / "uptime").read_text().split()[0])
        if not math.isfinite(value) or value < 0:
            raise ValueError("Invalid uptime")
        return int(value)

    def ssd(self):
        try:
            target = str(self.ssd_path.resolve())
            mounts = (self.proc / "self/mountinfo").read_text().splitlines()
            mounted = False
            for line in mounts:
                fields = line.split()
                if len(fields) < 6:
                    continue
                mount = re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), fields[4])
                if os.path.realpath(mount) == target:
                    mounted = True
                    break
            if not mounted:
                # Don't accidentally report SD-card free space at an empty mount directory.
                return {"status": "unmounted", "mounted": False}
            info = os.statvfs(target)
            total, available = info.f_blocks * info.f_frsize, info.f_bavail * info.f_frsize
            if total <= 0 or not 0 <= available <= total:
                raise ValueError("Invalid storage counters")
            return {"status": "ok", "mounted": True, "total_bytes": total,
                    "available_bytes": available}
        except (OSError, ValueError):
            return {"status": "unavailable", "mounted": None}

    def refresh_if_due(self):
        with self.lock:
            if time.monotonic() < self.next_sample_at:
                return False
            self.next_sample_at = time.monotonic() + self.interval
        data = {"cpu_temperature_c": self.optional(self.temperature), "cpu_percent": self.optional(self.cpu),
                "memory": self.optional(self.memory), "uptime_seconds": self.optional(self.uptime),
                "ssd": self.ssd()}
        failed = all(data[key] is None for key in ("cpu_temperature_c", "cpu_percent", "memory", "uptime_seconds"))
        with self.lock:
            self.failed = failed
            if not failed:
                self.data, self.sampled_at = data, time.monotonic()
        return True

    def snapshot(self):
        with self.lock:
            if self.data is None:
                return {"status": "unavailable" if self.failed else "loading", "refresh_seconds": self.interval}
            age = max(0, int(time.monotonic() - self.sampled_at))
            return dict(self.data, age_seconds=age, refresh_seconds=self.interval,
                        status="stale" if self.failed or age >= 2 * self.interval else "ok")

    def _run(self):
        while not self.stop.is_set():
            self.refresh_if_due()
            self.stop.wait(max(1, self.next_sample_at - time.monotonic()))

    def start(self):
        threading.Thread(target=self._run, daemon=True, name="pi-telemetry").start()

    def close(self):
        self.stop.set()
