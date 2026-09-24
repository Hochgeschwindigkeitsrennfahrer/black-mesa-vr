# DXVK GPLAsync / Low-Latency research (Black Mesa VR)

Research-only handoff for the implementation agent. **No binaries, VR source, or live install were changed while writing this.**

**Read this order:**

1. [`implementation-plan.md`](implementation-plan.md) — what to do, in what order, and what not to do
2. [`HOW-TO-APPLY.md`](HOW-TO-APPLY.md) — GPLALL 2.6.8-2 + VR **source** drop-in (not applied)
3. [`comparison.md`](comparison.md) — L4D2VR 2.6.1 fork vs DXVK-GPLALL 2.6.8-2 vs Ph42oN v3.0-1
3. [`config.md`](config.md) — every `dxvk.*` line, version-specific validity
4. [`l4d2vr.md`](l4d2vr.md) — what L4D2VR actually ships and how it loads
5. [`gplall.md`](gplall.md) — DXVK-GPLALL 2.6.8-2
6. [`ph42on.md`](ph42on.md) — Ph42oN DXVK-GPLAsync v3.0-1
7. [`gpl-async-architecture.md`](gpl-async-architecture.md) — GPL vs Async vs GPLAsync vs state cache
8. [`vr-implications.md`](vr-implications.md) — compositor timing, missed frames, recommended tests

## Headline recommendation

**Keep compiling the current L4D2VR DXVK 2.6.1+ tree.** It already contains GPLAsync + state-cache integration + low-latency frame pacing from DXVK-GPLALL 2.6.1-6.

**Do not drop-in replace `d3d9.dll` with a prebuilt GPLALL or Ph42oN build.** That would delete `IDirect3DVR9`, Present→Submit, and every VR hook inside DXVK.

**Do not move to Ph42oN v3.0-1.** It is DXVK 3.0 + Async only: Vulkan 1.4, no state cache, no `gplAsyncCache`, no Low Latency pacer, and DXVK 3.0.1 later had to fix a **Black Mesa** rendering regression.

**If a later upgrade happens, the only candidate is DXVK-GPLALL 2.6.8-2 source merged into `third_party/l4d2vr/dxvk_new` with VR patches reapplied.** A staging tree for that merge is ready under [`drop-in-dxvk_new/`](drop-in-dxvk_new/) — see [`HOW-TO-APPLY.md`](HOW-TO-APPLY.md). It is **not** wired into CMake and was **not** installed. Do not treat “drop-in folder exists” as “DXVK upgraded.”

## Evidence tags used in these files

| Tag | Meaning |
| --- | --- |
| **Confirmed from source** | Read in this repo or in a fetched source file |
| **Confirmed from release/build artifacts** | GitHub/GitLab release JSON, tags, asset names |
| **Confirmed from L4D2VR** | L4D2VR README / vendored tree |
| **Confirmed from current Black Mesa project** | `CMakeLists.txt`, `install.ps1`, `docs/`, `src/` |
| **Strong inference** | Follows from the above but was not executed at runtime in this research pass |
| **Unknown** | Not verified; do not treat as fact |

## Dates / versions (this research pass)

| Item | Value | Evidence |
| --- | --- | --- |
| Today | 2026-08-18 | user_info |
| L4D2VR bundled DXVK | `v2.6.1+` (`RELEASE` = `2.6.1`) | Confirmed from source |
| GPLALL snapshot L4D2VR ported | `7927e64` = GPLALL **2.6.1-6** | Confirmed from L4D2VR README |
| DXVK-GPLALL latest 2.6.x | **2.6.8-2**, published 2026-07-28, branch `GPLALL-master-2.6.8` | Confirmed from release/build artifacts |
| Ph42oN v3.0-1 | released 2026-06-28 | Confirmed from release/build artifacts |
| Upstream DXVK 3.0 | 2026-06-25, Vulkan 1.4 | Confirmed from release/build artifacts |
| Upstream DXVK 3.0.1 | 2026-07-05, **Black Mesa rendering regression fix** | Confirmed from release/build artifacts |
| Stock Steam Black Mesa DXVK | 2.6.2 gcc, `bin\thirdparty\dxvk-windows-x86\` | Confirmed from current Black Mesa project (`docs/DXVK.md`) |
