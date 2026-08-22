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

inline const char* kCleanPS =
R"HLSL(// Pass one: turn the captured planes into a picture and clean it up, still
// in the source's own geometry. Nothing here knows about fields, cropping,
// rotation or scaling -- those all come afterwards, and they all get to work
// on a signal that has already had the composite artefacts taken out of it.
Texture2D<float4> tex0 : register(t0);
Texture2D<float4> tex1 : register(t1);
Texture2D<float4> tex2 : register(t2);

// The same planes for the three preceding frames, newest first. YADIF needs one
// of them; the composite denoiser needs all three, because the PAL colour
// subcarrier walks through a four frame sequence and only a full four frame
// average cancels it. Bound but never sampled when nothing wants them, and the
// copies that fill them are skipped in that case.
Texture2D<float4> prvA0 : register(t3);
Texture2D<float4> prvA1 : register(t4);
Texture2D<float4> prvA2 : register(t5);
Texture2D<float4> prvB0 : register(t6);
Texture2D<float4> prvB1 : register(t7);
Texture2D<float4> prvB2 : register(t8);
Texture2D<float4> prvC0 : register(t9);
Texture2D<float4> prvC1 : register(t10);
Texture2D<float4> prvC2 : register(t11);

cbuffer ConvertCB : register(b0) {
  int   gFormatKind;      // see FormatKind in video_renderer.h
  int   gDeinterlaceMode; // 0 off, 1 bob, 2 bob linear, 3 motion adaptive,
                          // 4 edge directed, 5 yadif
  int   gFieldIndex;      // 0 = first field, 1 = second field
  int   gBottomUp;

  int   gCropLeft;
  int   gCropTop;
  int   gSrcWidth;
  int   gSrcHeight;

  int   gOutWidth;
  int   gOutHeight;
  int   gIsYuv;
  int   gHavePrev;        // previous frame's planes hold a usable picture

  float gYOffset;
  float gYScale;
  float gCScale;
  float gPad1;

  int   gCoSitedPhase;    // -1 normal interlace, else the row a field pair starts on
  int   gRotation;        // 0 none, 1 quarter turn right, 2 half, 3 quarter turn left
  int   gLineDouble;      // 1 = every source line fills two output lines
  int   gChromaSoft;      // horizontal chroma blur radius in source pixels, 0 off

  float gTemporal;        // 0..1 strength of the temporal average
  int   gHistCount;       // how many previous frames actually hold a picture
  float gDotNotch;        // 0..1: width of the demodulation window, 0 = off
  float gCarrierPeriod;   // samples per cycle of the colour subcarrier

  float4 gCoef;           // Cr->R, Cb->G, Cr->G, Cb->B
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

// `frame` is 0 for the picture being shown and 1..3 for the ones before it.
// HLSL has no way to put a texture behind a variable here, so picking one is a
// chain of comparisons rather than an index.
float4 LoadPlane0(int3 at, int frame) {
  if (frame == 0) return tex0.Load(at);
  if (frame == 1) return prvA0.Load(at);
  if (frame == 2) return prvB0.Load(at);
  return prvC0.Load(at);
}
float4 LoadPlane1(int3 at, int frame) {
  if (frame == 0) return tex1.Load(at);
  if (frame == 1) return prvA1.Load(at);
  if (frame == 2) return prvB1.Load(at);
  return prvC1.Load(at);
}
float4 LoadPlane2(int3 at, int frame) {
  if (frame == 0) return tex2.Load(at);
  if (frame == 1) return prvA2.Load(at);
  if (frame == 2) return prvB2.Load(at);
  return prvC2.Load(at);
}

float3 FetchYuv(int2 p, int frame) {
  // p is a coordinate in the full source frame.
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    // Packed 4:2:2. One texel of the RGBA8 view holds two pixels.
    float4 t = LoadPlane0(int3(p.x >> 1, p.y, 0), frame);
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
    float y  = LoadPlane0(int3(p, 0), frame).x;
    float2 c = LoadPlane1(int3(p.x >> 1, p.y >> 1, 0), frame).xy;
    return float3(y, c.x, c.y);
  } else {
    // Planar 4:2:0. Plane order differs between YV12 and I420, which the
    // caller has already resolved by binding the planes accordingly.
    int3 ac = int3(p.x >> 1, p.y >> 1, 0);
    float y = LoadPlane0(int3(p, 0), frame).x;
    float u = LoadPlane1(ac, frame).x;
    float v = LoadPlane2(ac, frame).x;
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

float3 FetchRgbIn(int2 p, int frame) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv != 0) {
    return YuvToRgb(FetchYuv(q, frame));
  }
  float3 rgb = LoadPlane0(int3(q, 0), frame).rgb;
  // RGB from a capture card can still be limited range.
  return (rgb - gYOffset) * gYScale;
}

// The current frame, read straight from its own textures. Worth writing twice:
// the deinterlacers call this dozens of times per pixel, and going through the
// four way choice of which frame to read expands every one of those calls into
// four branches for a decision that is a constant here. That expansion is
// nobody's runtime cost, but it is very much the shader compiler's -- and the
// compiler runs while the user waits for the window to appear.
float3 FetchYuvCur(int2 p) {
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    float4 t = tex0.Load(int3(p.x >> 1, p.y, 0));
    bool odd = (p.x & 1) != 0;
    if (gFormatKind == 0) {
      return float3(odd ? t.z : t.x, t.y, t.w);
    } else if (gFormatKind == 1) {
      return float3(odd ? t.w : t.y, t.x, t.z);
    } else {
      return float3(odd ? t.z : t.x, t.w, t.y);
    }
  } else if (gFormatKind == 3) {
    float y  = tex0.Load(int3(p, 0)).x;
    float2 c = tex1.Load(int3(p.x >> 1, p.y >> 1, 0)).xy;
    return float3(y, c.x, c.y);
  } else {
    int3 ac = int3(p.x >> 1, p.y >> 1, 0);
    return float3(tex0.Load(int3(p, 0)).x, tex1.Load(ac).x, tex2.Load(ac).x);
  }
}

float3 FetchRgbAt(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  float3 rgb;
  if (gIsYuv != 0) {
    rgb = YuvToRgb(FetchYuvCur(q));
  } else {
    rgb = tex0.Load(int3(q, 0)).rgb;
    rgb = (rgb - gYOffset) * gYScale;
  }
  return rgb;
}
float3 FetchRgbPrev(int2 p) { return FetchRgbIn(p, 1); }
float3 FetchRgbFrame(int2 p, int frame) { return FetchRgbIn(p, frame); }

float Luma(float3 c) {
  return dot(c, float3(0.299, 0.587, 0.114));
}

// Brightness alone, without the colour conversion. The motion test below needs
// a lot of samples and none of the colour, and converting each one would triple
// its cost for nothing.
float LumaFrameAt(int2 p, int frame) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv == 0) return Luma(LoadPlane0(int3(q, 0), frame).rgb);
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    float4 t = LoadPlane0(int3(q.x >> 1, q.y, 0), frame);
    bool odd = (q.x & 1) != 0;
    if (gFormatKind == 1) return odd ? t.w : t.y;   // UYVY
    return odd ? t.z : t.x;                          // YUY2 and YVYU
  }
  return LoadPlane0(int3(q, 0), frame).x;
}

// Composite carries colour at roughly a quarter of the bandwidth it carries
// brightness, so a picture decoded from it has no fine colour detail in it to
// begin with. Averaging the colour sideways therefore costs nothing real, and it
// takes the rainbow shimmer with it -- that shimmer is dense luma detail the
// decoder mistook for colour, and it lives in the chroma.
//
// The brightness is put back sharp afterwards. Only the colour is softened.
float3 SoftenChroma(float3 rgb, int x, int row) {
  float3 sum = 0.0;
  float n = 0.0;
  // Deliberately not unrolled. Every FetchRgbAt expands into a pixel format
  // branch and a four way choice of which frame to read, so seventeen unrolled
  // copies of it cost the shader compiler seconds -- which the user waits
  // through at every start. A real loop over the actual radius costs nothing at
  // runtime and compiles in a fraction of the time.
  for (int dx = -gChromaSoft; dx <= gChromaSoft; ++dx) {
    sum += FetchRgbAt(int2(x + dx, row));
    n += 1.0;
  }
  float3 soft = sum / max(n, 1.0);
  return soft + (Luma(rgb) - Luma(soft));
}

// Dot crawl is the other half of the same crosstalk, going the other way: colour
// leaking into brightness, as that crawling zip along vertical colour edges.
// Blurring the chroma does nothing for it, because it is not in the chroma.
//
// What does work is time. The colour subcarrier inverts its phase from one frame
// to the next, so the pattern it leaves is inverted too, and averaging two
// consecutive frames cancels it -- but only where the picture stood still.
// Anywhere something moved, the average would be a smear, so the blend is let go
// exactly there. Analogue noise, which is uncorrelated between frames, goes the
// same way for free.
// Four frames, not two, and the number comes from the card rather than from a
// textbook. Measured on this signal, the mean frame to frame difference of a
// still picture runs 1.91 at a lag of one frame, 2.62 at two, 1.90 at three and
// 0.78 at four -- a clean four frame cycle, which is the PAL subcarrier walking
// through its sequence. Averaging two frames therefore cancels nothing; it picks
// two arbitrary points of that cycle. Averaging four covers it exactly, and what
// is left is the picture.
//
// The correction is computed on the woven pixels and then applied to whatever
// the deinterlacer produced, so cleaning the picture never undoes the
// reconstruction.
// What four frames of averaging would change about one source pixel. Kept apart
// from the decision of whether to apply it, because a deinterlaced pixel does
// not come from the row it is drawn on -- and the pattern has no vertical
// correlation whatsoever, so a correction taken from the neighbouring line does
// not remove the crawl, it lays a second one on top. That is why the filter
// worked with deinterlacing off and fell apart with it on.
float3 TemporalDelta(int x, int row) {
  float3 f0 = FetchRgbFrame(int2(x, row), 0);
  float3 f1 = FetchRgbFrame(int2(x, row), 1);
  float3 f2 = FetchRgbFrame(int2(x, row), 2);
  float3 f3 = FetchRgbFrame(int2(x, row), 3);
  return (f0 + f1 + f2 + f3) * 0.25 - f0;
}

// How much of that correction to trust here: everything where the picture is
// standing still, nothing where it is moving.
float TemporalGate(int x, int row) {
  // How much this spot actually moved, as opposed to shimmered -- and the
  // difference between those two is the whole problem. Dot crawl crawls: it is
  // not still, so any detector that simply asks "did this pixel change" calls it
  // movement and switches the filter off exactly where the artefact is. Measured
  // that way the filter took 48 % off the mild half of the picture and 15 % off
  // the worst one per cent, which is the wrong way round and invisible.
  //
  // What separates them is not amount but scale. The shimmer is a fine pattern,
  // a few pixels across -- that is what makes it look like dots -- while a
  // picture that is really moving moves in broad shapes. So the decision is made
  // on a horizontally smoothed picture, over seven samples, which is a little
  // over two cycles of the colour subcarrier: the shimmer averages itself away
  // before it is ever asked about, and real movement comes through untouched.
  float s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
  for (int dx = -3; dx <= 3; ++dx) {
    int2 q = int2(x + dx, row);
)HLSL"
R"HLSL(    s0 += LumaFrameAt(q, 0);
    s1 += LumaFrameAt(q, 1);
    s2 += LumaFrameAt(q, 2);
    s3 += LumaFrameAt(q, 3);
  }
  float mhi = max(max(s0, s1), max(s2, s3)) * 0.142857;
  float mlo = min(min(s0, s1), min(s2, s3)) * 0.142857;
  // Five levels out of 255 of slack, then full suppression twenty-odd levels
  // later. Below the slack nothing is moving; above it, something is.
  return 1.0 - saturate((mhi - mlo - 0.02) * 12.0);
}

// The spatial half of the same job, for everything the temporal filter cannot
// reach. Averaging frames only works where the picture stands still; in a
// running game most of the screen does not, and there the crawl stays.
//
// Measured on this card, the crawling pattern repeats every three pixels across
// a line -- the horizontal autocorrelation runs -0.35, -0.37, +0.43 at one, two
// and three pixels -- and shows no correlation at all from one line to the next.
// That is the colour subcarrier, 4.43 MHz against roughly 13.5 MHz of sampling,
// and it means a three sample average has its null exactly on top of it.
//
// So the pattern is what a three tap average removes: the residual between a
// pixel and that average is the crawl. Subtracting all of it would soften every
// sharp vertical edge as well, because a real edge has a residual too -- a much
// larger one. Hence the falloff: small residuals, which is all the crawl ever
// is, are taken out completely, and the bigger a residual gets the more of it
// survives.
//
// Costs a little horizontal sharpness at strong edges. That is the price of
// cleaning a picture that is moving, and it is why this is a separate knob.
// Dot crawl is the colour subcarrier leaking into brightness, so the way to
// take it out is to treat it as what it is: a carrier of known frequency,
// amplitude modulated by however much colour is at that point in the picture.
//
// A plain notch cannot do this well. Measured on this card, isolating the
// pattern through its own four frame cycle and looking at its spectrum, a band
// of thirty-two bins around the peak holds under half its energy -- the
// modulation smears it far too wide for any narrow filter to catch, and a wide
// one takes the picture with it.
//
// Synchronous detection does not care. Multiplying the line by the carrier and
// by the same carrier a quarter cycle over moves the pattern down to nothing,
// where a short average recovers exactly how much of it is there; multiplying
// back up reconstructs it, and it is subtracted. Picture detail is not tied to
// the carrier's phase and survives, apart from whatever happened to sit right on
// the frequency.
//
// Measured against this card's own frames, at a window of nine samples that
// removes 81 % of the pattern for 17 % of the horizontal sharpness, where the
// notch it replaces managed 67 % for 18 %. At five samples it reaches 99 %.
float DotDemodDelta(int x, int row) {
  // The slider is the window: wide and gentle at the left, narrow and thorough
  // at the right. 25 samples down to 5.
  int r = (int)floor((25.0 - gDotNotch * 20.0) * 0.5 + 0.5);
  r = clamp(r, 2, 12);

  const float kTwoPi = 6.28318531;
  float w = kTwoPi / max(gCarrierPeriod, 1.5);

  // Two passes over the window. The first is only there to find the local
  // average, and it is not optional: the carrier does not sum to zero over a
  // window of a few samples, so a flat area of picture correlates with it and
  // gets a pattern invented on top of itself. Measured on a constant field of
  // 128, leaving this out produced swings of ninety levels -- which is what
  // "the picture goes oddly bright" was.
  float mean = 0.0, norm = 0.0;
  for (int k = -12; k <= 12; ++k) {
    if (abs(k) > r) continue;
    // Raised cosine over the window, so the ends do not ring.
    float hann = 0.5 + 0.5 * cos(3.14159265 * float(k) / float(r + 1));
    mean += hann * Luma(FetchRgbAt(int2(x + k, row)));
    norm += hann;
  }
  if (norm <= 0.0) return 0.0;
  mean /= norm;

  float acc = 0.0, accQ = 0.0;
  for (int k = -12; k <= 12; ++k) {
    if (abs(k) > r) continue;
    float hann = 0.5 + 0.5 * cos(3.14159265 * float(k) / float(r + 1));
    float l = Luma(FetchRgbAt(int2(x + k, row))) - mean;
    float ph = w * float(x + k);
    acc += hann * l * cos(ph);
    accQ += hann * l * sin(ph);
  }

  float ph0 = w * float(x);
  float pattern = 2.0 * (acc * cos(ph0) + accQ * sin(ph0)) / norm;

  // Subtracting the same amount from all three channels moves brightness and
  // leaves colour alone, which is the half of the picture this belongs to.
  return -pattern;
}

float4 main(VSOut i) : SV_Target {
  // Source coordinates, already the right way up: FetchRgbAt resolves a bottom
  // up layout, so everything downstream of this pass can forget about it.
  int2 p = int2(i.pos.xy);
  float3 rgb = FetchRgbAt(p);

  // Everything that cleans the signal happens here, before any deinterlacer has
  // touched it. That is the entire reason this pass exists. A cleanup that runs
  // afterwards has to work out which source line the pixel in front of it came
  // from, and for a mode that interpolates there is no single answer -- so it
  // subtracts a pattern that pixel never carried, which is not a cleanup but a
  // new artefact. Doing it first removes the question.
  // The two halves have to be told about each other. Both work out how much
  // pattern is present from the *captured* pixels, so where the four frame
  // average has already taken it away, the demodulator would go and subtract it
  // a second time -- putting an inverted copy back. That is why turning both up
  // made the picture worse than either one alone.
  //
  // So the temporal half reports how much of the job it did, and the
  // demodulator only handles the rest. Still picture: the average does it all,
  // at no cost in sharpness. Moving picture: the average steps back and the
  // demodulator takes over.
  float handled = 0.0;
  if (gTemporal > 0.0 && gHistCount >= 3) {
    handled = TemporalGate(p.x, p.y) * gTemporal;
    rgb += TemporalDelta(p.x, p.y) * handled;
  }
  if (gDotNotch > 0.0) rgb += DotDemodDelta(p.x, p.y) * (1.0 - handled);
  if (gChromaSoft > 0) rgb = SoftenChroma(rgb, p.x, p.y);

  // Not clamped: the target is floating point and limited range material
  // legitimately reaches past both ends after expansion.
  return float4(rgb, 1.0);
}
)HLSL";

inline const char* kConvertPS =
R"HLSL(// Pass two: fields, cropping, line doubling and rotation. Reads the cleaned
// picture from pass one rather than the captured planes, so it needs to know
// nothing about pixel formats, frame history or where a pattern sat in a line --
// all of that is already dealt with.
Texture2D<float4> texClean : register(t0);
// The same, one frame earlier. Only YADIF looks at it.
Texture2D<float4> texCleanPrev : register(t1);

cbuffer ConvertCB : register(b0) {
  int   gFormatKind;      // see FormatKind in video_renderer.h
  int   gDeinterlaceMode; // 0 off, 1 bob, 2 bob linear, 3 motion adaptive,
                          // 4 edge directed, 5 yadif
  int   gFieldIndex;      // 0 = first field, 1 = second field
  int   gBottomUp;

  int   gCropLeft;
  int   gCropTop;
  int   gSrcWidth;
  int   gSrcHeight;

  int   gOutWidth;
  int   gOutHeight;
  int   gIsYuv;
  int   gHavePrev;        // previous frame's planes hold a usable picture

  float gYOffset;
  float gYScale;
  float gCScale;
  float gPad1;

  int   gCoSitedPhase;    // -1 normal interlace, else the row a field pair starts on
  int   gRotation;        // 0 none, 1 quarter turn right, 2 half, 3 quarter turn left
  int   gLineDouble;      // 1 = every source line fills two output lines
  int   gChromaSoft;      // horizontal chroma blur radius in source pixels, 0 off

  float gTemporal;        // 0..1 strength of the temporal average
  int   gHistCount;       // how many previous frames actually hold a picture
  float gDotNotch;        // 0..1: width of the demodulation window, 0 = off
  float gCarrierPeriod;   // samples per cycle of the colour subcarrier

  float4 gCoef;           // Cr->R, Cb->G, Cr->G, Cb->B
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 FetchRgbAt(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  return texClean.Load(int3(p, 0)).rgb;
}

float3 FetchRgbPrev(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  return texCleanPrev.Load(int3(p, 0)).rgb;
}

float Luma(float3 c) {
  return dot(c, float3(0.299, 0.587, 0.114));
}

// Brightness with the fine pattern averaged out of it, for deciding *where* an
// edge runs. What is left of the composite shimmer repeats every three pixels
// across a line, and a direction search that scores raw brightness will happily
// lock onto that instead of onto the picture -- it finds a diagonal along which
// the dots line up, and reports it as an edge. The result is the grain that
// shows up on edge directed and, in the modes that use it, on YADIF.
//
// The value of a pixel still comes from the pixel. Only the decision about which
// direction to take it from is made on the smoothed version.
float LumaSmooth(int2 p) {
  return (Luma(FetchRgbAt(int2(p.x - 1, p.y))) + Luma(FetchRgbAt(p)) +
          Luma(FetchRgbAt(int2(p.x + 1, p.y)))) * 0.3333333;
}

// Interpolates a missing line by looking for the direction in which the line
// above and the line below agree, instead of straight down the column. On a
// diagonal edge the vertical average produces a staircase; following the edge
// does not.
float3 EdgeDirected(int x, int rowAbove, int rowBelow) {
  float3 a0 = FetchRgbAt(int2(x, rowAbove));
  float3 b0 = FetchRgbAt(int2(x, rowBelow));
  float3 best = (a0 + b0) * 0.5;

  // Scored over three pixels, not one. A single pair matches by coincidence all
  // over a flat or noisy picture -- dozens of directions come out near zero and
  // the winner is essentially arbitrary, which is exactly the speckle this mode
  // was producing. Three neighbouring pixels have to agree, and noise does not
  // manage that.
  float bestCost = abs(LumaSmooth(int2(x - 1, rowAbove)) - LumaSmooth(int2(x - 1, rowBelow))) +
                   abs(LumaSmooth(int2(x, rowAbove)) - LumaSmooth(int2(x, rowBelow))) +
                   abs(LumaSmooth(int2(x + 1, rowAbove)) - LumaSmooth(int2(x + 1, rowBelow)));

  [unroll]
  for (int d = 1; d <= 3; ++d) {
    // Slanting costs a little, so a genuinely vertical edge is not talked out of
    // being vertical by a marginally better diagonal match. Three terms now, so
    // the charge is three times what it was.
    float penalty = float(d) * 0.018;

    float costR = abs(LumaSmooth(int2(x - 1 + d, rowAbove)) -
                      LumaSmooth(int2(x - 1 - d, rowBelow))) +
                  abs(LumaSmooth(int2(x + d, rowAbove)) - LumaSmooth(int2(x - d, rowBelow))) +
                  abs(LumaSmooth(int2(x + 1 + d, rowAbove)) -
                      LumaSmooth(int2(x + 1 - d, rowBelow))) +
                  penalty;
    if (costR < bestCost) {
      bestCost = costR;
      best = (FetchRgbAt(int2(x + d, rowAbove)) + FetchRgbAt(int2(x - d, rowBelow))) * 0.5;
    }

    float costL = abs(LumaSmooth(int2(x - 1 - d, rowAbove)) -
                      LumaSmooth(int2(x - 1 + d, rowBelow))) +
                  abs(LumaSmooth(int2(x - d, rowAbove)) - LumaSmooth(int2(x + d, rowBelow))) +
                  abs(LumaSmooth(int2(x + 1 - d, rowAbove)) -
                      LumaSmooth(int2(x + 1 + d, rowBelow))) +
                  penalty;
    if (costL < bestCost) {
      bestCost = costL;
      best = (FetchRgbAt(int2(x - d, rowAbove)) + FetchRgbAt(int2(x + d, rowBelow))) * 0.5;
    }
  }

  // The answer still has to be plausible. A pixel interpolated from some
  // diagonal that lies outside the range its own two neighbours span did not
  // come from this edge -- it came from wherever the search wandered off to.
  // Pulling it back inside is the standard safeguard, and it is what removes the
  // gross misses rather than merely making them rarer.
  return clamp(best, min(a0, b0), max(a0, b0));
}

// YADIF's own directional predictor. Where EdgeDirected above compares single
// pixels, this scores a three-pixel window, which is what stops it from latching
// onto a coincidental match in noise -- and it only widens the search to two
// pixels of slant if one pixel already looked better than straight down.
float3 YadifSpatial(int x, int rowAbove, int rowBelow) {
  // Same three pixel window as before, but scored on the smoothed brightness for
  // the same reason as EdgeDirected: an unsmoothed score follows the composite
  // shimmer's own diagonal instead of the picture's.
  float3 pred = (FetchRgbAt(int2(x, rowAbove)) + FetchRgbAt(int2(x, rowBelow))) * 0.5;
  float best = abs(LumaSmooth(int2(x - 1, rowAbove)) - LumaSmooth(int2(x - 1, rowBelow))) +
               abs(LumaSmooth(int2(x, rowAbove)) - LumaSmooth(int2(x, rowBelow))) +
               abs(LumaSmooth(int2(x + 1, rowAbove)) - LumaSmooth(int2(x + 1, rowBelow))) -
               0.004;

  // Slant by j, and only widen to 2j when j already looked better than straight
  // down. The score compares the window on one line against the window on the
  // other, shifted by the slant being tried.
  [unroll]
  for (int side = 0; side < 2; ++side) {
    int j = side == 0 ? -1 : 1;
    float s1 = abs(LumaSmooth(int2(x - 1 + j, rowAbove)) - LumaSmooth(int2(x - 1 - j, rowBelow))) +
               abs(LumaSmooth(int2(x + j, rowAbove)) - LumaSmooth(int2(x - j, rowBelow))) +
               abs(LumaSmooth(int2(x + 1 + j, rowAbove)) - LumaSmooth(int2(x + 1 - j, rowBelow)));
    if (s1 < best) {
      best = s1;
      pred = (FetchRgbAt(int2(x + j, rowAbove)) + FetchRgbAt(int2(x - j, rowBelow))) * 0.5;

      int k = j * 2;
      float s2 = abs(LumaSmooth(int2(x - 1 + k, rowAbove)) -
                     LumaSmooth(int2(x - 1 - k, rowBelow))) +
                 abs(LumaSmooth(int2(x + k, rowAbove)) - LumaSmooth(int2(x - k, rowBelow))) +
                 abs(LumaSmooth(int2(x + 1 + k, rowAbove)) -
                     LumaSmooth(int2(x + 1 - k, rowBelow)));
      if (s2 < best) {
        best = s2;
        pred = (FetchRgbAt(int2(x + k, rowAbove)) + FetchRgbAt(int2(x - k, rowBelow))) * 0.5;
      }
    }
  }
  return pred;
}

// One missing line, reconstructed the way YADIF does it: predict it spatially,
// then refuse to let that prediction stray further from the temporal evidence
// than the surrounding lines say it plausibly could.
//
// The temporal pair is the same line in this frame and in the previous one --
// both belong to the field that is *not* being shown, half a field apart on
// either side of it. Textbook YADIF also looks at the frame after this one,
// which would mean holding every frame back until its successor arrives. In a
// viewer whose entire point is latency that trade is not worth making, so the
// filter runs one-sided; where that costs it, it falls back towards the spatial
// prediction, which is the safe direction to be wrong in.
float3 Yadif(int x, int row, int rowTop, int rowBottom) {
  int rowAbove = clamp(row - 1, rowTop, rowBottom);
  int rowBelow = clamp(row + 1, rowTop, rowBottom);
  float3 spatial = YadifSpatial(x, rowAbove, rowBelow);
  if (gHavePrev == 0) return spatial;

  float3 c = FetchRgbAt(int2(x, rowAbove));
  float3 e = FetchRgbAt(int2(x, rowBelow));

  float3 tPrev = FetchRgbPrev(int2(x, row));   // missing line, one frame ago
  float3 tCur  = FetchRgbAt(int2(x, row));     // missing line, woven into this frame
  float3 d = (tPrev + tCur) * 0.5;

  // How much this part of the picture is moving, measured on the lines we do
  // have as well as on the line we are guessing.
  float3 diff = max(abs(tPrev - tCur) * 0.5,
                    (abs(FetchRgbPrev(int2(x, rowAbove)) - c) +
                     abs(FetchRgbPrev(int2(x, rowBelow)) - e)) * 0.5);

  // Two lines out, same parity as the missing one: the local gradient the
  // reconstruction has to stay inside.
  int rowUp2 = clamp(row - 2, rowTop, rowBottom);
  int rowDn2 = clamp(row + 2, rowTop, rowBottom);
  float3 b = (FetchRgbPrev(int2(x, rowUp2)) + FetchRgbAt(int2(x, rowUp2))) * 0.5;
  float3 f = (FetchRgbPrev(int2(x, rowDn2)) + FetchRgbAt(int2(x, rowDn2))) * 0.5;

  float3 hi = max(max(d - e, d - c), min(b - c, f - e));
  float3 lo = min(min(d - e, d - c), max(b - c, f - e));
  diff = max(max(diff, lo), -hi);

  return clamp(spatial, d - diff, d + diff);
}


float4 main(VSOut i) : SV_Target {
  int2 op = int2(i.pos.xy);

  // Undo the rotation first: everything below works in the picture's own
  // orientation, which is the only one in which "the line above" means anything.
  // The logical picture is the cropped area, twice as tall if lines are doubled;
  // a quarter turn swaps the two axes of the render target but not of this.
  int logicalW = gOutWidth;
  int logicalH = gOutHeight * (gLineDouble != 0 ? 2 : 1);
  int lx, ly;
  if (gRotation == 1) {
    lx = op.y;
    ly = logicalH - 1 - op.x;
  } else if (gRotation == 2) {
    lx = logicalW - 1 - op.x;
    ly = logicalH - 1 - op.y;
  } else if (gRotation == 3) {
    lx = logicalW - 1 - op.y;
    ly = op.x;
  } else {
    lx = op.x;
    ly = op.y;
  }

  int srcX = lx + gCropLeft;

  // Rows of the cropped area, in full-frame coordinates.
  int rowTop = gCropTop;
  int rowBottom = gCropTop + gOutHeight - 1;
  int row = gCropTop + (gLineDouble != 0 ? (ly >> 1) : ly);

  float3 rgb;
  if (gDeinterlaceMode == 0) {
    rgb = FetchRgbAt(int2(srcX, row));
  } else if (gCoSitedPhase >= 0) {
    // Both fields hold the same picture lines, half a field apart in time: a
    // 240p or 288p console the card packed into an interlaced frame. Nothing is
    // spatially missing here, so every mode collapses to the same and only
    // correct answer -- take the line belonging to the field being shown and use
    // it for both rows of its pair. No interpolation, no ghosting, and no
    // vertical step, because the two fields are not offset from each other.
    int base = ((row - gCoSitedPhase) & ~1) + gCoSitedPhase;
    rgb = FetchRgbAt(int2(srcX, clamp(base + gFieldIndex, rowTop, rowBottom)));
  } else if (gDeinterlaceMode <= 2) {
    // Reconstruct a full frame out of the selected field. `t` is the
    // continuous index of that field's own rows.
    float t = (float(row) - float(gFieldIndex)) * 0.5;
    if (gDeinterlaceMode == 1) {
      // floor, not round. Rounding lands on .5 for every second output row, and
      // which way it goes depends on the field -- so the whole picture shifts by
      // a line each time the field changes, sixty times a second. Flooring is
      // half a line low for both fields equally, which is invisible; a line of
      // difference between fields is not.
      int r = int(floor(t)) * 2 + gFieldIndex;
      rgb = FetchRgbAt(int2(srcX, clamp(r, rowTop, rowBottom)));
    } else {
      float fl = floor(t);
      float frac = t - fl;
      int row0 = int(fl) * 2 + gFieldIndex;
      int row1 = row0 + 2;
      float3 a = FetchRgbAt(int2(srcX, clamp(row0, rowTop, rowBottom)));
      float3 b = FetchRgbAt(int2(srcX, clamp(row1, rowTop, rowBottom)));
      rgb = lerp(a, b, frac);
    }
  } else {
    // Modes 3 and 4 keep the frame's own geometry rather than stretching one
    // field over it. A row belonging to the field being shown is used exactly as
    // captured; the rows between them come from the other field, half a field
    // time away, and are only trustworthy where nothing moved.
    if ((row & 1) == gFieldIndex) {
      rgb = FetchRgbAt(int2(srcX, row));
    } else {
      int rowAbove = clamp(row - 1, rowTop, rowBottom);
      int rowBelow = clamp(row + 1, rowTop, rowBottom);
      float3 above = FetchRgbAt(int2(srcX, rowAbove));
      float3 below = FetchRgbAt(int2(srcX, rowBelow));

      if (gDeinterlaceMode == 5) {
        rgb = Yadif(srcX, row, rowTop, rowBottom);
      } else if (gDeinterlaceMode == 4) {
        rgb = EdgeDirected(srcX, rowAbove, rowBelow);
      } else {
        float3 woven = FetchRgbAt(int2(srcX, row));

        // Combing is the woven line sitting outside the range its two
        // neighbours span: bright, dark, bright. The product of the two
        // differences is positive exactly then, and near zero on a smooth
)HLSL"
R"HLSL(        // gradient. Vertical detail that is genuinely in the picture raises the
        // bar, so a static but finely striped image is not read as motion.
        //
        // Measured across three columns rather than one. A composite signal
        // carries enough noise that a single column flickers between weave and
        // interpolate from frame to frame, and that flicker is more visible than
        // the combing it was trying to remove.
        // Measured so that it scales with contrast rather than with its
        // square. The obvious form multiplies the two deviations together, and
        // on soft, anti-aliased material that collapses: against this card the
        // typical comb value came out at 0.00008 with the threshold sitting at
        // 0.00422 -- fifty times too high, so the mode wove essentially
        // everywhere and left the picture interlaced wherever anything moved.
        // Hard pixel edges cleared it and 3D rendering never did.
        //
        // Taking the smaller of the two deviations is linear in contrast and
        // works on both. It also needs no allowance for genuine vertical
        // detail: on a real edge the middle line lies *between* its
        // neighbours, so the deviations have opposite signs and this reads zero
        // by construction. Sweeping the remaining two numbers against frames
        // from the card, with movement established by comparing the same field
        // across frames, 0.003 and 60 reach 47 % of what genuinely moves while
        // touching 4.7 % of what does not.
        float motion = 0.0;
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
          float lw = Luma(FetchRgbAt(int2(srcX + dx, row)));
          float la = Luma(FetchRgbAt(int2(srcX + dx, rowAbove)));
          float lb = Luma(FetchRgbAt(int2(srcX + dx, rowBelow)));
          float da = lw - la;
          float db = lw - lb;
          float comb = (da * db > 0.0) ? min(abs(da), abs(db)) : 0.0;
          motion += saturate((comb - 0.003) * 60.0);
        }
        motion = saturate(motion * 0.3333);

        // Where it does interpolate, follow the edge rather than averaging
        // straight down. That is what was leaving jagged steps on things moving
        // quickly across the picture.
        rgb = lerp(woven, EdgeDirected(srcX, rowAbove, rowBelow), motion);
      }
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
