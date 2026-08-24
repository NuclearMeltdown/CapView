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

// Persuades the frame server to let go of whatever media source it has mapped.
//
// This lives here rather than in CapView for one reason: regsvr32 is already
// running elevated when it calls into this file, and stopping a service needs
// exactly those rights. CapView has none and cannot get them without asking a
// second time.
//
// It is needed because a service does not reread a DLL it already has open.
// Naming each build differently stops two of them being mistaken for one
// another, but the service still has to be made to go and look -- and the only
// thing that reliably does that is starting it over. It comes back by itself
// the moment an application next asks for a camera, so there is nothing to
// start here.
void StopFrameServer() {
  SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return;
  SC_HANDLE service =
      ::OpenServiceW(manager, L"FrameServer", SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (service) {
    SERVICE_STATUS status = {};
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int i = 0; i < 40; ++i) {
      if (!::QueryServiceStatus(service, &status)) break;
      if (status.dwCurrentState == SERVICE_STOPPED) break;
      ::Sleep(100);
    }
    ::CloseServiceHandle(service);
  }
  ::CloseServiceHandle(manager);
}

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
  if (status != ERROR_SUCCESS) return SELFREG_E_CLASS;
  StopFrameServer();
  return S_OK;
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
    StopFrameServer();
    return S_OK;
  }
  return SELFREG_E_CLASS;
}
