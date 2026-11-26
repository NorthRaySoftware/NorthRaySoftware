# Development Environment and Setup

How to build, test and release NorthRay from a fresh copy. **No secrets are stored in this repository or this document.** Where credentials are needed, it says where they come from.

## 1. Getting the source

**From the archive**
- `03_Application_Projects/NorthRay-source/` is a working copy without build outputs.
- `10_Git_History/northray.bundle` has the full git history. Restore it with:

```
git clone -b source 10_Git_History/northray.bundle NorthRay
```

**From GitHub**
- `https://github.com/NorthRaySoftware/NorthRaySoftware`. Branch **`source`** is development; **`main`** is the published website.
- Pushing needs GitHub access to that repository: the director's account, or a token or credential configured on the machine. **Never** write the token into a file in the repository.

## 2. Toolchains

| Purpose | Requirement |
|---|---|
| Windows builds, cross-compiled on Linux (how every release so far was made) | `mingw-w64` (GCC 13), CMake ≥ 3.21, NSIS (`makensis`) for installers |
| Windows builds on Windows | Visual Studio 2022 with "Desktop development with C++". Untested recently: releases use MinGW |
| Native tests (Linux) | GCC or Clang with C++20, CMake, Ninja. The tests build with ASan and UBSan; run with `ASAN_OPTIONS=detect_leaks=0` |
| Writing Studio editor | Node.js ≥ 22, npm (`apps/studio/editor`: esbuild, ProseMirror) |
| Website | Node.js ≥ 22, Astro 7.4.2 (`website/`) |
| UI checks without Windows | Wine and Xvfb (`Xvfb :99`, `wine explorer /desktop=...`), ImageMagick `import` for screenshots, xdotool for input |
| Editor checks | Chromium with Playwright (`tools/editor-harness.cjs`) |
| Capture format checks | ffmpeg / ffprobe (`tools/check-mkv.sh`); a local RTMP server (`tools/check-rtmp.sh`) |

Third-party libraries are vendored in `libs/third_party`, and PDFium's Windows DLL in `apps/studio/third_party/pdfium`, so there is no download step. The exception is npm packages, fetched with `npm install`.

## 3. Commands

```
# Native tests (core libraries of every app)
cmake -S . -B build/native -G Ninja && ninja -C build/native && (cd build/native && ASAN_OPTIONS=detect_leaks=0 ctest)

# Writing Studio editor
cd apps/studio/editor && npm install && npm run build && npm test

# Windows build of everything, then installers + catalogue.json
tools/build-windows.sh
tools/make-installers.sh

# Website
cd website && npm install && npm run build      # output: website/dist

# Metadata
node tools/validate-metadata.mjs
```

**Expected results** on 26 Nov 2026: all 5 native test suites pass.

| Suite | Checks |
|---|---|
| studiocore | 2,384 |
| cjcore | 68 |
| browsercore | 315 |
| filescore | 373 |
| capturecore | 45,641 |
| Editor (npm test) | 28 tests |

## 4. Environment variables

None are needed for normal builds or use. The development and test switches are listed here **without values**; none of them is a secret.

| Variable | Where | Effect |
|---|---|---|
| `COMMIT_TRAILER` | `tools/publish-site.sh` | Extra lines (attribution) for the website commit message |
| `NORTHRAY_TIMING` | Writing Studio | File to append part-by-part timings to |
| `NORTHRAY_FRAME_LOG` | cjui (all apps) | File to append each frame's milliseconds to |
| `NORTHRAY_GITHUB_API` | Writing Studio sync | A stand-in GitHub API for tests. Only `http://127.0.0.1:` addresses are accepted |
| `NORTHRAY_CHAT_TEST_URL` | Capture | A stand-in chat server for tests |
| `NORTHRAY_REOPENED`, `NORTHRAY_TRACE_FAULTS` | cjui `packages.cpp` | Internal restart marker; fault tracing |
| `STUDIO_DEBUG_IMPORT` | studiocore | Prints converted HTML during imports |

## 5. Credentials and secrets (where they live; never in the repository)

| Secret | Where it's kept |
|---|---|
| Capture's stream keys | Windows Credential Manager, `NorthRayCapture/<destination id>` |
| Writing Studio's GitHub token for sync | Windows Credential Manager, on the user's PC |
| Twitch and YouTube OAuth client IDs (parked) | `apps/capture/oauth-clients.json` holds **empty** values by design. A local override can go in `%LOCALAPPDATA%\NorthRay\Capture\oauth-clients.json` |
| GitHub push access for releases | The developer's own git credential helper. Never committed |

## 6. Release checklist

The full routine is in `CLAUDE_PROJECT_INSTRUCTIONS.md` ("Release routine"). In short:
1. Write the notes.
2. Bump `VERSION`.
3. Validate the metadata.
4. Update the architecture section.
5. Commit and push to `source`.
6. Build the apps, installers and website.
7. Run `publish-site.sh --push`.
8. Commit "Release list for suite X" and push.
9. Update the manuals.
