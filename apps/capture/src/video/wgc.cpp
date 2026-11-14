#include "video/wgc.hpp"

#include <winstring.h>

#include <cwchar>

namespace ncap::wgc {

namespace {

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);

struct Api {
    RoGetActivationFactoryFn factory = nullptr;
    WindowsCreateStringFn create = nullptr;
    WindowsDeleteStringFn del = nullptr;
    CreateDeviceFn wrap = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api r;
        if (HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            r.factory = reinterpret_cast<RoGetActivationFactoryFn>(reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory")));
            r.create = reinterpret_cast<WindowsCreateStringFn>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
            r.del = reinterpret_cast<WindowsDeleteStringFn>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
        }
        if (HMODULE d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            r.wrap = reinterpret_cast<CreateDeviceFn>(reinterpret_cast<void*>(GetProcAddress(d3d, "CreateDirect3D11DeviceFromDXGIDevice")));
        r.ok = r.factory && r.create && r.del && r.wrap;
        return r;
    }();
    return a;
}

template <class T>
HRESULT factory(const wchar_t* className, T** out) {
    *out = nullptr;
    const Api& a = api();
    if (!a.ok) return E_NOTIMPL;
    HSTRING name = nullptr;
    HRESULT hr = a.create(className, (UINT32)wcslen(className), &name);
    if (FAILED(hr)) return hr;
    hr = a.factory(name, __uuidof(T), reinterpret_cast<void**>(out));
    a.del(name);
    return hr;
}

}  // namespace

bool supported() {
    static const bool yes = [] {
        IGraphicsCaptureSessionStatics* statics = nullptr;
        if (FAILED(factory(L"Windows.Graphics.Capture.GraphicsCaptureSession", &statics)) || !statics) return false;
        boolean ok = false;
        const bool r = SUCCEEDED(statics->IsSupported(&ok)) && ok;
        statics->Release();
        // The free-threaded frame pool (needed here) arrived a release later than capture itself.
        IDirect3D11CaptureFramePoolStatics2* pool = nullptr;
        const bool pooled = SUCCEEDED(factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", &pool)) && pool;
        if (pool) pool->Release();
        return r && pooled;
    }();
    return yes;
}

HRESULT wrapDevice(ID3D11Device* device, IInspectable** out) {
    *out = nullptr;
    if (!api().ok) return E_NOTIMPL;
    IDXGIDevice* dxgi = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
    if (FAILED(hr)) return hr;
    hr = api().wrap(dxgi, out);
    dxgi->Release();
    return hr;
}

HRESULT itemForWindow(HWND window, IGraphicsCaptureItem** out) {
    IGraphicsCaptureItemInterop* interop = nullptr;
    HRESULT hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", &interop);
    if (FAILED(hr)) return hr;
    hr = interop->CreateForWindow(window, __uuidof(IGraphicsCaptureItem), reinterpret_cast<void**>(out));
    interop->Release();
    return hr;
}

HRESULT itemForMonitor(HMONITOR monitor, IGraphicsCaptureItem** out) {
    IGraphicsCaptureItemInterop* interop = nullptr;
    HRESULT hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", &interop);
    if (FAILED(hr)) return hr;
    hr = interop->CreateForMonitor(monitor, __uuidof(IGraphicsCaptureItem), reinterpret_cast<void**>(out));
    interop->Release();
    return hr;
}

HRESULT createFramePool(IInspectable* device, SizeInt32 size, IDirect3D11CaptureFramePool** out) {
    IDirect3D11CaptureFramePoolStatics2* statics = nullptr;
    HRESULT hr = factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", &statics);
    if (FAILED(hr)) return hr;
    hr = statics->CreateFreeThreaded(device, kPixelFormatBGRA8, 2, size, out);
    statics->Release();
    return hr;
}

}  // namespace ncap::wgc
