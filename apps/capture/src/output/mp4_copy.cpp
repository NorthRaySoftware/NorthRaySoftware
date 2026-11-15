#include "output/mp4_copy.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ncap::output {

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

class FileInput : public mp4::Input {
public:
    explicit FileInput(const std::wstring& p) {
        h_ = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER s{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &s)) size_ = (std::uint64_t)s.QuadPart;
    }
    ~FileInput() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, std::uint8_t* out, std::size_t n) override {
        // A small read-ahead window: the scan reads headers a few bytes at a time.
        if (offset >= bufAt_ && offset + n <= bufAt_ + buf_.size()) {
            std::memcpy(out, buf_.data() + (offset - bufAt_), n);
            return true;
        }
        if (n >= (1u << 16)) return direct(offset, out, n);
        buf_.resize((size_t)std::min<std::uint64_t>(1u << 20, size_ > offset ? size_ - offset : 0));
        if (buf_.size() < n || !direct(offset, buf_.data(), buf_.size())) {
            buf_.clear();
            return false;
        }
        bufAt_ = offset;
        std::memcpy(out, buf_.data(), n);
        return true;
    }

private:
    bool direct(std::uint64_t offset, std::uint8_t* out, std::size_t n) {
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(h_, at, nullptr, FILE_BEGIN)) return false;
        std::size_t done = 0;
        while (done < n) {
            DWORD got = 0;
            if (!ReadFile(h_, out + done, (DWORD)std::min<std::size_t>(n - done, 1u << 24), &got, nullptr) || got == 0) return false;
            done += got;
        }
        return true;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0, bufAt_ = 0;
    std::vector<std::uint8_t> buf_;
};

class FileSink : public mkv::Sink {
public:
    explicit FileSink(const std::wstring& p) {
        h_ = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~FileSink() override { close(); }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            flush();
            CloseHandle(h_);
        }
        h_ = INVALID_HANDLE_VALUE;
    }
    bool write(const std::uint8_t* d, std::size_t n) override {
        buf_.insert(buf_.end(), d, d + n);
        pos_ += n;
        return buf_.size() < (4u << 20) || flush();
    }
    bool writeAt(std::uint64_t, const std::uint8_t*, std::size_t) override { return false; }   // not needed: written in one go
    std::uint64_t position() const override { return pos_; }
    bool flush() {
        std::size_t done = 0;
        while (done < buf_.size()) {
            DWORD wrote = 0;
            if (!WriteFile(h_, buf_.data() + done, (DWORD)std::min<std::size_t>(buf_.size() - done, 1u << 24), &wrote, nullptr) || !wrote) {
                failed_ = true;
                return false;
            }
            done += wrote;
        }
        buf_.clear();
        return !failed_;
    }
    bool failed() const { return failed_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::vector<std::uint8_t> buf_;
    std::uint64_t pos_ = 0;
    bool failed_ = false;
};

std::wstring mp4Path(const std::wstring& mkv) {
    const std::size_t dot = mkv.find_last_of(L'.');
    if (dot != std::wstring::npos && _wcsicmp(mkv.c_str() + dot, L".mkv") == 0) return mkv.substr(0, dot) + L".mp4";
    return mkv + L".mp4";
}

}  // namespace

Mp4Copy::~Mp4Copy() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
}

bool Mp4Copy::start(const std::wstring& mkv, std::vector<mp4::Chapter> chapters) {
    if (busy_) return false;
    if (thread_.joinable()) thread_.join();
    busy_ = true;
    progress_ = 0;
    thread_ = std::thread([this, mkv, chapters = std::move(chapters)]() mutable { run(mkv, std::move(chapters)); });
    return true;
}

void Mp4Copy::run(std::wstring mkv, std::vector<mp4::Chapter> chapters) {
    const std::wstring target = mp4Path(mkv), part = target + L".part";
    bool ok = false;
    std::wstring problem;
    {
        FileInput in(mkv);
        FileSink out(part);
        if (!in.ok()) problem = L"the recording couldn't be opened";
        else if (!out.ok()) problem = L"the MP4 file couldn't be created";
        else {
            const mp4::Result r = mp4::remux(in, out, chapters, [this](double p) {
                progress_ = p;
                return !quit_.load();
            });
            out.close();
            if (!r.ok) problem = std::wstring(r.error.begin(), r.error.end());
            else if (out.failed()) problem = L"the MP4 file couldn't be written (is the disk full?)";
            else ok = true;
        }
    }
    if (ok && !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
        problem = L"the MP4 file couldn't be renamed";
    }
    if (!ok) DeleteFileW(part.c_str());   // never leave a half-made copy
    log::info(ok ? "MP4 copy made: " + utf8(target) : "MP4 copy failed: " + utf8(problem));
    {
        std::lock_guard<std::mutex> l(m_);
        finished_ = ok ? target : mkv;
        ok_ = ok;
        problem_ = problem;
    }
    busy_ = false;
}

std::wstring Mp4Copy::takeFinished(bool* ok, std::wstring* problem) {
    std::lock_guard<std::mutex> l(m_);
    std::wstring f;
    f.swap(finished_);
    if (ok) *ok = ok_;
    if (problem) *problem = problem_;
    return f;
}

}  // namespace ncap::output
