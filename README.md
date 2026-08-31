<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">CapView</h1>

<p align="center">
  A low-latency viewer and recorder for DirectShow capture cards on Windows.
</p>

![The viewer in fullscreen showing a console at 1080p60, with the statistics overlay reading a frame age of 1.2 ms](docs/viewer.jpg)

CapView displays the output of a capture card with as little delay as the
hardware allows, so the captured signal can be played on rather than only
watched. Measured on a StarTech PEXHDCAP60L: **1080p60 sustained, around 1 ms
from a frame arriving to the present that hands it to the compositor** — the
whole stretch CapView is answerable for, with nothing before or after it
quietly folded in.

It is meant for using a capture card to play. Recording, screenshots, a
microphone track and a virtual camera are included; scenes, overlays,
compositing and streaming are not. For those, use OBS. It covers the same ground
as AmaRecTV, whose last release with a bundled recording codec was version 3.10
in 2014.

> **The [wiki](../../wiki) is the detailed documentation** — one page per
> feature, covering what the code does, why it works that way, and what was
> measured to arrive at it.

## Latency

Four decisions account for the measured figure.

- **No queue in the capture path.** The capture filter copies each frame into a
  triple buffer and returns immediately. Frames arriving faster than they can be
  shown are dropped rather than buffered, so delay cannot accumulate.
- **No graph clock.** The DirectShow graph runs without a reference clock, so
  frames are not held back until a presentation time.
- **Flip-model swap chain**, maximum frame latency of one, tearing permitted.
  VSync is off by default.
- **Format conversion on the GPU.** YUY2, UYVY, YVYU, NV12, planar 4:2:0, RGB
  and P010 are unpacked in a pixel shader rather than on the CPU.

Details: [Latency](../../wiki/Latency).

## Features

### Settings follow the source

The settings live in a window of their own by default, with its own Direct3D
device, so it can be sized freely or moved to a second monitor. An embedded
panel remains under **Display**, because a window capture in OBS cannot see a
second window.

Their contents follow the source. Before a device is chosen there is nothing but
the device picker; afterwards, controls that cannot apply are absent rather than
disabled, and are not applied to the picture either:

| | Shown when |
|---|---|
| Native pixel grid, composite filter | the source is analogue |
| Scanlines and CRT mask | the source is 576 lines or fewer, analogue or not |
| Deinterlacing | the source has fields at all |
| HDR source curve and source peak | the source is not analogue |

The test is the picture, not the socket: no analogue standard produces more than
576 lines, so a hybrid card delivering more is treated as digital however its
decoder is reported.

The first run opens on a welcome screen rather than on the settings.

### Source

Any DirectShow video device. Resolution, frame rate, pixel format and colour
space are selected independently, so combinations a driver does not advertise
but does accept can be forced — which covers the common case of a card reporting
only 1080p30 for a mode it will in fact deliver at 1080p60.

Cards with an analogue decoder also expose their **video standard** — PAL,
PAL-60, NTSC, SECAM and the rest. It matters on a console that does both 50 and
60 Hz: PAL is 625 lines at 50, PAL-60 is 525 at 60, and the wrong one gives
either no picture or one with the wrong number of lines. Leaving it on
**automatic** is the intended way to run it; see below. The standard belongs to
the analogue decoder, so it is dropped whenever the input it described goes
away, and a source declared Digital never gets one.

**Configure card** opens the driver's own property pages while the picture keeps
running. **Reinitialise card** releases the card, finds it again and returns the
standard and format to automatic, leaving the device and input alone — enough,
after moving a card from composite to DVI, to have 1920×1080 at 60 found on its
own.

Whether anything is coming in is measured from the pixels, not from whether
frames arrive: an analogue card with nothing connected keeps delivering frames,
and no signal on composite is snow rather than black.

More: [Source and signal](../../wiki/Source-and-signal),
[Signal detection](../../wiki/Signal-detection).

![The Source tab: the capture device, the analogue video standard set to PAL 60, and the decoder reporting no lock because nothing was connected when this was taken](docs/settings-source.png)

### Finding the video standard

On **automatic**, the standard is not chosen once at startup but kept correct
for as long as the program runs. Switching a console from 50 to 60 Hz, or
unplugging the cable and putting it back, is followed without touching the
settings.

It takes two stages, because the decoder can answer only half the question.

**The lock** settles the line count and the timing. Candidates are tried in
turn, and the list is walked twice: a fast pass giving each 0.6 s, and — only if
that finds nothing — a patient one at the full deadline. A standard with the
wrong line count never locks at all, it only sits out its deadline, and in a
list of eight that is most of the time the search spends. Nothing is discarded
for good, so a slow lock the fast pass misses is caught by the second, and the
overlay names which pass is running. The standard that was last good and its
50/60 Hz partner are tried first and given longer in the patient pass, since a
console that is restarting needs a moment before anything stable comes out of
it. A
candidate whose line count does not match what is arriving delivers no frames at
all — that is not evidence against it, so it is not discarded, but the program
stops waiting on it and moves on. A full pass with nothing locking means the
console is off, and the card is left alone for a few seconds before looking
again. In practice a lock is found well under a second.

**The colour round** settles the rest. Several standards lock equally well on
the same line count and differ only in how colour is carried — PAL B, PAL N and
SECAM all fit 625 lines, and the decoder reports a lock for the wrong one just
as confidently as for the right one. So each remaining candidate is set in turn
and the picture itself is measured: how much colour is present, and how far
black areas are tinted. The winner has to be clearly ahead rather than merely
ahead, and standards that carry colour identically are not tried twice, since
nothing in the picture could tell them apart.

Two of them cannot be separated that way at all. PAL 60 and NTSC 4.43 carry
colour on the same subcarrier and differ only in that PAL reverses the phase on
every other line — so the wrong one of the pair does not decode nonsense, it
decodes *less*, and measuring amounts rewards it for the omission. The round
therefore also measures the reversal directly: colour that flips from line to
line on one axis and not the other is a decoder working against the signal, and
that standard is dropped from the comparison rather than scored in it.

Each candidate takes only as long as it actually needs. Rather than giving every
one of them a fixed moment to settle — which means giving all of them whatever
the slowest one requires — the round watches the measurement itself: while the
decoder is still locking the reading climbs, and once it stands the candidate is
done. On the hardware this was built against that is a third of a second for
most standards and about twice that for PAL 60, which recovers its colour
noticeably more slowly than the rest.

Most of the time none of that happens: a standard that already shows confident
colour with black that stays black is simply the answer, and nothing is switched
at all. From a deliberately wrong standard to a confirmed right one takes 6.3
seconds on the hardware this was built against, half of it the single
measurement that confirms the result.

A round is only as good as the picture it ran on, so two cases are refused
rather than believed. If the scene changes while the round is running the
comparison is void and repeated a second later — what was missing there is
stillness, and waiting longer does not supply it. If the screen is essentially
black there is nothing to compare at all, since black is black in every
standard; the program waits for a picture instead, and waiting costs nothing it
would need later. A scene that is genuinely grey when decoded correctly cannot
decide this either, and is not made to: the measurement is repeated after 3 and
6 seconds and then left alone. Until it decides, the picture runs on in the
standard that matches the configured region.

While either stage is running, the picture says which step of how many is on
screen and what set the search off — colour too pale for a standard that could
be right, colour reaching into the blacks, or no lock at all. The standards are
switched in front of you either way; the line is there so that what you are
watching has a reason attached to it.

The search can also be **asked for**: **F7**, or *Detect video standard* in the
right-click menu, runs both stages again even when the current standard holds
and shows plenty of colour. That is the case none of these measurements covers —
colour that is wrong rather than missing — and the only route to it used to be
picking a standard by hand and setting it back to automatic. A search asked for
this way **says what it found** when it stops: the standard, and which of its
ways out it took — corrected by colour, confirmed, nothing to decide with, no
signal at all.

Every step is written to `CapView.log` with what was measured and why it was
enough, so a wrong decision can be read back rather than guessed at.

More: [Automatic video standard](../../wiki/Automatic-video-standard).

### Picture

Nearest, bilinear, Catmull-Rom, Lanczos3 and sharp-bilinear scaling; contrast
adaptive sharpening; aspect override and integer scaling; rotation in quarter
turns; line doubling for 240p and 288p sources.

Crop is dragged on the picture, or found by **Detect** — in the settings, in the
right-click menu, or on **F8** — which measures the black border as a union
across about two seconds so a fade to black is not read as the picture getting
smaller, and refuses when too little would survive: a GameCube home screen
leaves 37 % of the area, against 57 % for the widest border that is still a
border.

A crop is a count of source pixels, so it stops meaning anything the moment the
source changes size. When that happens the crop is **dropped** and a notice says
so, rather than being scaled into a guess or silently cutting the wrong edge off
a 480-line picture measured on a 576-line one. It is not re-measured
automatically either: the moment a standard changes is the worst moment to
measure, since the picture is usually a logo on black or nothing at all.

A console that keeps changing between 50 and 60 Hz would then want its border
measured twice over, every time. **Remember per picture size** keeps one crop
per source size instead, so a PAL GameCube's 576-line crop and its 480-line one
both stay set up and the switch puts the right one back. Off by default, because
dropping the crop is the right answer for a source that has genuinely become
something else.

Colour range and matrix default to automatic, the range measured from the image
rather than inferred from the pixel format — a console set to full range
delivers full range whether the card is asked for NV12 or RGB32.

A **native pixel grid** setting resolves every output pixel to the console's own
rather than to a fraction of one. A card samples the line at a fixed rate,
usually 720, while a SNES draws 256 pixels across it, so the boundaries land
wherever the arithmetic puts them; told the real count, the grid comes back. It
recovers the *grid*, not the detail that grid carried — only sampling at the
console's own dot clock does that, and nothing downstream of a capture card can.

Optional **scanlines** and a **shadow mask**, off by default and display only,
each compensating its own brightness so the sliders change structure rather than
exposure. Scanlines switch off below twice the source height, where there is
nowhere to put a gap.

More: [Scaling and sharpening](../../wiki/Scaling-and-sharpening),
[Cropping and geometry](../../wiki/Cropping-and-geometry),
[Colour range and matrix](../../wiki/Colour-range-and-matrix).

### Deinterlacing

Whether the source is interlaced is measured rather than believed — a media type
is entitled to say so and frequently does not. The figures are vertical movement
between consecutive frames, measured on a 480i console:

| Mode | Vertical movement | |
|---|---|---|
| Off (weave) | none | combing on anything that moves |
| Bob | **1.0 line** | full rate, no latency, no interpolation |
| Bob interpolated | 0.56 | the alternation between sharp and interpolated lines |
| Motion adaptive | **0.005** | weaves what is still, interpolates what is not |
| Edge directed | 0.69 | follows edges; meant for pixel art |
| YADIF | **0.002** | best quality; keeps one frame in memory |

![Left: a 480i GameCube frame woven, with combing across the moving item boxes. Right: the same source through YADIF, clean](docs/deinterlace-before-after.png)

More: [Deinterlacing](../../wiki/Deinterlacing).

### Composite

A composite signal carries colour and brightness on one wire, and the two leak
into each other: dot crawl along colour edges, rainbow shimmer over fine detail.

Two controls address the dot crawl and hand over to each other. A **four-frame
average** removes it wherever the picture stands still, at no cost in sharpness;
a **synchronous demodulator** handles what is moving, reconstructing the colour
subcarrier out of the brightness and subtracting it at some cost in horizontal
sharpness. The averaging engages whenever the demodulator is used, so it never
pays for what is already free. Its slider snaps to the steps that actually
change the window width — nine on PAL — because the positions in between compute
the same filter.

**Avoid ghosting** decides where the averaging lets go of movement. Averaging
across movement is smearing, so this moves the trade rather than removing it:
held on late, slow low-contrast movement drags a trail; released early, moving
edges stay clean and slow areas keep some crawl for the demodulator.

![Left: a GameCube over composite with the filter off, dot crawl speckling the gold laurel and the chequered flag. Right: the same frame with the filter on](docs/composite-before-after.png)

Colour shimmer is handled separately, by averaging the colour sideways — which
composite carries at a quarter of the bandwidth anyway. Each neighbour is
weighted by how close its colour is to the centre's; an unweighted average
across a colour edge turns complementary neighbours into grey.

The subcarrier frequency follows from the video standard, so the Source tab
matters here too. SECAM is not handled by the demodulator; the averaging still
applies.

More: [The composite filter](../../wiki/The-composite-filter).

![The Picture tab: scaling and sharpening, the deinterlacer, the composite filter with its two
controls, and the crop with its Detect button](docs/settings-picture.png)

### High dynamic range

P010 and P016 sources are read against either PQ (ST 2084) or HLG (BT.2100) and
turned into linear light. An ordinary screen gets BT.2390 tone mapping; an HDR
screen gets scRGB.

Recording, screenshots and the virtual camera each get the tone mapped picture
by default, and each can be told to keep the range instead — ten bit P010 for a
recording, JPEG XR or AVIF for a screenshot, ten bit P010 for the camera.

More: [High dynamic range](../../wiki/High-dynamic-range).

![The HDR tab: source curve, what goes to the display, paper white and source peak, and the three
switches for keeping the range — greyed out here, because the source was SDR](docs/settings-hdr.png)

### Audio

The card's embedded audio, or any Windows recording device, played out through
WASAPI. The buffer target is configurable, exclusive mode is optional, and an
A/V offset is available. Drift between the capture and playback clocks is
corrected by nudging the playback rate by a fraction of a per cent.

An optional microphone is recorded as a separate input with its own gain, and is
never played back. By default the file gets three tracks: a mix, plus the
capture and microphone separately.

More: [Audio](../../wiki/Audio).

### Recording

H.264, H.265 or AV1 through NVENC, Quick Sync, AMF, x264 or x265, encoded by
ffmpeg. The recording is made at source resolution, after crop and deinterlacing
and before window scaling, so window size does not affect the result.

The capture audio serves as the master clock and the video timeline is derived
from the number of audio samples written, so the output is constant frame rate
and does not drift: measured at 1 ms over 15 seconds.

Frame size and rate stand fixed in the encoder's command line for the length of
a file, so a console switched from 60 to 50 Hz while recording no longer fits
the file being written. When that happens the recording is **cut and continued
in a new file** at the new shape, rather than stopping at the change or filling
the rest of the file with frames that do not match its header. A standard search
walks through several line counts on its way to an answer, so the cut waits
until the source has held one shape for a moment — a search that ends where it
started does not cut at all.

More: [Recording](../../wiki/Recording).

### Screenshots

Taken at source resolution, after crop and deinterlacing and before window
scaling. The grab happens **before the interface is drawn**, so no overlay,
toolbar or settings panel reaches the file. **Include the interface** moves it to
after, saving the finished window instead — window-sized rather than
source-sized, and always SDR.

**SDR: PNG or JPEG.** Both go through Windows Imaging Component, so **no ffmpeg
is needed**. PNG is the default; JPEG has an adjustable quality.

**HDR: JPEG XR or AVIF.** When the source is HDR, a screenshot can keep the
range instead of being mapped down to SDR first. The two are a genuine trade
rather than a preference:

| | Needs | Read by |
|---|---|---|
| **JPEG XR** (`.jxr`) | nothing — Windows ships the encoder | the Windows Photos app; little else |
| **AVIF** (`.avif`) | **ffmpeg**, with libaom | every browser, and most things that are not Windows |

AVIF is the only part of screenshots that needs ffmpeg. Without it the setting
says so and points at the download, rather than failing at the moment you press
the key. The still is encoded as 10-bit AV1 on the PQ curve, tagged BT.2020 —
without those tags a viewer reads the samples as ordinary SDR and shows a dark
picture.

Whether HDR stills are written at all is a switch of its own under **HDR**,
alongside the equivalents for recordings and the virtual camera.

More: [Screenshots](../../wiki/Screenshots).

### Encoder settings

Rate control, preset, tuning, look-ahead, adaptive quantisation and multipass
are exposed under one set of names and translated into each vendor's own.
Everything defaults to automatic, which passes nothing at all rather than
passing the encoder's default. Anything a given encoder has no opinion about is
greyed out rather than hidden.

**Bitrate and quality sit directly under rate control**, which is what decides
which of the two counts; whichever is not in use is greyed out rather than
hidden. The bitrate slider is logarithmic, because the range a card like this
actually lands in — 1 to 10 Mbit — is nine percent of a linear scale that runs
to 100, and a field beside it takes a typed number for when the slider is close
but not right.

More: [Encoder settings](../../wiki/Encoder-settings).

![The Encoder tab: ffmpeg at the top, then which encoder — naming the five that passed the test on
this machine and the four that did not — and the settings it is given](docs/settings-encoder.png)

### Virtual camera

The picture can be offered to other programs as a webcam called **CapView
Virtual Camera**, at the source's own resolution and the source's own frame
rate. Not a list of sizes: a 240p SNES goes out as 240p, a 1080p60 Switch as
1080p60, and if you ever put 8K at 120 in front of it, that is what comes out.

Programs that cannot take that get one of the ordinary sizes below it -- 640x480
and the rest -- scaled and letterboxed inside their own process, at no cost to
anything else reading the same camera. Every consumer negotiates for itself, and
the settings page lists them by name while they read.

Nothing above the source is offered. A camera that advertises more than it has
is a camera that misleads: the program picks the largest entry, keeps that
choice for as long as it holds the camera, and goes on listing it after the
console has changed. So a 576i console offers 576i and smaller, and a program
wanting 1080p from it upscales at its own end, where that work belongs.

An HDR source is additionally offered as ten bit P010, with the eight bit form
right behind it so that programs which have never heard of an HDR webcam still
find something they understand.

The cost is a one-time install with a UAC prompt, because a DirectShow filter is
registered machine-wide. There is an uninstall button next to it.

Being registered machine-wide also means the camera stays in every device list
once installed, whether CapView is running or not -- the same as OBS's virtual
camera. While nothing is feeding it, it shows a picture that says so instead of
black.

A program settles its format once, when it opens the camera, and keeps it for as
long as it holds it open. Swapping a 1080p console for a 576i one changes what
CapView publishes straight away, but a program already reading goes on asking
for the size it negotiated, so it keeps getting the new picture fitted into the
old shape. Reopening the camera there picks the new size up.

**Whatever program you read the camera with, leave its resolution on automatic
and do not pick a size by hand.** In OBS that is **Resolution/FPS Type: Device
Default**; on *Custom* it asks for the size written in the box and nothing else,
whatever the console is now doing. After changing console, disable the device
and enable it again and OBS picks the new size up. Discord needs no more than
the camera off and back on.

More: [Virtual camera](../../wiki/Virtual-camera).

![The Recording tab: container, bitrate, frame rate and output folder, with screenshots and the virtual camera below them](docs/settings-recording.png)

### One profile per console

A profile holds **everything**: the device, the card input, the video standard,
the capture format, and every picture and audio setting. Ctrl+1 to Ctrl+9 switch
between them. This is how the program is meant to be used, and it is worth
setting up before anything else.

Almost nothing carries over between consoles. A SNES over composite wants the
four-frame average and the demodulator against dot crawl, a native width of 256,
PAL at 625 lines and 50 Hz; a Switch over HDMI wants none of that and 1080p at
60. Set both up once and swapping a cable is one keystroke rather than a tour of
the settings.

**Save current as …** turns whatever is set up right now into a profile, asking
for a name with the cursor already in the field, so a second console is a matter
of changing what is actually different rather than rebuilding what was already
correct.

Settings that do not apply to the current source are **not applied**, not merely
hidden. The values stay in the profile — that console will be back — but they
are not restored on the way out and back, since the next analogue source may
well be a different console.

A profile can also say **which video standard means it**. Give the PAL profile
PAL and the NTSC one NTSC, and the standard the search settles on picks the
profile: the same cable, two consoles, and neither a keystroke nor a menu. It
fires when the colour round has settled rather than when the lock comes in,
because until then the standard is only a line count — and never during a
recording, never onto a profile on a different input, and never away from a
profile that already answers to what is on screen. A profile chosen by hand
stays until the source really changes.

### Updates

*Settings → Updates* compares this build against the newest release on GitHub,
either at startup or on request. Installing replaces `CapView.exe` itself, by
renaming rather than overwriting, and a failed update leaves the program as it
was rather than gone.

More: [Updates](../../wiki/Updates).

## Shortcuts

| Key | Action |
|---|---|
| Enter | Fullscreen |
| Esc | Leave fullscreen |
| F1 | Statistics |
| F2 | Settings |
| F5 | Restart capture |
| Shift+F5 | Reinitialise card |
| F7 | Detect video standard |
| F8 | Detect border |
| F9 | Start / stop recording |
| F10 | Screenshot |
| M | Mute |
| `+` / `-` or mouse wheel | Volume |
| Ctrl+1 … Ctrl+9 | Switch profile |
| Right click | Menu |

All of these except Esc, the profile digits and Alt+F4 can be reassigned under
*Settings → Keys*.

More: [Shortcuts](../../wiki/Shortcuts),
[The settings window](../../wiki/The-settings-window).

## Building

Requires Visual Studio 2022 with the Desktop C++ workload, and CMake. There are
no external dependencies; Dear ImGui is vendored in `third_party/`.

```bash
build.bat
```

The result is `CapView.exe` in the repository root, about 2 MB, linked
against the static CRT. `build.bat keep` retains the build tree for incremental
rebuilds, and `build.bat debug` produces a debug configuration.

Settings are stored in `CapView.json` beside the executable; nothing is written
to the registry. Prebuilt executables are attached to each
[release](../../releases).

More: [Building](../../wiki/Building).

## ffmpeg

Two things require `ffmpeg.exe`, and nothing else does:

- **Recording**, whichever encoder is used.
- **HDR screenshots in AVIF**, which go through libaom. The other HDR format,
  JPEG XR, does not — Windows ships that encoder — so an HDR still can be saved
  without ffmpeg by choosing it instead. SDR screenshots never need it.

The preview, the composite filters, deinterlacing and the virtual camera run
without it.

*Settings → Encoder* provides a button that downloads a static build, verifies
its published SHA-256 and extracts only the executable. The same is available
from the command line:

```bash
CapView.exe --fetch-ffmpeg
```

Available encoders are determined by test-encoding two frames with each
candidate, rather than by reading `ffmpeg -encoders`, which lists what the build
was compiled with rather than what the hardware supports.

More: [ffmpeg](../../wiki/ffmpeg).

## Limitations

- **A card grants its capture pin to one process at a time.** If OBS holds it,
  CapView cannot open it, and the other way round.
- **The virtual camera is not visible to packaged apps.** Its shared memory
  lives in the session namespace, which an app container cannot see -- so the
  Windows Camera app and Store builds of Teams do not find it. Everything that
  loads DirectShow normally does: OBS, Discord, browsers, vMix, XSplit.
- **The HDR display path is untested on real HDR hardware.** The maths is
  checked against the standards and the tone mapped path is verified; the scRGB
  output has never been run against an HDR monitor.
- **SECAM dot crawl** is only handled by the temporal half of the composite
  filter.

## Why DirectShow

Capture cards that ship a Media Foundation driver also expose a DirectShow
interface, since both sit on the same Kernel Streaming layer. The reverse does
not hold: older and semi-professional cards are frequently DirectShow only. On
the development machine, DirectShow enumerates five video devices where Media
Foundation enumerates three.

## Licence

CapView is [MIT](LICENSE) licensed. Dear ImGui is MIT licensed as well.

ffmpeg is a separate program, downloaded from upstream and executed as a child
process. The usual Windows builds contain x264 and x265 and are therefore GPL
licensed; invoking a program is not linking against it, so those terms do not
extend to CapView. Redistributing CapView together with an ffmpeg build is a
different matter, and the GPL then applies to what is being distributed.
