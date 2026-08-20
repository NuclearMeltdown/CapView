#include "audio/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace cap {

void AudioRing::Reset(size_t capacityFrames) {
  std::lock_guard<std::mutex> lock(mutex_);
  capacityFrames_ = std::max<size_t>(capacityFrames, 256);
  data_.assign(capacityFrames_ * 2, 0.0f);
  readPos_ = 0;
  count_ = 0;
  overruns_.store(0, std::memory_order_relaxed);
}

size_t AudioRing::Write(const float* interleaved, size_t frames) {
  if (frames == 0) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacityFrames_ == 0) return 0;

  if (frames > capacityFrames_) {
    // Far more than fits: keep only the newest tail.
    interleaved += (frames - capacityFrames_) * 2;
    frames = capacityFrames_;
  }
  const size_t freeFrames = capacityFrames_ - count_;
  if (frames > freeFrames) {
    // Drop the oldest audio rather than letting the delay grow.
    const size_t drop = frames - freeFrames;
    readPos_ = (readPos_ + drop) % capacityFrames_;
    count_ -= drop;
    overruns_.fetch_add(1, std::memory_order_relaxed);
  }

  const size_t writePos = (readPos_ + count_) % capacityFrames_;
  const size_t firstChunk = std::min(frames, capacityFrames_ - writePos);
  memcpy(&data_[writePos * 2], interleaved, firstChunk * 2 * sizeof(float));
  if (firstChunk < frames) {
    memcpy(&data_[0], interleaved + firstChunk * 2, (frames - firstChunk) * 2 * sizeof(float));
  }
  count_ += frames;
  return frames;
}

size_t AudioRing::Read(float* out, size_t frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacityFrames_ == 0) return 0;
  const size_t take = std::min(frames, count_);
  const size_t firstChunk = std::min(take, capacityFrames_ - readPos_);
  memcpy(out, &data_[readPos_ * 2], firstChunk * 2 * sizeof(float));
  if (firstChunk < take) {
    memcpy(out + firstChunk * 2, &data_[0], (take - firstChunk) * 2 * sizeof(float));
  }
  readPos_ = (readPos_ + take) % capacityFrames_;
  count_ -= take;
  return take;
}

size_t AudioRing::Available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

}  // namespace cap
