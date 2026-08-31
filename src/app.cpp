#include "app.h"

#include <shellapi.h>   // ShellExecuteW
#include <windowsx.h>  // GET_X_LPARAM

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "i18n.h"
#include "imgui.h"
#include "record/ffmpeg_locator.h"
#include "render/d3d_context.h"
#include "record/screenshot.h"
#include "resource.h"
#include "ui/theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {
namespace {

const wchar_t kWindowClass[] = L"CapViewMainWindow";
const wchar_t kWindowTitle[] = L"CapView";

// How long without a frame before we call it "no signal".
const double kNoSignalSeconds = 1.5;
// How long a flat picture is given before it counts as no signal rather than as
// a black screen. Generous, because being wrong here means covering a game that
// was only fading out; a title screen that stays black for eight seconds is rare
// and a card with nothing on it stays black forever.
const double kFlatSeconds = 8.0;
// Unless the decoder also says it has no lock, in which case there is no reason
// to keep waiting on a second opinion.
const double kFlatUnlockedSeconds = 2.0;
// Snow is the confident verdict, but not by as much as a synthetic test
// suggested. Measured against a real racing demo on this card, steady fast
// motion reads a change of 37 against a threshold of 48, and a fade-and-cut
// between scenes produced a burst of Snow, Picture, Flat, Picture, Snow inside
// 1.2 seconds. Two seconds clears that comfortably and costs nothing where it
// matters: an unterminated input does not stop being snow after two seconds.
const double kSnowSeconds = 2.0;
// Delay between reconnect attempts, and the slower pace once it is clear the
// device is not coming back on its own.
const double kRetrySeconds = 2.0;
const double kRetrySlowSeconds = 10.0;
const int kRetryBackoffAfter = 5;
// Hide the pointer after this much stillness in fullscreen.
const double kCursorIdleSeconds = 2.0;
// How long the volume readout stays on screen after a change.
const double kVolumeOsdSeconds = 1.6;
// One wheel notch.
const float kVolumeStep = 0.05f;

int64_t QpcNow() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcFreq() {
  static const double f = [] {
    LARGE_INTEGER v;
    ::QueryPerformanceFrequency(&v);
    return (double)v.QuadPart;
  }();
  return f;
}

int64_t SecondsToQpc(double seconds) {
  return (int64_t)(seconds * QpcFreq());
}

double QpcToSeconds(int64_t ticks) {
  return (double)ticks / QpcFreq();
}

// Wie SetItemTooltip, aber mit Umbruch -- dieselbe Breite wie im
// Einstellungsfenster, damit beide gleich aussehen. Der eingebaute bricht nicht
// um: ein ganzer Satz laeuft dann als eine einzige Zeile quer ueber den
// Bildschirm und steht mit dem Ende davon ausserhalb.
void WrappedTooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  App* app = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = (CREATESTRUCTW*)lparam;
    app = (App*)cs->lpCreateParams;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
  } else {
    app = (App*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  if (app) return app->HandleMessage(hwnd, msg, wparam, lparam);
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// ------------------------------------------------------------ FrameDelayLine

void FrameDelayLine::Configure(double delayMs) {
  if (delayMs <= 0.0) {
    delayMs_ = 0.0;
    Clear();
    return;
  }
  if (std::abs(delayMs - delayMs_) > 0.5) Clear();
  delayMs_ = delayMs;
}

void FrameDelayLine::Clear() {
  queue_.clear();
  pool_.clear();
  current_ = Entry{};
}

void FrameDelayLine::Push(const FrameView& frame, int64_t qpc) {
  if (!active() || !frame.valid()) return;
  Entry e;
  if (!pool_.empty()) {
    e = std::move(pool_.back());
    pool_.pop_back();
  }
  e.data.assign(frame.data, frame.data + frame.size);
  e.qpc = qpc;
  e.sequence = frame.sequence;
  queue_.push_back(std::move(e));

  // A pathological setting must not eat all memory.
  while (queue_.size() > 64) queue_.pop_front();
}

bool FrameDelayLine::Pop(FrameView* out, int64_t qpc) {
  if (!active()) return false;
  const int64_t threshold = qpc - SecondsToQpc(delayMs_ / 1000.0);
  bool got = false;
  while (!queue_.empty() && queue_.front().qpc <= threshold) {
    if (!current_.data.empty()) pool_.push_back(std::move(current_));
    current_ = std::move(queue_.front());
    queue_.pop_front();
    got = true;
  }
  if (!got || current_.data.empty()) return false;
  if (out) {
    out->data = current_.data.data();
    out->size = current_.data.size();
    out->sequence = current_.sequence;
  }
  return true;
}

// ----------------------------------------------------------------------- App

App::~App() {
  Shutdown();
}

bool App::Initialize(HINSTANCE instance, int showCmd) {
  instance_ = instance;

  std::string configError;
  // Load meldet dasselbe False fuer "keine Datei" und fuer "Datei kaputt". Der
  // Unterschied steht in error: beim allerersten Start ist es leer.
  firstRun_ = !config_.Load(&configError) && configError.empty();
  LogInit(config_.app.logToFile);
  CAP_LOG("CapView startet");
  if (!configError.empty()) CAP_WARN("%s", configError.c_str());

  SetLanguage(config_.app.language);
  darkMode_ = ResolveDark(config_.app.theme);
  lastSerialized_ = config_.Serialize();

  if (!CreateMainWindow(instance, showCmd)) return false;

  std::string error;
  if (!d3d_.Initialize(hwnd_, &error)) {
    ::MessageBoxW(nullptr, ToWide(error).c_str(), L"CapView", MB_ICONERROR | MB_OK);
    return false;
  }
  if (!InitImGui()) return false;
  if (!renderer_.Initialize(&d3d_, &error)) {
    ::MessageBoxW(nullptr, ToWide(error).c_str(), L"CapView", MB_ICONERROR | MB_OK);
    return false;
  }

  LoadIdleIcon();

  if (config_.app.startFullscreen) SetFullscreen(true);

  // Straight into the picture if we can; into the settings if we cannot.
  //
  // Beim allerersten Start aber nicht: zehn Reiter voller Optionen sind das
  // Erste, was jemand von diesem Programm sehen sollte, am allerwenigsten. Da
  // steht stattdessen der Willkommensbildschirm, der eine einzige Sache sagt --
  // welche Taste die Einstellungen oeffnet. Ab dem zweiten Start ist es wieder
  // die Abkuerzung, denn dann ist "kein Geraet" keine Begruessung mehr, sondern
  // ein Problem.
  if (firstRun_) {
    CAP_LOG("Erster Start: Willkommensbildschirm statt Einstellungen");
  } else if (config_.active().capture.video.empty()) {
    OpenSettings(T("Noch kein Aufnahmegerät ausgewählt. Wähle unten die Capture-Karte aus.",
                   "No capture device selected yet. Pick your capture card below."));
  } else if (!StartCapture(&error)) {
    OpenSettings(error);
  }
  CaptureAppliedState();
  ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
  // No test on startup. Which encoders work does not change from one run to the
  // next, so the answer is loaded from the config; testing is something the user
  // asks for once, or when the machine changed underneath it.
  LoadCachedEncoders();

  // A previous update left its predecessor lying next to us; it can go now that
  // nothing is running from it.
  Updater::CleanUpPreviousBuild();
  VirtualCamera::CleanUpOldSources();
  if (config_.app.checkUpdatesOnStart) updater_.CheckAsync(true);

  running_ = true;
  return true;
}

void App::Shutdown() {
  if (hwnd_) SaveWindowPlacement();
  // Before anything else: closing the pipes is what makes ffmpeg finalise the
  // container, and that has to happen while the app is still alive.
  StopRecording();
  if (probeThread_.joinable()) probeThread_.join();
  StopSignalWatch();
  settingsHost_.Destroy();
  StopCapture();
  mic_.Stop();
  audio_.Stop();
  renderer_.Shutdown();
  ShutdownImGui();
  d3d_.Shutdown();
  if (hwnd_) {
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ::SetThreadExecutionState(ES_CONTINUOUS);
}

// -------------------------------------------------------------------- window

bool App::CreateMainWindow(HINSTANCE instance, int showCmd) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // we paint every pixel ourselves
  wc.lpszClassName = kWindowClass;
  // Large icon for Alt+Tab, small one for the title bar and taskbar.
  wc.hIcon = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                 ::GetSystemMetrics(SM_CXICON),
                                 ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXSMICON),
                                   ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  if (!::RegisterClassExW(&wc)) {
    ::MessageBoxW(nullptr,
                  ToWide(T("Fensterklasse konnte nicht registriert werden.",
                           "The window class could not be registered."))
                      .c_str(),
                  L"CapView", MB_ICONERROR);
    return false;
  }

  RECT rc = {0, 0, config_.app.windowW, config_.app.windowH};
  ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  // Zwei Gruende, die Stelle nicht zu benutzen, und beide sind echte Fragen.
  // Erstens: es wurde noch nie eine gespeichert. Zweitens: die gespeicherte
  // liegt heute auf keinem Bildschirm mehr -- ein Monitor kann abgezogen worden
  // sein, oder die Anordnung hat sich geaendert, und ein Fenster ausserhalb
  // jeder Arbeitsflaeche waere unerreichbar. Alles andere wird uebernommen,
  // ausdruecklich auch negative Werte: ein Bildschirm links vom Hauptbildschirm
  // hat gar keine anderen.
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  if (config_.app.windowX != AppSettings::kWindowPosUnset &&
      config_.app.windowY != AppSettings::kWindowPosUnset) {
    const RECT want = {config_.app.windowX, config_.app.windowY,
                       config_.app.windowX + width, config_.app.windowY + height};
    if (::MonitorFromRect(&want, MONITOR_DEFAULTTONULL) != nullptr) {
      x = config_.app.windowX;
      y = config_.app.windowY;
    }
  }

  hwnd_ = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                            height, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    ::MessageBoxW(
        nullptr,
        ToWide(T("Fenster konnte nicht erstellt werden.", "The window could not be created."))
            .c_str(),
        L"CapView", MB_ICONERROR);
    return false;
  }

  ApplyWindowDarkMode(hwnd_, darkMode_);
  ::ShowWindow(hwnd_, config_.app.maximized ? SW_SHOWMAXIMIZED : showCmd);
  ::UpdateWindow(hwnd_);
  ApplyWindowFlags();
  return true;
}

void App::SaveWindowPlacement() {
  if (!hwnd_ || fullscreen_) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!::GetWindowPlacement(hwnd_, &wp)) return;
  config_.app.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);

  RECT rc = wp.rcNormalPosition;
  RECT frame = {0, 0, 0, 0};
  ::AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  config_.app.windowX = rc.left;
  config_.app.windowY = rc.top;
  config_.app.windowW = std::max(160L, (rc.right - rc.left) - (frame.right - frame.left));
  config_.app.windowH = std::max(120L, (rc.bottom - rc.top) - (frame.bottom - frame.top));
}

void App::ApplyWindowFlags() {
  if (!hwnd_) return;
  // The driver's dialog is a top-level window of its own with no owner, which is
  // what keeps it out of our message loop -- and also means nothing lifts it
  // above a window that insists on staying on top. So while it is up, we do not.
  const bool top = config_.app.alwaysOnTop && !devicePages_.busy();
  ::SetWindowPos(hwnd_, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::SetFullscreen(bool on) {
  if (!hwnd_ || on == fullscreen_) return;

  if (on) {
    windowedPlacement_.length = sizeof(windowedPlacement_);
    ::GetWindowPlacement(hwnd_, &windowedPlacement_);
    SaveWindowPlacement();

    // Which monitor: the configured one, otherwise the one the window is on.
    RECT target = {};
    bool haveTarget = false;
    if (config_.app.fullscreenMonitor >= 0) {
      std::vector<MonitorInfoEntry> monitors = EnumerateMonitors();
      if (config_.app.fullscreenMonitor < (int)monitors.size()) {
        target = monitors[(size_t)config_.app.fullscreenMonitor].rect;
        haveTarget = true;
      }
    }
    if (!haveTarget) {
      HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi = {};
      mi.cbSize = sizeof(mi);
      if (::GetMonitorInfoW(mon, &mi)) {
        target = mi.rcMonitor;
        haveTarget = true;
      }
    }
    if (!haveTarget) return;

    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    ::SetWindowPos(hwnd_, config_.app.alwaysOnTop ? HWND_TOPMOST : HWND_TOP, target.left,
                   target.top, target.right - target.left, target.bottom - target.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE);
    fullscreen_ = true;
  } else {
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    ::SetWindowPlacement(hwnd_, &windowedPlacement_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    fullscreen_ = false;
    if (cursorHidden_) {
      ::ShowCursor(TRUE);
      cursorHidden_ = false;
    }
    ApplyWindowFlags();
  }
  d3d_.Resize();
}

void App::ToggleFullscreen() {
  SetFullscreen(!fullscreen_);
}

// --------------------------------------------------------------------- ImGui

bool App::InitImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini next to the exe
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  const UINT dpi = ::GetDpiForWindow(hwnd_);
  const float scale = dpi > 0 ? (float)dpi / 96.0f : 1.0f;
  LoadUiFont(17.0f * scale);
  ApplyImGuiTheme(darkMode_, config_.app.accentColor);
  ImGui::GetStyle().ScaleAllSizes(scale);
  uiScale_ = scale;

  if (!ImGui_ImplWin32_Init(hwnd_)) return false;
  if (!ImGui_ImplDX11_Init(d3d_.device(), d3d_.context())) return false;
  imguiReady_ = true;
  return true;
}

void App::ShutdownImGui() {
  if (!imguiReady_) return;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  imguiReady_ = false;
}

void App::ApplyTheme() {
  const bool dark = ResolveDark(config_.app.theme);
  darkMode_ = dark;
  // ApplyImGuiTheme resets the style wholesale, so the DPI scaling has to be
  // reapplied on top of it every time.
  ApplyImGuiTheme(dark, config_.app.accentColor);
  ImGui::GetStyle().ScaleAllSizes(uiScale_);
  ApplyWindowDarkMode(hwnd_, dark);
  if (settingsHost_.created()) settingsHost_.ApplyTheme(dark, config_.app.accentColor);
}

// ------------------------------------------------------------------- capture

bool App::StartCapture(std::string* error) {
  StopCapture();

  const Profile& p = config_.active();
  std::string err;
  if (!capture_.Start(p.capture, &err)) {
    captureState_ = CaptureState::Reconnecting;
    captureError_ = err;
    nextRetryQpc_ = QpcNow() + SecondsToQpc(kRetrySeconds);
    if (error) *error = err;
    return false;
  }

  // Write back the device identity we actually opened, so a card that moved
  // slots keeps working on the next start.
  const VideoDeviceInfo& resolved = capture_.resolvedDevice();
  if (!resolved.id.empty()) {
    config_.active().capture.video.id = resolved.id;
    config_.active().capture.video.name = resolved.name;
  }

  // Dieselbe Ueberlegung fuer die Aufloesung: stand im Profil keine, hat die
  // Karte sie gerade ausgesucht, und niemand sonst weiss welche.
  //
  // Wird sie nicht aufgeschrieben, schreibt sie spaeter jemand anders auf, und
  // zwar teuer. Das Profil bleibt leer, der Abgleich in ApplyProfile sieht im
  // naechsten Bild einen Unterschied zu dem, was gerade gebaut wurde, und baut
  // den Graphen ein zweites Mal auf; wenn danach irgendwann die Einstellungen
  // gezeichnet werden, traegt EnsureValidFormat dieselbe Zahl nach, und es wird
  // ein drittes Mal aufgebaut -- dann mitten im Betrieb, mit schwarzem Bild und
  // Tonabriss. Beim Wechsel des Wuerfels auf 60 Hz stand genau das im Log:
  // 01:34:07,795 und 01:34:08,055 direkt hintereinander, 01:34:34,748
  // sechsundzwanzig Sekunden spaeter.
  //
  // Nur wenn nichts dastand. Eine von Hand erzwungene Aufloesung bleibt stehen,
  // auch wenn die Karte sie gerade nicht hergibt und ersatzweise etwas anderes
  // verbunden hat: sie ist ein Wunsch fuer jeden Start, kein Messwert, und
  // wegzuschreiben waere sie fuer immer weg.
  //
  // Und nur Pixelformat und Groesse. Die Bildrate bleibt, wie sie gewuenscht
  // war -- eine 0 heisst "hoechste verfuegbare" und muss eine 0 bleiben. Wuerde
  // hier die ausgehandelte Zahl hineingeschrieben, waere aus dem Wunsch eine
  // festgenagelte Rate geworden, und zwar die einer einzigen Norm: unter PAL B
  // steht dann 59,94 im Profil, die Karte lehnt jeden Kandidaten damit ab, und
  // im Log stapeln sich "SetFormat mit 59.940 fps abgelehnt".
  if (!config_.active().capture.format.valid() && capture_.connectedFormat().valid()) {
    FormatSel& stored = config_.active().capture.format;
    const double wishedFps = stored.fps;
    stored = capture_.connectedFormat();
    stored.fps = wishedFps;
  }

  // Was hier gebaut wurde, gilt ab jetzt als angewandt -- unabhaengig davon, ob
  // der Aufrufer gleich noch CaptureAppliedState() ruft. Die meisten Wege
  // hierher tun das naemlich nicht (UpdateVideoStandard, ReinitialiseCard, der
  // Wiederverbindungsversuch), und dann steht in applied_ noch das Format von
  // vorhin, obwohl es das schon nicht mehr gibt.
  applied_.format = config_.active().capture.format;

  std::string rendererError;
  renderer_.SetSourceFormat(capture_.format(), &rendererError);
  if (!rendererError.empty()) CAP_WARN("%s", rendererError.c_str());

  captureState_ = CaptureState::Running;
  captureError_.clear();
  retryCount_ = 0;
  secondFieldPending_ = false;
  sawFirstFrame_ = false;
  captureStartQpc_ = QpcNow();

  // The dot crawl filter works on the colour subcarrier, so it has to be told
  // which one this signal carries. The card knows, when it has an analogue
  // decoder at all; without one the default covers the common case.
  const DeviceProbeResult& caps = capture_.capabilities();
  const long standard = caps.availableStandards != 0 ? caps.currentStandard : 0;
  renderer_.SetCarrierSamples(VideoStandardSubcarrierSamples(standard));
  // Unter welcher Zeilenzahl dieses Format ausgesucht wurde. Aus der Karte, weil
  // sie beim Graphenbau gefragt wird und die Einstellung im Profil "automatisch"
  // heissen kann; siehe ReleaseStandardBoundFormat.
  appliedStandardLines_ = VideoStandardLines(standard);

  StartAudio();
  UpdatePowerRequest();
  StartSignalWatch();
  return true;
}

void App::StopCapture() {
  StopSignalWatch();
  capture_.Stop();
  renderer_.DropFrame();
  delayLine_.Clear();
  captureState_ = CaptureState::Idle;
}

void App::StartAudio() {
  audio_.Stop();
  const Profile& p = config_.active();

  DeviceRef input;
  switch (p.capture.audioSource) {
    case AudioSource::None: return;
    case AudioSource::Manual: input = p.capture.audio; break;
    case AudioSource::Embedded:
    default: {
      AudioDeviceInfo found;
      if (FindEmbeddedAudioDevice(capture_.resolvedDevice(), &found)) {
        input = found.ToRef();
      } else {
        CAP_WARN("Kein eingebettetes Audiogerät gefunden, Ton bleibt aus");
        return;
      }
      break;
    }
  }

  std::string err;
  if (!audio_.Start(input, p.audio, &err)) {
    CAP_WARN("Ton konnte nicht gestartet werden: %s", err.c_str());
    Toast(T("Ton konnte nicht gestartet werden: ", "Sound could not be started: ") + err);
  }
  delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
}

// Aufloesung und Bildrate loslassen, wenn die neue Norm eine andere Zeilenzahl
// hat als die, unter der sie ausgesucht wurden.
//
// 625/50 liefert 720x576, 525/60 liefert 720x480 -- eine Groesse, die unter der
// einen Norm gewaehlt wurde, ist unter der anderen falsch. Im Profil steht sie
// trotzdem, und der naechste Graphenbau fordert genau sie wieder an. Am
// GameCube heisst das: zwischen 50 und 60 Hz umschalten, und die Anzeige bleibt
// auf der alten Groesse stehen, bis jemand von Hand ins Aufklappmenue geht.
// Die Liste dort ist da laengst richtig -- sie kommt aus der laufenden Karte --,
// nur wird nichts daraus genommen, solange die alte Wahl noch dasteht. Genau
// dieses Haengenbleiben am Nutzer soll weg.
//
// Zurueck auf 0 heisst "such es dir aus", nicht "nimm irgendwas": die Auswahl
// beim Graphenbau geht ueber das, was die Karte unter der jetzigen Norm
// tatsaechlich anbietet.
//
// Das Pixelformat bleibt stehen, aus demselben Grund wie in ReinitialiseCard:
// es haengt an der Karte, nicht an der Norm. Wer RGB32 ausgesucht hat, will es
// nach dem Umschalten immer noch.
bool App::ReleaseStandardBoundFormat(int newLines) {
  if (appliedStandardLines_ <= 0 || newLines <= 0) return false;
  if (appliedStandardLines_ == newLines) return false;
  FormatSel& f = config_.active().capture.format;
  if (f.width <= 0 && f.height <= 0 && f.fps <= 0.0) return false;
  CAP_LOG("Videonorm: %d statt %d Zeilen -- Auflösung %dx%d @ %.2f wird losgelassen",
          newLines, appliedStandardLines_, f.width, f.height, f.fps);
  f.width = 0;
  f.height = 0;
  f.fps = 0.0;
  f.forced = false;
  return true;
}

// Die Karte von vorn aufmachen, nicht nur den Graphen neu bauen.
//
// Der Unterschied ist, was verworfen wird. Ein Neustart nimmt die Einstellungen
// mit, die gerade dastehen -- und genau die sind das Problem, wenn zwischen dem
// analogen und dem digitalen Eingang umgesteckt wurde: Videonorm und Format
// beschreiben, was an der *vorherigen* Buchse hing. Eine auf PAL festgehaltene
// Karte liefert dann 720x576 bei 50 Hz an einem Eingang, an dem etwas voellig
// anderes anliegt.
//
// Geraet und Eingang bleiben stehen. Die hat jemand ausgesucht; Norm und Format
// hat CapView gemessen oder geraten, und Gemessenes darf weg.
void App::ReinitialiseCard() {
  StopCapture();

  CaptureSettings& c = config_.active().capture;
  const bool hadStandard = c.videoStandard > 0;
  const std::string keptSubtype = c.format.subtype;
  c.videoStandard = -1;  // wieder suchen lassen

  // Aufloesung und Bildrate wieder suchen lassen, das Pixelformat nicht. Eine
  // Karte, die eben noch RGB32 konnte, kann es nach dem Neueinlesen immer
  // noch, und wer es ausgewaehlt hat, will es nicht jedes Mal neu auswaehlen.
  // Kann sie es wirklich nicht mehr, faellt die Auswahl beim Start zurueck.
  c.format = FormatSel{};
  c.format.subtype = keptSubtype;

  // Die Geraeteliste ebenfalls, denn eine umgesteckte Karte kann unter einem
  // anderen Pfad auftauchen als der, den wir uns gemerkt haben.
  settings_.InvalidateDeviceLists();

  CAP_LOG("Karte neu einlesen: Videonorm %s, Auflösung verworfen, Pixelformat %s",
          hadStandard ? "verworfen" : "war schon automatisch",
          keptSubtype.empty() ? "war schon automatisch" : keptSubtype.c_str());

  std::string error;
  if (StartCapture(&error)) {
    Toast(keptSubtype.empty()
              ? T("Karte neu eingelesen. Videonorm und Format stehen wieder auf automatisch.",
                  "Card reinitialised. Video standard and format are back to automatic.")
              : T("Karte neu eingelesen. Videonorm und Auflösung wieder automatisch, "
                  "Pixelformat beibehalten.",
                  "Card reinitialised. Video standard and resolution back to automatic, "
                  "pixel format kept."));
  } else {
    Toast(error);
  }
}

// Wie lange die Anzeige stehen darf, wenn kein Bild ankommt.
//
// Das Video braucht keinen Boden: kommt nichts, gibt es nichts Neues zu zeigen,
// und 200 ms halten den Ruhebildschirm am Leben, ohne Rechenzeit fuer ein
// unveraendertes Bild zu verbrennen.
//
// Die Bedienoberflaeche ist etwas anderes. Sie bewegt sich aus eigener Kraft --
// und das eingebettete Einstellungsfeld wird von genau dieser Schleife
// gezeichnet. Mit dem Boden fuer das Video lief es ohne Aufnahmegeraet mit
// gemessenen 4,7 Bildern in der Sekunde, was sich anfuehlt wie zwei.
//
// Es geht dabei nicht darum, ob ein Signal anliegt, sondern ob etwas auf dem
// Schirm ist, das sich bewegen koennen muss. Liegt ein Signal an, gibt dessen
// Takt ohnehin alles vor und dieser Boden kommt nie zum Tragen.
double App::IdleFloorMs() const {
  const bool embeddedPanel = settings_.isOpen() && !config_.app.settingsSeparateWindow;
  const double now = ImGui::GetTime();
  const bool toastUp = !toastText_.empty() && now - toastStart_ <= 2.5;
  const bool osdUp = now - volumeOsdStart_ <= kVolumeOsdSeconds;
  if (embeddedPanel || cropPick_.active || toastUp || osdUp) return 16.0;
  return 200.0;
}

void App::RestartAll(bool userRequested) {
  std::string error;
  if (StartCapture(&error)) {
    if (userRequested) Toast(T("Aufnahme neu gestartet", "Capture restarted"));
  } else if (userRequested) {
    Toast(error);
  }
}

// ------------------------------------------------------------------ settings

void App::OpenSettings(const std::string& reason) {
  settings_.Open(&config_, reason);
}

void App::CaptureAppliedState() {
  const Profile& p = config_.active();
  applied_.activeProfile = config_.activeProfile;
  applied_.video = p.capture.video;
  applied_.format = p.capture.format;
  applied_.crossbarInput = p.capture.crossbarInput;
  applied_.videoStandard = p.capture.videoStandard;
  applied_.audioSource = p.capture.audioSource;
  applied_.audioIn = p.capture.audio;
  applied_.audioOut = p.audio.output;
  applied_.exclusive = p.audio.exclusive;
  applied_.bufferMs = p.audio.bufferMs;
  applied_.avOffsetMs = p.audio.avOffsetMs;
  applied_.volume = p.audio.volume;
  applied_.mute = p.audio.mute;
  applied_.theme = config_.app.theme;
  applied_.accent = config_.app.accentColor;
  applied_.alwaysOnTop = config_.app.alwaysOnTop;
  applied_.language = config_.app.language;
}

void App::SyncConfigChanges() {
  const Profile& p = config_.active();

  if (config_.app.language != applied_.language) {
    SetLanguage(config_.app.language);
  }
  if (config_.app.theme != applied_.theme || config_.app.accentColor != applied_.accent) {
    ApplyTheme();
  }
  if (config_.app.alwaysOnTop != applied_.alwaysOnTop ||
      devicePages_.busy() != devicePagesWereBusy_) {
    devicePagesWereBusy_ = devicePages_.busy();
    ApplyWindowFlags();
  }

  // Anything that changes what the card is asked to produce needs the graph
  // rebuilt. Everything else is applied without interrupting the picture.
  const bool profileChanged = config_.activeProfile != applied_.activeProfile;
  const bool deviceChanged = !(p.capture.video == applied_.video);
  const bool formatChanged = !p.capture.format.SameFormat(applied_.format) ||
                             std::abs(p.capture.format.fps - applied_.format.fps) > 0.01;
  // A different standard usually means a different number of lines, so the graph
  // has to come up again around it. Automatic is the exception: it has nothing
  // to apply until it has found something, and rebuilds by itself when it does.
  const bool standardChanged = p.capture.videoStandard != applied_.videoStandard &&
                               p.capture.videoStandard > 0;
  if (standardChanged) {
    CAP_LOG("Videonorm gewechselt: %s -> %s, Graph wird neu aufgebaut",
            VideoStandardSettingName(applied_.videoStandard).c_str(),
            VideoStandardSettingName(p.capture.videoStandard).c_str());
    // Vor dem Neubau, denn der liest die Groesse aus dem Profil.
    ReleaseStandardBoundFormat(VideoStandardLines(p.capture.videoStandard));
  }
  // Zurueck auf Automatisch: die Suche faengt von vorne an. Das ist keine
  // Selbstverstaendlichkeit, sondern eine Entscheidung -- ohne diesen Block
  // ueberlebt `standardCandidate_` das Umschalten und die Suche setzt dort
  // fort, wo sie beim Verlassen stand. Das waere zweimal falsch: die
  // Kandidatenliste wird bei jedem Aufruf neu sortiert, ein alter Index zeigt
  // darin auf eine andere Norm als damals, und die vorderen Plaetze -- die
  // Geschwisternorm, die zuletzt gute, die Region -- sind gerade die besten
  // Vermutungen und wuerden uebersprungen. Wer von Hand auf Automatisch
  // zurueckstellt, will die beste Vermutung, nicht die naechste.
  //
  // `standardLastGood_` bleibt absichtlich stehen: dass hier eine Norm schon
  // einmal gehalten hat, ist auch nach dem Umschalten noch wahr.
  //
  // `standardLostQpc_` wird hier *nicht* angefasst. Es steht fuer den Moment,
  // in dem der Lock verloren ging, und daran hat das Umschalten nichts
  // geaendert. Genullt hiesse: die Schonfrist laeuft neu, und die Zeile "Lock
  // auf ... verloren" wird ein zweites Mal geschrieben -- am 30.08. um 08:53:15
  // genau so im Log gestanden, zweimal im Abstand von 148 ms.
  if (p.capture.videoStandard == -1 && applied_.videoStandard != -1) {
    standardCandidate_ = -1;
    standardSweeps_ = 0;
    standardNextTryQpc_ = 0;
    ResetStandardColourCheck();
  }

  if (profileChanged || deviceChanged || formatChanged || standardChanged) {
    std::string error;
    if (!StartCapture(&error)) {
      // Keep the dialog open on the failing setting instead of closing over it.
      if (!settings_.isOpen()) OpenSettings(error);
      Toast(error);
    }
    CaptureAppliedState();
    UpdatePowerRequest();
    return;
  }

  if (p.capture.crossbarInput != applied_.crossbarInput) {
    capture_.SetCrossbarInput(p.capture.crossbarInput);
    // A different input is a different signal on the same format: levels, field
    // structure and where the picture sits all have to be measured again.
    renderer_.ResetAnalysis();
  }

  const bool audioRouteChanged = p.capture.audioSource != applied_.audioSource ||
                                 !(p.capture.audio == applied_.audioIn) ||
                                 !(p.audio.output == applied_.audioOut) ||
                                 p.audio.exclusive != applied_.exclusive;
  if (audioRouteChanged) {
    StartAudio();
  } else if (p.audio.bufferMs != applied_.bufferMs || p.audio.avOffsetMs != applied_.avOffsetMs ||
             p.audio.volume != applied_.volume || p.audio.mute != applied_.mute) {
    audio_.ApplySettings(p.audio);
    delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
  }

  CaptureAppliedState();
}

void App::MaybeSaveConfig() {
  const double now = ImGui::GetTime();

  // Serialising four times a second is cheap and means no field can be
  // forgotten here when one gets added to the config.
  if (now - lastSerializeCheck_ >= 0.25) {
    lastSerializeCheck_ = now;
    std::string current = config_.Serialize();
    if (current != lastSerialized_) {
      lastSerialized_ = std::move(current);
      configDirty_ = true;
      lastConfigChange_ = now;
    }
  }

  // Wait for the user to stop fiddling before touching the disk.
  if (configDirty_ && now - lastConfigChange_ >= 1.0) {
    configDirty_ = false;
    std::string error;
    if (!config_.Save(&error)) CAP_ERR("%s", error.c_str());
  }
}

void App::SaveConfig() {
  // Wherever the settings window stands right now goes into what is about to be
  // written -- otherwise it is only remembered when the mode is toggled, and a
  // window moved and then left alone would come back somewhere else.
  RememberSettingsWindow();
  SaveWindowPlacement();
  std::string error;
  if (!config_.Save(&error)) {
    CAP_ERR("%s", error.c_str());
    Toast(error);
  }
}

void App::SwitchProfile(int index) {
  if (index < 0 || index >= (int)config_.profiles.size()) return;
  if (index == config_.activeProfile) return;
  config_.SetActiveProfile(index);
  std::string error;
  if (StartCapture(&error)) {
    Toast(Format(T("Profil %d: %s", "Profile %d: %s"), index + 1, config_.active().name.c_str()));
  } else {
    Toast(error);
  }
  CaptureAppliedState();
  SaveConfig();
}

// --------------------------------------------------------------------- misc

void App::Toast(const std::string& text) {
  toastText_ = text;
  toastStart_ = ImGui::GetTime();
}

void App::UpdatePowerRequest() {
  const DWORD now = ::GetTickCount();
  if (now - lastPowerPokeTick_ < 30000) return;
  lastPowerPokeTick_ = now;
  if (config_.app.preventSleep && captureState_ == CaptureState::Running) {
    ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
  } else {
    ::SetThreadExecutionState(ES_CONTINUOUS);
  }
}

void App::AdjustVolume(float delta) {
  AudioSettings& a = config_.active().audio;
  a.volume = Clamp(a.volume + delta, 0.0f, 1.0f);
  a.mute = false;
  audio_.ApplySettings(a);
  applied_.volume = a.volume;
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

void App::ToggleMute() {
  AudioSettings& a = config_.active().audio;
  a.mute = !a.mute;
  audio_.ApplySettings(a);
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

// ------------------------------------------------------------------ recording

void App::StartEncoderProbe(bool full) {
  if (probing_.load(std::memory_order_relaxed) || !ffmpeg_.found) return;
  if (probeThread_.joinable()) probeThread_.join();

  probeDone_.store(false, std::memory_order_relaxed);
  probing_.store(true, std::memory_order_relaxed);
  // The thread works on its own copy and publishes it in one go, so the UI
  // thread never sees a half filled structure.
  FfmpegInfo copy = ffmpeg_;
  probeThread_ = std::thread([this, copy, full]() mutable {
    ComScope com(COINIT_MULTITHREADED);
    if (full) {
      ProbeEncoders(&copy);
    } else {
      EnsureUsableEncoder(&copy, config_.record.encoder);
    }
    probeResult_ = std::move(copy);
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
}

void App::CollectEncoderProbe() {
  if (!probeDone_.load(std::memory_order_acquire)) return;
  probeDone_.store(false, std::memory_order_relaxed);
  if (probeThread_.joinable()) probeThread_.join();
  ffmpeg_ = std::move(probeResult_);
  SaveCachedEncoders();
}

std::string App::EncoderSignature() const {
  // The ffmpeg build belongs in here as well as the hardware: a different build
  // can be missing an encoder the last one had, and would then be judged by a
  // result it never produced.
  return GraphicsAdapterSignature() + " | " + ffmpeg_.version;
}

void App::LoadCachedEncoders() {
  if (!ffmpeg_.found) return;
  RecordSettings& rec = config_.record;
  if (rec.encodersAvailable.empty() && rec.encoderProbeSignature.empty()) return;

  if (rec.encoderProbeSignature != EncoderSignature()) {
    CAP_LOG("Encoder-Test verworfen: Hardware oder ffmpeg hat sich geändert");
    rec.encoderProbeSignature.clear();
    rec.encodersAvailable.clear();
    return;
  }

  ApplyCachedProbe(&ffmpeg_, rec.encodersAvailable);
  CAP_LOG("Encoder aus der Konfiguration übernommen: %zu verwendbar",
          rec.encodersAvailable.size());
}

void App::SaveCachedEncoders() {
  if (!ffmpeg_.found || !ffmpeg_.tested) return;
  RecordSettings& rec = config_.record;
  rec.encodersAvailable.clear();
  for (const EncoderInfo& e : ffmpeg_.encoders) {
    if (e.tested && e.available) rec.encodersAvailable.push_back((int)e.id);
  }
  rec.encoderProbeSignature = EncoderSignature();

  // A selection that did not survive the test would silently fail at F9, so it
  // goes back to Auto rather than staying as a trap.
  const EncoderInfo* chosen = ffmpeg_.Find(rec.encoder);
  if (!IsAutoEncoder(rec.encoder) && (!chosen || !chosen->available)) {
    CAP_WARN("Gewählter Encoder ist nicht verfügbar, zurück auf Automatisch");
    rec.encoder = RecordEncoder::Auto;
    Toast(T("Der gewählte Encoder funktioniert hier nicht, zurück auf Automatisch.",
            "The selected encoder does not work here, back to Automatic."));
  }
}

void App::ToggleRecording() {
  if (recorder_.recording()) {
    StopRecording();
  } else {
    StartRecording();
  }
}

void App::StartRecording() {
  if (recorder_.recording()) return;

  // The path was resolved at startup and could have gone stale since -- a
  // portable build on a stick that got unplugged, an antivirus quarantine, or
  // simply someone tidying up. Checking costs one file attribute lookup and
  // turns a confusing "ffmpeg exited with code 1" into an honest answer.
  if (ffmpeg_.found &&
      ::GetFileAttributesW(ToWide(ffmpeg_.path).c_str()) == INVALID_FILE_ATTRIBUTES) {
    CAP_WARN("ffmpeg ist verschwunden: %s", ffmpeg_.path.c_str());
    ffmpeg_ = FfmpegInfo{};
  }
  if (!ffmpeg_.found) {
    ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
    settings_.InvalidateFolderFields();
  }
  if (!ffmpeg_.found) {
    Toast(T("ffmpeg fehlt — in den Einstellungen unter Aufnahme holen.",
            "ffmpeg is missing — fetch it under Recording in the settings."));
    return;
  }
  if (captureState_ != CaptureState::Running || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }

  if (probing_.load(std::memory_order_relaxed)) {
    Toast(T("Encoder werden noch geprüft, gleich nochmal.",
            "Still testing encoders, try again in a moment."));
    return;
  }
  const EncoderInfo* usable = ffmpeg_.Resolve(config_.record.encoder);
  if (!usable || !usable->available) {
    // Nothing tested yet: kick a full probe off in the background rather than
    // freezing the window here, and let the user press again. Full, so the
    // settings end up with the complete list rather than the first answer that
    // happened to work.
    StartEncoderProbe(true);
    Toast(T("Encoder werden geprüft, gleich nochmal.",
            "Testing encoders, try again in a moment."));
    return;
  }

  // The tap has to exist before the recorder pulls from it, and the readback
  // before the first frame is fetched.
  const bool wantAudio = audio_.running() && audio_.tapSampleRate() > 0;
  if (wantAudio) audio_.SetTapEnabled(true);
  renderer_.SetReadbackEnabled(true);

  // Start the microphone before asking it for its rate: the device decides that,
  // and ffmpeg has to be told it on the command line.
  SyncMicrophone(true);
  const bool wantMic = mic_.running() && mic_.sampleRate() > 0;
  if (wantMic) mic_.ResetBuffer();

  // Resolve the folder before handing it over, so a deleted directory is
  // recreated -- or answered with the default -- instead of failing the start.
  RecordSettings settings = config_.record;
  const std::wstring folder =
      ResolveOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
  if (folder.empty()) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  settings.outputFolder = ToUtf8(folder);

  const VideoFormatInfo format = renderer_.sourceFormat();
  std::string error;
  Recorder::AudioSource mainTrack;
  if (wantAudio) {
    mainTrack.sampleRate = audio_.tapSampleRate();
    mainTrack.pull = [this](float* out, size_t frames) { return audio_.ReadTap(out, frames); };
  }
  Recorder::AudioSource micTrack;
  if (wantMic) {
    micTrack.sampleRate = mic_.sampleRate();
    micTrack.pull = [this](float* out, size_t frames) { return mic_.Read(out, frames); };
  }

  // Which picture the writer will be fed, decided before it starts: the wide
  // path only exists while the source is HDR and the setting asks for it.
  renderer_.SetHdrWideWanted(config_.app.recordHdr, virtualCamera_.wantsWide());
  const bool wideRecording = config_.app.recordHdr && renderer_.hdrWideActive();
  recorder_.SetPixelFormat(wideRecording ? VideoRenderer::kHdrReadbackPixelFormat
                                         : VideoRenderer::kReadbackPixelFormat,
                           wideRecording);

  // Mit welcher Bildrate aufgenommen wird -- und die gemeldete ist es nicht.
  //
  // `format.fps` kommt aus `AvgTimePerFrame` des Medientyps, und das Feld haelt
  // nicht, was sein Name verspricht: es steht da, was zuletzt jemand
  // hineingeschrieben hat. Am 30.08. um 14:00 Uhr erzwang das Profil noch ein
  // `720x480 @ 59,94 RGB32` aus den NTSC-Testlaeufen, die Karte lehnte es ab,
  // fiel auf `720x576 YUY2` zurueck -- und liess die 59,94 im Kopf stehen. Die
  // echte Quelle war PAL mit gemessenen 25,00 Bildern.
  //
  // Der Recorder haelt seine Rate gegen die Tonuhr durch und verdoppelt
  // notfalls Bilder, um sie zu erreichen. Die Aufnahme um 14:02 lief deshalb
  // mit 59,94 statt der 50,0, die im Status danebenstanden: rund hundert
  // Bilder wurden gedoppelt, ohne dass eines davon neu war. Falsch abgespielt
  // wird nichts -- die Dauer stimmte auf 10,41 s genau -- aber die Datei nennt
  // eine Bildrate, die die Quelle nie hatte, und traegt sie mit.
  //
  // Der Deinterlacer weiter unten in `Tick` steht vor derselben Frage und
  // beantwortet sie seit jeher so: die gemessene Ankunftsrate gewinnt, wenn es
  // eine gibt. Hier gilt dasselbe -- nur muss das Bobbing dazu, weil dabei aus
  // jedem Halbbild ein Vollbild wird und wirklich doppelt so viele verschiedene
  // Bilder den Renderer verlassen. Gemessene 25,00 und Bobbing ergeben also die
  // 50,0, die die Statuszeile die ganze Aufnahme ueber gezeigt hat; die
  // gemeldeten 59,94 waren an keiner Stelle die Rate von irgendetwas. An einer
  // NTSC-Quelle rechnet dieselbe Zeile 29,97 x 2 = 59,94 -- die native Rate
  // dieser Karte, die 60 gar nicht kann.
  //
  // Verdoppelt wird nach der *Einstellung*, nicht nach dem gerade gemessenen
  // Zustand, und das ist wichtig. `SourceLooksInterlaced` misst bei
  // eingeschalteter Automatik am Bildinhalt, und ein stehendes Menuebild hat
  // keine Kammlinien: die Erkennung sagt dort "progressiv" und kippt erst,
  // wenn sich etwas bewegt. Wer auf dem Menue aufnimmt und dann losfaehrt,
  // haette die Aufnahme sonst auf 25 fps festgenagelt, und der Recorder wirft
  // gegen die Tonuhr jedes zweite Bild weg -- also genau die zweiten
  // Halbbilder, um derentwillen ueberhaupt gebobbt wird. Nach dem Start laesst
  // sich nichts mehr richten, `-r` steht in der ffmpeg-Zeile fest.
  //
  // Die Obergrenze kostet dafuer bei einer wirklich progressiven Quelle mit
  // eingeschalteter Automatik doppelte Bilder in der Datei. Das ist der
  // billigere der beiden Fehler: Platz laesst sich nachtraeglich sparen,
  // weggeworfene Halbbilder nicht.
  double recordFps = format.fps;
  if (const FrameSink* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) recordFps = measured;
  }
  if (config_.active().image.deinterlace != Deinterlace::Off) recordFps *= 2.0;

  const bool ok = recorder_.Start(settings, ffmpeg_, renderer_.outputWidth(),
                                  renderer_.outputHeight(), recordFps, mainTrack, micTrack,
                                  config_.active().audio.micTrackMode, &error);

  if (!ok) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(error);
    return;
  }
  Toast(T("Aufnahme läuft", "Recording"));
}

void App::StopRecording() {
  if (!recorder_.recording()) return;
  const RecordStats stats = recorder_.stats();
  recorder_.Stop();
  renderer_.SetReadbackEnabled(false);
  audio_.SetTapEnabled(false);
  Toast(Format(T("Aufnahme gespeichert (%.0f s)", "Recording saved (%.0f s)"), stats.seconds));
}

std::wstring App::ResolveOutputFolder(std::string* configured, const std::wstring& fallback) {
  std::wstring folder = configured->empty() ? fallback : ToWide(*configured);
  while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();

  // Deleting the folder between two recordings is normal housekeeping, so the
  // first answer is simply to make it again.
  if (EnsureFolder(folder)) return folder;

  // It cannot be created either -- an unplugged drive, a path that is no longer
  // writable. Rather than refuse, fall back to the default and clear the custom
  // path so the settings show where the files are actually going now.
  CAP_WARN("Ordner nicht verfügbar: %s", ToUtf8(folder).c_str());
  if (!configured->empty() && EnsureFolder(fallback)) {
    Toast(T("Ordner nicht verfügbar, Standardordner wird benutzt.",
            "Folder not available, using the default folder."));
    configured->clear();
    settings_.InvalidateFolderFields();
    return fallback;
  }
  return {};
}

void App::SyncMicrophone(bool aboutToRecord) {
  const AudioSettings& a = config_.active().audio;

  // Held open only while it is actually being used: during a recording, and
  // while the settings are open so the level meter means something. Keeping a
  // microphone open the rest of the time would light up the Windows privacy
  // indicator for no reason and tell the user something untrue.
  const bool wanted =
      a.micEnabled && (aboutToRecord || recorder_.recording() || settings_.isOpen());

  // Changing the device is a fresh start, and clears a previous failure.
  if (!(micApplied_ == a.micDevice)) {
    micApplied_ = a.micDevice;
    micAttempted_ = false;
    micFailed_ = false;
    if (mic_.running()) mic_.Stop();
  }

  if (wanted && !mic_.running() && !micFailed_) {
    std::string error;
    micAttempted_ = true;
    if (!mic_.Start(a.micDevice, &error)) {
      // Not fatal: the recording still happens, just without the second track.
      // Latched, because retrying every frame would fill the log and the screen
      // with the same message sixty times a second.
      micFailed_ = true;
      if (!error.empty()) Toast(error);
    }
  } else if (!wanted && mic_.running()) {
    mic_.Stop();
    micAttempted_ = false;
  }
  mic_.SetGain(a.micGain);
}

void App::OpenFolderInExplorer(std::string* configured, const std::wstring& fallback) {
  const std::wstring folder = ResolveOutputFolder(configured, fallback);
  if (folder.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::DrawToolbarStrip() {
  ToolbarState state;
  state.recording = recorder_.recording();
  state.recordSeconds = state.recording ? recorder_.stats().seconds : 0.0;
  state.muted = config_.active().audio.mute;
  state.volume = config_.active().audio.volume;
  state.canRecord = captureState_ == CaptureState::Running && renderer_.hasFrame();

  const ToolbarResult result = DrawToolbar(state, config_.app.accentColor);
  if (result.volume >= 0.0f) {
    config_.active().audio.volume = result.volume;
    if (config_.active().audio.mute) config_.active().audio.mute = false;
    audio_.ApplySettings(config_.active().audio);
    ShowVolumeOsd();
  }

  switch (result.action) {
    case ToolbarAction::ToggleRecording: ToggleRecording(); break;
    case ToolbarAction::Screenshot: RequestScreenshot(); break;
    case ToolbarAction::Settings: OpenSettings({}); break;
    case ToolbarAction::OpenRecordFolder:
      OpenFolderInExplorer(&config_.record.outputFolder, DefaultRecordFolder());
      break;
    case ToolbarAction::OpenScreenshotFolder:
      OpenFolderInExplorer(&config_.record.screenshotFolder, DefaultScreenshotFolder());
      break;
    case ToolbarAction::ToggleMute: ToggleMute(); break;
    case ToolbarAction::Hide: config_.app.showToolbar = false; break;
    default: break;
  }
}

void App::WriteScreenshot(bool includeUi) {
  RecordSettings& rec = config_.record;

  std::vector<uint8_t> pixels;
  std::vector<uint16_t> halfPixels;
  int width = 0, height = 0, halfStride = 0;
  bool wide = false;
  std::string note;

  if (includeUi) {
    // The whole window as it stands, which is what "with the interface" has to
    // mean: the picture scaled into the window, the bar, the overlays. Only
    // possible in eight bit -- an HDR back buffer is scRGB float, and the
    // conversion out of it is not something to guess at. There, the picture
    // itself is saved instead and the message says so.
    if (!d3d_.GrabBackBuffer(&pixels, &width, &height)) {
      includeUi = false;
      note = T(" (ohne Oberfläche, HDR-Ausgabe)", " (without interface, HDR output)");
    }
  }

  if (!includeUi) {
    // Keeping the range only means anything when there is a range to keep, so
    // the setting and the source both have to say so. Otherwise this is an
    // ordinary screenshot and takes the ordinary path.
    wide = config_.app.screenshotHdr &&
           renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
    if (wide) {
      if (!renderer_.GrabStillHalf(&halfPixels, &width, &height, &halfStride)) {
        Toast(T("Kein Bild zum Speichern.", "No picture to save."));
        return;
      }
    } else if (!renderer_.GrabStill(&pixels, &width, &height)) {
      Toast(T("Kein Bild zum Speichern.", "No picture to save."));
      return;
    }
  }

  const std::wstring folder =
      ResolveOutputFolder(&rec.screenshotFolder, DefaultScreenshotFolder());
  const std::wstring path =
      folder.empty() ? std::wstring()
      : wide          ? MakeHdrScreenshotPath(folder, config_.app.hdrShotFormat)
                      : MakeScreenshotPath(folder, rec.screenshotFormat);
  if (path.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    CAP_ERR("Screenshot: Ordner nicht verfügbar: %s", ToUtf8(folder).c_str());
    return;
  }

  std::string error;
  const bool ok =
      !wide ? SaveScreenshot(path, pixels.data(), width, height, rec.screenshotFormat,
                             rec.jpegQuality, &error)
      : config_.app.hdrShotFormat == HdrShotFormat::Avif
          ? SaveScreenshotAvif(path, ToWide(ffmpeg_.path), halfPixels.data(), width, height,
                               halfStride, config_.app.paperWhiteNits, &error)
          : SaveScreenshotHdr(path, halfPixels.data(), width, height, halfStride,
                              config_.app.paperWhiteNits, &error);
  if (!ok) {
    Toast(T("Screenshot fehlgeschlagen: ", "Screenshot failed: ") + error);
    CAP_ERR("Screenshot fehlgeschlagen: %s", error.c_str());
    return;
  }

  // The file name, not the whole path: the path is long, and the point of the
  // message is "it worked and it is called this".
  const size_t slash = path.find_last_of(L'\\');
  Toast(T("Screenshot: ", "Screenshot: ") +
        ToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1)) + note);
  CAP_LOG("Screenshot gespeichert: %s (%dx%d)", ToUtf8(path).c_str(), width, height);
}

// ------------------------------------------------------------- crop picker
//
// Typing four numbers and checking the result is a loop nobody enjoys, so the
// edges can be dragged on the picture instead. While picking, the crop is set
// to zero so the whole frame is visible -- otherwise you would be cropping an
// already cropped image and the numbers would compound.

// How long the lock has to be missing before anything is changed, and how long
// a freshly set standard is given to prove itself. Both err on the generous
// side: a console being switched between 50 and 60 Hz drops out for a moment on
// its own, and reacting to that would mean changing the card's setting every
// time somebody opens a menu.
static const double kStandardLostSeconds = 1.5;
// Gemessen am 30.08.2026 an einer SA7160, sechs echte Locks: 0,78 / 0,72 /
// 0,76 s bei frisch eingeschalteter Quelle und 0,30 / 0,39 / 0,55 s bei bereits
// anliegendem Signal. 1,5 s ist gut das Doppelte des schlechtesten Wertes --
// eng genug, dass ein voller Durchlauf von rund zwanzig auf zwoelf Sekunden
// faellt, und weit genug, dass keiner dieser Locks knapp geworden waere.
//
// Der eine Ausreisser von 2,00 s aus einer frueheren Messung ist bewusst nicht
// abgedeckt: dort lief die Konsole selbst noch hoch. Dieser Fall verliert den
// Durchlauf ohnehin, weil zu dem Zeitpunkt auch die *richtige* Norm nicht
// einrastet -- gerettet wird er nicht von einem laengeren Fenster, sondern vom
// naechsten Durchlauf.
//
// Von 1,5 auf 1,25 s heruntergesetzt, und zwar nicht, weil enger gerechnet
// wird, sondern weil das Gemessene kleiner geworden ist. In jedem der zehn
// Werte oben steckt das Warten auf zwei frische Messwerte des Wachtthreads
// mit drin, und das waren bei 250 ms Takt bis zu 0,5 s davon; bei 100 ms sind
// es bis zu 0,2 s.
//
// Hier stand zuerst 1,0 s, hergeleitet aus genau dieser Rechnung: der
// schlechteste alte Wert von 0,88 s muesste auf gut 0,6 s fallen, und 1,0 s
// stuende dazu wie vorher 1,5 s zu 0,88 s. Die erste Messung mit dem neuen
// Takt hat das nicht bestaetigt -- am 31.08. um 03:09:16, ein verlorener Lock
// auf PAL B und der Wechsel zurueck auf PAL 60: **0,73 s**. Gegen 1,0 s ist
// das Faktor 1,37, wo vorher 1,7 stand, und da der Poll-Anteil jetzt bis zu
// 0,2 s betraegt, liegt der schlechteste Fall dieser Art bei rund 0,83 s.
//
// 1,25 s stellt das Verhaeltnis wieder her (Faktor 1,5 auf 0,83 s) und behaelt
// den groesseren Teil des Gewinns: die Frist faellt um ein Sechstel, das
// Warten davor um bis zu 0,3 s, und die wirklich teuren Faelle nimmt ohnehin
// die Kuerzung bei Bildmangel weiter unten weg. Eine Vorhersage durch eine
// Messung ersetzt, nicht durch eine zweite Vorhersage.
//
// Nachpruefbar bleibt es an derselben Zeile: "Videonorm automatisch gefunden:
// ... (Lock nach %.2f s)". Bleiben diese Werte unter 0,85 s, stimmt die
// Rechnung; kommen sie in die Naehe von 1,25, gehoert die Frist zurueck auf
// 1,5.
static const double kStandardSettleSeconds = 1.25;
// Die vorgezogenen Kandidaten nach einem verlorenen Lock -- der Partner der
// zuletzt guten Norm und sie selbst -- bekommen deutlich mehr. Eine Konsole,
// die gerade neu startet oder von 50 auf 60 Hz umschaltet, braucht ein paar
// Sekunden, bis ueberhaupt wieder etwas Stabiles aus ihr herauskommt; mit
// 0,6 Sekunden waere man laengst weitergezogen, wenn sie so weit ist.
//
// Von 3,0 auf 2,0 s, und das ist der doppelte Sprung: die Frist selbst faellt
// um eine Sekunde, und sie faellt ganz weg, sobald gar kein Bild mehr ankommt
// -- siehe die Kuerzung in UpdateVideoStandard. Was bleibt, ist der Fall, fuer
// den sie gedacht war: es kommen Bilder, sie rasten nur noch nicht ein. Zwei
// Sekunden sind auch dann noch das Doppelte einer gewoehnlichen Frist, und die
// Konsole, die laenger braucht, wird ohnehin erst von der naechsten Runde
// eingefangen.
static const double kStandardPreferredSeconds = 2.0;
// After a full pass with nothing locking, there is probably no signal at all --
// the console is off. Stop poking the card and look again in a while.
static const double kStandardBackoffSeconds = 6.0;
// Ab wann "es kommt kein Bild mehr" heisst, dass die eingestellte Norm das
// anliegende Signal nicht dekodieren kann. Grosszuegig gegen den Neuaufbau
// des Graphen gewaehlt: waehrenddessen kommt naturgemaess nichts, und ein
// frisch gestarteter Graph liefert an der SA7160 nach rund 0,3 s wieder --
// gemessen am 31.08. um 02:29:37, Neubau in 118 ms. Zwei Sekunden sind das
// Sechsfache davon und trotzdem kurz genug, dass ein wieder angestecktes Kabel
// nicht sekundenlang ins Leere laeuft.
static const double kStandardStarvedSeconds = 2.0;

// Bis die Karte nach einem Normwechsel wieder saubere Bilder liefert. Die
// Zeilenzahl bleibt gleich, es geht nur um den Farb-PLL, deshalb kurz.
//
// Dass die Frist ueberhaupt etwas tut, ist am 30.08. nachgestellt worden,
// indem sie auf null gesetzt wurde: dann misst jeder Kandidat den
// Umschaltmoment mit und wird zu seinem Vorgaenger hin verschmiert -- PAL N
// 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den Tiefen 0,246 statt
// 0,271 bis 0,410. Wie weit sie darueber hinaus Reserve hat, ist nicht
// gemessen.
//
// Sie war frueher gratis: der Rundgang wartet nach einem Normwechsel ohnehin
// auf zwei frische Messwerte des Wachtthreads, und bei 250 ms Takt dauerte das
// ungefaehr ebenso lange. Seit der Takt bei 100 ms liegt -- siehe
// kSignalPollNaps -- ist sie es nicht mehr, sondern der laengste Einzelposten
// eines Kandidaten. Verkuerzt wird sie trotzdem nicht: was sie abwartet, ist
// der Farb-PLL der Karte, und der wird nicht schneller, weil wir oefter
// hinsehen.
//
// Sie steht hier statt in VerifyStandardColour, weil sie inzwischen von zwei
// Seiten gebraucht wird: auch der Neubau des Graphen nach einem gefundenen
// Lock legt sie an, damit die erste Messung der neuen Norm hinter dem Umbau
// beginnt statt darueber hinweg zu mitteln. Siehe UpdateVideoStandard.
static const double kColourSettleSeconds = 0.5;

// Wie oft der Wachtthread den Decoder nach seinem Lock fragt, in Zehnteln
// einer Zehntelsekunde -- er schlaeft in 10-ms-Haeppchen, damit das Beenden
// nicht darauf warten muss.
//
// Das ist der Boden unter allen Fristen dieser Datei, und er war lange
// unsichtbar. Bevor ueber eine frisch gesetzte Norm geurteilt werden darf,
// wartet `UpdateVideoStandard` auf zwei frische Messwerte -- bei 250 ms Takt
// sind das 0,25 bis 0,5 s, in denen nichts gemessen wird, sondern nur gewartet.
// Genau diese Spanne steckt in jedem gemessenen "Lock nach"-Wert mit drin:
// zehn davon aus drei Sitzungen liegen zwischen 0,32 und 0,88 s, und ein
// gutes Drittel davon ist dieses Warten.
//
// Bei 100 ms schrumpft es auf 0,1 bis 0,2 s. Das ist die Voraussetzung dafuer,
// dass die Fristen darunter kuerzer werden koennen, ohne enger zu werden --
// gekuerzt wird das Warten, nicht die Messung. Der Preis sind zwei
// Property-Gets auf dem Decoder zehnmal statt viermal je Sekunde, auf einem
// eigenen Thread, weit weg vom Bildweg.
//
// Der Rundgang profitiert genauso: auch er wartet je Kandidat auf zwei frische
// Messwerte, fuenfmal je Runde.
static const int kSignalPollNaps = 10;

void App::StartSignalWatch() {
  StopSignalWatch();
  IBaseFilter* filter = capture_.filter();
  if (!filter) return;

  // The thread keeps its own reference, so tearing the graph down does not pull
  // the object out from under it; the worst that happens is that it asks a
  // filter that is no longer running, and gets told so.
  ComPtr<IBaseFilter> held = filter;
  signalWatchRun_.store(true, std::memory_order_relaxed);
  signalWatch_ = std::thread([this, held]() {
    // The same apartment the graph lives in, so the pointer is usable as it is.
    ComScope com(COINIT_MULTITHREADED);
    while (signalWatchRun_.load(std::memory_order_relaxed)) {
      // Beides aus demselben Durchgang: was eingestellt ist und ob es haelt.
      // Getrennt gefragt koennten die zwei aus verschiedenen Momenten stammen,
      // und genau daraus entsteht die Anzeige, die eine Norm als eingerastet
      // meldet, waehrend laengst eine andere auf der Karte steht.
      signalStandard_.store(CurrentVideoStandard(held.Get()), std::memory_order_relaxed);
      signalLocked_.store(VideoStandardLocked(held.Get()), std::memory_order_relaxed);
      signalSeq_.fetch_add(1, std::memory_order_release);
      // Split into short naps so shutdown does not have to wait for the interval.
      for (int i = 0; i < kSignalPollNaps && signalWatchRun_.load(std::memory_order_relaxed);
           ++i) {
        ::Sleep(10);
      }
    }
  });
}

void App::StopSignalWatch() {
  signalWatchRun_.store(false, std::memory_order_relaxed);
  if (signalWatch_.joinable()) signalWatch_.join();
  signalLocked_.store(-1, std::memory_order_relaxed);
  signalStandard_.store(0, std::memory_order_relaxed);
}

int App::PollSignalLocked() {
  if (!capture_.running()) return -1;
  return signalLocked_.load(std::memory_order_relaxed);
}

SettingsWindow::StandardSearch App::StandardSearchDisplay() const {
  using S = SettingsWindow::StandardSearch;
  // Ohne laufende Aufnahme oder an einer digitalen Quelle gibt es keine Suche,
  // und der Wachthread laeuft dann auch gar nicht.
  if (captureState_ != CaptureState::Running || !SourceIsAnalogue()) return S::Off;
  // Eine von Hand gesetzte Norm wird nicht gesucht. Sie ist eingestellt, auch
  // wenn nichts einrastet -- dass sie nicht haelt, sagt die Zeile darunter.
  if (config_.active().capture.videoStandard != -1) return S::Off;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return S::Off;  // noch nichts gemessen
  if (locked == 1) {
    // Eingerastet -- aber die Sache ist erst entschieden, wenn auch die Farbe
    // stimmt. Waehrend des Gegenversuchs steht die *andere* Norm auf der Karte,
    // und ohne diesen Zustand saehe das aus wie ein Ergebnis, das sich von
    // selbst wieder aendert.
    return !colourCandidates_.empty() ? S::Colour : S::Off;
  }
  // Kein Lock: gesucht wird -- es sei denn, gerade laeuft die Pause zwischen
  // zwei Runden.
  //
  // Hier stand nur `standardSweeps_ >= 1`, und das war die falsche Frage. Nach
  // der ersten erfolglosen Runde geht der Zaehler nie wieder auf null, die
  // Zeile blieb also fuer immer bei "Suche pausiert" -- auch waehrend der
  // zweiten, dritten, zehnten Runde, in der sehr wohl gesucht wird. Sichtbar
  // wurde es daran, dass die genannte Norm munter weiterlief, waehrend
  // danebenstand, es sei pausiert: am 30.08. um 13:31 Uhr zeigte der Dialog
  // "Scanning paused, card set to PAL M", und PAL M war Platz 6 einer gerade
  // laufenden Runde. Die Pause ist der Zustand *zwischen* den Runden, und den
  // erkennt man nicht am Zaehler, sondern daran, dass gerade kein Kandidat
  // gesetzt ist.
  return standardSweeps_ >= 1 && standardCandidate_ < 0 ? S::Paused : S::Trying;
}

// Ob der Wachthread ueberhaupt etwas zu beobachten hat.
//
// Er fragt den Analogdecoder zehnmal in der Sekunde nach seinem Lock. An einem
// digitalen Eingang haengt der Decoder gar nicht im Signalweg -- seine Antwort
// ist dort bedeutungslos, und alles, was auf ihr aufbaut, soll sie deshalb
// auch nicht bekommen: weder die automatische Normensuche noch die kuerzere
// Geduld bei flachem Bild in `HoldingSignal`.
//
// Es ist dieselbe Frage, die den Videonorm-Picker aus den Einstellungen und
// aus dem Rechtsklickmenue nimmt. Was nirgends einstellbar ist, darf auch
// nicht im Hintergrund an der Karte drehen.
void App::UpdateSignalWatch() {
  const bool want = captureState_ == CaptureState::Running && SourceIsAnalogue();
  const bool have = signalWatch_.joinable();
  if (want == have) return;

  if (want) {
    StartSignalWatch();
    return;
  }
  StopSignalWatch();
  // Eine halb gelaufene Suche darf nicht liegen bleiben. Kommt spaeter doch
  // eine analoge Quelle, faengt sie von vorne an statt in der Mitte der Liste.
  standardCandidate_ = -1;
  standardLostQpc_ = 0;
  standardSweeps_ = 0;
  standardNextTryQpc_ = 0;
  // Auch die Erinnerung. Wer von digital zurueck auf analog wechselt, haengt
  // etwas anderes an -- die alte Norm zu bevorzugen waere dann ein Rat aus
  // einem anderen Leben.
  standardLastGood_ = 0;
  ResetStandardColourCheck();
}

void App::UpdateVideoStandard() {
  const Profile& profile = config_.active();
  if (profile.capture.videoStandard != -1) return;  // not our job
  // Und auch dann nicht, solange die Umstellung noch nicht angekommen ist.
  //
  // Das Einstellungsfenster schreibt beim Klick sofort in `config_`, aber
  // `SyncConfigChanges` -- und damit der Ruecksetzer der Suchposition -- laeuft
  // erst am Ende des naechsten Bildes des *Hauptfensters*. In `Tick` steht
  // diese Funktion davor. Wer also von einer festen Norm auf Automatisch
  // zurueckstellt, kaeme genau einmal hier durch, waehrend `standardCandidate_`
  // noch auf dem alten Stand steht: die Suche schriebe der Karte den naechsten
  // Kandidaten der *alten* Position, und erst danach faengt sie richtig von
  // vorne an. Am 30.08. um 13:32:56 so im Log gestanden -- `PAL N` (Platz 6 der
  // abgebrochenen Runde), 679 ms spaeter dann `PAL B` (Platz 0).
  if (applied_.videoStandard != profile.capture.videoStandard) return;
  if (captureState_ != CaptureState::Running) return;
  // Ein digitaler Eingang hat keine Videonorm, die man suchen koennte. Ohne
  // Wachthread stuende unten ohnehin -1 und die Funktion kehrte um; das hier
  // sagt es aber an der Stelle, an der es gemeint ist.
  if (!SourceIsAnalogue()) return;

  // Einmal je Sitzung festhalten, womit die Suche anfaengt. Das ist die erste
  // Frage, wenn die Automatik danebenliegt -- und die Antwort sagt zugleich, ob
  // die Region stimmt, ohne dass die (uebersetzte) Regionsbezeichnung in dieses
  // unuebersetzte Log muesste. Der Normname ist in jeder Sprache derselbe.
  static bool searchStartLogged = false;
  if (!searchStartLogged) {
    const std::vector<long> plan = AutoStandardCandidates(
        capture_.capabilities().availableStandards, config_.app.videoRegion, 0, nullptr);
    if (!plan.empty()) {
      searchStartLogged = true;
      const char* how = "von Hand gesetzt";
      if (config_.app.videoRegion == VideoRegion::Auto) how = "automatisch aus Windows";
      if (config_.app.videoRegion == VideoRegion::None) how = "keine, nur die allgemeine Folge";
      CAP_LOG("Videonorm-Suche: Region %s, zuerst %s", how,
              VideoStandardName(VideoStandardIndexOf(plan.front())));
    }
  }

  const int64_t now = QpcNow();

  // Kommt ueberhaupt noch ein Bild an? Vor allen Ausstiegen weiter unten, damit
  // diese Uhr auch dann laeuft, wenn die Funktion an anderer Stelle umkehrt.
  //
  // Das ist der Unterschied zwischen den beiden Arten von "kein Signal", die
  // sich sonst gleich anfuehlen. Ist die Quelle aus, liefert die Karte weiter
  // Bilder, nur eben leere -- daraus wird das Urteil Flat, und dann ist Parken
  // richtig. Steht die Karte dagegen auf einer Norm mit der falschen
  // Zeilenzahl, kommt gar nichts mehr; das letzte Urteil bleibt als Flat
  // stehen, obwohl es ein Bild von vorhin beschreibt.
  //
  // Genau das hat sich am 31.08.2026 aufgehaengt: um 02:02:46 stand die Karte
  // auf NTSC 4.43 (525/60), am Eingang lag wieder ein 625/50-Signal, und der
  // Graph lieferte kein einziges Bild mehr. Der Parkzweig war schon auf seinem
  // Ziel angekommen, tat also nichts, und kehrte um -- alle 1,5 Sekunden, 112
  // Sekunden lang, bis von Hand neu gestartet wurde. Das Bildalter stieg dabei
  // von 633 ms auf 112352 ms, und das ist der Messwert, an dem es haengt.
  //
  // Wenn nichts mehr ankommt, ist Weitersuchen keine Stoerung, sondern das
  // Einzige, was das Bild zurueckholen kann.
  // Gemessen wird am Alter des letzten Bildes selbst, nicht daran, wann uns
  // zum ersten Mal aufgefallen ist, dass keines mehr kommt. Der Unterschied
  // sind volle kNoSignalSeconds, die sonst vor der eigentlichen Frist noch
  // einmal verstreichen -- am 31.08. um 02:29:44 standen dadurch 4,3 s im Log,
  // wo 3,0 gemeint waren. Nur solange die Senke ueberhaupt schon einmal ein
  // Bild gesehen hat; danach ist ihr Alter die Messung. Vorher -- frischer
  // Graph, noch nichts angekommen -- bleibt nur, ab dem ersten Hinsehen zu
  // zaehlen.
  const FrameSink* frameSink = capture_.sink();
  const double frameAgeMs = frameSink ? frameSink->stats().lastArrivalAgeMs : -1.0;
  bool starved = false;
  if (frameAgeMs >= 0.0) {
    standardStarvedSinceQpc_ = 0;
    starved = frameAgeMs >= kStandardStarvedSeconds * 1000.0;
  } else {
    if (standardStarvedSinceQpc_ == 0) standardStarvedSinceQpc_ = now;
    starved = QpcToSeconds(now - standardStarvedSinceQpc_) >= kStandardStarvedSeconds;
  }
  if (!starved) standardStarvedLogged_ = false;

  // Two fresh readings since the standard was last changed, so what is being
  // judged is the standard that is actually set.
  if (signalSeq_.load(std::memory_order_acquire) - standardSeqAtSet_ < 2) return;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return;  // no analogue decoder, or nothing measured yet

  if (locked == 1) {
    // Settled. Whatever is set is right, and the search starts from scratch if
    // it is ever needed again.
    standardLostQpc_ = 0;
    standardSweeps_ = 0;
    // Woran nach dem naechsten Aussetzer zuerst gedacht wird. Auch dann
    // gemerkt, wenn gar nicht gesucht wurde: eine Norm, die von selbst
    // eingerastet ist, ist genauso ein guter Hinweis wie eine gefundene.
    standardLastGood_ = capture_.currentStandard();
    if (standardCandidate_ >= 0) {
      // A candidate just proved itself. The line count may have changed with
      // it, so the graph has to be rebuilt around the new format.
      standardCandidate_ = -1;
      const long settled = standardLastGood_;
      // Mit der Einfangzeit daneben. Sie sagt, ob die Wartefrist gereicht hat
      // oder ob sie nur knapp gereicht hat -- und was eine Norm braucht, die
      // die Zeilenfrequenz wirklich neu einfangen musste.
      CAP_LOG("Videonorm automatisch gefunden: %s (Lock nach %.2f s)",
              VideoStandardName(VideoStandardIndexOf(settled)),
              standardSetQpc_ != 0 ? QpcToSeconds(now - standardSetQpc_) : 0.0);
      standardSetQpc_ = 0;
      // Die Zeilenzahl kann sich mit der Norm geaendert haben, und dann passt
      // die gemerkte Groesse nicht mehr. Vor dem Neubau, der sie sonst wieder
      // anfordert.
      ReleaseStandardBoundFormat(VideoStandardLines(settled));
      std::string error;
      if (StartCapture(&error)) {
        // The toast speaks the same vocabulary as the picker, so it names the
        // group. The exact variant stays in the log and under the picker.
        Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                     VideoStandardPickerName(settled).c_str()));
      } else {
        Toast(error);
      }
      // Und die Farbmessung von vorn, hinter dem Umbau.
      //
      // Das hat gefehlt, und es war teuer. Die laufende Messung mittelt ueber
      // rund drei Sekunden; wird der Graph mittendrin neu gebaut, steht in ihr
      // die alte Norm, die alte Groesse und der Umbau selbst. Genau das ist am
      // 31.08. um 03:09 passiert: PAL 60 mass unmittelbar nach dem Wechsel von
      // 720x576 auf 720x480 eine Farbe von 0,005 -- gemessen wurde in
      // Wahrheit das Nichts davor. Damit galt die frisch gefundene, richtige
      // Norm als zweifelhaft, und es lief ein voller Rundgang ueber vier
      // Normen los, der zu allem Ueberfluss verworfen wurde, weil die zweite
      // Messung derselben Norm dann 0,258 ergab. Drei Sekunden spaeter stand
      // ohne einen einzigen Normwechsel "PAL 60 hat deutlich Farbe (0,149)"
      // im Log -- die Antwort war die ganze Zeit da, sie wurde nur zu frueh
      // gefragt. Rund sechseinhalb der elfeinhalb Sekunden zwischen
      // Signalverlust und Urteil gingen dafuer drauf.
      //
      // Die Frist darunter ist dieselbe wie im Rundgang und aus demselben
      // Grund da: sie setzt die Messung an ihrem Ende noch einmal zurueck, so
      // dass das Fenster sicher hinter dem Umbau beginnt und nicht davor.
      ResetStandardColourCheck();
      colourSettleUntilQpc_ = now + SecondsToQpc(kColourSettleSeconds);
    }
    VerifyStandardColour(now);
    return;
  }

  // No lock.
  const long available = capture_.capabilities().availableStandards;
  if (standardLostQpc_ == 0) {
    standardLostQpc_ = now;
    if (standardLastGood_ > 0) {
      int planned = 0;
      const std::vector<long> plan =
          AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &planned);
      if (planned > 0) {
        CAP_LOG("Videonorm: Lock auf %s verloren, zuerst wird %s versucht",
                VideoStandardName(VideoStandardIndexOf(standardLastGood_)),
                VideoStandardName(VideoStandardIndexOf(plan.front())));
      }
    }
    return;
  }
  if (QpcToSeconds(now - standardLostQpc_) < kStandardLostSeconds) return;

  // Eine Frist, die auf ein Bild wartet, das nicht kommt, ist kein Zuhoeren.
  //
  // Beim Setzen des Kandidaten wird entschieden, wieviel Zeit er bekommt, und
  // an dem Punkt ist `starved` fast immer noch falsch: das letzte Bild ist
  // erst ein, zwei Sekunden alt, die Schwelle noch nicht erreicht. Die
  // Unterscheidung stand also da, wo sie nichts entscheiden konnte -- am
  // 31.08. um 02:43:39 bekam PAL B seine vollen kStandardPreferredSeconds und
  // lief sie voll aus, obwohl schon beim Setzen seit sieben Sekunden kein Bild
  // mehr angekommen war. 02:43:42,6 "nach 3,00 s ohne Lock verworfen",
  // 02:43:43,2 wieder Bild: die halbe Wartezeit war diese eine Frist.
  //
  // Also wird die Frist nachtraeglich gekuerzt, sobald das Aushungern
  // feststeht. Gekuerzt und nicht gestrichen: der Kandidat braucht seine
  // kStandardSettleSeconds, um seinen Lock ueberhaupt zeigen zu koennen, und
  // dass gerade kein Bild ankommt, sagt darueber nichts -- eine Norm mit der
  // anderen Zeilenzahl liefert grundsaetzlich nichts in einen Graphen der
  // alten Groesse, und trotzdem kann genau sie die richtige sein. Was hier
  // wegfaellt, ist allein die zusaetzliche Geduld der vorgezogenen Plaetze.
  // Die ist fuer eine Konsole gedacht, die noch hochfaehrt, und die liefert
  // dabei Bilder -- nur noch keine stabilen.
  if (starved && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    const int64_t shortened = standardSetQpc_ + SecondsToQpc(kStandardSettleSeconds);
    if (shortened < standardNextTryQpc_) standardNextTryQpc_ = shortened;
  }
  if (now < standardNextTryQpc_) return;

  int preferred = 0;
  const std::vector<long> candidates =
      AutoStandardCandidates(available, config_.app.videoRegion, standardLastGood_, &preferred);
  if (candidates.empty()) return;

  // Bei totem Eingang wird nicht weitergeschaltet -- und vor allem gilt nichts
  // als geprueft, was hier gemessen wurde.
  //
  // Das ist ein gemessener Fehler, kein gedachter: am 30.08.2026 wurde der
  // GameCube ausgeschaltet und wieder eingeschaltet, und in dem Fenster ohne
  // Signal lief die Suche weiter. Als das Bild zurueckkam, war PAL B gerade
  // durch, und die Suche stand bei den 525/60-Normen -- sie lief 9,6 s lang
  // vier Kandidaten gegen ein anliegendes 625/50-Signal, die richtige Antwort
  // hatte sie kurz vorher schon in der Hand gehabt und weggeworfen. Eine Norm
  // gegen kein Signal zu pruefen ist keine Pruefung; sie faellt zwangslaeufig
  // durch, und was durchfaellt, wird eine ganze Runde lang nicht wieder gefragt.
  //
  // Also: Karte auf die beste Vermutung parken -- aus demselben Grund wie bei
  // der Pause unten, ein auftauchendes Signal bestaetigt sich sonst auf der
  // zuletzt zufaellig eingestellten Norm selbst -- und die Runde von vorn
  // beginnen lassen, sobald wieder etwas anliegt.
  if (starved && !standardStarvedLogged_) {
    standardStarvedLogged_ = true;
    CAP_LOG("Videonorm: seit %.1f s kommt kein Bild mehr an -- %s passt nicht zum anliegenden "
            "Signal, die Suche laeuft weiter",
            frameAgeMs >= 0.0 ? frameAgeMs / 1000.0
                              : QpcToSeconds(now - standardStarvedSinceQpc_),
            VideoStandardName(VideoStandardIndexOf(capture_.currentStandard())));
  }
  if (!starved && renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    if (standardCandidate_ >= 0 || capture_.currentStandard() != candidates.front()) {
      bool switched = false;
      if (capture_.currentStandard() != candidates.front()) {
        capture_.SetStandard(candidates.front());
        standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
        ResetStandardColourCheck();
        switched = true;
      }
      CAP_LOG("Videonorm: kein Signal am Eingang, Suche angehalten und auf %s geparkt",
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
      standardCandidate_ = -1;

      // Parken heisst die Karte umstellen, und eine Norm bringt ihre Zeilenzahl
      // mit. Der Graph muss also mit, genau wie beim Einrasten weiter oben --
      // sonst laeuft er auf der alten Groesse weiter, und die Karte schiebt ein
      // 625-Zeilen-Bild in einen 480 Zeilen hohen Graphen.
      //
      // Auch das ist gemessen: am 31.08.2026 wurde der GameCube von 60 auf 50 Hz
      // zurueckgestellt. 01:48:03 wurde auf PAL B geparkt, 01:48:04 kamen 25,03
      // Bilder/s an -- und der Graph stand noch auf 720x480. Neunundvierzig
      // Sekunden lang war das Bild gestaucht, die Kammpruefung urteilte auf
      // einer Groesse, die es nicht gab, und die Aufloesungsliste zeigte die
      // Formate der alten Norm. Erst ein Neustart von Hand hat es geradegezogen.
      //
      // Ohne Signal neu aufzubauen ist dabei kein Nachteil, sondern der Sinn:
      // wenn das Bild wiederkommt, steht der Graph schon richtig. Und oefter
      // als noetig geschieht es nicht -- geparkt wird nur, wenn die Karte noch
      // nicht auf der Vermutung steht, und losgelassen nur, wenn sich die
      // Zeilenzahl wirklich geaendert hat.
      if (switched && ReleaseStandardBoundFormat(VideoStandardLines(candidates.front()))) {
        std::string error;
        if (!StartCapture(&error)) Toast(error);
      }
    }
    // Die Frist des laufenden Kandidaten immer wieder von vorn, damit sie erst
    // zu laufen beginnt, wenn es etwas zu messen gibt.
    standardSetQpc_ = now;
    standardNextTryQpc_ = now + SecondsToQpc(kStandardSettleSeconds);
    return;
  }

  // Eine ganze Runde durch und nichts ist eingerastet: dann liegt vermutlich
  // gar kein Signal an. Pause, dann von vorn.
  //
  // Hier stand `standardSweeps_ >= 1 && standardCandidate_ < 0`, und das konnte
  // nie beides zugleich gelten. Der Index geht ausserhalb der Initialisierung
  // nur in dem Zweig auf -1, in dem der Lock geklappt hat, und der kehrt sofort
  // zurueck; beim allerersten Durchlauf ist er zwar -1, dann sind aber noch
  // null Runden gelaufen. Die Pause trat also nie ein, und die Suche schrieb
  // der Karte fuer immer alle `kStandardSettleSeconds` eine neue Norm.
  //
  // Jetzt wird das Ende einer Runde erkannt, bevor weitergeschaltet wird, und
  // `standardSweeps_` zaehlt wirklich abgeschlossene Runden, so wie es in
  // app.h beschrieben ist.
  if (standardCandidate_ + 1 >= (int)candidates.size()) {
    ++standardSweeps_;
    standardCandidate_ = -1;
    standardNextTryQpc_ = now + SecondsToQpc(kStandardBackoffSeconds);
    // Und dabei nicht stehen lassen, was zuletzt probiert wurde.
    //
    // Waehrend der Pause steht irgendeine Norm auf der Karte, und wenn in
    // dieser Zeit ein Signal auftaucht -- die Konsole wird eingeschaltet,
    // jemand steckt endlich das Kabel an -- dann rastet der Lock darauf ein und
    // oben gilt "was gesetzt ist, stimmt". Gesucht wird dann gar nicht mehr.
    //
    // Das war ein echter Fehler: die Liste endet mit den seltensten Normen, die
    // Runde hinterliess also NTSC 4.43 auf der Karte, und weil der Lock nur
    // waagerecht misst, bestaetigt sich NTSC 4.43 an jeder 525/60-Quelle
    // selbst. Ein PAL-60-GameCube wurde so zuverlaessig als NTSC 4.43 erkannt.
    // Die Pause dauert laenger als eine Runde, das traf also die Mehrzahl der
    // Faelle -- und zwar genau den haeufigsten Ablauf ueberhaupt, naemlich
    // CapView zuerst starten und die Konsole danach.
    //
    // Der erste Kandidat ist bauartbedingt die beste Vermutung: der Partner der
    // zuletzt eingerasteten Norm, sonst die haeufigste ueberhaupt. Ein Irrtum
    // dieser Art ist damit der wahrscheinlichste statt der unwahrscheinlichste,
    // und wo der erste Kandidat 625/50 ist, kann eine 525/60-Quelle sich gar
    // nicht mehr selbst bestaetigen: der Lock scheitert und es wird richtig
    // gesucht.
    if (capture_.currentStandard() != candidates.front()) {
      capture_.SetStandard(candidates.front());
      standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    }
    // Nur beim ersten Mal, sonst laeuft das Log voll: dass pausiert wird, ist
    // einmal eine Nachricht und danach der Normalzustand.
    if (standardSweeps_ == 1) {
      CAP_LOG("Videonorm: eine Runde ohne Lock, Suche pausiert (%.0f s), Karte auf %s gestellt",
              kStandardBackoffSeconds,
              VideoStandardName(VideoStandardIndexOf(candidates.front())));
    }
    return;
  }

  // Verworfen -- und in der ersten Runde steht im Log, nach wie langer Frist.
  // Genau hier entsteht der Fehler, wenn die Frist zu kurz ist: eine Norm, die
  // nur noch nicht fertig eingefangen hat, sieht genauso aus wie eine falsche.
  // Nur die erste Runde, sonst schreibt eine Quelle ohne Signal das Log voll.
  if (standardSweeps_ == 0 && standardCandidate_ >= 0 && standardSetQpc_ != 0) {
    CAP_LOG("Videonorm: %s nach %.2f s ohne Lock verworfen",
            VideoStandardName(VideoStandardIndexOf(candidates[(size_t)standardCandidate_])),
            QpcToSeconds(now - standardSetQpc_));
  }

  ++standardCandidate_;

  // Wer uns gerade aushungert, ist schon widerlegt und braucht keine Frist.
  //
  // Nach dem Parken steht die Karte bereits auf candidates.front(), und genau
  // die ist der naechste Kandidat. Ohne diesen Schritt wird sie noch einmal
  // gesetzt -- ein Nichts -- und bekommt dann als vorgezogene Norm ihre vollen
  // kStandardPreferredSeconds, obwohl seit Sekunden kein Bild kommt. Am
  // 31.08. um 02:29 waren das drei geschenkte Sekunden von elf: 02:29:44,7
  // PAL B gesetzt, 02:29:47,6 "nach 3,03 s ohne Lock verworfen", 02:29:48,3
  // Lock auf PAL 60.
  if (starved && standardCandidate_ + 1 < (int)candidates.size() &&
      candidates[(size_t)standardCandidate_] == capture_.currentStandard()) {
    ++standardCandidate_;
  }

  const long next = candidates[(size_t)standardCandidate_];
  capture_.SetStandard(next);
  standardSetQpc_ = now;
  standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
  // Setting it is not the same as it working. Give the decoder a moment, then
  // this function will look at the lock again and either keep it or move on.
  // Den vorgezogenen Kandidaten wird laenger zugehoert, siehe oben.
  //
  // Ausser es kommt gar kein Bild. Die laengere Frist ist fuer eine Konsole
  // gedacht, die noch hochfaehrt -- die liefert dabei Bilder, nur noch keine
  // stabilen. Kommt nichts, ist Warten nur Warten, und der Rueckweg zu einem
  // Bild fuehrt ausschliesslich ueber den naechsten Kandidaten.
  //
  // Steht das Aushungern hier noch nicht fest, faellt es weiter oben nach --
  // die Frist wird dann nachtraeglich gekuerzt statt vorher verweigert.
  standardNextTryQpc_ =
      now + SecondsToQpc(!starved && standardCandidate_ < preferred ? kStandardPreferredSeconds
                                                                   : kStandardSettleSeconds);
  // Die Norm hat gewechselt, also gehoert die bisherige Farbmessung zu einer
  // anderen Einstellung.
  ResetStandardColourCheck();
}

// Ob die eingerastete Norm auch farblich stimmt.
//
// Der Lock allein kann das nicht sagen, und das ist keine Schwaeche der
// Umsetzung, sondern der Auskunft: er meldet, dass die Zeilenfrequenz gefunden
// wurde. 525/60 zerfaellt aber in fuenf Normen, die sich allein im Farbtraeger
// unterscheiden -- PAL 60 und NTSC 4.43 bei 4,43 MHz, NTSC M, NTSC M (Japan)
// und PAL M bei 3,58 MHz -- und waagerecht sehen die fuenf identisch aus. Steht
// die falsche, rastet die Karte trotzdem ein und meldet Erfolg.
//
// Was dann herauskommt, ist aber nicht unsichtbar: der Burst sitzt auf der
// falschen Frequenz, der Farbkiller des Decoders greift, und das Bild wird grau
// mit etwas Regenbogengries darin. Genau das laesst sich messen, und es ist die
// einzige Auskunft ueber den Farbtraeger, die es ueberhaupt gibt.
//
// Deshalb wird nicht behauptet, sondern verglichen: unter einem Wert, bei dem
// von Farbe keine Rede mehr sein kann, wird die Norm mit dem anderen Traeger
// probiert und nachgemessen. Nur wenn die deutlich farbiger ist, wird
// gewechselt. Bei einer wirklich schwarzweissen Quelle -- einem alten Film,
// einer S/W-Kamera -- sind beide gleich blass, es gewinnt keiner, und es bleibt
// bei dem, was die Region vorgeschlagen hat. Das ist die Rolle der Region an
// dieser Stelle: sie ist der Gleichstandssieger, den eine Messung allein nicht
// hat.
//
// Einmal je Norm. Danach ist die Sache entschieden, und eine Messung, die sich
// jede Minute neu meldet, waere ein Schalter, der von selbst umspringt.
void App::VerifyStandardColour(int64_t now) {
  // Unterhalb davon ist das Bild grau. Bewusst etwas ueber Null: an einem
  // Composite-Eingang rauscht auch ein totgeschalteter Farbkanal noch ein
  // wenig, und ein Verdacht, der am Rauschen scheitert, meldet sich nie.
  //
  // Am 29.08.2026 an einem GameCube im PAL-60-Modus gemessen, je einmal mit der
  // richtigen und der falschen Norm auf derselben Szene:
  //
  //   Szene                     NTSC M (Japan)   PAL 60
  //   Sherbet Land (Schnee)              0,024    0,037
  //   Dry Dry Desert (bunt)              0,013    0,207
  //
  // Der Schwellwert liegt ueber beiden falschen Messungen und unter der
  // richtigen der bunten Szene. Die farbarme Szene ist der enge Fall -- 0,037
  // gegen 0,035 --, und genau fuer den gibt es die Wiederholung weiter unten:
  // dort entscheidet nicht der Schwellwert, sondern die naechste Kurve.
  static const float kChromaSuspect = 0.035f;
  // Und so viel Farbe muss dastehen, damit der erste Durchgang die anliegende
  // Norm ohne jeden Vergleich durchwinkt.
  //
  // kChromaSuspect beantwortet "ist ueberhaupt Farbe da". Das ist die richtige
  // Frage fuer den Vergleich und die falsche fuer die Abkuerzung, und der
  // Unterschied hat heute eine Fehlerkennung gekostet: die Karte stand noch auf
  // PAL N, mass an einem PAL-B-Signal 0,042 -- knapp ueber der Sichtbarkeit --
  // und hatte dazu saubere Tiefen, weil ein falscher Traeger in den Tiefen eben
  // wenig anrichtet, wenn er ueberhaupt wenig anrichtet. Damit war die falsche
  // Norm bestaetigt, bevor irgendetwas verglichen wurde.
  //
  // Eine richtige Dekodierung liegt nicht knapp ueber der Sichtbarkeit, sondern
  // deutlich darueber: dieselbe Szene mass mit PAL B 0,152, mit PAL N 0,017 bis
  // 0,042. Die Schwelle liegt geometrisch dazwischen. Wer sie verfehlt, ist
  // deswegen nicht falsch -- er wird nur verglichen statt geglaubt, und das
  // kostet ein paar Sekunden, keine Fehlerkennung.
  static const float kChromaConfident = 0.080f;
  // So viel farbiger muss der Gewinner sein. Ein knapper Vorsprung ist kein
  // Befund, sondern Rauschen -- und im Zweifel bleibt es bei der Norm, die zur
  // Region passt.
  //
  // Zwei Werte, weil es zwei Faelle sind. Hat der Gewinner sichtbar Farbe, ist
  // die Messung schon von allein aus dem Rauschen heraus und ein deutlicher
  // Vorsprung genuegt. Liegen beide unter der Sichtbarkeitsschwelle, zaehlt
  // allein der Abstand, und der muss dann groesser sein: zwei Rauschwerte
  // derselben Szene liegen dicht beieinander.
  //
  // Hier stand vorher, der Gewinner muesse ausserdem selbst ueber
  // kChromaSuspect liegen -- sonst ergaeben zwei Rauschwerte allein durch ihr
  // Verhaeltnis einen Sieger, den niemand sieht. Der Gedanke stimmt, die
  // Umsetzung war zu grob: sie warf einen Sieg um Faktor 12 (0,024 gegen
  // 0,002) mit einem um Faktor 1,5 in denselben Topf. Genau das trennen die
  // zwei Werte jetzt, und die Messungen vom 29.08. ziehen die Grenze von
  // selbst -- entschieden waren Faktor 12 und 16, unentschieden 1,54.
  // Dazwischen ist viel Platz.
  static const float kChromaBetterBy = 1.6f;
  static const float kChromaBetterByFaint = 3.0f;
  // Und darunter zaehlt gar nichts mehr. Ein Verhaeltnis braucht einen Nenner:
  // misst der Verlierer glatt null, gewinnt jede noch so kleine Zahl mit
  // unendlichem Vorsprung. Der Boden liegt weit unter der kleinsten Messung,
  // die je etwas entschieden hat (0,024), und weit ueber dem Nichts.
  static const float kChromaFloor = 0.005f;
  // Wie oft der Gegenversuch wiederholt wird, wenn er nichts entscheidet.
  //
  // Das ist der Fall, den es wirklich gibt: eine Szene, die auch richtig
  // dekodiert fast grau ist. Am 29.08. an Sherbet Land gemessen -- eine
  // Schneepiste -- kamen 0,024 gegen 0,037 heraus, und daraus laesst sich
  // nichts schliessen. Einmal zu fragen und dann fuer immer zu schweigen waere
  // hier das Schlechteste: das Bild bliebe grau, obwohl die naechste Kurve die
  // Antwort liefert.
  //
  // Also wird spaeter noch einmal gemessen, mit wachsendem Abstand, und dann
  // ist Schluss. Jeder Versuch kostet ein paar Sekunden falsche Farbe, das darf
  // nicht endlos sein -- und eine wirklich schwarzweisse Quelle waere sonst
  // genau das.
  //
  // Der Abstand war einmal 30 s, weil "die naechste Kurve" bei Mario Kart
  // ungefaehr so lange braucht. Das war zu vorsichtig gedacht: solange nichts
  // entschieden ist, laeuft das Bild in der Ausgangsnorm weiter und der
  // Gegenversuch kostet zwei Sekunden. Drei Sekunden, verdoppelt, ergeben
  // 3 + 6 + 12 -- nach gut zwanzig Sekunden ist das Urteil gefaellt.
  //
  // Vorher standen hier 10 + 20 + 40. Das war zu vorsichtig gerechnet: der
  // Wiederholungsfall ist nicht teuer, weil er meistens gar keine Runde ist.
  // Steht ueber die Farbe der eingestellten Norm inzwischen genug fest, kostet
  // er nur den Blick darauf -- so am 31.08. um 02:17:47 und 02:25:41, beide
  // Male "PAL 60 hat deutlich Farbe", ohne einen einzigen Normwechsel. Nur
  // wenn es wieder nicht reicht, laeuft eine volle Runde, und die kostet die
  // vier Sekunden, gegen die die Pause gedacht war.
  static const int kColourRetries = 3;
  static const double kColourRetryBaseSeconds = 3.0;
  // Und was ein verworfener Rundgang wartet, naemlich fast nichts.
  //
  // Die Pause oben ist gegen eine graue Szene gedacht, und dort ist das Warten
  // der Zweck: die Szene soll erst farbig werden. Ein Rundgang, der verworfen
  // wurde, weil sich das Bild waehrenddessen geaendert hat, ist der
  // entgegengesetzte Fall -- gefehlt hat nicht die Farbe, sondern die Ruhe,
  // und ein Rennspiel wird durch Zuwarten nicht ruhiger. Sechs Sekunden
  // spaeter ist die Szene genauso in Bewegung, nur ist die Antwort dann sechs
  // Sekunden aelter.
  //
  // Die Zahl der Anlaeufe bleibt bei kColourRetries, also bleibt auch die
  // sichtbare Stoerung dieselbe: gleich viele Normwechsel, nur ohne die
  // Totzeit dazwischen. Aus 3 + 6 s werden 1 + 1 s.
  static const double kColourMotionRetrySeconds = 1.0;
  // Kommt in dieser Zeit keine Messung zustande, kommt keine. Das Format hat
  // dann keine lesbare Farbe -- siehe VideoRenderer::AnalyzeChroma -- oder es
  // laufen zu wenige Bilder durch. Beides ist kein Fehler, nur ein Nein.
  // Grosszuegig gegen die knapp drei Sekunden, die das Messfenster braucht.
  static const double kColourGiveUpSeconds = 10.0;

  // Um wie viel sauberer das Schwarz des Siegers sein muss.
  //
  // Es gibt hier bewusst *keinen* festen Schwellwert dafuer, ab wann Schwarz
  // als eingefaerbt gilt, und das ist eine Messung, keine Vorsicht. Am
  // 30.08.2026 an einem PAL-GameCube, dieselbe Szene dreimal dekodiert:
  //
  //   Norm                Farbe   dunkle Bereiche
  //   PAL B  (richtig)    0,152   0,101
  //   SECAM B (falsch)    0,461   0,353
  //   PAL N  (falsch)     0,017   0,014
  //
  // Ein fester Wert muesste zwischen 0,101 und 0,353 liegen, und der erste
  // Versuch mit 0,060 warf prompt die *richtige* Norm hinaus. Der Grund ist,
  // dass "dunkel" nicht "schwarz" heisst: unterhalb der Lumaschwelle liegen
  // auch dunkelrote und dunkelblaue Flaechen, die dort mit Recht Farbe haben,
  // und wie viel davon im Bild ist, haengt an der Szene. Ein absoluter Wert
  // misst also mit, was er nicht messen soll.
  //
  // Der Vergleich untereinander tut das nicht: alle drei Messungen sehen
  // dieselbe Szene, der Szenenanteil ist in allen dreien derselbe, und was sie
  // trennt, ist allein der Farbtraeger. 0,353 gegen 0,101 ist Faktor 3,5;
  // gefordert wird 1,6, also gut das Doppelte an Luft.
  static const float kDarkCleanerBy = 1.6f;

  // Ab wann die Tiefen eines Kandidaten fuer sich allein als eingefaerbt
  // gelten -- ohne Vergleich, ohne zweite Norm.
  //
  // Der Absatz darueber sagt, ein fester Wert sei untauglich, weil "dunkel"
  // nicht "schwarz" heisst. Das gilt weiter, und trotzdem steht hier einer.
  // Der Grund ist der Versuch mit dem Anteil der Tiefen an der Gesamtfarbe,
  // der genau diesen festen Wert vermeiden sollte und daran gescheitert ist.
  // Alle Messungen an einem PAL-GameCube, 30.08.2026:
  //
  //   Norm                Farbe   Tiefen   Anteil
  //   PAL B  (richtig)    0,152    0,101    0,66
  //   PAL B  (richtig)    0,192    0,110    0,57
  //   PAL B  (richtig)    0,075    0,057    0,76
  //   SECAM B (falsch)    0,461    0,353    0,77
  //   SECAM B (falsch)    0,358    0,335    0,94
  //   SECAM B (falsch)    0,424    0,363    0,86
  //   SECAM B (falsch)    0,421    0,289    0,69
  //   PAL N  (falsch)     0,017    0,014    0,82
  //   PAL N  (falsch)     0,042    0,037    0,88
  //   PAL N  (falsch)     0,017    0,022    1,29
  //
  // Im Anteil ueberlappen richtig (0,57 bis 0,76) und falsch (0,69 bis 1,29).
  // In den Tiefen selbst nicht: richtig bleibt unter 0,110, falsches SECAM
  // faengt bei 0,289 an, Faktor 2,6 dazwischen.
  //
  // Am selben Abend noch einmal, nachdem das Messfenster von 3,2 s auf 0,4 s
  // verkuerzt war (siehe SetChromaCadence), vier Rundgaenge:
  //
  //   PAL B  (richtig)  0,190/0,112  0,259/0,044  0,135/0,053  0,163/0,069
  //   SECAM B (falsch)  0,357/0,334  0,477/0,273  0,438/0,393  0,425/0,204
  //   PAL N  (falsch)   0,042/0,038  0,012/0,011  0,019/0,015  0,018/0,027
  //
  // Die Tiefen des richtigen PAL B bleiben, wo sie waren -- unter 0,112 --,
  // die des falschen SECAM B reichen jetzt bis 0,204 herunter. Der Abstand
  // schrumpft damit von Faktor 2,6 auf 1,8, und der Wert steht nicht mehr in
  // der Mitte, sondern naeher am falschen Rand.
  //
  // Er bleibt trotzdem, wo er ist, weil die beiden Irrtuemer verschieden viel
  // kosten. Zu hoch heisst: ein falscher Kandidat gilt als plausibel und tritt
  // in Stufe eins an -- wo er gegen richtige Tiefen von 0,069 mit 0,204 immer
  // noch um Faktor 3 verliert. Zu niedrig heisst: der *richtige* Kandidat
  // fliegt aus beiden Stufen, der Rundgang entscheidet nichts, und der Nutzer
  // sieht bis zum naechsten Versuch weiter falsche Farben. Der Wert lehnt sich
  // deshalb an die Seite, auf der ein Fehler noch aufgefangen wird.
  //
  // Das Argument fuer den Anteil war, beide Zahlen kaemen aus denselben
  // Bildern, die Szene kuerze sich also heraus. Das stimmt fuer einen
  // *Vergleich zweier Kandidaten* -- und dort wird weiter verglichen, siehe
  // kDarkCleanerBy. Fuer einen festen Wert je Kandidat stimmt es nicht: der
  // Anteil haengt daran, wie viel des Bildes ueberhaupt dunkel ist, und das
  // ist selbst eine Szeneneigenschaft. Auf einer Szene mit wenig dunkler
  // Flaeche faellt der Anteil des falschen Traegers (0,69), auf einer flauen
  // steigt der des richtigen (0,76), und die beiden tauschen die Plaetze.
  //
  // Was den festen Wert hier tragfaehig macht und ihn beim ersten Versuch mit
  // 0,060 zu Fall brachte, ist die Bedingung davor: geprueft wird nur, wer
  // schon kraeftig Farbe hat (kChromaConfident). Wer wenig Farbe zeigt, hat
  // auch wenig davon in den Tiefen und wird gar nicht erst beurteilt.
  //
  // Faellt der Wert einmal falsch, kostet das keine falsche Norm: sind alle
  // Kandidaten eingefaerbt, bleibt die Entscheidung aus und es gilt die
  // Reihenfolge der Wohnregion.
  static const float kDarkTinted = 0.18f;

  // Wie viel des Bildes beleuchtet sein muss, damit ein Rundgang ueberhaupt
  // etwas entscheiden kann.
  //
  // Schwarz ist in jeder Norm schwarz. Auf einem fast schwarzen Bild messen
  // alle Kandidaten dieselbe Null, und aus lauter gleichen Zahlen laesst sich
  // keine Norm auswaehlen -- der Rundgang laeuft, schaltet sichtbar durch drei
  // falsche Normen und kommt mit nichts zurueck.
  //
  // Genau so geschehen am 31.08. um 05:30:03. Der Wertebereichsmesser hatte
  // eine Sekunde vorher "95,135 % unter 16" notiert, das Bild war bis auf einen
  // hellen Rest schwarz. Gemessen wurde dann PAL 60 mit 0,005, NTSC M mit
  // 0,002, PAL M mit 0,003, NTSC 4.43 mit 0,003. Zweieinhalb Sekunden
  // Durchschalten fuer vier Mal nichts; als das Bild zurueck war, stand PAL 60
  // bei 0,194 und die Sache war in einer Messung erledigt.
  //
  // Der vorhandene Waechter greift hier nicht und soll es auch nicht: er fragt
  // SignalVerdict::Flat, also ob ueberhaupt etwas anliegt, und es lag etwas an
  // -- die Spanne war 154. "Es ist etwas zu sehen" und "es ist genug zu sehen,
  // um Farbe daran zu messen" sind zwei Fragen.
  //
  // Ein Zehntel, und die Wahl ist bewusst weit weg von dem, was ein Spielbild
  // trifft: gemeint ist nicht "dunkle Szene", sondern "praktisch nichts da".
  //
  // Nachgemessen am 31.08. um 05:43, mit kuenstlich hochgesetztem
  // kChromaConfident, damit ein Rundgang auf laufendem Bild erzwungen wird:
  // **54 % beleuchtet**. Das Fuenffache der Schwelle -- auf gewoehnlichem
  // Spielinhalt kann sie nicht danebengreifen. Der Fall, um den es geht, lag
  // mit "95 % unter Luma 16" auf der anderen Seite.
  //
  // Und falsch herum kostet sie nichts: eingefaerbte Tiefen laufen unten an ihr
  // vorbei, und wer wartet, verbraucht keinen Anlauf.
  static const float kChromaLitWanted = 0.10f;

  auto darkText = [&](float d) {
    return d < 0.0f ? std::string("keine dunklen Stellen") : Format("%.3f", d);
  };

  // Waehrend eines Vergleichs wird dicht abgetastet, sonst duenn.
  //
  // Hier oben, vor jedem Ruecksprung, damit der dichte Takt nicht in den
  // Normalbetrieb durchsickern kann: colourCandidates_ *ist* der Suchzustand,
  // und wer ihn leert, stellt damit auch den Takt zurueck. Die Bedingung wird
  // je Bild neu gestellt, ausgefuehrt wird nur der Wechsel.
  renderer_.SetChromaCadence(colourCandidates_.empty() ? 8 : 1);

  const long current = capture_.currentStandard();
  if (current == 0) return;
  const bool walking = !colourCandidates_.empty();
  if (!walking && current == colourCheckedStandard_) return;
  // Ein unentschiedener Versuch wartet, bevor er sich wiederholt.
  if (colourRetryQpc_ != 0 && now < colourRetryQpc_) return;
  colourRetryQpc_ = 0;

  // Bei totem Eingang wird nicht gemessen. Ein Eingang ohne Signal ist grau,
  // und grau ist hier die Aussage "der Farbtraeger stimmt nicht" -- die Messung
  // saehe also nicht etwa nichts, sie saehe zuverlaessig das Falsche. Dasselbe
  // Argument wie beim Weiterschalten der Normensuche, siehe oben.
  if (renderer_.detectedSignal() == VideoRenderer::SignalVerdict::Flat) {
    renderer_.ResetChroma();
    colourStartedQpc_ = 0;
    return;
  }

  // Frisch gewechselt: die ersten Bilder gehoeren noch der alten Einstellung.
  //
  // Zurueckgesetzt wird genau einmal, naemlich wenn die Frist abgelaufen ist.
  // Hier stand vorher ein Ruecksetzer je Bild, solange gewartet wird, und der
  // sah richtig aus und war es nicht: diese Funktion laeuft nach einem
  // Normwechsel eine knappe halbe Sekunde ueberhaupt nicht: die Suche wartet
  // auf zwei frische Messwerte des Wachtthreads, siehe UpdateVideoStandard.
  // Faellt das Ende der Frist in dieses Loch, ist der erste Aufruf danach
  // schon zu spaet -- es hat nie jemand zurueckgesetzt, und gemessen wird ab
  // dem Wechsel statt ab dem Fristende, mitsamt dem Umschaltmoment der Karte.
  //
  // Am 30.08. mit Frist null nachgestellt, weil dort dasselbe Loch immer
  // klafft: PAL N mass 0,047 und 0,064 statt 0,012 bis 0,021, SECAM B in den
  // Tiefen 0,246 statt 0,271 bis 0,410 -- jeder Kandidat zum Nachbarn hin
  // verschmiert. Mit einer Frist von 0,5 s traf es nur die Laeufe, in denen
  // das Loch etwas laenger war als die Frist, und das war nicht zu sehen.
  //
  // Ein Ruecksetzer am Fristende macht den Anfang des Messfensters unabhaengig
  // davon, wann der naechste Aufruf kommt, und kostet nichts: das Fenster lag
  // ohnehin dahinter.
  if (colourSettleUntilQpc_ != 0) {
    if (now < colourSettleUntilQpc_) return;
    colourSettleUntilQpc_ = 0;
    colourStartedQpc_ = now;
    renderer_.ResetChroma();
    return;
  }
  if (colourStartedQpc_ == 0) colourStartedQpc_ = now;

  const float energy = renderer_.chromaEnergy();
  if (energy < 0.0f) {
    if (QpcToSeconds(now - colourStartedQpc_) <= kColourGiveUpSeconds) return;
    // Keine Messung zustande gekommen. Ohne laufenden Rundgang ist die Sache
    // damit erledigt -- es gibt nichts zu vergleichen. Im Rundgang zaehlt es
    // als "weiss nicht", wird als solches eingetragen und der naechste
    // Kandidat ist dran.
    if (!walking) {
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      return;
    }
  }
  const float dark = energy < 0.0f ? -1.0f : renderer_.darkChromaEnergy();

  if (!walking) {
    // Erster Durchgang, und hier werden zwei Fragen gestellt statt einer: hat
    // das Bild Farbe, und bleibt sein Schwarz schwarz?
    //
    // Die zweite fehlte, und das war ein echter Fehler mit sichtbarer Folge.
    // Am 30.08.2026 rastete ein PAL-Signal auf SECAM ein, und SECAM auf PAL
    // liefert kein graues Bild, sondern ein kraeftig eingefaerbtes -- der
    // Nutzer sah einen leuchtend roten Hintergrund, wo Schwarz sein sollte.
    // Der Test "hat Farbe" ging glatt durch, die Fehldekodierung galt als
    // bestaetigt, und danach wurde sie nie wieder in Frage gestellt.
    // Der Schwellwert hier ist ein *Verdacht*, kein Urteil. Er entscheidet nur,
    // ob ueberhaupt verglichen wird; welche Norm gewinnt, entscheiden danach
    // die Kandidaten untereinander. Ein zu hoher Wert kostet eine
    // Fehldekodierung, ein zu niedriger ein paar Sekunden Vergleich. Es ist
    // dieselbe Frage wie beim Vergleich -- sind diese Tiefen eingefaerbt --
    // und deshalb derselbe Wert; die Messreihe steht bei kDarkTinted.
    if (energy >= kChromaConfident && dark < kDarkTinted) {
      CAP_LOG("Videonorm: %s hat deutlich Farbe (%.3f), dunkle Bereiche neutral (%s)",
              VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str());
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      colourAttempts_ = 0;
      colourWaitingForPicture_ = false;
      return;
    }

    // Zweifel ja -- aber ist genug Bild da, um ihn auszuraeumen?
    //
    // Der Zweifel hat zwei ganz verschiedene Gruende, und nur einer davon ist
    // eine Frage, die ein Rundgang beantworten kann. Zeigt ein *helles* Bild
    // keine Farbe, dann hat der Farbtraeger nicht gestimmt, und welcher es
    // stattdessen ist, entscheiden die Kandidaten untereinander. Zeigt ein
    // schwarzes Bild keine Farbe, dann ist es schwarz. Da ist nichts zu
    // entscheiden, und die vier gleichen Nullen von oben sind die Antwort
    // darauf.
    //
    // Eingefaerbte Tiefen fuehren *nicht* hierher, sondern in den Rundgang, und
    // zwar auch auf einem dunklen Bild: eine Farbe, die im Schwarzen steht, ist
    // ein Beleg fuer einen falschen Traeger und kein fehlender Messwert. Sie
    // ist auf einem dunklen Bild sogar am deutlichsten -- dort sind die
    // Bloecke, um die es geht.
    //
    // Gewartet wird, ohne etwas zu verbrauchen: kein Anlauf wird gezaehlt,
    // keine Norm als geprueft vermerkt. Ein Rundgang, der nie stattgefunden
    // hat, darf weder als unentschieden zaehlen noch die drei Anlaeufe
    // aufbrauchen, bevor das Bild ueberhaupt da ist -- eine Konsole, die
    // hochfaehrt, ist ein paar Sekunden lang schwarz, und danach soll die
    // Pruefung noch alle Anlaeufe haben. Zurueckgesetzt wird dabei wie beim
    // toten Eingang, damit die erste Messung nach dem Warten nicht durch das
    // Schwarze davor verduennt wird.
    const float lit = renderer_.chromaLitFraction();
    if (lit >= 0.0f && lit < kChromaLitWanted && dark >= 0.0f && dark < kDarkTinted) {
      if (!colourWaitingForPicture_) {
        colourWaitingForPicture_ = true;
        CAP_LOG("Videonorm: %s ist zweifelhaft (Farbe %.3f), aber nur %.0f %% des Bildes sind "
                "beleuchtet -- auf Schwarz ist keine Norm zu erkennen, es wird gewartet",
                VideoStandardName(VideoStandardIndexOf(current)), energy, lit * 100.0f);
      }
      renderer_.ResetChroma();
      colourStartedQpc_ = 0;
      return;
    }
    colourWaitingForPicture_ = false;

    colourCandidates_ = VideoStandardColourCandidates(
        current, capture_.capabilities().availableStandards, config_.app.videoRegion);
    // Der Rundgang beginnt bei der jetzigen Norm, die ja gerade gemessen wurde.
    // Steht sie nicht vorn, hat die Karte etwas gemeldet, das sie laut eigener
    // Auskunft gar nicht kann -- dann lieber nichts tun als raten.
    if (colourCandidates_.size() < 2 || colourCandidates_.front() != current) {
      colourCandidates_.clear();
      colourCheckedStandard_ = current;
      colourStartedQpc_ = 0;
      return;
    }
    const int count = (int)colourCandidates_.size();
    // Die Ausgangsnorm kommt am Ende ein zweites Mal dran.
    //
    // Der Vergleich unterstellt, alle Kandidaten saehen dieselbe Szene. Am
    // 30.08. um 15:24 Uhr, als jede Messung noch gut drei Sekunden brauchte
    // und der ganze Rundgang vierzehn, dieselbe Norm im Abstand von zwanzig
    // Sekunden: PAL B 0,181/0,217 und PAL B 0,191/0,107. Die Tiefen
    // halbierten sich, ohne dass sich am Signal etwas geaendert haette.
    //
    // Der Rundgang dauert seither knapp drei Sekunden (siehe
    // SetChromaCadence), und damit ist der Grund kleiner geworden, aber nicht
    // weg -- und er hat einen zweiten bekommen: die erste Messung der
    // Ausgangsnorm ist die mitlaufende aus dem Normalbetrieb, ueber drei
    // Sekunden gemittelt, die der Herausforderer sind kurze Aufnahmen. Die
    // Wiederholung stellt die Ausgangsnorm auf dieselbe Grundlage wie die
    // anderen.
    //
    // Der Schaden daraus ist einseitig. Ueber alle Messungen des 30.08.
    // streuen die Tiefen des falschen SECAM B um Faktor zwei (0,204 bis
    // 0,410), die des richtigen PAL B um Faktor fuenf (0,041 bis 0,217): eine
    // Schwebung aus dem falschen Traeger liegt gleichmaessig ueber jedem Bild
    // und haengt nur wenig an der Szene, echte Farbe in dunklen Flaechen
    // dagegen ganz und gar. Eine flaue Szene laesst also vor allem den
    // *richtigen* Kandidaten schlecht aussehen.
    //
    // Deshalb wird die Ausgangsnorm zweimal gemessen und tritt mit der
    // guenstigeren der beiden Messungen an. Das ist mit Absicht ungleich
    // verteilt: sie ist der Kandidat, der schon laeuft, und ein Wechsel weg
    // von einer richtigen Norm ist der teure Fehler.
    colourCandidates_.push_back(colourCandidates_.front());
    colourEnergies_.assign(colourCandidates_.size(), -1.0f);
    colourDarks_.assign(colourCandidates_.size(), -1.0f);
    colourIndex_ = 0;
    // Der beleuchtete Anteil steht mit in der Zeile, obwohl er die Runde nicht
    // ausloest. Er ist die Grundlage, auf der sie ueberhaupt etwas entscheiden
    // kann, und wenn ein Rundgang spaeter einmal unerklaerlich lauter Nullen
    // misst, steht die Erklaerung schon in der Zeile davor.
    CAP_LOG("Videonorm: %s ist zweifelhaft (Farbe %.3f, dunkle Bereiche %s, %.0f %% beleuchtet) "
            "-- die %d Normen mit %d Zeilen werden verglichen",
            VideoStandardName(VideoStandardIndexOf(current)), energy, darkText(dark).c_str(),
            lit < 0.0f ? 0.0f : lit * 100.0f, count, VideoStandardLines(current));
  }

  // Eintragen, was dieser Kandidat gemessen hat, und zum naechsten.
  colourEnergies_[(size_t)colourIndex_] = energy;
  colourDarks_[(size_t)colourIndex_] = dark;
  CAP_LOG("Videonorm: %s gemessen -- Farbe %s, dunkle Bereiche %s",
          VideoStandardName(VideoStandardIndexOf(current)),
          energy < 0.0f ? "keine Messung" : Format("%.3f", energy).c_str(),
          darkText(dark).c_str());

  ++colourIndex_;
  if (colourIndex_ < (int)colourCandidates_.size()) {
    capture_.SetStandard(colourCandidates_[(size_t)colourIndex_]);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    colourSettleUntilQpc_ = now + SecondsToQpc(kColourSettleSeconds);
    colourStartedQpc_ = 0;
    renderer_.ResetChroma();
    return;
  }

  // Alle durch, die Ausgangsnorm zweimal. Von ihren beiden Messungen zaehlt
  // die mit den saubereren Tiefen, und zwar als Paar: die Farbmenge kommt aus
  // derselben Messung wie die Tiefen, sonst stuenden Zahlen aus zwei Szenen
  // nebeneinander. Die Begruendung steht oben beim zweiten Anlauf.
  // Um wie viel die beiden Messungen derselben Norm auseinanderliegen duerfen,
  // bevor die Runde als ungueltig gilt. Siehe sceneChanged gleich darunter.
  static const float kSceneAgreeBy = 2.0f;
  bool sceneChanged = false;

  if (colourCandidates_.size() > 1 && colourCandidates_.back() == colourCandidates_.front()) {
    const size_t last = colourCandidates_.size() - 1;
    const bool better = colourDarks_[last] >= 0.0f &&
                        (colourDarks_[0] < 0.0f || colourDarks_[last] < colourDarks_[0]);

    // Und hier wird die Zweitmessung das, wofuer sie eigentlich da ist: die
    // Probe darauf, ob die Runde ueberhaupt eine Szene gesehen hat.
    //
    // Die Kandidaten werden nacheinander gemessen, eine ganze Runde dauert
    // ein paar Sekunden, und das Bild wartet nicht. Aendert sich die Szene
    // dazwischen -- ein Bootbildschirm wird zum Spiel --, dann sahen die
    // frueheren Kandidaten etwas anderes als die spaeteren, und der Vergleich
    // vergleicht nichts. Wer zufaellig drankam, als es bunt wurde, gewinnt.
    //
    // Genau das ist am 31.08.2026 um 02:02 passiert. Die Runde ueber die fuenf
    // 525-Zeilen-Normen fing auf einem dunklen Bild an und endete auf einem
    // farbigen: PAL 60 zuerst 0.005, am Ende 0.122, und dazwischen bekam
    // NTSC 4.43 mit 0.157 den Zuschlag -- gegen einen GameCube, der PAL 60
    // ausgibt. Danach stand die Karte auf einer Norm, die das Signal nicht
    // dekodieren kann.
    //
    // Gemessen wird die Abweichung an den beiden Schwellen, die in dieser
    // Runde ueberhaupt etwas entscheiden: kChromaConfident trennt "beurteilt"
    // von "unbeurteilt", kDarkTinted "plausibel" von "eingefaerbt". Springt
    // eine der beiden Messungen ueber eine dieser Schwellen, hat sich der
    // Massstab selbst bewegt. Der Faktor daneben faengt die Faelle, die
    // innerhalb einer Schwelle bleiben und trotzdem eine andere Szene sind.
    const float e0 = colourEnergies_[0], e1 = colourEnergies_[last];
    const float d0 = colourDarks_[0], d1 = colourDarks_[last];
    if (e0 >= 0.0f && e1 >= 0.0f) {
      if ((e0 < kChromaConfident) != (e1 < kChromaConfident)) sceneChanged = true;
      const float lo = e0 < e1 ? e0 : e1, hi = e0 < e1 ? e1 : e0;
      if (hi > lo * kSceneAgreeBy && hi >= kChromaConfident) sceneChanged = true;
    }
    if (d0 >= 0.0f && d1 >= 0.0f && (d0 < kDarkTinted) != (d1 < kDarkTinted)) sceneChanged = true;

    CAP_LOG("Videonorm: %s zum zweiten Mal gemessen -- Farbe %s, dunkle Bereiche %s (zuerst %s "
            "und %s)%s",
            VideoStandardName(VideoStandardIndexOf(colourCandidates_.front())),
            colourEnergies_[last] < 0.0f ? "keine Messung"
                                         : Format("%.3f", colourEnergies_[last]).c_str(),
            darkText(colourDarks_[last]).c_str(),
            colourEnergies_[0] < 0.0f ? "keine Messung" : Format("%.3f", colourEnergies_[0]).c_str(),
            darkText(colourDarks_[0]).c_str(),
            sceneChanged ? " -- die Szene hat sich waehrend der Runde geaendert, der Vergleich "
                           "gilt nicht"
                         : (better ? " -- die zweite zaehlt" : ""));
    if (better) {
      colourEnergies_[0] = colourEnergies_[last];
      colourDarks_[0] = colourDarks_[last];
    }
    colourCandidates_.pop_back();
    colourEnergies_.pop_back();
    colourDarks_.pop_back();
  }

  // Erst aussortieren, dann vergleichen -- und die Reihenfolge ist der ganze
  // Punkt.
  //
  // "Wer hat am meisten Farbe" waere das Naheliegende und ist falsch. Ein
  // falscher Farbtraeger toetet die Farbe nicht nur, er kann sie auch
  // erfinden -- und zwar gerade dann, wenn die richtige Norm nichts anzeigt.
  // Am 30.08. um 14:57 Uhr, an einem fast grauen Bild: PAL B (richtig) 0,006,
  // SECAM B (falsch) 0,424. Nach Farbmenge gewaenne die Fehldekodierung um
  // Faktor siebzig.
  //
  // Was die beiden trennt, ist nicht die Menge, sondern der Ort. Ein richtig
  // dekodiertes Bild hat farbige Mitten und neutrale Tiefen -- Schwarz ist
  // schwarz, weil dort nichts zu modulieren ist. Ein falscher Traeger legt
  // seine Schwebung gleichmaessig ueber alles, Mitten wie Tiefen.
  //
  // Daraus wird ein Test, den ein einzelner Kandidat fuer sich besteht oder
  // nicht: wie viel Farbe in den dunklen Stellen steht. Die Messreihe dazu und
  // der Grund, warum es der absolute Wert ist und nicht sein Anteil an der
  // Gesamtfarbe, stehen bei kDarkTinted.
  //
  // Der Test greift nur, wo er etwas messen kann: unterhalb kChromaConfident
  // sind die Tiefen ein Rauschwert und sagen nichts, und ohne dunkle Stellen
  // im Bild gibt es sie gar nicht. In beiden Faellen gilt der Kandidat als
  // unbeurteilt -- nicht als bestaetigt und nicht als widerlegt.
  enum class Verdict { Unjudged, Plausible, Tinted };
  std::vector<Verdict> verdicts(colourCandidates_.size(), Verdict::Unjudged);
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (colourEnergies_[i] < kChromaConfident || colourDarks_[i] < 0.0f) continue;
    verdicts[i] = colourDarks_[i] < kDarkTinted ? Verdict::Plausible : Verdict::Tinted;
  }

  int best = -1, runnerUp = -1;
  bool byDarks = false;

  // Stufe eins: unter den plausiblen gewinnt das sauberste Schwarz.
  //
  // Hier ist der absolute Vergleich richtig, denn jetzt stehen sich nur noch
  // Kandidaten gegenueber, die denselben Test bestanden haben und dieselbe
  // Szene sehen. Steht einer allein da, ist das ein Befund und kein Zufall:
  // er hat kraeftige Farbe und dazu neutrale Tiefen, und keiner der anderen
  // hat das.
  int dark1 = -1, dark2 = -1;
  for (size_t i = 0; i < colourCandidates_.size(); ++i) {
    if (verdicts[i] != Verdict::Plausible) continue;
    if (dark1 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark1]) {
      dark2 = dark1;
      dark1 = (int)i;
    } else if (dark2 < 0 || colourDarks_[i] < colourDarks_[(size_t)dark2]) {
      dark2 = (int)i;
    }
  }
  if (dark1 >= 0 && (dark2 < 0 || colourDarks_[(size_t)dark2] >
                                      colourDarks_[(size_t)dark1] * kDarkCleanerBy)) {
    best = dark1;
    runnerUp = dark2;
    byDarks = true;
  }

  // Stufe zwei: geben die Tiefen nichts her -- weil niemand kraeftig genug
  // Farbe hatte oder weil zwei gleich sauber sind --, bleibt es beim alten
  // Verfahren, wer am meisten Farbe hat. Fuer eine wirklich schwarzweisse
  // Quelle ist das nach wie vor die einzige sinnvolle Frage.
  //
  // Wer den Test aber *nicht* bestanden hat, ist hier raus und nicht bloss
  // hinten. Genau daran haengt der graue Fall von 14:57 Uhr: SECAM B haette
  // ihn mit Faktor siebzig gewonnen. Ein eingefaerbtes Schwarz ist ein
  // Ausschlussgrund, kein Nachteil.
  if (best < 0) {
    for (size_t i = 0; i < colourCandidates_.size(); ++i) {
      if (colourEnergies_[i] < 0.0f || verdicts[i] == Verdict::Tinted) continue;
      if (best < 0 || colourEnergies_[i] > colourEnergies_[(size_t)best]) {
        runnerUp = best;
        best = (int)i;
      } else if (runnerUp < 0 || colourEnergies_[i] > colourEnergies_[(size_t)runnerUp]) {
        runnerUp = (int)i;
      }
    }
  }

  const long origin = colourCandidates_.front();
  const float winner = best >= 0 ? colourEnergies_[(size_t)best] : -1.0f;
  const float second = runnerUp >= 0 ? colourEnergies_[(size_t)runnerUp] : -1.0f;
  const float needed = winner >= kChromaSuspect ? kChromaBetterBy : kChromaBetterByFaint;
  // Ueber die Tiefen entscheidet auch ein einzelner Kandidat, denn dort hat er
  // etwas bestanden. Ueber die blosse Farbmenge dagegen ist ein einzelner nur
  // eine gelungene Messung ohne Vergleich, und die entscheidet nichts.
  const bool alone = best >= 0 && runnerUp < 0;
  // sceneChanged sticht alles: hat die Runde zwei Szenen gesehen, sind die
  // Zahlen nicht falsch, sie gehoeren nur nicht zusammen. Dann entscheidet
  // hier nichts, und der Weg unten -- zurueck zum Ausgangspunkt, spaeter noch
  // einmal -- ist derselbe wie bei einer zu farbarmen Szene.
  const bool decided =
      !sceneChanged &&
      (byDarks || (best >= 0 && !alone && winner >= kChromaFloor && winner > second * needed));

  const long chosen = best >= 0 ? colourCandidates_[(size_t)best] : origin;
  const long runnerUpStandard = runnerUp >= 0 ? colourCandidates_[(size_t)runnerUp] : 0;
  const float winnerDark = best >= 0 ? colourDarks_[(size_t)best] : -1.0f;
  const float secondDark = runnerUp >= 0 ? colourDarks_[(size_t)runnerUp] : -1.0f;
  const float originEnergy = colourEnergies_.front();
  const float originDark = colourDarks_.front();

  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourIndex_ = 0;
  colourStartedQpc_ = 0;

  // Die Karte steht jetzt auf dem zuletzt gemessenen Kandidaten. In jedem Fall
  // muss sie da weg -- entweder auf den Sieger oder zurueck auf den Anfang.
  const long target = decided ? chosen : origin;
  if (current != target) {
    capture_.SetStandard(target);
    standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
    renderer_.ResetChroma();
  }

  if (decided) {
    if (alone) {
      CAP_LOG("Videonorm: %s ist als einzige kraeftig farbig mit neutralen Tiefen (Farbe %.3f, "
              "Tiefen %s) -- eingestellt",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner, darkText(winnerDark).c_str());
    } else if (byDarks) {
      CAP_LOG("Videonorm: %s hat die neutraleren Tiefen als %s (%s gegen %s, beide farbig) -- "
              "eingestellt",
              VideoStandardName(VideoStandardIndexOf(chosen)),
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)),
              darkText(winnerDark).c_str(), darkText(secondDark).c_str());
    } else {
      CAP_LOG("Videonorm: %s (Farbe %.3f) hat mehr Farbe als %s (%.3f) -- eingestellt",
              VideoStandardName(VideoStandardIndexOf(chosen)), winner,
              VideoStandardName(VideoStandardIndexOf(runnerUpStandard)), second);
    }
    colourCheckedStandard_ = chosen;
    standardLastGood_ = chosen;
    colourAttempts_ = 0;
    if (chosen != origin) {
      Toast(Format(T("Videonorm nach Farbe berichtigt: %s", "Video standard corrected by colour: %s"),
                   VideoStandardPickerName(chosen).c_str()));
    }
    return;
  }

  // Der Vergleich ist hin -- die Ausgangsnorm ist es deshalb nicht.
  //
  // "Verworfen" heisst: die Kandidaten haben verschiedene Szenen gesehen, also
  // sagt ihr Verhaeltnis zueinander nichts. Ueber die Ausgangsnorm allein sagt
  // das nichts aus. Ihre Messung ist eine vollstaendige Messung einer
  // einzelnen Norm, und fuer die gibt es oben laengst ein Urteil: kraeftig
  // Farbe und neutrale Tiefen heisst richtig dekodiert. Es ist woertlich
  // dieselbe Pruefung wie die im Normalbetrieb, nur spaeter im Ablauf -- also
  // wird sie hier gestellt statt eine Wiederholung dafuer zu bezahlen.
  //
  // Am 31.08.2026 um 05:24 hat genau das drei Sekunden gekostet. Die Runde
  // wurde um 39.908 verworfen, und in derselben Zeile stand PAL 60 mit Farbe
  // 0,194 und Tiefen 0,107 -- beides klar innerhalb der Schwellen. Der
  // Wiederholer stellte um 42.573 fest, was schon dagestanden hatte:
  // "PAL 60 hat deutlich Farbe (0.192), dunkle Bereiche neutral (0.107)".
  //
  // Neu ist daran kein Massstab. Damit eine falsche Norm hier durchkaeme,
  // muesste sie kraeftig Farbe *und* neutrale Tiefen zeigen, und das ist die
  // Beschreibung einer richtigen -- ein falsch dekodiertes SECAM lag in den
  // Tiefen bei 0,353 gegen eine Schwelle von 0,18.
  if (sceneChanged && originEnergy >= kChromaConfident && originDark >= 0.0f &&
      originDark < kDarkTinted) {
    CAP_LOG("Videonorm: Vergleich verworfen, aber %s steht fuer sich (Farbe %.3f, dunkle "
            "Bereiche %s) -- es bleibt dabei",
            VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
            darkText(originDark).c_str());
    colourCheckedStandard_ = origin;
    standardLastGood_ = origin;
    colourAttempts_ = 0;
    return;
  }

  // Kein klarer Sieger. Zurueck zum Ausgangspunkt -- und nicht abgehakt,
  // sondern spaeter noch einmal, denn eine graue Szene sagt nichts ueber den
  // Farbtraeger. Erst nach einigen Anlaeufen ist die Quelle wohl wirklich
  // schwarzweiss.
  ++colourAttempts_;
  if (colourAttempts_ >= kColourRetries) {
    // Auch hier zaehlt der Grund. "Die Quelle ist schwarzweiss" ist eine
    // Aussage ueber das Signal, und die darf nicht fallen, wenn gar nicht die
    // Farbe gefehlt hat, sondern die Ruhe. Aufgegeben wird trotzdem: nach drei
    // Anlaeufen ueber gut siebzig Sekunden bewegt sich das Bild eben staendig,
    // und ohne verlaesslichen Vergleich bleibt der Ausgangspunkt das Beste,
    // was wir haben.
    if (sceneChanged) {
      CAP_LOG("Videonorm: nach %d Anlaeufen war das Bild jedesmal in Bewegung -- kein "
              "verlaesslicher Vergleich moeglich, es bleibt bei %s",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)));
    } else {
      CAP_LOG("Videonorm: nach %d Anlaeufen entscheidet nichts -- die Quelle ist wohl "
              "schwarzweiss, es bleibt bei %s (Farbe %.3f, dunkle Bereiche %s)",
              colourAttempts_, VideoStandardName(VideoStandardIndexOf(origin)), originEnergy,
              darkText(originDark).c_str());
    }
    colourCheckedStandard_ = origin;
    return;
  }
  const double wait = sceneChanged ? kColourMotionRetrySeconds
                                   : kColourRetryBaseSeconds * (double)(1 << (colourAttempts_ - 1));
  CAP_LOG("Videonorm: Vergleich %s -- in %.0f s noch einmal, bis dahin %s",
          sceneChanged ? "verworfen, waehrenddessen hat sich das Bild geaendert"
                       : "unentschieden, die Szene ist zu farbarm",
          wait, VideoStandardName(VideoStandardIndexOf(origin)));
  colourRetryQpc_ = now + SecondsToQpc(wait);
}

void App::ResetStandardColourCheck() {
  colourCheckedStandard_ = 0;
  colourCandidates_.clear();
  colourEnergies_.clear();
  colourDarks_.clear();
  colourIndex_ = 0;
  colourSettleUntilQpc_ = 0;
  colourStartedQpc_ = 0;
  colourRetryQpc_ = 0;
  colourAttempts_ = 0;
  colourWaitingForPicture_ = false;
  // Auch der Takt zurueck: der Abbruch kann von aussen kommen -- Signal weg,
  // Graph neu -- und dann laeuft VerifyStandardColour nicht mehr, das den Takt
  // sonst selbst zuruecknimmt.
  renderer_.SetChromaCadence(8);
  renderer_.ResetChroma();
}

// The window icon, as a texture for the empty state.
//
// Not through WIC, although WIC is already linked: the resource compiler splits
// an .ico into RT_GROUP_ICON plus one RT_ICON per size, so there is no .ico file
// in the binary for WIC to decode. LoadImage understands that split and picks
// the size asked for, which is the whole reason to go the GDI way here.
void App::LoadIdleIcon() {
  if (idleIcon_ || !d3d_.device()) return;

  const int want = 256;
  HICON icon = (HICON)::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON, want,
                                   want, LR_DEFAULTCOLOR);
  if (!icon) return;

  ICONINFO info = {};
  BITMAP bm = {};
  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  if (::GetIconInfo(icon, &info) && info.hbmColor &&
      ::GetObjectW(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
    w = bm.bmWidth;
    h = bm.bmHeight;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // negative: top down, so no row flip afterwards
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    pixels.resize((size_t)w * (size_t)h * 4);
    HDC screen = ::GetDC(nullptr);
    if (!::GetDIBits(screen, info.hbmColor, 0, (UINT)h, pixels.data(), &bi, DIB_RGB_COLORS)) {
      pixels.clear();
    }
    ::ReleaseDC(nullptr, screen);
  }
  if (info.hbmColor) ::DeleteObject(info.hbmColor);
  if (info.hbmMask) ::DeleteObject(info.hbmMask);
  ::DestroyIcon(icon);
  if (pixels.empty()) return;

  // GetDIBits hands back BGRA. Two things have to happen on the way to a
  // D3D texture: the channel swap, and premultiplying by alpha -- ImGui's
  // blend state is premultiplied, and handing it straight alpha draws a dark
  // halo around every edge of the icon.
  //
  // An icon with no alpha at all is an old-style one whose transparency lives
  // in the mask instead. Rather than decode the mask, such an icon is drawn
  // opaque: the rectangle is square and the background behind it is flat, so
  // the result is plain rather than wrong.
  bool anyAlpha = false;
  for (size_t i = 3; i < pixels.size(); i += 4) {
    if (pixels[i] != 0) { anyAlpha = true; break; }
  }
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const uint8_t b = pixels[i];
    const uint8_t r = pixels[i + 2];
    const unsigned a = anyAlpha ? pixels[i + 3] : 255u;
    pixels[i + 0] = (uint8_t)(r * a / 255u);
    pixels[i + 1] = (uint8_t)(pixels[i + 1] * a / 255u);
    pixels[i + 2] = (uint8_t)(b * a / 255u);
    pixels[i + 3] = (uint8_t)a;
  }

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA init = {};
  init.pSysMem = pixels.data();
  init.SysMemPitch = (UINT)w * 4;

  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(d3d_.device()->CreateTexture2D(&td, &init, &tex))) return;
  if (FAILED(d3d_.device()->CreateShaderResourceView(tex.Get(), nullptr, &idleIcon_))) {
    idleIcon_.Reset();
    return;
  }
  idleIconSize_ = w;
}

// Frames arriving is the whole answer on a digital input and no answer at all on
// an analogue one, where the card keeps delivering whatever an open wire decodes
// to. So the pixels are asked as well -- and the decoder's own lock is used only
// to say "no" faster, never to say "yes": this card reports a lock on some
// standards with nothing connected, which is measured and written down in the
// wiki.
bool App::HaveLiveSignal() const {
  const FrameSink* sink = capture_.sink();
  if (!sink || !sink->HasRecentFrame(kNoSignalSeconds) || !renderer_.hasFrame()) return false;

  // Solange die automatische Normensuche laeuft, sind die Pixel kein Zeuge.
  //
  // Jeder Normwechsel wirft fuer einen Moment breite gruene Streifen durch das
  // blaue Bild, waehrend der Decoder neu synchronisiert. Ein einziger solcher
  // Blitz liest sich als "Picture", nimmt unten die Abkuerzung und setzt die
  // Geduld wieder auf null -- bei einer Suche, die alle paar hundert
  // Millisekunden umschaltet, kommt die Kein-Signal-Anzeige deshalb nie.
  //
  // Nach einer vollen Runde ohne Lock ist die Sache aber entschieden: es wurde
  // jede Norm durchprobiert, die die Karte kann, und keine hat gegriffen. Was
  // das Bild dann noch zeigt, haben wir selbst verursacht.
  if (standardSweeps_ >= 1 && signalLocked_.load(std::memory_order_relaxed) == 0) {
    return false;
  }

  const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
  if (verdict == VideoRenderer::SignalVerdict::Picture ||
      verdict == VideoRenderer::SignalVerdict::Unknown) {
    return true;
  }

  // Neither of the two failure verdicts is acted on the instant it appears.
  //
  // Snow is the confident one, but a hard cut between two busy scenes can clear
  // both its thresholds for a single measurement, and a viewer that blinks the
  // idle screen mid-game is worse than one that takes a moment to notice a
  // pulled cable. A second is far longer than any cut and far shorter than
  // anyone's patience with a dead input.
  //
  // Flat is the ambiguous one: a black loading screen measures exactly like a
  // muted input, so only time separates them at all. A decoder that says it has
  // no lock is reason enough to stop waiting for a second opinion.
  //
  // An einem digitalen Eingang laeuft der Wachthread gar nicht, `locked` ist
  // dann -1 und es bleibt bei der langen Geduld. Das ist genau richtig: dort
  // sitzt der Decoder nicht im Signalweg, seine Meinung waere geraten.
  double patience = kSnowSeconds;
  if (verdict == VideoRenderer::SignalVerdict::Flat) {
    const int locked = signalLocked_.load(std::memory_order_relaxed);
    patience = locked == 0 ? kFlatUnlockedSeconds : kFlatSeconds;
  }
  return renderer_.signalHeldSeconds() < patience;
}

void App::RememberSettingsWindow() {
  if (!settingsHost_.created()) return;
  const SettingsHost::Placement where = settingsHost_.placement();
  if (where.width <= 0 || where.height <= 0) return;
  config_.app.settingsWindowX = where.x;
  config_.app.settingsWindowY = where.y;
  config_.app.settingsWindowW = where.width;
  config_.app.settingsWindowH = where.height;
}

void App::DrawSettingsWindowed() {
  const bool wanted = config_.app.settingsSeparateWindow;

  // Nothing to do, and nothing built: the common case, and it costs one branch.
  if (!wanted && !settingsHost_.created()) return;

  if (wanted && !settingsHost_.created()) {
    std::string error;
    // The font atlas is shared rather than rebuilt -- same glyphs, and one copy
    // on the GPU is enough for both windows.
    // Auspoppen: das Fenster geht dort auf, wo das eingebettete Feld gerade
    // stand. Der Weg fuehrt ueber Bildschirmkoordinaten, weil die beiden in
    // verschiedenen Bezugssystemen leben -- das Feld im Client des
    // Hauptfensters, das Fenster auf dem Desktop.
    //
    // Verglichen werden die *Aussenkanten* beider, nicht ihre Inhalte. Das ist
    // die einzige Zuordnung, die sich nicht um ein paar Pixel verzieht: das
    // eingebettete Feld zaehlt seine eigene Titelleiste zur Flaeche dazu, das
    // freigestellte faengt beim Client unterhalb der Windows-Titelleiste an.
    // Wer Client auf Client abbildet, verschiebt beim Umschalten jedes Mal um
    // die Differenz der beiden Leisten -- einmal nach unten, einmal nach oben.
    SettingsHost::Placement where;
    where.x = config_.app.settingsWindowX;
    where.y = config_.app.settingsWindowY;
    where.width = config_.app.settingsWindowW;
    where.height = config_.app.settingsWindowH;
    if (config_.app.settingsPanelW > 200 && config_.app.settingsPanelH > 200) {
      POINT topLeft = {config_.app.settingsPanelX, config_.app.settingsPanelY};
      ::ClientToScreen(hwnd_, &topLeft);
      where.x = topLeft.x;
      where.y = topLeft.y;
      where.width = config_.app.settingsPanelW;
      where.height = config_.app.settingsPanelH;
    }
    if (!settingsHost_.Create(instance_, hwnd_, d3d_.device(), d3d_.context(),
                              ImGui::GetIO().Fonts, uiScale_, d3d_.tearingSupported(), where,
                              &error)) {
      CAP_WARN("%s", error.c_str());
      config_.app.settingsSeparateWindow = false;
      Toast(error);
      return;
    }
    settingsHost_.ApplyTheme(darkMode_, config_.app.accentColor);
    // While its window is being dragged, Windows keeps the loop to itself. The
    // timer inside that loop is what still lets the picture run.
    // Dragging a window puts Windows into a modal loop of its own that does not
    // return until the mouse is let go, so the main loop stops running and the
    // preview stops with it. A timer inside that loop is the only way back in.
    // It has to do what one turn of the main loop does -- which since the two
    // were separated means the settings window as well as the preview.
    settingsHost_.SetFrameCallback([this]() {
      if (inModalFrame_) return;
      inModalFrame_ = true;
      Tick();
      // The preview only. The dialog's *content* does not change while its
      // frame is being dragged, and redrawing it here means a second present
      // between every mouse movement and the window catching up with it --
      // which turns a frozen preview into a window that lags the cursor.
      //
      // Genau dieselbe Frage wie in der Hauptschleife, und aus demselben Grund:
      // ist ein neues Bild da, ist ein zweites Halbbild faellig, oder ist es zu
      // lange her? Frueher stand hier stattdessen eine Zeitschranke, und jede
      // Zahl, die dort stand, war neben der Kadenz der Quelle -- mal zu
      // langsam, mal gegen sie schwebend.
      //
      // Das Ereignis der Karte laesst sich mit einer Wartezeit von null
      // abfragen; es setzt sich selbst zurueck, also ist das dieselbe
      // Entnahme, die die Hauptschleife sonst macht. Sie laeuft in diesem
      // Moment nicht, also nimmt ihr das nichts weg.
      bool newPicture = false;
      if (FrameSink* sink = capture_.sink()) {
        HANDLE ev = sink->frameEvent();
        newPicture = ev && ::WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
      }
      const int64_t nowQpc = QpcNow();
      const double sinceRenderMs =
          lastRenderQpc_ == 0 ? 1e9 : QpcToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
      const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
      if (newPicture || fieldDue || sinceRenderMs >= 200.0) {
        lastRenderQpc_ = nowQpc;
        RenderFrame();
      }
      inModalFrame_ = false;
    });
  }

  if (!wanted) {
    // Switched off again: put the panel back inside the picture, and give the
    // preview its shortest queue back. Where it stood is remembered first --
    // this is the same object that will be built again if it is switched back
    // on, and it should come up where it was left.
    RememberSettingsWindow();
    // Einbetten: das Feld geht dort auf, wo das Fenster gerade stand. Umgekehrt
    // derselbe Weg -- Client des Fensters auf den Bildschirm, von dort in den
    // Client des Hauptfensters.
    //
    // Liegt das Fenster ganz oder ueberwiegend neben dem Hauptfenster, kommt
    // dabei eine Lage heraus, die das Feld unerreichbar machen wuerde. Das faengt
    // die Wiederherstellung selbst ab und setzt in die Mitte.
    if (HWND host = settingsHost_.hwnd()) {
      RECT outer = {};
      if (::GetWindowRect(host, &outer)) {
        POINT inMain = {outer.left, outer.top};
        ::ScreenToClient(hwnd_, &inMain);
        config_.app.settingsPanelX = inMain.x;
        config_.app.settingsPanelY = inMain.y;
        config_.app.settingsPanelW = outer.right - outer.left;
        config_.app.settingsPanelH = outer.bottom - outer.top;
      }
    }
    settings_.RestorePosition();
    settingsHost_.Destroy();
    return;
  }

  // Closing the window is closing the settings, the same as the button is.
  if (settingsHost_.takeCloseRequest()) settings_.Close();

  if (settings_.isOpen() != settingsHost_.visible()) {
    if (settings_.isOpen()) {
      settingsHost_.Show(ToWide(T("CapView – Einstellungen", "CapView – Settings")));
    } else {
      settingsHost_.Hide();
    }
  }

  // Two swapchains on one device need somewhere to queue. Measured: at a depth
  // of one the dialog's present cost eight to fourteen milliseconds because it
  // was waiting for the preview's frame to retire, and the preview's cost three
  // because it was waiting for the dialog's. At three, both are under a
  // millisecond. Only while the dialog is actually on screen -- the preview
  // wants the shortest queue there is the rest of the time, and that is the
  // single biggest lever on its latency.
  // Die Vorschau behaelt ihre kurze Warteschlange, immer. Frueher musste sie
  // hier auf drei hoch, weil beide Fenster an einem Geraet hingen und sich
  // gegenseitig auf das Present warten liessen; seit der Dialog sein eigenes
  // Geraet hat, geht ihn das nichts mehr an.

  if (!settingsHost_.BeginFrame(darkMode_, config_.app.accentColor)) return;

  settings_.SetFillsWindow(true);
  const SettingsWindow::Result result =
      settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_);
  settingsHost_.EndFrame();

  // Nothing to put back. This runs after the main window has presented, so the
  // targets it wants are set again by the next frame's first pass.

  // Closing is closing, whether it was the footer button or the window's own.
  if (result == SettingsWindow::Result::Close) settings_.Close();
}

void App::OpenReleasePage(const std::string& tag) {
  std::wstring url = L"https://github.com/NuclearMeltdown/CapView/releases";
  if (!tag.empty()) url += L"/tag/" + ToWide(tag);
  ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::UpdateHdr() {
  const VideoFormatInfo& src = renderer_.sourceFormat();

  VideoRenderer::Transfer transfer = VideoRenderer::Transfer::Sdr;
  bool wideGamut = false;
  switch (config_.app.hdrInput) {
    case HdrInput::Pq:
      transfer = VideoRenderer::Transfer::Pq;
      wideGamut = true;
      break;
    case HdrInput::Hlg:
      transfer = VideoRenderer::Transfer::Hlg;
      wideGamut = true;
      break;
    case HdrInput::Sdr:
      break;
    case HdrInput::Auto:
    default:
      // 15 and 16 are PQ and HLG in the DXVA numbering the media type uses.
      if (src.transferFunction == 15) transfer = VideoRenderer::Transfer::Pq;
      if (src.transferFunction == 16) transfer = VideoRenderer::Transfer::Hlg;
      // 9 is BT.2020 primaries; the matrix codes say the same thing a second way.
      wideGamut = src.primaries == 9 || src.transferMatrix == 4 || src.transferMatrix == 5;
      break;
  }
  renderer_.SetHdrInput(transfer, wideGamut);

  // The screen can change without anything else doing so -- dragging the window
  // to another monitor, or turning HDR on in Windows while this runs. Asking
  // DXGI costs a little, so not every frame.
  if (++hdrDisplayPoll_ >= 120) {
    hdrDisplayPoll_ = 0;
    d3d_.RefreshDisplayCapability();
  }

  const D3DContext::DisplayCapability display = d3d_.displayCapability();
  bool want = false;
  switch (config_.app.hdrOutput) {
    case HdrOutput::Always:
      want = display.hdr;
      break;
    case HdrOutput::Auto:
      // Only when there is something to gain. An ordinary picture on an HDR
      // screen goes through one more conversion for no benefit.
      want = display.hdr && transfer != VideoRenderer::Transfer::Sdr;
      break;
    case HdrOutput::Off:
    default:
      break;
  }

  if (want != d3d_.hdrOutput()) {
    std::string error;
    if (!d3d_.SetHdrOutput(want, &error)) {
      // Said once and then left alone, rather than every frame from here on.
      if (!error.empty() && want) {
        config_.app.hdrOutput = HdrOutput::Off;
        Toast(error);
      }
    }
  }

  renderer_.SetHdrOutput(d3d_.hdrOutput(), config_.app.paperWhiteNits,
                         config_.app.sourcePeakNits,
                         d3d_.hdrOutput() ? display.peakNits : 100.0f);

  settings_.SetHdrState(display.hdr, d3d_.hdrOutput(), display.peakNits, (int)transfer);
  settings_.SetCarrierPeriod(renderer_.effectiveCarrierPeriod());
}

void App::DrawUpdatePrompt() {
  // The startup check runs on its own thread, so the result turns up a second or
  // two in. Raised once per session and never again, whatever the user does with
  // it -- a notice that keeps coming back is an advertisement.
  if (!updatePromptRaised_ && updater_.status().announce &&
      updater_.status().state == UpdateStatus::State::Available) {
    updatePromptRaised_ = true;
    updatePromptQueued_ = true;
  }

  const char* id = T("Update verfügbar###capview_update", "Update available###capview_update");
  if (updatePromptQueued_) {
    ImGui::OpenPopup(id);
    updatePromptQueued_ = false;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  const UpdateStatus st = updater_.status();
  ImGui::Text(T("CapView %s ist verfügbar.", "CapView %s is available."),
              st.latestVersion.c_str());
  ImGui::TextDisabled(T("Installiert ist %s.", "This build is %s."), Updater::currentVersion());
  ImGui::Spacing();

  switch (st.state) {
    case UpdateStatus::State::Downloading:
      ImGui::TextDisabled("%s", T("wird geladen ...", "downloading ..."));
      break;
    case UpdateStatus::State::Ready:
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                         T("Eingesetzt. Ein Neustart übernimmt sie.",
                           "Installed. A restart picks it up."));
      break;
    case UpdateStatus::State::Failed:
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1.0f), "%s", UpdateErrorText(st).c_str());
      break;
    default:
      break;
  }

  ImGui::Spacing();
  const float buttonWidth = 130.0f * uiScale_;

  if (st.state == UpdateStatus::State::Ready) {
    if (ImGui::Button(T("Jetzt neu starten", "Restart now"), ImVec2(buttonWidth, 0))) {
      if (updater_.RestartIntoNewBuild()) {
        running_ = false;
      } else {
        Toast(T("Neustart fehlgeschlagen.", "Restart failed."));
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  ImGui::BeginDisabled(updater_.busy());
  if (ImGui::Button(T("Installieren", "Install"), ImVec2(buttonWidth, 0))) {
    updater_.InstallAsync();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Später", "Later"), ImVec2(buttonWidth, 0))) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  if (ImGui::Button(T("Was ist neu", "What is new"), ImVec2(buttonWidth, 0))) {
    // The release page, not the Updates tab. The tab shows the notes trimmed to
    // something that fits; the page has the whole of them, the file, and the
    // history above it.
    OpenReleasePage(st.latestVersion);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::OpenDeviceConfig() {
  if (!capture_.running()) {
    Toast(T("Die Karte läuft nicht.", "The card is not running."));
    return;
  }
  const std::wstring title = ToWide(config_.active().capture.video.name);
  std::string error;
  if (!devicePages_.Open(capture_.filter(), title, &error)) {
    Toast(error.empty() ? T("Der Konfigurationsdialog ließ sich nicht öffnen.",
                            "The configuration dialog could not be opened.")
                        : error);
  }
}

void App::DetectCrop() {
  int left = 0, top = 0, right = 0, bottom = 0;
  if (!renderer_.contentBounds(&left, &top, &right, &bottom)) {
    Toast(T("Noch nichts gemessen. Einen Moment warten.",
            "Nothing measured yet. Give it a moment."));
    return;
  }
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid()) return;

  // Wieviel vom Bild ueberhaupt uebrig bliebe -- und das ist die Frage, die vor
  // dem Zuschneiden zu stellen ist.
  //
  // Die Messung sucht die Grenzen dessen, was nicht schwarz ist, und kann
  // zwischen einem Rand und einer dunklen Stelle nicht unterscheiden. Zeigt
  // eine Konsole gerade nur ihr Startlogo auf Schwarz, ist das gemessene
  // Rechteck das Logo, und alles darum wuerde weggeschnitten: Bildflaeche, die
  // in diesem Moment nur nicht beleuchtet ist.
  //
  // Gemessen wird die Flaeche, nicht die einzelne Kante, und das ist der
  // Unterschied, auf den es ankommt: ein echter Rand frisst eine Richtung, ein
  // Logo auf Schwarz frisst beide.
  //
  // Der breiteste Rand, der noch einer ist, ist ein Kinoformat in 4:3 --
  // 2,35:1 laesst 57 Prozent der Hoehe und damit auch 57 Prozent der Flaeche
  // stehen. Ein 4:3-Bild in einem 16:9-Signal laesst 75 Prozent der Breite.
  // Der Startbildschirm des GameCube dagegen, an einem 720x576-Signal
  // nachgerechnet: 45 Prozent der Breite, 73 Prozent der Hoehe, zusammen 33
  // Prozent der Flaeche -- ueber die Kanten allein waeren das nur fuenf Punkte
  // Abstand zur Haelfte, ueber die Flaeche sind es siebzehn.
  //
  // Die zweite Schranke ist nur gegen den entarteten Fall: ein schmaler
  // Streifen kann die halbe Flaeche halten und trotzdem kein Rand sein.
  const int keptW = right - left + 1;
  const int keptH = bottom - top + 1;
  const double partW = format.width > 0 ? (double)keptW / format.width : 1.0;
  const double partH = format.height > 0 ? (double)keptH / format.height : 1.0;
  if (partW * partH < 0.5 || partW < 0.4 || partH < 0.4) {
    char text[240];
    std::snprintf(text, sizeof(text),
                  T("Da blieben nur %.0f Prozent des Bildes stehen (%.0f x %.0f). Das sieht "
                    "nach einem Logo auf Schwarz aus, nicht nach einem Rand -- erst ein "
                    "richtiges Bild der Konsole abwarten.",
                    "That would leave only %.0f per cent of the picture (%.0f x %.0f). It "
                    "looks like a logo on black rather than a border -- wait for a real "
                    "picture from the console first."),
                  partW * partH * 100.0, partW * 100.0, partH * 100.0);
    Toast(text);
    CAP_LOG("Zuschnitt verworfen: nur %dx%d von %dx%d uebrig (%.0f %% der Flaeche)", keptW, keptH,
            format.width, format.height, partW * partH * 100.0);
    return;
  }

  ImageSettings& img = config_.active().image;
  const int cl = left;
  const int ct = top;
  const int cr = format.width - 1 - right;
  const int cb = format.height - 1 - bottom;
  // A border of a pixel or two is measurement noise on an analogue input, not a
  // border, and cropping it would only cost resolution.
  const int floorPx = 3;
  img.cropLeft = cl >= floorPx ? cl : 0;
  img.cropTop = ct >= floorPx ? ct : 0;
  img.cropRight = cr >= floorPx ? cr : 0;
  img.cropBottom = cb >= floorPx ? cb : 0;

  if (img.cropLeft || img.cropRight || img.cropTop || img.cropBottom) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  T("Rand erkannt: links %d, rechts %d, oben %d, unten %d",
                    "Border found: left %d, right %d, top %d, bottom %d"),
                  img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
    Toast(text);
  } else {
    Toast(T("Kein schwarzer Rand gefunden.", "No black border found."));
  }
}

// Ein Zuschnitt gilt fuer die Groesse, an der er gemessen wurde.
//
// Die vier Zahlen sind Bildpunkte der Quelle, nicht Anteile: "links 20" heisst
// zwanzig Punkte von 720. Wechselt die Norm von 525 auf 625 Zeilen, wechselt
// mit ihr die Bildhoehe, und "oben 17" beschreibt dann einen anderen Streifen
// als den gemessenen. Im besten Fall steht ein schmaler schwarzer Rand wieder
// im Bild, im schlechteren wird echter Bildinhalt weggeschnitten -- und beides
// sieht nicht nach einer Einstellung aus, die noch von vorhin steht, sondern
// nach einem kaputten Bild.
//
// Zurueckgesetzt statt neu gemessen, und das ist eine Entscheidung. Ein
// automatischer zweiter Anlauf laege nahe -- die Norm hat gerade gewechselt,
// gleich einmal nachmessen --, aber genau in diesem Moment ist das Bild am
// wenigsten dazu geeignet: der Graph ist eben erst wieder aufgebaut, die
// Konsole schaltet gerade um oder faehrt hoch, und was anliegt, ist ein
// Startlogo auf Schwarz oder noch gar nichts. Die Messung hat gegen genau
// diesen Fall bereits eine Schranke (siehe DetectCrop), aber sie noch dazu
// ungefragt in ihn hineinzuschicken hiesse, sie gegen ihre eigene Schranke
// laufen zu lassen. Also: sauber aufraeumen, es sagen, und die Entscheidung
// dem ueberlassen, der das Bild sieht -- der Knopf dafuer liegt jetzt im
// Rechtsklickmenue.
void App::UpdateCropForFormat() {
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid()) return;
  // Waehrend des Ziehens sind die Zahlen ohnehin auf null gesetzt und das
  // Format zu merken waere verfrueht.
  if (cropPick_.active) return;

  const int w = fmt.width;
  const int h = fmt.height;
  if (cropFormatWidth_ == w && cropFormatHeight_ == h) return;

  // Das erste Format einer Sitzung hat nichts geaendert; es ist das, wofuer
  // die gespeicherten Zahlen gelten sollen.
  const bool first = cropFormatWidth_ == 0 && cropFormatHeight_ == 0;
  cropFormatWidth_ = w;
  cropFormatHeight_ = h;
  if (first) return;

  ImageSettings& img = config_.active().image;
  if (!img.cropLeft && !img.cropRight && !img.cropTop && !img.cropBottom) return;

  CAP_LOG("Zuschnitt zurueckgesetzt: Quelle jetzt %dx%d (Rand war links %d, rechts %d, oben %d, "
          "unten %d)",
          w, h, img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
  img.cropLeft = 0;
  img.cropRight = 0;
  img.cropTop = 0;
  img.cropBottom = 0;
  Toast(Format(T("Videoformat geändert (%dx%d) — Zuschnitt zurückgesetzt.",
                 "Video format changed (%dx%d) — crop reset."),
               w, h));
}

// Ob das Bild aus dem Analogdekoder kommt. Dieselbe Frage, die entscheidet, was
// in den Einstellungen erscheint -- und sie muss dieselbe Antwort geben, sonst
// wirkt etwas, das nirgends mehr zu sehen ist.
//
// Einen Dekoder zu haben heisst nicht, ihn zu benutzen: auf einer Karte, die
// beides kann, wird er weiterhin gemeldet, waehrend das Bild vom digitalen
// Eingang kommt. Keine Videonorm liefert mehr als 576 Zeilen, also beantwortet
// das Bild die Frage selbst.
bool App::SourceIsAnalogue() const {
  const SignalKind kind = config_.active().capture.signalKind;
  if (kind == SignalKind::Analog) return true;
  if (kind == SignalKind::Digital) return false;
  const bool hasDecoder = capture_.running() && capture_.capabilities().availableStandards != 0;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  return hasDecoder && (!fmt.valid() || fmt.height <= 576);
}

// Die Einstellungen, wie sie fuer *diese* Quelle gelten.
//
// Ausgeblendet muss auch abgeschaltet heissen. Wer am SNES die Kriechfilter und
// das native Raster anhatte und dann eine HD-Konsole ansteckt, saehe sonst ein
// Bild, das er nicht will, und faende den Regler dafuer nirgends mehr -- die
// Einstellung ist ja gerade verschwunden, weil sie nicht passt.
//
// Neutralisiert wird nur die Kopie, die gezeichnet wird. Das Profil behaelt
// seine Werte, denn es beschreibt eine Konsole, und die kommt wieder. Was es
// nicht tut, ist sich zu merken und wiederherzustellen: die naechste analoge
// Quelle ist vielleicht eine andere Konsole mit anderen Werten. Genau dafuer
// gibt es Profile.
ImageSettings App::EffectiveImage(const Profile& profile) const {
  ImageSettings img = profile.image;
  const VideoFormatInfo fmt = renderer_.sourceFormat();

  if (!SourceIsAnalogue()) {
    // Composite bringt diese Stoerungen mit, ein digitaler Eingang nicht. Der
    // Demodulator wuerde einen Traeger herausrechnen, den es nicht gibt.
    img.chromaSoft = 0;
    img.temporalDenoise = 0.0f;
    img.dotNotch = 0.0f;
    // Das native Raster rechnet das Abtasten einer analogen Zeile zurueck.
    img.nativeWidth = 0;
  }

  // Verdoppeln nur, wo wirklich die halbe Bildhoehe ankommt.
  //
  // Das Kaestchen wird sonst gar nicht erst gezeigt, aber ein gespeichertes
  // Haekchen aus einer Sitzung mit 240p-Quelle wuerde hier weiterwirken -- und
  // zwar unsichtbar: das Fenster passt das Bild auf das eingestellte
  // Seitenverhaeltnis, die Aufnahme dagegen kaeme doppelt so hoch heraus.
  if (!SourceIsAnalogue() || !fmt.valid() || fmt.height > kHalfHeightLines) {
    img.lineDouble = false;
  }

  // Bildroehreneffekte nach der Zeilenzahl, nicht nach analog nur digital: ein
  // RetroTINK mit 480p ueber HDMI soll sie behalten duerfen.
  if (fmt.valid() && fmt.height > 576) {
    img.scanlines = 0.0f;
    img.mask = 0;
  }

  // Halbbilder: hat die Quelle keine, darf ein von Hand gewaehlter Deinterlacer
  // nicht trotzdem laufen. Nicht abschalten, sondern auf "nur bei interlaced"
  // stellen -- das ist dieselbe Aussage und ueberlebt den Wechsel zurueck.
  const bool hasFields =
      fmt.interlaced || renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
  if (!SourceIsAnalogue() && !hasFields) img.deinterlaceAuto = true;

  return img;
}

// Ob das gemessene "interlaced" bei dieser Quelle nach einem Irrtum aussieht.
//
// Kein Veto, sondern eine Nachfrage, und das mit Absicht: die Messung kann hier
// nicht widerlegt werden. Ein bildschirmfuellendes Foto von Kammlinien ist
// raeumlich von Kammlinien nicht zu unterscheiden, und genau das steht auf
// einem erfassten Desktop schnell einmal im Browser. Also entscheidet weiter
// die Messung, und danebengestellt wird der Satz, den ein Mensch braucht, um
// den Fehler in zwei Sekunden selbst zu beheben.
//
// Vier Bedingungen, jede mit einem eigenen Grund:
// * Die Karte hat *nicht* selbst interlaced gemeldet. Sagt sie es, stimmt es,
//   und dann gibt es nichts zu bezweifeln.
// * Die Quelle ist digital. An einem Analogeingang ist interlaced der
//   Normalfall und die Messung ohnehin die einzige Auskunft.
// * Mindestens 720 Zeilen. Bei 720 ist die Sache klar -- ein 720i hat es nie
//   gegeben. Bei 1080 ist sie es nicht, 1080i gab es im Fernsehen wirklich,
//   und deshalb steht hier eine Frage und keine Behauptung. Darunter, etwa bei
//   480i von einem DVD-Spieler ueber HDMI, ist interlaced schlicht richtig.
//
// Die Haelfte dieser Faelle beantwortet inzwischen die Bildrate von selbst:
// kommen bei 720 Zeilen oder mehr fuenfzig oder sechzig Bilder in der Sekunde
// an, laesst die Erkennung "interlaced" gar nicht mehr zu (siehe
// VideoRenderer::SetFrameRateHint). Uebrig bleibt hier also die Quelle, die mit
// 25 oder 30 Bildern ankommt und kaemmt -- und die kann eben beides sein, ein
// echtes 1080i und ein erfasster Bildschirm, auf dem ein Video davon laeuft.
// * Die automatische Erkennung ist eingeschaltet. Ist sie es nicht, laeuft der
//   Deinterlacer sowieso und die Messung aendert am Bild nichts -- vor etwas zu
//   warnen, das gar nicht wirkt, zeigt in die falsche Richtung.
bool App::InterlaceVerdictDoubtful(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return false;
  if (renderer_.detectedInterlace() != VideoRenderer::InterlaceVerdict::Interlaced) return false;
  const VideoFormatInfo fmt = renderer_.sourceFormat();
  if (!fmt.valid() || fmt.interlaced) return false;
  if (SourceIsAnalogue()) return false;
  return fmt.height >= 720;
}

bool App::SourceLooksInterlaced(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return true;
  // The media type is believed when it claims interlaced -- a card that bothers
  // to say so is right. It is not believed when it stays quiet, which is the
  // usual case on an analogue input and is why the picture is measured as well.
  if (renderer_.sourceFormat().interlaced) return true;
  return renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
}

void App::BeginCropPick() {
  if (cropPick_.active) return;
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid() || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Zuschneiden.", "No picture to crop."));
    return;
  }

  ImageSettings& img = config_.active().image;
  cropPick_.saved = img;
  cropPick_.left = img.cropLeft;
  cropPick_.right = img.cropRight;
  cropPick_.top = img.cropTop;
  cropPick_.bottom = img.cropBottom;
  cropPick_.drag = -1;
  cropPick_.active = true;

  // Show the full frame underneath, so screen position maps straight to source
  // pixels and the handles start where the current crop is.
  img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  settings_.Close();
}

void App::EndCropPick(bool apply) {
  if (!cropPick_.active) return;
  ImageSettings& img = config_.active().image;
  img = cropPick_.saved;
  if (apply) {
    img.cropLeft = cropPick_.left;
    img.cropRight = cropPick_.right;
    img.cropTop = cropPick_.top;
    img.cropBottom = cropPick_.bottom;
    CAP_LOG("Zuschnitt gesetzt: links %d, rechts %d, oben %d, unten %d", img.cropLeft,
            img.cropRight, img.cropTop, img.cropBottom);
  }
  cropPick_.active = false;
  OpenSettings({});
}

void App::DrawCropPicker() {
  const VideoFormatInfo format = renderer_.sourceFormat();
  const RECT& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!format.valid() || rw < 8.0f || rh < 8.0f) {
    EndCropPick(false);
    return;
  }

  const float srcW = (float)format.width;
  const float srcH = (float)format.height;
  const float scaleX = rw / srcW;
  const float scaleY = rh / srcH;

  // Source pixels <-> client pixels.
  auto toScreenX = [&](int src) { return (float)r.left + (float)src * scaleX; };
  auto toScreenY = [&](int src) { return (float)r.top + (float)src * scaleY; };
  auto toSrcX = [&](float screen) { return (int)std::lround((screen - (float)r.left) / scaleX); };
  auto toSrcY = [&](float screen) { return (int)std::lround((screen - (float)r.top) / scaleY); };

  // Hintergrundliste, nicht Vordergrundliste: ImGui zeichnet die Vordergrundliste
  // nach allen Fenstern, die Hintergrundliste davor. Beide liegen ueber dem
  // Video, denn das Bild kommt gar nicht aus ImGui -- es steht schon im
  // Rueckpuffer, bevor hier irgendetwas gezeichnet wird.
  //
  // Im Vordergrund lag die Abdunklung ueber der eigenen Werkzeugleiste: zieht
  // man eine Kante ueber sie hinweg, waechst das abgedunkelte Feld darueber und
  // Übernehmen und Abbrechen werden unlesbar -- genau in dem Moment, in dem man
  // sie braucht. Dieselbe Ordnung, die der Ruhebildschirm in overlay.cpp
  // benutzt, und aus demselben Grund.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 mouse = ImGui::GetMousePos();

  float xL = toScreenX(cropPick_.left);
  float xR = toScreenX(format.width - cropPick_.right);
  float yT = toScreenY(cropPick_.top);
  float yB = toScreenY(format.height - cropPick_.bottom);

  // ---- grab handling ----
  // Everything outside the picture is ignored, so dragging the window or using
  // the buttons above still works.
  const float grab = 10.0f;
  const bool overVideo = mouse.x >= r.left - grab && mouse.x <= r.right + grab &&
                         mouse.y >= r.top - grab && mouse.y <= r.bottom + grab;

  int hot = -1;
  if (cropPick_.drag >= 0) {
    hot = cropPick_.drag;
  } else if (overVideo && !ImGui::GetIO().WantCaptureMouse) {
    float best = grab;
    if (std::abs(mouse.x - xL) < best) { best = std::abs(mouse.x - xL); hot = 0; }
    if (std::abs(mouse.x - xR) < best) { best = std::abs(mouse.x - xR); hot = 1; }
    if (std::abs(mouse.y - yT) < best) { best = std::abs(mouse.y - yT); hot = 2; }
    if (std::abs(mouse.y - yB) < best) { best = std::abs(mouse.y - yB); hot = 3; }
  }

  if (hot >= 0) {
    ImGui::SetMouseCursor(hot < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  }
  if (hot >= 0 && cropPick_.drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    cropPick_.drag = hot;
  }
  if (cropPick_.drag >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    cropPick_.drag = -1;
  }

  if (cropPick_.drag >= 0) {
    // At least sixteen source pixels have to survive in each direction --
    // a zero sized picture is not a crop, it is a crash waiting to happen.
    const int minKeep = 16;
    switch (cropPick_.drag) {
      case 0:
        cropPick_.left = Clamp(toSrcX(mouse.x), 0, format.width - cropPick_.right - minKeep);
        break;
      case 1:
        cropPick_.right =
            Clamp(format.width - toSrcX(mouse.x), 0, format.width - cropPick_.left - minKeep);
        break;
      case 2:
        cropPick_.top = Clamp(toSrcY(mouse.y), 0, format.height - cropPick_.bottom - minKeep);
        break;
      case 3:
        cropPick_.bottom =
            Clamp(format.height - toSrcY(mouse.y), 0, format.height - cropPick_.top - minKeep);
        break;
      default: break;
    }
    xL = toScreenX(cropPick_.left);
    xR = toScreenX(format.width - cropPick_.right);
    yT = toScreenY(cropPick_.top);
    yB = toScreenY(format.height - cropPick_.bottom);
  }

  // ---- painting ----
  const ImU32 dim = IM_COL32(0, 0, 0, 150);
  dl->AddRectFilled(ImVec2((float)r.left, (float)r.top), ImVec2(xL, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xR, (float)r.top), ImVec2((float)r.right, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xL, (float)r.top), ImVec2(xR, yT), dim);
  dl->AddRectFilled(ImVec2(xL, yB), ImVec2(xR, (float)r.bottom), dim);

  const ImU32 line = IM_COL32(255, 255, 255, 230);
  const ImU32 lineHot = IM_COL32(255, 200, 80, 255);
  dl->AddRect(ImVec2(xL, yT), ImVec2(xR, yB), line, 0.0f, 0, 1.5f);

  // A thicker bar on each edge, so there is something obvious to aim at.
  const float bar = 4.0f;
  dl->AddRectFilled(ImVec2(xL - bar * 0.5f, yT), ImVec2(xL + bar * 0.5f, yB),
                    hot == 0 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xR - bar * 0.5f, yT), ImVec2(xR + bar * 0.5f, yB),
                    hot == 1 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yT - bar * 0.5f), ImVec2(xR, yT + bar * 0.5f),
                    hot == 2 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yB - bar * 0.5f), ImVec2(xR, yB + bar * 0.5f),
                    hot == 3 ? lineHot : line);

  // ---- toolbar ----
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 18.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (ImGui::Begin("##croptools", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::TextUnformatted(T("Ränder ziehen", "Drag the edges"));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("links %d  rechts %d  oben %d  unten %d", "left %d  right %d  top %d  bottom %d"),
                cropPick_.left, cropPick_.right, cropPick_.top, cropPick_.bottom);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("Ergebnis %dx%d", "Result %dx%d"),
                format.width - cropPick_.left - cropPick_.right,
                format.height - cropPick_.top - cropPick_.bottom);

    ImGui::SameLine();
    if (ImGui::Button(T("Übernehmen", "Apply"))) {
      EndCropPick(true);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"))) {
      EndCropPick(false);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Nichts##crop", "None##crop"))) {
      cropPick_.left = cropPick_.right = cropPick_.top = cropPick_.bottom = 0;
    }
  }
  ImGui::End();
}

void App::FeedRecorder() {
  if (!recorder_.recording()) return;

  // Splitting is a restart, not a seamless cut: ffmpeg has to close the
  // container it is writing. A fraction of a second is lost at the boundary,
  // which is why this is off unless someone really is on FAT32.
  if (config_.record.splitFiles) {
    const double now = ImGui::GetTime();
    if (now - lastSplitCheck_ > 1.0) {
      lastSplitCheck_ = now;
      const uint64_t limit = (uint64_t)config_.record.splitSizeMb * 1024ull * 1024ull;
      if (recorder_.outputFileSize() >= limit) {
        CAP_LOG("Aufnahme: Größenlimit erreicht, neue Datei");
        StopRecording();
        StartRecording();
        return;
      }
    }
  }

  // ffmpeg died on its own: stop cleanly rather than filling a dead pipe.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    StopRecording();
    Toast(stats.error.empty() ? T("Aufnahme abgebrochen.", "Recording aborted.") : stats.error);
    return;
  }

}

void App::FeedFrameConsumers() {
  const bool wantRecorder = recorder_.recording();
  // Only while something is actually watching. An idle camera costs a flag.
  const bool wantCamera = virtualCamera_.running() && virtualCamera_.consumed();

  renderer_.SetHdrWideWanted(wantRecorder && config_.app.recordHdr,
                             wantCamera && virtualCamera_.wantsWide());
  renderer_.SetReadbackEnabled(wantRecorder || wantCamera);
  if (!wantRecorder && !wantCamera) return;

  // Either of the two can be on the wide path while the other is not, so both
  // rings can be live at once. They are read in one place so a frame is never
  // fetched twice and never half handed out.
  const bool wide = renderer_.hdrWideActive();
  const bool recordWide = wantRecorder && wide && config_.app.recordHdr;
  const bool cameraWide = wantCamera && wide && virtualCamera_.wantsWide();

  if (recordWide || cameraWide) {
    VideoRenderer::ReadbackFrame w;
    if (renderer_.FetchHdrReadback(&w)) {
      if (recordWide) recorder_.PushVideo(w.data, w.stride);
      if (cameraWide) virtualCamera_.PushFrameWide(w.data, w.stride, w.width, w.height);
      renderer_.ReleaseHdrReadback();
    }
  }

  if (!(wantCamera && !cameraWide) && !(wantRecorder && !recordWide)) return;

  VideoRenderer::ReadbackFrame frame;
  if (!renderer_.FetchReadback(&frame)) return;
  if (wantRecorder && !recordWide) recorder_.PushVideo(frame.data, frame.stride);
  if (wantCamera && !cameraWide) {
    virtualCamera_.PushFrame(frame.data, frame.stride, frame.width, frame.height);
  }
  renderer_.ReleaseReadback();
}

void App::UpdateVirtualCamera() {
  const int request = settings_.takeVirtualCameraRequest();
  if (request != 0) {
    // Installing while it runs would pull the source out from under a reader,
    // so it goes off first either way.
    const bool wasOn = config_.app.virtualCamera;
    if (virtualCamera_.running()) virtualCamera_.Stop();

    std::string error;
    const bool ok = request == 1 ? VirtualCamera::InstallSource(&error)
                                 : VirtualCamera::UninstallSource(&error);
    if (ok) {
      Toast(request == 1 ? T("Kamera installiert.", "Camera installed.")
                         : T("Kamera deinstalliert.", "Camera uninstalled."));
      if (request == 2) config_.app.virtualCamera = false;
    } else {
      Toast(error);
      config_.app.virtualCamera = wasOn;
    }
  }

  virtualCamera_.SetWideOffered(config_.app.cameraHdr);

  const bool want = config_.app.virtualCamera;
  if (want && !virtualCamera_.running() && !virtualCamera_.starting()) {
    virtualCamera_.StartAsync();
  } else if (!want && (virtualCamera_.running() || virtualCamera_.starting())) {
    virtualCamera_.Stop();
  }

  std::string startError;
  if (virtualCamera_.takeError(&startError)) {
    // Turning the switch back off rather than leaving it on and doing nothing,
    // so the tab does not claim a camera that is not there.
    config_.app.virtualCamera = false;
    Toast(startError);
  }

  // What the camera would hand out right now, written whether or not anybody is
  // listening. A consumer reads all of this the moment it connects -- before it
  // has asked for a frame -- so it has to be there beforehand rather than be
  // discovered from the first picture.
  const VideoFormatInfo& format = renderer_.sourceFormat();
  const bool wide = config_.app.cameraHdr &&
                    renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
  virtualCamera_.SetSourceShape(renderer_.outputWidth(), renderer_.outputHeight(), format.fps,
                                wide);

  virtualCamera_.consumers(&virtualCameraConsumers_);
  settings_.SetVirtualCameraState(virtualCamera_.running(), virtualCameraConsumers_);
}

void App::ShowVolumeOsd() {
  // The readout replaces a toast here: a number plus a bar says more than a
  // line of text, and it is what you want to see while a game is running.
  if (config_.app.showVolumeOsd) {
    volumeOsdStart_ = ImGui::GetTime();
  } else {
    const AudioSettings& a = config_.active().audio;
    Toast(a.mute ? T("Stumm", "Muted")
                 : Format(T("Lautstärke %.0f %%", "Volume %.0f %%"), a.volume * 100.0f));
  }
}

// ------------------------------------------------------------------ main loop

int App::Run() {
  MSG msg = {};
  while (running_) {
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running_ = false;
        break;
      }
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    if (!running_) break;

    Tick();

    if (minimized_) {
      // Nothing to draw; block on messages so we use no CPU at all.
      ::WaitMessage();
      continue;
    }

    // The preview is drawn when there is a new picture to draw, and at no
    // other time.
    //
    // This is the second attempt at pacing it and the first was wrong in a way
    // worth recording. Waking on any message and redrawing was clearly wrong --
    // measured, the whole pipeline ran 235 times a second for a source
    // delivering 25. But replacing it with a clock was no better: at a fixed
    // 33 ms against a 25 fps source the two beat against each other, which is
    // judder of exactly the kind the change was meant to remove. A source has a
    // cadence; anything that is not that cadence is wrong.
    //
    // So: the frame event, a second field falling due, and a slow floor that
    // only matters when no pictures are arriving at all -- with a source
    // running, everything on screen animates at the source's rate anyway.
    const int64_t nowQpc = QpcNow();
    const double sinceRenderMs =
        lastRenderQpc_ == 0 ? 1e9 : QpcToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
    const bool wokeOnPicture = lastWait_ == WAIT_OBJECT_0 && lastWaitHadEvent_;
    // Due by the clock, not by *how* the wait ended.
    //
    // This used to read `lastWait_ == WAIT_TIMEOUT`, which is only ever true
    // when nothing else woke the loop at all. Open the settings and there is a
    // steady stream of messages, so the wait returns "input available" every
    // time and the timeout branch never runs -- on an interlaced source that
    // silently dropped every second field for as long as the dialog was open,
    // because the next arriving picture takes the other branch and resets the
    // field index before the second one was ever drawn.
    //
    // The same shape of mistake as the WM_TIMER that could not compete with the
    // drag loop's message flood: a schedule must not be conditional on the
    // queue being quiet.
    const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
    const double idleFloor = IdleFloorMs();
    if (wokeOnPicture || fieldDue || sinceRenderMs >= idleFloor) {
      lastRenderQpc_ = nowQpc;
      RenderFrame();
    }

    // And the settings window is drawn on its own account, every time round,
    // with its own throttle inside. It wants to follow the mouse; the preview
    // wants to follow the capture card. Tying them together made one of them
    // wrong whichever rate was chosen.
    DrawSettingsWindowed();

    // Wait for the next captured frame, a pending second field, or input.
    //
    // This bounds how long the loop *sleeps*, not how often it draws. It used
    // to be both, back when any wake-up redrew the preview -- 16 rather than 8
    // was the ceiling that stopped the settings dialog from driving the whole
    // video pipeline at a hundred and twenty-five times a second for the sake
    // of feeling responsive. Since the redraw became conditional on a picture
    // having arrived, the ceiling is gone: a source delivering two hundred and
    // forty frames signals the event two hundred and forty times and gets two
    // hundred and forty redraws, and this timeout never comes into it.
    // Short while the dialog is open, because that is what keeps *it* smooth;
    // it no longer costs the preview anything, since a wake-up without a
    // picture no longer redraws the preview.
    // Ein Boden nuetzt nichts, wenn die Schleife laenger schlaeft als er lang
    // ist. Solange die Einstellungen offen sind kurz, weil das freigestellte
    // Fenster jede Runde gezeichnet wird; sonst so kurz, wie der Boden es
    // verlangt, und hoechstens 100 ms.
    DWORD timeout = settings_.isOpen() ? 16 : (DWORD)Clamp((int)idleFloor, 16, 100);
    if (secondFieldPending_) {
      // Rounded up, not truncated. Truncating asks to be woken a fraction of a
      // millisecond before the field is due, at which point the loop finds it is
      // not due yet, redraws the same field for nothing and then spins on a zero
      // timeout until it is. Waiting the extra millisecond costs a millisecond
      // and saves all of that.
      const double waitMs = QpcToSeconds(secondFieldQpc_ - QpcNow()) * 1000.0;
      timeout = (DWORD)Clamp((int)std::ceil(waitMs), 0, (int)timeout);
    }
    HANDLE waits[1] = {nullptr};
    DWORD waitCount = 0;
    if (capture_.sink() && capture_.sink()->frameEvent()) {
      waits[0] = capture_.sink()->frameEvent();
      waitCount = 1;
    }
    lastWaitHadEvent_ = waitCount > 0;
    lastWait_ = ::MsgWaitForMultipleObjectsEx(waitCount, waitCount ? waits : nullptr, timeout,
                                              QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  return 0;
}

void App::Tick() {
  UpdatePowerRequest();
  CollectEncoderProbe();
  // Erst entscheiden, ob der Decoder ueberhaupt befragt wird, dann das
  // Ergebnis benutzen. Beim Start der Aufnahme steht das Format noch nicht
  // fest, der Wachthread laeuft also zunaechst an und wird hier ein Bild
  // spaeter wieder angehalten, sobald sich die Quelle als digital erweist.
  UpdateSignalWatch();
  UpdateVideoStandard();
  UpdateCropForFormat();

  if (captureState_ == CaptureState::Running) {
    std::string message;
    if (capture_.PumpEvents(&message)) {
      CAP_WARN("Capture unterbrochen: %s", message.c_str());
      captureError_ = message;
      captureState_ = CaptureState::Reconnecting;
      capture_.Stop();
      audio_.Stop();
      nextRetryQpc_ = QpcNow() + SecondsToQpc(kRetrySeconds);
      retryCount_ = 0;
    }
  }

  // While the settings dialog is open the user is presumably fixing exactly
  // this, so retrying behind their back only produces noise and blocks the
  // device they are about to pick.
  if (captureState_ == CaptureState::Reconnecting && !settings_.isOpen() &&
      QpcNow() >= nextRetryQpc_) {
    ++retryCount_;
    std::string error;
    if (StartCapture(&error)) {
      CAP_LOG("Wieder verbunden nach %d Versuchen", retryCount_);
      Toast(T("Wieder verbunden", "Reconnected"));
    } else {
      captureError_ = error;
      // Back off once it is clear this is not a brief hiccup.
      const double wait = retryCount_ >= kRetryBackoffAfter ? kRetrySlowSeconds : kRetrySeconds;
      nextRetryQpc_ = QpcNow() + SecondsToQpc(wait);
    }
  }

  if (audio_.failed()) {
    CAP_WARN("Audio ausgefallen, starte neu");
    StartAudio();
  }

  // Hide the pointer once it has been still for a while in fullscreen.
  if (fullscreen_ && config_.app.hideCursorFullscreen && !settings_.isOpen()) {
    if (!cursorHidden_ && QpcToSeconds(QpcNow() - lastMouseMoveQpc_) > kCursorIdleSeconds) {
      ::ShowCursor(FALSE);
      cursorHidden_ = true;
    }
  } else if (cursorHidden_) {
    ::ShowCursor(TRUE);
    cursorHidden_ = false;
  }
}

void App::RenderFrame() {
  const int64_t now = QpcNow();
  const Profile& profile = config_.active();

  // ---- pull the newest frame ----
  FrameSink* sink = capture_.sink();
  bool haveNewFrame = false;
  if (sink) {
    FrameView view;
    if (sink->AcquireFrame(&view) && view.valid()) {
      if (!sawFirstFrame_) {
        sawFirstFrame_ = true;
        CAP_LOG("Erstes Bild nach %.0f ms empfangen (%zu Byte)",
                QpcToSeconds(now - captureStartQpc_) * 1000.0, view.size);
      }
      renderer_.SetSourceFormat(sink->format(), nullptr);
      if (delayLine_.active()) {
        delayLine_.Push(view, now);
      } else {
        renderer_.UploadFrame(view);
        haveNewFrame = true;
      }
    }
    if (delayLine_.active()) {
      FrameView delayed;
      if (delayLine_.Pop(&delayed, now)) {
        renderer_.UploadFrame(delayed);
        haveNewFrame = true;
      }
    }
  }

  // ---- bob deinterlacing: second field of the previous frame ----
  const VideoFormatInfo format = renderer_.sourceFormat();
  const bool deinterlacing =
      profile.image.deinterlace != Deinterlace::Off && SourceLooksInterlaced(profile);

  // Which field is the earlier one. The media type is asked first and is usually
  // silent on an analogue card, in which case top-field-first is the convention
  // for standard definition -- but a wrong guess here does not soften the
  // picture, it makes it jump: the two fields are shown in the wrong order, so
  // every frame steps back half a frame and then forward again. Hence the
  // override in the settings, which is the only reliable fix when the card says
  // nothing.
  int firstField = format.fieldOneFirst ? 0 : 1;
  if (profile.image.fieldOrder == FieldOrder::TopFirst) firstField = 0;
  else if (profile.image.fieldOrder == FieldOrder::BottomFirst) firstField = 1;

  if (haveNewFrame) {
    fieldIndex_ = firstField;
    if (deinterlacing) ++fieldsShown_[0];
    if (deinterlacing) {
      // The announced rate and the delivered rate are not always the same
      // number. This card announces 50 fps and hands over 25 woven frames a
      // second -- half the rate, twice the content per frame. Splitting the
      // announced interval would put the second field on screen 10 ms in and
      // then leave it there for 30, which judders harder than not deinterlacing
      // at all, so the measured arrival rate wins whenever there is one.
      double frameSeconds = format.fps > 1.0 ? 1.0 / format.fps : 1.0 / 60.0;
      const double measured = sink ? sink->stats().sourceFps : 0.0;
      if (measured > 1.0) frameSeconds = 1.0 / measured;

      // Counted from when the frame arrived, not from when we got round to
      // drawing it. Those are not the same instant: the log has shown the
      // displayed frame to be twenty milliseconds old, and half a frame after
      // *that* falls past the arrival of the next one -- at which point the
      // second field is never shown at all. Half the frames then get both
      // fields and half get one, which is exactly what a juddering picture is.
      const int64_t arrival = sink && sink->lastArrivalQpc() != 0 ? sink->lastArrivalQpc() : now;
      const int64_t period = SecondsToQpc(frameSeconds);

      // The card delivers when the driver gets round to it -- the graph runs
      // without a reference clock on purpose -- and measured here the arrivals
      // wander by ten milliseconds either way. Half a frame after each raw
      // arrival therefore lands anywhere, and bob ends up showing one field for
      // five milliseconds and the next for twenty-eight. The average is exactly
      // right and the picture stutters anyway.
      //
      // So the next arrival is predicted from a phase that is nudged towards the
      // arrivals rather than following each one, and the second field is put
      // halfway between the frame we have and the frame we expect. A late frame
      // then shortens both of its fields equally instead of crushing one of them.
      if (framePhaseQpc_ == 0 || std::llabs(arrival - framePhaseQpc_) > period) {
        framePhaseQpc_ = arrival;  // first frame, or the stream jumped
      } else {
        framePhaseQpc_ += (arrival - framePhaseQpc_) / 8;
      }
      const int64_t expectedNext = framePhaseQpc_ + period;
      framePhaseQpc_ = expectedNext;

      int64_t gap = (expectedNext - arrival) / 2;
      const int64_t minGap = period / 4;
      const int64_t maxGap = period * 3 / 4;
      if (gap < minGap) gap = minGap;
      if (gap > maxGap) gap = maxGap;
      secondFieldQpc_ = arrival + gap;
      secondFieldPending_ = true;
    } else {
      secondFieldPending_ = false;
    }
  } else if (secondFieldPending_ && now >= secondFieldQpc_) {
    fieldIndex_ = 1 - firstField;
    secondFieldPending_ = false;
    ++fieldsShown_[1];
  }

  // ---- draw ----
  float clear[4];
  GetBackgroundColor(darkMode_, config_.app.accentColor, clear);
  if (!d3d_.BeginFrame(clear)) return;

  // Decided before drawing, so the picture is laid out around the bar in the
  // same frame the bar appears in.
  renderer_.SetTopInset(toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0);
  UpdateHdr();
  renderer_.Draw(EffectiveImage(profile), fieldIndex_);

  // Right after the first pass, so the still is the picture that was just put on
  // screen -- and before the UI is drawn, so the overlay never lands in it. The
  // other setting takes the same shot one step later; see below.
  if (screenshotPending_ && !config_.record.screenshotIncludeUi) {
    screenshotPending_ = false;
    WriteScreenshot(false);
  }

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  DrawUi();
  ImGui::Render();
  // In HDR the interface goes to a buffer of its own first: it is drawn in sRGB
  // and the screen is being fed linear light, so it needs converting rather than
  // copying. In SDR both calls do nothing and it draws straight to the screen.
  const bool uiLayer = renderer_.BeginUiLayer();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  if (uiLayer) renderer_.CompositeUiLayer();

  // The other grab point. Everything has been drawn and nothing has been
  // presented yet, which is the only moment the back buffer holds the finished
  // window: under the flip model its contents are undefined after the present.
  if (screenshotPending_) {
    screenshotPending_ = false;
    WriteScreenshot(true);
  }

  // Beide Messwerte jedes Bild, unabhaengig davon, ob das Panel offen ist: das
  // Log schreibt seine Statuszeile auch dann, und ein Ruckler waehrend einer
  // geschlossenen Anzeige ist genau der, den man spaeter sucht.
  if (captureState_ == CaptureState::Running) {
    const double nowSeconds = QpcToSeconds(now);
    if (sink) frameAgeMeter_.Sample(sink->stats().lastArrivalAgeMs, nowSeconds);
    const AudioStats audioNow = audio_.stats();
    if (audioNow.running) audioBufferMeter_.Sample(audioNow.bufferMs, nowSeconds);
  }
  SyncMicrophone();
  FeedRecorder();
  UpdateVirtualCamera();
  FeedFrameConsumers();
  d3d_.EndFrame(config_.app.vsync);

  // ---- present rate ----
  ++presentCount_;
  if (fpsWindowQpc_ == 0) fpsWindowQpc_ = now;
  const double elapsed = QpcToSeconds(now - fpsWindowQpc_);
  if (elapsed >= 1.0) {
    presentFps_ = presentCount_ / elapsed;
    presentCount_ = 0;
    fpsWindowQpc_ = now;


    // With logging on, write a line every few seconds. This is what makes a
    // "it stutters" report actionable without having to reproduce it here.
    if (config_.app.logToFile && captureState_ == CaptureState::Running) {
      if (++statsLogCounter_ >= 5) {
        statsLogCounter_ = 0;
        const SinkStats sinkStats = sink ? sink->stats() : SinkStats{};
        const AudioStats audioStats = audio_.stats();
        // Mittel und Spitze ueber die ganzen fuenf Sekunden statt eines
        // Augenblickswerts. Ein Ruckler ist ein einzelnes Bild -- die Chance,
        // ihn mit einer Stichprobe alle fuenf Sekunden zu erwischen, ist etwa
        // eins zu dreihundert, und genau danach wird in diesen Zeilen gesucht.
        double ageHigh = 0.0;
        frameAgeMeter_.TakeRange(nullptr, &ageHigh);
        double bufLow = 0.0, bufHigh = 0.0;
        audioBufferMeter_.TakeRange(&bufLow, &bufHigh);
        CAP_LOG("Status: Quelle %.2f fps, Ausgabe %.1f fps, %llu angezeigt, %llu verworfen, "
                "Bildalter %.1f ms (Spitze %.1f) | Halbbilder %llu/%llu | "
                "Ton %.1f/%.0f ms (%.1f-%.1f), %llu leer, %llu übergelaufen",
                sinkStats.sourceFps, presentFps_, (unsigned long long)sinkStats.displayed,
                (unsigned long long)sinkStats.dropped, frameAgeMeter_.average, ageHigh,
                (unsigned long long)fieldsShown_[0], (unsigned long long)fieldsShown_[1],
                audioBufferMeter_.average, audioStats.targetMs, bufLow, bufHigh,
                (unsigned long long)audioStats.underruns,
                (unsigned long long)audioStats.overruns);
      }
    }
  }
}

void App::DrawUi() {
  const Profile& profile = config_.active();
  FrameSink* sink = capture_.sink();

  // ---- toolbar ----
  // Windowed it is simply there; in fullscreen it follows the pointer, which is
  // already hidden after a couple of seconds of play.
  toolbarVisible_ = config_.app.showToolbar && !cropPick_.active &&
                    (!fullscreen_ || !cursorHidden_);
  if (toolbarVisible_) {
    DrawToolbarStrip();
    // Everything else positions itself against the viewport work area, which is
    // exactly what it is for: the part of the window not taken by a bar. Moving
    // it here means the statistics, the toasts, the status card and the settings
    // dialog all keep clear of the toolbar without knowing it exists. ImGui
    // resets this at the start of every frame.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float reserved = ToolbarHeight();
    vp->WorkPos.y += reserved;
    vp->WorkSize.y -= reserved;
  }

  // ---- status card, or the empty state ----
  if (!HaveLiveSignal()) {
    if (captureState_ == CaptureState::Reconnecting) {
      // This one keeps the card: it interrupts a picture that was there a moment
      // ago and is expected back, and the spinner says so.
      DrawStatusCard(T("Verbindung unterbrochen", "Connection lost"),
                     captureError_.empty() ? std::string() : captureError_, true);
    } else if (captureState_ == CaptureState::Running) {
      const VideoRenderer::SignalVerdict verdict = renderer_.detectedSignal();
      DrawIdleScreen(
          (unsigned long long)idleIcon_.Get(), idleIconSize_,
          verdict == VideoRenderer::SignalVerdict::Snow
              ? T("Kein Signal — die Karte empfängt nur Rauschen. Kabel und Eingang prüfen.",
                  "No signal — the card is receiving noise only. Check the cable and input.")
              : T("Kein Signal. Quelle eingeschaltet? Richtiger Eingang gewählt?",
                  "No signal. Is the source on? Is the right input selected?"));
    } else if (!settings_.isOpen()) {
      // Beim ersten Start eine Begruessung statt einer Fehlermeldung. Es ist
      // derselbe Bildschirm -- Zeichen, Schriftzug, eine Zeile darunter --, nur
      // sagt die Zeile hier, was als Naechstes zu tun ist, statt zu melden, dass
      // etwas fehlt. Beim ersten Mal fehlt naemlich noch nichts.
      DrawIdleScreen(
          (unsigned long long)idleIcon_.Get(), idleIconSize_,
          firstRun_ ? T("Willkommen. F2 öffnet die Einstellungen — dort zuerst die "
                        "Capture-Karte auswählen.",
                        "Welcome. Press F2 for the settings, and pick your capture card "
                        "there first.")
                    : T("Kein Gerät aktiv — Rechtsklick oder F2 öffnet die Einstellungen.",
                        "No device active — right-click or press F2 for the settings."));
    }
  }

  // ---- Normensuche ----
  //
  // Nur solange etwas laeuft: "Eingestellt: PAL B" gehoert in den Dialog, nicht
  // dauerhaft ins Bild. Und nicht, waehrend der Dialog offen ist -- dort steht
  // dieselbe Auskunft schon, ausfuehrlicher.
  if (!settings_.isOpen()) {
    const SettingsWindow::StandardSearch search = StandardSearchDisplay();
    const long shown = capture_.running() ? signalStandard_.load(std::memory_order_relaxed) : 0;
    const int idx = shown != 0 ? VideoStandardIndexOf(shown) : -1;
    const char* name = idx >= 0 ? VideoStandardName(idx) : "?";
    switch (search) {
      case SettingsWindow::StandardSearch::Trying:
        DrawSearchIndicator(Format(T("Videonorm wird gesucht: %s", "Scanning video standard: %s"),
                                   name));
        break;
      case SettingsWindow::StandardSearch::Colour:
        DrawSearchIndicator(Format(T("Farbe wird geprüft: %s", "Checking colour: %s"), name));
        break;
      // Die Pause ist kein Vorgang, sondern deren Abwesenheit -- dafuer laufende
      // Punkte ins Bild zu setzen, waere gelogen. Der Dialog sagt es weiterhin.
      case SettingsWindow::StandardSearch::Paused:
      case SettingsWindow::StandardSearch::Off:
        break;
    }
  }

  // ---- stats ----
  if (config_.app.showStats) {
    OverlayStats stats;
    stats.profileName = profile.name;
    stats.deviceName = capture_.resolvedDevice().name;
    const auto& inputs = capture_.capabilities().crossbarInputs;
    if (profile.capture.crossbarInput >= 0 &&
        profile.capture.crossbarInput < (int)inputs.size()) {
      stats.inputName = inputs[(size_t)profile.capture.crossbarInput].name;
    }
    stats.format = renderer_.sourceFormat();
    if (sink) stats.sink = sink->stats();
    stats.audio = audio_.stats();
    stats.presentFps = presentFps_;
    stats.frameAge = frameAgeMeter_;
    stats.audioBuffer = audioBufferMeter_;
    stats.vsync = config_.app.vsync;
    stats.tearing = d3d_.tearingSupported();
    // Vier Zustaende, und der erste ist derjenige, der sonst wie ein Fehler
    // aussieht: solange gemessen wird, steht das auch da. Die Erkennung braucht
    // rund eine Sekunde bewegtes Bild, und wer in dieser Sekunde hinsieht, soll
    // "wird gemessen" lesen und nicht ein "progressiv", das gleich widerrufen
    // wird.
    if (!renderer_.sourceFormat().valid()) {
      stats.scanLabel = "—";
    } else if (profile.image.deinterlaceAuto && !renderer_.sourceFormat().interlaced &&
               renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Pending) {
      stats.scanLabel = T("wird gemessen", "measuring");
    } else if (!SourceLooksInterlaced(profile)) {
      stats.scanLabel = T("progressiv", "progressive");
    } else if (renderer_.sourceCoSitedFields()) {
      stats.scanLabel = T("deckungsgleich (240p/288p)", "aligned (240p/288p)");
    } else if (profile.image.deinterlace == Deinterlace::Off) {
      stats.scanLabel = T("interlaced, kein Deinterlacer", "interlaced, no deinterlacer");
    } else {
      stats.scanLabel = std::string(T("interlaced, ", "interlaced, ")) +
                        DeinterlaceName((int)profile.image.deinterlace);
    }
    // Der Toast ist weg, sobald man kurz weggesehen hat, und die Einstellungen
    // sind zu. Diese Zeile ist die eine Flaeche, die dauerhaft sichtbar ist --
    // wer sich fragt, warum das Bild weicher geworden ist, findet die Antwort
    // dort, wo er ohnehin nachsieht. Ein Fragezeichen und nicht mehr: der
    // Zustand steht davor, dies ist nur der Zweifel daran.
    if (InterlaceVerdictDoubtful(profile)) stats.scanLabel += T(" (?)", " (?)");
    const RECT& r = renderer_.videoRect();
    stats.displayWidth = (int)(r.right - r.left);
    stats.displayHeight = (int)(r.bottom - r.top);
    stats.filterName = ScaleFilterName((int)profile.image.filter);
    stats.videoDelayMs = (int)delayLine_.delayMs();
    stats.detail = config_.app.statsDetail;
    // Spell out what "automatic" resolved to, since that is the setting people
    // second-guess when a picture looks wrong.
    {
      std::string range = profile.image.range == ColorRange::Auto
                              ? (detectedRangeText_ ? detectedRangeText_
                                                    : T("wird gemessen", "measuring"))
                              : ColorRangeName((int)profile.image.range);
      std::string matrix = ColorMatrixName((int)profile.image.matrix);
      stats.colorInfo = range + "  /  " + matrix;
    }
    DrawStatsPanel(stats);
  }

  // ---- crop picker ----
  if (cropPick_.active) {
    DrawCropPicker();
    if (!cropPick_.active) return;  // Apply or Cancel closed it this frame
  }

  // ---- recording indicator ----
  if (recorder_.recording()) {
    DrawRecordIndicator(recorder_.stats().seconds, config_.app.osdCorner);
  }

  // ---- volume readout ----
  {
    const double age = ImGui::GetTime() - volumeOsdStart_;
    if (age < kVolumeOsdSeconds) {
      DrawVolumeOsd(profile.audio.volume, profile.audio.mute, config_.app.osdCorner, age,
                    kVolumeOsdSeconds);
    }
  }

  // ---- toast ----
  if (!toastText_.empty()) {
    const double age = ImGui::GetTime() - toastStart_;
    if (age > 2.5) {
      toastText_.clear();
    } else {
      DrawToast(toastText_, age, 2.5);
    }
  }

  DrawContextMenu();

  // ---- settings ----
  // The banner explains why the dialog opened by itself. Once the card is
  // actually running the reason is gone, whichever route got it there --
  // picking a device, F5, or the automatic retry. Leaving it up until the
  // dialog is closed and reopened reads like the selection did not take.
  if (captureState_ == CaptureState::Running) settings_.ClearReason();

  switch (renderer_.detectedRange()) {
    case VideoRenderer::RangeVerdict::Limited:
      detectedRangeText_ = T("begrenzt (16-235)", "limited (16-235)");
      break;
    case VideoRenderer::RangeVerdict::Full:
      detectedRangeText_ = T("voll (0-255)", "full (0-255)");
      break;
    default:
      detectedRangeText_ = nullptr;
      break;
  }
  settings_.SetDetectedRange(&detectedRangeText_);

  switch (renderer_.detectedInterlace()) {
    case VideoRenderer::InterlaceVerdict::Interlaced:
      detectedInterlaceText_ = renderer_.sourceCoSitedFields()
                                   ? T("interlaced, 240p/288p-Quelle",
                                       "interlaced, 240p/288p source")
                                   : T("interlaced", "interlaced");
      break;
    case VideoRenderer::InterlaceVerdict::Progressive:
      detectedInterlaceText_ = T("progressiv", "progressive");
      break;
    default:
      detectedInterlaceText_ = nullptr;
      break;
  }
  settings_.SetDetectedInterlace(&detectedInterlaceText_);
  settings_.SetInterlaceDoubtful(InterlaceVerdictDoubtful(profile));

  // Ein 1080p- oder 720p-Bild, das die Karte als progressiv meldet und das hier
  // trotzdem als interlaced gemessen wurde: das ist kaum je richtig, und wer
  // gerade zusieht, merkt sonst nur, dass das Bild ploetzlich weicher wird,
  // ohne den Grund zu finden. Die Meldung nennt deshalb gleich den Reiter, in
  // dem es abzustellen ist.
  {
    const bool doubtful = InterlaceVerdictDoubtful(profile);
    if (doubtful && !interlaceDoubtToasted_) {
      interlaceDoubtToasted_ = true;
      const VideoFormatInfo fmt = renderer_.sourceFormat();
      CAP_LOG("Interlacing bei %dx%d erkannt, obwohl die Karte progressiv meldet -- Hinweis "
              "ausgegeben",
              fmt.width, fmt.height);
      Toast(Format(T("Halbbilder bei %dx%d erkannt — bitte prüfen (Reiter Bild)",
                     "Fields detected at %dx%d — please check (Image tab)"),
                   fmt.width, fmt.height));
    } else if (!doubtful) {
      interlaceDoubtToasted_ = false;
    }
  }
  settings_.SetCoSitedFields(renderer_.sourceCoSitedFields());
  settings_.SetSignalLocked(PollSignalLocked());
  settings_.SetStandardSearch(StandardSearchDisplay());
  settings_.SetLiveStandard(capture_.running() ? signalStandard_.load(std::memory_order_relaxed)
                                               : 0);
  settings_.SetProbeAllowed(captureState_ != CaptureState::Reconnecting);
  settings_.SetUpdater(&updater_);
  // Auto means: analogue when the card has a decoder for it. A card that only
  // does one of the two therefore needs nobody to say which.
  // Dieselbe Quelle der Wahrheit, die auch entscheidet, was ueberhaupt noch
  // gezeichnet wird. Liefen die beiden auseinander, wirkte etwas, das nirgends
  // mehr einstellbar ist.
  settings_.SetAnalogueSource(SourceIsAnalogue());
  // Und dieselbe Wahrheit noch einmal an den Renderer, der daran entscheidet,
  // ob die kachelweise Interlacing-Erkennung mitreden darf.
  renderer_.SetAnalogueSource(SourceIsAnalogue());
  // Und die gemessene Ankunftsrate dazu, mit der die Erkennung ein Kammurteil
  // verwerfen kann, das der Formatraum nicht hergibt -- siehe SetFrameRateHint.
  // Gemessen, nicht angekuendigt: die Karte kuendigt bei einer Halbbildquelle
  // 50 an und liefert 25 gewebte Bilder, und mit der angekuendigten Zahl haette
  // das Veto genau die Quellen erwischt, vor denen es schuetzen soll. Ohne
  // Messung 0, und 0 heisst "noch nicht gemessen", nicht "steht still".
  double measuredFps = 0.0;
  if (const FrameSink* sink = capture_.sink()) {
    const double measured = sink->stats().sourceFps;
    if (measured > 1.0) measuredFps = measured;
  }
  renderer_.SetFrameRateHint(measuredFps);
  settings_.SetSourceFps(renderer_.sourceFormat().fps);
  // Woraus sich entscheidet, welche Abschnitte im Reiter Bild erscheinen: die
  // Zeilenzahl fuer die Bildroehreneffekte, die Halbbilder fuer das
  // Deinterlacing. Beides aus der Quelle, nicht daraus, ob sie analog ist --
  // 1080i gibt es ueber HDMI, und 480p gibt es von einem RetroTINK.
  settings_.SetSourceHeight(renderer_.sourceFormat().valid() ? renderer_.sourceFormat().height : 0);
  settings_.SetSourceInterlaced(
      renderer_.sourceFormat().interlaced ||
      renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced);
  settings_.SetLevels(audio_.inputPeak(), mic_.peak(), mic_.running());
  if (settings_.takeCropPickRequest()) BeginCropPick();
  if (settings_.takeDeviceConfigRequest()) OpenDeviceConfig();
  if (settings_.takeCropDetectRequest()) DetectCrop();
  if (settings_.takeCardResetRequest()) ReinitialiseCard();
  settings_.setProbeBusy(probing_.load(std::memory_order_relaxed));
  // Drawn here only when the settings live inside the picture. The separate
  // window is deliberately not touched from in here: this runs between the main
  // context's NewFrame and Render, and presenting a second swapchain in the
  // middle of another window's frame flushes every bit of GPU work already
  // queued for it -- sixty times a second, while the preview runs at twice that
  // or more. That was not merely CapView stuttering; it was enough to make the
  // desktop's own cursor stutter. It happens after the present instead.
  if (!settingsAreWindowed()) {
    settings_.SetFillsWindow(false);
    if (settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_) ==
        SettingsWindow::Result::Close) {
      settings_.Close();
    }
  }
  if (settings_.takeProbeRequest()) StartEncoderProbe(true);
  DrawUpdatePrompt();
  if (settings_.takeRestartRequest()) {
    if (updater_.RestartIntoNewBuild()) {
      // The new build is coming up; this one gets out of its way so the window
      // position and the configuration are written before it reads them.
      running_ = false;
    } else {
      Toast(T("Neustart fehlgeschlagen.", "Restart failed."));
    }
  }

  // Everything above may have edited the configuration in place, so act on it
  // here in one spot rather than sprinkling apply calls through the UI code.
  SyncConfigChanges();
  MaybeSaveConfig();
}

void App::DrawContextMenu() {
  if (!ImGui::BeginPopupContextVoid("capview_context", ImGuiPopupFlags_MouseButtonRight)) return;

  // Shortcut labels come from the live bindings, so rebinding a key is visible
  // here immediately instead of leaving the menu quietly lying about it. The
  // strings have to outlive the frame, hence the static buffer per action.
  auto sc = [this](HotkeyAction action) -> const char* {
    static std::string text[(int)HotkeyAction::Count];
    const int i = (int)action;
    text[i] = config_.hotkeys[action].bound() ? HotkeyText(config_.hotkeys[action]) : std::string();
    return text[i].empty() ? nullptr : text[i].c_str();
  };

  if (ImGui::MenuItem(T("Einstellungen...", "Settings..."), sc(HotkeyAction::Settings))) OpenSettings({});
  ImGui::Separator();

  bool fs = fullscreen_;
  if (ImGui::MenuItem(T("Vollbild", "Fullscreen"), sc(HotkeyAction::Fullscreen), &fs)) SetFullscreen(fs);

  bool top = config_.app.alwaysOnTop;
  if (ImGui::MenuItem(T("Immer im Vordergrund", "Always on top"), nullptr, &top)) {
    config_.app.alwaysOnTop = top;
    ApplyWindowFlags();
  }

  bool stats = config_.app.showStats;
  if (ImGui::MenuItem(T("Statistik", "Statistics"), sc(HotkeyAction::Stats), &stats)) config_.app.showStats = stats;

  bool toolbar = config_.app.showToolbar;
  if (ImGui::MenuItem(T("Werkzeugleiste", "Toolbar"), nullptr, &toolbar)) {
    config_.app.showToolbar = toolbar;
  }

  // Same reasoning as the colour menu below, only more so: whether the standard
  // is right is something you see instantly, and on a console that switches
  // between 50 and 60 Hz it is the setting you reach for most.
  //
  // Both conditions, exactly as the settings window has them. A card with an
  // analogue decoder still reports its whole standard list while it is showing
  // HDMI, and on that input every entry in it is dead: the decoder is not in the
  // path at all. Offering them anyway is a menu that does nothing, which is
  // worse than one that is not there.
  const long availableStandards =
      capture_.running() ? capture_.capabilities().availableStandards : 0;
  if (availableStandards != 0 && SourceIsAnalogue() &&
      ImGui::BeginMenu(T("Videonorm", "Video standard"))) {
    CaptureSettings& cap = config_.active().capture;
    const long before = cap.videoStandard;

    if (ImGui::MenuItem(T("Automatisch", "Automatic"), nullptr, cap.videoStandard == -1)) {
      cap.videoStandard = -1;
    }
    if (ImGui::MenuItem(T("Nicht ändern", "Leave alone"), nullptr, cap.videoStandard == 0)) {
      cap.videoStandard = 0;
    }
    ImGui::Separator();
    const int chosen = VideoStandardGroupOf(cap.videoStandard);
    for (int i = 0; i < VideoStandardGroupCount(); ++i) {
      const long value = VideoStandardGroupPick(i, availableStandards);
      if (value == 0) continue;
      if (ImGui::MenuItem(VideoStandardGroupName(i), nullptr, chosen == i)) {
        cap.videoStandard = value;
      }
      WrappedTooltip(VideoStandardGroupHint(i));
    }

    if (cap.videoStandard != before) {
      // A different standard usually means a different number of lines, so the
      // graph has to come up again around the new format. Automatic is the one
      // case that does not restart here: it has nothing to apply yet and will
      // rebuild by itself once it has found something that locks.
      if (cap.videoStandard > 0) {
        std::string error;
        if (StartCapture(&error)) {
          Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                       VideoStandardPickerName(cap.videoStandard).c_str()));
        } else {
          Toast(error);
        }
      }
      SaveConfig();
    }
    ImGui::EndMenu();
  }

  // Right here rather than buried in the dialog: wrong levels or a wrong matrix
  // are things you spot by looking at the picture, and both take effect on the
  // very next frame, so switching them while watching is the fastest way to
  // land on the right one.
  if (ImGui::BeginMenu(T("Farbe", "Colour"))) {
    ImageSettings& img = config_.active().image;

    ImGui::SeparatorText(T("Wertebereich", "Range"));
    for (int i = 0; i < 3; ++i) {
      // "##range" keeps the id unique: the first entry of both lists is called
      // "Automatic", and two menu items with the same label in one menu share an
      // id, which Dear ImGui reports as a programmer error.
      const std::string label = std::string(ColorRangeName(i)) + "##range";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.range == i)) {
        img.range = (ColorRange)i;
        Toast(std::string(T("Wertebereich: ", "Range: ")) + ColorRangeName(i));
      }
    }

    ImGui::SeparatorText(T("Farbmatrix", "Colour matrix"));
    for (int i = 0; i < 3; ++i) {
      const std::string label = std::string(ColorMatrixName(i)) + "##matrix";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.matrix == i)) {
        img.matrix = (ColorMatrix)i;
        Toast(std::string(T("Farbmatrix: ", "Colour matrix: ")) + ColorMatrixName(i));
      }
    }
    ImGui::EndMenu();
  }

  // Volume lives in the menu as well as on the wheel: the menu is where you
  // look when you cannot remember the shortcut.
  AudioSettings& audio = config_.active().audio;
  bool muted = audio.mute;
  if (ImGui::MenuItem(T("Stumm", "Muted"), sc(HotkeyAction::Mute), &muted)) ToggleMute();

  ImGui::SetNextItemWidth(180.0f);
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
    audio.mute = false;
    audio_.ApplySettings(audio);
    applied_.volume = audio.volume;
    applied_.mute = audio.mute;
  }

  if (config_.profiles.size() > 1 && ImGui::BeginMenu(T("Profil", "Profile"))) {
    for (int i = 0; i < (int)config_.profiles.size(); ++i) {
      const bool selected = (i == config_.activeProfile);
      std::string shortcut = i < 9 ? Format(T("Strg+%d", "Ctrl+%d"), i + 1) : std::string();
      if (ImGui::MenuItem(config_.profiles[(size_t)i].name.c_str(),
                          shortcut.empty() ? nullptr : shortcut.c_str(), selected)) {
        SwitchProfile(i);
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  {
    const bool rec = recorder_.recording();
    if (ImGui::MenuItem(rec ? T("Aufnahme stoppen", "Stop recording")
                            : T("Aufnahme starten", "Start recording"),
                        sc(HotkeyAction::Record))) {
      ToggleRecording();
    }
  }
  if (ImGui::MenuItem(T("Screenshot", "Screenshot"), sc(HotkeyAction::Screenshot))) RequestScreenshot();

  // Der schwarze Rand ist etwas, das man sieht, und das Suchen danach gehoert
  // deshalb dorthin, wo man hinsieht, statt in einen Reiter des
  // Einstellungsfensters. Zumal die Messung nur so gut ist wie das Bild, das
  // gerade anliegt: sie sucht die Grenzen dessen, was nicht schwarz ist, und
  // auf einem Ladebildschirm sind das die Grenzen des Ladebildschirms. Wer den
  // Knopf im Vorbeigehen erreicht, drueckt ihn im richtigen Moment noch einmal.
  if (ImGui::MenuItem(T("Rand suchen", "Detect border"), sc(HotkeyAction::DetectCrop))) {
    DetectCrop();
  }
  WrappedTooltip(T("Schneidet den schwarzen Rand weg, den die Karte mitliefert. Braucht ein "
                   "richtiges Bild — auf Schwarz gemessen kommt Unsinn heraus.",
                   "Crops the black border the card delivers. Needs a real picture — measured "
                   "on black it produces nonsense."));

  if (ImGui::MenuItem(T("Aufnahme neu starten", "Restart capture"), sc(HotkeyAction::RestartCapture))) RestartAll(true);
  if (ImGui::MenuItem(T("Karte neu einlesen", "Reinitialise card"), sc(HotkeyAction::ReinitCard))) ReinitialiseCard();
  if (ImGui::MenuItem(T("Beenden", "Quit"), "Alt+F4")) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);

  ImGui::EndPopup();
}

// ------------------------------------------------------------------ messages

bool App::HandleKeyDown(WPARAM key) {
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

  // Fixed on purpose, see hotkeys.h: a rebindable Escape is a way to lock
  // yourself into fullscreen, and the profile digits are a block, not a key.
  if (ctrl && key >= '1' && key <= '9') {
    SwitchProfile((int)(key - '1'));
    return true;
  }
  if (cropPick_.active) {
    if (key == VK_ESCAPE) {
      EndCropPick(false);
      return true;
    }
    if (key == VK_RETURN) {
      EndCropPick(true);
      return true;
    }
    return true;  // swallow everything else: one job at a time
  }

  if (key == VK_ESCAPE) {
    if (settings_.isOpen()) {
      settings_.Close();
    } else if (fullscreen_) {
      SetFullscreen(false);
    }
    return true;
  }

  switch (config_.hotkeys.Find((int)key, ctrl, shift, alt)) {
    case HotkeyAction::Fullscreen:
      ToggleFullscreen();
      return true;
    case HotkeyAction::Settings:
      if (settings_.isOpen()) {
        settings_.Close();
      } else {
        OpenSettings({});
      }
      return true;
    case HotkeyAction::Stats:
      config_.app.showStats = !config_.app.showStats;
      return true;
    case HotkeyAction::RestartCapture:
      RestartAll(true);
      return true;
    case HotkeyAction::ReinitCard:
      ReinitialiseCard();
      return true;
    case HotkeyAction::Record:
      ToggleRecording();
      return true;
    case HotkeyAction::Screenshot:
      RequestScreenshot();
      return true;
    case HotkeyAction::DetectCrop:
      DetectCrop();
      return true;
    case HotkeyAction::Mute:
      ToggleMute();
      return true;
    case HotkeyAction::VolumeUp:
      AdjustVolume(kVolumeStep);
      return true;
    case HotkeyAction::VolumeDown:
      AdjustVolume(-kVolumeStep);
      return true;
    default:
      break;
  }

  // The numeric keypad follows the main volume keys without needing its own
  // binding -- nobody expects to have to bind both.
  if (key == VK_ADD && config_.hotkeys[HotkeyAction::VolumeUp].bound()) {
    AdjustVolume(kVolumeStep);
    return true;
  }
  if (key == VK_SUBTRACT && config_.hotkeys[HotkeyAction::VolumeDown].bound()) {
    AdjustVolume(-kVolumeStep);
    return true;
  }
  return false;
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;

  switch (msg) {
    case WM_ENTERSIZEMOVE:
      // Same as for the settings window: Windows takes the loop away while a
      // window is being dragged, and only a timer still gets through.
      ::SetTimer(hwnd, 1, 8, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      ::KillTimer(hwnd, 1);
      return 0;
    case WM_TIMER:
      if (wparam == 1 && !inModalFrame_) {
        inModalFrame_ = true;
        Tick();
        RenderFrame();
        inModalFrame_ = false;
      }
      return 0;
    case WM_SIZE:
      minimized_ = (wparam == SIZE_MINIMIZED);
      if (!minimized_) d3d_.Resize();
      return 0;

    case WM_GETMINMAXINFO: {
      auto* info = (MINMAXINFO*)lparam;
      info->ptMinTrackSize.x = 320;
      info->ptMinTrackSize.y = 240;
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (p.x != lastMousePos_.x || p.y != lastMousePos_.y) {
        lastMousePos_ = p;
        lastMouseMoveQpc_ = QpcNow();
        if (cursorHidden_) {
          ::ShowCursor(TRUE);
          cursorHidden_ = false;
        }
      }
      return 0;
    }

    case WM_SETCURSOR:
      if (cursorHidden_ && LOWORD(lparam) == HTCLIENT) {
        ::SetCursor(nullptr);
        return 1;
      }
      break;

    case WM_MOUSEWHEEL:
      // Only over the picture: inside the settings window the wheel scrolls.
      if (config_.app.wheelVolume && imguiReady_ && !ImGui::GetIO().WantCaptureMouse) {
        const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (notches != 0) {
          AdjustVolume(kVolumeStep * (float)notches);
          return 0;
        }
      }
      break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // The binding editor wants the raw key, before anyone acts on it -- that
      // is the whole point of it being open.
      if (settings_.waitingForKey()) {
        settings_.OfferKey((int)wparam, (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                           (::GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (::GetKeyState(VK_MENU) & 0x8000) != 0);
        return 0;
      }
      // While the settings window has the keyboard, let it type.
      if (!(imguiReady_ && ImGui::GetIO().WantCaptureKeyboard)) {
        lastKeyLParam_ = (uint64_t)lparam;
        if (HandleKeyDown(wparam)) return 0;
      } else if (wparam == VK_ESCAPE) {
        settings_.Close();
        return 0;
      }
      break;

    case WM_SYSCOMMAND:
      // Block the screensaver from starting over our window.
      if ((wparam & 0xFFF0) == SC_SCREENSAVE || (wparam & 0xFFF0) == SC_MONITORPOWER) {
        if (config_.app.preventSleep) return 0;
      }
      break;

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
      settings_.InvalidateDeviceLists();
      break;

    case WM_SETTINGCHANGE:
      if (config_.app.theme == Theme::System) ApplyTheme();
      break;

    case WM_DPICHANGED: {
      auto* rc = (RECT*)lparam;
      ::SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }

    case WM_CLOSE:
      SaveConfig();
      running_ = false;
      ::PostQuitMessage(0);
      return 0;

    case WM_DESTROY:
      running_ = false;
      ::PostQuitMessage(0);
      return 0;

    default: break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace cap
