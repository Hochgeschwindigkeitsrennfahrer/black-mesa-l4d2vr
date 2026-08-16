# DXVK

## Runtime fact (this Steam install)

- Game: `C:\Program Files (x86)\Steam\steamapps\common\Black Mesa`
- `bms\steam.inf` PatchVersion `100002`, appID `362890`
- `bms.exe` is a small Win32 launcher
- Stock DXVK: `bin\thirdparty\dxvk-windows-x86\d3d9.dll` (2.6.2, ~4.4MB gcc)
- An earlier `bms_d3d9.log` proved DXVK can run: D3D9 → Vulkan, swapchain **1920×1080** `A8R8G8B8` / `VK_FORMAT_B8G8R8A8_UNORM`, plus a **1×1 stub**

The 2026-08-16 19:40 session wrote **no** DXVK log. That launch was native D3D9.

## What the launcher actually does (Ghidra, `bms.exe`)

Command line: `-enabledxvk` / `-disabledxvk`, `-forceuid3d9` / `-forceuid3d11`.

When DXVK is considered on:

1. `DXVK_CONFIG_FILE` = that folder’s `dxvk.conf`
2. Overlay Vulkan layers disabled
3. `AddDllDirectory` of `bin\thirdparty\dxvk-windows-x86`
4. **Full-path `LoadLibraryW` of that `d3d9.dll` only if `FUN_00401470` returns 0**

`FUN_00401470` returns 1 when `AddDllDirectory` + `SetDefaultDllDirectories` exist and `-disablenewdlldirectoryfunctions` was **not** passed (normal Windows 8+). Then `FUN_00401620` calls `SetDefaultDllDirectories(0xC00)` = `LOAD_LIBRARY_SEARCH_USER_DIRS (0x400) | LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800)`. **Application directory is not in that mask**; search is USER_DIRS then System32.

`shaderapidx9` `LoadLibrary("d3d9.dll")` therefore:

- DXVK on → first USER_DIR is the dxvk folder → our thirdparty copy
- DXVK off → dxvk folder never added → `bin\` then game root then System32

A thirdparty-only install is invisible on native D3D9. Install to `bin\` and next to `bms.exe` as well.

`-disablenewdlldirectoryfunctions` would restore the full-path `LoadLibraryW`. Prefer installing to the search path over changing Steam options. `-enabledxvk` is the documented extra if the video menu stays on Direct3D 9.

## Architecture decision

**Keep L4D2VR’s combined DXVK+OpenVR `d3d9.dll`.** Do not hook stock Steam DXVK at the Vulkan ICD. Do not switch the VR runtime to OpenXR because DXVK is on.

## CreateDevice patch

L4D2VR’s `CreateDevice` calls `VR_Init` and overwrites `BackBufferWidth/Height` with the HMD recommended size. This build **retried that** and it **failed** on Black Mesa (2026-08-16): 3168×3100 swapchain → black desktop, one Submit, then Reset + SteamVR waiting room. `hmd_swap` is persisted in `bmvr_skip.txt`. `Reset`/`ResetEx` do not force the backbuffer to eye size while that skip is set. `VR_Init` still runs later from `VR::InitOpenVR`. No `ExitProcess` if SteamVR is off.
