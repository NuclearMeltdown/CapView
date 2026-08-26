// CapView's virtual camera, as a DirectShow source filter.
//
// The shape of this file follows the shape of DirectShow: a filter that owns
// one output pin, the pin that negotiates a format and pushes samples, and the
// two enumerators the graph insists on. Everything else is the reader that
// takes pictures out of CapView's shared section and the scaler that fits them
// to whatever the consumer asked for.
//
// The scaling happens here, in the consumer's process, and that is the point of
// the whole exercise. CapView publishes the source once, at its own size and
// rate, and each consumer is served from that: OBS takes it untouched, Discord
// gets it fitted into 1280x720, and neither of them costs the other anything.

#include "vcam/vcam_filter.h"

#include <dshow.h>
#include <strsafe.h>

#include <cmath>
#include <new>
#include <vector>

#include "vcam/vcam_idle.h"
#include "vcam/vcam_shared.h"

namespace cap {
namespace vcam {

// {A326E6EC-3F70-468B-A826-4F9D42CB5C8E}
const CLSID CLSID_CapViewFilter = {
    0xa326e6ec, 0x3f70, 0x468b, {0xa8, 0x26, 0x4f, 0x9d, 0x42, 0xcb, 0x5c, 0x8e}};

namespace {

long g_liveObjects = 0;

// A media subtype built from a FOURCC. The SDK spells some of these out and not
// others depending on which headers are in scope; the layout is fixed, so
// building them is steadier than hunting for the right include.
GUID SubtypeFromFourCC(DWORD fourcc) {
  GUID g = {0x00000000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
  g.Data1 = fourcc;
  return g;
}

const DWORD kFourCcNv12 = MAKEFOURCC('N', 'V', '1', '2');
const DWORD kFourCcP010 = MAKEFOURCC('P', '0', '1', '0');

DWORD FourCcFor(uint32_t pixel) { return pixel == kPixelP010 ? kFourCcP010 : kFourCcNv12; }
WORD BitsFor(uint32_t pixel) { return pixel == kPixelP010 ? 24 : 12; }

// The fallback the camera stands in for when there is nothing to show: the
// source's shape if we know it, otherwise a plain 720p frame. A consumer that
// opens the camera before CapView is running still gets a working stream, which
// is what every other virtual camera does and what applications expect.
const uint32_t kIdleWidth = 1280;
const uint32_t kIdleHeight = 720;
const int64_t kIdleInterval = 333333;  // 30 fps

// The slowest the pin will ever agree to run. Not a real limit, just an end for
// the range: no consumer asks for a frame every ten seconds.
const int64_t kSlowestInterval = 10000000;  // 1 fps

// Sizes offered as concrete entries alongside the source's own. Consumers come
// in two kinds: the ones that read VIDEO_STREAM_CONFIG_CAPS and understand that
// a range means "anything in here", and the ones that only look at the media
// type attached to each capability and treat the list as the whole truth. The
// ranges are for the first kind and this list is for the second, which is the
// kind Discord is.
struct StdSize {
  uint32_t w, h;
};
const StdSize kOfferedSizes[] = {
    {3840, 2160}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720},
    {960, 540},   {854, 480},   {640, 480},   {640, 360},  {320, 240},
};

// ------------------------------------------------------------- media types

void FreeMediaTypeContents(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  if (mt->cbFormat && mt->pbFormat) {
    ::CoTaskMemFree(mt->pbFormat);
    mt->pbFormat = nullptr;
    mt->cbFormat = 0;
  }
  if (mt->pUnk) {
    mt->pUnk->Release();
    mt->pUnk = nullptr;
  }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  FreeMediaTypeContents(mt);
  ::CoTaskMemFree(mt);
}

bool CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
  if (!dst || !src) return false;
  *dst = *src;
  dst->pbFormat = nullptr;
  dst->pUnk = nullptr;
  if (src->cbFormat && src->pbFormat) {
    dst->pbFormat = (BYTE*)::CoTaskMemAlloc(src->cbFormat);
    if (!dst->pbFormat) {
      dst->cbFormat = 0;
      return false;
    }
    ::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  }
  if (src->pUnk) {
    dst->pUnk = src->pUnk;
    dst->pUnk->AddRef();
  }
  return true;
}

// Every picture this filter deals in is planar with half-height chroma, so the
// stride is the width in samples and the size is a stride and a half per row.
DWORD ImageBytes(uint32_t pixel, uint32_t width, uint32_t height) {
  const uint32_t stride = width * BytesPerSample(pixel);
  return (DWORD)((uint64_t)stride * height * 3ull / 2ull);
}

// Builds one VIDEOINFOHEADER media type. Sizes are forced even because chroma
// is subsampled in both directions and an odd edge has no defined meaning.
bool FillMediaType(AM_MEDIA_TYPE* mt, uint32_t pixel, uint32_t width, uint32_t height,
                   int64_t interval) {
  width &= ~1u;
  height &= ~1u;
  if (width < 2 || height < 2) return false;

  auto* vih = (VIDEOINFOHEADER*)::CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
  if (!vih) return false;
  ::ZeroMemory(vih, sizeof(VIDEOINFOHEADER));

  vih->AvgTimePerFrame = interval > 0 ? interval : kIdleInterval;
  vih->dwBitRate =
      (DWORD)((uint64_t)ImageBytes(pixel, width, height) * 8ull * 10000000ull /
              (uint64_t)vih->AvgTimePerFrame);

  BITMAPINFOHEADER& bih = vih->bmiHeader;
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = (LONG)width;
  // Positive, and the picture is top row first. That is the convention for
  // planar YUV FourCCs: the sign only carries orientation for RGB.
  bih.biHeight = (LONG)height;
  bih.biPlanes = 1;
  bih.biBitCount = BitsFor(pixel);
  bih.biCompression = FourCcFor(pixel);
  bih.biSizeImage = ImageBytes(pixel, width, height);

  ::ZeroMemory(mt, sizeof(AM_MEDIA_TYPE));
  mt->majortype = MEDIATYPE_Video;
  mt->subtype = SubtypeFromFourCC(FourCcFor(pixel));
  mt->bFixedSizeSamples = TRUE;
  mt->bTemporalCompression = FALSE;
  mt->lSampleSize = bih.biSizeImage;
  mt->formattype = FORMAT_VideoInfo;
  mt->cbFormat = sizeof(VIDEOINFOHEADER);
  mt->pbFormat = (BYTE*)vih;
  return true;
}

// Reads back what FillMediaType put in, and says no to anything else. Both the
// pin's QueryAccept and its connection path go through here, so there is one
// answer to "would you take this" rather than two that can drift apart.
bool ParseMediaType(const AM_MEDIA_TYPE* mt, uint32_t* pixel, uint32_t* width, uint32_t* height,
                    int64_t* interval) {
  if (!mt || mt->majortype != MEDIATYPE_Video) return false;
  if (mt->formattype != FORMAT_VideoInfo || mt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
  if (!mt->pbFormat) return false;

  uint32_t p;
  if (mt->subtype == SubtypeFromFourCC(kFourCcNv12)) {
    p = kPixelNv12;
  } else if (mt->subtype == SubtypeFromFourCC(kFourCcP010)) {
    p = kPixelP010;
  } else {
    return false;
  }

  const auto* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
  const LONG w = vih->bmiHeader.biWidth;
  const LONG h = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
  if (w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
  if (w > 16384 || h > 16384) return false;

  if (pixel) *pixel = p;
  if (width) *width = (uint32_t)w;
  if (height) *height = (uint32_t)h;
  if (interval) *interval = vih->AvgTimePerFrame > 0 ? vih->AvgTimePerFrame : kIdleInterval;
  return true;
}

// ------------------------------------------------------------------ scaling

// Bilinear resample of one plane into a sub-rectangle of the destination.
// `channels` is 1 for luma and 2 for the interleaved chroma pair, which is the
// only difference between the two planes of an NV12 picture.
template <typename S>
void ScalePlane(const S* src, size_t srcStrideBytes, int srcW, int srcH, S* dst,
                size_t dstStrideBytes, int dstX0, int dstY0, int dstW, int dstH, int channels) {
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

  // Whole-number scaling is the common case -- OBS takes the source untouched
  // -- and deserves not to go through the interpolator at all.
  if (srcW == dstW && srcH == dstH) {
    for (int y = 0; y < dstH; ++y) {
      const S* s = (const S*)((const uint8_t*)src + (size_t)y * srcStrideBytes);
      S* d = (S*)((uint8_t*)dst + (size_t)(dstY0 + y) * dstStrideBytes) + (size_t)dstX0 * channels;
      ::memcpy(d, s, (size_t)dstW * channels * sizeof(S));
    }
    return;
  }

  const int64_t stepX = ((int64_t)srcW << 16) / dstW;
  const int64_t stepY = ((int64_t)srcH << 16) / dstH;
  // Half a destination pixel back, so the sample sits in the middle of the
  // source area it stands for rather than at its leading edge.
  const int64_t baseX = (stepX >> 1) - 32768;
  const int64_t baseY = (stepY >> 1) - 32768;
  const int64_t maxX = (int64_t)(srcW - 1) << 16;
  const int64_t maxY = (int64_t)(srcH - 1) << 16;

  for (int y = 0; y < dstH; ++y) {
    int64_t fy = baseY + (int64_t)y * stepY;
    if (fy < 0) fy = 0;
    if (fy > maxY) fy = maxY;
    const int y0 = (int)(fy >> 16);
    const int y1 = y0 + 1 < srcH ? y0 + 1 : y0;
    const uint32_t wy = (uint32_t)(fy & 0xFFFF);

    const S* row0 = (const S*)((const uint8_t*)src + (size_t)y0 * srcStrideBytes);
    const S* row1 = (const S*)((const uint8_t*)src + (size_t)y1 * srcStrideBytes);
    S* d = (S*)((uint8_t*)dst + (size_t)(dstY0 + y) * dstStrideBytes) + (size_t)dstX0 * channels;

    for (int x = 0; x < dstW; ++x) {
      int64_t fx = baseX + (int64_t)x * stepX;
      if (fx < 0) fx = 0;
      if (fx > maxX) fx = maxX;
      const int x0 = (int)(fx >> 16);
      const int x1 = x0 + 1 < srcW ? x0 + 1 : x0;
      const uint32_t wx = (uint32_t)(fx & 0xFFFF);

      for (int c = 0; c < channels; ++c) {
        const uint32_t a = row0[(size_t)x0 * channels + c];
        const uint32_t b = row0[(size_t)x1 * channels + c];
        const uint32_t e = row1[(size_t)x0 * channels + c];
        const uint32_t f = row1[(size_t)x1 * channels + c];
        const uint32_t top = a + (uint32_t)(((int64_t)(b - a) * wx) >> 16);
        const uint32_t bot = e + (uint32_t)(((int64_t)(f - e) * wx) >> 16);
        d[(size_t)x * channels + c] = (S)(top + (uint32_t)(((int64_t)(bot - top) * wy) >> 16));
      }
    }
  }
}

template <typename S>
void FillPlane(S* dst, size_t dstStrideBytes, int w, int h, S value, int channels) {
  for (int y = 0; y < h; ++y) {
    S* d = (S*)((uint8_t*)dst + (size_t)y * dstStrideBytes);
    for (int x = 0; x < w * channels; ++x) d[x] = value;
  }
}

// --------------------------------------------------------------- pixel form
//
// CapView publishes one layout at a time -- whichever the source is -- and a
// consumer negotiated its own before the source was necessarily known. Usually
// they agree. When they do not, converting is the only thing that keeps a
// picture on the screen, so it converts.
//
// This is deliberately an approximation and not a colour managed path. Going
// from PQ BT.2020 to eight bit BT.709 properly means a tone mapper; what is
// here is a fixed curve and a saturation nudge. It exists so that switching a
// console to HDR while OBS is recording changes how the picture looks rather
// than whether there is one.

// Ten bit PQ luma to eight bit BT.709 luma, one entry per code. Built once:
// there are only 1024 of them and the alternative is a pow() per pixel.
const uint8_t* PqToSdrLuma() {
  static uint8_t table[1024];
  static bool built = false;
  if (built) return table;
  for (int code = 0; code < 1024; ++code) {
    // Limited range 64..940 to signal level.
    double e = (code - 64.0) / 876.0;
    if (e < 0.0) e = 0.0;
    if (e > 1.0) e = 1.0;

    // PQ EOTF, giving absolute luminance up to 10,000 nits.
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
                 c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(e, 1.0 / m2);
    double num = p - c1;
    if (num < 0.0) num = 0.0;
    double y = std::pow(num / (c2 - c3 * p), 1.0 / m1) * 10000.0;

    // Reinhard against a 400 nit white, which is where an ordinary HDR game
    // puts its diffuse white. Highlights roll off instead of clipping.
    const double white = 400.0;
    double v = y / white;
    v = v * (1.0 + v / 16.0) / (1.0 + v);
    if (v > 1.0) v = 1.0;

    // BT.709 OETF, then limited range again.
    const double g = v < 0.018 ? v * 4.5 : 1.099 * std::pow(v, 0.45) - 0.099;
    int out = (int)(g * 219.0 + 16.5);
    if (out < 16) out = 16;
    if (out > 235) out = 235;
    table[code] = (uint8_t)out;
  }
  built = true;
  return table;
}

// BT.2020 chroma read as BT.709 comes out undersaturated, so it is opened up by
// a constant. A matrix would need the luma back in linear light, which is the
// tone mapper's business and not worth doing twice.
const double kWideToNarrowSaturation = 1.25;

void ConvertP010ToNv12(const uint8_t* src, uint32_t srcStride, uint32_t w, uint32_t h,
                       uint8_t* dst) {
  const uint8_t* luma = PqToSdrLuma();
  const size_t dstStride = w;
  for (uint32_t y = 0; y < h; ++y) {
    const uint16_t* s = (const uint16_t*)(src + (size_t)y * srcStride);
    uint8_t* d = dst + (size_t)y * dstStride;
    for (uint32_t x = 0; x < w; ++x) d[x] = luma[s[x] >> 6];
  }
  const uint8_t* srcC = src + (size_t)srcStride * h;
  uint8_t* dstC = dst + dstStride * h;
  for (uint32_t y = 0; y < h / 2; ++y) {
    const uint16_t* s = (const uint16_t*)(srcC + (size_t)y * srcStride);
    uint8_t* d = dstC + (size_t)y * dstStride;
    for (uint32_t x = 0; x < w; ++x) {
      const double c = ((s[x] >> 6) - 512.0) / 4.0 * kWideToNarrowSaturation + 128.0;
      d[x] = (uint8_t)(c < 16.0 ? 16 : c > 240.0 ? 240 : (int)(c + 0.5));
    }
  }
}

void ConvertNv12ToP010(const uint8_t* src, uint32_t srcStride, uint32_t w, uint32_t h,
                       uint8_t* dst) {
  const size_t dstStride = (size_t)w * 2;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)y * srcStride;
    uint16_t* d = (uint16_t*)(dst + (size_t)y * dstStride);
    for (uint32_t x = 0; x < w; ++x) {
      // Eight bit limited to ten bit limited. The curve is left alone: an SDR
      // picture handed to a consumer expecting PQ will read bright, which is
      // visible and recoverable, unlike a black frame.
      int v = (int)((s[x] - 16) * 4.0 + 64.5);
      if (v < 64) v = 64;
      if (v > 940) v = 940;
      d[x] = (uint16_t)((uint32_t)v << 6);
    }
  }
  const uint8_t* srcC = src + (size_t)srcStride * h;
  uint8_t* dstC = dst + dstStride * h;
  for (uint32_t y = 0; y < h / 2; ++y) {
    const uint8_t* s = srcC + (size_t)y * srcStride;
    uint16_t* d = (uint16_t*)(dstC + (size_t)y * dstStride);
    for (uint32_t x = 0; x < w; ++x) {
      int v = (int)((s[x] - 128.0) * 4.0 / kWideToNarrowSaturation + 512.5);
      if (v < 64) v = 64;
      if (v > 960) v = 960;
      d[x] = (uint16_t)((uint32_t)v << 6);
    }
  }
}

// Fits a source picture into a destination of a different shape, keeping the
// aspect ratio and filling what is left with black. The destination rectangle
// is snapped to even edges because chroma is half resolution in both directions
// and a rectangle starting on an odd row has no chroma row of its own.
struct FitRect {
  int x, y, w, h;
};

FitRect FitInto(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) {
  FitRect r{0, 0, (int)dstW, (int)dstH};
  if (!srcW || !srcH) return r;

  // Compare srcW/srcH against dstW/dstH without dividing.
  const uint64_t srcAspect = (uint64_t)srcW * dstH;
  const uint64_t dstAspect = (uint64_t)dstW * srcH;
  if (srcAspect > dstAspect) {
    r.w = (int)dstW;
    r.h = (int)((uint64_t)dstW * srcH / srcW);
  } else if (srcAspect < dstAspect) {
    r.h = (int)dstH;
    r.w = (int)((uint64_t)dstH * srcW / srcH);
  }
  if (r.w < 2) r.w = 2;
  if (r.h < 2) r.h = 2;
  r.w &= ~1;
  r.h &= ~1;
  if (r.w > (int)dstW) r.w = (int)dstW;
  if (r.h > (int)dstH) r.h = (int)dstH;
  r.x = ((int)dstW - r.w) / 2 & ~1;
  r.y = ((int)dstH - r.h) / 2 & ~1;
  return r;
}

// ------------------------------------------------------------------- reader

// The consumer's end of the shared section. One per filter instance, so no
// locking against the other consumers -- the section is written by CapView and
// only ever read here.
class FrameReader {
 public:
  ~FrameReader() { Close(); }

  void Close() {
    if (frameView_) ::UnmapViewOfFile(frameView_);
    if (frameSection_) ::CloseHandle(frameSection_);
    if (controlView_) ::UnmapViewOfFile(controlView_);
    if (controlSection_) ::CloseHandle(controlSection_);
    frameView_ = nullptr;
    frameSection_ = nullptr;
    controlView_ = nullptr;
    controlSection_ = nullptr;
    frames_ = nullptr;
    control_ = nullptr;
    mappedGeneration_ = 0;
  }

  ControlBlock* control() { return control_; }

  // Opens the control section if it is not open yet. Cheap to call often: when
  // CapView is not running there is nothing to open and this is one failed
  // OpenFileMapping, which is why it is rate limited.
  bool EnsureControl() {
    if (control_) return true;
    const DWORD now = ::GetTickCount();
    if (lastControlTry_ && now - lastControlTry_ < 500) return false;
    lastControlTry_ = now ? now : 1;

    controlSection_ = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                         kControlSectionName);
    if (!controlSection_) return false;
    controlView_ = ::MapViewOfFile(controlSection_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                   sizeof(ControlBlock));
    if (!controlView_) {
      ::CloseHandle(controlSection_);
      controlSection_ = nullptr;
      return false;
    }
    auto* cb = (ControlBlock*)controlView_;
    if (cb->magic != kMagic || cb->version != kVersion) {
      // A CapView from a different build. Saying nothing and showing black is
      // the one outcome worth avoiding, so the mapping is dropped and the
      // camera falls back to its idle picture, which at least moves.
      ::UnmapViewOfFile(controlView_);
      ::CloseHandle(controlSection_);
      controlView_ = nullptr;
      controlSection_ = nullptr;
      return false;
    }
    control_ = cb;
    return true;
  }

  // Maps the frame section the control block currently points at, if it is not
  // the one already mapped.
  bool EnsureFrames() {
    if (!control_) return false;
    const uint32_t generation = control_->frameGeneration;
    if (!generation) return false;
    if (frames_ && mappedGeneration_ == generation) return true;

    if (frameView_) ::UnmapViewOfFile(frameView_);
    if (frameSection_) ::CloseHandle(frameSection_);
    frameView_ = nullptr;
    frameSection_ = nullptr;
    frames_ = nullptr;
    mappedGeneration_ = 0;

    wchar_t name[128];
    FormatName(name, 128, kFrameSectionPrefix, generation);
    frameSection_ = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!frameSection_) return false;

    frameView_ = ::MapViewOfFile(frameSection_, FILE_MAP_READ, 0, 0, 0);
    if (!frameView_) {
      ::CloseHandle(frameSection_);
      frameSection_ = nullptr;
      return false;
    }
    auto* fs = (FrameSectionHeader*)frameView_;
    if (fs->magic != kMagic || fs->version != kVersion || fs->generation != generation) {
      ::UnmapViewOfFile(frameView_);
      ::CloseHandle(frameSection_);
      frameView_ = nullptr;
      frameSection_ = nullptr;
      return false;
    }
    frames_ = fs;
    mappedGeneration_ = generation;
    return true;
  }

  // Copies the newest whole picture into `dst`, scaled and letterboxed to the
  // requested shape. Returns false when there is nothing to copy, and leaves
  // `dst` untouched -- the caller then decides between repeating and blanking.
  bool ReadInto(uint8_t* dst, uint32_t dstW, uint32_t dstH, uint32_t dstPixel,
                uint32_t* frameIndex) {
    if (!EnsureControl()) return false;
    if (!control_->producerAlive) return false;
    if (!EnsureFrames()) return false;

    // Newest slot wins. Reading the sequence before and after the copy is what
    // makes this safe without a lock: a slot whose sequence changed underneath
    // is discarded rather than shown torn.
    //
    // Discarded, and then tried again. A torn read only means the writer got
    // to that slot first, and with three slots the next attempt very nearly
    // always lands somewhere else. Giving up after one attempt turns a moment
    // of contention into a black frame, and at large sizes -- where the copy
    // is long enough for contention to be ordinary rather than rare -- into a
    // black picture that never recovers.
    const FrameSectionHeader* fs = frames_;
    const uint8_t* picture = nullptr;
    uint32_t pictureStride = 0;
    uint32_t srcW = 0, srcH = 0, bestFrame = 0;

    for (int attempt = 0; attempt < 3 && !picture; ++attempt) {
      uint32_t bestSlot = 0;
      bool found = false;
      bestFrame = 0;
      for (uint32_t i = 0; i < fs->slotCount && i < kSlotCount; ++i) {
        const uint32_t seq = fs->slots[i].sequence;
        if (seq & 1u) continue;  // being written
        if (!seq) continue;      // never written
        if (!found || fs->slots[i].frameIndex >= bestFrame) {
          bestFrame = fs->slots[i].frameIndex;
          bestSlot = i;
          found = true;
        }
      }
      // Nothing whole anywhere is not contention, and retrying will not change
      // it within one frame's time.
      if (!found) return false;

      const SlotHeader& slot = fs->slots[bestSlot];
      const uint32_t before = slot.sequence;
      if (before & 1u) continue;

      const uint32_t w = slot.width;
      const uint32_t h = slot.height;
      const uint32_t srcStride = slot.strideY;
      if (!w || !h || !srcStride) return false;

      // Into a staging buffer first. Copying straight into the sample would
      // mean the seqlock check happens after the consumer could already have
      // seen the bytes, which defeats the point of checking.
      const size_t need = (size_t)PictureBytes(srcStride, h);
      if (staging_.size() < need) staging_.resize(need);
      ::memcpy(staging_.data(), SlotData(frames_, fs->slotBytes, bestSlot), need);

      if (slot.sequence != before) continue;  // torn

      const uint32_t srcPixel = slot.pixel;
      picture = staging_.data();
      pictureStride = srcStride;
      srcW = w;
      srcH = h;

      if (srcPixel != dstPixel) {
        pictureStride = srcW * BytesPerSample(dstPixel);
        const size_t converted = (size_t)PictureBytes(pictureStride, srcH);
        if (converted_.size() < converted) converted_.resize(converted);
        if (dstPixel == kPixelNv12) {
          ConvertP010ToNv12(staging_.data(), srcStride, srcW, srcH, converted_.data());
        } else {
          ConvertNv12ToP010(staging_.data(), srcStride, srcW, srcH, converted_.data());
        }
        picture = converted_.data();
      }
    }
    if (!picture) return false;

    Compose(picture, pictureStride, srcW, srcH, dst, dstW, dstH, dstPixel);
    if (frameIndex) *frameIndex = bestFrame;
    return true;
  }

  // True while CapView is feeding the camera. What it separates is a picture
  // that is expected and momentarily missing -- a torn read, the moment before
  // the first frame -- from one that is not coming at all. The first deserves
  // black, because it lasts a frame or two. The second deserves an explanation.
  bool ProducerAlive() {
    if (!EnsureControl()) return false;
    return control_->producerAlive != 0;
  }

  // The idle picture: drawn once for a given shape and kept. Laying out text
  // thirty times a second to say the same thing would be thirty times too many.
  void Idle(uint8_t* dst, uint32_t dstW, uint32_t dstH, uint32_t pixel) {
    if (idleW_ != dstW || idleH_ != dstH || idlePixel_ != pixel) {
      idleW_ = dstW;
      idleH_ = dstH;
      idlePixel_ = pixel;
      idle_.assign(ImageBytes(pixel, dstW, dstH), 0);
      idleReady_ = RenderIdlePicture(idle_.data(), dstW, dstH, pixel);
      // Whatever went wrong will go wrong again at this size, so the attempt is
      // not repeated every frame -- and the buffer is given back rather than
      // held for a picture that is never drawn.
      if (!idleReady_) std::vector<uint8_t>().swap(idle_);
    }
    if (idleReady_) {
      ::memcpy(dst, idle_.data(), idle_.size());
    } else {
      Blank(dst, dstW, dstH, pixel);
    }
  }

  // Black, in whichever layout the consumer negotiated. Used while a picture is
  // expected, and as the fallback when the idle picture cannot be drawn.
  static void Blank(uint8_t* dst, uint32_t dstW, uint32_t dstH, uint32_t pixel) {
    const size_t stride = (size_t)dstW * BytesPerSample(pixel);
    uint8_t* chroma = dst + stride * dstH;
    if (pixel == kPixelP010) {
      FillPlane<uint16_t>((uint16_t*)dst, stride, (int)dstW, (int)dstH, (uint16_t)(64 << 6), 1);
      FillPlane<uint16_t>((uint16_t*)chroma, stride, (int)(dstW / 2), (int)(dstH / 2),
                          (uint16_t)(512 << 6), 2);
    } else {
      FillPlane<uint8_t>(dst, stride, (int)dstW, (int)dstH, 16, 1);
      FillPlane<uint8_t>(chroma, stride, (int)(dstW / 2), (int)(dstH / 2), 128, 2);
    }
  }

 private:
  // Scales one picture into the destination, letterboxing what does not fit.
  static void Compose(const uint8_t* src, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                      uint8_t* dst, uint32_t dstW, uint32_t dstH, uint32_t pixel) {
    const FitRect fit = FitInto(srcW, srcH, dstW, dstH);
    const size_t dstStride = (size_t)dstW * BytesPerSample(pixel);
    const uint8_t* srcChroma = src + (size_t)srcStride * srcH;
    uint8_t* dstChroma = dst + dstStride * dstH;

    // Only when there is something to letterbox. A full-bleed fit would have
    // every one of these pixels overwritten a moment later.
    const bool letterbox = fit.w != (int)dstW || fit.h != (int)dstH;
    if (letterbox) Blank(dst, dstW, dstH, pixel);

    if (pixel == kPixelP010) {
      ScalePlane<uint16_t>((const uint16_t*)src, srcStride, (int)srcW, (int)srcH,
                           (uint16_t*)dst, dstStride, fit.x, fit.y, fit.w, fit.h, 1);
      ScalePlane<uint16_t>((const uint16_t*)srcChroma, srcStride, (int)(srcW / 2),
                           (int)(srcH / 2), (uint16_t*)dstChroma, dstStride, fit.x / 2,
                           fit.y / 2, fit.w / 2, fit.h / 2, 2);
    } else {
      ScalePlane<uint8_t>(src, srcStride, (int)srcW, (int)srcH, dst, dstStride, fit.x, fit.y,
                          fit.w, fit.h, 1);
      ScalePlane<uint8_t>(srcChroma, srcStride, (int)(srcW / 2), (int)(srcH / 2), dstChroma,
                          dstStride, fit.x / 2, fit.y / 2, fit.w / 2, fit.h / 2, 2);
    }
  }

  HANDLE controlSection_ = nullptr;
  void* controlView_ = nullptr;
  ControlBlock* control_ = nullptr;

  HANDLE frameSection_ = nullptr;
  void* frameView_ = nullptr;
  FrameSectionHeader* frames_ = nullptr;
  uint32_t mappedGeneration_ = 0;

  DWORD lastControlTry_ = 0;
  std::vector<uint8_t> staging_;
  std::vector<uint8_t> converted_;  // only used when the two layouts disagree

  std::vector<uint8_t> idle_;
  uint32_t idleW_ = 0, idleH_ = 0, idlePixel_ = 0;
  bool idleReady_ = false;
};

}  // namespace

// ============================================================================

class Filter;

// ------------------------------------------------------------------ the pin

class Pin : public IPin, public IAMStreamConfig, public IKsPropertySet, public IQualityControl {
 public:
  explicit Pin(Filter* owner);
  ~Pin();

  // IUnknown -- reference counting is the filter's; a pin is not separately
  // owned and outliving its filter would be meaningless.
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IPin
  STDMETHODIMP Connect(IPin* receive, const AM_MEDIA_TYPE* mt) override;
  STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
  STDMETHODIMP Disconnect() override;
  STDMETHODIMP ConnectedTo(IPin** pin) override;
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
  STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
  STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
  STDMETHODIMP QueryId(LPWSTR* id) override;
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** out) override;
  STDMETHODIMP QueryInternalConnections(IPin**, ULONG* count) override;
  STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
  STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
  STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

  // IAMStreamConfig
  STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
  STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
  STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
  STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) override;

  // IKsPropertySet -- how a capture application finds out that this is the
  // capture pin rather than a preview or still pin. Without it some of them
  // decline to use the filter at all.
  STDMETHODIMP Set(REFGUID set, DWORD id, void* instance, DWORD instanceLength, void* data,
                   DWORD length) override;
  STDMETHODIMP Get(REFGUID set, DWORD id, void* instance, DWORD instanceLength, void* data,
                   DWORD length, DWORD* returned) override;
  STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

  // IQualityControl -- the answer is always "noted": a live source cannot go
  // back and try again, and slowing down would only make the picture later.
  STDMETHODIMP Notify(IBaseFilter*, Quality) override { return S_OK; }
  STDMETHODIMP SetSink(IQualityControl*) override { return S_OK; }

  HRESULT Active();
  HRESULT Inactive();
  bool connected() const { return connected_ != nullptr; }

  // The format list, rebuilt from the source's current shape. Public because
  // the enumerator hands it out.
  void BuildTypeList(std::vector<AM_MEDIA_TYPE>* out);

 private:
  HRESULT AgreeFormat(const AM_MEDIA_TYPE* mt);
  HRESULT NegotiateAllocator(IMemInputPin* input);
  static DWORD WINAPI ThreadEntry(void* self);
  void ThreadLoop();
  void CurrentSource(uint32_t* w, uint32_t* h, int64_t* interval, uint32_t* pixel);

  Filter* owner_;
  IPin* connected_ = nullptr;
  IMemInputPin* memInput_ = nullptr;
  IMemAllocator* allocator_ = nullptr;
  AM_MEDIA_TYPE current_ = {};
  bool haveCurrent_ = false;

  // What the consumer settled on, kept apart from the media type so the
  // streaming thread never has to parse it again.
  uint32_t width_ = kIdleWidth;
  uint32_t height_ = kIdleHeight;
  uint32_t pixel_ = kPixelNv12;
  int64_t interval_ = kIdleInterval;

  HANDLE thread_ = nullptr;
  HANDLE quit_ = nullptr;
  HANDLE wake_ = nullptr;
  FrameReader reader_;
};

// --------------------------------------------------------------- the filter

class Filter : public IBaseFilter, public IAMFilterMiscFlags {
 public:
  Filter();
  ~Filter();

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IPersist
  STDMETHODIMP GetClassID(CLSID* id) override;

  // IMediaFilter
  STDMETHODIMP Stop() override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Run(REFERENCE_TIME start) override;
  STDMETHODIMP GetState(DWORD wait, FILTER_STATE* state) override;
  STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
  STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

  // IBaseFilter
  STDMETHODIMP EnumPins(IEnumPins** out) override;
  STDMETHODIMP FindPin(LPCWSTR id, IPin** out) override;
  STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
  STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
  STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override;

  // IAMFilterMiscFlags -- says "I am a source", which is how a graph decides
  // who to ask about the stream's start.
  STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

  Pin* pin() { return pin_; }
  FILTER_STATE state() const { return state_; }
  IReferenceClock* clock() const { return clock_; }
  CRITICAL_SECTION* lock() { return &lock_; }

  // The consumer table entry this instance owns, or null when CapView is not
  // running. Claimed lazily, because the filter is usually created before
  // CapView has any idea anyone wants a picture.
  ConsumerSlot* ClaimSlot(FrameReader* reader);
  void ReleaseSlot();

 private:
  long refs_ = 1;
  Pin* pin_ = nullptr;
  FILTER_STATE state_ = State_Stopped;
  IReferenceClock* clock_ = nullptr;
  IFilterGraph* graph_ = nullptr;  // weak, per the DirectShow rules
  wchar_t name_[MAX_FILTER_NAME] = {};
  CRITICAL_SECTION lock_ = {};

  ControlBlock* slotOwner_ = nullptr;
  uint32_t slotIndex_ = 0;
  bool haveSlot_ = false;
};

// ----------------------------------------------------------- the enumerators

class PinEnumerator : public IEnumPins {
 public:
  PinEnumerator(Filter* filter, ULONG position) : filter_(filter), position_(position) {
    filter_->AddRef();
    AddLiveObject();
  }
  ~PinEnumerator() {
    filter_->Release();
    ReleaseLiveObject();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
      *out = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const long n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
    if (!pins) return E_POINTER;
    ULONG got = 0;
    while (got < count && position_ == 0) {
      pins[got] = static_cast<IPin*>(filter_->pin());
      pins[got]->AddRef();
      ++got;
      ++position_;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) override {
    position_ += count;
    return position_ > 1 ? S_FALSE : S_OK;
  }
  STDMETHODIMP Reset() override {
    position_ = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumPins** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) PinEnumerator(filter_, position_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  long refs_ = 1;
  Filter* filter_;
  ULONG position_;
};

class MediaTypeEnumerator : public IEnumMediaTypes {
 public:
  MediaTypeEnumerator(Pin* pin, ULONG position) : pin_(pin), position_(position) {
    pin_->AddRef();
    pin_->BuildTypeList(&types_);
    AddLiveObject();
  }
  ~MediaTypeEnumerator() {
    for (AM_MEDIA_TYPE& mt : types_) FreeMediaTypeContents(&mt);
    pin_->Release();
    ReleaseLiveObject();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
      *out = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const long n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** out, ULONG* fetched) override {
    if (!out) return E_POINTER;
    ULONG got = 0;
    while (got < count && position_ < types_.size()) {
      auto* copy = (AM_MEDIA_TYPE*)::CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
      if (!copy) break;
      if (!CopyMediaType(copy, &types_[position_])) {
        ::CoTaskMemFree(copy);
        break;
      }
      out[got++] = copy;
      ++position_;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) override {
    position_ += count;
    return position_ > types_.size() ? S_FALSE : S_OK;
  }
  STDMETHODIMP Reset() override {
    // Re-derived rather than rewound: the source may have changed shape since
    // this enumerator was made, and a stale list would offer a size that is no
    // longer the native one. DirectShow allows exactly this, which is why
    // Reset is documented as able to return S_FALSE-worthy surprises.
    for (AM_MEDIA_TYPE& mt : types_) FreeMediaTypeContents(&mt);
    types_.clear();
    pin_->BuildTypeList(&types_);
    position_ = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumMediaTypes** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) MediaTypeEnumerator(pin_, position_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  long refs_ = 1;
  Pin* pin_;
  ULONG position_;
  std::vector<AM_MEDIA_TYPE> types_;
};

// ============================================================== Pin methods

Pin::Pin(Filter* owner) : owner_(owner) {
  quit_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Pin::~Pin() {
  Inactive();
  Disconnect();
  if (haveCurrent_) FreeMediaTypeContents(&current_);
  if (quit_) ::CloseHandle(quit_);
  if (wake_) ::CloseHandle(wake_);
}

STDMETHODIMP Pin::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IPin) {
    *out = static_cast<IPin*>(this);
  } else if (riid == IID_IAMStreamConfig) {
    *out = static_cast<IAMStreamConfig*>(this);
  } else if (riid == IID_IKsPropertySet) {
    *out = static_cast<IKsPropertySet*>(this);
  } else if (riid == IID_IQualityControl) {
    *out = static_cast<IQualityControl*>(this);
  } else {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) Pin::AddRef() { return owner_->AddRef(); }
STDMETHODIMP_(ULONG) Pin::Release() { return owner_->Release(); }

// What the source looks like right now, or the idle stand-in when CapView is
// not running. Everything the pin offers is derived from this.
void Pin::CurrentSource(uint32_t* w, uint32_t* h, int64_t* interval, uint32_t* pixel) {
  *w = kIdleWidth;
  *h = kIdleHeight;
  *interval = kIdleInterval;
  *pixel = kPixelNv12;

  if (!reader_.EnsureControl()) return;
  ControlBlock* cb = reader_.control();
  if (!cb || !cb->producerAlive) return;
  if (cb->sourceWidth >= 2 && cb->sourceHeight >= 2) {
    *w = cb->sourceWidth & ~1u;
    *h = cb->sourceHeight & ~1u;
  }
  if (cb->sourceInterval100ns > 0) *interval = cb->sourceInterval100ns;
  *pixel = cb->sourcePixel == kPixelP010 ? kPixelP010 : kPixelNv12;
}

void Pin::BuildTypeList(std::vector<AM_MEDIA_TYPE>* out) {
  out->clear();

  uint32_t srcW, srcH, srcPixel;
  int64_t interval;
  CurrentSource(&srcW, &srcH, &interval, &srcPixel);

  // The source's own shape leads, because a consumer that takes the first thing
  // offered should get the picture untouched. That is the whole point of this
  // rewrite: CapView 2.x offered 1080p30, 720p30 and 480p30 and nothing else,
  // so a 576i50 SNES arrived upscaled and stuttering no matter what.
  auto add = [&](uint32_t pixel, uint32_t w, uint32_t h) {
    AM_MEDIA_TYPE mt;
    if (!FillMediaType(&mt, pixel, w, h, interval)) return;
    for (const AM_MEDIA_TYPE& existing : *out) {
      uint32_t ep, ew, eh;
      if (ParseMediaType(&existing, &ep, &ew, &eh, nullptr) && ep == pixel && ew == (w & ~1u) &&
          eh == (h & ~1u)) {
        FreeMediaTypeContents(&mt);
        return;
      }
    }
    out->push_back(mt);
  };

  // Ten bit first when the source is ten bit: a consumer that understands P010
  // should be given the chance before it settles for eight. It is never offered
  // otherwise, because there would be nothing behind it -- an eight bit picture
  // stretched into ten bits is not an HDR picture, it just reads wrong.
  if (srcPixel == kPixelP010) add(kPixelP010, srcW, srcH);
  add(kPixelNv12, srcW, srcH);

  // Then the ordinary sizes, for consumers that read the list rather than the
  // ranges. Nothing larger than the source, because upscaling here would only
  // move work from the consumer to us and lose nothing on the way -- except
  // that consumers with a fixed wish list need their wish to be in the list, so
  // 720p is offered even from a 576i source.
  const uint32_t ceilingW = srcW > 1280 ? srcW : 1280;
  const uint32_t ceilingH = srcH > 720 ? srcH : 720;
  for (const StdSize& s : kOfferedSizes) {
    if (s.w > ceilingW || s.h > ceilingH) continue;
    add(kPixelNv12, s.w, s.h);
  }
}

STDMETHODIMP Pin::EnumMediaTypes(IEnumMediaTypes** out) {
  if (!out) return E_POINTER;
  *out = new (std::nothrow) MediaTypeEnumerator(this, 0);
  return *out ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Pin::QueryAccept(const AM_MEDIA_TYPE* mt) {
  uint32_t pixel, w, h;
  int64_t interval;
  if (!ParseMediaType(mt, &pixel, &w, &h, &interval)) return S_FALSE;

  uint32_t srcW, srcH, srcPixel;
  int64_t srcInterval;
  CurrentSource(&srcW, &srcH, &srcInterval, &srcPixel);
  if (pixel == kPixelP010 && srcPixel != kPixelP010) return S_FALSE;

  // Anything from a postage stamp up to the source's own size, or 1080p when
  // the source is smaller than that -- a consumer asking for more pixels than
  // exist gets them, it just gets them stretched.
  const uint32_t maxW = srcW > 1920 ? srcW : 1920;
  const uint32_t maxH = srcH > 1080 ? srcH : 1080;
  if (w < 32 || h < 32 || w > maxW || h > maxH) return S_FALSE;

  // Slower than the source is fine, and is how Discord gets its 30 out of a
  // 59.94 signal. Faster is refused rather than faked: there is no picture to
  // put in the extra frames, and a consumer told it is getting 120 would be
  // told a lie 60 times a second.
  if (interval < srcInterval || interval > kSlowestInterval) return S_FALSE;
  return S_OK;
}

HRESULT Pin::AgreeFormat(const AM_MEDIA_TYPE* mt) {
  uint32_t pixel, w, h;
  int64_t interval;
  if (!ParseMediaType(mt, &pixel, &w, &h, &interval)) return VFW_E_TYPE_NOT_ACCEPTED;
  if (QueryAccept(mt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;

  if (haveCurrent_) FreeMediaTypeContents(&current_);
  haveCurrent_ = CopyMediaType(&current_, mt);
  if (!haveCurrent_) return E_OUTOFMEMORY;

  width_ = w;
  height_ = h;
  pixel_ = pixel;
  interval_ = interval;
  return S_OK;
}

STDMETHODIMP Pin::Connect(IPin* receive, const AM_MEDIA_TYPE* mt) {
  if (!receive) return E_POINTER;
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (connected_) return VFW_E_ALREADY_CONNECTED;

  // An offered type is taken as an instruction; without one, the list is walked
  // from the top, which puts the source's native shape first.
  std::vector<AM_MEDIA_TYPE> candidates;
  bool ownCandidates = false;
  AM_MEDIA_TYPE offered = {};
  if (mt && mt->majortype == MEDIATYPE_Video && mt->pbFormat) {
    if (!CopyMediaType(&offered, mt)) return E_OUTOFMEMORY;
    candidates.push_back(offered);
  } else if (haveCurrent_) {
    // SetFormat was called before connecting, which is how most capture
    // applications drive this. That choice stands.
    AM_MEDIA_TYPE copy = {};
    if (!CopyMediaType(&copy, &current_)) return E_OUTOFMEMORY;
    candidates.push_back(copy);
  } else {
    BuildTypeList(&candidates);
    ownCandidates = true;
  }
  (void)ownCandidates;

  HRESULT hr = VFW_E_NO_ACCEPTABLE_TYPES;
  for (AM_MEDIA_TYPE& candidate : candidates) {
    if (QueryAccept(&candidate) != S_OK) continue;
    hr = receive->ReceiveConnection(static_cast<IPin*>(this), &candidate);
    if (FAILED(hr)) continue;

    hr = AgreeFormat(&candidate);
    if (FAILED(hr)) {
      receive->Disconnect();
      continue;
    }

    IMemInputPin* input = nullptr;
    hr = receive->QueryInterface(IID_IMemInputPin, (void**)&input);
    if (FAILED(hr)) {
      receive->Disconnect();
      continue;
    }
    hr = NegotiateAllocator(input);
    if (FAILED(hr)) {
      input->Release();
      receive->Disconnect();
      continue;
    }

    connected_ = receive;
    connected_->AddRef();
    memInput_ = input;
    break;
  }

  for (AM_MEDIA_TYPE& candidate : candidates) FreeMediaTypeContents(&candidate);
  return connected_ ? S_OK : hr;
}

HRESULT Pin::NegotiateAllocator(IMemInputPin* input) {
  IMemAllocator* allocator = nullptr;
  // The downstream filter's own allocator is preferred: it may have reasons of
  // its own for wanting the buffers where they are.
  HRESULT hr = input->GetAllocator(&allocator);
  if (FAILED(hr) || !allocator) {
    hr = ::CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IMemAllocator, (void**)&allocator);
    if (FAILED(hr)) return hr;
  }

  ALLOCATOR_PROPERTIES want = {};
  want.cBuffers = 4;
  want.cbBuffer = (long)ImageBytes(pixel_, width_, height_);
  want.cbAlign = 1;
  want.cbPrefix = 0;

  ALLOCATOR_PROPERTIES request = want;
  input->GetAllocatorRequirements(&request);
  if (request.cBuffers > want.cBuffers) want.cBuffers = request.cBuffers;
  if (request.cbAlign > 0) want.cbAlign = request.cbAlign;
  if (request.cbPrefix > 0) want.cbPrefix = request.cbPrefix;

  ALLOCATOR_PROPERTIES actual = {};
  hr = allocator->SetProperties(&want, &actual);
  if (FAILED(hr) || actual.cbBuffer < want.cbBuffer) {
    allocator->Release();
    return FAILED(hr) ? hr : E_FAIL;
  }
  hr = input->NotifyAllocator(allocator, FALSE);
  if (FAILED(hr)) {
    allocator->Release();
    return hr;
  }
  allocator_ = allocator;
  return S_OK;
}

STDMETHODIMP Pin::Disconnect() {
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (allocator_) {
    allocator_->Decommit();
    allocator_->Release();
    allocator_ = nullptr;
  }
  if (memInput_) {
    memInput_->Release();
    memInput_ = nullptr;
  }
  if (connected_) {
    connected_->Release();
    connected_ = nullptr;
  }
  return S_OK;
}

STDMETHODIMP Pin::ConnectedTo(IPin** pin) {
  if (!pin) return E_POINTER;
  *pin = connected_;
  if (!connected_) return VFW_E_NOT_CONNECTED;
  connected_->AddRef();
  return S_OK;
}

STDMETHODIMP Pin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return E_POINTER;
  if (!connected_ || !haveCurrent_) {
    ::ZeroMemory(mt, sizeof(*mt));
    return VFW_E_NOT_CONNECTED;
  }
  return CopyMediaType(mt, &current_) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Pin::QueryPinInfo(PIN_INFO* info) {
  if (!info) return E_POINTER;
  info->pFilter = static_cast<IBaseFilter*>(owner_);
  info->pFilter->AddRef();
  info->dir = PINDIR_OUTPUT;
  ::StringCchCopyW(info->achName, MAX_PIN_NAME, L"Capture");
  return S_OK;
}

STDMETHODIMP Pin::QueryDirection(PIN_DIRECTION* dir) {
  if (!dir) return E_POINTER;
  *dir = PINDIR_OUTPUT;
  return S_OK;
}

STDMETHODIMP Pin::QueryId(LPWSTR* id) {
  if (!id) return E_POINTER;
  const wchar_t kId[] = L"Capture";
  *id = (LPWSTR)::CoTaskMemAlloc(sizeof(kId));
  if (!*id) return E_OUTOFMEMORY;
  ::memcpy(*id, kId, sizeof(kId));
  return S_OK;
}

STDMETHODIMP Pin::QueryInternalConnections(IPin**, ULONG* count) {
  if (count) *count = 0;
  return E_NOTIMPL;
}

// ------------------------------------------------------- IAMStreamConfig

STDMETHODIMP Pin::SetFormat(AM_MEDIA_TYPE* mt) {
  if (!mt) return E_POINTER;
  if (owner_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (connected_) return VFW_E_ALREADY_CONNECTED;
  return AgreeFormat(mt);
}

STDMETHODIMP Pin::GetFormat(AM_MEDIA_TYPE** mt) {
  if (!mt) return E_POINTER;
  *mt = (AM_MEDIA_TYPE*)::CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!*mt) return E_OUTOFMEMORY;

  if (haveCurrent_) {
    if (!CopyMediaType(*mt, &current_)) {
      ::CoTaskMemFree(*mt);
      *mt = nullptr;
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }

  uint32_t srcW, srcH, srcPixel;
  int64_t interval;
  CurrentSource(&srcW, &srcH, &interval, &srcPixel);
  if (!FillMediaType(*mt, srcPixel, srcW, srcH, interval)) {
    ::CoTaskMemFree(*mt);
    *mt = nullptr;
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP Pin::GetNumberOfCapabilities(int* count, int* size) {
  if (!count || !size) return E_POINTER;
  std::vector<AM_MEDIA_TYPE> types;
  BuildTypeList(&types);
  *count = (int)types.size();
  *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
  for (AM_MEDIA_TYPE& mt : types) FreeMediaTypeContents(&mt);
  return S_OK;
}

STDMETHODIMP Pin::GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) {
  if (!mt || !caps) return E_POINTER;
  if (index < 0) return E_INVALIDARG;

  std::vector<AM_MEDIA_TYPE> types;
  BuildTypeList(&types);
  if (index >= (int)types.size()) {
    for (AM_MEDIA_TYPE& t : types) FreeMediaTypeContents(&t);
    return S_FALSE;
  }

  *mt = (AM_MEDIA_TYPE*)::CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  const bool copied = *mt && CopyMediaType(*mt, &types[index]);
  uint32_t pixel = kPixelNv12, w = kIdleWidth, h = kIdleHeight;
  ParseMediaType(&types[index], &pixel, &w, &h, nullptr);
  for (AM_MEDIA_TYPE& t : types) FreeMediaTypeContents(&t);
  if (!copied) {
    if (*mt) ::CoTaskMemFree(*mt);
    *mt = nullptr;
    return E_OUTOFMEMORY;
  }

  uint32_t srcW, srcH, srcPixel;
  int64_t srcInterval;
  CurrentSource(&srcW, &srcH, &srcInterval, &srcPixel);

  // The range, which is the reason this filter exists in the shape it does. A
  // consumer that understands VIDEO_STREAM_CONFIG_CAPS never has to find its
  // wish in a list: it states what it wants and gets it, scaled here, in its
  // own process, at no cost to anybody else reading the same camera.
  auto* c = (VIDEO_STREAM_CONFIG_CAPS*)caps;
  ::ZeroMemory(c, sizeof(*c));
  c->guid = FORMAT_VideoInfo;
  c->VideoStandard = AnalogVideo_None;
  c->InputSize.cx = (LONG)srcW;
  c->InputSize.cy = (LONG)srcH;
  c->MinCroppingSize = c->InputSize;
  c->MaxCroppingSize = c->InputSize;
  c->CropGranularityX = 1;
  c->CropGranularityY = 1;
  c->CropAlignX = 1;
  c->CropAlignY = 1;
  c->MinOutputSize.cx = 32;
  c->MinOutputSize.cy = 32;
  c->MaxOutputSize.cx = (LONG)(srcW > 1920 ? srcW : 1920);
  c->MaxOutputSize.cy = (LONG)(srcH > 1080 ? srcH : 1080);
  // Two, because chroma is half resolution in both directions and an odd edge
  // has no chroma sample to call its own.
  c->OutputGranularityX = 2;
  c->OutputGranularityY = 2;
  c->StretchTapsX = 2;
  c->StretchTapsY = 2;
  c->ShrinkTapsX = 2;
  c->ShrinkTapsY = 2;
  c->MinFrameInterval = srcInterval;  // shortest interval: the source's own rate
  c->MaxFrameInterval = kSlowestInterval;
  c->MinBitsPerSecond = 1;
  c->MaxBitsPerSecond =
      (LONG)((uint64_t)ImageBytes(pixel, w, h) * 8ull * 10000000ull / (uint64_t)srcInterval);
  return S_OK;
}

// -------------------------------------------------------- IKsPropertySet

STDMETHODIMP Pin::Set(REFGUID, DWORD, void*, DWORD, void*, DWORD) { return E_NOTIMPL; }

STDMETHODIMP Pin::Get(REFGUID set, DWORD id, void*, DWORD, void* data, DWORD length,
                      DWORD* returned) {
  if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
  if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (returned) *returned = sizeof(GUID);
  if (!data) return S_OK;
  if (length < sizeof(GUID)) return E_UNEXPECTED;
  *(GUID*)data = PIN_CATEGORY_CAPTURE;
  return S_OK;
}

STDMETHODIMP Pin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
  if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
  if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (support) *support = KSPROPERTY_SUPPORT_GET;
  return S_OK;
}

// ---------------------------------------------------------------- streaming

HRESULT Pin::Active() {
  if (!connected_ || !allocator_ || thread_) return S_OK;

  HRESULT hr = allocator_->Commit();
  if (FAILED(hr)) return hr;

  ::ResetEvent(quit_);
  thread_ = ::CreateThread(nullptr, 0, &Pin::ThreadEntry, this, 0, nullptr);
  if (!thread_) {
    allocator_->Decommit();
    return HRESULT_FROM_WIN32(::GetLastError());
  }
  return S_OK;
}

HRESULT Pin::Inactive() {
  if (thread_) {
    ::SetEvent(quit_);
    if (wake_) ::SetEvent(wake_);
    ::WaitForSingleObject(thread_, 5000);
    ::CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (allocator_) allocator_->Decommit();
  return S_OK;
}

DWORD WINAPI Pin::ThreadEntry(void* self) {
  ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  static_cast<Pin*>(self)->ThreadLoop();
  ::CoUninitialize();
  return 0;
}

void Pin::ThreadLoop() {
  ConsumerSlot* slot = owner_->ClaimSlot(&reader_);
  if (slot) {
    slot->width = width_;
    slot->height = height_;
    slot->pixel = pixel_;
    slot->frameInterval100ns = interval_;
    slot->streaming = 1;
  }
  if (slot && !wake_) {
    // The slot's own event, so a published picture wakes every consumer once.
    // A single shared event cannot do that -- whoever waited first would eat it.
    ControlBlock* cb = reader_.control();
    if (cb) {
      wchar_t name[128];
      FormatName(name, 128, kWakeEventPrefix,
                 (uint32_t)(slot - cb->consumers));
      wake_ = ::CreateEventW(nullptr, FALSE, FALSE, name);
    }
  }

  LARGE_INTEGER frequency = {};
  ::QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER now = {};
  ::QueryPerformanceCounter(&now);

  const int64_t tickPerFrame = frequency.QuadPart * interval_ / 10000000;
  int64_t deadline = now.QuadPart;
  int64_t streamTime = 0;
  uint32_t lastFrameIndex = 0;
  bool haveLast = false;
  bool first = true;
  uint32_t served = 0;
  uint32_t fresh = 0;

  HANDLE waits[2] = {quit_, wake_};

  for (;;) {
    ::QueryPerformanceCounter(&now);
    int64_t remaining = deadline - now.QuadPart;
    if (remaining > 0) {
      DWORD ms = (DWORD)(remaining * 1000 / frequency.QuadPart);
      if (ms > 100) ms = 100;  // so a stopped camera still notices the quit
      const DWORD count = wake_ ? 2 : 1;
      const DWORD hit = ::WaitForMultipleObjects(count, waits, FALSE, ms);
      if (hit == WAIT_OBJECT_0) break;
      // Woken by a new picture is not a reason to deliver early: the consumer
      // asked for a rate and gets that rate. It only means the wait ends.
      continue;
    }
    if (::WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0) break;

    IMediaSample* sample = nullptr;
    HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
    if (FAILED(hr) || !sample) break;

    BYTE* buffer = nullptr;
    if (SUCCEEDED(sample->GetPointer(&buffer)) && buffer) {
      uint32_t frameIndex = 0;
      const bool got = reader_.ReadInto(buffer, width_, height_, pixel_, &frameIndex);
      if (!got) {
        if (reader_.ProducerAlive()) {
          FrameReader::Blank(buffer, width_, height_, pixel_);
        } else {
          reader_.Idle(buffer, width_, height_, pixel_);
        }
        haveLast = false;
      } else {
        if (!haveLast || frameIndex != lastFrameIndex) ++fresh;
        lastFrameIndex = frameIndex;
        haveLast = true;
      }
    }

    sample->SetActualDataLength((long)ImageBytes(pixel_, width_, height_));
    REFERENCE_TIME start = streamTime;
    REFERENCE_TIME stop = streamTime + interval_;
    sample->SetTime(&start, &stop);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(first ? TRUE : FALSE);
    sample->SetPreroll(FALSE);
    first = false;

    hr = memInput_->Receive(sample);
    sample->Release();
    ++served;
    streamTime += interval_;

    if (slot) {
      slot->framesServed = served;
      slot->framesFresh = fresh;
      slot->lastSeenMs = ::GetTickCount();
    }
    if (FAILED(hr)) break;

    deadline += tickPerFrame;
    // Falling far enough behind that catching up would mean a burst -- the
    // machine went to sleep, or the consumer blocked for a second. Start over
    // from now rather than delivering the backlog as fast as the loop can run.
    ::QueryPerformanceCounter(&now);
    if (deadline < now.QuadPart - tickPerFrame * 4) deadline = now.QuadPart + tickPerFrame;
  }

  if (slot) {
    slot->streaming = 0;
    slot->lastSeenMs = ::GetTickCount();
  }
  owner_->ReleaseSlot();
  if (wake_) {
    ::CloseHandle(wake_);
    wake_ = nullptr;
  }
}

// =========================================================== Filter methods

Filter::Filter() {
  ::InitializeCriticalSection(&lock_);
  pin_ = new (std::nothrow) Pin(this);
  ::StringCchCopyW(name_, MAX_FILTER_NAME, kFilterName);
  AddLiveObject();
}

Filter::~Filter() {
  delete pin_;
  if (clock_) clock_->Release();
  ::DeleteCriticalSection(&lock_);
  ReleaseLiveObject();
}

STDMETHODIMP Filter::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter ||
      riid == IID_IPersist) {
    *out = static_cast<IBaseFilter*>(this);
  } else if (riid == IID_IAMFilterMiscFlags) {
    *out = static_cast<IAMFilterMiscFlags*>(this);
  } else {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) Filter::AddRef() { return ::InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) Filter::Release() {
  const long n = ::InterlockedDecrement(&refs_);
  if (n == 0) delete this;
  return n;
}

STDMETHODIMP Filter::GetClassID(CLSID* id) {
  if (!id) return E_POINTER;
  *id = CLSID_CapViewFilter;
  return S_OK;
}

STDMETHODIMP Filter::Stop() {
  ::EnterCriticalSection(&lock_);
  if (state_ != State_Stopped && pin_) pin_->Inactive();
  state_ = State_Stopped;
  ::LeaveCriticalSection(&lock_);
  return S_OK;
}

STDMETHODIMP Filter::Pause() {
  ::EnterCriticalSection(&lock_);
  // A live source has nothing to hold still for. Streaming starts here rather
  // than in Run because a graph pauses first and expects the first sample to be
  // on its way before it ever calls Run.
  if (state_ == State_Stopped && pin_) pin_->Active();
  state_ = State_Paused;
  ::LeaveCriticalSection(&lock_);
  return S_OK;
}

STDMETHODIMP Filter::Run(REFERENCE_TIME) {
  ::EnterCriticalSection(&lock_);
  if (state_ == State_Stopped && pin_) pin_->Active();
  state_ = State_Running;
  ::LeaveCriticalSection(&lock_);
  return S_OK;
}

STDMETHODIMP Filter::GetState(DWORD, FILTER_STATE* state) {
  if (!state) return E_POINTER;
  *state = state_;
  return S_OK;
}

STDMETHODIMP Filter::SetSyncSource(IReferenceClock* clock) {
  ::EnterCriticalSection(&lock_);
  if (clock) clock->AddRef();
  if (clock_) clock_->Release();
  clock_ = clock;
  ::LeaveCriticalSection(&lock_);
  return S_OK;
}

STDMETHODIMP Filter::GetSyncSource(IReferenceClock** clock) {
  if (!clock) return E_POINTER;
  *clock = clock_;
  if (clock_) clock_->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::EnumPins(IEnumPins** out) {
  if (!out) return E_POINTER;
  *out = new (std::nothrow) PinEnumerator(this, 0);
  return *out ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Filter::FindPin(LPCWSTR id, IPin** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!id || ::lstrcmpW(id, L"Capture") != 0) return VFW_E_NOT_FOUND;
  *out = static_cast<IPin*>(pin_);
  (*out)->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* info) {
  if (!info) return E_POINTER;
  ::StringCchCopyW(info->achName, MAX_FILTER_NAME, name_);
  info->pGraph = graph_;
  if (graph_) graph_->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
  ::EnterCriticalSection(&lock_);
  // Held weakly on purpose: the graph owns the filter, and holding a reference
  // back would mean neither ever goes away.
  graph_ = graph;
  if (name) ::StringCchCopyW(name_, MAX_FILTER_NAME, name);
  ::LeaveCriticalSection(&lock_);
  return S_OK;
}

STDMETHODIMP Filter::QueryVendorInfo(LPWSTR* vendor) {
  if (!vendor) return E_POINTER;
  *vendor = nullptr;
  return E_NOTIMPL;
}

ConsumerSlot* Filter::ClaimSlot(FrameReader* reader) {
  if (haveSlot_ && slotOwner_) return &slotOwner_->consumers[slotIndex_];
  if (!reader->EnsureControl()) return nullptr;
  ControlBlock* cb = reader->control();
  if (!cb) return nullptr;

  for (uint32_t i = 0; i < kConsumerSlots; ++i) {
    if (::InterlockedCompareExchange((volatile LONG*)&cb->consumers[i].inUse, 1, 0) != 0) {
      continue;
    }
    ConsumerSlot& s = cb->consumers[i];
    s.pid = ::GetCurrentProcessId();

    // The consuming application's name, which DirectShow gives away for free:
    // this code is running inside it. The Media Foundation source could never
    // know this -- it lived in a service and every consumer looked the same
    // from in there.
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p; ++p) {
      if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    ::StringCchCopyW(s.name, 64, leaf);

    s.framesServed = 0;
    s.framesFresh = 0;
    s.streaming = 0;
    s.frameInterval100ns = 0;
    s.lastSeenMs = ::GetTickCount();

    slotOwner_ = cb;
    slotIndex_ = i;
    haveSlot_ = true;
    return &s;
  }
  return nullptr;
}

void Filter::ReleaseSlot() {
  if (!haveSlot_ || !slotOwner_) return;
  ConsumerSlot& s = slotOwner_->consumers[slotIndex_];
  s.streaming = 0;
  s.framesServed = 0;
  ::InterlockedExchange((volatile LONG*)&s.inUse, 0);
  haveSlot_ = false;
  slotOwner_ = nullptr;
}

// ============================================================ class factory

namespace {

class ClassFactory : public IClassFactory {
 public:
  ClassFactory() { AddLiveObject(); }
  ~ClassFactory() { ReleaseLiveObject(); }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *out = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const long n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;

    auto* filter = new (std::nothrow) Filter();
    if (!filter) return E_OUTOFMEMORY;
    if (!filter->pin()) {
      filter->Release();
      return E_OUTOFMEMORY;
    }
    const HRESULT hr = filter->QueryInterface(riid, out);
    filter->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      AddLiveObject();
    } else {
      ReleaseLiveObject();
    }
    return S_OK;
  }

 private:
  long refs_ = 1;
};

}  // namespace

HRESULT CreateFilterClassFactory(REFIID riid, void** out) {
  auto* factory = new (std::nothrow) ClassFactory();
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, out);
  factory->Release();
  return hr;
}

long LiveObjectCount() { return g_liveObjects; }
void AddLiveObject() { ::InterlockedIncrement(&g_liveObjects); }
void ReleaseLiveObject() { ::InterlockedDecrement(&g_liveObjects); }

}  // namespace vcam
}  // namespace cap
