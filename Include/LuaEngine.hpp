#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include "lua.hpp"
#include "DX11Overlay.hpp"

class LuaEngine
{
public:
    LuaEngine();
    ~LuaEngine();
    bool Start(const std::string& pluginDirectory);
    void Stop();
    void Call(const char* functionName);
    void CallLong(const char* functionName, long value);
    void CallScreen(const char* functionName, const struct ScreenInfoV01& info);
    bool HasFunction(const char* functionName);
    void CallTelemetry(const struct TelemInfoV01& info);
    void CallScoring(const struct ScoringInfoV01& info);
    void CallGraphics(const struct GraphicsInfoV02& info);
    void CallEnvironment(const struct EnvironmentInfoV01& info);
    void RenderOverlay(const struct ScreenInfoV01& info);
    void RenderOverlayFrame(int width, int height, const struct ScreenInfoV01* screen = nullptr);
    // v6: remaining SDK access channels (V02-V07) that were never wired.
    bool InitCustomControl(struct CustomControlInfoV01& info);
    bool ApplyPitMenu(struct PitMenuV01& info);
    bool ApplyTrackRules(struct TrackRulesV01& info);
    bool ApplyMultiSessionRules(struct MultiSessionRulesV01& info);
    bool GetCustomVariable(long index, struct CustomVariableV01& var);
    void GetCustomVariableSetting(struct CustomVariableV01& var, long index, struct CustomSettingV01& setting);
    void AccessCustomVariable(struct CustomVariableV01& var);
    std::vector<DX11OverlayLine> GetOverlayLines();
    bool CallNumber(const char* functionName, double input, double& output);
    bool CallHWControl(const char* controlName, double gameValue, double& output);
    void ApplyPhysicsOptions(struct PhysicsOptionsV01& options);
    bool ApplyWeather(double trackNodeSize, struct WeatherControlInfoV01& info);
    unsigned char ApplyCamera(struct CameraControlInfoV01& info);
    bool ApplyMessage(struct MessageInfoV01& info);
    bool ApplyCommentary(struct CommentaryRequestInfoV01& info);
    bool IsRunning() const { return state_ != nullptr; }
    const std::string& GetScriptDirectory() const { return scriptDirectory_; }
    // Writes to the same lua_plugin.log as rf2.log, for diagnostics from
    // the plugin / DX11 hook code (hook install result, target swap chain,
    // Direct2D failure reason). These were previously only sent to
    // OutputDebugString, which the user cannot see.
    void LogLine(const std::string& message) const { Log(message); }

private:
    lua_State* state_;
    std::string pluginDirectory_;
    std::string scriptDirectory_;
    std::string logPath_;
    void LoadScripts();
    void Log(const std::string& message) const;
    static LuaEngine* FromState(lua_State* state);
    static int LuaLog(lua_State* state);
    static int LuaPluginDirectory(lua_State* state);
    static int LuaScriptDirectory(lua_State* state);
    static int LuaTime(lua_State* state);
    static int LuaBlink(lua_State* state);
    static int LuaOverlayText(lua_State* state);
    static int LuaOverlayImage(lua_State* state);
    struct OverlayLine {
        int x, y, r, g, b, size;
        std::string text;
        bool isImage = false;
        std::string path;
        int w = 0, h = 0, alpha = 255;
    };
    std::vector<OverlayLine> overlayLines_;
    bool onRenderMissingLogged_ = false;
    // v6: how many custom controls have been handed out in this run of
    // the engine (the game enumerates them at startup via repeated
    // InitCustomControl calls), and image paths already reported as
    // missing (log once per path, not once per frame).
    long customControlCount_ = 0;
    std::set<std::string> missingImageLogged_;
    std::recursive_mutex luaMutex_;
};
