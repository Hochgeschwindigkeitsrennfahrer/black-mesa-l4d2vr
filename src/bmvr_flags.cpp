#include "bmvr_flags.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bmvr
{
    uint32_t g_RecommendedEyeWidth = 0;
    uint32_t g_RecommendedEyeHeight = 0;
    bool g_OpenVRInitedFromCreateDevice = false;

    static HMODULE g_Module = nullptr;
    static std::mutex g_LogMutex;
    static bool g_TryHmdSwapchain = true;
    static bool g_TryNamedRT = true;
    static bool g_TryStereoRV = true;
    static bool g_TryWaitIdle = true;
    static bool g_TryAbsView = true;
    static bool g_TryMenuCompositor = true;
    static bool g_TryRelativeHmdLook = true;
    static bool g_Inited = false;
    static bool g_WatchdogStarted = false;
    static char g_Stage[64] = "dll_attach";

    static std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        HMODULE mod = g_Module;
        if (!mod)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleDir), &mod);
        if (!mod)
            return L".";
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::wstring ExeDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::vector<std::wstring> FlagDirs()
    {
        std::vector<std::wstring> dirs;
        const std::wstring exe = ExeDir();
        const std::wstring mod = ModuleDir();
        dirs.push_back(exe);
        if (_wcsicmp(exe.c_str(), mod.c_str()) != 0)
            dirs.push_back(mod);
        return dirs;
    }

    static bool FileExists(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static void WriteUtf8File(const std::wstring& path, const char* text, bool append)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), append ? L"ab" : L"wb");
        if (!f)
            return;
        fputs(text, f);
        if (text[0] && text[strlen(text) - 1] != '\n')
            fputc('\n', f);
        fflush(f);
        fclose(f);
    }

    static void WriteAll(const wchar_t* fileName, const char* text, bool append)
    {
        for (const auto& dir : FlagDirs())
            WriteUtf8File(dir + L"\\" + fileName, text, append);
    }

    static std::wstring SkipPath()
    {
        return ExeDir() + L"\\bmvr_skip.txt";
    }

    static void ApplySkipName(const std::string& name, const char* via)
    {
        if (name == "hmd_swap")
            g_TryHmdSwapchain = false;
        else if (name == "named_rt")
            g_TryNamedRT = false;
        else if (name == "stereo_rv")
            g_TryStereoRV = false;
        else if (name == "wait_idle")
            g_TryWaitIdle = false;
        else if (name == "abs_view")
            g_TryAbsView = false;
        else if (name == "menu_vr")
            g_TryMenuCompositor = false;
        else if (name == "rel_look")
            g_TryRelativeHmdLook = false;
        else
            return;
        Log("Skip %s (%s)", name.c_str(), via ? via : "skip file");
    }

    static void ReadSkipFile(const std::wstring& path)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f)
            return;
        char line[128];
        while (fgets(line, sizeof(line), f))
        {
            char* n = line;
            while (*n == ' ' || *n == '\t')
                ++n;
            for (char* p = n; *p; ++p)
            {
                if (*p == '\r' || *p == '\n' || *p == '#')
                {
                    *p = 0;
                    break;
                }
            }
            if (!*n)
                continue;
            // named_rt was persisted because GetBackBufferFormat's vtable slot
            // is wrong on BM, not because CreateNamed crashed. Retry with the
            // verified RVAs. Crash-sticky bmvr_in_named_rt.flag still wins.
            if (std::strcmp(n, "named_rt") == 0)
            {
                Log("Ignoring named_rt skip-file entry (retry CreateNamedEx by RVA)");
                continue;
            }
            // rel_look sticky was left by a WaitGetPoses hang on the Present
            // thread (BeginRisky still set). That hang is not the look copy.
            if (std::strcmp(n, "rel_look") == 0)
            {
                Log("Ignoring rel_look skip-file entry (WaitGetPoses hang, not look)");
                continue;
            }
            ApplySkipName(n, "bmvr_skip.txt");
        }
        fclose(f);
    }

    void PersistSkip(const char* name, const char* reason)
    {
        if (!name || !name[0])
            return;
        ApplySkipName(name, reason);

        std::string existing;
        FILE* f = nullptr;
        _wfopen_s(&f, SkipPath().c_str(), L"r");
        if (f)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), f))
                existing += buf;
            fclose(f);
        }
        if (existing.find(name) != std::string::npos)
            return;

        char line[256];
        snprintf(line, sizeof(line), "%s\n", name);
        WriteUtf8File(SkipPath(), line, true);
        WriteUtf8File(ModuleDir() + L"\\bmvr_skip.txt", line, true);
        if (reason)
            Log("Persisted skip '%s': %s", name, reason);
    }

    void SetDllModule(HMODULE module)
    {
        g_Module = module;
    }

    HMODULE DllModule()
    {
        return g_Module;
    }

    void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_LogMutex);
        OutputDebugStringA("[BMVR] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        printf("[BMVR] %s\n", buf);

        const std::string utf8 = std::string(buf) + "\n";
        const std::wstring exeLog = ExeDir() + L"\\bmvr_log.txt";
        WriteUtf8File(exeLog, utf8.c_str(), true);
        const std::wstring modLog = ModuleDir() + L"\\bmvr_log.txt";
        if (_wcsicmp(exeLog.c_str(), modLog.c_str()) != 0)
            WriteUtf8File(modLog, utf8.c_str(), true);
    }

    static void WriteHeartbeat()
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "stage=%s pid=%lu tick=%lu\n",
            g_Stage, GetCurrentProcessId(), GetTickCount());
        WriteUtf8File(ExeDir() + L"\\bmvr_heartbeat.txt", buf, false);
    }

    void SetStage(const char* stage)
    {
        if (!stage)
            return;
        strncpy_s(g_Stage, stage, _TRUNCATE);
        WriteHeartbeat();
    }

    static DWORD WINAPI WatchdogThread(LPVOID)
    {
        Sleep(200);
        while (true)
        {
            WriteHeartbeat();
            Sleep(500);
        }
        return 0;
    }

    void StartWatchdog()
    {
        if (g_WatchdogStarted)
            return;
        g_WatchdogStarted = true;
        CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    }

    static void ConsumeIfStuck(const wchar_t* name, bool& enabled, const char* key, const char* label)
    {
        bool stuck = false;
        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            if (!FileExists(path))
                continue;
            stuck = true;
            DeleteFileW(path.c_str());
        }
        if (!stuck)
            return;
        enabled = false;
        PersistSkip(key, "previous launch died while trying it");
        Log("Disabled %s: previous launch died while trying it", label);
    }

    void InitFromDisk()
    {
        if (g_Inited)
            return;
        g_Inited = true;
        StartWatchdog();
        ReadSkipFile(SkipPath());
        ReadSkipFile(ModuleDir() + L"\\bmvr_skip.txt");
        ConsumeIfStuck(L"hmd_swap", g_TryHmdSwapchain, "hmd_swap", "HMD-sized swapchain");
        ConsumeIfStuck(L"named_rt", g_TryNamedRT, "named_rt", "MaterialSystem named eye RTs");
        ConsumeIfStuck(L"stereo_rv", g_TryStereoRV, "stereo_rv", "double RenderView stereo");
        ConsumeIfStuck(L"wait_idle", g_TryWaitIdle, "wait_idle", "WaitDeviceIdle");
        ConsumeIfStuck(L"abs_view", g_TryAbsView, "abs_view", "absolute HMD CViewSetup");
        ConsumeIfStuck(L"menu_vr", g_TryMenuCompositor, "menu_vr", "menu/background compositor Submit");
        ConsumeIfStuck(L"rel_look", g_TryRelativeHmdLook, "rel_look", "relative HMD look on RenderView copy");
        // Verified on this DLL 2026-08-16: CreateDevice + Reset forced 3168x3100,
        // desktop went black, one OpenVR Submit then rt0=null / waiting room.
        if (g_TryHmdSwapchain)
            PersistSkip("hmd_swap", "black desktop + SteamVR waiting room after Reset");
        // TransferSurface(..., TRUE) + WaitDeviceIdle during loading with
        // compositor DoNotHaveFocus coincided with the 1 FPS crash.
        if (g_TryWaitIdle)
            PersistSkip("wait_idle", "WaitDeviceIdle during loading crash");
        // Absolute HMD yaw/pitch on the live CViewSetup replaced the tram's
        // scripted camera (e.g. yaw 96.5 → HMD 14.7) and the headset went
        // black after the load logo. L4D2VR only writes HMD on stereo copies.
        if (g_TryAbsView)
            PersistSkip("abs_view", "absolute HMD on live CViewSetup blacked gameplay after load logo");
        SetStage("init");
    }

    bool TryHmdSwapchain() { return g_TryHmdSwapchain; }
    bool TryNamedRenderTargets() { return g_TryNamedRT; }
    bool TryStereoRenderView() { return g_TryStereoRV; }
    bool TryWaitDeviceIdle() { return g_TryWaitIdle; }
    bool TryAbsoluteHmdView() { return g_TryAbsView; }
    bool TryMenuCompositor() { return g_TryMenuCompositor; }
    bool TryRelativeHmdLook() { return g_TryRelativeHmdLook; }

    void DisableNamedRenderTargets(const char* reason)
    {
        PersistSkip("named_rt", reason);
    }

    void DisableStereoRenderView(const char* reason)
    {
        PersistSkip("stereo_rv", reason);
    }

    void BeginRisky(const wchar_t* name)
    {
        char ascii[64]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, ascii, sizeof(ascii) - 1, nullptr, nullptr);
        char stage[80];
        snprintf(stage, sizeof(stage), "in_%s", ascii);
        SetStage(stage);

        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    }

    void EndRisky(const wchar_t* name)
    {
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_" + name + L".flag").c_str());
        SetStage("ok");
    }
}
