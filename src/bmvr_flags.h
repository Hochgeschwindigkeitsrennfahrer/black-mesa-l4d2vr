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
    // Offscreen FullFrame/G-buffer size: max(HWND, OpenVR recommended), 16-aligned.
    // HWND width stays so 16:9 pass-through still fits. Height can exceed the window.
    bool ComputeGrownWorldFramebuffer(uint32_t& width, uint32_t& height);
    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height);
    bool QueryWindowClientSize(uint32_t& width, uint32_t& height);
    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height);
    void InstallEarlyFramebufferHook();

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
    extern float g_ViewmodelAngOffsetX;
    extern float g_ViewmodelAngOffsetY;
    extern float g_ViewmodelAngOffsetZ;
    // L4D2VR tilts Vive wands -45°. G2 points more forward; default -35.
    extern float g_ControllerPitchTilt;
    extern float g_IPDScale;
    extern float g_HeightOffset;
    extern bool g_AutoMatQueueMode;
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
    // Temporary: right HEV glove off while diagnosing grip/pistol. Left stays on.
    extern bool g_VrHandsRightEnabled;
    extern float g_VrHandsModelScale;
    // Debug boxes at each controller. Drawn if gloves are off, or if GLB draw
    // fails. Leave off once gloves are verified.
    extern bool g_VrHandsDebugBoxes;
    // Wrist HUD (HL2VR-style): health+suit on the left wrist, ammo on the right.
    extern bool g_HandHud;
    // SteamVR glove local Rx,Ry,Rz (degrees) inside BuildControllerWorld.
    // Default yaw 180: OpenVR glove +Z is opposite Source controller forward,
    // which made the mesh point backward and clip the near plane (mostly invisible).
    extern float g_VrHandsPoseRotX;
    extern float g_VrHandsPoseRotY;
    extern float g_VrHandsPoseRotZ;
    // L4D2VR BuildControllerWorld local translation (meters, before model
    // scale). Controller basis Z is -forward, so a negative local Z moves the
    // mesh further along aim. HEV GLBs bake the visual front at the origin;
    // keep this at 0 unless a small +Z pull-back is still needed.
    extern float g_VrHandsPoseOffX;
    extern float g_VrHandsPoseOffY;
    extern float g_VrHandsPoseOffZ;
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
    // false = runtime PostPresentHandoff. true = L4D2VR app handoff (default).
    extern bool g_CompositorPostPresentHandoff;
    // World-space v_ models vs world props. Applied in DrawModelExecute around
    // the controller (not m_flModelScale at entity origin). Range 0.2–1.5.
    extern float g_ViewmodelScale;
    extern float g_HudMaxFov;
    extern float g_HudDisplayRatio;
    extern float g_HudDistance;
    extern float g_HudSize;

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

    extern uint32_t g_FullFrameActualWidth;
    extern uint32_t g_FullFrameActualHeight;
    // false (default) skips the leftover 16:9 desktop scene after stereo.
    extern bool g_DesktopLeftoverRender;
}
