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
struct ModelRenderInfo_t;

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
        if (MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<LPVOID*>(&fOriginal)) != MH_OK)
            return 1;
        pTarget = targetFunc;
        return 0;
    }

    int enableHook()
    {
        if (!pTarget)
            return 1;
        if (isEnabled)
            return 0;
        if (MH_EnableHook(pTarget) != MH_OK)
            return 1;
        isEnabled = true;
        return 0;
    }
};

typedef ITexture*(__thiscall* tGetRenderTarget)(void* thisptr);
// Black Mesa: 3-arg RenderView (no separate hudViewSetup). L4D2VR is 4-arg.
typedef void(__thiscall* tRenderView)(void* thisptr, CViewSetup& setup, int nClearFlags, int whatToDraw);
typedef bool(__thiscall* tCreateMove)(void* thisptr, float flInputSampleTime, CUserCmd* cmd);
typedef void(__thiscall* tLevelInit)(void* thisptr, const char* newmap);
typedef void(__thiscall* tLevelShutdown)(void* thisptr);
typedef void(__thiscall* tCalcViewModelView)(void* thisptr, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
typedef void(__thiscall* tAdjustEngineViewport)(void* thisptr, int& x, int& y, int& width, int& height);
typedef void(__thiscall* tViewport)(void* thisptr, int x, int y, int width, int height);
typedef void(__thiscall* tGetViewport)(void* thisptr, int& x, int& y, int& width, int& height);
typedef void(__thiscall* tDrawModelExecute)(void* thisptr, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
typedef void(__thiscall* tPushRenderTargetAndViewport)(void* thisptr, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
typedef void(__thiscall* tPopRenderTargetAndViewport)(void* thisptr);
typedef void(__thiscall* tVgui_Paint)(void* thisptr, int mode);
typedef void(__thiscall* tGetBackBufferDimensions)(void* thisptr, int& width, int& height);
typedef void(__thiscall* tGetScreenSize)(void* thisptr, int& width, int& height);
typedef ITexture*(__thiscall* tCreateNamedRTEx)(void* thisptr, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags);

class Hooks
{
public:
    static inline Game* m_Game;
    static inline VR* m_VR;

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
    static inline Hook<tGetBackBufferDimensions> hkGetBackBufferDimensions;
    static inline Hook<tGetScreenSize> hkGetScreenSize;
    static inline Hook<tCreateNamedRTEx> hkCreateNamedRTEx;

    Hooks() = default;
    explicit Hooks(Game* game);
    ~Hooks();

    int initSourceHooks();

    static ITexture* __fastcall dGetRenderTarget(void* ecx, void* edx);
    static void __fastcall dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw);
    static bool __fastcall dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd);
    static void __fastcall dLevelInit(void* ecx, void* edx, const char* newmap);
    static void __fastcall dLevelShutdown(void* ecx, void* edx);
    static void __fastcall dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
    static void __fastcall dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height);
    static void __fastcall dViewport(void* ecx, void* edx, int x, int y, int width, int height);
    static void __fastcall dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height);
    static void __fastcall dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
    static void __fastcall dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
    static void __fastcall dPopRenderTargetAndViewport(void* ecx, void* edx);
    static void __fastcall dVGui_Paint(void* ecx, void* edx, int mode);
    static void __fastcall dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height);
    static void __fastcall dGetScreenSize(void* ecx, void* edx, int& width, int& height);
    static ITexture* __fastcall dCreateNamedRTEx(void* ecx, void* edx, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags);
};
