#include "thingspeak.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include <stdlib_noniso.h>

#include "secrets.h"

namespace {

uint32_t lastAttemptMs = 0;

}  // namespace

void thingSpeakBegin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print(F("Connecting to WiFi"));
  const uint32_t startedAt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedAt) < THINGSPEAK_CONNECT_TIMEOUT_MS) {
    Serial.print('.');
    delay(250);
  }
  Serial.println();

  if (thingSpeakWifiConnected()) {
    Serial.print(F("WiFi connected, IP "));
    Serial.println(thingSpeakIpAddress());
    configTime(0, 0, "pool.ntp.org");
  } else {
    Serial.println(F("ERROR: WiFi did not connect. Readings will not be uploaded."));
  }
}

bool thingSpeakWifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

int32_t thingSpeakIpAddress() {
  return WiFi.localIP();
}

PublishResult thingSpeakPublish(float weightGrams, uint32_t minIntervalMs) {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastAttemptMs) < minIntervalMs) {
    return PublishResult::Skipped;
  }
  lastAttemptMs = now;

  if (!thingSpeakWifiConnected()) {
    Serial.println(F("Upload skipped: WiFi is down."));
    return PublishResult::RetryLater;
  }

  char weightText[16];
  dtostrf(weightGrams, 0, 2, weightText);

  char url[256];
  snprintf(url, sizeof(url),
           "https://api.thingspeak.com/update?api_key=%s&field1=%s",
           THINGSPEAK_CHANNEL_API_KEY, weightText);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(THINGSPEAK_REQUEST_TIMEOUT_MS / 1000);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println(F("Upload failed: could not start the HTTPS request."));
    return PublishResult::RetryLater;
  }

  http.setTimeout(THINGSPEAK_REQUEST_TIMEOUT_MS);
  http.setUserAgent("cat-feeder-loadcell/1.0");

  const int status = http.GET();
  String body = http.getString();
  http.end();

  if (status == 200) {
    Serial.print(F("Uploaded "));
    Serial.print(weightGrams, 2);
    Serial.print(F(" g to ThingSpeak ("));
    Serial.print(body);
    Serial.println(F(")"));
    return PublishResult::Uploaded;
  }

  if (status == 429) {
    Serial.println(F("Upload failed: ThingSpeak rate limit (HTTP 429)."));
    Serial.println(F("Free channels accept one update every 15 seconds."));
    return PublishResult::RetryLater;
  }

  Serial.print(F("Upload failed: HTTP "));
  Serial.print(status);
  Serial.print(F(" "));
  Serial.println(body);

  if (status >= 400 && status < 500) {
    Serial.println(F("Check the write API key in include/secrets.h and that the "));
    Serial.println(F("channel exists. Detection will carry on without uploading."));
    return PublishResult::Misconfigured;
  }

  return PublishResult::RetryLater;
}
