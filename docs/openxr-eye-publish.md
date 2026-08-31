# OpenXR eye publish pipeline (2026-08-31)

How a rendered stereo pair gets from the game's D3D9 eye RTs into the helper's
Vulkan swapchain, and the five defects found while chasing "ghost frames when I
move my head, perfect when I hold still".

## Pipeline

Game (`d3d9.dll`, 32-bit, ~200 fps) and helper (`OpenXRHelper64.exe`, 90 Hz)
are separate processes sharing a bridge struct in shared memory.

1. `RenderView` draws both eyes into the engine eye RTs.
2. `PrepareOpenXrEyeSurfacesForRead` StretchRects both eyes into publish slot
   `k`, then `TransferSurface` + `WaitDeviceIdle`.
3. `PublishOpenXrResolvedEyeTextures` writes both eye descriptors plus a frame
   id into the bridge.
4. Helper reads the descriptors, imports/reuses the Vulkan images, blits each
   eye into its swapchain, `xrEndFrame`.

## The symptom and what it actually was

Ghosting only during head rotation, clean when still, "dominantly left eye".
That signature is a **binocular** defect, not a latency one: latency alone
produces uniform lag, whereas a left/right pair drawn from *different game
frames* produces horizontal disparity error that your brain resolves as a
double image, and only while the image is changing (i.e. while the head moves).
In-game stick turning did not reproduce it as strongly, which is consistent -
VOR keeps your gaze locked during head motion, so disparity error is far more
visible than during a stick turn.

Originally the helper blitted straight from the live engine eye RTs. The game
overwrote those 2-3 times per compositor frame, and the helper's left blit and
right blit happen at different moments, so the eyes could legitimately land on
different game frames.

## Defects fixed

### 1. Descriptor snapshot taken before `xrWaitFrame`

`ImportSharedGameTexturesIfNeeded()` ran before `xrWaitFrame`, so the eye
descriptors could be a full display frame (~11 ms) old. Harmless while there
was one texture per eye (a stale descriptor still pointed at the continuously
updated image), but **once publish textures rotate, a stale descriptor pins
both blits to an already-superseded slot**. That is a hard 11-19 ms of extra
staleness and is what made the image drag behind the head. Now re-imported
immediately before the `RenderEye` loop. Safe: `RenderEye` defaults to
`waitForQueueIdle = true`, so the queue is idle and `vkUpdateDescriptorSets`
cannot touch an in-flight descriptor set.

### 2. Non-atomic left/right descriptor publish

The two eyes were published with separate `L4D2VR_PublishOpenXrSharedTexture`
calls, and the helper read them from shared memory with no lock (the game-side
`std::mutex` is process-local and buys nothing cross-process). The helper could
read left from slot `k` and right from slot `k-1` - **each eye a different game
frame**, exactly the double image above. Replaced with
`L4D2VR_PublishOpenXrSharedTexturePair` using the odd/even seqlock convention
already used by `gameRenderPoseGeneration`, plus release/acquire fences, and a
retrying `BridgeWriter::ReadSharedTexturePair` on the helper side.

### 3. `GetTickCount()` used for a millisecond rate cap

The publish gate was `(now - last) < 8` on `GetTickCount()`, whose resolution is
~15.6 ms. The intended ~125/s cap actually produced **~64/s against a 90 Hz
compositor**, starving roughly every third display frame - the helper then had
no new pair, reused its previous swapchain content, and SteamVR reprojected.
Now QPC with a 7 ms interval (~143/s), so every 11.1 ms compositor frame sees a
fresh pair with margin.

### 4. Log write per published frame

The helper's "Shared game eye textures ready" line was gated on
`sharedTextureGeneration` changing. With rotating handles that changes every
publish, so the helper wrote a log line to disk **inside the frame loop at the
publish rate**. Now gated on the eye layout (width/height/format).

### 5. Submitted vertical FOV did not match the rendered frustum

`PublishOpenXrEyeTexture` carried `renderAspect = m_Aspect` latched at texture
creation. Logged 1.0972, while `NormalizeViewSetupForVREye` renders each eye at
the eye RT's own aspect:

```
view.m_flAspectRatio = (float)eyeWidth / (float)eyeHeight;   // hooks.cpp
view.fov             = vr->m_Fov;
```

which for a 3664x3584 eye is 1.0223 (game log: `fov=106.3->98.5
aspect=1.778->1.022`). The helper derives the submitted vertical FOV from
`renderFovXDeg` + `renderAspect`, so it submitted +/-0.8135 rad where the image
was actually rendered at +/-0.8482 rad - a frustum ~7% short vertically, so the
compositor reprojected through the wrong projection. `renderAspect` is now
derived from the dimensions of the image actually being submitted.

### 6. Publish textures were never shareable, so none of the above ran

The first test of all the above changed nothing, because the rotating publish
path had been silently inactive the whole time. Game log:

```
OpenXR publish rate present=111/s published=108/s skipStale=0/s skipRate=3/s slots=3 copy=0
OpenXR publish RT GetVRDesc failed eye=0 slot=0
```

`copy=0` every line, and the helper only ever saw `gen=2` - one descriptor, never
rotating. `EnsureOpenXrPublishTextures` created its textures with a plain
`device->CreateTexture(...)`, but the DXVK fork only requests an exportable
shared handle when the `m_CreatingTextureID` sentinel is set to an ID that
`ShouldExportOpenXrEyeTexture` accepts:

```cpp
// third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_device.cpp
if (pSharedHandle == nullptr && Pool == D3DPOOL_DEFAULT && g_Game && g_Game->m_VR) {
  const VR::TextureID texID = g_Game->m_VR->m_CreatingTextureID;
  if (g_Game->m_VR->ShouldExportOpenXrEyeTexture(texID, submitSampleCount))
    effectiveSharedHandle = &openXrSharedHandle;
}
```

Without the sentinel the texture is unshared, `GetVRDesc` returns no handle,
`EnsureOpenXrPublishTextures` bails, and `PrepareOpenXrEyeSurfacesForRead` falls
back to publishing the live engine eye RT - the exact race the slots exist to
remove. Fixed with a dedicated `Texture_OpenXrPublish` ID.

That ID is deliberately absent from the post-create `if (texID == ...)` chain in
`d3d9_device.cpp`. Reusing `Texture_LeftEye` would have overwritten
`m_D9LeftEyeSurface` with a publish copy and applied the eye MSAA sample count.

The failure log was also unthrottled, so it wrote to disk at the publish rate
(the game log reached 65 MB). Any per-submit failure log must be rate limited.

## Invariants to preserve

- Both eyes must be copied into the **same** publish slot and published as one
  seqlock'd pair. Never publish eyes independently.
- Import/refresh eye descriptors **after** `xrWaitFrame`, not before.
- Publish rate must stay above the display rate; use QPC, never `GetTickCount`.
- Slot count (3) x publish interval (7 ms) = 21 ms before a slot is rewritten,
  which must stay comfortably above the helper's blit time.

## Status

Defects 1-5 are implemented and confirmed live in the game/helper logs (publish
rate ~110/s, `aspect=1.0143`, `U=0.8526`), and on their own they did **not** fix
the ghosting. Defect 6 is implemented and installed but **not yet
runtime-verified**; until a run shows `copy=1` and a rotating `gen=` in the
helper log, the read/write race is still untested.

Check after the next run:

```
Select-String -Path "...\bin\bmvr_log.txt" -Pattern 'OpenXR publish rate|publish RT'
Select-String -Path "...\openxr_helper64_from_game.log" -Pattern 'Shared game eye textures ready'
```

Expect `copy=1` and `OpenXR publish RTs ready 1136x1120 slots=3`.
