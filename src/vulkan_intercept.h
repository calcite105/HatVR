#pragma once
#include <vulkan/vulkan.h>

namespace ahitvr
{
    // rendering. Safe to call repeatedly.
    bool StartVulkanGpuBridgeTrace();

    // called by our OpenXR Vulkan backend once its device/queue are known so
    // the trace can distinguish HatVR's device from DXVK's device(s).
    void VulkanTraceMarkHatVRDevice(VkDevice device, VkQueue queue, uint32_t queueFamily);
}
