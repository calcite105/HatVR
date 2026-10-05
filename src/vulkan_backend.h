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
    bool SubmitVulkanOpenXREyes(
        IDirect3DTexture9* leftEye,
        IDirect3DTexture9* rightEye,
        UINT width,
        UINT height,
        const XrView submitViews[2],
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
