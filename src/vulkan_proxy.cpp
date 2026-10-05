#include <windows.h>

namespace
{
    HMODULE g_realVulkan = nullptr;
    HMODULE g_hatVR = nullptr;

    void ProxyLog(const char* text)
    {
        HMODULE selfModule = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&ProxyLog),
                &selfModule))
            return;

        char modulePath[MAX_PATH] = {};
        if (!GetModuleFileNameA(selfModule, modulePath, MAX_PATH))
            return;

        char* lastSlash = nullptr;
        for (char* p = modulePath; *p; ++p)
            if (*p == '\\' || *p == '/')
                lastSlash = p;

        if (!lastSlash)
            return;

        *(lastSlash + 1) = '\0';

        char logPath[MAX_PATH] = {};
        lstrcpyA(logPath, modulePath);
        lstrcatA(logPath, "AHiTVR_VULKAN_PROXY.log");

        HANDLE file = CreateFileA(
            logPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
            return;

        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
        CloseHandle(file);
    }

    bool GetProxyDirectory(char (&directory)[MAX_PATH])
    {
        HMODULE selfModule = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&GetProxyDirectory),
                &selfModule))
            return false;

        if (!GetModuleFileNameA(selfModule, directory, MAX_PATH))
            return false;

        char* lastSlash = nullptr;
        for (char* p = directory; *p; ++p)
            if (*p == '\\' || *p == '/')
                lastSlash = p;

        if (!lastSlash)
            return false;

        *(lastSlash + 1) = '\0';
        return true;
    }

    bool EnsureHatVR()
    {
        if (g_hatVR)
            return true;

        char path[MAX_PATH] = {};
        if (!GetProxyDirectory(path))
            return false;

        lstrcatA(path, "HatVR.dll");
        g_hatVR = LoadLibraryA(path);

        if (!g_hatVR)
        {
            ProxyLog("Vulkan proxy: LoadLibraryA(HatVR.dll) FAILED\n");
            return false;
        }

        ProxyLog("Vulkan proxy: HatVR.dll LOADED\n");
        return true;
    }

    bool EnsureRealVulkan()
    {
        if (g_realVulkan)
            return true;

        char systemDir[MAX_PATH] = {};
        if (!GetSystemDirectoryA(systemDir, MAX_PATH))
            return false;

        char path[MAX_PATH] = {};
        lstrcpyA(path, systemDir);
        lstrcatA(path, "\\vulkan-1.dll");

        g_realVulkan = LoadLibraryA(path);
        if (!g_realVulkan)
        {
            ProxyLog("Vulkan proxy: real system vulkan-1.dll FAILED to load\n");
            return false;
        }

        ProxyLog("Vulkan proxy: real system vulkan-1.dll LOADED\n");
        EnsureHatVR();
        return true;
    }

    FARPROC ResolveVulkanExport(const char* name)
    {
        if (!EnsureRealVulkan())
            return nullptr;

        return GetProcAddress(g_realVulkan, name);
    }
}

// Minimal Vulkan ABI declarations.  These are deliberately opaque so this
// loader does not require Vulkan SDK headers and cannot accidentally own
// renderer state.  The future HatVR Vulkan backend will do that.
using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkDevice = void*;
using VkQueue = void*;
using VkResult = int;
using VkBool32 = unsigned int;
using VkFlags = unsigned int;
using PFN_vkVoidFunction = void (WINAPI*)();

struct VkAllocationCallbacks;
struct VkInstanceCreateInfo;
struct VkDeviceCreateInfo;
struct VkSubmitInfo;
struct VkPresentInfoKHR;
struct VkExtensionProperties;
struct VkLayerProperties;

#define FORWARD_VK(ret, name, params, args)                 \
extern "C" __declspec(dllexport) ret WINAPI name params     \
{                                                           \
    using Fn = ret (WINAPI*) params;                        \
    auto fn = reinterpret_cast<Fn>(ResolveVulkanExport(#name)); \
    if (!fn)                                                \
        return ret();                                       \
    return fn args;                                         \
}

#define FORWARD_VK_VOID(name, params, args)                 \
extern "C" __declspec(dllexport) void WINAPI name params    \
{                                                           \
    using Fn = void (WINAPI*) params;                       \
    auto fn = reinterpret_cast<Fn>(ResolveVulkanExport(#name)); \
    if (fn)                                                 \
        fn args;                                            \
}

extern "C" __declspec(dllexport) PFN_vkVoidFunction WINAPI
vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
    using Fn = PFN_vkVoidFunction (WINAPI*)(VkInstance, const char*);
    auto fn = reinterpret_cast<Fn>(ResolveVulkanExport("vkGetInstanceProcAddr"));
    return fn ? fn(instance, name) : nullptr;
}

extern "C" __declspec(dllexport) PFN_vkVoidFunction WINAPI
vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    using Fn = PFN_vkVoidFunction (WINAPI*)(VkDevice, const char*);
    auto fn = reinterpret_cast<Fn>(ResolveVulkanExport("vkGetDeviceProcAddr"));
    return fn ? fn(device, name) : nullptr;
}

FORWARD_VK(VkResult, vkCreateInstance,
    (const VkInstanceCreateInfo* createInfo, const VkAllocationCallbacks* allocator, VkInstance* instance),
    (createInfo, allocator, instance))

FORWARD_VK(VkResult, vkCreateDevice,
    (VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo,
     const VkAllocationCallbacks* allocator, VkDevice* device),
    (physicalDevice, createInfo, allocator, device))

FORWARD_VK_VOID(vkDestroyInstance,
    (VkInstance instance, const VkAllocationCallbacks* allocator),
    (instance, allocator))

FORWARD_VK_VOID(vkDestroyDevice,
    (VkDevice device, const VkAllocationCallbacks* allocator),
    (device, allocator))

FORWARD_VK(VkResult, vkQueuePresentKHR,
    (VkQueue queue, const VkPresentInfoKHR* presentInfo),
    (queue, presentInfo))

FORWARD_VK(VkResult, vkQueueSubmit,
    (VkQueue queue, unsigned int submitCount, const VkSubmitInfo* submits, void* fence),
    (queue, submitCount, submits, fence))

FORWARD_VK(VkResult, vkEnumerateInstanceExtensionProperties,
    (const char* layerName, unsigned int* propertyCount, VkExtensionProperties* properties),
    (layerName, propertyCount, properties))

FORWARD_VK(VkResult, vkEnumerateInstanceLayerProperties,
    (unsigned int* propertyCount, VkLayerProperties* properties),
    (propertyCount, properties))

#undef FORWARD_VK
#undef FORWARD_VK_VOID
