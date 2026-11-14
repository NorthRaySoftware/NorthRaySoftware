// NorthRay Files: Windows paths as text. Portable (no Windows calls), so the
// rules are unit-tested on every platform.
//
// A folder's path is kept in its ordinary form: "C:\Users\CJ", "C:\" for a
// drive's root, "\\server\share\folder" for a network folder. API calls get the
// extended-length form ("\\?\C:\...", "\\?\UNC\server\share\..."), which lifts
// the 260-character limit and turns off Win32's name rewriting.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace nrf {

// Turns what was typed or pasted into a folder path, or explains why it can't.
// Accepts "C:\x", "C:/x", "C:" (the drive's root), "\\server\share\x",
// extended-length "\\?\C:\x" and "\\?\UNC\server\share\x", "file:///C:/x",
// quotes around it, and paths relative to `current` ("..", "sub\folder").
// Dots and doubled separators are tidied; "." and ".." are resolved, but never
// above the root.
std::optional<std::wstring> normalizePath(const std::wstring& typed, const std::wstring& current, std::wstring* problem = nullptr);

bool isDriveRoot(const std::wstring& path);   // "C:\"
bool isUncRoot(const std::wstring& path);     // "\\server\share"
bool isRoot(const std::wstring& path);        // either
bool isUnc(const std::wstring& path);

// The folder above, or "" at a root.
std::wstring parentOf(const std::wstring& path);
// The last part ("CJ" for C:\Users\CJ; "C:" for C:\; "share" for \\server\share).
std::wstring nameOf(const std::wstring& path);
std::wstring join(const std::wstring& folder, const std::wstring& name);
// For Windows API calls: "\\?\C:\Users", "\\?\UNC\server\share\x".
std::wstring extendedPath(const std::wstring& path);

// The address bar's parts, root first.
struct Crumb {
    std::wstring label;
    std::wstring path;
};
std::vector<Crumb> breadcrumbs(const std::wstring& path);

// Whether a name could be a file or folder name at all (no \ / : * ? " < > |,
// no control characters, not empty, not "." or "..", no trailing space or dot).
bool validName(const std::wstring& name);

// The extension as Windows sees it, lower-case with the dot (".txt"); "" for none.
std::wstring extensionOf(const std::wstring& name);

}  // namespace nrf
