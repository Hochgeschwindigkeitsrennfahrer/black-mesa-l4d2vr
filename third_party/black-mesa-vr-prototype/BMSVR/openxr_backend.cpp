#include "openxr_backend.h"
#include "game.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    const char *XrResultString(XrResult r)
    {
        switch (r)
        {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
        case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        default: return "XR_ERROR";
        }
    }

    bool Check(XrResult r, const char *what)
    {
        if (XR_SUCCEEDED(r))
            return true;
        Game::logMsg("OpenXR %s failed: %s (%d)", what, XrResultString(r), (int)r);
        return false;
    }

    QAngle QuatToQAngle(const XrQuaternionf &q)
    {
        const float sinr_cosp = 2.f * (q.w * q.x + q.y * q.z);
        const float cosr_cosp = 1.f - 2.f * (q.x * q.x + q.y * q.y);
        const float roll = atan2f(sinr_cosp, cosr_cosp);

        float sinp = 2.f * (q.w * q.y - q.z * q.x);
        sinp = (std::max)(-1.f, (std::min)(1.f, sinp));
        const float pitch = asinf(sinp);

        const float siny_cosp = 2.f * (q.w * q.z + q.x * q.y);
        const float cosy_cosp = 1.f - 2.f * (q.y * q.y + q.z * q.z);
        const float yaw = atan2f(siny_cosp, cosy_cosp);

        QAngle out;
        out.x = -pitch * 180.f / kPi;
        out.y = -yaw * 180.f / kPi;
        out.z = roll * 180.f / kPi;
        return out;
    }

    Vector XrPosToSourceMeters(const XrVector3f &p)
    {
        return Vector{ p.x, -p.z, p.y };
    }

    void InsertImageBarrier(VkCommandBuffer cmd, VkImage image,
                            VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                            VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

bool OpenXrBackend::CreateInstance()
{
    // GraphicsBindingVulkanKHR lives in XR_KHR_vulkan_enable (not enable2).
    // BM is Win32 + DXVK Vulkan — runtime must expose this; WMR 32-bit often only has D3D11.
    const char *exts[] = {
        XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
    };
    constexpr uint32_t kReqCount = 1;

    uint32_t extCount = 0;
    XrResult enumRes = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (!XR_SUCCEEDED(enumRes))
    {
        Game::logMsg("OpenXR xrEnumerateInstanceExtensionProperties failed: %s (%d)",
                     XrResultString(enumRes), (int)enumRes);
        return false;
    }

    std::vector<XrExtensionProperties> available(extCount);
    for (auto &p : available)
        p = { XR_TYPE_EXTENSION_PROPERTIES };
    enumRes = xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, available.data());
    if (!XR_SUCCEEDED(enumRes))
    {
        Game::logMsg("OpenXR enumerate extensions (fill) failed: %s (%d)",
                     XrResultString(enumRes), (int)enumRes);
        return false;
    }

    Game::logMsg("OpenXR instance extensions available (%u):", extCount);
    bool hasD3d11 = false;
    bool hasVulkanEnable = false;
    bool hasVulkanEnable2 = false;
    for (uint32_t i = 0; i < extCount; ++i)
    {
        Game::logMsg("  [%u] %s (v%u)", i, available[i].extensionName, available[i].extensionVersion);
        if (strcmp(available[i].extensionName, "XR_KHR_D3D11_enable") == 0)
            hasD3d11 = true;
        if (strcmp(available[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
            hasVulkanEnable = true;
        if (strcmp(available[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
            hasVulkanEnable2 = true;
    }
    Game::logMsg("OpenXR graphics ext summary: vulkan_enable=%d vulkan_enable2=%d D3D11_enable=%d",
                 (int)hasVulkanEnable, (int)hasVulkanEnable2, (int)hasD3d11);

    bool allPresent = true;
    for (uint32_t r = 0; r < kReqCount; ++r)
    {
        bool found = false;
        for (uint32_t i = 0; i < extCount; ++i)
        {
            if (strcmp(available[i].extensionName, exts[r]) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            Game::logMsg("OpenXR REQUIRED extension missing: %s", exts[r]);
            allPresent = false;
        }
        else
            Game::logMsg("OpenXR required extension OK: %s", exts[r]);
    }
    if (!allPresent)
    {
        Game::logMsg(
            "OpenXR CreateInstance skipped — runtime lacks XR_KHR_vulkan_enable. "
            "BMSVR is DXVK Vulkan (not D3D11). WMR/Mixed Reality 32-bit often has only "
            "XR_KHR_D3D11_enable; use run-bms-vr-steamvr.bat for HMD until a WMR Vulkan path exists.");
        return false;
    }

    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "Black Mesa VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "Source");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = kReqCount;
    ci.enabledExtensionNames = exts;

    if (!Check(xrCreateInstance(&ci, &m_Instance), "xrCreateInstance"))
    {
        // Loader returned -9 without our pre-check catching it (unusual); restate request.
        for (uint32_t r = 0; r < kReqCount; ++r)
            Game::logMsg("OpenXR CreateInstance requested: %s", exts[r]);
        return false;
    }
    Game::logMsg("OpenXR CreateInstance OK (XR_KHR_vulkan_enable)");
    return true;
}

bool OpenXrBackend::Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                         VkQueue queue, uint32_t queueFamilyIndex)
{
    if (m_Instance != XR_NULL_HANDLE)
        return true;
    if (m_InitFailed)
        return false;

    m_VkInstance = instance;
    m_VkPhysicalDevice = physicalDevice;
    m_VkDevice = device;
    m_VkQueue = queue;
    m_VkQueueFamily = queueFamilyIndex;

    if (!CreateInstance())
    {
        m_InitFailed = true;
        return false;
    }

    XrSystemGetInfo sysInfo{ XR_TYPE_SYSTEM_GET_INFO };
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(m_Instance, &sysInfo, &m_SystemId), "xrGetSystem"))
    {
        m_InitFailed = true;
        return false;
    }

    // Spec: must call graphics requirements before session — do it early while probing.
    PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetReqs = nullptr;
    xrGetInstanceProcAddr(m_Instance, "xrGetVulkanGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetReqs));
    if (pfnGetReqs)
    {
        XrGraphicsRequirementsVulkanKHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
        pfnGetReqs(m_Instance, m_SystemId, &reqs);
        Game::logMsg("OpenXR Vulkan reqs min=0x%llx max=0x%llx",
                     (unsigned long long)reqs.minApiVersionSupported,
                     (unsigned long long)reqs.maxApiVersionSupported);
    }

    if (!QueryViewConfig())
    {
        m_InitFailed = true;
        return false;
    }

    // Defer xrCreateSession until in-game — avoids SteamVR killing apps that don't submit frames at menu.
    Game::logMsg("OpenXR probed (%ux%u) — session deferred until map", m_ViewWidth, m_ViewHeight);
    return true;
}

bool OpenXrBackend::QueryViewConfig()
{
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (viewCount < 2)
    {
        Game::logMsg("OpenXR stereo views unavailable");
        return false;
    }
    m_ConfigViews[0] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
    m_ConfigViews[1] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
    xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, m_ConfigViews);

    m_ViewWidth = m_ConfigViews[0].recommendedImageRectWidth;
    m_ViewHeight = m_ConfigViews[0].recommendedImageRectHeight;
    if (!m_ViewWidth || !m_ViewHeight)
    {
        Game::logMsg("OpenXR recommended view size invalid (%ux%u) — using 1512x1680", m_ViewWidth, m_ViewHeight);
        m_ViewWidth = 1512;
        m_ViewHeight = 1680;
    }
    constexpr uint32_t kMaxEye = 1440;
    if (m_ViewWidth > kMaxEye || m_ViewHeight > kMaxEye)
    {
        const float scale = (std::min)((float)kMaxEye / (float)m_ViewWidth, (float)kMaxEye / (float)m_ViewHeight);
        m_ViewWidth = (uint32_t)(m_ViewWidth * scale);
        m_ViewHeight = (uint32_t)(m_ViewHeight * scale);
        Game::logMsg("OpenXR eye res capped to %ux%u", m_ViewWidth, m_ViewHeight);
    }
    m_ConfigViews[0].recommendedImageRectWidth = m_ViewWidth;
    m_ConfigViews[0].recommendedImageRectHeight = m_ViewHeight;
    m_ConfigViews[1].recommendedImageRectWidth = m_ViewWidth;
    m_ConfigViews[1].recommendedImageRectHeight = m_ViewHeight;
    m_Aspect = (float)m_ViewWidth / (float)m_ViewHeight;
    m_FovDegrees = 90.f;
    return true;
}

bool OpenXrBackend::StartSession()
{
    if (m_Session != XR_NULL_HANDLE)
        return true;
    if (m_Instance == XR_NULL_HANDLE || m_InitFailed)
        return false;

    if (!CreateSession(m_VkInstance, m_VkPhysicalDevice, m_VkDevice, m_VkQueueFamily))
        return false;

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
    if (!Check(xrCreateReferenceSpace(m_Session, &spaceInfo, &m_LocalSpace), "xrCreateReferenceSpace(local)"))
        return false;

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(m_Session, &spaceInfo, &m_ViewSpace), "xrCreateReferenceSpace(view)"))
        return false;

    if (!CreateSwapchains() || !CreateActions() || !SuggestBindings() || !EnsureBlitResources())
        return false;

    for (int i = 0; i < 64; ++i)
        PollEvents();

    Game::logMsg("OpenXR session started (%ux%u) running=%d", m_ViewWidth, m_ViewHeight, (int)m_SessionRunning);
    return true;
}

bool OpenXrBackend::CreateSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                  uint32_t queueFamilyIndex)
{
    // Spec: must call xrGetVulkanGraphicsRequirementsKHR before xrCreateSession.
    PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetReqs = nullptr;
    xrGetInstanceProcAddr(m_Instance, "xrGetVulkanGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetReqs));
    if (!pfnGetReqs)
    {
        Game::logMsg("xrGetVulkanGraphicsRequirementsKHR not available");
        return false;
    }

    XrGraphicsRequirementsVulkanKHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    if (!Check(pfnGetReqs(m_Instance, m_SystemId, &reqs), "xrGetVulkanGraphicsRequirementsKHR"))
        return false;

    Game::logMsg("OpenXR Vulkan reqs min=0x%llx max=0x%llx",
                 (unsigned long long)reqs.minApiVersionSupported,
                 (unsigned long long)reqs.maxApiVersionSupported);

    // Prefer the physical device OpenXR wants (should match DXVK's GPU).
    PFN_xrGetVulkanGraphicsDeviceKHR pfnGetDev = nullptr;
    xrGetInstanceProcAddr(m_Instance, "xrGetVulkanGraphicsDeviceKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetDev));
    if (pfnGetDev)
    {
        VkPhysicalDevice xrPd = VK_NULL_HANDLE;
        if (XR_SUCCEEDED(pfnGetDev(m_Instance, m_SystemId, instance, &xrPd)))
        {
            if (xrPd && xrPd != physicalDevice)
            {
                Game::logMsg("OpenXR physical device differs from DXVK — using DXVK device anyway");
            }
            else
                Game::logMsg("OpenXR physical device matches DXVK");
        }
    }

    XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance = instance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamilyIndex;
    binding.queueIndex = 0;

    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = m_SystemId;
    return Check(xrCreateSession(m_Instance, &sci, &m_Session), "xrCreateSession");
}

bool OpenXrBackend::CreateSwapchains()
{
    int64_t format = VK_FORMAT_R8G8B8A8_SRGB;
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_Session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_Session, (uint32_t)formats.size(), &formatCount, formats.data());
    for (int64_t f : formats)
    {
        if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB ||
            f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM)
        {
            format = f;
            break;
        }
    }

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = m_ConfigViews[eye].recommendedImageRectWidth;
        ci.height = m_ConfigViews[eye].recommendedImageRectHeight;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!Check(xrCreateSwapchain(m_Session, &ci, &m_Swapchain[eye]), "xrCreateSwapchain"))
            return false;

        uint32_t n = 0;
        xrEnumerateSwapchainImages(m_Swapchain[eye], 0, &n, nullptr);
        m_SwapchainImages[eye].resize(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
        xrEnumerateSwapchainImages(m_Swapchain[eye], n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_SwapchainImages[eye].data()));
    }
    return true;
}

bool OpenXrBackend::EnsureBlitResources()
{
    if (m_CmdPool)
        return true;

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_VkQueueFamily;
    if (vkCreateCommandPool(m_VkDevice, &pci, nullptr, &m_CmdPool) != VK_SUCCESS)
    {
        Game::logMsg("vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = m_CmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_VkDevice, &ai, &m_CmdBuf) != VK_SUCCESS)
    {
        Game::logMsg("vkAllocateCommandBuffers failed");
        return false;
    }

    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(m_VkDevice, &fci, nullptr, &m_BlitFence) != VK_SUCCESS)
    {
        Game::logMsg("vkCreateFence failed");
        return false;
    }
    return true;
}

bool OpenXrBackend::BlitToSwapchain(const XrVulkanEyeImage &src, VkImage dst, uint32_t dstW, uint32_t dstH)
{
    if (!src.image || !dst || !m_CmdBuf)
        return false;

    vkResetCommandBuffer(m_CmdBuf, 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_CmdBuf, &bi);

    // Source was transitioned to TRANSFER_SRC_OPTIMAL by TransferSurface.
    InsertImageBarrier(m_CmdBuf, dst,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Stretch-fill the eye swapchain with a V-flip (D3D top-left → OpenXR bottom-left).
    // Aspect-fit letterboxing into nearly-square HMD RTs (1280x720 → 1440x1407) left
    // large black bands and, with WMR's mirror/FOV, looked like a ~50% horizontal shift
    // of the pause UI. Fill the eye; accept mild squash while capture is diagnosed.
    static bool s_blitGeomLog;
    if (!s_blitGeomLog)
    {
        s_blitGeomLog = true;
        Game::logMsg("BlitToSwapchain stretch-fill+Vflip src=%ux%u -> dst=%ux%u",
                     src.width, src.height, dstW, dstH);
    }

    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[0] = { 0, 0, 0 };
    region.srcOffsets[1] = { (int32_t)src.width, (int32_t)src.height, 1 };
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    // V-flip: dst Y0 = height, Y1 = 0.
    region.dstOffsets[0] = { 0, (int32_t)dstH, 0 };
    region.dstOffsets[1] = { (int32_t)dstW, 0, 1 };

    vkCmdBlitImage(m_CmdBuf,
        src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region, VK_FILTER_LINEAR);

    InsertImageBarrier(m_CmdBuf, dst,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    vkEndCommandBuffer(m_CmdBuf);

    vkResetFences(m_VkDevice, 1, &m_BlitFence);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_CmdBuf;
    if (vkQueueSubmit(m_VkQueue, 1, &si, m_BlitFence) != VK_SUCCESS)
        return false;

    vkWaitForFences(m_VkDevice, 1, &m_BlitFence, VK_TRUE, 5'000'000'000ull);
    return true;
}

bool OpenXrBackend::CreateActions()
{
    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "bmsvr");
    strcpy_s(asci.localizedActionSetName, "Black Mesa VR");
    if (!Check(xrCreateActionSet(m_Instance, &asci, &m_ActionSet), "xrCreateActionSet"))
        return false;

    auto makeAction = [&](XrAction *out, const char *name, const char *localized, XrActionType type, uint32_t count = 0, XrPath *sub = nullptr) {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, localized);
        aci.actionType = type;
        aci.countSubactionPaths = count;
        aci.subactionPaths = sub;
        return Check(xrCreateAction(m_ActionSet, &aci, out), name);
    };

    xrStringToPath(m_Instance, "/user/hand/left", &m_HandPath[0]);
    xrStringToPath(m_Instance, "/user/hand/right", &m_HandPath[1]);

    if (!makeAction(&m_PoseAction, "hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT, 2, m_HandPath)) return false;
    if (!makeAction(&m_AimAction, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, 2, m_HandPath)) return false;
    if (!makeAction(&m_MoveAction, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    if (!makeAction(&m_TurnAction, "turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    if (!makeAction(&m_JumpAction, "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_FireAction, "fire", "Fire", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_AltFireAction, "alt_fire", "Alt Fire", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_ReloadAction, "reload", "Reload", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_UseAction, "use", "Use", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_CrouchAction, "crouch", "Crouch", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_SprintAction, "sprint", "Sprint", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_NextWeaponAction, "next_weapon", "Next Weapon", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_PrevWeaponAction, "prev_weapon", "Prev Weapon", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_ResetAction, "reset_seated", "Reset Seated", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!makeAction(&m_MenuAction, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;

    for (int i = 0; i < 2; ++i)
    {
        XrActionSpaceCreateInfo asciPose{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        asciPose.action = m_PoseAction;
        asciPose.subactionPath = m_HandPath[i];
        asciPose.poseInActionSpace.orientation.w = 1.f;
        if (!Check(xrCreateActionSpace(m_Session, &asciPose, &m_HandSpace[i]), "xrCreateActionSpace(pose)"))
            return false;

        asciPose.action = m_AimAction;
        if (!Check(xrCreateActionSpace(m_Session, &asciPose, &m_AimSpace[i]), "xrCreateActionSpace(aim)"))
            return false;
    }

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &m_ActionSet;
    return Check(xrAttachSessionActionSets(m_Session, &attach), "xrAttachSessionActionSets");
}

bool OpenXrBackend::SuggestBindings()
{
    XrPath profile{};
    xrStringToPath(m_Instance, "/interaction_profiles/oculus/touch_controller", &profile);

    auto path = [&](const char *s) {
        XrPath p{};
        xrStringToPath(m_Instance, s, &p);
        return p;
    };

    XrActionSuggestedBinding binds[] = {
        { m_PoseAction, path("/user/hand/left/input/grip/pose") },
        { m_PoseAction, path("/user/hand/right/input/grip/pose") },
        { m_AimAction, path("/user/hand/left/input/aim/pose") },
        { m_AimAction, path("/user/hand/right/input/aim/pose") },
        { m_MoveAction, path("/user/hand/left/input/thumbstick") },
        { m_TurnAction, path("/user/hand/right/input/thumbstick") },
        { m_JumpAction, path("/user/hand/right/input/a/click") },
        { m_FireAction, path("/user/hand/right/input/trigger/value") },
        { m_AltFireAction, path("/user/hand/left/input/trigger/value") },
        { m_ReloadAction, path("/user/hand/right/input/b/click") },
        { m_UseAction, path("/user/hand/right/input/squeeze/value") },
        { m_CrouchAction, path("/user/hand/left/input/x/click") },
        { m_SprintAction, path("/user/hand/left/input/squeeze/value") },
        { m_MenuAction, path("/user/hand/left/input/menu/click") },
        { m_ResetAction, path("/user/hand/left/input/thumbstick/click") },
        { m_NextWeaponAction, path("/user/hand/right/input/thumbstick/click") },
    };

    XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = (uint32_t)(sizeof(binds) / sizeof(binds[0]));
    suggested.suggestedBindings = binds;
    xrSuggestInteractionProfileBindings(m_Instance, &suggested);

    // Also suggest Index / Vive cosmos-ish knuckle profile via simple controller fallback.
    XrPath simple{};
    if (XR_SUCCEEDED(xrStringToPath(m_Instance, "/interaction_profiles/khr/simple_controller", &simple)))
    {
        XrActionSuggestedBinding simpleBinds[] = {
            { m_PoseAction, path("/user/hand/left/input/grip/pose") },
            { m_PoseAction, path("/user/hand/right/input/grip/pose") },
            { m_AimAction, path("/user/hand/left/input/aim/pose") },
            { m_AimAction, path("/user/hand/right/input/aim/pose") },
            { m_FireAction, path("/user/hand/right/input/select/click") },
            { m_UseAction, path("/user/hand/left/input/select/click") },
            { m_MenuAction, path("/user/hand/left/input/menu/click") },
            { m_MenuAction, path("/user/hand/right/input/menu/click") },
        };
        suggested.interactionProfile = simple;
        suggested.countSuggestedBindings = (uint32_t)(sizeof(simpleBinds) / sizeof(simpleBinds[0]));
        suggested.suggestedBindings = simpleBinds;
        xrSuggestInteractionProfileBindings(m_Instance, &suggested);
    }
    return true;
}

const char *OpenXrBackend::SessionStateName(XrSessionState s)
{
    switch (s)
    {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "OTHER";
    }
}

void OpenXrBackend::PollEvents()
{
    if (m_Instance == XR_NULL_HANDLE)
        return;

    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(m_Instance, &event) == XR_SUCCESS)
    {
        switch (event.type)
        {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            auto *s = reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
            const XrSessionState prev = m_SessionState;
            m_SessionState = s->state;
            Game::logMsg("OpenXR session state %s(%d) -> %s(%d)",
                         SessionStateName(prev), (int)prev,
                         SessionStateName(s->state), (int)s->state);
            if (s->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(m_Session, &begin), "xrBeginSession"))
                    m_SessionRunning = true;
            }
            else if (s->state == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession(m_Session);
                m_SessionRunning = false;
            }
            else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING)
            {
                m_SessionRunning = false;
            }
            break;
        }
        default:
            break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

XrPoseState OpenXrBackend::PoseFromSpace(XrSpaceLocation &loc)
{
    XrPoseState out{};
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return out;

    out.valid = true;
    out.position = XrPosToSourceMeters(loc.pose.position);
    out.angles = QuatToQAngle(loc.pose.orientation);
    return out;
}

void OpenXrBackend::UpdatePosesAndInput()
{
    if (m_Instance == XR_NULL_HANDLE || m_Session == XR_NULL_HANDLE)
        return;

    PollEvents();
    if (!m_SessionRunning)
        return;

    // Need a real predicted display time from BeginFrame — fake time=1 crashes some runtimes.
    if (!m_FrameState.predictedDisplayTime)
        return;

    XrActiveActionSet active{ m_ActionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(m_Session, &sync);

    const XrTime t = m_FrameState.predictedDisplayTime;

    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    xrLocateSpace(m_ViewSpace, m_LocalSpace, t, &loc);
    m_Hmd = PoseFromSpace(loc);

    xrLocateSpace(m_AimSpace[0], m_LocalSpace, t, &loc);
    m_LeftHand = PoseFromSpace(loc);
    xrLocateSpace(m_AimSpace[1], m_LocalSpace, t, &loc);
    m_RightHand = PoseFromSpace(loc);

    auto dig = [&](XrAction a) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
        xrGetActionStateBoolean(m_Session, &gi, &st);
        return st.isActive && st.currentState;
    };
    auto vec2 = [&](XrAction a, float &x, float &y) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
        xrGetActionStateVector2f(m_Session, &gi, &st);
        if (st.isActive)
        {
            x = st.currentState.x;
            y = st.currentState.y;
        }
    };

    m_Input = {};
    m_Input.jump = dig(m_JumpAction);
    m_Input.primaryAttack = dig(m_FireAction);
    m_Input.secondaryAttack = dig(m_AltFireAction);
    m_Input.reload = dig(m_ReloadAction);
    m_Input.use = dig(m_UseAction);
    m_Input.crouch = dig(m_CrouchAction);
    m_Input.sprint = dig(m_SprintAction);
    m_Input.nextWeapon = dig(m_NextWeaponAction);
    m_Input.prevWeapon = dig(m_PrevWeaponAction);
    m_Input.resetSeated = dig(m_ResetAction);
    m_Input.pause = dig(m_MenuAction);
    vec2(m_MoveAction, m_Input.moveX, m_Input.moveY);
    float turnY = 0.f;
    vec2(m_TurnAction, m_Input.turnX, turnY);
    (void)turnY;
}

bool OpenXrBackend::BeginFrame()
{
    if (m_Session == XR_NULL_HANDLE)
        return false;

    PollEvents();
    if (!m_SessionRunning)
        return false;

    m_SubmittedLayersThisFrame = false;

    XrFrameWaitInfo wait{ XR_TYPE_FRAME_WAIT_INFO };
    m_FrameState = { XR_TYPE_FRAME_STATE };
    const XrResult waitRes = xrWaitFrame(m_Session, &wait, &m_FrameState);
    if (!XR_SUCCEEDED(waitRes))
    {
        static uint32_t s_waitFailLog;
        if ((s_waitFailLog++ % 120u) == 0u)
            Game::logMsg("xrWaitFrame failed: %s (%d) state=%s",
                         XrResultString(waitRes), (int)waitRes, SessionStateName(m_SessionState));
        return false;
    }

    XrFrameBeginInfo begin{ XR_TYPE_FRAME_BEGIN_INFO };
    const XrResult beginRes = xrBeginFrame(m_Session, &begin);
    if (!XR_SUCCEEDED(beginRes))
    {
        static uint32_t s_beginFailLog;
        if ((s_beginFailLog++ % 120u) == 0u)
            Game::logMsg("xrBeginFrame failed: %s (%d) state=%s",
                         XrResultString(beginRes), (int)beginRes, SessionStateName(m_SessionState));
        return false;
    }

    m_FrameStarted = true;
    ++m_FrameLogCounter;

    XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = m_FrameState.predictedDisplayTime;
    vli.space = m_LocalSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    uint32_t vc = 2;
    m_Views[0] = { XR_TYPE_VIEW };
    m_Views[1] = { XR_TYPE_VIEW };
    xrLocateViews(m_Session, &vli, &vs, 2, &vc, m_Views);

    if (vc >= 1)
    {
        const float l = tanf(m_Views[0].fov.angleLeft);
        const float r = tanf(m_Views[0].fov.angleRight);
        const float u = tanf(m_Views[0].fov.angleUp);
        const float d = tanf(m_Views[0].fov.angleDown);
        const float tanHalf = (std::max)({ fabsf(l), fabsf(r), fabsf(u), fabsf(d) });
        m_FovDegrees = 2.f * atanf(tanHalf) * 180.f / kPi;
        m_Aspect = (fabsf(l) + fabsf(r)) / (fabsf(u) + fabsf(d) + 1e-5f);

        static bool s_fovLog;
        if (!s_fovLog && vc >= 2)
        {
            s_fovLog = true;
            auto deg = [](float rad) { return rad * 180.f / kPi; };
            Game::logMsg(
                "OpenXR FOV L(l=%.1f r=%.1f u=%.1f d=%.1f) R(l=%.1f r=%.1f u=%.1f d=%.1f) "
                "aspect=%.3f fov~%.1f swap=%ux%u",
                deg(m_Views[0].fov.angleLeft), deg(m_Views[0].fov.angleRight),
                deg(m_Views[0].fov.angleUp), deg(m_Views[0].fov.angleDown),
                deg(m_Views[1].fov.angleLeft), deg(m_Views[1].fov.angleRight),
                deg(m_Views[1].fov.angleUp), deg(m_Views[1].fov.angleDown),
                m_Aspect, m_FovDegrees, m_ViewWidth, m_ViewHeight);
        }
    }
    return true;
}

Vector OpenXrBackend::EyeOffsetMeters(int eye) const
{
    if (eye < 0 || eye > 1 || !m_Hmd.valid)
        return {};
    return XrPosToSourceMeters(m_Views[eye].pose.position) - m_Hmd.position;
}

float OpenXrBackend::MeasuredIpdMeters() const
{
    const Vector l = XrPosToSourceMeters(m_Views[0].pose.position);
    const Vector r = XrPosToSourceMeters(m_Views[1].pose.position);
    const float dx = r.x - l.x;
    const float dy = r.y - l.y;
    const float dz = r.z - l.z;
    const float ipd = sqrtf(dx * dx + dy * dy + dz * dz);
    if (ipd >= 0.04f && ipd <= 0.12f)
        return ipd;
    return 0.063f; // typical adult IPD fallback
}

float OpenXrBackend::SuggestedStereoOffsetPx(uint32_t srcWidth, float convergeMeters) const
{
    if (srcWidth < 64)
        return 24.f;
    if (convergeMeters < 0.25f)
        convergeMeters = 0.25f;

    const float ipd = MeasuredIpdMeters();

    // Horizontal tan extents from both eyes (OpenXR: angleLeft usually negative).
    float tanL = fabsf(tanf(m_Views[0].fov.angleLeft));
    float tanR = fabsf(tanf(m_Views[0].fov.angleRight));
    tanL = (std::max)(tanL, fabsf(tanf(m_Views[1].fov.angleLeft)));
    tanR = (std::max)(tanR, fabsf(tanf(m_Views[1].fov.angleRight)));
    float tanHalfH = 0.5f * (tanL + tanR);
    if (tanHalfH < 0.15f)
        tanHalfH = tanf(50.f * kPi / 180.f);

    // Parallel mono cameras: shift each eye by IPD/2 toward center at converge distance.
    const float halfWMeters = convergeMeters * tanHalfH;
    float offsetPx = (ipd * 0.5f / halfWMeters) * (0.5f * (float)srcWidth);

    // Mild FOV-asymmetry boost (nasal/temporal difference between eyes).
    const float leftCenter =
        0.5f * (tanf(m_Views[0].fov.angleLeft) + tanf(m_Views[0].fov.angleRight));
    const float rightCenter =
        0.5f * (tanf(m_Views[1].fov.angleLeft) + tanf(m_Views[1].fov.angleRight));
    const float centerDelta = fabsf(rightCenter - leftCenter);
    if (centerDelta > 1e-4f && tanHalfH > 1e-4f)
        offsetPx += 0.25f * (centerDelta / tanHalfH) * (0.5f * (float)srcWidth);

    const float maxOff = (float)(srcWidth / 8);
    if (offsetPx < 4.f)
        offsetPx = 4.f;
    if (offsetPx > maxOff)
        offsetPx = maxOff;
    return offsetPx;
}

bool OpenXrBackend::SubmitEyes(const XrVulkanEyeImage &left, const XrVulkanEyeImage &right)
{
    if (!m_FrameStarted)
        return false;

    if (!m_FrameState.shouldRender)
    {
        if ((m_FrameLogCounter % 120u) == 1u)
            Game::logMsg("SubmitEyes skip (shouldRender=0) state=%s", SessionStateName(m_SessionState));
        return true;
    }

    // Fresh projection views each submit — avoid stale swapchain handles on failed blit.
    m_ProjectionViews[0] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
    m_ProjectionViews[1] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };

    const XrVulkanEyeImage eyes[2] = { left, right };
    int blitted = 0;
    for (int eye = 0; eye < 2; ++eye)
    {
        if (!eyes[eye].image)
            continue;

        XrSwapchainImageAcquireInfo acq{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        uint32_t index = 0;
        if (!XR_SUCCEEDED(xrAcquireSwapchainImage(m_Swapchain[eye], &acq, &index)))
            continue;

        XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(m_Swapchain[eye], &wait);

        VkImage dst = m_SwapchainImages[eye][index].image;
        if (!BlitToSwapchain(eyes[eye], dst, m_ViewWidth, m_ViewHeight))
        {
            XrSwapchainImageReleaseInfo rel{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(m_Swapchain[eye], &rel);
            continue;
        }

        XrSwapchainImageReleaseInfo rel{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(m_Swapchain[eye], &rel);

        m_ProjectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        m_ProjectionViews[eye].pose = m_Views[eye].pose;
        m_ProjectionViews[eye].fov = m_Views[eye].fov;
        m_ProjectionViews[eye].subImage.swapchain = m_Swapchain[eye];
        m_ProjectionViews[eye].subImage.imageRect.offset = { 0, 0 };
        m_ProjectionViews[eye].subImage.imageRect.extent = { (int32_t)m_ViewWidth, (int32_t)m_ViewHeight };
        ++blitted;
    }

    m_SubmittedLayersThisFrame = (blitted == 2);
    if ((m_FrameLogCounter % 120u) == 1u || (m_SubmittedLayersThisFrame && m_FrameLogCounter <= 3u))
        Game::logMsg("SubmitEyes blit=%d/%d %ux%u state=%s shouldRender=1",
                     blitted, 2, m_ViewWidth, m_ViewHeight, SessionStateName(m_SessionState));
    return m_SubmittedLayersThisFrame;
}

void OpenXrBackend::EndFrame()
{
    if (!m_FrameStarted || m_Session == XR_NULL_HANDLE)
        return;

    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = m_LocalSpace;
    layer.viewCount = 2;
    layer.views = m_ProjectionViews;

    const XrCompositionLayerBaseHeader *layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)
    };

    const bool haveLayers = m_FrameState.shouldRender &&
        m_ProjectionViews[0].subImage.swapchain != XR_NULL_HANDLE &&
        m_ProjectionViews[1].subImage.swapchain != XR_NULL_HANDLE;

    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
    end.displayTime = m_FrameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = haveLayers ? 1 : 0;
    end.layers = haveLayers ? layers : nullptr;
    const XrResult endRes = xrEndFrame(m_Session, &end);
    if (!XR_SUCCEEDED(endRes) || (m_FrameLogCounter % 120u) == 1u)
    {
        Game::logMsg("xrEndFrame %s (%d) layers=%u shouldRender=%d state=%s submitted=%d",
                     XrResultString(endRes), (int)endRes, end.layerCount,
                     (int)m_FrameState.shouldRender, SessionStateName(m_SessionState),
                     (int)m_SubmittedLayersThisFrame);
    }
    m_FrameStarted = false;
    m_SubmittedLayersThisFrame = false;
}

void OpenXrBackend::Shutdown()
{
    if (m_VkDevice)
    {
        if (m_BlitFence)
            vkDestroyFence(m_VkDevice, m_BlitFence, nullptr);
        if (m_CmdPool)
            vkDestroyCommandPool(m_VkDevice, m_CmdPool, nullptr);
        m_BlitFence = VK_NULL_HANDLE;
        m_CmdPool = VK_NULL_HANDLE;
        m_CmdBuf = VK_NULL_HANDLE;
    }

    auto destroy = [](auto fn, auto h) { if (h) fn(h); };
    for (int i = 0; i < 2; ++i)
    {
        destroy(xrDestroySpace, m_HandSpace[i]);
        destroy(xrDestroySpace, m_AimSpace[i]);
        destroy(xrDestroySwapchain, m_Swapchain[i]);
    }
    destroy(xrDestroySpace, m_LocalSpace);
    destroy(xrDestroySpace, m_ViewSpace);
    if (m_Session)
    {
        if (m_SessionRunning)
            xrEndSession(m_Session);
        xrDestroySession(m_Session);
    }
    if (m_Instance)
        xrDestroyInstance(m_Instance);

    *this = OpenXrBackend{};
}
