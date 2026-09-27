> Historical record of the initial movement-cube milestone (1 October 2026). Its pending items and implementation status describe that milestone, not the current repository. See [current validation](../VALIDATION.md), [current rendering](../RENDERING.md), and the [README](../../README.md).

# Experimental movement cube

`[Preview] Enabled=1` enables an independent D3D9 rendering probe through xNVSE's `OnFramePresent` callback. It requires both games to report loaded player state. It anchors a 70-unit cube three blocks ahead of the current New Vegas camera when the first Minecraft sample arrives. Minecraft player displacement then moves the cube relative to this origin. A new New Vegas world/load epoch resets the origin.

This is an overlay for validating camera transforms and visible cross-process movement. Present-time depth is unavailable, so it does not claim terrain occlusion, collision, block placement or rendering of Minecraft chunks. It creates no game forms and makes no save-game changes. It restores D3D device state after drawing and allocates no persistent GPU resources.

The addresses/offsets were researched against the following primary sources, without copying their code:

- [NVTF](https://github.com/carxt/New-Vegas-Tick-Fix), commit `f442e43d5b82cd229d5ea84ff181142e8a233ec6`: renderer singleton `0x011C73B4`, TESMain singleton `0x011DEA0C`.
- [TESReloaded10](https://github.com/llde/TESReloaded10), commit `a2cf6f3560f909ec366f5818a7172a0a3d61aab7`: New Vegas device offset `0x288` and forward/up/right axis conventions. Its camera comments are outdated; main camera `0xA0` is the first-person camera, not the world camera.
- [JohnnyGuitarNVSE](https://github.com/carxt/JohnnyGuitarNVSE), commit `17af2dac17d436774046fef5ab394b83c0cf113f`: world scene singleton `0x011DEB7C`, scene camera `0xAC`, camera world transform `0x68`, and actual frustum `0xDC`. These replace the initial incorrect `0xD4` offset; the live game correctly rejected that initial frustum as invalid.
- Pinned xNVSE `Hooks_Gameplay.cpp`: message 24 is emitted immediately before `0xB6B730` displays the frame; its integer payload distinguishes loading screens.

The older xNVSE `NiObjects.h` camera layout and the TESReloaded camera comments disagree with the actual New Vegas layout. The initial live test reported an invalid frustum and skipped drawing. JohnnyGuitar's explicit `0xDC` offset and separate world camera resolve those source discrepancies. At that point New Vegas still holds its own Direct3D scene open, so `BeginScene` returns `D3DERR_INVALIDCALL`. The probe draws inside the open scene and ends only a scene it began itself; the log reports both `begin` and the draw `hr`. Verified in the live game: `begin=8876086C hr=00000000`, with the cube visible.

The probe uses bounded `ReadProcessMemory` calls, validates finite transforms and a perspective frustum, and logs the actual camera/frustum plus draw HRESULT. Only a real game capture can verify visible behavior; successful compilation does not establish it.
