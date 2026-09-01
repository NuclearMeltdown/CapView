// capview_probe: prints what CapView sees. Useful when a card behaves oddly --
// it lists every video device, the formats and inputs it reports, every audio
// endpoint, and which audio endpoint CapView would pair with which card.

#include <mmdeviceapi.h>
// Must come after mmdeviceapi.h: it needs the PROPERTYKEY macros pulled in there.
#include <functiondiscoverykeys_devpkey.h>

#include <ks.h>
#include <ksmedia.h>
#include <olectl.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
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

  // Und dieselbe Frage von der anderen Seite. Bisher wurde nur der Capture-
  // Filter gefragt und der Graph nach oben abgesucht; ein WDM-Crossbar ist aber
  // ein eigener Filter in einer eigenen Kategorie, und ob der Graph-Builder ihn
  // von einem bestimmten Capture-Filter aus findet, ist eine andere Frage als
  // ob er ueberhaupt existiert. Ein Programm, das die Eingaenge einer Karte
  // anzeigt, an der CapView "keine umschaltbaren Eingaenge" meldet, muss ihn
  // irgendwo herhaben.
  std::printf("\n================ Crossbar-Kategorie ================\n");
  const std::vector<VideoDeviceInfo> crossbars = EnumerateCrossbarDevices();
  if (crossbars.empty()) {
    std::printf("  (kein Crossbar registriert)\n");
  }
  for (const VideoDeviceInfo& d : crossbars) {
    std::printf("\n%s\n    %s\n", d.name.c_str(), d.id.c_str());
    ComPtr<IBaseFilter> filter = CreateFilterFromMoniker(d);
    if (!filter) {
      std::printf("    ! Filter liess sich nicht erzeugen\n");
      continue;
    }
    ComPtr<IAMCrossbar> xbar;
    if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&xbar)))) {
      std::printf("    ! kein IAMCrossbar am Filter dieser Kategorie\n");
      continue;
    }
    long outPins = 0, inPins = 0;
    xbar->get_PinCounts(&outPins, &inPins);
    std::printf("    IAMCrossbar: %ld Eingaenge, %ld Ausgaenge\n", inPins, outPins);
    for (long i = 0; i < inPins; ++i) {
      long related = 0, physType = 0;
      if (FAILED(xbar->get_CrossbarPinInfo(TRUE, i, &related, &physType))) continue;
      std::printf("      [%ld] %-22s Typ %ld, verwandter Pin %ld\n", i,
                  PhysicalConnectorName(physType).c_str(), physType, related);
    }
  }

  // Und noch eine Ebene tiefer. EnumerateVideoDevices fragt nach
  // CLSID_VideoInputDeviceCategory -- das ist die Liste, die jedes
  // Aufnahmeprogramm zeigt. Ein Treiber registriert seine KS-Filter aber unter
  // KSCATEGORY_VIDEO und KSCATEGORY_CAPTURE, und die Videoliste ist nur der
  // Schnitt daraus, den der KS-Proxy nach oben reicht. Weichen die Listen
  // voneinander ab, gibt es Geraete, die ein Programm sehen kann und ein
  // anderes nicht -- ohne dass eines von beiden etwas falsch macht.
  const struct {
    const char* label;
    const GUID& id;
  } kCategories[] = {
      {"CLSID_VideoInputDeviceCategory", CLSID_VideoInputDeviceCategory},
      {"KSCATEGORY_VIDEO", AM_KSCATEGORY_VIDEO},
      {"KSCATEGORY_CAPTURE", AM_KSCATEGORY_CAPTURE},
  };
  std::printf("\n================ Geräte je Kategorie ================\n");
  for (const auto& cat : kCategories) {
    const std::vector<VideoDeviceInfo> found = EnumerateDeviceCategory(cat.id);
    std::printf("\n%s -- %zu Gerät(e)\n", cat.label, found.size());
    for (const VideoDeviceInfo& d : found) std::printf("    %s\n", d.name.c_str());
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
        std::printf("        [%zu]%s %s  (Pin %d, Typ %ld)\n", k,
                    (int)k == probed.currentInput ? " <-" : "   ",
                    probed.crossbarInputs[k].name.c_str(), probed.crossbarInputs[k].pinIndex,
                    probed.crossbarInputs[k].physicalType);
      }
      if (probed.currentInput < 0) {
        std::printf("        (auf welchem die Karte steht, sagt sie nicht)\n");
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

// ---------------------------------------------------------------------------
// Finding a driver's private property set.
//
// A card whose inputs cannot be switched through IAMCrossbar can still have a
// selector -- it just lives in a property set the vendor never documented. KS
// has no call that lists the sets a filter answers to, but it has one that
// answers for a single set at a time and changes nothing: QuerySupported. The
// discriminator is the error. A set the filter does not know returns
// ERROR_SET_NOT_FOUND; a set it does know returns S_OK, or an error about the
// property rather than about the set.
//
// So the candidates come from the driver's own binaries. A property set GUID
// is a 16-byte constant in whatever module uses it, and a GUID is recognisable
// on sight: the version nibble is 1..5 and the top variant bits are 10. That
// leaves a lot of false positives in a few megabytes, which costs nothing --
// each one is a single fast ioctl, and only what answers gets printed.

struct GuidLess {
  bool operator()(const GUID& a, const GUID& b) const {
    return std::memcmp(&a, &b, sizeof(GUID)) < 0;
  }
};

std::string GuidText(const GUID& g) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

void CollectGuidsFromFile(const std::string& path, std::set<GUID, GuidLess>& out) {
  std::FILE* f = nullptr;
  if (::fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= (long)sizeof(GUID)) {
    std::fclose(f);
    return;
  }
  std::vector<unsigned char> blob((size_t)size);
  const size_t read = std::fread(blob.data(), 1, blob.size(), f);
  std::fclose(f);
  blob.resize(read);

  size_t before = out.size();
  for (size_t off = 0; off + sizeof(GUID) <= blob.size(); off += 4) {
    GUID g;
    std::memcpy(&g, blob.data() + off, sizeof(GUID));
    if (g.Data1 == 0 && g.Data2 == 0 && g.Data3 == 0) continue;
    const unsigned version = (g.Data3 >> 12) & 0xFu;
    if (version < 1 || version > 5) continue;
    if ((g.Data4[0] & 0xC0) != 0x80) continue;
    out.insert(g);
  }
  std::printf("  %-52s %6zu neue Kandidaten\n", path.c_str(), out.size() - before);
}

void CollectGuidsFromTree(const std::string& path, std::set<GUID, GuidLess>& out) {
  const DWORD attr = ::GetFileAttributesA(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    std::printf("  ! %s gibt es nicht\n", path.c_str());
    return;
  }
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    CollectGuidsFromFile(path, out);
    return;
  }
  WIN32_FIND_DATAA fd{};
  HANDLE h = ::FindFirstFileA((path + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::string name = fd.cFileName;
    if (name == "." || name == "..") continue;
    const std::string child = path + "\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      CollectGuidsFromTree(child, out);
      continue;
    }
    // Only the parts that talk to the hardware. The installer and the PDFs
    // would just add noise.
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) continue;
    std::string ext = name.substr(dot);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".ax" || ext == ".sys" || ext == ".dll") CollectGuidsFromFile(child, out);
  } while (::FindNextFileA(h, &fd));
  ::FindClose(h);
}

// How far up the property ids are walked. The vendor's numbering is sparse: the
// SA7160 answers for a block below 16 and then again at 201 and 255, with more
// than a hundred and eighty unanswered ids in between. A scan that stops at 64 --
// as this one first did -- finds the block, misses the two, and reports a card
// with a switchable input as having none.
constexpr DWORD kPropertyIdLimit = 1024;

void FindPrivatePropertySets(const std::string& path, const std::string& deviceMatch) {
  std::printf("================ Kandidaten aus den Treiberdateien ================\n");
  std::set<GUID, GuidLess> candidates;
  CollectGuidsFromTree(path, candidates);
  std::printf("  ---\n  %zu verschiedene GUID-Kandidaten\n\n", candidates.size());
  if (candidates.empty()) return;

  ComPtr<IBaseFilter> filter;
  std::string chosen;
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    if (d.name.find(deviceMatch) == std::string::npos) continue;
    filter = CreateFilterFromMoniker(d);
    if (filter) {
      chosen = d.name;
      break;
    }
  }
  if (!filter) {
    std::printf("Kein Videogerät gefunden, dessen Name \"%s\" enthält.\n", deviceMatch.c_str());
    return;
  }

  ComPtr<IKsPropertySet> ks;
  if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&ks)))) {
    std::printf("%s hat kein IKsPropertySet.\n", chosen.c_str());
    return;
  }

  std::printf("================ Was %s beantwortet ================\n", chosen.c_str());
  const HRESULT kNoSuchSet = HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  size_t hits = 0;
  for (const GUID& g : candidates) {
    DWORD support = 0;
    const HRESULT hr = ks->QuerySupported(g, 0, &support);
    if (hr == kNoSuchSet) continue;  // Das ist die überwältigende Mehrheit.
    ++hits;
    std::printf("\n%s  Property 0: 0x%08lX (support 0x%lX)\n", GuidText(g).c_str(),
                (unsigned long)hr, (unsigned long)support);
    // Der Rest des Sets. Ein Set mit vielen Properties ist eher ein echtes als
    // eine GUID, die zufällig durchrutscht.
    std::printf("   Properties:");
    for (DWORD id = 0; id < kPropertyIdLimit; ++id) {
      DWORD s = 0;
      if (SUCCEEDED(ks->QuerySupported(g, id, &s))) std::printf(" %lu(0x%lX)", id, (unsigned long)s);
    }
    std::printf("\n");
  }
  if (hits == 0) std::printf("  (keiner der Kandidaten wird beantwortet)\n");
  std::printf("\n");
}

struct PropValue {
  DWORD id = 0;
  DWORD support = 0;
  bool readable = false;
  std::vector<unsigned char> bytes;
};

// Every readable property of one set, with the size the driver reports. The
// size is unknown up front, so it is found by asking with a bigger buffer each
// time until one is accepted.
std::vector<PropValue> ReadWholeSet(IKsPropertySet* ks, const GUID& setId) {
  std::vector<PropValue> values;
  for (DWORD id = 0; id < kPropertyIdLimit; ++id) {
    DWORD support = 0;
    if (FAILED(ks->QuerySupported(setId, id, &support))) continue;
    PropValue v;
    v.id = id;
    v.support = support;
    if (support & KSPROPERTY_SUPPORT_GET) {
      unsigned char buf[512];
      for (size_t size : {(size_t)4, (size_t)8, (size_t)16, (size_t)32, (size_t)64, (size_t)128,
                          (size_t)256, sizeof(buf)}) {
        std::memset(buf, 0, sizeof(buf));
        DWORD returned = 0;
        if (SUCCEEDED(ks->Get(setId, id, nullptr, 0, buf, (ULONG)size, &returned))) {
          const size_t take = returned ? (size_t)returned : size;
          v.bytes.assign(buf, buf + (take < sizeof(buf) ? take : sizeof(buf)));
          v.readable = true;
          break;
        }
      }
    }
    values.push_back(std::move(v));
  }
  return values;
}

std::string BytesText(const std::vector<unsigned char>& b) {
  std::string out;
  char cell[8];
  for (size_t i = 0; i < b.size() && i < 32; ++i) {
    std::snprintf(cell, sizeof(cell), "%02X ", b[i]);
    out += cell;
  }
  if (b.size() >= 4) {
    DWORD d = 0;
    std::memcpy(&d, b.data(), sizeof(d));
    std::snprintf(cell, sizeof(cell), "%lu", (unsigned long)d);
    out += "(= ";
    out += cell;
    out += ")";
  }
  return out;
}

// Reads a set, hands the card's own dialog to the user, and reads it again.
// Which property carries the input selector is a guess until something moves
// it; the driver's own property page moves it without anyone having to know
// its shape, and the diff says which one it was. Nothing here writes.
void WatchPropertySet(const std::string& guidText, const std::string& deviceMatch) {
  GUID setId{};
  if (FAILED(::CLSIDFromString(ToWide(guidText).c_str(), &setId))) {
    std::printf("\"%s\" ist keine GUID.\n", guidText.c_str());
    return;
  }

  ComPtr<IBaseFilter> filter;
  std::string chosen;
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    if (d.name.find(deviceMatch) == std::string::npos) continue;
    filter = CreateFilterFromMoniker(d);
    if (filter) {
      chosen = d.name;
      break;
    }
  }
  ComPtr<IKsPropertySet> ks;
  if (!filter || FAILED(filter->QueryInterface(IID_PPV_ARGS(&ks)))) {
    std::printf("Kein Gerät mit \"%s\" und IKsPropertySet.\n", deviceMatch.c_str());
    return;
  }

  std::vector<GUID> pages;
  ComPtr<ISpecifyPropertyPages> spec;
  CAUUID ca = {};
  if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&spec))) || FAILED(spec->GetPages(&ca))) {
    std::printf("%s bietet keine Konfigurationsseiten an.\n", chosen.c_str());
    return;
  }
  for (ULONG i = 0; i < ca.cElems; ++i) pages.push_back(ca.pElems[i]);
  if (ca.pElems) ::CoTaskMemFree(ca.pElems);

  const std::vector<PropValue> before = ReadWholeSet(ks.Get(), setId);
  std::printf("================ %s an %s ================\n", guidText.c_str(), chosen.c_str());
  std::printf("Vorher gelesen: %zu Properties.\n\n", before.size());
  std::printf("Der Kartendialog geht jetzt auf. Stell den Eingang um und mach ihn zu.\n");
  std::fflush(stdout);

  IUnknown* object = filter.Get();
  std::thread worker([&]() {
    ::OleInitialize(nullptr);
    ::OleCreatePropertyFrame(nullptr, 0, 0, L"Karte", 1, &object, (ULONG)pages.size(), pages.data(),
                             0, 0, nullptr);
    ::OleUninitialize();
  });
  worker.join();

  const std::vector<PropValue> after = ReadWholeSet(ks.Get(), setId);
  std::printf("\n================ Was sich geändert hat ================\n");
  size_t moved = 0;
  for (const PropValue& a : after) {
    for (const PropValue& b : before) {
      if (b.id != a.id) continue;
      if (b.bytes == a.bytes) break;
      ++moved;
      std::printf("\n[%2lu] %s\n     vorher : %s\n     nachher: %s\n", (unsigned long)a.id,
                  (a.support & KSPROPERTY_SUPPORT_SET) ? "lesen schreiben" : "nur lesen",
                  BytesText(b.bytes).c_str(), BytesText(a.bytes).c_str());
      break;
    }
  }
  if (moved == 0) std::printf("  (nichts)\n");
  std::printf("\n");
}

// The only mode in this file that writes. Read-only was the right default while
// the shape of a private property was a guess. On the SA7160 it is no longer
// one: property 201 of the vendor set reads back exactly the value the card's
// own dialog just wrote, the driver keeps that value under the name
// AnalogCrossbarVideoInputProperty, and AmaRecTV sets the same id on the same
// set. What a read cannot answer is whether the driver accepts the write from
// anyone other than its own property page, and CapView cannot offer an input
// list until it does. Reads the property, writes, reads it back.
void SetOneProperty(const std::string& guidText, DWORD id, DWORD value,
                    const std::string& deviceMatch) {
  GUID setId{};
  if (FAILED(::CLSIDFromString(ToWide(guidText).c_str(), &setId))) {
    std::printf("\"%s\" ist keine GUID.\n", guidText.c_str());
    return;
  }

  ComPtr<IBaseFilter> filter;
  std::string chosen;
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    if (d.name.find(deviceMatch) == std::string::npos) continue;
    filter = CreateFilterFromMoniker(d);
    if (filter) {
      chosen = d.name;
      break;
    }
  }
  ComPtr<IKsPropertySet> ks;
  if (!filter || FAILED(filter->QueryInterface(IID_PPV_ARGS(&ks)))) {
    std::printf("Kein Gerät mit \"%s\" und IKsPropertySet.\n", deviceMatch.c_str());
    return;
  }

  std::printf("================ %s [%lu] an %s ================\n", guidText.c_str(),
              (unsigned long)id, chosen.c_str());
  DWORD support = 0;
  const HRESULT hrSupport = ks->QuerySupported(setId, id, &support);
  if (FAILED(hrSupport)) {
    std::printf("QuerySupported: 0x%08lX -- die Karte kennt diese Property nicht.\n",
                (unsigned long)hrSupport);
    return;
  }
  if (!(support & KSPROPERTY_SUPPORT_SET)) {
    std::printf("Die Property ist nicht schreibbar (support 0x%lX).\n", (unsigned long)support);
    return;
  }

  DWORD before = 0;
  DWORD returned = 0;
  const bool readable = (support & KSPROPERTY_SUPPORT_GET) &&
                        SUCCEEDED(ks->Get(setId, id, nullptr, 0, &before, sizeof(before), &returned));
  if (readable) std::printf("vorher : %lu\n", (unsigned long)before);

  DWORD payload = value;
  const HRESULT hr = ks->Set(setId, id, nullptr, 0, &payload, sizeof(payload));
  std::printf("Set(%lu): 0x%08lX%s\n", (unsigned long)value, (unsigned long)hr,
              SUCCEEDED(hr) ? " (S_OK)" : "");

  if (readable) {
    DWORD after = 0;
    returned = 0;
    if (SUCCEEDED(ks->Get(setId, id, nullptr, 0, &after, sizeof(after), &returned)))
      std::printf("nachher: %lu\n", (unsigned long)after);
  }
  std::printf("\n");
}

// Reads every property of one set and prints the bytes. Read-only on purpose:
// the shape of a private property is a guess until something confirms it, and
// the safe way to confirm it is to read the same property twice with the card
// on two different inputs and see which byte moved.
void DumpPropertySet(const std::string& guidText, const std::string& deviceMatch) {
  GUID setId{};
  const std::wstring wide = ToWide(guidText);
  if (FAILED(::CLSIDFromString(wide.c_str(), &setId))) {
    std::printf("\"%s\" ist keine GUID. Erwartet wird {........-....-....-....-............}\n",
                guidText.c_str());
    return;
  }

  ComPtr<IBaseFilter> filter;
  std::string chosen;
  for (const VideoDeviceInfo& d : EnumerateVideoDevices()) {
    if (d.name.find(deviceMatch) == std::string::npos) continue;
    filter = CreateFilterFromMoniker(d);
    if (filter) {
      chosen = d.name;
      break;
    }
  }
  ComPtr<IKsPropertySet> ks;
  if (!filter || FAILED(filter->QueryInterface(IID_PPV_ARGS(&ks)))) {
    std::printf("Kein Gerät mit \"%s\" und IKsPropertySet.\n", deviceMatch.c_str());
    return;
  }

  std::printf("================ %s an %s ================\n", guidText.c_str(), chosen.c_str());
  for (DWORD id = 0; id < kPropertyIdLimit; ++id) {
    DWORD support = 0;
    if (FAILED(ks->QuerySupported(setId, id, &support))) continue;

    std::printf("\n[%2lu] %s%s", (unsigned long)id, (support & KSPROPERTY_SUPPORT_GET) ? "lesen " : "",
                (support & KSPROPERTY_SUPPORT_SET) ? "schreiben" : "");
    if (!(support & KSPROPERTY_SUPPORT_GET)) {
      std::printf("\n     (nicht lesbar)\n");
      continue;
    }

    // Die Groesse ist unbekannt, also wird sie ausprobiert. Der erste Aufruf,
    // der nicht ueber zu wenig Platz klagt, hat sie.
    unsigned char buf[512];
    DWORD returned = 0;
    HRESULT hr = E_FAIL;
    size_t used = 0;
    for (size_t size : {(size_t)4, (size_t)8, (size_t)16, (size_t)32, (size_t)64, (size_t)128,
                        (size_t)256, sizeof(buf)}) {
      std::memset(buf, 0, sizeof(buf));
      returned = 0;
      hr = ks->Get(setId, id, nullptr, 0, buf, (ULONG)size, &returned);
      if (SUCCEEDED(hr)) {
        used = size;
        break;
      }
    }
    if (FAILED(hr)) {
      std::printf("\n     Get: 0x%08lX\n", (unsigned long)hr);
      continue;
    }

    const DWORD show = returned ? returned : (DWORD)used;
    std::printf(", %lu Byte\n     ", (unsigned long)show);
    for (DWORD i = 0; i < show && i < 64; ++i) std::printf("%02X ", buf[i]);
    if (show >= 4) {
      DWORD asDword = 0;
      std::memcpy(&asDword, buf, sizeof(asDword));
      std::printf("\n     als DWORD: %lu", (unsigned long)asDword);
    }
    std::printf("\n");
  }
  std::printf("\n");
}

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

  if (argc > 1 && std::string(argv[1]) == "propsets") {
    if (argc < 3) {
      std::printf("Aufruf: capview_probe propsets <Treiberdatei oder -ordner> [Gerätename]\n");
      return 1;
    }
    FindPrivatePropertySets(argv[2], argc > 3 ? argv[3] : "SA7160");
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "propget") {
    if (argc < 3) {
      std::printf("Aufruf: capview_probe propget {GUID} [Gerätename]\n");
      return 1;
    }
    DumpPropertySet(argv[2], argc > 3 ? argv[3] : "SA7160");
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "propwatch") {
    if (argc < 3) {
      std::printf("Aufruf: capview_probe propwatch {GUID} [Gerätename]\n");
      return 1;
    }
    WatchPropertySet(argv[2], argc > 3 ? argv[3] : "SA7160");
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "propset") {
    if (argc < 5) {
      std::printf("Aufruf: capview_probe propset {GUID} <id> <wert> [Gerätename]\n");
      return 1;
    }
    SetOneProperty(argv[2], (DWORD)std::strtoul(argv[3], nullptr, 0),
                   (DWORD)std::strtoul(argv[4], nullptr, 0), argc > 5 ? argv[5] : "SA7160");
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
