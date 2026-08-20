#pragma once

// Fetches a static ffmpeg build on request and puts ffmpeg.exe next to CapView.
//
// Downloaded from the upstream, never redistributed by us: the usual Windows
// builds contain x264 and x265 and are therefore GPL, and whoever hands them on
// owes the source with them. Pressing a button that fetches from gyan.dev is not
// distribution, which keeps CapView's own MIT licence uncomplicated. Calling
// ffmpeg as a child process is not linking either, so nothing about the GPL
// reaches this program.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"

namespace cap {

class FfmpegDownloader {
 public:
  enum class State { Idle, Running, Done, Failed };

  FfmpegDownloader() = default;
  ~FfmpegDownloader();

  FfmpegDownloader(const FfmpegDownloader&) = delete;
  FfmpegDownloader& operator=(const FfmpegDownloader&) = delete;

  // Downloads, verifies, extracts, cleans up. Returns false when one is already
  // running. `targetFolder` receives ffmpeg.exe directly.
  bool Start(const std::wstring& targetFolder);

  // Asks the server which version it would deliver, without downloading. Runs on
  // the same worker, so it is also asynchronous.
  bool StartVersionCheck();

  void Cancel();

  State state() const { return state_.load(std::memory_order_relaxed); }
  bool busy() const { return state() == State::Running; }

  // 0..1 while downloading, negative when the size is unknown.
  float progress() const { return progress_.load(std::memory_order_relaxed); }

  // What is happening right now, or what went wrong.
  std::string message() const;
  // Filled in after a successful download or version check.
  std::string remoteVersion() const;
  std::string resultPath() const;

 private:
  void Run(std::wstring targetFolder, bool versionOnly);
  void SetMessage(const std::string& text);

  std::thread thread_;
  std::atomic<State> state_{State::Idle};
  std::atomic<float> progress_{-1.0f};
  std::atomic<bool> cancel_{false};

  mutable std::mutex mutex_;
  std::string message_;
  std::string remoteVersion_;
  std::string resultPath_;
};

}  // namespace cap
