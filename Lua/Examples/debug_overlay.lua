-- Debug overlay: displays the broad telemetry/scoring subset exposed by this build.
-- Install by copying this file to Plugins/Lua/Scripts/ (use only one overlay script at a time).
local telem, scoring, graphics = {}, {}, {}
local function fmt(v, f) if type(v)=="number" then return string.format(f or "%.2f", v) end return tostring(v) end
function onTelemetry(d) telem = d end
function onScoring(d) scoring = d end
function onGraphics(d) graphics = d end
function onRender(screen)
    local y, x = 18, 18
    local function line(s, r,g,b) overlay.text(x,y,s,r or 230,g or 240,b or 255,15); y=y+18 end
    line("rF2 LUA DEBUG | SDK V07",255,210,90)
    line("Screen: "..fmt(screen.mWidth).." x "..fmt(screen.mHeight))
    line("Vehicle: "..tostring(telem.mVehicleName or "-").." / Track: "..tostring(telem.mTrackName or "-"))
    line("ID "..fmt(telem.mID).."  ET "..fmt(telem.mElapsedTime).."  Lap "..fmt(telem.mLapNumber))
    line("RPM "..fmt(telem.mEngineRPM,"%.0f").." / "..fmt(telem.mEngineMaxRPM,"%.0f").."  Gear "..fmt(telem.mGear).."  Clutch RPM "..fmt(telem.mClutchRPM,"%.0f"))
    line("Fuel "..fmt(telem.mFuel).." / "..fmt(telem.mFuelCapacity).."  Water "..fmt(telem.mEngineWaterTemp).."C  Oil "..fmt(telem.mEngineOilTemp).."C")
    line("Input T/B/S/C "..fmt(telem.mUnfilteredThrottle).." / "..fmt(telem.mUnfilteredBrake).." / "..fmt(telem.mUnfilteredSteering).." / "..fmt(telem.mUnfilteredClutch))
    local v=telem.mLocalVel or {}; line("Local velocity XYZ "..fmt(v.x).." / "..fmt(v.y).." / "..fmt(v.z))
    local p=telem.mPos or {}; line("World position XYZ "..fmt(p.x).." / "..fmt(p.y).." / "..fmt(p.z))
    line("Sector "..fmt(telem.mCurrentSector).."  Boost "..fmt(telem.mTurboBoostPressure).."  Rear bias "..fmt(telem.mRearBrakeBias))
    for i,w in ipairs(telem.mWheel or {}) do
        local t=w.mTemperature or {}
        line(string.format("W%d RH %.3f  Brake %.1f  Press %.1f  Wear %.3f  Temp %.1f/%.1f/%.1f",i,w.mRideHeight or 0,w.mBrakeTemp or 0,w.mPressure or 0,w.mWear or 0,t[1] or 0,t[2] or 0,t[3] or 0))
    end
    line("Session "..fmt(scoring.mSession).." Phase "..fmt(scoring.mGamePhase).." ET "..fmt(scoring.mCurrentET))
    line("Vehicles "..fmt(scoring.mNumVehicles).."  Camera "..fmt(graphics.mCameraType).."  View ID "..fmt(graphics.mID))
end
