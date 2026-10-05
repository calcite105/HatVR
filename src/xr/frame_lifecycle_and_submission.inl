    static void BeginOpenXRFrameForRender()
    {
        static unsigned long long v10BeginCalls = 0;
        ++v10BeginCalls;
        if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0)
            DxvkPathTrace("V10 BeginOpenXRFrameForRender call=%llu initialized=%d running=%d exit=%d frameBegun=%d present=%llu",
                v10BeginCalls, g_xrInitialized ? 1 : 0, g_xrSessionRunning ? 1 : 0,
                g_xrExitRequested ? 1 : 0, g_xrFrameBegun ? 1 : 0,
                (unsigned long long)g_presentFrameNumber);
        if (g_xrFrameBegun)
            return;

        g_xrFrameBegun = false;
        g_xrFrameShouldRender = false;
        g_xrViewsValidThisFrame = false;
        g_renderPoseSnapshotValid = false;

        // DXVK/Vulkan backend owns a separate OpenXR session. Publish its
        // downstream camera/projection/avatar feature keeps using the same
        if (IsVulkanBackendActive())
        {
            XrView vkViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            XrTime vkPredicted = 0;
            bool vkShouldRender = false;
            if (!BeginVulkanOpenXRFrame(vkViews, &vkPredicted, &vkShouldRender))
                return;

            g_xrFrameBegun = true;
            g_xrFrameShouldRender = vkShouldRender;
            g_xrPredictedDisplayTime = vkPredicted;

            // Vulkan session. This restores grip poses, avatar arm/head
            // publishing, XInput emulation, menu buttons, sticks and swing.
            g_xrSessionRunning = IsVulkanXrSessionRunning();
            if (g_xrSessionRunning)
            {
                AV_V2UpdateControllerPoses(vkPredicted);
                SyncVrControllerActions();
            }
            g_xrViews[0] = vkViews[0];
            g_xrViews[1] = vkViews[1];
            g_xrViewsValidThisFrame = true;
            QueryPerformanceCounter(&g_xrLocateQpc);
            g_xrLocateQpcValid = true;

            g_xrHeadPosition = {
                (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x) * 0.5f,
                (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y) * 0.5f,
                (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z) * 0.5f
            };
            g_xrHeadOrientation = g_xrViews[0].pose.orientation;

            if (g_xrRuntimeLocalRecenterPending || !g_xrTrackingOriginSet)
            {
                // LOCAL is already the runtime's recentered coordinate system.
                // do not redefine zero from the first tracked pose; that made
                // startup depend on where the headset was sitting during boot.
                g_xrTrackingOriginPosition = { 0.0f, 0.0f, 0.0f };
                g_xrTrackingOriginOrientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                g_xrTrackingOriginSet = true;
                g_xrRuntimeLocalRecenterPending = false;
                g_xrPositionRecenterPending = false;
            }
            else if (g_xrPositionRecenterPending)
            {
                // recenter position and facing direction; keep pitch/roll gravity-aligned.
                g_xrTrackingOriginPosition = g_xrHeadPosition;
                const XrQuaternionf& q = g_xrHeadOrientation;
                const float yaw = atan2f(2.0f*(q.w*q.y + q.x*q.z),
                    1.0f - 2.0f*(q.x*q.x + q.y*q.y));
                const float halfYaw = yaw * 0.5f;
                g_xrTrackingOriginOrientation = { 0.0f, sinf(halfYaw), 0.0f, cosf(halfYaw) };
                g_xrPositionRecenterPending = false;
            }
            g_xrTrackingPoseValid = true;
            return;
        }

        if (!g_xrInitialized)
            return;

        PollOpenXREvents();
        if (!g_xrSessionRunning || g_xrExitRequested)
        {
            if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0)
                DxvkPathTrace("V10 frame gate STOP running=%d exit=%d sessionState=%d",
                    g_xrSessionRunning ? 1 : 0, g_xrExitRequested ? 1 : 0, (int)g_xrSessionState);
            return;
        }

        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };

        LARGE_INTEGER v71BeforeWait{}, v71AfterWait{}, v71AfterBegin{};
        const unsigned long long v71PresentBeforeWait = (unsigned long long)g_presentFrameNumber;
        QueryPerformanceCounter(&v71BeforeWait);

        if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0)
            DxvkPathTrace("V10 xrWaitFrame ENTER call=%llu", v10BeginCalls);
        XrResult xr = xrWaitFrame(g_xrSession, &waitInfo, &frameState);
        if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0 || XR_FAILED(xr))
            DxvkPathTrace("V10 xrWaitFrame RETURN result=%d shouldRender=%d predicted=%lld",
                (int)xr, frameState.shouldRender == XR_TRUE ? 1 : 0,
                (long long)frameState.predictedDisplayTime);

        QueryPerformanceCounter(&v71AfterWait);
        const unsigned long long v71PresentAfterWait = (unsigned long long)g_presentFrameNumber;
        if (XR_FAILED(xr))
            return;

        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        xr = xrBeginFrame(g_xrSession, &beginInfo);
        if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0 || XR_FAILED(xr))
            DxvkPathTrace("V10 xrBeginFrame RETURN result=%d", (int)xr);

        QueryPerformanceCounter(&v71AfterBegin);
        const unsigned long long v71PresentAfterBegin = (unsigned long long)g_presentFrameNumber;
        if (XR_FAILED(xr))
            return;

        static unsigned long long v71PhaseSerial = 0;
        ++v71PhaseSerial;
        V71PhaseLog(v71PhaseSerial,
            v71PresentBeforeWait, v71PresentAfterWait, v71PresentAfterBegin,
            v71BeforeWait.QuadPart, v71AfterWait.QuadPart, v71AfterBegin.QuadPart,
            frameState.predictedDisplayTime, frameState.predictedDisplayPeriod);

        g_xrFrameBegun = true;
        g_xrFrameShouldRender = frameState.shouldRender == XR_TRUE;
        g_xrPredictedDisplayTime = frameState.predictedDisplayTime;
        AV_V2UpdateControllerPoses(frameState.predictedDisplayTime);
        SyncVrControllerActions();

        XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = g_xrLocalSpace;

        XrViewState viewState{ XR_TYPE_VIEW_STATE };
        uint32_t viewCount = 0;
        g_xrViews[0] = { XR_TYPE_VIEW };
        g_xrViews[1] = { XR_TYPE_VIEW };

        xr = xrLocateViews(
            g_xrSession, &locateInfo, &viewState, 2, &viewCount, g_xrViews);
        if (v10BeginCalls <= 12 || (v10BeginCalls % 300ULL) == 0 || XR_FAILED(xr))
            DxvkPathTrace("V10 xrLocateViews result=%d count=%u flags=0x%llX",
                (int)xr, viewCount, (unsigned long long)viewState.viewStateFlags);

        if (XR_SUCCEEDED(xr) && viewCount >= 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        {
            g_xrViewsValidThisFrame = true;
            QueryPerformanceCounter(&g_xrLocateQpc);
            g_xrLocateQpcValid = true;

            g_xrHeadPosition = {
                (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x) * 0.5f,
                (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y) * 0.5f,
                (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z) * 0.5f
            };
            g_xrHeadOrientation = g_xrViews[0].pose.orientation;

            if (g_xrRuntimeLocalRecenterPending || !g_xrTrackingOriginSet)
            {
                // Respect SteamVR/OpenXR's LOCAL origin. In particular, don't
                // capture the headset's first pose here: the headset may be
                // sitting beside the user while the game boots.
                g_xrTrackingOriginPosition = { 0.0f, 0.0f, 0.0f };
                g_xrTrackingOriginOrientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                const bool wasRuntimeRecenter = g_xrRuntimeLocalRecenterPending;
                g_xrTrackingOriginSet = true;
                g_xrRuntimeLocalRecenterPending = false;
                g_xrPositionRecenterPending = false;
                DebugLog(wasRuntimeRecenter
                    ? "XR_RECENTER RUNTIME LOCAL ORIGIN APPLIED\n"
                    : "OPENXR LOCAL POSITION ORIGIN USED AT STARTUP\n");
            }
            else if (g_xrPositionRecenterPending)
            {
                // recenter position and facing direction; keep pitch/roll gravity-aligned.
                g_xrTrackingOriginPosition = g_xrHeadPosition;
                const XrQuaternionf& q = g_xrHeadOrientation;
                const float yaw = atan2f(2.0f*(q.w*q.y + q.x*q.z),
                    1.0f - 2.0f*(q.x*q.x + q.y*q.y));
                const float halfYaw = yaw * 0.5f;
                g_xrTrackingOriginOrientation = { 0.0f, sinf(halfYaw), 0.0f, cosf(halfYaw) };
                g_xrPositionRecenterPending = false;
                char recenterLine[256] = {};
                sprintf_s(recenterLine, sizeof(recenterLine),
                    "XR_RECENTER HATVR APPLIED origin=(%.4f %.4f %.4f)\n",
                    g_xrTrackingOriginPosition.x, g_xrTrackingOriginPosition.y,
                    g_xrTrackingOriginPosition.z);
                DebugLog(recenterLine);
            }
            g_xrTrackingPoseValid = true;
        }
    }
    static bool EnsureFinishedUiTargets(IDirect3DDevice9* device, const D3DSURFACE_DESC& d)
    {
        D3DSURFACE_DESC old{};
        if (g_uiWorldBeforeSurface && SUCCEEDED(g_uiWorldBeforeSurface->GetDesc(&old)) &&
            old.Width == d.Width && old.Height == d.Height && old.Format == d.Format &&
            g_uiFinalSurface) return true;

        if (g_uiWorldBeforeSurface) { g_uiWorldBeforeSurface->Release(); g_uiWorldBeforeSurface = nullptr; }
        if (g_uiWorldBeforeTexture) { g_uiWorldBeforeTexture->Release(); g_uiWorldBeforeTexture = nullptr; }
        if (g_uiFinalSurface) { g_uiFinalSurface->Release(); g_uiFinalSurface = nullptr; }
        if (g_uiFinalTexture) { g_uiFinalTexture->Release(); g_uiFinalTexture = nullptr; }

        DxvkPathTrace("V14 UI TARGET allocation requested size=%ux%u format=%u", d.Width, d.Height, (unsigned)d.Format);
        HRESULT hr = device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET,
            d.Format, D3DPOOL_DEFAULT, &g_uiWorldBeforeTexture, nullptr);
        if (FAILED(hr)) return false;
        hr = g_uiWorldBeforeTexture->GetSurfaceLevel(0, &g_uiWorldBeforeSurface);
        if (FAILED(hr)) return false;
        hr = device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET,
            d.Format, D3DPOOL_DEFAULT, &g_uiFinalTexture, nullptr);
        if (FAILED(hr)) return false;
        hr = g_uiFinalTexture->GetSurfaceLevel(0, &g_uiFinalSurface);
        if (FAILED(hr)) return false;
        UiExtractLog("created full-SBS finished-frame UI extraction targets\n");
        return true;
    }

    // Snapshot immediately after the retained stereo scene exists.  The important
    // distinction from the old classifier experiment is that this snapshot is
    // taken from the actual native VR color surface, before Present-time UI
    // extraction, rather than at an arbitrary DIPUP.
    static void CaptureWorldBeforeUi(IDirect3DDevice9* device)
    {
        if (!device || !g_nativeVrColorSurface) return;
        D3DSURFACE_DESC d{};
        if (FAILED(g_nativeVrColorSurface->GetDesc(&d)) ||
            !EnsureFinishedUiTargets(device, d)) return;
        if (SUCCEEDED(device->StretchRect(g_nativeVrColorSurface, nullptr,
            g_uiWorldBeforeSurface, nullptr, D3DTEXF_NONE)))
            g_uiWorldBeforeValid = true;
    }

    static bool CaptureFinishedUi(IDirect3DDevice9* device)
    {
        if (!device || !g_nativeVrColorSurface || !g_uiFinalSurface) return false;
        // for the first pass retain the entire completed frame.  We submit only
        // the LEFT half to both eyes.  This deliberately prioritizes completeness:
        // pause/full-screen/shared UI all survive.  The next pass can reconstruct
        // alpha from world-before vs final without touching HUD draw classification.
        HRESULT hr = device->StretchRect(g_nativeVrColorSurface, nullptr,
            g_uiFinalSurface, nullptr, D3DTEXF_NONE);
        g_uiExtractValid = SUCCEEDED(hr);
        return g_uiExtractValid;
    }

    static bool EnsureUiXrSwapchain(uint32_t w, uint32_t h)
    {
        if (g_xrUiSwapchain != XR_NULL_HANDLE) return true;
        if (g_xrSession == XR_NULL_HANDLE || !w || !h) return false;
        XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.arraySize = 1; ci.mipCount = 1; ci.faceCount = 1; ci.sampleCount = 1;
        ci.width = w; ci.height = h;
        ci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        XrResult xr = xrCreateSwapchain(g_xrSession, &ci, &g_xrUiSwapchain);
        if (XR_FAILED(xr))
        {
            char line[160] = {};
            sprintf_s(line, sizeof(line), "UI_QUAD xrCreateSwapchain failed xr=%d size=%ux%u\n",
                (int)xr, w, h);
            UiExtractLog(line);
            return false;
        }
        uint32_t count = 0;
        xr = xrEnumerateSwapchainImages(g_xrUiSwapchain, 0, &count, nullptr);
        if (XR_FAILED(xr))
        {
            char line[128] = {};
            sprintf_s(line, sizeof(line), "UI_QUAD enumerate-count failed xr=%d\n", (int)xr);
            UiExtractLog(line);
            return false;
        }
        g_xrUiSwapchainImages.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        xr = xrEnumerateSwapchainImages(g_xrUiSwapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_xrUiSwapchainImages.data()));
        if (XR_FAILED(xr))
        {
            char line[128] = {};
            sprintf_s(line, sizeof(line), "UI_QUAD enumerate-images failed xr=%d\n", (int)xr);
            UiExtractLog(line);
            return false;
        }
        char line[160] = {};
        sprintf_s(line, sizeof(line), "UI_QUAD swapchain ready size=%ux%u images=%u\n", w, h, count);
        UiExtractLog(line);
        return true;
    }

    static bool PrepareCompleteMonoUi(XrCompositionLayerQuad& quad)
    {
        static unsigned int traceCount = 0;
        auto traceFailure = [&](const char* stage)
            {
                if (traceCount < 64)
                {
                    char line[192] = {};
                    sprintf_s(line, sizeof(line),
                        "UI_QUAD prepare failed stage=%s extract=%d texture=%p copySize=%ux%u\n",
                        stage, g_uiExtractValid ? 1 : 0, g_uiFinalTexture,
                        g_stereoCopyWidth, g_stereoCopyHeight);
                    UiExtractLog(line);
                    ++traceCount;
                }
                return false;
            };

        if (!g_uiExtractValid || !g_uiFinalTexture || !g_stereoCopyWidth || !g_stereoCopyHeight)
            return traceFailure("inputs");
        if (!EnsureUiXrSwapchain(g_stereoCopyWidth, g_stereoCopyHeight))
            return traceFailure("swapchain");

        // Extract completed LEFT eye to a retained eye-sized texture.  g_leftEyeCopy
        // already has exactly the dimensions OpenXR expects.
        IDirect3DDevice9* dev = nullptr;
        if (FAILED(g_uiFinalTexture->GetDevice(&dev)) || !dev)
            return traceFailure("GetDevice");
        IDirect3DSurface9* dst = nullptr;
        bool ok = false;
        if (g_leftEyeCopy && SUCCEEDED(g_leftEyeCopy->GetSurfaceLevel(0, &dst)) && dst)
        {
            D3DSURFACE_DESC d{}; g_uiFinalSurface->GetDesc(&d);

            // do not crop to the left SBS half here.  Full-screen UE3
            // menus are authored over the complete render target, which is why the
            // previous experiment showed only their left side.  Preserve the full
            // finished canvas and scale it into the mono OpenXR UI texture.
            RECT src{ 0,0,(LONG)d.Width,(LONG)d.Height };
            ok = SUCCEEDED(dev->StretchRect(g_uiFinalSurface, &src, dst, nullptr, D3DTEXF_LINEAR));
        }
        if (dst) dst->Release();
        dev->Release();
        if (!ok) return traceFailure("StretchRect");

        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(g_xrUiSwapchain, &ai, &idx)))
            return traceFailure("acquire");
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(g_xrUiSwapchain, &wi)))
            return traceFailure("wait");
        ok = idx < g_xrUiSwapchainImages.size() &&
            CopyD3D9EyeToXR(g_leftEyeCopy, g_xrUiSwapchainImages[idx].texture);
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(g_xrUiSwapchain, &ri);
        if (!ok) return traceFailure("D3D9-to-XR-copy");

        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = g_xrLocalSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        // Alpha is captured using each UI draw's own RGB blend factors, so the
        // compositor can now use it without making normally-hidden elements
        // visible.
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.subImage.swapchain = g_xrUiSwapchain;
        quad.subImage.imageRect.offset = { 0,0 };
        quad.subImage.imageRect.extent = { (int32_t)g_stereoCopyWidth,(int32_t)g_stereoCopyHeight };
        quad.pose.orientation = { 0,0,0,1 };
        quad.pose.position = { 0,0,-0.75f };
        // AHiT's UI is authored in a 16:9 virtual canvas (192x108).  The source
        // texture is eye-shaped/tall, so using its pixel aspect visibly stretches
        // ordinary HUD.  Keep the physical panel itself 16:9.
        quad.size.width = 1.60f;
        quad.size.height = 0.90f;
        if (traceCount < 64)
        {
            UiExtractLog("UI_QUAD prepared and appended (mirrored game blend alpha)\n");
            ++traceCount;
        }
        return true;
    }

    static unsigned long long g_v80SubmitAttemptSerial = 0;
    static unsigned long long g_v80LastSeenCaptureGeneration = 0;

    // record to AHiTVR.log when a frame is rejected or an XR transfer stage fails.
    static unsigned long long g_xrBadFrameSerial = 0;
    static unsigned long long g_xrBadFrameSuppressed = 0;
    static unsigned long long g_xrLastBadFramePresent = 0;

    static void SubmitOpenXRGameEyes()
    {
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "PIPE_PRESENT camSerial=%llu present=%llu qpc=%lld\n",
                g_v72CameraSerial,
                (unsigned long long)g_presentFrameNumber,
                (long long)now.QuadPart);
            RenderPipelineDiagnosticLog(line);
        }
        if (!g_xrFrameBegun)
            return;

        LARGE_INTEGER xrSubmitA{}, xrSubmitB{};
        QueryPerformanceCounter(&xrSubmitA);

        const unsigned long long v80Attempt = ++g_v80SubmitAttemptSerial;
        const unsigned long long v80CaptureGeneration =
            (unsigned long long)InterlockedCompareExchange64(
                &g_stereoCaptureGeneration, 0, 0);
        const bool v80NewPair =
            v80CaptureGeneration != 0 &&
            v80CaptureGeneration != g_v80LastSeenCaptureGeneration;
        const char* v80PairState =
            v80CaptureGeneration == 0 ? "NO_PAIR" :
            (v80NewPair ? "NEW_PAIR" : "REUSED_PAIR");
        bool v80AcquireOk[2] = {};
        bool v80WaitOk[2] = {};
        bool v80CopyOk[2] = {};
        bool v80ReleaseOk[2] = {};
        uint32_t v80ImageIndices[2] = {};
        XrResult v80AcquireResult[2] = { XR_SUCCESS, XR_SUCCESS };
        XrResult v80WaitResult[2] = { XR_SUCCESS, XR_SUCCESS };
        XrResult v80ReleaseResult[2] = { XR_SUCCESS, XR_SUCCESS };

        const XrCompositionLayerBaseHeader* layers[2] = {};
        XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerQuad uiQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerProjectionView projectionViews[2] = {
            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
        };

        bool submitted = false;

        RefreshOpenXRRuntimeEyeExtent();

        // per-frame "ensure" path transiently returns false. Once valid eye
        // swapchains/resources exist, reuse them; only call Ensure when they
        // actually need creation/recreation.
        const bool v81HaveEyeSwapchains =
            g_xrEyeSwapchains[0] != XR_NULL_HANDLE &&
            g_xrEyeSwapchains[1] != XR_NULL_HANDLE &&
            !g_xrSwapchainImages[0].empty() &&
            !g_xrSwapchainImages[1].empty();

        const bool v81EyeSwapchainsReady =
            v81HaveEyeSwapchains ||
            EnsureOpenXREyeSwapchains(
                (std::max)(1u, g_xrRuntimeEyeWidth),
                (std::max)(1u, g_xrRuntimeEyeHeight));

        // and camera threads overlap, so logging the live globals after the
        // decision can describe a different state than the one the if saw.
        const bool v82ShouldRender = g_xrFrameShouldRender;
        const bool v82ViewsValid = g_xrViewsValidThisFrame;
        const bool v82RenderViewsValid =
            InterlockedCompareExchange(&g_rendererBoundViewsValid, 0, 0) != 0;
        const bool v82HaveLeft = g_haveLeftEye;
        const bool v82HaveRight = g_haveRightEye;
        const bool v82LeftCopy = g_leftEyeCopy != nullptr;
        const bool v82RightCopy = g_rightEyeCopy != nullptr;
        const UINT v82CopyWidth = g_stereoCopyWidth;
        const UINT v82CopyHeight = g_stereoCopyHeight;
        const bool v82SwapReady = v81EyeSwapchainsReady;

        unsigned int v82DropMask = 0;
        if (!v82ShouldRender)     v82DropMask |= 0x001;
        // that flag belongs to the moving XR/camera frame and can clear after the
        // pixels were rendered. The captured renderer-owned pose below is the
        // authoritative metadata for these pixels.
        if (!v82RenderViewsValid) v82DropMask |= 0x004;
        if (!v82HaveLeft)         v82DropMask |= 0x008;
        if (!v82HaveRight)        v82DropMask |= 0x010;
        if (!v82LeftCopy)         v82DropMask |= 0x020;
        if (!v82RightCopy)        v82DropMask |= 0x040;
        if (v82CopyWidth == 0)    v82DropMask |= 0x080;
        if (v82CopyHeight == 0)   v82DropMask |= 0x100;
        if (!v82SwapReady)        v82DropMask |= 0x200;

        const bool v82SubmitPredicatesOk = (v82DropMask == 0);

        if (v82SubmitPredicatesOk)
        {
            static unsigned long long v10TransferAttempts = 0;
            ++v10TransferAttempts;
            if (v10TransferAttempts <= 8 || (v10TransferAttempts % 300ULL) == 0)
                DxvkPathTrace("V10 eye transfer attempt=%llu captureGen=%llu size=%ux%u",
                    v10TransferAttempts, v80CaptureGeneration, v82CopyWidth, v82CopyHeight);
            uint32_t imageIndices[2] = {};
            bool acquired[2] = {};

            for (int eye = 0; eye < 2; ++eye)
            {
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrResult xr = xrAcquireSwapchainImage(
                    g_xrEyeSwapchains[eye], &ai, &imageIndices[eye]);
                v80AcquireResult[eye] = xr;
                v80ImageIndices[eye] = imageIndices[eye];
                v80AcquireOk[eye] = XR_SUCCEEDED(xr);
                if (XR_FAILED(xr))
                    break;
                acquired[eye] = true;

                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                xr = xrWaitSwapchainImage(g_xrEyeSwapchains[eye], &wi);
                v80WaitResult[eye] = xr;
                v80WaitOk[eye] = XR_SUCCEEDED(xr);
                if (XR_FAILED(xr))
                    break;

                IDirect3DTexture9* src =
                    eye == 0 ? g_leftEyeCopy : g_rightEyeCopy;
                ID3D12Resource* dst =
                    g_xrSwapchainImages[eye][imageIndices[eye]].texture;

                v80CopyOk[eye] = CopyD3D9EyeToXR(src, dst);
                if (!v80CopyOk[eye])
                    break;
            }

            if (v10TransferAttempts <= 8 || (v10TransferAttempts % 300ULL) == 0)
                DxvkPathTrace("V10 eye transfer stage acq=%d%d wait=%d%d copy=%d%d idx=%u,%u xrAcq=%d,%d xrWait=%d,%d",
                    v80AcquireOk[0]?1:0, v80AcquireOk[1]?1:0,
                    v80WaitOk[0]?1:0, v80WaitOk[1]?1:0,
                    v80CopyOk[0]?1:0, v80CopyOk[1]?1:0,
                    v80ImageIndices[0], v80ImageIndices[1],
                    (int)v80AcquireResult[0], (int)v80AcquireResult[1],
                    (int)v80WaitResult[0], (int)v80WaitResult[1]);

            // always release every image we acquired, even if wait/copy failed.
            for (int eye = 0; eye < 2; ++eye)
            {
                if (acquired[eye])
                {
                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    v80ReleaseResult[eye] =
                        xrReleaseSwapchainImage(g_xrEyeSwapchains[eye], &ri);
                    v80ReleaseOk[eye] = XR_SUCCEEDED(v80ReleaseResult[eye]);
                }
            }

            // Never submit a half-written stereo pair.  The old path only checked
            // that both swapchain images had been acquired; a failed wait or copy
            // could therefore put an untouched/partially-updated image in the HMD
            // for exactly one frame (black flash or one-eye warp).
            const bool v84CompleteEyeTransfer =
                acquired[0] && acquired[1] &&
                v80WaitOk[0] && v80WaitOk[1] &&
                v80CopyOk[0] && v80CopyOk[1] &&
                v80ReleaseOk[0] && v80ReleaseOk[1];

            if (v84CompleteEyeTransfer)
            {
                // that most recently executed into the pixels reaching this Present.
                const unsigned long long v79PixelPose = (unsigned long long)
                    InterlockedCompareExchange64(&g_capturedStereoPoseSerial,0,0);
                const unsigned long long v83OwnedPose = (unsigned long long)
                    InterlockedCompareExchange64(&g_rendererBoundPoseSerial,0,0);
                const bool v83OwnedPoseValid =
                    InterlockedCompareExchange(&g_rendererBoundViewsValid,0,0) != 0 &&
                    v83OwnedPose == v79PixelPose && v79PixelPose != 0;

                const RenderPoseHistorySlot& v79Candidate = g_renderPoseHistory[v79PixelPose & 63ULL];
                const bool v79HistoryPoseValid =
                    v79PixelPose != 0 && v79Candidate.serial == v79PixelPose;
                const bool v79OwnedPoseValid = v83OwnedPoseValid || v79HistoryPoseValid;
                const XrView* v79SubmitViews =
                    v83OwnedPoseValid ? g_rendererBoundViews :
                    (v79HistoryPoseValid ? v79Candidate.views : g_renderPoseSnapshotViews);
                const unsigned long long v79LayerPoseSerial =
                    v83OwnedPoseValid ? v83OwnedPose :
                    (v79HistoryPoseValid ? v79PixelPose : g_renderPoseSnapshotSerial);

                for (int eye = 0; eye < 2; ++eye)
                {
                    // projection.space is g_xrLocalSpace, therefore this pose
                    // MUST also be expressed in that exact LOCAL reference space.
                    projectionViews[eye].pose = v79SubmitViews[eye].pose;
                    projectionViews[eye].fov = v79SubmitViews[eye].fov;
                    projectionViews[eye].subImage.swapchain = g_xrEyeSwapchains[eye];
                    projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projectionViews[eye].subImage.imageRect.extent = {
                        (int32_t)g_xrSwapchainWidth,
                        (int32_t)g_xrSwapchainHeight
                    };
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                }

                projection.space = g_xrLocalSpace;
                projection.viewCount = 2;

                projection.views = projectionViews;
                layers[0] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                submitted = true;
                g_lastSubmittedPoseSerial = v79LayerPoseSerial;

                // the prior systematic N vs N+1 mismatch into MATCH=1.
                // executed into this Present against the pose metadata submitted
                {
                    const unsigned long long pixelPose = (unsigned long long)
                        InterlockedCompareExchange64(&g_lastExecutedScenePoseSerial,0,0);
                    const unsigned long long pixelProducer = (unsigned long long)
                        InterlockedCompareExchange64(&g_lastExecutedSceneProducerSerial,0,0);
                    const unsigned long long pixelCam = (unsigned long long)
                        InterlockedCompareExchange64(&g_lastExecutedSceneCameraSerial,0,0);
                    const unsigned long long producePresent = (unsigned long long)
                        InterlockedCompareExchange64(&g_lastExecutedScenePresentAtProduce,0,0);
                    static unsigned long long v78SubmitSerial = 0;
                    ++v78SubmitSerial;
                    if (v78SubmitSerial <= 64 || (v78SubmitSerial % 120ULL) == 0 ||
                        pixelPose != g_lastSubmittedPoseSerial)
                    {
                        char line[512] = {};
                        sprintf_s(line,sizeof(line),
                            "V79_SUBMIT submit=%llu present=%llu pixelProducer=%llu pixelCam=%llu pixelPose=%llu layerPose=%llu MATCH=%d ownedValid=%d producePresent=%llu predicted=%lld\n",
                            v78SubmitSerial,(unsigned long long)g_presentFrameNumber,
                            pixelProducer,pixelCam,pixelPose,g_lastSubmittedPoseSerial,
                            pixelPose == g_lastSubmittedPoseSerial ? 1 : 0,
                            v79OwnedPoseValid ? 1 : 0, producePresent,
                            (long long)g_xrPredictedDisplayTime);
                        RenderPipelineDiagnosticLog(line);
                    }
                }

                if (g_lastSubmittedPoseSerial <= 16 ||
                    (g_lastSubmittedPoseSerial % 120ULL) == 0)
                {
                    char line[224] = {};
                    sprintf_s(line, sizeof(line),
                        "XR_SUBMIT serial=%llu present=%llu predicted=%lld ownedPixelPose=%llu MATCH=%d\n",
                        g_lastSubmittedPoseSerial,
                        (unsigned long long)g_presentFrameNumber,
                        (long long)g_xrPredictedDisplayTime, v79PixelPose,
                        g_lastSubmittedPoseSerial == v79PixelPose ? 1 : 0);
                    RenderPipelineDiagnosticLog(line);
                }

                // mono physical panel.  This intentionally proves completeness
                // before alpha subtraction is enabled.
                if (PrepareCompleteMonoUi(uiQuad))
                    layers[1] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&uiQuad);

                static bool logged = false;
                if (!logged)
                {
                    DebugLog("OPENXR FIRST AHiT PIXELS SUBMITTED TO HEADSET\n");
                    logged = true;
                }
            }
        }

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = g_xrPredictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount =
            submitted ? (layers[1] ? 2u : 1u) : 0u;
        endInfo.layers = submitted ? layers : nullptr;
        const XrResult endResult = xrEndFrame(g_xrSession, &endInfo);
        static unsigned long long v10EndCalls = 0;
        ++v10EndCalls;
        if (v10EndCalls <= 12 || (v10EndCalls % 300ULL) == 0 || XR_FAILED(endResult))
            DxvkPathTrace("V10 xrEndFrame call=%llu result=%d submitted=%d layerCount=%u shouldRender=%d dropMask=0x%03X haveLR=%d%d copies=%d%d swapReady=%d",
                v10EndCalls, (int)endResult, submitted ? 1 : 0, endInfo.layerCount,
                v82ShouldRender ? 1 : 0, v82DropMask,
                v82HaveLeft ? 1 : 0, v82HaveRight ? 1 : 0,
                v82LeftCopy ? 1 : 0, v82RightCopy ? 1 : 0,
                v82SwapReady ? 1 : 0);

        // cannot recreate the old 100 MB log problem.
        const bool v84TransferFailure =
            v82SubmitPredicatesOk &&
            (!v80AcquireOk[0] || !v80AcquireOk[1] ||
             !v80WaitOk[0] || !v80WaitOk[1] ||
             !v80CopyOk[0] || !v80CopyOk[1] ||
             !v80ReleaseOk[0] || !v80ReleaseOk[1]);
        const bool v84PoseMismatch = submitted &&
            ((unsigned long long)InterlockedCompareExchange64(
                &g_capturedStereoPoseSerial,0,0) != g_lastSubmittedPoseSerial);
        const bool v84EndFailure = XR_FAILED(endResult);
        const bool v84UnexpectedDrop = !submitted &&
            (v82ShouldRender && v82HaveLeft && v82HaveRight &&
             v82LeftCopy && v82RightCopy);
        if (v84TransferFailure || v84PoseMismatch || v84EndFailure || v84UnexpectedDrop)
        {
            const unsigned long long bad = ++g_xrBadFrameSerial;
            const unsigned long long presentNow = (unsigned long long)g_presentFrameNumber;
            // Log the first 32 anomalies, then at most one every 120 Presents.
            const bool emit = bad <= 32 ||
                presentNow >= g_xrLastBadFramePresent + 120ULL;
            if (emit)
            {
                g_xrLastBadFramePresent = presentNow;
                Log(
                    "XR_BAD_FRAME #%llu present=%llu attempt=%llu captureGen=%llu newPair=%d "
                    "dropMask=0x%03X submitted=%d transferFail=%d poseMismatch=%d endFail=%d "
                    "acq=%d%d xr=%d,%d idx=%u,%u wait=%d%d xr=%d,%d copy=%d%d "
                    "release=%d%d xr=%d,%d pixelPose=%llu layerPose=%llu endXr=%d suppressed=%llu",
                    bad,presentNow,v80Attempt,v80CaptureGeneration,v80NewPair?1:0,
                    v82DropMask,submitted?1:0,v84TransferFailure?1:0,
                    v84PoseMismatch?1:0,v84EndFailure?1:0,
                    v80AcquireOk[0]?1:0,v80AcquireOk[1]?1:0,
                    (int)v80AcquireResult[0],(int)v80AcquireResult[1],
                    v80ImageIndices[0],v80ImageIndices[1],
                    v80WaitOk[0]?1:0,v80WaitOk[1]?1:0,
                    (int)v80WaitResult[0],(int)v80WaitResult[1],
                    v80CopyOk[0]?1:0,v80CopyOk[1]?1:0,
                    v80ReleaseOk[0]?1:0,v80ReleaseOk[1]?1:0,
                    (int)v80ReleaseResult[0],(int)v80ReleaseResult[1],
                    (unsigned long long)InterlockedCompareExchange64(
                        &g_capturedStereoPoseSerial,0,0),
                    (unsigned long long)g_lastSubmittedPoseSerial,
                    (int)endResult,g_xrBadFrameSuppressed);
                g_xrBadFrameSuppressed = 0;
            }
            else
            {
                ++g_xrBadFrameSuppressed;
            }
        }

        // visible black/unposed flashes with pair freshness and every swapchain
        // stage without changing pacing or ownership.
        {
            const char* v80Outcome =
                submitted ? (v80NewPair ? "SUBMITTED_NEW_PAIR" : "SUBMITTED_REUSED_PAIR") :
                (v80CaptureGeneration == 0 ? "NO_PAIR" : "NOT_SUBMITTED");
            char line[768] = {};
            sprintf_s(line, sizeof(line),
                "V80_FRAME attempt=%llu present=%llu captureGen=%llu pair=%s outcome=%s "
                "haveLR=%d%d shouldRender=%d viewsValid=%d renderViewsValid=%d "
                "copies=%d%d size=%ux%u swapExisting=%d swapReady=%d "
                "dropMask=0x%03X predicatesOk=%d acq=%d%d acqXr=%d,%d idx=%u,%u wait=%d%d waitXr=%d,%d "
                "copy=%d%d release=%d%d releaseXr=%d,%d submitted=%d "
                "layerCount=%u endXr=%d pixelPose=%llu layerPose=%llu\n",
                v80Attempt, (unsigned long long)g_presentFrameNumber,
                v80CaptureGeneration, v80PairState, v80Outcome,
                v82HaveLeft ? 1 : 0, v82HaveRight ? 1 : 0,
                v82ShouldRender ? 1 : 0,
                v82ViewsValid ? 1 : 0,
                v82RenderViewsValid ? 1 : 0,
                v82LeftCopy ? 1 : 0, v82RightCopy ? 1 : 0,
                v82CopyWidth, v82CopyHeight,
                v81HaveEyeSwapchains ? 1 : 0,
                v82SwapReady ? 1 : 0,
                v82DropMask, v82SubmitPredicatesOk ? 1 : 0,
                v80AcquireOk[0] ? 1 : 0, v80AcquireOk[1] ? 1 : 0,
                (int)v80AcquireResult[0], (int)v80AcquireResult[1],
                v80ImageIndices[0], v80ImageIndices[1],
                v80WaitOk[0] ? 1 : 0, v80WaitOk[1] ? 1 : 0,
                (int)v80WaitResult[0], (int)v80WaitResult[1],
                v80CopyOk[0] ? 1 : 0, v80CopyOk[1] ? 1 : 0,
                v80ReleaseOk[0] ? 1 : 0, v80ReleaseOk[1] ? 1 : 0,
                (int)v80ReleaseResult[0], (int)v80ReleaseResult[1],
                submitted ? 1 : 0, endInfo.layerCount, (int)endResult,
                (unsigned long long)InterlockedCompareExchange64(
                    &g_capturedStereoPoseSerial,0,0),
                (unsigned long long)g_lastSubmittedPoseSerial);
            RenderPipelineDiagnosticLog(line);
        }
        if (!submitted && v82DropMask != 0)
        {
            char line[384] = {};
            sprintf_s(line, sizeof(line),
                "V82_DROP attempt=%llu present=%llu captureGen=%llu mask=0x%03X "
                "shouldRender=%d viewsValid=%d renderViewsValid=%d haveLR=%d%d "
                "copies=%d%d size=%ux%u swapExisting=%d swapReady=%d "
                "pixelPose=%llu lastLayerPose=%llu\n",
                v80Attempt, (unsigned long long)g_presentFrameNumber,
                v80CaptureGeneration, v82DropMask,
                v82ShouldRender ? 1 : 0, v82ViewsValid ? 1 : 0,
                v82RenderViewsValid ? 1 : 0,
                v82HaveLeft ? 1 : 0, v82HaveRight ? 1 : 0,
                v82LeftCopy ? 1 : 0, v82RightCopy ? 1 : 0,
                v82CopyWidth, v82CopyHeight,
                v81HaveEyeSwapchains ? 1 : 0, v82SwapReady ? 1 : 0,
                (unsigned long long)InterlockedCompareExchange64(
                    &g_capturedStereoPoseSerial, 0, 0),
                (unsigned long long)g_lastSubmittedPoseSerial);
            RenderPipelineDiagnosticLog(line);
        }

        if (submitted)
        {
            const unsigned long long v83SubmittedPixelPose = (unsigned long long)
                InterlockedCompareExchange64(&g_capturedStereoPoseSerial,0,0);
            const unsigned long long v83PublishedPose = (unsigned long long)
                InterlockedCompareExchange64(&g_rendererBoundPoseSerial,0,0);
            if (v83PublishedPose == v83SubmittedPixelPose)
                InterlockedExchange(&g_rendererBoundViewsValid, 0);
        }

        if (submitted && v80CaptureGeneration != 0)
            g_v80LastSeenCaptureGeneration = v80CaptureGeneration;
        static unsigned int uiEndTraceCount = 0;
        if (uiEndTraceCount < 64 && layers[1])
        {
            char line[160] = {};
            sprintf_s(line, sizeof(line),
                "UI_QUAD xrEndFrame xr=%d layerCount=%u\n",
                (int)endResult, endInfo.layerCount);
            UiExtractLog(line);
            ++uiEndTraceCount;
        }

        QueryPerformanceCounter(&xrSubmitB);
        g_vrPerf.xrSubmitMs += PerfMs(xrSubmitA, xrSubmitB);
        FlushPerfSummaryIfNeeded();

        g_xrFrameBegun = false;
        g_xrFrameShouldRender = false;
        g_xrViewsValidThisFrame = false;
    }
static float* HookedPerspectiveMatrixCandidate(
        float* outMatrix,
        float halfFovX,
        float halfFovY,
        float scaleX,
        float scaleY,
        float nearZ,
        float farZ)
    {
        float* result = g_originalPerspectiveMatrixCandidate(
            outMatrix,
            halfFovX,
            halfFovY,
            scaleX,
            scaleY,
            nearZ,
            farZ
        );

        if (!result)
            return result;

        // Camera position/rotation are already identical in Theater Mode. Lock
        // the RIGHT render pass to the exact projection matrix produced for the
        // LEFT pass as well. This removes any remaining eye-dependent projection
        // state upstream of this hook without changing normal immersive VR.
        //
        // if left/right Theater output now fuses, the residual stereo disparity
        // was projection/frustum state rather than camera translation.
        if (g_theaterMode && g_alternatingStereoEnabled)
        {
            static thread_local float s_theaterLeftProjection[16] = {};
            static thread_local bool s_theaterLeftProjectionValid = false;

            if (!g_renderRightEye)
            {
                memcpy(s_theaterLeftProjection, result,
                       sizeof(s_theaterLeftProjection));
                s_theaterLeftProjectionValid = true;
            }
            else if (s_theaterLeftProjectionValid)
            {
                memcpy(result, s_theaterLeftProjection,
                       sizeof(s_theaterLeftProjection));
            }

            return result;
        }

        if (!g_alternatingStereoEnabled ||
            !g_xrViewsValidThisFrame)
        {
            return result;
        }

        // lifecycle as one coherent system.  Do NOT let HatVR's older
        // per-eye PerspectiveMatrix hook rewrite the principal/source
        // projection while native same-frame stereo is active.
        //
        // keep UE3's validated symmetric projection here and perform
        // its headset crop later in the presentation path.  Returning the
        // game's original matrix also guarantees the single principal view
        // cannot silently become "left eye" merely because g_renderRightEye
        // is false during CalcSceneView.
        if (g_nativeStereoEnabled && g_sharperNativeStereo)
        {
            // sharp stereo folds the horizontal crop into the projection. keep the
            // runtime's vertical projection unchanged.
            const int eye = g_calcSceneRightEye ? 1 : 0;
            const XrView* pv = g_renderPoseSnapshotValid
                ? g_renderPoseSnapshotViews : g_xrViews;
            const XrFovf& fov = pv[eye].fov;

            const float tanLeft  = tanf(fov.angleLeft);
            const float tanRight = tanf(fov.angleRight);
            const float width = tanRight - tanLeft;

            const float tanDown = tanf(fov.angleDown);
            const float tanUp = tanf(fov.angleUp);
            const float height = tanUp - tanDown;

            if (fabsf(width) > 0.0001f && fabsf(height) > 0.0001f)
            {
                result[0] = 2.0f / width;
                result[5] = 2.0f / height;
                result[8] = (tanLeft + tanRight) / (tanLeft - tanRight);
                result[9] = (tanUp + tanDown) / (tanDown - tanUp);

                static unsigned int r261ProjectionLogs = 0;
                if (r261ProjectionLogs++ < 24)
                    DxvkPathTrace(
                        "R261 NATIVE HORIZONTAL-ASYM eye=%s tan=[L%.5f R%.5f] M00=%.5f M11=%.5f M20=%.5f M21=%.5f",
                        eye ? "RIGHT" : "LEFT", tanLeft, tanRight,
                        result[0], result[5], result[8], result[9]);
            }
            return result;
        }

        // normal stereo keeps the shared horizontal envelope and crops it later.
        // vertical projection stays per-eye; there is no vertical presentation crop.
        if (g_nativeStereoEnabled)
        {
            const XrView* pv = g_renderPoseSnapshotValid
                ? g_renderPoseSnapshotViews : g_xrViews;

            const float l0 = tanf(pv[0].fov.angleLeft);
            const float r0 = tanf(pv[0].fov.angleRight);
            const float l1 = tanf(pv[1].fov.angleLeft);
            const float r1 = tanf(pv[1].fov.angleRight);
            const XrFovf& fov = pv[g_calcSceneRightEye ? 1 : 0].fov;
            const float tanDown = tanf(fov.angleDown);
            const float tanUp = tanf(fov.angleUp);
            const float height = tanUp - tanDown;

            const float halfX = (std::max)(
                (std::max)(fabsf(l0), fabsf(r0)),
                (std::max)(fabsf(l1), fabsf(r1)));

            if (halfX > 0.0001f && fabsf(height) > 0.0001f)
            {
                result[0] = 1.0f / halfX;
                result[5] = 2.0f / height;
                result[8] = 0.0f;
                result[9] = (tanUp + tanDown) / (tanDown - tanUp);

                static LONG v8ProjectionOnce = 0;
                if (InterlockedCompareExchange(&v8ProjectionOnce, 1, 0) == 0)
                {
                    DxvkPathTrace(
                        "BL1_NATIVE_STEREO_V11 XR PROJECTION halfX=%.6f height=%.6f M00=%.6f M11=%.6f M21=%.6f",
                        halfX, height, result[0], result[5], result[9]);
                }
            }
            return result;
        }

        const int eye = g_calcSceneRightEye ? 1 : 0;

        // CalcSceneView is currently using.  g_xrViews is live OpenXR state and
        // can advance independently while UE3 is constructing/rendering the two
        // views; using it here could give one eye (usually the second/right eye)
        // FOV metadata from a newer XR generation.
        const bool v86FrozenViewsValid = g_renderPoseSnapshotValid;
        const XrFovf& fov = v86FrozenViewsValid
            ? g_renderPoseSnapshotViews[eye].fov
            : g_xrViews[eye].fov;

        // unless the live FOV has diverged from the frozen render snapshot.
        if (v86FrozenViewsValid)
        {
            const XrFovf& live = g_xrViews[eye].fov;
            const float maxDiff = (std::max)(
                (std::max)(fabsf(live.angleLeft - fov.angleLeft), fabsf(live.angleRight - fov.angleRight)),
                (std::max)(fabsf(live.angleUp - fov.angleUp), fabsf(live.angleDown - fov.angleDown)));
            if (maxDiff > 0.00001f)
            {
                static unsigned long long v86MismatchCount = 0;
                static unsigned long long v86LastMismatchPresent = 0;
                ++v86MismatchCount;
                const unsigned long long presentNow = (unsigned long long)g_presentFrameNumber;
                if (v86MismatchCount <= 24 || presentNow >= v86LastMismatchPresent + 120ULL)
                {
                    v86LastMismatchPresent = presentNow;
                    Log(
                        "XR_RENDER_FOV_DRIFT #%llu present=%llu cam=%llu renderPose=%llu eye=%s maxDiff=%.8f "
                        "frozen=[%.6f %.6f %.6f %.6f] live=[%.6f %.6f %.6f %.6f]",
                        v86MismatchCount,presentNow,g_v72CameraSerial,g_renderPoseSnapshotSerial,
                        eye ? "RIGHT" : "LEFT",maxDiff,
                        fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown,
                        live.angleLeft,live.angleRight,live.angleUp,live.angleDown);
                }
            }
        }

        const float tanLeft = tanf(fov.angleLeft);
        const float tanRight = tanf(fov.angleRight);
        const float tanDown = tanf(fov.angleDown);
        const float tanUp = tanf(fov.angleUp);
        const float height = tanUp - tanDown;

        // UE3 gets a symmetric horizontal frustum wide enough to contain the XR
        // eye, but keeps the ORIGINAL HatVR/OpenXR vertical asymmetric projection.
        // the Vulkan bridge corrects horizontal asymmetry only.
        const float sourceHalfX = (std::max)(fabsf(tanLeft), fabsf(tanRight));
        if (sourceHalfX < 0.0001f || fabsf(height) < 0.0001f)
            return result;

        result[0] = 1.0f / sourceHalfX;
        result[5] = 2.0f / height;
        result[8] = 0.0f;
        result[9] = (tanUp + tanDown) / (tanDown - tanUp);

        static unsigned int firstProjectionLogs = 0;
        if (firstProjectionLogs < 24)
        {
            ++firstProjectionLogs;
            constexpr float kRadToDeg = 57.29577951308232f;
            char line[640] = {};
            sprintf_s(
                line, sizeof(line),
                "PHASE7.1 HORIZONTAL-ONLY PROJECTION eye=%s XR=[L%.2f R%.2f U%.2f D%.2f] "
                "sourceHalfX=%.5f M00=%.5f M11=%.5f M20=%.5f M21=%.5f frame=%llu\n",
                eye ? "RIGHT" : "LEFT",
                fov.angleLeft * kRadToDeg, fov.angleRight * kRadToDeg,
                fov.angleUp * kRadToDeg, fov.angleDown * kRadToDeg,
                sourceHalfX, result[0], result[5], result[8], result[9],
                g_presentFrameNumber);
            DebugLog(line);
        }

        return result;

    }

    static bool InstallPerspectiveMatrixCandidateHook()
    {
        if (g_perspectiveMatrixHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            DebugLog("Perspective matrix: GetModuleHandleW(NULL) FAILED\n");
            return false;
        }

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kPerspectiveMatrixCandidateRva
            );

        char line[256] = {};
        sprintf_s(
            line,
            sizeof(line),
            "Installing perspective matrix hook: target=%p RVA=0x%llX\n",
            target,
            static_cast<unsigned long long>(kPerspectiveMatrixCandidateRva)
        );
        DebugLog(line);

        MH_STATUS status = MH_CreateHook(
            target,
            &HookedPerspectiveMatrixCandidate,
            reinterpret_cast<void**>(&g_originalPerspectiveMatrixCandidate)
        );

        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            sprintf_s(
                line,
                sizeof(line),
                "MH_CreateHook(perspective matrix) FAILED status=%d\n",
                static_cast<int>(status)
            );
            DebugLog(line);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            sprintf_s(
                line,
                sizeof(line),
                "MH_EnableHook(perspective matrix) FAILED status=%d\n",
                static_cast<int>(status)
            );
            DebugLog(line);
            return false;
        }

        g_perspectiveMatrixHookInstalled = true;
        DebugLog("Perspective matrix hook installed\n");
        return true;
    }

    static void ApplyCameraLocalOffset(
        FVectorUE3& location,
        const FRotatorUE3& rotation)
    {
        // UE3 rotator units -> radians.
        constexpr double kRotToRad =
            6.28318530717958647692 / 65536.0;

        const double pitch = static_cast<double>(rotation.Pitch) * kRotToRad;
        const double yaw = static_cast<double>(rotation.Yaw) * kRotToRad;

        const float cp = static_cast<float>(cos(pitch));
        const float sp = static_cast<float>(sin(pitch));
        const float cy = static_cast<float>(cos(yaw));
        const float sy = static_cast<float>(sin(yaw));

        // standard UE3-style forward/right/up basis sufficient for this debug test.
        const FVectorUE3 forward{ cp * cy, cp * sy, sp };
        const FVectorUE3 right{ -sy, cy, 0.0f };
        const FVectorUE3 up{
            -sp * cy,
            -sp * sy,
            cp
        };

        location.X +=
            forward.X * g_cameraLocalForward +
            right.X * g_cameraLocalRight +
            up.X * g_cameraLocalUp;

        location.Y +=
            forward.Y * g_cameraLocalForward +
            right.Y * g_cameraLocalRight +
            up.Y * g_cameraLocalUp;

        location.Z +=
            forward.Z * g_cameraLocalForward +
            right.Z * g_cameraLocalRight +
            up.Z * g_cameraLocalUp;
    }

    static bool EnsureNativeVrRenderSurfaces(IDirect3DDevice9* device)
    {
        if (!device || !g_xrRuntimeEyeWidth || !g_xrRuntimeEyeHeight)
            return false;

        const UINT wantedW = g_xrRuntimeEyeWidth * 2u;
        const UINT wantedH = g_xrRuntimeEyeHeight;
        if (g_nativeVrColorSurface && g_nativeVrDepthSurface &&
            g_nativeVrWidth == wantedW && g_nativeVrHeight == wantedH)
            return true;

        if (g_nativeVrColorSurface) { g_nativeVrColorSurface->Release(); g_nativeVrColorSurface = nullptr; }
        if (g_nativeVrColorTexture) { g_nativeVrColorTexture->Release(); g_nativeVrColorTexture = nullptr; }
        if (g_nativeVrDepthSurface) { g_nativeVrDepthSurface->Release(); g_nativeVrDepthSurface = nullptr; }
        g_nativeVrWidth = g_nativeVrHeight = 0;

        DxvkPathTrace("V14 NATIVE TARGET allocation requested size=%ux%u", wantedW, wantedH);
        VulkanTraceBeginD3D9Resource("NATIVE-VR-COLOR", wantedW, wantedH, (uint32_t)D3DFMT_A8R8G8B8);
        // render-target texture rather than a standalone surface. UE3 still renders
        // to level 0 exactly as before, but DXVK interop can now expose the ORIGINAL
        // 2688x1440 backing VkImage directly to the OpenXR bridge. This deletes the
        // two intermediate D3D9 per-eye StretchRect copies on the Vulkan path.
        HRESULT hr = device->CreateTexture(
            wantedW, wantedH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &g_nativeVrColorTexture, nullptr);
        if (SUCCEEDED(hr) && g_nativeVrColorTexture)
            hr = g_nativeVrColorTexture->GetSurfaceLevel(0, &g_nativeVrColorSurface);
        VulkanTraceEndD3D9Resource("NATIVE-VR-COLOR-DIRECT-SBS", (long)hr, g_nativeVrColorTexture);
        if (FAILED(hr) || !g_nativeVrColorTexture || !g_nativeVrColorSurface)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "PHASE8 DIRECT-SBS: CreateTexture/GetSurfaceLevel %ux%u failed hr=0x%08X\n", wantedW, wantedH, (unsigned)hr);
            DebugLog(line);
            if (g_nativeVrColorSurface) { g_nativeVrColorSurface->Release(); g_nativeVrColorSurface = nullptr; }
            if (g_nativeVrColorTexture) { g_nativeVrColorTexture->Release(); g_nativeVrColorTexture = nullptr; }
            return false;
        }
        DxvkPathTrace("VKBRIDGE-V5 PHASE8 DIRECT-SBS NATIVE-TEXTURE READY texture=%p surface=%p size=%ux%u",
            g_nativeVrColorTexture, g_nativeVrColorSurface, wantedW, wantedH);

        VulkanTraceBeginD3D9Resource("NATIVE-VR-DEPTH-D24S8", wantedW, wantedH, (uint32_t)D3DFMT_D24S8);
        hr = device->CreateDepthStencilSurface(
            wantedW, wantedH, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
            &g_nativeVrDepthSurface, nullptr);
        VulkanTraceEndD3D9Resource("NATIVE-VR-DEPTH-D24S8", (long)hr, g_nativeVrDepthSurface);
        if (FAILED(hr) || !g_nativeVrDepthSurface)
        {
            // Some hardware/runtime combinations dislike D24S8 here. Try D24X8.
            VulkanTraceBeginD3D9Resource("NATIVE-VR-DEPTH-D24X8", wantedW, wantedH, (uint32_t)D3DFMT_D24X8);
            hr = device->CreateDepthStencilSurface(
                wantedW, wantedH, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
                &g_nativeVrDepthSurface, nullptr);
            VulkanTraceEndD3D9Resource("NATIVE-VR-DEPTH-D24X8", (long)hr, g_nativeVrDepthSurface);
        }
        if (FAILED(hr) || !g_nativeVrDepthSurface)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line), "NATIVE VR TARGET: CreateDepthStencilSurface %ux%u failed hr=0x%08X\n", wantedW, wantedH, (unsigned)hr);
            DebugLog(line);
            g_nativeVrColorSurface->Release(); g_nativeVrColorSurface = nullptr;
            if (g_nativeVrColorTexture) { g_nativeVrColorTexture->Release(); g_nativeVrColorTexture = nullptr; }
            return false;
        }

        g_nativeVrWidth = wantedW;
        g_nativeVrHeight = wantedH;
        char line[256] = {};
        sprintf_s(line, sizeof(line), "NATIVE VR TARGET CREATED: %ux%u SBS (%ux%u per eye)\n",
            wantedW, wantedH, g_xrRuntimeEyeWidth, g_xrRuntimeEyeHeight);
        DebugLog(line);
        return true;
    }

    static unsigned int HookedViewportGetSizeX(void* viewport)
    {
        if (g_nativeVrResolutionEnabled && g_xrRuntimeEyeWidth &&
            g_gameplayViewportResourceHandle && viewport &&
            *reinterpret_cast<unsigned long long*>(static_cast<unsigned char*>(viewport) + 8) == g_gameplayViewportResourceHandle)
            return g_xrRuntimeEyeWidth * 2u;
        return g_originalViewportGetSizeX ? g_originalViewportGetSizeX(viewport) : 0;
    }

    static unsigned int HookedViewportGetSizeY(void* viewport)
    {
        if (g_nativeVrResolutionEnabled && g_xrRuntimeEyeHeight &&
            g_gameplayViewportResourceHandle && viewport &&
            *reinterpret_cast<unsigned long long*>(static_cast<unsigned char*>(viewport) + 8) == g_gameplayViewportResourceHandle)
            return g_xrRuntimeEyeHeight;
        return g_originalViewportGetSizeY ? g_originalViewportGetSizeY(viewport) : 0;
    }

    static void HookedRHISetRenderTarget(void* rhi, unsigned long long colorHandle, unsigned long long depthHandle)
    {
        DiscoverRHI4B0(rhi);
        DiscoverRHIBackend2A0(rhi);

        // Concrete D3D9 RHI stores IDirect3DDevice9* at +0x34.
        IDirect3DDevice9* device = rhi
            ? *reinterpret_cast<IDirect3DDevice9**>(static_cast<unsigned char*>(rhi) + 0x34)
            : nullptr;

        // Let UE3 perform its normal render-target change first.
        if (g_originalRHISetRenderTarget)
            g_originalRHISetRenderTarget(rhi, colorHandle, depthHandle);

        // DXVK 3.1.1 fix:
        // this eligibility check must NOT be gated by a temporary trace/log flag.
        // Whenever UE3 selects the gameplay viewport resource, replace RT0 with
        // HatVR's native SBS target.
        const bool gameplayRequest =
            g_nativeVrResolutionEnabled &&
            rhi && colorHandle &&
            colorHandle == g_gameplayViewportResourceHandle;

        if (gameplayRequest && device && EnsureNativeVrRenderSurfaces(device))
        {
            const HRESULT setRtHr =
                device->SetRenderTarget(0, g_nativeVrColorSurface);
            const HRESULT setDepthHr =
                device->SetDepthStencilSurface(g_nativeVrDepthSurface);

            D3DVIEWPORT9 vp{};
            vp.X = 0;
            vp.Y = 0;
            vp.Width = g_nativeVrWidth;
            vp.Height = g_nativeVrHeight;
            vp.MinZ = 0.0f;
            vp.MaxZ = 1.0f;
            const HRESULT setVpHr = device->SetViewport(&vp);

            static unsigned int s_nativeBindLogs = 0;
            if (s_nativeBindLogs < 8)
            {
                ++s_nativeBindLogs;
                DxvkPathTrace(
                    "NATIVE VR TARGET BOUND: handle=0x%llX %ux%u SetRT=0x%08X SetDepth=0x%08X SetVP=0x%08X",
                    colorHandle, g_nativeVrWidth, g_nativeVrHeight,
                    (unsigned)setRtHr, (unsigned)setDepthHr, (unsigned)setVpHr);
            }
        }
    }

    static bool InstallNativeVrResolutionHooks()
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);

        struct HookSpec { uintptr_t rva; void* hook; void** original; const char* name; } specs[] = {
            { 0x3F0E10, reinterpret_cast<void*>(&HookedViewportGetSizeX), reinterpret_cast<void**>(&g_originalViewportGetSizeX), "FViewport::GetSizeX" },
            { 0x896620, reinterpret_cast<void*>(&HookedViewportGetSizeY), reinterpret_cast<void**>(&g_originalViewportGetSizeY), "FViewport::GetSizeY" },
            { 0xA25390, reinterpret_cast<void*>(&HookedRHISetRenderTarget), reinterpret_cast<void**>(&g_originalRHISetRenderTarget), "D3D9RHI::SetRenderTarget" },
        };

        for (auto& spec : specs)
        {
            void* target = reinterpret_cast<void*>(base + spec.rva);
            MH_STATUS st = MH_CreateHook(target, spec.hook, spec.original);
            if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
            {
                char line[256] = {};
                sprintf_s(line, sizeof(line), "NATIVE VR RES: MH_CreateHook(%s) failed status=%d\n", spec.name, (int)st);
                DebugLog(line);
                return false;
            }
            st = MH_EnableHook(target);
            if (st != MH_OK && st != MH_ERROR_ENABLED)
            {
                char line[256] = {};
                sprintf_s(line, sizeof(line), "NATIVE VR RES: MH_EnableHook(%s) failed status=%d\n", spec.name, (int)st);
                DebugLog(line);
                return false;
            }
        }
        DebugLog("NATIVE VR RESOLUTION HOOKS INSTALLED (GetSizeX/GetSizeY/RHI SetRenderTarget)\n");
        return true;
    }

    static unsigned long long HookedCalcSceneViewCandidate(
        void* localPlayer,
        void* viewFamily,
        FVectorUE3* viewLocation,
        FRotatorUE3* viewRotation,
        void* viewport,
        void* viewDrawer)
    {
        static unsigned long long callCount = 0;
        ++callCount;
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t calcReturnRva = exe
            ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(exe)
            : 0;

        // native stereo injection.
        //
        // AHiT/UE3 stores LocalPlayer viewport fractions at:
        //   +0xA8 OriginX, +0xAC OriginY, +0xB0 SizeX, +0xB4 SizeY.
        // CalcSceneView turns those fractions into the FSceneView X/Y/SizeX/SizeY
        // and appends the resulting FSceneView to ViewFamily->Views.
        //
        // for the normal gameplay caller, build TWO views in the SAME family:
        // LEFT  = left half of the viewport
        // RIGHT = right half of the viewport
        //
        // this makes UE3 render both eyes during one renderer invocation instead
        // of alternating whole game frames.
        unsigned long long result = 0;

        if (calcReturnRva == kGameplayCalcSceneViewReturnRva)
        {
            // Remember its exact family until 0x81C9C0 consumes that family.
            g_v136GameplayViewFamily = viewFamily;
            g_v137ProducerGameplayViewFamily = viewFamily;
            g_v136GameplayRenderer = nullptr;
            ++g_v136GameplayFamilySerial;

            static unsigned int familyLogs = 0;
            if (familyLogs++ < 24)
                DxvkPathTrace(
                    "V139 GAMEPLAY_FAMILY mark family=%p familySerial=%llu present=%llu",
                    viewFamily,
                    (unsigned long long)g_v136GameplayFamilySerial,
                    (unsigned long long)g_presentFrameNumber);

            if (g_v26SequentialReentry && g_v26InsideDraw)
                g_v26SawGameplayCalc = true;
            static unsigned v16GameplayCalcLogs = 0;
            const bool v16TraceCalc = (v16GameplayCalcLogs++ < 12);
            if (v16TraceCalc)
            {
                DxvkPathTrace("PHASE8.3 CALC gameplay ENTER call=%llu present=%llu viewport=%p BUILD=2026-09-20G",
                    callCount, (unsigned long long)g_presentFrameNumber, viewport);
            }

            // we already know CALC gameplay ENTER fires but the later CALC-PATH
            // marker does not.  These markers isolate the exact call that diverts
            // control without changing any render/camera state.
            auto Phase81CalcStep = [&](const char* step)
            {
                static unsigned int phase81StepLogs = 0;
                if (phase81StepLogs < 192 || (callCount % 600ULL) == 0)
                {
                    ++phase81StepLogs;
                    char line[384] = {};
                    sprintf_s(line, sizeof(line),
                        "PHASE8.3 CALC-STEP call=%llu present=%llu step=%s xrValid=%d frameBegun=%d snapValid=%d snapSerial=%llu\n",
                        callCount, (unsigned long long)g_presentFrameNumber, step,
                        g_xrViewsValidThisFrame ? 1 : 0, g_xrFrameBegun ? 1 : 0,
                        g_renderPoseSnapshotValid ? 1 : 0,
                        (unsigned long long)g_renderPoseSnapshotSerial);
                    DxvkPathTrace("%s", line);
                }
            };

            if (viewport)
            {
                Phase81CalcStep("03-before-viewport-resource-read");
                g_gameplayViewportResourceHandle = *reinterpret_cast<unsigned long long*>(static_cast<unsigned char*>(viewport) + 8);
                Phase81CalcStep("04-after-viewport-resource-read");
            }
            Phase81CalcStep("05-before-camera-serial");
            ++g_v72CameraSerial;
            Phase81CalcStep("06-before-V72CameraMark-ENTER");
            V72CameraMark("ENTER");
            Phase81CalcStep("07-after-V72CameraMark-ENTER");
            if (v16TraceCalc)
            Phase81CalcStep("08-before-BeginOpenXRFrameForRender");
            if (!(g_v26SequentialReentry && g_v26SecondPass))
                BeginOpenXRFrameForRender();
            Phase81CalcStep("09-after-BeginOpenXRFrameForRender");
            if (v16TraceCalc)
            {
            }
            Phase81CalcStep("10-after-memory-trace");
            Phase81CalcStep("11-before-V72CameraMark-AFTER_XR");
            V72CameraMark("AFTER_XR");
            Phase81CalcStep("12-after-V72CameraMark-AFTER_XR");

            // gameplay render is about to bake into its two FSceneViews.
            Phase81CalcStep("13-before-V70SnapshotRenderViews");
            if (!(g_v26SequentialReentry && g_v26SecondPass))
                V70SnapshotRenderViews();
            Phase81CalcStep("14-after-V70SnapshotRenderViews");
        }

        const bool doNativeStereo =
            !g_v26SequentialReentry &&
            g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            g_xrViewsValidThisFrame &&
            localPlayer &&
            calcReturnRva == kGameplayCalcSceneViewReturnRva;

        if (calcReturnRva == kGameplayCalcSceneViewReturnRva)
        {
            static unsigned int phase72bGateLogs = 0;
            if (phase72bGateLogs < 32 || (callCount % 600ULL) == 0)
            {
                ++phase72bGateLogs;
                char p72b[768] = {};
                sprintf_s(p72b, sizeof(p72b),
                    "PHASE8.3 CALC-PATH call=%llu present=%llu localPlayer=%p viewFamily=%p "
                    "native=%d alternating=%d xrValid=%d doNativeStereo=%d renderSnap=%d snapSerial=%llu "
                    "viewLoc=%p viewRot=%p\n",
                    callCount, (unsigned long long)g_presentFrameNumber, localPlayer, viewFamily,
                    g_nativeStereoEnabled ? 1 : 0, g_alternatingStereoEnabled ? 1 : 0,
                    g_xrViewsValidThisFrame ? 1 : 0, doNativeStereo ? 1 : 0,
                    g_renderPoseSnapshotValid ? 1 : 0,
                    (unsigned long long)g_renderPoseSnapshotSerial,
                    viewLocation, viewRotation);
                DxvkPathTrace("%s", p72b);
            }
        }

        const bool doSequentialEye =
            g_v26SequentialReentry && g_v26InsideDraw &&
            !g_nativeStereoEnabled && g_alternatingStereoEnabled &&
            g_xrViewsValidThisFrame && localPlayer &&
            calcReturnRva == kGameplayCalcSceneViewReturnRva;

        if (doSequentialEye)
        {
            // Each re-entry is a completely ordinary ONE-view UE3 renderer.
            // we only choose which SBS half that one view occupies. The existing
            // projection/camera hooks use g_renderRightEye to choose the frozen XR eye.
            unsigned char* lp = static_cast<unsigned char*>(localPlayer);
            float* originX = reinterpret_cast<float*>(lp + 0xA8);
            float* originY = reinterpret_cast<float*>(lp + 0xAC);
            float* sizeX = reinterpret_cast<float*>(lp + 0xB0);
            float* sizeY = reinterpret_cast<float*>(lp + 0xB4);
            const float ox=*originX, oy=*originY, sx=*sizeX, sy=*sizeY;
            const bool right = g_v26SecondPass;
            g_renderRightEye = right;
            *originX = right ? ox + sx*0.5f : ox;
            *originY = oy;
            *sizeX = sx * 0.5f;
            *sizeY = sy;
            result = g_originalCalcSceneViewCandidate(
                localPlayer, viewFamily, viewLocation, viewRotation, viewport, viewDrawer);
            *originX=ox; *originY=oy; *sizeX=sx; *sizeY=sy;

            static unsigned int v26CalcLogs=0;
            if (v26CalcLogs++ < 24)
                DxvkPathTrace("V26 SINGLE_VIEW eye=%s view=0x%llX family=%p snap=%llu",
                    right ? "RIGHT" : "LEFT", result, viewFamily,
                    (unsigned long long)g_renderPoseSnapshotSerial);
        }
        else if (doNativeStereo)
        {
            // genuine co-op-style LEFT+RIGHT FSceneViews, but emulate the
            // piece a real second LocalPlayer naturally owns: independent
            //
            // No CAM1, no matrix copying, no post-CalcSceneView repair.
            unsigned char* lp = static_cast<unsigned char*>(localPlayer);
            float* originX = reinterpret_cast<float*>(lp + 0xA8);
            float* originY = reinterpret_cast<float*>(lp + 0xAC);
            float* sizeX = reinterpret_cast<float*>(lp + 0xB0);
            float* sizeY = reinterpret_cast<float*>(lp + 0xB4);
            const float ox=*originX, oy=*originY, sx=*sizeX, sy=*sizeY;

            FVectorUE3 cameraInputLocation{};
            FRotatorUE3 cameraInputRotation{};
            FVectorUE3 leftOutLocation{};
            FRotatorUE3 leftOutRotation{};
            if (viewLocation) cameraInputLocation=*viewLocation;
            if (viewRotation) cameraInputRotation=*viewRotation;

            unsigned char savedLeftHistory[12] = {};
            memcpy(savedLeftHistory, lp + 0x110, sizeof(savedLeftHistory));

            g_renderRightEye=false;
            g_calcSceneRightEye=false;
            *originX=ox; *originY=oy; *sizeX=sx*0.5f; *sizeY=sy;
            result = g_originalCalcSceneViewCandidate(
                localPlayer, viewFamily, viewLocation, viewRotation,
                viewport, viewDrawer);
            if (viewLocation) leftOutLocation=*viewLocation;
            if (viewRotation) leftOutRotation=*viewRotation;

            // the outer AHiT player loop normally commits each LocalPlayer's
            // completed camera to +0x110 after CalcSceneView. Preserve LEFT's
            // normal result for caller/outer-loop handling, but do not expose
            uintptr_t* primaryViewState =
                reinterpret_cast<uintptr_t*>(lp + 0xF0);
            const uintptr_t savedPrimaryState=*primaryViewState;
            const uintptr_t rightState=V51GetOrCreateRightViewState();

            unsigned long long rightResult=0;
            if (result && rightState)
            {
                if (!g_r257RightCameraHistoryValid)
                {
                    memcpy(g_r257RightCameraHistory,
                           savedLeftHistory,
                           sizeof(g_r257RightCameraHistory));
                    g_r257RightCameraHistoryValid=true;
                }

                memcpy(lp + 0x110,
                       g_r257RightCameraHistory,
                       sizeof(g_r257RightCameraHistory));

                *primaryViewState=rightState;
                if (viewLocation) *viewLocation=cameraInputLocation;
                if (viewRotation) *viewRotation=cameraInputRotation;
                g_renderRightEye=true;
                g_calcSceneRightEye=true;
                *originX=ox+sx*0.5f; *originY=oy; *sizeX=sx*0.5f; *sizeY=sy;

                rightResult = g_originalCalcSceneViewCandidate(
                    localPlayer, viewFamily, viewLocation, viewRotation,
                    viewport, viewDrawer);

                // CalcSceneView derives FSceneView::PlayerIndex from the
                // LocalPlayer's ordinal in GEngine->GamePlayers. Because HatVR's
                // synthetic RIGHT reuses LocalPlayer 0, AHiT constructs BOTH eyes
                // with PlayerIndex 0. Real two-player co-op constructs [0, 1].
                //
                // FSceneView ctor 0x7F1D90 stores its fifth argument at +0x14.
                // CalcSceneView does not consume +0x14 again after construction,
                // so retag RIGHT before the renderer sees the completed family.
                if (rightResult)
                    *reinterpret_cast<int*>(
                        static_cast<uintptr_t>(rightResult) + 0x14) = 1;

                // Mirror what AHiT's outer co-op loop does for a real LocalPlayer:
                if (viewLocation)
                    memcpy(g_r257RightCameraHistory,
                           viewLocation,
                           sizeof(g_r257RightCameraHistory));
            }

            *primaryViewState=savedPrimaryState;
            memcpy(lp + 0x110, savedLeftHistory, sizeof(savedLeftHistory));
            *originX=ox; *originY=oy; *sizeX=sx; *sizeY=sy;
            if (viewLocation) *viewLocation=leftOutLocation;
            if (viewRotation) *viewRotation=leftOutRotation;
            g_renderRightEye=false;
            g_calcSceneRightEye=false;

            static unsigned int r257Logs=0;
            if (r257Logs++ < 24)
                DxvkPathTrace(
                    "R259 COOP_PLAYER_IDENTITY L=0x%llX R=0x%llX family=%p rightState=%p snap=%llu",
                    result, rightResult, viewFamily,
                    reinterpret_cast<void*>(rightState),
                    (unsigned long long)g_renderPoseSnapshotSerial);
        }
        else
        {
            // AFR compatibility mode: render exactly one eye per game frame into
            // its SBS half. Present-time capture retains the previous opposite eye.
            // this keeps UE3 on a normal single-view renderer path so effects that
            // assume one FSceneView can still execute normally.
            const bool doAfr =
                !g_nativeStereoEnabled && g_alternatingStereoEnabled &&
                g_xrViewsValidThisFrame && localPlayer &&
                calcReturnRva == kGameplayCalcSceneViewReturnRva;
            if (doAfr)
            {
                unsigned char* lp = static_cast<unsigned char*>(localPlayer);
                float* originX = reinterpret_cast<float*>(lp + 0xA8);
                float* originY = reinterpret_cast<float*>(lp + 0xAC);
                float* sizeX = reinterpret_cast<float*>(lp + 0xB0);
                float* sizeY = reinterpret_cast<float*>(lp + 0xB4);
                const float ox=*originX, oy=*originY, sx=*sizeX, sy=*sizeY;

                g_v138SameTickAfrActive = true;
                g_v146GenuineRightView = nullptr;
                g_v146GenuineRightFamily = nullptr;
                g_v146GenuineRightPoseSerial = 0;
                g_v151GenuineLeftView = nullptr;

                // Genuine LEFT: normal first view retained in the family.
                g_renderRightEye = false;
                *originX = ox;
                *originY = oy;
                *sizeX = sx * 0.5f;
                *sizeY = sy;
                result = g_originalCalcSceneViewCandidate(
                    localPlayer, viewFamily, viewLocation, viewRotation, viewport, viewDrawer);
                if (result)
                    g_v151GenuineLeftView =
                        reinterpret_cast<void*>(static_cast<uintptr_t>(result));

                // Genuine RIGHT: run AHiT's CalcSceneView again so every engine
                // side effect associated with creating a RIGHT view happens
                // naturally.  Give it the native second-eye ViewState while the
                // call executes, matching the old sequential Native path.
                uintptr_t* lpViewState = reinterpret_cast<uintptr_t*>(lp + 0xF0);
                const uintptr_t savedViewState = *lpViewState;
                const uintptr_t rightState = V51GetOrCreateRightViewState();
                if (rightState) *lpViewState = rightState;

                g_renderRightEye = true;
                *originX = ox + sx * 0.5f;
                *originY = oy;
                *sizeX = sx * 0.5f;
                *sizeY = sy;

                const unsigned long long genuineRight =
                    g_originalCalcSceneViewCandidate(
                        localPlayer, viewFamily, viewLocation, viewRotation, viewport, viewDrawer);

                // before 0x81C9C0 creates the completed RIGHT FViewInfo.
                if (genuineRight && g_v151GenuineLeftView)
                {
                    __try
                    {
                        auto* camLeft =
                            static_cast<unsigned char*>(g_v151GenuineLeftView);
                        auto* camRight =
                            reinterpret_cast<unsigned char*>(
                                static_cast<uintptr_t>(genuineRight));

                        memcpy(camRight + 0x90, camLeft + 0x90, 0x40);
                        memcpy(camRight + 0xD0, camLeft + 0xD0, 0x40);

                        const XrView* camViews = g_renderPoseSnapshotValid
                            ? g_renderPoseSnapshotViews : g_xrViews;
                        const float dx = camViews[1].pose.position.x -
                                         camViews[0].pose.position.x;
                        const float dy = camViews[1].pose.position.y -
                                         camViews[0].pose.position.y;
                        const float dz = camViews[1].pose.position.z -
                                         camViews[0].pose.position.z;
                        const float ipdUU =
                            sqrtf(dx*dx + dy*dy + dz*dz) *
                            g_xrWorldUnitsPerMeter;

                        float* rightViewMatrix =
                            reinterpret_cast<float*>(camRight + 0x90);
                        rightViewMatrix[12] -= ipdUU;

                        using RebuildViewFn = void(__fastcall*)(void*, int);
                        const uintptr_t exeBase =
                            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                        auto rebuild = reinterpret_cast<RebuildViewFn>(
                            exeBase + 0x80A190);
                        rebuild(camRight, 0);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }

                *lpViewState = savedViewState;
                *originX=ox; *originY=oy; *sizeX=sx; *sizeY=sy;

                // CalcSceneView appends RIGHT to the family. Preserve the pointer
                // for renderer #2, then restore the visible family to LEFT-only so
                if (genuineRight && viewFamily)
                {
                    __try
                    {
                        auto* fam = reinterpret_cast<unsigned char*>(viewFamily);
                        void** data = *reinterpret_cast<void***>(fam + 0x00);
                        int count = *reinterpret_cast<int*>(fam + 0x08);
                        if (data && count >= 2)
                        {
                            g_v146GenuineRightView = reinterpret_cast<void*>(genuineRight);
                            g_v146GenuineRightFamily = viewFamily;
                            g_v146GenuineRightPoseSerial = g_renderPoseSnapshotSerial;
                            *reinterpret_cast<int*>(fam + 0x08) = 1;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        g_v146GenuineRightView = nullptr;
                    }
                }

                // Producer #1 is LEFT. HookSceneRendererCreate swaps the exact
                // saved genuine RIGHT into producer #2.
                g_renderRightEye = false;

                static unsigned int afrLogs=0;
                if (afrLogs++ < 24)
                    DxvkPathTrace(
                        "V154 GENUINE_CALC_PAIR present=%llu left=0x%llX right=0x%llX family=%p snap=%llu captured=%d",
                        (unsigned long long)g_presentFrameNumber,
                        result,
                        genuineRight,
                        viewFamily,
                        (unsigned long long)g_renderPoseSnapshotSerial,
                        g_v146GenuineRightView ? 1 : 0);
            }
            else
            {
                g_renderRightEye = false;
                result = g_originalCalcSceneViewCandidate(
                    localPlayer,
                    viewFamily,
                    viewLocation,
                    viewRotation,
                    viewport,
                    viewDrawer
                );
            }

            if (calcReturnRva == kGameplayCalcSceneViewReturnRva)
            {
                static unsigned int phase72bSingleLogs = 0;
                if (phase72bSingleLogs++ < 32)
                {
                    char p72b[768] = {};
                    if (viewLocation && viewRotation)
                        sprintf_s(p72b, sizeof(p72b),
                            "PHASE8.3 SINGLE-RETURN call=%llu view=0x%llX loc=[%.5f %.5f %.5f] rot=[%d %d %d] family=%p\n",
                            callCount, result, viewLocation->X, viewLocation->Y, viewLocation->Z,
                            viewRotation->Pitch, viewRotation->Yaw, viewRotation->Roll, viewFamily);
                    else
                        sprintf_s(p72b, sizeof(p72b),
                            "PHASE8.3 SINGLE-RETURN call=%llu view=0x%llX loc/rot unavailable family=%p\n",
                            callCount, result, viewFamily);
                    DxvkPathTrace("%s", p72b);
                }
            }
        }

        return result;
    }

