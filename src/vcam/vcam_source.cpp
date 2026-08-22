#include "vcam/vcam_source.h"

#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <sddl.h>

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

#include "vcam/vcam_shared.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "advapi32.lib")

namespace cap {
namespace vcam {

// {A1E4F2C7-6B3D-4A58-9E21-7C0D5B8F3A46}
const CLSID CLSID_CapViewSource = {
    0xa1e4f2c7, 0x6b3d, 0x4a58, {0x9e, 0x21, 0x7c, 0x0d, 0x5b, 0x8f, 0x3a, 0x46}};

namespace {

std::atomic<long> g_objects{0};

template <typename T>
void SafeRelease(T** p) {
  if (*p) {
    (*p)->Release();
    *p = nullptr;
  }
}

// ---------------------------------------------------------------------------
// The window onto CapView.
//
// Created here rather than in CapView because this side runs as a service and
// therefore may create objects in the global namespace, while an ordinary user
// account may not. The descriptor hands read and write to interactive users --
// it has to, since the whole point is that a program in the user's session
// fills it.
// ---------------------------------------------------------------------------
class SharedFrames {
 public:
  ~SharedFrames() { Close(); }

  bool Open() {
    if (view_) return true;

    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR sd = nullptr;
    const wchar_t* kAcl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;BU)(A;;GRGWGX;;;IU)";
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(kAcl, SDDL_REVISION_1, &sd,
                                                               nullptr)) {
      sa.nLength = sizeof(sa);
      sa.lpSecurityDescriptor = sd;
    }

    section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0,
                                    kSectionBytes, kSectionName);
    if (sd) ::LocalFree(sd);
    if (!section_) return false;

    const bool fresh = ::GetLastError() != ERROR_ALREADY_EXISTS;
    view_ = static_cast<SharedState*>(
        ::MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, kSectionBytes));
    if (!view_) {
      ::CloseHandle(section_);
      section_ = nullptr;
      return false;
    }
    if (fresh) {
      ::memset(view_, 0, sizeof(SharedState));
      view_->magic = kMagic;
      view_->version = kVersion;
    }
    return true;
  }

  void Close() {
    if (view_) {
      ::UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    if (section_) {
      ::CloseHandle(section_);
      section_ = nullptr;
    }
  }

  SharedState* state() { return view_; }

  // Announces what the consumer settled on, so CapView can produce it.
  void Announce(uint32_t w, uint32_t h, uint32_t fps, bool streaming) {
    if (!view_) return;
    view_->wantWidth = w;
    view_->wantHeight = h;
    view_->wantFps = fps;
    if (streaming) {
      ::InterlockedIncrement((volatile LONG*)&view_->consumers);
    } else if (view_->consumers > 0) {
      ::InterlockedDecrement((volatile LONG*)&view_->consumers);
    }
  }

  // Copies the newest whole picture. False when CapView has published nothing
  // yet, or when what is there does not match what we are handing out.
  bool ReadNewest(uint8_t* dst, uint32_t width, uint32_t height, uint32_t* lastIndex) {
    if (!view_ || view_->magic != kMagic) return false;
    const uint32_t index = view_->writeIndex;
    if (index == 0 || index == *lastIndex) return false;

    const uint32_t slot = (index - 1) % kSlotCount;
    SlotHeader& head = view_->slots[slot];

    // Two reads of the sequence around the copy. Odd means the writer is inside
    // this slot; a changed value means it got there while we were copying.
    const uint32_t before = head.sequence;
    if (before & 1u) return false;
    if (head.width != width || head.height != height) return false;
    const uint32_t bytes = head.bytes;
    if (bytes == 0 || bytes > kSlotBytes) return false;

    ::memcpy(dst, SlotData(view_, slot), bytes);

    ::MemoryBarrier();
    if (head.sequence != before) return false;

    *lastIndex = index;
    return true;
  }

 private:
  HANDLE section_ = nullptr;
  SharedState* view_ = nullptr;
};

// ---------------------------------------------------------------------------
// Media types
// ---------------------------------------------------------------------------
HRESULT MakeType(const CameraFormat& fmt, IMFMediaType** out) {
  *out = nullptr;
  IMFMediaType* type = nullptr;
  HRESULT hr = ::MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(type, MF_MT_FRAME_SIZE, fmt.width, fmt.height);
  if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fmt.fps, 1);
  if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)fmt.width);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_SAMPLE_SIZE, fmt.width * fmt.height * 3 / 2);
  if (FAILED(hr)) {
    type->Release();
    return hr;
  }
  *out = type;
  return S_OK;
}

class VCamSource;

// ---------------------------------------------------------------------------
// The stream. One per source: a camera with one picture pin.
// ---------------------------------------------------------------------------
class VCamStream : public IMFMediaStream2 {
 public:
  VCamStream(VCamSource* source, IMFStreamDescriptor* descriptor);

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
      *out = static_cast<IMFMediaStream2*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  // IMFMediaEventGenerator -- one queue per stream, so pictures do not queue
  // behind source level events.
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    IMFMediaEventQueue* queue = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return MF_E_SHUTDOWN;
      queue = queue_;
      queue->AddRef();
    }
    const HRESULT hr = queue->GetEvent(flags, event);  // may block; not under the lock
    queue->Release();
    return hr;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                          const PROPVARIANT* value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extended, status, value);
  }

  // IMFMediaStream
  STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!descriptor) return E_POINTER;
    *descriptor = descriptor_;
    (*descriptor)->AddRef();
    return S_OK;
  }
  STDMETHODIMP RequestSample(IUnknown* token) override;

  // IMFMediaStream2
  STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
  STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!state) return E_POINTER;
    *state = state_;
    return S_OK;
  }

  HRESULT Start();
  HRESULT Stop();
  HRESULT Shutdown();
  void SetFormat(const CameraFormat& fmt) { format_ = fmt; }

 private:
  ~VCamStream();
  HRESULT ProduceSample(IMFSample** out);

  std::atomic<ULONG> refs_{1};
  std::mutex mutex_;
  bool shutdown_ = false;
  MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;

  VCamSource* source_ = nullptr;  // weak: the source outlives the stream
  IMFStreamDescriptor* descriptor_ = nullptr;
  IMFMediaEventQueue* queue_ = nullptr;

  CameraFormat format_ = kFormats[0];
  SharedFrames frames_;
  uint32_t lastIndex_ = 0;
  std::vector<uint8_t> picture_;  // the last whole picture, repeated if need be
  bool havePicture_ = false;
  LONGLONG nextTime_ = 0;
};

// ---------------------------------------------------------------------------
// The source.
// ---------------------------------------------------------------------------
class VCamSource : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
 public:
  VCamSource() { AddLiveObject(); }

  HRESULT Init();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
      *out = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == IID_IMFGetService) {
      *out = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
      *out = static_cast<IKsControl*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  // IMFMediaEventGenerator
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    IMFMediaEventQueue* queue = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return MF_E_SHUTDOWN;
      queue = queue_;
      queue->AddRef();
    }
    const HRESULT hr = queue->GetEvent(flags, event);
    queue->Release();
    return hr;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                          const PROPVARIANT* value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extended, status, value);
  }

  // IMFMediaSource
  STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!characteristics) return E_POINTER;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override;
  STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                     const PROPVARIANT* start) override;
  STDMETHODIMP Stop() override;
  // A live source cannot pause, and the pipeline is entitled to be told so
  // rather than to be lied to.
  STDMETHODIMP Pause() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_ ? MF_E_SHUTDOWN : MF_E_INVALID_STATE_TRANSITION;
  }
  STDMETHODIMP Shutdown() override;

  // IMFMediaSourceEx
  STDMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!out) return E_POINTER;
    *out = attributes_;
    (*out)->AddRef();
    return S_OK;
  }
  STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!out) return E_POINTER;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    *out = streamAttributes_;
    (*out)->AddRef();
    return S_OK;
  }
  // No Direct3D here: the pictures arrive as bytes from another process, so
  // there is nothing a device manager would help with.
  STDMETHODIMP SetD3DManager(IUnknown*) override { return E_NOTIMPL; }

  // IMFGetService -- mandatory, and entitled to say it serves nothing.
  STDMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override {
    return MF_E_UNSUPPORTED_SERVICE;
  }

  // IKsControl -- the pipeline routes camera controls through here. This
  // camera has none.
  STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* bytesReturned) override {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }
  STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* bytesReturned) override {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }
  STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* bytesReturned) override {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }

 private:
  ~VCamSource();

  std::atomic<ULONG> refs_{1};
  std::mutex mutex_;
  bool shutdown_ = false;

  IMFMediaEventQueue* queue_ = nullptr;
  IMFAttributes* attributes_ = nullptr;
  IMFAttributes* streamAttributes_ = nullptr;
  IMFStreamDescriptor* descriptor_ = nullptr;
  IMFPresentationDescriptor* presentation_ = nullptr;
  VCamStream* stream_ = nullptr;
};

// --------------------------------- stream ----------------------------------

VCamStream::VCamStream(VCamSource* source, IMFStreamDescriptor* descriptor)
    : source_(source), descriptor_(descriptor) {
  descriptor_->AddRef();
  ::MFCreateEventQueue(&queue_);
  AddLiveObject();
}

VCamStream::~VCamStream() {
  SafeRelease(&queue_);
  SafeRelease(&descriptor_);
  ReleaseLiveObject();
}

STDMETHODIMP VCamStream::GetMediaSource(IMFMediaSource** source) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (!source) return E_POINTER;
  return source_->QueryInterface(IID_PPV_ARGS(source));
}

HRESULT VCamStream::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  frames_.Open();
  frames_.Announce(format_.width, format_.height, format_.fps, true);
  picture_.assign((size_t)format_.width * format_.height * 3 / 2, 0);
  // Mid grey in Y and neutral chroma, so a consumer that connects before
  // CapView has published anything sees a blank picture rather than noise.
  ::memset(picture_.data(), 16, (size_t)format_.width * format_.height);
  ::memset(picture_.data() + (size_t)format_.width * format_.height, 128,
           (size_t)format_.width * format_.height / 2);
  havePicture_ = false;
  lastIndex_ = 0;
  nextTime_ = 0;
  state_ = MF_STREAM_STATE_RUNNING;
  return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT VCamStream::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (state_ == MF_STREAM_STATE_RUNNING) frames_.Announce(0, 0, 0, false);
  state_ = MF_STREAM_STATE_STOPPED;
  return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT VCamStream::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (state_ == MF_STREAM_STATE_RUNNING) frames_.Announce(0, 0, 0, false);
  shutdown_ = true;
  state_ = MF_STREAM_STATE_STOPPED;
  if (queue_) queue_->Shutdown();
  frames_.Close();
  source_ = nullptr;
  return S_OK;
}

HRESULT VCamStream::SetStreamState(MF_STREAM_STATE state) {
  switch (state) {
    case MF_STREAM_STATE_RUNNING:
      return Start();
    case MF_STREAM_STATE_STOPPED:
    case MF_STREAM_STATE_PAUSED:
      return Stop();
    default:
      return E_INVALIDARG;
  }
}

HRESULT VCamStream::ProduceSample(IMFSample** out) {
  *out = nullptr;
  const size_t bytes = (size_t)format_.width * format_.height * 3 / 2;

  // Nothing new is not a failure. A camera that stops answering stalls the
  // consumer; one that repeats its last picture merely looks frozen, which is
  // the truth of the matter.
  if (frames_.ReadNewest(picture_.data(), format_.width, format_.height, &lastIndex_)) {
    havePicture_ = true;
  }

  IMFMediaBuffer* buffer = nullptr;
  HRESULT hr = ::MFCreateMemoryBuffer((DWORD)bytes, &buffer);
  if (FAILED(hr)) return hr;

  BYTE* dst = nullptr;
  DWORD maxLen = 0;
  hr = buffer->Lock(&dst, &maxLen, nullptr);
  if (SUCCEEDED(hr)) {
    ::memcpy(dst, picture_.data(), bytes);
    buffer->Unlock();
    hr = buffer->SetCurrentLength((DWORD)bytes);
  }

  IMFSample* sample = nullptr;
  if (SUCCEEDED(hr)) hr = ::MFCreateSample(&sample);
  if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);

  if (SUCCEEDED(hr)) {
    const LONGLONG duration = 10000000LL / (format_.fps ? format_.fps : 30);
    if (nextTime_ == 0) nextTime_ = ::MFGetSystemTime();
    sample->SetSampleTime(nextTime_);
    sample->SetSampleDuration(duration);
    nextTime_ += duration;
    *out = sample;
    sample = nullptr;
  }

  SafeRelease(&sample);
  SafeRelease(&buffer);
  return hr;
}

STDMETHODIMP VCamStream::RequestSample(IUnknown* token) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

  IMFSample* sample = nullptr;
  HRESULT hr = ProduceSample(&sample);
  if (FAILED(hr)) return hr;

  if (token) sample->SetUnknown(MFSampleExtension_Token, token);

  PROPVARIANT value = {};
  value.vt = VT_UNKNOWN;
  value.punkVal = sample;  // the queue takes its own reference
  hr = queue_->QueueEventParamVar(MEMediaSample, GUID_NULL, S_OK, &value);
  sample->Release();
  return hr;
}

// --------------------------------- source ----------------------------------

VCamSource::~VCamSource() {
  SafeRelease(&queue_);
  SafeRelease(&attributes_);
  SafeRelease(&streamAttributes_);
  SafeRelease(&descriptor_);
  SafeRelease(&presentation_);
  if (stream_) {
    stream_->Release();
    stream_ = nullptr;
  }
  ReleaseLiveObject();
}

HRESULT VCamSource::Init() {
  // Deliberately cheap. Windows builds its sensor group by creating every
  // custom source and asking what it offers, and doing real work here would
  // cost that on every enumeration.
  HRESULT hr = ::MFCreateEventQueue(&queue_);
  if (FAILED(hr)) return hr;

  hr = ::MFCreateAttributes(&attributes_, 4);
  if (FAILED(hr)) return hr;

  std::vector<IMFMediaType*> types;
  for (uint32_t i = 0; i < kFormatCount; ++i) {
    IMFMediaType* type = nullptr;
    hr = MakeType(kFormats[i], &type);
    if (FAILED(hr)) break;
    types.push_back(type);
  }

  if (SUCCEEDED(hr)) {
    hr = ::MFCreateStreamDescriptor(0, (DWORD)types.size(), types.data(), &descriptor_);
  }
  for (IMFMediaType* type : types) type->Release();
  if (FAILED(hr)) return hr;

  IMFMediaTypeHandler* handler = nullptr;
  hr = descriptor_->GetMediaTypeHandler(&handler);
  if (SUCCEEDED(hr)) {
    IMFMediaType* first = nullptr;
    if (SUCCEEDED(handler->GetMediaTypeByIndex(0, &first))) {
      handler->SetCurrentMediaType(first);
      first->Release();
    }
    handler->Release();
  }

  // The two attributes the frame server insists on, plus the one that lets
  // several applications read the camera at once.
  hr = descriptor_->QueryInterface(IID_PPV_ARGS(&streamAttributes_));
  if (SUCCEEDED(hr)) {
    streamAttributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    streamAttributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    streamAttributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  }
  if (FAILED(hr)) return hr;

  IMFStreamDescriptor* descriptors[] = {descriptor_};
  hr = ::MFCreatePresentationDescriptor(1, descriptors, &presentation_);
  if (FAILED(hr)) return hr;

  stream_ = new (std::nothrow) VCamStream(this, descriptor_);
  return stream_ ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VCamSource::CreatePresentationDescriptor(IMFPresentationDescriptor** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (!out) return E_POINTER;
  return presentation_->Clone(out);
}

STDMETHODIMP VCamSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                               const PROPVARIANT* start) {
  if (!pd) return E_INVALIDARG;
  if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

  VCamStream* stream = nullptr;
  CameraFormat chosen = kFormats[0];
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;

    // Which format did the consumer settle on? That is what CapView has to
    // produce, so it has to be read off the descriptor rather than assumed.
    BOOL selected = FALSE;
    IMFStreamDescriptor* sd = nullptr;
    if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd)) && sd) {
      IMFMediaTypeHandler* handler = nullptr;
      if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) {
        IMFMediaType* current = nullptr;
        if (SUCCEEDED(handler->GetCurrentMediaType(&current)) && current) {
          UINT32 w = 0, h = 0, num = 30, den = 1;
          ::MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
          ::MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &num, &den);
          if (w && h) {
            chosen.width = w;
            chosen.height = h;
            chosen.fps = den ? num / den : 30;
          }
          current->Release();
        }
        handler->Release();
      }
      sd->Release();
    }

    stream_->SetFormat(chosen);
    stream = stream_;
    stream->AddRef();
  }

  // MENewStream hands the pipeline the stream object; it must carry the
  // stream as an IUnknown and must be queued before the source reports started.
  PROPVARIANT value = {};
  value.vt = VT_UNKNOWN;
  value.punkVal = static_cast<IMFMediaStream2*>(stream);
  HRESULT hr = QueueEvent(MENewStream, GUID_NULL, S_OK, &value);

  if (SUCCEEDED(hr)) hr = stream->Start();

  if (SUCCEEDED(hr)) {
    PROPVARIANT time = {};
    time.vt = VT_I8;
    time.hVal.QuadPart = start && start->vt == VT_I8 ? start->hVal.QuadPart : 0;
    hr = QueueEvent(MESourceStarted, GUID_NULL, S_OK, &time);
  }
  stream->Release();
  return hr;
}

STDMETHODIMP VCamSource::Stop() {
  VCamStream* stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    stream = stream_;
    if (stream) stream->AddRef();
  }
  if (stream) {
    stream->Stop();
    stream->Release();
  }
  return QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP VCamSource::Shutdown() {
  VCamStream* stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    shutdown_ = true;
    stream = stream_;
    stream_ = nullptr;
  }
  if (stream) {
    stream->Shutdown();
    stream->Release();
  }
  if (queue_) queue_->Shutdown();
  return S_OK;
}

// -------------------------------- factory ----------------------------------

class SourceFactory : public IClassFactory {
 public:
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
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;

    VCamSource* source = new (std::nothrow) VCamSource();
    if (!source) return E_OUTOFMEMORY;

    HRESULT hr = source->Init();
    if (SUCCEEDED(hr)) hr = source->QueryInterface(riid, out);
    source->Release();
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
  std::atomic<ULONG> refs_{1};
};

}  // namespace

HRESULT CreateSourceClassFactory(REFIID riid, void** out) {
  SourceFactory* factory = new (std::nothrow) SourceFactory();
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, out);
  factory->Release();
  return hr;
}

long LiveObjectCount() { return g_objects.load(); }
void AddLiveObject() { ++g_objects; }
void ReleaseLiveObject() { --g_objects; }

}  // namespace vcam
}  // namespace cap
