#include "hooks.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "offsets.h"
#include "bmvr_flags.h"
#include <cmath>

Hooks::Hooks(Game* game)
{
    if (MH_Initialize() != MH_OK)
        Game::errorMsg("Failed to init MinHook");

    m_Game = game;
    m_VR = m_Game->m_VR;
    initSourceHooks();

    auto enableIfReady = [](auto& hk, const char* name) {
        if (hk.pTarget && hk.fOriginal)
        {
            if (hk.enableHook() == 0)
                Game::logMsg("Hook enabled: %s", name);
            else
                Game::logMsg("Hook enable failed: %s", name);
        }
        else
            Game::logMsg("Hook skipped: %s", name);
    };

    enableIfReady(hkGetRenderTarget, "GetRenderTarget");
    enableIfReady(hkPushRenderTargetAndViewport, "PushRT");
    enableIfReady(hkPopRenderTargetAndViewport, "PopRT");
    enableIfReady(hkViewport, "Viewport");
    enableIfReady(hkGetViewport, "GetViewport");
    enableIfReady(hkAdjustEngineViewport, "AdjustEngineViewport");
    enableIfReady(hkDrawModelExecute, "DrawModelExecute");
    enableIfReady(hkVgui_Paint, "VGui_Paint");
    enableIfReady(hkRenderView, "RenderView");
    enableIfReady(hkCreateMove, "CreateMove");
    enableIfReady(hkLevelInit, "LevelInit");
    enableIfReady(hkLevelShutdown, "LevelShutdown");
    enableIfReady(hkCalcViewModelView, "CalcViewModelView");
}

Hooks::~Hooks()
{
    MH_Uninitialize();
}

int Hooks::initSourceHooks()
{
    auto& o = *m_Game->m_Offsets;

    if (o.GetRenderTarget.valid)
        hkGetRenderTarget.createHook((LPVOID)o.GetRenderTarget.address, &dGetRenderTarget);
    if (o.RenderView.valid)
        hkRenderView.createHook((LPVOID)o.RenderView.address, &dRenderView);
    // CreateMove trampoline on background01 coincided with a 17s crash after
    // the first menu Submit. Look is applied in RenderView (CViewSetup).
    // LevelInit MinHook crashed inside the original on background01.
    if (o.AdjustEngineViewport.valid)
        hkAdjustEngineViewport.createHook((LPVOID)o.AdjustEngineViewport.address, &dAdjustEngineViewport);
    if (o.Viewport.valid)
        hkViewport.createHook((LPVOID)o.Viewport.address, &dViewport);
    if (o.GetViewport.valid)
        hkGetViewport.createHook((LPVOID)o.GetViewport.address, &dGetViewport);
    if (o.PushRenderTargetAndViewport.valid)
        hkPushRenderTargetAndViewport.createHook((LPVOID)o.PushRenderTargetAndViewport.address, &dPushRenderTargetAndViewport);
    if (o.PopRenderTargetAndViewport.valid)
        hkPopRenderTargetAndViewport.createHook((LPVOID)o.PopRenderTargetAndViewport.address, &dPopRenderTargetAndViewport);

    return 1;
}

ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
    return hkGetRenderTarget.fOriginal(ecx);
}

namespace
{
    thread_local void* g_LastMatContext = nullptr;

    IMatRenderContext* GetRenderContextVerified(bool& releaseWhenDone)
    {
        releaseWhenDone = false;
        if (Hooks::m_Game && Hooks::m_Game->m_MaterialSystem)
        {
            void** vt = *reinterpret_cast<void***>(Hooks::m_Game->m_MaterialSystem);
            if (vt)
            {
                using Fn = IMatRenderContext*(__thiscall*)(void*);
                auto fn = reinterpret_cast<Fn>(vt[0x19C / 4]);
                if (fn)
                {
                    IMatRenderContext* ctx = fn(Hooks::m_Game->m_MaterialSystem);
                    if (ctx)
                    {
                        releaseWhenDone = true;
                        g_LastMatContext = ctx;
                        return ctx;
                    }
                }
            }
        }
        return reinterpret_cast<IMatRenderContext*>(g_LastMatContext);
    }

    void NormalizeViewSetupForVREye(CViewSetup& view, const VR* vr)
    {
        const int eyeWidth = static_cast<int>(vr->m_RenderWidth);
        const int eyeHeight = static_cast<int>(vr->m_RenderHeight);
        view.x = 0;
        view.y = 0;
        view.m_nUnscaledX = 0;
        view.m_nUnscaledY = 0;
        view.width = eyeWidth;
        view.height = eyeHeight;
        view.m_nUnscaledWidth = eyeWidth;
        view.m_nUnscaledHeight = eyeHeight;
        view.fov = vr->m_Fov;
        view.fovViewmodel = vr->m_Fov;
        view.m_flAspectRatio = vr->m_Aspect;
        view.zNear = 6.f;
        view.zNearViewmodel = view.zNear;
    }
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw)
{
    (void)edx;
    if (!hkRenderView.fOriginal)
        return;

    static thread_local int s_depth = 0;
    auto callOriginal = [&](CViewSetup& view, int clearFlags, int drawFlags) {
        ++s_depth;
        hkRenderView.fOriginal(ecx, view, clearFlags, drawFlags);
        --s_depth;
    };

    if (s_depth > 0)
    {
        hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
        return;
    }

    const bool mainView = setup.width >= 640 && setup.height >= 360;

    auto wrap180 = [](float a) {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    };

    auto passThrough = [&]() {
        if (m_VR)
        {
            m_VR->m_DirectEyeSubmit = false;
            m_VR->m_StereoRenderViewActive = false;
        }
        // Relative yaw/pitch on a copy. Absolute HMD on the live setup
        // replaced the tram camera and blacked the headset (abs_view).
        // Desktop will move: capture submits this same RenderView.
        if (m_VR && bmvr::TryRelativeHmdLook() && m_VR->IsGameplayEligible()
            && mainView && m_VR->m_HmdPoseValid && m_VR->m_HmdOriginLatched)
        {
            CViewSetup vrView = setup;
            vrView.angles.y += wrap180(m_VR->m_HmdAngAbs.y - m_VR->m_HmdAngAbsZero.y);
            float pitch = vrView.angles.x + wrap180(m_VR->m_HmdAngAbs.x - m_VR->m_HmdAngAbsZero.x);
            if (pitch > 89.f) pitch = 89.f;
            if (pitch < -89.f) pitch = -89.f;
            vrView.angles.x = pitch;

            static int s_relLog;
            if (s_relLog < 4)
            {
                Game::logMsg("Relative look copy engine=(%.1f,%.1f) hmd=(%.1f,%.1f) out=(%.1f,%.1f)",
                    setup.angles.x, setup.angles.y,
                    m_VR->m_HmdAngAbs.x, m_VR->m_HmdAngAbs.y,
                    vrView.angles.x, vrView.angles.y);
                ++s_relLog;
            }

            static int s_relFrames;
            if (s_relFrames == 0)
                bmvr::BeginRisky(L"rel_look");
            callOriginal(vrView, nClearFlags, whatToDraw);
            ++s_relFrames;
            if (s_relFrames == 120)
                bmvr::EndRisky(L"rel_look");
            return;
        }
        callOriginal(setup, nClearFlags, whatToDraw);
    };

    if (!m_VR || !m_VR->m_IsVREnabled)
    {
        callOriginal(setup, nClearFlags, whatToDraw);
        return;
    }
    if (mainView && m_VR->IsGameplayEligible())
    {
        static int s_setupLog;
        if (s_setupLog < 4)
        {
            Game::logMsg("RenderView setup=%dx%d fov=%.1f aspect=%.3f origin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f) submits=%d",
                setup.width, setup.height, setup.fov, setup.m_flAspectRatio,
                setup.origin.x, setup.origin.y, setup.origin.z,
                setup.angles.x, setup.angles.y, m_VR->m_SubmitCount);
            ++s_setupLog;
        }
    }

    const bool namedReady = m_VR->m_LeftEyeTexture && m_VR->m_RightEyeTexture
        && m_VR->m_D9LeftEyeSurface && m_VR->m_D9RightEyeSurface
        && m_VR->m_UsedNamedRenderTargets;
    const bool originOk = std::isfinite(setup.origin.x) && std::isfinite(setup.origin.y)
        && std::isfinite(setup.origin.z)
        && fabsf(setup.origin.x) < 100000.f && fabsf(setup.origin.y) < 100000.f;
    const bool fovOk = std::isfinite(setup.fov) && setup.fov > 10.f && setup.fov < 170.f;

    if (!bmvr::TryStereoRenderView()
        || !m_VR->IsGameplayEligible()
        || m_VR->m_SubmitCount < 90
        || !mainView
        || !namedReady
        || !originOk
        || !fovOk
        || m_VR->m_RenderWidth < 640
        || m_VR->m_RenderHeight < 360
        || !hkPushRenderTargetAndViewport.fOriginal
        || !hkPopRenderTargetAndViewport.fOriginal)
    {
        passThrough();
        return;
    }

    Game::logMsg("Stereo RenderView begin setup=%dx%d ctxLast=%p submits=%d",
        setup.width, setup.height, g_LastMatContext, m_VR->m_SubmitCount);

    bool releaseCtx = false;
    IMatRenderContext* ctx = GetRenderContextVerified(releaseCtx);
    if (!ctx)
    {
        static int s_ctxLog;
        if (s_ctxLog < 4)
        {
            Game::logMsg("Stereo RenderView missing IMatRenderContext");
            ++s_ctxLog;
        }
        passThrough();
        return;
    }

    CViewSetup leftEyeView = setup;
    CViewSetup rightEyeView = setup;
    NormalizeViewSetupForVREye(leftEyeView, m_VR);
    NormalizeViewSetupForVREye(rightEyeView, m_VR);

    Vector viewAngles = m_VR->GetViewAngle();
    if (!m_VR->m_HmdPoseValid || m_VR->m_HmdForward.IsZero())
    {
        viewAngles.x = setup.angles.x;
        viewAngles.y = setup.angles.y;
        viewAngles.z = setup.angles.z;
    }

    const float ipd = m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale;
    Vector leftOrigin = setup.origin;
    Vector rightOrigin = setup.origin;
    if (m_VR->m_HmdPoseValid && !m_VR->m_HmdRight.IsZero())
    {
        leftOrigin = setup.origin + (m_VR->m_HmdRight * (-(ipd * 0.5f)));
        rightOrigin = setup.origin + (m_VR->m_HmdRight * (ipd * 0.5f));
    }
    leftEyeView.origin = leftOrigin;
    rightEyeView.origin = rightOrigin;
    leftEyeView.angles = viewAngles;
    rightEyeView.angles = viewAngles;

    static bool s_stereoOnce = false;
    if (!s_stereoOnce)
    {
        s_stereoOnce = true;
        bmvr::BeginRisky(L"stereo_rv");
        Game::logMsg("Stereo RenderView copies setup=%dx%d eye=%ux%u origin=(%.1f,%.1f,%.1f) hmdAng=(%.1f,%.1f) ipd=%.2f named=1",
            setup.width, setup.height, m_VR->m_RenderWidth, m_VR->m_RenderHeight,
            setup.origin.x, setup.origin.y, setup.origin.z,
            viewAngles.x, viewAngles.y, ipd);
    }

    m_VR->m_DirectEyeSubmit = true;
    m_VR->m_StereoRenderViewActive = true;
    m_VR->m_DesktopMirrorEnabled = true;

    const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
    const int eyeH = static_cast<int>(m_VR->m_RenderHeight);

    hkPushRenderTargetAndViewport.fOriginal(ctx, m_VR->m_LeftEyeTexture, nullptr, 0, 0, eyeW, eyeH);
    callOriginal(leftEyeView, nClearFlags, whatToDraw);
    hkPopRenderTargetAndViewport.fOriginal(ctx);

    hkPushRenderTargetAndViewport.fOriginal(ctx, m_VR->m_RightEyeTexture, nullptr, 0, 0, eyeW, eyeH);
    callOriginal(rightEyeView, nClearFlags, whatToDraw);
    hkPopRenderTargetAndViewport.fOriginal(ctx);

    m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);

    static bool s_stereoSettled = false;
    if (!s_stereoSettled)
    {
        s_stereoSettled = true;
        bmvr::EndRisky(L"stereo_rv");
    }

    if (releaseCtx)
        ctx->Release();
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
    (void)edx;
    if (!hkCreateMove.fOriginal)
        return false;
    if (!cmd)
        return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    if (m_VR->m_SafeLookActive && m_VR->m_HmdPoseValid && m_VR->m_LookApplyEnabled)
    {
        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        auto clampAbs = [](float v, float lim) {
            if (v > lim) return lim;
            if (v < -lim) return -lim;
            return v;
        };

        float dyaw = wrap180(m_VR->m_HmdAngAbs.y - m_VR->m_PrevAppliedHmdYaw);
        if (fabsf(dyaw) > 0.01f)
            cmd->viewangles.y += dyaw;
        m_VR->m_PrevAppliedHmdYaw = m_VR->m_HmdAngAbs.y;

        if (m_VR->m_SoftPitchLook)
        {
            float dpitch = wrap180(m_VR->m_HmdAngAbs.x - m_VR->m_PrevAppliedHmdPitch);
            dpitch = clampAbs(dpitch, 2.0f);
            if (fabsf(dpitch) > 0.01f)
            {
                cmd->viewangles.x += dpitch;
                if (cmd->viewangles.x > 89.f) cmd->viewangles.x = 89.f;
                if (cmd->viewangles.x < -89.f) cmd->viewangles.x = -89.f;
            }
            m_VR->m_PrevAppliedHmdPitch = m_VR->m_HmdAngAbs.x;
        }
    }

    if (!m_VR->m_ProcessInputEnabled)
        return result;

    const float maxSpeed = 450.f;
    cmd->forwardmove += m_Game->m_AnalogForward * maxSpeed;
    cmd->sidemove += m_Game->m_AnalogSide * maxSpeed;
    return result;
}

void __fastcall Hooks::dLevelInit(void* ecx, void* edx, const char* newmap)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelInit(newmap);
    if (hkLevelInit.fOriginal)
        hkLevelInit.fOriginal(ecx, newmap);
}

void __fastcall Hooks::dLevelShutdown(void* ecx, void* edx)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelShutdown();
    if (hkLevelShutdown.fOriginal)
        hkLevelShutdown.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
    (void)edx;
    if (hkCalcViewModelView.fOriginal)
        hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
}

void __fastcall Hooks::dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    if (hkAdjustEngineViewport.fOriginal)
        hkAdjustEngineViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
    g_LastMatContext = ecx;
    if (hkViewport.fOriginal)
        hkViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    if (hkGetViewport.fOriginal)
        hkGetViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
    if (hkDrawModelExecute.fOriginal)
        hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

void __fastcall Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    if (hkPushRenderTargetAndViewport.fOriginal)
        hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
}

void __fastcall Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (hkPopRenderTargetAndViewport.fOriginal)
        hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void __fastcall Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    if (hkVgui_Paint.fOriginal)
        hkVgui_Paint.fOriginal(ecx, mode);
}
