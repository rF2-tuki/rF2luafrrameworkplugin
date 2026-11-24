#pragma once
#include <windows.h>
#include <vector>
#include "DX11Overlay.hpp"

// External overlay window (implementation in OverlayWindow.cpp).
//
// Why this exists: the v3 test on 2026-11-15 proved that the in-game
// Present hook itself works (installed, target chain found, 7,648
// detour frames), but Direct2D initialisation on the *game's* D3D11
// device always fails with E_INVALIDARG (0x80070057) because that
// device was created without D3D11_CREATE_DEVICE_BGRA_SUPPORT, which
// Direct2D strictly requires. No change to the hook can fix that.
//
// So rendering lives in a separate topmost window placed exactly over
// the game's client area. The game is only observed (its window
// position/size), never hooked, in this mode.
//
// v5 presentation: the window is a *layered* window
// (WS_EX_LAYERED | WS_EX_TRANSPARENT) and frames are presented with
// UpdateLayeredWindow after drawing into a memory DC via Direct2D's
// DC render target. v4 used DirectComposition and relied on
// WM_NCHITTEST -> HTTRANSPARENT for click-through, but HTTRANSPARENT
// only passes the hit test to windows in the SAME thread -- the game
// window lives on the game's main thread, so clicks died on the
// overlay. With WS_EX_LAYERED, the window manager itself excludes the
// window from hit testing, in any thread, so clicks always reach the
// game.
void StartOverlayWindow();
// v7: returns false if the thread did not stop within the timeout and
// had to be detached. In that case the caller must NOT tear down the
// renderer or the Lua engine -- the detached thread may still be
// inside them; process exit reclaims everything instead. Either way,
// this never blocks the game's shutdown for long.
bool StopOverlayWindow();

// Implemented in Plugin.cpp, which owns the Lua engine, the overlay
// renderer and the diagnostic counters:
// - FetchOverlayLines runs the Lua onRender callback for the given
//   client size and returns the lines to draw.
// - DrawOverlayLinesToDC draws them into the given memory DC (cleared
//   to fully transparent first) via DX11Overlay::RenderToDC.
bool FetchOverlayLines(int width, int height, std::vector<DX11OverlayLine>& out);
bool DrawOverlayLinesToDC(HDC hdc, int width, int height, const std::vector<DX11OverlayLine>& lines);
