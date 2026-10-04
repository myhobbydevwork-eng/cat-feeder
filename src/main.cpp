#include <Arduino.h>
#include <EEPROM.h>
#include <HX711.h>

#include <cstddef>

#include "change_detector.h"
#include "secrets.h"
#include "thingspeak.h"

constexpr uint8_t LOADCELL_DATA_PIN = 13;
constexpr uint8_t LOADCELL_CLOCK_PIN = 12;
constexpr byte HX711_CHANNEL_GAIN = 128;

constexpr float REFERENCE_UNIT_COUNT = 424.5f;
constexpr float REFERENCE_UNIT_GRAMS = 1.0f;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SERIAL_STARTUP_GRACE_MS = 1500;
constexpr uint32_t SAMPLE_INTERVAL_MS = 500;
constexpr byte SAMPLE_AVERAGE_COUNT = 5;
constexpr byte TARE_AVERAGE_COUNT = 25;
constexpr uint32_t AMPLIFIER_SETTLE_MS = 1500;
constexpr uint32_t READY_TIMEOUT_MS = 2000;
constexpr uint32_t POLL_INTERVAL_MS = 10;
constexpr size_t SERIAL_ENTRY_MAX_LENGTH = 16;

constexpr float DEFAULT_CHANGE_THRESHOLD_GRAMS = 1.0f;
constexpr float STABILITY_BAND_GRAMS = 0.4f;
constexpr uint32_t CONFIRM_WINDOW_MS = 10000;
constexpr uint8_t DEBOUNCE_TICKS = 3;
constexpr float MIN_CHANGE_THRESHOLD_GRAMS = STABILITY_BAND_GRAMS * 2.0f;
constexpr uint32_t THINGSPEAK_MIN_INTERVAL_MS = 15000;

constexpr size_t CALIBRATION_EEPROM_ADDRESS = 0;
constexpr size_t SETTINGS_EEPROM_ADDRESS = 16;
constexpr uint16_t RECORD_MAGIC = 0xA55A;
constexpr uint8_t CALIBRATION_VERSION = 1;
constexpr uint8_t SETTINGS_VERSION = 1;

struct CalibrationRecord {
  uint16_t magic;
  uint8_t version;
  uint8_t channelGain;
  int32_t offset;
  float scale;
  uint32_t checksum;
};

struct SettingsRecord {
  uint16_t magic;
  uint8_t version;
  uint8_t reserved;
  float changeThresholdGrams;
  uint32_t checksum;
};

static_assert(sizeof(CalibrationRecord) == 16,
              "CalibrationRecord must stay tightly packed for EEPROM storage");
static_assert(sizeof(SettingsRecord) == 12,
              "SettingsRecord must stay tightly packed for EEPROM storage");

constexpr size_t EEPROM_SIZE = SETTINGS_EEPROM_ADDRESS + sizeof(SettingsRecord);

HX711 scale;
ChangeDetector detector;

uint32_t checksumOf(const void *record, size_t bytes) {
  const uint8_t *data = static_cast<const uint8_t *>(record);
  uint32_t checksum = 0x1D;

  for (size_t i = 0; i < bytes; i++) {
    checksum = (checksum << 1) ^ data[i];
  }

  return checksum;
}

void storeCalibration() {
  CalibrationRecord record = {};
  record.magic = RECORD_MAGIC;
  record.version = CALIBRATION_VERSION;
  record.channelGain = HX711_CHANNEL_GAIN;
  record.offset = static_cast<int32_t>(scale.get_offset());
  record.scale = scale.get_scale();
  record.checksum = checksumOf(&record, offsetof(CalibrationRecord, checksum));

  EEPROM.put(CALIBRATION_EEPROM_ADDRESS, record);

  if (EEPROM.commit()) {
    Serial.println(F("Calibration saved to EEPROM."));
  } else {
    Serial.println(F("ERROR: EEPROM commit failed."));
  }
}

bool restoreCalibration() {
  CalibrationRecord record = {};
  EEPROM.get(CALIBRATION_EEPROM_ADDRESS, record);

  if (record.magic != RECORD_MAGIC ||
      record.version != CALIBRATION_VERSION ||
      record.channelGain != HX711_CHANNEL_GAIN ||
      record.checksum != checksumOf(&record, offsetof(CalibrationRecord, checksum))) {
    return false;
  }

  scale.set_offset(record.offset);
  scale.set_scale(record.scale);
  return true;
}

void clearCalibration() {
  const CalibrationRecord empty = {};
  EEPROM.put(CALIBRATION_EEPROM_ADDRESS, empty);
  EEPROM.commit();
  Serial.println(F("Stored calibration erased. Next boot will tare from scratch."));
}

float changeThresholdGrams = DEFAULT_CHANGE_THRESHOLD_GRAMS;

float constrainThreshold(float grams) {
  return grams < MIN_CHANGE_THRESHOLD_GRAMS ? MIN_CHANGE_THRESHOLD_GRAMS : grams;
}

void storeSettings() {
  SettingsRecord record = {};
  record.magic = RECORD_MAGIC;
  record.version = SETTINGS_VERSION;
  record.reserved = 0;
  record.changeThresholdGrams = changeThresholdGrams;
  record.checksum = checksumOf(&record, offsetof(SettingsRecord, checksum));

  EEPROM.put(SETTINGS_EEPROM_ADDRESS, record);
  EEPROM.commit();
}

void restoreSettings() {
  SettingsRecord record = {};
  EEPROM.get(SETTINGS_EEPROM_ADDRESS, record);

  if (record.magic != RECORD_MAGIC || record.version != SETTINGS_VERSION ||
      record.checksum != checksumOf(&record, offsetof(SettingsRecord, checksum))) {
    changeThresholdGrams = DEFAULT_CHANGE_THRESHOLD_GRAMS;
    return;
  }

  if (record.changeThresholdGrams > 0.0f) {
    changeThresholdGrams = constrainThreshold(record.changeThresholdGrams);
  }
}

void applyReferenceScale() {
  scale.set_scale(REFERENCE_UNIT_COUNT / REFERENCE_UNIT_GRAMS);
}

void tareLoadCell() {
  const float currentScale = scale.get_scale();

  scale.tare(TARE_AVERAGE_COUNT);
  scale.set_scale(currentScale);
  storeCalibration();

  Serial.print(F("Tared. Raw offset = "));
  Serial.println(scale.get_offset());
}

bool readNumberFromSerial(float &outValue) {
  String entry;

  while (true) {
    while (!Serial.available()) {
      delay(POLL_INTERVAL_MS);
    }

    const char character = static_cast<char>(Serial.read());

    if (character == '\n' || character == '\r') {
      if (entry.length() > 0) {
        break;
      }
      continue;
    }

    if (entry.length() < SERIAL_ENTRY_MAX_LENGTH) {
      entry += character;
    }
  }

  outValue = entry.toFloat();
  return outValue > 0.0f;
}

void calibrateWithKnownWeight() {
  Serial.println(F("Enter the known weight in grams, then press Enter:"));

  float knownGrams = 0.0f;
  if (!readNumberFromSerial(knownGrams)) {
    Serial.println(F("Calibration aborted: weight must be greater than zero."));
    return;
  }

  const long zeroedRaw = scale.read_average(1) - scale.get_offset();

  if (zeroedRaw == 0) {
    Serial.println(F("Calibration aborted: no change detected on the load cell."));
    return;
  }

  scale.set_scale(knownGrams / static_cast<float>(zeroedRaw));

  Serial.print(F("Calibrated against "));
  Serial.print(knownGrams, 1);
  Serial.print(F(" g. Scale factor = "));
  Serial.println(scale.get_scale(), 4);

  storeCalibration();
}

void setChangeThreshold() {
  Serial.println(F("Enter the minimum weight change in grams, then press Enter:"));

  float grams = 0.0f;
  if (!readNumberFromSerial(grams)) {
    Serial.println(F("Threshold unchanged: value must be greater than zero."));
    return;
  }

  changeThresholdGrams = constrainThreshold(grams);
  detector.setThresholdGrams(changeThresholdGrams);
  storeSettings();

  Serial.print(F("Change threshold set to "));
  Serial.print(grams, 2);
  Serial.println(F(" g."));
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  t      tare, keeping the current scale factor"));
  Serial.println(F("  c      calibrate: send c, then the known mass in grams"));
  Serial.println(F("  g      set change threshold: send g, then the grams"));
  Serial.println(F("  x      erase the stored calibration"));
  Serial.println(F("  h      print this help"));
}

void handleSerialCommands() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());

    switch (command) {
      case 't':
      case 'T':
        tareLoadCell();
        break;
      case 'c':
      case 'C':
        calibrateWithKnownWeight();
        break;
      case 'g':
      case 'G':
        setChangeThreshold();
        break;
      case 'x':
      case 'X':
        clearCalibration();
        break;
      case 'h':
      case 'H':
      case '?':
        printHelp();
        break;
      default:
        break;
    }
  }
}

void printSample() {
  Serial.print(F("weight_g="));
  Serial.print(scale.get_units(SAMPLE_AVERAGE_COUNT), 2);
  Serial.print(F("\traw="));
  Serial.print(scale.get_value(1), 0);
  Serial.print(F("\tfiltered="));
  Serial.print(detector.filteredGrams(), 2);
  Serial.print(F("\tbaseline="));
  Serial.print(detector.baselineGrams(), 2);
  Serial.print(F("\tdev="));
  Serial.print(detector.deviationGrams(), 2);
  Serial.print(F("\tquiet="));
  Serial.print(detector.quiet() ? 1 : 0);

  if (detector.debounceProgress() > 0) {
    Serial.print(F("\tconfirm="));
    Serial.print(detector.debounceProgress());
    Serial.print('/');
    Serial.print(DEBOUNCE_TICKS);
  }

  Serial.println();
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  const uint32_t serialStart = millis();
  while (!Serial && (millis() - serialStart) < SERIAL_STARTUP_GRACE_MS) {
    delay(POLL_INTERVAL_MS);
  }

  EEPROM.begin(EEPROM_SIZE);
  restoreSettings();

  scale.begin(LOADCELL_DATA_PIN, LOADCELL_CLOCK_PIN, HX711_CHANNEL_GAIN);

  Serial.println();
  Serial.println(F("HX711 load cell scale with ThingSpeak upload"));
  Serial.print(F("  Data pin (DOUT): GPIO"));
  Serial.println(LOADCELL_DATA_PIN);
  Serial.print(F("  Clock pin (SCK): GPIO"));
  Serial.println(LOADCELL_CLOCK_PIN);

  if (!scale.wait_ready_timeout(READY_TIMEOUT_MS, POLL_INTERVAL_MS)) {
    Serial.println(F("ERROR: HX711 never signalled data ready."));
    Serial.println(F("Check VCC/GND, and the DOUT/SCK wiring."));
  }

  delay(AMPLIFIER_SETTLE_MS);

  if (restoreCalibration()) {
    Serial.print(F("Restored calibration. Offset = "));
    Serial.print(scale.get_offset());
    Serial.print(F(", scale factor = "));
    Serial.println(scale.get_scale(), 4);
  } else {
    applyReferenceScale();
    Serial.println(F("No stored calibration; taring an empty platform."));
    tareLoadCell();
  }

  ChangeDetectorConfig detectorConfig = {};
  detectorConfig.thresholdGrams = changeThresholdGrams;
  detectorConfig.stabilityBandGrams = STABILITY_BAND_GRAMS;
  detectorConfig.confirmWindowMs = CONFIRM_WINDOW_MS;
  detectorConfig.debounceTicks = DEBOUNCE_TICKS;
  detector.begin(detectorConfig);

  Serial.print(F("Change threshold = "));
  Serial.print(changeThresholdGrams, 2);
  Serial.print(F(" g. A change is confirmed after the weight holds steady within "));
  Serial.print(STABILITY_BAND_GRAMS, 2);
  Serial.print(F(" g for "));
  Serial.print(CONFIRM_WINDOW_MS / 1000);
  Serial.println(F(" s."));

  thingSpeakBegin();

  printHelp();
}

void loop() {
  static uint32_t nextSampleAt = 0;
  const uint32_t now = millis();

  if (static_cast<int32_t>(now - nextSampleAt) >= 0) {
    nextSampleAt = now + SAMPLE_INTERVAL_MS;

    const float weightGrams = scale.get_units(SAMPLE_AVERAGE_COUNT);
    const ChangeEvent event = detector.update(weightGrams, now);

    if (event == ChangeEvent::WeightChanged) {
      Serial.print(F("Confirmed sustained change to "));
      Serial.print(detector.pendingCommitGrams(), 2);
      Serial.println(F(" g."));
    }

    if (detector.hasPendingCommit()) {
      const PublishResult result =
          thingSpeakPublish(detector.pendingCommitGrams(), THINGSPEAK_MIN_INTERVAL_MS);

      if (result == PublishResult::Uploaded) {
        detector.commitPending();
      } else if (result == PublishResult::Misconfigured) {
        detector.discardPending();
      }
    }

    printSample();
    handleSerialCommands();
  } else {
    delay(POLL_INTERVAL_MS);
  }
}
