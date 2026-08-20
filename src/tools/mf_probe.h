#pragma once

// Media Foundation device listing for the diagnostic tool.
//
// Purely a comparison against what DirectShow reports: CapView itself captures
// through DirectShow, and this exists to answer "is there hardware the
// DirectShow path cannot see?" with a list rather than a guess.

namespace cap {

// Prints MF video and audio capture devices, and for each video device the
// formats MF reports. Opening a device that another program holds fails
// harmlessly and is reported as such.
void PrintMediaFoundationDevices();

}  // namespace cap
