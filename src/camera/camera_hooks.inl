#pragma message("HatVR PHASE8.4 camera_hooks.inl COMPILE-PROOF 2026-09-20H")

#include "../first_person/first_person.inl"
#include "../xr/mod_api_bridge.inl"

extern float g_hatVrAnimHeadCameraForwardUU;
extern float g_hatVrAnimHeadCameraUpUU;
extern bool  g_hatVrAnimHeadCameraOffsetValid;

    // Upstream gameplay camera hook.
    //
    // CalcSceneView calls HatinTimeGame.exe + 0x402EC0 with:
    //   RCX = PlayerController*
    //   RDX = FVector*  outLocation
    //   R8  = FRotator* outRotation
    //
    // we call the original first, then alter the finished gameplay POV BEFORE
    // CalcSceneView consumes it to construct the FSceneView and matrices.

    static constexpr uintptr_t kGetPlayerViewPointCandidateRva = 0x402EC0;

    using GetPlayerViewPointCandidateFn = void (*)(
        void* playerController,
        FVectorUE3* outLocation,
        FRotatorUE3* outRotation
        );

    static GetPlayerViewPointCandidateFn g_originalGetPlayerViewPointCandidate = nullptr;
    static bool g_getPlayerViewPointHookInstalled = false;

    static void HookedGetPlayerViewPointCandidate(
        void* playerController,
        FVectorUE3* outLocation,
        FRotatorUE3* outRotation)
    {
        // restore the PSVR2 eye-wheel's live PlayerController feed.
        // while the camera-ownership logic was rebuilt.
        if (playerController)
            g_hatVrEyeWheelPlayerController = playerController;

        g_originalGetPlayerViewPointCandidate(
            playerController,
            outLocation,
            outRotation
        );

        if (!outLocation || !outRotation)
            return;

        //
        // Camera ownership comes from AHiT's own ViewTarget gate. While a
        // cutscene/external view target remains active we reconcile Theater
        // against the CURRENT Auto Cutscene Theater setting every camera call,
        // so changing Cinematic <-> Immersive takes effect immediately.
        //
        // Avatar suspension is intentionally independent of that preference:
        // any external/cutscene camera hides the HatVR avatar.
        bool externalViewTarget = false;
        uintptr_t gameViewTarget = 0;
        uintptr_t possessedPawn = 0;
        const bool ownershipKnown =
            FP_V143ExternalViewTarget(
                playerController,
                externalViewTarget,
                &gameViewTarget,
                &possessedPawn);

        if (ownershipKnown && !g_hatVrCameraStateInitialized)
        {
            g_hatVrCameraStateInitialized = true;
            g_hatVrGameCameraActive = externalViewTarget;

            if (externalViewTarget)
            {
                // Save the user's pre-cutscene Theater state exactly once.
                g_hatVrCameraSavedTheater = g_theaterMode;
                g_hatVrCameraSavedAvatar = g_avV29ProbeEnabled;

                // Avatar is always hidden for a cutscene, regardless of whether
                // the user wants Cinematic Theater or Immersive presentation.
                g_avV29ProbeEnabled = false;

                // Theater follows the live preference.
                g_hatVrCameraForcedTheater = g_autoTheaterCutscenes;
                g_theaterMode = g_autoTheaterCutscenes
                    ? true
                    : g_hatVrCameraSavedTheater;
            }

            DxvkPathTrace(
                "V144 CAMERA-STATE baseline=%s target=%p pawn=%p autoTheater=%d theater=%d avatar=%d",
                externalViewTarget ? "GAME_CAMERA" : "GAMEPLAY",
                reinterpret_cast<void*>(gameViewTarget),
                reinterpret_cast<void*>(possessedPawn),
                g_autoTheaterCutscenes ? 1 : 0,
                g_theaterMode ? 1 : 0,
                g_avV29ProbeEnabled ? 1 : 0);
        }
        else if (ownershipKnown &&
                 !g_hatVrGameCameraActive &&
                 externalViewTarget)
        {
            g_hatVrGameCameraActive = true;

            // Capture user state at the transition, before HatVR changes it.
            g_hatVrCameraSavedTheater = g_theaterMode;
            g_hatVrCameraSavedAvatar = g_avV29ProbeEnabled;

            // always hide avatar in a cutscene.
            g_avV29ProbeEnabled = false;

            // apply the current cutscene presentation preference.
            g_hatVrCameraForcedTheater = g_autoTheaterCutscenes;
            g_theaterMode = g_autoTheaterCutscenes
                ? true
                : g_hatVrCameraSavedTheater;

            DxvkPathTrace(
                "V144 CAMERA-STATE GAME_CAMERA target=%p pawn=%p autoTheater=%d theater=%d avatarWas=%d",
                reinterpret_cast<void*>(gameViewTarget),
                reinterpret_cast<void*>(possessedPawn),
                g_autoTheaterCutscenes ? 1 : 0,
                g_theaterMode ? 1 : 0,
                g_hatVrCameraSavedAvatar ? 1 : 0);
        }
        else if (ownershipKnown &&
                 g_hatVrGameCameraActive &&
                 externalViewTarget)
        {
            // the cutscene is still active. Make menu changes live instead of
            // freezing the policy chosen when the cutscene began.
            g_avV29ProbeEnabled = false;

            const bool wantTheater = g_autoTheaterCutscenes;
            if (wantTheater != g_hatVrCameraForcedTheater)
            {
                g_hatVrCameraForcedTheater = wantTheater;
                g_theaterMode = wantTheater
                    ? true
                    : g_hatVrCameraSavedTheater;

                DxvkPathTrace(
                    "V144 CAMERA-POLICY LIVE autoTheater=%d theater=%d avatar=0",
                    wantTheater ? 1 : 0,
                    g_theaterMode ? 1 : 0);
            }
        }
        else if (ownershipKnown &&
                 g_hatVrGameCameraActive &&
                 !externalViewTarget)
        {
            g_hatVrGameCameraActive = false;

            // always restore the user's pre-cutscene Theater state, regardless
            // of which presentation preference was active at the end.
            g_theaterMode = g_hatVrCameraSavedTheater;
            g_hatVrCameraForcedTheater = false;

            // Restore avatar only if it was enabled before the cutscene.
            g_avV29ProbeEnabled = g_hatVrCameraSavedAvatar;
            if (g_hatVrCameraSavedAvatar)
                InterlockedExchange(&g_avV210CaptureRemaining, 180);

            DxvkPathTrace(
                "V144 CAMERA-STATE GAMEPLAY target=%p pawn=%p theater=%d avatar=%d",
                reinterpret_cast<void*>(gameViewTarget),
                reinterpret_cast<void*>(possessedPawn),
                g_theaterMode ? 1 : 0,
                g_avV29ProbeEnabled ? 1 : 0);
        }

        FVectorUE3 beforeLocation = *outLocation;
        FRotatorUE3 beforeRotation = *outRotation;

        // impossible to mistake a stale camera_hooks.inl for the basis-fix build.
        static bool phase84SessionStampWritten = false;
        if (!phase84SessionStampWritten)
        {
            phase84SessionStampWritten = true;
            DxvkPathTrace(
                "PHASE8.4 CAMERA-HOOK ACTIVE BUILD=2026-09-20H "
                "inverse-frozen-eye-basis=1 source=camera_hooks.inl");
        }

        // OpenXR view pair used by the projection hook. Previously this hook used
        // live g_xrViews/g_xrHead* while projection used g_renderPoseSnapshotViews,
        // allowing the second/right eye to mix two XR generations.
        const bool phase71FrozenPose = g_renderPoseSnapshotValid;
        const XrView* phase71Views = phase71FrozenPose ? g_renderPoseSnapshotViews : g_xrViews;
        const XrVector3f phase71HeadPosition{
            (phase71Views[0].pose.position.x + phase71Views[1].pose.position.x) * 0.5f,
            (phase71Views[0].pose.position.y + phase71Views[1].pose.position.y) * 0.5f,
            (phase71Views[0].pose.position.z + phase71Views[1].pose.position.z) * 0.5f
        };
        const XrQuaternionf phase71HeadOrientation = phase71Views[0].pose.orientation;

        // Discovery-only: this does not alter the camera yet.
        FP_OnPlayerViewPoint(playerController, *outLocation, *outRotation);
        HatVrApiDeliverTracking(playerController);

        // Remove AHiT's third-person pitch/roll. A level physical HMD starts
        // from a level UE3 world camera; HMD orientation is layered below.
        if (!g_theaterMode && g_xrTrackingPoseValid && g_vrPitchLockEnabled)
        {
            outRotation->Pitch = 0;
            outRotation->Roll = 0;
        }

        // apply the latest OpenXR head pose on top of AHiT's finished gameplay
        // camera. Translation is relative to the first valid HMD pose so putting
        // the headset on does not teleport the game camera. 1 meter = 100 UU.
        if (!g_theaterMode && g_xrTrackingPoseValid && g_xrTrackingOriginSet)
        {
            XrPosef headPose{};
            headPose.position = phase71HeadPosition;
            headPose.orientation = phase71HeadOrientation;
            const XrPosef relativeHead = XrPoseRelativeToTrackingOrigin(headPose);
            XrVector3f delta = relativeHead.position;

            if (g_rotationOnlyTracking)
            {
                delta.x = 0.0f;
                delta.y = 0.0f;
                delta.z = 0.0f;
            }

            // OpenXR local: +X right, +Y up, -Z forward.
            const float savedForward = g_cameraLocalForward;
            const float savedRight = g_cameraLocalRight;
            const float savedUp = g_cameraLocalUp;
            g_cameraLocalForward = -delta.z * g_xrWorldUnitsPerMeter;
            g_cameraLocalRight = delta.x * g_xrWorldUnitsPerMeter;
            g_cameraLocalUp = delta.y * g_xrWorldUnitsPerMeter;
            ApplyCameraLocalOffset(*outLocation, *outRotation);
            g_cameraLocalForward = savedForward;
            g_cameraLocalRight = savedRight;
            g_cameraLocalUp = savedUp;

            // No synthetic Eyes/socket rotation. Animation contributes forward/up only.
            if(g_fpV1Enabled && g_avV29ProbeEnabled &&
               g_hatVrAnimHeadCameraOffsetValid && !g_rotationOnlyTracking)
            {
                const float sf=g_cameraLocalForward;
                const float sr=g_cameraLocalRight;
                const float su=g_cameraLocalUp;
                g_cameraLocalForward=g_hatVrAnimHeadCameraForwardUU + 4.0f;
                g_cameraLocalRight=0.0f;
                g_cameraLocalUp=g_hatVrAnimHeadCameraUpUU;
                ApplyCameraLocalOffset(*outLocation,*outRotation);
                g_cameraLocalForward=sf;
                g_cameraLocalRight=sr;
                g_cameraLocalUp=su;
            }

            // Relative HMD orientation. Convert OpenXR quaternion to yaw/pitch/roll
            // in its local basis, then map those angles onto UE3 Rotator units.
            // XR_REFERENCE_SPACE_TYPE_LOCAL is already gravity-aligned.
            // do NOT redefine pitch/roll zero from whatever direction the headset
            // happened to face when tracking started. Use the LOCAL-space HMD
            // orientation directly so "level" is determined by OpenXR, not startup pose.
            const XrQuaternionf q = relativeHead.orientation;

            const float sinPitch = 2.0f * (q.w * q.x - q.y * q.z);
            const float pitchRad = asinf((std::max)(-1.0f, (std::min)(1.0f, sinPitch)));
            const float yawRad = atan2f(
                2.0f * (q.w * q.y + q.x * q.z),
                1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float rollRad = atan2f(
                2.0f * (q.w * q.z + q.x * q.y),
                1.0f - 2.0f * (q.x * q.x + q.z * q.z));

            constexpr float kRadToDeg = 57.29577951308232f;
            outRotation->Pitch += DegreesToUnrealRotator(pitchRad * kRadToDeg);
            outRotation->Yaw += DegreesToUnrealRotator(-yawRad * kRadToDeg);
            outRotation->Roll += DegreesToUnrealRotator(-rollRad * kRadToDeg);
        }

        if (g_alternatingStereoEnabled)
        {
            // Reuse the camera-local right-vector math without disturbing the
            // user's debug offsets.
            const float savedForward = g_cameraLocalForward;
            const float savedRight = g_cameraLocalRight;
            const float savedUp = g_cameraLocalUp;

            if (g_theaterMode)
            {
                // Theater stereo: keep the game's original, identical projection
                // in both eyes and introduce ONLY a parallel camera baseline.
                //
                // HatVR uses 100 UE units per meter, so the runtime-measured HMD
                // IPD maps directly into game-world scale. Using the magnitude
                // (rather than the tracked eye vector) keeps the theater baseline
                // camera-local and independent of head rotation.
                const float ipdDx =
                    phase71Views[1].pose.position.x - phase71Views[0].pose.position.x;
                const float ipdDy =
                    phase71Views[1].pose.position.y - phase71Views[0].pose.position.y;
                const float ipdDz =
                    phase71Views[1].pose.position.z - phase71Views[0].pose.position.z;
                const float theaterIpdMeters =
                    sqrtf(ipdDx * ipdDx + ipdDy * ipdDy + ipdDz * ipdDz);
                const float theaterHalfIpdUU =
                    0.5f * theaterIpdMeters * g_xrWorldUnitsPerMeter;

                g_cameraLocalForward = 0.0f;
                g_cameraLocalRight =
                    g_calcSceneRightEye ? theaterHalfIpdUU : -theaterHalfIpdUU;
                g_cameraLocalUp = 0.0f;

                static unsigned int s_theaterIpdLogs = 0;
                if (s_theaterIpdLogs < 12)
                {
                    ++s_theaterIpdLogs;
                    DxvkPathTrace(
                        "THEATER STEREO IPD eye=%s runtimeIPD=%.3fmm halfBaseline=%.4fUU worldScale=%.1fUU/m",
                        g_calcSceneRightEye ? "RIGHT" : "LEFT",
                        theaterIpdMeters * 1000.0f,
                        theaterHalfIpdUU,
                        g_xrWorldUnitsPerMeter);
                }
            }
            else if (g_xrViewsValidThisFrame)
            {
                const int eye = g_calcSceneRightEye ? 1 : 0;
                const XrVector3f midpoint = phase71HeadPosition;
                const XrVector3f eyeDelta{
                    phase71Views[eye].pose.position.x - midpoint.x,
                    phase71Views[eye].pose.position.y - midpoint.y,
                    phase71Views[eye].pose.position.z - midpoint.z
                };

                static unsigned int phase71IpdLogs = 0;
                if (phase71IpdLogs < 24)
                {
                    ++phase71IpdLogs;
                    const float dx = phase71Views[1].pose.position.x - phase71Views[0].pose.position.x;
                    const float dy = phase71Views[1].pose.position.y - phase71Views[0].pose.position.y;
                    const float dz = phase71Views[1].pose.position.z - phase71Views[0].pose.position.z;
                    const float ipdMeters = sqrtf(dx*dx + dy*dy + dz*dz);
                    char p71[640] = {};
                    sprintf_s(p71, sizeof(p71),
                        "PHASE7.1 EYE-ORIGIN eye=%s frozen=%d poseSerial=%llu IPD=%.3fmm midpoint=[%.5f %.5f %.5f] eyeDelta=[%.5f %.5f %.5f]\n",
                        eye ? "RIGHT" : "LEFT", phase71FrozenPose ? 1 : 0,
                        g_renderPoseSnapshotSerial, ipdMeters * 1000.0f,
                        midpoint.x, midpoint.y, midpoint.z, eyeDelta.x, eyeDelta.y, eyeDelta.z);
                    DebugLog(p71);
                }

                // ApplyCameraLocalOffset expects a CAMERA-LOCAL offset and rotates it
                // by the finished UE camera rotation. Feeding tracking-space directly
                // therefore applies the HMD orientation twice and makes the stereo
                // baseline swing as the headset rotates. Remove the frozen HMD
                // apply reference-local orientation first, then convert XR local axes to UE camera-local F/R/U.
                //
                // for a unit quaternion, inverse(q) == conjugate(q). Rotate v by
                // inverse(q) without introducing any extra dependency/helper.
                const XrQuaternionf& rq = phase71HeadOrientation;
                const float qLenSq = rq.x*rq.x + rq.y*rq.y + rq.z*rq.z + rq.w*rq.w;
                XrVector3f eyeLocal = eyeDelta;
                if (qLenSq > 1.0e-8f)
                {
                    const float invLenSq = 1.0f / qLenSq;
                    const float qx = -rq.x * invLenSq;
                    const float qy = -rq.y * invLenSq;
                    const float qz = -rq.z * invLenSq;
                    const float qw =  rq.w * invLenSq;

                    // q^-1 * v * q, expanded as quaternion-vector rotation.
                    const float tx = 2.0f * (qy * eyeDelta.z - qz * eyeDelta.y);
                    const float ty = 2.0f * (qz * eyeDelta.x - qx * eyeDelta.z);
                    const float tz = 2.0f * (qx * eyeDelta.y - qy * eyeDelta.x);
                    eyeLocal.x = eyeDelta.x + qw * tx + (qy * tz - qz * ty);
                    eyeLocal.y = eyeDelta.y + qw * ty + (qz * tx - qx * tz);
                    eyeLocal.z = eyeDelta.z + qw * tz + (qx * ty - qy * tx);
                }

                static unsigned int phase84BasisLogs = 0;
                if (phase84BasisLogs < 48)
                {
                    ++phase84BasisLogs;
                    char p84[768] = {};
                    sprintf_s(p84, sizeof(p84),
                        "PHASE8.4 EYE-BASIS eye=%s frozen=%d poseSerial=%llu tracking=[%.6f %.6f %.6f] local=[%.6f %.6f %.6f] localUU=[F%.5f R%.5f U%.5f]\n",
                        eye ? "RIGHT" : "LEFT", phase71FrozenPose ? 1 : 0,
                        g_renderPoseSnapshotSerial,
                        eyeDelta.x, eyeDelta.y, eyeDelta.z,
                        eyeLocal.x, eyeLocal.y, eyeLocal.z,
                        -eyeLocal.z * g_xrWorldUnitsPerMeter,
                         eyeLocal.x * g_xrWorldUnitsPerMeter,
                         eyeLocal.y * g_xrWorldUnitsPerMeter);
                    // Session logger on purpose: this must appear in AHiTVR_SESSION_*.log.
                    DxvkPathTrace("%s", p84);
                }

                // OpenXR reference-local: +X right, +Y up, -Z forward.
                g_cameraLocalForward = -eyeLocal.z * g_xrWorldUnitsPerMeter;
                g_cameraLocalRight = eyeLocal.x * g_xrWorldUnitsPerMeter;
                g_cameraLocalUp = eyeLocal.y * g_xrWorldUnitsPerMeter;
            }
            else
            {
                g_cameraLocalForward = 0.0f;
                g_cameraLocalRight = g_calcSceneRightEye ? g_stereoHalfIpdUU : -g_stereoHalfIpdUU;
                g_cameraLocalUp = 0.0f;
            }

            // basis. ApplyCameraLocalOffset is an old debug-camera helper whose
            // right/up vectors intentionally ignore Roll; that is fine for the free
            // camera, but wrong for an HMD stereo baseline. In particular, after
            // step that puts the camera-local eye offset back into UE world space.
            // Omitting Roll here leaves the two eye origins upright while the view
            // itself rolls, producing a vertical/sheared stereo error on head tilt.
            {
                constexpr double kRotToRad =
                    6.28318530717958647692 / 65536.0;
                const double pitch = static_cast<double>(outRotation->Pitch) * kRotToRad;
                const double yaw   = static_cast<double>(outRotation->Yaw)   * kRotToRad;
                const double roll  = static_cast<double>(outRotation->Roll)  * kRotToRad;

                const float cp = static_cast<float>(cos(pitch));
                const float sp = static_cast<float>(sin(pitch));
                const float cy = static_cast<float>(cos(yaw));
                const float sy = static_cast<float>(sin(yaw));
                const float cr = static_cast<float>(cos(roll));
                const float sr = static_cast<float>(sin(roll));

                const FVectorUE3 forward{ cp * cy, cp * sy, sp };
                const FVectorUE3 right{
                    sr * sp * cy - cr * sy,
                    sr * sp * sy + cr * cy,
                    -sr * cp
                };
                const FVectorUE3 up{
                    -(cr * sp * cy + sr * sy),
                    cy * sr - cr * sp * sy,
                    cr * cp
                };

                outLocation->X +=
                    forward.X * g_cameraLocalForward +
                    right.X * g_cameraLocalRight +
                    up.X * g_cameraLocalUp;
                outLocation->Y +=
                    forward.Y * g_cameraLocalForward +
                    right.Y * g_cameraLocalRight +
                    up.Y * g_cameraLocalUp;
                outLocation->Z +=
                    forward.Z * g_cameraLocalForward +
                    right.Z * g_cameraLocalRight +
                    up.Z * g_cameraLocalUp;
            }

            g_cameraLocalForward = savedForward;
            g_cameraLocalRight = savedRight;
            g_cameraLocalUp = savedUp;
        }

    }

    static bool InstallGetPlayerViewPointCandidateHook()
    {
        if (g_getPlayerViewPointHookInstalled)
            return true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe)
        {
            DebugLog("GetPlayerViewPoint candidate: GetModuleHandleW(NULL) FAILED\n");
            return false;
        }

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(exe) + kGetPlayerViewPointCandidateRva
            );

        char line[320] = {};
        sprintf_s(
            line,
            sizeof(line),
            "Installing GetPlayerViewPoint candidate hook: EXE=%p target=%p RVA=0x%llX\n",
            exe,
            target,
            static_cast<unsigned long long>(kGetPlayerViewPointCandidateRva)
        );
        DebugLog(line);

        MH_STATUS status = MH_CreateHook(
            target,
            &HookedGetPlayerViewPointCandidate,
            reinterpret_cast<void**>(&g_originalGetPlayerViewPointCandidate)
        );

        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            sprintf_s(
                line,
                sizeof(line),
                "MH_CreateHook(GetPlayerViewPoint candidate) FAILED status=%d\n",
                static_cast<int>(status)
            );
            DebugLog(line);
            return false;
        }

        status = MH_EnableHook(target);

        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            sprintf_s(
                line,
                sizeof(line),
                "MH_EnableHook(GetPlayerViewPoint candidate) FAILED status=%d\n",
                static_cast<int>(status)
            );
            DebugLog(line);
            return false;
        }

        g_getPlayerViewPointHookInstalled = true;
        DebugLog("GetPlayerViewPoint candidate hook installed\n");
        return true;
    }

