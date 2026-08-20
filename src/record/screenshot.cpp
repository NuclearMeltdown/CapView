#include "record/screenshot.h"

#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "i18n.h"

namespace cap {
namespace {

bool FileExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

bool SaveScreenshot(const std::wstring& path, const uint8_t* pixels, int width, int height,
                    ScreenshotFormat format, int jpegQuality, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = what;
    return false;
  };

  if (!pixels || width <= 0 || height <= 0) return fail(T("Kein Bild.", "No picture."));

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CAP_HR(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))))) {
    return fail(T("WIC nicht verfügbar.", "WIC not available."));
  }

  ComPtr<IWICStream> stream;
  if (FAILED(CAP_HR(factory->CreateStream(&stream))) ||
      FAILED(CAP_HR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))) {
    return fail(T("Datei konnte nicht angelegt werden.", "Could not create the file."));
  }

  const GUID container =
      format == ScreenshotFormat::Jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;

  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(CAP_HR(factory->CreateEncoder(container, nullptr, &encoder))) ||
      FAILED(CAP_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))) {
    return fail(T("Encoder konnte nicht erstellt werden.", "Could not create the encoder."));
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(CAP_HR(encoder->CreateNewFrame(&frame, &props)))) {
    return fail(T("Bild konnte nicht angelegt werden.", "Could not create the frame."));
  }

  if (format == ScreenshotFormat::Jpeg && props) {
    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = (float)std::max(1, std::min(100, jpegQuality)) / 100.0f;
    props->Write(1, &option, &value);
  }

  if (FAILED(CAP_HR(frame->Initialize(props.Get()))) ||
      FAILED(CAP_HR(frame->SetSize((UINT)width, (UINT)height)))) {
    return fail(T("Bild konnte nicht angelegt werden.", "Could not create the frame."));
  }

  // 24 bit BGR on purpose. Both the PNG and the JPEG encoder take it natively,
  // so WIC never silently substitutes a different layout behind our back -- and
  // a screenshot of opaque video has no use for an alpha channel. SetPixelFormat
  // is only a request, so the result is checked rather than trusted.
  WICPixelFormatGUID wanted = GUID_WICPixelFormat24bppBGR;
  if (FAILED(CAP_HR(frame->SetPixelFormat(&wanted)))) {
    return fail(T("Pixelformat abgelehnt.", "Pixel format rejected."));
  }
  if (wanted != GUID_WICPixelFormat24bppBGR) {
    return fail(T("Pixelformat abgelehnt.", "Pixel format rejected."));
  }

  // RGBA in, BGR out: drop alpha and swap the two outer channels.
  const UINT stride = (UINT)(((size_t)width * 3 + 3) & ~(size_t)3);
  std::vector<uint8_t> row((size_t)stride, 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = pixels + (size_t)y * (size_t)width * 4;
    for (int x = 0; x < width; ++x) {
      row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 2];  // B
      row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];  // G
      row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 0];  // R
    }
    if (FAILED(CAP_HR(frame->WritePixels(1, stride, (UINT)row.size(), row.data())))) {
      return fail(T("Schreiben fehlgeschlagen.", "Writing failed."));
    }
  }

  if (FAILED(CAP_HR(frame->Commit())) || FAILED(CAP_HR(encoder->Commit()))) {
    return fail(T("Datei konnte nicht abgeschlossen werden.", "Could not finalise the file."));
  }
  return true;
}

std::wstring DefaultScreenshotFolder() {
  PWSTR pictures = nullptr;
  std::wstring folder;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)) && pictures) {
    folder = pictures;
    ::CoTaskMemFree(pictures);
  }
  if (folder.empty()) folder = ExeDirectory();
  if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
  return folder + L"CapView";
}

std::wstring MakeScreenshotPath(const std::wstring& folder, ScreenshotFormat format) {
  std::wstring dir = folder;
  if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  if (!EnsureFolder(dir)) return {};

  const wchar_t* ext = format == ScreenshotFormat::Jpeg ? L".jpg" : L".png";

  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t stamp[64];
  swprintf_s(stamp, L"CapView_%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

  // Someone holding the key down produces several shots inside one second, and
  // silently overwriting the earlier ones would be the wrong answer.
  std::wstring candidate = dir + L"\\" + stamp + ext;
  for (int n = 2; FileExists(candidate) && n < 1000; ++n) {
    candidate = dir + L"\\" + stamp + L"_" + std::to_wstring(n) + ext;
  }
  return candidate;
}

}  // namespace cap
