-- Race overlay: compact driving display using telemetry and scoring callbacks.
-- Install by copying this file to Plugins/Lua/Scripts/ (use only one overlay script at a time).
local t, s = {}, {}
local function n(v, f) if type(v)=="number" then return string.format(f or "%.1f",v) end return "--" end
function onTelemetry(data) t=data end
function onScoring(data) s=data end
function onRender(screen)
    local x,y=screen.mWidth-250,24
    local function line(str,r,g,b,size) overlay.text(x,y,str,r or 255,g or 255,b or 255,size or 22); y=y+27 end
    line("RACE DATA",255,210,70,18)
    line("RPM  "..n(t.mEngineRPM,"%.0f"),255,255,255,26)
    line("GEAR "..n(t.mGear,"%.0f"),255,255,255,26)
    local vel=t.mLocalVel or {}; line("SPEED "..n(math.sqrt((vel.x or 0)^2+(vel.y or 0)^2+(vel.z or 0)^2)*3.6,"%.1f").." km/h",255,255,255,22)
    line("FUEL  "..n(t.mFuel,"%.1f").." L",150,220,255,20)
    line("WATER "..n(t.mEngineWaterTemp,"%.0f").." C",150,220,255,18)
    line("OIL   "..n(t.mEngineOilTemp,"%.0f").." C",150,220,255,18)
    local wheels=t.mWheel or {}
    for i=1,4 do local w=wheels[i] or {}; local temp=w.mTemperature or {}; line(string.format("T%d %.0f / %.0f / %.0f C",i,(temp[1] or 0)-273.15,(temp[2] or 0)-273.15,(temp[3] or 0)-273.15),220,220,220,15) end
    line("LAP "..n(t.mLapNumber,"%.0f").."  POS "..n((function() for _,v in ipairs(s.mVehicle or {}) do if v.mIsPlayer then return v.mPlace end end end)(),"%.0f"),255,210,70,18)
end
