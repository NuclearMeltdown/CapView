#pragma once

// CapView's end of the virtual camera.
//
// Two things live here that are easy to confuse. Installing puts the media
// source's DLL into the registry, needs administrator rights, and is done once.
// Turning the camera on happens afterwards under the ordinary user account and
// needs no rights at all -- MFVirtualCameraAccess_CurrentUser is explicitly
// allowed to a normal user, as long as the device categories stay the standard
// ones.
//
// Windows 11 only. MFCreateVirtualCamera arrived in build 22000 and there is no
// equivalent before it; a DirectShow filter would have been the alternative,
// but those are invisible to everything that captures through Media Foundation,
// which by now is the Camera app, Teams and the browsers.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMFVirtualCamera;

namespace cap {

class VirtualCamera {
 public:
  enum class Install {
    Missing,      // the source is not registered
    Installed,    // registered and the file is where the registry says
    Stale,        // registered, but pointing at a file that is not there
    Unsupported,  // not Windows 11
  };

  ~VirtualCamera();

  VirtualCamera(const VirtualCamera&) = delete;
  VirtualCamera& operator=(const VirtualCamera&) = delete;
  VirtualCamera() = default;

  // Whether this Windows can host a virtual camera at all.
  static bool Supported();

  // Where the registration stands right now. Cheap enough to call per frame of
  // UI, but it does touch the registry, so the settings tab caches it.
  static Install Status();

  // Both raise a UAC prompt and wait for it. False with `error` filled when the
  // user declines or regsvr32 fails. The DLL is expected beside CapView.exe.
  static bool InstallSource(std::string* error);

  // Deletes the copies left behind by earlier installs. Called once at start,
  // when whatever had them open has usually let go.
  static void CleanUpOldSources();
  static bool UninstallSource(std::string* error);

  // Turning the camera on and off. Start fails if the source is not installed.
  bool Start(std::string* error);
  void Stop();
  bool running() const { return running_.load(std::memory_order_relaxed); }

  // True while an application is actually reading the camera. Until then
  // pushing frames costs nothing, so callers need not check first.
  bool consumed() const { return consumed_.load(std::memory_order_relaxed); }

  // The installed media source is from a different build than this executable
  // and the two cannot agree on the shape of what they share. Reported rather
  // than worked around: the picture would be black either way, and only saying
  // so turns that into something the user can act on.
  bool sourceOutdated() const { return outdated_.load(std::memory_order_relaxed); }

  // Hands over one displayed frame, RGBA8, top row first. Returns immediately:
  // the copy is cheap and the conversion happens on a thread of its own, so the
  // render loop is never waiting on a webcam.
  void PushFrame(const uint8_t* rgba, int stride, int width, int height);

  // What the consuming application asked for, or 0x0 when nobody is reading.
  void wanted(int* width, int* height, int* fps) const;

 private:
  void WorkerLoop();
  bool AttachShared();
  void DetachShared();
  void Publish(const uint8_t* rgba, int stride, int width, int height);

  IMFVirtualCamera* camera_ = nullptr;
  bool mfStarted_ = false;

  std::atomic<bool> running_{false};
  std::atomic<bool> consumed_{false};
  std::atomic<bool> outdated_{false};
  std::atomic<bool> quit_{false};

  std::thread worker_;
  HANDLE wake_ = nullptr;

  // The handover from the render thread: one frame deep, newest wins. A webcam
  // that shows the second newest frame is worse than one that skips it.
  std::mutex pendingMutex_;
  std::vector<uint8_t> pending_;
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;
  bool pendingFull_ = false;

  HANDLE section_ = nullptr;
  void* view_ = nullptr;
  std::vector<uint8_t> scratch_;
  unsigned long lastReport_ = 0;
};

}  // namespace cap
