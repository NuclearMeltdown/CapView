#include "capture/dshow_util.h"

#include "i18n.h"

#include <dvdmedia.h>  // VIDEOINFOHEADER2

#include <algorithm>
#include <cmath>
#include <set>

namespace cap {
namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return (uint32_t)(unsigned char)a | ((uint32_t)(unsigned char)b << 8) |
         ((uint32_t)(unsigned char)c << 16) | ((uint32_t)(unsigned char)d << 24);
}

// FOURCC based media subtypes all share the same GUID tail.
constexpr GUID FourccGuid(uint32_t fourcc) {
  return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

// How the bytes of one frame are arranged, which is all that is needed to work
// out stride and total size.
enum class Layout {
  Packed,      // one row of bpp bits per pixel, no padding
  Planar420,   // full res luma plane, then half res chroma
  Rgb,         // like Packed, but rows padded to 4 bytes like a DIB
  Compressed,  // size comes from the sample
};

struct SubtypeInfo {
  const char* label;
  GUID guid;
  int bpp;          // 0 for compressed
  Layout layout;
  bool rendererOk;  // can the D3D renderer upload it directly
};

// Order matters only for the reverse lookup; the UI keeps driver order.
// The 10 and 16 bit entries are listed so that a card offering them gets a
// readable label and a correct frame size instead of a hex FOURCC -- the
// renderer cannot draw them yet, so selecting one falls back to a format it can.
const SubtypeInfo kSubtypes[] = {
    {"YUY2", FourccGuid(Fourcc('Y', 'U', 'Y', '2')), 16, Layout::Packed, true},
    {"UYVY", FourccGuid(Fourcc('U', 'Y', 'V', 'Y')), 16, Layout::Packed, true},
    {"YVYU", FourccGuid(Fourcc('Y', 'V', 'Y', 'U')), 16, Layout::Packed, true},
    {"HDYC", FourccGuid(Fourcc('H', 'D', 'Y', 'C')), 16, Layout::Packed, true},
    {"NV12", FourccGuid(Fourcc('N', 'V', '1', '2')), 12, Layout::Planar420, true},
    {"YV12", FourccGuid(Fourcc('Y', 'V', '1', '2')), 12, Layout::Planar420, true},
    {"I420", FourccGuid(Fourcc('I', '4', '2', '0')), 12, Layout::Planar420, true},
    {"IYUV", FourccGuid(Fourcc('I', 'Y', 'U', 'V')), 12, Layout::Planar420, true},
    // 10 and 16 bit, the formats an HDR capable card delivers.
    {"P010", FourccGuid(Fourcc('P', '0', '1', '0')), 24, Layout::Planar420, true},
    {"P016", FourccGuid(Fourcc('P', '0', '1', '6')), 24, Layout::Planar420, true},
    {"Y210", FourccGuid(Fourcc('Y', '2', '1', '0')), 32, Layout::Packed, false},
    {"Y216", FourccGuid(Fourcc('Y', '2', '1', '6')), 32, Layout::Packed, false},
    {"Y410", FourccGuid(Fourcc('Y', '4', '1', '0')), 32, Layout::Packed, false},
    {"v210", FourccGuid(Fourcc('v', '2', '1', '0')), 0, Layout::Compressed, false},
    {"r210", FourccGuid(Fourcc('r', '2', '1', '0')), 32, Layout::Packed, false},
    {"MJPG", FourccGuid(Fourcc('M', 'J', 'P', 'G')), 0, Layout::Compressed, false},
    {"dvsd", FourccGuid(Fourcc('d', 'v', 's', 'd')), 0, Layout::Compressed, false},
    {"H264", FourccGuid(Fourcc('H', '2', '6', '4')), 0, Layout::Compressed, false},
    {"HEVC", FourccGuid(Fourcc('H', 'E', 'V', 'C')), 0, Layout::Compressed, false},
};

const SubtypeInfo* FindSubtype(const GUID& g) {
  for (const SubtypeInfo& s : kSubtypes) {
    if (IsEqualGUID(s.guid, g)) return &s;
  }
  return nullptr;
}

bool IsRgbSubtype(const GUID& g) {
  return IsEqualGUID(g, MEDIASUBTYPE_RGB24) || IsEqualGUID(g, MEDIASUBTYPE_RGB32) ||
         IsEqualGUID(g, MEDIASUBTYPE_ARGB32) || IsEqualGUID(g, MEDIASUBTYPE_RGB565) ||
         IsEqualGUID(g, MEDIASUBTYPE_RGB555);
}

// Resolutions offered on top of what the driver lists.
struct StdRes {
  int w, h;
};
const StdRes kStandardResolutions[] = {
    {320, 240},   {640, 360},   {640, 480},   {720, 480},   {720, 576},   {800, 600},
    {960, 540},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1360, 768},
    {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
    {3840, 2160},
};

// DirectShow itself has no frame rate ceiling worth speaking of -- it stores the
// interval in 100 ns units -- so this list only has to cover what hardware and
// monitors actually run at, including the high refresh rates.
const double kStandardFps[] = {23.976, 24.0,  25.0,  29.97, 30.0,  48.0,  50.0,
                               59.94,  60.0,  72.0,  75.0,  90.0,  100.0, 119.88,
                               120.0,  144.0, 165.0, 180.0, 200.0, 240.0};

bool FpsNear(double a, double b) {
  return std::fabs(a - b) < 0.05;
}

double FpsFromInterval(REFERENCE_TIME interval) {
  if (interval <= 0) return 0.0;
  return 10000000.0 / (double)interval;
}

REFERENCE_TIME IntervalFromFps(double fps) {
  if (fps <= 0.0) return 0;
  return (REFERENCE_TIME)llround(10000000.0 / fps);
}

std::string ReadBagString(IPropertyBag* bag, const wchar_t* key) {
  VARIANT var;
  ::VariantInit(&var);
  std::string out;
  if (SUCCEEDED(bag->Read(key, &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal) {
    out = ToUtf8(var.bstrVal);
  }
  ::VariantClear(&var);
  return out;
}

}  // namespace

// ------------------------------------------------------------- media subtypes

std::string SubtypeLabel(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->label;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return "RGB24";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) return "RGB32";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return "ARGB32";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB565)) return "RGB565";
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB555)) return "RGB555";

  // Unknown: if the first four bytes look like a printable FOURCC, show that.
  const unsigned char* p = (const unsigned char*)&subtype.Data1;
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) printable = false;
  }
  if (printable) return std::string((const char*)p, 4);
  return Format("%08X", (unsigned)subtype.Data1);
}

bool SubtypeFromLabel(const std::string& label, GUID* out) {
  for (const SubtypeInfo& s : kSubtypes) {
    if (label == s.label) {
      *out = s.guid;
      return true;
    }
  }
  if (label == "RGB24") { *out = MEDIASUBTYPE_RGB24; return true; }
  if (label == "RGB32") { *out = MEDIASUBTYPE_RGB32; return true; }
  if (label == "ARGB32") { *out = MEDIASUBTYPE_ARGB32; return true; }
  if (label == "RGB565") { *out = MEDIASUBTYPE_RGB565; return true; }
  if (label == "RGB555") { *out = MEDIASUBTYPE_RGB555; return true; }
  return false;
}

bool IsRendererSubtype(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->rendererOk;
  return IsEqualGUID(subtype, MEDIASUBTYPE_RGB24) || IsEqualGUID(subtype, MEDIASUBTYPE_RGB32) ||
         IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32);
}

int BitsPerPixelForSubtype(const GUID& subtype) {
  if (const SubtypeInfo* s = FindSubtype(subtype)) return s->bpp;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return 24;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32) || IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return 32;
  if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB565) || IsEqualGUID(subtype, MEDIASUBTYPE_RGB555)) return 16;
  return 0;
}

size_t ImageSizeForSubtype(const GUID& subtype, int width, int height, int* strideOut) {
  if (width <= 0 || height <= 0) {
    if (strideOut) *strideOut = 0;
    return 0;
  }
  const int bpp = BitsPerPixelForSubtype(subtype);
  Layout layout = Layout::Compressed;
  if (const SubtypeInfo* s = FindSubtype(subtype)) {
    layout = s->layout;
  } else if (IsRgbSubtype(subtype)) {
    layout = Layout::Rgb;
  }

  int stride = 0;
  size_t size = 0;
  switch (layout) {
    case Layout::Rgb:
      // DIB rows are padded to a 4 byte boundary.
      stride = ((width * bpp + 31) / 32) * 4;
      size = (size_t)stride * (size_t)height;
      break;
    case Layout::Packed:
      stride = width * bpp / 8;
      size = (size_t)stride * (size_t)height;
      break;
    case Layout::Planar420:
      // Luma plane stride: one byte per sample at 8 bit, two at 10 or 16.
      stride = width * (bpp > 12 ? 2 : 1);
      size = (size_t)stride * (size_t)height * 3 / 2;
      break;
    case Layout::Compressed:
    default:
      stride = 0;
      size = 0;  // size comes from the sample
      break;
  }
  if (strideOut) *strideOut = stride;
  return size;
}

// -------------------------------------------------------- colour description

const char* NominalRangeName(int value) {
  switch (value) {
    case 1: return "Full (0-255)";
    case 2: return "Limited (16-235)";
    case 3: return "48-208";
    case 4: return "64-127";
    default: return "unbekannt";
  }
}

const char* TransferMatrixName(int value) {
  switch (value) {
    case 1: return "BT.709";
    case 2: return "BT.601";
    case 3: return "SMPTE 240M";
    case 4: return "BT.2020 (10 Bit)";
    case 5: return "BT.2020 (12 Bit)";
    default: return "unbekannt";
  }
}

const char* PrimariesName(int value) {
  switch (value) {
    case 2: return "BT.709";
    case 3: return "BT.470-2 System M";
    case 4: return "BT.470-2 System B/G";
    case 5: return "SMPTE 170M";
    case 6: return "SMPTE 240M";
    case 9: return "BT.2020";
    case 11: return "DCI-P3";
    default: return "unbekannt";
  }
}

const char* TransferFunctionName(int value) {
  switch (value) {
    case 1: return "linear";
    case 4: return "Gamma 2.2";
    case 5: return "BT.709";
    case 7: return "sRGB";
    case 12: return "BT.2020 (konstante Luminanz)";
    case 13: return "BT.2020";
    case 15: return "PQ / ST.2084 (HDR10)";
    case 16: return "HLG (HDR)";
    default: return "unbekannt";
  }
}

// ---------------------------------------------------------------- media types

void FreeMediaType(AM_MEDIA_TYPE& mt) {
  if (mt.cbFormat != 0) {
    ::CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
  }
  if (mt.pUnk) {
    mt.pUnk->Release();
    mt.pUnk = nullptr;
  }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  FreeMediaType(*mt);
  ::CoTaskMemFree(mt);
}

AM_MEDIA_TYPE* CreateMediaTypeCopy(const AM_MEDIA_TYPE* src) {
  if (!src) return nullptr;
  auto* dst = (AM_MEDIA_TYPE*)::CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!dst) return nullptr;
  *dst = *src;
  dst->pbFormat = nullptr;
  dst->pUnk = nullptr;
  if (src->cbFormat && src->pbFormat) {
    dst->pbFormat = (BYTE*)::CoTaskMemAlloc(src->cbFormat);
    if (!dst->pbFormat) {
      ::CoTaskMemFree(dst);
      return nullptr;
    }
    memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  } else {
    dst->cbFormat = 0;
  }
  if (src->pUnk) {
    dst->pUnk = src->pUnk;
    dst->pUnk->AddRef();
  }
  return dst;
}

bool ParseVideoMediaType(const AM_MEDIA_TYPE* mt, VideoFormatInfo* out) {
  if (!mt || !out || !mt->pbFormat) return false;
  if (!IsEqualGUID(mt->majortype, MEDIATYPE_Video)) return false;

  const BITMAPINFOHEADER* bih = nullptr;
  REFERENCE_TIME avgTime = 0;

  if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
    const auto* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
    bih = &vih->bmiHeader;
    avgTime = vih->AvgTimePerFrame;
    out->aspectX = 0;
    out->aspectY = 0;
    out->interlaced = false;
  } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
             mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
    const auto* vih2 = (const VIDEOINFOHEADER2*)mt->pbFormat;
    bih = &vih2->bmiHeader;
    avgTime = vih2->AvgTimePerFrame;
    out->aspectX = (int)vih2->dwPictAspectRatioX;
    out->aspectY = (int)vih2->dwPictAspectRatioY;
    out->interlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
    out->fieldOneFirst = (vih2->dwInterlaceFlags & AMINTERLACE_Field1First) != 0;

    // When this flag is set, dwControlFlags is really a DXVA_ExtendedFormat
    // bitfield. Decoded by hand rather than by casting to the struct: the bit
    // layout is fixed and documented, and this way there is no dependency on
    // which DXVA header happens to be reachable.
    if (vih2->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
      const DWORD f = vih2->dwControlFlags;
      out->colorInfoPresent = true;
      out->nominalRange = (int)((f >> 12) & 0x7);
      out->transferMatrix = (int)((f >> 15) & 0x7);
      out->primaries = (int)((f >> 22) & 0x1F);
      out->transferFunction = (int)((f >> 27) & 0x1F);
    }
  } else {
    return false;
  }

  out->subtype = mt->subtype;
  out->subtypeLabel = SubtypeLabel(mt->subtype);
  out->width = (int)bih->biWidth;
  out->height = (int)std::abs(bih->biHeight);
  out->bottomUp = IsRgbSubtype(mt->subtype) && bih->biHeight > 0;
  out->fps = FpsFromInterval(avgTime);

  int stride = 0;
  size_t computed = ImageSizeForSubtype(mt->subtype, out->width, out->height, &stride);
  out->stride = stride;
  out->imageSize = bih->biSizeImage ? (size_t)bih->biSizeImage : computed;
  // Some drivers report a bogus biSizeImage; trust the computed size when we
  // know the layout and the reported one is clearly too small.
  if (computed && out->imageSize < computed) out->imageSize = computed;

  return out->width > 0 && out->height > 0;
}

// ----------------------------------------------------------------- device enum

namespace {

std::vector<VideoDeviceInfo> EnumerateDShowCategory(const GUID& category) {
  std::vector<VideoDeviceInfo> devices;

  ComPtr<ICreateDevEnum> devEnum;
  if (FAILED(CAP_HR(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&devEnum))))) {
    return devices;
  }

  ComPtr<IEnumMoniker> enumMoniker;
  HRESULT hr = devEnum->CreateClassEnumerator(category, &enumMoniker, 0);
  if (hr != S_OK || !enumMoniker) return devices;  // S_FALSE means no devices

  ComPtr<IMoniker> moniker;
  while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
    VideoDeviceInfo info;

    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
      info.name = ReadBagString(bag.Get(), L"FriendlyName");
      info.id = ReadBagString(bag.Get(), L"DevicePath");
    }

    LPOLESTR display = nullptr;
    if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display)) && display) {
      info.monikerName = ToUtf8(display);
      ::CoTaskMemFree(display);
    }

    if (info.name.empty()) info.name = "Unbenanntes Gerät";
    if (info.id.empty()) info.id = info.monikerName;
    devices.push_back(std::move(info));

    moniker.Reset();
  }
  return devices;
}

}  // namespace

std::vector<VideoDeviceInfo> EnumerateVideoDevices() {
  return EnumerateDShowCategory(CLSID_VideoInputDeviceCategory);
}

std::vector<VideoDeviceInfo> EnumerateAudioCaptureDShowDevices() {
  return EnumerateDShowCategory(CLSID_AudioInputDeviceCategory);
}

ComPtr<IBaseFilter> CreateVideoFilter(const DeviceRef& ref, VideoDeviceInfo* resolved) {
  ComPtr<ICreateDevEnum> devEnum;
  if (FAILED(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum)))) {
    return nullptr;
  }
  ComPtr<IEnumMoniker> enumMoniker;
  if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0) != S_OK) {
    return nullptr;
  }

  // Three passes so an exact id match always wins over a name match.
  struct Candidate {
    ComPtr<IMoniker> moniker;
    VideoDeviceInfo info;
  };
  std::vector<Candidate> candidates;

  ComPtr<IMoniker> moniker;
  while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
    Candidate c;
    c.moniker = moniker;
    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
      c.info.name = ReadBagString(bag.Get(), L"FriendlyName");
      c.info.id = ReadBagString(bag.Get(), L"DevicePath");
    }
    LPOLESTR display = nullptr;
    if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display)) && display) {
      c.info.monikerName = ToUtf8(display);
      ::CoTaskMemFree(display);
    }
    if (c.info.id.empty()) c.info.id = c.info.monikerName;
    candidates.push_back(std::move(c));
    moniker.Reset();
  }

  const Candidate* pick = nullptr;
  if (!ref.id.empty()) {
    for (const Candidate& c : candidates) {
      if (c.info.id == ref.id) { pick = &c; break; }
    }
    if (!pick) {
      for (const Candidate& c : candidates) {
        if (!c.info.monikerName.empty() && c.info.monikerName == ref.id) { pick = &c; break; }
      }
    }
  }
  if (!pick && !ref.name.empty()) {
    for (const Candidate& c : candidates) {
      if (c.info.name == ref.name) { pick = &c; break; }
    }
  }
  if (!pick) return nullptr;

  ComPtr<IBaseFilter> filter;
  if (FAILED(CAP_HR(pick->moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))))) {
    return nullptr;
  }
  if (resolved) *resolved = pick->info;
  return filter;
}

ComPtr<IBaseFilter> CreateFilterFromMoniker(const VideoDeviceInfo& info) {
  if (info.monikerName.empty()) return nullptr;

  ComPtr<IBindCtx> bindCtx;
  if (FAILED(CAP_HR(::CreateBindCtx(0, &bindCtx)))) return nullptr;

  ComPtr<IMoniker> moniker;
  ULONG eaten = 0;
  std::wstring display = ToWide(info.monikerName);
  if (FAILED(CAP_HR(::MkParseDisplayName(bindCtx.Get(), display.c_str(), &eaten, &moniker)))) {
    return nullptr;
  }

  ComPtr<IBaseFilter> filter;
  if (FAILED(CAP_HR(moniker->BindToObject(bindCtx.Get(), nullptr, IID_PPV_ARGS(&filter))))) {
    return nullptr;
  }
  return filter;
}

ComPtr<IPin> FindPinByDirection(IBaseFilter* filter, PIN_DIRECTION dir, int skip) {
  if (!filter) return nullptr;
  ComPtr<IEnumPins> pins;
  if (FAILED(filter->EnumPins(&pins))) return nullptr;
  ComPtr<IPin> pin;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) {
      if (skip == 0) return pin;
      --skip;
    }
    pin.Reset();
  }
  return nullptr;
}

ComPtr<IPin> FindCapturePin(ICaptureGraphBuilder2* builder, IBaseFilter* filter) {
  if (!filter) return nullptr;
  if (builder) {
    ComPtr<IPin> pin;
    if (SUCCEEDED(builder->FindPin(filter, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   FALSE, 0, &pin)) &&
        pin) {
      return pin;
    }
  }
  // Fallback: first output pin that exposes IAMStreamConfig.
  ComPtr<IEnumPins> pins;
  if (FAILED(filter->EnumPins(&pins))) return nullptr;
  ComPtr<IPin> pin;
  ComPtr<IPin> firstOutput;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == PINDIR_OUTPUT) {
      if (!firstOutput) firstOutput = pin;
      ComPtr<IAMStreamConfig> cfg;
      if (SUCCEEDED(pin.As(&cfg))) return pin;
    }
    pin.Reset();
  }
  return firstOutput;
}

// ------------------------------------------------------------------ capability

std::vector<CapsEntry> EnumerateCaps(IPin* capturePin) {
  std::vector<CapsEntry> out;
  if (!capturePin) return out;

  ComPtr<IAMStreamConfig> cfg;
  if (FAILED(capturePin->QueryInterface(IID_PPV_ARGS(&cfg)))) return out;

  int count = 0, size = 0;
  if (FAILED(cfg->GetNumberOfCapabilities(&count, &size))) return out;
  if (size != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
    CAP_WARN("Unerwartete Größe von VIDEO_STREAM_CONFIG_CAPS (%d), Caps werden übersprungen",
             size);
    return out;
  }

  for (int i = 0; i < count; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    VIDEO_STREAM_CONFIG_CAPS vscc = {};
    if (FAILED(cfg->GetStreamCaps(i, &mt, (BYTE*)&vscc)) || !mt) continue;

    VideoFormatInfo info;
    if (ParseVideoMediaType(mt, &info)) {
      CapsEntry e;
      e.subtype = info.subtype;
      e.subtypeLabel = info.subtypeLabel;
      e.width = info.width;
      e.height = info.height;
      e.defaultFps = info.fps;
      e.minFps = FpsFromInterval(vscc.MaxFrameInterval);
      e.maxFps = FpsFromInterval(vscc.MinFrameInterval);
      e.minWidth = (int)vscc.MinOutputSize.cx;
      e.maxWidth = (int)vscc.MaxOutputSize.cx;
      e.minHeight = (int)vscc.MinOutputSize.cy;
      e.maxHeight = (int)vscc.MaxOutputSize.cy;
      e.granularityX = (int)vscc.OutputGranularityX;
      e.granularityY = (int)vscc.OutputGranularityY;

      // Drivers that fill the caps struct with nonsense: fall back to the
      // media type's own resolution as the only supported one.
      if (e.minWidth <= 0 || e.maxWidth < e.minWidth) {
        e.minWidth = e.maxWidth = e.width;
      }
      if (e.minHeight <= 0 || e.maxHeight < e.minHeight) {
        e.minHeight = e.maxHeight = e.height;
      }
      if (e.maxFps <= 0.0) e.maxFps = e.defaultFps;
      if (e.minFps <= 0.0 || e.minFps > e.maxFps) e.minFps = e.maxFps > 0 ? 1.0 : 0.0;
      if (e.defaultFps <= 0.0) e.defaultFps = e.maxFps;

      out.push_back(std::move(e));
    }
    DeleteMediaType(mt);
  }

  CAP_LOG("Capture-Pin meldet %d Capability-Einträge, %zu davon lesbar", count, out.size());
  return out;
}

// ------------------------------------------------------------------ CapsModel

void CapsModel::Build(std::vector<CapsEntry> entries) {
  // Deduplicate identical (format, resolution, fps) rows -- some drivers list
  // the same combination several times.
  std::vector<CapsEntry> unique;
  for (CapsEntry& e : entries) {
    bool dup = false;
    for (const CapsEntry& u : unique) {
      if (u.subtypeLabel == e.subtypeLabel && u.width == e.width && u.height == e.height &&
          FpsNear(u.defaultFps, e.defaultFps)) {
        dup = true;
        break;
      }
    }
    if (!dup) unique.push_back(std::move(e));
  }
  entries_ = std::move(unique);
}

std::vector<std::string> CapsModel::Subtypes() const {
  std::vector<std::string> out;
  for (const CapsEntry& e : entries_) {
    if (std::find(out.begin(), out.end(), e.subtypeLabel) == out.end()) {
      out.push_back(e.subtypeLabel);
    }
  }
  return out;
}

std::vector<ResolutionOption> CapsModel::Resolutions(const std::string& subtype) const {
  std::vector<ResolutionOption> out;
  int minW = INT32_MAX, maxW = 0, minH = INT32_MAX, maxH = 0;
  bool any = false;
  // Only meaningful when the driver reports a size range rather than a list of
  // fixed sizes. Cards that report min == max per entry support exactly those
  // sizes, and offering anything in between would just be wrong.
  bool scalable = false;

  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    any = true;
    minW = std::min(minW, e.minWidth);
    maxW = std::max(maxW, e.maxWidth);
    minH = std::min(minH, e.minHeight);
    maxH = std::max(maxH, e.maxHeight);
    if (e.minWidth < e.maxWidth || e.minHeight < e.maxHeight) scalable = true;

    bool dup = false;
    for (const ResolutionOption& r : out) {
      if (r.width == e.width && r.height == e.height) { dup = true; break; }
    }
    if (!dup) out.push_back({e.width, e.height, false});
  }
  if (!any) return out;

  // Standard resolutions the driver did not list but that fit inside a range it
  // says it can scale to. Flagged so the UI can mark them as not guaranteed.
  if (scalable) {
    for (const StdRes& r : kStandardResolutions) {
      if (r.w < minW || r.w > maxW || r.h < minH || r.h > maxH) continue;
      bool dup = false;
      for (const ResolutionOption& o : out) {
        if (o.width == r.w && o.height == r.h) { dup = true; break; }
      }
      if (!dup) out.push_back({r.w, r.h, true});
    }
  }

  std::sort(out.begin(), out.end(), [](const ResolutionOption& a, const ResolutionOption& b) {
    if (a.width * a.height != b.width * b.height) {
      return a.width * a.height > b.width * b.height;
    }
    return a.width > b.width;
  });
  return out;
}

namespace {

// The union of the frame-rate ranges the driver reports for one format at one
// resolution. `borrowed` says the numbers came from a different resolution
// because none covered the one asked about -- they are then an educated guess
// rather than a promise, and the UI says so.
struct FpsRange {
  double min = 0.0;
  double max = 0.0;
  bool have = false;
  bool borrowed = false;
};

// Widens `r` to include one entry's range.
void Widen(FpsRange* r, const CapsEntry& e) {
  if (!r->have) {
    r->min = e.minFps;
    r->max = e.maxFps;
    r->have = true;
  } else {
    r->min = std::min(r->min, e.minFps);
    r->max = std::max(r->max, e.maxFps);
  }
}

// What the driver says about this format at this resolution.
FpsRange ReportedRange(const std::vector<CapsEntry>& entries, const std::string& subtype,
                       int width, int height) {
  FpsRange r;
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel != subtype) continue;
    const bool sameRes = (e.width == width && e.height == height);
    const bool coversRes = width >= e.minWidth && width <= e.maxWidth && height >= e.minHeight &&
                           height <= e.maxHeight;
    if (sameRes || coversRes) Widen(&r, e);
  }
  if (r.have) return r;

  // Nothing covers this resolution, so it is one the user forced. Borrow the
  // union of everything reported for the format: an educated guess beats an
  // empty dropdown, as long as it is labelled as one.
  for (const CapsEntry& e : entries) {
    if (e.subtypeLabel == subtype) Widen(&r, e);
  }
  r.borrowed = r.have;
  return r;
}

}  // namespace

std::vector<FpsOption> CapsModel::FpsList(const std::string& subtype, int width, int height) const {
  std::vector<FpsOption> out;
  const FpsRange range = ReportedRange(entries_, subtype, width, height);

  auto known = [&out](double fps) {
    for (const FpsOption& o : out) {
      if (FpsNear(o.fps, fps)) return true;
    }
    return false;
  };

  // Rates named outright for exactly this resolution.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    if (e.defaultFps > 0.0 && !known(e.defaultFps)) out.push_back({e.defaultFps, false, false});
  }

  // Standard rates that fall inside the advertised range. These are not
  // inventions: a DirectShow range is the driver's own claim that it accepts
  // anything between the two ends, and 48 inside 25..59.94 is as much a promise
  // as 25 is. What used to sit here as well -- everything up to twice the
  // advertised maximum, on the theory that a card claiming 30 might secretly do
  // 60 -- is gone. It filled the list with rates no card had ever mentioned,
  // and on an input that reports no range at all it marked every single entry
  // from 25 to 119.88 "not reported", which is a dropdown that tells you
  // nothing. Anyone who wants to gamble on an unlisted rate can still type it
  // in by hand, and then it is their guess rather than ours.
  if (range.have) {
    for (double f : kStandardFps) {
      if (f < range.min - 0.05 || f > range.max + 0.05) continue;
      if (!known(f)) out.push_back({f, range.borrowed, false});
    }
    // The top of the range itself, when no standard rate happened to land on
    // it. A card topping out at 47.5 should still offer 47.5.
    if (range.max > 0.0 && !known(range.max)) out.push_back({range.max, range.borrowed, false});
  }

  std::sort(out.begin(), out.end(),
            [](const FpsOption& a, const FpsOption& b) { return a.fps > b.fps; });

  // Above the numbers, the entry that is not a number. It leads because it is
  // the right answer for almost everyone: it cannot go stale when the console
  // switches from 576i50 to 480p60.
  out.insert(out.begin(), FpsOption{0.0, false, true});
  return out;
}

double CapsModel::HighestFps(const std::string& subtype, int width, int height) const {
  const FpsRange range = ReportedRange(entries_, subtype, width, height);
  double best = range.have ? range.max : 0.0;
  // A discrete entry can name a rate above its own range when a driver fills
  // the two fields inconsistently. Taking the larger keeps "highest" honest.
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    best = std::max(best, e.defaultFps);
  }
  return best;
}

bool CapsModel::IsAdvertised(const std::string& subtype, int width, int height, double fps) const {
  for (const CapsEntry& e : entries_) {
    if (e.subtypeLabel != subtype) continue;
    if (e.width != width || e.height != height) continue;
    // "Highest available" names no rate of its own, so it is advertised exactly
    // when the resolution is -- whatever comes back is the driver's own number.
    if (fps <= 0.0) return true;
    if (FpsNear(e.defaultFps, fps)) return true;
    if (fps >= e.minFps - 0.05 && fps <= e.maxFps + 0.05) return true;
  }
  return false;
}

FormatSel CapsModel::PickDefault(const std::string& preferSubtype) const {
  FormatSel best;
  long long bestScore = -1;
  for (const CapsEntry& e : entries_) {
    // A named subtype beats everything below it, and everything below it still
    // decides between the entries that carry it. The area term tops out around
    // 2^33 at 4K and the renderer bonus sits at 2^40, so 2^42 clears both.
    const long long wishBonus =
        (!preferSubtype.empty() && e.subtypeLabel == preferSubtype) ? 1LL << 42 : 0;
    // Prefer formats the renderer can take without a decoder in the graph.
    const long long formatBonus = IsRendererSubtype(e.subtype) ? 1LL << 40 : 0;
    const double fps = e.maxFps > 0.0 ? e.maxFps : e.defaultFps;
    const long long score =
        wishBonus + formatBonus + (long long)e.width * e.height * 1000 + (long long)(fps * 10);
    if (score > bestScore) {
      bestScore = score;
      best.subtype = e.subtypeLabel;
      best.width = e.width;
      best.height = e.height;
      best.fps = fps;
      best.forced = false;
    }
  }
  return best;
}

// --------------------------------------------------------------- apply format

namespace {

// Rewrites width / height / frame interval in a copied media type.
bool PatchMediaType(AM_MEDIA_TYPE* mt, int width, int height, double fps) {
  if (!mt || !mt->pbFormat) return false;

  BITMAPINFOHEADER* bih = nullptr;
  REFERENCE_TIME* avgTime = nullptr;
  RECT* rcSource = nullptr;
  RECT* rcTarget = nullptr;

  if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
    auto* vih = (VIDEOINFOHEADER*)mt->pbFormat;
    bih = &vih->bmiHeader;
    avgTime = &vih->AvgTimePerFrame;
    rcSource = &vih->rcSource;
    rcTarget = &vih->rcTarget;
  } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
             mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
    auto* vih2 = (VIDEOINFOHEADER2*)mt->pbFormat;
    bih = &vih2->bmiHeader;
    avgTime = &vih2->AvgTimePerFrame;
    rcSource = &vih2->rcSource;
    rcTarget = &vih2->rcTarget;
    // Keep the aspect ratio consistent with the new size when it was set.
    if (vih2->dwPictAspectRatioX && vih2->dwPictAspectRatioY) {
      vih2->dwPictAspectRatioX = (DWORD)width;
      vih2->dwPictAspectRatioY = (DWORD)height;
    }
  } else {
    return false;
  }

  const bool wasBottomUp = bih->biHeight > 0;
  bih->biWidth = width;
  bih->biHeight = wasBottomUp ? height : -height;

  int bpp = BitsPerPixelForSubtype(mt->subtype);
  if (bpp > 0) {
    bih->biBitCount = (WORD)bpp;
    bih->biSizeImage = (DWORD)ImageSizeForSubtype(mt->subtype, width, height, nullptr);
  } else if (bih->biSizeImage == 0) {
    bih->biSizeImage = (DWORD)((size_t)width * height * 2);
  }

  if (fps > 0.0) *avgTime = IntervalFromFps(fps);

  // Empty source/target rectangles mean "whole image", which is what we want.
  *rcSource = RECT{};
  *rcTarget = RECT{};

  mt->lSampleSize = bih->biSizeImage;
  mt->bFixedSizeSamples = bpp > 0 ? TRUE : FALSE;
  return true;
}

}  // namespace

HRESULT ApplyFormat(IPin* capturePin, const FormatSel& fmt, VideoFormatInfo* applied) {
  if (!capturePin || !fmt.valid()) return E_INVALIDARG;

  ComPtr<IAMStreamConfig> cfg;
  HRESULT hr = capturePin->QueryInterface(IID_PPV_ARGS(&cfg));
  if (FAILED(hr)) return hr;

  GUID wantSubtype = GUID_NULL;
  if (!SubtypeFromLabel(fmt.subtype, &wantSubtype)) {
    CAP_WARN("Unbekanntes Farbformat '%s'", fmt.subtype.c_str());
    return E_INVALIDARG;
  }

  int count = 0, size = 0;
  if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) ||
      size != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
    return E_FAIL;
  }

  // Pick a template: exact resolution match first, then any entry with the
  // same subtype. Patching an entry of the right subtype keeps driver-specific
  // fields (palette, private data) intact.
  AM_MEDIA_TYPE* templateMt = nullptr;
  int templateScore = -1;
  for (int i = 0; i < count; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    VIDEO_STREAM_CONFIG_CAPS vscc = {};
    if (FAILED(cfg->GetStreamCaps(i, &mt, (BYTE*)&vscc)) || !mt) continue;

    int score = -1;
    VideoFormatInfo info;
    if (IsEqualGUID(mt->subtype, wantSubtype) && ParseVideoMediaType(mt, &info)) {
      score = 1;
      if (info.width == fmt.width && info.height == fmt.height) score = 3;
      else if (info.width >= fmt.width && info.height >= fmt.height) score = 2;
    }
    if (score > templateScore) {
      DeleteMediaType(templateMt);
      templateMt = mt;
      templateScore = score;
    } else {
      DeleteMediaType(mt);
    }
  }

  if (!templateMt || templateScore < 0) {
    DeleteMediaType(templateMt);
    CAP_WARN("Kein passender Media-Type für %s gefunden", fmt.subtype.c_str());
    return VFW_E_INVALIDMEDIATYPE;
  }

  AM_MEDIA_TYPE* patched = CreateMediaTypeCopy(templateMt);
  DeleteMediaType(templateMt);
  if (!patched) return E_OUTOFMEMORY;

  hr = E_FAIL;
  if (PatchMediaType(patched, fmt.width, fmt.height, fmt.fps)) {
    hr = cfg->SetFormat(patched);
    if (FAILED(hr) && fmt.fps > 0.0) {
      // Some drivers reject an unusual frame interval but accept the size.
      // Retry with the template's own interval so at least the resolution
      // takes effect; the actual rate is reported back to the caller.
      CAP_WARN("SetFormat mit %.3f fps abgelehnt (%s), versuche ohne Bildrate",
               fmt.fps, HrToString(hr).c_str());
      AM_MEDIA_TYPE* retry = CreateMediaTypeCopy(patched);
      if (retry && PatchMediaType(retry, fmt.width, fmt.height, 0.0)) {
        hr = cfg->SetFormat(retry);
      }
      DeleteMediaType(retry);
    }
  }
  DeleteMediaType(patched);

  if (FAILED(hr)) return hr;

  if (applied) {
    AM_MEDIA_TYPE* current = nullptr;
    if (SUCCEEDED(cfg->GetFormat(&current)) && current) {
      ParseVideoMediaType(current, applied);
      DeleteMediaType(current);
    }
  }
  return S_OK;
}

// -------------------------------------------------------------------- crossbar

std::string PhysicalConnectorName(long physicalType) {
  switch (physicalType) {
    case PhysConn_Video_Tuner: return "Tuner";
    case PhysConn_Video_Composite: return "Composite";
    case PhysConn_Video_SVideo: return "S-Video";
    case PhysConn_Video_RGB: return "VGA / RGB";
    case PhysConn_Video_YRYBY: return "Component (YPbPr)";
    case PhysConn_Video_SerialDigital: return "HDMI / SDI";
    case PhysConn_Video_ParallelDigital: return "DVI";
    case PhysConn_Video_SCSI: return "SCSI";
    case PhysConn_Video_AUX: return "AUX";
    case PhysConn_Video_1394: return "FireWire";
    case PhysConn_Video_USB: return "USB";
    case PhysConn_Video_VideoDecoder: return "Video-Decoder";
    case PhysConn_Video_VideoEncoder: return "Video-Encoder";
    case PhysConn_Video_SCART: return "SCART";
    case PhysConn_Video_Black: return "Schwarz (kein Eingang)";
    case PhysConn_Audio_Tuner: return "Audio Tuner";
    case PhysConn_Audio_Line: return "Audio Line-In";
    case PhysConn_Audio_Mic: return "Mikrofon";
    case PhysConn_Audio_AESDigital: return "AES/EBU";
    case PhysConn_Audio_SPDIFDigital: return "S/PDIF";
    case PhysConn_Audio_SCSI: return "Audio SCSI";
    case PhysConn_Audio_AUX: return "Audio AUX";
    case PhysConn_Audio_1394: return "Audio FireWire";
    case PhysConn_Audio_USB: return "Audio USB";
    case PhysConn_Audio_AudioDecoder: return "Audio-Decoder";
    default: return Format("Eingang Typ %ld", physicalType);
  }
}

namespace {

ComPtr<IAMCrossbar> FindCrossbar(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter) {
  if (!builder || !captureFilter) return nullptr;
  ComPtr<IAMCrossbar> xbar;
  HRESULT hr = builder->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, captureFilter,
                                      IID_IAMCrossbar, (void**)xbar.GetAddressOf());
  if (FAILED(hr)) return nullptr;
  return xbar;
}

bool IsVideoConnector(long physicalType) {
  return physicalType < PhysConn_Audio_Tuner;
}

}  // namespace

std::vector<CrossbarInput> EnumerateCrossbarInputs(ICaptureGraphBuilder2* builder,
                                                   IBaseFilter* captureFilter) {
  std::vector<CrossbarInput> inputs;
  ComPtr<IAMCrossbar> xbar = FindCrossbar(builder, captureFilter);
  if (!xbar) return inputs;

  long outCount = 0, inCount = 0;
  if (FAILED(xbar->get_PinCounts(&outCount, &inCount))) return inputs;

  // Count duplicates so two composite inputs become "Composite 1" / "Composite 2".
  std::vector<std::pair<long, int>> seen;
  for (long i = 0; i < inCount; ++i) {
    long related = 0, physType = 0;
    if (FAILED(xbar->get_CrossbarPinInfo(TRUE, i, &related, &physType))) continue;
    if (!IsVideoConnector(physType)) continue;

    int ordinal = 0;
    for (auto& s : seen) {
      if (s.first == physType) { ordinal = ++s.second; break; }
    }
    if (ordinal == 0) seen.emplace_back(physType, 0);

    CrossbarInput in;
    in.pinIndex = (int)i;
    in.physicalType = physType;
    in.name = PhysicalConnectorName(physType);
    if (ordinal > 0) in.name += Format(" %d", ordinal + 1);
    inputs.push_back(std::move(in));
  }
  return inputs;
}

bool RouteCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter, int index) {
  if (index < 0) return true;  // "leave alone"
  ComPtr<IAMCrossbar> xbar = FindCrossbar(builder, captureFilter);
  if (!xbar) return false;

  long outCount = 0, inCount = 0;
  if (FAILED(xbar->get_PinCounts(&outCount, &inCount))) return false;

  std::vector<CrossbarInput> inputs = EnumerateCrossbarInputs(builder, captureFilter);
  if (index >= (int)inputs.size()) return false;
  const long videoIn = inputs[(size_t)index].pinIndex;

  long relatedAudioIn = 0, physType = 0;
  xbar->get_CrossbarPinInfo(TRUE, videoIn, &relatedAudioIn, &physType);

  bool routed = false;
  for (long o = 0; o < outCount; ++o) {
    long relatedOut = 0, outPhys = 0;
    if (FAILED(xbar->get_CrossbarPinInfo(FALSE, o, &relatedOut, &outPhys))) continue;
    if (!IsVideoConnector(outPhys)) continue;
    if (xbar->CanRoute(o, videoIn) != S_OK) continue;
    if (SUCCEEDED(xbar->Route(o, videoIn))) {
      routed = true;
      CAP_LOG("Crossbar: Videoeingang '%s' auf Ausgang %ld geroutet",
              inputs[(size_t)index].name.c_str(), o);
      // Route the paired audio input to the matching audio output, so picking
      // "Component" also selects the audio jacks that belong to it.
      if (relatedAudioIn >= 0 && relatedOut >= 0) {
        if (xbar->CanRoute(relatedOut, relatedAudioIn) == S_OK) {
          xbar->Route(relatedOut, relatedAudioIn);
        }
      }
      break;
    }
  }
  if (!routed) CAP_WARN("Crossbar: Eingang %d konnte nicht geroutet werden", index);
  return routed;
}

// ---------------------------------------------------------------------------
// Analogue video standard

namespace {

struct StandardEntry {
  long value;
  const char* name;
  int lines;
};

// Ordered so the common ones come first; the index is written to the config, so
// entries may be appended but not reordered.
const StandardEntry kStandards[] = {
    {AnalogVideo_PAL_B, "PAL B", 625},        {AnalogVideo_PAL_G, "PAL G", 625},
    {AnalogVideo_PAL_I, "PAL I", 625},        {AnalogVideo_PAL_D, "PAL D", 625},
    {AnalogVideo_PAL_H, "PAL H", 625},        {AnalogVideo_PAL_N, "PAL N", 625},
    {AnalogVideo_PAL_60, "PAL 60", 525},      {AnalogVideo_PAL_M, "PAL M", 525},
    {AnalogVideo_NTSC_M, "NTSC M", 525},      {AnalogVideo_NTSC_M_J, "NTSC M (Japan)", 525},
    {AnalogVideo_NTSC_433, "NTSC 4.43", 525}, {AnalogVideo_SECAM_B, "SECAM B", 625},
    {AnalogVideo_SECAM_D, "SECAM D", 625},    {AnalogVideo_SECAM_G, "SECAM G", 625},
    {AnalogVideo_SECAM_H, "SECAM H", 625},    {AnalogVideo_SECAM_K, "SECAM K", 625},
    {AnalogVideo_SECAM_K1, "SECAM K1", 625},  {AnalogVideo_SECAM_L, "SECAM L", 625},
    {AnalogVideo_SECAM_L1, "SECAM L1", 625},  {AnalogVideo_PAL_N_COMBO, "PAL N combo", 625},
};
const int kStandardCount = (int)(sizeof(kStandards) / sizeof(kStandards[0]));

ComPtr<IAMAnalogVideoDecoder> DecoderOf(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec;
  if (filter) filter->QueryInterface(IID_PPV_ARGS(&dec));
  return dec;
}

}  // namespace

int VideoStandardCount() { return kStandardCount; }

long VideoStandardValue(int index) {
  if (index < 0 || index >= kStandardCount) return 0;
  return kStandards[index].value;
}

const char* VideoStandardName(int index) {
  if (index < 0 || index >= kStandardCount) return "";
  return kStandards[index].name;
}

int VideoStandardIndexOf(long value) {
  for (int i = 0; i < kStandardCount; ++i) {
    if (kStandards[i].value == value) return i;
  }
  return -1;
}

int VideoStandardLines(long value) {
  const int index = VideoStandardIndexOf(value);
  return index < 0 ? 0 : kStandards[index].lines;
}

long AvailableVideoStandards(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return 0;
  long available = 0;
  if (FAILED(dec->get_AvailableTVFormats(&available))) return 0;
  return available;
}

long CurrentVideoStandard(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return 0;
  long current = 0;
  if (FAILED(dec->get_TVFormat(&current))) return 0;
  return current;
}

bool SetVideoStandard(IBaseFilter* filter, long standard) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec || standard == 0) return false;
  const HRESULT hr = dec->put_TVFormat(standard);
  if (FAILED(hr)) {
    CAP_WARN("Videonorm %s konnte nicht gesetzt werden: %s",
             VideoStandardName(VideoStandardIndexOf(standard)), HrToString(hr).c_str());
    return false;
  }
  CAP_LOG("Videonorm gesetzt: %s", VideoStandardName(VideoStandardIndexOf(standard)));
  return true;
}

int VideoStandardLocked(IBaseFilter* filter) {
  ComPtr<IAMAnalogVideoDecoder> dec = DecoderOf(filter);
  if (!dec) return -1;
  long locked = 0;
  if (FAILED(dec->get_HorizontalLocked(&locked))) return -1;
  return locked ? 1 : 0;
}

std::string VideoStandardSettingName(long setting) {
  if (setting == 0) return T("Nicht ändern", "Leave alone");
  if (setting == -1) return T("Automatisch", "Automatic");
  const int index = VideoStandardIndexOf(setting);
  return index < 0 ? T("Unbekannt", "Unknown") : VideoStandardName(index);
}

double VideoStandardSubcarrierSamples(long standard) {
  // 13.5 MHz of sampling over the active line, from BT.601.
  const double kSampleRate = 13500000.0;
  // NTSC M and its Japanese variant carry 3.579545 MHz; PAL M is close enough to
  // count with them. Everything else in practice -- PAL, PAL-60, SECAM, and the
  // 4.43 MHz NTSC variant -- sits at 4.43361875 MHz.
  const bool ntsc = standard == AnalogVideo_NTSC_M || standard == AnalogVideo_NTSC_M_J ||
                    standard == AnalogVideo_PAL_M;
  const double carrier = ntsc ? 3579545.0 : 4433618.75;
  return kSampleRate / carrier;
}


std::vector<long> AutoStandardCandidates(long available) {
  // One per line count and colour system. Trying PAL B and then PAL G would be
  // asking the same question twice: they differ in the sound carrier, which is
  // no part of the picture.
  static const long kOrder[] = {AnalogVideo_PAL_B,   AnalogVideo_PAL_60, AnalogVideo_NTSC_M,
                                AnalogVideo_NTSC_M_J, AnalogVideo_SECAM_B};
  std::vector<long> out;
  for (long value : kOrder) {
    if (available & value) out.push_back(value);
  }
  return out;
}

}  // namespace cap
