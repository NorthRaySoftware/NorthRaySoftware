#include "video/gpu.hpp"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ncap::video {

namespace {

// Positions come from the constant buffer (no vertex buffer): four corners as a triangle strip.
const char kShader[] = R"(
cbuffer C : register(b0) { float4 pos[4]; float4 uv[4]; float4 colour; float4 flags; };
Texture2D tex : register(t0);
Texture2D mask : register(t1);   // a luma wipe's pattern
SamplerState smp : register(s0);
struct V { float4 p : SV_Position; float2 t : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    // Strip order: top-left, top-right, bottom-left, bottom-right.
    uint k = id == 0 ? 0 : id == 1 ? 1 : id == 2 ? 3 : 2;
    V o;
    o.p = float4(pos[k].xy, 0, 1);
    o.t = uv[k].xy;
    return o;
}
float4 ps(V i) : SV_Target {
    if (flags.x < 0.5) return colour;
    float4 s = tex.Sample(smp, i.t);
    if (flags.y > 0.5) s.a = 1;
    if (flags.z > 0.5) {
        // Luma wipe (the same sum as ncap::transition::lumaAlpha): progress in flags.w, softness in
        // colour.r, invert in colour.g.
        float l = mask.Sample(smp, i.t).r;
        if (colour.g > 0.5) l = 1 - l;
        float soft = colour.r, p = flags.w;
        float t = p * (1 + soft) - soft;
        float a = soft > 0 ? saturate((t + soft - l) / soft) : ((l < t || p >= 1) ? 1 : 0);
        return float4(s.rgb, s.a * a);
    }
    return float4(s.rgb, s.a * colour.a);
}
)";

// A scene item with a border (ncap::border, the same sums): its picture inside rounded corners with a
// ring of colour over its edge (mode 1), or the glow around it (mode 0). uv.xy is the texture's place,
// uv.zw the pixel's place in the item (0..w, 0..h).
const char kFrameShader[] = R"(
cbuffer C : register(b0) { float4 pos[4]; float4 uv[4]; float4 colour; float4 flags; };
cbuffer F : register(b1) { float4 size; float4 c1; float4 c2; float4 glowCol; float4 g; float4 o; float4 k1; float4 k2; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 p : SV_Position; float4 t : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    uint k = id == 0 ? 0 : id == 1 ? 1 : id == 2 ? 3 : 2;
    V v;
    v.p = float4(pos[k].xy, 0, 1);
    v.t = uv[k];
    return v;
}
float sd(float2 p, float2 sz, float r) {
    r = clamp(r, 0, min(sz.x, sz.y) * 0.5);
    float2 q = abs(p - sz * 0.5) - (sz * 0.5 - r);
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
float cov(float d) { return saturate(0.5 - d); }
float4 ring(float2 L, float2 sz) {
    if (g.z < 0.5) return c1;
    float2 dir = float2(cos(g.y), sin(g.y));
    float half_ = (abs(dir.x) * sz.x + abs(dir.y) * sz.y) * 0.5;
    float t = half_ > 0 ? saturate(dot(L - sz * 0.5, dir) / (2 * half_) + 0.5) : 0;
    return lerp(c1, c2, t);
}
// Green screen (core/keying's sums): k1 = key Cb, Cr, similarity, smoothness; k2.x = spill, k2.y = on.
float4 keyed(float4 s) {
    if (k2.y < 0.5) return s;
    float3 c = s.rgb;
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    float2 cc = float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    float d = distance(cc, k1.xy);
    float a = smoothstep(k1.z, k1.z + k1.w, d);
    float nearK = 1 - smoothstep(k1.z, k1.z * 2 + k1.w + 0.05, d);
    c = lerp(c, y.xxx, nearK * k2.x);
    return float4(c, s.a * a);
}
float4 ps(V i) : SV_Target {
    float2 L = i.t.zw, sz = size.xy;
    float r = size.z, w = size.w;
    // The ring goes round the outside: the item's shape grown by its thickness.
    float2 L2 = L + w, sz2 = sz + 2 * w;
    float r2 = r > 0 ? r + w : 0;   // square stays square
    float inside = cov(sd(L, sz, r));
    float outer = cov(sd(L2, sz2, r2));
    if (g.w < 0.5) {
        float d = sd(L2 - o.xy, sz2, r2);
        float a = d <= 0 ? 1 : 1 - saturate(d / max(g.x, 0.001));
        a = a * a * (3 - 2 * a);
        return float4(glowCol.rgb, a * glowCol.a * (1 - outer) * o.z);
    }
    float4 s = tex.Sample(smp, i.t.xy);
    if (o.w > 0.5) s.a = 1;
    s = keyed(s);
    s.a *= inside;
    float ra = 0;
    float4 rc = c1;
    if (w > 0) {
        rc = ring(L2, sz2);
        ra = outer * (1 - inside) * rc.a;
    }
    float oa = ra + s.a * (1 - ra);
    float3 rgb = (rc.rgb * ra + s.rgb * s.a * (1 - ra)) / max(oa, 1e-5);
    return float4(rgb, oa * o.z);
}
)";

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                   ID3DBlob**);

CompileFn compiler() {
    static CompileFn fn = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    return fn;
}

}  // namespace

bool Target::ensure(ID3D11Device* device, int width, int height) {
    if (tex && w == width && h == height) return true;
    reset();
    if (width <= 0 || height <= 0) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)width;
    d.Height = (UINT)height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex)) || FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)) ||
        FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        reset();
        return false;
    }
    w = width;
    h = height;
    return true;
}

bool Gpu::create(std::wstring& error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // The video flag lets hardware encoders take these textures later (Milestone 5); some drivers
    // refuse it, so it's tried first and dropped if need be.
    HRESULT hr = E_FAIL;
    for (UINT flags : {(UINT)(D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT), (UINT)D3D11_CREATE_DEVICE_BGRA_SUPPORT}) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, (UINT)(sizeof levels / sizeof levels[0]),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (hr == E_INVALIDARG)   // no 11_1 on this system's runtime
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        error = L"Direct3D 11 isn't available on this graphics card or driver.";
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(adapter->GetDesc(&d))) {
            adapterName_ = d.Description;
            luid_ = d.AdapterLuid;
        }
    }

    CompileFn compile = compiler();
    if (!compile) {
        error = L"The Direct3D shader compiler (d3dcompiler_47.dll) is missing from Windows.";
        return false;
    }
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(compile(kShader, sizeof kShader - 1, "capture", nullptr, nullptr, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
        FAILED(compile(kShader, sizeof kShader - 1, "capture", nullptr, nullptr, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err)) ||
        FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
        error = L"The compositor's shader couldn't be prepared.";
        return false;
    }
    {   // Borders: if this fails, items are drawn without them (and the log says so).
        ComPtr<ID3DBlob> fv, fp, fe;
        if (FAILED(compile(kFrameShader, sizeof kFrameShader - 1, "frame", nullptr, nullptr, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &fv, &fe)) ||
            FAILED(compile(kFrameShader, sizeof kFrameShader - 1, "frame", nullptr, nullptr, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &fp, &fe)) ||
            FAILED(device_->CreateVertexShader(fv->GetBufferPointer(), fv->GetBufferSize(), nullptr, &frameVs_)) ||
            FAILED(device_->CreatePixelShader(fp->GetBufferPointer(), fp->GetBufferSize(), nullptr, &framePs_))) {
            frameVs_.Reset();
            framePs_.Reset();
        }
        D3D11_BUFFER_DESC fb{};
        fb.ByteWidth = sizeof(FrameConstants);
        fb.Usage = D3D11_USAGE_DYNAMIC;
        fb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        fb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&fb, nullptr, &frameCb_))) framePs_.Reset();
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC inv = blend;
    inv.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    inv.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    inv.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    inv.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // mirrored items wind the other way
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_)) || FAILED(device_->CreateSamplerState(&sd, &sampler_)) ||
        FAILED(device_->CreateBlendState(&blend, &alpha_)) || FAILED(device_->CreateBlendState(&inv, &invert_)) ||
        FAILED(device_->CreateRasterizerState(&rd, &raster_))) {
        error = L"The compositor couldn't be set up on the graphics card.";
        return false;
    }
    return true;
}

void Gpu::begin(ID3D11RenderTargetView* rtv, int w, int h) {
    targetW_ = std::max(1, w);
    targetH_ = std::max(1, h);
    D3D11_VIEWPORT vp{0, 0, (float)targetW_, (float)targetH_, 0, 1};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    setBlend(Blend::Alpha);
}

void Gpu::clear(ID3D11RenderTargetView* rtv, float r, float g, float b, float a) {
    const float c[4] = {r, g, b, a};
    context_->ClearRenderTargetView(rtv, c);
}

void Gpu::setBlend(Blend b) {
    const float factor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(b == Blend::Invert ? invert_.Get() : alpha_.Get(), factor, 0xFFFFFFFF);
}

void Gpu::draw(const Constants& c, ID3D11ShaderResourceView* srv) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &c, sizeof c);
    context_->Unmap(cb_.Get(), 0);
    ID3D11ShaderResourceView* views[1] = {srv};
    context_->PSSetShaderResources(0, 1, views);
    context_->Draw(4, 0);
    if (srv) {
        ID3D11ShaderResourceView* none[1] = {nullptr};
        context_->PSSetShaderResources(0, 1, none);
    }
}

namespace {
void toClip(const Pt in[4], float w, float h, float out[4][4]) {
    for (int i = 0; i < 4; ++i) {
        out[i][0] = in[i].x / w * 2 - 1;
        out[i][1] = 1 - in[i].y / h * 2;
        out[i][2] = out[i][3] = 0;
    }
}
}  // namespace

void Gpu::drawTexture(ID3D11ShaderResourceView* srv, const Pt corners[4], const Pt uv[4], float opacity, bool opaque) {
    if (!srv) return;
    Constants c{};
    toClip(corners, (float)targetW_, (float)targetH_, c.pos);
    for (int i = 0; i < 4; ++i) {
        c.uv[i][0] = uv[i].x;
        c.uv[i][1] = uv[i].y;
    }
    c.colour[0] = c.colour[1] = c.colour[2] = 1;
    c.colour[3] = opacity;
    c.flags[0] = 1;
    c.flags[1] = opaque ? 1.0f : 0.0f;
    draw(c, srv);
}

void Gpu::drawFramed(ID3D11ShaderResourceView* srv, const Pt corners[4], const Pt uv[4], float opacity, bool opaque, const border::Style& bIn,
                     float w, float h, const keying::Key& key) {
    if (!srv) return;
    // No border but a key: the same shader with no ring, no corners and no glow.
    border::Style b = bIn;
    if (!b.on) {
        b = border::Style{};
        b.width = 0, b.roundness = 0, b.glow = 0;
    }
    if (!framePs_ || (!bIn.on && !key.on) || w <= 0 || h <= 0) {
        drawTexture(srv, corners, uv, opacity, opaque);
        return;
    }
    auto rgba = [](const sourceutil::Rgba& c, float* out) {
        out[0] = c.r / 255.0f, out[1] = c.g / 255.0f, out[2] = c.b / 255.0f, out[3] = c.a / 255.0f;
    };
    FrameConstants f{};
    f.size[0] = w, f.size[1] = h, f.size[2] = border::radius(b, w, h), f.size[3] = b.width;
    rgba(b.colour, f.c1);
    rgba(b.colour2, f.c2);
    rgba(b.glowColour, f.glowCol);
    f.glowCol[3] *= b.glowOpacity / 100.0f;
    f.g[0] = b.glow, f.g[1] = b.angle * 3.14159265f / 180.0f, f.g[2] = b.gradient ? 1.0f : 0.0f;
    f.o[0] = b.offsetX, f.o[1] = b.offsetY, f.o[2] = opacity, f.o[3] = opaque ? 1.0f : 0.0f;
    if (key.on) {
        const keying::Params kp = keying::params(key);
        f.k1[0] = kp.cb, f.k1[1] = kp.cr, f.k1[2] = kp.sim, f.k1[3] = kp.smooth;
        f.k2[0] = kp.spill, f.k2[1] = 1;
    }
    // A point of the item's own rectangle, in the target: corners[0] + across + down.
    auto at = [&](float u, float v) {
        return Pt{corners[0].x + (corners[1].x - corners[0].x) * (u / w) + (corners[3].x - corners[0].x) * (v / h),
                  corners[0].y + (corners[1].y - corners[0].y) * (u / w) + (corners[3].y - corners[0].y) * (v / h)};
    };
    // The texture's place at a point of the item (extended past its edges the same way).
    auto uvAt = [&](float u, float v) {
        return Pt{uv[0].x + (uv[1].x - uv[0].x) * (u / w) + (uv[3].x - uv[0].x) * (v / h),
                  uv[0].y + (uv[1].y - uv[0].y) * (u / w) + (uv[3].y - uv[0].y) * (v / h)};
    };
    auto pass = [&](float mode, float m) {
        f.g[3] = mode;
        Constants c{};
        const Pt q[4] = {at(-m, -m), at(w + m, -m), at(w + m, h + m), at(-m, h + m)};
        const float lu[4] = {-m, w + m, w + m, -m}, lv[4] = {-m, -m, h + m, h + m};
        toClip(q, (float)targetW_, (float)targetH_, c.pos);
        for (int i = 0; i < 4; ++i) {
            const Pt t = uvAt(lu[i], lv[i]);
            c.uv[i][0] = t.x, c.uv[i][1] = t.y;
            c.uv[i][2] = lu[i], c.uv[i][3] = lv[i];
        }
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return;
        std::memcpy(mp.pData, &c, sizeof c);
        context_->Unmap(cb_.Get(), 0);
        if (FAILED(context_->Map(frameCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return;
        std::memcpy(mp.pData, &f, sizeof f);
        context_->Unmap(frameCb_.Get(), 0);
        ID3D11ShaderResourceView* views[1] = {srv};
        context_->PSSetShaderResources(0, 1, views);
        context_->Draw(4, 0);
    };
    context_->VSSetShader(frameVs_.Get(), nullptr, 0);
    context_->PSSetShader(framePs_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(1, 1, frameCb_.GetAddressOf());
    if (b.glow > 0 && b.glowOpacity > 0) pass(0, border::margin(b));
    pass(1, b.width);
    ID3D11ShaderResourceView* none[1] = {nullptr};
    context_->PSSetShaderResources(0, 1, none);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
}

void Gpu::drawLumaWipe(ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* maskSrv, const Pt corners[4], float progress, float softness,
                       bool invert) {
    if (!srv || !maskSrv) return;
    Constants c{};
    toClip(corners, (float)targetW_, (float)targetH_, c.pos);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        c.uv[i][0] = uv[i][0];
        c.uv[i][1] = uv[i][1];
    }
    c.colour[0] = softness;
    c.colour[1] = invert ? 1.0f : 0.0f;
    c.colour[3] = 1;
    c.flags[0] = 1;
    c.flags[1] = 1;
    c.flags[2] = 1;
    c.flags[3] = progress;
    ID3D11ShaderResourceView* m[1] = {maskSrv};
    context_->PSSetShaderResources(1, 1, m);
    draw(c, srv);
    ID3D11ShaderResourceView* none[1] = {nullptr};
    context_->PSSetShaderResources(1, 1, none);
}

ComPtr<ID3D11ShaderResourceView> Gpu::makeMask(const std::uint8_t* pixels, int w, int h) {
    ComPtr<ID3D11ShaderResourceView> view;
    if (!pixels || w <= 0 || h <= 0) return view;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels, (UINT)w, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(device_->CreateTexture2D(&d, &data, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &view);
    return view;
}

ComPtr<ID3D11PixelShader> Gpu::compilePixelShader(const std::string& source, std::string& error) {
    ComPtr<ID3D11PixelShader> ps;
    CompileFn compile = compiler();
    if (!compile) {
        error = "The shader compiler (d3dcompiler_47.dll) is missing";
        return ps;
    }
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(compile(source.data(), source.size(), "transition", nullptr, nullptr, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
        error = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "It didn't compile";
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r' || error.back() == '\0')) error.pop_back();
        return ps;
    }
    if (FAILED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps))) error = "The graphics card refused the shader";
    return ps;
}

void Gpu::drawWith(ID3D11PixelShader* ps, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, const Pt corners[4], float progress, float w,
                   float h) {
    if (!ps || !a || !b) return;
    Constants c{};
    toClip(corners, (float)targetW_, (float)targetH_, c.pos);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        c.uv[i][0] = uv[i][0];
        c.uv[i][1] = uv[i][1];
    }
    c.colour[0] = w;
    c.colour[1] = h;
    c.flags[3] = progress;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &c, sizeof c);
    context_->Unmap(cb_.Get(), 0);
    ID3D11ShaderResourceView* views[2] = {a, b};
    context_->PSSetShader(ps, nullptr, 0);
    context_->PSSetShaderResources(0, 2, views);
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, none);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
}

void Gpu::drawSolid(const Pt corners[4], float r, float g, float b, float a) {
    Constants c{};
    toClip(corners, (float)targetW_, (float)targetH_, c.pos);
    c.colour[0] = r;
    c.colour[1] = g;
    c.colour[2] = b;
    c.colour[3] = a;
    draw(c, nullptr);
}

void Gpu::fillRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    const Pt p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    drawSolid(p, r, g, b, a);
}

void Gpu::line(Pt a, Pt b, float t, float r, float g, float bl, float alpha) {
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    const float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    const Pt p[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
    drawSolid(p, r, g, bl, alpha);
}

}  // namespace ncap::video
