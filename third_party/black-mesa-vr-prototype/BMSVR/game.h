#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <Windows.h>
#include "vector.h"

class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IMaterialSystem;
class IModelInfo;
class IModelRender;
class IInput;
class ISurface;
class CBaseEntity;
class IMaterial;
struct model_t;

class Offsets;
class VR;
class Hooks;

inline class Game *g_Game = nullptr;

class Game
{
public:
    IClientEntityList *m_ClientEntityList = nullptr;
    IEngineTrace *m_EngineTrace = nullptr;
    IEngineClient *m_EngineClient = nullptr;
    IMaterialSystem *m_MaterialSystem = nullptr;
    IModelInfo *m_ModelInfo = nullptr;
    IModelRender *m_ModelRender = nullptr;
    IInput *m_VguiInput = nullptr;
    ISurface *m_VguiSurface = nullptr;

    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    Offsets *m_Offsets = nullptr;
    VR *m_VR = nullptr;
    Hooks *m_Hooks = nullptr;

    bool m_Initialized = false;
    model_t *m_ArmsModel = nullptr;
    IMaterial *m_ArmsMaterial = nullptr;
    bool m_CachedArmsModel = false;
    bool m_SwitchedWeapons = false;
    bool m_IsMeleeWeaponActive = false;

    // Analog locomotion from OpenXR (applied in CreateMove)
    float m_AnalogForward = 0.f;
    float m_AnalogSide = 0.f;

    Game();

    void *GetInterface(const char *dllname, const char *interfacename);
    CBaseEntity *GetClientEntity(int entityIndex);
    void ClientCmd(const char *szCmdString);
    void ClientCmd_Unrestricted(const char *szCmdString);

    static void logMsg(const char *fmt, ...);
    static void errorMsg(const char *msg);
};
