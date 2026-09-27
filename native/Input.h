#pragma once
#include <windows.h>

namespace vegas::input {
// Hooks DirectInput's device state reads (keyboard and mouse) so Minecraft gets New Vegas'
// input while it drives the player. Safe to call more than once; false if it couldn't hook.
bool install();
// Before the core is unloaded: gives the game window its own procedure back (the DirectInput
// slots go back with hook::restoreAll). False if something else subclassed the window since.
bool uninstall();
// Per frame: closes the Pip-Boy's Minecraft page (F4) when the Pip-Boy or Minecraft's screen closes.
void pipBoyPage();
// The Pip-Boy's menu button (0 STATS, 1 ITEMS, 2 DATA), as F1-F3 press it (the probe's pipsec).
void selectSection(int section);
// Hides the Pip-Boy's page and shows New Vegas' own screen of a menu (0 STATS, 1 ITEMS, 2 DATA).
void nativeView(int section);
// Moves Minecraft's screen cursor to a window pixel (the probe's cursor): through the Pip-Boy page's mapping when it is open.
void setCursor(int x, int y);
// Mouse movement accumulated since the last call (main thread).
void consumeLook(float& dx, float& dy);
// Tells Minecraft to release every held key and button.
void releaseAll();
// Typed characters for Minecraft's text fields come from the game window's WM_CHAR.
void attachWindow(HWND window);
} // namespace vegas::input
