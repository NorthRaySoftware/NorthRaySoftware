#include "operations.hpp"

#include "nrf/operations.hpp"
#include "nrf/paths.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>

#include <thread>

namespace nrf::ops {

namespace {

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring describe(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED) return L"Windows didn't allow it (access denied).";
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) return L"Another program has it open.";
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL)) return L"The drive is full.";
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return L"It isn't there any more.";
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr, 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"It didn't work (error " + std::to_wstring((unsigned long)hr) + L")." : s;
}

// The older Shell call, for Windows setups where IFileOperation isn't available.
Result shFileOperation(HWND owner, UINT func, const std::vector<std::wstring>& from, const std::wstring& to, FILEOP_FLAGS flags) {
    std::wstring list;
    for (auto& f : from) { list += f; list.push_back(L'\0'); }
    list.push_back(L'\0');
    std::wstring dest = to;
    dest.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = func;
    op.pFrom = list.c_str();
    op.pTo = to.empty() ? nullptr : dest.c_str();
    op.fFlags = flags;
    Result r;
    const int rc = SHFileOperationW(&op);
    if (op.fAnyOperationsAborted) { r.ok = false; r.cancelled = true; }
    else if (rc != 0) { r.ok = false; r.message = describe(HRESULT_FROM_WIN32((DWORD)rc)); }
    return r;
}

struct Op {
    Com<IFileOperation> op;
    bool create(HWND owner, DWORD flags) {
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put())))) return false;
        op->SetOwnerWindow(owner);
        return SUCCEEDED(op->SetOperationFlags(flags));
    }
    Result perform() {
        Result r;
        const HRESULT hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        if (aborted) { r.ok = false; r.cancelled = true; }
        else if (FAILED(hr)) { r.ok = false; r.message = describe(hr); }
        return r;
    }
};

Com<IShellItem> item(const std::wstring& path) {
    Com<IShellItem> i;
    SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(i.put()));
    return i;
}

}  // namespace

Result transfer(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destination, bool move) {
    if (sources.empty()) return {};
    // Copying into the same folder: a new name, never a clash.
    bool sameFolder = false;
    for (auto& s : sources) if (CompareStringOrdinal(parentOf(s).c_str(), -1, destination.c_str(), -1, TRUE) == CSTR_EQUAL) sameFolder = true;
    Op o;
    const DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | (sameFolder && !move ? FOF_RENAMEONCOLLISION : 0) | FOFX_ADDUNDORECORD;
    if (o.create(owner, flags)) {
        auto dest = item(destination);
        bool queued = (bool)dest;
        for (auto& s : sources) {
            auto it = item(s);
            if (!it || !dest) { queued = false; break; }
            if (FAILED(move ? o.op->MoveItem(it.p, dest.p, nullptr, nullptr) : o.op->CopyItem(it.p, dest.p, nullptr, nullptr))) { queued = false; break; }
        }
        if (queued) return o.perform();
    }
    return shFileOperation(owner, move ? FO_MOVE : FO_COPY, sources, destination,
                           FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | (sameFolder && !move ? FOF_RENAMEONCOLLISION : 0));
}

Result copyAs(HWND owner, const std::vector<std::pair<std::wstring, std::wstring>>& fromTo) {
    if (fromTo.empty()) return {};
    Op o;
    if (o.create(owner, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD)) {
        bool queued = true;
        for (auto& [from, to] : fromTo) {
            auto it = item(from), dest = item(parentOf(to));
            const std::wstring name = nameOf(to);
            if (!it || !dest || FAILED(o.op->CopyItem(it.p, dest.p, name.c_str(), nullptr))) { queued = false; break; }
        }
        if (queued) return o.perform();
    }
    std::vector<std::wstring> from, to;
    for (auto& [f, t] : fromTo) { from.push_back(f); to.push_back(t); }
    std::wstring dests;
    for (auto& t : to) { dests += t; dests.push_back(L'\0'); }
    // pTo holds one destination per source (FOF_MULTIDESTFILES).
    std::wstring list;
    for (auto& f : from) { list += f; list.push_back(L'\0'); }
    list.push_back(L'\0');
    dests.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_COPY;
    op.pFrom = list.c_str();
    op.pTo = dests.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_MULTIDESTFILES;
    Result r;
    const int rc = SHFileOperationW(&op);
    if (op.fAnyOperationsAborted) { r.ok = false; r.cancelled = true; }
    else if (rc != 0) { r.ok = false; r.message = describe(HRESULT_FROM_WIN32((DWORD)rc)); }
    return r;
}

Result remove(HWND owner, const std::vector<std::wstring>& paths, bool permanently) {
    if (paths.empty()) return {};
    Op o;
    const DWORD flags = permanently ? (FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING) : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING);
    if (o.create(owner, flags)) {
        bool queued = true;
        for (auto& p : paths) {
            auto it = item(p);
            if (!it || FAILED(o.op->DeleteItem(it.p, nullptr))) { queued = false; break; }
        }
        if (queued) return o.perform();
    }
    return shFileOperation(owner, FO_DELETE, paths, {}, permanently ? FOF_NOCONFIRMATION : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING));
}

Result rename(HWND owner, const std::wstring& path, const std::wstring& newName) {
    Op o;
    if (o.create(owner, FOF_ALLOWUNDO | FOFX_ADDUNDORECORD)) {
        auto it = item(path);
        if (it && SUCCEEDED(o.op->RenameItem(it.p, newName.c_str(), nullptr))) return o.perform();
    }
    const std::wstring to = join(parentOf(path), newName);
    Result r;
    if (!MoveFileExW(extendedPath(path).c_str(), extendedPath(to).c_str(), 0)) { r.ok = false; r.message = describe(HRESULT_FROM_WIN32(GetLastError())); }
    return r;
}

Result newFolder(const std::wstring& path) {
    Result r;
    if (!CreateDirectoryW(extendedPath(path).c_str(), nullptr)) { r.ok = false; r.message = describe(HRESULT_FROM_WIN32(GetLastError())); }
    return r;
}

// ------------------------------------------------------------------ clipboard

bool putOnClipboard(HWND owner, const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t chars = 1;
    for (auto& p : paths) chars += p.size() + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    bool ok = false;
    if (g) {
        auto* df = static_cast<DROPFILES*>(GlobalLock(g));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        auto* out = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
        for (auto& p : paths) { memcpy(out, p.c_str(), p.size() * sizeof(wchar_t)); out += p.size() + 1; }
        GlobalUnlock(g);
        ok = SetClipboardData(CF_HDROP, g) != nullptr;
        if (!ok) GlobalFree(g);
    }
    if (ok) {
        const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (HGLOBAL e = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
            *static_cast<DWORD*>(GlobalLock(e)) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(e);
            if (!SetClipboardData(effectFormat, e)) GlobalFree(e);
        }
        // The paths as text too, for pasting into a document.
        std::wstring text;
        for (auto& p : paths) text += (text.empty() ? L"" : L"\r\n") + p;
        if (HGLOBAL t = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(t), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(t);
            if (!SetClipboardData(CF_UNICODETEXT, t)) GlobalFree(t);
        }
    }
    CloseClipboard();
    return ok;
}

bool clipboardHasFiles() { return IsClipboardFormatAvailable(CF_HDROP) != FALSE; }

ClipboardFiles readClipboard(HWND owner) {
    ClipboardFiles out;
    if (!clipboardHasFiles() || !OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = static_cast<HDROP>(h);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && i < 100000; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            if (!p.empty()) out.paths.push_back(p);
        }
    }
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (HANDLE e = GetClipboardData(effectFormat)) {
        if (const DWORD* d = static_cast<const DWORD*>(GlobalLock(e))) {
            out.cut = (*d & DROPEFFECT_MOVE) != 0;
            GlobalUnlock(e);
        }
    }
    CloseClipboard();
    return out;
}

// ------------------------------------------------------------------ Windows' right-click menu

namespace {
IContextMenu2* g_menu2 = nullptr;
IContextMenu3* g_menu3 = nullptr;
}

bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (g_menu3) return SUCCEEDED(g_menu3->HandleMenuMsg2(msg, wp, lp, &result));
    if (g_menu2) { result = 0; return SUCCEEDED(g_menu2->HandleMenuMsg(msg, wp, lp)); }
    return false;
}

bool showShellMenu(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& paths, POINT screen, bool extended) {
    Com<IContextMenu> cm;
    std::vector<PIDLIST_ABSOLUTE> owned;
    if (paths.empty()) {
        // The folder's background: New, Paste, Properties...
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
        owned.push_back(pidl);
        Com<IShellFolder> sf;
        if (FAILED(SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(sf.put()))) ||
            FAILED(sf->CreateViewObject(owner, IID_PPV_ARGS(cm.put())))) {
            for (auto p : owned) CoTaskMemFree(p);
            return false;
        }
    } else {
        Com<IShellFolder> parent;
        std::vector<PCUITEMID_CHILD> children;
        for (auto& p : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) continue;
            owned.push_back(pidl);
            PCUITEMID_CHILD child = nullptr;
            if (!parent) { if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(parent.put()), &child))) continue; }
            else child = ILFindLastID(pidl);
            children.push_back(child);
        }
        if (!parent || children.empty() ||
            FAILED(parent->GetUIObjectOf(owner, (UINT)children.size(), children.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(cm.put())))) {
            for (auto p : owned) CoTaskMemFree(p);
            return false;
        }
    }
    bool ran = false;
    if (HMENU menu = CreatePopupMenu()) {
        if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_CANRENAME | (extended ? CMF_EXTENDEDVERBS : 0)))) {
            cm->QueryInterface(IID_PPV_ARGS(&g_menu3));
            if (!g_menu3) cm->QueryInterface(IID_PPV_ARGS(&g_menu2));
            const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, owner, nullptr);
            if (g_menu3) { g_menu3->Release(); g_menu3 = nullptr; }
            if (g_menu2) { g_menu2->Release(); g_menu2 = nullptr; }
            if (cmd > 0) {
                CMINVOKECOMMANDINFOEX info{};
                info.cbSize = sizeof info;
                info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                if (GetKeyState(VK_CONTROL) & 0x8000) info.fMask |= CMIC_MASK_CONTROL_DOWN;
                if (GetKeyState(VK_SHIFT) & 0x8000) info.fMask |= CMIC_MASK_SHIFT_DOWN;
                info.hwnd = owner;
                info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                info.nShow = SW_SHOWNORMAL;
                info.ptInvoke = screen;
                ran = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
            }
        }
        DestroyMenu(menu);
    }
    for (auto p : owned) CoTaskMemFree(p);
    return ran;
}

// ------------------------------------------------------------------ watching

void watchFolder(HWND window, UINT message, std::wstring folder, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest) {
    std::thread([=] {
        HANDLE h = FindFirstChangeNotificationW(extendedPath(folder).c_str(), FALSE,
                                                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        if (h == INVALID_HANDLE_VALUE) return;
        while (latest->load() == request) {
            const DWORD w = WaitForSingleObject(h, 500);
            if (latest->load() != request) break;
            if (w == WAIT_OBJECT_0) {
                PostMessageW(window, message, (WPARAM)request, 0);
                if (!FindNextChangeNotification(h)) break;
            } else if (w != WAIT_TIMEOUT) {
                break;
            }
        }
        FindCloseChangeNotification(h);
    }).detach();
}

// ------------------------------------------------------------------ Recycle Bin

bool recycleBinInfo(std::uint64_t& items, std::uint64_t& bytes) {
    SHQUERYRBINFO info{};
    info.cbSize = sizeof info;
    if (FAILED(SHQueryRecycleBinW(nullptr, &info))) return false;
    items = (std::uint64_t)info.i64NumItems;
    bytes = (std::uint64_t)info.i64Size;
    return true;
}

Result emptyRecycleBin(HWND owner) {
    Result r;
    const HRESULT hr = SHEmptyRecycleBinW(owner, nullptr, 0);
    if (FAILED(hr) && hr != E_UNEXPECTED) { r.ok = false; r.message = describe(hr); }
    if (hr == E_UNEXPECTED) r.cancelled = true;   // nothing to empty, or the person said no
    return r;
}


namespace {

Com<IShellFolder2> recycleBinFolder() {
    Com<IShellFolder2> bin;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &pidl)) || !pidl) return bin;
    SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(bin.put()));
    CoTaskMemFree(pidl);
    return bin;
}

std::wstring strRet(IShellFolder* f, PCUITEMID_CHILD child, SHGDNF how) {
    STRRET sr{};
    if (FAILED(f->GetDisplayNameOf(child, how, &sr))) return L"";
    wchar_t* s = nullptr;
    if (FAILED(StrRetToStrW(&sr, child, &s)) || !s) return L"";
    std::wstring out = s;
    CoTaskMemFree(s);
    return out;
}

// The Recycle Bin's own columns: where it came from, and when it was deleted.
const PROPERTYKEY kDisplacedFrom = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
const PROPERTYKEY kDisplacedDate = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};
const PROPERTYKEY kSize = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 12};

}  // namespace

bool listRecycleBin(std::vector<BinItem>& out, std::wstring& problem) {
    Com<IShellFolder2> bin = recycleBinFolder();
    if (!bin) { problem = L"The Recycle Bin couldn't be opened."; return false; }
    Com<IEnumIDList> en;
    if (bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, en.put()) != S_OK || !en) return true;   // empty
    PITEMID_CHILD child = nullptr;
    while (en->Next(1, &child, nullptr) == S_OK) {
        BinItem it;
        it.id = strRet(bin.p, child, SHGDN_INFOLDER | SHGDN_FORPARSING);
        it.file = strRet(bin.p, child, SHGDN_FORPARSING);
        it.name = strRet(bin.p, child, SHGDN_INFOLDER);
        SFGAOF a = SFGAO_FOLDER;
        PCUITEMID_CHILD c = child;
        if (SUCCEEDED(bin->GetAttributesOf(1, &c, &a))) it.folder = (a & SFGAO_FOLDER) != 0;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bin->GetDetailsEx(child, &kDisplacedFrom, &v))) {
            if (v.vt == VT_BSTR && v.bstrVal) it.from = v.bstrVal;
            VariantClear(&v);
        }
        if (SUCCEEDED(bin->GetDetailsEx(child, &kDisplacedDate, &v))) {
            if (v.vt == VT_DATE) {
                SYSTEMTIME st;
                FILETIME ft, utc;
                if (VariantTimeToSystemTime(v.date, &st) && SystemTimeToFileTime(&st, &ft) && LocalFileTimeToFileTime(&ft, &utc))
                    it.deleted = ((std::uint64_t)utc.dwHighDateTime << 32) | utc.dwLowDateTime;
            }
            VariantClear(&v);
        }
        if (SUCCEEDED(bin->GetDetailsEx(child, &kSize, &v))) {
            if (v.vt == VT_UI8) it.size = v.ullVal;
            else if (v.vt == VT_UI4) it.size = v.ulVal;
            else if (v.vt == VT_I8) it.size = (std::uint64_t)v.llVal;
            VariantClear(&v);
        }
        // Older shells only give the columns as text.
        if (it.from.empty()) {
            SHELLDETAILS d{};
            if (SUCCEEDED(bin->GetDetailsOf(child, 1, &d))) {
                wchar_t* s = nullptr;
                if (SUCCEEDED(StrRetToStrW(&d.str, child, &s)) && s) { it.from = s; CoTaskMemFree(s); }
            }
        }
        if (it.size == 0 && !it.folder && !it.file.empty()) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(it.file.c_str(), GetFileExInfoStandard, &fa)) it.size = ((std::uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        }
        CoTaskMemFree(child);
        // Only what the Recycle Bin says it holds (where it came from, or when it was deleted): a shell that
        // answers with something else (Wine's lists the desktop) isn't shown as if it were deleted things.
        if (!it.id.empty() && (!it.from.empty() || it.deleted != 0)) out.push_back(std::move(it));
    }
    return true;
}

Result recycleBinCommand(HWND owner, const std::vector<std::wstring>& ids, bool restore) {
    Result r;
    Com<IShellFolder2> bin = recycleBinFolder();
    if (!bin) { r.ok = false; r.message = L"The Recycle Bin couldn't be opened."; return r; }
    std::vector<PITEMID_CHILD> kids;
    for (const auto& id : ids) {
        PIDLIST_RELATIVE rel = nullptr;
        if (SUCCEEDED(bin->ParseDisplayName(owner, nullptr, const_cast<LPWSTR>(id.c_str()), nullptr, &rel, nullptr)) && rel)
            kids.push_back(reinterpret_cast<PITEMID_CHILD>(rel));
    }
    if (kids.empty()) { r.ok = false; r.message = L"They aren't in the Recycle Bin any more."; return r; }
    Com<IContextMenu> cm;
    std::vector<PCUITEMID_CHILD> constKids(kids.begin(), kids.end());
    HRESULT hr = bin->GetUIObjectOf(owner, (UINT)constKids.size(), constKids.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(cm.put()));
    if (SUCCEEDED(hr)) {
        HMENU m = CreatePopupMenu();
        cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
        CMINVOKECOMMANDINFOEX ci{};
        ci.cbSize = sizeof ci;
        ci.fMask = CMIC_MASK_UNICODE;
        ci.hwnd = owner;
        ci.lpVerb = restore ? "undelete" : "delete";
        ci.lpVerbW = restore ? L"undelete" : L"delete";
        ci.nShow = SW_SHOWNORMAL;
        hr = cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        DestroyMenu(m);
    }
    for (auto k : kids) CoTaskMemFree(k);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) r.cancelled = true;
    else if (FAILED(hr)) { r.ok = false; r.message = describe(hr); }
    return r;
}

}  // namespace nrf::ops
