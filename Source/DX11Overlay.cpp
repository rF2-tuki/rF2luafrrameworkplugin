#include "DX11Overlay.hpp"
#include <d2d1.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {
    // Convert a Lua string (UTF-8 by convention in this framework) to wide
    // characters. The original code reused the UTF-8 length estimate for
    // the ANSI fallback, which could truncate or overflow the conversion;
    // the fallback size is now computed with the code page actually used.
    bool ToWide(const std::string& text, std::vector<wchar_t>& out)
    {
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.c_str(), -1, nullptr, 0);
        UINT cp = CP_UTF8;
        if (needed <= 0) {
            cp = CP_ACP;
            needed = MultiByteToWideChar(cp, 0, text.c_str(), -1, nullptr, 0);
            if (needed <= 0) return false;
        }
        out.assign((size_t)needed, L'\0');
        if (!MultiByteToWideChar(cp, 0, text.c_str(), -1, out.data(), needed)) {
            // If the UTF-8 size query above succeeded but conversion failed,
            // retry once as ANSI with its own size query.
            if (cp == CP_UTF8) {
                needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
                if (needed <= 0) return false;
                out.assign((size_t)needed, L'\0');
                return MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, out.data(), needed) > 0;
            }
            return false;
        }
        return true;
    }
    int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    // v2: snapshot of the D3D11 immediate-context state that Direct2D
    // drawing can disturb. Saved before BeginDraw and restored after
    // EndDraw, then released. Geometry/compute shader state and shader
    // class instances are not saved: Direct2D does not bind a geometry
    // shader, and this overlay never relies on the game's dynamic shader
    // linkage. All getters AddRef; Restore() re-binds and ReleaseSaved()
    // drops those references.
    struct D3D11StateBackup {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11DepthStencilView* dsv;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT viewportCount;
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT scissorCount;
        ID3D11RasterizerState* rsState;
        ID3D11BlendState* blendState;
        FLOAT blendFactor[4];
        UINT sampleMask;
        ID3D11DepthStencilState* dsState;
        UINT stencilRef;
        ID3D11InputLayout* inputLayout;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11Buffer* vertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
        UINT vbStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
        UINT vbOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
        ID3D11Buffer* indexBuffer;
        DXGI_FORMAT indexFormat;
        UINT indexOffset;
        ID3D11VertexShader* vs;
        ID3D11Buffer* vsCBs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
        ID3D11SamplerState* vsSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
        ID3D11PixelShader* ps;
        ID3D11Buffer* psCBs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
        ID3D11ShaderResourceView* psSRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
        ID3D11SamplerState* psSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
        D3D11StateBackup() : dsv(nullptr), viewportCount(0), scissorCount(0),
            rsState(nullptr), blendState(nullptr), sampleMask(0), dsState(nullptr),
            stencilRef(0), inputLayout(nullptr), topology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
            indexBuffer(nullptr), indexFormat(DXGI_FORMAT_UNKNOWN), indexOffset(0),
            vs(nullptr), ps(nullptr) {
            memset(rtvs, 0, sizeof(rtvs));
            memset(blendFactor, 0, sizeof(blendFactor));
            memset(vertexBuffers, 0, sizeof(vertexBuffers));
            memset(vbStrides, 0, sizeof(vbStrides));
            memset(vbOffsets, 0, sizeof(vbOffsets));
            memset(vsCBs, 0, sizeof(vsCBs));
            memset(vsSamplers, 0, sizeof(vsSamplers));
            memset(psCBs, 0, sizeof(psCBs));
            memset(psSRVs, 0, sizeof(psSRVs));
            memset(psSamplers, 0, sizeof(psSamplers));
        }
    };
    template <typename T> void ReleaseArray(T** arr, size_t n) {
        for (size_t i = 0; i < n; ++i) if (arr[i]) { arr[i]->Release(); arr[i] = nullptr; }
    }
    void SaveState(ID3D11DeviceContext* ctx, D3D11StateBackup& b)
    {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtvs, &b.dsv);
        b.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&b.viewportCount, b.viewports);
        b.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&b.scissorCount, b.scissors);
        ctx->RSGetState(&b.rsState);
        ctx->OMGetBlendState(&b.blendState, b.blendFactor, &b.sampleMask);
        ctx->OMGetDepthStencilState(&b.dsState, &b.stencilRef);
        ctx->IAGetInputLayout(&b.inputLayout);
        ctx->IAGetPrimitiveTopology(&b.topology);
        ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
            b.vertexBuffers, b.vbStrides, b.vbOffsets);
        ctx->IAGetIndexBuffer(&b.indexBuffer, &b.indexFormat, &b.indexOffset);
        ctx->VSGetShader(&b.vs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b.vsCBs);
        ctx->VSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b.vsSamplers);
        ctx->PSGetShader(&b.ps, nullptr, nullptr);
        ctx->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b.psCBs);
        ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b.psSRVs);
        ctx->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b.psSamplers);
    }
    void RestoreState(ID3D11DeviceContext* ctx, D3D11StateBackup& b)
    {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtvs, b.dsv);
        ctx->RSSetViewports(b.viewportCount, b.viewports);
        ctx->RSSetScissorRects(b.scissorCount, b.scissors);
        ctx->RSSetState(b.rsState);
        ctx->OMSetBlendState(b.blendState, b.blendFactor, b.sampleMask);
        ctx->OMSetDepthStencilState(b.dsState, b.stencilRef);
        ctx->IASetInputLayout(b.inputLayout);
        ctx->IASetPrimitiveTopology(b.topology);
        ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
            b.vertexBuffers, b.vbStrides, b.vbOffsets);
        ctx->IASetIndexBuffer(b.indexBuffer, b.indexFormat, b.indexOffset);
        ctx->VSSetShader(b.vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b.vsCBs);
        ctx->VSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b.vsSamplers);
        ctx->PSSetShader(b.ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b.psCBs);
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b.psSRVs);
        ctx->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b.psSamplers);
        // Drop the references the getters added.
        ReleaseArray(b.rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
        if (b.dsv) { b.dsv->Release(); b.dsv = nullptr; }
        if (b.rsState) { b.rsState->Release(); b.rsState = nullptr; }
        if (b.blendState) { b.blendState->Release(); b.blendState = nullptr; }
        if (b.dsState) { b.dsState->Release(); b.dsState = nullptr; }
        if (b.inputLayout) { b.inputLayout->Release(); b.inputLayout = nullptr; }
        ReleaseArray(b.vertexBuffers, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
        if (b.indexBuffer) { b.indexBuffer->Release(); b.indexBuffer = nullptr; }
        if (b.vs) { b.vs->Release(); b.vs = nullptr; }
        ReleaseArray(b.vsCBs, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
        ReleaseArray(b.vsSamplers, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
        if (b.ps) { b.ps->Release(); b.ps = nullptr; }
        ReleaseArray(b.psCBs, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
        ReleaseArray(b.psSRVs, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        ReleaseArray(b.psSamplers, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
}

DX11Overlay::DX11Overlay()
    : device_(nullptr), d2dFactory_(nullptr), d2dDevice_(nullptr),
      d2dContext_(nullptr), dcTarget_(nullptr), writeFactory_(nullptr),
      wicFactory_(nullptr), imageTarget_(nullptr),
      lastError_(S_OK), disabled_(false),
      frameCounter_(0), failFrame_(0), failedDeviceKey_(nullptr),
      failCount_(0), retryInterval_(kRetryFrames) {}

DX11Overlay::~DX11Overlay() { ReleaseD2D(); }

void DX11Overlay::ReleaseD2D()
{
    for (std::map<int, IDWriteTextFormat*>::iterator it = formats_.begin(); it != formats_.end(); ++it)
        if (it->second) it->second->Release();
    formats_.clear();
    for (std::map<std::string, ID2D1Bitmap*>::iterator it = imageBitmaps_.begin(); it != imageBitmaps_.end(); ++it)
        if (it->second) it->second->Release();
    imageBitmaps_.clear();
    imageTarget_ = nullptr;
    for (std::map<std::string, IWICBitmapSource*>::iterator it = imageSources_.begin(); it != imageSources_.end(); ++it)
        if (it->second) it->second->Release();
    imageSources_.clear();
    if (wicFactory_) { wicFactory_->Release(); wicFactory_ = nullptr; }
    if (dcTarget_) { dcTarget_->Release(); dcTarget_ = nullptr; }
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);
        d2dContext_->Release();
        d2dContext_ = nullptr;
    }
    if (d2dDevice_) { d2dDevice_->Release(); d2dDevice_ = nullptr; }
    if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }
    if (writeFactory_) { writeFactory_->Release(); writeFactory_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

void DX11Overlay::Fail(HRESULT hr)
{
    // Latch the failure: never leave a half-initialised object behind that
    // a later frame would dereference, and never retry every frame.
    // (v2) The latch expires: EnsureD2D retries after kRetryFrames or
    // when the device changes, so a transient startup failure does not
    // suppress the overlay for the whole session.
    lastError_ = hr;
    disabled_ = true;
    failFrame_ = frameCounter_;
    failedDeviceKey_ = device_;
    // v3: count the failure (the plugin logs when this changes) and back
    // the retry interval off so a permanently unsupported device does
    // not cause a heavy re-initialisation spike every 300 frames.
    ++failCount_;
    if (failCount_ == 2) retryInterval_ = 900;
    else if (failCount_ >= 3) retryInterval_ = kRetryFramesMax;
    ReleaseD2D();
}

void DX11Overlay::Invalidate()
{
    // Per-frame targets mean nothing is held between frames. Still unbind
    // and flush defensively so no D2D reference to the back buffer can
    // survive into the game's ResizeBuffers call.
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);
        d2dContext_->Flush();
    }
}

void DX11Overlay::ReleaseAll()
{
    ReleaseD2D();
}

bool DX11Overlay::EnsureFactories()
{
    // The factory may be created on the plugin thread and used on the
    // render thread, so it must be multi-threaded (the original used a
    // single-threaded factory).
    if (!d2dFactory_) {
        D2D1_FACTORY_OPTIONS opts = {};
        opts.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
            __uuidof(ID2D1Factory1), &opts,
            reinterpret_cast<void**>(&d2dFactory_));
        if (FAILED(hr)) { Fail(hr); return false; }
    }
    if (!writeFactory_) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&writeFactory_));
        if (FAILED(hr)) { Fail(hr); return false; }
    }
    return true;
}

bool DX11Overlay::EnsureD2D(IDXGISwapChain* swapChain)
{
    if (!swapChain) return false;

    ComPtr<ID3D11Device> incoming;
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device),
        reinterpret_cast<void**>(incoming.GetAddressOf()));
    if (FAILED(hr)) { Fail(hr); return false; }

    if (disabled_) {
        // v2 latch expiry: retry on a new device, or after the cooldown.
        // v3: the cooldown is retryInterval_, which backs off after
        // repeated failures (see Fail()).
        const bool newDevice = failedDeviceKey_ &&
            incoming.Get() != static_cast<const ID3D11Device*>(failedDeviceKey_);
        const bool cooledDown = (frameCounter_ - failFrame_) >= retryInterval_;
        if (!newDevice && !cooledDown) return false;
        disabled_ = false;
        lastError_ = S_OK;
    }

    if (d2dContext_) {
        if (device_ == incoming.Get()) return true;
        // Device changed under us (reset/recreate): rebuild from scratch.
        ReleaseD2D();
    }

    device_ = incoming.Get();
    device_->AddRef();

    if (!EnsureFactories()) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) { Fail(hr); return false; }

    // NOTE: this fails when the game's D3D11 device was not created with
    // D3D11_CREATE_DEVICE_BGRA_SUPPORT. That is a hard Direct2D requirement;
    // failing here disables only the overlay, never the game.
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(hr)) { Fail(hr); return false; }

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(hr)) { Fail(hr); return false; }

    // v3: success resets the retry backoff (failCount_ stays cumulative
    // so the plugin's change detection keeps working).
    retryInterval_ = kRetryFrames;
    return true;
}

bool DX11Overlay::Render(IDXGISwapChain* swapChain, const std::vector<DX11OverlayLine>& lines,
                         bool clearTransparent)
{
    ++frameCounter_;
    if (!swapChain) return false;
    // In-game mode keeps the old behaviour (nothing to draw = do not
    // even touch D2D). Overlay-window mode must still run so an empty
    // line set clears the previous frame off the persistent surface.
    if (lines.empty() && !clearTransparent) return false;
    if (!EnsureD2D(swapChain)) return false;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc))) return false;

    // v2: save the game's immediate-context state before Direct2D draws
    // (restored after EndDraw below), so the overlay cannot corrupt the
    // game's own rendering state for the following frames.
    ComPtr<ID3D11DeviceContext> gameContext;
    device_->GetImmediateContext(gameContext.GetAddressOf());
    D3D11StateBackup backup;
    const bool haveBackup = (gameContext.Get() != nullptr);
    if (haveBackup) SaveState(gameContext.Get(), backup);

    // Per-frame target: build a D2D bitmap from *this* swap chain's
    // *current* back buffer, draw, then unbind and release it before
    // returning. Nothing references the back buffer between frames.
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())))) {
        if (haveBackup) RestoreState(gameContext.Get(), backup);
        return false;
    }
    ID2D1Bitmap1* bitmap = nullptr;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap))) {
        if (haveBackup) RestoreState(gameContext.Get(), backup);
        return false;
    }

    d2dContext_->SetTarget(bitmap);
    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (clearTransparent) {
        // v4 overlay-window mode: erase the previous frame (the
        // composition surface persists between Presents).
        d2dContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    }
    DrawLines(d2dContext_, lines, (int)desc.BufferDesc.Width);
    HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    bitmap->Release();
    // Restore the game's context state no matter how drawing went.
    if (haveBackup) RestoreState(gameContext.Get(), backup);

    if (hr == D2DERR_RECREATE_TARGET) {
        // Device/target lost: reset once. EnsureD2D will rebuild on the
        // next frame; this is not latched as a permanent failure.
        ReleaseD2D();
        return false;
    }
    if (FAILED(hr)) { Fail(hr); return false; }
    return true;
}

void DX11Overlay::DrawLines(ID2D1RenderTarget* target, const std::vector<DX11OverlayLine>& lines, int maxWidth)
{
    for (size_t i = 0; i < lines.size(); ++i) {
        const DX11OverlayLine& line = lines[i];
        if (line.isImage) { DrawImageItem(target, line); continue; }
        const int size = line.size > 0 ? line.size : 18;
        IDWriteTextFormat* format = nullptr;
        std::map<int, IDWriteTextFormat*>::iterator fit = formats_.find(size);
        if (fit != formats_.end()) {
            format = fit->second;
        } else {
            if (SUCCEEDED(writeFactory_->CreateTextFormat(L"Arial", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, (FLOAT)size, L"ja-JP", &format))) {
                formats_[size] = format;
            } else {
                continue;
            }
        }
        D2D1_COLOR_F color = D2D1::ColorF(
            (float)Clamp255(line.r) / 255.0f,
            (float)Clamp255(line.g) / 255.0f,
            (float)Clamp255(line.b) / 255.0f, 1.0f);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) continue;
        std::vector<wchar_t> wide;
        if (!ToWide(line.text, wide) || wide.empty()) continue;
        D2D1_RECT_F rect = D2D1::RectF((FLOAT)line.x, (FLOAT)line.y,
            (FLOAT)maxWidth, (FLOAT)(line.y + size * 2));
        target->DrawText(wide.data(), (UINT32)(wide.size() - 1),
            format, rect, brush.Get());
    }
}

bool DX11Overlay::RenderToDC(HDC hdc, int width, int height, const std::vector<DX11OverlayLine>& lines)
{
    ++frameCounter_;
    if (!hdc || width <= 0 || height <= 0) return false;
    if (!EnsureFactories()) return false;

    if (!dcTarget_) {
        // Premultiplied BGRA to match the 32bpp DIB the caller selects
        // into the memory DC (and UpdateLayeredWindow's AC_SRC_ALPHA).
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        HRESULT hr = d2dFactory_->CreateDCRenderTarget(&props, &dcTarget_);
        if (FAILED(hr)) { Fail(hr); return false; }
    }

    RECT rect = { 0, 0, width, height };
    HRESULT hr = dcTarget_->BindDC(hdc, &rect);
    if (FAILED(hr)) {
        // The DC went away or changed incompatibly: drop the target so
        // the next frame recreates it, without latching a failure.
        dcTarget_->Release();
        dcTarget_ = nullptr;
        return false;
    }

    dcTarget_->BeginDraw();
    dcTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    dcTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    DrawLines(dcTarget_, lines, width);
    hr = dcTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        dcTarget_->Release();
        dcTarget_ = nullptr;
        return false;
    }
    if (FAILED(hr)) { Fail(hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v6: image items (overlay.image). Decoded once via WIC; see the header
// for the caching scheme. Any failure simply skips the image -- a bad
// path must never take the text overlay (or the game) down with it.
// ---------------------------------------------------------------------------

IWICBitmapSource* DX11Overlay::LoadImageSource(const std::string& path)
{
    if (!wicFactory_) {
        // Requires COM on this thread; the overlay thread initialises
        // it (CoInitializeEx). If creation fails, images are skipped.
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_))))
            return nullptr;
    }
    std::vector<wchar_t> wide;
    if (!ToWide(path, wide) || wide.empty()) return nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(wide.data(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    HRESULT hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) return nullptr;
    IWICFormatConverter* converter = nullptr;
    hr = wicFactory_->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    frame->Release();
    if (FAILED(hr)) {
        if (converter) converter->Release();
        return nullptr;
    }
    return converter;
}

ID2D1Bitmap* DX11Overlay::GetImageBitmap(ID2D1RenderTarget* target, const std::string& path)
{
    if (imageTarget_ != target) {
        for (std::map<std::string, ID2D1Bitmap*>::iterator it = imageBitmaps_.begin(); it != imageBitmaps_.end(); ++it)
            if (it->second) it->second->Release();
        imageBitmaps_.clear();
        imageTarget_ = target;
    }
    std::map<std::string, ID2D1Bitmap*>::iterator bit = imageBitmaps_.find(path);
    if (bit != imageBitmaps_.end()) return bit->second;
    IWICBitmapSource* source = nullptr;
    std::map<std::string, IWICBitmapSource*>::iterator sit = imageSources_.find(path);
    if (sit != imageSources_.end()) {
        source = sit->second;
    } else {
        source = LoadImageSource(path);
        imageSources_[path] = source; // nullptr is cached too: no per-frame retries
    }
    ID2D1Bitmap* bitmap = nullptr;
    if (source) {
        if (FAILED(target->CreateBitmapFromWicBitmap(source, nullptr, &bitmap)))
            bitmap = nullptr;
    }
    imageBitmaps_[path] = bitmap;
    return bitmap;
}

void DX11Overlay::DrawImageItem(ID2D1RenderTarget* target, const DX11OverlayLine& line)
{
    ID2D1Bitmap* bitmap = GetImageBitmap(target, line.path);
    if (!bitmap) return;
    const D2D1_SIZE_F natural = bitmap->GetSize();
    float w = line.w > 0 ? (float)line.w : natural.width;
    float h = line.h > 0 ? (float)line.h : natural.height;
    if (line.w > 0 && line.h <= 0 && natural.width > 0.0f)
        h = natural.height * (w / natural.width);
    if (line.h > 0 && line.w <= 0 && natural.height > 0.0f)
        w = natural.width * (h / natural.height);
    if (w <= 0.0f || h <= 0.0f) return;
    const D2D1_RECT_F dest = D2D1::RectF((FLOAT)line.x, (FLOAT)line.y,
        (FLOAT)line.x + w, (FLOAT)line.y + h);
    target->DrawBitmap(bitmap, &dest, (FLOAT)line.alpha / 255.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}
