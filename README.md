# HatVR

HatVR is a WIP VR mod/conversion for A Hat in Time.

The goal is to get A Hat in Time properly running in VR while keeping the game itself as unchanged as possible, allowing existing levels and mods to continue working normally. HatVR supports stereoscopic rendering and 6DOF head tracking through OpenXR, and the game can still be played with a normal controller.

First person VR and VR controller support are also being worked on, although both are still experimental.

> **Warning**
>
> HatVR is still experimental and in active development. Expect bugs, visual issues, broken camera sequences, and possible crashes.

## Features

* Stereoscopic VR rendering
* 6DOF head tracking
* OpenXR support
* Support for playing with a standard controller
* Experimental VR controller support
* Experimental first person mode with an avatar system using the player model for VR arms
* In-headset HatVR menu
* Stereoscopic Theater Mode for cutscenes and playing on a screen
* Designed to remain compatible with existing A Hat in Time levels and mods

## Requirements

* Windows

  * HatVR may work on Linux, but I haven't tested it.
* A copy of A Hat in Time
* An OpenXR-compatible headset
* A controller is currently recommended

## Setup

HatVR currently uses DXVK and Vulkan for its VR rendering path.

1. Set A Hat in Time's Graphics API to Vulkan in the game's settings. While HatVR still has DX9 support left over from prototyping, it will have worse performance than Vulkan.
2. Download the latest HatVR release.
3. Open your A Hat in Time installation folder.
4. Go to `HatinTime\Binaries\Win64`.
5. Place the HatVR files from the release into the `Win64` folder.
6. Start SteamVR or your preferred OpenXR runtime.
7. Launch A Hat in Time normally.

HatVR should start automatically with the game.

To uninstall HatVR, remove the HatVR files from the `Win64` folder.

## RAM Usage

Please note that this game just eats up RAM.

I highly recommend replacing the game's `dxvk.dll` with the latest version of DXVK. While it won't stop high RAM usage completely, in my testing it makes the issue less common and can be the difference between something like 16 GB of RAM being used and around 11 GB being used.

This is still something I'm looking into, but at the moment it seems to mostly be an issue with the game itself rather than HatVR.

## Performance

Performance is still something I'm working on, although at this point the game is very playable for me.

HatVR currently uses DXVK and Vulkan to transfer the game to OpenXR almost entirely on the GPU. Earlier versions had to copy the game through the CPU, which worked, but had noticeably worse performance and latency.

That being said, A Hat in Time will probably always be a bit hard to run in VR. The game isn't particularly difficult to run at something like 1080p 60 FPS, but once you start asking for higher resolutions and framerates it can get surprisingly demanding, and unfortunately VR asks for both.

Your experience is obviously going to depend on your headset, resolution and hardware, so I'd like to hear how it runs on other systems.

## VR Controllers

HatVR has experimental support for using normal VR controllers to play the game.

The controllers are mapped into the game's existing controller input, meaning the game still receives mostly normal gamepad controls rather than requiring special support from A Hat in Time itself.

Basic buttons, triggers, sticks and other controller inputs are supported. HatVR also tracks the physical position of the controllers for the experimental player avatar.

There is also an experimental motion attack. Swinging the right controller can trigger Hat Kid's normal attack.

VR controller support is still being worked on, so using a standard controller may currently give you a more consistent experience.

## HatVR Menu

HatVR has a basic menu that can be opened from a VR controller.

At the moment the menu is mostly being used for VR-specific options rather than replacing the game's normal menus.

One of the current options is **Theater Mode**, which displays the game on a fixed stereo screen in front of you instead of using the normal immersive VR camera. This can be useful for parts of the game that don't behave well in full VR.

The menu and its controls are still a work in progress.

## First Person

A custom first person mode is currently being worked on.

The goal is not just to move the camera into Hat Kid's head, but to eventually make the normal player model work as a usable VR avatar. The current implementation already contains experimental head and arm positioning using the headset and VR controllers, but this system is still heavily in development and isn't something I'd consider finished yet.

The normal third person camera is still the main way I recommend playing HatVR right now.

## To-do

do this later!

## Current Issues

HatVR is still a work in progress, and there are plenty of things that aren't completely solved yet.

* Some UI elements may look or behave incorrectly in VR.
* Some cutscenes and scripted camera sequences may not work correctly. For example, Down with the Mafia's boss fight locks the camera.
* First person mode and the VR avatar are still experimental.
* Performance can vary quite a bit depending on the area, headset resolution and hardware. I'm currently looking into AFR.
* A Hat in Time loves RAM.
* Some parts of the game were never designed with a freely moving VR camera in mind, so expect to sometimes see things you normally weren't supposed to see.

If you find a bug, including one that isn't listed here, feel free to open an issue.

## Development

HatVR began as an experiment to see how far I could push A Hat in Time into VR before spiraling into a full-on mod.

A lot of HatVR is still active reverse engineering, especially around the camera, UI and player avatar. Because of that, some of the source is deliberately kept together in modules that share the same translation unit rather than being split into a cleaner architecture while those systems are still changing constantly.

The source is available both so people can see exactly what the DLL is doing and so anyone interested can experiment with or improve it.

## Credits

**[BOTW BetterVR](https://github.com/Crementif/BotW-BetterVR)**. HatVR's experimental first person avatar solution and weapon swinging were heavily based around ideas from its implementation, since both projects are dealing with turning third person games into something that can work from a first person VR perspective, and I already knew that mod's solution worked well.

**[BL1GOTYVR](https://github.com/Mastersellz/BL1GOTYVR)**. Borderlands 1 also runs on Unreal Engine 3, and that project was a very useful reference later in HatVR's development once we got past prototyping.

**First Person Camera Badge**. Used as a basis for our custom first person mode.

HatVR also uses:

* **MinHook** by Tsuda Kageyu and its contributors
* **OpenXR SDK** by the Khronos Group
* **DXVK**

A Hat in Time is owned by Gears for Breakfast. HatVR is an unofficial project and is not affiliated with or endorsed by Gears for Breakfast.
