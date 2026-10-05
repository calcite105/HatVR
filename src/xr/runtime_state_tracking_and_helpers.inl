    // Central XR/render logging aliases.  No local file handles or logger state.
    #define RenderPipelineDiagnosticLog(text) DiagnosticLog((text))
    #define RHIBackend2A0Log(text) LogCategory("D3D9", "%s", (text))
    #define RHI4B0Log(text) LogCategory("D3D9", "%s", (text))
    #define PerfLog(text) LogCategory("PERF", "%s", (text))
    #define V67CounterLog(text) LogCategory("RENDER", "%s", (text))
    #define UiExtractLog(text) LogCategory("UI", "%s", (text))

#pragma message("HatVR PHASE7.2C openxr_runtime.inl ACTIVE 2026-09-20C")
// inside namespace ahitvr by d3d9_hook.cpp.
void VulkanTraceBeginD3D9Resource(const char* label, uint32_t width, uint32_t height, uint32_t d3dFormat);
void VulkanTraceEndD3D9Resource(const char* label, long result, const void* d3dResource);

    //
    // the Khronos loader is statically linked into this d3d9.dll. For this
    // milestone we create a small dedicated D3D12 device on the exact adapter
    // requested by the active OpenXR runtime. That is sufficient to create a
    // real XrSession and retrieve live HMD eye poses/FOVs. AHiT image interop
    // comes next; it does not need to block headset tracking/FOV validation.
    static XrInstance g_xrInstance = XR_NULL_HANDLE;
    static XrSystemId g_xrSystemId = XR_NULL_SYSTEM_ID;
    static XrSession g_xrSession = XR_NULL_HANDLE;
    static XrSpace g_xrLocalSpace = XR_NULL_HANDLE;
    static bool g_xrInitialized = false;
    static bool g_xrSessionRunning = false;
    static bool g_xrExitRequested = false;
    static XrSessionState g_xrSessionState = XR_SESSION_STATE_UNKNOWN;

    static bool InitializeVrControllerInput();
    static void SyncVrControllerActions();
    static bool InitializeVrControllerActionsOnAvatarSet(
        XrActionSet actionSet, XrAction gripPoseAction);

    static ID3D12Device* g_xrD3D12Device = nullptr;
    static ID3D12CommandQueue* g_xrD3D12Queue = nullptr;

    static IDirect3DDevice9On12* g_d3d9On12Device = nullptr;
    static XrSwapchain g_xrEyeSwapchains[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    static XrSwapchain g_xrUiSwapchain = XR_NULL_HANDLE;
    static std::vector<XrSwapchainImageD3D12KHR> g_xrUiSwapchainImages;
    static XrPosef g_uiPose{ {0,0,0,1}, {0,0,-0.75f} };

    static std::vector<XrSwapchainImageD3D12KHR> g_xrSwapchainImages[2];
    static int64_t g_xrSwapchainFormat = 0;
    static uint32_t g_xrSwapchainWidth = 0;
    static uint32_t g_xrSwapchainHeight = 0;

    static ID3D12CommandAllocator* g_xrCommandAllocator = nullptr;
    static ID3D12GraphicsCommandList* g_xrCommandList = nullptr;
    static ID3D12Fence* g_xrFence = nullptr;
    static HANDLE g_xrFenceEvent = nullptr;
    static UINT64 g_xrFenceValue = 0;

    struct CpuBridgeCache
    {
        IDirect3DTexture9* source = nullptr;
        IDirect3DSurface9* systemSurface = nullptr;
        ID3D12Resource* upload = nullptr;
        void* mappedUpload = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 uploadSize = 0;
        UINT width = 0;
        UINT height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
    };
    static CpuBridgeCache g_cpuBridgeCache[2] = {};

    static void ReleaseCpuBridgeCache(CpuBridgeCache& c)
    {
        if (c.upload && c.mappedUpload) c.upload->Unmap(0, nullptr);
        if (c.upload) c.upload->Release();
        if (c.systemSurface) c.systemSurface->Release();
        c = {};
    }

    static bool g_xrFrameBegun = false;
    static bool g_xrFrameShouldRender = false;
    static XrTime g_xrPredictedDisplayTime = 0;
    static LARGE_INTEGER g_xrLocateQpc{};
    static bool g_xrLocateQpcValid = false;
    static bool g_xrViewsValidThisFrame = false;

    // Render ownership model (readability refactor only):
    //   live XR views -> immutable render snapshot -> scene/renderer binding
    //   -> completed stereo capture owns that pose -> OpenXR submission.
    // Never substitute the mutable live views after a render has acquired ownership.

    static XrViewConfigurationView g_xrViewConfig[2] = {
        { XR_TYPE_VIEW_CONFIGURATION_VIEW },
        { XR_TYPE_VIEW_CONFIGURATION_VIEW }
    };

    static XrView g_xrViews[2] = {
        { XR_TYPE_VIEW },
        { XR_TYPE_VIEW }
    };

    // Immutable pose/FOV snapshot belonging to the gameplay render currently being built.
    // the compositor submission must describe the pose baked into THESE pixels,
    // rather than reading the mutable live g_xrViews array at xrEndFrame time.
    static XrView g_renderPoseSnapshotViews[2] = {
        { XR_TYPE_VIEW },
        { XR_TYPE_VIEW }
    };
    static bool g_renderPoseSnapshotValid = false;
    static unsigned long long g_renderPoseSnapshotSerial = 0;
    static unsigned long long g_lastSubmittedPoseSerial = 0;

    // submit the exact pose owned by the renderer that produced the pixels.
    // this is a data handoff only: no sleeps, yields, waits, or lifecycle changes.
    struct RenderPoseHistorySlot
    {
        unsigned long long serial = 0;
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    };
    static RenderPoseHistorySlot g_renderPoseHistory[64] = {};

    // Renderer-bound submission snapshot. Execute resolves its exact pose
    // Camera/XR frame startup must never invalidate this handoff.
    static XrView g_rendererBoundViews[2] = {
        { XR_TYPE_VIEW }, { XR_TYPE_VIEW }
    };
    static volatile LONG64 g_rendererBoundPoseSerial = 0;
    static volatile LONG64 g_rendererBoundProducerSerial = 0;
    static volatile LONG g_rendererBoundViewsValid = 0;
    // intentionally pose-only; the existing game/controller input path is untouched.
    static XrActionSet g_avV2PoseActionSet = XR_NULL_HANDLE;
    static XrAction g_avV2GripPoseAction = XR_NULL_HANDLE;
    static XrPath g_controllerHands[2] = { XR_NULL_PATH, XR_NULL_PATH };
    static XrSpace g_controllerGripSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    static XrSpaceLocation g_controllerGripLocations[2] = {
        { XR_TYPE_SPACE_LOCATION }, { XR_TYPE_SPACE_LOCATION }
    };
    static bool g_controllerGripPoseValid[2] = { false, false };

    static bool AV_V2InitializeControllerPoses()
    {
        if (g_xrInstance == XR_NULL_HANDLE || g_xrSession == XR_NULL_HANDLE)
            return false;
        if (g_avV2PoseActionSet != XR_NULL_HANDLE)
            return true;

        if (XR_FAILED(xrStringToPath(g_xrInstance, "/user/hand/left", &g_controllerHands[0])) ||
            XR_FAILED(xrStringToPath(g_xrInstance, "/user/hand/right", &g_controllerHands[1])))
            return false;

        XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy_s(asci.actionSetName, "hatvr_avatar");
        strcpy_s(asci.localizedActionSetName, "HatVR Avatar");
        asci.priority = 0;
        if (XR_FAILED(xrCreateActionSet(g_xrInstance, &asci, &g_avV2PoseActionSet)))
            return false;

        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(aci.actionName, "grip_pose");
        strcpy_s(aci.localizedActionName, "Grip Pose");
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_controllerHands;
        if (XR_FAILED(xrCreateAction(g_avV2PoseActionSet, &aci, &g_avV2GripPoseAction)))
            return false;

        // Pose + game bindings are suggested together before the one attach.
        if (!InitializeVrControllerActionsOnAvatarSet(
                g_avV2PoseActionSet, g_avV2GripPoseAction))
            return false;

        XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attach.countActionSets = 1;
        attach.actionSets = &g_avV2PoseActionSet;
        if (XR_FAILED(xrAttachSessionActionSets(g_xrSession, &attach)))
            return false;

        for (int h = 0; h < 2; ++h)
        {
            XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            sci.action = g_avV2GripPoseAction;
            sci.subactionPath = g_controllerHands[h];
            sci.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(g_xrSession, &sci, &g_controllerGripSpaces[h])))
                return false;
        }
        DebugLog("AV_V2 controller grip pose actions attached\n");
        return true;
    }

    static void AV_V2UpdateControllerPoses(XrTime time)
    {
        g_controllerGripPoseValid[0] = g_controllerGripPoseValid[1] = false;
        if (g_avV2PoseActionSet == XR_NULL_HANDLE || g_xrLocalSpace == XR_NULL_HANDLE)
            return;
        XrActiveActionSet active{};
        active.actionSet = g_avV2PoseActionSet;
        XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(g_xrSession, &sync)))
            return;
        for (int h = 0; h < 2; ++h)
        {
            if (g_controllerGripSpaces[h] == XR_NULL_HANDLE) continue;
            XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
            if (XR_FAILED(xrLocateSpace(g_controllerGripSpaces[h], g_xrLocalSpace, time, &loc)))
                continue;
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((loc.locationFlags & need) != need) continue;
            g_controllerGripLocations[h] = loc;
            g_controllerGripPoseValid[h] = true;
        }
    }

    // No sleeps, gates, or lifecycle changes.
    // this is passive tracing only.
    static LARGE_INTEGER g_v72LastCameraQpc = {};
    static unsigned long long g_v72CameraSerial = 0;

    static void V72CameraMark(const char* stage)
    {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);

        char line[256] = {};
        sprintf_s(line, sizeof(line),
            "PIPE_CAMERA stage=%s camSerial=%llu present=%llu qpc=%lld\n",
            stage,
            g_v72CameraSerial,
            (unsigned long long)g_presentFrameNumber,
            (long long)now.QuadPart);
        RenderPipelineDiagnosticLog(line);

        if (stage && stage[0] == 'E') // ENTER
            g_v72LastCameraQpc = now;
    }

    static void V71PhaseLog(
        unsigned long long serial,
        unsigned long long presentBeforeWait,
        unsigned long long presentAfterWait,
        unsigned long long presentAfterBegin,
        long long qpcBeforeWait,
        long long qpcAfterWait,
        long long qpcAfterBegin,
        XrTime predictedTime,
        XrDuration predictedPeriod)
    {
        static LARGE_INTEGER freq = {};
        if (freq.QuadPart == 0)
            QueryPerformanceFrequency(&freq);
        static XrTime lastPredicted = 0;

        const double waitMs = 1000.0 * double(qpcAfterWait-qpcBeforeWait) / double(freq.QuadPart);
        const double beginMs = 1000.0 * double(qpcAfterBegin-qpcAfterWait) / double(freq.QuadPart);
        const double deltaMs = lastPredicted ? double(predictedTime-lastPredicted)/1000000.0 : 0.0;
        const double periodMs = double(predictedPeriod)/1000000.0;

        char line[384] = {};
        sprintf_s(line, sizeof(line),
            "PHASE serial=%llu present=%llu>%llu>%llu waitMs=%.3f beginMs=%.3f "
            "predicted=%lld predDeltaMs=%.3f predPeriodMs=%.3f\n",
            serial, presentBeforeWait, presentAfterWait, presentAfterBegin,
            waitMs, beginMs, (long long)predictedTime, deltaMs, periodMs);
        RenderPipelineDiagnosticLog(line);
        lastPredicted = predictedTime;
    }

    static void V70SnapshotRenderViews()
    {
        if (!g_xrViewsValidThisFrame)
        {
            g_renderPoseSnapshotValid = false;
            return;
        }

        g_renderPoseSnapshotViews[0] = g_xrViews[0];
        g_renderPoseSnapshotViews[1] = g_xrViews[1];
        g_renderPoseSnapshotValid = true;
        ++g_renderPoseSnapshotSerial;

        // Preserve this exact LOCAL-space pose/FOV by serial. The producer hook
        // later stamps this serial onto the actual SceneRenderer.
        RenderPoseHistorySlot& v79Slot = g_renderPoseHistory[g_renderPoseSnapshotSerial & 63ULL];
        v79Slot.views[0] = g_renderPoseSnapshotViews[0];
        v79Slot.views[1] = g_renderPoseSnapshotViews[1];
        v79Slot.serial = g_renderPoseSnapshotSerial;

        // keep logging intentionally sparse: first few frames, then once per 120.
        if (g_renderPoseSnapshotSerial <= 16 || (g_renderPoseSnapshotSerial % 120ULL) == 0)
        {
            char line[320] = {};
            sprintf_s(line, sizeof(line),
                "POSE_CAPTURE serial=%llu present=%llu predicted=%lld "
                "Lq=(%.6f %.6f %.6f %.6f) Rq=(%.6f %.6f %.6f %.6f)\n",
                g_renderPoseSnapshotSerial,
                (unsigned long long)g_presentFrameNumber,
                (long long)g_xrPredictedDisplayTime,
                g_renderPoseSnapshotViews[0].pose.orientation.x,
                g_renderPoseSnapshotViews[0].pose.orientation.y,
                g_renderPoseSnapshotViews[0].pose.orientation.z,
                g_renderPoseSnapshotViews[0].pose.orientation.w,
                g_renderPoseSnapshotViews[1].pose.orientation.x,
                g_renderPoseSnapshotViews[1].pose.orientation.y,
                g_renderPoseSnapshotViews[1].pose.orientation.z,
                g_renderPoseSnapshotViews[1].pose.orientation.w);
            RenderPipelineDiagnosticLog(line);
        }
    }

    // Live HMD tracking consumed by the UE3 camera hook on the next game frame.
    static bool g_xrTrackingPoseValid = false;
    static bool g_xrTrackingOriginSet = false;
    static XrVector3f g_xrTrackingOriginPosition{};
    static XrQuaternionf g_xrTrackingOriginOrientation{ 0, 0, 0, 1 };
    static XrVector3f g_xrHeadPosition{};
    // Rebase HatVR positional tracking on the first valid HMD pose after that event.
    // HatVR manual recenter captures the current HMD position as the gameplay
    // translation origin. A runtime LOCAL-space recenter is different: SteamVR
    // has already moved LOCAL itself, so HatVR must return to LOCAL's zero rather
    // than capturing whatever pose the headset happens to have afterward.
    static bool g_xrPositionRecenterPending = false;
    static bool g_xrRuntimeLocalRecenterPending = false;
    static XrQuaternionf g_xrHeadOrientation{ 0, 0, 0, 1 };
    static float g_xrWorldUnitsPerMeter = 100.0f;

    // OpenXR owns the headset presentation resolution. The game may continue
    // rendering its desktop backbuffer at any size; the CPU bridge resamples
    // NDC-equivalent pixels into runtime-sized eye swapchains.
    static bool g_xrSwapchainRecreateRequested = false;
    static uint32_t g_xrRuntimeEyeWidth = 0;

    // active native-resolution experiment. The desktop FViewport remains the real
    // window target, but its renderer-facing size and final RHI target are overridden
    // during the gameplay render path.
    static IDirect3DTexture9* g_nativeVrColorTexture = nullptr;
    static IDirect3DSurface9* g_nativeVrColorSurface = nullptr;
    static IDirect3DSurface9* g_nativeVrDepthSurface = nullptr;
    // Finished-frame mono UI extraction.  This deliberately does NOT classify
    // individual HUD draws.  We retain the pre-HUD world and subtract it from the
    // completed frame, then place the result on one OpenXR quad.
    static IDirect3DTexture9* g_uiWorldBeforeTexture = nullptr;
    static IDirect3DSurface9* g_uiWorldBeforeSurface = nullptr;
    static IDirect3DTexture9* g_uiFinalTexture = nullptr;
    static IDirect3DSurface9* g_uiFinalSurface = nullptr;
    static bool g_uiWorldBeforeValid = false;
    static bool g_uiExtractValid = false;

    static UINT g_nativeVrWidth = 0;
    static UINT g_nativeVrHeight = 0;
    static unsigned long long g_gameplayViewportResourceHandle = 0;
    static bool g_nativeVrResolutionEnabled = true;

    using FViewportGetSizeFn = unsigned int (*)(void* viewport);
    static FViewportGetSizeFn g_originalViewportGetSizeX = nullptr;
    static FViewportGetSizeFn g_originalViewportGetSizeY = nullptr;

    using RHISetRenderTargetFn = void (*)(void* rhi, unsigned long long colorHandle, unsigned long long depthHandle);
    static RHISetRenderTargetFn g_originalRHISetRenderTarget = nullptr;

    static bool g_rhi4B0Logged = false;

    static bool g_rhiBackend2A0Logged = false;

    // the RHI owns a D3D9 device that is not necessarily the device returned
    // through our proxy CreateDevice path.  Keep this target and trampoline
    // separate from the proxy-device DIPUP hook: MinHook trampolines are tied
    // to one exact function address and must never be shared across targets.
    static DrawIndexedPrimitiveUPFn g_originalRHIBackendDrawIndexedPrimitiveUP = nullptr;
    static bool g_rhiBackendDipupHookInstalled = false;
    static void* g_rhiBackendDipupTarget = nullptr;
    static bool g_rhiBackendDipupCoveredByExistingHook = false;
    static unsigned int g_uiDipupLogCount = 0;

    static HRESULT STDMETHODCALLTYPE HookedRHIBackendDrawIndexedPrimitiveUP(
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type,
        UINT minVertexIndex, UINT numVertices, UINT primitiveCount,
        const void* indexData, D3DFORMAT indexFormat,
        const void* vertexData, UINT stride);
    static bool InstallRHIBackendDipupHook(void* target);
    static void DiscoverRHIBackend2A0(void* rhi)
    {
        if (g_rhiBackend2A0Logged || !rhi)
            return;

        const unsigned char* rhiBytes =
            reinterpret_cast<const unsigned char*>(rhi);

        // FUN_140A1E400:
        //   plVar2 = *(longlong **)(param_1 + 0x34);
        //   (**(code **)(*plVar2 + 0x2A0))(...)
        void* backend =
            *reinterpret_cast<void* const*>(rhiBytes + 0x34);
        if (!backend)
            return;

        void** backendVtable = *reinterpret_cast<void***>(backend);
        if (!backendVtable)
            return;

        void* target = backendVtable[0x2A0 / 8];

        // Install directly on the exact backend function resolved from
        // RHI+0x34.  This is the device FUN_140A1E400 actually submits through.
        const bool hookInstalled = InstallRHIBackendDipupHook(target);

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t targetAddr = reinterpret_cast<uintptr_t>(target);
        const uintptr_t targetRva =
            (exe && targetAddr >= exeBase) ? targetAddr - exeBase : 0;

        HMODULE owner = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(target), &owner);

        char ownerPath[MAX_PATH] = {};
        if (owner)
            GetModuleFileNameA(owner, ownerPath, MAX_PATH);

        char line[640] = {};
        sprintf_s(
            line, sizeof(line),
            "RHI_BACKEND_2A0 rhi=%p backend@+34=%p backendVtable=%p "
            "slot=84 target=%p exeRVA=0x%llX owner=%p ownerPath=%s hook=%s\n",
            rhi, backend, backendVtable, target,
            static_cast<unsigned long long>(targetRva),
            owner, ownerPath[0] ? ownerPath : "<unknown>",
            hookInstalled ? "installed" : "FAILED");

        RHIBackend2A0Log(line);
        g_rhiBackend2A0Logged = true;
    }
    static void DiscoverRHI4B0(void* rhi)
    {
        if (g_rhi4B0Logged || !rhi)
            return;

        void** vt = *reinterpret_cast<void***>(rhi);
        if (!vt)
            return;

        void* target = vt[0x4B0 / 8];
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
        const uintptr_t rva = (exe && addr >= base) ? addr - base : 0;

        char line[320] = {};
        sprintf_s(line, sizeof(line),
            "RHI4B0 rhi=%p vtable=%p slot=150 target=%p exeRVA=0x%llX\n",
            rhi, vt, target, static_cast<unsigned long long>(rva));
        RHI4B0Log(line);
        g_rhi4B0Logged = true;
    }

    static uint32_t g_xrRuntimeEyeHeight = 0;

    // In VR, mouse/controller camera pitch should not tilt the player's head.
    // Capture the gameplay camera's neutral pitch once, preserve gameplay yaw,
    // then layer physical HMD pitch/yaw/roll on top.
    static bool g_vrPitchLockEnabled = true;

    // the OpenXR frustum is vertically asymmetric (currently +42/-54 deg).
    // if the engine's row-vector convention expects the opposite Y-center
    // sign, the whole world appears pitched down even with correct tracking.
    // Default to the flipped convention; F3 toggles it live for an A/B test.
    static bool g_flipXrVerticalProjectionCenter = true;

    static XrQuaternionf XrQuatConjugate(const XrQuaternionf& q)
    {
        return { -q.x, -q.y, -q.z, q.w };
    }

    static XrQuaternionf XrQuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }

    static XrVector3f XrQuatRotate(const XrQuaternionf& q, const XrVector3f& v)
    {
        const XrQuaternionf p{ v.x, v.y, v.z, 0.0f };
        const XrQuaternionf r = XrQuatMul(XrQuatMul(q, p), XrQuatConjugate(q));
        return { r.x, r.y, r.z };
    }

    static XrPosef XrPoseRelativeToTrackingOrigin(const XrPosef& pose)
    {
        XrPosef relative{};

        const XrQuaternionf invOrigin =
            XrQuatConjugate(g_xrTrackingOriginOrientation);

        relative.orientation =
            XrQuatMul(invOrigin, pose.orientation);

        const XrVector3f deltaWorld{
            pose.position.x - g_xrTrackingOriginPosition.x,
            pose.position.y - g_xrTrackingOriginPosition.y,
            pose.position.z - g_xrTrackingOriginPosition.z
        };

        relative.position =
            XrQuatRotate(invOrigin, deltaWorld);

        return relative;
    }

    static int DegreesToUnrealRotator(float degrees)
    {
        return static_cast<int>(degrees * (65536.0f / 360.0f));
    }

    static PFN_xrGetD3D12GraphicsRequirementsKHR
        p_xrGetD3D12GraphicsRequirementsKHR = nullptr;

    template<typename T>
    static bool LoadXrInstanceProc(const char* name, T& out)
    {
        PFN_xrVoidFunction fn = nullptr;
        const XrResult xr = xrGetInstanceProcAddr(g_xrInstance, name, &fn);
        if (XR_FAILED(xr) || !fn)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "OpenXR missing proc: %s result=%d\n", name, xr);
            DebugLog(line);
            return false;
        }
        out = reinterpret_cast<T>(fn);
        return true;
    }

    static double XrRadiansToDegrees(float radians)
    {
        return static_cast<double>(radians) * 57.2957795130823208768;
    }

    static bool CreateOpenXRD3D12Device(const XrGraphicsRequirementsD3D12KHR& req)
    {
        IDXGIFactory6* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "OpenXR: CreateDXGIFactory1 failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }

        IDXGIAdapter1* matchedAdapter = nullptr;
        DXGI_ADAPTER_DESC1 matchedDesc{};

        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!adapter)
                continue;

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.HighPart == req.adapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == req.adapterLuid.LowPart)
            {
                matchedAdapter = adapter;
                matchedDesc = desc;
                break;
            }
            adapter->Release();
        }

        factory->Release();

        if (!matchedAdapter)
        {
            DebugLog("OpenXR: could not find DXGI adapter matching runtime LUID\n");
            return false;
        }

        char adapterName[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, matchedDesc.Description, -1,
            adapterName, sizeof(adapterName), nullptr, nullptr);

        char line[512] = {};
        sprintf_s(line, sizeof(line),
            "OpenXR: matched adapter '%s' LUID=%08X:%08X requiredFeature=0x%X\n",
            adapterName,
            (unsigned)matchedDesc.AdapterLuid.HighPart,
            (unsigned)matchedDesc.AdapterLuid.LowPart,
            (unsigned)req.minFeatureLevel);
        DebugLog(line);

        hr = D3D12CreateDevice(
            matchedAdapter,
            req.minFeatureLevel,
            IID_PPV_ARGS(&g_xrD3D12Device));
        matchedAdapter->Release();

        if (FAILED(hr) || !g_xrD3D12Device)
        {
            sprintf_s(line, sizeof(line), "OpenXR: D3D12CreateDevice failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        hr = g_xrD3D12Device->CreateCommandQueue(
            &queueDesc, IID_PPV_ARGS(&g_xrD3D12Queue));
        if (FAILED(hr) || !g_xrD3D12Queue)
        {
            sprintf_s(line, sizeof(line), "OpenXR: CreateCommandQueue failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }

        hr = g_xrD3D12Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_xrCommandAllocator));
        if (FAILED(hr))
        {
            sprintf_s(line, sizeof(line), "OpenXR: CreateCommandAllocator failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }

        hr = g_xrD3D12Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_xrCommandAllocator, nullptr,
            IID_PPV_ARGS(&g_xrCommandList));
        if (FAILED(hr))
        {
            sprintf_s(line, sizeof(line), "OpenXR: CreateCommandList failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }
        g_xrCommandList->Close();

        hr = g_xrD3D12Device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_xrFence));
        if (FAILED(hr))
        {
            sprintf_s(line, sizeof(line), "OpenXR: CreateFence failed hr=0x%08X\n", (unsigned)hr);
            DebugLog(line);
            return false;
        }

        g_xrFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_xrFenceEvent)
        {
            DebugLog("OpenXR: CreateEvent for copy fence failed\n");
            return false;
        }

        DebugLog("OpenXR: dedicated D3D12 device + DIRECT queue ready (CPU image bridge enabled)\n");
        return true;
    }
