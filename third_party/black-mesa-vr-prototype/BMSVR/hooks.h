#pragma once
#include <iostream>
#include "MinHook.h"

class Game;
class VR;
class ITexture;
class CViewSetup;
class CUserCmd;
class QAngle;
class Vector;
class ModelRenderInfo_t;

template <typename T>
struct Hook
{
    T fOriginal = nullptr;
    LPVOID pTarget = nullptr;
    bool isEnabled = false;

    int createHook(LPVOID targetFunc, LPVOID detourFunc)
    {
        if (!targetFunc)
            return 1;
        if (MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<LPVOID *>(&fOriginal)) != MH_OK)
            return 1;
        pTarget = targetFunc;
        return 0;
    }

    int enableHook()
    {
        if (!pTarget)
            return 1;
        if (MH_EnableHook(pTarget) != MH_OK)
            return 1;
        isEnabled = true;
        return 0;
    }
};

typedef ITexture *(__thiscall *tGetRenderTarget)(void *thisptr);
// Black Mesa: 3-arg RenderView (no separate hudViewSetup)
typedef void(__thiscall *tRenderView)(void *thisptr, CViewSetup &setup, int nClearFlags, int whatToDraw);
typedef bool(__thiscall *tCreateMove)(void *thisptr, float flInputSampleTime, CUserCmd *cmd);
// ClientModeShared::LevelInit(const char* newmap) / LevelShutdown() — VR eligibility gate
typedef void(__thiscall *tLevelInit)(void *thisptr, const char *newmap);
typedef void(__thiscall *tLevelShutdown)(void *thisptr);
// C_BaseViewModel::CalcViewModelView(owner, eyePosition, eyeAngles) — retn 0x0C
typedef void(__thiscall *tCalcViewModelView)(void *thisptr, void *owner, const Vector &eyePosition, const QAngle &eyeAngles);
// ClientMode thiscall: this in ecx, four int* on stack (ret 0x10 stub on BM).
typedef void(__thiscall *tAdjustEngineViewport)(void *thisptr, int &x, int &y, int &width, int &height);
typedef void(__thiscall *tViewport)(void *thisptr, int x, int y, int width, int height);
typedef void(__thiscall *tGetViewport)(void *thisptr, int &x, int &y, int &width, int &height);
typedef void(__thiscall *tDrawModelExecute)(void *thisptr, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld);
typedef void(__thiscall *tPushRenderTargetAndViewport)(void *thisptr, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
typedef void(__thiscall *tPopRenderTargetAndViewport)(void *thisptr);
typedef void(__thiscall *tVgui_Paint)(void *thisptr, int mode);

class Hooks
{
public:
    static inline Game *m_Game;
    static inline VR *m_VR;

    static inline Hook<tGetRenderTarget> hkGetRenderTarget;
    static inline Hook<tRenderView> hkRenderView;
    static inline Hook<tCreateMove> hkCreateMove;
    static inline Hook<tLevelInit> hkLevelInit;
    static inline Hook<tLevelShutdown> hkLevelShutdown;
    static inline Hook<tCalcViewModelView> hkCalcViewModelView;
    static inline Hook<tAdjustEngineViewport> hkAdjustEngineViewport;
    static inline Hook<tViewport> hkViewport;
    static inline Hook<tGetViewport> hkGetViewport;
    static inline Hook<tDrawModelExecute> hkDrawModelExecute;
    static inline Hook<tPushRenderTargetAndViewport> hkPushRenderTargetAndViewport;
    static inline Hook<tPopRenderTargetAndViewport> hkPopRenderTargetAndViewport;
    static inline Hook<tVgui_Paint> hkVgui_Paint;

    Hooks() = default;
    explicit Hooks(Game *game);
    ~Hooks();

    int initSourceHooks();

    static ITexture *__fastcall dGetRenderTarget(void *ecx, void *edx);
    static void __fastcall dRenderView(void *ecx, void *edx, CViewSetup &setup, int nClearFlags, int whatToDraw);
    static bool __fastcall dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd);
    static void __fastcall dLevelInit(void *ecx, void *edx, const char *newmap);
    static void __fastcall dLevelShutdown(void *ecx, void *edx);
    static void __fastcall dCalcViewModelView(void *ecx, void *edx, void *owner, const Vector &eyePosition, const QAngle &eyeAngles);
    static void __fastcall dAdjustEngineViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height);
    static void __fastcall dViewport(void *ecx, void *edx, int x, int y, int width, int height);
    static void __fastcall dGetViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height);
    static void __fastcall dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld);
    static void __fastcall dPushRenderTargetAndViewport(void *ecx, void *edx, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
    static void __fastcall dPopRenderTargetAndViewport(void *ecx, void *edx);
    static void __fastcall dVGui_Paint(void *ecx, void *edx, int mode);
};
