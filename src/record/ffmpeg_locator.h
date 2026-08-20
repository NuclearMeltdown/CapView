#pragma once

// Finds ffmpeg.exe and works out which encoders actually work on this machine.
//
// Parsing "ffmpeg -encoders" is not enough: it lists what the build was
// compiled with, not what the hardware can do. A build with h264_nvenc on a
// machine with an AMD card lists it and then fails at record time. So each
// candidate gets a one frame test encode to /dev/null, which takes a moment and
// tells the truth.

#include <string>
#include <vector>

#include "common.h"
#include "config.h"

namespace cap {

struct EncoderInfo {
  RecordEncoder id = RecordEncoder::Auto;
  std::string ffmpegName;  // e.g. "h264_nvenc"
  std::string label;       // what the settings show
  bool hardware = false;
  bool tested = false;     // the test encode has been run for this entry
  bool available = false;  // survived the test encode
  std::string error;       // why not, when it did not
};

struct FfmpegInfo {
  bool found = false;
  std::string path;     // full path to ffmpeg.exe
  std::string version;  // first line of "ffmpeg -version"
  bool tested = false;  // encoder probe has run
  std::vector<EncoderInfo> encoders;

  const EncoderInfo* Find(RecordEncoder id) const;
  // Best available encoder, hardware first, for RecordEncoder::Auto.
  const EncoderInfo* BestAvailable() const;
  bool AnyAvailable() const;
};

// Looks in this order: the configured path, an "ffmpeg" folder next to
// CapView.exe, next to CapView.exe itself, then PATH. Only fills in path and
// version -- the encoder probe is separate because it takes seconds.
FfmpegInfo LocateFfmpeg(const std::string& configuredPath);

// Runs a one frame encode per candidate and marks what worked. Slow (several
// seconds for the whole list), so this is called on demand, not at startup.
void ProbeEncoders(FfmpegInfo* info);

// Tests only as much as is needed to answer "can I record right now": the
// requested encoder, or for Auto the preference order until one works. Results
// are remembered, so a second call costs nothing. Returns null when nothing
// usable was found.
const EncoderInfo* EnsureUsableEncoder(FfmpegInfo* info, RecordEncoder wanted);

// The full candidate list with labels, regardless of availability.
std::vector<EncoderInfo> KnownEncoders();

// Runs ffmpeg with the given arguments and captures stdout+stderr.
// Returns false when the process could not be started at all.
bool RunFfmpeg(const std::string& exe, const std::wstring& args, std::string* output,
               DWORD* exitCode, DWORD timeoutMs);

// Default recording folder: Videos\CapView.
std::wstring DefaultRecordFolder();

}  // namespace cap
