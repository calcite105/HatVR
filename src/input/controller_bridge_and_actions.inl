    static DWORD WINAPI HookedXInputSetState(
        DWORD userIndex, HatVrXInputVibration* vibration)
    {
        // Preserve AHiT's normal controller rumble first.
        const DWORD result = g_originalXInputSetState
            ? g_originalXInputSetState(userIndex, vibration)
            : ERROR_DEVICE_NOT_CONNECTED;

        if (userIndex == 0 && vibration)
        {
            ApplyHatVrControllerHaptics(*vibration);
            // HMD rumble is routed by UnrealScript source at ProcessInternal.
        }
        return result;
    }

    // normal swing profile: 7 rad/s, 40 ms motion, 36 degrees travel, 250 ms cooldown.
    static bool g_motionSwingTracking[2] = { false, false };
    static bool g_motionPrevGripValid[2] = { false, false };
    static XrQuaternionf g_motionPrevGripQ[2] = {
        { 0,0,0,1 }, { 0,0,0,1 }
    };
    static XrQuaternionf g_motionSwingStartQ[2] = {
        { 0,0,0,1 }, { 0,0,0,1 }
    };
    static ULONGLONG g_motionPrevSampleMs[2] = { 0, 0 };
    static ULONGLONG g_motionGoodSinceMs[2] = { 0, 0 };
    static ULONGLONG g_motionLastGoodMs[2] = { 0, 0 };
    static ULONGLONG g_motionAttackPulseUntilMs[2] = { 0, 0 };
    static ULONGLONG g_motionRefractoryUntilMs[2] = { 0, 0 };
    static ULONGLONG g_motionLastLogMs[2] = { 0, 0 };

    static XrQuaternionf MotionNormalizeQ(XrQuaternionf q)
    {
        const float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        if (n > 1.0e-6f) {
            const float inv = 1.0f / n;
            q.x*=inv; q.y*=inv; q.z*=inv; q.w*=inv;
        } else {
            q = {0,0,0,1};
        }
        return q;
    }

    static float MotionQuatAngle(const XrQuaternionf& aIn, const XrQuaternionf& bIn)
    {
        const XrQuaternionf a = MotionNormalizeQ(aIn);
        const XrQuaternionf b = MotionNormalizeQ(bIn);
        float d = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
        d = (std::max)(0.0f, (std::min)(1.0f, d));
        return 2.0f * acosf(d);
    }

    static bool UpdateHandSwingAttack(int hand)
    {
        if (hand < 0 || hand > 1)
            return false;

        const ULONGLONG now = GetTickCount64();

        if (!g_controllerGripPoseValid[hand]) {
            g_motionPrevGripValid[hand] = false;
            g_motionSwingTracking[hand] = false;
            g_motionGoodSinceMs[hand] = 0;
            return now < g_motionAttackPulseUntilMs[hand];
        }

        const XrQuaternionf q =
            MotionNormalizeQ(g_controllerGripLocations[hand].pose.orientation);

        if (!g_motionPrevGripValid[hand]) {
            g_motionPrevGripQ[hand] = q;
            g_motionPrevSampleMs[hand] = now;
            g_motionPrevGripValid[hand] = true;
            return now < g_motionAttackPulseUntilMs[hand];
        }

        ULONGLONG dtMs = now - g_motionPrevSampleMs[hand];
        if (dtMs < 1) dtMs = 1;
        if (dtMs > 100) {
            g_motionPrevGripQ[hand] = q;
            g_motionPrevSampleMs[hand] = now;
            g_motionSwingTracking[hand] = false;
            g_motionGoodSinceMs[hand] = 0;
            return now < g_motionAttackPulseUntilMs[hand];
        }

        const float dt = (float)dtMs * 0.001f;
        const float stepAngle = MotionQuatAngle(g_motionPrevGripQ[hand], q);
        const float angularSpeed = stepAngle / dt;

        g_motionPrevGripQ[hand] = q;
        g_motionPrevSampleMs[hand] = now;

        constexpr float kVelocityGate = 9.5f; // v4.1 stricter swing gate
        constexpr float kTravelGate = 0.7853981634f; // v4.1: 45 degrees
        constexpr ULONGLONG kMinGoodMs = 50; // v4.1: require a little more sustained motion
        constexpr ULONGLONG kGraceMs = 40;
        constexpr ULONGLONG kPulseMs = 80;
        constexpr ULONGLONG kRefractoryMs = 250;

        if (now >= g_motionRefractoryUntilMs[hand]) {
            if (angularSpeed >= kVelocityGate) {
                if (!g_motionSwingTracking[hand]) {
                    g_motionSwingTracking[hand] = true;
                    g_motionSwingStartQ[hand] = q;
                    g_motionGoodSinceMs[hand] = now;
                }
                g_motionLastGoodMs[hand] = now;

                const float travel =
                    MotionQuatAngle(g_motionSwingStartQ[hand], q);
                const ULONGLONG goodMs =
                    now - g_motionGoodSinceMs[hand];

                if (goodMs >= kMinGoodMs && travel >= kTravelGate) {
                    g_motionAttackPulseUntilMs[hand] = now + kPulseMs;
                    g_motionRefractoryUntilMs[hand] = now + kRefractoryMs;
                    g_motionSwingTracking[hand] = false;
                    g_motionGoodSinceMs[hand] = 0;

                    char line[256]{};
                    sprintf_s(line, sizeof(line),
                        "MOTION_ATTACK hand=%c speed=%.2f rad/s travel=%.1f deg good=%llums -> X pulse\n",
                        hand == 0 ? 'L' : 'R',
                        angularSpeed, travel * 57.2957795f,
                        (unsigned long long)goodMs);
                    DebugLog(line);
                }
            } else if (g_motionSwingTracking[hand] &&
                       now - g_motionLastGoodMs[hand] > kGraceMs) {
                g_motionSwingTracking[hand] = false;
                g_motionGoodSinceMs[hand] = 0;
            }
        } else {
            g_motionSwingTracking[hand] = false;
            g_motionGoodSinceMs[hand] = 0;
        }

        if (now - g_motionLastLogMs[hand] >= 500) {
            g_motionLastLogMs[hand] = now;
            char line[256]{};
            sprintf_s(line, sizeof(line),
                "MOTION_SAMPLE hand=%c speed=%.2f rad/s tracking=%d refractory=%llums pulse=%d\n",
                hand == 0 ? 'L' : 'R',
                angularSpeed, g_motionSwingTracking[hand] ? 1 : 0,
                now < g_motionRefractoryUntilMs[hand]
                    ? (unsigned long long)(g_motionRefractoryUntilMs[hand]-now) : 0ULL,
                now < g_motionAttackPulseUntilMs[hand] ? 1 : 0);
            DebugLog(line);
        }

        return now < g_motionAttackPulseUntilMs[hand];
    }

    static bool UpdateSwingAttack()
    {
        const bool leftAttack = UpdateHandSwingAttack(0);
        const bool rightAttack = UpdateHandSwingAttack(1);
        return leftAttack || rightAttack;
    }

    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, HatVrXInputState*);
    static XInputGetStateFn g_originalXInputGetState = nullptr;
    static bool g_xinputHookInstalled = false;

    static XrAction CreateHatVrAction(XrActionType type, const char* name,
        const char* localized, uint32_t subactionCount = 0, const XrPath* subactions = nullptr)
    {
        XrAction action = XR_NULL_HANDLE;
        XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
        ci.actionType = type;
        strncpy_s(ci.actionName, sizeof(ci.actionName), name, _TRUNCATE);
        strncpy_s(ci.localizedActionName, sizeof(ci.localizedActionName), localized, _TRUNCATE);
        ci.countSubactionPaths = subactionCount;
        ci.subactionPaths = subactions;
        if (XR_FAILED(xrCreateAction(g_controllerActionSet, &ci, &action)))
            return XR_NULL_HANDLE;
        return action;
    }

    static void AddBinding(std::vector<XrActionSuggestedBinding>& bindings,
        XrAction action, const char* path)
    {
        XrPath xrPath = XR_NULL_PATH;
        if (action != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrStringToPath(g_xrInstance, path, &xrPath)))
            bindings.push_back({ action, xrPath });
    }

    static void SuggestControllerProfile(const char* profile,
        const std::vector<XrActionSuggestedBinding>& bindings)
    {
        XrPath profilePath = XR_NULL_PATH;
        if (bindings.empty() || XR_FAILED(xrStringToPath(g_xrInstance, profile, &profilePath)))
            return;
        XrInteractionProfileSuggestedBinding info{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        info.interactionProfile = profilePath;
        info.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        info.suggestedBindings = bindings.data();
        xrSuggestInteractionProfileBindings(g_xrInstance, &info);
    }

    // Shared HatVR menu action path. Keyboard, physical XInput pads and OpenXR
    // controllers all feed the same menu state instead of maintaining separate menus.
    static int HatVrMenuPageRows(int page)
    {
        switch(page){case 0:return 1;case 1:return 12;case 2:return 9;case 3:return 8;case 4:return 3;case 5:return 1;case 6:return 6;default:return 1;}
    }

    static void HatVrMenuOpenFromInput(const char* source)
    {
        g_vrMenuOpen=true; g_psvr2MenuOpen=false; g_vrMenuSelection=0; g_vrMenuInsideCategory=false;
        LogCategory("MENU", "VR menu OPEN (%s)", source ? source : "input");
    }

    static void HatVrMenuCloseFromInput(const char* source)
    {
        g_vrMenuOpen=false; g_psvr2MenuOpen=false;
        LogCategory("MENU", "VR menu CLOSED (%s)", source ? source : "input");
    }

    static void HatVrMenuNavigate(bool up, bool down, bool left, bool right, bool accept, bool back)
    {
        if(!g_vrMenuOpen) return;
        if(!g_vrMenuInsideCategory)
        {
            const int pageCount=g_debugToolsEnabled?7:6;
            if(up) g_vrMenuPage=(g_vrMenuPage+pageCount-1)%pageCount;
            if(down) g_vrMenuPage=(g_vrMenuPage+1)%pageCount;
            g_vrMenuSelection=0;
            if(accept){ g_vrMenuInsideCategory=true; g_vrMenuSelection=0; }
            if(back) HatVrMenuCloseFromInput("Back");
            return;
        }
        const int rows=HatVrMenuPageRows(g_vrMenuPage);
        if(g_vrMenuSelection<0 || g_vrMenuSelection>=rows) g_vrMenuSelection=0;
        if(up) g_vrMenuSelection=(g_vrMenuSelection+rows-1)%rows;
        if(down) g_vrMenuSelection=(g_vrMenuSelection+1)%rows;
        const float dir=right?1.0f:(left?-1.0f:0.0f);
        if(dir!=0.0f)
        {
            if(g_vrMenuPage==3 && g_vrMenuSelection==0) g_hatVrHudScale=(std::max)(0.50f,(std::min)(2.00f,g_hatVrHudScale+dir*0.05f));
            else if(g_vrMenuPage==3 && g_vrMenuSelection==1) g_hatVrHudDistance=(std::max)(0.25f,(std::min)(2.00f,g_hatVrHudDistance+dir*0.05f));
            else if(g_vrMenuPage==3 && g_vrMenuSelection==2) g_hatVrHudHeight=(std::max)(-1.00f,(std::min)(1.00f,g_hatVrHudHeight+dir*0.05f));
            else if(g_vrMenuPage==2 && g_vrMenuSelection==5) g_psvr2HmdRumbleIntensity=(std::max)(0,(std::min)(100,g_psvr2HmdRumbleIntensity+(int)dir*5));
            else if(g_vrMenuPage==2 && g_vrMenuSelection==6) g_psvr2HmdRumbleCurve=(g_psvr2HmdRumbleCurve+(dir>0?1:2))%3;
            else if(g_vrMenuPage==1 && g_vrMenuSelection==9) g_openXrUpscaler=(g_openXrUpscaler==0)?1:0;
            else if(g_vrMenuPage==1 && g_vrMenuSelection==10) g_openXrUpscalePercent=(std::max)(25,(std::min)(100,g_openXrUpscalePercent+(dir>0?5:-5)));
            else if(g_vrMenuPage==1 && g_vrMenuSelection==11) g_openXrUpscaleSharpness=(std::max)(0,(std::min)(100,g_openXrUpscaleSharpness+(dir>0?5:-5)));
            SaveHatVrConfig();
        }
        if(accept)
        {
            if(g_vrMenuPage==0) g_xrPositionRecenterPending=true;
            else if(g_vrMenuPage==1)
            {
                // keep the Rendering-page row map explicit.  Sharper Native Stereo was
                // inserted above First Person; using named row IDs prevents this menu
                // from silently dispatching a newly inserted row to an older action.
                enum HatVrRenderingRow
                {
                    kRenderingMode = 0,
                    kSharperNativeStereo = 1,
                    kFirstPerson = 2,
                    kModApi = 3,
                    kDisablePlayerFade = 4,
                    kTheaterMode = 5,
                    kAutoTheaterCutscenes = 6,
                    kOverrideLockedCameras = 7,
                    kUpscaling = 8,
                    kUpscaler = 9,
                    kUpscalePercent = 10,
                    kUpscaleSharpness = 11
                };
                switch(g_vrMenuSelection){
                case kRenderingMode:{const bool requestedStereo=!g_nativeStereoEnabled;if(SetNativeRenderLoopStereoPatch(false))g_nativeStereoEnabled=requestedStereo;SaveHatVrConfig();break;}
                case kSharperNativeStereo:
                {
                    // this row was inserted above First Person. Treat activation as terminal:
                    // one Accept press must never continue into an older/stale menu action.
                    g_sharperNativeStereo=!g_sharperNativeStereo;
                    SaveHatVrConfig();
                    LogCategory("MENU", "Sharper Native Stereo %s (row=%d; FirstPerson=%d)",
                        g_sharperNativeStereo ? "ON" : "OFF", g_vrMenuSelection, g_fpV1Enabled ? 1 : 0);
                    return;
                }
                case kFirstPerson:{const bool enable=!(g_fpV1Enabled&&g_avV29ProbeEnabled);SetUnifiedFirstPersonEnabled(enable);SaveHatVrConfig();break;}
                case kModApi:g_hatVrModApiEnabled=!g_hatVrModApiEnabled;SaveHatVrConfig();break;
                case kDisablePlayerFade:g_disablePlayerFade=!g_disablePlayerFade;SaveHatVrConfig();break; case kTheaterMode:g_theaterMode=!g_theaterMode;SaveHatVrConfig();break;
                case kAutoTheaterCutscenes:g_autoTheaterCutscenes=!g_autoTheaterCutscenes;SaveHatVrConfig();break;
                case kOverrideLockedCameras:{const bool was=g_overrideLockedGameplayCameras;g_overrideLockedGameplayCameras=!g_overrideLockedGameplayCameras;if(was&&!g_overrideLockedGameplayCameras)FP_V116RestoreWorkshopCameraModes();else if(!was&&g_overrideLockedGameplayCameras)g_fpV127WorkshopRemoveApplied=false;SaveHatVrConfig();break;}
                case kUpscaling:g_openXrUpscalingEnabled=!g_openXrUpscalingEnabled;SaveHatVrConfig();break; case kUpscaler:g_openXrUpscaler=(g_openXrUpscaler==0)?1:0;SaveHatVrConfig();break;
                case kUpscalePercent:g_openXrUpscalePercent=g_openXrUpscalePercent>=100?25:g_openXrUpscalePercent+25;SaveHatVrConfig();break; case kUpscaleSharpness:g_openXrUpscaleSharpness=g_openXrUpscaleSharpness>=100?0:g_openXrUpscaleSharpness+10;SaveHatVrConfig();break;}
            }
            else if(g_vrMenuPage==2) switch(g_vrMenuSelection){
                case 0:g_rightHandHookshot=!g_rightHandHookshot;SaveHatVrConfig();break; case 1:g_umbrellaMotionControls=!g_umbrellaMotionControls;SaveHatVrConfig();break;
                case 2:g_playStationControllerIcons=!g_playStationControllerIcons;if(g_playStationControllerIcons)g_nintendoSwitchControllerIcons=false;SaveHatVrConfig();break;
                case 3:g_nintendoSwitchControllerIcons=!g_nintendoSwitchControllerIcons;if(g_nintendoSwitchControllerIcons)g_playStationControllerIcons=false;SaveHatVrConfig();break;
                case 4:g_psvr2HmdRumbleEnabled=!g_psvr2HmdRumbleEnabled;if(!g_psvr2HmdRumbleEnabled)HatVrSendPsvr2HmdHz(0);else EnsureHatVrPsvr2Capi();SaveHatVrConfig();break;
                case 5:break; case 6:g_psvr2HmdRumbleCurve=(g_psvr2HmdRumbleCurve+1)%3;SaveHatVrConfig();break;
                case 7:g_psvr2HookshotAdaptiveTrigger=!g_psvr2HookshotAdaptiveTrigger;if(!g_psvr2HookshotAdaptiveTrigger)HatVrSetPsvr2RightHookshotTrigger(false);SaveHatVrConfig();break; case 8:g_psvr2EyeWheelEnabled=!g_psvr2EyeWheelEnabled;SaveHatVrConfig();break;}
            else if(g_vrMenuPage==3) switch(g_vrMenuSelection){case 3:g_hatVrHudScale=1.0f;SaveHatVrConfig();break;case 4:g_hatVrHudDistance=0.65f;g_hatVrHudHeight=0.0f;SaveHatVrConfig();break;case 5:g_hatVrHudHeadLocked=!g_hatVrHudHeadLocked;SaveHatVrConfig();break;case 6:g_hatVrCurseCasualMenuFont=!g_hatVrCurseCasualMenuFont;SaveHatVrConfig();break;case 7:g_hatVrHudScale=1.0f;g_hatVrHudDistance=0.65f;g_hatVrHudHeight=0.0f;g_hatVrHudHeadLocked=false;g_hatVrCurseCasualMenuFont=true;SaveHatVrConfig();break;}
            else if(g_vrMenuPage==4){if(g_vrMenuSelection==0){g_spectatorView=(g_spectatorView+1)%4;SaveHatVrConfig();}else if(g_vrMenuSelection==1){g_spectatorUiMode=(g_spectatorUiMode+1)%2;SaveHatVrConfig();}else if(g_vrMenuSelection==2&&!g_sharperNativeStereo){g_spectatorExpandedFov=!g_spectatorExpandedFov;SaveHatVrConfig();}}
            else if(g_vrMenuPage==6) switch(g_vrMenuSelection){case 0:UiCandidateSelectDelta(-1);break;case 1:UiCandidateSelectDelta(+1);break;case 2:UiCandidateCyclePreview();break;case 3:UiCandidateMarkGame();break;case 4:UiCandidateMarkUi();break;case 5:UiCandidateClearMark();break;}
        }
        if(back){g_vrMenuInsideCategory=false;g_vrMenuSelection=0;}
    }

    static constexpr ULONGLONG kHatVrMenuDoubleTapMs = 375;
    static bool g_physicalStartWasDown=false, g_physicalStartPending=false;
    static ULONGLONG g_physicalStartFirstTapMs=0;
    static unsigned short g_physicalMenuPrevButtons=0;
    static bool g_physicalMenuStickUp=false,g_physicalMenuStickDown=false,g_physicalMenuStickLeft=false,g_physicalMenuStickRight=false;

    static WNDPROC g_hatVrOriginalWndProc=nullptr;
    static HWND g_hatVrInputWindow=nullptr;
    static bool g_hatVrEscPending=false;
    static ULONGLONG g_hatVrEscFirstTapMs=0;
    static constexpr UINT_PTR kHatVrEscTimerId=0x48565245; // 'HVRE'

    static LRESULT CALLBACK HatVrInputWndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
    {
        if(msg==WM_CLOSE && IsVulkanBackendActive())
        {
            // close OpenXR while the game/rendering code is still alive.
            ShutdownVulkanBackend();
        }
        if(msg==WM_TIMER && wParam==kHatVrEscTimerId && g_hatVrEscPending)
        {
            KillTimer(hwnd,kHatVrEscTimerId); g_hatVrEscPending=false;
            // Deliver the delayed single Escape directly to AHiT's original
            // window procedure, bypassing our own double-tap recognizer.
            if(g_hatVrOriginalWndProc){CallWindowProcW(g_hatVrOriginalWndProc,hwnd,WM_KEYDOWN,VK_ESCAPE,1);CallWindowProcW(g_hatVrOriginalWndProc,hwnd,WM_KEYUP,VK_ESCAPE,0xC0000001);}
            return 0;
        }
        if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN) && !(lParam&(1LL<<30)))
        {
            const int vk=(int)wParam;
            if(g_vrMenuOpen)
            {
                if(vk==VK_ESCAPE){HatVrMenuCloseFromInput("Escape");return 0;}
                const bool up=vk==VK_UP||vk=='W', down=vk==VK_DOWN||vk=='S', left=vk==VK_LEFT||vk=='A', right=vk==VK_RIGHT||vk=='D';
                const bool accept=vk==VK_RETURN||vk==VK_SPACE;
                if(up||down||left||right||accept){HatVrMenuNavigate(up,down,left,right,accept,false);return 0;}
            }
            else if(vk==VK_ESCAPE)
            {
                const ULONGLONG now=GetTickCount64();
                if(g_hatVrEscPending && now-g_hatVrEscFirstTapMs<=kHatVrMenuDoubleTapMs)
                {
                    KillTimer(hwnd,kHatVrEscTimerId);g_hatVrEscPending=false;HatVrMenuOpenFromInput("double Escape");return 0;
                }
                g_hatVrEscPending=true;g_hatVrEscFirstTapMs=now;SetTimer(hwnd,kHatVrEscTimerId,(UINT)kHatVrMenuDoubleTapMs,nullptr);return 0;
            }
        }
        if((msg==WM_KEYUP||msg==WM_SYSKEYUP))
        {
            const int vk=(int)wParam;
            if(vk==VK_ESCAPE && (g_hatVrEscPending||g_vrMenuOpen)) return 0;
            if(g_vrMenuOpen && (vk==VK_UP||vk==VK_DOWN||vk==VK_LEFT||vk==VK_RIGHT||vk=='W'||vk=='A'||vk=='S'||vk=='D'||vk==VK_RETURN||vk==VK_SPACE)) return 0;
        }
        return g_hatVrOriginalWndProc?CallWindowProcW(g_hatVrOriginalWndProc,hwnd,msg,wParam,lParam):DefWindowProcW(hwnd,msg,wParam,lParam);
    }

    static void InstallHatVrKeyboardMenuHook(HWND hwnd)
    {
        if(!hwnd || g_hatVrInputWindow==hwnd) return;
        SetLastError(0);
        LONG_PTR previous=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&HatVrInputWndProc));
        if(previous || GetLastError()==0){g_hatVrInputWindow=hwnd;g_hatVrOriginalWndProc=reinterpret_cast<WNDPROC>(previous);LogCategory("INPUT","HatVR focused keyboard menu hook installed hwnd=%p",hwnd);}
        else LogCategory("INPUT","HatVR keyboard menu hook failed error=%lu",GetLastError());
    }

    static DWORD WINAPI HookedXInputGetState(DWORD userIndex, HatVrXInputState* state)
    {
        DWORD result = g_originalXInputGetState ? g_originalXInputGetState(userIndex, state) : ERROR_DEVICE_NOT_CONNECTED;
        if (!state || userIndex != 0) return result;

        // Physical controller menu ownership works even when OpenXR controller
        // emulation is disabled. A single Start is delayed briefly and passed to
        // AHiT; a second Start inside the window opens HatVR and neither tap leaks.
        if(result==ERROR_SUCCESS)
        {
            const ULONGLONG now=GetTickCount64();
            const bool startDown=(state->Gamepad.wButtons&HV_START)!=0;
            const bool startPressed=startDown&&!g_physicalStartWasDown;
            g_physicalStartWasDown=startDown;

            if(g_vrMenuOpen)
            {
                const unsigned short buttons=state->Gamepad.wButtons;
                const bool a=(buttons&HV_A)!=0, b=(buttons&HV_B)!=0;
                const bool up=(buttons&HV_DPAD_UP)!=0 || state->Gamepad.sThumbLY>18000;
                const bool down=(buttons&HV_DPAD_DOWN)!=0 || state->Gamepad.sThumbLY<-18000;
                const bool left=(buttons&HV_DPAD_LEFT)!=0 || state->Gamepad.sThumbLX<-18000;
                const bool right=(buttons&HV_DPAD_RIGHT)!=0 || state->Gamepad.sThumbLX>18000;
                const bool accept=a && !(g_physicalMenuPrevButtons&HV_A);
                const bool back=b && !(g_physicalMenuPrevButtons&HV_B);
                HatVrMenuNavigate(up&&!g_physicalMenuStickUp,down&&!g_physicalMenuStickDown,left&&!g_physicalMenuStickLeft,right&&!g_physicalMenuStickRight,accept,back);
                if(startPressed) HatVrMenuCloseFromInput("physical Start");
                g_physicalMenuPrevButtons=buttons; g_physicalMenuStickUp=up;g_physicalMenuStickDown=down;g_physicalMenuStickLeft=left;g_physicalMenuStickRight=right;
                ZeroMemory(&state->Gamepad,sizeof(state->Gamepad));
                return ERROR_SUCCESS;
            }

            if(startPressed)
            {
                if(g_physicalStartPending && now-g_physicalStartFirstTapMs<=kHatVrMenuDoubleTapMs)
                {
                    g_physicalStartPending=false; HatVrMenuOpenFromInput("double Start");
                    ZeroMemory(&state->Gamepad,sizeof(state->Gamepad)); return ERROR_SUCCESS;
                }
                g_physicalStartPending=true; g_physicalStartFirstTapMs=now;
            }
            if(g_physicalStartPending)
            {
                state->Gamepad.wButtons &= ~HV_START;
                if(now-g_physicalStartFirstTapMs>kHatVrMenuDoubleTapMs)
                {
                    g_physicalStartPending=false;
                    state->Gamepad.wButtons |= HV_START;
                }
            }
        }

        if (!g_vrControllerInputEnabled || !g_vrControllerActive) return result;
        if (result != ERROR_SUCCESS) ZeroMemory(state, sizeof(*state));
        state->Gamepad.wButtons |= g_vrXInputState.Gamepad.wButtons;
        auto strongerAxis=[](SHORT physical,SHORT vr)->SHORT{const int vm=vr<0?-int(vr):int(vr);const int pm=physical<0?-int(physical):int(physical);return vm>pm?vr:physical;};
        state->Gamepad.sThumbLX=strongerAxis(state->Gamepad.sThumbLX,g_vrXInputState.Gamepad.sThumbLX);
        state->Gamepad.sThumbLY=strongerAxis(state->Gamepad.sThumbLY,g_vrXInputState.Gamepad.sThumbLY);
        state->Gamepad.sThumbRX=strongerAxis(state->Gamepad.sThumbRX,g_vrXInputState.Gamepad.sThumbRX);
        state->Gamepad.sThumbRY=strongerAxis(state->Gamepad.sThumbRY,g_vrXInputState.Gamepad.sThumbRY);
        state->Gamepad.bLeftTrigger=(std::max)(state->Gamepad.bLeftTrigger,g_vrXInputState.Gamepad.bLeftTrigger);
        state->Gamepad.bRightTrigger=(std::max)(state->Gamepad.bRightTrigger,g_vrXInputState.Gamepad.bRightTrigger);
        state->dwPacketNumber=g_vrXInputState.dwPacketNumber;
        return ERROR_SUCCESS;
    }

    // HatVR AHiT controller presentation bridge
    //
    // Resolved statically from HatinTimeGame.exe (2026-09-24):
    //   UHat_PlayerInput_Base::GetGamepadButtonIcon  RVA 0xA99260
    //   UHat_PlayerInput_Base::GetGamepadName        RVA 0xA99680
    //
    // Their UnrealScript exec thunks are RVA 0xAD2440, 0xAD1D20, 0xAD1480.
    // Hook the underlying C++ methods so both native and UnrealScript callers
    // see the same HatVR behavior.
    struct HatVrFString
    {
        wchar_t* Data;
        int32_t Num;
        int32_t Max;
    };

    using HatVrGetStringFn = HatVrFString*(__fastcall*)(void*, HatVrFString*);
    using HatVrGetButtonIconFn =
        HatVrFString*(__fastcall*)(void*, HatVrFString*, unsigned char);
    using HatVrFStringAssignFn =
        HatVrFString*(__fastcall*)(HatVrFString*, const HatVrFString*);

    static HatVrGetStringFn g_hatVrOriginalGetGamepadName = nullptr;
    static HatVrGetButtonIconFn g_hatVrOriginalGetGamepadButtonIcon = nullptr;
    static HatVrFStringAssignFn g_hatVrFStringAssign = nullptr;
    static bool g_hatVrAHiTControllerHooksInstalled = false;
    static ULONGLONG g_hatVrLastPhysicalVrInputMs = 0;

    static void HatVrAssignFStringLiteral(HatVrFString* dst, const wchar_t* text)
    {
        if (!dst || !text || !g_hatVrFStringAssign) return;
        const size_t len = wcslen(text);
        HatVrFString src{};
        src.Data = const_cast<wchar_t*>(text);
        src.Num = static_cast<int32_t>(len + 1);
        src.Max = src.Num;
        g_hatVrFStringAssign(dst, &src);
    }

    static const wchar_t* HatVrOpenXrControllerName()
    {
        if (g_xrSession == XR_NULL_HANDLE || g_xrInstance == XR_NULL_HANDLE)
            return L"VR Controller";

        XrPath hand = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(g_xrInstance, "/user/hand/right", &hand)))
            return L"VR Controller";

        XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(g_xrSession, hand, &state)) ||
            state.interactionProfile == XR_NULL_PATH)
            return L"VR Controller";

        char profile[XR_MAX_PATH_LENGTH] = {};
        uint32_t count = 0;
        if (XR_FAILED(xrPathToString(g_xrInstance, state.interactionProfile,
            static_cast<uint32_t>(sizeof(profile)), &count, profile)))
            return L"VR Controller";

        if (strstr(profile, "/oculus/touch_controller"))
            return L"Oculus Touch Controller";
        if (strstr(profile, "/valve/index_controller"))
            return L"Valve Index Controller";
        if (strstr(profile, "/microsoft/motion_controller"))
            return L"Windows Mixed Reality Controller";
        if (strstr(profile, "/htc/vive_controller"))
            return L"HTC Vive Controller";
        if (strstr(profile, "/khr/simple_controller"))
            return L"VR Controller";

        // do not invent a retail model when the runtime only exposes an
        // unfamiliar/emulated interaction profile.
        return L"VR Controller";
    }

    static HatVrFString* __fastcall HatVrHookedGetGamepadName(
        void* self, HatVrFString* out)
    {
        HatVrFString* result = g_hatVrOriginalGetGamepadName
            ? g_hatVrOriginalGetGamepadName(self, out) : out;
        const ULONGLONG now = GetTickCount64();
        const bool recentlyUsedVr =
            g_hatVrLastPhysicalVrInputMs != 0 &&
            now >= g_hatVrLastPhysicalVrInputMs &&
            (now - g_hatVrLastPhysicalVrInputMs) <= 350;
        if (out && g_vrControllerInputEnabled && g_vrControllerActive &&
            recentlyUsedVr)
            HatVrAssignFStringLiteral(out, HatVrOpenXrControllerName());
        return result ? result : out;
    }

    static const wchar_t* HatVrPlayStationIconForBind(unsigned char bind)
    {
        // HatControllerBind enum from Hat_PlayerInput_Base.uc.  These mappings
        // are taken from the game's own Dualshock4.ini and preserve the
        // existing Xbox/XInput gameplay mapping underneath.
        switch (bind)
        {
            case 1:  return L"PS4_Options";    // Menu_Start
            case 2:  return L"PS4_Cross";      // Menu_Confirm
            case 3:  return L"PS4_Circle";     // Menu_Cancel
            case 5:  return L"PS4_R1";         // Menu_PageRight
            case 6:  return L"PS4_L1";         // Menu_PageLeft
            case 7:  return L"PS4_Cross";      // Player_Jump
            case 8:  return L"PS4_Square";     // Player_Attack
            case 9:  return L"PS4_Circle";     // Player_Interact
            case 10: return L"PS4_R2";         // Player_Crouch
            case 11: return L"PS4_L2";         // Player_Ability
            case 13: return L"PS4_R1";         // Player_CameraSnap
            case 14: return L"circle_black";   // ReverseCameraSnap / R3
            case 15: return L"PS4_Touchpad";   // Player_Share
            case 16: return L"PS4_Triangle";   // FocusLookUp
            case 17: return L"circle_black";   // Emote / L3
            case 18: return L"PS4_L1";         // AbilitySwap
            case 22: return L"PS4_DPad_Up";
            case 23: return L"PS4_DPad_Down";
            case 24: return L"PS4_DPad_Left";
            case 25: return L"PS4_DPad_Right";
            default: return nullptr;
        }
    }

    static const wchar_t* HatVrNintendoSwitchIconForBind(unsigned char bind)
    {
        // presentation-only Nintendo Switch glyph theme. Keep A Hat in Time's
        // existing Xbox/XInput action semantics; only substitute icon resources.
        // Resource names come from NintendoSwitch.ini.
        switch (bind)
        {
            case 1:  return L"Switch_Plus";              // Menu_Start / Xbox Start
            case 2:  return L"Switch_A";                 // Menu_Confirm / Xbox A
            case 3:  return L"Switch_B";                 // Menu_Cancel / Xbox B
            case 5:  return L"Switch_R";                 // Menu_PageRight / Xbox RB
            case 6:  return L"Switch_L";                 // Menu_PageLeft / Xbox LB
            case 7:  return L"Switch_A";                 // Player_Jump / Xbox A
            case 8:  return L"Switch_X";                 // Player_Attack / Xbox X
            case 9:  return L"Switch_B";                 // Player_Interact / Xbox B
            case 10: return L"Switch_ZR";                // Player_Crouch / Xbox RT
            case 11: return L"Switch_ZL";                // Player_Ability / Xbox LT
            case 13: return L"Switch_R";                 // CameraSnap / Xbox RB
            case 14: return L"Switch_Right_Stick_Press"; // ReverseCameraSnap / Xbox R3
            case 15: return L"Switch_Minus";             // Player_Share / Xbox Back
            case 16: return L"Switch_Y";                 // FocusLookUp / Xbox Y
            case 17: return L"Switch_Left_Stick_Press";  // Emote / Xbox L3
            case 18: return L"Switch_L";                 // AbilitySwap / Xbox LB
            case 22: return L"Switch_DPad_Up";
            case 23: return L"Switch_DPad_Down";
            case 24: return L"Switch_DPad_Left";
            case 25: return L"Switch_DPad_Right";
            default: return nullptr;
        }
    }

    static HatVrFString* __fastcall HatVrHookedGetGamepadButtonIcon(
        void* self, HatVrFString* out, unsigned char bind)
    {
        HatVrFString* result = g_hatVrOriginalGetGamepadButtonIcon
            ? g_hatVrOriginalGetGamepadButtonIcon(self, out, bind) : out;

        if (out && g_vrControllerInputEnabled && g_vrControllerActive)
        {
            if (g_nintendoSwitchControllerIcons)
            {
                if (const wchar_t* icon = HatVrNintendoSwitchIconForBind(bind))
                    HatVrAssignFStringLiteral(out, icon);
            }
            else if (g_playStationControllerIcons)
            {
                if (const wchar_t* icon = HatVrPlayStationIconForBind(bind))
                    HatVrAssignFStringLiteral(out, icon);
            }
        }
        return result ? result : out;
    }

    static void InstallAHiTControllerHooks()
    {
        if (g_hatVrAHiTControllerHooksInstalled) return;
        auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!module) return;

        g_hatVrFStringAssign = reinterpret_cast<HatVrFStringAssignFn>(
            module + 0x1BE490);

        struct HookSpec { uintptr_t rva; void* detour; void** original; };
        HookSpec hooks[] = {
            { 0xA99680, reinterpret_cast<void*>(&HatVrHookedGetGamepadName),
                reinterpret_cast<void**>(&g_hatVrOriginalGetGamepadName) },
            { 0xA99260, reinterpret_cast<void*>(&HatVrHookedGetGamepadButtonIcon),
                reinterpret_cast<void**>(&g_hatVrOriginalGetGamepadButtonIcon) },
        };

        bool any = false;
        for (auto& hook : hooks)
        {
            void* target = module + hook.rva;
            MH_STATUS st = MH_CreateHook(target, hook.detour, hook.original);
            if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
                continue;
            st = MH_EnableHook(target);
            if (st == MH_OK || st == MH_ERROR_ENABLED)
                any = true;
        }

        g_hatVrAHiTControllerHooksInstalled = any;
        if (any)
            LogCategory("INPUT",
                "AHiT controller hooks active: activity-scoped VR name, controller glyph override");
    }

    static void InstallXInputBridge()
    {
        if (g_xinputHookInstalled) return;
        const wchar_t* modules[] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
        for (const wchar_t* moduleName : modules)
        {
            HMODULE module = GetModuleHandleW(moduleName);
            if (!module) continue;
            void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
            void* setTarget = reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState"));
            if (!target) continue;

            MH_STATUS st = MH_CreateHook(target, &HookedXInputGetState,
                reinterpret_cast<void**>(&g_originalXInputGetState));
            if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) continue;
            st = MH_EnableHook(target);
            if (st != MH_OK && st != MH_ERROR_ENABLED) continue;

            if (setTarget)
            {
                MH_STATUS setSt = MH_CreateHook(setTarget, &HookedXInputSetState,
                    reinterpret_cast<void**>(&g_originalXInputSetState));
                if (setSt == MH_OK || setSt == MH_ERROR_ALREADY_CREATED)
                    MH_EnableHook(setTarget);
            }

            g_xinputHookInstalled = true;
            break;
        }
    }

    static bool InitializeVrControllerActionsOnAvatarSet(
        XrActionSet actionSet, XrAction gripPoseAction)
    {
        if (g_controllerActionsReady) return true;
        if (g_xrInstance == XR_NULL_HANDLE || actionSet == XR_NULL_HANDLE ||
            gripPoseAction == XR_NULL_HANDLE) return false;

        g_controllerActionSet = actionSet; // alias; avatar runtime owns attachment

        g_actionLeftStick = CreateHatVrAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
        g_actionRightStick = CreateHatVrAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "camera", "Camera");
        g_actionA = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "gamepad_a", "A / Jump");
        g_actionB = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "gamepad_b", "B / Cancel");
        g_actionX = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "gamepad_x", "X");
        g_actionY = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "gamepad_y", "Y");
        g_actionLeftTrigger = CreateHatVrAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Left Trigger");
        g_actionRightTrigger = CreateHatVrAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "Right Trigger");
        g_actionLeftGrip = CreateHatVrAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Left Bumper");
        g_actionRightGrip = CreateHatVrAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "Right Bumper");
        g_actionStart = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "start", "Menu (Hold for Back)");
        g_actionBack = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "back", "Share / Back");
        g_actionLeftStickClick = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Left Stick Click / Emote");
        g_actionRightStickClick = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click", "Right Stick Click");
        g_actionDpadUp = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_up", "D-Pad Up");
        g_actionDpadDown = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_down", "D-Pad Down");
        g_actionDpadLeft = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_left", "D-Pad Left");
        g_actionDpadRight = CreateHatVrAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_right", "D-Pad Right");
        g_actionHapticLeft = CreateHatVrAction(XR_ACTION_TYPE_VIBRATION_OUTPUT,
            "haptic_left", "Left Controller Haptics");
        g_actionHapticRight = CreateHatVrAction(XR_ACTION_TYPE_VIBRATION_OUTPUT,
            "haptic_right", "Right Controller Haptics");

        if (g_actionLeftStick == XR_NULL_HANDLE || g_actionRightStick == XR_NULL_HANDLE ||
            g_actionA == XR_NULL_HANDLE || g_actionB == XR_NULL_HANDLE ||
            g_actionX == XR_NULL_HANDLE || g_actionStart == XR_NULL_HANDLE)
            return false;

        std::vector<XrActionSuggestedBinding> touch;
        AddBinding(touch, gripPoseAction, "/user/hand/left/input/grip/pose");
        AddBinding(touch, gripPoseAction, "/user/hand/right/input/grip/pose");
        AddBinding(touch, g_actionLeftStick, "/user/hand/left/input/thumbstick");
        AddBinding(touch, g_actionRightStick, "/user/hand/right/input/thumbstick");
        AddBinding(touch, g_actionA, "/user/hand/right/input/a/click");
        AddBinding(touch, g_actionB, "/user/hand/right/input/b/click");
        AddBinding(touch, g_actionX, "/user/hand/left/input/x/click");
        AddBinding(touch, g_actionStart, "/user/hand/left/input/y/click");
        AddBinding(touch, g_actionLeftTrigger, "/user/hand/left/input/trigger/value");
        AddBinding(touch, g_actionRightTrigger, "/user/hand/right/input/trigger/value");
        AddBinding(touch, g_actionLeftGrip, "/user/hand/left/input/squeeze/value");
        AddBinding(touch, g_actionRightGrip, "/user/hand/right/input/squeeze/value");
        AddBinding(touch, g_actionLeftStickClick, "/user/hand/left/input/thumbstick/click");
        AddBinding(touch, g_actionRightStickClick, "/user/hand/right/input/thumbstick/click");
        AddBinding(touch, g_actionHapticLeft, "/user/hand/left/output/haptic");
        AddBinding(touch, g_actionHapticRight, "/user/hand/right/output/haptic");
        SuggestControllerProfile("/interaction_profiles/oculus/touch_controller", touch);

        std::vector<XrActionSuggestedBinding> index;
        AddBinding(index, gripPoseAction, "/user/hand/left/input/grip/pose");
        AddBinding(index, gripPoseAction, "/user/hand/right/input/grip/pose");
        AddBinding(index, g_actionLeftStick, "/user/hand/left/input/thumbstick");
        AddBinding(index, g_actionRightStick, "/user/hand/right/input/thumbstick");
        AddBinding(index, g_actionA, "/user/hand/right/input/a/click");
        AddBinding(index, g_actionB, "/user/hand/right/input/b/click");
        AddBinding(index, g_actionX, "/user/hand/left/input/a/click");
        AddBinding(index, g_actionStart, "/user/hand/left/input/b/click");
        AddBinding(index, g_actionLeftTrigger, "/user/hand/left/input/trigger/value");
        AddBinding(index, g_actionRightTrigger, "/user/hand/right/input/trigger/value");
        AddBinding(index, g_actionLeftGrip, "/user/hand/left/input/squeeze/value");
        AddBinding(index, g_actionRightGrip, "/user/hand/right/input/squeeze/value");
        AddBinding(index, g_actionLeftStickClick, "/user/hand/left/input/thumbstick/click");
        AddBinding(index, g_actionRightStickClick, "/user/hand/right/input/thumbstick/click");
        AddBinding(index, g_actionHapticLeft, "/user/hand/left/output/haptic");
        AddBinding(index, g_actionHapticRight, "/user/hand/right/output/haptic");
        SuggestControllerProfile("/interaction_profiles/valve/index_controller", index);

        g_controllerActionsReady = true;
        DebugLog("MOTION_V4 gamepad actions created on avatar action set\\n");
        return true;
    }

    static bool InitializeVrControllerInput()
    {
        if (!g_controllerActionsReady) return false;
        InstallXInputBridge();
        InstallAHiTControllerHooks();
        InstallUE3SourceAwareHaptics();
        g_vrControllerInputEnabled = true;
        DebugLog("MOTION_V4 XInput bridge ready -- shared avatar/gamepad action set\n");
        return true;
    }

    static bool ReadBoolAction(XrAction action)
    {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = action;
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
        return XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession, &gi, &state)) &&
            state.isActive && state.currentState;
    }
    static float ReadFloatAction(XrAction action)
    {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = action;
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        return XR_SUCCEEDED(xrGetActionStateFloat(g_xrSession, &gi, &state)) && state.isActive
            ? state.currentState : 0.0f;
    }
    static XrVector2f ReadVectorAction(XrAction action)
    {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = action;
        XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_xrSession, &gi, &state)) && state.isActive)
            return state.currentState;
        return {};
    }

