    // Central UI logging aliases.  These do not own files or logger state.
    #define HudWindowLog(text) LogCategory("UI", "%s", (text))
    #define CanvasBatchFlushLog(text) LogCategory("UI", "%s", (text))
    #define UiCandidateLog(text) LogCategory("UI", "%s", (text))
    #define CanvasFinalizeLog(text) LogCategory("UI", "%s", (text))

    // Exact Hat_HUD::PostRender D3D census window
    // we do not patch/intercept the known indirect PostRender call at 0x5D93D6.
    // Instead:
    //   0x5D93C5 calls FUN_14011D180
    //   return 0x5D93CA => arm HUD window
    //   Canvas finalize later reaches SHARED phase => close HUD window
    // this intentionally includes the tiny tail after Hat_HUD::PostRender but
    // excludes world rendering before it. All D3D hooks below are passive.
    static constexpr uintptr_t kHudWindowResolverRva = 0x11D560;
    static constexpr uintptr_t kHudWindowResolverReturnRva = 0x5D93CA;

    using HudWindowResolverFn = void* (*)(void*, void*, unsigned int);
    static HudWindowResolverFn g_originalHudWindowResolver = nullptr;
    static bool g_hudWindowResolverInstalled = false;
    static volatile LONG g_insideHudWindow = 0;
    static unsigned long long g_hudWindowSerial = 0;
    static unsigned int g_hudWindowLogLines = 0;
    static void* HookedHudWindowResolver(void* a, void* b, unsigned int c)
    {
        void* result = g_originalHudWindowResolver ? g_originalHudWindowResolver(a, b, c) : nullptr;
        HMODULE exe = GetModuleHandleW(nullptr);
        if (exe && reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(exe) == kHudWindowResolverReturnRva)
        {
            InterlockedExchange(&g_insideHudWindow, 1);
            ++g_hudWindowSerial;
            if (g_hudWindowLogLines < 256) {
                char line[160] = {};
                sprintf_s(line, sizeof(line), "BEGIN serial=%llu returnRVA=0x5D93CA\n", g_hudWindowSerial);
                HudWindowLog(line); ++g_hudWindowLogLines;
            }
        }
        return result;
    }

    static bool InstallHudWindowResolverHook()
    {
        if (g_hudWindowResolverInstalled) return true;
        HMODULE exe = GetModuleHandleW(nullptr); if (!exe) return false;
        void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kHudWindowResolverRva);
        MH_STATUS st = MH_CreateHook(target, &HookedHudWindowResolver, reinterpret_cast<void**>(&g_originalHudWindowResolver));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
        g_hudWindowResolverInstalled = true;
        HudWindowLog("HUD D3D census installed; PostRender 0x5D93D6 remains untouched.\n");
        return true;
    }

    static void HudWindowRecordD3D(
        const char* op, IDirect3DDevice9* device,
        D3DPRIMITIVETYPE primType = static_cast<D3DPRIMITIVETYPE>(0),
        UINT primCount = 0, UINT stride = 0, DWORD stage = 0, void* object = nullptr)
    {
        if (InterlockedCompareExchange(&g_insideHudWindow, 0, 0) == 0 || g_hudWindowLogLines >= 256)
            return;

        D3DVIEWPORT9 vp{};
        IDirect3DVertexShader9* currentVs = nullptr;
        if (device)
        {
            device->GetViewport(&vp);          // passive query only
            device->GetVertexShader(&currentVs); // passive query only
        }

        char line[320] = {};
        sprintf_s(line, sizeof(line),
            "D3D serial=%llu op=%s type=%u prims=%u stride=%u stage=%lu object=%p "
            "VS=%p VP=%lu,%lu %lux%lu\n",
            g_hudWindowSerial, op, (unsigned)primType, primCount, stride,
            (unsigned long)stage, object, currentVs,
            (unsigned long)vp.X, (unsigned long)vp.Y,
            (unsigned long)vp.Width, (unsigned long)vp.Height);

        if (currentVs)
            currentVs->Release();

        HudWindowLog(line); ++g_hudWindowLogLines;
    }

    // Passive Canvas/FCanvas batched-geometry flush census
    // Ghidra: FUN_1407A13E0
    // this is the 48-byte-vertex batched geometry renderer sitting above the
    static constexpr uintptr_t kCanvasBatchFlushRva = 0x7A19C0;

    using CanvasBatchFlushFn = unsigned long long (*)(
        long long* batch,
        long long transformOrContext,
        unsigned int viewportWidth,
        unsigned long long arg4,
        unsigned int arg5,
        unsigned int arg6);

    static CanvasBatchFlushFn g_originalCanvasBatchFlush = nullptr;
    static bool g_canvasBatchFlushHookInstalled = false;
    static unsigned int g_canvasBatchFlushLogCount = 0;

    static bool g_canvasUiRedirectEnabled = true; // active 0x760236 -> backend DIPUP UI separation
    static unsigned long long g_canvasUiClearedFrame = ~0ULL;
    static unsigned int g_canvasUiRedirectLogCount = 0;
    static volatile LONG g_insideRedirectedUiFlush = 0;
    // Interactive out-of-scope UI family classifier.
    //
    // A candidate is keyed only by its main-EXE call stack.  Shader/texture and
    // geometry values are recorded as evidence, but deliberately do not split
    // one caller family into hundreds of individual UI elements.
    static constexpr unsigned int kMaxUiCandidates = 128;
    static constexpr unsigned int kUiCandidateStackDepth = 8;

    struct UiDrawCandidate
    {
        uint64_t hash = 0;
        uintptr_t stackRvas[kUiCandidateStackDepth] = {};
        unsigned int stackCount = 0;
        unsigned long long hits = 0;
        unsigned int lastDrawOp = 0; // 1=DP, 2=DIP, 3=DPUP, 4=DIPUP
        UINT lastStride = 0;
        UINT lastVertices = 0;
        UINT lastPrimitives = 0;
        D3DVIEWPORT9 lastViewport{};
        void* lastVertexShader = nullptr;
        void* lastTexture0 = nullptr;
        bool markedUi = false;
        bool markedGame = false; // hard override: keep this family in the game framebuffer
    };

    static UiDrawCandidate g_uiCandidates[kMaxUiCandidates] = {};
    static unsigned int g_uiCandidateCount = 0;
    static int g_selectedUiCandidate = -1;
    static int g_uiCandidatePreviewMode = 0; // 0=normal, 1=force game/SBS, 2=force OpenXR
    static bool g_f6WasDown = false;
    static bool g_f7WasDown = false;
    static bool g_insertWasDown = false;
    static bool g_deleteWasDown = false;
    static bool g_homeWasDown = false;
    // r232 current-build auto-UI family isolation. Both default ON.
    static bool g_autoUiFamilyAEnabled = true;
    static bool g_numpad1WasDown = false;
    static void* g_knownCanvasUiVertexShader = nullptr;

    // exhaustive GAME-framebuffer census of draws that HatVR is
    // actually about to redirect through the confirmed Canvas UI scope.
    // this intentionally has NO shader/UI plausibility filter.
    static constexpr unsigned int kMaxRedirectedGameCandidates = 512;
    struct RedirectedGameDrawCandidate
    {
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DBaseTexture9* tex0 = nullptr;
        unsigned int drawOp = 0; // 1=DP, 2=DIP, 3=DPUP, 4=DIPUP
        UINT stride = 0;
        UINT vertices = 0;
        UINT primitives = 0;
        D3DVIEWPORT9 viewport{};
        unsigned long long hits = 0;
        bool markedGame = false;
    };
    static RedirectedGameDrawCandidate g_redirectedGameCandidates[kMaxRedirectedGameCandidates] = {};
    static unsigned int g_redirectedGameCandidateCount = 0;
    static int g_selectedRedirectedGameCandidate = -1;

    static int IdentifyRedirectedGameDrawCandidate(
        IDirect3DDevice9* device, unsigned int drawOp,
        UINT stride, UINT vertices, UINT primitives)
    {
        if (!device || InterlockedCompareExchange(&g_insideRedirectedUiFlush, 0, 0) == 0)
            return -1;

        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DBaseTexture9* tex0 = nullptr;
        D3DVIEWPORT9 vp{};
        device->GetVertexShader(&vs);
        device->GetPixelShader(&ps);
        device->GetTexture(0, &tex0);
        device->GetViewport(&vp);

        // Pointer identity is intentional for this interactive per-run census:
        // it distinguishes the actual bound resources without expensive hashing.
        int found = -1;
        for (unsigned int i = 0; i < g_redirectedGameCandidateCount; ++i)
        {
            const auto& c = g_redirectedGameCandidates[i];
            if (c.vs == vs && c.ps == ps && c.tex0 == tex0 &&
                c.drawOp == drawOp && c.stride == stride &&
                c.vertices == vertices && c.primitives == primitives &&
                c.viewport.X == vp.X && c.viewport.Y == vp.Y &&
                c.viewport.Width == vp.Width && c.viewport.Height == vp.Height)
            {
                found = static_cast<int>(i);
                break;
            }
        }

        if (found < 0 && g_redirectedGameCandidateCount < kMaxRedirectedGameCandidates)
        {
            found = static_cast<int>(g_redirectedGameCandidateCount++);
            auto& c = g_redirectedGameCandidates[found];
            c.vs = vs; c.ps = ps; c.tex0 = tex0;
            c.drawOp = drawOp; c.stride = stride;
            c.vertices = vertices; c.primitives = primitives;
            c.viewport = vp;
            if (g_selectedRedirectedGameCandidate < 0)
                g_selectedRedirectedGameCandidate = 0;
        }

        if (found >= 0)
            ++g_redirectedGameCandidates[found].hits;

        if (tex0) tex0->Release();
        if (ps) ps->Release();
        if (vs) vs->Release();
        return found;
    }

    static bool IsRedirectedGameCandidateMarked(int index)
    {
        return index >= 0 && index < static_cast<int>(g_redirectedGameCandidateCount) &&
            g_redirectedGameCandidates[index].markedGame;
    }

    static bool IsRedirectedGameCandidatePreviewGame(int index)
    {
        // INSERT preview is a temporary GAME-framebuffer route.  The draw is
        // still executed; it simply bypasses HatVR's detached OpenXR UI target.
        return false; // legacy redirected-census preview superseded by unified hashed-candidate preview
    }

    static void LogRedirectedGameCandidateState(const char* action, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_redirectedGameCandidateCount))
        {
            char line[160] = {};
            sprintf_s(line, sizeof(line), "%s no redirected Canvas draws discovered yet\n", action);
            UiCandidateLog(line);
            return;
        }
        const auto& c = g_redirectedGameCandidates[index];
        char line[768] = {};
        sprintf_s(line, sizeof(line),
            "%s GAME_DRAW=%d/%u markedGame=%d previewGame=%d hits=%llu "
            "op=%u stride=%u verts=%u prims=%u VP=%lu,%lu %lux%lu VS=%p PS=%p Tex0=%p\n",
            action, index + 1, g_redirectedGameCandidateCount,
            c.markedGame ? 1 : 0, 0,
            c.hits, c.drawOp, c.stride, c.vertices, c.primitives,
            (unsigned long)c.viewport.X, (unsigned long)c.viewport.Y,
            (unsigned long)c.viewport.Width, (unsigned long)c.viewport.Height,
            c.vs, c.ps, c.tex0);
        UiCandidateLog(line);
    }

    static void LogUiCandidateState(const char* action, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_uiCandidateCount))
        {
            char line[128] = {};
            sprintf_s(line, sizeof(line), "%s no candidates discovered yet\n", action);
            UiCandidateLog(line);
            return;
        }

        const UiDrawCandidate& c = g_uiCandidates[index];
        char stackText[384] = {};
        size_t used = 0;
        for (unsigned int i = 0; i < c.stackCount; ++i)
        {
            char one[48] = {};
            sprintf_s(one, sizeof(one), "%s0x%llX", i ? "," : "",
                static_cast<unsigned long long>(c.stackRvas[i]));
            const size_t n = strlen(one);
            if (used + n + 1 >= sizeof(stackText)) break;
            memcpy(stackText + used, one, n);
            used += n;
            stackText[used] = 0;
        }

        char line[1024] = {};
        sprintf_s(line, sizeof(line),
            "%s candidate=%d/%u hash=0x%llX preview=%d markedUi=%d markedGame=%d hits=%llu "
            "op=%u stride=%u verts=%u prims=%u VP=%lu,%lu %lux%lu VS=%p Tex0=%p stack=[%s]\n",
            action, index + 1, g_uiCandidateCount,
            static_cast<unsigned long long>(c.hash),
            g_uiCandidatePreviewMode, c.markedUi ? 1 : 0, c.markedGame ? 1 : 0,
            c.hits, c.lastDrawOp, c.lastStride, c.lastVertices, c.lastPrimitives,
            (unsigned long)c.lastViewport.X, (unsigned long)c.lastViewport.Y,
            (unsigned long)c.lastViewport.Width, (unsigned long)c.lastViewport.Height,
            c.lastVertexShader, c.lastTexture0, stackText);
        UiCandidateLog(line);
    }

    static int IdentifyUiDrawCandidate(
        IDirect3DDevice9* device, unsigned int drawOp,
        UINT stride, UINT vertices, UINT primitives)
    {
        if (!device)
            return -1;

        // UP draws are the normal UE3 Canvas route, but different UI materials
        // can use different shaders and vertex layouts.  Admit modest UP batches
        // broadly.  Keep the exact confirmed-Canvas shader requirement for the
        // regular DP/DIP paths, which otherwise contain most world geometry.
        IDirect3DVertexShader9* currentVs = nullptr;
        if (FAILED(device->GetVertexShader(&currentVs)) || !currentVs)
            return -1;
        const bool shaderMatchesKnownCanvas =
            g_knownCanvasUiVertexShader && currentVs == g_knownCanvasUiVertexShader;
        const bool isUpDraw = drawOp == 3 || drawOp == 4;
        const bool plausibleUpUi =
            isUpDraw && stride >= 12 && stride <= 128 && primitives <= 4096;
        if (!plausibleUpUi && !shaderMatchesKnownCanvas)
        {
            currentVs->Release();
            return -1;
        }
        void* currentVsIdentity = currentVs;
        currentVs->Release();

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return -1;
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
        const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

        void* frames[24] = {};
        const USHORT frameCount = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        uintptr_t rvas[kUiCandidateStackDepth] = {};
        unsigned int rvaCount = 0;
        for (USHORT i = 0; i < frameCount && rvaCount < kUiCandidateStackDepth; ++i)
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
            if (address >= base && address < end)
                rvas[rvaCount++] = address - base;
        }
        if (!rvaCount) return -1;

        uint64_t hash = 14695981039346656037ULL;
        hash ^= static_cast<uint64_t>(drawOp);
        hash *= 1099511628211ULL;
        for (unsigned int i = 0; i < rvaCount; ++i)
        {
            hash ^= static_cast<uint64_t>(rvas[i]);
            hash *= 1099511628211ULL;
        }

        int index = -1;
        for (unsigned int i = 0; i < g_uiCandidateCount; ++i)
        {
            const UiDrawCandidate& c = g_uiCandidates[i];
            if (c.hash == hash && c.stackCount == rvaCount &&
                memcmp(c.stackRvas, rvas, rvaCount * sizeof(uintptr_t)) == 0)
            {
                index = static_cast<int>(i);
                break;
            }
        }

        if (index < 0)
        {
            if (g_uiCandidateCount >= kMaxUiCandidates) return -1;
            index = static_cast<int>(g_uiCandidateCount++);
            UiDrawCandidate& c = g_uiCandidates[index];
            c.hash = hash;
            c.stackCount = rvaCount;
            memcpy(c.stackRvas, rvas, rvaCount * sizeof(uintptr_t));
            // Confirmed interactively in CandidateSelector_v2.log.
            c.markedUi =
                // Pre-hotfix confirmed UI families.
                hash == 0xD2EFE7F4500173ECULL ||
                hash == 0xF00EF310FF49C295ULL ||
                // Steam Build 25577316: additional UI family found during
                // release-candidate UI discovery (candidate 87).
                hash == 0xABB64F60A774AAB8ULL ||
                // Steam Build 25577316 hotfix equivalents of those exact
                // call-stack families.  The game update relocated the EXE
                // return RVAs, so the FNV stack hashes changed even though
                // the underlying UI draw families did not.
                (g_autoUiFamilyAEnabled && hash == 0x0CFC42F8ABB7D59CULL);
            if (g_selectedUiCandidate < 0) g_selectedUiCandidate = 0;
        }

        UiDrawCandidate& c = g_uiCandidates[index];
        ++c.hits;
        c.lastDrawOp = drawOp;
        c.lastStride = stride;
        c.lastVertices = vertices;
        c.lastPrimitives = primitives;
        device->GetViewport(&c.lastViewport);
        IDirect3DBaseTexture9* tex = nullptr;
        c.lastVertexShader = currentVsIdentity;
        if (SUCCEEDED(device->GetTexture(0, &tex)) && tex)
        {
            c.lastTexture0 = tex;
            tex->Release();
        }
        return index;
    }

    static void UiCandidateSelectDelta(int delta)
    {
        if (!g_uiCandidateCount) { g_selectedUiCandidate = -1; return; }
        if (g_selectedUiCandidate < 0) g_selectedUiCandidate = 0;
        g_selectedUiCandidate =
            (g_selectedUiCandidate + delta + static_cast<int>(g_uiCandidateCount)) %
            static_cast<int>(g_uiCandidateCount);
        LogUiCandidateState(delta < 0 ? "UI_PREV" : "UI_NEXT", g_selectedUiCandidate);
    }

    static void UiCandidateCyclePreview()
    {
        g_uiCandidatePreviewMode = (g_uiCandidatePreviewMode + 1) % 3;
        const char* mode = g_uiCandidatePreviewMode == 0 ? "NORMAL" :
                           g_uiCandidatePreviewMode == 1 ? "SBS/GAME" : "OPENXR";
        char line[192] = {};
        sprintf_s(line, sizeof(line), "UI_PREVIEW mode=%s candidate=%d/%u\n",
            mode, g_selectedUiCandidate >= 0 ? g_selectedUiCandidate + 1 : 0, g_uiCandidateCount);
        UiCandidateLog(line);
    }

    static void UiCandidateMarkGame()
    {
        if (g_selectedUiCandidate < 0 || g_selectedUiCandidate >= static_cast<int>(g_uiCandidateCount)) return;
        auto& c = g_uiCandidates[g_selectedUiCandidate];
        c.markedGame = true; c.markedUi = false;
        LogUiCandidateState("MARK_SBS_GAME", g_selectedUiCandidate);
    }

    static void UiCandidateMarkUi()
    {
        if (g_selectedUiCandidate < 0 || g_selectedUiCandidate >= static_cast<int>(g_uiCandidateCount)) return;
        auto& c = g_uiCandidates[g_selectedUiCandidate];
        c.markedUi = true; c.markedGame = false;
        LogUiCandidateState("MARK_OPENXR", g_selectedUiCandidate);
    }

    static void UiCandidateClearMark()
    {
        if (g_selectedUiCandidate < 0 || g_selectedUiCandidate >= static_cast<int>(g_uiCandidateCount)) return;
        auto& c = g_uiCandidates[g_selectedUiCandidate];
        c.markedUi = false; c.markedGame = false;
        LogUiCandidateState("CLEAR_ROUTE", g_selectedUiCandidate);
    }

    static bool IsUiCandidatePreviewGame(int index)
    {
        return g_uiCandidatePreviewMode == 1 && index >= 0 && index == g_selectedUiCandidate;
    }

    static bool IsUiCandidatePreviewUi(int index)
    {
        return g_uiCandidatePreviewMode == 2 && index >= 0 && index == g_selectedUiCandidate;
    }

    static void UpdateUiCandidateSelectorInput()
    {
        if (!g_debugToolsEnabled) return;
        if (g_hatVrGameWindow && GetForegroundWindow() != g_hatVrGameWindow) return;
        const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        const bool deleteDown = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        const bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        const bool numpad1Down = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;

        if (numpad1Down && !g_numpad1WasDown)
        {
            g_autoUiFamilyAEnabled = !g_autoUiFamilyAEnabled;
            for (unsigned int i = 0; i < g_uiCandidateCount; ++i)
                if (g_uiCandidates[i].hash == 0x0CFC42F8ABB7D59CULL)
                    g_uiCandidates[i].markedUi = g_autoUiFamilyAEnabled;
            char line[160] = {};
            sprintf_s(line, sizeof(line), "AUTO_UI_FAMILY_A enabled=%d\n", g_autoUiFamilyAEnabled ? 1 : 0);
            UiCandidateLog(line);
        }

        // Unified release-debug selector. These are the exact hashed candidates
        // consumed by the OpenXR/game-framebuffer router.
        if (g_uiCandidateCount && f6 && !g_f6WasDown) UiCandidateSelectDelta(-1);
        if (g_uiCandidateCount && f7 && !g_f7WasDown) UiCandidateSelectDelta(+1);
        if (insertDown && !g_insertWasDown) UiCandidateCyclePreview();
        if (homeDown && !g_homeWasDown) UiCandidateMarkGame();
        if (deleteDown && !g_deleteWasDown) UiCandidateMarkUi();

        g_f6WasDown = f6; g_f7WasDown = f7;
        g_insertWasDown = insertDown; g_deleteWasDown = deleteDown;
        g_homeWasDown = homeDown;
        g_numpad1WasDown = numpad1Down;
    }

    static unsigned long long HookedCanvasBatchFlush(
        long long* batch,
        long long transformOrContext,
        unsigned int viewportWidth,
        unsigned long long arg4,
        unsigned int arg5,
        unsigned int arg6)
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t returnRva = (exe && ret >= base) ? ret - base : 0;

        int q08 = 0, q18 = 0, q28 = 0, q3c = 0, q4c = 0, qC0 = 0, q1B8 = 0;
        if (batch)
        {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(batch);
            q08 = *reinterpret_cast<const int*>(p + 0x08);
            q18 = *reinterpret_cast<const int*>(p + 0x18);
            q28 = *reinterpret_cast<const int*>(p + 0x28);
            q3c = *reinterpret_cast<const int*>(p + 0x3C);
            q4c = *reinterpret_cast<const int*>(p + 0x4C);
            qC0 = *reinterpret_cast<const int*>(p + 0xC0);
            q1B8 = *reinterpret_cast<const int*>(p + 0x1B8);
        }

        if (batch && g_canvasBatchFlushLogCount < 256)
        {
            char line[512] = {};
            sprintf_s(
                line, sizeof(line),
                "FLUSH hit=%u returnRVA=0x%llX batch=%p ctx=0x%llX param3=%u "
                "arg4=0x%llX arg5=%u arg6=%u "
                "q08=%d q18=%d q28=%d q3C=%d q4C=%d qC0=%d q1B8=%d "
                "hudFlag=%ld hudSerial=%llu\n",
                g_canvasBatchFlushLogCount,
                static_cast<unsigned long long>(returnRva),
                batch,
                static_cast<unsigned long long>(transformOrContext),
                viewportWidth,
                static_cast<unsigned long long>(arg4),
                arg5, arg6, q08, q18, q28, q3c, q4c, qC0, q1B8,
                InterlockedCompareExchange(&g_insideHudWindow, 0, 0),
                g_hudWindowSerial);
            CanvasBatchFlushLog(line);
            ++g_canvasBatchFlushLogCount;
        }

        // First active separation test:
        // ACTIVE TEST #2:
        // Test #1 proved A18874/native/qC0 is predominantly WORLD: redirecting it
        // put the game into the OpenXR panel while the HUD remained in the VR view.
        // the earlier stack census also found a distinct full-canvas branch through
        // caller 0x760236. Redirect ONLY that native-sized branch as the UI candidate.
        // A18874 is deliberately left completely untouched.
        const bool redirect =
            g_canvasUiRedirectEnabled &&
            g_nativeVrColorSurface &&
            g_nativeVrWidth && g_nativeVrHeight &&
            returnRva == 0x760236 &&
            viewportWidth == g_nativeVrWidth &&
            arg4 == static_cast<unsigned long long>(g_nativeVrHeight);

        // Use only state already visible in this module; do not depend on the
        // D3D device global or mutate renderer-owned state.
        if (returnRva == 0x760236 && g_canvasUiRedirectLogCount < 96)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "CANVAS_FLUSH_BOUNDARY frame=%llu hud=%ld serial=%llu canvas=%ux%llu native=%ux%u afr=%d eye=%s redirect=%d\n",
                g_presentFrameNumber,
                InterlockedCompareExchange(&g_insideHudWindow, 0, 0),
                g_hudWindowSerial,
                viewportWidth,
                static_cast<unsigned long long>(arg4),
                g_nativeVrWidth, g_nativeVrHeight,
                (!g_nativeStereoEnabled && g_alternatingStereoEnabled) ? 1 : 0,
                g_renderRightEye ? "RIGHT" : "LEFT",
                redirect ? 1 : 0);
            CanvasBatchFlushLog(line);
            ++g_canvasUiRedirectLogCount;
        }

        if (!redirect || !g_originalCanvasBatchFlush)
            return g_originalCanvasBatchFlush
            ? g_originalCanvasBatchFlush(batch, transformOrContext, viewportWidth, arg4, arg5, arg6)
            : 0;

        // do not switch the RT around the whole high-level flush: the flush performs
        // internal RHI/state work that can replace our target. Mark the exact flush
        // instead; the low-level stride-48 DIPUP hook below redirects each actual
        // draw at the moment it reaches D3D9.
        InterlockedExchange(&g_insideRedirectedUiFlush, 1);
        const unsigned long long result =
            g_originalCanvasBatchFlush(batch, transformOrContext, viewportWidth, arg4, arg5, arg6);
        InterlockedExchange(&g_insideRedirectedUiFlush, 0);

        if (g_canvasUiRedirectLogCount < 96)
        {
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "UI_SCOPE frame=%llu caller=760136 size=%ux%llu qC0=%d result=0x%llX\n",
                g_presentFrameNumber, viewportWidth,
                static_cast<unsigned long long>(arg4), qC0, result);
            CanvasBatchFlushLog(line);
            ++g_canvasUiRedirectLogCount;
        }
        return result;
    }

    static bool InstallCanvasBatchFlushHook()
    {
        if (g_canvasBatchFlushHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
            return false;

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kCanvasBatchFlushRva);

        MH_STATUS st = MH_CreateHook(
            target,
            &HookedCanvasBatchFlush,
            reinterpret_cast<void**>(&g_originalCanvasBatchFlush));

        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
            return false;

        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            return false;

        g_canvasBatchFlushHookInstalled = true;
        CanvasBatchFlushLog(
            "Canvas batch flush census installed at RVA 0x7A19C0; passive only.\n");
        return true;
    }

    static constexpr uintptr_t kCanvasFinalizeCandidateRva = 0x768AD0;
    using CanvasFinalizeCandidateFn = void (*)(void* canvasObject);
    static CanvasFinalizeCandidateFn g_originalCanvasFinalizeCandidate = nullptr;
    static bool g_canvasFinalizeHookInstalled = false;
    static unsigned int g_canvasFinalizeLogCount = 0;

    static void HookedCanvasFinalizeCandidate(void* canvasObject)
    {
        if (!canvasObject)
        {
            g_originalCanvasFinalizeCandidate(canvasObject);
            return;
        }

        auto* c = reinterpret_cast<unsigned char*>(canvasObject);

        // Proven from FUN_1405D82C0:
        // +0x90 and +0x94 are populated immediately before FUN_140768610.
        // +0xA4 is the associated FSceneView for the per-player phase and is
        // explicitly cleared to NULL for the later shared/fullscreen phase.
        auto* sizeXSlot = reinterpret_cast<unsigned int*>(c + 0x90);
        auto* sizeYSlot = reinterpret_cast<unsigned int*>(c + 0x94);
        void* associatedView = *reinterpret_cast<void**>(c + 0xA4);

        const unsigned int savedX = *sizeXSlot;
        const unsigned int savedY = *sizeYSlot;
        bool changed = false;

        // Per-player Canvas logical-width normalization only.
        // the HUD shift, but changing renderer-owned view state breaks AFR.
        if (associatedView &&
            g_nativeVrWidth > 0 &&
            g_nativeVrHeight > 0 &&
            savedX == g_nativeVrWidth / 2 &&
            savedY == g_nativeVrHeight)
        {
            *sizeXSlot = g_nativeVrWidth;
            changed = true;
        }

        if (g_canvasFinalizeLogCount < 32)
        {
            char line[384] = {};
            sprintf_s(
                line, sizeof(line),
                "call=%u canvas=%p view=%p phase=%s size=%ux%u -> %ux%u native=%ux%u changed=%d\n",
                g_canvasFinalizeLogCount,
                canvasObject,
                associatedView,
                associatedView ? "PER_PLAYER" : "SHARED",
                savedX, savedY,
                *sizeXSlot, *sizeYSlot,
                g_nativeVrWidth, g_nativeVrHeight,
                changed ? 1 : 0);
            CanvasFinalizeLog(line);
            ++g_canvasFinalizeLogCount;
        }

        g_originalCanvasFinalizeCandidate(canvasObject);

        // keep this test local to FUN_140768610.  UE3's CanvasObject is restored
        // immediately so later shared-HUD setup sees its normal state.
        if (changed)
        {
            *sizeXSlot = savedX;
            *sizeYSlot = savedY;
        }

        if (canvasObject &&
            *reinterpret_cast<void**>(static_cast<unsigned char*>(canvasObject) + 0xA4) == nullptr &&
            InterlockedExchange(&g_insideHudWindow, 0) != 0)
        {
            if (g_hudWindowLogLines < 256)
            {
                char line[128] = {};
                sprintf_s(line, sizeof(line), "END serial=%llu at SHARED Canvas\n", g_hudWindowSerial);
                HudWindowLog(line);
                ++g_hudWindowLogLines;
            }
        }
    }

    // AFR mono-HUD Canvas transform normalization
    // Caller 0x5D8AEF builds a 4x4 screen transform from FSceneView origin:
    //
    //   [rsi+0x58] -> matrix[12] (OriginX)
    //   [rsi+0x5C] -> matrix[13] (OriginY)
    //   call 0x765030
    //
    // mutating the renderer-owned FSceneView also corrupts AFR world rendering.
    //
    // Normalize the *matrix argument* instead.  This gives the Canvas/HUD a
    // stable mono X origin while leaving the actual AFR FSceneView untouched.
    static constexpr uintptr_t kCanvasTransformAppendRva = 0x765030;
    static constexpr uintptr_t kPerPlayerCanvasTransformReturnRva = 0x5D8AF4;

    using CanvasTransformAppendFn = void (*)(void* transformStack, const float* matrix4x4);
    static CanvasTransformAppendFn g_originalCanvasTransformAppend = nullptr;
    static bool g_canvasTransformAppendHookInstalled = false;
    static unsigned int g_canvasTransformNormalizeLogCount = 0;

    static void HookedCanvasTransformAppend(void* transformStack, const float* matrix4x4)
    {
        if (!g_originalCanvasTransformAppend)
            return;

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t returnRva =
            exe ? (reinterpret_cast<uintptr_t>(_ReturnAddress()) -
                   reinterpret_cast<uintptr_t>(exe)) : 0;

        const bool afrPerPlayerCanvas =
            matrix4x4 &&
            returnRva == kPerPlayerCanvasTransformReturnRva &&
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled;

        if (!afrPerPlayerCanvas)
        {
            g_originalCanvasTransformAppend(transformStack, matrix4x4);
            return;
        }

        alignas(16) float monoMatrix[16] = {};
        memcpy(monoMatrix, matrix4x4, sizeof(monoMatrix));

        const float originalX = monoMatrix[12];
        const float originalY = monoMatrix[13];

        // normalized.  FSceneView+0x58 itself is never written.
        monoMatrix[12] = 0.0f;

        if (g_canvasTransformNormalizeLogCount < 160)
        {
            char line[320] = {};
            sprintf_s(
                line, sizeof(line),
                "AFR_CANVAS_TRANSFORM frame=%llu eye=%s ret=0x%llX "
                "origin=(%.3f,%.3f)->(0.000,%.3f) native=%ux%u\n",
                g_presentFrameNumber,
                g_renderRightEye ? "RIGHT" : "LEFT",
                static_cast<unsigned long long>(returnRva),
                originalX, originalY, originalY,
                g_nativeVrWidth, g_nativeVrHeight);
            CanvasFinalizeLog(line);
            ++g_canvasTransformNormalizeLogCount;
        }

        g_originalCanvasTransformAppend(transformStack, monoMatrix);
    }

    static bool InstallCanvasTransformAppendHook()
    {
        if (g_canvasTransformAppendHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            CanvasFinalizeLog("AFR Canvas transform hook: EXE unavailable\n");
            return false;
        }

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kCanvasTransformAppendRva);

        MH_STATUS status = MH_CreateHook(
            target,
            &HookedCanvasTransformAppend,
            reinterpret_cast<void**>(&g_originalCanvasTransformAppend));

        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            char line[192] = {};
            sprintf_s(
                line, sizeof(line),
                "MH_CreateHook(AFR Canvas transform) FAILED status=%d\n",
                static_cast<int>(status));
            CanvasFinalizeLog(line);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            char line[192] = {};
            sprintf_s(
                line, sizeof(line),
                "MH_EnableHook(AFR Canvas transform) FAILED status=%d\n",
                static_cast<int>(status));
            CanvasFinalizeLog(line);
            return false;
        }

        g_canvasTransformAppendHookInstalled = true;
        CanvasFinalizeLog(
            "AFR mono-HUD Canvas transform hook installed RVA=0x765030 "
            "caller=0x5D8AEF; FSceneView remains untouched.\n");
        return true;
    }

    static bool InstallCanvasFinalizeCandidateHook()
    {
        if (g_canvasFinalizeHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            CanvasFinalizeLog("Canvas full-width test: EXE unavailable\n");
            return false;
        }

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kCanvasFinalizeCandidateRva);

        MH_STATUS status = MH_CreateHook(
            target,
            &HookedCanvasFinalizeCandidate,
            reinterpret_cast<void**>(&g_originalCanvasFinalizeCandidate));

        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            char line[192] = {};
            sprintf_s(
                line, sizeof(line),
                "MH_CreateHook(Canvas finalize) FAILED status=%d\n",
                (int)status);
            CanvasFinalizeLog(line);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            char line[192] = {};
            sprintf_s(
                line, sizeof(line),
                "MH_EnableHook(Canvas finalize) FAILED status=%d\n",
                (int)status);
            CanvasFinalizeLog(line);
            return false;
        }

        g_canvasFinalizeHookInstalled = true;
        CanvasFinalizeLog(
            "Canvas full-width active test installed RVA=0x768AD0\n");

        // Chain the AFR Canvas-transform hook from this already-established UI
        // installation path so no other source file needs to change.
        InstallCanvasTransformAppendHook();
        return true;
    }

    static bool InstallCalcSceneViewCandidateHook()
    {
        if (g_calcSceneViewCandidateHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            DebugLog("CalcSceneView candidate: GetModuleHandleW(NULL) FAILED\n");
            return false;
        }

        void* address = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kCalcSceneViewCandidateRva
            );

        char line[256] = {};
        sprintf_s(
            line,
            sizeof(line),
            "Installing CalcSceneView candidate hook: EXE=%p target=%p RVA=0x%llX\n",
            exe,
            address,
            static_cast<unsigned long long>(kCalcSceneViewCandidateRva)
        );
        DebugLog(line);

        MH_STATUS createStatus = MH_CreateHook(
            address,
            &HookedCalcSceneViewCandidate,
            reinterpret_cast<void**>(&g_originalCalcSceneViewCandidate)
        );

        if (createStatus != MH_OK &&
            createStatus != MH_ERROR_ALREADY_CREATED)
        {
            sprintf_s(line, sizeof(line),
                "MH_CreateHook(CalcSceneView candidate) FAILED status=%d\n",
                static_cast<int>(createStatus));
            DebugLog(line);
            return false;
        }

        MH_STATUS enableStatus = MH_EnableHook(address);
        if (enableStatus != MH_OK &&
            enableStatus != MH_ERROR_ENABLED)
        {
            sprintf_s(line, sizeof(line),
                "MH_EnableHook(CalcSceneView candidate) FAILED status=%d\n",
                static_cast<int>(enableStatus));
            DebugLog(line);
            return false;
        }

        g_calcSceneViewCandidateHookInstalled = true;
        DebugLog("CalcSceneView candidate hook installed\n");
        return true;
    }

