#pragma once

// What CapView and the virtual camera's media source agree on.
//
// The two do not run in the same place. CapView runs as the logged in user;
// the media source is loaded by the Windows Frame Server, which is a service in
// session 0. So the pictures travel through a named section in the global
// object namespace, and the names below are the whole of the contract.
//
// Creating anything under Global\ needs SeCreateGlobalPrivilege, which an
// ordinary user account does not have and a service does. So the *media source*
// creates the section and CapView opens it. That ordering is not a workaround:
// the section is only wanted while something is actually consuming the camera,
// and that is exactly when the media source exists.

#include <stdint.h>

namespace cap {
namespace vcam {

// The media source's COM class, in the form MFCreateVirtualCamera wants it.
// Registered under HKLM by the installer; see vcam_dll.cpp.
inline constexpr wchar_t kSourceClsidString[] = L"{A1E4F2C7-6B3D-4A58-9E21-7C0D5B8F3A46}";

inline constexpr wchar_t kSectionName[] = L"Global\\CapViewVirtualCameraFrames";
// Raised by CapView when a picture has been published, so the media source can
// wait instead of spinning. Not required for correctness -- a reader that misses
// it simply serves the previous picture.
inline constexpr wchar_t kFrameEventName[] = L"Global\\CapViewVirtualCameraFrame";

inline constexpr uint32_t kMagic = 0x43565643u;  // 'CVVC'
inline constexpr uint32_t kVersion = 1u;

// Three slots is enough to keep a reader from ever waiting on the writer: one
// being written, one being read, one spare.
inline constexpr uint32_t kSlotCount = 3u;

// The largest picture the camera offers. Sized for the media type list below.
inline constexpr uint32_t kMaxWidth = 1920u;
inline constexpr uint32_t kMaxHeight = 1080u;
inline constexpr uint32_t kSlotBytes = kMaxWidth * kMaxHeight * 3u / 2u;  // NV12

// The formats the camera advertises. NV12 throughout, because that is what the
// frame server and nearly every consumer want, and offering one layout means
// there is no conversion to get wrong.
struct CameraFormat {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
};

inline constexpr CameraFormat kFormats[] = {
    {1920, 1080, 30},
    {1280, 720, 30},
    {640, 480, 30},
};
inline constexpr uint32_t kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

// One published picture. `sequence` is a seqlock: odd while the slot is being
// written, even when it holds something whole. A reader that sees the same even
// value before and after copying knows the copy is not torn.
struct alignas(64) SlotHeader {
  volatile uint32_t sequence;
  uint32_t width;
  uint32_t height;
  uint32_t bytes;
  int64_t timestamp100ns;
};

struct SharedState {
  uint32_t magic;
  uint32_t version;

  // Written by the media source, read by CapView: the format the consumer
  // actually settled on. Zero means nobody is streaming.
  volatile uint32_t wantWidth;
  volatile uint32_t wantHeight;
  volatile uint32_t wantFps;
  volatile uint32_t consumers;

  // Written by CapView, read by the media source.
  volatile uint32_t writeIndex;  // total pictures published, ever
  volatile uint32_t producerAlive;

  SlotHeader slots[kSlotCount];
};

// The pictures follow the state, each slot aligned so a copy never straddles.
inline constexpr uint32_t kSlotDataOffset = 4096u;
inline constexpr uint32_t kSectionBytes = kSlotDataOffset + kSlotCount * kSlotBytes;

inline uint8_t* SlotData(void* base, uint32_t slot) {
  return static_cast<uint8_t*>(base) + kSlotDataOffset + slot * kSlotBytes;
}

}  // namespace vcam
}  // namespace cap
