# Black Mesa offset notes (offline RE)

Verified against install path:
`C:\Program Files (x86)\Steam\steamapps\common\Black Mesa`
Build stamped in `offsets.h` as **19042901**.

## How to re-verify (no game launch)

```powershell
py -3 tools\verify_offsets.py
py -3 tools\bm_clientmode_map.py
py -3 tools\bm_cvm_bgmaps.py
```

Modules used:

| Module | Path |
| --- | --- |
| `client.dll` | `bms\bin\client.dll` |
| `server.dll` | `bms\bin\server.dll` |
| `engine.dll` | `bin\engine.dll` |
| `materialsystem.dll` | `bin\MaterialSystem.dll` |

## Critical hooks (OK on disk)

| Name | Module | RVA | Notes |
| --- | --- | --- | --- |
| RenderView | client | `0x207730` | 3-arg (`ret` path differs from L4D/Portal 4-arg); **0 raw dword vtable hits** (thunk/wrapper?) |
| CreateMove | client | `0x110310` | ClientModeShared slot [21]; BM wrapper `0x216130` calls this |
| g_pClientMode | client | `0x16AD56` | pattern @ `0x16AD50` +6 imm32 |
| AdjustEngineViewport | client | `0x1102C0` | empty `retn 0x10` stub; Shared + BM slot [26] |
| CalcViewModelView | client | `0x29D930` | **BM override** `retn 0x0C` (owner*, eyePos&, eyeAng& **inputs**); Base Shared `0x7CF60` unused by BM vt slot [230]; cfg `viewmodel_vr` local-copy eyePos nudge |
| DrawModelExecute | engine | `0xF6A20` | |
| VGui_Paint | engine | `0x238C50` | |
| Get/Push/Pop/Viewport | matsys | see `offsets.h` | |
| ProcessUsercmds | server | `0x5320F0` | |

## IClientMode vtable maps (HIGH confidence)

Anchored on CreateMove VA @ file+`0x4403FC` and AEV @ file+`0x440410` / `0x468BFC`.
COL-backed starts:

| Class | Vtable file | Vtable RVA |
| --- | --- | --- |
| `ClientModeShared` | `0x4403A8` | `0x4415A8` |
| `ClientModeBlackMesaNormal` | `0x468B94` | `0x469D94` |

Slot names follow Source `IClientMode` (PE32 MSVC: [0]=dtor). Same indices on Shared and BM.

### Shared vs BM (key slots)

| Slot | Name | Shared RVA | BM RVA | Notes |
| --- | --- | --- | --- | --- |
| 16 | OverrideView | `0x110BE0` | `0x216EB0` | BM = jmp thunk → Shared |
| 21 | CreateMove | `0x110310` | `0x216130` | BM = thin wrapper E8→Shared |
| 22 | LevelInit | `0x110A80` | `0x216B90` | BM calls Shared then local setup |
| 23 | LevelShutdown | `0x110B30` | `0x216C60` | BM calls Shared then local teardown |
| 25 | ShouldDrawCrosshair | `0x110F00` | `0x217250` | BM = jmp → Shared |
| 26 | AdjustEngineViewport | `0x1102C0` | `0x1102C0` | same stub |
| 32 | GetViewModelFOV | `0x110490` | `0x216510` | BM override (cvar path) |
| 37 | GetMapName | `0x110430` (null) | `0x2164B0` | BM: `lea eax,[this+0x224]; ret` |

Other BM overrides (not VR-critical): InitViewport `0x216790`, Init `0x216750`, ProcessInput thunk `0x216EC0`, Update `0x2172E0`, SetMapName `0x217210`, Get/SetServerName `0x216500`/`0x217230`.

Full 0–52 dumps: `py -3 tools\bm_clientmode_map.py`.

## Best new hook candidates (ranked)

### 1. Map / VR gate — ClientMode LevelInit / LevelShutdown (HIGH) — WIRED

Safer than CreateMove frame counting for menu-vs-gameplay:

- **Hooked** MinHook Shared `LevelInit` `0x110A80` / `LevelShutdown` `0x110B30` (BM always `E8`s into them).
- `LevelInit(const char* newmap)` — reject names like `background01` / `background04` (basename prefix).
- `SeenGameplay` latched only while `vr_eligible`; Update requires eligible + ~300 CreateMove frames before OpenXR.
- Optional later: BM `GetMapName` → wchar at `ClientMode+0x224`.

`LevelInitPreEntity`/`PostEntity` prol RVAs `0x16E840` / `0x16E7D0` are **entity-system log wrappers**, not ClientMode slots (do not use as mode gate).

### 2. OverrideView — Shared `0x110BE0` (HIGH)

- `retn 0x4` → one stack arg (`CViewSetup*`), thiscall.
- Hook Shared (covers BM via thunk `0x216EB0`).
- Candidate for view/FOV tweaks without double RenderView — **runtime untested**.

### 3. CalcViewModelView — MinHook `0x29D930` (HIGH) — CFG-GATED POSE NUDGE

- Gameplay class: **C_BlackMesaViewModel** vt RVA `0x4A4A98` **slot [230]** → `0x29D930`.
- Base Shared `C_BaseViewModel` vt `0x41CBAC` slot [230] → `0x7CF60` (same 3-arg `retn 0x0C`) — **not called** by BM viewmodels.
- Signature: `void(this, CBasePlayer* owner, const Vector& eyePos, const QAngle& eyeAng)`.
- BM body: optional VR/headtrack eye offsets from player, weapon helpers, `SetLocalOrigin`/`SetLocalAngles`.
- Shared `0x7CF60` also applies `vr_viewmodel_offset_forward` when headtrack active (helper `0x107A40`).
- **False positive retired:** old `0xF090` (`retn 0x14`) is an unrelated tile/bitmap helper — do not hook.
- **Pose path:** eyePos/eyeAng are inputs — post-return mutation is useless/unsafe. When `viewmodel_vr 1`, hook builds local eyePos copy + `viewmodel_off_f/r/u` in eyeAngles basis, calls original with that; angles unchanged. Repo default `viewmodel_vr 0`. Disable cfg on instability.

### 4. Engine BackgroundMaps — `0xC0630` (LOW as gate)

- String `BackgroundMaps` @ engine `0x3322C4`; loader pushes `background01`.
- Fn RVA `0xC0630` (calls from `0xA6A0B`, `0xBF561`, `0xC02ED`) — **menu background map load helper**, not a per-frame `IsLevelMainMenuBackground`.
- Use only as name-list hint for LevelInit filter; no `IsLevelMainMenuBackground` / `GetLevelName*` strings in this engine build.

### 5. GetViewModelFOV — BM `0x216510` / Shared `0x110490` (MED)

Useful later with hand/viewmodel work; `viewmodel_fov` string @ client `0x469EB4`.

## Updated / cleaned

- **ProcessUsercmds** → `server.dll` `0x5320F0` (old `0xEF710` stale)
- Removed dead L4D leftovers that always logged “Signature not found”:
  GetMeleeWeaponInfoClient, GetActiveWeapon, WriteUsercmd*, ReadUserCmd

## Still blocked without runtime

- Confirm `g_pClientMode` instance is `ClientModeBlackMesaNormal` in gameplay (vtable expected).
- OverrideView live behavior (LevelInit gate validated: `background*` vs real maps).
- CalcViewModelView cfg-gated eyePos input nudge (`viewmodel_vr`; default off).
- RenderView vtable owner (0 dword hits on `0x207730` VA).
- True stereo / high-res capture without 2× RenderView (window buffer still limits sharpness).
