#include "render/video_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/shaders.h"
#include "i18n.h"

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
  int32_t havePrev;

  float yOffset;
  float yScale;
  float cScale;
  // P010 keeps ten bits in the top of sixteen, so a R16_UNORM read comes back
  // 1023*64/65535 for full scale rather than 1.0. This puts that right; it is
  // 1.0 for everything else.
  float pixelScale;

  // -1 = the fields are half a picture line apart, as a real interlaced signal
  // has them. 0 or 1 = they are co-sited, and this is the row a pair starts on.
  int32_t coSitedPhase;
  int32_t rotation;
  int32_t lineDouble;
  int32_t chromaSoft;

  float temporal;
  int32_t histCount;
  float dotNotch;
  float carrierPeriod;

  // 0 = the picture is already display referred, as everything SDR is.
  // 1 = PQ (SMPTE ST 2084), 2 = HLG. Either way the clean pass turns it into
  // linear light with 1.0 meaning diffuse white.
  int32_t transfer;
  int32_t gamut;  // 1 = the primaries are BT.2020 and want converting to BT.709
  // Where the temporal filter's motion gate lets go: how much movement it
  // ignores, and how fast it gives up above that. See TemporalGate.
  float motionSlack;
  float motionSlope;

  float coef[4];
};
static_assert(sizeof(ConvertCB) % 16 == 0, "constant buffer must be 16 byte aligned");

struct ScaleCB {
  float srcSize[2];
  float dstSize[2];
  int32_t filter;
  float sharpen;
  int32_t transfer;    // as ConvertCB::transfer -- was the source HDR
  int32_t outputHdr;   // the swapchain is scRGB and wants linear light

  float paperWhite;    // nits the source's diffuse white should come out at
  float sourcePeak;    // nits the brightest part of the source is assumed to reach
  float displayPeak;   // nits this display can actually manage
  float scanlines;     // 0..1, how dark the gaps between source lines go

  int32_t mask;        // 0 off, 1 aperture grille, 2 shadow mask
  float maskStrength;  // 0..1
  int32_t nativeWidth; // pixels the source really has across, 0 = leave alone
  float linePitch;     // output rows per real picture line; 0 disables scanlines

  int32_t passthrough; // resample only: no display effects, no transfer, no clamp
  float brightness;    // -1..1 added, or stops of exposure in linear light
  float contrast;      // 0..2 around a pivot; 1 neutral
  float saturation;    // 0..2; 1 neutral

  float hue;           // radians
  int32_t procAmp;     // apply the four above in this pass
  float pad[2];
};
static_assert(sizeof(ScaleCB) % 16 == 0, "constant buffer must be 16 byte aligned");

// Where a compiled shader is kept between runs. The conversion shader has grown
// into several hundred lines of branching, and D3DCompile spends seconds on it
// at optimisation level three -- seconds the user waits through before the
// window appears, every single time, to arrive at byte-for-byte the same answer.
//
// The name carries a hash of the source and the target profile, so editing the
// shader or changing the profile simply misses the cache rather than loading
// something stale. A miss costs what it always cost; there is nothing to
// invalidate by hand.
std::wstring ShaderCachePath(const char* source, const char* target) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  for (const char* p = source; *p; ++p) {
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  }
  for (const char* p = target; *p; ++p) {
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  }
  wchar_t name[64];
  ::swprintf(name, 64, L"shader-%016llx.cso", (unsigned long long)hash);
  return ExeDirectory() + name;
}

ComPtr<ID3DBlob> LoadCachedShader(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER size = {};
  ComPtr<ID3DBlob> blob;
  if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22) &&
      SUCCEEDED(::D3DCreateBlob((SIZE_T)size.QuadPart, &blob))) {
    DWORD read = 0;
    if (!::ReadFile(file, blob->GetBufferPointer(), (DWORD)size.QuadPart, &read, nullptr) ||
        read != size.QuadPart) {
      blob.Reset();
    }
  }
  ::CloseHandle(file);
  return blob;
}

void StoreCachedShader(const std::wstring& path, ID3DBlob* code) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;  // read-only folder: compile every time, no harm
  DWORD written = 0;
  ::WriteFile(file, code->GetBufferPointer(), (DWORD)code->GetBufferSize(), &written, nullptr);
  ::CloseHandle(file);
}

// Was in diesem Lauf tatsaechlich gebraucht wurde. Alles andere neben der exe
// ist ein Rest aus einer aelteren Fassung des Shaders.
std::vector<std::wstring>& ShaderCacheInUse() {
  static std::vector<std::wstring> names;
  return names;
}

// Loescht die Dateien, die kein Shader dieser Fassung mehr beansprucht. Der
// Cache ist nach Inhalt benannt, eine geaenderte Quelle trifft also eine neue
// Datei und die alte bleibt sonst fuer immer liegen -- nach ein paar Releases
// steht da ein Dutzend toter Blobs.
void PruneShaderCache() {
  const std::wstring dir = ExeDirectory();
  WIN32_FIND_DATAW found = {};
  HANDLE search = ::FindFirstFileW((dir + L"shader-*.cso").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  int removed = 0;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring path = dir + found.cFileName;
    bool live = false;
    for (const std::wstring& used : ShaderCacheInUse()) {
      if (_wcsicmp(used.c_str(), path.c_str()) == 0) {
        live = true;
        break;
      }
    }
    // Ein Fehlschlag ist keiner: liegt die Datei fest, weil eine zweite Instanz
    // sie gerade liest, ist der naechste Start wieder an der Reihe.
    if (!live && ::DeleteFileW(path.c_str())) ++removed;
  } while (::FindNextFileW(search, &found));
  ::FindClose(search);
  if (removed > 0) CAP_LOG("Shadercache: %d veraltete Datei(en) entfernt", removed);
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, std::string* error) {
  const std::wstring cachePath = ShaderCachePath(source, target);
  ShaderCacheInUse().push_back(cachePath);
  if (ComPtr<ID3DBlob> cached = LoadCachedShader(cachePath)) return cached;

  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const DWORD started = ::GetTickCount();
  HRESULT hr = ::D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target,
                            flags, 0, &code, &errors);
  if (SUCCEEDED(hr)) {
    CAP_LOG("Shader %s kompiliert in %lu ms, gespeichert", target,
            (unsigned long)(::GetTickCount() - started));
    StoreCachedShader(cachePath, code.Get());
  }
  if (FAILED(hr)) {
    std::string detail = errors ? std::string((const char*)errors->GetBufferPointer(),
                                              errors->GetBufferSize())
                                : HrToString(hr);
    if (error) *error = T("Shader (", "Shader (") + std::string(target) +
                  T(") konnte nicht kompiliert werden: ", ") could not be compiled: ") + detail;
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
    if (error) *error = T("Kein Direct3D-Gerät", "No Direct3D device");
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
    if (error) *error = T("Vertex-Shader konnte nicht erstellt werden",
                             "The vertex shader could not be created");
    return false;
  }

  ComPtr<ID3DBlob> cleanCode = CompileShader(kCleanPS, "ps_4_0", error);
  if (!cleanCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(cleanCode->GetBufferPointer(),
                                           cleanCode->GetBufferSize(), nullptr, &psClean_)))) {
    if (error) *error = T("Aufbereitungs-Shader konnte nicht erstellt werden",
                             "The cleanup shader could not be created");
    return false;
  }

  ComPtr<ID3DBlob> convertCode = CompileShader(kConvertPS, "ps_4_0", error);
  if (!convertCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(convertCode->GetBufferPointer(),
                                           convertCode->GetBufferSize(), nullptr, &psConvert_)))) {
    if (error) *error = T("Konvertierungs-Shader konnte nicht erstellt werden",
                             "The conversion shader could not be created");
    return false;
  }

  ComPtr<ID3DBlob> scaleCode = CompileShader(kScalePS, "ps_4_0", error);
  if (!scaleCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(scaleCode->GetBufferPointer(), scaleCode->GetBufferSize(),
                                           nullptr, &psScale_)))) {
    if (error) *error = T("Skalierungs-Shader konnte nicht erstellt werden",
                             "The scaling shader could not be created");
    return false;
  }

  ComPtr<ID3DBlob> recordCode = CompileShader(kHdrRecordPS, "ps_4_0", error);
  if (!recordCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(recordCode->GetBufferPointer(),
                                           recordCode->GetBufferSize(), nullptr,
                                           &psHdrRecord_)))) {
    if (error) *error = T("Aufnahme-Shader konnte nicht erstellt werden",
                             "The recording shader could not be created");
    return false;
  }

  ComPtr<ID3DBlob> uiCode = CompileShader(kUiCompositePS, "ps_4_0", error);
  if (!uiCode) return false;
  if (FAILED(CAP_HR(dev->CreatePixelShader(uiCode->GetBufferPointer(), uiCode->GetBufferSize(),
                                           nullptr, &psUiComposite_)))) {
    if (error) *error = T("Oberflächen-Shader konnte nicht erstellt werden",
                             "The interface shader could not be created");
    return false;
  }

  // Erst hier, wo feststeht, welche Dateien diese Fassung braucht.
  PruneShaderCache();

  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  bd.ByteWidth = sizeof(ConvertCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbConvert_)))) {
    if (error) *error = T("Konstantenpuffer konnte nicht erstellt werden",
                             "The constant buffer could not be created");
    return false;
  }
  bd.ByteWidth = 16;  // one float plus padding
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbRecord_)))) {
    if (error) *error = T("Konstantenpuffer konnte nicht erstellt werden",
                             "The constant buffer could not be created");
    return false;
  }
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbUi_)))) {
    if (error) *error = T("Konstantenpuffer konnte nicht erstellt werden",
                             "The constant buffer could not be created");
    return false;
  }

  // Premultiplied: what arrives has already been multiplied by its own coverage,
  // so the source contributes as it is rather than being scaled again.
  D3D11_BLEND_DESC pm = {};
  pm.RenderTarget[0].BlendEnable = TRUE;
  pm.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  pm.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  pm.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  pm.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  pm.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  pm.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  pm.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(CAP_HR(dev->CreateBlendState(&pm, &blendPremultiplied_)))) {
    if (error) *error = T("Blend-State konnte nicht erstellt werden",
                             "The blend state could not be created");
    return false;
  }

  bd.ByteWidth = sizeof(ScaleCB);
  if (FAILED(CAP_HR(dev->CreateBuffer(&bd, nullptr, &cbScale_)))) {
    if (error) *error = T("Konstantenpuffer konnte nicht erstellt werden",
                             "The constant buffer could not be created");
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
    if (error) *error = T("Sampler konnte nicht erstellt werden",
                             "The sampler could not be created");
    return false;
  }
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  if (FAILED(CAP_HR(dev->CreateSamplerState(&sd, &sampLinear_)))) {
    if (error) *error = T("Sampler konnte nicht erstellt werden",
                             "The sampler could not be created");
    return false;
  }

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(CAP_HR(dev->CreateRasterizerState(&rd, &raster_)))) {
    if (error) *error = T("Rasterizer-State konnte nicht erstellt werden",
                             "The rasteriser state could not be created");
    return false;
  }

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(CAP_HR(dev->CreateBlendState(&bd, &blendOpaque_)))) {
    if (error) *error = T("Blend-State konnte nicht erstellt werden",
                             "The blend state could not be created");
    return false;
  }
  return true;
}

// ------------------------------------------------------------- source format

void VideoRenderer::ReleaseSourceTextures() {
  for (int i = 0; i < 3; ++i) {
    planeSrv_[i].Reset();
    plane_[i].Reset();
    for (int h = 0; h < kHistoryDepth; ++h) {
      planeHistSrv_[h][i].Reset();
      planeHist_[h][i].Reset();
    }
  }
  planeCount_ = 0;
  historyWrite_ = 0;
  historyCount_ = 0;
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

  // New format means a new measurement. The verdicts themselves should not
  // change -- the source decides its levels and its field structure, not the
  // pixel format we asked for -- but the evidence has to be gathered from the
  // new byte layout.
  ResetAnalysis();

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
  } else if (label == "P010" || label == "P016") {
    kind_ = FormatKind::P010;
    // P010 parks ten bits at the top of each sixteen, so a full scale sample
    // reads back as 65472/65535 rather than 1. P016 uses all sixteen.
    tenBitContainer_ = (label == "P010");
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

    // The history copies. Written by the GPU rather than the CPU, so they are
    // plain default resources; they are allocated whether or not anything reads
    // them, because switching a filter on mid-stream should not have to
    // reallocate anything. Three copies of a 480 line frame is under two
    // megabytes.
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    for (int h = 0; h < kHistoryDepth; ++h) {
      if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &planeHist_[h][index])))) return false;
      if (FAILED(CAP_HR(dev->CreateShaderResourceView(planeHist_[h][index].Get(), nullptr,
                                                      &planeHistSrv_[h][index])))) {
        return false;
      }
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
      break;

    case FormatKind::P010:
      ok = makePlane(0, w, h, DXGI_FORMAT_R16_UNORM) &&
           makePlane(1, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R16G16_UNORM);
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
    if (error) *error = T("Videotexturen konnten nicht angelegt werden",
                             "The video textures could not be created");
    return false;
  }
  CAP_LOG("Quelltexturen angelegt: %s %dx%d (%d Ebenen)", source_.subtypeLabel.c_str(), w, h,
          planeCount_);
  return true;
}

bool VideoRenderer::EnsureClean(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (cleanTex_ && cleanWidth_ == width && cleanHeight_ == height) return true;

  cleanSrv_.Reset();
  cleanRtv_.Reset();
  cleanTex_.Reset();
  cleanPrevSrv_.Reset();
  cleanPrevTex_.Reset();
  cleanWidth_ = 0;
  cleanHeight_ = 0;

  ID3D11Device* dev = ctx_->device();
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(cleanTex_.Get(), nullptr, &cleanSrv_)))) {
    return false;
  }
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(cleanTex_.Get(), nullptr, &cleanRtv_)))) {
    return false;
  }

  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &cleanPrevTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(cleanPrevTex_.Get(), nullptr,
                                                  &cleanPrevSrv_)))) {
    return false;
  }

  cleanWidth_ = width;
  cleanHeight_ = height;
  return true;
}

bool VideoRenderer::EnsureIntermediate(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  // Eight bits cannot hold linear light: an HDR highlight is a value above one,
  // and this is the buffer it would be thrown away in -- before the tone mapping
  // at the end of the pipeline ever got to look at it. So the picture between
  // the passes is half float whenever the source is HDR, and stays eight bit
  // otherwise, where it costs nothing and is all that is needed.
  const DXGI_FORMAT format = hdrTransfer_ == Transfer::Sdr ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                           : DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (intermediate_ && intermediateWidth_ == width && intermediateHeight_ == height &&
      intermediateFormat_ == format) {
    return true;
  }

  intermediateRtv_.Reset();
  intermediateSrv_.Reset();
  intermediate_.Reset();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
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
  intermediateFormat_ = format;
  return true;
}

// The picture as an ordinary screen would see it, whatever the screen is
// actually doing. Recording, screenshots and the virtual camera are all eight
// bit and all want the same thing: the tone mapped picture. Reading back the
// buffer between the passes would hand them linear light, and reading back what
// went to the display would hand them scRGB when the display is HDR -- neither
// is a picture anything else can use.
//
// It reuses the scaling shader at one to one with nearest sampling, because
// that shader already knows how to turn linear light into an ordinary picture
// and there is no reason to have two copies of that curve.
bool VideoRenderer::GrabStillHalf(std::vector<uint16_t>* out, int* width, int* height,
                                  int* strideBytes) {
  if (!intermediate_ || !ctx_ || hdrTransfer_ == Transfer::Sdr) return false;
  if (intermediateFormat_ != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;

  // Square pixels here too, and in linear light so nothing about the range is
  // decided on the way. A still is allowed the extra pass; it happens when
  // somebody presses a key, not sixty times a second.
  ID3D11Texture2D* from = intermediate_.Get();
  int fromW = intermediateWidth_;
  int fromH = intermediateHeight_;
  if (deliveryHalfNeeded()) {
    if (!RenderDelivery(true)) return false;
    from = deliveryHalf_.Get();
    fromW = deliveryWidth_;
    fromH = deliveryHeight_;
  }

  D3D11_TEXTURE2D_DESC td = {};
  from->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(ctx_->device()->CreateTexture2D(&td, nullptr, &staging)))) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  dc->CopyResource(staging.Get(), from);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) return false;

  out->resize((size_t)mapped.RowPitch / sizeof(uint16_t) * fromH);
  memcpy(out->data(), mapped.pData, out->size() * sizeof(uint16_t));
  dc->Unmap(staging.Get(), 0);

  *width = fromW;
  *height = fromH;
  *strideBytes = (int)mapped.RowPitch;
  return true;
}

bool VideoRenderer::EnsureHdrRecord(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (hdrRecTex_ && hdrRecWidth_ == width && hdrRecHeight_ == height) return true;

  hdrRecRtv_.Reset();
  hdrRecTex_.Reset();
  for (int i = 0; i < kReadbackSlots; ++i) hdrReadbackTex_[i].Reset();
  hdrReadWrite_ = 0;
  hdrReadQueued_ = 0;
  hdrReadMapped_ = -1;

  ID3D11Device* dev = ctx_->device();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &hdrRecTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(hdrRecTex_.Get(), nullptr, &hdrRecRtv_)))) {
    return false;
  }

  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int i = 0; i < kReadbackSlots; ++i) {
    if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &hdrReadbackTex_[i])))) return false;
  }

  hdrRecWidth_ = width;
  hdrRecHeight_ = height;
  return true;
}

void VideoRenderer::QueueHdrReadback() {
  if (!hdrWideActive() || !intermediate_ || !ctx_) return;
  if (!EnsureHdrRecord(deliveryWidth_, deliveryHeight_)) return;
  if (hdrReadWrite_ == hdrReadMapped_) return;

  // Resample first, in linear light, and only then lay on the PQ curve. The
  // other way round would interpolate between PQ coded values, which are not
  // proportional to anything, and every soft edge would end up at the wrong
  // brightness.
  ID3D11ShaderResourceView* wideSource = intermediateSrv_.Get();
  if (deliveryHalfNeeded()) {
    if (!RenderDelivery(true)) return;
    wideSource = deliveryHalfSrv_.Get();
  }

  ID3D11DeviceContext* dc = ctx_->context();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbRecord_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    const float value[4] = {paperWhiteNits_, 0.0f, 0.0f, 0.0f};
    memcpy(mapped.pData, value, sizeof(value));
    dc->Unmap(cbRecord_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {hdrRecRtv_.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)hdrRecWidth_;
  vp.Height = (float)hdrRecHeight_;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psHdrRecord_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {wideSource};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {sampPoint_.Get()};
  dc->PSSetSamplers(0, 1, samp);
  ID3D11Buffer* cbs[] = {cbRecord_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);

  dc->CopyResource(hdrReadbackTex_[hdrReadWrite_].Get(), hdrRecTex_.Get());
  hdrReadWrite_ = (hdrReadWrite_ + 1) % kReadbackSlots;
  if (hdrReadQueued_ < kReadbackSlots) ++hdrReadQueued_;
}

bool VideoRenderer::FetchHdrReadback(ReadbackFrame* out) {
  if (!hdrWideActive() || !ctx_ || hdrReadQueued_ < kReadbackSlots) return false;
  if (hdrReadMapped_ >= 0) return false;

  const int slot = hdrReadWrite_;  // oldest: two copies are queued behind it
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(ctx_->context()->Map(hdrReadbackTex_[slot].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  hdrReadMapped_ = slot;
  out->data = static_cast<const uint8_t*>(mapped.pData);
  out->stride = (int)mapped.RowPitch;
  out->width = hdrRecWidth_;
  out->height = hdrRecHeight_;
  out->size = (size_t)mapped.RowPitch * hdrRecHeight_;
  return true;
}

void VideoRenderer::ReleaseHdrReadback() {
  if (hdrReadMapped_ < 0) return;
  ctx_->context()->Unmap(hdrReadbackTex_[hdrReadMapped_].Get(), 0);
  hdrReadMapped_ = -1;
}

bool VideoRenderer::EnsureDelivery(int width, int height, bool half) {
  width = std::max(1, width);
  height = std::max(1, height);

  ComPtr<ID3D11Texture2D>& tex = half ? deliveryHalf_ : delivery_;
  ComPtr<ID3D11RenderTargetView>& rtv = half ? deliveryHalfRtv_ : deliveryRtv_;
  int& haveW = half ? deliveryHalfTexWidth_ : deliveryTexWidth_;
  int& haveH = half ? deliveryHalfTexHeight_ : deliveryTexHeight_;
  if (tex && haveW == width && haveH == height) return true;

  if (half) deliveryHalfSrv_.Reset();
  rtv.Reset();
  tex.Reset();
  haveW = 0;
  haveH = 0;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  // The half float one is read again afterwards, by the shader that lays the PQ
  // curve over it. The eight bit one is only ever copied out.
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (half) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

  ID3D11Device* dev = ctx_->device();
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &tex)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))) {
    tex.Reset();
    return false;
  }
  if (half &&
      FAILED(CAP_HR(dev->CreateShaderResourceView(tex.Get(), nullptr, &deliveryHalfSrv_)))) {
    rtv.Reset();
    tex.Reset();
    return false;
  }
  haveW = width;
  haveH = height;
  return true;
}

// The picture on its way out of the window's world. Two jobs in one pass,
// either of which can be the only reason it runs: resample to square pixels,
// and -- when the source is HDR and the destination is not -- lay the tone
// curve over it. It reuses the scaling shader because that shader already
// knows both, and there is no reason to keep a second copy of either.
bool VideoRenderer::RenderDelivery(bool half) {
  if (!intermediate_ || !ctx_) return false;
  if (deliveryWidth_ <= 0 || deliveryHeight_ <= 0) return false;
  if (!EnsureDelivery(deliveryWidth_, deliveryHeight_, half)) return false;

  ID3D11DeviceContext* dc = ctx_->context();

  ScaleCB sc = {};
  sc.srcSize[0] = (float)intermediateWidth_;
  sc.srcSize[1] = (float)intermediateHeight_;
  sc.dstSize[0] = (float)deliveryWidth_;
  sc.dstSize[1] = (float)deliveryHeight_;
  // At one to one nearest is exact, and it is the only filter that cannot
  // invent anything. When the size does change, the picture gets the same
  // filter the window would have used -- that is the whole point of the
  // setting reaching this far.
  sc.filter = deliveryResize() ? (int32_t)deliveryFilter_ : 0;
  // All of these belong to the display rather than to the picture. Scanlines
  // and the mask especially: they are drawn for a particular size on a
  // particular screen, and baking them into a file would put the gaps in the
  // wrong places for whoever plays it back.
  sc.sharpen = 0.0f;
  sc.scanlines = 0.0f;
  sc.mask = 0;
  sc.maskStrength = 0.0f;
  sc.nativeWidth = 0;
  sc.linePitch = 0.0f;
  sc.transfer = (int32_t)hdrTransfer_;
  sc.outputHdr = 0;   // this copy is for things that are not a screen
  sc.paperWhite = paperWhiteNits_;
  sc.sourcePeak = sourcePeakNits_;
  sc.displayPeak = 100.0f;  // an ordinary screen, by definition of what this is for
  // The half float target is the one case here that is not headed for an
  // ordinary screen: it carries linear light on to either a PQ recording or a
  // wide screenshot, and tone mapping it down to a hundred nits first would
  // throw away precisely what those exist for. It only ever resamples.
  //
  // The eight bit target skips the same block whenever there is no HDR to map:
  // an SDR picture is already display encoded, the transfer step would be a
  // pair of inverse curves cancelling out, and the clamp is the only thing left
  // in there -- which is exactly what the resample must not need.
  sc.passthrough = (half || hdrTransfer_ == Transfer::Sdr) ? 1 : 0;

  // The one setting in this pass the user chose rather than the pass deciding
  // for itself. Off, the picture leaves exactly as the window's own scaling
  // found it -- which is the default, because a recording that clipped its
  // highlights cannot be graded back and a clean one can always be graded
  // forward.
  sc.procAmp = (deliveryProcAmp_ && procAmpActive()) ? 1 : 0;
  sc.brightness = deliveryBrightness_;
  sc.contrast = deliveryContrast_;
  sc.saturation = deliverySaturation_;
  sc.hue = deliveryHue_;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  ID3D11RenderTargetView* rtv[] = {half ? deliveryHalfRtv_.Get() : deliveryRtv_.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)deliveryWidth_;
  vp.Height = (float)deliveryHeight_;
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psScale_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {intermediateSrv_.Get()};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samplers[] = {sampPoint_.Get(), sampLinear_.Get()};
  dc->PSSetSamplers(0, 2, samplers);
  ID3D11Buffer* cbs[] = {cbScale_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  dc->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);
  return true;
}

// ----------------------------------------------------------------- uploading

// How often a frame is looked at, and how many frames of evidence are wanted
// before the verdict is frozen. Every third frame for 40 frames is about two
// seconds at 60 Hz -- long enough to see some dark content, short enough that
// the picture has settled before anyone reaches for the settings.
static const int kRangeSampleEvery = 3;
static const int kRangeFramesWanted = 40;
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

  // Two ways the sample can be worthless. Without any dark pixels there is
  // nothing to tell the two apart: limited range piles its blacks up at exactly
  // 16, full range goes below it. And a picture with no contrast at all -- a
  // console asleep, a card between signals -- is entirely black, which reads as
  // 100 % below 16 and would otherwise be written down as a confident verdict of
  // full range on no evidence at all.
  //
  // In both cases the evidence is discarded and the measurement starts over, so
  // that whatever appears later is judged on its own. Until something decides,
  // Draw falls back to the rule of thumb for the pixel format.
  if (rangeMin_ > 40 || (rangeMax_ - rangeMin_) < 64) {
    rangeFramesSeen_ = 0;
    rangeSamples_ = 0;
    rangeBelow16_ = 0;
    rangeAbove235_ = 0;
    rangeMin_ = 255;
    rangeMax_ = 0;
    return;
  }

  const double below = (double)rangeBelow16_ / (double)rangeSamples_;
  // The black end decides. Values above 235 are not proof of anything: limited
  // range signals are allowed to carry superwhites, and plenty of sources do.
  rangeVerdict_ = below > 0.002 ? RangeVerdict::Full : RangeVerdict::Limited;
  CAP_LOG("Wertebereich erkannt: %s (min %d, max %d, %.3f %% unter 16, %llu Proben)",
          rangeVerdict_ == RangeVerdict::Full ? "voll 0-255" : "begrenzt 16-235", rangeMin_,
          rangeMax_, below * 100.0, (unsigned long long)rangeSamples_);
}

// Two questions, asked of the same samples.
//
// First: do the two rows of a pair hold the same picture line? A 240p or 288p
// console packed into a 480 or 576 line frame arrives that way -- the card takes
// two consecutive progressive pictures and interleaves them, so row 2k and row
// 2k+1 are the same line of the picture at two different moments. On everything
// that is standing still they are identical, which is what this measures, and on
// anything that moves they comb, which is why such a source still needs
// deinterlacing. What it does not need is the half line offset a real interlaced
// signal has: applying that is what makes bob step the whole picture down a line
// on every second field.
//
// The test needs nothing to be moving, which is what makes it worth doing first.
//
// Second, only if the answer to the first is no: is there combing? A woven frame
// of a moving picture has lines that sit outside the range their neighbours
// span. That does need motion, which is why the verdict is never frozen on
// "progressive" -- see the header.
static const int kCombSampleEvery = 3;
// Sixty frames of source: a second at 60 Hz, two and a half at 25.
static const int kCombFramesWanted = 20;
// Fraction of a frame's samples that have to comb for that frame to count.
// Genuine interlacing scores in the high single digits to well over ten per
// cent; a still progressive picture scores around a tenth of one. The gap is
// wide, so the threshold sits well clear of both.
static const double kCombThreshold = 0.030;
// And this many frames in the window have to reach it. One is a noise spike;
// two is something that was moving.
static const int kCombFramesNeeded = 2;
// Co-sited fields: the rows within a pair have to differ by at least this factor
// less than neighbouring pairs do, and there has to be enough vertical detail
// for the comparison to mean anything in the first place.
// Measured on a composite SNES: the rows within a pair differ by 1 to 5, the
// pairs themselves by around 20. Genuinely interlaced and genuinely progressive
// full resolution pictures both sit near 1, because in either case the two
// comparisons are looking at the same kind of thing.
static const double kDoubleRatio = 3.0;
static const double kDoubleFloor = 4.0;

void VideoRenderer::ResetAnalysis() {
  rangeVerdict_ = RangeVerdict::Pending;
  rangeFramesSeen_ = 0;
  rangeSamples_ = 0;
  rangeBelow16_ = 0;
  rangeAbove235_ = 0;
  rangeMin_ = 255;
  rangeMax_ = 0;

  interlaceVerdict_ = InterlaceVerdict::Pending;
  coSitedFields_ = false;
  coSitedPhase_ = 0;
  combFramesSeen_ = 0;
  combSamples_ = 0;
  combHits_ = 0;
  combFrameHits_ = 0;
  combFramesAnalysed_ = 0;
  pairInner_ = 0;
  pairOuter_ = 0;

  boundsValid_ = false;
  accAny_ = false;
  boundsFramesSeen_ = 0;

  signalVerdict_ = SignalVerdict::Unknown;
  signalFramesSeen_ = 0;
  signalSinceTick_ = 0;
  signalPrev_.clear();

  ResetChroma();
}

bool VideoRenderer::ChromaLayout(ChromaPlanes* planes) const {
  // Alle YUV-Formate, die hier ankommen koennen. Hier stand einmal nur das
  // gepackte 4:2:2 mit der Begruendung, ein Analogdecoder liefere die planaren
  // nicht -- das stimmt fuer diese Karte, ist aber die falsche Frage: die
  // Farbmessung haengt am Format des Bildes, nicht an der Herkunft, und eine
  // Karte, die NV12 liefert, hat sonst gar keine.
  //
  // MJPG fehlt nicht: es kommt hier nie an. Der Graph haengt dafuer einen
  // Decoder davor (siehe video_capture.cpp), und was hier eintrifft, ist
  // dessen Ausgabe -- also eines der Formate unten.
  const size_t w = (size_t)source_.width;
  const size_t h = (size_t)source_.height;
  if (w < 2 || h < 2) return false;

  ChromaPlanes r;
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Yvyu:
    case FormatKind::Uyvy: {
      // Gepacktes 4:2:2. Vier Bytes decken zwei Bildpunkte ab, die beiden
      // Farbdifferenzen liegen zwischen deren Luma. Nur waagerecht
      // unterabgetastet -- jede Bildzeile hat ihre eigene Farbe.
      if (kind_ == FormatKind::Yuy2) {         // Y U Y V
        r.yOff = 0; r.uOff = 1; r.vOff = 3;
      } else if (kind_ == FormatKind::Yvyu) {  // Y V Y U
        r.yOff = 0; r.uOff = 3; r.vOff = 1;
      } else {                                 // U Y V Y
        r.yOff = 1; r.uOff = 0; r.vOff = 2;
      }
      r.yPitch = r.uPitch = r.vPitch = w * 2;
      r.yStep = 2;
      r.uStep = r.vStep = 4;
      r.cxShift = 1;
      r.cyShift = 0;
      r.needed = w * 2 * h;
      break;
    }
    case FormatKind::Nv12: {
      // Halbplanar 4:2:0: erst die ganze Lumaebene, dahinter eine Ebene mit U
      // und V im Wechsel und halber Hoehe. Eine Farbzeile gilt fuer zwei
      // Bildzeilen, daher cyShift.
      r.yOff = 0;
      r.yPitch = w;
      r.yStep = 1;
      r.uOff = w * h;
      r.vOff = w * h + 1;
      r.uPitch = r.vPitch = w;
      r.uStep = r.vStep = 2;
      r.cxShift = r.cyShift = 1;
      r.needed = w * h + w * (h / 2);
      break;
    }
    case FormatKind::Planar420: {
      // Voll planar 4:2:0: drei getrennte Ebenen, die beiden Farbebenen je ein
      // Viertel so gross. YV12 legt V vor U, I420 und IYUV umgekehrt -- das
      // steht schon in planarUvSwapped_.
      const size_t cw = w / 2, ch = h / 2;
      const size_t first = w * h;
      const size_t second = first + cw * ch;
      r.yOff = 0;
      r.yPitch = w;
      r.yStep = 1;
      r.uOff = planarUvSwapped_ ? second : first;
      r.vOff = planarUvSwapped_ ? first : second;
      r.uPitch = r.vPitch = cw;
      r.uStep = r.vStep = 1;
      r.cxShift = r.cyShift = 1;
      r.needed = second + cw * ch;
      break;
    }
    case FormatKind::P010: {
      // Wie NV12, nur sechzehn Bit je Probe. Gelesen wird das obere Byte, und
      // das ist hier kein Verlust an der richtigen Stelle: P010 parkt die zehn
      // Bits oben, das obere Byte sind also genau deren obere acht, und die
      // Farbmitte bleibt die Farbmitte (512 -> 0x8000 -> 128). Gemessen wird
      // ohnehin ein Blockmittel, die fehlenden zwei Bit fallen darin nicht auf.
      r.yOff = 1;
      r.yPitch = w * 2;
      r.yStep = 2;
      r.uOff = w * h * 2 + 1;
      r.vOff = w * h * 2 + 3;
      r.uPitch = r.vPitch = w * 2;
      r.uStep = r.vStep = 4;
      r.cxShift = r.cyShift = 1;
      r.needed = w * h * 2 + w * (h / 2) * 2;
      break;
    }
    default:
      // RGB laeuft ueber einen eigenen Weg, siehe AnalyzeChroma -- und das ist
      // an dieser Karte nicht der Ausnahmefall, sondern der Regelfall.
      return false;
  }
  *planes = r;
  return true;
}

// Jedes achte Bild. Die Messung laeuft dauerhaft mit, also muss sie billig
// sein; gebraucht wird kein genauer Wert, sondern die Unterscheidung "farbig"
// von "grau".
static const int kChromaSampleEvery = 8;

// Gemessen wird nicht die Farbe eines Pixels, sondern die eines Bloecks --
// und das ist der ganze Trick.
//
// Der naheliegende Weg, den mittleren Abstand von der Farbmitte zu nehmen,
// funktioniert naemlich nicht, und zwar genau in dem Fall, fuer den die Messung
// da ist. Steht der falsche Farbtraeger, ist das Bild grau mit Regenbogengries
// darin, und dieser Gries ist pixelweise kraeftig bunt: am 29.08. an einem
// GameCube gemessen, der als NTSC M (Japan) statt PAL 60 dekodiert wurde, kam
// pixelweise 0,132 heraus -- ein Wert, den ein farbiges Bild kaum uebertrifft.
// Der Gries ist die Schwebung zwischen 4,43 und 3,58 MHz, und die ist bunt.
//
// Was ihn von Farbe trennt, ist nicht die Staerke, sondern der Zusammenhalt.
// Ein rotes Kart bleibt rot, wenn man darueber mittelt; die Schwebung
// durchlaeuft in sechzehn Bildpunkten einen ganzen Farbkreis und mittelt sich
// zu grau weg. Sechzehn, weil die Schwebung von 0,85 MHz bei 13,5 MHz Abtastung
// eine Periode von knapp sechzehn Proben hat -- ein schmalerer Block erwischte
// nur einen Teil davon und liesse einen Rest stehen.
static const int kChromaBlockW = 16;
static const int kChromaBlockH = 4;
// Nicht jeder Block, sondern ein Raster daraus. Fuer 720x480 sind das gut
// fuenfhundert Bloecke, und mehr sagt ueber "ist da Farbe" nichts Neues.
static const int kChromaBlockStepX = 32;
static const int kChromaBlockStepY = 16;

// Ab wann ein Block als dunkel zaehlt, auf der Luma-Skala 0..255.
//
// Schwarz liegt im begrenzten Bereich bei 16, und darueber muss Platz bleiben:
// gemeint sind nicht nur die schwarzen Bloecke, sondern das dunkle Ende
// ueberhaupt -- Schatten, Nachthimmel, der Rahmen um ein Menue. Mitten darf
// nicht hinein, denn dort *gehoert* Farbe hin.
static const int kChromaDarkLuma = 56;

// Wie viele dunkle Bloecke es mindestens braucht, damit ihr Mittel etwas
// aussagt. Ein Bild, das durchweg hell ist, hat schlicht kein dunkles Ende,
// und dann muss die Pruefung schweigen statt zu raten.
static const uint64_t kChromaDarkBlocksWanted = 200;
// Genug, dass ein einzelnes graues Titelbild die Auskunft nicht traegt. Bei
// jedem achten Bild und 30 Bildern je Sekunde sind das knapp drei Sekunden.
static const int kChromaFramesWanted = 10;

void VideoRenderer::AnalyzeChroma(const FrameView& frame) {
  if (++chromaFramesSeen_ % kChromaSampleEvery != 0) return;

  const int w = source_.width;
  const int h = source_.height;
  if (w < 16 || h < 16) return;

  // Die oberen und unteren Zehntel bleiben aussen vor. Dort steht bei einer
  // analogen Quelle der Rand: Austastluecke, Kopfrauschen eines Bandes, der
  // schwarze Balken eines Letterbox-Bildes. Alles davon ist farblos, und alles
  // davon wuerde den Mittelwert genau in die Richtung ziehen, in die er nicht
  // gezogen werden darf -- gemessen wird ja, ob das Bild grau ist.
  const int y0 = h / 10;
  const int y1 = h - h / 10;
  uint64_t sum = 0;
  uint64_t count = 0;

  uint64_t darkSum = 0;
  uint64_t darkCount = 0;

  ChromaPlanes cp;
  size_t step = 4;
  if (ChromaLayout(&cp)) {
    if (cp.needed > frame.size) return;
    for (int by = y0; by + kChromaBlockH <= y1; by += kChromaBlockStepY) {
      for (int bx = 0; bx + kChromaBlockW <= w; bx += kChromaBlockStepX) {
        int su = 0, sv = 0, sy = 0, n = 0;
        for (int y = by; y < by + kChromaBlockH; y += 2) {
          const uint8_t* yRow = frame.data + cp.yOff + (size_t)y * cp.yPitch;
          const size_t cy = (size_t)(y >> cp.cyShift);
          const uint8_t* uRow = frame.data + cp.uOff + cy * cp.uPitch;
          const uint8_t* vRow = frame.data + cp.vOff + cy * cp.vPitch;
          for (int x = bx; x < bx + kChromaBlockW; x += 2) {
            const size_t cx = (size_t)(x >> cp.cxShift);
            su += (int)uRow[cx * cp.uStep];
            sv += (int)vRow[cx * cp.vStep];
            // Beide Bildpunkte, die sich diese Farbprobe teilen. Die
            // Helligkeit soll ueber demselben Stueck Bild stehen wie die
            // Farbe, sonst entscheidet an einer Kante der Zufall.
            sy += ((int)yRow[(size_t)x * cp.yStep] + (int)yRow[(size_t)(x + 1) * cp.yStep]) / 2;
            ++n;
          }
        }
        if (n == 0) continue;
        const int cu = su / n - 128;
        const int cv = sv / n - 128;
        // Auf 128 bezogen, den groessten Abstand von der Farbmitte, den ein
        // Byte hergibt; beide Differenzen zusammen landen damit auf derselben
        // Skala 0..255 wie der RGB-Weg unten.
        const uint64_t block = (uint64_t)((cu < 0 ? -cu : cu) + (cv < 0 ? -cv : cv));
        sum += block;
        count += 1;
        if (sy / n < kChromaDarkLuma) {
          darkSum += block;
          darkCount += 1;
        }
      }
    }
  } else if (kind_ == FormatKind::Rgb) {
    // Die Karte rechnet dann selbst nach RGB um, und danach heisst farblos
    // schlicht R = G = B. Das ist sogar der sauberere Weg: greift der
    // Farbkiller im Decoder, kommen die drei Kanaele exakt gleich heraus, und
    // der Abstand zwischen groesstem und kleinstem ist nicht ungefaehr null,
    // sondern null.
    //
    // Dieser Zweig ist nicht der Sonderfall. Die SA7160 liefert am
    // Composite-Eingang RGB32, und ohne ihn haette die Farbpruefung genau dort
    // nie eine Messung zustande gebracht, wo sie gebraucht wird.
    step = source_.subtypeLabel == "RGB24" ? 3 : 4;
    const size_t pitch = (size_t)w * step;
    if (pitch * (size_t)h > frame.size) return;
    for (int by = y0; by + kChromaBlockH <= y1; by += kChromaBlockStepY) {
      for (int bx = 0; bx + kChromaBlockW <= w; bx += kChromaBlockStepX) {
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (int y = by; y < by + kChromaBlockH; y += 2) {
          const uint8_t* row = frame.data + (size_t)y * pitch;
          for (int x = bx; x < bx + kChromaBlockW; x += 2) {
            const uint8_t* px = row + (size_t)x * step;
            sb += px[0];
            sg += px[1];
            sr += px[2];
            ++n;
          }
        }
        if (n == 0) continue;
        const int r = sr / n, g = sg / n, b = sb / n;
        const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        sum += (uint64_t)(hi - lo);
        count += 1;
        // Luma zurueckgerechnet statt einfach die Helligkeit genommen. Die
        // Karte hat mit BT.601 nach RGB gerechnet, dieselben Gewichte holen Y
        // exakt wieder heraus -- und zwar das Y des Signals, nicht das des
        // Farbfehlers. Rotes Schwarz (R 60, G 10, B 10) ergibt so 25 und
        // bleibt dunkel; als Mittelwert der drei Kanaele waeren es 27, als
        // groesster Kanal schon 60, und damit fiele der Block genau dann aus
        // der Messung, wenn er gebraucht wird.
        const int luma = (299 * r + 587 * g + 114 * b) / 1000;
        if (luma < kChromaDarkLuma) {
          darkSum += (uint64_t)(hi - lo);
          darkCount += 1;
        }
      }
    }
  } else {
    return;
  }
  if (count == 0) return;

  chromaSum_ += sum;
  chromaCount_ += count;
  chromaDarkSum_ += darkSum;
  chromaDarkCount_ += darkCount;
  ++chromaFramesAnalysed_;
}

float VideoRenderer::chromaEnergy() const {
  if (chromaFramesAnalysed_ < kChromaFramesWanted || chromaCount_ == 0) return -1.0f;
  // Beide Wege liefern einen Wert im Bereich 0..255 je Block, siehe
  // AnalyzeChroma. Die Zahlen sind nicht auf denselben Farbton geeicht -- der
  // RGB-Weg faellt bei gleichem Bild etwas hoeher aus --, aber darum geht es
  // nicht: unterschieden wird farbig von grau, und grau ist auf beiden Wegen
  // dieselbe Null.
  return (float)((double)chromaSum_ / (double)chromaCount_ / 255.0);
}

float VideoRenderer::darkChromaEnergy() const {
  if (chromaFramesAnalysed_ < kChromaFramesWanted) return -1.0f;
  // Eigene Untergrenze: die Gesamtmessung hat immer Bloecke, diese hier nicht
  // unbedingt. Ein helles Standbild ohne dunkle Stelle darf keine Aussage
  // erfinden, es muss "weiss nicht" sagen duerfen.
  if (chromaDarkCount_ < kChromaDarkBlocksWanted) return -1.0f;
  return (float)((double)chromaDarkSum_ / (double)chromaDarkCount_ / 255.0);
}

void VideoRenderer::ResetChroma() {
  chromaFramesSeen_ = 0;
  chromaFramesAnalysed_ = 0;
  chromaSum_ = 0;
  chromaCount_ = 0;
  chromaDarkSum_ = 0;
  chromaDarkCount_ = 0;
}

bool VideoRenderer::LumaLayout(size_t* offset, size_t* step) const {
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Yvyu: *offset = 0; *step = 2; return true;
    case FormatKind::Uyvy: *offset = 1; *step = 2; return true;
    case FormatKind::Nv12:
    case FormatKind::Planar420: *offset = 0; *step = 1; return true;
    // Das obere Byte der sechzehn. P010 parkt die zehn Bits oben, das obere
    // Byte sind also deren obere acht -- dieselbe Skala wie bei allen anderen,
    // weshalb auch die 16/235-Grenzen der Pegelerkennung weiter stimmen. Ohne
    // diese Zeile war P010 das einzige Format, in dem gar nichts gemessen wird.
    case FormatKind::P010: *offset = 1; *step = 2; return true;
    case FormatKind::Rgb:
      // Green stands in for luma. It carries most of it, and it costs one read
      // instead of three.
      *offset = 1;
      *step = source_.subtypeLabel == "RGB24" ? 3 : 4;
      return true;
    default: return false;
  }
}

// Anything above this is picture rather than border. Analogue black does not
// arrive at zero and it is not quiet, so the bar sits a little above where the
// blacker-than-black of a limited range signal would be.
static const int kContentLuma = 24;
// Every ninth frame, not every third, and every second column rather than all of
// them. This is the only one of the three analyses that never finishes -- the
// level and interlace verdicts latch and stop -- so it is the only one still
// costing anything once the picture has settled, and it was costing enough to
// matter: a full width scan of every third frame lands ten times a second, and
// measured against the deinterlacer, ten per cent of its field switches were
// arriving more than ten milliseconds late. That is a stutter you can see.
//
// The border is a fixed property of the signal and in no hurry, so the same
// answer arrives just as reliably from a sixth of the work.
static const int kBoundsSampleEvery = 9;
static const int kBoundsFramesWanted = 5;
static const int kBoundsColumnStep = 2;

void VideoRenderer::AnalyzeContentBounds(const FrameView& frame) {
  if (++boundsFramesSeen_ % kBoundsSampleEvery != 0) return;

  const int w = source_.width;
  const int h = source_.height;
  if (w < 16 || h < 16) return;

  size_t offset = 0, step = 1;
  if (!LumaLayout(&offset, &step)) return;
  const size_t pitch = (size_t)w * step;
  if (pitch * (size_t)h > frame.size) return;

  if ((int)columnHits_.size() != w) columnHits_.assign((size_t)w, 0);
  std::fill(columnHits_.begin(), columnHits_.end(), 0);

  // A row counts as picture only when a decent stretch of it is above black. One
  // bright speck of analogue noise in the letterbox must not widen the crop.
  const int minRun = w / (50 * kBoundsColumnStep) > 2 ? w / (50 * kBoundsColumnStep) : 2;
  int top = -1, bottom = -1, litRows = 0;
  for (int y = 0; y < h; y += 2) {
    const uint8_t* row = frame.data + offset + (size_t)y * pitch;
    int lit = 0;
    for (int x = 0; x < w; x += kBoundsColumnStep) {
      if (row[(size_t)x * step] > kContentLuma) {
        ++lit;
        ++columnHits_[(size_t)x];
      }
    }
    if (lit >= minRun) {
      if (top < 0) top = y;
      bottom = y;
      ++litRows;
    }
  }
  if (litRows == 0) return;  // an entirely black frame says nothing

  // Same idea the other way round: a column has to be lit in a fair number of
  // the rows that carry picture at all.
  const int minCol = litRows / 20 > 1 ? litRows / 20 : 1;
  int left = -1, right = -1;
  for (int x = 0; x < w; ++x) {
    if (columnHits_[(size_t)x] >= minCol) {
      if (left < 0) left = x;
      right = x;
    }
  }
  if (left < 0) return;

  // Stored top-down. The bottom-up layouts are read in buffer order, so the two
  // vertical edges swap on the way out.
  if (source_.bottomUp) {
    const int t = h - 1 - bottom;
    bottom = h - 1 - top;
    top = t;
  }
  // Both scans step, so each far edge can fall one step short.
  if (right + kBoundsColumnStep <= w - 1) right += kBoundsColumnStep - 1;
  // The row scan steps by two, so the bottom edge can be one line short.
  if (bottom + 1 < h) ++bottom;

  if (!accAny_) {
    accL_ = left; accT_ = top; accR_ = right; accB_ = bottom;
    accAny_ = true;
  } else {
    if (left < accL_) accL_ = left;
    if (top < accT_) accT_ = top;
    if (right > accR_) accR_ = right;
    if (bottom > accB_) accB_ = bottom;
  }

  if (boundsFramesSeen_ / kBoundsSampleEvery < kBoundsFramesWanted) return;

  // Logged when it moves, not every window: the border is a fixed property of
  // the signal, so a line that keeps reappearing means something is wrong.
  const bool changed = !boundsValid_ || accL_ != boundsL_ || accT_ != boundsT_ ||
                       accR_ != boundsR_ || accB_ != boundsB_;
  boundsL_ = accL_; boundsT_ = accT_; boundsR_ = accR_; boundsB_ = accB_;
  boundsValid_ = true;
  boundsFramesSeen_ = 0;
  accAny_ = false;
  if (changed) {
    CAP_LOG("Bildinhalt gemessen: x %d..%d, y %d..%d (Rand links %d, rechts %d, oben %d, unten %d)",
            boundsL_, boundsR_, boundsT_, boundsB_, boundsL_, source_.width - 1 - boundsR_,
            boundsT_, source_.height - 1 - boundsB_);
  }
}

// ---------------------------------------------------------------- signal
//
// A sparse grid of luma samples, compared against itself and against the same
// grid one measurement ago. Two numbers come out: how much contrast the frame
// holds, and how much it changed.
//
// The grid is deliberately coarse and spread with a prime stride, for the same
// reason the level detector uses one -- a stride that divides the row length
// reads a single column and calls it the picture.
static const int kSignalSampleEvery = 6;
static const int kSignalSamples = 2048;
// Below this the frame has no contrast worth the name. Sixteen levels out of
// 255, which is wide enough to cover the noise an analogue decoder puts on a
// muted output and narrow enough that a dim night-time scene still clears it.
static const int kSignalFlatSpan = 16;
// Snow: mean absolute change per sample between two measurements. Two
// independent uniform samples average 85 apart, which is what an open input
// produces; the question is only how close real content can get to that.
//
// A synthetic estimate said 17 and was far too generous. Measured against live
// 1080p60 with a busy animated background, real motion reaches 52 and sits in
// the high forties for seconds at a time -- so the first value tried here, 48,
// had no margin at all and the verdict flipped several times a second.
//
// Sixty-five, because the two errors are not equally bad. Too low and a moving
// picture is declared dead and covered by the idle screen, which is the one
// outcome nobody can work around. Too high and snow goes unrecognised -- and
// then you simply see the snow, which tells you exactly the same thing the idle
// screen would have.
static const int kSignalSnowDelta = 65;
// And it has to be contrasty as well, so a hard cut between two flat colours
// cannot be mistaken for it.
static const int kSignalSnowSpan = 96;

void VideoRenderer::AnalyzeSignal(const FrameView& frame) {
  if (++signalFramesSeen_ % kSignalSampleEvery != 0) return;

  size_t offset = 0, step = 1;
  if (!LumaLayout(&offset, &step)) return;
  const size_t span = (size_t)source_.width * step * (size_t)source_.height;
  if (span > frame.size || span <= offset + step) return;

  const size_t count = (span - offset) / step;
  size_t stride = count / (size_t)kSignalSamples;
  if (stride < 1) stride = 1;
  if (stride % 2 == 0) ++stride;

  std::vector<uint8_t> now;
  now.reserve((size_t)kSignalSamples);
  int lo = 255, hi = 0;
  for (size_t i = 0; i < count && now.size() < (size_t)kSignalSamples; i += stride) {
    const uint8_t v = frame.data[offset + i * step];
    now.push_back(v);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (now.empty()) return;

  // Only comparable against a grid of the same shape. A format change resizes
  // it, and then this measurement simply starts over.
  int delta = -1;
  if (signalPrev_.size() == now.size()) {
    uint64_t sum = 0;
    for (size_t i = 0; i < now.size(); ++i) {
      const int d = (int)now[i] - (int)signalPrev_[i];
      sum += (uint64_t)(d < 0 ? -d : d);
    }
    delta = (int)(sum / now.size());
  }
  signalPrev_.swap(now);
  if (delta < 0) return;  // nothing to compare against yet

  const int spread = hi - lo;
  SignalVerdict verdict;
  if (spread < kSignalFlatSpan) {
    verdict = SignalVerdict::Flat;
  } else if (delta >= kSignalSnowDelta && spread >= kSignalSnowSpan) {
    verdict = SignalVerdict::Snow;
  } else {
    verdict = SignalVerdict::Picture;
  }

  if (verdict != signalVerdict_) {
    signalVerdict_ = verdict;
    signalSinceTick_ = ::GetTickCount();
    if (signalSinceTick_ == 0) signalSinceTick_ = 1;  // 0 means "never measured"
    CAP_LOG("Signal: %s (Spanne %d, Aenderung %d)",
            verdict == SignalVerdict::Picture ? "Bild"
            : verdict == SignalVerdict::Snow  ? "Rauschen"
                                              : "flach",
            spread, delta);
  }
}

double VideoRenderer::signalHeldSeconds() const {
  if (signalSinceTick_ == 0) return 0.0;
  // Unsigned subtraction, so the wrap after seven weeks of uptime costs one
  // wrong reading rather than a negative age.
  const unsigned long elapsed = ::GetTickCount() - signalSinceTick_;
  return (double)elapsed / 1000.0;
}

bool VideoRenderer::contentBounds(int* left, int* top, int* right, int* bottom) const {
  if (!boundsValid_) return false;
  if (left) *left = boundsL_;
  if (top) *top = boundsT_;
  if (right) *right = boundsR_;
  if (bottom) *bottom = boundsB_;
  return true;
}

void VideoRenderer::AnalyzeInterlace(const FrameView& frame) {
  // Both of these are structural, not momentary: once seen there is no reason to
  // keep asking. Only a plain "progressive" stays open, because that one can be
  // nothing more than a picture that happened to be standing still.
  if (interlaceVerdict_ == InterlaceVerdict::Interlaced) return;
  if (++combFramesSeen_ % kCombSampleEvery != 0) return;

  const int w = source_.width;
  const int h = source_.height;
  if (w < 16 || h < 16) return;

  // Only the luma plane is looked at: chroma is subsampled vertically on half
  // these formats, which would blur the very thing being measured away.
  size_t offset = 0, step = 1;
  if (!LumaLayout(&offset, &step)) return;
  const size_t pitch = (size_t)w * step;
  if (pitch * (size_t)h > frame.size) return;

  // Every other row, so each sample straddles one line of each field, and a
  // column every few pixels -- 64 across the width is plenty and keeps this off
  // the profile entirely.
  const int stepX = w / 64 > 0 ? w / 64 : 1;
  uint64_t hits = 0;
  uint64_t samples = 0;
  uint64_t inner = 0;
  uint64_t outer = 0;
  // y is odd throughout, so rowA/rowM are the two rows of one pair and rowM/rowB
  // straddle the boundary to the next -- which is what makes both tests fall out
  // of the same three reads.
  for (int y = 1; y + 1 < h; y += 2) {
    const uint8_t* rowA = frame.data + offset + (size_t)(y - 1) * pitch;
    const uint8_t* rowM = rowA + pitch;
    const uint8_t* rowB = rowM + pitch;
    for (int x = 0; x < w; x += stepX) {
      const size_t at = (size_t)x * step;
      const int la = rowA[at];
      const int lm = rowM[at];
      const int lb = rowB[at];

      inner += (uint64_t)(la > lm ? la - lm : lm - la);
      outer += (uint64_t)(lm > lb ? lm - lb : lb - lm);

      // Positive exactly when the middle line lies outside the range the other
      // two span, and near zero on a smooth gradient. Vertical detail that is
      // genuinely in the picture raises the bar, so a finely striped but static
      // image is not read as combing.
      const int comb = (lm - la) * (lm - lb);
      const int detail = la > lb ? la - lb : lb - la;
      if (comb > 900 + detail * 12) ++hits;
      ++samples;
    }
  }
  if (samples == 0) return;
  // Judged per frame. Combing lives where something moved, so it is concentrated
  // in the few frames that had movement in them; spreading those hits across a
  // window full of still ones is how a real interlaced source gets called
  // progressive.
  if ((double)hits / (double)samples > kCombThreshold) ++combFrameHits_;
  ++combFramesAnalysed_;

  combHits_ += hits;
  combSamples_ += samples;
  pairInner_ += inner;
  pairOuter_ += outer;

  if (combFramesAnalysed_ < kCombFramesWanted || combSamples_ == 0) return;

  const double n = (double)combSamples_;
  const double dInner = (double)pairInner_ / n;
  const double dOuter = (double)pairOuter_ / n;
  // Either phase counts: which of the two rows of a pair comes first is a
  // property of where the capture happens to have started, not of the signal.
  const double lo = dInner < dOuter ? dInner : dOuter;
  const double hi = dInner < dOuter ? dOuter : dInner;

  if (hi >= kDoubleFloor && lo * kDoubleRatio < hi) {
    coSitedFields_ = true;
    // Measured in buffer order, used in picture order. Those differ for the
    // bottom-up layouts, but only by a mirror, and mirroring an even number of
    // rows maps a pair starting on an even row to a pair starting on an even
    // row. Capture heights are even, so the phase carries over unchanged.
    coSitedPhase_ = dInner < dOuter ? 0 : 1;
    interlaceVerdict_ = InterlaceVerdict::Interlaced;
    CAP_LOG("Interlacing erkannt: ja, Halbbilder deckungsgleich, 240p/288p-Quelle "
            "(%.2f gegen %.2f pro Zeilenpaar, Phase %d)",
            lo, hi, coSitedPhase_);
    return;
  }

  if (combFrameHits_ >= kCombFramesNeeded) {
    interlaceVerdict_ = InterlaceVerdict::Interlaced;
    CAP_LOG("Interlacing erkannt: ja (%d von %d Bildern mit Kammartefakten)", combFrameHits_,
            combFramesAnalysed_);
    return;
  }

  // Nothing found this time. Say so once, then start a fresh window and keep
  // watching: a picture that was standing still proves nothing, and the next
  // window is only a second or two away.
  if (interlaceVerdict_ == InterlaceVerdict::Pending) {
    interlaceVerdict_ = InterlaceVerdict::Progressive;
    CAP_LOG("Interlacing erkannt: nein (%d von %d Bildern mit Kammartefakten, "
            "%.2f/%.2f pro Zeilenpaar)",
            combFrameHits_, combFramesAnalysed_, lo, hi);
  }
  combFramesSeen_ = 0;
  combFrameHits_ = 0;
  combFramesAnalysed_ = 0;
  combSamples_ = 0;
  combHits_ = 0;
  pairInner_ = 0;
  pairOuter_ = 0;
}

bool VideoRenderer::UploadFrame(const FrameView& frame) {
  if (!frame.valid() || planeCount_ == 0) return false;

  AnalyzeLevels(frame);
  AnalyzeInterlace(frame);
  AnalyzeContentBounds(frame);
  AnalyzeSignal(frame);
  AnalyzeChroma(frame);

  // Before anything is written over: the frame currently in the planes moves
  // into the ring. One copy, whatever the depth. Skipped entirely unless
  // something that reads the history is on, so nobody pays for a filter they
  // have not switched on.
  if (historyWanted_ && hasFrame_) {
    ID3D11DeviceContext* dc = ctx_->context();
    // The cleaned picture currently in cleanTex_ belongs to the frame that is
    // about to be replaced, so this is the moment it becomes the previous one.
    if (cleanTex_ && cleanPrevTex_) dc->CopyResource(cleanPrevTex_.Get(), cleanTex_.Get());
    for (int i = 0; i < planeCount_; ++i) {
      if (plane_[i] && planeHist_[historyWrite_][i]) {
        dc->CopyResource(planeHist_[historyWrite_][i].Get(), plane_[i].Get());
      }
    }
    historyWrite_ = (historyWrite_ + 1) % kHistoryDepth;
    if (historyCount_ < kHistoryDepth) ++historyCount_;
  } else if (!historyWanted_) {
    historyWrite_ = 0;
    historyCount_ = 0;
  }

  bool ok = false;
  switch (kind_) {
    case FormatKind::Yuy2:
    case FormatKind::Uyvy:
    case FormatKind::Yvyu: ok = UploadPacked(frame); break;
    case FormatKind::Nv12: ok = UploadNv12(frame); break;
    case FormatKind::P010: ok = UploadP010(frame); break;
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

bool VideoRenderer::EnsureUiLayer(int width, int height) {
  if (uiTex_ && uiWidth_ == width && uiHeight_ == height) return true;
  uiSrv_.Reset();
  uiRtv_.Reset();
  uiTex_.Reset();
  uiWidth_ = 0;
  uiHeight_ = 0;
  if (width <= 0 || height <= 0) return false;

  // Eight bit on purpose. This holds an ordinary sRGB interface, and giving it
  // more precision than the thing that drew it would buy nothing.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ID3D11Device* dev = ctx_->device();
  if (FAILED(CAP_HR(dev->CreateTexture2D(&td, nullptr, &uiTex_)))) return false;
  if (FAILED(CAP_HR(dev->CreateRenderTargetView(uiTex_.Get(), nullptr, &uiRtv_)))) return false;
  if (FAILED(CAP_HR(dev->CreateShaderResourceView(uiTex_.Get(), nullptr, &uiSrv_)))) return false;
  uiWidth_ = width;
  uiHeight_ = height;
  return true;
}

bool VideoRenderer::BeginUiLayer() {
  if (!ctx_->hdrOutput()) return false;
  if (!EnsureUiLayer(ctx_->width(), ctx_->height())) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dc->ClearRenderTargetView(uiRtv_.Get(), clear);
  ID3D11RenderTargetView* rtv[] = {uiRtv_.Get()};
  dc->OMSetRenderTargets(1, rtv, nullptr);
  return true;
}

void VideoRenderer::CompositeUiLayer() {
  if (!ctx_->hdrOutput() || !uiSrv_) return;

  ID3D11DeviceContext* dc = ctx_->context();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbUi_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    float value[4] = {paperWhiteNits_, 0.0f, 0.0f, 0.0f};
    memcpy(mapped.pData, value, sizeof(value));
    dc->Unmap(cbUi_.Get(), 0);
  }

  ID3D11RenderTargetView* backbuffer[] = {ctx_->rtv()};
  dc->OMSetRenderTargets(1, backbuffer, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)ctx_->width();
  vp.Height = (float)ctx_->height();
  vp.MaxDepth = 1.0f;
  dc->RSSetViewports(1, &vp);

  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(psUiComposite_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {uiSrv_.Get()};
  dc->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {sampPoint_.Get()};
  dc->PSSetSamplers(0, 1, samp);
  ID3D11Buffer* cbs[] = {cbUi_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());

  const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dc->OMSetBlendState(blendPremultiplied_.Get(), blendFactor, 0xffffffff);
  dc->Draw(3, 0);
  dc->OMSetBlendState(nullptr, blendFactor, 0xffffffff);

  ID3D11ShaderResourceView* none[] = {nullptr};
  dc->PSSetShaderResources(0, 1, none);
}

bool VideoRenderer::UploadP010(const FrameView& frame) {
  // The same shape as NV12 with every sample twice as wide, chroma included:
  // (w/2) pairs of two sixteen bit values is w*2 bytes, the same as the luma row.
  const int w = source_.width;
  const int h = source_.height;
  const size_t srcPitch = source_.stride > 0 ? (size_t)source_.stride : (size_t)w * 2;
  const size_t lumaBytes = srcPitch * (size_t)h;
  const int ch = (h + 1) / 2;
  if (frame.size < lumaBytes + srcPitch * (size_t)ch) return false;

  ID3D11DeviceContext* dc = ctx_->context();
  D3D11_MAPPED_SUBRESOURCE mapped = {};

  if (FAILED(dc->Map(plane_[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data, srcPitch, (size_t)w * 2, h);
  dc->Unmap(plane_[0].Get(), 0);

  if (FAILED(dc->Map(plane_[1].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  CopyRows(mapped, frame.data + lumaBytes, srcPitch, (size_t)((w + 1) / 2) * 4, ch);
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
  if (readbackWidth_ != deliveryWidth_ || readbackHeight_ != deliveryHeight_) {
    ReleaseReadbackResources();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)deliveryWidth_;
    td.Height = (UINT)deliveryHeight_;
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
    readbackWidth_ = deliveryWidth_;
    readbackHeight_ = deliveryHeight_;
  }

  // The slot about to be written must not be the one the caller still holds.
  if (readbackWrite_ == readbackMapped_) return;

  // Straight off the intermediate whenever it already is what the recorder
  // wants: same size, eight bits, nothing to do. Otherwise through the delivery
  // pass, which handles either reason or both at once.
  ID3D11Texture2D* from = intermediate_.Get();
  if (deliveryNeeded()) {
    if (!RenderDelivery(false)) return;
    from = delivery_.Get();
  }
  ctx_->context()->CopyResource(readbackTex_[readbackWrite_].Get(), from);
  QueueHdrReadback();
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

  // Same two reasons as the recording path: square pixels, and eight bits out
  // of a picture that may not be eight bits. Either one sends the still through
  // the delivery pass first. The HDR case is not new here so much as newly
  // correct -- copying a half float intermediate straight into an eight bit
  // staging texture never was a legal copy.
  const bool viaDelivery = deliveryNeeded();
  if (viaDelivery && !RenderDelivery(false)) return false;
  ID3D11Texture2D* from = viaDelivery ? delivery_.Get() : intermediate_.Get();
  const int fromW = viaDelivery ? deliveryWidth_ : intermediateWidth_;
  const int fromH = viaDelivery ? deliveryHeight_ : intermediateHeight_;

  // A staging texture of its own rather than a slot from the readback ring: the
  // ring only exists while recording, and its slots are deliberately two frames
  // stale. A screenshot should be the picture that was on screen when the key
  // went down.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)fromW;
  td.Height = (UINT)fromH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(CAP_HR(ctx_->device()->CreateTexture2D(&td, nullptr, &staging)))) return false;

  ctx_->context()->CopyResource(staging.Get(), from);

  // Blocking map: D3D11_MAP_READ without DO_NOT_WAIT flushes and waits.
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(CAP_HR(ctx_->context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))) {
    return false;
  }

  const size_t rowBytes = (size_t)fromW * 4;
  pixels->resize(rowBytes * (size_t)fromH);
  const uint8_t* src = (const uint8_t*)mapped.pData;
  for (int y = 0; y < fromH; ++y) {
    uint8_t* dst = pixels->data() + (size_t)y * rowBytes;
    memcpy(dst, src + (size_t)y * (size_t)mapped.RowPitch, rowBytes);
    // The pipeline carries an alpha channel it has no use for. Whatever ended up
    // in it, a screenshot of opaque video is opaque.
    for (size_t x = 3; x < rowBytes; x += 4) dst[x] = 0xFF;
  }
  ctx_->context()->Unmap(staging.Get(), 0);

  if (width) *width = fromW;
  if (height) *height = fromH;
  return true;
}

// Shape the picture wants to be seen in, width over height.
//
// Built from the *cropped* size rather than the output size, and that is the
// whole subtlety of it. Line doubling puts two rows where the signal has one
// and a quarter turn moves the lines onto the other axis; neither changes what
// shape the picture is, so neither may be allowed to leak in through a pixel
// count. Only cropping and the turn itself really move the answer.
double VideoRenderer::TargetAspect(const ImageSettings& image) const {
  const int w = croppedWidth_;
  const int h = croppedHeight_;
  if (w <= 0 || h <= 0) return 0.0;

  const bool turned =
      image.rotation == Rotation::Cw90 || image.rotation == Rotation::Ccw90;

  // A forced aspect describes the signal, not the window: turn a 4:3 console on
  // its side and what you should be looking at is 3:4.
  if (image.aspect == AspectMode::Force16x9) return turned ? 9.0 / 16.0 : 16.0 / 9.0;
  if (image.aspect == AspectMode::Force4x3) return turned ? 3.0 / 4.0 : 4.0 / 3.0;

  double dar = (double)w / (double)h;
  if (source_.aspectX > 0 && source_.aspectY > 0 && source_.width > 0 &&
      source_.height > 0) {
    // The media type's aspect describes the whole uncropped frame, so it has to
    // be reduced to a per-pixel figure first -- how much wider a source pixel
    // is than it is tall. Otherwise a cropped picture comes out stretched.
    const double par = ((double)source_.aspectX / (double)source_.aspectY) *
                       ((double)source_.height / (double)source_.width);
    if (par > 0.0) dar = (double)w * par / (double)h;
  } else if (analogueSource_ && source_.width > 0 && source_.height > 0) {
    // Kein Seitenverhaeltnis im Medientyp -- aber am Analogeingang muss auch
    // keines drinstehen, weil es dort nur eines gibt.
    //
    // Eine analoge Bildzeile ist keine Reihe von Bildpunkten, sondern ein
    // durchgehender Spannungsverlauf; die 720 Werte sind die Abtastung, die
    // sich die Karte ausgesucht hat, und ueber die Form des Bildes sagen sie
    // nichts. Die steht in der Norm, und die kennt genau eine: 4:3. Wer das
    // ignoriert, zeigt PAL mit 720/576 = 1,25 an und nimmt es auch so auf --
    // sechs Prozent zu schmal, ein Kreis wird zum stehenden Ei.
    //
    // Die SA7160 tut genau das: sie beschreibt Composite mit
    // `FORMAT_VideoInfo`, und die Struktur hat gar kein Feld dafuer. Am
    // 30.08. um 13:55 Uhr kam der Screenshot deshalb als 720x576 heraus statt
    // als 768x576, und im Log stand keine einzige Zeile ueber quadratische
    // Pixel -- der Durchgang lief, verglich 1,25 mit 1,25 und hatte nichts zu
    // tun.
    //
    // Anamorphes PAL waere die Ausnahme, aber die steht im WSS-Signal der
    // Austastluecke, und das reicht keine Karte hier durch. Dafuer gibt es die
    // feste 16:9-Einstellung.
    const double par = (4.0 / 3.0) * ((double)source_.height / (double)source_.width);
    if (par > 0.0) dar = (double)w * par / (double)h;
  }
  if (dar <= 0.0) dar = (double)w / (double)h;
  return turned ? 1.0 / dar : dar;
}

// What size the picture leaves at, for everything that is not the window.
//
// The window can show non-square pixels for nothing: it simply draws into a
// rectangle of the right shape. A file cannot -- it is a grid, and a grid has
// no room for "these pixels are not square" -- so anything leaving gets
// resampled instead, and PAL's 720x576 goes out as 768x576.
//
// On HDMI the pixels are already square, the two sizes come out equal, and the
// whole pass is skipped.
void VideoRenderer::ComputeDeliverySize(const ImageSettings& image) {
  deliveryWidth_ = outputWidth_;
  deliveryHeight_ = outputHeight_;
  deliveryFilter_ = (int)image.filter;

  deliveryBrightness_ = Clamp(image.brightness, -1.0f, 1.0f);
  deliveryContrast_ = Clamp(image.contrast, 0.0f, 2.0f);
  deliverySaturation_ = Clamp(image.saturation, 0.0f, 2.0f);
  deliveryHue_ = Clamp(image.hue, -180.0f, 180.0f) * 3.14159265358979f / 180.0f;
  deliveryProcAmp_ = image.procAmpToOutput;

  if (!image.squarePixelOutput) return;
  if (outputWidth_ <= 0 || outputHeight_ <= 0) return;
  // Neither of these has a shape to resample to. Stretch takes whatever the
  // window happens to be, which is not a property of the picture at all, and
  // Integer is a promise about pixels that resampling is precisely the way to
  // break.
  if (image.aspect == AspectMode::Stretch || image.aspect == AspectMode::Integer) return;

  const double target = TargetAspect(image);
  if (target <= 0.0) return;

  // Not an exact comparison. A card is free to describe 16:9 as 157:88 or to
  // round its own numbers, and a fifth of a percent of aspect is a pixel or two
  // -- nowhere near worth resampling a whole frame for, and the resample would
  // do more damage than the error it corrects.
  const double have = (double)outputWidth_ / (double)outputHeight_;
  if (std::abs(have - target) <= target * 0.005) return;

  // Grow the short axis rather than shrink the long one. Resampling can only
  // lose detail, so a pass that has to happen anyway should at least not throw
  // samples away on top of it.
  int w = outputWidth_;
  int h = outputHeight_;
  if (target > have) {
    w = (int)std::lround((double)outputHeight_ * target);
  } else {
    h = (int)std::lround((double)outputWidth_ / target);
  }
  // Every encoder in reach subsamples chroma, and half of an odd number is not
  // a number of pixels. x264 refuses outright; others round quietly and shear.
  // To the *nearest* even, not upwards: rounding 721 up to 722 would invent a
  // resampling pass out of a rounding error.
  w = 2 * (int)std::lround((double)w / 2.0);
  h = 2 * (int)std::lround((double)h / 2.0);
  w = std::max(2, std::min(w, 8192));
  h = std::max(2, std::min(h, 8192));

  // After that rounding there may be no difference left at all.
  if (w == outputWidth_ && h == outputHeight_) return;

  deliveryWidth_ = w;
  deliveryHeight_ = h;

  // Worth a line in the log: it changes the size of every recording, every
  // screenshot and the camera, and it is the first thing to look at when one of
  // them comes out an unexpected shape.
  if (w != loggedDeliveryW_ || h != loggedDeliveryH_ || outputWidth_ != loggedOutW_ ||
      outputHeight_ != loggedOutH_) {
    loggedDeliveryW_ = w;
    loggedDeliveryH_ = h;
    loggedOutW_ = outputWidth_;
    loggedOutH_ = outputHeight_;
    CAP_LOG("Ausgabe auf quadratische Pixel: %dx%d -> %dx%d (Seitenverhältnis %.4f)",
            outputWidth_, outputHeight_, w, h, target);
  }
}

void VideoRenderer::ComputeDestRect(const ImageSettings& image) {
  // Fit into the window minus the reserved strip, then push the result down by
  // it. Every branch below can then go on thinking it owns the whole window.
  ComputeDestRectIn(image, ctx_->width(), ctx_->height() - topInset_);
  videoRect_.top += topInset_;
  videoRect_.bottom += topInset_;
}

void VideoRenderer::ComputeDestRectIn(const ImageSettings& image, int winW, int winH) {
  const int srcW = outputWidth_;
  const int srcH = outputHeight_;

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

  double target = TargetAspect(image);
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

  // What comes out of the first pass. Doubling makes it taller, a quarter turn
  // swaps the axes, and everything downstream -- aspect, recording, stills --
  // reads these rather than the cropped size.
  const int doubledHeight = croppedHeight_ * (image.lineDouble ? 2 : 1);
  const bool quarterTurn =
      image.rotation == Rotation::Cw90 || image.rotation == Rotation::Ccw90;
  outputWidth_ = quarterTurn ? doubledHeight : croppedWidth_;
  outputHeight_ = quarterTurn ? croppedWidth_ : doubledHeight;

  // The size everything that is not the window leaves at, plus the filter that
  // gets it there. Worked out here because this is the only place holding the
  // settings; the readback and the stills run outside Draw and read what this
  // leaves behind.
  ComputeDeliverySize(image);

  if (!EnsureClean(srcW, srcH)) return;
  if (!EnsureIntermediate(outputWidth_, outputHeight_)) return;

  ID3D11DeviceContext* dc = ctx_->context();

  // The media type is believed when it claims interlaced; when it says nothing,
  // which is the normal case on an analogue input, the measurement decides.
  const bool interlaced =
      source_.interlaced || interlaceVerdict_ == InterlaceVerdict::Interlaced;
  Deinterlace deint = image.deinterlace;
  if (image.deinterlaceAuto && !interlaced) deint = Deinterlace::Off;
  // YADIF is not the only thing that needs yesterday's picture any more.
  historyWanted_ = DeinterlaceNeedsHistory(deint) || image.temporalDenoise > 0.0f;

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
  cb.havePrev = historyCount_ > 0 ? 1 : 0;
  cb.histCount = historyCount_;
  cb.rotation = (int32_t)image.rotation;
  cb.lineDouble = image.lineDouble ? 1 : 0;
  cb.chromaSoft = Clamp(image.chromaSoft, 0, 8);
  cb.temporal = image.temporalDenoise < 0.0f   ? 0.0f
                : image.temporalDenoise > 1.0f ? 1.0f
                                               : image.temporalDenoise;
  // Kriechen zuerst, oder Ghosting zuerst -- derselbe Ausdruck an zwei
  // Arbeitspunkten.
  //
  // Das Spiel bleibt in beiden Faellen bei fuenf von 255 Stufen, und zwar
  // absichtlich: das ist nicht die Empfindlichkeit, sondern der Rauschboden der
  // Karte. Wer den wegnimmt, laesst den Filter auf ruhendem Bild an- und
  // abschalten und hat nichts gewonnen. Verschoben wird nur die Flanke: ganz
  // zu nach 26 Stufen oder schon nach 9. Dazwischen liegen genau die langsamen,
  // kontrastarmen Bewegungen, an denen die Fahne sichtbar wird.
  cb.motionSlack = 0.02f;
  cb.motionSlope = image.avoidGhosting ? 64.0f : 12.0f;
  cb.dotNotch = image.dotNotch < 0.0f   ? 0.0f
                : image.dotNotch > 1.0f ? 1.0f
                                        : image.dotNotch;
  // The subcarrier's period in samples scales with how many samples the card
  // puts on a line: the same cycle spread over more pixels is more pixels long.
  cb.carrierPeriod = (float)(carrierSamples_ * (double)srcW / 720.0);
  cb.coSitedPhase = coSitedFields_ ? coSitedPhase_ : -1;
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

  cb.pixelScale = (kind_ == FormatKind::P010 && tenBitContainer_) ? 65535.0f / 65472.0f : 1.0f;
  cb.transfer = (int32_t)hdrTransfer_;
  cb.gamut = hdrWideGamut_ ? 1 : 0;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(dc->Map(cbConvert_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &cb, sizeof(cb));
    dc->Unmap(cbConvert_.Get(), 0);
  }

  ID3D11ShaderResourceView* nullSrvs[3 + kHistoryDepth * 3] = {};
  dc->IASetInputLayout(nullptr);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cbConvert_.Get()};
  dc->PSSetConstantBuffers(0, 1, cbs);
  dc->RSSetState(raster_.Get());
  const float blendFactor[4] = {0, 0, 0, 0};
  dc->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);

  // ---- pass 1: decode the planes and clean the signal, in source geometry ----
  //
  // Deliberately before anything touches the field structure. A cleanup that
  // runs after a deinterlacer has to work out which source line the pixel in
  // front of it came from, and for the interpolating modes there is no single
  // answer -- which is how the same class of artefact kept coming back.
  {
    ID3D11RenderTargetView* rtv[] = {cleanRtv_.Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)srcW;
    vp.Height = (float)srcH;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    // Newest history first, so the shader can treat them as "one frame ago",
    // "two frames ago", "three frames ago" without knowing where the ring
    // happens to stand. historyWrite_ points at the slot that will be
    // overwritten next, which is the oldest one.
    ID3D11ShaderResourceView* srvs[3 + kHistoryDepth * 3] = {};
    for (int i = 0; i < 3; ++i) srvs[i] = planeSrv_[i].Get();
    for (int h = 0; h < kHistoryDepth; ++h) {
      const int slot = (historyWrite_ - 1 - h + kHistoryDepth * 2) % kHistoryDepth;
      for (int i = 0; i < 3; ++i) srvs[3 + h * 3 + i] = planeHistSrv_[slot][i].Get();
    }
    dc->PSSetShaderResources(0, 3 + kHistoryDepth * 3, srvs);
    dc->PSSetShader(psClean_.Get(), nullptr, 0);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, 3 + kHistoryDepth * 3, nullSrvs);
  }

  // ---- pass 2: fields, cropping, line doubling, rotation ----
  {
    ID3D11RenderTargetView* rtv[] = {intermediateRtv_.Get()};
    dc->OMSetRenderTargets(1, rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)outputWidth_;
    vp.Height = (float)outputHeight_;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* srvs[2] = {cleanSrv_.Get(), cleanPrevSrv_.Get()};
    dc->PSSetShaderResources(0, 2, srvs);
    dc->PSSetShader(psConvert_.Get(), nullptr, 0);
    dc->Draw(3, 0);
  }

  // ---- pass 3: scale onto the back buffer ----
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
  sc.srcSize[0] = (float)outputWidth_;
  sc.srcSize[1] = (float)outputHeight_;
  sc.dstSize[0] = (float)dstW;
  sc.dstSize[1] = (float)dstH;
  sc.filter = (int32_t)image.filter;
  sc.sharpen = Clamp(image.sharpen, 0.0f, 1.0f);
  sc.scanlines = Clamp(image.scanlines, 0.0f, 1.0f);
  sc.mask = (int32_t)Clamp(image.mask, 0, 2);
  sc.maskStrength = Clamp(image.maskStrength, 0.0f, 1.0f);
  // Only meaningful when it is actually below what the card delivers; asking to
  // "recover" a grid wider than the samples there are would invent detail.
  sc.nativeWidth = image.nativeWidth > 0 && image.nativeWidth < srcW
                       ? (int32_t)Clamp(image.nativeWidth, 64, 4096)
                       : 0;

  // How many rows of the intermediate make up one line the console drew.
  // Doubling and co-sited fields each put two rows where the signal has one;
  // a quarter turn puts the lines along the other axis entirely, and rather
  // than draw them sideways the effect simply stands down.
  float pitch = 1.0f;
  if (image.lineDouble) pitch *= 2.0f;
  if (coSitedFields_) pitch *= 2.0f;
  const bool turned =
      image.rotation == Rotation::Cw90 || image.rotation == Rotation::Ccw90;
  sc.linePitch = turned ? 0.0f : pitch;
  sc.transfer = (int32_t)hdrTransfer_;
  sc.outputHdr = hdrOutput_ ? 1 : 0;
  sc.paperWhite = paperWhiteNits_;
  sc.sourcePeak = sourcePeakNits_;
  sc.displayPeak = displayPeakNits_;

  // The window always gets them -- that is what the sliders are for. Whether
  // they also go into a file is decided in RenderDelivery, not here.
  sc.procAmp = procAmpActive() ? 1 : 0;
  sc.brightness = deliveryBrightness_;
  sc.contrast = deliveryContrast_;
  sc.saturation = deliverySaturation_;
  sc.hue = deliveryHue_;

  if (SUCCEEDED(dc->Map(cbScale_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &sc, sizeof(sc));
    dc->Unmap(cbScale_.Get(), 0);
  }

  ID3D11RenderTargetView* backbuffer[] = {ctx_->rtv()};
  dc->OMSetRenderTargets(1, backbuffer, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.MaxDepth = 1.0f;
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
