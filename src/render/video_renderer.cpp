#include "render/video_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/shaders.h"

namespace cap {
namespace {

struct ConvertCB {
  int32_t formatKind;
  int32_t deinterlaceMode;
  int32_t fieldIndex;
  int32_t bottomUp;

  int32_t cropLeft;
  int32_t cropTop;
  int32_t srcWidth;
  int32_t srcHeight;

  int32_t outWidth;
  int32_t outHeight;
  int32_t isYuv;
  int32_t pad0;

  float yOffset;
  float yScale;
  float cScale;
  float pad1;

  float coef[4];
};
static_assert(sizeof(ConvertCB) % 16 == 0, "constant buffer must be 16 byte aligned");

struct ScaleCB {
  float srcSize[2];
  float dstSize[2];
  int32_t filter;
  float sharpen;
  float pad[2];
};
static_assert(sizeof(ScaleCB) % 16 == 0, "constant buffer must be 16 byte aligned");

ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, std::string* error) {
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  HRESULT hr = ::D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target,
                            flags, 0, &code, &errors);
  if (FAILED(hr)) {
    std::string detail = errors ? std::string((const char*)errors->GetBufferPointer(),
                                              errors->GetBufferSize())
                                : HrToString(hr);
    if (error) *error = "Shader (" + std::string(target) + ") konnte nicht kompiliert werden: " + detail;
    CAP_ERR("Shaderfehler: %s", detail.c_str());
    return nullptr;
  }
  return code;
}

// Rec.601 / Rec.709 conversion coefficients: Cr->R, Cb->G, Cr->G, Cb->B.
void MatrixCoefficients(ColorMatrix matrix, bool hd, float out[4]) {
  const bool use709 = (matrix == ColorMatrix::BT709) || (matrix == ColorMatrix::Auto && hd);
  if (use709) {
    out[0] = 1.5748f;
    out[1] = 0.187324f;
    out[2] = 0.468124f;
    out[3] = 1.8556f;
  } else {
    out[0] = 1.402f;
    out[1] = 0.344136f;
    out[2] = 0.714136f;
    out[3] = 1.772f;
  }
}

}  // namespace

VideoRenderer::~VideoRenderer() {
  Shutdown();
}

// ------------------------------------------------------------------ lifetime

bool VideoRenderer::Initialize(D3DContext* ctx, std::string* error) {
  ctx_ = ctx;
  if (!ctx_ || !ctx_->device()) {
    if (error) *error = "Kein Direct3D-Gerät";
    return false;
  }
  if (!CreateShaders(error)) return false;
  if (!CreateStates(error)) return false;
  return true;
}

void VideoRenderer::Shutdown() {
  ReleaseReadbackResources();
  ReleaseSourceTextures();
  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();
  blendOpaque_.Reset();
  raster_.Reset();
  sampLinear_.Reset();
  sampPoint_.Reset();
  cbScale_.Reset();
  cbConvert_.Reset();
  psScale_.Reset();
  psConvert_.Reset();
  vs_.Reset();
  ctx_ = nullptr;
}

bool VideoRenderer::CreateShaders(std::string* error) {
  ID3D11Device* dev = ctx_->device();

  ComPtr<ID3DBlob> vsCode = CompileShader(kFullscreenVS, "vs_4_0", error);
  if (!vsCode) return false;
  if (FAILED(CAP_HR(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                            nullptr, &vs_)))) {
    if (error) *error = "Vertex-Shader konnte nicht erstellt werden";
    return false;
  }

  ComPtr<ID3DBlob> convertCode = CompileShader(kConvertPS, "ps_4_0", error);
  if (!convertCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(convertCode->GetBufferPointer(),
                                           convertCode->GetBufferSize(), nullptr, &psConvert_)))) {
    if (error) *error = "Konvertierungs-Shader konnte nicht erstellt werden";
    return false;
  }

  ComPtr<ID3DBlob> scaleCode = CompileShader(kScalePS, "ps_4_0", error);
  if (!scaleCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(scaleCode->GetBufferPointer(), scaleCode->GetBufferSize(),
                                           nullptr, &psScale_)))) {
    if (error) *error = "Skalierungs-Shader konnte nicht erstellt werden";
    return false;
  }

  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  bd.ByteWidth = sizeof(ConvertCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbConvert_)))) {
    if (error) *error = "Konstantenpuffer konnte nicht erstellt werden";
    return false;
  }
  bd.ByteWidth = sizeof(ScaleCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbScale_)))) {
    if (error) *error = "Konstantenpuffer konnte nicht erstellt werden";
    return false;
  }
  return true;
}

bool VideoRenderer::CreateStates(std::string* error) {
  ID3D11Device* dev = ctx_->device();

  D3D11_SAMPLER_DESC sd = {};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;

  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (FAILED(CAP_HR(dev->CreateSamplerState(&sd, &sampPoint_)))) {
    if (error) *error = "Sampler konnte nicht erstellt werden";
    return false;
  }
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  if (FAILED(CAP_HR(dev->CreateSamplerState(&sd, &sampLinear_)))) {
    if (error) *error = "Sampler konnte nicht erstellt werden";
    return false;
  }

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(CAP_HR(dev->CreateRasterizerState(&rd, &raster_)))) {
    if (error) *error = "Rasterizer-State konnte nicht erstellt werden";
    return false;
  }

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(CAP_HR(dev->CreateBlendState(&bd, &blendOpaque_)))) {
    if (error) *error = "Blend-State konnte nicht erstellt werden";
    return false;
  }
  return true;
}

// ------------------------------------------------------------- source format

void VideoRenderer::ReleaseSourceTextures() {
  for (int i = 0; i < 3; ++i) {
    planeSrv_[i].Reset();
    plane_[i].Reset();
  }
  planeCount_ = 0;
  hasFrame_ = false;
}

bool VideoRenderer::SetSourceFormat(const VideoFormatInfo& info, std::string* error) {
  if (!info.valid()) {
    ReleaseSourceTextures();
    source_ = VideoFormatInfo{};
    return false;
  }
  const bool same = source_.width == info.width && source_.height == info.height &&
                    IsEqualGUID(source_.subtype, info.subtype) && planeCount_ > 0;
  source_ = info;
  if (same) return true;

  // New format means a new measurement. The verdict itself should not change --
  // the source decides its levels, not the pixel format we asked for -- but the
  // evidence has to be gathered from the new byte layout.
  rangeVerdict_ = RangeVerdict::Pending;
  rangeFramesSeen_ = 0;
  rangeSamples_ = 0;
  rangeBelow16_ = 0;
  rangeAbove235_ = 0;
  rangeMin_ = 255;
  rangeMax_ = 0;

  ReleaseSourceTextures();

  const std::string& label = info.subtypeLabel;
  if (label == "YUY2") {
    kind_ = FormatKind::Yuy2;
  } else if (label == "UYVY" || label == "HDYC") {
    kind_ = FormatKind::Uyvy;
  } else if (label == "YVYU") {
    kind_ = FormatKind::Yvyu;
  } else if (label == "NV12") {
    kind_ = FormatKind::Nv12;
  } else if (label == "YV12" || label == "I420" || label == "IYUV") {
    kind_ = FormatKind::Planar420;
    planarUvSwapped_ = (label == "YV12");  // YV12 stores V first
  } else {
    kind_ = FormatKind::Rgb;
  }

  return CreateSourceTextures(error);
}

bool VideoRenderer::CreateSourceTextures(std::string* error) {
  ID3D11Device* dev = ctx_->device();
  const int w = source_.width;
  const int h = source_.height;

  auto makePlane = [&](int index, int pw, int ph, DXGI_FORMAT fmt) -> bool {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)std::max(1, pw);
    td.Height = (UINT)std::max(1, ph);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &plane_[index])))) return false;
    if (FAILED(CAP_HR(dev->CreateShaderResourceView(plane_[index].Get(), nullptr,
                                                    &planeSrv_[index])))) {
      return false;
    }
    return true;
  };

  bool ok = true;
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Uyvy:
    case FormatKind::Yvyu:
      // Two pixels per RGBA texel.
      ok = makePlane(0, (w + 1) / 2, h, DXGI_FORMAT_R8G8B8A8_UNORM);
      planeCount_ = 1;
      break;
    case FormatKind::Nv12:
      ok = makePlane(0, w, h, DXGI_FORMAT_R8_UNORM) &&
           makePlane(1, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R8G8_UNORM);
      planeCount_ = 2;
      break;
    case FormatKind::Planar420:
      ok = makePlane(0, w, h, DXGI_FORMAT_R8_UNORM) &&
           makePlane(1, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R8_UNORM) &&
           makePlane(2, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R8_UNORM);
      planeCount_ = 3;
      break;
    case FormatKind::Rgb:
    default:
      // RGB24 is expanded to RGBA on upload; RGB32 goes in as BGRA directly.
      ok = makePlane(0, w, h,
                     source_.subtypeLabel == "RGB24" ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                     : DXGI_FORMAT_B8G8R8A8_UNORM);
      planeCount_ = 1;
      break;
  }

  if (!ok) {
    ReleaseSourceTextures();
    if (error) *error = "Videotexturen konnten nicht angelegt werden";
    return false;
  }
  CAP_LOG("Quelltexturen angelegt: %s %dx%d (%d Ebenen)", source_.subtypeLabel.c_str(), w, h,
          planeCount_);
  return true;
}

bool VideoRenderer::EnsureIntermediate(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (intermediate_ && intermediateWidth_ == width && intermediateHeight_ == height) return true;

  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

  ID3D11Device* dev = ctx_->device();
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &intermediate_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(intermediate_.Get(), nullptr, &intermediateSrv_)))) {
    return false;
  }
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(intermediate_.Get(), nullptr, &intermediateRtv_)))) {
    return false;
  }
  intermediateWidth_ = width;
  intermediateHeight_ = height;
  return true;
}

// ----------------------------------------------------------------- uploading

// How often a frame is looked at, and how many frames of evidence are wanted
// before the verdict is frozen. Every third frame for 40 frames is about two
// seconds at 60 Hz -- long enough to see some dark content, short enough that
// the picture has settled before anyone reaches for the settings.
static const int kRangeSampleEvery = 3;
static const int kRangeFramesWanted = 40;
// Give up and fall back to the format based rule after this many frames without
// ever seeing anything dark. A permanently bright source cannot be judged.
static const int kRangeFramesGiveUp = 600;
// Luma values per analysed frame. Sparse on purpose: this runs on the render
// thread and must not show up in the frame time.
static const int kRangeSamplesPerFrame = 8192;

void VideoRenderer::AnalyzeLevels(const FrameView& frame) {
  if (rangeVerdict_ != RangeVerdict::Pending) return;
  if (++rangeFramesSeen_ % kRangeSampleEvery != 0) return;

  const int w = source_.width;
  const int h = source_.height;
  if (w <= 0 || h <= 0) return;

  // Where the brightness lives, and how far apart two of them are. For the
  // packed YUV layouts luma is every other byte; NV12 and planar keep it in a
  // plane of its own; RGB has no luma, so every channel is looked at directly
  // -- limited range RGB confines all three to 16-235.
  size_t offset = 0;
  size_t step = 2;
  size_t span = 0;
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Yvyu:
      offset = 0; step = 2; span = (size_t)w * 2 * (size_t)h; break;
    case FormatKind::Uyvy:
      offset = 1; step = 2; span = (size_t)w * 2 * (size_t)h; break;
    case FormatKind::Nv12:
    case FormatKind::Planar420:
      offset = 0; step = 1; span = (size_t)w * (size_t)h; break;
    case FormatKind::Rgb:
    default:
      offset = 0; step = 1;
      span = (size_t)w * (size_t)h * (source_.subtypeLabel == "RGB24" ? 3 : 4);
      break;
  }
  if (span > frame.size) span = frame.size;
  if (span <= offset + step) return;

  // A prime stride so the samples do not land on the same column every row,
  // which on a UI heavy picture would read one vertical stripe and call it the
  // whole frame.
  const size_t usable = span - offset;
  const size_t count = usable / step;
  size_t stride = count / (size_t)kRangeSamplesPerFrame;
  if (stride < 1) stride = 1;
  if (stride % 2 == 0) ++stride;

  for (size_t i = 0; i < count; i += stride) {
    const int v = frame.data[offset + i * step];
    if (v < rangeMin_) rangeMin_ = v;
    if (v > rangeMax_) rangeMax_ = v;
    if (v < 16) ++rangeBelow16_;
    else if (v > 235) ++rangeAbove235_;
    ++rangeSamples_;
  }

  const int analysed = rangeFramesSeen_ / kRangeSampleEvery;
  if (analysed < kRangeFramesWanted || rangeSamples_ == 0) return;

  // Without any dark pixels there is nothing to tell the two apart: limited
  // range piles its blacks up at exactly 16, full range goes below it. Keep
  // watching rather than guessing from a bright menu screen.
  if (rangeMin_ > 40 && rangeFramesSeen_ < kRangeFramesGiveUp) return;

  const double below = (double)rangeBelow16_ / (double)rangeSamples_;
  // The black end decides. Values above 235 are not proof of anything: limited
  // range signals are allowed to carry superwhites, and plenty of sources do.
  rangeVerdict_ = below > 0.002 ? RangeVerdict::Full : RangeVerdict::Limited;
  CAP_LOG("Wertebereich erkannt: %s (min %d, max %d, %.3f %% unter 16, %llu Proben)",
          rangeVerdict_ == RangeVerdict::Full ? "voll 0-255" : "begrenzt 16-235", rangeMin_,
          rangeMax_, below * 100.0, (unsigned long long)rangeSamples_);
}

bool VideoRenderer::UploadFrame(const FrameView& frame) {
  if (!frame.valid() || planeCount_ == 0) return false;

  AnalyzeLevels(frame);

  bool ok = false;
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Uyvy:
    case FormatKind::Yvyu: ok = UploadPacked(frame); break;
    case FormatKind::Nv12: ok = UploadNv12(frame); break;
    case FormatKind::Planar420: ok = UploadPlanar(frame); break;
    case FormatKind::Rgb:
    default:
      ok = source_.subtypeLabel == "RGB24" ? UploadRgb24(frame) : UploadRgb32(frame);
      break;
  }
  if (ok) hasFrame_ = true;
  return ok;
}

namespace {

// Copies `rows` scanlines, honouring both the source and the mapped pitch.
void CopyRows(const D3D11_MAPPED_SUBRESOURCE& dst, const uint8_t* src, size_t srcPitch,
              size_t bytesPerRow, int rows) {
  const size_t copy = std::min(bytesPerRow, (size_t)dst.RowPitch);
  auto* out = (uint8_t*)dst.pData;
  for (int y = 0; y < rows; ++y) {
    memcpy(out + (size_t)y * dst.RowPitch, src + (size_t)y * srcPitch, copy);
  }
}

}  // namespace

bool VideoRenderer::UploadPacked(const FrameView& frame) {
  const int w = source_.width;
  const int h = source_.height;
  const size_t srcPitch = source_.stride > 0 ? (size_t)source_.stride : (size_t)w * 2;
  if (frame.size < srcPitch * (size_t)h) return false;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  ID3D11DeviceContext* dc = ctx_->context();
  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data, srcPitch, (size_t)w * 2, h);
  dc->Unmap(plane_[0].Get(), 0);
  return true;
}

bool VideoRenderer::UploadNv12(const FrameView& frame) {
  const int w = source_.width;
  const int h = source_.height;
  const size_t srcPitch = source_.stride > 0 ? (size_t)source_.stride : (size_t)w;
  const size_t lumaBytes = srcPitch * (size_t)h;
  const int ch = (h + 1) / 2;
  if (frame.size < lumaBytes + srcPitch * (size_t)ch) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  D3D11_MAPPED_SUBRESOURCE mapped = {};

  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data, srcPitch, (size_t)w, h);
  dc->Unmap(plane_[0].Get(), 0);

  if (FAILED(dc->Map(plane_[1].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  // Interleaved chroma: (w/2) texels of two bytes each.
  CopyRows(mapped, frame.data + lumaBytes, srcPitch, (size_t)((w + 1) / 2) * 2, ch);
  dc->Unmap(plane_[1].Get(), 0);
  return true;
}

bool VideoRenderer::UploadPlanar(const FrameView& frame) {
  const int w = source_.width;
  const int h = source_.height;
  const size_t yPitch = source_.stride > 0 ? (size_t)source_.stride : (size_t)w;
  const size_t cPitch = yPitch / 2;
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  const size_t lumaBytes = yPitch * (size_t)h;
  const size_t chromaBytes = cPitch * (size_t)ch;
  if (frame.size < lumaBytes + chromaBytes * 2 || cPitch == 0) return false;

  const uint8_t* first = frame.data + lumaBytes;
  const uint8_t* second = first + chromaBytes;
  // YV12 stores V before U; I420/IYUV store U before V. Bind so that plane 1 is
  // always U and plane 2 always V.
  const uint8_t* uPlane = planarUvSwapped_ ? second : first;
  const uint8_t* vPlane = planarUvSwapped_ ? first : second;

  ID3D11DeviceContext* dc = ctx_->context();
  D3D11_MAPPED_SUBRESOURCE mapped = {};

  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data, yPitch, (size_t)w, h);
  dc->Unmap(plane_[0].Get(), 0);

  if (FAILED(dc->Map(plane_[1].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, uPlane, cPitch, (size_t)cw, ch);
  dc->Unmap(plane_[1].Get(), 0);

  if (FAILED(dc->Map(plane_[2].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, vPlane, cPitch, (size_t)cw, ch);
  dc->Unmap(plane_[2].Get(), 0);
  return true;
}

bool VideoRenderer::UploadRgb24(const FrameView& frame) {
  const int w = source_.width;
  const int h = source_.height;
  const size_t srcPitch = source_.stride > 0 ? (size_t)source_.stride : (((size_t)w * 3 + 3) & ~3u);
  if (frame.size < srcPitch * (size_t)h) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;

  // DirectShow delivers BGR; the texture is RGBA, so expand and swap per pixel.
  auto* out = (uint8_t*)mapped.pData;
  for (int y = 0; y < h; ++y) {
    const uint8_t* src = frame.data + (size_t)y * srcPitch;
    uint8_t* dst = out + (size_t)y * mapped.RowPitch;
    for (int x = 0; x < w; ++x) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 255;
      src += 3;
      dst += 4;
    }
  }
  dc->Unmap(plane_[0].Get(), 0);
  return true;
}

bool VideoRenderer::UploadRgb32(const FrameView& frame) {
  const int w = source_.width;
  const int h = source_.height;
  const size_t srcPitch = source_.stride > 0 ? (size_t)source_.stride : (size_t)w * 4;
  if (frame.size < srcPitch * (size_t)h) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data, srcPitch, (size_t)w * 4, h);
  dc->Unmap(plane_[0].Get(), 0);
  return true;
}

// ------------------------------------------------------------------- drawing

// ----------------------------------------------------------------- readback

void VideoRenderer::SetReadbackEnabled(bool enabled) {
  if (enabled == readbackEnabled_) return;
  readbackEnabled_ = enabled;
  if (!enabled) ReleaseReadbackResources();
}

void VideoRenderer::ReleaseReadbackResources() {
  if (readbackMapped_ >= 0 && ctx_) {
    ctx_->context()->Unmap(readbackTex_[readbackMapped_].Get(), 0);
    readbackMapped_ = -1;
  }
  for (int i = 0; i < kReadbackSlots; ++i) readbackTex_[i].Reset();
  readbackWidth_ = readbackHeight_ = 0;
  readbackWrite_ = 0;
  readbackQueued_ = 0;
}

void VideoRenderer::QueueReadback() {
  if (!readbackEnabled_ || !intermediate_ || !ctx_) return;

  // Resolution changed (crop, or the card switched mode): start over.
  if (readbackWidth_ != intermediateWidth_ || readbackHeight_ != intermediateHeight_) {
    ReleaseReadbackResources();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)intermediateWidth_;
    td.Height = (UINT)intermediateHeight_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < kReadbackSlots; ++i) {
      if (FAILED(CAP_HR(ctx_->device()->CreateTexture2D(&td, nullptr, &readbackTex_[i])))) {
        ReleaseReadbackResources();
        readbackEnabled_ = false;
        return;
      }
    }
    readbackWidth_ = intermediateWidth_;
    readbackHeight_ = intermediateHeight_;
  }

  // The slot about to be written must not be the one the caller still holds.
  if (readbackWrite_ == readbackMapped_) return;

  ctx_->context()->CopyResource(readbackTex_[readbackWrite_].Get(), intermediate_.Get());
  readbackWrite_ = (readbackWrite_ + 1) % kReadbackSlots;
  if (readbackQueued_ < kReadbackSlots) ++readbackQueued_;
}

bool VideoRenderer::FetchReadback(ReadbackFrame* out) {
  if (!readbackEnabled_ || !ctx_ || readbackQueued_ < kReadbackSlots) return false;
  if (readbackMapped_ >= 0) return false;  // previous frame not released yet

  // Oldest slot: two copies have been queued behind it, so the GPU is long done
  // and the map returns immediately instead of stalling the pipeline.
  const int slot = readbackWrite_;
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = ctx_->context()->Map(readbackTex_[slot].Get(), 0, D3D11_MAP_READ,
                                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
  if (FAILED(hr)) return false;

  readbackMapped_ = slot;
  if (out) {
    out->data = (const uint8_t*)mapped.pData;
    out->stride = (int)mapped.RowPitch;
    out->width = readbackWidth_;
    out->height = readbackHeight_;
    out->size = (size_t)mapped.RowPitch * (size_t)readbackHeight_;
  }
  return true;
}

void VideoRenderer::ReleaseReadback() {
  if (readbackMapped_ < 0 || !ctx_) return;
  ctx_->context()->Unmap(readbackTex_[readbackMapped_].Get(), 0);
  readbackMapped_ = -1;
}

bool VideoRenderer::GrabStill(std::vector<uint8_t>* pixels, int* width, int* height) {
  if (!pixels || !intermediate_ || !ctx_ || intermediateWidth_ <= 0 || intermediateHeight_ <= 0) {
    return false;
  }

  // A staging texture of its own rather than a slot from the readback ring: the
  // ring only exists while recording, and its slots are deliberately two frames
  // stale. A screenshot should be the picture that was on screen when the key
  // went down.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)intermediateWidth_;
  td.Height = (UINT)intermediateHeight_;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(ctx_->device()->CreateTexture2D(&td, nullptr, &staging)))) return false;

  ctx_->context()->CopyResource(staging.Get(), intermediate_.Get());

  // Blocking map: D3D11_MAP_READ without DO_NOT_WAIT flushes and waits.
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(ctx_->context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) {
    return false;
  }

  const size_t rowBytes = (size_t)intermediateWidth_ * 4;
  pixels->resize(rowBytes * (size_t)intermediateHeight_);
  const uint8_t* src = (const uint8_t*)mapped.pData;
  for (int y = 0; y < intermediateHeight_; ++y) {
    uint8_t* dst = pixels->data() + (size_t)y * rowBytes;
    memcpy(dst, src + (size_t)y * (size_t)mapped.RowPitch, rowBytes);
    // The pipeline carries an alpha channel it has no use for. Whatever ended up
    // in it, a screenshot of opaque video is opaque.
    for (size_t x = 3; x < rowBytes; x += 4) dst[x] = 0xFF;
  }
  ctx_->context()->Unmap(staging.Get(), 0);

  if (width) *width = intermediateWidth_;
  if (height) *height = intermediateHeight_;
  return true;
}

void VideoRenderer::ComputeDestRect(const ImageSettings& image) {
  // Fit into the window minus the reserved strip, then push the result down by
  // it. Every branch below can then go on thinking it owns the whole window.
  ComputeDestRectIn(image, ctx_->width(), ctx_->height() - topInset_);
  videoRect_.top += topInset_;
  videoRect_.bottom += topInset_;
}

void VideoRenderer::ComputeDestRectIn(const ImageSettings& image, int winW, int winH) {
  const int srcW = croppedWidth_;
  const int srcH = croppedHeight_;

  if (winW <= 0 || winH <= 0 || srcW <= 0 || srcH <= 0) {
    videoRect_ = RECT{0, 0, 0, 0};
    return;
  }

  if (image.aspect == AspectMode::Stretch) {
    videoRect_ = RECT{0, 0, (LONG)winW, (LONG)winH};
    return;
  }

  if (image.aspect == AspectMode::Integer) {
    int scale = std::min(winW / srcW, winH / srcH);
    if (scale < 1) {
      // Window too small for 1:1 -- fall back to fitting, otherwise nothing
      // would be visible at all.
      const double a = (double)srcW / (double)srcH;
      int w = winW, h = (int)(winW / a);
      if (h > winH) {
        h = winH;
        w = (int)(winH * a);
      }
      videoRect_ = RECT{(winW - w) / 2, (winH - h) / 2, (winW - w) / 2 + w, (winH - h) / 2 + h};
      return;
    }
    const int w = srcW * scale;
    const int h = srcH * scale;
    videoRect_ = RECT{(winW - w) / 2, (winH - h) / 2, (winW - w) / 2 + w, (winH - h) / 2 + h};
    return;
  }

  double target;
  if (image.aspect == AspectMode::Force16x9) {
    target = 16.0 / 9.0;
  } else if (image.aspect == AspectMode::Force4x3) {
    target = 4.0 / 3.0;
  } else if (source_.aspectX > 0 && source_.aspectY > 0 && source_.width > 0 &&
             source_.height > 0) {
    // The media type's aspect describes the whole frame, so cropping has to be
    // folded in or a cropped picture would come out stretched.
    const double full = (double)source_.aspectX / (double)source_.aspectY;
    target = full * ((double)srcW / (double)source_.width) *
             ((double)source_.height / (double)srcH);
  } else {
    target = (double)srcW / (double)srcH;
  }
  if (target <= 0.0) target = (double)srcW / (double)srcH;

  int w = winW;
  int h = (int)std::lround(winW / target);
  if (h > winH) {
    h = winH;
    w = (int)std::lround(winH * target);
  }
  w = std::max(1, std::min(w, winW));
  h = std::max(1, std::min(h, winH));
  const int x = (winW - w) / 2;
  const int y = (winH - h) / 2;
  videoRect_ = RECT{x, y, x + w, y + h};
}

void VideoRenderer::Draw(const ImageSettings& image, int fieldIndex) {
  if (!hasFrame_ || planeCount_ == 0 || !ctx_) return;

  const int srcW = source_.width;
  const int srcH = source_.height;

  // Cropping, clamped so at least one pixel survives even with silly values.
  const int cropL = Clamp(image.cropLeft, 0, std::max(0, srcW - 1));
  const int cropT = Clamp(image.cropTop, 0, std::max(0, srcH - 1));
  const int cropR = Clamp(image.cropRight, 0, std::max(0, srcW - 1 - cropL));
  const int cropB = Clamp(image.cropBottom, 0, std::max(0, srcH - 1 - cropT));
  croppedWidth_ = std::max(1, srcW - cropL - cropR);
  croppedHeight_ = std::max(1, srcH - cropT - cropB);

  if (!EnsureIntermediate(croppedWidth_, croppedHeight_)) return;

  ID3D11DeviceContext* dc = ctx_->context();

  const bool interlaced = source_.interlaced;
  Deinterlace deint = image.deinterlace;
  if (image.deinterlaceAuto && !interlaced) deint = Deinterlace::Off;

  const bool isYuv = kind_ != FormatKind::Rgb;
  const bool hd = croppedHeight_ >= 720;

  ColorRange range = image.range;
  if (range == ColorRange::Auto) {
    if (source_.colorInfoPresent && (source_.nominalRange == 1 || source_.nominalRange == 2)) {
      // The driver said so outright, which beats both measuring and guessing.
      range = source_.nominalRange == 1 ? ColorRange::Full : ColorRange::Limited;
    } else if (rangeVerdict_ != RangeVerdict::Pending) {
      // Measured from the picture itself. This is the case that matters on cards
      // that attach no colour description, and unlike the rule below it gives
      // the same answer whichever pixel format the card was asked for -- which
      // is correct, because the levels come from the source, not the format.
      range = rangeVerdict_ == RangeVerdict::Full ? ColorRange::Full : ColorRange::Limited;
    } else {
      // Still measuring. YUV off a capture card is limited range far more often
      // than not; RGB is usually full.
      range = isYuv ? ColorRange::Limited : ColorRange::Full;
    }
  }

  ConvertCB cb = {};
  cb.formatKind = (int32_t)kind_;
  cb.deinterlaceMode = (int32_t)deint;
  cb.fieldIndex = fieldIndex ? 1 : 0;
  cb.bottomUp = source_.bottomUp ? 1 : 0;
  cb.cropLeft = cropL;
  cb.cropTop = cropT;
  cb.srcWidth = srcW;
  cb.srcHeight = srcH;
  cb.outWidth = croppedWidth_;
  cb.outHeight = croppedHeight_;
  cb.isYuv = isYuv ? 1 : 0;
  if (range == ColorRange::Limited) {
    cb.yOffset = 16.0f / 255.0f;
    cb.yScale = 255.0f / 219.0f;
    cb.cScale = 255.0f / 224.0f;
  } else {
    cb.yOffset = 0.0f;
    cb.yScale = 1.0f;
    cb.cScale = 1.0f;
  }
  ColorMatrix matrix = image.matrix;
  if (matrix == ColorMatrix::Auto) {
    if (source_.colorInfoPresent &&
        (source_.transferMatrix == 1 || source_.transferMatrix == 2)) {
      matrix = source_.transferMatrix == 1 ? ColorMatrix::BT709 : ColorMatrix::BT601;
    } else if (source_.subtypeLabel == "HDYC") {
      // HDYC is UYVY that carries BT.709 by definition.
      matrix = ColorMatrix::BT709;
    }
  }
  MatrixCoefficients(matrix, hd, cb.coef);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbConvert_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &cb, sizeof(cb));
    dc->Unmap(cbConvert_.Get(), 0);
  }

  // ---- pass 1: decode into the intermediate ----
  ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
  dc->PSSetShaderResources(0, 3, nullSrvs);

  ID3D11RenderTargetView* rtvs[] = {intermediateRtv_.Get()};
  dc->OMSetRenderTargets(1, rtvs, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)croppedWidth_;
  vp.Height = (float)croppedHeight_;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  ID3D11ShaderResourceView* srvs[3] = {planeSrv_[0].Get(), planeSrv_[1].Get(), planeSrv_[2].Get()};
  dc->PSSetShaderResources(0, 3, srvs);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psConvert_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cbConvert_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);
  dc->Draw(3, 0);

  // ---- pass 2: scale onto the back buffer ----
  dc->PSSetShaderResources(0, 3, nullSrvs);

  // Queue the recording copy here, between the passes: the intermediate holds
  // the finished picture at source resolution, and the copy runs on the GPU
  // while pass 2 is being set up rather than blocking anything.
  QueueReadback();

  ComputeDestRect(image);
  const int dstW = (int)(videoRect_.right - videoRect_.left);
  const int dstH = (int)(videoRect_.bottom - videoRect_.top);
  if (dstW <= 0 || dstH <= 0) return;

  ScaleCB sc = {};
  sc.srcSize[0] = (float)croppedWidth_;
  sc.srcSize[1] = (float)croppedHeight_;
  sc.dstSize[0] = (float)dstW;
  sc.dstSize[1] = (float)dstH;
  sc.filter = (int32_t)image.filter;
  sc.sharpen = Clamp(image.sharpen, 0.0f, 1.0f);
  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  ID3D11RenderTargetView* backbuffer[] = {ctx_->rtv()};
  dc->OMSetRenderTargets(1, backbuffer, nullptr);

  vp.TopLeftX = (float)videoRect_.left;
  vp.TopLeftY = (float)videoRect_.top;
  vp.Width = (float)dstW;
  vp.Height = (float)dstH;
  dc->RSSetViewports(1, &vp);

  ID3D11ShaderResourceView* scaleSrv[] = {intermediateSrv_.Get()};
  dc->PSSetShaderResources(0, 1, scaleSrv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  dc->PSSetShader(psScale_.Get(), nullptr, 0);
  ID3D11Buffer* scaleCbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, scaleCbs);
  dc->Draw(3, 0);

  // Leave the pipeline clean so ImGui's own state setup starts from scratch.
  dc->PSSetShaderResources(0, 1, nullSrvs);

  // Restore the full window viewport for whatever draws next.
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  vp.Width = (float)ctx_->width();
  vp.Height = (float)ctx_->height();
  dc->RSSetViewports(1, &vp);
}

}  // namespace cap
