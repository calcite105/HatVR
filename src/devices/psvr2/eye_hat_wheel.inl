    // hook the game's real Hat Wheel open state.
    static int g_eyeWheelInBadgeSwapOffset = -1;
    static uintptr_t g_eyeWheelInBadgeSwapField = 0;
    static uint32_t g_eyeWheelInBadgeSwapMask = 0;

    static bool ResolveEyeWheelInBadgeSwap()
    {
        if (g_eyeWheelInBadgeSwapOffset >= 0 &&
            g_eyeWheelInBadgeSwapField &&
            g_eyeWheelInBadgeSwapMask)
            return true;

        const uintptr_t pc =
            reinterpret_cast<uintptr_t>(g_hatVrEyeWheelPlayerController);
        if (!FP_V111PlausiblePtr(pc))
            return false;

        int32_t nameIndex = -1;
        uintptr_t cls = 0, field = 0;
        if (!FP_V123FindRawNameIndex("InBadgeSwap", nameIndex) ||
            nameIndex < 0 ||
            !FP_V18ReadPtr(pc + 0x50, cls) ||
            !FP_V111PlausiblePtr(cls) ||
            !FP_V123FindFieldByRawNameIndex(cls, nameIndex, field) ||
            !FP_V111PlausiblePtr(field))
            return false;

        int32_t off = -1;
        if (!FP_ReadMemory(reinterpret_cast<const void*>(field + 0x8C),
                           &off, sizeof(off)) ||
            off < 0 || off > 0x4000)
            return false;

        // UE3 bool UProperties store a one-bit mask in the property tail.
        uint32_t mask = 0;
        for (uintptr_t q = field + 0x90; q <= field + 0xC0; q += 4)
        {
            uint32_t v = 0;
            if (!FP_ReadMemory(reinterpret_cast<const void*>(q), &v, sizeof(v)))
                continue;
            if (v && (v & (v - 1u)) == 0u)
                mask = v;
        }
        if (!mask)
            return false;

        g_eyeWheelInBadgeSwapOffset = off;
        g_eyeWheelInBadgeSwapField = field;
        g_eyeWheelInBadgeSwapMask = mask;
        return true;
    }

    static bool IsHatSwapWheelOpen()
    {
        if (!ResolveEyeWheelInBadgeSwap())
            return false;

        const uintptr_t pc =
            reinterpret_cast<uintptr_t>(g_hatVrEyeWheelPlayerController);
        if (!FP_V111PlausiblePtr(pc))
            return false;

        uint32_t bits = 0;
        if (!FP_ReadMemory(
                reinterpret_cast<const void*>(
                    pc + static_cast<uintptr_t>(g_eyeWheelInBadgeSwapOffset)),
                &bits, sizeof(bits)))
            return false;

        return (bits & g_eyeWheelInBadgeSwapMask) != 0;
    }

    static XrVector3f EyeWheelRotate(
        const XrQuaternionf& q, const XrVector3f& v)
    {
        // q * v * conjugate(q), expanded.
        const XrVector3f u{ q.x, q.y, q.z };
        const float dotUV = u.x*v.x + u.y*v.y + u.z*v.z;
        const float dotUU = u.x*u.x + u.y*u.y + u.z*u.z;
        const XrVector3f cross{
            u.y*v.z - u.z*v.y,
            u.z*v.x - u.x*v.z,
            u.x*v.y - u.y*v.x
        };
        return {
            2.0f*dotUV*u.x + (q.w*q.w-dotUU)*v.x + 2.0f*q.w*cross.x,
            2.0f*dotUV*u.y + (q.w*q.w-dotUU)*v.y + 2.0f*q.w*cross.y,
            2.0f*dotUV*u.z + (q.w*q.w-dotUU)*v.z + 2.0f*q.w*cross.z
        };
    }

    // The eye-wheel is an ANALOG stick replacement, not an eight-way D-pad.
    // Do not warp diagonals toward corners: that previously changed which
    // wedge the original AHiT hat selector chose for a given gaze direction.
    static bool g_eyeWheelFilteredValid = false;
    static float g_eyeWheelFilteredX = 0.0f;
    static float g_eyeWheelFilteredY = 0.0f;
    static float g_eyeWheelHeldAngleX = 0.0f;
    static float g_eyeWheelHeldAngleY = 0.0f;
    static bool g_eyeWheelHeldDirection = false;
    static LARGE_INTEGER g_eyeWheelLastSampleQpc{};
    static LARGE_INTEGER g_eyeWheelQpcFrequency{};

    static void EyeWheelDebugPublish(bool wheelOpen, bool gazeValid,
        float rawX, float rawY, float smoothX, float smoothY, float stickX, float stickY)
    {
        if (!g_psvr2EyeWheelDebugVisuals) return;
        HatVrEyeWheelDebugSample sample{};
        sample.wheelOpen=wheelOpen; sample.gazeValid=gazeValid;
        sample.rawX=rawX; sample.rawY=rawY;
        sample.smoothX=smoothX; sample.smoothY=smoothY;
        sample.stickX=stickX; sample.stickY=stickY;
        sample.updatedMs=GetTickCount64();
        HatVrPublishEyeWheelDebug(sample);
    }

    static void ResetPsvr2EyeWheelFilter()
    {
        g_eyeWheelFilteredValid = false;
        g_eyeWheelHeldDirection = false;
        g_eyeWheelLastSampleQpc.QuadPart = 0;
        EyeWheelDebugPublish(false, false, 0,0,0,0,0,0);
    }

    // Gaze from PSVR2 Toolkit is a combined normalized gaze ray at +0x100
    // (the validity bit is +0xFC in its 0x148-byte packet). Transform it into
    // the SAME reference space, pose and physical dimensions as HatVR's OpenXR
    // UI quad. This deliberately keeps true gaze coordinates rather than
    // trying to infer the desired hat from headset orientation alone.
    static bool ReadPsvr2EyeWheelStick(XrVector2f& out)
    {
        out = {};
        if (!g_psvr2EyeWheelEnabled || !IsHatSwapWheelOpen())
        {
            ResetPsvr2EyeWheelFilter();
            return false;
        }
        // Keep a visible calibration target even if eye tracking drops out.
        if (!g_xrTrackingPoseValid || !EnsureHatVrPsvr2Capi() || !g_psvr2GazeStatus)
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }

        alignas(8) unsigned char gaze[0x148] = {};
        if (!g_psvr2GazeStatus(gaze, 0))
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }

        uint32_t valid = 0;
        float gx = 0.0f, gy = 0.0f, gz = 1.0f;
        memcpy(&valid, gaze + 0xFC, sizeof(valid));
        memcpy(&gx, gaze + 0x100, sizeof(gx));
        memcpy(&gy, gaze + 0x104, sizeof(gy));
        memcpy(&gz, gaze + 0x108, sizeof(gz));
        if (!valid || !isfinite(gx) || !isfinite(gy) || !isfinite(gz))
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }

        // Native PSVR2-to-OpenXR conversion used by the original implementation.
        // Keep signs here; user-tunable alignment happens in panel space below.
        XrVector3f localDir{ -gx, gy, -gz };
        const float localLen = sqrtf(localDir.x*localDir.x +
                                     localDir.y*localDir.y +
                                     localDir.z*localDir.z);
        if (localLen < 0.001f)
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }
        localDir.x /= localLen; localDir.y /= localLen; localDir.z /= localLen;

        const float uiScale = (std::max)(0.50f, (std::min)(2.00f, g_hatVrHudScale));
        const float uiDistance = (std::max)(0.25f, (std::min)(2.00f, g_hatVrHudDistance));
        const float uiHeight = (std::max)(-1.00f, (std::min)(1.00f, g_hatVrHudHeight));
        const float halfW = 0.80f * uiScale; // 0.90 * (16/9) / 2
        const float halfH = 0.45f * uiScale; // 0.90 / 2

        XrVector3f rayOrigin{};
        XrVector3f rayDir{};
        if (g_hatVrHudHeadLocked)
        {
            rayOrigin = {0.0f, 0.0f, 0.0f};
            rayDir = localDir;
        }
        else
        {
            rayOrigin = g_xrHeadPosition;
            rayDir = EyeWheelRotate(g_xrHeadOrientation, localDir);
        }

        const float panelZ = -uiDistance;
        if (fabsf(rayDir.z) < 0.0001f)
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }
        const float t = (panelZ - rayOrigin.z) / rayDir.z;
        if (t <= 0.0f)
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true, false, 0,0,0,0,0,0);
            return false;
        }

        float x = (rayOrigin.x + rayDir.x*t) / halfW;
        float y = (rayOrigin.y + rayDir.y*t - uiHeight) / halfH;
        // Prevent grazing-angle rays from producing unbounded joystick vectors.
        if (!isfinite(x) || !isfinite(y) || fabsf(x)>4.0f || fabsf(y)>4.0f)
        {
            ResetPsvr2EyeWheelFilter();
            EyeWheelDebugPublish(true,false,0,0,0,0,0,0);
            return false;
        }

        // Absolute projected gaze is in normalized HUD space. The virtual stick
        // circle is centered at the tuned Hat Wheel center (NOT screen center).
        // Its radius is specified in fractions of HUD HALF-height, so its X/Y
        // mapping remains circular in real meters on a widescreen panel.
        // No diagonal weighting or arbitrary angle correction is applied.
        const float rawGazeX=x, rawGazeY=y;
        const float centerX=(float)g_psvr2EyeWheelOffsetX * 0.01f;
        const float centerY=(float)g_psvr2EyeWheelOffsetY * 0.01f;
        const float radiusY=(float)(std::max)(10,(std::min)(95,g_psvr2EyeWheelRadius))*0.01f;
        const float radiusX=radiusY * (halfH/halfW);

        if (!g_eyeWheelQpcFrequency.QuadPart)
            QueryPerformanceFrequency(&g_eyeWheelQpcFrequency);
        LARGE_INTEGER sampleQpc{};
        QueryPerformanceCounter(&sampleQpc);
        const float elapsedMs = (g_eyeWheelLastSampleQpc.QuadPart && g_eyeWheelQpcFrequency.QuadPart)
            ? (float)(1000.0*(double)(sampleQpc.QuadPart-g_eyeWheelLastSampleQpc.QuadPart)
                / (double)g_eyeWheelQpcFrequency.QuadPart) : 16.0f;
        const float dt = (std::max)(0.0f,(std::min)(100.0f,elapsedMs));
        if (!g_eyeWheelFilteredValid || elapsedMs>120.0f)
        {
            g_eyeWheelFilteredX = x;
            g_eyeWheelFilteredY = y;
            g_eyeWheelFilteredValid = true;
            g_eyeWheelHeldDirection = false;
        }
        else
        {
            // Frame-rate-independent low-pass with a tunable blend. 0 = raw,
            // 90 = very steady. No stale data persists across Hat Wheel closes.
            const float stability = (float)(std::max)(0,(std::min)(90,g_psvr2EyeWheelStability)) * 0.01f;
            const float alphaAt60 = 1.0f-stability;
            const float alpha = 1.0f-powf(1.0f-alphaAt60,dt/16.0f);
            g_eyeWheelFilteredX += (x-g_eyeWheelFilteredX)*alpha;
            g_eyeWheelFilteredY += (y-g_eyeWheelFilteredY)*alpha;
        }
        g_eyeWheelLastSampleQpc = sampleQpc;
        x = g_eyeWheelFilteredX;
        y = g_eyeWheelFilteredY;
        const float smoothedGazeX=x, smoothedGazeY=y;

        // Normalized circular joystick coordinates. The outer circle bounds
        // a physically circular calibration region on the rendered HUD quad.
        // The stick still snaps to full magnitude beyond its inner deadzone.
        const float stickCircleX=(x-centerX)/radiusX;
        const float stickCircleY=(y-centerY)/radiusY;
        const float distance=sqrtf(stickCircleX*stickCircleX + stickCircleY*stickCircleY);
        const float deadzone=(float)(std::max)(5,(std::min)(40,g_psvr2EyeWheelDeadzone))*0.01f;
        // Slight hysteresis prevents small gaze noise from bouncing between
        // idle and selecting a hat at the center of the wheel.
        if (distance < deadzone + (g_eyeWheelHeldDirection ? -0.025f : 0.025f))
        {
            g_eyeWheelHeldDirection = false;
            EyeWheelDebugPublish(true,true,rawGazeX,rawGazeY,smoothedGazeX,smoothedGazeY,0,0);
            return true; // neutral XInput stick
        }
        if (distance < 0.0001f)
        {
            EyeWheelDebugPublish(true,true,rawGazeX,rawGazeY,smoothedGazeX,smoothedGazeY,0,0);
            return true;
        }

        // Project onto the virtual stick circle, clamp to its edge and then
        // normalize to full strength. This preserves direction even when gaze
        // travels beyond the wheel instead of producing a dead input.
        const float boundedRadius=(std::min)(1.0f,distance);
        const float vxOnCircle=(stickCircleX/distance)*boundedRadius;
        const float vyOnCircle=(stickCircleY/distance)*boundedRadius;
        float vx=vxOnCircle/boundedRadius;
        float vy=vyOnCircle/boundedRadius;
        // Hold the previously selected angle inside a tiny cone. Unlike the
        // old 1.10x corner assist, this cannot bias gaze toward another hat.
        const float holdAngle = 0.060f +
            0.13f*((float)g_psvr2EyeWheelStability/90.0f); // ~3.4-10.9 degrees
        if (g_eyeWheelHeldDirection)
        {
            const float dot = vx*g_eyeWheelHeldAngleX + vy*g_eyeWheelHeldAngleY;
            if (dot > cosf(holdAngle))
            {
                vx = g_eyeWheelHeldAngleX;
                vy = g_eyeWheelHeldAngleY;
            }
            else
            {
                g_eyeWheelHeldAngleX = vx;
                g_eyeWheelHeldAngleY = vy;
            }
        }
        else
        {
            g_eyeWheelHeldAngleX = vx;
            g_eyeWheelHeldAngleY = vy;
            g_eyeWheelHeldDirection = true;
        }
        out.x = vx;
        out.y = vy;
        EyeWheelDebugPublish(true,true,rawGazeX,rawGazeY,smoothedGazeX,smoothedGazeY,vx,vy);
        return true;
    }
