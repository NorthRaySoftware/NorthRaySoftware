# NorthRay Capture: architecture

A livestreaming and recording application for Windows in the NorthRay suite, built to be a real alternative to OBS
Studio. This document is the plan the code follows: the systems, what each owns, how data flows, the decisions taken
and why, and the milestones. It's updated at every milestone with what was actually built and tested.

## 0. Decisions (14 November 2026)

| Decision | Choice | Why |
|---|---|---|
| UI toolkit | **cjui** (the suite's Win32 + Direct2D toolkit), not Qt 6 | CJ chose this (14 Nov). Identical look and behaviour to Files, Browser, Studio and the Launcher; a few MB of RAM and no extra DLLs; no LGPL dynamic-linking obligations; builds in the suite's MinGW pipeline. Cost: docking, splitters and layouts are built into cjui/Capture rather than taken from Qt. |
| Composition | **Direct3D 11** on one device shared with capture, preview and encoding | Textures never leave the GPU between capture → compose → encode. D3D11 is supported by the GTX 960 class and by every Windows 11 PC; D3D12 adds complexity with no gain at 1080p60. |
| Preview drawing | The D3D11 compositor's output shown in a DXGI swap chain child window; cjui (Direct2D) draws the chrome around it | The preview costs one copy on the GPU, never a CPU readback. Low-resource mode lowers the preview's rate or turns it off; it never touches the output. |
| Screen and window capture | **Windows.Graphics.Capture** (WGC), with **DXGI Desktop Duplication** as the monitor fallback | WGC captures windows (even covered ones) and monitors with a yellow border it can turn off on Windows 11; Duplication is the fastest monitor path and works where WGC isn't allowed. MinGW ships `windows.graphics.capture.h`. |
| Game capture | **No graphics-API hooking at first.** WGC window capture of the game, plus Duplication | Hooking (as OBS's game capture does) injects code into games, trips anti-cheat systems and needs per-API maintenance. WGC handles windowed and borderless games well on Windows 11. Hooking is a separate, opt-in decision for later, discussed with CJ first. |
| Cameras and capture cards | **Media Foundation** source reader (`IMFSourceReader`, `MFEnumDeviceSources`) | Built into Windows; gives NV12/YUY2/MJPEG frames, uploaded into D3D11 textures. UVC capture cards (Elgato, AVerMedia and the like) are cameras to Windows. |
| Device audio | **WASAPI** shared mode, event-driven; loopback for "desktop audio" | Standard, low-latency, no drivers. |
| Application audio | **Process loopback capture** (`ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`) | Captures one process tree's sound, with no virtual cable. Windows 10 build 20348+ / Windows 11 (the target). One stream per process tree. Its format is fixed by the caller (it has no mix format), so Capture asks for 48 kHz float stereo. The SDK header `audioclientactivationparams.h` is missing from MinGW, so its two small structs are declared in Capture. |
| Encoding | **Media Foundation transforms** (H.264/HEVC; hardware MFTs: NVIDIA NVENC, AMD AMF, Intel Quick Sync, enumerated with `MFTEnumEx(MFT_ENUM_FLAG_HARDWARE)`), Microsoft's software H.264 MFT as the fallback. AAC from the Windows AAC encoder MFT | Hardware encoders without shipping vendor SDKs or FFmpeg; D3D11 textures go straight into hardware MFTs (`IMFDXGIDeviceManager`). Only encoders actually found are offered. If quality or control proves short (B-frames, look-ahead, precise CBR), the NVENC/AMF SDK headers are the next step (they're MIT-style licensed and need no DLLs beyond the driver's); FFmpeg is not planned (size, licence: x264 is GPL). |
| Muxing | Capture's own **FLV** (for RTMP), **fragmented MP4** and **MKV** writers | Small, well-documented formats. Fragmented MP4/MKV keep everything written so far playable after a crash or power loss; "remux to plain MP4" runs afterwards. |
| Streaming | Capture's own **RTMP** client over Winsock, **RTMPS** through **SChannel** (Windows' TLS) | No OpenSSL or librtmp. RTMP is simple and fully specified; SChannel is what Windows uses for TLS anyway. |
| Multistreaming | One encode, **fan-out** of the same packets to each destination's own queue and connection | Encoding once is the efficiency win; per-destination queues, reconnection and drop policies keep one failing platform from touching the others. Destinations needing different settings get their own encoder. |
| Virtual camera | Windows 11 **`MFCreateVirtualCamera`** with Capture's own media source (a COM DLL) | The supported Windows 11 API; no kernel driver. Later milestone. |
| Settings | JSON (versioned, read defensively, saved atomically), like the other apps; scene collections as their own JSON files | Portable, diffable, easy to back up. |
| Secrets | **Windows Credential Manager** (`CredWriteW`, generic credentials, per destination) | Stream keys never sit in JSON files or logs. |
| Logs | Plain text, daily files, size-capped; every line passes `redact()` | Exportable diagnostics with no stream keys, tokens or URLs' secret parts. |

## 1. Systems

```
                       ┌──────────────────────────── UI thread (cjui) ─────────────────────────────┐
                       │ workspace: top bar · preview · scenes · sources · mixer · controls · status│
                       └───────────────▲──────────────────────────────▲─────────────────────────────┘
                                       │ snapshots / commands          │ meters (lock-free)
 ┌──────────────┐  textures  ┌─────────┴────────┐  frames   ┌─────────┐ │ packets ┌──────────────┐
 │ video capture ├──────────▶│  compositor (D3D11)├────────▶│ encoders ├─┼───────▶│ outputs:      │
 │ WGC · DXGI ·  │            │ scenes · filters  │ NV12    │ MFT H.264│ │        │ record (MKV/  │
 │ MF cameras ·  │            │ transitions ·     │         │ AAC      │ │        │ fMP4) · stream│
 │ images · text │            │ canvases (16:9,   │         └─────────┘ │        │ × N (RTMP/S) ·│
 └──────────────┘            │ 9:16)             │                     │        │ replay buffer │
 ┌──────────────┐  PCM 48k   └───────────────────┘                     │        │ · virtual cam │
 │ audio capture ├─────────────────▶┌───────────────┐  stream mix  ──────┘        └──────────────┘
 │ WASAPI dev ·  │                  │ mixer/router   │  rec tracks 1–6
 │ loopback ·    │                  │ filters · buses│  monitor ───────▶ WASAPI render (headphones)
 │ process loop. │                  └───────────────┘
 └──────────────┘
```

| # | System | Responsibility | Lives in |
|---|---|---|---|
| 1 | Application & UI | Window, workspace, panels, layouts, dialogs, hotkeys; never on the media path | `src/ui/` (cjui) |
| 2 | Video capture | One `VideoSource` per capture (WGC, Duplication, camera, image, text…); delivers GPU textures; shared between scenes | `src/video/` |
| 3 | Audio capture | One `AudioSource` per device, loopback or process; 48 kHz float, timestamped on the audio clock | `src/audio/` |
| 4 | Scene management | Scenes, items, transforms, groups, nesting, collections; pure data | `core/scenes` |
| 5 | Compositor | Renders a scene (or a transition between two) to each canvas at the output rate | `src/video/compositor` |
| 6 | Mixer & routing | Per-source gain/filters, the routing matrix to buses (Stream, Recording tracks 1–6, Monitor), meters, leak analysis | `core/routing` (rules) + `src/audio/mixer` |
| 7 | Encoders | Video (MFT, hardware or software) and audio (AAC MFT); an encoder is shared by every output with the same settings | `src/encode/` |
| 8 | Stream outputs | RTMP/RTMPS connection, FLV packaging, buffering, reconnection, stats | `src/outputs/rtmp*` |
| 9 | Recording | MKV / fragmented MP4 writers, segmentation, remux, disk-space watch | `src/outputs/record*` |
| 10 | Multistream manager | Destinations, which encoder feeds which output, per-destination start/stop, bandwidth estimate | `core/multistream` + `src/outputs/` |
| 11 | Effects & filters | Video (colour, keying, crop, blur, LUT; HLSL) and audio (gate, compressor, EQ, limiter, delay) | `src/video/filters`, `core/dsp` |
| 12 | Creative assets | Overlay designer, titles, timers, media library | later, `src/creative/` |
| 13 | Profiles & config | Settings, profiles, scene collections, credentials | `core/config`, `src/config` |
| 14 | Plugins | Versioned C ABI, off by default, per-plugin approval | later |
| 15 | Diagnostics | Logs with redaction, CPU/GPU/memory, frame timing, dropped frames, routing diagnostics view | `core/log`, `src/diag/` |
| 16 | Automation | Hotkeys first; rules (timer → scene) later | `core/automation` |
| 17 | Recovery & stability | Autosave, safe start (skips a source that crashed last time), device reconnection, recording recovery | throughout |

`core/` is portable C++ with no Windows calls: the rules (scenes, routing, leak analysis, bandwidth sums, log
redaction, filenames, layouts) are unit-tested on every platform, as in the other NorthRay apps. `src/` is the Windows
layer.

## 2. Threads and timing

- **UI thread**: cjui window; reads snapshots, sends commands. Minimising, dragging or a modal menu never stalls media.
- **Video thread**: a high-resolution waitable timer at the output rate (e.g. 60 Hz) drives the compositor; it takes
  each source's *latest* texture (sources never block it). Frame times are on the QPC clock.
- **Capture threads**: WGC delivers on its own thread-pool; Duplication and cameras get one thread each.
- **Audio thread**: event-driven WASAPI reads into per-source ring buffers; the mixer runs in 10 ms blocks on the audio
  clock; video and audio timestamps are both QPC so recording and streams stay in sync (with per-source offsets for
  capture cards).
- **Encoder threads**: one per encoder; outputs get packets through bounded queues.
- **Output threads**: one per destination (network) and one per recording (disk).
- Everything crossing threads goes through bounded, lock-light queues; a slow consumer drops by policy (stream: drop
  non-reference frames, then whole GOPs; recording: never drop, warn instead), never blocks capture.

## 3. Application audio: the design that prevents leaks

The brief's key case: stream Minecraft and the microphone, keep Spotify and Discord private.

- **Sources**: `Application audio` (a process tree, chosen from the running apps that have audio sessions, matched by
  executable name so it reconnects when the app restarts), `Desktop audio` (a device's loopback: everything), `Input
  device` (microphone, capture card audio).
- **Session ≠ capturable unit.** `IAudioSessionManager2` lists sessions per process; process loopback captures a
  *process tree*. Capture maps sessions to their top-most process that owns the tree (e.g. Chrome's audio service is a
  child of the browser process; Discord has several), shows the app once, and explains when an app plays sound through
  a different process than the one chosen.
- **Routing matrix** (`core/routing`): each source → {Stream, Recording track 1–6, Monitor}, with volume/mute per
  source applied before routing. Muting in Capture never touches the app's own volume in Windows.
- **Leak analysis** (pure, tested): warns when
  1. a desktop-audio source routed to the stream would carry an app that's *excluded* (not routed to the stream) — the
     "Spotify leaks through desktop audio" case — with the fix offered: replace desktop audio with
     *desktop audio except these apps* (process loopback in EXCLUDE mode, one process tree), or with app sources;
  2. two sources capture the same process (doubling, phasing);
  3. a source is monitored to the device it's captured from (feedback), or desktop audio is monitored at all;
  4. a microphone and a monitored output share a device in a way that echoes.
- **Routing diagnostics view**: per source, the path (process → capture → filters → buses → outputs) and any warning.
- **Dual-PC**: application audio only exists for processes on this PC; the capture card's audio is one mixed stream.
  The setup wizard says so and explains routing separate audio from the gaming PC (e.g. a second audio interface).

## 4. Multistreaming and bandwidth

`Destination { platform, server, key (Credential Manager), encoder profile, enabled }`. Destinations with equal
encoder settings share one encoder; the rest get their own. Each destination: queue, connection, reconnect policy
(back-off 2, 4, 8… up to 60 s, configurable), status (connecting, live, reconnecting n, failed: reason), bitrate,
dropped frames, duration. Starting or stopping one never touches the others. The bandwidth estimate is the sum of
distinct video+audio bitrates per destination plus ~5% protocol overhead, shown against a measured upload speed.

Platform ingest (checked again at Milestone 6/7): Twitch `rtmp://<region>.contribute.live-video.net/app` (RTMPS
available), YouTube `rtmp://a.rtmp.youtube.com/live2` and `rtmps://a.rtmps.youtube.com:443/live2`, Kick an RTMPS URL
from its dashboard, plus custom RTMP/RTMPS. H.264 + AAC, CBR, 2-second keyframes is the common ground.

## 5. Low resource use (and the i7-3770K / GTX 960 target)

- GPU-only video path (no CPU readback except for the software encoder fallback).
- Sources update only when their content changes (WGC/duplication deliver on change; images and text render once).
- The compositor skips work for hidden items and for canvases nobody outputs.
- Preview: 30 fps by default, its own setting, off in low-resource mode; never affects output.
- Meters at 30 Hz with peak-hold computed on the audio thread.
- The GTX 960 (Maxwell, 2nd-gen NVENC) encodes H.264 (and HEVC 8-bit) in hardware; 1080p60 H.264 is within its
  NVENC's capability, but composition at 1080p60 with several sources on 2 GB VRAM needs measuring, not assuming.
  Defaults on that machine: 1080p30 or 720p60, measured at Milestone 12 against OBS under the same scene.
- Low-resource mode: preview off or 10 fps, meters at 10 Hz, no animations, no thumbnails; output untouched.

## 6. Configuration on disk

`%LOCALAPPDATA%\NorthRay\Capture\`: `settings.json` (app), `profiles\<name>.json` (encoding, outputs, routing),
`scenes\<collection>.json`, `layouts\<name>.json`, `logs\capture-YYYY-MM-DD.log`. Stream keys: Credential Manager
targets `NorthRayCapture/<destination id>`. Recordings: the user's Videos folder by default.

## 7. Milestones

As in the brief (1 Foundation → 14 Release), with application audio in Milestone 3 and routing in 4. Each milestone
ends with: what was built, what was actually tested (and how), what isn't done, what's next.

## 8. Milestone 1: Foundation (status: done, 14 Nov 2026)

**Built**
- `NorthRayCapture.exe` (MinGW cross-build, cjui + Direct2D), single instance (a second launch brings
  the first forward), crash report, per-monitor DPI, app icon and manifest.
- The workspace: night-sky top bar (profile and scene-collection names, Layout, Settings, About),
  the preview area (the canvas at its shape, an honest note that live preview arrives in M2), four
  docked panels in a row (Scenes, Sources, Audio mixer, Controls), a status bar.
- Docking: the divider between preview and panels and the dividers between panels drag; a panel's
  header drags it left or right; right-click a header to move or hide it; Layout menu shows/hides
  panels and the preview, saves named layouts, loads and deletes them, resets. All kept in settings.
- Scenes (fully working): add, rename (F2, double-click, menu), duplicate, move up/down, delete
  (asks; at least one stays), Ctrl+1..9 and Up/Down to switch, Ctrl+N. Saved atomically to
  `%LOCALAPPDATA%\NorthRay\Capture\scenes\<collection>.json`, shortly after each change and on
  close; an unreadable file is kept aside as `.unreadable`, never overwritten.
- Sources "+" lists every source type, disabled, labelled with the milestone it arrives in. The
  mixer and controls panels say plainly what isn't built yet.
- Settings page: theme (as Windows / light / dark), low-resource mode, preview frame rate, canvas
  (1080p, 720p, 1440p, vertical), output frame rate, the encoders found on this PC, logs folder,
  copy diagnostics (redacted), About.
- Status bar: process and system CPU, memory, the first hardware H.264/HEVC encoder found; GPU
  says it's measured from M2.
- Encoder discovery through `MFTEnumEx` on a worker thread (hardware vs software, vendor).
- Logging: daily files, 8 MB cap, 14 days kept, every line through `redact()`.
- No ambient twinkle in Capture (cjui `setAmbientAllowed(false)`): the idle window repaints only for
  the once-a-second (twice in low-resource mode) status sample.

**Tested**
- capturecore: 64 tests pass (scenes, routing/leak analysis, redaction, layout).
- In Wine at 100% and 150%: start-up, scene add / rename / duplicate / right-click menu, divider
  drag, hiding a panel from Layout, settings page and its scrolling, dark theme, low-resource
  toggle, the sources menu, clean shutdown, and everything coming back after a restart.

**Not tested / limits**
- Not run on real Windows yet. Wine renders with llvmpipe, so its CPU and memory figures say nothing
  about real hardware; Wine's Media Foundation lists no encoders, so the encoder list has only been
  seen empty.
- Drag a panel header to reorder: implemented, not exercised in Wine.
- Released as 0.1.0 in NorthRay Launcher and on the website (suite 0.46.0, 14 Nov 2026) at CJ's request,
  marked "In development" with a description that says it doesn't capture, record or stream yet.
  Installing it through the Launcher hasn't been tried here (the test machine can't reach the live site).

## 9. Milestone 2: Video capture (status: done, 14 Nov 2026; released as 0.2.0)

**Built**
- `src/video/gpu.*`: one D3D11 device (BGRA + video support where the driver allows, multithread-protected), one
  runtime-compiled shader (d3dcompiler_47, loaded at run time) drawing a textured or solid four-cornered shape,
  alpha and invert blending, render-target textures.
- `src/video/wgc.*`: Windows.Graphics.Capture through its ABI interfaces (declared from Microsoft's metadata; MinGW's
  header lacks them), activated through combase at run time; free-threaded frame pool polled with TryGetNextFrame
  (no event handler, no dispatcher queue); cursor on/off; the Windows 11 yellow border turned off where allowed.
- `src/video/sources.*`: Display (Desktop Duplication → WGC → GDI), Window (WGC → GDI PrintWindow, BitBlt when
  PrintWindow draws nothing; found again by program, class and title with `matchWindow`), Camera (Media Foundation
  source reader: the largest format up to 1080p that keeps ≥ 29 fps, converted to RGB32 by the reader on the CPU).
  Duplication draws the pointer itself (colour, masked-colour and monochrome shapes; the inverting parts in a second
  pass with an invert blend) and turns the picture for rotated monitors. CPU-picture threads (GDI, cameras) skip their
  work when the source hasn't been asked for in 0.5 s.
- `src/video/engine.*`: the video thread on a high-resolution waitable timer at the output rate; per frame it updates
  each shown source once, composes the current scene into the canvas texture, and at the preview rate draws the
  letterboxed canvas, the selection (handles; grey when locked), hover outline and snapping guides into the preview's
  own swap chain (flip-discard → flip-sequential → discard). GPU time per frame from timestamp queries. Nothing is
  composed while the preview is hidden and no output is running (`setOutputsActive` for Milestone 5).
- `core/placement`: frames, rotated corners, hit tests, handles, resize keeping the opposite side fixed (rotated too),
  crop by Alt+drag (scale kept, mirrored items handled), snapping, fit/stretch/centre/reset/rotate, window matching.
  `SceneCollection`: transforms, visibility, lock, order, source rename and settings.
- UI (`capture_sources.cpp`): + menu with displays, windows (searchable when long), cameras and sources from other
  scenes; rows with status, eye and lock; item menu (rename, hide, lock, transform, order, capture method, pointer,
  remove); preview mouse (select, move with snapping, resize, Alt-crop, cursors per handle and rotation); keys (arrows
  nudge, Ctrl+F/S/D/R, Ctrl+Up/Down order, Delete, F2). New items are fitted (if bigger than the canvas) or centred
  once their size is known. Removing the last item showing a source deletes the source and stops its capture.

**Tested**
- capturecore: 110 tests (placement and crop maths, item editing, window matching, plus Milestone 1's).
- In Wine (software rendering, no WGC, no Duplication, no cameras): display capture in compatibility mode, window
  capture of Notepad (PrintWindow drew nothing there, so the BitBlt fallback), the live preview, select, move with
  snapping, resize keeping the shape, Alt+drag crop, rotate 90°, fit to the canvas with the rotation, hide, the item
  menus, saving, restarting (the window found again; "Waiting for notepad.exe to open" when it's closed), 150% scaling.
  Hiding the preview took Capture from ~72% to ~7% CPU in Wine (where the GPU work runs on the CPU).

**Not tested / limits**
- Desktop Duplication, Windows Graphics Capture (and the declared WGC interfaces), cameras, rotated monitors, the
  pointer shapes and GPU timing have not run on real Windows yet. They need CJ's PC.
- Cameras convert on the CPU (fine at 1080p30 on most PCs; a GPU path is Milestone 12). HDR displays are captured
  as SDR (DuplicateOutput, not DuplicateOutput1). Scaling uses bilinear filtering; better downscaling is later.
- A graphics-driver reset stops the preview with a message; recovering without a restart is later (Milestone 13).
- Window capture captures the whole window including its frame; Alt+drag crops it. A "client area only" option is
  a possible later addition.

## 10. Milestone 3: Audio capture (status: done, 14 Nov 2026; released as 0.3.0)

**Decision: sound sources are global.** They belong to the scene collection, not to a scene, and are heard in every
scene (like OBS's global audio devices). `unusedSources()` never lists them. Per-scene muting can come with the
advanced scenes and audio milestones if wanted.

**Built**
- `core/audio`: dB/gain, the fader curve (gain = position³: 0.5 ≈ −18 dB), dB text, peaks, mixing, a level meter
  (instant rise, 20 dB/s fall, 1.5 s peak hold, clip flag), a lock-free single-producer/single-consumer ring of
  stereo frames, the float WAV header.
- `src/audio/devices`: input and output endpoints with names and the default; programs with audio sessions on any
  output, one per program, by the top process of that program's tree (Chrome's audio service and Discord's helpers
  are children of the same exe); the program's own description from its version info; the Windows build check.
- `src/audio/capture`: one thread per sound source, WASAPI shared mode, event-driven (with a 10 ms timeout because
  loopback signals nothing during silence), AUTOCONVERTPCM to 48 kHz stereo float (16-bit PCM as a fallback,
  converted here). Process loopback through `ActivateAudioInterfaceAsync("VAD\\Process_Loopback")` with an agile
  completion handler and the activation structs declared here; INCLUDE_TARGET_PROCESS_TREE. Follows the default device
  when set to it, notices the program closing (and finds it again), retries a missing device every 2 s, and says why in
  its status (privacy settings, unplugged, Windows 10, an error code).
- `src/audio/mixer`: a 10 ms clock of its own; each source's block (silence when it has none; trimmed to 20 ms when
  more than 100 ms has built up), volume and mute, peaks, the mix; test clips of a source's own sound before volume and
  mute (10 s WAV in Videos\\NorthRay Capture).
- UI (`capture_audio.cpp`): the mixer panel (the mix meter; per source name, dB, status, stereo meter, mute and fader),
  the + menus (in the mixer and in Sources), the right-click menu (rename, mute, device or program, test clip,
  remove), meters at 30 Hz (10 Hz in low-resource mode), only while there are sound sources.

**Tested**
- capturecore: 130 tests (dB, fader, meter ballistics, ring wrap-round and overflow, WAV header, global sound
  sources, plus earlier milestones').
- In Wine with PulseAudio null devices: the input list and default; capturing an input (a 44.1 kHz device, converted
  to 48 kHz by Windows' conversion), its meter, the fader (−17.5 dB) and mute, settings kept across a restart, the
  "Needs Windows 11" message for program sound, a status error shown in full on click. A 10-second test clip of the
  input held its 880 Hz tone with the 440 Hz tone playing on the speakers at −95 dB relative (absent), 48 kHz float,
  exactly 10.0 s, with no gaps.

**Not tested / limits**
- Desktop audio (loopback): Wine 9 refuses loopback capture (AUDCLNT_E_WRONG_ENDPOINT_TYPE), so it has only been
  seen failing gracefully here. Process loopback (one program's sound) can't run on Wine at all. Both need CJ's
  Windows 11 PC: the test clip is the way to confirm a program's sound is captured alone.
- Drift between device clocks is handled by trimming and padding (a rare click over long sessions); proper
  drift-corrected resampling arrives with recording and A/V sync in Milestone 5.
- No monitoring (headphones), no separate stream/recording mixes, no filters yet (Milestones 4 and 9). The leak
  warnings in `core/routing` are used from Milestone 4, when routing exists.

## 11. Milestone 4: Audio routing (status: done, 14 Nov 2026; released as 0.4.0)

**Built**
- Mixer buses: Stream, Track 1-6, Monitor (`busBit(i)` matches `routing::Bus`). A source's block is added only to
  the buses in its `buses` setting, after its volume and mute; a source not routed to a bus contributes nothing to it.
  Meters for every bus; test clips of any bus (`Mixer::busKey`).
- `MonitorOutput`: the Monitor bus played on a chosen output (Settings › Audio: Windows' default, a device, or off),
  event-driven WASAPI render with AUTOCONVERTPCM, about 40 ms buffer, trimmed if it falls behind, follows the
  default device. It only runs while some unmuted source is sent to Monitor.
- Defaults (`routing::defaultBuses`): Stream + Track 1 + a track per kind (input 2, desktop 3, application 4).
- Desktop audio "leave a program out": process loopback in EXCLUDE mode for that program's tree while it runs
  (Windows 11; one program per source, a Windows limit), the device's ordinary loopback while it isn't, switching
  when it starts. (Process loopback exclude captures what Windows plays rather than one device.)
- Leak and feedback checks (`routing::analyse`, Milestone 1's rules) run every status tick against every program
  with an audio session on each device (active or not, to warn before it plays). Shown as banners at the top of the
  mixer (up to three; leaks get a one-click "Leave X out of desktop audio"), red status on the sources involved, and
  in Settings › Audio, which also lists each source's route (`describeRoute`, with device names). Desktop sources
  show which programs they hear now (`carriedApps`).
- Mixer panel: routing chips (Stream, 1-6, Mon) under each fader; Stream and Monitor meters; wheel scrolling
  (no scroll bar; the edges fade when there's more).

**Tested**
- capturecore: 133 tests (routing analysis, defaults, labels, carried apps, plus earlier milestones').
- In Wine with PulseAudio null devices: a "microphone" (880 Hz) sent to Stream + tracks, and a second input
  (440 Hz, standing in for a private app) sent to tracks + Monitor but not Stream. 10-second clips: the Stream mix held
  880 Hz with 440 Hz at −94 dB relative (absent); Track 1 held both. The monitor device (a null sink) received the
  440 Hz source only, recorded independently with `parec`. The feedback warning appeared when desktop audio was
  monitored on its own device and cleared when monitoring was turned off. Routing chips, Send to menu, monitor device
  menu, the routes view, wheel scrolling.

**Not tested / limits**
- The leak warnings and the leave-a-program-out fix with real programs: Wine has no Windows programs playing sound
  and no process loopback. The logic is unit-tested; the Windows behaviour needs CJ's PC (the test clip of the stream
  mix is the check: a program kept off the stream must not be audible in it).
- One program per desktop source can be left out (Windows' process loopback takes one process tree). To leave out
  several, use application sources for what should be heard instead of desktop audio.
- Monitoring adds the device's own latency (~40 ms buffer); there's no per-source monitor delay or "monitor only"
  vs "monitor and output" distinction beyond the Monitor and Stream buttons, which already cover both.

## 12. Milestone 5: Recording (status: built, 14 Nov 2026; released as 0.5.0; not yet run with a real encoder)

**Built**
- `core/media`: Annex B parsing (3- and 4-byte start codes), length-prefixed conversion, keyframe detection, SPS/PPS
  extraction, the avcC record, the AAC-LC AudioSpecificConfig, and an interleaver that releases packets in decode
  order (waiting for every live stream to pass a time, or one second for a stream that's gone quiet).
- `core/mkv`: a Matroska writer: EBML head; segment and clusters written with "unknown size" and patched when
  closed (a crashed file still plays); seek head with a void that becomes the cues entry; info with the duration
  patched at the end; one H.264 track and up to six named AAC tracks; a cluster per video keyframe (and at most 30 s);
  SimpleBlocks; cues for every keyframe.
- `src/encode/video_encoder`: an H.264 MFT (the chosen one, else hardware first, then software). Canvas BGRA →
  NV12 by the D3D11 video processor (BT.709 limited range, signalled in the output type). Hardware: async MFT
  (unlocked), D3D manager, NV12 textures straight in (MFCreateDXGISurfaceBuffer). Software or no D3D manager: copied
  to memory through a staging texture. Settings through ICodecAPI: VBR (peak-constrained, 1.5× max) or CBR, mean
  bitrate, GOP = 2 s, no B-frames; High profile, Main as a fallback. Its own thread (event-driven for async MFTs);
  at most 3 frames waiting, more are dropped and counted. Drains on finish.
- `src/encode/audio_encoder`: Windows' AAC MFT, 48 kHz stereo 16-bit in (converted from float), raw AAC out,
  96-192 kbit/s; sample times by counting samples from the first block.
- `src/output/recorder`: one recording: taps on the chosen track buses (the mixer stamps each tap's first block
  with its QPC time; it now mixes missed blocks after a stall instead of skipping them), the engine's frame sink (the
  canvas each frame; composition runs while it's set), a constant frame grid at the output rate (late frames counted),
  audio in 1024-frame blocks, packets interleaved and written once the first keyframe has given the SPS/PPS; a 1 MB
  write buffer; free space checked every 5 s (won't start under 500 MB, stops cleanly under 200 MB); on stop: no more
  frames, the encoders drained, everything written in order, the file finished. A file with no pictures is deleted.
- UI: Start/Stop recording in Controls with the time; the status bar's time, size and lost frames; Settings ›
  Recording (folder picker, encoder menu, bitrate, tracks 1-6, audio quality); canvas and frame rate locked while
  recording; the recording is finished properly if the window closes. Track names list the sources on them.
- A sync test: a pattern source (white for the first 33 ms of each QPC second) and a mixer-generated tone (a 1 kHz
  beep for the first 50 ms of each QPC second), added together from + in Sources.

**Tested**
- capturecore: 152 tests (NAL parsing, avcC, AAC config, the interleaver including quiet and not-yet-started
  streams, the Matroska writer's header and patched sizes).
- `tools/check-mkv.sh`: an H.264 stream (libx264, with AUDs) and two AAC streams (ffmpeg) put through Capture's
  writer and interleaver: ffprobe reads 1 video and 2 named audio tracks, ffmpeg decodes every packet without errors,
  seeking to 9 s works (cues), all 360 frames read back, and a copy cut off halfway still gives 180 frames.
- In Wine: "No H.264 encoder was found" reported when starting (Wine has no MF encoders); the controls, the
  status bar, the settings section; the sync pattern and tone (the tone shows on the Stream meter).

**Not tested / limits**
- The MF encoders (hardware or software), the video processor conversion, the recorder's timing and the A/V sync
  have not run anywhere yet: this machine has no H.264/AAC MFTs. CJ's PC (GTX 960: NVENC) is the first real test.
  The sync test source is for exactly that: record ~20 s and check each flash and beep start together.
- MKV only; "save as MP4" (a remux) is planned. Recording resolution is the canvas's (no downscaling yet).
- Audio and video are timed from the same QPC clock; device latencies (capture card, Bluetooth) aren't compensated
  yet (a per-source sync offset is Milestone 9 material).

## 13. Milestone 6: Streaming (status: built, 14 Nov 2026; released as 0.6.0; not yet run with a real encoder or platform)

**Built**
- `core/rtmp`: URL parsing (rtmp:// and rtmps://, default ports 1935/443, a key pasted on the end split off), AMF0
  (numbers, booleans, strings, objects, ECMA arrays, null), chunking both ways (all four header types, extended
  timestamps, set chunk size 4096, window acknowledgement and acknowledgements, ping replies), and the publishing
  client: handshake (C0-C2/S0-S2), connect, releaseStream / FCPublish / createStream / publish with the result and
  onStatus checked (a refusal is reported with the server's own words), @setDataFrame onMetaData, the AVC sequence
  header (avcC), the AAC sequence header (AudioSpecificConfig), FLV video and audio tags, FCUnpublish /
  deleteStream on stop.
- `src/output/net`: TCP through Winsock (connect with a timeout, send/receive with select, a send timeout so a stalled
  connection fails rather than hangs). TLS through SChannel (Windows' own TLS: TLS 1.2/1.3 as Windows offers), and the
  server's certificate checked by Capture itself as well: the chain must end at a root Windows trusts, have no errors
  and pass the SSL policy for the server's name; the refusal says which (wrong name, untrusted authority, expired).
- `src/output/credentials`: the stream key in Windows Credential Manager ("NorthRayCapture/stream/main", this PC
  only). It's never in settings.json, never shown, and registered with the log so it's masked if it ever reached a
  log line. A key pasted on the end of a server address is moved to Credential Manager.
- `src/output/streamer`: its own H.264 encoder (CBR at the stream bitrate, GOP 2 s, the recording's encoder choice)
  and AAC encoder on the Stream bus; the canvas from the engine's named frame sink "stream" (recording and streaming
  can run together, each with its own encode); packets interleaved and queued; connects once the first keyframe has
  given the SPS/PPS; every connection starts at a keyframe with timestamps from 0. If the connection fails or drops,
  encoding carries on and it reconnects after 2, 4, 8, 16, 32 then 60 s; a refused key on the first attempt isn't
  retried. The queue never holds more than ~3 s: older groups of frames are dropped (from a keyframe) and counted.
- UI: Start/Stop streaming in Controls; the status bar shows the state (connecting, live with the time and kbit/s,
  reconnecting with the attempt and countdown; the reason goes to the log) and dropped frames; Settings › Streaming: service (Twitch, YouTube, Kick,
  Custom), server, stream key (masked entry, Replace / Remove), video bitrate (2.5-8 Mbit/s). Typing an address that
  isn't the service's own switches it to Custom. Settings that would change the stream are locked while live. The
  window stops the stream (politely) before the recording and the engine when it closes.
- Ingest addresses (checked 14 Nov 2026): Twitch's automatic "global contribute" RTMPS server; YouTube's RTMPS
  server on port 443; Kick's are per channel, so they're pasted.

**Tested**
- capturecore: 164 tests, including AMF0 both ways, URL parsing (keys pasted in the URL, ports, rtmps), chunk headers
  and extended timestamps, the publish exchange against a scripted server, refusal messages, FLV tag bodies.
- `tools/check-rtmp.sh` (Linux): Capture's RTMP client publishing H.264 + AAC to `ffmpeg -listen 1`: all 240 frames
  received and decoded, with the audio.
- In Wine, with a test program built from Capture's own net.cpp and RTMP client: plain RTMP over Winsock, 180/180
  frames; RTMPS through SChannel (via a TLS proxy in front of ffmpeg, with a test certificate authority), 180/180
  frames; a certificate for another name refused; a certificate from an authority Windows doesn't trust refused
  (Wine's own SChannel accepts both, so Capture's extra check is what refuses them there).
- In Wine, the app: Settings › Streaming (service switch, server change, the key entry masked, "Saved", Replace /
  Remove); a key pasted on the end of an address moved out of the address and out of settings.json; no key in
  settings, scenes or the log; Start streaming without a key ("There's no stream key…") and without an encoder ("No
  H.264 encoder was found…").

**Not tested / limits**
- The whole stream with a real encoder: this machine has no H.264/AAC MFTs, so the streamer's encode, timing and
  reconnects have not run end to end. Nor has any real platform (Twitch, YouTube, Kick) been streamed to.
- A/V sync over the stream, reconnects after a real network drop, and bandwidth backoff are untested.
- One destination at a time; several at once from one encode is Milestone 7. No bandwidth test, and no automatic
  bitrate lowering yet.

## 14. Signing in to Twitch and YouTube (status: parked, 14 Nov 2026; was in 0.6.1, taken out of the app in 0.7.0)

CJ chose manual stream keys (pasted per destination) over signing in. The code below stays in the repository
(`core/oauth` is still built and tested; `src/output/accounts.cpp` and `http.cpp` are not built into the app, and
`tools/check-accounts.sh` still checks them) so it can come back if wanted; the steps under "To switch it on" would
then apply, plus wiring the Account row back into Settings › Streaming.

**Built**
- `core/oauth`: SHA-256, base64url, PKCE (S256), URL/form encoding, and reading every answer used below (device
  codes, tokens and their errors, Twitch's validate and stream key, YouTube's streams and channel). No network.
- `src/output/http`: HTTPS requests through WinHTTP (TLS 1.2/1.3, Windows checks certificates, no redirects followed,
  nothing logged).
- `src/output/accounts`: one Credential Manager entry per service ("NorthRayCapture/account/twitch|youtube": the
  refresh token and the account's name); access tokens only in memory.
  - Twitch: the device code flow for public clients (no client secret anywhere). Capture asks for a code, opens
    twitch.tv/activate (only a twitch.tv address is ever opened), shows the code to check, and polls. Scope:
    `channel:read:stream_key` only. Refresh tokens are single-use, so each new one is saved at once. Validated at
    start and every hour while kept (Twitch requires it). Sign out revokes the token with Twitch.
  - YouTube: Google's installed-app flow: a one-off listener on 127.0.0.1 (a port Windows picks), the browser
    sent to Google's sign-in with PKCE and a random state; requests with another state are ignored, other paths
    get 404, and the listener gives up after 10 minutes. Scope: `youtube.readonly` only. Sign out revokes with Google.
  - The stream target: Twitch's key (Get Stream Key) with the chosen server; YouTube's streams (liveStreams,
    mine=true), the default one first: its RTMPS address and stream name.
- UI: Settings › Streaming › Account for Twitch and YouTube: "Sign in with …", the Twitch code and Cancel while
  waiting, "Signed in as …" and Sign out after. Signed in, the key (and YouTube's server) come from the account each
  time you go live, so a key reset on the website is picked up; Capture doesn't store a copy. The Controls button
  shows "Getting your stream key…" meanwhile. Problems are shown in plain words (Twitch's own message for 403s,
  as Twitch asks).
- App registrations: `apps/capture/oauth-clients.json`, built in. Empty means the service shows "isn't set up in
  this version". A copy in `%LOCALAPPDATA%\NorthRay\Capture\oauth-clients.json` overrides it without a rebuild.

**Tested**
- capturecore: SHA-256 (FIPS vectors incl. two-block and 1000-byte inputs), the RFC 7636 PKCE example, encoding,
  every parser with real-shaped answers and errors (36 checks).
- `tools/check-accounts.sh`: the Windows code in Wine against a local stand-in for Twitch and Google
  (`tools/accounts-mock.py`): Twitch sign-in, key, an expired access token refreshed (and the rotated refresh token
  kept, no access token stored), sign-in restored in a new instance, sign out (Credential Manager entry gone,
  token revoked), a code never approved (expires with a message), cancel; YouTube sign-in through the loopback
  listener (favicon 404, a forged state ignored, PKCE checked by the stand-in), key and RTMPS server, refresh,
  sign out with revoke, cancelled in the browser; "not set up" without registrations. 24/24.
- The app in Wine, with id.twitch.tv and api.twitch.tv pointed at the stand-in over HTTPS (a test authority,
  removed afterwards): the not-set-up text; "Sign in with Twitch…" with no connection (message shown); the code
  shown while waiting, the browser opened; signed in as the account; Start streaming took the key from the account
  (then stopped at "No H.264 encoder", as Wine has none); no key in the log; still signed in after a restart;
  Sign out revoked the token.

**Not tested / limits**
- The real Twitch and Google services: there are no app registrations yet (below). YouTube's sign-in in the app
  itself (as opposed to the test program) wasn't run, since Google's sign-in page can't be stood in for.
- YouTube: Capture uses the channel's default stream key; going live still follows YouTube's own settings (auto-start
  in YouTube Studio). Creating or scheduling broadcasts, titles and categories aren't done from Capture.

**To switch it on (CJ, once)**
1. Twitch: dev.twitch.tv/console › Applications › Register Your Application. Name "NorthRay Capture", OAuth
   Redirect URL `http://localhost` (required by the form; the device flow doesn't use it), Category "Broadcaster
   Suite", Client Type **Public**. Copy the Client ID.
2. Google: console.cloud.google.com › new project "NorthRay Capture" › APIs & Services › enable **YouTube Data API
   v3**. Google Auth Platform: Branding (app name, support email), Audience External (it starts in Testing: add the
   Google accounts that may sign in as test users), Data access: add `.../auth/youtube.readonly`. Clients › Create
   client › **Desktop app**. Copy the client ID and client secret (for desktop apps Google doesn't treat it as secret).
3. Put them in `apps/capture/oauth-clients.json` and rebuild (or, to try first, in the local override file).
- While the Google app is in Testing, only listed test users can sign in, they see an "unverified app" notice, and
  Google ends test sign-ins after 7 days (sign in again). For everyone: publish the app and complete Google's
  verification for the YouTube scope.

## 15. Milestone 7: Multistreaming (status: built, 14 Nov 2026; released as 0.7.0; not yet run with a real encoder or platform)

**Built**
- Capture → Composite → Encode → Distribute → Multiple Destinations:
  - `src/output/streamer`: one encode path per video bitrate (an H.264 encoder at CBR, an AAC encoder on the Stream
    bus, its own frame sink "stream:<kbps>"); destinations at the same bitrate share it, a different bitrate gets
    its own. Each path fans its packets out to its destinations. An encode is stopped once no running destination
    uses it (also when its destinations have failed by themselves).
  - `src/output/destination`: one connection per destination on its own thread with its own buffer (at most ~3 s;
    older groups of frames dropped from a keyframe, counted only while connected), its own reconnection (2 s,
    doubling, up to the chosen longest wait), and its own status: state, message, live time, kbit/s, bytes sent,
    dropped frames, attempts, reconnections, retry countdown, connect time (TCP + TLS + RTMP). A connection
    starts at the newest keyframe waiting. A refused key fails just that destination.
- Settings: up to six destinations, each with a service (Twitch, YouTube, Kick, Custom), server, stream key (in
  Credential Manager as "stream/<id>"), video bitrate (2.5-12 Mbit/s) and whether Start streaming starts it. 0.6
  settings become the first destination ("main", whose key was already "stream/main"). Below them: the upload the
  switched-on destinations need (video + audio + ~5%, each destination its own copy), how many encodes that means,
  and the longest wait between reconnection attempts (15 s, 30 s, 1 or 2 min).
- Controls: Start/Stop streaming for all switched-on destinations, then a row per destination (state dot, live
  time, Mbit/s, dropped frames, reconnections, retry countdown, or why it stopped); clicking a row starts or stops
  just that one. When several start at once, any that can't are listed with their reasons and the others go ahead;
  one stopping later is shown briefly while the others carry on. The status bar: "live on 2 of 3 · 12.4 Mbit/s up".
- The Account row (signing in) was removed from Settings › Streaming (section 14).

**Tested**
- `tools/check-destinations.sh` (Wine): one 20 s H.264 + AAC stream fed in real time (2x) to four local ffmpeg
  RTMP servers through Capture's Destination: A received 600/600 frames; B, stopped at frame 240 while the others
  carried on, 239 (the frame fed as it stopped); C, added at frame 360, 240/240 from there; D, whose server only
  started ~4 s in, reconnected by itself and received everything from then (360). Every file decodes without
  errors; no frames dropped.
- The app in Wine: settings from 0.6.1 carried over (the saved key still found); adding destinations (a service not
  yet used suggested, YouTube at 8 Mbit/s); the upload and encode summary (2 destinations at 6 and 8 Mbit/s: about
  15 Mbit/s, 2 encodes); the destination rows in Controls; Start streaming with three destinations each failing for
  its own reason (no encoder, no key, no server address), listed together.

**Not tested / limits**
- The shared encode itself (one encoder feeding several destinations) and separate encodes per bitrate have not run:
  this machine has no H.264/AAC encoders. Nor has any real platform been streamed to.
- Bandwidth is estimated from the chosen bitrates; there's no upload speed test, and no automatic lowering of the
  bitrate when a connection can't keep up (frames are dropped instead, and counted).
- All destinations use the canvas size and frame rate; per-destination resolutions (e.g. a vertical canvas to one
  platform) come with Milestone 11's vertical canvases.

## 16. Milestone 8, part 1: Transitions, Studio Mode, nested scenes (status: built, 14 Nov 2026; released as 0.8.0)

**Built**
- `core/transition`: the kinds (Cut, Fade, Slide, Swipe, Luma wipe), duration, easing (steady, smooth in and out,
  quick start and gentle stop), direction, luma patterns (left to right, top to bottom, diagonal, circle, clock,
  blinds) generated as bytes, the soft-edge sum the shader uses, and the settings as JSON.
- The engine composes the program; when its scene changes it composes the old and the new scene each into its own
  texture and makes the program from them over the duration, on the video thread's own clock (so a busy UI doesn't
  make it stutter). Fade: the new one over the old with rising opacity. Slide/Swipe: the two canvases offset.
  Luma wipe: a second pixel-shader path takes the pattern (an R8 texture at a quarter of the canvas size) and
  reveals the new scene where the pattern is below the progress, with a soft edge. Switching again mid-way starts
  from the scene it was going to. Recordings and streams get the transition (they take the program).
- Nested scenes: a Scene source shows another scene (`{"scene": id}`), composed at canvas size with see-through
  where it has nothing, then placed like any source (move, resize, crop, rotate). Each nested scene is composed
  once a frame however often it's shown, four levels deep at most. The collection refuses an item that would show
  a scene inside itself (directly or further down); deleting a scene removes the source showing it.
- Studio mode: the preview area splits into Preview (left: the scene picked in Scenes, edited there) and Program
  (right, red frame: what's live). Transition (Controls) or Ctrl+Enter puts the preview's scene live; the Scenes list
  tags the live one; under the views: which scene each shows, and a warning when the scene being edited is the live
  one ("changes show at once"). Entering keeps the live scene live; leaving makes it the one shown. Without studio
  mode, picking a scene puts it live with the transition. Both persist (settings.json: studioMode, transition).
- Items can be duplicated in the collection (`duplicateItem`: its own transform, the same source); the menu entry
  comes with part 2's source controls.

**Tested**
- capturecore: 255 checks, including easing (ends, monotonic), slide and swipe offsets for each direction, every
  luma pattern's ends and size, the soft-edge sum (nothing at 0, everything at 1, any softness, inverted), settings
  JSON, nesting (loops refused at any depth, removal, save and load).
- The app in Wine: Fade, Slide and Luma wipe (2 s) captured part-way (both scenes visible as expected); a scene
  nested inside another, shrunk by its handle; the transition menu; studio mode (selecting and editing in the
  preview, Ctrl+Enter taking it live with a luma wipe, the LIVE tag moving, the warning); everything kept after a
  restart.

**Not built yet / limits**
- Stinger and custom shader transitions (listed as later in the menu), per-transition shortcuts, and choosing a
  transition per scene. Part 2: image, text, slideshow, screen-region sources, opacity, numeric transform. Part 3:
  scene import/export, folders, groups.

## 17. Milestone 8, part 2: sources made in Capture, exact controls (status: built, 14 Nov 2026; released as 0.8.1)

**Built** (`src/video/made_sources.cpp`, `src/capture_made.cpp`, `core/sourceutil`)
- Image: WIC (PNG, JPEG, BMP, GIF, TIFF; WebP and HEIC where Windows has the codecs) to straight-alpha BGRA on a
  worker thread. Animated GIFs: the logical screen size, each frame's position, delay (0 or 1 hundredths count as
  a tenth of a second, as browsers do) and disposal (keep, clear its area, restore the previous picture) put
  together on the CPU, up to 512 MB of frames; played from the video thread by the clock. The file is looked at
  every 2 s and reloaded when it changes (an overlay edited elsewhere updates by itself).
- Text: DirectWrite into a WIC bitmap through Direct2D (greyscale antialiasing, so no coloured fringes), then
  un-premultiplied. Font (16 common families, shown if installed, or any installed family by name), size 6-600,
  colour, bold, italic, alignment, wrapping width, an outline (the text drawn around itself, under the fill) and its
  colour. "\\n" in the text box starts a new line.
- Colour: a solid colour (#RRGGBB or #RRGGBBAA) at a chosen size (the canvas's by default).
- Slideshow: a folder's images in natural name order (or shuffled each round), each fitted inside a fixed size with
  see-through margins, for 3-60 s each; files Windows can't read are skipped; the folder is read again each round.
- Screen region: a picker shows a dimmed still of the whole desktop in a full-screen window (not a layered one: those
  misbehaved in testing) and the dragged rectangle at full brightness, using each mouse message's own position.
  The region's monitor is captured by the display source (Desktop Duplication, WGC or GDI) and the rectangle copied
  out on the graphics card (CopySubresourceRegion).
- Sources with see-through parts (images, text, colours, slideshows) are blended with their alpha; captures stay opaque.
- Every item's menu: Position and size... (typed x, y, width, height; "-" keeps one), Rotation... (degrees),
  Opacity (100-10% or typed), Duplicate (the same source, its own placement). Each made source's own settings are
  in its menu too (choose another image or folder, the text's font, size, colour and so on, the slideshow's timing,
  pick the region again). No dialogs with scroll bars: lists in menus, typed values in the ask card; files and folders
  through Windows' own picker.

**Tested**
- capturecore: 275 checks, including colours, typed placement and degrees, natural and shuffled slide order, GIF delays.
- The app in Wine: a colour (#7A5CFF) filling the canvas; two-line outlined text, centred once its size was known,
  then made italic from its menu; an animated GIF (three frames with transparency and "clear" disposal) playing over
  the text; a slideshow of three images (landscape and portrait) changing and fitting with see-through margins; a
  screen region (290x240 exactly as dragged), placed by typing 100, 100, 580, 480, rotated -12 degrees, at 50% opacity,
  and duplicated.

**Not built yet / limits**
- Video files and browser sources, part 3 (scene import/export, folders, groups), stinger transitions.
- The region picker covers one still of the desktop: what moves underneath while picking isn't shown.
- Wine draws the preview over its own file dialog and the picker; on Windows they sit above it as usual. Not checkable here.

## 18. Milestone 8, part 3: scene folders, export and import (status: built, 14 Nov 2026; released as 0.8.2)

**Built**
- Scene folders: each scene has an optional folder name (saved with it). Putting a scene in a folder moves it next to
  that folder's other scenes; the Scenes list shows each folder as a heading with its scenes indented under it.
  The scene's menu: Folder › (the folders there are, A new folder..., Out of its folder).
- Export: a .nrscene file (JSON, "format": "northray-capture-scene") with the scene, every scene shown inside it
  (however deep) and the sources they show, with their settings and placements. Stream keys are never in it (they're
  in Credential Manager, not in sources); files such as images are referred to by their paths, not copied (the
  message after exporting says so).
- Import: the file is read with the same checks as a saved collection, then added with new ids for everything and
  names made unique ("Gameplay 2"); scenes inside it are rewired to their new copies; placements are scaled when the
  file was made for another canvas size; importing twice gives two independent copies. Files over 16 MB are refused.
- Scene menu: Export to a file..., Import a scene... (Windows' own file dialogs).

**Tested**
- capturecore: 289 checks, including folders (grouping, out of a folder, saved), export (only what the scene needs),
  import (new ids, unique names, nested scenes rewired, scaling from 1920x1080 to 1280x720, twice, refusing other files).
- The app in Wine: a folder made from the menu and a second scene put in it (heading and indent shown); "Gameplay"
  (colour, text, GIF, slideshow, two items sharing a screen region, rotated and half see-through) exported to a file
  (five sources) and imported back as "Gameplay 2", looking the same.

**Not built / limits**
- Grouping sources inside a scene: a scene shown inside another does the same job for now (move, resize or hide
  the whole set at once); multiple selection and groups as such are still to come.
- Folders can't be collapsed; an exported scene's images aren't packed into the file.

