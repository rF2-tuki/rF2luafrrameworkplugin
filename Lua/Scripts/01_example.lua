-- rFactor 2 Lua Framework - writable SDK callbacks
-- SDK member names (m-prefixed) are intentionally preserved.

function onStartup()
    rf2.log("Lua framework started: " .. rf2.version)
end

function onTelemetry(data)
    -- Read-only telemetry. C++ SDK names are available as-is.
    -- Example: data.mEngineRPM, data.mFuel, data.mLocalVel.z (vector table support
    -- nested vector fields such as data.mLocalVel.z are also available).
    if data.mEngineRPM > 0 then
        -- Avoid logging every frame.
    end
end

-- Return a number to take over Force Feedback.
-- Return nil to leave the game's original value unchanged.
function onForceFeedback(gameForceValue)
    return gameForceValue
end

-- Return a number to take over the named control.
-- Return nil for controls this script does not handle.
function onCheckHWControl(controlName, gameValue)
    -- Example:
    -- if controlName == "LookLeft" then return 0 end
    return nil
end

-- SDK V02: return a table containing only the PhysicsOptions fields to change.
-- Values not returned remain unchanged.
function onPhysicsOptions(options)
    -- options.mTractionControl = 0 -- 0..3
    -- options.mAntiLockBrakes = 0  -- 0..2
    return options
end

-- SDK V03: return camera table and result code (0..3).
-- Return nil/no callback to leave camera handling untouched.
function onCameraControl(info)
    return nil, 0
end

-- SDK V03: return a table to display a message; nil means no message.
function onMessage(info)
    return nil
end

-- SDK V01: return a table to request a commentary event; nil means no event.
function onCommentary(info)
    return nil
end

-- SDK V06: return weather table to apply changes; nil means no change.
function onWeather(info)
    -- info.mCloudiness = 0.25
    -- info.mAmbientTempK = 293.15
    -- info.mWindMaxSpeed = 5.0
    -- info.mET = desired elapsed time
    -- info.mRaining is a 3x3 Lua array (indices 1..3)
    return nil
end

function onStartSession()
    rf2.log("Session started")
end

function onEnterRealtime()
    rf2.log("Entered realtime")
end

function onExitRealtime()
    rf2.log("Exited realtime")
end

function onEndSession()
    rf2.log("Session ended")
end

function onShutdown()
    rf2.log("Lua framework stopped")
end
