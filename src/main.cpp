#include <Arduino.h>
#include <EEPROM.h>
#include <HX711.h>

constexpr uint8_t LOADCELL_DATA_PIN = 13;
constexpr uint8_t LOADCELL_CLOCK_PIN = 12;
constexpr byte HX711_CHANNEL_GAIN = 128;

constexpr float REFERENCE_UNIT_COUNT = 424.5f;
constexpr float REFERENCE_UNIT_GRAMS = 1.0f;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SERIAL_STARTUP_GRACE_MS = 1500;
constexpr uint32_t SAMPLE_INTERVAL_MS = 100;
constexpr byte SAMPLE_AVERAGE_COUNT = 10;
constexpr byte TARE_AVERAGE_COUNT = 25;
constexpr uint32_t AMPLIFIER_SETTLE_MS = 1500;
constexpr uint32_t READY_TIMEOUT_MS = 2000;
constexpr uint32_t POLL_INTERVAL_MS = 10;
constexpr size_t SERIAL_ENTRY_MAX_LENGTH = 16;

constexpr size_t CALIBRATION_EEPROM_ADDRESS = 0;
constexpr uint16_t CALIBRATION_MAGIC = 0xA55A;
constexpr uint8_t CALIBRATION_VERSION = 1;

struct CalibrationRecord {
  uint16_t magic;
  uint8_t version;
  uint8_t channelGain;
  int32_t offset;
  float scale;
  uint32_t checksum;
};

static_assert(sizeof(CalibrationRecord) == 16,
              "CalibrationRecord must stay tightly packed for EEPROM storage");

HX711 scale;

uint32_t computeChecksum(const CalibrationRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint32_t checksum = 0x1D;

  for (size_t i = 0; i < offsetof(CalibrationRecord, checksum); i++) {
    checksum = (checksum << 1) ^ bytes[i];
  }

  return checksum;
}

void storeCalibration() {
  CalibrationRecord record = {};
  record.magic = CALIBRATION_MAGIC;
  record.version = CALIBRATION_VERSION;
  record.channelGain = HX711_CHANNEL_GAIN;
  record.offset = static_cast<int32_t>(scale.get_offset());
  record.scale = scale.get_scale();
  record.checksum = computeChecksum(record);

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

  if (record.magic != CALIBRATION_MAGIC ||
      record.version != CALIBRATION_VERSION ||
      record.channelGain != HX711_CHANNEL_GAIN ||
      record.checksum != computeChecksum(record)) {
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

bool readWeightFromSerial(float &outGrams) {
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

  outGrams = entry.toFloat();
  return outGrams > 0.0f;
}

void calibrateWithKnownWeight() {
  Serial.println(F("Enter the known weight in grams, then press Enter:"));

  float knownGrams = 0.0f;
  if (!readWeightFromSerial(knownGrams)) {
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

void printHelp() {
  Serial.println(F("Commands: t = tare, c = calibrate, x = erase stored calibration, h = help"));
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

void setup() {
  Serial.begin(SERIAL_BAUD);

  const uint32_t serialStart = millis();
  while (!Serial && (millis() - serialStart) < SERIAL_STARTUP_GRACE_MS) {
    delay(POLL_INTERVAL_MS);
  }

  EEPROM.begin(sizeof(CalibrationRecord));
  scale.begin(LOADCELL_DATA_PIN, LOADCELL_CLOCK_PIN, HX711_CHANNEL_GAIN);

  Serial.println();
  Serial.println(F("HX711 load cell scale"));
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

  printHelp();
}

void loop() {
  static uint32_t nextSampleAt = 0;
  const uint32_t now = millis();

  if (static_cast<int32_t>(now - nextSampleAt) >= 0) {
    nextSampleAt = now + SAMPLE_INTERVAL_MS;

    Serial.print(F("weight_g="));
    Serial.print(scale.get_units(SAMPLE_AVERAGE_COUNT), 2);
    Serial.print(F("\traw="));
    Serial.println(scale.get_value(1), 0);

    handleSerialCommands();
  } else {
    delay(POLL_INTERVAL_MS);
  }
}
