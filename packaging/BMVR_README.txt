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

RenderScale multiplies the working HMD-aspect fit, then aligns to 16 pixels.
Sizes taller than the game window are fitted back into the HWND. SteamVR's
full recommended eye size (~3296x3216 on a G2) blacked the world when used as
an offscreen RT; that path is skipped.

  RenderScale=1.0     ~1584 x 1440 on a 1440p window
                      Verified fused gameplay. Start here.

  RenderScale=1.25 / 1.50
                      Tried a larger G-buffer, then fitted back into the window
                      so it is not taller than the HWND.

After launch, bmvr_log.txt reports the size actually used, for example:
  Eye/G-buffer size 1584x1440 (CreateDevice HMD-aspect, window 2560x1440)


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
HL2 v_ models are oversized in world-space VR; ViewmodelScale=0.5 is the default.
Shooting aims with that controller; walking still follows where you look.
If the gun sits too far forward/back, edit VR\config.txt:

  ViewmodelPosOffsetX=16.0
  ViewmodelPosOffsetY=3.0
  ViewmodelPosOffsetZ=-2.0
  ControllerPitchTilt=-35.0

Those take effect on the next launch (config is read at DLL load).

Also in VR\config.txt (defaults):

  AutoMatQueueMode=true   menu/load = single-thread, gameplay = queued
  IPDScale=1.0            HeightOffset=0.0
  Haptics=true            HideCrosshair=true
  MatchHmdHz=true         DisableViewBob=true
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
