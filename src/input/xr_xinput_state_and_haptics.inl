    // OpenXR exposes named game actions.  SteamVR users can remap these through
    // SteamVR's normal VR bindings UI; other runtimes receive sensible profile
    // suggestions.  The resulting state is merged into player-one XInput so the
    // game continues using its existing controller code and button prompts.

    static XrActionSet g_controllerActionSet = XR_NULL_HANDLE;
    static XrAction g_actionLeftStick = XR_NULL_HANDLE;
    static XrAction g_actionRightStick = XR_NULL_HANDLE;
    static XrAction g_actionA = XR_NULL_HANDLE;
    static XrAction g_actionB = XR_NULL_HANDLE;
    static XrAction g_actionX = XR_NULL_HANDLE;
    static XrAction g_actionY = XR_NULL_HANDLE;
    static XrAction g_actionLeftTrigger = XR_NULL_HANDLE;
    static XrAction g_actionRightTrigger = XR_NULL_HANDLE;
    static XrAction g_actionLeftGrip = XR_NULL_HANDLE;
    static XrAction g_actionRightGrip = XR_NULL_HANDLE;
    static XrAction g_actionStart = XR_NULL_HANDLE;
    static XrAction g_actionBack = XR_NULL_HANDLE;
    static XrAction g_actionLeftStickClick = XR_NULL_HANDLE;
    static XrAction g_actionRightStickClick = XR_NULL_HANDLE;
    static XrAction g_actionDpadUp = XR_NULL_HANDLE;
    static XrAction g_actionDpadDown = XR_NULL_HANDLE;
    static XrAction g_actionDpadLeft = XR_NULL_HANDLE;
    static XrAction g_actionDpadRight = XR_NULL_HANDLE;
    static XrAction g_actionHapticLeft = XR_NULL_HANDLE;
    static XrAction g_actionHapticRight = XR_NULL_HANDLE;
    static bool g_controllerActionsReady = false;
    static bool g_vrControllerActive = false;
    static bool g_vrControllerInputEnabled = true;

    // Minimal XInput ABI; the current stripped project no longer includes XInput headers.
    struct HatVrGamepad {
        unsigned short wButtons;
        unsigned char bLeftTrigger, bRightTrigger;
        short sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    };
    struct HatVrXInputState {
        unsigned long dwPacketNumber;
        HatVrGamepad Gamepad;
    };
    static constexpr unsigned short HV_DPAD_UP=0x0001, HV_DPAD_DOWN=0x0002,
        HV_DPAD_LEFT=0x0004, HV_DPAD_RIGHT=0x0008, HV_START=0x0010, HV_BACK=0x0020,
        HV_LEFT_THUMB=0x0040, HV_RIGHT_THUMB=0x0080, HV_LEFT_SHOULDER=0x0100,
        HV_RIGHT_SHOULDER=0x0200, HV_A=0x1000, HV_B=0x2000, HV_X=0x4000, HV_Y=0x8000;

    struct HatVrXInputVibration {
        unsigned short wLeftMotorSpeed;
        unsigned short wRightMotorSpeed;
    };

    static HatVrXInputState g_vrXInputState{};

    using XInputSetStateFn = DWORD(WINAPI*)(DWORD, HatVrXInputVibration*);
    static XInputSetStateFn g_originalXInputSetState = nullptr;

    static void ApplyHatVrControllerHaptics(const HatVrXInputVibration& vibration)
    {
        if (!g_vrControllerInputEnabled || !g_vrControllerActive ||
            !g_xrSessionRunning || g_xrSession == XR_NULL_HANDLE)
            return;

        // XInput's two values are low-frequency and high-frequency motors,
        // NOT left/right hands.  Preserve the game's authored overall strength
        // by combining the two motors, then send that pulse to both tracked
        // VR controllers. XInputSetState is refreshed continuously while a
        // waveform is active, so a short pulse naturally follows UE3's envelope.
        const float low = static_cast<float>(vibration.wLeftMotorSpeed) / 65535.0f;
        const float high = static_cast<float>(vibration.wRightMotorSpeed) / 65535.0f;
        const float amplitude = (std::max)(low, high);

        XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
        XrHapticVibration pulse{ XR_TYPE_HAPTIC_VIBRATION };
        pulse.duration = 100000000; // 100 ms; refreshed by subsequent SetState calls.
        pulse.frequency = XR_FREQUENCY_UNSPECIFIED;
        pulse.amplitude = amplitude;

        const XrAction actions[2] = { g_actionHapticLeft, g_actionHapticRight };
        for (int hand = 0; hand < 2; ++hand)
        {
            if (actions[hand] == XR_NULL_HANDLE)
                continue;
            info.action = actions[hand];
            if (amplitude > 0.0001f)
                xrApplyHapticFeedback(g_xrSession, &info,
                    reinterpret_cast<const XrHapticBaseHeader*>(&pulse));
            else
                xrStopHapticFeedback(g_xrSession, &info);
        }
    }

