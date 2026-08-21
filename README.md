<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">CapView</h1>

<p align="center">
  A low-latency viewer and recorder for DirectShow capture cards on Windows.
</p>

![The viewer showing a console at 1080p60, with the statistics overlay reading a frame age of 1.0 ms](docs/viewer.jpg)

Point it at your capture card and it puts the picture on screen with as little
delay as the hardware allows — so you can play on the captured signal instead of
watching it.

Measured on a StarTech PEXHDCAP60L: **1080p60 sustained, ~1 ms between a frame
arriving and being drawn.** That is the number in the corner of the screenshot
above, and it is what the whole program is arranged around.

Written for people who use a capture card to play, not to stream. OBS overkill; this is lightweight. If you know AmaRecTV, it is the same idea: the card's
picture, as directly as the hardware permits, with recording attached and
nothing else in the way.

## Why it is fast

- **The frame is never queued.** The capture filter copies into a triple buffer
  and returns. A late frame is dropped, not backed up — a queue is just latency
  with extra steps.
- **No graph clock.** The DirectShow graph runs without a reference clock, so
  nothing is held back waiting for its presentation time.
- **Flip-model swapchain, one frame of latency, tearing allowed.** VSync off is
  the single biggest lever there is, and it is the default.
- **Decoding happens on the GPU.** YUY2, UYVY, YVYU, NV12, planar 4:2:0 and RGB
  are unpacked in a shader, not on the CPU.

## What it does

**Source** — any DirectShow video device. Resolution, frame rate, pixel format
and colour space are picked separately, like OBS. Combinations the driver does
not advertise but does support (the classic "1080p60 shows up as 1080p30" case)
can be forced.

![The source tab, with the capture device, its embedded audio, and separate pickers for colour format, resolution and frame rate](docs/settings-source.jpg)

**Picture** — nearest, bilinear, Catmull-Rom, Lanczos3 and sharp-bilinear
scaling, adaptive sharpening, bob deinterlacing, aspect override, integer
scaling. Crop edges are dragged on the picture itself rather than typed in.
Colour range and matrix are measured from the picture and can be overridden.

**Audio** — the card's embedded audio or any Windows input, played out over
WASAPI with a configurable buffer, optional exclusive mode, and an A/V offset.
Drift between the two clocks is corrected continuously. Level meters on both
inputs, so you can see there is signal before you record ten minutes of silence.

**Microphone** — an optional second input that goes into the recording and
nowhere else; it is never played back, which would only be an echo. Its own
gain, on top of whatever Windows is doing. By default the file gets three
tracks: a mix that plays in anything, plus game and microphone separately for
editing. Mixed-only and separate-only are both a setting away.

**Recording** — H.264, H.265 or AV1 through NVENC, QuickSync, AMF or x264/x265,
via ffmpeg. Records the picture at source resolution; window size is irrelevant.
The capture audio is the master clock, so the result is constant frame rate and
stays in sync (measured: 1 ms drift over 15 seconds).

![The recording tab, with container, bitrate, output folder, and an encoder list naming the five that passed the test on this machine](docs/settings-recording.jpg)

**Screenshots** — PNG or JPEG at source resolution, without the interface,
written through Windows' own imaging stack. No ffmpeg needed.

**Profiles** — device, input, format and every picture and audio setting in one
bundle. Set one up per console, switch with Ctrl+1…9.

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

Statistics come in three levels: frame rates and frame age, plus format and
audio, plus everything.

Everything except Esc, the profile digits and Alt+F4 can be rebound under
*Settings → Keys*.

## Building

Needs Visual Studio 2022 (Desktop C++) and CMake. No other dependencies —
Dear ImGui is vendored in `third_party/`.

```bash
build.bat
```

The result is a single self-contained `CapView.exe` in the repository root,
about 1.6 MB, statically linked against the CRT. Everything else the build
produced is deleted afterwards, so what is left is the program and nothing
else. Settings live in `CapView.json` next to it; nothing is written to the
registry.

`build.bat keep` leaves the build tree in place for incremental rebuilds, and
`build.bat debug` builds a debug configuration.

Or take a build from [Releases](../../releases).

## ffmpeg

Recording needs `ffmpeg.exe`; nothing else does. *Settings → Recording* has a
button that fetches a static build, verifies its SHA-256 and keeps only the
executable. On the command line:

```bash
CapView.exe --fetch-ffmpeg
```

CapView looks in `ffmpeg\bin` and `ffmpeg` next to the executable, then beside
it, then on `PATH`. A custom path can be set.

Encoders are established by test-encoding two frames each, not by reading
`ffmpeg -encoders`: that list says what was compiled in, not what the hardware
can do. The test runs once, on request, and the result is kept in the config —
so the encoder list offers what this machine can actually do, rather than every
vendor's hardware encoder. It is discarded and asked for again when the
graphics adapter or the ffmpeg build changes.

There are two automatic modes, because the two things you might want from an
encoder are in direct conflict:

| Mode | Order | For |
|---|---|---|
| Plays anywhere | H.264, H.265, AV1 | Files that open on any phone, TV or editor |
| Smaller files | AV1, H.265, H.264 | Same quality at a fraction of the size |

Hardware comes before the CPU encoders in both. Neither scores anything: each
walks its order and takes the first entry that passed the test.

## Why DirectShow

Every capture card that ships a Media Foundation driver also exposes a
DirectShow interface, because both sit on the same Kernel Streaming layer.
The reverse is not true — older and semi-professional cards are DirectShow only.
On the development machine DirectShow finds five video devices where Media
Foundation finds three.

A card hands its capture pin to one process at a time. If OBS has it, CapView
cannot open it, and the other way round.

HDR is deliberately not implemented. The colour description is already parsed,
including the PQ and HLG transfer functions, but nothing downstream acts on it.

## Licence

CapView is [MIT](LICENSE) licensed. Dear ImGui is MIT as well.

ffmpeg is a separate program, downloaded from upstream and run as a child
process. The usual Windows builds contain x264 and x265 and are therefore GPL;
calling a program is not linking to it, so none of that reaches CapView. If you
redistribute CapView bundled with an ffmpeg build, the GPL applies to what you
are shipping.
