#pragma once
#include <string>

namespace vegas::launcher {
// At plugin load: queues Minecraft startup without blocking NVSE/Windows DLL initialization.
void startMinecraft(const std::wstring& gameDir, const std::wstring& ini);
// A Minecraft with the VegasCraft mod is running (it holds a named mutex from early on).
bool minecraftRunning();
} // namespace vegas::launcher
