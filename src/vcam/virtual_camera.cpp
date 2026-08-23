#include "vcam/virtual_camera.h"

#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <shellapi.h>

#include <algorithm>

#include "common.h"
#include "i18n.h"
#include "vcam/vcam_shared.h"

#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "shell32.lib")

namespace cap {
namespace {

const wchar_t kDllName[] = L"capview_vcam.dll";

std::wstring ExeFolder() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s = path;
  const size_t cut = s.find_last_of(L'\\');
  return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut + 1);
}

std::wstring DllPath() { return ExeFolder() + kDllName; }

bool FileThere(const std::wstring& path) {
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Reads back what the registration points at, so "installed" can mean the file
// is actually there rather than merely that a key exists. A build that was
// moved leaves exactly that trap behind.
bool RegisteredPath(std::wstring* out) {
  std::wstring key = L"Software\\Classes\\CLSID\\";
  key += vcam::kSourceClsidString;
  key += L"\\InprocServer32";

  HKEY handle = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t value[MAX_PATH] = {};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(handle, nullptr, nullptr, &type, (BYTE*)value, &bytes);
  ::RegCloseKey(handle);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  *out = value;
  return true;
}

// regsvr32 under UAC. Waiting for it matters: without that the settings tab
// would report the old state right after the user clicked.
bool RunRegsvr(bool remove, std::string* error) {
  const std::wstring dll = DllPath();
  if (!FileThere(dll)) {
    if (error) {
      *error = T("capview_vcam.dll fehlt neben CapView.exe.",
                 "capview_vcam.dll is missing next to CapView.exe.");
    }
    return false;
  }

  std::wstring args = remove ? L"/s /u \"" : L"/s \"";
  args += dll;
  args += L"\"";

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";  // this is the UAC prompt
  info.lpFile = L"regsvr32.exe";
  info.lpParameters = args.c_str();
  info.nShow = SW_HIDE;

  if (!::ShellExecuteExW(&info) || !info.hProcess) {
    const DWORD err = ::GetLastError();
    if (error) {
      *error = err == ERROR_CANCELLED
                   ? T("Abgebrochen -- ohne Administratorrechte geht es nicht.",
                       "Cancelled -- this cannot be done without administrator rights.")
                   : T("regsvr32 ließ sich nicht starten.", "regsvr32 could not be started.");
    }
    return false;
  }

  ::WaitForSingleObject(info.hProcess, 30000);
  DWORD code = 1;
  ::GetExitCodeProcess(info.hProcess, &code);
  ::CloseHandle(info.hProcess);

  if (code != 0) {
    if (error) {
      *error = remove ? T("Die Registrierung ließ sich nicht entfernen.",
                          "The registration could not be removed.")
                      : T("Die Registrierung ist fehlgeschlagen.", "Registration failed.");
    }
    return false;
  }
  return true;
}

// --------------------------------------------------------------------------
// RGBA to NV12, letterboxed into the size the consumer asked for.
//
// BT.601 with limited range, which is what a consumer assumes of NV12 when it
// is not told otherwise, and what the media type declares.
// --------------------------------------------------------------------------
void FillBlank(uint8_t* nv12, int width, int height) {
  ::memset(nv12, 16, (size_t)width * height);
  ::memset(nv12 + (size_t)width * height, 128, (size_t)width * height / 2);
}

void ConvertLetterboxed(const uint8_t* rgba, int stride, int srcW, int srcH, uint8_t* nv12,
                        int dstW, int dstH) {
  FillBlank(nv12, dstW, dstH);
  if (srcW <= 0 || srcH <= 0) return;

  // The picture keeps its shape; the rest stays black. Both offsets are forced
  // even so the chroma plane lines up with the luma one.
  const double scale = std::min((double)dstW / srcW, (double)dstH / srcH);
  int boxW = (int)(srcW * scale + 0.5);
  int boxH = (int)(srcH * scale + 0.5);
  boxW = std::max(2, std::min(boxW, dstW)) & ~1;
  boxH = std::max(2, std::min(boxH, dstH)) & ~1;
  const int offX = ((dstW - boxW) / 2) & ~1;
  const int offY = ((dstH - boxH) / 2) & ~1;

  uint8_t* y = nv12;
  uint8_t* uv = nv12 + (size_t)dstW * dstH;

  // Two rows at a time: the chroma plane is half height, so a pair of luma rows
  // produces one row of it and the source is only walked once.
  for (int dy = 0; dy < boxH; dy += 2) {
    const int sy0 = std::min(srcH - 1, (int)((dy + 0.5) * srcH / boxH));
    const int sy1 = std::min(srcH - 1, (int)((dy + 1.5) * srcH / boxH));
    const uint8_t* row0 = rgba + (size_t)sy0 * stride;
    const uint8_t* row1 = rgba + (size_t)sy1 * stride;
    uint8_t* y0 = y + (size_t)(offY + dy) * dstW + offX;
    uint8_t* y1 = y + (size_t)(offY + dy + 1) * dstW + offX;
    uint8_t* uvRow = uv + (size_t)((offY + dy) / 2) * dstW + offX;

    for (int dx = 0; dx < boxW; dx += 2) {
      const int sx0 = std::min(srcW - 1, (int)((dx + 0.5) * srcW / boxW));
      const int sx1 = std::min(srcW - 1, (int)((dx + 1.5) * srcW / boxW));

      int rSum = 0, gSum = 0, bSum = 0;
      const uint8_t* px[4] = {row0 + sx0 * 4, row0 + sx1 * 4, row1 + sx0 * 4, row1 + sx1 * 4};
      uint8_t* dstY[4] = {y0 + dx, y0 + dx + 1, y1 + dx, y1 + dx + 1};

      for (int i = 0; i < 4; ++i) {
        const int r = px[i][0], g = px[i][1], b = px[i][2];
        rSum += r;
        gSum += g;
        bSum += b;
        *dstY[i] = (uint8_t)std::clamp((66 * r + 129 * g + 25 * b + 128) / 256 + 16, 16, 235);
      }

      // Chroma from the average of the four, which is what 4:2:0 means.
      const int r = rSum / 4, g = gSum / 4, b = bSum / 4;
      uvRow[dx] = (uint8_t)std::clamp((-38 * r - 74 * g + 112 * b + 128) / 256 + 128, 16, 240);
      uvRow[dx + 1] = (uint8_t)std::clamp((112 * r - 94 * g - 18 * b + 128) / 256 + 128, 16, 240);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- lifetime

VirtualCamera::~VirtualCamera() { Stop(); }

bool VirtualCamera::Supported() {
  // Build 22000. Asking the version directly rather than trusting a manifest,
  // because the manifest governs what GetVersionEx admits to and this has to be
  // right whatever the manifest says.
  static const bool ok = [] {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = (RtlGetVersionFn)::GetProcAddress(ntdll, "RtlGetVersion");
    if (!fn) return false;
    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) return false;
    return info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
  }();
  return ok;
}

VirtualCamera::Install VirtualCamera::Status() {
  if (!Supported()) return Install::Unsupported;
  std::wstring registered;
  if (!RegisteredPath(&registered)) return Install::Missing;
  return FileThere(registered) ? Install::Installed : Install::Stale;
}

bool VirtualCamera::InstallSource(std::string* error) {
  if (!Supported()) {
    if (error) {
      *error = T("Virtuelle Kameras gibt es erst ab Windows 11.",
                 "Virtual cameras need Windows 11.");
    }
    return false;
  }
  return RunRegsvr(false, error);
}

bool VirtualCamera::UninstallSource(std::string* error) { return RunRegsvr(true, error); }

bool VirtualCamera::Start(std::string* error) {
  if (running()) return true;

  const Install status = Status();
  if (status == Install::Unsupported) {
    if (error) {
      *error = T("Virtuelle Kameras gibt es erst ab Windows 11.",
                 "Virtual cameras need Windows 11.");
    }
    return false;
  }
  if (status != Install::Installed) {
    if (error) {
      *error = T("Die Kameraquelle ist nicht installiert.",
                 "The camera source is not installed.");
    }
    return false;
  }

  if (!mfStarted_) {
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      if (error) *error = T("Media Foundation ließ sich nicht starten.",
                            "Media Foundation could not be started.");
      return false;
    }
    mfStarted_ = true;
  }

  // Session lifetime: the camera goes away when CapView does, which is the only
  // honest thing for a camera whose pictures come from a running CapView.
  // Current user and the default categories, which is the combination a normal
  // account is allowed to register without elevation.
  HRESULT hr = ::MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                       MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser, L"CapView",
                                       vcam::kSourceClsidString, nullptr, 0, &camera_);
  if (SUCCEEDED(hr)) hr = camera_->Start(nullptr);

  if (FAILED(hr)) {
    if (camera_) {
      camera_->Release();
      camera_ = nullptr;
    }
    if (error) {
      *error = hr == E_ACCESSDENIED
                   ? T("Windows verweigert den Kamerazugriff. Unter Einstellungen -> "
                       "Datenschutz -> Kamera erlauben.",
                       "Windows is denying camera access. Allow it under Settings -> "
                       "Privacy -> Camera.")
                   : T("Die virtuelle Kamera ließ sich nicht starten.",
                       "The virtual camera could not be started.");
    }
    CAP_ERR("Virtuelle Kamera: MFCreateVirtualCamera/Start 0x%08lX", (unsigned long)hr);
    return false;
  }

  quit_ = false;
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  running_ = true;
  worker_ = std::thread(&VirtualCamera::WorkerLoop, this);
  CAP_LOG("Virtuelle Kamera gestartet");
  return true;
}

void VirtualCamera::Stop() {
  if (running_.exchange(false)) {
    quit_ = true;
    if (wake_) ::SetEvent(wake_);
    if (worker_.joinable()) worker_.join();
  }
  if (wake_) {
    ::CloseHandle(wake_);
    wake_ = nullptr;
  }
  DetachShared();
  if (camera_) {
    camera_->Stop();
    camera_->Shutdown();
    camera_->Release();
    camera_ = nullptr;
  }
  if (mfStarted_) {
    ::MFShutdown();
    mfStarted_ = false;
  }
  consumed_ = false;
}

// ------------------------------------------------------------- the handover

void VirtualCamera::PushFrame(const uint8_t* rgba, int stride, int width, int height) {
  if (!running() || !rgba || width <= 0 || height <= 0) return;
  // Nobody watching costs nothing. The flag is set by the worker from what the
  // media source announced, so this is one relaxed load per displayed frame.
  if (!consumed()) return;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.resize((size_t)width * height * 4);
    for (int row = 0; row < height; ++row) {
      ::memcpy(pending_.data() + (size_t)row * width * 4, rgba + (size_t)row * stride,
               (size_t)width * 4);
    }
    pendingWidth_ = width;
    pendingHeight_ = height;
    pendingFull_ = true;
  }
  if (wake_) ::SetEvent(wake_);
}

void VirtualCamera::wanted(int* width, int* height, int* fps) const {
  auto* state = static_cast<vcam::SharedState*>(view_);
  const bool live = state && state->magic == vcam::kMagic && state->consumers > 0;
  if (width) *width = live ? (int)state->wantWidth : 0;
  if (height) *height = live ? (int)state->wantHeight : 0;
  if (fps) *fps = live ? (int)state->wantFps : 0;
}

bool VirtualCamera::AttachShared() {
  if (view_) return true;
  // Opening, never creating: the section lives in the global namespace, which
  // an ordinary account may not create in. The media source makes it, and it
  // only exists while something is actually reading the camera -- so failing
  // here is the normal state, not an error.
  section_ = ::OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, vcam::kSectionName);
  if (!section_) return false;
  view_ = ::MapViewOfFile(section_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, vcam::kSectionBytes);
  if (!view_) {
    ::CloseHandle(section_);
    section_ = nullptr;
    return false;
  }
  return true;
}

void VirtualCamera::DetachShared() {
  if (view_) {
    ::UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (section_) {
    ::CloseHandle(section_);
    section_ = nullptr;
  }
}

void VirtualCamera::Publish(const uint8_t* rgba, int stride, int width, int height) {
  auto* state = static_cast<vcam::SharedState*>(view_);
  if (!state || state->magic != vcam::kMagic) return;

  const uint32_t dstW = state->wantWidth;
  const uint32_t dstH = state->wantHeight;
  if (dstW == 0 || dstH == 0 || dstW > vcam::kMaxWidth || dstH > vcam::kMaxHeight) return;

  const size_t bytes = (size_t)dstW * dstH * 3 / 2;
  scratch_.resize(bytes);
  ConvertLetterboxed(rgba, stride, width, height, scratch_.data(), (int)dstW, (int)dstH);

  const uint32_t slot = state->writeIndex % vcam::kSlotCount;
  vcam::SlotHeader& head = state->slots[slot];

  // Odd while writing, even when whole. The reader checks the number either
  // side of its copy, so it can tell a torn read from a good one without
  // either side ever waiting for the other.
  head.sequence = head.sequence + 1;
  ::MemoryBarrier();

  ::memcpy(vcam::SlotData(state, slot), scratch_.data(), bytes);
  head.width = dstW;
  head.height = dstH;
  head.bytes = (uint32_t)bytes;
  head.timestamp100ns = 0;

  ::MemoryBarrier();
  head.sequence = head.sequence + 1;
  ::MemoryBarrier();
  state->writeIndex = state->writeIndex + 1;
}

void VirtualCamera::WorkerLoop() {
  ::SetThreadDescription(::GetCurrentThread(), L"CapView vcam");

  while (!quit_.load(std::memory_order_relaxed)) {
    ::WaitForSingleObject(wake_, 200);
    if (quit_.load(std::memory_order_relaxed)) break;

    if (!AttachShared()) {
      // No section means no reader. Say so, so PushFrame stops copying.
      if (consumed_.exchange(false)) CAP_LOG("Virtuelle Kamera: Leser weg");
      const DWORD now = ::GetTickCount();
      if (now - lastReport_ > 5000) {
        lastReport_ = now;
        CAP_LOG("Virtuelle Kamera: kein geteilter Speicher (Fehler %lu)",
                (unsigned long)::GetLastError());
      }
      continue;
    }

    auto* state = static_cast<vcam::SharedState*>(view_);

    // Same build on both sides, or nothing doing. A mismatch here is what a
    // half finished update looks like from the inside.
    if (state->magic != vcam::kMagic || state->version != vcam::kVersion ||
        state->stateBytes != sizeof(vcam::SharedState)) {
      if (!outdated_.exchange(true)) {
        CAP_ERR("Virtuelle Kamera: Quelle passt nicht zum Programm "
                "(Version %u statt %u, %u statt %u Byte)",
                state->version, vcam::kVersion, state->stateBytes,
                (unsigned)sizeof(vcam::SharedState));
      }
      consumed_ = false;
      continue;
    }
    outdated_ = false;

    const bool live = state->consumers > 0 && state->wantWidth > 0;
    consumed_ = live;
    if (!live) continue;

    // Once a second, what both ends think is happening. Everything about this
    // feature crosses a process and a session boundary, so without this the
    // only symptom available is "the picture is black".
    const DWORD now = ::GetTickCount();
    if (now - lastReport_ > 1000) {
      lastReport_ = now;
      CAP_LOG("Virtuelle Kamera: %ux%u, Leser %u, Quellstarts %u, Proben %u, davon frisch %u, "
              "veröffentlicht %u",
              state->wantWidth, state->wantHeight, state->consumers, state->sourceStarted,
              state->samplesServed, state->framesTaken, state->writeIndex);
    }

    std::vector<uint8_t> frame;
    int width = 0, height = 0;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      if (!pendingFull_) continue;
      frame.swap(pending_);
      width = pendingWidth_;
      height = pendingHeight_;
      pendingFull_ = false;
    }
    if (width > 0 && height > 0) Publish(frame.data(), width * 4, width, height);
  }
}

}  // namespace cap
