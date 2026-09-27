# Rendering and bridge architecture

Reviewed on **3 October 2026**, VegasCraft **0.2.2**, protocol **20**. The [movement-cube document](history/MOVEMENT-CUBE.md) describes the initial probe, not the production renderer.

## Process and module ownership

| Component | Responsibility |
| --- | --- |
| `native/Plugin.cpp` → `VegasCraft.dll` | Permanent xNVSE loader, bridge ownership/heartbeat, background Minecraft startup, core loading and message forwarding |
| `native/Core.cpp` → `VegasCraftCore.dll` | Session, input, gameplay, engine hooks, collision export, rendering and interface |
| `fabric/src/main/java/dev/vegascraft/` | Link, integrated-server gameplay, inventory links, native actor hitboxes, digging and world data |
| `fabric/src/client/java/dev/vegascraft/client/` | Client input, mirror-world startup, mesh/frame export, icons, avatar/hands and Pip-Boy pages |

`native/CoreAbi.h` defines loader/core ABI **1**, including bridge size, paths, console callback, generation, and reload request. The loader loads a copy from a `live` subdirectory so its installed source DLL can be replaced. Shutdown releases core resources and restores hooks; a new generation creates a new collision epoch. See [DEVELOPMENT.md](DEVELOPMENT.md) for restart boundaries.

## Shared memory and coordinates

`protocol/skycraft_protocol.h` and `fabric/.../link/Proto.java` define **v20**, derived from SkyCraft. `TelemetryBridge.java` mirrors the state subset for cross-process testing. All three must agree. The default VegasCraft mapping is **`Local\VegasCraft_v1`**; the inherited header's `Local\SkyCraft_v1` constant is not the native runtime default.

The mapping contains headers/heartbeats, seqlocked player/arm state, water grid, input/gameplay event rings, actor/world-entity tables, a 32 MiB collision ring, three maximum-3840×2160 RGBA overlay slots, and a 64 MiB render ring. Total size is approximately **191 MiB**. The x86 host maps the low state/collision region separately, reserves the render-ring view at startup when possible, and maps overlay pixels as needed. Java maps the complete layout.

At **70 native units per block**:

```text
Minecraft x = New Vegas x / 70
Minecraft y = New Vegas z / 70
Minecraft z = -New Vegas y / 70
Minecraft yaw = New Vegas heading in degrees + 180
```

New Vegas publishes load/world changes and teleport sequences. Minecraft acknowledges teleports before it owns movement; the native session interpolates Minecraft feet and updates the player/camera. Menus, loads, and furniture change input/movement ownership. This does not merge the save systems.

## World geometry

`client/render/WorldExporter.java` exports section meshes, atlas/texture updates, avatar/scene models, particles, hands, lights, solid cells, and dug cells through the render ring. `native/WorldRender.cpp` consumes these messages and maintains D3D9 resources.

The renderer hooks two world-pass call sites, applies the Minecraft-driven camera and held-weapon transforms, updates cuts/lights before native rendering, and draws Minecraft geometry once after the native world pass. World geometry uses depth testing; opaque sections write depth, translucent sections blend without depth writes. The shader combines Minecraft light levels/face shading with New Vegas sun, ambient light, and fog. Entity meshes receive face normals for daylight shading. Exported entities also drive dropped items/projectiles, mining cracks, selection outlines, and contact shadows.

Sections outside the view frustum are skipped (bounding-sphere test). The atlas gets a driver-built mip chain when the driver supports it (`D3DUSAGE_AUTOGENMIPMAP`): point filtering up close, blended at a distance. Block light is tinted by the four strongest nearby emitters' colors instead of one warm tone. First-person hands take New Vegas' ambient plus part of its sun as a flat light (they live in camera space), so they darken with the scene. A soft blob shadow is drawn under the third-person avatar. The probe `skydump` logs the Sky object's floats to locate New Vegas' fog distances, which are still fixed constants in `environment()`.

Textures use point filtering. Uploads use small staging strips to limit x86 memory pressure, with padded textures when needed. Device reset releases default-pool resources; textures return when Minecraft resends them. Draw paths save/restore D3D state.

## Hands, native weapons, and the avatar

Native ranged weapons retain New Vegas firing/aiming/reloading and skeletal animation. Minecraft can fit arms to exported native poses and aim FOV; the third-person avatar holds native weapons. Native melee models use Minecraft's held-item pose/swing path.

The first-person hands pass uses a separate depth surface populated from native weapon triangles, including skinned geometry, to occlude fingers behind weapons. It runs before the interface in the present callback. **Minecraft first-person arms with a native ranged weapon are hidden by default.** Optional `showArms 1` tuning is in [CONFIGURATION.md](CONFIGURATION.md). Pip-Boy hands follow their own visible path.

## HUD and Pip-Boy

`FrameExporter.java` sends interface frames through the overlay triple buffer. `native/Overlay.cpp` uploads/presents them; a virtual cursor receives native input. HUD suppression hides native health/AP/reticle during Minecraft control while retaining compass, messages, and enemy health; native menus restore ownership.

The Pip-Boy model is generated by `tools/gen_pipboy.py`. Native arm/screen state places it and supplies projected screen corners. Minecraft draws STATS, ITEMS, DATA, and crafting pages; `native/Homography.h` maps the page into the screen quadrilateral and inversely maps clicks. The page waits for opening motion to settle. F1/F2/F3 control section buttons, F4 switches ITEMS, and wheel input animates the knob while scrolling.

`native/PipData.cpp` publishes stats, quests, notes, radio information, and map data while the Pip-Boy is up. `native/Inventory.cpp` supplies inventory snapshots separately. `NvIcons.java` builds local resources from the user's BSA archives/loose interface files for item/stat icons and map images. Extracted game assets are not shipped.

STATS and DATA offer **NV** fallback. DATA's local map and unsupported note content need the native interface; the radio is tuned from the page (the `PipboyRadio` script command). Map travel and quest tracking send events to New Vegas; the native game decides their effects.

## Terrain, collision, and lights

`native/Collision.cpp` exports geometry and terrain columns. Minecraft's player uses smooth triangle collision; native actor proxies are hitboxes, while New Vegas renders real characters. Columns feed `NvLand`, `NvDecorator`, `NvStructures`, and `NvSpawner`. The Mojave is the overworld; other native spaces currently share `vegascraft:elsewhere`.

Minecraft saves dug cells with chunk data. `SkyDigClient` sends them after epoch/world changes. `native/Dig.cpp` tracks holes; `DigMesh.cpp` clips landscape triangles and interpolates vertex/texture weights, restoring owned meshes when cleared. `DigPhysics.cpp` drops native character contacts with removed ground and adjusts rescue/falling while Minecraft owns the player. `NpcBlocks.cpp` pushes native actors sideways out of placed blocks; it does not provide block-floor support. Arbitrary-object digging and grass removal remain incomplete.

`native/BlockLights.cpp` clusters emitters and registers native point lights with exported colors, flame/lava behavior, and brightness tuning. Lights update before native world rendering, retain stable identities, and are removed across emitter/scene changes. Earlier real-game tuning showed weak landscape response despite lighting on objects; visual checks remain necessary.

## Diagnostics and evidence

The log reports world-pass hooks, section/atlas availability, render-message counts, and session/core state. `profile` reports frame/component timings. See [DEVELOPMENT.md](DEVELOPMENT.md) for probes and [VALIDATION.md](VALIDATION.md) for coverage.

`tests/render_preview.cpp` still exercises `MotionPreview.cpp`, the old cube. `MotionPreview.cpp` is linked into that fixture, not the production core; **`[Preview] Enabled=1` is not a supported current setting**. Current rendering, hands, Pip-Boy layout, light response, and landscape seams require the [in-game checklist](TEST-0.2.2.md).
