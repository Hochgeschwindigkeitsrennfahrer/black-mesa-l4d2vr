#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "offsets.h"
#include "MinHook.h"
#include "d3d9_vr.h"
#include "bmvr_flags.h"
#include "hooks.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include "vr_hands.h"
#include "vr_hud_icons.h"
#include "openxr_helper_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using tPresent = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using tSetRenderTarget = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using tSetDepthStencil = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
    tPresent g_OrigPresent = nullptr;
    tSetRenderTarget g_OrigSetRenderTarget = nullptr;
    tSetDepthStencil g_OrigSetDepthStencil = nullptr;
    bool g_DeviceHooksEnabled = false;
    BmVrGloves g_VrGloves;

    constexpr UINT kIDirect3DDevice9_Present = 17;
    constexpr UINT kIDirect3DDevice9_SetRenderTarget = 37;
    constexpr UINT kIDirect3DDevice9_SetDepthStencilSurface = 39;

    template <typename T>
    void ReleaseT(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    static bool FileExistsA(const char* path)
    {
        const DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // Rank scene-color RTs for stereo unbind blit. Copy LDR only.
    // A2R10/R16F FullFrame (fmt 35/111) is pre-tonemap HDR. StretchRect into
    // A8R8G8B8 eyes looks untextured-white in the HMD while the desktop
    // swapchain (post-tonemap) stays textured. 2026-08-18 also blacked the
    // tram when this copy was stretched onto the backbuffer.
    static int SceneColorRank(D3DFORMAT format)
    {
        switch (format)
        {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
            return 3;
        default:
            return 0;
        }
    }

    static std::string DirFromModulePath(const wchar_t* full)
    {
        char out[MAX_PATH]{};
        WideCharToMultiByte(CP_ACP, 0, full, -1, out, MAX_PATH, nullptr, nullptr);
        std::string s(out);
        const size_t slash = s.find_last_of("\\/");
        if (slash == std::string::npos)
            return ".";
        return s.substr(0, slash);
    }

    QAngle HmdMatrixToSourceAngles(const vr::HmdMatrix34_t& mat)
    {
        QAngle ang;
        ang.x = asinf(mat.m[1][2]) * (180.0f / 3.141592654f);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0f / 3.141592654f);
        ang.z = 0.f;
        return ang;
    }

    QAngle HmdMatrixToSourceAnglesWithRoll(const vr::HmdMatrix34_t& mat)
    {
        QAngle ang;
        ang.x = asinf(mat.m[1][2]) * (180.0f / 3.141592654f);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0f / 3.141592654f);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0f / 3.141592654f);
        return ang;
    }

    float WrapYaw(float yaw)
    {
        return yaw - 360.f * floorf((yaw + 180.f) / 360.f);
    }

    void PivotYaw(Vector& delta, float yawDeg)
    {
        const float rad = yawDeg * (3.14159265f / 180.f);
        const float s = sinf(rad);
        const float c = cosf(rad);
        const float nx = delta.x * c - delta.y * s;
        const float ny = delta.x * s + delta.y * c;
        delta.x = nx;
        delta.y = ny;
    }

    Vector HmdMatrixToSourcePos(const vr::HmdMatrix34_t& mat, float scale)
    {
        Vector pos;
        pos.x = -mat.m[2][3] * scale;
        pos.y = -mat.m[0][3] * scale;
        pos.z = mat.m[1][3] * scale;
        return pos;
    }

    vr::HmdMatrix34_t MulHmd34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b)
    {
        vr::HmdMatrix34_t o{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
            o.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] + a.m[r][2] * b.m[2][3] + a.m[r][3];
        }
        return o;
    }

    // G2 / WMR device pose is the tracking-ring origin. OpenVR grip/tip is the
    // handle. Do not apply this to the gun (offsets were tuned on raw pose).
    vr::HmdMatrix34_t DevicePoseToGrip(vr::IVRSystem* system, vr::TrackedDeviceIndex_t idx,
        const vr::HmdMatrix34_t& devicePose)
    {
        if (!system || idx >= vr::k_unMaxTrackedDeviceCount)
            return devicePose;
        vr::IVRRenderModels* rm = vr::VRRenderModels();
        if (!rm)
            return devicePose;
        char model[vr::k_unMaxPropertyStringSize]{};
        if (system->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String,
                model, sizeof(model)) == 0 || !model[0])
            return devicePose;
        vr::VRControllerState_t cs{};
        if (!system->GetControllerState(idx, &cs, sizeof(cs)))
            memset(&cs, 0, sizeof(cs));
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t comp{};
        const char* names[] = {
            vr::k_pch_Controller_Component_OpenXR_Grip,
            vr::k_pch_Controller_Component_HandGrip,
            vr::k_pch_Controller_Component_Tip,
        };
        static int s_gripLog;
        for (const char* name : names)
        {
            if (!rm->GetComponentState(model, name, &cs, &mode, &comp))
                continue;
            if (s_gripLog < 4)
            {
                Game::logMsg("Hand grip component=%s model=%s t=(%.3f,%.3f,%.3f)",
                    name, model, comp.mTrackingToComponentRenderModel.m[0][3],
                    comp.mTrackingToComponentRenderModel.m[1][3],
                    comp.mTrackingToComponentRenderModel.m[2][3]);
                ++s_gripLog;
            }
            return MulHmd34(devicePose, comp.mTrackingToComponentRenderModel);
        }
        if (s_gripLog < 4)
        {
            Game::logMsg("Hand grip component missing model=%s — raw device pose", model);
            ++s_gripLog;
        }
        return devicePose;
    }

    bool QueryGameClientSize(UINT& w, UINT& h);

    bool SurfaceMatchesWindowOrBackbuffer(IDirect3DDevice9* device, IDirect3DSurface9* surf)
    {
        if (!device || !surf)
            return false;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        {
            const bool isBb = (surf == bb);
            bb->Release();
            if (isBb)
                return true;
        }
        D3DSURFACE_DESC desc{};
        if (FAILED(surf->GetDesc(&desc)) || desc.Width < 640 || desc.Height < 360)
            return false;
        UINT winW = 0, winH = 0;
        if (QueryGameClientSize(winW, winH) && desc.Width == winW && desc.Height == winH)
            return true;
        uint32_t fbW = 0, fbH = 0;
        if (bmvr::HaveHmdFramebufferSize(fbW, fbH) && desc.Width == fbW && desc.Height == fbH)
            return true;
        return false;
    }

    HRESULT __stdcall HookedPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty)
    {
        if (g_Game && g_Game->m_VR)
            g_Game->m_VR->CaptureFrameBeforePresent();
        if (!g_OrigPresent)
            return D3DERR_INVALIDCALL;
        return g_OrigPresent(device, src, dst, hwnd, dirty);
    }

    HRESULT __stdcall HookedSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* rt)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        // Map-load stdshader hammers SetRT. GetRenderTarget/GetViewport on
        // every call was extra DXVK lock + COM during that nest. Capture
        // only matters while a stereo eye blit is in progress.
        bool redirectedToEye = false;
        if (index == 0 && vr && device && !vr->m_CaptureReentry && vr->StereoEyeBlitActive())
        {
            if ((bmvr::TrySteamVrEyeRt() || bmvr::OffscreenWorldMatchesEyes())
                && vr->StereoEyeBlitDest() && rt
                && rt != vr->StereoEyeBlitDest()
                && SurfaceMatchesWindowOrBackbuffer(device, rt))
            {
                rt = vr->StereoEyeBlitDest();
                vr->NoteStereoRedirectedToEye();
                redirectedToEye = true;
            }
            IDirect3DSurface9* oldRt = nullptr;
            D3DVIEWPORT9 vp{};
            device->GetRenderTarget(0, &oldRt);
            const bool haveVp = SUCCEEDED(device->GetViewport(&vp));
            if (oldRt && oldRt != rt)
            {
                vr->CaptureGameColorOnUnbind(
                    oldRt,
                    haveVp ? vp.X : 0,
                    haveVp ? vp.Y : 0,
                    haveVp ? vp.Width : 0,
                    haveVp ? vp.Height : 0);
            }
            if (oldRt)
                oldRt->Release();
        }
        if (!g_OrigSetRenderTarget)
            return D3DERR_INVALIDCALL;
        const HRESULT hr = g_OrigSetRenderTarget(device, index, rt);
        // Redirecting the 2560x1440 backbuffer onto a taller eye keeps the
        // window viewport (1440). DXVK only forces full-eye size on the next
        // SetViewport while RT0 is already the eye — so do that here.
        if (SUCCEEDED(hr) && redirectedToEye && vr
            && vr->m_RenderWidth >= 640 && vr->m_RenderHeight >= 360)
        {
            D3DVIEWPORT9 eyeVp{};
            eyeVp.X = 0;
            eyeVp.Y = 0;
            eyeVp.Width = vr->m_RenderWidth;
            eyeVp.Height = vr->m_RenderHeight;
            eyeVp.MinZ = 0.f;
            eyeVp.MaxZ = 1.f;
            device->SetViewport(&eyeVp);
        }
        return hr;
    }

    HRESULT __stdcall HookedSetDepthStencil(IDirect3DDevice9* device, IDirect3DSurface9* depth)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        IDirect3DSurface9* eyeDepth = nullptr;
        if (vr && vr->StereoEyeBlitActive()
            && (bmvr::TrySteamVrEyeRt() || bmvr::OffscreenWorldMatchesEyes()))
        {
            if (vr->StereoEyeBlitDest() == vr->m_D9LeftEyeSurface)
                eyeDepth = vr->m_D9LeftEyeDepthSurface;
            else if (vr->StereoEyeBlitDest() == vr->m_D9RightEyeSurface)
                eyeDepth = vr->m_D9RightEyeDepthSurface;
        }
        if (vr && device && !vr->m_CaptureReentry && eyeDepth
            && depth && depth != eyeDepth
            && SurfaceMatchesWindowOrBackbuffer(device, depth))
        {
            depth = eyeDepth;
        }
        if (!g_OrigSetDepthStencil)
            return D3DERR_INVALIDCALL;
        return g_OrigSetDepthStencil(device, depth);
    }

    bool QueryGameClientSize(UINT& w, UINT& h)
    {
        HWND hwnd = FindWindowA("Valve001", nullptr);
        if (!hwnd)
            hwnd = FindWindowA(nullptr, "Black Mesa");
        if (!hwnd)
            return false;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return false;
        w = static_cast<UINT>(rc.right - rc.left);
        h = static_cast<UINT>(rc.bottom - rc.top);
        return w >= 640 && h >= 360;
    }

    using tBeginRTAlloc = void(__thiscall*)(void*);
    using tEndRTAlloc = void(__thiscall*)(void*);
    using tCreateNamedRTEx = ITexture*(__thiscall*)(void*, const char*, int, int, int, int, int, unsigned, unsigned);

    ITexture* SehCreateNamedEyeRT(tCreateNamedRTEx fn, void* mat, const char* name, int w, int h)
    {
        ITexture* tex = nullptr;
        if (!fn || !mat)
            return nullptr;
        __try
        {
            tex = fn(mat, name, w, h, RT_SIZE_LITERAL, IMAGE_FORMAT_RGBA16161616F,
                MATERIAL_RT_DEPTH_SHARED,
                TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
                CREATERENDERTARGETFLAGS_HDR);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tex = nullptr;
        }
        return tex;
    }

    void SehBeginRTAlloc(tBeginRTAlloc fn, void* mat, bool& ok)
    {
        ok = false;
        if (!fn || !mat)
            return;
        __try
        {
            fn(mat);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
    }

    void SehEndRTAlloc(tEndRTAlloc fn, void* mat)
    {
        if (!fn || !mat)
            return;
        __try
        {
            fn(mat);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    const char* SehGetLevelNameShort(IEngineClient* eng)
    {
        const char* map = nullptr;
        if (!eng)
            return "";
        __try
        {
            map = eng->GetLevelNameShort();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            map = nullptr;
        }
        return map ? map : "";
    }

    bool SehIsPaused(IEngineClient* eng)
    {
        if (!eng)
            return false;
        bool paused = false;
        __try
        {
            paused = eng->IsPaused();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            paused = false;
        }
        return paused;
    }

    HWND FindGameHwnd()
    {
        HWND hwnd = FindWindowA("Valve001", nullptr);
        if (!hwnd)
            hwnd = FindWindowA(nullptr, "Black Mesa");
        return hwnd;
    }

    bool GameWindowIsForeground()
    {
        HWND hwnd = FindGameHwnd();
        if (!hwnd)
            return false;
        return GetForegroundWindow() == hwnd;
    }

    void SehSetCursorPos(IInput* input, int x, int y)
    {
        if (!input)
            return;
        __try { input->SetCursorPos(x, y); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

VR::VR(Game* game)
    : m_Game(game)
{
    Game::logMsg("VR ctor: L4D2VR OpenVR / OpenXR helper path (Black Mesa capture)");
    const VrRuntimeBackendConfig runtimeConfig = L4D2VR_ReadRuntimeBackendConfig();
    m_RuntimeBackendFallbackToOpenVR = true;
    const VrRuntimeBackendSelection runtimeSelection =
        L4D2VR_SelectRuntimeBackend(runtimeConfig.requestedBackend, m_RuntimeBackendFallbackToOpenVR);
    m_RequestedRuntimeBackend = runtimeSelection.requested;
    m_RuntimeBackend = runtimeSelection.active;
    m_OpenXrLoaderAvailable = runtimeSelection.openXrLoaderAvailable;
    Game::logMsg(
        "[VR][Runtime] requested=%s active=%s fallback=%d usedFallback=%d openxrLoader=%d openxrBackendImplemented=%d detail=%s",
        L4D2VR_RuntimeBackendName(runtimeSelection.requested),
        L4D2VR_RuntimeBackendName(runtimeSelection.active),
        runtimeSelection.fallbackToOpenVR ? 1 : 0,
        runtimeSelection.usedFallback ? 1 : 0,
        runtimeSelection.openXrLoaderAvailable ? 1 : 0,
        runtimeSelection.openXrBackendImplemented ? 1 : 0,
        runtimeSelection.message.c_str());

    if (runtimeSelection.active == VrRuntimeBackend::OpenXR)
    {
        m_IsInitialized = InitOpenXR();
        if (m_IsInitialized)
            return;
        Game::logMsg("[VR][Runtime] OpenXR helper init failed; falling back to OpenVR (desktop must still launch)");
        m_OpenXrHelperBridgeActive = false;
        m_RuntimeBackend = VrRuntimeBackend::OpenVR;
    }
    m_IsInitialized = InitOpenVR();
}

bool VR::IsGameplayMapName(const char* map)
{
    if (!map || !map[0])
        return false;
    const char* slash = strrchr(map, '/');
    const char* bslash = strrchr(map, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
    const char* base = slash ? slash + 1 : map;
    std::string name(base);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    for (char& c : name)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name == "dedicated" || name.rfind("background", 0) == 0)
        return false;
    return true;
}

void VR::PollMapFromEngine()
{
    if (!m_Game || !m_Game->m_EngineClient)
        return;
    const char* map = SehGetLevelNameShort(m_Game->m_EngineClient);
    const size_t n = strlen(map);
    if (n > 96)
        return;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(map[i]);
        if (ch < 32 || ch > 126)
            return;
    }
    if (m_CurrentMapName == map)
        return;
    if (!map[0])
    {
        if (!m_CurrentMapName.empty())
            OnLevelShutdown();
    m_CompositorHandoffSlowCount = 0;
        m_CurrentMapName.clear();
        return;
    }
    OnLevelInit(map);
}

void VR::OnLevelInit(const char* newmap)
{
    m_CurrentMapName = newmap ? newmap : "";
    m_GameplayEligible = IsGameplayMapName(newmap);
    bmvr::SetGameplayWorldRts(m_GameplayEligible);
    m_SeenGameplay = false;
    m_GameplayFrames = 0;
    m_EligiblePresents = 0;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_HmdOriginLatched = false;
    m_PassThroughMainViews = 0;
    m_AutoMatQueueModeLastRequested = -999;
    Game::logMsg("LevelInit map=%s eligible=%d", m_CurrentMapName.c_str(), m_GameplayEligible ? 1 : 0);
    ApplyRenderTargetFramebufferOverride();
}

bool VR::ShouldCompositorSubmit() const
{
    if (!m_IsVREnabled)
        return false;
    if (m_GameplayEligible)
    {
        // LevelInit sets eligible before IsInGame. Keep Submit off until
        // spawn so load precache is not also capturing. Size lies stay on
        // so G-buffers and GetScreenSize match (see hooks.cpp).
        return m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    }
    // background01 / menu. Pre-LevelInit StretchRect crashed; require a map name.
    return bmvr::TryMenuCompositor() && !m_CurrentMapName.empty();
}

void VR::OnLevelShutdown()
{
    m_GameplayEligible = false;
    bmvr::SetGameplayWorldRts(false);
    m_SeenGameplay = false;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_DirectEyeSubmit = false;
    m_StereoRenderViewActive = false;
    m_PassThroughMainViews = 0;
    Hooks::RestoreViewmodelArmHides();
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HasViewmodelBake = false;
    }
    m_FirstAttackLogged = false;
    m_FirstAttackPresentTick = 0;
    m_FirstAttackSpikeLogs = 0;
    m_CompositorHandoffSlowCount = 0;
    m_EmptyHands = false;
    Game::logMsg("LevelShutdown");
}

void VR::ApplyRenderTargetFramebufferOverride(void* materialSystem)
{
    if (!bmvr::TryFramebufferOverride())
        return;
    void* mat = materialSystem;
    if (!mat && m_Game)
        mat = m_Game->m_MaterialSystem;
    if (!mat)
        return;

    static uint32_t s_setW = 0;
    static uint32_t s_setH = 0;

    uint32_t w = 0, h = 0;
    static bool s_latchedOffscreen = false;
    if (!bmvr::ComputeGrownWorldFramebuffer(w, h))
    {
        s_latchedOffscreen = false;
        uint32_t winW = 0, winH = 0;
        if (s_setW != 0 && bmvr::QueryWindowClientSize(winW, winH)
            && (s_setW != winW || s_setH != winH))
        {
            w = winW;
            h = winH;
        }
        else
            return;
    }
    else if (s_latchedOffscreen && s_setW != 0)
    {
        // Do not follow live SteamVR SS. G-buffers are sized at map load.
        return;
    }

    void** vt = nullptr;
    __try
    {
        vt = *reinterpret_cast<void***>(mat);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt || !vt[Offsets::kIMaterialSystem_SetRTFBOverrideVt]
        || !vt[Offsets::kIMaterialSystem_GetRTFBDimensionsVt])
        return;

    if (s_setW == w && s_setH == h)
        return;

    using SetFn = void(__thiscall*)(void*, int, int);
    using GetFn = void(__thiscall*)(void*, int&, int&);
    int gotW = 0;
    int gotH = 0;
    __try
    {
        bmvr::BeginRisky(L"fb_override");
        reinterpret_cast<SetFn>(vt[Offsets::kIMaterialSystem_SetRTFBOverrideVt])(
            mat, static_cast<int>(w), static_cast<int>(h));
        reinterpret_cast<GetFn>(vt[Offsets::kIMaterialSystem_GetRTFBDimensionsVt])(
            mat, gotW, gotH);
        s_setW = w;
        s_setH = h;
        uint32_t offW = 0, offH = 0;
        s_latchedOffscreen = bmvr::ComputeGrownWorldFramebuffer(offW, offH);
        bmvr::EndRisky(L"fb_override");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Game::logMsg("SetRenderTargetFrameBufferSizeOverrides SEH %ux%u", w, h);
        return;
    }
    Game::logMsg("SetRenderTargetFrameBufferSizeOverrides %ux%u (Get %dx%d) HWND unchanged",
        w, h, gotW, gotH);
}

void VR::LogFullFrameSizeIfReady()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return;
    static int s_logs;
    void** vt = nullptr;
    __try
    {
        vt = *reinterpret_cast<void***>(m_Game->m_MaterialSystem);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt || !vt[Offsets::kIMaterialSystem_FindTextureVt])
        return;
    using FindFn = ITexture*(__thiscall*)(void*, const char*, const char*, bool, int);
    const char* names[] = { "_rt_FullFrameFB", "_rt_FullFrameFB1", "_rt_gui", "_rt_Hud" };
    bool any = false;
    for (const char* name : names)
    {
        ITexture* tex = nullptr;
        int aw = 0;
        int ah = 0;
        __try
        {
            tex = reinterpret_cast<FindFn>(vt[Offsets::kIMaterialSystem_FindTextureVt])(
                m_Game->m_MaterialSystem, name, "RenderTargets", false, 0);
            if (tex)
            {
                aw = tex->GetActualWidth();
                ah = tex->GetActualHeight();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tex = nullptr;
        }
        if (tex && aw > 0)
        {
            if (s_logs < 4)
                Game::logMsg("FindTexture %s actual %dx%d", name, aw, ah);
            any = true;
            if (name && std::strcmp(name, "_rt_FullFrameFB") == 0)
            {
                const uint32_t nw = static_cast<uint32_t>(aw);
                const uint32_t nh = static_cast<uint32_t>(ah);
                if (nw != bmvr::g_FullFrameActualWidth || nh != bmvr::g_FullFrameActualHeight)
                    Game::logMsg("FullFrame actual %ux%u -> %ux%u",
                        bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight, nw, nh);
                bmvr::g_FullFrameActualWidth = nw;
                bmvr::g_FullFrameActualHeight = nh;
            }
        }
    }
    if (any && s_logs < 8)
        ++s_logs;
}

void VR::HandleMissingRenderContext(const char* location)
{
    Game::logMsg("Missing render context at %s", location ? location : "?");
}

bool VR::InitOpenVR()
{
    ++m_OpenVRInitAttempts;
    m_AntiAliasing = bmvr::g_AntiAliasing;

    if (bmvr::g_OpenVRInitedFromCreateDevice && vr::VRSystem())
    {
        m_System = vr::VRSystem();
        Game::logMsg("OpenVR already initialized from CreateDevice");
    }
    else
    {
        vr::EVRInitError error = vr::VRInitError_None;
        m_System = vr::VR_Init(&error, vr::VRApplication_Scene);
        if (error != vr::VRInitError_None || !m_System)
        {
            Game::logMsg("VR_Init failed (%d): %s", (int)error,
                vr::VR_GetVRInitErrorAsEnglishDescription(error));
            m_System = nullptr;
            m_IsVREnabled = false;
            return false;
        }
    }

    m_Compositor = vr::VRCompositor();
    if (!m_Compositor)
    {
        Game::logMsg("VRCompositor() returned null");
        m_IsVREnabled = false;
        return false;
    }

    m_Input = vr::VRInput();
    m_Overlay = vr::VROverlay();
    if (m_Overlay && bmvr::TryHudOverlay())
    {
        const vr::EVROverlayError oe = m_Overlay->CreateOverlay("bmvr.hud", "Black Mesa VR HUD", &m_HUDTopHandle);
        if (oe == vr::VROverlayError_None)
        {
            m_Overlay->SetOverlayWidthInMeters(m_HUDTopHandle, bmvr::g_HudSize);
            m_Overlay->SetOverlayFlag(m_HUDTopHandle, vr::VROverlayFlags_IgnoreTextureAlpha, false);
            m_Overlay->SetOverlayInputMethod(m_HUDTopHandle, vr::VROverlayInputMethod_None);
            m_Overlay->HideOverlay(m_HUDTopHandle);
            Game::logMsg("HUD overlay created (distance=%.2f m width=%.2f m; hidden until VGUI paints)",
                bmvr::g_HudDistance, bmvr::g_HudSize);
        }
        else
        {
            Game::logMsg("HUD overlay CreateOverlay err=%d", (int)oe);
            m_HUDTopHandle = vr::k_ulOverlayHandleInvalid;
        }
    }

    uint32_t recW = bmvr::g_RecommendedEyeWidth;
    uint32_t recH = bmvr::g_RecommendedEyeHeight;
    if (recW < 640 || recH < 360)
        m_System->GetRecommendedRenderTargetSize(&recW, &recH);
    if (recW >= 640 && recH >= 360)
    {
        bmvr::g_RecommendedEyeWidth = recW;
        bmvr::g_RecommendedEyeHeight = recH;
        // Do not assign m_RenderWidth/Height to the HMD size unless the
        // swapchain retry is still enabled. Reset() forces the D3D9
        // backbuffer to these values; 3168x3100 produced a black desktop
        // and SteamVR waiting room on this DLL (2026-08-16).
        if (bmvr::TryHmdSwapchain())
        {
            m_RenderWidth = recW;
            m_RenderHeight = recH;
        }
    }
    Game::logMsg("OpenVR recommended RT %ux%u (swapchain force=%d, eye/swapchain size %ux%u)",
        recW, recH, bmvr::TryHmdSwapchain() ? 1 : 0, m_RenderWidth, m_RenderHeight);
    ApplyRenderTargetFramebufferOverride();
    RefreshIpdFromHmd();

    float l_left = 0, l_right = 0, l_top = 0, l_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);
    float r_left = 0, r_right = 0, r_top = 0, r_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    const float tanHalfFovX = (std::max)({ -l_left, l_right, -r_left, r_right });
    const float tanHalfFovY = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });

    // Same projection crop as L4D2VR vr_lifecycle_init.inl. Keep OpenVR v
    // unflipped on this DXVK path (Vulkan v-swap inverted the fused image).
    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFovX;
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFovX;
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFovY;
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFovY;
    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFovX;
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFovX;
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFovY;
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFovY;
    Game::logMsg("OpenVR projection UV L=(%.3f,%.3f)-(%.3f,%.3f) R=(%.3f,%.3f)-(%.3f,%.3f)",
        m_TextureBounds[0].uMin, m_TextureBounds[0].vMin, m_TextureBounds[0].uMax, m_TextureBounds[0].vMax,
        m_TextureBounds[1].uMin, m_TextureBounds[1].vMin, m_TextureBounds[1].uMax, m_TextureBounds[1].vMax);

    m_Aspect = tanHalfFovX / tanHalfFovY;
    m_Fov = 2.0f * atanf(tanHalfFovX) * 180.0f / 3.14159265358979323846f;
    ChooseEyeRenderSize();

    m_IsVREnabled = true;
    m_CompositorAppHandoff = bmvr::g_CompositorPostPresentHandoff;
    m_Compositor->SetExplicitTimingMode(m_CompositorAppHandoff
        ? vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff
        : vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
    m_Compositor->CompositorBringToFront();
    SetActionManifest();
    StartPoseWaiter();
    Game::logMsg("OpenVR scene app ready compositor=%p canRender=%d fov=%.1f aspect=%.3f appHandoff=%d aa=%u",
        (void*)m_Compositor, m_Compositor->CanRenderScene() ? 1 : 0, m_Fov, m_Aspect,
        m_CompositorAppHandoff ? 1 : 0, m_AntiAliasing);
    return true;
}

void VR::BindOpenXrActionHandles()
{
    auto handle = [](L4D2VROpenXrActionId id) -> vr::VRActionHandle_t {
        return static_cast<vr::VRActionHandle_t>(static_cast<uint32_t>(id));
    };
    m_ActionJump = handle(L4D2VROpenXrActionId::Jump);
    m_ActionPrimaryAttack = handle(L4D2VROpenXrActionId::PrimaryAttack);
    m_ActionSecondaryAttack = handle(L4D2VROpenXrActionId::SecondaryAttack);
    m_ActionReload = handle(L4D2VROpenXrActionId::Reload);
    m_ActionUse = handle(L4D2VROpenXrActionId::Use);
    m_ActionWalk = handle(L4D2VROpenXrActionId::Walk);
    m_ActionTurn = handle(L4D2VROpenXrActionId::Turn);
    m_ActionNextItem = handle(L4D2VROpenXrActionId::NextItem);
    m_ActionPrevItem = handle(L4D2VROpenXrActionId::PrevItem);
    m_ActionResetPosition = handle(L4D2VROpenXrActionId::ResetPosition);
    m_ActionCrouch = handle(L4D2VROpenXrActionId::Crouch);
    m_ActionFlashlight = handle(L4D2VROpenXrActionId::Flashlight);
    m_ActionScoreboard = handle(L4D2VROpenXrActionId::Scoreboard);
    m_ActionPause = handle(L4D2VROpenXrActionId::Pause);
    m_ActionMenuSelect = handle(L4D2VROpenXrActionId::MenuSelect);
    m_ActionWeaponMenu = handle(L4D2VROpenXrActionId::InventoryQuickSwitch);
    m_ActionInventoryQuickSwitch = handle(L4D2VROpenXrActionId::InventoryQuickSwitch);
    // Helper has no Sprint action id. CustomAction1 is left-stick click (G2 sprint).
    m_ActionSprint = handle(L4D2VROpenXrActionId::CustomAction1);
    m_ActionCrouchToggle = handle(L4D2VROpenXrActionId::CustomAction2);
    m_ActionSkeletonLeft = handle(L4D2VROpenXrActionId::CustomAction3);
    m_ActionSkeletonRight = handle(L4D2VROpenXrActionId::CustomAction4);
    m_ActionsReady.store(true, std::memory_order_release);
}

bool VR::InitOpenXR()
{
    const OpenXrHelperLaunchConfig helperConfig = L4D2VR_ReadOpenXrHelperLaunchConfig();
    m_OpenXrSwapGameEyeOrigins = helperConfig.swapGameEyeOrigins;
    Game::logMsg("[VR][OpenXRHelper] enabled=%d submitTestFrames=%u waitReadySeconds=%u swapGameEyeOrigins=%d",
        helperConfig.enabled ? 1 : 0,
        helperConfig.submitTestFrames,
        helperConfig.waitReadySeconds,
        m_OpenXrSwapGameEyeOrigins ? 1 : 0);
    if (!helperConfig.enabled)
    {
        Game::logMsg("[VR][OpenXRHelper] OpenXR selected but OpenXRHelper=false in VR/config.txt");
        return false;
    }

    m_OpenXrHelperBridgeActive = L4D2VR_StartOpenXrHelper(helperConfig);
    bmvr::SetOpenXrHelperSession(m_OpenXrHelperBridgeActive);
    if (!m_OpenXrHelperBridgeActive)
    {
        Game::logMsg("[VR][OpenXRHelper] helper did not start");
        return false;
    }

    L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig{};
    uint32_t runtimeViewConfigGeneration = 0;
    const ULONGLONG runtimeViewStartMs = GetTickCount64();
    const ULONGLONG runtimeViewTimeoutMs = (std::max)(1000u, helperConfig.waitReadySeconds * 1000u);
    while (true)
    {
        const bool haveConfig = L4D2VR_ReadOpenXrRuntimeViewConfig(runtimeViewConfig, &runtimeViewConfigGeneration);
        if (haveConfig && runtimeViewConfig.reserved0 == L4D2VR_OPENXR_RUNTIME_VIEW_FOV_LOCATED)
            break;
        if (!L4D2VR_OpenXrHelperBridgeIsStarted())
        {
            Game::logMsg("[VR][OpenXRHelper] helper exited before runtime views");
            m_OpenXrHelperBridgeActive = false;
            bmvr::SetOpenXrHelperSession(false);
            return false;
        }
        if (GetTickCount64() - runtimeViewStartMs > runtimeViewTimeoutMs)
        {
            Game::logMsg("[VR][OpenXRHelper] runtime view projection was not published in time");
            m_OpenXrHelperBridgeActive = false;
            bmvr::SetOpenXrHelperSession(false);
            return false;
        }
        Sleep(10);
    }

    auto getOpenXrRawProjection = [](const L4D2VROpenXrRuntimeViewDesc& view,
        float& left, float& right, float& top, float& bottom) -> bool
    {
        if (!view.valid || view.width == 0 || view.height == 0)
            return false;
        left = std::tan(view.angleLeft);
        right = std::tan(view.angleRight);
        top = std::tan(view.angleDown);
        bottom = std::tan(view.angleUp);
        return std::isfinite(left) && std::isfinite(right) &&
            std::isfinite(top) && std::isfinite(bottom) &&
            left < -0.001f && right > 0.001f &&
            top < -0.001f && bottom > 0.001f;
    };

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    if (!getOpenXrRawProjection(runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT], l_left, l_right, l_top, l_bottom) ||
        !getOpenXrRawProjection(runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT], r_left, r_right, r_top, r_bottom))
    {
        Game::logMsg("[VR][OpenXRHelper] runtime view projection is invalid");
        m_OpenXrHelperBridgeActive = false;
        bmvr::SetOpenXrHelperSession(false);
        return false;
    }

    const float tanHalfFovX = (std::max)({ -l_left, l_right, -r_left, r_right });
    const float tanHalfFovY = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });
    if (!(std::isfinite(tanHalfFovX) && tanHalfFovX > 0.001f &&
        std::isfinite(tanHalfFovY) && tanHalfFovY > 0.001f))
    {
        Game::logMsg("[VR][OpenXRHelper] runtime view FOV is invalid");
        m_OpenXrHelperBridgeActive = false;
        bmvr::SetOpenXrHelperSession(false);
        return false;
    }

    m_RenderWidth = (std::max)(
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT].width,
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT].width);
    m_RenderHeight = (std::max)(
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT].height,
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT].height);
    bmvr::g_RecommendedEyeWidth = m_RenderWidth;
    bmvr::g_RecommendedEyeHeight = m_RenderHeight;

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFovX;
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFovX;
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFovY;
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFovY;
    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFovX;
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFovX;
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFovY;
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFovY;

    auto sanitizeTextureBounds = [](vr::VRTextureBounds_t& bounds)
    {
        bounds.uMin = std::clamp(bounds.uMin, 0.0f, 1.0f);
        bounds.uMax = std::clamp(bounds.uMax, 0.0f, 1.0f);
        bounds.vMin = std::clamp(bounds.vMin, 0.0f, 1.0f);
        bounds.vMax = std::clamp(bounds.vMax, 0.0f, 1.0f);
        if (bounds.uMax <= bounds.uMin || bounds.vMax <= bounds.vMin)
            bounds = vr::VRTextureBounds_t{ 0.0f, 0.0f, 1.0f, 1.0f };
    };
    sanitizeTextureBounds(m_TextureBounds[0]);
    sanitizeTextureBounds(m_TextureBounds[1]);

    m_Aspect = tanHalfFovX / tanHalfFovY;
    m_Fov = 2.0f * atanf(tanHalfFovX) * 180.0f / 3.14159265358979323846f;
    ChooseEyeRenderSize();
    BindOpenXrActionHandles();

    m_IsVREnabled = true;
    Game::logMsg(
        "[VR][OpenXR] helper scene backend ready runtimeViewGen=%u recommendedRT=%ux%u finalRT=%ux%u fov=%.3f aspect=%.6f boundsL=(%.4f %.4f %.4f %.4f) boundsR=(%.4f %.4f %.4f %.4f)",
        runtimeViewConfigGeneration,
        bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight,
        m_RenderWidth, m_RenderHeight, m_Fov, m_Aspect,
        m_TextureBounds[0].uMin, m_TextureBounds[0].vMin, m_TextureBounds[0].uMax, m_TextureBounds[0].vMax,
        m_TextureBounds[1].uMin, m_TextureBounds[1].vMin, m_TextureBounds[1].uMax, m_TextureBounds[1].vMax);
    return true;
}

namespace
{
    vr::TrackedDevicePose_t OpenXrPoseToTracked(
        const float* position, const float* orientation, bool valid)
    {
        vr::TrackedDevicePose_t pose{};
        if (!valid)
            return pose;
        float x = orientation[0];
        float y = orientation[1];
        float z = orientation[2];
        float w = orientation[3];
        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.0f / std::sqrt(lenSq);
            x *= invLen; y *= invLen; z *= invLen; w *= invLen;
        }
        else
        {
            x = 0.0f; y = 0.0f; z = 0.0f; w = 1.0f;
        }
        pose.bDeviceIsConnected = true;
        pose.bPoseIsValid = true;
        pose.eTrackingResult = vr::TrackingResult_Running_OK;
        vr::HmdMatrix34_t& mat = pose.mDeviceToAbsoluteTracking;
        mat.m[0][0] = 1.0f - 2.0f * y * y - 2.0f * z * z;
        mat.m[0][1] = 2.0f * x * y - 2.0f * z * w;
        mat.m[0][2] = 2.0f * x * z + 2.0f * y * w;
        mat.m[1][0] = 2.0f * x * y + 2.0f * z * w;
        mat.m[1][1] = 1.0f - 2.0f * x * x - 2.0f * z * z;
        mat.m[1][2] = 2.0f * y * z - 2.0f * x * w;
        mat.m[2][0] = 2.0f * x * z - 2.0f * y * w;
        mat.m[2][1] = 2.0f * y * z + 2.0f * x * w;
        mat.m[2][2] = 1.0f - 2.0f * x * x - 2.0f * y * y;
        mat.m[0][3] = position[0];
        mat.m[1][3] = position[1];
        mat.m[2][3] = position[2];
        return pose;
    }

    // Differences a position sample into a cached velocity. Leaves the previous
    // value untouched when the sample is unusable, so a bad dt cannot blank a
    // good velocity.
    void DiffLinearVelocity(
        const float* position,
        const float* prevPosition,
        double dtSeconds,
        float* velocity)
    {
        if (!position || !prevPosition || !velocity || dtSeconds < 0.0005 || dtSeconds > 0.25)
            return;
        const float dt = static_cast<float>(dtSeconds);
        velocity[0] = (position[0] - prevPosition[0]) / dt;
        velocity[1] = (position[1] - prevPosition[1]) / dt;
        velocity[2] = (position[2] - prevPosition[2]) / dt;
    }

    void ApplyLinearVelocity(vr::TrackedDevicePose_t& pose, const float* velocity)
    {
        if (!pose.bPoseIsValid || !velocity)
            return;
        pose.vVelocity.v[0] = velocity[0];
        pose.vVelocity.v[1] = velocity[1];
        pose.vVelocity.v[2] = velocity[2];
    }

    const L4D2VROpenXrControllerPoseDesc& PickOpenXrHandPose(
        const L4D2VROpenXrControllerPoseDesc& grip,
        const L4D2VROpenXrControllerPoseDesc& aim)
    {
        if (grip.valid && grip.active)
            return grip;
        return aim;
    }

    bool FillOpenXrOverlayTextureFromShared(
        L4D2VROpenXrOverlayDesc& overlay,
        const SharedTextureHolder& texture)
    {
        const uint32_t width = texture.m_VulkanData.m_nWidth;
        const uint32_t height = texture.m_VulkanData.m_nHeight;
        const uint32_t sampleCount = texture.m_VulkanData.m_nSampleCount;
        if (!texture.m_SharedHandleValid || texture.m_SharedHandle == 0 ||
            width == 0 || height == 0 || sampleCount != 1)
        {
            return false;
        }

        overlay.texture.valid = 1;
        overlay.texture.width = width;
        overlay.texture.height = height;
        overlay.texture.format = texture.m_VulkanData.m_nFormat;
        overlay.texture.sampleCount = sampleCount;
        overlay.texture.handleType = texture.m_SharedHandleType;
        overlay.texture.queueFamilyIndex = texture.m_VulkanData.m_nQueueFamilyIndex;
        overlay.texture.kmtHandle = texture.m_SharedHandle;
        overlay.texture.image = static_cast<uint64_t>(texture.m_VulkanData.m_nImage);
        overlay.texture.uMin = 0.0f;
        overlay.texture.vMin = 0.0f;
        overlay.texture.uMax = 1.0f;
        overlay.texture.vMax = 1.0f;
        overlay.texture.renderFovXDeg = 90.0f;
        overlay.texture.renderAspect = static_cast<float>(width) / static_cast<float>(height);
        return true;
    }
}

bool VR::ConsumeOpenXrTracking()
{
    if (!m_OpenXrHelperBridgeActive)
        return false;

    L4D2VROpenXrPoseDesc openXrPose{};
    uint32_t generation = 0;
    if (!L4D2VR_ReadOpenXrHmdPose(openXrPose, &generation))
        return false;

    m_OpenXrLastHmdPose = openXrPose;
    m_OpenXrLastHmdPoseGeneration = generation;
    if (openXrPose.reserved0 != 0)
    {
        float ipd = 0.f;
        std::memcpy(&ipd, &openXrPose.reserved0, sizeof(ipd));
        if (ipd >= 0.04f && ipd <= 0.10f)
            m_Ipd = ipd;
    }
    L4D2VR_ReadOpenXrInputState(m_OpenXrLastInputState, &m_OpenXrLastInputStateGeneration);

    const L4D2VROpenXrControllerPoseDesc& physicalLeft = PickOpenXrHandPose(
        m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_LEFT],
        m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_LEFT]);
    const L4D2VROpenXrControllerPoseDesc& physicalRight = PickOpenXrHandPose(
        m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_RIGHT],
        m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_RIGHT]);
    vr::TrackedDevicePose_t hmdPose = OpenXrPoseToTracked(
        openXrPose.position, openXrPose.orientation, openXrPose.valid != 0);
    vr::TrackedDevicePose_t leftPose = OpenXrPoseToTracked(
        physicalLeft.position, physicalLeft.orientation,
        physicalLeft.valid != 0 && physicalLeft.active != 0);
    vr::TrackedDevicePose_t rightPose = OpenXrPoseToTracked(
        physicalRight.position, physicalRight.orientation,
        physicalRight.valid != 0 && physicalRight.active != 0);

    // OpenXR controller poses carry no velocity, so it is differenced from
    // position. This runs several times per rendered frame
    // (WaitPosesForStereoFrame, BeginStereoFramePose, Update); the repeat calls
    // read the same bridge sample, so differencing them produced dt ~= 0 and a
    // zero velocity that stuck for the rest of the frame. Crowbar melee reads
    // that velocity, so swinging silently stopped registering. Only
    // re-difference when the bridge has actually published a new sample, and
    // reuse the last result on the repeat calls.
    // Head and hand samples carry independent generations, so each is
    // differenced against its own previous sample and timestamp. Keying both
    // off one counter would re-difference unchanged hand positions whenever
    // only the head pose advanced, which is what zeroed the swing velocity.
    static struct
    {
        bool haveHmd = false;
        LARGE_INTEGER hmdQpc{};
        uint32_t hmdGeneration = 0;
        float hmd[3]{};
        float velHmd[3]{};

        bool haveLeft = false;
        bool haveRight = false;
        LARGE_INTEGER handQpc{};
        uint32_t inputGeneration = 0;
        float left[3]{};
        float right[3]{};
        float velLeft[3]{};
        float velRight[3]{};
    } s_openXrVel;
    LARGE_INTEGER nowQpc{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&nowQpc);
    QueryPerformanceFrequency(&freq);
    auto elapsedSeconds = [&freq, &nowQpc](const LARGE_INTEGER& then) -> double {
        if (freq.QuadPart <= 0)
            return 0.0;
        return static_cast<double>(nowQpc.QuadPart - then.QuadPart) /
            static_cast<double>(freq.QuadPart);
    };

    if (generation != s_openXrVel.hmdGeneration)
    {
        if (s_openXrVel.haveHmd)
            DiffLinearVelocity(openXrPose.position, s_openXrVel.hmd,
                elapsedSeconds(s_openXrVel.hmdQpc), s_openXrVel.velHmd);
        if (openXrPose.valid)
        {
            s_openXrVel.hmd[0] = openXrPose.position[0];
            s_openXrVel.hmd[1] = openXrPose.position[1];
            s_openXrVel.hmd[2] = openXrPose.position[2];
            s_openXrVel.haveHmd = true;
            s_openXrVel.hmdQpc = nowQpc;
        }
        s_openXrVel.hmdGeneration = generation;
    }

    if (m_OpenXrLastInputStateGeneration != s_openXrVel.inputGeneration)
    {
        const double dt = elapsedSeconds(s_openXrVel.handQpc);
        if (s_openXrVel.haveLeft)
            DiffLinearVelocity(physicalLeft.position, s_openXrVel.left, dt, s_openXrVel.velLeft);
        if (s_openXrVel.haveRight)
            DiffLinearVelocity(physicalRight.position, s_openXrVel.right, dt, s_openXrVel.velRight);
        s_openXrVel.haveLeft = physicalLeft.valid && physicalLeft.active;
        if (s_openXrVel.haveLeft)
        {
            s_openXrVel.left[0] = physicalLeft.position[0];
            s_openXrVel.left[1] = physicalLeft.position[1];
            s_openXrVel.left[2] = physicalLeft.position[2];
        }
        s_openXrVel.haveRight = physicalRight.valid && physicalRight.active;
        if (s_openXrVel.haveRight)
        {
            s_openXrVel.right[0] = physicalRight.position[0];
            s_openXrVel.right[1] = physicalRight.position[1];
            s_openXrVel.right[2] = physicalRight.position[2];
        }
        s_openXrVel.handQpc = nowQpc;
        s_openXrVel.inputGeneration = m_OpenXrLastInputStateGeneration;
    }
    ApplyLinearVelocity(hmdPose, s_openXrVel.velHmd);
    if (s_openXrVel.haveLeft)
        ApplyLinearVelocity(leftPose, s_openXrVel.velLeft);
    if (s_openXrVel.haveRight)
        ApplyLinearVelocity(rightPose, s_openXrVel.velRight);

    if (bmvr::g_LeftHanded)
        std::swap(leftPose, rightPose);

    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        m_WaitedPoses[vr::k_unTrackedDeviceIndex_Hmd] = hmdPose;
        for (uint32_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
            m_WaitedPoses[i] = {};
        m_WaitedPoses[1] = leftPose;
        m_WaitedPoses[2] = rightPose;
    }
    m_WaitedPoseTick.store(GetTickCount(), std::memory_order_release);
    m_LastPoseWaitError.store(static_cast<int>(vr::VRCompositorError_None), std::memory_order_release);

    static bool s_logged;
    static bool s_loggedHands;
    if (!s_logged && openXrPose.valid)
    {
        s_logged = true;
        Game::logMsg("[VR][OpenXRHelper] consumed HMD pose gen=%u pos=(%.3f %.3f %.3f) L=%u R=%u",
            generation, openXrPose.position[0], openXrPose.position[1], openXrPose.position[2],
            physicalLeft.valid && physicalLeft.active ? 1u : 0u,
            physicalRight.valid && physicalRight.active ? 1u : 0u);
    }
    if (!s_loggedHands && (s_openXrVel.haveLeft || s_openXrVel.haveRight))
    {
        s_loggedHands = true;
        Game::logMsg("[VR][OpenXRHelper] controller poses L=%u R=%u velR=(%.2f %.2f %.2f)",
            s_openXrVel.haveLeft ? 1u : 0u,
            s_openXrVel.haveRight ? 1u : 0u,
            rightPose.vVelocity.v[0], rightPose.vVelocity.v[1], rightPose.vVelocity.v[2]);
    }
    return true;
}

bool VR::ShouldExportOpenXrEyeTexture(TextureID texID, uint32_t sampleCount) const
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return false;
    if (sampleCount != 1)
        return false;
    return texID == Texture_LeftEye ||
        texID == Texture_RightEye ||
        texID == Texture_LeftEyeSubmit ||
        texID == Texture_RightEyeSubmit ||
        texID == Texture_HUD ||
        texID == Texture_OpenXrPublish;
}

void VR::PublishOpenXrEyeTexture(TextureID texID, const D3D9_TEXTURE_VR_DESC& desc)
{
    if (!ShouldExportOpenXrEyeTexture(texID, desc.SampleCount))
        return;

    const bool isLeft = texID == Texture_LeftEye || texID == Texture_LeftEyeSubmit;
    const bool isRight = texID == Texture_RightEye || texID == Texture_RightEyeSubmit;
    if (!isLeft && !isRight)
        return;
    if (!desc.SharedHandleValid || desc.SharedHandle == 0)
    {
        Game::logMsg("[VR][OpenXRHelper] shared texture export missing texID=%d size=%ux%u",
            static_cast<int>(texID), desc.Width, desc.Height);
        return;
    }

    L4D2VROpenXrSharedTextureDesc shared{};
    shared.valid = 1;
    shared.width = desc.Width;
    shared.height = desc.Height;
    shared.format = static_cast<uint32_t>(desc.Format);
    shared.sampleCount = desc.SampleCount;
    shared.handleType = desc.SharedHandleType;
    shared.queueFamilyIndex = desc.QueueFamilyIndex;
    shared.kmtHandle = desc.SharedHandle;
    shared.image = desc.Image;
    const uint32_t eyeIndex = isLeft ? L4D2VR_OPENXR_EYE_LEFT : L4D2VR_OPENXR_EYE_RIGHT;
    const bool submitTexture = texID == Texture_LeftEyeSubmit || texID == Texture_RightEyeSubmit;
    // OpenXR helper contain-blits the full eye RT (hands/HUD/wheel are in-eye).
    // OpenVR-style UV bounds only crop overlay content away from the HMD image.
    const bool fullEyeBounds = submitTexture || m_OpenXrHelperBridgeActive;
    if (fullEyeBounds)
    {
        shared.uMin = 0.0f;
        shared.vMin = 0.0f;
        shared.uMax = 1.0f;
        shared.vMax = 1.0f;
        if (submitTexture)
        {
            shared.renderFovXDeg = 0.0f;
            shared.renderAspect = 0.0f;
        }
        else
        {
            shared.renderFovXDeg = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 90.0f;
            shared.renderAspect = (std::isfinite(m_Aspect) && m_Aspect > 0.1f && m_Aspect < 10.0f)
                ? m_Aspect
                : ((desc.Height > 0) ? (static_cast<float>(desc.Width) / static_cast<float>(desc.Height)) : 1.0f);
        }
    }
    else
    {
        shared.uMin = std::clamp(m_TextureBounds[eyeIndex].uMin, 0.0f, 1.0f);
        shared.vMin = std::clamp(m_TextureBounds[eyeIndex].vMin, 0.0f, 1.0f);
        shared.uMax = std::clamp(m_TextureBounds[eyeIndex].uMax, 0.0f, 1.0f);
        shared.vMax = std::clamp(m_TextureBounds[eyeIndex].vMax, 0.0f, 1.0f);
        shared.renderFovXDeg = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 90.0f;
        shared.renderAspect = (std::isfinite(m_Aspect) && m_Aspect > 0.1f && m_Aspect < 10.0f)
            ? m_Aspect
            : ((desc.Height > 0) ? (static_cast<float>(desc.Width) / static_cast<float>(desc.Height)) : 1.0f);
    }
    if (shared.uMax <= shared.uMin)
    {
        shared.uMin = 0.0f;
        shared.uMax = 1.0f;
    }
    if (shared.vMax <= shared.vMin)
    {
        shared.vMin = 0.0f;
        shared.vMax = 1.0f;
    }

    m_OpenXrSharedEyeTextures[eyeIndex] = shared;
    m_OpenXrSharedEyeTextureReadyMask.fetch_or(1u << eyeIndex, std::memory_order_acq_rel);
    Game::logMsg(
        "[VR][OpenXRHelper][GamePublishTexture] texID=%d eye=%s handle=0x%llX size=%ux%u fmt=%u bounds=(%.4f %.4f %.4f %.4f)",
        static_cast<int>(texID),
        eyeIndex == L4D2VR_OPENXR_EYE_LEFT ? "left" : "right",
        static_cast<unsigned long long>(shared.kmtHandle),
        shared.width, shared.height, shared.format,
        shared.uMin, shared.vMin, shared.uMax, shared.vMax);
}

void VR::PublishOpenXrResolvedEyeTextures(uint32_t frameId)
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted() || frameId == 0)
        return;
    if (m_OpenXrLastPublishedSharedTextureFrameId.load(std::memory_order_acquire) == frameId)
        return;
    const uint32_t readyMask = m_OpenXrSharedEyeTextureReadyMask.load(std::memory_order_acquire);
    if ((readyMask & L4D2VR_OPENXR_EYES_READY_MASK) != L4D2VR_OPENXR_EYES_READY_MASK)
        return;
    const L4D2VROpenXrSharedTextureDesc left = m_OpenXrSharedEyeTextures[L4D2VR_OPENXR_EYE_LEFT];
    const L4D2VROpenXrSharedTextureDesc right = m_OpenXrSharedEyeTextures[L4D2VR_OPENXR_EYE_RIGHT];
    if (!left.valid || !right.valid)
        return;
    L4D2VROpenXrSharedTextureDesc publishLeft = left;
    L4D2VROpenXrSharedTextureDesc publishRight = right;
    // Hand the helper the rotating publish copy, not the live engine eye RT.
    // Keep the FOV/aspect the eye export carries: the helper builds the submit
    // projection from renderFovXDeg/renderAspect.
    if (m_OpenXrPublishActive && m_OpenXrPublishReady)
    {
        const L4D2VROpenXrSharedTextureDesc& slotLeft =
            m_OpenXrPublishDesc[L4D2VR_OPENXR_EYE_LEFT][m_OpenXrPublishSlot];
        const L4D2VROpenXrSharedTextureDesc& slotRight =
            m_OpenXrPublishDesc[L4D2VR_OPENXR_EYE_RIGHT][m_OpenXrPublishSlot];
        if (slotLeft.valid && slotRight.valid)
        {
            const float leftFovX = publishLeft.renderFovXDeg;
            const float leftAspect = publishLeft.renderAspect;
            const float rightFovX = publishRight.renderFovXDeg;
            const float rightAspect = publishRight.renderAspect;
            publishLeft = slotLeft;
            publishRight = slotRight;
            publishLeft.renderFovXDeg = leftFovX;
            publishLeft.renderAspect = leftAspect;
            publishRight.renderFovXDeg = rightFovX;
            publishRight.renderAspect = rightAspect;
        }
    }
    // The helper derives the submitted vertical FOV from renderFovXDeg and
    // renderAspect. NormalizeViewSetupForVREye renders each eye at the eye RT's
    // own aspect (logged 3664x3584 -> 1.022), but the export carried m_Aspect
    // latched at texture creation (logged 1.0972), so the submitted frustum was
    // ~7% short vertically and the compositor reprojected through the wrong
    // projection. Derive it from the image actually being submitted.
    auto useImageAspect = [](L4D2VROpenXrSharedTextureDesc& desc) {
        if (desc.height == 0)
            return;
        const float aspect = static_cast<float>(desc.width) / static_cast<float>(desc.height);
        if (std::isfinite(aspect) && aspect > 0.1f && aspect < 10.0f)
            desc.renderAspect = aspect;
    };
    useImageAspect(publishLeft);
    useImageAspect(publishRight);
    // Hands/HUD/wheel are in-eye overlays; helper contain-blits the full RT.
    publishLeft.uMin = 0.0f;
    publishLeft.vMin = 0.0f;
    publishLeft.uMax = 1.0f;
    publishLeft.vMax = 1.0f;
    publishRight.uMin = 0.0f;
    publishRight.vMin = 0.0f;
    publishRight.uMax = 1.0f;
    publishRight.vMax = 1.0f;
    L4D2VR_PublishOpenXrSharedTexturePair(publishLeft, publishRight);
    L4D2VR_PublishOpenXrSharedTextureFrame(frameId);
    m_OpenXrLastPublishedSharedTextureFrameId.store(frameId, std::memory_order_release);
}

void VR::ReleaseOpenXrPublishTextures()
{
    for (uint32_t eye = 0; eye < L4D2VR_OPENXR_EYE_COUNT; ++eye)
    {
        for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
        {
            ReleaseT(m_D9OpenXrPublishSurface[eye][slot]);
            ReleaseT(m_D9OpenXrPublishTexture[eye][slot]);
            m_OpenXrPublishDesc[eye][slot] = L4D2VROpenXrSharedTextureDesc{};
        }
    }
    m_OpenXrPublishReady = false;
    m_OpenXrPublishActive = false;
    m_OpenXrPublishWidth = 0;
    m_OpenXrPublishHeight = 0;
    m_OpenXrPublishSlot = 0;
}

// Setup failure repeats every submit until the eye size changes, so an
// unthrottled log here writes to disk at the publish rate.
void VR::LogOpenXrPublishSetupFailure(const char* stage, uint32_t eye, uint32_t slot, unsigned hr)
{
    static uint32_t s_count = 0;
    static uint32_t s_lastTick = 0;
    const uint32_t now = GetTickCount();
    if (s_count >= 3 && (now - s_lastTick) < 10000)
    {
        ++s_count;
        return;
    }
    s_lastTick = now;
    ++s_count;
    Game::logMsg("OpenXR publish RT %s failed eye=%u slot=%u hr=0x%08X (occurrence %u; "
        "rotating publish disabled, helper reads the live eye RT)",
        stage, eye, slot, hr, s_count);
}

bool VR::EnsureOpenXrPublishTextures(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (m_OpenXrPublishReady && m_OpenXrPublishWidth == w && m_OpenXrPublishHeight == h)
        return true;
    ReleaseOpenXrPublishTextures();
    if (!device || !g_D3DVR9 || w < 64 || h < 64)
        return false;

    for (uint32_t eye = 0; eye < L4D2VR_OPENXR_EYE_COUNT; ++eye)
    {
        for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
        {
            IDirect3DTexture9** tex = &m_D9OpenXrPublishTexture[eye][slot];
            IDirect3DSurface9** surf = &m_D9OpenXrPublishSurface[eye][slot];
            // The DXVK fork only requests an exportable shared handle when
            // ShouldExportOpenXrEyeTexture accepts m_CreatingTextureID. Without
            // this the texture is created unshared, GetVRDesc has no handle, and
            // the whole rotating-publish path silently falls back to letting the
            // helper read the live engine eye RT (2026-08-31).
            m_CreatingTextureID = Texture_OpenXrPublish;
            const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
            m_CreatingTextureID = Texture_None;
            if (FAILED(hr) || !*tex || FAILED((*tex)->GetSurfaceLevel(0, surf)) || !*surf)
            {
                LogOpenXrPublishSetupFailure("create", eye, slot, (unsigned)hr);
                ReleaseOpenXrPublishTextures();
                return false;
            }
            D3D9_TEXTURE_VR_DESC vrDesc{};
            const HRESULT descHr = g_D3DVR9->GetVRDesc(*surf, &vrDesc);
            if (FAILED(descHr) || !vrDesc.Image
                || !vrDesc.SharedHandleValid || vrDesc.SharedHandle == 0)
            {
                LogOpenXrPublishSetupFailure("GetVRDesc", eye, slot, (unsigned)descHr);
                ReleaseOpenXrPublishTextures();
                return false;
            }
            L4D2VROpenXrSharedTextureDesc& shared = m_OpenXrPublishDesc[eye][slot];
            shared = L4D2VROpenXrSharedTextureDesc{};
            shared.valid = 1;
            shared.width = vrDesc.Width;
            shared.height = vrDesc.Height;
            shared.format = static_cast<uint32_t>(vrDesc.Format);
            shared.sampleCount = vrDesc.SampleCount;
            shared.handleType = vrDesc.SharedHandleType;
            shared.queueFamilyIndex = vrDesc.QueueFamilyIndex;
            shared.kmtHandle = vrDesc.SharedHandle;
            shared.image = vrDesc.Image;
            shared.uMin = 0.0f;
            shared.vMin = 0.0f;
            shared.uMax = 1.0f;
            shared.vMax = 1.0f;
        }
    }

    m_OpenXrPublishWidth = w;
    m_OpenXrPublishHeight = h;
    m_OpenXrPublishSlot = 0;
    m_OpenXrPublishReady = true;
    Game::logMsg("OpenXR publish RTs ready %ux%u slots=%u", w, h, kOpenXrPublishSlots);
    return true;
}

bool VR::PrepareOpenXrEyeSurfacesForRead()
{
    if (!m_OpenXrHelperBridgeActive || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    ResolveMsaaEyesToSubmit(device);
    IDirect3DSurface9* left = m_D9LeftEyeSubmitSurface ? m_D9LeftEyeSubmitSurface : m_D9LeftEyeSurface;
    IDirect3DSurface9* right = m_D9RightEyeSubmitSurface ? m_D9RightEyeSubmitSurface : m_D9RightEyeSurface;
    if (!left || !right)
    {
        device->Release();
        return false;
    }

    m_OpenXrPublishActive = false;
    D3DSURFACE_DESC eyeDesc{};
    if (SUCCEEDED(left->GetDesc(&eyeDesc))
        && EnsureOpenXrPublishTextures(device, eyeDesc.Width, eyeDesc.Height))
    {
        const uint32_t slot = (m_OpenXrPublishSlot + 1) % kOpenXrPublishSlots;
        IDirect3DSurface9* dstLeft = m_D9OpenXrPublishSurface[L4D2VR_OPENXR_EYE_LEFT][slot];
        IDirect3DSurface9* dstRight = m_D9OpenXrPublishSurface[L4D2VR_OPENXR_EYE_RIGHT][slot];
        if (dstLeft && dstRight
            && SUCCEEDED(device->StretchRect(left, nullptr, dstLeft, nullptr, D3DTEXF_NONE))
            && SUCCEEDED(device->StretchRect(right, nullptr, dstRight, nullptr, D3DTEXF_NONE)))
        {
            left = dstLeft;
            right = dstRight;
            m_OpenXrPublishSlot = slot;
            m_OpenXrPublishActive = true;
        }
    }
    device->Release();
    if (FAILED(g_D3DVR9->TransferSurface(left, FALSE)) ||
        FAILED(g_D3DVR9->TransferSurface(right, FALSE)))
        return false;
    return SUCCEEDED(g_D3DVR9->WaitDeviceIdle());
}

bool VR::PublishOpenXrHudOverlay(uint32_t frameId)
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return false;
    if (frameId == 0)
        return false;

    SharedTextureHolder hud{};
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        hud = m_VKHUD;
    }

    L4D2VROpenXrOverlayDesc overlay{};
    overlay.valid = 1;
    overlay.visible = 1;
    if (!FillOpenXrOverlayTextureFromShared(overlay, hud))
        return false;

    const bool pauseUi = PauseUiActive();
    overlay.widthMeters = (std::max)(0.10f, pauseUi ? 1.35f : bmvr::g_HudSize);
    overlay.heightMeters = overlay.widthMeters *
        (static_cast<float>(hud.m_VulkanData.m_nHeight) /
            static_cast<float>((std::max)(1u, hud.m_VulkanData.m_nWidth)));
    overlay.distanceMeters = (std::max)(0.10f, pauseUi ? 1.15f : bmvr::g_HudDistance);
    overlay.curvature = 0.0f;
    overlay.offsetMeters[0] = 0.0f;
    overlay.offsetMeters[1] = pauseUi ? -0.05f : -0.12f;
    overlay.offsetMeters[2] = 0.0f;

    L4D2VR_PublishOpenXrOverlay(L4D2VR_OPENXR_OVERLAY_HUD, overlay);
    L4D2VR_PublishOpenXrOverlayFrame(frameId);
    return true;
}

void VR::HideOpenXrHudOverlay()
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return;
    L4D2VROpenXrOverlayDesc overlay{};
    overlay.valid = 1;
    overlay.visible = 0;
    L4D2VR_PublishOpenXrOverlay(L4D2VR_OPENXR_OVERLAY_HUD, overlay);
}

void VR::SetActionManifest()
{
    m_ActionsReady.store(false, std::memory_order_release);
    if (!m_Input)
    {
        Game::logMsg("VRInput() null; motion controllers disabled");
        return;
    }

    char cwd[MAX_PATH]{};
    GetCurrentDirectoryA(MAX_PATH, cwd);

    wchar_t wexe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, wexe, MAX_PATH);
    const std::string exeDir = DirFromModulePath(wexe);

    wchar_t wmod[MAX_PATH]{};
    HMODULE mod = bmvr::DllModule();
    if (mod)
        GetModuleFileNameW(mod, wmod, MAX_PATH);
    const std::string modDir = wmod[0] ? DirFromModulePath(wmod) : std::string();

    std::vector<std::string> dirs;
    dirs.push_back(cwd);
    if (!exeDir.empty())
        dirs.push_back(exeDir);
    if (!modDir.empty())
        dirs.push_back(modDir);

    char path[MAX_PATH]{};
    bool found = false;
    for (const std::string& dir : dirs)
    {
        snprintf(path, sizeof(path), "%s\\VR\\SteamVRActionManifest\\action_manifest.json", dir.c_str());
        if (FileExistsA(path))
        {
            found = true;
            break;
        }
        snprintf(path, sizeof(path), "%s\\..\\VR\\SteamVRActionManifest\\action_manifest.json", dir.c_str());
        if (FileExistsA(path))
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        Game::logMsg("SteamVR action manifest missing (install VR\\SteamVRActionManifest next to bms.exe)");
        return;
    }
    char full[MAX_PATH]{};
    if (GetFullPathNameA(path, MAX_PATH, full, nullptr) && full[0])
        snprintf(path, sizeof(path), "%s", full);

    const vr::EVRInputError err = m_Input->SetActionManifestPath(path);
    Game::logMsg("SetActionManifestPath %s err=%d", path, (int)err);
    if (err != vr::VRInputError_None)
        return;

    auto grab = [this](const char* name, vr::VRActionHandle_t* handle) {
        const vr::EVRInputError e = m_Input->GetActionHandle(name, handle);
        if (e != vr::VRInputError_None)
            Game::logMsg("GetActionHandle %s err=%d", name, (int)e);
    };
    grab("/actions/main/in/Jump", &m_ActionJump);
    grab("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    grab("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    grab("/actions/main/in/Reload", &m_ActionReload);
    grab("/actions/main/in/Use", &m_ActionUse);
    grab("/actions/main/in/Walk", &m_ActionWalk);
    grab("/actions/main/in/Turn", &m_ActionTurn);
    grab("/actions/main/in/boolean_turnleft", &m_ActionBooleanTurnLeft);
    grab("/actions/main/in/boolean_turnright", &m_ActionBooleanTurnRight);
    grab("/actions/main/in/NextItem", &m_ActionNextItem);
    grab("/actions/main/in/PrevItem", &m_ActionPrevItem);
    grab("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    grab("/actions/main/in/Crouch", &m_ActionCrouch);
    grab("/actions/main/in/CrouchToggle", &m_ActionCrouchToggle);
    grab("/actions/main/in/Flashlight", &m_ActionFlashlight);
    grab("/actions/main/in/Scoreboard", &m_ActionScoreboard);
    grab("/actions/main/in/Pause", &m_ActionPause);
    grab("/actions/main/in/Sprint", &m_ActionSprint);
    grab("/actions/main/in/MenuSelect", &m_ActionMenuSelect);
    grab("/actions/main/in/WeaponMenu", &m_ActionWeaponMenu);
    grab("/actions/main/in/InventoryQuickSwitch", &m_ActionInventoryQuickSwitch);
    grab("/actions/base/in/skeleton_lefthand", &m_ActionSkeletonLeft);
    grab("/actions/base/in/skeleton_righthand", &m_ActionSkeletonRight);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_Input->GetActionSetHandle("/actions/base", &m_BaseActionSet);
    m_ActiveActionSets[0] = {};
    m_ActiveActionSets[0].ulActionSet = m_ActionSet;
    m_ActiveActionSets[1] = {};
    m_ActiveActionSets[1].ulActionSet = m_BaseActionSet;
    m_ActionsReady.store(true, std::memory_order_release);
    Game::logMsg("SteamVR actions ready (Walk/Turn/Use/Attack). G2 type hpmotioncontroller");
}

bool VR::GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const
{
    out = {};
    if (m_RuntimeBackend == VrRuntimeBackend::OpenXR)
    {
        const uint32_t actionIndex = static_cast<uint32_t>(handle);
        if (actionIndex == 0 || actionIndex >= L4D2VR_OPENXR_ACTION_COUNT)
            return false;
        const L4D2VROpenXrDigitalActionDesc& action = m_OpenXrLastInputState.digitalActions[actionIndex];
        if (!action.active)
            return false;
        out.bActive = true;
        out.activeOrigin = vr::k_ulInvalidInputValueHandle;
        out.bState = action.state != 0;
        out.bChanged = action.changed != 0;
        out.fUpdateTime = 0.0f;
        return true;
    }
    if (!m_Input || handle == vr::k_ulInvalidActionHandle)
        return false;
    const vr::EVRInputError result = m_Input->GetDigitalActionData(
        handle, &out, sizeof(out), vr::k_ulInvalidInputValueHandle);
    return result == vr::VRInputError_None;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const
{
    out = {};
    if (m_RuntimeBackend == VrRuntimeBackend::OpenXR)
    {
        const uint32_t actionIndex = static_cast<uint32_t>(handle);
        if (actionIndex == 0 || actionIndex >= L4D2VR_OPENXR_ACTION_COUNT)
            return false;
        const L4D2VROpenXrAnalogActionDesc& action = m_OpenXrLastInputState.analogActions[actionIndex];
        if (!action.active)
            return false;
        out.bActive = true;
        out.activeOrigin = vr::k_ulInvalidInputValueHandle;
        out.x = action.x;
        out.y = action.y;
        out.z = 0.0f;
        out.deltaX = 0.0f;
        out.deltaY = 0.0f;
        out.deltaZ = 0.0f;
        out.fUpdateTime = 0.0f;
        return true;
    }
    if (!m_Input || handle == vr::k_ulInvalidActionHandle)
        return false;
    const vr::EVRInputError result = m_Input->GetAnalogActionData(
        handle, &out, sizeof(out), vr::k_ulInvalidInputValueHandle);
    return result == vr::VRInputError_None;
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t handle, bool onChanged) const
{
    vr::InputDigitalActionData_t data{};
    if (!GetDigitalActionData(handle, data))
        return false;
    if (onChanged)
        return data.bState && data.bChanged;
    return data.bState;
}

void VR::ApplyTurnStick(float stickX, float deltaMs)
{
    float offset = m_RotationOffsetY.load(std::memory_order_relaxed);
    if (bmvr::g_SnapTurning)
    {
        if (!m_PressedTurn && stickX > 0.5f)
        {
            offset -= bmvr::g_SnapTurnAngle;
            m_PressedTurn = true;
            PulseAimHaptic(1800);
        }
        else if (!m_PressedTurn && stickX < -0.5f)
        {
            offset += bmvr::g_SnapTurnAngle;
            m_PressedTurn = true;
            PulseAimHaptic(1800);
        }
        else if (stickX < 0.3f && stickX > -0.3f)
            m_PressedTurn = false;
    }
    else
    {
        const float deadzone = 0.2f;
        const float a = fabsf(stickX);
        if (a > deadzone)
        {
            const float xNormalized = (a - deadzone) / (1.f - deadzone);
            if (stickX > deadzone)
                offset -= bmvr::g_TurnSpeed * deltaMs * xNormalized;
            else
                offset += bmvr::g_TurnSpeed * deltaMs * xNormalized;
        }
        else
            m_PressedTurn = false;
    }
    offset -= 360.f * floorf(offset / 360.f);
    m_RotationOffsetY.store(offset, std::memory_order_release);
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled || !m_ActionsReady.load(std::memory_order_acquire))
        return;
    if (m_RuntimeBackend != VrRuntimeBackend::OpenXR && !m_Input)
        return;

    static auto s_prev = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float deltaMs = std::chrono::duration<float, std::milli>(now - s_prev).count();
    s_prev = now;
    if (!(deltaMs > 0.f) || deltaMs > 250.f)
        deltaMs = 16.f;

    static bool s_next, s_prevItem, s_pause, s_reset;
    static bool s_atk, s_atk2, s_reload, s_use;
    static bool s_crouchAxis;
    if (!m_GameplayEligible)
    {
        m_ProcessInputEnabled = false;
        m_WalkForward.store(0.f, std::memory_order_release);
        m_WalkSide.store(0.f, std::memory_order_release);
        m_HeldButtons.store(0, std::memory_order_release);
        s_next = s_prevItem = s_pause = s_reset = false;
        s_atk = s_atk2 = s_reload = s_use = false;
        m_CrouchToggled = false;
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        if (m_Game)
        {
            m_Game->m_AnalogForward = 0.f;
            m_Game->m_AnalogSide = 0.f;
        }
        return;
    }

    m_ProcessInputEnabled = true;

    vr::InputAnalogActionData_t analog{};
    float walkX = 0.f, walkY = 0.f;
    if (GetAnalogActionData(m_ActionWalk, analog))
    {
        walkX = analog.x;
        walkY = analog.y;
    }
    auto dead = [](float v) {
        const float dz = 0.2f;
        const float a = fabsf(v);
        if (a <= dz)
            return 0.f;
        const float t = (a - dz) / (1.f - dz);
        return v < 0.f ? -t : t;
    };
    const float nx = dead(walkX);
    const float ny = dead(walkY);
    m_WalkSide.store(nx, std::memory_order_release);
    m_WalkForward.store(ny, std::memory_order_release);
    if (m_Game)
    {
        m_Game->m_AnalogSide = nx;
        m_Game->m_AnalogForward = ny;
    }

    const bool menuStick = WeaponMenuStickHeld();
    UpdateWeaponMenu(menuStick, deltaMs);

    bool usedAnalogTurn = false;
    float turnY = 0.f;
    bool haveTurnY = false;
    if (GetAnalogActionData(m_ActionTurn, analog))
    {
        if (!menuStick)
            ApplyTurnStick(analog.x, deltaMs);
        else
            ApplyTurnStick(0.f, deltaMs);
        usedAnalogTurn = true;
        if (!menuStick)
        {
            turnY = analog.y;
            haveTurnY = true;
        }
    }
    if (!usedAnalogTurn)
    {
        if (!menuStick && PressedDigitalAction(m_ActionBooleanTurnLeft))
            ApplyTurnStick(-1.f, deltaMs);
        else if (!menuStick && PressedDigitalAction(m_ActionBooleanTurnRight))
            ApplyTurnStick(1.f, deltaMs);
        else
            ApplyTurnStick(0.f, deltaMs);
    }

    const bool paused = SehIsPaused(m_Game->m_EngineClient);
    RefreshActiveWeaponModel();
    if (!paused && !m_WeaponMenuOpen)
        UpdateCrowbarMelee();
    if (!paused)
    {
        UpdateWeaponFireHaptics();
        if (m_PendingFireHaptic.exchange(0, std::memory_order_acq_rel))
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.85f);
    }

    uint32_t buttons = 0;
    if (!paused && !m_GameUiVisible)
    {
        if (!m_WeaponMenuOpen && !m_EmptyHands && PressedDigitalAction(m_ActionPrimaryAttack))
            buttons |= IN_ATTACK;
        if (!m_WeaponMenuOpen && !m_EmptyHands && PressedDigitalAction(m_ActionSecondaryAttack))
            buttons |= IN_ATTACK2;
        if (PressedDigitalAction(m_ActionJump) || (haveTurnY && turnY > 0.65f))
            buttons |= IN_JUMP;
        if (PressedDigitalAction(m_ActionUse))
            buttons |= IN_USE;
        if (PressedDigitalAction(m_ActionReload))
            buttons |= IN_RELOAD;
        if (PressedDigitalAction(m_ActionCrouch))
            buttons |= IN_DUCK;
        if (!haveTurnY || turnY > -0.4f)
        {
            if (PressedDigitalAction(m_ActionCrouchToggle, true))
                m_CrouchToggled = !m_CrouchToggled;
        }
        if (haveTurnY)
        {
            if (turnY < -0.65f)
            {
                if (!s_crouchAxis)
                    m_CrouchToggled = !m_CrouchToggled;
                s_crouchAxis = true;
            }
            else
                s_crouchAxis = false;
        }
        if (m_CrouchToggled)
            buttons |= IN_DUCK;
        if (PressedDigitalAction(m_ActionSprint))
            buttons |= IN_SPEED;
        if (GetTickCount() < m_MeleeAttackUntilMs)
            buttons |= IN_ATTACK;
        if (ny > 0.5f)
            buttons |= IN_FORWARD;
        else if (ny < -0.5f)
            buttons |= IN_BACK;
        if (nx > 0.5f)
            buttons |= IN_MOVERIGHT;
        else if (nx < -0.5f)
            buttons |= IN_MOVELEFT;
    }
    m_HeldButtons.store(buttons, std::memory_order_release);

    if ((buttons & IN_ATTACK) && !m_FirstAttackLogged)
    {
        m_FirstAttackLogged = true;
        m_FirstAttackPresentTick = m_PresentTick;
        m_FirstAttackSpikeLogs = 0;
        const char* wpn = m_LastViewmodelModel.empty() ? "?" : m_LastViewmodelModel.c_str();
        Game::logMsg("First IN_ATTACK this level weapon=%s bake=%d ox=%.2f oy=%.2f oz=%.2f scale=%.3f presentTick=%u",
            wpn, m_HasViewmodelBake ? 1 : 0, m_ViewmodelBakeOx, m_ViewmodelBakeOy, m_ViewmodelBakeOz,
            bmvr::g_ViewmodelScale, m_FirstAttackPresentTick);
    }

    const bool nextHeld = PressedDigitalAction(m_ActionNextItem);
    const bool prevHeld = PressedDigitalAction(m_ActionPrevItem);
    const bool pauseHeld = PressedDigitalAction(m_ActionPause);
    const bool resetHeld = PressedDigitalAction(m_ActionResetPosition);
    vr::InputDigitalActionData_t flash{};
    static bool s_flashLatched = false;
    static DWORD s_flashImpulseMs = 0;
    static int s_flashReleasePolls = 0;
    if (GetDigitalActionData(m_ActionFlashlight, flash))
    {
        const DWORD nowTick = GetTickCount();
        if (flash.bState)
        {
            s_flashReleasePolls = 0;
            if (!paused && !m_GameUiVisible && !s_flashLatched && (nowTick - s_flashImpulseMs) > 300)
            {
                m_PendingImpulse.store(100, std::memory_order_release);
                s_flashImpulseMs = nowTick;
                s_flashLatched = true;
                Game::logMsg("Flashlight queued impulse 100 (CreateMove only; vanilla ImpulseCommands) handle=%llu",
                    static_cast<unsigned long long>(m_ActionFlashlight));
            }
        }
        else if (++s_flashReleasePolls >= 3)
            s_flashLatched = false;
    }
    if (!paused && !menuStick && nextHeld && !s_next)
    {
        m_PendingInvDelta.store(1, std::memory_order_release);
        m_EmptyHands = false;
        Game::logMsg("NextItem queued on CreateMove (weaponselect, not invnext)");
    }
    if (!paused && !menuStick && prevHeld && !s_prevItem)
    {
        m_PendingInvDelta.store(-1, std::memory_order_release);
        m_EmptyHands = false;
        Game::logMsg("PrevItem queued on CreateMove (weaponselect, not invprev)");
    }
    if (pauseHeld && !s_pause)
    {
        Game::logMsg("Pause action edge paused=%d handle=%llu",
            paused ? 1 : 0, static_cast<unsigned long long>(m_ActionPause));
        QueueGameUiToggle(paused);
    }
    if (paused || m_GameUiVisible)
        ApplyMenuCursor();
    else
        m_MenuTriggerWasDown = false;
    // Recenter is a short tap of the same right-stick click that opens the
    // weapon menu. Do not recenter on press-edge while that click is held.
    if (resetHeld && !s_reset && !menuStick && !m_WeaponMenuOpenedThisHold)
    {
        m_HmdOriginLatched = false;
        if (bmvr::g_RecenterResetsYaw)
            m_RotationOffsetY.store(0.f, std::memory_order_release);
        Game::logMsg("ResetPosition: cleared HMD origin latch%s",
            bmvr::g_RecenterResetsYaw ? " + yaw" : "");
    }

    const bool atk = PressedDigitalAction(m_ActionPrimaryAttack);
    const bool atk2 = PressedDigitalAction(m_ActionSecondaryAttack);
    const bool reload = PressedDigitalAction(m_ActionReload);
    const bool use = PressedDigitalAction(m_ActionUse);
    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (atk && !s_atk)
    {
        if (!melee)
            m_WeaponActionAnimUntilMs = GetTickCount() + 450;
    }
    if (atk2 && !s_atk2)
    {
        if (!melee)
            PulseAimHaptic(1600);
        m_WeaponActionAnimUntilMs = GetTickCount() + 450;
    }
    if (reload && !s_reload)
    {
        PulseAimHaptic(1200);
        m_WeaponActionAnimUntilMs = GetTickCount() + 2800;
    }
    if (use && !s_use)
        PulseAimHaptic(900);
    if (nextHeld && !s_next)
        PulseAimHaptic(800);
    if (prevHeld && !s_prevItem)
        PulseAimHaptic(800);
    s_atk = atk;
    s_atk2 = atk2;
    s_reload = reload;
    s_use = use;
    s_next = nextHeld;
    s_prevItem = prevHeld;
    s_pause = pauseHeld;
    s_reset = resetHeld;

    static int s_inLog;
    if (s_inLog < 8 && (fabsf(nx) > 0.1f || fabsf(ny) > 0.1f || buttons != 0))
    {
        Game::logMsg("VR input walk=(%.2f,%.2f) buttons=0x%x turnOff=%.1f",
            ny, nx, buttons, m_RotationOffsetY.load(std::memory_order_relaxed));
        ++s_inLog;
    }

    UpdateViewmodelNumpadAdjust(paused || m_GameUiVisible);
}

void VR::ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds)
{
    const float tmp = bounds.vMin;
    bounds.vMin = bounds.vMax;
    bounds.vMax = tmp;
}

void VR::RefreshIpdFromHmd()
{
    if (!m_System)
        return;
    const vr::HmdMatrix34_t right = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    const float ipd = fabsf(right.m[0][3]) * 2.0f;
    if (ipd >= 0.04f && ipd <= 0.10f)
        m_Ipd = ipd;
    m_EyeZ = right.m[2][3];
}

bool VR::ResolveSurfaceSize(IDirect3DSurface9* surf, UINT& w, UINT& h, D3DSURFACE_DESC* outDesc)
{
    w = 0;
    h = 0;
    if (!surf)
        return false;
    D3DSURFACE_DESC desc{};
    if (FAILED(surf->GetDesc(&desc)))
        return false;
    if (outDesc)
        *outDesc = desc;
    w = desc.Width;
    h = desc.Height;
    return w >= 2 && h >= 2;
}

UINT VR::KnownWindowWidth() const
{
    UINT w = 0, h = 0;
    if (QueryGameClientSize(w, h))
        return w;
    if (m_VKBackBuffer.m_VulkanData.m_nWidth >= 640)
        return m_VKBackBuffer.m_VulkanData.m_nWidth;
    if (m_RenderWidth >= 640)
        return m_RenderWidth;
    return 1920;
}

UINT VR::KnownWindowHeight() const
{
    UINT w = 0, h = 0;
    if (QueryGameClientSize(w, h))
        return h;
    if (m_VKBackBuffer.m_VulkanData.m_nHeight >= 360)
        return m_VKBackBuffer.m_VulkanData.m_nHeight;
    if (m_RenderHeight >= 360)
        return m_RenderHeight;
    return 1080;
}

void VR::ChooseEyeRenderSize()
{
    PollSteamVrRecommendedSize();

    if (m_CreatedVRTextures.load(std::memory_order_acquire)
        && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth >= 640
        && m_VKLeftEye.m_VulkanData.m_nHeight >= 360)
    {
        const uint32_t recW = bmvr::g_RecommendedEyeWidth;
        const uint32_t recH = bmvr::g_RecommendedEyeHeight;
        const UINT haveW = m_VKLeftEye.m_VulkanData.m_nWidth;
        const UINT haveH = m_VKLeftEye.m_VulkanData.m_nHeight;
        const bool wantSteamVr = bmvr::TrySteamVrEyeRt() && recW >= 640 && recH >= 360
            && (haveW + 32 < recW || haveH + 32 < recH);
        uint32_t offW = 0, offH = 0;
        const bool haveOff = bmvr::ComputeOffscreenEyeSize(offW, offH);
        const bool wantOffscreenResize = haveOff
            && (haveW + 32 < offW || haveH + 32 < offH || offW + 32 < haveW || offH + 32 < haveH);
        if (wantOffscreenResize && m_EyeResizeSettleMs != 0
            && (GetTickCount() - m_EyeResizeSettleMs) < 300)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return;
        }
        uint32_t ffW = 0, ffH = 0;
        const bool wantFullFrame = bmvr::TryFullFrameStereo()
            && bmvr::g_FullFrameActualWidth >= 640 && bmvr::g_FullFrameActualHeight >= 360;
        if (wantFullFrame)
        {
            bmvr::FitHmdAspectInWindow(
                bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                m_Aspect, ffW, ffH);
            if (ffW + 32 > haveW || ffH + 32 > haveH)
            {
                // Fall through and recreate eyes at FullFrame-fitted size.
            }
            else if (!wantSteamVr && !wantOffscreenResize)
            {
                m_RenderWidth = haveW;
                m_RenderHeight = haveH;
                return;
            }
        }
        else if (!wantSteamVr && !wantOffscreenResize)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return;
        }
    }

    const uint32_t recW = bmvr::g_RecommendedEyeWidth;
    const uint32_t recH = bmvr::g_RecommendedEyeHeight;
    const UINT winWKnown = KnownWindowWidth();
    const UINT winHKnown = KnownWindowHeight();

    uint32_t offW = 0, offH = 0;
    if (bmvr::ComputeOffscreenEyeSize(offW, offH))
    {
        if (m_RenderWidth != offW || m_RenderHeight != offH)
        {
            Game::logMsg(
                "Eye RT %ux%u (offscreen rec=%ux%u RenderScale=%.2f window %ux%u)",
                offW, offH, recW, recH, bmvr::g_RenderScale, winWKnown, winHKnown);
        }
        m_RenderWidth = offW;
        m_RenderHeight = offH;
        return;
    }

    // steamvr_rt: offscreen eyes at GetRecommendedRenderTargetSize(). Verified
    // 2026-08-18: 3296x3216 > 2560x1440 G-buffer, SetRT redirect, skipped BB
    // blit, world black on desktop and HMD (audio + Escape menu still worked).
    // Only retry if the recommended size actually fits the window.
    if (bmvr::TrySteamVrEyeRt() && recW >= 640 && recH >= 360
        && recW <= winWKnown + 32 && recH <= winHKnown + 32)
    {
        float s = bmvr::g_RenderScale;
        if (!(s > 0.24f && s < 4.f))
            s = 1.f;
        if (s > 1.f)
            s = 1.f;
        uint32_t eyeW = (static_cast<uint32_t>(static_cast<float>(recW) * s + 0.5f) + 15u) & ~15u;
        uint32_t eyeH = (static_cast<uint32_t>(static_cast<float>(recH) * s + 0.5f) + 15u) & ~15u;
        if (eyeW < 640)
            eyeW = recW;
        if (eyeH < 360)
            eyeH = recH;
        m_RenderWidth = eyeW;
        m_RenderHeight = eyeH;
        Game::logMsg("Eye RT %ux%u (SteamVR recommended %ux%u RenderScale=%.2f, window %ux%u)",
            eyeW, eyeH, recW, recH, bmvr::g_RenderScale, winWKnown, winHKnown);
        return;
    }

    if (bmvr::TryFullFrameStereo()
        && bmvr::g_FullFrameActualWidth >= 640 && bmvr::g_FullFrameActualHeight >= 360)
    {
        float aspect = m_Aspect;
        if (!(aspect > 0.5f && aspect < 3.f) && recW >= 640 && recH >= 360)
            aspect = static_cast<float>(recW) / static_cast<float>(recH);
        uint32_t eyeW = 0, eyeH = 0;
        const uint32_t recAlignW = (recW + 15u) & ~15u;
        const uint32_t recAlignH = (recH + 15u) & ~15u;
        if (recAlignW >= 640 && recAlignH >= 360
            && recAlignW <= bmvr::g_FullFrameActualWidth
            && recAlignH <= bmvr::g_FullFrameActualHeight)
        {
            eyeW = recAlignW;
            eyeH = recAlignH;
        }
        else
        {
            bmvr::FitHmdAspectInWindow(
                bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                aspect, eyeW, eyeH);
        }
        float s = bmvr::g_RenderScale;
        if (!(s > 0.24f && s < 4.f))
            s = 1.f;
        if (s > 1.001f || s < 0.999f)
        {
            uint32_t scaledW = (static_cast<uint32_t>(static_cast<float>(eyeW) * s + 0.5f) + 15u) & ~15u;
            uint32_t scaledH = (static_cast<uint32_t>(static_cast<float>(eyeH) * s + 0.5f) + 15u) & ~15u;
            if (scaledW > bmvr::g_FullFrameActualWidth || scaledH > bmvr::g_FullFrameActualHeight)
                bmvr::FitHmdAspectInWindow(
                    bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                    aspect, scaledW, scaledH);
            if (scaledW >= 640 && scaledH >= 360)
            {
                eyeW = scaledW;
                eyeH = scaledH;
            }
        }
        if (eyeW >= 640 && eyeH >= 360)
        {
            if (m_RenderWidth != eyeW || m_RenderHeight != eyeH)
            {
                Game::logMsg(
                    "Resolution rec=%ux%u fullframe=%ux%u window=%ux%u eye=%ux%u RenderScale=%.2f ff_stereo=1",
                    recW, recH,
                    bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                    winWKnown, winHKnown, eyeW, eyeH, bmvr::g_RenderScale);
                if (eyeW > winWKnown + 32 || eyeH > winHKnown + 32)
                    bmvr::BeginRisky(L"ff_stereo");
            }
            m_RenderWidth = eyeW;
            m_RenderHeight = eyeH;
            return;
        }
    }

    uint32_t fbW = 0, fbH = 0;
    if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
    {
        if (m_RenderWidth != fbW || m_RenderHeight != fbH)
            Game::logMsg("Eye/G-buffer size %ux%u (CreateDevice HMD-aspect, window %ux%u recommended %ux%u aspect=%.3f)",
                fbW, fbH, KnownWindowWidth(), KnownWindowHeight(),
                recW, recH, m_Aspect);
        m_RenderWidth = fbW;
        m_RenderHeight = fbH;
        return;
    }

    uint32_t winW = KnownWindowWidth();
    uint32_t winH = KnownWindowHeight();
    if (winW < 640)
        winW = 1280;
    if (winH < 360)
        winH = 720;
    winW = (winW + 1u) & ~1u;
    winH = (winH + 1u) & ~1u;

    if (bmvr::TryHmdFramebuffer() && recW >= 640 && recH >= 360)
    {
        bmvr::ComputeHmdFramebufferSize(recW, recH, winW, winH, m_Aspect);
        if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
        {
            m_RenderWidth = fbW;
            m_RenderHeight = fbH;
            Game::logMsg("Eye/G-buffer size %ux%u (L4D2VR recommended %ux%u, window %ux%u native=%d aspect=%.3f)",
                fbW, fbH, recW, recH, winW, winH, bmvr::TryHmdNative() ? 1 : 0, m_Aspect);
            return;
        }
    }

    if (m_RenderWidth != winW || m_RenderHeight != winH)
        Game::logMsg("Eye RT size %ux%u (window, recommended %ux%u hmdAspect=%.3f)",
            winW, winH, recW, recH, m_Aspect);
    m_RenderWidth = winW;
    m_RenderHeight = winH;
}

Vector VR::GetViewAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const QAngle& ang = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    float yaw = ang.y + m_RotationOffsetY.load(std::memory_order_acquire);
    yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
    // L4D2VR GetViewAngle keeps HMD roll. Zeroing it here while OpenVR
    // Submit uses the full pose makes a head tilt rotate the world.
    return Vector(ang.x, yaw, ang.z);
}

void VR::GetViewBasis(Vector* forward, Vector* right, Vector* up) const
{
    const Vector va = GetViewAngle();
    QAngle ang(va.x, va.y, va.z);
    QAngle::AngleVectors(ang, forward, right, up);
}

Vector VR::GetViewOrigin(const Vector& setupOrigin) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    // Portal 2: player eye + HMD 6DOF. L4D2VR VectorPivotXY applies stick yaw
    // to the tracking delta so snap-turn does not leave room-scale offset in
    // unrotated playspace (rubberband). IPD/forward use GetViewAngle, not the
    // raw un-offset m_HmdRight from UpdateTracking.
    Vector center = setupOrigin;
    // L4D2VR: CameraAnchor.z = setup.z + HeightOffset, then
    // HmdPosAbs = CameraAnchor - (0,0,64) + tracking. ResetPosition sets
    // HeightOffset so the current pose sits at engine eye height (NPC level).
    // We cannot write that absolute HmdPosAbs onto live CViewSetup (abs_view).
    // Same result on a copy: engine eye + (tracking - pose at recenter).
    // Do not apply a recenter latched at the tracking origin (identity / headset
    // on the desk) — that stacks ~64u of standing height on top of setup.origin.
    if (m_HmdOriginLatched
        && (m_OpenXrHelperBridgeActive || m_HmdPosAbsZero.z > 24.f))
    {
        const Vector& hmdPos = m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs;
        Vector delta = hmdPos - m_HmdPosAbsZero;
        const float yaw = m_RotationOffsetY.load(std::memory_order_acquire);
        PivotYaw(delta, yaw);
        center += delta;
    }
    center.z += bmvr::g_HeightOffset;
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return center + (fwd * (-(m_EyeZ * m_VRScale)));
}

Vector VR::GetViewOriginLeft(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) - (right * ((m_Ipd * m_IpdScale * m_VRScale) * 0.5f));
}

Vector VR::GetViewOriginRight(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) + (right * ((m_Ipd * m_IpdScale * m_VRScale) * 0.5f));
}

Vector VR::ControllerTrackingToWorld(const Vector& setupOrigin, const Vector& trackingPos) const
{
    // Same world frame as stereo RenderView (GetViewOrigin + controller delta).
    Vector world = GetViewOrigin(setupOrigin);
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (!m_ControllerPoseValid)
        return world;
    Vector delta = trackingPos - m_HmdPosAbs;
    PivotYaw(delta, m_RotationOffsetY.load(std::memory_order_acquire));
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return world;
    if (VectorLength(delta) > 80.f)
        return world;
    return world + delta;
}

Vector VR::GetRightControllerAbsPos(const Vector& eyePosition) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return ControllerTrackingToWorld(eyePosition, m_RightControllerPosAbs);
}

QAngle VR::GetAimAngles() const
{
    // The one place the firing direction is defined. Both cmd->viewangles and
    // the shot origin projection read this, so they cannot drift apart.
    QAngle aim = GetRightControllerAbsAngle();
    // Trim the shot direction only. The viewmodel keeps its tuned pose, so a
    // grip that consistently lands low is corrected without rotating models.
    aim.x -= bmvr::g_AimPitchOffset;
    if (aim.x > 180.f) aim.x -= 360.f;
    if (aim.x < -180.f) aim.x += 360.f;
    if (aim.x > 89.f) aim.x = 89.f;
    if (aim.x < -89.f) aim.x = -89.f;
    aim.z = 0.f;
    return aim;
}

Vector VR::GetRecommendedViewmodelAbsPos(const Vector& eyePosition) const
{
    Vector base = eyePosition;
    Vector aimTracking{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_HasStereoBodyOrigin)
            base = m_StereoBodyOrigin;
        else if (m_SetupOrigin.LengthSqr() > 1.f)
            base = m_SetupOrigin;
        aimTracking = m_RightControllerPosAbs;
    }
    Vector p0 = ControllerTrackingToWorld(base, aimTracking);
    float ox = 0.f, oy = 0.f, oz = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    ResolveWeaponViewmodelPose(ox, oy, oz, ax, ay, az);
    static int s_poseLog;
    if (s_poseLog < 8)
    {
        Game::logMsg("Viewmodel pose p0=(%.1f,%.1f,%.1f) base=(%.1f,%.1f,%.1f) ox,oy,oz=(%.1f,%.1f,%.1f) bake=%d",
            p0.x, p0.y, p0.z, base.x, base.y, base.z, ox, oy, oz, m_HasViewmodelBake ? 1 : 0);
        ++s_poseLog;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    p0 -= m_ViewmodelForward * ox;
    p0 -= m_ViewmodelRight * oy;
    p0 -= m_ViewmodelUp * oz;
    return p0;
}

float VR::ViewmodelVisualScale() const
{
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    const char* modelName = model.c_str();
    float s = bmvr::g_ViewmodelScale;
    if (std::strstr(modelName, "crowbar") || std::strstr(modelName, "wrench")
        || std::strstr(modelName, "Crowbar") || std::strstr(modelName, "Wrench"))
        s = 1.f;
    if (std::strstr(modelName, "shotgun") || std::strstr(modelName, "spas")
        || std::strstr(modelName, "pump"))
        s = 0.64f;
    if (std::strstr(modelName, "grenade") || std::strstr(modelName, "Grenade")
        || std::strstr(modelName, "frag") || std::strstr(modelName, "Frag"))
        s *= 1.25f;
    if (std::strstr(modelName, "357") || std::strstr(modelName, "python")
        || std::strstr(modelName, "revolver") || std::strstr(modelName, "Revolver"))
        s *= 1.15f;
    if (s < 0.2f)
        s = 0.2f;
    if (s > 1.5f)
        s = 1.5f;
    return s;
}

void VR::ApplyViewmodelVisualScale(Vector& world) const
{
    const float scale = ViewmodelVisualScale();
    if (fabsf(scale - 1.f) <= 0.001f)
        return;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const Vector pivot = GetRightControllerAbsPos(body);
    world.x = pivot.x + (world.x - pivot.x) * scale;
    world.y = pivot.y + (world.y - pivot.y) * scale;
    world.z = pivot.z + (world.z - pivot.z) * scale;
}

bool VR::ScaleViewmodelRenderableAttachment(void* renderable, Vector& origin) const
{
    if (!renderable || !m_IsVREnabled || !m_ControllerPoseValid || !IsGameplayEligible())
        return false;
    if (!m_Game)
        return false;
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    if (!vm)
        return false;
    if (renderable != reinterpret_cast<unsigned char*>(vm) + 4)
        return false;
    ApplyViewmodelVisualScale(origin);
    return true;
}

bool VR::TryGetVrMuzzleWorld(Vector& origin) const
{
    if (!m_IsVREnabled || !m_ControllerPoseValid || !m_Game)
        return false;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return false;

    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    QAngle ang{};
    static const char* kNames[] = { "muzzle", "Fire01", "Fire02" };
    if (vm)
    {
        for (const char* name : kNames)
        {
            if (m_Game->GetEntityAttachment(vm, name, origin, ang) && origin.LengthSqr() > 1.f)
                return true;
        }
    }

    origin = GetRecommendedViewmodelAbsPos(body);
    ApplyViewmodelVisualScale(origin);
    const QAngle aim = GetRecommendedViewmodelAbsAngle();
    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    origin += fwd * (8.f * ViewmodelVisualScale());
    return origin.LengthSqr() > 1.f;
}

bool VR::TryGetVrShootOrigin(Vector& origin) const
{
    Vector muzzle{};
    if (!TryGetVrMuzzleWorld(muzzle))
        return false;

    // Bullets fly along cmd->viewangles, which is the controller aim, but they
    // start at the muzzle. The muzzle sits off that axis by the per-weapon
    // viewmodel offset and visual scale, so every shot lands a fixed distance
    // off target. It is worst on the revolver, whose 1.15x scale pushes the
    // muzzle furthest from the controller pivot. Slide the origin onto the aim
    // ray, keeping its distance along the barrel, so the shot goes where the
    // controller points regardless of how the model is posed.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const Vector aimOrigin = GetRightControllerAbsPos(body);
    if (aimOrigin.LengthSqr() <= 1.f)
    {
        origin = muzzle;
        return true;
    }
    const QAngle aim = GetAimAngles();
    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    if (VectorNormalize(fwd) <= 0.01f)
    {
        origin = muzzle;
        return true;
    }
    float along = (muzzle - aimOrigin).Dot(fwd);
    if (along < 0.f)
        along = 0.f;
    origin = aimOrigin + fwd * along;
    return true;
}

void VR::QueueWeaponMenuSound(uint32_t bit, int kind, int entityIndex)
{
    if (!bit)
        return;
    m_PendingWeaponSounds.fetch_or(bit, std::memory_order_acq_rel);
    if (bit & kWeaponSoundSelect)
    {
        m_PendingWeaponSoundKind.store(kind, std::memory_order_release);
        m_PendingWeaponSoundEntity.store(entityIndex, std::memory_order_release);
    }
}

void VR::FlushPendingWeaponSounds()
{
    const uint32_t bits = m_PendingWeaponSounds.exchange(0, std::memory_order_acq_rel);
    if (!bits || !m_Game)
        return;
    if (bits & kWeaponSoundHover)
        m_Game->PlayUiSound("common/wpn_moveselect.wav");
    if (bits & kWeaponSoundSelect)
        m_Game->PlayUiSound("common/wpn_hudoff.wav");
}

QAngle VR::GetRightControllerAbsAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return m_RightControllerAngAbs;
}

QAngle VR::GetRecommendedViewmodelAbsAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    QAngle result{};
    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);
    return result;
}

bool VR::ComputeHudInset(int fbW, int fbH, int& x, int& y, int& w, int& h) const
{
    // Overlay capture uses this to lay HUD out in the center of bmvrHUD
    // instead of the 2560x1440 texture edges (those were the "screen-edge"
    // elements the overlay transform could not move).
    if (fbW < 640 || fbH < 360)
        return false;
    float hmdFov = m_Fov;
    if (!(hmdFov > 20.f && hmdFov < 170.f))
        hmdFov = 98.5f;
    float hudFov = bmvr::g_HudMaxFov;
    if (!(hudFov >= 30.f && hudFov <= 90.f))
        hudFov = 60.f;
    float ratio = bmvr::g_HudDisplayRatio;
    if (!(ratio >= 0.4f && ratio <= 1.f))
        ratio = 0.82f;
    float s = (hudFov / hmdFov) * ratio;
    if (s > 0.9f)
        s = 0.9f;
    if (s < 0.4f)
        s = 0.4f;
    w = (static_cast<int>(static_cast<float>(fbW) * s) + 1) & ~1;
    h = (static_cast<int>(static_cast<float>(fbH) * s) + 1) & ~1;
    const int h16 = (w * 9) / 16;
    if (h16 >= 360 && h16 < h)
        h = h16;
    if (w < 640)
        w = 640;
    if (h < 360)
        h = 360;
    if (w > fbW)
        w = fbW;
    if (h > fbH)
        h = fbH;
    x = (fbW - w) / 2;
    y = (fbH - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    m_HudInsetX = x;
    m_HudInsetY = y;
    m_HudInsetW = w;
    m_HudInsetH = h;
    return true;
}

void VR::QueueEscapeKey()
{
    HWND hwnd = FindWindowA("Valve001", nullptr);
    if (!hwnd)
        hwnd = FindWindowA(nullptr, "Black Mesa");
    if (!hwnd)
        return;
    PostMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 1);
    PostMessageA(hwnd, WM_KEYUP, VK_ESCAPE, static_cast<LPARAM>(1u | (1u << 30) | (1u << 31)));
    Game::logMsg("Pause queued as WM_ESCAPE (gameui_activate skipped or sticky)");
}

void VR::QueueGameUiToggle(bool currentlyPaused)
{
    (void)currentlyPaused;
    const bool hide = m_GameUiVisible;
    m_PendingGameUi.store(hide ? 2 : 1, std::memory_order_release);
    Game::logMsg("Pause queued %s for engine thread (gameUiVisible=%d)",
        hide ? "gameui_hide" : "gameui_activate", hide ? 1 : 0);
}

void VR::FlushPendingGameUi()
{
    const int op = m_PendingGameUi.exchange(0, std::memory_order_acq_rel);
    if (op == 0 || !m_Game)
        return;
    const char* cmd = (op == 2) ? "gameui_hide" : "gameui_activate";
    bool ok = false;
    if (bmvr::TryGameUiActivate())
    {
        bmvr::BeginRisky(L"gameui");
        ok = m_Game->ClientCmd_Unrestricted(cmd);
        bmvr::EndRisky(L"gameui");
        Game::logMsg("Pause ClientCmd_Unrestricted(%s) engine-thread ok=%d (slot 108)",
            cmd, ok ? 1 : 0);
    }
    if (!ok)
    {
        ok = m_Game->ClientCmd(cmd);
        Game::logMsg("Pause ClientCmd(%s) slot 7 fallback ok=%d", cmd, ok ? 1 : 0);
    }
    if (ok)
        m_GameUiVisible = (op == 1);
}

bool VR::PauseUiActive() const
{
    if (m_GameUiVisible)
        return true;
    return m_Game && SehIsPaused(m_Game->m_EngineClient);
}

void VR::ApplyMenuCursor()
{
    if (!m_Game || !m_ControllerPoseValid || !m_HmdPoseValid)
        return;
    if (!GameWindowIsForeground())
    {
        static int s_unfocused;
        if (s_unfocused < 4)
        {
            Game::logMsg("VR cursor inject skipped (bms.exe not foreground)");
            ++s_unfocused;
        }
        return;
    }

    int vx = 0;
    int vy = 0;
    int hudW = 0;
    int hudH = 0;
    bool haveOverlayHit = false;
    if (m_HudOverlayReady && m_Overlay && m_HUDTopHandle != vr::k_ulOverlayHandleInvalid
        && m_Compositor && m_RightControllerTrackingValid)
    {
        vr::VROverlayIntersectionParams_t params{};
        vr::VROverlayIntersectionResults_t results{};
        params.eOrigin = m_Compositor->GetTrackingSpace();
        params.vSource = {
            m_RightControllerTracking.m[0][3],
            m_RightControllerTracking.m[1][3],
            m_RightControllerTracking.m[2][3]
        };
        params.vDirection = {
            -m_RightControllerTracking.m[0][2],
            -m_RightControllerTracking.m[1][2],
            -m_RightControllerTracking.m[2][2]
        };
        if (m_Overlay->ComputeOverlayIntersection(m_HUDTopHandle, &params, &results)
            == vr::VROverlayError_None)
        {
            hudW = m_HUDTexture ? m_HUDTexture->GetActualWidth() : static_cast<int>(KnownWindowWidth());
            hudH = m_HUDTexture ? m_HUDTexture->GetActualHeight() : static_cast<int>(KnownWindowHeight());
            if (hudW < 320)
                hudW = 1280;
            if (hudH < 180)
                hudH = 720;
            float u = results.vUVs.v[0];
            float v = results.vUVs.v[1];
            if (u < 0.f) u = 0.f;
            if (u > 1.f) u = 1.f;
            if (v < 0.f) v = 0.f;
            if (v > 1.f) v = 1.f;
            vx = static_cast<int>(u * static_cast<float>(hudW));
            vy = static_cast<int>(v * static_cast<float>(hudH));
            haveOverlayHit = true;
        }
        else
        {
            static int s_miss;
            if (s_miss < 4)
            {
                Game::logMsg("VR overlay intersection missed; not injecting 0,0 cursor");
                ++s_miss;
            }
        }
    }

    if (!haveOverlayHit)
        return;
    if (!m_GameUiVisible)
    {
        // Gameplay HUD overlay is not a mouse target. Never inject.
        return;
    }

    if (m_Game->m_VguiInput)
        SehSetCursorPos(m_Game->m_VguiInput, vx, vy);

    HWND hwnd = FindGameHwnd();
    const int wx = vx;
    const int wy = vy;
    if (hwnd)
        PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(wx, wy));

    const bool click = PressedDigitalAction(m_ActionPrimaryAttack)
        || PressedDigitalAction(m_ActionMenuSelect);
    if (click && !m_MenuTriggerWasDown && hwnd)
    {
        PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(wx, wy));
        PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(wx, wy));
        static int s_clickLog;
        if (s_clickLog < 8)
        {
            Game::logMsg("VR menu click at %d,%d overlayHit=1", wx, wy);
            ++s_clickLog;
        }
    }
    m_MenuTriggerWasDown = click;
}

void VR::UpdateCrowbarMelee()
{
    const bool attackWindow = GetTickCount() < m_MeleeAttackUntilMs;
    if (!m_ControllerPoseValid || !m_Game)
    {
        m_PerformingMelee = false;
        m_MeleeBladeAnglesValid = false;
        return;
    }

    QAngle prevAng{};
    QAngle curAng{};
    Vector curPos{};
    float speedMs = 0.f;
    bool crowbar = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        crowbar = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
        prevAng = m_PrevControllerAngAbs;
        curAng = m_RightControllerAngAbs;
        curPos = m_RightControllerPosAbs;
        speedMs = m_RightControllerSpeedMs;
    }
    if (!crowbar)
    {
        m_MeleeNewSwing = true;
        m_MeleeHitEntity = nullptr;
        m_PrevControllerAngAbs = curAng;
        m_PrevControllerPosAbs = curPos;
        m_PerformingMelee = false;
        m_MeleeBladeAnglesValid = false;
        return;
    }

    // L4D2VR used |vel| > 1.1 m/s with NewSwing reset on every drop below
    // that, which machine-gunned BM's IN_ATTACK pulse. Hysteresis plus the
    // cooldown below already stop the machine-gunning, so the on-threshold does
    // not also need to be high: 2.4 was set while OpenXR velocity was stuck at
    // zero, and it only reads as "swinging does nothing" now that velocity
    // works again.
    constexpr float kSwingOnMs = 1.5f;
    constexpr float kSwingOffMs = 0.8f;
    constexpr DWORD kSwingCooldownMs = 400;
    constexpr DWORD kAttackPulseMs = 120;
    const bool heldSwing = !m_MeleeNewSwing || attackWindow;
    const bool swinging = speedMs > (heldSwing ? kSwingOffMs : kSwingOnMs);

    // Peak speed while the crowbar is out, so a swing that never crosses the
    // threshold can still be told apart from velocity being dead.
    {
        static float s_peakSpeed = 0.f;
        static DWORD s_peakLogMs = 0;
        const DWORD nowMs = GetTickCount();
        s_peakSpeed = (std::max)(s_peakSpeed, speedMs);
        if (s_peakLogMs == 0)
            s_peakLogMs = nowMs;
        else if (nowMs - s_peakLogMs >= 5000)
        {
            Game::logMsg("Crowbar swing peak speed=%.2f m/s over 5s (on=%.2f off=%.2f)",
                s_peakSpeed, kSwingOnMs, kSwingOffMs);
            s_peakSpeed = 0.f;
            s_peakLogMs = nowMs;
        }
    }

    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    // L4D2VR: vanilla melee origin is the viewmodel abs origin, not the eye
    // and not a TraceRay rewrite to the controller tip.
    const Vector vmOrigin = GetRecommendedViewmodelAbsPos(body);
    m_MeleeTraceOrigin = vmOrigin;

    Vector bladeDir{};
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(curAng, &fwd, &right, &up);
        bladeDir = VectorRotate(fwd, right, 50.f);
        VectorNormalize(bladeDir);
    }
    QAngle bladeAng{};
    QAngle::VectorAngles(bladeDir, bladeAng);
    m_MeleeBladeAngles = bladeAng;
    m_MeleeBladeAnglesValid = swinging || attackWindow;

    if (!swinging)
    {
        m_MeleeNewSwing = true;
        m_MeleeHitEntity = nullptr;
        m_PrevControllerAngAbs = curAng;
        m_PrevControllerPosAbs = curPos;
        m_PerformingMelee = attackWindow;
        if (!m_PerformingMelee)
            m_MeleeBladeAnglesValid = false;
        return;
    }

    // Fan + IN_ATTACK only on the new-swing edge (not every ProcessInput tick —
    // 10 hulls/frame stuttered).
    if (m_MeleeNewSwing)
    {
        const DWORD now = GetTickCount();
        if (now < m_MeleeNextSwingMs)
        {
            m_PrevControllerAngAbs = curAng;
            m_PrevControllerPosAbs = curPos;
            m_PerformingMelee = attackWindow;
            if (!m_PerformingMelee)
                m_MeleeBladeAnglesValid = false;
            return;
        }
        m_MeleeNewSwing = false;
        m_MeleeHitEntity = nullptr;
        m_MeleeNextSwingMs = now + kSwingCooldownMs;
        m_MeleeAttackUntilMs = now + kAttackPulseMs;
        static int s_meleeLog;
        if (s_meleeLog < 12)
        {
            Game::logMsg("Crowbar swing IN_ATTACK speed=%.2f vmOrigin=(%.1f,%.1f,%.1f)",
                speedMs, vmOrigin.x, vmOrigin.y, vmOrigin.z);
            ++s_meleeLog;
        }

        if (m_Game->m_EngineTrace)
        {
            Vector initialForward, initialRight, initialUp;
            QAngle::AngleVectors(prevAng, &initialForward, &initialRight, &initialUp);
            Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.f);
            VectorNormalize(initialMeleeDirection);

            float swingDot = DotProduct(initialMeleeDirection, bladeDir);
            if (swingDot > 1.f) swingDot = 1.f;
            if (swingDot < -1.f) swingDot = -1.f;
            Vector pivot;
            CrossProduct(initialMeleeDirection, bladeDir, pivot);
            bool canTraceSwing = true;
            if (VectorNormalize(pivot) <= 0.0001f)
            {
                if (swingDot > -0.999f)
                    canTraceSwing = false;
                else
                {
                    pivot = initialUp;
                    canTraceSwing = VectorNormalize(pivot) > 0.0001f;
                }
            }
            float swingAngle = acosf(swingDot) * 180.f / 3.14159265f;
            if (!std::isfinite(swingAngle) || swingAngle <= 0.01f)
                canTraceSwing = false;

            if (canTraceSwing)
            {
                C_BaseEntity* player = nullptr;
                if (m_Game->m_EngineClient)
                {
                    const int lp = m_Game->m_EngineClient->GetLocalPlayer();
                    player = m_Game->GetClientEntity(lp);
                }
                CTraceFilterSkipSelf filter(player, 0);
                Vector traceDirection = initialMeleeDirection;
                const int numTraces = 10;
                const float traceAngle = swingAngle / static_cast<float>(numTraces);
                const float range = 56.f;
                const Vector hullMins(-16.f, -16.f, -16.f);
                const Vector hullMaxs(16.f, 16.f, 16.f);
                for (int i = 0; i < numTraces; ++i)
                {
                    traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
                    const Vector end = vmOrigin + traceDirection * range;
                    Ray_t ray;
                    ray.Init(vmOrigin, end, hullMins, hullMaxs);
                    CGameTrace tr{};
                    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT_HULL, &filter, &tr);
                    if (tr.fraction < 1.f && tr.m_pEnt && tr.m_pEnt != player && tr.m_pEnt != m_MeleeHitEntity)
                    {
                        m_MeleeHitEntity = tr.m_pEnt;
                        PulseAimHaptic(3999);
                        QAngle hitAng{};
                        QAngle::VectorAngles(traceDirection, hitAng);
                        m_MeleeBladeAngles = hitAng;
                        static int s_fanLog;
                        if (s_fanLog < 12)
                        {
                            Game::logMsg("Crowbar melee fan hit i=%d frac=%.2f vmOrigin=(%.1f,%.1f,%.1f)",
                                i, tr.fraction, vmOrigin.x, vmOrigin.y, vmOrigin.z);
                            ++s_fanLog;
                        }
                        break;
                    }
                }
            }
        }
    }

    m_PerformingMelee = true;
    m_PrevControllerAngAbs = curAng;
    m_PrevControllerPosAbs = curPos;
}

bool VR::TryGetMeleeBladeViewAngles(QAngle& out) const
{
    if (!m_PerformingMelee || !m_MeleeBladeAnglesValid)
        return false;
    out = m_MeleeBladeAngles;
    return true;
}

bool VR::TryGetMeleeTraceOrigin(Vector& origin) const
{
    if (!m_PerformingMelee)
        return false;
    origin = m_MeleeTraceOrigin;
    return origin.LengthSqr() > 1.f;
}

bool VR::WantsWeaponActionAnim() const
{
    // Manual VR crowbar swings must not play ACT_VM_HITCENTER on top of the
    // controller motion (implementation-plan §9).
    if (m_PerformingMelee)
        return false;
    if (GetTickCount() < m_WeaponActionAnimUntilMs)
        return true;
    const uint32_t buttons = m_HeldButtons.load(std::memory_order_acquire);
    return (buttons & (IN_ATTACK | IN_ATTACK2 | IN_RELOAD)) != 0
        && GetTickCount() >= m_MeleeAttackUntilMs;
}

void VR::UpdateControllerTracking(const vr::TrackedDevicePose_t& hmdPose)
{
    m_ControllerPoseValid = false;
    m_LeftControllerTrackingValid = false;
    m_PhysicalRightTrackingValid = false;
    m_RightControllerTrackingValid = false;
    if (!m_OpenXrHelperBridgeActive && !m_System)
        return;

    struct Sample
    {
        bool valid = false;
        vr::TrackedDeviceIndex_t idx = vr::k_unTrackedDeviceIndexInvalid;
        vr::TrackedDevicePose_t pose{};
        Vector pos{};
        QAngle ang{};
        Vector fwd{};
        Vector right{};
        Vector up{};
    };
    auto sampleIndex = [&](vr::TrackedDeviceIndex_t idx, Sample& out) {
        out.valid = false;
        out.idx = vr::k_unTrackedDeviceIndexInvalid;
        if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount)
            return;
        vr::TrackedDevicePose_t pose{};
        {
            std::lock_guard<std::mutex> lock(m_PoseMutex);
            pose = m_WaitedPoses[idx];
        }
        if (!pose.bPoseIsValid || !pose.bDeviceIsConnected)
            return;
        Vector pos = HmdMatrixToSourcePos(pose.mDeviceToAbsoluteTracking, m_VRScale);
        QAngle ang = HmdMatrixToSourceAnglesWithRoll(pose.mDeviceToAbsoluteTracking);
        if (!std::isfinite(ang.x) || !std::isfinite(ang.y) || !std::isfinite(ang.z))
            return;
        ang.y = WrapYaw(ang.y + m_RotationOffsetY.load(std::memory_order_acquire));
        Vector fwd, right, up;
        QAngle::AngleVectors(ang, &fwd, &right, &up);
        const float tilt = bmvr::g_ControllerPitchTilt;
        if (tilt != 0.f)
        {
            fwd = VectorRotate(fwd, right, tilt);
            up = VectorRotate(up, right, tilt);
        }
        QAngle ctrlAng{};
        QAngle::VectorAngles(fwd, up, ctrlAng);
        ctrlAng.y = WrapYaw(ctrlAng.y);
        out.valid = true;
        out.idx = idx;
        out.pose = pose;
        out.pos = pos;
        out.ang = ctrlAng;
        out.fwd = fwd;
        out.right = right;
        out.up = up;
    };
    auto sampleRole = [&](vr::ETrackedControllerRole role, Sample& out) {
        sampleIndex(m_System->GetTrackedDeviceIndexForControllerRole(role), out);
    };

    Sample physLeft{};
    Sample physRight{};
    if (m_OpenXrHelperBridgeActive)
    {
        sampleIndex(1, physLeft);
        sampleIndex(2, physRight);
    }
    else
    {
        sampleRole(vr::TrackedControllerRole_LeftHand, physLeft);
        sampleRole(vr::TrackedControllerRole_RightHand, physRight);
        if (!physLeft.valid || !physRight.valid)
        {
            for (uint32_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
            {
                if (m_System->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
                    continue;
                if (i == physLeft.idx || i == physRight.idx)
                    continue;
                Sample extra{};
                sampleIndex(i, extra);
                if (!extra.valid)
                    continue;
                const vr::ETrackedControllerRole role = m_System->GetControllerRoleForTrackedDeviceIndex(i);
                if (!physLeft.valid && (role == vr::TrackedControllerRole_LeftHand || role == vr::TrackedControllerRole_Invalid))
                    physLeft = extra;
                else if (!physRight.valid && (role == vr::TrackedControllerRole_RightHand || role == vr::TrackedControllerRole_Invalid))
                    physRight = extra;
                if (physLeft.valid && physRight.valid)
                    break;
            }
        }
    }
    const Sample& aim = (AimControllerRole() == vr::TrackedControllerRole_LeftHand) ? physLeft : physRight;
    if (!aim.valid)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    m_LeftControllerTrackingValid = physLeft.valid;
    m_LeftControllerDevice = physLeft.idx;
    if (physLeft.valid)
    {
        m_LeftControllerPosAbs = physLeft.pos;
        m_LeftControllerAngAbs = physLeft.ang;
        m_LeftControllerTracking = physLeft.pose.mDeviceToAbsoluteTracking;
    }
    m_PhysicalRightTrackingValid = physRight.valid;
    m_PhysicalRightDevice = physRight.idx;
    if (physRight.valid)
    {
        m_PhysicalRightPosAbs = physRight.pos;
        m_PhysicalRightAngAbs = physRight.ang;
        m_PhysicalRightTracking = physRight.pose.mDeviceToAbsoluteTracking;
    }

    m_RightControllerPosAbs = aim.pos;
    m_RightControllerAngAbs = aim.ang;
    m_RightControllerForward = aim.fwd;
    m_RightControllerRight = aim.right;
    m_RightControllerUp = aim.up;
    float vx = aim.pose.vVelocity.v[0];
    float vy = aim.pose.vVelocity.v[1];
    float vz = aim.pose.vVelocity.v[2];
    if (hmdPose.bPoseIsValid)
    {
        vx -= hmdPose.vVelocity.v[0];
        vy -= hmdPose.vVelocity.v[1];
        vz -= hmdPose.vVelocity.v[2];
    }
    m_RightControllerRelVel = Vector(vx, vy, vz);
    m_RightControllerSpeedMs = sqrtf(vx * vx + vy * vy + vz * vz);
    m_RightControllerTracking = aim.pose.mDeviceToAbsoluteTracking;
    m_RightControllerTrackingValid = true;
    if (hmdPose.bPoseIsValid)
    {
        const auto& hm = hmdPose.mDeviceToAbsoluteTracking.m;
        m_HmdTrackForward = Vector(-hm[0][2], -hm[1][2], -hm[2][2]);
    }

    m_ViewmodelForward = aim.fwd;
    m_ViewmodelRight = aim.right;
    m_ViewmodelUp = aim.up;
    ApplyViewmodelBasisOffsets();

    m_ControllerPoseValid = true;
    static int s_ctrlLog;
    if (s_ctrlLog < 4)
    {
        Game::logMsg("Controller tracking aim=(%.1f,%.1f,%.1f) left=%d right=%d tilt=%.1f",
            m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z,
            m_LeftControllerTrackingValid ? 1 : 0,
            m_PhysicalRightTrackingValid ? 1 : 0,
            bmvr::g_ControllerPitchTilt);
        ++s_ctrlLog;
    }
}

vr::ETrackedControllerRole VR::AimControllerRole() const
{
    return bmvr::g_LeftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
}

bool VR::GetFingerCurls(vr::VRActionHandle_t skeletonAction, float outCurls[5]) const
{
    if (!outCurls)
        return false;
    if (m_OpenXrHelperBridgeActive)
    {
        const uint32_t hand = (skeletonAction == m_ActionSkeletonRight)
            ? L4D2VR_OPENXR_HAND_RIGHT
            : L4D2VR_OPENXR_HAND_LEFT;
        const L4D2VROpenXrHandTrackingDesc& ht = m_OpenXrLastInputState.handTracking[hand];
        if (!ht.valid || !ht.active)
            return false;
        for (int i = 0; i < 5; ++i)
            outCurls[i] = std::clamp(ht.fingerCurls[i], 0.f, 1.f);
        return true;
    }
    if (!m_Input || skeletonAction == vr::k_ulInvalidActionHandle)
        return false;
    vr::VRSkeletalSummaryData_t summary{};
    const vr::EVRInputError err = m_Input->GetSkeletalSummaryData(
        skeletonAction, vr::VRSummaryType_FromAnimation, &summary);
    if (err != vr::VRInputError_None)
        return false;
    for (int i = 0; i < vr::VRFinger_Count; ++i)
        outCurls[i] = std::clamp(summary.flFingerCurl[i], 0.f, 1.f);
    return true;
}

void VR::TryCompositorPostPresentHandoff(DWORD nowMs, DWORD poseAgeMs)
{
    (void)poseAgeMs;
    if (!m_CompositorAppHandoff || !ShouldCompositorSubmit() || !m_Compositor)
        return;

    // Death-spiral fix (OpenCode 2026-08-24): combat GPU load made
    // PostPresentHandoff block 63-79ms every call (~4fps). Suspend app
    // timing after 5 slow calls; resume only after 10s AND ~300 fast presents.
    if (m_HandoffSuspended.load(std::memory_order_acquire))
    {
        if (nowMs >= m_HandoffResumeAtMs.load(std::memory_order_acquire)
            && m_HandoffFastFrames.load(std::memory_order_acquire) >= 300)
        {
            m_Compositor->SetExplicitTimingMode(
                vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
            m_HandoffSuspended.store(false, std::memory_order_release);
            m_HandoffSlowRun.store(0, std::memory_order_release);
            m_HandoffFastFrames.store(0, std::memory_order_release);
            Game::logMsg("Compositor: app handoff resumed (runtime probe)");
        }
        return;
    }

    const DWORD t0 = GetTickCount();
    m_Compositor->PostPresentHandoff();
    const DWORD dt = GetTickCount() - t0;
    if (dt >= 40)
    {
        const int run = m_HandoffSlowRun.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (run >= 5)
        {
            m_Compositor->SetExplicitTimingMode(
                vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
            m_HandoffSuspended.store(true, std::memory_order_release);
            m_HandoffResumeAtMs.store(nowMs + 10000, std::memory_order_release);
            m_HandoffSlowRun.store(0, std::memory_order_release);
            Game::logMsg("Compositor: app handoff suspended -> runtime timing (5 slow calls, last dt=%ums)", dt);
        }
        else if (dt >= 50)
        {
            Game::logMsg("Compositor PostPresentHandoff slow dt=%ums poseAge=%ums overshoot=%u (slow run %d/5)",
                dt, nowMs - m_WaitedPoseTick.load(std::memory_order_acquire),
                m_PoseWaitOvershootCount.load(std::memory_order_relaxed), run);
        }
        ++m_CompositorHandoffSlowCount;
    }
    else if (dt < 20)
    {
        m_HandoffSlowRun.store(0, std::memory_order_release);
    }
}

void VR::DrawIndependentHandMarkers(IDirect3DSurface9* eyeSurf, int stereoEye)
{
    if (!eyeSurf || !m_IsVREnabled || !IsGameplayEligible() || !m_HmdPoseValid || !g_D3DVR9)
        return;
    if (!(m_Fov > 10.f) || !m_HmdOriginLatched)
        return;

    bool leftOk = false;
    bool rightOk = false;
    QAngle leftAng{};
    QAngle rightAng{};
    Vector leftPos{};
    Vector rightPos{};
    Vector body{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_StereoFramePoseActive)
        {
            leftOk = m_StereoFrameLeftCtrlValid;
            rightOk = m_StereoFrameRightCtrlValid;
            leftAng = m_StereoFrameLeftCtrlAng;
            rightAng = m_StereoFrameRightCtrlAng;
            leftPos = m_StereoFrameLeftCtrlPos;
            rightPos = m_StereoFrameRightCtrlPos;
        }
        else
        {
            leftOk = m_LeftControllerTrackingValid;
            rightOk = m_PhysicalRightTrackingValid;
            leftAng = m_LeftControllerAngAbs;
            rightAng = m_PhysicalRightAngAbs;
            leftPos = m_LeftControllerPosAbs;
            rightPos = m_PhysicalRightPosAbs;
        }
        body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    }
    if (!leftOk && !rightOk)
        return;

    auto toWorld = [&](const Vector& tracking) {
        return ControllerTrackingToWorld(body, tracking);
    };

    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    const Vector eyeOrig = (stereoEye == 1)
        ? GetViewOriginLeft(body)
        : (stereoEye == 2) ? GetViewOriginRight(body) : GetViewOrigin(body);

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    if (bmvr::g_VrHandsGlovesEnabled)
        g_VrGloves.WarmupGpu(device);

    D3DSURFACE_DESC desc{};
    UINT w = 0, h = 0;
    if (FAILED(eyeSurf->GetDesc(&desc)) || desc.Width < 64 || desc.Height < 64)
    {
        device->Release();
        return;
    }
    w = desc.Width;
    h = desc.Height;
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float projFov = HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
    {
        device->Release();
        return;
    }

    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return sx > -40.f && sy > -40.f
            && sx < static_cast<float>(w) + 40.f && sy < static_cast<float>(h) + 40.f;
    };

    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    D3DVIEWPORT9 oldVp{};
    const bool haveOldVp = SUCCEEDED(device->GetViewport(&oldVp));
    device->GetRenderTarget(0, &oldRt);
    device->GetDepthStencilSurface(&oldDepth);
    device->SetDepthStencilSurface(nullptr);
    if (FAILED(device->SetRenderTarget(0, eyeSurf)))
    {
        if (oldRt)
            oldRt->Release();
        if (oldDepth)
            oldDepth->Release();
        device->Release();
        return;
    }

    IDirect3DSurface9* gloveDepth = nullptr;
    if (stereoEye == 1)
        gloveDepth = m_D9LeftEyeDepthSurface;
    else if (stereoEye == 2)
        gloveDepth = m_D9RightEyeDepthSurface;
    if (gloveDepth)
    {
        D3DSURFACE_DESC depthDesc{};
        if (FAILED(gloveDepth->GetDesc(&depthDesc))
            || depthDesc.MultiSampleType != desc.MultiSampleType
            || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height)
            gloveDepth = nullptr;
    }
    if (gloveDepth)
        device->SetDepthStencilSurface(gloveDepth);
    else if (stereoEye != 0)
        device->SetDepthStencilSurface(nullptr);

    D3DVIEWPORT9 eyeVp{};
    eyeVp.X = 0;
    eyeVp.Y = 0;
    eyeVp.Width = w;
    eyeVp.Height = h;
    eyeVp.MinZ = 0.f;
    eyeVp.MaxZ = 1.f;
    device->SetViewport(&eyeVp);
    if (gloveDepth)
        device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.f, 0);

    bool drewGloves = false;
    if (bmvr::g_VrHandsGlovesEnabled)
    {
        const Vector viewAngles = GetViewAngle();
        drewGloves = g_VrGloves.DrawForEye(
            device,
            stereoEye,
            eyeOrig,
            viewAngles,
            projFov,
            aspect,
            m_VRScale,
            bmvr::g_VrHandsModelScale,
            m_Input,
            m_ActionSkeletonLeft,
            m_ActionSkeletonRight,
            leftOk,
            toWorld(leftPos),
            leftAng,
            rightOk,
            toWorld(rightPos),
            rightAng);
    }

    const bool drawBoxes = bmvr::g_VrHandsDebugBoxes;
    if (bmvr::g_VrHandsGlovesEnabled && !drewGloves)
    {
        static int s_gloveFallbackLog;
        if (s_gloveFallbackLog < 3)
        {
            Game::logMsg("VR gloves fallback to debug boxes: %s",
                g_VrGloves.FailureReason().empty() ? "no mesh this eye" : g_VrGloves.FailureReason().c_str());
            ++s_gloveFallbackLog;
        }
    }
    auto restoreTargets = [&]() {
        if (haveOldVp)
            device->SetViewport(&oldVp);
        if (oldRt)
        {
            device->SetRenderTarget(0, oldRt);
            oldRt->Release();
            oldRt = nullptr;
        }
        if (oldDepth)
        {
            device->SetDepthStencilSurface(oldDepth);
            oldDepth->Release();
            oldDepth = nullptr;
        }
        else
            device->SetDepthStencilSurface(nullptr);
    };
    if (!drawBoxes && !(bmvr::g_HandHud && m_Game) && !WeaponMenuOpen())
    {
        restoreTargets();
        device->Release();
        return;
    }

    device->SetDepthStencilSurface(nullptr);

    IDirect3DStateBlock9* saved = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &saved);
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    device->GetVertexShader(&oldVs);
    device->GetPixelShader(&oldPs);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    struct Vert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
    auto drawBox = [&](float sx, float sy, D3DCOLOR color, float half) {
        Vert v[4] = {
            { sx - half, sy - half, 0.f, 1.f, color },
            { sx + half, sy - half, 0.f, 1.f, color },
            { sx - half, sy + half, 0.f, 1.f, color },
            { sx + half, sy + half, 0.f, 1.f, color },
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
    };

    auto drawHandProxy = [&](const Vector& origin, const QAngle& ang, D3DCOLOR color, bool mirror,
        vr::VRActionHandle_t skeletonAction) {
        Vector hf, hr, hu;
        QAngle::AngleVectors(ang, &hf, &hr, &hu);
        float curls[5]{ 0.15f, 0.15f, 0.15f, 0.15f, 0.15f };
        GetFingerCurls(skeletonAction, curls);
        auto drawPart = [&](float lx, float ly, float lz, float half) {
            if (mirror)
                lx = -lx;
            const Vector w = origin + hr * lx + hu * ly - hf * lz;
            float px = 0.f, py = 0.f;
            if (project(w, px, py))
                drawBox(px, py, color, half);
        };
        // §4 v1: W = B at controller; finger stubs driven by OpenVR skeletal summary.
        drawPart(0.f, 0.f, 0.f, 14.f);
        const float thumbSign = mirror ? -1.f : 1.f;
        drawPart(thumbSign * (-2.f + curls[0] * 1.5f), -1.f, 3.f + curls[0] * 2.f, 9.f);
        drawPart(thumbSign * (2.f + curls[1] * 2.f), 0.f, 4.f + curls[1] * 3.f, 8.f);
        drawPart(thumbSign * (4.f + curls[2] * 2.f), 0.f, 6.f + curls[2] * 3.f, 8.f);
        drawPart(thumbSign * (5.f + curls[3] * 1.5f), 0.f, 5.f + curls[3] * 2.5f, 7.f);
        drawPart(thumbSign * (6.f + curls[4] * 1.f), 0.f, 3.f + curls[4] * 2.f, 6.f);
    };

    if (drawBoxes)
    {
        if (leftOk)
            drawHandProxy(toWorld(leftPos), leftAng, D3DCOLOR_XRGB(0, 255, 255), true, m_ActionSkeletonLeft);
        if (rightOk && (bmvr::g_VrHandsRightEnabled
            || (g_Game && g_Game->m_VR && g_Game->m_VR->WantsRightGloveVisible())))
            drawHandProxy(toWorld(rightPos), rightAng, D3DCOLOR_XRGB(255, 0, 255), false, m_ActionSkeletonRight);
    }

    if (bmvr::g_HandHud && m_Game)
    {
        Vector lhf, lhr, lhu, rhf, rhr, rhu;
        QAngle::AngleVectors(leftAng, &lhf, &lhr, &lhu);
        QAngle::AngleVectors(rightAng, &rhf, &rhr, &rhu);
        // Left: watch on the forearm, past the HEV gauntlet. After yaw 180
        // the fingers aim along controller forward; behind the wrist is
        // -forward. Right ammo sits beside the gun, not down the forearm:
        // controller -right is the inner / left side of the grip.
        constexpr float kLeftWristBehindHu = 7.f;
        constexpr float kLeftWristInnerHu = 2.f;
        constexpr float kRightHudLeftHu = 4.5f;
        constexpr float kRightHudBehindHu = 0.8f;
        constexpr float kRightHudDownHu = 1.2f;
        const Vector leftWrist = leftOk
            ? toWorld(leftPos) - lhf * kLeftWristBehindHu - lhu * kLeftWristInnerHu
            : Vector{};
        const Vector rightWrist = rightOk
            ? toWorld(rightPos) - rhr * kRightHudLeftHu - rhf * kRightHudBehindHu - rhu * kRightHudDownHu
            : Vector{};
        DrawHandHud(device, stereoEye, w, h,
            leftOk, leftWrist, rightOk, rightWrist, eyeOrig, fwd, right, up);
    }
    if (WeaponMenuOpen())
        DrawWeaponMenu(device, w, h, eyeOrig, fwd, right, up);

    static int s_handLog;
    if (s_handLog < 4)
    {
        Game::logMsg("Hand proxy eye=%d left=%d right=%d gloves=%d %ux%u fmt=%u",
            stereoEye, leftOk ? 1 : 0, rightOk ? 1 : 0, drewGloves ? 1 : 0, w, h, desc.Format);
        ++s_handLog;
    }

    if (oldVs)
    {
        device->SetVertexShader(oldVs);
        oldVs->Release();
    }
    else
        device->SetVertexShader(nullptr);
    if (oldPs)
    {
        device->SetPixelShader(oldPs);
        oldPs->Release();
    }
    else
        device->SetPixelShader(nullptr);
    if (saved)
    {
        saved->Apply();
        saved->Release();
    }
    restoreTargets();
    device->Release();
}

namespace
{
    // 5x7 glyphs, bit 4 = left column. 0-9 then A-Z. Digit 0 is slashed so
    // it does not read as O/8; 6/8/9 keep open counters.
    const uint8_t kHud5x7[36][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E },
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F },
        { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
        { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
        { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    };

    struct HudVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    void HudQuad(IDirect3DDevice9* device, float x, float y, float w, float h, D3DCOLOR color)
    {
        HudVert v[4] = {
            { x, y, 0.f, 1.f, color },
            { x + w, y, 0.f, 1.f, color },
            { x, y + h, 0.f, 1.f, color },
            { x + w, y + h, 0.f, 1.f, color }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(HudVert));
    }

    int HudGlyphIndex(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'Z')
            return 10 + (c - 'A');
        return -1;
    }

    void HudGlyph(IDirect3DDevice9* device, float x, float y, float cell, char c, D3DCOLOR color)
    {
        const int idx = HudGlyphIndex(c);
        if (idx < 0)
            return;
        const float fill = cell * 0.84f;
        for (int row = 0; row < 7; ++row)
        {
            const uint8_t bits = kHud5x7[idx][row];
            for (int col = 0; col < 5; ++col)
            {
                if (bits & (0x10 >> col))
                    HudQuad(device, x + col * cell, y + row * cell, fill, fill, color);
            }
        }
    }

    void HudNumber(IDirect3DDevice9* device, float xRight, float y, float cell, int value, int minDigits, D3DCOLOR color)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value < 0 ? 0 : value);
        const int len = static_cast<int>(strlen(buf));
        const int digits = len < minDigits ? minDigits : len;
        float x = xRight - digits * 6.f * cell;
        for (int pad = len; pad < digits; ++pad)
        {
            HudGlyph(device, x, y, cell, '0', color);
            x += 6.f * cell;
        }
        for (int i = 0; i < len; ++i)
        {
            HudGlyph(device, x, y, cell, buf[i], color);
            x += 6.f * cell;
        }
    }

    // HEV style: the digit cells the value does not reach are filled with a dim
    // "0". Only the unused leading cells are drawn, so a bright digit never sits
    // on top of a dim one. A backing alpha of zero disables the field entirely.
    void HudNumberField(IDirect3DDevice9* device, float xRight, float y, float cell,
        int value, int fieldDigits, D3DCOLOR color, D3DCOLOR backing)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value < 0 ? 0 : value);
        const int len = static_cast<int>(strlen(buf));
        const float advance = 6.f * cell;
        const float x = xRight - fieldDigits * advance;
        if ((backing >> 24) != 0)
        {
            for (int i = 0; i < fieldDigits - len; ++i)
                HudGlyph(device, x + i * advance, y, cell, '0', backing);
        }
        HudNumber(device, xRight, y, cell, value, 1, color);
    }

    struct HudVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };

    // Same texture-stage dance the weapon wheel uses, restoring the untextured
    // diffuse setup the rest of the hand overlay draws with. The icon is fitted
    // inside the box rather than stretched to it: the source art is cropped to
    // its alpha bounds, so it is rarely square.
    void HudIcon(IDirect3DDevice9* device, IDirect3DTexture9* tex,
        float x, float y, float w, float h, D3DCOLOR tint)
    {
        if (!tex)
            return;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(tex->GetLevelDesc(0, &desc)) && desc.Width > 0 && desc.Height > 0)
        {
            const float srcAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            float fitW = w;
            float fitH = w / srcAspect;
            if (fitH > h)
            {
                fitH = h;
                fitW = h * srcAspect;
            }
            x += (w - fitW) * 0.5f;
            y += (h - fitH) * 0.5f;
            w = fitW;
            h = fitH;
        }
        device->SetTexture(0, tex);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        HudVertTex v[4] = {
            { x, y, 0.f, 1.f, tint, 0.f, 0.f },
            { x + w, y, 0.f, 1.f, tint, 1.f, 0.f },
            { x, y + h, 0.f, 1.f, tint, 0.f, 1.f },
            { x + w, y + h, 0.f, 1.f, tint, 1.f, 1.f }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(HudVertTex));
        device->SetTexture(0, nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
}

void VR::DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    bool leftOk, const Vector& leftWrist, bool rightOk, const Vector& rightWrist,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    (void)stereoEye;
    if (!m_Game || !device)
        return;
    int health = -1, armor = -1, clip = -1, reserve = -1, secondary = -1;
    if (!m_Game->ReadWristHudValues(health, armor, clip, reserve, secondary))
        return;
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float projFov = HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return sx > -80.f && sy > -80.f && sx < static_cast<float>(w) + 80.f && sy < static_cast<float>(h) + 80.f;
    };

    // The hand overlay draws opaque, but the wrist HUD needs real alpha: the
    // icons carry their shape in the alpha channel (white RGB), so without
    // blending they fill as solid blocks, and the dim backing digits would draw
    // at full strength. Restored before returning.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    const float s = static_cast<float>(h) / 1440.f * 1.44f;
    const float cell = 3.6f * s;
    const float smallCell = cell * 0.62f;
    const float fieldW = 3.f * 6.f * cell;
    const float smallFieldW = 3.f * 6.f * smallCell;
    const float glyphH = 7.f * cell;
    const float smallGlyphH = 7.f * smallCell;
    const float iconS = 30.f * s;
    const float smallIconS = iconS * 0.66f;
    const float gap = 7.f * s;

    const D3DCOLOR amber = D3DCOLOR_RGBA(255, 176, 0, 235);
    const D3DCOLOR amberDim = D3DCOLOR_RGBA(255, 176, 0, 170);
    const D3DCOLOR amberBack = D3DCOLOR_RGBA(255, 176, 0, 55);
    const D3DCOLOR red = D3DCOLOR_RGBA(255, 48, 32, 240);
    const D3DCOLOR redBack = D3DCOLOR_RGBA(255, 48, 32, 60);
    // Black Mesa's own HUD warns below 25 (scripts/hudlayout.res warnIfLessThan).
    constexpr int kLowHealth = 25;

    // One "000"-backed value with its icon to the right, centred on cx.
    auto drawValueIcon = [&](float cx, float yTop, float valueCell, float fieldWidth,
        float rowH, float size, int value, D3DCOLOR color, D3DCOLOR backing,
        IDirect3DTexture9* icon) {
        const float total = fieldWidth + gap + size;
        const float x0 = cx - total * 0.5f;
        HudNumberField(device, x0 + fieldWidth, yTop, valueCell, value, 3, color, backing);
        HudIcon(device, icon, x0 + fieldWidth + gap, yTop + (rowH - size) * 0.5f,
            size, size, color);
    };

    if (leftOk && health >= 0 && health <= 200)
    {
        float px = 0.f, py = 0.f;
        if (project(leftWrist, px, py))
        {
            const bool low = health <= kLowHealth;
            drawValueIcon(px, py - glyphH - gap * 0.5f, cell, fieldW, glyphH, iconS,
                health, low ? red : amber, low ? redBack : amberBack,
                bmvr::AcquireHudIcon(device, "hud_health_overlay.vtf"));
            if (armor >= 0 && armor <= 200)
                drawValueIcon(px, py + gap * 0.5f, cell, fieldW, glyphH, iconS,
                    armor, amber, amberBack,
                    bmvr::AcquireHudIcon(device, "hud_hev_overlay.vtf"));
        }
    }

    const bool hasAmmoHud = rightOk
        && ((clip >= 0 && clip <= 999) || (reserve >= 0 && reserve <= 999)
            || (secondary >= 0 && secondary <= 999));
    if (hasAmmoHud)
    {
        float px = 0.f, py = 0.f;
        if (project(rightWrist, px, py))
        {
            std::string weaponModel;
            {
                std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                weaponModel = m_LastViewmodelModel;
            }
            const char* model = weaponModel.c_str();
            IDirect3DTexture9* ammoIcon =
                bmvr::AcquireHudIcon(device, bmvr::PrimaryAmmoIconVtf(model, nullptr));

            const bool showClip = clip >= 0 && clip <= 999;
            const int primary = showClip ? clip : reserve;
            if (primary >= 0 && primary <= 999)
                drawValueIcon(px, py - glyphH - gap * 0.5f, cell, fieldW, glyphH, iconS,
                    primary, amber, amberBack, ammoIcon);

            // Reserve and secondary share the lower row at reduced size. Only
            // the big primary counter gets the dim "000" field; these two read
            // as clutter with it.
            const bool showRes = showClip && reserve >= 0 && reserve <= 999;
            const bool showSec = secondary >= 0 && secondary <= 999;
            const float cellW = smallFieldW + gap + smallIconS;
            float total = 0.f;
            if (showRes)
                total += cellW;
            if (showSec)
                total += (total > 0.f ? gap * 2.f : 0.f) + cellW;
            float cursor = px - total * 0.5f + cellW * 0.5f;
            const float y1 = py + gap * 0.5f;
            if (showRes)
            {
                drawValueIcon(cursor, y1, smallCell, smallFieldW, smallGlyphH, smallIconS,
                    reserve, amberDim, 0, ammoIcon);
                cursor += cellW + gap * 2.f;
            }
            if (showSec)
                drawValueIcon(cursor, y1, smallCell, smallFieldW, smallGlyphH, smallIconS,
                    secondary, amberDim, 0,
                    bmvr::AcquireHudIcon(device, bmvr::SecondaryAmmoIconVtf(model, nullptr)));
        }
    }

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void VR::DrawIndependentHandsOnDesktop()
{
    if (!(bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes))
        return;
    if (!g_D3DVR9)
        return;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }
    DrawIndependentHandMarkers(bb, 0);
    bb->Release();
    device->Release();
}

void VR::NoteViewmodelModel(const char* modelName)
{
    if (!modelName || !modelName[0])
        return;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (m_LastViewmodelModel != modelName)
    {
        m_LastViewmodelModel = modelName;
        Game::logMsg("Viewmodel model %s", modelName);
    }
}

void VR::NoteViewmodelWeaponBake(const char* modelName, const char* boneName, float restX, float restY, float restZ)
{
    (void)modelName;
    // studiohdr model space: +X forward, +Y left, +Z up (Source viewmodel).
    // Entity origin + rest places the weapon root; p0 - ox*f - oy*r - oz*u
    // compensates so the root sits on the aim controller (l4d2vr-weapons §2.3).
    const float ox = -restX;
    const float oy = -restY;
    const float oz = restZ;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const bool changed = !m_HasViewmodelBake
        || m_ViewmodelBakeOx != ox || m_ViewmodelBakeOy != oy || m_ViewmodelBakeOz != oz;
    m_ViewmodelBakeOx = ox;
    m_ViewmodelBakeOy = oy;
    m_ViewmodelBakeOz = oz;
    m_HasViewmodelBake = true;
    if (changed)
    {
        Game::logMsg("Viewmodel bake bone=%s rest=(%.2f,%.2f,%.2f) ox,oy,oz=(%.2f,%.2f,%.2f)",
            boneName ? boneName : "?", restX, restY, restZ, ox, oy, oz);
    }
}

void VR::RefreshActiveWeaponModel()
{
    if (!m_Game)
        return;
    const char* name = m_Game->GetActiveWeaponModelName();
    if (name && name[0])
        NoteViewmodelModel(name);
}

void VR::ApplyViewmodelBasisOffsets()
{
    float ox = 0.f, oy = 0.f, oz = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    ResolveWeaponViewmodelPose(ox, oy, oz, ax, ay, az);
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, ay);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, ay);
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, ax);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, ax);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, az);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, az);
    ApplyTwoHandShotgunAim();
}

void VR::ApplyTwoHandShotgunAim()
{
    // L4D2 ResolvePavlovTwoHandedAimBasis, shotgun/spas/pump only. No
    // virtual stock, no pistols, no m_hViewModel[1]. Left GLB stays on
    // the left controller matrix.
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    for (char& c : model)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const bool shotgun = model.find("shotgun") != std::string::npos
        || model.find("spas") != std::string::npos
        || model.find("pump") != std::string::npos;
    if (!shotgun || !m_LeftControllerTrackingValid || !m_RightControllerTrackingValid)
    {
        if (m_TwoHandShotgunActive)
            Game::logMsg("Two-hand shotgun off (weapon/tracking)");
        m_TwoHandShotgunActive = false;
        return;
    }

    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const Vector forend = m_RightControllerPosAbs + m_ViewmodelForward * (0.28f * scale);
    const float dist = (m_LeftControllerPosAbs - forend).Length();
    const float enterR = 0.18f * scale;
    const float stayR = 0.32f * scale;
    // Distance alone kept two-hand mode latched when the off-hand just dropped
    // to the player's side: the barrel then tracked the lowered hand and the
    // shotgun looked like it was lowering itself. Also require the off-hand to
    // stay ahead of the gun hand along the barrel.
    const float aheadHu = (m_LeftControllerPosAbs - m_RightControllerPosAbs).Dot(m_ViewmodelForward);
    const float enterAhead = 0.12f * scale;
    const float stayAhead = 0.06f * scale;
    const bool was = m_TwoHandShotgunActive;
    if (m_TwoHandShotgunActive)
        m_TwoHandShotgunActive = dist < stayR && aheadHu > stayAhead;
    else
        m_TwoHandShotgunActive = dist < enterR && aheadHu > enterAhead;
    if (m_TwoHandShotgunActive != was)
        Game::logMsg("Two-hand shotgun %s dist=%.1f hu ahead=%.1f enter=%.1f stay=%.1f",
            m_TwoHandShotgunActive ? "on" : "off", dist, aheadHu, enterR, stayR);
    if (!m_TwoHandShotgunActive)
        return;

    // L4D2: frontGrip = off-hand + 0.12 m along weapon forward.
    Vector front = m_LeftControllerPosAbs
        + m_ViewmodelForward * (0.12f * scale);
    Vector two = front - m_RightControllerPosAbs;
    if (VectorNormalize(two) <= 0.01f)
        return;
    const float strength = 0.85f;
    Vector blended = m_ViewmodelForward * (1.f - strength) + two * strength;
    if (VectorNormalize(blended) <= 0.01f)
        return;
    // A marginal off-hand position must not be able to swing the barrel far
    // from where the gun hand points. ~50 degrees.
    if (blended.Dot(m_ViewmodelForward) < 0.64f)
        return;
    m_ViewmodelForward = blended;
    m_ViewmodelRight = CrossProduct(m_ViewmodelForward, m_ViewmodelUp);
    if (VectorNormalize(m_ViewmodelRight) <= 0.01f)
        return;
    m_ViewmodelUp = CrossProduct(m_ViewmodelRight, m_ViewmodelForward);
    VectorNormalize(m_ViewmodelUp);
}

void VR::GetRightGlovePalmOffsetMeters(Vector& meters) const
{
    meters = Vector(0.f, 0.f, 0.f);
    if (m_EmptyHands)
        return;
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    if (model.empty())
        return;
    for (char& c : model)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return model.find(s) != std::string::npos; };
    if (has("crowbar") || has("wrench") || has("grenade") || has("frag")
        || has("satchel") || has("tripmine") || has("squeak") || has("snark"))
        return;
    // Controller local metres (BuildControllerWorld): +X right, +Y up,
    // +Z is -forward so negative Z moves the palm along aim onto the grip.
    // Starting table only — millimetre retune is a morning HMD pass.
    if (has("shotgun") || has("spas") || has("pump"))
        meters = Vector(0.f, -0.012f, -0.028f);
    else if (has("mp5") || has("smg") || has("mp5k"))
        meters = Vector(0.f, -0.010f, -0.022f);
    else if (has("357") || has("python") || has("revolver"))
        meters = Vector(0.f, -0.008f, -0.018f);
    else if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
        meters = Vector(0.f, -0.018f, -0.020f);
    else if (has("crossbow") || has("rpg") || has("rocket") || has("gauss")
        || has("tau") || has("egon") || has("gluon") || has("hornet") || has("hive")
        || has("hgun"))
        meters = Vector(0.f, -0.010f, -0.024f);
    else
        meters = Vector(0.f, -0.008f, -0.016f);
}

bool VR::WantsRightGloveVisible() const
{
    return bmvr::g_VrHandsRightEnabled || m_EmptyHands;
}

bool VR::WantsRightGloveWeaponGripCurl() const
{
    if (m_EmptyHands)
        return false;
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    if (model.empty())
        return false;
    for (char& c : model)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (model.find("crowbar") != std::string::npos || model.find("wrench") != std::string::npos)
        return false;
    if (model.find("grenade") != std::string::npos || model.find("frag") != std::string::npos
        || model.find("satchel") != std::string::npos || model.find("tripmine") != std::string::npos
        || model.find("squeak") != std::string::npos || model.find("snark") != std::string::npos)
        return false;
    return true;
}

namespace vm_numpad
{
    struct Offsets
    {
        float ox = 0.f;
        float oy = 0.f;
        float oz = 0.f;
        float ax = 0.f;
        float ay = 0.f;
        float az = 0.f;
    };

    std::map<std::string, Offsets> g_Off;
    bool g_Loaded = false;
    DWORD g_LastRepeatMs = 0;
    int g_LastVk = 0;

    std::wstring SavePath()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        return dir + L"\\VR\\viewmodel_offsets.txt";
    }

    std::string KeyFromModel(const std::string& model)
    {
        std::string m = model;
        for (char& c : m)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto has = [&](const char* s) { return m.find(s) != std::string::npos; };
        if (has("crowbar") || has("wrench")) return "crowbar";
        if (has("glock") || has("pistol") || has("9mm") || has("beretta")) return "glock";
        if (has("357") || has("python") || has("revolver")) return "357";
        if (has("mp5") || has("smg") || has("mp5k")) return "mp5";
        if (has("shotgun") || has("spas") || has("pump")) return "shotgun";
        if (has("crossbow")) return "crossbow";
        if (has("rpg") || has("rocket")) return "rpg";
        if (has("gauss") || has("tau")) return "gauss";
        if (has("egon") || has("gluon")) return "gluon";
        if (has("hgun") || has("hive") || has("hornet")) return "hgun";
        if (has("grenade") || has("frag")) return "grenade";
        if (has("satchel")) return "satchel";
        if (has("tripmine") || has("trip")) return "tripmine";
        if (has("squeak") || has("snark")) return "snark";
        if (m.empty()) return {};
        return "default";
    }

    void Load()
    {
        if (g_Loaded)
            return;
        g_Loaded = true;
        const std::wstring path = SavePath();
        std::ifstream in(path);
        if (!in)
            return;
        std::string line;
        int n = 0;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            char key[32]{};
            Offsets o{};
            if (sscanf_s(line.c_str(), "%31s %f %f %f %f %f %f",
                key, static_cast<unsigned>(sizeof(key)),
                &o.ox, &o.oy, &o.oz, &o.ax, &o.ay, &o.az) >= 4)
            {
                g_Off[key] = o;
                ++n;
            }
        }
        Game::logMsg("Viewmodel numpad offsets loaded=%d %ls", n, path.c_str());
    }

    void Save()
    {
        const std::wstring path = SavePath();
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            Game::logMsg("Viewmodel numpad save failed %ls", path.c_str());
            return;
        }
        out << "# Numpad (gameplay): 4/6 Y  8/2 Z  7/9 X. Hold Ctrl for angles.\n";
        out << "# Numpad 5 save, 0 reset this weapon, Shift = fine step.\n";
        out << "# key ox oy oz ax ay az  (added on top of the built-in table)\n";
        for (const auto& kv : g_Off)
        {
            out << kv.first << " "
                << kv.second.ox << " " << kv.second.oy << " " << kv.second.oz << " "
                << kv.second.ax << " " << kv.second.ay << " " << kv.second.az << "\n";
        }
        Game::logMsg("Viewmodel numpad saved %d weapons %ls", static_cast<int>(g_Off.size()), path.c_str());
    }

    void Apply(const std::string& model, float& ox, float& oy, float& oz, float& ax, float& ay, float& az)
    {
        Load();
        const std::string key = KeyFromModel(model);
        if (key.empty())
            return;
        auto it = g_Off.find(key);
        if (it == g_Off.end())
            return;
        ox += it->second.ox;
        oy += it->second.oy;
        oz += it->second.oz;
        ax += it->second.ax;
        ay += it->second.ay;
        az += it->second.az;
    }

    bool KeyHeld(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    bool RepeatEdge(int vk, DWORD now)
    {
        if (!KeyHeld(vk))
        {
            if (g_LastVk == vk)
                g_LastVk = 0;
            return false;
        }
        if (g_LastVk != vk)
        {
            g_LastVk = vk;
            g_LastRepeatMs = now;
            return true;
        }
        const DWORD delay = (now - g_LastRepeatMs > 400) ? 50u : 400u;
        if (now - g_LastRepeatMs >= delay)
        {
            g_LastRepeatMs = now;
            return true;
        }
        return false;
    }
}

void VR::UpdateViewmodelNumpadAdjust(bool paused)
{
    vm_numpad::Load();
    if (paused || !m_GameplayEligible)
        return;

    // Aim pitch trim is global, so it is tuned before the per-weapon key check
    // below bails out. Ctrl+Numpad+ shoots higher, Ctrl+Numpad- lower; the
    // logged value goes into AimPitchOffset in VR/config.txt to persist.
    {
        const DWORD nowMs = GetTickCount();
        const bool ctrlHeld = vm_numpad::KeyHeld(VK_CONTROL);
        const float step = vm_numpad::KeyHeld(VK_SHIFT) ? 0.1f : 0.5f;
        float delta = 0.f;
        if (ctrlHeld && vm_numpad::RepeatEdge(VK_ADD, nowMs))
            delta = step;
        else if (ctrlHeld && vm_numpad::RepeatEdge(VK_SUBTRACT, nowMs))
            delta = -step;
        if (delta != 0.f)
        {
            bmvr::g_AimPitchOffset += delta;
            Game::logMsg("Aim pitch trim %+.2f deg (put AimPitchOffset=%.2f in VR/config.txt)",
                delta, bmvr::g_AimPitchOffset);
        }
    }

    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    const std::string key = vm_numpad::KeyFromModel(model);
    if (key.empty())
        return;

    const DWORD now = GetTickCount();
    const bool ctrl = vm_numpad::KeyHeld(VK_CONTROL);
    const bool fine = vm_numpad::KeyHeld(VK_SHIFT);
    const float pstep = fine ? 0.15f : 0.5f;
    const float astep = fine ? 0.5f : 2.0f;
    vm_numpad::Offsets& o = vm_numpad::g_Off[key];
    bool changed = false;
    auto bump = [&](int vk, float* dst, float delta) {
        if (!vm_numpad::RepeatEdge(vk, now))
            return;
        *dst += delta;
        changed = true;
    };
    if (ctrl)
    {
        bump(VK_NUMPAD8, &o.ax, astep);
        bump(VK_UP, &o.ax, astep);
        bump(VK_NUMPAD2, &o.ax, -astep);
        bump(VK_DOWN, &o.ax, -astep);
        bump(VK_NUMPAD6, &o.ay, astep);
        bump(VK_RIGHT, &o.ay, astep);
        bump(VK_NUMPAD4, &o.ay, -astep);
        bump(VK_LEFT, &o.ay, -astep);
        bump(VK_NUMPAD9, &o.az, astep);
        bump(VK_PRIOR, &o.az, astep);
        bump(VK_NUMPAD7, &o.az, -astep);
        bump(VK_HOME, &o.az, -astep);
    }
    else
    {
        bump(VK_NUMPAD9, &o.ox, pstep);
        bump(VK_PRIOR, &o.ox, pstep);
        bump(VK_NUMPAD7, &o.ox, -pstep);
        bump(VK_HOME, &o.ox, -pstep);
        bump(VK_NUMPAD6, &o.oy, pstep);
        bump(VK_RIGHT, &o.oy, pstep);
        bump(VK_NUMPAD4, &o.oy, -pstep);
        bump(VK_LEFT, &o.oy, -pstep);
        bump(VK_NUMPAD8, &o.oz, pstep);
        bump(VK_UP, &o.oz, pstep);
        bump(VK_NUMPAD2, &o.oz, -pstep);
        bump(VK_DOWN, &o.oz, -pstep);
    }
    if (vm_numpad::RepeatEdge(VK_NUMPAD0, now) || vm_numpad::RepeatEdge(VK_INSERT, now))
    {
        o = {};
        changed = true;
        Game::logMsg("Viewmodel numpad reset %s", key.c_str());
    }
    static bool s_saveDown = false;
    const bool saveHeld = vm_numpad::KeyHeld(VK_NUMPAD5) || vm_numpad::KeyHeld(VK_CLEAR);
    if (saveHeld && !s_saveDown)
    {
        vm_numpad::Save();
        Game::logMsg("Viewmodel numpad %s extra pos=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f) saved",
            key.c_str(), o.ox, o.oy, o.oz, o.ax, o.ay, o.az);
    }
    s_saveDown = saveHeld;
    if (changed)
    {
        Game::logMsg("Viewmodel numpad %s extra pos=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f)%s",
            key.c_str(), o.ox, o.oy, o.oz, o.ax, o.ay, o.az, ctrl ? " [ang]" : "");
    }
}

void VR::ResolveWeaponViewmodelPose(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const
{
    ox = 0.f;
    oy = 0.f;
    oz = 0.f;
    ax = 0.f;
    ay = 0.f;
    az = 0.f;
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    if (!model.empty())
    {
        for (char& c : model)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto has = [&](const char* s) { return model.find(s) != std::string::npos; };
        // Plan §7: L4D2 empirical position tables (HMD-tuned starting point).
        // DME bake rest is logged via NoteViewmodelWeaponBake for pivot diagnosis
        // only — not 1:1 with L4D2 offsets (implementation-plan §7 evidence).
        // 2026-08-28 HMD-saved numpad extras baked into this table. Numpad
        // extras in VR/viewmodel_offsets.txt still add on top.
        if (has("crowbar") || has("wrench"))
        { ox = 15.5f; oy = 8.5f; oz = -12.f; }
        else if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
        { ox = 16.5f; oy = 4.f; oz = -7.f; }
        else if (has("357") || has("python") || has("revolver"))
        { ox = 10.f; oy = 3.5f; oz = -5.5f; }
        else if (has("mp5") || has("smg") || has("mp5k"))
        { ox = 9.f; oy = 4.5f; oz = -9.f; }
        else if (has("shotgun") || has("spas") || has("pump"))
        { ox = 9.f; oy = 8.f; oz = -9.5f; }
        else if (has("crossbow"))
        { ox = 12.5f; oy = 11.5f; oz = -13.f; }
        else if (has("rpg") || has("rocket"))
        { ox = 12.f; oy = 10.5f; oz = -6.5f; }
        else if (has("gauss") || has("tau"))
        { ox = 15.f; oy = 11.5f; oz = -16.5f; }
        else if (has("egon") || has("gluon"))
        { ox = 19.f; oy = 8.f; oz = -2.f; }
        else if (has("hgun") || has("hive") || has("hornet"))
        { ox = 19.5f; oy = 11.34f; oz = -17.69f; }
        else if (has("grenade") || has("frag"))
        { ox = 15.f; oy = 6.5f; oz = -8.f; }
        else if (has("satchel"))
        { ox = 19.f; oy = -10.f; oz = -12.f; }
        else if (has("tripmine") || has("trip"))
        { ox = 12.5f; oy = 4.5f; oz = -9.5f; }
        else if (has("squeak") || has("snark"))
        { ox = 15.f; oy = 7.5f; oz = -6.5f; }

        if (has("crowbar") || has("wrench"))
        { ax = -24.5f; ay = -6.5f; az = -6.f; }
        else if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
        { ax = -1.f; }
        else if (has("357") || has("python") || has("revolver"))
        { ax = -0.5f; }
        else if (has("mp5") || has("smg") || has("mp5k"))
        { ax = -0.5f; }
        else if (has("shotgun") || has("spas") || has("pump"))
        { ax = -0.5f; ay = -4.f; }
        else if (has("crossbow"))
        { ax = -4.5f; ay = -5.f; }
        else if (has("rpg") || has("rocket"))
        { ax = -1.f; }
        else if (has("gauss") || has("tau"))
        { ax = -1.5f; ay = -8.f; }
        else if (has("egon") || has("gluon"))
        { ax = -1.5f; ay = -2.f; }
        else if (has("hgun") || has("hive") || has("hornet"))
        { ax = -1.5f; ay = -2.f; }
    }
    ox += bmvr::g_ViewmodelPosOffsetX;
    oy += bmvr::g_ViewmodelPosOffsetY;
    oz += bmvr::g_ViewmodelPosOffsetZ;
    ax += bmvr::g_ViewmodelAngOffsetX;
    ay += bmvr::g_ViewmodelAngOffsetY;
    az += bmvr::g_ViewmodelAngOffsetZ;
    vm_numpad::Apply(model, ox, oy, oz, ax, ay, az);
}

void VR::PulseAimHaptic(unsigned short durationUs)
{
    PulseHandHaptic(AimControllerRole(), durationUs, 0.6f);
}

void VR::ApplyVrQualityOfLifeCvars()
{
    if (m_VrCvarsApplied || !m_Game)
        return;
    m_VrCvarsApplied = true;
    int n = 0;
    auto seti = [&](const char* name, int v) {
        if (m_Game->SetConVarInt(name, v))
            ++n;
    };
    auto setf = [&](const char* name, float v) {
        if (m_Game->SetConVarFloat(name, v))
            ++n;
    };
    if (bmvr::g_HideCrosshair)
        seti("crosshair", 0);
    setf("mat_grain_scale_override", 0.f);
    seti("engine_no_focus_sleep", 0);
    seti("mat_vsync", 0);
    seti("mat_triplebuffered", 0);
    seti("mat_motion_blur_enabled", 0);
    seti("mat_motion_blur_forward_enabled", 0);
    setf("mat_motion_blur_strength", 0.f);
    setf("mat_motion_blur_percent_of_screen_max", 0.f);
    setf("mat_motion_blur_rotation_intensity", 0.f);
    setf("mat_motion_blur_falling_intensity", 0.f);
    // fps_max 0: do not cap at HMD Hz (was 90 on G2). WaitGetPoses still
    // paces the compositor. MatchHmdHz no longer writes fps_max.
    if (m_Game->SetConVarInt("fps_max", 0) || m_Game->SetConVarFloat("fps_max", 0.f))
    {
        ++n;
        m_MenuFpsMaxLastHz = 0;
        Game::logMsg("fps_max 0 (uncapped; compositor syncs via WaitGetPoses)");
    }
    if (bmvr::g_DisableViewBob)
    {
        setf("cl_bob", 0.f);
        setf("cl_bobcycle", 0.f);
        setf("cl_bobup", 0.f);
        setf("cl_viewmodel_lag", 0.f);
        seti("r_jiggle_bones", 0);
    }
    // These gate only CBasePlayer::ViewPunch, the legacy m_vecPunchAngleVel
    // spring behind damage flinch and env_viewpunch. That angle is summed into
    // the server's shot direction, so suppressing it keeps a hit from throwing
    // aim off. It is not what makes MP5 fire climb: Black Mesa's weapon recoil
    // runs through AddRecoil, which reads no cvar at all. The climb is removed
    // in dGetShootAngles instead. Raw ConVar write; FCVAR_CHEAT does not block.
    seti("sv_suppress_viewpunch", 1);
    setf("sv_viewpunch_spring_constant", 0.f);
    // Stereo used to force r_occlusion 0 + r_portalsopenall 1 because two
    // RenderViews reused HW occlusion / areaportals and could latch empty vis.
    // That pair draws every leaf and tanks open/complex maps. Default: restore
    // PVS + occlusion. ForceOpenVis=true in config.txt brings the old path back.
    // r_visocclusion is a cheat/debug overlay, not HW occlusion. Default 1
    // was a mistaken "restore vis" write and can crash on Xen. r_occlusion
    // is the real query. Always leave visocclusion off.
    if (bmvr::g_ForceOpenVis)
    {
        seti("r_occlusion", 0);
        seti("r_fastzreject", 0);
        seti("r_visocclusion", 0);
        seti("r_portalsopenall", 1);
    }
    else
    {
        seti("r_occlusion", 1);
        seti("r_fastzreject", 1);
        seti("r_visocclusion", 0);
        seti("r_portalsopenall", 0);
    }
    // BM new-renderer CSM logged 8192x8192 (_rt_gbshadowmaprt) inside each
    // stereo RenderView. Lowest quality + 2048 res + one shadow pass.
    // Do not touch r_flashlightdepthtexture — that broke deferred flashlight.
    // Verified worse on bm_c2a5a (2026-08-28): cl_csm_enabled 0, nr_shadow_active
    // 0, nr_dev_gb_debug_type 1 (GBuffer_Fast), r_lod 2. Best session so far was
    // this block only (QoL 27 ok, csmQ=0). Undo the poisoned values so ARCHIVE
    // / leftover bmvr.cfg writes do not stick.
    seti("cl_csm_enabled", 1);
    seti("cl_csm_cascade_dynamic_ignore", 0);
    seti("nr_shadow_active", 1);
    seti("nr_dev_gb_debug_type", 2);
    seti("nr_lights_procedural_disable_all_lights", 0);
    seti("nr_dev_shoot_lights_enabled", 1);
    seti("gb_flashlight_shadow_enabled", 1);
    seti("cl_dlight_manager_enable", 1);
    seti("r_dynamic", 1);
    seti("r_maxdlights", 32);
    seti("r_lod", -1);
    seti("r_rootlod", 0);
    seti("r_eyes", 1);
    seti("r_teeth", 1);
    seti("flex_smooth", 1);
    seti("r_drawflecks", 1);
    seti("mat_reduceparticles", 0);
    seti("cl_new_impact_effects", 1);
    seti("nr_allow_hammer_nerfs", 0);
    seti("nr_allow_hammer_nerfs_4ways", 0);
    seti("nr_gbuffer_for_refraction_enabled", 1);
    seti("cl_csm_qualitymode", 0);
    seti("nr_shadow_quality", 0);
    seti("nr_shadow_filter_quality", 0);
    seti("nr_shadow_max_passes_per_frame", 1);
    seti("nr_shadow_res", 2048);
    seti("nr_lights_quality", 0);
    seti("r_shadowrendertotexture", 0);
    seti("r_shadowlod", 2);
    // Planar water used the HMD camera inside ViewDrawScene (not a nested
    // RenderView). Aux GetScreenSize skip was not enough. Cheap water instead
    // of a view-locked ghost world. BM new-renderer planar glass uses
    // nr_gbuffer_for_reflection_enabled (client.dll string 0x10454168).
    seti("r_WaterDrawReflection", 0);
    seti("r_waterforcereflectentities", 0);
    seti("r_waterforceexpensive", 0);
    seti("nr_gbuffer_for_reflection_enabled", 0);
    Game::logMsg("VR QoL cvars applied (%d ok) icvar=%p hideCrosshair=%d bobOff=%d forceOpenVis=%d blitFlush=%d csmQ=0 nrShadowRes=2048 waterrefl0",
        n, m_Game->m_Cvar, bmvr::g_HideCrosshair ? 1 : 0, bmvr::g_DisableViewBob ? 1 : 0,
        bmvr::g_ForceOpenVis ? 1 : 0, bmvr::g_StereoBlitGpuFlush ? 1 : 0);
}

void VR::TickMatQueueFromRenderView()
{
    if (m_Game)
        m_Game->ProbeMatQueueModeFromRenderView();
    ApplyVrQualityOfLifeCvars();
    // QoL SetValue(mat_vsync) crashed the first RenderView (2026-08-26).
    // Crosshair stays in bmvr.cfg. Only mat_queue_mode is written after warmup.
    UpdateAutoMatQueueMode();
}

void VR::UpdateAutoMatQueueMode()
{
    if (!m_IsVREnabled || !m_Game)
        return;

    const int current = m_Game->GetMatQueueMode();
    static int s_qlog;
    if (s_qlog < 8)
    {
        Game::logMsg("GetMatQueueMode=%d vtableOk=%d slots=%d auto=%d try=%d set=%d get=%d eq=%d",
            current, m_Game->MaterialVTableMatchesDump() ? 1 : 0,
            m_Game->MaterialThreadSlotsValid() ? 1 : 0,
            bmvr::g_AutoMatQueueMode ? 1 : 0, bmvr::TryMatQueue() ? 1 : 0,
            m_Game->m_MatSetThreadSlot, m_Game->m_MatGetThreadSlot,
            m_Game->m_MatExecuteQueuedSlot);
        ++s_qlog;
    }

    if (current == 2 && m_GameplayEligible)
    {
        ++m_MatQueueOkPresents;
        if (m_MatQueueOkPresents == 120)
            bmvr::EndRisky(L"mat_queue");
    }

    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    if (m_MenuFpsMaxLastHz != 0)
    {
        if (m_Game->SetConVarInt("fps_max", 0) || m_Game->SetConVarFloat("fps_max", 0.f))
        {
            m_MenuFpsMaxLastHz = 0;
            Game::logMsg("fps_max 0 (uncapped inGame=%d)", inGame ? 1 : 0);
        }
    }

    if (!bmvr::TryMatQueue())
        return;

    const bool paused = SehIsPaused(m_Game->m_EngineClient);
    bool hasLocalPlayer = false;
    if (inGame && m_Game->m_EngineClient)
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        hasLocalPlayer = playerIndex > 0 && m_Game->GetClientEntity(playerIndex) != nullptr;
    }
    const bool loadingMap = inGame && !hasLocalPlayer;

    int desired = 0;
    if (bmvr::g_AutoMatQueueMode)
    {
        const bool warmup = !PassThroughWarmupDone();
        desired = (!inGame || !m_GameplayEligible || paused || loadingMap || warmup) ? 0 : 2;
    }

    static bool s_forcedSingleThreadOnce;
    if (current == desired)
    {
        if (bmvr::g_AutoMatQueueMode || s_forcedSingleThreadOnce)
        {
            m_AutoMatQueueModeLastRequested = desired;
            return;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const float since = (m_AutoMatQueueModeLastCmdTime.time_since_epoch().count() == 0)
        ? 9999.f
        : std::chrono::duration<float>(now - m_AutoMatQueueModeLastCmdTime).count();
    const float retrySec = (desired == 2) ? 5.f : 0.5f;
    if (m_AutoMatQueueModeLastRequested == desired && since < retrySec)
        return;

    if (desired == 2 && m_AutoMatQueueModeLastRequested != 2)
        bmvr::BeginRisky(L"mat_queue");

    const bool ok = m_Game->SetMatQueueMode(desired);
    m_AutoMatQueueModeLastRequested = desired;
    m_AutoMatQueueModeLastCmdTime = now;
    if (desired == 0 && ok)
        s_forcedSingleThreadOnce = true;
    if (desired == 2 && !ok)
        bmvr::EndRisky(L"mat_queue");
    const char* reason = "in-game";
    if (!inGame) reason = "menu";
    else if (!m_GameplayEligible) reason = "loading";
    else if (loadingMap) reason = "no-local-player";
    else if (!PassThroughWarmupDone()) reason = "pass-through";
    else if (paused) reason = "paused";
    Game::logMsg("AutoMatQueueMode set %d (was %d) ok=%d reason=%s",
        desired, current, ok ? 1 : 0, reason);
}

void VR::TryApplySteamVrRecommendedEyeSize()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    vr::IVRSystem* sys = m_System ? m_System : vr::VRSystem();
    if (!sys)
        return;
    uint32_t recW = 0, recH = 0;
    sys->GetRecommendedRenderTargetSize(&recW, &recH);
    if (recW < 640 || recH < 360)
        return;
    if (recW > 4096)
        recW = 4096;
    if (recH > 4096)
        recH = 4096;
    if (recW == bmvr::g_RecommendedEyeWidth && recH == bmvr::g_RecommendedEyeHeight)
        return;
    const uint32_t prevW = bmvr::g_RecommendedEyeWidth;
    const uint32_t prevH = bmvr::g_RecommendedEyeHeight;
    bmvr::g_RecommendedEyeWidth = recW;
    bmvr::g_RecommendedEyeHeight = recH;
    Game::logMsg("SteamVR recommended RT %ux%u (was %ux%u)", recW, recH, prevW, prevH);
    if (prevW >= 640 && prevH >= 360
        && (recW + 32 < prevW || recH + 32 < prevH || prevW + 32 < recW || prevH + 32 < recH))
        m_EyeResizeSettleMs = GetTickCount();
}

void VR::PollSteamVrRecommendedSize()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    TryApplySteamVrRecommendedEyeSize();
}

void VR::ReclaimCompositorFocus(const char* reason)
{
    if (!m_Compositor)
        return;
    const DWORD now = GetTickCount();
    if (m_LastCompositorReclaimMs != 0 && (now - m_LastCompositorReclaimMs) < 1000)
        return;
    m_LastCompositorReclaimMs = now;
    m_Compositor->CompositorBringToFront();
    m_Compositor->SetExplicitTimingMode(m_CompositorAppHandoff
        ? vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff
        : vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
    if (m_HandoffSuspended.load(std::memory_order_acquire))
    {
        m_HandoffSuspended.store(false, std::memory_order_release);
        m_HandoffSlowRun.store(0, std::memory_order_release);
        m_HandoffFastFrames.store(0, std::memory_order_release);
    }
    m_Compositor->ForceInterleavedReprojectionOn(false);
    Game::logMsg("Compositor reclaim (%s) canRender=%d poseErr=%d",
        reason ? reason : "?",
        m_Compositor->CanRenderScene() ? 1 : 0,
        m_LastPoseWaitError.load(std::memory_order_acquire));
}

void VR::TickCompositorFocus()
{
    if (!m_Compositor)
        return;
    const bool can = m_Compositor->CanRenderScene();
    if (m_LastCanRenderScene && !can)
        Game::logMsg("Compositor lost scene focus (alt-tab / dashboard)");
    if (!m_LastCanRenderScene && can)
        ReclaimCompositorFocus("CanRenderScene restored");
    m_LastCanRenderScene = can;

    vr::EDeviceActivityLevel activity = vr::k_EDeviceActivityLevel_Unknown;
    if (m_System)
        activity = m_System->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
    const bool hmdActive = activity == vr::k_EDeviceActivityLevel_UserInteraction
        || activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
    const int poseErr = m_LastPoseWaitError.load(std::memory_order_acquire);
    if (hmdActive && (!can || poseErr == static_cast<int>(vr::VRCompositorError_DoNotHaveFocus)))
        ReclaimCompositorFocus("HMD active without scene focus");
}

float VR::HorizontalFovForAspect(float targetAspect) const
{
    if (!(m_Fov > 10.f) || !(m_Aspect > 0.1f) || !(targetAspect > 0.1f))
        return m_Fov;
    const float halfRad = m_Fov * 0.5f * (3.14159265358979323846f / 180.0f);
    const float tanHalfY = tanf(halfRad) / m_Aspect;
    return 2.0f * atanf(tanHalfY * targetAspect) * (180.0f / 3.14159265358979323846f);
}

void VR::BeginStereoFramePose()
{
    if (m_OpenXrHelperBridgeActive)
        ConsumeOpenXrTracking();
    UpdateTracking();
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_StereoFrameAngles = m_HmdAngAbs;
        m_StereoFrameHmdPosAbs = m_HmdPosAbs;
        m_StereoFrameLeftCtrlValid = m_LeftControllerTrackingValid;
        m_StereoFrameRightCtrlValid = m_PhysicalRightTrackingValid;
        m_StereoFrameLeftCtrlAng = m_LeftControllerAngAbs;
        m_StereoFrameRightCtrlAng = m_PhysicalRightAngAbs;
        m_StereoFrameLeftCtrlPos = m_LeftControllerPosAbs;
        m_StereoFrameRightCtrlPos = m_PhysicalRightPosAbs;
    }
    m_StereoFramePoseActive = true;
    if (m_OpenXrHelperBridgeActive)
    {
        m_OpenXrStereoRenderPose = m_OpenXrLastHmdPose;
        m_OpenXrStereoRenderPoseValid = m_OpenXrLastHmdPose.valid != 0;
        const float renderIpd = m_Ipd * m_IpdScale;
        if (renderIpd >= 0.04f && renderIpd <= 0.10f)
            std::memcpy(&m_OpenXrStereoRenderPose.reserved0, &renderIpd, sizeof(renderIpd));
        static int s_stereoPoseLog;
        if (s_stereoPoseLog < 4)
        {
            Game::logMsg("Stereo frame pose latch openxr=%d pos=(%.3f %.3f %.3f) ang=(%.1f %.1f)",
                m_OpenXrStereoRenderPoseValid ? 1 : 0,
                m_OpenXrStereoRenderPose.position[0],
                m_OpenXrStereoRenderPose.position[1],
                m_OpenXrStereoRenderPose.position[2],
                m_StereoFrameAngles.x, m_StereoFrameAngles.y);
            ++s_stereoPoseLog;
        }
    }
}

void VR::EndStereoFramePose()
{
    m_StereoFramePoseActive = false;
}

void VR::LogOpenXrPublishRate()
{
    const uint32_t now = GetTickCount();
    if (m_OpenXrPublishRateTick == 0)
    {
        m_OpenXrPublishRateTick = now;
        return;
    }
    const uint32_t dt = now - m_OpenXrPublishRateTick;
    if (dt < 5000)
        return;
    const float scale = 1000.f / static_cast<float>(dt);
    Game::logMsg("OpenXR publish rate present=%.0f/s published=%.0f/s skipStale=%.0f/s skipRate=%.0f/s slots=%u copy=%d",
        m_OpenXrSubmitAttempts * scale, m_OpenXrPublishes * scale,
        m_OpenXrSkippedNoNewFrame * scale, m_OpenXrSkippedHelperBusy * scale,
        kOpenXrPublishSlots, m_OpenXrPublishActive ? 1 : 0);
    m_OpenXrPublishRateTick = now;
    m_OpenXrSubmitAttempts = 0;
    m_OpenXrPublishes = 0;
    m_OpenXrSkippedNoNewFrame = 0;
    m_OpenXrSkippedHelperBusy = 0;
}

void VR::WaitPosesForStereoFrame()
{
    if (m_PosesWaitedThisFrame)
        return;
    if (m_OpenXrHelperBridgeActive)
    {
        ConsumeOpenXrTracking();
        UpdateTracking();
        // Game-render pose for OpenXR submit is latched in BeginStereoFramePose
        // when the stereo pair actually renders. An earlier main RenderView can
        // call WaitPoses tens of ms before the eyes, which desyncs reprojection.
        m_PosesWaitedThisFrame = true;
        return;
    }
    // NEVER call WaitGetPoses from the render thread: the pose-waiter thread
    // owns it. Concurrent WaitGetPoses races openvr_api compositor state
    // (verified OpenCode 2026-08-25: fps 100<->7 and 0xc0000374 heap
    // corruption). Bounded-wait up to ~8ms for the pose thread's next
    // delivery, then render with the newest pose we have.
    const DWORD tick0 = m_WaitedPoseTick.load(std::memory_order_acquire);
    for (int i = 0; i < 8; ++i)
    {
        const DWORD tick = m_WaitedPoseTick.load(std::memory_order_acquire);
        if (tick != tick0)
            break;
        const DWORD ageMs = tick ? (GetTickCount() - tick) : 0xffffffffu;
        if (ageMs <= 16)
            break;
        Sleep(1);
    }
    UpdateTracking();
    m_PosesWaitedThisFrame = true;
}

bool VR::RefreshPosesFromCompositor()
{
    if (!m_Compositor)
        return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    const vr::EVRCompositorError err = m_Compositor->WaitGetPoses(
        poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        std::memcpy(m_WaitedPoses, poses, sizeof(poses));
    }
    m_LastPoseWaitError.store(static_cast<int>(err), std::memory_order_release);
    m_WaitedPoseTick.store(GetTickCount(), std::memory_order_release);
    m_PoseWaitCount.fetch_add(1, std::memory_order_relaxed);
    if (m_ActionsReady.load(std::memory_order_acquire) && m_Input)
        m_Input->UpdateActionState(m_ActiveActionSets, sizeof(vr::VRActiveActionSet_t), 2);
    return err == vr::VRCompositorError_None;
}

void VR::StartPoseWaiter()
{
    if (m_OpenXrHelperBridgeActive || m_PoseWaiterThread)
        return;
    m_PoseWaiterStop.store(false, std::memory_order_release);
    m_PoseWaiterThread = CreateThread(nullptr, 0, &VR::PoseWaiterThreadMain, this, 0, nullptr);
    if (m_PoseWaiterThread)
        Game::logMsg("Pose waiter thread started (WaitGetPoses off Present)");
    else
        Game::logMsg("Pose waiter CreateThread failed err=%lu", GetLastError());
}

DWORD WINAPI VR::PoseWaiterThreadMain(LPVOID param)
{
    VR* vr = static_cast<VR*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Game::logMsg("Pose waiter running");
    while (!vr->m_PoseWaiterStop.load(std::memory_order_acquire))
    {
        if (!vr->m_Compositor)
        {
            Sleep(8);
            continue;
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        const DWORD t0 = GetTickCount();
        const vr::EVRCompositorError err = vr->m_Compositor->WaitGetPoses(
            poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        const DWORD dt = GetTickCount() - t0;
        {
            std::lock_guard<std::mutex> lock(vr->m_PoseMutex);
            std::memcpy(vr->m_WaitedPoses, poses, sizeof(poses));
        }
        vr->m_LastPoseWaitError.store(static_cast<int>(err), std::memory_order_release);
        vr->m_WaitedPoseTick.store(GetTickCount(), std::memory_order_release);
        vr->m_PoseWaitCount.fetch_add(1, std::memory_order_relaxed);
        if (vr->m_ActionsReady.load(std::memory_order_acquire) && vr->m_Input)
        {
            const vr::EVRInputError inErr = vr->m_Input->UpdateActionState(
                vr->m_ActiveActionSets, sizeof(vr::VRActiveActionSet_t), 2);
            static int s_actLog;
            if (s_actLog < 4 || (inErr != vr::VRInputError_None && s_actLog < 8))
            {
                Game::logMsg("UpdateActionState err=%d", (int)inErr);
                ++s_actLog;
            }
        }
        if (dt > 50)
            vr->m_PoseWaitOvershootCount.fetch_add(1, std::memory_order_relaxed);
        static int s_poseLog;
        if (s_poseLog < 4 || dt > 100)
        {
            Game::logMsg("Pose waiter WaitGetPoses err=%d dt=%ums valid=%d connected=%d",
                (int)err, dt,
                poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid ? 1 : 0,
                poses[vr::k_unTrackedDeviceIndex_Hmd].bDeviceIsConnected ? 1 : 0);
            ++s_poseLog;
        }
    }
    return 0;
}

bool VR::RefreshBackBufferTexture(bool forceRefresh)
{
    if (!g_D3DVR9)
        return false;
    if (!forceRefresh && m_BackBufferTextureValid)
        return true;
    const HRESULT hr = g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    const UINT w = m_VKBackBuffer.m_VulkanData.m_nWidth;
    const UINT h = m_VKBackBuffer.m_VulkanData.m_nHeight;
    m_BackBufferTextureValid = SUCCEEDED(hr) && m_VKBackBuffer.m_VulkanData.m_nImage && w >= 640 && h >= 360;
    if (m_BackBufferTextureValid && (m_RenderWidth == 0 || m_RenderHeight == 0))
    {
        m_RenderWidth = w;
        m_RenderHeight = h;
        Game::logMsg("Backbuffer VR desc %ux%u fmt=%u", w, h, m_VKBackBuffer.m_VulkanData.m_nFormat);
    }
    else if (!m_BackBufferTextureValid)
    {
        static int s_stubLog;
        if (s_stubLog < 3)
        {
            Game::logMsg("GetBackBufferData stub/unresolved hr=0x%08X %ux%u image=%llu",
                (unsigned)hr, w, h, (unsigned long long)m_VKBackBuffer.m_VulkanData.m_nImage);
            ++s_stubLog;
        }
    }
    return m_BackBufferTextureValid;
}

bool VR::FillSharedTexture(IDirect3DSurface9* surface, SharedTextureHolder& holder)
{
    if (!g_D3DVR9 || !surface)
        return false;
    D3D9_TEXTURE_VR_DESC desc{};
    if (FAILED(g_D3DVR9->GetVRDesc(surface, &desc)) || !desc.Image)
        return false;
    std::memcpy(&holder.m_VulkanData, &desc, sizeof(holder.m_VulkanData));
    holder.m_VRTexture.handle = &holder.m_VulkanData;
    holder.m_VRTexture.eType = vr::TextureType_Vulkan;
    holder.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
    holder.m_SharedHandle = desc.SharedHandle;
    holder.m_SharedHandleType = desc.SharedHandleType;
    holder.m_SharedHandleValid = desc.SharedHandleValid;
    return holder.m_VulkanData.m_nImage != 0;
}

IDirect3DSurface9* VR::SubmitSurfaceForEye(IDirect3DSurface9* eye) const
{
    if (eye && eye == m_D9LeftEyeSurface)
        return m_D9LeftEyeSubmitSurface;
    if (eye && eye == m_D9RightEyeSurface)
        return m_D9RightEyeSubmitSurface;
    return nullptr;
}

void VR::NoteMsaaEyeScene(IDirect3DSurface9* dst, bool copied)
{
    if (!copied || !dst)
        return;
    if (dst == m_D9LeftEyeSurface && m_D9LeftEyeSubmitSurface)
        m_LeftEyeMsaaHasScene = true;
    else if (dst == m_D9RightEyeSurface && m_D9RightEyeSubmitSurface)
        m_RightEyeMsaaHasScene = true;
}

IDirect3DSurface9* VR::ColorTargetForStereoEye(int stereoEye) const
{
    if (stereoEye == 1)
    {
        if (m_LeftEyeMsaaHasScene || !m_D9LeftEyeSubmitSurface)
            return m_D9LeftEyeSurface;
        return m_D9LeftEyeSubmitSurface;
    }
    if (stereoEye == 2)
    {
        if (m_RightEyeMsaaHasScene || !m_D9RightEyeSubmitSurface)
            return m_D9RightEyeSurface;
        return m_D9RightEyeSubmitSurface;
    }
    return nullptr;
}

void VR::ResolveMsaaEyesToSubmit(IDirect3DDevice9* device)
{
    if (!device)
        return;
    m_CaptureReentry = true;
    if (m_LeftEyeMsaaHasScene && m_D9LeftEyeSurface && m_D9LeftEyeSubmitSurface
        && m_D9LeftEyeSurface != m_D9LeftEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9LeftEyeSurface, nullptr, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA left eye resolve hr=0x%08X", (unsigned)hr);
    }
    if (m_RightEyeMsaaHasScene && m_D9RightEyeSurface && m_D9RightEyeSubmitSurface
        && m_D9RightEyeSurface != m_D9RightEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9RightEyeSurface, nullptr, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA right eye resolve hr=0x%08X", (unsigned)hr);
    }
    m_CaptureReentry = false;
}

void VR::ReleaseVRRenderTargetsForDeviceReset()
{
    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
    ReleaseT(m_D9LeftEyeSurface);
    ReleaseT(m_D9RightEyeSurface);
    ReleaseT(m_D9LeftEyeSubmitSurface);
    ReleaseT(m_D9RightEyeSubmitSurface);
    g_VrGloves.OnDeviceLost();
    ReleaseT(m_D9LeftEyeDepthSurface);
    ReleaseT(m_D9RightEyeDepthSurface);
    ReleaseT(m_D9LeftEyeTexture);
    ReleaseT(m_D9RightEyeTexture);
    ReleaseT(m_D9LeftEyeSubmitTexture);
    ReleaseT(m_D9RightEyeSubmitTexture);
    ReleaseOpenXrPublishTextures();
    m_LeftEyeMsaaHasScene = false;
    m_RightEyeMsaaHasScene = false;
    ReleaseT(m_D9FrameColorSurface);
    ReleaseT(m_BlitEventQuery);
    m_LeftEyeTexture = nullptr;
    m_RightEyeTexture = nullptr;
    m_UsedNamedRenderTargets = false;
    m_DirectEyeSubmit = false;
    m_StereoRenderViewActive = false;
    m_FrameCopyWidth = 0;
    m_FrameCopyHeight = 0;
    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_BackBufferTextureValid = false;
    m_FrameCopyLatched = false;
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_SkipBlockingPoseWait = true;
    m_HasSubmittedSceneFrame.store(false, std::memory_order_release);
    Game::logMsg("Released VR render targets for device reset");
}

void VR::InstallDeviceHooks(IDirect3DDevice9* device)
{
    if (m_D3DHooksInstalled || !device)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);
    if (!vtbl)
        return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        Game::logMsg("MinHook init for D3D hooks failed %d", (int)st);
        return;
    }

    if (MH_CreateHook(vtbl[kIDirect3DDevice9_Present], reinterpret_cast<LPVOID>(&HookedPresent),
            reinterpret_cast<LPVOID*>(&g_OrigPresent)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook Present failed");
        return;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetRenderTarget], reinterpret_cast<LPVOID>(&HookedSetRenderTarget),
            reinterpret_cast<LPVOID*>(&g_OrigSetRenderTarget)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetRenderTarget failed");
        return;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetDepthStencilSurface], reinterpret_cast<LPVOID>(&HookedSetDepthStencil),
            reinterpret_cast<LPVOID*>(&g_OrigSetDepthStencil)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetDepthStencilSurface failed");
        g_OrigSetDepthStencil = nullptr;
    }
    if (MH_EnableHook(vtbl[kIDirect3DDevice9_Present]) != MH_OK ||
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetRenderTarget]) != MH_OK)
    {
        Game::logMsg("MH_EnableHook D3D present/RT failed");
        return;
    }
    if (g_OrigSetDepthStencil)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetDepthStencilSurface]);

    m_D3DHooksInstalled = true;
    g_DeviceHooksEnabled = true;
    Game::logMsg("D3D9 Present + SetRenderTarget + SetDepthStencil hooks installed");
}

bool VR::EnsureFrameCopySurface(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    if (!device || width < 640 || height < 360)
        return false;
    if (m_D9FrameColorSurface && m_FrameCopyWidth == width && m_FrameCopyHeight == height)
        return true;

    IDirect3DSurface9* surf = nullptr;
    const HRESULT hr = device->CreateRenderTarget(
        width, height, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr);
    if (FAILED(hr) || !surf)
    {
        Game::logMsg("Frame copy RT create failed hr=0x%08X %ux%u", (unsigned)hr, width, height);
        return false;
    }
    ReleaseT(m_D9FrameColorSurface);
    m_D9FrameColorSurface = surf;
    m_FrameCopyWidth = width;
    m_FrameCopyHeight = height;
    Game::logMsg("Frame copy RT ready %ux%u", width, height);
    return true;
}

bool VR::EnsureNamedEyeTextures()
{
    return false;
}

bool VR::NamedStereoReady() const
{
    return false;
}

void VR::PrepareNamedStereoFromPresent()
{
    if (!bmvr::TryStereoRenderView() || !m_GameplayEligible)
        return;
    // Do not ClientCmd cvars from Present during load. The 1576 matching-
    // swapchain death logged "queued cl_csm_enabled 0" then died before any
    // RenderView; CSM disable was a named-RT wrap leftover and is not
    // required for G-buffer-sized blit stereo (stereo_copy survived with CSM).
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;
    EnsureStereoEyeSurfaces();
}

bool VR::EnsurePrivateEyeSurfaces(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    m_AntiAliasing = bmvr::g_AntiAliasing;
    ChooseEyeRenderSize();
    UINT w = m_RenderWidth;
    UINT h = m_RenderHeight;
    if (w < 640 || h < 360)
    {
        w = KnownWindowWidth();
        h = KnownWindowHeight();
    }
    if (w < 640 || h < 360)
        return false;

    if (m_CreatedVRTextures.load(std::memory_order_acquire) && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth >= 640 && m_VKLeftEye.m_VulkanData.m_nHeight >= 360)
    {
        const UINT haveW = m_VKLeftEye.m_VulkanData.m_nWidth;
        const UINT haveH = m_VKLeftEye.m_VulkanData.m_nHeight;
        if (haveW == w && haveH == h)
            return true;
        const bool growForFullFrame = bmvr::TryFullFrameStereo()
            && w > haveW + 32
            && !m_StereoEyeBlitActive;
        const bool liveOffscreenResize = bmvr::TryOffscreenHmd()
            && !m_StereoEyeBlitActive
            && (w + 32 < haveW || h + 32 < haveH || haveW + 32 < w || haveH + 32 < h);
        if (liveOffscreenResize && m_EyeResizeSettleMs != 0
            && (GetTickCount() - m_EyeResizeSettleMs) < 300)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return true;
        }
        if ((!bmvr::TrySteamVrEyeRt() && !growForFullFrame && !liveOffscreenResize)
            || (!liveOffscreenResize && haveW + 32 >= w))
        {
            Game::logMsg("Keep existing D3D eyes %ux%u (requested %ux%u; HWND resize must not recreate)",
                haveW, haveH, w, h);
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return true;
        }
        Game::logMsg("Recreating D3D eyes %ux%u -> %ux%u (%s)",
            haveW, haveH, w, h,
            liveOffscreenResize ? "SteamVR live SS" : (growForFullFrame ? "FullFrame stereo" : "SteamVR recommended"));
        // Do not BeginRisky on live SS. Stereo already settled hmd_offscreen
        // after 120 frames; a recreate+quit false-banned the path.
        if (growForFullFrame && !liveOffscreenResize)
            bmvr::BeginRisky(L"ff_stereo");
        m_CreatedVRTextures.store(false, std::memory_order_release);
    }

    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);

    auto createEye = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
        ReleaseT(*surf);
        ReleaseT(*tex);
        m_CreatingTextureID = id;
        const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*tex)
        {
            Game::logMsg("CreateTexture eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        if (!*surf)
            (*tex)->GetSurfaceLevel(0, surf);
        // MSAA eyes cannot be OpenVR submit targets (GetVRDesc is skipped).
        if (!UseVrMsaa())
        {
            if (!FillSharedTexture(*surf, vk))
            {
                Game::logMsg("GetVRDesc failed for eye id=%d surf=%p", (int)id, (void*)*surf);
                return false;
            }
            Game::logMsg("Eye RT id=%d %ux%u img=%llu", (int)id, vk.m_VulkanData.m_nWidth, vk.m_VulkanData.m_nHeight,
                (unsigned long long)vk.m_VulkanData.m_nImage);
        }
        else
        {
            Game::logMsg("Eye RT id=%d %ux%u MSAA=%u (submit texture holds Vulkan desc)",
                (int)id, w, h, m_AntiAliasing);
        }
        return true;
    };

    auto createDepth = [&](TextureID id, IDirect3DSurface9** depth) -> bool {
        ReleaseT(*depth);
        m_CreatingTextureID = id;
        const HRESULT hr = device->CreateDepthStencilSurface(
            w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, depth, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*depth)
        {
            Game::logMsg("CreateDepthStencil eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        return true;
    };

    bool leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
    bool rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
    const bool depthOk = createDepth(Texture_LeftEye, &m_D9LeftEyeDepthSurface)
        && createDepth(Texture_RightEye, &m_D9RightEyeDepthSurface);
    EnsureFrameCopySurface(device, w, h);

    bool submitOk = !UseVrMsaa();
    if (UseVrMsaa())
    {
        auto createSubmit = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
            ReleaseT(*surf);
            ReleaseT(*tex);
            m_CreatingTextureID = id;
            const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
            m_CreatingTextureID = Texture_None;
            if (FAILED(hr) || !*tex)
            {
                Game::logMsg("CreateTexture submit id=%d hr=0x%08X", (int)id, (unsigned)hr);
                return false;
            }
            if (!*surf)
                (*tex)->GetSurfaceLevel(0, surf);
            if (!FillSharedTexture(*surf, vk))
            {
                Game::logMsg("GetVRDesc failed for submit id=%d surf=%p", (int)id, (void*)*surf);
                return false;
            }
            Game::logMsg("Eye submit RT id=%d %ux%u img=%llu", (int)id, vk.m_VulkanData.m_nWidth, vk.m_VulkanData.m_nHeight,
                (unsigned long long)vk.m_VulkanData.m_nImage);
            return true;
        };
        submitOk = createSubmit(Texture_LeftEyeSubmit, &m_D9LeftEyeSubmitTexture, &m_D9LeftEyeSubmitSurface, m_VKLeftEye)
            && createSubmit(Texture_RightEyeSubmit, &m_D9RightEyeSubmitTexture, &m_D9RightEyeSubmitSurface, m_VKRightEye);
        if (!submitOk)
        {
            Game::logMsg("MSAA submit RTs failed; recreating eyes without MSAA");
            m_AntiAliasing = 0;
            bmvr::g_AntiAliasing = 0;
            ReleaseT(m_D9LeftEyeSubmitSurface);
            ReleaseT(m_D9RightEyeSubmitSurface);
            ReleaseT(m_D9LeftEyeSubmitTexture);
            ReleaseT(m_D9RightEyeSubmitTexture);
            const bool leftRetry = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
            const bool rightRetry = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
            leftOk = leftRetry;
            rightOk = rightRetry;
            submitOk = leftRetry && rightRetry
                && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage;
        }
    }

    m_CreatedVRTextures.store(leftOk && rightOk && submitOk
        && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage,
        std::memory_order_release);
    if (m_CreatedVRTextures.load(std::memory_order_acquire))
        m_EyeResizeSettleMs = 0;
    Game::logMsg("VR D3D eye RTs ready=%d depth=%d msaa=%u submit=%d L=%p R=%p copy=%p %ux%u",
        m_CreatedVRTextures.load() ? 1 : 0, depthOk ? 1 : 0, m_AntiAliasing, submitOk ? 1 : 0,
        (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface, (void*)m_D9FrameColorSurface, w, h);
    return m_CreatedVRTextures.load();
}

bool VR::EnsureStereoEyeSurfaces()
{
    if (!g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    const bool ok = EnsurePrivateEyeSurfaces(device);
    static int s_readyLog;
    if (ok && s_readyLog < 2)
    {
        Game::logMsg("Stereo D3D eyes ready=%d %ux%u L=%p R=%p",
            1, m_RenderWidth, m_RenderHeight,
            (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface);
        ++s_readyLog;
    }
    device->Release();
    return ok;
}

bool VR::StereoEyesReady() const
{
    return m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_RenderWidth >= 640 && m_RenderHeight >= 360
        && m_VKLeftEye.m_VulkanData.m_nWidth == m_RenderWidth
        && m_VKLeftEye.m_VulkanData.m_nHeight == m_RenderHeight;
}

void VR::ClearUnusedDesktopBackbuffer()
{
    if (!g_D3DVR9)
        return;
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    // ColorFill the unused 16:9 window region. Do not use SteamVR eye size
    // here — 3168x3100 would black the whole desktop backbuffer.

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(bb->GetDesc(&desc)))
    {
        if (desc.Width > fbW)
        {
            const RECT right = {
                static_cast<LONG>(fbW), 0,
                static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)
            };
            device->ColorFill(bb, &right, D3DCOLOR_XRGB(0, 0, 0));
        }
        if (desc.Height > fbH)
        {
            const LONG fillW = static_cast<LONG>((std::min)(fbW, desc.Width));
            const RECT bottom = {
                0, static_cast<LONG>(fbH),
                fillW, static_cast<LONG>(desc.Height)
            };
            device->ColorFill(bb, &bottom, D3DCOLOR_XRGB(0, 0, 0));
        }
    }
    bb->Release();
    device->Release();
}

void VR::MirrorStereoToDesktopWindow()
{
    if (!m_D9LeftEyeSurface || !g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }

    D3DSURFACE_DESC bbDesc{};
    UINT eyeW = 0, eyeH = 0;
    if (FAILED(bb->GetDesc(&bbDesc)) || !ResolveSurfaceSize(m_D9LeftEyeSurface, eyeW, eyeH)
        || bbDesc.Width < 640 || bbDesc.Height < 360 || eyeW < 640 || eyeH < 360)
    {
        bb->Release();
        device->Release();
        return;
    }

    const float srcAspect = static_cast<float>(eyeW) / static_cast<float>(eyeH);
    const float dstAspect = static_cast<float>(bbDesc.Width) / static_cast<float>(bbDesc.Height);
    UINT dw = bbDesc.Width;
    UINT dh = bbDesc.Height;
    UINT dx = 0;
    UINT dy = 0;
    if (srcAspect > dstAspect)
    {
        dh = static_cast<UINT>(static_cast<float>(bbDesc.Width) / srcAspect + 0.5f);
        if (dh > bbDesc.Height)
            dh = bbDesc.Height;
        dy = (bbDesc.Height - dh) / 2;
    }
    else
    {
        dw = static_cast<UINT>(static_cast<float>(bbDesc.Height) * srcAspect + 0.5f);
        if (dw > bbDesc.Width)
            dw = bbDesc.Width;
        dx = (bbDesc.Width - dw) / 2;
    }
    RECT dest = {
        static_cast<LONG>(dx), static_cast<LONG>(dy),
        static_cast<LONG>(dx + dw), static_cast<LONG>(dy + dh)
    };

    IDirect3DSurface9* oldRt = nullptr;
    device->GetRenderTarget(0, &oldRt);
    m_CaptureReentry = true;
    if (oldRt == bb)
        device->SetRenderTarget(0, m_D9LeftEyeSurface);
    device->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    HRESULT hr = device->StretchRect(m_D9LeftEyeSurface, nullptr, bb, &dest, D3DTEXF_LINEAR);
    if (FAILED(hr))
        hr = device->StretchRect(m_D9LeftEyeSurface, nullptr, bb, &dest, D3DTEXF_NONE);
    if (oldRt)
        device->SetRenderTarget(0, oldRt);
    m_CaptureReentry = false;

    static int s_mirrorLog;
    if (s_mirrorLog < 4 || FAILED(hr))
    {
        Game::logMsg("Desktop letterbox left eye %ux%u -> BB %ux%u dest=%d,%d %ux%u hr=0x%08X",
            eyeW, eyeH, bbDesc.Width, bbDesc.Height,
            dest.left, dest.top, dw, dh, (unsigned)hr);
        ++s_mirrorLog;
    }

    if (oldRt)
        oldRt->Release();
    bb->Release();
    device->Release();
}

bool VR::BlitHmdViewFromBackbuffer(IDirect3DSurface9* dst, bool flushGpu)
{
    // Fallback only: when FullFrame == eye size the unbind StretchRect is 1:1.
    // If that miss fires, copy the top-left HMD-fit rectangle of the 16:9 BB.
    if (!dst || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return false;
    }

    UINT bbW = 0, bbH = 0;
    if (!ResolveSurfaceSize(bb, bbW, bbH) || bbW < 640 || bbH < 360)
    {
        bb->Release();
        device->Release();
        return false;
    }

    UINT cropW = m_RenderWidth >= 640 ? m_RenderWidth : bbW;
    UINT cropH = m_RenderHeight >= 360 ? m_RenderHeight : bbH;
    if (cropW > bbW)
        cropW = bbW;
    if (cropH > bbH)
        cropH = bbH;
    // GB-match: the 2560 BB holds the full HMD frustum (anamorphic in 16:9
    // pixels). Top-left 1584 crop would drop FOV. Squash the whole BB into
    // the 1.1 eye (cancels the 16:9 vs 1.1 pixel stretch).
    const bool squashFull = bmvr::UseGbMatchViewLock();
    if (squashFull)
    {
        cropW = bbW;
        cropH = bbH;
    }
    const UINT x0 = 0;
    const UINT y0 = 0;
    RECT srcRect = {
        0, 0,
        static_cast<LONG>(cropW), static_cast<LONG>(cropH)
    };
    const RECT* srcPtr = (!squashFull && (cropW != bbW || cropH != bbH)) ? &srcRect : nullptr;
    m_LastEyeBlitWasWindowCrop = false;

    m_CaptureReentry = true;
    const bool upscale = m_RenderWidth > bbW + 32 || m_RenderHeight > bbH + 32;
    const D3DTEXTUREFILTERTYPE filter = upscale ? D3DTEXF_LINEAR : D3DTEXF_NONE;
    HRESULT hr = device->StretchRect(bb, srcPtr, dst, nullptr, filter);
    if (FAILED(hr))
    {
        IDirect3DSurface9* submit = SubmitSurfaceForEye(dst);
        if (submit && submit != dst)
            hr = device->StretchRect(bb, srcPtr, submit, nullptr, filter);
    }
    else
        NoteMsaaEyeScene(dst, true);
    m_CaptureReentry = false;

    static int s_bbBlitLog;
    if (s_bbBlitLog < 8 || FAILED(hr))
    {
        Game::logMsg(
            "HMD BB blit %u,%u %ux%u of BB %ux%u -> eye %ux%u hr=0x%08X (crop=%d squash=%d upscale=%d)",
            x0, y0, cropW, cropH, bbW, bbH, m_RenderWidth, m_RenderHeight, (unsigned)hr,
            (srcPtr != nullptr) ? 1 : 0, squashFull ? 1 : 0, upscale ? 1 : 0);
        if (upscale && squashFull)
            Game::logMsg("G-buffer did not grow; upscaling window %ux%u into offscreen eyes %ux%u",
                bbW, bbH, m_RenderWidth, m_RenderHeight);
        ++s_bbBlitLog;
    }

    if (SUCCEEDED(hr))
    {
        m_LastStereoBlitWidth = cropW;
        m_LastStereoBlitHeight = cropH;
        if (flushGpu)
            FlushStereoBlitGpu();
    }

    bb->Release();
    device->Release();
    return SUCCEEDED(hr);
}

void VR::EnsureHudOverlay()
{
    if (m_HudOverlayReady)
        return;
    if (!bmvr::TryHudOverlay() || !m_Game || !m_Game->m_MaterialSystem || !m_Game->m_Offsets)
        return;
    if (!m_OpenXrHelperBridgeActive && m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;
    static DWORD s_lastTry;
    const DWORD now = GetTickCount();
    if (s_lastTry != 0 && (now - s_lastTry) < 250)
        return;
    s_lastTry = now;

    const int w = static_cast<int>(KnownWindowWidth());
    const int h = static_cast<int>(KnownWindowHeight());
    if (w < 640 || h < 360)
        return;

    bmvr::BeginRisky(L"hud_overlay");
    void* mat = m_Game->m_MaterialSystem;
    unsigned char* running = reinterpret_cast<unsigned char*>(mat) + Offsets::kCMaterialSystem_isGameRunning;
    const unsigned char prevRunning = *running;
    *running = 0;

    using BeginFn = void(__thiscall*)(void*);
    using EndFn = void(__thiscall*)(void*);
    using CreateFn = ITexture*(__thiscall*)(void*, const char*, int, int, int, int, int, unsigned, unsigned);
    auto beginRt = reinterpret_cast<BeginFn>(m_Game->m_Offsets->BeginRTAlloc.address);
    auto endRt = reinterpret_cast<EndFn>(m_Game->m_Offsets->EndRTAlloc.address);
    auto createRt = reinterpret_cast<CreateFn>(m_Game->m_Offsets->CreateNamedRTEx.address);
    if (beginRt)
        beginRt(mat);
    m_CreatingTextureID = Texture_HUD;
    ITexture* hud = nullptr;
    if (createRt)
    {
        hud = createRt(mat, "bmvrHUD", w, h, RT_SIZE_NO_CHANGE, IMAGE_FORMAT_BGRA8888,
            MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_NOMIP, 0);
    }
    m_CreatingTextureID = Texture_None;
    if (endRt)
        endRt(mat);
    *running = prevRunning;

    m_HUDTexture = hud;
    if (m_HUDTexture && m_D9HUDSurface && m_VKHUD.m_VRTexture.handle)
    {
        m_HudOverlayReady = true;
        ClearHudSurface(false);
        Game::logMsg("HUD overlay RT bmvrHUD %dx%d surface=%p (cleared transparent; hidden until VGUI paints)",
            w, h, (void*)m_D9HUDSurface);
        bmvr::EndRisky(L"hud_overlay");
    }
    else
    {
        Game::logMsg("HUD overlay RT not ready tex=%p surf=%p vk=%p (will retry)",
            (void*)m_HUDTexture, (void*)m_D9HUDSurface, m_VKHUD.m_VRTexture.handle);
        bmvr::EndRisky(L"hud_overlay");
    }
}

void VR::ClearHudSurface(bool opaque)
{
    if (!m_D9HUDSurface)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(m_D9HUDSurface->GetDevice(&device)) || !device)
        return;
    IDirect3DSurface9* prev = nullptr;
    device->GetRenderTarget(0, &prev);
    if (SUCCEEDED(device->SetRenderTarget(0, m_D9HUDSurface)))
    {
        const D3DCOLOR color = opaque ? D3DCOLOR_ARGB(255, 0, 0, 0) : D3DCOLOR_ARGB(0, 0, 0, 0);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.f, 0);
        if (prev)
            device->SetRenderTarget(0, prev);
    }
    if (prev)
        prev->Release();
    device->Release();
}

void VR::NoteEngineHudRtPush(const char* name, int w, int h)
{
    if (!m_IsVREnabled || !IsGameplayEligible())
        return;
    if (!m_HudOverlayReady || !m_D9HUDSurface)
        return;
    m_EngineHudRtPushed = true;
    static int s_log;
    if (s_log < 8)
    {
        Game::logMsg("HUD engine PushRT %s %dx%d (copy to bmvrHUD on pop; engine dest kept)",
            name && name[0] ? name : "?", w, h);
        ++s_log;
    }
}

void VR::BlitEngineHudRtToOverlay()
{
    const bool pushed = m_EngineHudRtPushed;
    m_EngineHudRtPushed = false;
    if (!pushed || !m_D9HUDSurface || !g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* src = nullptr;
    if (FAILED(device->GetRenderTarget(0, &src)) || !src)
    {
        device->Release();
        return;
    }
    if (src == m_D9HUDSurface)
    {
        src->Release();
        device->Release();
        return;
    }
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    src->GetDesc(&srcDesc);
    m_D9HUDSurface->GetDesc(&dstDesc);
    const HRESULT hr = device->StretchRect(src, nullptr, m_D9HUDSurface, nullptr, D3DTEXF_LINEAR);
    if (SUCCEEDED(hr))
        NoteHudPainted();
    static int s_blitLog;
    if (s_blitLog < 8)
    {
        Game::logMsg("HUD blit engineRT %ux%u fmt=0x%X -> bmvrHUD %ux%u hr=0x%08X painted=%d",
            srcDesc.Width, srcDesc.Height, srcDesc.Format,
            dstDesc.Width, dstDesc.Height, hr, SUCCEEDED(hr) ? 1 : 0);
        ++s_blitLog;
    }
    src->Release();
    device->Release();
}

void VR::SubmitHudOverlay()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    if (!m_HudOverlayReady || !m_Overlay || m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;
    if (!m_VKHUD.m_VRTexture.handle)
        return;

    // After UnlockSubmissionQueue only. Nested LockSubmissionQueue deadlocks
    // DXVK m_mutexQueue. Do not ShowOverlay a cleared-black RT: VGui_Paint
    // was skipped last launch and this was the opaque black square in the HMD.
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire))
    {
        m_Overlay->HideOverlay(m_HUDTopHandle);
        static int s_hideLog;
        if (s_hideLog < 4)
        {
            Game::logMsg("HUD overlay hidden (VGUI has not painted bmvrHUD this frame)");
            ++s_hideLog;
        }
        return;
    }

    const bool pauseUi = PauseUiActive();
    // Gameplay extra-paint is CEngineVGui PAINT_UIPANELS (GameUI / pause
    // chrome), not client DRAWHUD / _rt_Hud. Showing that on a transparent
    // SteamVR overlay every frame is the floating pause-menu glass in the HMD
    // (log: extra-paint mode=0x7 pause=0, overlay shown). HEV HUD cannot come
    // from this path.
    if (!pauseUi)
    {
        m_Overlay->HideOverlay(m_HUDTopHandle);
        static int s_hidePlay;
        if (s_hidePlay < 4)
        {
            Game::logMsg("HUD overlay hidden (gameplay; skip GameUI extra-paint)");
            ++s_hidePlay;
        }
        m_HudPaintedThisFrame.store(false, std::memory_order_release);
        return;
    }
    // Pause needs a readable quad. Do not enlarge until VGUI has painted.
    const float widthM = pauseUi ? 1.35f : bmvr::g_HudSize;
    const float distM = pauseUi ? 1.15f : bmvr::g_HudDistance;
    const float downM = pauseUi ? -0.05f : -0.12f;
    vr::HmdMatrix34_t rel{};
    rel.m[0][0] = 1.f;
    rel.m[1][1] = 1.f;
    rel.m[2][2] = 1.f;
    rel.m[1][3] = downM;
    rel.m[2][3] = -distM;
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(
        m_HUDTopHandle, vr::k_unTrackedDeviceIndex_Hmd, &rel);
    m_Overlay->SetOverlayWidthInMeters(m_HUDTopHandle, widthM);
    m_Overlay->SetOverlayInputMethod(m_HUDTopHandle,
        pauseUi ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
    m_Overlay->ShowOverlay(m_HUDTopHandle);
    static int s_showLog;
    if (s_showLog < 4)
    {
        Game::logMsg("HUD overlay shown distance=%.2f m width=%.2f m pause=%d",
            distM, widthM, pauseUi ? 1 : 0);
        ++s_showLog;
    }
    m_HudPaintedThisFrame.store(false, std::memory_order_release);
}

void VR::BindHudOverlayWhileQueueLocked()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    if (!m_HudOverlayReady || !m_Overlay || m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;
    if (!m_VKHUD.m_VRTexture.handle)
        return;
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire))
        return;
    if (!PauseUiActive())
        return;
    m_Overlay->SetOverlayTexture(m_HUDTopHandle, &m_VKHUD.m_VRTexture);
    static int s_hudBindLog;
    if (s_hudBindLog < 2)
    {
        Game::logMsg("HUD overlay SetOverlayTexture while queue locked");
        ++s_hudBindLog;
    }
}

void VR::CreateVRTextures()
{
    Game::logMsg("CreateVRTextures begin presents=%u", m_EligiblePresents);
    if (!g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    RefreshBackBufferTexture(true);
    EnsurePrivateEyeSurfaces(device);
    device->Release();
    EnsureHudOverlay();
}

void VR::BeginStereoEyeBlit(IDirect3DSurface9* dst)
{
    m_StereoEyeBlitDest = dst;
    m_StereoEyeBlitActive = dst != nullptr;
    m_StereoEyeBlitOk = false;
    if (dst == m_D9LeftEyeSurface)
        m_LeftEyeMsaaHasScene = false;
    else if (dst == m_D9RightEyeSurface)
        m_RightEyeMsaaHasScene = false;
    m_StereoRedirectedToEye = false;
    m_StereoEyeBlitRank = 0;
}

bool VR::EndStereoEyeBlit()
{
    const bool ok = m_StereoEyeBlitOk;
    m_StereoEyeBlitActive = false;
    m_StereoEyeBlitDest = nullptr;
    return ok;
}

bool VR::StereoUnbindMatchesEye() const
{
    if (bmvr::UseGbMatchViewLock())
        return false;
    const int dw = static_cast<int>(m_LastStereoBlitWidth) - static_cast<int>(m_RenderWidth);
    const int dh = static_cast<int>(m_LastStereoBlitHeight) - static_cast<int>(m_RenderHeight);
    return dw > -32 && dw < 32 && dh > -32 && dh < 32
        && m_LastStereoBlitWidth >= 640 && m_LastStereoBlitHeight >= 360;
}

void VR::FlushStereoBlitGpu()
{
    // Both eyes share _rt_FullFrameFB. StretchRect is queued; the right-eye
    // RenderView overwrites that RT before the left copy lands, so both eyes
    // submit the same image and near field cannot fuse. Event-query flush is
    // not WaitDeviceIdle (that crash-skipped during load).
    // One GetData(FLUSH) waits for the whole left eye. 64 FLUSH polls serialized
    // complex scenes (~2x GPU time). Off by default (g_StereoBlitGpuFlush);
    // DXVK StretchRect hazards should keep the BB copy coherent.
    if (!g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    if (!m_BlitEventQuery)
    {
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &m_BlitEventQuery)) || !m_BlitEventQuery)
        {
            static int s_qfail;
            if (s_qfail < 3)
            {
                Game::logMsg("Stereo blit GPU event-query create failed");
                ++s_qfail;
            }
            device->Release();
            return;
        }
    }
    m_BlitEventQuery->Issue(D3DISSUE_END);
    const HRESULT hr = m_BlitEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    static int s_qok;
    if (s_qok < 2)
    {
        Game::logMsg("Stereo blit GPU event-query flush hr=0x%08X", (unsigned)hr);
        ++s_qok;
    }
    device->Release();
}

void VR::CaptureGameColorOnUnbind(IDirect3DSurface9* oldRt, uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH)
{
    (void)vpX;
    (void)vpY;
    (void)vpW;
    (void)vpH;
    // Menu/Present unbind StretchRect raced (2026-08-16). Only copy during an
    // eye RenderView, while FullFrameFB still holds the HMD-aspect scene.
    // client.dll RenderView (Ghidra 1020f5e4) restores the prologue RT — the
    // 16:9 D3D backbuffer — before our post-RenderView blit, so RT0 after
    // callOriginal is the wrong aspect (near fusion only at distance).
    if (!m_StereoEyeBlitActive || !m_StereoEyeBlitDest || !oldRt || m_CaptureReentry)
        return;
    // Native offscreen paints LDR onto the eyes via SetRT redirect. Copying
    // A2R10 FullFrame here is the ff_hmdfit white HMD.
    if (bmvr::OffscreenWorldMatchesEyes())
        return;
    if (oldRt == m_StereoEyeBlitDest || oldRt == m_D9LeftEyeSurface || oldRt == m_D9RightEyeSurface
        || oldRt == m_D9FrameColorSurface || oldRt == m_D9BlankSurface)
        return;

    UINT w = 0, h = 0;
    D3DSURFACE_DESC desc{};
    if (!ResolveSurfaceSize(oldRt, w, h, &desc) || w < 640 || h < 360)
        return;
    if (desc.Format == D3DFMT_D16 || desc.Format == D3DFMT_D24S8 || desc.Format == D3DFMT_D24X8
        || desc.Format == D3DFMT_D32 || desc.Format == D3DFMT_D24FS8)
        return;

    const int rank = SceneColorRank(desc.Format);
    if (rank <= 0)
        return;
    if (m_StereoEyeBlitOk && rank < m_StereoEyeBlitRank)
        return;

    const int eyeW = static_cast<int>(m_RenderWidth);
    const int eyeH = static_cast<int>(m_RenderHeight);
    if (eyeW < 640 || eyeH < 360)
        return;
    if (abs(static_cast<int>(w) - eyeW) > 32 || abs(static_cast<int>(h) - eyeH) > 32)
        return;

    IDirect3DDevice9* device = nullptr;
    if (!g_D3DVR9 || FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    m_CaptureReentry = true;
    HRESULT hr = device->StretchRect(oldRt, nullptr, m_StereoEyeBlitDest, nullptr, D3DTEXF_NONE);
    if (FAILED(hr))
    {
        IDirect3DSurface9* submit = SubmitSurfaceForEye(m_StereoEyeBlitDest);
        if (submit && submit != m_StereoEyeBlitDest)
            hr = device->StretchRect(oldRt, nullptr, submit, nullptr, D3DTEXF_NONE);
    }
    else
        NoteMsaaEyeScene(m_StereoEyeBlitDest, true);
    m_CaptureReentry = false;
    device->Release();

    if (SUCCEEDED(hr))
    {
        m_StereoEyeBlitOk = true;
        m_StereoEyeBlitRank = rank;
        m_LastStereoBlitWidth = w;
        m_LastStereoBlitHeight = h;
        static int s_unbindBlitLog;
        if (s_unbindBlitLog < 12)
        {
            Game::logMsg("Stereo unbind blit 1:1 eye=%d %ux%u fmt=%u rank=%d src=%p dest=%p",
                m_StereoEye, w, h, (unsigned)desc.Format, rank, (void*)oldRt, (void*)m_StereoEyeBlitDest);
            ++s_unbindBlitLog;
        }
    }
    else
    {
        static int s_unbindFailLog;
        if (s_unbindFailLog < 6)
        {
            Game::logMsg("Stereo unbind blit failed hr=0x%08X src=%ux%u fmt=%u",
                (unsigned)hr, w, h, (unsigned)desc.Format);
            ++s_unbindFailLog;
        }
    }
}

bool VR::BlitCurrentGameColorTo(IDirect3DSurface9* dst, bool flushGpu)
{
    if (!dst || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    IDirect3DSurface9* rt0 = nullptr;
    device->GetRenderTarget(0, &rt0);
    IDirect3DSurface9* bb = nullptr;
    UINT w = 0, h = 0;
    IDirect3DSurface9* src = rt0;
    if (!ResolveSurfaceSize(rt0, w, h) || w < 640 || h < 360)
    {
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        src = bb;
        ResolveSurfaceSize(src, w, h);
    }

    bool ok = false;
    if (src && w >= 640 && h >= 360)
    {
        RECT srcRect{};
        const RECT* srcPtr = nullptr;
        D3DVIEWPORT9 vp{};
        UINT cropW = w, cropH = h;
        LONG x0 = 0, y0 = 0;
        if (SUCCEEDED(device->GetViewport(&vp)) && vp.Width >= 640 && vp.Height >= 360
            && vp.Width <= w && vp.Height <= h)
        {
            x0 = static_cast<LONG>(vp.X);
            y0 = static_cast<LONG>(vp.Y);
            cropW = vp.Width;
            cropH = vp.Height;
        }
        else if (m_RenderWidth >= 640 && m_RenderHeight >= 360
            && w >= m_RenderWidth && h >= m_RenderHeight)
        {
            cropW = m_RenderWidth;
            cropH = m_RenderHeight;
        }
        if (cropW != w || cropH != h || x0 != 0 || y0 != 0)
        {
            srcRect = { x0, y0, x0 + static_cast<LONG>(cropW), y0 + static_cast<LONG>(cropH) };
            srcPtr = &srcRect;
        }
        m_CaptureReentry = true;
        HRESULT hr = device->StretchRect(src, srcPtr, dst, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            IDirect3DSurface9* submit = SubmitSurfaceForEye(dst);
            if (submit && submit != dst)
                hr = device->StretchRect(src, srcPtr, submit, nullptr, D3DTEXF_NONE);
        }
        else
            NoteMsaaEyeScene(dst, true);
        m_CaptureReentry = false;
        ok = SUCCEEDED(hr);
        static int s_blitLog;
        if (s_blitLog < 8)
        {
            Game::logMsg("Stereo blit fallback RT0/bb %ux%u crop=%ux%u hr=0x%08X (want HMD %ux%u)",
                w, h, cropW, cropH, (unsigned)hr, m_RenderWidth, m_RenderHeight);
            ++s_blitLog;
        }
        if (ok)
        {
            if (flushGpu)
                FlushStereoBlitGpu();
        }
    }

    if (rt0)
        rt0->Release();
    if (bb)
        bb->Release();
    device->Release();
    return ok;
}

void VR::CaptureFrameBeforePresent()
{
    const bool paused = m_Game && SehIsPaused(m_Game->m_EngineClient);
    // Direct-eye Submit skips this capture. While paused the ESC menu is
    // painted onto the backbuffer after the stereo blit, so copy that crop
    // into both eyes (L4D2VR desktop HUD / menu overlay).
    if (m_DirectEyeSubmit && paused && m_D9LeftEyeSurface && m_D9RightEyeSurface)
    {
        BlitHmdViewFromBackbuffer(m_D9LeftEyeSurface);
        BlitHmdViewFromBackbuffer(m_D9RightEyeSurface);
        m_RenderedNewFrame.store(true, std::memory_order_release);
        return;
    }
    if (m_DirectEyeSubmit)
        return;
    if (!m_IsVREnabled || !g_D3DVR9)
        return;
    if (!ShouldCompositorSubmit())
        return;

    if (m_FrameCopyLatched && m_D9FrameColorSurface)
    {
        m_RenderedNewFrame.store(true, std::memory_order_release);
        return;
    }

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    IDirect3DSurface9* rt0 = nullptr;
    device->GetRenderTarget(0, &rt0);

    UINT probeW = 0, probeH = 0;
    const bool rtUsable = ResolveSurfaceSize(rt0, probeW, probeH) && probeW >= 640 && probeH >= 360;

    IDirect3DSurface9* bb = nullptr;
    if (!rtUsable)
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

    IDirect3DSurface9* srcSurf = rtUsable ? rt0 : bb;

    UINT rtW = 0, rtH = 0;
    D3DSURFACE_DESC rtDesc{};
    const bool rtOk = ResolveSurfaceSize(srcSurf, rtW, rtH, &rtDesc);
    const UINT winW = KnownWindowWidth();
    const UINT winH = KnownWindowHeight();

    D3DVIEWPORT9 vp{};
    const bool vpRawOk = SUCCEEDED(device->GetViewport(&vp)) && vp.Width >= 640 && vp.Height >= 360;
    const bool vpOk = vpRawOk && vp.Width <= winW + 16 && vp.Height <= winH + 16;

    LONG x0 = 0, y0 = 0;
    UINT cropW = 0, cropH = 0;
    const char* name = "none";
    if (rtOk && srcSurf && rtW >= 640 && rtH >= 360)
    {
        if (srcSurf == bb)
        {
            cropW = rtW;
            cropH = rtH;
            name = "bb";
        }
        else if (vpOk)
        {
            x0 = (LONG)vp.X;
            y0 = (LONG)vp.Y;
            cropW = (std::min)((UINT)vp.Width, winW);
            cropH = (std::min)((UINT)vp.Height, winH);
            name = "rt0-vp";
        }
        else if (rtW > winW + 16 || rtH > winH + 16)
        {
            cropW = (std::min)(rtW, winW);
            cropH = (std::min)(rtH, winH);
            x0 = 0;
            y0 = (rtH > cropH) ? (LONG)(rtH - cropH) : 0;
            name = "rt0-bl";
        }
        else
        {
            cropW = rtW;
            cropH = rtH;
            name = "rt0";
        }
    }

    HRESULT hr = E_FAIL;
    if (cropW >= 640 && cropH >= 360 && EnsureFrameCopySurface(device, cropW, cropH))
    {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        cropW = (std::min)(cropW, rtW - (UINT)x0);
        cropH = (std::min)(cropH, rtH - (UINT)y0);
        RECT src{ x0, y0, x0 + (LONG)cropW, y0 + (LONG)cropH };
        const bool useRect = (x0 != 0 || y0 != 0 || rtW > winW + 16 || rtH > winH + 16);
        m_CaptureReentry = true;
        hr = device->StretchRect(srcSurf, useRect ? &src : nullptr, m_D9FrameColorSurface, nullptr, D3DTEXF_NONE);
        m_CaptureReentry = false;
        if (SUCCEEDED(hr))
        {
            m_RenderedNewFrame.store(true, std::memory_order_release);
            m_FrameCopyLatched = true;
            static int s_preLog;
            if (s_preLog < 6)
            {
                Game::logMsg("PrePresent capture %s %ux%u from src=%ux%u hr=0x%08X",
                    name, cropW, cropH, rtW, rtH, (unsigned)hr);
                ++s_preLog;
            }
        }
        else
        {
            static int s_srLog;
            if (s_srLog < 6)
            {
                Game::logMsg("PrePresent StretchRect %s failed hr=0x%08X src=%ux%u crop=%ux%u",
                    name, (unsigned)hr, rtW, rtH, cropW, cropH);
                ++s_srLog;
            }
        }
    }
    else
    {
        static int s_noneLog;
        if (s_noneLog < 6)
        {
            UINT bbW = 0, bbH = 0;
            ResolveSurfaceSize(bb, bbW, bbH);
            Game::logMsg("PrePresent capture NONE (rt0=%dx%d bb=%ux%u win=%ux%u vp=%u,%u %ux%u)",
                rtUsable ? (int)probeW : -1, rtUsable ? (int)probeH : -1,
                bbW, bbH, winW, winH,
                vpRawOk ? vp.X : 0u, vpRawOk ? vp.Y : 0u,
                vpRawOk ? vp.Width : 0u, vpRawOk ? vp.Height : 0u);
            ++s_noneLog;
        }
    }

    if (rt0)
        rt0->Release();
    if (bb)
        bb->Release();
    device->Release();
}

void VR::SubmitVRTextures()
{
    if (!g_D3DVR9 || !m_IsVREnabled)
        return;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;

    if (m_OpenXrHelperBridgeActive)
    {
        const bool haveNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
        ++m_OpenXrSubmitAttempts;
        // Present runs far faster than the compositor consumes (measured
        // 2026-08-30: ~212 publishes/s against 90Hz SteamVR, stereo pair only
        // 1.5-5ms). Every publish re-copies two full-res eye textures and
        // drains the GPU in PrepareOpenXrEyeSurfacesForRead, and the helper has
        // no lock on the shared images: it blits a 3664x3584 eye while the game
        // overwrites that same texture 2-3 times, so its swapchain copy mixes
        // several game frames. That tears only when consecutive frames differ,
        // i.e. while the head turns. Publish one pair per compositor frame:
        // require freshly rendered eyes and wait for the helper to finish the
        // previous submit, which lands our writes in its post-xrEndFrame gap.
        if (!haveNewFrame)
        {
            ++m_OpenXrSkippedNoNewFrame;
            LogOpenXrPublishRate();
            return;
        }
        // Waiting for the helper's submit counter published right after its
        // xrEndFrame, so the image then sat a whole compositor frame before it
        // was read and the ghost flipped to leading the turn. Rotating publish
        // slots make that handshake unnecessary: cap the copy cost instead and
        // let the helper pick up a much fresher image.
        //
        // Must be QPC, not GetTickCount: that counter only advances every
        // ~15.6ms, so a millisecond gate on it published ~64/s against a 90Hz
        // compositor and starved every third display frame.
        const double nowMs = []() {
            static double s_toMs = 0.0;
            if (s_toMs == 0.0)
            {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                s_toMs = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
            }
            LARGE_INTEGER t{};
            QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) * s_toMs;
        }();
        // 7ms => ~143/s, so every 11.1ms compositor frame sees a fresh pair.
        if (m_OpenXrLastPublishMs != 0.0 && (nowMs - m_OpenXrLastPublishMs) < 7.0)
        {
            ++m_OpenXrSkippedHelperBusy;
            LogOpenXrPublishRate();
            return;
        }
        m_OpenXrLastPublishMs = nowMs;
        LogOpenXrPublishRate();
        if (!PrepareOpenXrEyeSurfacesForRead())
            return;
        if (m_OpenXrStereoRenderPoseValid)
            L4D2VR_PublishOpenXrGameRenderPose(m_OpenXrStereoRenderPose);
        const uint32_t frameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
        PublishOpenXrResolvedEyeTextures(frameId);
        ++m_OpenXrPublishes;
        if (m_HudOverlayReady && m_D9HUDSurface
            && m_HudPaintedThisFrame.load(std::memory_order_acquire)
            && PauseUiActive())
        {
            g_D3DVR9->TransferSurface(m_D9HUDSurface, FALSE);
            FillSharedTexture(m_D9HUDSurface, m_VKHUD);
            PublishOpenXrHudOverlay(frameId);
        }
        else
            HideOpenXrHudOverlay();
        if (L4D2VR_OpenXrHelperHasSubmittedFrame() || frameId > 1)
        {
            m_HasSubmittedSceneFrame.store(true, std::memory_order_release);
            ++m_SubmitCount;
            m_RenderedNewFrame.store(false, std::memory_order_release);
            if (!m_LoggedFirstSubmit)
            {
                m_LoggedFirstSubmit = true;
                Game::logMsg("[VR][OpenXRHelper] published shared eye frame %u gameRenderPose=%d",
                    frameId, m_OpenXrStereoRenderPoseValid ? 1 : 0);
            }
        }
        return;
    }

    if (!m_Compositor)
        return;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;
    const int poseErr = m_LastPoseWaitError.load(std::memory_order_acquire);
    if (poseErr == static_cast<int>(vr::VRCompositorError_DoNotHaveFocus))
    {
        if (m_Compositor)
            m_Compositor->CompositorBringToFront();
        static int s_focusSubmitLog;
        if (s_focusSubmitLog < 4)
        {
            Game::logMsg("Submit despite DoNotHaveFocus (bring compositor to front)");
            ++s_focusSubmitLog;
        }
    }

    const bool haveNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
    const bool directEyes = haveNewFrame && m_DirectEyeSubmit && m_D9LeftEyeSurface && m_D9RightEyeSurface;
    const bool haveFrame = haveNewFrame && m_D9FrameColorSurface;
    if (!directEyes && !haveFrame)
        return;

    const uint32_t poseToken = m_PoseWaitCount.load(std::memory_order_acquire);
    if (poseToken == 0)
        return;
    if (poseToken == m_LastSubmittedPoseToken.load(std::memory_order_acquire))
        return;

    static int s_submitEnter;
    if (s_submitEnter < 4)
    {
        Game::logMsg("Submit enter direct=%d frame=%d named=%d eye=%ux%u",
            directEyes ? 1 : 0, haveFrame ? 1 : 0, m_UsedNamedRenderTargets ? 1 : 0,
            m_RenderWidth, m_RenderHeight);
        ++s_submitEnter;
    }

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    HRESULT hrL = S_OK, hrR = S_OK;
    LONG usedOff = 0;
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};

    if (!directEyes)
    {
        const bool haveSrc = SUCCEEDED(m_D9FrameColorSurface->GetDesc(&srcDesc)) && srcDesc.Width >= 64 && srcDesc.Height >= 64;
        const bool haveDst = m_D9LeftEyeSurface && SUCCEEDED(m_D9LeftEyeSurface->GetDesc(&dstDesc)) && dstDesc.Width >= 64;

        hrL = E_FAIL;
        hrR = E_FAIL;
        if (haveSrc && haveDst && m_D9LeftEyeSurface)
        {
            m_CaptureReentry = true;
            if (m_StereoCopyOffset && m_D9RightEyeSurface)
            {
                LONG useOff = m_StereoOffsetPx;
                if (useOff <= 0)
                    useOff = (std::max)(8L, (LONG)(srcDesc.Width / 64));
                const LONG maxOff = (LONG)(srcDesc.Width / 8);
                if (useOff > maxOff) useOff = maxOff;
                usedOff = useOff;
                RECT leftSrc{ useOff, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
                RECT rightSrc{ 0, 0, (LONG)srcDesc.Width - useOff, (LONG)srcDesc.Height };
                hrL = device->StretchRect(m_D9FrameColorSurface, &leftSrc, m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = device->StretchRect(m_D9FrameColorSurface, &rightSrc, m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR);
                if (SUCCEEDED(hrL))
                    NoteMsaaEyeScene(m_D9LeftEyeSurface, true);
                else if (m_D9LeftEyeSubmitSurface)
                    hrL = device->StretchRect(m_D9FrameColorSurface, &leftSrc, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
                if (SUCCEEDED(hrR))
                    NoteMsaaEyeScene(m_D9RightEyeSurface, true);
                else if (m_D9RightEyeSubmitSurface)
                    hrR = device->StretchRect(m_D9FrameColorSurface, &rightSrc, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
            }
            else
            {
                hrL = device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = m_D9RightEyeSurface
                    ? device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR)
                    : hrL;
                if (SUCCEEDED(hrL))
                    NoteMsaaEyeScene(m_D9LeftEyeSurface, true);
                else if (m_D9LeftEyeSubmitSurface)
                    hrL = device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
                if (m_D9RightEyeSurface && SUCCEEDED(hrR))
                    NoteMsaaEyeScene(m_D9RightEyeSurface, true);
                else if (m_D9RightEyeSubmitSurface)
                    hrR = device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
            }
            m_CaptureReentry = false;
        }

        if (FAILED(hrL))
        {
            device->Release();
            m_RenderedNewFrame.store(false, std::memory_order_release);
            m_FrameCopyLatched = false;
            return;
        }
    }
    else
    {
        if (m_D9LeftEyeSurface)
            m_D9LeftEyeSurface->GetDesc(&dstDesc);
        srcDesc = dstDesc;
        hrR = S_OK;
    }

    g_D3DVR9->LockDevice();
    ResolveMsaaEyesToSubmit(device);
    IDirect3DSurface9* leftSubmit = m_D9LeftEyeSubmitSurface ? m_D9LeftEyeSubmitSurface : m_D9LeftEyeSurface;
    IDirect3DSurface9* rightSubmit = m_D9RightEyeSubmitSurface ? m_D9RightEyeSubmitSurface : m_D9RightEyeSurface;
    const BOOL waitGpu = bmvr::TryWaitDeviceIdle() ? TRUE : FALSE;
    if (bmvr::TryWaitDeviceIdle())
        bmvr::BeginRisky(L"wait_idle");
    const bool okL = leftSubmit && SUCCEEDED(g_D3DVR9->TransferSurface(leftSubmit, waitGpu))
        && FillSharedTexture(leftSubmit, m_VKLeftEye);
    bool okR = false;
    if (rightSubmit && SUCCEEDED(hrR))
        okR = SUCCEEDED(g_D3DVR9->TransferSurface(rightSubmit, waitGpu)) && FillSharedTexture(rightSubmit, m_VKRightEye);
    if (m_HudOverlayReady && m_D9HUDSurface
        && m_HudPaintedThisFrame.load(std::memory_order_acquire))
    {
        g_D3DVR9->TransferSurface(m_D9HUDSurface, FALSE);
        FillSharedTexture(m_D9HUDSurface, m_VKHUD);
    }
    if (bmvr::TryWaitDeviceIdle() && g_D3DVR9)
        g_D3DVR9->WaitDeviceIdle();
    if (bmvr::TryWaitDeviceIdle())
        bmvr::EndRisky(L"wait_idle");
    g_D3DVR9->UnlockDevice();

    if (!okL)
    {
        device->Release();
        return;
    }

    EnsureHudOverlay();

    if (FAILED(g_D3DVR9->LockSubmissionQueue()))
    {
        device->Release();
        return;
    }

    vr::VRTextureBounds_t leftBounds{};
    vr::VRTextureBounds_t rightBounds{};
    const float imgW = srcDesc.Width > 0 ? (float)srcDesc.Width : (float)dstDesc.Width;
    const float imgH = srcDesc.Height > 0 ? (float)srcDesc.Height : (float)dstDesc.Height;
    const float imgAspect = (imgW >= 64.f && imgH >= 64.f) ? (imgW / imgH) : 0.f;

    if (directEyes)
    {
        // L4D2VR vr_lifecycle_init.inl: eye textures are the hidden-area HMD
        // frustum (fov=m_Fov, aspect=m_Aspect). Submit GetProjectionRaw UVs.
        // Do not ApplyVulkanYFlip — that inverted the fused image (2026-08-17).
        leftBounds = m_TextureBounds[0];
        rightBounds = m_TextureBounds[1];
        static int s_directBoundsLog;
        if (s_directBoundsLog < 3)
        {
            Game::logMsg("Direct eye Submit UV L=(%.3f,%.3f)-(%.3f,%.3f) R=(%.3f,%.3f)-(%.3f,%.3f) img=%gx%g",
                leftBounds.uMin, leftBounds.vMin, leftBounds.uMax, leftBounds.vMax,
                rightBounds.uMin, rightBounds.vMin, rightBounds.uMax, rightBounds.vMax,
                imgW, imgH);
            ++s_directBoundsLog;
        }
    }
    else
    {
        // 16:9 game blit is not an HMD-projection eye RT. GetProjectionRaw UVs
        // showed the left strip of the pause menu, 180° rotated, rest black
        // (WMR portal 2026-08-16). Keep v unflipped. Crop U so 16:9 matches
        // HMD aspect (~1.1) instead of stretching vertically into the FOV.
        leftBounds = { 0.f, 0.f, 1.f, 1.f };
        const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
        // Cropping U to HMD aspect (u=0.191..0.809) ate the left-aligned old
        // game-UI buttons. Keep full 16:9 on the menu / !inGame capture path.
        if (inGame && imgAspect > m_Aspect * 1.02f && m_Aspect > 0.1f)
        {
            const float vis = m_Aspect / imgAspect;
            const float du = (1.f - vis) * 0.5f;
            leftBounds.uMin = du;
            leftBounds.uMax = 1.f - du;
        }
        rightBounds = leftBounds;
        static int s_boundsLog;
        if (s_boundsLog < 3)
        {
            Game::logMsg("Capture Submit UV u=%.3f..%.3f v=%.3f..%.3f img=%ux%u hmdAspect=%.3f",
                leftBounds.uMin, leftBounds.uMax, leftBounds.vMin, leftBounds.vMax,
                srcDesc.Width, srcDesc.Height, m_Aspect);
            ++s_boundsLog;
        }
    }

    vr::EVRCompositorError eL = m_Compositor->Submit(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &leftBounds, vr::Submit_Default);
    vr::EVRCompositorError eR = vr::VRCompositorError_None;
    if (okR)
        eR = m_Compositor->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &rightBounds, vr::Submit_Default);
    else
        eR = m_Compositor->Submit(vr::Eye_Right, &m_VKLeftEye.m_VRTexture, &rightBounds, vr::Submit_Default);

    BindHudOverlayWhileQueueLocked();

    g_D3DVR9->UnlockSubmissionQueue();
    SubmitHudOverlay();

    m_ActualCompositorSubmitCount.fetch_add(1, std::memory_order_relaxed);
    ++m_SubmitCount;
    m_LastSubmittedPoseToken.store(poseToken, std::memory_order_release);

    if (!m_LoggedFirstSubmit)
    {
        m_LoggedFirstSubmit = true;
        Game::logMsg("OpenVR Submit %s src=%ux%u eye=%ux%u stereoCopy=%d off=%ld namedRT=%d eL=%d eR=%d",
            directEyes ? "direct-eyes" : "capture",
            srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height,
            m_StereoCopyOffset ? 1 : 0, usedOff, m_UsedNamedRenderTargets ? 1 : 0, (int)eL, (int)eR);
    }
    else if ((m_SubmitCount % 120) == 0)
    {
        Game::logMsg("OpenVR submit #%d eL=%d eR=%d direct=%d blit=%ux%u eye=%ux%u captured=%ux%u",
            m_SubmitCount, (int)eL, (int)eR, directEyes ? 1 : 0,
            m_LastStereoBlitWidth, m_LastStereoBlitHeight,
            m_RenderWidth, m_RenderHeight, m_FrameCopyWidth, m_FrameCopyHeight);
    }

    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_FrameCopyLatched = false;
    if (eL == vr::VRCompositorError_None || eL == vr::VRCompositorError_AlreadySubmitted)
        m_HasSubmittedSceneFrame.store(true, std::memory_order_release);
    device->Release();
}

void VR::UpdateTracking()
{
    m_HmdPoseValid = false;
    vr::TrackedDevicePose_t hmd{};
    const vr::EVRCompositorError err = static_cast<vr::EVRCompositorError>(
        m_LastPoseWaitError.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        hmd = m_WaitedPoses[vr::k_unTrackedDeviceIndex_Hmd];
    }
    if (m_WaitedPoseTick.load(std::memory_order_acquire) == 0)
        return;
    if (err == vr::VRCompositorError_DoNotHaveFocus)
    {
        if (m_Compositor)
            m_Compositor->CompositorBringToFront();
        static int s_focusLog;
        if (s_focusLog < 6)
        {
            Game::logMsg("WaitGetPoses DoNotHaveFocus (SteamVR waiting room still owns the compositor)");
            ++s_focusLog;
        }
        // Still apply the pose OpenVR returned. Skipping here freezes the
        // game camera at 10Hz while the compositor reprojects — rubberband
        // after one alt-tab.
    }
    else if (err != vr::VRCompositorError_None)
        return;

    if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
        return;

    QAngle ang = HmdMatrixToSourceAnglesWithRoll(hmd.mDeviceToAbsoluteTracking);
    if (!std::isfinite(ang.x) || !std::isfinite(ang.y) || !std::isfinite(ang.z))
        return;

    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HmdAngAbs = ang;
        m_HmdPosAbs = HmdMatrixToSourcePos(hmd.mDeviceToAbsoluteTracking, m_VRScale);
        QAngle::AngleVectors(m_HmdAngAbs, &m_HmdForward, &m_HmdRight, &m_HmdUp);
        m_HmdPoseValid = true;
    }
    m_IpdScale = bmvr::g_IPDScale;
    RefreshIpdFromHmd();
    UpdateControllerTracking(hmd);

    if (m_GameplayEligible)
    {
        const bool openXrLatch = m_OpenXrHelperBridgeActive && m_HmdPoseValid;
        const bool openVrLatch = m_HmdPosAbs.z > 24.f;
        if (!m_HmdOriginLatched && (openXrLatch || openVrLatch))
        {
            m_HmdPosAbsZero = m_HmdPosAbs;
            m_HmdAngAbsZero = m_HmdAngAbs;
            m_HmdOriginLatched = true;
            m_PrevAppliedHmdYaw = m_HmdAngAbs.y;
            m_PrevAppliedHmdPitch = m_HmdAngAbs.x;
            Game::logMsg("ResetPosition latch hmd=(%.1f,%.1f,%.1f) openxr=%d (engine eye + 6DOF, L4D2VR CameraAnchor)",
                m_HmdPosAbs.x, m_HmdPosAbs.y, m_HmdPosAbs.z,
                openXrLatch ? 1 : 0);
        }
        ++m_GameplayFrames;
        if (m_LookApplyWanted)
        {
            m_SafeLookActive = true;
            m_LookApplyEnabled = true;
        }
    }
}

void VR::Update()
{
    if (!m_Game)
        return;

    if (!m_IsVREnabled && m_RequestedRuntimeBackend != VrRuntimeBackend::OpenXR && m_OpenVRInitAttempts < 8)
        m_IsInitialized = InitOpenVR();
    if (!m_IsVREnabled && m_RequestedRuntimeBackend == VrRuntimeBackend::OpenXR && m_RuntimeBackend == VrRuntimeBackend::OpenVR
        && m_OpenVRInitAttempts < 8)
        m_IsInitialized = InitOpenVR();
    if (!m_IsVREnabled)
        return;

    PollMapFromEngine();
    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    // LevelInit names the map before IsInGame. Do not run AutoMatQueueMode,
    // pose, or input during that window — Present is about to stop for
    // material precache, and overnight Update work is not what unblocks it.
    if (m_GameplayEligible && !inGame)
        return;

    if (m_OpenXrHelperBridgeActive && !m_PosesWaitedThisFrame)
        ConsumeOpenXrTracking();

    TickCompositorFocus();

    // QoL cvars go through ICvar from RenderView (game thread). FindVar/SetValue
    // from this Present/DXVK path crashed ~13s after init (2026-08-26).
    // Do not UpdateAutoMatQueueMode from Present. GetThreadMode nested
    // materialsystem after the first stereo frame (x32dbg 2026-08-18).

    ++m_PresentTick;
    m_StereoEyesDrawnThisFrame = false;
    static DWORD s_fpsLogMs;
    static uint32_t s_fpsLogTick;
    static DWORD s_lastPresentMs;
    static DWORD s_lastStallLogMs;
    const DWORD nowMs = GetTickCount();
    if (s_lastPresentMs != 0 && m_GameplayEligible && inGame)
    {
        const DWORD intervalMs = nowMs - s_lastPresentMs;
        if (intervalMs >= 50 && nowMs - s_lastStallLogMs >= 500)
        {
            s_lastStallLogMs = nowMs;
            Game::logMsg("Present stall interval=%ums tick=%u poseAge=%ums handoffSlow=%u poseOvershoot=%u",
                intervalMs, m_PresentTick,
                m_WaitedPoseTick.load(std::memory_order_acquire)
                    ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu,
                m_CompositorHandoffSlowCount,
                m_PoseWaitOvershootCount.load(std::memory_order_relaxed));
        }
    }
    if (s_lastPresentMs != 0 && m_FirstAttackPresentTick > 0 && m_FirstAttackSpikeLogs < 6)
    {
        const uint32_t sinceAttack = m_PresentTick - m_FirstAttackPresentTick;
        if (sinceAttack <= 90)
        {
            const DWORD intervalMs = nowMs - s_lastPresentMs;
            if (intervalMs >= 16)
            {
                ++m_FirstAttackSpikeLogs;
                Game::logMsg("First-shot present spike interval=%ums tick=%u sinceAttack=%u poseAge=%ums weapon=%s",
                    intervalMs, m_PresentTick, sinceAttack,
                    m_WaitedPoseTick.load(std::memory_order_acquire)
                        ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu,
                    m_LastViewmodelModel.empty() ? "?" : m_LastViewmodelModel.c_str());
            }
        }
    }
    // While app handoff is suspended (runtime timing), count consecutive fast
    // presents. Resume probe only after ~300 smooth frames — a blind 10s
    // timer caused 5x70ms stall bursts every 10s in combat.
    if (m_HandoffSuspended.load(std::memory_order_acquire))
    {
        const DWORD interval = s_lastPresentMs ? (nowMs - s_lastPresentMs) : 0;
        if (interval >= 1 && interval <= 12)
        {
            const int fast = m_HandoffFastFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fast == 300)
                Game::logMsg("Compositor: runtime frames smooth; app handoff probe armed");
        }
        else
            m_HandoffFastFrames.store(0, std::memory_order_relaxed);
    }
    s_lastPresentMs = nowMs;
    if (m_PresentTick == 1 || nowMs - s_fpsLogMs >= 1000)
    {
        const uint32_t dt = s_fpsLogMs ? (nowMs - s_fpsLogMs) : 0;
        const uint32_t dn = m_PresentTick - s_fpsLogTick;
        const unsigned fps = (dt > 0) ? (dn * 1000u / dt) : 0;
        Game::logMsg("present tick n=%u ~%ufps inGame=%d eligible=%d map=%s createdRT=%d namedRT=%d stereo=%d direct=%d blit=%ux%u poseWait=%u poseAge=%ums",
            m_PresentTick, fps,
            inGame ? 1 : 0, m_GameplayEligible ? 1 : 0,
            m_CurrentMapName.c_str(),
            m_CreatedVRTextures.load(std::memory_order_acquire) ? 1 : 0,
            m_UsedNamedRenderTargets ? 1 : 0,
            m_StereoRenderViewActive ? 1 : 0,
            m_DirectEyeSubmit ? 1 : 0,
            m_LastStereoBlitWidth, m_LastStereoBlitHeight,
            m_PoseWaitCount.load(std::memory_order_relaxed),
            m_WaitedPoseTick.load(std::memory_order_acquire)
                ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu);
        s_fpsLogMs = nowMs;
        s_fpsLogTick = m_PresentTick;
    }
    if (m_GameplayEligible && inGame && m_EligiblePresents < 100000)
        ++m_EligiblePresents;
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdFramebuffer())
        bmvr::EndRisky(L"hmd_fb");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryFramebufferOverride())
        bmvr::EndRisky(L"fb_override");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryDrawHud())
        bmvr::EndRisky(L"drawhud");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdNative())
        bmvr::EndRisky(L"hmd_native");

    const DWORD poseTickEarly = m_WaitedPoseTick.load(std::memory_order_acquire);
    const DWORD poseAgeEarly = poseTickEarly ? (nowMs - poseTickEarly) : 0xffffffffu;
    TryCompositorPostPresentHandoff(nowMs, poseAgeEarly);
    if (m_Compositor && poseAgeEarly > 80 && poseAgeEarly <= 2000)
    {
        static DWORD s_lastBring = 0;
        if (nowMs - s_lastBring >= 1000)
        {
            m_Compositor->CompositorBringToFront();
            s_lastBring = nowMs;
        }
    }

    if (!m_PosesWaitedThisFrame)
        UpdateTracking();
    ProcessInput();
    m_PosesWaitedThisFrame = false;

    // Capture+Submit on background01 when menu_vr is still enabled. Look/stereo
    // stay gated on real maps. Named RTs / WaitDeviceIdle stay skipped.
    if (!ShouldCompositorSubmit())
        return;

    static int s_menuRisky;
    if (!m_GameplayEligible && s_menuRisky == 0)
    {
        bmvr::BeginRisky(L"menu_vr");
        s_menuRisky = 1;
        Game::logMsg("Menu compositor begin map=%s", m_CurrentMapName.c_str());
    }

    if (g_D3DVR9 && !m_CreatedVRTextures.load(std::memory_order_acquire))
        CreateVRTextures();

    PrepareNamedStereoFromPresent();

    const DWORD poseTick = m_WaitedPoseTick.load(std::memory_order_acquire);
    const DWORD poseAge = poseTick ? (GetTickCount() - poseTick) : 0xffffffffu;
    if (poseTick == 0 || (!m_OpenXrHelperBridgeActive && poseAge > 500))
    {
        static int s_staleLog;
        if (s_staleLog < 6)
        {
            Game::logMsg("Skipping Submit, pose waiter %s age=%ums",
                poseTick == 0 ? "not ready" : "stalled", poseAge);
            ++s_staleLog;
        }
        return;
    }
    SubmitVRTextures();
    if (!m_GameplayEligible && s_menuRisky == 1 && m_SubmitCount >= 120)
    {
        bmvr::EndRisky(L"menu_vr");
        s_menuRisky = 2;
    }
    static int s_updLog;
    if (s_updLog < 3)
    {
        Game::logMsg("Update done poses=%d created=%d", m_HmdPoseValid ? 1 : 0,
            m_CreatedVRTextures.load() ? 1 : 0);
        ++s_updLog;
    }
}

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression()
{
}
