#pragma once

// Vulkan Win32 extension declarations (external memory/semaphore HANDLE APIs)
// must be enabled before the first inclusion of vulkan.h.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <windows.h>
#include <d3d9.h>
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

namespace ahitvr
{
    // True only when the active OpenXR instance enabled native Steam Frame inputs.
    extern bool g_hatVrFrameControllerExtensionEnabled;
    // Render-thread-safe, display-only eye-wheel calibration sample. The gameplay
    // input path writes this; the Vulkan debug quad never feeds input back.
    struct HatVrEyeWheelDebugSample
    {
        float rawX = 0.0f, rawY = 0.0f;       // normalized HUD coordinates
        float smoothX = 0.0f, smoothY = 0.0f;
        float stickX = 0.0f, stickY = 0.0f;
        bool wheelOpen = false;
        bool gazeValid = false;
        unsigned long long updatedMs = 0;
    };
    void HatVrPublishEyeWheelDebug(const HatVrEyeWheelDebugSample& sample);
    HatVrEyeWheelDebugSample HatVrReadEyeWheelDebug();

    // DXVK/Vulkan-only OpenXR backend. The normal DX9/D3D12 renderer does not
    // call these functions.
    bool ActivateVulkanBackend();
    bool IsVulkanBackendActive();

    // HatVR uses them only to ask OpenXR whether the runtime selects the same
    // physical device. No commands are submitted to the DXVK queue yet.
    void SetDxvkVulkanContextForOpenXRProbe(
        VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
        VkQueue queue, uint32_t queueFamily, uint32_t queueIndex);

    // Runtime-recommended PRIMARY_STEREO eye extent. This replaces the state
    bool GetVulkanRecommendedEyeExtent(uint32_t* width, uint32_t* height);

    // system can bind to the same instance/session/space instead of creating
    // a second feature implementation.
    XrInstance GetVulkanXrInstance();
    XrSession GetVulkanXrSession();
    XrSpace GetVulkanLocalSpace();
    bool IsVulkanXrSessionRunning();

    // Starts/polls the Vulkan OpenXR frame and returns live stereo views.
    bool BeginVulkanOpenXRFrame(
        XrView outViews[2],
        XrTime* outPredictedDisplayTime,
        bool* outShouldRender);

    // First-output bridge: read retained D3D9 eye textures back to CPU, upload
    // through Vulkan staging memory, and submit a stereo projection layer.
    // Physical aspect of the mono game-UI quad can follow the actual Canvas
    // source in NewSequentialStereo's eye-sized mode; does not alter headset projection.
    void SetNewSequentialStereoUiPixelAspectCorrection(bool enabled);
    void SetNewSequentialStereoUiCrop16x9(bool enabled);

    bool SubmitVulkanOpenXREyes(
        IDirect3DTexture9* leftEye,
        IDirect3DTexture9* rightEye,
        UINT width,
        UINT height,
        const XrView submitViews[2],
        bool reusePreviousEyes, // NewSequentialStereo miss: no eye copies, reproject XR swapchains
        IDirect3DTexture9* finishedUiTexture,
        bool theaterMode,
        bool menuOpen,
        bool nativeStereo,
        bool firstPersonEnabled,
        bool autoTheaterCutscenes,
        bool overrideLockedCameras,
        bool disablePlayerFade,
        bool rightHandHookshot,
        bool umbrellaMotionControls,
        bool playStationIcons,
        bool nintendoSwitchIcons,
        float hudScale,
        float hudDistance,
        float hudHeight,
        bool hudHeadLocked,
        int spectatorView,
        int spectatorUiMode,
        int menuPage,
        int menuSelection,
        bool menuInsideCategory,
        int uiDebugCandidateIndex,
        unsigned int uiDebugCandidateCount,
        unsigned long long uiDebugCandidateHash,
        unsigned long long uiDebugCandidateHits,
        int uiDebugPreviewMode,
        int uiDebugPermanentRoute);

    void ShutdownVulkanBackend();
}
