# Current investigation (2026-08-18)

Traced L4D2VR (vendored `third_party/l4d2vr`, including `hooks_combat_network.inl` / `vr_tracking.inl` / `vr_lifecycle_*.inl`) and Black Mesa `client.dll` (Ghidra image `0x10000000`) before this implementation pass.

Working systems we must not rewrite: fused stereo, uncoupled 6DoF, `bmvr.cfg` crosshair off.

## 1. L4D2VR motion-control / viewmodel architecture

Controller pose is copied into `m_RightControllerForward/Right/Up`. Per-weapon `viewmodelOffsets` / `meleeViewmodelOffsets` become `m_ViewmodelPosOffset` + `m_ViewmodelAngOffset` (`vr_tracking.inl`). The controller basis is then **yaw/pitch/roll rotated** by those angles.

`GetRecommendedViewmodelAbsPos()` = controller world pos minus `forward*x + right*y + up*z`.
`GetRecommendedViewmodelAbsAngle()` = `VectorAngles(m_ViewmodelForward, m_ViewmodelUp)`.

`dCalcViewModelView` feeds that pose into the engine as the **eye input**, then (queued / hard-lock path) **SetAbsOrigin / SetAbsAngles** to the same target so bob/lag cannot drift the gun off the hand. L4D2VR does **not** use `m_flModelScale`. Crowbar melee offset is `{19.5, 6, -13.5}` pos, `{-24.5, -6.5, -6}` ang.

BM already has the right *space* conversion (`GetViewOrigin(body) + (controller - HMD)`). It was missing L4D2VR’s **angle offsets**, **hard-lock after original**, and a reliable weapon-name source (DrawModelExecute is still unhooked). `m_flModelScale` at `+0x7C0` scales around the mesh camera origin, not the grip — that is why `ViewmodelScale=0.5` did not look smaller in the hand.

Ghidra: `C_BlackMesaViewModel::CalcViewModelView` `0x29D930` ends in `FUN_100af720` (abs origin `this+0x294`) and `FUN_100af600` (abs angles `this+0x2D0`). `DT_BaseEntity.m_nModelIndex` is `+0x94`. `m_hActiveWeapon` is `+0xFA4`.

## 2. L4D2VR melee architecture

Not “fast motion → IN_ATTACK”.

`WriteUsercmd`: if OpenVR `TrackedDeviceVel > 1.1` m/s, `command_number *= -1` (isMeleeing).
`ProcessUsercmds` (after original): if melee weapon and isMeleeing, reset `entitiesHitThisSwing` on new swing, rotate controller forward **50° around right** (blade), fan **10** `TestMeleeSwingCollisionServer` traces along the prev→current arc. `dEyePosition` while `m_PerformingMelee` returns **controllerPos** so traces originate at the hand.

BM has **no** `TestMeleeSwingCollision`. The 10-fan + global `TraceRay` rewrite hitch’d the first swing and teleported NPCs (every probe started at the controller). Current adaptation: L4D2VR velocity gate only (`> 1.1` m/s, HMD-relative forward **or** downward arc, not backward) then pulse `IN_ATTACK`. Hit geometry is the trigger crowbar (`cmd->viewangles` = controller). No `EyePosition` hook.

## 3. L4D2VR HUD / menu / pause

HUD is **not** a FOV inset in the eye. BM **does not PushRT `_rt_gui` during stereo gameplay** (2026-08-18 log: only gbuffer/null). Waiting to StretchRect `_rt_gui` left the overlay hidden; the HMD showed DRAWHUD in the eyes at 16:9 edges. Two-layer health = in-eye blit vs desktop VGUI.

L4D2VR `PaintToHudOnce`: extra `VGui_Paint` into `vrHUD`, then original to the native backbuffer. We extra-paint into `bmvrHUD` with a centered inset viewport (`GetScreenSize` lie only while `HudPaintActive`). Original dest is never stolen. Stereo copies strip `RENDERVIEW_DRAWHUD`. Main menu (`!IsInGame`) is original only.

Pause: extra-paint includes `PAINT_UIPANELS|CURSOR`. Cursor: overlay intersection only, focused, and only while `m_GameUiVisible`.

BM: `vr_hud_max_fov` / `vr_render_hud_in_world` exist (`10440b08` / `10440b38`) but feed `FUN_10105dd0`, which FindTexture(`_rt_gui`) and draws “left eye”/“right eye” quads through **ISourceVirtualReality**. That path never runs for an injected `d3d9.dll` (`UseVR()` is false). `CBlackMesaViewRender_RenderView` references `_rt_gui` (`1020f9a6`).

`gameui_activate` is a real engine command (`10450f10`). VK_ESCAPE PostMessage does not reach Source’s IInputSystem. Queue from ProcessInput; flush `ClientCmd_Unrestricted` from RenderView/CreateMove at **slot 108** (`+0x1B0`). Slot 107 SEH'd. Crash-sticky `gameui`.

HUD overlay submit must match L4D2VR’s queue rules: `TransferSurface` with the eyes under `LockDevice`, `SetOverlayTexture` only while the submission queue is **already** held, never a nested `LockSubmissionQueue`. DXVK `m_mutexQueue` is a non-recursive SRWLOCK. The 2026-08-18 save-load freeze was that nested lock.

| Issue | Adaptation |
|---|---|
| Bindings | Left stick click Sprint; right stick click Recenter; right stick up Jump; right stick down CrouchToggle; Y=next X=prev; flashlight → right grip |
| Viewmodel | L4D2VR pos+ang tables, rotate basis, SetAbsOrigin/angles after original; keep 6DoF delta math |
| Melee | 1.1 m/s HMD-relative forward **or** downward arc; IN_ATTACK only; no TraceRay rewrite; no 10-fan |
| Pause | Queue from ProcessInput; slot 108 on engine thread; extra-paint UIPANELS to overlay |
| HUD | Original VGui dest + extra-paint `bmvrHUD` inset; strip DRAWHUD on stereo eyes; never steal `_rt_gui` |
| Flashlight | Impulse 100 before ProcessInputEnabled return; no cvars |
| Resolution | HWND-fit 1584×1440; VRRenderScale alias; taller-than-window not enabled |
