// Not a test by itself: runs the MP4 copy made after a recording (output/mp4_copy) on an MKV, as
// Capture does, for checking the result with ffprobe/ffmpeg (Windows build, run under Wine or on Windows).
//   capture_mp4_copy_check.exe <recording.mkv>
#include "output/mp4_copy.hpp"

#include <windows.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    ncap::output::Mp4Copy copy;
    copy.start(argv[1], {{0, "Start"}, {1500000, "A marker"}});
    while (copy.busy()) Sleep(20);
    bool ok = false;
    std::wstring problem;
    const std::wstring done = copy.takeFinished(&ok, &problem);
    std::wprintf(L"%ls %ls %ls\n", ok ? L"OK" : L"FAILED", done.c_str(), problem.c_str());
    return ok ? 0 : 1;
}
