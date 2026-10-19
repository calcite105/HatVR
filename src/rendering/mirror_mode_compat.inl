// Mirror Badge / Rift Collapse VR compatibility.
//
// Hat_GameManager.SetMirrorMode() / SetMirrorModeForAct() write
// Engine.GetEngine().bMirrorMode. The game's co-op path already refuses this
// mode because it breaks split-screen. Only the mirrored image is suppressed;
// Death Wish logic and act bits are not modified.
//
// IMPORTANT: The high-level native Draw `self` at exe RVA 0x5D7DF0 is
// NOT a GameViewportClient UObject in the shipped executable. V1/V2 assumed
// it was and never found the engine, as confirmed by the 2026-10-14 log.
// Instead, capture the validated gameplay ULocalPlayer argument from
// CalcSceneView and follow its UObject Outer chain to the UEngine.

static bool g_vrDisableMirrorMode = true;
static uintptr_t g_vrMirrorEngine = 0;
static uintptr_t g_vrMirrorLocalPlayer = 0;
static uintptr_t g_vrMirrorObservedLocalPlayer = 0;
static int32_t g_vrMirrorFieldOffset = -1;
static uint32_t g_vrMirrorBitMask = 0;
static ULONGLONG g_vrMirrorRetryAfter = 0;

// The known gameplay CalcSceneView has a genuine ULocalPlayer* parameter.
// This is recorded on the normal game thread; no reflection work happens here.
static void VrMirrorObserveLocalPlayer(void* localPlayer)
{
    if (g_vrDisableMirrorMode && localPlayer)
        g_vrMirrorObservedLocalPlayer = reinterpret_cast<uintptr_t>(localPlayer);
}

static bool VrMirrorClassIsOrDerivesFrom(uintptr_t object, const char* baseName)
{
    if (!FP_V111PlausiblePtr(object)) return false;
    uintptr_t cls = 0;
    if (!FP_V18ReadPtr(object + 0x50, cls)) return false;
    for (int i = 0; i < 24 && FP_V111PlausiblePtr(cls); ++i)
    {
        char name[96] = {};
        if (!FP_V110ObjectName(cls, name, sizeof(name))) return false;
        if (strcmp(name, baseName) == 0) return true;
        uintptr_t super = 0;
        if (!FP_V18ReadPtr(cls + 0x78, super) || super == cls) break;
        cls = super;
    }
    return false;
}

// Resolution failure diagnostics are limited to the first twelve attempts.
// In particular, a configuration option being true does not mean the bit was
// found or modified; a successful resolution has its own "ready" log.
static const char* g_vrMirrorResolveFailure = "not attempted";
static uintptr_t g_vrMirrorProbeObject = 0;
static uintptr_t g_vrMirrorProbeEngine = 0;
static int32_t g_vrMirrorProbeOffset = -1;
static uint32_t g_vrMirrorProbeMask = 0;

static bool VrMirrorResolveFail(const char* reason)
{
    g_vrMirrorResolveFailure = reason;
    return false;
}

static bool VrMirrorResolveFromLocalPlayer(uintptr_t localPlayer)
{
    g_vrMirrorProbeObject = localPlayer;
    g_vrMirrorProbeEngine = 0;
    g_vrMirrorProbeOffset = -1;
    g_vrMirrorProbeMask = 0;

    if (!FP_V18FindGNames())
        return VrMirrorResolveFail("GNames unavailable");

    if (!FP_V111PlausiblePtr(localPlayer) ||
        !VrMirrorClassIsOrDerivesFrom(localPlayer, "LocalPlayer"))
        return VrMirrorResolveFail("gameplay CalcSceneView player is not a LocalPlayer UObject");

    // ULocalPlayer lives within UEngine in UE3. Traverse verified UObject
    // Outer pointers rather than interpreting native Draw's `self` as UObject.
    uintptr_t outer = localPlayer;
    uintptr_t engine = 0;
    for (unsigned depth = 0; depth < 16; ++depth)
    {
        uintptr_t next = 0;
        if (!FP_V18ReadPtr(outer + 0x40, next) ||
            !FP_V111PlausiblePtr(next) || next == outer)
            break;
        if (VrMirrorClassIsOrDerivesFrom(next, "Engine"))
        {
            engine = next;
            break;
        }
        outer = next;
    }
    g_vrMirrorProbeEngine = engine;
    if (!engine)
        return VrMirrorResolveFail("LocalPlayer Outer chain has no Engine UObject");

    // Verify that the discovered Engine actually owns this gameplay player.
    // Engine.GamePlayers is TArray<LocalPlayer*> in UE3. Checking membership
    // prevents accidentally clearing a Boolean in a similarly shaped object.
    const int32_t playersOffset = FP_V124RawPropertyOffset(engine, "GamePlayers");
    g_vrMirrorProbeOffset = playersOffset;
    if (playersOffset < 0 || playersOffset > 0x4000)
        return VrMirrorResolveFail("Engine.GamePlayers property not resolved");

    struct MirrorPlayerArray { uintptr_t data; int32_t count, maximum; } players{};
    if (!FP_ReadMemory(reinterpret_cast<const void*>(engine +
            static_cast<uintptr_t>(playersOffset)), &players, sizeof(players)) ||
        !FP_V111PlausiblePtr(players.data) ||
        players.count < 1 || players.count > 32 ||
        players.maximum < players.count || players.maximum > 64)
        return VrMirrorResolveFail("Engine.GamePlayers array is invalid");

    bool foundPlayer = false;
    for (int i = 0; i < players.count; ++i)
    {
        uintptr_t entry = 0;
        if (FP_V18ReadPtr(players.data + static_cast<uintptr_t>(i) * 8, entry) &&
            entry == localPlayer)
        {
            foundPlayer = true;
            break;
        }
    }
    if (!foundPlayer)
        return VrMirrorResolveFail("Engine.GamePlayers does not contain gameplay LocalPlayer");

    int32_t nameIndex = -1;
    if (!FP_V123FindRawNameIndex("bMirrorMode", nameIndex) || nameIndex < 0)
        return VrMirrorResolveFail("bMirrorMode not found in GNames");

    uintptr_t cls = 0, property = 0;
    if (!FP_V18ReadPtr(engine + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls))
        return VrMirrorResolveFail("Engine UObject class unreadable");
    if (!FP_V123FindFieldByRawNameIndex(cls, nameIndex, property) ||
        !FP_V111PlausiblePtr(property))
        return VrMirrorResolveFail("bMirrorMode property not found in Engine class chain");
    if (!VrMirrorClassIsOrDerivesFrom(property, "BoolProperty"))
        return VrMirrorResolveFail("bMirrorMode field is not a BoolProperty");

    int32_t offset = -1;
    uint32_t mask = 0;
    if (!FP_V18ReadI32(property + 0x8C, offset))
        return VrMirrorResolveFail("cannot read bMirrorMode property offset");
    g_vrMirrorProbeOffset = offset;
    if (!FP_ReadMemory(reinterpret_cast<const void*>(property + 0xA8),
                       &mask, sizeof(mask)))
        return VrMirrorResolveFail("cannot read bMirrorMode BoolProperty mask");
    g_vrMirrorProbeMask = mask;
    if (offset < 0 || offset > 0x4000 || (offset & 3))
        return VrMirrorResolveFail("bMirrorMode property offset invalid");
    if (!mask || (mask & (mask - 1u)))
        return VrMirrorResolveFail("bMirrorMode mask at +0xA8 is not a single bit");

    g_vrMirrorEngine = engine;
    g_vrMirrorLocalPlayer = localPlayer;
    g_vrMirrorFieldOffset = offset;
    g_vrMirrorBitMask = mask;
    g_vrMirrorResolveFailure = "ready";
    return true;
}

static void ForceMirrorModeOffForVr(void* drawSelf)
{
    if (!g_vrDisableMirrorMode || !g_xrInitialized || !drawSelf)
        return;

    // Native Draw `self` is not a UObject; use the observed ULocalPlayer.
    // First gameplay frame may be skipped until CalcSceneView supplies it.
    const uintptr_t localPlayer = g_vrMirrorObservedLocalPlayer;
    if (!localPlayer) return;
    if (g_vrMirrorLocalPlayer != localPlayer || !g_vrMirrorEngine ||
        g_vrMirrorFieldOffset < 0 || !g_vrMirrorBitMask)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < g_vrMirrorRetryAfter) return;
        g_vrMirrorRetryAfter = now + 2000;
        if (!VrMirrorResolveFromLocalPlayer(localPlayer))
        {
            static unsigned failureReports = 0;
            if (g_debugToolsEnabled && failureReports++ < 4)
            {
                char playerName[96] = {}, playerClassName[96] = {};
                uintptr_t actualClass = 0;
                FP_V110ObjectName(localPlayer, playerName, sizeof(playerName));
                if (FP_V18ReadPtr(localPlayer + 0x50, actualClass))
                    FP_V110ObjectName(actualClass, playerClassName, sizeof(playerClassName));
                LogCategory("RENDER",
                    "VR mirror suppression unresolved: %s; LocalPlayer=%p obj=%s class=%s Engine=%p offset=%d mask=0x%08X",
                    g_vrMirrorResolveFailure, reinterpret_cast<void*>(localPlayer),
                    playerName, playerClassName,
                    reinterpret_cast<void*>(g_vrMirrorProbeEngine),
                    g_vrMirrorProbeOffset, g_vrMirrorProbeMask);
            }
            return; // fail closed: no unverified engine-memory writes
        }
    }

    // Only clear the mirror bit; leave all other packed engine flags intact.
    // The high-level Draw hook runs on the game thread each frame.
    // After initial reflection from CalcSceneView, mirrored-image attempts
    // are suppressed on the following Draw. Do not modify Death Wish flags.
    volatile LONG* word = reinterpret_cast<volatile LONG*>(
        g_vrMirrorEngine + static_cast<uintptr_t>(g_vrMirrorFieldOffset));
    __try
    {
        if ((*word & static_cast<LONG>(g_vrMirrorBitMask)) != 0)
        {
            InterlockedAnd(word, ~static_cast<LONG>(g_vrMirrorBitMask));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // An invalid/stale engine pointer must never crash the game.
        g_vrMirrorEngine = 0;
        g_vrMirrorLocalPlayer = 0;
        g_vrMirrorFieldOffset = -1;
        g_vrMirrorBitMask = 0;
        g_vrMirrorRetryAfter = GetTickCount64() + 2000;
    }
}
