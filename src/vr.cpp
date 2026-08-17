#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "offsets.h"
#include "MinHook.h"
#include "d3d9_vr.h"
#include "bmvr_flags.h"
#include "in_buttons.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using tPresent = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using tSetRenderTarget = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);

    tPresent g_OrigPresent = nullptr;
    tSetRenderTarget g_OrigSetRenderTarget = nullptr;
    bool g_DeviceHooksEnabled = false;

    constexpr UINT kIDirect3DDevice9_Present = 17;
    constexpr UINT kIDirect3DDevice9_SetRenderTarget = 37;

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

    Vector HmdMatrixToSourcePos(const vr::HmdMatrix34_t& mat, float scale)
    {
        Vector pos;
        pos.x = -mat.m[2][3] * scale;
        pos.y = -mat.m[0][3] * scale;
        pos.z = mat.m[1][3] * scale;
        return pos;
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
        if (index == 0 && g_Game && g_Game->m_VR && device && !g_Game->m_VR->m_CaptureReentry)
        {
            IDirect3DSurface9* oldRt = nullptr;
            D3DVIEWPORT9 vp{};
            device->GetRenderTarget(0, &oldRt);
            const bool haveVp = SUCCEEDED(device->GetViewport(&vp));
            if (oldRt && oldRt != rt)
            {
                g_Game->m_VR->CaptureGameColorOnUnbind(
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
        return g_OrigSetRenderTarget(device, index, rt);
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
}

VR::VR(Game* game)
    : m_Game(game)
{
    Game::logMsg("VR ctor: L4D2VR OpenVR + IDirect3DVR9 path (Black Mesa capture)");
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
        m_CurrentMapName.clear();
        return;
    }
    OnLevelInit(map);
}

void VR::OnLevelInit(const char* newmap)
{
    m_CurrentMapName = newmap ? newmap : "";
    m_GameplayEligible = IsGameplayMapName(newmap);
    m_SeenGameplay = false;
    m_GameplayFrames = 0;
    m_EligiblePresents = 0;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_HmdOriginLatched = false;
    m_PassThroughMainViews = 0;
    Game::logMsg("LevelInit map=%s eligible=%d", m_CurrentMapName.c_str(), m_GameplayEligible ? 1 : 0);
}

bool VR::ShouldCompositorSubmit() const
{
    if (!m_IsVREnabled)
        return false;
    if (m_GameplayEligible)
        return true;
    // background01 / menu. Pre-LevelInit StretchRect crashed; require a map name.
    return bmvr::TryMenuCompositor() && !m_CurrentMapName.empty();
}

void VR::OnLevelShutdown()
{
    m_GameplayEligible = false;
    m_SeenGameplay = false;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_DirectEyeSubmit = false;
    m_StereoRenderViewActive = false;
    m_PassThroughMainViews = 0;
    Game::logMsg("LevelShutdown");
}

void VR::HandleMissingRenderContext(const char* location)
{
    Game::logMsg("Missing render context at %s", location ? location : "?");
}

bool VR::InitOpenVR()
{
    ++m_OpenVRInitAttempts;

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
    RefreshIpdFromHmd();

    float l_left = 0, l_right = 0, l_top = 0, l_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);
    float r_left = 0, r_right = 0, r_top = 0, r_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    const float tanHalfFovX = (std::max)({ -l_left, l_right, -r_left, r_right });
    const float tanHalfFovY = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });

    // Same projection crop as L4D2VR, then swap vMin/vMax for Vulkan (OpenVR docs).
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
    m_Compositor->SetExplicitTimingMode(
        vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
    m_Compositor->CompositorBringToFront();
    SetActionManifest();
    StartPoseWaiter();
    Game::logMsg("OpenVR scene app ready compositor=%p canRender=%d fov=%.1f aspect=%.3f",
        (void*)m_Compositor, m_Compositor->CanRenderScene() ? 1 : 0, m_Fov, m_Aspect);
    return true;
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
    grab("/actions/main/in/Flashlight", &m_ActionFlashlight);
    grab("/actions/main/in/Scoreboard", &m_ActionScoreboard);
    grab("/actions/main/in/Pause", &m_ActionPause);

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
    if (!m_Input || handle == vr::k_ulInvalidActionHandle)
        return false;
    const vr::EVRInputError result = m_Input->GetDigitalActionData(
        handle, &out, sizeof(out), vr::k_ulInvalidInputValueHandle);
    return result == vr::VRInputError_None;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const
{
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
        }
        else if (!m_PressedTurn && stickX < -0.5f)
        {
            offset += bmvr::g_SnapTurnAngle;
            m_PressedTurn = true;
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
    if (!m_IsVREnabled || !m_ActionsReady.load(std::memory_order_acquire) || !m_Input)
        return;

    static auto s_prev = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float deltaMs = std::chrono::duration<float, std::milli>(now - s_prev).count();
    s_prev = now;
    if (!(deltaMs > 0.f) || deltaMs > 250.f)
        deltaMs = 16.f;

    static bool s_next, s_prevItem, s_flash, s_pause, s_reset;
    if (!m_GameplayEligible)
    {
        m_ProcessInputEnabled = false;
        m_WalkForward.store(0.f, std::memory_order_release);
        m_WalkSide.store(0.f, std::memory_order_release);
        m_HeldButtons.store(0, std::memory_order_release);
        s_next = s_prevItem = s_flash = s_pause = s_reset = false;
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

    bool usedAnalogTurn = false;
    if (GetAnalogActionData(m_ActionTurn, analog))
    {
        ApplyTurnStick(analog.x, deltaMs);
        usedAnalogTurn = true;
    }
    if (!usedAnalogTurn)
    {
        if (PressedDigitalAction(m_ActionBooleanTurnLeft))
            ApplyTurnStick(-1.f, deltaMs);
        else if (PressedDigitalAction(m_ActionBooleanTurnRight))
            ApplyTurnStick(1.f, deltaMs);
        else
            ApplyTurnStick(0.f, deltaMs);
    }

    uint32_t buttons = 0;
    if (PressedDigitalAction(m_ActionPrimaryAttack))
        buttons |= IN_ATTACK;
    if (PressedDigitalAction(m_ActionSecondaryAttack))
        buttons |= IN_ATTACK2;
    if (PressedDigitalAction(m_ActionJump))
        buttons |= IN_JUMP;
    if (PressedDigitalAction(m_ActionUse))
        buttons |= IN_USE;
    if (PressedDigitalAction(m_ActionReload))
        buttons |= IN_RELOAD;
    if (PressedDigitalAction(m_ActionCrouch))
        buttons |= IN_DUCK;
    if (ny > 0.5f)
        buttons |= IN_FORWARD;
    else if (ny < -0.5f)
        buttons |= IN_BACK;
    if (nx > 0.5f)
        buttons |= IN_MOVERIGHT;
    else if (nx < -0.5f)
        buttons |= IN_MOVELEFT;
    m_HeldButtons.store(buttons, std::memory_order_release);

    const bool nextHeld = PressedDigitalAction(m_ActionNextItem);
    const bool prevHeld = PressedDigitalAction(m_ActionPrevItem);
    const bool flashHeld = PressedDigitalAction(m_ActionFlashlight);
    const bool pauseHeld = PressedDigitalAction(m_ActionPause);
    const bool resetHeld = PressedDigitalAction(m_ActionResetPosition);
    // Do not ClientCmd from Present. mat_queue_mode from RenderView crashed BM;
    // impulse 100 / gameui_activate from this path crashed on G2 stick-click and
    // left menu (Y/Pause, or menu remapped to X/Scoreboard).
    if (flashHeld && !s_flash)
    {
        m_PendingImpulse.store(100, std::memory_order_release);
        Game::logMsg("Flashlight queued on CreateMove (impulse 100)");
    }
    if (nextHeld && !s_next)
        m_PendingInvDelta.store(1, std::memory_order_release);
    if (prevHeld && !s_prevItem)
        m_PendingInvDelta.store(-1, std::memory_order_release);
    if (pauseHeld && !s_pause)
    {
        m_PendingPause.store(1, std::memory_order_release);
        Game::logMsg("Pause queued on CreateMove (gameui_activate)");
    }
    if (resetHeld && !s_reset)
    {
        m_HmdOriginLatched = false;
        Game::logMsg("ResetPosition: cleared HMD origin latch");
    }
    s_next = nextHeld;
    s_prevItem = prevHeld;
    s_flash = flashHeld;
    s_pause = pauseHeld;
    s_reset = resetHeld;

    static int s_inLog;
    if (s_inLog < 8 && (fabsf(nx) > 0.1f || fabsf(ny) > 0.1f || buttons != 0))
    {
        Game::logMsg("VR input walk=(%.2f,%.2f) buttons=0x%x turnOff=%.1f",
            ny, nx, buttons, m_RotationOffsetY.load(std::memory_order_relaxed));
        ++s_inLog;
    }
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
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
    {
        if (m_RenderWidth != fbW || m_RenderHeight != fbH)
            Game::logMsg("Eye/G-buffer size %ux%u (CreateDevice HMD-aspect, window %ux%u recommended %ux%u aspect=%.3f)",
                fbW, fbH, KnownWindowWidth(), KnownWindowHeight(),
                bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight, m_Aspect);
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

    const uint32_t recW = bmvr::g_RecommendedEyeWidth;
    const uint32_t recH = bmvr::g_RecommendedEyeHeight;
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
    float yaw = m_HmdAngAbs.y + m_RotationOffsetY.load(std::memory_order_acquire);
    yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
    return Vector(m_HmdAngAbs.x, yaw, 0.f);
}

void VR::GetViewBasis(Vector* forward, Vector* right, Vector* up) const
{
    const Vector va = GetViewAngle();
    QAngle ang(va.x, va.y, va.z);
    QAngle::AngleVectors(ang, forward, right, up);
}

Vector VR::GetViewOrigin(const Vector& setupOrigin) const
{
    // Portal 2: player eye + HMD 6DOF. L4D2VR VectorPivotXY applies stick yaw
    // to the tracking delta so snap-turn does not leave room-scale offset in
    // unrotated playspace (rubberband). IPD/forward use GetViewAngle, not the
    // raw un-offset m_HmdRight from UpdateTracking.
    Vector center = setupOrigin;
    if (m_HmdOriginLatched)
    {
        Vector delta = m_HmdPosAbs - m_HmdPosAbsZero;
        const float yaw = m_RotationOffsetY.load(std::memory_order_acquire);
        const float rad = yaw * (3.14159265f / 180.f);
        const float s = sinf(rad);
        const float c = cosf(rad);
        const float nx = delta.x * c - delta.y * s;
        const float ny = delta.x * s + delta.y * c;
        delta.x = nx;
        delta.y = ny;
        center += delta;
    }
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

float VR::HorizontalFovForAspect(float targetAspect) const
{
    if (!(m_Fov > 10.f) || !(m_Aspect > 0.1f) || !(targetAspect > 0.1f))
        return m_Fov;
    const float halfRad = m_Fov * 0.5f * (3.14159265358979323846f / 180.0f);
    const float tanHalfY = tanf(halfRad) / m_Aspect;
    return 2.0f * atanf(tanHalfY * targetAspect) * (180.0f / 3.14159265358979323846f);
}

void VR::WaitPosesForStereoFrame()
{
    if (m_PosesWaitedThisFrame)
        return;
    UpdateTracking();
    m_PosesWaitedThisFrame = true;
}

void VR::StartPoseWaiter()
{
    if (m_PoseWaiterThread)
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
    return holder.m_VulkanData.m_nImage != 0;
}

void VR::ReleaseVRRenderTargetsForDeviceReset()
{
    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
    ReleaseT(m_D9LeftEyeSurface);
    ReleaseT(m_D9RightEyeSurface);
    ReleaseT(m_D9LeftEyeTexture);
    ReleaseT(m_D9RightEyeTexture);
    ReleaseT(m_D9FrameColorSurface);
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
    if (MH_EnableHook(vtbl[kIDirect3DDevice9_Present]) != MH_OK ||
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetRenderTarget]) != MH_OK)
    {
        Game::logMsg("MH_EnableHook D3D present/RT failed");
        return;
    }

    m_D3DHooksInstalled = true;
    g_DeviceHooksEnabled = true;
    Game::logMsg("D3D9 Present + SetRenderTarget hooks installed (pre-Present capture)");
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

    // Match the D3D swapchain. With hmd_fb that is HMD aspect (e.g. 1580x1440)
    // inside a 16:9 HWND — not recommended 3k, and not the HWND client size.
    UINT w = KnownWindowWidth();
    UINT h = KnownWindowHeight();
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
    {
        w = fbW;
        h = fbH;
    }
    if (w < 640 || h < 360)
        return false;

    if (m_CreatedVRTextures.load(std::memory_order_acquire) && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth == w && m_VKLeftEye.m_VulkanData.m_nHeight == h)
        return true;

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
        if (!FillSharedTexture(*surf, vk))
        {
            Game::logMsg("GetVRDesc failed for eye id=%d surf=%p", (int)id, (void*)*surf);
            return false;
        }
        Game::logMsg("Eye RT id=%d %ux%u img=%llu", (int)id, vk.m_VulkanData.m_nWidth, vk.m_VulkanData.m_nHeight,
            (unsigned long long)vk.m_VulkanData.m_nImage);
        return true;
    };

    const bool leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
    const bool rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
    EnsureFrameCopySurface(device, w, h);

    m_CreatedVRTextures.store(leftOk && rightOk && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage,
        std::memory_order_release);
    Game::logMsg("VR D3D eye RTs ready=%d L=%p R=%p copy=%p %ux%u",
        m_CreatedVRTextures.load() ? 1 : 0,
        (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface, (void*)m_D9FrameColorSurface, w, h);
    return m_CreatedVRTextures.load();
}

bool VR::EnsureStereoEyeSurfaces()
{
    ChooseEyeRenderSize();
    const UINT w = m_RenderWidth;
    const UINT h = m_RenderHeight;
    if (w < 640 || h < 360 || !g_D3DVR9)
        return false;
    if (m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth == w && m_VKLeftEye.m_VulkanData.m_nHeight == h)
        return true;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
    // Do not Release capture-sized eyes the compositor may still hold.
    m_D9LeftEyeSurface = nullptr;
    m_D9RightEyeSurface = nullptr;
    m_D9LeftEyeTexture = nullptr;
    m_D9RightEyeTexture = nullptr;

    auto createEye = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
        m_CreatingTextureID = id;
        const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*tex)
        {
            Game::logMsg("Stereo eye CreateTexture id=%d hr=0x%08X %ux%u", (int)id, (unsigned)hr, w, h);
            return false;
        }
        (*tex)->GetSurfaceLevel(0, surf);
        if (!*surf || !FillSharedTexture(*surf, vk))
        {
            Game::logMsg("Stereo eye GetVRDesc failed id=%d", (int)id);
            return false;
        }
        return true;
    };

    const bool leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
    const bool rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
    m_CreatedVRTextures.store(leftOk && rightOk && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage,
        std::memory_order_release);
    Game::logMsg("Stereo HMD-aspect D3D eyes ready=%d %ux%u L=%p R=%p",
        m_CreatedVRTextures.load() ? 1 : 0, w, h,
        (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface);
    device->Release();
    return m_CreatedVRTextures.load();
}

bool VR::StereoEyesReady() const
{
    return m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_RenderWidth >= 640 && m_RenderHeight >= 360
        && m_VKLeftEye.m_VulkanData.m_nWidth == m_RenderWidth
        && m_VKLeftEye.m_VulkanData.m_nHeight == m_RenderHeight;
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
}

void VR::BeginStereoEyeBlit(IDirect3DSurface9* dst)
{
    m_StereoEyeBlitDest = dst;
    m_StereoEyeBlitActive = dst != nullptr;
    m_StereoEyeBlitOk = false;
}

bool VR::EndStereoEyeBlit()
{
    const bool ok = m_StereoEyeBlitOk;
    m_StereoEyeBlitActive = false;
    m_StereoEyeBlitDest = nullptr;
    return ok;
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
    if (oldRt == m_StereoEyeBlitDest || oldRt == m_D9LeftEyeSurface || oldRt == m_D9RightEyeSurface
        || oldRt == m_D9FrameColorSurface || oldRt == m_D9BlankSurface)
        return;
    if (m_StereoEyeBlitOk)
        return;

    UINT w = 0, h = 0;
    D3DSURFACE_DESC desc{};
    if (!ResolveSurfaceSize(oldRt, w, h, &desc) || w < 640 || h < 360)
        return;
    if (desc.Format == D3DFMT_D16 || desc.Format == D3DFMT_D24S8 || desc.Format == D3DFMT_D24X8
        || desc.Format == D3DFMT_D32 || desc.Format == D3DFMT_D24FS8)
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
    const HRESULT hr = device->StretchRect(oldRt, nullptr, m_StereoEyeBlitDest, nullptr, D3DTEXF_NONE);
    m_CaptureReentry = false;
    device->Release();

    if (SUCCEEDED(hr))
    {
        m_StereoEyeBlitOk = true;
        m_LastStereoBlitWidth = w;
        m_LastStereoBlitHeight = h;
        static int s_unbindBlitLog;
        if (s_unbindBlitLog < 8)
        {
            Game::logMsg("Stereo unbind blit %ux%u fmt=%u -> eye (HMD-aspect G-buffer)",
                w, h, (unsigned)desc.Format);
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

bool VR::BlitCurrentGameColorTo(IDirect3DSurface9* dst)
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
        const HRESULT hr = device->StretchRect(src, srcPtr, dst, nullptr, D3DTEXF_NONE);
        m_CaptureReentry = false;
        ok = SUCCEEDED(hr);
        static int s_blitLog;
        if (s_blitLog < 8)
        {
            Game::logMsg("Stereo blit fallback RT0/bb %ux%u crop=%ux%u hr=0x%08X (want HMD %ux%u)",
                w, h, cropW, cropH, (unsigned)hr, m_RenderWidth, m_RenderHeight);
            ++s_blitLog;
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
    if (!m_Compositor || !g_D3DVR9 || !m_IsVREnabled)
        return;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;
    if (m_LastPoseWaitError.load(std::memory_order_acquire) ==
        static_cast<int>(vr::VRCompositorError_DoNotHaveFocus))
        return;

    const bool directEyes = m_DirectEyeSubmit && m_D9LeftEyeSurface && m_D9RightEyeSurface;
    const bool haveFrame = m_RenderedNewFrame.load(std::memory_order_acquire) && m_D9FrameColorSurface;
    if (!directEyes && !haveFrame)
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
            }
            else
            {
                hrL = device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = m_D9RightEyeSurface
                    ? device->StretchRect(m_D9FrameColorSurface, nullptr, m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR)
                    : hrL;
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
    const BOOL waitGpu = bmvr::TryWaitDeviceIdle() ? TRUE : FALSE;
    if (bmvr::TryWaitDeviceIdle())
        bmvr::BeginRisky(L"wait_idle");
    const bool okL = SUCCEEDED(g_D3DVR9->TransferSurface(m_D9LeftEyeSurface, waitGpu)) && FillSharedTexture(m_D9LeftEyeSurface, m_VKLeftEye);
    bool okR = false;
    if (m_D9RightEyeSurface && SUCCEEDED(hrR))
        okR = SUCCEEDED(g_D3DVR9->TransferSurface(m_D9RightEyeSurface, waitGpu)) && FillSharedTexture(m_D9RightEyeSurface, m_VKRightEye);
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
    const bool hmdAspectRt = imgAspect > 0.1f && imgAspect <= m_Aspect * 1.02f;

    if (directEyes && hmdAspectRt)
    {
        leftBounds = m_TextureBounds[0];
        rightBounds = m_TextureBounds[1];
        // Do not ApplyVulkanYFlip. Capture Submit {0,0,1,1} with the same
        // StretchRect→TransferSurface path is upright (old UI). That flip on
        // HMD-aspect stereo inverted a fused image (2026-08-17).
        static int s_directBoundsLog;
        if (s_directBoundsLog < 3)
        {
            Game::logMsg("Direct HMD-aspect Submit UV L u=%.3f..%.3f v=%.3f..%.3f (no Vulkan v-flip)",
                leftBounds.uMin, leftBounds.uMax, leftBounds.vMin, leftBounds.vMax);
            ++s_directBoundsLog;
        }
    }
    else if (directEyes && m_UsedNamedRenderTargets && imgAspect > m_Aspect * 1.02f)
    {
        // Named RT matches the 16:9 G-buffer. Crop to HMD aspect, then apply
        // OpenVR projection UVs inside that crop so the center is the real
        // HMD frustum (same math as L4D2VR on an HMD-aspect eye).
        const float vis = m_Aspect / imgAspect;
        const float du = (1.f - vis) * 0.5f;
        auto mapU = [du](const vr::VRTextureBounds_t& proj) {
            vr::VRTextureBounds_t out = proj;
            const float span = 1.f - 2.f * du;
            out.uMin = du + span * proj.uMin;
            out.uMax = du + span * proj.uMax;
            return out;
        };
        leftBounds = mapU(m_TextureBounds[0]);
        rightBounds = mapU(m_TextureBounds[1]);
        static int s_namedBoundsLog;
        if (s_namedBoundsLog < 3)
        {
            Game::logMsg("Named 16:9 Submit UV L u=%.3f..%.3f v=%.3f..%.3f img=%gx%g hmdAspect=%.3f",
                leftBounds.uMin, leftBounds.uMax, leftBounds.vMin, leftBounds.vMax,
                imgW, imgH, m_Aspect);
            ++s_namedBoundsLog;
        }
    }
    else
    {
        // 16:9 game blit is not an HMD-projection eye RT. GetProjectionRaw UVs
        // showed the left strip of the pause menu, 180° rotated, rest black
        // (WMR portal 2026-08-16). Keep v unflipped. Crop U so 16:9 matches
        // HMD aspect (~1.1) instead of stretching vertically into the FOV.
        leftBounds = { 0.f, 0.f, 1.f, 1.f };
        if (imgAspect > m_Aspect * 1.02f && m_Aspect > 0.1f)
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

    g_D3DVR9->UnlockSubmissionQueue();

    m_ActualCompositorSubmitCount.fetch_add(1, std::memory_order_relaxed);
    ++m_SubmitCount;

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
        return;
    }
    if (err != vr::VRCompositorError_None)
        return;

    if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
        return;

    QAngle ang = HmdMatrixToSourceAngles(hmd.mDeviceToAbsoluteTracking);
    if (ang.x > 89.f) ang.x = 89.f;
    if (ang.x < -89.f) ang.x = -89.f;
    ang.z = 0.f;
    if (!std::isfinite(ang.x) || !std::isfinite(ang.y))
        return;

    m_HmdAngAbs = ang;
    m_HmdPosAbs = HmdMatrixToSourcePos(hmd.mDeviceToAbsoluteTracking, m_VRScale);
    QAngle::AngleVectors(m_HmdAngAbs, &m_HmdForward, &m_HmdRight, &m_HmdUp);
    m_HmdPoseValid = true;
    RefreshIpdFromHmd();

    if (m_GameplayEligible)
    {
        if (!m_HmdOriginLatched)
        {
            m_HmdPosAbsZero = m_HmdPosAbs;
            m_HmdAngAbsZero = m_HmdAngAbs;
            m_HmdOriginLatched = true;
            m_PrevAppliedHmdYaw = m_HmdAngAbs.y;
            m_PrevAppliedHmdPitch = m_HmdAngAbs.x;
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

    if (!m_IsVREnabled && m_OpenVRInitAttempts < 8)
        m_IsInitialized = InitOpenVR();
    if (!m_IsVREnabled)
        return;

    PollMapFromEngine();

    ++m_PresentTick;
    m_StereoEyesDrawnThisFrame = false;
    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    static DWORD s_fpsLogMs;
    static uint32_t s_fpsLogTick;
    const DWORD nowMs = GetTickCount();
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
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdNative())
        bmvr::EndRisky(L"hmd_native");

    const DWORD poseTickEarly = m_WaitedPoseTick.load(std::memory_order_acquire);
    const DWORD poseAgeEarly = poseTickEarly ? (nowMs - poseTickEarly) : 0xffffffffu;
    // PostPresentHandoff can block the same way WaitGetPoses does when WMR
    // stops vsync (headset off). Only hand off after a fresh waiter pose.
    // First WaitGetPoses does not need this call.
    if (m_Compositor && poseTickEarly != 0 && poseAgeEarly <= 500)
        m_Compositor->PostPresentHandoff();

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
    if (poseTick == 0 || poseAge > 500)
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
