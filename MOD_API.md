# HatVR Mod API

HatVR has an optional API for A Hat in Time mods that want access to some of its VR state and tracking data.

The API is disabled by default and can be enabled with **Mod API** in HatVR's settings. HatVR itself does not require it.

## What It Exposes

The current API provides:

- API version and capabilities
- Whether VR is currently active
- Whether first-person mode is enabled
- HMD position and rotation
- Left controller position and rotation
- Right controller position and rotation
- Tracking validity
- HatVR's solved hand orientation when available

The API is currently read-only. Mods can react to HatVR's state, but cannot change HatVR settings through it.

## Using the API

HatVR looks for API callbacks on loaded `GameMod` instances.

A mod only needs to implement the callbacks it actually uses. There is no requirement to implement every part of the API.

When HatVR finds a compatible mod, it calls:

```uc
function HatVRAPI_Connected(int APIVersion, int Capabilities)
{
}
```

`APIVersion` can be checked before relying on newer API features.

`Capabilities` describes which parts of the API are available.

Mods should continue working normally if HatVR is not installed, the Mod API is disabled, or a particular capability is unavailable.

## State

For basic HatVR state, implement:

```uc
function HatVRAPI_StateChanged(int StateFlags)
{
}
```

This is intended for state that changes occasionally rather than tracking data that changes continuously.

For example, a mod can use it to tell whether HatVR is active or whether first-person mode is enabled.

## Tracking

Tracking is separated by device. Implement only the callbacks for the devices your mod needs.

For occasional HMD updates:

```uc
function HatVRAPI_HMD20Hz(
    vector Position,
    Quat Orientation,
    int TrackingFlags)
{
}
```

For the left controller:

```uc
function HatVRAPI_LeftController20Hz(
    vector Position,
    Quat Orientation,
    Quat SolvedHandOrientation,
    int TrackingFlags)
{
}
```

And for the right controller:

```uc
function HatVRAPI_RightController20Hz(
    vector Position,
    Quat Orientation,
    Quat SolvedHandOrientation,
    int TrackingFlags)
{
}
```

The controller callbacks include both the tracked controller orientation and HatVR's solved hand orientation when available.

Check `TrackingFlags` before using tracking data.

## Tracking Update Rate

Only request tracking as often as your mod actually needs it.

If your mod only needs occasional tracking updates, prefer the 20 Hz callbacks. Use the per-frame callbacks when something needs to closely follow the headset or controllers, such as an attached object.

The per-frame equivalents are:

```uc
function HatVRAPI_HMDFrame(...)
function HatVRAPI_LeftControllerFrame(...)
function HatVRAPI_RightControllerFrame(...)
```

Per-frame tracking is supported and is designed to be lightweight. HatVR reuses tracking data it already calculates and avoids repeated discovery, function lookup, allocation, and other unnecessary work in the tracking path.

There is no benefit to using the per-frame callbacks when 20 Hz is already enough for what your mod is doing.

## Coordinate Space

Tracking positions and orientations are provided after HatVR's conversion into A Hat in Time's coordinate system.

Controller callbacks may also provide HatVR's solved hand orientation. This is useful when something should follow the orientation HatVR uses for the player's hand rather than the raw controller orientation.

## API Availability

Mods should not assume the API will always be available.

HatVR may not be installed, the player may have **Mod API** disabled, VR may not currently be active, or individual tracked devices may be unavailable.

Treat the API as an optional source of additional information rather than something required for the rest of your mod to function.

## Performance

The API is designed so unused features stay cheap.

HatVR does not continuously send every piece of tracking data to every mod. Only callbacks actually implemented by a consumer are used.

Tracking data is taken from state HatVR already maintains. The tracking path does not perform repeated mod discovery or function lookup.

This also means implementing only what you need is preferable. If you only care about the HMD, there is no reason to implement either controller callback.

## Versioning

Check the version supplied through `HatVRAPI_Connected`.

Existing API behavior will be kept compatible where practical. New functionality may be exposed through later API versions or additional capabilities.

For the initial API:

```uc
const HATVR_API_VERSION = 1;
```

That's all that's required to start using it. Implement the callbacks relevant to your mod, handle the API being unavailable, and use the lowest tracking update rate that makes sense for what you're doing.