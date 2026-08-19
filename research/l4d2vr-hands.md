# L4D2VR independent VR hands

Primary source: vendored `third_party/l4d2vr/L4D2VR`. Upstream: https://github.com/keyou91/l4d2vr.

Evidence tags: **Confirmed from source** / **Strong inference** / **Unknown**.

This document answers *how* L4D2VR represents hands independently of the traditional first-person weapon/viewmodel. It does **not** mean “the game has hands.” L4D2VR has **two local-hand pipelines** that can replace or IK the original arms, plus a **world-model pose relay** for other players. Default sample config does **not** draw the independent GLB gloves.

---

## 1. Architecture answer (read this first)

**Confirmed from source.** L4D2VR does **not** spawn Source entities for VR hands.

There are three related systems:

| System | What it is | Default sample config | Source |
|---|---|---|---|
| **GLB gloves (`VrHandSystem`)** | Independent D3D9 skinned meshes loaded from SteamVR `vr_glove_*.glb`, posed by OpenVR skeletal input (ozz used only to *build* the skeleton at init). Drawn into the eye RT after/around the Source viewmodel. | Off (`VrHandsGlovesEnabled=false`) | `vr_hands/vr_hand_system.cpp`, `vr_hand_skeleton_runtime.cpp`, `vr_hand_renderer_d3d9.cpp` |
| **Native viewmodel hands (`NativeViewmodelHandsOnly`)** | Keeps the original `CBaseViewModel` / `models/weapons/arms/` studio models. `DrawModelExecute` crops, freezes, or full-arm-IKs ValveBiped bones to the controllers and optionally applies OpenVR finger curls onto those bones. | On (`NativeViewmodelHandsOnly=true`) | `hooks/hooks_misc.inl`, `vr_hands/vr_hand_vr_bridge.cpp` |
| **World-model pose relay** | Packs HMD + anatomical left/right controller poses (and finger-curl flags) for *other players’* survivor world models. Not the local first-person hand mesh. | On via `WorldModelVRPoseEnabled` | `game.cpp` pose pack, `hooks/hooks_world_pose.inl` |

Config loader (`vr/vr_viewmodel_config.inl` 1862–1911):

- `m_VrHandsEnabled = VrHandsEnabled && VrHandsGlovesEnabled` (left-handed mode forces both true).
- If gloves are on: `NativeViewmodelHandsOnly = false`, **`HideArms = true`**.
- If `VrHandsEnabled` but gloves off: **`NativeViewmodelHandsOnly = true`**, `HideArms = false`.
- Sample defaults (`vr_config_overlay_embedded.inl` 2261–2281): `VrHandsEnabled=true`, `VrHandsGlovesEnabled=false`, `NativeViewmodelHandsOnly=true`.

So the *named* “independent VR hands” renderer is the GLB path. The *shipping default* independent-looking local hands are **native viewmodel arms with analytic IK**, not GLB gloves.

If GLB init fails, `VR::FallbackVrHandsGlovesToNative` (`vr_hand_vr_bridge.cpp` 7636–7652) turns gloves off and forces `NativeViewmodelHandsOnly`.

---

## 2. Representation: not Source entities

**Confirmed from source.**

`VrHandSystem` owns:

- two `HandState`s (`vr_hand_system.h` 109–119): action path, GLB `VrHandMeshAsset`, `VrHandSkeletonRuntime`, skinning palette.
- two `ViewmodelPoseState`s for “glove follows Source viewmodel palm” mode.
- a `VrHandRendererD3D9` that uploads VB/IB/texture and draws with custom VS/PS.

No `CreateEntity`, no `C_BaseViewModel` for the gloves, no studiohdr for the GLB mesh. The GLB is a raw mesh + joint names + inverse-bind matrices (`vr_hand_types.h`).

Native-hands mode *does* use the existing Source viewmodel / arms models. Those remain `CBaseViewModel` / player viewmodel draws; L4D2VR rewrites their bone matrices in `dDrawModelExecute`.

---

## 3. Creation, assets, render insertion

### 3.1 Assets

**Confirmed from source.** Constructor (`vr_hand_system.cpp` 410–416):

| Hand index | Gameplay meaning | SteamVR action | GLB filename |
|---|---|---|---|
| 0 | physical left | `/actions/base/in/skeleton_lefthand` | `vr_glove_left_model.glb` |
| 1 | physical right | `/actions/base/in/skeleton_righthand` | `vr_glove_right_model.glb` |

Path (`ResolveSteamVrAssetPath`, 447–467):

```text
{VR_GetRuntimePath()}/resources/rendermodels/vr_glove/{fileName}
```

Loaded with cgltf (`VrHandAssetLoader::LoadGlb`). Manifest already contains the skeleton actions (`SteamVRActionManifest/action_manifest.json` 34–41; `vr_hand_manifest.cpp` is a no-op that returns the original path).

`VR_HANDS_SETUP.md` states ozz 0.17.0 is required because init builds an ozz runtime skeleton from the OpenVR bone hierarchy. **Confirmed:** `VrHandSkeletonRuntime::Initialize` uses `ozz::animation::offline::SkeletonBuilder`. **Also confirmed:** `Update` then prefers OpenVR model-space bone matrices / skeletal *summary* and does **not** evaluate ozz animation clips each frame (`vr_hand_skeleton_runtime.cpp` 419–462, 464–523). Ozz is a hierarchy builder, not a clip player.

### 3.2 When the system is constructed

**Confirmed from source.** `VR::DrawVrHandsForEyeImmediate` / `BeginVrHandsEyeRender` (`vr_hand_vr_bridge.cpp` 7725–7726, 8111–8112):

```cpp
if (!m_VrHands)
    m_VrHands = std::make_unique<VrHandSystem>();
```

Lazy, first eye that actually needs gloves or debug boxes.

### 3.3 Where it is rendered

**Confirmed from source.** Per-eye pipeline:

1. `BeginVrHandsEyeRender` stores the active `CViewSetup` (`8092–8126`).
2. `DrawVrHandsWorldDepthMaskBeforeViewmodel` draws `VrHandDrawPass::WorldVisibilityMask` (stencil) so the Source viewmodel can punch a hole / occlude (`8128–8151`).
3. After the viewmodel, `FinishVrHandsEyeRender` draws either `ViewmodelComposite` (if the mask ran) or `WorldDepth` (`8154–8179`).

Queued `mat_queue_mode != 0`: draws are queued via `QueueVrHandsDrawForEye` (`vr/vr_lifecycle_update.inl`) onto Source’s material call queue. Immediate D3D9 draws bind the eye surface (`m_D9LeftEyeSurface` / `m_D9RightEyeSurface`).

`VR_HANDS_SETUP.md` originally said gloves only work at `mat_queue_mode == 0`. Current code supports queued insertion when `allowQueuedMode` is true. Immediate path still returns false if `queueMode != 0 && !allowQueuedMode` (`7662–7684`).

Draw passes (`vr_hand_renderer_d3d9.h`):

- `WorldDepth` — world FOV/near/far, writes depth.
- `WorldVisibilityMask` — stencil for viewmodel occlusion (`kVrHandOcclusionStencilBit = 0x80`).
- `ViewmodelComposite` — viewmodel FOV/depth range, stencil-tested against the mask.
- `ViewmodelStandalone` — viewmodel depth, **no** VR-hand stencil (detached magazine / left-hand-on-mag).

Shader bone cap: **64** (`kMaxShaderBones`). Palettes larger than that skip the draw.

Light: directional approx `{0.35, -0.45, -0.82}` with ambient `0.14`, scaled by scene luma (`vr_hand_renderer_d3d9.cpp` 468–470).

---

## 4. Bones and animation

### 4.1 OpenVR skeletal path (standalone glove)

**Confirmed from source.** `UpdatePoses` (`vr_hand_system.cpp` 1210–1273):

1. `GetSkeletalActionData` must be active.
2. `GetSkeletalSummaryData(..., VRSummaryType_FromAnimation)` → `flFingerCurl[5]` in `[0,1]`.
3. `GetSkeletalBoneData(..., VRSkeletalTransformSpace_Model, motionRange)`.
4. `BuildSkinningPalette`:
   - **Preferred:** `BuildSummaryCurlPalette` — start from GLB bind-local matrices, apply local-Z flexion `curl * kMaxCurlRadians[finger][segment]`, concatenate parent→model, `palette = model * inverseBind`.
   - **Fallback:** map GLB joint names onto OpenVR bones and use model-space `VRBoneTransform_t` directly.

`kMaxCurlRadians` (`vr_hand_skeleton_runtime.cpp` 184–191):

| Finger | proximal / middle / distal (radians) |
|---|---|
| thumb | 0.75 / 0.90 / 0.65 |
| index–pinky | 1.15 / 1.25 / 0.90 |

Motion range (`12140–1242`): `VRSkeletalMotionRange_WithController` or `WithoutController` from `m_VrHandsMotionRangeWithoutController`.

Magazine-held override (`1244–1266`): if left-hand mag grip, clamp curls to

```text
min = {0.34, 0.60, 0.66, 0.68, 0.68}
max = {0.58, 0.82, 0.88, 0.90, 0.90}
```

applied to the **physical** off-hand (`leftHanded ? 1 : 0`).

Global per-finger cap: `m_VrHandsGloveFingerMaxCurl` (default all `1.0`).

### 4.2 Viewmodel-pose path (glove glued to Source palm)

**Confirmed from source.** When `m_VrHandsRightUseViewmodelPose` and/or two-handed grip is active, `BuildViewmodelPalette` (`910–1159`):

1. Snapshot from `VrHandVmPose::GetLatest(..., maxAgeMs=500)` (`kVrHandsVmPoseMaxAgeMs`).
2. Source palm bone: `ValveBiped.Bip01_L_Hand` or `_R_Hand`.
3. Align SteamVR glove wrist (`wrist_l` / `wrist_r`) to the Source palm using a basis built from four finger-root positions (index–pinky).
4. Compute finger curl from Source finger bones in palm space (`ComputeVrHandsVmFingerCurl`):

```text
segment0 = bone0 - palm
segment1 = bone1 - bone0
segment2 = bone2 - bone1
proximal = max(0, angle(segment0,segment1) - 0.08 rad)
middle   = max(0, angle(segment1,segment2) - 0.08 rad)
curl[0] = clamp(proximal * proximalScale, 0, maxProximal)
curl[1] = clamp(middle   * middleScale,   0, maxMiddle)
curl[2] = clamp(middle   * distalScale,   0, maxDistal)
```

Thumb scales: `0.45 / 0.75 / 0.50`, max `0.75 / 0.90 / 0.65`. Other fingers use struct defaults `0.85 / 1.05 / 0.70` and max `1.15 / 1.25 / 0.90`.

5. Apply those curls as local-Z rotations on `finger_{thumb,index,middle,ring,pinky}_{0,1,2}_{l|r}`.
6. Skinning palette is **bind-pose glove + VM-derived curl**, not a retarget of every ValveBiped bone onto the GLB.

Snapshot capture (`hooks_misc.inl` `MaybeCaptureVrHandsVmPose` 8764–8824) runs from `DrawModelExecute` when drawing arms/hands models **or** an AutoGrip-aligned viewmodel. It copies `pCustomBoneToWorld` (already world-space studio bones) plus `modelInfo.origin/angles`.

### 4.3 Native viewmodel hands (no GLB)

**Confirmed from source.** OpenVR curls are sampled into `m_NativeViewmodelLeftHandOpenVRFingerCurls` / right (`vr_hand_vr_bridge.cpp` 4468–4573) and published on the world-pose snapshot (`vr_tracking.inl` 448–518). A cache older than **0.35 s** is treated as stale. `DrawModelExecute` then:

- **Wrist clip** (`NativeViewmodelArmCroppingEnabled`, default false in sample, true in `vr.h` 1779): hide forearm by collapsing bones past a cut plane.
- **Full-arm analytic IK** (`HooksNativeViewmodelFullArmIkActive`): two-bone IK from shoulder to controller/weapon palm. Shared solver also used for world-model arms (`VR_HANDS_SETUP.md`).
- Shoulder anchors: `m_NativeViewmodel{Left,Right}ArmAnchorOffsetMeters` / `...RotationOffsetDeg`, extra spacing `m_NativeViewmodelArmShoulderSpacingOffsetMeters`.
- Elbow pole: `m_NativeViewmodelArmElbowPoleBias = {-0.05, 0.20, -1.0}` (torso-local).
- Cut rotations (sample): left `{0,-25,0}`, right `{8,-25,0}` deg.
- Freeze-pose lock for the unused off-hand: offset `{0.55, 0.18, -0.18}` m.

World-model / remote: `leftFingerUsesNativeAnimation` is true for the **support** hand when two-handed grip is on, and for the **dominant** hand always (swapped in left-handed mode) (`vr_tracking.inl` 453–472). Empty-hands inventory placeholder clears both.

---

## 5. Controller pose → hand pose (transform hierarchy)

Coordinate spaces used everywhere:

- **OpenVR tracking**: meters, compositor space. `TrackedDevicePos` on `m_*Pose`.
- **Source world**: Hammer units. `m_VRScale` default **43.2** (`vr.h` 1691) → `sourceUnitsPerMeter`.
- **Camera anchor**: `m_CameraAnchor` with a hardcoded **64-unit** eye-height subtract (Source player eye offset).

### 5.1 Tracking → Source controller (shared with weapons)

**Confirmed from source.** `VR::UpdateTracking` (`vr_tracking.inl` 1680–1772):

```text
hmdToController = controllerLocalSmoothed - hmdLocal
controllerCorrected = hmdSmoothed + hmdToController
VectorPivotXY(controllerCorrected, hmdSmoothed, m_RotationOffset)   // stick yaw
controllerAbs = CameraAnchor - (0,0,64) + controllerCorrected * VRScale
angles.y += m_RotationOffset
```

Gameplay `m_LeftController*` / `m_RightController*` are **swapped in left-handed mode**. Anatomical snapshot for world-model IK always maps physical left→left (`1752–1763`).

Off-hand aim basis gets an extra **−45°** pitch about local right (`1775–1776`). Weapon-hand basis gets `m_WeaponAimPitchOffsetDeg` (default **−45°**, `vr.h` 396) (`1778–1787`). That −45° is the L4D2VR “gun points along controller” calibration, not a hand-mesh offset.

Two-handed grip, if active, **replaces** the weapon-hand forward/right/up via `ResolvePavlovTwoHandedAimBasis` (see weapons doc). That changes the **gun/aim**, not the standalone GLB left-hand world matrix (left glove still uses left controller pose unless two-handed *viewmodel-pose* is on).

### 5.2 Standalone GLB world matrix

**Confirmed from source.** `VrHandMath::BuildControllerWorld` (`vr_hand_math.h` 121–183):

Let \(F,R,U\) be AngleVectors of the controller QAngle, then:

\[
B = \begin{bmatrix} R & U & -F & origin \\ 0 & 0 & 0 & 1 \end{bmatrix}
\]

Local correction \(C = R_z R_y R_x\) with translation `offsetMeters * sourceUnitsPerMeter` (converted **before** model scale so size changes do not move the calibration).

\[
S = \mathrm{diag}(modelScale \cdot VRScale,\ \ldots,\ 1)
\]

\[
W = B\, C\, S
\]

`modelScale` is `m_VrHandsModelScale`, clamped `[0.25, 4]` at draw (`vr_hand_system.cpp` 1553). Per-hand offsets: `m_VrHands{Left,Right}PoseOffsetMeters` / `...RotationOffsetDeg`, plus extra left-handed addends (`vr.h` 1696–1705).

**Pivot:** the GLB root/wrist bind, not a Source attachment. The matrix maps GLB model space (meters) into Source world.

Skinning in the VS: `skinned = palette[joint] * vertex` then `WVP * worldPos`. Palette is already in glove-model space; `world` places that model.

WVP (`1622–1623`):

```text
camera = BuildSourceView(view.origin, view.angles)   // Source view matrix
proj   = BuildPerspective(fov, aspect, zNear, zFar)  // world or viewmodel FOV
wvp    = proj * camera * world
```

Viewmodel-layer FOV uses `view.fovViewmodel` / `zNearViewmodel` / `zFarViewmodel` and **viewport** aspect, not `view.m_flAspectRatio` (`80–89`, `92–103`). Scene points can be reprojected into the VM layer (`105–149`):

```text
viewX,Y,Z = project (P - eye) onto (right, up, forward)
P_vm = eye + right * viewX * (sceneXScale/vmXScale)
           + up    * viewY * (sceneYScale/vmYScale)
           + forward * viewZ
```

Used when the left glove is holding a magazine that lives in the viewmodel projection.

### 5.3 Viewmodel-anchored GLB world matrix

**Confirmed from source.** `BuildViewmodelWorld` (`1162–1207`):

```text
uniformScale = VRScale * modelScale
palmWorld = autoGripAligned ? snapshot.palmWorld
          : EntityWorld(currentViewmodelPos, currentViewmodelAng) * palmFromModelRoot
W = palmWorld * palmFromGloveWrist * localCorrection * Scale * gloveWristBindInverse
```

`currentViewmodelPos/Ang` = `GetRecommendedViewmodelAbsPos/Angle()` (`vr_hand_vr_bridge.cpp` 7911–7912). So when `VrHandsRightUseViewmodelPose` is on, the **right glove is parented to the Source viewmodel palm**, not to the raw controller. That is how the glove stays glued to a gun that still plays Source sequences.

Two-handed viewmodel pose does the same for the **gameplay left** hand (physical left unless left-handed) using `ValveBiped.Bip01_L_Hand`.

---

## 6. Left vs right, dominant vs off-hand

**Confirmed from source.**

- Physical mesh index 0/1 is always left/right GLB + left/right skeleton action.
- Gameplay “left” / “right” swap when `m_LeftHanded` (`physicalHandIndexForGameplay = leftHanded ? 1 - i : i`, `1498–1501`).
- Dominant weapon hand = gameplay right = `m_RightController*` after the swap.
- Off-hand = gameplay left.

World-model snapshot always publishes **anatomical** left/right (`vr_tracking.inl` 1752–1763).

---

## 7. Off-hand grip / two-handed weapons

**Confirmed from source.** Two-handed is **not** automatic for rifles. It is a runtime flag `m_VrHandsTwoHandedGripActive` (`vr.h` 1876–1882).

Activation lives in `VR::UpdateMagazineInteraction` (`vr_hand_vr_bridge.cpp` ~5497–5799), which also owns physical reload. Requires `m_VrHandsEnabled || m_NativeViewmodelHandsOnly` and an active weapon.

Two modes:

### 7.1 Button / curl grip (default `VrHandsTwoHandedAimMountFriendly=false`)

Off-hand grip input (reload/crouch/jump/attack2 remapped to left, or finger-curl if magazine interaction is in curl mode) plus proximity to a **left-hand target OBB**:

- Box from VM snapshot **left middle finger** bones `Bip01_L_Finger2 / Finger21 / Finger22` (center = average of those translations; orientation from the first found). Fallback: `ValveBiped.Bip01_L_Hand` (`TryBuildLeftHandTargetDebugBox`, age ≤ **500 ms**).
- Half-extent in **Source units**: `max(0.1, 0.05 m * clamp(targetBoxScale, 0.25, 8) * VRScale)`. With defaults that is `max(0.1, 2.16) = 2.16` units (~5 cm), not a 0.05-unit cube.
- `leftHandTouchesTwoHandedGripTarget` = nearest left-hand probe distance ≤ 0.
- Held vs toggle: `m_VrHandsTwoHandedGripHeldMode` (default false = press to latch, press again to release).
- Sets `m_VrHandsTwoHandedGripPistol = !VrHandsLongWeapon(weaponId)` (`5793`). Pistols skip min-distance weighting and virtual stock.

### 7.2 Mount-friendly auto grip (`VrHandsTwoHandedAimMountFriendly=true`)

Left hand must dwell **0.5 s** in a stock/forend OBB (`TryBuildMountFriendlyTwoHandedGripBox`): **0.50 × 0.20 × 0.20 m** along right-controller forward, origin = viewmodel-reprojected `m_RightControllerPosAbs`, then shifted forward by half-length. Grip press **releases**. Manual reload owns the off-hand and cancels this. Magazine-area exclusion padding is **0.006 × VRScale** Source units.

Effect when active (`IsVrHandsTwoHandedGripPoseActive`):

1. **Aim:** `ResolvePavlovTwoHandedAimBasis` blends weapon-hand forward toward `normalize(leftPos - rightPos)` (see weapons doc).
2. **Right glove / native right hand:** `twoHandedViewmodelPose` / `rightUseViewmodelPose` so the gripping hands follow the **viewmodel palm bones**, i.e. they wrap the gun.
3. **World-model:** support-hand fingers use native animation flags.

Off-hand does **not** become a second weapon entity. It supports the single Source viewmodel.

Magazine interaction is a separate state machine: off-hand can grab a **detached clip** (native viewmodel with non-clip bones pushed out of view, optional standalone box). That is reload, not two-handed aim.

---

## 8. Weapon attachment to hand vs hand attached to weapon

**Confirmed from source.** The gun is **not** parented to a GLB bone.

Hierarchy in normal play:

```text
OpenVR right pose
  → Source controller abs + −45° weapon pitch
    → optional two-hand / virtual-stock aim
      → per-weapon viewmodel pos/ang offsets
        → CBaseViewModel origin/angles (CalcViewModelView)
          → Source studio sequences (fire, reload, inspect)
            → optional AutoGrip rigid delta on bones
              → optional right GLB parented to Bip01_R_Hand  (if VrHandsRightUseViewmodelPose)
```

The **weapon stays the Source viewmodel**. Independent gloves either:

- float on the controllers (standalone OpenVR palette), in which case they will **drift from the gun** unless offsets are tuned; comments in `dCalcViewModelView` (`hooks_combat_network.inl` 94–96) say this is why controller hard-lock exists when VM-pose hands are on; or
- parent to the viewmodel palm (VM-pose mode), which is “hand attached to weapon,” not “weapon attached to hand.”

Native-hands IK is the opposite parenting: **arm bones reach toward the controller/palm target** while the weapon root is still controller-placed.

---

## 9. Original first-person arms: hide / replace / bypass

**Confirmed from source.** `dDrawModelExecute` (`hooks_misc.inl` 23706–24630):

```text
hideArms = !emptyHandsPlaceholder && (m_HideArms || (meleeActive && !NativeViewmodelHandsOnly))
```

If `hideArms` and the model path contains `"/arms/"`:

1. Cache the arms model + material.
2. On later draws of that model: `MATERIAL_VAR_NO_DRAW` + `ForcedMaterialOverride`, still call original (so attachments/events are not stripped by skipping the draw entirely).

GLB gloves force `HideArms=true`. Native-hands-only forces `HideArms=false` and instead **rewrites bones** (clip or IK). Melee weapons hide Source arms **unless** native-hands-only is on — melee viewmodels include huge swinging arms that look wrong in VR.

First-person **world-model body** (`FirstPersonBodyEnabled`, sample true): a separate survivor studio model is drawn in first person with head hidden and, if `FirstPersonBodyHideArms=true` (sample true), upper-arm branches frozen/collapsed past `FirstPersonBodyVisibleUpperArmLengthMeters=0.10`. That is a third-person-in-first-person body, not the FP viewmodel arms.

Empty-hands inventory placeholder: `m_ManualInventoryEmptyHandsActive` disables GLB draws and keeps native arms so the dummy pistol / empty pose can show.

---

## 10. How weapon animation continues after VR transforms

**Confirmed from source.** L4D2VR never replaces the viewmodel sequence player.

- `CalcViewModelView` is given the **controller-recommended origin/angles** instead of the HMD eye (`hooks_combat_network.inl` 84–227). The original function still runs, so bob, sway, and **sequence origin** are computed around the gun.
- Hard-lock: if `ViewmodelDisableMoveBob` or VM-pose hands / two-handed grip, L4D2VR **writes abs origin/angles back** after the original (and on queued mode uses `CBaseEntity::SetAbsOrigin`). Sequences still tick; the **root** is forced to the controller.
- `DrawModelExecute` then optionally:
  - AutoGrip: rigid delta so a named grip/palm bone sits on the controller.
  - Native IK / wrist clip on **arm bones only**.
  - Magazine freeze/detach on **clip bones only**.
- Fire/reload events, muzzle flash, and tracers still come from the Source viewmodel attachments.

So: **root transform is VR; local animation is still Source.** Independent GLB fingers are a parallel mesh, not the thing that plays `ACT_VM_PRIMARYATTACK`.

---

## 11. HUD / menu cursor (input architecture only)

**Confirmed from source.** Hands are **not** the menu cursor.

- **SteamVR overlay HUD:** wrist status on off-hand, ammo on gun-hand (`LeftWristHudEnabled` / `RightAmmoHudEnabled`). World quads can attach to controllers (`HandHudWorldQuadAttachToControllers`, sample true, 0.5 m).
- **Lift HUD:** raising the off-hand can show the main HUD (`HudAlwaysHidden` disables only that trigger).
- **Menu / VGUI cursor:** `CheckOverlayIntersectionForController` (`vr_roomscale_prediction.inl` 1314–1337) ray-tests SteamVR overlays from the controller **tip component**:
  - origin = controller pose × tip matrix translation
  - direction = **−Z** of that matrix
  - `IVROverlay::ComputeOverlayIntersection`
- Config overlay comments warn that covering overlays steal the laser hit-test.

This is OpenVR overlay intersection, not Source `IInput` mouse, and not the GLB index finger.

---

## 12. L4D2-specific vs reusable for Black Mesa

**Reusable (mechanism, not assets):**

- Independent D3D9 skinned GLB + OpenVR skeleton/summary curls.
- Controller→Source `CameraAnchor - 64 + tracking * scale` (BM already uses a related 6DOF copy).
- `BuildControllerWorld` / VM-layer reprojection / stencil-then-composite.
- Two-handed aim as a blend of `leftPos - rightPos` vs weapon-hand forward.
- Native studio IK onto existing `v_` arm bones (if BM has usable ValveBiped FP arms).
- Hide-arms via `MATERIAL_VAR_NO_DRAW` rather than skipping the draw.

**L4D2-specific:**

- SteamVR `vr_glove_*.glb` joint names (`wrist_l`, `finger_index_0_r`, …).
- ValveBiped L4D2 viewmodel bone names (`Bip01_L_Hand`, `Bip01_R_Finger1`, …).
- Survivor world-model pose relay, first-person body, terror melee arm hide.
- Magazine-interaction two-hand target box and shotgun/clip reload.
- `CTerrorPlayer` / L4D2 netvar offsets for body IK.
- Sample default: native-hands IK, gloves off.

**Unknown:** whether Black Mesa `v_` arms expose the same ValveBiped finger set; whether BM materials have a clean `/arms/` path for `HideArms`; whether DXVK queued insertion matches L4D2VR’s call-queue hook on BM.

---

## 13. BMVR today (read-only note)

`docs/L4D2VR-MAP.md` lists hands/ozz/pose relay as **not ported**. `src/vr.cpp` uncouples the viewmodel to the right controller (sd805 / Portal 2 style) and does **not** construct `VrHandSystem`. There is no `NativeViewmodelHandsOnly` bone IK in BMVR `src/`.
