#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "hooks.h"
#include "d3d9_vr.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace
{
    void AngleVectorsLocal(const QAngle &angles, Vector *forward, Vector *right, Vector *up)
    {
        float sr, sp, sy, cr, cp, cy;
        const float pitch = angles.x * (3.14159265f / 180.f);
        const float yaw = angles.y * (3.14159265f / 180.f);
        const float roll = angles.z * (3.14159265f / 180.f);
        sp = sinf(pitch); cp = cosf(pitch);
        sy = sinf(yaw);   cy = cosf(yaw);
        sr = sinf(roll);  cr = cosf(roll);

        if (forward)
        {
            forward->x = cp * cy;
            forward->y = cp * sy;
            forward->z = -sp;
        }
        if (right)
        {
            right->x = -1.f * sr * sp * cy + -1.f * cr * -sy;
            right->y = -1.f * sr * sp * sy + -1.f * cr * cy;
            right->z = -1.f * sr * cp;
        }
        if (up)
        {
            up->x = cr * sp * cy + -sr * -sy;
            up->y = cr * sp * sy + -sr * cy;
            up->z = cr * cp;
        }
    }

    void AspectFitDestRect(UINT srcW, UINT srcH, UINT dstW, UINT dstH, RECT &outDst)
    {
        if (!srcW || !srcH || !dstW || !dstH)
        {
            outDst = { 0, 0, (LONG)dstW, (LONG)dstH };
            return;
        }
        const float sa = (float)srcW / (float)srcH;
        const float da = (float)dstW / (float)dstH;
        if (sa > da)
        {
            LONG h = (LONG)lroundf((float)dstW / sa);
            if (h < 1) h = 1;
            if (h > (LONG)dstH) h = (LONG)dstH;
            const LONG y = ((LONG)dstH - h) / 2;
            outDst = { 0, y, (LONG)dstW, y + h };
        }
        else
        {
            LONG w = (LONG)lroundf((float)dstH * sa);
            if (w < 1) w = 1;
            if (w > (LONG)dstW) w = (LONG)dstW;
            const LONG x = ((LONG)dstW - w) / 2;
            outDst = { x, 0, x + w, (LONG)dstH };
        }
    }

    // Prefer Vulkan VR desc size — DXVK GetDesc on the backbuffer is often a 1x1 stub.
    bool ResolveSurfaceSize(IDirect3DSurface9 *surf, UINT &outW, UINT &outH, D3DSURFACE_DESC *optDesc = nullptr)
    {
        outW = 0;
        outH = 0;
        if (!surf)
            return false;

        D3DSURFACE_DESC desc{};
        const bool haveDesc = SUCCEEDED(surf->GetDesc(&desc));
        if (optDesc && haveDesc)
            *optDesc = desc;

        if (g_D3DVR9)
        {
            D3D9_TEXTURE_VR_DESC vr{};
            if (SUCCEEDED(g_D3DVR9->GetVRDesc(surf, &vr)) && vr.Width >= 640 && vr.Height >= 360)
            {
                outW = vr.Width;
                outH = vr.Height;
                return true;
            }
        }

        if (haveDesc && desc.Width >= 640 && desc.Height >= 360)
        {
            outW = desc.Width;
            outH = desc.Height;
            return true;
        }
        return false;
    }

    UINT KnownWindowWidth(const SharedTextureHolder &bb)
    {
        if (bb.m_VulkanData.m_nWidth >= 640)
            return bb.m_VulkanData.m_nWidth;
        return 1280;
    }

    UINT KnownWindowHeight(const SharedTextureHolder &bb)
    {
        if (bb.m_VulkanData.m_nHeight >= 360)
            return bb.m_VulkanData.m_nHeight;
        return 720;
    }
}

VR::VR(Game *game)
{
    m_Game = game;
    ParseConfigFile();

    // Wait for DXVK VR interface off the render thread; OpenXR init itself runs on Present (Update).
    std::thread([this]() {
        for (int i = 0; i < 600 && !g_D3DVR9; ++i)
            Sleep(50);

        if (!g_D3DVR9)
        {
            Game::logMsg("IDirect3DVR9 unavailable — DXVK VR did not initialize.");
            return;
        }

        m_RenderWidth = 1512;
        m_RenderHeight = 1680;
        m_Aspect = (float)m_RenderWidth / (float)m_RenderHeight;
        m_IsInitialized = true;
        Game::logMsg("BMSVR waiting for OpenXR (from Present thread)");
    }).detach();
}

bool VR::InitOpenXrFromDxvk()
{
    if (!g_D3DVR9)
        return false;
    if (m_Xr.IsInitialized())
        return true;
    if (m_Xr.InitFailed())
        return false;

    Game::logMsg("OpenXR init: GetBackBufferData...");
    if (FAILED(g_D3DVR9->GetBackBufferData(&m_VKBackBuffer)))
    {
        Game::logMsg("GetBackBufferData failed");
        return false;
    }

    const auto &v = m_VKBackBuffer.m_VulkanData;
    if (!v.m_pDevice || !v.m_pInstance)
    {
        Game::logMsg("Backbuffer Vulkan handles empty");
        return false;
    }

    Game::logMsg("OpenXR init: xr probe...");
    if (!m_Xr.Init(
            (VkInstance)v.m_pInstance,
            (VkPhysicalDevice)v.m_pPhysicalDevice,
            (VkDevice)v.m_pDevice,
            (VkQueue)v.m_pQueue,
            v.m_nQueueFamilyIndex))
    {
        Game::logMsg("OpenXR init: probe failed");
        return false;
    }

    if (m_Xr.RecommendedWidth())
    {
        m_RenderWidth = m_Xr.RecommendedWidth();
        m_RenderHeight = m_Xr.RecommendedHeight();
        m_Aspect = m_Xr.Aspect();
        m_Fov = m_Xr.FovDegrees();
    }
    Game::logMsg("OpenXR init: probe ok %ux%u", m_RenderWidth, m_RenderHeight);
    return true;
}

Vector VR::ScalePos(const Vector &meters) const
{
    return Vector{ meters.x * m_VRScale, meters.y * m_VRScale, meters.z * m_VRScale };
}

void VR::ParseConfigFile()
{
    std::ifstream in("bmsvr.cfg");
    if (!in)
        in.open("VR\\bmsvr.cfg");
    if (!in)
        return;

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '/' || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "vr_scale") ss >> m_VRScale;
        else if (key == "snap_turn") { int v; ss >> v; m_SnapTurning = v != 0; }
        else if (key == "snap_angle") ss >> m_SnapTurnAngle;
        else if (key == "turn_speed") ss >> m_TurnSpeed;
        else if (key == "left_handed") { int v; ss >> v; m_LeftHanded = v != 0; }
        else if (key == "roomscale") { int v; ss >> v; m_RoomscaleActive = v != 0; }
        else if (key == "hud_distance") ss >> m_HudDistance;
        else if (key == "msaa") ss >> m_AntiAliasing;
        else if (key == "stereo_copy") { int v; ss >> v; m_StereoCopyOffset = v != 0; }
        else if (key == "stereo_offset") ss >> m_StereoOffsetPx;
        else if (key == "stereo_converge") ss >> m_StereoConvergeMeters;
        else if (key == "soft_pitch") { int v; ss >> v; m_SoftPitchLook = v != 0; }
        else if (key == "move_deadzone") ss >> m_MoveDeadzone;
        else if (key == "turn_deadzone") ss >> m_TurnDeadzone;
        else if (key == "eye_use_hmd_res") { int v; ss >> v; m_EyeUseHmdRes = v != 0; }
        else if (key == "capture_src") ss >> m_CaptureSrc;
        else if (key == "viewmodel_vr") { int v; ss >> v; m_ViewmodelVr = v != 0; }
        else if (key == "viewmodel_follow") { int v; ss >> v; m_ViewmodelFollow = v != 0; }
        else if (key == "viewmodel_aim") { int v; ss >> v; m_ViewmodelAim = v != 0; }
        else if (key == "viewmodel_off_f") ss >> m_ViewmodelOffForward;
        else if (key == "viewmodel_off_r") ss >> m_ViewmodelOffRight;
        else if (key == "viewmodel_off_u") ss >> m_ViewmodelOffUp;
    }
    for (char &c : m_CaptureSrc)
    {
        c = (char)std::tolower((unsigned char)c);
        if (c == '_') c = '-'; // rt0_vp → rt0-vp
    }
    if (m_CaptureSrc.empty())
        m_CaptureSrc = "auto";
    Game::logMsg("cfg soft_pitch=%d stereo_copy=%d eye_hmd_res=%d capture_src=%s viewmodel_vr=%d follow=%d aim=%d off=(%.1f,%.1f,%.1f) move_dz=%.2f turn_dz=%.2f",
                 (int)m_SoftPitchLook, (int)m_StereoCopyOffset, (int)m_EyeUseHmdRes,
                 m_CaptureSrc.c_str(),
                 (int)m_ViewmodelVr, (int)m_ViewmodelFollow, (int)m_ViewmodelAim,
                 m_ViewmodelOffForward, m_ViewmodelOffRight, m_ViewmodelOffUp,
                 m_MoveDeadzone, m_TurnDeadzone);
}

void VR::CreateVRTextures()
{
    // Avoid MaterialSystem Begin/EndRenderTargetAllocation — that path destabilizes BM.
    // Allocate eye color + shared depth directly on the DXVK D3D9 device.
    if (!g_D3DVR9 || m_EyeRtCreateAttempted)
        return;

    if (!m_RenderWidth)
    {
        m_RenderWidth = 1512;
        m_RenderHeight = 1680;
    }

    IDirect3DDevice9 *device = nullptr;
    if (FAILED(g_D3DVR9->GetDevice(&device)) || !device)
    {
        Game::logMsg("GetDevice failed for eye RT create");
        return;
    }

    // Prefer real window/backbuffer size from DXVK VR desc (GetBackBuffer can be 1x1 briefly).
    // OpenXR compositor scales to HMD recommended resolution; optional cfg uses HMD size.
    UINT eyeW = m_VKBackBuffer.m_VulkanData.m_nWidth;
    UINT eyeH = m_VKBackBuffer.m_VulkanData.m_nHeight;
    if (m_EyeUseHmdRes && m_RenderWidth >= 640 && m_RenderHeight >= 360)
    {
        eyeW = m_RenderWidth;
        eyeH = m_RenderHeight;
    }
    else if (eyeW < 640 || eyeH < 360)
    {
        eyeW = 1280;
        eyeH = 720;
    }

    m_EyeRtCreateAttempted = true;
    Game::logMsg("Creating VR eye RTs via D3D CreateRenderTarget %ux%u (hmd rec %ux%u)",
                 eyeW, eyeH, m_RenderWidth, m_RenderHeight);

    auto capture = [this](IDirect3DSurface9 *surf, TextureID id, SharedTextureHolder *vk,
                          IDirect3DSurface9 **slot) {
        if (!surf)
            return;
        if (*slot)
            (*slot)->Release();
        *slot = surf; // CreateRenderTarget already AddRef'd for the caller
        D3D9_TEXTURE_VR_DESC texDesc{};
        if (SUCCEEDED(g_D3DVR9->GetVRDesc(surf, &texDesc)))
        {
            memcpy(&vk->m_VulkanData, &texDesc, sizeof(vk->m_VulkanData));
            vk->m_VRTexture.handle = &vk->m_VulkanData;
            vk->m_VRTexture.eColorSpace = 0;
            vk->m_VRTexture.eType = 2;
            Game::logMsg("Eye RT id=%d %ux%u img=%llu", (int)id, texDesc.Width, texDesc.Height,
                         (unsigned long long)texDesc.Image);
        }
    };

    // Leave m_CreatingTextureID = None so CreateRenderTargetEx does not double-capture.
    IDirect3DSurface9 *left = nullptr;
    IDirect3DSurface9 *right = nullptr;
    IDirect3DSurface9 *depth = nullptr;

    HRESULT hrL = device->CreateRenderTarget(
        eyeW, eyeH, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &left, nullptr);
    HRESULT hrR = device->CreateRenderTarget(
        eyeW, eyeH, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &right, nullptr);
    HRESULT hrD = device->CreateDepthStencilSurface(
        eyeW, eyeH, D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr);

    Game::logMsg("CreateRenderTarget hrL=0x%08X hrR=0x%08X hrD=0x%08X",
                 (unsigned)hrL, (unsigned)hrR, (unsigned)hrD);

    if (SUCCEEDED(hrL) && left)
        capture(left, Texture_LeftEye, &m_VKLeftEye, &m_D9LeftEyeSurface);
    else if (left)
        left->Release();

    if (SUCCEEDED(hrR) && right)
        capture(right, Texture_RightEye, &m_VKRightEye, &m_D9RightEyeSurface);
    else if (right)
        right->Release();

    if (SUCCEEDED(hrD) && depth)
    {
        if (m_D9DepthSurface)
            m_D9DepthSurface->Release();
        m_D9DepthSurface = depth;
    }
    else if (depth)
        depth->Release();

    // Pre-create frame-copy RT so unbind capture never CreateRenderTarget mid-SetRenderTarget.
    EnsureFrameCopySurface(device, eyeW, eyeH);

    device->Release();

    m_CreatedVRTextures = (m_D9LeftEyeSurface && m_D9RightEyeSurface && m_D9DepthSurface
                           && m_VKLeftEye.m_VulkanData.m_nImage
                           && m_VKRightEye.m_VulkanData.m_nImage);
    Game::logMsg("VR D3D eye RTs ready=%d L=%p R=%p D=%p", m_CreatedVRTextures ? 1 : 0,
                 (void *)m_D9LeftEyeSurface, (void *)m_D9RightEyeSurface, (void *)m_D9DepthSurface);
}

bool VR::EnsureFrameCopySurface(IDirect3DDevice9 *device, uint32_t width, uint32_t height)
{
    if (!device || width < 640 || height < 360)
        return false;

    if (m_D9FrameColorSurface && m_FrameCopyWidth == width && m_FrameCopyHeight == height)
        return true;

    IDirect3DSurface9 *surf = nullptr;
    const HRESULT hr = device->CreateRenderTarget(
        width, height, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr);
    if (FAILED(hr) || !surf)
    {
        Game::logMsg("Frame copy RT create failed hr=0x%08X %ux%u", (unsigned)hr, width, height);
        return false;
    }

    if (m_D9FrameColorSurface)
        m_D9FrameColorSurface->Release();
    m_D9FrameColorSurface = surf;
    m_FrameCopyWidth = width;
    m_FrameCopyHeight = height;
    Game::logMsg("Frame copy RT ready %ux%u", width, height);
    return true;
}

void VR::CaptureGameColorOnUnbind(IDirect3DSurface9 *oldRt,
                                  uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH)
{
    // Source often finishes FullFrameFB then SetRenderTarget away and clears it
    // before Present. Pre-Present GetRenderTarget(0) can still point at that
    // surface but already black — StretchRect S_OK + lit=0 under DXVK_ASYNC.
    // Copy here (after DXVK FlushImplicit) while GPU content is still valid.
    if (!m_IsVREnabled || !m_CreatedVRTextures || !g_D3DVR9 || !oldRt)
        return;
    if (oldRt == m_D9FrameColorSurface || oldRt == m_D9LeftEyeSurface
        || oldRt == m_D9RightEyeSurface || oldRt == m_D9HUDSurface
        || oldRt == m_D9BlankSurface)
        return;

    UINT rtW = 0, rtH = 0;
    D3DSURFACE_DESC desc{};
    if (!ResolveSurfaceSize(oldRt, rtW, rtH, &desc))
        return;

    const UINT winW = KnownWindowWidth(m_VKBackBuffer);
    const UINT winH = KnownWindowHeight(m_VKBackBuffer);
    // Only FullFrameFB-class POT oversized RTs. Window-sized ping-pong RTs fire
    // many SetRenderTarget unbinds per frame — StretchRect there destabilized BM.
    const bool oversized = (rtW > winW + 16) || (rtH > winH + 16);
    if (!oversized || rtW < 640 || rtH < 360)
        return;

    if (desc.Format != D3DFMT_UNKNOWN
        && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8
        && desc.Format != D3DFMT_A8B8G8R8 && desc.Format != D3DFMT_X8B8G8R8
        && desc.Format != D3DFMT_A2R10G10B10 && desc.Format != D3DFMT_A16B16G16R16F
        && desc.Format != D3DFMT_A32B32G32R32F)
        return;

    IDirect3DDevice9 *device = nullptr;
    if (FAILED(g_D3DVR9->GetDevice(&device)) || !device)
        return;

    // Window-sized viewport only. Full-RT vp (e.g. 0,0 2048x1024) + min(win)
    // collapses to rt0-tl and shows half/offset UI.
    const bool vpOk = vpW >= 640 && vpH >= 360
        && vpW <= winW + 16 && vpH <= winH + 16;
    LONG x0 = 0, y0 = 0;
    UINT cropW = (std::min)(rtW, winW);
    UINT cropH = (std::min)(rtH, winH);
    const char *cropName = "unbind-tl";

    auto applyVp = [&]() {
        if (!vpOk) return false;
        x0 = (LONG)vpX;
        y0 = (LONG)vpY;
        cropW = (std::min)((UINT)vpW, winW);
        cropH = (std::min)((UINT)vpH, winH);
        cropName = "unbind-vp";
        return true;
    };
    auto applyTl = [&]() {
        x0 = 0; y0 = 0;
        cropW = (std::min)(rtW, winW);
        cropH = (std::min)(rtH, winH);
        cropName = "unbind-tl";
    };
    auto applyBl = [&]() {
        cropW = (std::min)(rtW, winW);
        cropH = (std::min)(rtH, winH);
        x0 = 0;
        y0 = (rtH > cropH) ? (LONG)(rtH - cropH) : 0;
        cropName = "unbind-bl";
    };
    auto applyCtr = [&]() {
        cropW = (std::min)(rtW, winW);
        cropH = (std::min)(rtH, winH);
        x0 = (rtW > cropW) ? (LONG)((rtW - cropW) / 2) : 0;
        y0 = (rtH > cropH) ? (LONG)((rtH - cropH) / 2) : 0;
        cropName = "unbind-ctr";
    };

    std::string want = m_CaptureSrcLocked.empty() ? m_CaptureSrc : m_CaptureSrcLocked;
    for (char &c : want) c = (char)std::tolower((unsigned char)c);
    if (want == "rt0-tl" || want == "rt0_tl") applyTl();
    else if (want == "rt0-bl" || want == "rt0_bl") applyBl();
    else if (want == "rt0-ctr" || want == "rt0_ctr") applyCtr();
    else if (want == "rt0-vp" || want == "rt0_vp" || want == "auto" || want.empty())
    {
        if (!applyVp())
            applyBl();
    }
    else if (!applyVp())
        applyBl();

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if ((UINT)x0 >= rtW || (UINT)y0 >= rtH)
    {
        device->Release();
        return;
    }
    cropW = (std::min)(cropW, rtW - (UINT)x0);
    cropH = (std::min)(cropH, rtH - (UINT)y0);
    if (cropW < 640 || cropH < 360)
    {
        device->Release();
        return;
    }

    if (!EnsureFrameCopySurface(device, cropW, cropH))
    {
        device->Release();
        return;
    }

    RECT srcRect{ x0, y0, x0 + (LONG)cropW, y0 + (LONG)cropH };
    const bool useRect = oversized || x0 != 0 || y0 != 0;
    const HRESULT hr = device->StretchRect(
        oldRt, useRect ? &srcRect : nullptr,
        m_D9FrameColorSurface, nullptr, D3DTEXF_NONE);
    device->Release();

    if (FAILED(hr))
        return;

    m_RenderedNewFrame = true;
    m_FrameCopyLatched = true;
    if (m_CaptureSrcLocked.empty() && (m_CaptureSrc == "auto" || m_CaptureSrc.empty()))
    {
        if (strcmp(cropName, "unbind-vp") == 0) m_CaptureSrcLocked = "rt0-vp";
        else if (strcmp(cropName, "unbind-tl") == 0) m_CaptureSrcLocked = "rt0-tl";
        else if (strcmp(cropName, "unbind-bl") == 0) m_CaptureSrcLocked = "rt0-bl";
        else if (strcmp(cropName, "unbind-ctr") == 0) m_CaptureSrcLocked = "rt0-ctr";
    }

    static int s_unbindLog;
    if (s_unbindLog < 4 || (s_unbindLog % 120) == 0)
    {
        Game::logMsg(
            "Unbind capture %s src=%ux%u rect=(%ld,%ld)-(%ld,%ld) hr=0x%08X "
            "(rt=%ux%u win=%ux%u vp=%u,%u %ux%u) locked=%s",
            cropName, cropW, cropH,
            useRect ? srcRect.left : 0, useRect ? srcRect.top : 0,
            useRect ? srcRect.right : (LONG)cropW,
            useRect ? srcRect.bottom : (LONG)cropH,
            (unsigned)hr, rtW, rtH, winW, winH,
            vpX, vpY, vpW, vpH,
            m_CaptureSrcLocked.empty() ? "-" : m_CaptureSrcLocked.c_str());
    }
    ++s_unbindLog;
}

void VR::CaptureFrameBeforePresent()
{
    // Prefer unbind latch — by Present, FullFrameFB is often already cleared
    // (CPU readback lit=0 with StretchRect S_OK). Never use unresolved bb-win.
    if (!m_IsVREnabled || !m_CreatedVRTextures || !g_D3DVR9)
        return;

    if (m_FrameCopyLatched && m_D9FrameColorSurface)
    {
        m_RenderedNewFrame = true;
        static bool s_latchedPresentLog;
        if (!s_latchedPresentLog)
        {
            s_latchedPresentLog = true;
            Game::logMsg("PrePresent using unbind-latched copy %ux%u locked=%s",
                         m_FrameCopyWidth, m_FrameCopyHeight,
                         m_CaptureSrcLocked.empty() ? "-" : m_CaptureSrcLocked.c_str());
        }
        return;
    }

    IDirect3DDevice9 *device = nullptr;
    if (FAILED(g_D3DVR9->GetDevice(&device)) || !device)
        return;

    IDirect3DSurface9 *rt0 = nullptr;
    IDirect3DSurface9 *bb = nullptr;
    device->GetRenderTarget(0, &rt0);
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

    UINT rtW = 0, rtH = 0, bbW = 0, bbH = 0;
    D3DSURFACE_DESC rtDesc{}, bbDesc{};
    const bool rtOk = ResolveSurfaceSize(rt0, rtW, rtH, &rtDesc);
    const bool bbOk = ResolveSurfaceSize(bb, bbW, bbH, &bbDesc);

    const UINT winW = KnownWindowWidth(m_VKBackBuffer);
    const UINT winH = KnownWindowHeight(m_VKBackBuffer);

    D3DVIEWPORT9 vp{};
    const bool vpRawOk = SUCCEEDED(device->GetViewport(&vp)) && vp.Width >= 640 && vp.Height >= 360;
    // Letterbox crop only when viewport ≈ window. Full-RT vp → false (use rt0-bl).
    const bool vpOk = vpRawOk && vp.Width <= winW + 16 && vp.Height <= winH + 16;

    struct Cand
    {
        IDirect3DSurface9 *surf = nullptr;
        UINT w = 0, h = 0;
        RECT rect{};
        bool useRect = false;
        const char *name = "none";
    };

    auto makeRt0Crop = [&](LONG x0, LONG y0, UINT cropW, UINT cropH, const char *name) -> Cand {
        Cand c{};
        if (!rtOk || !rt0 || cropW < 640 || cropH < 360)
            return c;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if ((UINT)x0 >= rtW || (UINT)y0 >= rtH)
            return c;
        cropW = (std::min)(cropW, rtW - (UINT)x0);
        cropH = (std::min)(cropH, rtH - (UINT)y0);
        if (cropW < 640 || cropH < 360)
            return c;
        c.surf = rt0;
        c.w = cropW;
        c.h = cropH;
        c.name = name;
        if (rtW > winW + 16 || rtH > winH + 16 || x0 != 0 || y0 != 0)
        {
            c.rect = { x0, y0, x0 + (LONG)cropW, y0 + (LONG)cropH };
            c.useRect = true;
        }
        return c;
    };

    auto candByName = [&](const std::string &name) -> Cand {
        if (name == "bb")
        {
            // Only resolved backbuffer — unresolved stub StretchRect → pure black (S_OK).
            if (!bb || !bbOk)
            {
                static bool s_bbStubLog;
                if (!s_bbStubLog)
                {
                    s_bbStubLog = true;
                    D3DSURFACE_DESC stub{};
                    const bool haveStub = bb && SUCCEEDED(bb->GetDesc(&stub));
                    Game::logMsg(
                        "PrePresent skip bb (unresolved/stub) desc=%dx%d resolved=%d — never StretchRect stub",
                        haveStub ? (int)stub.Width : -1, haveStub ? (int)stub.Height : -1,
                        bbOk ? 1 : 0);
                }
                return {};
            }
            Cand c{};
            c.surf = bb;
            c.w = bbW;
            c.h = bbH;
            c.name = "bb";
            return c;
        }
        if (name == "rt0")
        {
            if (!rtOk || !rt0 || rtW < 640 || rtH < 360)
                return {};
            // Only exact/window-sized RT0 — oversized must use explicit crops.
            if (rtW > winW + 16 || rtH > winH + 16)
                return {};
            Cand c{};
            c.surf = rt0;
            c.w = rtW;
            c.h = rtH;
            c.name = "rt0";
            return c;
        }
        if (name == "rt0-vp" || name == "rt0_vp")
        {
            if (!vpOk)
            {
                static bool s_vpSkipLog;
                if (!s_vpSkipLog && vpRawOk)
                {
                    s_vpSkipLog = true;
                    Game::logMsg(
                        "PrePresent rt0-vp skipped (vp %ux%u not window-sized; win=%ux%u) — prefer rt0-bl",
                        vp.Width, vp.Height, winW, winH);
                }
                return {};
            }
            return makeRt0Crop((LONG)vp.X, (LONG)vp.Y,
                               (std::min)((UINT)vp.Width, winW),
                               (std::min)((UINT)vp.Height, winH), "rt0-vp");
        }
        if (name == "rt0-tl" || name == "rt0_tl")
            return makeRt0Crop(0, 0, (std::min)(rtW, winW), (std::min)(rtH, winH), "rt0-tl");
        if (name == "rt0-bl" || name == "rt0_bl")
        {
            const UINT cropW = (std::min)(rtW, winW);
            const UINT cropH = (std::min)(rtH, winH);
            const LONG y0 = (rtH > cropH) ? (LONG)(rtH - cropH) : 0;
            return makeRt0Crop(0, y0, cropW, cropH, "rt0-bl");
        }
        if (name == "rt0-ctr" || name == "rt0_ctr")
        {
            const UINT cropW = (std::min)(rtW, winW);
            const UINT cropH = (std::min)(rtH, winH);
            const LONG x0 = (rtW > cropW) ? (LONG)((rtW - cropW) / 2) : 0;
            const LONG y0 = (rtH > cropH) ? (LONG)((rtH - cropH) / 2) : 0;
            return makeRt0Crop(x0, y0, cropW, cropH, "rt0-ctr");
        }
        return {};
    };

    auto sampleNonBlack = [&](IDirect3DSurface9 *gpuSurf, UINT w, UINT h,
                               unsigned &outRgb, HRESULT &outReadHr) -> bool {
        outRgb = 0;
        outReadHr = E_FAIL;
        if (!gpuSurf || w < 8 || h < 8)
            return false;
        IDirect3DSurface9 *sys = nullptr;
        if (FAILED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        outReadHr = device->GetRenderTargetData(gpuSurf, sys);
        bool lit = false;
        if (SUCCEEDED(outReadHr))
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
                // Sample center + a few offsets (UI may not sit at dead center).
                const int samples[][2] = {
                    { (int)(w / 2), (int)(h / 2) },
                    { (int)(w / 4), (int)(h / 4) },
                    { (int)(3 * w / 4), (int)(h / 4) },
                    { (int)(w / 2), (int)(h / 3) },
                    { (int)(w / 2), (int)(2 * h / 3) },
                };
                unsigned best = 0;
                for (const auto &s : samples)
                {
                    const BYTE *row = (const BYTE *)lr.pBits + s[1] * lr.Pitch;
                    const DWORD px = *(const DWORD *)(row + s[0] * 4);
                    const unsigned b = px & 0xFF;
                    const unsigned g = (px >> 8) & 0xFF;
                    const unsigned r = (px >> 16) & 0xFF;
                    const unsigned sum = r + g + b;
                    if (sum > best)
                    {
                        best = sum;
                        outRgb = (r << 16) | (g << 8) | b;
                    }
                    if (sum >= 24)
                        lit = true;
                }
                sys->UnlockRect();
            }
        }
        sys->Release();
        return lit;
    };

    auto tryCopy = [&](const Cand &c, HRESULT &outHr) -> bool {
        outHr = E_FAIL;
        if (!c.surf || c.w < 640 || c.h < 360)
            return false;
        if (!EnsureFrameCopySurface(device, c.w, c.h))
            return false;
        const RECT *pRect = c.useRect ? &c.rect : nullptr;
        outHr = device->StretchRect(c.surf, pRect, m_D9FrameColorSurface, nullptr, D3DTEXF_NONE);
        return SUCCEEDED(outHr);
    };

    auto logSizes = [&](const char *tag, const Cand &c, HRESULT hr, unsigned rgb, int lit, HRESULT readHr) {
        Game::logMsg(
            "%s %s StretchRect=0x%08X read=0x%08X src=%ux%u rect=(%ld,%ld)-(%ld,%ld) "
            "bestRGB=0x%06X lit=%d (rt0=%dx%d bb=%dx%d win=%ux%u vp=%u,%u %ux%u)",
            tag, c.name, (unsigned)hr, (unsigned)readHr, c.w, c.h,
            c.useRect ? c.rect.left : 0, c.useRect ? c.rect.top : 0,
            c.useRect ? c.rect.right : (LONG)c.w,
            c.useRect ? c.rect.bottom : (LONG)c.h,
            rgb, lit,
            rtOk ? (int)rtW : -1, rtOk ? (int)rtH : -1,
            bbOk ? (int)bbW : -1, bbOk ? (int)bbH : -1,
            winW, winH,
            vpOk ? vp.X : 0u, vpOk ? vp.Y : 0u,
            vpOk ? vp.Width : 0u, vpOk ? vp.Height : 0u);
    };

    Cand chosen{};
    HRESULT chosenHr = E_FAIL;

    // Forced cfg source (capture_src != auto).
    if (m_CaptureSrc != "auto" && m_CaptureSrcLocked.empty())
        m_CaptureSrcLocked = m_CaptureSrc;

    if (!m_CaptureSrcLocked.empty())
    {
        Cand c = candByName(m_CaptureSrcLocked);
        if (c.surf && tryCopy(c, chosenHr))
            chosen = c;
        else if (c.surf)
        {
            static bool s_lockFail;
            if (!s_lockFail)
            {
                s_lockFail = true;
                Game::logMsg("PrePresent locked src=%s StretchRect FAILED hr=0x%08X",
                             c.name, (unsigned)chosenHr);
            }
        }
    }
    else
    {
        // auto: probe RT0 crops only. Never auto-pick bb (stub StretchRect → black S_OK).
        // Prefer rt0-vp (letterboxed FullFrameFB); then rt0-bl. rt0-tl was half/offset UI.
        // CPU lit under DXVK_ASYNC is unreliable — lock preferred crop on first OK copy.
        const char *order[] = {
            "rt0-vp",
            "rt0-bl",
            "rt0-tl",
            "rt0-ctr",
            "rt0",
        };

        Cand fallbackVp{};
        Cand fallbackBl{};
        Cand fallbackAny{};
        HRESULT fallbackHr = E_FAIL;
        static int s_probeRounds;
        const bool logThisRound = (s_probeRounds < 3) || ((s_probeRounds % 90) == 0);
        ++s_probeRounds;

        for (const char *nm : order)
        {
            Cand c = candByName(nm);
            if (!c.surf)
                continue;
            if (fallbackAny.surf && fallbackAny.name && c.name &&
                strcmp(fallbackAny.name, c.name) == 0)
                continue;

            HRESULT hr = E_FAIL;
            if (!tryCopy(c, hr))
            {
                if (logThisRound)
                    Game::logMsg("PrePresent StretchRect FAILED hr=0x%08X src=%s %ux%u",
                                 (unsigned)hr, c.name, c.w, c.h);
                continue;
            }

            unsigned rgb = 0;
            HRESULT readHr = E_FAIL;
            // Optional diagnostic only — do not gate lock on lit (DXVK_ASYNC → always dark).
            const bool lit = logThisRound
                && sampleNonBlack(m_D9FrameColorSurface, c.w, c.h, rgb, readHr);
            if (logThisRound)
                logSizes("PrePresent probe", c, hr, rgb, (int)lit, readHr);

            if (!fallbackAny.surf)
            {
                fallbackAny = c;
                fallbackHr = hr;
            }
            if (!fallbackVp.surf && strcmp(c.name, "rt0-vp") == 0)
                fallbackVp = c;
            if (!fallbackBl.surf && strcmp(c.name, "rt0-bl") == 0)
                fallbackBl = c;

            // Prefer locking rt0-vp (or first successful preferred) immediately.
            if (strcmp(c.name, "rt0-vp") == 0 || strcmp(c.name, "rt0-bl") == 0)
            {
                chosen = c;
                chosenHr = hr;
                m_CaptureSrcLocked = c.name;
                Game::logMsg("PrePresent probe LOCKED src=%s after %d round(s) (prefer vp/bl; CPU lit ignored)",
                             c.name, s_probeRounds);
                break;
            }
        }

        if (!chosen.surf)
        {
            Cand use = fallbackVp.surf ? fallbackVp
                : (fallbackBl.surf ? fallbackBl : fallbackAny);
            if (use.surf)
            {
                tryCopy(use, fallbackHr);
                chosen = use;
                chosenHr = fallbackHr;
                m_CaptureSrcLocked = use.name;
                if (logThisRound)
                    Game::logMsg("PrePresent probe: locking fallback %s", use.name);
            }
        }
    }

    if (chosen.surf && SUCCEEDED(chosenHr))
    {
        m_RenderedNewFrame = true;
        static bool s_srcLog;
        if (!s_srcLog && !m_CaptureSrcLocked.empty())
        {
            s_srcLog = true;
            Game::logMsg(
                "PrePresent capture %s src=%ux%u rect=(%ld,%ld)-(%ld,%ld) hr=0x%08X "
                "(rt0=%dx%d bb=%dx%d win=%ux%u vp=%u,%u %ux%u) locked=%s -> copyRT ok",
                chosen.name, chosen.w, chosen.h,
                chosen.useRect ? chosen.rect.left : 0, chosen.useRect ? chosen.rect.top : 0,
                chosen.useRect ? chosen.rect.right : (LONG)chosen.w,
                chosen.useRect ? chosen.rect.bottom : (LONG)chosen.h,
                (unsigned)chosenHr,
                rtOk ? (int)rtW : -1, rtOk ? (int)rtH : -1,
                bbOk ? (int)bbW : -1, bbOk ? (int)bbH : -1,
                winW, winH,
                vpOk ? vp.X : 0u, vpOk ? vp.Y : 0u,
                vpOk ? vp.Width : 0u, vpOk ? vp.Height : 0u,
                m_CaptureSrcLocked.c_str());
        }
    }
    else
    {
        static bool s_failLog;
        if (!s_failLog)
        {
            s_failLog = true;
            Game::logMsg(
                "PrePresent capture NONE (rt0=%dx%d bb=%dx%d win=%ux%u vp=%u,%u %ux%u mode=%s)",
                rtOk ? (int)rtW : -1, rtOk ? (int)rtH : -1,
                bbOk ? (int)bbW : -1, bbOk ? (int)bbH : -1,
                winW, winH,
                vpOk ? vp.X : 0u, vpOk ? vp.Y : 0u,
                vpOk ? vp.Width : 0u, vpOk ? vp.Height : 0u,
                m_CaptureSrc.c_str());
        }
    }

    if (rt0) rt0->Release();
    if (bb) bb->Release();
    device->Release();
}

void VR::SubmitVRTextures()
{
    if (!g_D3DVR9 || !m_Xr.IsSessionRunning())
        return;

    auto fillEye = [](const SharedTextureHolder &h, XrVulkanEyeImage &out) {
        out.image = (VkImage)h.m_VulkanData.m_nImage;
        out.device = (VkDevice)h.m_VulkanData.m_pDevice;
        out.physicalDevice = (VkPhysicalDevice)h.m_VulkanData.m_pPhysicalDevice;
        out.instance = (VkInstance)h.m_VulkanData.m_pInstance;
        out.queue = (VkQueue)h.m_VulkanData.m_pQueue;
        out.queueFamilyIndex = h.m_VulkanData.m_nQueueFamilyIndex;
        out.width = h.m_VulkanData.m_nWidth;
        out.height = h.m_VulkanData.m_nHeight;
        out.format = (VkFormat)h.m_VulkanData.m_nFormat;
    };

    auto transferEye = [&](IDirect3DSurface9 *surf, XrVulkanEyeImage &out) -> bool {
        if (!surf)
            return false;
        g_D3DVR9->TransferSurface(surf, FALSE);
        D3D9_TEXTURE_VR_DESC texDesc{};
        if (FAILED(g_D3DVR9->GetVRDesc(surf, &texDesc)) || !texDesc.Image)
            return false;
        SharedTextureHolder holder{};
        memcpy(&holder.m_VulkanData, &texDesc, sizeof(holder.m_VulkanData));
        holder.m_VRTexture.handle = &holder.m_VulkanData;
        holder.m_VRTexture.eType = 2;
        fillEye(holder, out);
        return true;
    };

    if (!m_Xr.BeginFrame())
    {
        m_RenderedNewFrame = false;
        return;
    }

    // Copy private pre-Present frame into eye RTs (aspect-fit), Transfer, Submit.
    // Submitting the live game RT on the shared VkQueue races DXVK and exits BM.
    if (m_RenderedNewFrame && m_D9FrameColorSurface)
    {
        if (!m_CreatedVRTextures)
            CreateVRTextures();

        IDirect3DDevice9 *device = nullptr;
        if (m_D9LeftEyeSurface && SUCCEEDED(g_D3DVR9->GetDevice(&device)) && device)
        {
            D3DSURFACE_DESC srcDesc{};
            D3DSURFACE_DESC dstDesc{};
            const bool haveSrc = SUCCEEDED(m_D9FrameColorSurface->GetDesc(&srcDesc))
                && srcDesc.Width >= 64 && srcDesc.Height >= 64;
            const bool haveDst = SUCCEEDED(m_D9LeftEyeSurface->GetDesc(&dstDesc))
                && dstDesc.Width >= 64 && dstDesc.Height >= 64;

            // 1:1 StretchRect into eye RTs (no D3D letterbox). OpenXR blit stretch-fills
            // the HMD swapchain with V-flip — avoids double aspect-fit framing bugs.
            HRESULT hrL = E_FAIL;
            HRESULT hrR = E_FAIL;
            RECT usedDst{ 0, 0, (LONG)dstDesc.Width, (LONG)dstDesc.Height };
            LONG usedOff = 0;
            if (m_StereoCopyOffset && m_D9RightEyeSurface && haveSrc && haveDst)
            {
                LONG useOff = m_StereoOffsetPx;
                if (useOff <= 0)
                {
                    useOff = (LONG)lroundf(
                        m_Xr.SuggestedStereoOffsetPx(srcDesc.Width, m_StereoConvergeMeters));
                }
                const LONG maxOff = (LONG)(srcDesc.Width / 8);
                if (useOff < 1) useOff = 1;
                if (useOff > maxOff) useOff = maxOff;
                usedOff = useOff;

                RECT leftSrc{ useOff, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
                RECT rightSrc{ 0, 0, (LONG)srcDesc.Width - useOff, (LONG)srcDesc.Height };

                hrL = device->StretchRect(m_D9FrameColorSurface, &leftSrc,
                                          m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = device->StretchRect(m_D9FrameColorSurface, &rightSrc,
                                          m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR);
            }
            else if (haveSrc && haveDst)
            {
                hrL = device->StretchRect(m_D9FrameColorSurface, nullptr,
                                          m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = hrL;
                if (m_D9RightEyeSurface)
                    hrR = device->StretchRect(m_D9FrameColorSurface, nullptr,
                                              m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR);
            }

            static bool s_capLog;
            if (!s_capLog && haveSrc && haveDst)
            {
                s_capLog = true;
                Game::logMsg(
                    "Capture path src=%ux%u -> eyeRT=%ux%u dst=(%ld,%ld)-(%ld,%ld) "
                    "stereoCopy=%d off=%ldpx upsample=%d stretch=1",
                    srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height,
                    usedDst.left, usedDst.top, usedDst.right, usedDst.bottom,
                    m_StereoCopyOffset ? 1 : 0, usedOff,
                    (dstDesc.Width > srcDesc.Width || dstDesc.Height > srcDesc.Height) ? 1 : 0);
            }

            g_D3DVR9->LockDevice();
            // Transfer without WaitDeviceIdle — WaitDeviceIdle every frame exits BM.
            XrVulkanEyeImage leftEye{};
            XrVulkanEyeImage rightEye{};
            const bool okL = SUCCEEDED(hrL) && transferEye(m_D9LeftEyeSurface, leftEye);
            bool okR = false;
            if (m_StereoCopyOffset && m_D9RightEyeSurface && SUCCEEDED(hrR))
                okR = transferEye(m_D9RightEyeSurface, rightEye);
            else if (SUCCEEDED(hrR) && m_D9RightEyeSurface)
                okR = transferEye(m_D9RightEyeSurface, rightEye);

            if (okL)
            {
                const bool submitted = okR
                    ? m_Xr.SubmitEyes(leftEye, rightEye)
                    : m_Xr.SubmitEyes(leftEye, leftEye);

                if (!m_LoggedFirstSubmit)
                {
                    m_LoggedFirstSubmit = true;
                    Game::logMsg("OpenXR SubmitEyes after StretchRect+Transfer %ux%u stereoCopy=%d dualEye=%d submitted=%d state=%s",
                                 leftEye.width, leftEye.height, m_StereoCopyOffset ? 1 : 0,
                                 (okR ? 1 : 0), (int)submitted,
                                 OpenXrBackend::SessionStateName(m_Xr.SessionState()));
                }
            }
            g_D3DVR9->UnlockDevice();
            device->Release();
        }
    }
    else if (m_RenderedNewFrame)
    {
        static bool s_missLog;
        if (!s_missLog)
        {
            s_missLog = true;
            Game::logMsg("No frame color RT captured yet for OpenXR submit");
        }
    }

    m_Xr.EndFrame();
    m_RenderedNewFrame = false;
    m_FrameCopyLatched = false;
}

void VR::UpdateTracking()
{
    m_HmdPoseValid = false;
    const auto &hmd = m_Xr.Hmd();
    if (hmd.valid)
    {
        QAngle ang = hmd.angles;
        ang.y += m_RotationOffset;
        // Clamp pitch; zero roll — Source view roll tends to destabilize BM.
        if (ang.x > 89.f) ang.x = 89.f;
        if (ang.x < -89.f) ang.x = -89.f;
        ang.z = 0.f;
        // Reject NaN / absurd values (fail-open: keep previous).
        if (std::isfinite(ang.x) && std::isfinite(ang.y)
            && fabsf(ang.x) < 360.f && fabsf(ang.y) < 1.0e5f)
        {
            m_HmdAngAbs = ang;
            m_HmdPosAbs = ScalePos(hmd.position);
            m_HmdPosAbs.z += m_HeightOffset;
            m_HmdPoseValid = true;
        }
    }

    AngleVectorsLocal(m_HmdAngAbs, &m_HmdForward, &m_HmdRight, &m_HmdUp);

    m_ControllerPoseValid = false;
    const auto &hand = m_LeftHanded ? m_Xr.LeftHand() : m_Xr.RightHand();
    if (hand.valid)
    {
        QAngle ang = hand.angles;
        ang.y += m_RotationOffset;
        ang.z = 0.f;
        Vector pos = ScalePos(hand.position);
        pos.z += m_HeightOffset;
        if (std::isfinite(ang.x) && std::isfinite(ang.y)
            && std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)
            && fabsf(ang.x) < 360.f && fabsf(ang.y) < 1.0e5f)
        {
            m_RightControllerAngAbs = ang;
            m_RightControllerPosAbs = pos;
            m_ControllerPoseValid = true;
            static bool s_loggedCtrl;
            if (!s_loggedCtrl)
            {
                s_loggedCtrl = true;
                Game::logMsg("Controller aim pose VALID (hand=%s) pos=(%.1f,%.1f,%.1f)",
                             m_LeftHanded ? "left" : "right",
                             pos.x, pos.y, pos.z);
            }
        }
    }

    m_SetupOriginToHMD = m_HmdPosAbs - m_CameraAnchor;
}

void VR::ProcessInput()
{
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;

    const auto &in = m_Xr.Input();

    if (in.resetSeated)
        ResetPosition();

    if (m_SnapTurning)
    {
        if (in.turnX > 0.6f && !m_PressedTurn)
        {
            m_RotationOffset -= m_SnapTurnAngle;
            m_PressedTurn = true;
        }
        else if (in.turnX < -0.6f && !m_PressedTurn)
        {
            m_RotationOffset += m_SnapTurnAngle;
            m_PressedTurn = true;
        }
        else if (fabsf(in.turnX) < 0.4f)
            m_PressedTurn = false;
    }
    else
    {
        float turn = in.turnX;
        const float tdz = m_TurnDeadzone > 0.f ? m_TurnDeadzone : 0.f;
        if (tdz > 0.f && tdz < 0.95f)
        {
            if (fabsf(turn) < tdz)
                turn = 0.f;
            else
            {
                const float sign = turn > 0.f ? 1.f : -1.f;
                turn = sign * (fabsf(turn) - tdz) / (1.f - tdz);
            }
        }
        m_RotationOffset -= turn * m_TurnSpeed * 3.f;
    }

    auto edge = [&](bool now, bool &was, const char *on, const char *off) {
        if (now && !was) m_Game->ClientCmd_Unrestricted(on);
        if (!now && was) m_Game->ClientCmd_Unrestricted(off);
        was = now;
    };
    static bool s_jump, s_atk, s_atk2, s_reload, s_use, s_duck, s_speed;
    edge(in.jump, s_jump, "+jump", "-jump");
    edge(in.primaryAttack, s_atk, "+attack", "-attack");
    edge(in.secondaryAttack, s_atk2, "+attack2", "-attack2");
    edge(in.reload, s_reload, "+reload", "-reload");
    edge(in.use, s_use, "+use", "-use");
    edge(in.crouch, s_duck, "+duck", "-duck");
    edge(in.sprint, s_speed, "+speed", "-speed");

    if (in.nextWeapon) m_Game->ClientCmd_Unrestricted("invnext");
    if (in.prevWeapon) m_Game->ClientCmd_Unrestricted("invprev");
    if (in.pause) m_Game->ClientCmd_Unrestricted("gameui_activate");

    auto deadzone = [](float v, float dz) -> float {
        if (dz <= 0.f || dz >= 0.95f)
            return v;
        if (fabsf(v) < dz)
            return 0.f;
        const float sign = v > 0.f ? 1.f : -1.f;
        return sign * (fabsf(v) - dz) / (1.f - dz);
    };
    m_Game->m_AnalogForward = deadzone(in.moveY, m_MoveDeadzone);
    m_Game->m_AnalogSide = deadzone(in.moveX, m_MoveDeadzone);
}

void VR::ResetPosition()
{
    m_HeightOffset = -m_HmdPosAbs.z + 64.f;
    m_CameraAnchor = m_HmdPosAbs;
    m_CameraAnchor.z = 0;
}

void VR::TryCaptureSurface(IDirect3DSurface9 *surf, TextureID hint)
{
    if (!surf || !g_D3DVR9)
        return;

    TextureID texID = hint;
    if (texID == Texture_None)
        texID = m_CreatingTextureID;
    if (texID == Texture_None)
        texID = ConsumePendingRtId();

    // Size fallback only while actively creating VR textures — never steal random 512² menu RTs.
    if (texID == Texture_None && m_CreatedVRTextures == false && m_IsVREnabled && m_RenderWidth > 0
        && (m_CreatingTextureID != Texture_None || m_PendingRtNext < m_PendingRtCount))
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(surf->GetDesc(&desc)))
        {
            if (desc.Width == m_RenderWidth && desc.Height == m_RenderHeight)
            {
                if (!m_D9LeftEyeSurface)
                    texID = Texture_LeftEye;
                else if (!m_D9RightEyeSurface)
                    texID = Texture_RightEye;
                else if (!m_D9HUDSurface)
                    texID = Texture_HUD;
            }
            else if (desc.Width == 512 && desc.Height == 512 && !m_D9BlankSurface
                     && m_CreatingTextureID == Texture_Blank)
                texID = Texture_Blank;
        }
    }

    if (texID == Texture_None)
        return;

    SharedTextureHolder *target = nullptr;
    IDirect3DSurface9 **slot = nullptr;
    if (texID == Texture_LeftEye) { target = &m_VKLeftEye; slot = &m_D9LeftEyeSurface; }
    else if (texID == Texture_RightEye) { target = &m_VKRightEye; slot = &m_D9RightEyeSurface; }
    else if (texID == Texture_HUD) { target = &m_VKHUD; slot = &m_D9HUDSurface; }
    else if (texID == Texture_Blank) { target = &m_VKBlankTexture; slot = &m_D9BlankSurface; }
    else return;

    if (*slot == surf)
        return;

    if (*slot)
        (*slot)->Release();
    *slot = surf;
    surf->AddRef();

    D3D9_TEXTURE_VR_DESC texDesc{};
    if (FAILED(g_D3DVR9->GetVRDesc(surf, &texDesc)))
        return;

    memcpy(&target->m_VulkanData, &texDesc, sizeof(target->m_VulkanData));
    target->m_VRTexture.handle = &target->m_VulkanData;
    target->m_VRTexture.eColorSpace = 0;
    target->m_VRTexture.eType = 2;
    Game::logMsg("Captured VR surface id=%d %ux%u img=%llu", (int)texID, texDesc.Width, texDesc.Height,
                 (unsigned long long)texDesc.Image);
}

bool VR::IsGameplayMapName(const char *map)
{
    if (!map || !map[0])
        return false;

    // Use basename (engine usually passes bare names; tolerate maps/background01).
    const char *base = map;
    for (const char *p = map; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    if (!base[0])
        return false;

    // Menu / attract maps: background01, background04, …
    static const char kBg[] = "background";
    for (size_t i = 0; kBg[i]; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(base[i]);
        if (!c || static_cast<char>(std::tolower(c)) != kBg[i])
            return true; // diverged before full prefix → real map (e.g. bm_c0a0a)
    }
    return false; // basename starts with "background"
}

void VR::OnLevelInit(const char *newmap)
{
    const char *name = newmap ? newmap : "";
    m_CurrentMapName = name;
    m_GameplayEligible = IsGameplayMapName(name);
    m_GameplayFrames = 0;
    m_CaptureSrcLocked.clear();
    m_FrameCopyLatched = false;
    if (!m_GameplayEligible)
    {
        m_SeenGameplay = false;
        // Returning to menu / attract: drop OpenXR so SteamVR isn't held open.
        if (m_Xr.IsInitialized())
        {
            Game::logMsg("LevelInit background map — shutting down OpenXR");
            m_Xr.Shutdown();
            m_SessionReadyLogged = false;
            m_LoggedFirstSubmit = false;
        }
    }

    Game::logMsg("LevelInit map='%s' vr_eligible=%d",
                 name[0] ? name : "(null)", (int)m_GameplayEligible);
}

void VR::OnLevelShutdown()
{
    Game::logMsg("LevelShutdown map='%s' (clearing vr eligibility)",
                 m_CurrentMapName.empty() ? "(none)" : m_CurrentMapName.c_str());
    m_GameplayEligible = false;
    m_CurrentMapName.clear();
    m_GameplayFrames = 0;
    m_SeenGameplay = false;
    // Do not Shutdown OpenXR here — map→map transitions Shutdown then LevelInit;
    // tear down only when the next LevelInit is a background* map.
}

void VR::Update()
{
    if (!m_IsInitialized || !m_Game || !m_Game->m_Initialized)
        return;

    // Primary gate: LevelInit must have named a real (non-background*) map.
    // Secondary: SeenGameplay + sustained CreateMove (BM IsInGame false-positives on menu).
    const bool rawInGame = m_GameplayEligible
        && m_SeenGameplay
        && m_Game->m_EngineClient
        && m_Game->m_EngineClient->IsInGame();
    if (rawInGame)
        ++m_GameplayFrames;
    else
        m_GameplayFrames = 0;

    // ~1.5s at 60fps — LevelInit already filters background*; short settle only.
    constexpr int kGameplayFramesBeforeVr = 90;
    const bool inGame = rawInGame && m_GameplayFrames >= kGameplayFramesBeforeVr;
    if (!inGame)
    {
        if (m_IsVREnabled)
        {
            m_IsVREnabled = false;
            m_CreatedVRTextures = false;
            m_EyeRtCreateAttempted = false;
            m_LoggedFirstSubmit = false;
            m_StereoBindEye = Texture_None;
            m_SafeLookActive = false;
            m_LookApplyEnabled = false;
            m_ProcessInputEnabled = false;
            m_ValidPoseFrames = 0;
            m_HmdPoseValid = false;
            m_ControllerPoseValid = false;
            m_SessionFocusFrames = 0;
            m_LeftEyeTexture = m_RightEyeTexture = m_HUDTexture = m_BlankTexture = nullptr;
            auto releaseSurf = [](IDirect3DSurface9 *&s) {
                if (s) { s->Release(); s = nullptr; }
            };
            releaseSurf(m_D9LeftEyeSurface);
            releaseSurf(m_D9RightEyeSurface);
            releaseSurf(m_D9HUDSurface);
            releaseSurf(m_D9BlankSurface);
            releaseSurf(m_D9DepthSurface);
            releaseSurf(m_D9FrameColorSurface);
            m_FrameCopyWidth = 0;
            m_FrameCopyHeight = 0;
            m_FrameCopyLatched = false;
            m_CaptureSrcLocked.clear();
        }
        return;
    }

    if (!m_Xr.IsInitialized() && !m_Xr.InitFailed() && g_D3DVR9)
    {
        Game::logMsg("In-game confirmed map='%s' (%d frames) — starting OpenXR",
                     m_CurrentMapName.c_str(), m_GameplayFrames);
        InitOpenXrFromDxvk();
    }

    if (!m_Xr.IsInitialized())
        return;

    if (m_Xr.RecommendedWidth() && !m_SessionReadyLogged)
    {
        m_SessionReadyLogged = true;
        m_RenderWidth = m_Xr.RecommendedWidth();
        m_RenderHeight = m_Xr.RecommendedHeight();
        m_Aspect = m_Xr.Aspect();
        m_Fov = m_Xr.FovDegrees();
        Game::logMsg("OpenXR ready %ux%u", m_RenderWidth, m_RenderHeight);
    }

    // Start OpenXR session once map + gameplay confirmed.
    if (!m_Xr.HasSession())
    {
        if (!m_Xr.StartSession())
        {
            Game::logMsg("OpenXR StartSession failed");
            return;
        }
    }

    if (m_Xr.HasSession() && !m_Xr.IsSessionRunning())
        m_Xr.PollEvents();

    if (!m_Xr.IsSessionRunning())
        return;

    if (!m_IsVREnabled)
    {
        m_IsVREnabled = true;
        m_SafeLookActive = false;
        m_LookApplyEnabled = false;
        m_ProcessInputEnabled = false;
        m_ValidPoseFrames = 0;
        m_SessionFocusFrames = 0;
        Game::logMsg("BMSVR VR ENABLED in-game (%ux%u)", m_RenderWidth, m_RenderHeight);
    }

    m_Xr.PollEvents();
    if (!m_Xr.IsSessionRunning())
        return;

    ++m_SessionFocusFrames;

    // Phase 1: empty-frame pacing until session settles (~60 frames) — do NOT sync poses yet.
    if (m_SessionFocusFrames == 60 || (m_SessionFocusFrames > 60 && !m_CreatedVRTextures && !m_EyeRtCreateAttempted))
        CreateVRTextures();

    if (!m_CreatedVRTextures)
    {
        if (m_Xr.BeginFrame())
            m_Xr.EndFrame();
        return;
    }

    // Frame color is copied in CaptureFrameBeforePresent (pre-Present). Do not
    // re-bind game RT0 here — post-Present contents are often black/undefined.

    // Phase 2: submit mono/stereo-copy frames first (proven path).
    SubmitVRTextures();

    // Phase 3: poses only after BeginFrame has run (Submit sets predictedDisplayTime).
    // Fail-open: if pose sync misbehaves, submit path already completed this frame.
    constexpr int kPoseStartFrames = 150; // 30 frames after RT create
    if (m_SessionFocusFrames >= kPoseStartFrames)
    {
        m_Xr.UpdatePosesAndInput();
        UpdateTracking();

        if (m_HmdPoseValid)
            ++m_ValidPoseFrames;
        else
            m_ValidPoseFrames = 0;

        constexpr int kLookStableFrames = 60;
        if (!m_SafeLookActive && m_ValidPoseFrames >= kLookStableFrames)
        {
            m_SafeLookActive = true;
            m_PrevAppliedHmdYaw = m_HmdAngAbs.y;
            Game::logMsg("Safe HMD look READY (tracking ok)");
        }

        // Enable clamped relative-yaw apply after more stable frames (absolute replace crashed).
        constexpr int kLookApplyFrames = 180;
        if (m_SafeLookActive && !m_LookApplyEnabled && m_ValidPoseFrames >= kLookApplyFrames)
        {
            m_LookApplyEnabled = true;
            m_PrevAppliedHmdYaw = m_HmdAngAbs.y;
            m_PrevAppliedHmdPitch = m_HmdAngAbs.x;
            Game::logMsg("Safe HMD look APPLY relative yaw%s (clamped)",
                         m_SoftPitchLook ? "+soft_pitch" : "");
        }

        // ProcessInput (buttons/turn/analog) after look apply has been live a bit.
        constexpr int kInputStableFrames = 300;
        if (m_LookApplyEnabled && !m_ProcessInputEnabled && m_ValidPoseFrames >= kInputStableFrames)
        {
            m_ProcessInputEnabled = true;
            Game::logMsg("ProcessInput ENABLED (pose/look stable)");
        }

        if (m_ProcessInputEnabled)
            ProcessInput();
    }
}

Vector VR::GetViewAngle()
{
    return Vector{ m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z };
}

Vector VR::GetViewOriginLeft()
{
    return m_SetupOrigin + m_SetupOriginToHMD + ScalePos(m_Xr.EyeOffsetMeters(0));
}

Vector VR::GetViewOriginRight()
{
    return m_SetupOrigin + m_SetupOriginToHMD + ScalePos(m_Xr.EyeOffsetMeters(1));
}

Vector VR::GetRecommendedViewmodelAbsPos()
{
    // Prefer eye-relative controller delta (works without a latched SetupOrigin).
    Vector delta;
    if (TryGetControllerRelToHmd(delta))
        return m_SetupOrigin + m_SetupOriginToHMD + delta;
    return m_SetupOrigin + (m_RightControllerPosAbs - m_CameraAnchor);
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    return m_RightControllerAngAbs;
}

bool VR::TryGetControllerRelToHmd(Vector &outDelta) const
{
    outDelta = {};
    if (!m_ControllerPoseValid || !m_HmdPoseValid)
        return false;
    Vector d = m_RightControllerPosAbs - m_HmdPosAbs;
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z))
        return false;
    // Reject absurd tracking jumps (arm reach ~1m ≈ 39 hu; allow some slack).
    constexpr float kMaxReachHu = 80.f;
    if (VectorLength(d) > kMaxReachHu)
        return false;
    outDelta = d;
    return true;
}
