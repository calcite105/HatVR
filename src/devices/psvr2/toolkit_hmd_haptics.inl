    // PSVR2 Toolkit CAPI headset-rumble bridge.  The CAPI is optional: HatVR
    // dynamically discovers it and leaves every existing controller path alone
    // when it is absent.  Current PSVR2Toolkit exposes HMD vibration as a byte
    // frequency (0 stops, 1..25 Hz vibrates), not a conventional amplitude.
    using HatVrPsvr2InitFn = int (__cdecl*)();
    using HatVrPsvr2DeinitFn = void (__cdecl*)();
    using HatVrPsvr2SetHmdRumbleFn = int (__cdecl*)(unsigned char);
    using HatVrPsvr2GetDriverActiveFn = bool (__cdecl*)();
    using HatVrPsvr2GazeStatusFn = bool (__cdecl*)(void* gazeStatus, uint32_t timeoutMs);

    // PSVR2 Toolkit adaptive-trigger ABI. ScePadTriggerEffectCommand is 56 bytes:
    // mode at +0, command-data union at +8. Weapon mode uses the first three
    // command bytes as start position, end position, and strength.
    struct HatVrPsvr2TriggerCommand
    {
        int32_t mode = 0;
        uint32_t reserved = 0;
        unsigned char commandData[48]{};
    };
    static_assert(sizeof(HatVrPsvr2TriggerCommand) == 56, "PSVR2 trigger ABI mismatch");
    using HatVrPsvr2SetTriggerEffectFn = int (__cdecl*)(int controllerType, const HatVrPsvr2TriggerCommand& command);

    static HMODULE g_psvr2CapiModule = nullptr;
    static HatVrPsvr2InitFn g_psvr2Init = nullptr;
    static HatVrPsvr2DeinitFn g_psvr2Deinit = nullptr;
    static HatVrPsvr2SetHmdRumbleFn g_psvr2SetHmdRumble = nullptr;
    static HatVrPsvr2GetDriverActiveFn g_psvr2GetDriverActive = nullptr;
    static HatVrPsvr2GazeStatusFn g_psvr2GazeStatus = nullptr;
    static HatVrPsvr2SetTriggerEffectFn g_psvr2SetTriggerEffect = nullptr;
    static bool g_psvr2CapiTried = false;
    static bool g_psvr2CapiConnected = false;
    static unsigned char g_psvr2LastHmdHz = 0xFF;

    static bool HatVrFileExistsA(const char* path)
    {
        if (!path || !*path) return false;
        const DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    static bool HatVrFindPsvr2Capi(char* outPath, size_t outBytes)
    {
        if (!outPath || outBytes < MAX_PATH) return false;
        outPath[0] = 0;

        // PSVR2Toolkit v1 writes a one-line CAPI directory hint in %TEMP%.
        char temp[MAX_PATH] = {};
        if (GetEnvironmentVariableA("TEMP", temp, MAX_PATH) > 0)
        {
            char hint[MAX_PATH] = {};
            _snprintf_s(hint, sizeof(hint), _TRUNCATE, "%s\\psvr2tk_capi_path.txt", temp);
            FILE* f = nullptr;
            if (fopen_s(&f, hint, "rb") == 0 && f)
            {
                char dir[MAX_PATH] = {};
                if (fgets(dir, MAX_PATH, f))
                {
                    for (char* q = dir; *q; ++q)
                        if (*q == '\r' || *q == '\n') { *q = 0; break; }
                    _snprintf_s(outPath, outBytes, _TRUNCATE,
                        "%s\\psvr2_toolkit_capi.dll", dir);
                    fclose(f);
                    if (HatVrFileExistsA(outPath)) return true;
                }
                else fclose(f);
            }
        }

        // Standard Windows install location from PSVR2Toolkit's installation guide.
        char pf86[MAX_PATH] = {};
        if (GetEnvironmentVariableA("ProgramFiles(x86)", pf86, MAX_PATH) > 0)
        {
            _snprintf_s(outPath, outBytes, _TRUNCATE,
                "%s\\Steam\\steamapps\\common\\PlayStation VR2 App\\SteamVR_Plug-In\\bin\\win64\\psvr2_toolkit_capi.dll",
                pf86);
            if (HatVrFileExistsA(outPath)) return true;
        }
        outPath[0] = 0;
        return false;
    }

    static bool EnsureHatVrPsvr2Capi()
    {
        if (g_psvr2CapiConnected && g_psvr2SetHmdRumble) return true;
        if (g_psvr2CapiTried) return false;
        g_psvr2CapiTried = true;

        char path[MAX_PATH] = {};
        if (!HatVrFindPsvr2Capi(path, sizeof(path)))
        {
            LogCategory("HAPTIC", "PSVR2 CAPI not found; HMD rumble unavailable");
            return false;
        }
        g_psvr2CapiModule = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_psvr2CapiModule)
        {
            LogCategory("HAPTIC", "PSVR2 CAPI LoadLibrary failed err=%lu", GetLastError());
            return false;
        }
        g_psvr2Init = reinterpret_cast<HatVrPsvr2InitFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_init"));
        g_psvr2Deinit = reinterpret_cast<HatVrPsvr2DeinitFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_deinit"));
        g_psvr2SetHmdRumble = reinterpret_cast<HatVrPsvr2SetHmdRumbleFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_set_hmd_rumble"));
        g_psvr2GetDriverActive = reinterpret_cast<HatVrPsvr2GetDriverActiveFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_get_driver_active"));
        g_psvr2GazeStatus = reinterpret_cast<HatVrPsvr2GazeStatusFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_gaze_status"));
        g_psvr2SetTriggerEffect = reinterpret_cast<HatVrPsvr2SetTriggerEffectFn>(GetProcAddress(g_psvr2CapiModule, "psvr2_toolkit_set_trigger_effect"));
        if (!g_psvr2Init || !g_psvr2Deinit || !g_psvr2SetHmdRumble)
        {
            LogCategory("HAPTIC", "PSVR2 CAPI missing required HMD-rumble exports");
            FreeLibrary(g_psvr2CapiModule); g_psvr2CapiModule = nullptr;
            return false;
        }
        const int initResult = g_psvr2Init();
        if (initResult == -2)
        {
            LogCategory("HAPTIC", "PSVR2 CAPI has no free client slot");
            return false;
        }
        g_psvr2CapiConnected = true;
        LogCategory("HAPTIC", "PSVR2 HMD rumble READY init=%d driverActive=%d",
            initResult,
            g_psvr2GetDriverActive ? (g_psvr2GetDriverActive() ? 1 : 0) : -1);
        return true;
    }

    static void HatVrSetPsvr2RightHookshotTrigger(bool enabled)
    {
        // Adaptive triggers are a normal Toolkit controller feature; unlike HMD
        // rumble they do not depend on the headset jailbreak state.
        if (!EnsureHatVrPsvr2Capi() || !g_psvr2SetTriggerEffect)
            return;

        HatVrPsvr2TriggerCommand cmd{};
        if (enabled)
        {
            // SCE_PAD_TRIGGER_EFFECT_MODE_WEAPON = 2. Unlike Feedback mode's
            // continuous resistance, Weapon mode creates a defined resistant band
            // that gives the trigger a much sharper wall/break sensation. Keep the
            // band fairly short and strong so the Hookshot feels clicky rather than
            // mushy while Right Grip is held.
            cmd.mode = 2;
            cmd.commandData[0] = 2; // start position 2..7
            cmd.commandData[1] = 5; // end position: start+1 .. 8
            cmd.commandData[2] = 8; // strength 0..8
        }
        // mode 0 is OFF. Controller type 1 is Right in PSVR2Toolkit CAPI.
        g_psvr2SetTriggerEffect(1, cmd);
    }

    static void HatVrSendPsvr2HmdHz(unsigned char hz)
    {
        if (hz > 25) hz = 25;
        if (hz == g_psvr2LastHmdHz) return;
        if (!EnsureHatVrPsvr2Capi()) return;
        g_psvr2LastHmdHz = hz;
        g_psvr2SetHmdRumble(hz);
    }

    // Source-aware PSVR2 HMD haptics.
    //
    // Static RE of this AHiT executable identifies UObject::ProcessInternal at
    // RVA 0xE2860.  At that boundary every UnrealScript function has a real
    // FFrame, including a PreviousFrame pointer, so we can distinguish e.g.
    // Hat_Player.OnDamageTaken -> PlayGenericForceFeedback from UI code that
    // happens to request the same motor strength.
    //
    // AHiT x64 FFrame offsets, confirmed against ProcessInternal's machine code:
    //   +0x14 Node (UFunction*)
    //   +0x2C Locals
    //   +0x34 PreviousFrame
    using HatVrProcessInternalFn = void(__fastcall*)(void*, void*, void*);
    static HatVrProcessInternalFn g_hapticOriginalProcessInternal = nullptr;
    static bool g_hapticProcessInternalHookInstalled = false;

    static ULONGLONG g_psvr2SourceHapticUntilMs = 0;
    static unsigned char g_psvr2SourceHapticHz = 0;

    static int32_t g_hnPlayGeneric = -1;
    static int32_t g_hnClientPlay = -1;
    static int32_t g_hnOnDamageTaken = -1;
    static int32_t g_hnStartWallSlide = -1;
    static int32_t g_hnWallSlideJump = -1;
    static int32_t g_hnLanded = -1;
    static int32_t g_hnWallHardImpact = -1;
    static int32_t g_hnHardImpactLand = -1;
    static int32_t g_hnDoSpringJump = -1;
    static int32_t g_hnDoJumpDive = -1;
    static int32_t g_hnCameraShakePostProcess = -1;
    static int32_t g_hnOnHitWall = -1;
    static int32_t g_hnOnShootComplete = -1;
    static int32_t g_hnTakeDamage = -1;
    static int32_t g_hnNotifyTakeHit = -1;
    static int32_t g_hnDoForceFeedbackForScreenShake = -1;
    static int32_t g_hnCameraShakeBigShort = -1;
    static int32_t g_hnCameraShakeBigLong = -1;
    static int32_t g_hnCameraShakeMediumShort = -1;
    static int32_t g_hnCameraShakeMediumLong = -1;
    static int32_t g_hnSetCamShakeScale = -1;
    static int32_t g_hnReinitShake = -1;
    static ULONGLONG g_psvr2ShakeHapticUntilMs = 0;
    static unsigned char g_psvr2ShakeHapticHz = 0;

    static bool HatVrReadFramePtr(uintptr_t frame, uintptr_t off, uintptr_t& out)
    {
        out = 0;
        return FP_V111PlausiblePtr(frame) &&
            FP_V18ReadPtr(frame + off, out);
    }

    static int32_t HatVrFrameNodeName(uintptr_t frame)
    {
        uintptr_t node = 0;
        int32_t name = -1;
        if (!HatVrReadFramePtr(frame, 0x14, node) ||
            !FP_V111PlausiblePtr(node) ||
            !FP_V123ObjectRawNameIndex(node, name))
            return -1;
        return name;
    }

    static void HatVrMixPsvr2Haptics()
    {
        if (!g_psvr2HmdRumbleEnabled)
        {
            g_psvr2SourceHapticUntilMs = 0; g_psvr2SourceHapticHz = 0;
            g_psvr2ShakeHapticUntilMs = 0;  g_psvr2ShakeHapticHz = 0;
            HatVrSendPsvr2HmdHz(0);
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (g_psvr2SourceHapticUntilMs && now >= g_psvr2SourceHapticUntilMs)
        { g_psvr2SourceHapticUntilMs = 0; g_psvr2SourceHapticHz = 0; }
        if (g_psvr2ShakeHapticUntilMs && now >= g_psvr2ShakeHapticUntilMs)
        { g_psvr2ShakeHapticUntilMs = 0; g_psvr2ShakeHapticHz = 0; }
        HatVrSendPsvr2HmdHz((std::max)(g_psvr2SourceHapticHz,g_psvr2ShakeHapticHz));
    }

    static int HatVrMapPsvr2HmdIntensity(int hz)
    {
        hz=(std::max)(0,(std::min)(25,hz));
        if(hz==0 || g_psvr2HmdRumbleIntensity>=100) return hz;
        const float x=float(hz)/25.0f;
        const float amount=(std::max)(0,(std::min)(100,g_psvr2HmdRumbleIntensity))/100.0f;
        float multiplier=amount;
        // Curve only changes how reductions below 100% are distributed.
        // 0 Linear: uniform scaling.
        // 1 Preserve Low: weak feedback is reduced less, strong feedback more.
        // 2 Reduce Low: weak feedback is reduced more, strong feedback less.
        const float bend=0.5f*(1.0f-amount);
        if(g_psvr2HmdRumbleCurve==1) multiplier += bend*(1.0f-2.0f*x);
        else if(g_psvr2HmdRumbleCurve==2) multiplier += bend*(2.0f*x-1.0f);
        multiplier=(std::max)(0.0f,(std::min)(1.0f,multiplier));
        const int out=static_cast<int>(x*multiplier*25.0f+0.5f);
        return (std::max)(0,out);
    }

    static void HatVrTriggerPsvr2SourceHaptic(int hz, unsigned durationMs)
    {
        if (!g_psvr2HmdRumbleEnabled || !durationMs) return;
        hz=HatVrMapPsvr2HmdIntensity(hz);
        if(hz<=0) return;
        const ULONGLONG now=GetTickCount64();
        if (g_psvr2SourceHapticUntilMs>now &&
            g_psvr2SourceHapticHz>static_cast<unsigned char>(hz)) return;
        g_psvr2SourceHapticHz=static_cast<unsigned char>(hz);
        g_psvr2SourceHapticUntilMs=now+durationMs;
        HatVrMixPsvr2Haptics();
    }

    static void HatVrSetPsvr2CameraShake(float scale)
    {
        if (!g_psvr2HmdRumbleEnabled || !isfinite(scale)) return;
        scale=(std::max)(0.0f,(std::min)(1.0f,scale));
        constexpr float kDead=0.03f;
        if (scale<=kDead)
        {
            g_psvr2ShakeHapticUntilMs=0; g_psvr2ShakeHapticHz=0;
            HatVrMixPsvr2Haptics(); return;
        }
        // PSVR2Toolkit's CAPI parameter is literally rumble frequency in Hz:
        // 0 stops the HMD motor and the official test UI exposes 0..25 Hz.
        // Convert the game's normalized ActiveShake Scale directly into that
        // range rather than treating the byte as an amplitude value.
        const float n=(scale-kDead)/(1.0f-kDead);
        const int hz=HatVrMapPsvr2HmdIntensity(static_cast<int>(n*25.0f+0.5f));
        if(hz<=0)
        {
            g_psvr2ShakeHapticUntilMs=0; g_psvr2ShakeHapticHz=0;
            HatVrMixPsvr2Haptics(); return;
        }
        g_psvr2ShakeHapticHz=static_cast<unsigned char>((std::min)(25,hz));
        // Continuous writers refresh this. The timeout prevents a stale buzz.
        g_psvr2ShakeHapticUntilMs=GetTickCount64()+140;
        HatVrMixPsvr2Haptics();
    }

    static void HatVrRefreshLateLoadedHapticNames()
    {
        // DLC packages (notably Nyakuza Metro) are not necessarily loaded when
        // HatVR installs the VM hook, so their FNames may not exist yet.
        // Retry only once per second and stop permanently for each name once
        // found; this keeps the normal per-frame path essentially free.
        static ULONGLONG nextRetryMs = 0;
        if (g_hnSetCamShakeScale >= 0 && g_hnReinitShake >= 0)
            return;

        const ULONGLONG now = GetTickCount64();
        if (now < nextRetryMs)
            return;
        nextRetryMs = now + 1000;

        if (g_hnSetCamShakeScale < 0)
            FP_V123FindRawNameIndex("SetCamShakeScale", g_hnSetCamShakeScale);
        if (g_hnReinitShake < 0)
            FP_V123FindRawNameIndex("ReinitShake", g_hnReinitShake);

        if (g_hnSetCamShakeScale >= 0)
            LogCategory("HAPTIC", "late-loaded SetCamShakeScale resolved name=%d",
                g_hnSetCamShakeScale);
    }

    static void HatVrUpdatePsvr2SourceHaptic()
    {
        HatVrRefreshLateLoadedHapticNames();
        HatVrMixPsvr2Haptics();
    }

    static bool HatVrReadScriptFloat(uintptr_t frame, uintptr_t off, float& out)
    {
        uintptr_t locals=0; out=0.0f;
        if (!HatVrReadFramePtr(frame,0x2C,locals) || !FP_V111PlausiblePtr(locals))
            return false;
        __try {
            memcpy(&out,reinterpret_cast<const void*>(locals+off),sizeof(float));
            return isfinite(out);
        } __except(EXCEPTION_EXECUTE_HANDLER) { out=0.0f; return false; }
    }

    static void HatVrRouteGenericHaptic(uintptr_t frame, int32_t callerName)
    {
        uintptr_t locals = 0;
        float amount = 0.0f, seconds = 0.0f;
        if (HatVrReadFramePtr(frame, 0x2C, locals) &&
            FP_V111PlausiblePtr(locals))
        {
            __try {
                memcpy(&amount, reinterpret_cast<const void*>(locals + 0), 4);
                memcpy(&seconds, reinterpret_cast<const void*>(locals + 4), 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                amount = 0.0f;
                seconds = 0.0f;
            }
        }

        // Chosen for HMD/body sensation, not controller-rumble strength.
        if (callerName == g_hnOnDamageTaken)
            HatVrTriggerPsvr2SourceHaptic(25, 200);
        else if (callerName == g_hnWallHardImpact)
            HatVrTriggerPsvr2SourceHaptic(25, 300);
        else if (callerName == g_hnOnHitWall)
            HatVrTriggerPsvr2SourceHaptic(25, 300);
        else if (callerName == g_hnHardImpactLand)
            HatVrTriggerPsvr2SourceHaptic(17, 150);
        else if (callerName == g_hnDoSpringJump)
            HatVrTriggerPsvr2SourceHaptic(19, 300);
        else if (callerName == g_hnOnShootComplete)
            HatVrTriggerPsvr2SourceHaptic(19, 200);
        else if (callerName == g_hnDoJumpDive)
            HatVrTriggerPsvr2SourceHaptic(14, 180);
        else if (callerName == g_hnWallSlideJump)
            HatVrTriggerPsvr2SourceHaptic(16, 180);
        else if (callerName == g_hnStartWallSlide)
            HatVrTriggerPsvr2SourceHaptic(10, 300);
        else if (callerName == g_hnLanded)
            HatVrTriggerPsvr2SourceHaptic(9, 90);
        else if (callerName == g_hnCameraShakePostProcess)
        {
            // This function computes generic FF amount from shake strength.
            // Preserve that useful magnitude but use the HMD's assertive range.
            float normalized = amount / 100.0f;
            normalized = (std::max)(0.0f, (std::min)(1.0f, normalized));
            const int hz = 10 + static_cast<int>(normalized * 15.0f);
            unsigned ms = 120;
            if (seconds > 0.0f && isfinite(seconds))
                ms = static_cast<unsigned>((std::max)(0.08f,
                     (std::min)(1.5f, seconds)) * 1000.0f);
            HatVrTriggerPsvr2SourceHaptic(hz, ms);
        }
        // Deliberately ignored: umbrella attack, collectibles, ledge release,
        // Hat Wheel/menu/HUD feedback and the Hat_HUDElement forwarding helper.
    }

    static void HatVrRouteWaveformHaptic(uintptr_t frame, int32_t callerName)
    {
        if (callerName == g_hnTakeDamage)
        {
            // Pawn.TakeDamage only requests KilledFFWaveform in its death path.
            HatVrTriggerPsvr2SourceHaptic(25, 350);
            return;
        }
        if (callerName == g_hnNotifyTakeHit)
        {
            HatVrTriggerPsvr2SourceHaptic(24, 220);
            return;
        }
        if (callerName != g_hnDoForceFeedbackForScreenShake)
            return; // sequence/animation/UI waveform: do not guess.

        // GamePlayerController chooses one of four named camera-shake waveforms.
        uintptr_t locals = 0, waveform = 0;
        if (!HatVrReadFramePtr(frame, 0x2C, locals) ||
            !FP_V111PlausiblePtr(locals) ||
            !FP_V18ReadPtr(locals, waveform) ||
            !FP_V111PlausiblePtr(waveform))
            return;

        int32_t waveName = -1;
        if (!FP_V123ObjectRawNameIndex(waveform, waveName))
            return;

        if (waveName == g_hnCameraShakeBigShort)
            HatVrTriggerPsvr2SourceHaptic(25, 500);
        else if (waveName == g_hnCameraShakeBigLong)
            HatVrTriggerPsvr2SourceHaptic(25, 1500);
        else if (waveName == g_hnCameraShakeMediumShort)
            HatVrTriggerPsvr2SourceHaptic(18, 500);
        else if (waveName == g_hnCameraShakeMediumLong)
            HatVrTriggerPsvr2SourceHaptic(18, 1500);
    }

    static void __fastcall HatVrHookedProcessInternal(
        void* object, void* stackPtr, void* result)
    {
        const uintptr_t frame = reinterpret_cast<uintptr_t>(stackPtr);
        const int32_t calleeName = HatVrFrameNodeName(frame);

        if (calleeName == g_hnPlayGeneric || calleeName == g_hnClientPlay)
        {
            uintptr_t previous = 0;
            int32_t callerName = -1;
            if (HatVrReadFramePtr(frame, 0x34, previous) &&
                FP_V111PlausiblePtr(previous))
                callerName = HatVrFrameNodeName(previous);

            if (calleeName == g_hnPlayGeneric)
                HatVrRouteGenericHaptic(frame, callerName);
            else
                HatVrRouteWaveformHaptic(frame, callerName);
        }
        else if (calleeName == g_hnSetCamShakeScale)
        {
            // Metro train and any other content using this helper: third
            // script parameter is the live 0..1 ActiveShake scale.
            float scale=0.0f;
            if (HatVrReadScriptFloat(frame,16,scale))
                HatVrSetPsvr2CameraShake(scale);
        }
        else if (calleeName == g_hnReinitShake)
        {
            // Generic Engine CameraModifier_CameraShake scale path:
            // ReinitShake(int ActiveShakeIdx, float Scale).
            float scale=0.0f;
            if (HatVrReadScriptFloat(frame,4,scale))
                HatVrSetPsvr2CameraShake(scale);
        }

        if (g_hapticOriginalProcessInternal)
            g_hapticOriginalProcessInternal(object, stackPtr, result);
    }

    static void InstallUE3SourceAwareHaptics()
    {
        if (g_hapticProcessInternalHookInstalled)
            return;

        FP_V123FindRawNameIndex("PlayGenericForceFeedback", g_hnPlayGeneric);
        FP_V123FindRawNameIndex("ClientPlayForceFeedbackWaveform", g_hnClientPlay);
        FP_V123FindRawNameIndex("OnDamageTaken", g_hnOnDamageTaken);
        FP_V123FindRawNameIndex("StartWallSlide", g_hnStartWallSlide);
        FP_V123FindRawNameIndex("WallSlideJump", g_hnWallSlideJump);
        FP_V123FindRawNameIndex("Landed", g_hnLanded);
        FP_V123FindRawNameIndex("WallHardImpact", g_hnWallHardImpact);
        FP_V123FindRawNameIndex("HardImpactLand", g_hnHardImpactLand);
        FP_V123FindRawNameIndex("DoSpringJump", g_hnDoSpringJump);
        FP_V123FindRawNameIndex("DoJumpDive", g_hnDoJumpDive);
        FP_V123FindRawNameIndex("CameraShakePostProcess", g_hnCameraShakePostProcess);
        FP_V123FindRawNameIndex("OnHitWall", g_hnOnHitWall);
        FP_V123FindRawNameIndex("OnShootComplete", g_hnOnShootComplete);
        FP_V123FindRawNameIndex("TakeDamage", g_hnTakeDamage);
        FP_V123FindRawNameIndex("NotifyTakeHit", g_hnNotifyTakeHit);
        FP_V123FindRawNameIndex("DoForceFeedbackForScreenShake", g_hnDoForceFeedbackForScreenShake);
        FP_V123FindRawNameIndex("CameraShakeBigShort", g_hnCameraShakeBigShort);
        FP_V123FindRawNameIndex("CameraShakeBigLong", g_hnCameraShakeBigLong);
        FP_V123FindRawNameIndex("CameraShakeMediumShort", g_hnCameraShakeMediumShort);
        FP_V123FindRawNameIndex("CameraShakeMediumLong", g_hnCameraShakeMediumLong);
        FP_V123FindRawNameIndex("SetCamShakeScale", g_hnSetCamShakeScale);
        FP_V123FindRawNameIndex("ReinitShake", g_hnReinitShake);

        auto* exe = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!exe)
            return;

        // HatinTimeGame.exe 2026-09-25:
        // UObject::ProcessInternal = 0x1400E2330 (RVA 0xE2860).
        // The signature is confirmed by the EX_Return (0x04) bytecode loop and
        // GNatives dispatch table at 0x141148C50.
        void* target = exe + 0xE2860;
        MH_STATUS st = MH_CreateHook(
            target,
            reinterpret_cast<void*>(&HatVrHookedProcessInternal),
            reinterpret_cast<void**>(&g_hapticOriginalProcessInternal));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
        {
            LogCategory("HAPTIC", "UE3 source-aware haptic hook create failed status=%d", int(st));
            return;
        }

        st = MH_EnableHook(target);
        if (st == MH_OK || st == MH_ERROR_ENABLED)
        {
            g_hapticProcessInternalHookInstalled = true;
            LogCategory("HAPTIC",
                "UE3 source-aware HMD haptics ACTIVE ProcessInternalRVA=0xE2860");
        }
        else
        {
            LogCategory("HAPTIC", "UE3 source-aware haptic hook enable failed status=%d", int(st));
        }
    }

