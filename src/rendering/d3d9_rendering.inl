    // Debug texture management

    static void ReleaseFrameCopy()
    {
        if (g_frameCopy)
        {
            g_frameCopy->Release();
            g_frameCopy = nullptr;
        }

        g_copyWidth = 0;
        g_copyHeight = 0;
        g_copyFormat = D3DFMT_UNKNOWN;
    }

    static bool EnsureFrameCopy(
        IDirect3DDevice9* device,
        UINT width,
        UINT height,
        D3DFORMAT format)
    {
        if (g_frameCopy &&
            g_copyWidth == width &&
            g_copyHeight == height &&
            g_copyFormat == format)
        {
            return true;
        }

        ReleaseFrameCopy();

        HRESULT hr = device->CreateTexture(
            width,
            height,
            1,
            D3DUSAGE_RENDERTARGET,
            format,
            D3DPOOL_DEFAULT,
            &g_frameCopy,
            nullptr
        );

        if (FAILED(hr))
        {
            OutputDebugStringA(
                "[AHiTVR] CreateTexture for debug SBS FAILED\n"
            );

            return false;
        }

        g_copyWidth = width;
        g_copyHeight = height;
        g_copyFormat = format;

        OutputDebugStringA(
            "[AHiTVR] Debug SBS texture created\n"
        );

        return true;
    }

    // Draw completed frame twice

    static void ReleaseAlternatingStereoCopies()
    {
        if (g_leftEyeCopy)
        {
            g_leftEyeCopy->Release();
            g_leftEyeCopy = nullptr;
        }

        if (g_rightEyeCopy)
        {
            g_rightEyeCopy->Release();
            g_rightEyeCopy = nullptr;
        }

        g_stereoCopyWidth = 0;
        g_stereoCopyHeight = 0;
        g_stereoCopyFormat = D3DFMT_UNKNOWN;
        g_haveLeftEye = false;
        g_haveRightEye = false;
    }

    static bool EnsureAlternatingStereoCopies(
        IDirect3DDevice9* device,
        const D3DSURFACE_DESC& desc)
    {
        const UINT eyeWidth = (std::max)(1u, desc.Width / 2u);
        const UINT eyeHeight = desc.Height;

        // native Stereo's direct-SBS fast path deliberately aliases both
        // "eye copy" pointers to g_nativeVrColorTexture.  When switching live
        // from Native Stereo -> AFR, the dimensions/format still match, so the
        // half-width retention textures. StretchRect then tried to copy from
        // g_nativeVrColorSurface back into a surface of the SAME native SBS
        // texture and failed every frame (AFR capture ok=0).
        //
        // AFR requires two genuinely separate eye textures. Reject any direct-
        // SBS aliases here so ReleaseAlternatingStereoCopies() drops the AddRef'd
        // aliases and the dedicated 1344x1440 textures are allocated below.
        const bool aliasesNativeSbs =
            g_nativeVrColorTexture &&
            (g_leftEyeCopy == g_nativeVrColorTexture ||
             g_rightEyeCopy == g_nativeVrColorTexture);

        if (g_leftEyeCopy &&
            g_rightEyeCopy &&
            !aliasesNativeSbs &&
            g_stereoCopyWidth == eyeWidth &&
            g_stereoCopyHeight == eyeHeight &&
            g_stereoCopyFormat == desc.Format)
        {
            return true;
        }

        if (aliasesNativeSbs)
        {
            DxvkPathTrace(
                "AFR transition: replacing direct-SBS eye aliases with dedicated %ux%u retention textures",
                eyeWidth, eyeHeight);
        }

        ReleaseAlternatingStereoCopies();

        HRESULT hr = device->CreateTexture(
            eyeWidth,
            eyeHeight,
            1,
            D3DUSAGE_RENDERTARGET,
            desc.Format,
            D3DPOOL_DEFAULT,
            &g_leftEyeCopy,
            nullptr
        );

        if (FAILED(hr))
        {
            DebugLog("CreateTexture LEFT alternating eye FAILED\n");
            ReleaseAlternatingStereoCopies();
            return false;
        }

        hr = device->CreateTexture(
            eyeWidth,
            eyeHeight,
            1,
            D3DUSAGE_RENDERTARGET,
            desc.Format,
            D3DPOOL_DEFAULT,
            &g_rightEyeCopy,
            nullptr
        );

        if (FAILED(hr))
        {
            DebugLog("CreateTexture RIGHT alternating eye FAILED\n");
            ReleaseAlternatingStereoCopies();
            return false;
        }

        g_stereoCopyWidth = eyeWidth;
        g_stereoCopyHeight = eyeHeight;
        g_stereoCopyFormat = desc.Format;

        char line[256] = {};
        sprintf_s(
            line,
            sizeof(line),
            "Native stereo eye copies created: %ux%u each from SBS %ux%u format=%d\n",
            eyeWidth,
            eyeHeight,
            desc.Width,
            desc.Height,
            static_cast<int>(desc.Format)
        );
        DebugLog(line);

        return true;
    }

    static void CaptureAndDrawAlternatingStereoSBS(IDirect3DDevice9* device)
    {
        // the generation represent a COMPLETE stereo pixel pair and stamped the
        // SBS accidentally regressed that rule when the StretchRect split was
        // removed.

        // AFR compatibility path. One UE3 view is rendered per frame into the
        // selected SBS half; retain the previous opposite eye in dedicated copies.
        if (IsVulkanBackendActive() && !g_nativeStereoEnabled &&
            !g_v138SameTickAfrActive &&
            g_nativeVrResolutionEnabled && g_nativeVrColorSurface)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(g_nativeVrColorSurface->GetDesc(&desc)) && desc.Width >= 2 &&
                EnsureAlternatingStereoCopies(device, desc))
            {
                IDirect3DSurface9* dst = nullptr;
                IDirect3DTexture9* dstTex = g_renderRightEye ? g_rightEyeCopy : g_leftEyeCopy;
                if (dstTex && SUCCEEDED(dstTex->GetSurfaceLevel(0, &dst)) && dst)
                {
                    const LONG half=(LONG)desc.Width/2;
                    RECT src = g_renderRightEye
                        ? RECT{half,0,(LONG)desc.Width,(LONG)desc.Height}
                        : RECT{0,0,half,(LONG)desc.Height};
                    const bool ok = SUCCEEDED(device->StretchRect(
                        g_nativeVrColorSurface, &src, dst, nullptr, D3DTEXF_NONE));
                    if (g_renderRightEye) g_haveRightEye = ok; else g_haveLeftEye = ok;
                    dst->Release();
                    if (ok && g_haveLeftEye && g_haveRightEye)
                    {
                        const unsigned long long capturedPose=(unsigned long long)
                            InterlockedCompareExchange64(&g_lastExecutedScenePoseSerial,0,0);
                        InterlockedExchange64(&g_capturedStereoPoseSerial,(LONG64)capturedPose);
                        InterlockedIncrement64(&g_stereoCaptureGeneration);
                    }
                    static unsigned int afrCaptureLogs=0;
                    if (afrCaptureLogs++ < 24)
                        DxvkPathTrace("AFR capture eye=%s ok=%d haveLR=%d%d",
                            g_renderRightEye ? "RIGHT" : "LEFT", ok?1:0,
                            g_haveLeftEye?1:0,g_haveRightEye?1:0);
                    g_renderRightEye=false;
                    return;
                }
                if (dst) dst->Release();
            }
        }

        // SPLIT-AFR-1:
        // Same-tick AFR has already rendered BOTH eyes into the native SBS target
        // by the time Present reaches us.  Do not alias that full-width texture as
        // both eye copies.  Split the completed pair into two independent,
        // eye-sized D3D9 render-target textures and let the existing Vulkan/OpenXR
        // path consume those textures normally.
        //
        // this intentionally changes capture only.  UE3 rendering, FSceneView /
        // ownership are untouched.
        if (IsVulkanBackendActive() && !g_nativeStereoEnabled &&
            g_v138SameTickAfrActive &&
            g_nativeVrResolutionEnabled && g_nativeVrColorSurface)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(g_nativeVrColorSurface->GetDesc(&desc)) && desc.Width >= 2 &&
                EnsureAlternatingStereoCopies(device, desc))
            {
                IDirect3DSurface9* leftSurface = nullptr;
                IDirect3DSurface9* rightSurface = nullptr;

                const HRESULT leftGet =
                    g_leftEyeCopy
                        ? g_leftEyeCopy->GetSurfaceLevel(0, &leftSurface)
                        : E_FAIL;
                const HRESULT rightGet =
                    g_rightEyeCopy
                        ? g_rightEyeCopy->GetSurfaceLevel(0, &rightSurface)
                        : E_FAIL;

                bool leftOk = false;
                bool rightOk = false;

                if (SUCCEEDED(leftGet) && SUCCEEDED(rightGet) &&
                    leftSurface && rightSurface)
                {
                    const LONG width = static_cast<LONG>(desc.Width);
                    const LONG height = static_cast<LONG>(desc.Height);
                    const LONG halfWidth = width / 2;

                    RECT leftSrc{ 0, 0, halfWidth, height };
                    RECT rightSrc{ halfWidth, 0, width, height };

                    leftOk = SUCCEEDED(device->StretchRect(
                        g_nativeVrColorSurface, &leftSrc,
                        leftSurface, nullptr, D3DTEXF_NONE));

                    rightOk = SUCCEEDED(device->StretchRect(
                        g_nativeVrColorSurface, &rightSrc,
                        rightSurface, nullptr, D3DTEXF_NONE));
                }

                if (leftSurface) leftSurface->Release();
                if (rightSurface) rightSurface->Release();

                g_haveLeftEye = leftOk;
                g_haveRightEye = rightOk;

                if (leftOk && rightOk)
                {
                    const unsigned long long capturedPose =
                        (unsigned long long)InterlockedCompareExchange64(
                            &g_lastExecutedScenePoseSerial, 0, 0);

                    InterlockedExchange64(
                        &g_capturedStereoPoseSerial, (LONG64)capturedPose);

                    const unsigned long long captureGeneration =
                        (unsigned long long)InterlockedIncrement64(
                            &g_stereoCaptureGeneration);

                    static unsigned int splitAfrLogs = 0;
                    if (splitAfrLogs++ < 24)
                    {
                        DxvkPathTrace(
                            "SPLIT-AFR-1 SAME_TICK pair split ok=11 "
                            "sbs=%ux%u eyes=%ux%u captureGen=%llu pixelPose=%llu",
                            desc.Width, desc.Height,
                            desc.Width / 2u, desc.Height,
                            captureGeneration, capturedPose);
                    }

                    g_renderRightEye = false;
                    return;
                }

                static unsigned int splitAfrFailLogs = 0;
                if (splitAfrFailLogs++ < 24)
                {
                    DxvkPathTrace(
                        "SPLIT-AFR-1 SAME_TICK split FAILED left=%d right=%d "
                        "sbs=%ux%u",
                        leftOk ? 1 : 0, rightOk ? 1 : 0,
                        desc.Width, desc.Height);
                }

                // do not fall through into Direct-SBS for this path:
                // that would silently turn a failed split back into the old path.
                g_renderRightEye = false;
                return;
            }
        }

        // R186: Stereo intentionally uses the same dedicated per-eye retention
        // model as Synchronized Sequential.  The old Vulkan DIRECT-SBS path
        // aliased both eye-copy pointers to the same native SBS texture and made
        // the Vulkan/OpenXR bridge switch between shared-SBS and independent-eye
        // ownership when changing rendering modes.  That transition could leave
        // the right eye frozen (and repeated transitions could destabilize the
        // bridge).  Keep the native SBS render target as UE3's render target, but
        // split the completed pair below with two GPU StretchRects.  Downstream,
        // both rendering modes now always present independent eye-sized textures.

        // Split the completed SBS pair into independent eye textures.  This is
        IDirect3DSurface9* backbuffer = nullptr;
        if (g_nativeVrResolutionEnabled && g_nativeVrColorSurface)
        {
            backbuffer = g_nativeVrColorSurface;
            backbuffer->AddRef();
        }
        else if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer)
        {
            return;
        }

        D3DSURFACE_DESC desc{};
        if (FAILED(backbuffer->GetDesc(&desc)) ||
            !EnsureAlternatingStereoCopies(device, desc))
        {
            backbuffer->Release();
            return;
        }

        IDirect3DSurface9* leftSurface = nullptr;
        IDirect3DSurface9* rightSurface = nullptr;
        if (SUCCEEDED(g_leftEyeCopy->GetSurfaceLevel(0, &leftSurface)) &&
            SUCCEEDED(g_rightEyeCopy->GetSurfaceLevel(0, &rightSurface)) &&
            leftSurface && rightSurface)
        {
            const LONG width = static_cast<LONG>(desc.Width);
            const LONG height = static_cast<LONG>(desc.Height);
            const LONG halfWidth = width / 2;
            RECT leftSrc{ 0, 0, halfWidth, height };
            RECT rightSrc{ halfWidth, 0, width, height };
            const HRESULT leftHr = device->StretchRect(backbuffer, &leftSrc, leftSurface, nullptr, D3DTEXF_NONE);
            const HRESULT rightHr = device->StretchRect(backbuffer, &rightSrc, rightSurface, nullptr, D3DTEXF_NONE);
            g_haveLeftEye = SUCCEEDED(leftHr);
            g_haveRightEye = SUCCEEDED(rightHr);

            // capture generation == a complete stereo pair, tagged with the
            // renderer pose that actually produced its pixels.
            if (g_haveLeftEye && g_haveRightEye)
            {
                const unsigned long long capturedPose = (unsigned long long)
                    InterlockedCompareExchange64(&g_lastExecutedScenePoseSerial, 0, 0);
                InterlockedExchange64(&g_capturedStereoPoseSerial, (LONG64)capturedPose);
                InterlockedIncrement64(&g_stereoCaptureGeneration);
            }
        }
        if (leftSurface) leftSurface->Release();
        if (rightSurface) rightSurface->Release();
        backbuffer->Release();
        g_renderRightEye = false;
    }

    // r182 desktop spectator compositor. This runs only after the OpenXR frame
    // has been submitted. It writes exclusively to the normal D3D9 backbuffer;
    // the retained eye textures consumed by OpenXR are source-only here.
    static void DrawDesktopSpectator(
        IDirect3DDevice9* device,
        int viewMode,
        int uiMode,
        IDirect3DTexture9* uiTexture)
    {
        if (!device || viewMode <= 0 || viewMode > 3 ||
            !g_leftEyeCopy || !g_rightEyeCopy || !g_stereoCopyWidth || !g_stereoCopyHeight)
            return;

        IDirect3DSurface9* backbuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer)
            return;

        D3DSURFACE_DESC bb{};
        if (FAILED(backbuffer->GetDesc(&bb)) || !bb.Width || !bb.Height)
        {
            backbuffer->Release();
            return;
        }

        auto getEyeSurface = [&](IDirect3DTexture9* tex, bool right,
                                 IDirect3DSurface9** outSurface, RECT& src)->bool
        {
            if (!tex || !outSurface) return false;
            *outSurface = nullptr;
            if (FAILED(tex->GetSurfaceLevel(0, outSurface)) || !*outSurface) return false;
            D3DSURFACE_DESC d{};
            if (FAILED((*outSurface)->GetDesc(&d))) { (*outSurface)->Release(); *outSurface=nullptr; return false; }
            // Phase-8 Direct-SBS aliases both eyes to one 2x-wide texture. The
            // normal synchronized-sequential path has one eye-sized texture each.
            if (d.Width >= g_stereoCopyWidth * 2u)
            {
                // Direct-SBS aliases both logical eyes to the same physical
                // texture. Derive the eye rectangle from the real surface, not
                // cached OpenXR dimensions: the desktop crop must preserve the
                // exact aspect ratio of the pixels we are actually presenting.
                const LONG eyeW = (LONG)(d.Width / 2u);
                const LONG eyeH = (LONG)d.Height;
                const LONG x0 = right ? eyeW : 0;
                src = RECT{x0, 0, x0 + eyeW, eyeH};
            }
            else
            {
                src = RECT{0, 0, (LONG)d.Width, (LONG)d.Height};
            }
            return true;
        };

        // the retained VR eye is NOT a square-pixel flat image. UE3 renders an
        // OpenXR-derived projection into an eye-sized/SBS rectangle (for example
        // 1344x1440), then the Vulkan bridge interprets that projection when it
        // crops/submits to OpenXR. Treating 1344/1440 as the image's visual aspect
        // makes a desktop mirror look horizontally squashed / vertically tall.
        // Recover the visual aspect from the same XR frustum used by the projection
        // hook and use the texture dimensions only to choose pixel crop coordinates.
        // Spectator projection is a property of the HMD/runtime, not of the game
        // scene.  r183 recomputed it from whichever view array happened to be
        // marked valid at Present time.  During camera/scene transitions those
        // validity flags can briefly drop or a transient render snapshot can be
        // selected, making one desktop frame fall back to the raw eye texture
        // aspect.  That presents as a one-frame "whole eye stretched to window"
        // flash.  Latch the last sane XR-derived aspect instead.
        static double s_spectatorEyeAspect[2] = { 0.0, 0.0 };

        auto aspectFromViews = [&](const XrView* pv, int eye)->double
        {
            if (!pv) return 0.0;

            if (g_nativeStereoEnabled)
            {
                const float l0=tanf(pv[0].fov.angleLeft), r0=tanf(pv[0].fov.angleRight);
                const float l1=tanf(pv[1].fov.angleLeft), r1=tanf(pv[1].fov.angleRight);
                eye = eye ? 1 : 0;
                const XrFovf& f = pv[eye].fov;
                const float height=tanf(f.angleUp)-tanf(f.angleDown);
                const float halfX=(std::max)((std::max)(fabsf(l0),fabsf(r0)),(std::max)(fabsf(l1),fabsf(r1)));
                return (halfX > 0.0001f && fabsf(height) > 0.0001f)
                    ? double((2.0f*halfX)/height) : 0.0;
            }

            eye = eye ? 1 : 0;
            const XrFovf& f = pv[eye].fov;
            const float tanL=tanf(f.angleLeft), tanR=tanf(f.angleRight);
            const float tanD=tanf(f.angleDown), tanU=tanf(f.angleUp);
            const float sourceHalfX=(std::max)(fabsf(tanL),fabsf(tanR));
            const float height=tanU-tanD;
            return (sourceHalfX > 0.0001f && fabsf(height) > 0.0001f)
                ? double((2.0f*sourceHalfX)/height) : 0.0;
        };

        auto projectedEyeAspect = [&](int eye, const RECT& src)->double
        {
            eye = eye ? 1 : 0;
            const double pixelFallback =
                double(src.right-src.left) / double(src.bottom-src.top);

            const XrView* pv = g_xrViewsValidThisFrame
                ? g_xrViews
                : (g_renderPoseSnapshotValid ? g_renderPoseSnapshotViews : nullptr);

            double candidate = 0.0;
            if (pv)
            {
                const XrFovf& f=pv[eye].fov;
                const float tanL=tanf(f.angleLeft), tanR=tanf(f.angleRight);
                const float tanD=tanf(f.angleDown), tanU=tanf(f.angleUp);
                const float width=tanR-tanL;
                const float height=tanU-tanD;

                if (fabsf(height) > 0.0001f)
                {
                    if (!g_spectatorExpandedFov || g_sharperNativeStereo)
                    {
                        candidate=double(width/height);
                    }
                    else
                    {
                        // expanded uses the same per-eye symmetric envelope in
                        // stereo and sequential.
                        const float halfX=(std::max)(fabsf(tanL),fabsf(tanR));
                        if (halfX > 0.0001f)
                            candidate=double((2.0f*halfX)/height);
                    }
                }
            }

            if (candidate > 0.35 && candidate < 3.5 && std::isfinite(candidate))
                s_spectatorEyeAspect[eye] = candidate;

            if (s_spectatorEyeAspect[eye] > 0.0)
                return s_spectatorEyeAspect[eye];

            return pixelFallback;
        };

        auto cropToFillProjected = [](const RECT& src, double visualAspect,
                                      UINT dstW, UINT dstH)->RECT
        {
            RECT r=src;
            const double sw=double(src.right-src.left), sh=double(src.bottom-src.top);
            const double dstAspect=double(dstW)/double(dstH);
            if (visualAspect <= 0.000001 || dstAspect <= 0.000001)
                return r;

            // Crop in projection/display space, then convert that normalized crop
            // back into source pixels. Scaling to the desktop is uniform in visual
            // space even though the intermediate VR texture itself is non-square.
            if (visualAspect > dstAspect)
            {
                const double keep=dstAspect/visualAspect;
                const LONG wanted=(LONG)(sw*keep+0.5);
                const LONG trim=((LONG)sw-wanted)/2;
                r.left+=trim; r.right=r.left+(std::max)(1L,wanted);
            }
            else if (visualAspect < dstAspect)
            {
                const double keep=visualAspect/dstAspect;
                const LONG wanted=(LONG)(sh*keep+0.5);
                const LONG trim=((LONG)sh-wanted)/2;
                r.top+=trim; r.bottom=r.top+(std::max)(1L,wanted);
            }
            return r;
        };

        IDirect3DSurface9* left = nullptr;
        IDirect3DSurface9* right = nullptr;
        RECT leftSrc{}, rightSrc{};
        const bool haveL = getEyeSurface(g_leftEyeCopy, false, &left, leftSrc);
        const bool haveR = getEyeSurface(g_rightEyeCopy, true, &right, rightSrc);

        // Theater presents a centered 16:9 slice of each rendered eye.
        // the desktop spectator must mirror that same source rectangle instead
        // of independently treating the full tall VR eye as its image.
        auto cropEyeToTheater169 = [](RECT& r)
        {
            const LONG w = r.right - r.left;
            const LONG h = r.bottom - r.top;
            if (w <= 0 || h <= 0) return;
            const double target = 16.0 / 9.0;
            const double a = double(w) / double(h);
            if (a < target)
            {
                const LONG wantedH = (std::max)(1L, (LONG)(double(w) / target + 0.5));
                const LONG trim = (h - wantedH) / 2;
                r.top += trim;
                r.bottom = r.top + wantedH;
            }
            else if (a > target)
            {
                const LONG wantedW = (std::max)(1L, (LONG)(double(h) * target + 0.5));
                const LONG trim = (w - wantedW) / 2;
                r.left += trim;
                r.right = r.left + wantedW;
            }
        };

        if (g_theaterMode)
        {
            if (haveL) cropEyeToTheater169(leftSrc);
            if (haveR) cropEyeToTheater169(rightSrc);
        }
        else if (!g_spectatorExpandedFov && (!g_nativeStereoEnabled || !g_sharperNativeStereo))
        {
            // stereo and sequential both render a wider symmetric horizontal
            // frustum and crop it to the runtime eye during submission.
            // Keep the last valid runtime-FOV crop for each eye. Present can
            // occasionally arrive while both XR view-valid flags are transiently
            // false (scene/camera transitions are the common case). Previously
            // that made this function return without cropping, so one desktop
            // frame exposed the full wide render envelope and looked horizontally
            // squashed. The projection itself has not changed, so retain its last
            // sane normalized crop until a newer valid XR view replaces it.
            static float s_runtimeCropOffsetX[2] = { 0.0f, 0.0f };
            static float s_runtimeCropScaleX[2] = { 1.0f, 1.0f };
            static bool s_runtimeCropValid[2] = { false, false };

            auto cropEyeToRuntimeFov = [&](RECT& r, int eye)
            {
                eye = eye ? 1 : 0;
                const XrView* pv = g_xrViewsValidThisFrame
                    ? g_xrViews
                    : (g_renderPoseSnapshotValid ? g_renderPoseSnapshotViews : nullptr);

                if (pv)
                {
                    const XrFovf& fov = pv[eye].fov;
                    const float tanL = tanf(fov.angleLeft);
                    const float tanR = tanf(fov.angleRight);
                    const float sourceHalfX = (std::max)(fabsf(tanL), fabsf(tanR));
                    if (sourceHalfX > 0.0001f)
                    {
                        const float scaleX = (tanR - tanL) / (2.0f * sourceHalfX);
                        const float offsetX = (tanL + sourceHalfX) / (2.0f * sourceHalfX);
                        if (std::isfinite(scaleX) && std::isfinite(offsetX) &&
                            scaleX > 0.0f && scaleX <= 1.0001f &&
                            offsetX >= -0.0001f && offsetX < 1.0f &&
                            offsetX + scaleX <= 1.0001f)
                        {
                            s_runtimeCropOffsetX[eye] = offsetX;
                            s_runtimeCropScaleX[eye] = scaleX;
                            s_runtimeCropValid[eye] = true;
                        }
                    }
                }

                if (!s_runtimeCropValid[eye]) return;

                const LONG w = r.right - r.left;
                if (w <= 1) return;
                const float offsetX = s_runtimeCropOffsetX[eye];
                const float scaleX = s_runtimeCropScaleX[eye];
                LONG x0 = r.left + (LONG)floorf(offsetX * (float)w);
                LONG x1 = r.left + (LONG)ceilf((offsetX + scaleX) * (float)w);
                x0 = (std::max)(r.left, (std::min)(x0, r.right - 1));
                x1 = (std::max)(x0 + 1, (std::min)(x1, r.right));
                r.left = x0;
                r.right = x1;
            };

            if (haveL) cropEyeToRuntimeFov(leftSrc, 0);
            if (haveR) cropEyeToRuntimeFov(rightSrc, 1);
        }
        else if (g_spectatorExpandedFov && g_nativeStereoEnabled && !g_sharperNativeStereo)
        {
            // stereo renders one shared envelope. expanded spectator uses the
            // per-eye symmetric envelope so it matches sequential exactly.
            auto cropStereoToSequentialEnvelope = [&](RECT& r, int eye)
            {
                const XrView* pv = g_xrViewsValidThisFrame
                    ? g_xrViews
                    : (g_renderPoseSnapshotValid ? g_renderPoseSnapshotViews : nullptr);
                if (!pv) return;

                const float l0=tanf(pv[0].fov.angleLeft), r0=tanf(pv[0].fov.angleRight);
                const float l1=tanf(pv[1].fov.angleLeft), r1=tanf(pv[1].fov.angleRight);
                const float sharedHalfX=(std::max)((std::max)(fabsf(l0),fabsf(r0)),
                                                   (std::max)(fabsf(l1),fabsf(r1)));
                const XrFovf& f=pv[eye ? 1 : 0].fov;
                const float eyeHalfX=(std::max)(fabsf(tanf(f.angleLeft)),fabsf(tanf(f.angleRight)));
                if (sharedHalfX <= 0.0001f || eyeHalfX <= 0.0001f) return;

                const float keep=(std::min)(1.0f, eyeHalfX/sharedHalfX);
                const LONG w=r.right-r.left;
                const LONG wanted=(std::max)(1L,(LONG)floorf((float)w*keep+0.5f));
                const LONG trim=(w-wanted)/2;
                r.left+=trim;
                r.right=r.left+wanted;
            };

            if (haveL) cropStereoToSequentialEnvelope(leftSrc,0);
            if (haveR) cropStereoToSequentialEnvelope(rightSrc,1);
        }

        if (viewMode == 1 && haveL)
        {
            const double visualAspect = g_theaterMode ? (16.0/9.0) : projectedEyeAspect(0, leftSrc);
            RECT src = cropToFillProjected(leftSrc, visualAspect, bb.Width, bb.Height);
            device->StretchRect(left, &src, backbuffer, nullptr, D3DTEXF_LINEAR);
        }
        else if (viewMode == 2 && haveR)
        {
            const double visualAspect = g_theaterMode ? (16.0/9.0) : projectedEyeAspect(1, rightSrc);
            RECT src = cropToFillProjected(rightSrc, visualAspect, bb.Width, bb.Height);
            device->StretchRect(right, &src, backbuffer, nullptr, D3DTEXF_LINEAR);
        }
        else if (viewMode == 3 && haveL && haveR)
        {
            // Preserve the complete SBS pair. Fit it as one image and leave any
            // unused desktop area black instead of cropping either eye.
            device->ColorFill(backbuffer, nullptr, D3DCOLOR_XRGB(0,0,0));
            const double leftAspect = g_theaterMode ? (16.0/9.0) : projectedEyeAspect(0, leftSrc);
            const double rightAspect = g_theaterMode ? (16.0/9.0) : projectedEyeAspect(1, rightSrc);
            // Two eyes side-by-side have the sum of their visual aspects, not
            // the raw (pixelWidthL+pixelWidthR)/pixelHeight ratio.
            const double sbsAspect = leftAspect + rightAspect;
            const double dstAspect = double(bb.Width)/double(bb.Height);
            LONG outW=(LONG)bb.Width, outH=(LONG)bb.Height, x=0, y=0;
            if (sbsAspect > dstAspect) { outH=(LONG)(double(bb.Width)/sbsAspect+0.5); y=((LONG)bb.Height-outH)/2; }
            else { outW=(LONG)(double(bb.Height)*sbsAspect+0.5); x=((LONG)bb.Width-outW)/2; }
            const LONG mid=x+outW/2;
            RECT dl{x,y,mid,y+outH}, dr{mid,y,x+outW,y+outH};
            device->StretchRect(left,&leftSrc,backbuffer,&dl,D3DTEXF_LINEAR);
            device->StretchRect(right,&rightSrc,backbuffer,&dr,D3DTEXF_LINEAR);
        }

        if (left) left->Release();
        if (right) right->Release();

        // Overlay mode reuses the already-extracted game-UI texture. No new UI
        // render or CPU readback is performed. Draw it once over the whole window,
        // including Both-Eyes mode.
        if (uiMode == 1 && uiTexture)
        {
            IDirect3DStateBlock9* state = nullptr;
            if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state)) && state)
                state->Capture();

            device->SetRenderTarget(0, backbuffer);
            device->SetDepthStencilSurface(nullptr);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            // the extracted HUD is authored/composited as linear UI color for
            // the OpenXR path. The desktop backbuffer needs the normal sRGB
            // encode on this draw or the same UI appears noticeably darker.
            // StateBlock restores the game's previous value immediately after.
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetTexture(0, uiTexture);
            device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
            device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
            device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
            device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
            device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
            device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            struct V { float x,y,z,rhw,u,v; };
            const float w=(float)bb.Width, h=(float)bb.Height;
            V q[4]={{-0.5f,-0.5f,0,1,0,0},{w-0.5f,-0.5f,0,1,1,0},
                    {-0.5f,h-0.5f,0,1,0,1},{w-0.5f,h-0.5f,0,1,1,1}};
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V));
            device->SetTexture(0,nullptr);
            if (state) { state->Apply(); state->Release(); }
        }

        backbuffer->Release();
    }

    static void DrawDebugSBS(IDirect3DDevice9* device)
    {
        IDirect3DSurface9* backbuffer = nullptr;

        HRESULT hr = device->GetBackBuffer(
            0,
            0,
            D3DBACKBUFFER_TYPE_MONO,
            &backbuffer
        );

        if (FAILED(hr) || !backbuffer)
            return;

        D3DSURFACE_DESC desc = {};

        hr = backbuffer->GetDesc(&desc);

        if (FAILED(hr))
        {
            backbuffer->Release();
            return;
        }

        if (!EnsureFrameCopy(
            device,
            desc.Width,
            desc.Height,
            desc.Format))
        {
            backbuffer->Release();
            return;
        }

        IDirect3DSurface9* copySurface = nullptr;

        hr = g_frameCopy->GetSurfaceLevel(
            0,
            &copySurface
        );

        if (FAILED(hr) || !copySurface)
        {
            backbuffer->Release();
            return;
        }

        // copy the completed AHiT backbuffer before we overwrite it.

        hr = device->StretchRect(
            backbuffer,
            nullptr,
            copySurface,
            nullptr,
            D3DTEXF_NONE
        );

        if (SUCCEEDED(hr))
        {
            const LONG width =
                static_cast<LONG>(desc.Width);

            const LONG height =
                static_cast<LONG>(desc.Height);

            const LONG halfWidth =
                width / 2;

            RECT leftRect =
            {
                0,
                0,
                halfWidth,
                height
            };

            RECT rightRect =
            {
                halfWidth,
                0,
                width,
                height
            };

            // Scale the saved full frame into the LEFT half.

            device->StretchRect(
                copySurface,
                nullptr,
                backbuffer,
                &leftRect,
                D3DTEXF_LINEAR
            );

            // Scale the exact same frame into the RIGHT half.

            device->StretchRect(
                copySurface,
                nullptr,
                backbuffer,
                &rightRect,
                D3DTEXF_LINEAR
            );
        }

        copySurface->Release();
        backbuffer->Release();
    }

