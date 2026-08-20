#include "ui/settings_window.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "i18n.h"
#include "record/screenshot.h"
#include "imgui.h"

namespace cap {
namespace {

using NameFn = const char* (*)(int);

// A small "?" that shows an explanation on hover.
void HelpMarker(const char* text) {
  if (!text || !*text) return;
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Combo over an enum whose labels come from a lookup function, so the list
// follows the selected language without any table to keep in sync.
bool ComboEnum(const char* label, int* value, int count, NameFn name, NameFn help = nullptr) {
  if (*value < 0 || *value >= count) *value = 0;
  bool changed = false;
  if (ImGui::BeginCombo(label, name(*value))) {
    for (int i = 0; i < count; ++i) {
      const bool selected = (*value == i);
      if (ImGui::Selectable(name(i), selected)) {
        *value = i;
        changed = true;
      }
      if (help && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(help(i));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

std::string FpsLabel(double fps) {
  std::string s = Format("%.2f", fps);
  if (CurrentLanguage() == Language::German) {
    for (char& c : s) {
      if (c == '.') c = ',';
    }
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ",00") == 0) s.resize(s.size() - 3);
  } else if (s.size() > 3 && s.compare(s.size() - 3, 3, ".00") == 0) {
    s.resize(s.size() - 3);
  }
  return s + " Hz";
}

BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* list = (std::vector<MonitorInfoEntry>*)data;
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!::GetMonitorInfoW(monitor, &info)) return TRUE;

  MonitorInfoEntry entry;
  entry.index = (int)list->size();
  entry.rect = info.rcMonitor;
  entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  entry.name = Format(T("Monitor %d  (%ldx%ld)%s", "Monitor %d  (%ldx%ld)%s"), entry.index + 1,
                      info.rcMonitor.right - info.rcMonitor.left,
                      info.rcMonitor.bottom - info.rcMonitor.top,
                      entry.primary ? T("  [Hauptmonitor]", "  [primary]") : "");
  list->push_back(std::move(entry));
  return TRUE;
}

// The Explorer folder picker. IFileOpenDialog with FOS_PICKFOLDERS is the
// modern one -- SHBrowseForFolder still works but looks a decade old and has no
// address bar to paste a path into.
std::wstring PickFolder(HWND owner, const std::wstring& start) {
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return {};
  }

  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

  if (!start.empty()) {
    ComPtr<IShellItem> item;
    if (SUCCEEDED(::SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
      dialog->SetFolder(item.Get());
    }
  }

  if (FAILED(dialog->Show(owner))) return {};  // also the cancel path

  ComPtr<IShellItem> result;
  if (FAILED(dialog->GetResult(&result))) return {};
  PWSTR path = nullptr;
  if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
  std::wstring folder = path;
  ::CoTaskMemFree(path);
  return folder;
}

}  // namespace

std::vector<MonitorInfoEntry> EnumerateMonitors() {
  std::vector<MonitorInfoEntry> list;
  ::EnumDisplayMonitors(nullptr, nullptr, MonitorProc, (LPARAM)&list);
  return list;
}

// ------------------------------------------------------------------ lifecycle

void SettingsWindow::Open(Config* live, const std::string& reason) {
  if (!live) return;
  live_ = live;
  snapshot_ = *live;
  reason_ = reason;
  open_ = true;
  centerNext_ = true;
  listsValid_ = false;
  renameTarget_ = -1;
  recordBuffersLoaded_ = false;
}

bool SettingsWindow::takeProbeRequest() {
  const bool requested = probeRequested_;
  probeRequested_ = false;
  return requested;
}

void SettingsWindow::Close() {
  open_ = false;
  captureAction_ = -1;
}

void SettingsWindow::InvalidateDeviceLists() {
  listsValid_ = false;
  probedId_.clear();
  probed_ = DeviceProbeResult{};
  embeddedAudioForDevice_.clear();
}

void SettingsWindow::RefreshDeviceLists() {
  videoDevices_ = EnumerateVideoDevices();
  audioInputs_ = EnumerateAudioDevices(true);
  audioOutputs_ = EnumerateAudioDevices(false);
  monitors_ = EnumerateMonitors();
  listsValid_ = true;
}

const DeviceProbeResult& SettingsWindow::CapsFor(const DeviceRef& device,
                                                 const DeviceProbeResult* liveCaps) {
  static const DeviceProbeResult kEmpty;
  if (device.empty()) return kEmpty;

  // The running device is already open; probing it again would fail.
  if (liveCaps && liveCaps->ok && !liveCaps->device.id.empty() &&
      (liveCaps->device.id == device.id || liveCaps->device.name == device.name)) {
    return *liveCaps;
  }

  const std::string key = device.id.empty() ? device.name : device.id;
  if (probedId_ != key) {
    CAP_LOG("Frage Fähigkeiten von '%s' ab", device.name.c_str());
    probed_ = VideoCapture::Probe(device);
    probedId_ = key;
  }
  return probed_;
}

void SettingsWindow::EnsureValidFormat(const DeviceProbeResult& caps) {
  Profile& p = cfg().active();
  if (!caps.ok || caps.caps.empty()) return;

  const std::vector<std::string> subtypes = caps.caps.Subtypes();
  if (subtypes.empty()) return;

  // Format from another card, or nothing set yet: pick something sensible.
  if (!p.capture.format.valid() ||
      std::find(subtypes.begin(), subtypes.end(), p.capture.format.subtype) == subtypes.end()) {
    p.capture.format = caps.caps.PickDefault();
  }
  p.capture.format.forced =
      !caps.caps.IsAdvertised(p.capture.format.subtype, p.capture.format.width,
                              p.capture.format.height, p.capture.format.fps);
}

// ----------------------------------------------------------------------- draw

SettingsWindow::Result SettingsWindow::Draw(const DeviceProbeResult* liveCaps,
                                            FfmpegInfo* ffmpeg) {
  if (!open_ || !live_) return Result::None;
  if (!listsValid_) RefreshDeviceLists();

  Result result = Result::None;

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (centerNext_) {
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    centerNext_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(780.0f, 660.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 460.0f), ImVec2(FLT_MAX, FLT_MAX));

  bool stayOpen = true;
  if (!ImGui::Begin(T("Einstellungen", "Settings"), &stayOpen, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return Result::None;
  }
  if (!stayOpen) {
    ImGui::End();
    return Result::Close;
  }

  if (!reason_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.85f, 0.55f, 0.15f, 0.18f));
    ImGui::BeginChild("banner", ImVec2(0, ImGui::GetFrameHeight() * 1.6f), ImGuiChildFlags_Borders);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", reason_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  const DeviceProbeResult& caps = CapsFor(cfg().active().capture.video, liveCaps);
  EnsureValidFormat(caps);

  // Height to leave free for the button row. Each tab scrolls inside its own
  // child, which is what keeps the tab strip pinned to the top.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f;

  if (ImGui::BeginTabBar("settings_tabs", ImGuiTabBarFlags_None)) {
    if (ImGui::BeginTabItem(T("Quelle", "Source"))) {
      ImGui::BeginChild("scroll_source", ImVec2(0, -footer));
      DrawSourceTab(caps);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Bild", "Picture"))) {
      ImGui::BeginChild("scroll_image", ImVec2(0, -footer));
      DrawImageTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Ton", "Audio"))) {
      ImGui::BeginChild("scroll_audio", ImVec2(0, -footer));
      DrawAudioTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Anzeige", "Display"))) {
      ImGui::BeginChild("scroll_display", ImVec2(0, -footer));
      DrawDisplayTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Aufnahme", "Recording"))) {
      ImGui::BeginChild("scroll_record", ImVec2(0, -footer));
      DrawRecordTab(ffmpeg);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Werkzeuge", "Tools"))) {
      ImGui::BeginChild("scroll_tools", ImVec2(0, -footer));
      DrawToolsTab(ffmpeg);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Tasten", "Keys"))) {
      ImGui::BeginChild("scroll_keys", ImVec2(0, -footer));
      DrawHotkeysTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Profile", "Profiles"))) {
      ImGui::BeginChild("scroll_profiles", ImVec2(0, -footer));
      DrawProfilesTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::TextDisabled(T("Änderungen wirken sofort.", "Changes take effect immediately."));
  ImGui::SameLine();

  const float buttonWidth = 120.0f;
  const float spacing = style.ItemSpacing.x;
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - style.WindowPadding.x - buttonWidth * 2.0f -
                       spacing);
  if (ImGui::Button(T("Verwerfen", "Discard"), ImVec2(buttonWidth, 0))) {
    *live_ = snapshot_;
    result = Result::Close;
  }
  ImGui::SetItemTooltip(T("Alles auf den Stand beim Öffnen zurücksetzen und schließen.",
                          "Restore everything to how it was when this opened, and close."));
  ImGui::SameLine();
  if (ImGui::Button(T("Schließen", "Close"), ImVec2(buttonWidth, 0))) result = Result::Close;

  ImGui::End();
  return result;
}

// ---------------------------------------------------------------- source tab

void SettingsWindow::DrawSourceTab(const DeviceProbeResult& caps) {
  Profile& p = cfg().active();
  ImGui::Spacing();

  ImGui::SeparatorText(T("Videogerät", "Video device"));

  const char* preview = p.capture.video.name.empty()
                            ? T("— nichts ausgewählt —", "— nothing selected —")
                            : p.capture.video.name.c_str();
  ImGui::SetNextItemWidth(-150.0f);
  if (ImGui::BeginCombo("##videodev", preview)) {
    if (videoDevices_.empty()) {
      ImGui::TextDisabled(T("Keine Videogeräte gefunden", "No video devices found"));
    }
    for (const VideoDeviceInfo& d : videoDevices_) {
      const bool selected = (d.id == p.capture.video.id);
      if (ImGui::Selectable(d.name.c_str(), selected) && !selected) {
        p.capture.video = DeviceRef{d.name, d.id, ""};
        p.capture.format = FormatSel{};  // format belongs to the old card
        p.capture.crossbarInput = -1;
        embeddedAudioForDevice_.clear();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Aktualisieren", "Refresh"), ImVec2(-1, 0))) InvalidateDeviceLists();

  if (!caps.error.empty()) {
    ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", caps.error.c_str());
  }

  // ---- crossbar ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Eingang der Karte", "Card input"));
  if (cfg().active().capture.video.empty()) {
    // Nothing has been probed yet, so there is nothing true to say about inputs.
    ImGui::TextDisabled(T("Zuerst ein Videogerät wählen.", "Select a video device first."));
  } else if (caps.crossbarInputs.empty()) {
    ImGui::TextDisabled(T("Diese Karte hat keine umschaltbaren Eingänge.",
                          "This card has no switchable inputs."));
  } else {
    std::string current = T("Nicht ändern", "Leave alone");
    if (p.capture.crossbarInput >= 0 &&
        p.capture.crossbarInput < (int)caps.crossbarInputs.size()) {
      current = caps.crossbarInputs[(size_t)p.capture.crossbarInput].name;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##crossbar", current.c_str())) {
      if (ImGui::Selectable(T("Nicht ändern", "Leave alone"), p.capture.crossbarInput < 0)) {
        p.capture.crossbarInput = -1;
      }
      for (size_t i = 0; i < caps.crossbarInputs.size(); ++i) {
        const bool selected = ((int)i == p.capture.crossbarInput);
        if (ImGui::Selectable(caps.crossbarInputs[i].name.c_str(), selected)) {
          p.capture.crossbarInput = (int)i;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    HelpMarker(T("Welcher physische Anschluss aufgenommen wird.",
                 "Which physical connector is captured."));
  }

  // ---- audio source ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Ton der Quelle", "Source audio"));

  bool useEmbedded = (p.capture.audioSource == AudioSource::Embedded);
  if (ImGui::Checkbox(T("Eingebettetes Audio des Videogeräts verwenden",
                        "Use the video device's embedded audio"),
                      &useEmbedded)) {
    p.capture.audioSource = useEmbedded ? AudioSource::Embedded : AudioSource::Manual;
  }
  ImGui::SameLine();
  HelpMarker(T("Sucht das Aufnahmegerät auf derselben Karte.",
               "Finds the recording device on the same card."));

  if (useEmbedded) {
    // Resolve once per selected video device, purely to show what was found.
    if (embeddedAudioForDevice_ != p.capture.video.id) {
      embeddedAudioForDevice_ = p.capture.video.id;
      embeddedAudioName_.clear();
      VideoDeviceInfo vinfo;
      vinfo.name = p.capture.video.name;
      vinfo.id = p.capture.video.id;
      AudioDeviceInfo found;
      if (!p.capture.video.empty() && FindEmbeddedAudioDevice(vinfo, &found)) {
        embeddedAudioName_ = found.name;
      }
    }
    char shown[256];
    std::snprintf(shown, sizeof(shown), "%s",
                  embeddedAudioName_.empty()
                      ? T("— nichts Passendes gefunden —", "— nothing matching found —")
                      : embeddedAudioName_.c_str());
    ImGui::BeginDisabled(true);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##embedded", shown, sizeof(shown), ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (embeddedAudioName_.empty() && !p.capture.video.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.35f, 1.0f),
                         T("Nichts gefunden — Haken entfernen und manuell wählen.",
                           "Nothing found — untick and choose manually."));
    }
  } else {
    ImGui::SetNextItemWidth(-1.0f);
    const char* audioPreview =
        p.capture.audioSource == AudioSource::None
            ? T("— kein Ton —", "— no audio —")
            : (p.capture.audio.name.empty() ? T("— nichts ausgewählt —", "— nothing selected —")
                                            : p.capture.audio.name.c_str());
    if (ImGui::BeginCombo("##audiodev", audioPreview)) {
      if (ImGui::Selectable(T("— kein Ton —", "— no audio —"),
                            p.capture.audioSource == AudioSource::None)) {
        p.capture.audioSource = AudioSource::None;
      }
      for (const AudioDeviceInfo& d : audioInputs_) {
        const bool selected =
            (p.capture.audioSource == AudioSource::Manual && d.id == p.capture.audio.id);
        std::string label = d.name + (d.directShow ? "   [DirectShow]" : "");
        if (ImGui::Selectable(label.c_str(), selected)) {
          p.capture.audioSource = AudioSource::Manual;
          p.capture.audio = d.ToRef();
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // ---- format ----
  ImGui::Spacing();
  ImGui::SeparatorText("Format");

  if (!caps.ok || caps.caps.empty()) {
    ImGui::TextDisabled(T("Erst ein Videogerät auswählen.", "Select a video device first."));
    return;
  }

  const std::vector<std::string> subtypes = caps.caps.Subtypes();
  FormatSel& fmt = p.capture.format;

  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Farbformat", "Colour format"), fmt.subtype.c_str())) {
    for (const std::string& s : subtypes) {
      const bool selected = (s == fmt.subtype);
      if (ImGui::Selectable(s.c_str(), selected) && !selected) {
        fmt.subtype = s;
        // Resolution and rate lists depend on the format, so re-pick.
        std::vector<ResolutionOption> res = caps.caps.Resolutions(s);
        if (!res.empty()) {
          fmt.width = res.front().width;
          fmt.height = res.front().height;
        }
        for (const FpsOption& f : caps.caps.FpsList(s, fmt.width, fmt.height)) {
          if (!f.forced) {
            fmt.fps = f.fps;
            break;
          }
        }
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  HelpMarker(T("YUY2, UYVY, NV12: unkomprimiert. MJPG: braucht einen Decoder, kostet Latenz.",
               "YUY2, UYVY, NV12: uncompressed. MJPG: needs a decoder, costs latency."));

  ImGui::BeginDisabled(customFormat_);
  const std::vector<ResolutionOption> resolutions = caps.caps.Resolutions(fmt.subtype);
  std::string resPreview = Format("%dx%d", fmt.width, fmt.height);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Auflösung", "Resolution"), resPreview.c_str())) {
    bool forcedSection = false;
    for (const ResolutionOption& r : resolutions) {
      if (r.forced && !forcedSection) {
        forcedSection = true;
        ImGui::SeparatorText(T("Nicht gemeldet", "Not reported"));
      }
      const bool selected = (r.width == fmt.width && r.height == fmt.height);
      std::string label = Format("%dx%d", r.width, r.height);
      if (ImGui::Selectable(label.c_str(), selected) && !selected) {
        fmt.width = r.width;
        fmt.height = r.height;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  const std::vector<FpsOption> fpsOptions = caps.caps.FpsList(fmt.subtype, fmt.width, fmt.height);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Bildrate", "Frame rate"), FpsLabel(fmt.fps).c_str())) {
    bool forcedSection = false;
    for (const FpsOption& f : fpsOptions) {
      if (f.forced && !forcedSection) {
        forcedSection = true;
        ImGui::SeparatorText(T("Nicht gemeldet", "Not reported"));
      }
      const bool selected = std::fabs(f.fps - fmt.fps) < 0.05;
      if (ImGui::Selectable(FpsLabel(f.fps).c_str(), selected) && !selected) fmt.fps = f.fps;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  HelpMarker(T("\"Nicht gemeldet\": nicht in der Treiberliste, funktioniert meist trotzdem. "
               "Bei Ablehnung fällt CapView zurück.",
               "\"Not reported\": absent from the driver list, usually works anyway. "
               "CapView falls back if rejected."));
  ImGui::EndDisabled();

  ImGui::Checkbox(T("Werte von Hand eingeben", "Enter values manually"), &customFormat_);
  if (customFormat_) {
    ImGui::Indent();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt(T("Breite", "Width"), &customWidth_, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt(T("Höhe", "Height"), &customHeight_, 0, 0);
    float fpsInput = (float)customFps_;
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputFloat(T("Bilder/s", "Frames/s"), &fpsInput, 0.0f, 0.0f, "%.3f")) {
      customFps_ = fpsInput;
    }
    if (ImGui::Button(T("Anwenden##customfmt", "Apply##customfmt"))) {
      fmt.width = Clamp(customWidth_, 16, 8192);
      fmt.height = Clamp(customHeight_, 16, 8192);
      fmt.fps = Clamp(customFps_, 1.0, 480.0);
    }
    ImGui::Unindent();
  }

  fmt.forced = !caps.caps.IsAdvertised(fmt.subtype, fmt.width, fmt.height, fmt.fps);
  ImGui::Spacing();
  if (fmt.forced) {
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), T("Erzwungen: %s", "Forced: %s"),
                       fmt.Label().c_str());
  } else {
    ImGui::TextDisabled(T("Gewählt: %s", "Selected: %s"), fmt.Label().c_str());
  }
}

// ----------------------------------------------------------------- image tab

void SettingsWindow::DrawImageTab() {
  ImageSettings& img = cfg().active().image;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Skalierung", "Scaling"));
  int filter = (int)img.filter;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Filter", "Filter"), &filter, 5, ScaleFilterName, ScaleFilterHelp)) {
    img.filter = (ScaleFilter)filter;
  }
  ImGui::SameLine();
  HelpMarker(ScaleFilterHelp(filter));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Schärfen", "Sharpen"), &img.sharpen, 0.0f, 1.0f, "%.2f");
  ImGui::SameLine();
  HelpMarker(T("Hebt Kanten nach der Skalierung an. 0 schaltet es ab.",
               "Lifts edges after scaling. 0 turns it off."));

  int aspect = (int)img.aspect;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Seitenverhältnis", "Aspect ratio"), &aspect, 5, AspectName, AspectHelp)) {
    img.aspect = (AspectMode)aspect;
  }
  ImGui::SameLine();
  HelpMarker(AspectHelp(aspect));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Halbbilder", "Fields"));
  ImGui::Checkbox(T("Nur bei interlaced Quellen anwenden", "Only apply to interlaced sources"),
                  &img.deinterlaceAuto);
  ImGui::SameLine();
  HelpMarker(T("Meldet die Karte 480i/1080i nicht korrekt, Haken entfernen.",
               "If the card misreports 480i/1080i, untick this."));

  int deint = (int)img.deinterlace;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Deinterlacing", "Deinterlacing"), &deint, 3, DeinterlaceName,
                DeinterlaceHelp)) {
    img.deinterlace = (Deinterlace)deint;
  }
  ImGui::SameLine();
  HelpMarker(DeinterlaceHelp(deint));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildrand abschneiden", "Crop"));
  ImGui::TextDisabled(T("Pixel, die vom Quellbild wegfallen.",
                        "Pixels removed from the source picture."));
  const float quarter =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropl", &img.cropLeft, 0.5f, 0, 2048, T("links %d", "left %d"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropr", &img.cropRight, 0.5f, 0, 2048, T("rechts %d", "right %d"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropt", &img.cropTop, 0.5f, 0, 2048, T("oben %d", "top %d"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(quarter);
  ImGui::DragInt("##cropb", &img.cropBottom, 0.5f, 0, 2048, T("unten %d", "bottom %d"));
  if (ImGui::SmallButton(T("Zurücksetzen##crop", "Reset##crop"))) {
    img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Farbe", "Colour"));
  int range = (int)img.range;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Wertebereich", "Range"), &range, 3, ColorRangeName)) {
    img.range = (ColorRange)range;
  }
  ImGui::SameLine();
  HelpMarker(T("Falsch gewählt: Schwarz wirkt grau, oder Zeichnung geht verloren. "
               "Automatisch misst den Wertebereich am Bild.",
               "Set wrong: black looks grey, or detail is lost. Automatic measures the range "
               "from the picture."));

  int matrix = (int)img.matrix;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Farbmatrix", "Colour matrix"), &matrix, 3, ColorMatrixName)) {
    img.matrix = (ColorMatrix)matrix;
  }
  ImGui::SameLine();
  HelpMarker(T("BT.601 für SD, BT.709 für HD. Falsch gewählt kippen Hauttöne.",
               "BT.601 for SD, BT.709 for HD. Set wrong, skin tones shift."));
  if (img.range == ColorRange::Auto && detectedRange_ && *detectedRange_) {
    ImGui::TextDisabled(T("Gemessen: %s", "Measured: %s"), *detectedRange_);
  }
  ImGui::TextDisabled(T("Beides auch per Rechtsklick im Bild erreichbar.",
                        "Both are also in the right-click menu over the picture."));
}

// ----------------------------------------------------------------- audio tab

void SettingsWindow::DrawAudioTab() {
  AudioSettings& audio = cfg().active().audio;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Wiedergabe", "Playback"));
  const char* outPreview =
      audio.output.name.empty() ? T("Systemstandard", "System default") : audio.output.name.c_str();
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##audioout", outPreview)) {
    if (ImGui::Selectable(T("Systemstandard", "System default"), audio.output.empty())) {
      audio.output = DeviceRef{};
    }
    for (const AudioDeviceInfo& d : audioOutputs_) {
      const bool selected = (d.id == audio.output.id);
      if (ImGui::Selectable(d.name.c_str(), selected)) audio.output = d.ToRef();
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::Checkbox("Exclusive Mode", &audio.exclusive);
  ImGui::SameLine();
  HelpMarker(T("Umgeht den Windows-Mixer, geringere Latenz. Sperrt das Gerät für andere "
               "Programme.",
               "Bypasses the Windows mixer, lower latency. Locks the device against other "
               "programs."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Verzögerung", "Delay"));
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Tonpuffer", "Audio buffer"), &audio.bufferMs, 5, 200, "%d ms");
  ImGui::SameLine();
  HelpMarker(T("Kleiner = weniger Verzögerung, ab einem Punkt Aussetzer. 20-40 ms üblich.",
               "Smaller = less delay, dropouts below a point. 20-40 ms is typical."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("A/V-Versatz", "A/V offset"), &audio.avOffsetMs, -200, 200, "%d ms");
  ImGui::SameLine();
  HelpMarker(T("Positiv verzögert den Ton, negativ das Bild.",
               "Positive delays the audio, negative delays the picture."));
  if (audio.avOffsetMs < 0) {
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                       T("Bild wird um %d ms verzögert — das erhöht die Eingabelatenz.",
                         "Picture delayed by %d ms — this adds input latency."),
                       -audio.avOffsetMs);
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Lautstärke", "Volume"));
  ImGui::SetNextItemWidth(-260.0f);
  // Reads straight from the live config, so the wheel and the hotkeys move this
  // slider while the dialog is open.
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
  }
  ImGui::Checkbox(T("Stumm", "Muted"), &audio.mute);
}

// --------------------------------------------------------------- display tab

void SettingsWindow::DrawDisplayTab() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Sprache", "Language"));
  int lang = (int)app.language;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Sprache", "Language"), &lang, 2, LanguageName)) {
    app.language = (Language)lang;
    SetLanguage(app.language);
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Darstellung", "Appearance"));
  int theme = (int)app.theme;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Design", "Theme"), &theme, 3, ThemeName)) app.theme = (Theme)theme;

  // Accent presets as a row of swatches, plus a free colour picker. The whole
  // palette including the window background is derived from this.
  ImGui::Text("%s", T("Akzentfarbe", "Accent colour"));
  ImGui::SameLine();
  HelpMarker(T("Färbt auch den Fensterhintergrund in einen dunklen Ton davon.",
               "Also tints the window background with a dark shade of it."));

  const int presetCount = AccentPresetCount();
  const float swatch = ImGui::GetFrameHeight();
  for (int i = 0; i < presetCount; ++i) {
    const unsigned rgb = AccentPresetColor(i);
    const ImVec4 color(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                       (rgb & 0xFF) / 255.0f, 1.0f);
    ImGui::PushID(i);
    if (ImGui::ColorButton("##accent", color,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(swatch, swatch))) {
      app.accentColor = rgb;
    }
    ImGui::SetItemTooltip("%s", AccentPresetName(i));
    ImGui::PopID();
    if (i % 6 != 5 && i != presetCount - 1) ImGui::SameLine();
  }

  float custom[3] = {((app.accentColor >> 16) & 0xFF) / 255.0f,
                     ((app.accentColor >> 8) & 0xFF) / 255.0f,
                     (app.accentColor & 0xFF) / 255.0f};
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::ColorEdit3(T("Eigene Farbe", "Custom colour"), custom,
                        ImGuiColorEditFlags_DisplayHex)) {
    app.accentColor = ((unsigned)(Clamp(custom[0], 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
                      ((unsigned)(Clamp(custom[1], 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
                      (unsigned)(Clamp(custom[2], 0.0f, 1.0f) * 255.0f + 0.5f);
  }

  ImGui::Spacing();
  ImGui::SeparatorText(T("Fenster", "Window"));
  ImGui::Checkbox("VSync", &app.vsync);
  ImGui::SameLine();
  HelpMarker(T("Aus ist der größte Latenzgewinn, kann aber Tearing zeigen.",
               "Off is the biggest latency win, but can show tearing."));

  ImGui::Checkbox(T("Immer im Vordergrund", "Always on top"), &app.alwaysOnTop);
  ImGui::Checkbox(T("Mauszeiger im Vollbild ausblenden", "Hide cursor in fullscreen"),
                  &app.hideCursorFullscreen);
  ImGui::Checkbox(T("Bildschirmschoner und Standby verhindern", "Prevent screensaver and sleep"),
                  &app.preventSleep);
  ImGui::SameLine();
  HelpMarker(T("Zeiger nach kurzer Ruhe ausblenden.", "Hides the pointer after a short idle."));

  ImGui::Checkbox(T("Statistik einblenden", "Show statistics"), &app.showStats);
  ImGui::SameLine();
  ImGui::TextDisabled("(F1)");

  ImGui::Spacing();
  ImGui::SeparatorText(T("Lautstärke-Anzeige", "Volume readout"));
  ImGui::Checkbox(T("Bei Änderung einblenden", "Show on change"), &app.showVolumeOsd);
  ImGui::BeginDisabled(!app.showVolumeOsd);
  int corner = (int)app.osdCorner;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Ecke", "Corner"), &corner, 4, OsdCornerName)) {
    app.osdCorner = (OsdCorner)corner;
  }
  ImGui::EndDisabled();
  ImGui::Checkbox(T("Mausrad über dem Bild ändert die Lautstärke",
                    "Mouse wheel over the picture changes the volume"),
                  &app.wheelVolume);

  ImGui::Spacing();
  ImGui::SeparatorText(T("Vollbild", "Fullscreen"));
  std::string monitorPreview = T("Monitor des Fensters", "Whichever monitor the window is on");
  for (const MonitorInfoEntry& m : monitors_) {
    if (m.index == app.fullscreenMonitor) monitorPreview = m.name;
  }
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##fsmonitor", monitorPreview.c_str())) {
    if (ImGui::Selectable(T("Monitor des Fensters", "Whichever monitor the window is on"),
                          app.fullscreenMonitor < 0)) {
      app.fullscreenMonitor = -1;
    }
    for (const MonitorInfoEntry& m : monitors_) {
      const bool selected = (m.index == app.fullscreenMonitor);
      if (ImGui::Selectable(m.name.c_str(), selected)) app.fullscreenMonitor = m.index;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox(T("Beim Start im Vollbild öffnen", "Start in fullscreen"), &app.startFullscreen);

  ImGui::Spacing();
  ImGui::SeparatorText(T("Sonstiges", "Other"));
  ImGui::Checkbox(T("Protokoll in CapView.log schreiben", "Write a log to CapView.log"),
                  &app.logToFile);
  ImGui::SameLine();
  HelpMarker(T("Nur zur Fehlersuche. Wirkt beim nächsten Start.",
               "For troubleshooting only. Takes effect on the next start."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Tastenkürzel", "Shortcuts"));
  ImGui::BulletText(T("Enter — Vollbild ein/aus", "Enter — toggle fullscreen"));
  ImGui::BulletText(T("Esc — Vollbild verlassen", "Esc — leave fullscreen"));
  ImGui::BulletText(T("F1 — Statistik", "F1 — statistics"));
  ImGui::BulletText(T("F2 — Einstellungen", "F2 — settings"));
  ImGui::BulletText(T("F5 — Aufnahme neu starten", "F5 — restart capture"));
  ImGui::BulletText(T("F9 — Aufnahme starten/stoppen", "F9 — start/stop recording"));
  ImGui::BulletText(T("M — stumm, +/- oder Mausrad — Lautstärke",
                      "M — mute, +/- or mouse wheel — volume"));
  ImGui::BulletText(T("Strg+1 bis Strg+9 — Profil wechseln", "Ctrl+1 to Ctrl+9 — switch profile"));
  ImGui::BulletText(T("Rechtsklick im Bild — Menü", "Right-click the picture — menu"));
}

// --------------------------------------------------------------- record tab

void SettingsWindow::FolderRow(const char* id, char* buffer, size_t bufferSize,
                              std::string* value, const std::wstring& defaultFolder) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float browse = ImGui::CalcTextSize(T("Durchsuchen", "Browse")).x + style.FramePadding.x * 2;
  const float reset = ImGui::CalcTextSize(T("Standard", "Default")).x + style.FramePadding.x * 2;
  const float open = ImGui::CalcTextSize(T("Öffnen", "Open")).x + style.FramePadding.x * 2;

  ImGui::SetNextItemWidth(-(browse + reset + open + style.ItemSpacing.x * 3.0f));
  const std::string field = std::string("##") + id;
  if (ImGui::InputTextWithHint(field.c_str(), ToUtf8(defaultFolder).c_str(), buffer, bufferSize)) {
    *value = buffer;
  }

  ImGui::SameLine();
  if (ImGui::Button((std::string(T("Durchsuchen", "Browse")) + "##" + id).c_str())) {
    const std::wstring start = value->empty() ? defaultFolder : ToWide(*value);
    const std::wstring picked =
        PickFolder((HWND)ImGui::GetMainViewport()->PlatformHandleRaw, start);
    if (!picked.empty()) {
      *value = ToUtf8(picked);
      std::snprintf(buffer, bufferSize, "%s", value->c_str());
    }
  }

  ImGui::SameLine();
  // An empty string is what "default" means in the config, so resetting clears
  // the field rather than writing the resolved path into it -- that way the
  // folder keeps following the user profile if it ever moves.
  ImGui::BeginDisabled(value->empty());
  if (ImGui::Button((std::string(T("Standard", "Default")) + "##" + id).c_str())) {
    value->clear();
    buffer[0] = 0;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ImGui::Button((std::string(T("Öffnen", "Open")) + "##" + id).c_str())) {
    std::wstring folder = value->empty() ? defaultFolder : ToWide(*value);
    EnsureFolder(folder);
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void SettingsWindow::DrawRecordTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;

  if (!recordBuffersLoaded_) {
    std::snprintf(ffmpegPathBuffer_, sizeof(ffmpegPathBuffer_), "%s", rec.ffmpegPath.c_str());
    std::snprintf(folderBuffer_, sizeof(folderBuffer_), "%s", rec.outputFolder.c_str());
    std::snprintf(shotFolderBuffer_, sizeof(shotFolderBuffer_), "%s", rec.screenshotFolder.c_str());
    recordBuffersLoaded_ = true;
  }

  ImGui::Spacing();
  ImGui::TextWrapped(
      T("Aufgenommen wird das Bild in Quellauflösung, nach Zuschnitt und Deinterlacing, vor der "
        "Fensterskalierung. Die Fenstergröße hat keinen Einfluss auf das Ergebnis.",
        "Recording captures the picture at source resolution, after crop and deinterlacing, "
        "before window scaling. Window size does not affect the result."));
  ImGui::Spacing();

  // ---- output ----
  ImGui::SeparatorText(T("Ausgabe", "Output"));

  int container = (int)rec.container;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Container", "Container"), &container, 2, RecordContainerName)) {
    rec.container = (RecordContainer)container;
  }
  ImGui::SameLine();
  HelpMarker(T("MKV übersteht einen Absturz. Eine nicht sauber geschlossene MP4 lässt sich nicht "
               "öffnen.",
               "MKV survives a crash. An MP4 that was not closed properly will not open."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Bitrate", "Bitrate"), &rec.bitrateKbps, 1000, 100000, "%d kbit/s");

  int fps = (int)std::lround(rec.fps);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::SliderInt(T("Bildrate", "Frame rate"), &fps, 0, 240,
                       fps == 0 ? T("wie die Quelle", "same as source") : "%d fps")) {
    rec.fps = (double)fps;
  }
  ImGui::SameLine();
  HelpMarker(T("0 = Bildrate der Quelle. Niedriger verwirft Bilder, Auflösung bleibt.",
               "0 = source frame rate. Lower drops frames, resolution unchanged."));

  FolderRow("recfolder", folderBuffer_, sizeof(folderBuffer_), &rec.outputFolder,
            DefaultRecordFolder());

  ImGui::Checkbox(T("Bei Größe aufteilen", "Split at size"), &rec.splitFiles);
  ImGui::SameLine();
  HelpMarker(T("Nur für FAT32 nötig. NTFS und exFAT haben kein 4-GB-Limit. Beim Teilen entsteht "
               "eine kurze Lücke.",
               "Only needed on FAT32. NTFS and exFAT have no 4 GB limit. Splitting leaves a "
               "brief gap."));
  ImGui::BeginDisabled(!rec.splitFiles);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Teilgröße", "Split size"), &rec.splitSizeMb, 100, 20000, "%d MB");
  ImGui::EndDisabled();

  // ---- encoder ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Encoder", "Encoder"));

  int encoder = (int)rec.encoder;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Encoder", "Encoder"), &encoder, 10, RecordEncoderName)) {
    rec.encoder = (RecordEncoder)encoder;
  }
  ImGui::SameLine();
  HelpMarker(T("Automatisch: bester Encoder, der den Test besteht, Hardware zuerst.",
               "Automatic: best encoder that passes the test, hardware first."));

  const bool busy = downloader_.busy();
  ImGui::BeginDisabled(!ffmpeg || !ffmpeg->found || busy || probeBusy_);
  if (ImGui::Button(probeBusy_ ? T("Wird geprüft ...", "Testing ...")
                               : T("Encoder testen", "Test encoders"))) {
    probeRequested_ = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Kodiert je zwei Testbilder. Die Encoder-Liste des Builds nennt nur, was "
               "einkompiliert ist, nicht was die Hardware kann.",
               "Encodes two test frames each. The build's encoder list only names what was "
               "compiled in, not what the hardware can do."));

  if (ffmpeg && ffmpeg->found) {
    for (const EncoderInfo& e : ffmpeg->encoders) {
      if (!e.tested) continue;
      ImGui::TextColored(e.available ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f)
                                     : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                         "%s %s", e.available ? "+" : "-", e.label.c_str());
      if (!e.available && !e.error.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", e.error.c_str());
      }
    }
  }

  const EncoderInfo* selected =
      ffmpeg ? (rec.encoder == RecordEncoder::Auto ? nullptr : ffmpeg->Find(rec.encoder)) : nullptr;
  const bool softwareSelected = rec.encoder == RecordEncoder::X264 ||
                                rec.encoder == RecordEncoder::X265 ||
                                (selected && !selected->hardware);
  ImGui::BeginDisabled(!softwareSelected);
  int speed = (int)rec.speed;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Geschwindigkeit", "Speed"), &speed, 5, RecordSpeedName)) {
    rec.speed = (RecordSpeed)speed;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Nur für CPU-Encoder. Hardware-Encoder behalten die Werkseinstellung.",
               "Software encoders only. Hardware encoders keep the vendor default."));

  // ---- ffmpeg ----
  ImGui::Spacing();
  ImGui::SeparatorText("ffmpeg");
  if (ffmpeg && ffmpeg->found) {
    ImGui::TextWrapped("%s", ffmpeg->version.c_str());
    ImGui::TextDisabled("%s", ffmpeg->path.c_str());
  } else {
    ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.35f, 1.0f), "%s",
                       T("Nicht gefunden — ohne ffmpeg keine Aufnahme.",
                         "Not found — no recording without ffmpeg."));
  }

  ImGui::BeginDisabled(busy);
  if (ImGui::Button(ffmpeg && ffmpeg->found ? T("Neu herunterladen", "Download again")
                                            : T("ffmpeg herunterladen", "Download ffmpeg"))) {
    downloader_.Start(ExeDirectory() + L"ffmpeg");
  }
  ImGui::SetItemTooltip(T("Statisches Build von gyan.dev, rund 106 MB, SHA-256 wird geprüft.",
                          "Static build from gyan.dev, about 106 MB, SHA-256 is verified."));
  ImGui::SameLine();
  if (ImGui::Button(T("Auf Updates prüfen", "Check for updates"))) {
    downloader_.StartVersionCheck();
  }
  ImGui::EndDisabled();

  if (busy) {
    const float p = downloader_.progress();
    if (p >= 0.0f) ImGui::ProgressBar(p, ImVec2(-1.0f, 0.0f));
    ImGui::TextDisabled("%s", downloader_.message().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Abbrechen", "Cancel"))) downloader_.Cancel();
  } else if (downloader_.state() != FfmpegDownloader::State::Idle) {
    const bool failed = downloader_.state() == FfmpegDownloader::State::Failed;
    ImGui::TextColored(failed ? ImVec4(0.95f, 0.5f, 0.35f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f),
                       "%s", downloader_.message().c_str());
    // A fresh download only counts once the locator has confirmed it runs.
    if (!failed && !downloader_.resultPath().empty() && ffmpeg && !ffmpeg->found) {
      *ffmpeg = LocateFfmpeg(rec.ffmpegPath);
    }
  }

  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputTextWithHint("##ffmpegpath",
                               T("Eigener Pfad zu ffmpeg.exe (optional)",
                                 "Custom path to ffmpeg.exe (optional)"),
                               ffmpegPathBuffer_, sizeof(ffmpegPathBuffer_))) {
    rec.ffmpegPath = ffmpegPathBuffer_;
  }

  // ---- stills ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Screenshots", "Screenshots"));
  ImGui::TextWrapped(T("Einzelbild in Quellauflösung, ohne Bedienoberfläche. Benötigt kein ffmpeg.",
                       "Single frame at source resolution, without the interface. Does not need "
                       "ffmpeg."));
  ImGui::Spacing();

  int shot = (int)rec.screenshotFormat;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Format", "Format"), &shot, 2, ScreenshotFormatName)) {
    rec.screenshotFormat = (ScreenshotFormat)shot;
  }
  ImGui::SameLine();
  HelpMarker(T("PNG verlustfrei, JPEG kleiner.", "PNG lossless, JPEG smaller."));

  ImGui::BeginDisabled(rec.screenshotFormat != ScreenshotFormat::Jpeg);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("JPEG-Qualität", "JPEG quality"), &rec.jpegQuality, 1, 100, "%d %%");
  ImGui::EndDisabled();

  FolderRow("shotfolder", shotFolderBuffer_, sizeof(shotFolderBuffer_), &rec.screenshotFolder,
            DefaultScreenshotFolder());
}

// ----------------------------------------------------------------- tools tab

void SettingsWindow::DrawToolsTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Nach MP4 umpacken", "Rewrap to MP4"));

  ImGui::TextWrapped(T("Legt MKV-Dateien ohne Neukodierung in eine MP4 um. Dauert Sekunden und "
                       "kostet keine Qualität. Das Original bleibt erhalten.",
                       "Puts MKV files into an MP4 without re-encoding. Takes seconds and costs "
                       "no quality. The original is kept."));
  ImGui::Spacing();

  const bool remuxBusy = remuxer_.busy();
  ImGui::BeginDisabled(remuxBusy || !ffmpeg || !ffmpeg->found);
  if (ImGui::Button(T("Dateien wählen ...", "Choose files ..."))) {
    HWND owner = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
    const std::wstring start =
        rec.outputFolder.empty() ? DefaultRecordFolder() : ToWide(rec.outputFolder);
    const std::vector<std::wstring> files = AskForRecordings(owner, start);
    if (!files.empty()) remuxer_.Start(ToWide(ffmpeg->path), files);
  }
  ImGui::EndDisabled();

  if (remuxBusy) {
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen##remux", "Cancel##remux"))) remuxer_.Cancel();
  }
  if (ffmpeg && !ffmpeg->found) {
    ImGui::SameLine();
    ImGui::TextDisabled(T("(benötigt ffmpeg)", "(needs ffmpeg)"));
  }

  if (remuxer_.state() != Remuxer::State::Idle) {
    if (remuxBusy) ImGui::ProgressBar(remuxer_.progress(), ImVec2(-1.0f, 0.0f));
    const std::string status = remuxer_.message();
    if (!status.empty()) {
      const bool failed = remuxer_.state() == Remuxer::State::Failed;
      ImGui::TextColored(
          failed ? ImVec4(0.9f, 0.5f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text], "%s",
          status.c_str());
    }
    // Only failures are listed; a success is its own file on disk.
    for (const Remuxer::Item& item : remuxer_.items()) {
      if (!item.done || item.ok) continue;
      ImGui::TextDisabled("- %s", item.error.c_str());
    }
  }
}

// --------------------------------------------------------------- hotkeys tab

void SettingsWindow::DrawHotkeysTab() {
  ImGui::Spacing();
  ImGui::TextWrapped(T("Zum Ändern die Taste anklicken und die neue Kombination drücken. "
                       "Esc bricht ab, Entf löscht die Belegung.",
                       "Click a shortcut and press the new combination. Esc cancels, Delete "
                       "clears the binding."));
  ImGui::Spacing();

  Hotkeys& keys = cfg().hotkeys;

  if (ImGui::BeginTable("hotkeys", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T("Aktion", "Action"), ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn(T("Taste", "Shortcut"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
      const HotkeyAction action = (HotkeyAction)i;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(HotkeyActionName(action));

      ImGui::TableSetColumnIndex(1);
      const bool capturing = (captureAction_ == i);
      const std::string label = (capturing ? std::string(T("Taste drücken ...", "Press a key ..."))
                                           : HotkeyText(keys[action])) +
                                "##hk" + std::to_string(i);

      // A duplicate is flagged rather than refused: which of the two the user
      // meant to keep is not something this dialog can know.
      bool clash = false;
      if (keys[action].bound()) {
        for (int j = 0; j < (int)HotkeyAction::Count && !clash; ++j) {
          if (j != i && keys.items[j] == keys[action]) clash = true;
        }
      }
      if (clash) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.6f, 0.35f, 1.0f));
      if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
        captureAction_ = capturing ? -1 : i;
      }
      if (clash) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(T("Mehrfach belegt.", "Bound more than once."));
      }
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  if (ImGui::Button(T("Alle zurücksetzen", "Reset all"))) {
    cfg().hotkeys = Hotkeys();
    captureAction_ = -1;
  }

  ImGui::Spacing();
  ImGui::TextDisabled(T("Fest belegt: Esc verlässt das Vollbild und schließt Dialoge, "
                        "Strg+1 bis Strg+9 wählen ein Profil, Alt+F4 beendet.",
                        "Fixed: Esc leaves fullscreen and closes dialogs, Ctrl+1 to Ctrl+9 pick "
                        "a profile, Alt+F4 quits."));
}

void SettingsWindow::OfferKey(int vk, bool ctrl, bool shift, bool alt) {
  if (captureAction_ < 0 || captureAction_ >= (int)HotkeyAction::Count) return;

  if (vk == VK_ESCAPE) {  // cancel, change nothing
    captureAction_ = -1;
    return;
  }
  if (vk == VK_DELETE || vk == VK_BACK) {  // unbind
    cfg().hotkeys.items[captureAction_] = HotkeyBinding{};
    captureAction_ = -1;
    return;
  }
  // A modifier on its own is not a shortcut yet -- keep waiting so the user can
  // hold Ctrl and then reach for the actual key.
  if (IsReservedKey(vk)) return;

  HotkeyBinding binding;
  binding.vk = vk;
  binding.ctrl = ctrl;
  binding.shift = shift;
  binding.alt = alt;
  cfg().hotkeys.items[captureAction_] = binding;
  captureAction_ = -1;
}

// -------------------------------------------------------------- profiles tab

void SettingsWindow::DrawProfilesTab() {
  Config& c = cfg();
  ImGui::Spacing();
  ImGui::TextWrapped(
      T("Ein Profil hält Gerät, Eingang, Format sowie alle Bild- und Toneinstellungen. "
        "Für mehrere Konsolen an einer Karte einmal einrichten, danach mit Strg+Zahl "
        "umschalten.",
        "A profile holds the device, input, format and all picture and audio settings. "
        "Set one up per console on the same card, then switch with Ctrl+number."));
  ImGui::Spacing();

  const float listHeight =
      ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
  if (ImGui::BeginListBox("##profiles", ImVec2(-1.0f, std::max(120.0f, listHeight)))) {
    for (int i = 0; i < (int)c.profiles.size(); ++i) {
      const bool selected = (i == c.activeProfile);
      std::string label = Format("%d.  %s", i + 1, c.profiles[(size_t)i].name.c_str());
      if (ImGui::Selectable(label.c_str(), selected)) c.activeProfile = i;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndListBox();
  }

  if (ImGui::Button(T("Neu", "New"))) {
    Profile p;
    p.name = Format(T("Profil %d", "Profile %d"), (int)c.profiles.size() + 1);
    c.profiles.push_back(p);
    c.activeProfile = (int)c.profiles.size() - 1;
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Duplizieren", "Duplicate"))) {
    Profile copy = c.active();
    copy.name += T(" (Kopie)", " (copy)");
    c.profiles.push_back(copy);
    c.activeProfile = (int)c.profiles.size() - 1;
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Umbenennen", "Rename"))) {
    renameTarget_ = c.activeProfile;
    std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", c.active().name.c_str());
    ImGui::OpenPopup("rename_profile");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(c.profiles.size() <= 1);
  if (ImGui::Button(T("Löschen", "Delete"))) {
    c.profiles.erase(c.profiles.begin() + c.activeProfile);
    c.activeProfile = Clamp(c.activeProfile, 0, (int)c.profiles.size() - 1);
  }
  ImGui::EndDisabled();

  if (ImGui::BeginPopupModal("rename_profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(T("Profil umbenennen", "Rename profile"));
    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("##name", renameBuffer_, sizeof(renameBuffer_));
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(100, 0))) {
      if (renameTarget_ >= 0 && renameTarget_ < (int)c.profiles.size()) {
        std::string trimmed = Trim(renameBuffer_);
        if (!trimmed.empty()) c.profiles[(size_t)renameTarget_].name = trimmed;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"), ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace cap
