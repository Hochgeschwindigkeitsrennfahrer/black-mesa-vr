# Comparison: L4D2VR DXVK vs GPLALL 2.6.8-2 vs Ph42oN v3.0-1

## Verdict (read this first)

| Candidate | Role for Black Mesa VR |
| --- | --- |
| **L4D2VR DXVK 2.6.1+ (current tree)** | **Use this now.** Already compiled into our `d3d9.dll`. Has Async + gplAsyncCache + Low Latency + `IDirect3DVR9`. |
| **DXVK-GPLALL 2.6.8-2** | **Only future source-upgrade.** Same feature family, newer D3D9 CPU work, default `max-frame-latency`, 2.6.8-2 hotfix. Merge cost is high. Prebuilt zip is unusable as the VR DLL. |
| **Ph42oN v3.0-1** | **Do not use.** DXVK 3.0 Async-only, no state cache, no frame pacer, Vulkan 1.4, predates the Black Mesa 3.0.1 rendering fix. |

Evidence tags in the cells below: **S** source, **R** release artifacts, **L** L4D2VR, **P** this BMVR project, **I** strong inference, **U** unknown.

---

## L4D2VR bundled DXVK 2.6.1 fork

| Topic | Detail |
| --- | --- |
| Exact version | `RELEASE=2.6.1`, `DXVK_VERSION="v2.6.1+"` (**S**) |
| Fork | doitsujin 2.6.1 + GPLALL **2.6.1-6** (`7927e64`) + L4D2VR VR files (**L**, **S**) |
| Patches | Async, gplAsyncCache, GPL default False, FramePacer (default **low-latency** if conf empty), state cache, `IDirect3DVR9`, Present/CreateTexture VR, ReShade VR, BM `bms.exe` profile in *this* clone (**S**, **P**) |
| Build | MSVC `Release\|x86` (L4D2VR sln) / BMVR CMake Win32 (**L**, **P**). Not a meson mingw zip. |
| Configuration | Compiled defaults; **no** populated `dxvk.conf` in git (**S**). Brief's `framePace=max-frame-latency` is **not** this snapshot's default. |
| Why this version | L4D2VR ported GPLALL once at 2.6.1-6 and froze the DXVK tree so VR patches stay applyable. **I** (no L4D2VR changelog saying "we refuse 2.6.8"). |
| D3D9 / D3D11 | Game path is **D3D9 only**. D3D11 exists in source, not in our CMake list (**P**: `tools/dxvk_sources.txt`). |
| Vulkan | 1.3 (**S**) |
| 32-bit | Yes; worker cap 16 (**S**) |

---

## DXVK-GPLALL 2.6.8-2

| Topic | Detail |
| --- | --- |
| Upstream base | DXVK **2.6.2** + selected 2.7/2.7.1+ commits (**R**) |
| GPLAsync | Yes; Ph42oN 2.6.2 + 2.7.1-1 patch lineage; RT-age threshold raised to **6** in 2.6.8-1 notes (**R**) |
| State cache | **Kept** (2.6.x branch reason for existing) (**R**) |
| `gplAsyncCache` | **Yes**, default True (**S** on `GPLALL-master-2.6.8`) |
| Low latency | Yes; default **`max-frame-latency`**; extra `low-latency-vrr-*`; timestamp query pools (**S**) |
| Compatibility | Vulkan 1.3, 32-bit D3D9, SSE2 builds, Windows GCC/MSVC zips (**R**) |
| Risks | Frame pacer API change; D3D9 tracking rewrites; 2.6.8-1 visual bug (fixed in -2); Intel `low-latency` crash; merging destroys VR patches if careless (**S**, **R**, **I**) |

---

## Ph42oN DXVK-GPLAsync v3.0-1

| Topic | Detail |
| --- | --- |
| Upstream base | **DXVK 3.0** (tag/date match; **I** until tar `RELEASE` is read) (**R**) |
| GPLAsync architecture | Skip-draw async + RT-age ≥5; GPL **can** stay on (**S** master patch) |
| State cache | **Gone** since Ph42oN 2.7-1 (**R**) |
| `gplAsyncCache` | **Does not exist** (**R**, **S**) |
| Low latency | **Not included** (**S** patch; Ph42oN README) |
| Compatibility | Vulkan **1.4**; dxbc-spirv; descriptor heaps; 32-bit support claimed by history (**U** for this zip) |
| Risks | BM rendering regression until 3.0.1; GPLALL cancelled 3.x after x32 D3D9 failure; RDNA1/2 Windows; our VR hooks sit on 2.6 internals (**R**, **P**) |

---

## Feature matrix

| Feature | L4D2VR 2.6.1+ | GPLALL 2.6.8-2 | Ph42oN v3.0-1 |
| --- | --- | --- | --- |
| `enableAsync` default True | Yes | Yes | Yes |
| `gplAsyncCache` | Yes | Yes | **No** |
| DXVK pipeline state cache | Yes | Yes | **No** (IR shader cache instead) |
| FramePacer / `framePace` | Yes, default **low-latency** | Yes, default **max-frame-latency** | **No** |
| `latencySleep` default False for pacer | Yes | Yes | Upstream (not pacer-oriented) |
| GPL default | False | False | Auto (upstream) |
| `IDirect3DVR9` | **Yes** | No | No |
| Combined VR `d3d9.dll` | **Yes** | No | No |
| Vulkan | 1.3 | 1.3 | 1.4 |
| Safe 32-bit D3D9 story | Yes | Yes (branch purpose) | Weak (3.0.1 still patching x32 Nvidia; GPLALL skipped 3.x) |
| Black Mesa-specific 3.0 bug | N/A | N/A | **Present in 3.0**, fixed 3.0.1 **after** this tag |

---

## What changed between L4D2VR's `7927e64` port and 2.6.8-2

L4D2VR **can** be treated as an older snapshot we may later supersede **in the DXVK core**, not in the VR layer.

Material differences (not a complete git log):

1. Default frame pace **low-latency → max-frame-latency**
2. `low-latency-vrr-*` + timestamp query pools
3. Async RT-age 5 → 6 (notes)
4. D3D9 CPU bind / RT / SetRenderState work
5. Push constant size 128 → 64
6. `enableDescriptorUpdateTemplates` default True
7. 32-bit compiler thread policy (16 in our tree vs 12 in GPLALL notes)
8. 2.6.8-2 rendering hotfix vs 2.6.8-1
9. FramePacer constructor signature (`DxvkDevice*`)

None of that is available by copying Ph42oN v3.0-1.

---

## Which one Black Mesa should use

**Now:** L4D2VR 2.6.1+ already in `third_party/l4d2vr/dxvk_new`.

**Config:** explicit `dxvk.conf` via BM's `DXVK_CONFIG_FILE` path; prefer documenting `max-frame-latency` as the VR A/B *starting* override even though L4D2VR's implicit default is low-latency. See [`config.md`](config.md).

**Later, only if stutter/CPU in DXVK is still the limiter after config:** merge GPLALL **2.6.8-2 source**, never 2.6.8-1, never Ph42oN 3.x.

**Never:** replace the installed combined `d3d9.dll` with a GPLALL or Ph42oN prebuilt.
