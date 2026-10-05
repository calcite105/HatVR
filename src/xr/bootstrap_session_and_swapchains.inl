    static bool InitializeOpenXRBootstrap(IDirect3DDevice9* d3d9Device)
    {
        DebugLog("DXVK-V7-XR: bootstrap START\n");
        DxvkPathTrace("V9 XR bootstrap START device=%p", d3d9Device);
        if (g_xrInitialized)
        {
            DebugLog("DXVK-V7-XR: already initialized\n");
            return true;
        }

        const char* extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };

        XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
            "A Hat in Time VR");
        createInfo.applicationInfo.applicationVersion = 1;
        strcpy_s(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE,
            "UE3-AHiT-VR-Mod");
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        createInfo.enabledExtensionCount = 1;
        createInfo.enabledExtensionNames = extensions;

        DxvkPathTrace("V15 xrCreateInstance FORENSICS BEGIN");

        LARGE_INTEGER v15XrCiStart{}, v15XrCiEnd{}, v15XrCiFreq{};
        QueryPerformanceFrequency(&v15XrCiFreq);
        QueryPerformanceCounter(&v15XrCiStart);
        XrResult xr = xrCreateInstance(&createInfo, &g_xrInstance);
        QueryPerformanceCounter(&v15XrCiEnd);

        const double v15XrCiMs = v15XrCiFreq.QuadPart > 0
            ? (double)(v15XrCiEnd.QuadPart - v15XrCiStart.QuadPart) * 1000.0 /
              (double)v15XrCiFreq.QuadPart
            : 0.0;
        DxvkPathTrace("V15 xrCreateInstance RETURN result=%d instance=%p elapsedMs=%.3f",
            (int)xr, g_xrInstance, v15XrCiMs);
        DxvkPathTrace("V15 xrCreateInstance FORENSICS END");
        if (XR_FAILED(xr))
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "OpenXR: xrCreateInstance failed %d\n", xr);
            DebugLog(line);
            return false;
        }

        XrInstanceProperties instanceProps{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_xrInstance, &instanceProps)))
        {
            char line[512] = {};
            sprintf_s(line, sizeof(line), "OpenXR runtime: %s version=%u.%u.%u\n",
                instanceProps.runtimeName,
                XR_VERSION_MAJOR(instanceProps.runtimeVersion),
                XR_VERSION_MINOR(instanceProps.runtimeVersion),
                XR_VERSION_PATCH(instanceProps.runtimeVersion));
            DebugLog(line);
        }

        DebugLog("DXVK-V7-XR: xrCreateInstance SUCCESS\n");
        DxvkPathTrace("V9 XR xrCreateInstance SUCCESS instance=%p", g_xrInstance);
        if (!LoadXrInstanceProc("xrGetD3D12GraphicsRequirementsKHR",
            p_xrGetD3D12GraphicsRequirementsKHR))
        {
            DebugLog("DXVK-V7-XR: LoadXrInstanceProc(xrGetD3D12GraphicsRequirementsKHR) FAILED\n");
            return false;
        }
        DebugLog("DXVK-V7-XR: D3D12 requirements proc loaded\n");
        DxvkPathTrace("V9 XR D3D12 requirements proc loaded");

        XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr = xrGetSystem(g_xrInstance, &systemInfo, &g_xrSystemId);
        if (XR_FAILED(xr))
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "OpenXR: xrGetSystem(HMD) failed %d\n", xr);
            DebugLog(line);
            return false;
        }

        XrSystemProperties systemProps{ XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties(g_xrInstance, g_xrSystemId, &systemProps)))
        {
            char line[512] = {};
            sprintf_s(line, sizeof(line),
                "OpenXR system: '%s' vendorId=%u maxSwapchain=%ux%u maxLayers=%u\n",
                systemProps.systemName, systemProps.vendorId,
                systemProps.graphicsProperties.maxSwapchainImageWidth,
                systemProps.graphicsProperties.maxSwapchainImageHeight,
                systemProps.graphicsProperties.maxLayerCount);
            DebugLog(line);
        }

        uint32_t viewCount = 0;
        xr = xrEnumerateViewConfigurationViews(
            g_xrInstance, g_xrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &viewCount, nullptr);
        if (XR_FAILED(xr) || viewCount < 2)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "OpenXR: PRIMARY_STEREO count query failed %d count=%u\n", xr, viewCount);
            DebugLog(line);
            return false;
        }

        uint32_t outCount = 0;
        xr = xrEnumerateViewConfigurationViews(
            g_xrInstance, g_xrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            2, &outCount, g_xrViewConfig);
        if (XR_FAILED(xr) || outCount < 2)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "OpenXR: PRIMARY_STEREO view query failed %d count=%u\n", xr, outCount);
            DebugLog(line);
            return false;
        }

        char line[512] = {};
        sprintf_s(line, sizeof(line),
            "OpenXR recommended eyes: LEFT=%ux%u samples=%u RIGHT=%ux%u samples=%u\n",
            g_xrViewConfig[0].recommendedImageRectWidth,
            g_xrViewConfig[0].recommendedImageRectHeight,
            g_xrViewConfig[0].recommendedSwapchainSampleCount,
            g_xrViewConfig[1].recommendedImageRectWidth,
            g_xrViewConfig[1].recommendedImageRectHeight,
            g_xrViewConfig[1].recommendedSwapchainSampleCount);
        DebugLog(line);

        g_xrRuntimeEyeWidth = g_xrViewConfig[0].recommendedImageRectWidth;
        g_xrRuntimeEyeHeight = g_xrViewConfig[0].recommendedImageRectHeight;

        XrGraphicsRequirementsD3D12KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        xr = p_xrGetD3D12GraphicsRequirementsKHR(g_xrInstance, g_xrSystemId, &req);
        if (XR_FAILED(xr))
        {
            sprintf_s(line, sizeof(line),
                "OpenXR: xrGetD3D12GraphicsRequirementsKHR failed %d\n", xr);
            DebugLog(line);
            return false;
        }

        sprintf_s(line, sizeof(line),
            "OpenXR D3D12 requirements: featureLevel=0x%X adapterLuid=%08X:%08X\n",
            (unsigned)req.minFeatureLevel,
            (unsigned)req.adapterLuid.HighPart,
            (unsigned)req.adapterLuid.LowPart);
        DebugLog(line);

        (void)d3d9Device;
        DebugLog("DXVK-V7-XR: CreateOpenXRD3D12Device ENTER\n");
        DxvkPathTrace("V9 XR CreateOpenXRD3D12Device ENTER");
        if (!CreateOpenXRD3D12Device(req))
        {
            DebugLog("DXVK-V7-XR: CreateOpenXRD3D12Device FAILED\n");
            DxvkPathTrace("V9 XR CreateOpenXRD3D12Device FAILED");
            return false;
        }
        DebugLog("DXVK-V7-XR: CreateOpenXRD3D12Device SUCCESS\n");
        DxvkPathTrace("V9 XR CreateOpenXRD3D12Device SUCCESS device=%p queue=%p", g_xrD3D12Device, g_xrD3D12Queue);

        XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        binding.device = g_xrD3D12Device;
        binding.queue = g_xrD3D12Queue;

        XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
        sessionInfo.next = &binding;
        sessionInfo.systemId = g_xrSystemId;

        xr = xrCreateSession(g_xrInstance, &sessionInfo, &g_xrSession);
        if (XR_FAILED(xr))
        {
            sprintf_s(line, sizeof(line), "OpenXR: xrCreateSession failed %d\n", xr);
            DebugLog(line);
            DxvkPathTrace("V9 XR xrCreateSession FAILED result=%d", xr);
            return false;
        }
        DebugLog("OPENXR SESSION CREATED\n");
        DxvkPathTrace("V9 XR SESSION CREATED session=%p", g_xrSession);

        XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        xr = xrCreateReferenceSpace(g_xrSession, &spaceInfo, &g_xrLocalSpace);
        if (XR_FAILED(xr))
        {
            sprintf_s(line, sizeof(line), "OpenXR: xrCreateReferenceSpace(LOCAL) failed %d\n", xr);
            DebugLog(line);
            return false;
        }
        DebugLog("OpenXR LOCAL reference space created\n");

        if (!AV_V2InitializeControllerPoses())
            DebugLog("AV_V2 controller grip pose initialization failed; avatar hands will stay animation-driven\n");
        // create/attach another OpenXR action set.
        if (!InitializeVrControllerInput())
            DebugLog("MOTION_V3 swing bridge initialization failed\n");

        g_xrInitialized = true;
        DebugLog("DXVK-V7-XR: bootstrap COMPLETE SUCCESS\n");
        DxvkPathTrace("V9 XR bootstrap COMPLETE SUCCESS");
        return true;
    }

    static void PollOpenXREvents()
    {
        if (!g_xrInitialized || g_xrInstance == XR_NULL_HANDLE)
            return;

        for (;;)
        {
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            const XrResult xr = xrPollEvent(g_xrInstance, &event);
            if (xr == XR_EVENT_UNAVAILABLE)
                break;
            if (XR_FAILED(xr))
                break;

            if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
            {
                const auto* changed =
                    reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
                if (changed->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
                {
                    g_xrRuntimeLocalRecenterPending = true;
                    DebugLog("XR_RECENTER LOCAL reference-space change detected; using runtime LOCAL origin\n");
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                g_xrSessionState = changed->state;
                DxvkPathTrace("V10 XR SESSION STATE=%d running=%d",
                    (int)changed->state, g_xrSessionRunning ? 1 : 0);

                char line[256] = {};
                sprintf_s(line, sizeof(line), "OpenXR session state -> %d\n", (int)changed->state);
                DebugLog(line);

                if (changed->state == XR_SESSION_STATE_READY && !g_xrSessionRunning)
                {
                    XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    DxvkPathTrace("V10 xrBeginSession ENTER session=%p", g_xrSession);
                    const XrResult beginResult = xrBeginSession(g_xrSession, &begin);
                    DxvkPathTrace("V10 xrBeginSession RETURN result=%d", (int)beginResult);
                    if (XR_SUCCEEDED(beginResult))
                    {
                        g_xrSessionRunning = true;
                        DebugLog("OPENXR SESSION RUNNING\n");
                    }
                    else
                    {
                        sprintf_s(line, sizeof(line), "OpenXR: xrBeginSession failed %d\n", beginResult);
                        DebugLog(line);
                    }
                }
                else if (changed->state == XR_SESSION_STATE_STOPPING && g_xrSessionRunning)
                {
                    xrEndSession(g_xrSession);
                    g_xrSessionRunning = false;
                    DebugLog("OpenXR session stopped\n");
                }
                else if (changed->state == XR_SESSION_STATE_EXITING ||
                    changed->state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    g_xrExitRequested = true;
                }
            }
        }
    }

    static void RefreshOpenXRRuntimeEyeExtent()
    {
        if (g_xrInstance == XR_NULL_HANDLE || g_xrSystemId == XR_NULL_SYSTEM_ID)
            return;

        uint32_t count = 0;
        XrResult xr = xrEnumerateViewConfigurationViews(
            g_xrInstance, g_xrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &count, nullptr);
        if (XR_FAILED(xr) || count < 2)
            return;

        std::vector<XrViewConfigurationView> views(count);
        for (auto& v : views) v = { XR_TYPE_VIEW_CONFIGURATION_VIEW };

        xr = xrEnumerateViewConfigurationViews(
            g_xrInstance, g_xrSystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            count, &count, views.data());
        if (XR_FAILED(xr))
            return;

        const uint32_t w = views[0].recommendedImageRectWidth;
        const uint32_t h = views[0].recommendedImageRectHeight;
        if (!w || !h) return;

        if (w != g_xrRuntimeEyeWidth || h != g_xrRuntimeEyeHeight)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "OPENXR RUNTIME EYE EXTENT CHANGED %ux%u -> %ux%u; recreating swapchains\\n",
                g_xrRuntimeEyeWidth, g_xrRuntimeEyeHeight, w, h);
            DebugLog(line);
            g_xrRuntimeEyeWidth = w;
            g_xrRuntimeEyeHeight = h;
            g_xrSwapchainRecreateRequested = true;
        }
    }

    static void DestroyOpenXREyeSwapchains()
    {
        for (int eye = 0; eye < 2; ++eye)
        {
            g_xrSwapchainImages[eye].clear();
            if (g_xrEyeSwapchains[eye] != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(g_xrEyeSwapchains[eye]);
                g_xrEyeSwapchains[eye] = XR_NULL_HANDLE;
            }
        }
        g_xrSwapchainWidth = 0;
        g_xrSwapchainHeight = 0;
        g_xrSwapchainFormat = 0;
    }

    static bool EnsureOpenXREyeSwapchains(uint32_t width, uint32_t height)
    {
        if (g_xrSwapchainRecreateRequested)
        {
            DebugLog("OpenXR: live resolution change -> recreating eye swapchains\n");
            DestroyOpenXREyeSwapchains();
            g_xrSwapchainRecreateRequested = false;
        }

        if (g_xrEyeSwapchains[0] != XR_NULL_HANDLE &&
            g_xrEyeSwapchains[1] != XR_NULL_HANDLE &&
            g_xrSwapchainWidth == width &&
            g_xrSwapchainHeight == height)
            return true;

        if (g_xrEyeSwapchains[0] != XR_NULL_HANDLE ||
            g_xrEyeSwapchains[1] != XR_NULL_HANDLE)
        {
            DestroyOpenXREyeSwapchains();
        }

        uint32_t formatCount = 0;
        XrResult xr = xrEnumerateSwapchainFormats(
            g_xrSession, 0, &formatCount, nullptr);
        if (XR_FAILED(xr) || formatCount == 0)
        {
            DebugLog("OpenXR: xrEnumerateSwapchainFormats(count) failed\n");
            return false;
        }

        std::vector<int64_t> formats(formatCount);
        xr = xrEnumerateSwapchainFormats(
            g_xrSession, formatCount, &formatCount, formats.data());
        if (XR_FAILED(xr))
            return false;

        const int64_t wanted = static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        if (std::find(formats.begin(), formats.end(), wanted) == formats.end())
        {
            DebugLog("OpenXR: BGRA8_UNORM_SRGB not supported\n");
            return false;
        }

        g_xrSwapchainFormat = wanted;
        g_xrSwapchainWidth = width;
        g_xrSwapchainHeight = height;

        for (int eye = 0; eye < 2; ++eye)
        {
            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags =
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ci.format = g_xrSwapchainFormat;
            ci.sampleCount = 1;
            ci.width = width;
            ci.height = height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;

            xr = xrCreateSwapchain(g_xrSession, &ci, &g_xrEyeSwapchains[eye]);
            if (XR_FAILED(xr))
            {
                char line[256] = {};
                sprintf_s(line, sizeof(line),
                    "OpenXR: xrCreateSwapchain eye=%d failed %d\n", eye, xr);
                DebugLog(line);
                DestroyOpenXREyeSwapchains();
                return false;
            }

            uint32_t imageCount = 0;
            xr = xrEnumerateSwapchainImages(
                g_xrEyeSwapchains[eye], 0, &imageCount, nullptr);
            if (XR_FAILED(xr) || imageCount == 0)
            {
                DestroyOpenXREyeSwapchains();
                return false;
            }

            g_xrSwapchainImages[eye].resize(imageCount);
            for (auto& image : g_xrSwapchainImages[eye])
                image = { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR };

            xr = xrEnumerateSwapchainImages(
                g_xrEyeSwapchains[eye],
                imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(
                    g_xrSwapchainImages[eye].data()));
            if (XR_FAILED(xr))
            {
                DestroyOpenXREyeSwapchains();
                return false;
            }
        }

        char line[256] = {};
        sprintf_s(line, sizeof(line),
            "OPENXR EYE SWAPCHAINS CREATED %ux%u (live runtime extent) BGRA8_SRGB\n",
            width, height);
        DebugLog(line);
        return true;
    }

    static bool WaitForXRFence(UINT64 value)
    {
        if (g_xrFence->GetCompletedValue() >= value)
            return true;
        if (FAILED(g_xrFence->SetEventOnCompletion(value, g_xrFenceEvent)))
            return false;
        return WaitForSingleObject(g_xrFenceEvent, 5000) == WAIT_OBJECT_0;
    }

    struct VrPerfAccumulator
    {
        LARGE_INTEGER frequency{};
        unsigned long long frames = 0;
        double readbackMs = 0.0;
        double cpuCopyMs = 0.0;
        double d3d12SubmitWaitMs = 0.0;
        double xrSubmitMs = 0.0;
        double worstReadbackMs = 0.0;
        double worstCpuCopyMs = 0.0;
        double worstD3D12Ms = 0.0;
    };
    static VrPerfAccumulator g_vrPerf{};

    static double PerfMs(LARGE_INTEGER a, LARGE_INTEGER b)
    {
        if (!g_vrPerf.frequency.QuadPart)
            QueryPerformanceFrequency(&g_vrPerf.frequency);
        return double(b.QuadPart - a.QuadPart) * 1000.0 /
            double(g_vrPerf.frequency.QuadPart);
    }
    static void FlushPerfSummaryIfNeeded()
    {
        if (g_vrPerf.frames < 300)
            return;
        const double n = double(g_vrPerf.frames);
        char line[512] = {};
        sprintf_s(line, sizeof(line),
            "300f AVG per-eye-call: readback=%.3fms cpuCopy=%.3fms d3d12SubmitWait=%.3fms | worst readback=%.3f cpuCopy=%.3f d3d12=%.3f | xrSubmit/frame=%.3fms\n",
            g_vrPerf.readbackMs / n,
            g_vrPerf.cpuCopyMs / n,
            g_vrPerf.d3d12SubmitWaitMs / n,
            g_vrPerf.worstReadbackMs,
            g_vrPerf.worstCpuCopyMs,
            g_vrPerf.worstD3D12Ms,
            g_vrPerf.xrSubmitMs / (n * 0.5));
        PerfLog(line);
        const LARGE_INTEGER freq = g_vrPerf.frequency;
        g_vrPerf = {};
        g_vrPerf.frequency = freq;
    }

    static bool CopyD3D9EyeToXR(
        IDirect3DTexture9* source9,
        ID3D12Resource* destination12)
    {
        static unsigned long long v12BridgeCalls = 0;
        ++v12BridgeCalls;
        const bool v12Trace = v12BridgeCalls <= 16 || (v12BridgeCalls % 120ULL) == 0;
        if (v12Trace) DxvkPathTrace("V12 BRIDGE ENTER call=%llu src=%p dst=%p",
            v12BridgeCalls, source9, destination12);

        if (!source9 || !destination12 || !g_xrD3D12Device || !g_xrD3D12Queue ||
            !g_xrCommandAllocator || !g_xrCommandList || !g_xrFence)
            return false;

        IDirect3DDevice9* device9 = nullptr;
        IDirect3DSurface9* sourceSurface = nullptr;
        HRESULT hr = source9->GetDevice(&device9);
        if (FAILED(hr) || !device9) goto cleanup;

        hr = source9->GetSurfaceLevel(0, &sourceSurface);
        if (FAILED(hr) || !sourceSurface) goto cleanup;

        D3DSURFACE_DESC srcDesc{};
        hr = sourceSurface->GetDesc(&srcDesc);
        if (FAILED(hr)) goto cleanup;
        if (srcDesc.Format != D3DFMT_A8R8G8B8 && srcDesc.Format != D3DFMT_X8R8G8B8)
        {
            hr = E_FAIL;
            goto cleanup;
        }

        CpuBridgeCache* cache = nullptr;
        for (auto& c : g_cpuBridgeCache)
            if (c.source == source9) { cache = &c; break; }
        if (!cache)
            for (auto& c : g_cpuBridgeCache)
                if (!c.source) { cache = &c; break; }
        if (!cache) cache = &g_cpuBridgeCache[0];

        const D3D12_RESOURCE_DESC dstDesc = destination12->GetDesc();
        const bool mismatch =
            cache->source != source9 || cache->width != srcDesc.Width ||
            cache->height != srcDesc.Height || cache->format != srcDesc.Format ||
            cache->footprint.Footprint.Width != dstDesc.Width ||
            cache->footprint.Footprint.Height != dstDesc.Height;

        if (mismatch)
        {
            ReleaseCpuBridgeCache(*cache);
            cache->source = source9;
            cache->width = srcDesc.Width;
            cache->height = srcDesc.Height;
            cache->format = srcDesc.Format;

            hr = device9->CreateOffscreenPlainSurface(
                srcDesc.Width, srcDesc.Height, srcDesc.Format,
                D3DPOOL_SYSTEMMEM, &cache->systemSurface, nullptr);
            DxvkPathTrace("V12 BRIDGE CreateOffscreen call=%llu hr=0x%08X size=%ux%u surface=%p",
                v12BridgeCalls, (unsigned)hr, srcDesc.Width, srcDesc.Height, cache->systemSurface);
            if (FAILED(hr) || !cache->systemSurface) goto cleanup;

            UINT64 rowSize = 0;
            g_xrD3D12Device->GetCopyableFootprints(
                &dstDesc, 0, 1, 0, &cache->footprint,
                &cache->numRows, &rowSize, &cache->uploadSize);

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = cache->uploadSize;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            hr = g_xrD3D12Device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cache->upload));
            DxvkPathTrace("V12 BRIDGE CreateCommittedResource call=%llu hr=0x%08X bytes=%llu upload=%p",
                v12BridgeCalls, (unsigned)hr, (unsigned long long)cache->uploadSize, cache->upload);
            if (FAILED(hr) || !cache->upload) goto cleanup;

            D3D12_RANGE noRead{ 0, 0 };
            hr = cache->upload->Map(0, &noRead, &cache->mappedUpload);
            if (FAILED(hr) || !cache->mappedUpload) goto cleanup;
        }

        LARGE_INTEGER perfA{}, perfB{};
        QueryPerformanceCounter(&perfA);
        if (v12Trace) DxvkPathTrace("V12 BRIDGE before GetRenderTargetData call=%llu", v12BridgeCalls);
        hr = device9->GetRenderTargetData(sourceSurface, cache->systemSurface);
        QueryPerformanceCounter(&perfB);
        if (v12Trace || FAILED(hr))
            DxvkPathTrace("V12 BRIDGE GetRenderTargetData RETURN call=%llu hr=0x%08X",
                v12BridgeCalls, (unsigned)hr);
        const double readbackCost = PerfMs(perfA, perfB);
        g_vrPerf.readbackMs += readbackCost;
        g_vrPerf.worstReadbackMs =
            (std::max)(g_vrPerf.worstReadbackMs, readbackCost);
        if (FAILED(hr)) goto cleanup;

        QueryPerformanceCounter(&perfA);
        D3DLOCKED_RECT locked{};
        hr = cache->systemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) goto cleanup;

        const UINT dstWidth = cache->footprint.Footprint.Width;
        const UINT dstHeight = cache->footprint.Footprint.Height;
        const UINT rows = (std::min)(dstHeight, cache->numRows);
        auto* mapped = static_cast<unsigned char*>(cache->mappedUpload);

        if (srcDesc.Width == dstWidth && srcDesc.Height == dstHeight)
        {
            const size_t rowBytes = size_t(dstWidth) * sizeof(uint32_t);
            for (UINT y = 0; y < rows; ++y)
            {
                const void* s = static_cast<const unsigned char*>(locked.pBits) +
                    size_t(y) * size_t(locked.Pitch);
                void* d = mapped + cache->footprint.Offset +
                    size_t(y) * cache->footprint.Footprint.RowPitch;
                memcpy(d, s, rowBytes);
            }
        }
        else
        {
            for (UINT y = 0; y < rows; ++y)
            {
                const UINT sy = UINT((uint64_t(y) * srcDesc.Height) / dstHeight);
                const uint32_t* s = reinterpret_cast<const uint32_t*>(
                    static_cast<const unsigned char*>(locked.pBits) +
                    size_t((std::min)(sy, srcDesc.Height - 1)) * size_t(locked.Pitch));
                uint32_t* d = reinterpret_cast<uint32_t*>(
                    mapped + cache->footprint.Offset +
                    size_t(y) * cache->footprint.Footprint.RowPitch);
                for (UINT x = 0; x < dstWidth; ++x)
                {
                    const UINT sx = UINT((uint64_t(x) * srcDesc.Width) / dstWidth);
                    d[x] = s[(std::min)(sx, srcDesc.Width - 1)];
                }
            }
        }
        cache->systemSurface->UnlockRect();
        QueryPerformanceCounter(&perfB);
        const double cpuCopyCost = PerfMs(perfA, perfB);
        g_vrPerf.cpuCopyMs += cpuCopyCost;
        g_vrPerf.worstCpuCopyMs =
            (std::max)(g_vrPerf.worstCpuCopyMs, cpuCopyCost);

        QueryPerformanceCounter(&perfA);
        hr = g_xrCommandAllocator->Reset();
        if (SUCCEEDED(hr)) hr = g_xrCommandList->Reset(g_xrCommandAllocator, nullptr);
        if (FAILED(hr)) goto cleanup;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination12;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_xrCommandList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = destination12;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = cache->upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = cache->footprint;
        g_xrCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g_xrCommandList->ResourceBarrier(1, &barrier);

        hr = g_xrCommandList->Close();
        if (FAILED(hr)) goto cleanup;
        {
            if (v12Trace) DxvkPathTrace("V12 BRIDGE before ExecuteCommandLists call=%llu", v12BridgeCalls);
            ID3D12CommandList* lists[] = { g_xrCommandList };
            g_xrD3D12Queue->ExecuteCommandLists(1, lists);
            if (v12Trace) DxvkPathTrace("V12 BRIDGE after ExecuteCommandLists call=%llu", v12BridgeCalls);
        }
        {
            const UINT64 value = ++g_xrFenceValue;
            hr = g_xrD3D12Queue->Signal(g_xrFence, value);
            if (v12Trace || FAILED(hr))
                DxvkPathTrace("V12 BRIDGE Signal call=%llu fence=%llu hr=0x%08X",
                    v12BridgeCalls, (unsigned long long)value, (unsigned)hr);
            if (SUCCEEDED(hr))
            {
                const bool fenceOk = WaitForXRFence(value);
                if (v12Trace || !fenceOk)
                    DxvkPathTrace("V12 BRIDGE FenceWait call=%llu fence=%llu ok=%d",
                        v12BridgeCalls, (unsigned long long)value, fenceOk ? 1 : 0);
                if (!fenceOk) hr = E_FAIL;
            }
        }
        QueryPerformanceCounter(&perfB);
        {
            const double d3d12Cost = PerfMs(perfA, perfB);
            g_vrPerf.d3d12SubmitWaitMs += d3d12Cost;
            g_vrPerf.worstD3D12Ms =
                (std::max)(g_vrPerf.worstD3D12Ms, d3d12Cost);
            ++g_vrPerf.frames; // this is per eye-copy call
        }

    cleanup:
        if (v12Trace || FAILED(hr))
            DxvkPathTrace("V12 BRIDGE EXIT call=%llu hr=0x%08X success=%d",
                v12BridgeCalls, (unsigned)hr, SUCCEEDED(hr) ? 1 : 0);
        if (sourceSurface) sourceSurface->Release();
        if (device9) device9->Release();
        return SUCCEEDED(hr);
    }

