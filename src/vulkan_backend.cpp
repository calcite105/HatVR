#include "vulkan_backend.h"
#include "graphics_backend.h"
#include "dxvk_trace.h"
#include "vulkan_intercept.h"
#include "logger.h"
#include "resources/resource.h"

#include <windows.h>
#include <wincodec.h>
#include <openxr/openxr.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include "NIS_Config.h"
#include "nis_scaler_spv.h"
#include "fsr_easu_spv.h"
#include "fsr_rcas_spv.h"
#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")

// Vulkan is an intentional build requirement for the experimental DXVK path.
// if this include fails, stop the build: do not emit another DLL whose Vulkan
// backend silently compiles out.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#define HATVR_HAS_VULKAN_HEADERS 1

namespace ahitvr {
    extern bool g_recommendedGraphicsMenuOpen;
    // Release-safe master gate for retained development/debug tools. The tools
    bool g_debugToolsEnabled = false;
    bool g_sharperNativeStereo = false;
    bool g_spectatorExpandedFov = false;
    // User-facing HatVR menu font choice. Curse Casual is the default; the
    // ordinary Dear ImGui font remains available as a compatibility/fallback option.
    bool g_hatVrCurseCasualMenuFont = true;
    // Workshop-facing API is opt-in so ordinary HatVR use performs no API work.
    bool g_hatVrModApiEnabled = false;
    float g_cameraLocalForward = 0.0f;
    float g_cameraLocalRight = 0.0f;
    float g_cameraLocalUp = 0.0f;
}

MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTextureHatVR : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(
        VkImage* image, VkImageLayout* layout, VkImageCreateInfo* info) = 0;
};

MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevicePhase5 : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t*, uint32_t*) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTextureHatVR*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9*, DWORD) = 0;
};

namespace ahitvr
{
    // Shared PSVR2 HMD-rumble settings.  These have external linkage because
    // controller_input/config live in the D3D9 translation unit while this
    // menu lives in the Vulkan translation unit.
    bool g_psvr2HmdRumbleEnabled = false;
    bool g_psvr2StrongPhysicalOnly = true; // legacy
    bool g_psvr2EyeWheelEnabled = true;
    bool g_psvr2MenuOpen = false;
    int  g_psvr2HmdRumbleIntensity = 100;
    int  g_psvr2HmdRumbleCurve = 0;
    bool g_psvr2HookshotAdaptiveTrigger = true;
    bool g_openXrUpscalingEnabled = false;
    int  g_openXrUpscaler = 0; // 0=NIS, 1=FSR 1 (spatial)
    int  g_openXrUpscalePercent = 50;
    int  g_openXrUpscaleSharpness = 50;

    namespace
    {
        XrInstance g_vkXrInstance = XR_NULL_HANDLE;
        XrSystemId g_vkXrSystemId = XR_NULL_SYSTEM_ID;
        XrSession g_vkXrSession = XR_NULL_HANDLE;
        XrSpace g_vkLocalSpace = XR_NULL_HANDLE;
        XrSpace g_vkViewSpace = XR_NULL_HANDLE;
        XrSessionState g_vkSessionState = XR_SESSION_STATE_UNKNOWN;
        bool g_vkSessionRunning = false;
        bool g_vkExitRequested = false;
        bool g_vkFrameBegun = false;
        bool g_vkFrameShouldRender = false;
        XrTime g_vkPredictedDisplayTime = 0;
        XrView g_vkViews[2] = {
            { XR_TYPE_VIEW }, { XR_TYPE_VIEW }
        };

        VkInstance g_vkInstance = VK_NULL_HANDLE;
        VkPhysicalDevice g_vkPhysicalDevice = VK_NULL_HANDLE;
        VkDevice g_vkDevice = VK_NULL_HANDLE;
        VkQueue g_vkQueue = VK_NULL_HANDLE;
        uint32_t g_vkQueueFamily = UINT32_MAX;
        uint32_t g_vkQueueIndex = 0;
        bool g_vkOwnsInstance = false;
        bool g_vkOwnsDevice = false;
        bool g_vkUsingDxvkDevice = false;

        VkInstance g_dxvkProbeInstance = VK_NULL_HANDLE;
        VkPhysicalDevice g_dxvkProbePhysical = VK_NULL_HANDLE;
        VkDevice g_dxvkProbeDevice = VK_NULL_HANDLE;
        VkQueue g_dxvkProbeQueue = VK_NULL_HANDLE;
        uint32_t g_dxvkProbeQueueFamily = UINT32_MAX;
        uint32_t g_dxvkProbeQueueIndex = UINT32_MAX;

        XrSwapchain g_vkEyeSwapchains[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
        std::vector<XrSwapchainImageVulkan2KHR> g_vkSwapchainImages[2];
        int64_t g_vkSwapchainFormat = 0;
        uint32_t g_vkSwapchainWidth = 0;
        uint32_t g_vkSwapchainHeight = 0;
        uint32_t g_vkRecommendedEyeWidth = 0;
        uint32_t g_vkRecommendedEyeHeight = 0;
        uint32_t g_vkMaxEyeWidth = 0;
        uint32_t g_vkMaxEyeHeight = 0;
        uint32_t g_vkLastSourceEyeWidth = 0;
        uint32_t g_vkLastSourceEyeHeight = 0;
        int g_vkActiveUpscalePercent = -1; // currently applied OpenXR output scale; may change live
        char g_vkRuntimeName[XR_MAX_RUNTIME_NAME_SIZE] = "Unknown";
        char g_vkSystemName[XR_MAX_SYSTEM_NAME_SIZE] = "Unknown";
        uint32_t g_vkRuntimeMajor=0, g_vkRuntimeMinor=0, g_vkRuntimePatch=0;
        float g_vkRefreshHz = 0.0f;

        XrSwapchain g_vkUiSwapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkan2KHR> g_vkUiSwapchainImages;
        uint32_t g_vkUiWidth = 0;
        uint32_t g_vkUiHeight = 0;

        // Dear ImGui is used only as a GPU 2D command generator/renderer.
        // HatVR keeps its own controller navigation and never uses a platform backend.
        VkRenderPass g_vkMenuRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> g_vkMenuFramebuffers;
        std::vector<VkImageView> g_vkMenuImageViews;
        VkCommandPool g_vkMenuGuiCommandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> g_vkMenuGuiCommandBuffers;
        std::vector<VkFence> g_vkMenuGuiFences;
        bool g_vkMenuImGuiReady = false;
        ImFont* g_vkMenuFont = nullptr; // currently selected/default menu font
        ImFont* g_vkMenuDefaultFont = nullptr;
        ImFont* g_vkMenuCurseCasualFont = nullptr;

        // HatVR's settings menu has its own compositor layer and swapchain.
        // It deliberately does not share AHiT's UI texture/swapchain.
        XrSwapchain g_vkMenuSwapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkan2KHR> g_vkMenuSwapchainImages;
        uint32_t g_vkMenuWidth = 0;
        uint32_t g_vkMenuHeight = 0;
        // render texture.  The old UI path reused leftEye as a 1344x1440 staging
        // texture, which now overwrites/crops the native SBS render target.
        // keep a dedicated eye-sized D3D9 texture for the UI instead.
        IDirect3DTexture9* g_phase81UiStagingTexture = nullptr;
        IDirect3DDevice9* g_phase81UiStagingDevice = nullptr;
        uint32_t g_phase81UiStagingWidth = 0;
        uint32_t g_phase81UiStagingHeight = 0;

        VkCommandPool g_vkCommandPool = VK_NULL_HANDLE;
        VkCommandBuffer g_vkCommandBuffer = VK_NULL_HANDLE;
        VkFence g_vkFence = VK_NULL_HANDLE;

        struct VkUploadCache
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            VkDeviceSize capacity = 0;
        };
        VkUploadCache g_vkUploadCache[3] = {};

        struct D3D9ReadbackCache
        {
            IDirect3DSurface9* surface = nullptr;
            UINT width = 0, height = 0;
            D3DFORMAT format = D3DFMT_UNKNOWN;
        };
        D3D9ReadbackCache g_vkReadbackCache[2] = {};
        std::vector<unsigned char> g_vkCpuPixels[3];

        void DestroyTransferCaches()
        {
            for (auto& c : g_vkUploadCache)
            {
                if (c.mapped && c.memory) vkUnmapMemory(g_vkDevice, c.memory);
                if (c.buffer) vkDestroyBuffer(g_vkDevice, c.buffer, nullptr);
                if (c.memory) vkFreeMemory(g_vkDevice, c.memory, nullptr);
                c = {};
            }
            for (auto& c : g_vkReadbackCache)
            {
                if (c.surface) c.surface->Release();
                c = {};
            }
            for (auto& v : g_vkCpuPixels) { std::vector<unsigned char>().swap(v); }
        }

        PFN_vkDestroyInstance g_vkDestroyInstance = nullptr;
        PFN_vkDestroyDevice g_vkDestroyDevice = nullptr;

        unsigned long long g_vkBeginSerial = 0;
        unsigned long long g_vkSubmitSerial = 0;

        bool HasExtension(const std::vector<XrExtensionProperties>& exts, const char* name)
        {
            return std::any_of(exts.begin(), exts.end(),
                [name](const XrExtensionProperties& e)
                {
                    return std::strcmp(e.extensionName, name) == 0;
                });
        }

        void LogXrFailure(const char* where, XrResult xr)
        {
            DxvkPathTrace("VKXR-V8 %s FAILED xr=%d", where, (int)xr);
        }

        uint32_t PickGraphicsQueueFamily(VkPhysicalDevice physicalDevice)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
            if (!count) return UINT32_MAX;
            std::vector<VkQueueFamilyProperties> props(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, props.data());
            for (uint32_t i = 0; i < count; ++i)
                if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && props[i].queueCount)
                    return i;
            return UINT32_MAX;
        }

        uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags wanted)
        {
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(g_vkPhysicalDevice, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((bits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & wanted) == wanted)
                    return i;
            return UINT32_MAX;
        }

        void DestroyEyeSwapchains()
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                g_vkSwapchainImages[eye].clear();
                if (g_vkEyeSwapchains[eye] != XR_NULL_HANDLE)
                {
                    xrDestroySwapchain(g_vkEyeSwapchains[eye]);
                    g_vkEyeSwapchains[eye] = XR_NULL_HANDLE;
                }
            }
            g_vkSwapchainFormat = 0;
            g_vkSwapchainWidth = g_vkSwapchainHeight = 0;
        }

        bool EnsureCommandResources()
        {
            if (g_vkCommandPool && g_vkCommandBuffer && g_vkFence)
                return true;

            VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = g_vkQueueFamily;
            if (vkCreateCommandPool(g_vkDevice, &pci, nullptr, &g_vkCommandPool) != VK_SUCCESS)
                return false;

            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = g_vkCommandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(g_vkDevice, &cai, &g_vkCommandBuffer) != VK_SUCCESS)
                return false;

            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(g_vkDevice, &fci, nullptr, &g_vkFence) != VK_SUCCESS)
                return false;
            return true;
        }

        bool EnsureEyeSwapchains(uint32_t width, uint32_t height)
        {
            if (g_vkEyeSwapchains[0] && g_vkEyeSwapchains[1] &&
                g_vkSwapchainWidth == width && g_vkSwapchainHeight == height)
                return true;

            DestroyEyeSwapchains();

            uint32_t formatCount = 0;
            XrResult xr = xrEnumerateSwapchainFormats(
                g_vkXrSession, 0, &formatCount, nullptr);
            if (XR_FAILED(xr) || !formatCount)
            {
                LogXrFailure("xrEnumerateSwapchainFormats(count)", xr);
                return false;
            }

            std::vector<int64_t> formats(formatCount);
            xr = xrEnumerateSwapchainFormats(
                g_vkXrSession, formatCount, &formatCount, formats.data());
            if (XR_FAILED(xr))
            {
                LogXrFailure("xrEnumerateSwapchainFormats(list)", xr);
                return false;
            }

            const int64_t preferred[] = {
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_FORMAT_R8G8B8A8_SRGB,
                VK_FORMAT_R8G8B8A8_UNORM
            };
            for (int64_t f : preferred)
            {
                if (std::find(formats.begin(), formats.end(), f) != formats.end())
                {
                    g_vkSwapchainFormat = f;
                    break;
                }
            }
            if (!g_vkSwapchainFormat)
            {
                DxvkPathTrace("VKXR-V8 no supported BGRA/RGBA swapchain format");
                return false;
            }

            for (int eye = 0; eye < 2; ++eye)
            {
                XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
                ci.usageFlags =
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
                ci.format = g_vkSwapchainFormat;
                ci.sampleCount = 1;
                ci.width = width;
                ci.height = height;
                ci.faceCount = 1;
                ci.arraySize = 1;
                ci.mipCount = 1;

                xr = xrCreateSwapchain(
                    g_vkXrSession, &ci, &g_vkEyeSwapchains[eye]);
                if (XR_FAILED(xr))
                {
                    LogXrFailure("xrCreateSwapchain(Vulkan)", xr);
                    DestroyEyeSwapchains();
                    return false;
                }

                uint32_t imageCount = 0;
                xr = xrEnumerateSwapchainImages(
                    g_vkEyeSwapchains[eye], 0, &imageCount, nullptr);
                if (XR_FAILED(xr) || !imageCount)
                {
                    LogXrFailure("xrEnumerateSwapchainImages(count)", xr);
                    DestroyEyeSwapchains();
                    return false;
                }

                g_vkSwapchainImages[eye].resize(
                    imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
                xr = xrEnumerateSwapchainImages(
                    g_vkEyeSwapchains[eye], imageCount, &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        g_vkSwapchainImages[eye].data()));
                if (XR_FAILED(xr))
                {
                    LogXrFailure("xrEnumerateSwapchainImages(list)", xr);
                    DestroyEyeSwapchains();
                    return false;
                }
            }

            g_vkSwapchainWidth = width;
            g_vkSwapchainHeight = height;
            DxvkPathTrace(
                "VKXR-V8 SWAPCHAINS READY size=%ux%u format=%lld images=%u,%u",
                width, height, (long long)g_vkSwapchainFormat,
                (unsigned)g_vkSwapchainImages[0].size(),
                (unsigned)g_vkSwapchainImages[1].size());
            return true;
        }

        void DestroyUiSwapchain()
        {
            g_vkUiSwapchainImages.clear();
            if (g_vkUiSwapchain != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(g_vkUiSwapchain);
                g_vkUiSwapchain = XR_NULL_HANDLE;
            }
            g_vkUiWidth = 0;
            g_vkUiHeight = 0;
        }

        bool EnsureUiSwapchain(uint32_t width, uint32_t height)
        {
            if (g_vkUiSwapchain != XR_NULL_HANDLE &&
                g_vkUiWidth == width && g_vkUiHeight == height)
                return true;

            DestroyUiSwapchain();
            if (!width || !height || !g_vkSwapchainFormat) return false;

            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ci.format = g_vkSwapchainFormat;
            ci.sampleCount = 1;
            ci.width = width; ci.height = height;
            ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;

            XrResult xr = xrCreateSwapchain(g_vkXrSession, &ci, &g_vkUiSwapchain);
            if (XR_FAILED(xr))
            {
                LogXrFailure("xrCreateSwapchain(Vulkan UI)", xr);
                DestroyUiSwapchain();
                return false;
            }

            uint32_t count = 0;
            xr = xrEnumerateSwapchainImages(g_vkUiSwapchain, 0, &count, nullptr);
            if (XR_FAILED(xr) || !count)
            {
                LogXrFailure("xrEnumerateSwapchainImages(Vulkan UI count)", xr);
                DestroyUiSwapchain();
                return false;
            }

            g_vkUiSwapchainImages.resize(count,
                { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
            xr = xrEnumerateSwapchainImages(g_vkUiSwapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(
                    g_vkUiSwapchainImages.data()));
            if (XR_FAILED(xr))
            {
                LogXrFailure("xrEnumerateSwapchainImages(Vulkan UI list)", xr);
                DestroyUiSwapchain();
                return false;
            }
            g_vkUiWidth = width; g_vkUiHeight = height;
            DxvkPathTrace("VKXR-V8 UI SWAPCHAIN READY size=%ux%u images=%u",
                width, height, count);
            return true;
        }

        void DestroyMenuGpuRenderer()
        {
            if (!g_vkDevice) return;
            if (g_vkMenuImGuiReady)
            {
                ImGui_ImplVulkan_Shutdown();
                if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
                g_vkMenuImGuiReady=false;
                g_vkMenuFont=nullptr;
                g_vkMenuDefaultFont=nullptr;
                g_vkMenuCurseCasualFont=nullptr;
            }
            for(auto f:g_vkMenuGuiFences) if(f) vkDestroyFence(g_vkDevice,f,nullptr);
            g_vkMenuGuiFences.clear();
            if(g_vkMenuGuiCommandPool) vkDestroyCommandPool(g_vkDevice,g_vkMenuGuiCommandPool,nullptr);
            g_vkMenuGuiCommandPool=VK_NULL_HANDLE;
            g_vkMenuGuiCommandBuffers.clear();
            for(auto fb:g_vkMenuFramebuffers) if(fb) vkDestroyFramebuffer(g_vkDevice,fb,nullptr);
            g_vkMenuFramebuffers.clear();
            for(auto v:g_vkMenuImageViews) if(v) vkDestroyImageView(g_vkDevice,v,nullptr);
            g_vkMenuImageViews.clear();
            if(g_vkMenuRenderPass) vkDestroyRenderPass(g_vkDevice,g_vkMenuRenderPass,nullptr);
            g_vkMenuRenderPass=VK_NULL_HANDLE;
        }

        static void HatVrImGuiAddOutlined(ImDrawList* dl, ImFont* font, float size,
            ImVec2 p, ImU32 color, const char* text, float outline=3.0f)
        {
            const ImU32 edge=IM_COL32(23,19,28,255);
            const ImVec2 offsets[8]={{-outline,0},{outline,0},{0,-outline},{0,outline},
                {-outline,-outline},{outline,-outline},{-outline,outline},{outline,outline}};
            for(const auto& o:offsets) dl->AddText(font,size,{p.x+o.x,p.y+o.y},edge,text);
            dl->AddText(font,size,p,color,text);
        }

        static void HatVrImGuiAddCentered(ImDrawList* dl, ImFont* font, float size,
            float x, float y, float w, ImU32 color, const char* text, float outline=3.0f)
        {
            ImVec2 sz=font->CalcTextSizeA(size,FLT_MAX,0.0f,text);
            HatVrImGuiAddOutlined(dl,font,size,{x+(w-sz.x)*0.5f,y},color,text,outline);
        }

        static void HatVrImGuiAddRight(ImDrawList* dl, ImFont* font, float size,
            float x, float y, float w, ImU32 color, const char* text, float outline=3.0f)
        {
            ImVec2 sz=font->CalcTextSizeA(size,FLT_MAX,0.0f,text);
            HatVrImGuiAddOutlined(dl,font,size,{x+w-sz.x,y},color,text,outline);
        }

        static void HatVrImGuiAddWrappedCentered(ImDrawList* dl, ImFont* font, float size,
            float x,float y,float w,float lineHeight,ImU32 color,const std::string& text,float outline=3.0f)
        {
            std::string line, word;
            auto flush=[&](){
                if(!line.empty()){HatVrImGuiAddCentered(dl,font,size,x,y,w,color,line.c_str(),outline);y+=lineHeight;line.clear();}
            };
            for(size_t i=0;i<=text.size();++i)
            {
                const char c=(i<text.size()?text[i]:' ');
                if(c==' '||c=='\n')
                {
                    if(!word.empty())
                    {
                        std::string candidate=line.empty()?word:(line+" "+word);
                        if(!line.empty() && font->CalcTextSizeA(size,FLT_MAX,0.0f,candidate.c_str()).x>w) flush();
                        line=line.empty()?word:(line+" "+word);
                        word.clear();
                    }
                    if(c=='\n') flush();
                }
                else word.push_back(c);
            }
            flush();
        }

        bool EnsureMenuGpuRenderer()
        {
            if(g_vkMenuImGuiReady) return true;
            if(!g_vkDevice || !g_vkPhysicalDevice || !g_vkQueue || g_vkMenuSwapchainImages.empty()) return false;

            VkAttachmentDescription ad{};
            ad.format=(VkFormat)g_vkSwapchainFormat;
            ad.samples=VK_SAMPLE_COUNT_1_BIT;
            ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
            ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            ad.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ad.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sp{};
            sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
            sp.colorAttachmentCount=1; sp.pColorAttachments=&ar;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rp.attachmentCount=1; rp.pAttachments=&ad; rp.subpassCount=1; rp.pSubpasses=&sp;
            if(vkCreateRenderPass(g_vkDevice,&rp,nullptr,&g_vkMenuRenderPass)!=VK_SUCCESS) return false;

            g_vkMenuFramebuffers.resize(g_vkMenuSwapchainImages.size());
            g_vkMenuImageViews.resize(g_vkMenuSwapchainImages.size());
            for(size_t i=0;i<g_vkMenuSwapchainImages.size();++i)
            {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image=g_vkMenuSwapchainImages[i].image;
                vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=(VkFormat)g_vkSwapchainFormat;
                vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
                vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
                VkImageView view=VK_NULL_HANDLE;
                if(vkCreateImageView(g_vkDevice,&vi,nullptr,&view)!=VK_SUCCESS) return false;
                g_vkMenuImageViews[i]=view;
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass=g_vkMenuRenderPass; fi.attachmentCount=1; fi.pAttachments=&g_vkMenuImageViews[i];
                fi.width=g_vkMenuWidth; fi.height=g_vkMenuHeight; fi.layers=1;
                if(vkCreateFramebuffer(g_vkDevice,&fi,nullptr,&g_vkMenuFramebuffers[i])!=VK_SUCCESS) return false;
            }

            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex=g_vkQueueFamily;
            if(vkCreateCommandPool(g_vkDevice,&pci,nullptr,&g_vkMenuGuiCommandPool)!=VK_SUCCESS) return false;
            g_vkMenuGuiCommandBuffers.resize(g_vkMenuSwapchainImages.size());
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool=g_vkMenuGuiCommandPool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount=(uint32_t)g_vkMenuGuiCommandBuffers.size();
            if(vkAllocateCommandBuffers(g_vkDevice,&cai,g_vkMenuGuiCommandBuffers.data())!=VK_SUCCESS) return false;
            g_vkMenuGuiFences.resize(g_vkMenuSwapchainImages.size());
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            for(auto& f:g_vkMenuGuiFences) if(vkCreateFence(g_vkDevice,&fci,nullptr,&f)!=VK_SUCCESS) return false;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io=ImGui::GetIO();
            io.DisplaySize=ImVec2((float)g_vkMenuWidth,(float)g_vkMenuHeight);
            io.DeltaTime=1.0f/90.0f;
            io.IniFilename=nullptr; io.LogFilename=nullptr;

            // keep both fonts in the same ImGui atlas so switching the user-facing
            // menu font is instant and does not rebuild Vulkan resources.
            g_vkMenuDefaultFont=io.Fonts->AddFontDefault();

            HMODULE fontModule=nullptr;
            if(GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&EnsureMenuGpuRenderer),&fontModule) && fontModule)
            {
                HRSRC fontResource=FindResourceA(fontModule,MAKEINTRESOURCEA(IDR_CURSE_CASUAL),RT_RCDATA);
                HGLOBAL fontLoaded=fontResource?LoadResource(fontModule,fontResource):nullptr;
                void* fontData=fontLoaded?LockResource(fontLoaded):nullptr;
                const DWORD fontBytes=fontResource?SizeofResource(fontModule,fontResource):0;
                if(fontData && fontBytes)
                {
                    ImFontConfig fontCfg{};
                    fontCfg.FontDataOwnedByAtlas=false;
                    g_vkMenuCurseCasualFont=io.Fonts->AddFontFromMemoryTTF(
                        fontData,(int)fontBytes,18.0f,&fontCfg);
                }
            }

            if(!g_vkMenuCurseCasualFont)
                DxvkPathTrace("HATVR-MENU Curse Casual ImGui font unavailable; using default font");

            g_vkMenuFont=(g_hatVrCurseCasualMenuFont && g_vkMenuCurseCasualFont)
                ? g_vkMenuCurseCasualFont : g_vkMenuDefaultFont;
            io.FontDefault=g_vkMenuFont;

            ImGui_ImplVulkan_InitInfo ii{};
            ii.ApiVersion=VK_API_VERSION_1_1;
            ii.Instance=g_vkInstance; ii.PhysicalDevice=g_vkPhysicalDevice; ii.Device=g_vkDevice;
            ii.QueueFamily=g_vkQueueFamily; ii.Queue=g_vkQueue;
            ii.DescriptorPoolSize=64;
            ii.MinImageCount=2;
            ii.ImageCount=(uint32_t)g_vkMenuSwapchainImages.size();
            ii.PipelineInfoMain.RenderPass=g_vkMenuRenderPass;
            ii.PipelineInfoMain.Subpass=0;
            ii.PipelineInfoMain.MSAASamples=VK_SAMPLE_COUNT_1_BIT;
            if(!ImGui_ImplVulkan_Init(&ii)) return false;
            g_vkMenuImGuiReady=true;
            DxvkPathTrace("HATVR-MENU ImGui Vulkan renderer ready images=%u",(unsigned)g_vkMenuSwapchainImages.size());
            return true;
        }

        static const char* HatVrMenuPageName(int page)
        {
            static const char* names[] = { "Home", "VR", "Controls", "UI / HUD", "Spectator", "Graphics", "Debug Tools" };
            return (page >= 0 && page < 7) ? names[page] : "Home";
        }

        void BuildHatVrImGuiMenu(bool nativeStereo,bool theaterMode,bool firstPersonEnabled,
            bool autoTheaterCutscenes,bool overrideLockedCameras,bool disablePlayerFade,
            bool rightHandHookshot,bool umbrellaMotionControls,bool playStationIcons,bool nintendoSwitchIcons,float hudScale,float hudDistance,
            float hudHeight,bool hudHeadLocked,int spectatorView,int spectatorUiMode,int page,int selection,bool insideCategory,
            int uiDebugCandidateIndex,unsigned int uiDebugCandidateCount,unsigned long long uiDebugCandidateHash,
            unsigned long long uiDebugCandidateHits,int uiDebugPreviewMode,int uiDebugPermanentRoute)
        {
            constexpr float W=900.0f,H=760.0f;
            ImGuiIO& io=ImGui::GetIO(); io.DisplaySize={W,H}; io.DeltaTime=1.0f/90.0f;
            g_vkMenuFont=(g_hatVrCurseCasualMenuFont && g_vkMenuCurseCasualFont)
                ? g_vkMenuCurseCasualFont : g_vkMenuDefaultFont;
            if(g_vkMenuFont) io.FontDefault=g_vkMenuFont;
            ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame();

            // Intentionally close to stock Dear ImGui. The point of this menu is
            // that future settings can be appended without bespoke layout work.
            ImGui::StyleColorsDark();
            ImGuiStyle& style=ImGui::GetStyle();
            style.WindowRounding=0.0f; style.ChildRounding=0.0f; style.FrameRounding=2.0f;
            style.WindowPadding={12,12}; style.ItemSpacing={8,9};
            ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({W,H});
            ImGui::Begin("HatVR Settings",nullptr,ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);

            const float sidebarW=150.0f, descH=142.0f;
            ImGui::BeginChild("Sidebar",{sidebarW,0},true);
            for(int i=0;i<(g_debugToolsEnabled?7:6);++i)
            {
                const bool selectedPage=(i==page);
                if(selectedPage && !insideCategory) ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                ImGui::Selectable(HatVrMenuPageName(i),selectedPage);
                if(selectedPage && !insideCategory) ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::SameLine();

            ImGui::BeginGroup();
            ImGui::BeginChild("Content",{0,-descH},true,ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGui::TextUnformatted(HatVrMenuPageName(page)); ImGui::Separator(); ImGui::Spacing();
            const char* descTitle=HatVrMenuPageName(page);
            const char* desc="";
            int row=0;
            auto selected=[&](int r){return insideCategory && selection==r;};
            auto beginSelectedRow=[&](int r)
            {
                if(!selected(r)) return false;
                ImVec2 p=ImGui::GetCursorScreenPos();
                const float h=ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    {p.x-4.0f,p.y-2.0f},
                    {p.x+ImGui::GetContentRegionAvail().x+4.0f,p.y+h+2.0f},
                    ImGui::GetColorU32(ImGuiCol_Header));
                return true;
            };
            auto mark=[&](int r,const char* title,const char* body){if(selected(r)){descTitle=title;desc=body;ImGui::SetScrollHereY(0.35f);}};
            auto check=[&](const char* label,bool value,int r,const char* body){beginSelectedRow(r); bool v=value; ImGui::Checkbox(label,&v); mark(r,label,body); ++row;};

            if(page==0)
            {
                ImGui::TextUnformatted("HatVR");
                ImGui::TextDisabled("Release Candidate B - 0.1");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::Text("Headset: %s",g_vkSystemName);
                ImGui::Text("OpenXR Runtime: %s %u.%u.%u",g_vkRuntimeName,g_vkRuntimeMajor,g_vkRuntimeMinor,g_vkRuntimePatch);
                if(g_vkRecommendedEyeWidth && g_vkRecommendedEyeHeight)
                    ImGui::Text("Recommended Resolution: %u x %u per eye",g_vkRecommendedEyeWidth,g_vkRecommendedEyeHeight);
                if(g_vkRefreshHz>1.0f) ImGui::Text("Refresh Rate: %.1f Hz",g_vkRefreshHz);
                ImGui::Text("Graphics Path: D3D9 -> DXVK/Vulkan -> OpenXR");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                beginSelectedRow(0); ImGui::Button("Recenter View",{170,0});
                mark(0,"Recenter View","Makes your current headset position and facing direction the new VR center. Pitch and roll remain gravity-aligned.");
                if(!insideCategory){ descTitle="Home"; desc="Press A to enter this category. Press B to close the HatVR menu."; }
            }
            else if(page==1)
            {
                const char* modes[]={"Synchronized Sequential","Stereo"}; int mode=nativeStereo?1:0;
                beginSelectedRow(0); ImGui::Combo("Rendering Mode",&mode,modes,2);
                mark(0,"Rendering Mode",nativeStereo?
                    "Renders both eyes together using a method similar to A Hat in Time's split-screen renderer. Recommended for normal VR gameplay and generally provides better performance, though some effects may not render correctly in both eyes.":
                    "Renders each eye separately in sequence from the same game state. May improve compatibility with effects that do not work correctly with Stereo, but requires more rendering work and may reduce performance.");
                check("Sharper Native Stereo",g_sharperNativeStereo,1,nativeStereo ?
                    "Uses the full OpenXR eye projection for a sharper image at the same render resolution instead of rendering a wider symmetric view and cropping it afterward. Some screen-space effects, especially Ambient Occlusion, may render incorrectly with this projection and should be disabled manually if needed." :
                    "Saved for Native Stereo. This option has no effect while Synchronized Sequential is selected. In Native Stereo it uses the full OpenXR eye projection for a sharper image. Some screen-space effects, especially Ambient Occlusion, may need to be disabled manually.");
                check("First Person (Experimental)",firstPersonEnabled,2,"Moves the gameplay camera to Hat Kid and enables the VR avatar. Third Person is the most stable way to play HatVR; First Person is experimental and some player models, animations, interactions, or camera sequences may not behave correctly.");
                check("Mod API",g_hatVrModApiEnabled,3,"Allows compatible A Hat in Time mods to access basic HatVR state and requested tracking data. Disabled by default. Tracking data is only intended to be delivered when a mod asks for it.");
                check("Disable Player Fade",disablePlayerFade,4,"Disables A Hat in Time's effect that fades the player when they get too close to the camera. HatVR stops overriding the fade while a cutscene camera is active. This workaround can still cause incorrect shadows on some nearby objects.");
                check("Theater Mode",theaterMode,5,"Displays the game on a fixed stereoscopic screen instead of using the game's camera directly in VR. Useful for cutscenes or sections where direct VR camera movement is uncomfortable.");
                check("Automatically Use Theater Mode for Cutscenes",autoTheaterCutscenes,6,"Automatically switches to Theater Mode when HatVR detects that A Hat in Time has taken control of the camera, then returns to immersive VR afterward.");
                check("Override Locked Cameras",overrideLockedCameras,7,"Overrides certain forced camera modes so they do not take control of the VR camera. This may not work with every locked camera in the game. If a camera remains locked, temporarily switching to Third Person may be necessary.");
                check("Upscaling",g_openXrUpscalingEnabled,8,"Upscales HatVR's completed eye image before it is submitted to OpenXR. A Hat in Time keeps rendering at its normal resolution. This is spatial image scaling, not temporal or AI reconstruction such as DLSS or FSR 2, so it cannot recreate the detail of rendering the game natively at the higher resolution.");
                const char* upscalers[]={"NIS","FSR 1"};
                beginSelectedRow(9); ImGui::Combo("Upscaler",&g_openXrUpscaler,upscalers,2); mark(9,"Upscaler",g_openXrUpscaler==1 ? "AMD FidelityFX Super Resolution 1 uses EASU spatial upscaling and optional RCAS sharpening. It uses only the current frame; this is not the temporal reconstruction used by FSR 2 or newer versions." : "NIS (NVIDIA Image Scaling) is a current-frame spatial upscaler. It does not use motion vectors, frame history, or AI reconstruction and works through Vulkan on supported GPUs.");
                char outputScaleText[96]={}; _snprintf_s(outputScaleText,sizeof(outputScaleText),_TRUNCATE,"Output Upscale: +%d%%",g_openXrUpscalePercent);
                beginSelectedRow(10); ImGui::Button(outputScaleText); mark(10,"Output Upscale","Controls how much larger HatVR makes the OpenXR eye image than A Hat in Time's source image. +100% means 2x the width and 2x the height. NIS supports +25% through +100%. Changes apply live; the game's own rendering resolution is not changed.");
                char sharpText[96]={}; _snprintf_s(sharpText,sizeof(sharpText),_TRUNCATE,"Upscaler Sharpness: %d%%",g_openXrUpscaleSharpness);
                beginSelectedRow(11); ImGui::Button(sharpText); mark(11,"Upscaler Sharpness",g_openXrUpscaler==1 ? "Controls FSR 1 RCAS sharpening. At 0%, HatVR skips the RCAS pass entirely. Higher values increase sharpening after EASU. Use Left / Right to adjust." : "Controls NIS adaptive sharpening after spatial upscaling. Higher values can make fine edges clearer but may exaggerate aliasing or texture detail. Use Left / Right to adjust.");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                const uint32_t sourceEyeW = g_vkLastSourceEyeWidth;
                const uint32_t sourceEyeH = g_vkLastSourceEyeHeight;
                const int effectivePct = g_openXrUpscalingEnabled ? (std::max)(25,(std::min)(100,g_openXrUpscalePercent)) : 0;
                const uint32_t requestedW = sourceEyeW ? (uint32_t)std::llround((double)sourceEyeW*(1.0+(double)effectivePct/100.0)) : 0u;
                const uint32_t requestedH = sourceEyeH ? (uint32_t)std::llround((double)sourceEyeH*(1.0+(double)effectivePct/100.0)) : 0u;
                const uint32_t actualW = g_vkMaxEyeWidth ? (std::min)(requestedW,g_vkMaxEyeWidth) : requestedW;
                const uint32_t actualH = g_vkMaxEyeHeight ? (std::min)(requestedH,g_vkMaxEyeHeight) : requestedH;
                if(sourceEyeW&&sourceEyeH) ImGui::Text("Game Render: %u x %u per eye",sourceEyeW,sourceEyeH);
                if(actualW&&actualH) ImGui::Text("OpenXR Output: %u x %u per eye",actualW,actualH);
                if(g_vkRecommendedEyeWidth&&g_vkRecommendedEyeHeight) ImGui::Text("OpenXR Recommended: %u x %u per eye",g_vkRecommendedEyeWidth,g_vkRecommendedEyeHeight);
            }
            else if(page==2)
            {
                ImGui::TextWrapped("Most VR controller inputs correspond directly to their standard controller counterparts. Press Y / Triangle once to open A Hat in Time's normal menu, or double-tap Y / Triangle to open the HatVR menu.");
                ImGui::Separator();
                check("Right-Hand Hookshot",rightHandHookshot,0,"Uses Right Grip as a Hookshot modifier and Right Trigger as X / Square. While enabled, HatVR does not send the normal right-shoulder / R button from Right Grip, which can interfere with menus or other game actions that expect that button.");
                check("Umbrella Motion Controls",umbrellaMotionControls,1,"Swinging the right VR controller triggers the umbrella attack. Turn this off to use normal button controls without motion-triggered attacks.");
                check("PlayStation Controller Icons",playStationIcons,2,"Uses PlayStation-style controller button icons instead of Xbox-style icons. This only changes the displayed button prompts and does not change controller input.");
                check("Nintendo Switch Controller Icons",nintendoSwitchIcons,3,"Uses Nintendo Switch-style button icons without changing the controls. Due to a current HatVR UI limitation, symbols may not appear on some physical in-world button prompts. This can help if the disconnect between those prompts and Xbox symbols is distracting.");
                ImGui::Spacing(); ImGui::TextUnformatted("PSVR2"); ImGui::Separator();
                check("HMD Rumble (Experimental)",g_psvr2HmdRumbleEnabled,4,"PSVR2 only. Enables headset vibration for supported impacts and camera-shake effects. Requires PSVR2 Toolkit with its jailbreak enabled. Leave this off on other headsets; enabling it may cause gameplay stalls.");
                char intensity[64]={}; _snprintf_s(intensity,sizeof(intensity),_TRUNCATE,"HMD Rumble Intensity: %d%%",g_psvr2HmdRumbleIntensity);
                beginSelectedRow(5); ImGui::Button(intensity); mark(5,"HMD Rumble Intensity","Controls PSVR2 headset vibration strength. At 100%, HatVR leaves the source intensity unchanged. Lower values are remapped using the selected HMD Rumble Curve. Use Left / Right to adjust.");
                const char* curves[]={"Linear","Preserve Low","Reduce Low"};
                char curve[80]={}; _snprintf_s(curve,sizeof(curve),_TRUNCATE,"HMD Rumble Curve: %s",curves[(g_psvr2HmdRumbleCurve>=0&&g_psvr2HmdRumbleCurve<3)?g_psvr2HmdRumbleCurve:0]);
                beginSelectedRow(6); ImGui::Button(curve); mark(6,"HMD Rumble Curve","Controls which vibration strengths are reduced when HMD Rumble Intensity is below 100%. Linear reduces the whole range evenly. Preserve Low keeps subtle feedback stronger while compressing high values. Reduce Low suppresses small vibrations more while retaining more of strong impacts. Has no effect at 100% intensity.");
                check("Hookshot Adaptive Trigger",g_psvr2HookshotAdaptiveTrigger,7,"Enables the PSVR2 right adaptive-trigger effect while Right-Hand Hookshot is active. This only controls the trigger effect and does not disable Right-Hand Hookshot itself.");
                check("Eye-Controlled Hat Wheel",g_psvr2EyeWheelEnabled,8,"Uses PSVR2 eye tracking to select directions in the Hat Wheel while it is open. Physical right-stick input takes priority when used. Requires PSVR2 Toolkit.");
            }
            else if(page==3)
            {
                float a=hudScale,b=hudDistance,c=hudHeight;
                beginSelectedRow(0); ImGui::SliderFloat("HUD Size",&a,0.50f,2.00f,"%.2fx"); mark(0,"HUD Size","Changes the size of HatVR's OpenXR UI.");
                beginSelectedRow(1); ImGui::SliderFloat("HUD Distance",&b,0.25f,2.00f,"%.2f m"); mark(1,"HUD Distance","Changes how far HatVR's OpenXR UI appears from you.");
                beginSelectedRow(2); ImGui::SliderFloat("HUD Height",&c,-1.00f,1.00f,"%.2f m"); mark(2,"HUD Height","Moves HatVR's OpenXR UI higher or lower in your view.");
                beginSelectedRow(3); ImGui::Button("Reset Size"); mark(3,"Reset Size","Restores HatVR's UI size to its default value.");
                beginSelectedRow(4); ImGui::Button("Reset Position"); mark(4,"Reset Position","Restores HatVR's UI distance and height to their default values.");
                check("Head-Locked HUD",hudHeadLocked,5,"Keeps HatVR's OpenXR UI attached to your view instead of remaining positioned in the VR world.");
                check("Curse Casual Menu Font",g_hatVrCurseCasualMenuFont,6,"Uses A Hat in Time's Curse Casual typeface for the HatVR settings menu. Enabled by default. Turn this off to use Dear ImGui's standard font instead.");
                beginSelectedRow(7); ImGui::Button("Reset All UI Settings"); mark(7,"Reset All UI Settings","Restores HatVR's UI size, distance, height, position, and menu font to their defaults.");
            }
            else if(page==4)
            {
                const char* views[]={"Off","Left Eye","Right Eye","Both Eyes"};
                int view=(spectatorView>=0&&spectatorView<4)?spectatorView:2;
                beginSelectedRow(0); ImGui::Combo("Spectator View",&view,views,4);
                mark(0,"Spectator View","Chooses what HatVR displays in A Hat in Time's desktop window. Left and Right Eye are center-cropped to fill the window. Both Eyes preserves the complete stereo pair. Off adds no spectator-specific desktop work.");
                const char* uiModes[]={"Hidden","Overlay"};
                int ui=(spectatorUiMode>=0&&spectatorUiMode<2)?spectatorUiMode:1;
                beginSelectedRow(1); ImGui::Combo("VR UI",&ui,uiModes,2);
                mark(1,"VR UI","Hidden shows only the selected game view. Overlay composites A Hat in Time's existing VR UI texture once over the desktop window. HatVR's settings menu remains headset-only for now.");
                if(g_sharperNativeStereo)
                {
                    beginSelectedRow(2); ImGui::BeginDisabled(); bool expanded=false; ImGui::Checkbox("Expanded Spectator FOV",&expanded); ImGui::EndDisabled();
                    mark(2,"Expanded Spectator FOV","Unavailable with Sharp. Stereo and Sequential render a wider horizontal envelope that can be shown in the desktop spectator view; Sharp renders directly to the OpenXR eye projection and has no extra image to reveal.");
                }
                else
                    check("Expanded Spectator FOV",g_spectatorExpandedFov,2,"Shows the wider horizontal render envelope in the desktop spectator view instead of cropping it to the headset's submitted FOV. This only changes the desktop view, not the image in the headset. Not supported by Sharp.");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("Accurate OpenXR compositor mirroring is intentionally omitted; use SteamVR VR View when a runtime-accurate mirror is needed.");
                ImGui::PopStyleColor();
            }
            else if(page==5)
            {
                ImGui::TextUnformatted("A Hat in Time Split-Screen Settings");
                ImGui::TextWrapped("A Hat in Time lowers Environment Detail and disables Light Rays in Split Screen. These seem to be performance changes rather than fixes for rendering issues. They are completely optional in HatVR.");
                if(ImGui::BeginTable("SplitScreenGraphics",2,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg))
                {
                    const char* names[]={"Environment Detail","Light Rays"};
                    const char* vals[]={"Lowered by Split Screen","Disabled by Split Screen"};
                    for(int i=0;i<2;++i){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(names[i]);ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(vals[i]);}
                    ImGui::EndTable();
                }
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextUnformatted("Known Graphics Issues");
                ImGui::TextWrapped("Ambient Occlusion - Can look wrong in Stereo on some headsets. If this happens, try turning Ambient Occlusion off.");
                ImGui::TextWrapped("Some cutscenes and effects may render incorrectly in one eye with Stereo. Synchronized Sequential is the compatibility fallback when this happens.");
                mark(0,"Graphics","Notes about A Hat in Time's own split-screen graphics changes and known HatVR rendering issues.");
            }
            else // Debug Tools
            {
                ImGui::TextWrapped("Internal candidate routing tools. This page only exists when DebugToolsEnabled=1 is set in HatVR.ini.");
                ImGui::Spacing();
                const char* preview = uiDebugPreviewMode==1 ? "SBS / Game" : (uiDebugPreviewMode==2 ? "OpenXR" : "Normal");
                const char* route = uiDebugPermanentRoute==1 ? "SBS / Game" : (uiDebugPermanentRoute==2 ? "OpenXR" : "Unmarked");
                ImGui::Text("Candidate: %d / %u", uiDebugCandidateIndex >= 0 ? uiDebugCandidateIndex + 1 : 0, uiDebugCandidateCount);
                ImGui::Text("Hash: 0x%016llX   Hits: %llu", uiDebugCandidateHash, uiDebugCandidateHits);
                ImGui::Text("Preview: %s   Route: %s", preview, route);
                beginSelectedRow(0); ImGui::Button("Previous Candidate"); mark(0,"Previous Candidate","Selects the previous discovered draw family.");
                beginSelectedRow(1); ImGui::Button("Next Candidate"); mark(1,"Next Candidate","Selects the next discovered draw family.");
                char previewLabel[96]={}; _snprintf_s(previewLabel,sizeof(previewLabel),_TRUNCATE,"Preview Destination: %s",preview);
                beginSelectedRow(2); ImGui::Button(previewLabel); mark(2,"Preview Destination","Cycles the selected candidate between its normal route, the game framebuffer, and the OpenXR UI panel.");
                beginSelectedRow(3); ImGui::Button("Mark SBS / Game"); mark(3,"Mark SBS / Game","Keeps the selected candidate in the normal game framebuffer.");
                beginSelectedRow(4); ImGui::Button("Mark OpenXR"); mark(4,"Mark OpenXR","Routes the selected candidate into the detached OpenXR UI panel.");
                beginSelectedRow(5); ImGui::Button("Clear Mark"); mark(5,"Clear Mark","Clears the manual route and returns to built-in handling.");
            }
            if(!insideCategory)
            {
                descTitle=HatVrMenuPageName(page);
                desc="Press A to enter this category. Use Up / Down to choose another category, or B to close the HatVR menu.";
            }
            ImGui::EndChild();
            ImGui::BeginChild("Description",{0,0},true);
            ImGui::TextUnformatted(descTitle); ImGui::Separator(); ImGui::TextWrapped("%s",desc);
            ImGui::EndChild();
            ImGui::EndGroup();
            ImGui::End();
            ImGui::Render();
        }

        void BuildHatVrStartupHint()
        {
            constexpr float W=900.0f,H=760.0f;
            ImGuiIO& io=ImGui::GetIO(); io.DisplaySize={W,H}; io.DeltaTime=1.0f/90.0f;
            g_vkMenuFont=(g_hatVrCurseCasualMenuFont && g_vkMenuCurseCasualFont)
                ? g_vkMenuCurseCasualFont : g_vkMenuDefaultFont;
            if(g_vkMenuFont) io.FontDefault=g_vkMenuFont;
            ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame();
            ImGui::StyleColorsDark();
            ImGui::SetNextWindowPos({W*0.5f, H*0.72f},ImGuiCond_Always,{0.5f,0.5f});
            ImGui::SetNextWindowBgAlpha(0.82f);
            ImGui::Begin("##HatVrStartupHint",nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_AlwaysAutoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("Double-tap Y / Triangle to open the HatVR menu");
            ImGui::End();
            ImGui::Render();
        }

        static bool HatVrMenuSwapchainIsSrgb()
        {
            return g_vkSwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                   g_vkSwapchainFormat == VK_FORMAT_R8G8B8A8_SRGB;
        }

        static unsigned char HatVrMenuSrgbByteToLinear(unsigned char v)
        {
            const float s = float(v) / 255.0f;
            const float linear = (s <= 0.04045f)
                ? (s / 12.92f)
                : powf((s + 0.055f) / 1.055f, 2.4f);
            return (unsigned char)(linear * 255.0f + 0.5f);
        }

        static void HatVrMenuConvertDrawDataSrgbToLinear(ImDrawData* drawData)
        {
            if (!drawData || !HatVrMenuSwapchainIsSrgb())
                return;

            for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
            {
                ImDrawList* list = drawData->CmdLists[listIndex];
                for (int vertexIndex = 0; vertexIndex < list->VtxBuffer.Size; ++vertexIndex)
                {
                    ImU32& col = list->VtxBuffer[vertexIndex].col;
                    const unsigned char r = (unsigned char)((col >> IM_COL32_R_SHIFT) & 0xFFu);
                    const unsigned char g = (unsigned char)((col >> IM_COL32_G_SHIFT) & 0xFFu);
                    const unsigned char b = (unsigned char)((col >> IM_COL32_B_SHIFT) & 0xFFu);
                    const unsigned char a = (unsigned char)((col >> IM_COL32_A_SHIFT) & 0xFFu);

                    col =
                        (ImU32(HatVrMenuSrgbByteToLinear(r)) << IM_COL32_R_SHIFT) |
                        (ImU32(HatVrMenuSrgbByteToLinear(g)) << IM_COL32_G_SHIFT) |
                        (ImU32(HatVrMenuSrgbByteToLinear(b)) << IM_COL32_B_SHIFT) |
                        (ImU32(a) << IM_COL32_A_SHIFT);
                }
            }
        }

        bool RenderHatVrImGuiToSwapchain(uint32_t imageIndex)
        {
            if(imageIndex>=g_vkMenuGuiCommandBuffers.size()) return false;
            VkFence fence=g_vkMenuGuiFences[imageIndex];
            if(vkWaitForFences(g_vkDevice,1,&fence,VK_TRUE,1000000000ULL)!=VK_SUCCESS) return false;
            vkResetFences(g_vkDevice,1,&fence);
            VkCommandBuffer cb=g_vkMenuGuiCommandBuffers[imageIndex];
            vkResetCommandBuffer(cb,0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if(vkBeginCommandBuffer(cb,&bi)!=VK_SUCCESS) return false;
            VkClearValue clear{}; clear.color={{0,0,0,0}};
            VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            ri.renderPass=g_vkMenuRenderPass; ri.framebuffer=g_vkMenuFramebuffers[imageIndex];
            ri.renderArea.extent={g_vkMenuWidth,g_vkMenuHeight}; ri.clearValueCount=1; ri.pClearValues=&clear;
            vkCmdBeginRenderPass(cb,&ri,VK_SUBPASS_CONTENTS_INLINE);

            // Dear ImGui's stock colors are authored as display/sRGB values. When the
            // OpenXR menu swapchain itself is sRGB, Vulkan's attachment conversion expects
            // linear shader output. Convert only the generated ImGui vertex RGB here.
            // Alpha is deliberately left untouched. Draw data is rebuilt every frame.
            ImDrawData* menuDrawData = ImGui::GetDrawData();
            HatVrMenuConvertDrawDataSrgbToLinear(menuDrawData);
            ImGui_ImplVulkan_RenderDrawData(menuDrawData,cb);

            vkCmdEndRenderPass(cb);
            if(vkEndCommandBuffer(cb)!=VK_SUCCESS) return false;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
            if(vkQueueSubmit(g_vkQueue,1,&si,fence)!=VK_SUCCESS) return false;
            // OpenXR must not consume the image before the GPU finishes writing it.
            return vkWaitForFences(g_vkDevice,1,&fence,VK_TRUE,1000000000ULL)==VK_SUCCESS;
        }

        void DestroyMenuResources()
        {
            DestroyMenuGpuRenderer();
            g_vkMenuSwapchainImages.clear();
            if (g_vkMenuSwapchain != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(g_vkMenuSwapchain);
                g_vkMenuSwapchain = XR_NULL_HANDLE;
            }
            g_vkMenuWidth = g_vkMenuHeight = 0;
        }

        bool EnsureMenuResources(IDirect3DTexture9* referenceTexture, uint32_t width, uint32_t height)
        {
            if (!referenceTexture || !width || !height || !g_vkSwapchainFormat)
                return false;

            const bool same = g_vkMenuSwapchain != XR_NULL_HANDLE &&
                g_vkMenuWidth == width && g_vkMenuHeight == height;
            if (same) return true;

            DestroyMenuResources();
            g_vkMenuWidth = width; g_vkMenuHeight = height;

            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ci.format = g_vkSwapchainFormat;
            ci.sampleCount = 1; ci.width = width; ci.height = height;
            ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
            XrResult xr = xrCreateSwapchain(g_vkXrSession, &ci, &g_vkMenuSwapchain);
            if (XR_FAILED(xr)) { DestroyMenuResources(); return false; }
            uint32_t count = 0;
            xr = xrEnumerateSwapchainImages(g_vkMenuSwapchain, 0, &count, nullptr);
            if (XR_FAILED(xr) || !count) { DestroyMenuResources(); return false; }
            g_vkMenuSwapchainImages.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
            xr = xrEnumerateSwapchainImages(g_vkMenuSwapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(g_vkMenuSwapchainImages.data()));
            if (XR_FAILED(xr)) { DestroyMenuResources(); return false; }
            DxvkPathTrace("HATVR-MENU dedicated OpenXR swapchain ready %ux%u images=%u", width, height, count);
            return true;
        }







        uint64_t g_hatVrMenuContentGeneration=0;



        void DestroyPhase81UiStagingTexture()
        {
            if (g_phase81UiStagingTexture)
            {
                g_phase81UiStagingTexture->Release();
                g_phase81UiStagingTexture = nullptr;
            }
            if (g_phase81UiStagingDevice)
            {
                g_phase81UiStagingDevice->Release();
                g_phase81UiStagingDevice = nullptr;
            }
            g_phase81UiStagingWidth = 0;
            g_phase81UiStagingHeight = 0;
        }

        bool EnsurePhase81UiStagingTexture(
            IDirect3DTexture9* finishedUiTexture,
            uint32_t width, uint32_t height)
        {
            if (!finishedUiTexture || !width || !height) return false;

            IDirect3DDevice9* device = nullptr;
            if (FAILED(finishedUiTexture->GetDevice(&device)) || !device)
                return false;

            const bool reusable =
                g_phase81UiStagingTexture &&
                g_phase81UiStagingDevice == device &&
                g_phase81UiStagingWidth == width &&
                g_phase81UiStagingHeight == height;
            if (reusable)
            {
                device->Release();
                return true;
            }

            DestroyPhase81UiStagingTexture();
            g_phase81UiStagingDevice = device; // keep the GetDevice reference

            HRESULT hr = device->CreateTexture(
                width, height, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                &g_phase81UiStagingTexture, nullptr);
            if (FAILED(hr) || !g_phase81UiStagingTexture)
            {
                DxvkPathTrace("VKBRIDGE-V5 PHASE8.3 UI-STAGING CREATE FAILED hr=0x%08X size=%ux%u",
                    (unsigned)hr, width, height);
                DestroyPhase81UiStagingTexture();
                return false;
            }

            g_phase81UiStagingWidth = width;
            g_phase81UiStagingHeight = height;
            DxvkPathTrace("VKBRIDGE-V5 PHASE8.3 UI-STAGING READY texture=%p size=%ux%u (native SBS untouched)",
                g_phase81UiStagingTexture, width, height);
            return true;
        }

        bool ScaleFinishedUiToEyeTexture(
            IDirect3DTexture9* finishedUiTexture,
            IDirect3DTexture9* eyeSizedTexture,
            bool nativeStereo)
        {
            if (!finishedUiTexture || !eyeSizedTexture) return false;
            IDirect3DDevice9* device = nullptr;
            IDirect3DSurface9* src = nullptr;
            IDirect3DSurface9* dst = nullptr;
            bool ok = false;
            if (SUCCEEDED(finishedUiTexture->GetDevice(&device)) && device &&
                SUCCEEDED(finishedUiTexture->GetSurfaceLevel(0, &src)) && src &&
                SUCCEEDED(eyeSizedTexture->GetSurfaceLevel(0, &dst)) && dst)
            {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(src->GetDesc(&sd)))
                {
                    // AHiT UI is its own compositor layer.  It is NOT an AFR eye.
                    // the redirected UE3 Canvas/UI target is a complete full-width
                    // UI canvas and should be retained as one mono image, then shown
                    // identically to both OpenXR eyes.  Cropping it to the active AFR
                    // half makes a 0.93:1 half-canvas fill the 16:9 quad, which is the
                    // avoids horizontal stretching/flicker.
                    //
                    // keep AFR exclusively in the world-eye textures.  The UI path
                    // always consumes the complete canvas, regardless of render mode.
                    RECT r{0,0,(LONG)sd.Width,(LONG)sd.Height};
                    ok = SUCCEEDED(device->StretchRect(
                        src,&r,dst,nullptr,D3DTEXF_LINEAR));
                }
            }
            if(dst) dst->Release();
            if(src) src->Release();
            if(device) device->Release();
            return ok;
        }

        struct VkBridgeTimingSample
        {
            double getRenderTargetDataMs = 0.0;
            double lockCopyMs = 0.0;
            double uploadMemcpyMs = 0.0;
            double uploadGpuMs = 0.0;
        };

        VkBridgeTimingSample g_vkBridgeTiming[3];

        double QpcElapsedMs(const LARGE_INTEGER& begin, const LARGE_INTEGER& end)
        {
            static LARGE_INTEGER freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return f;
            }();
            return (double(end.QuadPart - begin.QuadPart) * 1000.0) / double(freq.QuadPart);
        }

        bool ReadD3D9TextureBGRA(
            IDirect3DTexture9* texture, UINT width, UINT height,
            std::vector<unsigned char>& pixels, int cacheSlot)
        {
            if (!texture || !width || !height || cacheSlot < 0 || cacheSlot > 1)
                return false;

            IDirect3DDevice9* device = nullptr;
            IDirect3DSurface9* src = nullptr;
            bool ok = false;
            if (FAILED(texture->GetDevice(&device)) || !device) goto done;
            if (FAILED(texture->GetSurfaceLevel(0, &src)) || !src) goto done;

            D3DSURFACE_DESC desc{};
            if (FAILED(src->GetDesc(&desc))) goto done;
            if (desc.Width != width || desc.Height != height) goto done;
            if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
                goto done;

            auto& c = g_vkReadbackCache[cacheSlot];
            if (!c.surface || c.width != width || c.height != height || c.format != desc.Format)
            {
                if (c.surface) c.surface->Release();
                c = {};
                if (FAILED(device->CreateOffscreenPlainSurface(
                    width, height, desc.Format, D3DPOOL_SYSTEMMEM,
                    &c.surface, nullptr)) || !c.surface)
                    goto done;
                c.width = width; c.height = height; c.format = desc.Format;
                DxvkPathTrace("VKXR-V8 readback cache[%d] READY %ux%u", cacheSlot,width,height);
            }

            LARGE_INTEGER tRead0{}, tRead1{}, tCopy1{};
            QueryPerformanceCounter(&tRead0);
            if (FAILED(device->GetRenderTargetData(src, c.surface))) goto done;
            QueryPerformanceCounter(&tRead1);
            D3DLOCKED_RECT lr{};
            if (FAILED(c.surface->LockRect(&lr, nullptr, D3DLOCK_READONLY))) goto done;

            const size_t bytes = size_t(width) * size_t(height) * 4u;
            if (pixels.size() != bytes) pixels.resize(bytes);
            for (UINT y=0; y<height; ++y)
                std::memcpy(pixels.data()+size_t(y)*width*4u,
                    static_cast<const unsigned char*>(lr.pBits)+size_t(y)*lr.Pitch,
                    size_t(width)*4u);
            c.surface->UnlockRect();
            QueryPerformanceCounter(&tCopy1);
            g_vkBridgeTiming[cacheSlot].getRenderTargetDataMs = QpcElapsedMs(tRead0, tRead1);
            g_vkBridgeTiming[cacheSlot].lockCopyMs = QpcElapsedMs(tRead1, tCopy1);
            ok = true;
        done:
            if (src) src->Release();
            if (device) device->Release();
            return ok;
        }


        // Returns true only when a new menu image was submitted. Never waits.

        bool UploadPixelsToImage(
            const std::vector<unsigned char>& pixels,
            uint32_t width, uint32_t height,
            VkImage image, int cacheSlot)
        {
            if (pixels.empty() || !image || !EnsureCommandResources() ||
                cacheSlot < 0 || cacheSlot > 2)
                return false;

            const VkDeviceSize byteCount = VkDeviceSize(pixels.size());
            auto& c = g_vkUploadCache[cacheSlot];
            if (!c.buffer || c.capacity < byteCount)
            {
                if (c.mapped && c.memory) vkUnmapMemory(g_vkDevice,c.memory);
                if (c.buffer) vkDestroyBuffer(g_vkDevice,c.buffer,nullptr);
                if (c.memory) vkFreeMemory(g_vkDevice,c.memory,nullptr);
                c = {};

                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = byteCount;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                if (vkCreateBuffer(g_vkDevice,&bci,nullptr,&c.buffer) != VK_SUCCESS)
                    return false;
                VkMemoryRequirements mr{};
                vkGetBufferMemoryRequirements(g_vkDevice,c.buffer,&mr);
                const uint32_t mt=FindMemoryType(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (mt==UINT32_MAX) return false;
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.allocationSize=mr.size; mai.memoryTypeIndex=mt;
                if (vkAllocateMemory(g_vkDevice,&mai,nullptr,&c.memory)!=VK_SUCCESS) return false;
                if (vkBindBufferMemory(g_vkDevice,c.buffer,c.memory,0)!=VK_SUCCESS) return false;
                if (vkMapMemory(g_vkDevice,c.memory,0,VK_WHOLE_SIZE,0,&c.mapped)!=VK_SUCCESS) return false;
                c.capacity=mr.size;
                DxvkPathTrace("VKXR-V8 upload cache[%d] READY bytes=%llu",
                    cacheSlot,(unsigned long long)c.capacity);
            }
            LARGE_INTEGER tUpload0{}, tUploadCopy1{}, tUploadGpu1{};
            QueryPerformanceCounter(&tUpload0);
            std::memcpy(c.mapped,pixels.data(),pixels.size());
            QueryPerformanceCounter(&tUploadCopy1);
            VkBuffer buffer=c.buffer;

            vkResetFences(g_vkDevice, 1, &g_vkFence);
            vkResetCommandBuffer(g_vkCommandBuffer, 0);

            VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(g_vkCommandBuffer, &cbi) != VK_SUCCESS)
                goto fail;

            VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            // OpenXR Vulkan swapchain color images are handed to us in a
            // layout compatible with COLOR_ATTACHMENT_OPTIMAL after Wait.
            // Preserve that contract rather than pretending the contents/layout
            // are undefined.
            toTransfer.srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.baseMipLevel = 0;
            toTransfer.subresourceRange.levelCount = 1;
            toTransfer.subresourceRange.baseArrayLayer = 0;
            toTransfer.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(
                g_vkCommandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toTransfer);

            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = { width, height, 1 };
            vkCmdCopyBufferToImage(
                g_vkCommandBuffer, buffer, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor.image = image;
            toColor.subresourceRange = toTransfer.subresourceRange;

            vkCmdPipelineBarrier(
                g_vkCommandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toColor);

            if (vkEndCommandBuffer(g_vkCommandBuffer) != VK_SUCCESS)
                goto fail;

            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g_vkCommandBuffer;
            if (vkQueueSubmit(g_vkQueue, 1, &si, g_vkFence) != VK_SUCCESS)
                goto fail;
            if (vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                goto fail;
            QueryPerformanceCounter(&tUploadGpu1);
            g_vkBridgeTiming[cacheSlot].uploadMemcpyMs = QpcElapsedMs(tUpload0, tUploadCopy1);
            g_vkBridgeTiming[cacheSlot].uploadGpuMs = QpcElapsedMs(tUploadCopy1, tUploadGpu1);

            return true;

        fail:
            return false;
        }

        // NIS (NVIDIA Image Scaling) resources live on HatVR's OpenXR Vulkan device.
        // the game/DXVK source resolution never changes; NIS only enlarges the final eye image.
        struct HatVrNisState
        {
            VkShaderModule shader = VK_NULL_HANDLE;
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkBuffer configBuffer = VK_NULL_HANDLE;
            VkDeviceMemory configMemory = VK_NULL_HANDLE;
            void* configMapped = nullptr;
            VkImage coefScale = VK_NULL_HANDLE, coefUsm = VK_NULL_HANDLE;
            VkDeviceMemory coefScaleMem = VK_NULL_HANDLE, coefUsmMem = VK_NULL_HANDLE;
            VkImageView coefScaleView = VK_NULL_HANDLE, coefUsmView = VK_NULL_HANDLE;
            VkImage output = VK_NULL_HANDLE;
            VkDeviceMemory outputMem = VK_NULL_HANDLE;
            VkImageView outputView = VK_NULL_HANDLE;
            uint32_t outputW = 0, outputH = 0;
            VkFormat outputFormat = VK_FORMAT_UNDEFINED;
            bool initialized = false;
            bool failed = false;
        };
        HatVrNisState g_nis{};

        bool CreateNisBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
            VkBuffer& buffer, VkDeviceMemory& memory, void** mapped = nullptr)
        {
            VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            if(vkCreateBuffer(g_vkDevice,&bi,nullptr,&buffer)!=VK_SUCCESS) return false;
            VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(g_vkDevice,buffer,&mr);
            uint32_t mt=FindMemoryType(mr.memoryTypeBits,props); if(mt==UINT32_MAX) return false;
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize=mr.size; ai.memoryTypeIndex=mt;
            if(vkAllocateMemory(g_vkDevice,&ai,nullptr,&memory)!=VK_SUCCESS) return false;
            if(vkBindBufferMemory(g_vkDevice,buffer,memory,0)!=VK_SUCCESS) return false;
            if(mapped && vkMapMemory(g_vkDevice,memory,0,size,0,mapped)!=VK_SUCCESS) return false;
            return true;
        }

        bool CreateNisImage(uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage,VkImage& image,VkDeviceMemory& memory)
        {
            VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format; ci.extent={w,h,1};
            ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            if(vkCreateImage(g_vkDevice,&ci,nullptr,&image)!=VK_SUCCESS) return false;
            VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(g_vkDevice,image,&mr);
            uint32_t mt=FindMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if(mt==UINT32_MAX) return false;
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize=mr.size; ai.memoryTypeIndex=mt;
            if(vkAllocateMemory(g_vkDevice,&ai,nullptr,&memory)!=VK_SUCCESS) return false;
            return vkBindImageMemory(g_vkDevice,image,memory,0)==VK_SUCCESS;
        }

        VkImageView CreateNisView(VkImage image,VkFormat format)
        {
            VkImageView view=VK_NULL_HANDLE; VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            return vkCreateImageView(g_vkDevice,&vi,nullptr,&view)==VK_SUCCESS ? view : VK_NULL_HANDLE;
        }

        bool UploadNisCoefficientImage(VkImage image,const void* data,size_t bytes)
        {
            VkBuffer staging=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE; void* mapped=nullptr;
            if(!CreateNisBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,staging,mem,&mapped)) return false;
            memcpy(mapped,data,bytes); vkUnmapMemory(g_vkDevice,mem);
            vkResetFences(g_vkDevice,1,&g_vkFence); vkResetCommandBuffer(g_vkCommandBuffer,0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if(vkBeginCommandBuffer(g_vkCommandBuffer,&bi)!=VK_SUCCESS) return false;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=0; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(g_vkCommandBuffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy c{}; c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.imageExtent={kFilterSize/4,kPhaseCount,1};
            vkCmdCopyBufferToImage(g_vkCommandBuffer,staging,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(g_vkCommandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
            vkEndCommandBuffer(g_vkCommandBuffer); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&g_vkCommandBuffer;
            bool ok=vkQueueSubmit(g_vkQueue,1,&si,g_vkFence)==VK_SUCCESS && vkWaitForFences(g_vkDevice,1,&g_vkFence,VK_TRUE,UINT64_MAX)==VK_SUCCESS;
            vkDestroyBuffer(g_vkDevice,staging,nullptr); vkFreeMemory(g_vkDevice,mem,nullptr); return ok;
        }

        bool EnsureNisResources()
        {
            if(g_nis.initialized) return true; if(g_nis.failed || !g_vkDevice || !EnsureCommandResources()) return false;
            auto fail=[&](){ g_nis.failed=true; DxvkPathTrace("NIS-V1 initialization failed; using Vulkan linear fallback"); return false; };
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=kHatVrNisScalerSpvSize; sm.pCode=reinterpret_cast<const uint32_t*>(kHatVrNisScalerSpv);
            if(vkCreateShaderModule(g_vkDevice,&sm,nullptr,&g_nis.shader)!=VK_SUCCESS) return fail();
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter=VK_FILTER_LINEAR; sci.minFilter=VK_FILTER_LINEAR; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.maxLod=0.0f;
            if(vkCreateSampler(g_vkDevice,&sci,nullptr,&g_nis.sampler)!=VK_SUCCESS) return fail();
            VkDescriptorSetLayoutBinding binds[6]{};
            binds[0]={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            binds[1]={1,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&g_nis.sampler};
            binds[2]={2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            binds[3]={3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            binds[4]={4,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            binds[5]={5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=6; dl.pBindings=binds;
            if(vkCreateDescriptorSetLayout(g_vkDevice,&dl,nullptr,&g_nis.setLayout)!=VK_SUCCESS) return fail();
            VkDescriptorPoolSize ps[3]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=3; dp.pPoolSizes=ps;
            if(vkCreateDescriptorPool(g_vkDevice,&dp,nullptr,&g_nis.pool)!=VK_SUCCESS) return fail();
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=g_nis.pool; da.descriptorSetCount=1; da.pSetLayouts=&g_nis.setLayout;
            if(vkAllocateDescriptorSets(g_vkDevice,&da,&g_nis.set)!=VK_SUCCESS) return fail();
            if(!CreateNisBuffer(sizeof(NISConfig),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,g_nis.configBuffer,g_nis.configMemory,&g_nis.configMapped)) return fail();
            VkDescriptorBufferInfo db{g_nis.configBuffer,0,sizeof(NISConfig)}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=g_nis.set; w.dstBinding=0; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&db; vkUpdateDescriptorSets(g_vkDevice,1,&w,0,nullptr);
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&g_nis.setLayout;
            if(vkCreatePipelineLayout(g_vkDevice,&pl,nullptr,&g_nis.pipelineLayout)!=VK_SUCCESS) return fail();
            VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; st.stage=VK_SHADER_STAGE_COMPUTE_BIT; st.module=g_nis.shader; st.pName="main";
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage=st; cp.layout=g_nis.pipelineLayout;
            if(vkCreateComputePipelines(g_vkDevice,VK_NULL_HANDLE,1,&cp,nullptr,&g_nis.pipeline)!=VK_SUCCESS) return fail();
            if(!CreateNisImage(kFilterSize/4,kPhaseCount,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,g_nis.coefScale,g_nis.coefScaleMem) ||
               !CreateNisImage(kFilterSize/4,kPhaseCount,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,g_nis.coefUsm,g_nis.coefUsmMem)) return fail();
            if(!UploadNisCoefficientImage(g_nis.coefScale,coef_scale_fp16,sizeof(coef_scale_fp16)) || !UploadNisCoefficientImage(g_nis.coefUsm,coef_usm_fp16,sizeof(coef_usm_fp16))) return fail();
            g_nis.coefScaleView=CreateNisView(g_nis.coefScale,VK_FORMAT_R16G16B16A16_SFLOAT); g_nis.coefUsmView=CreateNisView(g_nis.coefUsm,VK_FORMAT_R16G16B16A16_SFLOAT);
            if(!g_nis.coefScaleView||!g_nis.coefUsmView) return fail();
            g_nis.initialized=true; DxvkPathTrace("NIS-V1 initialized (official NVIDIA NIS shader, embedded SPIR-V)"); return true;
        }

        bool EnsureNisOutput(uint32_t w,uint32_t h,VkFormat format)
        {
            if(!EnsureNisResources()) return false;
            if(g_nis.output && g_nis.outputW==w && g_nis.outputH==h && g_nis.outputFormat==format) return true;
            if(g_nis.outputView) vkDestroyImageView(g_vkDevice,g_nis.outputView,nullptr); if(g_nis.output) vkDestroyImage(g_vkDevice,g_nis.output,nullptr); if(g_nis.outputMem) vkFreeMemory(g_vkDevice,g_nis.outputMem,nullptr);
            g_nis.outputView=VK_NULL_HANDLE; g_nis.output=VK_NULL_HANDLE; g_nis.outputMem=VK_NULL_HANDLE;
            if(!CreateNisImage(w,h,format,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,g_nis.output,g_nis.outputMem)) return false;
            g_nis.outputView=CreateNisView(g_nis.output,format); if(!g_nis.outputView) return false;
            g_nis.outputW=w; g_nis.outputH=h; g_nis.outputFormat=format; return true;
        }

        // Records NIS into an already-open command buffer. Caller owns source/destination layout restoration and queue synchronization.
        // Returns a temporary source view which must be destroyed only after the submitted command buffer has completed.
        VkImageView RecordNisUpscale(VkCommandBuffer cmd,VkImage src,VkFormat srcFormat,VkImageLayout srcLayout,
            uint32_t textureW,uint32_t textureH,uint32_t originX,uint32_t originY,uint32_t srcW,uint32_t srcH,
            VkImage dst,VkImageLayout dstLayout,uint32_t dstW,uint32_t dstH)
        {
            if(!EnsureNisOutput(dstW,dstH,(VkFormat)g_vkSwapchainFormat)) return VK_NULL_HANDLE;
            VkImageView srcView=CreateNisView(src,srcFormat); if(!srcView) return VK_NULL_HANDLE;
            NISConfig cfg{}; if(!NVScalerUpdateConfig(cfg,(float)g_openXrUpscaleSharpness/100.0f,originX,originY,srcW,srcH,textureW,textureH,0,0,dstW,dstH,dstW,dstH,NISHDRMode::None)){ vkDestroyImageView(g_vkDevice,srcView,nullptr); return VK_NULL_HANDLE; }
            memcpy(g_nis.configMapped,&cfg,sizeof(cfg));
            VkDescriptorImageInfo in{VK_NULL_HANDLE,srcView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo out{VK_NULL_HANDLE,g_nis.outputView,VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo cs{VK_NULL_HANDLE,g_nis.coefScaleView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo cu{VK_NULL_HANDLE,g_nis.coefUsmView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wr[4]{}; uint32_t bindings[4]={2,3,4,5}; VkDescriptorType types[4]={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}; VkDescriptorImageInfo* infos[4]={&in,&out,&cs,&cu};
            for(int i=0;i<4;++i){wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=g_nis.set;wr[i].dstBinding=bindings[i];wr[i].descriptorCount=1;wr[i].descriptorType=types[i];wr[i].pImageInfo=infos[i];} vkUpdateDescriptorSets(g_vkDevice,4,wr,0,nullptr);
            VkImageMemoryBarrier pre[3]{};
            pre[0]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; pre[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; pre[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT; pre[0].oldLayout=srcLayout; pre[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; pre[0].srcQueueFamilyIndex=pre[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; pre[0].image=src; pre[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            pre[1]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; pre[1].dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT; pre[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; pre[1].newLayout=VK_IMAGE_LAYOUT_GENERAL; pre[1].srcQueueFamilyIndex=pre[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; pre[1].image=g_nis.output; pre[1].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            pre[2]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; pre[2].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; pre[2].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; pre[2].oldLayout=dstLayout; pre[2].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; pre[2].srcQueueFamilyIndex=pre[2].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; pre[2].image=dst; pre[2].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,3,pre);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_nis.pipeline); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_nis.pipelineLayout,0,1,&g_nis.set,0,nullptr);
            vkCmdDispatch(cmd,(dstW+31)/32,(dstH+23)/24,1);
            VkImageMemoryBarrier post{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; post.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; post.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; post.oldLayout=VK_IMAGE_LAYOUT_GENERAL; post.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; post.srcQueueFamilyIndex=post.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; post.image=g_nis.output; post.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&post);
            VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.extent={dstW,dstH,1}; vkCmdCopyImage(cmd,g_nis.output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            VkImageMemoryBarrier restore[2]={pre[0],pre[2]}; restore[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT; restore[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; restore[0].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; restore[0].newLayout=srcLayout; restore[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; restore[1].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT; restore[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; restore[1].newLayout=dstLayout;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,restore);
            return srcView;
        }

        // AMD FidelityFX Super Resolution 1.x (EASU + optional RCAS).
        // Like NIS, this runs only on HatVR's Vulkan/OpenXR side and never changes AHiT's render resolution.
        struct HatVrFsrState
        {
            VkShaderModule easuShader=VK_NULL_HANDLE, rcasShader=VK_NULL_HANDLE;
            VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
            VkDescriptorPool pool=VK_NULL_HANDLE;
            VkDescriptorSet easuSet=VK_NULL_HANDLE, rcasSet=VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
            VkPipeline easuPipeline=VK_NULL_HANDLE, rcasPipeline=VK_NULL_HANDLE;
            VkSampler sampler=VK_NULL_HANDLE;
            VkBuffer easuConstants=VK_NULL_HANDLE, rcasConstants=VK_NULL_HANDLE;
            VkDeviceMemory easuConstantsMem=VK_NULL_HANDLE, rcasConstantsMem=VK_NULL_HANDLE;
            void* easuMapped=nullptr; void* rcasMapped=nullptr;
            VkImage intermediate=VK_NULL_HANDLE, output=VK_NULL_HANDLE;
            VkDeviceMemory intermediateMem=VK_NULL_HANDLE, outputMem=VK_NULL_HANDLE;
            VkImageView intermediateView=VK_NULL_HANDLE, outputView=VK_NULL_HANDLE;
            uint32_t outputW=0, outputH=0;
            bool initialized=false, failed=false;
        };
        HatVrFsrState g_fsr{};

        struct HatVrFsrConstants {
            uint32_t Const0[4];
            uint32_t Const1[4];
            uint32_t Const2[4];
            uint32_t Const3[4];
            uint32_t Sample[4];
        };

        bool EnsureFsrResources()
        {
            if(g_fsr.initialized) return true;
            if(g_fsr.failed || !g_vkDevice || !EnsureCommandResources()) return false;
            auto fail=[&](){ g_fsr.failed=true; DxvkPathTrace("FSR1-V1 initialization failed; using Vulkan linear fallback"); return false; };
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize=kHatVrFsrEasuSpvSize; sm.pCode=kHatVrFsrEasuSpv; if(vkCreateShaderModule(g_vkDevice,&sm,nullptr,&g_fsr.easuShader)!=VK_SUCCESS) return fail();
            sm.codeSize=kHatVrFsrRcasSpvSize; sm.pCode=kHatVrFsrRcasSpv; if(vkCreateShaderModule(g_vkDevice,&sm,nullptr,&g_fsr.rcasShader)!=VK_SUCCESS) return fail();
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter=VK_FILTER_LINEAR; sci.minFilter=VK_FILTER_LINEAR; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.minLod=-1000.0f; sci.maxLod=1000.0f; sci.maxAnisotropy=1.0f;
            if(vkCreateSampler(g_vkDevice,&sci,nullptr,&g_fsr.sampler)!=VK_SUCCESS) return fail();
            VkDescriptorSetLayoutBinding b[4]{};
            b[0]={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            b[1]={1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            b[2]={2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            b[3]={3,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&g_fsr.sampler};
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=4; dl.pBindings=b;
            if(vkCreateDescriptorSetLayout(g_vkDevice,&dl,nullptr,&g_fsr.setLayout)!=VK_SUCCESS) return fail();
            VkDescriptorPoolSize ps[3]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=2; dp.poolSizeCount=3; dp.pPoolSizes=ps;
            if(vkCreateDescriptorPool(g_vkDevice,&dp,nullptr,&g_fsr.pool)!=VK_SUCCESS) return fail();
            VkDescriptorSetLayout layouts[2]={g_fsr.setLayout,g_fsr.setLayout}; VkDescriptorSet sets[2]{};
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=g_fsr.pool; da.descriptorSetCount=2; da.pSetLayouts=layouts;
            if(vkAllocateDescriptorSets(g_vkDevice,&da,sets)!=VK_SUCCESS) return fail(); g_fsr.easuSet=sets[0]; g_fsr.rcasSet=sets[1];
            if(!CreateNisBuffer(sizeof(HatVrFsrConstants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,g_fsr.easuConstants,g_fsr.easuConstantsMem,&g_fsr.easuMapped) ||
               !CreateNisBuffer(sizeof(HatVrFsrConstants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,g_fsr.rcasConstants,g_fsr.rcasConstantsMem,&g_fsr.rcasMapped)) return fail();
            VkDescriptorBufferInfo db[2]={{g_fsr.easuConstants,0,sizeof(HatVrFsrConstants)},{g_fsr.rcasConstants,0,sizeof(HatVrFsrConstants)}};
            VkWriteDescriptorSet wb[2]{}; for(int i=0;i<2;++i){wb[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wb[i].dstSet=i?g_fsr.rcasSet:g_fsr.easuSet;wb[i].dstBinding=0;wb[i].descriptorCount=1;wb[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;wb[i].pBufferInfo=&db[i];} vkUpdateDescriptorSets(g_vkDevice,2,wb,0,nullptr);
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&g_fsr.setLayout; if(vkCreatePipelineLayout(g_vkDevice,&pl,nullptr,&g_fsr.pipelineLayout)!=VK_SUCCESS) return fail();
            auto makePipe=[&](VkShaderModule shader,VkPipeline& pipe){VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};st.stage=VK_SHADER_STAGE_COMPUTE_BIT;st.module=shader;st.pName="main";VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage=st;cp.layout=g_fsr.pipelineLayout;return vkCreateComputePipelines(g_vkDevice,VK_NULL_HANDLE,1,&cp,nullptr,&pipe)==VK_SUCCESS;};
            if(!makePipe(g_fsr.easuShader,g_fsr.easuPipeline)||!makePipe(g_fsr.rcasShader,g_fsr.rcasPipeline)) return fail();
            g_fsr.initialized=true; DxvkPathTrace("FSR1-V1 initialized (AMD FidelityFX FSR 1 EASU/RCAS, embedded SPIR-V)"); return true;
        }

        bool EnsureFsrOutput(uint32_t w,uint32_t h)
        {
            if(!EnsureFsrResources()) return false;
            if(g_fsr.output && g_fsr.outputW==w && g_fsr.outputH==h) return true;
            auto kill=[&](VkImageView& v,VkImage& i,VkDeviceMemory& m){if(v)vkDestroyImageView(g_vkDevice,v,nullptr);if(i)vkDestroyImage(g_vkDevice,i,nullptr);if(m)vkFreeMemory(g_vkDevice,m,nullptr);v=VK_NULL_HANDLE;i=VK_NULL_HANDLE;m=VK_NULL_HANDLE;};
            kill(g_fsr.intermediateView,g_fsr.intermediate,g_fsr.intermediateMem); kill(g_fsr.outputView,g_fsr.output,g_fsr.outputMem);
            const VkImageUsageFlags usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if(!CreateNisImage(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,usage,g_fsr.intermediate,g_fsr.intermediateMem) || !CreateNisImage(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,usage,g_fsr.output,g_fsr.outputMem)) return false;
            g_fsr.intermediateView=CreateNisView(g_fsr.intermediate,VK_FORMAT_R16G16B16A16_SFLOAT); g_fsr.outputView=CreateNisView(g_fsr.output,VK_FORMAT_R16G16B16A16_SFLOAT);
            if(!g_fsr.intermediateView||!g_fsr.outputView) return false; g_fsr.outputW=w;g_fsr.outputH=h;return true;
        }

        VkImageView RecordFsrUpscale(VkCommandBuffer cmd,VkImage src,VkFormat srcFormat,VkImageLayout srcLayout,
            uint32_t textureW,uint32_t textureH,uint32_t originX,uint32_t originY,uint32_t srcW,uint32_t srcH,
            VkImage dst,VkImageLayout dstLayout,uint32_t dstW,uint32_t dstH)
        {
            if(!EnsureFsrOutput(dstW,dstH)) return VK_NULL_HANDLE;
            VkImageView srcView=CreateNisView(src,srcFormat); if(!srcView) return VK_NULL_HANDLE;
            HatVrFsrConstants easu{}; FsrEasuConOffset(easu.Const0,easu.Const1,easu.Const2,easu.Const3,(AF1)srcW,(AF1)srcH,(AF1)textureW,(AF1)textureH,(AF1)dstW,(AF1)dstH,(AF1)originX,(AF1)originY);
            const bool xrTargetIsSrgb = ((VkFormat)g_vkSwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB || (VkFormat)g_vkSwapchainFormat == VK_FORMAT_R8G8B8A8_SRGB);
            const bool useRcas=g_openXrUpscaleSharpness>0;
            // Sample.x is a HatVR extension to AMD's sample shader: only the final FSR pass
            // decodes the display-curve result to linear before Vulkan blits into an sRGB XR image.
            // r247d's embedded shaders use the exact inverse-sRGB transfer function here.
            easu.Sample[0]=(xrTargetIsSrgb && !useRcas) ? 1u : 0u; memcpy(g_fsr.easuMapped,&easu,sizeof(easu));
            HatVrFsrConstants rcas{}; if(useRcas){const float stops=2.0f*(1.0f-(float)g_openXrUpscaleSharpness/100.0f);FsrRcasCon(rcas.Const0,(AF1)stops);rcas.Sample[0]=xrTargetIsSrgb ? 1u : 0u;memcpy(g_fsr.rcasMapped,&rcas,sizeof(rcas));}
            VkDescriptorImageInfo ein{VK_NULL_HANDLE,srcView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorImageInfo eout{VK_NULL_HANDLE,useRcas?g_fsr.intermediateView:g_fsr.outputView,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet ew[2]{}; for(int i=0;i<2;++i) ew[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ew[0].dstSet=g_fsr.easuSet;ew[0].dstBinding=1;ew[0].descriptorCount=1;ew[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;ew[0].pImageInfo=&ein; ew[1].dstSet=g_fsr.easuSet;ew[1].dstBinding=2;ew[1].descriptorCount=1;ew[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;ew[1].pImageInfo=&eout; vkUpdateDescriptorSets(g_vkDevice,2,ew,0,nullptr);
            if(useRcas){VkDescriptorImageInfo rin{VK_NULL_HANDLE,g_fsr.intermediateView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorImageInfo rout{VK_NULL_HANDLE,g_fsr.outputView,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet rw[2]{};for(int i=0;i<2;++i)rw[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;rw[0].dstSet=g_fsr.rcasSet;rw[0].dstBinding=1;rw[0].descriptorCount=1;rw[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;rw[0].pImageInfo=&rin;rw[1].dstSet=g_fsr.rcasSet;rw[1].dstBinding=2;rw[1].descriptorCount=1;rw[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;rw[1].pImageInfo=&rout;vkUpdateDescriptorSets(g_vkDevice,2,rw,0,nullptr);}
            VkImageMemoryBarrier pre[4]{}; int pc=0;
            auto ib=[&](VkImage image,VkImageLayout oldL,VkImageLayout newL,VkAccessFlags srcA,VkAccessFlags dstA){VkImageMemoryBarrier& x=pre[pc++];x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcAccessMask=srcA;x.dstAccessMask=dstA;x.oldLayout=oldL;x.newLayout=newL;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.image=image;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};};
            ib(src,srcLayout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            ib(useRcas?g_fsr.intermediate:g_fsr.output,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT);
            if(useRcas) ib(g_fsr.output,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT);
            ib(dst,dstLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,pc,pre);
            const uint32_t gx=(dstW+15)/16,gy=(dstH+15)/16;
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_fsr.easuPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_fsr.pipelineLayout,0,1,&g_fsr.easuSet,0,nullptr);vkCmdDispatch(cmd,gx,gy,1);
            if(useRcas){VkImageMemoryBarrier mid{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};mid.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mid.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;mid.oldLayout=VK_IMAGE_LAYOUT_GENERAL;mid.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;mid.srcQueueFamilyIndex=mid.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;mid.image=g_fsr.intermediate;mid.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&mid);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_fsr.rcasPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,g_fsr.pipelineLayout,0,1,&g_fsr.rcasSet,0,nullptr);vkCmdDispatch(cmd,gx,gy,1);}
            VkImageMemoryBarrier ready{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;ready.oldLayout=VK_IMAGE_LAYOUT_GENERAL;ready.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.image=g_fsr.output;ready.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ready);
            VkImageBlit bl{};bl.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};bl.srcOffsets[1]={(int32_t)dstW,(int32_t)dstH,1};bl.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};bl.dstOffsets[1]={(int32_t)dstW,(int32_t)dstH,1};vkCmdBlitImage(cmd,g_fsr.output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_NEAREST);
            VkImageMemoryBarrier restore[2]{};restore[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;restore[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;restore[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;restore[0].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;restore[0].newLayout=srcLayout;restore[0].srcQueueFamilyIndex=restore[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;restore[0].image=src;restore[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};restore[1]=restore[0];restore[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;restore[1].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;restore[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;restore[1].newLayout=dstLayout;restore[1].image=dst;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,restore);
            return srcView;
        }

        // DXVK copies its D3D9 VkImage into an exportable Win32-memory image; HatVR's
        // OpenXR VkDevice imports that allocation and copies it into the XR swapchain.
        struct Phase6EyeBridge
        {
            VkImage dxvkImage = VK_NULL_HANDLE;
            VkDeviceMemory dxvkMemory = VK_NULL_HANDLE;
            VkImage xrImage = VK_NULL_HANDLE;
            VkDeviceMemory xrMemory = VK_NULL_HANDLE;
            VkSemaphore dxvkSemaphore = VK_NULL_HANDLE;
            VkSemaphore xrSemaphore = VK_NULL_HANDLE;
            HANDLE memoryHandle = nullptr;
            HANDLE semaphoreHandle = nullptr;
            uint32_t width = 0, height = 0;
            bool initialized = false;
            bool externalOwnedByXr = false;
        };
        Phase6EyeBridge g_phase6Eye[3] = {};
        VkCommandPool g_phase6DxvkCommandPool = VK_NULL_HANDLE;
        VkCommandBuffer g_phase6DxvkCommandBuffer = VK_NULL_HANDLE;
        VkFence g_phase6DxvkFence = VK_NULL_HANDLE;
        bool g_phase6Unavailable = false;
        bool g_phase6LoggedActive = false;

        uint32_t FindMemoryTypeOn(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags wanted)
        {
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(physical, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted)
                    return i;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if (bits & (1u << i)) return i;
            return UINT32_MAX;
        }

        bool EnsurePhase6DxvkCommands()
        {
            if (g_phase6DxvkCommandPool && g_phase6DxvkCommandBuffer && g_phase6DxvkFence) return true;
            if (!g_dxvkProbeDevice || g_dxvkProbeQueueFamily == UINT32_MAX) return false;
            VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = g_dxvkProbeQueueFamily;
            if (vkCreateCommandPool(g_dxvkProbeDevice, &pci, nullptr, &g_phase6DxvkCommandPool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = g_phase6DxvkCommandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(g_dxvkProbeDevice, &cai, &g_phase6DxvkCommandBuffer) != VK_SUCCESS) return false;
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(g_dxvkProbeDevice, &fci, nullptr, &g_phase6DxvkFence) != VK_SUCCESS) return false;
            return true;
        }

        void DestroyPhase6EyeBridge(int slot)
        {
            if (slot < 0 || slot >= 3) return;
            auto& b = g_phase6Eye[slot];
            if (!b.initialized && !b.xrSemaphore && !b.dxvkSemaphore && !b.xrImage && !b.dxvkImage &&
                !b.xrMemory && !b.dxvkMemory && !b.memoryHandle && !b.semaphoreHandle)
                return;

            // this is only reached when the OpenXR output extent changes. Both bridge
            // queues are normally fence-complete every submitted eye, but wait here so
            // live resize can never destroy an image/semaphore still referenced by a GPU.
            if (g_vkDevice) vkDeviceWaitIdle(g_vkDevice);
            if (g_dxvkProbeDevice) vkDeviceWaitIdle(g_dxvkProbeDevice);
            if (b.xrSemaphore && g_vkDevice) vkDestroySemaphore(g_vkDevice, b.xrSemaphore, nullptr);
            if (b.dxvkSemaphore && g_dxvkProbeDevice) vkDestroySemaphore(g_dxvkProbeDevice, b.dxvkSemaphore, nullptr);
            if (b.xrImage && g_vkDevice) vkDestroyImage(g_vkDevice, b.xrImage, nullptr);
            if (b.xrMemory && g_vkDevice) vkFreeMemory(g_vkDevice, b.xrMemory, nullptr);
            if (b.dxvkImage && g_dxvkProbeDevice) vkDestroyImage(g_dxvkProbeDevice, b.dxvkImage, nullptr);
            if (b.dxvkMemory && g_dxvkProbeDevice) vkFreeMemory(g_dxvkProbeDevice, b.dxvkMemory, nullptr);
            if (b.memoryHandle) CloseHandle(b.memoryHandle);
            if (b.semaphoreHandle) CloseHandle(b.semaphoreHandle);
            b = {};
        }

        bool EnsurePhase6EyeBridge(int slot, uint32_t width, uint32_t height)
        {
            if (slot < 0 || slot >= 3 || g_phase6Unavailable) return false;
            auto& b = g_phase6Eye[slot];
            if (b.initialized && b.width == width && b.height == height) return true;
            if (b.initialized)
            {
                DxvkPathTrace("VKBRIDGE-V5 LIVE-RESIZE slot=%d old=%ux%u new=%ux%u",
                    slot, b.width, b.height, width, height);
                DestroyPhase6EyeBridge(slot);
            }
            if (!g_dxvkProbeDevice || !g_dxvkProbePhysical || !g_vkDevice || !g_vkPhysicalDevice) return false;
            if (!EnsurePhase6DxvkCommands()) return false;
            auto phase6Fail = [&]() -> bool {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 SHARED-IMAGE FAILED slot=%d; CPU fallback is DISABLED", slot);
                g_phase6Unavailable = true;
                return false;
            };

            const VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkExternalMemoryImageCreateInfo extImg{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            extImg.handleTypes = handleType;
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.pNext = &extImg;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_B8G8R8A8_UNORM;
            ici.extent = { width, height, 1 };
            ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(g_dxvkProbeDevice, &ici, nullptr, &b.dxvkImage) != VK_SUCCESS) return phase6Fail();

            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(g_dxvkProbeDevice, b.dxvkImage, &mr);
            uint32_t dxType = FindMemoryTypeOn(g_dxvkProbePhysical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (dxType == UINT32_MAX) return phase6Fail();
            VkExportMemoryAllocateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
            exportInfo.handleTypes = handleType;
            VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicated.pNext = &exportInfo;
            dedicated.image = b.dxvkImage;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.pNext = &dedicated; mai.allocationSize = mr.size; mai.memoryTypeIndex = dxType;
            if (vkAllocateMemory(g_dxvkProbeDevice, &mai, nullptr, &b.dxvkMemory) != VK_SUCCESS) return phase6Fail();
            if (vkBindImageMemory(g_dxvkProbeDevice, b.dxvkImage, b.dxvkMemory, 0) != VK_SUCCESS) return phase6Fail();

            auto dxGetMemHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(g_dxvkProbeDevice, "vkGetMemoryWin32HandleKHR"));
            if (!dxGetMemHandle) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=vkGetMemoryWin32HandleKHR-proc", slot);
                return phase6Fail();
            }
            VkMemoryGetWin32HandleInfoKHR ghi{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
            ghi.memory = b.dxvkMemory; ghi.handleType = handleType;
            VkResult getMemHandleResult = dxGetMemHandle(g_dxvkProbeDevice, &ghi, &b.memoryHandle);
            if (getMemHandleResult != VK_SUCCESS || !b.memoryHandle) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=vkGetMemoryWin32HandleKHR result=%d handle=%p", slot, (int)getMemHandleResult, b.memoryHandle);
                return phase6Fail();
            }

            // OPAQUE_WIN32 handles are deliberately opaque. Vulkan explicitly forbids
            // vkGetMemoryWin32HandlePropertiesKHR for opaque handle types, so do NOT query it.
            // Both logical devices are on the same physical NVIDIA adapter; use the exporter's
            // memory type when that type is also valid for the import-side image requirements.
            VkResult xrCreateBridgeImageResult = vkCreateImage(g_vkDevice, &ici, nullptr, &b.xrImage);
            if (xrCreateBridgeImageResult != VK_SUCCESS) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=xr-vkCreateImage result=%d", slot, (int)xrCreateBridgeImageResult);
                return phase6Fail();
            }
            VkMemoryRequirements xmr{};
            vkGetImageMemoryRequirements(g_vkDevice, b.xrImage, &xmr);
            uint32_t xrType = UINT32_MAX;
            if (dxType < 32 && (xmr.memoryTypeBits & (1u << dxType)))
                xrType = dxType;
            if (xrType == UINT32_MAX) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=opaque-memory-type dxType=%u xrBits=0x%08X", slot, dxType, xmr.memoryTypeBits);
                return phase6Fail();
            }
            DxvkPathTrace("VKBRIDGE-V5 PHASE6 OPAQUE-MEM slot=%d exportType=%u importType=%u exportSize=%llu importSize=%llu",
                slot, dxType, xrType, (unsigned long long)mr.size, (unsigned long long)xmr.size);
            VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            importInfo.handleType = handleType; importInfo.handle = b.memoryHandle;
            VkMemoryDedicatedAllocateInfo xrDedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            xrDedicated.pNext = &importInfo; xrDedicated.image = b.xrImage;
            if (xmr.size != mr.size) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=opaque-allocation-size export=%llu import=%llu", slot,
                    (unsigned long long)mr.size, (unsigned long long)xmr.size);
                return phase6Fail();
            }
            VkMemoryAllocateInfo xmai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            xmai.pNext = &xrDedicated; xmai.allocationSize = mr.size; xmai.memoryTypeIndex = xrType;
            VkResult xrAllocImportResult = vkAllocateMemory(g_vkDevice, &xmai, nullptr, &b.xrMemory);
            if (xrAllocImportResult != VK_SUCCESS) {
                DxvkPathTrace("VKBRIDGE-V5 PHASE6 FAIL slot=%d stage=xr-vkAllocateMemory-import result=%d", slot, (int)xrAllocImportResult);
                return phase6Fail();
            }
            if (vkBindImageMemory(g_vkDevice, b.xrImage, b.xrMemory, 0) != VK_SUCCESS) return phase6Fail();

            // Export one binary semaphore from DXVK and import it into the XR device.
            VkExportSemaphoreCreateInfo es{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
            es.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            sci.pNext = &es;
            if (vkCreateSemaphore(g_dxvkProbeDevice, &sci, nullptr, &b.dxvkSemaphore) != VK_SUCCESS) return phase6Fail();
            auto dxGetSemHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(g_dxvkProbeDevice, "vkGetSemaphoreWin32HandleKHR"));
            auto xrImportSem = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(g_vkDevice, "vkImportSemaphoreWin32HandleKHR"));
            if (!dxGetSemHandle || !xrImportSem) return phase6Fail();
            VkSemaphoreGetWin32HandleInfoKHR sghi{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
            sghi.semaphore = b.dxvkSemaphore; sghi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            if (dxGetSemHandle(g_dxvkProbeDevice, &sghi, &b.semaphoreHandle) != VK_SUCCESS || !b.semaphoreHandle) return phase6Fail();
            VkSemaphoreCreateInfo xsci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(g_vkDevice, &xsci, nullptr, &b.xrSemaphore) != VK_SUCCESS) return phase6Fail();
            VkImportSemaphoreWin32HandleInfoKHR ishi{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
            ishi.semaphore = b.xrSemaphore; ishi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            ishi.handle = b.semaphoreHandle;
            if (xrImportSem(g_vkDevice, &ishi) != VK_SUCCESS) return phase6Fail();

            b.width = width; b.height = height; b.initialized = true;
            DxvkPathTrace("VKBRIDGE-V5 PHASE6 SHARED-IMAGE READY slot=%d %ux%u dxImage=%p xrImage=%p (GPU-only)",
                slot, width, height, b.dxvkImage, b.xrImage);
            return true;
        }

        bool Phase6CopyDxvkTextureToOpenXrImage(
            IDirect3DTexture9* texture, VkImage dstImage, uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight, int slot, const XrFovf* projectionFov, bool nativeStereoFullEye = false)
        {
            bool wantNis = g_openXrUpscalingEnabled && slot >= 0 && slot < 2 && (dstWidth > srcWidth || dstHeight > srcHeight);
            if(wantNis && g_openXrUpscaler==0 && !EnsureNisOutput(dstWidth,dstHeight,(VkFormat)g_vkSwapchainFormat)) wantNis=false;
            if(wantNis && g_openXrUpscaler==1 && !EnsureFsrOutput(dstWidth,dstHeight)) wantNis=false;
            const uint32_t bridgeW = wantNis ? srcWidth : dstWidth;
            const uint32_t bridgeH = wantNis ? srcHeight : dstHeight;
            if (!texture || !dstImage || g_vkUsingDxvkDevice || !EnsurePhase6EyeBridge(slot, bridgeW, bridgeH) || !EnsureCommandResources())
                return false;
            auto& b = g_phase6Eye[slot];
            ID3D9VkInteropTextureHatVR* texInterop = nullptr;
            IDirect3DDevice9* d3dDevice = nullptr;
            ID3D9VkInteropDevicePhase5* devInterop = nullptr;
            VkImage srcImage = VK_NULL_HANDLE; VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageCreateInfo srcInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            if (FAILED(texture->QueryInterface(__uuidof(ID3D9VkInteropTextureHatVR), reinterpret_cast<void**>(&texInterop))) || !texInterop) goto fail;
            if (FAILED(texture->GetDevice(&d3dDevice)) || !d3dDevice) goto fail;
            if (FAILED(d3dDevice->QueryInterface(__uuidof(ID3D9VkInteropDevicePhase5), reinterpret_cast<void**>(&devInterop))) || !devInterop) goto fail;
            devInterop->FlushRenderingCommands();
            if (FAILED(texInterop->GetVulkanImageInfo(&srcImage, &srcLayout, &srcInfo)) || !srcImage) goto fail;

            // (width x height) or the ORIGINAL native SBS texture (2*width x
            // height). In the latter case both eye submissions reference the
            // same VkImage and select their own half below.
            const bool directSbs =
                slot >= 0 && slot < 2 &&
                srcInfo.extent.width == srcWidth * 2u &&
                srcInfo.extent.height == srcHeight;
            if (!directSbs &&
                (srcInfo.extent.width != srcWidth || srcInfo.extent.height != srcHeight))
            {
                DxvkPathTrace(
                    "VKBRIDGE-V5 PHASE8 SOURCE-SIZE REJECT slot=%d src=%ux%u expectedEye=%ux%u",
                    slot, srcInfo.extent.width, srcInfo.extent.height, srcWidth, srcHeight);
                goto fail;
            }

            // Stage A: DXVK queue copies source texture -> shared image, then releases it externally.
            devInterop->LockSubmissionQueue();
            vkResetFences(g_dxvkProbeDevice, 1, &g_phase6DxvkFence);
            vkResetCommandBuffer(g_phase6DxvkCommandBuffer, 0);
            {
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(g_phase6DxvkCommandBuffer, &bi) != VK_SUCCESS) goto dx_locked_fail;
                VkImageMemoryBarrier pre[2]{};
                pre[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; pre[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT; pre[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                pre[0].oldLayout=srcLayout; pre[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; pre[0].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; pre[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; pre[0].image=srcImage; pre[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                pre[1].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; pre[1].srcAccessMask=0; pre[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                pre[1].oldLayout=b.externalOwnedByXr ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED; pre[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                pre[1].srcQueueFamilyIndex=b.externalOwnedByXr ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED; pre[1].dstQueueFamilyIndex=g_dxvkProbeQueueFamily; pre[1].image=b.dxvkImage; pre[1].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdPipelineBarrier(g_phase6DxvkCommandBuffer,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,pre);
                if (nativeStereoFullEye && slot >= 0 && slot < 2)
                {
                    // native Stereo now folds the old horizontal crop into
                    // each eye's projection. Copy the complete rendered eye here.
                    const int32_t sourceX = directSbs ? ((int32_t)slot * (int32_t)srcWidth) : 0;
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.srcOffsets[0] = {sourceX,0,0};
                    blit.srcOffsets[1] = {sourceX + (int32_t)srcWidth,(int32_t)srcHeight,1};
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.dstOffsets[0] = {0,0,0};
                    blit.dstOffsets[1] = {(int32_t)b.width,(int32_t)b.height,1};
                    vkCmdBlitImage(g_phase6DxvkCommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        b.dxvkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                    if (g_vkSubmitSerial <= 12 || (g_vkSubmitSerial % 300ULL) == 0)
                        DxvkPathTrace(
                            "R261 NATIVE FULL-EYE eye=%d direct=%d srcPx=[%d,0 -> %d,%u]",
                            slot, directSbs ? 1 : 0, sourceX, sourceX + (int32_t)srcWidth, srcHeight);
                }
                else if (projectionFov && slot >= 0 && slot < 2)
                {
                    // a symmetric frustum that covers the asymmetric XR eye, then
                    // we crop that image to the runtime frustum entirely on-GPU.
                    const float tanL = tanf(projectionFov->angleLeft);
                    const float tanR = tanf(projectionFov->angleRight);
                    const float tanU = tanf(projectionFov->angleUp);
                    const float tanD = tanf(projectionFov->angleDown);
                    const float sourceHalfX = (std::max)(fabsf(tanL), fabsf(tanR));
                    const float scaleX = (tanR - tanL) / (2.0f * sourceHalfX);
                    const float scaleY = 1.0f;
                    const float offsetX = (tanL + sourceHalfX) / (2.0f * sourceHalfX);
                    // do not crop or recenter vertically.
                    const float offsetY = 0.0f;

                    const int32_t eyeBaseX = directSbs ? ((int32_t)slot * (int32_t)srcWidth) : 0;
                    int32_t localX0 = (int32_t)floorf(offsetX * (float)srcWidth);
                    int32_t y0 = (int32_t)floorf(offsetY * (float)srcHeight);
                    int32_t localX1 = (int32_t)ceilf((offsetX + scaleX) * (float)srcWidth);
                    int32_t y1 = (int32_t)ceilf((offsetY + scaleY) * (float)srcHeight);
                    localX0 = (std::max)(0, (std::min)(localX0, (int32_t)srcWidth - 1));
                    y0 = (std::max)(0, (std::min)(y0, (int32_t)srcHeight - 1));
                    localX1 = (std::max)(localX0 + 1, (std::min)(localX1, (int32_t)srcWidth));
                    y1 = (std::max)(y0 + 1, (std::min)(y1, (int32_t)srcHeight));
                    const int32_t x0 = eyeBaseX + localX0;
                    const int32_t x1 = eyeBaseX + localX1;

                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.srcOffsets[0] = {x0,y0,0};
                    blit.srcOffsets[1] = {x1,y1,1};
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.dstOffsets[0] = {0,0,0};
                    blit.dstOffsets[1] = {(int32_t)b.width,(int32_t)b.height,1};
                    vkCmdBlitImage(g_phase6DxvkCommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        b.dxvkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                    if (g_vkSubmitSerial <= 12 || (g_vkSubmitSerial % 300ULL) == 0)
                        DxvkPathTrace(
                            "VKBRIDGE-V5 PHASE8 DIRECT-SBS-CROP eye=%d direct=%d uv=[%.5f %.5f %.5f %.5f] srcPx=[%d,%d -> %d,%d] eyeBaseX=%d sourceHalfX=%.5f",
                            slot, directSbs ? 1 : 0, offsetX, offsetY, scaleX, scaleY,
                            x0, y0, x1, y1, eyeBaseX, sourceHalfX);
                }
                else
                {
                    if (!projectionFov && slot >= 0 && slot < 2)
                    {
                        // Theater presentation is a 16:9 physical quad. This crop
                        // must apply to BOTH source layouts:
                        //   Stereo: native Direct-SBS (two eyes in one texture)
                        //   Sequential: independent per-eye textures
                        //
                        // the old condition required directSbs, so Sequential fell
                        // through to a full 1344x1440 -> 16:9 stretch and looked
                        // vertically squashed.
                        // quad distorts it. Instead, retain the full horizontal eye
                        // image and center-crop vertically to a true 16:9 source.
                        //
                        // 1344 / (16/9) = 756 source pixels high at the current
                        // runtime resolution. We then scale that crop into the
                        // existing eye swapchain; normalized sampling onto the 16:9
                        // quad restores the correct geometry without changing XR
                        // swapchain allocation.
                        const float targetAspect = 16.0f / 9.0f;
                        int32_t cropW = (int32_t)srcWidth;
                        int32_t cropH = (int32_t)floorf((float)cropW / targetAspect + 0.5f);
                        cropH = (std::max)(1, (std::min)(cropH, (int32_t)srcHeight));

                        const int32_t eyeBaseX =
                            directSbs ? ((int32_t)slot * (int32_t)srcWidth) : 0;
                        const int32_t cropY = ((int32_t)srcHeight - cropH) / 2;

                        VkImageBlit blit{};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                        blit.srcOffsets[0] = {eyeBaseX, cropY, 0};
                        blit.srcOffsets[1] = {eyeBaseX + cropW, cropY + cropH, 1};
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                        blit.dstOffsets[0] = {0,0,0};
                        blit.dstOffsets[1] = {(int32_t)b.width,(int32_t)b.height,1};

                        vkCmdBlitImage(
                            g_phase6DxvkCommandBuffer,
                            srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            b.dxvkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &blit, VK_FILTER_LINEAR);

                        static unsigned int s_theaterCropLogs = 0;
                        if (s_theaterCropLogs < 12)
                        {
                            ++s_theaterCropLogs;
                            DxvkPathTrace(
                                "THEATER 16:9 CROP eye=%d src=[%d,%d -> %d,%d] srcEye=%ux%u crop=%dx%d",
                                slot,
                                eyeBaseX, cropY,
                                eyeBaseX + cropW, cropY + cropH,
                                srcWidth, srcHeight, cropW, cropH);
                        }
                    }
                    else
                    {
                        VkImageBlit blit{};
                        blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                        blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                        const int32_t sourceX = directSbs ? ((int32_t)slot * (int32_t)srcWidth) : 0;
                        blit.srcOffsets[0] = {sourceX,0,0};
                        blit.srcOffsets[1] = {sourceX+(int32_t)srcWidth,(int32_t)srcHeight,1};
                        blit.dstOffsets[0] = {0,0,0};
                        blit.dstOffsets[1] = {(int32_t)b.width,(int32_t)b.height,1};
                        vkCmdBlitImage(g_phase6DxvkCommandBuffer,srcImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.dxvkImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
                    }
                }
                VkImageMemoryBarrier post[2]{};
                post[0]=pre[0]; post[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; post[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; post[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; post[0].newLayout=srcLayout;
                post[1]=pre[1]; post[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; post[1].dstAccessMask=0; post[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; post[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; post[1].srcQueueFamilyIndex=g_dxvkProbeQueueFamily; post[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
                vkCmdPipelineBarrier(g_phase6DxvkCommandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,post);
                if (vkEndCommandBuffer(g_phase6DxvkCommandBuffer)!=VK_SUCCESS) goto dx_locked_fail;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&g_phase6DxvkCommandBuffer; si.signalSemaphoreCount=1; si.pSignalSemaphores=&b.dxvkSemaphore;
                if (vkQueueSubmit(g_dxvkProbeQueue,1,&si,g_phase6DxvkFence)!=VK_SUCCESS) goto dx_locked_fail;
            }
            devInterop->ReleaseSubmissionQueue();

            // Stage B: XR queue waits entirely on-GPU, acquires shared image, copies -> swapchain.
            vkResetFences(g_vkDevice,1,&g_vkFence); vkResetCommandBuffer(g_vkCommandBuffer,0);
            {
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if(vkBeginCommandBuffer(g_vkCommandBuffer,&bi)!=VK_SUCCESS) goto fail;
                VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; acquire.srcAccessMask=0; acquire.dstAccessMask=wantNis?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT; acquire.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; acquire.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; acquire.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL; acquire.dstQueueFamilyIndex=g_vkQueueFamily; acquire.image=b.xrImage; acquire.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdPipelineBarrier(g_vkCommandBuffer,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,wantNis?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&acquire);
                VkImageView nisTempView=VK_NULL_HANDLE;
                if(wantNis)
                {
                    nisTempView=(g_openXrUpscaler==1) ? RecordFsrUpscale(g_vkCommandBuffer,b.xrImage,VK_FORMAT_B8G8R8A8_UNORM,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.width,b.height,0,0,b.width,b.height,dstImage,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,dstWidth,dstHeight) : RecordNisUpscale(g_vkCommandBuffer,b.xrImage,VK_FORMAT_B8G8R8A8_UNORM,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.width,b.height,0,0,b.width,b.height,dstImage,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,dstWidth,dstHeight);
                    if(!nisTempView) goto fail;
                }
                else
                {
                    VkImageMemoryBarrier d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; d.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; d.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; d.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.srcQueueFamilyIndex=d.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; d.image=dstImage; d.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    vkCmdPipelineBarrier(g_vkCommandBuffer,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&d);
                    VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.extent={dstWidth,dstHeight,1}; vkCmdCopyImage(g_vkCommandBuffer,b.xrImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dstImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
                    d.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; d.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT; d.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; d.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    vkCmdPipelineBarrier(g_vkCommandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&d);
                }
                VkImageMemoryBarrier release=acquire; release.srcAccessMask=wantNis?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT; release.dstAccessMask=0; release.srcQueueFamilyIndex=g_vkQueueFamily; release.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
                vkCmdPipelineBarrier(g_vkCommandBuffer,wantNis?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&release);
                if(vkEndCommandBuffer(g_vkCommandBuffer)!=VK_SUCCESS) goto fail;
                VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount=1; si.pWaitSemaphores=&b.xrSemaphore; si.pWaitDstStageMask=&waitStage; si.commandBufferCount=1; si.pCommandBuffers=&g_vkCommandBuffer;
                if(vkQueueSubmit(g_vkQueue,1,&si,g_vkFence)!=VK_SUCCESS) goto fail;
                if(vkWaitForFences(g_vkDevice,1,&g_vkFence,VK_TRUE,UINT64_MAX)!=VK_SUCCESS) goto fail;
                if(nisTempView) vkDestroyImageView(g_vkDevice,nisTempView,nullptr);
            }
            b.externalOwnedByXr=true;
            if(!g_phase6LoggedActive){ DxvkPathTrace("VKBRIDGE-V5 PHASE8 DIRECT-SBS GPU-ONLY ACTIVE - original SBS VkImage, no D3D9 eye StretchRects, no CPU pixels"); g_phase6LoggedActive=true; }
            if (g_vkSubmitSerial <= 12 || (g_vkSubmitSerial % 300ULL)==0)
                DxvkPathTrace("VKBRIDGE-V5 PHASE8 GPU-COPY slot=%d directSbs=%d src=%p srcExtent=%ux%u shared=%p xrShared=%p dst=%p eye=%ux%u",slot,directSbs?1:0,srcImage,srcInfo.extent.width,srcInfo.extent.height,b.dxvkImage,b.xrImage,dstImage,dstWidth,dstHeight);
            devInterop->Release(); d3dDevice->Release(); texInterop->Release(); return true;
        dx_locked_fail:
            devInterop->ReleaseSubmissionQueue();
        fail:
            if(devInterop)devInterop->Release(); if(d3dDevice)d3dDevice->Release(); if(texInterop)texInterop->Release();
            return false;
        }

        bool CopyDxvkTextureToOpenXrImage(
            IDirect3DTexture9* texture, VkImage dstImage, uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight, int slot)
        {
            if (!g_vkUsingDxvkDevice || !texture || !dstImage || !EnsureCommandResources())
                return false;

            ID3D9VkInteropTextureHatVR* texInterop = nullptr;
            IDirect3DDevice9* d3dDevice = nullptr;
            ID3D9VkInteropDevicePhase5* devInterop = nullptr;
            VkImage srcImage = VK_NULL_HANDLE;
            VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageCreateInfo srcInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            if (FAILED(texture->QueryInterface(__uuidof(ID3D9VkInteropTextureHatVR), reinterpret_cast<void**>(&texInterop))) || !texInterop)
                goto fail;
            if (FAILED(texture->GetDevice(&d3dDevice)) || !d3dDevice)
                goto fail;
            if (FAILED(d3dDevice->QueryInterface(__uuidof(ID3D9VkInteropDevicePhase5), reinterpret_cast<void**>(&devInterop))) || !devInterop)
                goto fail;

            // DXVK explicitly requires this before Vulkan commands touch a D3D9 resource.
            devInterop->FlushRenderingCommands();

            if (FAILED(texInterop->GetVulkanImageInfo(&srcImage, &srcLayout, &srcInfo)) || !srcImage)
                goto fail;
            const bool directSbs = slot >= 0 && slot < 2 &&
                srcInfo.extent.width == srcWidth * 2u && srcInfo.extent.height == srcHeight;
            if (!directSbs && (srcInfo.extent.width != srcWidth || srcInfo.extent.height != srcHeight))
            {
                DxvkPathTrace("VKBRIDGE-V5 PHASE5 DIRECT size mismatch src=%ux%u expectedEye=%ux%u slot=%d",
                    srcInfo.extent.width, srcInfo.extent.height, srcWidth, srcHeight, slot);
                goto fail;
            }

            bool canNis = g_openXrUpscalingEnabled && slot >= 0 && slot < 2 && (dstWidth > srcWidth || dstHeight > srcHeight);

            // No D3D9 calls are made while the DXVK submission queue is locked.
            devInterop->LockSubmissionQueue();
            {
                if(canNis && g_openXrUpscaler==0 && !EnsureNisOutput(dstWidth,dstHeight,(VkFormat)g_vkSwapchainFormat)) canNis=false;
                if(canNis && g_openXrUpscaler==1 && !EnsureFsrOutput(dstWidth,dstHeight)) canNis=false;
                vkResetFences(g_vkDevice, 1, &g_vkFence);
                vkResetCommandBuffer(g_vkCommandBuffer, 0);
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(g_vkCommandBuffer, &bi) != VK_SUCCESS) goto locked_fail;

                VkImageView nisTempView = VK_NULL_HANDLE;
                bool recordedNis = false;
                const bool wantNis = canNis;
                if (wantNis)
                {
                    const uint32_t sourceX = directSbs ? (uint32_t)slot * srcWidth : 0u;
                    nisTempView = (g_openXrUpscaler==1) ? RecordFsrUpscale(g_vkCommandBuffer, srcImage, srcInfo.format, srcLayout,
                        srcInfo.extent.width, srcInfo.extent.height, sourceX, 0, srcWidth, srcHeight,
                        dstImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dstWidth, dstHeight) : RecordNisUpscale(g_vkCommandBuffer, srcImage, srcInfo.format, srcLayout,
                        srcInfo.extent.width, srcInfo.extent.height, sourceX, 0, srcWidth, srcHeight,
                        dstImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dstWidth, dstHeight);
                    if (nisTempView) recordedNis = true;
                    else DxvkPathTrace("SCALER-V1 same-device dispatch unavailable (upscaler=%d); falling back to linear for slot=%d", g_openXrUpscaler, slot);

                }

                if(!recordedNis)
                {
                    VkImageMemoryBarrier barriers[2]{};
                    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    barriers[0].oldLayout = srcLayout;
                    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[0].image = srcImage;
                    barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[1].image = dstImage;
                    barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                    vkCmdPipelineBarrier(g_vkCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 2, barriers);

                    VkImageBlit blit{};
                    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    const int32_t sourceX = directSbs ? (int32_t)slot * (int32_t)srcWidth : 0;
                    blit.srcOffsets[0] = { sourceX, 0, 0 };
                    blit.srcOffsets[1] = { sourceX + (int32_t)srcWidth, (int32_t)srcHeight, 1 };
                    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    blit.dstOffsets[0] = { 0, 0, 0 };
                    blit.dstOffsets[1] = { (int32_t)dstWidth, (int32_t)dstHeight, 1 };
                    vkCmdBlitImage(g_vkCommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                    VkImageMemoryBarrier restore[2] = { barriers[0], barriers[1] };
                    restore[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    restore[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    restore[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    restore[0].newLayout = srcLayout;
                    restore[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    restore[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                    restore[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    restore[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    vkCmdPipelineBarrier(g_vkCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        0, 0, nullptr, 0, nullptr, 2, restore);

                }

                if (vkEndCommandBuffer(g_vkCommandBuffer) != VK_SUCCESS) goto locked_fail;
                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &g_vkCommandBuffer;
                if (vkQueueSubmit(g_vkQueue, 1, &si, g_vkFence) != VK_SUCCESS) goto locked_fail;
                if (vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) goto locked_fail;
                if(nisTempView) vkDestroyImageView(g_vkDevice,nisTempView,nullptr);
                devInterop->ReleaseSubmissionQueue();
                DxvkPathTrace("VKBRIDGE-V5 PHASE5 DIRECT-GPU copy src=%p layout=%d dst=%p %ux%u",
                    srcImage, (int)srcLayout, dstImage, dstWidth, dstHeight);
                devInterop->Release(); d3dDevice->Release(); texInterop->Release();
                return true;
            }

        locked_fail:
            devInterop->ReleaseSubmissionQueue();
        fail:
            if (devInterop) devInterop->Release();
            if (d3dDevice) d3dDevice->Release();
            if (texInterop) texInterop->Release();
            return false;
        }

        void PollEvents()
        {
            if (!g_vkXrInstance) return;
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(g_vkXrInstance, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    const auto* changed =
                        reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    g_vkSessionState = changed->state;
                    DxvkPathTrace("VKXR-V8 SESSION STATE=%d", (int)g_vkSessionState);

                    if (changed->state == XR_SESSION_STATE_READY && !g_vkSessionRunning)
                    {
                        XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                        bi.primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        const XrResult xr = xrBeginSession(g_vkXrSession, &bi);
                        DxvkPathTrace("VKXR-V8 xrBeginSession result=%d", (int)xr);
                        if (XR_SUCCEEDED(xr)) g_vkSessionRunning = true;
                    }
                    else if (changed->state == XR_SESSION_STATE_STOPPING &&
                             g_vkSessionRunning)
                    {
                        xrEndSession(g_vkXrSession);
                        g_vkSessionRunning = false;
                    }
                    else if (changed->state == XR_SESSION_STATE_EXITING ||
                             changed->state == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        g_vkExitRequested = true;
                    }
                }
                event = { XR_TYPE_EVENT_DATA_BUFFER };
            }
        }
    }

    bool IsVulkanBackendActive()
    {
        return g_vkXrSession != XR_NULL_HANDLE;
    }

    void SetDxvkVulkanContextForOpenXRProbe(
        VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
        VkQueue queue, uint32_t queueFamily, uint32_t queueIndex)
    {
        g_dxvkProbeInstance = instance;
        g_dxvkProbePhysical = physicalDevice;
        g_dxvkProbeDevice = device;
        g_dxvkProbeQueue = queue;
        g_dxvkProbeQueueFamily = queueFamily;
        g_dxvkProbeQueueIndex = queueIndex;
        DxvkPathTrace(
            "VKBRIDGE-V5 PHASE4A STORED-DXVK instance=%p physical=%p device=%p queue=%p family=%u index=%u",
            instance, physicalDevice, device, queue, queueFamily, queueIndex);
    }

    bool ActivateVulkanBackend()
    {
        if (IsVulkanBackendActive()) return true;

        DxvkPathTrace("VKBRIDGE-V5 PHASE8.3 DIRECT-SBS+UI-FIX BUILD-STAMP 2026-09-20G vulkan_backend.cpp ACTIVE");
        DxvkPathTrace("VKXR-V8 ActivateVulkanBackend ENTER");
        uint32_t extCount = 0;
        XrResult xr = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
        if (XR_FAILED(xr)) { LogXrFailure("xrEnumerateInstanceExtensionProperties(count)", xr); return false; }

        std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
        xr = xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
        if (XR_FAILED(xr)) { LogXrFailure("xrEnumerateInstanceExtensionProperties(list)", xr); return false; }
        const bool haveVulkan1 = HasExtension(exts, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        const bool haveVulkan2 = HasExtension(exts, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        if (!haveVulkan1 || !haveVulkan2)
        {
            DxvkPathTrace("VKXR-V8 Vulkan extensions unavailable enable1=%d enable2=%d", haveVulkan1 ? 1 : 0, haveVulkan2 ? 1 : 0);
            return false;
        }

        // the borrowed DXVK device must use XR_KHR_vulkan_enable. enable2 requires
        // the VkInstance/VkDevice to be created through OpenXR, which DXVK's are not.
        // keep enable2 enabled only for HatVR's separate-device fallback below.
        const char* enabledExtensions[] = {
            XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
            XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
        };
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        std::strncpy(ici.applicationInfo.applicationName, "A Hat in Time (VR)", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(ici.applicationInfo.engineName, "Unreal Engine 3 / HatVR", XR_MAX_ENGINE_NAME_SIZE - 1);
        ici.applicationInfo.applicationVersion = 1;
        ici.applicationInfo.engineVersion = 1;
        // conservative OpenXR request.
        ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ici.enabledExtensionCount = 2;
        ici.enabledExtensionNames = enabledExtensions;

        xr = xrCreateInstance(&ici, &g_vkXrInstance);
        if (XR_FAILED(xr)) { LogXrFailure("xrCreateInstance", xr); g_vkXrInstance = XR_NULL_HANDLE; return false; }

        XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_vkXrInstance, &ip)))
        {
            strncpy_s(g_vkRuntimeName, ip.runtimeName, _TRUNCATE);
            g_vkRuntimeMajor=XR_VERSION_MAJOR(ip.runtimeVersion);
            g_vkRuntimeMinor=XR_VERSION_MINOR(ip.runtimeVersion);
            g_vkRuntimePatch=XR_VERSION_PATCH(ip.runtimeVersion);
            DxvkPathTrace("VKXR-V8 runtime='%s' version=%u.%u.%u", ip.runtimeName, g_vkRuntimeMajor, g_vkRuntimeMinor, g_vkRuntimePatch);
        }

        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr = xrGetSystem(g_vkXrInstance, &sgi, &g_vkXrSystemId);
        if (XR_FAILED(xr)) { LogXrFailure("xrGetSystem", xr); ShutdownVulkanBackend(); return false; }

        XrSystemProperties sysProps{ XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties(g_vkXrInstance, g_vkXrSystemId, &sysProps)))
            strncpy_s(g_vkSystemName, sysProps.systemName, _TRUNCATE);

        // drives the native SBS target and OpenXR eye swapchains.
        uint32_t vkViewCount = 0;
        xr = xrEnumerateViewConfigurationViews(
            g_vkXrInstance, g_vkXrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &vkViewCount, nullptr);
        if (XR_FAILED(xr) || vkViewCount < 2)
        {
            LogXrFailure("xrEnumerateViewConfigurationViews(count)", xr);
            ShutdownVulkanBackend(); return false;
        }
        std::vector<XrViewConfigurationView> vkViewConfig(vkViewCount);
        for (auto& v : vkViewConfig) v = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
        xr = xrEnumerateViewConfigurationViews(
            g_vkXrInstance, g_vkXrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            vkViewCount, &vkViewCount, vkViewConfig.data());
        if (XR_FAILED(xr) || vkViewCount < 2)
        {
            LogXrFailure("xrEnumerateViewConfigurationViews(list)", xr);
            ShutdownVulkanBackend(); return false;
        }
        g_vkRecommendedEyeWidth = vkViewConfig[0].recommendedImageRectWidth;
        g_vkRecommendedEyeHeight = vkViewConfig[0].recommendedImageRectHeight;
        g_vkMaxEyeWidth = vkViewConfig[0].maxImageRectWidth;
        g_vkMaxEyeHeight = vkViewConfig[0].maxImageRectHeight;
        DxvkPathTrace(
            "VKXR-V8 recommended eyes LEFT=%ux%u samples=%u RIGHT=%ux%u samples=%u",
            vkViewConfig[0].recommendedImageRectWidth,
            vkViewConfig[0].recommendedImageRectHeight,
            vkViewConfig[0].recommendedSwapchainSampleCount,
            vkViewConfig[1].recommendedImageRectWidth,
            vkViewConfig[1].recommendedImageRectHeight,
            vkViewConfig[1].recommendedSwapchainSampleCount);

        PFN_xrGetVulkanGraphicsRequirementsKHR xrGetReq1 = nullptr;
        PFN_xrGetVulkanGraphicsDeviceKHR xrGetVkDevice1 = nullptr;
        PFN_xrGetVulkanInstanceExtensionsKHR xrGetVkInstanceExts1 = nullptr;
        PFN_xrGetVulkanDeviceExtensionsKHR xrGetVkDeviceExts1 = nullptr;
        PFN_xrGetVulkanGraphicsRequirements2KHR xrGetReq = nullptr;
        PFN_xrCreateVulkanInstanceKHR xrCreateVkInstance = nullptr;
        PFN_xrGetVulkanGraphicsDevice2KHR xrGetVkDevice = nullptr;
        PFN_xrCreateVulkanDeviceKHR xrCreateVkLogicalDevice = nullptr;
#define GET_XR_PROC(n, v) do { xr = xrGetInstanceProcAddr(g_vkXrInstance, n, reinterpret_cast<PFN_xrVoidFunction*>(&(v))); if (XR_FAILED(xr) || !(v)) { LogXrFailure("xrGetInstanceProcAddr(" n ")", xr); ShutdownVulkanBackend(); return false; } } while (0)
        GET_XR_PROC("xrGetVulkanGraphicsRequirementsKHR", xrGetReq1);
        GET_XR_PROC("xrGetVulkanGraphicsDeviceKHR", xrGetVkDevice1);
        GET_XR_PROC("xrGetVulkanInstanceExtensionsKHR", xrGetVkInstanceExts1);
        GET_XR_PROC("xrGetVulkanDeviceExtensionsKHR", xrGetVkDeviceExts1);
        GET_XR_PROC("xrGetVulkanGraphicsRequirements2KHR", xrGetReq);
        GET_XR_PROC("xrCreateVulkanInstanceKHR", xrCreateVkInstance);
        GET_XR_PROC("xrGetVulkanGraphicsDevice2KHR", xrGetVkDevice);
        GET_XR_PROC("xrCreateVulkanDeviceKHR", xrCreateVkLogicalDevice);
#undef GET_XR_PROC

        XrGraphicsRequirementsVulkan2KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        xr = xrGetReq(g_vkXrInstance, g_vkXrSystemId, &req);
        if (XR_FAILED(xr)) { LogXrFailure("xrGetVulkanGraphicsRequirements2KHR", xr); ShutdownVulkanBackend(); return false; }

        DxvkPathTrace("VKBRIDGE-V5 PHASE4A XR-REQ minApi=%u.%u.%u maxApi=%u.%u.%u",
            XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_PATCH(req.minApiVersionSupported),
            XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported), XR_VERSION_PATCH(req.maxApiVersionSupported));

        if (g_dxvkProbeInstance != VK_NULL_HANDLE && g_dxvkProbePhysical != VK_NULL_HANDLE)
        {
            XrVulkanGraphicsDeviceGetInfoKHR probeInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
            probeInfo.systemId = g_vkXrSystemId;
            probeInfo.vulkanInstance = g_dxvkProbeInstance;
            VkPhysicalDevice xrPhysicalInDxvkInstance = VK_NULL_HANDLE;
            const XrResult probeXr = xrGetVkDevice(g_vkXrInstance, &probeInfo, &xrPhysicalInDxvkInstance);
            DxvkPathTrace(
                "VKBRIDGE-V5 PHASE4A XR-PHYSICAL-FOR-DXVK-INSTANCE xr=%d runtimePhysical=%p dxvkPhysical=%p MATCH=%s",
                (int)probeXr, xrPhysicalInDxvkInstance, g_dxvkProbePhysical,
                (XR_SUCCEEDED(probeXr) && xrPhysicalInDxvkInstance == g_dxvkProbePhysical) ? "YES" : "NO");

            HMODULE probeVulkan = GetModuleHandleA("vulkan-1.dll");
            auto probeGip = probeVulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(probeVulkan, "vkGetInstanceProcAddr")) : nullptr;
            if (probeGip)
            {
                auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(probeGip(g_dxvkProbeInstance, "vkGetPhysicalDeviceProperties"));
                if (getProps)
                {
                    VkPhysicalDeviceProperties props{};
                    getProps(g_dxvkProbePhysical, &props);
                    DxvkPathTrace(
                        "VKBRIDGE-V5 PHASE4A DXVK-PHYSICAL name=%s api=%u.%u.%u driver=0x%08X vendor=0x%04X deviceId=0x%04X",
                        props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
                        props.driverVersion, props.vendorID, props.deviceID);
                }
            }
            DxvkPathTrace(
                "VKBRIDGE-V5 PHASE4A DXVK-QUEUE device=%p queue=%p family=%u index=%u (probe only; no HatVR submissions)",
                g_dxvkProbeDevice, g_dxvkProbeQueue, g_dxvkProbeQueueFamily, g_dxvkProbeQueueIndex);
        }
        else
        {
            DxvkPathTrace("VKBRIDGE-V5 PHASE4A no DXVK Vulkan context was supplied before OpenXR activation");
        }

        // Borrowed DXVK Vulkan objects must use the original Vulkan binding.
        // VirtualDesktopXR requires Vulkan instance extensions that DXVK's existing
        // instance was not created with. Use HatVR's OpenXR-owned Vulkan device so
        // the existing external-memory bridge can move the rendered eyes across.
        const bool forceSeparateXrDevice = _stricmp(g_vkRuntimeName, "VirtualDesktopXR") == 0;
        if (forceSeparateXrDevice)
            DxvkPathTrace("VKBRIDGE-V5 PHASE5 runtime=%s forcing OpenXR-owned Vulkan device + GPU bridge", g_vkRuntimeName);

        // enable2 is only valid when OpenXR created the instance and device.
        if (!forceSeparateXrDevice && g_dxvkProbeInstance && g_dxvkProbePhysical && g_dxvkProbeDevice && g_dxvkProbeQueue)
        {
            XrGraphicsRequirementsVulkanKHR req1{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
            const XrResult req1Xr = xrGetReq1(g_vkXrInstance, g_vkXrSystemId, &req1);

            uint32_t instanceExtBytes = 0, deviceExtBytes = 0;
            XrResult instanceExtXr = xrGetVkInstanceExts1(g_vkXrInstance, g_vkXrSystemId, 0, &instanceExtBytes, nullptr);
            XrResult deviceExtXr = xrGetVkDeviceExts1(g_vkXrInstance, g_vkXrSystemId, 0, &deviceExtBytes, nullptr);
            std::vector<char> instanceExts(instanceExtBytes ? instanceExtBytes : 1, 0);
            std::vector<char> deviceExts(deviceExtBytes ? deviceExtBytes : 1, 0);
            if (XR_SUCCEEDED(instanceExtXr) && instanceExtBytes)
                instanceExtXr = xrGetVkInstanceExts1(g_vkXrInstance, g_vkXrSystemId, instanceExtBytes, &instanceExtBytes, instanceExts.data());
            if (XR_SUCCEEDED(deviceExtXr) && deviceExtBytes)
                deviceExtXr = xrGetVkDeviceExts1(g_vkXrInstance, g_vkXrSystemId, deviceExtBytes, &deviceExtBytes, deviceExts.data());

            VkPhysicalDevice runtimePhysical = VK_NULL_HANDLE;
            const XrResult physicalXr = xrGetVkDevice1(g_vkXrInstance, g_vkXrSystemId, g_dxvkProbeInstance, &runtimePhysical);
            DxvkPathTrace(
                "VKBRIDGE-V5 PHASE5 ENABLE1 req=%d physicalXr=%d runtimePhysical=%p dxvkPhysical=%p MATCH=%s",
                (int)req1Xr, (int)physicalXr, runtimePhysical, g_dxvkProbePhysical,
                (XR_SUCCEEDED(physicalXr) && runtimePhysical == g_dxvkProbePhysical) ? "YES" : "NO");
            DxvkPathTrace("VKBRIDGE-V5 PHASE5 ENABLE1 required-instance-exts='%s'",
                XR_SUCCEEDED(instanceExtXr) ? instanceExts.data() : "<query failed>");
            DxvkPathTrace("VKBRIDGE-V5 PHASE5 ENABLE1 required-device-exts='%s'",
                XR_SUCCEEDED(deviceExtXr) ? deviceExts.data() : "<query failed>");

            if (XR_SUCCEEDED(req1Xr) && XR_SUCCEEDED(physicalXr) && runtimePhysical == g_dxvkProbePhysical &&
                XR_SUCCEEDED(instanceExtXr) && XR_SUCCEEDED(deviceExtXr))
            {
                XrGraphicsBindingVulkanKHR dxvkBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
                dxvkBinding.instance = g_dxvkProbeInstance;
                dxvkBinding.physicalDevice = g_dxvkProbePhysical;
                dxvkBinding.device = g_dxvkProbeDevice;
                dxvkBinding.queueFamilyIndex = g_dxvkProbeQueueFamily;
                dxvkBinding.queueIndex = g_dxvkProbeQueueIndex;
                XrSessionCreateInfo dxvkSci{ XR_TYPE_SESSION_CREATE_INFO };
                dxvkSci.next = &dxvkBinding;
                dxvkSci.systemId = g_vkXrSystemId;
                XrSession dxvkSession = XR_NULL_HANDLE;
                const XrResult directXr = xrCreateSession(g_vkXrInstance, &dxvkSci, &dxvkSession);
                DxvkPathTrace("VKBRIDGE-V5 PHASE5 DIRECT-XR-SESSION enable1 xr=%d session=%p",
                    (int)directXr, dxvkSession);
                if (XR_SUCCEEDED(directXr) && dxvkSession != XR_NULL_HANDLE)
                {
                    g_vkXrSession = dxvkSession;
                    g_vkInstance = g_dxvkProbeInstance;
                    g_vkPhysicalDevice = g_dxvkProbePhysical;
                    g_vkDevice = g_dxvkProbeDevice;
                    g_vkQueue = g_dxvkProbeQueue;
                    g_vkQueueFamily = g_dxvkProbeQueueFamily;
                    g_vkQueueIndex = g_dxvkProbeQueueIndex;
                    g_vkOwnsInstance = false;
                    g_vkOwnsDevice = false;
                    g_vkUsingDxvkDevice = true;
                    DxvkPathTrace("VKBRIDGE-V5 PHASE5 DIRECT-XR-SESSION SUCCESS - legal enable1 binding shares DXVK VkDevice");
                }
            }
        }

        HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
        if (!vulkan) vulkan = LoadLibraryA("vulkan-1.dll");
        auto gip = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
        if (!gip) { DxvkPathTrace("VKXR-V8 vkGetInstanceProcAddr unavailable"); ShutdownVulkanBackend(); return false; }

        uint32_t vkApi = VK_API_VERSION_1_1;
        const uint32_t reqApi = VK_MAKE_API_VERSION(
            0,
            XR_VERSION_MAJOR(req.minApiVersionSupported),
            XR_VERSION_MINOR(req.minApiVersionSupported), 0);
        if (reqApi > vkApi) vkApi = reqApi;

        if (g_vkXrSession == XR_NULL_HANDLE)
        {
        VkApplicationInfo vai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        vai.pApplicationName = "A Hat in Time (VR)";
        vai.pEngineName = "Unreal Engine 3 / HatVR";
        vai.apiVersion = vkApi;
        VkInstanceCreateInfo vici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        vici.pApplicationInfo = &vai;

        XrVulkanInstanceCreateInfoKHR xvici{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        xvici.systemId = g_vkXrSystemId;
        xvici.pfnGetInstanceProcAddr = gip;
        xvici.vulkanCreateInfo = &vici;

        VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
        xr = xrCreateVkInstance(g_vkXrInstance, &xvici, &g_vkInstance, &vr);
        if (XR_FAILED(xr) || vr != VK_SUCCESS || !g_vkInstance)
        {
            DxvkPathTrace("VKXR-V8 xrCreateVulkanInstanceKHR FAILED xr=%d vk=%d", (int)xr, (int)vr);
            ShutdownVulkanBackend(); return false;
        }
        g_vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gip(g_vkInstance, "vkDestroyInstance"));

        XrVulkanGraphicsDeviceGetInfoKHR gdgi{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        gdgi.systemId = g_vkXrSystemId;
        gdgi.vulkanInstance = g_vkInstance;
        xr = xrGetVkDevice(g_vkXrInstance, &gdgi, &g_vkPhysicalDevice);
        if (XR_FAILED(xr) || !g_vkPhysicalDevice) { LogXrFailure("xrGetVulkanGraphicsDevice2KHR", xr); ShutdownVulkanBackend(); return false; }

        g_vkQueueFamily = PickGraphicsQueueFamily(g_vkPhysicalDevice);
        if (g_vkQueueFamily == UINT32_MAX) { DxvkPathTrace("VKXR-V8 no graphics queue"); ShutdownVulkanBackend(); return false; }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = g_vkQueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        const char* phase6DeviceExtensions[] = {
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
            VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME
        };
        VkPhysicalDeviceShaderFloat16Int8Features nisFp16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceFeatures2 featureQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; featureQuery.pNext=&nisFp16;
        vkGetPhysicalDeviceFeatures2(g_vkPhysicalDevice,&featureQuery);
        VkPhysicalDeviceFeatures nisBaseEnable{};
        nisBaseEnable.shaderStorageImageWriteWithoutFormat = featureQuery.features.shaderStorageImageWriteWithoutFormat;
        VkPhysicalDeviceShaderFloat16Int8Features nisEnable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        nisEnable.shaderFloat16=nisFp16.shaderFloat16;
        VkDeviceCreateInfo vdci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        vdci.pNext = nisEnable.shaderFloat16 ? &nisEnable : nullptr;
        vdci.pEnabledFeatures = &nisBaseEnable;
        vdci.queueCreateInfoCount = 1;
        vdci.pQueueCreateInfos = &qci;
        vdci.enabledExtensionCount = 3;
        vdci.ppEnabledExtensionNames = phase6DeviceExtensions;
        DxvkPathTrace("VKBRIDGE-V5 requesting XR-device extensions + NIS fp16=%d",nisEnable.shaderFloat16?1:0);

        XrVulkanDeviceCreateInfoKHR xvdci{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        xvdci.systemId = g_vkXrSystemId;
        xvdci.pfnGetInstanceProcAddr = gip;
        xvdci.vulkanPhysicalDevice = g_vkPhysicalDevice;
        xvdci.vulkanCreateInfo = &vdci;

        xr = xrCreateVkLogicalDevice(g_vkXrInstance, &xvdci, &g_vkDevice, &vr);
        if (XR_FAILED(xr) || vr != VK_SUCCESS || !g_vkDevice)
        {
            DxvkPathTrace("VKXR-V8 xrCreateVulkanDeviceKHR FAILED xr=%d vk=%d", (int)xr, (int)vr);
            ShutdownVulkanBackend(); return false;
        }
        auto gdp = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gip(g_vkInstance, "vkGetDeviceProcAddr"));
        g_vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdp(g_vkDevice, "vkDestroyDevice"));
        auto gdq = reinterpret_cast<PFN_vkGetDeviceQueue>(gdp(g_vkDevice, "vkGetDeviceQueue"));
        gdq(g_vkDevice, g_vkQueueFamily, 0, &g_vkQueue);
        VulkanTraceMarkHatVRDevice(g_vkDevice, g_vkQueue, g_vkQueueFamily);

        XrGraphicsBindingVulkan2KHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
        binding.instance = g_vkInstance;
        binding.physicalDevice = g_vkPhysicalDevice;
        binding.device = g_vkDevice;
        binding.queueFamilyIndex = g_vkQueueFamily;
        binding.queueIndex = 0;
        XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
        sci.next = &binding;
        sci.systemId = g_vkXrSystemId;
        xr = xrCreateSession(g_vkXrInstance, &sci, &g_vkXrSession);
        if (XR_FAILED(xr)) { LogXrFailure("xrCreateSession(Vulkan)", xr); ShutdownVulkanBackend(); return false; }
        g_vkOwnsInstance = true;
        g_vkOwnsDevice = true;
        g_vkUsingDxvkDevice = false;
        }
        else
        {
            // Resolve loader entry points for the borrowed DXVK device; never destroy it.
            g_vkDestroyInstance = nullptr;
            g_vkDestroyDevice = nullptr;
        }

        XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.orientation.w = 1.0f;
        xr = xrCreateReferenceSpace(g_vkXrSession, &rs, &g_vkLocalSpace);
        if (XR_FAILED(xr)) { LogXrFailure("xrCreateReferenceSpace", xr); ShutdownVulkanBackend(); return false; }
        XrReferenceSpaceCreateInfo viewRs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        viewRs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewRs.poseInReferenceSpace.orientation.w = 1.0f;
        xr = xrCreateReferenceSpace(g_vkXrSession, &viewRs, &g_vkViewSpace);
        if (XR_FAILED(xr)) { LogXrFailure("xrCreateReferenceSpace(VIEW)", xr); ShutdownVulkanBackend(); return false; }

        if (!EnsureCommandResources())
        {
            DxvkPathTrace("VKXR-V8 command resources FAILED");
            ShutdownVulkanBackend(); return false;
        }

        NoteGraphicsApiInitialized(GraphicsApi::Vulkan);
        DxvkPathTrace("VKXR-V8 BOOTSTRAP SUCCESS session=%p device=%p queueFamily=%u",
            g_vkXrSession, g_vkDevice, g_vkQueueFamily);
        return true;
    }

    XrInstance GetVulkanXrInstance() { return g_vkXrInstance; }
    XrSession GetVulkanXrSession() { return g_vkXrSession; }
    XrSpace GetVulkanLocalSpace() { return g_vkLocalSpace; }
    bool IsVulkanXrSessionRunning() { return g_vkSessionRunning; }

    bool GetVulkanRecommendedEyeExtent(uint32_t* width, uint32_t* height)
    {
        if (!IsVulkanBackendActive() ||
            !g_vkRecommendedEyeWidth || !g_vkRecommendedEyeHeight)
            return false;
        if (width) *width = g_vkRecommendedEyeWidth;
        if (height) *height = g_vkRecommendedEyeHeight;
        return true;
    }

    bool BeginVulkanOpenXRFrame(
        XrView outViews[2],
        XrTime* outPredictedDisplayTime,
        bool* outShouldRender)
    {
        if (!IsVulkanBackendActive()) return false;
        ++g_vkBeginSerial;
        if (g_vkFrameBegun)
        {
            if (outViews) { outViews[0] = g_vkViews[0]; outViews[1] = g_vkViews[1]; }
            if (outPredictedDisplayTime) *outPredictedDisplayTime = g_vkPredictedDisplayTime;
            if (outShouldRender) *outShouldRender = g_vkFrameShouldRender;
            return true;
        }

        PollEvents();
        if (!g_vkSessionRunning || g_vkExitRequested)
            return false;

        XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        XrResult xr = xrWaitFrame(g_vkXrSession, &wi, &fs);
        if (XR_FAILED(xr)) { LogXrFailure("xrWaitFrame", xr); return false; }

        XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
        xr = xrBeginFrame(g_vkXrSession, &bi);
        if (XR_FAILED(xr)) { LogXrFailure("xrBeginFrame", xr); return false; }

        g_vkFrameBegun = true;
        g_vkFrameShouldRender = fs.shouldRender == XR_TRUE;
        g_vkPredictedDisplayTime = fs.predictedDisplayTime;
        if(fs.predictedDisplayPeriod>0) g_vkRefreshHz = 1000000000.0f / float(fs.predictedDisplayPeriod);

        XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_vkLocalSpace;
        XrViewState vs{ XR_TYPE_VIEW_STATE };
        uint32_t count = 0;
        g_vkViews[0] = { XR_TYPE_VIEW };
        g_vkViews[1] = { XR_TYPE_VIEW };
        xr = xrLocateViews(g_vkXrSession, &li, &vs, 2, &count, g_vkViews);
        const bool valid = XR_SUCCEEDED(xr) && count >= 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

        if (g_vkBeginSerial <= 12 || (g_vkBeginSerial % 300ULL) == 0)
            DxvkPathTrace("VKXR-V8 FRAME BEGIN #%llu shouldRender=%d locate=%d count=%u flags=0x%llX",
                g_vkBeginSerial, g_vkFrameShouldRender ? 1 : 0, (int)xr, count,
                (unsigned long long)vs.viewStateFlags);

        if (!valid) return true;
        if (outViews) { outViews[0] = g_vkViews[0]; outViews[1] = g_vkViews[1]; }
        if (outPredictedDisplayTime) *outPredictedDisplayTime = g_vkPredictedDisplayTime;
        if (outShouldRender) *outShouldRender = g_vkFrameShouldRender;
        return true;
    }

    static bool ClearOpenXrUiImageTransparent(VkImage image)
    {
        if (!image || !g_vkDevice || !g_vkQueue || !EnsureCommandResources())
            return false;

        vkResetFences(g_vkDevice, 1, &g_vkFence);
        vkResetCommandBuffer(g_vkCommandBuffer, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(g_vkCommandBuffer, &bi) != VK_SUCCESS)
            return false;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = range;
        vkCmdPipelineBarrier(g_vkCommandBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &toTransfer);

        const VkClearColorValue transparent = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(g_vkCommandBuffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);

        VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = image;
        toColor.subresourceRange = range;
        vkCmdPipelineBarrier(g_vkCommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            0, nullptr, 0, nullptr, 1, &toColor);

        if (vkEndCommandBuffer(g_vkCommandBuffer) != VK_SUCCESS)
            return false;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_vkCommandBuffer;
        if (vkQueueSubmit(g_vkQueue, 1, &si, g_vkFence) != VK_SUCCESS)
            return false;
        return vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }

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
        int uiDebugPermanentRoute)
    {
        if (!g_vkFrameBegun)
            return false;

        ++g_vkSubmitSerial;

        // A successful xrBeginFrame must eventually be paired with xrEndFrame.
        // During startup the stereo textures may not exist yet; in that case we
        // still end the frame with zero layers instead of leaving OpenXR stuck
        // in an in-progress frame forever.
        if (!leftEye || !rightEye || !width || !height)
        {
            XrFrameEndInfo emptyEnd{ XR_TYPE_FRAME_END_INFO };
            emptyEnd.displayTime = g_vkPredictedDisplayTime;
            emptyEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            emptyEnd.layerCount = 0;
            emptyEnd.layers = nullptr;
            const XrResult emptyResult = xrEndFrame(g_vkXrSession, &emptyEnd);
            DxvkPathTrace(
                "VKXR-V8 FRAME END EMPTY #%llu xr=%d haveLR=%d%d size=%ux%u",
                g_vkSubmitSerial, (int)emptyResult,
                leftEye ? 1 : 0, rightEye ? 1 : 0, width, height);
            g_vkFrameBegun = false;
            return false;
        }

        bool submitted = false;
        XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView pv[2] = {
            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
        };
        XrCompositionLayerQuad theaterQuad[2] = {
            { XR_TYPE_COMPOSITION_LAYER_QUAD },
            { XR_TYPE_COMPOSITION_LAYER_QUAD }
        };
        XrCompositionLayerQuad uiQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerQuad menuQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        const XrCompositionLayerBaseHeader* layers[4] = {};
        uint32_t layerCount = 0;

        g_vkLastSourceEyeWidth = width; g_vkLastSourceEyeHeight = height;

        // presentation-only output scaling. The captured AHiT/DXVK eye size
        // remains width x height in every renderer mode; only the OpenXR images
        // become larger. Clamp against the runtime's advertised maximum while
        // preserving aspect ratio.
        const int desiredUpscale = g_openXrUpscalingEnabled ? (std::max)(25, (std::min)(100, g_openXrUpscalePercent)) : 0;
        if (g_vkActiveUpscalePercent != desiredUpscale)
        {
            const int oldUpscale = g_vkActiveUpscalePercent;
            g_vkActiveUpscalePercent = desiredUpscale;
            DxvkPathTrace("VKXR-OUTPUT-SCALE live change old=%d new=%d (OpenXR destination only)",
                oldUpscale, g_vkActiveUpscalePercent);
            // EnsureEyeSwapchains() below owns eye swapchain recreation. UI and the
            // cross-device bridge lazily recreate themselves against the same new
            // destination extent. AHiT/DXVK source textures are intentionally untouched.
        }
        const int requestedUpscale = g_vkActiveUpscalePercent;
        double outputScale = 1.0 + (double)requestedUpscale / 100.0;
        if (g_vkMaxEyeWidth && width)
            outputScale = (std::min)(outputScale, (double)g_vkMaxEyeWidth / (double)width);
        if (g_vkMaxEyeHeight && height)
            outputScale = (std::min)(outputScale, (double)g_vkMaxEyeHeight / (double)height);
        outputScale = (std::max)(1.0, outputScale);
        const uint32_t xrWidth = (std::max)(1u, (uint32_t)floor((double)width * outputScale + 0.5));
        const uint32_t xrHeight = (std::max)(1u, (uint32_t)floor((double)height * outputScale + 0.5));

        if ((g_vkSubmitSerial <= 12 || (g_vkSubmitSerial % 300ULL) == 0) && requestedUpscale)
            DxvkPathTrace("VKXR-OUTPUT-SCALE requested=+%d%% effective=%.3fx source=%ux%u output=%ux%u max=%ux%u",
                requestedUpscale, outputScale, width, height, xrWidth, xrHeight, g_vkMaxEyeWidth, g_vkMaxEyeHeight);

        if (g_vkFrameShouldRender && EnsureEyeSwapchains(xrWidth, xrHeight))
        {
            IDirect3DTexture9* src[2] = { leftEye, rightEye };
            bool complete = true;

            g_vkBridgeTiming[0] = {};
            g_vkBridgeTiming[1] = {};
            LARGE_INTEGER bridgeBegin{}, bridgeEnd{};
            QueryPerformanceCounter(&bridgeBegin);

            const XrView* views = submitViews ? submitViews : g_vkViews;
            for (int eye = 0; eye < 2 && complete; ++eye)
            {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrResult xr = xrAcquireSwapchainImage(g_vkEyeSwapchains[eye], &ai, &idx);
                if (XR_FAILED(xr)) { LogXrFailure("xrAcquireSwapchainImage", xr); complete = false; break; }

                XrSwapchainImageWaitInfo swi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                swi.timeout = XR_INFINITE_DURATION;
                xr = xrWaitSwapchainImage(g_vkEyeSwapchains[eye], &swi);
                bool eyeOk = XR_SUCCEEDED(xr);
                if (!eyeOk) LogXrFailure("xrWaitSwapchainImage", xr);

                if (eyeOk && g_vkUsingDxvkDevice)
                    eyeOk = CopyDxvkTextureToOpenXrImage(
                        src[eye], g_vkSwapchainImages[eye][idx].image, width, height, xrWidth, xrHeight, eye);
                else if (eyeOk)
                    // OpenXR-frustum crop. Passing no projection FOV selects the
                    // existing direct-copy path below, which copies the literal
                    // SBS half:
                    //   LEFT  = [0, eyeWidth)
                    //   RIGHT = [eyeWidth, 2*eyeWidth)
                    eyeOk = Phase6CopyDxvkTextureToOpenXrImage(
                        src[eye],
                        g_vkSwapchainImages[eye][idx].image,
                        width, height,
                        xrWidth, xrHeight,
                        eye,
                        theaterMode ? nullptr : &views[eye].fov,
                        nativeStereo && g_sharperNativeStereo && !theaterMode);

                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                const XrResult rr = xrReleaseSwapchainImage(g_vkEyeSwapchains[eye], &ri);
                if (XR_FAILED(rr)) { LogXrFailure("xrReleaseSwapchainImage", rr); eyeOk = false; }

                if (!eyeOk) complete = false;
            }

            QueryPerformanceCounter(&bridgeEnd);
            if (g_vkSubmitSerial <= 120 || (g_vkSubmitSerial % 300ULL) == 0)
            {
                const auto& l = g_vkBridgeTiming[0];
                const auto& r = g_vkBridgeTiming[1];
                DxvkPathTrace(
                    "VKXR-TIMING #%llu L[rtd=%.3f copy=%.3f upcopy=%.3f gpu=%.3f] "
                    "R[rtd=%.3f copy=%.3f upcopy=%.3f gpu=%.3f] total=%.3f ms complete=%d",
                    g_vkSubmitSerial,
                    l.getRenderTargetDataMs, l.lockCopyMs, l.uploadMemcpyMs, l.uploadGpuMs,
                    r.getRenderTargetDataMs, r.lockCopyMs, r.uploadMemcpyMs, r.uploadGpuMs,
                    QpcElapsedMs(bridgeBegin, bridgeEnd), complete ? 1 : 0);
            }

            if (complete)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    pv[eye].pose = views[eye].pose;
                    pv[eye].fov = views[eye].fov;
                    // runtime vertical FOV unchanged. Horizontal FOV is also the
                    // original runtime FOV because the GPU crop reconstructs it.
                    pv[eye].subImage.swapchain = g_vkEyeSwapchains[eye];
                    pv[eye].subImage.imageRect.offset = { 0, 0 };
                    pv[eye].subImage.imageRect.extent = {
                        (int32_t)g_vkSwapchainWidth,
                        (int32_t)g_vkSwapchainHeight
                    };
                    pv[eye].subImage.imageArrayIndex = 0;
                }
                if (theaterMode)
                {
                    // Each OpenXR eye sees the corresponding rendered eye on the
                    // same fixed physical screen, preserving cinematic stereo depth
                    // while head motion moves naturally relative to the screen.
                    for (int eye = 0; eye < 2; ++eye)
                    {
                        XrCompositionLayerQuad& q = theaterQuad[eye];
                        // Theater is one virtual screen. The rendered game
                        // uses the same HUD transform as the extracted AHiT UI so
                        // Head-Locked HUD / Size / Distance / Height move both
                        // together instead of detaching the UI from the movie.
                        q.space = (hudHeadLocked && g_vkViewSpace!=XR_NULL_HANDLE)
                            ? g_vkViewSpace : g_vkLocalSpace;
                        q.eyeVisibility = eye == 0
                            ? XR_EYE_VISIBILITY_LEFT
                            : XR_EYE_VISIBILITY_RIGHT;
                        q.layerFlags = 0;
                        q.subImage.swapchain = g_vkEyeSwapchains[eye];
                        q.subImage.imageRect.offset = {0,0};
                        q.subImage.imageRect.extent = {
                            (int32_t)g_vkSwapchainWidth,
                            (int32_t)g_vkSwapchainHeight };
                        q.subImage.imageArrayIndex = 0;
                        q.pose.orientation = {0,0,0,1};
                        q.pose.position = {0,hudHeight,-hudDistance};
                        q.size.width = 1.60f * hudScale;
                        q.size.height = 0.90f * hudScale;
                        layers[layerCount++] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
                    }
                }
                else
                {
                    projection.space = g_vkLocalSpace;
                    projection.viewCount = 2;
                    projection.views = pv;
                    layers[layerCount++] =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                }
                submitted = true;

                // eye-sized intermediate texture into the FULL native SBS source.
                // Reusing it for UI staging therefore overwrote the scene source and
                // fed a 2688-wide texture into the 1344-wide UI bridge.  Stage UI in
                // its own eye-sized D3D9 texture and leave native SBS untouched.
                IDirect3DTexture9* uiStaging = nullptr;
                const bool uiSwapchainReady = EnsureUiSwapchain(xrWidth, xrHeight);
                if (finishedUiTexture && uiSwapchainReady &&
                    EnsurePhase81UiStagingTexture(finishedUiTexture, xrWidth, xrHeight))
                    uiStaging = g_phase81UiStagingTexture;

                // Never let an old OpenXR UI image become the failure state.
                // we acquire an image whenever the UI swapchain exists; a valid
                // fresh source overwrites it, otherwise it is explicitly cleared
                // to transparent before release.
                if (uiSwapchainReady)
                {
                    bool uiOk = true;
                    uint32_t uiIndex = 0;
                    bool uiAcquired = false;
                    if (uiOk)
                    {
                        XrSwapchainImageAcquireInfo ai{
                            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                        XrResult ux = xrAcquireSwapchainImage(
                            g_vkUiSwapchain, &ai, &uiIndex);
                        uiOk = XR_SUCCEEDED(ux);
                        uiAcquired = uiOk;
                    }
                    if (uiOk)
                    {
                        XrSwapchainImageWaitInfo wi{
                            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                        wi.timeout = XR_INFINITE_DURATION;
                        uiOk = XR_SUCCEEDED(
                            xrWaitSwapchainImage(g_vkUiSwapchain, &wi));
                    }
                    bool haveFreshUi = false;
                    if (uiOk && uiStaging &&
                        ScaleFinishedUiToEyeTexture(finishedUiTexture, uiStaging, nativeStereo))
                    {
                        if (g_vkUsingDxvkDevice)
                            haveFreshUi = CopyDxvkTextureToOpenXrImage(
                                uiStaging, g_vkUiSwapchainImages[uiIndex].image, xrWidth, xrHeight, xrWidth, xrHeight, 2);
                        else
                            haveFreshUi = Phase6CopyDxvkTextureToOpenXrImage(
                                uiStaging, g_vkUiSwapchainImages[uiIndex].image, xrWidth, xrHeight, xrWidth, xrHeight, 2, nullptr);
                    }
                    if (uiOk && !haveFreshUi)
                        uiOk = ClearOpenXrUiImageTransparent(
                            g_vkUiSwapchainImages[uiIndex].image);
                    else if (uiOk)
                        uiOk = haveFreshUi;
                    if (uiAcquired)
                    {
                        XrSwapchainImageReleaseInfo ri{
                            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                        uiOk = XR_SUCCEEDED(
                            xrReleaseSwapchainImage(g_vkUiSwapchain, &ri)) && uiOk;
                    }
                    if (uiOk && haveFreshUi)
                    {
                        // In Theater Mode the AHiT UI is a second compositor
                        // layer on the exact same virtual screen transform. Keeping
                        // it as a layer preserves alpha quality while making it
                        // behave as part of the movie screen.
                        uiQuad.space = (hudHeadLocked && g_vkViewSpace!=XR_NULL_HANDLE)
                            ? g_vkViewSpace : g_vkLocalSpace;
                        uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        uiQuad.layerFlags =
                            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        uiQuad.subImage.swapchain = g_vkUiSwapchain;
                        uiQuad.subImage.imageRect.offset = {0,0};
                        uiQuad.subImage.imageRect.extent = {
                            (int32_t)xrWidth,(int32_t)xrHeight};
                        uiQuad.subImage.imageArrayIndex = 0;
                        uiQuad.pose.orientation = {0,0,0,1};
                        uiQuad.pose.position = {0,hudHeight,-hudDistance};
                        uiQuad.size.width = 1.60f * hudScale;
                        uiQuad.size.height = 0.90f * hudScale;
                        // UI/menu remains a separate compositor layer in either
                        // immersive or Theater Mode.
                        if (layerCount < 4)
                            layers[layerCount++] =
                                reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                    &uiQuad);
                    }
                }

                // Dedicated HatVR settings panel rendered directly by Dear ImGui/Vulkan.
                // On first successful rendering, briefly show a head-locked hint so
                // new users know how to open it without leaving a permanent HUD.
                static ULONGLONG startupHintBeginMs=0;
                if(!startupHintBeginMs) startupHintBeginMs=GetTickCount64();
                const bool showStartupHint=!menuOpen && (GetTickCount64()-startupHintBeginMs)<5000ULL;
                if((menuOpen || showStartupHint) && layerCount<4)
                {
                    const bool menuResourcesOk=EnsureMenuResources(leftEye,900,760);
                    if(menuResourcesOk && EnsureMenuGpuRenderer())
                    {
                        static bool menuImageReady=false;
                        if(true)
                        {
                            if(menuOpen)
                                BuildHatVrImGuiMenu(nativeStereo,theaterMode,firstPersonEnabled,
                                    autoTheaterCutscenes,overrideLockedCameras,disablePlayerFade,
                                    rightHandHookshot,umbrellaMotionControls,playStationIcons,nintendoSwitchIcons,hudScale,hudDistance,hudHeight,
                                    hudHeadLocked,spectatorView,spectatorUiMode,menuPage,menuSelection,menuInsideCategory,
                                    uiDebugCandidateIndex,uiDebugCandidateCount,uiDebugCandidateHash,uiDebugCandidateHits,
                                    uiDebugPreviewMode,uiDebugPermanentRoute);
                            else
                                BuildHatVrStartupHint();
                            uint32_t idx=0;
                            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                            if(XR_SUCCEEDED(xrAcquireSwapchainImage(g_vkMenuSwapchain,&ai,&idx)))
                            {
                                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                                wi.timeout=XR_INFINITE_DURATION;
                                if(XR_SUCCEEDED(xrWaitSwapchainImage(g_vkMenuSwapchain,&wi)))
                                {
                                    if(RenderHatVrImGuiToSwapchain(idx))
                                    {
                                        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                                        if(XR_SUCCEEDED(xrReleaseSwapchainImage(g_vkMenuSwapchain,&rel)))
                                        {
                                            menuImageReady=true;
                                            // the plain ImGui menu is cheap and intentionally redrawn while open.
                                        }
                                    }
                                }
                            }
                        }
                        if(menuImageReady)
                        {
                            menuQuad.space=((showStartupHint || hudHeadLocked) && g_vkViewSpace!=XR_NULL_HANDLE)?g_vkViewSpace:g_vkLocalSpace;
                            menuQuad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                            menuQuad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                            menuQuad.subImage.swapchain=g_vkMenuSwapchain;
                            menuQuad.subImage.imageRect.offset={0,0};
                            menuQuad.subImage.imageRect.extent={900,760};
                            menuQuad.subImage.imageArrayIndex=0;
                            menuQuad.pose.orientation={0,0,0,1};
                            if(showStartupHint)
                            {
                                menuQuad.pose.position={0,0,-0.50f};
                                menuQuad.size.width=1.02f; menuQuad.size.height=0.86f;
                            }
                            else
                            {
                                menuQuad.pose.position={0,hudHeight,-(std::max)(0.20f,hudDistance-0.08f)};
                                menuQuad.size.width=1.02f*hudScale; menuQuad.size.height=0.86f*hudScale;
                            }
                            layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
                        }
                    }
                }
            }
        }

        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
        ei.displayTime = g_vkPredictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = submitted ? layerCount : 0u;
        ei.layers = submitted ? layers : nullptr;
        const XrResult endResult = xrEndFrame(g_vkXrSession, &ei);

        if (g_vkSubmitSerial <= 12 || (g_vkSubmitSerial % 300ULL) == 0 || XR_FAILED(endResult))
            DxvkPathTrace("VKXR-V8 FRAME END #%llu submitted=%d xr=%d captured=%ux%u xr=%ux%u layers=%u",
                g_vkSubmitSerial, submitted ? 1 : 0, (int)endResult,
                width, height, xrWidth, xrHeight, ei.layerCount);

        g_vkFrameBegun = false;
        return submitted && XR_SUCCEEDED(endResult);
    }

    void ShutdownVulkanBackend()
    {
        // finish any frame we own before asking the runtime to stop the session.
        if (g_vkXrSession != XR_NULL_HANDLE && g_vkFrameBegun)
        {
            XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
            endInfo.displayTime = g_vkPredictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            xrEndFrame(g_vkXrSession, &endInfo);
            g_vkFrameBegun = false;
        }

        // do not tear a live OpenXR session out from under the runtime.
        if (g_vkXrSession != XR_NULL_HANDLE && g_vkSessionRunning)
        {
            const XrResult requestResult = xrRequestExitSession(g_vkXrSession);
            DxvkPathTrace("VKXR shutdown: xrRequestExitSession=%d state=%d",
                (int)requestResult, (int)g_vkSessionState);

            const ULONGLONG deadline = GetTickCount64() + 500;
            while (g_vkSessionRunning && GetTickCount64() < deadline)
            {
                XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
                const XrResult pollResult = xrPollEvent(g_vkXrInstance, &event);
                if (pollResult == XR_EVENT_UNAVAILABLE)
                {
                    Sleep(1);
                    continue;
                }
                if (XR_FAILED(pollResult))
                    break;

                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    const auto* changed =
                        reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    g_vkSessionState = changed->state;
                    if (changed->state == XR_SESSION_STATE_STOPPING)
                    {
                        const XrResult endResult = xrEndSession(g_vkXrSession);
                        DxvkPathTrace("VKXR shutdown: xrEndSession=%d", (int)endResult);
                        g_vkSessionRunning = false;
                    }
                    else if (changed->state == XR_SESSION_STATE_EXITING ||
                             changed->state == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        g_vkSessionRunning = false;
                        g_vkExitRequested = true;
                    }
                }
            }
        }

        DestroyMenuResources();
        DestroyPhase81UiStagingTexture();
        DestroyUiSwapchain();
        DestroyEyeSwapchains();
        if (g_vkDevice) vkDeviceWaitIdle(g_vkDevice);
        if (g_dxvkProbeDevice) vkDeviceWaitIdle(g_dxvkProbeDevice);
        for (auto& b : g_phase6Eye)
        {
            if (b.xrSemaphore && g_vkDevice) vkDestroySemaphore(g_vkDevice, b.xrSemaphore, nullptr);
            if (b.dxvkSemaphore && g_dxvkProbeDevice) vkDestroySemaphore(g_dxvkProbeDevice, b.dxvkSemaphore, nullptr);
            if (b.xrImage && g_vkDevice) vkDestroyImage(g_vkDevice, b.xrImage, nullptr);
            if (b.xrMemory && g_vkDevice) vkFreeMemory(g_vkDevice, b.xrMemory, nullptr);
            if (b.dxvkImage && g_dxvkProbeDevice) vkDestroyImage(g_dxvkProbeDevice, b.dxvkImage, nullptr);
            if (b.dxvkMemory && g_dxvkProbeDevice) vkFreeMemory(g_dxvkProbeDevice, b.dxvkMemory, nullptr);
            if (b.memoryHandle) CloseHandle(b.memoryHandle);
            if (b.semaphoreHandle) CloseHandle(b.semaphoreHandle);
            b = {};
        }
        if (g_phase6DxvkFence && g_dxvkProbeDevice) vkDestroyFence(g_dxvkProbeDevice, g_phase6DxvkFence, nullptr);
        if (g_phase6DxvkCommandPool && g_dxvkProbeDevice) vkDestroyCommandPool(g_dxvkProbeDevice, g_phase6DxvkCommandPool, nullptr);
        g_phase6DxvkFence = VK_NULL_HANDLE; g_phase6DxvkCommandPool = VK_NULL_HANDLE; g_phase6DxvkCommandBuffer = VK_NULL_HANDLE;
        DestroyTransferCaches();

        if(g_vkDevice)
        {
            if(g_fsr.easuMapped && g_fsr.easuConstantsMem) vkUnmapMemory(g_vkDevice,g_fsr.easuConstantsMem); if(g_fsr.rcasMapped && g_fsr.rcasConstantsMem) vkUnmapMemory(g_vkDevice,g_fsr.rcasConstantsMem);
            if(g_fsr.intermediateView) vkDestroyImageView(g_vkDevice,g_fsr.intermediateView,nullptr); if(g_fsr.outputView) vkDestroyImageView(g_vkDevice,g_fsr.outputView,nullptr);
            if(g_fsr.intermediate) vkDestroyImage(g_vkDevice,g_fsr.intermediate,nullptr); if(g_fsr.intermediateMem) vkFreeMemory(g_vkDevice,g_fsr.intermediateMem,nullptr); if(g_fsr.output) vkDestroyImage(g_vkDevice,g_fsr.output,nullptr); if(g_fsr.outputMem) vkFreeMemory(g_vkDevice,g_fsr.outputMem,nullptr);
            if(g_fsr.easuPipeline) vkDestroyPipeline(g_vkDevice,g_fsr.easuPipeline,nullptr); if(g_fsr.rcasPipeline) vkDestroyPipeline(g_vkDevice,g_fsr.rcasPipeline,nullptr); if(g_fsr.pipelineLayout) vkDestroyPipelineLayout(g_vkDevice,g_fsr.pipelineLayout,nullptr);
            if(g_fsr.easuConstants) vkDestroyBuffer(g_vkDevice,g_fsr.easuConstants,nullptr); if(g_fsr.easuConstantsMem) vkFreeMemory(g_vkDevice,g_fsr.easuConstantsMem,nullptr); if(g_fsr.rcasConstants) vkDestroyBuffer(g_vkDevice,g_fsr.rcasConstants,nullptr); if(g_fsr.rcasConstantsMem) vkFreeMemory(g_vkDevice,g_fsr.rcasConstantsMem,nullptr);
            if(g_fsr.pool) vkDestroyDescriptorPool(g_vkDevice,g_fsr.pool,nullptr); if(g_fsr.setLayout) vkDestroyDescriptorSetLayout(g_vkDevice,g_fsr.setLayout,nullptr); if(g_fsr.sampler) vkDestroySampler(g_vkDevice,g_fsr.sampler,nullptr);
            if(g_fsr.easuShader) vkDestroyShaderModule(g_vkDevice,g_fsr.easuShader,nullptr); if(g_fsr.rcasShader) vkDestroyShaderModule(g_vkDevice,g_fsr.rcasShader,nullptr); g_fsr={};
            if(g_nis.configMapped && g_nis.configMemory) vkUnmapMemory(g_vkDevice,g_nis.configMemory);
            if(g_nis.outputView) vkDestroyImageView(g_vkDevice,g_nis.outputView,nullptr);
            if(g_nis.output) vkDestroyImage(g_vkDevice,g_nis.output,nullptr); if(g_nis.outputMem) vkFreeMemory(g_vkDevice,g_nis.outputMem,nullptr);
            if(g_nis.coefScaleView) vkDestroyImageView(g_vkDevice,g_nis.coefScaleView,nullptr); if(g_nis.coefUsmView) vkDestroyImageView(g_vkDevice,g_nis.coefUsmView,nullptr);
            if(g_nis.coefScale) vkDestroyImage(g_vkDevice,g_nis.coefScale,nullptr); if(g_nis.coefScaleMem) vkFreeMemory(g_vkDevice,g_nis.coefScaleMem,nullptr);
            if(g_nis.coefUsm) vkDestroyImage(g_vkDevice,g_nis.coefUsm,nullptr); if(g_nis.coefUsmMem) vkFreeMemory(g_vkDevice,g_nis.coefUsmMem,nullptr);
            if(g_nis.pipeline) vkDestroyPipeline(g_vkDevice,g_nis.pipeline,nullptr); if(g_nis.pipelineLayout) vkDestroyPipelineLayout(g_vkDevice,g_nis.pipelineLayout,nullptr);
            if(g_nis.configBuffer) vkDestroyBuffer(g_vkDevice,g_nis.configBuffer,nullptr); if(g_nis.configMemory) vkFreeMemory(g_vkDevice,g_nis.configMemory,nullptr);
            if(g_nis.pool) vkDestroyDescriptorPool(g_vkDevice,g_nis.pool,nullptr); if(g_nis.setLayout) vkDestroyDescriptorSetLayout(g_vkDevice,g_nis.setLayout,nullptr);
            if(g_nis.sampler) vkDestroySampler(g_vkDevice,g_nis.sampler,nullptr); if(g_nis.shader) vkDestroyShaderModule(g_vkDevice,g_nis.shader,nullptr);
            g_nis={};
        }


        if (g_vkDevice != VK_NULL_HANDLE)
        {
            if (g_vkOwnsDevice) vkDeviceWaitIdle(g_vkDevice);
            if (g_vkFence) { vkDestroyFence(g_vkDevice, g_vkFence, nullptr); g_vkFence = VK_NULL_HANDLE; }
            if (g_vkCommandPool) { vkDestroyCommandPool(g_vkDevice, g_vkCommandPool, nullptr); g_vkCommandPool = VK_NULL_HANDLE; g_vkCommandBuffer = VK_NULL_HANDLE; }
        }
        if (g_vkViewSpace != XR_NULL_HANDLE) { xrDestroySpace(g_vkViewSpace); g_vkViewSpace = XR_NULL_HANDLE; }
        if (g_vkLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(g_vkLocalSpace); g_vkLocalSpace = XR_NULL_HANDLE; }
        if (g_vkXrSession != XR_NULL_HANDLE) { xrDestroySession(g_vkXrSession); g_vkXrSession = XR_NULL_HANDLE; }
        if (g_vkOwnsDevice && g_vkDevice != VK_NULL_HANDLE && g_vkDestroyDevice) g_vkDestroyDevice(g_vkDevice, nullptr);
        if (g_vkOwnsInstance && g_vkInstance != VK_NULL_HANDLE && g_vkDestroyInstance) g_vkDestroyInstance(g_vkInstance, nullptr);
        g_vkDevice = VK_NULL_HANDLE; g_vkInstance = VK_NULL_HANDLE;
        g_vkPhysicalDevice = VK_NULL_HANDLE; g_vkQueue = VK_NULL_HANDLE;
        g_vkOwnsDevice = false; g_vkOwnsInstance = false; g_vkUsingDxvkDevice = false;
        if (g_vkXrInstance != XR_NULL_HANDLE) { xrDestroyInstance(g_vkXrInstance); g_vkXrInstance = XR_NULL_HANDLE; }

        g_vkXrSystemId = XR_NULL_SYSTEM_ID;
        g_vkSessionRunning = false;
        g_vkExitRequested = false;
        g_vkFrameBegun = false;
        g_vkFrameShouldRender = false;
        g_vkSessionState = XR_SESSION_STATE_UNKNOWN;
        g_vkRecommendedEyeWidth = 0;
        g_vkRecommendedEyeHeight = 0;
        g_vkMaxEyeWidth = 0;
        g_vkMaxEyeHeight = 0;
        g_vkLastSourceEyeWidth = g_vkLastSourceEyeHeight = 0;
        g_vkActiveUpscalePercent = -1;
    }
}
