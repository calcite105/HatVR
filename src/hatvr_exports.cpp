#include <windows.h>
#include <d3d9.h>

#include "d3d9_backend.h"
#include "graphics_backend.h"
#include "dxvk_intercept.h"
#include "vulkan_intercept.h"
#include "logger.h"
#include "dxvk_trace.h"

namespace
{
    bool IsHatInTimeGameProcess()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return false;

        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                name = p + 1;
        }

        return _wcsicmp(name, L"HatinTimeGame.exe") == 0;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI HatVR_HookCreateDevice(IDirect3D9* d3d9)
{
    if (!IsHatInTimeGameProcess())
        return FALSE;

    ahitvr::Log("DXVK-V4: HATVR BUILD ACTIVE");
    ahitvr::Log("DXVK-EARLY-V3: HatVR entry");
    ahitvr::StartDxvkEarlyLoadInterceptor();
    return ahitvr::ActivateD3D9Backend(d3d9) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) const char* WINAPI HatVR_GetGraphicsApiName()
{
    return ahitvr::GetGraphicsApiName();
}

extern "C" __declspec(dllexport) BOOL WINAPI HatVR_EarlyBootstrap()
{
    if (!IsHatInTimeGameProcess())
        return FALSE;

    ahitvr::Log("PHASE8.3 BUILD=2026-09-20G HatVR_EarlyBootstrap ENTERED");
    ahitvr::DxvkPathTrace("PHASE8.3 BUILD=2026-09-20G EARLY-BOOTSTRAP ACTIVE pid=%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));
    // the old VKBRIDGE allocation/image tracer generated enormous logs and is
    // intentionally disabled for release. The normal Vulkan/OpenXR startup
    // path already records the useful device/runtime information centrally.
    ahitvr::StartVulkanGpuBridgeTrace();
    const bool ok = ahitvr::StartDxvkEarlyLoadInterceptor();
    ahitvr::Log("DXVK-V5: HatVR_EarlyBootstrap result=%s", ok ? "TRUE" : "FALSE");

    const bool discovery = ahitvr::StartDxvkExistingDeviceDiscovery();
    ahitvr::Log("DXVK-V8: verified live-device discovery result=%s",
        discovery ? "TRUE" : "FALSE");

    return (ok && discovery) ? TRUE : FALSE;
}
