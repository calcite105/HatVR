
    struct ShaderStats
    {
        IDirect3DVertexShader9* shader;
        unsigned long long binds;
        unsigned long long draws;
        unsigned long long constantWrites;
        unsigned long long c0Writes;
        unsigned long long c6Writes;
        unsigned long long c10Writes;
        unsigned long long c231Writes;
        int candidateRank;
    };

    static constexpr int kMaxShaderStats = 256;
    static ShaderStats g_shaderStats[kMaxShaderStats] = {};
    static int g_shaderStatsCount = 0;
    static CRITICAL_SECTION g_shaderStatsLock;
    static INIT_ONCE g_shaderStatsInitOnce = INIT_ONCE_STATIC_INIT;

    static BOOL CALLBACK InitShaderStatsLock(PINIT_ONCE, PVOID, PVOID*)
    {
        InitializeCriticalSection(&g_shaderStatsLock);
        return TRUE;
    }

    static ShaderStats* FindOrCreateShaderStats(IDirect3DVertexShader9* shader)
    {
        for (int i = 0; i < g_shaderStatsCount; ++i)
        {
            if (g_shaderStats[i].shader == shader)
                return &g_shaderStats[i];
        }

        if (g_shaderStatsCount >= kMaxShaderStats)
            return nullptr;

        ShaderStats* stats = &g_shaderStats[g_shaderStatsCount++];
        *stats = {};
        stats->shader = shader;
        stats->candidateRank = -1;
        return stats;
    }

    struct UiAlphaBlendState
    {
        bool valid = false;
        DWORD colorWrite = 0;
        DWORD separateAlpha = FALSE;
        DWORD srcAlpha = D3DBLEND_ONE;
        DWORD dstAlpha = D3DBLEND_ZERO;
        DWORD opAlpha = D3DBLENDOP_ADD;
    };

    static DWORD NormalizeAlphaBlendFactor(DWORD factor)
    {
        if (factor == D3DBLEND_BOTHSRCALPHA) return D3DBLEND_SRCALPHA;
        if (factor == D3DBLEND_BOTHINVSRCALPHA) return D3DBLEND_INVSRCALPHA;
        return factor;
    }

    static void BeginMirroredUiAlpha(
        IDirect3DDevice9* device, UiAlphaBlendState& saved)
    {
        if (!device) return;

        saved.valid =
            SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &saved.colorWrite)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &saved.separateAlpha)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_SRCBLENDALPHA, &saved.srcAlpha)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_DESTBLENDALPHA, &saved.dstAlpha)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_BLENDOPALPHA, &saved.opAlpha));
        if (!saved.valid) return;

        // Build coverage alpha independently from UE3's RGB blend equation.
        //
        // the old path mirrored SRCBLEND/DESTBLEND into the alpha channel.
        // for the common SRCALPHA / INVSRCALPHA color blend, a transparent
        // destination therefore produced:
        //
        //     Aout = As * As
        //
        // so a 50% translucent widget became 25% alpha in the OpenXR layer.
        //
        // What the standalone compositor texture needs is normal source-over
        // coverage accumulation:
        //
        //     Aout = As + Ad * (1 - As)
        //
        // D3D9 evaluates the source alpha term from the source pixel's alpha,
        // so ONE / INVSRCALPHA gives exactly that while leaving RGB blending
        // completely untouched.
        device->SetRenderState(
            D3DRS_COLORWRITEENABLE,
            saved.colorWrite | D3DCOLORWRITEENABLE_ALPHA);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }

    static void EndMirroredUiAlpha(
        IDirect3DDevice9* device, const UiAlphaBlendState& saved)
    {
        if (!device || !saved.valid) return;
        device->SetRenderState(D3DRS_BLENDOPALPHA, saved.opAlpha);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, saved.dstAlpha);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, saved.srcAlpha);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, saved.separateAlpha);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, saved.colorWrite);
    }

    static bool BeginScopedUiD3DRedirect(
        IDirect3DDevice9* self,
        IDirect3DSurface9** oldColor,
        IDirect3DSurface9** oldDepth,
        D3DVIEWPORT9* oldVp,
        bool redirectRequested);
    static void EndScopedUiD3DRedirect(
        IDirect3DDevice9* self,
        IDirect3DSurface9* oldColor,
        IDirect3DSurface9* oldDepth,
        const D3DVIEWPORT9& oldVp,
        HRESULT hr);

    static bool IsUiCandidateSelected(int candidateIndex)
    {
        // Candidate marks supplement the broad Canvas scope. Draws originating
        // from 0x760236 are UI by default again; explicit Game/SBS marks remain
        // exceptions, while marked UI candidates can also opt in from outside it.
        if (candidateIndex < 0 || candidateIndex >= static_cast<int>(g_uiCandidateCount)) return false;

        // this recurring draw keeps the OpenXR UI from going stale/black after level transitions.
        // it did not produce anything visible in testing.
        if (g_uiCandidates[candidateIndex].hash == 0xC8D52F2D0E25F48AULL)
            return true;

        if (candidateIndex == g_selectedUiCandidate && g_uiCandidatePreviewMode != 0)
            return IsUiCandidatePreviewUi(candidateIndex);
        return g_uiCandidates[candidateIndex].markedUi;
    }

    // Family B is authored across the full native-Stereo SBS viewport.
    // for the detached mono UI layer, render that entire family into one eye-sized
    // viewport, then replay the exact same draw into the other eye-sized viewport.
    static bool IsUiCandidateFamilyB(int candidateIndex)
    {
        return candidateIndex >= 0 &&
            candidateIndex < static_cast<int>(g_uiCandidateCount) &&
            g_uiCandidates[candidateIndex].hash == 0x08FBD6C0B1404765ULL;
    }

    static bool IsUiCandidateGameFramebuffer(int candidateIndex)
    {
        if (candidateIndex < 0 || candidateIndex >= static_cast<int>(g_uiCandidateCount)) return false;
        if (candidateIndex == g_selectedUiCandidate && g_uiCandidatePreviewMode != 0)
            return IsUiCandidatePreviewGame(candidateIndex);
        return g_uiCandidates[candidateIndex].markedGame;
    }

    static bool IsUiCandidateHidden(int candidateIndex)
    {
        (void)candidateIndex;
        return false; // Insert now previews routing instead of hiding the draw.
    }

    static HRESULT STDMETHODCALLTYPE HookedHudDrawPrimitive(
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
    {
        HudWindowRecordD3D("DP", self, type, primitiveCount);
        const bool insideUiScope =
            InterlockedCompareExchange(&g_insideRedirectedUiFlush, 0, 0) != 0;
        const int candidateIndex =
            IdentifyUiDrawCandidate(self, 1, 0, 0, primitiveCount);
        const int gameCandidateIndex = insideUiScope
            ? IdentifyRedirectedGameDrawCandidate(self, 1, 0, 0, primitiveCount) : -1;
        if (IsUiCandidateHidden(candidateIndex))
            return D3D_OK;

        IDirect3DSurface9* oldColor = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        D3DVIEWPORT9 oldVp{};
        const bool redirected = BeginScopedUiD3DRedirect(
            self, &oldColor, &oldDepth, &oldVp,
            !IsRedirectedGameCandidateMarked(gameCandidateIndex) &&
                !IsRedirectedGameCandidatePreviewGame(gameCandidateIndex) &&
                !IsUiCandidateGameFramebuffer(candidateIndex) &&
                !IsUiCandidateFamilyB(candidateIndex) &&
                (insideUiScope || IsUiCandidateSelected(candidateIndex)));
        UiAlphaBlendState alphaState{};
        if (redirected) BeginMirroredUiAlpha(self, alphaState);
        const bool familyBMono = !redirected && IsUiCandidateFamilyB(candidateIndex) && g_nativeVrWidth >= 2;
        D3DVIEWPORT9 familyBVp{};
        if (familyBMono)
        {
            self->GetViewport(&familyBVp);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
        }
        HRESULT hr = g_originalHudDrawPrimitive(self, type, startVertex, primitiveCount);
        if (familyBMono && SUCCEEDED(hr))
        {
            familyBVp.X = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
            hr = g_originalHudDrawPrimitive(self, type, startVertex, primitiveCount);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth;
            self->SetViewport(&familyBVp);
        }
        if (redirected)
        {
            EndMirroredUiAlpha(self, alphaState);
            EndScopedUiD3DRedirect(self, oldColor, oldDepth, oldVp, hr);
        }
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE HookedHudDrawPrimitiveUP(
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride)
    {
        HudWindowRecordD3D("DPUP", self, type, primitiveCount, stride);
        const bool insideUiScope =
            InterlockedCompareExchange(&g_insideRedirectedUiFlush, 0, 0) != 0;
        const int candidateIndex =
            IdentifyUiDrawCandidate(self, 3, stride, 0, primitiveCount);
        const int gameCandidateIndex = insideUiScope
            ? IdentifyRedirectedGameDrawCandidate(self, 3, stride, 0, primitiveCount) : -1;
        if (IsUiCandidateHidden(candidateIndex))
            return D3D_OK;

        IDirect3DSurface9* oldColor = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        D3DVIEWPORT9 oldVp{};
        const bool redirected = BeginScopedUiD3DRedirect(
            self, &oldColor, &oldDepth, &oldVp,
            !IsRedirectedGameCandidateMarked(gameCandidateIndex) &&
                !IsRedirectedGameCandidatePreviewGame(gameCandidateIndex) &&
                !IsUiCandidateGameFramebuffer(candidateIndex) &&
                !IsUiCandidateFamilyB(candidateIndex) &&
                (insideUiScope || IsUiCandidateSelected(candidateIndex)));
        UiAlphaBlendState alphaState{};
        if (redirected) BeginMirroredUiAlpha(self, alphaState);
        const bool familyBMono = !redirected && IsUiCandidateFamilyB(candidateIndex) && g_nativeVrWidth >= 2;
        D3DVIEWPORT9 familyBVp{};
        if (familyBMono)
        {
            self->GetViewport(&familyBVp);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
        }
        HRESULT hr = g_originalHudDrawPrimitiveUP(self, type, primitiveCount, data, stride);
        if (familyBMono && SUCCEEDED(hr))
        {
            familyBVp.X = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
            hr = g_originalHudDrawPrimitiveUP(self, type, primitiveCount, data, stride);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth;
            self->SetViewport(&familyBVp);
        }
        if (redirected)
        {
            EndMirroredUiAlpha(self, alphaState);
            EndScopedUiD3DRedirect(self, oldColor, oldDepth, oldVp, hr);
        }
        return hr;
    }

    static unsigned int g_globalDipupCallerLogCount = 0;

    static void GlobalDipupCallerLog(
        IDirect3DDevice9* self,
        uintptr_t callerRva,
        D3DPRIMITIVETYPE type,
        UINT numVertices,
        UINT primitiveCount,
        D3DFORMAT indexFormat,
        UINT stride)
    {
        // the previous census proved that IDirect3DDevice9::DIPUP is always reached
        // through the generic UE3 wrapper at RVA 0xA1E1A6. The useful discriminator
        // is the caller ABOVE that wrapper. Focus on the known Canvas/UI-like
        // stride-48 quad path so stride-12 geometry cannot exhaust the log cap.
        if (stride != 48 || g_globalDipupCallerLogCount >= 128)
            return;

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);

        void* stack[12] = {};
        const USHORT frames = RtlCaptureStackBackTrace(0, 12, stack, nullptr);

        D3DVIEWPORT9 vp{};
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DBaseTexture9* tex0 = nullptr;
        if (self)
        {
            self->GetViewport(&vp);
            self->GetVertexShader(&vs);
            self->GetTexture(0, &tex0);
        }

        char stackText[512] = {};
        size_t used = 0;
        for (USHORT i = 0; i < frames && i < 10; ++i)
        {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(stack[i]);
            char one[64] = {};
            if (exe && addr >= base)
                sprintf_s(one, sizeof(one), "%s0x%llX", i ? "," : "",
                    static_cast<unsigned long long>(addr - base));
            else
                sprintf_s(one, sizeof(one), "%s%p", i ? "," : "", stack[i]);

            const size_t n = strlen(one);
            if (used + n + 1 >= sizeof(stackText))
                break;
            memcpy(stackText + used, one, n);
            used += n;
            stackText[used] = 0;
        }

        char line[1024] = {};
        sprintf_s(
            line, sizeof(line),
            "DIPUP48 hit=%u directCaller=0x%llX verts=%u prims=%u indexFmt=%u "
            "VS=%p Tex0=%p VP=%lu,%lu %lux%lu hudFlag=%ld hudSerial=%llu stack=[%s]\n",
            g_globalDipupCallerLogCount,
            static_cast<unsigned long long>(callerRva),
            numVertices,
            primitiveCount,
            static_cast<unsigned>(indexFormat),
            vs,
            tex0,
            static_cast<unsigned long>(vp.X),
            static_cast<unsigned long>(vp.Y),
            static_cast<unsigned long>(vp.Width),
            static_cast<unsigned long>(vp.Height),
            InterlockedCompareExchange(&g_insideHudWindow, 0, 0),
            g_hudWindowSerial,
            stackText);

        HudWindowLog(line);
        ++g_globalDipupCallerLogCount;

        if (tex0) tex0->Release();
        if (vs) vs->Release();
    }

    static HRESULT DispatchDrawIndexedPrimitiveUP(
        DrawIndexedPrimitiveUPFn original,
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT numVertices,
        UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride)
    {
        if (!original)
            return D3DERR_INVALIDCALL;

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t callerRva = (exe && ret >= base) ? ret - base : 0;
        GlobalDipupCallerLog(self, callerRva, type, numVertices, primitiveCount, indexFormat, stride);

        HudWindowRecordD3D("DIPUP", self, type, primitiveCount, stride);
        const bool insideUiScope =
            InterlockedCompareExchange(&g_insideRedirectedUiFlush, 0, 0) != 0;
        // Broad Canvas routing restored: 0x760236 membership is the default UI
        // path again. Candidate marks remain useful for explicit exceptions and
        // for UI draws discovered outside this scope.
        const int candidateIndex = IdentifyUiDrawCandidate(
            self, 4, stride, numVertices, primitiveCount);
        const int gameCandidateIndex = insideUiScope
            ? IdentifyRedirectedGameDrawCandidate(self, 4, stride, numVertices, primitiveCount) : -1;

        if (IsUiCandidateHidden(candidateIndex))
            return D3D_OK;

        const bool selectedCandidate = IsUiCandidateSelected(candidateIndex);
        const bool forceGameFramebuffer =
            IsRedirectedGameCandidateMarked(gameCandidateIndex) ||
            IsRedirectedGameCandidatePreviewGame(gameCandidateIndex) ||
            IsUiCandidateGameFramebuffer(candidateIndex) ||
            IsUiCandidateFamilyB(candidateIndex);
        const bool uiDraw =
            !forceGameFramebuffer &&
            (insideUiScope || selectedCandidate) &&
            g_nativeVrColorSurface && self;

        if (!uiDraw)
        {
            if (IsUiCandidateFamilyB(candidateIndex) && g_nativeVrWidth >= 2 && self)
            {
                D3DVIEWPORT9 originalVp{};
                if (SUCCEEDED(self->GetViewport(&originalVp)))
                {
                    D3DVIEWPORT9 eyeVp = originalVp;
                    eyeVp.X = 0;
                    eyeVp.Width = g_nativeVrWidth / 2;
                    self->SetViewport(&eyeVp);
                    HRESULT hr = original(
                        self, type, minVertexIndex, numVertices, primitiveCount,
                        indexData, indexFormat, vertexData, stride);
                    if (SUCCEEDED(hr))
                    {
                        eyeVp.X = g_nativeVrWidth / 2;
                        self->SetViewport(&eyeVp);
                        hr = original(
                            self, type, minVertexIndex, numVertices, primitiveCount,
                            indexData, indexFormat, vertexData, stride);
                    }
                    self->SetViewport(&originalVp);
                    return hr;
                }
            }
            return original(
                self, type, minVertexIndex, numVertices, primitiveCount,
                indexData, indexFormat, vertexData, stride);
        }

        // r188c: Native Stereo's shared Canvas pass emits a full-SBS 1x1-texture
        // overwrite translated by one eye width.  When HatVR extracts that pass
        // into its transparent OpenXR UI target, clipping turns the overwrite
        // into the opaque black right half.  r188 proved that sending the draw
        // back to UE3 merely moves the black rectangle into the world view.
        //
        // r187 already demonstrated that the normal game view is correct while
        // this draw is redirected away from UE3, so for HatVR's detached UI
        // composition this exact housekeeping draw should contribute nowhere:
        // neither to the world target nor to the transparent UI texture.
        if (g_nativeStereoEnabled && stride == 48 && vertexData &&
            numVertices >= 4 && primitiveCount == 2 &&
            g_nativeVrWidth >= 2 && g_nativeVrHeight > 0)
        {
            IDirect3DBaseTexture9* r188cTex0 = nullptr;
            UINT r188cTexW = 0, r188cTexH = 0;
            DWORD r188cAlphaBlend = TRUE;

            self->GetRenderState(D3DRS_ALPHABLENDENABLE, &r188cAlphaBlend);
            if (SUCCEEDED(self->GetTexture(0, &r188cTex0)) && r188cTex0)
            {
                if (r188cTex0->GetType() == D3DRTYPE_TEXTURE)
                {
                    IDirect3DTexture9* t2d =
                        static_cast<IDirect3DTexture9*>(r188cTex0);
                    D3DSURFACE_DESC td{};
                    if (SUCCEEDED(t2d->GetLevelDesc(0, &td)))
                    {
                        r188cTexW = td.Width;
                        r188cTexH = td.Height;
                    }
                }
                r188cTex0->Release();
            }

            const float* q0 = reinterpret_cast<const float*>(vertexData);
            const float* q1 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride);
            const float* q2 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride * 2);
            const float* q3 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride * 3);

            const float minX =
                (std::min)((std::min)(q0[0], q1[0]), (std::min)(q2[0], q3[0]));
            const float maxX =
                (std::max)((std::max)(q0[0], q1[0]), (std::max)(q2[0], q3[0]));
            const float minY =
                (std::min)((std::min)(q0[1], q1[1]), (std::min)(q2[1], q3[1]));
            const float maxY =
                (std::max)((std::max)(q0[1], q1[1]), (std::max)(q2[1], q3[1]));

            const float eyeW = static_cast<float>(g_nativeVrWidth) * 0.5f;
            const float fullW = static_cast<float>(g_nativeVrWidth);
            const float fullH = static_cast<float>(g_nativeVrHeight);
            const auto nearR188c = [](float a, float b)
            {
                return a >= b - 2.0f && a <= b + 2.0f;
            };

            const bool stereoSharedOverwrite =
                r188cTexW == 1 && r188cTexH == 1 &&
                r188cAlphaBlend == FALSE &&
                nearR188c(minX, eyeW) &&
                nearR188c(maxX, eyeW + fullW) &&
                nearR188c(minY, 0.0f) &&
                nearR188c(maxY, fullH);

            if (stereoSharedOverwrite)
            {
                static unsigned int r188cSkipLogs = 0;
                if (r188cSkipLogs < 16)
                {
                    char line[384] = {};
                    sprintf_s(
                        line, sizeof(line),
                        "R188C_DROP_STEREO_SHARED_OVERWRITE frame=%llu "
                        "bounds=[%.1f %.1f -> %.1f %.1f] tex=%ux%u AB=%lu\n",
                        g_presentFrameNumber,
                        minX, minY, maxX, maxY,
                        r188cTexW, r188cTexH,
                        static_cast<unsigned long>(r188cAlphaBlend));
                    CanvasBatchFlushLog(line);
                    ++r188cSkipLogs;
                }

                return D3D_OK;
            }
        }

        D3DSURFACE_DESC desc{};
        if (FAILED(g_nativeVrColorSurface->GetDesc(&desc)) ||
            !EnsureFinishedUiTargets(self, desc) || !g_uiFinalSurface)
        {
            if (g_uiDipupLogCount < 128)
            {
                char line[256] = {};
                sprintf_s(line, sizeof(line),
                    "UI_DIPUP setup_failed frame=%llu stride=%u verts=%u prims=%u device=%p\n",
                    g_presentFrameNumber, stride, numVertices, primitiveCount, self);
                CanvasBatchFlushLog(line);
                ++g_uiDipupLogCount;
            }
            return original(
                self, type, minVertexIndex, numVertices, primitiveCount,
                indexData, indexFormat, vertexData, stride);
        }

        IDirect3DSurface9* oldColor = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        D3DVIEWPORT9 oldVp{};
        self->GetRenderTarget(0, &oldColor);
        self->GetDepthStencilSurface(&oldDepth);
        self->GetViewport(&oldVp);

        self->SetRenderTarget(0, g_uiFinalSurface);

        // Preserve UE3's active depth/stencil attachment while redirecting
        // Canvas color into HatVR's transparent UI surface.
        //
        // FCanvas PushMaskRegion/PopMaskRegion is a real masked-region system.
        // the mask is established by the Canvas renderer and depends on the
        // renderer's depth/stencil state.  The old HatVR path explicitly
        // detached that surface here, so masked Canvas draws (notably the
        // save-file carousel) lost the mask while their color was redirected.
        //
        // g_uiFinalSurface is created at the native VR target dimensions, so
        // the existing attachment remains compatible with this redirect.
        self->SetDepthStencilSurface(oldDepth);
        D3DVIEWPORT9 uiVp{ 0,0,desc.Width,desc.Height,0.0f,1.0f };
        self->SetViewport(&uiVp);

        if (g_canvasUiClearedFrame != g_presentFrameNumber)
        {
            self->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            g_canvasUiClearedFrame = g_presentFrameNumber;
            g_uiExtractValid = false;
        }

        UiAlphaBlendState alphaState{};
        BeginMirroredUiAlpha(self, alphaState);

        // Gate on the actual redirected Canvas candidate, not insideUiScope.
        // First log the real stride-48 vertex payload on BOTH AFR eyes.  This
        // tells us whether the half-eye offset is baked into vertex X or is
        // introduced later by a shader/transform.
        const void* drawVertexData = vertexData;
        unsigned char* afrUiVertexCopy = nullptr;

        const bool afrRedirectedCanvasDraw =
            uiDraw &&
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            stride == 48 &&
            vertexData &&
            numVertices > 0 &&
            g_nativeVrWidth >= 2;

        if (afrRedirectedCanvasDraw)
        {
            if (g_uiDipupLogCount < 192)
            {
                const float* v0 = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(vertexData));
                const float* v1 = (numVertices > 1)
                    ? reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(vertexData) + stride)
                    : v0;
                const float* v2 = (numVertices > 2)
                    ? reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(vertexData) + stride * 2)
                    : v1;
                const float* v3 = (numVertices > 3)
                    ? reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(vertexData) + stride * 3)
                    : v2;

                char line[640] = {};
                sprintf_s(
                    line, sizeof(line),
                    "AFR_UI_VERTEX_SAMPLE frame=%llu eye=%s uiDraw=1 "
                    "verts=%u prims=%u stride=%u "
                    "v0=[%.3f %.3f %.3f %.3f] "
                    "v1=[%.3f %.3f %.3f %.3f] "
                    "v2=[%.3f %.3f %.3f %.3f] "
                    "v3=[%.3f %.3f %.3f %.3f]\n",
                    g_presentFrameNumber,
                    g_renderRightEye ? "RIGHT" : "LEFT",
                    numVertices,
                    primitiveCount,
                    stride,
                    v0[0], v0[1], v0[2], v0[3],
                    v1[0], v1[1], v1[2], v1[3],
                    v2[0], v2[1], v2[2], v2[3],
                    v3[0], v3[1], v3[2], v3[3]);
                CanvasBatchFlushLog(line);
                ++g_uiDipupLogCount;
            }

            // Experimental correction is deliberately limited to RIGHT AFR.
            // Work on a private copy only; never alter UE3's source memory.
            if (g_renderRightEye)
            {
                const size_t byteCount =
                    static_cast<size_t>(numVertices) * static_cast<size_t>(stride);

                afrUiVertexCopy =
                    static_cast<unsigned char*>(malloc(byteCount));

                if (afrUiVertexCopy)
                {
                    memcpy(afrUiVertexCopy, vertexData, byteCount);

                    const float eyeOriginX =
                        static_cast<float>(g_nativeVrWidth) * 0.5f;

                    // Only apply the correction when the first float looks like
                    // screen-space pixel X.  This avoids corrupting a normalized
                    // or transformed vertex stream while the samples tell us
                    // what the real layout is.
                    unsigned int shifted = 0;
                    for (UINT i = 0; i < numVertices; ++i)
                    {
                        float* x = reinterpret_cast<float*>(
                            afrUiVertexCopy +
                            static_cast<size_t>(i) * stride);

                        if (*x >= eyeOriginX &&
                            *x <= static_cast<float>(g_nativeVrWidth) + 4096.0f)
                        {
                            *x -= eyeOriginX;
                            ++shifted;
                        }
                    }

                    if (shifted > 0)
                    {
                        drawVertexData = afrUiVertexCopy;

                        if (g_uiDipupLogCount < 192)
                        {
                            const float beforeX =
                                *reinterpret_cast<const float*>(vertexData);
                            const float afterX =
                                *reinterpret_cast<const float*>(afrUiVertexCopy);

                            char line[320] = {};
                            sprintf_s(
                                line, sizeof(line),
                                "AFR_UI_VERTEX_APPLY frame=%llu eye=RIGHT "
                                "shifted=%u/%u originX=%.1f firstX=%.3f->%.3f\n",
                                g_presentFrameNumber,
                                shifted,
                                numVertices,
                                eyeOriginX,
                                beforeX,
                                afterX);
                            CanvasBatchFlushLog(line);
                            ++g_uiDipupLogCount;
                        }
                    }
                }
            }
        }

        // AHiT's redirected textured Canvas quads consistently arrive
        // with packed white at vertex +0x28 as 0x00FFFFFF.  On the game's
        // original opaque target that zero alpha is harmless because UE3's
        // Canvas path preserves destination alpha.  HatVR, however, extracts
        // the same draw into a transparent UI surface, so the RGB can render
        // while the extracted layer remains transparent and disappears during
        // OpenXR composition.
        //
        // only repair the matching textured quad family:
        // stride 48, four vertices / two triangles, non-1x1 texture, and only
        // when the packed value is exactly white-with-zero-alpha.  Work on a
        // private copy; never mutate UE3's source vertices.
        unsigned char* r194VertexCopy = nullptr;
        if (g_nativeStereoEnabled &&
            insideUiScope &&
            stride == 48 &&
            vertexData &&
            numVertices == 4 &&
            primitiveCount == 2)
        {
            IDirect3DBaseTexture9* r194Tex0 = nullptr;
            UINT r194TexW = 0, r194TexH = 0;

            if (SUCCEEDED(self->GetTexture(0, &r194Tex0)) && r194Tex0)
            {
                if (r194Tex0->GetType() == D3DRTYPE_TEXTURE)
                {
                    IDirect3DTexture9* t2d =
                        static_cast<IDirect3DTexture9*>(r194Tex0);
                    D3DSURFACE_DESC td{};
                    if (SUCCEEDED(t2d->GetLevelDesc(0, &td)))
                    {
                        r194TexW = td.Width;
                        r194TexH = td.Height;
                    }
                }
                r194Tex0->Release();
            }

            if (r194TexW > 1 || r194TexH > 1)
            {
                const size_t byteCount =
                    static_cast<size_t>(numVertices) * static_cast<size_t>(stride);
                r194VertexCopy = static_cast<unsigned char*>(malloc(byteCount));

                if (r194VertexCopy)
                {
                    memcpy(r194VertexCopy, drawVertexData, byteCount);

                    unsigned int repaired = 0;
                    for (UINT i = 0; i < numVertices; ++i)
                    {
                        DWORD* packed = reinterpret_cast<DWORD*>(
                            r194VertexCopy + static_cast<size_t>(i) * stride + 40);

                        if (*packed == 0x00FFFFFFu)
                        {
                            *packed = 0xFFFFFFFFu;
                            ++repaired;
                        }
                    }

                    if (repaired > 0)
                        drawVertexData = r194VertexCopy;
                    else
                    {
                        free(r194VertexCopy);
                        r194VertexCopy = nullptr;
                    }
                }
            }
        }

        // half-screen/SBS quad seen once per AFR UI phase.  The real small HUD
        // quads are left untouched.  This is intentionally a surgical test:
        // if the alternating black half disappears, this draw is the source.
        bool suppressAfrHalfScreenQuad = false;

        if (!g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            stride == 48 &&
            vertexData &&
            numVertices == 4 &&
            primitiveCount == 2 &&
            g_nativeVrWidth >= 2 &&
            g_nativeVrHeight > 0)
        {
            const float* q0 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData));
            const float* q1 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride);
            const float* q2 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride * 2);
            const float* q3 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData) + stride * 3);

            const float eyeW = static_cast<float>(g_nativeVrWidth) * 0.5f;
            const float fullH = static_cast<float>(g_nativeVrHeight);
            const auto nearf = [](float a, float b, float eps) {
                return (a >= b - eps) && (a <= b + eps);
            };

            const bool fullHeight =
                nearf(q0[1], 0.0f, 2.0f) &&
                nearf(q1[1], 0.0f, 2.0f) &&
                nearf(q2[1], fullH, 2.0f) &&
                nearf(q3[1], fullH, 2.0f);

            // Observed AFR patterns:
            // RIGHT: x = 0 .. eyeW
            // LEFT : x = eyeW .. eyeW + full SBS width (1344 .. 4032 at 2688)
            const bool rightPattern =
                nearf(q0[0], 0.0f, 2.0f) &&
                nearf(q1[0], eyeW, 2.0f) &&
                nearf(q2[0], 0.0f, 2.0f) &&
                nearf(q3[0], eyeW, 2.0f);

            const bool leftPattern =
                nearf(q0[0], eyeW, 2.0f) &&
                nearf(q1[0], eyeW + static_cast<float>(g_nativeVrWidth), 2.0f) &&
                nearf(q2[0], eyeW, 2.0f) &&
                nearf(q3[0], eyeW + static_cast<float>(g_nativeVrWidth), 2.0f);

            // q[3] was consistently 1.0 for this large quad while ordinary
            // HUD batches in the same capture used 0.1, giving us another
            // discriminator against suppressing normal widgets/text.
            const bool largeQuadSignature =
                nearf(q0[3], 1.0f, 0.01f) &&
                nearf(q1[3], 1.0f, 0.01f) &&
                nearf(q2[3], 1.0f, 0.01f) &&
                nearf(q3[3], 1.0f, 0.01f);

            suppressAfrHalfScreenQuad =
                fullHeight && largeQuadSignature &&
                (rightPattern || leftPattern);

            if (suppressAfrHalfScreenQuad && g_uiDipupLogCount < 256)
            {
                char line[384] = {};
                sprintf_s(
                    line, sizeof(line),
                    "AFR_UI_HALFSCREEN_SUPPRESS frame=%llu eye=%s "
                    "x=[%.1f %.1f %.1f %.1f] y=[%.1f %.1f %.1f %.1f] "
                    "pattern=%s\n",
                    g_presentFrameNumber,
                    g_renderRightEye ? "RIGHT" : "LEFT",
                    q0[0], q1[0], q2[0], q3[0],
                    q0[1], q1[1], q2[1], q3[1],
                    rightPattern ? "RIGHT_HALF" : "LEFT_SBS");
                CanvasBatchFlushLog(line);
                ++g_uiDipupLogCount;
            }
        }

        // the Canvas origin transform, and the alternating black quad.
        //
        if (!suppressAfrHalfScreenQuad &&
            uiDraw &&
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            stride == 48 &&
            vertexData &&
            numVertices > 0 &&
            g_uiDipupLogCount < 320)
        {
            D3DVIEWPORT9 vp{};
            RECT scissor{};
            DWORD scissorEnable = FALSE;
            IDirect3DVertexShader9* vs = nullptr;

            self->GetViewport(&vp);
            self->GetScissorRect(&scissor);
            self->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnable);
            self->GetVertexShader(&vs);

            float c0[4] = {};
            float c1[4] = {};
            float c2[4] = {};
            float c3[4] = {};
            float c4[4] = {};
            float c5[4] = {};
            float c6[4] = {};
            float c7[4] = {};

            // the UI path has previously shown useful screen/canvas transforms
            // in the low VS constant registers.  Snapshot 0..7 immediately
            // before the real draw so LEFT/RIGHT can be compared directly.
            self->GetVertexShaderConstantF(0, c0, 1);
            self->GetVertexShaderConstantF(1, c1, 1);
            self->GetVertexShaderConstantF(2, c2, 1);
            self->GetVertexShaderConstantF(3, c3, 1);
            self->GetVertexShaderConstantF(4, c4, 1);
            self->GetVertexShaderConstantF(5, c5, 1);
            self->GetVertexShaderConstantF(6, c6, 1);
            self->GetVertexShaderConstantF(7, c7, 1);

            const float* v0 = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertexData));

            char line[1024] = {};
            sprintf_s(
                line, sizeof(line),
                "AFR_UI_D3D_STATE frame=%llu eye=%s verts=%u prims=%u "
                "v0=(%.3f,%.3f,%.3f,%.3f) "
                "vp=%u,%u %ux%u z=%.3f..%.3f "
                "scissor=%lu [%ld,%ld,%ld,%ld] vs=%p "
                "c0=[%.5f %.5f %.5f %.5f] "
                "c1=[%.5f %.5f %.5f %.5f] "
                "c2=[%.5f %.5f %.5f %.5f] "
                "c3=[%.5f %.5f %.5f %.5f] "
                "c4=[%.5f %.5f %.5f %.5f] "
                "c5=[%.5f %.5f %.5f %.5f] "
                "c6=[%.5f %.5f %.5f %.5f] "
                "c7=[%.5f %.5f %.5f %.5f]\n",
                g_presentFrameNumber,
                g_renderRightEye ? "RIGHT" : "LEFT",
                numVertices, primitiveCount,
                v0[0], v0[1], v0[2], v0[3],
                vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ,
                static_cast<unsigned long>(scissorEnable),
                scissor.left, scissor.top, scissor.right, scissor.bottom,
                vs,
                c0[0], c0[1], c0[2], c0[3],
                c1[0], c1[1], c1[2], c1[3],
                c2[0], c2[1], c2[2], c2[3],
                c3[0], c3[1], c3[2], c3[3],
                c4[0], c4[1], c4[2], c4[3],
                c5[0], c5[1], c5[2], c5[3],
                c6[0], c6[1], c6[2], c6[3],
                c7[0], c7[1], c7[2], c7[3]);
            CanvasBatchFlushLog(line);
            ++g_uiDipupLogCount;

            if (vs)
                vs->Release();
        }

        // geometry/viewport/scissor/shader state between eyes, but VS c1.w
        // alternates by exactly one eye width:
        //
        //   LEFT  c1.w = eyeWidth + 0.5
        //   RIGHT c1.w = 0.5
        //
        // Normalize only that component for redirected AFR UI draws, perform
        // the draw, then restore the exact original c1 immediately afterward.
        // World rendering and the separately-suppressed black quad are untouched.
        bool patchedAfrUiC1 = false;
        float savedAfrUiC1[4] = {};

        if (!suppressAfrHalfScreenQuad &&
            uiDraw &&
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            stride == 48 &&
            vertexData &&
            numVertices > 0 &&
            g_nativeVrWidth >= 2)
        {
            if (SUCCEEDED(self->GetVertexShaderConstantF(1, savedAfrUiC1, 1)))
            {
                const float eyeW = static_cast<float>(g_nativeVrWidth) * 0.5f;
                const float monoHalfPixelW = 0.5f;
                const float shiftedHalfPixelW = eyeW + 0.5f;

                // Be deliberately strict: only touch the exact eye-origin
                // shader uses c1.w for something else, leave it alone.
                if (savedAfrUiC1[3] >= shiftedHalfPixelW - 2.0f &&
                    savedAfrUiC1[3] <= shiftedHalfPixelW + 2.0f)
                {
                    float monoC1[4] = {
                        savedAfrUiC1[0],
                        savedAfrUiC1[1],
                        savedAfrUiC1[2],
                        monoHalfPixelW
                    };

                    if (SUCCEEDED(self->SetVertexShaderConstantF(1, monoC1, 1)))
                    {
                        patchedAfrUiC1 = true;

                        if (g_uiDipupLogCount < 384)
                        {
                            char line[320] = {};
                            sprintf_s(
                                line, sizeof(line),
                                "AFR_UI_C1_NORMALIZE frame=%llu eye=%s "
                                "c1=[%.3f %.3f %.3f %.3f]->"
                                "[%.3f %.3f %.3f %.3f] eyeW=%.1f\n",
                                g_presentFrameNumber,
                                g_renderRightEye ? "RIGHT" : "LEFT",
                                savedAfrUiC1[0], savedAfrUiC1[1],
                                savedAfrUiC1[2], savedAfrUiC1[3],
                                monoC1[0], monoC1[1], monoC1[2], monoC1[3],
                                eyeW);
                            CanvasBatchFlushLog(line);
                            ++g_uiDipupLogCount;
                        }
                    }
                }
            }
        }

        const bool familyBMono =
            !suppressAfrHalfScreenQuad &&
            IsUiCandidateFamilyB(candidateIndex) &&
            g_nativeVrWidth >= 2;
        D3DVIEWPORT9 familyBVp{};
        if (familyBMono)
        {
            self->GetViewport(&familyBVp);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
        }

        HRESULT hr = suppressAfrHalfScreenQuad
            ? D3D_OK
            : original(
                self, type, minVertexIndex, numVertices, primitiveCount,
                indexData, indexFormat, drawVertexData, stride);

        if (familyBMono && SUCCEEDED(hr))
        {
            familyBVp.X = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
            hr = original(
                self, type, minVertexIndex, numVertices, primitiveCount,
                indexData, indexFormat, drawVertexData, stride);
        }

        if (patchedAfrUiC1)
            self->SetVertexShaderConstantF(1, savedAfrUiC1, 1);

        if (afrUiVertexCopy)
            free(afrUiVertexCopy);
        if (r194VertexCopy)
            free(r194VertexCopy);

        EndMirroredUiAlpha(self, alphaState);

        if (oldColor) self->SetRenderTarget(0, oldColor);
        self->SetDepthStencilSurface(oldDepth);
        self->SetViewport(&oldVp);

        if (oldDepth) oldDepth->Release();
        if (oldColor) oldColor->Release();

        if (SUCCEEDED(hr))
            g_uiExtractValid = true;

        if (g_uiDipupLogCount < 128)
        {
            char line[320] = {};
            sprintf_s(line, sizeof(line),
                "UI_DIPUP redirected frame=%llu source=%s candidate=%d stride=%u "
                "verts=%u prims=%u drawHR=0x%08X extractValid=%d device=%p\n",
                g_presentFrameNumber,
                insideUiScope ? "760236" : "selector",
                candidateIndex >= 0 ? candidateIndex + 1 : 0,
                stride, numVertices, primitiveCount,
                static_cast<unsigned>(hr), g_uiExtractValid ? 1 : 0, self);
            CanvasBatchFlushLog(line);
            ++g_uiDipupLogCount;
        }

        return hr;
    }

    static HRESULT STDMETHODCALLTYPE HookedHudDrawIndexedPrimitiveUP(
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT numVertices,
        UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride)
    {
        return DispatchDrawIndexedPrimitiveUP(
            g_originalHudDrawIndexedPrimitiveUP,
            self, type, minVertexIndex, numVertices, primitiveCount,
            indexData, indexFormat, vertexData, stride);
    }

    static HRESULT STDMETHODCALLTYPE HookedRHIBackendDrawIndexedPrimitiveUP(
        IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT numVertices,
        UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride)
    {
        return DispatchDrawIndexedPrimitiveUP(
            g_originalRHIBackendDrawIndexedPrimitiveUP,
            self, type, minVertexIndex, numVertices, primitiveCount,
            indexData, indexFormat, vertexData, stride);
    }

    static bool InstallRHIBackendDipupHook(void* target)
    {
        if (!target)
            return false;

        if (g_rhiBackendDipupHookInstalled)
            return target == g_rhiBackendDipupTarget;

        MH_STATUS st = MH_CreateHook(
            target,
            reinterpret_cast<void*>(&HookedRHIBackendDrawIndexedPrimitiveUP),
            reinterpret_cast<void**>(&g_originalRHIBackendDrawIndexedPrimitiveUP));
        if (st == MH_ERROR_ALREADY_CREATED)
        {
            // MinHook hooks a function address, not a COM object.  The ordinary
            // CreateDevice path already installed HookedHudDrawIndexedPrimitiveUP
            // on this same system-d3d9 address, so it covers calls from RHI+0x34
            // as well.  Do not try to create a second trampoline for it.
            g_rhiBackendDipupTarget = target;
            g_rhiBackendDipupCoveredByExistingHook = true;
            CanvasBatchFlushLog(
                "ACTIVE UI: RHI backend slot 84 already covered by existing DIPUP hook.\n");
            return true;
        }

        if (st != MH_OK)
            return false;

        if (!g_originalRHIBackendDrawIndexedPrimitiveUP)
            return false;

        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            return false;

        g_rhiBackendDipupTarget = target;
        g_rhiBackendDipupHookInstalled = true;
        CanvasBatchFlushLog(
            "ACTIVE UI: hooked RHI+0x34 backend slot 84; 0x760236 scope enabled.\n");
        return true;
    }
    static HRESULT STDMETHODCALLTYPE HookedHudSetTexture(
        IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture)
    {
        HudWindowRecordD3D("SetTexture", self, static_cast<D3DPRIMITIVETYPE>(0), 0, 0, stage, texture);
        return g_originalHudSetTexture(self, stage, texture);
    }

    static HRESULT STDMETHODCALLTYPE HookedSetVertexShader(
        IDirect3DDevice9* self,
        IDirect3DVertexShader9* pShader)
    {
        HudWindowRecordD3D("SetVS", self, static_cast<D3DPRIMITIVETYPE>(0), 0, 0, 0, pShader);
        g_currentVertexShader = pShader;

        InitOnceExecuteOnce(
            &g_shaderStatsInitOnce,
            InitShaderStatsLock,
            nullptr,
            nullptr
        );

        EnterCriticalSection(&g_shaderStatsLock);
        ShaderStats* stats = FindOrCreateShaderStats(pShader);
        if (stats)
            ++stats->binds;
        LeaveCriticalSection(&g_shaderStatsLock);

        return g_originalSetVertexShader(self, pShader);
    }
    static float g_playerOcclusionPsConstants[224][4] = {};

    static uint64_t HashPixelShaderBytecode(IDirect3DPixelShader9* shader)
    {
        if (!shader) return 0;
        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || !size) return 0;
        std::vector<unsigned char> bytes(size);
        if (FAILED(shader->GetFunction(bytes.data(), &size))) return 0;
        uint64_t hash = 14695981039346656037ULL;
        for (UINT i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    struct PlayerOcclusionSemanticCacheEntry
    {
        IDirect3DPixelShader9* shader = nullptr;
        int gameStateRegister = -1;
    };

    static std::vector<PlayerOcclusionSemanticCacheEntry> g_playerOcclusionSemanticCache;
    static int g_playerOcclusionCurrentGameStateRegister = -1;

    // D3D9 CTAB records are embedded in the shader bytecode as a comment block.
    // we deliberately parse only the small piece we need instead of depending on
    // D3DX: locate GameStateSetAndAcceptDecals and return its float4 register.
    static int PlayerOcclusionFindGameStateRegister(IDirect3DPixelShader9* shader)
    {
        if (!shader) return -1;

        for (const auto& e : g_playerOcclusionSemanticCache)
            if (e.shader == shader) return e.gameStateRegister;

        int foundRegister = -1;
        UINT byteCount = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &byteCount)) && byteCount >= 32)
        {
            std::vector<unsigned char> bytes(byteCount);
            if (SUCCEEDED(shader->GetFunction(bytes.data(), &byteCount)))
            {
                const unsigned char tag[4] = { 'C', 'T', 'A', 'B' };
                for (UINT pos = 0; pos + 4 + 28 <= byteCount; ++pos)
                {
                    if (memcmp(bytes.data() + pos, tag, 4) != 0) continue;

                    const unsigned char* base = bytes.data() + pos + 4;
                    const size_t remaining = static_cast<size_t>(byteCount) - (pos + 4);
                    auto readU32 = [&](size_t off, DWORD& out) -> bool
                    {
                        if (off + 4 > remaining) return false;
                        memcpy(&out, base + off, 4);
                        return true;
                    };
                    auto readU16 = [&](size_t off, WORD& out) -> bool
                    {
                        if (off + 2 > remaining) return false;
                        memcpy(&out, base + off, 2);
                        return true;
                    };

                    DWORD tableSize=0, constantCount=0, constantInfoOff=0;
                    if (!readU32(0, tableSize) || !readU32(12, constantCount) ||
                        !readU32(16, constantInfoOff))
                        continue;
                    if (tableSize < 28 || constantCount > 1024) continue;

                    // D3DXSHADER_CONSTANTINFO is 20 bytes. All CTAB offsets are
                    // relative to the first byte of the constant-table header.
                    for (DWORD i = 0; i < constantCount; ++i)
                    {
                        const size_t info = static_cast<size_t>(constantInfoOff) +
                                            static_cast<size_t>(i) * 20u;
                        DWORD nameOff=0;
                        WORD registerSet=0, registerIndex=0, registerCount=0;
                        if (!readU32(info + 0, nameOff) ||
                            !readU16(info + 4, registerSet) ||
                            !readU16(info + 6, registerIndex) ||
                            !readU16(info + 8, registerCount))
                            break;
                        if (nameOff >= remaining) continue;

                        const char* name = reinterpret_cast<const char*>(base + nameOff);
                        const size_t maxName = remaining - nameOff;
                        const void* nul = memchr(name, 0, maxName);
                        if (!nul) continue;

                        if (strcmp(name, "GameStateSetAndAcceptDecals") == 0)
                        {
                            // D3DXRS_FLOAT4 == 2. PlayerOcclusion only needs the
                            // first register's .y component.
                            if (registerSet == 2 && registerCount >= 1 && registerIndex < 224)
                                foundRegister = static_cast<int>(registerIndex);
                            break;
                        }
                    }
                    if (foundRegister >= 0) break;
                }
            }
        }

        PlayerOcclusionSemanticCacheEntry entry{};
        entry.shader = shader;
        entry.gameStateRegister = foundRegister;
        g_playerOcclusionSemanticCache.push_back(entry);
        if (foundRegister >= 0)
        {
            const uint64_t hash = HashPixelShaderBytecode(shader);
            LogCategory("FP_OCCLUSION",
                "semantic match PS=%016llX GameStateSetAndAcceptDecals=c%d",
                hash, foundRegister);
        }
        return foundRegister;
    }

    static void PlayerOcclusionForceCurrentSemantic(IDirect3DDevice9* self)
    {
        if (!self || !g_fpV1Enabled || !g_disablePlayerFade || g_hatVrGameCameraActive ||
            g_playerOcclusionCurrentGameStateRegister < 0)
            return;

        const UINT reg = static_cast<UINT>(g_playerOcclusionCurrentGameStateRegister);
        float value[4] = {};
        if (SUCCEEDED(self->GetPixelShaderConstantF(reg, value, 1)))
        {
            value[1] = 10.0f;
            g_originalSetPixelShaderConstantF(self, reg, value, 1);
            memcpy(g_playerOcclusionPsConstants[reg], value, sizeof(value));
        }
    }

    static HRESULT STDMETHODCALLTYPE HookedSetPixelShader(
        IDirect3DDevice9* self, IDirect3DPixelShader9* pShader)
    {
        g_currentPixelShader = pShader;
        g_playerOcclusionCurrentGameStateRegister =
            PlayerOcclusionFindGameStateRegister(pShader);

        const HRESULT hr = g_originalSetPixelShader(self, pShader);
        if (SUCCEEDED(hr)) PlayerOcclusionForceCurrentSemantic(self);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE HookedSetPixelShaderConstantF(
        IDirect3DDevice9* self,
        UINT StartRegister,
        const float* pConstantData,
        UINT Vector4fCount)
    {
        if (pConstantData && StartRegister < 224)
        {
            const UINT count = (Vector4fCount > (224 - StartRegister))
                ? (224 - StartRegister) : Vector4fCount;
            memcpy(&g_playerOcclusionPsConstants[StartRegister][0],
                   pConstantData, static_cast<size_t>(count) * 4u * sizeof(float));
        }

        if (pConstantData && g_fpV1Enabled && g_disablePlayerFade && !g_hatVrGameCameraActive &&
            g_playerOcclusionCurrentGameStateRegister >= 0 && Vector4fCount > 0)
        {
            const UINT semanticReg =
                static_cast<UINT>(g_playerOcclusionCurrentGameStateRegister);
            const UINT endRegister = StartRegister + Vector4fCount;
            if (StartRegister <= semanticReg && endRegister > semanticReg)
            {
                const size_t floatCount = static_cast<size_t>(Vector4fCount) * 4u;
                float* patched = static_cast<float*>(_alloca(floatCount * sizeof(float)));
                memcpy(patched, pConstantData, floatCount * sizeof(float));
                patched[(semanticReg - StartRegister) * 4u + 1u] = 10.0f;

                if (semanticReg < 224)
                    g_playerOcclusionPsConstants[semanticReg][1] = 10.0f;

                return g_originalSetPixelShaderConstantF(
                    self, StartRegister, patched, Vector4fCount);
            }
        }

        return g_originalSetPixelShaderConstantF(self, StartRegister, pConstantData, Vector4fCount);
    }

    static bool BeginScopedUiD3DRedirect(
        IDirect3DDevice9* self,
        IDirect3DSurface9** oldColor,
        IDirect3DSurface9** oldDepth,
        D3DVIEWPORT9* oldVp,
        bool redirectRequested)
    {
        if (!self ||
            !redirectRequested ||
            !g_nativeVrColorSurface)
            return false;

        D3DSURFACE_DESC desc{};
        if (FAILED(g_nativeVrColorSurface->GetDesc(&desc)) ||
            !EnsureFinishedUiTargets(self, desc) || !g_uiFinalSurface)
            return false;

        *oldColor = nullptr; *oldDepth = nullptr; *oldVp = {};
        self->GetRenderTarget(0, oldColor);
        self->GetDepthStencilSurface(oldDepth);
        self->GetViewport(oldVp);

        self->SetRenderTarget(0, g_uiFinalSurface);

        // Preserve UE3's active depth/stencil attachment while redirecting
        // Canvas color into HatVR's transparent UI surface.
        //
        // FCanvas PushMaskRegion/PopMaskRegion is a real masked-region system.
        // the mask is established by the Canvas renderer and depends on the
        // renderer's depth/stencil state.  The old HatVR path explicitly
        // detached that surface here, so masked Canvas draws (notably the
        // save-file carousel) lost the mask while their color was redirected.
        //
        // g_uiFinalSurface is created at the native VR target dimensions, so
        // the existing attachment remains compatible with this redirect.
        self->SetDepthStencilSurface(*oldDepth);
        D3DVIEWPORT9 vp{ 0,0,desc.Width,desc.Height,0.0f,1.0f };
        self->SetViewport(&vp);

        if (g_canvasUiClearedFrame != g_presentFrameNumber)
        {
            self->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            g_canvasUiClearedFrame = g_presentFrameNumber;
            g_uiExtractValid = false;
        }
        return true;
    }

    static void EndScopedUiD3DRedirect(
        IDirect3DDevice9* self,
        IDirect3DSurface9* oldColor,
        IDirect3DSurface9* oldDepth,
        const D3DVIEWPORT9& oldVp,
        HRESULT hr)
    {
        if (oldColor) self->SetRenderTarget(0, oldColor);
        self->SetDepthStencilSurface(oldDepth);
        self->SetViewport(&oldVp);
        if (oldDepth) oldDepth->Release();
        if (oldColor) oldColor->Release();
        if (SUCCEEDED(hr)) g_uiExtractValid = true;
    }

    static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(
        IDirect3DDevice9* self,
        D3DPRIMITIVETYPE Type,
        INT BaseVertexIndex,
        UINT MinVertexIndex,
        UINT NumVertices,
        UINT StartIndex,
        UINT PrimitiveCount)
    {
        HudWindowRecordD3D("DIP", self, Type, PrimitiveCount);


        IDirect3DVertexShader9* current = g_currentVertexShader;

        RecordInspectorDraw(current);

        // bytecode hash, independent of pointer address or draw-count rank.
        ProbeKnownCameraShader(
            self,
            current,
            PrimitiveCount,
            NumVertices
        );

        IDirect3DVertexShader9* selected = nullptr;
        if (!g_shaderInspector.empty() &&
            g_shaderInspectorSelected >= 0 &&
            g_shaderInspectorSelected < static_cast<int>(g_shaderInspector.size()))
        {
            selected = g_shaderInspector[g_shaderInspectorSelected].shader;
        }

        if (selected)
        {
            if (g_shaderDebugMode == ShaderDebugMode::HideSelected &&
                current == selected)
            {
                return D3D_OK;
            }

            if (g_shaderDebugMode == ShaderDebugMode::SoloSelected &&
                current != selected)
            {
                return D3D_OK;
            }
        }

        IDirect3DSurface9* uiOldColor = nullptr;
        IDirect3DSurface9* uiOldDepth = nullptr;
        D3DVIEWPORT9 uiOldVp{};
        const bool insideUiScope =
            InterlockedCompareExchange(&g_insideRedirectedUiFlush, 0, 0) != 0;
        const int candidateIndex =
            IdentifyUiDrawCandidate(self, 2, 0, NumVertices, PrimitiveCount);
        const int gameCandidateIndex = insideUiScope
            ? IdentifyRedirectedGameDrawCandidate(self, 2, 0, NumVertices, PrimitiveCount) : -1;
        if (IsUiCandidateHidden(candidateIndex))
            return D3D_OK;
        const bool uiRedirect = BeginScopedUiD3DRedirect(
            self, &uiOldColor, &uiOldDepth, &uiOldVp,
            !IsRedirectedGameCandidateMarked(gameCandidateIndex) &&
                !IsRedirectedGameCandidateMarked(gameCandidateIndex) &&
                !IsRedirectedGameCandidatePreviewGame(gameCandidateIndex) &&
                !IsUiCandidateGameFramebuffer(candidateIndex) &&
                !IsUiCandidateFamilyB(candidateIndex) &&
                (insideUiScope || IsUiCandidateSelected(candidateIndex)));

        UiAlphaBlendState alphaState{};
        if (uiRedirect) BeginMirroredUiAlpha(self, alphaState);

        const bool familyBMono = !uiRedirect && IsUiCandidateFamilyB(candidateIndex) && g_nativeVrWidth >= 2;
        D3DVIEWPORT9 familyBVp{};
        if (familyBMono)
        {
            self->GetViewport(&familyBVp);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
        }

        HRESULT hr = g_originalDrawIndexedPrimitive(
            self, Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
        if (familyBMono && SUCCEEDED(hr))
        {
            familyBVp.X = g_nativeVrWidth / 2;
            self->SetViewport(&familyBVp);
            hr = g_originalDrawIndexedPrimitive(
                self, Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
            familyBVp.X = 0;
            familyBVp.Width = g_nativeVrWidth;
            self->SetViewport(&familyBVp);
        }

        if (uiRedirect)
        {
            EndMirroredUiAlpha(self, alphaState);
            EndScopedUiD3DRedirect(
                self, uiOldColor, uiOldDepth, uiOldVp, hr);
        }

        return hr;
    }

    // Basic VR-controller support
