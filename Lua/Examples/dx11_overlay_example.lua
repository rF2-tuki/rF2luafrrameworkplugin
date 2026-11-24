-- Example: Lua-defined DX11 in-game overlay.
-- Put this file in the plugin's Lua/Scripts directory.
-- NOTE (fix): this file used to live in Scripts by default, where it was
-- loaded after debug_overlay.lua and silently overwrote its onTelemetry /
-- onRender, so the debug overlay never appeared. Overlay examples are now
-- all in Examples; copy exactly one of them into Scripts.
local latest = {}

function onTelemetry(data)
    latest.rpm = data.mEngineRPM or 0
    latest.gear = data.mGear or 0
    latest.fuel = data.mFuel or 0
end

function onRender(screen)
    overlay.text(30, 30, string.format("RPM  %5.0f", latest.rpm or 0), 255, 255, 255, 24)
    overlay.text(30, 60, string.format("GEAR %d", latest.gear or 0), 255, 220, 80, 22)
    overlay.text(30, 88, string.format("FUEL %.1f L", latest.fuel or 0), 120, 220, 255, 20)
    if screen and screen.mWidth then
        overlay.text(30, screen.mHeight - 42, "rF2 Lua DX11 overlay (development)", 180, 180, 180, 16)
    end
end

-- SDK-writable callback example:
-- Only takes effect when the simulator invokes CheckHWControl for this name.
function onCheckHWControl(name, currentValue)
    return currentValue -- keep the game's current value
end
