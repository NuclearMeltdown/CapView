#include "capture/video_capture.h"

#include <algorithm>
#include <vector>

#include "i18n.h"

namespace cap {
namespace {

// Creates a filter graph plus capture graph builder, wired together.
bool CreateGraph(ComPtr<IGraphBuilder>* graph, ComPtr<ICaptureGraphBuilder2>* builder,
                 std::string* error) {
  HRESULT hr = ::CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(graph->GetAddressOf()));
  if (FAILED(hr)) {
    if (error) *error = T("Filtergraph konnte nicht erstellt werden: ",
                             "The filter graph could not be created: ") + HrToString(hr);
    return false;
  }
  hr = ::CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(builder->GetAddressOf()));
  if (FAILED(hr)) {
    if (error) *error = T("Capture Graph Builder konnte nicht erstellt werden: ",
                             "The capture graph builder could not be created: ") + HrToString(hr);
    return false;
  }
  hr = (*builder)->SetFiltergraph(graph->Get());
  if (FAILED(hr)) {
    if (error) *error = T("SetFiltergraph fehlgeschlagen: ", "SetFiltergraph failed: ") + HrToString(hr);
    return false;
  }
  return true;
}

// True for errors that mean "somebody else has this device", as opposed to
// "this device does not like that format". KS drivers report a pin whose single
// instance is already taken as ERROR_NO_SYSTEM_RESOURCES, which reads like a
// memory problem but is not one -- it is the usual answer when a second program
// (another CapView window, OBS, the vendor tool) is holding the card.
bool IsDeviceBusyError(HRESULT hr) {
  return hr == HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES) ||
         hr == HRESULT_FROM_WIN32(ERROR_BUSY) ||
         hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED ||
         hr == VFW_E_NO_ALLOCATOR;
}

std::string BusyMessage(const DeviceRef& device) {
  const std::string name = device.name.empty() ? device.id : device.name;
  return T("Das Gerät '", "Device '") + name +
         T("' wird bereits von einem anderen Programm benutzt. Die meisten Karten geben "
           "ihren Videopin nur einmal her — schließe die andere CapView-Instanz, OBS oder "
           "das Hersteller-Tool. CapView versucht es von allein weiter.",
           "' is already in use by another program. Most cards hand out their video pin "
           "only once, so close the other CapView window, OBS or the vendor tool. CapView "
           "keeps retrying on its own.");
}

// Cost of one frame in bytes, used to order fallbacks from cheapest upwards.
size_t FrameBytes(const std::string& subtypeLabel, int width, int height) {
  GUID subtype = GUID_NULL;
  if (!SubtypeFromLabel(subtypeLabel, &subtype)) return SIZE_MAX;
  const size_t size = ImageSizeForSubtype(subtype, width, height, nullptr);
  return size ? size : SIZE_MAX;  // compressed formats sort last
}

// The requested format first, then the same resolution in every other format
// the card offers, cheapest first, and finally the driver's own default.
std::vector<FormatSel> BuildFormatCandidates(const FormatSel& wanted, const CapsModel& caps) {
  std::vector<FormatSel> out;
  if (wanted.valid()) out.push_back(wanted);

  std::vector<FormatSel> alternatives;
  for (const std::string& subtype : caps.Subtypes()) {
    if (wanted.valid() && subtype == wanted.subtype) continue;
    FormatSel alt = wanted;
    alt.subtype = subtype;
    alt.forced = false;
    if (!alt.valid()) continue;
    alternatives.push_back(alt);
  }
  std::sort(alternatives.begin(), alternatives.end(),
            [](const FormatSel& a, const FormatSel& b) {
              return FrameBytes(a.subtype, a.width, a.height) <
                     FrameBytes(b.subtype, b.width, b.height);
            });
  out.insert(out.end(), alternatives.begin(), alternatives.end());

  const FormatSel fallback = caps.PickDefault();
  if (fallback.valid()) {
    const bool known = std::any_of(out.begin(), out.end(), [&](const FormatSel& f) {
      return f.SameFormat(fallback);
    });
    if (!known) out.push_back(fallback);
  }
  return out;
}

}  // namespace

VideoCapture::~VideoCapture() {
  Stop();
}

// ------------------------------------------------------------------- probing

DeviceProbeResult VideoCapture::Probe(const DeviceRef& device) {
  DeviceProbeResult out;
  if (device.empty()) {
    out.error = T("Kein Videogerät ausgewählt", "No video device selected");
    return out;
  }

  ComPtr<IGraphBuilder> graph;
  ComPtr<ICaptureGraphBuilder2> builder;
  if (!CreateGraph(&graph, &builder, &out.error)) return out;

  ComPtr<IBaseFilter> filter = CreateVideoFilter(device, &out.device);
  if (!filter) {
    out.error = T("Gerät '", "Device '") + (device.name.empty() ? device.id : device.name) +
                T("' wurde nicht gefunden. Ist die Karte angeschlossen?",
                  "' was not found. Is the card plugged in?");
    return out;
  }

  HRESULT hr = graph->AddFilter(filter.Get(), L"Capture");
  if (FAILED(hr)) {
    out.error = T("Capture-Filter konnte nicht in den Graph eingefügt werden: ",
                  "The capture filter could not be added to the graph: ") + HrToString(hr);
    return out;
  }

  ComPtr<IPin> pin = FindCapturePin(builder.Get(), filter.Get());
  if (!pin) {
    out.error = T("Das Gerät hat keinen brauchbaren Capture-Pin",
                  "The device offers no usable capture pin");
    return out;
  }

  out.availableStandards = AvailableVideoStandards(filter.Get());
  out.currentStandard = CurrentVideoStandard(filter.Get());

  out.caps.Build(EnumerateCaps(pin.Get()));

  // The crossbar is a separate upstream filter that only joins the graph once
  // the capture pin is connected, so build the full chain before asking for it.
  ComPtr<FrameSink> sink = FrameSink::Create();
  if (sink && SUCCEEDED(graph->AddFilter(sink.Get(), L"CapView Probe Sink"))) {
    // Direct connection only. Falling back to intelligent connect here would
    // make the graph builder try every registered filter against every media
    // type the card offers, which takes seconds and is far more than is needed
    // to make the crossbar show up.
    graph->ConnectDirect(pin.Get(), static_cast<IPin*>(sink->pin()), nullptr);
  }
  out.crossbarInputs = EnumerateCrossbarInputs(builder.Get(), filter.Get());

  ComPtr<IAMStreamConfig> config;
  if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&config)))) {
    AM_MEDIA_TYPE* current = nullptr;
    if (SUCCEEDED(config->GetFormat(&current)) && current) {
      ParseVideoMediaType(current, &out.colorInfo);
      DeleteMediaType(current);
    }
  }
  out.ok = true;

  // Tear down explicitly and in order so the device is released before we
  // return -- the caller may want to open it for real right afterwards.
  if (sink) {
    sink->pin()->Disconnect();
    graph->RemoveFilter(sink.Get());
  }
  graph->RemoveFilter(filter.Get());
  pin.Reset();
  sink.Reset();
  filter.Reset();
  builder.Reset();
  graph.Reset();
  return out;
}

// -------------------------------------------------------------------- start

bool VideoCapture::Start(const CaptureSettings& settings, std::string* error) {
  Stop();

  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    CAP_ERR("Start fehlgeschlagen: %s", msg.c_str());
    Teardown();
    return false;
  };

  if (settings.video.empty())
    return fail(T("Kein Videogerät ausgewählt", "No video device selected"));

  std::string err;
  if (!CreateGraph(&graph_, &builder_, &err)) return fail(err);

  captureFilter_ = CreateVideoFilter(settings.video, &capabilities_.device);
  if (!captureFilter_) {
    return fail(T("Videogerät '", "Video device '") +
                (settings.video.name.empty() ? settings.video.id : settings.video.name) +
                T("' nicht gefunden. Ist die Capture-Karte angeschlossen?",
                  "' not found. Is the capture card plugged in?"));
  }

  HRESULT hr = graph_->AddFilter(captureFilter_.Get(), L"Capture");
  if (FAILED(hr))
    return fail(T("Capture-Filter konnte nicht eingefügt werden: ",
                  "The capture filter could not be added: ") +
                HrToString(hr));

  capturePin_ = FindCapturePin(builder_.Get(), captureFilter_.Get());
  if (!capturePin_)
    return fail(T("Das Gerät hat keinen brauchbaren Capture-Pin",
                  "The device has no usable capture pin"));

  // Before the formats are read, not after: the standard decides how many lines
  // the card will produce, and therefore which formats it advertises at all.
  capabilities_.availableStandards = AvailableVideoStandards(captureFilter_.Get());
  // Eine Videonorm gehoert dem Analogdekoder. Eine Karte, die beides kann, hat
  // ihn trotzdem und meldet die Normen weiterhin -- auf einem HDMI-Eingang sagt
  // das aber nichts ueber das Signal, und PAL dort zu setzen legt die Karte auf
  // 720x576 bei 50 Hz fest, obwohl an der Buchse etwas ganz anderes anliegt.
  //
  // Wer die Quelle ausdruecklich als digital angegeben hat, meint genau das.
  const bool digital = settings.signalKind == SignalKind::Digital;
  if (digital && settings.videoStandard > 0) {
    CAP_LOG("Quelle ist als digital angegeben, Videonorm wird nicht gesetzt");
  }
  if (!digital && settings.videoStandard > 0 &&
      (capabilities_.availableStandards & settings.videoStandard) != 0) {
    SetVideoStandard(captureFilter_.Get(), settings.videoStandard);
  }
  capabilities_.currentStandard = CurrentVideoStandard(captureFilter_.Get());

  capabilities_.caps.Build(EnumerateCaps(capturePin_.Get()));
  capabilities_.ok = true;

  FormatSel wanted = settings.format;
  if (!wanted.valid()) {
    // A subtype without a size is what re-reading the card leaves behind: the
    // resolution is to be found again, the pixel format is not up for grabs.
    const std::string wish = wanted.subtype;
    wanted = capabilities_.caps.PickDefault(wish);
    if (wish.empty()) {
      CAP_LOG("Kein Format konfiguriert, verwende Standard: %s", wanted.Label().c_str());
    } else if (wanted.subtype == wish) {
      CAP_LOG("Auflösung neu gesucht, Pixelformat %s beibehalten: %s", wish.c_str(),
              wanted.Label().c_str());
    } else {
      CAP_WARN("Pixelformat %s bietet die Karte nicht mehr an, stattdessen: %s", wish.c_str(),
               wanted.Label().c_str());
    }
  }

  // "Highest available" is stored as no rate at all, and this is where it turns
  // into one. Late on purpose: the number then comes from the card that is
  // actually in front of us, so the setting survives a console switching from
  // 576i50 to 480p60 without anyone editing it. If the card names no rate the
  // zero simply stays, and the driver's own default interval is left in place.
  if (wanted.fps <= 0.0) {
    const double top = capabilities_.caps.HighestFps(wanted.subtype, wanted.width, wanted.height);
    if (top > 0.0) {
      wanted.fps = top;
      CAP_LOG("Höchste verfügbare Bildrate für %dx%d %s: %.3f fps", wanted.width, wanted.height,
              wanted.subtype.c_str(), top);
    } else {
      CAP_LOG("Höchste verfügbare Bildrate: Karte meldet keine, Treiberstandard bleibt stehen");
    }
  }

  sink_ = FrameSink::Create();
  if (!sink_)
    return fail(T("Sink-Filter konnte nicht erstellt werden",
                  "The sink filter could not be created"));

  hr = graph_->AddFilter(sink_.Get(), L"CapView Sink");
  if (FAILED(hr))
    return fail(T("Sink-Filter konnte nicht eingefügt werden: ",
                  "The sink filter could not be added: ") +
                HrToString(hr));

  IPin* sinkPin = static_cast<IPin*>(sink_->pin());

  // A card can say no when the format is set and again when the pins connect.
  // Only a format-shaped refusal is worth retrying with a different format; if
  // the device is simply taken, trying eleven more formats just makes the user
  // wait for the same answer eleven more times.
  std::string lastError;
  bool connected = false;
  for (const FormatSel& candidate : BuildFormatCandidates(wanted, capabilities_.caps)) {
    VideoFormatInfo appliedFormat;
    hr = ApplyFormat(capturePin_.Get(), candidate, &appliedFormat);
    if (FAILED(hr)) {
      if (IsDeviceBusyError(hr)) return fail(BusyMessage(settings.video));
      lastError = "SetFormat " + candidate.Label() + ": " + HrToString(hr);
      continue;
    }

    // Direct connection keeps the graph at two filters. Only fall back to
    // intelligent connect when the capture format needs a decoder (MJPG).
    hr = graph_->ConnectDirect(capturePin_.Get(), sinkPin, nullptr);
    if (FAILED(hr) && !IsDeviceBusyError(hr)) hr = graph_->Connect(capturePin_.Get(), sinkPin);

    if (SUCCEEDED(hr)) {
      if (!candidate.SameFormat(wanted)) {
        CAP_WARN("Format %s ging nicht, benutze stattdessen %s", wanted.Label().c_str(),
                 candidate.Label().c_str());
      }
      connected = true;
      break;
    }

    if (IsDeviceBusyError(hr)) return fail(BusyMessage(settings.video));

    lastError = "Connect " + candidate.Label() + ": " + HrToString(hr);
    CAP_WARN("%s", lastError.c_str());
    // Leave no half-connected pins behind before the next attempt.
    capturePin_->Disconnect();
    sinkPin->Disconnect();
  }

  if (!connected) {
    return fail(T("Die Karte hat kein einziges Format akzeptiert. Letzter Fehler: ",
                  "The card accepted none of the formats. Last error: ") +
                lastError);
  }

  // Only now that the capture pin is connected is the crossbar filter part of
  // the graph, so this is the earliest point at which the input can be routed.
  capabilities_.crossbarInputs = EnumerateCrossbarInputs(builder_.Get(), captureFilter_.Get());
  if (settings.crossbarInput >= 0 && !capabilities_.crossbarInputs.empty()) {
    RouteCrossbarInput(builder_.Get(), captureFilter_.Get(), settings.crossbarInput);
  }

  // No reference clock: with a clock the graph would hold each sample until its
  // presentation time, which is pure added latency for a live preview.
  ComPtr<IMediaFilter> mediaFilter;
  if (SUCCEEDED(graph_.As(&mediaFilter))) {
    mediaFilter->SetSyncSource(nullptr);
  }

  if (FAILED(hr = graph_.As(&control_))) {
    return fail(T("IMediaControl nicht verfügbar: ", "IMediaControl not available: ") +
                HrToString(hr));
  }
  graph_.As(&events_);

  hr = control_->Run();
  if (FAILED(hr)) {
    return fail(T("Der Graph konnte nicht gestartet werden (",
                  "The graph could not be started (") +
                HrToString(hr) + T("). Benutzt ein anderes Programm die Karte gerade?",
                                   "). Is another program using the card right now?"));
  }

  VideoFormatInfo info = sink_->format();
  CAP_LOG("Capture läuft: %s %dx%d @ %.3f fps%s", info.subtypeLabel.c_str(), info.width,
          info.height, info.fps, info.interlaced ? " (interlaced)" : "");
  return true;
}

void VideoCapture::Stop() {
  if (control_) {
    control_->Stop();
  }
  Teardown();
}

void VideoCapture::Teardown() {
  if (graph_) {
    if (captureFilter_) graph_->RemoveFilter(captureFilter_.Get());
    if (sink_) graph_->RemoveFilter(sink_.Get());
  }
  events_.Reset();
  control_.Reset();
  capturePin_.Reset();
  captureFilter_.Reset();
  sink_.Reset();
  builder_.Reset();
  graph_.Reset();
}

VideoFormatInfo VideoCapture::format() const {
  if (!sink_) return VideoFormatInfo{};
  return sink_->format();
}

bool VideoCapture::PumpEvents(std::string* message) {
  if (!events_) return false;
  bool fatal = false;
  long code = 0;
  LONG_PTR p1 = 0, p2 = 0;
  while (SUCCEEDED(events_->GetEvent(&code, &p1, &p2, 0))) {
    switch (code) {
      case EC_DEVICE_LOST:
        // p2 == 0 means the device went away; == 1 means it came back.
        if (p2 == 0) {
          fatal = true;
          if (message)
            *message = T("Das Aufnahmegerät wurde entfernt.", "The capture device was removed.");
        }
        break;
      case EC_ERRORABORT:
      case EC_ERRORABORTEX:
        fatal = true;
        if (message) {
          *message = T("Der Capture-Graph wurde mit einem Fehler abgebrochen (",
                       "The capture graph was aborted with an error (") +
                     HrToString((HRESULT)p1) + ").";
        }
        break;
      case EC_COMPLETE:
      case EC_USERABORT:
        fatal = true;
        if (message) *message = T("Der Stream wurde beendet.", "The stream ended.");
        break;
      default: break;
    }
    events_->FreeEventParams(code, p1, p2);
  }
  if (!fatal && sink_ && sink_->ended()) {
    fatal = true;
    if (message) *message = T("Die Karte liefert keine Daten mehr.", "The card stopped sending data.");
  }
  return fatal;
}

bool VideoCapture::SetCrossbarInput(int index) {
  if (!builder_ || !captureFilter_) return false;
  return RouteCrossbarInput(builder_.Get(), captureFilter_.Get(), index);
}

}  // namespace cap
