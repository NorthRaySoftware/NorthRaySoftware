#include "log.hpp"

#include "ncap/diag.hpp"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>

namespace ncap::log {

namespace {

std::mutex g_mutex;
std::wstring g_folder;
std::vector<std::string> g_secrets;
constexpr long long kMostBytesPerDay = 8ll * 1024 * 1024;

std::wstring dayName(const SYSTEMTIME& t) {
    wchar_t name[64];
    swprintf(name, 64, L"capture-%04u-%02u-%02u.log", t.wYear, t.wMonth, t.wDay);
    return name;
}

void write(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_folder.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring path = g_folder + L"\\" + dayName(t);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
        (((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > kMostBytesPerDay)
        return;   // a runaway day stops at 8 MB rather than filling the disk
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::string line = redact(message, g_secrets);
    for (auto& c : line)
        if (c == '\r' || c == '\n') c = ' ';
    fprintf(f, "%s %-5s %s\r\n", stamp, level, line.c_str());
    fclose(f);
}

}  // namespace

void start(const std::wstring& folder) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_folder = folder;
        CreateDirectoryW(folder.c_str(), nullptr);
        // Days past the last fortnight go.
        SYSTEMTIME now;
        GetLocalTime(&now);
        FILETIME ft;
        SystemTimeToFileTime(&now, &ft);
        ULARGE_INTEGER cut{};
        cut.LowPart = ft.dwLowDateTime;
        cut.HighPart = ft.dwHighDateTime;
        cut.QuadPart -= 14ull * 24 * 3600 * 10000000ull;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\capture-*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                ULARGE_INTEGER w{};
                w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                if (w.QuadPart < cut.QuadPart) DeleteFileW((folder + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
}

void info(const std::string& m) { write("INFO", m); }
void warn(const std::string& m) { write("WARN", m); }
void error(const std::string& m) { write("ERROR", m); }

void addSecret(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (s.size() >= 4) g_secrets.push_back(s);
}

std::wstring folder() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_folder;
}

std::wstring todaysFile() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return folder() + L"\\" + dayName(t);
}

}  // namespace ncap::log
