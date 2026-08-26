// The DLL wrapper around the DirectShow source filter: what COM needs to find
// it, and what regsvr32 needs to install and remove it.
//
// Two registrations, not one. The CLSID keys are what COM uses to turn a class
// id into this file; the filter mapper entry is what makes the camera appear in
// the device lists applications actually browse. Either one alone gets you a
// camera that exists but nobody can find, or a name in a list that fails to
// open.
//
// It goes to HKLM. Not because a service needs to see it any more -- the Media
// Foundation source this replaces ran in the frame server and that was the
// reason back then -- but because IFilterMapper2 has no root parameter and
// writes where it writes. That is what the elevation prompt on installing the
// camera is for; running it needs nothing.

#include <windows.h>

#include <dshow.h>
#include <olectl.h>
#include <strsafe.h>

#include <string>

#include "vcam/vcam_filter.h"
#include "vcam/vcam_shared.h"

namespace {

HMODULE g_module = nullptr;

std::wstring KeyPath(const wchar_t* clsid, const wchar_t* suffix) {
  std::wstring path = L"Software\\Classes\\CLSID\\";
  path += clsid;
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

// Removes a class and its InprocServer32. The child goes first: RegDeleteKeyW
// will not remove a key that still has subkeys, and a half-deleted class points
// at a file that is about to go away.
void DeleteClass(const wchar_t* clsid) {
  ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyPath(clsid, L"\\InprocServer32").c_str());
  ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyPath(clsid, nullptr).c_str());
}

// Everything CapView 2.x left in the registry.
//
// That version registered a Media Foundation source, which is a different
// mechanism end to end: a different class id, and a name under the frame
// server's list of virtual cameras rather than an entry in the DirectShow
// filter mapper. Neither is touched by installing this one, so both are removed
// by hand -- otherwise the old camera keeps appearing in device lists, pointing
// at a DLL that no longer exists, and every application that tries it gets an
// error instead of a picture.
void RemoveLegacyRegistration() {
  DeleteClass(cap::vcam::kLegacySourceClsidString);

  // The frame server's list. Entries are named after the camera, and CapView
  // 2.x made one called "CapView Virtual Camera".
  HKEY cameras = nullptr;
  const wchar_t kVirtualCameras[] =
      L"SOFTWARE\\Microsoft\\Windows Media Foundation\\Platform\\VirtualCameras";
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVirtualCameras, 0, KEY_READ | KEY_WRITE, &cameras) ==
      ERROR_SUCCESS) {
    ::RegDeleteTreeW(cameras, cap::vcam::kFilterName);
    ::RegCloseKey(cameras);
  }
}

// Registers or removes the entry the device enumerator reads.
HRESULT MapFilter(bool add) {
  IFilterMapper2* mapper = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, (void**)&mapper);
  if (FAILED(hr)) return hr;

  if (!add) {
    hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, cap::vcam::kFilterName,
                                  cap::vcam::CLSID_CapViewFilter);
    mapper->Release();
    return hr;
  }

  // The subtype is left open. What the pin actually offers depends on what
  // CapView has in front of it right now, and pinning a list here would be a
  // second, staler answer to a question the pin already answers properly.
  REGPINTYPES types = {};
  types.clsMajorType = &MEDIATYPE_Video;
  types.clsMinorType = &MEDIASUBTYPE_NULL;

  REGFILTERPINS pins = {};
  pins.strName = const_cast<LPWSTR>(L"Capture");
  pins.bRendered = FALSE;
  pins.bOutput = TRUE;
  pins.bZero = FALSE;
  pins.bMany = FALSE;
  pins.clsConnectsToFilter = &CLSID_NULL;
  pins.strConnectsToPin = nullptr;
  pins.nMediaTypes = 1;
  pins.lpMediaType = &types;

  REGFILTER2 filter = {};
  filter.dwVersion = 1;
  // A capture device is chosen, never auto-connected into. Any merit above
  // MERIT_DO_NOT_USE would invite the graph builder to wire this camera into
  // arbitrary graphs on its own, which is how a virtual camera ends up in a
  // recording nobody asked to put it in.
  filter.dwMerit = MERIT_DO_NOT_USE;
  filter.cPins = 1;
  filter.rgPins = &pins;

  hr = mapper->RegisterFilter(cap::vcam::CLSID_CapViewFilter, cap::vcam::kFilterName, nullptr,
                              &CLSID_VideoInputDeviceCategory, cap::vcam::kFilterName, &filter);
  mapper->Release();
  return hr;
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
  if (clsid != cap::vcam::CLSID_CapViewFilter) return CLASS_E_CLASSNOTAVAILABLE;
  return cap::vcam::CreateFilterClassFactory(riid, out);
}

STDAPI DllCanUnloadNow() { return cap::vcam::LiveObjectCount() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH] = {};
  if (::GetModuleFileNameW(g_module, path, MAX_PATH) == 0) return SELFREG_E_CLASS;

  const wchar_t* clsid = cap::vcam::kFilterClsidString;
  LONG status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(clsid, nullptr), nullptr,
                           cap::vcam::kFilterName);
  if (status == ERROR_SUCCESS) {
    status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(clsid, L"\\InprocServer32"), nullptr, path);
  }
  if (status == ERROR_SUCCESS) {
    // "Both" because a consumer may create this from either kind of apartment
    // and the filter is written to be safe in either.
    status = WriteValue(HKEY_LOCAL_MACHINE, KeyPath(clsid, L"\\InprocServer32"),
                        L"ThreadingModel", L"Both");
  }
  if (status == ERROR_ACCESS_DENIED) return E_ACCESSDENIED;
  if (status != ERROR_SUCCESS) return SELFREG_E_CLASS;

  // regsvr32 has already initialised COM for this thread, but the camera is
  // also installed from CapView's own elevated helper, so this does not assume.
  const HRESULT initialised = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const HRESULT hr = MapFilter(true);
  if (SUCCEEDED(hr)) RemoveLegacyRegistration();
  if (SUCCEEDED(initialised)) ::CoUninitialize();

  if (FAILED(hr)) {
    DeleteClass(clsid);
    return hr == E_ACCESSDENIED ? E_ACCESSDENIED : SELFREG_E_CLASS;
  }
  return S_OK;
}

STDAPI DllUnregisterServer() {
  const HRESULT initialised = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const HRESULT hr = MapFilter(false);
  RemoveLegacyRegistration();
  if (SUCCEEDED(initialised)) ::CoUninitialize();

  DeleteClass(cap::vcam::kFilterClsidString);

  // Already gone counts as removed: uninstalling twice is not a failure, and
  // neither is uninstalling something that a previous attempt half removed.
  if (hr == E_ACCESSDENIED) return E_ACCESSDENIED;
  return S_OK;
}
