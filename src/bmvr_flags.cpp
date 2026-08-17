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
    uint32_t g_FramebufferWidth = 0;
    uint32_t g_FramebufferHeight = 0;
    bool g_OpenVRInitedFromCreateDevice = false;

    static HMODULE g_Module = nullptr;
    static std::mutex g_LogMutex;
    static bool g_TryHmdSwapchain = true;
    static bool g_TryHmdFramebuffer = true;
    static bool g_TryNamedRT = true;
    static bool g_TryNamedStereoWrap = false;
    static bool g_TryStereoRV = true;
    static bool g_TryWaitIdle = true;
    static bool g_TryAbsView = true;
    static bool g_TryMenuCompositor = true;
    static bool g_TryRelativeHmdLook = true;
    static bool g_TryStereoCopy = false;
    static bool g_TryStereoFov = false;
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
        else if (name == "hmd_fb")
            g_TryHmdFramebuffer = false;
        else if (name == "named_rt")
            g_TryNamedRT = false;
        else if (name == "stereo_rv")
            g_TryStereoRV = false;
        else if (name == "named_bind" || name == "named_l4d" || name == "named_eye" || name == "named_push")
            g_TryNamedStereoWrap = false;
        else if (name == "wait_idle")
            g_TryWaitIdle = false;
        else if (name == "abs_view")
            g_TryAbsView = false;
        else if (name == "menu_vr")
            g_TryMenuCompositor = false;
        else if (name == "rel_look")
            g_TryRelativeHmdLook = false;
        else if (name == "stereo_copy")
            g_TryStereoCopy = false;
        else if (name == "stereo_fov")
            g_TryStereoFov = false;
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
            // Named wrap onto HDR leftEye0 died in CSimpleWorldView after
            // three rewritten PushRT(NULL) even at 2560x1440. That skip must
            // not disable double RenderView / HMD-aspect G-buffer.
            if (std::strcmp(n, "hmd_fb") == 0)
            {
                // Matching swapchain still died after LevelInit. 2026-08-17
                // evening: GetScreenSize fix produced RenderView setup=1584,
                // then first stereo left RenderView during spawn crashed.
                // Crash-sticky is cleared without disabling the framebuffer.
                Log("Ignoring hmd_fb skip-file entry (1584 world RV works; stereo wrote height into m_eStereoEye at 0x1C)");
                continue;
            }
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
            // Named-RT stereo_rv / named_push deaths. Retry named_l4d.
            if (std::strcmp(n, "stereo_rv") == 0)
            {
                Log("Ignoring stereo_rv skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Outer L4D2 PushRT(eye) then BM PushRT(NULL,0,0,0,0) died.
            // named_l4d keeps the wrap and rewrites those null PushRTs.
            if (std::strcmp(n, "named_push") == 0)
            {
                Log("Ignoring named_push skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Rewrite-only (no wrap) was not the L4D2 path. RenderView's
            // prologue GetRT / SetRT(+0x18) restore needs the eye already
            // pushed or it puts the backbuffer back.
            if (std::strcmp(n, "named_bind") == 0)
            {
                Log("Ignoring named_bind skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_l4d") == 0)
            {
                Log("Ignoring named_l4d skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_eye") == 0)
            {
                Log("Ignoring named_eye skip-file entry (named PushRT wrap is off; HMD-aspect G-buffer retry)");
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
        // Last launch: 8 pass-through 1584 RenderViews (zNear=7) succeeded,
        // then stereo wrote 1440 into CViewSetup+0x1C (m_eStereoEye on BM).
        // RenderView indexes this+0x744 by that field. Do not disable hmd_fb.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_hmd_fb.flag").c_str());
        ConsumeIfStuck(L"named_rt", g_TryNamedRT, "named_rt", "MaterialSystem named eye RTs");
        ConsumeIfStuck(L"wait_idle", g_TryWaitIdle, "wait_idle", "WaitDeviceIdle");
        ConsumeIfStuck(L"abs_view", g_TryAbsView, "abs_view", "absolute HMD CViewSetup");
        ConsumeIfStuck(L"menu_vr", g_TryMenuCompositor, "menu_vr", "menu/background compositor Submit");
        ConsumeIfStuck(L"rel_look", g_TryRelativeHmdLook, "rel_look", "relative HMD look on RenderView copy");
        ConsumeIfStuck(L"stereo_copy", g_TryStereoCopy, "stereo_copy", "double RenderView blit stereo");
        ConsumeIfStuck(L"stereo_fov", g_TryStereoFov, "stereo_fov", "same-size HMD-FOV double RenderView");
        // 16:9 blit stereo is not fused. Do not keep offering it.
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
        if (g_TryStereoCopy)
            PersistSkip("stereo_copy", "16:9 blit stereo is not fused");
        PersistSkip("stereo_fov", "HMD FOV in 16:9 pixels is magnified and not fused");
        // Named HDR wrap + rewriting CSimpleWorldView PushRT(NULL) dies after
        // the third bind even when the named RT is 2560x1440. Do not retry it.
        g_TryNamedStereoWrap = false;
        SetStage("init");
    }

    bool TryHmdSwapchain() { return g_TryHmdSwapchain; }
    bool TryHmdFramebuffer() { return g_TryHmdFramebuffer; }
    bool TryNamedRenderTargets() { return g_TryNamedRT; }
    bool TryNamedStereoWrap() { return g_TryNamedStereoWrap; }
    bool TryStereoRenderView() { return g_TryStereoRV; }
    bool TryWaitDeviceIdle() { return g_TryWaitIdle; }
    bool TryAbsoluteHmdView() { return g_TryAbsView; }
    bool TryMenuCompositor() { return g_TryMenuCompositor; }
    bool TryRelativeHmdLook() { return g_TryRelativeHmdLook; }
    bool TryStereoCopy() { return g_TryStereoCopy; }
    bool TryStereoFov() { return g_TryStereoFov; }

    void DisableNamedRenderTargets(const char* reason)
    {
        PersistSkip("named_rt", reason);
    }

    void DisableStereoRenderView(const char* reason)
    {
        PersistSkip("stereo_rv", reason);
    }

    void DisableStereoFov(const char* reason)
    {
        PersistSkip("stereo_fov", reason);
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

    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect)
    {
        if (recW < 640 || recH < 360)
            return;
        if (winW < 640)
            winW = recW;
        if (winH < 360)
            winH = recH;
        float aspect = projAspect;
        if (!(aspect > 0.5f && aspect < 3.f))
            aspect = static_cast<float>(recW) / static_cast<float>(recH);
        uint32_t eyeH = winH & ~1u;
        uint32_t eyeW = (static_cast<uint32_t>(static_cast<float>(eyeH) * aspect + 0.5f) + 1u) & ~1u;
        if (eyeW > winW)
        {
            eyeW = winW & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeW) / aspect + 0.5f) + 1u) & ~1u;
        }
        if (eyeW > recW || eyeH > recH)
        {
            const float sx = static_cast<float>(recW) / static_cast<float>(eyeW);
            const float sy = static_cast<float>(recH) / static_cast<float>(eyeH);
            const float s = sx < sy ? sx : sy;
            eyeW = (static_cast<uint32_t>(static_cast<float>(eyeW) * s) + 1u) & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeH) * s) + 1u) & ~1u;
        }
        if (eyeW < 640 || eyeH < 360)
            return;
        // Deferred MRT / HUD downsample want 16-pixel pitch. 1576 (1440*1.097
        // rounded even) is 1576%16=8. Menu 2D survived; first 3D/HUD did not.
        eyeW = (eyeW + 15u) & ~15u;
        eyeH = (eyeH + 15u) & ~15u;
        g_FramebufferWidth = eyeW;
        g_FramebufferHeight = eyeH;
    }

    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height)
    {
        if (!g_TryHmdFramebuffer || g_FramebufferWidth < 640 || g_FramebufferHeight < 360)
            return false;
        width = g_FramebufferWidth;
        height = g_FramebufferHeight;
        return true;
    }

    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height)
    {
        uint32_t w = 0, h = 0;
        if (!HaveHmdFramebufferSize(w, h))
            return false;
        if (width == w && height == h)
            return false;
        width = w;
        height = h;
        return true;
    }
}
