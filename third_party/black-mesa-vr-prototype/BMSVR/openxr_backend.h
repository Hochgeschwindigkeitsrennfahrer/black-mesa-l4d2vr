#pragma once

#ifndef XR_KHR_vulkan_enable2
// Will be defined by openxr_platform.h when XR_USE_GRAPHICS_API_VULKAN is set.
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Intentionally omit XR_USE_PLATFORM_WIN32 — it pulls MSFT holographic COM types we do not need.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vector.h"

struct XrVulkanEyeImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct XrPoseState
{
    Vector position{};
    QAngle angles{};
    bool valid = false;
};

struct XrInputState
{
    bool jump = false;
    bool primaryAttack = false;
    bool secondaryAttack = false;
    bool reload = false;
    bool use = false;
    bool crouch = false;
    bool sprint = false;
    bool nextWeapon = false;
    bool prevWeapon = false;
    bool resetSeated = false;
    bool pause = false;
    float moveX = 0.f;
    float moveY = 0.f;
    float turnX = 0.f;
};

// Thin OpenXR session: Vulkan stereo submit + controller poses/actions.
class OpenXrBackend
{
public:
    bool Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
              VkQueue queue, uint32_t queueFamilyIndex);
    bool StartSession(); // deferred until in-game
    void Shutdown();

    bool BeginFrame();
    bool SubmitEyes(const XrVulkanEyeImage &left, const XrVulkanEyeImage &right);
    void EndFrame();

    void UpdatePosesAndInput();
    void PollEvents();

    uint32_t RecommendedWidth() const { return m_ViewWidth; }
    uint32_t RecommendedHeight() const { return m_ViewHeight; }
    float FovDegrees() const { return m_FovDegrees; }
    float Aspect() const { return m_Aspect; }

    const XrPoseState &Hmd() const { return m_Hmd; }
    const XrPoseState &LeftHand() const { return m_LeftHand; }
    const XrPoseState &RightHand() const { return m_RightHand; }
    const XrInputState &Input() const { return m_Input; }

    Vector EyeOffsetMeters(int eye /*0=left 1=right*/) const;
    // Pixel crop for mono→stereo StretchRect from IPD + view FOVs (single RenderView).
    float SuggestedStereoOffsetPx(uint32_t srcWidth, float convergeMeters = 1.5f) const;
    float MeasuredIpdMeters() const;
    bool IsSessionRunning() const { return m_SessionRunning; }
    bool IsInitialized() const { return m_Instance != XR_NULL_HANDLE; }
    bool HasSession() const { return m_Session != XR_NULL_HANDLE; }
    bool InitFailed() const { return m_InitFailed; }
    XrSessionState SessionState() const { return m_SessionState; }
    bool ShouldRender() const { return m_FrameState.shouldRender == XR_TRUE; }
    static const char *SessionStateName(XrSessionState s);

private:
    bool CreateInstance();
    bool CreateSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                       uint32_t queueFamilyIndex);
    bool CreateSwapchains();
    bool CreateActions();
    bool SuggestBindings();
    bool EnsureBlitResources();
    bool BlitToSwapchain(const XrVulkanEyeImage &src, VkImage dst, uint32_t dstW, uint32_t dstH);
    bool QueryViewConfig();
    static XrPoseState PoseFromSpace(XrSpaceLocation &loc);

    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
    XrSession m_Session = XR_NULL_HANDLE;
    XrSpace m_LocalSpace = XR_NULL_HANDLE;
    XrSpace m_ViewSpace = XR_NULL_HANDLE;

    XrSwapchain m_Swapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::vector<XrSwapchainImageVulkanKHR> m_SwapchainImages[2];

    XrViewConfigurationView m_ConfigViews[2]{};
    XrView m_Views[2]{};
    XrCompositionLayerProjectionView m_ProjectionViews[2]{};

    uint32_t m_ViewWidth = 0;
    uint32_t m_ViewHeight = 0;
    float m_FovDegrees = 90.f;
    float m_Aspect = 1.f;

    XrFrameState m_FrameState{};
    XrSessionState m_SessionState = XR_SESSION_STATE_UNKNOWN;
    bool m_SessionRunning = false;
    bool m_FrameStarted = false;
    bool m_InitFailed = false;
    bool m_SubmittedLayersThisFrame = false;
    uint32_t m_FrameLogCounter = 0;

    // Actions
    XrActionSet m_ActionSet = XR_NULL_HANDLE;
    XrAction m_PoseAction = XR_NULL_HANDLE;
    XrAction m_AimAction = XR_NULL_HANDLE;
    XrAction m_MoveAction = XR_NULL_HANDLE;
    XrAction m_TurnAction = XR_NULL_HANDLE;
    XrAction m_JumpAction = XR_NULL_HANDLE;
    XrAction m_FireAction = XR_NULL_HANDLE;
    XrAction m_AltFireAction = XR_NULL_HANDLE;
    XrAction m_ReloadAction = XR_NULL_HANDLE;
    XrAction m_UseAction = XR_NULL_HANDLE;
    XrAction m_CrouchAction = XR_NULL_HANDLE;
    XrAction m_SprintAction = XR_NULL_HANDLE;
    XrAction m_NextWeaponAction = XR_NULL_HANDLE;
    XrAction m_PrevWeaponAction = XR_NULL_HANDLE;
    XrAction m_ResetAction = XR_NULL_HANDLE;
    XrAction m_MenuAction = XR_NULL_HANDLE;

    XrPath m_HandPath[2]{};
    XrSpace m_HandSpace[2]{};
    XrSpace m_AimSpace[2]{};

    XrPoseState m_Hmd{};
    XrPoseState m_LeftHand{};
    XrPoseState m_RightHand{};
    XrInputState m_Input{};

    VkInstance m_VkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice m_VkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_VkDevice = VK_NULL_HANDLE;
    VkQueue m_VkQueue = VK_NULL_HANDLE;
    uint32_t m_VkQueueFamily = 0;

    VkCommandPool m_CmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_CmdBuf = VK_NULL_HANDLE;
    VkFence m_BlitFence = VK_NULL_HANDLE;
};
