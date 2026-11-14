#include "nrf/paths.hpp"

#include <cwctype>

namespace nrf {

namespace {

bool isSep(wchar_t c) { return c == L'\\' || c == L'/'; }
bool isLetter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

int hexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// "file:///C:/My%20Docs" -> "C:/My Docs" (UTF-8 percent escapes decoded).
std::optional<std::wstring> fromFileUrl(const std::wstring& url) {
    std::wstring rest = url.substr(7);   // after "file://"
    std::wstring host;
    if (!rest.empty() && rest[0] != L'/') {   // file://server/share
        const size_t slash = rest.find(L'/');
        host = rest.substr(0, slash);
        rest = slash == std::wstring::npos ? L"" : rest.substr(slash);
    }
    if (!rest.empty() && rest[0] == L'/') rest.erase(0, 1);
    std::string bytes;
    std::wstring out;
    auto flush = [&] {
        // Decode the gathered UTF-8 bytes.
        size_t i = 0;
        while (i < bytes.size()) {
            const unsigned char c = (unsigned char)bytes[i];
            unsigned cp = 0;
            int n = 0;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if (i + (size_t)n > bytes.size()) break;
            for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)bytes[i + (size_t)k] & 0x3F);
            if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
                cp -= 0x10000;
                out += (wchar_t)(0xD800 + (cp >> 10));
                out += (wchar_t)(0xDC00 + (cp & 0x3FF));
            } else {
                out += (wchar_t)cp;
            }
            i += (size_t)n;
        }
        bytes.clear();
    };
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == L'%' && i + 2 < rest.size() && hexValue(rest[i + 1]) >= 0 && hexValue(rest[i + 2]) >= 0) {
            bytes += (char)(hexValue(rest[i + 1]) * 16 + hexValue(rest[i + 2]));
            i += 2;
        } else {
            flush();
            out += rest[i];
        }
    }
    flush();
    if (!host.empty() && host != L"localhost") return L"\\\\" + host + L"\\" + out;
    return out;
}

}  // namespace

bool isDriveRoot(const std::wstring& p) { return p.size() == 3 && isLetter(p[0]) && p[1] == L':' && p[2] == L'\\'; }

bool isUnc(const std::wstring& p) { return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\'; }

bool isUncRoot(const std::wstring& p) {
    if (!isUnc(p)) return false;
    const size_t a = p.find(L'\\', 2);
    return a != std::wstring::npos && a > 2 && a + 1 < p.size() && p.find(L'\\', a + 1) == std::wstring::npos;
}

bool isRoot(const std::wstring& p) { return isDriveRoot(p) || isUncRoot(p); }

bool validName(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L".." || n.size() > 255) return false;
    for (wchar_t c : n)
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
            c == L'|')
            return false;
    return n.back() != L' ' && n.back() != L'.';
}

std::optional<std::wstring> normalizePath(const std::wstring& typed, const std::wstring& current, std::wstring* problem) {
    auto fail = [&](const wchar_t* why) -> std::optional<std::wstring> {
        if (problem) *problem = why;
        return std::nullopt;
    };
    std::wstring s = trim(typed);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = trim(s.substr(1, s.size() - 2));
    if (s.empty()) return fail(L"Type a folder's path.");
    std::wstring lower;
    for (wchar_t c : s.substr(0, 8)) lower += (wchar_t)std::towlower(c);
    if (lower.rfind(L"file://", 0) == 0) {
        auto f = fromFileUrl(s);
        if (!f) return fail(L"That file address couldn't be read.");
        s = *f;
    }
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';

    // Extended-length and device prefixes.
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) s = L"\\\\" + s.substr(8);
    else if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    else if (s.rfind(L"\\\\.\\", 0) == 0) return fail(L"Device paths (\\\\.\\) can't be browsed.");

    // Root and the parts after it.
    std::wstring root;
    std::wstring rest;
    if (s.size() >= 2 && isLetter(s[0]) && s[1] == L':') {
        root = std::wstring(1, (wchar_t)std::towupper(s[0])) + L":\\";
        if (s.size() > 2 && !isSep(s[2])) return fail(L"Write it as C:\\folder (\"C:folder\" depends on a hidden current folder).");
        rest = s.size() > 3 ? s.substr(3) : L"";
    } else if (s.size() >= 2 && isSep(s[0]) && isSep(s[1])) {
        size_t i = 2;
        while (i < s.size() && isSep(s[i])) ++i;
        const size_t serverEnd = s.find(L'\\', i);
        if (serverEnd == std::wstring::npos || serverEnd == i) return fail(L"A network path needs a server and a share: \\\\server\\share.");
        size_t shareStart = serverEnd;
        while (shareStart < s.size() && isSep(s[shareStart])) ++shareStart;
        const size_t shareEnd = s.find(L'\\', shareStart);
        const std::wstring server = s.substr(i, serverEnd - i);
        const std::wstring share = s.substr(shareStart, shareEnd == std::wstring::npos ? std::wstring::npos : shareEnd - shareStart);
        if (share.empty()) return fail(L"A network path needs a share as well as a server: \\\\server\\share.");
        if (server == L"?" || server == L".") return fail(L"That kind of path can't be browsed.");
        root = L"\\\\" + server + L"\\" + share;
        rest = shareEnd == std::wstring::npos ? L"" : s.substr(shareEnd + 1);
    } else if (isSep(s[0])) {
        // "\folder": on the current folder's drive.
        if (current.empty() || isUnc(current)) return fail(L"Include the drive, like C:\\folder.");
        root = current.substr(0, 3);
        rest = s.substr(1);
    } else {
        // Relative to the current folder.
        if (current.empty()) return fail(L"Type a full path, like C:\\Users.");
        auto base = normalizePath(current, L"", problem);
        if (!base) return std::nullopt;
        return normalizePath(*base + (isDriveRoot(*base) ? L"" : L"\\") + s, L"", problem);
    }

    // Walk the parts: drop empty and ".", resolve "..", check the rest.
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i <= rest.size()) {
        const size_t j = rest.find(L'\\', i);
        const std::wstring part = rest.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
        if (part.empty() || part == L".") {
        } else if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            // Trailing dots and spaces are what Win32 would strip; refuse rather than guess.
            if (!validName(part)) return fail(L"That path has a name Windows doesn't allow (no * ? \" < > | :, and no trailing dot or space).");
            parts.push_back(part);
        }
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    std::wstring out = root;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out += parts[k];
    }
    if (problem) problem->clear();
    return out;
}

std::wstring parentOf(const std::wstring& p) {
    if (p.empty() || isRoot(p)) return L"";
    const size_t at = p.find_last_of(L'\\');
    if (at == std::wstring::npos) return L"";
    if (at == 2 && isLetter(p[0]) && p[1] == L':') return p.substr(0, 3);   // C:\x -> C:\ .
    return p.substr(0, at);
}

std::wstring nameOf(const std::wstring& p) {
    if (isDriveRoot(p)) return p.substr(0, 2);
    const size_t at = p.find_last_of(L'\\');
    return at == std::wstring::npos ? p : p.substr(at + 1);
}

std::wstring join(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty()) return name;
    return folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
}

std::wstring extendedPath(const std::wstring& p) {
    if (isUnc(p)) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::vector<Crumb> breadcrumbs(const std::wstring& p) {
    std::vector<Crumb> out;
    if (p.empty()) return out;
    std::wstring root;
    size_t restAt = 0;
    if (isDriveRoot(p.substr(0, 3)) || (p.size() >= 3 && p[1] == L':')) {
        root = p.substr(0, 3);
        out.push_back({p.substr(0, 2), root});
        restAt = 3;
    } else if (isUnc(p)) {
        const size_t a = p.find(L'\\', 2);
        const size_t b = a == std::wstring::npos ? std::wstring::npos : p.find(L'\\', a + 1);
        root = b == std::wstring::npos ? p : p.substr(0, b);
        out.push_back({root, root});
        restAt = b == std::wstring::npos ? p.size() : b + 1;
    } else {
        out.push_back({p, p});
        return out;
    }
    std::wstring at = root;
    while (restAt < p.size()) {
        const size_t j = p.find(L'\\', restAt);
        const std::wstring part = p.substr(restAt, j == std::wstring::npos ? std::wstring::npos : j - restAt);
        at = join(at, part);
        if (!part.empty()) out.push_back({part, at});
        if (j == std::wstring::npos) break;
        restAt = j + 1;
    }
    return out;
}

std::wstring extensionOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 == name.size()) return L"";
    std::wstring e = name.substr(dot);
    for (wchar_t& c : e) c = (wchar_t)std::towlower(c);
    return e;
}

}  // namespace nrf
