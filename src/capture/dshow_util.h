#pragma once

// Thin helpers over DirectShow: device enumeration, media type juggling,
// capability enumeration and crossbar routing.

#include <dshow.h>

#include <string>
#include <vector>

#include "common.h"
#include "config.h"

namespace cap {

// ------------------------------------------------------------- media subtypes

// Short label for a subtype GUID ("YUY2", "RGB24", "MJPG", ... ). Unknown GUIDs
// come back as their FOURCC if printable, otherwise as a hex string.
std::string SubtypeLabel(const GUID& subtype);

// Reverse lookup for the labels above. Returns false for unknown labels.
bool SubtypeFromLabel(const std::string& label, GUID* out);

// True for the uncompressed formats the renderer can upload directly.
// Compressed capture formats (MJPG) are still selectable -- the graph builder
// inserts a decoder whose output is one of these.
bool IsRendererSubtype(const GUID& subtype);

// Bytes per row and total image size for an uncompressed subtype.
// Returns 0 for formats we cannot size (compressed).
size_t ImageSizeForSubtype(const GUID& subtype, int width, int height, int* strideOut);

int BitsPerPixelForSubtype(const GUID& subtype);

// ---------------------------------------------------------------- media types

void FreeMediaType(AM_MEDIA_TYPE& mt);
void DeleteMediaType(AM_MEDIA_TYPE* mt);
AM_MEDIA_TYPE* CreateMediaTypeCopy(const AM_MEDIA_TYPE* src);

// Reads the parts of a video media type the renderer cares about.
struct VideoFormatInfo {
  GUID subtype = GUID_NULL;
  std::string subtypeLabel;
  int width = 0;
  int height = 0;       // always positive
  int stride = 0;       // bytes per row of the top plane
  bool bottomUp = false;  // RGB DIBs are stored upside down
  bool interlaced = false;
  bool fieldOneFirst = true;
  double fps = 0.0;
  int aspectX = 0;  // from VIDEOINFOHEADER2, 0 when unknown
  int aspectY = 0;
  size_t imageSize = 0;

  // Colour description the driver put in DXVA_ExtendedFormat. Most cards leave
  // this empty, but when it is there it beats guessing the range and matrix from
  // the picture height -- and it is where an HDR source announces PQ or HLG.
  bool colorInfoPresent = false;
  int nominalRange = 0;      // 1 = full 0-255, 2 = limited 16-235
  int transferMatrix = 0;    // 1 = BT.709, 2 = BT.601, 4/5 = BT.2020
  int primaries = 0;         // 2 = BT.709, 9 = BT.2020
  int transferFunction = 0;  // 5 = BT.709, 15 = PQ (ST.2084), 16 = HLG

  bool isHdrTransfer() const { return transferFunction == 15 || transferFunction == 16; }

  bool valid() const { return width > 0 && height > 0 && imageSize > 0; }
};

// Human readable names for the DXVA colour fields above, for the diagnostics.
const char* NominalRangeName(int value);
const char* TransferMatrixName(int value);
const char* PrimariesName(int value);
const char* TransferFunctionName(int value);

bool ParseVideoMediaType(const AM_MEDIA_TYPE* mt, VideoFormatInfo* out);

// ----------------------------------------------------------------- device enum

struct VideoDeviceInfo {
  std::string name;         // friendly name shown in the UI
  std::string id;           // DevicePath -- stable across restarts
  std::string monikerName;  // display name, used to re-bind quickly
};

std::vector<VideoDeviceInfo> EnumerateVideoDevices();

// DirectShow's own audio input category. Some capture cards expose their
// embedded audio here even when Windows hides the matching sound endpoint, so
// this is worth looking at when the audio pairing comes up empty.
std::vector<VideoDeviceInfo> EnumerateAudioCaptureDShowDevices();

// Resolves a saved reference to a live filter. Matching order: exact id, then
// moniker display name, then friendly name. `resolved` receives what was
// actually opened so the caller can write the fresh id back to the config.
ComPtr<IBaseFilter> CreateVideoFilter(const DeviceRef& ref, VideoDeviceInfo* resolved);

// Instantiates any enumerated device from its moniker display name. Works for
// every category, unlike CreateVideoFilter which searches the video category.
ComPtr<IBaseFilter> CreateFilterFromMoniker(const VideoDeviceInfo& info);

ComPtr<IPin> FindPinByDirection(IBaseFilter* filter, PIN_DIRECTION dir, int skip = 0);

// The pin that carries the live stream. Uses the capture category when a graph
// builder is available, otherwise falls back to the first output pin that
// exposes IAMStreamConfig.
ComPtr<IPin> FindCapturePin(ICaptureGraphBuilder2* builder, IBaseFilter* filter);

// ------------------------------------------------------------------ capability

// One capability as reported by IAMStreamConfig::GetStreamCaps. Drivers vary:
// some return a row per (format, resolution, fps), others a row per
// (format, resolution) with a frame rate range.
struct CapsEntry {
  GUID subtype = GUID_NULL;
  std::string subtypeLabel;
  int width = 0;
  int height = 0;
  double defaultFps = 0.0;
  double minFps = 0.0;
  double maxFps = 0.0;
  // Output size range for this entry, used to offer resolutions the driver
  // does not list explicitly.
  int minWidth = 0, maxWidth = 0, granularityX = 0;
  int minHeight = 0, maxHeight = 0, granularityY = 0;
};

std::vector<CapsEntry> EnumerateCaps(IPin* capturePin);

// A resolution or frame rate offered in the settings UI. `forced` marks values
// outside what the driver advertises -- they often work anyway (the card that
// claims 1080p30 but does 1080p60), but may fail.
struct ResolutionOption {
  int width = 0;
  int height = 0;
  bool forced = false;
};

struct FpsOption {
  double fps = 0.0;
  bool forced = false;
};

// Derives the three dropdown lists (format / resolution / frame rate) from a
// capability list, mixing advertised and forced values.
class CapsModel {
 public:
  void Build(std::vector<CapsEntry> entries);

  bool empty() const { return entries_.empty(); }
  const std::vector<CapsEntry>& entries() const { return entries_; }

  std::vector<std::string> Subtypes() const;
  std::vector<ResolutionOption> Resolutions(const std::string& subtype) const;
  std::vector<FpsOption> FpsList(const std::string& subtype, int width, int height) const;

  // True when the combination is advertised by the driver as-is.
  bool IsAdvertised(const std::string& subtype, int width, int height, double fps) const;

  // Picks a sensible starting format: highest resolution at the highest
  // advertised frame rate, preferring uncompressed formats.
  FormatSel PickDefault() const;

 private:
  std::vector<CapsEntry> entries_;
};

// Applies a format to the capture pin. Returns the media type that was actually
// negotiated in `applied` (optional). Failure means the driver rejected it.
HRESULT ApplyFormat(IPin* capturePin, const FormatSel& fmt, VideoFormatInfo* applied);

// -------------------------------------------------------------------- crossbar

struct CrossbarInput {
  int pinIndex = -1;  // index of the input pin on the crossbar
  long physicalType = 0;
  std::string name;  // "HDMI", "Component (YPbPr)", "Composite", ...
};

// Enumerates the video inputs of the crossbar upstream of `captureFilter`.
// Empty when the card has no crossbar (pure HDMI cards often do not).
std::vector<CrossbarInput> EnumerateCrossbarInputs(ICaptureGraphBuilder2* builder,
                                                   IBaseFilter* captureFilter);

// Routes the given input (index into the list above) to the crossbar output.
// Also routes the matching audio input when the crossbar has one.
bool RouteCrossbarInput(ICaptureGraphBuilder2* builder, IBaseFilter* captureFilter, int index);

std::string PhysicalConnectorName(long physicalType);

// ---------------------------------------------------------------------------
// Analogue video standard
//
// The setting behind the video standard list in a capture driver's own dialog.
// It matters more than it looks: PAL is 625 lines at 50 Hz and PAL-60 is 525 at
// 60, so a console that can do both needs the card told which one it is sending.
// Set the wrong one and either nothing locks at all or the picture arrives with
// the wrong number of lines in it.
//
// Values are the AnalogVideo_* bitmask from the DirectShow headers.

// Everything a card might offer, in a fixed order, so an index can be stored.
int VideoStandardCount();
long VideoStandardValue(int index);
const char* VideoStandardName(int index);
// Index of a bitmask value, or -1 when it is not one we know.
int VideoStandardIndexOf(long value);
// 525 or 625, or 0 when unknown. Derived from the standard, not measured.
int VideoStandardLines(long value);

// What the card says it can do, as a bitmask. Zero when it has no analogue
// decoder -- a pure HDMI input does not.
long AvailableVideoStandards(IBaseFilter* filter);
long CurrentVideoStandard(IBaseFilter* filter);
bool SetVideoStandard(IBaseFilter* filter, long standard);

// Whether the decoder has locked onto a signal: 1 yes, 0 no, -1 when the card
// cannot say. This one is genuinely measured -- the decoder loses the lock when
// it is set to the wrong line count -- which is what makes automatic selection
// possible at all. Its sibling get_NumberOfLines is not: that merely repeats
// the standard it was given.
int VideoStandardLocked(IBaseFilter* filter);

// The order automatic selection tries, filtered to what `available` offers.
// One variant per line count and colour system: the PAL letters differ only in
// sound carrier, which is nothing to do with the picture.
std::vector<long> AutoStandardCandidates(long available);

// Label for a stored setting: 0 "leave alone", -1 "automatic", otherwise the
// standard's name. Follows the selected language for the first two.
std::string VideoStandardSettingName(long setting);

// How many samples of a 720 pixel line one cycle of the colour subcarrier
// occupies. This is what the dot crawl filter needs to know: the crawl *is* the
// subcarrier leaking into brightness, so removing it means knowing its
// frequency. Both numbers follow from the standard and BT.601 sampling, and
// measuring the picture agrees with them to within two parts in a thousand.
double VideoStandardSubcarrierSamples(long standard);

// How many samples of a 720 pixel line one cycle of the colour subcarrier
// occupies. This is what the dot crawl filter needs to know: the crawl *is* the
// subcarrier leaking into brightness, so removing it means knowing its
// frequency. Both numbers follow from the standard and BT.601 sampling, and
// measuring the picture agrees with them to within two parts in a thousand.
double VideoStandardSubcarrierSamples(long standard);

}  // namespace cap
