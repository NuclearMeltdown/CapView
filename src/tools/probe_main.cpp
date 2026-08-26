// capview_probe: prints what CapView sees. Useful when a card behaves oddly --
// it lists every video device, the formats and inputs it reports, every audio
// endpoint, and which audio endpoint CapView would pair with which card.

#include <mmdeviceapi.h>
// Must come after mmdeviceapi.h: it needs the PROPERTYKEY macros pulled in there.
#include <functiondiscoverykeys_devpkey.h>

#include <ks.h>
#include <ksmedia.h>
#include <olectl.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_devices.h"
#include "capture/dshow_util.h"
#include "capture/video_capture.h"
#include "common.h"
#include "record/ffmpeg_locator.h"
#include "tools/mf_probe.h"

using namespace cap;

namespace {

// What the driver offers as its own settings dialog, and on which object. Cards
// differ: some hang everything off the filter, some off the capture pin, some
// off neither and expect you to use their own tool.
void PrintPropertyPages(const VideoDeviceInfo& info) {
  ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(info);
  if (!filter) {
    std::printf("    Konfigurationsseiten: Filter konnte nicht erzeugt werden\n");
    return;
  }

  auto report = [](const char* what, IUnknown* object) {
    if (!object) return;
    ComPtr<ISpecifyPropertyPages> spec;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&spec)))) {
      std::printf("    %-22s kein ISpecifyPropertyPages\n", what);
      return;
    }
    CAUUID ca = {};
    const HRESULT hr = spec->GetPages(&ca);
    if (FAILED(hr)) {
      std::printf("    %-22s GetPages 0x%08lX\n", what, (unsigned long)hr);
      return;
    }
    std::printf("    %-22s %lu Seite(n)\n", what, (unsigned long)ca.cElems);
    for (ULONG i = 0; i < ca.cElems; ++i) {
      wchar_t guid[64] = {};
      ::StringFromGUID2(ca.pElems[i], guid, 64);
      std::printf("      %ls\n", guid);
    }
    if (ca.pElems) ::CoTaskMemFree(ca.pElems);
  };

  report("Filter:", filter.Get());

  ComPtr<IEnumPins> pins;
  if (SUCCEEDED(filter->EnumPins(&pins))) {
    ComPtr<IPin> pin;
    int index = 0;
    while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
      PIN_INFO pi = {};
      char label[64];
      if (SUCCEEDED(pin->QueryPinInfo(&pi))) {
        std::snprintf(label, sizeof(label), "Pin %d (%ls):", index,
                      pi.achName[0] ? pi.achName : L"?");
        if (pi.pFilter) pi.pFilter->Release();
      } else {
        std::snprintf(label, sizeof(label), "Pin %d:", index);
      }
      report(label, pin.Get());
      ++index;
    }
  }
}

// Does a property page load and accept the filter? That is the step that fails
// silently when the object has crossed an apartment boundary: a vendor page
// queries the filter for an interface of its own, the proxy has no marshaller
// for it, SetObjects returns an error and the frame comes up empty.
//
// Run with: capview_probe pages
void TestPropertyPages() {
  std::printf("========== Konfigurationsseiten im Detail ==========\n");
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) continue;
    ComPtr<ISpecifyPropertyPages> spec;
    if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&spec)))) continue;
    CAUUID ca = {};
    if (FAILED(spec->GetPages(&ca)) || ca.cElems == 0) continue;

    std::printf("\n%s\n", d.name.c_str());

    // 1) same apartment as the filter, raw pointer
    for (ULONG i = 0; i < ca.cElems; ++i) {
      wchar_t guid[64] = {};
      ::StringFromGUID2(ca.pElems[i], guid, 64);
      ComPtr<IPropertyPage> page;
      HRESULT hr = ::CoCreateInstance(ca.pElems[i], nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&page));
      if (FAILED(hr)) {
        std::printf("  MTA roh    %ls  CoCreateInstance 0x%08lX\n", guid, (unsigned long)hr);
        continue;
      }
      IUnknown* unk = filter.Get();
      hr = page->SetObjects(1, &unk);
      std::printf("  MTA roh    %ls  SetObjects 0x%08lX%s\n", guid, (unsigned long)hr,
                  SUCCEEDED(hr) ? "  ok" : "  FEHLER");
      page->SetObjects(0, nullptr);
    }

    // 2) apartment threaded thread, pointer put through the marshaller
    // 3) apartment threaded thread, pointer handed over as it is
    for (int mode = 0; mode < 2; ++mode) {
      IStream* stream = nullptr;
      if (mode == 0 &&
          FAILED(::CoMarshalInterThreadInterfaceInStream(IID_IBaseFilter, filter.Get(), &stream))) {
        std::printf("  STA %s  CoMarshalInterThreadInterfaceInStream fehlgeschlagen\n",
                    mode == 0 ? "marshal" : "roh    ");
        continue;
      }
      IBaseFilter* raw = filter.Get();
      CAUUID pages = ca;
      std::thread([stream, raw, pages, mode]() {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ComPtr<IBaseFilter> f;
        if (stream) {
          ::CoGetInterfaceAndReleaseStream(stream, IID_PPV_ARGS(&f));
        } else {
          f = raw;
        }
        for (ULONG i = 0; i < pages.cElems && f; ++i) {
          wchar_t guid[64] = {};
          ::StringFromGUID2(pages.pElems[i], guid, 64);
          ComPtr<IPropertyPage> page;
          HRESULT hr = ::CoCreateInstance(pages.pElems[i], nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&page));
          if (FAILED(hr)) {
            std::printf("  STA %s %ls  CoCreateInstance 0x%08lX\n",
                        mode == 0 ? "marshal" : "roh    ", guid, (unsigned long)hr);
            continue;
          }
          IUnknown* unk = f.Get();
          hr = page->SetObjects(1, &unk);
          std::printf("  STA %s %ls  SetObjects 0x%08lX%s\n", mode == 0 ? "marshal" : "roh    ",
                      guid, (unsigned long)hr, SUCCEEDED(hr) ? "  ok" : "  FEHLER");
          page->SetObjects(0, nullptr);
        }
        f.Reset();
        ::CoUninitialize();
      }).join();
    }
    ::CoTaskMemFree(ca.pElems);
  }
  std::printf("\n");
}

// Opens the property frame for real and reports what came up: which windows the
// dialog actually built, and how big they are. A page that failed to activate
// leaves the frame with its buttons and nothing in the middle, which is exactly
// what "opens but is blank" looks like from the outside.
//
// Run with: capview_probe frame [ole|co] [pin]
struct FrameProbe {
  bool useOle = true;
  bool includePin = false;
  DWORD threadId = 0;
};

BOOL CALLBACK DumpChild(HWND hwnd, LPARAM depthParam) {
  const int depth = (int)depthParam;
  wchar_t cls[128] = {};
  wchar_t text[128] = {};
  ::GetClassNameW(hwnd, cls, 128);
  ::GetWindowTextW(hwnd, text, 128);
  RECT r = {};
  ::GetWindowRect(hwnd, &r);
  std::printf("%*s%-24ls %4ldx%-4ld %s  \"%ls\"\n", depth * 2, "", cls, r.right - r.left,
              r.bottom - r.top, ::IsWindowVisible(hwnd) ? "sichtbar" : "versteckt", text);
  if (depth < 3) ::EnumChildWindows(hwnd, DumpChild, depthParam + 1);
  return TRUE;
}

BOOL CALLBACK DumpThreadWindow(HWND hwnd, LPARAM) {
  DumpChild(hwnd, 0);
  ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  return TRUE;
}

void TestPropertyFrame(bool useOle, bool includePin) {
  std::printf("========== Property-Frame: %s, Pin %s ==========\n",
              useOle ? "OleInitialize" : "CoInitializeEx",
              includePin ? "dabei" : "nicht dabei");

  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    if (d.name.find("SA7160") == std::string::npos) continue;
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) {
      std::printf("Filter konnte nicht erzeugt werden\n");
      return;
    }

    std::vector<GUID> pages;
    std::vector<IUnknown*> objects;
    ComPtr<ISpecifyPropertyPages> spec;
    CAUUID ca = {};
    if (SUCCEEDED(filter->QueryInterface(IID_PPV_ARGS(&spec))) && SUCCEEDED(spec->GetPages(&ca))) {
      for (ULONG i = 0; i < ca.cElems; ++i) pages.push_back(ca.pElems[i]);
      if (ca.pElems) ::CoTaskMemFree(ca.pElems);
      objects.push_back(filter.Get());
    }
    ComPtr<IPin> pin = FindPinByDirection(filter.Get(), PINDIR_OUTPUT);
    if (includePin && pin) {
      ComPtr<ISpecifyPropertyPages> pinSpec;
      CAUUID pinCa = {};
      if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&pinSpec))) &&
          SUCCEEDED(pinSpec->GetPages(&pinCa))) {
        for (ULONG i = 0; i < pinCa.cElems; ++i) pages.push_back(pinCa.pElems[i]);
        if (pinCa.pElems) ::CoTaskMemFree(pinCa.pElems);
        objects.push_back(pin.Get());
      }
    }
    std::printf("%d Seiten, %d Objekte\n", (int)pages.size(), (int)objects.size());

    DWORD tid = 0;
    HRESULT frameHr = E_FAIL;
    std::thread worker([&]() {
      tid = ::GetCurrentThreadId();
      const HRESULT init = useOle ? ::OleInitialize(nullptr)
                                  : ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                                  COINIT_DISABLE_OLE1DDE);
      frameHr = ::OleCreatePropertyFrame(nullptr, 0, 0, L"Test", (ULONG)objects.size(),
                                         objects.data(), (ULONG)pages.size(), pages.data(), 0, 0,
                                         nullptr);
      if (SUCCEEDED(init)) {
        if (useOle) ::OleUninitialize(); else ::CoUninitialize();
      }
    });

    ::Sleep(3000);
    std::printf("--- Fenster des Dialogthreads ---\n");
    if (tid) ::EnumThreadWindows(tid, DumpThreadWindow, 0);
    worker.join();
    std::printf("OleCreatePropertyFrame: 0x%08lX\n\n", (unsigned long)frameHr);
    return;
  }
  std::printf("SA7160 nicht gefunden\n");
}

// What the analogue decoder can be told and what it reports back. This is the
// interface behind the video standard list in the driver's dialog, and the two
// read-only values are the interesting part: whether it has locked onto a
// signal, and how many lines it thinks it is receiving.
//
// Run with: capview_probe decoder
void TestAnalogDecoder() {
  static const struct { long bit; const char* name; } kStandards[] = {
      {AnalogVideo_NTSC_M, "NTSC_M"},         {AnalogVideo_NTSC_M_J, "NTSC_M_J (Japan)"},
      {AnalogVideo_NTSC_433, "NTSC_433"},     {AnalogVideo_PAL_B, "PAL_B"},
      {AnalogVideo_PAL_D, "PAL_D"},           {AnalogVideo_PAL_G, "PAL_G"},
      {AnalogVideo_PAL_H, "PAL_H"},           {AnalogVideo_PAL_I, "PAL_I"},
      {AnalogVideo_PAL_M, "PAL_M"},           {AnalogVideo_PAL_N, "PAL_N"},
      {AnalogVideo_PAL_60, "PAL_60"},         {AnalogVideo_SECAM_B, "SECAM_B"},
      {AnalogVideo_SECAM_D, "SECAM_D"},       {AnalogVideo_SECAM_G, "SECAM_G"},
      {AnalogVideo_SECAM_H, "SECAM_H"},       {AnalogVideo_SECAM_K, "SECAM_K"},
      {AnalogVideo_SECAM_K1, "SECAM_K1"},     {AnalogVideo_SECAM_L, "SECAM_L"},
      {AnalogVideo_SECAM_L1, "SECAM_L1"},     {AnalogVideo_PAL_N_COMBO, "PAL_N_COMBO"},
  };

  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) continue;
    ComPtr<IAMAnalogVideoDecoder> dec;
    if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&dec)))) continue;

    std::printf("\n%s\n", d.name.c_str());

    long available = 0;
    if (SUCCEEDED(dec->get_AvailableTVFormats(&available))) {
      std::printf("  Unterstützte Normen (0x%08lX):\n", (unsigned long)available);
      for (const auto& s : kStandards) {
        if (available & s.bit) std::printf("    %s\n", s.name);
      }
    } else {
      std::printf("  get_AvailableTVFormats fehlgeschlagen\n");
    }

    long current = 0;
    if (SUCCEEDED(dec->get_TVFormat(&current))) {
      const char* name = "?";
      for (const auto& s : kStandards) {
        if (current == s.bit) name = s.name;
      }
      std::printf("  Aktuell: %s (0x%08lX)\n", name, (unsigned long)current);
    }

    long locked = -1;
    HRESULT hr = dec->get_HorizontalLocked(&locked);
    std::printf("  get_HorizontalLocked: 0x%08lX  -> %ld\n", (unsigned long)hr, locked);

    long lines = -1;
    hr = dec->get_NumberOfLines(&lines);
    std::printf("  get_NumberOfLines:    0x%08lX  -> %ld\n", (unsigned long)hr, lines);

    long vcr = -1;
    hr = dec->get_VCRHorizontalLocking(&vcr);
    std::printf("  get_VCRHorizontalLocking: 0x%08lX -> %ld\n", (unsigned long)hr, vcr);
  }
  std::printf("\n");
}

// Does get_NumberOfLines report the signal or merely echo the standard that was
// set? That decides whether CapView can tell PAL from PAL-60 by itself or
// whether it has to be told. Sets two standards that cannot both be true of one
// source and watches what comes back, then puts the card back as it was.
//
// Run with: capview_probe lines
void TestLineReporting() {
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) continue;
    ComPtr<IAMAnalogVideoDecoder> dec;
    if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&dec)))) continue;

    long original = 0;
    if (FAILED(dec->get_TVFormat(&original))) continue;
    std::printf("\n%s  (Ausgangswert 0x%08lX)\n", d.name.c_str(), (unsigned long)original);

    auto probe = [&](long standard, const char* name) {
      const HRESULT put = dec->put_TVFormat(standard);
      ::Sleep(1200);  // give the decoder time to relock
      long lines = -1, locked = -1;
      dec->get_NumberOfLines(&lines);
      dec->get_HorizontalLocked(&locked);
      std::printf("  %-10s put 0x%08lX   Zeilen %4ld   H-Lock %ld\n", name, (unsigned long)put,
                  lines, locked);
    };

    probe(AnalogVideo_NTSC_M, "NTSC_M");
    probe(AnalogVideo_PAL_B, "PAL_B");
    probe(AnalogVideo_PAL_60, "PAL_60");

    dec->put_TVFormat(original);
    ::Sleep(500);
    long back = 0;
    dec->get_TVFormat(&back);
    std::printf("  zurueckgesetzt auf 0x%08lX %s\n", (unsigned long)back,
                back == original ? "(ok)" : "(ABWEICHUNG!)");
  }
  std::printf("\n");
}

// Is the input selector reachable at all? IAMCrossbar is the documented way and
// this card does not offer it, but a WDM driver can also carry the crossbar as a
// KS property set on the filter itself. If that is there, CapView could list and
// switch HDMI / component / composite by itself instead of sending people into
// the vendor's dialog.
//
// Run with: capview_probe inputs
void TestInputSelection() {
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) continue;
    std::printf("\n%s\n", d.name.c_str());

    ComPtr<IAMCrossbar> xbar;
    if (SUCCEEDED(filter->QueryInterface(IID_PPV_ARGS(&xbar)))) {
      long in = 0, out = 0;
      xbar->get_PinCounts(&out, &in);
      std::printf("  IAMCrossbar direkt am Filter: %ld Eingaenge, %ld Ausgaenge\n", in, out);
    } else {
      std::printf("  IAMCrossbar direkt am Filter: nein\n");
    }

    auto tryKs = [](IUnknown* object, const char* what) {
      if (!object) return;
      ComPtr<IKsPropertySet> ks;
      if (FAILED(object->QueryInterface(IID_PPV_ARGS(&ks)))) {
        std::printf("  %-18s kein IKsPropertySet\n", what);
        return;
      }
      DWORD support = 0;
      const HRESULT hr = ks->QuerySupported(PROPSETID_VIDCAP_CROSSBAR,
                                            KSPROPERTY_CROSSBAR_CAPS, &support);
      std::printf("  %-18s IKsPropertySet ja, Crossbar-Set 0x%08lX (support 0x%lX)\n", what,
                  (unsigned long)hr, (unsigned long)support);

      DWORD videoDecoder = 0;
      const HRESULT hr2 = ks->QuerySupported(PROPSETID_VIDCAP_VIDEODECODER,
                                             KSPROPERTY_VIDEODECODER_STANDARD, &videoDecoder);
      std::printf("  %-18s Videodecoder-Set 0x%08lX (support 0x%lX)\n", what,
                  (unsigned long)hr2, (unsigned long)videoDecoder);
    };

    tryKs(filter.Get(), "Filter:");
    ComPtr<IEnumPins> pins;
    if (SUCCEEDED(filter->EnumPins(&pins))) {
      ComPtr<IPin> pin;
      int index = 0;
      while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        char label[32];
        std::snprintf(label, sizeof(label), "Pin %d:", index++);
        tryKs(pin.Get(), label);
      }
    }
  }
  std::printf("\n");
}

void PrintVideoDevices() {
  std::printf("================ Videogeräte ================\n");
  std::vector<VideoDeviceInfo> devices = EnumerateVideoDevices();
  if (devices.empty()) {
    std::printf("  (keine gefunden)\n\n");
    return;
  }

  for (size_t i = 0; i < devices.size(); ++i) {
    const VideoDeviceInfo& d = devices[i];
    std::printf("\n[%zu] %s\n", i, d.name.c_str());
    std::printf("    DevicePath : %s\n", d.id.c_str());

    DeviceProbeResult probed = VideoCapture::Probe(DeviceRef{d.name, d.id});
    if (!probed.ok) {
      std::printf("    ! Konnte nicht abgefragt werden: %s\n", probed.error.c_str());
      continue;
    }

    PrintPropertyPages(d);

    std::printf("    Crossbar-Eingänge: ");
    if (probed.crossbarInputs.empty()) {
      std::printf("keine\n");
    } else {
      std::printf("\n");
      for (size_t k = 0; k < probed.crossbarInputs.size(); ++k) {
        std::printf("        [%zu] %s  (Pin %d, Typ %ld)\n", k,
                    probed.crossbarInputs[k].name.c_str(), probed.crossbarInputs[k].pinIndex,
                    probed.crossbarInputs[k].physicalType);
      }
    }

    std::printf("    Gemeldete Capability-Einträge: %zu\n", probed.caps.entries().size());
    for (const auto& e : probed.caps.entries()) {
      std::printf("        %-6s %5dx%-5d  %6.2f fps  (Bereich %.2f-%.2f)  "
                  "Größe %d-%d x %d-%d  Schritt %d/%d\n",
                  e.subtypeLabel.c_str(), e.width, e.height, e.defaultFps, e.minFps, e.maxFps,
                  e.minWidth, e.maxWidth, e.minHeight, e.maxHeight, e.granularityX,
                  e.granularityY);
    }

    std::printf("    Auswahl, wie CapView sie anbietet:\n");
    for (const std::string& sub : probed.caps.Subtypes()) {
      std::printf("      Format %s\n", sub.c_str());
      const std::vector<ResolutionOption> resolutions = probed.caps.Resolutions(sub);
      for (const auto& r : resolutions) {
        std::printf("        %4dx%-4d%s", r.width, r.height, r.forced ? " [erzwungen]" : "");
        std::vector<FpsOption> fps = probed.caps.FpsList(sub, r.width, r.height);
        std::printf("   fps:");
        for (const auto& f : fps) {
          if (f.highest) {
            std::printf(" [höchste=%.2f]", probed.caps.HighestFps(sub, r.width, r.height));
          } else {
            std::printf(" %.2f%s", f.fps, f.forced ? "*" : "");
          }
        }
        std::printf("\n");
      }
    }

    // Colour description, when the driver bothers to fill it in. This is the
    // only place an HDR source announces itself, so it is worth showing even
    // though most cards leave it empty.
    const VideoFormatInfo& colour = probed.colorInfo;
    if (colour.colorInfoPresent) {
      std::printf("    Farbbeschreibung des Treibers:\n");
      std::printf("        Wertebereich    : %s\n", NominalRangeName(colour.nominalRange));
      std::printf("        Matrix          : %s\n", TransferMatrixName(colour.transferMatrix));
      std::printf("        Primärvalenzen  : %s\n", PrimariesName(colour.primaries));
      std::printf("        Transferfunktion: %s%s\n",
                  TransferFunctionName(colour.transferFunction),
                  colour.isHdrTransfer() ? "   <- HDR" : "");
    } else {
      std::printf("    Farbbeschreibung: keine (CapView rät dann anhand der Bildhöhe)\n");
    }

    FormatSel def = probed.caps.PickDefault();
    std::printf("    Standardauswahl: %s\n", def.Label().c_str());
  }
  std::printf("\n  (* = nicht vom Treiber gemeldet, wird als erzwungen angeboten)\n\n");
}

void PrintDShowAudioDevices() {
  std::printf("================ DirectShow-Audioeingänge ================\n");
  std::vector<VideoDeviceInfo> devices = EnumerateAudioCaptureDShowDevices();
  if (devices.empty()) {
    std::printf("  (keine gefunden)\n\n");
    return;
  }
  for (size_t i = 0; i < devices.size(); ++i) {
    std::printf("  [%zu] %s\n", i, devices[i].name.c_str());
    std::printf("       DevicePath: %s\n",
                devices[i].id.empty() ? "(keiner)" : devices[i].id.c_str());
  }
  std::printf("\n");
}

const char* StateName(DWORD state) {
  switch (state) {
    case DEVICE_STATE_ACTIVE: return "aktiv";
    case DEVICE_STATE_DISABLED: return "deaktiviert";
    case DEVICE_STATE_NOTPRESENT: return "nicht vorhanden";
    case DEVICE_STATE_UNPLUGGED: return "nicht angeschlossen";
    default: return "unbekannt";
  }
}

// Raw dump straight from WASAPI, including endpoints CapView normally skips,
// so a card whose audio input is merely disabled is easy to spot.
void PrintAllEndpoints(bool capture) {
  std::printf("================ WASAPI %s (alle Zustände) ================\n",
              capture ? "Aufnahme" : "Wiedergabe");

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
    std::printf("  ! Enumerator nicht verfügbar\n\n");
    return;
  }

  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATEMASK_ALL,
                                            &collection))) {
    std::printf("  ! Aufzählung fehlgeschlagen\n\n");
    return;
  }

  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;

    DWORD state = 0;
    device->GetState(&state);

    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) continue;

    PROPVARIANT var;
    ::PropVariantInit(&var);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
      name = ToUtf8(var.pwszVal);
    }
    ::PropVariantClear(&var);

    std::printf("  [%u] %-52s %s\n", i, name.c_str(), StateName(state));

    // Report the property read verbatim, so an empty instance id is
    // distinguishable from a failed read.
    ::PropVariantInit(&var);
    HRESULT hr = store->GetValue(PKEY_Device_InstanceId, &var);
    std::printf("       InstanceId: hr=0x%08X vt=%u  %s\n", (unsigned)hr, (unsigned)var.vt,
                (SUCCEEDED(hr) && var.vt == VT_LPWSTR) ? ToUtf8(var.pwszVal).c_str() : "(leer)");
    ::PropVariantClear(&var);
  }
  std::printf("\n");
}

void PrintEmbeddedMatches() {
  std::printf("================ Zuordnung eingebettetes Audio ================\n");
  std::vector<VideoDeviceInfo> devices = EnumerateVideoDevices();
  for (const VideoDeviceInfo& d : devices) {
    AudioDeviceInfo found;
    if (FindEmbeddedAudioDevice(d, &found)) {
      std::printf("  %s\n      -> %s\n", d.name.c_str(), found.name.c_str());
    } else {
      std::printf("  %s\n      -> nichts gefunden\n", d.name.c_str());
    }
  }
  std::printf("\n");
}

void PrintFfmpeg() {
  std::printf("================ ffmpeg ================\n");
  FfmpegInfo info = LocateFfmpeg({});
  if (!info.found) {
    std::printf("  nicht gefunden.\n");
    std::printf("  Gesucht wurde: <CapView>\\ffmpeg\\bin, <CapView>\\ffmpeg,\n");
    std::printf("                 <CapView>, dann PATH.\n");
    std::printf("  Ohne ffmpeg ist die Aufnahme nicht verfügbar.\n\n");
    return;
  }
  std::printf("  Pfad   : %s\n", info.path.c_str());
  std::printf("  Version: %s\n", info.version.c_str());
  std::printf("  Encoder werden getestet, das dauert einen Moment ...\n");

  ProbeEncoders(&info);
  for (const EncoderInfo& e : info.encoders) {
    const std::string note = e.error.empty() ? std::string() : ("  (" + e.error + ")");
    std::printf("    %-28s %-9s %s%s\n", e.label.c_str(), e.hardware ? "Hardware" : "CPU",
                e.available ? "verfügbar" : "nicht verfügbar", note.c_str());
  }
  const EncoderInfo* best = info.BestAvailable();
  std::printf("  Automatik würde nehmen: %s\n\n", best ? best->label.c_str() : "nichts");
}

}  // namespace

int main(int argc, char** argv) {
  ::SetConsoleOutputCP(CP_UTF8);
  ComScope com(COINIT_MULTITHREADED);
  if (!com.ok()) {
    std::printf("COM konnte nicht initialisiert werden.\n");
    return 1;
  }

  if (argc > 1 && std::string(argv[1]) == "inputs") {
    TestInputSelection();
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "lines") {
    TestLineReporting();
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "decoder") {
    TestAnalogDecoder();
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "frame") {
    const bool ole = argc < 3 || std::string(argv[2]) != "co";
    const bool pin = argc > 3 && std::string(argv[3]) == "pin";
    TestPropertyFrame(ole, pin);
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "pages") {
    TestPropertyPages();
    return 0;
  }

  PrintVideoDevices();
  PrintDShowAudioDevices();
  PrintAllEndpoints(true);
  PrintAllEndpoints(false);
  PrintEmbeddedMatches();
  PrintMediaFoundationDevices();
  PrintFfmpeg();
  return 0;
}
