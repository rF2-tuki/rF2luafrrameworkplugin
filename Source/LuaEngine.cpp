#include "LuaEngine.hpp"
#include "InternalsPlugin.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <windows.h>
#pragma comment(lib, "gdi32.lib")

namespace {
    const char* kLuaApiVersion = "rF2 Lua Framework 1.2 (SDK V07)";
    // Dedicated build (2026-11-22): true when the host process is the
    // rFactor 2 dedicated server ("rFactor2 Dedicated.exe"). Exposed
    // to scripts as rf2.is_dedicated so a script in the shared
    // Lua\Scripts folder can tell which host it is running on.
    // GetModuleFileNameA with a nullptr module returns the host
    // process path.
    bool HostProcessIsDedicated() {
        char path[MAX_PATH] = {0};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return false;
        std::string host(path);
        const size_t slash = host.find_last_of("\\/");
        const std::string exe = (slash == std::string::npos) ? host : host.substr(slash + 1);
        return exe.find("Dedicated") != std::string::npos;
    }
    std::string JoinPath(const std::string& a, const char* b) {
        if (a.empty()) return b;
        const char last = a[a.size() - 1];
        return a + ((last == '\\' || last == '/') ? "" : "\\") + b;
    }
    void PushNumberField(lua_State* L, const char* name, double value) {
        lua_pushnumber(L, value); lua_setfield(L, -2, name);
    }
    void PushIntegerField(lua_State* L, const char* name, lua_Integer value) {
        lua_pushinteger(L, value); lua_setfield(L, -2, name);
    }
    void PushStringField(lua_State* L, const char* name, const char* value) {
        lua_pushstring(L, value ? value : ""); lua_setfield(L, -2, name);
    }
    // The screen table passed to onRender and to the screen lifecycle
    // callbacks. mWidth/mHeight are always present; the remaining
    // ScreenInfoV01 scalar fields are added when the game actually
    // supplied a ScreenInfo (in the overlay-window path only the size
    // is known). The pointer/handle members (mAppWindow, mDevice,
    // mRenderTarget, mDriver) are OS objects, not data, and are
    // intentionally not exposed.
    void PushScreenTable(lua_State* L, int width, int height, const ScreenInfoV01* info) {
        lua_newtable(L);
        PushIntegerField(L, "mWidth", width);
        PushIntegerField(L, "mHeight", height);
        if (info) {
            PushIntegerField(L, "mPixelFormat", info->mPixelFormat);
            PushIntegerField(L, "mRefreshRate", info->mRefreshRate);
            lua_pushboolean(L, info->mWindowed); lua_setfield(L, -2, "mWindowed");
            PushIntegerField(L, "mOptionsWidth", info->mOptionsWidth);
            PushIntegerField(L, "mOptionsHeight", info->mOptionsHeight);
            PushIntegerField(L, "mOptionsLeft", info->mOptionsLeft);
            PushIntegerField(L, "mOptionsUpper", info->mOptionsUpper);
            PushIntegerField(L, "mOptionsLocation", info->mOptionsLocation);
            PushStringField(L, "mOptionsPage", info->mOptionsPage);
        }
    }
}

LuaEngine::LuaEngine() : state_(nullptr), onRenderMissingLogged_(false) {}
LuaEngine::~LuaEngine() { Stop(); }

bool LuaEngine::Start(const std::string& pluginDirectory)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    Stop();
    pluginDirectory_ = pluginDirectory;
    scriptDirectory_ = JoinPath(JoinPath(pluginDirectory_, "Lua"), "Scripts");
    logPath_ = JoinPath(JoinPath(pluginDirectory_, "Lua"), "lua_plugin.log");
    CreateDirectoryA(JoinPath(pluginDirectory_, "Lua").c_str(), nullptr);
    CreateDirectoryA(scriptDirectory_.c_str(), nullptr);

    state_ = luaL_newstate();
    if (!state_) { Log("ERROR: luaL_newstate failed"); return false; }
    luaL_openlibs(state_);
    lua_pushlightuserdata(state_, this);
    lua_setfield(state_, LUA_REGISTRYINDEX, "rf2.luaengine.instance");

    lua_newtable(state_);
    lua_pushcfunction(state_, &LuaEngine::LuaLog); lua_setfield(state_, -2, "log");
    lua_pushcfunction(state_, &LuaEngine::LuaPluginDirectory); lua_setfield(state_, -2, "plugin_dir");
    lua_pushcfunction(state_, &LuaEngine::LuaScriptDirectory); lua_setfield(state_, -2, "script_dir");
    lua_pushcfunction(state_, &LuaEngine::LuaTime); lua_setfield(state_, -2, "time");
    lua_pushcfunction(state_, &LuaEngine::LuaBlink); lua_setfield(state_, -2, "blink");
    lua_pushstring(state_, kLuaApiVersion); lua_setfield(state_, -2, "version");
    lua_pushboolean(state_, HostProcessIsDedicated()); lua_setfield(state_, -2, "is_dedicated");
    lua_setglobal(state_, "rf2");
    lua_newtable(state_);
    lua_pushcfunction(state_, &LuaEngine::LuaOverlayText); lua_setfield(state_, -2, "text");
    lua_pushcfunction(state_, &LuaEngine::LuaOverlayImage); lua_setfield(state_, -2, "image");
    lua_setglobal(state_, "overlay");

    Log(std::string("Starting ") + kLuaApiVersion);
    Log(std::string("Script directory: ") + scriptDirectory_);
    onRenderMissingLogged_ = false;
    customControlCount_ = 0;
    missingImageLogged_.clear();
    LoadScripts();
    Call("onStartup");
    return true;
}

void LuaEngine::Stop()
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (state_) {
        Call("onShutdown");
        lua_close(state_);
        state_ = nullptr;
    }
}

void LuaEngine::Log(const std::string& message) const
{
    std::ofstream out(logPath_.c_str(), std::ios::out | std::ios::app);
    if (out) out << message << "\r\n";
}

LuaEngine* LuaEngine::FromState(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "rf2.luaengine.instance");
    LuaEngine* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return engine;
}

int LuaEngine::LuaLog(lua_State* L)
{
    LuaEngine* engine = FromState(L);
    const char* message = luaL_checkstring(L, 1);
    if (engine) engine->Log(std::string("[Lua] ") + message);
    return 0;
}
int LuaEngine::LuaPluginDirectory(lua_State* L)
{
    LuaEngine* engine = FromState(L);
    lua_pushstring(L, engine ? engine->pluginDirectory_.c_str() : "");
    return 1;
}
int LuaEngine::LuaScriptDirectory(lua_State* L)
{
    LuaEngine* engine = FromState(L);
    lua_pushstring(L, engine ? engine->scriptDirectory_.c_str() : "");
    return 1;
}
int LuaEngine::LuaOverlayText(lua_State* L)
{
    LuaEngine* e = FromState(L);
    if (!e) return 0;
    OverlayLine line;
    line.x = (int)luaL_checkinteger(L, 1); line.y = (int)luaL_checkinteger(L, 2);
    line.text = luaL_checkstring(L, 3);
    line.r = (int)luaL_optinteger(L, 4, 255); line.g = (int)luaL_optinteger(L, 5, 255);
    line.b = (int)luaL_optinteger(L, 6, 255); line.size = (int)luaL_optinteger(L, 7, 18);
    if (line.size < 8) line.size = 8; if (line.size > 72) line.size = 72;
    e->overlayLines_.push_back(line);
    return 0;
}

namespace {
    // Steady-clock seconds since the first call (process-wide). Used by
    // rf2.time() and rf2.blink(); QueryPerformanceCounter is monotonic,
    // so blinking stays regular regardless of session state.
    double SteadySeconds()
    {
        static LARGE_INTEGER freq = {};
        static LARGE_INTEGER start = {};
        if (freq.QuadPart == 0) {
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    }
    bool IsAbsolutePath(const std::string& p)
    {
        return p.size() >= 2 && p[1] == ':' || p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
    }
}

int LuaEngine::LuaTime(lua_State* L)
{
    lua_pushnumber(L, SteadySeconds());
    return 1;
}

int LuaEngine::LuaBlink(lua_State* L)
{
    // rf2.blink(period [, duty]) -> boolean: true for the first `duty`
    // fraction (default 0.5) of every `period` seconds, repeating.
    const double period = luaL_checknumber(L, 1);
    double duty = luaL_optnumber(L, 2, 0.5);
    if (period <= 0.0) { lua_pushboolean(L, 1); return 1; }
    if (duty < 0.0) duty = 0.0;
    if (duty > 1.0) duty = 1.0;
    const double phase = fmod(SteadySeconds(), period) / period;
    lua_pushboolean(L, phase < duty);
    return 1;
}

int LuaEngine::LuaOverlayImage(lua_State* L)
{
    LuaEngine* e = FromState(L);
    if (!e) return 0;
    OverlayLine line;
    line.isImage = true;
    line.x = (int)luaL_checkinteger(L, 1); line.y = (int)luaL_checkinteger(L, 2);
    const char* path = luaL_checkstring(L, 3);
    line.w = (int)luaL_optinteger(L, 4, 0); line.h = (int)luaL_optinteger(L, 5, 0);
    line.alpha = (int)luaL_optinteger(L, 6, 255);
    if (line.alpha < 0) line.alpha = 0;
    if (line.alpha > 255) line.alpha = 255;
    // Relative paths are resolved against the Scripts directory, so a
    // script can just write overlay.image(0, 0, "logo.png").
    line.path = IsAbsolutePath(path) ? path : JoinPath(e->scriptDirectory_, path);
    // Report a missing file once per path (this runs every frame).
    if (GetFileAttributesA(line.path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        e->missingImageLogged_.insert(line.path).second) {
        e->Log(std::string("overlay.image: file not found: ") + line.path);
    }
    e->overlayLines_.push_back(line);
    return 0;
}

void LuaEngine::LoadScripts()
{
    const std::string pattern = JoinPath(scriptDirectory_, "*.lua");
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        Log("No Lua scripts found. Add *.lua files to Plugins\\Lua\\Scripts.");
        return;
    }
    std::vector<std::string> files;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    std::sort(files.begin(), files.end());

    for (size_t i = 0; i < files.size(); ++i) {
        const std::string path = JoinPath(scriptDirectory_, files[i].c_str());
        if (luaL_loadfilex(state_, path.c_str(), nullptr) != LUA_OK || lua_pcall(state_, 0, 0, 0) != LUA_OK) {
            const char* error = lua_tostring(state_, -1);
            Log(std::string("Script load error [") + files[i] + "]: " + (error ? error : "unknown error"));
            lua_pop(state_, 1);
        } else {
            Log(std::string("Loaded script: ") + files[i]);
        }
    }
}

void LuaEngine::Call(const char* functionName)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, functionName);
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return; }
    if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [") + functionName + "]: " + (error ? error : "unknown error"));
        lua_pop(state_, 1);
    }
}

void LuaEngine::CallLong(const char* functionName, long value)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, functionName);
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return; }
    lua_pushinteger(state_, value);
    if (lua_pcall(state_, 1, 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [") + functionName + "]: " + (error ? error : "unknown error"));
        lua_pop(state_, 1);
    }
}

void LuaEngine::CallScreen(const char* functionName, const ScreenInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, functionName);
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return; }
    PushScreenTable(state_, (int)info.mWidth, (int)info.mHeight, &info);
    if (lua_pcall(state_, 1, 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [") + functionName + "]: " + (error ? error : "unknown error"));
        lua_pop(state_, 1);
    }
}

bool LuaEngine::HasFunction(const char* functionName)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    lua_getglobal(state_, functionName);
    const bool found = lua_isfunction(state_, -1) != 0;
    lua_pop(state_, 1);
    return found;
}

void LuaEngine::CallTelemetry(const TelemInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, "onTelemetry");
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return; }
    lua_newtable(state_);
    PushIntegerField(state_, "id", info.mID);
    PushNumberField(state_, "delta_time", info.mDeltaTime);
    PushNumberField(state_, "elapsed_time", info.mElapsedTime);
    PushIntegerField(state_, "lap", info.mLapNumber);
    PushNumberField(state_, "lap_start_time", info.mLapStartET);
    PushStringField(state_, "vehicle", info.mVehicleName);
    PushStringField(state_, "track", info.mTrackName);
    PushIntegerField(state_, "gear", info.mGear);
    PushNumberField(state_, "rpm", info.mEngineRPM);
    PushNumberField(state_, "max_rpm", info.mEngineMaxRPM);
    PushNumberField(state_, "water_temp", info.mEngineWaterTemp);
    PushNumberField(state_, "oil_temp", info.mEngineOilTemp);
    PushNumberField(state_, "fuel", info.mFuel);
    PushNumberField(state_, "throttle", info.mUnfilteredThrottle);
    PushNumberField(state_, "brake", info.mUnfilteredBrake);
    PushNumberField(state_, "steering", info.mUnfilteredSteering);
    PushNumberField(state_, "clutch", info.mUnfilteredClutch);
    PushNumberField(state_, "speed_x", info.mLocalVel.x);
    PushNumberField(state_, "speed_y", info.mLocalVel.y);
    PushNumberField(state_, "speed_z", info.mLocalVel.z);
    PushNumberField(state_, "world_x", info.mPos.x);
    PushNumberField(state_, "world_y", info.mPos.y);
    PushNumberField(state_, "world_z", info.mPos.z);
    PushNumberField(state_, "steering_torque", info.mSteeringShaftTorque);
    PushNumberField(state_, "engine_torque", info.mEngineTorque);
    PushNumberField(state_, "boost", info.mTurboBoostPressure);
    PushNumberField(state_, "fuel_capacity", info.mFuelCapacity);
    // Original SDK member names are also exposed for scripts that mirror C++.
    PushIntegerField(state_, "mID", info.mID);
    PushNumberField(state_, "mDeltaTime", info.mDeltaTime);
    PushNumberField(state_, "mElapsedTime", info.mElapsedTime);
    PushIntegerField(state_, "mLapNumber", info.mLapNumber);
    PushNumberField(state_, "mLapStartET", info.mLapStartET);
    PushStringField(state_, "mVehicleName", info.mVehicleName);
    PushStringField(state_, "mTrackName", info.mTrackName);
    PushIntegerField(state_, "mGear", info.mGear);
    PushNumberField(state_, "mEngineRPM", info.mEngineRPM);
    PushNumberField(state_, "mEngineMaxRPM", info.mEngineMaxRPM);
    PushNumberField(state_, "mEngineWaterTemp", info.mEngineWaterTemp);
    PushNumberField(state_, "mEngineOilTemp", info.mEngineOilTemp);
    PushNumberField(state_, "mFuel", info.mFuel);
    PushNumberField(state_, "mFuelCapacity", info.mFuelCapacity);
    PushNumberField(state_, "mUnfilteredThrottle", info.mUnfilteredThrottle);
    PushNumberField(state_, "mUnfilteredBrake", info.mUnfilteredBrake);
    PushNumberField(state_, "mUnfilteredSteering", info.mUnfilteredSteering);
    PushNumberField(state_, "mUnfilteredClutch", info.mUnfilteredClutch);
    PushNumberField(state_, "mFilteredThrottle", info.mFilteredThrottle);
    PushNumberField(state_, "mFilteredBrake", info.mFilteredBrake);
    PushNumberField(state_, "mFilteredSteering", info.mFilteredSteering);
    PushNumberField(state_, "mFilteredClutch", info.mFilteredClutch);
    PushNumberField(state_, "mSteeringShaftTorque", info.mSteeringShaftTorque);
    PushNumberField(state_, "mEngineTorque", info.mEngineTorque);
    PushNumberField(state_, "mTurboBoostPressure", info.mTurboBoostPressure);
    lua_newtable(state_); PushNumberField(state_, "x", info.mLocalVel.x); PushNumberField(state_, "y", info.mLocalVel.y); PushNumberField(state_, "z", info.mLocalVel.z); lua_setfield(state_, -2, "mLocalVel");
    lua_newtable(state_); PushNumberField(state_, "x", info.mLocalAccel.x); PushNumberField(state_, "y", info.mLocalAccel.y); PushNumberField(state_, "z", info.mLocalAccel.z); lua_setfield(state_, -2, "mLocalAccel");
    lua_newtable(state_); PushNumberField(state_, "x", info.mPos.x); PushNumberField(state_, "y", info.mPos.y); PushNumberField(state_, "z", info.mPos.z); lua_setfield(state_, -2, "mPos");
    PushNumberField(state_, "mFrontWingHeight", info.mFrontWingHeight);
    PushNumberField(state_, "mFrontRideHeight", info.mFrontRideHeight);
    PushNumberField(state_, "mRearRideHeight", info.mRearRideHeight);
    PushNumberField(state_, "mDrag", info.mDrag);
    PushNumberField(state_, "mFrontDownforce", info.mFrontDownforce);
    PushNumberField(state_, "mRearDownforce", info.mRearDownforce);

    PushNumberField(state_, "mClutchRPM", info.mClutchRPM);
    PushNumberField(state_, "mFront3rdDeflection", info.mFront3rdDeflection);
    PushNumberField(state_, "mRear3rdDeflection", info.mRear3rdDeflection);
    PushIntegerField(state_, "mScheduledStops", info.mScheduledStops);
    lua_pushboolean(state_, info.mOverheating); lua_setfield(state_, -2, "mOverheating");
    lua_pushboolean(state_, info.mDetached); lua_setfield(state_, -2, "mDetached");
    lua_pushboolean(state_, info.mHeadlights); lua_setfield(state_, -2, "mHeadlights");
    PushNumberField(state_, "mLastImpactET", info.mLastImpactET);
    PushNumberField(state_, "mLastImpactMagnitude", info.mLastImpactMagnitude);
    lua_newtable(state_); PushNumberField(state_, "x", info.mLastImpactPos.x); PushNumberField(state_, "y", info.mLastImpactPos.y); PushNumberField(state_, "z", info.mLastImpactPos.z); lua_setfield(state_, -2, "mLastImpactPos");
    PushIntegerField(state_, "mCurrentSector", info.mCurrentSector);
    PushIntegerField(state_, "mSpeedLimiter", info.mSpeedLimiter);
    PushIntegerField(state_, "mMaxGears", info.mMaxGears);
    PushIntegerField(state_, "mFrontTireCompoundIndex", info.mFrontTireCompoundIndex);
    PushIntegerField(state_, "mRearTireCompoundIndex", info.mRearTireCompoundIndex);
    PushIntegerField(state_, "mFrontFlapActivated", info.mFrontFlapActivated);
    PushIntegerField(state_, "mRearFlapActivated", info.mRearFlapActivated);
    PushIntegerField(state_, "mRearFlapLegalStatus", info.mRearFlapLegalStatus);
    PushIntegerField(state_, "mIgnitionStarter", info.mIgnitionStarter);
    PushStringField(state_, "mFrontTireCompoundName", info.mFrontTireCompoundName);
    PushStringField(state_, "mRearTireCompoundName", info.mRearTireCompoundName);
    PushIntegerField(state_, "mSpeedLimiterAvailable", info.mSpeedLimiterAvailable);
    PushIntegerField(state_, "mAntiStallActivated", info.mAntiStallActivated);
    PushNumberField(state_, "mVisualSteeringWheelRange", info.mVisualSteeringWheelRange);
    PushNumberField(state_, "mRearBrakeBias", info.mRearBrakeBias);
    PushNumberField(state_, "mPhysicalSteeringWheelRange", info.mPhysicalSteeringWheelRange);
    lua_createtable(state_, 8, 0); for(int i=0;i<8;++i){lua_pushinteger(state_,info.mDentSeverity[i]);lua_rawseti(state_,-2,i+1);} lua_setfield(state_,-2,"mDentSeverity");
    lua_createtable(state_, 3, 0); for(int i=0;i<3;++i){lua_pushnumber(state_,info.mPhysicsToGraphicsOffset[i]);lua_rawseti(state_,-2,i+1);} lua_setfield(state_,-2,"mPhysicsToGraphicsOffset");
    lua_newtable(state_); for(int i=0;i<3;++i){lua_newtable(state_); PushNumberField(state_,"x",info.mOri[i].x); PushNumberField(state_,"y",info.mOri[i].y); PushNumberField(state_,"z",info.mOri[i].z); lua_rawseti(state_,-2,i+1);} lua_setfield(state_,-2,"mOri");
    lua_newtable(state_); PushNumberField(state_,"x",info.mLocalRot.x); PushNumberField(state_,"y",info.mLocalRot.y); PushNumberField(state_,"z",info.mLocalRot.z); lua_setfield(state_,-2,"mLocalRot");
    lua_newtable(state_); PushNumberField(state_,"x",info.mLocalRotAccel.x); PushNumberField(state_,"y",info.mLocalRotAccel.y); PushNumberField(state_,"z",info.mLocalRotAccel.z); lua_setfield(state_,-2,"mLocalRotAccel");

    lua_createtable(state_, 4, 0);
    for (int i = 0; i < 4; ++i) {
        const TelemWheelV01& w = info.mWheel[i];
        lua_newtable(state_);
        PushNumberField(state_, "ride_height", w.mRideHeight);
        PushNumberField(state_, "suspension_deflection", w.mSuspensionDeflection);
        PushNumberField(state_, "brake_temp", w.mBrakeTemp);
        PushNumberField(state_, "tire_load", w.mTireLoad);
        PushNumberField(state_, "pressure", w.mPressure);
        PushNumberField(state_, "wear", w.mWear);
        PushNumberField(state_, "temperature_inner", w.mTemperature[0]);
        PushNumberField(state_, "temperature_middle", w.mTemperature[1]);
        PushNumberField(state_, "temperature_outer", w.mTemperature[2]);
        PushNumberField(state_, "lateral_force", w.mLateralForce);
        PushNumberField(state_, "longitudinal_force", w.mLongitudinalForce);
        PushNumberField(state_, "mSuspForce", w.mSuspForce);
        PushNumberField(state_, "mBrakePressure", w.mBrakePressure);
        PushNumberField(state_, "mRotation", w.mRotation);
        PushNumberField(state_, "mLateralPatchVel", w.mLateralPatchVel);
        PushNumberField(state_, "mLongitudinalPatchVel", w.mLongitudinalPatchVel);
        PushNumberField(state_, "mLateralGroundVel", w.mLateralGroundVel);
        PushNumberField(state_, "mLongitudinalGroundVel", w.mLongitudinalGroundVel);
        PushNumberField(state_, "mCamber", w.mCamber);
        PushNumberField(state_, "mGripFract", w.mGripFract);
        PushStringField(state_, "mTerrainName", w.mTerrainName);
        PushIntegerField(state_, "mSurfaceType", w.mSurfaceType);
        lua_pushboolean(state_,w.mFlat); lua_setfield(state_,-2,"mFlat");
        lua_pushboolean(state_,w.mDetached); lua_setfield(state_,-2,"mDetached");
        PushNumberField(state_, "mVerticalTireDeflection", w.mVerticalTireDeflection);
        PushNumberField(state_, "mWheelYLocation", w.mWheelYLocation);
        PushNumberField(state_, "mToe", w.mToe);
        PushNumberField(state_, "mTireCarcassTemperature", w.mTireCarcassTemperature);
        lua_createtable(state_,3,0); for(int j=0;j<3;++j){lua_pushnumber(state_,w.mTireInnerLayerTemperature[j]);lua_rawseti(state_,-2,j+1);} lua_setfield(state_,-2,"mTireInnerLayerTemperature");
        PushNumberField(state_, "mRideHeight", w.mRideHeight);
        PushNumberField(state_, "mSuspensionDeflection", w.mSuspensionDeflection);
        PushNumberField(state_, "mBrakeTemp", w.mBrakeTemp);
        PushNumberField(state_, "mTireLoad", w.mTireLoad);
        PushNumberField(state_, "mPressure", w.mPressure);
        PushNumberField(state_, "mWear", w.mWear);
        PushNumberField(state_, "mTemperature0", w.mTemperature[0]);
        PushNumberField(state_, "mTemperature1", w.mTemperature[1]);
        PushNumberField(state_, "mTemperature2", w.mTemperature[2]);
        lua_createtable(state_, 3, 0);
        for (int ti = 0; ti < 3; ++ti) { lua_pushnumber(state_, w.mTemperature[ti]); lua_rawseti(state_, -2, ti + 1); }
        lua_setfield(state_, -2, "mTemperature");
        PushNumberField(state_, "mLateralForce", w.mLateralForce);
        PushNumberField(state_, "mLongitudinalForce", w.mLongitudinalForce);
        lua_rawseti(state_, -2, i + 1);
    }
    lua_pushvalue(state_, -1);
    lua_setfield(state_, -3, "mWheel");
    lua_setfield(state_, -2, "wheels");
    if (lua_pcall(state_, 1, 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onTelemetry]: ") + (error ? error : "unknown error"));
        lua_pop(state_, 1);
    }
}


void LuaEngine::CallScoring(const ScoringInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, "onScoring"); if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return;}
    lua_newtable(state_); PushStringField(state_,"mTrackName",info.mTrackName); PushIntegerField(state_,"mSession",info.mSession);
    PushNumberField(state_,"mCurrentET",info.mCurrentET); PushNumberField(state_,"mEndET",info.mEndET); PushIntegerField(state_,"mMaxLaps",info.mMaxLaps);
    PushNumberField(state_,"mLapDist",info.mLapDist); PushStringField(state_,"mResultsStream",info.mResultsStream); PushIntegerField(state_,"mNumVehicles",info.mNumVehicles); PushIntegerField(state_,"mGamePhase",info.mGamePhase);
    PushIntegerField(state_,"mYellowFlagState",info.mYellowFlagState); PushIntegerField(state_,"mStartLight",info.mStartLight); PushIntegerField(state_,"mNumRedLights",info.mNumRedLights);
    lua_pushboolean(state_,info.mInRealtime); lua_setfield(state_,-2,"mInRealtime"); PushStringField(state_,"mPlayerName",info.mPlayerName); PushStringField(state_,"mPlrFileName",info.mPlrFileName);
    PushNumberField(state_,"mDarkCloud",info.mDarkCloud); PushNumberField(state_,"mRaining",info.mRaining); PushNumberField(state_,"mAmbientTemp",info.mAmbientTemp); PushNumberField(state_,"mTrackTemp",info.mTrackTemp);
    lua_newtable(state_); PushNumberField(state_,"x",info.mWind.x); PushNumberField(state_,"y",info.mWind.y); PushNumberField(state_,"z",info.mWind.z); lua_setfield(state_,-2,"mWind");
    PushNumberField(state_,"mMinPathWetness",info.mMinPathWetness); PushNumberField(state_,"mMaxPathWetness",info.mMaxPathWetness);
    lua_createtable(state_,3,0); for(int i=0;i<3;++i){lua_pushinteger(state_,info.mSectorFlag[i]);lua_rawseti(state_,-2,i+1);} lua_setfield(state_,-2,"mSectorFlag");
    lua_createtable(state_,info.mNumVehicles,0);
    for(long i=0;i<info.mNumVehicles && i<256 && info.mVehicle;++i){const VehicleScoringInfoV01& v=info.mVehicle[i];lua_newtable(state_);
      PushIntegerField(state_,"mID",v.mID);PushStringField(state_,"mDriverName",v.mDriverName);PushStringField(state_,"mVehicleName",v.mVehicleName);PushIntegerField(state_,"mTotalLaps",v.mTotalLaps);PushIntegerField(state_,"mSector",v.mSector);PushIntegerField(state_,"mFinishStatus",v.mFinishStatus);
      PushNumberField(state_,"mLapDist",v.mLapDist);PushNumberField(state_,"mPathLateral",v.mPathLateral);PushNumberField(state_,"mTrackEdge",v.mTrackEdge);PushNumberField(state_,"mBestSector1",v.mBestSector1);PushNumberField(state_,"mBestSector2",v.mBestSector2);PushNumberField(state_,"mBestLapTime",v.mBestLapTime);PushNumberField(state_,"mLastSector1",v.mLastSector1);PushNumberField(state_,"mLastSector2",v.mLastSector2);PushNumberField(state_,"mLastLapTime",v.mLastLapTime);PushNumberField(state_,"mCurSector1",v.mCurSector1);PushNumberField(state_,"mCurSector2",v.mCurSector2);
      PushIntegerField(state_,"mNumPitstops",v.mNumPitstops);PushIntegerField(state_,"mNumPenalties",v.mNumPenalties);lua_pushboolean(state_,v.mIsPlayer);lua_setfield(state_,-2,"mIsPlayer");PushIntegerField(state_,"mControl",v.mControl);lua_pushboolean(state_,v.mInPits);lua_setfield(state_,-2,"mInPits");PushIntegerField(state_,"mPlace",v.mPlace);PushStringField(state_,"mVehicleClass",v.mVehicleClass);PushNumberField(state_,"mTimeBehindNext",v.mTimeBehindNext);PushIntegerField(state_,"mLapsBehindNext",v.mLapsBehindNext);PushNumberField(state_,"mTimeBehindLeader",v.mTimeBehindLeader);PushIntegerField(state_,"mLapsBehindLeader",v.mLapsBehindLeader);PushNumberField(state_,"mLapStartET",v.mLapStartET);
      lua_newtable(state_);PushNumberField(state_,"x",v.mPos.x);PushNumberField(state_,"y",v.mPos.y);PushNumberField(state_,"z",v.mPos.z);lua_setfield(state_,-2,"mPos");lua_newtable(state_);PushNumberField(state_,"x",v.mLocalVel.x);PushNumberField(state_,"y",v.mLocalVel.y);PushNumberField(state_,"z",v.mLocalVel.z);lua_setfield(state_,-2,"mLocalVel");
      lua_newtable(state_);PushNumberField(state_,"x",v.mLocalAccel.x);PushNumberField(state_,"y",v.mLocalAccel.y);PushNumberField(state_,"z",v.mLocalAccel.z);lua_setfield(state_,-2,"mLocalAccel");lua_newtable(state_);for(int oi=0;oi<3;++oi){lua_newtable(state_);PushNumberField(state_,"x",v.mOri[oi].x);PushNumberField(state_,"y",v.mOri[oi].y);PushNumberField(state_,"z",v.mOri[oi].z);lua_rawseti(state_,-2,oi+1);}lua_setfield(state_,-2,"mOri");lua_newtable(state_);PushNumberField(state_,"x",v.mLocalRot.x);PushNumberField(state_,"y",v.mLocalRot.y);PushNumberField(state_,"z",v.mLocalRot.z);lua_setfield(state_,-2,"mLocalRot");lua_newtable(state_);PushNumberField(state_,"x",v.mLocalRotAccel.x);PushNumberField(state_,"y",v.mLocalRotAccel.y);PushNumberField(state_,"z",v.mLocalRotAccel.z);lua_setfield(state_,-2,"mLocalRotAccel");
      PushIntegerField(state_,"mHeadlights",v.mHeadlights);PushIntegerField(state_,"mPitState",v.mPitState);PushIntegerField(state_,"mServerScored",v.mServerScored);PushIntegerField(state_,"mIndividualPhase",v.mIndividualPhase);PushIntegerField(state_,"mQualification",v.mQualification);lua_createtable(state_,16,0);for(int ui=0;ui<16;++ui){lua_pushinteger(state_,v.mUpgradePack[ui]);lua_rawseti(state_,-2,ui+1);}lua_setfield(state_,-2,"mUpgradePack");PushNumberField(state_,"mTimeIntoLap",v.mTimeIntoLap);PushNumberField(state_,"mEstimatedLapTime",v.mEstimatedLapTime);PushStringField(state_,"mPitGroup",v.mPitGroup);PushIntegerField(state_,"mFlag",v.mFlag);lua_pushboolean(state_,v.mUnderYellow);lua_setfield(state_,-2,"mUnderYellow");PushIntegerField(state_,"mCountLapFlag",v.mCountLapFlag);lua_pushboolean(state_,v.mInGarageStall);lua_setfield(state_,-2,"mInGarageStall");lua_rawseti(state_,-2,i+1);
    }
    lua_setfield(state_,-2,"mVehicle");
    if(lua_pcall(state_,1,0,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onScoring]: ")+(e?e:"unknown"));lua_pop(state_,1);}
}

void LuaEngine::CallGraphics(const GraphicsInfoV02& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_)return; lua_getglobal(state_,"onGraphics"); if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return;} lua_newtable(state_);
    lua_newtable(state_);PushNumberField(state_,"x",info.mCamPos.x);PushNumberField(state_,"y",info.mCamPos.y);PushNumberField(state_,"z",info.mCamPos.z);lua_setfield(state_,-2,"mCamPos");
    lua_newtable(state_);for(int i=0;i<3;++i){lua_newtable(state_);PushNumberField(state_,"x",info.mCamOri[i].x);PushNumberField(state_,"y",info.mCamOri[i].y);PushNumberField(state_,"z",info.mCamOri[i].z);lua_rawseti(state_,-2,i+1);}lua_setfield(state_,-2,"mCamOri");
    PushNumberField(state_,"mAmbientRed",info.mAmbientRed);PushNumberField(state_,"mAmbientGreen",info.mAmbientGreen);PushNumberField(state_,"mAmbientBlue",info.mAmbientBlue);PushIntegerField(state_,"mID",info.mID);PushIntegerField(state_,"mCameraType",info.mCameraType);
    if(lua_pcall(state_,1,0,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onGraphics]: ")+(e?e:"unknown"));lua_pop(state_,1);}
}
void LuaEngine::CallEnvironment(const EnvironmentInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_)return;lua_getglobal(state_,"onEnvironment");if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return;}lua_newtable(state_);lua_createtable(state_,16,0);for(int i=0;i<16;++i){if(info.mPath[i])lua_pushstring(state_,info.mPath[i]);else lua_pushnil(state_);lua_rawseti(state_,-2,i+1);}lua_setfield(state_,-2,"mPath");
    if(lua_pcall(state_,1,0,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onEnvironment]: ")+(e?e:"unknown"));lua_pop(state_,1);}
}
void LuaEngine::RenderOverlay(const ScreenInfoV01& info)
{
    RenderOverlayFrame(info.mWidth, info.mHeight, &info);
}

void LuaEngine::RenderOverlayFrame(int width, int height, const ScreenInfoV01* screen)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    overlayLines_.clear();
    lua_getglobal(state_, "onRender");
    if (lua_isfunction(state_, -1)) {
        PushScreenTable(state_, width, height, screen);
        if (lua_pcall(state_, 1, 0, 0) != LUA_OK) {
            const char* e = lua_tostring(state_, -1);
            Log(std::string("Lua callback error [onRender]: ") + (e ? e : "unknown"));
            lua_pop(state_, 1);
        }
    } else {
        lua_pop(state_, 1);
        if (!onRenderMissingLogged_) {
            onRenderMissingLogged_ = true;
            Log("onRender is not defined by any loaded script: nothing will be drawn. "
                "Copy exactly one overlay script (debug_overlay.lua, race_overlay.lua or "
                "dx11_overlay_example.lua) into the Scripts directory shown above.");
        }
    }
}

std::vector<DX11OverlayLine> LuaEngine::GetOverlayLines()
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    std::vector<DX11OverlayLine> result;
    result.reserve(overlayLines_.size());
    for (const auto& line : overlayLines_) {
        DX11OverlayLine item;
        item.x = line.x; item.y = line.y;
        item.r = line.r; item.g = line.g; item.b = line.b; item.size = line.size;
        item.text = line.text;
        item.isImage = line.isImage; item.path = line.path;
        item.w = line.w; item.h = line.h; item.alpha = line.alpha;
        result.push_back(item);
    }
    return result;
}

namespace {
    bool LuaCallWithTable(lua_State* L, const char* fn) {
        lua_getglobal(L, fn);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
        lua_newtable(L);
        return true;
    }
    bool ReadNumber(lua_State* L, int table, const char* key, double& value) {
        lua_getfield(L, table, key);
        const bool ok = lua_isnumber(L, -1) != 0;
        if (ok) value = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return ok;
    }
    bool ReadInteger(lua_State* L, int table, const char* key, lua_Integer& value) {
        lua_getfield(L, table, key);
        const bool ok = lua_isinteger(L, -1) != 0 || lua_isnumber(L, -1) != 0;
        if (ok) value = static_cast<lua_Integer>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return ok;
    }
    bool ReadBoolean(lua_State* L, int table, const char* key, bool& value) {
        lua_getfield(L, table, key);
        const bool ok = lua_isboolean(L, -1) != 0;
        if (ok) value = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ok;
    }
    void ReadPhysics(lua_State* L, int t, PhysicsOptionsV01& o) {
#define RF2_READ_BYTE(member) { double v=o.member; if(ReadNumber(L,t,#member,v)) o.member=(unsigned char)v; }
#define RF2_READ_FLOAT(member) { double v=o.member; if(ReadNumber(L,t,#member,v)) o.member=(float)v; }
        RF2_READ_BYTE(mTractionControl) RF2_READ_BYTE(mAntiLockBrakes)
        RF2_READ_BYTE(mStabilityControl) RF2_READ_BYTE(mAutoShift)
        RF2_READ_BYTE(mAutoClutch) RF2_READ_BYTE(mInvulnerable)
        RF2_READ_BYTE(mOppositeLock) RF2_READ_BYTE(mSteeringHelp)
        RF2_READ_BYTE(mBrakingHelp) RF2_READ_BYTE(mSpinRecovery)
        RF2_READ_BYTE(mAutoPit) RF2_READ_BYTE(mAutoLift) RF2_READ_BYTE(mAutoBlip)
        RF2_READ_BYTE(mFuelMult) RF2_READ_BYTE(mTireMult) RF2_READ_BYTE(mMechFail)
        RF2_READ_BYTE(mAllowPitcrewPush) RF2_READ_BYTE(mRepeatShifts)
        RF2_READ_BYTE(mHoldClutch) RF2_READ_BYTE(mAutoReverse)
        RF2_READ_BYTE(mAlternateNeutral) RF2_READ_BYTE(mAIControl)
        RF2_READ_FLOAT(mManualShiftOverrideTime) RF2_READ_FLOAT(mAutoShiftOverrideTime)
        RF2_READ_FLOAT(mSpeedSensitiveSteering) RF2_READ_FLOAT(mSteerRatioSpeed)
#undef RF2_READ_BYTE
#undef RF2_READ_FLOAT
    }
}

bool LuaEngine::CallNumber(const char* functionName, double input, double& output)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    lua_getglobal(state_, functionName);
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return false; }
    lua_pushnumber(state_, input);
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e=lua_tostring(state_,-1); Log(std::string("Lua callback error [")+functionName+"]: "+(e?e:"unknown")); lua_pop(state_,1); return false;
    }
    const bool ok=lua_isnumber(state_,-1)!=0;
    if(ok) output=lua_tonumber(state_,-1);
    lua_pop(state_,1);
    return ok;
}

bool LuaEngine::CallHWControl(const char* controlName, double gameValue, double& output)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    lua_getglobal(state_, "onCheckHWControl");
    if (!lua_isfunction(state_,-1)) { lua_pop(state_,1); return false; }
    lua_pushstring(state_,controlName?controlName:"");
    lua_pushnumber(state_,gameValue);
    if(lua_pcall(state_,2,1,0)!=LUA_OK) {
        const char* e=lua_tostring(state_,-1); Log(std::string("Lua callback error [onCheckHWControl]: ")+(e?e:"unknown")); lua_pop(state_,1); return false;
    }
    const bool ok=lua_isnumber(state_,-1)!=0;
    if(ok) output=lua_tonumber(state_,-1);
    lua_pop(state_,1);
    return ok;
}

void LuaEngine::ApplyPhysicsOptions(PhysicsOptionsV01& options)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_) return;
    lua_getglobal(state_,"onPhysicsOptions");
    if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return;}
    lua_newtable(state_);
#define RF2_PUSH_BYTE(member) PushIntegerField(state_,#member,options.member);
#define RF2_PUSH_FLOAT(member) PushNumberField(state_,#member,options.member);
    RF2_PUSH_BYTE(mTractionControl) RF2_PUSH_BYTE(mAntiLockBrakes)
    RF2_PUSH_BYTE(mStabilityControl) RF2_PUSH_BYTE(mAutoShift)
    RF2_PUSH_BYTE(mAutoClutch) RF2_PUSH_BYTE(mInvulnerable)
    RF2_PUSH_BYTE(mOppositeLock) RF2_PUSH_BYTE(mSteeringHelp)
    RF2_PUSH_BYTE(mBrakingHelp) RF2_PUSH_BYTE(mSpinRecovery)
    RF2_PUSH_BYTE(mAutoPit) RF2_PUSH_BYTE(mAutoLift) RF2_PUSH_BYTE(mAutoBlip)
    RF2_PUSH_BYTE(mFuelMult) RF2_PUSH_BYTE(mTireMult) RF2_PUSH_BYTE(mMechFail)
    RF2_PUSH_BYTE(mAllowPitcrewPush) RF2_PUSH_BYTE(mRepeatShifts)
    RF2_PUSH_BYTE(mHoldClutch) RF2_PUSH_BYTE(mAutoReverse)
    RF2_PUSH_BYTE(mAlternateNeutral) RF2_PUSH_BYTE(mAIControl)
    RF2_PUSH_FLOAT(mManualShiftOverrideTime) RF2_PUSH_FLOAT(mAutoShiftOverrideTime)
    RF2_PUSH_FLOAT(mSpeedSensitiveSteering) RF2_PUSH_FLOAT(mSteerRatioSpeed)
#undef RF2_PUSH_BYTE
#undef RF2_PUSH_FLOAT
    if(lua_pcall(state_,1,1,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onPhysicsOptions]: ")+(e?e:"unknown"));lua_pop(state_,1);return;}
    if(lua_istable(state_,-1)) ReadPhysics(state_,-1,options);
    lua_pop(state_,1);
}

bool LuaEngine::ApplyWeather(double trackNodeSize, WeatherControlInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_) return false;
    lua_getglobal(state_,"onWeather");
    if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return false;}
    lua_newtable(state_);
    PushNumberField(state_,"mET",info.mET);
    PushNumberField(state_,"mCloudiness",info.mCloudiness);
    PushNumberField(state_,"mAmbientTempK",info.mAmbientTempK);
    PushNumberField(state_,"mWindMaxSpeed",info.mWindMaxSpeed);
    lua_pushboolean(state_,info.mApplyCloudinessInstantly);lua_setfield(state_,-2,"mApplyCloudinessInstantly");
    lua_createtable(state_,3,0);
    for(int r=0;r<3;++r){lua_createtable(state_,3,0);for(int c=0;c<3;++c){lua_pushnumber(state_,info.mRaining[r][c]);lua_rawseti(state_,-2,c+1);}lua_rawseti(state_,-2,r+1);}
    lua_setfield(state_,-2,"mRaining");
    PushNumberField(state_,"trackNodeSize",trackNodeSize);
    if(lua_pcall(state_,1,1,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onWeather]: ")+(e?e:"unknown"));lua_pop(state_,1);return false;}
    bool changed=false;
    if(lua_istable(state_,-1)){
        double v;
#define RF2_WREAD(member) v=info.member;if(ReadNumber(state_,-1,#member,v)){info.member=v;changed=true;}
        RF2_WREAD(mET) RF2_WREAD(mCloudiness) RF2_WREAD(mAmbientTempK) RF2_WREAD(mWindMaxSpeed)
#undef RF2_WREAD
        bool b=info.mApplyCloudinessInstantly;if(ReadBoolean(state_,-1,"mApplyCloudinessInstantly",b)){info.mApplyCloudinessInstantly=b;changed=true;}
        lua_getfield(state_,-1,"mRaining");
        if(lua_istable(state_,-1)){for(int r=0;r<3;++r){lua_rawgeti(state_,-1,r+1);if(lua_istable(state_,-1)){for(int c=0;c<3;++c){lua_rawgeti(state_,-1,c+1);if(lua_isnumber(state_,-1)){info.mRaining[r][c]=lua_tonumber(state_,-1);changed=true;}lua_pop(state_,1);}}lua_pop(state_,1);}}
        lua_pop(state_,1);
    }
    lua_pop(state_,1);
    return changed;
}

unsigned char LuaEngine::ApplyCamera(CameraControlInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_) return 0;
    lua_getglobal(state_,"onCameraControl");
    if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return 0;}
    lua_newtable(state_);
    PushIntegerField(state_,"mID",info.mID); PushIntegerField(state_,"mCameraType",info.mCameraType);
    lua_pushboolean(state_,info.mReplayActive);lua_setfield(state_,-2,"mReplayActive");
    PushIntegerField(state_,"mReplayCommand",info.mReplayCommand);
    lua_pushboolean(state_,info.mReplaySetTime);lua_setfield(state_,-2,"mReplaySetTime");
    PushNumberField(state_,"mReplaySeconds",info.mReplaySeconds);
    if(lua_pcall(state_,1,2,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onCameraControl]: ")+(e?e:"unknown"));lua_pop(state_,1);return 0;}
    unsigned char result=0;
    if(lua_istable(state_,-2)){
        lua_Integer n;
        if(ReadInteger(state_,-2,"mID",n))info.mID=(long)n;
        if(ReadInteger(state_,-2,"mCameraType",n))info.mCameraType=(long)n;
        if(ReadInteger(state_,-2,"mReplayCommand",n))info.mReplayCommand=(unsigned char)n;
        bool b;
        if(ReadBoolean(state_,-2,"mReplaySetTime",b))info.mReplaySetTime=b;
        if(ReadBoolean(state_,-2,"mReplayActive",b))info.mReplayActive=b;
        double d=info.mReplaySeconds;if(ReadNumber(state_,-2,"mReplaySeconds",d))info.mReplaySeconds=(float)d;
    }
    if(lua_isnumber(state_,-1))result=(unsigned char)lua_tointeger(state_,-1);
    lua_pop(state_,2);
    return result;
}

bool LuaEngine::ApplyMessage(MessageInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_) return false;
    lua_getglobal(state_,"onMessage");
    if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return false;}
    lua_newtable(state_);PushStringField(state_,"mText",info.mText);
    PushIntegerField(state_,"mDestination",info.mDestination);PushIntegerField(state_,"mTranslate",info.mTranslate);
    if(lua_pcall(state_,1,1,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onMessage]: ")+(e?e:"unknown"));lua_pop(state_,1);return false;}
    bool handled=false;
    if(lua_istable(state_,-1)){
        lua_getfield(state_,-1,"mText");if(lua_isstring(state_,-1)){strncpy_s(info.mText,sizeof(info.mText),lua_tostring(state_,-1),_TRUNCATE);handled=true;}lua_pop(state_,1);
        lua_Integer n;if(ReadInteger(state_,-1,"mDestination",n)){info.mDestination=(unsigned char)n;handled=true;}
        if(ReadInteger(state_,-1,"mTranslate",n)){info.mTranslate=(unsigned char)n;handled=true;}
    }
    lua_pop(state_,1);return handled;
}

bool LuaEngine::ApplyCommentary(CommentaryRequestInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if(!state_) return false;
    lua_getglobal(state_,"onCommentary");
    if(!lua_isfunction(state_,-1)){lua_pop(state_,1);return false;}
    lua_newtable(state_);PushStringField(state_,"mName",info.mName);
    PushNumberField(state_,"mInput1",info.mInput1);PushNumberField(state_,"mInput2",info.mInput2);PushNumberField(state_,"mInput3",info.mInput3);
    lua_pushboolean(state_,info.mSkipChecks);lua_setfield(state_,-2,"mSkipChecks");
    if(lua_pcall(state_,1,1,0)!=LUA_OK){const char*e=lua_tostring(state_,-1);Log(std::string("Lua callback error [onCommentary]: ")+(e?e:"unknown"));lua_pop(state_,1);return false;}
    bool handled=false;
    if(lua_istable(state_,-1)){
        lua_getfield(state_,-1,"mName");if(lua_isstring(state_,-1)){strncpy_s(info.mName,sizeof(info.mName),lua_tostring(state_,-1),_TRUNCATE);handled=true;}lua_pop(state_,1);
        double d;if(ReadNumber(state_,-1,"mInput1",d)){info.mInput1=d;handled=true;}
        if(ReadNumber(state_,-1,"mInput2",d)){info.mInput2=d;handled=true;}
        if(ReadNumber(state_,-1,"mInput3",d)){info.mInput3=d;handled=true;}
        bool b;if(ReadBoolean(state_,-1,"mSkipChecks",b)){info.mSkipChecks=b;handled=true;}
    }
    lua_pop(state_,1);return handled;
}

// ---------------------------------------------------------------------------
// v6: SDK access channels that were never wired to Lua (audit 2026-11-15
// against InternalsPlugin.hpp V01-V07). Same conventions as the existing
// writable callbacks: the current values arrive as a table, and returning
// a table applies the fields present in it.
// ---------------------------------------------------------------------------

namespace {
    bool ReadStringField(lua_State* L, int table, const char* key, char* dest, size_t destSize)
    {
        lua_getfield(L, table, key);
        const bool ok = lua_isstring(L, -1) != 0;
        if (ok) strncpy_s(dest, destSize, lua_tostring(L, -1), _TRUNCATE);
        lua_pop(L, 1);
        return ok;
    }
}

bool LuaEngine::InitCustomControl(CustomControlInfoV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    lua_getglobal(state_, "onInitCustomControl");
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return false; }
    // The game calls this repeatedly at startup; Lua is handed the
    // 1-based index of the control being requested and returns a table
    // { mUntranslatedName = "...", mRepeat = 0 } or nil when done.
    lua_pushinteger(state_, customControlCount_ + 1);
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onInitCustomControl]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return false;
    }
    bool ok = false;
    if (lua_istable(state_, -1)) {
        if (ReadStringField(state_, -1, "mUntranslatedName", info.mUntranslatedName, sizeof(info.mUntranslatedName)) ||
            ReadStringField(state_, -1, "name", info.mUntranslatedName, sizeof(info.mUntranslatedName))) {
            ok = true;
        }
        lua_Integer n = info.mRepeat;
        if (ReadInteger(state_, -1, "mRepeat", n)) info.mRepeat = (long)n;
    }
    lua_pop(state_, 1);
    if (ok) ++customControlCount_;
    return ok;
}

bool LuaEngine::ApplyPitMenu(PitMenuV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    if (!LuaCallWithTable(state_, "onPitMenu")) return false;
    PushIntegerField(state_, "mCategoryIndex", info.mCategoryIndex);
    PushStringField(state_, "mCategoryName", info.mCategoryName);
    PushIntegerField(state_, "mChoiceIndex", info.mChoiceIndex);
    PushStringField(state_, "mChoiceString", info.mChoiceString);
    PushIntegerField(state_, "mNumChoices", info.mNumChoices);
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onPitMenu]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return false;
    }
    bool changed = false;
    if (lua_istable(state_, -1)) {
        lua_Integer n;
        if (ReadInteger(state_, -1, "mCategoryIndex", n)) { info.mCategoryIndex = (long)n; changed = true; }
        if (ReadInteger(state_, -1, "mChoiceIndex", n)) { info.mChoiceIndex = (long)n; changed = true; }
    }
    lua_pop(state_, 1);
    return changed;
}

bool LuaEngine::ApplyTrackRules(TrackRulesV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    if (!LuaCallWithTable(state_, "onTrackRules")) return false;
    // Input-only fields.
    PushNumberField(state_, "mCurrentET", info.mCurrentET);
    PushIntegerField(state_, "mStage", (long)info.mStage);
    PushIntegerField(state_, "mPoleColumn", (long)info.mPoleColumn);
    PushIntegerField(state_, "mNumActions", info.mNumActions);
    const long numActions = (info.mAction && info.mNumActions > 0)
        ? (info.mNumActions < 512 ? info.mNumActions : 512) : 0;
    lua_createtable(state_, (int)numActions, 0);
    for (long i = 0; i < numActions; ++i) {
        const TrackRulesActionV01& a = info.mAction[i];
        lua_newtable(state_);
        PushIntegerField(state_, "mCommand", (long)a.mCommand);
        PushIntegerField(state_, "mID", a.mID);
        PushNumberField(state_, "mET", a.mET);
        lua_rawseti(state_, -2, (int)(i + 1));
    }
    lua_setfield(state_, -2, "mAction");
    PushIntegerField(state_, "mNumParticipants", info.mNumParticipants);
    lua_pushboolean(state_, info.mYellowFlagDetected); lua_setfield(state_, -2, "mYellowFlagDetected");
    lua_pushboolean(state_, info.mYellowFlagLapsWasOverridden); lua_setfield(state_, -2, "mYellowFlagLapsWasOverridden");
    lua_pushboolean(state_, info.mSafetyCarExists); lua_setfield(state_, -2, "mSafetyCarExists");
    lua_pushboolean(state_, info.mSafetyCarActive); lua_setfield(state_, -2, "mSafetyCarActive");
    PushIntegerField(state_, "mSafetyCarLaps", info.mSafetyCarLaps);
    PushNumberField(state_, "mSafetyCarThreshold", info.mSafetyCarThreshold);
    PushNumberField(state_, "mSafetyCarLapDist", info.mSafetyCarLapDist);
    PushNumberField(state_, "mSafetyCarLapDistAtStart", info.mSafetyCarLapDistAtStart);
    PushNumberField(state_, "mPitLaneStartDist", info.mPitLaneStartDist);
    PushNumberField(state_, "mTeleportLapDist", info.mTeleportLapDist);
    // Input/output fields.
    PushIntegerField(state_, "mYellowFlagState", info.mYellowFlagState);
    PushIntegerField(state_, "mYellowFlagLaps", info.mYellowFlagLaps);
    PushIntegerField(state_, "mSafetyCarInstruction", info.mSafetyCarInstruction);
    PushNumberField(state_, "mSafetyCarSpeed", info.mSafetyCarSpeed);
    PushNumberField(state_, "mSafetyCarMinimumSpacing", info.mSafetyCarMinimumSpacing);
    PushNumberField(state_, "mSafetyCarMaximumSpacing", info.mSafetyCarMaximumSpacing);
    PushNumberField(state_, "mMinimumColumnSpacing", info.mMinimumColumnSpacing);
    PushNumberField(state_, "mMaximumColumnSpacing", info.mMaximumColumnSpacing);
    PushNumberField(state_, "mMinimumSpeed", info.mMinimumSpeed);
    PushNumberField(state_, "mMaximumSpeed", info.mMaximumSpeed);
    PushStringField(state_, "mMessage", info.mMessage);
    const long numParts = (info.mParticipant && info.mNumParticipants > 0)
        ? (info.mNumParticipants < 256 ? info.mNumParticipants : 256) : 0;
    lua_createtable(state_, (int)numParts, 0);
    for (long i = 0; i < numParts; ++i) {
        const TrackRulesParticipantV01& p = info.mParticipant[i];
        lua_newtable(state_);
        PushIntegerField(state_, "mID", p.mID);
        PushIntegerField(state_, "mFrozenOrder", p.mFrozenOrder);
        PushIntegerField(state_, "mPlace", p.mPlace);
        PushNumberField(state_, "mYellowSeverity", p.mYellowSeverity);
        PushNumberField(state_, "mCurrentRelativeDistance", p.mCurrentRelativeDistance);
        PushIntegerField(state_, "mRelativeLaps", p.mRelativeLaps);
        PushIntegerField(state_, "mColumnAssignment", (long)p.mColumnAssignment);
        PushIntegerField(state_, "mPositionAssignment", p.mPositionAssignment);
        lua_pushboolean(state_, p.mAllowedToPit); lua_setfield(state_, -2, "mAllowedToPit");
        PushNumberField(state_, "mGoalRelativeDistance", p.mGoalRelativeDistance);
        PushStringField(state_, "mMessage", p.mMessage);
        lua_rawseti(state_, -2, (int)(i + 1));
    }
    lua_setfield(state_, -2, "mParticipant");
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onTrackRules]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return false;
    }
    bool changed = false;
    if (lua_istable(state_, -1)) {
        lua_Integer n; double d; bool b;
        if (ReadInteger(state_, -1, "mYellowFlagState", n)) { info.mYellowFlagState = (signed char)n; changed = true; }
        if (ReadInteger(state_, -1, "mYellowFlagLaps", n)) { info.mYellowFlagLaps = (short)n; changed = true; }
        if (ReadInteger(state_, -1, "mSafetyCarInstruction", n)) { info.mSafetyCarInstruction = (long)n; changed = true; }
        if (ReadNumber(state_, -1, "mSafetyCarSpeed", d)) { info.mSafetyCarSpeed = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mSafetyCarMinimumSpacing", d)) { info.mSafetyCarMinimumSpacing = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mSafetyCarMaximumSpacing", d)) { info.mSafetyCarMaximumSpacing = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mMinimumColumnSpacing", d)) { info.mMinimumColumnSpacing = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mMaximumColumnSpacing", d)) { info.mMaximumColumnSpacing = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mMinimumSpeed", d)) { info.mMinimumSpeed = (float)d; changed = true; }
        if (ReadNumber(state_, -1, "mMaximumSpeed", d)) { info.mMaximumSpeed = (float)d; changed = true; }
        if (ReadStringField(state_, -1, "mMessage", info.mMessage, sizeof(info.mMessage))) changed = true;
        lua_getfield(state_, -1, "mParticipant");
        if (lua_istable(state_, -1)) {
            for (long i = 0; i < numParts; ++i) {
                lua_rawgeti(state_, -1, (int)(i + 1));
                if (lua_istable(state_, -1)) {
                    TrackRulesParticipantV01& p = info.mParticipant[i];
                    if (ReadInteger(state_, -1, "mRelativeLaps", n)) { p.mRelativeLaps = (long)n; changed = true; }
                    if (ReadInteger(state_, -1, "mColumnAssignment", n)) { p.mColumnAssignment = (TrackRulesColumnV01)n; changed = true; }
                    if (ReadInteger(state_, -1, "mPositionAssignment", n)) { p.mPositionAssignment = (long)n; changed = true; }
                    if (ReadBoolean(state_, -1, "mAllowedToPit", b)) { p.mAllowedToPit = b; changed = true; }
                    if (ReadNumber(state_, -1, "mGoalRelativeDistance", d)) { p.mGoalRelativeDistance = d; changed = true; }
                    if (ReadStringField(state_, -1, "mMessage", p.mMessage, sizeof(p.mMessage))) changed = true;
                }
                lua_pop(state_, 1);
            }
        }
        lua_pop(state_, 1);
    }
    lua_pop(state_, 1);
    return changed;
}

bool LuaEngine::ApplyMultiSessionRules(MultiSessionRulesV01& info)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    if (!LuaCallWithTable(state_, "onMultiSessionRules")) return false;
    PushIntegerField(state_, "mSession", info.mSession);
    PushIntegerField(state_, "mSpecialSlotID", info.mSpecialSlotID);
    PushStringField(state_, "mTrackType", info.mTrackType);
    PushIntegerField(state_, "mNumParticipants", info.mNumParticipants);
    PushIntegerField(state_, "mNumQualSessions", info.mNumQualSessions);
    PushIntegerField(state_, "mNumRaceSessions", info.mNumRaceSessions);
    PushIntegerField(state_, "mMaxLaps", info.mMaxLaps);
    PushIntegerField(state_, "mMaxSeconds", info.mMaxSeconds);
    PushStringField(state_, "mName", info.mName);
    const long numParts = (info.mParticipant && info.mNumParticipants > 0)
        ? (info.mNumParticipants < 256 ? info.mNumParticipants : 256) : 0;
    lua_createtable(state_, (int)numParts, 0);
    for (long i = 0; i < numParts; ++i) {
        const MultiSessionParticipantV01& p = info.mParticipant[i];
        lua_newtable(state_);
        PushIntegerField(state_, "mID", p.mID);
        PushStringField(state_, "mDriverName", p.mDriverName);
        PushStringField(state_, "mVehicleName", p.mVehicleName);
        lua_createtable(state_, 16, 0);
        for (int ui = 0; ui < 16; ++ui) { lua_pushinteger(state_, p.mUpgradePack[ui]); lua_rawseti(state_, -2, ui + 1); }
        lua_setfield(state_, -2, "mUpgradePack");
        PushNumberField(state_, "mBestPracticeTime", p.mBestPracticeTime);
        PushIntegerField(state_, "mQualParticipantIndex", p.mQualParticipantIndex);
        lua_createtable(state_, 4, 0);
        for (int qi = 0; qi < 4; ++qi) { lua_pushnumber(state_, p.mQualificationTime[qi]); lua_rawseti(state_, -2, qi + 1); }
        lua_setfield(state_, -2, "mQualificationTime");
        lua_createtable(state_, 4, 0);
        for (int qi = 0; qi < 4; ++qi) { lua_pushnumber(state_, p.mFinalRacePlace[qi]); lua_rawseti(state_, -2, qi + 1); }
        lua_setfield(state_, -2, "mFinalRacePlace");
        lua_createtable(state_, 4, 0);
        for (int qi = 0; qi < 4; ++qi) { lua_pushnumber(state_, p.mFinalRaceTime[qi]); lua_rawseti(state_, -2, qi + 1); }
        lua_setfield(state_, -2, "mFinalRaceTime");
        lua_pushboolean(state_, p.mServerScored); lua_setfield(state_, -2, "mServerScored");
        PushIntegerField(state_, "mGridPosition", p.mGridPosition);
        lua_rawseti(state_, -2, (int)(i + 1));
    }
    lua_setfield(state_, -2, "mParticipant");
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onMultiSessionRules]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return false;
    }
    bool changed = false;
    if (lua_istable(state_, -1)) {
        lua_Integer n; bool b;
        if (ReadInteger(state_, -1, "mNumQualSessions", n)) { info.mNumQualSessions = (long)n; changed = true; }
        if (ReadInteger(state_, -1, "mNumRaceSessions", n)) { info.mNumRaceSessions = (long)n; changed = true; }
        if (ReadInteger(state_, -1, "mMaxLaps", n)) { info.mMaxLaps = (long)n; changed = true; }
        if (ReadInteger(state_, -1, "mMaxSeconds", n)) { info.mMaxSeconds = (long)n; changed = true; }
        if (ReadStringField(state_, -1, "mName", info.mName, sizeof(info.mName))) changed = true;
        lua_getfield(state_, -1, "mParticipant");
        if (lua_istable(state_, -1)) {
            for (long i = 0; i < numParts; ++i) {
                lua_rawgeti(state_, -1, (int)(i + 1));
                if (lua_istable(state_, -1)) {
                    MultiSessionParticipantV01& p = info.mParticipant[i];
                    if (ReadBoolean(state_, -1, "mServerScored", b)) { p.mServerScored = b; changed = true; }
                    if (ReadInteger(state_, -1, "mGridPosition", n)) { p.mGridPosition = (long)n; changed = true; }
                }
                lua_pop(state_, 1);
            }
        }
        lua_pop(state_, 1);
    }
    lua_pop(state_, 1);
    return changed;
}

bool LuaEngine::GetCustomVariable(long index, CustomVariableV01& var)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return false;
    lua_getglobal(state_, "onGetCustomVariable");
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return false; }
    // The game enumerates the plugin's custom variables at startup;
    // Lua gets the 1-based index and returns
    // { mCaption = "...", mNumSettings = n, mCurrentSetting = i }
    // (mNumSettings = 0 means a free-form string variable), or nil
    // when there are no more variables.
    lua_pushinteger(state_, index + 1);
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onGetCustomVariable]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return false;
    }
    bool ok = false;
    if (lua_istable(state_, -1)) {
        ok = ReadStringField(state_, -1, "mCaption", var.mCaption, sizeof(var.mCaption));
        lua_Integer n;
        if (ReadInteger(state_, -1, "mNumSettings", n)) var.mNumSettings = (long)n;
        if (ReadInteger(state_, -1, "mCurrentSetting", n)) var.mCurrentSetting = (long)n;
    }
    lua_pop(state_, 1);
    return ok;
}

void LuaEngine::GetCustomVariableSetting(CustomVariableV01& var, long index, CustomSettingV01& setting)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    lua_getglobal(state_, "onGetCustomVariableSetting");
    if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); return; }
    lua_pushstring(state_, var.mCaption);
    lua_pushinteger(state_, index + 1);
    if (lua_pcall(state_, 2, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onGetCustomVariableSetting]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return;
    }
    if (lua_isstring(state_, -1)) {
        strncpy_s(setting.mName, sizeof(setting.mName), lua_tostring(state_, -1), _TRUNCATE);
    }
    lua_pop(state_, 1);
}

void LuaEngine::AccessCustomVariable(CustomVariableV01& var)
{
    std::lock_guard<std::recursive_mutex> lock(luaMutex_);
    if (!state_) return;
    if (!LuaCallWithTable(state_, "onCustomVariable")) return;
    PushStringField(state_, "mCaption", var.mCaption);
    PushIntegerField(state_, "mNumSettings", var.mNumSettings);
    PushIntegerField(state_, "mCurrentSetting", var.mCurrentSetting);
    if (lua_pcall(state_, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(state_, -1);
        Log(std::string("Lua callback error [onCustomVariable]: ") + (e ? e : "unknown"));
        lua_pop(state_, 1);
        return;
    }
    // Notification of the current value; if Lua returns a table with a
    // different mCurrentSetting, adopt it.
    if (lua_istable(state_, -1)) {
        lua_Integer n;
        if (ReadInteger(state_, -1, "mCurrentSetting", n)) var.mCurrentSetting = (long)n;
    }
    lua_pop(state_, 1);
}
