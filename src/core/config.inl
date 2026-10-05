// HatVR persistent user configuration.
// Kept deliberately small and human-editable. The file lives next to HatVR.dll.
//
// HatVR.ini is created on first startup and menu changes are written immediately.

static char g_hatVrConfigPath[MAX_PATH] = {};
static bool g_hatVrConfigLoaded = false;

static bool HatVrConfigParseBool(const char* text, bool fallback)
{
    if (!text || !*text) return fallback;
    if (!_stricmp(text, "1") || !_stricmp(text, "true") ||
        !_stricmp(text, "on") || !_stricmp(text, "yes"))
        return true;
    if (!_stricmp(text, "0") || !_stricmp(text, "false") ||
        !_stricmp(text, "off") || !_stricmp(text, "no"))
        return false;
    return fallback;
}

static bool HatVrBuildConfigPath()
{
    if (g_hatVrConfigPath[0]) return true;

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&HatVrBuildConfigPath), &module) || !module)
        return false;

    if (!GetModuleFileNameA(module, g_hatVrConfigPath, MAX_PATH))
        return false;

    char* slash = nullptr;
    for (char* p = g_hatVrConfigPath; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return false;

    *(slash + 1) = '\0';
    if (lstrlenA(g_hatVrConfigPath) + 9 >= MAX_PATH) return false;
    lstrcatA(g_hatVrConfigPath, "HatVR.ini");
    return true;
}

static bool HatVrReadConfigBool(const char* key, bool fallback)
{
    char fallbackText[8] = {};
    lstrcpyA(fallbackText, fallback ? "true" : "false");
    char value[32] = {};
    GetPrivateProfileStringA("HatVR", key, fallbackText, value,
        static_cast<DWORD>(sizeof(value)), g_hatVrConfigPath);
    return HatVrConfigParseBool(value, fallback);
}

static void HatVrWriteConfigBool(const char* key, bool value)
{
    WritePrivateProfileStringA("HatVR", key, value ? "true" : "false",
        g_hatVrConfigPath);
}

static float HatVrReadConfigFloat(const char* key, float fallback)
{
    char fallbackText[32] = {};
    _snprintf_s(fallbackText, sizeof(fallbackText), _TRUNCATE, "%.4f", fallback);
    char value[64] = {};
    GetPrivateProfileStringA("HatVR", key, fallbackText, value, static_cast<DWORD>(sizeof(value)), g_hatVrConfigPath);
    char* end = nullptr;
    const float parsed = strtof(value, &end);
    return (end && end != value) ? parsed : fallback;
}

static void HatVrWriteConfigFloat(const char* key, float value)
{
    char text[32] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4f", value);
    WritePrivateProfileStringA("HatVR", key, text, g_hatVrConfigPath);
}

// Renderer selection compatibility.  StereoMode is authoritative when present.
//   NativeStereo = later sequential/re-entry experiment
//   StereoLite   = BL1 principal-view -> renderer-boundary RIGHT clone
//   AFR          = same-frame AFR path
static int HatVrReadStereoModeCompat()
{
    char value[32] = {};
    GetPrivateProfileStringA("HatVR", "StereoMode", "", value,
        static_cast<DWORD>(sizeof(value)), g_hatVrConfigPath);
    if (!_stricmp(value, "NativeStereo")) return 0;
    if (!_stricmp(value, "StereoLite"))   return 1;
    if (!_stricmp(value, "AFR"))          return 2;

    // BL1/StereoLite renderer, not to the later sequential NativeStereo mode.
    return HatVrReadConfigBool("NativeStereo", true) ? 1 : 2;
}

static void HatVrApplyStereoModeCompat(int mode)
{
    if (mode < 0 || mode > 2) mode = 1;

    if (mode == 2) // AFR
    {
        g_nativeStereoEnabled = false;
        g_v26SequentialReentry = false;
        SetNativeRenderLoopStereoPatch(false);
    }
    else if (mode == 1) // StereoLite / BL1
    {
        g_nativeStereoEnabled = true;
        g_v26SequentialReentry = false;
        SetNativeRenderLoopStereoPatch(false);
    }
    else // historical explicit NativeStereo
    {
        g_nativeStereoEnabled = true;
        g_v26SequentialReentry = true;
        SetNativeRenderLoopStereoPatch(false);
    }
}

static void SaveHatVrConfig()
{
    if (!HatVrBuildConfigPath()) return;

    HatVrWriteConfigBool("NativeStereo", g_nativeStereoEnabled);
    HatVrWriteConfigBool("FirstPerson", g_firstPersonConfigured);
    HatVrWriteConfigBool("TheaterMode", g_theaterMode);
    HatVrWriteConfigBool("AutoCutsceneTheater", g_autoTheaterCutscenes);
    HatVrWriteConfigBool("OverrideLockedCameras", g_overrideLockedGameplayCameras);
    HatVrWriteConfigBool("RotationOnly", g_rotationOnlyTracking);
    HatVrWriteConfigBool("PlayStationIcons", g_playStationControllerIcons);
    HatVrWriteConfigBool("NintendoSwitchIcons", g_nintendoSwitchControllerIcons);
    HatVrWriteConfigBool("DisablePlayerFade", g_disablePlayerFade);
    HatVrWriteConfigBool("SharperNativeStereo", g_sharperNativeStereo);
    HatVrWriteConfigBool("RightHandHookshot", g_rightHandHookshot);
    HatVrWriteConfigBool("UmbrellaMotionControls", g_umbrellaMotionControls);
    HatVrWriteConfigFloat("HudScale", g_hatVrHudScale);
    HatVrWriteConfigFloat("HudDistance", g_hatVrHudDistance);
    HatVrWriteConfigFloat("HudHeight", g_hatVrHudHeight);
    HatVrWriteConfigBool("HudHeadLocked", g_hatVrHudHeadLocked);
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_spectatorView); WritePrivateProfileStringA("HatVR", "SpectatorView", v, g_hatVrConfigPath); }
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_spectatorUiMode); WritePrivateProfileStringA("HatVR", "SpectatorUiMode", v, g_hatVrConfigPath); }
    HatVrWriteConfigBool("SpectatorExpandedFov", g_spectatorExpandedFov);
    WritePrivateProfileStringA("HatVR", "DebugToolsEnabled", g_debugToolsEnabled ? "1" : "0", g_hatVrConfigPath);
    HatVrWriteConfigBool("CurseCasualMenuFont", g_hatVrCurseCasualMenuFont);
    HatVrWriteConfigBool("ModAPI", g_hatVrModApiEnabled);
    HatVrWriteConfigBool("PSVR2HmdRumble", g_psvr2HmdRumbleEnabled);
    HatVrWriteConfigBool("PSVR2StrongPhysicalOnly", g_psvr2StrongPhysicalOnly); // legacy
    HatVrWriteConfigBool("PSVR2EyeHatWheel", g_psvr2EyeWheelEnabled);
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_psvr2HmdRumbleIntensity); WritePrivateProfileStringA("HatVR", "PSVR2HmdRumbleIntensity", v, g_hatVrConfigPath); }
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_psvr2HmdRumbleCurve); WritePrivateProfileStringA("HatVR", "PSVR2HmdRumbleCurve", v, g_hatVrConfigPath); }
    HatVrWriteConfigBool("PSVR2HookshotAdaptiveTrigger", g_psvr2HookshotAdaptiveTrigger);
    { WritePrivateProfileStringA("HatVR", "OpenXRUpscalingEnabled", g_openXrUpscalingEnabled ? "1" : "0", g_hatVrConfigPath); }
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_openXrUpscaler); WritePrivateProfileStringA("HatVR", "OpenXRUpscaler", v, g_hatVrConfigPath); }
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_openXrUpscalePercent); WritePrivateProfileStringA("HatVR", "OpenXRUpscalePercent", v, g_hatVrConfigPath); }
    { char v[16] = {}; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_openXrUpscaleSharpness); WritePrivateProfileStringA("HatVR", "OpenXRUpscaleSharpness", v, g_hatVrConfigPath); }
}

static void LoadHatVrConfig()
{
    if (g_hatVrConfigLoaded) return;
    g_hatVrConfigLoaded = true;
    if (!HatVrBuildConfigPath()) return;

    // if the file does not exist, write the compiled defaults first. This also
    // makes every supported option discoverable without requiring documentation.
    const DWORD attrs = GetFileAttributesA(g_hatVrConfigPath);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        // first launch still needs the same renderer setup as later launches.
        // SaveHatVrConfig writes the compiled defaults, but it does not apply
        // the renderer's startup state by itself.
        SaveHatVrConfig();
        LogCategory("CONFIG", "Created %s with default settings", g_hatVrConfigPath);
    }

    const int stereoMode = HatVrReadStereoModeCompat();
    HatVrApplyStereoModeCompat(stereoMode);
    const bool firstPerson =
        HatVrReadConfigBool("FirstPerson", g_fpV1Enabled && g_avV29ProbeEnabled);
    g_theaterMode =
        HatVrReadConfigBool("TheaterMode", g_theaterMode);
    g_autoTheaterCutscenes =
        HatVrReadConfigBool("AutoCutsceneTheater", g_autoTheaterCutscenes);
    g_overrideLockedGameplayCameras =
        HatVrReadConfigBool("OverrideLockedCameras", g_overrideLockedGameplayCameras);
    g_rotationOnlyTracking =
        HatVrReadConfigBool("RotationOnly", g_rotationOnlyTracking);
    g_playStationControllerIcons =
        HatVrReadConfigBool("PlayStationIcons", g_playStationControllerIcons);
    g_nintendoSwitchControllerIcons =
        HatVrReadConfigBool("NintendoSwitchIcons", g_nintendoSwitchControllerIcons);
    // Glyph themes are mutually exclusive. Prefer the explicit Nintendo option
    // if an edited config happens to enable both.
    if (g_nintendoSwitchControllerIcons)
        g_playStationControllerIcons = false;
    g_disablePlayerFade = HatVrReadConfigBool("DisablePlayerFade", g_disablePlayerFade);
    g_sharperNativeStereo = HatVrReadConfigBool("SharperNativeStereo", false);
    g_rightHandHookshot = HatVrReadConfigBool("RightHandHookshot", g_rightHandHookshot);
    g_umbrellaMotionControls = HatVrReadConfigBool("UmbrellaMotionControls", g_umbrellaMotionControls);
    g_hatVrHudScale = HatVrReadConfigFloat("HudScale", g_hatVrHudScale);
    g_hatVrHudDistance = HatVrReadConfigFloat("HudDistance", g_hatVrHudDistance);
    g_hatVrHudHeight = HatVrReadConfigFloat("HudHeight", g_hatVrHudHeight);
    g_hatVrHudHeadLocked = HatVrReadConfigBool("HudHeadLocked", g_hatVrHudHeadLocked);
    g_spectatorView = GetPrivateProfileIntA("HatVR", "SpectatorView", g_spectatorView, g_hatVrConfigPath);
    g_spectatorUiMode = GetPrivateProfileIntA("HatVR", "SpectatorUiMode", g_spectatorUiMode, g_hatVrConfigPath);
    g_spectatorExpandedFov = HatVrReadConfigBool("SpectatorExpandedFov", false);
    g_debugToolsEnabled = GetPrivateProfileIntA("HatVR", "DebugToolsEnabled", 0, g_hatVrConfigPath) != 0;
    g_hatVrCurseCasualMenuFont =
        HatVrReadConfigBool("CurseCasualMenuFont", true);
    g_hatVrModApiEnabled = HatVrReadConfigBool("ModAPI", false);
    if (g_spectatorView < 0 || g_spectatorView > 3) g_spectatorView = 2;
    if (g_spectatorUiMode < 0 || g_spectatorUiMode > 1) g_spectatorUiMode = 1;
    if (g_hatVrHudScale < 0.50f) g_hatVrHudScale = 0.50f;
    if (g_hatVrHudScale > 2.00f) g_hatVrHudScale = 2.00f;
    if (g_hatVrHudDistance < 0.25f) g_hatVrHudDistance = 0.25f;
    if (g_hatVrHudDistance > 2.00f) g_hatVrHudDistance = 2.00f;
    if (g_hatVrHudHeight < -1.00f) g_hatVrHudHeight = -1.00f;
    if (g_hatVrHudHeight > 1.00f) g_hatVrHudHeight = 1.00f;
    g_psvr2HmdRumbleEnabled =
        HatVrReadConfigBool("PSVR2HmdRumble", g_psvr2HmdRumbleEnabled);
    g_psvr2StrongPhysicalOnly =
        HatVrReadConfigBool("PSVR2StrongPhysicalOnly", g_psvr2StrongPhysicalOnly);
    g_psvr2EyeWheelEnabled =
        HatVrReadConfigBool("PSVR2EyeHatWheel", g_psvr2EyeWheelEnabled);
    g_psvr2HmdRumbleIntensity = GetPrivateProfileIntA("HatVR", "PSVR2HmdRumbleIntensity", g_psvr2HmdRumbleIntensity, g_hatVrConfigPath);
    g_psvr2HmdRumbleCurve = GetPrivateProfileIntA("HatVR", "PSVR2HmdRumbleCurve", g_psvr2HmdRumbleCurve, g_hatVrConfigPath);
    g_psvr2HookshotAdaptiveTrigger = HatVrReadConfigBool("PSVR2HookshotAdaptiveTrigger", g_psvr2HookshotAdaptiveTrigger);
    if (g_psvr2HmdRumbleIntensity < 0) g_psvr2HmdRumbleIntensity = 0;
    if (g_psvr2HmdRumbleIntensity > 100) g_psvr2HmdRumbleIntensity = 100;
    if (g_psvr2HmdRumbleCurve < 0 || g_psvr2HmdRumbleCurve > 2) g_psvr2HmdRumbleCurve = 0;
    g_openXrUpscalingEnabled = GetPrivateProfileIntA("HatVR", "OpenXRUpscalingEnabled", g_openXrUpscalingEnabled ? 1 : 0, g_hatVrConfigPath) != 0;
    g_openXrUpscaler = GetPrivateProfileIntA("HatVR", "OpenXRUpscaler", g_openXrUpscaler, g_hatVrConfigPath);
    if (g_openXrUpscaler < 0 || g_openXrUpscaler > 1) g_openXrUpscaler = 0;
    g_openXrUpscalePercent = GetPrivateProfileIntA("HatVR", "OpenXRUpscalePercent", g_openXrUpscalePercent, g_hatVrConfigPath);
    if (g_openXrUpscalePercent < 25) g_openXrUpscalePercent = 25;
    if (g_openXrUpscalePercent > 100) g_openXrUpscalePercent = 100;
    g_openXrUpscaleSharpness = GetPrivateProfileIntA("HatVR", "OpenXRUpscaleSharpness", g_openXrUpscaleSharpness, g_hatVrConfigPath);
    if (g_openXrUpscaleSharpness < 0) g_openXrUpscaleSharpness = 0;
    if (g_openXrUpscaleSharpness > 100) g_openXrUpscaleSharpness = 100;

    // remember this until openxr is ready.
    g_firstPersonConfigured = firstPerson;

    LogCategory("CONFIG",
        "Loaded HatVR.ini stereoMode=%d native=%d sequential=%d fp=%d theater=%d autoCutscene=%d locked=%d rotationOnly=%d psIcons=%d switchIcons=%d",
        stereoMode,
        g_nativeStereoEnabled ? 1 : 0,
        g_v26SequentialReentry ? 1 : 0,
        firstPerson ? 1 : 0,
        g_theaterMode ? 1 : 0,
        g_autoTheaterCutscenes ? 1 : 0,
        g_overrideLockedGameplayCameras ? 1 : 0,
        g_rotationOnlyTracking ? 1 : 0,
        g_playStationControllerIcons ? 1 : 0,
        g_nintendoSwitchControllerIcons ? 1 : 0);
}
