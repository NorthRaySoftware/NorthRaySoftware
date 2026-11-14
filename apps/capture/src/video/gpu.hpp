// NorthRay Capture: the one Direct3D 11 device that capture, composition and the preview share
// (system 5's base). Pictures stay on the graphics card from capture to screen.
//
// Drawing is a single small shader: a textured or solid four-cornered shape, placed in the target's
// pixels, which is all the compositor needs for moving, scaling, rotating, cropping and mirroring.
// The device is multithread-protected so capture threads can create and fill textures; drawing
// happens on the video thread only.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace ncap::video {

using Microsoft::WRL::ComPtr;

struct Pt { float x = 0, y = 0; };

// A texture that can be drawn into and drawn from.
struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool ensure(ID3D11Device* device, int width, int height);   // (re)created when the size changes
    void reset() { tex.Reset(); rtv.Reset(); srv.Reset(); w = h = 0; }
};

class Gpu {
public:
    bool create(std::wstring& error);
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    const std::wstring& adapterName() const { return adapterName_; }
    LUID adapterLuid() const { return luid_; }

    // Where the following draws go.
    void begin(ID3D11RenderTargetView* rtv, int w, int h);
    void clear(ID3D11RenderTargetView* rtv, float r, float g, float b, float a);

    enum class Blend { Alpha, Invert };   // Invert: white in the source inverts what's under it
    void setBlend(Blend b);
    // `corners` are top-left, top-right, bottom-right, bottom-left, in target pixels; `uv` the same
    // corners in the texture (0..1). `opaque` ignores the texture's alpha (captures, cameras).
    void drawTexture(ID3D11ShaderResourceView* srv, const Pt corners[4], const Pt uv[4], float opacity, bool opaque);
    void drawSolid(const Pt corners[4], float r, float g, float b, float a);
    // A luma wipe: `srv` (the new scene's canvas) shown where `mask` (one byte per pixel, stretched
    // over the corners) is below the progress, with a soft edge.
    void drawLumaWipe(ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* mask, const Pt corners[4], float progress, float softness, bool invert);
    ComPtr<ID3D11ShaderResourceView> makeMask(const std::uint8_t* pixels, int w, int h);
    void fillRect(float x, float y, float w, float h, float r, float g, float b, float a);
    void line(Pt a, Pt b, float thickness, float r, float g, float b2, float alpha);

private:
    struct Constants {
        float pos[4][4];
        float uv[4][4];
        float colour[4];
        float flags[4];   // x: textured, y: opaque
    };
    void draw(const Constants& c, ID3D11ShaderResourceView* srv);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> alpha_, invert_;
    ComPtr<ID3D11RasterizerState> raster_;
    std::wstring adapterName_;
    LUID luid_{};
    int targetW_ = 1, targetH_ = 1;
};

}  // namespace ncap::video
