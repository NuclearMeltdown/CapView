<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">CapView</h1>

<p align="center">
  A low-latency viewer and recorder for DirectShow capture cards on Windows.
</p>

![The viewer showing a console at 1080p60, with the statistics overlay reading a frame age of 1.0 ms](docs/viewer.jpg)

CapView displays the output of a capture card with as little delay as the
hardware allows, so the captured signal can be played on directly rather than
only watched. Measured on a StarTech PEXHDCAP60L: **1080p60 sustained, around
1 ms between a frame arriving from the card and being drawn.**

It is meant for using a capture card to play. Recording, screenshots and a
microphone track are included; scenes, overlays, compositing and streaming are
not. For those, use OBS.

It covers the same ground as AmaRecTV, whose last release with a bundled
recording codec was version 3.10 in 2014.

## Latency

Four decisions account for the measured figure.

- **No queue in the capture path.** The capture filter copies each frame into a
  triple buffer and returns immediately. Frames arriving faster than they can be
  shown are dropped rather than buffered, so delay cannot accumulate.
- **No graph clock.** The DirectShow graph runs without a reference clock, so
  frames are not held back until a presentation time.
- **Flip-model swap chain**, maximum frame latency of one, tearing permitted.
  VSync is off by default.
- **Format conversion on the GPU.** YUY2, UYVY, YVYU, NV12, planar 4:2:0 and RGB
  are unpacked in a pixel shader rather than on the CPU.

## Features

### Source

Any DirectShow video device. Resolution, frame rate, pixel format and colour
space are selected independently. Combinations a driver does not advertise but
does accept can be forced, which covers the common case of a card reporting only
1080p30 for a mode it will in fact deliver at 1080p60.

![The source tab, with the capture device, its embedded audio, and separate pickers for colour format, resolution and frame rate](docs/settings-source.jpg)

### Picture

Nearest, bilinear, Catmull-Rom, Lanczos3 and sharp-bilinear scaling; contrast
adaptive sharpening; bob deinterlacing; aspect override and integer scaling.
Crop is set by dragging the edges on the picture.

Colour range and matrix default to automatic. The range is measured from the
image rather than inferred from the pixel format, because it is a property of
the source signal: a console set to full range delivers full range whether the
card is asked for NV12 or RGB32.

### Audio

The card's embedded audio, or any Windows recording device, played out through
WASAPI. The buffer target is configurable, exclusive mode is optional, and an
A/V offset is available. Drift between the capture and playback clocks is
corrected continuously by adjusting the playback rate by a fraction of a per
cent. Both inputs have level meters.

An optional microphone is recorded as a separate input with its own gain. It is
never played back. By default the file receives three audio tracks: a mix, plus
the capture and microphone separately. Mixed-only and separate-only are also
available.

### Recording

H.264, H.265 or AV1 through NVENC, QuickSync, AMF, x264 or x265, encoded by
ffmpeg. The recording is made from the picture at source resolution, after crop
and deinterlacing and before window scaling, so window size does not affect the
result.

The capture audio serves as the master clock, and the video timeline is derived
from the number of audio samples written. The output is therefore constant frame
rate and does not drift: measured at 1 ms over 15 seconds.

![The recording tab, with container, bitrate, output folder, and an encoder list naming the five that passed the test on this machine](docs/settings-recording.jpg)

### Screenshots

PNG or JPEG at source resolution, taken before the interface is drawn. Written
through Windows Imaging Component, so no ffmpeg is required.

### Profiles

A profile holds the device, input, capture format and all picture and audio
settings. One per source, selected with Ctrl+1 to Ctrl+9.

## Shortcuts

| Key | Action |
|---|---|
| Enter | Fullscreen |
| Esc | Leave fullscreen |
| F1 | Statistics |
| F2 | Settings |
| F5 | Restart capture |
| F9 | Start / stop recording |
| F10 | Screenshot |
| M | Mute |
| `+` / `-` or mouse wheel | Volume |
| Ctrl+1 … Ctrl+9 | Switch profile |
| Right click | Menu |

All of these except Esc, the profile digits and Alt+F4 can be reassigned under
*Settings → Keys*.

The statistics overlay has three levels of detail: frame rates and frame age;
those plus capture format, colour handling and audio buffer; and everything,
including device names and dropped frame counts.

## Building

Requires Visual Studio 2022 with the Desktop C++ workload, and CMake. There are
no external dependencies; Dear ImGui is vendored in `third_party/`.

```bash
build.bat
```

The result is `CapView.exe` in the repository root, around 1.6 MB, linked
against the static CRT. The build tree is removed afterwards, leaving only the
executable. Settings are stored in `CapView.json` beside it; nothing is written
to the registry.

`build.bat keep` retains the build tree for incremental rebuilds, and
`build.bat debug` produces a debug configuration.

Prebuilt executables are attached to each [release](../../releases).

## ffmpeg

Recording requires `ffmpeg.exe`. Nothing else does. *Settings → Recording*
provides a button that downloads a static build, verifies its published SHA-256
and extracts only the executable. The same operation is available from the
command line:

```bash
CapView.exe --fetch-ffmpeg
```

CapView looks for ffmpeg in `ffmpeg\bin` and `ffmpeg` next to the executable,
then beside the executable itself, then on `PATH`. A specific path can be
configured.

Available encoders are determined by test-encoding two frames with each
candidate, rather than by reading `ffmpeg -encoders`, which lists what the build
was compiled with rather than what the hardware supports. The test runs once, on
request; the result is stored in the configuration and reused on subsequent
starts. It is discarded when the graphics adapter or the ffmpeg build changes.

Two automatic modes are offered, because compatibility and file size pull in
opposite directions:

| Mode | Order | Suited to |
|---|---|---|
| Plays anywhere | H.264, H.265, AV1 | Files that open on any phone, television or editor |
| Smaller files | AV1, H.265, H.264 | The same quality at a fraction of the size |

Hardware encoders precede the CPU encoders in both orders. Each mode takes the
first entry of its order that passed the test.

## Why DirectShow

Capture cards that ship a Media Foundation driver also expose a DirectShow
interface, since both sit on the same Kernel Streaming layer. The reverse does
not hold: older and semi-professional cards are frequently DirectShow only. On
the development machine, DirectShow enumerates five video devices where Media
Foundation enumerates three.

A card grants its capture pin to one process at a time. If OBS holds it, CapView
cannot open it, and the other way round.

HDR is not implemented. The colour description is parsed, including the PQ and
HLG transfer functions, but nothing downstream acts on it.

## Licence

CapView is [MIT](LICENSE) licensed. Dear ImGui is MIT licensed as well.

ffmpeg is a separate program, downloaded from upstream and executed as a child
process. The usual Windows builds contain x264 and x265 and are therefore GPL
licensed; invoking a program is not linking against it, so those terms do not
extend to CapView. Redistributing CapView together with an ffmpeg build is a
different matter, and the GPL then applies to what is being distributed.
