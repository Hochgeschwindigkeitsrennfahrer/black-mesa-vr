# Black Mesa offsets (scanned)

Install: `C:\Program Files (x86)\Steam\steamapps\common\Black Mesa`

## public-beta buildid 25607072 (2026-10-02)

Anomalous Improvements rebuilt `client.dll`, `server.dll`, `engine.dll`, and `materialsystem.dll`. `steam.inf` PatchVersion is still `100002`. Interface names did not change (`VEngineClient015`, `VMaterialSystem081`, `VEngineModel016`). `-oldgameui` is still a command-line string in `engine.dll`.

Vtable slots used by the mod held (RenderView slot 6, DrawModelExecute slot 19, GetRenderTarget slot 7, GetViewport slot 39, PushRT slot 143, PopRT slot 147, FindTexture slot 84, SetRTFBOverride slot 142).

What moved:

- CMatRenderContext RT stack: count `+0x4C` → `+0x58`, array base `+0x40` → `+0x4C`. GetRenderTarget / GetViewport / PopRT signatures follow that.
- CMaterialSystem alloc flags `+0x24`: `isGameRunning` `0x2AA4` → `0x2AC8`, named-RT lock `0x2AA0` → `0x2AC4`.
- CMatQueuedRenderContext::GetRenderTargetDimensions is no longer a `ret 8` stub. It reads the same RT stack.
- `CClientShadowMgr::UpdateFlashlightState` is `client.dll+0x11BD80` (still pushes that function's name).
- `SetAbsOrigin` / `SetAbsAngles` are `client.dll+0xAF980` / `+0xAF860`. Abs fields stay `+0x294` / `+0x2D0`.
- `CViewSetup::m_eStereoEye` is still `+0x1C` (`cmp [ebx+1Ch], 2` inside the new RenderView).

`CEnvLaserDot::Draw` no longer matches its old prologue, so that hook skips. `ImpulseCommands` at the old server RVA no longer contains `m_nImpulse`, so that passthrough hook skips too.

Hooking `CEngineVGui::Paint` (`engine.dll+0x239900`) and calling through the trampoline makes this beta exit on the first menu paint, before any headset frame. Leaving that hook off, the backbuffer menu capture submits to OpenXR. The machine skip file `bmvr_skip.txt` records `vgui_paint` from that failure.

`C_BlackMesaViewModel` vtable slot `0x398` is `CalcViewModelView` (`client.dll+0x2A1170`). The VR body of that hook access-violates during `bm_c1a0a` load (`C_BasePlayer` slot `0x404` at `client.dll+0x7A210` calls it). The detour catches that and calls the engine with the original eye pose. The lightcache walker (stride `0x1E0`) treats a real next index as a cycle, writes the sentinel into the pool, and `engine.dll` then faults; the hook calls the engine function instead. Material render-target and viewport detours, installed together, fault in `CBlackMesaViewRender` at `client.dll+0x1F303C` (call to a stack address) after LevelInit, so those detours stay off on this build.

The table below is the pre-beta scan. Current RVAs live in `src/offsets.h`.

| Name | Module | Expected RVA | Status | Found RVAs |
| --- | --- | --- | --- | --- |
| RenderView | client.dll | `0x20EE40` | MATCH | `IViewRender` slot 6 of `CBlackMesaViewRender` (`ret 0xC`, 3-arg `CViewSetup&`). Old `0x207730` is a float-arg helper, not RenderView. |
| g_pClientMode | client.dll | `0x16AD56` | MATCH | `0x16AD56` |
| CreateMove | client.dll | `0x110310` | MATCH | `0x110310` |
| m_hMyWeapons | client.dll | `0xEE4` | MATCH | `DT_BaseCombatCharacter` RecvTable `FUN_100a3de0`, count `0x30`. `m_hActiveWeapon` at `0xFA4`. |
| m_hUseEntity | client.dll | `0x1344` | MATCH | `DT_BasePlayer` RecvTable `FUN_100b6a00`. Held pickup / use target. |
| Grab hold origin | server.dll | `0x77240` | MATCH | `CBasePlayer` vtable `+0x474` `FUN_10077240`. `CGrabController::UpdateObject` `FUN_10470da0` hold origin (not `Weapon_ShootPosition` `+0x23C`). Distance `2*radius+24`. |
| Grab SetTargetPosition | server.dll | `0x470990` | MATCH | `CGrabController::SetTargetPosition` `FUN_10470990`. Final hold point after `2*radius+24`. |
| CalcViewModelView | client.dll | `0x29D930` | MATCH | `0x29D930` |
| AdjustEngineViewport | client.dll | `0x1102C0` | MATCH | `0x1102C0` |
| LevelInit | client.dll | `0x110A80` | MATCH | `0x110A80` |
| LevelShutdown | client.dll | `0x110B30` | MATCH | `0x110B30` |
| OverrideView | client.dll | `0x110BE0` | MOVED | `0x1D40`, `0x1E70`, `0x2770`, `0x6FA0`, `0x77C0`, `0x8AC0`, `0xFF90`, `0x10840` |
| DrawModelExecute | engine.dll | `0x113E80` | MATCH | `CModelRender` IModelRender slot 19 (`+0x4C`), thiscall 3-arg `ret 0xC`. **Not** `0xF6A20` (DispInfo displacement helper; that hang 2026-08-18). Do **not** `FindMaterial` from this detour (2026-08-19 load crash on first eligible `v_*`). Hide arms from `DrawModelState` studiohwdata materials. |
| VGui_Paint | engine.dll | `0x238C50` | MATCH | `0x238C50` |
| GetRenderTarget | materialsystem.dll | `0x68820` | MATCH | `0x68820` |
| GetViewport | materialsystem.dll | `0x68A70` | MATCH | `0x68A70` |
| Viewport | materialsystem.dll | `0x69F30` | MATCH | `0x69F30` |
| PushRenderTargetAndViewport | materialsystem.dll | `0x6A3D0` | MATCH | `0x6A3D0` |
| PopRenderTargetAndViewport | materialsystem.dll | `0x6A250` | MATCH | `0x6A250` |
| ProcessUsercmds | server.dll | `0x5320F0` | MATCH | `0x5320F0` |
