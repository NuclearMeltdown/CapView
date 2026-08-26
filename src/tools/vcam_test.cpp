// Both ends of the virtual camera, small enough to read.
//
// The camera's whole job happens inside somebody else's process, and the
// somebodies that matter -- OBS, Discord, a browser -- are large, slow to
// start, and say nothing useful when they show black. This does what they do
// and narrates it: load the filter, ask what it offers, take one of the offers,
// pull frames, and report what actually arrived.
//
// It loads the DLL by path rather than by CLSID on purpose. Testing must not
// depend on an installation, and an installation needs administrator rights.
//
//   capview_vcam_test.exe [--size WxH] [--fps N] [--p010] [--seconds N]
//                         [--dump file.nv12] [--caps-only]
//   capview_vcam_test.exe --produce [--size WxH] [--fps N] [--p010] [--seconds N]
//
// Without --size it connects to whatever the filter offers first, which is the
// source's own shape -- the same path OBS takes.
//
// --produce stands in for CapView: it publishes a moving pattern into the same
// shared section CapView publishes into, so the reading half can be exercised
// without a capture card, without an installed camera, and without CapView
// running at all. It is written straight against vcam_shared.h rather than
// against VirtualCamera on purpose -- an independent second implementation of
// the same contract. Where the two agree, the contract is unambiguous.

#include <windows.h>

#include <dshow.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "vcam/vcam_shared.h"

namespace {

// MEDIASUBTYPE_NV12 and _P010 are FourCC subtypes, built the way every FourCC
// subtype is: the code in the first four bytes of the DirectShow GUID base.
GUID SubtypeFromFourCC(DWORD fourcc) {
  GUID g = {fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
  return g;
}

const GUID kNv12 = SubtypeFromFourCC(MAKEFOURCC('N', 'V', '1', '2'));
const GUID kP010 = SubtypeFromFourCC(MAKEFOURCC('P', '0', '1', '0'));

void FreeMediaTypeContents(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  if (mt->pbFormat) ::CoTaskMemFree(mt->pbFormat);
  if (mt->pUnk) mt->pUnk->Release();
  ::ZeroMemory(mt, sizeof(*mt));
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  FreeMediaTypeContents(mt);
  ::CoTaskMemFree(mt);
}

bool CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
  *dst = *src;
  dst->pbFormat = nullptr;
  dst->pUnk = nullptr;
  if (src->cbFormat && src->pbFormat) {
    dst->pbFormat = (BYTE*)::CoTaskMemAlloc(src->cbFormat);
    if (!dst->pbFormat) return false;
    ::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  }
  if (src->pUnk) {
    dst->pUnk = src->pUnk;
    dst->pUnk->AddRef();
  }
  return true;
}

std::string SubtypeName(const GUID& g) {
  if (g == kNv12) return "NV12";
  if (g == kP010) return "P010";
  char buffer[64];
  ::sprintf_s(buffer, "%08lX-....", (unsigned long)g.Data1);
  return buffer;
}

double FpsOf(REFERENCE_TIME interval) {
  return interval > 0 ? 10000000.0 / (double)interval : 0.0;
}

void DescribeType(const AM_MEDIA_TYPE* mt) {
  if (!mt || mt->formattype != FORMAT_VideoInfo || !mt->pbFormat) {
    ::printf("      (no VIDEOINFOHEADER)\n");
    return;
  }
  const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
  ::printf("      %ld x %ld  %s  %.4g fps  %lu bytes/frame\n", vih->bmiHeader.biWidth,
           vih->bmiHeader.biHeight, SubtypeName(mt->subtype).c_str(),
           FpsOf(vih->AvgTimePerFrame), (unsigned long)mt->lSampleSize);
}

// ---------------------------------------------------------------- the sink

// Everything a source filter needs on the other end of a connection, and
// nothing else. No graph, no clock: the source paces itself, which is exactly
// the behaviour under test.
class Sink : public IPin, public IMemInputPin {
 public:
  Sink() = default;
  ~Sink() {
    FreeMediaTypeContents(&type_);
    if (allocator_) allocator_->Release();
    if (connected_) connected_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
      *out = static_cast<IPin*>(this);
    } else if (riid == IID_IMemInputPin) {
      *out = static_cast<IMemInputPin*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const long n = ::InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // ---- IPin ----
  STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }

  STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override {
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    if (!connector || !mt) return E_POINTER;
    if (mt->majortype != MEDIATYPE_Video || !mt->pbFormat) return VFW_E_TYPE_NOT_ACCEPTED;
    if (mt->subtype != kNv12 && mt->subtype != kP010) return VFW_E_TYPE_NOT_ACCEPTED;
    if (!CopyMediaType(&type_, mt)) return E_OUTOFMEMORY;
    connected_ = connector;
    connected_->AddRef();
    return S_OK;
  }

  STDMETHODIMP Disconnect() override {
    if (connected_) {
      connected_->Release();
      connected_ = nullptr;
    }
    FreeMediaTypeContents(&type_);
    return S_OK;
  }

  STDMETHODIMP ConnectedTo(IPin** pin) override {
    if (!pin) return E_POINTER;
    *pin = connected_;
    if (!connected_) return VFW_E_NOT_CONNECTED;
    connected_->AddRef();
    return S_OK;
  }

  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override {
    if (!mt) return E_POINTER;
    if (!connected_) {
      ::ZeroMemory(mt, sizeof(*mt));
      return VFW_E_NOT_CONNECTED;
    }
    return CopyMediaType(mt, &type_) ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP QueryPinInfo(PIN_INFO* info) override {
    if (!info) return E_POINTER;
    ::ZeroMemory(info, sizeof(*info));
    info->dir = PINDIR_INPUT;
    ::wcscpy_s(info->achName, L"Input");
    return S_OK;
  }
  STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override {
    if (!dir) return E_POINTER;
    *dir = PINDIR_INPUT;
    return S_OK;
  }
  STDMETHODIMP QueryId(LPWSTR* id) override {
    if (!id) return E_POINTER;
    const wchar_t kId[] = L"Input";
    *id = (LPWSTR)::CoTaskMemAlloc(sizeof(kId));
    if (!*id) return E_OUTOFMEMORY;
    ::memcpy(*id, kId, sizeof(kId));
    return S_OK;
  }
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override {
    return mt && mt->majortype == MEDIATYPE_Video ? S_OK : S_FALSE;
  }
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
  STDMETHODIMP QueryInternalConnections(IPin**, ULONG* count) override {
    if (count) *count = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP EndOfStream() override { return S_OK; }
  STDMETHODIMP BeginFlush() override { return S_OK; }
  STDMETHODIMP EndFlush() override { return S_OK; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

  // ---- IMemInputPin ----
  STDMETHODIMP GetAllocator(IMemAllocator** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    return VFW_E_NO_ALLOCATOR;  // the source brings its own, which is the point
  }
  STDMETHODIMP NotifyAllocator(IMemAllocator* allocator, BOOL) override {
    if (allocator) allocator->AddRef();
    if (allocator_) allocator_->Release();
    allocator_ = allocator;
    return S_OK;
  }
  STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
  STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

  STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* done) override {
    long n = 0;
    for (; n < count; ++n) {
      const HRESULT hr = Receive(samples[n]);
      if (FAILED(hr)) break;
    }
    if (done) *done = n;
    return S_OK;
  }

  STDMETHODIMP Receive(IMediaSample* sample) override {
    if (!sample) return E_POINTER;
    BYTE* data = nullptr;
    if (FAILED(sample->GetPointer(&data)) || !data) return S_OK;
    const long bytes = sample->GetActualDataLength();

    REFERENCE_TIME start = 0, stop = 0;
    if (SUCCEEDED(sample->GetTime(&start, &stop))) {
      if (received_ == 0) firstStart_ = start;
      lastStop_ = stop;
    }

    // Enough of the picture to tell a blank one from a real one, which is the
    // question this program exists to answer. The luma plane of a blank frame
    // is a single value; anything the camera actually carried is not.
    const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)type_.pbFormat;
    if (vih) {
      const long width = vih->bmiHeader.biWidth;
      const long height = vih->bmiHeader.biHeight;
      const bool wide = type_.subtype == kP010;
      const long stride = width * (wide ? 2 : 1);
      if (stride > 0 && height > 0 && bytes >= stride * height) {
        unsigned lo = 0xFFFFu, hi = 0;
        unsigned long long sum = 0;
        const long step = height > 64 ? height / 64 : 1;
        long counted = 0;
        for (long y = 0; y < height; y += step) {
          const BYTE* row = data + (size_t)y * stride;
          for (long x = 0; x < width; x += 8) {
            const unsigned v = wide ? (((const uint16_t*)row)[x] >> 6) : row[x];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            sum += v;
            ++counted;
          }
        }
        lumaLow_ = lo;
        lumaHigh_ = hi;
        lumaMean_ = counted ? (unsigned)(sum / counted) : 0;
        if (hi != lo) ++moving_;
      }
    }

    if (!dumpPath_.empty() && !dumped_ && lumaHigh_ != lumaLow_) {
      FILE* file = nullptr;
      if (::fopen_s(&file, dumpPath_.c_str(), "wb") == 0 && file) {
        ::fwrite(data, 1, (size_t)bytes, file);
        ::fclose(file);
        dumped_ = true;
        ::printf("  dumped %ld bytes to %s\n", bytes, dumpPath_.c_str());
      }
    }

    ++received_;
    return S_OK;
  }

  void setDump(const std::string& path) { dumpPath_ = path; }
  long received() const { return received_; }
  long moving() const { return moving_; }
  unsigned lumaLow() const { return lumaLow_; }
  unsigned lumaHigh() const { return lumaHigh_; }
  unsigned lumaMean() const { return lumaMean_; }
  REFERENCE_TIME span() const { return lastStop_ - firstStart_; }

 private:
  long refs_ = 1;
  IPin* connected_ = nullptr;
  IMemAllocator* allocator_ = nullptr;
  AM_MEDIA_TYPE type_ = {};
  long received_ = 0;
  long moving_ = 0;
  unsigned lumaLow_ = 0;
  unsigned lumaHigh_ = 0;
  unsigned lumaMean_ = 0;
  REFERENCE_TIME firstStart_ = 0;
  REFERENCE_TIME lastStop_ = 0;
  std::string dumpPath_;
  bool dumped_ = false;
};

// --------------------------------------------------------------- the filter

std::wstring ExeFolder() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s = path;
  const size_t cut = s.find_last_of(L'\\');
  return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut + 1);
}

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

IBaseFilter* CreateFilter(const wchar_t* dllPath) {
  HMODULE module = ::LoadLibraryW(dllPath);
  if (!module) {
    ::wprintf(L"cannot load %s (error %lu)\n", dllPath, (unsigned long)::GetLastError());
    return nullptr;
  }
  auto entry = (GetClassObjectFn)::GetProcAddress(module, "DllGetClassObject");
  if (!entry) {
    ::printf("DllGetClassObject missing\n");
    return nullptr;
  }
  // From the string rather than the compiled-in constant, so this asks for the
  // same class the registry entry names -- which is what a real consumer does.
  CLSID clsid = {};
  if (FAILED(::CLSIDFromString(cap::vcam::kFilterClsidString, &clsid))) {
    ::printf("the CLSID string does not parse\n");
    return nullptr;
  }
  IClassFactory* factory = nullptr;
  HRESULT hr = entry(clsid, IID_IClassFactory, (void**)&factory);
  if (FAILED(hr) || !factory) {
    ::printf("DllGetClassObject failed 0x%08lX\n", (unsigned long)hr);
    return nullptr;
  }
  IBaseFilter* filter = nullptr;
  hr = factory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&filter);
  factory->Release();
  if (FAILED(hr) || !filter) {
    ::printf("CreateInstance failed 0x%08lX\n", (unsigned long)hr);
    return nullptr;
  }
  return filter;
}

bool BuildRequest(AM_MEDIA_TYPE* mt, bool wide, int width, int height, double fps) {
  const uint32_t bytesPerSample = wide ? 2u : 1u;
  const size_t image = (size_t)width * bytesPerSample * height * 3 / 2;

  ::ZeroMemory(mt, sizeof(*mt));
  auto* vih = (VIDEOINFOHEADER*)::CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
  if (!vih) return false;
  ::ZeroMemory(vih, sizeof(*vih));
  vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  vih->bmiHeader.biWidth = width;
  vih->bmiHeader.biHeight = height;
  vih->bmiHeader.biPlanes = 1;
  vih->bmiHeader.biBitCount = wide ? 24 : 12;
  vih->bmiHeader.biCompression = wide ? MAKEFOURCC('P', '0', '1', '0')
                                      : MAKEFOURCC('N', 'V', '1', '2');
  vih->bmiHeader.biSizeImage = (DWORD)image;
  vih->AvgTimePerFrame = fps > 0.0 ? (REFERENCE_TIME)(10000000.0 / fps + 0.5) : 333333;
  vih->dwBitRate = (DWORD)(image * 8 * (fps > 0.0 ? fps : 30.0));

  mt->majortype = MEDIATYPE_Video;
  mt->subtype = wide ? kP010 : kNv12;
  mt->bFixedSizeSamples = TRUE;
  mt->bTemporalCompression = FALSE;
  mt->lSampleSize = (ULONG)image;
  mt->formattype = FORMAT_VideoInfo;
  mt->cbFormat = sizeof(VIDEOINFOHEADER);
  mt->pbFormat = (BYTE*)vih;
  return true;
}

// ------------------------------------------------------------- the producer

// CapView's half of the contract, written again from the header alone.
int RunProducer(int width, int height, double fps, bool wide, int seconds) {
  namespace vc = cap::vcam;

  if (width <= 0 || height <= 0) {
    width = 1920;
    height = 1080;
  }
  width &= ~1;
  height &= ~1;
  if (fps <= 0.0) fps = 60.0;
  const int64_t interval = (int64_t)(10000000.0 / fps + 0.5);
  const uint32_t pixel = wide ? vc::kPixelP010 : vc::kPixelNv12;
  const uint32_t strideY = (uint32_t)width * vc::BytesPerSample(pixel);
  const uint64_t slotBytes = (vc::PictureBytes(strideY, (uint32_t)height) + 4095ull) & ~4095ull;

  HANDLE control = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(vc::ControlBlock), vc::kControlSectionName);
  if (!control) {
    ::printf("cannot create the control section (error %lu)\n", (unsigned long)::GetLastError());
    return 1;
  }
  auto* cb = (vc::ControlBlock*)::MapViewOfFile(control, FILE_MAP_ALL_ACCESS, 0, 0,
                                                sizeof(vc::ControlBlock));
  if (!cb) {
    ::printf("cannot map the control section\n");
    return 1;
  }
  ::ZeroMemory(cb, sizeof(*cb));
  cb->magic = vc::kMagic;
  cb->version = vc::kVersion;
  cb->stateBytes = (uint32_t)sizeof(vc::ControlBlock);
  cb->sourceWidth = (uint32_t)width;
  cb->sourceHeight = (uint32_t)height;
  cb->sourceInterval100ns = interval;
  cb->sourcePixel = pixel;
  cb->producerAlive = 1;
  cb->frameGeneration = 0;

  HANDLE wake[vc::kConsumerSlots] = {};
  for (uint32_t i = 0; i < vc::kConsumerSlots; ++i) {
    wchar_t name[128];
    ::swprintf_s(name, L"%s%u", vc::kWakeEventPrefix, i);
    wake[i] = ::CreateEventW(nullptr, FALSE, FALSE, name);
  }

  const uint32_t generation = ::GetTickCount() | 1u;
  wchar_t frameName[128];
  ::swprintf_s(frameName, L"%s%u", vc::kFrameSectionPrefix, generation);
  const uint64_t sectionBytes = vc::FrameSectionBytes(slotBytes);
  HANDLE frames = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       (DWORD)(sectionBytes >> 32), (DWORD)sectionBytes,
                                       frameName);
  if (!frames) {
    ::printf("cannot create the frame section (error %lu)\n", (unsigned long)::GetLastError());
    return 1;
  }
  auto* fh = (vc::FrameSectionHeader*)::MapViewOfFile(frames, FILE_MAP_ALL_ACCESS, 0, 0,
                                                      (SIZE_T)sectionBytes);
  if (!fh) {
    ::printf("cannot map the frame section\n");
    return 1;
  }
  ::ZeroMemory(fh, sizeof(*fh));
  fh->magic = vc::kMagic;
  fh->version = vc::kVersion;
  fh->generation = generation;
  fh->slotCount = vc::kSlotCount;
  fh->width = (uint32_t)width;
  fh->height = (uint32_t)height;
  fh->strideY = strideY;
  fh->pixel = pixel;
  fh->slotBytes = slotBytes;
  ::MemoryBarrier();
  cb->frameGeneration = generation;

  ::printf("producing %d x %d %s @ %.4g fps, generation %u, %llu bytes per slot\n", width, height,
           wide ? "P010" : "NV12", fps, generation, (unsigned long long)slotBytes);
  ::printf("press Ctrl+C to stop\n");

  uint32_t writeIndex = 0, published = 0;
  const int64_t startedAt = (int64_t)::GetTickCount64();
  uint32_t lastReport = 0;

  for (;;) {
    const int64_t elapsed = (int64_t)::GetTickCount64() - startedAt;
    if (seconds > 0 && elapsed >= (int64_t)seconds * 1000) break;

    const uint32_t slot = writeIndex % vc::kSlotCount;
    vc::SlotHeader& head = fh->slots[slot];
    head.sequence = head.sequence + 1;  // odd: being written
    ::MemoryBarrier();

    // A vertical ramp with a bar sliding down it. Two properties matter and
    // nothing else does: no frame is one flat value, and no two consecutive
    // frames are the same. That is exactly what the reading half is asked to
    // prove it carried through.
    uint8_t* data = vc::SlotData(fh, slotBytes, slot);
    const int bar = (int)(published % (uint32_t)height);
    for (int y = 0; y < height; ++y) {
      uint8_t* row = data + (size_t)y * strideY;
      const unsigned ramp = 16u + (unsigned)(200 * y / height);
      const unsigned value = (y >= bar && y < bar + 16) ? 235u : ramp;
      if (wide) {
        auto* wideRow = (uint16_t*)row;
        const uint16_t v = (uint16_t)(value << 8);
        for (int x = 0; x < width; ++x) wideRow[x] = v;
      } else {
        ::memset(row, (int)value, (size_t)width);
      }
    }
    // Neutral chroma: 128 for eight bit, mid scale for ten in the high bits.
    uint8_t* chroma = data + (size_t)strideY * height;
    if (wide) {
      auto* wideChroma = (uint16_t*)chroma;
      const size_t samples = (size_t)width * height / 2;
      for (size_t i = 0; i < samples; ++i) wideChroma[i] = 0x8000;
    } else {
      ::memset(chroma, 128, (size_t)strideY * height / 2);
    }

    head.width = (uint32_t)width;
    head.height = (uint32_t)height;
    head.strideY = strideY;
    head.pixel = pixel;
    head.frameIndex = ++published;
    FILETIME ft = {};
    ::GetSystemTimePreciseAsFileTime(&ft);
    head.timestamp100ns =
        (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime);
    ::MemoryBarrier();
    head.sequence = head.sequence + 1;  // even: whole again

    ++writeIndex;
    cb->framesPublished = published;
    for (uint32_t i = 0; i < vc::kConsumerSlots; ++i) {
      if (cb->consumers[i].inUse && wake[i]) ::SetEvent(wake[i]);
    }

    const uint32_t now = ::GetTickCount();
    if (now - lastReport >= 1000) {
      lastReport = now;
      ::printf("  %u published", published);
      for (uint32_t i = 0; i < vc::kConsumerSlots; ++i) {
        const vc::ConsumerSlot& s = cb->consumers[i];
        if (!s.inUse) continue;
        ::printf(" | pid %lu %ls %ux%u %s served %u fresh %u", (unsigned long)s.pid, s.name,
                 s.width, s.height, s.streaming ? "streaming" : "idle", s.framesServed,
                 s.framesFresh);
      }
      ::printf("\n");
    }

    ::Sleep((DWORD)(interval / 10000));
  }

  cb->producerAlive = 0;
  cb->frameGeneration = 0;
  for (uint32_t i = 0; i < vc::kConsumerSlots; ++i) {
    if (wake[i]) {
      ::SetEvent(wake[i]);
      ::CloseHandle(wake[i]);
    }
  }
  ::UnmapViewOfFile(fh);
  ::CloseHandle(frames);
  ::UnmapViewOfFile(cb);
  ::CloseHandle(control);
  ::printf("stopped after %u frames\n", published);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  int width = 0, height = 0, seconds = 3;
  double fps = 0.0;
  bool wide = false, capsOnly = false, produce = false, secondsGiven = false;
  std::string dump;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--p010") {
      wide = true;
    } else if (arg == L"--caps-only") {
      capsOnly = true;
    } else if (arg == L"--produce") {
      produce = true;
    } else if (arg == L"--size" && i + 1 < argc) {
      ::swscanf_s(argv[++i], L"%dx%d", &width, &height);
    } else if (arg == L"--fps" && i + 1 < argc) {
      fps = ::_wtof(argv[++i]);
    } else if (arg == L"--seconds" && i + 1 < argc) {
      seconds = ::_wtoi(argv[++i]);
      secondsGiven = true;
    } else if (arg == L"--dump" && i + 1 < argc) {
      const std::wstring path = argv[++i];
      dump.clear();
      for (wchar_t c : path) dump.push_back(c < 128 ? (char)c : '_');
    } else {
      ::wprintf(L"unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }

  // A producer nobody asked to stop runs until it is stopped.
  if (produce) return RunProducer(width, height, fps, wide, secondsGiven ? seconds : 0);

  ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  const std::wstring dll = ExeFolder() + L"capview_vcam.dll";
  IBaseFilter* filter = CreateFilter(dll.c_str());
  if (!filter) return 1;

  IPin* pin = nullptr;
  {
    IEnumPins* pins = nullptr;
    if (SUCCEEDED(filter->EnumPins(&pins)) && pins) {
      ULONG got = 0;
      pins->Next(1, &pin, &got);
      pins->Release();
    }
  }
  if (!pin) {
    ::printf("the filter has no pins\n");
    return 1;
  }

  // ---- what it offers ----
  IAMStreamConfig* config = nullptr;
  if (SUCCEEDED(pin->QueryInterface(IID_IAMStreamConfig, (void**)&config)) && config) {
    int count = 0, size = 0;
    if (SUCCEEDED(config->GetNumberOfCapabilities(&count, &size))) {
      ::printf("%d capabilities, %d bytes each\n", count, size);
      for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps = {};
        if (config->GetStreamCaps(i, &mt, (BYTE*)&caps) != S_OK || !mt) continue;
        ::printf("  [%2d]\n", i);
        DescribeType(mt);
        ::printf("      range %ld x %ld .. %ld x %ld step %ld/%ld, %.4g .. %.4g fps\n",
                 caps.MinOutputSize.cx, caps.MinOutputSize.cy, caps.MaxOutputSize.cx,
                 caps.MaxOutputSize.cy, caps.OutputGranularityX, caps.OutputGranularityY,
                 FpsOf(caps.MaxFrameInterval), FpsOf(caps.MinFrameInterval));
        DeleteMediaType(mt);
      }
    }

    AM_MEDIA_TYPE* current = nullptr;
    if (SUCCEEDED(config->GetFormat(&current)) && current) {
      ::printf("default format:\n");
      DescribeType(current);
      DeleteMediaType(current);
    }
  }
  if (capsOnly) {
    if (config) config->Release();
    pin->Release();
    filter->Release();
    ::CoUninitialize();
    return 0;
  }

  // ---- ask for something, if asked to ----
  if (config && (width > 0 || fps > 0.0 || wide)) {
    AM_MEDIA_TYPE want = {};
    int useW = width, useH = height;
    if (useW <= 0 || useH <= 0) {
      AM_MEDIA_TYPE* current = nullptr;
      if (SUCCEEDED(config->GetFormat(&current)) && current && current->pbFormat) {
        const auto* vih = (const VIDEOINFOHEADER*)current->pbFormat;
        useW = (int)vih->bmiHeader.biWidth;
        useH = (int)vih->bmiHeader.biHeight;
        if (fps <= 0.0) fps = FpsOf(vih->AvgTimePerFrame);
      }
      if (current) DeleteMediaType(current);
    }
    if (useW > 0 && useH > 0 && BuildRequest(&want, wide, useW, useH, fps)) {
      const HRESULT hr = config->SetFormat(&want);
      ::printf("SetFormat %d x %d %s @ %.4g -> 0x%08lX\n", useW, useH, wide ? "P010" : "NV12",
               fps, (unsigned long)hr);
      FreeMediaTypeContents(&want);
    }
  }

  // ---- connect and run ----
  Sink* sink = new Sink();
  sink->setDump(dump);

  HRESULT hr = pin->Connect(static_cast<IPin*>(sink), nullptr);
  if (FAILED(hr)) {
    ::printf("Connect failed 0x%08lX\n", (unsigned long)hr);
    sink->Release();
    if (config) config->Release();
    pin->Release();
    filter->Release();
    ::CoUninitialize();
    return 1;
  }

  {
    AM_MEDIA_TYPE agreed = {};
    if (SUCCEEDED(pin->ConnectionMediaType(&agreed))) {
      ::printf("connected as:\n");
      DescribeType(&agreed);
      FreeMediaTypeContents(&agreed);
    }
  }

  filter->Pause();
  filter->Run(0);
  ::printf("running for %d s...\n", seconds);
  ::Sleep((DWORD)seconds * 1000);
  filter->Stop();

  const double span = (double)sink->span() / 10000000.0;
  ::printf("%ld samples in %.3f s of stream time (%.4g fps)\n", sink->received(), span,
           span > 0.0 ? sink->received() / span : 0.0);
  ::printf("luma %u..%u, mean %u -- %s\n", sink->lumaLow(), sink->lumaHigh(), sink->lumaMean(),
           sink->moving() > 0 ? "a picture" : "flat (blank, or CapView is not publishing)");

  pin->Disconnect();
  sink->Release();
  if (config) config->Release();
  pin->Release();
  filter->Release();
  ::CoUninitialize();
  return 0;
}
