#include "ui/overlay.h"

#include <cmath>

#include "i18n.h"
#include "imgui.h"

namespace cap {
namespace {

const ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoInputs;

void Row(const char* label, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(value.c_str());
}

std::string Decimal(double v, int digits) {
  std::string s = Format("%.*f", digits, v);
  if (CurrentLanguage() == Language::German) {
    for (char& c : s) {
      if (c == '.') c = ',';
    }
  }
  return s;
}

// Places an overlay window in one of the four corners, inset from the edge.
void PositionInCorner(OsdCorner corner, float inset) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool right = (corner == OsdCorner::TopRight || corner == OsdCorner::BottomRight);
  const bool bottom = (corner == OsdCorner::BottomLeft || corner == OsdCorner::BottomRight);

  const ImVec2 pos(vp->WorkPos.x + (right ? vp->WorkSize.x - inset : inset),
                   vp->WorkPos.y + (bottom ? vp->WorkSize.y - inset : inset));
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
}

}  // namespace

void DrawStatsPanel(const OverlayStats& s) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16.0f, vp->WorkPos.y + 16.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.78f);

  if (!ImGui::Begin("##stats", nullptr, kOverlayFlags)) {
    ImGui::End();
    return;
  }

  if (ImGui::BeginTable("statstable", 2, ImGuiTableFlags_SizingFixedFit)) {
    Row(T("Profil", "Profile"), s.profileName);
    if (!s.deviceName.empty()) Row(T("Gerät", "Device"), s.deviceName);
    if (!s.inputName.empty()) Row(T("Eingang", "Input"), s.inputName);

    if (s.format.valid()) {
      std::string src = Format("%dx%d  %s", s.format.width, s.format.height,
                               s.format.subtypeLabel.c_str());
      if (s.format.interlaced) src += "  interlaced";
      Row(T("Quelle", "Source"), src);
      Row(T("Quellrate", "Source rate"),
          Decimal(s.sink.sourceFps, 2) + T(" fps  (gemeldet ", " fps  (reported ") +
              Decimal(s.format.fps, 2) + ")");
    } else {
      Row(T("Quelle", "Source"), "—");
    }

    Row(T("Anzeige", "Display"),
        Format("%dx%d  %s", s.displayWidth, s.displayHeight, s.filterName));
    Row(T("Ausgabe", "Output"),
        Decimal(s.presentFps, 1) + (s.vsync ? T(" fps  VSync an", " fps  VSync on")
                                            : T(" fps  VSync aus", " fps  VSync off")) +
            ((!s.vsync && s.tearing) ? T(", Tearing erlaubt", ", tearing allowed") : ""));
    if (s.deinterlacing) Row(T("Halbbilder", "Fields"), T("Bob aktiv", "bob active"));

    Row(T("Bilder", "Frames"),
        Format(T("%llu angezeigt, %llu verworfen", "%llu shown, %llu dropped"),
               (unsigned long long)s.sink.displayed, (unsigned long long)s.sink.dropped));
    Row(T("Bildalter", "Frame age"), Decimal(s.frameAgeMs, 1) + " ms");
    if (s.videoDelayMs > 0) {
      Row(T("Bildverzögerung", "Video delay"),
          Format(T("%d ms (A/V-Versatz)", "%d ms (A/V offset)"), s.videoDelayMs));
    }

    if (s.audio.running) {
      Row(T("Ton ein", "Audio in"),
          s.audio.inputName + (s.audio.directShowInput ? "  [DirectShow]" : "  [WASAPI]"));
      Row(T("Ton aus", "Audio out"),
          s.audio.outputName + (s.audio.exclusive ? "  [Exclusive]" : "  [Shared]"));
      Row(T("Tonpuffer", "Audio buffer"),
          Decimal(s.audio.bufferMs, 1) + T(" ms  (Ziel ", " ms  (target ") +
              Decimal(s.audio.targetMs, 0) + " ms)");
      Row(T("Tonformat", "Audio format"),
          Format("%d Hz -> %d Hz", s.audio.captureRate, s.audio.renderRate));
      if (s.audio.underruns || s.audio.overruns) {
        Row(T("Tonaussetzer", "Audio glitches"),
            Format(T("%llu leer, %llu übergelaufen", "%llu underrun, %llu overrun"),
                   (unsigned long long)s.audio.underruns,
                   (unsigned long long)s.audio.overruns));
      }
    } else {
      Row(T("Ton", "Audio"), T("aus", "off"));
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

void DrawStatusCard(const std::string& title, const std::string& detail, bool spinner) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 0.0f), ImVec2(560.0f, FLT_MAX));

  if (!ImGui::Begin("##status", nullptr, kOverlayFlags)) {
    ImGui::End();
    return;
  }

  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
  ImGui::TextUnformatted(title.c_str());
  if (!detail.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", detail.c_str());
  }
  ImGui::PopTextWrapPos();

  if (spinner) {
    ImGui::Spacing();
    // Three dots cycling, so a long reconnect does not look frozen.
    const int phase = (int)(ImGui::GetTime() * 3.0) % 4;
    std::string dots(3, '.');
    for (int i = 0; i < 3; ++i) {
      if (i >= phase) dots[(size_t)i] = ' ';
    }
    ImGui::TextDisabled(T("Neuer Versuch%s", "Retrying%s"), dots.c_str());
  }

  ImGui::End();
}

void DrawToast(const std::string& text, double age, double duration) {
  if (age >= duration) return;
  const float fade = (float)Clamp((duration - age) / 0.4, 0.0, 1.0);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 48.0f),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.82f * fade);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  if (ImGui::Begin("##toast", nullptr, kOverlayFlags)) {
    ImGui::TextUnformatted(text.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void DrawRecordIndicator(double seconds, OsdCorner volumeCorner) {
  // Opposite corner to the volume readout: both can appear at once, and a
  // number sliding out from under a dot looks like a bug.
  OsdCorner corner = OsdCorner::TopRight;
  switch (volumeCorner) {
    case OsdCorner::TopRight: corner = OsdCorner::TopLeft; break;
    case OsdCorner::TopLeft: corner = OsdCorner::TopRight; break;
    case OsdCorner::BottomRight: corner = OsdCorner::BottomLeft; break;
    case OsdCorner::BottomLeft: corner = OsdCorner::BottomRight; break;
  }
  // Except when the statistics panel already owns the top left.
  if (corner == OsdCorner::TopLeft) corner = OsdCorner::BottomLeft;

  PositionInCorner(corner, 24.0f);
  ImGui::SetNextWindowBgAlpha(0.80f);

  if (ImGui::Begin("##recording", nullptr, kOverlayFlags)) {
    // One slow pulse per second, so it reads as "running" without flickering.
    const float pulse = 0.55f + 0.45f * (float)std::abs(std::sin(ImGui::GetTime() * 3.14159));
    const float radius = ImGui::GetFontSize() * 0.32f;
    const ImVec2 centre = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(centre.x + radius, centre.y + ImGui::GetFontSize() * 0.5f), radius,
        ImGui::GetColorU32(ImVec4(0.95f, 0.25f, 0.25f, pulse)));
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, ImGui::GetFontSize()));
    ImGui::SameLine();

    const int total = (int)seconds;
    ImGui::Text("%s  %02d:%02d:%02d", T("Aufnahme", "REC"), total / 3600, (total / 60) % 60,
                total % 60);
  }
  ImGui::End();
}

void DrawVolumeOsd(float volume, bool muted, OsdCorner corner, double age, double duration) {
  if (age >= duration) return;
  const float fade = (float)Clamp((duration - age) / 0.35, 0.0, 1.0);

  PositionInCorner(corner, 24.0f);
  ImGui::SetNextWindowBgAlpha(0.85f * fade);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  if (ImGui::Begin("##volume", nullptr, kOverlayFlags)) {
    const float percent = volume * 100.0f;
    if (muted) {
      ImGui::TextUnformatted(T("Stumm", "Muted"));
    } else {
      ImGui::Text(T("Lautstärke  %.0f %%", "Volume  %.0f %%"), percent);
    }

    // A plain bar rather than a slider: this is a readout, not a control.
    const ImVec2 size(190.0f, 6.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 trackColor = ImGui::GetColorU32(ImGuiCol_FrameBg, fade);
    const ImU32 fillColor =
        ImGui::GetColorU32(muted ? ImGuiCol_TextDisabled : ImGuiCol_SliderGrab, fade);

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), trackColor, 3.0f);
    const float filled = muted ? 0.0f : size.x * Clamp(volume, 0.0f, 1.0f);
    if (filled > 0.5f) {
      draw->AddRectFilled(origin, ImVec2(origin.x + filled, origin.y + size.y), fillColor, 3.0f);
    }
    ImGui::Dummy(size);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace cap
