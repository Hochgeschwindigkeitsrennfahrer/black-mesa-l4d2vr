#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "openvr.h"
#include "vector.h"
#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <chrono>
#include <d3d9.h>

class Game;
class ITexture;
class IMatRenderContext;
class CViewSetup;

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;

using TextureStateMutex = std::recursive_mutex;

struct SharedTextureHolder
{
    vr::VRVulkanTextureData_t m_VulkanData{};
    vr::Texture_t m_VRTexture{};
};

struct D3DAimLineOverlayEyeState
{
    bool valid = false;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float widthPixels = 0.0f;
    float outlinePixels = 0.0f;
    float endpointPixels = 0.0f;
    uint32_t color = 0;
    uint32_t outlineColor = 0;
};

class VR
{
public:
    Game* m_Game = nullptr;

    vr::IVRSystem* m_System = nullptr;
    vr::IVRInput* m_Input = nullptr;
    vr::IVROverlay* m_Overlay = nullptr;
    vr::VROverlayHandle_t m_HUDTopHandle = vr::k_ulOverlayHandleInvalid;
    vr::IVRCompositor* m_Compositor = nullptr;

    // Eye / G-buffer size. L4D2VR uses OpenVR recommended size. Named RTs at
    // that size died on BM, so the blit path matches G-buffer to recommended
    // (hmd_native) or fits HMD aspect in the window if that crash-stickies.
    uint32_t m_RenderWidth = 0;
    uint32_t m_RenderHeight = 0;
    uint32_t m_AntiAliasing = 0;
    bool UseVrMsaa() const
    {
        return m_AntiAliasing == 2 || m_AntiAliasing == 4
            || m_AntiAliasing == 8 || m_AntiAliasing == 16;
    }
    float m_Aspect = 1.f;
    float m_Fov = 90.f;
    float m_VRScale = 39.37f;
    float m_Ipd = 0.063f;
    float m_IpdScale = 1.0f;
    float m_EyeZ = 0.0f;

    Vector m_HmdForward{};
    Vector m_HmdRight{};
    Vector m_HmdUp{};
    Vector m_HmdPosAbs{};
    Vector m_HmdPosAbsZero{};
    bool m_HmdOriginLatched = false;
    QAngle m_HmdAngAbs{};
    QAngle m_HmdAngAbsZero{};
    // Right-hand tracking in the same Source space as m_HmdPosAbs (sd805 / Portal 2).
    bool m_ControllerPoseValid = false;
    Vector m_RightControllerPosAbs{};
    QAngle m_RightControllerAngAbs{};
    Vector m_RightControllerForward{};
    Vector m_RightControllerRight{};
    Vector m_RightControllerUp{};
    float m_RightControllerSpeedMs = 0.f;
    Vector m_RightControllerRelVel{};
    Vector m_HmdTrackForward{};
    vr::HmdMatrix34_t m_RightControllerTracking{};
    bool m_RightControllerTrackingValid = false;
    // Physical left/right (not gameplay-swapped). Gun still uses m_RightController*
    // filled from AimControllerRole(). Independent hand meshes use these.
    Vector m_LeftControllerPosAbs{};
    QAngle m_LeftControllerAngAbs{};
    vr::HmdMatrix34_t m_LeftControllerTracking{};
    vr::TrackedDeviceIndex_t m_LeftControllerDevice = vr::k_unTrackedDeviceIndexInvalid;
    bool m_LeftControllerTrackingValid = false;
    Vector m_PhysicalRightPosAbs{};
    QAngle m_PhysicalRightAngAbs{};
    vr::HmdMatrix34_t m_PhysicalRightTracking{};
    vr::TrackedDeviceIndex_t m_PhysicalRightDevice = vr::k_unTrackedDeviceIndexInvalid;
    bool m_PhysicalRightTrackingValid = false;
    Vector m_ViewmodelForward{};
    Vector m_ViewmodelRight{};
    Vector m_ViewmodelUp{};
    mutable std::recursive_mutex m_ControllerMutex;
    std::string m_LastViewmodelModel;
    bool m_HasViewmodelBake = false;
    float m_ViewmodelBakeOx = 0.f;
    float m_ViewmodelBakeOy = 0.f;
    float m_ViewmodelBakeOz = 0.f;
    bool m_FirstAttackLogged = false;
    uint32_t m_FirstAttackPresentTick = 0;
    int m_FirstAttackSpikeLogs = 0;
    std::mutex m_PoseMutex;
    vr::TrackedDevicePose_t m_WaitedPoses[vr::k_unMaxTrackedDeviceCount]{};
    std::atomic<DWORD> m_WaitedPoseTick{ 0 };
    std::atomic<bool> m_PoseWaiterStop{ false };
    HANDLE m_PoseWaiterThread = nullptr;
    Vector m_SetupOrigin{};
    Vector m_SetupOriginToHMD{};

    bool m_ReShadeVRCompat = false;
    vr::VRTextureBounds_t m_TextureBounds[2]{};

    enum TextureID
    {
        Texture_None = -1,
        Texture_LeftEye,
        Texture_RightEye,
        Texture_LeftEyeSubmit,
        Texture_RightEyeSubmit,
        Texture_NekoPostLinearInput,
        Texture_NekoPostSmallInput,
        Texture_HUD,
        Texture_Scope,
        Texture_RearMirror,
        Texture_DesktopMirror,
        Texture_Blank
    };

    TextureID m_CreatingTextureID = Texture_None;

    ITexture* m_LeftEyeTexture = nullptr;
    ITexture* m_RightEyeTexture = nullptr;
    ITexture* m_LeftEyeSubmitTexture = nullptr;
    ITexture* m_RightEyeSubmitTexture = nullptr;
    ITexture* m_NekoPostLinearInputTexture = nullptr;
    ITexture* m_NekoPostSmallInputTexture = nullptr;
    ITexture* m_HUDTexture = nullptr;
    ITexture* m_ScopeTexture = nullptr;
    ITexture* m_RearMirrorTexture = nullptr;
    ITexture* m_DesktopMirrorTexture = nullptr;
    ITexture* m_BlankTexture = nullptr;

    IDirect3DSurface9* m_D9LeftEyeSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeSurface = nullptr;
    IDirect3DSurface9* m_D9LeftEyeDepthSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeDepthSurface = nullptr;
    IDirect3DSurface9* m_D9LeftEyeSubmitSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeSubmitSurface = nullptr;
    IDirect3DSurface9* m_D9NekoPostLinearInputSurface = nullptr;
    IDirect3DSurface9* m_D9NekoPostSmallInputSurface = nullptr;
    IDirect3DPixelShader9* m_D9NekoPostOutputTransferPixelShader = nullptr;
    IDirect3DDevice9* m_D9NekoPostOutputTransferShaderDevice = nullptr;
    IDirect3DSurface9* m_D9HUDSurface = nullptr;
    IDirect3DSurface9* m_D9ScopeSurface = nullptr;
    IDirect3DTexture9* m_D9ScopeLensScratchTexture = nullptr;
    IDirect3DSurface9* m_D9ScopeLensScratchSurface = nullptr;
    IDirect3DTexture9* m_D9ScopeLensTexture = nullptr;
    IDirect3DSurface9* m_D9ScopeLensSurface = nullptr;
    IDirect3DSurface9* m_D9RearMirrorSurface = nullptr;
    IDirect3DSurface9* m_D9DesktopMirrorSurface = nullptr;
    IDirect3DSurface9* m_D9BlankSurface = nullptr;
    IDirect3DSurface9* m_D9FrameColorSurface = nullptr;
    IDirect3DTexture9* m_D9LeftEyeTexture = nullptr;
    IDirect3DTexture9* m_D9RightEyeTexture = nullptr;
    IDirect3DTexture9* m_D9LeftEyeSubmitTexture = nullptr;
    IDirect3DTexture9* m_D9RightEyeSubmitTexture = nullptr;
    IDirect3DTexture9* m_D9FrameColorTexture = nullptr;

    SharedTextureHolder m_VKLeftEye;
    SharedTextureHolder m_VKRightEye;
    SharedTextureHolder m_VKBackBuffer;
    SharedTextureHolder m_VKHUD;
    SharedTextureHolder m_VKScope;
    SharedTextureHolder m_VKScopeLens;
    SharedTextureHolder m_VKRearMirror;
    SharedTextureHolder m_VKBlankTexture;
    bool m_BackBufferTextureValid = false;

    mutable TextureStateMutex m_TextureMutex;

    bool m_IsVREnabled = false;
    bool m_IsInitialized = false;
    std::atomic<bool> m_RenderedNewFrame{ false };
    std::atomic<bool> m_RenderedHud{ false };
    std::atomic<bool> m_NativeDesktopHudPainted{ false };
    bool m_MenuBlankSubmitted = false;
    std::atomic<bool> m_HasSubmittedSceneFrame{ false };
    std::atomic<uint32_t> m_SubmitPoseToken{ 0 };
    std::atomic<uint32_t> m_LastSubmittedPoseToken{ 0 };
    std::atomic<bool> m_SubmitInFlight{ false };
    std::atomic<uint32_t> m_LastSubmittedCompositorFrameIndex{ 0 };
    std::atomic<bool> m_CreatedVRTextures{ false };
    bool m_SkipBlockingPoseWait = false;
    std::atomic<int> m_LastPoseWaitError{ static_cast<int>(vr::VRCompositorError_None) };
    std::atomic<uint32_t> m_RenderCompletedFrameId{ 0 };
    std::atomic<uint32_t> m_LastSubmittedFrameId{ 0 };
    std::atomic<uint32_t> m_QueuedSubmitStaleStreak{ 0 };
    std::atomic<uint64_t> m_PresentExclusiveLockWaitUsLast{ 0 };
    std::atomic<uint64_t> m_PresentExclusiveLockWaitUsMax{ 0 };
    std::atomic<uint32_t> m_PresentCallCount{ 0 };
    std::atomic<uint64_t> m_PresentFrameIntervalUsMax{ 0 };
    std::atomic<uint32_t> m_SubmitVRTexturesEntryCount{ 0 };
    std::atomic<uint32_t> m_SubmitInFlightSkipCount{ 0 };
    std::atomic<uint32_t> m_CompositorFrameIndexDedupSkipCount{ 0 };
    std::atomic<uint32_t> m_ActualCompositorSubmitCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeNoneCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeAlreadySubmittedCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeOtherErrorCount{ 0 };
    std::atomic<uint32_t> m_PoseWaitCount{ 0 };
    std::atomic<uint32_t> m_PoseWaitOvershootCount{ 0 };
    std::atomic<uint64_t> m_PoseWaitOvershootUsMax{ 0 };
    std::atomic<uint32_t> m_RenderThreadId{ 0 };
    std::atomic<bool> m_QueuedDesktopMirrorPreOverlayReady{ false };
    std::atomic<bool> m_QueuedEyeSubmitIsolationReady{ false };

    int m_QueuedSubmitWaitMs = 0;
    bool m_RenderPipelineDebugLog = false;
    float m_RenderPipelineDebugLogHz = 1.0f;
    bool m_ShadowTweaksEnabled = false;
    bool m_DesktopMirrorEnabled = false;
    int m_DesktopMirrorEye = 1;
    bool m_DesktopMirrorKeepAspect = true;
    bool m_DesktopMirrorLinearFilter = true;
    bool m_DesktopMirrorHidePluginOverlaysRequested = false;
    bool m_DesktopMirrorHidePluginOverlays = false;
    bool m_ItemModelLabelEnabled = false;
    bool m_D3DAimLineOverlayEnabled = false;
    std::array<IDirect3DSurface9*, 2> m_D3DAimLineOverlayBackupSurfaces{};
    std::array<bool, 2> m_D3DAimLineOverlayBackupValid{};

    // Fields required by L4D2VR's d3d9_device.cpp Present path. Defaults keep
    // the queued/overlay/spike branches inactive.
    mutable std::recursive_mutex m_SourceRenderConsumerGate;
    std::atomic<uint32_t> m_SourceRenderQueueBuildCount{ 0 };
    std::atomic<uint32_t> m_SourceRenderQueueAuxPendingCount{ 0 };
    std::atomic<bool> m_SourceRenderQueueOwnershipUncertain{ false };
    std::atomic<uint32_t> m_SourceRenderQueueMarkerQueuedId{ 0 };
    std::atomic<uint32_t> m_SourceRenderQueueMarkerCompletedId{ 0 };
    inline bool IsSourceRenderQueueBusy() const
    {
        if (m_SourceRenderQueueBuildCount.load(std::memory_order_acquire) != 0)
            return true;
        if (m_SourceRenderQueueAuxPendingCount.load(std::memory_order_acquire) != 0)
            return true;
        if (m_SourceRenderQueueOwnershipUncertain.load(std::memory_order_acquire))
            return true;
        return m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire) !=
            m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
    }

    bool m_PresentSpikeDebugLog = false;
    float m_PresentSpikeThresholdMs = 32.0f;
    float m_PresentSpikeDebugLogHz = 1.0f;
    uint64_t m_PresentSpikeUpdatePrePoseUs = 0;
    uint64_t m_PresentSpikeUpdatePosesUs = 0;
    uint64_t m_PresentSpikeUpdateSettingsUs = 0;
    uint64_t m_PresentSpikeUpdateSubmitUs = 0;
    uint64_t m_PresentSpikeUpdatePlayerUs = 0;
    uint64_t m_PresentSpikeUpdateTrackingUs = 0;
    uint64_t m_PresentSpikeUpdateInputUs = 0;
    uint64_t m_PresentSpikeUpdateTailUs = 0;
    uint64_t m_PresentSpikeSubmitTimingDataUs = 0;
    uint64_t m_PresentSpikeSubmitTextureLockUs = 0;
    uint64_t m_PresentSpikeSubmitPrepareUs = 0;
    uint64_t m_PresentSpikeSubmitQueueLockUs = 0;
    uint64_t m_PresentSpikeSubmitOverlayBindUs = 0;
    uint64_t m_PresentSpikeSubmitLeftEyeUs = 0;
    uint64_t m_PresentSpikeSubmitRightEyeUs = 0;
    uint64_t m_PresentSpikeSubmitFinishUs = 0;
    uint64_t m_PresentSpikeSubmitHandHudUs = 0;
    uint64_t m_PresentSpikeSubmitSlotGateUs = 0;
    int m_PresentSpikeSubmitSlotIndex = 0;
    uint32_t m_PresentSpikeSubmitSlotLast = 0;
    bool m_PresentSpikeSubmitSlotSkipped = false;
    bool m_PresentSpikeSubmitSlotAdvanced = false;
    std::atomic<uint32_t> m_PresentSpikeEndFrameSeq{ 0 };
    std::atomic<uint64_t> m_PresentSpikeEndFrameEntryUs{ 0 };
    std::atomic<uint64_t> m_PresentSpikeEndFrameExitUs{ 0 };
    std::atomic<uint32_t> m_PresentSpikeEndFrameThreadId{ 0 };

    std::atomic<bool> m_QueuedCompositorSubmitPending{ false };
    std::atomic<uint64_t> m_QueuedCompositorSubmitTotalUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitSlotUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitQueueUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitEyesUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitHandoffUsLast{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitQueuedCount{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitCompletedCount{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitErrorCount{ 0 };

    bool m_HmdPoseValid = false;
    bool m_SafeLookActive = false;
    bool m_LookApplyEnabled = false;
    float m_PrevAppliedHmdYaw = 0.f;
    float m_PrevAppliedHmdPitch = 0.f;
    bool m_SoftPitchLook = false;
    bool m_ProcessInputEnabled = false;
    std::atomic<bool> m_ActionsReady{ false };
    vr::VRActionSetHandle_t m_ActionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActionSetHandle_t m_BaseActionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActiveActionSet_t m_ActiveActionSets[2]{};
    vr::VRActionHandle_t m_ActionJump = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPrimaryAttack = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSecondaryAttack = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionReload = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionUse = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionWalk = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionTurn = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionBooleanTurnLeft = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionBooleanTurnRight = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionNextItem = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPrevItem = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionResetPosition = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionCrouch = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionCrouchToggle = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionFlashlight = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionScoreboard = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPause = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSprint = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuSelect = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSkeletonLeft = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSkeletonRight = vr::k_ulInvalidActionHandle;
    bool m_CompositorAppHandoff = false;
    uint32_t m_CompositorHandoffSlowCount = 0;
    // Adaptive app-handoff suspension (OpenCode stutter death-spiral fix).
    std::atomic<bool> m_HandoffSuspended{ false };
    std::atomic<int> m_HandoffSlowRun{ 0 };
    std::atomic<DWORD> m_HandoffResumeAtMs{ 0 };
    std::atomic<int> m_HandoffFastFrames{ 0 };
    std::atomic<float> m_WalkForward{ 0.f };
    std::atomic<float> m_WalkSide{ 0.f };
    std::atomic<uint32_t> m_HeldButtons{ 0 };
    std::atomic<float> m_RotationOffsetY{ 0.f };
    std::atomic<uint32_t> m_PendingImpulse{ 0 };
    std::atomic<int> m_PendingInvDelta{ 0 };
    std::atomic<int> m_PendingGameUi{ 0 };
    bool m_GameUiVisible = false;
    bool m_PressedTurn = false;
    bool m_StereoEyesDrawnThisFrame = false;
    // 0 = mono, 1 = left, 2 = right. Matches Source StereoEye_t.
    int m_StereoEye = 0;
    bool m_HasStereoBodyOrigin = false;
    Vector m_StereoBodyOrigin{};
    bool m_RoomscaleActive = false;
    bool m_StereoCopyOffset = false;
    bool m_SeenGameplay = false;
    bool m_GameplayEligible = false;
    int m_GameplayFrames = 0;
    uint32_t m_PresentTick = 0;
    uint32_t m_EligiblePresents = 0;
    uint32_t m_PassThroughMainViews = 0;
    bool m_AppliedVrPerfCvars = false;
    std::string m_CurrentMapName;
    bool m_FrameCopyLatched = false;
    uint32_t m_FrameCopyWidth = 0;
    uint32_t m_FrameCopyHeight = 0;
    bool m_D3DHooksInstalled = false;
    int m_SubmitCount = 0;
    std::string m_CaptureSrc = "auto";
    std::string m_CaptureSrcLocked;
    int m_StereoOffsetPx = 0;
    bool m_CaptureReentry = false;
    bool m_LoggedFirstSubmit = false;
    bool m_LookApplyWanted = true;
    int m_OpenVRInitAttempts = 0;
    bool m_DirectEyeSubmit = false;
    bool m_UsedNamedRenderTargets = false;
    bool m_StereoRenderViewActive = false;
    bool m_PosesWaitedThisFrame = false;
    bool m_NamedCreateFailed = false;
    uint32_t m_NamedRtReadyPresent = 0;

    VR() = default;
    explicit VR(Game* game);

    void Update();
    void CreateVRTextures();
    void SubmitVRTextures();
    void WaitPosesForStereoFrame();
    Vector GetViewAngle() const;
    Vector GetViewOrigin(const Vector& setupOrigin) const;
    void GetViewBasis(Vector* forward, Vector* right, Vector* up) const;
    Vector GetViewOriginLeft(const Vector& setupOrigin) const;
    Vector GetViewOriginRight(const Vector& setupOrigin) const;
    Vector ControllerTrackingToWorld(const Vector& setupOrigin, const Vector& trackingPos) const;
    Vector GetRightControllerAbsPos(const Vector& eyePosition) const;
    QAngle GetRightControllerAbsAngle() const;
    Vector GetRecommendedViewmodelAbsPos(const Vector& eyePosition) const;
    QAngle GetRecommendedViewmodelAbsAngle() const;
    float HorizontalFovForAspect(float targetAspect) const;
    void CaptureFrameBeforePresent();
    bool BlitCurrentGameColorTo(IDirect3DSurface9* dst, bool flushGpu = true);
    bool BlitHmdViewFromBackbuffer(IDirect3DSurface9* dst, bool flushGpu = true);
    IDirect3DSurface9* ColorTargetForStereoEye(int stereoEye) const;
    void BeginStereoEyeBlit(IDirect3DSurface9* dst);
    bool EndStereoEyeBlit();
    bool StereoUnbindMatchesEye() const;
    void CaptureGameColorOnUnbind(IDirect3DSurface9* oldRt, uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH);
    void MirrorStereoToDesktopWindow();
    void ReleaseVRRenderTargetsForDeviceReset();
    bool RefreshBackBufferTexture(bool forceRefresh = false);
    void EnsureOpticsRTTTextures() {}
    void HandleMissingRenderContext(const char* location);
    void LogVAS(const char*) {}
    void ClearD3DAimLineOverlay() {}
    void ClearD3DAimLineOverlayEye(int) {}
    bool GetD3DAimLineOverlayEye(int, D3DAimLineOverlayEyeState& out) const { out = {}; return false; }
    void ClearQueuedProjectedItemLabels() {}
    void DrawQueuedProjectedItemLabelsToSurface(IDirect3DDevice9*, int, IDirect3DSurface9*, bool = true) {}
    bool IsGameplayHudRequested() const { return false; }
    bool IsQueuedHudFresh() const { return false; }
    void OnLevelInit(const char* newmap);
    void OnLevelShutdown();
    void ApplyRenderTargetFramebufferOverride(void* materialSystem = nullptr);
    void LogFullFrameSizeIfReady();
    bool IsGameplayEligible() const { return m_GameplayEligible; }
    bool HasEngineMap() const { return !m_CurrentMapName.empty(); }
    bool ShouldCompositorSubmit() const;
    bool StereoEyeBlitActive() const { return m_StereoEyeBlitActive; }
    bool HudPaintActive() const { return m_HudPaintActive; }
    void SetHudPaintActive(bool active) { m_HudPaintActive = active; }
    bool ShouldRedirectHudRt() const { return false; }
    void SetRedirectHudRt(bool) {}
    void SetVguiPaintActive(bool active) { m_VguiPaintActive = active; }
    void NoteEngineHudRtPush(const char* name, int w, int h);
    bool EngineHudRtPushed() const { return m_EngineHudRtPushed; }
    void BlitEngineHudRtToOverlay();
    bool ComputeHudInset(int fbW, int fbH, int& x, int& y, int& w, int& h) const;
    bool StereoRedirectedToEye() const { return m_StereoRedirectedToEye; }
    IDirect3DSurface9* StereoEyeBlitDest() const { return m_StereoEyeBlitDest; }
    void NoteStereoRedirectedToEye() { m_StereoRedirectedToEye = true; }
    // First gameplay RenderViews stay single-threaded. SetThreadMode(2) on
    // the first in-game Present (2026-08-18) ran during spawn Reset to
    // 2384x2160 and Present died after pass-through 2/8.
    static constexpr uint32_t kPassThroughViewsBeforeQueued = 8;
    bool PassThroughWarmupDone() const
    {
        return m_PassThroughMainViews >= kPassThroughViewsBeforeQueued;
    }
    void InstallDeviceHooks(IDirect3DDevice9* device);
    bool EnsureNamedEyeTextures();
    void PrepareNamedStereoFromPresent();
    bool NamedStereoReady() const;
    bool EnsureStereoEyeSurfaces();
    bool StereoEyesReady() const;
    void ClearUnusedDesktopBackbuffer();
    void ProcessInput();
    void ApplyMenuCursor();
    void QueueEscapeKey();
    void QueueGameUiToggle(bool currentlyPaused);
    void FlushPendingGameUi();
    bool GameUiVisible() const { return m_GameUiVisible; }
    // True when the pause/GameUI overlay should exist. Extra VGui_Paint of
    // PAINT_UIPANELS during gameplay is GameUI glass, not HEV HUD.
    bool PauseUiActive() const;
    void NoteHudPainted() { m_HudPaintedThisFrame.store(true, std::memory_order_release); }
    bool HudPaintedThisFrame() const { return m_HudPaintedThisFrame.load(std::memory_order_acquire); }
    void UpdateCrowbarMelee();
    bool IsPerformingMelee() const { return m_PerformingMelee; }
    bool TryGetMeleeBladeViewAngles(QAngle& out) const;
    bool TryGetMeleeTraceOrigin(Vector& origin) const;
    // True while VR should keep fire/reload/equip sequences running.
    bool WantsWeaponActionAnim() const;
    void GetRightGlovePalmOffsetMeters(Vector& meters) const;
    bool WantsRightGloveWeaponGripCurl() const;
    bool HudOverlayReady() const { return m_HudOverlayReady; }
    void EnsureHudOverlay();
    void SubmitHudOverlay();
    // Bind HUD overlay texture. Caller must already hold LockSubmissionQueue.
    void BindHudOverlayWhileQueueLocked();
    void ClearHudSurface(bool opaque);
    void TickMatQueueFromRenderView();
    uint32_t HeldButtons() const { return m_HeldButtons.load(std::memory_order_acquire); }
    void NoteViewmodelModel(const char* modelName);
    void NoteViewmodelWeaponBake(const char* modelName, const char* boneName, float restX, float restY, float restZ);
    vr::ETrackedControllerRole AimControllerRole() const;
    void DrawIndependentHandMarkers(IDirect3DSurface9* eyeSurf, int stereoEye);
    void DrawIndependentHandsOnDesktop();
    bool GetFingerCurls(vr::VRActionHandle_t skeletonAction, float outCurls[5]) const;
    void TryCompositorPostPresentHandoff(DWORD nowMs, DWORD poseAgeMs);

private:
    void SetActionManifest();
    bool GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const;
    bool GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const;
    bool PressedDigitalAction(vr::VRActionHandle_t handle, bool onChanged = false) const;
    void ApplyTurnStick(float stickX, float deltaMs);
    static bool IsGameplayMapName(const char* map);
    void PollMapFromEngine();
    bool InitOpenVR();
    void UpdateTracking();
    bool RefreshPosesFromCompositor();
    void StartPoseWaiter();
    static DWORD WINAPI PoseWaiterThreadMain(LPVOID param);
    void ChooseEyeRenderSize();
    bool EnsurePrivateEyeSurfaces(IDirect3DDevice9* device);
    bool EnsureFrameCopySurface(IDirect3DDevice9* device, uint32_t width, uint32_t height);
    bool FillSharedTexture(IDirect3DSurface9* surface, SharedTextureHolder& holder);
    IDirect3DSurface9* SubmitSurfaceForEye(IDirect3DSurface9* eye) const;
    void NoteMsaaEyeScene(IDirect3DSurface9* dst, bool copied);
    void ResolveMsaaEyesToSubmit(IDirect3DDevice9* device);
    void ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds);
    void RefreshIpdFromHmd();
    void UpdateControllerTracking(const vr::TrackedDevicePose_t& hmdPose);
    void UpdateAutoMatQueueMode();
    void ApplyVrQualityOfLifeCvars();
    void PollSteamVrRecommendedSize();
    void TickCompositorFocus();
    void ReclaimCompositorFocus(const char* reason);
    void PulseAimHaptic(unsigned short durationUs = 2500);
    void ResolveWeaponViewmodelPose(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const;
    void DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        bool leftOk, const Vector& leftWrist, bool rightOk, const Vector& rightWrist,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    void RefreshActiveWeaponModel();
    void ApplyViewmodelBasisOffsets();
    void ApplyTwoHandShotgunAim();
    bool m_TwoHandShotgunActive = false;
    int m_AutoMatQueueModeLastRequested = -999;
    std::chrono::steady_clock::time_point m_AutoMatQueueModeLastCmdTime{};
    bool m_VrCvarsApplied = false;
    bool m_MenuFpsMaxSent = false;
    int m_MenuFpsMaxLastHz = -1;
    bool m_LastCanRenderScene = true;
    DWORD m_EyeResizeSettleMs = 0;
    DWORD m_LastCompositorReclaimMs = 0;
    uint32_t m_MatQueueOkPresents = 0;
    IDirect3DQuery9* m_BlitEventQuery = nullptr;
    void FlushStereoBlitGpu();
    UINT KnownWindowWidth() const;
    UINT KnownWindowHeight() const;
    static bool ResolveSurfaceSize(IDirect3DSurface9* surf, UINT& w, UINT& h, D3DSURFACE_DESC* outDesc = nullptr);

    IDirect3DSurface9* m_StereoEyeBlitDest = nullptr;
    bool m_StereoEyeBlitActive = false;
    bool m_LeftEyeMsaaHasScene = false;
    bool m_RightEyeMsaaHasScene = false;
    bool m_StereoRedirectedToEye = false;
    bool m_HudPaintActive = false;
    bool m_EngineHudRtPushed = false;
    bool m_VguiPaintActive = false;
    bool m_RedirectHudRt = false;
    mutable int m_HudInsetX = 0;
    mutable int m_HudInsetY = 0;
    mutable int m_HudInsetW = 0;
    mutable int m_HudInsetH = 0;
    Vector m_PrevControllerPosAbs{};
    QAngle m_PrevControllerAngAbs{};
    DWORD m_PrevControllerTick = 0;
    DWORD m_MeleeAttackUntilMs = 0;
    bool m_PerformingMelee = false;
    bool m_MeleeNewSwing = true;
    void* m_MeleeHitEntity = nullptr;
    Vector m_MeleeTraceOrigin{};
    QAngle m_MeleeBladeAngles{};
    bool m_MeleeBladeAnglesValid = false;
    DWORD m_WeaponActionAnimUntilMs = 0;
    int m_LatchedViewmodelIdleSeq = -1;
    bool m_CrouchToggled = false;
    bool m_HudOverlayReady = false;
    bool m_HudOverlayCreateAttempted = false;
    std::atomic<bool> m_HudPaintedThisFrame{ false };
    bool m_MenuTriggerWasDown = false;
    bool m_StereoEyeBlitOk = false;
    int m_StereoEyeBlitRank = 0;
    uint32_t m_LastStereoBlitWidth = 0;
    uint32_t m_LastStereoBlitHeight = 0;
    bool m_LastEyeBlitWasWindowCrop = false;
};
