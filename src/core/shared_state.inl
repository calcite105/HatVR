    // AHiT D3D9 focus window. Used to keep retained debug hotkeys foreground-only.
    static HWND g_hatVrGameWindow = nullptr;

    // old DebugLog calls still land in the normal session log.
    #define DebugLog(text) LogCategory("DEBUG", "%s", (text))
    // Function types

    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9* self,
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface
        );

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9* self,
        const RECT* pSourceRect,
        const RECT* pDestRect,
        HWND hDestWindowOverride,
        const RGNDATA* pDirtyRegion
        );

    using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9* self,
        IDirect3DVertexShader9* pShader
        );

    using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9* self,
        D3DPRIMITIVETYPE Type,
        INT BaseVertexIndex,
        UINT MinVertexIndex,
        UINT NumVertices,
        UINT StartIndex,
        UINT PrimitiveCount
        );

    using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
        const void*, D3DFORMAT, const void*, UINT);
    using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

    // native ULocalPlayer::CalcSceneView candidate / debug camera

    // Ghidra: HatinTimeGame.exe image base 0x140000000, candidate at 0x1405D4870.
    static constexpr uintptr_t kCalcSceneViewCandidateRva = 0x5D4390;

    // the decompiler showed six arguments:
    //   (ULocalPlayer*, FSceneViewFamily*, FVector*, FRotator*, FViewport*, FViewElementDrawer*)
    // the exact semantic names are still provisional, but x64 ABI/argument count are sufficient
    // for this controlled hook test.
    struct FVectorUE3
    {
        float X;
        float Y;
        float Z;
    };

    struct FRotatorUE3
    {
        int Pitch;
        int Yaw;
        int Roll;
    };

    // Confirmed from the function prologue:
    // RCX=this, RDX=ViewFamily, R8=&ViewLocation, R9=&ViewRotation,
    // stack arg 5=Viewport, stack arg 6=ViewDrawer.
    using CalcSceneViewCandidateFn = unsigned long long (*)(
        void* localPlayer,
        void* viewFamily,
        FVectorUE3* viewLocation,
        FRotatorUE3* viewRotation,
        void* viewport,
        void* viewDrawer
        );

    static CalcSceneViewCandidateFn g_originalCalcSceneViewCandidate = nullptr;
    static bool g_calcSceneViewCandidateHookInstalled = false;

    // Retained early-development free-camera state. Defined once in
    // vulkan_backend.cpp so the debug menu can display it while the D3D9 hook
    // continues to own the actual camera manipulation.
    extern float g_cameraLocalForward;
    extern float g_cameraLocalRight;
    extern float g_cameraLocalUp;

    // Alternating-eye stereo proof.
    //
    // this is intentionally a stepping stone: AHiT still renders once per
    // Present, but successive game renders use opposite eye offsets. We retain
    // the most recent LEFT and RIGHT completed frames and composite them SBS.
    // that gives us real geometric parallax immediately, while we locate the
    // UE3 render entry point needed to issue both eyes in the same game frame.
    // native same-frame stereo: CalcSceneView is invoked twice into the SAME
    // FSceneViewFamily, once per half of the LocalPlayer viewport.
    static bool g_alternatingStereoEnabled = true; // retained as master stereo switch
    static bool g_nativeStereoEnabled = true;
    // verified patches make GameViewportClient::Draw iterate twice over
    // GamePlayers[0] without changing the actual GamePlayers array/count.
    static bool g_nativeRenderLoopStereoActive = false;
    static constexpr uintptr_t kViewportLoopAdvanceRva = 0x5D8BC9;
    static constexpr uintptr_t kViewportLoopLimitRva   = 0x5D8BD1;
    static bool SetNativeRenderLoopStereoPatch(bool enable);
    static bool g_renderRightEye = false; // current eye while CalcSceneView is building a view
    // Game-thread-only eye identity for CalcSceneView/camera construction.
    // keep this separate from g_renderRightEye: D3D9 capture/presentation paths
    static thread_local bool g_calcSceneRightEye = false;
    // 0 normal
    // 1 RIGHT uses LEFT camera/eye origin only
    // 2 RIGHT uses LEFT projection/FOV only
    // 3 RIGHT uses both LEFT camera and LEFT projection
    // 4 both eyes use a symmetric camera-local baseline from runtime IPD
    //   (real stereo depth, no independently-derived right-eye origin)
    static int g_rightEyeDiagnosticMode = 0;
    // 0 = independent persistent RIGHT ViewState (current behavior)
    // 1 = RIGHT ViewState nullptr
    // 2 = RIGHT shares LEFT ViewState
    static int g_rightEyeViewStateDiagnosticMode = 0;
    // When frame rate collapses, positional head translation baked into the UE3
    // camera can feel much worse than rotational timewarp.  Keep full 6DoF render
    // translation from the UE3 camera while retaining rotational tracking.
    static bool g_rotationOnlyTracking = false;

    // HatVR in-headset menu + stereo Theater Mode. Y opens/closes the menu;
    // on a fixed OpenXR quad instead of as an immersive projection layer.
    static bool g_vrMenuOpen = false;
    static bool g_theaterMode = false;

    // Camera compatibility policy.
    // Auto Theater follows HatVR's global AHiT camera-ownership bridge. When AHiT
    // leaves its normal gameplay ApplyCameraModes path, HatVR can temporarily use
    // Theater Mode. This applies in both third person and first person; any active
    // VR avatar is suspended and prior user state is restored afterward.
    // these are fallback defaults only. HatVR.ini overrides them at startup.
    static bool g_autoTheaterCutscenes = false;
    static bool g_overrideLockedGameplayCameras = false;
    // presentation-only controller glyph style. Input remains Xbox/XInput.
    static bool g_playStationControllerIcons = false;
    // presentation-only Nintendo/Switch glyph style. Input remains Xbox/XInput.
    static bool g_nintendoSwitchControllerIcons = false;
    // User-facing convenience/settings added with the simple ImGui menu.
    static bool g_disablePlayerFade = true;
    static bool g_rightHandHookshot = false;
    static bool g_umbrellaMotionControls = true;
    // Optional r261 direct OpenXR eye projection. Native Stereo only.
    // Defined by vulkan_backend.cpp because Vulkan submission also consumes it.
    extern bool g_sharperNativeStereo;
    // vulkan_backend.cpp so the menu renderer and D3D9 hook TU share one state.
    extern bool g_debugToolsEnabled;
    // User-facing menu font. Defined by vulkan_backend.cpp so the renderer and
    // D3D9-side menu/config code share the same live setting.
    extern bool g_hatVrCurseCasualMenuFont;
    // Optional Workshop-facing HatVR Mod API. Off by default.
    extern bool g_hatVrModApiEnabled;
    static int  g_vrMenuPage = 0;
    static float g_hatVrHudScale = 1.0f;
    static float g_hatVrHudDistance = 0.65f;
    static float g_hatVrHudHeight = 0.0f;
    static bool g_hatVrHudHeadLocked = false;
    // Desktop spectator output. 0 Off, 1 Left, 2 Right, 3 Both.
    // UI mode: 0 Hidden, 1 Overlay. (Accurate runtime composition intentionally omitted.)
    static int g_spectatorView = 2;
    static int g_spectatorUiMode = 1;
    extern bool g_spectatorExpandedFov;

    // Implemented by config.inl. Menu changes persist immediately.
    static void SaveHatVrConfig();

    // Global AHiT camera ownership bridge.
    //
    // the native ApplyCameraModes hook increments this serial every time AHiT's
    // normal gameplay camera pipeline actually executes. camera_hooks.inl snapshots
    // the serial around ORIGINAL GetPlayerViewPoint, so each camera query can tell
    // whether it passed through gameplay ApplyCameraModes without depending on
    // First Person, camera priorities, POV-distance heuristics, or stale heartbeats.
    static volatile LONG g_gameplayCameraApplySerial = 0;
    static bool g_hatVrGameCameraActive = false; // true while AHiT bypasses gameplay ApplyCameraModes
    static bool g_hatVrCameraStateInitialized = false;
    static bool g_hatVrCameraSavedTheater = false;
    static bool g_hatVrCameraForcedTheater = false;
    static bool g_hatVrCameraSavedAvatar = false;
    static int  g_hatVrCameraGameplayConfirm = 0;
    static int  g_hatVrCameraGameConfirm = 0;
    // Dedicated OpenXR HatVR menu state. The active UI is a deliberately plain
    // Dear ImGui settings window rendered into its own OpenXR compositor layer.
    // g_vrMenuPage selects Home / VR / Controls / UI-HUD / Spectator / Graphics / Debug Tools and
    // g_vrMenuSelection selects a row within that page. The controller layer
    // owns navigation so the renderer remains presentation-only.
    static int g_vrMenuSelection = 0;
    // false = category/sidebar focus, true = inside selected category.
    static bool g_vrMenuInsideCategory = false;
    bool g_recommendedGraphicsMenuOpen = false; // legacy compatibility; no longer a separate page

    // PSVR2 headset-rumble routing. The first functional pass deliberately
    // accepts only strong game-authored controller feedback. Static source
    // mapping shows AHiT UI feedback tops out below the physical 100-strength
    // without pretending final XInput motor values identify every script source.
    // Owned by vulkan_backend.cpp so both the D3D9/controller TU and the
    // Vulkan menu TU see the same PSVR2 HMD-rumble settings.
    extern bool g_psvr2HmdRumbleEnabled;
    extern bool g_psvr2StrongPhysicalOnly; // legacy config compatibility
    extern bool g_psvr2EyeWheelEnabled;
    extern bool g_psvr2MenuOpen;
    extern int  g_psvr2HmdRumbleIntensity; // 0..100; 100 leaves source intensity unchanged
    extern int  g_psvr2HmdRumbleCurve;     // 0 Linear, 1 Preserve Low, 2 Reduce Low
    extern bool g_psvr2HookshotAdaptiveTrigger;

    // Current AHiT PlayerController observed by the PSVR2 eye-controlled
    // Hat Wheel path. controller_input.inl consumes this as an opaque UE3
    // object pointer; the concrete object layout is handled by that module.
    static void* g_hatVrEyeWheelPlayerController = nullptr;

    // Implemented in first_person.inl after avatar_system.inl is included.
    static void SetUnifiedFirstPersonEnabled(bool enabled);

    static constexpr uintptr_t kGameplayCalcSceneViewReturnRva = 0x5D84FE;
    static float g_stereoHalfIpdUU = 3.2f; // 6.4 UU total at 100 UU / meter

    static IDirect3DTexture9* g_leftEyeCopy = nullptr;
    static IDirect3DTexture9* g_rightEyeCopy = nullptr;
    static UINT g_stereoCopyWidth = 0;

    // OpenXR presentation-only output scaling. 0 = off; 25..200 = enlarge
    // the OpenXR eye images by that percentage without changing AHiT/DXVK render size.
    extern bool g_openXrUpscalingEnabled;
    extern int g_openXrUpscaler;
    extern int g_openXrUpscalePercent;
    extern int g_openXrUpscaleSharpness;
    static UINT g_stereoCopyHeight = 0;
    static D3DFORMAT g_stereoCopyFormat = D3DFMT_UNKNOWN;
    static bool g_haveLeftEye = false;
    static bool g_haveRightEye = false;

    // OpenXR per-eye off-center projection hook. This is functional VR rendering,
    // not the removed scalar game-FOV override.
    static constexpr uintptr_t kPerspectiveMatrixCandidateRva = 0x5D22A0;
    using PerspectiveMatrixCandidateFn = float* (*)(
        float* outMatrix, float halfFovX, float halfFovY,
        float scaleX, float scaleY, float nearZ, float farZ);
    static PerspectiveMatrixCandidateFn g_originalPerspectiveMatrixCandidate = nullptr;
    static bool g_perspectiveMatrixHookInstalled = false;

    // Frame counters used by the stereo/render pipeline.
    static volatile unsigned long long g_presentFrameNumber = 0;

    // Present must treat the SBS texture as a complete pair, not retain one
    // alternating half from a previous engine frame.
    static bool g_v138SameTickAfrActive = false;
