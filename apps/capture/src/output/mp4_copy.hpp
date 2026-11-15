// NorthRay Capture: the MP4 copy made after a recording ends (the MKV is kept). Runs on its own
// thread with core/mp4; writes "<name>.mp4.part" and renames it when it's complete, so a half-made
// copy is never mistaken for a finished one. Markers become chapters.
#pragma once

#include "ncap/mp4.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::output {

class Mp4Copy {
public:
    ~Mp4Copy();
    // Starts (after any copy already going finishes). False if one is already queued.
    bool start(const std::wstring& mkvPath, std::vector<mp4::Chapter> chapters);
    bool busy() const { return busy_.load(); }
    double progress() const { return progress_.load(); }   // 0..1
    // Once, when a copy is done: its path ("" if none finished), whether it worked, and why not.
    std::wstring takeFinished(bool* ok, std::wstring* problem);

private:
    void run(std::wstring mkv, std::vector<mp4::Chapter> chapters);
    std::thread thread_;
    std::atomic<bool> busy_{false}, quit_{false};
    std::atomic<double> progress_{0};
    std::mutex m_;
    std::wstring finished_, problem_;
    bool ok_ = false;
};

}  // namespace ncap::output
