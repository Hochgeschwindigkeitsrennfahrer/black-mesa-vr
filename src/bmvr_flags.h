#pragma once
#include <Windows.h>
#include <cstdint>

// Crash-sticky retries of L4D2VR mechanisms the old OpenXR prototype banned.
// If Black Mesa dies mid-attempt, the in-progress flag remains on disk and
// that attempt is skipped next launch. Successful completion deletes the flag.
// Durable skips also live in bmvr_skip.txt next to bms.exe (probe loop + DLL).

namespace bmvr
{
    void SetDllModule(HMODULE module);
    HMODULE DllModule();
    void InitFromDisk();
    void StartWatchdog();

    void Log(const char* fmt, ...);
    void SetStage(const char* stage);

    bool TryHmdSwapchain();
    bool TryHmdFramebuffer();
    bool TryHmdNative();
    bool TryNamedRenderTargets();
    bool TryNamedStereoWrap();
    bool TryStereoRenderView();
    bool TryWaitDeviceIdle();
    bool TryAbsoluteHmdView();
    bool TryMenuCompositor();
    bool TryRelativeHmdLook();
    bool TryStereoCopy();
    bool TryStereoFov();
    bool TryMatQueue();
    bool TrySteamVrEyeRt();
    bool TryOffscreenHmd();
    // LITERAL FullFrame/G-buffer at SteamVR rec. The 2026-08-26 miss
    // (worldMatch+redirect drew HWND 2560x1440 into 2544x2480) is explained:
    // FullFrame never grew, so worldMatch stayed 0 and the viewport/view-lock
    // fixes never engaged. Retried behind WorldRenderAtEyeSize in config.txt.
    bool TryOffscreenWorldGrow();
    // Grow gate for `_rt_FullFrameFB*`. FullFrame is created at SetMode, before
    // Game hooks. Early CreateNamedRT (InstallEarlyFramebufferHook) must be
    // live by then. Menu maps no longer block grow — New Game reused the
    // background01 HWND RTs and left worldMatch=0 (G2, 2026-09-05).
    bool WorldRtGrowActive();
    // Peeked from GetLevelNameShort during CreateNamedRT / LevelInit.
    // background* still blocks OffscreenWorldMatchesEyes (menu 2D path).
    void NoteEngineMapName(const char* map);
    // gbmatch keeps CViewSetup at HWND/G-buffer size unless FullFrame and
    // G-buffers actually allocated at offscreen eye size (native HMD pixels).
    bool UseGbMatchViewLock();
    bool OffscreenWorldMatchesEyes();
    // OpenXR helper session: stereo views must use HMD-aspect pixels so ATW
    // does not warp a 16:9 framebuffer. Disables the flashlight gbmatch lock.
    void SetOpenXrHelperSession(bool active);

    void DisableNamedRenderTargets(const char* reason);
    void DisableStereoRenderView(const char* reason);
    void DisableStereoFov(const char* reason);
    void PersistSkip(const char* name, const char* reason);

    void BeginRisky(const wchar_t* name);
    void EndRisky(const wchar_t* name);

    // L4D2VR/Portal2: G-buffer = OpenVR recommended size. Window-fit is only
    // the fallback if recommended size crash-stickies. ApplyHmdAspectBackbuffer
    // rewrites the windowed D3D backbuffer to that size. GetScreenSize must
    // return the same values or HUD composite uses 16:9 CViewSetup on HMD RTs.
    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect);
    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH);
    // GMod/L4D2VR: OpenVR recommended * RenderScale, 16-aligned, cap 4096.
    // Independent of HWND. False if offscreen path is skipped or rec is unknown.
    bool ComputeOffscreenEyeSize(uint32_t& width, uint32_t& height);
    // `_rt_FullFrameFB*` target size. False unless WorldRtGrowActive.
    bool ComputeGrownWorldFramebuffer(uint32_t& width, uint32_t& height);
    // `_rt_gb*` target size. Additionally requires FullFrame to already be at
    // eye size, so the two never disagree (that mismatch is the hmd_world warp).
    bool ComputeGrownWorldGbuffer(uint32_t& width, uint32_t& height);
    // Size to pass to SetRenderTargetFrameBufferSizeOverrides. Eye size only
    // after FullFrame already allocated at that size. Otherwise false so the
    // caller pins HWND — advertising 3168 here before FullFrame grew is what
    // created 3168 G-buffers behind a 2560 FullFrame (flashlight + ghost world,
    // 2026-09-01).
    bool ComputeWorldRtOverrideSize(uint32_t& width, uint32_t& height);
    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height);
    // Cached game HWND (Valve001 / "Black Mesa"). Null while the window is
    // missing; re-searched at most every 250 ms. Never call FindWindow per
    // D3D call — see QueryWindowClientSize.
    HWND GameWindow();
    // Client size of GameWindow(), cached for a few ms. Thread-safe.
    bool QueryWindowClientSize(uint32_t& width, uint32_t& height);
    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height);
    void InstallEarlyFramebufferHook();
    void SetGameplayWorldRts(bool gameplayMap);

    extern uint32_t g_RecommendedEyeWidth;
    extern uint32_t g_RecommendedEyeHeight;
    extern uint32_t g_FramebufferWidth;
    extern uint32_t g_FramebufferHeight;
    extern bool g_OpenVRInitedFromCreateDevice;
    extern float g_RenderScale;
    extern float g_TurnSpeed;
    extern bool g_SnapTurning;
    extern float g_SnapTurnAngle;
    // sd805 / Portal 2 uncoupled viewmodel (Source units along controller axes).
    extern float g_ViewmodelPosOffsetX;
    extern float g_ViewmodelPosOffsetY;
    extern float g_ViewmodelPosOffsetZ;
    // Touch OpenXR aim origin sits ahead of grip. Extra ox pulls the gun
    // back along -forward (same as numpad 7/9). G2/Index/Vive stay 0.
    extern float g_ViewmodelPosOffsetXTouch;
    extern float g_ViewmodelAngOffsetX;
    extern float g_ViewmodelAngOffsetY;
    extern float g_ViewmodelAngOffsetZ;
    // L4D2VR tilts Vive wands -45°. G2 points more forward; default -35.
    // Quest/Touch OpenXR aim already points; that family uses
    // ControllerPitchTiltTouch (0) instead of this G2 default.
    extern float g_ControllerPitchTilt;
    extern float g_ControllerPitchTiltTouch;
    extern float g_ControllerPitchTiltIndex;
    extern float g_ControllerPitchTiltVive;
    float EffectiveControllerPitchTilt(uint32_t controllerFamily);
    // Trim applied to the firing pitch only, in degrees, positive = shoot
    // higher. Leaves the viewmodel alone, so it corrects a grip that aims low
    // without disturbing tuned weapon poses. Live-tune with Ctrl+Numpad +/-.
    extern float g_AimPitchOffset;
    // Drop the weapon-recoil term from the server's shot direction so sustained
    // fire stops walking upward. The viewmodel still kicks.
    extern bool g_DisableRecoilAim;
    extern float g_IPDScale;
    extern float g_HeightOffset;
    extern bool g_AutoMatQueueMode;
    // Default play path: 1x-eye stereo + SetThreadMode(2) after warmup.
    // Menu / load stay queue 0. Config MulticoreMode=false turns it off.
    extern bool g_MulticoreMode;
    // L4D2VR overlay AntiAliasing: 0 / 2 / 4 / 8 / 16. DXVK MSAA on the
    // private eye RTs, resolved into non-MSAA submit textures. Restart.
    extern uint32_t g_AntiAliasing;
    extern bool g_Haptics;
    extern bool g_HideCrosshair;
    extern bool g_MatchHmdHz;
    extern bool g_DisableViewBob;
    extern bool g_LeftHanded;
    extern bool g_RecenterResetsYaw;
    extern bool g_HideLocalPlayerModel;
    // Hybrid VR hands: hide FP `arms` bodypart by zeroing studiohdr nummeshes
    // (not MATERIAL_VAR_NO_DRAW). Opposite of L4D2VR NativeViewmodelHandsOnly.
    extern bool g_HideViewmodelArms;
    // SteamVR vr_glove_*.glb on each controller (plan §4.3.2). Default on for
    // BM hybrid; L4D2VR sample ships gloves off / native IK on.
    extern bool g_VrHandsGlovesEnabled;
    // Temporary: right HEV glove off while holding a gun. Left stays on.
    // Both hands still draw when inventory is empty (intro tram, after HEV
    // before the first weapon).
    extern bool g_VrHandsRightEnabled;
    extern float g_VrHandsModelScale;
    // Debug boxes at each controller. Drawn if gloves are off, or if GLB draw
    // fails. Leave off once gloves are verified.
    extern bool g_VrHandsDebugBoxes;
    // Wrist HUD: health+suit flat on the off-hand, ammo sideways beside the gun hand.
    extern bool g_HandHud;
    // World-space aim reticle at the controller firing ray. Off by default;
    // VrCrosshair=true in config.txt turns it on.
    extern bool g_VrCrosshair;
    // Reticle size multiplier, for players who want it smaller or bolder.
    extern float g_VrCrosshairScale;
    // Hide HEV gloves and wrist HUD until the HEV suit is picked up. Bare-hand
    // GLBs still draw on the intro tram. Depends on a scanned m_bWearingSuit
    // offset; set false if the scan ever misfires.
    extern bool g_HideHandsWithoutSuit;
    // While scoped (crossbow), aim from the headset instead of the controller so
    // bolts land in the middle of the scope picture.
    extern bool g_ScopeUsesHmdAim;
    // Stereo world FOV as a fraction of the HMD FOV while the crossbow is
    // zoomed. Compositor / OpenXR submit keep the real HMD FOV, so the
    // narrower render is magnified in the lenses. 0.28 ≈ 3.5× (HL2 20/75).
    extern float g_ScopeZoomFovScale;
    // Exponential smoothing time (seconds) applied to HMD pitch/yaw while
    // scoped. Magnification otherwise turns every micro-movement into shake.
    extern float g_ScopeZoomSmoothSec;
    // SteamVR glove local Rx,Ry,Rz (degrees) inside BuildControllerWorld.
    // Default yaw 180: OpenVR glove +Z is opposite Source controller forward,
    // which made the mesh point backward and clip the near plane (mostly invisible).
    extern float g_VrHandsPoseRotX;
    extern float g_VrHandsPoseRotY;
    extern float g_VrHandsPoseRotZ;
    // Index OpenXR: aim +Y is the controller face, ~90° from the palm.
    // Applied only to hand poses around aim forward (weapons stay unrolled).
    // Left = -deg, right = +deg. Flip the sign if palms go the wrong way.
    extern float g_VrHandsIndexRollDeg;
    // L4D2VR BuildControllerWorld local translation (meters, before model
    // scale). Controller basis Z is -forward, so a negative local Z moves the
    // mesh further along aim. HEV gloves sat behind the controller; -0.10 m
    // is ~60% of a ~16 cm scaled hand along aim.
    extern float g_VrHandsPoseOffX;
    extern float g_VrHandsPoseOffY;
    extern float g_VrHandsPoseOffZ;
    extern float g_VrHandsTouchOffX;
    extern float g_VrHandsTouchOffY;
    extern float g_VrHandsTouchOffZ;
    void EffectiveVrHandsPoseOffset(uint32_t controllerFamily, float& x, float& y, float& z);
    extern float g_VrHandsLeftPoseOffX;
    extern float g_VrHandsLeftPoseOffY;
    extern float g_VrHandsLeftPoseOffZ;
    extern float g_VrHandsRightPoseOffX;
    extern float g_VrHandsRightPoseOffY;
    extern float g_VrHandsRightPoseOffZ;
    extern float g_VrHandsRightGripRotX;
    extern float g_VrHandsRightGripRotY;
    extern float g_VrHandsRightGripRotZ;
    // Prefer ripped HEV GLBs in VR/hands when present.
    extern bool g_VrHandsUseHevGloves;
    // True for this process when launched with -game bshift (or a bs_* map).
    // Calhoun never wears the HEV suit — use bare-hand GLBs the whole session.
    extern bool g_IsBlueShift;
    bool IsBlueShift();
    // false = runtime PostPresentHandoff. true = L4D2VR app handoff (default).
    extern bool g_CompositorPostPresentHandoff;
    // Old stereo vis workaround: r_portalsopenall + r_occlusion 0. Default
    // off — that pair draws the whole map and tanks open/complex areas.
    extern bool g_ForceOpenVis;
    // Hidden-area mesh early-Z after D3DCLEAR_ZBUFFER. Verified miss
    // 2026-09-05: OpenXR visibility-mask draw was a black overlay, no FPS
    // gain. Persist-skipped (hl2vr_ham). HiddenAreaMesh=true does not retry.
    extern bool g_HiddenAreaMesh;
    // Dummy-draw materials after LevelInit. Verified miss 2026-09-05:
    // DrawScreenSpaceRectangle null-called shaderapidx9 (C0000005 eip=0)
    // and leftover hmd_offscreen dropped the next launch onto HWND gbmatch.
    extern bool g_MaterialPredraw;
    // CPU-wait the GPU after the left-eye StretchRect. The HMD-fb blit path
    // always flushes regardless of this flag (shared FullFrame/BB hazard).
    extern bool g_StereoBlitGpuFlush;
    // OpenXR helper eye publish: copy eyes into a slot, issue a GPU event and
    // hand the slot over on a later Present once the event signalled, instead
    // of vkDeviceWaitIdle on every publish (CPU/GPU serialisation). Default
    // false: HMD runs 2026-09-06 showed deferred=true raises fps but breaks
    // WMR motion smoothing, so framedrops feel like stutter. true keeps the
    // throughput path for A/B.
    extern bool g_OpenXrDeferredPublish;
    // Minimum ms a publish slot rests after it stopped being the helper's
    // visible slot before the game writes it again. Safety margin only: the
    // real gate is the helper's consumed frame id (bridge v14).
    extern float g_OpenXrSlotCoolingMs;
    // Deferred publish run-ahead: how many eye copies may be in flight on the
    // GPU before Present blocks on the oldest one. 1 = CPU at most one frame
    // ahead of the GPU (pose paired with a frame is <= 1 GPU frame old when
    // the helper gets it). 2-3 trade latency and reprojection quality for
    // throughput (measured 2026-09-06: unbounded run-ahead sat at 2 pending
    // frames on a 90Hz WMR headset at ~75fps and felt like judder). Range 1-3.
    extern uint32_t g_OpenXrMaxPending;
    // World-space v_ models vs world props. Applied in DrawModelExecute around
    // the controller (not m_flModelScale at entity origin). Range 0.2–1.5.
    extern float g_ViewmodelScale;
    extern float g_HudMaxFov;
    extern float g_HudDisplayRatio;
    extern float g_HudDistance;
    extern float g_HudSize;
    // 2D GameUI in the HMD: fraction of the full-width letterbox (1 = glued to
    // the view). ~0.70 is a closer monitor; lower = farther / smaller.
    extern float g_MenuPanelScale;
    // HL2VR hlvr_menu_forward / overlay width, in metres. Defaults match the
    // OpenXR helper 2D quad (0.65 m × 1.05 m, 2026-09-03). Do not copy
    // HL2VR's 150 Source-unit (~3.8 m) distance — that was too far here.
    extern float g_MenuForwardMeters;
    extern float g_MenuWidthMeters;
    extern float g_MenuHeightOffsetMeters;
    // Seconds of low-pass on the VR menu pointer (hand tremor).
    extern float g_MenuCursorSmoothSec;

    bool TryHudOverlay();
    bool TryVguiPaint();
    bool TryGameUiActivate();
    bool TryMeleeTrace();
    bool TryFramebufferOverride();
    bool TryFullFrameStereo();
    // 47777b5 same-buffer: FullFrame/G-buffer LITERAL at window-capped HMD-fit
    // (~1584x1440). Independent of ff_stereo (broken grow). Sticky ff_hmdfit.
    bool TryHmdFitFullFrame();
    // LITERAL FullFrame AND G-buffer at eye size (1584). Verified miss
    // 2026-08-22 (died on background04 before stereo). Do not retry.
    bool TryEyeFitWorldRts();
    // Stereo view stays 2560 (G-buffer size) with HMD fov/aspect/IPD so the
    // deferred flashlight apply runs, then squash-blit into 1584 eyes.
    // Sticky fl_gbmatch. Not ff_hmdfit / ff_gbfit / 16:9 stereo.
    bool TryFlashlightGbMatch();
    // Under gbmatch, skip the leftover 16:9 desktop main (2 renders/frame).
    // Own sticky gb_leftskip; a death falls back to 3-render gbmatch.
    bool TryGbLeftSkip();
    bool TryDrawHud();
    bool TryDrawModelExecute();
    // HL2VR: QueueCall WaitGetPoses onto ICallQueue. Sticky disables queued
    // WGP only — sync WaitGetPoses on the calling thread stays on.
    bool TryHl2vrQueuedWgp();
    // Write CViewSetup.m_eStereoEye LEFT/RIGHT (1/2). Never a pixel height.
    bool TryHl2vrStereoEye();
    // OpenVR GetHiddenAreaMesh early-Z after D3DCLEAR_ZBUFFER (3adc23b2).
    // Persist-skipped: black overlay, 2026-09-05.
    bool TryHl2vrHam();
    // PixelVisibility_EndCurrentView once per stereo pair (26a5fe68).
    // Persist-skipped: second-eye skip, one-eye 2D skybox miss, 2026-09-05.
    bool TryHl2vrPixelVis();
    // CVRViewRender::DrawAllMaterials dummy draw after map load (aeaf3f03).
    bool TryHl2vrPredraw();
    // C_BaseVRPlayer::GetRenderBoundsWorldspace ±1000 (feb36883). Off —
    // leftover cull-opt from 2026-09-05. Do not re-arm.
    bool TryHl2vrPlayerCull();
    // Do not zero CViewRender+0x744. That byte is m_rbTakeFreezeFrame
    // (RenderView cmp + m_flFreezeFrameUntil at +0x748). Persist-skipped:
    // empty freeze / white void, 2026-09-05.
    bool TryStereoVisReset();
    // Apply OpenXR/OpenVR controller poses into tracking (viewmodel + gloves).
    bool TryCtrlPose();
    // CalcViewModelView hard-lock to the aim controller.
    bool TryCtrlVm();
    // Draw GLB gloves / independent hand meshes into the eye.
    bool TryVrGloves();
    // 2D wrist HUD / weapon menu / debug boxes on the stereo eye (FVF overlay).
    bool TryHandOverlay();

    extern uint32_t g_FullFrameActualWidth;
    extern uint32_t g_FullFrameActualHeight;
    extern uint32_t g_GbActualWidth;
    extern uint32_t g_GbActualHeight;
    // false (default) skips the leftover 16:9 desktop scene after stereo.
    extern bool g_DesktopLeftoverRender;
}
