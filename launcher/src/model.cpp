#include "model.hpp"

#include "cjui/gfx.hpp"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include "cj/json_safe.hpp"
#include <sstream>

namespace nrl {

using nlohmann::json;
using cjui::narrow;
using cjui::widen;

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

cj::UtcSeconds nowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<cj::UtcSeconds>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

static bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void writeFileAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << text;
        f.flush();
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static std::vector<std::wstring> listJson(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// Downloads must come over HTTPS. Plain http is accepted only from this PC,
// for testing a locally served catalogue.
static bool allowedUrl(const std::string& u) {
    return u.rfind("https://", 0) == 0 || u.rfind("http://localhost", 0) == 0 || u.rfind("http://127.0.0.1", 0) == 0;
}

bool AppItem::updateAvailable() const {
    return inst.found && release && !inst.version.empty() && cjui::compareVersions(release->version, inst.version) > 0;
}

void Model::load() {
    apps.clear();
    warnings.clear();
    dataDir = exeDir() + L"\\data";
    stateDir = cjui::northRayDataDir() + L"\\Launcher";
    CreateDirectoryW(cjui::northRayDataDir().c_str(), nullptr);
    CreateDirectoryW(stateDir.c_str(), nullptr);
    loadSettings();

    std::string text;
    for (const std::wstring& path : listJson(dataDir + L"\\apps")) {
        if (!readFile(path, text)) continue;
        std::string err;
        auto entry = cj::parseAppEntry(text, err);
        if (!entry) { warnings.push_back(narrow(path) + ": " + err); continue; }
        if (hideCjPhilipApps && isCjPhilipApp(*entry)) continue;
        AppItem a;
        a.entry = *entry;
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("artwork") && j["artwork"].is_object()) {
            const std::string icon = cj::valueOr(j["artwork"], "icon", "");
            if (!icon.empty() && icon.find("..") == std::string::npos) {
                std::wstring p = dataDir + L"\\" + widen(icon);
                std::replace(p.begin(), p.end(), L'/', L'\\');
                a.iconPath = p;
            }
        }
        loadNotes(a.entry.id, a.notes);
        // Screenshots: data/screenshots/<id>.json lists files beside it and their captions.
        if (readFile(dataDir + L"\\screenshots\\" + widen(a.entry.id) + L".json", text)) {
            json sj = json::parse(text, nullptr, false);
            if (sj.is_object() && sj.contains("screenshots") && sj["screenshots"].is_array())
                for (auto& x : sj["screenshots"]) {
                    const std::string f = cj::valueOr(x, "file", "");
                    if (f.empty() || f.find("..") != std::string::npos) continue;
                    std::wstring p = dataDir + L"\\screenshots\\" + widen(f);
                    std::replace(p.begin(), p.end(), L'/', L'\\');
                    a.screenshots.push_back({p, cj::valueOr(x, "caption", "")});
                }
        }
        apps.push_back(std::move(a));
    }
    loadNotes("launcher", launcherNotes);

    catalogueUrl.clear();
    downloadPage.clear();
    if (readFile(dataDir + L"\\community\\links.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            catalogueUrl = cj::valueOr(j, "catalogueUrl", "");
            downloadPage = cj::valueOr(j, "downloadPage", "");
            if (!allowedUrl(catalogueUrl)) catalogueUrl.clear();
            if (downloadPage.rfind("https://", 0) != 0) downloadPage.clear();
        }
    }
    if (readFile(dataDir + L"\\brand\\identity.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) contactEmail = cj::valueOr(j, "contactEmail", "");
    }
    // The last catalogue seen, so update information is there offline too.
    std::string cached;
    if (readFile(stateDir + L"\\catalogue-cache.json", cached)) {
        json c = json::parse(cached, nullptr, false);
        if (c.is_object()) {
            applyCatalogue(cj::valueOr(c, "catalogue", ""), cj::valueOr(c, "baseUrl", ""));
            catalogueChecked = cj::valueOr(c, "checked", (cj::UtcSeconds)0);
        }
    }
    refreshInstalls();
}

void Model::refreshInstalls() {
    for (auto& a : apps) a.inst = cjui::findInstalledApp(a.entry.id, a.entry.executable);
}

void Model::loadNotes(const std::string& id, std::vector<ReleaseNote>& out) {
    out.clear();
    std::string text;
    if (!readFile(dataDir + L"\\releases\\" + widen(id) + L".json", text)) return;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("releases") || !j["releases"].is_array()) return;
    for (auto& r : j["releases"]) {
        if (!r.is_object()) continue;
        ReleaseNote n;
        n.version = cj::valueOr(r, "version", "");
        n.date = cj::valueOr(r, "date", "");
        if (r.contains("notes") && r["notes"].is_array())
            for (auto& x : r["notes"]) if (x.is_string()) n.notes.push_back(x.get<std::string>());
        if (!n.version.empty()) out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return cjui::compareVersions(a.version, b.version) > 0; });
}

std::wstring Model::applyCatalogue(const std::string& text, const std::string& baseUrl) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("apps") || !j["apps"].is_array()) return L"The catalogue isn't in the expected format.";
    for (auto& a : apps) a.release.reset();
    launcherRelease.reset();
    const std::string base = baseUrl.substr(0, baseUrl.find_last_of('/') + 1);
    for (auto& r : j["apps"]) {
        if (!r.is_object()) continue;
        Release rel;
        rel.id = cj::valueOr(r, "id", "");
        rel.version = cj::valueOr(r, "version", "");
        rel.file = cj::valueOr(r, "file", "");
        rel.size = cj::valueOr(r, "size", 0ULL);
        rel.sha256 = cj::valueOr(r, "sha256", "");
        std::string url = cj::valueOr(r, "url", "");
        if (url.find("://") == std::string::npos) url = base + url;  // relative to the catalogue
        rel.url = url;
        // Every field must be sensible before anything is offered for download.
        if (rel.id.empty() || rel.version.empty() || rel.sha256.size() != 64 || rel.size == 0 || !allowedUrl(rel.url) ||
            rel.file.find_first_of("\\/:") != std::string::npos || rel.file.size() < 5 ||
            rel.file.compare(rel.file.size() - 4, 4, ".exe") != 0)
            continue;
        if (rel.id == "launcher") { launcherRelease = rel; continue; }
        for (auto& a : apps) if (a.entry.id == rel.id) a.release = rel;
    }
    return {};
}

std::wstring Model::effectiveInstallRoot() const {
    return settings.installRoot.empty() ? cjui::defaultAppsDir() : settings.installRoot;
}

std::wstring Model::installDirFor(const std::string& appId) const {
    for (auto& a : apps)
        if (a.entry.id == appId && a.inst.found && !a.inst.legacy) return a.inst.dir;  // updates go where it already is
    return effectiveInstallRoot() + L"\\" + widen(appId);
}

AppItem* Model::app(const std::string& id) {
    for (auto& a : apps) if (a.entry.id == id) return &a;
    return nullptr;
}

std::wstring Model::launch(const std::string& appId) {
    AppItem* a = app(appId);
    if (!a || !a->inst.found) return L"That application isn't installed.";
    std::wstring cmd = L"\"" + a->inst.exe + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(a->inst.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, a->inst.dir.c_str(), &si, &pi))
        return L"Windows couldn't start it (error " + std::to_wstring(GetLastError()) + L").";
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

std::wstring Model::uninstall(const std::string& appId) {
    AppItem* a = app(appId);
    if (!a || !a->inst.found) return L"That application isn't installed.";
    if (cjui::isAppRunning(appId)) return L"Close " + widen(a->entry.name) + L" first, then try again.";
    if (!a->inst.uninstaller.empty()) {
        // "_?=" runs the uninstaller in place, so this can wait for it to finish.
        std::wstring cmd = L"\"" + a->inst.uninstaller + L"\" /S _?=" + a->inst.dir;
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            return L"Its uninstaller couldn't be started.";
        WaitForSingleObject(pi.hProcess, 120000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::wstring err = cjui::removeInstalled(a->inst.dir);  // leftovers, including the uninstaller itself
    refreshInstalls();
    return err;
}

void Model::loadSettings() {
    std::string text;
    settings = {};
    firstRun = false;
    if (!readFile(stateDir + L"\\settings.json", text)) { firstRun = true; return; }
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    const std::string t = cj::valueOr(j, "theme", "system");
    settings.theme = t == "light" ? ThemeChoice::Light : t == "dark" ? ThemeChoice::Dark : ThemeChoice::System;
    settings.installRoot = widen(cj::valueOr(j, "installRoot", ""));
    settings.autoUpdate = cj::valueOr(j, "autoUpdate", false);
}

void Model::saveSettings() {
    json j;
    j["theme"] = settings.theme == ThemeChoice::Light ? "light" : settings.theme == ThemeChoice::Dark ? "dark" : "system";
    j["installRoot"] = narrow(settings.installRoot);
    j["autoUpdate"] = settings.autoUpdate;
    writeFileAtomic(stateDir + L"\\settings.json", j.dump(2));
}

}  // namespace nrl
