#pragma once

#include <d3d9.h>

namespace ahitvr
{
    bool HookCreateDevice(IDirect3D9* d3d9);
    // Attach the existing renderer hooks to a device that was created before
    // HatVR loaded (DXVK Vulkan startup path).
    bool AttachExistingD3D9Device(IDirect3DDevice9* device);
}
