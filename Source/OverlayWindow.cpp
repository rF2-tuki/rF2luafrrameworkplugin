#include "OverlayWindow.hpp"
#include "DiagLog.hpp"
#include <atomic>
#include <cstring>
#include <thread>

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running(false);
    HANDLE g_stopEvent = nullptr;
    HWND g_overlayHwnd = nullptr;
    const wchar_t* kOverlayClass = L"rF2LuaOverlayWindow";

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        // Belt and braces: the layered+transparent styles already keep
        // this window out of hit testing entirely.
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    struct FindContext {
        DWORD pid;
        HWND best;
        long bestArea;
    };

    // The game's main window is the largest visible, non-minimised
    // window owned by this process (other than the overlay itself).
    // InitScreen() -- which would have told us the window directly --
    // is never delivered in the tested rF2 build, so the window is
    // discovered this way instead. Re-run periodically: the splash /
    // launcher windows are small or short-lived and lose to the real
    // game window as soon as it exists.
    BOOL CALLBACK EnumGameWindow(HWND hwnd, LPARAM lp)
    {
        FindContext* ctx = reinterpret_cast<FindContext*>(lp);
        if (hwnd == g_overlayHwnd) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        RECT client = {};
        if (!GetClientRect(hwnd, &client)) return TRUE;
        const long w = client.right - client.left;
        const long h = client.bottom - client.top;
        if (w < 640 || h < 480) return TRUE;
        const long area = w * h;
        if (area > ctx->bestArea) {
            ctx->bestArea = area;
            ctx->best = hwnd;
        }
        return TRUE;
    }

    HWND FindGameWindow()
    {
        FindContext ctx = { GetCurrentProcessId(), nullptr, 0 };
        EnumWindows(EnumGameWindow, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    }

    bool LinesEqual(const std::vector<DX11OverlayLine>& a, const std::vector<DX11OverlayLine>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const DX11OverlayLine& x = a[i];
            const DX11OverlayLine& y = b[i];
            if (x.x != y.x || x.y != y.y || x.r != y.r || x.g != y.g ||
                x.b != y.b || x.size != y.size || x.text != y.text)
                return false;
        }
        return true;
    }

    void OverlayThreadMain()
    {
        // v6: WIC image decoding (overlay.image) needs COM on the
        // thread that creates the imaging factory -- this one.
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool coInit = SUCCEEDED(coHr);

        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kOverlayClass;
        RegisterClassExW(&wc);

        // v5: WS_EX_LAYERED is the important part. A layered window
        // with WS_EX_TRANSPARENT is excluded from hit testing by the
        // window manager itself, so mouse input always reaches the
        // game window underneath -- regardless of which thread owns
        // which window. (v4's HTTRANSPARENT-only approach only passes
        // hit tests within the same thread, which is why clicks on the
        // game were swallowed.) Content is supplied per-pixel-alpha
        // via UpdateLayeredWindow below.
        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kOverlayClass, L"", WS_POPUP, 0, 0, 64, 64,
            nullptr, nullptr, instance, nullptr);
        if (!hwnd) {
            DiagLog("[rF2Lua] Overlay window: CreateWindowExW failed (error %lu).",
                (unsigned long)GetLastError());
            if (coInit) CoUninitialize();
            return;
        }
        g_overlayHwnd = hwnd;

        // Memory DC + 32bpp top-down DIB section that Direct2D draws
        // into (via its DC render target) and UpdateLayeredWindow
        // presents. Recreated when the game client size changes.
        HDC memDC = CreateCompatibleDC(nullptr);
        HBITMAP dib = nullptr;
        HGDIOBJ stockBmp = nullptr;
        UINT dibW = 0, dibH = 0;
        auto ensureDib = [&](UINT w, UINT h) -> bool {
            if (dib && dibW == w && dibH == h) return true;
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)w;
            bi.bmiHeader.biHeight = -(LONG)h; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP nb = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!nb) return false;
            HGDIOBJ prev = SelectObject(memDC, nb);
            if (!stockBmp) stockBmp = prev; // the DC's original 1x1 bitmap
            if (dib) DeleteObject(dib);
            dib = nb;
            dibW = w;
            dibH = h;
            DiagLog("[rF2Lua] Overlay window: surface resized to %ux%u.", w, h);
            return true;
        };
        if (!memDC || !ensureDib(64, 64)) {
            DiagLog("[rF2Lua] Overlay window: failed to create the drawing surface. "
                    "Overlay rendering is unavailable; the game itself is unaffected.");
            if (memDC) DeleteDC(memDC);
            DestroyWindow(hwnd);
            g_overlayHwnd = nullptr;
            if (coInit) CoUninitialize();
            return;
        }

        DiagLog("[rF2Lua] Overlay window created (layered, click-through). "
                "Waiting for the game window.");

        HWND gameWindow = nullptr;
        ULONGLONG lastFindTick = 0;
        RECT lastPlaced = { -1, -1, -1, -1 };
        bool overlayVisible = false;
        std::vector<DX11OverlayLine> lines;
        std::vector<DX11OverlayLine> prevLines;
        bool havePrev = false;

        while (g_running.load()) {
            // The window lives on this thread, so pump its messages.
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            // (Re)discover the game window once a second, and at once
            // when the tracked one disappears.
            const ULONGLONG now = GetTickCount64();
            if (!gameWindow || !IsWindow(gameWindow) || now - lastFindTick >= 1000) {
                lastFindTick = now;
                HWND found = FindGameWindow();
                if (found != gameWindow) {
                    gameWindow = found;
                    lastPlaced = { -1, -1, -1, -1 };
                    havePrev = false;
                    if (gameWindow) {
                        DiagLog("[rF2Lua] Overlay window: tracking game window %p.",
                            (void*)gameWindow);
                    }
                }
            }

            bool showNow = false;
            UINT clientW = 0, clientH = 0;
            POINT origin = { 0, 0 };
            if (gameWindow && IsWindowVisible(gameWindow) && !IsIconic(gameWindow)) {
                RECT client = {};
                if (GetClientRect(gameWindow, &client) && ClientToScreen(gameWindow, &origin)) {
                    clientW = (UINT)(client.right - client.left);
                    clientH = (UINT)(client.bottom - client.top);
                    // Stay out of the way when the user has switched to
                    // another application.
                    HWND fg = GetForegroundWindow();
                    showNow = clientW >= 64 && clientH >= 64 &&
                        (fg == gameWindow || fg == hwnd || fg == nullptr);
                }
            }

            if (showNow && ensureDib(clientW, clientH)) {
                lines.clear();
                FetchOverlayLines((int)clientW, (int)clientH, lines);
                RECT want = { origin.x, origin.y,
                    origin.x + (LONG)clientW, origin.y + (LONG)clientH };
                const bool moved = memcmp(&want, &lastPlaced, sizeof(RECT)) != 0;
                // Skip the (expensive) full-window present when neither
                // the content nor the placement changed -- e.g. static
                // menu screens. In realtime the values change constantly
                // and this presents at the loop rate (~30 Hz) as before.
                if (moved || !overlayVisible || !havePrev || !LinesEqual(lines, prevLines)) {
                    if (DrawOverlayLinesToDC(memDC, (int)clientW, (int)clientH, lines)) {
                        GdiFlush(); // make sure D2D's GDI writes are complete
                        POINT pos = { want.left, want.top };
                        SIZE size = { (LONG)clientW, (LONG)clientH };
                        POINT src = { 0, 0 };
                        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                        if (!overlayVisible) {
                            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                            overlayVisible = true;
                        }
                        if (UpdateLayeredWindow(hwnd, nullptr, &pos, &size,
                                memDC, &src, 0, &blend, ULW_ALPHA)) {
                            lastPlaced = want;
                            prevLines = lines;
                            havePrev = true;
                        }
                    }
                }
            }
            if (!showNow && overlayVisible) {
                ShowWindow(hwnd, SW_HIDE);
                overlayVisible = false;
                havePrev = false;
            }

            if (WaitForSingleObject(g_stopEvent, 33) == WAIT_OBJECT_0) break;
        }

        if (memDC) {
            if (stockBmp) SelectObject(memDC, stockBmp);
            if (dib) DeleteObject(dib);
            DeleteDC(memDC);
        }
        if (hwnd) { DestroyWindow(hwnd); g_overlayHwnd = nullptr; }
        UnregisterClassW(kOverlayClass, instance);
        if (coInit) CoUninitialize();
        DiagLog("[rF2Lua] Overlay window stopped.");
    }
}

void StartOverlayWindow()
{
    if (g_running.load()) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) return;
    g_running.store(true);
    g_thread = std::thread(OverlayThreadMain);
}

bool StopOverlayWindow()
{
    if (!g_running.load()) return true;
    g_running.store(false);
    if (g_stopEvent) SetEvent(g_stopEvent);
    bool stopped = true;
    if (g_thread.joinable()) {
        // v7: bounded wait. The previous code joined unconditionally;
        // if the thread was stuck inside a render/Lua call during the
        // game's exit sequence, Shutdown blocked forever and the game
        // process never terminated. Three seconds is far beyond a
        // normal stop (one loop iteration is ~33 ms); past that,
        // detach and let the caller skip teardown instead.
        if (WaitForSingleObject(g_thread.native_handle(), 3000) == WAIT_OBJECT_0) {
            g_thread.join();
        } else {
            DiagLog("[rF2Lua] Overlay window: thread did not stop within 3 s; "
                    "detaching so the game's shutdown is not blocked.");
            g_thread.detach();
            stopped = false;
        }
    }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
    return stopped;
}
