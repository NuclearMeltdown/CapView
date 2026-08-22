// The DLL wrapper around the media source: what COM needs to find it, and what
// regsvr32 needs to install and remove it.
//
// Registration has to go to HKLM, not HKCU. The Frame Server and the Frame
// Server Monitor are services and load this under their own accounts, so a
// per-user registration would simply not be visible to them. That is the whole
// reason installing the camera asks for administrator rights and running it
// does not.

#include <windows.h>

#include <olectl.h>
#include <strsafe.h>

#include <string>

#include "vcam/vcam_shared.h"
#include "vcam/vcam_source.h"

namespace {

HMODULE g_module = nullptr;

const wchar_t kFriendlyName[] = L"CapView Virtual Camera Source";

std::wstring KeyPath(const wchar_t* suffix) {
  std::wstring path = L"Software\\Classes\\CLSID\\";
  path += cap::vcam::kSourceClsidString;
  if (suffix) path += suffix;
  return path;
}

LONG WriteValue(HKEY root, const std::wstring& subKey, const wchar_t* name,
                const std::wstring& value) {
  HKEY key = nullptr;
  LONG status = ::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return status;
  status = ::RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return status;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    ::DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (clsid != cap::vcam::CLSID_CapViewSource) return CLASS_E_CLASSNOTAVAILABLE;
  return cap::vcam::CreateSourceClassFactory(riid, out);
}

STDAPI DllCanUnloadNow() { return cap::vcam::LiveObjectCount() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH] = {};
  if (::GetModuleFileNameW(g_module, path, MAX_PATH) == 0) return SELFREG_E_CLASS;

  LONG status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(nullptr), nullptr, kFriendlyName);
  if (status == ERROR_SUCCESS) {
    status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(L"\\InprocServer32"), nullptr, path);
  }
  if (status == ERROR_SUCCESS) {
    // "Both" because the frame server may create this from either kind of
    // apartment, and the object is written to be safe in either.
    status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(L"\\InprocServer32"), L"ThreadingModel",
                        L"Both");
  }
  if (status == ERROR_ACCESS_DENIED) return E_ACCESSDENIED;
  return status == ERROR_SUCCESS ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
  // Delete the child first: RegDeleteKeyW will not remove a key that still has
  // subkeys, and leaving InprocServer32 behind would leave the class half
  // registered and pointing at a file that is about to go away.
  const std::wstring inproc = KeyPath(L"\\InprocServer32");
  LONG inner = ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, inproc.c_str());
  LONG outer = ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyPath(nullptr).c_str());

  if (inner == ERROR_ACCESS_DENIED || outer == ERROR_ACCESS_DENIED) return E_ACCESSDENIED;
  // Already gone counts as removed -- uninstalling twice is not a failure.
  if ((inner == ERROR_SUCCESS || inner == ERROR_FILE_NOT_FOUND) &&
      (outer == ERROR_SUCCESS || outer == ERROR_FILE_NOT_FOUND)) {
    return S_OK;
  }
  return SELFREG_E_CLASS;
}
