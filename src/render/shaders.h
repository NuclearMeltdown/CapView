#pragma once

// HLSL for the two render passes, compiled at startup with D3DCompile.
//
// Pass 1 ("convert") turns whatever the card delivers into linear-indexed RGBA
// at the cropped source resolution: colour space conversion, range expansion,
// bob deinterlacing and cropping all happen here, all with texel fetches so no
// sampler filtering can smear anything before we mean it to.
//
// Pass 2 ("scale") resamples that intermediate onto the window with the
// selected filter, plus optional sharpening.

namespace cap {

inline const char* kFullscreenVS = R"HLSL(
struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

// Single oversized triangle -- no vertex or index buffer needed.
VSOut main(uint vid : SV_VertexID) {
  VSOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
)HLSL";

inline const char* kConvertPS = R"HLSL(
Texture2D<float4> tex0 : register(t0);
Texture2D<float4> tex1 : register(t1);
Texture2D<float4> tex2 : register(t2);

cbuffer ConvertCB : register(b0) {
  int   gFormatKind;      // see FormatKind in video_renderer.h
  int   gDeinterlaceMode; // 0 off, 1 bob, 2 bob linear
  int   gFieldIndex;      // 0 = first field, 1 = second field
  int   gBottomUp;

  int   gCropLeft;
  int   gCropTop;
  int   gSrcWidth;
  int   gSrcHeight;

  int   gOutWidth;
  int   gOutHeight;
  int   gIsYuv;
  int   gPad0;

  float gYOffset;
  float gYScale;
  float gCScale;
  float gPad1;

  float4 gCoef;           // Cr->R, Cb->G, Cr->G, Cb->B
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 FetchYuv(int2 p) {
  // p is a coordinate in the full source frame.
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    // Packed 4:2:2. One texel of the RGBA8 view holds two pixels.
    float4 t = tex0.Load(int3(p.x >> 1, p.y, 0));
    bool odd = (p.x & 1) != 0;
    if (gFormatKind == 0) {          // YUY2: Y0 U Y1 V
      return float3(odd ? t.z : t.x, t.y, t.w);
    } else if (gFormatKind == 1) {   // UYVY: U Y0 V Y1
      return float3(odd ? t.w : t.y, t.x, t.z);
    } else {                         // YVYU: Y0 V Y1 U
      return float3(odd ? t.z : t.x, t.w, t.y);
    }
  } else if (gFormatKind == 3) {
    // NV12: full res luma, interleaved half res chroma.
    float y  = tex0.Load(int3(p, 0)).x;
    float2 c = tex1.Load(int3(p.x >> 1, p.y >> 1, 0)).xy;
    return float3(y, c.x, c.y);
  } else {
    // Planar 4:2:0. Plane order differs between YV12 and I420, which the
    // caller has already resolved by binding the planes accordingly.
    float y = tex0.Load(int3(p, 0)).x;
    float u = tex1.Load(int3(p.x >> 1, p.y >> 1, 0)).x;
    float v = tex2.Load(int3(p.x >> 1, p.y >> 1, 0)).x;
    return float3(y, u, v);
  }
}

float3 YuvToRgb(float3 yuv) {
  float y = (yuv.x - gYOffset) * gYScale;
  float cb = (yuv.y - 0.5) * gCScale;
  float cr = (yuv.z - 0.5) * gCScale;
  float3 rgb;
  rgb.r = y + gCoef.x * cr;
  rgb.g = y - gCoef.y * cb - gCoef.z * cr;
  rgb.b = y + gCoef.w * cb;
  return rgb;
}

float3 FetchRgbAt(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv != 0) {
    return YuvToRgb(FetchYuv(q));
  }
  float3 rgb = tex0.Load(int3(q, 0)).rgb;
  // RGB from a capture card can still be limited range.
  return (rgb - gYOffset) * gYScale;
}

float4 main(VSOut i) : SV_Target {
  int2 op = int2(i.pos.xy);
  int srcX = op.x + gCropLeft;

  // Rows of the cropped area, in full-frame coordinates.
  int rowTop = gCropTop;
  int rowBottom = gCropTop + gOutHeight - 1;

  float3 rgb;
  if (gDeinterlaceMode == 0) {
    rgb = FetchRgbAt(int2(srcX, gCropTop + op.y));
  } else {
    // Reconstruct a full frame out of the selected field. `t` is the
    // continuous index of that field's own rows.
    float t = (float(gCropTop + op.y) - float(gFieldIndex)) * 0.5;
    if (gDeinterlaceMode == 1) {
      int row = int(round(t)) * 2 + gFieldIndex;
      rgb = FetchRgbAt(int2(srcX, clamp(row, rowTop, rowBottom)));
    } else {
      float fl = floor(t);
      float frac = t - fl;
      int row0 = int(fl) * 2 + gFieldIndex;
      int row1 = row0 + 2;
      float3 a = FetchRgbAt(int2(srcX, clamp(row0, rowTop, rowBottom)));
      float3 b = FetchRgbAt(int2(srcX, clamp(row1, rowTop, rowBottom)));
      rgb = lerp(a, b, frac);
    }
  }
  return float4(saturate(rgb), 1.0);
}
)HLSL";

inline const char* kScalePS = R"HLSL(
Texture2D<float4> texSrc : register(t0);
SamplerState sampPoint  : register(s0);
SamplerState sampLinear : register(s1);

cbuffer ScaleCB : register(b0) {
  float2 gSrcSize;
  float2 gDstSize;
  int    gFilter;   // 0 nearest, 1 bilinear, 2 bicubic, 3 lanczos3, 4 sharp bilinear
  float  gSharpen;  // 0..1
  float2 gPad;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 Tap(float2 texel) {
  float2 uv = (texel + 0.5) / gSrcSize;
  return texSrc.SampleLevel(sampPoint, uv, 0).rgb;
}

float CatmullRom(float x) {
  x = abs(x);
  if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

float Sinc(float x) {
  if (abs(x) < 1e-5) return 1.0;
  float px = 3.14159265358979 * x;
  return sin(px) / px;
}

float Lanczos3(float x) {
  x = abs(x);
  if (x >= 3.0) return 0.0;
  return Sinc(x) * Sinc(x / 3.0);
}

float3 SampleBicubic(float2 pos) {
  float2 base = floor(pos - 0.5);
  float2 f = pos - 0.5 - base;
  float3 sum = 0.0;
  float wsum = 0.0;
  [unroll] for (int dy = -1; dy <= 2; ++dy) {
    float wy = CatmullRom(float(dy) - f.y);
    [unroll] for (int dx = -1; dx <= 2; ++dx) {
      float w = CatmullRom(float(dx) - f.x) * wy;
      sum += Tap(base + float2(dx, dy)) * w;
      wsum += w;
    }
  }
  return sum / max(wsum, 1e-5);
}

float3 SampleLanczos(float2 pos) {
  float2 base = floor(pos - 0.5);
  float2 f = pos - 0.5 - base;
  float3 sum = 0.0;
  float wsum = 0.0;
  [unroll] for (int dy = -2; dy <= 3; ++dy) {
    float wy = Lanczos3(float(dy) - f.y);
    [unroll] for (int dx = -2; dx <= 3; ++dx) {
      float w = Lanczos3(float(dx) - f.x) * wy;
      sum += Tap(base + float2(dx, dy)) * w;
      wsum += w;
    }
  }
  return sum / max(wsum, 1e-5);
}

// Nearest neighbour, but the transition between two source pixels is squeezed
// into the one output pixel that straddles the boundary. Crisp like nearest,
// without the unevenly wide pixels you get at non-integer scale factors.
float3 SampleSharpBilinear(float2 pos) {
  float2 scale = max(gDstSize / gSrcSize, 1.0);
  float2 texelFloor = floor(pos);
  float2 s = pos - texelFloor;
  float2 regionRange = 0.5 - 0.5 / scale;
  float2 centerDist = s - 0.5;
  float2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * scale + 0.5;
  float2 uv = (texelFloor + f) / gSrcSize;
  return texSrc.SampleLevel(sampLinear, uv, 0).rgb;
}

// Light contrast adaptive sharpening: boost the centre against its cross
// neighbourhood one output pixel away, clamped to the local min/max so flat
// areas stay clean and edges do not ring.
float3 Sharpen(float3 c, float2 uv, float amount) {
  float2 d = 1.0 / gDstSize;
  float3 n = texSrc.SampleLevel(sampLinear, uv + float2(0.0, -d.y), 0).rgb;
  float3 s = texSrc.SampleLevel(sampLinear, uv + float2(0.0, d.y), 0).rgb;
  float3 w = texSrc.SampleLevel(sampLinear, uv + float2(-d.x, 0.0), 0).rgb;
  float3 e = texSrc.SampleLevel(sampLinear, uv + float2(d.x, 0.0), 0).rgb;
  float3 lo = min(min(min(n, s), min(w, e)), c);
  float3 hi = max(max(max(n, s), max(w, e)), c);
  float3 sharp = c + (c * 4.0 - n - s - w - e) * (amount * 0.25);
  return clamp(sharp, lo, hi);
}

float4 main(VSOut i) : SV_Target {
  float2 pos = i.uv * gSrcSize;  // position in source pixels

  float3 rgb;
  if (gFilter == 0) {
    rgb = texSrc.SampleLevel(sampPoint, i.uv, 0).rgb;
  } else if (gFilter == 1) {
    rgb = texSrc.SampleLevel(sampLinear, i.uv, 0).rgb;
  } else if (gFilter == 2) {
    rgb = SampleBicubic(pos);
  } else if (gFilter == 3) {
    rgb = SampleLanczos(pos);
  } else {
    rgb = SampleSharpBilinear(pos);
  }

  if (gSharpen > 0.001) {
    rgb = Sharpen(rgb, i.uv, gSharpen);
  }
  return float4(saturate(rgb), 1.0);
}
)HLSL";

}  // namespace cap
