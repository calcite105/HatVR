// AFR retained mono-UI resources.
// these are referenced by HookedPresent's AFR submit path.  The reorganized source
// lost their declarations; keep them module-local and null-initialized so the
// existing guarded retention path is safe until/if targets are created.
static IDirect3DTexture9* g_afrRetainedUiTexture = nullptr;
static IDirect3DSurface9* g_afrRetainedUiSurface = nullptr;
static bool g_afrRetainedUiValid = false;

    // Present

    static uint64_t HashShaderBytecode(IDirect3DVertexShader9* shader)
    {
        if (!shader)
            return 0;

        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0)
            return 0;

        std::vector<unsigned char> bytes(size);
        if (FAILED(shader->GetFunction(bytes.data(), &size)))
            return 0;

        // 64-bit FNV-1a. Stable for identical compiled shader bytecode.
        uint64_t hash = 14695981039346656037ULL;
        for (UINT i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static ShaderIdentity* FindInspectorShader(IDirect3DVertexShader9* shader)
    {
        for (auto& entry : g_shaderInspector)
        {
            if (entry.shader == shader)
                return &entry;
        }
        return nullptr;
    }

    static uint64_t GetInspectorShaderHash(IDirect3DVertexShader9* shader)
    {
        if (!shader)
            return 0;

        ShaderIdentity* entry = FindInspectorShader(shader);
        if (entry)
            return entry->hash;

        return HashShaderBytecode(shader);
    }

    static void ProbeKnownCameraShader(
        IDirect3DDevice9* device,
        IDirect3DVertexShader9* shader,
        UINT primitiveCount,
        UINT numVertices)
    {
        if (!device || !shader)
            return;

        const uint64_t hash = GetInspectorShaderHash(shader);
        CameraProbeSample* sample = nullptr;

        for (auto& candidate : g_cameraProbeSamples)
        {
            if (candidate.hash == hash)
            {
                sample = &candidate;
                break;
            }
        }

        if (!sample || sample->dumpCount >= 8)
            return;

        const ULONGLONG now = GetTickCount64();
        if (sample->lastDumpTick != 0 && now - sample->lastDumpTick < 1000ULL)
            return;

        float constants[16 * 4] = {};
        HRESULT hr = device->GetVertexShaderConstantF(0, constants, 16);

        char line[512] = {};
        sprintf_s(
            line,
            sizeof(line),
            "\nCAMERA HASH PROBE %u hash=%016llX hr=0x%08X primitives=%u vertices=%u\n",
            sample->dumpCount + 1,
            static_cast<unsigned long long>(hash),
            static_cast<unsigned int>(hr),
            primitiveCount,
            numVertices
        );
        DebugLog(line);

        if (SUCCEEDED(hr))
        {
            for (UINT reg = 0; reg < 16; ++reg)
            {
                const float* v = &constants[reg * 4];
                sprintf_s(
                    line,
                    sizeof(line),
                    "c%-2u = [% .6f, % .6f, % .6f, % .6f]\n",
                    reg,
                    v[0], v[1], v[2], v[3]
                );
                DebugLog(line);
            }
        }

        sample->lastDumpTick = now;
        ++sample->dumpCount;
    }

    static void RecordInspectorDraw(IDirect3DVertexShader9* shader)
    {
        if (!shader)
            return;

        ShaderIdentity* entry = FindInspectorShader(shader);
        if (entry)
        {
            ++entry->draws;
            return;
        }

        if (g_shaderInspectorFrozen)
            return;

        ShaderIdentity created{};
        created.shader = shader;
        created.hash = HashShaderBytecode(shader);
        created.draws = 1;
        g_shaderInspector.push_back(created);
    }

    static void SortInspector()
    {
        if (g_shaderInspector.empty())
            return;

        IDirect3DVertexShader9* selectedShader = nullptr;
        if (g_shaderInspectorSelected >= 0 &&
            g_shaderInspectorSelected < static_cast<int>(g_shaderInspector.size()))
        {
            selectedShader = g_shaderInspector[g_shaderInspectorSelected].shader;
        }

        std::sort(
            g_shaderInspector.begin(),
            g_shaderInspector.end(),
            [](const ShaderIdentity& a, const ShaderIdentity& b)
            {
                return a.draws > b.draws;
            }
        );

        if (selectedShader)
        {
            for (int i = 0; i < static_cast<int>(g_shaderInspector.size()); ++i)
            {
                if (g_shaderInspector[i].shader == selectedShader)
                {
                    g_shaderInspectorSelected = i;
                    break;
                }
            }
        }
    }

    static bool CopyTextToClipboard(HWND hwnd, const char* text)
    {
        if (!text || !OpenClipboard(hwnd))
            return false;

        EmptyClipboard();

        const size_t bytes = strlen(text) + 1;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory)
        {
            CloseClipboard();
            return false;
        }

        void* dst = GlobalLock(memory);
        if (!dst)
        {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }

        memcpy(dst, text, bytes);
        GlobalUnlock(memory);

        if (!SetClipboardData(CF_TEXT, memory))
        {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }

        CloseClipboard();
        return true;
    }


    static HRESULT STDMETHODCALLTYPE HookedPresent(
        IDirect3DDevice9* self,
        const RECT* pSourceRect,
        const RECT* pDestRect,
        HWND hDestWindowOverride,
        const RGNDATA* pDirtyRegion)
    {
        static unsigned long long v7PresentCalls = 0;
        ++v7PresentCalls;
        if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0)
        {
            DxvkPathTrace("V12 PRESENT ENTER call=%llu device=%p frame=%llu frameBegun=%d running=%d",
                v7PresentCalls, self, (unsigned long long)g_presentFrameNumber,
                g_xrFrameBegun ? 1 : 0, g_xrSessionRunning ? 1 : 0);
            const HRESULT coop = self->TestCooperativeLevel();
            DxvkPathTrace("V12 DEVICE TestCooperativeLevel call=%llu hr=0x%08X",
                v7PresentCalls, (unsigned)coop);
        }
        if (v7PresentCalls <= 5 || (v7PresentCalls % 300ULL) == 0)
            DxvkPathTrace("V9 HookedPresent HIT call=%llu device=%p xrInitialized=%d xrSession=%p",
                v7PresentCalls, self, g_xrInitialized ? 1 : 0, g_xrSession);
        if (v7PresentCalls <= 5 || (v7PresentCalls % 300ULL) == 0)
        {
            char v7line[384] = {};
            sprintf_s(v7line, sizeof(v7line),
                "DXVK-V7: HookedPresent HIT call=%llu device=%p xrInitialized=%d xrSession=%p\n",
                v7PresentCalls, self, g_xrInitialized ? 1 : 0, g_xrSession);
            DebugLog(v7line);
        }

        const bool v13FineTrace = (v7PresentCalls <= 20 || (v7PresentCalls % 30ULL) == 0);
        const unsigned long long completedFrame = g_presentFrameNumber;
++g_presentFrameNumber;
        static bool loggedPresent = false;

        if (!loggedPresent)
        {
            DebugLog("DEVICE PRESENT CALLED\n");
            DebugLog("ATTEMPTING NATIVE SAME-FRAME UE3 STEREO SBS\n");
            loggedPresent = true;
        }

        // Poll numpad debug-camera controls once per rendered frame.
        if (g_debugToolsEnabled) UpdateUiCandidateSelectorInput();

        static unsigned int timingLogs = 0;
        if (g_xrFrameBegun && g_xrLocateQpcValid && timingLogs < 30)
        {
            LARGE_INTEGER now{}, freq{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const double ageMs =
                1000.0 * double(now.QuadPart - g_xrLocateQpc.QuadPart) /
                double(freq.QuadPart);

            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "XR PRE-RENDER TIMING frame=%llu locate->Present=%.3fms predictedTime=%lld\n",
                g_presentFrameNumber, ageMs, (long long)g_xrPredictedDisplayTime);
            DebugLog(line);
            ++timingLogs;
        }

        if (v13FineTrace) DxvkPathTrace("V13 PRESENT stage=F stereo ENTER call=%llu alternating=%d",
            v7PresentCalls, g_alternatingStereoEnabled ? 1 : 0);

        // Snapshot the eye UE3 actually rendered BEFORE the AFR capture helper
        // advances/touches eye state.
        const bool afrRenderedRight =
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            g_renderRightEye;

        if (g_alternatingStereoEnabled)
            CaptureAndDrawAlternatingStereoSBS(self);
        else
            DrawDebugSBS(self);
        if (v13FineTrace) DxvkPathTrace(
            "V13 PRESENT stage=G stereo DONE call=%llu afrRenderedRight=%d",
            v7PresentCalls, afrRenderedRight ? 1 : 0);

        // UI_STALE_TEST: framebuffer fallback deliberately disabled.
        // do not copy the completed g_nativeVrColorSurface into g_uiFinalTexture
        // because the actual UI redirect path captured UI during this frame.
        // if there was no UI draw, SubmitVulkanOpenXREyes receives no UI texture
        // and its existing no-fresh-UI path clears the OpenXR UI image transparent.
        if (v13FineTrace)
            DxvkPathTrace("UI_STALE_TEST framebuffer fallback disabled call=%llu extractValid=%d",
                v7PresentCalls, g_uiExtractValid ? 1 : 0);

        // the retained left/right UE3 renders now exist. Copy them through
        // D3D9On12 into OpenXR swapchain images and submit a projection layer.
        if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0 || v7PresentCalls >= 840)
            DxvkPathTrace("V13 PRESENT stage=J before SubmitOpenXRGameEyes call=%llu", v7PresentCalls);
        IDirect3DTexture9* spectatorUiForDesktop = nullptr;
        if (IsVulkanBackendActive())
        {
            // Same immutable renderer-owned pixel pose policy as the proven
            // D3D12 SubmitOpenXRGameEyes path.
            const unsigned long long vkPixelPose = (unsigned long long)
                InterlockedCompareExchange64(&g_capturedStereoPoseSerial,0,0);
            const unsigned long long vkOwnedPose = (unsigned long long)
                InterlockedCompareExchange64(&g_rendererBoundPoseSerial,0,0);
            const bool vkOwnedValid =
                InterlockedCompareExchange(&g_rendererBoundViewsValid,0,0) != 0 &&
                vkOwnedPose == vkPixelPose && vkPixelPose != 0;
            const RenderPoseHistorySlot& vkHistory =
                g_renderPoseHistory[vkPixelPose & 63ULL];
            const bool vkHistoryValid =
                vkPixelPose != 0 && vkHistory.serial == vkPixelPose;
            const XrView* vkSubmitViews =
                vkOwnedValid ? g_rendererBoundViews :
                (vkHistoryValid ? vkHistory.views :
                    (g_renderPoseSnapshotValid ? g_renderPoseSnapshotViews : g_xrViews));

            IDirect3DTexture9* uiForSubmit =
                g_uiExtractValid ? g_uiFinalTexture : nullptr;

            if (!g_nativeStereoEnabled && g_alternatingStereoEnabled)
            {
                // LEFT AFR phase is the canonical mono HUD. Copy it into a
                // persistent texture. RIGHT AFR phase may redraw g_uiFinalTexture
                // with the HUD shifted into the right SBS half, so ignore that
                // texture and keep submitting the retained LEFT-phase image.
                if (!afrRenderedRight &&
                    g_uiExtractValid &&
                    g_uiFinalSurface &&
                    g_afrRetainedUiSurface &&
                    g_afrRetainedUiTexture)
                {
                    const HRESULT retainHr = self->StretchRect(
                        g_uiFinalSurface, nullptr,
                        g_afrRetainedUiSurface, nullptr,
                        D3DTEXF_NONE);
                    if (SUCCEEDED(retainHr))
                        g_afrRetainedUiValid = true;

                    if (v13FineTrace)
                        DxvkPathTrace(
                            "AFR_UI retain refresh call=%llu hr=0x%08X valid=%d",
                            v7PresentCalls,
                            (unsigned)retainHr,
                            g_afrRetainedUiValid ? 1 : 0);
                }

                if (g_afrRetainedUiValid && g_afrRetainedUiTexture)
                    uiForSubmit = g_afrRetainedUiTexture;
            }

            spectatorUiForDesktop = uiForSubmit;

            const bool vkSubmitted = SubmitVulkanOpenXREyes(
                g_leftEyeCopy, g_rightEyeCopy,
                g_stereoCopyWidth, g_stereoCopyHeight,
                vkSubmitViews,
                uiForSubmit,
                g_theaterMode,
                g_vrMenuOpen,
                g_nativeStereoEnabled,
                g_fpV1Enabled && g_avV29ProbeEnabled,
                g_autoTheaterCutscenes,
                g_overrideLockedGameplayCameras,
                g_disablePlayerFade,
                g_rightHandHookshot,
                g_umbrellaMotionControls,
                g_playStationControllerIcons,
                g_nintendoSwitchControllerIcons,
                g_hatVrHudScale,
                g_hatVrHudDistance,
                g_hatVrHudHeight,
                g_hatVrHudHeadLocked,
                g_spectatorView,
                g_spectatorUiMode,
                g_vrMenuPage,
                g_vrMenuSelection,
                g_vrMenuInsideCategory,
                g_selectedUiCandidate,
                g_uiCandidateCount,
                (g_selectedUiCandidate >= 0 && g_selectedUiCandidate < static_cast<int>(g_uiCandidateCount)) ? g_uiCandidates[g_selectedUiCandidate].hash : 0ULL,
                (g_selectedUiCandidate >= 0 && g_selectedUiCandidate < static_cast<int>(g_uiCandidateCount)) ? g_uiCandidates[g_selectedUiCandidate].hits : 0ULL,
                g_uiCandidatePreviewMode,
                (g_selectedUiCandidate >= 0 && g_selectedUiCandidate < static_cast<int>(g_uiCandidateCount)) ?
                    (g_uiCandidates[g_selectedUiCandidate].markedUi ? 2 : (g_uiCandidates[g_selectedUiCandidate].markedGame ? 1 : 0)) : 0);

            g_xrFrameBegun = false;

            if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0)
                DxvkPathTrace(
                    "VKXR-V16T PRESENT submit=%d call=%llu haveLR=%d%d size=%ux%u pixelPose=%llu owned=%d hist=%d ui=%d",
                    vkSubmitted ? 1 : 0, v7PresentCalls,
                    g_leftEyeCopy ? 1 : 0, g_rightEyeCopy ? 1 : 0,
                    g_stereoCopyWidth, g_stereoCopyHeight,
                    vkPixelPose, vkOwnedValid ? 1 : 0,
                    vkHistoryValid ? 1 : 0,
                    uiForSubmit ? 1 : 0);
        }
        else
        {
            SubmitOpenXRGameEyes();
        }

        // during the frame we just submitted.  Consume that validity exactly once.
        // Without this reset, a following frame with zero UI draws inherits the
        // previous frame's true flag and resubmits g_uiFinalTexture as if it were
        // fresh, preventing the Vulkan UI path from taking its transparent-clear
        // fallback.  The next redirected UI draw will set this true again.
        if (g_uiExtractValid)
        {
            if (v13FineTrace)
                DxvkPathTrace(
                    "UI_STALE_TEST consumed extracted UI at Present call=%llu; next frame starts invalid",
                    v7PresentCalls);
            g_uiExtractValid = false;
        }

        // OpenXR submission is complete. The desktop spectator compositor
        // now writes only to the ordinary game backbuffer that downstream Present
        // will show; it never modifies the eye textures or OpenXR swapchains.
        DrawDesktopSpectator(self, g_spectatorView, g_spectatorUiMode, spectatorUiForDesktop);

        if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0 || v7PresentCalls >= 840)
            DxvkPathTrace("V13 PRESENT stage=K after SubmitOpenXRGameEyes call=%llu", v7PresentCalls);

        if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0)
            DxvkPathTrace("V12 PRESENT before downstream Present call=%llu", v7PresentCalls);
        HRESULT hr = g_originalPresent(
            self,
            pSourceRect,
            pDestRect,
            hDestWindowOverride,
            pDirtyRegion
        );
        if (v7PresentCalls <= 12 || (v7PresentCalls % 120ULL) == 0 || FAILED(hr))
            DxvkPathTrace("V12 PRESENT downstream RETURN call=%llu hr=0x%08X",
                v7PresentCalls, (unsigned)hr);

        return hr;
    }

