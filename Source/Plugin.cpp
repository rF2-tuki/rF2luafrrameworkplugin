#include "InternalsPlugin.hpp"
#include "LuaEngine.hpp"
#include <windows.h>
#include <string>
#include <cstring>
#include "DX11Overlay.hpp"
#include "DX11PresentHook.hpp"
#include "DiagLog.hpp"
#include "OverlayWindow.hpp"
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdarg>

static std::string GetPluginDirectory()
{
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetPluginDirectory), &module);
    char path[MAX_PATH] = {0};
    if (!module || !GetModuleFileNameA(module, path, MAX_PATH)) return ".";
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    if (slash != std::string::npos) result.resize(slash);
    return result;
}

// Dedicated build (2026-11-22): true when the host process is the
// rFactor 2 dedicated server ("rFactor2 Dedicated.exe"). The client
// and the dedicated server share one Bin64\Plugins folder, so the
// same DLL is loaded by both; the host exe name is the only reliable
// way to tell them apart. GetModuleFileNameA with a nullptr module
// returns the host process path (same Win32 call GetPluginDirectory
// already uses for this DLL's own module).
static bool IsDedicatedHostProcess()
{
    char path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return false;
    std::string host(path);
    const size_t slash = host.find_last_of("\\/");
    const std::string exe = (slash == std::string::npos) ? host : host.substr(slash + 1);
    return exe.find("Dedicated") != std::string::npos;
}

// v3 diagnostics: a logger that does NOT depend on the Lua engine.
// LuaEngine::Log writes to a path that is only set when Start() runs
// inside Startup(), so in v1/v2 every diagnostic emitted by a callback
// delivered before Startup (or by a different plugin instance than the
// one that received Startup) was silently lost. The 2026-11-15 test log
// proved this matters: it contained the Startup/session lines but no
// hook-install, target-swap-chain or D2D-failure lines at all, even
// though the game ran a full session. DiagLog derives the log path from
// the DLL's own module path, works from any callback/thread, and writes
// to the same lua_plugin.log. It is only called on events (first frame,
// state changes, failures), never per frame. (Declared in DiagLog.hpp;
// also used by OverlayWindow.cpp.)
void DiagLog(const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    static std::mutex diagMutex;
    std::lock_guard<std::mutex> lock(diagMutex);
    const std::string luaDir = GetPluginDirectory() + "\\Lua";
    CreateDirectoryA(luaDir.c_str(), nullptr);
    const std::string path = luaDir + "\\lua_plugin.log";
    HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, (DWORD)std::strlen(message), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

class LuaFrameworkPlugin;
// v3: this now points to the instance that received Startup() (set
// there, cleared in Shutdown()), NOT merely the last constructed
// object. If the game creates a probe instance, the Present bridge must
// still render through the instance whose Lua engine actually started.
static LuaFrameworkPlugin* g_pluginInstance = nullptr;
static DX11Overlay g_dx11Overlay;
static std::mutex g_dx11RenderMutex;
// Master switch for the Present detour bridge. Cleared first during
// Shutdown() so an in-flight or late Present can never touch the Lua
// engine or the overlay after teardown has begun.
static std::atomic<bool> g_renderEnabled(false);
// v8: dedicated shutdown gate for the overlay-window bridges. This
// must NOT be g_renderEnabled: that flag means "the in-game Present
// hook is live" and is only ever set in TryInstallHook(), so in the
// default overlay-window mode it stays false for the whole session.
// v7 gated FetchOverlayLines/DrawOverlayLinesToDC on it, which
// silently disabled all overlay rendering in overlay mode.
static std::atomic<bool> g_shutdownBegun(false);
// The game's main window (ScreenInfoV01::mAppWindow) and the swap chain
// that belongs to it. Only that swap chain is ever rendered to.
static std::atomic<HWND> g_appWindow(nullptr);
// v2: atomic, because the detours read/write this on the render thread
// while Shutdown/UninitScreen write it on the game thread (was a data
// race in v1, even if an aligned pointer write is benign in practice).
static std::atomic<IDXGISwapChain*> g_targetSwapChain(nullptr);
static bool g_loggedHookDisabled = false;
// v3 diagnostic counters/latches (atomic: written from both the game
// thread and the render thread). All latches are reset in Startup(), so
// a latch consumed before the log existed cannot hide a failure again.
static std::atomic<unsigned long long> g_presentRenders(0);
static std::atomic<unsigned long long> g_lastSeenD2DFailCount(0);
static std::atomic<bool> g_loggedFirstDraw(false);
static std::atomic<bool> g_loggedEmptyLines(false);
static std::atomic<IDXGISwapChain*> g_lastLoggedTarget(nullptr);
static std::atomic<unsigned long long> g_renderScreenCalls(0);

class LuaFrameworkPlugin : public InternalsPluginV07
{
public:
    LuaFrameworkPlugin() {
        // v3: do NOT claim g_pluginInstance here (see the comment at its
        // declaration); Startup() claims it. Creation/destruction are
        // logged so the next log shows how many instances the game made
        // and in what order callbacks arrived.
        DiagLog("[rF2Lua] Plugin object created: %p (v10+dedicated)", (void*)this);
    }
    virtual ~LuaFrameworkPlugin() {
        DiagLog("[rF2Lua] Plugin object destroyed: %p", (void*)this);
        if (g_pluginInstance == this) g_pluginInstance = nullptr;
    }

    void Startup(long version) override {
        (void)version;
        DiagLog("[rF2Lua] Startup called: instance=%p (v10+dedicated)", (void*)this);
        g_pluginInstance = this;
        // v3: reset every diagnostic latch/counter that a pre-Startup
        // callback may already have consumed, and allow hook installation
        // to be attempted again from post-Startup callbacks.
        g_lastSeenD2DFailCount.store(0);
        g_shutdownBegun.store(false);
        g_loggedFirstDraw.store(false);
        g_loggedEmptyLines.store(false);
        g_lastLoggedTarget.store(nullptr);
        g_loggedHookDisabled = false;
        hookAttempts_ = 0;
        lastHookAttemptTick_ = 0;
        pluginDirectory_ = GetPluginDirectory();
        engine_.Start(pluginDirectory_);
        // v4 rendering mode selection. The 2026-11-15 v3 test proved
        // the in-game Present hook works (installed from StartSession,
        // target chain found, 7,648 detour frames) but Direct2D can
        // never initialise on the game's device: it fails with
        // E_INVALIDARG (0x80070057) because the device was created
        // without BGRA support. Rendering therefore defaults to the
        // external overlay window (own BGRA device, no game hook).
        // The legacy in-game hook remains as an opt-in for devices
        // that do support BGRA: create Lua\dx11_hook_enabled.txt.
        hookOptIn_ = GetFileAttributesA(
            (pluginDirectory_ + "\\Lua\\dx11_hook_enabled.txt").c_str())
            != INVALID_FILE_ATTRIBUTES;
        // Dedicated build (2026-11-22): on the dedicated server there
        // is no screen to draw on, so the overlay window is never
        // created and the DX11 hook is never installed (hookOptIn_ is
        // forced false, and TryInstallHook() already returns at once
        // when it is false, so no later callback can install it
        // either). All Lua data callbacks, including scoring, work
        // exactly as on the client. Shutdown is unaffected:
        // StopOverlayWindow() returns true when the window was never
        // started and StopDX11PresentHook() returns at once when the
        // hook was never installed, i.e. this reuses the teardown
        // path the client already takes in hook mode.
        dedicatedMode_ = IsDedicatedHostProcess();
        if (dedicatedMode_) {
            hookOptIn_ = false;
            DiagLog("[rF2Lua] v10+dedicated: dedicated server host detected; no-overlay mode (overlay window and DX11 hook disabled, Lua/scoring active).");
            engine_.LogLine("[rF2Lua] Startup: dedicated no-overlay mode.");
        } else if (hookOptIn_) {
            DiagLog("[rF2Lua] v10: legacy in-game DX11 hook mode (dx11_hook_enabled.txt present); overlay window is disabled.");
            engine_.LogLine("[rF2Lua] Startup: legacy DX11 hook mode (opt-in).");
        } else {
            DiagLog("[rF2Lua] v10: overlay-window rendering mode (in-game DX11 hook is not installed).");
            engine_.LogLine("[rF2Lua] Startup: overlay-window rendering mode (v10).");
            StartOverlayWindow();
        }
    }

    void Shutdown() override {
        DiagLog("[rF2Lua] Shutdown: renderFrames=%llu telemetryCalls=%llu forwarded=%llu skipped=%llu d2dFailures=%llu",
            g_presentRenders.load(), telemetryCalls_, telemetryForwarded_,
            telemetrySkipped_, g_dx11Overlay.FailCount());
        // Order matters: stop new rendering first (overlay thread,
        // then the Present hook), then wait for any in-flight render
        // by taking the render mutex, then release overlay resources,
        // and only then close Lua. The overlay thread calls into Lua,
        // so it must be joined before engine_.Stop().
        //
        // v7: StopOverlayWindow() waits with a timeout. If the thread
        // had to be detached, it may still be inside the renderer or
        // Lua, so teardown is skipped entirely -- freeing resources
        // under a live thread would trade the shutdown hang for a
        // crash. Process exit reclaims everything in that rare case.
        g_renderEnabled.store(false);
        g_shutdownBegun.store(true);
        const bool overlayStopped = StopOverlayWindow();
        StopDX11PresentHook();
        if (overlayStopped) {
            {
                std::lock_guard<std::mutex> lock(g_dx11RenderMutex);
                g_dx11Overlay.Invalidate();
                // Release ALL overlay COM objects now, on the game
                // thread. Leaving them for the static destructor at DLL
                // unload made the game process hang on exit: releasing
                // D2D/DWrite/WIC objects under the loader lock during
                // DLL_PROCESS_DETACH blocked. After this, ~DX11Overlay
                // during unload is a no-op.
                g_dx11Overlay.ReleaseAll();
                g_targetSwapChain.store(nullptr);
            }
            engine_.Stop();
        } else {
            g_pluginInstance = nullptr;
            DiagLog("[rF2Lua] Shutdown: overlay thread was detached; renderer/Lua "
                    "teardown skipped so the game can finish exiting.");
        }
    }

    void InitScreen(const ScreenInfoV01& info) override {
        // v3: always logged (DiagLog works even before Startup), with
        // the screen details, so the next log proves whether and when
        // this callback is delivered.
        DiagLog("[rF2Lua] InitScreen: instance=%p window=%p size=%ldx%ld location=%d page=%.31s",
            (void*)this, (void*)info.mAppWindow, info.mWidth, info.mHeight,
            (int)info.mOptionsLocation, info.mOptionsPage);
        if (info.mAppWindow) {
            g_appWindow.store(info.mAppWindow);
            SetDX11PresentHookTargetWindow(info.mAppWindow);
        }
        {
            std::lock_guard<std::mutex> lock(g_dx11RenderMutex);
            g_targetSwapChain.store(nullptr);
            g_lastLoggedTarget.store(nullptr);
        }
        TryInstallHook("InitScreen");
    }

    void UninitScreen(const ScreenInfoV01& info) override {
        (void)info;
        DiagLog("[rF2Lua] UninitScreen: instance=%p", (void*)this);
        std::lock_guard<std::mutex> lock(g_dx11RenderMutex);
        g_dx11Overlay.Invalidate();
        g_targetSwapChain.store(nullptr);
    }

    void RenderScreenBeforeOverlays(const ScreenInfoV01& info) override {
        NoteRenderScreen(info, "Before");
    }

    void RenderScreenAfterOverlays(const ScreenInfoV01& info) override {
        NoteRenderScreen(info, "After");
    }

    void PreReset(const ScreenInfoV01& info) override {
        (void)info;
        DiagLog("[rF2Lua] PreReset: instance=%p", (void*)this);
    }

    void PostReset(const ScreenInfoV01& info) override {
        (void)info;
        DiagLog("[rF2Lua] PostReset: instance=%p", (void*)this);
        TryInstallHook("PostReset");
    }

    void LogLine(const std::string& message) { engine_.LogLine(message); }
    std::vector<DX11OverlayLine> GetEngineOverlayLines() { return engine_.GetOverlayLines(); }
    void RenderLuaFrame(int width, int height) { engine_.RenderOverlayFrame(width, height); }
    const std::string& PluginDirectory() const { return pluginDirectory_; }

    // v6: game-flow notifications that were never forwarded to Lua.
    void Load() override { engine_.Call("onLoad"); }
    void Unload() override { engine_.Call("onUnload"); }
    void ThreadStarted(long type) override { engine_.CallLong("onThreadStarted", type); }
    void ThreadStopping(long type) override { engine_.CallLong("onThreadStopping", type); }
    void DeactivateScreen(const ScreenInfoV01& info) override { engine_.CallScreen("onDeactivateScreen", info); }
    void ReactivateScreen(const ScreenInfoV01& info) override { engine_.CallScreen("onReactivateScreen", info); }

    // v6: remaining SDK access channels (custom controls and the
    // screen activation callbacks are V05, thread notifications are
    // V06, pit menu / track rules / multi-session rules / custom
    // variables are V07). The Wants* gates check whether the Lua side
    // actually defines the callback, so nothing changes for scripts
    // that do not use them.
    bool InitCustomControl(CustomControlInfoV01& info) override { return engine_.InitCustomControl(info); }
    bool WantsPitMenuAccess() override { return engine_.HasFunction("onPitMenu"); }
    bool AccessPitMenu(PitMenuV01& info) override { return engine_.ApplyPitMenu(info); }
    bool WantsTrackRulesAccess() override { return engine_.HasFunction("onTrackRules"); }
    bool AccessTrackRules(TrackRulesV01& info) override { return engine_.ApplyTrackRules(info); }
    bool WantsMultiSessionRulesAccess() override { return engine_.HasFunction("onMultiSessionRules"); }
    bool AccessMultiSessionRules(MultiSessionRulesV01& info) override { return engine_.ApplyMultiSessionRules(info); }
    bool GetCustomVariable(long i, CustomVariableV01& var) override { return engine_.GetCustomVariable(i, var); }
    void GetCustomVariableSetting(CustomVariableV01& var, long i, CustomSettingV01& setting) override { engine_.GetCustomVariableSetting(var, i, setting); }
    void AccessCustomVariable(CustomVariableV01& var) override { engine_.AccessCustomVariable(var); }

    void StartSession() override {
        engine_.Call("onStartSession");
        // v3: session callbacks are guaranteed to arrive after Startup
        // with the graphics device fully up, so they are a reliable
        // hook-install point even if the screen callbacks were delivered
        // earlier (or to another instance) and their attempts were lost.
        TryInstallHook("StartSession");
    }
    void EndSession() override { engine_.Call("onEndSession"); }
    void EnterRealtime() override {
        engine_.Call("onEnterRealtime");
        TryInstallHook("EnterRealtime");
        DiagLog("[rF2Lua] EnterRealtime: hookInstalled=%d",
            IsDX11PresentHookInstalled() ? 1 : 0);
    }
    void ExitRealtime() override {
        engine_.Call("onExitRealtime");
        // v3: per-session summary, so the counters survive even if the
        // game is closed abnormally afterwards.
        DiagLog("[rF2Lua] ExitRealtime: renderFrames=%llu telemetryCalls=%llu forwarded=%llu skipped=%llu d2dFailures=%llu",
            g_presentRenders.load(), telemetryCalls_, telemetryForwarded_,
            telemetrySkipped_, g_dx11Overlay.FailCount());
    }
    bool WantsScoringUpdates() override { return true; }
    void UpdateScoring(const ScoringInfoV01& info) override { engine_.CallScoring(info); }
    long WantsTelemetryUpdates() override { return 1; }
    void UpdateTelemetry(const TelemInfoV01& info) override {
        // v3 (stutter): CallTelemetry builds a ~150-field Lua table with
        // nested wheel/vector tables on every call. At the game's
        // telemetry rate that churns Lua garbage on the simulation
        // thread and showed up as intermittent stuttering. Overlay
        // scripts only keep the latest table, so forwarding is capped
        // at 100 Hz; the counters above show how much was skipped.
        ++telemetryCalls_;
        if (!ShouldForward(telemetryLastQpc_, 100)) { ++telemetrySkipped_; return; }
        ++telemetryForwarded_;
        engine_.CallTelemetry(info);
    }
    bool WantsGraphicsUpdates() override { return true; }
    void UpdateGraphics(const GraphicsInfoV02& info) override {
        // v3 (stutter): same reasoning as telemetry; 30 Hz is plenty
        // for the camera/graphics values the overlay displays.
        if (!ShouldForward(graphicsLastQpc_, 30)) return;
        engine_.CallGraphics(info);
    }
    void SetEnvironment(const EnvironmentInfoV01& info) override { engine_.CallEnvironment(info); }

    // Writable SDK callbacks. Lua returns a table (or a number for scalar callbacks).
    bool ForceFeedback(double& forceValue) override {
        double result = forceValue;
        if (!engine_.CallNumber("onForceFeedback", forceValue, result)) return false;
        forceValue = result;
        return true;
    }
    bool CheckHWControl(const char* controlName, double& value) override {
        double result = value;
        if (!engine_.CallHWControl(controlName, value, result)) return false;
        value = result;
        return true;
    }
    void SetPhysicsOptions(PhysicsOptionsV01& options) override {
        engine_.ApplyPhysicsOptions(options);
    }
    unsigned char WantsToViewVehicle(CameraControlInfoV01& info) override {
        return engine_.ApplyCamera(info);
    }
    bool WantsToDisplayMessage(MessageInfoV01& info) override {
        return engine_.ApplyMessage(info);
    }
    bool RequestCommentary(CommentaryRequestInfoV01& info) override {
        return engine_.ApplyCommentary(info);
    }
    bool WantsWeatherAccess() override { return true; }
    bool AccessWeather(double trackNodeSize, WeatherControlInfoV01& info) override {
        return engine_.ApplyWeather(trackNodeSize, info);
    }
private:
    // Shared handling for RenderScreenBefore/AfterOverlays: count calls,
    // log the first one, remember the app window, and use the callback
    // as a fallback hook-install point.
    void NoteRenderScreen(const ScreenInfoV01& info, const char* which) {
        const unsigned long long call = g_renderScreenCalls.fetch_add(1) + 1;
        if (call == 1) {
            DiagLog("[rF2Lua] RenderScreen%sOverlays first call: instance=%p window=%p size=%ldx%ld location=%d",
                which, (void*)this, (void*)info.mAppWindow, info.mWidth,
                info.mHeight, (int)info.mOptionsLocation);
        }
        if (!IsDX11PresentHookInstalled()) {
            if (info.mAppWindow && !g_appWindow.load()) {
                g_appWindow.store(info.mAppWindow);
                SetDX11PresentHookTargetWindow(info.mAppWindow);
            }
            TryInstallHook("RenderScreen");
        }
    }

    // Rate limiter for Lua forwarding (see UpdateTelemetry).
    static bool ShouldForward(long long& lastQpc, int hz) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (lastQpc == 0) { lastQpc = now.QuadPart; return true; }
        if (now.QuadPart - lastQpc >= freq.QuadPart / hz) {
            lastQpc = now.QuadPart;
            return true;
        }
        return false;
    }

    void TryInstallHook(const char* source) {
        if (IsDX11PresentHookInstalled()) { g_renderEnabled.store(true); return; }
        // v4: the in-game hook is opt-in only (see Startup). In the
        // default overlay-window mode this returns immediately.
        if (!hookOptIn_) return;
        // Manual kill switch: creating Lua\dx11_hook_disabled.txt next
        // to the plugin disables only the DX11 hook (Lua data callbacks
        // keep working), which is the safe rollback for this feature.
        // v3: the path is derived from the DLL module, not from the
        // instance member, so the check also works before Startup().
        const std::string flag = GetPluginDirectory() + "\\Lua\\dx11_hook_disabled.txt";
        if (GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!g_loggedHookDisabled) {
                g_loggedHookDisabled = true;
                DiagLog("[rF2Lua] DX11 hook disabled by Lua\\dx11_hook_disabled.txt");
            }
            return;
        }
        // v3: attempts are throttled to one per 2 seconds instead of a
        // small lifetime cap. In v1/v2 the cap of 3 could be burned by
        // callbacks delivered before the graphics device was ready (and
        // their failure logs were lost), after which installation was
        // never retried for the rest of the process.
        const ULONGLONG now = GetTickCount64();
        if (lastHookAttemptTick_ != 0 && now - lastHookAttemptTick_ < 2000) return;
        lastHookAttemptTick_ = now;
        ++hookAttempts_;
        if (StartDX11PresentHook()) {
            g_renderEnabled.store(true);
            DiagLog("[rF2Lua] DXGI Present/ResizeBuffers hook installed (from %s, attempt %d).",
                source, hookAttempts_);
        } else {
            DiagLog("[rF2Lua] DXGI hook installation failed (from %s, attempt %d): stage=%s error=0x%08lX. Will retry from a later callback.",
                source, hookAttempts_, DX11PresentHookLastStage(),
                (unsigned long)DX11PresentHookLastError());
        }
    }
    LuaEngine engine_;
    std::string pluginDirectory_;
    bool hookOptIn_ = false;
    // Dedicated build: dedicatedMode_ means only "the host process is
    // the dedicated server" (set once in Startup from the host exe
    // name, never changed afterwards). It is not shared with any
    // rendering flag.
    bool dedicatedMode_ = false;
    int hookAttempts_ = 0;
    ULONGLONG lastHookAttemptTick_ = 0;
    long long telemetryLastQpc_ = 0;
    long long graphicsLastQpc_ = 0;
    unsigned long long telemetryCalls_ = 0;
    unsigned long long telemetryForwarded_ = 0;
    unsigned long long telemetrySkipped_ = 0;
};

// Decides whether a swap chain is the game's main one. Called by the
// Present/ResizeBuffers detours for every swap chain in the process.
// The original code rendered into *every* swap chain and the overlay
// cached a back-buffer bitmap by device only, so the first (possibly
// temporary) swap chain's surface was drawn into forever: nothing on
// the main screen, and a stale surface after that chain was destroyed.
extern "C" bool __cdecl IsLuaOverlayTargetSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain || !g_renderEnabled.load()) return false;
    if (swapChain == g_targetSwapChain.load()) return true;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc))) return false;
    HWND app = g_appWindow.load();
    const bool matchesWindow = app && desc.OutputWindow == app;
    // v3: a fullscreen swap chain is the game's visible chain by
    // definition, even if its window handle differs from mAppWindow.
    const bool matchesFullscreen = !desc.Windowed;
    // Before InitScreen has supplied the window, accept only a
    // full-size swap chain as a heuristic and never a small helper one.
    const bool matchesSize = !app && desc.BufferDesc.Width >= 640 && desc.BufferDesc.Height >= 480;
    if (!matchesWindow && !matchesFullscreen && !matchesSize) return false;
    g_targetSwapChain.store(swapChain);
    // v3: log every *change* of target (via DiagLog, which works before
    // Startup too), not just the first one ever. If the overlay is being
    // drawn into a chain that is not the visible one, this line shows it.
    if (g_lastLoggedTarget.load() != swapChain) {
        g_lastLoggedTarget.store(swapChain);
        DiagLog("[rF2Lua] Target swap chain identified: %p %ux%u windowed=%d via=%s",
            (void*)swapChain, desc.BufferDesc.Width, desc.BufferDesc.Height,
            desc.Windowed ? 1 : 0,
            matchesWindow ? "window" : (matchesFullscreen ? "fullscreen" : "size"));
    }
    return true;
}

// Shared result reporting for both render paths (in-game Present
// bridge and v4 overlay window): first successful draw, and D2D
// failures logged whenever the failure counter changes (v3).
static void NoteDrawResult(bool ok, unsigned lineCount, int width, int height)
{
    g_presentRenders.fetch_add(1);
    if (ok && !g_loggedFirstDraw.exchange(true)) {
        DiagLog("[rF2Lua] First successful overlay draw: %u lines at %dx%d.",
            lineCount, width, height);
    }
    const unsigned long long fails = g_dx11Overlay.FailCount();
    if (fails != g_lastSeenD2DFailCount.load()) {
        g_lastSeenD2DFailCount.store(fails);
        DiagLog("[rF2Lua] DX11 overlay: Direct2D initialisation failed (failure #%llu, HRESULT 0x%08lX). "
                "A common cause is a game device created without BGRA support. The game itself is unaffected; "
                "retries are backed off (next interval %llu frames).",
            fails, (unsigned long)g_dx11Overlay.LastError(),
            g_dx11Overlay.RetryInterval());
    }
}

// Worker for the Present detour bridge. The render mutex is already held
// by RenderLuaOverlayDX11() below; do not lock it again here.
static void RenderLuaOverlayDX11Impl(IDXGISwapChain* swapChain) {
    if (!g_pluginInstance || !swapChain || !g_renderEnabled.load()) return;
    DXGI_SWAP_CHAIN_DESC desc = {};
    int width = 0, height = 0;
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        width = (int)desc.BufferDesc.Width;
        height = (int)desc.BufferDesc.Height;
        g_pluginInstance->RenderLuaFrame(width, height);
    }
    const std::vector<DX11OverlayLine> lines = g_pluginInstance->GetEngineOverlayLines();
    // v3: distinguish "the detour runs but Lua produced nothing" from
    // "the detour never runs" -- in v1/v2 both looked identical (a
    // blank screen and an empty log).
    if (lines.empty() && !g_loggedEmptyLines.exchange(true)) {
        DiagLog("[rF2Lua] Present detour is running but onRender produced 0 lines. "
                "Check that exactly one overlay script is in Scripts and defines onRender.");
    }
    const bool ok = g_dx11Overlay.Render(swapChain, lines);
    NoteDrawResult(ok, (unsigned)lines.size(), width, height);
}

// v4 overlay-window bridges (called from the overlay thread in
// OverlayWindow.cpp; see OverlayWindow.hpp). The render mutex is taken
// with the same SEH-safe manual lock as the Present bridge, because
// Shutdown/UninitScreen hold it on the game thread while invalidating
// the shared renderer.
static void FetchOverlayLinesImpl(int width, int height, std::vector<DX11OverlayLine>& out)
{
    g_pluginInstance->RenderLuaFrame(width, height);
    out = g_pluginInstance->GetEngineOverlayLines();
}

bool FetchOverlayLines(int width, int height, std::vector<DX11OverlayLine>& out)
{
    // v7/v8: once Shutdown has begun, never enter Lua again -- this
    // keeps the overlay thread's stop bounded. (v7 mistakenly used
    // g_renderEnabled here; see the note at its declaration.)
    if (!g_pluginInstance || g_shutdownBegun.load()) return false;
    g_dx11RenderMutex.lock();
    __try {
        FetchOverlayLinesImpl(width, height, out);
    } __finally {
        g_dx11RenderMutex.unlock();
    }
    if (out.empty() && !g_loggedEmptyLines.exchange(true)) {
        DiagLog("[rF2Lua] Overlay window is running but onRender produced 0 lines. "
                "Check that exactly one overlay script is in Scripts and defines onRender.");
    }
    return true;
}

static bool DrawOverlayLinesToDCImpl(HDC hdc, int width, int height, const std::vector<DX11OverlayLine>& lines)
{
    return g_dx11Overlay.RenderToDC(hdc, width, height, lines);
}

bool DrawOverlayLinesToDC(HDC hdc, int width, int height, const std::vector<DX11OverlayLine>& lines)
{
    if (g_shutdownBegun.load()) return false;
    bool ok = false;
    g_dx11RenderMutex.lock();
    __try {
        ok = DrawOverlayLinesToDCImpl(hdc, width, height, lines);
    } __finally {
        g_dx11RenderMutex.unlock();
    }
    NoteDrawResult(ok, (unsigned)lines.size(), width, height);
    return ok;
}

// Called by the DXGI Present detour, for the target swap chain only.
//
// v2 (SEH/mutex fix): the detour wraps this call in __try/__except.
// Structured exception handling does NOT run C++ destructors while
// unwinding, so the v1 code's std::lock_guard inside this function would
// have leaked g_dx11RenderMutex forever if an access violation occurred
// during rendering -- the next Present would then block on the mutex and
// the game would freeze on a black screen. The mutex is therefore now
// locked/unlocked manually, with the unlock in __finally, which *does*
// run during SEH unwinding. This function must not declare objects with
// destructors; the actual work lives in RenderLuaOverlayDX11Impl().
extern "C" __declspec(dllexport) void __cdecl RenderLuaOverlayDX11(IDXGISwapChain* swapChain) {
    if (!g_pluginInstance || !swapChain || !g_renderEnabled.load()) return;
    g_dx11RenderMutex.lock();
    __try {
        RenderLuaOverlayDX11Impl(swapChain);
    } __finally {
        g_dx11RenderMutex.unlock();
    }
}

extern "C" __declspec(dllexport) const char* __cdecl GetPluginName() { return "rF2 Lua Framework"; }
extern "C" __declspec(dllexport) PluginObjectType __cdecl GetPluginType() { return PO_INTERNALS; }
extern "C" __declspec(dllexport) int __cdecl GetPluginVersion() { return 7; }
extern "C" __declspec(dllexport) PluginObject* __cdecl CreatePluginObject() { return new LuaFrameworkPlugin(); }
extern "C" __declspec(dllexport) void __cdecl DestroyPluginObject(PluginObject* obj) { delete static_cast<LuaFrameworkPlugin*>(obj); }

extern "C" __declspec(dllexport) void __cdecl InvalidateLuaOverlayDX11() {
    // Same SEH-safe locking pattern as RenderLuaOverlayDX11().
    g_dx11RenderMutex.lock();
    __try {
        g_dx11Overlay.Invalidate();
    } __finally {
        g_dx11RenderMutex.unlock();
    }
}

