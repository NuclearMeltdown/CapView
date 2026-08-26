#pragma once

// The DirectShow source filter behind CapView's virtual camera.
//
// This half does not run inside CapView. DirectShow creates it inside whichever
// application opened the camera, so there is one of these per consumer, each
// negotiating its own format and each living exactly as long as that
// application keeps the camera open. It takes pictures out of the shared
// section described in vcam_shared.h and knows nothing else about CapView.
//
// Written against the raw interfaces rather than the DirectShow base classes.
// Those ship as sample source rather than a library in current SDKs, and
// building them would mean carrying a few thousand lines of somebody else's
// code to save a few hundred of ours.

#include <windows.h>
#include <unknwn.h>

namespace cap {
namespace vcam {

// The COM class DirectShow instantiates. Matches kFilterClsidString.
// {A326E6EC-3F70-468B-A826-4F9D42CB5C8E}
extern const CLSID CLSID_CapViewFilter;

// Hands out Filter instances. Returned by DllGetClassObject.
HRESULT CreateFilterClassFactory(REFIID riid, void** out);

// How many objects are alive, so DllCanUnloadNow can answer.
long LiveObjectCount();
void AddLiveObject();
void ReleaseLiveObject();

}  // namespace vcam
}  // namespace cap
