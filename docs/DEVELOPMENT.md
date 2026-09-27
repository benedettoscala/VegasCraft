# Development, packaging, and deployment

Reviewed on **3 October 2026** for **0.2.2**. Both native and Fabric projects declare **0.2.2**; all bridge declarations require **v20**.

## Build requirements and outputs

Use Windows, CMake **3.25+**, Ninja, a C++20 compiler targeting **x86 Windows**, a **64-bit JDK 25**, and Python for the cross-process test. Helpers discover LLVM-MinGW UCRT and `.tools/jdk-25*` installations, or accept explicit paths. From the repository root:

```powershell
./tools/Build-Native.ps1 -CompilerRoot C:\Tools\llvm-mingw
./tools/Build-Fabric.ps1 -JavaHome C:\Tools\jdk-25
python tools/test_bridge.py --java-home C:\Tools\jdk-25
```

The native helper configures `build/native`, builds loader/core and fixtures, and runs CTest. Fabric uses Gradle **9.7.1**, Loom **1.18.2**, Minecraft **26.3**, Fabric Loader **0.19.5**, and Fabric API **0.161.0+26.3**. Its `build` task includes JUnit tests, which Gradle may reuse if up to date. The helper temporarily uses `.tools/gradle-cache` and restores the prior Java/cache environment.

| Output | Use |
| --- | --- |
| `build/native/VegasCraft.dll` | Permanent xNVSE loader |
| `build/native/VegasCraftCore.dll` | Reloadable native gameplay/render core |
| `build/native/bridge_host.exe` | x86 host for the Java cross-process test |
| `fabric/build/libs/vegascraft-fabric-0.2.2.jar` | Minecraft runtime mod |
| `fabric/build/libs/vegascraft-fabric-0.2.2-sources.jar` | Development source archive |
| `fabric/build/test-results/test` | JUnit XML results |

CMake also accepts MSVC with a **Win32** generator. Packaging expects both DLLs directly under `build/native`; multi-configuration generators may require copying their output there. Fixtures have special image-base settings to emulate native fixed-address memory without ASLR collisions; those settings are not applied to release DLLs.

## Validation

```powershell
ctest --test-dir build/native --output-on-failure
python tools/test_bridge.py --java-home C:\Tools\jdk-25
```

CTest registers **five** cases: `plugin_lifecycle`, `homography`, `render_fixture`, `gameplay_fixture`, and `pipboy_data`. The Pip-Boy fixture runs production inventory/STATS/quest readers against synthetic engine memory, including inaccessible icon pointers and memory page boundaries. Fabric has **21** ray/collision cases. To explicitly re-execute Java tests, run `./gradlew.bat test --rerun-tasks --no-daemon` from `fabric` with `JAVA_HOME` pointing to the JDK. Coverage and evidence are in [VALIDATION.md](VALIDATION.md).

Native fixtures use isolated mapping names and do not need the actual games. The render fixture uses the historical movement cube. For current visual/gameplay checks use [TEST-0.2.2.md](TEST-0.2.2.md).

## Packaging

After building both halves:

```powershell
./tools/Package.ps1
```

The script reads the version from `fabric/gradle.properties`, requires both native DLLs and the runtime jar, and downloads/caches Prism **11.1.1**, Fabric API **0.161.0+26.3**, and e4mc **6.2.2**. The Prism archive is checked against pinned SHA-256 and mod jars against pinned SHA-512 values. Prism's separately downloaded license text has no configured hash.

Final outputs are `dist/VegasCraft-0.2.2.zip` and `dist/vegascraft-fabric-0.2.2.jar`. The release contains:

```text
NVSE/Plugins/VegasCraft.dll
NVSE/Plugins/VegasCraft.ini
NVSE/Plugins/VegasCraft/VegasCraftCore.dll
NVSE/Plugins/VegasCraft/VegasCraft-Minecraft.zip
NVSE/Plugins/VegasCraft/LICENSE.txt
NVSE/Plugins/VegasCraft/THIRD-PARTY-NOTICES.md
NVSE/Plugins/VegasCraft/SkyCraft-MIT.txt
```

The Minecraft zip is nested in the release; intermediate `dist/bundle` and the standalone bundle zip are removed by packaging. No Minecraft/Fallout game binaries or extracted game textures are packaged. Install relative to **Data**, not beside the game executable. Packaging does not rerun builds/tests or verify the installed game.

## Reloading the core in a running game

The loader reads `[Development] bHotReload=1` at startup. It watches `Data/NVSE/Plugins/VegasCraft/VegasCraftCore.dll`, waits for a changed build to settle, stops the old core, and loads a copy of the replacement. It avoids reload during native loading and starts a new collision epoch so Minecraft replays world state.

After initial native configuration, build/test and install core-only changes with:

```powershell
./tools/Deploy-Core.ps1 -GameDir 'C:\games\Steam\steamapps\common\Fallout New Vegas'
```

The helper builds, runs CTest unless `-SkipTests` is requested, copies the core, and waits up to 20 seconds for the reload log result. If the loader changed while New Vegas runs, it warns that a restart is required. If a core fails to start, install a working build and inspect the log; if shutdown cannot restore all hooks, the old module stays loaded, so restart before further testing.

| Changed component | Required action |
| --- | --- |
| Core implementation with compatible ABI/bridge | Deploy core; keep both games running |
| Loader (`Plugin.cpp`, launcher/loader behavior) | Restart New Vegas with updated loader/core |
| `CoreAbi.h` or `Bridge.h` interface/layout | Rebuild both native DLLs; restart New Vegas |
| Shared-memory protocol/layout | Update C++/Java declarations; rebuild both halves; restart both games |
| Fabric classes/resources | Rebuild/install jar; restart Minecraft |
| Normal release update | Close both games; install the complete matching package |

`tools/Restart-NewVegas.ps1 -GameDir ...` **forcibly stops FalloutNV**, installs both built DLLs, and starts the game. Save first. It chooses `FalloutNV.exe` if the 4GB Patcher backup exists, otherwise `nvse_loader.exe`. For MO2, use its normal xNVSE launch/deployment path so virtual files are available.

## Runtime probes and logs

The core polls `VegasCraft.cmd` beside `FalloutNV.exe`, executes its lines, deletes the file, and writes results to `VegasCraft.log`. A diagnostic example:

```text
pos
inv
profile 20
```

`profile 20` reports frame/component timings for 20 seconds; `pip` reports Pip-Boy data; `pipsec 0`, `pipsec 1`, and `pipsec 2` select STATS/ITEMS/DATA; `reload` requests core reload. Additional probes in `native/Probe.cpp` inject input or execute native console actions; use them deliberately on a test save.

Native logs live beside `FalloutNV.exe`; bundled Minecraft logs are under `%LOCALAPPDATA%\VegasCraft\Prism\instances\VegasCraft\.minecraft\logs\latest.log`. `tools/nvpeek.py` and `tools/nvdis.py` are engine-inspection helpers, not runtime dependencies. `tools/gen_pipboy.py` generates the checked-in Pip-Boy model/texture resources.
