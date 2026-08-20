#pragma once

// Interleaved stereo float ring buffer between audio capture and playback.
// Writers drop the oldest data on overflow, so a stalled consumer can never
// grow the delay without bound.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cap {

class AudioRing {
 public:
  void Reset(size_t capacityFrames);
  size_t Write(const float* interleaved, size_t frames);
  size_t Read(float* out, size_t frames);
  size_t Available() const;
  size_t Capacity() const { return capacityFrames_; }
  uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

 private:
  mutable std::mutex mutex_;
  std::vector<float> data_;
  size_t capacityFrames_ = 0;
  size_t readPos_ = 0;
  size_t count_ = 0;
  std::atomic<uint64_t> overruns_{0};
};

}  // namespace cap
