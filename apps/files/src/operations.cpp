#include "operations.hpp"

#include "nrf/operations.hpp"
#include "nrf/paths.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

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

}  // namespace nrf::ops
