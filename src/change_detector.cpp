#include "change_detector.h"

#include <cmath>
#include <cstring>

void ChangeDetector::begin(const ChangeDetectorConfig &config) {
  config_ = config;
  sampleCount_ = 0;
  sampleHead_ = 0;
  historyCount_ = 0;
  historyHead_ = 0;
  filtered_ = 0.0f;
  baseline_ = 0.0f;
  baselineReady_ = false;
  quiet_ = false;
  debounce_ = 0;
  pendingCommitActive_ = false;
  pendingCommitGrams_ = 0.0f;
}

void ChangeDetector::setThresholdGrams(float grams) {
  config_.thresholdGrams = grams;
  debounce_ = 0;
}

void ChangeDetector::pushSample(float value) {
  samples_[sampleHead_] = value;
  sampleHead_ = (sampleHead_ + 1) % kMedianWindow;
  if (sampleCount_ < kMedianWindow) {
    sampleCount_++;
  }
}

float ChangeDetector::median() const {
  memcpy(scratch_, samples_, sampleCount_ * sizeof(float));

  for (size_t i = 1; i < sampleCount_; i++) {
    const float key = scratch_[i];
    size_t j = i;
    while (j > 0 && scratch_[j - 1] > key) {
      scratch_[j] = scratch_[j - 1];
      j--;
    }
    scratch_[j] = key;
  }

  return scratch_[sampleCount_ / 2];
}

void ChangeDetector::pushHistory(uint32_t nowMs, float value) {
  if (historyCount_ == kMaxHistory) {
    historyHead_ = (historyHead_ + 1) % kMaxHistory;
    historyCount_--;
  }

  const size_t index = (historyHead_ + historyCount_) % kMaxHistory;
  historyMs_[index] = nowMs;
  historyGrams_[index] = value;
  historyCount_++;
}

bool ChangeDetector::windowIsQuiet(uint32_t nowMs) const {
  uint32_t oldestMs = 0;
  float minimum = 0.0f;
  float maximum = 0.0f;
  size_t considered = 0;

  for (size_t i = 0; i < historyCount_; i++) {
    const size_t index = (historyHead_ + i) % kMaxHistory;
    if (static_cast<uint32_t>(nowMs - historyMs_[index]) > config_.confirmWindowMs) {
      continue;
    }
    if (considered == 0 || historyMs_[index] < oldestMs) {
      oldestMs = historyMs_[index];
    }
    if (considered == 0 || historyGrams_[index] < minimum) {
      minimum = historyGrams_[index];
    }
    if (considered == 0 || historyGrams_[index] > maximum) {
      maximum = historyGrams_[index];
    }
    considered++;
  }

  if (considered < 2) {
    return false;
  }

  if (static_cast<uint32_t>(nowMs - oldestMs) < config_.confirmWindowMs) {
    return false;
  }

  return (maximum - minimum) <= config_.stabilityBandGrams;
}

ChangeEvent ChangeDetector::update(float rawGrams, uint32_t nowMs) {
  pushSample(rawGrams);
  filtered_ = median();
  pushHistory(nowMs, filtered_);
  quiet_ = windowIsQuiet(nowMs);

  if (!baselineReady_) {
    if (!quiet_) {
      return ChangeEvent::None;
    }
    baseline_ = filtered_;
    baselineReady_ = true;
    pendingCommitGrams_ = baseline_;
    pendingCommitActive_ = true;
    return ChangeEvent::InitialReading;
  }

  if (pendingCommitActive_) {
    debounce_ = 0;
    return ChangeEvent::None;
  }

  if (!quiet_ || fabsf(filtered_ - baseline_) <= config_.thresholdGrams) {
    debounce_ = 0;
    return ChangeEvent::None;
  }

  if (debounce_ < config_.debounceTicks) {
    debounce_++;
    return ChangeEvent::None;
  }

  debounce_ = 0;
  pendingCommitGrams_ = filtered_;
  pendingCommitActive_ = true;
  return ChangeEvent::WeightChanged;
}

bool ChangeDetector::hasPendingCommit() const {
  return pendingCommitActive_;
}

float ChangeDetector::pendingCommitGrams() const {
  return pendingCommitGrams_;
}

void ChangeDetector::commitPending() {
  if (!pendingCommitActive_) {
    return;
  }
  baseline_ = pendingCommitGrams_;
  pendingCommitActive_ = false;
}

void ChangeDetector::discardPending() {
  pendingCommitActive_ = false;
  debounce_ = 0;
}

float ChangeDetector::filteredGrams() const {
  return filtered_;
}

float ChangeDetector::baselineGrams() const {
  return baseline_;
}

float ChangeDetector::deviationGrams() const {
  if (!baselineReady_) {
    return 0.0f;
  }
  return filtered_ - baseline_;
}

bool ChangeDetector::baselineReady() const {
  return baselineReady_;
}

bool ChangeDetector::quiet() const {
  return quiet_;
}

uint8_t ChangeDetector::debounceProgress() const {
  return debounce_;
}

float ChangeDetector::thresholdGrams() const {
  return config_.thresholdGrams;
}
