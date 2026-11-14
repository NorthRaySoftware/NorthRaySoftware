// NorthRay Capture: what the computer has and how hard it's working (system 15, Windows side).
//  - Usage: this process's CPU (all cores = 100%) and memory, and the whole system's CPU, sampled
//    about once a second on the UI thread (cheap: two GetProcessTimes/GetSystemTimes calls).
//  - Encoders: the video and audio encoders Media Foundation offers on this PC, hardware ones
//    (NVIDIA NVENC, AMD AMF, Intel Quick Sync) told apart from software. Found on a worker thread at
//    start-up; only encoders actually found are ever offered.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::sys {

struct Usage {
    double processCpu = -1;      // percent of the whole CPU; -1 before the second sample
    double systemCpu = -1;
    std::uint64_t privateBytes = 0, workingSet = 0;
};

class UsageSampler {
public:
    Usage sample();   // call about once a second

private:
    ULONGLONG lastWall_ = 0, lastProc_ = 0, lastIdle_ = 0, lastKernel_ = 0, lastUser_ = 0;
    bool have_ = false;
    Usage last_;
};

struct Encoder {
    std::wstring name;     // "NVIDIA H.264 Encoder MFT"
    std::string codec;     // "H.264", "HEVC", "AV1", "AAC"
    bool hardware = false;
    std::wstring vendor;   // "NVIDIA", "AMD", "Intel", "Microsoft" (from the name or vendor id)
};

// Asks Media Foundation (MFStartup must have been called on this thread's COM apartment).
std::vector<Encoder> findEncoders();
// Posts `message` to `window` with a heap std::vector<Encoder>* in lParam when done.
void findEncodersAsync(HWND window, UINT message);

std::wstring windowsVersion();   // "Windows 11 (build 26100)"
std::wstring cpuName();
std::wstring gpuNames();         // the display adapters, from DXGI

}  // namespace ncap::sys
