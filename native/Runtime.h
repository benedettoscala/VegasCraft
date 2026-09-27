#pragma once
#include "Bridge.h"
#include <atomic>
#include <cstdint>

namespace vegas {
// State shared between the per-frame session update, the input hooks and the renderer hooks.
struct Runtime {
    Bridge* bridge = nullptr;
    // Minecraft is connected, in its world, has acknowledged our last teleport and New Vegas
    // isn't loading: Minecraft's player drives New Vegas' player.
    std::atomic<bool> puppeting{false};
    // The same, or waiting for Minecraft to arrive after a teleport: New Vegas' own controls
    // don't move its player either way.
    std::atomic<bool> minecraftOwnsPlayer{false};
    // A Minecraft GUI screen (inventory, chat, ...) is open: the mouse moves its cursor.
    std::atomic<bool> mcScreenOpen{false};
    // A New Vegas menu (Pip-Boy, dialogue, console, pause, loading, ...) owns input.
    std::atomic<bool> nvMenuOpen{false};
    std::atomic<bool> nvInGame{false};
    // Development: a New Vegas key held by the plugin (DirectInput scancode) until the tick count.
    std::atomic<std::uint32_t> injectDik{0};
    std::atomic<std::uint64_t> injectUntil{0};
    std::atomic<std::uint64_t> injectMouseUntil{0};  // probe nvclick: New Vegas' left button held until then
    std::atomic<int> injectMouseButton{0};            // which button (0 left, 1 right)
    // A New Vegas weapon is wielded: New Vegas gets the mouse buttons and R (fire, aim, reload).
    std::atomic<bool> nvWeapon{false};
    std::atomic<bool> exportLand{true};
    // The Pip-Boy is up and drawn Minecraft style: the overlay stays on, open over its screen's
    // corners (back buffer pixels, clockwise from the top left). Main thread.
    bool pipBoyUp = false;
    bool pipBoyHoleValid = false;
    float pipBoyHole[8]{};
    // The Pip-Boy shows its Minecraft page (F4): Minecraft's inventory and crafting on its screen.
    bool pipBoyMcPage = false;
    // Where that page is laid out in Minecraft's frame (x0 y0 x1 y1, back buffer pixels) and the share of
    // the Pip-Boy's screen it covers; the overlay warps the rectangle onto the screen as it moves.
    float pipBoyPage[4]{};
    bool pipBoyPageValid = false;   // VegasCraft.ini [World] bExportLand: the ground under Minecraft chunks (kColLand)
    std::atomic<bool> showNvArms{false};  // probe nvarms: keep New Vegas' own arms visible (lining up Minecraft's)
    // Native furniture animations own movement, but G/H retain their interaction mapping.
    std::atomic<bool> nativeInteraction{false};
    std::atomic<bool> mcInWorld{false};
    // Minecraft's crosshair should be drawn (first person, no screen open).
    std::atomic<bool> mcCrosshair{false};
    std::atomic<int> mcGuiScale{0};

    // Look direction in Minecraft degrees, integrated from raw mouse input (main thread).
    float yaw = 0, pitch = 0;
    bool lookInitialized = false;
    float sensitivity = 0.5f;

    // Virtual Minecraft cursor in overlay pixels while a Minecraft screen is open.
    std::atomic<int> cursorX{0}, cursorY{0};
    std::atomic<int> viewportW{1280}, viewportH{720};

    // Feet the camera follows this frame (Minecraft coordinates); main thread.
    double feetX = 0, feetY = 0, feetZ = 0;
    bool feetValid = false;
};
Runtime& state();
} // namespace vegas
