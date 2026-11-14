// Windows.Graphics.Capture through its COM (ABI) interfaces. MinGW's headers have only part of
// this API, so the interfaces Capture uses are declared here from Microsoft's published metadata
// (interface GUIDs and method order). Activated at run time through combase (RoGetActivationFactory),
// so NorthRay Capture still starts on a Windows without it and falls back to other methods.
#pragma once

#include <windows.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>

namespace ncap::wgc {

struct SizeInt32 { std::int32_t Width, Height; };
struct EventToken { std::int64_t value; };

struct DECLSPEC_UUID("79c3f95b-31f7-4ec2-a464-632ef5d30760") IGraphicsCaptureItem : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(IUnknown* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventToken token) = 0;
};

struct DECLSPEC_UUID("3628e81b-3cac-4c60-b7f4-23ce0e0c3356") IGraphicsCaptureItemInterop : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid, void** result) = 0;
};

struct DECLSPEC_UUID("814e42a9-f70f-4ad7-939b-fddcc6eb880d") IGraphicsCaptureSession : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct DECLSPEC_UUID("2c39ae40-7d2e-5044-804e-8b6799d4cf9e") IGraphicsCaptureSession2 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean value) = 0;
};
struct DECLSPEC_UUID("f2cdd966-22ae-5ea1-9596-3a289344c3be") IGraphicsCaptureSession3 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean value) = 0;
};
struct DECLSPEC_UUID("2224a540-5974-49aa-b232-0882536f4cb5") IGraphicsCaptureSessionStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(boolean* result) = 0;
};

struct DECLSPEC_UUID("fa50c623-38da-4b32-acf3-fa9734ad800e") IDirect3D11CaptureFrame : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable** value) = 0;   // an IDirect3DSurface
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(std::int64_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32* value) = 0;
};

struct DECLSPEC_UUID("24eb6d22-1975-422e-82e7-780dbd8ddf24") IDirect3D11CaptureFramePool : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, std::int32_t pixelFormat, std::int32_t buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(IUnknown* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem* item, IGraphicsCaptureSession** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable** value) = 0;
};

struct DECLSPEC_UUID("589b103f-6bbc-5df5-a991-02e28b3b66d5") IDirect3D11CaptureFramePoolStatics2 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, std::int32_t pixelFormat, std::int32_t buffers, SizeInt32 size,
                                                         IDirect3D11CaptureFramePool** result) = 0;
};

struct DECLSPEC_UUID("a9b3d012-3df2-4ee3-b8d1-8695f457d3c1") IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;
};

struct DECLSPEC_UUID("30d5a829-7fa4-4026-83bb-d75bae4ea99e") IClosable : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

constexpr std::int32_t kPixelFormatBGRA8 = 87;   // DirectXPixelFormat::B8G8R8A8UIntNormalized

// Is Windows.Graphics.Capture available (Windows 10 1903 and later, and not blocked by policy)?
bool supported();
// The WinRT device wrapper for a D3D11 device (CreateDirect3D11DeviceFromDXGIDevice).
HRESULT wrapDevice(ID3D11Device* device, IInspectable** out);
HRESULT itemForWindow(HWND window, IGraphicsCaptureItem** out);
HRESULT itemForMonitor(HMONITOR monitor, IGraphicsCaptureItem** out);
HRESULT createFramePool(IInspectable* device, SizeInt32 size, IDirect3D11CaptureFramePool** out);

}  // namespace ncap::wgc

// MinGW's __uuidof needs these for the declared interfaces.
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureItem, 0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60)
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureItemInterop, 0x3628e81b, 0x3cac, 0x4c60, 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56)
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureSession, 0x814e42a9, 0xf70f, 0x4ad7, 0x93, 0x9b, 0xfd, 0xdc, 0xc6, 0xeb, 0x88, 0x0d)
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureSession2, 0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e)
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureSession3, 0xf2cdd966, 0x22ae, 0x5ea1, 0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe)
__CRT_UUID_DECL(ncap::wgc::IGraphicsCaptureSessionStatics, 0x2224a540, 0x5974, 0x49aa, 0xb2, 0x32, 0x08, 0x82, 0x53, 0x6f, 0x4c, 0xb5)
__CRT_UUID_DECL(ncap::wgc::IDirect3D11CaptureFrame, 0xfa50c623, 0x38da, 0x4b32, 0xac, 0xf3, 0xfa, 0x97, 0x34, 0xad, 0x80, 0x0e)
__CRT_UUID_DECL(ncap::wgc::IDirect3D11CaptureFramePool, 0x24eb6d22, 0x1975, 0x422e, 0x82, 0xe7, 0x78, 0x0d, 0xbd, 0x8d, 0xdf, 0x24)
__CRT_UUID_DECL(ncap::wgc::IDirect3D11CaptureFramePoolStatics2, 0x589b103f, 0x6bbc, 0x5df5, 0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5)
__CRT_UUID_DECL(ncap::wgc::IDirect3DDxgiInterfaceAccess, 0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1)
__CRT_UUID_DECL(ncap::wgc::IClosable, 0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e)
