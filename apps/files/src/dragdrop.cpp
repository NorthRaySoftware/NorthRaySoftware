#include "dragdrop.hpp"

#include "nrf/paths.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <map>
#include <memory>

namespace nrf::dnd {

namespace {

class Target : public IDropTarget {
public:
    explicit Target(DropHandlers h) : h_(std::move(h)) {}
    virtual ~Target() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        paths_ = filesIn(data);
        allowed_ = *effect;
        *effect = paths_.empty() || !h_.over ? DROPEFFECT_NONE : h_.over(paths_, {pt.x, pt.y}, keys, allowed_) & allowed_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        *effect = paths_.empty() || !h_.over ? DROPEFFECT_NONE : h_.over(paths_, {pt.x, pt.y}, keys, allowed_) & allowed_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        paths_.clear();
        if (h_.leave) h_.leave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        auto paths = paths_.empty() ? filesIn(data) : paths_;
        paths_.clear();
        *effect = paths.empty() || !h_.drop ? DROPEFFECT_NONE : h_.drop(paths, {pt.x, pt.y}, keys, *effect);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    DropHandlers h_;
    std::vector<std::wstring> paths_;
    DWORD allowed_ = 0;
};

class Source : public IDropSource {
public:
    virtual ~Source() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    LONG refs_ = 1;
};

std::map<HWND, Target*>& targets() {
    static std::map<HWND, Target*> t;
    return t;
}

}  // namespace

std::vector<std::wstring> filesIn(IDataObject* data) {
    std::vector<std::wstring> out;
    if (!data) return out;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(data->GetData(&fe, &sm))) return out;
    if (HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && i < 100000; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            if (!p.empty()) out.push_back(p);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return out;
}

bool registerTarget(HWND window, DropHandlers handlers) {
    auto* t = new Target(std::move(handlers));
    if (FAILED(RegisterDragDrop(window, t))) {
        t->Release();
        return false;
    }
    targets()[window] = t;
    return true;
}

void revokeTarget(HWND window) {
    auto it = targets().find(window);
    if (it == targets().end()) return;
    RevokeDragDrop(window);
    it->second->Release();
    targets().erase(it);
}

DWORD dragOut(const std::vector<std::wstring>& paths) {
    // The Shell's own data object for the items, as File Explorer would give.
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    IDataObject* data = nullptr;
    // Things in one folder: that folder's data object (the way File Explorer's own drags are made).
    bool oneFolder = !paths.empty();
    for (const auto& p : paths) oneFolder = oneFolder && _wcsicmp(parentOf(p).c_str(), parentOf(paths[0]).c_str()) == 0;
    if (oneFolder && pidls.size() == paths.size()) {
        IShellFolder* parent = nullptr;
        PCUITEMID_CHILD first = nullptr;
        if (SUCCEEDED(SHBindToParent(pidls[0], IID_PPV_ARGS(&parent), &first)) && parent) {
            std::vector<PCUITEMID_CHILD> kids;
            for (auto p : pidls) kids.push_back(ILFindLastID(p));
            parent->GetUIObjectOf(nullptr, (UINT)kids.size(), kids.data(), IID_IDataObject, nullptr, reinterpret_cast<void**>(&data));
            parent->Release();
        }
    }
    if (!data && !pidls.empty()) {
        // From several folders (search results, collections): an item array's data object.
        std::vector<PCIDLIST_ABSOLUTE> constPidls(pidls.begin(), pidls.end());
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)constPidls.size(), constPidls.data(), &items)) && items) {
            items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
            items->Release();
        }
    }
    DWORD effect = DROPEFFECT_NONE;
    if (data) {
        auto* src = new Source;
        DoDragDrop(data, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        src->Release();
        data->Release();
    }
    for (auto p : pidls) CoTaskMemFree(p);
    return effect;
}

}  // namespace nrf::dnd
