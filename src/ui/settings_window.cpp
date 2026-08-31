#include "ui/settings_window.h"

#include "record/recorder.h"
#include "vcam/virtual_camera.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "i18n.h"
#include "record/screenshot.h"
#include "ui/file_dialog.h"
#include "imgui.h"

namespace cap {
namespace {

using NameFn = const char* (*)(int);

// A small "?" that shows an explanation on hover.
// Wie SetItemTooltip, aber mit Umbruch. Der eingebaute bricht nicht um, was bei
// den kurzen Einzeilern hier nirgends auffaellt und bei einem laengeren Text
// sofort: er laeuft dann quer ueber den Bildschirm aus dem Fenster heraus.
// Dieselbe Breite wie HelpMarker, damit beide gleich aussehen.
void WrappedTooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

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

// Wie TextDisabled, aber mit Umbruch. ImGui hat beides nur einzeln: TextDisabled
// faerbt und bricht nicht um, TextWrapped bricht um und faerbt nicht. Ein kurzer
// Halbsatz merkt den Unterschied nie, ein ganzer Erklaersatz sofort -- er
// schiebt das Fenster in die Breite, bis man waagerecht scrollen muss, um das
// Ende zu lesen.
void TextDisabledWrapped(const char* text) {
  if (!text || !*text) return;
  const ImVec4 grey = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
  ImGui::PushStyleColor(ImGuiCol_Text, grey);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// Dasselbe in der Farbe, die im Rest des Fensters schon "sieh hier hin" heisst
// -- dieselben Werte wie bei "Erzwungen: ...". Grau waere hier falsch: grau
// sagt "Nebensache", und ein Hinweis, den man lesen soll, ist keine.
void TextWarningWrapped(const char* text) {
  if (!text || !*text) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
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

// A level bar. Linear sample values crowd everything useful into the top of the
// bar, so the scale is decibels: -60 dB at the left, 0 dB at the right, with the
// last sixth coloured because that is where clipping lives.
void LevelMeter(const char* id, float peak, bool active) {
  const float height = ImGui::GetFrameHeight() * 0.55f;
  const ImVec2 size(ImGui::CalcItemWidth(), height);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiStyle& style = ImGui::GetStyle();

  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);

  if (active && peak > 0.0f) {
    const float db = 20.0f * std::log10(std::max(peak, 1e-5f));
    const float norm = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    const bool hot = db > -6.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x * norm, pos.y + size.y),
                      hot ? IM_COL32(230, 150, 60, 255) : IM_COL32(90, 200, 120, 255),
                      style.FrameRounding);
  }

  // -6 dB mark, the usual "do not go past this" line.
  const float markX = pos.x + size.x * (54.0f / 60.0f);
  dl->AddLine(ImVec2(markX, pos.y), ImVec2(markX, pos.y + size.y), IM_COL32(255, 255, 255, 70));

  ImGui::Dummy(size);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(active && peak > 0.0f ? "%.1f dB" : T("kein Signal", "no signal"),
                      20.0f * std::log10(std::max(peak, 1e-5f)));
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(id);
}

std::string FpsLabel(double fps) {
  // Zero is the stored form of "highest available" -- a mode, not a rate.
  if (fps <= 0.0) return T("Höchste verfügbare", "Highest available");
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
  restorePos_ = true;
  listsValid_ = false;
  renameTarget_ = -1;
  recordBuffersLoaded_ = false;
}

bool SettingsWindow::takeProbeRequest() {
  const bool requested = probeRequested_;
  probeRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCropPickRequest() {
  const bool requested = cropPickRequested_;
  cropPickRequested_ = false;
  return requested;
}

bool SettingsWindow::takeDeviceConfigRequest() {
  const bool requested = deviceConfigRequested_;
  deviceConfigRequested_ = false;
  return requested;
}

bool SettingsWindow::takeRestartRequest() {
  const bool requested = restartRequested_;
  restartRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCropDetectRequest() {
  const bool requested = cropDetectRequested_;
  cropDetectRequested_ = false;
  return requested;
}

bool SettingsWindow::takeCardResetRequest() {
  const bool requested = cardResetRequested_;
  cardResetRequested_ = false;
  return requested;
}

int SettingsWindow::takeVirtualCameraRequest() {
  const int request = virtualCameraRequest_;
  virtualCameraRequest_ = 0;
  return request;
}

void SettingsWindow::SetVirtualCameraState(bool running,
                                           const std::vector<VirtualCamera::Consumer>& consumers) {
  vcamRunning_ = running;
  vcamConsumers_ = consumers;
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
  if (!probeAllowed_) return probed_;
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
  // A subtype without a size counts as not set -- that is what re-reading the
  // card leaves behind -- but the subtype in it is still a wish, so it is
  // handed on rather than thrown away. Without that this would put the
  // ordinary choice back the moment the settings page was drawn.
  if (!p.capture.format.valid() ||
      std::find(subtypes.begin(), subtypes.end(), p.capture.format.subtype) == subtypes.end()) {
    p.capture.format = caps.caps.PickDefault(p.capture.format.subtype);
  }
  p.capture.format.forced =
      !caps.caps.IsAdvertised(p.capture.format.subtype, p.capture.format.width,
                              p.capture.format.height, p.capture.format.fps);
}

// ----------------------------------------------------------------------- draw

void SettingsWindow::PollFileDialog(FfmpegInfo* ffmpeg) {
  std::vector<std::wstring> picked;
  int tag = kPickNone;
  if (!picker_.TakeResult(&picked, &tag)) return;
  if (picked.empty()) return;  // cancelled

  RecordSettings& rec = cfg().record;
  switch (tag) {
    case kPickRecordFolder:
      rec.outputFolder = ToUtf8(picked.front());
      std::snprintf(folderBuffer_, sizeof(folderBuffer_), "%s", rec.outputFolder.c_str());
      break;
    case kPickShotFolder:
      rec.screenshotFolder = ToUtf8(picked.front());
      std::snprintf(shotFolderBuffer_, sizeof(shotFolderBuffer_), "%s",
                    rec.screenshotFolder.c_str());
      break;
    case kPickFfmpeg:
      rec.ffmpegPath = ToUtf8(picked.front());
      std::snprintf(ffmpegPathBuffer_, sizeof(ffmpegPathBuffer_), "%s", rec.ffmpegPath.c_str());
      if (ffmpeg) *ffmpeg = LocateFfmpeg(rec.ffmpegPath);
      break;
    case kPickRemux:
      if (ffmpeg && ffmpeg->found) remuxer_.Start(ToWide(ffmpeg->path), picked);
      break;
    default:
      break;
  }
}

SettingsWindow::Result SettingsWindow::Draw(const DeviceProbeResult* liveCaps,
                                            FfmpegInfo* ffmpeg) {
  if (!open_ || !live_) return Result::None;
  PollFileDialog(ffmpeg);
  if (!listsValid_) RefreshDeviceLists();

  Result result = Result::None;

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  bool stayOpen = true;
  bool opened;
  if (fillsWindow_) {
    // The dialog *is* the window here: the host already provides a title bar, a
    // border and a close button, so the panel inside it gets none of those and
    // simply covers the whole thing. The footer buttons still work -- the caller
    // acts on the result either way.
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    opened = ImGui::Begin("###capview_settings_host", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoSavedSettings);
  } else {
    // Wo das Feld zuletzt stand -- und der Rückfall in die Mitte, wenn das
    // nicht mehr geht.
    //
    // ImGui führt seine Fensterlagen im Kontext, also überlebt sie das
    // Umschalten zwischen eingebettet und freigestellt von selbst. Was sie
    // nicht überlebt, ist ein Neustart: die imgui.ini ist abgeschaltet, damit
    // neben der exe nichts liegt, das dort niemand erwartet. Also wird sie hier
    // mitgeschrieben.
    //
    // Der Rückfall greift, wenn das Hauptfenster inzwischen kleiner ist als
    // damals -- dann läge das Feld ganz oder überwiegend draußen, und ohne
    // Titelleiste in Reichweite bekäme man es nicht mehr zurück.
    if (restorePos_) {
      restorePos_ = false;
      const ImVec2 work = viewport->WorkPos;
      const ImVec2 area = viewport->WorkSize;
      const AppSettings& app = cfg().app;

      const float x = (float)app.settingsPanelX;
      const float y = (float)app.settingsPanelY;
      const float w = (float)app.settingsPanelW;
      const float h = (float)app.settingsPanelH;

      // Hundert Pixel Titelleiste müssen greifbar bleiben, sonst ist es
      // unerreichbar und die Mitte ist die bessere Antwort.
      const bool haveSaved = w > 200.0f && h > 200.0f;
      const bool reachable = haveSaved && x + 100.0f > work.x && x < work.x + area.x - 100.0f &&
                             y >= work.y - 4.0f && y < work.y + area.y - 40.0f;

      if (reachable) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(std::min(w, area.x), std::min(h, area.y)), ImGuiCond_Always);
      } else {
        ImGui::SetNextWindowPos(
            ImVec2(work.x + area.x * 0.5f, work.y + area.y * 0.5f), ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
      }
    }
    ImGui::SetNextWindowSize(ImVec2(780.0f, 660.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 460.0f), ImVec2(FLT_MAX, FLT_MAX));

    // Everything after "###" is the id, everything before it is the label.
    // Without this the window is a different window in each language: ImGui keys
    // position, size and the selected tab off the name, so switching language
    // moved the dialog, resized it and threw you back to the first tab.
    opened = ImGui::Begin(T("Einstellungen###capview_settings", "Settings###capview_settings"),
                          &stayOpen, ImGuiWindowFlags_NoCollapse);
  }
  if (!fillsWindow_ && opened) {
    // Mitschreiben, solange es sichtbar ist. Billig, und es erspart einen
    // eigenen Weg fuer "der Nutzer hat gerade losgelassen".
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    AppSettings& app = cfg().app;
    app.settingsPanelX = (int)pos.x;
    app.settingsPanelY = (int)pos.y;
    app.settingsPanelW = (int)size.x;
    app.settingsPanelH = (int)size.y;
  }
  if (!opened) {
    ImGui::End();
    return Result::None;
  }
  if (!stayOpen) {
    ImGui::End();
    return Result::Close;
  }

  if (!reason_.empty()) {
    // AutoResizeY makes the box follow the text. It used to be a fixed 1.6 rows,
    // which turned a longer message -- "the device is already in use by another
    // program", say -- into a scrollbar inside a banner.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.85f, 0.55f, 0.15f, 0.18f));
    ImGui::BeginChild("banner", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextWrapped("%s", reason_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  const DeviceProbeResult& caps = CapsFor(cfg().active().capture.video, liveCaps);
  captureRunning_ = liveCaps != nullptr;
  EnsureValidFormat(caps);

  // Height to leave free for the button row. Each tab scrolls inside its own
  // child, which is what keeps the tab strip pinned to the top.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f;

  // Which tab is open is state ImGui keeps per *context*, and the settings are
  // drawn into a different context depending on whether they live in a window
  // of their own -- so switching between the two always landed back on the
  // first tab. Remembered here instead, and asked for once whenever the context
  // underneath changes.
  // Once per run, the tab from the configuration; after that, whatever was last
  // opened. The context check below covers the other case -- which tab is open
  // is state ImGui keeps per context, and the settings are drawn into a
  // different one depending on whether they live in a window of their own.
  if (!tabRestored_) {
    tabRestored_ = true;
    activeTab_ = cfg().app.settingsTab;
    wantTab_ = activeTab_;
  }
  ImGuiContext* nowContext = ImGui::GetCurrentContext();
  if (nowContext != tabContext_) {
    tabContext_ = nowContext;
    wantTab_ = activeTab_;
  }
  int tabIndex = 0;
  auto tabFlags = [&]() {
    const bool select = wantTab_ == tabIndex;
    return select ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
  };

  if (ImGui::BeginTabBar("settings_tabs", ImGuiTabBarFlags_None)) {
    if (ImGui::BeginTabItem(T("Quelle###source", "Source###source"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_source", ImVec2(0, -footer));
      DrawSourceTab(caps);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    // Ohne Gerät gibt es nur eine sinnvolle Handlung, und das ist eines
    // auszuwählen. Neun weitere Reiter voller Regler, die alle auf ein Bild
    // wirken sollen, das es nicht gibt, sind an dieser Stelle kein Angebot
    // sondern eine Hürde. Sie kommen zurück, sobald die Karte läuft -- und dann
    // gleich passend zu dem, was sie tatsächlich liefert.
    if (!cfg().active().capture.video.empty()) {
    if (ImGui::BeginTabItem(T("Bild###picture", "Picture###picture"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_image", ImVec2(0, -footer));
      DrawImageTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("HDR###hdr", "HDR###hdr"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_hdr", ImVec2(0, -footer));
      DrawHdrTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Ton###audio", "Audio###audio"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_audio", ImVec2(0, -footer));
      DrawAudioTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Anzeige###display", "Display###display"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_display", ImVec2(0, -footer));
      DrawDisplayTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Aufnahme###recording", "Recording###recording"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_record", ImVec2(0, -footer));
      DrawRecordTab(ffmpeg);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Encoder###encoder", "Encoder###encoder"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_encoder", ImVec2(0, -footer));
      DrawEncoderTab(ffmpeg);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Tasten###keys", "Keys###keys"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_keys", ImVec2(0, -footer));
      DrawHotkeysTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Profile###profiles", "Profiles###profiles"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_profiles", ImVec2(0, -footer));
      DrawProfilesTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    if (ImGui::BeginTabItem(T("Updates###updates", "Updates###updates"), nullptr, tabFlags())) {
      activeTab_ = tabIndex;
      ImGui::BeginChild("scroll_updates", ImVec2(0, -footer));
      DrawUpdatesTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ++tabIndex;
    }  // alle Reiter außer Quelle, nur mit ausgewähltem Gerät
    ImGui::EndTabBar();
  }
  wantTab_ = -1;
  cfg().app.settingsTab = activeTab_;

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

// ---------------------------------------------------------------- updates tab

void SettingsWindow::DrawUpdatesTab() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();

  ImGui::SeparatorText(T("Version", "Version"));
  ImGui::Text("%s", T("Installiert:", "Installed:"));
  ImGui::SameLine();
  ImGui::TextDisabled("%s", Updater::currentVersion());

  ImGui::Checkbox(T("Beim Start nach Updates suchen", "Check for updates at startup"),
                  &app.checkUpdatesOnStart);
  ImGui::SameLine();
  HelpMarker(T("Fragt die Releases auf GitHub ab. Heruntergeladen wird nichts, solange du "
               "es nicht verlangst.",
               "Asks GitHub for the newest release. Nothing is downloaded until you ask "
               "for it."));

  if (!updater_) return;
  const UpdateStatus st = updater_->status();
  const bool busy = updater_->busy();

  ImGui::Spacing();
  ImGui::SeparatorText(T("Stand", "Status"));

  ImGui::BeginDisabled(busy);
  if (ImGui::Button(T("Jetzt suchen", "Check now"))) updater_->CheckAsync();
  ImGui::EndDisabled();

  ImGui::SameLine();
  switch (st.state) {
    case UpdateStatus::State::Idle:
      ImGui::TextDisabled("%s", T("noch nicht gesucht", "not checked yet"));
      break;
    case UpdateStatus::State::Checking:
      ImGui::TextDisabled("%s", T("wird gesucht ...", "checking ..."));
      break;
    case UpdateStatus::State::UpToDate:
      ImGui::TextDisabled(T("aktuell (neueste ist %s)", "up to date (newest is %s)"),
                          st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Available:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                         T("%s ist verfügbar", "%s is available"), st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Downloading:
      ImGui::TextDisabled("%s", T("wird geladen ...", "downloading ..."));
      break;
    case UpdateStatus::State::Ready:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                         T("%s ist eingesetzt", "%s is installed"), st.latestVersion.c_str());
      break;
    case UpdateStatus::State::Failed:
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", UpdateErrorText(st).c_str());
      break;
  }

  if (st.state == UpdateStatus::State::Available) {
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(T("Herunterladen und installieren", "Download and install"))) {
      updater_->InstallAsync();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpMarker(T("Ersetzt CapView.exe. Die bisherige Version wird beiseite gelegt und beim "
                 "nächsten Start entfernt, damit ein misslungenes Update nichts kaputt macht.",
                 "Replaces CapView.exe. The previous build is moved aside and removed on the "
                 "next start, so a failed update breaks nothing."));
  }

  if (st.state == UpdateStatus::State::Ready) {
    ImGui::Spacing();
    if (ImGui::Button(T("Jetzt neu starten", "Restart now"))) restartRequested_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("oder beim nächsten Start", "or on the next start"));
  }

  if (!st.notes.empty() && (st.state == UpdateStatus::State::Available ||
                            st.state == UpdateStatus::State::Ready)) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Was neu ist", "What is new"));
    ImGui::BeginChild("release_notes", ImVec2(0, 220.0f), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(st.notes.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
  }
}

// ---------------------------------------------------------------- source tab

void SettingsWindow::DrawSourceTab(const DeviceProbeResult& caps) {
  Profile& p = cfg().active();
  ImGui::Spacing();

  ImGui::SeparatorText(T("Videogerät", "Video device"));

  // A hybrid card carries both halves on one board and cannot say which one has
  // something plugged into it -- this one exposes no input selector at all, so
  // there is nothing to read. Saying so is left to the person who plugged the
  // cable in, and everything that only makes sense for one of the two follows
  // from it.
  {
    int kind = (int)p.capture.signalKind;
    ImGui::SetNextItemWidth(-260.0f);
    if (ComboEnum(T("Signalart", "Signal type"), &kind, kSignalKindCount, SignalKindName)) {
      p.capture.signalKind = (SignalKind)kind;
    }
    ImGui::SameLine();
    HelpMarker(T("Blendet aus, was für die andere Art keinen Sinn ergibt. Automatisch "
                 "heißt: analog, wenn die Karte einen Analogdecoder hat.",
                 "Hides what makes no sense for the other kind. Automatic means analogue "
                 "when the card has an analogue decoder."));
  }

  const char* preview = p.capture.video.name.empty()
                            ? T("— nichts ausgewählt —", "— nothing selected —")
                            : p.capture.video.name.c_str();
  ImGui::SetNextItemWidth(-150.0f);
  if (ImGui::BeginCombo("##videodev", preview)) {
    if (videoDevices_.empty()) {
      ImGui::TextDisabled(T("Keine Videogeräte gefunden", "No video devices found"));
    }
    // Nichts auszuwählen ist auch eine Auswahl. Niemand braucht das im Betrieb;
    // es ist der einzige Weg, den leeren Zustand zu sehen, ohne die
    // Konfigurationsdatei von Hand anzufassen.
    {
      const bool none = p.capture.video.empty();
      if (ImGui::Selectable(T("— kein Gerät —", "— no device —"), none) && !none) {
        p.capture.video = DeviceRef{};
        p.capture.format = FormatSel{};
        p.capture.videoStandard = -1;
        p.capture.crossbarInput = -1;
        embeddedAudioForDevice_.clear();
      }
      ImGui::Separator();
    }
    for (const VideoDeviceInfo& d : videoDevices_) {
      const bool selected = (d.id == p.capture.video.id);
      if (ImGui::Selectable(d.name.c_str(), selected) && !selected) {
        p.capture.video = DeviceRef{d.name, d.id, ""};
        p.capture.format = FormatSel{};  // format belongs to the old card
        // Und die Videonorm ebenso, aus genau demselben Grund. Auf dieser Karte
        // sind der analoge und der digitale Eingang zwei getrennte Geraete, das
        // Umstecken ist also ein Geraetewechsel -- und eine mitgeschleppte
        // PAL-Einstellung nagelt den digitalen Eingang auf 720x576 bei 50 Hz
        // fest, obwohl sie dort ueberhaupt nichts beschreibt.
        p.capture.videoStandard = -1;
        p.capture.crossbarInput = -1;
        embeddedAudioForDevice_.clear();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Aktualisieren", "Refresh"), ImVec2(-1, 0))) InvalidateDeviceLists();

  // Hier ist Schluss, solange nichts ausgewählt ist. Alles Weitere -- Norm,
  // Eingang, Ton, Format -- beschreibt eine Karte, und es gibt noch keine.
  // Sobald eine läuft, steht ohnehin fest, was sie kann, und der Rest erscheint
  // passend dazu.
  if (p.capture.video.empty()) {
    ImGui::Spacing();
    TextDisabledWrapped(
        T("Wähle oben eine Capture-Karte. Alles Weitere richtet sich danach, was sie "
          "liefert.",
          "Pick a capture card above. Everything else follows from what it delivers."));
    return;
  }

  // The driver's own dialog. Everything in it belongs to the card -- video
  // standard, decoder settings, whatever the vendor put there -- and none of it
  // has an equivalent here.
  ImGui::BeginDisabled(!captureRunning_);
  if (ImGui::Button(T("Karte konfigurieren ...", "Configure card ..."))) {
    deviceConfigRequested_ = true;
  }
  ImGui::EndDisabled();
  WrappedTooltip(
      captureRunning_
          ? T("Eigener Dialog des Treibers. Das Bild läuft dabei weiter.",
              "The driver's own dialog. The picture keeps running while it is open.")
          : T("Nur bei laufender Karte.", "Only while the card is running."));

  // Mehr als ein Neustart des Graphen, und deshalb ein eigener Knopf: die Karte
  // wird vollstaendig losgelassen und alles, was fuer den vorherigen Eingang
  // gemessen oder gewaehlt wurde, wird verworfen.
  ImGui::SameLine();
  if (ImGui::Button(T("Karte neu einlesen", "Reinitialise card"))) {
    cardResetRequested_ = true;
  }
  WrappedTooltip(
      T("Gibt die Karte ganz frei, sucht sie neu und beginnt von vorn.\n\n"
        "Videonorm und Format gehen dabei auf automatisch zurück, denn beide "
        "gehören zu dem Eingang, der vorher angeschlossen war. Wer zwischen analog "
        "und digital umsteckt, hängt sonst an einer PAL-Einstellung fest, die für "
        "HDMI nichts bedeutet und die Karte auf 720x576 bei 50 Hz festnagelt.\n\n"
        "Gerät und Eingang bleiben, wie sie eingestellt sind -- das ist eine "
        "Entscheidung, keine Messung.",
        "Releases the card completely, finds it again and starts over.\n\n"
        "The video standard and the format go back to automatic, because both "
        "belong to whichever input was plugged in before. Move between analogue "
        "and digital and you are otherwise stuck on a PAL setting that means "
        "nothing over HDMI and pins the card to 720x576 at 50 Hz.\n\n"
        "The device and the input stay as they are -- those are decisions, not "
        "measurements."));

  if (!caps.error.empty()) {
    ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", caps.error.c_str());
  }

  // ---- analogue video standard ----
  //
  // Only shown for cards that have an analogue decoder. On a pure HDMI input
  // there is no such thing and the list would be a row of dead entries.
  if (caps.availableStandards != 0 && analogueSource_) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Videonorm", "Video standard"));

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##vstandard",
                          VideoStandardPickerName(p.capture.videoStandard).c_str())) {
      if (ImGui::Selectable(T("Nicht ändern", "Leave alone"), p.capture.videoStandard == 0)) {
        p.capture.videoStandard = 0;
      }
      if (ImGui::Selectable(T("Automatisch", "Automatic"), p.capture.videoStandard == -1)) {
        p.capture.videoStandard = -1;
      }
      ImGui::Separator();
      const int chosen = VideoStandardGroupOf(p.capture.videoStandard);
      for (int i = 0; i < VideoStandardGroupCount(); ++i) {
        const long value = VideoStandardGroupPick(i, caps.availableStandards);
        if (value == 0) continue;
        if (ImGui::Selectable(VideoStandardGroupName(i), chosen == i)) {
          p.capture.videoStandard = value;
        }
        WrappedTooltip(VideoStandardGroupHint(i));
      }
      ImGui::EndCombo();
    }
    WrappedTooltip(
        T("Zusammengefasst zu den Normen, die am Kabel wirklich verschieden aussehen -- "
          "die Buchstaben hinter PAL und SECAM betreffen nur die Übertragung im "
          "Fernsehkanal. Automatisch probiert sie durch, bis der Decoder einrastet.",
          "Grouped by what actually looks different over a cable -- the letters behind PAL "
          "and SECAM only concern broadcasting in a television channel. Automatic tries "
          "them until the decoder locks."));

    // Nur bei "Automatisch": die Region ordnet die Suche, und wo nicht gesucht
    // wird, ordnet sie nichts. Ein Regler, der gerade nichts tut, ist ein
    // Regler, an dem jemand dreht und sich wundert.
    if (p.capture.videoStandard == -1) {
      int region = (int)cfg().app.videoRegion;
      ImGui::SetNextItemWidth(-1.0f);
      if (ComboEnum("##videoregion", &region, kVideoRegionCount, VideoRegionName)) {
        cfg().app.videoRegion = (VideoRegion)region;
      }
      ImGui::SameLine();
      HelpMarker(
          T("In welchem Land du wohnst. Die Karte meldet nur, ob sie die Zeilenfrequenz "
            "gefunden hat — PAL 60 und NTSC M haben beide 525 Zeilen bei 60 Hz und "
            "unterscheiden sich allein in der Farbe, die diese Meldung gar nicht misst. "
            "Wer zuerst probiert wird, gewinnt also, und diese Angabe bestimmt, wer das "
            "ist. Gefunden werden am Ende trotzdem alle Normen, nur langsamer.",
            "Where you live. The card only reports whether it found the line frequency — "
            "PAL 60 and NTSC M both have 525 lines at 60 Hz and differ only in colour, "
            "which that report does not measure. Whichever is tried first wins, and this "
            "setting decides which that is. Every standard is still found in the end, "
            "just more slowly."));
      // Was "Automatisch" hier gerade heisst. Sonst ist die haeufigste
      // Einstellung die einzige, die nichts ueber sich sagt.
      if (cfg().app.videoRegion == VideoRegion::Auto) {
        ImGui::TextDisabled(T("Laut Windows: %s", "According to Windows: %s"),
                            VideoRegionName((int)ResolveVideoRegion(VideoRegion::Auto)));
      }
    }

    // What the card is actually set to. On automatic this is the only way to see
    // what the search settled on, and "it looks right" is not the same as
    // knowing.
    //
    // "Eingestellt" ist dabei erst dann das richtige Wort, wenn die Norm auch
    // gilt. Waehrend der Suche steht auf der Karte eine Vermutung, die im
    // naechsten Moment verworfen wird -- sie so zu nennen, als waere sie eine
    // Entscheidung, ist die Auskunft, die vorher hier stand und die niemandem
    // half. Also sagt die Zeile jetzt, was gerade passiert, und nennt dabei
    // trotzdem die Norm: an ihr sieht man, dass die Suche laeuft und wie weit.
    const long shown = liveStandard_ != 0 ? liveStandard_ : caps.currentStandard;
    if (shown != 0) {
      const int idx = VideoStandardIndexOf(shown);
      const char* name = idx >= 0 ? VideoStandardName(idx) : "?";
      switch (standardSearch_) {
        case StandardSearch::Trying:
          ImGui::TextDisabled(T("Suche läuft: %s …", "Scanning: %s …"), name);
          break;
        case StandardSearch::Paused:
          // Nach einer erfolglosen Runde wird die Karte auf die beste Vermutung
          // gestellt und es wird gewartet. Das ist etwas anderes als Suchen, und
          // wer zusieht, soll nicht auf einen Fortschritt warten, den es gerade
          // nicht gibt.
          ImGui::TextDisabled(T("Suche pausiert, Karte auf %s", "Scanning paused, card set to %s"),
                              name);
          break;
        case StandardSearch::Colour:
          ImGui::TextDisabled(T("Farbe wird geprüft: %s", "Checking colour: %s"), name);
          break;
        case StandardSearch::Off:
          ImGui::TextDisabled(T("Eingestellt: %s", "In use: %s"), name);
          break;
      }
    }

    if (signalLocked_ >= 0) {
      if (signalLocked_ == 1) {
        ImGui::TextDisabled(T("Signal: eingerastet", "Signal: locked"));
      } else {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s",
                           T("Signal: kein Lock — falsche Norm oder nichts angeschlossen",
                             "Signal: no lock — wrong standard, or nothing connected"));
      }
    }
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
        // The rate goes back to "highest available" rather than to a number
        // carried over from a format that may not offer it. That entry leads
        // the list, so taking the first unforced one lands on it.
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
    bool namedSection = false;
    for (const FpsOption& f : fpsOptions) {
      if (!f.highest && !namedSection) {
        namedSection = true;
        ImGui::SeparatorText(T("Gemeldet", "Reported"));
      }
      if (f.forced && !forcedSection) {
        forcedSection = true;
        ImGui::SeparatorText(T("Für diese Auflösung nicht gemeldet",
                               "Not reported for this resolution"));
      }
      const bool selected =
          f.highest ? (fmt.fps <= 0.0) : (fmt.fps > 0.0 && std::fabs(f.fps - fmt.fps) < 0.05);
      if (ImGui::Selectable(FpsLabel(f.fps).c_str(), selected) && !selected) fmt.fps = f.fps;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  HelpMarker(T("\"Höchste verfügbare\" nimmt beim Öffnen der Karte deren Maximum und bleibt "
               "richtig, wenn die Quelle den Modus wechselt. Darunter steht, was der Treiber "
               "meldet. Etwas anderes erzwingen: unten von Hand eingeben.",
               "\"Highest available\" takes the card's maximum when it is opened and stays "
               "right when the source changes mode. Below it is what the driver reports. "
               "To force something else, enter it by hand below."));
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
      // The ends are the longest edge D3D11 can address and a rate no display
      // hardware reaches, not a judgement about what is sensible. This is the
      // box for forcing something the driver never mentioned; the point of it
      // is that CapView does not argue.
      fmt.width = Clamp(customWidth_, 16, 16384);
      fmt.height = Clamp(customHeight_, 16, 16384);
      fmt.fps = Clamp(customFps_, 1.0, 1000.0);
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

// The demodulation window the shader will use for this slider position. Kept
// in step with kCleanPS by hand -- there is no way to ask a compiled shader what
// it decided, and a readout that quietly drifts from the truth is worse than no
// readout at all.
int DotCrawlWindow(float slider, float carrierPeriod) {
  const float cycles = 8.2f - slider * 5.9f;
  int r = (int)std::floor(cycles * carrierPeriod * 0.5f + 0.5f);
  r = r < 2 ? 2 : (r > 12 ? 12 : r);
  return 2 * r + 1;
}

// The slider positions that actually do something different.
//
// The shader rounds the window to a whole number of samples and clamps it, so
// between two positions that land on the same one nothing happens at all: the
// control moves and the picture does not. How many distinct stops there are is
// not fixed either -- it falls out of the carrier period, which comes from the
// video standard and the capture width, so NTSC and PAL do not have the same
// ones.
//
// Rather than invert the shader's arithmetic and risk drifting from it, the
// range is simply walked and the distinct windows collected. One canonical
// position per window, in the middle of the span that produces it, so a stop is
// as far as possible from the rounding boundaries on either side.
struct DotCrawlStep {
  float slider;
  int window;
};

int BuildDotCrawlSteps(float carrierPeriod, DotCrawlStep* out, int max) {
  int count = 0;
  int lastWindow = -1;
  float spanStart = 0.0f;
  const int kProbe = 400;
  for (int i = 0; i <= kProbe; ++i) {
    const float s = (float)i / (float)kProbe;
    const int w = DotCrawlWindow(s, carrierPeriod);
    if (w != lastWindow) {
      if (lastWindow >= 0 && count < max) {
        out[count].slider = (spanStart + (float)(i - 1) / (float)kProbe) * 0.5f;
        out[count].window = lastWindow;
        ++count;
      }
      lastWindow = w;
      spanStart = s;
    }
  }
  if (lastWindow >= 0 && count < max) {
    out[count].slider = (spanStart + 1.0f) * 0.5f;
    out[count].window = lastWindow;
    ++count;
  }
  return count;
}

// What that window does, from the measurements on the capture card rather than
// from theory: how much of the pattern goes, and what it costs in horizontal
// sharpness. Between the measured points it is a straight line, which is honest
// enough for a readout meant to give a sense of the trade.
void DotCrawlEffect(int window, float* removed, float* softer) {
  struct Point {
    int window;
    float removed;
    float softer;
  };
  static const Point kMeasured[] = {
      {25, 42.0f, 7.0f}, {15, 61.0f, 11.0f}, {11, 74.0f, 14.0f}, {9, 81.0f, 17.0f},
  };
  const int count = (int)(sizeof(kMeasured) / sizeof(kMeasured[0]));
  if (window >= kMeasured[0].window) {
    *removed = kMeasured[0].removed;
    *softer = kMeasured[0].softer;
    return;
  }
  for (int i = 1; i < count; ++i) {
    if (window >= kMeasured[i].window) {
      const Point& a = kMeasured[i - 1];
      const Point& b = kMeasured[i];
      const float t = (float)(a.window - window) / (float)(a.window - b.window);
      *removed = a.removed + (b.removed - a.removed) * t;
      *softer = a.softer + (b.softer - a.softer) * t;
      return;
    }
  }
  *removed = kMeasured[count - 1].removed;
  *softer = kMeasured[count - 1].softer;
}

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

  // Die vier Regler, die sonst jedes Aufnahmeprogramm hat -- hier aber im
  // Shader statt auf der Karte. Die Regler der Karte werden beim Start
  // neutralisiert, damit das, was ankommt, das ist, was die Konsole geschickt
  // hat; siehe NeutraliseProcAmp.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildregler", "Picture controls"));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Helligkeit", "Brightness"), &img.brightness, -1.0f, 1.0f, "%+.2f");
  ImGui::SameLine();
  HelpMarker(T("Hebt oder senkt das ganze Bild. 0 ist neutral.",
               "Lifts or lowers the whole picture. 0 is neutral."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Kontrast", "Contrast"), &img.contrast, 0.0f, 2.0f, "%.2f");
  ImGui::SameLine();
  HelpMarker(T("Spreizt um das mittlere Grau. 1 ist neutral.",
               "Spreads around mid grey. 1 is neutral."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Sättigung", "Saturation"), &img.saturation, 0.0f, 2.0f, "%.2f");
  ImGui::SameLine();
  HelpMarker(T("0 macht das Bild grau, 1 ist neutral, 2 doppelt so bunt.",
               "0 makes the picture grey, 1 is neutral, 2 twice as colourful."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Farbton", "Hue"), &img.hue, -180.0f, 180.0f, "%+.0f°");
  ImGui::SameLine();
  HelpMarker(
      T("Dreht alle Farben um denselben Winkel. 0 ist neutral. Bei NTSC über "
        "Composite das, was am Fernseher der Farbton-Regler war.",
        "Turns every colour by the same angle. 0 is neutral. On NTSC over composite this "
        "is what the tint knob on a television did."));

  const bool neutral = img.brightness == 0.0f && img.contrast == 1.0f &&
                       img.saturation == 1.0f && img.hue == 0.0f;
  ImGui::BeginDisabled(neutral);
  if (ImGui::Button(T("Zurücksetzen", "Reset"))) {
    img.brightness = 0.0f;
    img.contrast = 1.0f;
    img.saturation = 1.0f;
    img.hue = 0.0f;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  // Eigene ID: derselbe Satz steht auf dieser Seite ein zweites Mal, bei den
  // quadratischen Pixeln. ImGui bildet die ID aus der Beschriftung, also waeren
  // es sonst zwei Bedienelemente mit einer ID -- und ImGui sagt das auch, mit
  // einem Fenster mitten im Bild. Umbenennen waere die schlechtere Antwort: der
  // Satz sagt an beiden Stellen dasselbe und soll auch gleich lauten.
  ImGui::PushID("procamp-to-output");
  ImGui::Checkbox(T("Auch für Aufnahme, Screenshot und Kamera",
                    "Apply to recording, screenshots and camera"),
                  &img.procAmpToOutput);
  ImGui::PopID();
  ImGui::SameLine();
  HelpMarker(
      T("Aus ist Absicht, und die beiden Stellungen sind nicht gleichwertig. Sauber "
        "aufnehmen und später nachbearbeiten kostet nichts -- dieselbe Korrektur lässt "
        "sich im Schnittprogramm jederzeit drauflegen. Mit angehobenem Kontrast "
        "aufnehmen klemmt dagegen die Lichter auf Vollausschlag und drückt die Tiefen "
        "auf null, und das holt kein Nachbearbeiten zurück.\n\nAn ist richtig, wenn die "
        "Aufnahme so hochgeladen wird, wie sie herauskommt.\n\nDie Anzeige bekommt die "
        "Regler in jedem Fall.",
        "Off on purpose, and the two positions are not equivalent. Recording clean and "
        "grading later costs nothing -- the same correction goes on in the editor at any "
        "time. Recording with contrast raised clips the highlights to full scale and "
        "crushes the shadows to zero, and no amount of editing brings those back.\n\nOn "
        "is right when the recording gets uploaded exactly as it comes out.\n\nThe "
        "display gets the controls either way."));

  // Natives Raster. Gehört zur Skalierung, nicht zur Bildröhre: es macht das
  // Bild sauberer, nicht nostalgischer.
  //
  // Und es ist rein analog. Es rechnet zurück, was das Abtasten einer analogen
  // Zeile mit fester Rate angerichtet hat -- ein digitaler Eingang überträgt
  // die Bildpunkte bereits einzeln, da gibt es kein Raster wiederzufinden.
  if (analogueSource_) {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Natives Pixelraster", "Native pixel grid"));

  // Die Konsole steht in der Liste selbst, nicht nur im Tooltip: wer hier
  // vorbeikommt, weiß was er angeschlossen hat und sucht die Zahl dazu -- nicht
  // umgekehrt.
  static const int kNativePresets[] = {0, 256, 320, 384, 512, 640};
  const char* kNativeWho[] = {
      T("aus", "off"),
      "256  ·  SNES, NES, PS1",
      T("320  ·  Mega Drive, PS1", "320  ·  Mega Drive, PS1"),
      T("384  ·  Amiga, PS1 breit", "384  ·  Amiga, PS1 wide"),
      T("512  ·  SNES hochauflösend", "512  ·  SNES hi-res"),
      T("640  ·  GameCube, PS2, Dreamcast", "640  ·  GameCube, PS2, Dreamcast"),
  };
  int nativeIdx = 0;
  for (int k = 0; k < 6; ++k) {
    if (img.nativeWidth == kNativePresets[k]) nativeIdx = k;
  }
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Breite der Quelle", "Source width"), kNativeWho[nativeIdx])) {
    for (int k = 0; k < 6; ++k) {
      const bool chosen = nativeIdx == k;
      if (ImGui::Selectable(kNativeWho[k], chosen)) img.nativeWidth = kNativePresets[k];
      if (chosen) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  HelpMarker(T("Wie viele Pixel die Konsole waagerecht wirklich zeichnet. Die Karte tastet "
               "mit fester Rate ab, meist 720 -- ein SNES-Pixel landet damit auf 2,8 "
               "Proben. Ist die Zahl bekannt, wird jedes Ausgabepixel dem richtigen "
               "Konsolenpixel zugeordnet statt irgendwo dazwischen.\n\n"
               "256 SNES, NES, PS1 niedrig · 320 Mega Drive, PS1 · 512 SNES hochauflösend · "
               "640 GameCube, PS2 in 480p\n\n"
               "Ersetzt den Filter oben, weil die Zuordnung selbst schon die Entscheidung "
               "ist. Am besten mit Integer-Skalierung.",
               "How many pixels the console really draws across. The card samples at a "
               "fixed rate, usually 720 -- so one SNES pixel lands on about 2.8 samples. "
               "Given the real number, every output pixel resolves to the console's pixel "
               "instead of somewhere between two of them.\n\n"
               "256 SNES, NES, PS1 low · 320 Mega Drive, PS1 · 512 SNES hi-res · "
               "640 GameCube, PS2 at 480p\n\n"
               "Replaces the filter above, because the mapping is the decision. Best with "
               "integer scaling."));

  }  // natives Pixelraster, nur analog

  // Bildröhre. Steht am Ende der Skalierungsgruppe, weil es dorthin gehört --
  // es sind Anzeigeeffekte und keine Signalbearbeitung, und sie landen weder in
  // einer Aufnahme noch in einem Screenshot.
  //
  // Gezeigt wird das nach der Zeilenzahl der Quelle, nicht danach, ob sie
  // analog ist. Der Grund ist ein Geraet, das es wirklich gibt: ein RetroTINK
  // oder ein MiSTer haengt mit 480p an HDMI, ist also digital, und genau dessen
  // Besitzer will Zeilenluecken. Umgekehrt greifen sie bei 720p und darueber
  // ohnehin nicht -- der Shader schaltet unterhalb der doppelten Quellhoehe ab,
  // weil dort keine Luecke mehr hinpasst.
  if (sourceHeight_ == 0 || sourceHeight_ <= 576) {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Bildröhre", "Cathode ray tube"));
  ImGui::TextDisabled(
      "%s", T("Setzt zurück, was ein Röhrenmonitor hinzugefügt hat. Nur für die Anzeige.",
              "Puts back what a CRT added. Display only."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Zeilenlücken", "Scanlines"), &img.scanlines, 0.0f, 0.5f, "%.2f");
  ImGui::SameLine();
  HelpMarker(T("Dunkelt die Lücken zwischen den Zeilen der Quelle ab. Braucht mindestens "
               "doppelte Höhe im Fenster, darunter bleibt es aus -- sonst gäbe es Moiré "
               "statt Zeilen. Die Helligkeit wird ausgeglichen.",
               "Darkens the gaps between the source's own lines. Needs at least twice the "
               "height in the window and stays off below that, because there is nowhere to "
               "put a gap otherwise. Brightness is compensated."));

  int mask = Clamp(img.mask, 0, 2);
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Maske", "Mask"), &mask, 3, MaskName, MaskHelp)) img.mask = mask;
  ImGui::SameLine();
  HelpMarker(MaskHelp(mask));

  if (img.mask != 0) {
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderFloat(T("Maskenstärke", "Mask strength"), &img.maskStrength, 0.0f, 0.5f, "%.2f");
    ImGui::SameLine();
    HelpMarker(T("Braucht eine hohe Ausgabeauflösung, um als Maske statt als Farbstich zu "
                 "wirken.",
                 "Needs a high output resolution to read as a mask rather than as a tint."));
  }
  }  // Bildröhre, nur bei standardaufloesenden Quellen
  ImGui::Spacing();

  int aspect = (int)img.aspect;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Seitenverhältnis", "Aspect ratio"), &aspect, 5, AspectName, AspectHelp)) {
    img.aspect = (AspectMode)aspect;
  }
  ImGui::SameLine();
  HelpMarker(AspectHelp(aspect));

  // Bei Strecken und Ganzzahlig gibt es nichts umzurechnen: das eine hat
  // ueberhaupt keine eigene Form, das andere ist ein Versprechen ueber Pixel,
  // das genau durch Umrechnen gebrochen wuerde.
  if (img.aspect != AspectMode::Stretch && img.aspect != AspectMode::Integer) {
    // Eigene ID, siehe oben beim gleichlautenden Kaestchen der Bildregler.
    ImGui::PushID("square-pixel-to-output");
    ImGui::Checkbox(T("Auch für Aufnahme, Screenshot und Kamera",
                      "Apply to recording, screenshots and camera"),
                    &img.squarePixelOutput);
    ImGui::PopID();
    ImGui::SameLine();
    HelpMarker(
        T("Das Fenster kann nicht-quadratische Pixel umsonst zeigen -- es zeichnet "
          "einfach in ein Rechteck der richtigen Form. Eine Datei kann das nicht, sie "
          "ist ein Raster. Also wird das Bild beim Hinausgehen mit dem oben gewählten "
          "Skalierungsfilter auf quadratische Pixel gerechnet, und aus 720x576 wird "
          "768x576.\n\nÜber HDMI ändert das gar nichts: dort sind die Pixel schon "
          "quadratisch, die Größen kommen gleich heraus und der Durchgang entfällt.",
          "The window can show non-square pixels for free -- it simply draws into a "
          "rectangle of the right shape. A file cannot; it is a grid. So the picture is "
          "resampled to square pixels on its way out, using the scaling filter chosen "
          "above, and 720x576 leaves as 768x576.\n\nOver HDMI this changes nothing: "
          "those pixels are already square, the sizes come out equal and the pass is "
          "skipped."));
  }

  int rotation = (int)img.rotation;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Drehung", "Rotation"), &rotation, kRotationCount, RotationName)) {
    img.rotation = (Rotation)rotation;
  }
  ImGui::SameLine();
  HelpMarker(T("Gilt auch für Aufnahme und Screenshots.",
               "Applies to recordings and screenshots as well."));

  // Nur wenn die Quelle wirklich halb so hoch ankommt.
  //
  // Frueher stand hier bloss `analogueSource_`, und damit war das Kaestchen an
  // jedem Composite-Eingang anklickbar -- auch an einem GameCube mit 576i.
  // Sichtbar passiert dort nichts, denn das Bild wird ohnehin auf das
  // eingestellte Seitenverhaeltnis gepasst und nicht auf die Zeilenzahl; die
  // Aufnahme dagegen kam mit 720x1152 heraus. Eine Einstellung, die dort nichts
  // tut, wo man hinsieht, und etwas dort, wo man nicht hinsieht, ist die
  // schlechteste Sorte.
  if (analogueSource_ && sourceHeight_ > 0 && sourceHeight_ <= kHalfHeightLines) {
    ImGui::Checkbox(T("Zeilen verdoppeln", "Double lines"), &img.lineDouble);
    ImGui::SameLine();
    HelpMarker(T("Für 240p/288p-Quellen, die halb so hoch ankommen wie sie sollen.",
                 "For 240p/288p sources that arrive half as tall as they should."));
  }

  // Halbbilder. Nicht danach gezeigt, ob die Quelle analog ist, sondern danach,
  // ob sie ueberhaupt Halbbilder hat -- 1080i und 480i gibt es auch ueber HDMI,
  // und wer so etwas anschliesst, braucht diese Regler genauso.
  //
  // Solange noch nichts gemessen ist, wird gezeigt. Etwas zu verstecken, bevor
  // man weiss, ob es gebraucht wird, ist der schlechtere Fehler von beiden.
  if (analogueSource_ || sourceInterlaced_ || sourceHeight_ == 0) {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Halbbilder", "Fields"));
  ImGui::Checkbox(T("Nur bei interlaced Quellen anwenden", "Only apply to interlaced sources"),
                  &img.deinterlaceAuto);
  ImGui::SameLine();
  HelpMarker(T("Erkennung aus dem Bild, nicht aus der Formatmeldung der Karte.",
               "Detected from the picture, not from what the card reports."));
  // Shown regardless of the checkbox. What the source is remains worth knowing
  // when the automatic handling is switched off -- that is exactly the situation
  // in which somebody wants to check whether it was right.
  if (detectedInterlace_) {
    ImGui::TextDisabled(T("Gemessen: %s", "Measured: %s"),
                        *detectedInterlace_ ? *detectedInterlace_
                                            : T("wird gemessen", "measuring"));
  }
  // Direkt unter der Messung und direkt ueber dem Schalter, mit dem man sie
  // uebergeht: das ist die Reihenfolge, in der jemand die Sache liest und
  // erledigt. Warum die Messung hier irren kann, steht bei
  // App::InterlaceVerdictDoubtful -- kurz: ein Bild von Kammlinien ist von
  // Kammlinien nicht zu unterscheiden.
  if (interlaceDoubtful_) {
    TextWarningWrapped(
        T("Bei dieser Auflösung ist das ungewöhnlich. Zeigt die Quelle gerade ein Bild "
          "oder Video mit feinen Zeilenmustern, kann die Erkennung darauf hereinfallen. "
          "Sieht das Bild richtig aus, ignoriere das; sonst stelle Deinterlacing hier "
          "von Hand ab.",
          "Unusual at this resolution. If the source is showing a picture or video with "
          "fine line patterns, the detection can be fooled by it. If the picture looks "
          "right, ignore this; otherwise turn deinterlacing off by hand here."));
  }

  int deint = (int)img.deinterlace;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Deinterlacing", "Deinterlacing"), &deint, kDeinterlaceCount, DeinterlaceName,
                DeinterlaceHelp)) {
    img.deinterlace = (Deinterlace)deint;
  }
  ImGui::SameLine();
  HelpMarker(DeinterlaceHelp(deint));
  if (coSitedFields_) {
    TextDisabledWrapped(T("Diese Quelle braucht keine Rekonstruktion: alle Modi liefern "
                          "dasselbe Bild.",
                          "This source needs no reconstruction: every mode gives the same "
                          "picture."));
  }

  int order = (int)img.fieldOrder;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Halbbildreihenfolge", "Field order"), &order, 3, FieldOrderName)) {
    img.fieldOrder = (FieldOrder)order;
  }
  ImGui::SameLine();
  HelpMarker(T("Springt das Bild auf und ab, die andere Reihenfolge wählen.",
               "If the picture jumps up and down, pick the other order."));
  }  // Halbbilder, nur wenn die Quelle welche hat

  if (analogueSource_) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Composite-Filter", "Composite filter"));
    ImGui::TextDisabled(T("Gegen die zwei Störungen, die Composite immer mitbringt.",
                          "Against the two defects composite always brings with it."));

    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderInt(T("Farbschimmern", "Colour shimmer"), &img.chromaSoft, 0, 8,
                     img.chromaSoft == 0 ? T("aus", "off") : "%d");
    ImGui::SameLine();
    HelpMarker(T("Regenbogenmuster über feinen Strukturen. Weichzeichnet die Farbe "
                 "seitlich; die Schärfe bleibt, weil Composite ohnehin keine feinen "
                 "Farbdetails überträgt.",
                 "Rainbow patterns over fine detail. Blurs colour sideways; sharpness "
                 "stays, because composite carries no fine colour detail anyway."));

    // Two controls, because they are two different bargains and pretending
    // otherwise hid the more useful one.
    //
    // Averaging over four frames removes the pattern completely wherever the
    // picture is standing still, and costs nothing at all. Working the carrier
    // back out of the brightness is the only thing that helps where something is
    // moving, and it costs sharpness. Behind one slider the first was switched
    // on by the very first step and everything after it was the second getting
    // gradually softer -- which is exactly what it felt like to use, and gave no
    // way to have the free half on its own.
    //
    // They are not independent, though, which is why the checkbox locks: with
    // the averaging off, the demodulator runs over still parts of the picture
    // too, paying sharpness for something the averaging does for free.
    const bool demodOn = img.dotNotch > 0.0f;
    if (demodOn) img.temporalDenoise = 1.0f;

    bool average = img.temporalDenoise > 0.0f;
    ImGui::BeginDisabled(demodOn);
    if (ImGui::Checkbox(T("Stillstehendes mitteln", "Average what stands still"), &average)) {
      img.temporalDenoise = average ? 1.0f : 0.0f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpMarker(T("Mittelt über vier Bilder und entfernt das Punktkriechen dort vollständig, "
                 "wo sich nichts bewegt -- ohne einen Deut Schärfe zu kosten. Vier, weil der "
                 "Farbträger eine Folge über vier Bilder durchläuft: bei zwei bliebe alles "
                 "stehen.\n\n"
                 "Fest angehakt, sobald der Regler darunter über null steht: ohne die "
                 "Mittelung rechnet der Demodulator auch über ruhende Bildteile und "
                 "bezahlt dort Schärfe für etwas, das hier umsonst zu haben ist.",
                 "Averages over four frames and removes the crawl entirely wherever nothing "
                 "is moving, at no cost in sharpness at all. Four, because the colour "
                 "subcarrier walks through a four frame sequence; two would cancel "
                 "nothing.\n\n"
                 "Held on whenever the slider below is above zero: without the averaging "
                 "the demodulator works over the still parts of the picture as well, and "
                 "pays sharpness there for something that is free here."));

    // Derselbe Filter an einem anderen Arbeitspunkt, nicht ein zweiter Filter.
    // Mitteln ueber Bewegung ist Schmieren -- das laesst sich nicht wegrechnen,
    // nur verschieben, und wohin es verschoben wird, weiss die Quelle besser
    // als der Code.
    ImGui::BeginDisabled(!average);
    ImGui::Indent();
    bool quick = img.avoidGhosting;
    if (ImGui::Checkbox(T("Ghosting vermeiden", "Avoid ghosting"), &quick)) {
      img.avoidGhosting = quick;
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpMarker(T("Wo die Mittelung bei Bewegung wieder loslaesst. Sie kann nur mitteln, und "
                 "Mitteln über Bewegung ist Schmieren -- der Haken verschiebt also nur, wo "
                 "der Tausch stattfindet, weg ist er nie.\n\n"
                 "Aus: das Gatter hält lange fest. Punktkriechen bewegt sich selbst, und ein "
                 "empfindliches Gatter schaltet den Filter genau dort ab, wo das Artefakt "
                 "sitzt -- dafür wird eine langsam wandernde Kante mit drei älteren Kopien "
                 "ihrer selbst gemittelt, und das ist die Fahne dahinter.\n\n"
                 "An: das Gatter lässt nach vier von 255 Stufen los. Bewegte Kanten bleiben "
                 "sauber, an langsamen Stellen bleibt etwas Kriechen stehen -- dort nimmt "
                 "der Regler darunter die Arbeit wieder auf.",
                 "Where the averaging lets go of moving parts. It can only average, and "
                 "averaging across movement is smearing -- so this only moves where the "
                 "trade happens, it never removes it.\n\n"
                 "Off: the gate holds on late. Dot crawl crawls, so a sensitive gate "
                 "switches the filter off exactly where the artefact is -- the price is "
                 "that a slowly moving edge gets averaged with three older copies of "
                 "itself, which is the trail behind it.\n\n"
                 "On: the gate lets go within four levels out of 255. Moving edges stay "
                 "clean, slow parts keep some crawl -- and there the slider below picks "
                 "the work back up."));

    // Stufen statt freiem Lauf: die Zwischenwerte sind ohne Wirkung, und ein
    // Regler, der sich bewegt ohne etwas zu ändern, behauptet etwas Falsches.
    DotCrawlStep steps[16];
    const int stepCount = BuildDotCrawlSteps(carrierPeriod_, steps, 16);
    int stepIndex = 0;  // 0 heißt aus
    if (img.dotNotch > 0.0f) {
      const int nowWindow = DotCrawlWindow(img.dotNotch, carrierPeriod_);
      stepIndex = 1;
      for (int k = 0; k < stepCount; ++k) {
        if (steps[k].window == nowWindow) stepIndex = k + 1;
      }
    }

    char label[64];
    if (stepIndex == 0) {
      snprintf(label, sizeof(label), "%s", T("aus", "off"));
    } else {
      snprintf(label, sizeof(label), T("Stufe %d von %d", "step %d of %d"), stepIndex, stepCount);
    }

    ImGui::SetNextItemWidth(-260.0f);
    if (ImGui::SliderInt(T("Bewegtes entstören", "Clean up what moves"), &stepIndex, 0, stepCount,
                         label)) {
      img.dotNotch = stepIndex <= 0 ? 0.0f : steps[stepIndex - 1].slider;
    }
    ImGui::SameLine();
    HelpMarker(T("Rechnet den Farbträger aus der Helligkeit heraus -- das Einzige, was gegen "
                 "Punktkriechen an bewegten Stellen hilft, und es kostet Schärfe. Weiter "
                 "rechts heißt gründlicher und weicher; die Zeile darunter sagt, wo man "
                 "gerade steht.\n\n"
                 "Braucht die richtige Trägerfrequenz, und die kommt aus der Videonorm im "
                 "Reiter Quelle. Steht die falsch, sinkt die Wirkung von rund 70 auf 34 "
                 "Prozent.",
                 "Works the colour subcarrier back out of the brightness -- the only thing "
                 "that helps against crawl on moving parts, and it costs sharpness. Further "
                 "right is more thorough and softer; the line below says where you are.\n\n"
                 "It needs the right carrier frequency, and that comes from the video "
                 "standard on the Source tab. Set wrong, it drops from about 70 % to 34 %."));

    // What the number amounts to, because a number on its own says nothing and
    // this is a trade the user should be able to see rather than infer.
    if (demodOn) {
      const int window = DotCrawlWindow(img.dotNotch, carrierPeriod_);
      float removed = 0.0f, softer = 0.0f;
      DotCrawlEffect(window, &removed, &softer);
      ImGui::Indent();
      ImGui::TextDisabled(T("In Bewegung: %.0f %% weg, dafür %.0f %% weicher (Fenster %d Punkte).",
                            "Moving: %.0f %% gone, %.0f %% softer for it (window %d samples)."),
                          removed, softer, window);
      ImGui::Unindent();
    }
  }

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
  // The crop is measured on the source, before the picture is turned. Dragging
  // an edge on a rotated image would move a different edge than the one under
  // the cursor, so that combination is simply not offered.
  const bool rotated = img.rotation != Rotation::None;
  ImGui::BeginDisabled(rotated);
  if (ImGui::Button(T("Konfigurieren ...", "Configure ..."))) {
    cropPickRequested_ = true;
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(
      rotated ? T("Bei gedrehtem Bild nicht verfügbar.", "Not available on a rotated picture.")
              : T("Schließt die Einstellungen und lässt die Ränder im Bild ziehen.",
                  "Closes the settings and lets you drag the edges on the picture."));
  ImGui::SameLine();
  ImGui::BeginDisabled(!captureRunning_);
  if (ImGui::Button(T("Erkennen", "Detect"))) cropDetectRequested_ = true;
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(T("Misst den schwarzen Rand aus und schneidet ihn weg.",
                          "Measures the black border and crops it away."));
  ImGui::SameLine();
  if (ImGui::Button(T("Zurücksetzen##crop", "Reset##crop"))) {
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

  ImGui::Spacing();
  ImGui::SetNextItemWidth(-260.0f);
  LevelMeter(T("Eingangspegel", "Input level"), inputPeak_, true);
  ImGui::SameLine();
  HelpMarker(T("Vor Lautstärke und Stumm. Zeigt, was die Karte liefert.",
               "Before volume and mute. Shows what the card delivers."));

  // ---- microphone ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Mikrofon", "Microphone"));
  ImGui::TextWrapped(
      T("Kommt als eigene Spur in die Aufnahme und wird nicht mitgehört — sonst hättest du "
        "dich selbst im Kopfhörer.",
        "Recorded as its own track and never played back — otherwise you would hear yourself "
        "in your headphones."));
  ImGui::Spacing();

  ImGui::Checkbox(T("Mikrofon aufnehmen", "Record a microphone"), &audio.micEnabled);

  ImGui::BeginDisabled(!audio.micEnabled);
  const char* micPreview = audio.micDevice.name.empty()
                               ? T("Systemstandard", "System default")
                               : audio.micDevice.name.c_str();
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##micdev", micPreview)) {
    if (ImGui::Selectable(T("Systemstandard", "System default"), audio.micDevice.empty())) {
      audio.micDevice = DeviceRef{};
    }
    for (const AudioDeviceInfo& d : audioInputs_) {
      // DirectShow inputs are capture card audio, not microphones.
      if (d.directShow) continue;
      const bool selected = (d.id == audio.micDevice.id);
      if (ImGui::Selectable(d.name.c_str(), selected)) audio.micDevice = d.ToRef();
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::SetNextItemWidth(-260.0f);
  float micDb = 20.0f * std::log10(std::max(audio.micGain, 0.01f));
  if (ImGui::SliderFloat(T("Verstärkung", "Gain"), &micDb, -20.0f, 12.0f, "%+.1f dB")) {
    audio.micGain = std::pow(10.0f, micDb / 20.0f);
  }
  ImGui::SameLine();
  HelpMarker(T("Zusätzlich zur Windows-Einstellung, wirkt nur auf die Aufnahme.",
               "On top of the Windows setting, affects the recording only."));

  int trackMode = (int)audio.micTrackMode;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Spuren", "Tracks"), &trackMode, 3, MicTrackModeName)) {
    audio.micTrackMode = (MicTrackMode)trackMode;
  }
  ImGui::SameLine();
  HelpMarker(T("Gemischt spielt überall ab. Getrennt lässt sich im Schnitt noch auseinander "
               "nehmen.",
               "Mixed plays anywhere. Separate can still be pulled apart in an editor."));

  ImGui::SetNextItemWidth(-260.0f);
  LevelMeter(T("Mikrofonpegel", "Microphone level"), micPeak_, micRunning_);
  if (audio.micEnabled && !micRunning_) {
    ImGui::TextDisabled(T("Wird geöffnet ...", "Opening ..."));
  }
  ImGui::EndDisabled();
}

// --------------------------------------------------------------- display tab

void SettingsWindow::DrawHdrBlock() {
  AppSettings& app = cfg().app;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Hoher Kontrastumfang (HDR)", "High dynamic range"));

  // What is actually the case, before any of the choices below. Two independent
  // facts, and most confusion about HDR comes from mixing them up.
  const char* sourceText =
      hdrSourceTransfer_ == 1 ? T("Quelle: HDR (PQ)", "Source: HDR (PQ)")
      : hdrSourceTransfer_ == 2 ? T("Quelle: HDR (HLG)", "Source: HDR (HLG)")
                                : T("Quelle: SDR", "Source: SDR");
  ImGui::TextDisabled("%s", sourceText);
  ImGui::SameLine();
  ImGui::TextDisabled("   |   ");
  ImGui::SameLine();
  if (!hdrDisplayCapable_) {
    ImGui::TextDisabled("%s", T("Anzeige: kein HDR eingeschaltet",
                                "Display: HDR not switched on"));
  } else if (hdrOutputActive_) {
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                       T("Anzeige: HDR aktiv, %.0f nits", "Display: HDR active, %.0f nits"),
                       hdrDisplayPeak_);
  } else {
    ImGui::TextDisabled(T("Anzeige: HDR möglich, %.0f nits",
                          "Display: HDR available, %.0f nits"), hdrDisplayPeak_);
  }
  ImGui::Spacing();

  // Was die Quelle liefert -- und ein Analogdekoder liefert kein HDR. PQ und HLG
  // sind digitale Uebertragungskurven; über Composite, S-Video oder Component
  // kommt so etwas nicht an, und eine Kurve zu wählen, die es nicht gibt, kann
  // nur schaden.
  //
  // Was darunter steht, bleibt trotzdem stehen: das ist die Anzeige, nicht die
  // Quelle. Läuft der Desktop im HDR-Modus, muss auch ein SDR-Bild mit einem
  // Weißwert in den HDR-Behälter geschrieben werden, sonst kommt es zu dunkel
  // oder zu hell heraus. Das gilt für ein SNES genauso wie für alles andere.
  if (!analogueSource_) {
    const char* inputNames[] = {
        T("Automatisch", "Automatic"), T("SDR", "SDR"), T("HDR10 (PQ)", "HDR10 (PQ)"),
        T("HLG", "HLG")};
    int input = (int)app.hdrInput;
    ImGui::SetNextItemWidth(-260.0f);
    if (ImGui::Combo(T("Quellkurve", "Source curve"), &input, inputNames, kHdrInputCount)) {
      app.hdrInput = (HdrInput)input;
    }
    ImGui::SameLine();
    HelpMarker(T("Automatisch glaubt, was der Treiber in den Medientyp schreibt. Die "
                 "meisten Karten schreiben dort nichts -- dann bleibt es bei SDR und muss "
                 "hier von Hand gesetzt werden.",
                 "Automatic believes what the driver put in the media type. Most cards put "
                 "nothing there, in which case it stays on SDR and has to be set here by "
                 "hand."));
  } else {
    TextDisabledWrapped(
        T("Analoge Quelle: HDR gibt es dort nicht. Was bleibt, betrifft die Anzeige.",
          "Analogue source: there is no HDR there. What remains concerns the display."));
  }

  const char* outputNames[] = {T("Aus", "Off"), T("Automatisch", "Automatic"),
                               T("Immer wenn möglich", "Whenever possible")};
  int output = (int)app.hdrOutput;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("An die Anzeige", "To the display"), &output, outputNames,
                   kHdrOutputCount)) {
    app.hdrOutput = (HdrOutput)output;
  }
  ImGui::SameLine();
  HelpMarker(T("Automatisch schaltet nur um, wenn die Anzeige im HDR-Modus läuft UND die "
               "Quelle wirklich HDR ist. Sonst wird das Bild auf die Anzeige "
               "heruntergerechnet, was auf einem gewöhnlichen Monitor genau richtig ist.",
               "Automatic switches over only when the display is in HDR mode AND the source "
               "really is HDR. Otherwise the picture is mapped down to the display, which "
               "is exactly right on an ordinary monitor."));

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Papierweiß", "Paper white"), &app.paperWhiteNits, 80.0f, 400.0f,
                     "%.0f nits");
  ImGui::SameLine();
  HelpMarker(T("Wie hell gewöhnliches Weiß herauskommt -- ein Blatt Papier im Bild, nicht "
               "die hellste Stelle. 203 ist der Wert, gegen den HDR-Material üblicherweise "
               "abgemischt wird.",
               "How bright ordinary white comes out -- a sheet of paper in the picture, not "
               "the brightest spot. 203 is what HDR material is usually graded against."));

  // Auch das gehört der Quelle, also weg bei einer analogen: was ein SNES an
  // seiner hellsten Stelle in nits erreicht, ist keine sinnvolle Frage.
  if (analogueSource_) return;

  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderFloat(T("Spitze der Quelle", "Source peak"), &app.sourcePeakNits, 200.0f,
                     10000.0f, "%.0f nits", ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine();
  HelpMarker(T("Wie hell die Quelle an ihrer hellsten Stelle wird. DirectShow überträgt "
               "das nirgends, es lässt sich also nicht auslesen -- und es zählt: nimmt man "
               "10000 an, wo die Quelle nur 1000 erreicht, landet Papierweiß auf einem "
               "SDR-Monitor bei 63 statt 88 von 100 nits, das ganze Bild wird zu dunkel. "
               "1000 passt für Konsolen.",
               "How bright the source gets at its brightest. DirectShow carries this "
               "nowhere, so it cannot be read -- and it matters: assume 10000 where the "
               "source only reaches 1000 and paper white lands at 63 instead of 88 out of "
               "100 nits on an SDR monitor, darkening the whole picture. 1000 suits "
               "consoles."));

  ImGui::Spacing();
  ImGui::SeparatorText(T("Was den Umfang behält", "What keeps the range"));

  // Only meaningful when the source has a range to keep. Shown either way, so
  // it is clear the settings exist and why they are doing nothing.
  const bool sourceIsHdr = hdrSourceTransfer_ != 0;
  ImGui::BeginDisabled(!sourceIsHdr);

  ImGui::Checkbox(T("Aufnahme", "Recording"), &app.recordHdr);
  ImGui::SameLine();
  HelpMarker(T("Nimmt in zehn Bit auf der PQ-Kurve auf, mit BT.2020 und den Farbangaben, "
               "die eine Datei als HDR lesbar machen. Braucht einen Encoder, der zehn Bit "
               "kann -- bei HEVC und AV1 üblich, bei H.264 selten -- und einen Player, der "
               "PQ versteht. Ohne den Haken wird das heruntergerechnete Bild aufgenommen, "
               "das überall läuft.",
               "Records in ten bits on the PQ curve, BT.2020, with the colour description "
               "that makes a file readable as HDR. Needs an encoder that does ten bits -- "
               "usual for HEVC and AV1, rare for H.264 -- and a player that understands PQ. "
               "Without it the tone mapped picture is recorded, which plays anywhere."));

  ImGui::Checkbox(T("Screenshots", "Screenshots"), &app.screenshotHdr);
  if (app.screenshotHdr) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const char* shotNames[] = {T("JPEG XR (.jxr)", "JPEG XR (.jxr)"), T("AVIF", "AVIF")};
    int shot = (int)app.hdrShotFormat;
    if (ImGui::Combo("##hdrshot", &shot, shotNames, kHdrShotFormatCount)) {
      app.hdrShotFormat = (HdrShotFormat)shot;
    }
  }
  ImGui::SameLine();
  HelpMarker(T("Das eingestellte Bildformat wird dabei übergangen -- PNG und JPEG können "
               "den Umfang nicht halten.\n\n"
               "JPEG XR braucht nichts: Windows bringt den Encoder mit und die Fotos-App "
               "liest es. AVIF lesen alle Browser und das meiste außerhalb von Windows, "
               "geht aber über ffmpeg -- Screenshots kommen sonst ohne aus.",
               "The chosen picture format is bypassed -- PNG and JPEG cannot hold the "
               "range.\n\n"
               "JPEG XR needs nothing: Windows ships the encoder and the Photos app reads "
               "it. AVIF is read by every browser and by most things outside Windows, but "
               "it goes through ffmpeg -- which screenshots otherwise never need."));

  ImGui::Checkbox(T("Virtuelle Kamera", "Virtual camera"), &app.cameraHdr);
  ImGui::SameLine();
  HelpMarker(T("Bietet die Kamera zusätzlich in zehn Bit an; das Programm am anderen Ende "
               "wählt. Standardmäßig aus, und das mit Absicht: kaum ein Programm weiß "
               "heute etwas mit einer HDR-Webcam anzufangen, und eines, das die zehn Bit "
               "nimmt ohne sie zu verstehen, zeigt ein falsches Bild. Wirkt erst, wenn die "
               "Kamera das nächste Mal geöffnet wird.",
               "Offers the camera in ten bits as well; the program at the other end picks. "
               "Off by default, deliberately: almost nothing today knows what to do with an "
               "HDR webcam, and something that takes the ten bits without understanding "
               "them shows a wrong picture. Takes effect the next time the camera is "
               "opened."));

  ImGui::EndDisabled();
  if (!sourceIsHdr) {
    ImGui::TextDisabled("%s", T("Die Quelle ist SDR -- es gibt nichts zu behalten.",
                                "The source is SDR -- there is nothing to keep."));
  }
}

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
  ImGui::SeparatorText(T("Fenster", "Window"));
  ImGui::Checkbox(T("Einstellungen in eigenem Fenster", "Settings in their own window"),
                  &app.settingsSeparateWindow);
  ImGui::SameLine();
  HelpMarker(T("Ein echtes Fenster statt einer Fläche über dem Bild -- verschiebbar auf "
               "einen zweiten Monitor oder neben die Vorschau.",
               "A real window instead of a panel over the picture -- movable to a second "
               "monitor or beside the preview."));

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
  ImGui::SeparatorText(T("Verhalten", "Behaviour"));
  ImGui::Checkbox("VSync", &app.vsync);
  ImGui::SameLine();
  HelpMarker(T("Aus ist der größte Latenzgewinn, kann aber Tearing zeigen.",
               "Off is the biggest latency win, but can show tearing."));

  ImGui::Checkbox(T("Immer im Vordergrund", "Always on top"), &app.alwaysOnTop);
  ImGui::Checkbox(T("Mauszeiger im Vollbild ausblenden", "Hide cursor in fullscreen"),
                  &app.hideCursorFullscreen);
  ImGui::SameLine();
  HelpMarker(T("Zeiger nach kurzer Ruhe ausblenden.", "Hides the pointer after a short idle."));

  ImGui::Checkbox(T("Bildschirmschoner und Standby verhindern", "Prevent screensaver and sleep"),
                  &app.preventSleep);
  ImGui::SameLine();
  HelpMarker(T("Hält den Bildschirm wach, solange CapView läuft -- beim Zusehen drückt "
               "niemand eine Taste.",
               "Keeps the screen awake while CapView is running -- nobody presses a key "
               "while watching."));

  // The toolbar's own Hide button was the only way to turn it off, and the
  // right-click menu the only way back. A setting that can be reached from one
  // place and undone from another is a setting people lose.
  ImGui::Checkbox(T("Werkzeugleiste anzeigen", "Show toolbar"), &app.showToolbar);

  ImGui::Checkbox(T("Statistik einblenden", "Show statistics"), &app.showStats);
  ImGui::SameLine();
  // Read from the binding rather than written out, so rebinding the key is
  // visible here instead of leaving a stale "(F1)" behind.
  ImGui::TextDisabled("(%s)", HotkeyText(cfg().hotkeys[HotkeyAction::Stats]).c_str());

  ImGui::BeginDisabled(!app.showStats);
  int detail = (int)app.statsDetail;
  ImGui::SetNextItemWidth(-260.0f);
  if (ComboEnum(T("Umfang", "Detail"), &detail, 3, StatsDetailName)) {
    app.statsDetail = (StatsDetail)detail;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Kompakt: Bildraten und Bildalter. Normal: zusätzlich Format und Ton. "
               "Vollständig: alles.",
               "Compact: frame rates and frame age. Normal: adds format and audio. "
               "Full: everything."));

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

}

// --------------------------------------------------------------- record tab

void SettingsWindow::FolderRow(const char* id, int pickTag, char* buffer, size_t bufferSize,
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
  ImGui::BeginDisabled(picker_.busy());
  if (ImGui::Button((std::string(T("Durchsuchen", "Browse")) + "##" + id).c_str())) {
    FileDialogRequest request;
    request.mode = FileDialogRequest::Mode::Folder;
    request.startPath = value->empty() ? defaultFolder : ToWide(*value);
    picker_.Start(request, (HWND)ImGui::GetMainViewport()->PlatformHandleRaw, pickTag);
  }
  ImGui::EndDisabled();

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

void SettingsWindow::DrawFfmpegBlock(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;
  const bool busy = downloader_.busy();

  ImGui::SeparatorText("ffmpeg");
  if (ffmpeg && ffmpeg->found) {
    ImGui::TextWrapped("%s", ffmpeg->version.c_str());
    ImGui::TextDisabled("%s", ffmpeg->path.c_str());
  } else {
    ImGui::TextWrapped(
        T("Ohne ffmpeg keine Aufnahme. Screenshots funktionieren trotzdem.",
          "No recording without ffmpeg. Screenshots work regardless."));
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

  const ImGuiStyle& style = ImGui::GetStyle();
  const float browse = ImGui::CalcTextSize(T("Durchsuchen", "Browse")).x + style.FramePadding.x * 2;
  ImGui::SetNextItemWidth(-(browse + style.ItemSpacing.x));
  if (ImGui::InputTextWithHint("##ffmpegpath",
                               T("Eigener Pfad zu ffmpeg.exe (optional)",
                                 "Custom path to ffmpeg.exe (optional)"),
                               ffmpegPathBuffer_, sizeof(ffmpegPathBuffer_))) {
    rec.ffmpegPath = ffmpegPathBuffer_;
    if (ffmpeg) *ffmpeg = LocateFfmpeg(rec.ffmpegPath);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(picker_.busy());
  if (ImGui::Button(T("Durchsuchen##ffmpeg", "Browse##ffmpeg"))) {
    FileDialogRequest request;
    request.mode = FileDialogRequest::Mode::OpenFile;
    request.title = ToWide(T("ffmpeg.exe auswählen", "Select ffmpeg.exe"));
    request.startPath = rec.ffmpegPath.empty() ? ExeDirectory() : ToWide(rec.ffmpegPath);
    request.filters = {{L"ffmpeg.exe", L"ffmpeg.exe"},
                       {ToWide(T("Programme", "Programs")), L"*.exe"}};
    picker_.Start(request, (HWND)ImGui::GetMainViewport()->PlatformHandleRaw, kPickFfmpeg);
  }
  ImGui::EndDisabled();
}

void SettingsWindow::LoadRecordBuffers() {
  if (recordBuffersLoaded_) return;
  const RecordSettings& rec = cfg().record;
  std::snprintf(ffmpegPathBuffer_, sizeof(ffmpegPathBuffer_), "%s", rec.ffmpegPath.c_str());
  std::snprintf(folderBuffer_, sizeof(folderBuffer_), "%s", rec.outputFolder.c_str());
  std::snprintf(shotFolderBuffer_, sizeof(shotFolderBuffer_), "%s", rec.screenshotFolder.c_str());
  recordBuffersLoaded_ = true;
}

// HDR has a tab of its own rather than a block inside Display. It is not a
// display setting: three of its switches decide what the recorder, the
// screenshots and the virtual camera write, and those live in other tabs
// entirely. A subject that reaches across four tabs is a subject, not a section.
void SettingsWindow::DrawHdrTab() { DrawHdrBlock(); }

void SettingsWindow::DrawRecordTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;

  LoadRecordBuffers();

  const bool ready = ffmpeg && ffmpeg->found;
  if (!ready) {
    ImGui::Spacing();
    ImGui::TextWrapped(T("Ohne ffmpeg lässt sich nicht aufnehmen. Im Reiter Encoder steht, "
                         "wie es dazu kommt.",
                         "Recording needs ffmpeg. The Encoder tab says how to get it."));
  }

  ImGui::BeginDisabled(!ready);

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

  // Die Bitrate stand frueher hier und steht jetzt im Reiter Encoder, direkt
  // unter der Ratensteuerung. Getrennt waren die beiden nicht zu verstehen: bei
  // "Feste Qualitaet" ignoriert der Encoder die Bitrate vollstaendig, und der
  // Regler dafuer sass einen Reiter weiter, voll bedienbar und wirkungslos.

  // Recording faster than the card delivers would only duplicate frames, so the
  // ceiling is the source rate. The number below it is only reached when the
  // rate is not known yet, and it is a slider end rather than a limit -- the
  // unbounded setting is 0, "same as source", which takes whatever arrives.
  const int maxFps = sourceFps_ > 1.0 ? (int)std::lround(sourceFps_) : 1000;
  if (rec.fps > (double)maxFps) rec.fps = (double)maxFps;
  int fps = (int)std::lround(rec.fps);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::SliderInt(T("Bildrate", "Frame rate"), &fps, 0, maxFps,
                       fps == 0 ? T("wie die Quelle", "same as source") : "%d fps")) {
    rec.fps = (double)fps;
  }
  ImGui::SameLine();
  HelpMarker(sourceFps_ > 1.0
                 ? T("0 = Bildrate der Quelle. Höher als die Quelle ergibt nur doppelte Bilder.",
                     "0 = source frame rate. Above the source only duplicates frames.")
                 : T("0 = Bildrate der Quelle. Niedriger verwirft Bilder, Auflösung bleibt.",
                     "0 = source frame rate. Lower drops frames, resolution unchanged."));

  FolderRow("recfolder", kPickRecordFolder, folderBuffer_, sizeof(folderBuffer_),
            &rec.outputFolder, DefaultRecordFolder());

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

  ImGui::EndDisabled();

  // ---- stills ----
  // Outside the disabled block on purpose: screenshots go through Windows' own
  // imaging stack and have nothing to do with ffmpeg.
  ImGui::Spacing();
  ImGui::SeparatorText(T("Screenshots", "Screenshots"));
  ImGui::TextWrapped(T("Einzelbild aus der Quelle, ohne die Bedienoberfläche. Benötigt kein ffmpeg.",
                       "Single frame from the source, without the interface. Does not need "
                       "ffmpeg."));
  ImGui::Spacing();

  ImGui::Checkbox(T("Bedienoberfläche mit aufnehmen", "Include the interface"),
                  &rec.screenshotIncludeUi);
  HelpMarker(T("Statt des Bildes wird das fertige Fenster gespeichert, also mit Leiste, "
               "Meldungen und allem, was gerade darauf liegt. Das Ergebnis hat dann die "
               "Größe des Fensters und nicht die der Quelle, und es ist immer SDR: bei "
               "HDR-Ausgabe wird stattdessen das reine Bild gespeichert.",
               "Saves the finished window instead of the picture: the bar, the messages and "
               "whatever else is on it. The result is then the size of the window rather than "
               "of the source, and it is always SDR -- with HDR output the picture alone is "
               "saved instead."));
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

  FolderRow("shotfolder", kPickShotFolder, shotFolderBuffer_, sizeof(shotFolderBuffer_),
            &rec.screenshotFolder, DefaultScreenshotFolder());
  ImGui::Spacing();
  DrawVirtualCameraBlock();
  ImGui::Spacing();
  ImGui::SeparatorText(T("Nach MP4 umpacken", "Rewrap to MP4"));

  ImGui::TextWrapped(T("Legt MKV-Dateien ohne Neukodierung in eine MP4 um. Dauert Sekunden und "
                       "kostet keine Qualität. Das Original bleibt erhalten.",
                       "Puts MKV files into an MP4 without re-encoding. Takes seconds and costs "
                       "no quality. The original is kept."));
  ImGui::Spacing();

  const bool remuxBusy = remuxer_.busy();
  ImGui::BeginDisabled(remuxBusy || picker_.busy() || !ffmpeg || !ffmpeg->found);
  if (ImGui::Button(T("Dateien wählen ...", "Choose files ..."))) {
    FileDialogRequest request;
    request.mode = FileDialogRequest::Mode::OpenFiles;
    request.title = ToWide(T("Aufnahmen auswählen", "Select recordings"));
    request.startPath =
        rec.outputFolder.empty() ? DefaultRecordFolder() : ToWide(rec.outputFolder);
    request.filters = {{ToWide(T("Aufnahmen", "Recordings")), L"*.mkv;*.mp4;*.mov;*.avi;*.ts"},
                       {ToWide(T("Alle Dateien", "All files")), L"*.*"}};
    picker_.Start(request, (HWND)ImGui::GetMainViewport()->PlatformHandleRaw, kPickRemux);
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
  }}

void SettingsWindow::DrawEncoderTab(FfmpegInfo* ffmpeg) {
  RecordSettings& rec = cfg().record;
  LoadRecordBuffers();

  // Without ffmpeg none of the recording settings mean anything, so it leads and
  // the rest is greyed out rather than inviting people to configure a bitrate
  // for an encoder that cannot run.
  const bool ready = ffmpeg && ffmpeg->found;
  // Always here, whether ffmpeg was found or not. It used to move to the bottom
  // of the tab once it was working, so where you last saw it was no guide to
  // where it is -- and a thing that moves is a thing you hunt for.
  ImGui::Spacing();
  DrawFfmpegBlock(ffmpeg);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::BeginDisabled(!ready);

  // ---- encoder ----
  ImGui::Spacing();
  ImGui::SeparatorText(T("Encoder", "Encoder"));

  // Only what survived the test is offered. Listing every hardware encoder on a
  // machine that has one vendor's card invites people to pick something that
  // cannot run, and then wonder why.
  const bool tested = ffmpeg && ffmpeg->tested;
  const EncoderInfo* chosen = ffmpeg ? ffmpeg->Find(rec.encoder) : nullptr;
  const bool chosenOk = IsAutoEncoder(rec.encoder) || (chosen && chosen->available);

  const char* preview = !tested ? T("noch nicht geprüft", "not tested yet")
                                : RecordEncoderName((int)rec.encoder);

  ImGui::BeginDisabled(!tested);
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::BeginCombo(T("Encoder", "Encoder"), preview)) {
    for (RecordEncoder mode : {RecordEncoder::Auto, RecordEncoder::AutoEfficient}) {
      if (ImGui::Selectable(RecordEncoderName((int)mode), rec.encoder == mode)) {
        rec.encoder = mode;
      }
    }
    ImGui::Separator();
    for (const EncoderInfo& e : ffmpeg->encoders) {
      if (!e.available) continue;
      const bool selected = (rec.encoder == e.id);
      if (ImGui::Selectable(e.label.c_str(), selected)) rec.encoder = e.id;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Automatisch: der verträglichste, der hier läuft — H.264 vor H.265 vor AV1, Hardware vor CPU.",
               "Automatic: the most compatible one that works here — H.264 before H.265 before AV1, hardware before CPU."));

  // What the two automatic modes actually do, in the terms that matter: where
  // the file will play, and how big it is. Naming the codecs alone would only
  // help people who already know the answer.
  if (tested && IsAutoEncoder(rec.encoder)) {
    const EncoderInfo* picked = ffmpeg->Resolve(rec.encoder);
    TextDisabledWrapped(
        rec.encoder == RecordEncoder::Auto
            ? T("H.264 zuerst: läuft auf allem, auch auf älteren Fernsehern und Handys.",
                "H.264 first: plays on anything, including older TVs and phones.")
            : T("AV1 und H.265 zuerst: bei gleicher Qualität deutlich kleinere Dateien, "
                "aber ältere Geräte können sie nicht abspielen.",
                "AV1 and H.265 first: much smaller files at the same quality, but older "
                "devices cannot play them."));
    if (picked) {
      ImGui::TextDisabled(T("Hier heißt das: %s", "Here that means: %s"), picked->label.c_str());
    }
  }

  if (!tested) {
    ImGui::TextWrapped(
        T("Welche Encoder gehen, hängt an der Grafikkarte. Einmal prüfen — das Ergebnis "
          "bleibt gespeichert und wird nur bei einem Hardwarewechsel neu gebraucht.",
          "Which encoders work depends on the graphics card. Test once — the result is "
          "kept and is only needed again after a hardware change."));
  } else if (!chosenOk) {
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.35f, 1.0f), "%s",
                       T("Der gespeicherte Encoder ist hier nicht verfügbar.",
                         "The saved encoder is not available here."));
  }

  const bool busy = downloader_.busy();
  ImGui::BeginDisabled(!ffmpeg || !ffmpeg->found || busy || probeBusy_);
  const char* testLabel = probeBusy_ ? T("Wird geprüft ...", "Testing ...")
                          : tested   ? T("Erneut testen", "Test again")
                                     : T("Encoder testen", "Test encoders");
  if (ImGui::Button(testLabel)) probeRequested_ = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(T("Kodiert je zwei Testbilder. Die Encoder-Liste des Builds nennt nur, was "
               "einkompiliert ist, nicht was die Hardware kann.",
               "Encodes two test frames each. The build's encoder list only names what was "
               "compiled in, not what the hardware can do."));

  // Two wrapped lines rather than ten. A list of every candidate ran off the
  // bottom of the dialog, which made the result of pressing Test nearly
  // invisible -- and the result is the entire point of pressing it.
  if (ffmpeg && ffmpeg->found && tested) {
    std::string works, fails;
    for (const EncoderInfo& e : ffmpeg->encoders) {
      if (!e.tested) continue;
      std::string& target = e.available ? works : fails;
      if (!target.empty()) target += ", ";
      target += e.label;
    }
    if (!works.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.5f, 1.0f));
      ImGui::TextWrapped("%s%s", T("Verwendbar: ", "Usable: "), works.c_str());
      ImGui::PopStyleColor();
    }
    if (!fails.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::TextWrapped("%s%s", T("Geht hier nicht: ", "Not available here: "), fails.c_str());
      ImGui::PopStyleColor();
    }
  }

  const EncoderInfo* selected =
      ffmpeg ? (IsAutoEncoder(rec.encoder) ? nullptr : ffmpeg->Find(rec.encoder)) : nullptr;
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
  HelpMarker(T("Nur für CPU-Encoder, und nur solange die Voreinstellung unten auf "
               "automatisch steht.",
               "Software encoders only, and only while the preset below is on automatic."));

  DrawEncoderBlock(selected ? selected : (ffmpeg ? ffmpeg->Resolve(rec.encoder) : nullptr));

  ImGui::EndDisabled();

}

// ----------------------------------------------------------------- tools tab

void SettingsWindow::DrawVirtualCameraBlock() {
  ImGui::Spacing();
  ImGui::SeparatorText(T("Virtuelle Kamera", "Virtual camera"));

  // The registration is a registry read; caching it keeps this off the disk on
  // every frame, and the two buttons below refresh it when they change it.
  const double now = ImGui::GetTime();
  if (now - vcamStatusChecked_ > 2.0) {
    vcamStatus_ = (int)VirtualCamera::Status();
    vcamStatusChecked_ = now;
  }
  const auto status = (VirtualCamera::Install)vcamStatus_;

  ImGui::TextWrapped(T("Gibt das Bild als Webcam an andere Programme weiter -- OBS, Discord, "
                       "Teams, den Browser. Die Kamera heißt \"CapView Virtual Camera\" und "
                       "steht ab dem Installieren dauerhaft in der Geräteliste. Läuft CapView "
                       "gerade nicht, zeigt sie einen Hinweis statt eines Bildes.",
                       "Offers the picture to other programs as a webcam -- OBS, Discord, "
                       "Teams, the browser. The camera is called \"CapView Virtual Camera\" "
                       "and stays in the device list once installed. While CapView is not "
                       "running it shows a notice instead of a picture."));
  ImGui::Spacing();

  if (status != VirtualCamera::Install::Installed) {
    ImGui::TextWrapped(
        status == VirtualCamera::Install::Stale
            ? T("Die Kameraquelle ist registriert, zeigt aber ins Leere -- vermutlich wurde "
                "CapView verschoben. Einmal neu installieren setzt das gerade.",
                "The camera source is registered but points nowhere -- CapView was probably "
                "moved. Installing once more puts that right.")
            : T("Einmalig zu installieren. Die Kameraquelle wird von jedem Programm "
                "geladen, das die Kamera öffnet, deshalb muss sie systemweit registriert "
                "werden und Windows fragt nach Administratorrechten. Das Benutzen danach "
                "braucht keine. Die Quelle steckt im Programm und wird beim Installieren "
                "danebengelegt -- es gibt also keine zweite Datei, um die man sich kümmern "
                "müsste.",
                "To be installed once. The camera source is loaded by every program that "
                "opens the camera, so it has to be registered machine-wide and Windows will "
                "ask for administrator rights. Using it afterwards needs none. The source "
                "travels inside the program and is laid down when installing -- so there is "
                "no second file to look after."));
    ImGui::Spacing();
    if (ImGui::Button(T("Kamera installieren", "Install camera"), ImVec2(200.0f, 0.0f))) {
      virtualCameraRequest_ = 1;
      vcamStatusChecked_ = 0.0;
    }
    return;
  }

  bool on = cfg().app.virtualCamera;
  if (ImGui::Checkbox(T("Virtuelle Kamera einschalten", "Turn the virtual camera on"), &on)) {
    cfg().app.virtualCamera = on;
  }

  if (vcamRunning_) {
    if (vcamConsumers_.empty()) {
      ImGui::TextDisabled("%s", T("Läuft, bisher liest niemand mit.",
                                  "Running; nothing is reading it yet."));
    } else {
      ImGui::Spacing();
      // Every reader gets its own row, because every reader gets its own
      // format. OBS taking the source untouched while Discord takes 720p30 is
      // the normal case, not an oddity, and one line could only lie about it.
      if (ImGui::BeginTable("vcamconsumers", 3,
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(T("Programm", "Program"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Format", "Format"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Status", "State"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const VirtualCamera::Consumer& c : vcamConsumers_) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(c.name.empty() ? "?" : c.name.c_str());
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("PID %lu", c.pid);
          ImGui::TableNextColumn();
          if (c.width > 0 && c.height > 0) {
            ImGui::Text("%d x %d @ %.4g%s", c.width, c.height, c.fps, c.wide ? " HDR" : "");
          } else {
            ImGui::TextDisabled("%s", T("noch offen", "not settled"));
          }
          ImGui::TableNextColumn();
          if (c.streaming) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                               T("liest mit", "reading"));
          } else {
            ImGui::TextDisabled("%s", T("verbunden", "connected"));
          }
        }
        ImGui::EndTable();
      }
    }
  }

  ImGui::Spacing();
  TextDisabledWrapped(T("Das Bild geht so hinaus, wie es hier ankommt -- gleiche Auflösung, "
                        "gleiche Bildrate. Wer weniger verlangt, bekommt es seitenrichtig "
                        "eingepasst; schwarze Balken füllen den Rest. Welches Format ein "
                        "Programm nimmt, entscheidet es beim Öffnen der Kamera und behält es "
                        "dann; wechselt die Quelle, muss man es dort neu öffnen.",
                        "The picture goes out as it arrives here -- same resolution, same "
                        "frame rate. Anything asking for less gets it fitted with its shape "
                        "kept; black bars fill the rest. Which format a program takes it "
                        "settles when it opens the camera and keeps from then on; if the "
                        "source changes, reopen it there."));

  ImGui::Spacing();
  if (ImGui::Button(T("Kamera deinstallieren", "Uninstall camera"), ImVec2(200.0f, 0.0f))) {
    virtualCameraRequest_ = 2;
    vcamStatusChecked_ = 0.0;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", T("(fragt wieder nach Administratorrechten)",
                              "(asks for administrator rights again)"));
}

void SettingsWindow::DrawEncoderBlock(const EncoderInfo* encoder) {
  RecordSettings& rec = cfg().record;
  ImGui::Spacing();
  ImGui::SeparatorText(T("Encoder-Einstellungen", "Encoder settings"));

  // Which of these mean anything depends on the encoder. Showing the rest
  // greyed out says more than hiding them: it is the difference between "your
  // card cannot" and "CapView cannot".
  const Recorder::Family family =
      encoder ? Recorder::FamilyOf(encoder->ffmpegName) : Recorder::Family::Software;
  const bool nvenc = family == Recorder::Family::Nvenc;
  const bool amf = family == Recorder::Family::Amf;
  const bool qsv = family == Recorder::Family::Qsv;

  const char* rateNames[] = {T("Konstant (CBR)", "Constant (CBR)"),
                             T("Variabel (VBR)", "Variable (VBR)"),
                             T("Feste Qualität", "Constant quality")};
  int rate = (int)rec.rateControl;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Ratensteuerung", "Rate control"), &rate, rateNames, kRateControlCount)) {
    rec.rateControl = (RateControl)rate;
  }
  ImGui::SameLine();
  HelpMarker(T("Konstant hält die Datenrate stabil -- das will man beim Streamen. Variabel "
               "gibt bewegten Stellen mehr und darf bis auf das Anderthalbfache "
               "hochgehen. Feste Qualität ignoriert die Datenrate ganz: die Datei wird so "
               "groß, wie das Bild es verlangt.",
               "Constant keeps the data rate steady, which is what streaming wants. "
               "Variable gives busy moments more and may peak at half again. Constant "
               "quality ignores the data rate entirely: the file comes out as large as the "
               "picture demands."));

  // Bitrate und Qualitaet sind die beiden Zahlen, die an der Ratensteuerung
  // haengen -- je nach Stellung zaehlt genau eine davon. Beide stehen deshalb
  // hier, direkt darunter, und die gerade nicht zaehlende wird ausgegraut statt
  // versteckt: dasselbe Argument wie oben bei den Encoder-Merkmalen. Ausgegraut
  // sagt "gilt hier nicht", versteckt sagt gar nichts und laesst die Seite
  // ausserdem springen.
  const bool byQuality = rec.rateControl == RateControl::Quality;

  ImGui::BeginDisabled(byQuality);
  // Logarithmisch, weil die Skala sonst am falschen Ende genau ist. 1000 bis
  // 100000 linear auf die Bahnbreite sind rund 330 kbit je Bildpunkt -- man
  // findet einen Wert, treffen kann man ihn nicht. Schlimmer ist die
  // Verteilung: der Bereich, in dem eine Quelle dieser Karte tatsaechlich
  // landet, 1 bis 10 Mbit, belegt neun Prozent der Bahn, die restlichen
  // neunzig sind fuer eine Karte gedacht, die es hier nicht gibt.
  // Logarithmisch bekommt der benutzte Bereich die halbe Bahn.
  ImGui::SetNextItemWidth(-360.0f);
  ImGui::SliderInt("##bitrate", &rec.bitrateKbps, 1000, 100000, "%d kbit/s",
                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
  ImGui::SameLine();
  // Und ein Feld daneben, weil ein Regler eine Zahl nur ungefaehr trifft und
  // man hier oft eine bestimmte will. Strg+Klick auf den Regler tut dasselbe,
  // aber das weiss niemand, dem es niemand sagt -- ein sichtbares Feld schon.
  ImGui::SetNextItemWidth(96.0f);
  if (ImGui::InputInt(T("Bitrate", "Bitrate"), &rec.bitrateKbps, 0, 0,
                      ImGuiInputTextFlags_CharsDecimal)) {
    rec.bitrateKbps = Clamp(rec.bitrateKbps, 1000, 100000);
  }
  ImGui::SameLine();
  HelpMarker(byQuality
                 ? T("Bei fester Qualität ohne Wirkung -- dort entscheidet der Regler "
                     "darunter, wie groß die Datei wird.",
                     "Ignored at constant quality -- there the setting below decides how "
                     "large the file gets.")
                 : T("In kbit/s. Für 720x576 sind 6000 bis 12000 der übliche Bereich. Die "
                     "Zahl lässt sich rechts auch eintippen.",
                     "In kbit/s. For 720x576 the usual range is 6000 to 12000. The number "
                     "can also be typed on the right."));
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!byQuality);
  ImGui::SetNextItemWidth(-260.0f);
  ImGui::SliderInt(T("Qualität", "Quality"), &rec.qualityLevel, 1, 51,
                   T("%d (kleiner = besser)", "%d (lower is better)"),
                   ImGuiSliderFlags_AlwaysClamp);
  ImGui::SameLine();
  HelpMarker(byQuality
                 ? T("Die Skala unterscheidet sich zwischen den Encodern leicht. 18 bis 24 ist "
                     "der übliche Bereich; darunter wächst die Datei schnell, ohne dass man "
                     "viel sieht.",
                     "The scale differs a little between encoders. 18 to 24 is the usual range; "
                     "below that the file grows quickly for little that anyone can see.")
                 : T("Nur bei fester Qualität. Sonst entscheidet die Bitrate darüber.",
                     "Constant quality only. Otherwise the bitrate decides."));
  ImGui::EndDisabled();

  const char* presetNames[] = {T("Automatisch", "Automatic"),
                               T("Am schnellsten", "Fastest"),
                               T("Schneller", "Faster"),
                               T("Schnell", "Fast"),
                               T("Mittel", "Medium"),
                               T("Langsam", "Slow"),
                               T("Langsamer", "Slower"),
                               T("Am langsamsten", "Slowest")};
  int preset = (int)rec.preset;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Voreinstellung", "Preset"), &preset, presetNames, kEncoderPresetCount)) {
    rec.preset = (EncoderPreset)preset;
  }
  ImGui::SameLine();
  HelpMarker(amf ? T("AMF kennt nur drei Stufen; die sieben hier fallen darauf zusammen.",
                     "AMF has three steps; these seven fold onto them.")
                 : T("Langsamer heißt besseres Bild bei gleicher Datenrate -- und mehr Last. "
                     "Automatisch überlässt es dem Encoder.",
                     "Slower means a better picture at the same data rate, and more load. "
                     "Automatic leaves it to the encoder."));

  ImGui::BeginDisabled(!nvenc && !amf);
  const char* tuneNames[] = {T("Automatisch", "Automatic"), T("Qualität", "Quality"),
                             T("Geringe Latenz", "Low latency")};
  int tune = (int)rec.tune;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Abstimmung", "Tuning"), &tune, tuneNames, kEncoderTuneCount)) {
    rec.tune = (EncoderTune)tune;
  }
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!nvenc);
  const char* passNames[] = {T("Automatisch", "Automatic"), T("Aus", "Off"),
                             T("Zwei (Viertelauflösung)", "Two (quarter resolution)"),
                             T("Zwei (volle Auflösung)", "Two (full resolution)")};
  int pass = (int)rec.multipass;
  ImGui::SetNextItemWidth(-260.0f);
  if (ImGui::Combo(T("Durchläufe", "Multipass"), &pass, passNames, kMultipassCount)) {
    rec.multipass = (Multipass)pass;
  }
  ImGui::SameLine();
  HelpMarker(T("Ein zweiter Durchlauf trifft die Datenrate genauer, vor allem nah am Limit. "
               "Nur NVENC kann das.",
               "A second pass hits the data rate more accurately, especially near the "
               "ceiling. NVENC only."));
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!nvenc && !amf && !qsv);
  ImGui::Checkbox(T("Vorausschau", "Look-ahead"), &rec.lookAhead);
  ImGui::SameLine();
  HelpMarker(T("Der Encoder sieht ein Stück in die Zukunft und verteilt die Bits besser. "
               "Kostet etwas Verzögerung -- beim Aufnehmen egal, beim Streamen nicht.",
               "The encoder looks a little way ahead and spreads its bits better. Costs some "
               "delay -- which does not matter for recording and does for streaming."));
  ImGui::Checkbox(T("Adaptive Quantisierung", "Adaptive quantisation"), &rec.adaptiveQuant);
  ImGui::SameLine();
  HelpMarker(T("Gibt den Stellen mehr Bits, an denen das Auge hinsieht -- Flächen und "
               "Verläufe -- und nimmt sie dort weg, wo ohnehin Unruhe ist.",
               "Gives more bits to where the eye looks -- flat areas and gradients -- and "
               "takes them from where there is already busyness."));
  ImGui::EndDisabled();

  if (!nvenc && !amf && !qsv) {
    TextDisabledWrapped(T("Der Softwareencoder nimmt nur Ratensteuerung und "
                          "Voreinstellung an.",
                          "The software encoder takes only rate control and preset."));
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
  TextDisabledWrapped(T("Fest belegt: Esc verlässt das Vollbild und schließt Dialoge, "
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
      T("Ein Profil hält alles: Gerät, Eingang, Videonorm, Format sowie sämtliche Bild- "
        "und Toneinstellungen. Genau eines pro Konsole -- ein SNES über Composite will "
        "andere Filter als eine Switch über HDMI, und keine dieser Einstellungen passt "
        "auf beide.\n\n"
        "Einmal einrichten, dann mit Strg+Zahl umschalten, statt bei jedem Kabelwechsel "
        "dieselben Regler wieder von Hand zu suchen.",
        "A profile holds everything: device, input, video standard, format and every "
        "picture and audio setting. One per console -- a SNES over composite wants "
        "different filters from a Switch over HDMI, and no setting suits both.\n\n"
        "Set them up once, then switch with Ctrl+number instead of hunting for the same "
        "controls by hand every time a cable changes."));
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
  // Der uebliche Weg, und deshalb steht er hier statt eines "Duplizieren".
  //
  // Eine neue Konsole anzuschliessen heisst fast nie, von vorn anzufangen: man
  // aendert das, was anders ist, und will das Ergebnis behalten. Ein leeres
  // Profil zwingt dagegen dazu, alles noch einmal einzustellen, was ohnehin
  // schon richtig stand.
  if (ImGui::Button(T("Aktuelles sichern als ...", "Save current as ..."))) {
    renameTarget_ = -1;  // -1 heißt: ein neues anlegen statt umbenennen
    renameBuffer_[0] = '\0';
    namePopupFocus_ = true;
    ImGui::OpenPopup("rename_profile");
  }
  WrappedTooltip(T("Legt ein neues Profil mit allem an, was gerade eingestellt ist -- Gerät, "
                   "Eingang, Format, Bild und Ton. Danach gleich den Namen eintippen, am "
                   "besten den der Konsole.\n\n"
                   "Das aktuelle Profil bleibt, wie es war.",
                   "Creates a new profile holding everything as it stands -- device, input, "
                   "format, picture and audio. Type the name straight away, ideally the "
                   "console's.\n\n"
                   "The profile you were on stays as it was."));
  ImGui::SameLine();
  if (ImGui::Button(T("Umbenennen", "Rename"))) {
    renameTarget_ = c.activeProfile;
    std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", c.active().name.c_str());
    namePopupFocus_ = true;
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
    const bool creating = renameTarget_ < 0;
    ImGui::TextUnformatted(creating ? T("Wie soll das Profil heißen?", "What should it be called?")
                                    : T("Profil umbenennen", "Rename profile"));
    if (creating) {
      ImGui::TextDisabled("%s", T("Zum Beispiel der Name der Konsole.",
                                  "The console's name, for instance."));
    }
    // Der Cursor steht gleich im Feld: wer hier landet, will tippen, nicht erst
    // hineinklicken. Nur im ersten Bild des Popups, sonst faenge es die Eingabe
    // jedes Mal neu ein.
    if (namePopupFocus_) {
      ImGui::SetKeyboardFocusHere();
      namePopupFocus_ = false;
    }
    ImGui::SetNextItemWidth(320.0f);
    const bool entered = ImGui::InputText("##name", renameBuffer_, sizeof(renameBuffer_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if (entered || ImGui::Button("OK", ImVec2(100, 0))) {
      const std::string trimmed = Trim(renameBuffer_);
      if (!trimmed.empty()) {
        if (creating) {
          // Das gerade aktive Profil ist der aktuelle Zustand -- die
          // Einstellungen wirken sofort und werden dorthin geschrieben.
          Profile copy = c.active();
          copy.name = trimmed;
          c.profiles.push_back(copy);
          c.activeProfile = (int)c.profiles.size() - 1;
        } else if (renameTarget_ < (int)c.profiles.size()) {
          c.profiles[(size_t)renameTarget_].name = trimmed;
        }
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"), ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace cap
