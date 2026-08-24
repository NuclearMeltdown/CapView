#pragma once

// Two pass video pipeline.
//
//   pass 1  card format -> RGBA at cropped source resolution
//           (colour matrix, range, bob deinterlace, crop)
//   pass 2  RGBA -> window rectangle
//           (nearest / bilinear / bicubic / lanczos / sharp bilinear, sharpen)
//
// Splitting it this way keeps every scaling filter independent of what the card
// happens to deliver, and means the filter always works on real pixels rather
// than on half-decoded chroma.

#include <d3d11.h>

#include <vector>
#include <string>

#include "capture/dshow_util.h"
#include "capture/frame_sink.h"
#include "common.h"
#include "config.h"
#include "render/d3d_context.h"

namespace cap {

class VideoRenderer {
 public:
  VideoRenderer() = default;
  ~VideoRenderer();

  VideoRenderer(const VideoRenderer&) = delete;
  VideoRenderer& operator=(const VideoRenderer&) = delete;

  bool Initialize(D3DContext* ctx, std::string* error);
  void Shutdown();

  // Reconfigures the input textures. Cheap when the format did not change.
  bool SetSourceFormat(const VideoFormatInfo& info, std::string* error);

  // Copies one captured frame into GPU memory. Returns false on a size or
  // format mismatch.
  bool UploadFrame(const FrameView& frame);

  // Renders the last uploaded frame into the currently bound back buffer.
  // `fieldIndex` picks the field when bob deinterlacing is on.
  void Draw(const ImageSettings& image, int fieldIndex);

  bool hasFrame() const { return hasFrame_; }
  void DropFrame() { hasFrame_ = false; }

  // Where the picture ended up inside the window, in client pixels.
  const RECT& videoRect() const { return videoRect_; }

  // Pixels reserved at the top of the window for the toolbar. The picture is
  // fitted below them rather than drawn underneath, so the bar never covers
  // what you are playing.
  void SetTopInset(int pixels) { topInset_ = pixels < 0 ? 0 : pixels; }

  // Samples per cycle of the colour subcarrier, for a full width line. Set from
  // the video standard; the dot crawl filter is built around it.
  void SetCarrierSamples(double samples) { carrierSamples_ = samples; }

  const VideoFormatInfo& sourceFormat() const { return source_; }
  // What curve the incoming picture is encoded against. Kept apart from the
  // format: a card can send eight bit PQ, and ten bit says nothing about HDR.
  enum class Transfer { Sdr = 0, Pq = 1, Hlg = 2 };

  // Told rather than guessed, because a DirectShow media type mostly does not
  // carry this and when it does it is worth believing. `wideGamut` says the
  // primaries are BT.2020 and want bringing back to BT.709.
  void SetHdrInput(Transfer transfer, bool wideGamut) {
    hdrTransfer_ = transfer;
    hdrWideGamut_ = wideGamut;
  }
  Transfer hdrTransfer() const { return hdrTransfer_; }

  // The interface, when the swapchain is scRGB. Between these two calls the
  // interface draws into a buffer of its own; the second brings it back over
  // the picture in linear light. Both do nothing at all in SDR.
  bool BeginUiLayer();
  void CompositeUiLayer();

  // Where the picture is going. Nits, because that is the only unit in which
  // the two ends of this can be compared at all.
  void SetHdrOutput(bool scRgbOutput, float paperWhiteNits, float sourcePeakNits,
                    float displayPeakNits) {
    hdrOutput_ = scRgbOutput;
    paperWhiteNits_ = paperWhiteNits;
    sourcePeakNits_ = sourcePeakNits;
    displayPeakNits_ = displayPeakNits;
  }

  // True when the current source is interlaced according to its media type.
  bool sourceInterlaced() const { return source_.interlaced; }

  // Effective size after cropping.
  // Size of the picture this produces: cropped, line doubled and rotated. This
  // is what the recorder and the screenshots get, so it is the size that counts
  // everywhere outside the shader.
  int outputWidth() const { return outputWidth_; }
  int outputHeight() const { return outputHeight_; }

  // ---- readback for recording ----
  //
  // Hands out the intermediate image -- cropped, deinterlaced, colour corrected,
  // at source resolution, before any display scaling. That is exactly what the
  // viewer shows and exactly what a recording should contain, and it does not
  // depend on the window size.
  //
  // The copy is queued on the GPU and read two frames later, so the display path
  // never waits on it. The recording is therefore a frame or two behind the
  // screen, which is the right trade: the screen is what you play on.
  void SetReadbackEnabled(bool enabled);
  bool readbackEnabled() const { return readbackEnabled_; }

  // Byte order the readback delivers, written the way ffmpeg names it. The
  // staging texture is DXGI_FORMAT_R8G8B8A8_UNORM, so the bytes run R, G, B, A
  // -- that is ffmpeg's "rgba", not "bgra". Whoever changes the texture format
  // in video_renderer.cpp changes this line in the same edit. Getting it wrong
  // is not a crash: red and blue simply trade places, and orange comes back
  // blue.
  static constexpr const char* kReadbackPixelFormat = "rgba";

  struct ReadbackFrame {
    const uint8_t* data = nullptr;  // kReadbackPixelFormat, valid until ReleaseReadback
    size_t size = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool valid() const { return data != nullptr; }
  };

  // Returns true when a frame was mapped; call ReleaseReadback when done with it.
  bool FetchReadback(ReadbackFrame* out);
  void ReleaseReadback();

  // ---- automatic level detection ----
  //
  // What a capture card hands over is limited range (16-235) or full range
  // (0-255) depending on what the *source* is sending -- a console setting, not
  // a property of the pixel format you asked the card for. Plenty of cards,
  // this one included, attach no colour description at all, so there is nothing
  // to read: the answer has to come from the pixels.
  //
  // Measured over the first couple of seconds after the format changes, then
  // frozen. A verdict that keeps changing with the content would be worse than
  // a wrong one held steady, so it is decided once and left alone.
  enum class RangeVerdict { Pending, Limited, Full };
  RangeVerdict detectedRange() const { return rangeVerdict_; }

  // Whether the source is interlaced, measured the same way and for the same
  // reason: the media type is the obvious place to ask and routinely does not
  // answer. A plain VIDEOINFOHEADER has no field to say it in, and cards that do
  // use VIDEOINFOHEADER2 often leave the interlace flags at zero -- so a 480i
  // console can arrive described as progressive.
  //
  // Unlike the range verdict this one is never frozen on "progressive". Combing
  // only exists where something moved, and a paused game or a title screen has
  // nothing moving in it; calling that progressive and sticking to it would
  // leave the deinterlacer switched off for the rest of the session. So the
  // measurement runs in windows and keeps running, and only "interlaced" latches
  // -- once combing has been seen there is no reason to doubt it again.
  enum class InterlaceVerdict { Pending, Progressive, Interlaced };
  InterlaceVerdict detectedInterlace() const { return interlaceVerdict_; }

  // True when the two fields hold the *same* lines rather than lines half a
  // picture line apart -- a 240p or 288p console that the card packed into an
  // interlaced frame. Such a source is still interlaced in the sense that
  // matters, because the two fields are half a field apart in time and will comb
  // on anything that moves. What it does not need is spatial reconstruction:
  // nothing is missing, so the right answer is to take the field's own line and
  // use it twice.
  bool sourceCoSitedFields() const { return coSitedFields_; }

  // Where the picture sits inside the frame, in source pixels, edges inclusive.
  // False until something has been measured. This is what the automatic crop
  // reads: a console pillarboxed inside a 720 pixel line, or an overscan band
  // the card hands over as black, is found here rather than by eye.
  bool contentBounds(int* left, int* top, int* right, int* bottom) const;

  // Throws every measurement away and starts over. Switching the crossbar puts a
  // different signal on the same pins without the format changing, and none of
  // the verdicts survive that.
  void ResetAnalysis();

  // Copies the current picture out once, tightly packed, in
  // kReadbackPixelFormat. Unlike the recording path this waits for the GPU,
  // which costs a millisecond or two -- acceptable for a key press, not for
  // every frame. Alpha comes back opaque. Must be called on the render thread.
  bool GrabStill(std::vector<uint8_t>* pixels, int* width, int* height);

 private:
  void QueueReadback();
  void ReleaseReadbackResources();

 public:

 private:
  // 7 is NV12's arrangement in sixteen bit containers -- P010 and P016, which
  // is what an HDR capable card hands over.
  enum class FormatKind {
    Yuy2 = 0, Uyvy = 1, Yvyu = 2, Nv12 = 3, Planar420 = 4, Rgb = 6, P010 = 7
  };


  bool CreateShaders(std::string* error);
  bool CreateStates(std::string* error);
  bool CreateSourceTextures(std::string* error);
  bool EnsureIntermediate(int width, int height);
  bool EnsureSdrCopy(int width, int height);
  void RenderSdrCopy();
  void ReleaseSourceTextures();

  bool UploadPacked(const FrameView& frame);
  bool UploadNv12(const FrameView& frame);
  bool UploadP010(const FrameView& frame);
  bool EnsureUiLayer(int width, int height);
  bool UploadPlanar(const FrameView& frame);
  bool UploadRgb24(const FrameView& frame);
  bool UploadRgb32(const FrameView& frame);

  void ComputeDestRect(const ImageSettings& image);
  void ComputeDestRectIn(const ImageSettings& image, int winW, int winH);

  D3DContext* ctx_ = nullptr;

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psClean_;
  ComPtr<ID3D11PixelShader> psConvert_;
  ComPtr<ID3D11PixelShader> psScale_;
  ComPtr<ID3D11PixelShader> psUiComposite_;
  ComPtr<ID3D11Texture2D> uiTex_;
  ComPtr<ID3D11RenderTargetView> uiRtv_;
  ComPtr<ID3D11ShaderResourceView> uiSrv_;
  ComPtr<ID3D11Buffer> cbUi_;
  ComPtr<ID3D11BlendState> blendPremultiplied_;
  int uiWidth_ = 0;
  int uiHeight_ = 0;
  ComPtr<ID3D11Buffer> cbConvert_;
  ComPtr<ID3D11Buffer> cbScale_;
  ComPtr<ID3D11SamplerState> sampPoint_;
  ComPtr<ID3D11SamplerState> sampLinear_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11BlendState> blendOpaque_;

  // Input planes. Which of these exist depends on the source format.
  ComPtr<ID3D11Texture2D> plane_[3];
  ComPtr<ID3D11ShaderResourceView> planeSrv_[3];
  int planeCount_ = 0;

  // The three preceding frames, as a ring. Filled by copying the current planes
  // just before they are overwritten, which is cheaper than uploading twice and
  // keeps the capture path untouched. One copy per frame regardless of depth;
  // the copies only happen while something that reads them is switched on.
  //
  // Three because the composite denoiser needs a four frame window -- see the
  // measurement in the shader. YADIF only ever looks at the first of them.
  static const int kHistoryDepth = 3;
  ComPtr<ID3D11Texture2D> planeHist_[kHistoryDepth][3];
  ComPtr<ID3D11ShaderResourceView> planeHistSrv_[kHistoryDepth][3];
  int historyWrite_ = 0;   // ring slot the next copy goes into
  int historyCount_ = 0;   // slots that hold a picture, up to kHistoryDepth
  bool historyWanted_ = false;

  // Between the capture planes and the deinterlacer: the picture as the card
  // sent it, in its own geometry, with the composite artefacts already taken
  // out. Floating point because limited range material legitimately runs past
  // both ends once it has been expanded, and rounding that back into eight bits
  // here would throw away what the expansion just recovered.
  bool EnsureClean(int width, int height);
  ComPtr<ID3D11Texture2D> cleanTex_;
  ComPtr<ID3D11ShaderResourceView> cleanSrv_;
  ComPtr<ID3D11RenderTargetView> cleanRtv_;
  // The same, one frame back. YADIF is the only thing that reads it.
  ComPtr<ID3D11Texture2D> cleanPrevTex_;
  ComPtr<ID3D11ShaderResourceView> cleanPrevSrv_;
  int cleanWidth_ = 0;
  int cleanHeight_ = 0;

  // Intermediate render target between the two passes.
  ComPtr<ID3D11Texture2D> intermediate_;
  ComPtr<ID3D11ShaderResourceView> intermediateSrv_;
  ComPtr<ID3D11RenderTargetView> intermediateRtv_;
  int intermediateWidth_ = 0;
  int intermediateHeight_ = 0;

  VideoFormatInfo source_;
  bool tenBitContainer_ = false;
  DXGI_FORMAT intermediateFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D11Texture2D> sdrCopy_;
  ComPtr<ID3D11RenderTargetView> sdrCopyRtv_;
  int sdrCopyWidth_ = 0;
  int sdrCopyHeight_ = 0;
  Transfer hdrTransfer_ = Transfer::Sdr;
  bool hdrWideGamut_ = false;
  bool hdrOutput_ = false;
  float paperWhiteNits_ = 203.0f;
  float sourcePeakNits_ = 1000.0f;
  float displayPeakNits_ = 100.0f;
  FormatKind kind_ = FormatKind::Rgb;
  bool planarUvSwapped_ = false;  // YV12 stores V before U
  bool hasFrame_ = false;

  int croppedWidth_ = 0;
  int croppedHeight_ = 0;
  int outputWidth_ = 0;
  int outputHeight_ = 0;
  RECT videoRect_ = {};
  int topInset_ = 0;
  double carrierSamples_ = 3.0449;  // PAL, the common case here

  // Scratch row buffer for RGB24, which has no matching DXGI format.
  std::vector<uint8_t> expandBuffer_;

  // Readback ring. Three staging textures: one being written by the GPU, one
  // in flight, one old enough to map without stalling.
  static const int kReadbackSlots = 3;
  ComPtr<ID3D11Texture2D> readbackTex_[kReadbackSlots];
  int readbackWidth_ = 0;
  int readbackHeight_ = 0;
  int readbackWrite_ = 0;
  int readbackQueued_ = 0;
  int readbackMapped_ = -1;  // slot currently mapped, -1 when none
  bool readbackEnabled_ = false;

  // Level detection state, reset whenever the source format changes.
  void AnalyzeLevels(const FrameView& frame);
  void AnalyzeInterlace(const FrameView& frame);
  void AnalyzeContentBounds(const FrameView& frame);
  // Byte offset of the first luma sample and the distance to the next one.
  // False for formats this cannot read.
  bool LumaLayout(size_t* offset, size_t* step) const;
  RangeVerdict rangeVerdict_ = RangeVerdict::Pending;
  int rangeFramesSeen_ = 0;
  uint64_t rangeSamples_ = 0;
  uint64_t rangeBelow16_ = 0;
  uint64_t rangeAbove235_ = 0;
  int rangeMin_ = 255;
  int rangeMax_ = 0;

  InterlaceVerdict interlaceVerdict_ = InterlaceVerdict::Pending;
  bool coSitedFields_ = false;
  // 0 when a pair starts on an even row, 1 when it starts on an odd one.
  int coSitedPhase_ = 0;
  int combFramesSeen_ = 0;
  uint64_t combSamples_ = 0;
  uint64_t combHits_ = 0;
  // How many of the frames in the current window combed. Counting frames rather
  // than averaging over all of them is the difference between noticing a second
  // of movement in an otherwise still scene and averaging it away.
  int combFrameHits_ = 0;
  int combFramesAnalysed_ = 0;
  // Sums of the difference between the two rows of a pair, and between two
  // neighbouring pairs. On a line doubled picture the first is nearly zero.
  uint64_t pairInner_ = 0;
  uint64_t pairOuter_ = 0;

  // Content bounds. Measured as a union across a window of frames, because a
  // fade to black is not evidence that the picture got smaller, and published
  // only once the window is complete so the answer never flickers.
  bool boundsValid_ = false;
  int boundsL_ = 0, boundsT_ = 0, boundsR_ = 0, boundsB_ = 0;
  int accL_ = 0, accT_ = 0, accR_ = 0, accB_ = 0;
  bool accAny_ = false;
  int boundsFramesSeen_ = 0;
  std::vector<int> columnHits_;  // scratch, sized once per format
};

}  // namespace cap
