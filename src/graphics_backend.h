#pragma once

namespace ahitvr
{
    enum class GraphicsApi
    {
        Unknown = 0,
        Direct3D9,
        Vulkan
    };

    // renderer-event state. We deliberately do not infer this from loaded DLLs
    // or from the in-game graphics setting.
    void NoteGraphicsApiInitialized(GraphicsApi api);
    GraphicsApi GetGraphicsApi();
    const char* GetGraphicsApiName();

    // Vulkan is the later/final renderer event when AHiT transitions away from
    // its startup D3D9 path. This lets a real Vulkan backend supersede D3D9.
    bool ShouldActivateGraphicsApi(GraphicsApi api);
}
