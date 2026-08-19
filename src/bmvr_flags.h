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
    // Debug boxes at each controller in the HMD eye surfaces (v1 independent hands).
    extern bool g_VrHandsDebugBoxes;
    // false = runtime PostPresentHandoff (default). true = app handoff (L4D2VR).
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
    bool TryDrawHud();
    bool TryDrawModelExecute();
}
