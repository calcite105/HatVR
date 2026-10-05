    static void SyncVrControllerActions()
    {
        HatVrUpdatePsvr2SourceHaptic();
        g_vrControllerActive = false;
        if (!g_vrControllerInputEnabled || !g_controllerActionsReady || !g_xrSessionRunning)
            return;

        HatVrXInputState next{};
        XrVector2f ls = ReadVectorAction(g_actionLeftStick);
        XrVector2f rs = ReadVectorAction(g_actionRightStick);
// Physical right stick is always dominant. Eye selection only supplies
        // the stick while the real controller is neutral.
        const float physicalRightMag =
            sqrtf(rs.x * rs.x + rs.y * rs.y);
        constexpr float kPhysicalRightStickTakeover = 0.18f;
        if (physicalRightMag < kPhysicalRightStickTakeover)
        {
            XrVector2f eyeStick{};
            if (ReadPsvr2EyeWheelStick(eyeStick))
                rs = eyeStick;
        }

        auto axis=[](float v)->short{v=fmaxf(-1.0f,fminf(1.0f,v));return (short)(v>=0?v*32767.0f:v*32768.0f);};
        auto trig=[](float v)->unsigned char{v=fmaxf(0.0f,fminf(1.0f,v));return (unsigned char)(v*255.0f);};

        const bool aDown = ReadBoolAction(g_actionA);
        const bool bDown = ReadBoolAction(g_actionB);

        // g_actionStart is bound to the physical VR-controller Y/menu button
        // (Touch Y / Index left B). A single tap remains AHiT Start/Menu;
        // a double tap opens HatVR without leaking either tap into the game.
        const bool menuDown = ReadBoolAction(g_actionStart);

        // Presentation identity should follow the input that actually caused
        // the controller transition, not merely the fact that a VR controller
        // exists. Keep a short "VR was just used" window for GetGamepadName.
        const bool vrWasPhysicallyUsed =
            aDown || bDown || menuDown ||
            ReadBoolAction(g_actionX) || ReadBoolAction(g_actionY) ||
            ReadBoolAction(g_actionBack) ||
            ReadBoolAction(g_actionLeftStickClick) ||
            ReadBoolAction(g_actionRightStickClick) ||
            ReadBoolAction(g_actionDpadUp) || ReadBoolAction(g_actionDpadDown) ||
            ReadBoolAction(g_actionDpadLeft) || ReadBoolAction(g_actionDpadRight) ||
            ReadFloatAction(g_actionLeftTrigger) > 0.10f ||
            ReadFloatAction(g_actionRightTrigger) > 0.10f ||
            ReadFloatAction(g_actionLeftGrip) > 0.10f ||
            ReadFloatAction(g_actionRightGrip) > 0.10f ||
            fabsf(ls.x) > 0.18f || fabsf(ls.y) > 0.18f ||
            fabsf(rs.x) > 0.18f || fabsf(rs.y) > 0.18f;
        if (vrWasPhysicallyUsed)
            g_hatVrLastPhysicalVrInputMs = GetTickCount64();

        static bool aWasDown = false;
        static bool bWasDown = false;
        static bool menuWasDown = false;
        static bool pendingGameStart = false;
        static ULONGLONG firstMenuTapMs = 0;
        static constexpr ULONGLONG kHatVrDoubleTapMs = 375;

        const bool aPressed = aDown && !aWasDown;
        const bool bPressed = bDown && !bWasDown;
        const bool menuPressed = menuDown && !menuWasDown;
        aWasDown = aDown;
        bWasDown = bDown;
        menuWasDown = menuDown;

        const ULONGLONG nowMs = GetTickCount64();
        bool sendGameStartThisFrame = false;

        //   single Y  -> delayed Start/Menu pulse to AHiT
        //   double Y  -> HatVR menu
        //   Y in HatVR -> close HatVR immediately
        if (g_vrMenuOpen)
        {
            if (menuPressed)
            {
                g_vrMenuOpen = false;
                g_psvr2MenuOpen = false;
                pendingGameStart = false;
                LogCategory("MENU", "VR menu CLOSED");
            }
        }
        else
        {
            if (menuPressed)
            {
                if (pendingGameStart &&
                    nowMs >= firstMenuTapMs &&
                    (nowMs - firstMenuTapMs) <= kHatVrDoubleTapMs)
                {
                    pendingGameStart = false;
                    g_vrMenuOpen = true;
                    g_psvr2MenuOpen = false;
                    g_vrMenuSelection = 0;
                    g_vrMenuInsideCategory = false;
                    LogCategory("MENU", "VR menu OPEN (double Y)");
                }
                else
                {
                    pendingGameStart = true;
                    firstMenuTapMs = nowMs;
                }
            }

            if (!g_vrMenuOpen &&
                pendingGameStart &&
                nowMs >= firstMenuTapMs &&
                (nowMs - firstMenuTapMs) > kHatVrDoubleTapMs)
            {
                pendingGameStart = false;
                sendGameStartThisFrame = true;
            }
        }

        if (g_vrMenuOpen)
        {
            // Two-level settings navigation:
            //   category focus: Up/Down selects a category, A enters it, B closes HatVR.
            //   page focus:     Up/Down selects a setting, A activates it, B returns.
            // Left/Right is reserved for changing values (currently the UI/HUD sliders).
            static bool navUpWasDown=false, navDownWasDown=false;
            static bool navLeftWasDown=false, navRightWasDown=false;
            const bool navUp=ls.y>0.55f, navDown=ls.y<-0.55f;
            const bool navLeft=ls.x<-0.55f, navRight=ls.x>0.55f;
            auto pageRows=[](int page)->int { switch(page){case 0:return 1;case 1:return 12;case 2:return 9;case 3:return 8;case 4:return 3;case 5:return 1;case 6:return 6;default:return 1;} };

            if(!g_vrMenuInsideCategory)
            {
                const int pageCount=g_debugToolsEnabled?7:6;
                if(navUp && !navUpWasDown) g_vrMenuPage=(g_vrMenuPage+pageCount-1)%pageCount;
                if(navDown && !navDownWasDown) g_vrMenuPage=(g_vrMenuPage+1)%pageCount;
                g_vrMenuSelection=0;

                if(aPressed)
                {
                    g_vrMenuInsideCategory=true;
                    g_vrMenuSelection=0;
                    LogCategory("MENU", "VR menu ENTER category=%d", g_vrMenuPage);
                }
                if(bPressed)
                {
                    g_vrMenuOpen=false;
                    pendingGameStart=false;
                    LogCategory("MENU", "VR menu CLOSED (B from categories)");
                }
            }
            else
            {
                const int rows=pageRows(g_vrMenuPage);
                if(g_vrMenuSelection<0 || g_vrMenuSelection>=rows) g_vrMenuSelection=0;
                if(navUp && !navUpWasDown) g_vrMenuSelection=(g_vrMenuSelection+rows-1)%rows;
                if(navDown && !navDownWasDown) g_vrMenuSelection=(g_vrMenuSelection+1)%rows;

                const bool hudSlider=(g_vrMenuPage==3 && g_vrMenuSelection>=0 && g_vrMenuSelection<=2);
                const bool hmdIntensity=(g_vrMenuPage==2 && g_vrMenuSelection==5);
                const bool hmdCurve=(g_vrMenuPage==2 && g_vrMenuSelection==6);
                const bool xrUpscalerChoice=(g_vrMenuPage==1 && g_vrMenuSelection==9);
                const bool xrUpscaleAmount=(g_vrMenuPage==1 && g_vrMenuSelection==10);
                const bool xrUpscaleSharpness=(g_vrMenuPage==1 && g_vrMenuSelection==11);
                if(hudSlider || hmdIntensity || hmdCurve || xrUpscalerChoice || xrUpscaleAmount || xrUpscaleSharpness)
                {
                    const float dir=(navRight&&!navRightWasDown)?1.0f:((navLeft&&!navLeftWasDown)?-1.0f:0.0f);
                    if(dir!=0.0f)
                    {
                        if(g_vrMenuSelection==0) g_hatVrHudScale=(std::max)(0.50f,(std::min)(2.00f,g_hatVrHudScale+dir*0.05f));
                        if(g_vrMenuSelection==1) g_hatVrHudDistance=(std::max)(0.25f,(std::min)(2.00f,g_hatVrHudDistance+dir*0.05f));
                        if(g_vrMenuSelection==2) g_hatVrHudHeight=(std::max)(-1.00f,(std::min)(1.00f,g_hatVrHudHeight+dir*0.05f));
                        if(hmdIntensity) g_psvr2HmdRumbleIntensity=(std::max)(0,(std::min)(100,g_psvr2HmdRumbleIntensity+(int)dir*5));
                        if(hmdCurve) g_psvr2HmdRumbleCurve=(g_psvr2HmdRumbleCurve+(dir>0?1:2))%3;
                        if(xrUpscalerChoice) g_openXrUpscaler=(g_openXrUpscaler==0)?1:0;
                        if(xrUpscaleAmount) g_openXrUpscalePercent=(std::max)(25,(std::min)(100,g_openXrUpscalePercent+(dir>0.0f?5:-5)));
                        if(xrUpscaleSharpness) g_openXrUpscaleSharpness=(std::max)(0,(std::min)(100,g_openXrUpscaleSharpness+(dir>0.0f?5:-5)));
                        SaveHatVrConfig();
                    }
                }

                if(aPressed)
                {
                    if(g_vrMenuPage==0) // Home
                        g_xrPositionRecenterPending=true;
                    else if(g_vrMenuPage==1) // VR
                    {
                        switch(g_vrMenuSelection)
                        {
                            case 0:{ const bool requestedStereo=!g_nativeStereoEnabled; if(SetNativeRenderLoopStereoPatch(false)) g_nativeStereoEnabled=requestedStereo; SaveHatVrConfig(); break; }
                            case 1:g_sharperNativeStereo=!g_sharperNativeStereo; SaveHatVrConfig(); LogCategory("MENU", "Sharper Native Stereo %s (OpenXR controller runtime)", g_sharperNativeStereo ? "ON" : "OFF"); break;
                            case 2:{ const bool enable=!(g_fpV1Enabled&&g_avV29ProbeEnabled); SetUnifiedFirstPersonEnabled(enable); SaveHatVrConfig(); break; }
                            case 3:g_hatVrModApiEnabled=!g_hatVrModApiEnabled; SaveHatVrConfig(); break;
                            case 4:g_disablePlayerFade=!g_disablePlayerFade; SaveHatVrConfig(); break;
                            case 5:g_theaterMode=!g_theaterMode; SaveHatVrConfig(); break;
                            case 6:g_autoTheaterCutscenes=!g_autoTheaterCutscenes; SaveHatVrConfig(); break;
                            case 7:{ const bool was=g_overrideLockedGameplayCameras; g_overrideLockedGameplayCameras=!g_overrideLockedGameplayCameras; if(was&&!g_overrideLockedGameplayCameras) FP_V116RestoreWorkshopCameraModes(); else if(!was&&g_overrideLockedGameplayCameras) g_fpV127WorkshopRemoveApplied=false; SaveHatVrConfig(); break; }
                            case 8:g_openXrUpscalingEnabled=!g_openXrUpscalingEnabled; SaveHatVrConfig(); break;
                            case 9:g_openXrUpscaler=(g_openXrUpscaler==0)?1:0; SaveHatVrConfig(); break;
                            case 10:g_openXrUpscalePercent = g_openXrUpscalePercent>=100 ? 25 : g_openXrUpscalePercent+25; SaveHatVrConfig(); break;
                            case 11:g_openXrUpscaleSharpness = g_openXrUpscaleSharpness>=100 ? 0 : g_openXrUpscaleSharpness+10; SaveHatVrConfig(); break;
                        }
                    }
                    else if(g_vrMenuPage==2) // Controls
                    {
                        switch(g_vrMenuSelection)
                        {
                            case 0:g_rightHandHookshot=!g_rightHandHookshot; SaveHatVrConfig(); break;
                            case 1:g_umbrellaMotionControls=!g_umbrellaMotionControls; SaveHatVrConfig(); break;
                            case 2:g_playStationControllerIcons=!g_playStationControllerIcons; if(g_playStationControllerIcons) g_nintendoSwitchControllerIcons=false; SaveHatVrConfig(); break;
                            case 3:g_nintendoSwitchControllerIcons=!g_nintendoSwitchControllerIcons; if(g_nintendoSwitchControllerIcons) g_playStationControllerIcons=false; SaveHatVrConfig(); break;
                            case 4:g_psvr2HmdRumbleEnabled=!g_psvr2HmdRumbleEnabled; if(!g_psvr2HmdRumbleEnabled) HatVrSendPsvr2HmdHz(0); else EnsureHatVrPsvr2Capi(); SaveHatVrConfig(); break;
                            case 5:break; // HMD intensity is adjusted with Left / Right.
                            case 6:g_psvr2HmdRumbleCurve=(g_psvr2HmdRumbleCurve+1)%3; SaveHatVrConfig(); break;
                            case 7:g_psvr2HookshotAdaptiveTrigger=!g_psvr2HookshotAdaptiveTrigger; if(!g_psvr2HookshotAdaptiveTrigger) HatVrSetPsvr2RightHookshotTrigger(false); SaveHatVrConfig(); break;
                            case 8:g_psvr2EyeWheelEnabled=!g_psvr2EyeWheelEnabled; SaveHatVrConfig(); break;
                        }
                    }
                    else if(g_vrMenuPage==3) // UI / HUD
                    {
                        switch(g_vrMenuSelection)
                        {
                            case 3:g_hatVrHudScale=1.0f; SaveHatVrConfig(); break;
                            case 4:g_hatVrHudDistance=0.65f; g_hatVrHudHeight=0.0f; SaveHatVrConfig(); break;
                            case 5:g_hatVrHudHeadLocked=!g_hatVrHudHeadLocked; SaveHatVrConfig(); break;
                            case 6:g_hatVrCurseCasualMenuFont=!g_hatVrCurseCasualMenuFont; SaveHatVrConfig(); break;
                            case 7:g_hatVrHudScale=1.0f; g_hatVrHudDistance=0.65f; g_hatVrHudHeight=0.0f; g_hatVrHudHeadLocked=false; g_hatVrCurseCasualMenuFont=true; SaveHatVrConfig(); break;
                        }
                    }
                    else if(g_vrMenuPage==4) // Spectator
                    {
                        if(g_vrMenuSelection==0) { g_spectatorView=(g_spectatorView+1)%4; SaveHatVrConfig(); }
                        else if(g_vrMenuSelection==1) { g_spectatorUiMode=(g_spectatorUiMode+1)%2; SaveHatVrConfig(); }
                        else if(g_vrMenuSelection==2 && !g_sharperNativeStereo) { g_spectatorExpandedFov=!g_spectatorExpandedFov; SaveHatVrConfig(); }
                    }
                    else if(g_vrMenuPage==6)
                    {
                        switch(g_vrMenuSelection)
                        {
                            case 0: UiCandidateSelectDelta(-1); break;
                            case 1: UiCandidateSelectDelta(+1); break;
                            case 2: UiCandidateCyclePreview(); break;
                            case 3: UiCandidateMarkGame(); break;
                            case 4: UiCandidateMarkUi(); break;
                            case 5: UiCandidateClearMark(); break;
                        }
                    }
                }

                if(bPressed)
                {
                    g_vrMenuInsideCategory=false;
                    g_vrMenuSelection=0;
                    LogCategory("MENU", "VR menu BACK to categories");
                }
            }

            navUpWasDown=navUp; navDownWasDown=navDown;
            navLeftWasDown=navLeft; navRightWasDown=navRight;

            static DWORD menuPacket=0;
            next.dwPacketNumber=++menuPacket;
            g_vrXInputState=next; g_vrControllerActive=true;
            return;
        }

        next.Gamepad.sThumbLX=axis(ls.x); next.Gamepad.sThumbLY=axis(ls.y);
        next.Gamepad.sThumbRX=axis(rs.x); next.Gamepad.sThumbRY=axis(rs.y);
        next.Gamepad.bLeftTrigger=trig(ReadFloatAction(g_actionLeftTrigger));

        // Right-Hand Hookshot mode turns Right Grip into a VR-only modifier.
        // Grip itself is never forwarded as R1/RB while this mode is enabled.
        // Trigger remains normal until Grip is held; while held, Trigger is
        // suppressed and instead mirrors X/Square for the Hookshot.
        const float rightGripValue = ReadFloatAction(g_actionRightGrip);
        const float rightTriggerValue = ReadFloatAction(g_actionRightTrigger);
        const bool rightHookshotModifier = g_rightHandHookshot && rightGripValue > 0.55f;

        static bool s_hatVrHookshotAdaptiveTriggerOn = false;
        const bool hookshotTriggerEffect = rightHookshotModifier && g_psvr2HookshotAdaptiveTrigger;
        if (hookshotTriggerEffect != s_hatVrHookshotAdaptiveTriggerOn)
        {
            HatVrSetPsvr2RightHookshotTrigger(hookshotTriggerEffect);
            s_hatVrHookshotAdaptiveTriggerOn = hookshotTriggerEffect;
        }

        next.Gamepad.bRightTrigger = rightHookshotModifier ? 0 : trig(rightTriggerValue);

        if(aDown) next.Gamepad.wButtons|=HV_A;
        if(bDown) next.Gamepad.wButtons|=HV_B;
        if(ReadBoolAction(g_actionX)) next.Gamepad.wButtons|=HV_X;
        if(ReadBoolAction(g_actionY)) next.Gamepad.wButtons|=HV_Y;

        // Optional right-hand Hookshot control. While Right Grip is held,
        // Right Trigger becomes X/Square and cannot leak its normal trigger
        // action (which can otherwise cause an accidental dive).
        if(rightHookshotModifier && rightTriggerValue>0.55f)
            next.Gamepad.wButtons|=HV_X;

        // Delayed single Y tap -> the game's normal Start/Menu button.
        if(sendGameStartThisFrame) next.Gamepad.wButtons|=HV_START;

        // Hat Kid holds the umbrella in her right hand.
        if(g_umbrellaMotionControls && UpdateHandSwingAttack(1)) next.Gamepad.wButtons|=HV_X;

        if(ReadBoolAction(g_actionBack)) next.Gamepad.wButtons|=HV_BACK;
        if(ReadFloatAction(g_actionLeftGrip)>0.55f) next.Gamepad.wButtons|=HV_LEFT_SHOULDER;
        // In Right-Hand Hookshot mode Right Grip is exclusively a modifier and
        // must not also reach the game as R1/RB.
        if(!g_rightHandHookshot && rightGripValue>0.55f) next.Gamepad.wButtons|=HV_RIGHT_SHOULDER;
        if(ReadBoolAction(g_actionLeftStickClick)) next.Gamepad.wButtons|=HV_LEFT_THUMB;
        if(ReadBoolAction(g_actionRightStickClick)) next.Gamepad.wButtons|=HV_RIGHT_THUMB;
        if(ReadBoolAction(g_actionDpadUp)) next.Gamepad.wButtons|=HV_DPAD_UP;
        if(ReadBoolAction(g_actionDpadDown)) next.Gamepad.wButtons|=HV_DPAD_DOWN;
        if(ReadBoolAction(g_actionDpadLeft)) next.Gamepad.wButtons|=HV_DPAD_LEFT;
        if(ReadBoolAction(g_actionDpadRight)) next.Gamepad.wButtons|=HV_DPAD_RIGHT;

        static DWORD packet=0;
        next.dwPacketNumber=++packet;
        g_vrXInputState=next;
        g_vrControllerActive=true;
    }
