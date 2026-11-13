// Installing CJ Community applications from verified packages.
//
// A package is a folder with a package.json listing every file with its size
// and SHA-256. Installing verifies every file before anything is copied, then
// swaps the new version into place, so a failed install never leaves a
// half-updated app. Nothing is ever downloaded or run during installation.
#pragma once

#include <string>
#include <vector>

namespace cjui {

struct PackageFile {
    std::string path;     // relative, forward slashes
    unsigned long long size = 0;
    std::string sha256;   // lowercase hex
};

struct PackageManifest {
    std::string id;
    std::string version;
    std::string executable;
    std::vector<PackageFile> files;
};

bool readManifest(const std::wstring& manifestPath, PackageManifest& out, std::wstring& error);
std::string sha256File(const std::wstring& path);  // empty on failure

// -1, 0, 1 comparing dotted versions numerically ("0.10.0" > "0.9.2").
int compareVersions(const std::string& a, const std::string& b);

// Verifies the package in `packageDir`, then installs it to `installDir`.
// Returns an empty string on success or a message for the user.
std::wstring installPackage(const std::wstring& packageDir, const std::wstring& installDir);

// Removes an installed app folder. User data lives elsewhere and is kept.
std::wstring removeInstalled(const std::wstring& installDir);

// Where NorthRay apps live. Data: %LOCALAPPDATA%\NorthRay. Default install
// root for apps: %LOCALAPPDATA%\NorthRay\Apps (the launcher lets the user pick another).
std::wstring northRayDataDir();
std::wstring defaultAppsDir();

// Finds an installed app, wherever it was installed:
//  1. its NorthRay setup entry (HKCU\...\Uninstall\NorthRay.<id>): folder and version;
//  2. an older CJ Community setup entry (CJCommunity.<id>), marked legacy;
//  3. the default folders (NorthRay\Apps\<id>, then the old CJ Community\Apps\<id>).
struct InstalledInfo {
    bool found = false;
    bool legacy = false;            // installed by a CJ Community-era installer
    std::wstring dir;               // install folder
    std::wstring exe;               // full path to its executable
    std::string version;            // from the setup entry, else package.json
    std::wstring uninstaller;       // Uninstall.exe if present
};
InstalledInfo findInstalledApp(const std::string& id, const std::string& exeName);

// True if the app holds its "running" mutex (Local\CJCommunity.App.<id>).
bool isAppRunning(const std::string& appId);
// Called by each app at start-up; keep the handle for the process lifetime.
void* markAppRunning(const std::string& appId);

// If the app hits an error it can't recover from, say so (and why) in a message
// and save a short report to %LOCALAPPDATA%\NorthRay\<folder>\crash.log,
// instead of closing without a word.
void installCrashReport(const std::wstring& appName, const std::wstring& stateFolder);

}  // namespace cjui
