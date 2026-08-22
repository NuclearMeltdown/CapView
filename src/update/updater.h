#pragma once

// Checks the GitHub releases page for a newer build, and can fetch and install
// it. Everything that touches the network runs on a thread of its own; the UI
// only ever reads a snapshot.
//
// Installing replaces the running executable. Windows will not let a running
// image be overwritten, but it will let it be *renamed* -- so the old build is
// moved aside, the new one takes its name, and the next start deletes the
// leftover. Nothing has to be running for that to work, and there is no helper
// process to go missing.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cap {

struct UpdateStatus {
  enum class State {
    Idle,         // nothing asked for yet
    Checking,     // querying the releases API
    UpToDate,     // asked, and this is the newest
    Available,    // a newer release exists
    Downloading,  // fetching it
    Ready,        // swapped in; a restart picks it up
    Failed,
  };

  State state = State::Idle;
  std::string latestVersion;  // as the tag names it, e.g. "v1.1"
  std::string notes;          // the release text, trimmed to something readable
  std::string error;
  int percent = 0;  // download progress
};

class Updater {
 public:
  Updater() = default;
  ~Updater();

  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  // The version this build reports. Compared against the release tag.
  static const char* currentVersion();

  // Removes the build a previous update moved aside. Call once at startup; it
  // costs nothing when there is nothing to remove.
  static void CleanUpPreviousBuild();

  // Both return immediately; watch status().
  void CheckAsync();
  void InstallAsync();

  UpdateStatus status() const;
  bool busy() const { return busy_.load(std::memory_order_relaxed); }

  // Starts the freshly installed build and asks the caller to quit. False when
  // nothing is waiting.
  bool RestartIntoNewBuild() const;

 private:
  void SetStatus(const UpdateStatus& s);
  void Run(bool install);

  mutable std::mutex mutex_;
  UpdateStatus status_;
  std::string downloadUrl_;  // filled by the check, used by the install
  std::thread thread_;
  std::atomic<bool> busy_{false};
};

}  // namespace cap
