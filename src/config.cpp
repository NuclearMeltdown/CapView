#include "config.h"

#include <cmath>

#include "common.h"
#include "json.h"

namespace cap {

namespace {

// Clamps an index into range so a stale config value cannot read past an array.
int Pick(int index, int count) {
  return (index < 0 || index >= count) ? 0 : index;
}

}  // namespace

// ------------------------------------------------------------------ UI labels

const char* ThemeName(int i) {
  static const char* de[3] = {"Dunkel", "Hell", "Wie Windows"};
  static const char* en[3] = {"Dark", "Light", "Follow Windows"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* LanguageName(int i) {
  static const char* names[2] = {"Deutsch", "English"};
  return names[Pick(i, 2)];
}

const char* ScaleFilterName(int i) {
  static const char* names[5] = {"Nearest Neighbor", "Bilinear", "Bicubic", "Lanczos3",
                                 "Sharp Bilinear"};
  return names[Pick(i, 5)];
}

const char* DeinterlaceName(int i) {
  static const char* de[kDeinterlaceCount] = {"Aus (Weave)",      "Bob",
                                              "Bob (interpoliert)", "Bewegungsadaptiv",
                                              "Kantenorientiert",   "YADIF"};
  static const char* en[kDeinterlaceCount] = {"Off (weave)",       "Bob",
                                              "Bob (interpolated)", "Motion adaptive",
                                              "Edge directed",      "YADIF"};
  i = Pick(i, kDeinterlaceCount);
  return T(de[i], en[i]);
}

const char* FieldOrderName(int i) {
  static const char* de[3] = {"Automatisch", "Oberes Halbbild zuerst", "Unteres Halbbild zuerst"};
  static const char* en[3] = {"Automatic", "Top field first", "Bottom field first"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* RotationName(int i) {
  static const char* de[kRotationCount] = {"Keine", "90 Grad rechts", "180 Grad",
                                           "90 Grad links"};
  static const char* en[kRotationCount] = {"None", "90 degrees right", "180 degrees",
                                           "90 degrees left"};
  i = Pick(i, kRotationCount);
  return T(de[i], en[i]);
}

const char* SignalKindName(int i) {
  static const char* de[kSignalKindCount] = {"Automatisch", "Analog", "Digital"};
  static const char* en[kSignalKindCount] = {"Automatic", "Analogue", "Digital"};
  i = Pick(i, kSignalKindCount);
  return T(de[i], en[i]);
}

const char* AspectName(int i) {
  static const char* de[5] = {"Quelle", "16:9 erzwingen", "4:3 erzwingen", "Strecken",
                              "Integer-Skalierung"};
  static const char* en[5] = {"Source", "Force 16:9", "Force 4:3", "Stretch", "Integer scaling"};
  i = Pick(i, 5);
  return T(de[i], en[i]);
}

const char* ColorRangeName(int i) {
  static const char* de[3] = {"Automatisch", "Limited (16-235)", "Full (0-255)"};
  static const char* en[3] = {"Automatic", "Limited (16-235)", "Full (0-255)"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* ColorMatrixName(int i) {
  static const char* de[3] = {"Automatisch", "BT.601 (SD)", "BT.709 (HD)"};
  static const char* en[3] = {"Automatic", "BT.601 (SD)", "BT.709 (HD)"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* StatsDetailName(int i) {
  static const char* de[3] = {"Kompakt", "Normal", "Vollständig"};
  static const char* en[3] = {"Compact", "Normal", "Full"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* MicTrackModeName(int i) {
  static const char* de[3] = {"Gemischt und getrennt", "Nur gemischt", "Nur getrennt"};
  static const char* en[3] = {"Mixed and separate", "Mixed only", "Separate only"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* RecordContainerName(int i) {
  static const char* names[2] = {"MKV (Matroska)", "MP4"};
  return names[Pick(i, 2)];
}

const char* ScreenshotFormatName(int i) {
  static const char* names[2] = {"PNG", "JPEG"};
  return names[Pick(i, 2)];
}

const char* RecordEncoderName(int i) {
  // Only the first entry differs between languages; the rest are product names.
  static const char* names[kRecordEncoderCount] = {
      "",
      "H.264 (CPU, x264)",
      "H.264 (NVIDIA NVENC)",
      "H.264 (Intel QuickSync / Arc)",
      "H.264 (AMD AMF)",
      "H.265 (CPU, x265)",
      "H.265 (NVIDIA NVENC)",
      "AV1 (NVIDIA NVENC, RTX 40+)",
      "AV1 (Intel Arc)",
      "AV1 (AMD RDNA 3+)",
      "",
  };
  i = Pick(i, kRecordEncoderCount);
  if (i == (int)RecordEncoder::Auto) {
    return T("Automatisch — überall abspielbar", "Automatic — plays anywhere");
  }
  if (i == (int)RecordEncoder::AutoEfficient) {
    return T("Automatisch — kleinere Dateien", "Automatic — smaller files");
  }
  return names[i];
}

const char* RecordSpeedName(int i) {
  // x264 preset names, left in English because that is what they are called.
  static const char* names[5] = {"ultrafast", "veryfast", "faster", "fast", "medium"};
  return names[Pick(i, 5)];
}

const char* OsdCornerName(int i) {
  static const char* de[4] = {"Oben links", "Oben rechts", "Unten links", "Unten rechts"};
  static const char* en[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
  i = Pick(i, 4);
  return T(de[i], en[i]);
}

// Kept to a line or two on purpose: the label already says what it is, the
// tooltip only has to say when you would want it.
const char* ScaleFilterHelp(int i) {
  static const char* de[5] = {
      "Harte Pixelkanten. Retro-Konsolen, zusammen mit Integer-Skalierung.",
      "Weich und neutral. Standard für moderne Quellen.",
      "Catmull-Rom. Schärfer als bilinear, leichte Überschwinger an Kanten.",
      "Schärfste Wahl. Gut für HD, erzeugt bei Pixelgrafik Halos.",
      "Wie Nearest, aber ohne ungleich breite Pixel bei krummen Faktoren.",
  };
  static const char* en[5] = {
      "Hard pixel edges. For retro consoles, paired with integer scaling.",
      "Soft and neutral. The default for modern sources.",
      "Catmull-Rom. Sharper than bilinear, slight overshoot at edges.",
      "Sharpest option. Good for HD, produces halos on pixel art.",
      "Like nearest, but without unevenly wide pixels at odd scale factors.",
  };
  i = Pick(i, 5);
  return T(de[i], en[i]);
}

const char* DeinterlaceHelp(int i) {
  static const char* de[kDeinterlaceCount] = {
      "Halbbilder bleiben verwoben. Bei Bewegung Kammartefakte.",
      "Volle Bildrate, keine Latenz. Zeigt je ein Halbbild doppelt, wodurch die Zeilenpaare im Takt der Halbbilder um eine Zeile wandern -- messbar 1,0 Zeilen. Für 240p/288p-Quellen ideal, für echtes Interlacing die unruhigste Wahl.",
      "Wie Bob, fehlende Zeilen interpoliert. Steht deutlich ruhiger als Bob, weil die Zeilen an ihrer wahren Position landen.",
      "Ruhige Bildteile behalten die volle Zeilenzahl, bewegte werden interpoliert. "
      "Keine Latenz.",
      "Für Pixelgrafik gedacht: interpoliert entlang der Kantenrichtung, glatte "
      "Diagonalen, keine Latenz. Auf Composite-Signalen schlechter als die anderen.",
      "Beste Qualität. Vergleicht mit dem vorherigen Bild, braucht ein Bild Speicher "
      "und etwas mehr GPU-Last.",
  };
  static const char* en[kDeinterlaceCount] = {
      "Fields stay woven. Combing artefacts on motion.",
      "Full frame rate, no added latency. Shows one field twice over, so the line pairs move by one line at field rate -- measured at 1.0 lines. Ideal for 240p/288p sources, the least steady choice for genuine interlacing.",
      "Like bob with interpolated lines. Considerably steadier than bob, because each line lands where it belongs.",
      "Still parts of the picture keep every line, moving parts are interpolated. "
      "No added latency.",
      "Meant for pixel art: interpolates along the direction of edges, smooth diagonals, "
      "no added latency. Worse than the others on a composite signal.",
      "Best quality. Compares against the previous frame, so it keeps one frame in "
      "memory and costs a little more GPU time.",
  };
  i = Pick(i, kDeinterlaceCount);
  return T(de[i], en[i]);
}

const char* MaskName(int i) {
  static const char* de[3] = {"Aus", "Streifenmaske", "Lochmaske"};
  static const char* en[3] = {"Off", "Aperture grille", "Shadow mask"};
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* MaskHelp(int i) {
  static const char* de[3] = {
      "Keine Maske.",
      "Senkrechte Streifen, wie bei einer Trinitron-Röhre.",
      "Versetzte Tripel, wie bei einer gewöhnlichen Lochmaskenröhre.",
  };
  static const char* en[3] = {
      "No mask.",
      "Vertical stripes, the way a Trinitron tube worked.",
      "Staggered triads, the way an ordinary shadow mask tube worked.",
  };
  i = Pick(i, 3);
  return T(de[i], en[i]);
}

const char* AspectHelp(int i) {
  static const char* de[5] = {
      "So wie die Karte es liefert.",
      "Unabhängig von der Auflösung der Quelle.",
      "Für alte Konsolen, die 4:3 in einem 16:9-Signal liefern.",
      "Füllt das Fenster, verzerrt das Bild.",
      "Nur ganzzahlige Faktoren. Mit Nearest sind alle Pixel gleich groß.",
  };
  static const char* en[5] = {
      "Whatever the card reports.",
      "Regardless of the source resolution.",
      "For old consoles that put 4:3 inside a 16:9 signal.",
      "Fills the window, distorts the picture.",
      "Whole-number factors only. With nearest, every pixel is the same size.",
  };
  i = Pick(i, 5);
  return T(de[i], en[i]);
}

// ------------------------------------------------------------ accent presets

namespace {

struct AccentEntry {
  unsigned rgb;
  const char* de;
  const char* en;
};

const AccentEntry kAccents[] = {
    {0x8B5CF6, "Violett", "Violet"},   {0xA855F7, "Purpur", "Purple"},
    {0xEC4899, "Pink", "Pink"},        {0xEF4444, "Rot", "Red"},
    {0xF97316, "Orange", "Orange"},    {0xF59E0B, "Bernstein", "Amber"},
    {0x22C55E, "Grün", "Green"},       {0x14B8A6, "Türkis", "Teal"},
    {0x06B6D4, "Cyan", "Cyan"},        {0x4C8DFF, "Blau", "Blue"},
    {0x6366F1, "Indigo", "Indigo"},    {0x94A3B8, "Grau", "Grey"},
};

}  // namespace

int AccentPresetCount() {
  return (int)(sizeof(kAccents) / sizeof(kAccents[0]));
}

unsigned AccentPresetColor(int index) {
  return kAccents[Pick(index, AccentPresetCount())].rgb;
}

const char* AccentPresetName(int index) {
  const AccentEntry& e = kAccents[Pick(index, AccentPresetCount())];
  return T(e.de, e.en);
}

namespace {

// --------------------------------------------------------------- enum helpers

template <typename E>
E EnumFromInt(int v, int count, E fallback) {
  if (v < 0 || v >= count) return fallback;
  return (E)v;
}

template <typename E>
E ReadEnum(const json::Value& v, const char* key, int count, E fallback) {
  if (!v.Has(key)) return fallback;
  return EnumFromInt<E>(v[key].AsInt((int)fallback), count, fallback);
}

// ---------------------------------------------------------- struct <-> json

json::Value WriteDevice(const DeviceRef& d) {
  json::Value o = json::Value::Object();
  o["name"] = d.name;
  o["id"] = d.id;
  if (!d.backend.empty()) o["backend"] = d.backend;
  return o;
}

DeviceRef ReadDevice(const json::Value& v) {
  DeviceRef d;
  d.name = v["name"].AsString();
  d.id = v["id"].AsString();
  d.backend = v["backend"].AsString();
  return d;
}

json::Value WriteFormat(const FormatSel& f) {
  json::Value o = json::Value::Object();
  o["subtype"] = f.subtype;
  o["width"] = f.width;
  o["height"] = f.height;
  o["fps"] = f.fps;
  o["forced"] = f.forced;
  return o;
}

FormatSel ReadFormat(const json::Value& v) {
  FormatSel f;
  f.subtype = v["subtype"].AsString();
  f.width = v["width"].AsInt(0);
  f.height = v["height"].AsInt(0);
  f.fps = v["fps"].AsNumber(0.0);
  f.forced = v["forced"].AsBool(false);
  return f;
}

json::Value WriteProfile(const Profile& p) {
  json::Value o = json::Value::Object();
  o["name"] = p.name;

  json::Value cap = json::Value::Object();
  cap["video"] = WriteDevice(p.capture.video);
  cap["audioSource"] = (int)p.capture.audioSource;
  cap["audio"] = WriteDevice(p.capture.audio);
  cap["crossbarInput"] = p.capture.crossbarInput;
  cap["videoStandard"] = (int)p.capture.videoStandard;
  cap["signalKind"] = (int)p.capture.signalKind;
  cap["format"] = WriteFormat(p.capture.format);
  o["capture"] = cap;

  json::Value img = json::Value::Object();
  img["filter"] = (int)p.image.filter;
  img["sharpen"] = p.image.sharpen;
  img["nativeWidth"] = p.image.nativeWidth;
  img["scanlines"] = p.image.scanlines;
  img["mask"] = p.image.mask;
  img["maskStrength"] = p.image.maskStrength;
  img["deinterlace"] = (int)p.image.deinterlace;
  img["deinterlaceAuto"] = p.image.deinterlaceAuto;
  img["fieldOrder"] = (int)p.image.fieldOrder;
  img["lineDouble"] = p.image.lineDouble;
  img["rotation"] = (int)p.image.rotation;
  img["chromaSoft"] = p.image.chromaSoft;
  img["temporalDenoise"] = p.image.temporalDenoise;
  img["dotNotch"] = p.image.dotNotch;
  img["aspect"] = (int)p.image.aspect;
  img["cropLeft"] = p.image.cropLeft;
  img["cropRight"] = p.image.cropRight;
  img["cropTop"] = p.image.cropTop;
  img["cropBottom"] = p.image.cropBottom;
  img["range"] = (int)p.image.range;
  img["matrix"] = (int)p.image.matrix;
  o["image"] = img;

  json::Value aud = json::Value::Object();
  aud["output"] = WriteDevice(p.audio.output);
  aud["bufferMs"] = p.audio.bufferMs;
  aud["exclusive"] = p.audio.exclusive;
  aud["avOffsetMs"] = p.audio.avOffsetMs;
  aud["volume"] = p.audio.volume;
  aud["mute"] = p.audio.mute;
  aud["micEnabled"] = p.audio.micEnabled;
  aud["micDevice"] = WriteDevice(p.audio.micDevice);
  aud["micGain"] = p.audio.micGain;
  aud["micTrackMode"] = (int)p.audio.micTrackMode;
  o["audio"] = aud;

  return o;
}

Profile ReadProfile(const json::Value& v) {
  Profile p;
  p.name = v["name"].AsString("Standard");

  const json::Value& c = v["capture"];
  p.capture.video = ReadDevice(c["video"]);
  p.capture.audioSource = ReadEnum<AudioSource>(c, "audioSource", 3, AudioSource::Embedded);
  p.capture.audio = ReadDevice(c["audio"]);
  p.capture.crossbarInput = c["crossbarInput"].AsInt(-1);
  p.capture.videoStandard = (long)c["videoStandard"].AsInt(0);
  p.capture.signalKind = ReadEnum<SignalKind>(c, "signalKind", kSignalKindCount, SignalKind::Auto);
  p.capture.format = ReadFormat(c["format"]);

  const json::Value& i = v["image"];
  p.image.filter = ReadEnum<ScaleFilter>(i, "filter", 5, ScaleFilter::Bilinear);
  p.image.sharpen = (float)Clamp(i["sharpen"].AsNumber(0.0), 0.0, 1.0);
  p.image.nativeWidth = Clamp(i["nativeWidth"].AsInt(0), 0, 4096);
  p.image.scanlines = (float)Clamp(i["scanlines"].AsNumber(0.0), 0.0, 0.5);
  p.image.mask = Clamp(i["mask"].AsInt(0), 0, 2);
  p.image.maskStrength = (float)Clamp(i["maskStrength"].AsNumber(0.35), 0.0, 0.5);
  p.image.deinterlace =
      ReadEnum<Deinterlace>(i, "deinterlace", kDeinterlaceCount, Deinterlace::Bob);
  p.image.deinterlaceAuto = i["deinterlaceAuto"].AsBool(true);
  p.image.fieldOrder = ReadEnum<FieldOrder>(i, "fieldOrder", 3, FieldOrder::Auto);
  p.image.lineDouble = i["lineDouble"].AsBool(false);
  p.image.rotation = ReadEnum<Rotation>(i, "rotation", kRotationCount, Rotation::None);
  p.image.chromaSoft = i["chromaSoft"].AsInt(0);
  p.image.temporalDenoise = (float)i["temporalDenoise"].AsNumber(0.0);
  p.image.dotNotch = (float)i["dotNotch"].AsNumber(0.0);
  p.image.aspect = ReadEnum<AspectMode>(i, "aspect", 5, AspectMode::Source);
  p.image.cropLeft = Clamp(i["cropLeft"].AsInt(0), 0, 4096);
  p.image.cropRight = Clamp(i["cropRight"].AsInt(0), 0, 4096);
  p.image.cropTop = Clamp(i["cropTop"].AsInt(0), 0, 4096);
  p.image.cropBottom = Clamp(i["cropBottom"].AsInt(0), 0, 4096);
  p.image.range = ReadEnum<ColorRange>(i, "range", 3, ColorRange::Auto);
  p.image.matrix = ReadEnum<ColorMatrix>(i, "matrix", 3, ColorMatrix::Auto);

  const json::Value& a = v["audio"];
  p.audio.output = ReadDevice(a["output"]);
  p.audio.bufferMs = Clamp(a["bufferMs"].AsInt(30), 2, 500);
  p.audio.exclusive = a["exclusive"].AsBool(false);
  p.audio.avOffsetMs = Clamp(a["avOffsetMs"].AsInt(0), -500, 500);
  p.audio.volume = (float)Clamp(a["volume"].AsNumber(1.0), 0.0, 1.0);
  p.audio.mute = a["mute"].AsBool(false);
  p.audio.micEnabled = a["micEnabled"].AsBool(false);
  p.audio.micDevice = ReadDevice(a["micDevice"]);
  p.audio.micGain = (float)Clamp(a["micGain"].AsNumber(1.0), 0.0, 4.0);
  p.audio.micTrackMode = ReadEnum<MicTrackMode>(a, "micTrackMode", 3, MicTrackMode::Both);

  return p;
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart > (LONGLONG)16 * 1024 * 1024) {
    ::CloseHandle(h);
    return false;
  }
  out.resize((size_t)size.QuadPart);
  DWORD read = 0;
  bool ok = out.empty() || (::ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) &&
                            read == out.size());
  ::CloseHandle(h);
  // Strip a UTF-8 BOM if an editor added one.
  if (ok && out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
      (unsigned char)out[2] == 0xBF) {
    out.erase(0, 3);
  }
  return ok;
}

// Writes to a sibling temp file and swaps it in, so an interrupted save cannot
// leave a truncated config behind.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) &&
            written == data.size();
  ok = ::FlushFileBuffers(h) && ok;
  ::CloseHandle(h);
  if (!ok) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------ FormatSel

std::string FormatSel::Label() const {
  if (!valid()) return T("(nicht gesetzt)", "(not set)");
  std::string fpsText;
  if (fps > 0.0) {
    fpsText = Format("%.2f", fps);
    // German decimal comma; the rest of the UI is German too.
    for (char& c : fpsText) {
      if (c == '.') c = ',';
    }
    // Trim a trailing ",00" so 60 fps reads as "60" and 59.94 stays "59,94".
    if (fpsText.size() > 3 && fpsText.compare(fpsText.size() - 3, 3, ",00") == 0) {
      fpsText.resize(fpsText.size() - 3);
    }
  }
  std::string out = Format("%dx%d", width, height);
  if (!fpsText.empty()) out += " @ " + fpsText + " Hz";
  if (!subtype.empty()) out += "  " + subtype;
  if (forced) out += T("  [erzwungen]", "  [forced]");
  return out;
}

// --------------------------------------------------------------------- Config

Config::Config() {
  profiles.emplace_back();  // one default profile so `active()` is always valid
}

Profile& Config::active() {
  if (profiles.empty()) profiles.emplace_back();
  if (activeProfile < 0 || activeProfile >= (int)profiles.size()) activeProfile = 0;
  return profiles[(size_t)activeProfile];
}

const Profile& Config::active() const {
  return const_cast<Config*>(this)->active();
}

void Config::SetActiveProfile(int index) {
  if (index >= 0 && index < (int)profiles.size()) activeProfile = index;
}

std::wstring Config::FilePath() {
  return ExeDirectory() + L"CapView.json";
}

bool Config::Load(std::string* error) {
  if (error) error->clear();

  std::string text;
  if (!ReadWholeFile(FilePath(), text)) {
    return false;  // no file yet -- first run, defaults apply
  }

  std::string parseError;
  json::Value root = json::Parse(text, &parseError);
  if (!parseError.empty() || !root.IsObject()) {
    if (error) {
      *error = parseError.empty() ? "Konfigurationsdatei hat kein gültiges Format"
                                  : ("Konfigurationsdatei fehlerhaft: " + parseError);
    }
    return false;
  }

  const json::Value& a = root["app"];
  app.theme = ReadEnum<Theme>(a, "theme", 3, Theme::Dark);
  app.language = ReadEnum<Language>(a, "language", 2, Language::German);
  app.accentColor = (unsigned)Clamp(a["accentColor"].AsInt(0x8B5CF6), 0, 0xFFFFFF);
  app.osdCorner = ReadEnum<OsdCorner>(a, "osdCorner", 4, OsdCorner::TopRight);
  app.showVolumeOsd = a["showVolumeOsd"].AsBool(true);
  app.wheelVolume = a["wheelVolume"].AsBool(true);
  app.vsync = a["vsync"].AsBool(false);
  app.alwaysOnTop = a["alwaysOnTop"].AsBool(false);
  app.hideCursorFullscreen = a["hideCursorFullscreen"].AsBool(true);
  app.preventSleep = a["preventSleep"].AsBool(true);
  app.showStats = a["showStats"].AsBool(false);
  app.showToolbar = a["showToolbar"].AsBool(true);
  app.settingsSeparateWindow = a["settingsSeparateWindow"].AsBool(false);
  app.settingsTab = a["settingsTab"].AsInt(0);
  app.settingsPanelX = a["settingsPanelX"].AsInt(-1);
  app.settingsPanelY = a["settingsPanelY"].AsInt(-1);
  app.settingsPanelW = a["settingsPanelW"].AsInt(0);
  app.settingsPanelH = a["settingsPanelH"].AsInt(0);
  app.settingsWindowX = a["settingsWindowX"].AsInt(-1);
  app.settingsWindowY = a["settingsWindowY"].AsInt(-1);
  app.settingsWindowW = a["settingsWindowW"].AsInt(0);
  app.settingsWindowH = a["settingsWindowH"].AsInt(0);
  app.checkUpdatesOnStart = a["checkUpdatesOnStart"].AsBool(true);
  app.virtualCamera = a["virtualCamera"].AsBool(false);
  app.hdrInput = ReadEnum<HdrInput>(a, "hdrInput", kHdrInputCount, HdrInput::Auto);
  app.hdrOutput = ReadEnum<HdrOutput>(a, "hdrOutput", kHdrOutputCount, HdrOutput::Auto);
  app.paperWhiteNits = (float)a["paperWhiteNits"].AsNumber(203.0);
  app.sourcePeakNits = (float)a["sourcePeakNits"].AsNumber(1000.0);
  app.recordHdr = a["recordHdr"].AsBool(false);
  app.screenshotHdr = a["screenshotHdr"].AsBool(false);
  app.hdrShotFormat = ReadEnum<HdrShotFormat>(a, "hdrShotFormat", kHdrShotFormatCount,
                                              HdrShotFormat::Jxr);
  app.cameraHdr = a["cameraHdr"].AsBool(false);
  app.statsDetail = ReadEnum<StatsDetail>(a, "statsDetail", 3, StatsDetail::Compact);
  app.logToFile = a["logToFile"].AsBool(false);
  app.windowX = a["windowX"].AsInt(-1);
  app.windowY = a["windowY"].AsInt(-1);
  app.windowW = Clamp(a["windowW"].AsInt(1280), 160, 16384);
  app.windowH = Clamp(a["windowH"].AsInt(720), 120, 16384);
  app.maximized = a["maximized"].AsBool(false);
  app.startFullscreen = a["startFullscreen"].AsBool(false);
  app.fullscreenMonitor = a["fullscreenMonitor"].AsInt(-1);

  const json::Value& r = root["record"];
  record.outputFolder = r["outputFolder"].AsString();
  record.container = ReadEnum<RecordContainer>(r, "container", 2, RecordContainer::Mkv);
  record.encoder =
      ReadEnum<RecordEncoder>(r, "encoder", kRecordEncoderCount, RecordEncoder::Auto);
  record.speed = ReadEnum<RecordSpeed>(r, "speed", 5, RecordSpeed::VeryFast);
  record.bitrateKbps = Clamp(r["bitrateKbps"].AsInt(20000), 500, 500000);
  record.rateControl = ReadEnum<RateControl>(r, "rateControl", kRateControlCount,
                                          RateControl::Cbr);
  record.qualityLevel = (int)r["qualityLevel"].AsNumber(23);
  record.preset = ReadEnum<EncoderPreset>(r, "preset", kEncoderPresetCount, EncoderPreset::Auto);
  record.tune = ReadEnum<EncoderTune>(r, "tune", kEncoderTuneCount, EncoderTune::Auto);
  record.multipass = ReadEnum<Multipass>(r, "multipass", kMultipassCount, Multipass::Auto);
  record.lookAhead = r["lookAhead"].AsBool(false);
  record.adaptiveQuant = r["adaptiveQuant"].AsBool(false);
  record.fps = Clamp(r["fps"].AsNumber(0.0), 0.0, 480.0);
  record.splitFiles = r["splitFiles"].AsBool(false);
  record.splitSizeMb = Clamp(r["splitSizeMb"].AsInt(4000), 100, 100000);
  record.ffmpegPath = r["ffmpegPath"].AsString();
  record.encoderProbeSignature = r["encoderProbeSignature"].AsString();
  record.encodersAvailable.clear();
  const json::Value& enc = r["encodersAvailable"];
  for (size_t i = 0; i < enc.Size(); ++i) {
    const int id = enc.At(i).AsInt(-1);
    if (id >= 0 && id < 10) record.encodersAvailable.push_back(id);
  }
  record.screenshotFolder = r["screenshotFolder"].AsString();
  record.screenshotFormat = ReadEnum<ScreenshotFormat>(r, "screenshotFormat", 2,
                                                       ScreenshotFormat::Png);
  record.jpegQuality = Clamp(r["jpegQuality"].AsInt(92), 1, 100);

  const json::Value& hk = root["hotkeys"];
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    const json::Value& entry = hk[HotkeyActionKey((HotkeyAction)i)];
    if (!entry.IsObject()) continue;  // missing entry keeps the factory default
    HotkeyBinding& b = hotkeys.items[i];
    b.vk = Clamp(entry["vk"].AsInt(b.vk), 0, 255);
    b.ctrl = entry["ctrl"].AsBool(false);
    b.shift = entry["shift"].AsBool(false);
    b.alt = entry["alt"].AsBool(false);
  }

  profiles.clear();
  const json::Value& list = root["profiles"];
  for (size_t i = 0; i < list.Size(); ++i) {
    profiles.push_back(ReadProfile(list.At(i)));
  }
  if (profiles.empty()) profiles.emplace_back();

  activeProfile = Clamp(root["activeProfile"].AsInt(0), 0, (int)profiles.size() - 1);
  return true;
}

bool Config::Save(std::string* error) const {
  if (error) error->clear();
  if (!WriteWholeFileAtomic(FilePath(), Serialize())) {
    if (error) {
      *error = T("Konfiguration konnte nicht geschrieben werden (Schreibrechte im Programmordner?)",
                 "Could not write the configuration (write access to the program folder?)");
    }
    return false;
  }
  return true;
}

std::string Config::Serialize() const {
  json::Value root = json::Value::Object();
  root["version"] = 1;

  json::Value a = json::Value::Object();
  a["theme"] = (int)app.theme;
  a["language"] = (int)app.language;
  a["accentColor"] = (int)app.accentColor;
  a["osdCorner"] = (int)app.osdCorner;
  a["showVolumeOsd"] = app.showVolumeOsd;
  a["wheelVolume"] = app.wheelVolume;
  a["vsync"] = app.vsync;
  a["alwaysOnTop"] = app.alwaysOnTop;
  a["hideCursorFullscreen"] = app.hideCursorFullscreen;
  a["preventSleep"] = app.preventSleep;
  a["showStats"] = app.showStats;
  a["showToolbar"] = app.showToolbar;
  a["settingsSeparateWindow"] = app.settingsSeparateWindow;
  a["settingsTab"] = app.settingsTab;
  a["settingsPanelX"] = app.settingsPanelX;
  a["settingsPanelY"] = app.settingsPanelY;
  a["settingsPanelW"] = app.settingsPanelW;
  a["settingsPanelH"] = app.settingsPanelH;
  a["settingsWindowX"] = app.settingsWindowX;
  a["settingsWindowY"] = app.settingsWindowY;
  a["settingsWindowW"] = app.settingsWindowW;
  a["settingsWindowH"] = app.settingsWindowH;
  a["checkUpdatesOnStart"] = app.checkUpdatesOnStart;
  a["virtualCamera"] = app.virtualCamera;
  a["hdrInput"] = (int)app.hdrInput;
  a["hdrOutput"] = (int)app.hdrOutput;
  a["paperWhiteNits"] = app.paperWhiteNits;
  a["sourcePeakNits"] = app.sourcePeakNits;
  a["recordHdr"] = app.recordHdr;
  a["screenshotHdr"] = app.screenshotHdr;
  a["hdrShotFormat"] = (int)app.hdrShotFormat;
  a["cameraHdr"] = app.cameraHdr;
  a["statsDetail"] = (int)app.statsDetail;
  a["logToFile"] = app.logToFile;
  a["windowX"] = app.windowX;
  a["windowY"] = app.windowY;
  a["windowW"] = app.windowW;
  a["windowH"] = app.windowH;
  a["maximized"] = app.maximized;
  a["startFullscreen"] = app.startFullscreen;
  a["fullscreenMonitor"] = app.fullscreenMonitor;
  root["app"] = a;

  json::Value r = json::Value::Object();
  r["outputFolder"] = record.outputFolder;
  r["container"] = (int)record.container;
  r["encoder"] = (int)record.encoder;
  r["speed"] = (int)record.speed;
  r["bitrateKbps"] = record.bitrateKbps;
  r["rateControl"] = (int)record.rateControl;
  r["qualityLevel"] = record.qualityLevel;
  r["preset"] = (int)record.preset;
  r["tune"] = (int)record.tune;
  r["multipass"] = (int)record.multipass;
  r["lookAhead"] = record.lookAhead;
  r["adaptiveQuant"] = record.adaptiveQuant;
  r["fps"] = record.fps;
  r["splitFiles"] = record.splitFiles;
  r["splitSizeMb"] = record.splitSizeMb;
  r["ffmpegPath"] = record.ffmpegPath;
  r["encoderProbeSignature"] = record.encoderProbeSignature;
  json::Value enc = json::Value::Array();
  for (int id : record.encodersAvailable) enc.Push(json::Value(id));
  r["encodersAvailable"] = enc;
  r["screenshotFolder"] = record.screenshotFolder;
  r["screenshotFormat"] = (int)record.screenshotFormat;
  r["jpegQuality"] = record.jpegQuality;
  root["record"] = r;

  json::Value hk = json::Value::Object();
  for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
    const HotkeyBinding& b = hotkeys.items[i];
    json::Value entry = json::Value::Object();
    entry["vk"] = b.vk;
    entry["ctrl"] = b.ctrl;
    entry["shift"] = b.shift;
    entry["alt"] = b.alt;
    hk[HotkeyActionKey((HotkeyAction)i)] = entry;
  }
  root["hotkeys"] = hk;

  json::Value list = json::Value::Array();
  for (const Profile& p : profiles) list.Push(WriteProfile(p));
  root["profiles"] = list;
  root["activeProfile"] = activeProfile;

  return json::Dump(root, 2);
}

}  // namespace cap
