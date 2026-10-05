# HatVR
<img width="800" height="450" alt="ezgif com-optimize (4)" src="https://github.com/user-attachments/assets/2f2cb980-3900-409f-ab2f-c783a330f79d" />


HatVR is a VR mod for A Hat in Time, built around playing the original game in VR without changing what it is. It supports stereoscopic rendering, 6DOF head tracking through OpenXR, normal controllers and VR controllers, an in-headset settings menu, Theater Mode, upscaling, and an experimental first person mode using the actual player model as a VR avatar.

Third person is the main way to play and is what I'd recommend for playing through the game. First person is there if you want to mess around with it, but it's much more experimental.

> **Before Playing**
>
> HatVR is still experimental. There are visual issues, camera sequences that don't always translate well to VR, and some things that may render incorrectly in one eye. I also wouldn't recommend using HatVR for your first playthrough of A Hat in Time.
>
> HatVR relies on reverse engineering parts of the game, so a game update can potentially break it. If A Hat in Time updates and HatVR suddenly stops working, check for a newer version.

## Features

* Stereoscopic VR rendering and 6DOF head tracking through OpenXR
* Third person VR with normal controller support
* VR controller support, haptics and optional motion controls
* Experimental first person mode with a player-model VR avatar
* In-headset settings menu and adjustable VR HUD
* Stereoscopic Theater Mode with automatic cutscene switching
* Desktop spectator view
* NIS and FSR 1 upscaling
* PlayStation and Nintendo Switch controller icons (the default Xbox icons have rendering issues)
* Compatibility with existing levels and mods

## Setup

### Requirements

* Windows

  * Linux may work, but I haven't tested it.
* A Hat in Time
* An OpenXR-compatible headset and runtime
* A normal controller or supported VR controllers

Set A Hat in Time's **Graphics API to Vulkan. Do not use DirectX 9 with HatVR.**

HatVR uses the game's DXVK/Vulkan rendering path. The version of DXVK included with the game may work, but I recommend updating the game's DXVK to **[DXVK 3.1.1](https://github.com/doitsujin/dxvk/releases/tag/v3.1.1)** before using HatVR.

Download HatVR and place the release files in `HatinTime\Binaries\Win64`. Start SteamVR or your preferred OpenXR runtime and then launch the game normally.

HatVR should start automatically. A message in the headset will tell you how to open the HatVR menu, and settings are saved to `HatVR.ini`. If OpenXR isn't available, A Hat in Time can still start normally in flatscreen without HatVR's VR gameplay changes being applied. HatVR also stays inactive in `HatinTimeEditor.exe`, so it shouldn't interfere with the Modding Tools.

To uninstall HatVR, remove its files from the `Win64` folder.

## Using HatVR

Double-tap **Y / Triangle** on a controller to open the HatVR menu. A single press still opens A Hat in Time's normal menu. The HatVR menu can currently only be opened using a controller or VR controller, not the keyboard or mouse.

Most things can be changed from inside the headset, including the renderer, first person, Theater Mode, controls, HUD, spectator view and upscaling.

### Rendering

HatVR has two main stereo rendering methods because neither works perfectly with everything in the game.

**Stereo** is the default and uses a method based around A Hat in Time's own split-screen rendering. Most of the game works well with it, but some objects and effects can occasionally render incorrectly or disappear in the right eye.

**Synchronized Sequential** renders the eyes separately from the same game state and can fix things that don't render correctly in Stereo, including some cutscenes. It has its own tradeoffs, but if something is clearly broken in one eye, Sequential is the first thing I'd try.

Ambient Occlusion can look wrong on some headsets. If that happens, turn **Ambient Occlusion off**.

### Performance and Upscaling

A Hat in Time isn't normally a very demanding game, but its rendering pipeline wasn't built around stereo rendering, and VR also asks for higher resolutions and framerates than the game would normally run at. Performance will depend on the level, headset resolution, refresh rate and hardware.

HatVR includes **NVIDIA Image Scaling (NIS)** and **AMD FidelityFX Super Resolution 1 (FSR 1)**. These upscale the finished game image before it's sent to OpenXR, so you can run A Hat in Time at a lower resolution while using a larger VR output. For example, +100% Output Upscale can turn a 1440p game image into a 2880p OpenXR output without changing the resolution the game itself renders at.

If you're running close to your system's memory limit, having a few GB of page file or swap space available is also a good idea.

### VR Controllers and First Person

VR controller input is mapped into A Hat in Time's existing controller controls, and normal controller vibration is sent back through OpenXR as haptics. **Umbrella Motion Controls** can optionally let you attack by swinging the right controller.

**Right-Hand Hookshot** lets you hold the right grip and use the right trigger for the Hookshot. This takes over the normal right grip/R input while enabled, so it can interfere with menus or anything else that expects that button.

First Person uses the actual player model as a VR avatar rather than simply putting the camera inside the character's head. The headset controls the head/body position while the VR controllers drive the arms and the game continues handling the rest of the character's animation.

It's very experimental, different characters, outfits, and animations can behave differently, and plenty of camera sequences weren't made to be viewed this way. **Third person is still the recommended way to play through the game.**

### Theater Mode

Theater Mode displays the game on a fixed stereoscopic screen instead of putting the game's camera directly in VR. HatVR can automatically switch to it during certain cutscenes and return to normal VR afterward, or you can turn it on manually.

### PSVR2

HatVR has optional PSVR2-specific features through **PSVR2 Toolkit**, including headset vibration, an adaptive trigger effect for Right-Hand Hookshot, and eye tracking for choosing directions in the Hat Wheel. Physical right-stick input takes priority over eye tracking.

**HMD Rumble is experimental and disabled by default.** It should only be enabled when using a PSVR2. Enabling it with another headset can cause unnecessary stalls while HatVR attempts to initialize PSVR2 Toolkit.

## Known Issues

* The HatVR menu currently needs to be opened with a normal controller or VR controller. There isn't a keyboard or mouse shortcut for it yet.
* Mouse input doesn't work very well in VR since it doesn't compensate for the much larger UI resolution.
* Some objects and effects can render incorrectly or disappear in one eye in Stereo. Try Sequential when this happens.
* Some cutscenes can have one-eye rendering problems in Stereo, and Sequential can help with some of them.
* Ambient Occlusion can look wrong in Stereo on some headsets. Turning it off is currently the best workaround.
* Some scripted cameras and cutscenes can still behave strangely in VR.
* First Person and the VR avatar are experimental, especially with different or modded player models. Your fingers may randomly distort then quickly go back to normal for example.
* Looking around freely can occasionally reveal things the original camera wasn't meant to show.

If you run into something that isn't covered here, feel free to report it.

## Credits

**BOTW BetterVR** — Used as a reference while developing the experimental first person avatar and weapon swinging.

**BL1GOTYVR** — A useful reference while working with DXVK/Vulkan and Unreal Engine 3's rendering behavior.

**First Person Camera Badge Mod** — Used as a reference for our first person implementation.

HatVR uses **MinHook**, the **OpenXR SDK**, **DXVK**, **Dear ImGui**, **AMD FidelityFX Super Resolution 1**, and **NVIDIA Image Scaling**.

The full HatVR source is available with the project.

A Hat in Time is owned by Gears for Breakfast. HatVR is an unofficial project and is not affiliated with or endorsed by Gears for Breakfast.
