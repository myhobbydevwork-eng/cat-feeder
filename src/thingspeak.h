#pragma once

#include <Arduino.h>

constexpr uint32_t THINGSPEAK_DEFAULT_MIN_INTERVAL_MS = 15000;
constexpr uint32_t THINGSPEAK_REQUEST_TIMEOUT_MS = 6000;
constexpr uint32_t THINGSPEAK_CONNECT_TIMEOUT_MS = 20000;

enum class PublishResult : uint8_t {
  Uploaded,
  Skipped,
  RetryLater,
  Misconfigured,
};

void thingSpeakBegin();
bool thingSpeakWifiConnected();
int32_t thingSpeakIpAddress();

PublishResult thingSpeakPublish(float weightGrams, uint32_t minIntervalMs);
