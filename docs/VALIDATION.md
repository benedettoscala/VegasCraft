# Validation — 3 October 2026

This report describes **0.2.2**, bridge protocol **20**, including the Pip-Boy data regression fix following the documentation review at `7f12069`. The 0.2.2 rendering changes (frustum culling, atlas mips, tinted block light, lit hands, avatar blob shadow) were observed once in a live session (night and day, first and third person, one torch); fog distances and the colored-light tint were not checked. It replaces the initial movement-cube status report, preserved in [history](history/VALIDATION-2026-10-01.md). Builds and isolated fixtures do not establish correct visuals in the real games.

## Checks performed for this documentation update

| Check | Result | Scope |
| --- | --- | --- |
| `tools/Build-Native.ps1` | Pass | Release x86 configuration/build; all five CTest cases executed successfully |
| `plugin_lifecycle` | Pass | Loader exports, executable/xNVSE compatibility, messages, state publication, loads, cell changes, shutdown, duplicate hosts, mapping lifetime/restart |
| `homography` | Pass | Pip-Boy corners, inverse mapping round trips, inset, inner layout rectangle |
| `render_fixture` | Pass | Historical movement-cube renderer, pixel readback, camera displacement, load reset, open-scene handling, D3D state restoration |
| `gameplay_fixture` | Pass | Dig packets, clipping/materials, mesh ownership/restoration, collision export, character-contact filtering, camera calculations, light lifecycle/clustering, furniture ownership transitions |
| `pipboy_data` | Pass | Production snapshot readers: inaccessible and unrelated icon pointers, page boundaries, inventory count/world/icon, STATS, quest name/tracking/objective wire payload, and Pip-Boy open/closed gating |
| `tools/Build-Fabric.ps1` | Pass | Minecraft 26.3, Java 25, Fabric Loader 0.19.5, Fabric API 0.161.0+26.3; Gradle rebuilt versioned artifacts and executed all 21 tests |
| Fabric test reports | 21 passed; no failures, errors, or skipped tests | 9 `SkyRayTest` cases and 12 `TriColliderTest` cases; reports dated 3 October 2026 |
| `tools/test_bridge.py` | Pass | Production Java telemetry bridge in a separate x64 JVM against the x86 host: bidirectional state, host shutdown, incompatible version rejection, truncated mapping rejection |

Reproduction commands are in [DEVELOPMENT.md](DEVELOPMENT.md). The Python test validates the state/telemetry path, not every ring, gameplay packet, or Fabric screen. The renderer fixture does not instantiate `WorldRender.cpp` or `Overlay.cpp`. Homography tests validate transform math, not visible Pip-Boy layout.

## Pip-Boy regression investigation

The installed game's existing 3 October log recorded `inventory: faulted reading the player's inventory` and `pipdata: faulted reading the statistics`. Speculative icon scanning treated unrelated form fields as text pointers; after the pointer-check optimization, an inaccessible candidate aborted the enclosing snapshot. The new production-reader fixture failed before the fix because no inventory packet was published and passed after the fix. Icon scanning now copies only readable memory and skips invalid optional candidates while retaining the existing per-form caches.

The quest fixture confirms publication of a tracked active quest with its name and objective. It does not establish the cause of the user's live quest-screen symptom, which requires an in-game check. The corrected core was deployed to the installed New Vegas directory with matching SHA-256; New Vegas was closed, so there is no live confirmation of the corrected screens.

## Current implementation established from source

- The plugin has a permanent loader (`VegasCraft.dll`) and reloadable core (`VegasCraftCore.dll`). Both are built and packaged. The loader owns the bridge, launcher, and xNVSE message forwarding.
- C++ protocol, production Fabric `Proto`, and Java telemetry helper declare **20**. The runtime mapping remains `Local\VegasCraft_v1`.
- Minecraft exports block meshes, atlas/texture updates, avatar/scene geometry, hands, lights, solid cells, and dug cells. New Vegas draws world geometry in its native world pass and presents hands/interface separately.
- Movement, smooth player collision, actor hitboxes/combat, health feedback, native ranged/melee weapons, NPC block push-out, furniture handoff, and F5 cameras are implemented.
- New Vegas owns its inventory. Minecraft holds hotbar links and receives native counts/data; native hotkeys fill only free Minecraft slots. Pip-Boy link placement and equip/use/drop actions are implemented.
- Pip-Boy STATS, ITEMS, and DATA use Minecraft pages. STATS includes status, S.P.E.C.I.A.L., skills, perks, and general statistics. DATA includes quests/tracking, text notes, a tunable radio, and the world map with fast travel. The local map uses the native screen. F4 in ITEMS opens Minecraft inventory and 3x3 crafting.
- Native terrain export drives Minecraft features and an added mob spawner. The Mojave maps to the overworld; other worldspaces/interiors share `vegascraft:elsewhere`.
- Exterior mesh cuts, dug-cell persistence in Minecraft, collision re-export, and native character-contact filtering are implemented. Object destruction and native support on Minecraft floors remain incomplete.
- Current performance changes reduce repeated engine walks, read Pip-Boy data while it is open, and expose component timing through the `profile` probe. No controlled frame-rate benchmark was performed here.

## Earlier in-game evidence

The previous README recorded tester confirmation of block rendering, the interface overlay, combat, and health, and actual-process verification of xNVSE/Prism startup. Earlier light tuning recorded visible illumination on native rocks/objects and weak landscape response; the observation is also retained in `native/BlockLights.cpp`.

The historical report records actual xNVSE loading, heartbeats, loaded-world telemetry, a visible D3D9 cube, Minecraft-driven cube motion, and anchor reset on quickload. These results belong to the earlier milestone and do not validate all current features.

No real-game session was launched, save loaded, screenshot captured, or multiplayer run performed during this documentation update.

## Remaining verification and known limits

Use the [in-game checklist](TEST-0.2.2.md) to establish evidence for:

1. All Pip-Boy data/actions, scrolling, held-click behavior, hotbar links, crafting, tracking, map zoom/pan/travel, and native fallback.
2. Native gunfire, aim/reload, melee, mob/block hits, weapon switching, third-person models, and default hidden first-person arms. Check optional arms against rigid and skinned weapon depth.
3. Connection camera handoff, F5, wall proximity, HUD restoration, furniture interaction, loads and cell transitions.
4. Landscape seams/weights, holes near cell boundaries, native actors entering holes, deep-hole movement, filled holes, and replay after loads/reconnect/core reload.
5. Light placement/removal, tint/intensity, native terrain response, scene changes, and device resets.
6. Trees/decorations/structures/mobs, the mob setting, worldspace separation, and render/memory cost.
7. Core replacement/recovery, repeated loads, and performance with Pip-Boy closed/open and native weapons equipped.
8. Multiplayer joining/leaving, host-owned destruction, actors/combat, and separate native saves.

Known incomplete behavior includes native NPC support on Minecraft floors, arbitrary-object digging, grass removal, Minecraft local-map rendering and full isolation of non-Mojave spaces. Native saves and the Minecraft world are separate: loading an older native save does not rewind Minecraft blocks, dug cells, or Minecraft items.

## Historical crash investigation

The 1 October report describes a machine-specific `msmpeg2ac3dec.dll` loading crash, decoder-block integration tests, and a separate workaround plugin. It was not re-investigated here and is not a bundled dependency. Exact events, source revision, and archive digest remain in the [historical report](history/VALIDATION-2026-10-01.md).
