> Historical record of the initial movement-cube milestone (1 October 2026). Its pending items and implementation status describe that milestone, not the current repository. See [current validation](../VALIDATION.md), [current rendering](../RENDERING.md), and the [README](../../README.md).

# Validation — 2026-10-01

## Completed

- Release x86 DLL built with LLVM-MinGW, with exactly the two expected `NVSEPlugin_Query` / `NVSEPlugin_Load` exports. No compiler runtime DLL dependencies; Windows UCRT is used.
- Native DLL lifecycle fixture: accepted/rejected executables, xNVSE messaging, player singleton and reference fields, exterior/interior cell changes, failed/successful loads, shutdown, duplicate hosts, retained mapping and restart.
- Test fixture maps use unique names so tests can run alongside the real game. Its image base is fixed only for the test executable, to emulate FalloutNV's absolute singleton address without ASLR collisions.
- Production Java bridge tested in a real x64 JVM against a separate x86 process: both state directions, shutdown detection, wrong protocol version, truncated mapping.
- Fabric mod built against Minecraft 26.3 / Fabric Loader 0.19.5 / Fabric API 0.161.0+26.3, with Java 25.
- Actual Minecraft development client started and loaded VegasCraft 0.1.0.
- Actual FalloutNV 1.4.0.525 started via xNVSE 6.4.9 and reported VegasCraft loaded correctly.
- Both real game processes exchanged heartbeats and sample counters while New Vegas was at the main menu.
- With the audio workaround installed, the user entered the real New Vegas world successfully. VegasCraft reported `inGame=1`, world `000DA726`, and changing player coordinates while moving.
- The real Minecraft client loaded the user's `New World`; server/chunk logs confirm world entry.
- With `bAlwaysActive=1` set in the user's backed-up Fallout.ini, both actual worlds exchanged live samples continuously while Minecraft was focused: New Vegas world `000DA726`, Minecraft positions changed from `-2.558,69.000,-51.430` to `-9.305,68.000,-45.300`. The two player positions are measured independently, not synchronized.
- Experimental opt-in D3D9 movement cube built and tested in a disposable hidden renderer fixture. Actual pixels were read back and visually inspected; culling was corrected after the first image showed interior faces. Device render state and projection restoration were verified. Pixel centroid movement follows a real shared-memory Minecraft sample, load epoch resets the origin, and unloaded Minecraft state suppresses drawing. This fixture is not evidence of rendering inside New Vegas.
- First real rendering attempt safely skipped drawing because the initial camera/frustum was invalid. Corrected the world camera and frustum against JohnnyGuitarNVSE; see `RENDERING.md`. The corrected DLL was installed and New Vegas was restarted through xNVSE from its game directory. Actual world rendering remains pending a loaded save.
- With the corrected camera, the live log reported a valid camera and frustum but `hr=8876086C` (`D3DERR_INVALIDCALL`), and no cube appeared. New Vegas still holds its own scene open when xNVSE reports the frame, so the probe's `BeginScene` was rejected. The probe now draws inside the open scene and only ends a scene it began. The renderer fixture reproduces the open scene and requires the cube to be drawn.
- With that DLL installed, the user's quicksave was loaded in the real New Vegas. The log reports `begin=8876086C hr=00000000`, and the cube was visible three blocks ahead of the camera, over the Mojave terrain.
- Moving the real Minecraft player moved the real cube in New Vegas. A quickload (F9) reset the anchor and the cube reappeared in front of the camera; two short Minecraft strafes then moved it step by step across the New Vegas view. Its on-screen direction does not yet match the Minecraft view, since axes are not calibrated.
- The real Minecraft HUD showed `VegasCraft | Mojave 000DA726 | -409.5, 91.1, -99.8`, matching the New Vegas log.

## Crash investigation

The first attempts to enter the New Vegas world crashed. Windows Event Viewer recorded `msmpeg2ac3dec.dll` 10.0.26100.9549, exception `0xc0000602`, offset `0x00053ebc`. Windows update KB5124010 was installed on the machine.

The source and release of [Sep2026LoadingCrashFix](https://github.com/Naragorn/Sep2026LoadingCrashFix) were inspected at commit `86f28ad4d527f08969a8377195b99f37a7e8175a`. The official New Vegas 0.2.0 archive was verified against GitHub's SHA-256 digest `196729db66f33a8bcec8f6b3576f27c2ce34bcac261411e0495cd5c3cb9c4207`.

The author's x86 DirectShow integration test reproduced the same `0xc0000602` crash on this machine in the unprotected control process. With its process-local COM decoder block enabled, four successive MP3 graphs passed in each of the STA and MTA cases. The author's export test also loaded the actual downloaded DLL and verified its NVSE entry points and successful decoder-block registration.

The official workaround was installed as a separate xNVSE plugin. It does not modify Windows files or registry settings. The earlier VegasCraft DLL was preserved in `diagnostics/disabled-plugin`; the installed VegasCraft was replaced with the tested version mapping only 4 KiB into the 32-bit game.

## Pending

- Calibrate the Minecraft-to-Mojave axes and orientation, so the cube moves in the same direction as the Minecraft view.
- The present-time overlay is also drawn over the New Vegas pause menu and has no terrain occlusion.
- Rendering of Minecraft blocks in New Vegas, collision export, movement control and gameplay integration are not implemented in this milestone.
