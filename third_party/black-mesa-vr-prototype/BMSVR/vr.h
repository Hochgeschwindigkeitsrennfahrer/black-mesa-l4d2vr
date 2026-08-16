#pragma once

#include "openxr_backend.h"
#include "vector.h"
#include <cstdint>
#include <chrono>
#include <string>

class Game;
class ITexture;
class IDirect3DSurface9;
class IDirect3DDevice9;

// Layout-compatible with OpenVR VRVulkanTextureData_t / Texture_t (no openvr.h — avoids linking openvr_api).
struct VRVulkanTextureData_POD
{
    uint64_t m_nImage = 0;
    void *m_pDevice = nullptr;
    void *m_pPhysicalDevice = nullptr;
    void *m_pInstance = nullptr;
    void *m_pQueue = nullptr;
    uint32_t m_nQueueFamilyIndex = 0;
    uint32_t m_nWidth = 0;
    uint32_t m_nHeight = 0;
    uint32_t m_nFormat = 0;
    uint32_t m_nSampleCount = 0;
};

struct VRTexture_POD
{
    void *handle = nullptr;
    int eType = 0;
    int eColorSpace = 0;
};

struct SharedTextureHolder
{
    VRVulkanTextureData_POD m_VulkanData{};
    VRTexture_POD m_VRTexture{};
};

class VR
{
public:
    Game *m_Game = nullptr;
    OpenXrBackend m_Xr;

    uint32_t m_RenderWidth = 0;
    uint32_t m_RenderHeight = 0;
    uint32_t m_AntiAliasing = 0;
    float m_Aspect = 1.f;
    float m_Fov = 90.f;

    Vector m_HmdForward{};
    Vector m_HmdRight{};
    Vector m_HmdUp{};
    Vector m_HmdPosAbs{};
    QAngle m_HmdAngAbs{};

    Vector m_SetupOrigin{};
    Vector m_SetupOriginToHMD{};
    Vector m_CameraAnchor{};

    Vector m_RightControllerPosAbs{};
    QAngle m_RightControllerAngAbs{};
    bool m_ControllerPoseValid = false;

    float m_HeightOffset = 0.f;
    bool m_RoomscaleActive = true;
    float m_RotationOffset = 0.f;

    float m_TurnSpeed = 0.35f;
    bool m_SnapTurning = false;
    float m_SnapTurnAngle = 45.f;
    bool m_LeftHanded = false;
    float m_VRScale = 39.37f;
    float m_HudDistance = 1.3f;

    ITexture *m_LeftEyeTexture = nullptr;
    ITexture *m_RightEyeTexture = nullptr;
    ITexture *m_HUDTexture = nullptr;
    ITexture *m_BlankTexture = nullptr;

    enum TextureID
    {
        Texture_None = -1,
        Texture_LeftEye,
        Texture_RightEye,
        Texture_HUD,
        Texture_Blank
    };

    IDirect3DSurface9 *m_D9LeftEyeSurface = nullptr;
    IDirect3DSurface9 *m_D9RightEyeSurface = nullptr;
    IDirect3DSurface9 *m_D9HUDSurface = nullptr;
    IDirect3DSurface9 *m_D9BlankSurface = nullptr;
    IDirect3DSurface9 *m_D9DepthSurface = nullptr;

    // Private GPU copy of frame color (filled pre-Present). Do not hold game RT pointers
    // across Present — post-Present RT0/backbuffer contents are often cleared/undefined,
    // which showed as black gameplay with only a pause-menu fragment surviving.
    IDirect3DSurface9 *m_D9FrameColorSurface = nullptr;
    uint32_t m_FrameCopyWidth = 0;
    uint32_t m_FrameCopyHeight = 0;

    // While stereo RenderView runs, DXVK redirects primary-sized SetRenderTarget calls here.
    TextureID m_StereoBindEye = Texture_None;

    SharedTextureHolder m_VKLeftEye{};
    SharedTextureHolder m_VKRightEye{};
    SharedTextureHolder m_VKHUD{};
    SharedTextureHolder m_VKBlankTexture{};
    SharedTextureHolder m_VKBackBuffer{};

    bool m_IsVREnabled = false;
    bool m_IsInitialized = false;
    bool m_RenderedNewFrame = false;
    bool m_CreatedVRTextures = false;
    bool m_PressedTurn = false;
    bool m_SessionReadyLogged = false;
    bool m_LoggedFirstSubmit = false;

    // Fail-open HMD look: angles only (never SetViewAngles / origin / FOV in RenderView).
    bool m_HmdPoseValid = false;
    bool m_SafeLookActive = false;
    bool m_LookApplyEnabled = false; // relative yaw into CreateMove (absolute replace crashes)
    float m_PrevAppliedHmdYaw = 0.f;
    float m_PrevAppliedHmdPitch = 0.f;
    // Soft relative pitch (same clamp pattern as yaw). Default off — absolute pitch crashes BM.
    bool m_SoftPitchLook = false;
    int m_ValidPoseFrames = 0;
    // Controller/buttons/locomotion — only after look has been stable for a while.
    bool m_ProcessInputEnabled = false;
    // Fake stereo from one mono frame via horizontal StretchRect crop (no 2x RenderView).
    // Default off: full mono frame to both eyes (safer while diagnosing capture crop).
    bool m_StereoCopyOffset = false;
    // 0 = auto from OpenXR IPD/FOV; >0 = fixed pixel crop override.
    int m_StereoOffsetPx = 0;
    float m_StereoConvergeMeters = 1.5f;
    float m_MoveDeadzone = 0.15f;
    float m_TurnDeadzone = 0.20f;
    // When true, allocate private eye RTs at OpenXR recommended size (else window/backbuffer).
    bool m_EyeUseHmdRes = false;
    // Capture source: "auto" | "bb" | "rt0" | "rt0-vp" | "rt0-tl" | "rt0-bl" | "rt0-ctr"
    // auto = unbind latch of FullFrameFB, else probe rt0-vp/bl (never auto bb stub).
    std::string m_CaptureSrc = "auto";
    // Locked after first non-black probe (or forced cfg); empty until chosen.
    std::string m_CaptureSrcLocked;
    // True after CaptureGameColorOnUnbind copied this frame (Present must not re-copy cleared RT0).
    bool m_FrameCopyLatched = false;
    // Viewmodel pose via CalcViewModelView input adjust (default off).
    bool m_ViewmodelVr = false;
    // When true + controller pose valid: eyePos += (controller - HMD) in tracking space.
    // When false or pose missing: constant eyeAngles-basis offsets only.
    bool m_ViewmodelFollow = true;
    // Experimental: pass controller aim angles as eyeAng input (viewmodel only; never CreateMove).
    bool m_ViewmodelAim = false;
    float m_ViewmodelOffForward = 2.f; // hammer units fine-tune (eyeAngles or follow basis)
    float m_ViewmodelOffRight = 0.f;
    float m_ViewmodelOffUp = 0.f;
    int m_SessionFocusFrames = 0;

    TextureID m_CreatingTextureID = Texture_None;
    bool m_EyeRtCreateAttempted = false;
    // Source often defers real D3D RT creation until EndRenderTargetAllocation.
    TextureID m_PendingRtIds[8]{};
    int m_PendingRtCount = 0;
    int m_PendingRtNext = 0;

    TextureID ConsumePendingRtId()
    {
        if (m_PendingRtNext >= m_PendingRtCount)
            return Texture_None;
        return m_PendingRtIds[m_PendingRtNext++];
    }

    void QueuePendingRtId(TextureID id)
    {
        if (m_PendingRtCount < 8)
            m_PendingRtIds[m_PendingRtCount++] = id;
    }

    VR() = default;
    explicit VR(Game *game);

    void Update();
    // Must run inside PresentEx BEFORE the swapchain Present (stable GPU copy).
    void CaptureFrameBeforePresent();
    // Called from DXVK SetRenderTarget(0) after flush when leaving a game color RT.
    // Copies FullFrameFB (often 2048x1024) before Source clears it for the next pass.
    void CaptureGameColorOnUnbind(IDirect3DSurface9 *oldRt,
                                  uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH);
    void CreateVRTextures();
    void SubmitVRTextures();
    void UpdateTracking();
    void ProcessInput();
    void ResetPosition();

    Vector GetViewAngle();
    Vector GetViewOriginLeft();
    Vector GetViewOriginRight();
    Vector GetRecommendedViewmodelAbsPos();
    QAngle GetRecommendedViewmodelAbsAngle();
    // Controller position relative to HMD in hammer units (tracking space). False if poses missing.
    bool TryGetControllerRelToHmd(Vector &outDelta) const;

    void ParseConfigFile();

    // Called from DXVK when a D3D surface is created or bound that may be a VR eye RT.
    void TryCaptureSurface(IDirect3DSurface9 *surf, TextureID hint = Texture_None);

    // LevelInit/Shutdown VR gate: background* / empty maps never start OpenXR.
    void OnLevelInit(const char *newmap);
    void OnLevelShutdown();
    bool IsGameplayEligible() const { return m_GameplayEligible; }

    // Latched from CreateMove (command_number != 0). Keep at end — DXVK inlines earlier fields.
    bool m_SeenGameplay = false;
    int m_GameplayFrames = 0;
    bool m_GameplayEligible = false;
    std::string m_CurrentMapName;

private:
    static bool IsGameplayMapName(const char *map);
    bool InitOpenXrFromDxvk();
    Vector ScalePos(const Vector &meters) const;
    bool EnsureFrameCopySurface(IDirect3DDevice9 *device, uint32_t width, uint32_t height);
};
