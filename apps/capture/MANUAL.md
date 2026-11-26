# NorthRay Capture: Development and Reference Manual

| | |
|---|---|
| Application | NorthRay Capture (`NorthRayCapture.exe`) |
| Version at the time of writing | 0.22.0 (`apps/capture/VERSION`) |
| Status | **In development**. Released as an early version. Recording and streaming are built but **not yet tried on real hardware or a real platform** |
| Source | `apps/capture/` (C++20; Direct3D 11, Media Foundation, WASAPI; UI in `libs/cjui`) |
| Architecture notes | `apps/capture/docs/ARCHITECTURE.md`: decisions (§0), systems, and one section per milestone or release (§8 to §69) |
| Release notes | `shared/metadata/releases/capture.json` |
| Plug-in SDK | `apps/capture/sdk/` (`northray_audio_plugin.h`, README, examples) |
| Manual last checked against code | 26 November 2026 |

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

---

## 1. Overview

**Purpose.** A livestreaming and recording app for Windows, built towards a real alternative to OBS Studio.

**Audience.** Streamers and creators: gaming, just chatting, art, writing, radio-style shows.

**Philosophy.**
- **Per-application audio.** Stream a game and your microphone while music and voice chat stay private.
- **Multistream from one encode.**
- **Light on older PCs.** The target machine class is an i7-3770K with a GTX 960.
- **GPU-only video path.** Textures never leave the GPU between capture, compose and encode.
- **Secrets stay safe.** Stream keys live only in Windows Credential Manager, and every log line is redacted.
- **No game hooking.** Capture never injects into games.
- **Honest status.** It never shows a stream as live unless it is.

**Where it sits.** One of the four suite apps. It reads Writing Studio's `summary.json` for writing overlays, shares the NorthRay media library (`%LOCALAPPDATA%\NorthRay\Library`), and can be opened on a scene by the Launcher.

## 2. Features

The full list is in `shared/metadata/apps/capture.json` (features) and the release notes. What follows is grouped.

### Implemented

- **Interface size** (0.21.3; since 0.21.4 the body only, the top bar and the bottom strip keeping their size): 50–200%, from Settings › Appearance (presets and a slider) or Ctrl with + / − / 0 and Ctrl with the wheel. Kept on this PC (cjui).
- **Panels that follow the window** (0.22): when the panels under the preview can't each have about 230 points across (200 in compact layout), they go into rows, filled evenly in their order, and the panel area grows to give each row room (up to 70% of the space). Dividers show only in a single row; in rows, dragging a panel's title onto another swaps them.
**Video sources**
- A whole display: Desktop Duplication, with Windows Graphics Capture and a compatibility mode as fallbacks.
- One window, even when covered (WGC). It's found again when the program restarts.
- Cameras and capture cards (Media Foundation).
- Images, including animated GIFs.
- Text, colours, slideshows, a screen region.
- Video and sound files and playlists.
- Scenes inside scenes, and groups.
- A sync test source.

**Preview and editing**
- Drag, resize, crop (Alt), snapping, rotate, mirror, nudge.
- Exact position, size, rotation and opacity.
- Undo.
- Studio Mode (Preview and Program).
- Transitions: cut, fade, slide, swipe, luma wipe, stinger, custom shader, aurora sweep.

**Scenes**
- Folders, export and import, Ctrl+1 to Ctrl+9.
- Profiles and scene collections.
- Scene templates: gaming, just chatting, art, reading and writing, radio show.

**Audio**
- Microphones and inputs; desktop audio (loopback).
- On Windows 11, **one program's sound on its own** (process loopback).
- Mixer with meters, faders and mute.
- Filters: gain, gate, noise suppression, expander, compressor, limiter, EQ, balance, delay, plus presets.
- Voice effects: reverb, pitch shift, radio voice.

**Routing**
- Each source can go to the stream, recording tracks 1 to 6 and the monitor, independently.
- Each destination can have its own mix.
- A second audio output.
- Warnings when private audio would leak through desktop audio, or when sound would feed back.
- 10-second test clips.

**Optional audio plug-ins** (documented C interface)
- Off until the user switches each one on, after seeing its SHA-256 and signature.
- Switched off again if the file changes.

**Recording**
- MKV with hardware H.264 (NVIDIA, AMD or Intel through Media Foundation) or Windows' software encoder, and up to 6 AAC tracks.
- Stays playable after a crash.
- An MP4 copy made by remuxing, not re-encoding, with markers as chapters.
- Time-lapse.

**Streaming**
- Twitch, YouTube, Kick, or custom RTMP/RTMPS (SChannel TLS, certificate checked).
- Automatic reconnection.
- **Multistreaming** to up to 6 destinations. Destinations at the same bitrate share one encode, and each has its own buffer and status.
- Keys in Credential Manager.
- Stream reports.
- Record while streaming.
- Go live at a set time.
- A Do not disturb reminder.

**Designs and overlays**
- An overlay designer: text, shapes, pictures, gradients, effects, animations, layers, snapping, align and distribute, groups.
- Templates: Starting soon, Be right back, Ending, lower third, facecam frame, panels.
- Timers.
- QR codes.
- Icons. These are general ones; official social logos are deliberately not drawn as imitations.
- A brand kit.
- **Themes:**
  - Nine built in: NorthRay aurora, Bookish, Dark fantasy, Sci-fi, Cosy, Minimal (charcoal and white), Radio, Steampunk.
  - Your own themes, made by copying a built-in one.
  - One switch restyles the whole stream.
- Export as PNG, or an animated GIF of the entrance.
- Camera border maker; green screen.

**Live elements**
- Read-only Twitch chat (no sign-in) in designs and in a Chat panel.
- Now playing.
- A music folder with auto-duck.
- A soundboard with 12 starter sounds made by NorthRay.
- Animated GPU backgrounds: Aurora sky, pulse and wave, with a design per theme.

**Writing Studio link** (one way: Studio writes, Capture reads)
- A Writing card with toggleable parts: book, chapter, word progress, viewpoint, cast (scrolling), sprint timer, celebration, stream words.
- A spoiler-safe switch.
- Scene automation by Writing Studio's workspace.
- Chapter markers.

**Other**
- Automation: switch scenes when a program opens or closes.
- Global hotkeys. A Stream Deck can press them through its built-in Hotkey action; there is no dedicated plug-in.
- Setup wizard with a Twitch bandwidth test.
- Pre-live checklist.
- Pointer and click highlight.
- Notes panel with a teleprompter.
- A view of what each destination receives.
- The NorthRay library: folders, tags, search, template packs.
- Workspace layouts.
- Low-resource mode.
- Calm controls (0.21.1); website link (0.21.2).

### Not yet verified (built, but not run for real)

- **Interface size (0.172) and panels in rows (0.22):** built and cross-compiled (the row rule is unit-tested), not yet run on Windows (Wine would not start in the build environment).
**Recording with a real hardware encoder.** Checked only as far as the file format, against ffmpeg (`tools/check-mkv.sh`, `capture_mkv_check`).

**Streaming and multistreaming against real platforms.** Checked against a local test server (`tools/check-rtmp.sh`, `capture_rtmp_check`, `tools/check-destinations.sh`).

**Anything needing real devices or Windows 11 features**, which Wine can't provide:
- Process-loopback audio.
- Cameras.
- WGC.
- Hardware encoders.
- Hotkeys under real games.

**Performance** on the target machine against OBS under the same scene. Planned for Milestone 12 in the architecture document; not done.

### Parked or decided against

- **Signing in to Twitch and YouTube** (OAuth, PKCE, device flow): built, then taken out of the app in 0.7.0. The director chose pasted stream keys. `core/oauth` is still built and tested; `src/output/accounts.cpp` and `http.cpp` are not linked into the app. `oauth-clients.json` holds **empty** client IDs on purpose.
- **Virtual camera:** "not planned" (0.11.0); the placeholder was removed.
- **Live captions; a live bitrate graph; TikTok:** answered "No" on 18 Nov.
- **Game-capture hooking:** not done, by decision. It would need a separate discussion first.

### Planned (from the director's answers; not in the release notes yet)

- Web pages as sources (WebView2 browser sources), including Send to Capture from NorthRay Browser.
- Vertical canvases per destination.
- A replay buffer.
- Camera background blur or removal without a green screen.
- Camera colour correction.
- Import scenes from OBS.
- Back up all settings, scenes and designs to one file.
- Draw on the stream live.
- A dedicated Stream Deck plug-in.
- Auto-framing.
- WebM export. Windows has no WebM encoder, so this would mean bundling one.

### Known limitations

- Windows 10 build 20348+ or Windows 11 is needed for per-program audio.
- No screen-reader support for drawn controls (cjui has no UI Automation).
- Chat is Twitch-only and read-only.

## 3. Interface and user experience

- **Layout:** a docked workspace with panels that can be resized, rearranged, hidden and saved as layouts:
  - Scenes, sources, preview (and program in Studio Mode), mixer, controls (Start streaming, Start recording, a big live timer), chat, notes.
  - A compact layout for laptops.
  - The NorthRay night-sky top area, with the caption buttons.
- **Principles:**
  - Honest status: a red LIVE outline only when live, and plain-words reasons for late frames.
  - Tooltips with shortcuts.
  - Undo.
  - A grouped + menu (Video, Sound, Designs, Backgrounds).
- **Workflows:** first-run wizard; build scenes from templates; route audio; test with clips; go live (with checklist); record; review the stream report.
- **Accessibility:** keyboard access, focus rings, contrast per the suite palette; no UI Automation.

## 4. Technical documentation

| Item | Detail |
|---|---|
| Language | C++20 |
| UI | `libs/cjui` (chosen over Qt 6 on 14 Nov 2026) |
| Video | Direct3D 11, one device shared by capture, compose and encode. Preview through a DXGI swap chain child window |
| Capture APIs | Desktop Duplication, Windows.Graphics.Capture, Media Foundation source reader |
| Audio | WASAPI shared mode (event-driven), loopback, process loopback |
| Encoding | Media Foundation transforms: hardware H.264/HEVC through NVENC, AMF or Quick Sync, the software H.264 MFT, and the AAC MFT |
| Muxing | Capture's own FLV (RTMP), fragmented MP4 and MKV writers |
| Network | Capture's own RTMP client over Winsock; RTMPS through SChannel |
| Secrets | Credential Manager, targets `NorthRayCapture/<destination id>` |
| Third-party | QR Code generator (Project Nayuki, MIT), vendored in `libs/third_party/qrcodegen` |

**Structure**

`core/` (`ncap/*.hpp`) is portable and unit-tested:

| Area | Modules |
|---|---|
| Audio | `audio`, `dsp`, `routing`, `soundboard`, `plugins` |
| Scenes and layout | `scenes`, `placement`, `transition`, `scene_templates` |
| Designs and visuals | `design` (with themes), `border`, `keying`, `colour`, `aurora_sky`, `visualiser`, `gif`, `pointer`, `pointer_style` |
| Containers and network | `mkv`, `mp4`, `rtmp`, `chat` |
| Setup and running | `live`, `wizard`, `checklist`, `hotkeys`, `automation`, `profiles` |
| Other | `media`, `diag`, `oauth`, `sourceutil` |

`src/`:

| Folder or file | Contents |
|---|---|
| `audio/` | Device capture, mixer, plug-in host, soundboard player |
| `video/` | GPU, engine, sources, WGC, design rendering, media sources |
| `encode/` | Audio and video encoders |
| `output/` | Recorder, streamer, destinations, credentials, Twitch chat, MP4 copy; `accounts` and `http` are parked |
| `capture_*.cpp` | One file per feature: designer, library, themes, wizard, automation, hotkeys, record, stream, and so on |
| `log.*` | Redacted, size-capped daily logs |
| `settings.*` | Settings |
| `main.cpp` | Start-up; `--collection`/`--scene` |

**Data storage**

`%LOCALAPPDATA%\NorthRay\Capture\`:

| Path | Contents |
|---|---|
| `settings.json` | App settings |
| `profiles\<name>.json` | Encoding, outputs, routing |
| `scenes\<collection>.json` | Scene collections |
| `layouts\<name>.json` | Saved workspace layouts |
| `logs\capture-YYYY-MM-DD.log` | Daily logs |
| `oauth-clients.json` | Optional override; empty by default |

Elsewhere:

| Path | Contents |
|---|---|
| `%LOCALAPPDATA%\NorthRay\Library\` | The shared media library |
| `%LOCALAPPDATA%\NorthRay\profile.json` | Shared details that templates fill in (socials and the like) |
| The Videos folder | Recordings, by default |
| `Pictures\NorthRay Capture` | Saved pictures |

**Environment variable:** `NORTHRAY_CHAT_TEST_URL` points the chat reader at a test server. It is for development only.

**External integrations**
- RTMP and RTMPS ingest servers (Twitch, YouTube, Kick, custom).
- Twitch chat (read-only, over Twitch's chat WebSocket `wss://irc-ws.chat.twitch.tv`, no sign-in).
- Writing Studio's `summary.json`.
- The Launcher's command line.

## 5. Development

**Status:** in development, at milestone level 10+ of the brief's 14.

| Milestone | Release | Content |
|---|---|---|
| 1 | | Foundation |
| 2 | 0.2 | Video capture |
| 3 | 0.3 | Audio |
| 4 | 0.4 | Routing |
| 5 | 0.5 | Recording |
| 6 | 0.6 | Streaming |
| 7 | 0.7 | Multistreaming |
| 8 | 0.8.x | Transitions and Studio Mode, sources, folders, media |
| 9 | 0.9.x | Audio filters, routing, plug-ins |
| 10 | 0.10.x | Overlays, brand kit, chat, wizard, many extras |

After milestone 10:
- 0.11 to 0.13: stream extras and the Writing Studio link.
- 0.14 to 0.18: designer, effects, pictures, the two looks.
- 0.19: open on a scene from the Launcher.
- 0.20: the Writing card.
- 0.21: themes.

**Milestones 11 to 14** from the original brief aren't recorded, and the director doesn't have them (26 Nov 2026). The architecture document names only Milestone 12 (performance against OBS) and 14 (release). Define the next milestones from the outstanding list below.

**Outstanding**
- Real-hardware verification of recording and streaming. This is the top priority before calling it stable.
- The planned list above.
- Performance measurement.
- UI Automation.

**Priorities** (the director, 15 Nov): first, the overlay designer and templates; second, the setup wizard and profiles. Both are now built.

## 6. Maintenance

**Run:** `NorthRayCapture-Setup`, or `build/windows/apps/capture/NorthRayCapture.exe [--collection <name> --scene <name>]`.

**Build:** `tools/build-windows.sh`. The capture check tools build when enabled in `apps/capture/CMakeLists.txt`.

**Test**
- `capturecore_tests`: 45,641 checks, all passing on 26 Nov 2026.
- Format checks: `tools/check-mkv.sh` (MKV against ffmpeg), `tools/check-rtmp.sh` (against a local RTMP server), `tools/check-destinations.sh` (multistream), `tools/check-accounts.sh` with `tools/accounts-mock.py` (the parked OAuth code).
- Also `capture_mp4_copy_check` and `capture_design_render_check`.

**Troubleshooting**
- Check `logs\capture-<date>.log` first. Keys and tokens are redacted.
- No per-program audio: needs Windows 11 or 10 build 20348+.
- Black window capture: try a display source with Desktop Duplication.
- An encoder isn't offered: only encoders Windows reports are listed.
- A stream doesn't start: check the key (stored in Credential Manager) and that the server's certificate is valid.

**Implementation notes**
- Never log a stream key or token. All logging goes through `redact()`.
- Plug-ins are never enabled automatically.
- A request from the Launcher to open on a scene never changes the scene while Capture is recording or streaming.
