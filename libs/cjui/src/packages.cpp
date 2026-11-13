#include "cjui/packages.hpp"

#include <shlobj.h>

#include <windows.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <ctime>
#include <exception>

namespace fs = std::filesystem;

namespace cjui {

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static bool safeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    std::string seg;
    std::istringstream ss(p);
    while (std::getline(ss, seg, '/'))
        if (seg.empty() || seg == "." || seg == ".." || seg.find('\\') != std::string::npos) return false;
    return true;
}

bool readManifest(const std::wstring& manifestPath, PackageManifest& out, std::wstring& error) {
    std::ifstream f(fs::path(manifestPath), std::ios::binary);
    if (!f) { error = L"The package has no manifest."; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) { error = L"The package manifest is damaged."; return false; }
    out.id = j.value("id", "");
    out.version = j.value("version", "");
    out.executable = j.value("executable", "");
    out.files.clear();
    for (const auto& e : j.value("files", nlohmann::json::array())) {
        PackageFile pf;
        pf.path = e.value("path", "");
        pf.size = e.value("size", 0ULL);
        pf.sha256 = e.value("sha256", "");
        if (!safeRelative(pf.path) || pf.sha256.size() != 64) {
            error = L"The package manifest lists an unsafe or invalid file.";
            return false;
        }
        out.files.push_back(pf);
    }
    if (out.id.empty() || out.version.empty() || out.files.empty() || !safeRelative(out.executable)) {
        error = L"The package manifest is incomplete.";
        return false;
    }
    return true;
}

std::string sha256File(const std::wstring& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream f(fs::path(path), std::ios::binary);
        if (f) {
            std::vector<char> buf(64 * 1024);
            bool ok = true;
            while (f) {
                f.read(buf.data(), buf.size());
                const std::streamsize n = f.gcount();
                if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), (ULONG)n, 0) != 0) { ok = false; break; }
            }
            unsigned char digest[32];
            if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
                static const char* hex = "0123456789abcdef";
                for (unsigned char b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

int compareVersions(const std::string& a, const std::string& b) {
    std::istringstream sa(a), sb(b);
    for (;;) {
        std::string pa, pb;
        const bool ha = (bool)std::getline(sa, pa, '.'), hb = (bool)std::getline(sb, pb, '.');
        if (!ha && !hb) return 0;
        const long va = ha ? std::strtol(pa.c_str(), nullptr, 10) : 0;
        const long vb = hb ? std::strtol(pb.c_str(), nullptr, 10) : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
}

std::wstring installPackage(const std::wstring& packageDir, const std::wstring& installDir) {
    PackageManifest m;
    std::wstring err;
    if (!readManifest(packageDir + L"\\package.json", m, err)) return err;

    // 1. Verify every file before touching the installed copy.
    for (const auto& f : m.files) {
        const fs::path src = fs::path(packageDir) / fs::path(widen(f.path));
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) return L"The package is missing " + widen(f.path) + L".";
        if (fs::file_size(src, ec) != f.size || sha256File(src.wstring()) != f.sha256)
            return L"The package failed its integrity check (" + widen(f.path) + L"), so nothing was installed.";
    }

    // 2. Copy into a staging folder next to the destination.
    const fs::path dest(installDir), staging(installDir + L".staging"), old(installDir + L".old");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return L"Couldn't create the installation folder.";
    for (const auto& f : m.files) {
        const fs::path to = staging / fs::path(widen(f.path));
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(fs::path(packageDir) / fs::path(widen(f.path)), to, fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove_all(staging, ec); return L"Copying " + widen(f.path) + L" failed."; }
    }
    // The manifest goes in too, so the launcher knows the installed version.
    fs::copy_file(fs::path(packageDir) / L"package.json", staging / L"package.json", fs::copy_options::overwrite_existing, ec);

    // 3. Swap: current -> .old, staging -> current, then delete .old.
    fs::remove_all(old, ec);
    if (fs::exists(dest)) {
        fs::rename(dest, old, ec);
        if (ec) { fs::remove_all(staging, ec); return L"The current version is in use. Close the app and try again."; }
    }
    fs::rename(staging, dest, ec);
    if (ec) {
        std::error_code ec2;
        if (fs::exists(old)) fs::rename(old, dest, ec2);  // put the old version back
        return L"Couldn't move the new version into place.";
    }
    fs::remove_all(old, ec);
    return {};
}

std::wstring removeInstalled(const std::wstring& installDir) {
    std::error_code ec;
    if (!fs::exists(installDir)) return {};
    fs::remove_all(installDir, ec);
    if (ec) return L"Some files couldn't be removed. Close the app and try again.";
    return {};
}

std::wstring northRayDataDir() {
    PWSTR p = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) base = p;
    CoTaskMemFree(p);
    return base + L"\\NorthRay";
}

std::wstring defaultAppsDir() { return northRayDataDir() + L"\\Apps"; }

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring regString(HKEY root, const std::wstring& key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

static bool isFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

InstalledInfo findInstalledApp(const std::string& id, const std::string& exeName) {
    InstalledInfo info;
    const std::wstring wid = widen(id), wexe = widen(exeName);
    auto tryDir = [&](std::wstring dir, std::string version, bool legacy) {
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'"')) dir.pop_back();
        if (!dir.empty() && dir.front() == L'"') dir.erase(0, 1);
        if (dir.empty() || exeName.empty() || !isFile(dir + L"\\" + wexe)) return false;
        info.found = true;
        info.legacy = legacy;
        info.dir = dir;
        info.exe = dir + L"\\" + wexe;
        if (version.empty()) {
            PackageManifest m;
            std::wstring err;
            if (readManifest(dir + L"\\package.json", m, err)) version = m.version;
        }
        info.version = version;
        if (isFile(dir + L"\\Uninstall.exe")) info.uninstaller = dir + L"\\Uninstall.exe";
        return true;
    };
    const std::wstring uninst = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
    for (auto [prefix, legacy] : {std::pair<const wchar_t*, bool>{L"NorthRay.", false}, {L"CJCommunity.", true}}) {
        const std::wstring key = uninst + prefix + wid;
        if (tryDir(regString(HKEY_CURRENT_USER, key, L"InstallLocation"), toUtf8(regString(HKEY_CURRENT_USER, key, L"DisplayVersion")), legacy))
            return info;
    }
    if (tryDir(defaultAppsDir() + L"\\" + wid, {}, false)) return info;
    const std::wstring legacyDir = northRayDataDir();
    if (tryDir(legacyDir.substr(0, legacyDir.size() - 8) + L"CJ Community\\Apps\\" + wid, {}, true)) return info;
    return info;
}

static std::wstring mutexName(const std::string& appId) { return L"Local\\CJCommunity.App." + widen(appId); }

bool isAppRunning(const std::string& appId) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName(appId).c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

void* markAppRunning(const std::string& appId) { return CreateMutexW(nullptr, FALSE, mutexName(appId).c_str()); }

// ------------------------------------------------------------- crash report

static std::wstring g_crashName, g_crashFolder;

static void reportCrash(const std::string& what) {
    static bool once = false;
    if (once) return;
    once = true;
    const std::wstring dir = northRayDataDir() + L"\\" + g_crashFolder;
    CreateDirectoryW(northRayDataDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring log = dir + L"\\crash.log";
    {
        std::ofstream f(log.c_str(), std::ios::app | std::ios::binary);
        char when[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        f << when << "  " << what << "\r\n";
    }
    const std::wstring msg = g_crashName + L" ran into a problem and has to close.\n\n" + widen(what) +
                             L"\n\nA report was saved to " + log + L".";
    MessageBoxW(nullptr, msg.c_str(), g_crashName.c_str(), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS* ep) {
    char b[96];
    std::snprintf(b, sizeof b, "Windows error 0x%08lX at %p", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0UL,
                  ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    reportCrash(b);
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashReport(const std::wstring& appName, const std::wstring& stateFolder) {
    g_crashName = appName;
    g_crashFolder = stateFolder;
    SetUnhandledExceptionFilter(unhandled);
    std::set_terminate([] {
        std::string what = "An unexpected error.";
        if (auto e = std::current_exception()) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& x) { what = x.what(); }
            catch (...) {}
        }
        reportCrash(what);
        std::_Exit(3);
    });
}

}  // namespace cjui
