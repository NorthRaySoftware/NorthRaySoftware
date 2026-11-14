// Small COM helpers for WebView2 without WRL (which MinGW doesn't have):
// an owning pointer and a callback object built from a lambda.
#pragma once

#include <windows.h>
#include <objbase.h>

#include <functional>
#include <string>
#include <utility>

namespace nrb {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }
    ComPtr& operator=(const ComPtr& o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    // Takes a reference the caller already owns (e.g. from an out-parameter).
    static ComPtr adopt(T* p) { ComPtr c; c.p_ = p; return c; }
    // Shares a reference (AddRef).
    static ComPtr share(T* p) { ComPtr c; c.p_ = p; if (p) p->AddRef(); return c; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() { reset(); return &p_; }
    // Another interface of the same object, or empty.
    template <class U>
    ComPtr<U> as() const {
        ComPtr<U> u;
        if (p_) p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(u.put()));
        return u;
    }

private:
    T* p_ = nullptr;
};

// A COM object implementing one WebView2 handler interface with a lambda.
// Created with a reference count of 1, which the caller releases after
// passing it on (WebView2 keeps its own reference).
template <class Iface, class... Args>
class Callback final : public Iface {
public:
    explicit Callback(std::function<HRESULT(Args...)> fn) : fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(Iface))) {
            *out = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn_ ? fn_(args...) : S_OK; }

private:
    std::function<HRESULT(Args...)> fn_;
    LONG refs_ = 1;
};

// Makes a handler and hands ownership to a ComPtr.
template <class Iface, class... Args, class Fn>
ComPtr<Iface> makeHandler(Fn&& fn) {
    return ComPtr<Iface>::adopt(new Callback<Iface, Args...>(std::function<HRESULT(Args...)>(std::forward<Fn>(fn))));
}

// A string WebView2 allocated (CoTaskMemAlloc), copied and freed.
inline std::wstring takeString(LPWSTR s) {
    std::wstring out = s ? s : L"";
    if (s) CoTaskMemFree(s);
    return out;
}

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace nrb
