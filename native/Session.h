#pragma once
#include "Bridge.h"

namespace vegas::session {
// New Vegas' per-frame half of the link (main thread, from xNVSE's main-loop message).
// `generation` counts hot reloads of the core (0 at game start).
void start(Bridge* bridge, std::uint32_t generation = 0);
void frame();
void stop();
// xNVSE lifecycle messages.
void onPreLoad();
void onLoaded(bool ok);
void onMainMenu();
} // namespace vegas::session
