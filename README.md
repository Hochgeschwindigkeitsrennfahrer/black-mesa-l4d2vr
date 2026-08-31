# Black Mesa VR

<a href="https://www.youtube.com/watch?v=5oqQ8akfu3s">
  <img src="https://i.imgur.com/t7A5Mot.png" alt="Black Mesa VR">
</a>

A VR mod for the Steam version of **Black Mesa**, bringing 6DOF motion controls, true stereo rendering, OpenXR, and VR-first weapon interaction to the game.

The foundation of the VR implementation is based heavily on the approach pioneered by **L4D2VR**, adapted and extended for Black Mesa.

L4D2VR source:
https://github.com/keyou91/l4d2vr

## Current Status

**The project is working start-to-finish and is playable as a full VR mod.**

* True stereo rendering — Done
* 6DOF motion controls — Done
* VR weapon wheel — Half-Life 2 VR style — Done
* Crowbar melee — Work in progress
* OpenXR implementation — Done
* Wrist HUD with game icons — Done

## Known Issues

The VR experience is fully playable, but several features are still a work in progress:

* Crowbar swinging can be unreliable.
* Performance can be poor in open and complex scenes.
* Controls are still a work in progress. Bindings can be configured through the SteamVR controller settings.
* TAU cannon effects can emit from the player's eyes and may glitch out.
* Crossbow scope aiming does not currently work.
* The left hand is visible even during the intro sequence, before the HEV suit is acquired.
* There are currently no HUD elements indicating Long Jump Module usage.
* Two-handed weapons are planned for a later update.
* Manual reloading is not currently implemented and is planned for a future update.

## Black Mesa: Blue Shift

Black Mesa VR also supports **Black Mesa: Blue Shift**.

After installing Blue Shift, launch the game with:

```text
-game bshift
```

The same VR setup can then be used with the Blue Shift campaign.

## VR Features

### True Stereo Rendering

Black Mesa is rendered in true stereoscopic VR for the headset rather than simply displaying the desktop image.

### 6DOF Motion Controls

Full HMD and motion-controller 6DOF tracking for immersive gameplay.

### VR Weapon Wheel

A Half-Life 2 VR-style weapon wheel provides quick weapon selection using the VR controllers.

### Crowbar Melee

Motion-controlled crowbar melee is implemented and currently being refined.

### OpenXR

The project uses OpenXR for VR headset and controller integration.

### Wrist HUD

A VR wrist HUD displays useful game information and game icons while keeping the player's view clear.

## Project Status

The core VR experience is functional from launch through normal gameplay. Development is continuing on features, controls, performance, and general polish, with physical crowbar melee currently marked as work in progress.

## Repository

https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr/tree/main
