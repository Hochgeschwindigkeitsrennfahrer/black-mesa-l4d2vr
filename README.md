# Black Mesa VR

<a href="https://www.youtube.com/watch?v=5oqQ8akfu3s">
  <img src="assets/release-2026-09-06.jpg" alt="Black Mesa VR — HL2VR weapon wheel and ammo counter">
</a>

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/P5P56X4HH)

A VR mod for the Steam version of **Black Mesa**, bringing 6DOF motion controls, true stereo rendering, OpenXR, and VR-first weapon interaction to the game.

The foundation of the VR implementation is based heavily on the approach pioneered by **L4D2VR**, adapted and extended for Black Mesa.

L4D2VR source:
https://github.com/keyou91/l4d2vr

## Current Status

**The project is working start-to-finish and is playable as a full VR mod.**

* True stereo rendering — Done
* 6DOF motion controls — Done
* HL2VR-style weapon selection — Done
* Accurate crowbar swinging (HL2VR) — Done
* OpenXR implementation — Done
* Health and armor on the hand — Done
* Weapon-mounted ammo counter — Done
* Per-hand item holding and use — Done
* In-world transparent pause menu — Done
* Bare hands until HEV (and throughout Blue Shift) — Done
* Crossbow zoom (headset aim while scoped) — Done
* VR main menu with a motion-controller cursor — Done
* HEV glove lighting and metal reflections — Done
* Blue Shift HUD theme (weapon wheel and wrist HUD) — Done
* Xen culling fixes — Done

## Known Issues

The VR experience is fully playable, but several features are still a work in progress:

* **Quest 3:** Meta Link uses the Link / Oculus OpenXR runtime. Steam Link uses `SteamVR/OpenXR in Meta compatibility mode` and must not Y-flip (that invert was this build treating Steam Link as SteamVR+Touch). Virtual Desktop must use Streamer Options → **OpenXR Runtime = VDXR**. SteamVR in that dropdown inverts the world; this build rasterizes a Y-flip only for SteamVR + Touch that is **not** Meta compatibility (not G2 — negative viewport made yellow bands). Horizon Link 2D menu/pause is a both-eyes quad (matching pose+FOV on a stereo projection still doubled on Oculus 1.207).
* **SteamVR OpenXR runtime:** G2 through SteamVR OpenXR must not use a negative-viewport Y-flip. WMR OpenXR on the same headset is fine. If Meta Link / Oculus stays the Windows ActiveRuntime while SteamVR is also running, you get a waiting room plus a second compositor. One runtime, then SteamVR, then the game.
* **Quest weapons:** Touch uses the OpenXR aim pose. Hands sit on the grip and point along the aim ray. If you already saved `ox=5.5` extras in `VR/viewmodel_offsets.txt`, reset those weapons (numpad 0) so they do not double with the built-in Touch pull-back.
* Performance can be poor in open and complex scenes.
* Bindings can be configured through the SteamVR controller settings.
* There are currently no HUD elements indicating Long Jump Module usage.
* Two-handed weapons are planned for a later update.
* Manual reloading is not currently implemented and is planned for a future update.

## Black Mesa: Blue Shift

Black Mesa VR also supports **Black Mesa: Blue Shift**.

After installing Blue Shift, launch the game with:

```text
-game bshift
```

The same VR setup can then be used with the Blue Shift campaign. Calhoun never
wears the HEV suit, so this build keeps the bare-hand models for the whole
Blue Shift session. The weapon wheel and wrist HUD use Calhoun blue instead of
HEV amber.

## VR Features

### True Stereo Rendering

Black Mesa is rendered in true stereoscopic VR for the headset rather than simply displaying the desktop image.

### 6DOF Motion Controls

Full HMD and motion-controller 6DOF tracking for immersive gameplay.

### VR Weapon Wheel

A Half-Life 2 VR-style hexagonal weapon wheel for in-world weapon selection.

### Crowbar Melee

HL2VR-style motion-controlled crowbar melee. Swings that move the controller through space pulse `IN_ATTACK`.

### OpenXR

The project uses OpenXR for VR headset and controller integration.

### Wrist HUD

Health and armor sit on top of the hand. Ammo is a counter on the side of the
weapon. Game icons stay in world space instead of a flat overlay.

### VR Menus

The main menu appears in the headset with a cursor. The pause menu is a
transparent in-world overlay. Point at either with a motion controller, or
navigate with controller buttons.

## Project Status

The core VR experience is functional from launch through normal gameplay. Development is continuing on features, controls, performance, and general polish.

## Huge thanks to my testers for their patience and help:

TheRealBubble

SilentVortiguant

yakupagagaming
