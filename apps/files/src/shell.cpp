#include "shell.hpp"

#include "nrf/paths.hpp"

#include <commctrl.h>
#include <commoncontrols.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace nrf {

namespace {

// File types and icons by extension: asked of the Shell once each.
struct TypeInfo {
    std::wstring name;
    int icon = -1;
};

std::mutex g_typesMutex;
std::unordered_map<std::wstring, TypeInfo> g_types;

// Extensions whose icon lives inside the file itself (programs, icons,
// shortcuts): those are read from the file, everything else by type alone.
bool iconFromFile(const std::wstring& ext) {
    static const wchar_t* exts[] = {L".exe", L".ico", L".lnk", L".url", L".cur", L".ani", L".msc", L".cpl", L".scr", L".appref-ms"};
    for (const wchar_t* e : exts)
        if (ext == e) return true;
    return false;
}

TypeInfo typeFor(const std::wstring& ext, bool folder) {
    const std::wstring key = folder ? L"<folder>" : ext;
    {
        std::lock_guard<std::mutex> lock(g_typesMutex);
        auto it = g_types.find(key);
        if (it != g_types.end()) return it->second;
    }
    SHFILEINFOW fi{};
    TypeInfo t;
    const std::wstring probe = folder ? L"folder" : L"file" + ext;
    if (SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof fi,
                       SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX)) {
        t.name = fi.szTypeName;
        t.icon = fi.iIcon;
    }
    if (t.name.empty()) {
        // As File Explorer words it: "PDF File" when Windows has no name for it.
        if (folder) t.name = L"File folder";
        else if (ext.size() > 1) {
            std::wstring up = ext.substr(1);
            for (auto& c : up) c = (wchar_t)towupper(c);
            t.name = up + L" File";
        } else t.name = L"File";
    }
    std::lock_guard<std::mutex> lock(g_typesMutex);
    g_types[key] = t;
    return t;
}

void post(HWND w, UINT msg, void* p, const std::function<void()>& discard) {
    if (!IsWindow(w) || !PostMessageW(w, msg, 0, reinterpret_cast<LPARAM>(p))) discard();
}

}  // namespace

void listFolderAsync(HWND window, std::wstring folder, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest) {
    std::thread([window, folder = std::move(folder), request, latest] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto send = [&](ListBatch* b) { post(window, WM_APP_LIST, b, [b] { delete b; }); };
        auto stale = [&] { return latest->load() != request; };
        auto fail = [&](ListError e, DWORD code) {
            auto* b = new ListBatch;
            b->request = request;
            b->done = true;
            b->error = e;
            b->code = code;
            send(b);
        };
        const std::wstring ext = extendedPath(folder);
        // Is it there, and a folder? (A file's path opens its folder, with the file selected.)
        const DWORD a = GetFileAttributesW(ext.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME) fail(ListError::NotFound, e);
            else if (e == ERROR_ACCESS_DENIED) fail(ListError::AccessDenied, e);
            else if (e == ERROR_NOT_READY || e == ERROR_INVALID_DRIVE || e == ERROR_UNRECOGNIZED_VOLUME) fail(ListError::NotReady, e);
            else if (e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME || e == ERROR_NETWORK_UNREACHABLE || e == ERROR_NETNAME_DELETED ||
                     e == ERROR_UNEXP_NET_ERR || e == ERROR_HOST_UNREACHABLE)
                fail(ListError::NetworkUnreachable, e);
            else if (e == ERROR_FILENAME_EXCED_RANGE) fail(ListError::PathTooLong, e);
            else fail(ListError::Other, e);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
            auto* b = new ListBatch;
            b->request = request;
            b->done = true;
            b->error = ListError::NotAFolder;
            b->fileToSelect = nameOf(folder);
            send(b);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }

        // Each name as it's actually written ("c:\\users" typed shows as "C:\\Users").
        std::wstring canonical;
        {
            std::wstring at;
            bool ok = true;
            for (const Crumb& c : breadcrumbs(folder)) {
                if (at.empty()) { at = c.path; continue; }   // the root stays as typed (drive letters are upper-case already)
                WIN32_FIND_DATAW cf;
                HANDLE ch = FindFirstFileExW(extendedPath(c.path).c_str(), FindExInfoBasic, &cf, FindExSearchNameMatch, nullptr, 0);
                if (ch == INVALID_HANDLE_VALUE) { ok = false; break; }
                FindClose(ch);
                at = join(at, cf.cFileName);
            }
            if (ok && at.size() == folder.size()) canonical = at;
        }

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((ext + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES) {   // an empty drive root
                auto* b = new ListBatch;
                b->request = request;
                b->canonical = canonical;
                b->done = true;
                send(b);
            } else if (e == ERROR_ACCESS_DENIED) fail(ListError::AccessDenied, e);
            else if (e == ERROR_NOT_READY) fail(ListError::NotReady, e);
            else if (e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME || e == ERROR_NETWORK_UNREACHABLE || e == ERROR_NETNAME_DELETED)
                fail(ListError::NetworkUnreachable, e);
            else fail(ListError::Other, e);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }
        auto* batch = new ListBatch;
        batch->request = request;
        batch->canonical = canonical;
        DWORD lastSend = GetTickCount();
        bool gaveUp = false;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            Entry e;
            e.name = name;
            e.attributes = fd.dwFileAttributes;
            e.size = (e.attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.modified = ((std::uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) e.reparseTag = fd.dwReserved0;
            const std::wstring x = e.folder() ? L"" : extensionOf(name);
            const TypeInfo t = typeFor(x, e.folder());
            e.type = t.name;
            e.icon = t.icon;
            // A program's or shortcut's own icon, read from the file, but never for an
            // online-only file (reading it would download it).
            if (!e.folder() && iconFromFile(x) && !e.cloudOnly()) {
                SHFILEINFOW fi{};
                if (SHGetFileInfoW(extendedPath(join(folder, name)).c_str(), 0, &fi, sizeof fi, SHGFI_SYSICONINDEX)) e.icon = fi.iIcon;
            }
            batch->entries.push_back(std::move(e));
            // Hand over what's ready now and then, so a big folder fills in as it's read.
            if (batch->entries.size() >= 4000 || (GetTickCount() - lastSend > 150 && batch->entries.size() >= 200)) {
                if (stale()) { gaveUp = true; break; }
                send(batch);
                batch = new ListBatch;
                batch->request = request;
                lastSend = GetTickCount();
            }
        } while (FindNextFileW(h, &fd));
        const DWORD end = GetLastError();
        FindClose(h);
        if (gaveUp) {
            delete batch;
        } else {
            batch->done = true;
            if (end != ERROR_NO_MORE_FILES && end != ERROR_SUCCESS) {   // stopped part-way (a network drop, say)
                batch->error = ListError::Other;
                batch->code = end;
            }
            send(batch);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void listDrivesAsync(HWND window, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest) {
    std::thread([window, request, latest] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* b = new DriveBatch;
        b->request = request;
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            Drive d;
            d.root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
            d.type = GetDriveTypeW(d.root.c_str());
            if (d.type == DRIVE_NO_ROOT_DIR) continue;
            // No "insert a disk" dialogs while asking about drives with no media.
            const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
            wchar_t name[MAX_PATH + 1] = L"";
            d.ready = GetVolumeInformationW(d.root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) != FALSE;
            ULARGE_INTEGER avail{}, total{}, freeAll{};
            if (d.ready && GetDiskFreeSpaceExW(d.root.c_str(), &avail, &total, &freeAll)) {
                d.total = total.QuadPart;
                d.free = avail.QuadPart;
            }
            SetErrorMode(oldMode);
            d.label = name;
            if (d.label.empty()) {
                switch (d.type) {
                    case DRIVE_FIXED: d.label = L"Local Disk"; break;
                    case DRIVE_REMOVABLE: d.label = L"Removable Disk"; break;
                    case DRIVE_CDROM: d.label = L"CD/DVD Drive"; break;
                    case DRIVE_REMOTE: d.label = L"Network Drive"; break;
                    case DRIVE_RAMDISK: d.label = L"RAM Disk"; break;
                    default: d.label = L"Drive"; break;
                }
            }
            SHFILEINFOW fi{};
            if (SHGetFileInfoW(d.root.c_str(), 0, &fi, sizeof fi, SHGFI_SYSICONINDEX)) d.icon = fi.iIcon;
            b->drives.push_back(std::move(d));
            if (latest->load() != request) break;
        }
        post(window, WM_APP_DRIVES, b, [b] { delete b; });
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

std::vector<Place> knownPlaces() {
    struct K { const KNOWNFOLDERID* id; const wchar_t* label; };
    const K ks[] = {{&FOLDERID_Profile, L"Home"},      {&FOLDERID_Desktop, L"Desktop"},     {&FOLDERID_Documents, L"Documents"},
                    {&FOLDERID_Downloads, L"Downloads"}, {&FOLDERID_Pictures, L"Pictures"}, {&FOLDERID_Music, L"Music"},
                    {&FOLDERID_Videos, L"Videos"}};
    std::vector<Place> out;
    for (const K& k : ks) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*k.id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) {
            const std::wstring path = p;
            const DWORD a = GetFileAttributesW(path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                Place pl{k.label, path, -1};
                SHFILEINFOW fi{};
                if (SHGetFileInfoW(path.c_str(), 0, &fi, sizeof fi, SHGFI_SYSICONINDEX)) pl.icon = fi.iIcon;
                out.push_back(pl);
            }
        }
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring openWithDefaultApp(const std::wstring& path, HWND owner) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = nullptr;   // the file's default action, as a double-click in File Explorer
    sei.lpFile = path.c_str();
    const std::wstring dir = parentOf(path);
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return {};
    switch (GetLastError()) {
        case ERROR_NO_ASSOCIATION: return L"No app on this PC is set to open this kind of file.";
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return L"It isn't there any more. Press F5 to refresh the folder.";
        case ERROR_ACCESS_DENIED: return L"Windows didn't allow it to be opened (access denied).";
        case ERROR_CANCELLED: return {};
        default: return L"Windows couldn't open it (error " + std::to_wstring(GetLastError()) + L").";
    }
}

IconPixels iconPixels(HICON icon) {
    IconPixels out;
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return out;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    const int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    if (ii.hbmColor && w > 0 && h > 0 && w <= 256 && h <= 256) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> bgra((size_t)w * (size_t)h * 4);
        HDC dc = GetDC(nullptr);
        const bool ok = GetDIBits(dc, ii.hbmColor, 0, (UINT)h, bgra.data(), &bi, DIB_RGB_COLORS) == h;
        bool anyAlpha = false;
        for (size_t i = 3; ok && i < bgra.size(); i += 4) anyAlpha = anyAlpha || bgra[i] != 0;
        if (ok && !anyAlpha) {
            // An old icon without alpha: transparency comes from its mask.
            std::vector<unsigned char> mask((size_t)w * (size_t)h * 4);
            const bool m = GetDIBits(dc, ii.hbmMask, 0, (UINT)h, mask.data(), &bi, DIB_RGB_COLORS) == h;
            for (size_t i = 0; i < bgra.size(); i += 4) bgra[i + 3] = m && mask[i] ? 0 : 255;
        }
        ReleaseDC(nullptr, dc);
        if (ok) {
            out.w = w;
            out.h = h;
            out.rgba.resize(bgra.size());
            for (size_t i = 0; i < bgra.size(); i += 4) {   // an icon's BGRA (straight alpha) -> RGBA
                out.rgba[i] = bgra[i + 2];
                out.rgba[i + 1] = bgra[i + 1];
                out.rgba[i + 2] = bgra[i];
                out.rgba[i + 3] = bgra[i + 3];
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

const IconPixels* systemIcon(int index, int size) {
    static std::map<std::pair<int, int>, IconPixels> cache;
    if (index < 0) return nullptr;
    const auto key = std::make_pair(index, size);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.rgba.empty() ? nullptr : &it->second;
    IconPixels& out = cache[key];
    const int list = size <= 16 ? SHIL_SMALL : size <= 32 ? SHIL_LARGE : SHIL_EXTRALARGE;
    IImageList* il = nullptr;
    if (FAILED(SHGetImageList(list, __uuidof(IImageList), reinterpret_cast<void**>(&il))) || !il) return nullptr;
    HICON icon = nullptr;
    il->GetIcon(index, ILD_TRANSPARENT, &icon);
    il->Release();
    if (!icon) return nullptr;
    out = iconPixels(icon);
    DestroyIcon(icon);
    return out.rgba.empty() ? nullptr : &out;
}

std::wstring describeListError(ListError e, DWORD code, const std::wstring& path, std::wstring* title) {
    auto set = [&](const wchar_t* t) { if (title) *title = t; };
    switch (e) {
        case ListError::NotFound:
            set(L"This folder isn't there");
            return L"\"" + path + L"\" doesn't exist. It may have been moved, renamed or deleted, or the path may be mistyped.";
        case ListError::AccessDenied:
            set(L"You don't have permission to open this folder");
            return L"Windows didn't allow your account to read \"" + path +
                   L"\". It may belong to another user or to Windows itself. NorthRay Files never asks for administrator rights on its own.";
        case ListError::NotReady:
            set(L"This drive isn't ready");
            return L"There's no disc or card in it, or it was disconnected. Put it back, then try again.";
        case ListError::NetworkUnreachable:
            set(L"This network location can't be reached");
            return L"\"" + path + L"\" didn't answer. Check that you're connected to the network and that the computer sharing it is on.";
        case ListError::PathTooLong:
            set(L"This path is too long");
            return L"Windows couldn't use a path this long here.";
        case ListError::NotAFolder:
            set(L"That's a file");
            return L"";
        case ListError::Other:
        case ListError::None:
            break;
    }
    set(L"This folder couldn't be read");
    return L"Windows reported error " + std::to_wstring(code) + L" while reading \"" + path + L"\".";
}

}  // namespace nrf
