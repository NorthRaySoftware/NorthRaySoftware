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
- No installer or Launcher entry yet, deliberately, until Capture captures something (M2+).
