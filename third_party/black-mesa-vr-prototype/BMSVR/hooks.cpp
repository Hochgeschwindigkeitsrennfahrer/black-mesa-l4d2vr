#include "hooks.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "offsets.h"
#include "d3d9_vr.h"

Hooks::Hooks(Game *game)
{
    if (MH_Initialize() != MH_OK)
        Game::errorMsg("Failed to init MinHook");

    m_Game = game;
    m_VR = m_Game->m_VR;
    initSourceHooks();

    auto enableIfReady = [](auto &hk, const char *name) {
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
    auto &o = *m_Game->m_Offsets;

    if (o.GetRenderTarget.valid())
        hkGetRenderTarget.createHook((LPVOID)o.GetRenderTarget.address, &dGetRenderTarget);
    if (o.RenderView.valid())
        hkRenderView.createHook((LPVOID)o.RenderView.address, &dRenderView);
    if (o.CreateMove.valid())
        hkCreateMove.createHook((LPVOID)o.CreateMove.address, &dCreateMove);
    if (o.LevelInit.valid())
        hkLevelInit.createHook((LPVOID)o.LevelInit.address, &dLevelInit);
    if (o.LevelShutdown.valid())
        hkLevelShutdown.createHook((LPVOID)o.LevelShutdown.address, &dLevelShutdown);
    if (o.CalcViewModelView.valid())
        hkCalcViewModelView.createHook((LPVOID)o.CalcViewModelView.address, &dCalcViewModelView);
    if (o.AdjustEngineViewport.valid())
        hkAdjustEngineViewport.createHook((LPVOID)o.AdjustEngineViewport.address, &dAdjustEngineViewport);
    if (o.Viewport.valid())
        hkViewport.createHook((LPVOID)o.Viewport.address, &dViewport);
    if (o.GetViewport.valid())
        hkGetViewport.createHook((LPVOID)o.GetViewport.address, &dGetViewport);
    if (o.DrawModelExecute.valid())
        hkDrawModelExecute.createHook((LPVOID)o.DrawModelExecute.address, &dDrawModelExecute);
    if (o.PushRenderTargetAndViewport.valid())
        hkPushRenderTargetAndViewport.createHook((LPVOID)o.PushRenderTargetAndViewport.address, &dPushRenderTargetAndViewport);
    if (o.PopRenderTargetAndViewport.valid())
        hkPopRenderTargetAndViewport.createHook((LPVOID)o.PopRenderTargetAndViewport.address, &dPopRenderTargetAndViewport);
    if (o.VGui_Paint.valid())
        hkVgui_Paint.createHook((LPVOID)o.VGui_Paint.address, &dVGui_Paint);

    return 1;
}

ITexture *__fastcall Hooks::dGetRenderTarget(void *ecx, void *edx)
{
    return hkGetRenderTarget.fOriginal(ecx);
}

void __fastcall Hooks::dRenderView(void *ecx, void *edx, CViewSetup &setup, int nClearFlags, int whatToDraw)
{
    if (!hkRenderView.fOriginal)
        return;

    hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
}

bool __fastcall Hooks::dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd)
{
    if (!hkCreateMove.fOriginal)
        return false;

    if (!cmd)
        return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    // Always call original first so game logic runs
    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    // Only latch gameplay on real maps — background* CreateMove must not arm VR.
    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    // Relative look only (clamped). Absolute pitch/yaw replace crashed BM.
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
        dyaw = clampAbs(dyaw, 2.5f);
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

        static bool s_relLog;
        if (!s_relLog)
        {
            s_relLog = true;
            Game::logMsg("CreateMove relative look active (yaw%s)",
                         m_VR->m_SoftPitchLook ? "+soft_pitch" : "");
        }
    }

    if (!m_VR->m_ProcessInputEnabled)
        return result;

    if (m_VR->m_RoomscaleActive)
    {
        Vector delta = m_VR->m_SetupOriginToHMD;
        delta.z = 0;
        float distance = VectorLength(delta);
        if (distance > 1.f)
        {
            float forwardSpeed = DotProduct2D(delta, m_VR->m_HmdForward);
            float sideSpeed = DotProduct2D(delta, m_VR->m_HmdRight);
            cmd->forwardmove += distance * forwardSpeed;
            cmd->sidemove += distance * sideSpeed;
        }
    }

    const float maxSpeed = 450.f;
    cmd->forwardmove += m_Game->m_AnalogForward * maxSpeed;
    cmd->sidemove += m_Game->m_AnalogSide * maxSpeed;

    return result;
}

void __fastcall Hooks::dLevelInit(void *ecx, void *edx, const char *newmap)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelInit(newmap);
    else
        Game::logMsg("LevelInit map='%s' (no VR)", newmap ? newmap : "(null)");
    if (hkLevelInit.fOriginal)
        hkLevelInit.fOriginal(ecx, newmap);
}

void __fastcall Hooks::dLevelShutdown(void *ecx, void *edx)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelShutdown();
    else
        Game::logMsg("LevelShutdown (no VR)");
    if (hkLevelShutdown.fOriginal)
        hkLevelShutdown.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void *ecx, void *edx, void *owner,
                                          const Vector &eyePosition, const QAngle &eyeAngles)
{
    (void)edx;
    // Source signature: eyePos/eyeAng are INPUTS (const refs). BM reads them then
    // SetLocalOrigin/SetLocalAngles on the viewmodel. Post-return tweaks of these
    // refs do nothing useful; mutating the caller's storage is unsafe. When
    // viewmodel_vr is on, adjust local copies and pass those into the original.
    static int s_logs;
    static int s_adjLogs;

    if (!hkCalcViewModelView.fOriginal)
        return;

    const bool wantAdj = m_VR && m_VR->m_ViewmodelVr && m_VR->m_IsVREnabled
                         && m_VR->IsGameplayEligible();

    if (!wantAdj)
    {
        if (s_logs < 8)
        {
            Game::logMsg("CalcViewModelView #%d this=%p owner=%p eye=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)",
                         s_logs, ecx, owner,
                         eyePosition.x, eyePosition.y, eyePosition.z,
                         eyeAngles.x, eyeAngles.y, eyeAngles.z);
            ++s_logs;
        }
        else if (s_logs == 8)
        {
            Game::logMsg("CalcViewModelView hook active (further logs suppressed)");
            ++s_logs;
        }
        hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
        return;
    }

    // Position: prefer controller-relative-to-HMD (tracking delta on top of game eyePos).
    // Never writes absolute HMD into CreateMove/viewangles — local copies only for viewmodel.
    Vector adjPos = eyePosition;
    QAngle adjAng = eyeAngles;
    bool usedCtrl = false;
    Vector ctrlDelta{};

    if (m_VR->m_ViewmodelFollow && m_VR->TryGetControllerRelToHmd(ctrlDelta))
    {
        adjPos = eyePosition + ctrlDelta;
        usedCtrl = true;
    }

    // Fine-tune offsets in the *input* eyeAngles basis (stable; not absolute HMD).
    Vector fwd, right, up;
    QAngle::AngleVectors(eyeAngles, &fwd, &right, &up);
    adjPos += fwd * m_VR->m_ViewmodelOffForward;
    adjPos += right * m_VR->m_ViewmodelOffRight;
    adjPos += up * m_VR->m_ViewmodelOffUp;

    // Optional: controller aim angles for viewmodel SetLocalAngles only (default off).
    const bool usedAim = m_VR->m_ViewmodelAim && m_VR->m_ControllerPoseValid;
    if (usedAim)
        adjAng = m_VR->m_RightControllerAngAbs;

    const char *mode = usedCtrl ? (usedAim ? "ctrl+aim" : "ctrl")
                                : (usedAim ? "const+aim" : "const");

    if (s_adjLogs < 8)
    {
        Game::logMsg("CalcViewModelView VR-adj #%d mode=%s (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) off_f=%.1f ctrl=%d",
                     s_adjLogs, mode,
                     eyePosition.x, eyePosition.y, eyePosition.z,
                     adjPos.x, adjPos.y, adjPos.z,
                     m_VR->m_ViewmodelOffForward,
                     (int)m_VR->m_ControllerPoseValid);
        ++s_adjLogs;
    }
    else if (s_adjLogs == 8)
    {
        Game::logMsg("CalcViewModelView VR-adj active (further logs suppressed)");
        ++s_adjLogs;
    }

    // One-shot when controller follow first succeeds (often after early const-mode logs).
    static bool s_loggedCtrlFollow;
    if (!s_loggedCtrlFollow && usedCtrl)
    {
        s_loggedCtrlFollow = true;
        Game::logMsg("CalcViewModelView VR-adj mode=%s first-ctrl delta=(%.1f,%.1f,%.1f)",
                     mode, ctrlDelta.x, ctrlDelta.y, ctrlDelta.z);
    }

    hkCalcViewModelView.fOriginal(ecx, owner, adjPos, adjAng);
}

void __fastcall Hooks::dAdjustEngineViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height)
{
    (void)edx;
    if (hkAdjustEngineViewport.fOriginal)
        hkAdjustEngineViewport.fOriginal(ecx, x, y, width, height);
    // Only while an eye pass is active — forcing VR size globally after RT create crashes BM.
    if (m_VR && m_VR->m_StereoBindEye != VR::Texture_None)
    {
        width = (int)m_VR->m_RenderWidth;
        height = (int)m_VR->m_RenderHeight;
    }
}

void __fastcall Hooks::dViewport(void *ecx, void *edx, int x, int y, int width, int height)
{
    if (m_VR && m_VR->m_StereoBindEye != VR::Texture_None)
    {
        width = (int)m_VR->m_RenderWidth;
        height = (int)m_VR->m_RenderHeight;
    }
    hkViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dGetViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height)
{
    hkGetViewport.fOriginal(ecx, x, y, width, height);
    if (m_VR && m_VR->m_StereoBindEye != VR::Texture_None)
    {
        width = (int)m_VR->m_RenderWidth;
        height = (int)m_VR->m_RenderHeight;
    }
}

void __fastcall Hooks::dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
    hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

void __fastcall Hooks::dPushRenderTargetAndViewport(void *ecx, void *edx, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
}

void __fastcall Hooks::dPopRenderTargetAndViewport(void *ecx, void *edx)
{
    hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void __fastcall Hooks::dVGui_Paint(void *ecx, void *edx, int mode)
{
    hkVgui_Paint.fOriginal(ecx, mode);
}
