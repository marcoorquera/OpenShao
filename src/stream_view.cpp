#include "stream_view.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace openshao::stream {

using Microsoft::WRL::ComPtr;

namespace {

// One triangle covers the viewport; the viewport places the picture or the ping box.
constexpr char kShaders[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
Texture2D<float4> overlay : register(t2);
SamplerState linear_sampler : register(s0);
float4 ps_video(VSOut i) : SV_Target {
    // BT.709 limited range, the same coefficients as Nv12ToBgra.
    float y = 1.164383 * (luma.Sample(linear_sampler, i.uv) - 0.062745);
    float2 c = chroma.Sample(linear_sampler, i.uv) - 0.501961;
    return float4(saturate(y + 1.792741 * c.y), saturate(y - 0.213249 * c.x - 0.532909 * c.y),
                  saturate(y + 2.112402 * c.x), 1);
}
float4 ps_overlay(VSOut i) : SV_Target {
    return float4(overlay.Sample(linear_sampler, i.uv).rgb, 1);
}
)";

std::string HrText(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof text, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

} // namespace

struct ViewRenderer::State {
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> target;
    UINT swap_flags = 0;
    bool tearing = false;
    HANDLE waitable = nullptr;
    int buffer_w = 0, buffer_h = 0;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_video, ps_overlay;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> luma, chroma;
    ComPtr<ID3D11ShaderResourceView> luma_view, chroma_view;
    uint32_t tex_w = 0, tex_h = 0;
    bool has_frame = false;
    float aspect = 4.f / 3.f;

    // The ping box is drawn with GDI into a bitmap and uploaded when the value or size changes.
    ComPtr<ID3D11ShaderResourceView> overlay_view;
    int overlay_w = 0, overlay_h = 0, overlay_ping = -1, overlay_font = 0;
    HFONT font = nullptr;

    ~State() {
        if (font) DeleteObject(font);
        if (waitable) CloseHandle(waitable);
    }

    bool CreateTarget() {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        return SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &target));
    }

    bool Compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& blob, std::string& error) {
        ComPtr<ID3DBlob> messages;
        const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "stream_view", nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &messages);
        if (SUCCEEDED(hr)) return true;
        error = HrText("Viewer shader compilation", hr);
        if (messages) error += std::string(": ") + static_cast<const char*>(messages->GetBufferPointer());
        return false;
    }

    bool Upload(const uint8_t* nv12, uint32_t w, uint32_t h) {
        if (!luma || tex_w != w || tex_h != h) {
            luma.Reset(); chroma.Reset(); luma_view.Reset(); chroma_view.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &luma))) return false;
            td.Width = w / 2;
            td.Height = h / 2;
            td.Format = DXGI_FORMAT_R8G8_UNORM;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &chroma))) return false;
            if (FAILED(device->CreateShaderResourceView(luma.Get(), nullptr, &luma_view)) ||
                FAILED(device->CreateShaderResourceView(chroma.Get(), nullptr, &chroma_view)))
                return false;
            tex_w = w;
            tex_h = h;
        }
        auto copy = [&](ID3D11Texture2D* texture, const uint8_t* src, uint32_t rows) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            auto* dst = static_cast<uint8_t*>(mapped.pData);
            for (uint32_t row = 0; row < rows; ++row)
                std::memcpy(dst + size_t(row) * mapped.RowPitch, src + size_t(row) * w, w);
            context->Unmap(texture, 0);
            return true;
        };
        // Both planes have rows of w bytes: luma one byte per pixel, chroma two bytes per pixel pair.
        return copy(luma.Get(), nv12, h) && copy(chroma.Get(), nv12 + size_t(w) * h, h / 2);
    }

    bool UpdateOverlay(int ping_ms, int font_height) {
        if (overlay_view && ping_ms == overlay_ping && font_height == overlay_font) return true;
        if (!font || font_height != overlay_font) {
            if (font) DeleteObject(font);
            font = CreateFontW(-font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        }
        wchar_t text[32];
        std::swprintf(text, 32, L"%d ms", ping_ms);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        HGDIOBJ old_font = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text, int(std::wcslen(text)), &size);
        const int pad = font_height / 3;
        const int w = size.cx + pad * 2, h = size.cy + pad;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        if (bitmap && bits) {
            HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
            RECT box{0, 0, w, h};
            HBRUSH back = CreateSolidBrush(RGB(16, 16, 16));
            FillRect(dc, &box, back);
            DeleteObject(back);
            // Green, yellow and red follow the usual thresholds of online games.
            SetTextColor(dc, ping_ms < 60 ? RGB(110, 220, 120) : ping_ms < 120 ? RGB(240, 200, 80) : RGB(240, 90, 80));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GdiFlush();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(w);
            td.Height = UINT(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{bits, UINT(w) * 4, 0};
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> view;
            if (SUCCEEDED(device->CreateTexture2D(&td, &init, &texture)) &&
                SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
                overlay_view = view;
                overlay_w = w;
                overlay_h = h;
                overlay_ping = ping_ms;
                overlay_font = font_height;
                ok = true;
            }
            SelectObject(dc, old_bitmap);
        }
        if (bitmap) DeleteObject(bitmap);
        SelectObject(dc, old_font);
        DeleteDC(dc);
        return ok;
    }
};

ViewRenderer::ViewRenderer() : s_(std::make_unique<State>()) {}
ViewRenderer::~ViewRenderer() = default;

bool ViewRenderer::Open(HWND__* window, std::string& error) {
    auto& s = *s_;
    s.window = window;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   UINT(std::size(levels)), D3D11_SDK_VERSION, &s.device, nullptr, &s.context);
    if (FAILED(hr)) { error = HrText("Direct3D 11 device creation", hr); return false; }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(s.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        error = "DXGI 1.2 is unavailable";
        return false;
    }
    ComPtr<IDXGIFactory5> factory5;
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing))))
        s.tearing = allow_tearing != FALSE;

    RECT rc{};
    GetClientRect(window, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(std::max<LONG>(1, rc.right - rc.left));
    sd.Height = UINT(std::max<LONG>(1, rc.bottom - rc.top));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // A waitable swap chain with one frame of latency shows each frame as soon as it is decoded,
    // without vsync queueing; tearing allows presenting between refreshes.
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
               (s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    hr = factory->CreateSwapChainForHwnd(s.device.Get(), window, &sd, nullptr, nullptr, &s.swap);
    if (FAILED(hr)) {
        s.tearing = false;
        sd.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(s.device.Get(), window, &sd, nullptr, nullptr, &s.swap);
    }
    if (FAILED(hr)) { error = HrText("Swap chain creation", hr); return false; }
    s.swap_flags = sd.Flags;
    // The viewer toggles its own borderless full screen on Alt+Enter.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> swap2;
    if ((sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && SUCCEEDED(s.swap.As(&swap2))) {
        swap2->SetMaximumFrameLatency(1);
        s.waitable = swap2->GetFrameLatencyWaitableObject();
    } else {
        ComPtr<IDXGIDevice1> device1;
        if (SUCCEEDED(s.device.As(&device1))) device1->SetMaximumFrameLatency(1);
    }
    s.buffer_w = int(sd.Width);
    s.buffer_h = int(sd.Height);
    if (!s.CreateTarget()) { error = "Cannot create the viewer render target"; return false; }

    ComPtr<ID3DBlob> vs, ps_video, ps_overlay;
    if (!s.Compile("vs_main", "vs_4_0", vs, error) || !s.Compile("ps_video", "ps_4_0", ps_video, error) ||
        !s.Compile("ps_overlay", "ps_4_0", ps_overlay, error))
        return false;
    if (FAILED(s.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s.vs)) ||
        FAILED(s.device->CreatePixelShader(ps_video->GetBufferPointer(), ps_video->GetBufferSize(), nullptr, &s.ps_video)) ||
        FAILED(s.device->CreatePixelShader(ps_overlay->GetBufferPointer(), ps_overlay->GetBufferSize(), nullptr, &s.ps_overlay))) {
        error = "Cannot create the viewer shaders";
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(s.device->CreateSamplerState(&sampler, &s.sampler))) { error = "Cannot create the viewer sampler"; return false; }
    return true;
}

bool ViewRenderer::Draw(const uint8_t* nv12, uint32_t width, uint32_t height, float aspect, int ping_ms) {
    auto& s = *s_;
    if (!s.swap) return false;
    if (s.waitable) WaitForSingleObjectEx(s.waitable, 100, TRUE);
    if (nv12 && width >= 2 && height >= 2) {
        if (!s.Upload(nv12, width & ~1u, height & ~1u)) return false;
        s.has_frame = true;
        s.aspect = aspect > 0 ? aspect : 4.f / 3.f;
    }
    RECT rc{};
    GetClientRect(s.window, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return true;  // minimized
    if (cw != s.buffer_w || ch != s.buffer_h) {
        s.target.Reset();
        s.context->ClearState();
        if (FAILED(s.swap->ResizeBuffers(0, UINT(cw), UINT(ch), DXGI_FORMAT_UNKNOWN, s.swap_flags)) || !s.CreateTarget())
            return false;
        s.buffer_w = cw;
        s.buffer_h = ch;
    }
    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* target = s.target.Get();
    s.context->OMSetRenderTargets(1, &target, nullptr);
    s.context->ClearRenderTargetView(target, black);
    s.context->IASetInputLayout(nullptr);
    s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.context->VSSetShader(s.vs.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = s.sampler.Get();
    s.context->PSSetSamplers(0, 1, &sampler);
    if (s.has_frame) {
        // Letterbox the picture in the shape the host shows it.
        float w = float(cw), h = float(cw) / s.aspect;
        if (h > float(ch)) { h = float(ch); w = float(ch) * s.aspect; }
        const D3D11_VIEWPORT viewport{(float(cw) - w) * .5f, (float(ch) - h) * .5f, w, h, 0, 1};
        s.context->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView* planes[2] = {s.luma_view.Get(), s.chroma_view.Get()};
        s.context->PSSetShaderResources(0, 2, planes);
        s.context->PSSetShader(s.ps_video.Get(), nullptr, 0);
        s.context->Draw(3, 0);
    }
    if (ping_ms >= 0 && s.UpdateOverlay(ping_ms, std::max(14, ch / 30))) {
        const int margin = s.overlay_font / 2;
        const D3D11_VIEWPORT viewport{float(std::max(0, cw - margin - s.overlay_w)), float(margin), float(s.overlay_w),
                                      float(s.overlay_h), 0, 1};
        s.context->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView* overlay = s.overlay_view.Get();
        s.context->PSSetShaderResources(2, 1, &overlay);
        s.context->PSSetShader(s.ps_overlay.Get(), nullptr, 0);
        s.context->Draw(3, 0);
    }
    const HRESULT hr = s.swap->Present(0, s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    return SUCCEEDED(hr) || hr == DXGI_STATUS_OCCLUDED;
}

} // namespace openshao::stream

#endif
