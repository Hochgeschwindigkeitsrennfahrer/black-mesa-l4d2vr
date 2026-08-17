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
    vr::IVRCompositor* m_Compositor = nullptr;

    // Eye / capture size. HMD-sized swapchain is skipped on BM (black desktop).
    // With hmd_fb this is HMD aspect fitted into the 16:9 window (G-buffer size).
    uint32_t m_RenderWidth = 0;
    uint32_t m_RenderHeight = 0;
    uint32_t m_AntiAliasing = 0;
    float m_Aspect = 1.f;
    float m_Fov = 90.f;
    float m_VRScale = 39.37f;
    float m_Ipd = 0.063f;
    float m_IpdScale = 1.0f;

    Vector m_HmdForward{};
    Vector m_HmdRight{};
    Vector m_HmdUp{};
    Vector m_HmdPosAbs{};
    Vector m_HmdPosAbsZero{};
    bool m_HmdOriginLatched = false;
    QAngle m_HmdAngAbs{};
    QAngle m_HmdAngAbsZero{};
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
    bool m_RoomscaleActive = false;
    bool m_StereoCopyOffset = false;
    bool m_SeenGameplay = false;
    bool m_GameplayEligible = false;
    int m_GameplayFrames = 0;
    uint32_t m_PresentTick = 0;
    uint32_t m_EligiblePresents = 0;
    uint32_t m_PassThroughMainViews = 0;
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
    Vector GetViewOriginLeft(const Vector& setupOrigin, const Vector& viewRight) const;
    Vector GetViewOriginRight(const Vector& setupOrigin, const Vector& viewRight) const;
    float HorizontalFovForAspect(float targetAspect) const;
    void CaptureFrameBeforePresent();
    bool BlitCurrentGameColorTo(IDirect3DSurface9* dst);
    void CaptureGameColorOnUnbind(IDirect3DSurface9* oldRt, uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH);
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
    bool IsGameplayEligible() const { return m_GameplayEligible; }
    bool ShouldCompositorSubmit() const;
    void InstallDeviceHooks(IDirect3DDevice9* device);
    bool EnsureNamedEyeTextures();
    void PrepareNamedStereoFromPresent();
    bool NamedStereoReady() const;
    bool EnsureStereoEyeSurfaces();
    bool StereoEyesReady() const;

private:
    static bool IsGameplayMapName(const char* map);
    void PollMapFromEngine();
    bool InitOpenVR();
    void UpdateTracking();
    void StartPoseWaiter();
    static DWORD WINAPI PoseWaiterThreadMain(LPVOID param);
    void ChooseEyeRenderSize();
    bool EnsurePrivateEyeSurfaces(IDirect3DDevice9* device);
    bool EnsureFrameCopySurface(IDirect3DDevice9* device, uint32_t width, uint32_t height);
    bool FillSharedTexture(IDirect3DSurface9* surface, SharedTextureHolder& holder);
    void ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds);
    void RefreshIpdFromHmd();
    UINT KnownWindowWidth() const;
    UINT KnownWindowHeight() const;
    static bool ResolveSurfaceSize(IDirect3DSurface9* surf, UINT& w, UINT& h, D3DSURFACE_DESC* outDesc = nullptr);
};
