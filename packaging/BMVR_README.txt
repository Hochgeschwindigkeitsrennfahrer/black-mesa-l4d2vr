Black Mesa VR — drag-and-drop install
=====================================

Quit Black Mesa first. A running bms.exe will not load a DLL you copy over it.


1) Copy into the game folder
----------------------------
Copy EVERYTHING in this folder into:

  Steam\steamapps\common\Black Mesa

That is the folder that already contains bms.exe. Merge / replace when Windows asks.

This puts d3d9.dll in all three places the game can load it:

  Black Mesa\d3d9.dll
  Black Mesa\bin\d3d9.dll
  Black Mesa\bin\thirdparty\dxvk-windows-x86\d3d9.dll

Do not copy d3d9.dll into only one of those. Native Direct3D 9 never loads this mod.

If you already tuned VR\config.txt, keep your copy (do not overwrite it).

This zip also includes:

  Black Mesa\VR\hands\          HEV glove meshes
  Black Mesa\bms\cfg\bmvr.cfg   crosshair off / VR QoL cvars

If bms\cfg\autoexec.cfg exists, add this line (once):

  exec bmvr

If it does not exist, create it with that single line.


2) Steam launch options
-----------------------
Right-click Black Mesa → Properties → Launch Options:

  -heapsize 524288 -processheap -high -novid -windowed -oldgameui

-oldgameui is required. The new game UI is upside-down in the headset and the
world stays black after the load screen.

If the in-game video / renderer menu is "Direct3D 9", also add:

  -enabledxvk

This DLL is DXVK. Native D3D9 will not show VR.


3) Launch
---------
1. Start SteamVR.
2. Launch Black Mesa from Steam (not a leftover desktop shortcut that skips SteamVR).
3. Confirm Black Mesa\bmvr_log.txt appears this session and starts with:
     BMVR d3d9.dll loaded
4. Look in the headset. Load / tracking alone is not success — you should see the game.


4) Resolution (RenderScale)
---------------------------
Edit:

  Black Mesa\VR\config.txt

Then fully quit and restart the game. Eye size is chosen at CreateDevice; changing
the file while the game is running does nothing.

RenderScale multiplies SteamVR's recommended per-eye size (not the window),
then aligns to 16 pixels (cap 4096). The headset copies are that size; the
game still renders at the window (G-buffers stay 2560x1440 on a 1440p
window) and is scaled into the eyes. Growing the world buffers to SteamVR
size left a warped strip and garbage in the HMD. Add `hmd_offscreen` to
bmvr_skip.txt to force window-fit eyes. To get more *source* pixels, raise
the game's own resolution (window/fullscreen size).

  RenderScale=1.0     SteamVR recommended (typical Index/G2 is taller than 1080p)
  RenderScale=0.75    cheaper if GPU bound
  RenderScale=1.25    extra SS on top of SteamVR

A 1080p window with a headset recommended height above 1080 is supported.
This zip renders at the window and scales into SteamVR-sized eyes. An older
zip that logged "HMD native G-buffer 1456x1808" and "Eye/G-buffer size
1456x1808 (CreateDevice HMD-aspect" on a 1920x1080 window treated that as
the engine framebuffer and could not run. Confirm this zip with:
  offscreen=1 hmd_world=0
  Eye RT ... window 1920x1080
  Skip hmd_native size ... over window ...

After launch, bmvr_log.txt reports the size actually used, for example:
  Eye RT 2016x2240 (offscreen rec=2016x2240 RenderScale=1.00 window 1920x1080)


5) Controllers (HP Reverb G2 / WMR / Touch)
-------------------------------------------
Left stick walk, right stick turn, right trigger attack, left trigger alt-fire,
right B use, right A jump, right grip crouch, left grip reload,
left-stick click recenter, right-stick click flashlight.
Left Y next weapon. Left X sprint. Right-stick dpad north/south prev/next weapon.
Left menu / system button opens the in-game ESC menu (same as keyboard Escape).
While that menu is up, point the right controller at it and pull the trigger to click.

If sticks or the new pause/sprint bindings do nothing, open SteamVR Bindings for
this app and reset the layout to the default in VR\SteamVRActionManifest.

The first-person weapon follows the right controller (uncoupled viewmodel).
HL2 v_ models are oversized in world-space VR; ViewmodelScale=0.68 is the default
(~25% smaller than the L4D2VR-matched 0.91). Crowbar stays full size.
Shooting aims with that controller; walking still follows where you look.
If the gun sits too far forward/back, edit VR\config.txt:

  ViewmodelPosOffsetX=16.0
  ViewmodelPosOffsetY=3.0
  ViewmodelPosOffsetZ=-2.0
  ControllerPitchTilt=-35.0

Those take effect on the next launch (config is read at DLL load).

Also in VR\config.txt (defaults):

  AutoMatQueueMode=false  leave engine mat_queue_mode alone (menu still 0)
  ForceOpenVis=false      keep PVS/occlusion (true = old whole-map vis)
  StereoBlitGpuFlush=false  do not CPU-wait GPU between stereo eyes
  CapEyesToWindow=true    do not upscale 1080p world into rec-sized eyes
  IPDScale=1.0            HeightOffset=0.0
  Haptics=true            HideCrosshair=true
  MatchHmdHz=false        (unused for fps; fps_max is 0 / uncapped)
  DisableViewBob=true
  LeftHanded=false        RecenterResetsYaw=true
  HideLocalPlayerModel=true

If AutoMatQueueMode crashes, delete is not needed — the next launch skips
mat_queue (bmvr_skip.txt next to bms.exe). Set AutoMatQueueMode=false to
leave the engine's own mat_queue_mode alone (menu still forced to 0).


6) If it does not load
----------------------
- Game was still running during the copy. Quit, copy again, relaunch.
- Video menu is Direct3D 9. Add -enabledxvk, or set Vulkan/DXVK in the launcher.
- Steam "Verify integrity" restores stock d3d9.dll and dxvk.conf. Copy this folder again.
- Log next to bms.exe never appears → this DLL did not load.
- Headset waiting room / black desktop after install → you are on native D3D9 or
  an old d3d9.dll is still mapped.

Need -oldgameui every launch. Do not Y-flip the 2D path to "fix" the new UI.
