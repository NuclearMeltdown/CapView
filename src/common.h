#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Short alias -- ComPtr shows up on almost every line of the DirectShow / D3D code.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

namespace cap {

// ---------------------------------------------------------------- string utils

std::string ToUtf8(const std::wstring& w);
std::wstring ToWide(const std::string& s);

// Uppercase ASCII copy, used for case-insensitive id/name matching.
std::string ToUpper(std::string s);

// Trims ASCII whitespace from both ends.
std::string Trim(const std::string& s);

std::string Format(const char* fmt, ...);

// ---------------------------------------------------------------------- logging

// Writes to the debugger and, if enabled, to CapView.log next to the exe.
void LogInit(bool toFile);
void LogWrite(const char* level, const char* fmt, ...);

#define CAP_LOG(...)  ::cap::LogWrite("INFO", __VA_ARGS__)
#define CAP_WARN(...) ::cap::LogWrite("WARN", __VA_ARGS__)
#define CAP_ERR(...)  ::cap::LogWrite("ERR ", __VA_ARGS__)

// Logs the call site and returns hr, so it can be used inline in a condition.
HRESULT LogHrFailure(HRESULT hr, const char* expr, const char* file, int line);

#define CAP_HR(expr) ::cap::LogHrFailure((expr), #expr, __FILE__, __LINE__)

// Human readable HRESULT, e.g. "0x80070002 (Das System kann die Datei nicht finden)".
std::string HrToString(HRESULT hr);

// ------------------------------------------------------------------ misc utils

// Directory containing the running executable, with trailing backslash.
std::wstring ExeDirectory();

// What this build calls itself. Compared against the newest release tag on
// GitHub, so it has to line up with how those are named -- "v1.1" there against
// "1.1" here.
//
// Set by CMake from project(CapView VERSION ...), which is also where the
// version resource in the executable comes from. Deliberately no fallback: a
// default here would be a second place the number can live, and the point is
// that there is only one.
#ifndef CAPVIEW_VERSION
#error "CAPVIEW_VERSION comes from CMake -- configure the build rather than compiling by hand."
#endif
inline const char* kAppVersion = CAPVIEW_VERSION;

// Creates a directory and every missing parent. True when it exists afterwards.
bool EnsureFolder(const std::wstring& path);

template <typename T>
T Clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// RAII wrapper for CoInitializeEx on the calling thread.
class ComScope {
 public:
  explicit ComScope(DWORD model = COINIT_APARTMENTTHREADED);
  ~ComScope();
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace cap
