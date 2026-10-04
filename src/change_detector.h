#pragma once

#include <Arduino.h>

struct ChangeDetectorConfig {
  float thresholdGrams;
  float stabilityBandGrams;
  uint32_t confirmWindowMs;
  uint8_t debounceTicks;
};

enum class ChangeEvent : uint8_t {
  None,
  InitialReading,
  WeightChanged,
};

class ChangeDetector {
public:
  void begin(const ChangeDetectorConfig &config);
  void setThresholdGrams(float grams);

  ChangeEvent update(float rawGrams, uint32_t nowMs);

  bool hasPendingCommit() const;
  float pendingCommitGrams() const;
  void commitPending();
  void discardPending();

  float filteredGrams() const;
  float baselineGrams() const;
  float deviationGrams() const;
  bool baselineReady() const;
  bool quiet() const;
  uint8_t debounceProgress() const;
  float thresholdGrams() const;

private:
  static constexpr size_t kMedianWindow = 13;
  static constexpr size_t kMaxHistory = 64;

  void pushSample(float value);
  void pushHistory(uint32_t nowMs, float value);
  float median() const;
  bool windowIsQuiet(uint32_t nowMs) const;

  ChangeDetectorConfig config_{};

  float samples_[kMedianWindow]{};
  size_t sampleCount_ = 0;
  size_t sampleHead_ = 0;
  mutable float scratch_[kMedianWindow]{};

  uint32_t historyMs_[kMaxHistory]{};
  float historyGrams_[kMaxHistory]{};
  size_t historyCount_ = 0;
  size_t historyHead_ = 0;

  float filtered_ = 0.0f;
  float baseline_ = 0.0f;
  bool baselineReady_ = false;

  bool quiet_ = false;
  uint8_t debounce_ = 0;

  bool pendingCommitActive_ = false;
  float pendingCommitGrams_ = 0.0f;
};
