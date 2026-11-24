#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>

struct DX11OverlayLine {
    int x, y, r, g, b, size;
    std::string text;
    // v6: image items (overlay.image). When isImage is true, text/r/g/b/
    // size are unused; path is the resolved absolute image path, w/h are
    // the requested draw size (0 = natural size / keep aspect), alpha is
    // 0-255 opacity.
    bool isImage = false;
    std::string path;
    int w = 0, h = 0, alpha = 255;
};

// Rendering backend. Call Render() from the game's DXGI Present hook, on the
// same thread that owns the swap-chain immediate context.
//
// Fixed design (see FIX_NOTES_ja.md):
//  * The D2D target bitmap is created from the *current* back buffer on
//    every frame and released before Render() returns. No reference to a
//    back buffer is ever held across frames, so ResizeBuffers can never
//    fail because of this overlay, and a stale bitmap from another swap
//    chain (or from before a resize) is never drawn into.
//  * Resource creation is all-or-nothing. If any step fails (for example
//    the game device was created without D3D11_CREATE_DEVICE_BGRA_SUPPORT,
//    which Direct2D requires), the failure is latched instead of
//    dereferencing a null D2D context on the next frame, and the reason
//    is available through LastError(). (v2) The latch is not permanent:
//    creation is retried when the device changes or after a cooldown of
//    kRetryFrames frames, so a transient failure during startup no
//    longer suppresses the overlay for the whole session.
//  * (v2) The game's D3D11 immediate-context state (render targets,
//    viewports, blend/depth/rasterizer state, input assembler, VS/PS
//    shaders, buffers, samplers and shader resources) is saved before
//    Direct2D draws and restored afterwards, so the overlay cannot
//    corrupt the game's next frame.
class DX11Overlay {
public:
    DX11Overlay();
    ~DX11Overlay();
    // clearTransparent (v4 overlay-window mode): the target is a
    // persistent composition surface, not a game back buffer that is
    // redrawn every frame, so it is cleared to fully transparent before
    // drawing. Never set this for the in-game path: the target there is
    // the game's own back buffer and clearing would erase the frame.
    bool Render(IDXGISwapChain* swapChain, const std::vector<DX11OverlayLine>& lines,
                bool clearTransparent = false);
    // v5 overlay-window path: draw the lines into a caller-owned memory
    // DC (a 32bpp DIB section selected into it) through a Direct2D DC
    // render target. The caller presents the result with
    // UpdateLayeredWindow. Needs no D3D11 device at all, so the game's
    // BGRA-less device is irrelevant here too. The surface is cleared
    // to fully transparent before drawing.
    bool RenderToDC(HDC hdc, int width, int height, const std::vector<DX11OverlayLine>& lines);
    // Call before the original ResizeBuffers runs. With per-frame targets
    // there is normally nothing held, but this also unbinds and flushes
    // the D2D context defensively.
    void Invalidate();
    // Release every D2D/DWrite/WIC object immediately (called from
    // Shutdown). Invalidate() alone leaves the factories and cached
    // objects alive until the static destructor runs at DLL unload,
    // and releasing them under the loader lock during
    // DLL_PROCESS_DETACH made the game process hang on exit.
    void ReleaseAll();
    HRESULT LastError() const { return lastError_; }
    bool IsDisabled() const { return disabled_; }
    // v3: number of times D2D initialisation has failed. The plugin logs
    // a failure only when this counter changes, so a failure that happens
    // before the Lua log is available can no longer swallow the only
    // diagnostic, and the retry itself stays log-free.
    unsigned long long FailCount() const { return failCount_; }
    // v3: frames until the next retry is allowed (grows with backoff).
    unsigned long long RetryInterval() const { return retryInterval_; }
private:
    bool EnsureD2D(IDXGISwapChain* swapChain);
    // Creates the D2D / DirectWrite factories (shared by the DXGI path
    // and the DC path) if they do not exist yet.
    bool EnsureFactories();
    // The per-line text drawing loop, shared by Render() and
    // RenderToDC(). The target must already be inside BeginDraw.
    void DrawLines(ID2D1RenderTarget* target, const std::vector<DX11OverlayLine>& lines, int maxWidth);
    // v6 image support: images are decoded once with WIC into a cached
    // IWICBitmapSource; the per-target ID2D1Bitmap is created from it on
    // demand (D2D bitmaps are bound to the render target that created
    // them, so the bitmap cache is dropped when the target changes).
    ID2D1Bitmap* GetImageBitmap(ID2D1RenderTarget* target, const std::string& path);
    IWICBitmapSource* LoadImageSource(const std::string& path);
    void DrawImageItem(ID2D1RenderTarget* target, const DX11OverlayLine& line);
    void ReleaseD2D();
    void Fail(HRESULT hr);
    ID3D11Device* device_;
    ID2D1Factory1* d2dFactory_;
    ID2D1Device* d2dDevice_;
    ID2D1DeviceContext* d2dContext_;
    ID2D1DCRenderTarget* dcTarget_;
    IDWriteFactory* writeFactory_;
    IWICImagingFactory* wicFactory_;
    std::map<std::string, IWICBitmapSource*> imageSources_;
    std::map<std::string, ID2D1Bitmap*> imageBitmaps_;
    ID2D1RenderTarget* imageTarget_;
    std::map<int, IDWriteTextFormat*> formats_;
    HRESULT lastError_;
    bool disabled_;
    // v2 retry bookkeeping for the failure latch.
    unsigned long long frameCounter_;
    unsigned long long failFrame_;
    const void* failedDeviceKey_;
    static const unsigned long long kRetryFrames = 300;
    // v3: fail count + exponential backoff of the retry interval
    // (300 -> 900 -> 1800 frames, capped). Rebuilding the D2D factory /
    // device is a heavy operation; doing it every 300 frames forever
    // (v2 behaviour when the device can never support D2D, e.g. no BGRA)
    // showed up as a periodic stutter every few seconds.
    unsigned long long failCount_;
    unsigned long long retryInterval_;
    static const unsigned long long kRetryFramesMax = 1800;
};
