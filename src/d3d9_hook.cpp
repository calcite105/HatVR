#include "d3d9_hook.h"
#include "dxvk_trace.h"
#include "logger.h"
#include "graphics_backend.h"
#include "vulkan_backend.h"
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <intrin.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <MinHook.h>
// DXVK's public D3D9 Vulkan interop interface. We only need the first two
// d3d9_interfaces.h.
MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDeviceHatVR : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(
        VkInstance* instance, VkPhysicalDevice* physicalDevice, VkDevice* device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(
        VkQueue* queue, uint32_t* queueIndex, uint32_t* queueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(IUnknown*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9*, DWORD) = 0;
};

namespace ahitvr
{

    // HatVR live SystemSettings compatibility overrides.
    static void ForceLiveMotionBlurOff()
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            DxvkPathTrace("HatVR motion-blur compatibility: main EXE handle unavailable");
            return;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);

        struct LiveSetting
        {
            const char* name;
            uintptr_t rva;
        };

        static const LiveSetting settings[] =
        {
            { "MotionBlur",             0x11039CC },
            { "MotionBlurPause",        0x11039D0 },
            { "bAllowMotionBlurScreen", 0x1103A24 },
            { "MotionBlurSkinning",     0x1103ADC },
            { "MotionBlurSamples",      0x1103A58 },
        };

        bool allOk = true;
        for (const LiveSetting& setting : settings)
        {
            volatile LONG* value =
                reinterpret_cast<volatile LONG*>(base + setting.rva);

            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(
                    const_cast<LONG*>(value),
                    &mbi,
                    sizeof(mbi)) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                DxvkPathTrace(
                    "HatVR motion-blur compatibility: %s address invalid addr=%p",
                    setting.name,
                    const_cast<LONG*>(value));
                allOk = false;
                continue;
            }

            InterlockedExchange(value, 0);

            DxvkPathTrace(
                "HatVR motion-blur compatibility: live %s=0 addr=%p",
                setting.name,
                const_cast<LONG*>(value));
        }

        DxvkPathTrace(
            "HatVR motion-blur compatibility: LIVE OVERRIDE %s",
            allOk ? "ACTIVE" : "PARTIAL");
    }

#include "core/shared_state.inl"
#include "include_order/openxr_runtime_order.inl"
#include "camera/camera_hooks.inl"
#include "ui/ui_discovery.inl"
#include "core/runtime_control.inl"
#include "rendering/d3d9_rendering.inl"
#include "ui/ui_rendering.inl"
#include "include_order/controller_input_order.inl"
#include "rendering/device_hooks.inl"
#include "core/config.inl"

bool AttachExistingD3D9Device(IDirect3DDevice9* device)
{
    LogCategory("DXVK", "HOTFIX-DIAG A00 AttachExistingD3D9Device FUNCTION ENTRY device=%p", device);
    // Build 25577316: defer the live motion-blur compatibility override.
    // the relocated globals are valid, but mutating them from the DXVK
    // discovery thread during attachment aborts startup on the hotfix build.

    DxvkPathTrace("VKBRIDGE-V5 PHASE5 BUILD-STAMP 2026-09-19D d3d9_hook.cpp ACTIVE");
    DxvkPathTrace("V11 AttachExistingD3D9Device ENTER device=%p", device);
    if (!device)
        return false;
    LogCategory("DXVK", "HOTFIX-DIAG A01 device non-null; about to QueryInterface DXVK interop");
    // No allocator hooks are installed in this pass.

    // device. This is read-only probing; we do not submit to DXVK's queue yet.
    ID3D9VkInteropDeviceHatVR* vkInterop = nullptr;
    const HRESULT interopHr = device->QueryInterface(
        __uuidof(ID3D9VkInteropDeviceHatVR), reinterpret_cast<void**>(&vkInterop));
    LogCategory("DXVK", "HOTFIX-DIAG A02 QueryInterface returned hr=0x%08X ptr=%p", (unsigned)interopHr, vkInterop);
    DxvkPathTrace("VKBRIDGE-V5 PHASE4A QueryInterface(ID3D9VkInteropDevice) hr=0x%08X ptr=%p",
        (unsigned)interopHr, vkInterop);
    if (SUCCEEDED(interopHr) && vkInterop)
    {
        VkInstance dxvkInstance = VK_NULL_HANDLE;
        VkPhysicalDevice dxvkPhysical = VK_NULL_HANDLE;
        VkDevice dxvkDevice = VK_NULL_HANDLE;
        VkQueue dxvkQueue = VK_NULL_HANDLE;
        uint32_t dxvkQueueIndex = UINT32_MAX;
        uint32_t dxvkQueueFamily = UINT32_MAX;
        LogCategory("DXVK", "HOTFIX-DIAG A03 about to GetVulkanHandles");
        vkInterop->GetVulkanHandles(&dxvkInstance, &dxvkPhysical, &dxvkDevice);
        LogCategory("DXVK", "HOTFIX-DIAG A04 GetVulkanHandles returned; about to GetSubmissionQueue");
        vkInterop->GetSubmissionQueue(&dxvkQueue, &dxvkQueueIndex, &dxvkQueueFamily);
        LogCategory("DXVK", "HOTFIX-DIAG A05 GetSubmissionQueue returned");
        DxvkPathTrace("VKBRIDGE-V5 PHASE4A DXVK-CONTEXT instance=%p physical=%p device=%p queue=%p family=%u index=%u",
            dxvkInstance, dxvkPhysical, dxvkDevice, dxvkQueue, dxvkQueueFamily, dxvkQueueIndex);

        // maximum two hops beyond the original handle.

        LogCategory("DXVK", "HOTFIX-DIAG A06 about to publish DXVK Vulkan context");
        SetDxvkVulkanContextForOpenXRProbe(
            dxvkInstance, dxvkPhysical, dxvkDevice, dxvkQueue, dxvkQueueFamily, dxvkQueueIndex);
        LogCategory("DXVK", "HOTFIX-DIAG A07 context published; about to Release interop");
        vkInterop->Release();
        LogCategory("DXVK", "HOTFIX-DIAG A08 interop released");
    }
    else
    {
        DxvkPathTrace("VKBRIDGE-V5 PHASE4A DXVK interop unavailable; normal Vulkan backend will continue unchanged");
    }

    // DXVK has already created the live device before HatVR gains control.
    // Recover its real parent IDirect3D9 and run the same core initialization
    // that the normal proxy path performs before CreateDevice.
    IDirect3D9* parent = nullptr;
    LogCategory("DXVK", "HOTFIX-DIAG A09 about to GetDirect3D");
    const HRESULT parentHr = device->GetDirect3D(&parent);
    LogCategory("DXVK", "HOTFIX-DIAG A10 GetDirect3D returned hr=0x%08X parent=%p", (unsigned)parentHr, parent);
    DxvkPathTrace("V11 GetDirect3D hr=0x%08X parent=%p",
        (unsigned)parentHr, parent);

    bool coreOk = false;
    if (SUCCEEDED(parentHr) && parent)
    {
        LogCategory("DXVK", "HOTFIX-DIAG A11 about to HookCreateDevice(parent)");
        coreOk = HookCreateDevice(parent);
        LogCategory("DXVK", "HOTFIX-DIAG A12 HookCreateDevice(parent) returned=%s", coreOk ? "TRUE" : "FALSE");
        DxvkPathTrace("V11 HookCreateDevice(parent)=%s", coreOk ? "TRUE" : "FALSE");
        parent->Release();
        parent = nullptr;
    }
    else
    {
        // we can still install the core UE3 hooks without a parent interface;
        // only the future CreateDevice interception would be unavailable.
        coreOk = EnsureCoreVrHooksInstalled();
        DxvkPathTrace("V11 fallback EnsureCoreVrHooksInstalled=%s", coreOk ? "TRUE" : "FALSE");
    }

    if (!coreOk)
    {
        DxvkPathTrace("V11 AttachExistingD3D9Device ABORT core initialization failed");
        return false;
    }

    // Load persistent HatVR user settings before the rendering/OpenXR backend
    // observes them. First startup creates HatVR.ini beside HatVR.dll.
    LoadHatVrConfig();

    // only the verified live-DXVK-device recovery path selects Vulkan.
    DxvkPathTrace("VKXR-V16T DXVK path selected; activating Vulkan OpenXR backend");
    const bool vkOk = ActivateVulkanBackend();
    if (!vkOk)
    {
        DxvkPathTrace("VKXR-V16T AttachExisting ABORT Vulkan activation failed");
        return false;
    }

    uint32_t vkEyeW = 0, vkEyeH = 0;
    if (!GetVulkanRecommendedEyeExtent(&vkEyeW, &vkEyeH))
    {
        DxvkPathTrace("VKXR-V16T FAILED to obtain runtime eye extent");
        return false;
    }
    // bootstrap. Native SBS creation, stereo eye copies, and UI extraction
    // already key off them.
    g_xrRuntimeEyeWidth = vkEyeW;
    g_xrRuntimeEyeHeight = vkEyeH;

    // session. The D3D12 bootstrap is skipped in this mode, so these globals
    // are otherwise unused and are safe to point at Vulkan's OpenXR objects.
    g_xrInstance = GetVulkanXrInstance();
    g_xrSession = GetVulkanXrSession();
    g_xrLocalSpace = GetVulkanLocalSpace();
    g_xrInitialized =
        g_xrInstance != XR_NULL_HANDLE &&
        g_xrSession != XR_NULL_HANDLE &&
        g_xrLocalSpace != XR_NULL_HANDLE;
    if (!g_xrInitialized)
    {
        DxvkPathTrace("VKXR-V16T Vulkan XR context publication FAILED");
        return false;
    }

    // apply vr-only camera changes after openxr is ready.
    SetUnifiedFirstPersonEnabled(g_firstPersonConfigured);
    if (!AV_V2InitializeControllerPoses())
        DxvkPathTrace("VKXR-V16T Vulkan controller/avatar action init FAILED");
    else if (!InitializeVrControllerInput())
        DxvkPathTrace("VKXR-V16T Vulkan XInput bridge init FAILED");
    else
        DxvkPathTrace("VKXR-V16T Vulkan controller + avatar actions READY");
    DxvkPathTrace("VKXR-V16T published runtime eye extent %ux%u to V16 renderer",
        vkEyeW, vkEyeH);

    // IDirect3DDevice9. HookDevice() below detects the active Vulkan backend
    // and skips only its old D3D12 OpenXR bootstrap.
    const bool deviceHooksOk = HookDevice(device);
    const bool ok = vkOk && deviceHooksOk;
    DxvkPathTrace("VKXR-V16T AttachExistingD3D9Device RETURN %s device=%p",
        ok ? "TRUE" : "FALSE", device);
    return ok;
}
}
