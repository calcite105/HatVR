    // Device hook

    static bool HookDevice(IDirect3DDevice9* device)
    {
        DxvkPathTrace("V9 HookDevice ENTER device=%p", device);
        if (!device) { DxvkPathTrace("V9 HookDevice FAIL null"); return false; }

        void** vtable =
            *reinterpret_cast<void***>(device);

        // vtable slot 47 and SetScissorRect is slot 75.
        if (!g_v41RasterHooksInstalled)
        {
            bool v41ok = true;
            MH_STATUS st = MH_CreateHook(vtable[47], &HookV41SetViewport,
                reinterpret_cast<void**>(&g_originalV41SetViewport));
            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                st = MH_EnableHook(vtable[47]);
                if (st != MH_OK && st != MH_ERROR_ENABLED) v41ok = false;
            } else v41ok = false;

            st = MH_CreateHook(vtable[75], &HookV41SetScissorRect,
                reinterpret_cast<void**>(&g_originalV41SetScissorRect));
            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                st = MH_EnableHook(vtable[75]);
                if (st != MH_OK && st != MH_ERROR_ENABLED) v41ok = false;
            } else v41ok = false;

            g_v41RasterHooksInstalled = v41ok;
            DxvkPathTrace("V43 raster hooks SetViewport/SetScissorRect passive + PP composite tracer installed=%d", v41ok ? 1 : 0);
        }

        if (!g_presentHookInstalled)
        {
            // IDirect3DDevice9::Present = index 17.
            void* presentAddress = vtable[17];
            DxvkPathTrace("V9 Present target device=%p vtable=%p target=%p", device, vtable, presentAddress);

            MH_STATUS createStatus = MH_CreateHook(
                presentAddress,
                &HookedPresent,
                reinterpret_cast<void**>(&g_originalPresent)
            );

            if (createStatus != MH_OK &&
                createStatus != MH_ERROR_ALREADY_CREATED)
            {
                DebugLog("MH_CreateHook(Present) FAILED\n");
                return false;
            }

            MH_STATUS enableStatus = MH_EnableHook(presentAddress);

            if (enableStatus != MH_OK &&
                enableStatus != MH_ERROR_ENABLED)
            {
                DebugLog("MH_EnableHook(Present) FAILED\n");
                return false;
            }

            g_presentHookInstalled = true;

            {
                char v7line[512] = {};
                sprintf_s(v7line, sizeof(v7line),
                    "DXVK-V7: Present hook ACTIVE device=%p vtable=%p target=%p original=%p createStatus=%d enableStatus=%d\n",
                    device, vtable, presentAddress, g_originalPresent,
                    static_cast<int>(createStatus), static_cast<int>(enableStatus));
                DebugLog(v7line);
            }

            // it still initializes the D3D12 OpenXR renderer here.
            // the verified DXVK recovery path has already activated its separate
            // Vulkan XrSession, so it skips only this D3D12 bootstrap.
            if (!IsVulkanBackendActive())
            {
                DebugLog("DXVK-V7: InitializeOpenXRBootstrap ENTER\n");
                DxvkPathTrace("V9 InitializeOpenXRBootstrap ENTER device=%p", device);
                const bool v7XrBootstrap = InitializeOpenXRBootstrap(device);
                DxvkPathTrace("V9 InitializeOpenXRBootstrap RETURN %s", v7XrBootstrap ? "TRUE" : "FALSE");
                DebugLog(v7XrBootstrap
                    ? "DXVK-V7: InitializeOpenXRBootstrap SUCCESS\n"
                    : "DXVK-V7: InitializeOpenXRBootstrap FAILED\n");
            }
            else
            {
                DxvkPathTrace("VKXR-V16T Present hook active; D3D12 OpenXR bootstrap SKIPPED");
            }
            DebugLog("Present hook installed\n");
        }

        if (!g_hudD3DHooksInstalled)
        {
            struct HudD3DHookSpec { int index; void* hook; void** original; const char* name; } specs[] = {
                {81, reinterpret_cast<void*>(&HookedHudDrawPrimitive), reinterpret_cast<void**>(&g_originalHudDrawPrimitive), "DP"},
                {83, reinterpret_cast<void*>(&HookedHudDrawPrimitiveUP), reinterpret_cast<void**>(&g_originalHudDrawPrimitiveUP), "DPUP"},
                {84, reinterpret_cast<void*>(&HookedHudDrawIndexedPrimitiveUP), reinterpret_cast<void**>(&g_originalHudDrawIndexedPrimitiveUP), "DIPUP"},
                {65, reinterpret_cast<void*>(&HookedHudSetTexture), reinterpret_cast<void**>(&g_originalHudSetTexture), "SetTexture"},
            };
            bool ok = true;
            for (auto& spec : specs) {
                MH_STATUS st = MH_CreateHook(vtable[spec.index], spec.hook, spec.original);
                if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) { ok = false; break; }
                st = MH_EnableHook(vtable[spec.index]);
                if (st != MH_OK && st != MH_ERROR_ENABLED) { ok = false; break; }
            }
            if (ok && !g_vertexShaderHookInstalled) {
                MH_STATUS st = MH_CreateHook(vtable[92], &HookedSetVertexShader, reinterpret_cast<void**>(&g_originalSetVertexShader));
                if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                    st = MH_EnableHook(vtable[92]);
                    if (st == MH_OK || st == MH_ERROR_ENABLED) g_vertexShaderHookInstalled = true; else ok = false;
                }
                else ok = false;
            }
            // float constants. D3D9 vtable: SetPixelShader=107,
            // SetPixelShaderConstantF=109.
            if (ok && !g_pixelShaderHookInstalled) {
                MH_STATUS st = MH_CreateHook(vtable[107], &HookedSetPixelShader, reinterpret_cast<void**>(&g_originalSetPixelShader));
                if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                    st = MH_EnableHook(vtable[107]);
                    if (st == MH_OK || st == MH_ERROR_ENABLED) g_pixelShaderHookInstalled = true; else ok = false;
                } else ok = false;
            }
            if (ok && !g_pixelConstantHookInstalled) {
                MH_STATUS st = MH_CreateHook(vtable[109], &HookedSetPixelShaderConstantF, reinterpret_cast<void**>(&g_originalSetPixelShaderConstantF));
                if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                    st = MH_EnableHook(vtable[109]);
                    if (st == MH_OK || st == MH_ERROR_ENABLED) g_pixelConstantHookInstalled = true; else ok = false;
                } else ok = false;
            }
            if (ok && !g_drawIndexedPrimitiveHookInstalled) {
                MH_STATUS st = MH_CreateHook(vtable[82], &HookedDrawIndexedPrimitive, reinterpret_cast<void**>(&g_originalDrawIndexedPrimitive));
                if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                    st = MH_EnableHook(vtable[82]);
                    if (st == MH_OK || st == MH_ERROR_ENABLED) g_drawIndexedPrimitiveHookInstalled = true; else ok = false;
                }
                else ok = false;
            }
            g_hudD3DHooksInstalled = ok;
            if (ok) HudWindowLog("Passive D3D hooks enabled: DP,DIP,DPUP,DIPUP,SetTexture,SetVS,SetPS,SetPSConstF\n");
        }

{
            char v7line[512] = {};
            sprintf_s(v7line, sizeof(v7line),
                "DXVK-V7: HookDevice COMPLETE present=%d hud=%d setVS=%d DIP=%d\n",
                g_presentHookInstalled ? 1 : 0,
                g_hudD3DHooksInstalled ? 1 : 0,
                g_vertexShaderHookInstalled ? 1 : 0,
                g_drawIndexedPrimitiveHookInstalled ? 1 : 0);
            DebugLog(v7line);
        }

        DxvkPathTrace("V9 HookDevice RETURN TRUE device=%p present=%d hud=%d",
            device, g_presentHookInstalled ? 1 : 0, g_hudD3DHooksInstalled ? 1 : 0);
        return true;
    }

    // CreateDevice

    static HRESULT STDMETHODCALLTYPE HookedCreateDevice(
        IDirect3D9* self,
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        DebugLog("HookedCreateDevice ENTERED\n");
        HRESULT hr = g_originalCreateDevice(
            self,
            Adapter,
            DeviceType,
            hFocusWindow,
            BehaviorFlags,
            pPresentationParameters,
            ppReturnedDeviceInterface
        );

        if (SUCCEEDED(hr) &&
            ppReturnedDeviceInterface &&
            *ppReturnedDeviceInterface)
        {
            DebugLog("CreateDevice SUCCEEDED\n");
            // Use the actual D3D9 focus window for HatVR keyboard input. This is
            // intentionally window-message based rather than GetAsyncKeyState,
            // so menu shortcuts cannot fire while A Hat in Time is unfocused.
            g_hatVrGameWindow = hFocusWindow;
            InstallHatVrKeyboardMenuHook(hFocusWindow);
            // Physical gamepads use the same bridge even when OpenXR motion-controller input is disabled.
            InstallXInputBridge();
            HookDevice(*ppReturnedDeviceInterface);
        }

        return hr;
    }

    // Install CreateDevice hook

    static bool EnsureCoreVrHooksInstalled()
    {
        static LONG coreState = 0; // 0=not started, 1=installing, 2=complete

        LONG observed = InterlockedCompareExchange(&coreState, 1, 0);
        if (observed == 2)
        {
            DxvkPathTrace("V11 core VR hooks already installed");
            return true;
        }

        if (observed == 1)
        {
            // the existing-device discovery runs on its own worker thread.
            // Avoid racing a simultaneous normal D3D9 initialization.
            for (int i = 0; i < 500 && InterlockedCompareExchange(&coreState, 0, 0) == 1; ++i)
                Sleep(1);

            const bool ready = InterlockedCompareExchange(&coreState, 0, 0) == 2;
            DxvkPathTrace("V11 core VR hooks wait result=%s", ready ? "READY" : "TIMEOUT");
            return ready;
        }

        DxvkPathTrace("V11 core VR hook installation ENTER");

        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            InterlockedExchange(&coreState, 0);
            DxvkPathTrace("V11 core VR hook installation FAIL MH_Initialize=%d", (int)status);
            return false;
        }

        // these are the same functional hooks the normal D3D9 HookCreateDevice
        // path installs before the device exists.  The DXVK path discovers an
        // already-created device, so it must explicitly install them too.
        const bool hudResolver = InstallHudWindowResolverHook();
        const bool canvasBatch = InstallCanvasBatchFlushHook();
        const bool canvasFinalize = InstallCanvasFinalizeCandidateHook();
        const bool calcScene = InstallCalcSceneViewCandidateHook();
        const bool playerView = InstallGetPlayerViewPointCandidateHook();
        const bool perspective = InstallPerspectiveMatrixCandidateHook();
        const bool resolution = InstallNativeVrResolutionHooks();
        const bool workCounter = InstallV67WorkCounterHooks();

        DxvkPathTrace(
            "V11 core hook results hud=%d batch=%d finalize=%d calcScene=%d playerView=%d perspective=%d resolution=%d workCounter=%d",
            hudResolver ? 1 : 0, canvasBatch ? 1 : 0, canvasFinalize ? 1 : 0,
            calcScene ? 1 : 0, playerView ? 1 : 0, perspective ? 1 : 0,
            resolution ? 1 : 0, workCounter ? 1 : 0);

        Log("Core VR hooks installed; UI interception and XR renderer/pose binding preserved");
        DebugLog("XR projection build: compositor eye poses restored to native LOCAL-space OpenXR poses\n");
        DebugLog("XR CAMERA MODE: absolute LOCAL orientation + position-only startup recenter\n");
        DebugLog("NATIVE STEREO MODE: dual CalcSceneView in one FSceneViewFamily, LocalPlayer viewport split 50/50\n");
        DebugLog("XR TIMING MODE: wait/begin/locate BEFORE gameplay CalcSceneView\n");

        // also true for Stereo Lite, so it MUST NOT be used to enable the later
        // GamePlayers[0] x2 native-loop patch here.
        const bool renderLoopStereo = SetNativeRenderLoopStereoPatch(false);
        DxvkPathTrace("V177 selected renderer=%s native render-loop patch=%d",
            g_nativeStereoEnabled ? "STEREO" : "AFR", renderLoopStereo ? 1 : 0);

        InterlockedExchange(&coreState, 2);
        DxvkPathTrace("V11 core VR hook installation COMPLETE");
        return true;
    }

    static bool SetNativeRenderLoopStereoPatch(bool enable)
    {
        auto* exe = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!exe)
            return false;

        unsigned char* advance = exe + kViewportLoopAdvanceRva;
        unsigned char* limit   = exe + kViewportLoopLimitRva;
        const unsigned char originalAdvance[4] = {0x48,0x83,0xC7,0x08}; // add rdi,8
        const unsigned char originalLimit[6]   = {0x3B,0x99,0xE4,0x06,0x00,0x00}; // cmp ebx,[rcx+6E4]
        const unsigned char stereoAdvance[4]  = {0x90,0x90,0x90,0x90};
        const unsigned char stereoLimit[6]    = {0x83,0xFB,0x02,0x90,0x90,0x90}; // cmp ebx,2

        const bool currentlyStereo =
            memcmp(advance, stereoAdvance, sizeof(stereoAdvance)) == 0 &&
            memcmp(limit, stereoLimit, sizeof(stereoLimit)) == 0;
        const bool currentlyOriginal =
            memcmp(advance, originalAdvance, sizeof(originalAdvance)) == 0 &&
            memcmp(limit, originalLimit, sizeof(originalLimit)) == 0;

        if (enable && currentlyStereo) {
            g_nativeRenderLoopStereoActive = true;
            return true;
        }
        if (!enable && currentlyOriginal) {
            g_nativeRenderLoopStereoActive = false;
            return true;
        }
        if (!currentlyStereo && !currentlyOriginal) {
            DxvkPathTrace("V177 RENDER_LOOP transition REFUSED: EXE bytes are neither verified AFR nor STEREO state");
            return false;
        }

        const unsigned char* targetAdvance = enable ? stereoAdvance : originalAdvance;
        const unsigned char* targetLimit   = enable ? stereoLimit : originalLimit;
        DWORD oldA=0, oldL=0;
        if (!VirtualProtect(advance, sizeof(stereoAdvance), PAGE_EXECUTE_READWRITE, &oldA))
            return false;
        if (!VirtualProtect(limit, sizeof(stereoLimit), PAGE_EXECUTE_READWRITE, &oldL)) {
            DWORD tmp=0;
            VirtualProtect(advance, sizeof(stereoAdvance), oldA, &tmp);
            return false;
        }

        memcpy(advance, targetAdvance, sizeof(stereoAdvance));
        memcpy(limit, targetLimit, sizeof(stereoLimit));
        FlushInstructionCache(GetCurrentProcess(), advance, sizeof(stereoAdvance));
        FlushInstructionCache(GetCurrentProcess(), limit, sizeof(stereoLimit));
        DWORD tmp=0;
        VirtualProtect(limit, sizeof(stereoLimit), oldL, &tmp);
        VirtualProtect(advance, sizeof(stereoAdvance), oldA, &tmp);

        g_nativeRenderLoopStereoActive = enable;
        DxvkPathTrace(enable
            ? "V177 STEREO LOOP ACTIVE: GamePlayers[0] rendered twice into one native UE3 view family"
            : "V177 AFR LOOP RESTORED: original GamePlayers iteration restored");
        return true;
    }

    bool HookCreateDevice(IDirect3D9* d3d9)
    {
        DebugLog("HookCreateDevice ENTERED\n");
        if (!d3d9)
            return false;

        static bool createDeviceHookInstalled = false;

        if (!EnsureCoreVrHooksInstalled())
            return false;

        if (createDeviceHookInstalled)
            return true;

        void** vtable = *reinterpret_cast<void***>(d3d9);

        // IDirect3D9::CreateDevice = index 16.
        void* createDeviceAddress = vtable[16];

        MH_STATUS createStatus = MH_CreateHook(
            createDeviceAddress,
            &HookedCreateDevice,
            reinterpret_cast<void**>(&g_originalCreateDevice));

        if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        {
            DxvkPathTrace("V11 CreateDevice MH_CreateHook failed=%d target=%p", (int)createStatus, createDeviceAddress);
            return false;
        }

        MH_STATUS enableStatus = MH_EnableHook(createDeviceAddress);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        {
            DxvkPathTrace("V11 CreateDevice MH_EnableHook failed=%d target=%p", (int)enableStatus, createDeviceAddress);
            return false;
        }

        createDeviceHookInstalled = true;
        OutputDebugStringA("[AHiTVR] CreateDevice hook installed\n");
        return true;
    }

