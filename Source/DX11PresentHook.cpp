#include "DX11PresentHook.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

extern "C" void __cdecl RenderLuaOverlayDX11(IDXGISwapChain* swapChain);
extern "C" void __cdecl InvalidateLuaOverlayDX11();
extern "C" bool __cdecl IsLuaOverlayTargetSwapChain(IDXGISwapChain* swapChain);

namespace {
    std::mutex g_hookMutex;
    void** g_vtable = nullptr;
    PresentFn g_originalPresent = nullptr;
    ResizeBuffersFn g_originalResize = nullptr;
    bool g_installed = false;
    // v3 diagnostics for the last failed install attempt.
    const char* g_lastStage = "";
    long g_lastError = 0;
    std::atomic<HWND> g_targetWindow(nullptr);
    // Set while a detour is executing overlay code, so Stop can avoid
    // restoring vtable slots underneath an in-flight Present.
    std::atomic<long> g_inFlight(0);

    HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT sync, UINT flags)
    {
        PresentFn original = g_originalPresent;
        if (!original) return DXGI_ERROR_INVALID_CALL;
        // DXGI_PRESENT_TEST is a query, not a frame: never render for it.
        // Render only for the game's main swap chain; all other swap
        // chains in the process (video, editors, helper windows) are
        // passed through untouched.
        if (swapChain && !(flags & DXGI_PRESENT_TEST)) {
            bool isTarget = false;
            __try { isTarget = IsLuaOverlayTargetSwapChain(swapChain); }
            __except (EXCEPTION_EXECUTE_HANDLER) { isTarget = false; }
            if (isTarget) {
                g_inFlight.fetch_add(1);
                // Do not allow exceptions or Lua errors to escape into
                // the game render thread.
                __try { RenderLuaOverlayDX11(swapChain); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                g_inFlight.fetch_sub(1);
            }
        }
        return original(swapChain, sync, flags);
    }

    HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT count,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        ResizeBuffersFn original = g_originalResize;
        if (!original) return DXGI_ERROR_INVALID_CALL;
        // Only the target swap chain has overlay resources. Release every
        // D2D reference to its back buffer *before* the real resize runs,
        // otherwise ResizeBuffers fails and the game loses its device
        // during the startup display-mode change.
        bool isTarget = false;
        __try { isTarget = IsLuaOverlayTargetSwapChain(swapChain); }
        __except (EXCEPTION_EXECUTE_HANDLER) { isTarget = false; }
        if (isTarget) {
            g_inFlight.fetch_add(1);
            __try { InvalidateLuaOverlayDX11(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_inFlight.fetch_sub(1);
        }
        return original(swapChain, count, width, height, format, flags);
    }

    LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // The dummy swap chain exists only to learn the vtable address. It is
    // created lazily at hook-install time (after the game's own device is
    // up), never during plugin Startup().
    bool MakeDummySwapChain(IDXGISwapChain** result, HWND* window, HINSTANCE* instance)
    {
        *result = nullptr; *window = nullptr; *instance = GetModuleHandleW(nullptr);
        const wchar_t* cls = L"rF2LuaDXGIHookDummyWindow";
        WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DummyWndProc;
        wc.hInstance = *instance; wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, cls, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                    nullptr, nullptr, *instance, nullptr);
        if (!hwnd) {
            g_lastStage = "dummy window (CreateWindowExW)";
            g_lastError = (long)GetLastError();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = 64; desc.BufferDesc.Height = 64;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL level = {};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &desc,
            result, &device, &level, &context);
        if (context) context->Release();
        if (device) device->Release();
        if (FAILED(hr)) {
            g_lastStage = "D3D11CreateDeviceAndSwapChain (dummy)";
            g_lastError = (long)hr;
            DestroyWindow(hwnd);
            UnregisterClassW(cls, *instance);
            return false;
        }
        *window = hwnd;
        return true;
    }
}

void SetDX11PresentHookTargetWindow(HWND hwnd)
{
    g_targetWindow.store(hwnd);
}

const char* DX11PresentHookLastStage()
{
    return g_lastStage;
}

long DX11PresentHookLastError()
{
    return g_lastError;
}

bool IsDX11PresentHookInstalled()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    return g_installed;
}

bool StartDX11PresentHook()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_installed) return true;

    IDXGISwapChain* dummy = nullptr;
    HWND hwnd = nullptr; HINSTANCE instance = nullptr;
    if (!MakeDummySwapChain(&dummy, &hwnd, &instance)) return false;

    void** table = *reinterpret_cast<void***>(dummy);
    // IDXGISwapChain vtable: Present=8, ResizeBuffers=13.
    //
    // The vtable is shared by every swap chain in the process, so both
    // slots are replaced under a *single* VirtualProtect covering slots
    // 8..13, instead of the original two separate writes that left a
    // window where Present was hooked but ResizeBuffers was not (and
    // vice versa on rollback after a partial failure).
    bool ok = false;
    void* originalPresent = table[8];
    void* originalResize = table[13];
    DWORD oldProtect = 0;
    if (VirtualProtect(&table[8], sizeof(void*) * 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        table[8] = reinterpret_cast<void*>(&HookPresent);
        table[13] = reinterpret_cast<void*>(&HookResizeBuffers);
        FlushInstructionCache(GetCurrentProcess(), &table[8], sizeof(void*) * 6);
        DWORD ignored = 0;
        VirtualProtect(&table[8], sizeof(void*) * 6, oldProtect, &ignored);
        ok = true;
    } else {
        g_lastStage = "VirtualProtect (vtable patch)";
        g_lastError = (long)GetLastError();
    }

    if (ok) {
        g_vtable = table;
        g_originalPresent = reinterpret_cast<PresentFn>(originalPresent);
        g_originalResize = reinterpret_cast<ResizeBuffersFn>(originalResize);
        g_installed = true;
    }

    dummy->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"rF2LuaDXGIHookDummyWindow", instance);
    return ok;
}

void StopDX11PresentHook()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (!g_installed || !g_vtable) return;
    // Best-effort wait for an in-flight detour to finish before restoring
    // the slots (bounded, so shutdown can never hang on this).
    for (int i = 0; i < 100 && g_inFlight.load() > 0; ++i) Sleep(1);
    DWORD oldProtect = 0;
    if (VirtualProtect(&g_vtable[8], sizeof(void*) * 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // Restore only if our detours are still installed. If another
        // overlay hooked after us, overwriting its entries with our
        // saved originals would break it; in that case leave the slots
        // (our detours forward to the saved originals and the plugin
        // instance check in Plugin.cpp makes them inert after shutdown).
        if (g_vtable[8] == reinterpret_cast<void*>(&HookPresent))
            g_vtable[8] = reinterpret_cast<void*>(g_originalPresent);
        if (g_vtable[13] == reinterpret_cast<void*>(&HookResizeBuffers))
            g_vtable[13] = reinterpret_cast<void*>(g_originalResize);
        FlushInstructionCache(GetCurrentProcess(), &g_vtable[8], sizeof(void*) * 6);
        DWORD ignored = 0;
        VirtualProtect(&g_vtable[8], sizeof(void*) * 6, oldProtect, &ignored);
    }
    g_vtable = nullptr; g_originalPresent = nullptr; g_originalResize = nullptr; g_installed = false;
}
