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

  const VideoFormatInfo& sourceFormat() const { return source_; }

  // True when the current source is interlaced according to its media type.
  bool sourceInterlaced() const { return source_.interlaced; }

  // Effective size after cropping.
  int croppedWidth() const { return croppedWidth_; }
  int croppedHeight() const { return croppedHeight_; }

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
  enum class FormatKind { Yuy2 = 0, Uyvy = 1, Yvyu = 2, Nv12 = 3, Planar420 = 4, Rgb = 6 };

  bool CreateShaders(std::string* error);
  bool CreateStates(std::string* error);
  bool CreateSourceTextures(std::string* error);
  bool EnsureIntermediate(int width, int height);
  void ReleaseSourceTextures();

  bool UploadPacked(const FrameView& frame);
  bool UploadNv12(const FrameView& frame);
  bool UploadPlanar(const FrameView& frame);
  bool UploadRgb24(const FrameView& frame);
  bool UploadRgb32(const FrameView& frame);

  void ComputeDestRect(const ImageSettings& image);

  D3DContext* ctx_ = nullptr;

  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psConvert_;
  ComPtr<ID3D11PixelShader> psScale_;
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

  // Intermediate render target between the two passes.
  ComPtr<ID3D11Texture2D> intermediate_;
  ComPtr<ID3D11ShaderResourceView> intermediateSrv_;
  ComPtr<ID3D11RenderTargetView> intermediateRtv_;
  int intermediateWidth_ = 0;
  int intermediateHeight_ = 0;

  VideoFormatInfo source_;
  FormatKind kind_ = FormatKind::Rgb;
  bool planarUvSwapped_ = false;  // YV12 stores V before U
  bool hasFrame_ = false;

  int croppedWidth_ = 0;
  int croppedHeight_ = 0;
  RECT videoRect_ = {};

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
  RangeVerdict rangeVerdict_ = RangeVerdict::Pending;
  int rangeFramesSeen_ = 0;
  uint64_t rangeSamples_ = 0;
  uint64_t rangeBelow16_ = 0;
  uint64_t rangeAbove235_ = 0;
  int rangeMin_ = 255;
  int rangeMax_ = 0;
};

}  // namespace cap
