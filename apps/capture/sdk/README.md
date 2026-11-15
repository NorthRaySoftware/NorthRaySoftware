# NorthRay Capture audio plugins (version 1)

A plugin is a 64-bit Windows DLL that exports `nr_audio_plugin()` (see `northray_audio_plugin.h` for the
rules: what may and may not happen on the audio thread, parameters, latency). Capture loads it only after
the user switches it on in **Settings › Plugins**, having been shown the file's SHA-256 and its
Authenticode signature, and switches it off again if the file changes, if it goes missing, or if Capture
closed unexpectedly while it was running.

Put the DLL in `%LOCALAPPDATA%\NorthRay\Capture\plugins`, press **Look again**, switch it on, then add it
from a sound source's **Filters…** page (Add a filter › Plugins).

The example, `examples/tremolo.c`:

    x86_64-w64-mingw32-gcc -O2 -shared -I.. tremolo.c -o tremolo.dll -static-libgcc
    cl /O2 /LD /I.. tremolo.c /Fe:tremolo.dll

Signing your DLL (Authenticode) lets users see who made it; unsigned plugins still work, with a warning.
