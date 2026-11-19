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

## 19. Milestone 8, part 4: media files, stingers, shader transitions, groups (status: built, 15 Nov 2026; released as 0.8.3)

**Built**
- Media source (`src/video/media_source.cpp`): a file or a list of files (played in turn, shuffled if asked, looping
  if asked). Each file is opened twice through the source reader, for pictures and for sound, each on its own thread
  and timed against one clock (late pictures dropped; sound handed to the mixer ~40 ms ahead, dropped if 200 ms late).
  Pictures: RGB32 if the reader converts, else NV12, I420 or YUY2 converted here (BT.709 for 720p and up, BT.601
  below). Sound: 48 kHz stereo float if the reader converts, else float at the file's rate and channels, converted
  here (first two channels; linear interpolation). The sound is a mixer channel like any other (volume, mute,
  routing, meters; the routing check treats it as an input). Menu: Pause, Play, From the start, Next file, Loop,
  Shuffle, Choose the files again.
- Stinger transition: a video (a media source kept open and paused at its first picture, so it starts on cue)
  played over the switch; the program shows the old scene until the cut point, then the new one, with the video over
  both; its length is the video's. Afterwards it rewinds and waits again. Its sound goes to the Stream and Track 1.
  Videos with transparency aren't supported (Media Foundation gives no alpha), so stingers cover the whole picture.
- Custom shader transition: the person's HLSL `float4 transition(float2 uv, float progress)` with sampleFrom(uv),
  sampleTo(uv) and resolution, wrapped and compiled (ps_4_0) on the video thread when the file is read; a compile
  error is shown in the menu and a fade used instead. "Make an example file to start from" writes a commented
  circle-wipe example; "Read the file again" after editing. The shader runs on the graphics card only.
- Groups: Group with › another source makes a scene of their own (in the "Groups" folder) shown by one item, so they
  move, resize and hide as one; Add to this group › moves another source in, keeping where it shows; Ungroup puts
  them back where they show (sources shown at their own size get that size written down first). A rotated, mirrored or
  cropped group can't be undone exactly, so Ungroup says so instead.

**Tested**
- capturecore: 307 checks (media file kinds, clock text, group, add to group and ungroup with a moved and halved
  group, refusing a rotated one, stinger and shader settings).
- The app in Wine: an MP4 (H.264 + AAC) playing with its sound on a mixer channel (Wine's decoders only gave I420 and
  the file's own sample rate, so both fallbacks ran); the example shader compiled and revealed the new scene in a
  growing circle; a stinger playing from its first frame on cue, twice; grouping two sources and ungrouping them.

**Not built / limits**
- Web pages as sources: WebView2 can't run here to test, so it isn't built yet.
- Media: no seeking bar or hardware decoding yet; the colour conversion fallback runs on the CPU.

## 20. Milestone 9, part 1: audio filters (status: built, 15 Nov 2026; released as 0.9.0)

**Built**
- `core/dsp`: a chain of up to 16 filters per sound source (JSON in its settings), run by a Processor on the
  mixer thread on the source's 48 kHz stereo sound, before its volume, routing, meters and test clips. Changing the
  chain makes a new Processor (the mixer swaps it in on its next block).
  - Gain (-30 to +30 dB).
  - Noise gate: opens above / closes below (hysteresis), attack, hold, release; a fast level follower.
  - Noise suppression: 512-point Hann windows at half overlap (exact reconstruction), each frequency's power
    smoothed, a noise level following its minimum (rising ~4 dB/s), counted three times over; a Wiener-like gain
    floored at the chosen reduction (3-40 dB), opening quickly and closing gently. 512 frames (10.7 ms) of delay.
    Steady unchanging sounds (a hum, a held note) are treated as noise after a few seconds, as with any
    suppressor of this kind.
  - Expander and compressor: a level follower (0.5 ms up, 30 ms down), a gain computer in dB (ratio, threshold),
    attack and release on the gain, make-up gain for the compressor.
  - Limiter: 1.5 ms look-ahead, instant gain reduction across the window, release; output clamped to the
    ceiling as a last guarantee.
  - Equaliser: RBJ low shelf (100 Hz), peaking middle (200 Hz-6 kHz, Q 1), high shelf (8 kHz), +/-15 dB.
  - Balance (constant power) and delay (up to 2 s, for lining a source up with a late camera).
- The filters page (a source's mixer menu › Filters...): each filter as a card with its switch, its values on
  sliders (click a value to type it), up, down and Remove; Add a filter (a limiter goes last, others before it);
  presets (a microphone: suppression, gate, compressor, limiter; game or music: a gentle compressor and limiter;
  a safety limiter); the delay the chain adds, in ms. Media files' channels have filters too.

**Tested**
- capturecore: 330 checks, on real signals: gain exactly +6 dB; a gate closed on -50 dB and open on loud sound; a
  compressor 12 dB over its threshold at 4:1 coming out ~9 dB down (and nothing below it changed); an expander; a
  limiter never over its ceiling with a sudden full-scale burst; EQ +6 dB at 50 Hz and nothing at 5 kHz, -12 dB at
  its middle; balance; a delay exactly 480 frames; noise suppression lowering steady noise by more than 10 dB while
  keeping speech-like tone bursts within 1.5 dB; the chain's JSON.
- The app in Wine: the filters page with one filter and with the microphone preset; a media file's test clip before
  and after a -12 dB gain filter measured -21.0 and -33.0 dBFS: exactly 12 dB apart.

**Not built / limits**
- A gain-reduction meter on the mixer channel; VST plugins (part 3 looks at plugins).

## 21. Milestone 9, part 2: routing and monitoring (status: built, 15 Nov 2026; released as 0.9.1)

**Built**
- Each stream destination chooses its sound: the Stream mix or one of Tracks 1-6 (Settings › Streaming › Sound),
  so one platform can get a game-only mix, say. The streamer keeps one H.264 encode per bitrate; within it, one
  AAC encode per mix that any destination carries (a tap on that bus, its own interleaving with the shared
  video), shared by the destinations that want it. A mix nobody carries any more is dropped; the video encode
  stops when its last destination does. Settings shows when destinations' different sound adds encodes.
- A second output (Settings › Audio): any one mix (Stream, a track or Monitor) also played on another device, a
  line out to a second PC or a recorder, by its own WASAPI render thread, independent of the monitor.
- Routing checks for it: desktop audio played by the second output onto the device it captures (it would feed
  back), and the second output sharing the monitor's device (both heard mixed).
- Routing presets in a source's Send to menu: Stream and recordings (as a new source starts), Recordings only
  (never the stream), Stream only (not recorded), Only me (monitor). The one the source matches is ticked; the
  buses stay adjustable one by one beneath. Presets keep whether a source is monitored.
- Media sources in the routing view are named by their files, not as a microphone.

**Tested**
- capturecore: 342 checks, including the second output's feedback and shared-device warnings (and none for a
  bus the source isn't on, or with the output off) and each preset for each kind of source.
- The app in Wine (PulseAudio): presets applied and ticked; the second output playing the Stream mix on the
  default device ("Playing on PulseAudio Output"); the destination Sound menu and settings saved.
- Not tested end to end: a stream carrying a track instead of the Stream mix. Wine has no H.264 or AAC encoders,
  so the streamer itself can't run here; it needs a Windows test before it's relied on.

**Not built**
- A different video per destination (another canvas or scene); per-destination audio bitrate.

## 22. The Camera Border maker (status: built, 15 Nov 2026; released as 0.9.2)

Asked for by CJ in the middle of Milestone 9 (plugins, part 3, is paused in git and comes next).

**Built**
- `core/border`: a border is part of a scene item (saved with it, copied with it, exported with its scene): corners
  rounded from square to a circle or pill (roundness as a share of half the shorter side), a ring of colour round the
  outside of the item's edge, so none of the picture is covered (since 0.9.3; 0.9.2 drew it just inside) (plain or a two-colour gradient at any angle, colours with transparency), and a glow or
  shadow round it (size, colour, strength, and an offset for a drop shadow). The shape is a rounded-box signed
  distance with one-pixel anti-aliasing.
- The video engine draws it on the graphics card in the same pass as the item (a second small shader: the glow
  first on a quad grown by the ring and glow's reach, then a quad grown by the ring's thickness with the picture
  masked to the item's shape and the ring between that and the shape grown by the thickness; a square item's ring
  keeps square corners), so it
  follows the item when it's moved, scaled, rotated or faded. If that shader can't be made, items draw as before.
- The editor: an item's menu (in Sources or on the preview) › Border... opens it in the dock under the live preview,
  so changes show on the real picture. Six presets (Clean white, Neon glow, Soft shadow, Circle, Gold frame, Thin
  and square), sliders, colour menus (named colours or typed #RRGGBB/#RRGGBBAA), and On/Off. Clicking another item
  in the preview moves the editor to it. Circle also crops the picture's middle to a square and makes the item square
  (same height and centre). Escape or Done closes it.
- Save as PNG: the same border drawn on the processor (border::render, the same sums) at the item's size plus the
  glow's reach, inside left clear, into Pictures › NorthRay Capture, for use as an overlay elsewhere.
- A group with a border can't be ungrouped (its sources would lose it) until the border is off.

**Tested**
- capturecore: 376 checks (406 with 0.9.3's), including distances (edges, rounded corners, a circle), the gradient's
  ends and middle, JSON kept and clamped, and the rendered picture: the ring outside the item's edge, the item's area clear, cut corners, a glow that
  falls further on the shadow's side, and the margin.
- The app in Wine: Neon glow on a video (rounded corners, cyan-to-pink ring, purple glow seen in the preview);
  Circle (cropped to the middle, 720 x 720, same centre, round in the preview); Save as PNG (756 x 756: clear
  inside, white ring, soft shadow, checked pixel by pixel); the border saved and back after a restart.

**Limits**
- The glow and shadow turn with a rotated item (the offset is in the item's own directions). Importing a scene
  that's scaled to another canvas doesn't scale ring thickness or glow.

### 0.9.3 (15 Nov 2026): the ring goes outward

CJ: the border should go out from the source, not into it. The ring is now drawn round the outside of the item (the
picture keeps its full size; the item's box and its handles are unchanged, the ring sits just beyond them), the
glow round the ring, and margin() and Save as PNG include the ring's thickness. Checked in Wine on the circle with a
thick ring: the whole picture shows inside, the ring outside the selection box. 406 core checks (the plugins
groundwork, pushed earlier as work in progress, is in this release's tree but not switched on anywhere yet).

## 23. Milestone 9, part 3: optional audio plugins (status: built, 15 Nov 2026; released as 0.9.4)

**Built**
- A documented C interface, `sdk/northray_audio_plugin.h` (version 1): a 64-bit DLL exports `nr_audio_plugin()`
  returning its name, author, version, up to 16 parameters (key, label, range, default, unit) and create, destroy,
  set_param, process (interleaved float stereo at 48 kHz, in place, on the audio thread) and optional latency.
  The header states the rules (no blocking, allocation, files, network, windows or lingering threads in process).
  `sdk/examples/tremolo.c` is a working example; `sdk/README.md` says how to build and install one.
- `core/plugins` (portable, tested): the user's choices in plugins.json, each tied to the SHA-256 of the file they
  switched on; reconcile() switches off a plugin whose file has changed or gone; plain-DLL-name checks (no folders,
  no device names); validation of a plugin's own description (interface version, functions present, names, ranges,
  unique keys, at most 16); a list of plugins running, kept in a file while any run and removed on a clean exit, so
  after a crash those plugins start switched off ("in case it was the cause").
- `src/audio/plugin_host`: scans %LOCALAPPDATA%\NorthRay\Capture\plugins (64 files, 64 MB each at most), hashes each
  file and checks its Authenticode signature with WinVerifyTrust (no online revocation checks: no network; the
  words say so) and the signer's name. Switching one on re-reads the file and refuses if its hash isn't the one the
  user was shown; it's then copied to plugin-cache\<sha256>.dll, that copy is held open read-only (nobody may write
  or delete it) while its hash is checked again and LoadLibraryEx loads it (DLL search limited to its own folder
  and System32), so the bytes that run are the bytes that were checked. Its description is validated before use.
  Modules stay loaded until Capture closes (instances may be running); switching one off makes the chains again
  without it. Files are checked at start, on Look again and when switching on.
- The filter chain: dsp::Type::Plugin with the file name and the plugin's own values, run through a factory the host
  provides; one that isn't running passes sound through and says why on its card. A chain's latency is now worked
  out once when it's made (including plugins' reported latency).
- Settings › Plugins: each file with its name (once loaded), status, signature, size and full SHA-256; a switch that
  asks first (the signature, the SHA-256 and a plain warning, with Cancel the default); Open the folder; Look
  again. Filters › Add a filter › Plugins lists the ones switched on; their values get sliders and typed entry.

**Tested**
- capturecore: 406 checks (registry, reconcile, crash list, name checks, description validation, a plugin filter in
  a chain via a stand-in factory and passing through when not running).
- The app in Wine with the example tremolo.dll (built with MinGW): listed off, with the SHA-256 matching sha256sum
  (0600d483...61a7) and "Not signed"; the confirmation shown; switched on ("Running: by NorthRay (example), version
  1.0"); added to a media file's filters (Rate 5 Hz, Depth 50%); its test clip's level rising and falling at exactly
  5.00 Hz with the quiet parts at 0.50 of the loud ones, where without it the level was steady (0.96). A file of
  junk named broken.dll: refused ("isn't a 64-bit Windows DLL", error 193) and left off. One byte added to
  tremolo.dll, then Look again: switched off ("its file has changed"). Capture killed while tremolo ran: on the
  next start it was off with the crash reason. Closed normally: the running list removed and tremolo stays on.

**Not built / limits**
- A plugin runs inside Capture: a crash in it closes Capture (it's then switched off), and it has the user's
  permissions. Running plugins in a separate process would remove that; not built.
- No plugin windows (version 1 has sliders only), no MIDI, no sample rates or channel counts other than 48 kHz stereo.
- VST 3 (researched only): Steinberg's VST 3 SDK is MIT-licensed since version 3.8 (October 2025), with no fees or
  agreements; the VST name and logo are optional and, if used, follow Steinberg's guidelines. Hosting VST 3 would mean
  building the SDK's hosting classes (module loading, IComponent/IAudioProcessor, parameter changes, editor views
  in a child window) on the same trust rules as above, and plugin editors would be the first windows a third party
  draws in Capture. VST 2 isn't available to new hosts. Not started; a later milestone if wanted.

## 24. Milestone 10, part 1: the overlay designer (status: built, 15 Nov 2026; released as 0.10.0)

CJ's answers (claude/northray-feature-questions-2026-11.md in the project) put the overlay designer and its templates first,
ahead of the brief's milestone order.

**Built**
- `core/design` (portable, tested): a design is a page (any size up to 8192) with a background and up to 200 elements, bottom
  to top: text, rectangles, ellipses and pictures. Each has a place, size, rotation, opacity, visible/locked, a fill (none,
  a colour, or a linear or round gradient of 2-16 stops with see-through colours), an outline, rounded corners, a soft
  shadow (offset, softness, colour) and an entrance (fade, slide in from any side, pop, wipe; its length). Text has its font,
  size, bold, italic and alignment; pictures fill their box or fit inside it. Editing (add with unique ids, remove,
  duplicate just above, up, down, top, bottom), hit-testing that honours rotation and ellipses, snapping to the page's
  edges and centre and other elements' edges and centres (moving or resizing) with the guides that matched, the
  entrance's pose at any moment, and JSON that drops nonsense and clamps values.
- Placeholders: text can hold {name}, {twitch}, {youtube}, {instagram}, {tiktok}, {website} and {schedule}, filled from a
  profile shared by the NorthRay apps (%LOCALAPPDATA%\NorthRay\profile.json, set under Your details... in the
  designer). On a line of parts between dots, a part whose details aren't set is left out with its dot, so a
  socials line never shows "  ·  ·"; the designer shows such parts as their {tag} so they can be seen.
- Templates in the NorthRay aurora look: Starting Soon, Be Right Back, Ending (night gradient, two soft round aurora
  glows, a title that pops in with a shadow, a subtitle, a gradient accent that wipes in, socials), a lower third (slides
  in), a facecam frame (glowing aurora outline and a name tag), a socials bar, a Twitch panel (320 x 100) and an offline
  banner (1920 x 1080).
- `src/video/design_render`: one renderer for the live source, the designer's canvas and PNG export: Direct2D and
  DirectWrite on a WIC bitmap. Gradients are pictures of their colours worked out with design::colourAt (so they match
  the tests and need no brush rotation); shadows are the element's shape drawn alone in a bitmap just big enough for it
  and its blur, blurred (three box blurs), tinted and placed under it, moving with its entrance.
- The Design source (kind "design"): drawn on a worker thread when it changes, when the profile changes, and every 33 ms
  while its elements come in; the entrances play when its scene goes live (the program scene changing sends "enter"),
  and once more after the last picture if it was taken mid-entrance.
- The designer (Sources › + › Overlay design..., or a design's menu › Edit the design...): a template or an empty page
  to start; the page fitted on a checkerboard (see-through parts show); click to pick (or in Layers), drag to move with
  snapping and pink guides (Shift: none), the corner to resize (Ctrl: keep its shape); arrow keys nudge (Shift: 10 px),
  Delete, Ctrl+D copies, Ctrl+Z undoes (60 steps). Layers lists elements top first with show/hide and lock. The
  properties panel: name, place (typed exactly), rotation, opacity, fill (colours, six gradients, typed stops "#RRGGBB
  pct, ...", direction, round), outline and thickness, corners, shadow, text (edit, size, bold, italic, font, alignment),
  picture (another, fill or fit), entrance and its length, and the layer buttons. Page...: size presets (canvas, 720p,
  vertical, Twitch panel) or typed, background none, colour or gradient. Save as PNG writes the design at its own size
  to Pictures › NorthRay Capture.

**Tested**
- capturecore: 456 checks, including gradients (ends, stops, between), editing and layers, hit-testing with rotation, locked
  and ellipses, JSON both ways and with nonsense, snapping (page centre, another's edge, resizing, nothing beyond the
  threshold), every entrance (fade, slide, pop overshooting and settling, wipe), every template round-tripping, and
  placeholders (dropped parts, shown tags, several lines).
- The app in Wine: Sources › + › Overlay design... › Starting Soon added and opened in the designer; the soft aurora glows,
  shadowed title, gradient accent and the socials tags drawn on the canvas; the title picked, dragged and snapped (guides
  at the page's centre lines; its place updated); the design drawn in the scene after its entrance; Save as PNG (1920 x
  1080, pixels checked). Found and fixed on the way: Wine's Direct2D ignored see-through gradient stops and rotated bitmap
  brushes, which is why gradients are now made as pictures; a picture taken mid-entrance stayed (one more is drawn).

**Not built yet (CJ's answers, coming next)**
- A brand kit (colours, fonts including library font files, logos and watermark, stingers), icons (official social logos
  per their guidelines, general icons, CJ and NorthRay marks), live elements in designs (timers, chat, now playing), QR
  codes, many-stop gradient editing by dragging, GIF and WebM export, Twitch panel and banner export presets beyond the
  templates, facecam frames that follow the camera, live animated aurora backgrounds on the GPU, and the media library.

## 25. Milestone 10, part 2: timers, QR codes and icons in designs (status: built, 15 Nov 2026; released as 0.10.1)

**Built**
- Three more element kinds in `core/design`, saved with the design:
  - Timer: text that counts, in the text's own look (font, size, bold, colour, shadow): a countdown for a length (from
    when its scene goes live), a countdown to a time of day (a time passed less than twelve hours ago has ended; earlier
    ones mean tomorrow), a stopwatch, time live (the longest-running stream destination; "Not live" otherwise) and a
    24-hour clock, optionally with the day and date. Words can go around it ("Starting in {time}"). Countdowns round up
    to whole seconds, show their own words at zero, and pulse for the last ten seconds (switchable). At zero a
    countdown can switch to a chosen scene (once each time its scene goes live; Studio Mode switches the program).
  - QR code: from a link or text (placeholders such as {website} work), via Project Nayuki's QR Code generator library
    (MIT, vendored in libs/third_party/qrcodegen, medium error correction), drawn as crisp squares with a quiet border;
    fill is its light colour and outline its dark one.
  - Icon: twenty built-in icons drawn for NorthRay (star, heart, arrow, tick, play, chat, music, camera, place, mail,
    bell, calendar, clock, dot, book, brush, microphone, globe, sparkle, moon) as simple paths in a 24 x 24 box, filled
    with any fill (gradients too) and outlined.
- The Design source draws again every second while a timer's text changes and every 33 ms while one pulses; the
  designer's canvas follows timers too. The window checks countdowns each second for their scene switch.
- Designer: Timer, QR code and Icon... in Add; their properties (what a timer shows, its length or time typed as 5,
  4:30, 1:00:00 or 20:00, what it shows at zero, the scene to switch to, pulse, date, the words around it; a QR code's
  link; which icon). A selection reaching off the page is drawn only up to the page.

**Tested**
- capturecore: 1321 checks (the icon checks look at every point of every icon): countdown text through the seconds,
  rounding, pulse only in the last ten seconds, the end words, hours; until before and after the time and tomorrow's;
  stopwatch, time live and not live, the clock with and without the date; saving; QR codes square with the three
  finder patterns; nothing for empty or over-long text; every icon parsing into closed figures inside its box.
- The app in Wine: a Timer added to Starting Soon, set to 0:08 and to switch to Just chatting; its scene shown, the
  countdown running on the page (0:01 seen) and Capture switching to Just chatting at zero (the log says so); a QR code
  for https://northray.example/download saved as PNG and read back by OpenCV's QR reader to exactly that link; star,
  heart and play icons drawn with gradient fills.

**Not built / limits**
- Playing a sound at zero: it belongs with the soundboard, so it's heard on stream through the mixer. Official
  social-platform logos: they need the platforms' own brand files and rules. A timer that pulses redraws its whole
  design about 30 times a second for ten seconds (fine on a PC, slow under Wine).


## 26. Milestone 10, part 3: the brand kit (status: built, 15 Nov 2026; released as 0.10.2)

CJ asked for a brand kit (colours, fonts including font files from the library, a logo/watermark) before the rest of
Milestone 10's extras, so templates and every design can follow one look.

**Built**
- core/design: `Brand` (six colours with keys primary, secondary, text, soft, panel, night; a heading and a body font;
  up to sixteen font files; a logo), saved as JSON (bad or missing parts take the NorthRay defaults; brand colours are
  always solid). Designs link to it: a solid fill, a gradient stop, an outline or a shadow can name a brand colour
  (`brand` / `strokeBrand` in the design's JSON), a text's font can be `brand:heading` or `brand:body`, a picture's
  file `brand:logo`. `applyBrand` gives the design with the kit's values (a linked colour takes the kit's red, green
  and blue and keeps its own see-through amount, so soft glows stay soft); `linkToBrand` links what matches the kit
  (colours with the same red, green and blue; Segoe UI text, bold to the heading font); `unlinkBrand` freezes how it
  looks now; `usesBrand`.
- Every template is linked to the default kit, so a new kit restyles them; a new "Logo watermark" template shows the
  kit's logo (60% opacity). New text, timers, rectangles, ellipses and icons start on brand colours and fonts.
- The kit is kept in %LOCALAPPDATA%\NorthRay\brand.json. Font files and the logo are copied into
  %LOCALAPPDATA%\NorthRay\Brand (Fonts, Logo) so the kit keeps working when the originals move.
- Drawing (video/design_render): every design is drawn through `applyBrand` with the window's kit; a change of kit
  redraws every Design source (it counts as a change of details). Font files are used without installing them:
  DirectWrite font sets (IDWriteFactory5, Windows 10 1703 and later) join the system's fonts and the kit's files into
  one collection, made once per list of files. Where that isn't available the kit says so and the system's fonts are
  used. A picture with no file (no logo yet) draws nothing on stream and a placeholder in the designer.
- Designer: Brand kit... (colours by #RRGGBB, back to the NorthRay colours, heading and body fonts, add or remove font
  files with the families each holds, the logo). Every colour menu lists the brand colours first; the fill menu has
  brand gradients (main to second, panels to background, main fading out); the font menu offers the brand heading and
  body fonts, the kit's font families, common Windows fonts or any installed font by name (checked against what
  Windows and the kit have); pictures can be "Your brand logo". Page... has a brand-colour background, "Follow the
  brand kit" and "Stop following the brand kit". Linked colours and fonts are named as "Brand: ..." in the properties.

**Not built / limits**
- Stingers (transition videos) in the kit: they come with the transitions work. Official social-platform logos: they
  need each platform's brand files and rules. Text sources (not designs) don't follow the kit yet.

## 27. Milestone 10, part 4: live Twitch chat in designs (status: built, 15 Nov 2026; released as 0.10.3)

CJ asked for a Twitch chat overlay that's read-only and needs no sign-in.

**Built**
- core/chat: IRC lines with IRCv3 tags (unescaped), turned into events: chat messages (display name, colour, badges,
  /me actions, ids), PING, RECONNECT, joins, notices, CLEARCHAT (a timeout or ban: that user's messages go; with no
  user: everything), CLEARMSG (one deleted message). `Log` keeps the last 100 and applies deletions; `recent` picks
  the newest few and drops faded ones; `channelFrom` reads a Twitch name from a name, @name or twitch.tv link;
  `loginLines` is the anonymous login ("justinfan" plus a number, Twitch's documented read-only login); sample
  messages (all named "Sample...") for the designer.
- output/twitch_chat: a WinHTTP WebSocket to wss://irc-ws.chat.twitch.tv on its own thread. It sends only the
  login, the JOIN and PONGs. It reconnects after 2 s, doubling to a minute, and when Twitch asks it to (RECONNECT).
  Stopping closes the socket to end a waiting read. Messages go to the renderer (video::setDesignChat), which
  redraws every design. NORTHRAY_CHAT_TEST_URL points it at a local test server (used for NorthRay's own tests;
  this build machine can't reach Twitch).
- It runs only when needed: each second the window checks for a design with a chat element (in the collection, or
  open in the designer) and a Twitch name under Your details; otherwise it disconnects.
- Design element `chat`: the newest messages at the bottom and older ones above until the box is full (the newest
  always shows), each on a rounded card (optional; the colour is linked to the brand's panel colour), the name in bold
  in the viewer's colour (Twitch's default colours for names with none; dark colours lightened so they read on dark
  cards) or the brand's main colour, /me messages in italics. The element's font, size, fill and alignment style the
  text. Messages can fade after a number of seconds. A "Chat box" template (600 x 700).
- Designer: Chat add button, the connection's state in words, messages, fade, cards and their colour, name colours.

**Not built / limits**
- Emotes show as their names (drawing them needs Twitch's emote images: a later step); badges aren't drawn.
- Only Twitch. YouTube and Kick chat need their own connections (YouTube's needs an API key or sign-in).
- A chat dock panel (reading chat inside Capture) comes next.
- Tested against a local stand-in server speaking Twitch's chat protocol, not against Twitch itself.

## 28. The Chat panel (status: built, 15 Nov 2026; released as 0.10.4)

- core/diag `Layout`: a fifth panel, Chat, hidden at first (Layout menu). Layouts saved before it keep their widths
  (missing widths are filled in rather than all reset); tested.
- The panel shows the connection in words (live / connecting / the error and the retry), then the newest messages
  that fit, newest at the bottom: the name in the viewer's colour (lightened on the dark theme, darkened on the
  light one), their role (streamer, mod, VIP, sub) and the message; /me messages in the name's colour. Deletions,
  timeouts and bans apply as on the overlay (same log).
- Read-only. "Reply on Twitch..." opens https://www.twitch.tv/popout/<channel>/chat in the browser.
- While the panel is shown, chat stays connected even with no chat element in a design.
- Tested in Wine against the local stand-in server (messages, roles, colours, a deletion and a timeout).
- Not yet: scrolling back through older messages (no scroll bars by design; the newest that fit are shown).

## 29. Now playing (status: built, 15 Nov 2026; released as 0.10.5)

- core/sourceutil `trackFromFileName`: title and artist from "Artist - Title", leading track numbers dropped,
  underscores to spaces (tested).
- The media source: when a sound-only file starts (music; not video files, not stingers) it reads the title and
  artist through Windows' property system (PKEY_Title, PKEY_Music_Artist, else the album artist), else the file
  name, and publishes it (video::setNowPlaying, keyed by the source). A source clears it when it finishes (no loop)
  or goes away; the latest track started wins.
- Designs: {song} and {artist} fill in like Your details (empty parts between dots drop out). A document option,
  "Show only while music plays" (Page...), draws an empty see-through page when nothing plays (the designer always
  shows it). A "Now playing" template (760 x 150, brand-linked, slides in).
- Tested in Wine with an untagged WAV named "Aurora Band - Night Sky.wav". Reading real tags (MP3/FLAC) is Windows'
  property system's job and needs checking on Windows (Wine's support is limited).
- Not yet: Capture's own music player (playlists, auto-duck under the microphone) - next.

## 30. Auto-duck and music from a folder (status: built, 15 Nov 2026; released as 0.10.6)

- core/dsp `Ducker` + `DuckSettings` (amount -12 dB by default, threshold -38 dBFS, attack 80 ms, hold 400 ms,
  release 700 ms): per 10 ms block it takes the trigger's loudest sample and returns the gain at the block's start
  and end; it moves in decibels at a steady rate (all the way down in the attack time, back up in the release time)
  and lands on exactly 1 when done (tested: timing, smoothness, re-ducking mid-release, 0 dB does nothing).
- Mixer: a source's settings can hold {"duck": {"under": <source id>, "amount": dB}}. The trigger's level is its
  previous block's peak after its volume and mute (so a muted microphone never ducks), the gain is ramped across
  the block, and the meters and the strip's words show how far it's ducked ("ducked -12 dB").
- The mixer menu: Auto-duck (quieter while you talk): Off, or "while <source> has sound" (inputs first), and the
  amount (6, 12, 18 or 24 dB).
- Music (a folder of songs)...: a media source with every MP3, M4A, AAC, WAV, WMA or FLAC in the folder (sorted,
  then shuffled, looping, at half volume), ducking 12 dB under the first microphone if there is one. Its tracks feed
  now playing ({song}, {artist}).
- Tested in Wine: a 440 Hz "song" ducked under the sync test tone's beeps; a 10-second Stream-mix clip measured
  down within 70 ms of each beep and 18 dB lower (the amount set), then easing back.
- Not yet: a folder that picks up new songs by itself (the list is read when it's added), music controls (pause,
  next) outside the source's menu.

## 31. The MP4 copy and markers (status: built, 15 Nov 2026; released as 0.10.7)

CJ: "automatic MP4 after recording (keep the MKV)" and "markers as chapters".

- core/mp4 (portable, tested): `scan` reads Matroska in place (EBML headers only; frames are found, not loaded):
  Info (timecode scale), Tracks (codec, private data, name, size, rate, channels), Clusters with SimpleBlocks or
  BlockGroups (a BlockGroup without a ReferenceBlock is a keyframe). Unknown-size segments and clusters (a
  recording still being written, or cut short) are read up to the next cluster or the end; a frame cut off at the
  end is dropped. Laced frames are refused in words (Capture never laces). `remux` writes an MP4 with the movie box
  first (fast start; its size is measured first since the offsets are fixed width), 32- or 64-bit offsets as the
  size needs, one frame per chunk, stts from the times between frames (rounded on the running total so nothing
  drifts), stss keyframes, avcC / esds (AudioSpecificConfig), each audio track named in its hdlr, only the first
  audio track enabled (the rest in the same alternate group), an empty edit for a track that starts late, and Nero
  chapters (udta/chpl).
- output/mp4_copy: runs it on its own thread after a recording ends (Settings > Recording > MP4 copy, on by
  default), reading with a 1 MB window and writing in 4 MB pieces to "<name>.mp4.part", renamed when complete and
  deleted if anything fails; the MKV is never touched. Progress shows under the Record button; the result is
  flashed (and logged).
- Markers: while recording, Controls has "Add a marker"; each becomes a chapter ("Marker 1"...) in the MP4.
- Tested: the unit tests check every frame's bytes and offset, keyframes, track flags, names, the late track's edit
  and chapters, a cut-short file, junk and cancelling. A real libx264/AAC MKV (5 s, 300 frames, two named tracks)
  converted natively and with the Windows build under Wine (tests/mp4_copy_check.cpp) decodes in ffmpeg without
  an error, with both chapters; the same file cut at 60% converts and decodes too. A full record-then-copy run
  needs Windows (Wine has no H.264 encoder).

## 32. Profiles and scene collections (status: built, 15 Nov 2026; released as 0.10.8)

- core/profiles (tested): names that can be file names (1-40 characters, no \ / : * ? " < > |, no Windows-reserved
  names, unique ignoring case), "Name 2" when taken, and recording names "<profile> <date time>" ("Recording ..." for
  the Default profile).
- A profile is the Settings that aren't the window's own: the window keeps its theme, low-resource mode, preview,
  workspace layouts, studio mode and scene collection when the profile changes. The current profile is
  settings.json; the others are profiles\<name>.json. Switching saves the current one there, loads the other, and
  applies it live (output frame rate, monitor and second outputs, the mixer, transitions). A new profile is a copy of
  the current one whose stream destinations get new ids with their keys copied in Credential Manager, so removing a
  key in one profile never touches another; deleting a profile removes only the keys no other profile uses. Not
  while recording or streaming.
- Scene collections (scenes\<name>.json, each with its canvas): switching saves, clears every live capture (ids can
  repeat between collections), loads and rebuilds; new (empty) or a copy; rename; delete to the Recycle Bin.
- The top bar's "Profile: X" and "Scenes: Y" are menus (they were a fixed label before).
- Tested in Wine: a copied profile (new destination ids), its frame rate changed to 30, switched back to Default at
  60 (the engine changed live) and the copy kept 30; an empty collection, then back to Default with everything
  (captures, music, designs) rebuilt.

## 33. The setup wizard (status: built, 15 Nov 2026; released as 0.10.9)

- core/wizard (tested): `sustainedKbps` (the median send rate after the first 3 seconds of a bandwidth test, less a
  share when frames were dropped; 0 if it didn't connect or ran under 6 seconds) and `recommend` (goal, a hardware
  encoder or not, processor threads, upload): a hardware encoder gets 1080p60; no hardware encoder gets 1080p30 with
  12+ threads, else 720p30; the stream gets 75% of the upload less the audio, capped at Twitch's 6000 kbps, with the
  picture to suit (under 4500 kbps 720p, under 4500 30 fps); unknown connection: 720p at 4500; recordings scale with
  pixels a second (8,000-40,000 kbps; 50,000 recording-only with a hardware encoder). Every choice comes with a
  reason in words. The canvas is never below 720p (the canvas sizes Settings offers).
- Settings > Setup wizard...: 1 what for (streaming / recording / both, this PC's encoder and threads); 2 the
  connection: "Test my connection" streams the program to the Twitch destination for 20 s at 8000 kbps with
  "?bandwidthtest=true" after the key (Twitch's documented test: received, never shown, the channel doesn't go
  live; the key with the suffix is registered as a secret so it never reaches the log), sampling the send rate and
  dropped frames each second; or a typed speed (Mbps x 0.8: speed tests measure bursts); or skip; 3 the plan with its
  reasons and "Use these settings" (frame rate, every destination's bitrate, the recording bitrate; the canvas only
  when the collection is empty, otherwise it says where to change it). Closing stops a test.
- Tested in Wine: every step; the test failing cleanly with no H.264 encoder (and nothing about the key in the log); a
  typed 20 Mbps giving 6000 kbps stream, 30 fps, 9000 kbps recording on a 2-thread PC with no hardware encoder,
  applied to the settings file, the canvas left alone. The test itself against Twitch needs Windows with an encoder
  and internet.

## 34. The soundboard (status: built, 15 Nov 2026; released as 0.10.10)

- core/soundboard (tested): the starter pack synthesised in code (tones with harmonics and decays, filtered
  deterministic noise): chime, ding-dong, fanfare, level up, drum roll, ba-dum-tss, whoosh, applause, wrong answer,
  sparkle, heartbeat, page turn; each normalised to 0.85 peak, trailing silence trimmed, 5 ms fades, the same every
  time. They're NorthRay's own, free to use on stream. `Voices` mixes up to 32 sounds at once with per-sound gain
  ("restart": pressing a playing sound starts it again rather than doubling it).
- SourceKind::Soundboard ("soundboard", sound only): its settings hold the buttons ({"sounds": [{"name",
  "starter"|"file"}]}, up to 48). audio/soundboard_player: decodes files once (Media Foundation source reader to 48
  kHz stereo float, up to 30 s each; failures kept per button in words), and a thread keeps ~20 ms of mixed voices
  (or silence) in the ring the mixer reads, so the soundboard is an ordinary mixer channel.
- The Soundboard panel (Layout menu; hidden at first): "Add the soundboard" (with the starter pack, at 70% volume),
  buttons in as many columns as fit (a note when some don't fit), "+ Sound...", Stop all (with how many are playing);
  right-click a button: rename, another file, move earlier/later, remove. A red dot marks a button whose file can't
  play.
- Tested: the core tests (levels, no clicks, determinism, voices); in Wine, a Stream-mix clip with the fanfare and
  applause pressed shows each one's energy at the right times on top of the music and the test tone.
- Not yet: hotkeys for the buttons (with the hotkeys work), per-button volume.

## 35. Voice effects (status: built, 15 Nov 2026; released as 0.10.11)

Three more built-in filters (core/dsp; kTypes is now 12, Plugin after them; saved by key, so older chains read
the same):
- Reverb: a Schroeder-Moorer room in Freeverb's layout (eight damped combs in parallel and four all-passes in series
  per side, the right side's lines 23 samples longer for width), scaled to 48 kHz. Room size (feedback 0.70-0.98),
  damping, mix.
- Pitch shift: two read heads moving through a delay line at the new speed, half a cycle apart with raised-cosine
  fades (a 50 ms window), -12 to +12 semitones and a mix; 0 semitones passes the sound untouched. Reports 25 ms of
  latency (half the window) so the filters page says so.
- Radio voice: mono, two high-passes at 300 Hz and two low-passes at 3,400 Hz (a telephone's band), a tanh drive
  normalised to keep the level, optional static (deterministic noise), mix.
- Tested: 440 Hz pitched to 880 / 220 / 659 Hz (+12 / -12 / +7) within 12 Hz; the reverb's tail after a click dies
  to under 5% and stays finite under four seconds of loud input; the radio cuts 100 Hz and 9 kHz by more than 15 dB
  against 1 kHz; static adds noise to silence; JSON round trip and descriptions. In Wine: the three in Add a filter,
  a pitch shift's card with its sliders and the chain's delay.

## 36. Green screen (status: built, 16 Nov 2026; released as 0.10.12)

- core/keying (tested): `Key` (on, colour, similarity, smoothness, spill; JSON with defaults and clamping) and the
  per-pixel sums the shader repeats: BT.709 Cb/Cr distance to the key colour (so brightness, i.e. shadows, doesn't
  matter), alpha = smoothstep(similarity, similarity + smoothness, distance), and spill: pixels near the key colour
  pulled towards their own grey by the spill amount. Tested on a green screen in light and shadow, skin, hair, red,
  white, black and blue, a greenish edge, a blue screen, and round trips.
- SceneItem.key (saved only when set); the engine's DrawItem carries it; the frame shader (video/gpu.cpp) gains
  `keyed()` before the corner mask, and drawFramed runs with no ring, corners or glow when only the key is on, so
  border and key combine.
- The item menu's Green screen submenu: off, green (#00B140) and blue (#0047BB) presets, "Pick the colour from the
  preview" (an eyedropper), a typed colour, and presets for how much is cleared, edge softness and spill.
- The eyedropper (capture_key.cpp): the next click in the preview gives a canvas pixel; the item's key is switched
  off, and a one-off engine frame sink (on the engine thread, after skipping 4 frames so the canvas is drawn without
  the key) copies that pixel into a 1x1 staging texture and reads it; the stats timer then turns the key on with that
  colour (or says it couldn't, after 8 ticks). Esc stops picking.
- Tested in Wine: a picture of a figure on #00B140 with a darker shadow band; the eyedropper read #00B140 exactly
  and the green and the shadow went, the face and shirt stayed.

## 37. The aurora sweep transition (status: built, 16 Nov 2026; released as 0.10.13)

- transition::Kind::Aurora ("aurora"). core/transition `aurora(u, v, progress, direction)` (tested) and
  `auroraShader(direction, colour1, colour2)`, the same sums as a person's transition() function: a front slanted
  12% across the picture and rippling (a sine across it that moves with the progress), its centre running from off one
  edge (-0.3) to off the other (1.3); the new scene shows behind it with a short soft edge, and a Gaussian band of light
  (shimmering across) is added in a blend of the two colours. Tested: only the old scene and no light at the start, only
  the new one and no light at the end (every direction), a bright band halfway, the right side turning first for each
  direction, each point only ever going from old to new, the generated HLSL (and default colours for bad ones).
- The window generates the shader in the brand kit's main and second colours and the transition's direction; the
  engine runs it through the custom-shader path (compiled when the code changes; a fade if it doesn't compile).
  Transition menu: "Aurora sweep" with Direction, Duration and Easing.
- Tested in Wine: a 2-second sweep to the right, frames show the band crossing and the new scene behind it.

## 38. Animated GIF export of designs (status: built, 16 Nov 2026; released as 0.10.14)

- core/gif (tested): `encode(w, h, frames, dither)` from straight-alpha BGRA frames. One global palette by median
  cut over a sample of every frame's opaque pixels (up to 255 colours plus a see-through entry when any pixel's alpha
  is under 128, else 256); nearest colour (weighted RGB distance, cached per 15-bit colour) with a light 4x4 ordered
  dither that stays still from frame to frame; LZW with variable code size and a clear code when the table fills;
  graphic-control blocks with the delay in hundredths (at least 2), disposal "restore to background" when there's a
  see-through entry; NETSCAPE2.0 loop-forever when there's more than one frame. A frame identical to the one before
  just adds its delay to it. Tests decode it with a small decoder in the test file (sizes, loop, delays, flat colours
  exact, see-through pixels, a 256-colour noise frame long enough to restart the LZW table, bad sizes); Pillow decodes
  it too.
- design::entranceMs(doc): the longest entrance among visible elements.
- Designer: "Save as GIF" renders the design at 25 fps (renderDesign at t = 0, 40, … ms) until the entrances end, then
  the finished look held for 1.5 s; scaled to at most 800 wide. Saved as "<name> <w>x<h> - <stamp>.gif" in
  Pictures\NorthRay Capture. A design with no entrances says so and points to Save as PNG.
- WebM is not included: Windows has no VP8/VP9 encoder, so it would mean bundling libvpx (a separate decision).
- Tested in Wine: Starting Soon (pop, fade, wipe) gave 23 frames at 800x450, 40 ms each and a 1540 ms last frame,
  looping; the frames show the entrance building up.

## 39. The pre-live checklist (status: built, 16 Nov 2026; released as 0.10.15)

- Optional (Settings › Streaming › Pre-live checklist; settings.json `preLiveChecklist`, off by default, and
  `checklistItems`, your list, starting with four suggestions). When it's on, the Start streaming button opens
  "Before you go live" (capture_checklist.cpp, drawn in the settings area like the wizard) instead of starting.
- core/checklist (tested): `run(Snapshot)` gives checks marked Ok, Waiting, Warn or Problem from: the destinations
  switched on and not already live (name, whether a key is in Credential Manager, whether a server address is set,
  the bus it carries); the current scene and whether it's empty; each microphone (InputAudio sources, not Capture's
  own like "#synctone": muted, its buses, whether its capture is working, seconds since it last had sound); whether
  there's a hardware H.264 encoder; whether recording is on. `tidy` cleans your list (trimmed, no blanks or
  repeats, at most 12 of at most 80 bytes, not cut inside a character).
- The mic check is live: meterTick notes when each source's level was last above about -50 dB (AudioLive.heardAt),
  and the meters repaint the checklist, so "Say something to check <mic>…" turns into "<mic> is picking you up" when
  you speak (Waiting after 20 s of quiet; Waiting isn't counted as a problem).
- Your list ticks for this time only; add (askText) and remove (×) are saved. "Go live" (or "Go live anyway" with
  warnings or unticked items) closes it and calls startStreaming, so every real error is still reported the usual way.
- Tested in Wine: the checks matched reality (Twitch with a key; YouTube with none; Kick with no server address, the
  same reason the stream start gave; Wine's unavailable inputs; no encoder); ticking, adding and removing (saved to
  settings.json); Not yet; Go live anyway handed over to the stream start; the setting toggles and saves. The mic
  turning green needs a working microphone (Wine's inputs are unavailable): to try on Windows.

## 40. Automation: scenes that follow your programs (status: built, 16 Nov 2026; released as 0.10.16)

- core/automation (tested): a Rule is a program's file name (normaliseExe: lower case, no folder, ".exe" added),
  a label, Opens or Closes, a scene by name, on, and onlyLive (only while streaming or recording). Saved in
  settings.json "automation" (at most 20; bad entries skipped). Scenes are kept by name, so a rule works in every
  scene collection with a scene of that name; a missing one is shown in red under the rule and named in a notice
  if it fires.
- Watcher.update(running, rules, live) gives the rule to act on or -1. Its first look only notes what's running
  (programs already open don't count as opening), and it's reset whenever the rules change; it fires on the change
  only; when several fire at once, the last in the list wins.
- The window (capture_automation.cpp), from the stats timer every 2 s while there are rules: a Toolhelp process
  snapshot (file names only), then the scene switch as a timer reaching zero does it (programScene_ in studio
  mode, else the current scene), a short notice ("Notepad opened: switched to Scene 2.") and a log line.
- Settings › Automation: each rule as a sentence with an on/off switch and More… (only while live or recording,
  move up/down, remove). Add a rule…: programs with a window open now (from listWindows, by file name with a window
  title as the hint), NorthRay Writing Studio (writingstudio.exe), or a typed file name; then "When <it> opens /
  closes, switch to" a scene.
- Tested in Wine: rules for notepad.exe (opens: Scene 2; closes: Just chatting); starting Notepad switched to Scene
  2 within the 2-second look and closing it switched back, with the notices and log lines; only-while-live,
  moving and removing saved to settings.json.

## 41. Global hotkeys (status: built, 16 Nov 2026; released as 0.10.17)

- core/hotkeys (tested): a Binding is a Combo (RegisterHotKey's modifier bits and a virtual-key code) and an
  Action: Scene (by name), ToggleStream, ToggleRecord, ToggleMute (a sound source by name), PlaySound (a soundboard
  sound by name), StopSounds, Marker, Transition. `allowed` refuses modifier keys alone, keys with no name (Print
  Screen, Caps Lock), typing keys (letters, digits, punctuation, Space, Enter, arrows, Home/End...) without Ctrl, Alt
  or Win, any Win combination (Windows keeps most), and Ctrl+Alt+Delete, Alt+Tab, Alt+F4 and Alt+Esc. F-keys, the
  keypad, media keys, Pause and Scroll Lock may be used alone. Saved in settings.json "hotkeys" (at most 60;
  unusable, repeated and target-less ones skipped).
- The window (capture_hotkeys.cpp) registers each with RegisterHotKey(MOD_NOREPEAT) at start and on every change;
  one another program holds is logged and shown in red. WM_HOTKEY runs the action: scenes switch the current scene
  (the preview in studio mode, where Transition sends it live); streaming starts straight away without the pre-live
  checklist (often pressed inside a game); a marker needs a recording running.
- Assigning: "Press the keys…" takes the next WM_KEYDOWN/WM_SYSKEYDOWN in preTranslate with the modifiers held
  (GetKeyState), ignoring modifier keys alone; Esc cancels; refused or already-used combinations say why. The
  registered hotkeys are released while assigning so they don't catch the keys.
- Stream Deck: its built-in Hotkey action presses these. A dedicated plugin (Elgato SDK) isn't included: there's
  no device to test one with yet.
- Tested in Wine: adding "Switch to Scene 2" (plain G refused with the reason; F7 taken), F7 switching scenes via
  WM_HOTKEY, re-assigning to Ctrl+Alt+2 and that switching from Just chatting; saved to settings.json. Pressing a
  hotkey with a game focused needs Windows (the test display has no window manager to confirm focus).

## 42. The aurora sky: a live background on the GPU (status: built, 16 Nov 2026; released as 0.10.18)

- SourceKind::AuroraSky ("aurora-sky"). core/aurora_sky (tested): Settings (main, second and night colours,
  speed 0.25-3, brightness 0.3-1.5, stars, followBrand, size) as JSON; `sample(u, v, seconds)` is the picture in
  C++ and `shader()` the same sums as an HLSL pixel shader with the colours, speed and brightness written in. The
  sky is the night colour, darker towards the top; three curtains each have a rippling lower edge (two sines),
  fall off sharply below it and fade upwards above it, carry fine rays (a sine across, bent by another), and blend
  between the two colours along their length; stars are one in about 600 2×2-pixel cells in the upper part,
  twinkling, hidden where the light is bright. Tests: every value in range over hours, the band brighter than the
  top and bottom, it moves, brightness scales only the light, the night colour shows below, speed doubles time,
  stars only up top and only when on, and the shader carries the settings.
- The source (made_sources.cpp AuroraSkySource) compiles the shader when its settings change and draws it into its
  own picture with Gpu::drawWith (which now accepts no textures) every frame it's being composed, with the seconds
  since it was made (wrapped daily). It's opaque, at the canvas size.
- The window: Add a source › Aurora sky (in the brand kit's primary, secondary and night colours, at the canvas
  size); its item menu has Aurora sky › Speed, Brightness, Stars, Colours from the brand kit, Main/Second/Sky
  colour… (a colour of its own stops it following the kit) and Its own size…. Saving the brand kit updates every sky
  that follows it. Settings reach the running source through syncSources.
- Tested in Wine (wined3d): added to an empty scene it draws and animates (frames 4 s apart differ by a mean of 14
  levels); changing a colour updates it live; switching back to the brand kit's colours restores them. Wine renders
  shaders in software, so its frame rate there says nothing about a real graphics card.

## 43. Items that follow items: facecam frames with the camera (status: built, 16 Nov 2026; released as 0.10.19)

- SceneItem.follow (core/scenes, tested): the item it follows (same scene) and its rectangle in that item's own
  frame as fractions of its size (x, y, w, h), plus a rotation added to the other's. Saved as "follow" on the item;
  a link to an item not in the scene is dropped when loading.
- SceneCollection::followItem works it out from where both are now (both need a size; no circles, checked along the
  chain). setTransform on an item moves everything following it (placeFollowers: size scaled, centre turned about
  the target's centre with its rotation, chains up to 8 deep); setTransform on a follower itself re-works out where
  it sits, so moving a frame by hand adjusts it. Removing the target leaves followers where they are, not
  following. duplicateScene and importScene point the links at the new items (a bug where the id map was keyed by
  the new ids, from assignment order, was caught by the test).
- The window: the item menu's "Follow another item" (or "Follows <name>") lists the scene's other items (cameras
  marked) and Stop following; an item at its source's own size gets that size written down first (pinSize).
- Tested in Wine: a text following a picture; Position and size… on the picture (800×600 at 560,240 to 400×300 at
  100,100) moved and halved the text to the same place on it (842,494.5 to 241,227.25), saved with the link.

## 44. Videos in the brand kit: stinger, intro, outro (status: built, 16 Nov 2026; released as 0.10.20)

- design::Brand gains `stinger`, `intro`, `outro` (paths, copied into %LOCALAPPDATA%\NorthRay\Brand\Videos) and
  `stingerCutMs` (0-10000), saved in brand.json (tested: round trip, bad types and long paths ignored, the cut
  clamped). Brand kit… › Videos sets and removes them and sets the stinger's cut point.
- Transitions: Stinger › "Your brand kit's stinger" stores stingerFile "brand:stinger" (kBrandStinger);
  applyTransitionFiles resolves it to the kit's file, pushSnapshot gives the engine the kit's cut point, and editing
  "When the scene switches…" while it's linked writes the kit. Saving the kit reloads the stinger (brandVideos).
- Media sources: "fromStartWhenShown" sends "start" (from the beginning, playing) when their scene goes live, from
  the same place designs get "enter" (pushSnapshot, top-level items of the program scene). Add a source › Your
  intro/outro video makes a once-through media source with it and "brandVideo": "intro"/"outro", which saving the
  kit points at the kit's current video.
- Fixed: media sources' menu. Their commands (pause, play, from the start, next, loop, shuffle, files) were added in
  0.8.3 without any menu offering them; right-click › Video or sound now has them, plus the new option.
- Tested in Wine with a 3-second H.264 clip in the kit: the intro played once and stopped ("Finished"), and
  restarted from the start each time its scene was switched to (frames 13, 22, 29; status 0:00 to 0:03); the
  transition set to the kit's stinger showed "Stinger · brand kit" and the kit's 1.5 s cut, played full-screen over
  the switch and ended on the new scene; changing the cut there saved 800 ms to brand.json.

## 45. Scene templates (status: built, 16 Nov 2026; released as 0.10.21)

- core/scene_templates (tested): five templates (gaming, chatting, art, reading, radio), each a list of parts
  bottom to top: Display, Camera, AuroraSky or Design (a design::templates() id), placed by left/top/width as
  fractions of the canvas (`place`: designs keep their page's shape, the camera is 16:9, the screen and sky fill the
  canvas); a part can follow an earlier one (a facecam frame its camera), placed by `frameAround` so the design's
  first element (its "Frame" opening) sits over the camera. Tests: every design exists, every follower follows an
  earlier camera through a "Frame" opening, every part lands on a 1920×1080 canvas (which caught the socials bar
  running off the bottom), shapes are kept, and the frame maths.
- New designs: Book card (900×320: a cover-shaped panel to swap for your cover, {book}, {series}, "NOW WRITING") and
  Show title (1100×220: ON AIR badge, {show}, {episode}, a microphone). Your details gains Book, Series, Radio show
  and Episode (profile.json, shared by the NorthRay apps).
- The window (capture_scene_templates.cpp): the Scenes panel's + offers An empty scene… or a template. The scene is
  named after it (made unique); the screen is the main monitor, reusing a display source already capturing it; the
  camera reuses the camera source in use (a device opens once) or the first Windows lists; none leaves out the camera
  and its frame and says so; the aurora sky takes the brand colours; the frame follows the camera.
- Tested in Wine: Gaming reused Display 1 and added the chat box and now playing, leaving out the camera with the
  note (Wine has no camera). Just chatting with a camera source present placed the camera at 77,86 1152×648 and its
  frame at 41,50 1224×756 (the opening exactly over it) following it, chat and lower third on the canvas. Wine can't
  open cameras at all (mf.dll MFCreateDeviceSource isn't implemented and aborts the process), so a camera source
  can't be run there; on Windows a missing camera is reported by the source.

## 46. The book card from Writing Studio (status: built, 16 Nov 2026; released as 0.10.22)

- Writing Studio's summary.json (%LOCALAPPDATA%\NorthRay\Writing Studio, "northray-writing-summary", written every
  30 s when it changes) now also carries the book being written (the book of the chapter in its editor): "book",
  "series" (its series line, "Book One of The Alvora Chronicles") and "cover" (the cover file in its library).
- Capture (writingTick, from the stats timer every 4 s) reads it when its time stamp changes and gives designs
  {book}, {series} and {cover} (ahead of Your details' Book and Series), {words} (today's words, 0 when the summary
  isn't today's) and {streak}. Your details shows "from Writing Studio: …" for those it supplies.
- Picture elements whose file holds a value ("{cover}") are filled in at render time; an empty one draws nothing
  on stream (a placeholder in the designer). The Book card has a "{cover}" picture over its coloured panel and book
  icon, so with no cover the panel shows.
- Tested in Wine: Writing Studio with the test library, opening The Envoy of Telvana in Writing (the editor itself
  needs WebView2, which Wine lacks), wrote the book, "Book One of The Alvora Chronicles" and the cover's library
  file; Capture's Reading and writing template then showed that cover, title and series on the book card.

## 47. The aurora pulse: a sound visualiser (status: built, 16 Nov 2026; released as 0.10.23)

- core/visualiser (tested): Analyser takes mono samples, and every 1024 samples runs a Hann-windowed 2048-point FFT;
  32 bands (4-64) spaced logarithmically from 40 Hz to 16 kHz each take their strongest bin in dB against a
  full-scale sine, mapped from -70 dB to 0 dB onto 0..1, rising with a 30 ms and falling with a 350 ms time constant;
  the loudness (RMS) the same way. Tests: the FFT, silence, a low tone in a low band and a high in a high one, a loud
  tone near full and a -40 dB one lower, the slow fall and fast rise, junk samples and settings clamped.
- core/aurora_sky gains the pulse: `bandAt` reads the bands as a clamped linear n×1 texture would; with a Pulse,
  each curtain's lower edge sits 0.12 lower and rises 0.3 with the band under it, its light scales by 0.25 + 1.4 ×
  band, and the light overall by 0.75 + 0.5 × loudness. `shader(s, true)` reads t0 (red the band, green the
  loudness). Tests: the band reading, loud lifts and brightens, bass lights the left, no pulse is the plain sky.
- SourceKind::AuroraPulse ("aurora-pulse"): the aurora sky's source with a band texture (kMaxBands×1, 8-bit RGBA,
  dynamic, written each frame from setSoundBands; 32-bit float textures were tried first and aren't filtered
  everywhere). Same item menu and brand following as the sky; Add a source › Aurora pulse; the Radio show template
  uses it.
- The window (visualiserTick, from the meter timer): while any aurora pulse exists it holds a mixer Tap on the Stream
  mix (bus 0), reads what has arrived, mixes it to mono for the analyser and hands the bands to the video thread;
  with none it removes the tap.
- Tested in Wine: with the bands silent the sky stays low and dim (mean brightness 13.6); with the low bands full
  (a test-only switch, removed) the left of the sky lit and the curtains rose (left 53.6, right 16.6). The analyser
  received the Stream mix (loudness up to 0.79 with the soundboard), but Wine runs the window's timers every few
  seconds instead of 30 times a second there, so the live response needs checking on Windows.

## 48. The NorthRay library (status: built, 16 Nov 2026; released as 0.10.24)

- cj/medialib (libs/cjcore, so every NorthRay app can use it; tested in cjcore_tests): the library in
  %LOCALAPPDATA%\NorthRay\Library: files\ (copies) and library.json ("northray-media-library"): items with an id,
  the stored file name (unique ignoring case; `storedName` makes "photo (2).png" and replaces characters Windows
  forbids), a name, a kind (picture, video, music, sound, overlay design, other; `kindOf` from the extension,
  ".nrdesign" for designs), a folder ('/'-separated, cleaned), tags (trimmed, lower case, sorted, no repeats, at
  most 30) and the day added; folders kept even when empty, listed with their parents; renaming a folder moves what's
  inside (not into itself), removing one moves its contents up. Search: every word in the name, folder, file or
  tags, filtered by kind, folder (and those inside) and tag, newest first. Reading skips bad or repeated items and
  carries the ids on after the highest.
- Capture (capture_library.cpp): top bar › Library opens the page: kinds, folders (indented) and tags (with counts) at
  the side; search (typed in), Add files… (several at once; copied with CopyFile, kind from the extension, into the
  folder being shown); cards with the picture itself (whole, not cropped) or the kind in words. Click: into the scene
  (Image, Media looping for videos and music and once for sounds, or a Design from the file); right-click: add to the
  scene, rename, kind, move to (or a new folder), add or remove a tag, show the file, remove (the file to the
  Recycle Bin). The designer's "To the library" saves the design as .nrdesign in "Overlay designs". library.json is
  read again when its time stamp changes (another app may have added to it) and written through a temporary file;
  if it can't be read it's left alone and nothing is changed.
- Also: About and Settings › About no longer say Capture is at Milestone 2.
- Tested in Wine with a cover, a video, a sound and a design in the library: kinds, nested folders and tags shown;
  the #alvora tag showed only the cover; searching "soon" found the design; clicking it put it in the scene; adding
  the tag "telvana" from the right-click menu was saved in library.json.

## 49. Template packs and design thumbnails (status: built, 16 Nov 2026; released as 0.10.25)

- cj/medialib (tested): `buildPack` zips (miniz, in memory) a folder and those inside it, or everything, as
  files/<n>-<stored name> plus pack.json ("northray-pack": name; per item its entry, file, name, kind, folder within
  the pack, tags); items whose files can't be read are left out; at most 2000 items and 1 GB. `readPack` takes only
  entries under files/, checks declared sizes against what's left of the 1 GB before unpacking, gives every file a
  fresh stored name (so names like "..\\..\\Windows\\a.png" become "a.png" and "../../up" folders are cleaned) and
  adds the items under the chosen folder with their folders and tags. Tests: a round trip into a library with a
  clashing name, a hostile pack (an entry outside files/, paths in names), junk, a cut-off zip; checked for leaks
  with AddressSanitizer (which caught the finished buffer not being freed). cjcore now links miniz.
- Capture's Library: Export folder… / Export all… (save dialog, the folder's name as the pack's) and Import a pack…
  (into "Packs/<pack name>", then shown). Overlay designs get a picture of the design (rendered at most 360 wide into
  Library\thumbs, again when the design file is newer).
- Tested in Wine: the design thumbnail showed; Export all saved kit.nrpack (4 files and pack.json; Python's zipfile
  reads it); importing it added the 4 items under Packs/My NorthRay library with Books/Alvora and Overlays inside,
  fresh names ("Envoy cover (2).png") and their tags.

## 50. Keeping in step with Writing Studio (status: built, 17 Nov 2026; released as 0.10.26)

- Writing Studio writes summary.json every 2 s (was 30 s), still only when its text changed, so a chapter switch,
  a book change or new words reach Capture within about two seconds. It adds "chapter" (the open chapter's label),
  "bookWords" (the book's words) and counts unsaved typing: today's words and the book's words add the editor's live
  count less the chapter's saved count (never below 0). Its page still repaints every 30 s as before.
- Capture's writingTick looks every ~2 s (the stats timer) and gives designs {chapter}, {words} and {goal} (grouped,
  "1,240"), {today} ("1,240 of 2,000 words today", or "1,240 words today" without a goal), {bookwords}, {bookline}
  ("48,312 words in the book"), {streak} and {streakline} ("4-day streak", empty at 0). loadProfile always defines
  these (empty without Writing Studio), so designs never show the braces; empty phrases drop out of "·" lines.
- The Writing progress design (1100 × 300, "On stream") and the Reading and writing scene template (placed under
  the chat).
- Tested in Wine: Writing Studio opening The Envoy of Telvana wrote "chapter": "Chapter 1" and "bookWords" within
  seconds; Capture's Writing progress showed "Chapter 1", "0 of 1,000 words today"; changing the summary by hand
  (chapter, 1,240 words, 48,312 in the book, streak 4) showed on the overlay within a few seconds with no action.
  Live typing needs Writing Studio's editor (WebView2), so it's for checking on Windows.

## 51. Chat bubble sides, and the chat element crash (status: built, 17 Nov 2026; released as 0.10.27)

- The crash CJ hit (0xC0000005 when selecting a chat element in the designer, on real Windows since 0.10.3):
  drawDesignerProperties named the element's kind from a 7-entry array, and Kind::Chat is the 8th, so it read past
  the array and built a std::wstring from a bad pointer. Wine swallows exceptions in window callbacks
  ("dispatch_callback ignoring exception"), so it never showed in testing. Fixed with the CHAT name, a static_assert
  that the array covers every kind, and a bounds check.
- cjui's crash report (every app) now writes the faulting module+offset and an unwound stack (RtlLookupFunctionEntry /
  RtlVirtualUnwind, 24 frames, module+offset only). NORTHRAY_TRACE_FAULTS=1 adds a vectored handler logging the first
  three access violations to fault-trace.log, for faults Wine swallows; that's how this one was found (offsets mapped
  with nm against the exe).
- Element.chatSide ("left", "right", "mine"; JSON chat.side; anything else is ""): placed bubbles are sized to the
  message with left-aligned text inside; "mine" puts messages with the broadcaster badge on the right and others on
  the left, at most 80% of the box wide. "" keeps the old behaviour (Align). Designer: Bubbles Left / Right / Split;
  Align is hidden while Bubbles is set. A SampleStreamer sample message shows the split in the designer. Tested
  (core: round trip and invalid values; Wine: all three sides, no faults with tracing on).

## 52. The chapter's word count, and the aurora wave (status: built, 17 Nov 2026; released as 0.10.28)

- Writing Studio's summary adds "chapterWords": nodeTotalWords of the open chapter (with its scenes) plus the
  editor's unsaved difference. Capture gives {chapterwords} ("3,214") and {chapterline} ("3,214 words"; empty without
  Writing Studio). The Writing progress template has a "Chapter words" element, right-aligned on the CHAPTER line.
  design::addChapterWords upgrades older Writing progress documents once (by document name, a Text element named
  Label reading CHAPTER, and no {chapter...} element already), copying the label's place, size, font, fill and
  entrance; loadScenes runs it over every Design source and saves if anything changed.
- aurora_sky::Settings::wave (JSON "wave"; only used by the pulse): three ribbons, waveLine(k, u, t, band) =
  0.56 + (k-1)·0.05 + (0.035 + 0.26·band)·sin(...) + a small ripple. Each ribbon is a Gaussian core (thickness grows
  with the band and loudness), an exponential halo, and rays above it. The band is smoothed along u (5 taps, ±0.06,
  weights 1-2-3-2-1) so narrow bands swell rather than kink. C++ sample() and the HLSL (WAVE static const; the curtain
  loop runs 0 times when WAVE) are the same sums. Menu: Sources + › Aurora wave; the pulse's submenu has Pulse /
  Wave (a source named "Aurora pulse"/"Aurora wave" is renamed to match when that name is free).
- Tested: core (round trip; quiet swing < 0.06, loud > 0.2; bright on the line, dark far from it; loud brighter
  than quiet; a sky ignores wave; addChapterWords adds once, keeps the label's style, leaves other or changed designs
  alone). Wine: Writing Studio wrote chapterWords; the 0.10.26 overlay in the test scenes gained "3,214 words"; the
  wave drew and swelled with the soundboard's Fanfare; switching to Pulse and back.

## 53. The reserved-word fix (released as 0.10.29, 17 Nov 2026)

- 0.10.28's wave HLSL declared `float line`; `line` is a geometry-shader primitive keyword in fxc/D3DCompile, so
  every aurora shader (the text is shared, WAVE or not) failed on Windows with X3000 at (25,19-22). Wine's compiler
  accepted it, so the Wine test passed. Renamed to `centre`.
- Tests: hlslReservedWordIn() tokenises a shader (skipping // comments and # directives) against fxc's reserved
  words; every aurora_sky::shader combination (pulse, wave, stars) and every built-in transition (wrapped by
  shaderSource) must be clean, and each aurora shader must end whole (not truncated by its snprintf buffer).
- Gpu::compilePixelShader's source name is "capture-shader" (was "transition", which made aurora errors look like
  transition errors). Lesson: Wine's shader compiler is more lenient than Windows'; shader text needs its own checks.

## 54. Clean sound from files, and monitoring them (released as 0.10.30, 17 Nov 2026)

- The cause: MediaSource's audio thread wrote a whole decoded (and resampled) block to its Ring once the block's
  timestamp was within 40 ms. FLAC (4096 frames), WAV and Media Foundation's resampled output arrive in blocks of
  ~90 ms to hundreds of ms, so the Ring often held over 100 ms, and Mixer::block() trims any source past 10 blocks
  back to 2, dropping the rest. Measured in Wine on a 10 s Stream mix clip of a 440 Hz tone (44.1 kHz FLAC):
  18.9 discontinuities a second and 95 silent gaps up to 90 ms.
- Now: writes are limited to the frames due within kLead (50 ms) of the playback position and to kMost (80 ms)
  in the Ring, polling every 2 ms (the mixer's timeBeginPeriod(1) makes Sleep(2) about 2 ms). Same test: 1 event
  per clip for FLAC, WAV and MP3, at the file's loop point (a restarting sine), no silent gaps over 1 ms.
  The analysis predicts each sample from the previous two (x[n] = 2cos(w)x[n-1] - x[n-2]) and counts misses.
- Monitoring: a Media source's default buses add Monitor (a file can't feed back). Media sources with saved
  buses are given Monitor once at load (marked "monitorChosen"); any later change of buses sets monitorChosen,
  so a choice to turn it off stays off.

## 55. Polish, batch 1 (released as 0.10.31, 18 Nov 2026; from CJ's 200 answers)

- Already there, so nothing to build: snapping in the preview (edges and centre lines; Shift turns it off), the
  preview's right-click giving the source's menu, and opening on the last scene (scenesChanged saves "current").
- Double-click a source row: Design kinds call openDesigner, anything else opens itemMenu at the pointer.
- Live: drawPreview wraps drawPreviewArea and adds a 2.5 px red outline and a LIVE tag when streamStatusText says
  live; drawStreamControls shows "LIVE h:mm:ss" (Font::Title) under the button, from the longest destination.
- Record and stream buttons: 50 DIPs tall, Font::Heading.
- cjui: MenuItem::heading(label), a separator with a label (kHeadH 30, upper-case Font::Label, a rule above;
  not choosable, hidden while searching). The add-source menu is grouped under five headings.
- cjui: AppWindow::tooltip(hitId, text), registered per frame; shown after 550 ms of hover (kTipTimer), hidden on
  a click until the hover changes, placed by the pointer and kept inside the window. Capture registers them for
  the record, stream and marker buttons (withHotkey adds the user's hotkey), the top bar and each filter setting
  (paramTip: one sentence per parameter key).
- Library: sizes (fs::file_size, cached per open) and lengths (video::mediaLength, Media Foundation on a detached
  worker thread with its own COM/MF start-up, results in a shared map, InvalidateRect when found); the folder is
  saved as settings.libraryFolder and restored on open if it still exists.
- Tested in Wine: the grouped menu, tooltips, double-click to the designer, sizes and lengths (0:03, 0:20), the
  folder remembered. Not testable here: the live outline and timer (no streaming in Wine).
- Environment note: on the new build machine Wine's wineserver aborted ("free(): invalid pointer") until
  /run/user/0 existed (mkdir -p /run/user/0; chmod 700).

## 56. Polish, batch 2 (released as 0.10.32, 18 Nov 2026)

- Undo: noteUndo runs where the scenes are saved (kSaveTimer, 600 ms after the last change), so quick changes
  settle into one step. Snapshots are the collection's JSON without "current" (undoForm), 50 kept; Ctrl+Z / Ctrl+Y
  (Ctrl+Shift+Z) restore with fromJson, keep the scene you're on, then syncSources + syncAudio and save at once.
  Cleared when another collection loads. The designer keeps its own undo.
- Source pictures: Engine::makeThumbs on the video thread every 1.5 s while the window asks (wantThumbs, from
  drawSources and the test page): each item of the shown scene (or the studio preview) drawn fitted into a 96x54
  target, copied to one staging texture and read back as RGBA, keyed by item id with a generation number. The UI
  draws them with drawPixels under "thumb:<item>:<gen>" and forgets the previous key. Off in low-resource mode.
- Test my setup (capture_setuptest.cpp): sources with picture and status; sound sources with live meters, ticked when
  heardAt is within 10 s, warned when muted, not sent to Stream or unavailable; a test chime from its own
  SoundboardPlayer added to the mix as "#testsound" on routing::Monitor only; the encoder, destinations' keys and
  keepUpAdvice.
- keepUpAdvice: below 85% of the output frame rate, the reason (GPU ms over 80% of the frame's budget, system CPU over
  90%, render ms over budget, else other programs) as a few words under the preview and full advice as its tooltip.
- Compact layout (settings.compactLayout: 0 automatic below 1500 x 860 DIPs, 1 on, 2 off): scene rows 27 (step 29),
  source rows 34 on one line (status in the tooltip, a red dot for problems, 43x24 pictures), big buttons 40.
- Start-up: the window shows first with "Opening your scenes..." (starting_), then loads; What's new (kWhatsNew,
  generated by CMake from releases/capture.json when its first entry is this version) shows once 700 ms after
  start when settings.seenVersion differs and the settings file already existed (an update, not a new install).
- Crash reopen (cjui): reportCrash asks "Open ... again now?" (MB_YESNO) with the app's note (setCrashReopenNote) and
  relaunches with NORTHRAY_REOPENED=1; a reopened copy that fails within 30 s isn't offered again.
- Fade-ins: a page (settings, designer, filters, wizard, checklist, test page, Library) and newly shown panels fade
  from the paper colour (anim 9/s); Windows' animation setting turns them off.
- Tested in Wine: undo/redo of hiding a source, the pictures, the opening screen, What's new after an update, compact
  rows, the test page (sources, meters, chime button, encoder and keys), late-frame reasons. Not testable here: the
  chime through real headphones, the crash reopen (no crash to cause safely).

## 57. The stream's extras (released as 0.11.0, 18 Nov 2026)

The first batch of CJ's feature answers (docs/answers-2026-11-18.md). Window code in `src/capture_live.cpp`; the pure
parts in `core/live` (tested).

- `core/live`: `Stamp` (a local date and time to the minute, "2026-11-22T19:00"), `civilMs` (days from civil, for
  differences between local times; no time zones involved), `stampWords` ("Sun 22 Nov, 19:00"), `addMinutes`. The
  stream report: `Collector` (begin with the scene, `sample` about once a second with the total upload rate and frames
  dropped so far, averaged into 5 s buckets, at most 12 hours of them; `scene` at each live-scene change, adding up each
  scene's live time; `destination` with each destination's final figures, added together if it was restarted;
  `finish`), and `toJson`/`fromJson` (lenient: bad values are skipped or clamped).
- The plan (Settings: `goLiveAt`, `goLiveStarts`, `goLiveScene`, `goLiveDone`, `plannedMinutes`, `nextStreamAt`): the
  window passes it to `video::setStreamPlan` each second, and `clockNow` fills `Clock::goLiveInMs`, `plannedMs` and
  `nextText` for the new timer modes `golive`, `remaining` and `next` (core/design `timerText`). Going live by itself
  happens only within two minutes after the time, once per time (`goLiveDone` is saved, so reopening doesn't repeat it).
- `liveStateCheck` (after starting or stopping a destination, and each second) notices streaming start and stop: on
  start the report begins and, with `recordWhileStreaming`, a recording starts (and only that one stops with the
  stream). On stop: Be right back ends, and a report of 10 s or more is saved to `<state>\Reports\<date time>.json`
  (the newest 100 kept) and shown. Destinations report their own figures when stopped (`stopDestination`).
- `liveSceneCheck` (from `scenesChanged`, `takeToProgram` and each second): the live scene is `programScene()`. The
  sources to leave out of the mix (`autoMuted_`) are the live scene's `mutes` plus, during Be right back, every
  microphone (InputAudio); a change calls `syncAudio`, which uses `mixMuted` (the source's own mute or `autoMuted_`) so
  the saved mute is never touched. A changed live scene adds a chapter (`sceneChapters`, while recording; the first one
  at 0 s when the recording starts) and a scene change to the report.
- Be right back (`toggleBrb`, the button under Start streaming once a scene is chosen, and hotkey action `brb`): remembers
  the live scene, switches to `brbScene` and mutes the mics; again (or switching away from that scene) goes back.
- Scenes keep `mutes` (sound source ids, at most 64; removed with the source); the scene menu's "Mute while this scene is
  live" sets them. Hotkey action `toggle-source` shows or hides a source by name in the live scene.
- Tested in Wine: scheduled go-live firing (then failing for want of an encoder, as expected here), Be right back on and
  off, scene muting and its mixer label, the settings section, the report page (from a sample file, since nothing can
  stream here). Not testable here: a real stream's report collection, recording alongside, chapters in a real MP4.

## 58. Pointer, time-lapse, notes, destinations (released as 0.12.0, 19 Nov 2026)

The second batch of CJ's feature answers.

- Pointer highlight: `core/pointer` (tested): `Style` on a SceneItem (JSON "pointer": on, size 12-240 source pixels,
  halo strength, clicks, colour, second colour for right clicks), `toCanvas` (a desktop point through the item's area,
  picture size, crop, mirroring, size and rotation to the canvas; false outside or cropped away), click `Rings`
  (450 ms, growing from half to twice the halo, fading; at most 16), and the white disc and ring pictures.
  `VideoSource::screenArea` gives the desktop rectangle a picture shows: Display (its monitor), Window (DWM extended
  frame bounds) and Region (the monitor's plus its rectangle). The engine draws, after each item with the highlight
  on, its click rings and the halo with drawTexture from small per-colour textures. The pointer comes from
  GetCursorInfo once a frame; clicks from a WH_MOUSE_LL hook on its own thread (it only notes where a button went
  down, never blocks; polling the buttons once a frame missed quick clicks), started while any shown item wants click
  rings and stopped 5 s after none does. Item menu › Highlight the pointer.
- Time-lapse: Settings.timelapse (0 off; 1, 2, 5, 10, 30 s) → RecordSettings.timelapse. The recorder takes the first
  composed frame of each interval (live::timelapseFrame) and numbers them one after another on the output frame grid;
  no audio tracks; " time-lapse" in the file name. live::timelapseWords ("An hour becomes 1 min (60× faster)").
  Not run here (no encoder).
- Notes panel (Layout::Notes, capture_notes.cpp): Write is a multi-line EDIT (no scroll bar, wheel scrolls via a
  subclass), placed in the panel each frame and hidden when the panel isn't drawn or the ask card is open; saved to
  <state>\notes.txt (UTF-8, through a temporary file) 0.9 s after typing stops and on close. Read draws the text with
  cjui's new Gfx::textSized (any size, cached formats) on a dark card, scrolled by a 16 ms timer at one of seven speeds
  (settings notesSpeed, notesSize), the wheel scrolls it, a marker a third of the way down, faded edges; drawn
  play/pause and top icons (symbol characters aren't in every font).
- What each destination gets (capture_destview.cpp): a page with a card per destination: host only (nothing before
  an @, never a key), state and live figures, the picture (canvas, fps, its bitrate, encodes shared by bitrate), the
  sound (its bus, the bus's live meter: the window now keeps meters for all eight buses, and what's on that mix).
- Do not disturb: researched. Windows.UI.Shell.FocusSessionManager can start Focus (which turns on Do not disturb)
  only with a Limited Access Feature token from Microsoft, so Capture doesn't switch it: a reminder when going live
  (settings dndReminder, on) and a button opening ms-settings:notifications, where Windows' own automatic rules are.
- Tested in Wine: the halo and a click ring on a display capture, the menus; typing notes (saved to notes.txt and
  loaded at start), Read scrolling by itself; the destinations page with live meters; the settings rows. Not testable
  here: time-lapse files, the pointer over a real window capture (Wine has no WGC).
