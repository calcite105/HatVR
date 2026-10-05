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

    // Eye -> Hat Wheel:
    // Use the SAME logical compositor quad transform as HatVR's UI.
    static bool ReadPsvr2EyeWheelStick(XrVector2f& out)
    {
        out = {};
        if (!g_psvr2EyeWheelEnabled || !IsHatSwapWheelOpen() ||
            !g_xrTrackingPoseValid || !EnsureHatVrPsvr2Capi() || !g_psvr2GazeStatus)
            return false;

        alignas(8) unsigned char gaze[0x148] = {};
        if (!g_psvr2GazeStatus(gaze, 0))
            return false;

        uint32_t valid = 0;
        float gx = 0.0f, gy = 0.0f, gz = 1.0f;
        memcpy(&valid, gaze + 0xFC, sizeof(valid));
        memcpy(&gx, gaze + 0x100, sizeof(gx));
        memcpy(&gy, gaze + 0x104, sizeof(gy));
        memcpy(&gz, gaze + 0x108, sizeof(gz));
        if (!valid || !isfinite(gx) || !isfinite(gy) || !isfinite(gz))
            return false;

        XrVector3f localDir{ -gx, gy, -gz };
        const float localLen = sqrtf(localDir.x*localDir.x +
                                     localDir.y*localDir.y +
                                     localDir.z*localDir.z);
        if (localLen < 0.001f)
            return false;
        localDir.x /= localLen; localDir.y /= localLen; localDir.z /= localLen;

        const float uiScale = (std::max)(0.50f, (std::min)(2.00f, g_hatVrHudScale));
        const float uiDistance = (std::max)(0.25f, (std::min)(2.00f, g_hatVrHudDistance));
        const float uiHeight = (std::max)(-1.00f, (std::min)(1.00f, g_hatVrHudHeight));
        const float halfW = 0.80f * uiScale;
        const float halfH = 0.45f * uiScale;

        XrVector3f rayOrigin{};
        XrVector3f rayDir{};
        if (g_hatVrHudHeadLocked)
        {
            // uiQuad.space == g_vkViewSpace.
            rayOrigin = {0.0f, 0.0f, 0.0f};
            rayDir = localDir;
        }
        else
        {
            // uiQuad.space == g_vkLocalSpace.
            rayOrigin = g_xrHeadPosition;
            rayDir = EyeWheelRotate(g_xrHeadOrientation, localDir);
        }

        const float panelZ = -uiDistance;
        if (fabsf(rayDir.z) < 0.0001f)
            return false;
        const float t = (panelZ - rayOrigin.z) / rayDir.z;
        if (t <= 0.0f)
            return false;

        const float hitX = rayOrigin.x + rayDir.x * t;
        const float hitY = rayOrigin.y + rayDir.y * t - uiHeight;

        // Normalize by the actual physical quad extent. This fixes the old 16:9
        // meter-space skew and makes HUD Size part of gaze selection geometry.
        float x = hitX / halfW;
        float y = hitY / halfH;

        static bool s_haveFilteredHit = false;
        static float s_filteredX = 0.0f;
        static float s_filteredY = 0.0f;
        constexpr float kJitterRadius = 0.018f;
        if (!s_haveFilteredHit)
        {
            s_filteredX = x; s_filteredY = y; s_haveFilteredHit = true;
        }
        else
        {
            const float dx = x - s_filteredX;
            const float dy = y - s_filteredY;
            if (sqrtf(dx*dx + dy*dy) >= kJitterRadius)
            {
                s_filteredX = x; s_filteredY = y;
            }
        }
        x = s_filteredX; y = s_filteredY;

        // Normalized panel-space deadzone. Keep this modest: unlike the old
        // meter-space mapping, normalized coordinates already account for the
        // 16:9 panel and do not need a large centre exclusion.
        constexpr float kPanelCenterDeadzone = 0.14f;
        if (sqrtf(x*x + y*y) < kPanelCenterDeadzone)
        {
            out = {};
            return true;
        }

        // Strong corner assist. After mapping gaze into the real normalized UI
        // panel, deliberately bend intermediate/diagonal directions toward the
        // corners so the four diagonal Hat Wheel sectors are easier to acquire.
        const float ax = fabsf(x);
        const float ay = fabsf(y);
        const float dominant = fmaxf(ax, ay);
        if (dominant > 0.0001f)
        {
            const float minor = fminf(ax, ay);
            const float diagonalness = minor / dominant; // 0=axis, 1=diagonal

            // Old mapping used 0.60. Make the assist intentionally stronger now.
            constexpr float kCornerAssist = 1.10f;
            const float boost = 1.0f + kCornerAssist * diagonalness;

            if (ax < ay)
                x *= boost;
            else if (ay < ax)
                y *= boost;
        }

        const float curvedLen = sqrtf(x*x + y*y);
        if (curvedLen < 0.0001f)
        {
            out = {};
            return true;
        }
        out.x = x / curvedLen;
        out.y = y / curvedLen;
        return true;
    }

    // keep its combined sync without directly referencing globals declared here.

