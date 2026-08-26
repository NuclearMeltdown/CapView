#pragma once

// Resource ids inside the filter DLL -- not inside CapView.exe, which has its
// own res/resource.h. The DLL carries exactly one resource: the CapView mark,
// packed by tools/make_vcam_icon.py, drawn on the picture the camera shows
// while nothing is feeding it.
#define IDR_VCAM_ICON 200
