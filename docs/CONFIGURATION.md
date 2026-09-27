# Configuration

Reviewed on **3 October 2026**, VegasCraft **0.2.2**, protocol **20**. Paths under `.minecraft` refer to the selected launcher instance, not this source repository.

## Native plugin

Install [config/VegasCraft.ini](../config/VegasCraft.ini) as `Data/NVSE/Plugins/VegasCraft.ini`, beside `VegasCraft.dll`.

| Section / key | Shipped default | Effect / activation |
| --- | --- | --- |
| `[Minecraft] bStartWithNewVegas` | `1` | Start Minecraft in a background thread at plugin startup; `0` lets you launch it yourself. Restart New Vegas to change startup behavior. |
| `[Minecraft] sLauncher` | Empty | Use the portable bundle. Set an absolute launcher or `.bat`/`.cmd` path for your own instance. Read at startup. |
| `[Minecraft] sArguments` | `--launch VegasCraft` | Arguments for a custom launcher; its instance folder name must match. Read at startup. |
| `[World] bExportLand` | `1` | Export native ground columns for Minecraft features/mobs. `0` disables this land export, not all collision export. Read when the core starts; reload/restart the core to apply. Existing generated features are not deleted. |
| `[Development] bHotReload` | `1` | Watch the installed `VegasCraftCore.dll` for replacement. Read at New Vegas startup. |
| `[Bridge] MappingName` | `Local\VegasCraft_v1` | Host mapping name; must begin with `Local\` and match the Fabric JVM argument if customized. Restart both games. |

Example for a custom launcher:

```ini
[Minecraft]
bStartWithNewVegas=1
sLauncher=C:\Tools\PrismLauncher\prismlauncher.exe
sArguments=--launch VegasCraft
```

The current plugin has no supported `[Preview] Enabled` switch. It belonged to the [old cube milestone](history/MOVEMENT-CUBE.md).

## Portable instance and data

| Path | Contents |
| --- | --- |
| `Data/NVSE/Plugins/VegasCraft/VegasCraftCore.dll` | Installed core watched by the loader |
| `Data/NVSE/Plugins/VegasCraft/VegasCraft-Minecraft.zip` | Portable launcher and instance template |
| `%LOCALAPPDATA%\VegasCraft\Prism` | Extracted launcher, account and download data |
| `%LOCALAPPDATA%\VegasCraft\Prism\instances\VegasCraft\.minecraft` | Minecraft mods, settings, saves, logs and generated resources |
| `.minecraft/saves/VegasCraft` | Automatically created/opened mirror world |
| `.minecraft/vegascraft/nvicons` | Generated native item/stat icons and world-map resources |

The instance uses Java 25, a 512 MiB initial/4096 MiB maximum memory allocation, hidden Minecraft startup, and native-access permission. Bundle replacement preserves launcher accounts, downloads, and worlds; bundled VegasCraft, Fabric API, and e4mc jars are replaced. Close both games for normal release updates.

## Minecraft properties

Edit `.minecraft/config/vegascraft.properties`. It is a Java properties file; forward slashes let you enter Windows paths without escaping backslashes:

```properties
destruction=true
mobs=true
join=
nvDataDir=C:/games/Steam/steamapps/common/Fallout New Vegas/Data
```

| Key | Default | Effect / activation |
| --- | --- | --- |
| `destruction` | `true` | Terrain mining and explosions. Read at Minecraft startup; the O-menu **Terrain destruction** button changes/saves it immediately. Turning it off leaves existing holes. In a guest world the host decides. |
| `mobs` | `true` | Added Minecraft animal/monster spawner on native terrain. Re-read roughly every 10 seconds; `false` also removes mobs managed by that spawner. |
| `join` | Empty | Persistent server/e4mc address instead of the mirror world. Read when attempting to open/join a world. To return reliably from a persistent join, clear it and restart Minecraft. |
| `nvDataDir` | Automatic discovery | Native `Data` folder fallback for icons/maps. Discovery checks a running FalloutNV process, this setting, then the standard Steam path. Restart Minecraft after changing it. |

First-world setup can create a template containing `join` and `mobs`; other keys can be added. The destruction toggle preserves other lines/comments.

Experimental session commands are `/join <address>` and `/leave`, entered with T. `/leave` handles a join requested during that session; it does not clear a saved `join` property. Host sharing uses **O → Open to LAN** and e4mc. See [validation limits](VALIDATION.md).

## First-person arm tuning

Edit `.minecraft/config/vegascraft_arms.txt`. It uses **whitespace-separated name/value pairs**, not `name=value`. The client checks it about every 500 ms; a Minecraft restart is not normally needed.

```text
showArms 1
```

| Name | Default | Meaning |
| --- | --- | --- |
| `showArms` | `0` | Show Minecraft first-person arms with a native ranged weapon when nonzero |
| `scale` | `0.08` | Native-pose scale for arm fitting |
| `handFov` | `70` | Minecraft hand field of view |
| `axis` | `1` | Arm alignment axis selection |
| `roll` | `0` | Arm roll adjustment |
| `fist` | `0.2` | Hand/fist offset |
| `back` | `4` | Native units to seat arms deeper behind the weapon |
| `nvTan` | `0` | Override native tangent of half the vertical FOV; zero uses the exported value |

These are development tuning controls. They do not replace third-person avatar or Pip-Boy settings.

## JVM and development options

| Option | Default / purpose |
| --- | --- |
| `--enable-native-access=ALL-UNNAMED` | Required by the Java Windows shared-memory bridge |
| `-Dvegascraft.startHidden=true` | Bundled instance starts Minecraft hidden |
| `-Dvegascraft.showWindow=true` | Keep the Minecraft window visible for debugging |
| `-Dvegascraft.quitWithSkyrim=false` | Keep Minecraft alive when the native host closes; inherited name applies to New Vegas; used by Gradle's development run |
| `-Dvegascraft.link=Local\VegasCraft_guest` | Custom mapping; must match the native INI host |
| `VEGASCRAFT_USERNAME` | Development `runClient` username; fallback `Courier` |
| `SKYCRAFT_LAN_PORT` | Development auto-publish port for the integrated server |
| `SKYCRAFT_LAN_OFFLINE` | Development offline-client LAN testing; inherited variable name |

JVM options take effect at Minecraft startup. See [DEVELOPMENT.md](DEVELOPMENT.md) for builds and probes. The mapping name's `_v1` suffix is not protocol **20**.
