#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
class Game;

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
    // New game UI study (2026-11-23): DXVK reports every shared texture or
    // surface (created or opened from a handle). Logged; the ones on the VR
    // (game) device are kept so `dump` can write them to VR/.
    void NoteSharedResource(void* device, IDirect3DTexture9* texture, IDirect3DSurface9* surface, const char* kind,
        bool opened, void* handle);
    // A shared texture was bound for drawing (SetTexture). On the game device
    // the last one bound in a frame is the top UI layer the game composites.
    void NoteSharedTextureBound(void* device, void* texture);
    // UI device window (second device, 1x1 back buffer), or nullptr.
    void SetNewUiWindow(void* hwnd);
    void* NewUiWindow();

    bool TryHmdFramebuffer();
    bool TryHmdNative();
    bool TryNamedRenderTargets();
    bool TryStereoRenderView();
    bool TryMenuCompositor();
    bool TryRelativeHmdLook();
    bool TryMatQueue();
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
    void PersistSkip(const char* name, const char* reason);

    void BeginRisky(const wchar_t* name);
    void EndRisky(const wchar_t* name);

    // Set around MaterialSystem CreateNamedRenderTarget for one scene RT.
    // DXVK stamps hlvr_msaa on color and depth created on this thread at
    // that size. Width/height 0 means "whatever size this call creates".
    // Eye MSAA post switch (implemented in DXVK d3d9_device.cpp): Retire at
    // the start of post-processing resolves the eye colour + depth once and
    // makes every later use of those surfaces 1x; Begin undoes it at eye start.
    void MsaaEyeRetire(IDirect3DDevice9* device, IDirect3DSurface9* color, IDirect3DSurface9* depth);
    void MsaaEyeBegin(IDirect3DDevice9* device, IDirect3DSurface9* color, IDirect3DSurface9* depth);
    // Audit: " ms=N ret=R pend=P sic=S" for a depth surface (eye MSAA state), "" when not multisampled.
    void MsaaDepthStateText(IDirect3DSurface9* depth, char* out, size_t outSize);

    struct Hl2vrMsaaScope
    {
        static int& Depth()
        {
            static thread_local int depth = 0;
            return depth;
        }
        static uint32_t& Width()
        {
            static thread_local uint32_t width = 0;
            return width;
        }
        static uint32_t& Height()
        {
            static thread_local uint32_t height = 0;
            return height;
        }
        static bool Active() { return Depth() > 0; }
    };

    class Hl2vrMsaaScopeGuard
    {
    public:
        Hl2vrMsaaScopeGuard(bool enable, uint32_t width, uint32_t height)
            : m_on(enable)
        {
            if (!m_on)
                return;
            int& depth = Hl2vrMsaaScope::Depth();
            if (depth == 0)
            {
                Hl2vrMsaaScope::Width() = width;
                Hl2vrMsaaScope::Height() = height;
            }
            ++depth;
        }
        ~Hl2vrMsaaScopeGuard()
        {
            if (m_on && Hl2vrMsaaScope::Depth() > 0)
                --Hl2vrMsaaScope::Depth();
        }
        Hl2vrMsaaScopeGuard(const Hl2vrMsaaScopeGuard&) = delete;
        Hl2vrMsaaScopeGuard& operator=(const Hl2vrMsaaScopeGuard&) = delete;
    private:
        bool m_on;
    };

    // L4D2VR/Portal2: G-buffer = OpenVR recommended size. Window-fit is only
    // the fallback if recommended size crash-stickies. GetScreenSize must
    // return the same values or HUD composite uses 16:9 CViewSetup on HMD RTs.
    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect);
    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH);
    // GMod/L4D2VR: OpenVR recommended * RenderScale, 16-aligned, cap 4096.
    // Independent of HWND. False if offscreen path is skipped or rec is unknown.
    bool ComputeOffscreenEyeSize(uint32_t& width, uint32_t& height);
    // Why OffscreenWorldMatchesEyes() is true or false, for the render-path log.
    void DescribeWorldRtMatch(char* buf, size_t size);
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
    // Explicit eye-sized render targets by exact name. Bloom: own depth,
    // aux class. Scene: keeps the engine's depth (it binds the G-buffer depth
    // with it) and counts as an eye-sized world target.
    enum class EyeRtClass { None, Bloom, Scene };
    EyeRtClass EyeScaledRtClass(const char* name);
    bool EyeScaledRtSize(const char* name, uint32_t& width, uint32_t& height);
    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height);
    // Cached game HWND (Valve001 / "Black Mesa"). Null while the window is
    // missing; re-searched at most every 250 ms. Never call FindWindow per
    // D3D call — see QueryWindowClientSize.
    HWND GameWindow();
    // Client size of GameWindow(), cached for a few ms. Thread-safe.
    bool QueryWindowClientSize(uint32_t& width, uint32_t& height);
    // The thread that refreshes that cache (main/RenderView thread, every frame); others only read it.
    void NoteClientSizeRefreshThread();
    void InvalidateClientSizeCache();  // device Reset: the window size may have changed
    void InstallEarlyFramebufferHook();
    void SetGameplayWorldRts(bool gameplayMap);

    extern uint32_t g_RecommendedEyeWidth;
    extern uint32_t g_RecommendedEyeHeight;
    extern uint32_t g_FramebufferWidth;
    extern uint32_t g_FramebufferHeight;
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
    // Player's extra gun pitch in degrees on top of the per-family tilt
    // (GunPitchOffset in config.txt, bmvr_gun_pitch / Options -> Game).
    extern float g_GunPitchOffset;
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
    // HL2VR hlvr_msaa. 0 is off; 2 / 4 / 8 / 16 are sample counts. Applied in
    // DXVK to separate-depth scene render targets (not the swapchain, not the
    // private eye copies). Set by VR/config.txt `Msaa`. The Advanced Video
    // dropdown stays stock (None / FXAA); rewriting it crashes that page.
    // A crash during the scoped create persist-skips it (g_MsaaAllowed = false).
    extern uint32_t g_Msaa;
    extern bool g_AlphaToCoverage;   // AlphaToCoverage (default true, needs Msaa): alpha-tested draws use the eye MSAA samples (foliage, fences)
    extern float g_LodBias;          // LodBias (default 0): extra mip LOD bias in the 3D views (softer textures; foliage/fences more solid)
    extern bool g_RefractOncePerEye; // RefractOncePerEye (default true): glass refraction copied once per eye
    extern float g_RefractCopiesPerEye; // RefractCopiesPerEye (1..20, default 6): copies per eye RefractOncePerEye lets through
    extern float g_MsaaF;           // VR panel copy of g_Msaa (0/2/4); applied live by recreating the eyes
    extern bool g_MsaaParticles1x; // MsaaParticles1x (default true, needs Msaa): particles drawn last, on the 1x resolve (the gun too in frames with particles)
    extern bool g_MsaaPostSwitch;   // MsaaPostSwitch (default true): post chain on the 1x resolve
    // PostCopySwap (default true, panel DEBUG > GPU SAVINGS): the post chain's
    // eye -> _rt_FullFrameFB copies swap image storage in DXVK instead of copying
    // (same image; the copy still runs if anything reads the eye before the
    // next full-screen pass). The arm values are the thread id of the hook
    // that may swap / overwrite, 0 = not armed.
    extern bool g_PostCopySwap;
    // ShadowsOncePerFrame (default true, panel DEBUG > SAVINGS): the deferred lights' shadow
    // atlas tiles are rendered by the first eye only; the second eye skips a light whose
    // render call (light, atlas slot, flags) matches one the first eye made in this stereo
    // pair (same frame, same content). Saves engine CPU (shadow views) and GPU.
    extern bool g_ShadowsOncePerFrame;
    // Bumped once per stereo pair on the main thread (dRenderView), before the first eye.
    extern int g_StereoPairSeq;
    // bmvr_shadows_once_test (test, not saved): the second eye skips every light shadow, matched or not.
    extern bool g_ShadowsOnceTestSkipAll;
    // bmvr_cpu_sample <s> (test, not saved): sample main + render thread CPU for s seconds, log the result.
    extern float g_CpuSampleSeconds;
    // PostCopySwapAlpha (cvar bmvr_post_copy_swap_alpha, default 1): 0 = an RGBA write is
    // needed to drop the debt, 1 = RGB-only writes count (eye alpha mid-post is
    // not needed), 2/3 = as 1 and the owed eye is cleared to alpha 0/1 (test).
    extern float g_PostCopySwapAlpha;
    extern volatile unsigned long g_PostSwapArmThread;
    extern volatile unsigned long g_FullOverwriteArmThread;
    extern bool g_MsaaAllowed;
    uint32_t NormalizeMsaa(int samples);
    extern bool g_Haptics;
    // Last round out of the gun (LastShotHaptic): 0 = the normal shot buzz, 1 = a double
    // buzz (default), 2 = one longer buzz. Amplitude = LastShotHapticStrength (0.05-1).
    extern float g_LastShotHaptic;
    extern float g_LastShotHapticStrength;
    // Trigger pulled on an empty gun (DryFireHaptic): 0 = no buzz, 1 = one tick,
    // 2 = a quick "nope" double tick (default). Amplitude = DryFireHapticStrength (0.05-1).
    extern float g_DryFireHaptic;
    extern float g_DryFireHapticStrength;
    extern bool g_HideCrosshair;
    extern bool g_MatchHmdHz;
    extern bool g_DisableViewBob;
    extern bool g_LeftHanded;
    // AutoRecenterOnResume (default true): closing the pause menu (GameUI) in
    // a map recenters the head position (not the turn direction), so a walk
    // away from the play spot while paused does not leave the eye in walls.
    extern bool g_AutoRecenterOnResume;
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
    // Red flash on the eye edges when health or armor drops; strength = peak alpha.
    extern bool g_DamageFlash;
    extern float g_DamageFlashStrength;
    // Off-hand (health/suit) panel placement, relative to the controller after
    // the tilt/roll: size and offsets in game units, angles in degrees. Roll is
    // mirrored for a right-hand panel (left-handed play).
    extern float g_HandHudSize;
    extern float g_HandHudUp;
    extern float g_HandHudForward;
    extern float g_HandHudSide;
    extern float g_HandHudTilt;
    extern float g_HandHudRoll;
    // World-space aim reticle at the controller firing ray. Off by default;
    // VrCrosshair=true in config.txt turns it on.
    extern bool g_VrCrosshair;
    // Reticle size multiplier, for players who want it smaller or bolder.
    extern float g_VrCrosshairScale;
    // Reticle drawn as a red laser dot instead of the amber ticks.
    extern bool g_VrCrosshairDot;
    // Thin red beam from the gun along the firing ray to where it lands.
    extern bool g_VrLaserBeam;
    // Beam half width in game units (1 unit ~ 2.5 cm).
    extern float g_VrLaserWidth;
    // Moves the beam start along the barrel (game units, + = forward).
    extern float g_VrLaserStartForward;
    // AimDebug=true: per shot, log the server bullet origin/direction and its
    // traced hit next to the reticle ray and hit (bmvr_log "AimDebug").
    extern bool g_AimDebug;
    // OffCenterTest=true (flat only, diagnostic): while VR/offcenter_test_on
    // exists, the main view renders the PSVR2 left-eye off-centre subrect
    // (left 0, right 0.758 of the frustum) to check the engine's off-centre
    // projection path (deferred lights, sky, water, viewmodel) on the desktop.
    extern bool g_OffCenterTest;
    // OffCenterEyes=true: each eye renders only its own asymmetric HMD
    // frustum (CViewSetup m_bOffCenter) instead of the symmetric union, and
    // is submitted at the runtime's per-eye FOV. PSVR2: ~54 % -> 100 % of
    // the rendered pixels reach the eye.
    extern bool g_OffCenterEyes;
    bool OffCenterTestActive();
    // Headless tests. BMVR_TESTDIR (a Windows path) stands in for the game
    // folder for VR\config.txt, the logs, heartbeat, skip and crash-sticky
    // flag files, so a test never reads or writes the player's files.
    const std::wstring& TestDir();
    // "<TestDir>\VR\<leaf>" when BMVR_TESTDIR is set, else "VR\<leaf>"
    // (relative to the game folder, as before).
    std::string VrFilePath(const char* leaf);
    // VRRuntimeBackend=fake: the OpenXR helper path with an in-process fake
    // helper (scripted poses/buttons from VR\fake.txt, PSVR2 projection, no
    // SteamVR). See vr_fake_helper.cpp.
    extern bool g_FakeRuntime;
    // MirrorBothEyes=true: the desktop window shows both eye images side by
    // side, each whole eye texture stretched into one half (tests take
    // screenshots of it).
    extern bool g_MirrorBothEyes;
    // VrShotOriginFix=true (default): bullets fired through server player
    // vtable +0x474 leave from the controller ray (TryGetVrShootOrigin), not
    // the head. false = old behaviour (tests compare both).
    extern bool g_VrShotOriginFix;
    // Shots start at the muzzle attachment (not slid onto the controller axis).
    extern bool g_VrShootFromMuzzle;
    // Fixes, default on (a config key can turn each off):
    extern bool g_TwoHandHoldWhileGrip;  // long guns stay two-handed while the grip is held
    extern bool g_TwoHandGripToggle;     // support grip latches: squeeze on the gun = on, squeeze again = off
    // Pistols (glock, .357): one shot per trigger pull; holding fires no more (off = hold repeats, game default).
    extern bool g_VrPistolSemiAuto;
    // Glock: a pull within VrGlockRapidWindow s of the last one fires Black Mesa's alt-fire (fast, wide spread).
    extern bool g_VrGlockRapidAlt;
    extern unsigned long g_ScreenshotAt;     // VR panel "Screenshot": GetTickCount when to dump the eyes (0 = none)
    extern unsigned long g_SteamScreenshotAt; // gesture screenshot: GetTickCount when to send the right eye to Steam (0 = none)
    extern unsigned long g_DebugCaptureAt;   // VR panel "Capture debug frame": GetTickCount when to capture (0 = none)
    extern bool g_FlareLog;              // VR panel DEBUG "Flare position log" / bmvr_flare_log / env BMVR_FLARE_LOG=1: log lens flare positions + visibility; each switch-on starts fresh line caps (not saved)
    extern bool g_FreezeHead;            // VR panel debug row / bmvr_freeze_head: game view keeps the head pose of the moment it was switched on (not saved)
    extern float g_DebugHideXog;         // VR panel debug row: skip Xen fog draws, 1 = global (__gbxog0), 2 = volumes, 3 = both (not saved)
    extern float g_DebugBlankEye;        // VR panel debug row: 1 = blank the left eye, 2 = the right (not saved)
    extern bool g_PauseMenuColourFromFrame; // pause-menu layer colour from the presented desktop frame (VR repaint stacks windows wrongly)
    extern float g_PauseMenuAlphaPasses; // pause-menu layer: N passes of a -> a + a(1-a), i.e. 1-(1-a)^(2^N); 0 = off
    // Flashlight from (FlashlightFrom): 0 = right (gun) hand (default), 1 = left hand (along the
    // gun while two-handed), 2 = head (the light follows the view, as in the flat game).
    // The old FlashlightLeftHand=true maps to 1.
    extern float g_FlashlightFrom;
    extern bool g_FlashlightOnSquare;    // left Square toggles the flashlight (replaces previous weapon)
    extern float g_FrameCapMode;         // fps_max: 0 = game's own, 1 = headset rate + 10, 2 = 2x headset rate
    extern bool g_VrHandThrow;           // frag leaves the hand at the hand's speed (Lambda1VR-style throw)
    extern float g_MountedGunGain;       // mounted gun: barrel turn per degree of hand swing around the mount (1 = handle stays in the hand)
    extern float g_SnarkThrowScale;      // snark hand throw: x this on top of the frag throw speed
    extern float g_ThrowScale;           // hand speed x 2/3 x this, capped at 1500 u/s (Lambda1VR vr_throw_scale)
    extern bool g_VrSkipMotionBlur;      // skip the engine motion blur pass in VR
    extern bool g_VrSkipScreenEffects;   // skip env_screeneffect overlays (stun / intro blur) in VR
    // EnvmapScaleWorld / EnvmapScaleModels (0..1, default 0.4 / 0.8): cubemap reflection
    // strength ($envmaptint) of brush materials / model materials in VR.
    // models/weapons/* (view models, hands) are never scaled.
    extern float g_EnvmapScaleWorld;
    extern float g_EnvmapScaleModels;
    extern bool g_StereoEnginePost;      // run engine_post (tonemap / colour correction / FXAA) on eyes, bloom zeroed
    // MaxEyeSize (default 4096, 2048..8192): largest eye RT side. Both axes
    // shrink by one factor when RenderScale asks for more. MulticoreMode also
    // caps the height at 5456 (ring = 3 eye heights, DXVK max 16384).
    extern uint32_t g_MaxEyeSize;
    // ReflectionRtScale (default 0.5; 0 = engine 1024x1024; 0.25..1 = fraction
    // of the eye size): grow _rt_Water*, *Reflect*/*Refract* and _rt_PowerOfTwoFB.
    // At 1024 mirrored floors / refract glass shimmered at the edges; 0.5 fixes
    // that at an acceptable cost in the headset (2026-10-30).
    extern float g_ReflectionRtScale;
    // RefractCopyRtScale (default -1 = same as ReflectionRtScale; 0 = engine
    // 1024x1024; 0.25..1 = fraction of the eye size): _rt_PowerOfTwoFB only,
    // the screen copy refract materials (shiny floors, wired glass) sample.
    // It is a copy, not a scene render, so it is cheap to grow on its own.
    extern float g_RefractCopyRtScale;
    float RefractCopyRtScale();
    // Size _rt_PowerOfTwoFB was grown to (0 = not grown).
    extern int g_RefractCopyRtW;
    extern int g_RefractCopyRtH;
    // w x h is the grown reflection set or the grown refract copy.
    bool GrownAuxRtSized(int w, int h);
    // Size the reflection set was grown to (0 = not grown). Set by the
    // CreateNamedRT hook; read by the viewport/scissor/clear/constant hooks.
    extern int g_ReflectionRtW;
    // GpuPassTiming (default false): log
    // GPU ms per frame for eye-sized RTs, the reflection set and refract
    // copies, per eye, every ~5 s (D3D timestamp queries, no stalls).
    extern bool g_GpuPassTiming;
    // GpuPassDetail (default false, test aid): GpuPassTiming split by every
    // RT0 and material group (Bind hook on) plus copies and eye MSAA
    // switches, per eye, logged every ~10 s as "GPU pass" lines.
    extern bool g_GpuPassDetail;
    // Foveated rendering (VK_KHR_fragment_shading_rate rate image per eye,
    // DXVK d3d9_device.cpp BmvrFoveationSelect). Foveated: 0 off, 1 fixed
    // rings around each eye's lens centre, 2 eye-tracked (centre follows the
    // OpenXR eye gaze; lens centre while there is none). FoveatedGazeStep:
    // gaze change (tangent units) before the centre moves. The GPU feature is only enabled
    // when Foveated > 0 at game start (g_FoveatedAtStart); later the panel
    // switches it live. Rings in units of half the eye height on the PS VR2:
    // inside Inner full rate, to Mid 2x1/1x2, to Outer 2x2, beyond 4x4 (driver
    // clamps to its largest rate, 2x2 on RDNA4). g_FoveatedRingScale (set per
    // eye pass) keeps them the same angle on other headsets: PS VR2 vertical
    // eye span (tangent) / this headset's.
    extern float g_Foveated;
    extern bool g_FoveatedAtStart;
    extern float g_FoveatedInner;
    extern float g_FoveatedMid;
    extern float g_FoveatedOuter;
    extern float g_FoveatedRingScale;
    // Panel/log status: "off", "eye-tracked", "fixed (no eye tracking)", ...
    std::string FoveationStatusText();
    extern float g_FoveatedGazeStep;
    // Called on the D3D thread after RT0 changes: stereoEye 1/2 = this eye's
    // world pass (rate image on eye-sized targets), 0 = none. centreU/V =
    // lens centre in the eye texture (0..1, v down).
    void FoveationSelect(IDirect3DDevice9* device, int stereoEye, uint32_t eyeW, uint32_t eyeH, float centreU, float centreV);
    bool FoveationAvailable(IDirect3DDevice9* device);
    // PerfLog (default false): playtest performance log. Turns on GpuPassTiming
    // and the engine world batch counters (per eye/view: surface groups, mesh
    // changes vs the engine's 127 limit, player position) and the glow
    // pixel-visibility stats (drops, swings by head roll) every ~5 s.
    extern bool g_PerfLog;
    extern int g_ReflectionRtH;
    // Test aid: frames left to log every render pass (VR/trace.txt, headless runs only).
    extern int g_PassTraceFrames;
    extern bool g_SprintAimedPose;       // sprint animations show the aimed idle pose
    extern float g_QuickDrawSec;         // after a weapon switch the draw wait is cut to this many seconds (< 0 = off)
    extern bool g_SkipDrawAnim;          // draw/holster/pickup animations show the idle pose
    // Game auto-switches (user 2026-11-23: both off). Off = a picked-up weapon goes to the
    // inventory only (CBM_SP_GameRules::FShouldSwitchWeapon -> false), and an emptied weapon stays in
    // the hand (ReloadOrSwitchWeapons sees ITEM_FLAG_NOAUTOSWITCHEMPTY on every weapon).
    extern bool g_AutoSwitchPickup;
    extern bool g_AutoSwitchEmpty;
    extern bool g_ThrowStroke;           // frag throw velocity from the throwing stroke (far point of the hold -> release)
    extern bool g_DeltaSeqIdle;          // a delta layer as the main viewmodel sequence shows idle (tau/crossbow)
    extern bool g_CrowbarHitDrawnBar;    // crowbar hit sweep along the drawn v_crowbar bar (not the old HL2VR grip frame)
    extern bool g_TwoHandNoLower;        // both hands on the gun: no lowered clip at friendlies, laser stays on
    extern bool g_TwoHandLoweredFollow;  // two-hand support glove follows the gun's lowered clip (aimed at a friendly)
    extern bool g_ScopeClipIdle;         // crossbow to/from-scope clips show idle (the clip moves the gun to the eye)
    extern bool g_MuteFidgetSounds;      // weapon fidget clips play no sound (the pose is frozen in VR)
    // Optional features, default off:
    // Grab zones (grip with the hand there). Every zone has a slot per hand
    // (user 2026-11-23): the left hand does an action, the right hand takes a
    // weapon. The forehead (head frame) has a left-hand slot only: the gun
    // hand aims and scopes there.
    enum GestureZoneId
    {
        kGestureShoulderLeft = 0,
        kGestureShoulderRight,
        kGestureHipLeft,        // right hand: just above it (cross draw)
        kGestureHipRight,       // left hand: just above it (cross draw)
        kGestureHead,
        kGestureZoneCount
    };
    constexpr int kGestureWeaponZoneCount = 4;   // all but the forehead
    // Left-hand actions. Ammo = the manual-reload ammo pouch (live only while
    // manual reload is on). Quick load needs a ~1 s grip hold.
    enum GestureAction
    {
        kGestureOff = -1,
        kGestureQuickSave = 0,
        kGestureFlashlight,
        kGestureAmmo,
        kGestureQuickLoad,
        kGestureScreenshot,
        kGestureRecenter,
        kGestureActionMax = kGestureRecenter
    };
    extern float g_GestureLeft[kGestureZoneCount];
    // Right-hand slots: -1 off, a weapon (index into GestureWeaponName / parts:
    // 0 crowbar, 1 pistol, 2 .357, 3 MP5, 4 shotgun, 5 crossbow, 6 RPG, 7 tau,
    // 8 gluon, 9 hivehand, 10 grenade), or kGestureDynamic: the zone holds the
    // weapon last put away there (g_GestureDyn, -1 = none yet; written by the
    // game). Floats for the panel's Choice rows. The old ShoulderGestures /
    // HipGestures switches and VrHolsterDynamic migrate once and are dropped.
    constexpr int kGestureWeaponCount = 11;
    constexpr int kGestureDynamic = kGestureWeaponCount;
    extern float g_GestureRight[kGestureWeaponZoneCount];
    extern float g_GestureDyn[kGestureWeaponZoneCount];
    const char* GestureLeftKey(int zone);    // config.txt keys
    const char* GestureRightKey(int zone);
    const char* GestureDynKey(int zone);
    const char* GestureZoneName(int zone);   // "left shoulder", ...
    const char* GestureActionName(int action);
    // The right-hand slot's weapon now (a Dynamic zone's stored one), -1 = none.
    int GestureWeaponAt(int zone);
    // Grab zones follow an estimated body yaw + neck point, not the headset
    // (debug A/B, not saved; on by default). Tunables in VR/config.txt and the
    // Debug tab TESTS rows.
    extern bool g_GestureBodyFrame;
    // MenuRoomFront (panel GENERAL > MENUS, saved; default off): main menu +
    // pause overlay face the room's front (STAGE forward) at eye height unless
    // the head is > 90 deg off it, and an open menu re-anchors when the head
    // moved/turned away. Off = the menu opens straight ahead of the head.
    extern bool g_MenuRoomFront;
    extern float g_BodyYawDeadzoneDeg;    // head turns this far without moving the body (35)
    extern float g_BodyYawHardLimitDeg;   // body dragged along past this (80)
    extern float g_BodyYawCatchUpSec;     // head held past the dead zone this long -> body turns (0.8)
    extern float g_BodyYawCatchUpDegPerSec; // turn speed while catching up (120)
    const char* GestureWeaponName(int index);
    // Name parts matched against the inventory's network / model names (nullptr-terminated).
    const char* const* GestureWeaponParts(int index);
    extern bool g_SettingsTriangleHold;  // Triangle/Y hold opens the VR settings panel, tap pauses on release
    extern bool g_WeaponMenuSettingsGear; // VR gear slot in the weapon menu (always shown without SettingsTriangleHold)
    extern bool g_WeaponMenuTilt;        // weapon menu opened low tilts back to face the eye
    // CrouchStickHold=true: right stick down crouches only while held
    // (default false = each push toggles crouch).
    extern bool g_CrouchStickHold;
    // StickJumpCrouchThreshold: how far (0..1) the right stick must go up/down
    // for jump/crouch (default 0.8); it also has to point within
    // StickJumpCrouchAngle degrees of straight up/down (default 30).
    extern float g_StickJumpCrouchThreshold;
    extern float g_StickJumpCrouchAngle;
    // TurnBlocksJumpCrouch=true: after the right stick went sideways (turn),
    // no stick jump/crouch until it returns near the centre.
    extern bool g_TurnBlocksJumpCrouch;
    // Button mapping: a button with a hold action mapped fires it after this
    // long (ms); released earlier = its press action. One global value.
    extern float g_ButtonHoldMs;
    // Button mapping on (OpenXR helper with raw inputs). false = the helper's
    // fixed bindings, as before mapping existed (fallback if a mapping bug
    // blocks a button).
    extern bool g_ButtonMapping;
    // VR panel CONTROLS tab: show each controller as a picture (SteamVR render
    // model) with a label per button, and label the controllers in your hands.
    // false (or no model found) = the plain list.
    extern bool g_ControlsPicture;
    // SprintToggle=true (default): one sprint click (left stick click) keeps
    // sprinting until the left stick returns to centre or sprint is clicked
    // again; false = hold the click. Fixed button layout only (ButtonMapping
    // off): with mapping, the "Sprint toggle" / "Sprint (hold)" actions pick
    // it, and an old SprintToggle=false becomes a Sprint (hold) mapping.
    extern bool g_SprintToggle;
    // VR settings panel pauses the game while open (setpause / unpause).
    extern bool g_VrSettingsPause;
    // First-run welcome ("hold <button> to open the VR menu") done; written by
    // the mod once the player opened the panel from it (VrWelcomeSeen=true).
    extern bool g_VrWelcomeSeen;
    // Logs (and resets) the access-violation count since the last call.
    void LogFaultStats();
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
    // Zoomed view starts on the laser's aim instead of straight ahead of the
    // head, keeping that offset while zoomed (config ScopeKeepsAimOffset).
    extern bool g_ScopeKeepsAimOffset;
    // Zoomed view points along the gun's aim every frame instead of the head
    // (config ScopeFollowsGun, VR panel). The head still sets the roll.
    extern bool g_ScopeFollowsGun;
    // Smoothing time in seconds for that gun-driven view (0 = raw aim).
    extern float g_ScopeGunSmoothSec;
    // Crossbow held with both hands and the gun hand brought within
    // ScopeRaiseMeters of the headset (pointing roughly where the head
    // looks) zooms in; taking it 0.10 m further away zooms out. Holds the
    // game's alt-fire (hold-to-zoom) meanwhile (config ScopeRaiseToZoom).
    extern bool g_ScopeRaiseToZoom;
    extern float g_ScopeRaiseMeters;
    // Eye the scoped crossbow bolt starts from (1 left, 2 right = default,
    // 0 = between the eyes). A scope is one-eyed: from the head centre the
    // bolt hit half an IPD beside the reticle in both eyes (2026-11-03).
    extern int g_ScopeEye;
    // ScopeMonoZoom (default on, user 2026-11-18): while the crossbow is
    // zoomed both eyes render from the head centre (zero IPD) and the bolt
    // starts there, so both eyes see the same picture and reticle. Stereo
    // zoomed 3.5x magnified the eyes' disparity: the target showed doubled
    // and only ScopeEye's copy matched the bolt (users closed the left eye).
    // ScopeEye then does nothing. Debug-tab A/B row, off = the old stereo zoom.
    extern bool g_ScopeMonoZoom;
    // LeftEyeCullPad (default 0 = off since 2026-10-30, max 0.25): the left eye's right
    // edge is pushed out by this fraction of the half-FOV tangent (off-centre
    // CViewSetup) and submitted at that FOV (OpenXR explicit tangents, OpenVR
    // texture bounds). Black Mesa culls props for the right eye with what it
    // computed for the left eye; the right eye sees ~IPD/z further right, so
    // near props popped in late at the far right (HMD 2026-10-17; a widened
    // left eye fixed it). 0.05 covers props from ~0.7 m at 64 mm IPD (PSVR2).
    // Works with and without OffCenterEyes. Same RT size, so the left eye
    // loses ~2.5 % horizontal sharpness at 0.05; no extra pixels.
    // The late props were the occlusion tracker reusing the first eye's
    // results (OcclusionTrackerPerEye fixes that at the source), so the pad is
    // off by default: it also made the left eye off-centre with OffCenterEyes
    // off, which exposed off-centre bugs (mirror views) on default settings.
    extern float g_LeftEyeCullPad;
    // MenuCursorHook (default true): GetCursorPos reports the VR menu pointer
    // while it is on a menu panel. false = real cursor (A/B test aid).
    extern bool g_MenuCursorHook;
    // DebugDumps (default false): poll VR/dump.txt and VR/trace.txt in a
    // normal session too (else headless tests only), so a headset session
    // can write full uncropped eye images plus the head pose of that frame.
    extern bool g_DebugDumps;
    // RightEyeFirst (default false, test aid, MulticoreMode only): record the
    // right eye's RenderView before the left one.
    extern bool g_RightEyeFirst;
    // bmvr_mc_frame_reset (default on): MulticoreMode resets the per-frame
    // "eyes drawn" / "poses waited" flags at the main thread's frame end
    // instead of the render thread's Present (which skipped every other
    // frame's view when the render thread lagged). 0 = old behaviour.
    extern bool g_McFrameReset;
    // bmvr_pose_flags_noblink (default on): HMD/controller "pose valid" flags
    // keep their last value while a tracking update runs (cleared only if the
    // update fails), so the other thread never sees a mid-update "invalid"
    // (laser from the forehead / hands gone for a frame). 0 = old behaviour.
    extern bool g_PoseFlagsNoBlink;
    // OcclusionTrackerPerEye (default true): Black Mesa's occlusion tracker
    // (r_occlusion_tracker_enabled) keys its results by view id, and both
    // eyes render as the same main view, so objects hidden from the first
    // eye were culled in the second too (pop-in at the far right and behind
    // doorways). The right eye's views get their own id, so they get their
    // own queries against their own depth. false = stock (A/B aid).
    extern bool g_OcclusionTrackerPerEye;
    // PixelVisPerEye (default true, cvar bmvr_pixelvis_per_eye): glows (light
    // halos, sprites, monitors) fade by a GPU visible-pixel query kept per view
    // id. Both eyes are the main view, so the right eye's query replaced the
    // left eye's and a glow outside one eye dropped out in both (flicker while
    // the head moves). The right eye gets its own queries for all its views
    // (incl. floor/water reflections). false = stock (A/B aid).
    extern bool g_PixelVisPerEye;
    // GlowCountDepthFix (default true): the glow queries' "pixels possible"
    // draw keeps its DepthRange(0, 0.01) instead of the viewmodel 0..1 rewrite;
    // false = glows behind objects show full bright when a sliver peeks out.
    extern bool g_GlowCountDepthFix;
    extern bool g_McHandSnapshot;
    extern bool g_SkyWaterPerEye;
    extern bool g_EyeUnderwaterWorld;
    // Eye at the water line (near plane crosses the surface): the eye view drops the engine's
    // "keep above the water" clip, so the G-buffer has the world under the surface for the
    // engine's water intersection view (false = stock: black/fog-green lower part of the view).
    extern bool g_WaterLineClipFix;
    // Screen-space quads (DrawScreenSpaceRectangle: underwater warp, post chain; lens flare layers) get an
    // identity model matrix around the draw. Both set identity view + projection but inherit the model
    // matrix; a leftover translation drew the warp / flares shifted for a whole launch (2026-11-17).
    extern bool g_ScreenSpaceModelFix;
    // Black Mesa's global Xen fog pass (__gbxog0) is not drawn in VR: view-direction noise at infinite stereo
    // depth, also inside rooms where flat shows none. Local fog volumes stay.
    extern bool g_XenGlobalFogOff;
    extern bool g_FlareEyeAxis;
    // Lens flare layers are turned so their streaks stay level with the world
    // horizon at the light, not with the eye's image (which rolls with the head).
    extern bool g_FlareLevel;
    // View-parallel sprites (light glows) face each eye with world up instead
    // of copying the eye's right/up (which roll and turn with the head).
    extern bool g_SpriteLevel;
    // Screen-aligned particles (spritecard: smoke, fog, steam) keep their
    // up with world up when the head rolls; they still turn with head yaw.
    extern bool g_ParticleLevel;
    // Muzzle flashes etc. follow the gun while moving (control point shifted
    // by the player's move since last frame's view).
    extern bool g_ViewmodelFxCatchUp;
    extern bool g_LightCullEyeView;
    extern bool g_FlareSharedLayout;
    extern float g_FlareHazeScale;
    extern float g_FlareHazeMinSize;
    extern float g_FlareGhostScale;
    extern float g_FlareFadeAngle;
    extern float g_FlareLightScale;
    // ViewProjNormalFix (default true, cvar bmvr_viewproj_normal_fix): vertex
    // shaders that turn normals into view space with cViewProj rows (Black
    // Mesa's rope g-buffer shader) get those rows without the off-centre
    // projection's forward part, so an off-centre eye (OffCenterEyes) writes
    // the same normal as a centred one. Without it the left eye's ropes went
    // dark over spotlit walls (normal-offset shadow bias into their own
    // shadow). false = stock rows (A/B aid). No effect on centred eyes.
    extern bool g_ViewProjNormalFix;
    // EyeBloom (default false, cvar bmvr_eye_bloom): test switch. true lets
    // the stock bloom chain (downsample / blur pyramid / bloomadd), the
    // auto-exposure histogram (lumcompare) and engine_post's bloom var run
    // on the eyes unchanged. false = the eye skip (2026-09-03 ghosts).
    extern bool g_EyeBloom;
    // FXAA quality parameters. The game never passes the mat_fxaa_* cvars to the
    // shader, so it runs with all of them at 0. FxaaApply (default true; false =
    // that stock behaviour) sends these instead. Values are FXAA 3.11's own terms.
    extern bool g_FxaaApply;
    // VR settings panel (vr_settings_panel.cpp): records every panel setting's
    // built-in default; called before config.txt is read. The panel then saves
    // only values that differ from it, so a later default change reaches players
    // who never touched that setting.
    void CapturePanelDefaults();
    // Debug capture: the VR panel's switches and engine settings as bm-scenario
    // "cmd" lines (bmvr_* cvars, engine cvars), so a headless restore runs with
    // exactly the session's settings. Game thread (reads engine cvars).
    std::string CaptureSettingsScenario(Game* game);
    extern float g_FxaaSubpixel;
    extern float g_FxaaEdgeThreshold;
    extern float g_FxaaEdgeThresholdMin;
    // GlowHold (seconds, default 0 = off, cvar bmvr_glow_hold): a glow
    // that dims keeps its brightest recent value this long, then fades over
    // the same time, so partial occlusion (dirty glass, the edge of an eye's
    // view, small head movements) no longer flickers it. Brightening is
    // immediate. Per glow, per eye and view.
    extern float g_GlowHold;
    // GlowSmooth (seconds, default 0.15, cvar bmvr_glow_smooth, 0 = off): each
    // glow's visible fraction is averaged over both eyes and smoothed with this
    // time constant. The tiny glow proxies cover a few pixels, so sub-pixel head
    // motion made the fraction jump every frame, differently per eye.
    // GlowHideMs (default 20 since 2026-11-11, was 100; cvar bmvr_glow_hide_ms): an eye whose query reads
    // 0 this long shows the glow at 0 at once (no glow through walls).
    extern float g_GlowSmooth;
    extern float g_GlowHideMs;
    // Tests (VR panel Debug -> TESTS, developer rows), default off:
    // GlowProxyScale (bmvr_glow_proxy_scale, default 1 since 2026-11-22 (was 1.5),
    // 1 = stock): the glow occlusion test area is this many times wider, so a query counts more pixels and
    // its visible fraction moves in finer steps.
    // PoseHoldDeg / PoseHoldMm (bmvr_pose_hold_deg / _mm, 0 = off): the game
    // keeps the previous head pose until the head turns or moves more than
    // this; the frame goes out with that pose, SteamVR reprojects the rest.
    // A still head then renders identical frames (no glow / shimmer crawl).
    extern float g_GlowProxyScale;
    // VrRpgRingScale (bmvr_rpg_ring_scale): size of the spinning red lines
    // around the RPG laser point. 1 = the flat HUD's size (10-18 px per 1440 px
    // of eye height); default 0.25. Own setting, not the aim marker size.
    extern float g_VrRpgRingScale;
    extern float g_PoseHoldDeg;
    extern float g_PoseHoldMm;
    // LOADING card on in-game loads (bmvr_loading_card, default on). Test switch
    // for the 2026-11-11 stuck-head-pose-after-load report.
    extern bool g_LoadingCard;
    // TitleLevel (default true since 2026-11-17, panel DEBUG > FIXES: RENDERING): intro logo / chapter
    // titles placed level with the room at the head's yaw when they appear and
    // left there (like the pause menu) instead of head-locked (turning,
    // pitching and rolling with the headset).
    extern bool g_TitleLevel;
    // TitleGlow (default on, panel DEBUG > FIXES: RENDERING, not saved): the
    // title layer gets flat Black Mesa's hud_post look (bloom + scanlines,
    // client 0x267c40).
    extern bool g_TitleGlow;
    // ParticlesLast (default true since 2026-11-17, panel DEBUG > FIXES: RENDERING): hold the main view's
    // particles to the end of the translucent pass also when "MSAA skips
    // particles" does not (MSAA 0 or that row off), as a pure draw-order change
    // without the 1x switch. The engine's distance sort can put a big water
    // model after a waterfall splash; the water then paints its refraction
    // texture (no particles in it) over the splash (capture 213054 vs 213110).
    extern bool g_ParticlesLast;
    // bmvr_glow_trace <seconds> / panel DEBUG "Record glow trace": start a glow
    // trace of that length (bmvr_glowtrace.h); reset to 0 once it starts.
    extern float g_GlowTraceSeconds;
    // FollowScriptedCamera (default true, cvar bmvr_follow_scripted_camera):
    // while a cutscene camera drives the view (fall + drag scene, tram), the
    // view takes its orientation with the head turning relative to it.
    extern bool g_FollowScriptedCamera;
    // OccluderViewPerEye (default true): the engine's occluder system
    // (r_occlusion, map func_occluder) takes its view in Push3DView, but the
    // client tests renderables against it before pushing the view. Flat, that
    // is the previous frame's view; in stereo the right eye was tested from the
    // left eye's position (static props pop in/out behind occluders). Each eye
    // now gets its own previous-frame occluder view. false = stock (A/B aid).
    extern bool g_OccluderViewPerEye;
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
    // Glove-only roll around the controller's forward axis so the drawn hands
    // match a hand wrapped around the grip (thumb up) instead of lying flat.
    // Left = -deg, right = +deg, same convention as the Index roll. One value
    // for every controller: on Index the pose already carries
    // g_VrHandsIndexRollDeg, so the glove adds only the difference (90 = as
    // before). Also on a hand posed on a weapon (it then sits under the gun);
    // never touches the tracked pose, so guns and aim are unaffected.
    extern float g_VrHandsRollDeg;
    // VR panel DEBUG "Show developer rows" (VrPanelDevRows, default off): shows the
    // developer groups (A/B switches, tests, engine cvars) on the DEBUG tab.
    extern bool g_PanelDevRows;
    extern float g_VrHandsGunGripRight;
    extern float g_VrHandsGunGripUp;
    extern float g_VrHandsGunGripForward;
    // Gesture easter egg: while the G-Man is in front of you, grip shows only
    // the middle finger extended.
    extern bool g_VrGmanGesture;
    // How far past the glove's rest pose the gesture finger bends back
    // (curl units, 0 = rest pose with its natural bend, default 0.3 ~ 20 degrees
    // per joint, tuned in the headset 2026-11-04). Config VrGmanFingerStraight, cvar bmvr_gman_finger_straight.
    extern float g_VrGmanFingerStraight;
    // Gesture thumb: g_VrGmanThumbCurl scales its joint angles (1 = rests on the
    // folded fingers, more presses into them), g_VrGmanThumbSwing is the sideways
    // swing at its root in radians (0 = plain curl, which goes through the fingers).
    // Config VrGmanThumbCurl / VrGmanThumbSwing, cvars bmvr_gman_thumb_curl / _swing.
    extern float g_VrGmanThumbCurl;
    extern float g_VrGmanThumbSwing;
    // Test aid (cvar bmvr_gman_gesture_force, not saved): gesture on without a G-Man in view.
    extern bool g_VrGmanGestureForce;
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
    // Gloves built from Black Mesa's own viewmodel arms (installed VPKs). Always on
    // (no config key since 2026-11-18); the glove files are only the fallback
    // when building them fails. Cvar bmvr_hands_blackmesa stays for A/B tests.
    extern bool g_VrHandsBlackMesa;
    extern float g_VrHandsBlackMesaScale;
    // The weapon's own right hand (viewmodel arms, left arm + forearms collapsed) on the gun.
    extern bool g_VrGunRightHand;
    // Black Mesa hands: the support glove on the gun takes the stock left
    // hand's finger pose (idle), and pinches the MP5 bolt / grenade pin.
    extern bool g_VrStockHandPose;
    // Support hand locks to the gun (user, 2026-11-18): while the left hand
    // holds the gun (two-hand latch) the gun's own stock left hand is drawn
    // in its grip pose (follows the shotgun pump) and the left glove is
    // hidden. Needs VrGunRightHand (the viewmodel's arms drawn).
    extern bool g_VrSupportHandLock;
    // A/B (user, 2026-11-18): VR hands lit with the raw light at the palm
    // (GetLightForPoint + flashlight) instead of the lifted/tinted value with
    // a floor ("indoor gloves stay readable", Aug 2026). Off = the old look.
    extern bool g_VrHandLightRaw;
    // Satchel (user, 2026-11-18): bag in the left hand (v_satchel's own left
    // hand), detonator in the right (v_satchel_radio's own right hand), both
    // pinned to the controllers by SatchelPinHand; left trigger throws from
    // the left hand on the release, right trigger detonates, left grip = Use.
    // Fist centre target in the controller frame (cm: forward, right, up;
    // right is mirrored for the left hand).
    extern bool g_VrSatchelHands;
    extern bool g_VrGrenadeWristAlign; // grenade own hand turned to the gun hands' wrist pose
    extern float g_VrSatchelFist[3];
    // Two-handed: health panel above the ammo panel on the gun (not on the support wrist).
    extern bool g_HudHealthOnGun;
    extern bool g_VrSupportHandForce; // test: lock as if the left hand held the gun
    // Pinch curls (thumb, index; 0-1) and a test switch that pinches the left
    // glove everywhere. Cvars only (bmvr_pinch_*), for tuning.
    extern float g_VrPinchThumb;
    extern float g_VrPinchIndex;
    extern bool g_VrPinchForce;
    // "Physical weapons (beta)" master switch (VR panel HANDS & WEAPONS, off by
    // default = the stable release's gun handling). The rows under it
    // (manual reloads, grenade pin, support hand pose and
    // lock, satchel detonator in the hand, snark hand throw, recoil) only act
    // while it is on; read them through PhysicalWeapon().
    extern bool g_PhysicalWeapons;
    inline bool PhysicalWeapon(bool sub) { return g_PhysicalWeapons && sub; }
    // Manual MP5 magazine (prototype): reload ejects, left grip at the left hip takes a magazine, seat it in the well.
    extern bool g_VrMagReload;
    // Frag grenade: pull the pin with the left hand before it can be thrown
    // (only with VrMagReload on). On by default.
    extern bool g_VrGrenadePin;
    // Small "PUMP IT" / "PULL THE CHARGING HANDLE" toasts on a dead trigger
    // (twice each per game start). On by default.
    extern bool g_VrReloadHints;
    // Left glove takes Black Mesa's own reload-clip hand pose on a held magazine, the MP5 bolt
    // and the glock slide (vr_clip_hand_poses.h); the magazine sits where that hand holds it.
    extern bool g_VrClipHandPose;
    // While the MP5 bolt / glock slide is held: the gun's own left hand in that clip pose sits on
    // it (like the support-hand lock), the glove hides.
    extern bool g_VrClipHandSnap;
    // RPG rocket held through a closed left fist (along the knuckle line).
    extern bool g_VrClipHandRpg;
    // That rocket's tail out of the pinky side (default, user 2026-11-19); off = thumb side.
    extern bool g_VrClipHandRpgFlip;
    // Manual reloads: rounds left in an ejected magazine / an opened .357
    // cylinder are lost (not topped up again); a chambered MP5/glock round
    // stays. Only while there is reserve ammo (user, 2026-11-18).
    extern bool g_VrMagDiscard;
    // Eye near clip plane, game units (BM's own is 7, ~18 cm at the VR
    // scale). Lower = guns/hands held at the face clip later (sights), at
    // some depth precision far away. Only ever lowers the game's value.
    extern float g_VrNearZ;
    // VR recoil: each shot kicks the gun + aim up (per-weapon amount, back in
    // ~0.15 s); both hands on the gun cut it to 30 % (pistol grip 50 %).
    // Off by default (user A/B). Strength scales every weapon's kick.
    extern bool g_VrRecoil;
    // Physical ladders: grip a ladder with either hand and pull/push to
    // climb (the stick still climbs too). On by default.
    extern bool g_PhysicalLadders;
    // VR panel DEBUG "HELD WEAPON ANGLE": pitch/yaw/roll (degrees) of the held
    // weapon, on top of the built-in table; live, saved per weapon in
    // VR/viewmodel_offsets.txt (ax ay az), not in config.txt.
    extern float g_VmTunePitch, g_VmTuneYaw, g_VmTuneRoll;
    // Same rows: support (left) hand point along the held gun, cm (+ = forward).
    extern float g_VmTuneSupportCm;
    // Same rows: how close (cm, magazine centre to the well) and how
    // straight (deg between the magazine in the hand and its seated pose,
    // 180 = any angle) a magazine/shell/loader must be to seat. Per weapon,
    // built-in defaults until changed (vm_numpad seat columns).
    extern float g_VmTuneSeatCm, g_VmTuneSeatDeg;
    // Built-in seat values for the weapon in the hand (vr.cpp): what
    // "Reset tab" puts back on those two rows.
    void HeldWeaponSeatDefaults(float& cm, float& deg);
    // Physical ladders: the view follows the climb hand every frame instead
    // of LadderMove's per-tick bursts (render offset only). On by default.
    extern bool g_PhysicalLadderSmooth;
    extern bool g_LadderBodyFollowsArms; // hand climbing: view closer/further from the ladder with the arms
    // Physical ladders, the gun hand: 0 = grabbing holsters the weapon (back
    // when off the ladder), 1 = only an empty hand grabs a ladder (default),
    // 2 = grabbing holsters it and it stays holstered.
    extern float g_LadderGunHand;
    // Physical ladders: the stick also mounts (walk-on, mount assist) and
    // climbs, as before hand climbing. Off = hands only (sv_ladder_useonly).
    extern bool g_LadderStickClimb;
    // Physical ladders: glove this far (units) in front of the rung it holds.
    extern float g_LadderHandBack;
    // Physical ladders: the glove on a rung is turned so its fist closes round
    // the rung (knuckle line along the rung, overhand), fingers at
    // g_LadderGripCurl; off = the controller's angle and curls.
    extern bool g_LadderHandWrap;
    extern float g_LadderGripCurl;
    // VR panel DEBUG "LADDER GRIP TUNING": the last held ladder model's grip
    // (out toward the player / up, cm; curl), saved in VR/ladder_grip.txt.
    extern float g_LadderTuneOutCm, g_LadderTuneUpCm, g_LadderTuneCurl;
    extern float g_SatchelBagDepth;
    extern float g_RpgFistCurl, g_RpgFistOutCm;
    extern float g_RpgHandTurn, g_RpgHandRoll, g_RpgHandSpin, g_RpgHandFwdCm, g_RpgHandUpCm, g_RpgHandSideCm; // RPG rocket in the left hand: finger curl, rocket out of the palm (cm)
    extern float g_GlockMagTurn, g_GlockMagRoll, g_GlockMagSpin, g_GlockMagFwdCm, g_GlockMagUpCm, g_GlockMagSideCm, g_GlockThumbCurl, g_GlockThumbBase; // glock magazine in the left hand: tuning on top of the reload-clip pose
    extern float g_SatchelBagTurn, g_SatchelBagInCm, g_SatchelBagUpCm; // satchel bag hand tuning
    extern float g_SatchelBagRoll, g_SatchelBagSpin, g_SatchelBagSideCm; // satchel bag top this far into the fist (units at scale 1)
    extern float g_SatchelGripCurl; // left glove fist round the satchel bag top
    extern float g_PauseSkipPaints; // pause-menu paints dropped at the start of each pause
    extern bool g_TripmineFaceAim;
    extern bool g_PauseCaptureArm; // DEBUG: capture the next pause (not saved)
    extern bool g_DesktopMouseGuard; // VR: no desktop mouse fire, stuck desktop buttons released
    extern bool g_PumpStrokeAtController; // bolt/pump stroke measured at the controller, back stroke at 0.75 (99c2b29); off = grab anchor, 0.85
    extern bool g_GunHandCuffCap; // cap the open HEV cuff of the gun's own hands // tripmine placed along the mine face (controller up)
    extern float g_GunHandCuff; // gun's own right hand: forearm folds this far behind the wrist (units at scale 1)
    extern float g_VrRecoilScale;
    // Kick with the second hand on the gun, as a share of the one-handed kick (default 0.3;
    // pistols use x5/3 of it, capped at 1).
    extern float g_VrRecoilTwoHand;
    // Show the RPG rocket whenever the RPG is loaded (stock loses it on a save load / a cut-off clip).
    extern bool g_RpgRocketShown;
    extern float g_DebugVmSeq, g_DebugVmCycle; // debug: hold the viewmodel on a sequence/cycle (-1 = off)
    extern bool g_DebugDumpHandPose; // debug hold: keep the stock left hand + log its pose around the object
    extern float g_VrMagHandForward, g_VrMagHandUp; // game units
    extern float g_VrShellOut; // shotgun shell centre beyond the pinch, fraction of its length
    extern bool g_VrShellFlip; // test: the other shell end in the fingers
    extern bool g_Vr357Flick;     // .357 cylinder closes on a wrist flick (or the reload button)
    extern float g_Vr357FlickSpeed; // deg/s
    extern bool g_Vr357Tilt;        // .357 empties stay in the open cylinder until the muzzle is tilted up
    extern float g_Vr357TiltPitch;  // muzzle-up degrees that dump them
    extern bool g_Vr357FireBeamHide; // laser beam off during the .357 fire clip (big kick)
    extern float g_VrBoltGrabRadius; // game units (before the viewmodel scale)
    // True for this process when launched with -game bshift (or a bs_* map).
    // Calhoun never wears the HEV suit — use bare-hand GLBs the whole session.
    extern bool g_IsBlueShift;
    bool IsBlueShift();
    // True when launched with -game asheep (or an as_* map). Same security-guard
    // HUD and bare hands as Blue Shift.
    extern bool g_IsAzureSheep;
    bool IsAzureSheep();
    bool IsAzureSheepReborn();   // -game asheep_reborn (own guard arms in v_glock)
    bool IsBmMilitary();         // -game bmmilitary (marine arms: v_glock HEV rig with marine_d)
    // Wrist HUD, weapon-wheel chrome, HUD icons, and the pause cursor use
    // Calhoun blue (64,168,255) instead of HEV orange.
    bool UseBlueHud();
    // Old stereo vis workaround: r_portalsopenall + r_occlusion 0. Default
    // off — that pair draws the whole map and tanks open/complex areas.
    extern bool g_ForceOpenVis;
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
    // HL2VR hlvr_menu_forward / overlay width, in metres (main menu quad and
    // pause overlay; the game publishes them to the OpenXR helper, bridge v16).
    // 1.4 m × 1.6 m (~60°) since 2026-10-26: 0.65 m × 1.05 m (~78°, 2026-09-03)
    // felt too close and made pointer lag obvious next to the hand. Do not
    // copy HL2VR's 150 Source-unit (~3.8 m) distance — too far here. VR
    // settings panel: MENU section.
    extern float g_MenuForwardMeters;
    extern float g_MenuWidthMeters;
    extern float g_MenuHeightOffsetMeters;
    // Seconds of low-pass on the VR menu pointer (hand tremor). 0.05 since
    // 2026-10-26 (0.18 dragged behind slow aiming).
    extern float g_MenuCursorSmoothSec;

    bool TryHudOverlay();
    bool TryVguiPaint();
    bool TryGameUiActivate();
    bool TryMeleeTrace();
    // Stereo view stays 2560 (G-buffer size) with HMD fov/aspect/IPD so the
    // deferred flashlight apply runs, then squash-blit into 1584 eyes.
    // Sticky fl_gbmatch.
    bool TryFlashlightGbMatch();
    // Under gbmatch, skip the leftover 16:9 desktop main (2 renders/frame).
    // Own sticky gb_leftskip; a death falls back to 3-render gbmatch.
    bool TryGbLeftSkip();
    bool TryDrawHud();
    bool TryDrawModelExecute();
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
