    static volatile LONG64 g_stereoCaptureGeneration=0;
    // Begin and locate the OpenXR frame before UE3 constructs the gameplay
    // FSceneViews. This gives both native eyes the predicted-display-time pose.

    // Immutable ownership of the most recently completed stereo capture.
    // the render thread may execute another scene between SBS capture and XR
    // submission, so submission must not consult the moving "last executed" pose.
    static volatile LONG64 g_capturedStereoPoseSerial=0;

    // Scene/pose binding: correlate an immutable XR pose snapshot with the actual SceneRenderer
    // created by 0x81C9C0, then carry that ownership through queued FDrawSceneCommand::Execute.
    // this is the core render-pose association used by capture/submission; it does not wait or alter timing.
    struct SceneProducerRecord
    {
        void* renderer = nullptr;
        void* viewArray = nullptr;
        unsigned long long producerSerial = 0;
        unsigned long long poseSerial = 0;
        unsigned long long cameraSerial = 0;
        unsigned long long presentAtProduce = 0;
        long long producerQpc = 0;
        unsigned long producerTid = 0;
        uintptr_t producerReturnRva = 0;
    };
    static SceneProducerRecord g_sceneProducerHistory[64] = {};
    static volatile LONG64 g_sceneProducerSerial = 0;
    static volatile LONG64 g_sceneExecuteSerial = 0;
    static thread_local void* g_lastCreatedSceneRenderer = nullptr;
    static thread_local void* g_lastCreatedSceneViewArray = nullptr;
    static thread_local int g_lastCreatedSceneViewCount = 0;
    static thread_local unsigned long long g_sceneRendererCreateSerial = 0;

    // Last renderer that actually entered Execute. Submit snapshots this only for
    // diagnosis; it does not alter which pixels or layer pose are submitted.
    static volatile LONG64 g_lastExecutedScenePoseSerial = 0;
    static volatile LONG64 g_lastExecutedSceneProducerSerial = 0;
    static volatile LONG64 g_lastExecutedSceneCameraSerial = 0;
    static volatile LONG64 g_lastExecutedScenePresentAtProduce = 0;
    static volatile LONG64 g_lastExecutedSceneQpc = 0;

    using V67ProbeFn = unsigned long long (*)(
        uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    using V67ExecFn = unsigned long long (*)(void*);
    static V67ProbeFn g_originalSceneProducer=nullptr;
    static V67ProbeFn g_originalSceneRendererCreate=nullptr;
    static V67ExecFn g_originalSceneExecute=nullptr;

    // Reverse engineering of HatinTimeGame.exe shows this routine reading the
    // PP downsample setting and writing these fields on its RCX object:
    //   +0x124 source width, +0x128 source height
    //   +0x14C effective downsample factor
    //   +0x150 downsample width, +0x154 downsample height
    using V40PpSetupFn = unsigned long long(__fastcall*)(
        uintptr_t,uintptr_t,uintptr_t,uintptr_t,
        uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    static V40PpSetupFn g_originalV40PpSetup=nullptr;
    static volatile LONG64 g_v40PpSetupSerial=0;

    // UE3 PP factor/dimensions change. controller_input.inl consumes this.
    static volatile LONG g_v44PpCaptureRemaining = 0;
    static volatile LONG g_v44PpCaptureGeneration = 0;
    static volatile LONG g_v44PpCaptureFactor = -1;
    static volatile LONG g_v44PpCaptureWidth = 0;
    static volatile LONG g_v44PpCaptureHeight = 0;

    // Disassembly shows the wrapper signature is effectively:
    //   (this, minX, minY, minZ, maxX, maxY, maxZ)
    // and it constructs D3DVIEWPORT9 as:
    //   X=minX, Y=minY, Width=maxX-minX, Height=maxY-minY
    // before calling IDirect3DDevice9::SetViewport (vtable slot 47).
    // this lets us identify the ENGINE caller supplying the stale 1344x720
    // rectangle when the PP target has already shrunk to 672x360, 448x240, etc.
    using V45RhiSetViewportFn = void(__fastcall*)(
        uintptr_t, unsigned int, unsigned int, float,
        unsigned int, unsigned int, float);
    static V45RhiSetViewportFn g_originalV45RhiSetViewport = nullptr;
    static volatile LONG64 g_v45RhiViewportSerial = 0;
    static volatile LONG g_v45ViewportCaptureRemaining = 0;

    static void __fastcall HookV45RhiSetViewport(
        uintptr_t self, unsigned int minX, unsigned int minY, float minZ,
        unsigned int maxX, unsigned int maxY, float maxZ)
    {
        const unsigned long long n =
            (unsigned long long)InterlockedIncrement64(&g_v45RhiViewportSerial);

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t retRva = (exe && ret >= exeBase) ? (ret - exeBase) : 0;

        const unsigned int width = (maxX >= minX) ? (maxX - minX) : 0;
        const unsigned int height = (maxY >= minY) ? (maxY - minY) : 0;
        int factor = 0;
        if (HMODULE exeForPp = GetModuleHandleW(nullptr))
        {
            __try
            {
                factor = *reinterpret_cast<volatile int*>(
                    reinterpret_cast<uintptr_t>(exeForPp) + 0x1103A54);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                factor = 0;
            }
        }

        LONG remaining = InterlockedCompareExchange(&g_v45ViewportCaptureRemaining, 0, 0);
        if (remaining > 0)
        {
            InterlockedDecrement(&g_v45ViewportCaptureRemaining);

            void* frames[10] = {};
            const USHORT frameCount = CaptureStackBackTrace(0, 10, frames, nullptr);
            unsigned long long stackRva[10] = {};
            for (USHORT i = 0; i < frameCount; ++i)
            {
                const uintptr_t p = reinterpret_cast<uintptr_t>(frames[i]);
                if (exe && p >= exeBase && p < exeBase + 0x2000000)
                    stackRva[i] = (unsigned long long)(p - exeBase);
            }

            DxvkPathTrace(
                "V45 RHI-SETVP n=%llu remain=%ld self=%p retRva=0x%llX "
                "rect=%u,%u..%u,%u size=%ux%u z=%.3f..%.3f pp=%d "
                "stack=[0x%llX,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX] tid=%lu",
                n, remaining - 1, reinterpret_cast<void*>(self),
                (unsigned long long)retRva,
                minX, minY, maxX, maxY, width, height, minZ, maxZ, factor,
                stackRva[0], stackRva[1], stackRva[2], stackRva[3], stackRva[4],
                stackRva[5], stackRva[6], stackRva[7], stackRva[8], stackRva[9],
                (unsigned long)GetCurrentThreadId());
        }

        if (g_originalV45RhiSetViewport)
            g_originalV45RhiSetViewport(self, minX, minY, minZ, maxX, maxY, maxZ);
    }

    // the PP path at 0x828BE0 calls this immediately after setting each
    // reduced-resolution eye viewport.  Capturing the destination rectangle,
    // source rectangle, target dimensions and texture dimensions tells us
    // whether the LEFT and RIGHT downsample quads themselves are correct.
    using V46DrawQuadFn = void(__fastcall*)(
        float, float, float, float,
        float, float, float, float,
        unsigned int, unsigned int, unsigned int, unsigned int, float);
    static V46DrawQuadFn g_originalV46DrawQuad = nullptr;
    static volatile LONG64 g_v46DrawQuadSerial = 0;
    static volatile LONG g_v46DrawQuadCaptureRemaining = 0;

    static void __fastcall HookV46DrawQuad(
        float x, float y, float sizeX, float sizeY,
        float u, float v, float sizeU, float sizeV,
        unsigned int targetX, unsigned int targetY,
        unsigned int textureX, unsigned int textureY, float extra)
    {
        const unsigned long long n =
            (unsigned long long)InterlockedIncrement64(&g_v46DrawQuadSerial);

        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t retRva = (exe && ret >= exeBase) ? (ret - exeBase) : 0;

        LONG remaining = InterlockedCompareExchange(&g_v46DrawQuadCaptureRemaining, 0, 0);
        if (remaining > 0)
        {
            const LONG slot = InterlockedDecrement(&g_v46DrawQuadCaptureRemaining);
            const LONG gen = InterlockedCompareExchange(&g_v44PpCaptureGeneration, 0, 0);
            const LONG factor = InterlockedCompareExchange(&g_v44PpCaptureFactor, 0, 0);
            const LONG ppW = InterlockedCompareExchange(&g_v44PpCaptureWidth, 0, 0);
            const LONG ppH = InterlockedCompareExchange(&g_v44PpCaptureHeight, 0, 0);

            DxvkPathTrace(
                "V47 QUAD gen=%ld remain=%ld factor=%ld ppDs=%ldx%ld n=%llu retRva=0x%llX "
                "dst=[%.3f,%.3f %.3fx%.3f] src=[%.3f,%.3f %.3fx%.3f] "
                "target=%ux%u texture=%ux%u extra=%.3f tid=%lu",
                gen, slot, factor, ppW, ppH, n, (unsigned long long)retRva,
                x, y, sizeX, sizeY, u, v, sizeU, sizeV,
                targetX, targetY, textureX, textureY, extra,
                (unsigned long)GetCurrentThreadId());
        }

        if (g_originalV46DrawQuad)
            g_originalV46DrawQuad(
                x, y, sizeX, sizeY, u, v, sizeU, sizeV,
                targetX, targetY, textureX, textureY, extra);
    }

    // intentionally passive.  We log viewport/scissor changes together with
    // the currently-bound render-target dimensions and live PPDownSampleFactor
    // to check whether RIGHT-eye source coordinates are being applied to
    // a downsampled target without scaling.
    using V41SetViewportFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    using V41SetScissorRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
    static V41SetViewportFn g_originalV41SetViewport = nullptr;
    static V41SetScissorRectFn g_originalV41SetScissorRect = nullptr;
    static volatile LONG64 g_v41ViewportSerial = 0;
    static volatile LONG64 g_v41ScissorSerial = 0;
    static bool g_v41RasterHooksInstalled = false;

    // this flag is armed only while AHiT is executing 0x825F30 for the
    // renderer's actual RIGHT FViewInfo (Views[1], stride 0x1400).
    static thread_local bool g_stereoLiteRightDecalPass = false;

    using V180PerViewDecalFn = void (__fastcall*)(void* renderer, int passIndex, void* view, int arg4);
    static V180PerViewDecalFn g_originalV180PerViewDecal = nullptr;

    // Detail-shadow relevance gate experiment.
    // 0x7E3850 consumes per-primitive packed relevance at FViewInfo+0x644.
    // Its native loops gate proxy submission on bits 6..9 (selected by pass)
    // and pass bits 10/12 into the proxy draw. Data3 already makes LEFT/RIGHT
    // share the same +0x644 array, so copying LEFT is redundant. Instead, for
    // RIGHT only, force bits 6..12 for primitives actually referenced by this
    // pass's three native lists (+0x704/+0x744/+0x784, 16-byte stride/pass).
    // Every modified word is restored immediately after the native call.
    using V186DetailRelevanceFn =
        void (__fastcall*)(void* renderer, void* view, int passIndex, int arg4, int arg5, int arg6);
    static V186DetailRelevanceFn g_originalV186DetailRelevance = nullptr;

    struct V186SavedRelevance
    {
        unsigned int* word;
        unsigned int value;
    };

    static void __fastcall HookV186DetailRelevance(
        void* renderer, void* view, int passIndex, int arg4, int arg5, int arg6)
    {
        V186SavedRelevance saved[768] = {};
        unsigned int savedCount = 0;
        bool isRight = false;

        if (g_nativeStereoEnabled && g_alternatingStereoEnabled &&
            renderer && view && passIndex >= 0 && passIndex < 4)
        {
            __try
            {
                auto* rr = static_cast<unsigned char*>(renderer);
                auto* views = *reinterpret_cast<unsigned char**>(rr + 0x6C);
                const int viewCount = *reinterpret_cast<int*>(rr + 0x74);
                isRight = views && viewCount >= 2 &&
                    static_cast<unsigned char*>(view) == views + 0x1400;

                if (isRight)
                {
                    auto* fv = static_cast<unsigned char*>(view);
                    auto* packed =
                        *reinterpret_cast<unsigned int**>(fv + 0x644);

                    if (packed)
                    {
                        const unsigned listBases[3] = { 0x704, 0x744, 0x784 };
                        for (unsigned li = 0; li < 3; ++li)
                        {
                            const unsigned off =
                                listBases[li] + static_cast<unsigned>(passIndex) * 0x10;
                            auto** list =
                                *reinterpret_cast<unsigned char***>(fv + off);
                            const int count =
                                *reinterpret_cast<int*>(fv + off + 0x08);

                            if (!list || count <= 0 || count > 65536)
                                continue;

                            for (int i = 0; i < count && savedCount < 768; ++i)
                            {
                                auto* prim = list[i];
                                if (!prim)
                                    continue;

                                const int primitiveIndex =
                                    *reinterpret_cast<int*>(prim + 0x30);
                                if (primitiveIndex < 0 || primitiveIndex > 1048576)
                                    continue;

                                unsigned int* word = packed + primitiveIndex;

                                bool alreadySaved = false;
                                for (unsigned int j = 0; j < savedCount; ++j)
                                {
                                    if (saved[j].word == word)
                                    {
                                        alreadySaved = true;
                                        break;
                                    }
                                }
                                if (alreadySaved)
                                    continue;

                                saved[savedCount].word = word;
                                saved[savedCount].value = *word;
                                ++savedCount;

                                // Bits 6..12 inclusive. Preserve every other
                                // relevance/visibility classification bit.
                                *word |= 0x00001FC0u;
                            }
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Restore whatever was safely captured below.
            }
        }

        if (g_originalV186DetailRelevance)
            g_originalV186DetailRelevance(
                renderer, view, passIndex, arg4, arg5, arg6);

        for (unsigned int i = 0; i < savedCount; ++i)
        {
            __try
            {
                if (saved[i].word)
                    *saved[i].word = saved[i].value;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        if (isRight && savedCount)
        {
            static unsigned int logs = 0;
            if (logs++ < 24)
                DxvkPathTrace(
                    "V186 DETAIL_RELEVANCE RIGHT pass=%d forcedBits6_12 primitives=%u",
                    passIndex, savedCount);
        }
    }

    static void __fastcall HookV180PerViewDecal(void* renderer, int passIndex, void* view, int arg4)
    {
        const bool previous = g_stereoLiteRightDecalPass;
        bool isRight = false;

        // StereoLite only. AFR has g_nativeStereoEnabled == false, and this
        // does not enable/consult sequential reentry at all.
        if (g_nativeStereoEnabled && g_alternatingStereoEnabled && renderer && view)
        {
            __try
            {
                auto* r = static_cast<unsigned char*>(renderer);
                auto* views = *reinterpret_cast<unsigned char**>(r + 0x6C);
                const int count = *reinterpret_cast<int*>(r + 0x74);
                if (views && count >= 2)
                    isRight = (static_cast<unsigned char*>(view) == views + 0x1400);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                isRight = false;
            }
        }

        g_stereoLiteRightDecalPass = isRight;

        if (g_originalV180PerViewDecal)
            g_originalV180PerViewDecal(renderer, passIndex, view, arg4);

        g_stereoLiteRightDecalPass = previous;
    }

    static void V41GetRt0Size(IDirect3DDevice9* device, UINT& w, UINT& h, D3DFORMAT& fmt)
    {
        w = h = 0; fmt = D3DFMT_UNKNOWN;
        if (!device) return;
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt)
        {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(rt->GetDesc(&d))) { w=d.Width; h=d.Height; fmt=d.Format; }
            rt->Release();
        }
    }

    static int V41LivePpFactor()
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return -1;
        __try { return *reinterpret_cast<volatile int*>(reinterpret_cast<uintptr_t>(exe)+0x1103A54); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    static HRESULT STDMETHODCALLTYPE HookV41SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp)
    {
        const unsigned long long n=(unsigned long long)InterlockedIncrement64(&g_v41ViewportSerial);
        HMODULE exe=GetModuleHandleW(nullptr);
        const uintptr_t ret=exe ? reinterpret_cast<uintptr_t>(_ReturnAddress())-reinterpret_cast<uintptr_t>(exe) : 0;
        UINT rw=0,rh=0; D3DFORMAT rf=D3DFMT_UNKNOWN; V41GetRt0Size(device,rw,rh,rf);
        const int factor=V41LivePpFactor();
        if (vp)
        {
            // keep logs useful rather than emitting every redundant full-screen set.
            static DWORD lx=0xffffffffu,ly=0xffffffffu,lw=0xffffffffu,lh=0xffffffffu;
            static UINT lrw=0xffffffffu,lrh=0xffffffffu; static int lf=-999;
            const bool changed=vp->X!=lx||vp->Y!=ly||vp->Width!=lw||vp->Height!=lh||rw!=lrw||rh!=lrh||factor!=lf;
            (void)changed;
            const bool suspicious=(rw && (vp->X>=rw || vp->X+vp->Width>rw || vp->Y>=rh || vp->Y+vp->Height>rh));
            // so "changed" is not a useful logging condition here. It produced ~245k lines/session.
            // keep a tiny startup sample, genuine OOB events, and a very sparse heartbeat.
            if (n<=24 || suspicious || (n%12000ULL)==0)
                DxvkPathTrace("V41 SETVP n=%llu retRva=0x%llX vp=%lu,%lu %lux%lu z=%.3f..%.3f rt=%ux%u fmt=%u pp=%d OOB=%d tid=%lu",
                    n,(unsigned long long)ret,(unsigned long)vp->X,(unsigned long)vp->Y,
                    (unsigned long)vp->Width,(unsigned long)vp->Height,vp->MinZ,vp->MaxZ,
                    rw,rh,(unsigned)rf,factor,suspicious?1:0,(unsigned long)GetCurrentThreadId());
            lx=vp->X;ly=vp->Y;lw=vp->Width;lh=vp->Height;lrw=rw;lrh=rh;lf=factor;
        }
        // RIGHT-eye per-view decal pass. Never infer the eye from raster state.
        if (vp && g_stereoLiteRightDecalPass && rw >= 2)
        {
            const DWORD half = rw / 2;
            if (vp->X < half && vp->Width <= half && vp->X + vp->Width <= half)
            {
                D3DVIEWPORT9 fixed = *vp;
                fixed.X += half;
                return g_originalV41SetViewport
                    ? g_originalV41SetViewport(device, &fixed)
                    : D3DERR_INVALIDCALL;
            }
        }

        return g_originalV41SetViewport ? g_originalV41SetViewport(device,vp) : D3DERR_INVALIDCALL;
    }

    static HRESULT STDMETHODCALLTYPE HookV41SetScissorRect(IDirect3DDevice9* device, const RECT* r)
    {
        const unsigned long long n=(unsigned long long)InterlockedIncrement64(&g_v41ScissorSerial);
        HMODULE exe=GetModuleHandleW(nullptr);
        const uintptr_t ret=exe ? reinterpret_cast<uintptr_t>(_ReturnAddress())-reinterpret_cast<uintptr_t>(exe) : 0;
        UINT rw=0,rh=0; D3DFORMAT rf=D3DFMT_UNKNOWN; V41GetRt0Size(device,rw,rh,rf);
        const int factor=V41LivePpFactor();
        if (r)
        {
            static LONG ll=LONG_MIN,lt=LONG_MIN,lr=LONG_MIN,lb=LONG_MIN; static UINT lw=0xffffffffu,lh=0xffffffffu; static int lf=-999;
            const bool changed=r->left!=ll||r->top!=lt||r->right!=lr||r->bottom!=lb||rw!=lw||rh!=lh||factor!=lf;
            const bool suspicious=(rw && (r->left<0||r->top<0||r->right>(LONG)rw||r->bottom>(LONG)rh||r->left>=r->right||r->top>=r->bottom));
            if (n<=160 || changed || suspicious || (n%600ULL)==0)
                DxvkPathTrace("V41 SCISSOR n=%llu retRva=0x%llX rect=%ld,%ld..%ld,%ld size=%ldx%ld rt=%ux%u fmt=%u pp=%d OOB=%d tid=%lu",
                    n,(unsigned long long)ret,r->left,r->top,r->right,r->bottom,
                    r->right-r->left,r->bottom-r->top,rw,rh,(unsigned)rf,factor,suspicious?1:0,(unsigned long)GetCurrentThreadId());
            ll=r->left;lt=r->top;lr=r->right;lb=r->bottom;lw=rw;lh=rh;lf=factor;
        }
        // Same exact-eye correction for stale LEFT-half scissor state.
        if (r && g_stereoLiteRightDecalPass && rw >= 2)
        {
            const LONG half = static_cast<LONG>(rw / 2);
            if (r->left >= 0 && r->right <= half && r->left < r->right)
            {
                RECT fixed = *r;
                fixed.left += half;
                fixed.right += half;
                return g_originalV41SetScissorRect
                    ? g_originalV41SetScissorRect(device, &fixed)
                    : D3DERR_INVALIDCALL;
            }
        }

        return g_originalV41SetScissorRect ? g_originalV41SetScissorRect(device,r) : D3DERR_INVALIDCALL;
    }

    static unsigned long long __fastcall HookV40PpSetup(
        uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,
        uintptr_t a5,uintptr_t a6,uintptr_t a7,uintptr_t a8)
    {
        const unsigned long long serial =
            (unsigned long long)InterlockedIncrement64(&g_v40PpSetupSerial);
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t retRva = exe
            ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(exe) : 0;

        int pre124=0, pre128=0, pre14c=0, pre150=0, pre154=0;
        if (a1)
        {
            __try
            {
                auto* p = reinterpret_cast<unsigned char*>(a1);
                memcpy(&pre124,p+0x124,4); memcpy(&pre128,p+0x128,4);
                memcpy(&pre14c,p+0x14c,4); memcpy(&pre150,p+0x150,4);
                memcpy(&pre154,p+0x154,4);
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
        }

        const unsigned long long result = g_originalV40PpSetup
            ? g_originalV40PpSetup(a1,a2,a3,a4,a5,a6,a7,a8) : 0;

        int post124=0, post128=0, post14c=0, post150=0, post154=0;
        if (a1)
        {
            __try
            {
                auto* p = reinterpret_cast<unsigned char*>(a1);
                memcpy(&post124,p+0x124,4); memcpy(&post128,p+0x128,4);
                memcpy(&post14c,p+0x14c,4); memcpy(&post150,p+0x150,4);
                memcpy(&post154,p+0x154,4);
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
        }

        // Log the first calls densely, then sample. Also always log if the
        // dimensions/factor change so a 100% -> below-100% comparison is easy.
        static int last124=-1,last128=-1,last14c=-1,last150=-1,last154=-1;
        const bool changed = post124!=last124 || post128!=last128 || post14c!=last14c ||
                             post150!=last150 || post154!=last154;

        // setup transition arms the next 100 candidate fullscreen draws.
        // Skip the very first observation; it is initialization, not a user
        // setting transition.
        if (changed && last14c != -1)
        {
            InterlockedExchange(&g_v44PpCaptureFactor, post14c);
            InterlockedExchange(&g_v44PpCaptureWidth, post150);
            InterlockedExchange(&g_v44PpCaptureHeight, post154);
            const LONG gen = InterlockedIncrement(&g_v44PpCaptureGeneration);
            InterlockedExchange(&g_v44PpCaptureRemaining, 100);
            InterlockedExchange(&g_v45ViewportCaptureRemaining, 80);
            InterlockedExchange(&g_v46DrawQuadCaptureRemaining, 160);
            DxvkPathTrace(
                "V44 PP-CHANGE gen=%ld oldFactor=%d oldDs=%dx%d newFactor=%d newDs=%dx%d capture=100 V45viewport=80",
                gen,last14c,last150,last154,post14c,post150,post154);
        }
        if (serial <= 64 || changed || (serial % 120ULL)==0)
        {
            DxvkPathTrace(
                "V40 PP-SETUP n=%llu obj=%p retRva=0x%llX "
                "pre[src=%dx%d factor=%d ds=%dx%d] "
                "post[src=%dx%d factor=%d ds=%dx%d] result=%p tid=%lu",
                serial,reinterpret_cast<void*>(a1),(unsigned long long)retRva,
                pre124,pre128,pre14c,pre150,pre154,
                post124,post128,post14c,post150,post154,
                reinterpret_cast<void*>(result),(unsigned long)GetCurrentThreadId());
        }
        last124=post124; last128=post128; last14c=post14c;
        last150=post150; last154=post154;
        return result;
    }

    // before HookSceneProducer because that hook appears earlier in this file
    static thread_local void* g_v137ProducerGameplayViewFamily = nullptr;

    //
    // 0x81E7A0 is the engine path that obtains a fresh ~0x108EE0 renderer
    // allocation, constructs it through 0x81C9C0, and hands it to UE3's normal
    // Render() twice on the same FSceneRenderer object.
    static thread_local bool g_v137InsideSecondProducer = false;
    static volatile LONG64 g_v137FreshRendererPairs = 0;

    // the gameplay Calc hook creates RIGHT through AHiT's real CalcSceneView and
    // removes it from the family again before the normal Draw continues.  The
    // second fresh producer later exposes that exact engine-created view to
    // FSceneRenderer instead of synthesizing RIGHT from LEFT.
    static thread_local void* g_v146GenuineRightView = nullptr;
    static thread_local void* g_v146GenuineRightFamily = nullptr;
    static thread_local unsigned long long g_v146GenuineRightPoseSerial = 0;
    // its ViewState pointer during 0x81C9C0. No camera fields are copied.
    static thread_local void* g_v151GenuineLeftView = nullptr;

    static unsigned long long HookSceneProducer(
        uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,
        uintptr_t a5,uintptr_t a6,uintptr_t a7,uintptr_t a8)
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t retRva = exe
            ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(exe) : 0;

        const bool v137GameplayProducer =
            !g_v137InsideSecondProducer &&
            !g_nativeStereoEnabled &&
            g_alternatingStereoEnabled &&
            a2 != 0 &&
            reinterpret_cast<void*>(a2) == g_v137ProducerGameplayViewFamily;

        // First producer is completely ordinary AFR/UE3 behavior.
        const unsigned long long result = g_originalSceneProducer
            ? g_originalSceneProducer(a1,a2,a3,a4,a5,a6,a7,a8) : 0;

        // same gameplay family.  This is intentionally still the same AFR eye;
        // the goal is to prove that fresh constructor-time renderer state keeps
        // decals/effects intact without the dangerous 0x5D7DF0 Draw reentry.
        //
        // Calling the original trampoline directly does not recurse through this
        // hook. The guard documents/defends the intended ownership anyway.
        if (v137GameplayProducer && g_originalSceneProducer)
        {
            g_v137InsideSecondProducer = true;
            const LONG64 pair = InterlockedIncrement64(&g_v137FreshRendererPairs);

            if (pair <= 24 || (pair % 600) == 0)
                DxvkPathTrace(
                    "V139 SAME_TICK_RIGHT_PRODUCER begin pair=%lld family=%p present=%llu snap=%llu eye=%s",
                    (long long)pair,
                    reinterpret_cast<void*>(a2),
                    (unsigned long long)g_presentFrameNumber,
                    (unsigned long long)g_renderPoseSnapshotSerial,
                    g_renderRightEye ? "RIGHT" : "LEFT");

            g_originalSceneProducer(a1,a2,a3,a4,a5,a6,a7,a8);

            if (pair <= 24 || (pair % 600) == 0)
                DxvkPathTrace(
                    "V139 SAME_TICK_RIGHT_PRODUCER end pair=%lld family=%p present=%llu",
                    (long long)pair,
                    reinterpret_cast<void*>(a2),
                    (unsigned long long)g_presentFrameNumber);

            g_v137InsideSecondProducer = false;
        }

        // renderer that is later queued as FDrawSceneCommand.
        if (retRva == 0x5D8C29 || retRva == 0x5D822C)
        {
            LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
            const unsigned long long serial =
                (unsigned long long)InterlockedIncrement64(&g_sceneProducerSerial);
            SceneProducerRecord slot{};
            slot.renderer = g_lastCreatedSceneRenderer;
            slot.viewArray = g_lastCreatedSceneViewArray;
            slot.producerSerial = serial;
            slot.poseSerial = g_renderPoseSnapshotSerial;
            slot.cameraSerial = g_v72CameraSerial;
            slot.presentAtProduce = (unsigned long long)g_presentFrameNumber;
            slot.producerQpc = q.QuadPart;
            slot.producerTid = GetCurrentThreadId();
            slot.producerReturnRva = retRva;
            g_sceneProducerHistory[serial & 63ULL] = slot;

            if (serial <= 32 || (serial % 120ULL) == 0)
            {
                char line[512] = {};
                sprintf_s(line,sizeof(line),
                    "V78_PRODUCE producer=%llu cam=%llu pose=%llu present=%llu renderer=%p viewArray=%p views=%d create=%llu retRva=0x%llX qpc=%lld tid=%lu\n",
                    serial,slot.cameraSerial,slot.poseSerial,slot.presentAtProduce,
                    slot.renderer,slot.viewArray,g_lastCreatedSceneViewCount,g_sceneRendererCreateSerial,
                    (unsigned long long)retRva,(long long)q.QuadPart,slot.producerTid);
                RenderPipelineDiagnosticLog(line);
            }
        }
        return result;
    }
    // Instead of feeding one FSceneRenderer two views, run AHiT's high-level
    // gameplay Draw twice in the SAME game tick. Each renderer therefore sees
    // exactly one FSceneView at index 0. Pass 1 writes the LEFT SBS half; pass 2
    // writes the RIGHT SBS half. The second pass reuses the same frozen OpenXR
    // snapshot, so this is not AFR and does not resample head pose.
    static bool g_v26SequentialReentry = true; // HYBRID-V1: sequential gameplay Draw reentry for compatibility AFR
    static thread_local bool g_v26InsideDraw = false;
    static thread_local bool g_v26SecondPass = false;
    static thread_local bool g_v26SawGameplayCalc = false;
    static volatile LONG64 g_v26DrawPairs = 0;
    static volatile LONG64 g_v26SecondDraws = 0;

    using V26GameplayDrawFn = void(__fastcall*)(void*, void*, void*);
    static V26GameplayDrawFn g_originalV26GameplayDraw = nullptr;

    static void __fastcall HookV26GameplayDraw(void* self, void* a2, void* a3)
    {
        // 0x5D7DF0 is the high-level AHiT gameplay/client Draw containing the
        // CalcSceneView -> scene-renderer submission path (both known producer
        // sites 0x5D8227 and 0x5D8C24 live inside it).
        //
        // do not guess which static caller is gameplay. Instead, allow the first
        // call through normally and let HookedCalcSceneViewCandidate prove that
        // this invocation actually contained the known gameplay CalcSceneView.
        if (!g_originalV26GameplayDraw)
            return;

        if (!g_v26SequentialReentry || g_v26InsideDraw ||
            g_nativeStereoEnabled || !g_alternatingStereoEnabled)
        {
            g_originalV26GameplayDraw(self, a2, a3);
            return;
        }

        // the enclosing viewport reaches its ordinary Present. Tell the backup's
        // proven Present-owned capture path to split that completed SBS pair.
        g_v138SameTickAfrActive = true;

        g_v26InsideDraw = true;
        g_v26SecondPass = false;
        g_v26SawGameplayCalc = false;
        g_originalV26GameplayDraw(self, a2, a3); // LEFT, ordinary single-view UE3

        const bool wasGameplay = g_v26SawGameplayCalc;
        if (wasGameplay && g_renderPoseSnapshotValid && g_xrViewsValidThisFrame)
        {
            g_v26SecondPass = true;
            g_v26SawGameplayCalc = false;
            InterlockedIncrement64(&g_v26SecondDraws);
            g_originalV26GameplayDraw(self, a2, a3); // RIGHT, again Views[0]
            g_v26SecondPass = false;
            const LONG64 pair = InterlockedIncrement64(&g_v26DrawPairs);
            if (pair <= 12 || (pair % 600) == 0)
                DxvkPathTrace("HYBRID-V1 V26 SEQUENTIAL_REENTRY pair=%lld secondDraws=%lld snap=%llu",
                    (long long)pair,
                    (long long)InterlockedCompareExchange64(&g_v26SecondDraws,0,0),
                    (unsigned long long)g_renderPoseSnapshotSerial);
        }

        g_v26SecondPass = false;
        g_v26InsideDraw = false;
    }

    // exact FSceneRenderer constructed from it.  This prevents the low-level
    static thread_local void* g_v136GameplayViewFamily = nullptr;
    static thread_local void* g_v136GameplayRenderer = nullptr;
    static thread_local unsigned long long g_v136GameplayFamilySerial = 0;

    // Static RE of 0x7E3850 shows the decal/dynamic consumer reads completed
    // FViewInfo state, not merely the 0x610-byte source FSceneView.
    static thread_local bool g_v153LeftDecalStateValid = false;
    static thread_local unsigned long long g_v153LeftDecalPoseSerial = 0;
    static thread_local unsigned char g_v153Left600_7FF[0x200] = {};
    static thread_local unsigned char g_v153Left1130_117F[0x50] = {};
    static thread_local unsigned char g_v153Left13F0_13FF[0x10] = {};
    // LEFT-DATA-3: full completed LEFT FViewInfo snapshot for a broad post-constructor
    // decal-state transplant. Camera/viewport/PP geometry on RIGHT remain untouched.
    static thread_local unsigned char g_leftData3CompletedLeft[0x1400] = {};

    static unsigned long long HookSceneRendererCreate(
        uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,
        uintptr_t a5,uintptr_t a6,uintptr_t a7,uintptr_t a8)
    {

        // constructor. Its first field is TArray<FSceneView*>:
        //   +0x00 data, +0x08 count, +0x0C max.
        // Compare pointer-sized fields in LEFT/RIGHT before the renderer copies
        // them. The right eye is currently created with LocalPlayer ViewState
        // temporarily nulled, so this should expose exactly which FSceneView
        // field becomes null/different.
        static unsigned long long v39Create = 0;
        ++v39Create;
        void* srcViews[2] = {};
        int srcCount = 0;
        if (a2)
        {
            __try
            {
                auto* fam = reinterpret_cast<unsigned char*>(a2);
                void** data = *reinterpret_cast<void***>(fam + 0x00);
                srcCount = *reinterpret_cast<int*>(fam + 0x08);
                if (data && srcCount > 0) srcViews[0] = data[0];
                if (data && srcCount > 1) srcViews[1] = data[1];
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                srcViews[0] = srcViews[1] = nullptr;
                srcCount = 0;
            }
        }

        // FSceneView created by the second CalcSceneView call earlier in this
        // same gameplay Draw.  Do not clone LEFT, patch matrices, or call
        // 0x80A190 here.
        void* v146RightOnlyPtrs[1] = {};
        alignas(16) unsigned char v154HybridRightView[0x610] = {};
        void** v146SavedFamilyData = nullptr;
        int v146SavedFamilyCount = 0;
        int v146SavedFamilyMax = 0;
        bool v146RightOnlyCtor = false;

        // Compare genuine LEFT vs genuine RIGHT immediately before renderer #2
        // construction. This deliberately changes NO view state.

        if (g_v137InsideSecondProducer &&
            g_v138SameTickAfrActive &&
            !g_nativeStereoEnabled && g_alternatingStereoEnabled &&
            g_v146GenuineRightView &&
            g_v146GenuineRightFamily == reinterpret_cast<void*>(a2) &&
            g_v146GenuineRightPoseSerial == g_renderPoseSnapshotSerial &&
            a2)
        {
            __try
            {
                auto* fam = reinterpret_cast<unsigned char*>(a2);
                v146SavedFamilyData = *reinterpret_cast<void***>(fam + 0x00);
                v146SavedFamilyCount = *reinterpret_cast<int*>(fam + 0x08);
                v146SavedFamilyMax = *reinterpret_cast<int*>(fam + 0x0C);

                // Return to the proven Data3 renderer topology.  The real
                // renderer #2 is constructed from genuine RIGHT only.
                v146RightOnlyPtrs[0] = g_v146GenuineRightView;
                srcViews[0] = g_v146GenuineRightView;

                *reinterpret_cast<void***>(fam + 0x00) = v146RightOnlyPtrs;
                *reinterpret_cast<int*>(fam + 0x08) = 1;
                *reinterpret_cast<int*>(fam + 0x0C) = 1;

                srcViews[1] = nullptr;
                srcCount = 1;
                v146RightOnlyCtor = true;

                static unsigned int v171PrimeLogs = 0;
                if (v171PrimeLogs++ < 12)
                    DxvkPathTrace(
                        "V174 DATA3 RIGHT-only ctor family=%p right=%p left=%p snap=%llu",
                        reinterpret_cast<void*>(a2),
                        g_v146GenuineRightView, g_v151GenuineLeftView,
                        (unsigned long long)g_renderPoseSnapshotSerial);

            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                v146RightOnlyCtor = false;
            }
        }

        // same-frame stereo: CalcSceneView supplies one principal
        // LEFT-half FSceneView. Expand that principal source to two byte-identical
        // views only at the FSceneRenderer construction boundary, exactly like
        // AHiT allocates FSceneView as 0x610
        // bytes at RVA 0x5D5332; unlike BL1, do not borrow BL1's 0x1750 layout.
        //
        // the principal view already contains the LEFT-eye camera and half-width
        // viewport. RIGHT starts as an exact copy, then receives a +1 IPD camera
        // translation in view space. RVA 0x80A190 is AHiT's own FSceneView derived-
        // state builder called by the native constructor after initializing the
        // base view/projection matrices; use it so frusta/inverses/cached matrices
        // remain engine-coherent rather than hand-patching derived fields.
        alignas(16) unsigned char bl1RightView[0x610] = {};
        void* bl1StereoPtrs[2] = {};
        void** bl1SavedFamilyData = nullptr;
        int bl1SavedFamilyCount = 0;
        int bl1SavedFamilyMax = 0;
        bool bl1Expanded = false;
        // Stereo Lite must only expand the renderer produced from the proven
        // gameplay CalcSceneView family.  FSceneRenderer is also constructed for
        // startup/menu/offscreen work before OpenXR has a current gameplay pose;
        // expanding those families produced a zero-IPD RIGHT clone and polluted
        // auxiliary renderers.
        const bool bl1GameplayFamily =
            a2 && reinterpret_cast<void*>(a2) == g_v136GameplayViewFamily;
        const bool bl1PoseReady =
            g_renderPoseSnapshotValid && g_xrViewsValidThisFrame;

        if (!g_v26SequentialReentry &&
            g_nativeStereoEnabled && g_alternatingStereoEnabled &&
            bl1GameplayFamily && bl1PoseReady &&
            srcCount == 1 && srcViews[0] && a2)
        {
            __try
            {
                memcpy(bl1RightView, srcViews[0], sizeof(bl1RightView));

                *reinterpret_cast<uintptr_t*>(bl1RightView + 0x08) = 0;

                // Principal is LEFT. Move the clone one full runtime IPD to the
                // camera's right. UE3 FMatrix is row-vector based; camera-local
                // +X/right movement subtracts from ViewMatrix M[3][0].
                const XrView* bv = g_renderPoseSnapshotValid
                    ? g_renderPoseSnapshotViews : g_xrViews;
                const float dx = bv[1].pose.position.x - bv[0].pose.position.x;
                const float dy = bv[1].pose.position.y - bv[0].pose.position.y;
                const float dz = bv[1].pose.position.z - bv[0].pose.position.z;
                const float ipdUU = sqrtf(dx*dx + dy*dy + dz*dz) * g_xrWorldUnitsPerMeter;
                float* rightViewMatrix = reinterpret_cast<float*>(bl1RightView + 0x90);
                rightViewMatrix[12] -= ipdUU;

                // LEFT CalcSceneView was built into the left half. Its clone needs
                // tracing independently established FSceneView+0x58 as OriginX.
                float* rightOriginX = reinterpret_cast<float*>(bl1RightView + 0x58);
                const float halfWidth = static_cast<float>(g_xrRuntimeEyeWidth);
                if (halfWidth > 1.0f)
                    *rightOriginX += halfWidth;

                using RebuildViewFn = void(__fastcall*)(void*, int);
                const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                RebuildViewFn rebuild = reinterpret_cast<RebuildViewFn>(exeBase + 0x80A190);
                rebuild(bl1RightView, 0);

                auto* fam = reinterpret_cast<unsigned char*>(a2);
                bl1SavedFamilyData = *reinterpret_cast<void***>(fam + 0x00);
                bl1SavedFamilyCount = *reinterpret_cast<int*>(fam + 0x08);
                bl1SavedFamilyMax = *reinterpret_cast<int*>(fam + 0x0C);
                bl1StereoPtrs[0] = srcViews[0];
                bl1StereoPtrs[1] = bl1RightView;
                *reinterpret_cast<void***>(fam + 0x00) = bl1StereoPtrs;
                *reinterpret_cast<int*>(fam + 0x08) = 2;
                *reinterpret_cast<int*>(fam + 0x0C) = 2;
                srcViews[1] = bl1RightView;
                srcCount = 2;
                bl1Expanded = true;

                static LONG bl1Once = 0;
                if (InterlockedCompareExchange(&bl1Once, 1, 0) == 0)
                    DxvkPathTrace(
                        "BL1_STEREOLITE_GAMEPLAY XR_PRINCIPAL family=%p principal=%p clone=%p size=0x610 ipdUU=%.5f snap=%llu rebuild=0x80A190",
                        reinterpret_cast<void*>(a2), srcViews[0], bl1RightView, ipdUU,
                        (unsigned long long)g_renderPoseSnapshotSerial);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                bl1Expanded = false;
            }
        }

        if ((v39Create <= 24 || (v39Create % 120ULL) == 0) &&
            srcCount >= 2 && srcViews[0] && srcViews[1])
        {
            char diff[4096] = {};
            size_t used = 0;
            int emitted = 0;
            __try
            {
                auto* l = static_cast<unsigned char*>(srcViews[0]);
                auto* r = static_cast<unsigned char*>(srcViews[1]);

                // Pointer-heavy front/middle region only. Log fields where one
                // side is null and the other is not, plus a small number of
                // unequal non-null pointers. This keeps the log useful.
                for (unsigned off = 0; off <= 0x500 && emitted < 40; off += 8)
                {
                    const uintptr_t lv = *reinterpret_cast<uintptr_t*>(l + off);
                    const uintptr_t rv = *reinterpret_cast<uintptr_t*>(r + off);
                    if (lv == rv) continue;

                    const bool nullSplit = ((lv == 0) != (rv == 0));
                    const bool ptrish =
                        (lv == 0 || lv > 0x10000ULL) &&
                        (rv == 0 || rv > 0x10000ULL);
                    if (!nullSplit && !ptrish) continue;

                    const int w = _snprintf_s(
                        diff + used, sizeof(diff) - used, _TRUNCATE,
                        "%s+%03X:L=%llX R=%llX%s",
                        used ? " | " : "",
                        off,
                        (unsigned long long)lv,
                        (unsigned long long)rv,
                        nullSplit ? " NULLSPLIT" : "");
                    if (w <= 0) break;
                    used += (size_t)w;
                    ++emitted;
                    if (used >= sizeof(diff) - 128) break;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                _snprintf_s(diff, sizeof(diff), _TRUNCATE, "<exception while comparing views>");
            }

            DxvkPathTrace(
                "V38 SOURCE-VIEWS create=%llu family=%p count=%d L=%p R=%p diffs=[%s]",
                v39Create, reinterpret_cast<void*>(a2), srcCount,
                srcViews[0], srcViews[1], diff);
        }

        // FSceneView + 0x08 is persistently non-null for LEFT and null for RIGHT.
        // do NOT change LocalPlayer/ViewState during CalcSceneView: keep the existing
        // anti-flicker construction path intact. Only at the renderer-construction
        // boundary, temporarily give RIGHT the LEFT source view's +0x08 pointer so
        // FSceneRenderer/FViewInfo can copy it. Restore the source view immediately
        // after the constructor returns. This intentionally shares state and is NOT
        // a final stereo architecture; it only tests whether the missing right-eye
        // renderer effects are caused by the null ViewState.
        uintptr_t v39LeftState = 0;
        uintptr_t v39RightStateBefore = 0;
        bool v39Patched = false;
        if (srcCount >= 2 && srcViews[0] && srcViews[1])
        {
            __try
            {
                auto* l = static_cast<unsigned char*>(srcViews[0]);
                auto* r = static_cast<unsigned char*>(srcViews[1]);
                v39LeftState = *reinterpret_cast<uintptr_t*>(l + 0x08);
                v39RightStateBefore = *reinterpret_cast<uintptr_t*>(r + 0x08);

                // we want its vtable and executable RVAs to identify the
                // engine-owned creation/destruction path for a second independent
                // persistent state rather than cloning/sharing this object.
                if (v39LeftState && (v39Create <= 8 || (v39Create % 120ULL) == 0))
                {
                    auto* state = reinterpret_cast<unsigned char*>(v39LeftState);
                    uintptr_t vt = *reinterpret_cast<uintptr_t*>(state);
                    uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

                    char slots[4096] = {};
                    size_t su = 0;
                    if (vt)
                    {
                        uintptr_t* table = reinterpret_cast<uintptr_t*>(vt);
                        for (unsigned i = 0; i < 48; ++i)
                        {
                            uintptr_t fn = table[i];
                            unsigned long long rva =
                                (exeBase && fn >= exeBase && fn < exeBase + 0x2000000ULL)
                                ? (unsigned long long)(fn - exeBase) : 0ULL;
                            int w = _snprintf_s(
                                slots + su, sizeof(slots) - su, _TRUNCATE,
                                "%s%u=%p(rva=0x%llX)",
                                su ? " | " : "", i,
                                reinterpret_cast<void*>(fn), rva);
                            if (w <= 0) break;
                            su += (size_t)w;
                            if (su >= sizeof(slots) - 128) break;
                        }
                    }

                    uintptr_t q[16] = {};
                    for (unsigned i = 0; i < 16; ++i)
                        q[i] = *reinterpret_cast<uintptr_t*>(state + i * 8);

                    DxvkPathTrace(
                        "V49 VIEWSTATE-OBJECT create=%llu state=%p vtable=%p "
                        "q=[%llX,%llX,%llX,%llX,%llX,%llX,%llX,%llX,"
                        "%llX,%llX,%llX,%llX,%llX,%llX,%llX,%llX] slots=[%s]",
                        v39Create, reinterpret_cast<void*>(v39LeftState),
                        reinterpret_cast<void*>(vt),
                        (unsigned long long)q[0], (unsigned long long)q[1],
                        (unsigned long long)q[2], (unsigned long long)q[3],
                        (unsigned long long)q[4], (unsigned long long)q[5],
                        (unsigned long long)q[6], (unsigned long long)q[7],
                        (unsigned long long)q[8], (unsigned long long)q[9],
                        (unsigned long long)q[10], (unsigned long long)q[11],
                        (unsigned long long)q[12], (unsigned long long)q[13],
                        (unsigned long long)q[14], (unsigned long long)q[15],
                        slots);
                }

                if (false && v39LeftState != 0 && v39RightStateBefore == 0)
                {
                    *reinterpret_cast<uintptr_t*>(r + 0x08) = v39LeftState;
                    v39Patched = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                v39Patched = false;
            }
        }

        if (v39Create <= 24 || (v39Create % 120ULL) == 0)
        {
            DxvkPathTrace(
                "V39 VIEWSTATE-PATCH create=%llu patched=%d L=%p Rbefore=%p sourceL=%p sourceR=%p",
                v39Create, v39Patched ? 1 : 0,
                reinterpret_cast<void*>(v39LeftState),
                reinterpret_cast<void*>(v39RightStateBefore),
                srcViews[0], srcViews[1]);
        }

        // Emit contiguous differing ranges and pointer-sized differences.
        // Camera-heavy regions are tagged to ignore them when selecting
        // decal candidates; nothing is patched in this build.
        if (v146RightOnlyCtor && g_v151GenuineLeftView && g_v146GenuineRightView)
        {
            static unsigned int v152DiffLogs = 0;
            if (v152DiffLogs++ < 4)
            {
                __try
                {
                    const auto* l =
                        static_cast<const unsigned char*>(g_v151GenuineLeftView);
                    const auto* r =
                        static_cast<const unsigned char*>(g_v146GenuineRightView);

                    char ranges[8192] = {};
                    size_t used = 0;
                    unsigned rangeCount = 0;
                    unsigned changedBytes = 0;

                    for (unsigned off = 0; off < 0x610; )
                    {
                        if (l[off] == r[off]) { ++off; continue; }

                        const unsigned begin = off;
                        while (off < 0x610 && l[off] != r[off])
                        {
                            ++changedBytes;
                            ++off;
                        }
                        const unsigned finish = off - 1;

                        const bool cameraRegion =
                            !(finish < 0x58 || begin > 0x56F) &&
                            ((begin <= 0x6F && finish >= 0x58) ||
                             (begin <= 0x10F && finish >= 0x90) ||
                             (begin <= 0x36F && finish >= 0x2F0) ||
                             (begin <= 0x56F && finish >= 0x560));

                        int w = _snprintf_s(
                            ranges + used, sizeof(ranges) - used, _TRUNCATE,
                            "%s+%03X..+%03X(%u)%s",
                            used ? " | " : "",
                            begin, finish, finish - begin + 1,
                            cameraRegion ? "[CAM]" : "");
                        if (w <= 0) break;
                        used += static_cast<size_t>(w);
                        ++rangeCount;
                        if (used >= sizeof(ranges) - 128) break;
                    }

                    DxvkPathTrace(
                        "V152 FSV_DIFF ranges=%u changedBytes=%u L=%p R=%p [%s]",
                        rangeCount, changedBytes,
                        g_v151GenuineLeftView, g_v146GenuineRightView, ranges);

                    // Pointer/qword census is useful for opaque engine-owned
                    // references that differ between genuine eyes.
                    char qdiff[8192] = {};
                    size_t qused = 0;
                    unsigned qcount = 0;
                    for (unsigned off = 0; off + 8 <= 0x610; off += 8)
                    {
                        const unsigned long long lv =
                            *reinterpret_cast<const unsigned long long*>(l + off);
                        const unsigned long long rv =
                            *reinterpret_cast<const unsigned long long*>(r + off);
                        if (lv == rv) continue;

                        int w = _snprintf_s(
                            qdiff + qused, sizeof(qdiff) - qused, _TRUNCATE,
                            "%s+%03X:%llX>%llX",
                            qused ? " | " : "", off, lv, rv);
                        if (w <= 0) break;
                        qused += static_cast<size_t>(w);
                        ++qcount;
                        if (qused >= sizeof(qdiff) - 160) break;
                    }

                    DxvkPathTrace(
                        "V152 FSV_QDIFF count=%u [%s]", qcount, qdiff);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    DxvkPathTrace("V152 FSV_DIFF exception");
                }
            }
        }

        const unsigned long long result = g_originalSceneRendererCreate
            ? g_originalSceneRendererCreate(a1,a2,a3,a4,a5,a6,a7,a8) : 0;

        if (v146RightOnlyCtor && a2)
        {
            __try
            {
                auto* fam = reinterpret_cast<unsigned char*>(a2);
                *reinterpret_cast<void***>(fam + 0x00) = v146SavedFamilyData;
                *reinterpret_cast<int*>(fam + 0x08) = v146SavedFamilyCount;
                *reinterpret_cast<int*>(fam + 0x0C) = v146SavedFamilyMax;

                // 0x81C9C0 saw exactly the genuine RIGHT source produced by
                // CalcSceneView, so renderer #2 owns one genuine RIGHT FViewInfo.
                static unsigned int v146CtorLogs = 0;
                if (v146CtorLogs++ < 24)
                    DxvkPathTrace(
                        "V154 GENUINE_RIGHT_CTOR renderer=%p family=%p rightView=%p snap=%llu restoredCount=%d passiveDiff=1",
                        reinterpret_cast<void*>(static_cast<uintptr_t>(result)),
                        reinterpret_cast<void*>(a2),
                        g_v146GenuineRightView,
                        (unsigned long long)g_renderPoseSnapshotSerial,
                        v146SavedFamilyCount);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // the renderer constructor owns/copies both views now. Restore the game's
        // original one-view family immediately; our stack clone must never escape.
        if (bl1Expanded && a2)
        {
            __try
            {
                auto* fam = reinterpret_cast<unsigned char*>(a2);
                *reinterpret_cast<void***>(fam + 0x00) = bl1SavedFamilyData;
                *reinterpret_cast<int*>(fam + 0x08) = bl1SavedFamilyCount;
                *reinterpret_cast<int*>(fam + 0x0C) = bl1SavedFamilyMax;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // the renderer has now consumed/copied the source views. Put RIGHT back
        // exactly as it was so no later source-view user inherits this test pointer.
        if (v39Patched && srcViews[1])
        {
            __try
            {
                auto* r = static_cast<unsigned char*>(srcViews[1]);
                *reinterpret_cast<uintptr_t*>(r + 0x08) = v39RightStateBefore;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        void* renderer = reinterpret_cast<void*>(result);

        // constructed from the family positively identified by the gameplay
        if (renderer && a2 &&
            reinterpret_cast<void*>(a2) == g_v136GameplayViewFamily)
        {
            g_v136GameplayRenderer = renderer;
            static unsigned int matchLogs = 0;
            if (matchLogs++ < 24)
                DxvkPathTrace(
                    "V139 GAMEPLAY_RENDERER matched family=%p renderer=%p familySerial=%llu snap=%llu",
                    reinterpret_cast<void*>(a2), renderer,
                    (unsigned long long)g_v136GameplayFamilySerial,
                    (unsigned long long)g_renderPoseSnapshotSerial);
        }

        void* viewArray = nullptr;
        int viewCount = 0;
        if (renderer)
        {
            unsigned char* r = static_cast<unsigned char*>(renderer);
            memcpy(&viewArray,r+0x6c,sizeof(viewArray));
            memcpy(&viewCount,r+0x74,sizeof(viewCount));
        }

        //   renderer +0x6C/+0x74 -> FViewInfo array/count, stride 0x1400
        //   FViewInfo +0x1140+pass -> early gate
        //   FViewInfo +0x604/+0x614 -> primitive visibility bitset helper
        //   FViewInfo +0x644       -> per-primitive packed visibility flags
        //   FViewInfo +0x6D4       -> helper input
        //   FViewInfo +(0x704/0x70C)+16*pass
        //             +(0x744/0x74C)+16*pass
        //             +(0x784/0x78C)+16*pass -> three pass-specific ptr/count lists
        //   FViewInfo +0x13FC      -> key passed into renderer scene-state lookup
        // Caller 0x825F30 additionally checks +0x113C+pass.
        //
        // StereoLite Data3 intra-renderer transplant.
        //
        // LEFT completed render/visibility/pass state while leaving the low
        // camera/viewport/matrix area and ScreenScaleBias (+0x560..+0x56F)
        // genuinely RIGHT. StereoLite already has both completed FViewInfos
        // in this renderer, so copy directly view0 -> view1.
        if (bl1Expanded && viewArray && viewCount >= 2)
        {
            __try
            {
                constexpr size_t kStereoLiteFViewInfoStride = 0x1400;
                auto* stereoLeft = static_cast<unsigned char*>(viewArray);
                auto* stereoRight = stereoLeft + kStereoLiteFViewInfoStride;

                memcpy(stereoRight + 0x570,
                       stereoLeft + 0x570,
                       0x604 - 0x570);
                memcpy(stereoRight + 0x604,
                       stereoLeft + 0x604,
                       0x1400 - 0x604);

                static unsigned int stereoLiteData3Logs = 0;
                if (stereoLiteData3Logs++ < 16)
                    DxvkPathTrace(
                        "STEREOLITE_DATA3 intra-renderer L=%p R=%p "
                        "copied=+0570..+13FF preserve<=+056F snap=%llu",
                        stereoLeft, stereoRight,
                        (unsigned long long)g_renderPoseSnapshotSerial);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DxvkPathTrace("STEREOLITE_DATA3 exception");
            }
        }

        // Capture these completed-renderer regions from LEFT renderer #1 and
        // compare against genuine RIGHT renderer #2. Passive only.
        if (viewArray && viewCount > 0 &&
            !g_nativeStereoEnabled && g_alternatingStereoEnabled &&
            a2 && reinterpret_cast<void*>(a2) == g_v136GameplayViewFamily)
        {
            __try
            {
                auto* fv = static_cast<unsigned char*>(viewArray);

                if (!g_v137InsideSecondProducer && !v146RightOnlyCtor)
                {
                    memcpy(g_v153Left600_7FF, fv + 0x600, sizeof(g_v153Left600_7FF));
                    memcpy(g_v153Left1130_117F, fv + 0x1130, sizeof(g_v153Left1130_117F));
                    memcpy(g_v153Left13F0_13FF, fv + 0x13F0, sizeof(g_v153Left13F0_13FF));
                    memcpy(g_leftData3CompletedLeft, fv, sizeof(g_leftData3CompletedLeft));
                    g_v153LeftDecalPoseSerial = g_renderPoseSnapshotSerial;
                    g_v153LeftDecalStateValid = true;
                }
                else if (v146RightOnlyCtor &&
                         g_v153LeftDecalStateValid &&
                         g_v153LeftDecalPoseSerial == g_renderPoseSnapshotSerial)
                {
                    // LEFT's completed visibility-state window only.
                    //
                    // keep RIGHT's genuine CalcSceneView, constructor, camera,
                    // viewport, matrices, ViewState and all other FViewInfo state.
                    // in +0x604..+0x6E3, so transplant exactly that 0xE0-byte
                    // window after 0x81C9C0 has completed.
                    // through the rest of the completed visibility/pass-state block,
                    // and also carry the two decal gates/key neighborhoods identified
                    // by static RE.  Deliberately leave camera, viewport, matrices and
                    // ScreenScaleBias untouched.
                    // LEFT-DATA-3: keep the exact genuine-RIGHT construction that
                    // restored objects, then inherit LEFT's completed render/decal
                    // state from +0x604 through the end of FViewInfo.  We intentionally
                    // do NOT touch the low portion containing viewport geometry,
                    // View/Projection matrices or ScreenScaleBias (+0x560).
                    // the narrow +0x570..+0x603 gap.  Static layout work places
                    // ScreenScaleBias at +0x560..+0x56F, while the known
                    // visibility/decal-adjacent tail begins at +0x604.  This
                    // deliberately leaves camera, viewport, matrices, and
                    // ScreenScaleBias untouched.
                    memcpy(fv + 0x570,
                           g_leftData3CompletedLeft + 0x570,
                           0x604 - 0x570);

                    memcpy(fv + 0x604,
                           g_leftData3CompletedLeft + 0x604,
                           0x1400 - 0x604);

                    static unsigned int v154PatchLogs = 0;
                    if (v154PatchLogs++ < 12)
                    {
                        DxvkPathTrace(
                            "LEFT-DATA-3 applied renderer=%p fvi=%p "
                            "range=+0604..+13FF bytes=%u snap=%llu",
                            renderer, fv,
                            (unsigned)(0x1400 - 0x604),
                            (unsigned long long)g_renderPoseSnapshotSerial);
                    }

                    static unsigned int v153Logs = 0;
                    if (v153Logs++ < 6)
                    {
                        char out[8192] = {};
                        size_t used = 0;
                        unsigned ranges = 0;
                        unsigned changed = 0;

                        auto emitDiff = [&](unsigned base,
                                            const unsigned char* left,
                                            const unsigned char* right,
                                            unsigned len)
                        {
                            for (unsigned i = 0; i < len; )
                            {
                                if (left[i] == right[i]) { ++i; continue; }
                                const unsigned begin = i;
                                while (i < len && left[i] != right[i])
                                {
                                    ++changed;
                                    ++i;
                                }
                                const unsigned finish = i - 1;
                                int w = _snprintf_s(
                                    out + used, sizeof(out) - used, _TRUNCATE,
                                    "%s+%04X..+%04X(%u)",
                                    used ? " | " : "",
                                    base + begin, base + finish,
                                    finish - begin + 1);
                                if (w > 0)
                                {
                                    used += static_cast<size_t>(w);
                                    ++ranges;
                                }
                            }
                        };

                        emitDiff(0x600, g_v153Left600_7FF, fv + 0x600, 0x200);
                        emitDiff(0x1130, g_v153Left1130_117F, fv + 0x1130, 0x50);
                        emitDiff(0x13F0, g_v153Left13F0_13FF, fv + 0x13F0, 0x10);

                        DxvkPathTrace(
                            "V153 DECAL_FVI_DIFF ranges=%u changedBytes=%u snap=%llu Lserial=%llu [%s]",
                            ranges, changed,
                            (unsigned long long)g_renderPoseSnapshotSerial,
                            (unsigned long long)g_v153LeftDecalPoseSerial,
                            out);

                        for (int pass = 0; pass < 4; ++pass)
                        {
                            const unsigned o704 = 0x704 + pass * 0x10;
                            const unsigned o744 = 0x744 + pass * 0x10;
                            const unsigned o784 = 0x784 + pass * 0x10;

                            const auto lq = [&](unsigned off) -> unsigned long long {
                                return *reinterpret_cast<const unsigned long long*>(
                                    g_v153Left600_7FF + (off - 0x600));
                            };
                            const auto rq = [&](unsigned off) -> unsigned long long {
                                return *reinterpret_cast<const unsigned long long*>(fv + off);
                            };
                            const auto li = [&](unsigned off) -> int {
                                return *reinterpret_cast<const int*>(
                                    g_v153Left600_7FF + (off - 0x600));
                            };
                            const auto ri = [&](unsigned off) -> int {
                                return *reinterpret_cast<const int*>(fv + off);
                            };

                            const unsigned char l113c = g_v153Left1130_117F[0x0C + pass];
                            const unsigned char r113c = *(fv + 0x113C + pass);
                            const unsigned char l1140 = g_v153Left1130_117F[0x10 + pass];
                            const unsigned char r1140 = *(fv + 0x1140 + pass);

                            DxvkPathTrace(
                                "V153 DECAL_PASS p=%d gate113C=%02X>%02X gate1140=%02X>%02X "
                                "A704=%llX/%d>%llX/%d A744=%llX/%d>%llX/%d A784=%llX/%d>%llX/%d",
                                pass,
                                (unsigned)l113c, (unsigned)r113c,
                                (unsigned)l1140, (unsigned)r1140,
                                lq(o704), li(o704+8), rq(o704), ri(o704+8),
                                lq(o744), li(o744+8), rq(o744), ri(o744+8),
                                lq(o784), li(o784+8), rq(o784), ri(o784+8));
                        }

                        const auto l644 =
                            *reinterpret_cast<const unsigned long long*>(
                                g_v153Left600_7FF + 0x44);
                        const auto r644 =
                            *reinterpret_cast<const unsigned long long*>(fv + 0x644);
                        const auto l6d4 =
                            *reinterpret_cast<const unsigned long long*>(
                                g_v153Left600_7FF + 0xD4);
                        const auto r6d4 =
                            *reinterpret_cast<const unsigned long long*>(fv + 0x6D4);
                        const auto l13fc =
                            *reinterpret_cast<const unsigned int*>(
                                g_v153Left13F0_13FF + 0x0C);
                        const auto r13fc =
                            *reinterpret_cast<const unsigned int*>(fv + 0x13FC);

                        DxvkPathTrace(
                            "V153 DECAL_CORE 644=%llX>%llX 6D4=%llX>%llX 13FC=%08X>%08X",
                            l644, r644, l6d4, r6d4,
                            (unsigned)l13fc, (unsigned)r13fc);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DxvkPathTrace("V153 DECAL_FVI_DIFF exception");
            }
        }

        // already proved there are two; this tells us whether the same
        // left-non-null/right-null state survives the copy.
        if ((v39Create <= 24 || (v39Create % 120ULL) == 0) &&
            viewArray && viewCount >= 2)
        {
            __try
            {
                // FSceneRenderer stores its FViewInfo array contiguously. The
                // exact stride is private, so don't guess it here. Log the
                // renderer/view-array identity; SOURCE-VIEWS is the important
                // structural comparison for this pass.
                DxvkPathTrace(
                    "V38 RENDERER create=%llu renderer=%p viewArray=%p views=%d sourceL=%p sourceR=%p",
                    v39Create, renderer, viewArray, viewCount, srcViews[0], srcViews[1]);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        // Static mapping from this exact AHiT EXE's FSceneRenderer constructor:
        //   FViewInfo stride       = 0x1400
        //   pixel rect            = +0x70 {X,Y,W,H}
        //   ScreenScaleBias       = +0x560 (constructor computes it from source
        //                           viewport/pixel state at 0x81CD90..0x81CF47)
        //   ViewMatrix            = +0x90
        //   ProjectionMatrix      = +0xD0
        //   ViewProjection        = +0x2F0
        //   InvViewProjection     = +0x330
        //   derived/frustum block = +0x370...
        //
        // keep pixel rect + ScreenScaleBias coherent for both
        // simultaneous eyes. Before changing more camera math, prove whether our
        // AHiT renderer-owned views actually have that same state.
        // frame (ipdUU was 0.0), so it did not describe the active renderer.
        if (bl1Expanded && viewArray && viewCount >= 2 &&
            g_renderPoseSnapshotValid)
        {
            static LONG v11AuditOnce = 0;
            if (InterlockedCompareExchange(&v11AuditOnce, 1, 0) == 0)
            {
                __try
                {
                    constexpr size_t kStride = 0x1400;
                    auto* l = static_cast<unsigned char*>(viewArray);
                    auto* r = l + kStride;
                    const int* lp = reinterpret_cast<const int*>(l + 0x70);
                    const int* rp = reinterpret_cast<const int*>(r + 0x70);
                    const float* ls = reinterpret_cast<const float*>(l + 0x560);
                    const float* rs = reinterpret_cast<const float*>(r + 0x560);
                    const float* lv = reinterpret_cast<const float*>(l + 0x90);
                    const float* rv = reinterpret_cast<const float*>(r + 0x90);
                    const float* lproj = reinterpret_cast<const float*>(l + 0xD0);
                    const float* rproj = reinterpret_cast<const float*>(r + 0xD0);

                    DxvkPathTrace(
                        "BL1_V11 GAMEPLAY_FVIEW L=%p R=%p stride=0x1400 "
                        "Lpx=[%d,%d,%d,%d] Rpx=[%d,%d,%d,%d] "
                        "Lssb=[%.6f,%.6f,%.6f,%.6f] Rssb=[%.6f,%.6f,%.6f,%.6f] "
                        "LviewT=[%.5f,%.5f,%.5f] RviewT=[%.5f,%.5f,%.5f] "
                        "Lproj=[%.6f,%.6f,%.6f,%.6f] Rproj=[%.6f,%.6f,%.6f,%.6f]",
                        l, r,
                        lp[0],lp[1],lp[2],lp[3],
                        rp[0],rp[1],rp[2],rp[3],
                        ls[0],ls[1],ls[2],ls[3],
                        rs[0],rs[1],rs[2],rs[3],
                        lv[12],lv[13],lv[14],
                        rv[12],rv[13],rv[14],
                        lproj[0],lproj[5],lproj[8],lproj[9],
                        rproj[0],rproj[5],rproj[8],rproj[9]);

                    // Audit the native derived state too. These are exactly the
                    // regions rebuilt by AHiT 0x80A190.
                    const float* lvp = reinterpret_cast<const float*>(l + 0x2F0);
                    const float* rvp = reinterpret_cast<const float*>(r + 0x2F0);
                    const float* lfr = reinterpret_cast<const float*>(l + 0x370);
                    const float* rfr = reinterpret_cast<const float*>(r + 0x370);
                    DxvkPathTrace(
                        "BL1_V11 GAMEPLAY_DERIVED LvpT=[%.5f,%.5f,%.5f,%.5f] "
                        "RvpT=[%.5f,%.5f,%.5f,%.5f] "
                        "Lfr0=[%.5f,%.5f,%.5f,%.5f] "
                        "Rfr0=[%.5f,%.5f,%.5f,%.5f]",
                        lvp[12],lvp[13],lvp[14],lvp[15],
                        rvp[12],rvp[13],rvp[14],rvp[15],
                        lfr[0],lfr[1],lfr[2],lfr[3],
                        rfr[0],rfr[1],rfr[2],rfr[3]);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    DxvkPathTrace("BL1_V11 GAMEPLAY_FVIEW AUDIT EXCEPTION");
                }
            }
        }

        g_lastCreatedSceneRenderer = renderer;
        g_lastCreatedSceneViewArray = viewArray;
        g_lastCreatedSceneViewCount = viewCount;
        ++g_sceneRendererCreateSerial;
        return result;
    }
    static unsigned long long HookSceneExecute(void* command)
    {

        // Pair this with PIPE_CAMERA/PHASE/PIPE_PRESENT in the same log.
        LARGE_INTEGER v73Enter{};
        QueryPerformanceCounter(&v73Enter);
        const unsigned long long v73CamAtEnter = g_v72CameraSerial;
        const unsigned long long v73PresentAtEnter =
            (unsigned long long)g_presentFrameNumber;
        {
            char line[320] = {};
            sprintf_s(line, sizeof(line),
                "PIPE_EXEC stage=ENTER camSerial=%llu present=%llu command=%p qpc=%lld tid=%lu\n",
                v73CamAtEnter, v73PresentAtEnter, command,
                (long long)v73Enter.QuadPart, (unsigned long)GetCurrentThreadId());
            RenderPipelineDiagnosticLog(line);
        }

        // then match renderer + exact viewArray to the producer record.
        SceneProducerRecord v78Hit{};
        bool v78Found = false;
        void* v78Renderer = nullptr;
        void* v78ViewArray = nullptr;
        int v78ViewCount = 0;
        if (command)
        {
            unsigned char* c = static_cast<unsigned char*>(command);
            v78Renderer = *reinterpret_cast<void**>(c + 8);
            if (v78Renderer)
            {
                unsigned char* r = static_cast<unsigned char*>(v78Renderer);
                memcpy(&v78ViewArray,r+0x6c,sizeof(v78ViewArray));
                memcpy(&v78ViewCount,r+0x74,sizeof(v78ViewCount));
            }
            unsigned long long best = 0;
            for (int i=0;i<64;++i)
            {
                const SceneProducerRecord candidate = g_sceneProducerHistory[i];
                if (candidate.producerSerial > best &&
                    candidate.renderer == v78Renderer && candidate.viewArray == v78ViewArray)
                {
                    best = candidate.producerSerial;
                    v78Hit = candidate;
                    v78Found = true;
                }
            }
        }
        const unsigned long long v78Ex =
            (unsigned long long)InterlockedIncrement64(&g_sceneExecuteSerial);
        if (v78Found)
        {
            InterlockedExchange64(&g_lastExecutedScenePoseSerial,(LONG64)v78Hit.poseSerial);
            InterlockedExchange64(&g_lastExecutedSceneProducerSerial,(LONG64)v78Hit.producerSerial);
            InterlockedExchange64(&g_lastExecutedSceneCameraSerial,(LONG64)v78Hit.cameraSerial);
            InterlockedExchange64(&g_lastExecutedScenePresentAtProduce,(LONG64)v78Hit.presentAtProduce);
            InterlockedExchange64(&g_lastExecutedSceneQpc,(LONG64)v73Enter.QuadPart);

            // before later camera activity can clear g_renderPoseSnapshotValid.
            const unsigned long long v83Pose = v78Hit.poseSerial;
            const RenderPoseHistorySlot& v83Slot = g_renderPoseHistory[v83Pose & 63ULL];
            InterlockedExchange(&g_rendererBoundViewsValid, 0);
            if (v83Pose != 0 && v83Slot.serial == v83Pose)
            {
                g_rendererBoundViews[0] = v83Slot.views[0];
                g_rendererBoundViews[1] = v83Slot.views[1];
                InterlockedExchange64(&g_rendererBoundPoseSerial, (LONG64)v83Pose);
                InterlockedExchange64(&g_rendererBoundProducerSerial, (LONG64)v78Hit.producerSerial);
                MemoryBarrier();
                InterlockedExchange(&g_rendererBoundViewsValid, 1);
            }
        }
        if (v78Ex <= 32 || (v78Ex % 120ULL) == 0 || !v78Found)
        {
            static LARGE_INTEGER v78Freq{};
            if (!v78Freq.QuadPart) QueryPerformanceFrequency(&v78Freq);
            const double ageMs = (v78Found && v78Freq.QuadPart)
                ? 1000.0 * double(v73Enter.QuadPart-v78Hit.producerQpc)/double(v78Freq.QuadPart) : -1.0;
            char line[640] = {};
            sprintf_s(line,sizeof(line),
                "V78_EXEC ex=%llu match=%d producer=%llu cam=%llu pose=%llu producePresent=%llu nowPresent=%llu renderer=%p viewArray=%p views=%d ageMs=%.3f qpc=%lld tid=%lu\n",
                v78Ex,v78Found?1:0,v78Found?v78Hit.producerSerial:0,
                v78Found?v78Hit.cameraSerial:0,v78Found?v78Hit.poseSerial:0,
                v78Found?v78Hit.presentAtProduce:0,(unsigned long long)g_presentFrameNumber,
                v78Renderer,v78ViewArray,v78ViewCount,ageMs,(long long)v73Enter.QuadPart,
                (unsigned long)GetCurrentThreadId());
            RenderPipelineDiagnosticLog(line);
        }

        const unsigned long long result =
            g_originalSceneExecute ? g_originalSceneExecute(command) : 0;

        LARGE_INTEGER v73Exit{};
        QueryPerformanceCounter(&v73Exit);
        {
            char line[320] = {};
            sprintf_s(line, sizeof(line),
                "PIPE_EXEC stage=EXIT camSerial=%llu present=%llu command=%p qpc=%lld tid=%lu\n",
                v73CamAtEnter, (unsigned long long)g_presentFrameNumber, command,
                (long long)v73Exit.QuadPart, (unsigned long)GetCurrentThreadId());
            RenderPipelineDiagnosticLog(line);
        }
        return result;
    }
    // Static RE from HatinTimeGame.exe:
    //   0x7FAED0 allocates 0x3C0 bytes (alignment 8), then calls 0x7F21E0.
    //   ULocalPlayer initialization at return RVA 0x5D264E stores that result at +0xF0.
    // these hooks DO NOT create, replace, or destroy any state yet.
    using V50ViewStateFactoryFn = uintptr_t(__fastcall*)();
    using V50ViewStateCtorFn = uintptr_t(__fastcall*)(uintptr_t);
    static V50ViewStateFactoryFn g_originalV50ViewStateFactory = nullptr;
    static V50ViewStateCtorFn g_originalV50ViewStateCtor = nullptr;
    static volatile LONG64 g_v50FactoryCalls = 0;
    static volatile LONG64 g_v50CtorCalls = 0;

    // Intentionally retained for the process lifetime for this path; do not
    // guess at destruction/ownership until the native teardown path is traced.
    static uintptr_t g_v51RightViewState = 0;
    static volatile LONG g_v51RightViewStateInit = 0;

    // at +0x110..+0x118. Our native Stereo reuses LocalPlayer 0, so preserve a
    static bool g_r257RightCameraHistoryValid = false;
    static unsigned char g_r257RightCameraHistory[12] = {};

    static uintptr_t V51GetOrCreateRightViewState()
    {
        uintptr_t state = g_v51RightViewState;
        if (state)
            return state;

        // CalcSceneView is expected on the game thread. Guard anyway so the
        // factory cannot accidentally be invoked twice.
        if (InterlockedCompareExchange(&g_v51RightViewStateInit, 1, 0) != 0)
            return g_v51RightViewState;

        if (g_originalV50ViewStateFactory)
        {
            state = g_originalV50ViewStateFactory();
            g_v51RightViewState = state;

            uintptr_t vt = 0;
            if (state)
                vt = *reinterpret_cast<uintptr_t*>(state);

            DxvkPathTrace(
                "V51 RIGHT-VIEWSTATE CREATE state=%p vtable=%p tid=%lu",
                reinterpret_cast<void*>(state),
                reinterpret_cast<void*>(vt),
                (unsigned long)GetCurrentThreadId());
        }
        else
        {
            DxvkPathTrace("V51 RIGHT-VIEWSTATE CREATE FAILED factory=null");
        }

        InterlockedExchange(&g_v51RightViewStateInit, 2);
        return g_v51RightViewState;
    }

    static uintptr_t __fastcall HookV50ViewStateCtor(uintptr_t self)
    {
        const LONG64 n = InterlockedIncrement64(&g_v50CtorCalls);
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t retRva = (base && ret >= base) ? (ret - base) : 0;

        const uintptr_t result =
            g_originalV50ViewStateCtor ? g_originalV50ViewStateCtor(self) : 0;

        uintptr_t vt = 0;
        if (result)
            vt = *reinterpret_cast<uintptr_t*>(result);

        if (n <= 32)
            DxvkPathTrace(
                "V50 VIEWSTATE-CTOR n=%lld self=%p result=%p vtable=%p retRva=0x%llX tid=%lu",
                (long long)n,
                reinterpret_cast<void*>(self),
                reinterpret_cast<void*>(result),
                reinterpret_cast<void*>(vt),
                (unsigned long long)retRva,
                (unsigned long)GetCurrentThreadId());

        return result;
    }

    static uintptr_t __fastcall HookV50ViewStateFactory()
    {
        const LONG64 n = InterlockedIncrement64(&g_v50FactoryCalls);
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t retRva = (base && ret >= base) ? (ret - base) : 0;

        const uintptr_t result =
            g_originalV50ViewStateFactory ? g_originalV50ViewStateFactory() : 0;

        uintptr_t vt = 0;
        if (result)
            vt = *reinterpret_cast<uintptr_t*>(result);

        if (n <= 32)
            DxvkPathTrace(
                "V50 VIEWSTATE-FACTORY n=%lld result=%p vtable=%p retRva=0x%llX tid=%lu",
                (long long)n,
                reinterpret_cast<void*>(result),
                reinterpret_cast<void*>(vt),
                (unsigned long long)retRva,
                (unsigned long)GetCurrentThreadId());

        return result;
    }

    //
    // test calling FSceneRenderer::Render (0x825840) twice without re-entering gameplay Draw.
    // re-entering at 0x5D7DF0 froze SteamVR.
    using V135SceneRenderFn = void(__fastcall*)(void*);
    static V135SceneRenderFn g_originalV135SceneRender = nullptr;
    static volatile LONG64 g_v135SceneRenderCalls = 0;
    static volatile LONG64 g_v135SecondRenders = 0;
    static thread_local bool g_v135InsideSecondRender = false;

    static void __fastcall HookV135SceneRender(void* renderer)
    {
        if (!g_originalV135SceneRender)
            return;

        const LONG64 call = InterlockedIncrement64(&g_v135SceneRenderCalls);

        // ordinary renderer behavior.
        // gameplay renderer through the engine's normal ownership path.
        const bool probe = false;

        if (!probe)
        {
            g_originalV135SceneRender(renderer);
            return;
        }

        // Consume this exact renderer match now.  If UE3 later reuses the
        // address, it must first be correlated with a new gameplay family again.
        g_v136GameplayRenderer = nullptr;

        // First execution is the exact renderer execution normal AFR would have
        // performed.
        g_originalV135SceneRender(renderer);

        // Re-enter ONLY FSceneRenderer::Render.  We intentionally do not call
        // CalcSceneView, LocalPlayer::Draw, Canvas/HUD, scene producer, or XR
        // begin/snapshot a second time.
        g_v135InsideSecondRender = true;
        const LONG64 second = InterlockedIncrement64(&g_v135SecondRenders);

        if (second <= 24 || (second % 600) == 0)
            DxvkPathTrace(
                "V136 GAMEPLAY_RENDER_REENTRY begin call=%lld second=%lld renderer=%p present=%llu eye=%s snap=%llu",
                (long long)call,
                (long long)second,
                renderer,
                (unsigned long long)g_presentFrameNumber,
                g_renderRightEye ? "RIGHT" : "LEFT",
                (unsigned long long)g_renderPoseSnapshotSerial);

        g_originalV135SceneRender(renderer);

        if (second <= 24 || (second % 600) == 0)
            DxvkPathTrace(
                "V136 GAMEPLAY_RENDER_REENTRY end second=%lld renderer=%p present=%llu",
                (long long)second,
                renderer,
                (unsigned long long)g_presentFrameNumber);

        g_v135InsideSecondRender = false;
    }

    static bool InstallV67WorkCounterHooks()
    {
        HMODULE exe=GetModuleHandleW(nullptr); if(!exe) return false;
        struct H { uintptr_t rva; void* hook; void** original; };
        H hooks[]={
            {0x5D7DF0,(void*)&HookV26GameplayDraw,(void**)&g_originalV26GameplayDraw},
            {0x81E7A0,(void*)&HookSceneProducer,(void**)&g_originalSceneProducer},
            {0x81C9C0,(void*)&HookSceneRendererCreate,(void**)&g_originalSceneRendererCreate},
            {0x825840,(void*)&HookV135SceneRender,(void**)&g_originalV135SceneRender},
            {0x825F30,(void*)&HookV180PerViewDecal,(void**)&g_originalV180PerViewDecal},
            {0x7E3850,(void*)&HookV186DetailRelevance,(void**)&g_originalV186DetailRelevance},
            {0x820400,(void*)&HookSceneExecute,(void**)&g_originalSceneExecute},
            {0x8334E0,(void*)&HookV40PpSetup,(void**)&g_originalV40PpSetup},
            {0xA25F40,(void*)&HookV45RhiSetViewport,(void**)&g_originalV45RhiSetViewport},
            {0x7FFDC0,(void*)&HookV46DrawQuad,(void**)&g_originalV46DrawQuad},
            {0x7FAED0,(void*)&HookV50ViewStateFactory,(void**)&g_originalV50ViewStateFactory},
            {0x7F21E0,(void*)&HookV50ViewStateCtor,(void**)&g_originalV50ViewStateCtor}
        };
        bool ok=true;
        for(const auto& h:hooks){
            void* target=(void*)((uintptr_t)exe+h.rva);
            MH_STATUS st=MH_CreateHook(target,h.hook,h.original);
            if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){ok=false;continue;}
            st=MH_EnableHook(target);
            if(st!=MH_OK && st!=MH_ERROR_ENABLED) ok=false;
        }
        V67CounterLog(ok ? "V186 StereoLite DetailShadow force relevance bits6..12 test + Data3 + exact RIGHT decal V80\n" : "V67 counter hook install had failure\n");
        return ok;
    }

