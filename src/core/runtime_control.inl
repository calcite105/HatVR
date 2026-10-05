    using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9* self, IDirect3DPixelShader9* pShader);
    using SetPixelShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9* self, UINT StartRegister,
        const float* pConstantData, UINT Vector4fCount);

    // Hook state

    static CreateDeviceFn g_originalCreateDevice = nullptr;
    static PresentFn g_originalPresent = nullptr;
    static SetVertexShaderFn g_originalSetVertexShader = nullptr;
    static SetPixelShaderFn g_originalSetPixelShader = nullptr;
    static SetPixelShaderConstantFFn g_originalSetPixelShaderConstantF = nullptr;
    static DrawIndexedPrimitiveFn g_originalDrawIndexedPrimitive = nullptr;
    static DrawPrimitiveFn g_originalHudDrawPrimitive = nullptr;
    static DrawPrimitiveUPFn g_originalHudDrawPrimitiveUP = nullptr;
    static DrawIndexedPrimitiveUPFn g_originalHudDrawIndexedPrimitiveUP = nullptr;
    static SetTextureFn g_originalHudSetTexture = nullptr;
    static bool g_hudD3DHooksInstalled = false;

    static bool g_presentHookInstalled = false;
    static bool g_vertexShaderHookInstalled = false;
    static bool g_pixelShaderHookInstalled = false;
    static bool g_pixelConstantHookInstalled = false;
    static bool g_drawIndexedPrimitiveHookInstalled = false;
    static IDirect3DVertexShader9* g_currentVertexShader = nullptr;
    static IDirect3DPixelShader9* g_currentPixelShader = nullptr;

    // F3 captures one comparison frame and logs changed PS float constants.

    static IDirect3DVertexShader9* g_visualTestCandidates[6] = {};

    enum class ShaderDebugMode
    {
        Normal = 0,
        HideSelected = 1,
        SoloSelected = 2
    };

    struct ShaderIdentity
    {
        IDirect3DVertexShader9* shader = nullptr;
        uint64_t hash = 0;
        unsigned long long draws = 0;
    };

    static std::vector<ShaderIdentity> g_shaderInspector;
    static int g_shaderInspectorSelected = 0;
    static ShaderDebugMode g_shaderDebugMode = ShaderDebugMode::Normal;
    static bool g_shaderInspectorFrozen = false;
    static ULONGLONG g_lastKeyTick = 0;

    static char g_clipboardStatus[96] = {};
    static ULONGLONG g_clipboardStatusUntil = 0;

    static constexpr uint64_t kStaticWorldShaderHash = 0xE9CBE91B4F6FA393ULL;
    static constexpr uint64_t kDynamicObjectShaderHash = 0x41E01DC942053AF7ULL;

    struct CameraProbeSample
    {
        uint64_t hash = 0;
        ULONGLONG lastDumpTick = 0;
        unsigned int dumpCount = 0;
    };

    static CameraProbeSample g_cameraProbeSamples[2] =
    {
        { kStaticWorldShaderHash, 0, 0 },
        { kDynamicObjectShaderHash, 0, 0 }
    };

    static IDirect3DTexture9* g_frameCopy = nullptr;

    static UINT g_copyWidth = 0;
    static UINT g_copyHeight = 0;
    static D3DFORMAT g_copyFormat = D3DFMT_UNKNOWN;

