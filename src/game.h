#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <string>
#include <cstdarg>
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
class C_BaseEntity;
class IMaterial;
struct model_t;

class Offsets;
class VR;
class Hooks;

inline class Game* g_Game = nullptr;

class Game
{
public:
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    IMaterialSystem* m_MaterialSystem = nullptr;
    IModelInfo* m_ModelInfo = nullptr;
    IModelRender* m_ModelRender = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;
    void* m_Cvar = nullptr;

    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    Offsets* m_Offsets = nullptr;
    VR* m_VR = nullptr;
    Hooks* m_Hooks = nullptr;

    bool m_Initialized = false;
    bool m_SwitchedWeapons = false;
    // Present/DXVK must not call IMaterialSystem. Refresh only from RenderView.
    mutable std::atomic<int> m_CachedMatQueueMode{ 0 };

    float m_AnalogForward = 0.f;
    float m_AnalogSide = 0.f;

    Game();

    void* GetInterface(const char* dllname, const char* interfacename);
    C_BaseEntity* GetClientEntity(int entityIndex);
    C_BaseEntity* GetActiveWeaponEntity();
    const char* GetActiveWeaponModelName();
    int CycleWeaponSelect(int direction);
    bool ClientCmd(const char* szCmdString);
    bool ClientCmd_Unrestricted(const char* szCmdString);
    int GetMatQueueMode() const;
    void ProbeMatQueueModeFromRenderView();
    bool SetMatQueueMode(int mode) const;
    bool SetConVarInt(const char* name, int value) const;
    bool SetConVarFloat(const char* name, float value) const;
    bool MaterialVTableMatchesDump() const;

    static void logMsg(const char* fmt, ...);
    static void errorMsg(const char* msg);
    static bool InstallVertexFormatWarningFilter() { return false; }
    static void UninstallVertexFormatWarningFilter() {}
};

#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif
