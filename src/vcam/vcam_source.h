#pragma once

// The media source behind CapView's virtual camera.
//
// This half does not run inside CapView. Windows loads it into the Frame Server
// service when an application opens the camera, and the service asks it for
// pictures. It takes those out of the shared section described in
// vcam_shared.h, and knows nothing else about CapView.

#include <windows.h>
#include <mfidl.h>
#include <unknwn.h>

namespace cap {
namespace vcam {

// The COM class Windows instantiates. Matches kSourceClsidString.
// {A1E4F2C7-6B3D-4A58-9E21-7C0D5B8F3A46}
extern const CLSID CLSID_CapViewSource;

// Hands out VCamSource instances. Returned by DllGetClassObject.
HRESULT CreateSourceClassFactory(REFIID riid, void** out);

// How many objects are alive, so DllCanUnloadNow can answer.
long LiveObjectCount();
void AddLiveObject();
void ReleaseLiveObject();

}  // namespace vcam
}  // namespace cap
