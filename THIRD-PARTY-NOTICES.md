# Third-party sources

VegasCraft is an experimental port project, independent of Mojang, Microsoft, Bethesda and ZeniMax. Reviewed on 3 October 2026 for VegasCraft 0.2.2.

- [SkyCraft](https://github.com/chasmlol/SkyCraft), commit `bfcaf178524b92c2cdeb88e4ce0f13ef9ded6f32`: source baseline for the shared-memory protocol, Fabric mod, and adapted input/rendering/gameplay/packaging paths. The protocol in `protocol/skycraft_protocol.h` is now extended to **v20**, mirrored in the Java bridge; it is no longer an unchanged v11 copy. The MIT notice is preserved in [licenses/SkyCraft-MIT.txt](licenses/SkyCraft-MIT.txt) and included in the Fabric jar and release. Initial Fabric build configuration and Gradle wrapper came from this revision; current pins are in `fabric/gradle.properties` and wrapper properties. SkyCraft's Skyrim plugin is not packaged.
- [xNVSE](https://github.com/xNVSE/NVSE), commit `0ccd23ad885ddae533c1790a3fc56cd073e38de3`: reference for plugin ABI, runtime, message IDs, player singleton and reference/cell offsets. An ignored local checkout under `upstream/xNVSE` was used for auditing; it is not shipped. The narrow ABI declaration and bridge implementation are VegasCraft code.
- [NVTF](https://github.com/carxt/New-Vegas-Tick-Fix), commit `f442e43d5b82cd229d5ea84ff181142e8a233ec6`; [TESReloaded10](https://github.com/llde/TESReloaded10), commit `a2cf6f3560f909ec366f5818a7172a0a3d61aab7`; and [JohnnyGuitarNVSE](https://github.com/carxt/JohnnyGuitarNVSE), commit `17af2dac17d436774046fef5ab394b83c0cf113f`: engine-layout references used for renderer/device/camera research. The initial research and conflicting camera offsets are recorded in [the historical rendering notes](docs/history/MOVEMENT-CUBE.md). These projects are not bundled runtime dependencies.
- Gradle wrapper **9.7.1**: [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0). Used for development only, not shipped in the mod package.
- LLVM-MinGW and Eclipse Temurin are local development tools in `.tools`, not packaged mod dependencies.

## Portable Minecraft launcher bundle

`tools/Package.ps1` downloads the following pinned artifacts into the nested `VegasCraft-Minecraft.zip`:

| Component | Packaged version | License / source |
| --- | --- | --- |
| Prism Launcher | 11.1.1, unmodified portable Windows build | GPL-3.0; [tagged source](https://github.com/PrismLauncher/PrismLauncher/tree/11.1.1); license copied into `Prism/LICENSE-PrismLauncher.txt` |
| Fabric API | 0.161.0+26.3 | Apache-2.0; [source](https://github.com/FabricMC/fabric) |
| e4mc | 6.2.2, Fabric modern jar | MIT; [source](https://github.com/vgskye/e4mc-minecraft-architectury) |
| VegasCraft Fabric mod | 0.2.2 | MIT; includes the preserved SkyCraft notice |

The bundle carries its own `Prism/THIRD-PARTY.txt`, generated from the template in `tools/minecraft-bundle`. Prism downloads Minecraft, Fabric Loader and Java after account setup; those downloads are not included in the release. e4mc provides LAN sharing infrastructure; New Vegas multiplayer remains unvalidated.

No Minecraft or Fallout game binaries, game textures, credentials or launcher authentication data are distributed.

Native inventory/stat icons and world-map resources are generated locally from the user's installed New Vegas archives/loose files into Minecraft's `vegascraft/nvicons` resource pack. They are not included in this repository's release bundle. The historical audio decoder workaround was a separate local plugin and is not part of VegasCraft packaging.
