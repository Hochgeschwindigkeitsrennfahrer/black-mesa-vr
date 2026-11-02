# Retired experiments

Approaches tried during bring-up (August–September 2026) that did not work on
Black Mesa. Each one was off on every launch and its code is removed (last in
git before the commit that added this file). Old `bmvr_skip.txt` files may still
list their names; the DLL ignores them.

Before reviving one, read why it failed. Most failures came from Black Mesa's
deferred renderer, whose world render targets (FullFrame, G-buffer) must match
the size of the view drawn into them.

## Rendering at headset resolution

What works instead: `WorldRenderAtEyeSize=true` grows FullFrame and the
G-buffers to the eye size at map load, and each eye is drawn with its own
`RenderView` into private eye targets (the log line `Render path: per-eye stereo
at eye size`).

| Skip name | What it tried | What happened |
|---|---|---|
| `hmd_swap` | Resize the game's D3D9 swapchain (window backbuffer) to the headset's recommended size | Black desktop, SteamVR stayed in the waiting room after the device reset |
| `steamvr_rt` | Eye targets at SteamVR's recommended size, bound over the window-sized G-buffer | World black on the desktop and in the headset |
| `ff_stereo` | Grow FullFrame and fit the eyes to it | Scene warped and smeared, gun and hands behind the player |
| `ff_hmdfit` | FullFrame at a window-capped headset-aspect size | Viewports stayed at window size; HDR copy turned the headset image white |
| `ff_gbfit` | FullFrame and G-buffer both at that size | Crashed on a menu background map before stereo started |
| `stereo_copy` | One 16:9 view copied into both eyes | Not fusable (no real stereo) |
| `stereo_fov` | Headset field of view squeezed into 16:9 pixels | Magnified, not fusable |
| `named_bind`, `named_l4d`, `named_eye`, `named_push` | Wrap the world pass in the engine's named render targets | Died after the third bind inside the world view |

## Engine view and timing

| Skip name | What it tried | What happened |
|---|---|---|
| `abs_view` | Write the absolute head pose into the live game camera | Replaced scripted cameras (tram) and blacked the headset after the load logo. Head pose goes on the per-eye view copies instead |
| `wait_idle` | Wait for the GPU to go idle around every eye submit | A stall every frame; coincided with a 1 fps crash while loading |
| `hl2vr_eye` | Set the view setup's stereo-eye field to left/right | On Black Mesa that field indexes other renderer state: first stereo frame crashed, a retry showed a white freeze. Eyes stay mono to the engine, left then right |
| `stereo_vis` | Clear a renderer byte believed to hold visibility state | It is the freeze-frame flag; clearing it drew an empty white frame |

## Speed-ups that broke the picture

| Skip name | What it tried | What happened |
|---|---|---|
| `hl2vr_ham` | Hidden-area mesh (mask the lens corners) as early-Z | Drew a black overlay on the world, no frame-time gain |
| `hl2vr_pixelvis` | Skip the pixel-visibility (glow/sprite) queries for the second eye | 2D skybox missing in one eye, flashlight through walls. Each eye needs its own queries, not fewer |
| `hl2vr_predraw` | Draw every material once after map load to warm caches | Null call inside shaderapidx9 (crash) |
| `hl2vr_playercull` | Widen the local player's render bounds | Left off since 2026-09-05 as a leftover culling tweak; nothing needed it |
| `hl2vr_proj`, `hl2vr_cproj`, `hl2vr_neye` | Native OpenVR stereo projection (reverted) | Replaced by the per-eye path above |

Also removed: the `HiddenAreaMesh` config key (parsed, never used).
