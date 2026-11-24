#pragma once

// Shared diagnostic logger (definition in Plugin.cpp). Writes to
// Lua\lua_plugin.log next to the plugin DLL. Unlike the Lua engine's
// log, it does not depend on the engine having started, so it works
// from any callback, any plugin instance, and any thread.
void DiagLog(const char* format, ...);
