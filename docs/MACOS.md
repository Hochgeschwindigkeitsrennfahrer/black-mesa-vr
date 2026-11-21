# CrossOver and MoltenVR compatibility

This port runs the Windows x86 game through CrossOver/DXVK/MoltenVK and uses the existing x64 OpenXR helper with MoltenVR.

## Setup

1. Install Windows Steam and Black Mesa in a CrossOver bottle with DXVK selected.
2. Install [MoltenVR and its Quest client](https://github.com/MoltenVR/MoltenVR-Preview). Register its OpenXR runtime in the same bottle and start streaming.
3. Use MoltenVK 1.4.2 or a compatible newer version. The development setup used the universal macOS library in both CrossOver's `lib/x86_64/libMoltenVK.dylib` and `lib/aarch64/libMoltenVK.dylib`. CrossOver's bundled version needs to expose the Vulkan features required by this DXVK fork.
4. Install the combined `d3d9.dll` beside `bms.exe`, in `bin/`, and in `bin/thirdparty/dxvk-windows-x86/`. Install the matching helper as `VR/openxr_helper64/OpenXRHelper64.exe` with its x64 OpenXR loader and the upstream mod assets.
5. Select `VRRuntimeBackend=openxr` and `OpenXRHelper=true` in `VR/config.txt`.
6. Set these Steam launch options and start the game from Windows Steam in CrossOver.

```text
-oldgameui -enabledxvk -novid -dev +developer 0
```

The classic menu and `-dev` startup avoid the background BSP. Loading a save after the background scene caused geometry corruption in the development setup. This remains a compatibility workaround.

The CPU texture transport is selected automatically when Wine reports the Darwin host. `BMVR_CPU_BRIDGE=1` explicitly selects it and `BMVR_CPU_BRIDGE=0` selects the original shared Vulkan image path. These are process environment variables.

## What changes

- MoltenVK device creation uses supported geometry, cull-distance and robustness features. Missing null-descriptor support uses explicit dummy resources.
- Fragment shader specialization constants are folded before Metal translation. This removes inactive color/depth sampler branches that MoltenVK otherwise has to translate together.
- The CPU transport publishes completed stereo pairs with the pose used to render them. BGRA image mappings provide a fallback when Win32 Vulkan memory handles cannot be shared.
- HUD/title textures have a separate transfer channel. Menu input and runtime recenter notifications can update independently of the held world frame.
- Readback polls completion events instead of waiting for every submission. The optional host-memory import path validates both eyes before enqueueing either copy.
- Hand/UI draws preserve vertex streams, declarations and render targets. The macOS UI path uses explicit shaders for projected vertices.
- Head, hand and weapon transforms use one stereo-frame snapshot. Grip and aim positions are compared before applying an extra Touch aim-origin offset.
- The model classification cache is invalidated across map changes and cached viewmodels are revalidated before applying viewmodel transforms.
- The process-local DEP setting used to avoid repeated execute-fault handling is enabled automatically only on Wine/Darwin. `BMVR_ENABLE_DEP=0` overrides it.

## Building and validation

Build the DLL with the existing Win32 CMake target and the helper with its x64 target. The Windows workflow selects clang-cl with the Visual Studio ClangCL toolset. SPIRV-Tools is linked statically. Its pinned source archives are fetched during configuration.

The existing `cmake/clang-cl-xwin-x86.cmake` and `cmake/clang-cl-xwin-x64.cmake` also allow cross-compilation with a Windows SDK/MSVC CRT splat selected by `XWIN`.

The extracted contribution has been cross-built for x86 and x64. Shared protocol sizes and offsets are asserted in both binaries. The original macOS fork has prior headset testing with Quest 3. This cleaned upstream contribution still needs headset validation and native Windows regression testing before merging.

Both binaries must be updated together because the bridge protocol changes to version 19. Future game updates can still require the usual engine offsets/signatures to be updated.
