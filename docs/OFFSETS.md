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

The lightcache walker (stride `0x1E0`) treats a real next index as a cycle, writes the sentinel into the pool, and `engine.dll` then faults; the hook calls the engine function instead.

## public-beta buildid 25844216 (2026-10-10)

`client.dll`, `server.dll`, `engine.dll`, `vgui2.dll`, `vguimatsurface.dll`, `studiorender.dll`, and `stdshader_dx9.dll` were rebuilt. `materialsystem.dll` and `shaderapidx9.dll` are the 2026-10-02 files.

### Two interface vtables gained a slot

These broke the stable path on both beta builds. Every crash that the 25607072 notes blamed on VGui_Paint, the material RT/viewport detours (`client.dll+0x1F303C`, "call to a stack address"), CalcViewModelView, CreateMove pre-work, eye-RT creation (`shaderapidx9+0x553FF`, ESI = device `this` garbage), and the first controller pose was a callee popping the wrong number of stack bytes. The caller's saved registers came back wrong or ESP was off on return.

- `VEngineClient015` (`engine.dll`): a 1-arg `ret 4` method sits at slot 26. `IsInGame` is slot 27 (`cmp [signonState], 6; setz al; ret`). `GetLevelName` / `GetLevelNameShort` are 52 / 53 (52 returns `"Dedicated Server"` when dedicated). `IsPaused` is 88, `ClientCmd_Unrestricted` 109. On 25607072 the same shift was already live: `IsInGame` stayed false through gameplay and the "short" map getter returned `maps/bm_c1a0a.bsp`. `src/sdk/sdk.h` has the extra slot.
- `IMatRenderContext` (`materialsystem.dll`): one slot inserted between 149 and 180. `OverrideAlphaWriteEnable` is slot 181 (`+0x2D4`, both contexts `ret 8`, forwards to shaderapi `+0x46C`). Slot 180 is a 0-arg `ret 0`; calling it with two args corrupted EDI in `PreparePauseHudForVgui` (exit on the first in-game pause HUD). `GetCallQueue` is slot 186: the queued context returns `ecx+0x298`, the immediate context returns 0. Old slot 185 writes `[ecx+0x208] = 0`. Slots 7, 39, 141, 143, 147, 149 held.

`IEngineTrace::TraceRay` (slot 5), `IVModelInfoClient` GetModel/GetModelIndex/GetModelName (1–3), `VClientEntityList003`, `VEngineVGui001`, IClientMode slots 30/32, and IClientRenderable attachment slots 36–38 held.

### client.dll moved about −0xE160 (−0xDBCC to −0xDE80 near 0x16xxxx–0x20xxxx)

Signature-scanned hooks follow automatically. Fixed RVAs updated in `src/offsets.h`:

| Name | 25607072 | 25844216 | Evidence |
| --- | --- | --- | --- |
| CalcViewModelView | `0x2A1170` | `0x29BA10` | `C_BlackMesaViewModel` vtable slot 230 (RTTI). Prologue changed. |
| GetViewModelFOV | `0x217060` | `0x209EF0` | `ClientModeBlackMesaNormal` vtable slot 32. |
| UpdateFlashlightState | `0x11BD80` | `0x10DC20` | Only xref of `"CClientShadowMgr::UpdateFlashlightState"`. |
| SetAbsOrigin / SetAbsAngles | `0xAF980` / `0xAF860` | `0xA1820` / `0xA1700` | `ucomiss [esi+0x294]` / `[esi+0x2D0]` prologue (checked at call time). |
| Weapon_ShootPosition (client) | `0x7C280` | `0x6E120` | Thunk bytes `55 8B EC 8B 11 FF 75 08 FF 92 68 02 00 00`. |
| GetAttachment vec / matrix | `0x97CE0` / `0x97C10` | `0x89B80` / `0x89AB0` | `C_BaseAnimating` renderable vtable slots 38 / 37. |
| IClientMode global | `0x6DEA90` | `0x6D5B58` | Operand of the `g_pClientMode` signature. |
| gpGlobals | `0x553A04` | `0x54FA04` | `mov reg,[imm]; movss xmm,[reg+0xC]` (curtime), 142 sites. |

`CBaseEntity::EmitSound` and the particle-system manager/find are not re-verified; the old RVAs are mid-function. Those calls are off (`kCBaseEntity_EmitSoundVerified`, `kParticleSystemMgrVerified`).

Engine RVAs moved +0x10 (DrawModelExecute `0x114110`, LightcacheFindNearest `0x1D1A70`, GetScreenAspectRatio `0x101490`) and VGui_Paint `+0x1300` (`0x23AC00`); signatures follow.

With those fixed, the stable main render path runs unchanged on this beta: `bm_c1a0a`, `mat_queue_mode 2` multicore, `_rt_FullFrameFB` grown to the 3168x3104 eye, two-eye HMD-fb stereo with IPD origins, ~90 published frames/s, pause GameUI panel, no fault after several minutes.

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
