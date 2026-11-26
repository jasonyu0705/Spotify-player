#pragma once

#include <Arduino.h>

struct TelemetryInfo {
  bool supported = false;
  bool available = false;
  bool stale = false;
  bool hasTemperature = false;
  bool hasCpuUsage = false;
  bool hasMemory = false;
  bool hasSsdSpace = false;
  bool ssdMounted = false;
  bool hasUptime = false;
  float temperatureC = 0;
  float cpuPercent = 0;
  uint64_t memoryUsed = 0, memoryTotal = 0;
  uint64_t ssdFree = 0, ssdTotal = 0;
  uint64_t uptimeSeconds = 0;
  uint32_t ageSeconds = 0, receivedAtMs = 0, refreshSeconds = 60;
  String status;
  String ssdStatus;
  String spotifyStatus;
  bool isPlaying = false;
};
