#pragma once

// Persistent settings model. Everything the user can change lives here; the
// UI edits a Config, the capture/render/audio subsystems read one.
//
// Layout: a Config holds app-wide settings (window, theme, vsync) plus a list
// of profiles. A profile bundles everything that changes when you swap the
// source -- device, crossbar input, capture format, image and audio settings --
// so "SNES 4:3 nearest" and "PS2 Component 480i" are one hotkey apart.

#include <string>
#include <vector>

#include "hotkeys.h"
#include "i18n.h"

namespace cap {

enum class Theme { Dark, Light, System };

enum class ScaleFilter { Nearest, Bilinear, Bicubic, Lanczos3, SharpBilinear };

enum class Deinterlace { Off, Bob, BobLinear };

enum class AspectMode { Source, Force16x9, Force4x3, Stretch, Integer };

enum class ColorRange { Auto, Limited, Full };

enum class ColorMatrix { Auto, BT601, BT709 };

enum class AudioSource { Embedded, Manual, None };

enum class OsdCorner { TopLeft, TopRight, BottomLeft, BottomRight };

// How much of the statistics panel is shown. The full panel is a wall of text
// to read while playing, so the useful numbers come first and the rest is opt in.
enum class StatsDetail { Compact, Normal, Full };

enum class RecordContainer { Mkv, Mp4 };

// PNG is lossless and the sane default for a capture card: the point of a still
// is usually to look at it closely. JPEG is there for whoever takes hundreds.
enum class ScreenshotFormat { Png, Jpeg };

// Which encoder ffmpeg is asked for. Auto picks the best one that survived the
// test encode, preferring hardware. The AV1 entries need recent hardware
// (NVIDIA RTX 40, Intel Arc, AMD RDNA 3); on anything older the probe simply
// marks them unavailable.
enum class RecordEncoder {
  Auto, X264, Nvenc, QuickSync, Amf, X265, NvencHevc, Av1Nvenc, Av1QuickSync, Av1Amf
};

// Only the software encoder gets a speed knob -- for the hardware encoders the
// vendor defaults are better than anything guessed here.
enum class RecordSpeed { UltraFast, VeryFast, Faster, Fast, Medium };

// UI labels in the language currently selected, taken by enum value. Passing an
// out of range index is safe and yields the first entry.
const char* ThemeName(int index);
const char* LanguageName(int index);
const char* ScaleFilterName(int index);
const char* DeinterlaceName(int index);
const char* AspectName(int index);
const char* ColorRangeName(int index);
const char* ColorMatrixName(int index);
const char* OsdCornerName(int index);
const char* StatsDetailName(int index);
const char* RecordContainerName(int index);
const char* ScreenshotFormatName(int index);
const char* RecordEncoderName(int index);
const char* RecordSpeedName(int index);

// Short explanations shown as tooltips. Empty string when there is nothing
// worth saying beyond the label.
const char* ScaleFilterHelp(int index);
const char* DeinterlaceHelp(int index);
const char* AspectHelp(int index);

// Accent colour presets offered in the settings, as 0xRRGGBB.
struct AccentPreset {
  unsigned rgb;
  const char* (*name)();
};
int AccentPresetCount();
unsigned AccentPresetColor(int index);
const char* AccentPresetName(int index);

// Identifies a device across restarts. `id` is the stable one (DirectShow
// DevicePath / WASAPI endpoint id); `name` is what the user sees and is used
// as a fallback when the id no longer resolves (e.g. card moved slots).
struct DeviceRef {
  std::string name;
  std::string id;
  // Audio inputs only: "wasapi" or "dshow". Not every capture card exposes its
  // embedded audio as a Windows sound device -- plenty of them, the StarTech
  // PEXHDCAP60L among them, offer it solely as a DirectShow audio input.
  std::string backend;

  bool empty() const { return name.empty() && id.empty(); }
  bool isDirectShow() const { return backend == "dshow"; }
  bool operator==(const DeviceRef& o) const {
    return name == o.name && id == o.id && backend == o.backend;
  }
};

// One concrete capture format. `subtype` is a short label such as "YUY2",
// "UYVY", "NV12", "RGB24" or "MJPG".
struct FormatSel {
  std::string subtype;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  // True when this combination is not advertised by the driver but lies inside
  // the ranges it reports -- the 1080p60 case.
  bool forced = false;

  bool valid() const { return width > 0 && height > 0 && !subtype.empty(); }
  bool SameFormat(const FormatSel& o) const {
    return subtype == o.subtype && width == o.width && height == o.height;
  }
  // "1920x1080 @ 60,00 Hz  YUY2"
  std::string Label() const;
};

struct CaptureSettings {
  DeviceRef video;
  AudioSource audioSource = AudioSource::Embedded;
  DeviceRef audio;         // only used when audioSource == Manual
  int crossbarInput = -1;  // index into the enumerated crossbar inputs, -1 = leave alone
  FormatSel format;
};

struct ImageSettings {
  ScaleFilter filter = ScaleFilter::Bilinear;
  float sharpen = 0.0f;  // 0..1, contrast-adaptive sharpening applied after scaling
  Deinterlace deinterlace = Deinterlace::Bob;
  bool deinterlaceAuto = true;  // only kick in when the media type says interlaced
  AspectMode aspect = AspectMode::Source;
  int cropLeft = 0;
  int cropRight = 0;
  int cropTop = 0;
  int cropBottom = 0;
  ColorRange range = ColorRange::Auto;
  ColorMatrix matrix = ColorMatrix::Auto;
};

struct AudioSettings {
  DeviceRef output;        // empty = system default output
  int bufferMs = 30;       // target buffer between capture and playback
  bool exclusive = false;  // WASAPI exclusive mode on the output endpoint
  int avOffsetMs = 0;      // >0 delays audio, <0 delays video
  float volume = 1.0f;     // 0..1 linear
  bool mute = false;

  // A second input that only goes into the recording, as its own track. Never
  // played back -- monitoring your own microphone through the same speakers the
  // game comes out of is a feedback loop, not a feature.
  bool micEnabled = false;
  DeviceRef micDevice;   // empty = system default recording device
  float micGain = 1.0f;  // linear, 0..4 (up to +12 dB), independent of Windows
};

// Recording is deliberately thin: it writes what the viewer shows, at the
// source resolution, and the only knobs are the ones that change the result in
// a way a preset cannot guess.
struct RecordSettings {
  std::string outputFolder;  // empty = Videos\CapView
  RecordContainer container = RecordContainer::Mkv;
  RecordEncoder encoder = RecordEncoder::Auto;
  RecordSpeed speed = RecordSpeed::VeryFast;  // software encoder only
  int bitrateKbps = 20000;
  // 0 = whatever the source delivers. Otherwise frames are dropped to hit this.
  double fps = 0.0;
  // Off by default: only FAT32 needs it, NTFS does not.
  bool splitFiles = false;
  int splitSizeMb = 4000;
  // Path to ffmpeg.exe. Empty means: look next to CapView, then on PATH.
  std::string ffmpegPath;

  // Stills. Empty folder = Pictures\CapView. Screenshots go somewhere separate
  // from the recordings on purpose: a folder holding both a handful of videos
  // and four hundred stills is a folder nobody can find anything in.
  std::string screenshotFolder;
  ScreenshotFormat screenshotFormat = ScreenshotFormat::Png;
  int jpegQuality = 92;
};

struct Profile {
  std::string name = "Standard";
  CaptureSettings capture;
  ImageSettings image;
  AudioSettings audio;
};

struct AppSettings {
  Theme theme = Theme::Dark;
  Language language = Language::English;
  // Drives the whole palette: buttons, sliders, borders and the shade of the
  // window background are all derived from this one colour. 0xRRGGBB.
  unsigned accentColor = 0x8B5CF6;  // violet
  bool vsync = false;  // off means "allow tearing" -- the biggest latency lever
  bool alwaysOnTop = false;
  bool hideCursorFullscreen = true;
  bool preventSleep = true;
  bool showStats = false;
  StatsDetail statsDetail = StatsDetail::Compact;
  bool logToFile = false;

  // Where the volume readout appears when it changes.
  OsdCorner osdCorner = OsdCorner::TopRight;
  bool showVolumeOsd = true;
  // Mouse wheel over the picture changes the volume.
  bool wheelVolume = true;

  int windowX = -1;  // -1 = let Windows place the window
  int windowY = -1;
  int windowW = 1280;
  int windowH = 720;
  bool maximized = false;
  bool startFullscreen = false;
  // -1 = whichever monitor the window is currently on.
  int fullscreenMonitor = -1;
};

struct Config {
  AppSettings app;
  RecordSettings record;
  Hotkeys hotkeys;
  std::vector<Profile> profiles;
  int activeProfile = 0;

  Config();

  Profile& active();
  const Profile& active() const;
  void SetActiveProfile(int index);

  // CapView.json next to the executable -- portable, no registry.
  static std::wstring FilePath();

  // Returns false when the file is missing or unreadable; defaults are kept in
  // that case and `error` describes the problem (empty if simply absent).
  bool Load(std::string* error = nullptr);
  bool Save(std::string* error = nullptr) const;

  // The exact text Save would write. Comparing two of these is how the app
  // notices that something changed without having to hand-write an equality
  // operator that would go stale the moment a field is added.
  std::string Serialize() const;
};

}  // namespace cap
