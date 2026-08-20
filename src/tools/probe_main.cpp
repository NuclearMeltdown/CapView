// capview_probe: prints what CapView sees. Useful when a card behaves oddly --
// it lists every video device, the formats and inputs it reports, every audio
// endpoint, and which audio endpoint CapView would pair with which card.

#include <mmdeviceapi.h>
// Must come after mmdeviceapi.h: it needs the PROPERTYKEY macros pulled in there.
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>

#include "audio/audio_devices.h"
#include "capture/dshow_util.h"
#include "capture/video_capture.h"
#include "common.h"
#include "record/ffmpeg_locator.h"
#include "tools/mf_probe.h"

using namespace cap;

namespace {

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
          std::printf(" %.2f%s", f.fps, f.forced ? "*" : "");
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

int main() {
  ::SetConsoleOutputCP(CP_UTF8);
  ComScope com(COINIT_MULTITHREADED);
  if (!com.ok()) {
    std::printf("COM konnte nicht initialisiert werden.\n");
    return 1;
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
