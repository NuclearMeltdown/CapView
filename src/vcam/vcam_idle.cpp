// The camera's idle picture.
//
// Drawn in the consumer's process, like everything else in this DLL, and drawn
// with GDI because GDI is already in every process that has a window and it is
// the only way to get real text at a size chosen at runtime. The alternative --
// a bitmap font baked into the binary -- would have to be baked at one size and
// would look like a bitmap font blown up at every other.
//
// The result is cached by the caller, so the cost of all this is paid once per
// consumer and per format rather than thirty times a second.

#include "vcam/vcam_idle.h"

#include <windows.h>

#include <strsafe.h>

#include <vector>

#include "vcam/vcam_shared.h"
#include "vcam_resource.h"

namespace cap {
namespace vcam {
namespace {

// Near black with a violet cast, the same ground the mark was drawn against in
// tools/make_icon.py. A flat colour rather than a gradient: this picture is
// going down a video pipe that will compress it, and flat survives that.
const COLORREF kGround = RGB(0x1A, 0x14, 0x2E);
const COLORREF kTitleInk = RGB(0xEC, 0xE9, 0xF5);
const COLORREF kSubtitleInk = RGB(0x9B, 0x93, 0xB0);

// English only, deliberately. The language CapView is set to lives in CapView's
// config, and the one thing that is certain when this picture is on screen is
// that CapView is not there to be asked.
const wchar_t kTitle[] = L"CapView is not running";
const wchar_t kSubtitle[] = L"Start CapView and turn the virtual camera on";

// Something to take the address of, so the DLL can find its own module handle
// without a global that DllMain has to set first.
const char kModuleAnchor = 0;

// ------------------------------------------------------------------- the mark

struct Bgra {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> px;  // top-down, four bytes per pixel

  bool valid() const { return w > 0 && h > 0 && px.size() == (size_t)w * h * 4; }
};

uint16_t Read16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Undoes tools/make_vcam_icon.py: a palette, then runs of indices into it.
Bgra DecodeIcon(const uint8_t* data, size_t size) {
  Bgra out;
  if (!data || size < 12) return out;
  if (data[0] != 'C' || data[1] != 'V' || data[2] != 'I' || data[3] != 'C') return out;

  const int w = Read16(data + 4);
  const int h = Read16(data + 6);
  const int colours = Read16(data + 8);
  const int runs = Read16(data + 10);
  if (w <= 0 || h <= 0 || colours <= 0 || runs <= 0) return out;
  if (size < 12 + (size_t)colours * 4 + (size_t)runs * 4) return out;

  const uint8_t* palette = data + 12;
  const uint8_t* run = palette + (size_t)colours * 4;

  out.px.resize((size_t)w * h * 4);
  size_t at = 0;
  for (int i = 0; i < runs; ++i) {
    const int length = Read16(run + (size_t)i * 4);
    const int index = Read16(run + (size_t)i * 4 + 2);
    if (index >= colours) return Bgra();
    if (at + (size_t)length * 4 > out.px.size()) return Bgra();
    const uint8_t* colour = palette + (size_t)index * 4;
    for (int n = 0; n < length; ++n) {
      out.px[at++] = colour[0];
      out.px[at++] = colour[1];
      out.px[at++] = colour[2];
      out.px[at++] = colour[3];
    }
  }
  if (at != out.px.size()) return Bgra();

  out.w = w;
  out.h = h;
  return out;
}

const Bgra& Icon() {
  // Function-local static: decoded once, and the decoding is thread safe
  // whichever pin gets here first.
  static const Bgra icon = [] {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&kModuleAnchor, &self);
    if (!self) return Bgra();

    HRSRC found = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_VCAM_ICON), RT_RCDATA);
    if (!found) return Bgra();
    HGLOBAL loaded = ::LoadResource(self, found);
    if (!loaded) return Bgra();
    const auto* bytes = (const uint8_t*)::LockResource(loaded);
    return DecodeIcon(bytes, ::SizeofResource(self, found));
  }();
  return icon;
}

// Halves a picture with a box filter. Repeated halving before the bilinear pass
// is what keeps a 256 pixel mark from turning into aliased confetti at 48.
Bgra Halve(const Bgra& src) {
  Bgra out;
  out.w = src.w / 2;
  out.h = src.h / 2;
  if (out.w < 1 || out.h < 1) return src;
  out.px.resize((size_t)out.w * out.h * 4);
  for (int y = 0; y < out.h; ++y) {
    const uint8_t* a = &src.px[(size_t)(y * 2) * src.w * 4];
    const uint8_t* b = &src.px[(size_t)(y * 2 + 1) * src.w * 4];
    uint8_t* d = &out.px[(size_t)y * out.w * 4];
    for (int x = 0; x < out.w; ++x) {
      for (int c = 0; c < 4; ++c) {
        const int sum = a[(size_t)(x * 2) * 4 + c] + a[(size_t)(x * 2 + 1) * 4 + c] +
                        b[(size_t)(x * 2) * 4 + c] + b[(size_t)(x * 2 + 1) * 4 + c];
        d[(size_t)x * 4 + c] = (uint8_t)((sum + 2) / 4);
      }
    }
  }
  return out;
}

Bgra Resample(const Bgra& source, int dstW, int dstH) {
  Bgra src = source;
  while (src.w >= dstW * 2 && src.h >= dstH * 2 && src.w > 1 && src.h > 1) src = Halve(src);

  Bgra out;
  out.w = dstW;
  out.h = dstH;
  out.px.resize((size_t)dstW * dstH * 4);
  if (src.w == dstW && src.h == dstH) {
    out.px = src.px;
    return out;
  }

  const int64_t stepX = ((int64_t)src.w << 16) / dstW;
  const int64_t stepY = ((int64_t)src.h << 16) / dstH;
  const int64_t baseX = (stepX >> 1) - 32768;
  const int64_t baseY = (stepY >> 1) - 32768;
  const int64_t maxX = (int64_t)(src.w - 1) << 16;
  const int64_t maxY = (int64_t)(src.h - 1) << 16;

  for (int y = 0; y < dstH; ++y) {
    int64_t fy = baseY + (int64_t)y * stepY;
    if (fy < 0) fy = 0;
    if (fy > maxY) fy = maxY;
    const int y0 = (int)(fy >> 16);
    const int y1 = y0 + 1 < src.h ? y0 + 1 : y0;
    const uint32_t wy = (uint32_t)(fy & 0xFFFF);
    const uint8_t* row0 = &src.px[(size_t)y0 * src.w * 4];
    const uint8_t* row1 = &src.px[(size_t)y1 * src.w * 4];
    uint8_t* d = &out.px[(size_t)y * dstW * 4];

    for (int x = 0; x < dstW; ++x) {
      int64_t fx = baseX + (int64_t)x * stepX;
      if (fx < 0) fx = 0;
      if (fx > maxX) fx = maxX;
      const int x0 = (int)(fx >> 16);
      const int x1 = x0 + 1 < src.w ? x0 + 1 : x0;
      const uint32_t wx = (uint32_t)(fx & 0xFFFF);
      for (int c = 0; c < 4; ++c) {
        const int32_t p00 = row0[(size_t)x0 * 4 + c];
        const int32_t p01 = row0[(size_t)x1 * 4 + c];
        const int32_t p10 = row1[(size_t)x0 * 4 + c];
        const int32_t p11 = row1[(size_t)x1 * 4 + c];
        const int32_t top = p00 + (int32_t)(((int64_t)(p01 - p00) * wx) >> 16);
        const int32_t bot = p10 + (int32_t)(((int64_t)(p11 - p10) * wx) >> 16);
        d[(size_t)x * 4 + c] = (uint8_t)(top + (int32_t)(((int64_t)(bot - top) * wy) >> 16));
      }
    }
  }
  return out;
}

// Straight alpha over an opaque destination. The mark is half transparent by
// area, so this is most of what makes it look like a mark rather than a tile.
void Composite(const Bgra& mark, uint8_t* dst, int dstStride, int dstW, int dstH, int atX,
               int atY) {
  for (int y = 0; y < mark.h; ++y) {
    const int dy = atY + y;
    if (dy < 0 || dy >= dstH) continue;
    const uint8_t* s = &mark.px[(size_t)y * mark.w * 4];
    uint8_t* d = dst + (size_t)dy * dstStride;
    for (int x = 0; x < mark.w; ++x) {
      const int dx = atX + x;
      if (dx < 0 || dx >= dstW) continue;
      const uint32_t a = s[(size_t)x * 4 + 3];
      if (!a) continue;
      uint8_t* out = d + (size_t)dx * 4;
      for (int c = 0; c < 3; ++c) {
        const uint32_t src = s[(size_t)x * 4 + c];
        out[c] = (uint8_t)((src * a + out[c] * (255 - a) + 127) / 255);
      }
    }
  }
}

// -------------------------------------------------------------------- the text

HFONT MakeFont(int px, int weight) {
  LOGFONTW lf = {};
  lf.lfHeight = -px;  // negative asks for a character height, not a cell height
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfOutPrecision = OUT_TT_PRECIS;
  // Grayscale antialiasing, never ClearType: subpixel fringes are red and blue
  // edges on the letters once this has been through a 4:2:0 chroma pass.
  lf.lfQuality = ANTIALIASED_QUALITY;
  lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
  ::StringCchCopyW(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
  return ::CreateFontIndirectW(&lf);
}

// A font at the largest size that still fits `maxWidth`, so a wide-but-short
// frame does not get its text running off both ends.
HFONT FitFont(HDC dc, const wchar_t* text, int px, int weight, int maxWidth, SIZE* extent) {
  HFONT font = MakeFont(px, weight);
  if (!font) return nullptr;
  HGDIOBJ old = ::SelectObject(dc, font);
  ::GetTextExtentPoint32W(dc, text, (int)::wcslen(text), extent);
  ::SelectObject(dc, old);

  if (extent->cx > maxWidth && extent->cx > 0 && maxWidth > 0) {
    int shrunk = (int)((int64_t)px * maxWidth / extent->cx);
    if (shrunk < 8) shrunk = 8;
    HFONT smaller = MakeFont(shrunk, weight);
    if (smaller) {
      ::DeleteObject(font);
      font = smaller;
      old = ::SelectObject(dc, font);
      ::GetTextExtentPoint32W(dc, text, (int)::wcslen(text), extent);
      ::SelectObject(dc, old);
    }
  }
  return font;
}

void DrawCentred(HDC dc, HFONT font, const wchar_t* text, COLORREF ink, int frameW, int top) {
  HGDIOBJ old = ::SelectObject(dc, font);
  ::SetTextColor(dc, ink);
  RECT r = {0, top, frameW, top + 4096};
  ::DrawTextW(dc, text, (int)::wcslen(text), &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
  ::SelectObject(dc, old);
}

// ------------------------------------------------------------------- to YUV

// BT.709, limited range. The picture is nearly achromatic, so a consumer that
// assumes 601 instead sees the mark shift a little and nothing else.
inline void ToYuv(int b, int g, int r, int* y, int* u, int* v) {
  *y = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
  *u = ((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128;
  *v = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
}

inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void BgraToYuv(const uint8_t* src, int srcStride, int w, int h, uint8_t* dst, uint32_t pixel) {
  const bool wide = pixel == kPixelP010;
  const size_t stride = (size_t)w * BytesPerSample(pixel);
  uint8_t* chroma = dst + stride * h;

  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)y * srcStride;
    uint8_t* row = dst + stride * y;
    for (int x = 0; x < w; ++x) {
      int luma, cb, cr;
      ToYuv(s[(size_t)x * 4 + 0], s[(size_t)x * 4 + 1], s[(size_t)x * 4 + 2], &luma, &cb, &cr);
      // Ten bits sit in the top of the word, and a limited-range eight-bit
      // value scales into a limited-range ten-bit one by four: 16 becomes 64
      // and 235 becomes 940. Four then six places over is eight.
      if (wide) {
        ((uint16_t*)row)[x] = (uint16_t)(Clamp8(luma) << 8);
      } else {
        row[x] = Clamp8(luma);
      }
    }
  }

  // One chroma sample per 2x2 block, averaged in RGB before the conversion
  // rather than after it -- the same thing to within a rounding step, and one
  // conversion instead of four.
  for (int y = 0; y < h / 2; ++y) {
    const uint8_t* s0 = src + (size_t)(y * 2) * srcStride;
    const uint8_t* s1 = src + (size_t)(y * 2 + 1) * srcStride;
    uint8_t* row = chroma + stride * y;
    for (int x = 0; x < w / 2; ++x) {
      int sum[3] = {0, 0, 0};
      for (int c = 0; c < 3; ++c) {
        sum[c] = s0[(size_t)(x * 2) * 4 + c] + s0[(size_t)(x * 2 + 1) * 4 + c] +
                 s1[(size_t)(x * 2) * 4 + c] + s1[(size_t)(x * 2 + 1) * 4 + c];
        sum[c] = (sum[c] + 2) / 4;
      }
      int luma, cb, cr;
      ToYuv(sum[0], sum[1], sum[2], &luma, &cb, &cr);
      if (wide) {
        ((uint16_t*)row)[x * 2 + 0] = (uint16_t)(Clamp8(cb) << 8);
        ((uint16_t*)row)[x * 2 + 1] = (uint16_t)(Clamp8(cr) << 8);
      } else {
        row[x * 2 + 0] = Clamp8(cb);
        row[x * 2 + 1] = Clamp8(cr);
      }
    }
  }
}

}  // namespace

bool RenderIdlePicture(uint8_t* dst, uint32_t width, uint32_t height, uint32_t pixel) {
  if (!dst || width < 2 || height < 2 || (width & 1) || (height & 1)) return false;
  if (width > 16384 || height > 16384) return false;

  const int w = (int)width;
  const int h = (int)height;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down, so a row index is a row index
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC dc = ::CreateCompatibleDC(nullptr);
  if (!dc) return false;
  void* bitsVoid = nullptr;
  HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsVoid, nullptr, 0);
  if (!bmp || !bitsVoid) {
    if (bmp) ::DeleteObject(bmp);
    ::DeleteDC(dc);
    return false;
  }
  HGDIOBJ oldBitmap = ::SelectObject(dc, bmp);

  auto* bits = (uint8_t*)bitsVoid;
  const int stride = w * 4;

  // Ground. Written as words rather than through FillRect so the alpha byte is
  // ours: GDI's text drawing leaves it alone, and nothing downstream reads it,
  // but a DIB full of zero alpha has surprised enough people to be worth a
  // definite value.
  {
    const uint8_t b = GetBValue(kGround), g = GetGValue(kGround), r = GetRValue(kGround);
    const uint32_t packed = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
    auto* first = (uint32_t*)bits;
    for (int x = 0; x < w; ++x) first[x] = packed;
    for (int y = 1; y < h; ++y) ::memcpy(bits + (size_t)y * stride, bits, (size_t)stride);
  }

  const int unit = w < h ? w : h;
  ::SetBkMode(dc, TRANSPARENT);

  // Below about a hundred lines there is no room for a sentence, and below
  // half of that not even for the mark with air around it. Both are far under
  // anything a real consumer asks for, but the pin does advertise down to
  // 32x32 and a frame that is one flat colour says nothing at all.
  const bool room_for_text = h >= 96 && w >= 160;
  const bool room_for_subtitle = h >= 200 && w >= 380;

  int markPx = (int)(unit * (room_for_text ? 0.26 : 0.72));
  if (markPx < 16) markPx = 16;
  if (markPx > 512) markPx = 512;

  HFONT title = nullptr;
  HFONT subtitle = nullptr;
  SIZE titleSize = {0, 0};
  SIZE subtitleSize = {0, 0};
  const int margin = w / 16;

  if (room_for_text) {
    int titlePx = (int)(unit * 0.085);
    if (titlePx < 11) titlePx = 11;
    if (titlePx > 240) titlePx = 240;
    title = FitFont(dc, kTitle, titlePx, FW_SEMIBOLD, w - margin * 2, &titleSize);
    if (room_for_subtitle) {
      int subPx = titlePx * 45 / 100;
      if (subPx < 11) subPx = 11;
      subtitle = FitFont(dc, kSubtitle, subPx, FW_NORMAL, w - margin * 2, &subtitleSize);
    }
  }

  const int markGap = title ? (int)(unit * 0.05) : 0;
  const int textGap = subtitle ? (int)(titleSize.cy * 0.35) : 0;
  int total = markPx + markGap + (int)titleSize.cy + textGap + (int)subtitleSize.cy;
  if (total > h) total = h;
  int top = (h - total) / 2;
  if (top < 0) top = 0;

  const Bgra& mark = Icon();
  if (mark.valid() && markPx > 0) {
    const Bgra scaled = Resample(mark, markPx, markPx);
    if (scaled.valid()) Composite(scaled, bits, stride, w, h, (w - markPx) / 2, top);
  }

  int cursor = top + markPx + markGap;
  if (title) {
    DrawCentred(dc, title, kTitle, kTitleInk, w, cursor);
    cursor += (int)titleSize.cy + textGap;
  }
  if (subtitle) DrawCentred(dc, subtitle, kSubtitle, kSubtitleInk, w, cursor);

  // GDI writes through the DIB's own mapping; nothing may read it until the
  // drawing has actually landed.
  ::GdiFlush();
  BgraToYuv(bits, stride, w, h, dst, pixel);

  if (title) ::DeleteObject(title);
  if (subtitle) ::DeleteObject(subtitle);
  ::SelectObject(dc, oldBitmap);
  ::DeleteObject(bmp);
  ::DeleteDC(dc);
  return true;
}

}  // namespace vcam
}  // namespace cap
