# NorthRay Files

A file manager for Windows in the NorthRay design language. Version 0.1 browses folders, opens files and shows pictures of image files; it doesn't
change anything yet. See `docs/ARCHITECTURE.md`.

## Build

From the repository root, like the other NorthRay apps:

- Windows app (cross-compiled with MinGW-w64): `bash tools/build-windows.sh` -> `build/windows/apps/files/NorthRayFiles.exe`
- Core tests (any platform): `cmake -S . -B build/native && cmake --build build/native --target filescore_tests && build/native/apps/files/filescore_tests`
- Installer: `bash tools/make-installers.sh` -> `website/public/downloads/NorthRayFiles-Setup-<version>.exe`

The version is in `apps/files/VERSION`. Running `NorthRayFiles.exe "C:\some\folder"` opens that folder.
