# Investigation: VR resolution, HUD/VGUI, flashlight

**Status:** implementation pass 2026-08-18 (DRAWHUD restore + framebuffer override + impulse-100 → EF_DIMLIGHT). Ghidra extraction is closed (§11–§14).  
**Do not grow the D3D swapchain** (`hmd_swap`). Override and HUD/flashlight changes are crash-sticky (`fb_override`, `drawhud`).

Evidence tags: **Confirmed from source** / **Confirmed from Ghidra** / **Confirmed from runtime** (existing `docs/RUNTIME.md`) / **Strong inference** / **Unknown**.

---

## Direct answers

### A. Is 1584×1440 inside a 2560×1440 HWND a fundamental Black Mesa limit?

**No.** It is the current BMVR *workaround* after HMD-sized swapchain and oversized named-RT attempts failed. Source itself has an official way to size render targets larger than the window. **Confirmed from Ghidra (`materialsystem.dll`):** Black Mesa’s `IMaterialSystem` **has** `SetRenderTargetFrameBufferSizeOverrides` / `GetRenderTargetFrameBufferDimensions`. `_rt_FullFrameFB*` (`RT_SIZE_FULL_FRAME_BUFFER`) sizes from that getter, not from the HWND. L4D2VR/Portal 2 VR/HL2 VR all render the VR scene into **offscreen targets at `GetRecommendedRenderTargetSize()`**, not into the HWND.

What *is* Black Mesa-specific: **HDR deferred G-buffers** (`_rt_FullFrameFB`, `_rt_gb*`) sized from that logical framebuffer, and `PushRT(NULL)` meaning “backbuffer” at the viewport-query layer. Those make a naive “named eye RT on top of a 2560 G-buffer” fail. They do **not** mean VR resolution must be cropped to the window.

### B. Why HUD is gone in the HMD *and* on desktop

Black Mesa’s gameplay HUD is **not** produced by engine `VGui_Paint` / `CEngineVGui::Paint`. **Confirmed from Ghidra (`client.dll` + `engine.dll`):** it is painted inside `CBlackMesaViewRender::RenderView` when `whatToDraw` bit **1** (`RENDERVIEW_DRAWHUD = 2`) is set: PushRT `_rt_gui` → HUD → downsample `_rt_Hud`. Engine has **no** `_rt_gui` / `_rt_Hud` strings and **never** FindTexture/PushRT those names. `CEngineVGui::Paint` only `PaintTraverseEx`s VGUI panels into the **currently bound RT / backbuffer** (sized from `GetViewport` / HWND `GetClientRect`). Stereo copies currently **strip DRAWHUD**, and the original main `RenderView` is replaced, so `_rt_gui` / `_rt_Hud` never receive a frame. Extra `VGui_Paint` into `bmvrHUD` cannot fill those RTs. Pause-menu flicker is a **second** path: `PAINT_UIPANELS` GameUI onto the desktop backbuffer, fighting the 1584 pillarbox ColorFill.

### C. Flashlight after `impulse 100`

Input is not the remaining question. BMVR already writes `cmd->impulse = 100` in CreateMove.

**Confirmed from Ghidra (`server.dll`):** the server **does receive** impulse 100 (`CUserCmd+0x28` → player `m_nImpulse` at **+0xe44** → `ItemPostFrame` `FUN_1012d330` → `CBasePlayer::ImpulseCommands` `FUN_1022dd40`). That handler **does not toggle the flashlight**. Cases are 200, 202, then `CheatImpulseCommands`. Neither `CBasePlayer::CheatImpulseCommands` (`FUN_102289e0`) nor `CBlackMesaPlayer::CheatImpulseCommands` (`FUN_10472bc0`) has case **100**. `CBlackMesaPlayer` does **not** override `ImpulseCommands` (vtable `+0x608` is the same `FUN_1022dd40`). There is no `flashlight` / `toggle_flashlight` player command.

The networked bool `m_bFlashlightEnabled` is **permission**, not “beam is on”:
- **Server** SendProp **+0x1375** (`DT_BlackMesaLocalPlayerExclusive`). **Client** RecvProp was **+0x17E8**. Same name, **different offsets**.
- Constructor `FUN_1047d750` sets server **+0x1375 = 1**. Watching client `+0x17E8` stay 1 after impulse is expected and does **not** mean the light toggled.
- Actual on-state is **`EF_DIMLIGHT`** (`m_fEffects` bit 2 = 4): server **+0xc4**, client **+0x80**. `FlashlightTurnOn` `FUN_1047eb70` `AddEffects(4)`; `FlashlightTurnOff` `FUN_1047ea90` clears bit 2. Those are **virtual** (`+0x5D8` / `+0x5DC`); impulse 100 never calls them.

TurnOn also requires HEV suit (`m_Local.m_bWearingSuit` **+0x94d**), `m_lifeState` **+0xe8** == 0, and `g_pGameRules` vtable **+0xe0** true (SP `CSingleplayRules` always 1; MP reads **`mp_flashlight`**). `FL_FROZEN` (`m_fFlags` **+0x108** bit `0x40`) zeros usercmd impulse before copy. Server has **no** `_rt_gbShadowMapFlashLight`; the beam is **client-only** render.

Do **not** add ClientCmd flashlight cvars or change bindings. A missing beam after impulse 100 is explained first by **the server ignoring 100**, then by TurnOn gates / wrong offset / stereo GB render (client §8).

---

## 1. What the current render-resolution restriction actually is

**Confirmed from runtime** (`docs/RUNTIME.md`, `docs/DXVK.md`) + **Confirmed from source** (`src/bmvr_flags.cpp`, `src/hooks.cpp`, `src/vr.cpp`):

| Attempt | Result |
|---|---|
| `CreateDevice` BackBuffer = OpenVR recommended (~3168×3100) (`hmd_swap`) | Black desktop, one Submit, Reset released VR RTs |
| Named `leftEye0`/`rightEye0` + rewrite `PushRT(NULL)` onto the eye | Crash / black world (`named_push`, `steamvr_rt`) |
| Lie `GetScreenSize` permanently to HMD size | 1584 G-buffer in 2560 HWND; VGUI mouse miss; pillarbox |
| Current: fit HMD *aspect* inside HWND height → **1584×1440**, crop top-left of 2560×1440 backbuffer into private D3D9 eye surfaces, Submit that crop | Fused stereo, but VR pixels ≤ window height |

The restriction is therefore:

1. **G-buffers follow the logical framebuffer / videomode**, not SteamVR.
2. BMVR currently **refuses** to make that framebuffer larger than the HWND (because forcing the D3D swapchain to 3168×3100 died).
3. Stereo then **crops** that G-buffer rather than rendering into a larger offscreen color target.

HWND size is **not** what SteamVR submits. We already submit private `IDirect3DSurface9` eye textures. The crop exists because those surfaces are filled from the **backbuffer**, whose content is the G-buffer resolve.

---

## 2. What L4D2VR does

**Confirmed from source** (`third_party/l4d2vr/L4D2VR`):

They do **not** submit the HWND.

1. `IVRSystem::GetRecommendedRenderTargetSize` → `m_RenderWidth/Height` (`vr_lifecycle_init.inl` ~1077).
2. **Named offscreen RTs** `leftEye0` / `rightEye0` at that size, `RT_SIZE_NO_CHANGE` (`vr_lifecycle_update.inl` ~2353). Optional submit pair `leftEyeSubmit0`/`rightEyeSubmit0`.
3. `dRenderView`: `EyeRenderTargetScope` **PushRT / SetRT** the named eye, set `CViewSetup.width/height/unscaled` to eye size, call original `RenderView` (`hooks_render.inl` ~4752, ~5226).
4. OpenVR Submit reads those named RTs (via `IDirect3DVR9` / DXVK shared Vulkan images), **not** the window.

**Logical D3D backbuffer vs HWND** (**Confirmed from source**, comment at `vr_lifecycle_update.inl` 2320–2325):

> DXVK intentionally exposes the square VR eye size as Source's logical D3D9 backbuffer (e.g. 4040×4040), while VGUI still lays out in the real desktop client area (e.g. 1920×1080).

Their `CreateDevice` overwrites `D3DPRESENT_PARAMETERS.BackBufferWidth/Height` with the recommended size so Source G-buffers match the eyes. Present still targets the smaller HWND (DXVK scales). `vrHUD` is allocated at **window** size, not eye size.

BMVR forked that CreateDevice path and then **skipped** it after the 3168×3100 failure (`hmd_swap` in `docs/DXVK.md`). So we kept L4D2VR’s *named RT idea* but not the *logical backbuffer = HMD* lie that makes G-buffers match.

L4D2 also does **not** fight a BM-style deferred `PushRT(NULL)` onto an HDR G-buffer the same way; BM `ViewDrawScene` binds the backbuffer three times (**Confirmed from runtime**).

---

## 3. What Portal 2 VR does

**Confirmed from source** (Gistix/portal2vr `hooks.cpp`, L4D2VR fork):

- Same named `leftEye0`/`rightEye0` pattern.
- `IMatRenderContext::SetRenderTarget(m_LeftEyeTexture)` then original `RenderView` with `setup.width/height = m_RenderWidth/Height`.
- Extra `hudViewSetup` (5-arg `RenderView` — BM is **3-arg**).
- `GetViewport` / VGUI bounds hooks return HMD size.
- Restores `SetRenderTarget(NULL)` after both eyes.

They render the **scene independently of the desktop backbuffer** by switching the material-system RT before `RenderView`. They do not require the HWND to be 3K+ tall.

Portal 2 is **not** BM’s deferred G-buffer stack. Copying `SetRenderTarget(eye)` without matching FullFrame/G-buffer size is exactly the BM failure already logged.

---

## 4. What Half-Life 2 VR / official Source VR does

This is the Source-side mechanism, not a special compiler privilege.

**Confirmed from SDK 2013** (`public/sourcevr/isourcevirtualreality.h`, `public/materialsystem/imaterialsystem.h`, `game/client/viewrender.cpp`, `game/client/client_virtualreality.cpp`) and Valve `sourcevr/sourcevirtualreality.cpp`:

1. `CSourceVirtualReality::CreateRenderTargets`:
   - `_rt_gui` 640×480 `RT_SIZE_OFFSCREEN` (HUD).
   - If `vr_use_offscreen_render_target`: `_rt_vr_predistort` at `GetRecommendedRenderTargetSize()`, `RT_SIZE_LITERAL`, plus depth. **HWND unchanged.**
2. `GetRenderTargetFrameBufferDimensions`: when offscreen RT is on, returns HMD recommended size; else `0,0` → “use real backbuffer.”
3. **`IMaterialSystem::SetRenderTargetFrameBufferSizeOverrides(w,h)`** — comment in SDK 2013:

   > Sets the override sizes for all render target size tests. These replace the frame buffer size. Set them when you are rendering primarily to something larger than the frame buffer **(as in VR mode).**

4. `SetupMain3DView`:
   - **HDR_TYPE_FLOAT:** always `Push3DView(..., GetFullFrameFrameBufferTexture(0), ...)` — **ignores** `g_pSourceVR->GetRenderTarget`.
   - Non-float stereo: `Push3DView` with `g_pSourceVR->GetRenderTarget(eye, RT_Color/Depth)`.
5. `CClientVirtualReality` stereo loop: `Push3DView(eyeView, CLEAR, pColor, NULL, pDepth)` then HUD quad / undistort.

**Implication for BM:** BM is HDR deferred (**Confirmed from runtime**: `A2R10G10B10` / FullFrameFB). Even official Source VR would draw the **world into `_rt_FullFrameFB`**, not into `_rt_vr_predistort`, unless the framebuffer-size override makes FullFrame **itself** HMD-sized. The VR-sized target is then a copy/composite of that oversized FullFrame, not a second world render into a tiny HWND backbuffer.

**Black Mesa already contains Source VR client hooks.** **Confirmed from Ghidra** (`client.dll`):

- String `exec sourcevr_%s.cfg`
- `DAT_105a4898` used as `ISourceVirtualReality` (`ShouldRunInVR` +0x24, `GetRenderTarget` +0x5c, `GetViewportBounds` +0x2c)
- `FUN_10105dd0` composites `_rt_gui` onto left/right eyes when that interface is live

`sourcevr.dll` is **present** in `Black Mesa\bin\` as of 2026-08-16 (157696 bytes) but was **not** loaded this pass; those client branches no-op if the pointer is null. The **code to consume engine VR RTs is already in `client.dll`.** We still call the override setters ourselves.

---

## 5. What Black Mesa / BMVR is doing differently

| Layer | L4D2VR / P2VR / HL2 VR | BMVR today |
|---|---|---|
| Eye color | Named MaterialSystem RT at recommended size | Private D3D9 surfaces, filled by **StretchRect from backbuffer crop** |
| G-buffer size | Matches logical BB = HMD (L4D2 DXVK lie) or `SetRenderTargetFrameBufferSizeOverrides` (HL2) | Window / 1584×1440 fit |
| `RenderView` dest | Bound to named eye **before** original | Original engine dest (FullFrame → backbuffer), then crop |
| `PushRT(NULL)` | Backbuffer is already HMD-sized (L4D2) | 2560×1440 LDR swapchain; rewriting it onto an HDR eye **died** |
| HUD | Separate `vrHUD` + `VGui_Paint`; L4D2 still has DRAWHUD on eyes unless suppressed | Stereo **strips DRAWHUD**; extra VGUI overlay |
| Desktop | Optional mirror from eye | Engine 16:9 resolve + ColorFill of “unused” 1584 pillarbox |

Architectural mistake: treating “we cannot grow the DXGI/D3D swapchain” as “we cannot grow the **Source framebuffer used for RT_SIZE_FULL_FRAME_BUFFER**.” Those are different objects. HL2 VR’s override API exists specifically to split them.

---

## 6. Mechanism we need to reproduce (not implement yet)

**Intended resolution path (not implemented):** override API sizes FullFrame; do **not** grow the D3D swapchain.

1. Keep HWND and D3D swapchain at the desktop resolution (do **not** retry `hmd_swap`).
2. Tell the material system the **framebuffer used for RT sizing** is `GetRecommendedRenderTargetSize()`:
   - **Prefer `IMaterialSystem::SetRenderTargetFrameBufferSizeOverrides`** — it **exists** on Black Mesa (vtable slot 142 / `+0x238`, stores `this+0x2ACC` / `+0x2AD0`). `RT_SIZE_FULL_FRAME_BUFFER` reads that through `GetRenderTargetFrameBufferDimensions` (slot 143 / `+0x23C`), **not** through `GetBackBufferDimensions` (slot 30).
   - With override `0,0`, that getter tail-calls **`IShaderDevice+0x0C` = `GetBackBufferDimensions`**, which reads `D3DPRESENT_PARAMETERS.BackBufferWidth/Height`. That is the **same two dwords** as `IShaderAPI+0x9C`. `IShaderAPI+0x458` is **not** GetBackBufferDimensions (it is SupportsMSAA). Hooking slot 30 / lying `GetScreenSize` is still the wrong lever for FullFrame and still breaks VGUI.
3. Allocate named eye RTs (or `_rt_vr_predistort`) at that same size.
4. Drive stereo `CViewSetup` width/height to that size.
5. Bind the **FullFrame / eye RT** for the world pass **without** rewriting flashlight/CSM `PushRT`s (already skipped by name).
6. Submit the eye RT to SteamVR; **downsample** FullFrame → HWND for desktop (do not ColorFill VGUI).
7. VGUI layout stays **window** sized (`vrHUD` / `_rt_gui` window-sized), independent of eye RT.

That is how the other projects get VR pixels **larger than the desktop window**: the extra pixels live in **offscreen Source textures**, sized by the VR framebuffer override, not by `GetClientRect`.

**Do not** retry `PushRT(NULL) → eye` — that was a verified BM crash.

**Do not** permanently lie `GetScreenSize` to 1584 — that is what broke VGUI mouse and pillarboxed the HUD.

---

## 7. HUD / VGUI — real failure point

### Engine path (gameplay HUD)

**Confirmed from Ghidra** `CBlackMesaViewRender_RenderView` (`client.dll` `0x1020EE40`):

```
1020f8cf  TEST byte ptr [EBP+0x10], 0x2    ; whatToDraw & RENDERVIEW_DRAWHUD
1020f8d3  JZ   0x1020fc9b                 ; skip entire HUD block
...
1020f9a6  PUSH "_rt_gui"                  ; FindTexture
1020fa90  CALL [EAX+0x23C]                ; PushRT _rt_gui
...
1020fbd6  CALL [EAX+0x24C]                ; PopRT
```

Then **Confirmed from Ghidra** `FUN_10267420`: FindTexture `_rt_Hud`, downsample/blur/scanlines using `CViewSetup` `param_1[4]`/`[6]` (width/height), composite back.

**Confirmed from source** (`src/hooks.cpp` stereo branch):

```cpp
const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
callOriginal(leftEyeView, ...);
callOriginal(rightEyeView, ...);
```

There is **no** leftover original `RenderView` with DRAWHUD. So `_rt_gui` / `_rt_Hud` are empty. In-eye HUD gone **by construction**. Desktop HUD gone because BM composites that same `_rt_Hud` onto the backbuffer inside this block — not because VGUI “forgot” the desktop.

Extra `PaintVguiToOverlay` (`dVGui_Paint` → `bmvrHUD`) only re-runs **engine VGUI**. It does not run `FUN_10267420`. That is why overlay capture + `_rt_gui` names + DRAWHUD stripping still yield no gameplay HUD.

**Confirmed from Ghidra (`engine.dll`, §14):** BMVR’s hook at RVA `0x238C50` is `CEngineVGui::Paint` itself (`FUN_10238c50`). It never FindTexture/Push `_rt_gui` or `_rt_Hud`. Modes: `PAINT_UIPANELS` (1) = GameUI/console tree on the embedded surface; `PAINT_INGAMEPANELS` (2) = `staticClientDLLPanel` / tools only; `PAINT_CURSOR` (4) = software cursor. Menu/loading calls `Paint(5)` = UIPANELS|CURSOR. In-game `V_RenderView` calls `g_ClientDLL->View_Render` and does **not** Paint; GameUI during gameplay is the client calling `IVRenderView::VGui_Paint` (`FUN_1014f000` trampoline). Extra `VGui_Paint` **cannot** restore gameplay HUD.

### Pause-menu flicker

**Confirmed from Ghidra (`engine.dll`) + Confirmed from source:**

1. Stereo `ClearUnusedDesktopBackbuffer` ColorFills the backbuffer **right of 1584 and below 1440** every eye pair (`src/vr.cpp`). `CEngineVGui::Simulate` sizes the material viewport from **HWND `GetClientRect`** (2560×1440). `Paint` then `SetBounds`s `staticPanel` from **`GetViewport`**. Pause/GameUI is a full-window VGUI tree (`PAINT_UIPANELS` → `PaintTraverseEx` embedded panel) onto the **current RT / backbuffer**, not `_rt_gui`. Logo/chrome in the pillarbox is erased every frame, then GameUI paints it again → flicker.
2. `dVGui_Paint` always calls original (desktop) **then** extra paint with `PAINT_UIPANELS|PAINT_INGAMEPANELS|PAINT_CURSOR` into `bmvrHUD`. That second pass is the same `CEngineVGui::Paint` (still no `_rt_gui`). L4D2VR `PaintToHudOnce` **clears** a HUD RT and may PushRT `_rt_gui` — BM engine Paint does not. Fighting happens on the **backbuffer** (and any RT BMVR binds for the extra pass), not by filling `_rt_gui`.

**Confirmed from source:** overlay is **hidden** unless `m_HudPaintedThisFrame` (`SubmitHudOverlay`). If extra paint fails or `HudOverlayReady` is false, HMD HUD is empty even when desktop VGUI ran.

**Correct architecture (do not implement yet):**

| Surface | Owner | Size | When |
|---|---|---|---|
| `_rt_gui` / `_rt_Hud` | Client `RenderView` **with DRAWHUD** on **one** pass (desktop or a dedicated HUD view), not both stereo eyes | Window / HUD RT | Once per frame |
| Desktop backbuffer | Engine composite of world crop + that HUD | HWND | After world |
| `bmvrHUD` / SteamVR overlay | **Copy** of `_rt_gui` or `_rt_Hud` (or a single extra VGUI paint **only if** DRAWHUD cannot run) | Window | After HUD exists |
| Eye textures | World only (`~DRAWHUD`) | HMD RT | Stereo |

Never ColorFill regions VGUI still owns. Never extra-paint GameUI onto two destinations that share `_rt_gui` without a copy.

---

## 8. Flashlight — real failure point (partial)

**Confirmed from source:** CreateMove writes `cmd->impulse = 100` after original CreateMove (`src/hooks.cpp`). User confirmed this fires. Stop binding work.

**Confirmed from Ghidra (`client.dll`):**

| Piece | Where | Role |
|---|---|---|
| `m_bFlashlightEnabled` | RecvProp `DT_BlackMesaLocalPlayerExclusive` **+0x17E8** (`FUN_1025c850`) | Server-replicated on/off |
| `EF_DIMLIGHT` | `this+0x80` bit 2 (`FUN_1025e000` FlashlightOff sound) | HL2 effect flag |
| `CClientShadowMgr::UpdateFlashlightState` | `FUN_1011bb20` | Copies `FlashlightState_t` (origin at `param_2[0]`, forward `param_2[3]`, FOV `param_2[9]`) into a perspective world→flashlight matrix |
| `CClientShadowMgr::BuildFlashlight` | `FUN_10112920` | Leaf enum / projected texture |
| G-buffer flashlight RTs | `FUN_1018bef0` | `_rt_gbShadowMapFlashLight` **1024×1024**, material `effects/flashlight001` |
| ConVars | `gb_flashlight_enabled`, `gb_flashlight_Pos*`, `gb_flashlight_dir`, `r_flashlightfov`, offsets | BM deferred light, **plus** HL2 projected flashlight |

**Confirmed from Ghidra (`server.dll`, §13):** impulse 100 is consumed by `FUN_1022dd40` and **ignored** (no flashlight case). HL2’s `CHL2_Player::ImpulseCommands` `switch(100)` is **not in this binary**. `m_bFlashlightEnabled` is permission at server **+0x1375** (ctor sets it to 1); the toggle that would matter is **`EF_DIMLIGHT`**, which impulse 100 never sets. No `flashlight` / `toggle_flashlight` command.

**Unknown (need more client trace + maybe runtime):** who **fills** `FlashlightState_t` origin — typically `EyePosition()` + `EyeAngles()`, i.e. **player body**, not the uncoupled HMD `CViewSetup`. If `EF_DIMLIGHT` *is* on, the cone can sit at the body while the camera is at the HMD (dim/wrong, not necessarily “off”). If the GB pass uses `CViewSetup` from stereo, origin might be HMD — then a broken RT bind would still hide it.

**Confirmed from source:** stereo `PushRT` redirect **skips** names containing `flashlight` / `Flashlight` / `shadow` / `csm`. We are probably **not** stuffing the 1024² flashlight depth into the eye. A remaining risk is the **lighting apply** using the 1584 crop / wrong view matrix, or the GB/projected path never seeing **`EF_DIMLIGHT`**.

**Do not change input.** Next flashlight proof (if still needed after a HUD pass): does server/client **`m_fEffects` bit 2** flip after a **vanilla** (non-VR) `impulse 100`? Expect **no**. If vanilla BM still lights up, TurnOn is reached by a **virtual call** Ghidra could not xref (only DATA vtable xrefs to `FUN_1047eb70`). That caller is **not** impulse 100.

---

## 9. Modules / functions still to reverse

### Already established in `client.dll`

| Function | Address | Why |
|---|---|---|
| `CBlackMesaViewRender::RenderView` | `0x1020EE40` | DRAWHUD / `_rt_gui` |
| HUD downsample | `0x10267420` | `_rt_Hud` |
| Source VR GUI composite | `0x10105DD0` | `_rt_gui` → eyes if `ISourceVirtualReality` live |
| Flashlight Recv table | `0x1025C850` | `m_bFlashlightEnabled` +0x17E8 |
| `UpdateFlashlightState` | `0x1011BB20` | origin/FOV matrix |
| GB flashlight RT init | `0x1018BEF0` | `_rt_gbShadowMapFlashLight` |

### Need Ghidra — remaining

`materialsystem.dll` extraction is in **§11**. `shaderapidx9.dll` extraction is in **§12**. `server.dll` flashlight extraction is in **§13**. `engine.dll` HUD / VGUI extraction is in **§14**. Resolution, impulse-100-as-flashlight, and `_rt_gui` vs `VGui_Paint` are closed. Do **not** patch flashlight input. **Do not start coding until asked.** Optional later: `sourcevr.dll` is **on disk** (see §10); not required for the three implementation items.

---

## 10. Pause: additional binaries for Ghidra

**Sequential list for this investigation is complete.** You may unload `engine.dll`. Do **not** start coding until asked.

1. ~~**`materialsystem.dll`**~~ — **done 2026-08-18.** Override API present. FullFrame uses `GetRenderTargetFrameBufferDimensions`, not `GetBackBufferDimensions`.

2. ~~**`shaderapidx9.dll`**~~ — **done 2026-08-18.** `IShaderDevice+0x0C` and `IShaderAPI+0x9C` are both `GetBackBufferDimensions` → `D3DPRESENT_PARAMETERS.BackBufferWidth/Height`. `IShaderAPI+0x458` is SupportsMSAA, not a size getter. No remaining resolution blocker in this DLL. Do **not** retry `hmd_swap`.

3. ~~**`server.dll`**~~ — **done 2026-08-18.** Impulse 100 reaches `ImpulseCommands` and is **not** a flashlight toggle. Permission bool defaults to 1 at **+0x1375**; on-state is `EF_DIMLIGHT`. No flashlight console command.

4. ~~**`engine.dll`**~~ — **done 2026-08-18.** `CEngineVGui::Paint` at RVA `0x238C50` does **not** fill `_rt_gui` / `_rt_Hud`. Gameplay HUD is client `RenderView` DRAWHUD only. Extra `VGui_Paint` cannot replace it.

5. **`bin\sourcevr.dll`** — earlier note said **absent**. **On disk now** (2026-08-16, 157696 bytes, PE32) at `Black Mesa\bin\sourcevr.dll`. **Not loaded** this pass. HUD / resolution / impulse-100 answers do not need it. Optional later if we want Valve `ISourceVirtualReality` internals; BM client already no-ops that interface when the pointer is null, and we will call `SetRenderTargetFrameBufferSizeOverrides` ourselves.

Do **not** load a different `d3d9.dll` into Ghidra for this; DXVK behavior is already in-tree.

No resolution / HUD / flashlight patch in this research pass.

---

## 11. `materialsystem.dll` findings (2026-08-18)

Image base `0x10000000`. Unique strings `mat_vrmode_adapter` at `0x100CFA44`, `_rt_FullFrameFB1`, BeginRT warnings — Ghidra was on this DLL.

CMaterialSystem vtable starts at `0x100CEE14`. Slot 30 (`+0x78`) = `GetBackBufferDimensions`. `GetRenderContext` is slot **103** (`+0x19C` = `0x100CEFB0`), matching `offsets.h`.

### Confirmed from Ghidra

#### 1. Override API — **present**

| Method | Slot | Vtable offset | Function | Body |
|---|---|---|---|---|
| `SetRenderTargetFrameBufferSizeOverrides(w,h)` | **142** | `+0x238` | `FUN_100524A0` | `this+0x2ACC = w; this+0x2AD0 = h;` |
| `GetRenderTargetFrameBufferDimensions(&w,&h)` | **143** | `+0x23C` | `FUN_1004C4C0` | If both `+0x2AD0` and `+0x2ACC` are **nonzero**, return them. Else **tail-call vtable `+0x88` (slot 34)** |
| `GetDisplayDeviceName` | 144 | `+0x240` | `FUN_1004BE50` | thunk `ShaderDevice001+0x84` |
| `CreateTextureFromBits` | 145 | `+0x244` | `FUN_100499D0` | creates `"frombits"` |

SDK 2013 puts `SetRenderTargetFrameBufferSizeOverrides` **24** slots after `GetRenderContext`. Black Mesa puts it **39** slots after (15 extra methods in between: G-buffer / BM-specific). The **methods exist**; do not use vanilla SDK 2013 slot numbers.

Slot 34 (`FUN_1004BC60`) is **not** `GetBackBufferDimensions`. It is `JMP [ShaderDevice001+0x0C]` (`DAT_1014D938`, factory `"ShaderDevice001"` from `Connect` `FUN_10048D20`).

No CODE xref to the setter — nothing **inside this DLL** calls it. `ApplyRenderTargetSizeMode` calls the getter via `g_pMaterialSystem` (`PTR_DAT_1012E058` → `DAT_10137A88`) **`+0x23C`**.

#### 2. `RT_SIZE_FULL_FRAME_BUFFER` / `_rt_FullFrameFB*` size source

`CreateNamedRenderTargetTextureEx` (`FUN_10049660`, RVA `0x49660`) does **not** apply size modes. It only checks BeginRT (`this+0x2AA0`), then `CTextureManager+0x18`.

Size is `CTexture::InitRenderTarget` `FUN_1006FB50` → `ApplyRenderTargetSizeMode` `FUN_1006C9D0` → allocate `FUN_1006F960`:

- Requested `w/h` stored as ushorts at texture `+0x40` / `+0x42`.
- `RenderTargetSizeMode_t` at texture `+0x90`.
- Switch calls **`GetRenderTargetFrameBufferDimensions`** (`g_pMS+0x23C`) for every mode that cares about the framebuffer.

| `this+0x90` | Mode | What BM does |
|---|---|---|
| 0 (default) | `RT_SIZE_NO_CHANGE` | Keep requested; `HushAsserts()` in default |
| 1 | `RT_SIZE_DEFAULT` | Halve until ≤ FB |
| 2 | `RT_SIZE_PICMIP` | `skipMipLevels` then clamp to FB |
| 3, `0xE` | `RT_SIZE_HDR` | FB `/ 4` |
| **4** | **`RT_SIZE_FULL_FRAME_BUFFER`** | **FB size**; if `HardwareConfig+0x50` (SupportsNonPow2) is false, `FloorPow2(w+1)` |
| 5 | `RT_SIZE_OFFSCREEN` | Halve while larger than FB |
| 6 | `RT_SIZE_FULL_FRAME_BUFFER_ROUNDED_UP` | FB size; else `CeilPow2` |
| 7 | `RT_SIZE_REPLAY_SCREENSHOT` | uses string `replay_screenshotresolution` |
| **8, 9** | **`RT_SIZE_LITERAL` / `LITERAL_PICMIP`** | **no-op** (keep requested) |
| `0xA`–`0xD` | BM extras | FB `/2`, `/8`, `/16`, `/32` |

`EndRenderTargetAllocation` / videomode restore `FUN_10070A10` **re-runs** `ApplyRenderTargetSizeMode` and reallocates if the computed size changed.

This DLL has **`_rt_FullFrameFB1`** (FindTexture alias → `_rt_FullScreen`) and **`_rt_FullFrameDepth`**, not the string `_rt_FullFrameFB`. The primary FullFrame name is allocated by client/engine with mode 4.

**FullFrame width/height = GetRenderTargetFrameBufferDimensions.** With override `0,0` (BMVR never sets it today) that is **`IShaderDevice+0x0C`**, not videomode and not `IMaterialSystem` slot 30.

#### 3. `GetBackBufferDimensions` thunk — **confirmed**

`FUN_10052D20` (RVA `0x52D20`), only DATA xref = vtable slot 30:

```
MOV ECX, [DAT_1014D930]    ; ShaderApi030 (Connect)
MOV EAX, [ECX]
MOV EAX, [EAX+0x458]
JMP EAX
```

`Connect` (`FUN_10048D20`) assigns:

| Global | Factory name |
|---|---|
| `DAT_1014D934` | `ShaderDeviceMgr001` |
| `DAT_1013BE4C` | `MaterialSystemHardwareConfig012` |
| `DAT_1014D930` | `ShaderApi030` |
| `DAT_1014D938` | `ShaderDevice001` |
| `DAT_1014D93C` | `ShaderShadow010` |

**§12:** IShaderAPI GetBackBufferDimensions is `+0x9C`, not `+0x458` (that slot is SupportsMSAA). Re-check this displacement if materialsystem is reloaded. Slot 31 is **not** `GetBackBufferFormat` (`FUN_1004C730` returns `&DAT_1013BED8`) — already noted in `offsets.h`.

#### 4. `mat_vrmode_adapter`

String `0x100CFA44`. DATA xref `0x10036CE7` is **not a function**; it is a CRT ConVar initializer at `0x10036CE0`:

`ConVar("mat_vrmode_adapter", "-1", 0)` → object `DAT_1013A908`.

Only consumer in this DLL: `UpdateConfig` `FUN_1004F430`:

- `MaterialSystem_Config_t+0x7C` = `GetInt()` (XOR default when unconnected).
- If **not** `-1`: `config+0x34 = (flags & ~0x40000) | 1` — same idea as SDK `matsys_interface.cpp`: VR adapter set → **force windowed**, do not go fullscreen on the HMD.

It does **not** size render targets, does **not** call OpenVR, and this DLL has **no** `sourcevr` / `predistort` / `offscreen` strings. Adapter pick itself is `SetMode` in shaderapi/engine.

#### 5. `CreateNamedRenderTargetTextureEx` / Ex2

- **Ex** `FUN_10049660` (`0x49660`): BeginRT guard; optional `-r_emulate_gl` depth rewrite; `TextureManager+0x18` → `FUN_1006D1B0` (RTTI cast to `CTexture`) → `InitRenderTarget`.
- **Ex2** `FUN_10049600` (`0x49600`): same guard, then `this+0x168` (**Ex**, slot 90) and an extra `ITexture+0x2C`.

#### 6. `PushRenderTargetAndViewport` when `pTexture == NULL`

`FUN_1006A3D0` (RVA `0x6A3D0`, 6-arg). Pushes a **0x24-byte** stack record at `this+0x40` (depth `this+0x4C`):

`[0] pTexture` (NULL allowed), `[0x10] pDepth`, `[0x14..0x20] viewport x,y,w,h`.

Then `this+0x440` — on `CMatRenderContext` that slot is `FUN_10048C80` = **`RET` (no-op)**. No immediate D3D bind.

Related:

- `GetRenderTarget` `FUN_10068820`: empty stack → **0**; else top `pTexture` (NULL stays NULL).
- `SetRenderTarget` (`IMatRenderContext+0x18` → `FUN_10052480` → `+0x290` `FUN_1006B520`): **rewrites the stack slot only**.
- `GetViewport` `FUN_10068A70`: if viewport w/h on the stack are ≤ 0 and **`pTexture == NULL`**, call **`IShaderAPI+0x9C`** for width/height. Else `ITexture+0x0C/+0x10` (GetActualWidth/Height).

**NULL means backbuffer** at the **dimension query** layer. **§12:** `IShaderAPI+0x9C` is `GetBackBufferDimensions` (present-param size), not GetViewports (`+0x04`). The GPU bind is deferred (queued draw/flush / shaderapi). Do **not** retry rewriting `PushRT(NULL)` onto an HDR eye.

`CMatRenderContext` vtable base `0x100D2874`: `+0x1C` = `GetRenderTarget` `0x10068820` (matches `offsets.h`). `+0x23C` = 6-arg PushRT.

#### 7. IMaterialSystem slots after `GetRenderContext`

`GetRenderContext` `FUN_1004C460` (slot 103): thread-local context, default `this+0x2290`.

Identified immediately after it (SDK order, with BM extras inserted):

| Slot | Offset | ID |
|---|---|---|
| 103 | `+0x19C` | `GetRenderContext` |
| 104 | `+0x1A0` | `SupportsShadowDepthTextures` (`ShaderAPI+0x47C`) |
| 105–106 | | `BeginUpdateLightmaps` / `EndUpdateLightmaps` |
| 107–108 | | `Lock` / `Unlock` (strings `*CMaterialSystem::Lock/Unlock`) |
| 124 | | `CreateRenderContext` (type 0 returns null; 1 queued; 2 other) |
| 125 | | `SetRenderContext` (thread-local) |
| 142–145 | `+0x238`… | **override pair**, `GetDisplayDeviceName`, `CreateTextureFromBits` |

Our `src/sdk/material.h` still **ends at `GetRenderContext`**. The override methods are **after** that pad. Seven extra methods also sit between `GetBackBufferDimensions` and `BeginRenderTargetAllocation` vs that header (`BeginRT` is vtable `0x100CEF70` = slot 87, not 80).

### Closed by `shaderapidx9.dll` (§12)

- `IShaderDevice+0x0C` and `IShaderAPI+0x9C` are the **same** `GetBackBufferDimensions` → `m_PresentParameters.BackBufferWidth/Height`. They stay in sync after Reset because Reset uses that same struct.
- `IShaderAPI+0x458` is **SupportsMSAA**, not a size getter. The §11 thunk note that IMaterialSystem slot 30 JMPs `ShaderApi030+0x458` does **not** land on GetBackBufferDimensions on this vtable; the real IShaderAPI GetBackBufferDimensions is **`+0x9C`**. Re-check that displacement if `materialsystem.dll` is reloaded. Do not hook `+0x458` expecting width/height.

### Still Unknown (not this DLL)

- Where the deferred `SetRenderTarget(backbuffer)` lives when the RT stack top is NULL (queued draw/flush). Not needed to choose the override path.
- Who in engine/`SetMode` consumes `config+0x7C` (`mat_vrmode_adapter`) to pick the D3D adapter.

### Not in this DLL

- `_rt_FullFrameFB` (no `1`) allocation site — client/engine.
- `sourcevr` / `_rt_vr_predistort` / official offscreen VR RT names.
- Flashlight `impulse 100` — **`server.dll` done, §13.**

---

## 12. `shaderapidx9.dll` findings (2026-08-18)

Image base `0x10000000`. Unique strings `ShaderDevice001`, `ShaderApi030`, `CShaderAPIDx8::`, `shaderapidx9.dll`, RTTI `.?AVCShaderDeviceDx8@@` — Ghidra was on this DLL. `mat_vrmode_adapter` is absent (that string is materialsystem).

One object, two interfaces (**Confirmed from Ghidra** constructor `FUN_1003E470`):

| Factory | Global | Object | Vtable |
|---|---|---|---|
| `ShaderDevice001` | `DAT_100EE7E4` | `g_ShaderAPIDx8` + **0** (`0x100E9FA0`) | `CShaderAPIDx8` IShaderDevice `0x1008A2EC` |
| `ShaderApi030` | `DAT_100EE7EC` | same object + **`0x220`** (`0x100EA1C0`) | IShaderAPI `0x1008A398` |
| `ShaderShadow010` | `DAT_100EE7F0` | | |

`CShaderDeviceDx8::vftable` at `0x1008B6D8` is written by `FUN_100528C0` then overwritten by the CShaderAPIDx8 vtable at offset 0. Getters: `FUN_10050C40` → device, `FUN_10050C30` → API.

`g_pD3DDevice` = `DAT_100EE808`. `IDirect3D9` lives on the device-mgr object `PTR_DAT_100E1AE0+0x30`.

### Confirmed from Ghidra

#### A. `IShaderDevice` vtable `+0x0C` (slot 3) — `GetBackBufferDimensions`

Returns **two ints** (`int& width`, `int& height`). **Not** HWND client rect, **not** a live `GetBackBuffer` query.

```
FUN_100468E0: jmp FUN_10054960          ; this = device
FUN_10054960:
  *width  = this+0x2C;   ; D3DPRESENT_PARAMETERS.BackBufferWidth
  *height = this+0x30;   ; D3DPRESENT_PARAMETERS.BackBufferHeight
```

`SetPresentParameters` `FUN_10055B60` `ZeroMemory(this+0x2C, 0x38)` then fills that struct. `sizeof(D3DPRESENT_PARAMETERS)` is 0x38. Field map matches D3D9:

| Device `this+` | Present-params field |
|---|---|
| `+0x2C` / `+0x30` | BackBufferWidth / Height |
| `+0x34` | BackBufferFormat — `GetBackBufferFormat` slot 2 `FUN_10054980` |
| `+0x38` | BackBufferCount |
| `+0x3C` | MultiSampleType |
| `+0x48` | hDeviceWindow |
| `+0x4C` | Windowed |
| `+0x60` | PresentationInterval |

**IShaderDevice slots 0–10** (vtable `0x1008A2EC`):

| Slot | Off | Addr | ID |
|---|---|---|---|
| 0 | `+0x00` | `FUN_10055680` | `ReleaseResources` (string) |
| 1 | `+0x04` | `FUN_10055520` | `ReacquireResources` |
| 2 | `+0x08` | `FUN_10054980` | **`GetBackBufferFormat`** |
| **3** | **`+0x0C`** | **`FUN_100468E0`** | **`GetBackBufferDimensions`** |
| 4 | `+0x10` | `FUN_10054990` | `GetCurrentAdapter` (`this+0x24`) |
| 5 | `+0x14` | `FUN_100551E0` | `IsUsingGraphics` (`g_pD3DDevice != 0`) |
| 6 | `+0x18` | `FUN_10055FB0` | `SpewDriverInfo` |
| 7 | `+0x1C` | `FUN_1004F970` | stencil bits (`this+0xAC` if bit `this+0xA8.4`) |
| 8 | `+0x20` | `FUN_100485D0` | MSAA in use (`this+0x3C != 0`) |
| 9 | `+0x24` | `FUN_100552A0` | **`Present`** (D3D `+0x44`, “Internal driver error at Present”) |
| 10 | `+0x28` | `FUN_10051CB0` | **window size** via `GetClientRect(this+0x14)` — **different fields** (`this+0x18/+0x1C` cache) |
| 34 | `+0x88` | `FUN_10054F40` | **`SetMode`** → `CreateD3DDevice` |
| 35 | `+0x8C` | `FUN_10055E50` | Release D3D device |
| 36 | `+0x90` | `FUN_10048630` | `IsDeactivated` |

No slot named GetDesktopResolution on IShaderDevice; desktop/videomode is `IShaderDeviceMgr+0x34` `GetCurrentModeInfo` (`FUN_10055B60` uses that to fill present params in the resizing/fullscreen-default branches).

#### B. `IShaderAPI` vtable `+0x458` (slot 278)

**Not GetBackBufferDimensions.** `0x1008A398+0x458` → `FUN_1004FA30`: `IDirect3D9::CheckDeviceMultiSampleType` (`+0x2C`) with `ComputeMultisampleType` `FUN_10054220`. SupportsMSAA / quality check. Uses device fields via `this-0x1FC` (adapter).

§11 said IMaterialSystem slot 30 JMPs `ShaderApi030+0x458`. On **this** ShaderApi030 vtable that displacement is SupportsMSAA. The actual IShaderAPI `GetBackBufferDimensions` is slot **39 / `+0x9C`**. Do not hook `+0x458` for resolution.

#### C. `IShaderAPI` vtable `+0x9C` (slot 39)

**`GetBackBufferDimensions`**, not GetViewport.

```
FUN_100468E9: sub ecx, 0x220    ; API this → device this
              jmp FUN_100468E0  ; → FUN_10054960 → this+0x2C / +0x30
```

Only rdata xref to `0x100468E9` is `0x1008A434` = IShaderAPI `+0x9C`.

**GetViewport / GetViewports is IShaderAPI slot 1 (`+0x04`) `FUN_10047DC0`:** copies `ShaderViewport_t` from API `this+0x2BE4` (x,y,w,h,minz,maxz). **SetViewports** is slot 0 `FUN_1004F300`, which itself calls **IShaderDevice+0x0C** to clamp, and if the device resizing flag is set also `GetClientRect`.

So `PushRT(NULL)` with viewport w/h ≤ 0 asking `IShaderAPI+0x9C` is asking for **swapchain present-param size**, not the current D3D viewport. Usual full-backbuffer viewports match; a smaller SetViewport would not.

#### D. Same numbers at runtime?

| Query | What it reads | Same as A? |
|---|---|---|
| **A** Device `+0x0C` | `m_PresentParameters` `+0x2C/+0x30` | — |
| **C** API `+0x9C` | same two dwords (this-adjust 0x220) | **Yes. Identical thunks.** |
| IMaterialSystem GetBackBufferDimensions | intended: IShaderAPI GetBackBufferDimensions | **Yes if it calls `+0x9C`; no if it really JMPs `+0x458`.** |
| **B** API `+0x458` | MSAA CheckDeviceMultiSampleType | **No.** |
| Device `+0x28` GetWindowSize | `GetClientRect(HWND)` | **No** — can differ when windowed-resizing allocates a desktop-sized swapchain |
| API `+0x04` GetViewports | last SetViewports (`this+0x2BE4`) | **No** — can be a sub-rect |

**Implication:** FullFrame with override `0,0` == GetBackBufferDimensions == D3D present-param size. Setting `SetRenderTargetFrameBufferSizeOverrides(recommended W,H)` sizes `_rt_FullFrameFB*` **without** changing those two dwords and **without** growing the swapchain. That is the intended resolution path. **Do not retry `hmd_swap`.**

HWND stays desktop-sized. Present still uses `hDeviceWindow`. Extra VR pixels belong in offscreen FullFrame / named eye RTs.

#### E. CreateDevice / SetMode how width/height are stored

`SetMode` `FUN_10054F40` (Device `+0x88`) → `CreateD3DDevice` `FUN_10054410`:

1. `SetPresentParameters` `FUN_10055B60` writes `m_PresentParameters` at `this+0x2C`.
2. `InvokeCreateDevice` `FUN_10054F70` calls `IDirect3D9::CreateDevice` (`+0x40`) with **`this+0x2C`** as `D3DPRESENT_PARAMETERS*`.
3. Stores `g_pD3DDevice`, HWND at `this+8` and `this+0x14`, adapter at `this+4/+0x24`.
4. Then Device `+0x28` caches **client rect** into `this+0x18/+0x1C` (separate from present params).
5. `FUN_10051DD0` creates a hidden `"shaderdx8"` child HWND at `this+0xC` (wndproc `FUN_10052740`) — not the size source.

`ResizeWindow` `FUN_10055750` (string): `SetPresentParameters` then `IDirect3DDevice9::Reset` (`+0x40`) with the **same** `this+0x2C` struct. Reset therefore refreshes A/C from the same fields.

**How BackBufferWidth/Height are chosen** (`FUN_10055B60`, matches Valve `CShaderDeviceDx8::SetPresentParameters`):

- Reads current display mode via DeviceMgr `+0x34` (`local_18`/`local_14`).
- **Fullscreen** (info `+0x34` bit 0 clear): width/height = `info+0x8/+0xC` (`m_DisplayMode`), or that current mode if those are 0.
- **Windowed, not resizing** (bit 0 set, bit 1/`m_bResizing` clear): width/height = **`info+0x8/+0xC` only**. No `GetClientRect`.
- **Windowed + resizing:** desktop mode size, unless `m_bLimitWindowedSize` and the limit (`info+0x2C/+0x30`) is smaller than the desktop mode.

There **is** a Source path whose swapchain is larger than the HWND (windowed-resizing → desktop mode). That is still **the D3D swapchain**, not a separate offscreen backbuffer. Forcing recommended HMD size into `BackBufferWidth/Height` **is** `hmd_swap` and already failed. The independent-of-HWND size we want for VR is the **materialsystem override / FullFrame RT**, not a second Present-params write.

`Direct3DCreate9` / `Direct3DCreate9Ex` is `FUN_10054310` (`-nod3d9ex`, LoadLibrary `d3d9.dll`). Native vs DXVK is which `d3d9.dll` loads; Source still stores size in `m_PresentParameters`.

### Unknown (not blocking resolution)

- Exact queued `SetRenderTarget(NULL)` → D3D backbuffer bind (not needed to choose the override path).
- `mat_vrmode_adapter` consumption remains engine SetMode (this DLL only prints adapter in `SpewDriverInfo`).

### Not in this DLL

- Flashlight `impulse 100` — **`server.dll` done, §13.**
- `VGui_Paint` / `_rt_gui` blit — `engine.dll`, not a resolution blocker.

---

## 13. `server.dll` findings (flashlight / impulse 100, 2026-08-18)

Image base `0x10000000`. **Confirmed this is `server.dll`:** `CBlackMesaPlayer` (`10726318`), `m_nImpulse` (`1064cee4`), `m_bFlashlightEnabled` (`10726d1c`), `sv_cheats` (`1062343c`), `CBasePlayer` (`1064bccc`). No `UTIL_PlayerByIndex` string. Function names mostly stripped; a few RTTI/vtable labels exist (`CBlackMesaPlayer::vftable` at `10726358`, `CMultiplayRules::vftable` at `10638a54`, `CSingleplayRules::vftable` at `1065fe1c`).

No strings: `"impulse 100"`, `ImpulseCommands`, `FlashlightTurnOn`, `toggle_flashlight`, `EF_DIMLIGHT`, `CheatImpulse`, `gbShadowMapFlashLight`. `"flashlight"` at `10722054` is an **attachment name** on HEV-zombie `flashlight_status`, not the player beam.

### Confirmed from Ghidra

#### 1. Who consumes impulse 100 — and it is **not** the flashlight

Path (server game thread, usercmd):

1. `CBlackMesaPlayer::PlayerRunCommand` `FUN_104779b0` (vtable `+0x6ec`) → `FUN_10231290`.
2. If `m_fFlags` **+0x108** bit `0x40` (`FL_FROZEN`): zeros move + **`CUserCmd+0x28` impulse**.
3. `CPlayerMove::RunCommand` `FUN_10237940` copies nonzero `CUserCmd+0x28` → player **`m_nImpulse` +0xe44** (`param_1[0x391]`; datadesc `FUN_102235e0`).
4. `CBlackMesaPlayer::PostThink` `FUN_10477e00` → `CBasePlayer::PostThink` `FUN_10231480` (`+0x68c` → `FUN_1047f050` → **`FUN_1012d330` ItemPostFrame**).
5. `FUN_1012d330` calls vtable **`+0x608` ImpulseCommands**. If dead, PostThink **clears** `m_nImpulse` first (`+0x11c` / `FUN_10069ff0`: `m_lifeState` **+0xe8** == 0 means alive).

**`CBasePlayer::ImpulseCommands` = `FUN_1022dd40`.** Reads `m_nImpulse`. Handles:

| Impulse | Action |
|---|---|
| **200** | `sv_cheats` (`DAT_1083cc68` `+0x2c`) then weapon `GetActiveWeapon` `FUN_100ff4f0` |
| **0xCA (202)** | screenshot-ish path |
| **else** | `this->vtable+0x60c(impulse)` = **CheatImpulseCommands**, then **clears `m_nImpulse`** |

**No case 100 (0x64).**

| Class | ImpulseCommands `+0x608` | CheatImpulseCommands `+0x60c` |
|---|---|---|
| CBasePlayer | `1064c44c` → `FUN_1022dd40` | `1064c450` → `FUN_102289e0` |
| HL2-derived | `1067c948` → **same** `FUN_1022dd40` | `1067c94c` → `FUN_10472bc0` |
| **CBlackMesaPlayer** | `10726960` → **same** `FUN_1022dd40` | `10726964` → `FUN_10472bc0` |

**CBlackMesaPlayer does not override ImpulseCommands.** HL2’s usual `CHL2_Player::ImpulseCommands` `switch(100)` flashlight toggle **is not in this binary**.

**`CBlackMesaPlayer::CheatImpulseCommands` = `FUN_10472bc0`:** requires `sv_cheats`. If impulse **!= 0x65 (101)**, calls base `FUN_102289e0`. If **101**, BM give-all-weapons `FUN_10474ea0`. **No case 100.**

**`CBasePlayer::CheatImpulseCommands` = `FUN_102289e0`:** `sv_cheats` then switch **76, 81–83, 101–103, 106–108, 195–197, 202–203, 207**. **No 100.** (`0x65` is HL2 give-weapons, not flashlight.)

#### 2. `m_bFlashlightEnabled` — server offset ≠ client offset

SendProp in `DT_BlackMesaLocalPlayerExclusive` (`FUN_1047cc40`):

- `m_bZooming` **+0x1374**
- **`m_bFlashlightEnabled` +0x1375** (1 byte)

Client RecvProp was **+0x17E8**. Names match over the net. **Do not poke client+0x1375 or server+0x17E8.**

**Setter `SetFlashlightEnabled(bool)` = `FUN_104791e0`:** writes `this+0x1375` with `NetworkStateChanged` (`FUN_10054a30`). Only DATA xrefs = vtables (`10726928`, `1067c910`). Slot **`vtable+0x5D0`**.

**Constructor `FUN_1047d750` sets `+0x1375 = 1`** if it is not already 1. Permission therefore defaults **on** at spawn. Impulse 100 does **not** write this byte. `EquipSuit` `FUN_10473ea0` → `FUN_1022b790` only sets **`m_bWearingSuit` +0x94d = 1**; it does not call `SetFlashlightEnabled`.

#### 3. Flashlight virtuals (CBlackMesaPlayer vtable `10726358`)

Same four functions on the HL2-derived vtable at `1067c910`–`1067c91c`. **No CODE xrefs** — only virtual (Ghidra cannot name `CALL [reg+0x5D8]` as xrefs to the function).

| Slot | Addr | Function | Role |
|---|---|---|---|
| `+0x5D0` | `10726928` | `FUN_104791e0` | Set `m_bFlashlightEnabled` |
| `+0x5D4` | `1072692c` | `FUN_1047ea80` | **FlashlightIsOn:** `(m_fEffects >> 2) & 1` |
| `+0x5D8` | `10726930` | `FUN_1047eb70` | **FlashlightTurnOn** |
| `+0x5DC` | `10726934` | `FUN_1047ea90` | **FlashlightTurnOff** |

**`m_fEffects` at +0xc4.** Bit 2 (value **4**) = **`EF_DIMLIGHT`**. Client used `this+0x80` for the same flag (layout differs). `AddEffects` = `FUN_1011d730` (OR into `+0xc4`).

#### 4. TurnOn / TurnOff gates (`FUN_1047eb70` / `FUN_1047ea90`)

**TurnOn** requires **all** of:

1. **`this+0x1375 != 0`** (`m_bFlashlightEnabled`) — permission already true; **TurnOn does not set it**
2. **`this+0xe8 == 0`** — `m_lifeState` / LIFE_ALIVE (`FUN_10069ff0` is the same test)
3. **`this+0x94d != 0`** — `m_Local.m_bWearingSuit` (`m_Local` at **+0x8BC**; 0x8BC+0x91=0x94D)
4. **`g_pGameRules` (`DAT_1086c5cc`) `vtable+0xe0` nonzero** — flashlight allowed
5. **EF_DIMLIGHT not already set**

Then: `AddEffects(4)`, optional sound **`HL2Player.FlashlightOn`**, FireOutput **`OnFlashlightOn`** via `logic_playerproxy` (`FUN_10474ca0` / `FUN_10474700`).

**TurnOff:** if EF_DIMLIGHT set, `*effects &= ~4` (`0xfffffffb`), IsAlive, sound **`HL2Player.FlashLightOff`**, FireOutput **`OnFlashlightOff`**. **Does not clear +0x1375.**

Bool and effect bit are **separate**. Server never creates GB RTs. Render is **client-only**. Server sets **permission bool + `EF_DIMLIGHT`**, not a projected-texture entity for Gordon.

PreThink `FUN_10472c00` calls **FlashlightIsOn** (`+0x5D4`) then NPC `+0x5E0` — “flashlight illuminating this NPC”, **not** a toggle. Darkness strings (`TLK_DARKNESS_*_FLASHLIGHT*`) are NPC talker. `on_flashlight_illum` is NPC dynamic-interaction (`FUN_1006dd50`).

#### 5. Gamerules `+0xe0` (flashlight allowed / hide HUD)

`DAT_1086c5cc` = `g_pGameRules` (set in `CGameRules` ctor `FUN_101aa650`, vtable `10624314`). `CGameRules` slot `+0xe0` (`106243f4`) is **purecall**. Overrides:

| Class | Vtable | `+0xe0` | Body |
|---|---|---|---|
| `CSingleplayRules` | `1065fe1c` | `FUN_1027f6a0` | **`return 1;`** always |
| `CMultiplayRules` | `10638a54` | `FUN_101d3d90` | **`mp_flashlight` GetInt() != 0** (`DAT_1086adf4` `+0x2c`) |
| `CTeamplayRules` | `10666c38` | same `FUN_101d3d90` | same ConVar |

`InitGameRules` `FUN_102cdd30`: maxplayers==1 → `"CBM_SP_GameRules"`; else coop/teamplay/`CBM_MP_GameRules`. CBM_SP factory site `10361334` **calls** `CSingleplayRules` ctor `FUN_1027f4f0`. **Strong inference:** SP campaign uses always-allow. MP uses **`mp_flashlight`**. Did not recover a CBM_SP vtable overwrite of `+0xe0` (no defined function at `10361310`).

PreThink `FUN_10477fa0`: if `+0xe0` is **false**, sets **`m_iHideHUD` bit 1 (`HIDEHUD_FLASHLIGHT` = 2)** at **+0x8f8** (`m_Local+0x3c`). If true, clears that bit.

#### 6. Other gates (not on TurnOn itself)

- **`FL_FROZEN`:** `m_fFlags +0x108` bit `0x40` in `FUN_10231290` **zeros usercmd impulse** — 100 never arrives.
- **`m_afPhysicsFlags +0xb54`:** BM `PlayerRunCommand` can strip jump/move bits (`0xc0`); not the flashlight effect bit.
- **Dead:** PostThink clears `m_nImpulse`.
- **No extra “must be game thread” string** — standard usercmd → RunCommand → ItemPostFrame.

#### 7. Console commands vs impulse

**No `flashlight` / `toggle_flashlight` player command.**

- `"flashlight"` `10722054`: HEV-zombie attachment / `flashlight_status` sprite (`FUN_1046a7b0`). **Not** Gordon’s beam.
- `m_flashlightStatusNetwork` SendProp on **`DT_NPC_Zombie_Hev`**.
- `InputSetFlashLightState` / `FlashLight_Shadows`: zombie I/O, not player.
- **`CBlackMesaPlayer::ClientCommand` `FUN_10472f60`:** `jointeam`, `playbootupsound`, `playLongJumpbootupsound`, then `FUN_102299a0`. **No flashlight.**
- **`CBasePlayer::ClientCommand` `FUN_102299a0`:** spectate/vehicle/vote/playerperf. **No flashlight / impulse.**
- Console `impulse 100` still only sets usercmd impulse → same ignored server handler.

`item_suit` give (`FUN_1014a120`) calls EquipSuit `+0x730` with 0 — suit only.

#### 8. BM-specific vs HL2

Present beyond typical HL2 `CBaseHLPlayer`:

- Networked **`m_bFlashlightEnabled` +0x1375** + `SetFlashlightEnabled`
- **`CBlackMesaPlayer::CheatImpulseCommands`** (impulse **101** give-all BM weapons)
- **`OnFlashlightOn` / `OnFlashlightOff`** via `logic_playerproxy` (`CLogicPlayerProxy` datadesc `FUN_1032d690`)
- HEV suit gate on TurnOn
- Gamerules `+0xe0` + `HIDEHUD_FLASHLIGHT`
- Sounds: `HL2Player.FlashlightOn` **and** `HL2Player.FlashLightOn` / `FlashLightOff` (two spellings)

**Missing vs HL2:** impulse-100 in ImpulseCommands; `m_flNextFlashlightCheckTime` string `10727090` had **zero xrefs** (leftover); no `CheckFlashlight` string; no `+flashlight` bind string.

**Not flashlight:** PreThink `+0x804` `FUN_10472cb0` is **IN_ZOOM (`0x80000`)** → zoom virtuals writing **`m_bZooming +0x1374`**, suit required. Adjacent bool, different feature.

### Why the beam can still be missing (even when BMVR applies impulse 100)

1. **Server ImpulseCommands ignores 100.** It never toggles flashlight. That is the main server-side reason.
2. **TurnOn never runs from that impulse**, so **`EF_DIMLIGHT` may stay off.**
3. **`m_bFlashlightEnabled` is a permission bit TurnOn checks.** Constructor already sets it to 1. Impulse 100 does not flip it. Watching client **+0x17E8** is the wrong test for “did the light toggle.”
4. **Wrong offset:** client bool **+0x17E8**, server **+0x1375**. Client `EF_DIMLIGHT` **+0x80**, server **+0xc4**.
5. **HEV suit (`+0x94d`), gamerules `+0xe0`, alive (`+0xe8`), frozen** still gate TurnOn / impulse delivery. Early maps without suit, or MP with `mp_flashlight` 0, block TurnOn even if something else called it.
6. **Visible beam is client render** (projected texture + `_rt_gbShadowMapFlashLight`). Server only replicates bool + effect flag. Stereo/GB/camera issues from §8 can still hide a beam **after** server state is correct.

Bindings / CreateMove impulse are **not** the remaining question.

### Unknown (this DLL, not blocking implementation)

- Exact **vanilla** caller of `FlashlightTurnOn` / `TurnOff` (virtual `+0x5D8/+0x5DC` only). Retail BM can still light up some other way; it is **not** impulse 100.
- Whether **`CBM_SP_GameRules` overwrites `+0xe0`** after `CSingleplayRules` ctor (always-1 is the inherited body).
- Who later calls `SetFlashlightEnabled(false)` after ctor sets 1 (if anyone).

### Not in this DLL / do not reopen

- Resolution: still closed at `SetRenderTargetFrameBufferSizeOverrides` (§11–§12). No contradiction here.
- HUD `_rt_gui` / `VGui_Paint`: **`engine.dll` done, §14.** Gameplay HUD is client RenderView DRAWHUD only.

---

## 14. `engine.dll` findings (HUD / VGUI, 2026-08-18)

Image base `0x10000000`. **Confirmed this is `engine.dll`:** `CEngineVGui::Paint` (`1036b9ac`), `CEngineVGui::Simulate` (`1036b994`), `VEngineVGui001` (`1036b50c`), `_Host_RunFrame_Input` (`10356cb8`), `engine.dll` (`10357288`). No `CBlackMesaPlayer` / `m_nImpulse`. Function names mostly stripped; RTTI labels exist (`.?AVCEngineVGui@@` `10402cd8`, `.?AVCVRenderView@@` `10400150`).

No strings: `VGui_Paint` (the export name is not a literal), `SCR_UpdateScreen`, `PAINT_UIPANELS`, `PAINT_INGAMEPANELS`, `_rt_gui`, `_rt_Hud`, `mat_viewportscale`, `IsGameUIVisible`. `r_drawvgui` is present (`1036a800`). Engine `_rt_*` names are FullFrame / water / cubemap / small FB only.

BMVR `offsets.h` `VGui_Paint` RVA **`0x238C50`** matches `CEngineVGui::Paint` `FUN_10238c50`. The hook is the **thiscall implementation**, not L4D2VR’s cdecl wrapper. Signature `dVGui_Paint(ecx, edx, mode)` is correct for this binary.

### Confirmed from Ghidra

#### 1. `CEngineVGui::Paint` = `FUN_10238c50` (RVA `0x238C50`)

VPROF string `"CEngineVGui::Paint"`. `thiscall`, `param_1` = `PaintMode_t`. Vtable slot **`+0x34`** (slot 13; slot 0 is the MSVC destructor).

Does **not** FindTexture, PushRT, PopRT, or name `_rt_gui` / `_rt_Hud`. Draws into whatever RT is **already bound**. Viewport comes from `IMaterialSystem+0x19c` `GetRenderContext` → context **`+0x9c` `GetViewport(x,y,w,h)`**. `staticPanel` (`this+0x5c`) `SetBounds(0,0,w,h)` via `FUN_10299e10`. Then `Repaint` (`+0x10`).

Gates: `staticPanel != 0`; `ISurface+0x24` `GetEmbeddedPanel` nonzero; `r_drawvgui` (and not time-demo with vgui off); `this+0xb4` bit 1 (`m_bNoShaderAPI` / `-noshaderapi`) clear.

| `param_1` bit | SDK name | What BM does |
|---|---|---|
| **1** | `PAINT_UIPANELS` | Hide `staticClientDLLPanel` (`this+0x60`) and `staticClientDLLToolsPanel` (`this+0x64`). `ISurface+0x1dc` **`PaintTraverseEx(embeddedPanel, true)`** — GameUI, console, root tree, **not** client HUD RT. Restore visibility. |
| **2** | `PAINT_INGAMEPANELS` | Hide embedded panel. Isolate `staticClientDLLPanel` / tools (`IPanel` SetParent 0), `PaintTraverseEx` each, restore parent/visibility. Client **VGUI children** of that panel only. |
| **4** | `PAINT_CURSOR` | `ISurface+0x270` software cursor. |

`PTR_DAT_103f8548 +0x94 / +0x98` around those blocks is **toolframework** `VGui_PreRenderAllTools` / `PostRenderAllTools` (same pointer as Simulate’s Pre/PostSimulate), **not** client HUD paint.

`BackwardCompatibility_Paint` `FUN_102373e0` (vtable `+0x28`): `this->Paint(3)` = UIPANELS\|INGAMEPANELS, no cursor.

#### 2. Callers vs client `RenderView`

`EngineVGui()` = `FUN_10237b70` → object `0x103f87c8`.

| Function | Addr | Role |
|---|---|---|
| `Host_RunFrame` | `FUN_101be9e0` | Tick loop: Input `FUN_101bf530`, client sim, then **render** `FUN_101bf630` (not dedicated). Does **not** call Paint directly. |
| `SCR_Init` | `FUN_101108b0` | Sets `DAT_108e539d = 1` (`scr_initialized`). |
| `SCR_UpdateScreen` | `FUN_101109b0` | If not `scr_disabled_for_loading`: `materials->BeginFrame` (`+0xa0`), **`EngineVGui()->Simulate` (`+0x88`)**, `ClientDLL_FrameStageNotify(5)` `FUN_100a52f0` (`IBaseClientDLL+0x94`, **FRAME_RENDER_START**), renderer/tool `FrameBegin`, **`V_RenderView`**, **`CL_TakeSnapshotAndSwap`** `FUN_100c2240`, `FrameStageNotify(6)` **FRAME_RENDER_END**, `materials->EndFrame` (`+0xa4`). |
| `V_RenderView` | `FUN_1014f190` | If world + `cl` signon **6** + `toolframework+0xa8` ShouldGameRenderView: `videomode+0x58` **GetClientViewRect**, then **`g_ClientDLL+0x70` `View_Render(&vrect)`**. That is client `CBlackMesaViewRender` / `RenderView` (DRAWHUD / `_rt_gui`). **This branch does not call Paint.** Else **`V_RenderVGuiOnly_NoSwap`**. Then `FullViewColorAdjustment` `FUN_1014ea70` (optional `_rt_FullFrameFB1` + `dev/red_green_projection` — anaglyph, not HUD). |
| `V_RenderVGuiOnly_NoSwap` | `FUN_1014f120` | `ClearBuffers(1,1,0)` then **`EngineVGui()->Paint(5)`** = `PAINT_UIPANELS\|PAINT_CURSOR`. Menu / loading / not-yet-active client. |
| `V_RenderVGuiOnly` | `FUN_1014f0b0` | BeginFrame, Simulate, FrameBegin, NoSwap, FrameEnd, EndFrame, `Shader_SwapBuffers` `FUN_10110be0` (`IMaterialSystem+0xac`). |
| `SCR_BeginLoadingPlaque` | `FUN_10110760` | `OnLevelLoadingStarted`, **two** `SCR_UpdateScreen`s, sets `scr_disabled_for_loading`. |
| `IVRenderView::VGui_Paint` | `FUN_1014f000` | Trampoline: `EngineVGui()` then **`JMP [vtable+0x34]`** with the caller’s `mode`. `CVRenderView` vtable slot `10344454`. **`VEngineRenderView014`**. This is how **client** `render->VGui_Paint(mode)` reaches `CEngineVGui::Paint` **during** `View_Render` (GameUI/pause over the world). Engine’s own in-game `V_RenderView` does not call it. |

Order for an in-game frame:

1. `Host_RunFrame` → `SCR_UpdateScreen`
2. `CEngineVGui::Simulate` (anim / GameUI `RunFrame` / `ivgui()->RunFrame` / HWND viewport)
3. `FRAME_RENDER_START`
4. `View_Render` → client `RenderView` (**DRAWHUD `_rt_gui` lives here**)
5. Client may call `render->VGui_Paint` → `FUN_1014f000` → `CEngineVGui::Paint` (GameUI onto **current RT / backbuffer**)
6. Snapshot / `SwapBuffers`

#### 3. `_rt_gui` / `_rt_Hud` are **client-only**

`list_strings` filter `_rt_gui` and `_rt_Hud`: **empty**. Engine `FindTexture` helper `FUN_100d1ab0` (`IMaterialSystem+0x150`) is the `mat_texture_save` path (`SaveTextureImage`), not HUD. Engine RT use in this DLL: `_rt_FullFrameFB` / `_rt_FullFrameFB1` (PFM screenshot `FUN_10220f00`, anaglyph `FUN_1014ea70`). **No PushRT of HUD targets.**

**Confirmed:** extra `VGui_Paint` cannot fill `_rt_gui` / `_rt_Hud`. Those are filled only by client `RenderView` with `RENDERVIEW_DRAWHUD`.

#### 4. Pause / GameUI onto the backbuffer

Panels built in `CEngineVGui::Init` `FUN_10237eb0` (first `SetBounds` from `videomode+0x50/+0x54` mode width/height):

| `this+` | Name | `GetPanel` type |
|---|---|---|
| `+0x5c` | `staticPanel` | `PANEL_ROOT` (0, default) |
| `+0x60` | `staticClientDLLPanel` | `PANEL_CLIENTDLL` (2) |
| `+0x64` | `staticClientDLLToolsPanel` | `PANEL_CLIENTDLL_TOOLS` (6) |
| `+0x68` | `GameUI Panel` | `PANEL_GAMEUIDLL` (1) |
| `+0x6c` | `staticGameDLLPanel` | `PANEL_GAMEDLL` (5) |
| `+0x70` | `Engine Tools` | `PANEL_TOOLS` (3) |

`GetPanel` = `FUN_10237ca0` → switch `FUN_10237d10`. Case 4 (`PANEL_INGAMESCREENS`) falls through to ROOT.

**`-oldgameui`:** if **absent**, `GameUI011` is `CreateInterface` from **`client.dll`**. If **present**, loads `GameUI.dll` (`EXECUTABLE_PATH`). IGameUI lives at `DAT_109dc1f0`. Either way, **paint is still engine `PaintTraverseEx`**, not `_rt_gui`. Stay out of GameUI.dll for this pass.

`gameui_activate`: Key_Event `FUN_10238900` (ESC-ish) `Cbuf_AddText("gameui_activate")`. **`ActivateGameUI` `FUN_10237340`** (vtable `+0x38`): skip if `this+0xb4` bit 3 (not-allowed-to-show); `GameUI Panel` `SetVisible(1)` + MoveToFront; **hide** `staticClientDLLPanel`; `IGameUI+0x1c` ActivateGameUI. **`HideGameUI` `FUN_10237dc0` (`+0x3c`):** reverse visibilities; `IGameUI+0x20`. **`IsGameUIVisible` `FUN_102388d0` (`+0x08`):** GameUI panel `IsVisible` (`+0x8c`).

Init also `ActivateGameUI()` at the end so the menu is up at boot. Loading plaque can `ActivateGameUI` again (`FUN_10238b80` `OnLevelLoadingStarted` if `this+0x88`).

**Where pause pixels land:** `PAINT_UIPANELS` paints the embedded VGUI tree (GameUI visible, client panel hidden) into the **currently bound RT**. After an in-game `View_Render` that is the **backbuffer**, not `_rt_gui`. That is the competing write vs BMVR’s 1584 ColorFill.

#### 5. Viewport vs FullFrame (`mat_viewportscale`)

**`mat_viewportscale` is not in this DLL.** `mat_vrmode_adapter` is (`10340a94`) — adapter/windowed, not HUD layout (same idea as materialsystem §11).

`CEngineVGui::Simulate` `FUN_10239250` (vtable `+0x88`): if the game HWND is not iconic, **`GetClientRect(*pmainwindow)`** for w/h; else videomode size. Then `GetRenderContext`, **`Viewport(0,0,w,h)`** at context **`+0x98`**. GameUI `RunFrame` (`IGameUI+0x18`), `ivgui()->RunFrame`. VGUI **layout is window-sized**, independent of `_rt_FullFrameFB` / override API.

`Paint` then trusts **`GetViewport`**, i.e. the bound RT’s viewport, not FullFrame and not OpenVR recommended size. Lying `GetScreenSize` to 1584 while Simulate still uses a 2560 client rect is exactly the VGUI/mouse/pillarbox class of bugs. Keep VGUI at **HWND** size; grow FullFrame via the override API (§11).

`r_drawvgui` master switch inside Paint. `Shader_SwapBuffers` is `IMaterialSystem+0xac` only — no VGUI blit there.

### Why extra `VGui_Paint` cannot restore gameplay HUD

**Confirmed:** `CEngineVGui::Paint` never binds `_rt_gui` / `_rt_Hud`. `PAINT_INGAMEPANELS` only walks `staticClientDLLPanel` VGUI children. Black Mesa’s health/weapon/HUD composite is the **client** DRAWHUD block (`0x1020EE40` / downsample `0x10267420`). Extra paint with `PAINT_UIPANELS|INGAMEPANELS|CURSOR` into `bmvrHUD` can copy **GameUI / engine VGUI**, not that HUD RT.

### Remaining Unknowns (not blocking the three implementation items)

- Exact **client.dll** call site of `render->VGui_Paint` (engine trampoline is confirmed; client was not reloaded). Pause-over-world requires that call (or equivalent) because in-game `V_RenderView` itself does not Paint.
- Whether any `staticClientDLLPanel` child is a leftover HL2 HUD element vs BM’s `_rt_gui` path. Even if some exist, they are not the `_rt_Hud` downsample.
- `sourcevr.dll` contents (file exists now; not loaded). Client already has `ISourceVirtualReality` consumers that no-op if null.
- Vanilla caller of server `FlashlightTurnOn` (still §13). Does not reopen impulse 100.

### Closed by this DLL / do not reopen

- Resolution: no contradiction with `SetRenderTargetFrameBufferSizeOverrides`. Engine `mat_vrmode_adapter` is not a size API. Do **not** retry `hmd_swap`.
- Flashlight: no impulse-100 / `EF_DIMLIGHT` handler here.
- **Ghidra pause for HUD/resolution/flashlight research is closed.** Next step (when asked): implement DRAWHUD restore + framebuffer-size override + `EF_DIMLIGHT` flashlight. **Do not implement in this pass.**

