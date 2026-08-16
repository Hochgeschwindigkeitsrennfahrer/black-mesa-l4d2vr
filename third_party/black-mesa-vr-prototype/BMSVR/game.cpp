#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"

#include <unordered_map>
#include <initializer_list>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>

static std::mutex g_LogMutex;
using tCreateInterface = void *(__cdecl *)(const char *name, int *returnCode);

static HMODULE GetModuleWithRetry(const char *dllname, int maxTries = 1800, int delayMs = 100)
{
    // d3d9.dll loads before client.dll in Source — wait up to ~3 minutes.
    for (int i = 0; i < maxTries; ++i)
    {
        if (HMODULE handle = GetModuleHandleA(dllname))
            return handle;
        if (i == 0 || (i + 1) % 50 == 0)
            Game::logMsg("Waiting for %s (%d/%d)", dllname, i + 1, maxTries);
        Sleep(delayMs);
    }
    Game::logMsg("Timed out waiting for %s — VR hooks will be limited", dllname);
    return nullptr;
}

static void *GetInterfaceSafe(const char *dllname, const char *interfacename)
{
    static std::unordered_map<std::string, void *> cache;
    const std::string key = std::string(dllname) + "::" + interfacename;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
    {
        Game::errorMsg(("CreateInterface missing in " + std::string(dllname)).c_str());
        return nullptr;
    }

    int rc = 0;
    void *iface = CreateInterface(interfacename, &rc);
    if (!iface)
        Game::logMsg("Interface not found: %s (will retry variants)", interfacename);
    else
        cache[key] = iface;
    return iface;
}

static void *GetInterfaceAny(const char *dll, std::initializer_list<const char *> names)
{
    for (const char *n : names)
    {
        if (void *p = GetInterfaceSafe(dll, n))
            return p;
    }
    return nullptr;
}

Game::Game()
{
    // Wait for engine first (always present before client in Source load order once menus start),
    // then client — Black Mesa loads client from bms\bin after d3d9 is already injected.
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    if (!m_BaseClient)
    {
        Game::errorMsg("Timed out waiting for client.dll.\nStart a map / load the menu and relaunch if this persists.");
        return;
    }

    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleHandleA("server.dll")); // may load later
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    m_ClientEntityList = static_cast<IClientEntityList *>(GetInterfaceAny("client.dll", { "VClientEntityList003" }));
    m_EngineTrace = static_cast<IEngineTrace *>(GetInterfaceAny("engine.dll", { "EngineTraceClient004", "EngineTraceClient003" }));
    m_EngineClient = static_cast<IEngineClient *>(GetInterfaceAny("engine.dll", { "VEngineClient015", "VEngineClient014", "VEngineClient013" }));
    m_MaterialSystem = static_cast<IMaterialSystem *>(GetInterfaceAny("MaterialSystem.dll", { "VMaterialSystem081", "VMaterialSystem080" }));
    m_ModelInfo = static_cast<IModelInfo *>(GetInterfaceAny("engine.dll", { "VModelInfoClient006", "VModelInfoClient004" }));
    m_ModelRender = static_cast<IModelRender *>(GetInterfaceAny("engine.dll", { "VEngineModel016" }));
    m_VguiInput = static_cast<IInput *>(GetInterfaceAny("vgui2.dll", { "VGUI_InputInternal001", "VGUI_Input005" }));
    m_VguiSurface = static_cast<ISurface *>(GetInterfaceAny("vguimatsurface.dll", { "VGUI_Surface030", "VGUI_Surface031" }));

    m_Offsets = new Offsets();
    m_VR = new VR(this);
    m_Hooks = new Hooks(this);
    m_Initialized = true;
    Game::logMsg("BMSVR Game initialized");
}

void *Game::GetInterface(const char *dllname, const char *interfacename)
{
    return GetInterfaceSafe(dllname, interfacename);
}

CBaseEntity *Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;
    return static_cast<CBaseEntity *>(m_ClientEntityList->GetClientEntity(entityIndex));
}

void Game::ClientCmd(const char *szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char *szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}

void Game::logMsg(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[BMSVR] %s\n", buf);
    if (FILE *f = fopen("bmsvr_log.txt", "a"))
    {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

void Game::errorMsg(const char *msg)
{
    logMsg("[ERROR] %s", msg);
    MessageBoxA(nullptr, msg, "Black Mesa VR", MB_ICONERROR | MB_OK);
}
