# HatVR Mod API

HatVR has an optional API for A Hat in Time mods that want access to some of its VR state and tracking data.

The API is disabled by default and can be enabled with **Mod API** in HatVR's settings. HatVR itself does not require it.

The current API is read-only. Mods can react to HatVR's state and tracking data, but cannot change HatVR settings through it.

## Using the API

HatVR looks for API callbacks on loaded `GameMod` instances. A mod must implement `HatVRAPI_Initialize` to be detected as an API consumer. Everything else is optional, so only implement the callbacks your mod actually needs.

```uc
function HatVRAPI_Initialize(int APIVersion, int Capabilities)
{
}
```

For the initial API:

```uc
const HATVR_API_VERSION = 1;
```

`APIVersion` should be checked before relying on behavior added by later API versions. `Capabilities` is reserved for reporting supported API features.

Mods should continue working normally if HatVR is not installed or the player has **Mod API** disabled.

## State

For HatVR state changes, implement:

```uc
function HatVRAPI_StateChanged(
    int APIVersion,
    int StateFlags,
    int TrackingMask)
{
}
```

HatVR sends this when the consumer is first registered and again when the state or tracking availability changes.

`StateFlags` currently uses:

- Bit 0 (`1`) - an OpenXR session is running
- Bit 1 (`2`) - HatVR first person is active

`TrackingMask` currently uses:

- Bit 0 (`1`) - HMD tracking is available
- Bit 1 (`2`) - left controller tracking is available
- Bit 2 (`4`) - right controller tracking is available

Check the bits you need rather than assuming every tracked device is available.

## Tracking

Tracking is separated by device. Each tracking callback currently has the same parameter layout:

```uc
function HatVRAPI_HMD20Hz(
    int Device,
    int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}
```

The controller versions are:

```uc
function HatVRAPI_LeftController20Hz(
    int Device,
    int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}

function HatVRAPI_RightController20Hz(
    int Device,
    int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}
```

`Device` is `0` for the HMD, `1` for the left controller, and `2` for the right controller. `Valid` is `1` when HatVR currently has a valid pose for that device and `0` otherwise. Check `Valid` before using the rest of the pose.

`X`, `Y`, and `Z` are positions converted into A Hat in Time's coordinate system and world-unit scale, relative to HatVR's tracking origin.

`Pitch`, `Yaw`, and `Roll` use Unreal rotator units.

For the HMD, the quaternion is the tracked HMD orientation. For controllers, the quaternion uses HatVR's solved hand orientation when it is available, otherwise it falls back to the tracked controller orientation. The controller `Pitch`, `Yaw`, and `Roll` values are based on the tracked controller orientation.

## Tracking Update Rate

Only request tracking as often as your mod actually needs it.

The callbacks above run at up to roughly 20 Hz. For something that needs to follow a tracked device every rendered frame, use the per-frame equivalent with the same parameters:

```uc
function HatVRAPI_HMDFrame(
    int Device, int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}

function HatVRAPI_LeftControllerFrame(
    int Device, int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}

function HatVRAPI_RightControllerFrame(
    int Device, int Valid,
    float X, float Y, float Z,
    float Qx, float Qy, float Qz, float Qw,
    int Pitch, int Yaw, int Roll)
{
}
```

For each device, the per-frame callback takes priority over its 20 Hz callback. If you implement `HatVRAPI_HMDFrame`, for example, HatVR uses that instead of also calling `HatVRAPI_HMD20Hz`. Pick one update rate for each device.

Per-frame tracking reuses data HatVR already calculates. Consumer discovery and callback lookup are cached rather than being repeated in the tracking path, but there is still no reason to request updates every frame when 20 Hz is enough for what your mod is doing.

## API Availability

Mods should treat the API as optional. HatVR may not be installed, **Mod API** may be disabled, VR may not currently be active, or individual tracked devices may be unavailable.

A mod only needs to implement the callbacks it uses, other than `HatVRAPI_Initialize`, which is how HatVR identifies the mod as an API consumer.

**The Mod API is experimental.** It has been tested with a real `GameMod` receiving live tracking data, but it hasn't been tested extensively yet and may change in the future.

## Versioning

Check `APIVersion` supplied through `HatVRAPI_Initialize` and `HatVRAPI_StateChanged`.

Existing API behavior will be kept compatible where practical. New functionality may be exposed through later API versions or additional capabilities.
