#pragma once
#include <windows.h>

// DXGI Present / ResizeBuffers hook (hardened, see FIX_NOTES_ja.md).
//
// Do NOT call StartDX11PresentHook() from the plugin's Startup(): at that
// point rFactor 2 is still creating its own graphics device (splash ->
// black screen phase), and creating a dummy D3D11 device and patching the
// shared DXGI vtable there is what crashed the game on startup. Call it
// from InitScreen() instead, which the SDK invokes right after the game's
// graphics device has been initialised. It is idempotent.
//
// SetDX11PresentHookTargetWindow() records the game's main window
// (ScreenInfoV01::mAppWindow). The Present detour only renders the Lua
// overlay for the swap chain that belongs to that window; every other
// swap chain in the process is passed straight through.
bool StartDX11PresentHook();
void StopDX11PresentHook();
bool IsDX11PresentHookInstalled();
void SetDX11PresentHookTargetWindow(HWND hwnd);
// v3 diagnostics: when StartDX11PresentHook() fails, these describe the
// stage that failed ("dummy window", "D3D11CreateDeviceAndSwapChain",
// "VirtualProtect") and its HRESULT / Win32 error, so the failure can be
// logged to lua_plugin.log instead of vanishing into OutputDebugString.
const char* DX11PresentHookLastStage();
long DX11PresentHookLastError();
