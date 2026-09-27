#pragma once
#include <string>

namespace vegas::probe {
// Development probes read from `path` (one command per line) on the main thread.
// `console` runs a line through xNVSE's console interface (null: command unavailable).
// `reload` is set by the `reload` command: the plugin loader then reloads the core.
void init(const std::wstring& path, bool (*console)(const char*, void*), volatile bool* reload = nullptr);
void poll();
// Before the core is unloaded: removes the debug watch and releases injected keys.
void shutdown();
// Runs one console line (false if xNVSE's console interface is unavailable).
bool console(const char* line);
} // namespace vegas::probe
