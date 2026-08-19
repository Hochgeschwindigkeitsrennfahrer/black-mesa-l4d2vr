# Source SDK 2013 weapon / viewmodel architecture (baseline)

**Status:** research-only baseline. Black Mesa is **not** assumed identical.

**SDK clone:** `%TEMP%\source-sdk-2013` (`C:\Users\amien\AppData\Local\Temp\source-sdk-2013`), GitHub [`ValveSoftware/source-sdk-2013`](https://github.com/ValveSoftware/source-sdk-2013) @ `22288b9` (“Remove hud_autoaim.cpp from client_base.vpc”). Paths below are relative to that tree’s `src/`.

**Wiki constraint:** live [developer.valvesoftware.com](https://developer.valvesoftware.com/) is Anubis-blocked for automated fetch. Authoring pages below were recovered from Wayback Machine `id_` snapshots (or Google-indexed wiki text). Tags:

- **Confirmed from source** — read in SDK 2013 C++
- **Confirmed from Valve wiki** — recovered wiki page
- **Strong inference** — follows from the two above, not a literal line
- **Unknown** — not established here; do not treat as BM fact

There is **no** `viewmodel_t` type in SDK 2013. Viewmodels are `CBaseViewModel` / `C_BaseViewModel` entities, classname `"viewmodel"`.

---

## 1. What normal Source architecture expects

Source splits **gameplay weapon** from **first-person display**:

| Piece | Entity | Typical MDL | Who sees it |
| --- | --- | --- | --- |
| Weapon | `CBaseCombatWeapon` | `w_*.mdl` (`playermodel`) | World, 3rd person, dropped pickup |
| Viewmodel | `CBaseViewModel` (owned by player) | `v_*.mdl` (`viewmodel`) | Owner only (plus in-eye spectators) |
| Player 3rd-person hold pose | Player `CBaseAnimating` sequences | Player model | Everyone else |

**Confirmed from Valve wiki** ([Authoring a weapon entity](https://developer.valvesoftware.com/wiki/Authoring_a_weapon_entity), archive 2025-03-29): “The viewmodel is a separate entity. It is created by the player and shared between all weapons.”

**Confirmed from Valve wiki** ([Viewmodel](https://developer.valvesoftware.com/wiki/Viewmodel), archive 2025-12-25):

- A viewmodel is a first-person-only mesh, usually missing unseen faces, extra-detailed on visible faces, and **distorted** for one camera angle.
- Each player has **two** viewmodel entities (dual-wield / off-hand).
- Both entities are transmitted only to their owner.
- Default viewmodel FOV is **54°** vs world FOV **75°**.
- Must bind a sequence to `ACT_VM_IDLE` or the model will not appear.
- Use `$origin` or the gun sits in the centre of the screen.

**Confirmed from source:** `MAX_VIEWMODELS` is **2** (`game/shared/shareddefs.h`). `VIEWMODEL_INDEX_BITS` is **1** (`game/shared/baseviewmodel_shared.h`), matching two slots.

Stock HL2 first-person is **one camera-locked prop**: arms + gun in a single `v_*.mdl`, posed in front of the eyes. It is **not** two independent 6DOF hands. Valve’s later Source VR path still uses that one camera-locked VM and only offsets it (see §6.4).

---

## 2. Entity graph and important fields

```
CBasePlayer
  m_hViewModel[MAX_VIEWMODELS]     // CHandle<CBaseViewModel>
  CreateViewModel(index)
  GetViewModel(index)
  CalcViewModelView(eyeOrigin, eyeAngles)   // loops all VMs

CBaseViewModel  : CBaseAnimating
  m_nViewModelIndex                // 0 or 1
  m_hOwner                         // player
  m_hWeapon                        // current CBaseCombatWeapon driving this VM
  m_sVMName                        // server string_t of current VM model
  m_sAnimationPrefix               // unused on the VM itself in the code read
  m_nAnimationParity               // 3 bits; restart cue
  m_nBody / m_nSkin / m_nSequence  // networked (body is 8 bits)
  m_flPoseParameter[]
  m_vecLastFacing                  // lag state
  SetWeaponModel(model, weapon)
  CalcViewModelView / CalcViewModelLag / AddViewModelBob
  SendViewModelMatchingSequence(seq)

CBaseCombatWeapon : CBaseAnimating
  FileWeaponInfo_t via m_hWeaponFileInfo
  m_nViewModelIndex                // which player VM slot this weapon uses
  m_iViewModelIndex                // precached model index of v_ mdl
  m_iWorldModelIndex               // precached model index of w_ mdl
  GetViewModel() / GetWorldModel() / GetAnimPrefix()
  SetViewModel() / SendViewModelAnim() / SendWeaponAnim()
```

**Confirmed from source:** `CBaseViewModel` is declared in `game/shared/baseviewmodel_shared.h`. The same class name is `#define`d to `C_BaseViewModel` on the client. Implementation is split:

- Shared logic: `game/shared/baseviewmodel_shared.cpp`
- Server transmit: `game/server/baseviewmodel.cpp`
- Client draw / attachments / events: `game/client/c_baseviewmodel.cpp` (header is a thin include of the shared header)

Weapon parse: `game/shared/weapon_parse.h`, `weapon_parse.cpp`.

There is no `viewmodel_t` struct. Player storage is `CNetworkArray(CBaseViewModelHandle, m_hViewModel, MAX_VIEWMODELS)` (`game/server/player.h`) / `CHandle<C_BaseViewModel> m_hViewModel[MAX_VIEWMODELS]` (`game/client/c_baseplayer.h`).

---

## 3. How weapons and viewmodels are separated

### 3.1 Scripts assign two models

**Confirmed from source** (`FileWeaponInfo_t::Parse` in `weapon_parse.cpp`):

| Script key | C++ field | Role |
| --- | --- | --- |
| `viewmodel` | `szViewModel[80]` | First-person MDL path |
| `playermodel` | `szWorldModel[80]` | World / 3rd-person MDL path |
| `anim_prefix` | `szAnimationPrefix[16]` | 3rd-person player anim prefix, **not** the VM sequence set |

`CBaseCombatWeapon::GetViewModel()` returns `GetWpnData().szViewModel`. The `int viewmodelindex` argument is **ignored in the base class**. TF overrides this to return a **hands** MDL when the item `ShouldAttachToHands()` (`tf_weaponbase.cpp`).

**Confirmed from Valve wiki** ([Weapon script](https://developer.valvesoftware.com/wiki/Weapon_script), archive 2026-06-12):

- `viewmodel` is relative to the game root and **must** include `models\`. Missing viewmodel + selecting the weapon **crashes**.
- `playermodel` is the world model.
- `anim_prefix` is the prefix of animations played by **characters wielding** the weapon (`prefix_idle`, `prefix_reload`). Wiki notes it **does not appear to have any effect when changed in Half-Life 2**.
- Root table name in the `.txt` is `WeaponData { }`. Files live at `game/scripts/<classname>.txt`.

The requested wiki page [WeaponData](https://developer.valvesoftware.com/wiki/WeaponData) has **no Wayback snapshot** (archive.org `available` returned empty). **Unknown** whether it is a distinct article or a redirect onto Weapon script. The **KeyValues root** used by Valve examples is `WeaponData` (**Confirmed from Valve wiki**, [Adding a new weapon to your mod](https://developer.valvesoftware.com/wiki/Adding_a_new_weapon_to_your_mod)).

### 3.2 Player owns the viewmodel; weapon only points at it

**Confirmed from source** (`CBasePlayer::CreateViewModel`, `game/server/player.cpp`):

```
vm = CreateEntityByName("viewmodel")
vm->SetAbsOrigin(GetAbsOrigin())
vm->SetOwner(this)
vm->SetIndex(index)
DispatchSpawn(vm)
vm->FollowEntity(this)          // parent + EF_BONEMERGE on SERVER
m_hViewModel.Set(index, vm)
```

Called from player spawn (`player.cpp` around the `CreateViewModel()` in `Spawn()`).

`FollowEntity` (`baseentity_shared.cpp`): `SetParent`, `MOVETYPE_NONE`, optional `EF_BONEMERGE`, `FSOLID_NOT_SOLID`, local origin/angles zero.

**Critical client/server split (Confirmed from source):** `DT_BaseViewModel` is `BEGIN_NETWORK_TABLE_NOBASE`. It networks model index, body, skin, sequence, viewmodel index, playback rate, effects, animation parity, `m_hWeapon`, `m_hOwner`, pose parameters, muzzle-flash parity — **not** `CBaseEntity` move-parent. So the server VM is parented to the player; the **client VM is a free entity** whose origin is written in world space by `CalcViewModelView`.

**Strong inference:** server `FollowEntity` is for hierarchy / PVS / lifetime, not for first-person placement. Do not assume client VMs are bone-merged onto the player.

### 3.3 Weapon model vs viewmodel model on the weapon entity

The **weapon entity itself** still has an MDL, because `SelectWeightedSequence(ACT_VM_*)` runs on `CBaseCombatWeapon` (`CBaseAnimating`).

**Confirmed from source:**

- `Spawn`: `SetModel(GetWorldModel())` if present.
- `GiveTo` / pickup when owner is a player: `SetModel(GetViewModel())`; NPCs keep the world model (`basecombatweapon_shared.cpp`).
- HL2MP/Portal `SetActivity`: temporarily `SetModel(GetWorldModel())`, `SelectWeightedSequence`, then `SetModel(GetViewModel())` — a documented “Oh man…” hack so sequence lookup uses the right MDL.

So: dropped/NPC weapon → `w_*.mdl`. Player-owned weapon → often `v_*.mdl` on the **weapon entity** so `ACT_VM_*` sequence indices exist, while the **visible first-person mesh** is still the separate `viewmodel` entity.

### 3.4 Deploy copies the script path onto the player VM

**Confirmed from source** (`CBaseCombatWeapon::SetViewModel`):

```
vm = pOwner->GetViewModel(m_nViewModelIndex, false)
vm->SetWeaponModel(GetViewModel(m_nViewModelIndex), this)
```

`SetWeaponModel` (`baseviewmodel_shared.cpp`):

- Stores `m_hWeapon = weapon`.
- Client: `SetModel(modelname)`.
- Server: if `m_sVMName` changed, `SetModel`, spawn VGUI control panels from attachments `controlpanel%d_ll` / `_ur`.

`DefaultDeploy` (`basecombatweapon_shared.cpp`):

1. `pOwner->SetAnimationExtension(szAnimExt)` — 3rd-person prefix from `anim_prefix`
2. `SetViewModel()`
3. `SendWeaponAnim(iActivity)` typically `ACT_VM_DRAW`
4. `SetWeaponVisible(true)` — clears `EF_NODRAW` on weapon **and** its VM

`SetViewModelIndex` chooses slot 0 or 1. TF grenades / cloak use index 1.

### 3.5 Who draws

**Confirmed from source** (`C_BaseCombatWeapon::ShouldDraw`):

- No owner → draw world model (pickup).
- Carried by **local** player, first person → **return false** (“the viewmodel will do that”).
- Local player third person (`ShouldDrawLocalPlayerViewModel` false) → draw world model.
- Other players → only the active weapon’s world model.

`C_BaseViewModel::ShouldDraw`: HLTV/replay in-eye of the owner; otherwise `C_BaseAnimating::ShouldDraw`. Transmit (`baseviewmodel.cpp`): owner always; in-eye observers; nobody else (`FL_EDICT_DONTSEND`).

`SetWeaponVisible(false)` sets `EF_NODRAW` on both weapon and VM.

---

## 4. How viewmodels are animated

### 4.1 Activity → sequence → VM

**Confirmed from source:**

```
SendWeaponAnim(iActivity)
  → SetIdealActivity(ideal)
       sequence = SelectWeightedSequence(ideal)   // on the WEAPON’s current MDL
       maybe FindTransitionSequence → ACT_TRANSITION
       else SetActivity + SetSequence on weapon
       SendViewModelAnim(sequence)
            vm->SendViewModelMatchingSequence(sequence)
```

`SendViewModelMatchingSequence`:

- `SetSequence(sequence)` on the VM
- bump `m_nAnimationParity` (3 bits)
- `SetCycle(0)`, `ResetSequenceInfo()`
- client also forces `m_flAnimTime = curtime`

The **same integer sequence index** is assumed to mean the same clip on weapon MDL and VM MDL. That only works if both MDLs share the activity/sequence layout, **or** the weapon entity is already using the `v_` MDL (the usual HL2 pickup path).

**Confirmed from Valve wiki** ([Viewmodel](https://developer.valvesoftware.com/wiki/Viewmodel)): `SendViewModelMatchingSequence` “just changes the animation. The fancy name is probably a holdover.”

Client restart: `RecvProxy_SequenceNum` resets cycle if sequence changed. `UpdateAnimationParity` resets cycle when parity changes and the VM is not predictable.

`C_BaseViewModel::Interpolate` **overrides cycle** from elapsed time × `GetSequenceCycleRate` × playback rate (clamped; looping uses `fmod`). Predicted VMs use `GetFinalPredictedTime()`.

### 4.2 Sequence metadata in the MDL

**Confirmed from source** (`studio.h`):

- `mstudioseqdesc_t`: label, activity name, `activity` (resolved at load), `actweight`, `numevents` / `pEvent()`, bbox, blends, IK rules, autolayers, bone weights, pose keys, fade in/out, `KeyValueText()`.
- `mstudioanimdesc_t`: fps, flags, frames, anim blocks, IK rules.
- `$includemodel` appends sequences at runtime (`studiohdr_t::GetNumSeq` walks includes). **Confirmed from Valve wiki** ([`$includemodel`](https://developer.valvesoftware.com/wiki/$includemodel)): copies `$animation`, `$sequence`, `$attachment`, `$collisiontext`; ignores meshes/textures. Included MDLs may have different bone **order** but need the same bone **hierarchy** and `$ikchain` declarations. Example: L4D2 Boomer hands mesh + `anims_v_claw_Boomer.mdl`.

### 4.3 Animation events

**Confirmed from source** (`C_BaseViewModel::FireEvent`):

- `AE_CL_PLAYSOUND` / `CL_EVENT_SOUND` → `EmitSound` on the **owner** (local filter), not on the VM origin as a world entity.
- Else `pWeapon->OnFireEvent(this, origin, angles, event, options)`; if false, `C_BaseAnimating::FireEvent`.

`FireEvent` uses the **global** `GetActiveWeapon()` (local player’s active weapon), not `m_hWeapon`, because `C_BaseViewModel` has no `GetActiveWeapon` member. **Strong inference:** dual-VM / off-hand events can fire on the wrong weapon if slot 1 is animating while slot 0 is “active”. `GetOwningWeapon()` / `m_hWeapon` is the safer handle (used by `DrawModel` / attachment override).

`mstudioevent_t`: `cycle`, `event`, `type`, `options[64]`.

**Confirmed from Valve wiki** ([`$bodygroup`](https://developer.valvesoftware.com/wiki/$bodygroup)): sequences can fire `AE_CL_BODYGROUP_SET_VALUE "group index"`, `AE_CL_ENABLE_BODYGROUP`, `AE_CL_DISABLE_BODYGROUP`.

NPC weapons use `Operator_HandleAnimEvent` (e.g. `EVENT_WEAPON_SMG1` + optional attachment name in `options`) — **Confirmed from Valve wiki** code sample on [Adding a new weapon](https://developer.valvesoftware.com/wiki/Adding_a_new_weapon_to_your_mod). That is 3rd-person / NPC, not the VM path.

### 4.4 Bone controllers

**Confirmed from source:** `C_BaseViewModel::GetBoneControllers` calls `C_BaseAnimating::GetBoneControllers` then `pWeapon->GetViewmodelBoneControllers(this, controllers)`. Base weapon stub is empty. TF minigun writes the barrel bone in `CTFViewModel::StandardBlendingRules`.

`studiohdr_t` has `numbonecontrollers` / `pBonecontroller()`.

### 4.5 Player `anim_prefix` vs VM activities

**Confirmed from source:** `GetAnimPrefix()` → script `anim_prefix` → `SetAnimationExtension` on the player at deploy. This feeds **player** 3rd-person sequences, not `ACT_VM_*`.

**Confirmed from Valve wiki** (Weapon script): changing `anim_prefix` in HL2 appears to do nothing. HL2MP weapons instead use `acttable_t` maps (`ACT_MP_STAND_IDLE` → `ACT_HL2MP_IDLE_AR2`, etc.).

VM activities (wiki list, non-exhaustive): `ACT_VM_DRAW`, `HOLSTER`, `IDLE`, `FIDGET`, `PRIMARYATTACK`, `SECONDARYATTACK`, `RELOAD`, `DRYFIRE`, melee hit/miss set, `IDLE_TO_LOWERED` / `IDLE_LOWERED` / `LOWERED_TO_IDLE`, `RECOIL1..3`, silencer attach/detach.

---

## 5. Where first-person transforms are applied

Call chain **Confirmed from source:**

```
CViewRender::SetUpViews  (game/client/view.cpp)
  pPlayer->CalcView(...)                 // eye origin/angles/FOV
  save ViewModelOrigin/Angles = view eye
  ... VR OverrideStereoView ...
  pPlayer->CalcViewModelView(ViewModelOrigin, ViewModelAngles)

CBasePlayer::CalcViewModelView           // shared, loops MAX_VIEWMODELS
  vm->CalcViewModelView(this, eyeOrigin, eyeAngles)
```

`CalcViewModelView` is compiled **client-only** (`#if defined(CLIENT_DLL)`). Comment in source: “UNDONE: Calc this on the server?”

### 5.1 `CBaseViewModel::CalcViewModelView` pipeline

**Confirmed from source** (`baseviewmodel_shared.cpp`):

Let \(\mathbf{p}_0\) = `eyePosition`, \(\mathbf{R}_0\) = `eyeAngles`.

1. Copy \(\mathbf{p} \leftarrow \mathbf{p}_0\), \(\mathbf{R} \leftarrow \mathbf{R}_0\), \(\mathbf{R}_{\mathrm{orig}} \leftarrow \mathbf{R}_0\).
2. If `m_hWeapon` and **not** `prediction->InPrediction()`:
   - `pWeapon->AddViewmodelBob(this, p, R)` — walk bob (HL2 scales ~10–80% of computed bob onto origin/angles).
   - `CalcViewModelLag(p, R, R_orig)` — look-lag / sway.
3. `AddViewModelBob(owner, p, R)` — empty in base; TF off-hand (index 1) adds head bob.
4. If not predicting: `vieweffects->ApplyShake(p, R, 0.1)` (10% of player shake).
5. If `UseVR()`: `g_ClientVirtualReality.OverrideViewModelTransform(p, R, weapon && weapon->ShouldUseLargeViewModelVROverride())`.
6. `SetLocalOrigin(p); SetLocalAngles(R)`.
7. Sixense (if enabled, not observing, not VR): extra gun-angle offset from view-angle offset × `viewmodelFOV/playerFOV`, pitch clamped.

Because the client VM has no move parent, `SetLocalOrigin(eyePosition)` **is** the render origin. **Confirmed from source** (NOBASE table + this setter).

TF wraps this (`CTFViewModel::CalcViewModelView`): weapon-lower pitch, inspect offset, min-viewmodel offset along view axes, then `BaseClass::CalcViewModelView`.

### 5.2 `CalcViewModelLag` equations (base)

**Confirmed from source.** Globals: `g_fMaxViewModelLag = 1.5`, `sv_viewmodel_lag_do_angles` (default 1).

Let \(\mathbf{f} = \mathrm{AngleVectors}(\mathbf{R})\).

Each frame (`frametime ≠ 0`):

\[
\Delta = \mathbf{f} - \mathbf{f}_{\mathrm{last}}
\]
\[
s = 5 \cdot \begin{cases} |\Delta| / 1.5 & \text{if } |\Delta| > 1.5 \\ 1 & \text{else} \end{cases}
\]
\[
\mathbf{f}_{\mathrm{last}} \leftarrow \mathrm{normalize}\bigl(\mathbf{f}_{\mathrm{last}} + s \cdot dt \cdot \Delta\bigr)
\]
\[
\mathbf{p} \leftarrow \mathbf{p} + 5 \cdot (-\Delta)
\]

If `sv_viewmodel_lag_do_angles`: wrap original pitch to \([-180,180]\), then

\[
\mathbf{p} \leftarrow \mathbf{p} - 0.035\,p\,\mathbf{f} - 0.03\,p\,\mathbf{r} - 0.02\,p\,\mathbf{u}
\]

with \(\mathbf{f},\mathbf{r},\mathbf{u}\) from **original** eye angles. If max lag is 0, origin/angles restore to pre-lag.

`CPredictedViewModel::CalcViewModelLag` (HL2MP predicted VMs): by default on HL2 it calls the **legacy** path (`sv_wpn_sway_pred_legacy` 0 on HL2, 1 elsewhere). Legacy: keep 0.1s of angle history (`cl_wpn_sway_interp`), convert lagged forward difference into a local offset scaled by `cl_wpn_sway_scale`.

### 5.3 HL2 walk bob (weapon)

**Confirmed from source** (`CBaseHLCombatWeapon::AddViewmodelBob`, client only):

\[
\mathbf{p} \mathrel{+}= 0.1\,b_v\,\mathbf{f},\quad p_z \mathrel{+}= 0.1\,b_v
\]
\[
R_{\mathrm{roll}} \mathrel{+}= 0.5\,b_v,\quad R_{\mathrm{pitch}} \mathrel{-}= 0.4\,b_v,\quad R_{\mathrm{yaw}} \mathrel{-}= 0.3\,b_\ell
\]
\[
\mathbf{p} \mathrel{+}= 0.8\,b_\ell\,\mathbf{r}
\]

`$origin` in the QC is a **compile-time** mesh shift in the MDL; it is not applied here. Runtime pose is “eyes + bob + lag + shake (+ VR offset)”.

### 5.4 Attachment FOV remap (`FormatViewModelAttachment`)

Viewmodels render with `fovViewmodel` (often 54°) while the world uses `fov`. Muzzle attachments would otherwise disagree with the world camera.

**Confirmed from source** (`c_baseviewmodel.cpp`):

\[
f = \frac{\tan(\mathrm{fov}/2)}{\tan(\mathrm{fovViewmodel}/2)}
\]

View-space: \(\mathbf{t} = ( \mathbf{r}\cdot(\mathbf{o}-\mathbf{c}),\; \mathbf{u}\cdot(\mathbf{o}-\mathbf{c}),\; \mathbf{f}\cdot(\mathbf{o}-\mathbf{c}) )\).

Forward map (used when formatting attachments): \(t_x \leftarrow f t_x\), \(t_y \leftarrow f t_y\). Inverse (`UncorrectViewModelAttachment`) divides.

Then \(\mathbf{o}' = \mathbf{c} + t_x\mathbf{r} + t_y\mathbf{u} + t_z\mathbf{f}\).

`C_BaseViewModel::FormatViewModelAttachment` applies this to the attachment matrix translation only.

**Strong inference for HMD VR:** this remap exists because 2D viewmodel FOV ≠ world FOV. Stereo IPD + matching eye FOV usually wants this **off**, or attachments computed in the same space as the submitted eye view.

### 5.5 Left-hand flip

**Confirmed from source** (`ApplyBoneMatrixTransform`): if `ShouldFlipViewModel()`, transform bone matrix into view space, negate the Y row (mirror in view X), transform back. `InternalDrawModel` switches cull to `CW`. CS: `cl_righthand` vs `m_bBuiltRightHanded` / `m_bAllowFlipping` from the script.

### 5.6 Hitscan origin vs VM muzzle

**Confirmed from source:** `CBasePlayer::Weapon_ShootPosition()` returns `EyePosition()` — **not** the muzzle attachment. Client `C_BaseCombatWeapon::GetShootPosition` **does** prefer viewmodel attachment `"muzzle"` for the local first-person active weapon, else worldmodel `"muzzle"`, else render origin.

Authoring wiki: projectiles spawn at `GetOwner()->Weapon_ShootPosition()` + `EyeAngles()`. Tracers: attachment 1 “should be the muzzle” (`UTIL_ParticleTracer(..., entindex(), 1)`).

**Strong inference:** bullets come from the **eye**; muzzle flash/tracer come from **attachment**. That mismatch is tolerable on a flat screen and very visible in VR.

---

## 6. Stock Source VR (SDK 2013) — not independent hands

**Confirmed from source** (`client_virtualreality.cpp`):

```
vr_viewmodel_offset_forward       = -8
vr_viewmodel_offset_forward_large = -15   // e.g. physcannon override
```

`OverrideViewModelTransform`:

\[
\mathbf{p} \leftarrow \mathbf{p} + f_{\mathrm{fwd}}\,\mathbf{f}(\mathbf{R})
\]
\[
\mathbf{R} \leftarrow \mathrm{MatrixAngles}(\texttt{m\_WorldFromWeapon})
\]

So Valve Source VR still draws **one** screen-space viewmodel, pushed along view forward (often **toward** the eye because the default is negative), and aligns its angles to a weapon/aim matrix. It does **not** bind the VM to a motion-controller pose.

`UseVR()` is the Source engine VR interface (`public/sourcevr/isourcevirtualreality.h`), not OpenVR in the game DLL.

**This is a baseline, not a BMVR architecture recommendation.** Independent hands need to hijack `CalcViewModelView` (or stop using VMs and drive a different entity).

---

## 7. Attachments

### 7.1 QC / MDL

**Confirmed from Valve wiki** ([`$attachment`](https://developer.valvesoftware.com/wiki/$attachment), [Attachments](https://developer.valvesoftware.com/wiki/Attachments)):

```
$attachment <name> <parent bone> <x y z> [absolute] [rigid] [rotate pitch yaw roll] [world_align]
```

- Offset is in the **bone** coordinate system (or origin if `absolute`; offset still described relative to the given bone).
- `rigid`: bone will not move; studiomdl may delete it and reparent the attachment.
- Worldmodel weapons that should snap to a hand: dummy bone named **exactly** like the character hand (`ValveBiped.Bip01_R_Hand` for HL2DM) plus `EF_BONEMERGE` on the weapon entity. Put `$bonemerge` on that bone in the **character** QC to avoid perf warnings.

**Confirmed from source** (`mstudioattachment_t`): name, flags (`ATTACHMENT_FLAG_WORLD_ALIGN = 0x10000`), `localbone`, `local` as `matrix3x4_t`.

Bones used by attachments get `BONE_USED_BY_ATTACHMENT` (`studio.h`).

### 7.2 Runtime lookup

**Confirmed from source:** `CBaseViewModel::LookupAttachment` / `GetAttachment*` on the client: if `m_hWeapon->WantsToOverrideViewmodelAttachments()`, **delegate to the weapon** (TF: some econ attachments live on the weapon mesh that is bonemerged / attached, e.g. Natascha, Backburner, Kritzkrieg). Base `WantsToOverrideViewmodelAttachments()` is false.

HL2 VM control panels: attachments `controlpanel0_ll` / `controlpanel0_ur` (**Confirmed from source** + wiki VGUI-on-entity example using `$attachment "controlpanel0_ur" "muzzle" ... rotate -90 0 0`).

Client first-person muzzle: `"muzzle"` on VM index 0 (`GetShootPosition`).

`ent_attachments viewmodel` is the wiki-recommended debug command.

### 7.3 VR lever

Attachments are the engine’s supported “socket” system: muzzle, shell eject, laser, `weapon_bone`, `anim_attachment_RH`. For “weapon glued to a controller”, two patterns already exist in Valve code/docs:

1. **Bone merge** a weapon MDL whose skeleton includes a hand bone onto a posed hand/VM (TF, worldmodel hold).
2. **Parent to attachment** (`SetParentAttachment` / `SetParentAttachmentMaintainOffset`, or `FollowEntity` + merge).

---

## 8. Bodygroups — hide arms, keep weapon (same skeleton)

**Confirmed from Valve wiki** ([`$bodygroup`](https://developer.valvesoftware.com/wiki/$bodygroup)):

- Named group of mutually exclusive meshes (`studio "mesh"` or `blank`).
- Runtime: `FindBodygroupByName` + `SetBodygroup`.
- Hammer KV `body` / input `SetBodyGroup` for non-static props.
- **Cannot change skeletons or collision models.**
- Packed into `m_nBody`. Wiki: `CBaseAnimating` supports 32-bit combinations; **`CBaseViewModel` is networked with 8 bits** (256 combinations). Matches **Confirmed from source** `SendPropInt(SENDINFO(m_nBody), 8)`.
- Sequence events can swap groups mid-anim.

**Confirmed from source:** TF viewmodel `DrawModel` can zero `m_nBody` and `RecalcBodygroupsIfDirty` from the player (class-specific VM bodygroups).

**VR lever:** if Black Mesa `v_*.mdl` files put arms and gun in **bodygroups** (or `$body` + `$bodygroup`), hide the arm group and keep sequences/attachments on the gun. If arms and gun are a **single mesh** with shared weights, bodygroups cannot split them — need a different MDL, `$bonemerge` weapon, or a replacement VM.

`$body` = primary reference mesh. `$model` = richer body (flex / eyeballs / extra options). **Confirmed from Valve wiki** ([Category:QC commands](https://developer.valvesoftware.com/wiki/Category:QC_commands), [QC](https://developer.valvesoftware.com/wiki/QC)).

---

## 9. Bonemerge — arms + weapon sharing a skeleton

### 9.1 QC flag

**Confirmed from Valve wiki** ([`$bonemerge`](https://developer.valvesoftware.com/wiki/$bonemerge)):

```
$bonemerge <bone name>
```

Sets `BONE_USED_BY_BONE_MERGE` on that bone. Merge still works without it, but you get performance warnings. For **animation-only MDLs** (no meshes), the flag is **mandatory** or the bone will not merge and will distort — unless an external model `$includemodel`s the anims.

Also used to **stop studiomdl collapsing** unused / single-weight bones. Some games ignore `$bonemerge` and collapse anyway.

**Confirmed from source:** `BONE_USED_BY_BONE_MERGE = 0x00040000` (`studio.h`).

### 9.2 Runtime merge (name match)

**Confirmed from source** (`CBoneMergeCache`, `game/client/bone_merge_cache.cpp` + `c_baseanimating.cpp`):

If child has `EF_BONEMERGE` and a follow parent:

1. For each child bone, `Studio_BoneIndexByName(parentHdr, childBoneName)`.
2. If found, record `(childIndex, parentIndex)` and copy the parent’s **bone-to-world** matrix onto the child each `SetupBones`.
3. If the parent bone lacked `BONE_USED_BY_BONE_MERGE`, follow mask becomes `BONE_USED_BY_ANYTHING` (the perf warning path).
4. Unmerged child bones still animate locally and concatenate from parent bone or entity origin.

Server analogue: `CBaseAnimating::BuildMatricesWithBoneMerge` — same **name** lookup.

`FollowEntity(p, bBoneMerge=true)` is how you turn this on.

**Confirmed from Valve wiki** (Attachments): “Entities linked with bone merge (`EF_BONEMERGE`) can be automatically snapped to the correct bone. Weapons use this to appear in the correct hand.”

### 9.3 TF: hands VM + bonemerged weapon (closest Valve 1st-person split)

**Confirmed from source** (`CTFWeaponBase::GetViewModel`): if item `ShouldAttachToHands()`, the **viewmodel entity’s model is the class hand MDL**, not the gun. Comment on `s_viewmodelacttable[]`:

> Needed this for weapons that bonemerge themselves to the hand models to create their viewmodel. The hand model needs to have all the animations, and be able to choose the right anims to play for the active weapon.

Weapon then `AddEffects(EF_BONEMERGE | EF_BONEMERGE_FASTCULL)` (around `tf_weaponbase.cpp` 6967). Hands play `ACT_PRIMARY_VM_*` / `ACT_SECONDARY_VM_*` / `ACT_MELEE_VM_*`; gun bones named like the hand skeleton hitch a ride.

**This is the most relevant Valve pattern for “independent hands + attach weapon”** — except TF still drives the **hands VM** from `CalcViewModelView` (camera), not from a controller. Replacing that pose with a controller pose, while leaving bonemerge, is the natural VR adaptation.

`EF_BONEMERGE_FASTCULL`: put this ent’s origin at parent and use parent bbox instead of setting up parent bones every frame for culling (`const.h`).

---

## 10. Rendering details (first person)

**Confirmed from source:**

- `GetRenderGroup()` → `RENDER_GROUP_VIEW_MODEL_OPAQUE`.
- `ShadowCastType` → `SHADOWS_NONE`; `ShouldReceiveProjectedTextures` → false.
- `view.cpp`: `zNearViewmodel = 1`.
- Draw path: player or weapon may `IsOverridingViewmodel()` and replace `DrawModel`. Then `pWeapon->ViewModelDrawn(this)`.
- Always-interpolate: `ENTCLIENTFLAG_ALWAYS_INTERPOLATE` in the VM ctor.

Wiki: elongation / 54° FOV is an artist convention. In an HMD that convention fights stereo.

---

## 11. Valve authoring: scripts, QC, Studiomdl, MDL

### 11.1 Weapon entity authoring

**Confirmed from Valve wiki** ([Authoring a weapon entity](https://developer.valvesoftware.com/wiki/Authoring_a_weapon_entity)):

- Most gun properties are a script; C++ can be minimal (`GetFireRate`, `GetBulletSpread`).
- Per-frame owner callbacks: `ItemPreFrame`, `ItemHolsterFrame`, `ItemBusyFrame`, `ItemPostFrame` → `PrimaryAttack` / `SecondaryAttack` / `HandleFireOnEmpty` / `Reload` / `WeaponIdle`.
- Hitscan: `GetOwner()->FireBullets(...)`. Projectiles: `CBaseEntity::Create(..., GetOwner()->Weapon_ShootPosition(), GetOwner()->EyeAngles(), GetOwnerEntity())` — owner is the **player**, not the weapon. Do not predict projectile spawn; **do** predict VM anim, ammo, reload.
- `PRECACHE_WEAPON_REGISTER(classname)` precaches script resources.

**Confirmed from Valve wiki** ([Weapon script](https://developer.valvesoftware.com/wiki/Weapon_script)): standard keys parsed by `CBaseCombatWeapon` / `FileWeaponInfo_t` (see §3.1 plus UI, ammo, `SoundData`, `TextureData`, `item_flags`). Custom keys: subclass `FileWeaponInfo_t::Parse` and `CreateWeaponInfo()`. ICE encryption via `.ctx` + `GetEncryptionKey()`.

`scripts/weapon_manifest.txt` lists files to precache (`PrecacheFileWeaponInfoDatabase`).

### 11.2 QC / Studiomdl

**Confirmed from Valve wiki** ([QC](https://developer.valvesoftware.com/wiki/QC), [Compiling a model](https://developer.valvesoftware.com/wiki/Compiling_a_model), [Category:QC commands](https://developer.valvesoftware.com/wiki/Category:QC_commands)):

- QC is the compile script; SMD/DMX supply mesh and anims; Studiomdl writes `mdl` (+ companions).
- `$include` pulls another QC/QCI **textually** at compile time (convention: `.qci` for includes).
- Every model needs **at least one** `$sequence`, even if it reuses the reference mesh as idle.
- `$modelname`, `$body` / `$model`, `$bodygroup`, `$sequence`, `$animation`, `$attachment`, `$origin`, `$scale`, `$cdmaterials`, `$collisionmodel`, `$hbox`, `$definebone`, `$bonemerge`, `$ikchain`, `$includemodel`.

**`$origin`:** wiki Viewmodel page: use it to place the VM in front of the camera; without it the gun draws at screen centre. **`$origin` fetch timed out** as a standalone page; treat placement rule as **Confirmed from Valve wiki** (Viewmodel), exact `$origin` syntax as **Unknown** until that page is read. Related: `$scale` does **not** scale `$attachment` numbers (**Confirmed from Valve wiki**, [`$scale`](https://developer.valvesoftware.com/wiki/$scale)).

**`$scale`:** multiplies subsequent mesh/anim files; put it **before** file references; per-sequence `scale` multiplies with it; negative values mirror (with caveats); does not affect VTA flexes or `$proceduralbones`; `$definebone` verts scale from the defined local position.

**`$definebone`:** declare a bone with no SMD geometry — required for anim-only MDLs that would otherwise strip bones. Studiomdl `-definebones` dumps lines. Order of `$definebone` can change bone order for merge. Must appear before `$hierarchy`. Missing bones break VTA flexes.

**`$hbox`:** `{group} {bone} {mins} {maxs}`. Group 0 generic … 1 head … 7 right leg. Any explicit `$hbox` disables autohitboxes. Mostly world/player, not first-person.

**`$ikchain`:** 3-bone IK (foot/hand). Required to match on `$includemodel` partners. Ken Birdwell (quoted on the wiki): IK is client-side, originally HL2 NPCs, not done for multiplayer players. **Low relevance** to gluing a gun to a VR controller unless you IK a wrist to a controller target (`ikrule ... attachment`).

**`$model` vs `$body`:** both add a mesh; `$model` is the “rich” form. Bodygroups cannot introduce a new skeleton.

**Studiomdl page** Wayback fetch timed out. Compiler location from wiki compile article: `sourcesdk/bin/<branch>/bin/studiomdl`. SDK 2013 tree used here did **not** include `src/utils/studiomdl` in the sparse checkout (utils not present). **Unknown** whether current `source-sdk-2013` master still vendors studiomdl.

### 11.3 MDL on disk / in memory

**Confirmed from source** (`studiohdr_t` in `studio.h`): `id`, `version`, `checksum` (must match `.phy` / `.vtx`), `name[64]`, `numbones` / `pBone()`, bone controllers, hitbox sets, local anims/seqs, textures, bodyparts, **local attachments**, includemodels, pose parameters, flex, IK, etc.

**Confirmed from Valve wiki** ([Models (C++)](https://developer.valvesoftware.com/wiki/Models_(C++)), Talk:MDL):

| Representation | Role |
| --- | --- |
| Model index | `PrecacheModel` / `GetModelIndex` |
| `model_t*` | Engine handle |
| `CStudioHdr` | Mapped MDL + helpers (SDK class) |
| `ModelInstanceHandle_t` | Client render instance |
| `.mdl` | Header, bones, sequences, attachments |
| `.vvd` | Vertex data |
| `.vtx` | Hardware-optimized strips |
| `.phy` | Collision |
| `.ani` / `*_animations.mdl` | External animation blocks via `$includemodel` |

Live [MDL (Source)](https://developer.valvesoftware.com/wiki/MDL_(Source)) fetch timed out; Talk:MDL + Models (C++) used instead. Full binary layout of all companion files is **Unknown** here beyond `studio.h`.

### 11.4 Category:Modeling

**Confirmed from Valve wiki** ([Category:Modeling](https://developer.valvesoftware.com/wiki/Category:Modeling), archive 2026-04-20): hub for DCC tools (Blender, Maya, XSI, 3ds Max, …), QC command category, blend sequences, export articles. Not weapon-code specific. Useful siblings: [Viewmodels in XSI](https://developer.valvesoftware.com/wiki/Viewmodels_in_XSI), [Viewmodels in Blender](https://developer.valvesoftware.com/wiki/Viewmodels_in_Blender), [Creating worldmodels from viewmodels](https://developer.valvesoftware.com/wiki/Creating_worldmodels_from_viewmodels) (titles listed on the Viewmodel page; bodies not fetched).

---

## 12. VR-relevant hooks (priority)

Do not assume Black Mesa matches any of this until BM MDLs / `C_BaseViewModel` are checked.

| Lever | Why it matters for independent hands + gun-on-controller | Evidence |
| --- | --- | --- |
| **`CalcViewModelView`** | **The** per-frame pose writer. Replace \(\mathbf{p},\mathbf{R}\) with controller pose (or two poses for `m_nViewModelIndex` 0/1). Today: eyes + bob + lag + 10% shake + Valve VR offset. | **Confirmed from source** |
| **`MAX_VIEWMODELS == 2`** | Engine already has a second VM slot (TF off-hand / dual). Possible left/right hands without a new entity class. | **Confirmed from source** + wiki |
| **`$bodygroup` / `m_nBody` (8-bit on VM)** | Hide arm meshes, keep gun + existing `ACT_VM_*` and muzzle attachments. Fails if arms+gun are one mesh. | **Confirmed from wiki** + source |
| **`$bonemerge` + `EF_BONEMERGE`** | TF pattern: VM = animated hands, weapon MDL merged by **bone name**. For VR: pose a tiny “hand/weapon_bone” skeleton from the controller, merge `w_` or stripped `v_` onto it. Parent and child **must share bone names**. | **Confirmed from source** + wiki |
| **`$attachment` (`muzzle`, `weapon_bone`, `anim_attachment_RH`)** | Particles, tracers, laser; or parent a separate gun entity to a socket. `FormatViewModelAttachment` FOV squash likely wrong in HMD. | **Confirmed from source** + wiki |
| **`SetWeaponModel` / `GetViewModel` override** | Swap in a hands-only or gun-only MDL without changing weapon scripts globally (TF hands path). | **Confirmed from source** |
| **`$includemodel` / `$definebone`** | Share one animation library across hand/gun MDLs; keep merge bones alive in anim-only files. | **Confirmed from wiki** |
| **`$origin`** | Compile-time camera-relative placement. For controller-driven pose you probably want `$origin 0 0 0` and a known attachment at the grip. | **Confirmed from wiki** (Viewmodel) |
| **Disable bob/lag/shake** | `AddViewmodelBob`, `CalcViewModelLag`, `ApplyShake(0.1)` fight 6DOF. Skip when a controller pose is applied. | **Confirmed from source** |
| **`OverrideViewModelTransform`** | Stock Source VR is **not** 6DOF hands. Do not copy it as the BMVR hand solution. | **Confirmed from source** |
| **`Weapon_ShootPosition` = eyes** | Hitscan from camera. VR often wants muzzle or controller forward. | **Confirmed from source** |

### Practical composition (inference, not BM-verified)

**Strong inference:** smallest adaptation that still uses Source’s own machinery:

1. Keep `CBaseViewModel` + `SendWeaponAnim` so fire/reload sequences still run.
2. In `CalcViewModelView`, set origin/angles from the **right** controller (slot 0) / **left** (slot 1) instead of the eyes; skip lag/bob.
3. Choose a mesh strategy from actual BM `v_` QC/MDL:
   - **Bodygroup** arms off, or
   - **Bonemerge** gun onto a controller-posed skeleton, or
   - **Gun-only VM MDL** with `$origin` at the grip attachment.
4. Bypass `FormatViewModelAttachment` when eye FOV == VM FOV (HMD).
5. Decide whether bullets stay eye-sourced or move to muzzle/controller.

---

## 13. What this baseline does **not** establish

- Black Mesa weapon scripts, `v_`/`w_` paths, bodygroups, or whether arms share a mesh with the gun. **Unknown.**
- Whether BM viewmodels use HL2-style combined arms+gun, TF-style hands+merge, or something custom. **Unknown.**
- Whether BM already patches `CalcViewModelView` (Source VR, Sixense, custom). **Unknown** (do not inspect BMVR `src/` in this note’s mandate beyond treating it as out of scope).
- Exact `$origin` argument order (page fetch failed). **Unknown.**
- Full [MDL (Source)](https://developer.valvesoftware.com/wiki/MDL_(Source)) and [Studiomdl](https://developer.valvesoftware.com/wiki/Studiomdl) wiki bodies (timeouts). Use `studio.h` as the canonical layout.
- [WeaponData](https://developer.valvesoftware.com/wiki/WeaponData) as a separate article (no archive). Treat [Weapon script](https://developer.valvesoftware.com/wiki/Weapon_script) + `FileWeaponInfo_t::Parse` as canonical.

---

## 14. Source index

### SDK 2013 (commit `22288b9`)

| Path | Why |
| --- | --- |
| `game/shared/baseviewmodel_shared.h/.cpp` | VM class, `SetWeaponModel`, `CalcViewModelView/Lag`, nettable |
| `game/client/c_baseviewmodel.cpp` | Draw, flip, attachment FOV, events, bone controllers |
| `game/server/baseviewmodel.cpp` | Transmit-to-owner only |
| `game/shared/basecombatweapon_shared.h/.cpp` | Deploy, `SetViewModel`, anim send, visibility |
| `game/client/c_basecombatweapon.cpp` | ShouldDraw, muzzle `GetShootPosition` |
| `game/shared/weapon_parse.h/.cpp` | Script keys |
| `game/server/player.cpp` | `CreateViewModel`, `FollowEntity` |
| `game/shared/baseplayer_shared.cpp` | `CalcViewModelView` loop, `Weapon_ShootPosition` |
| `game/client/view.cpp` | When VM pose is computed vs camera |
| `game/client/client_virtualreality.cpp` | Stock VR VM offset |
| `game/client/bone_merge_cache.cpp` | Name-based merge |
| `game/client/c_baseanimating.cpp` | `EF_BONEMERGE` in `SetupBones` |
| `public/studio.h` | MDL structs, bone flags, attachments, sequences, events |
| `public/const.h` | `EF_BONEMERGE`, `EF_BONEMERGE_FASTCULL` |
| `game/shared/shareddefs.h` | `MAX_VIEWMODELS` |
| `game/shared/predicted_viewmodel.cpp` | Predicted lag/sway |
| `game/shared/hl2/basehlcombatweapon_shared.cpp` | HL2 bob |
| `game/shared/tf/tf_viewmodel.cpp` | Dual VM, offsets, attached models |
| `game/shared/tf/tf_weaponbase.cpp` | Hands VM + bonemerge + act remap |

### Valve wiki (authoring, not Workshop)

| Page | Snapshot used |
| --- | --- |
| [Authoring a weapon entity](https://developer.valvesoftware.com/wiki/Authoring_a_weapon_entity) | 2025-03-29 |
| [Weapon script](https://developer.valvesoftware.com/wiki/Weapon_script) | 2026-06-12 (covers what “WeaponData” scripts contain) |
| [WeaponData](https://developer.valvesoftware.com/wiki/WeaponData) | **no archive** |
| [Adding a new weapon to your mod](https://developer.valvesoftware.com/wiki/Adding_a_new_weapon_to_your_mod) | live index / Google cache |
| [Viewmodel](https://developer.valvesoftware.com/wiki/Viewmodel) | 2025-12-25 |
| [Category:Modeling](https://developer.valvesoftware.com/wiki/Category:Modeling) | 2026-04-20 |
| [Category:QC commands](https://developer.valvesoftware.com/wiki/Category:QC_commands) | live index |
| [QC](https://developer.valvesoftware.com/wiki/QC) | 2026-05-18 |
| [MDL (Source)](https://developer.valvesoftware.com/wiki/MDL_(Source)) | fetch timeout; see [Models (C++)](https://developer.valvesoftware.com/wiki/Models_(C++)), Talk:MDL |
| [`$attachment`](https://developer.valvesoftware.com/wiki/$attachment) | 2026-02-04 |
| [Attachments](https://developer.valvesoftware.com/wiki/Attachments) | 2026-04-18 |
| [`$bonemerge`](https://developer.valvesoftware.com/wiki/$bonemerge) | 2026-04-11 |
| [`$bodygroup`](https://developer.valvesoftware.com/wiki/$bodygroup) | 2026-06-25 |
| [`$includemodel`](https://developer.valvesoftware.com/wiki/$includemodel) | 2026-05-17 |
| [`$definebone`](https://developer.valvesoftware.com/wiki/$definebone) | 2026-04-15 |
| [`$scale`](https://developer.valvesoftware.com/wiki/$scale) | 2026-05-20 |
| [`$ikchain`](https://developer.valvesoftware.com/wiki/$ikchain) | 2026-05-17 |
| [`$hbox`](https://developer.valvesoftware.com/wiki/$hbox) | 2026-04-11 |
| [`$sequence`](https://developer.valvesoftware.com/wiki/$sequence) | fetch timeout |
| [`$origin`](https://developer.valvesoftware.com/wiki/$origin), [`$include`](https://developer.valvesoftware.com/wiki/$include), [Studiomdl](https://developer.valvesoftware.com/wiki/Studiomdl) | fetch timeout / partial |

SDK clone is outside the BMVR git worktree (`%TEMP%\source-sdk-2013`) so this research did not dirty the implementation repo except this file.
