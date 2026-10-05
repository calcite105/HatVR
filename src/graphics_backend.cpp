#include "graphics_backend.h"

#include <atomic>

namespace ahitvr
{
    namespace
    {
        std::atomic<GraphicsApi> g_graphicsApi{ GraphicsApi::Unknown };
    }

    void NoteGraphicsApiInitialized(GraphicsApi api)
    {
        if (api == GraphicsApi::Unknown)
            return;

        GraphicsApi current = g_graphicsApi.load(std::memory_order_acquire);

        // A real Vulkan renderer event may occur after AHiT's startup D3D9
        // initialization, so it is allowed to supersede D3D9. We never switch
        // back merely because another incidental D3D9 object appears later.
        if (api == GraphicsApi::Vulkan ||
            current == GraphicsApi::Unknown ||
            current == api)
        {
            g_graphicsApi.store(api, std::memory_order_release);
        }
    }

    GraphicsApi GetGraphicsApi()
    {
        return g_graphicsApi.load(std::memory_order_acquire);
    }

    const char* GetGraphicsApiName()
    {
        switch (GetGraphicsApi())
        {
        case GraphicsApi::Direct3D9:
            return "Direct3D 9";
        case GraphicsApi::Vulkan:
            return "Vulkan";
        default:
            return "Unknown";
        }
    }

    bool ShouldActivateGraphicsApi(GraphicsApi api)
    {
        const GraphicsApi current = GetGraphicsApi();

        if (api == GraphicsApi::Vulkan)
            return current != GraphicsApi::Vulkan;

        if (api == GraphicsApi::Direct3D9)
            return current == GraphicsApi::Unknown ||
                   current == GraphicsApi::Direct3D9;

        return false;
    }
}
