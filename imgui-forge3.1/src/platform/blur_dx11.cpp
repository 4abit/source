#include "platform/blur_dx11.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <d3dcompiler.h>

namespace nl::blur_dx11 {
namespace {

// Each level halves the resolution; the result is upsampled back into the first level.
constexpr int k_levels = 4;
constexpr float k_spread = 1.0f;

constexpr const char* k_shader = R"(
cbuffer Params : register(b0) { float2 texel; float spread; float unused; };
Texture2D src : register(t0);
SamplerState smp : register(s0);

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p;
    return o;
}

float4 ps_down(VsOut i) : SV_TARGET
{
    float2 o = texel * spread;
    float3 c = src.Sample(smp, i.uv).rgb * 4.0;
    c += src.Sample(smp, i.uv + float2(-o.x, -o.y)).rgb;
    c += src.Sample(smp, i.uv + float2( o.x, -o.y)).rgb;
    c += src.Sample(smp, i.uv + float2(-o.x,  o.y)).rgb;
    c += src.Sample(smp, i.uv + float2( o.x,  o.y)).rgb;
    return float4(c / 8.0, 1.0);
}

float4 ps_up(VsOut i) : SV_TARGET
{
    float2 o = texel * spread;
    float3 c = src.Sample(smp, i.uv + float2(-o.x * 2.0, 0.0)).rgb;
    c += src.Sample(smp, i.uv + float2(-o.x,  o.y)).rgb * 2.0;
    c += src.Sample(smp, i.uv + float2( 0.0,  o.y * 2.0)).rgb;
    c += src.Sample(smp, i.uv + float2( o.x,  o.y)).rgb * 2.0;
    c += src.Sample(smp, i.uv + float2( o.x * 2.0, 0.0)).rgb;
    c += src.Sample(smp, i.uv + float2( o.x, -o.y)).rgb * 2.0;
    c += src.Sample(smp, i.uv + float2( 0.0, -o.y * 2.0)).rgb;
    c += src.Sample(smp, i.uv + float2(-o.x, -o.y)).rgb * 2.0;
    return float4(c / 12.0, 1.0);
}
)";

template <class T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;

    void reset()
    {
        release(srv);
        release(rtv);
        release(tex);
        w = h = 0;
    }
};

struct State {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps_down = nullptr;
    ID3D11PixelShader* ps_up = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    // Copy of the render target at the time of the capture.
    ID3D11Texture2D* copy = nullptr;
    ID3D11ShaderResourceView* copy_srv = nullptr;
    D3D11_TEXTURE2D_DESC copy_desc{};
    Target levels[k_levels];
    int width = 0, height = 0;
};
State g;

ID3DBlob* compile(const char* entry, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(k_shader, std::strlen(k_shader), "blur", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    release(errors);
    return SUCCEEDED(hr) ? code : nullptr;
}

bool make_target(Target& t, int w, int h)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &t.tex)) ||
        FAILED(g.device->CreateRenderTargetView(t.tex, nullptr, &t.rtv)) ||
        FAILED(g.device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
        t.reset();
        return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

bool ensure_copy(const D3D11_TEXTURE2D_DESC& src)
{
    if (g.copy && g.copy_desc.Width == src.Width && g.copy_desc.Height == src.Height &&
        g.copy_desc.Format == src.Format)
        return true;
    release(g.copy_srv);
    release(g.copy);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.copy)) ||
        FAILED(g.device->CreateShaderResourceView(g.copy, nullptr, &g.copy_srv))) {
        release(g.copy_srv);
        release(g.copy);
        return false;
    }
    g.copy_desc = d;
    return true;
}

void pass(ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, int src_w, int src_h, const Target& dst)
{
    ID3D11ShaderResourceView* none = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &none);
    g.ctx->OMSetRenderTargets(1, &dst.rtv, nullptr);
    const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(dst.w), static_cast<float>(dst.h), 0.0f, 1.0f };
    g.ctx->RSSetViewports(1, &vp);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g.ctx->Map(g.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const float values[4] = { 1.0f / static_cast<float>(src_w), 1.0f / static_cast<float>(src_h), k_spread, 0.0f };
        std::memcpy(mapped.pData, values, sizeof(values));
        g.ctx->Unmap(g.params, 0);
    }
    g.ctx->PSSetShader(ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, &src);
    g.ctx->Draw(3, 0);
}

void capture(const ImDrawList*, const ImDrawCmd*)
{
    if (!g.levels[0].srv)
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    g.ctx->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) {
        release(dsv);
        return;
    }
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    ID3D11Texture2D* frame = nullptr;
    if (resource)
        resource->QueryInterface(IID_PPV_ARGS(&frame));
    release(resource);

    D3D11_TEXTURE2D_DESC desc{};
    if (frame)
        frame->GetDesc(&desc);
    if (frame && desc.SampleDesc.Count == 1 && ensure_copy(desc)) {
        g.ctx->CopyResource(g.copy, frame);

        g.ctx->IASetInputLayout(nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.vs, nullptr, 0);
        g.ctx->GSSetShader(nullptr, nullptr, 0);
        g.ctx->PSSetSamplers(0, 1, &g.sampler);
        g.ctx->PSSetConstantBuffers(0, 1, &g.params);
        const float factor[4] = {};
        g.ctx->OMSetBlendState(g.blend, factor, 0xffffffff);
        g.ctx->OMSetDepthStencilState(g.depth, 0);
        g.ctx->RSSetState(g.raster);

        pass(g.ps_down, g.copy_srv, static_cast<int>(desc.Width), static_cast<int>(desc.Height), g.levels[0]);
        for (int i = 1; i < k_levels; ++i)
            pass(g.ps_down, g.levels[i - 1].srv, g.levels[i - 1].w, g.levels[i - 1].h, g.levels[i]);
        for (int i = k_levels - 2; i >= 0; --i)
            pass(g.ps_up, g.levels[i + 1].srv, g.levels[i + 1].w, g.levels[i + 1].h, g.levels[i]);

        ID3D11ShaderResourceView* none = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &none);
    }
    g.ctx->OMSetRenderTargets(1, &rtv, dsv);
    release(frame);
    release(rtv);
    release(dsv);
}

void destroy_targets()
{
    for (Target& t : g.levels)
        t.reset();
    release(g.copy_srv);
    release(g.copy);
    g.width = g.height = 0;
}

} // namespace

bool init(ID3D11Device* device, ID3D11DeviceContext* context)
{
    g.device = device;
    g.ctx = context;

    ID3DBlob* vs = compile("vs_main", "vs_4_0");
    ID3DBlob* down = compile("ps_down", "ps_4_0");
    ID3DBlob* up = compile("ps_up", "ps_4_0");
    bool ok = vs && down && up;
    ok = ok && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs));
    ok = ok && SUCCEEDED(device->CreatePixelShader(down->GetBufferPointer(), down->GetBufferSize(), nullptr, &g.ps_down));
    ok = ok && SUCCEEDED(device->CreatePixelShader(up->GetBufferPointer(), up->GetBufferSize(), nullptr, &g.ps_up));
    release(vs);
    release(down);
    release(up);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sd, &g.sampler));

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g.params));

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &g.blend));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &g.raster));

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &g.depth));

    if (!ok)
        shutdown();
    return ok;
}

void shutdown()
{
    destroy_targets();
    release(g.depth);
    release(g.raster);
    release(g.blend);
    release(g.params);
    release(g.sampler);
    release(g.ps_up);
    release(g.ps_down);
    release(g.vs);
    g.device = nullptr;
    g.ctx = nullptr;
}

blur::Backend prepare(int width, int height)
{
    if (!g.vs || width <= 0 || height <= 0)
        return {};
    if (width != g.width || height != g.height) {
        destroy_targets();
        int w = width, h = height;
        for (Target& t : g.levels) {
            w = std::max(1, (w + 1) / 2);
            h = std::max(1, (h + 1) / 2);
            if (!make_target(t, w, h)) {
                destroy_targets();
                return {};
            }
        }
        g.width = width;
        g.height = height;
    }
    return { &capture, static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(g.levels[0].srv)) };
}

} // namespace nl::blur_dx11
