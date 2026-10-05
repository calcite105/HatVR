//
// this module intentionally owns all first-person reverse-engineering state.
// camera_hooks.inl only calls FP_OnPlayerViewPoint() at the finished gameplay POV.
// No OpenXR frame/submission code is changed.

#define FP_Log(text) LogCategory("AVATAR", "%s", (text))

static bool g_fpV1Enabled = false;
static bool g_firstPersonConfigured = false;
// the game may overwrite that anchor later in the SAME GetPlayerViewPoint call.
static bool g_fpV136AnchorWritten = false;
static FVectorUE3 g_fpV136AnchorExpected{};
static bool g_fpV136CameraYielded = false;
static bool g_fpV136SavedTheater = false;
static bool g_fpV136ForcedTheater = false;
static ULONGLONG g_fpV1LastScanTick = 0;

static bool FP_ReadMemory(const void* address, void* output, size_t bytes)
{
    if (!address || !output || bytes == 0)
        return false;

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
        GetCurrentProcess(),
        address,
        output,
        bytes,
        &bytesRead) != FALSE && bytesRead == bytes;
}

static bool FP_TryReadPointer(const void* address, uintptr_t& value)
{
    value = 0;
    return FP_ReadMemory(address, &value, sizeof(value));
}

static bool FP_TryReadVector(const void* address, FVectorUE3& value)
{
    if (!FP_ReadMemory(address, &value, sizeof(value)))
        return false;

    return std::isfinite(value.X) &&
           std::isfinite(value.Y) &&
           std::isfinite(value.Z);
}

static float FP_DistanceSq(const FVectorUE3& a, const FVectorUE3& b)
{
    const float dx = a.X - b.X;
    const float dy = a.Y - b.Y;
    const float dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

//
// UE3 source confirms FName::Names is TArrayNoInit<FNameEntry*>.
// FNameEntry is variable-sized and can be ANSI or Unicode. read it incrementally
// and validate the embedded entry index instead of assuming a fixed readable block.

struct FP_V18TArray64
{
    uintptr_t Data;
    int32_t Count;
    int32_t Max;
};

static bool FP_V18ReadBytes(uintptr_t p, void* dst, size_t n)
{
    SIZE_T got = 0;
    return p &&
        ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(p),
                          dst, n, &got) &&
        got == n;
}

static bool FP_V18ReadPtr(uintptr_t p, uintptr_t& v)
{
    return FP_V18ReadBytes(p, &v, sizeof(v));
}

static bool FP_V18ReadI32(uintptr_t p, int32_t& v)
{
    return FP_V18ReadBytes(p, &v, sizeof(v));
}

static bool FP_V18ReadAnsiIncremental(uintptr_t p, char* out, size_t cap)
{
    if (!out || cap < 2) return false;
    size_t n = 0;
    for (; n + 1 < cap; ++n)
    {
        unsigned char c = 0;
        if (!FP_V18ReadBytes(p + n, &c, 1))
            return false;
        if (c == 0)
        {
            out[n] = 0;
            return n > 0;
        }
        if (c < 0x20 || c > 0x7e)
            return false;
        out[n] = static_cast<char>(c);
    }
    out[cap - 1] = 0;
    return false;
}

static bool FP_V18ReadWideIncremental(uintptr_t p, char* out, size_t cap)
{
    if (!out || cap < 2) return false;
    size_t n = 0;
    for (; n + 1 < cap; ++n)
    {
        uint16_t wc = 0;
        if (!FP_V18ReadBytes(p + n * 2, &wc, sizeof(wc)))
            return false;
        if (wc == 0)
        {
            out[n] = 0;
            return n > 0;
        }
        // All target reflection identifiers are ASCII even when stored wide.
        if (wc < 0x20 || wc > 0x7e)
            return false;
        out[n] = static_cast<char>(wc);
    }
    out[cap - 1] = 0;
    return false;
}

static bool FP_V18DecodeNameEntry(
    uintptr_t entry, int32_t expectedIndex, char* out, size_t cap)
{
    if (entry < 0x10000ULL || entry >= 0x0000800000000000ULL)
        return false;

    // UE3 FNameEntry begins with HashNext, then an encoded NAME_INDEX.
    // Some release configurations also carry EObjectFlags before the
    // variable-sized name payload.  Test the concrete 64-bit layouts rather
    // than arbitrary offsets:
    //
    //   +0x00 HashNext (8)
    //   +0x08 Index/state (4)
    //   +0x0C padding
    //   +0x10 name              FINAL_RELEASE/no flags
    //   +0x10 Flags (8)
    //   +0x18 name              SUPPORT_NAME_FLAGS
    //
    // the low state bit denotes Unicode in common UE3 layouts; GetIndex()
    // effectively shifts away state bits.  Accept both direct and shifted
    // forms because AHiT's exact UE3 branch is what we are discovering.
    int32_t encoded = 0;
    if (!FP_V18ReadI32(entry + 0x08, encoded))
        return false;

    const uint32_t u = static_cast<uint32_t>(encoded);
    const bool indexMatches =
        expectedIndex < 0 ||
        static_cast<uint32_t>(expectedIndex) == u ||
        static_cast<uint32_t>(expectedIndex) == (u >> 1);

    if (!indexMatches)
        return false;

    const bool wideHint = (u & 1u) != 0;
    // Exact AHiT layout recovered from HatinTimeGame.exe FName constructor
    // (RVA 0x1201F0): [entry+0x8] is encoded index/state,
    // [entry+0xC] is HashNext, and text begins at +0x14.
    static const unsigned int kPayloads[] = { 0x14 };

    for (unsigned int off : kPayloads)
    {
        if (wideHint)
        {
            if (FP_V18ReadWideIncremental(entry + off, out, cap))
                return true;
            if (FP_V18ReadAnsiIncremental(entry + off, out, cap))
                return true;
        }
        else
        {
            if (FP_V18ReadAnsiIncremental(entry + off, out, cap))
                return true;
            if (FP_V18ReadWideIncremental(entry + off, out, cap))
                return true;
        }
    }
    return false;
}

static uintptr_t g_fpV18GNamesData = 0;
static int32_t   g_fpV18GNamesCount = 0;
static bool      g_fpV18NamesAttempted = false;

static bool FP_V18TryNameIndex(int32_t index, char* out, size_t cap)
{
    if (!g_fpV18GNamesData || index < 0 || index >= g_fpV18GNamesCount)
        return false;

    uintptr_t entry = 0;
    if (!FP_V18ReadPtr(g_fpV18GNamesData + static_cast<uintptr_t>(index) * 8, entry))
        return false;

    return FP_V18DecodeNameEntry(entry, index, out, cap);
}

static bool FP_V18ValidateNamesArray(
    const FP_V18TArray64& arr,
    uintptr_t descriptorAddress,
    const char* descriptorKind)
{
    if (arr.Count < 1000 || arr.Count > 1000000 ||
        arr.Max < arr.Count || arr.Max > 1200000 ||
        arr.Data < 0x10000ULL || arr.Data >= 0x0000800000000000ULL)
        return false;

    int good = 0;
    int tested = 0;

    // Early names are particularly useful because their embedded index should
    // be stable and small.  Also sample across the table.
    for (int i = 0; i < 32 && i < arr.Count; ++i)
    {
        ++tested;
        uintptr_t entry = 0;
        if (!FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(i) * 8, entry))
            continue;
        char name[96] = {};
        if (FP_V18DecodeNameEntry(entry, i, name, sizeof(name)))
            ++good;
    }

    for (int k = 1; k <= 32; ++k)
    {
        const int i = static_cast<int>(
            (static_cast<int64_t>(arr.Count - 1) * k) / 33);
        if (i < 0 || i >= arr.Count) continue;
        ++tested;
        uintptr_t entry = 0;
        if (!FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(i) * 8, entry))
            continue;
        char name[96] = {};
        if (FP_V18DecodeNameEntry(entry, i, name, sizeof(name)))
            ++good;
    }

    if (good < 12)
        return false;

    bool sawPawn = false, sawLocation = false, sawMesh = false;
    int pawnIndex = -1, locationIndex = -1, meshIndex = -1;

    for (int32_t i = 0; i < arr.Count; ++i)
    {
        uintptr_t entry = 0;
        if (!FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(i) * 8, entry))
            continue;

        char name[96] = {};
        if (!FP_V18DecodeNameEntry(entry, i, name, sizeof(name)))
            continue;

        if (!sawPawn && strcmp(name, "Pawn") == 0)
        {
            sawPawn = true; pawnIndex = i;
        }
        else if (!sawLocation && strcmp(name, "Location") == 0)
        {
            sawLocation = true; locationIndex = i;
        }
        else if (!sawMesh && strcmp(name, "Mesh") == 0)
        {
            sawMesh = true; meshIndex = i;
        }

        if (sawPawn && sawLocation && sawMesh)
            break;
    }

    if (!(sawPawn && sawLocation))
        return false;

    g_fpV18GNamesData = arr.Data;
    g_fpV18GNamesCount = arr.Count;

    HMODULE exe = GetModuleHandleW(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    char line[768] = {};
    sprintf_s(line, sizeof(line),
        "FP_V18_GNAMES FOUND kind=%s descriptor=%p descriptorRva=0x%llX "
        "data=%p count=%d max=%d Pawn=%d Location=%d Mesh=%d score=%d/%d\n",
        descriptorKind,
        reinterpret_cast<void*>(descriptorAddress),
        (descriptorAddress >= base && descriptorAddress < base + 0x1464000ULL)
            ? static_cast<unsigned long long>(descriptorAddress - base) : 0ULL,
        reinterpret_cast<void*>(arr.Data), arr.Count, arr.Max,
        pawnIndex, locationIndex, meshIndex, good, tested);
    FP_Log(line);
    return true;
}

static bool FP_V18FindGNames()
{
    if (g_fpV18GNamesData)
        return true;
    if (g_fpV18NamesAttempted)
        return false;
    g_fpV18NamesAttempted = true;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);

    // Recovered directly from this exact HatinTimeGame.exe by following the
    // FName ANSI constructor at RVA 0x1201F0.
    //
    //   RVA 0x125CCE8 : FNameEntry** global index array
    //   RVA 0x125CCF0 : INT count
    //   RVA 0x125CCF4 : INT max
    //
    // the constructor also proves:
    //   entry + 0x08 : encoded (Index << 1) | bIsUnicode
    //   entry + 0x0C : hash-chain pointer
    //   entry + 0x14 : ANSI/WIDE name payload
    //
    // these globals are in zero-fill/BSS-style image memory beyond the raw
    // chance to see the real descriptor.
    constexpr uintptr_t kNamesDataRva  = 0x125CCE8ULL;
    constexpr uintptr_t kNamesCountRva = 0x125CCF0ULL;
    constexpr uintptr_t kNamesMaxRva   = 0x125CCF4ULL;

    FP_V18TArray64 arr{};
    if (!FP_V18ReadPtr(base + kNamesDataRva, arr.Data) ||
        !FP_V18ReadI32(base + kNamesCountRva, arr.Count) ||
        !FP_V18ReadI32(base + kNamesMaxRva, arr.Max))
    {
        FP_Log("FP_V19_GNAMES READ_FAILED at recovered executable globals.\n");
        return false;
    }

    char line[512] = {};
    sprintf_s(line, sizeof(line),
        "FP_V19_GNAMES_GLOBALS data=%p count=%d max=%d "
        "rvas=(0x%llX,0x%llX,0x%llX)\n",
        reinterpret_cast<void*>(arr.Data), arr.Count, arr.Max,
        static_cast<unsigned long long>(kNamesDataRva),
        static_cast<unsigned long long>(kNamesCountRva),
        static_cast<unsigned long long>(kNamesMaxRva));
    FP_Log(line);

    if (FP_V18ValidateNamesArray(arr, base + kNamesDataRva, "exe-exact"))
        return true;

    // if validation fails, dump the first few entries using the exact
    // constructor-derived layout.  This makes any remaining discrepancy
    // concrete instead of falling back to another memory scan.
    for (int32_t i = 0; i < arr.Count && i < 16; ++i)
    {
        uintptr_t entry = 0;
        if (!FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(i) * 8, entry))
            continue;

        int32_t encoded = -1;
        FP_V18ReadI32(entry + 0x08, encoded);
        char name[96] = {};
        const bool decoded = FP_V18DecodeNameEntry(entry, i, name, sizeof(name));

        sprintf_s(line, sizeof(line),
            "FP_V19_ENTRY index=%d ptr=%p encoded=0x%08X decoded=%d name=\"%s\"\n",
            i, reinterpret_cast<void*>(entry),
            static_cast<unsigned int>(encoded),
            decoded ? 1 : 0, decoded ? name : "");
        FP_Log(line);
    }

    FP_Log("FP_V19_GNAMES VALIDATION_FAILED -- exact globals found; no property offsets guessed.\n");
    return false;
}

//
// Exact/strongly corroborated AHiT x64 layout:
//   UObject::Name       +0x48   (FName: index, number)
//   UObject::Class      +0x50
//   UField::Next        +0x70
//   UStruct::SuperStruct+0x78   (also visible in executable class-chain code)
//   UStruct::Children   +0x80
//
// UProperty tail is tested structurally. UE3 source order after UField is
// ArrayDim, ElementSize, PropertyFlags, RepOffset/RepIndex, Category,
// ArraySizeEnum, Offset.  On this 64-bit PC build that predicts Offset +0xA0.

static bool FP_V110ReadFNameAt(uintptr_t p, char* out, size_t cap, int32_t* numberOut=nullptr)
{
    int32_t idx=-1, num=0;
    if (!FP_V18ReadI32(p, idx) || !FP_V18ReadI32(p+4, num))
        return false;
    if (numberOut) *numberOut=num;
    return FP_V18TryNameIndex(idx,out,cap);
}

static bool FP_V110ObjectName(uintptr_t obj, char* out, size_t cap)
{
    return FP_V110ReadFNameAt(obj+0x48,out,cap);
}

static bool FP_V110ObjectClass(uintptr_t obj, uintptr_t& cls)
{
    return FP_V18ReadPtr(obj+0x50,cls) &&
           cls>=0x10000ULL && cls<0x0000800000000000ULL;
}

static void FP_V110DescribeObject(uintptr_t obj,const char* tag)
{
    char n[128]={}, cn[128]={}, line[512]={};
    uintptr_t cls=0;
    const bool no=FP_V110ObjectName(obj,n,sizeof(n));
    const bool co=FP_V110ObjectClass(obj,cls);
    if (co) FP_V110ObjectName(cls,cn,sizeof(cn));
    sprintf_s(line,sizeof(line),
        "FP_V110_OBJECT tag=%s ptr=%p name=\"%s\" class=%p className=\"%s\"\n",
        tag,reinterpret_cast<void*>(obj),no?n:"?",
        reinterpret_cast<void*>(cls),co?cn:"?");
    FP_Log(line);
}

//
// producing the complete Hat_PlayerControllerSpecial -> ... -> Object chain.
// What remains unknown is the exact UField::Next offset in this AHiT build.
//
// Rather than assuming +0x70 again, test pointer-sized members of each class's
// first child. A candidate is accepted only if repeatedly following that same
// member produces valid named UObjects with valid class objects. Then search
// the coherent chain for the requested reflected field.
//
// member offset. The expected value is additionally validated against the live
// owner object (Pawn must yield a named UObject; Location a finite FVector;
// Mesh a named UObject).

static bool FP_V111PlausiblePtr(uintptr_t p)
{
    return p >= 0x10000ULL && p < 0x0000800000000000ULL;
}

static bool FP_V111LooksLikeNamedObject(uintptr_t obj, char* name, size_t nameCap,
                                        char* className, size_t classCap)
{
    if (!FP_V111PlausiblePtr(obj))
        return false;

    uintptr_t cls = 0;
    if (!FP_V110ObjectName(obj, name, nameCap) ||
        !FP_V110ObjectClass(obj, cls) ||
        !FP_V110ObjectName(cls, className, classCap))
        return false;

    return name[0] != '\0' && className[0] != '\0';
}

static int FP_V111ScoreNextOffset(uintptr_t first, unsigned int nextOff)
{
    uintptr_t cur = first;
    uintptr_t seen[24] = {};
    int seenCount = 0;
    int score = 0;

    for (int i = 0; i < 24 && cur; ++i)
    {
        if (!FP_V111PlausiblePtr(cur))
            break;

        for (int j = 0; j < seenCount; ++j)
            if (seen[j] == cur)
                return score;
        if (seenCount < 24) seen[seenCount++] = cur;

        char n[96] = {}, cn[96] = {};
        if (!FP_V111LooksLikeNamedObject(cur, n, sizeof(n), cn, sizeof(cn)))
            break;

        ++score;

        uintptr_t next = 0;
        if (!FP_V18ReadPtr(cur + nextOff, next))
            break;
        if (!next)
            break;
        cur = next;
    }
    return score;
}

static bool FP_V111DiscoverNextOffset(uintptr_t first, unsigned int& nextOffOut)
{
    nextOffOut = 0;
    int bestScore = 0;
    unsigned int bestOff = 0;
    char line[512] = {};

    // UObject's proven fields occupy through +0x50. Search the following
    // pointer-sized region only; this is a structural UField test, not an
    // arbitrary process-memory crawl.
    for (unsigned int off = 0x58; off <= 0xA0; off += 8)
    {
        const int score = FP_V111ScoreNextOffset(first, off);
        if (score > 0)
        {
            sprintf_s(line, sizeof(line),
                "FP_V111_NEXT_CANDIDATE first=%p off=0x%X score=%d\n",
                reinterpret_cast<void*>(first), off, score);
            FP_Log(line);
        }
        if (score > bestScore)
        {
            bestScore = score;
            bestOff = off;
        }
    }

    if (bestScore >= 2)
    {
        nextOffOut = bestOff;
        sprintf_s(line, sizeof(line),
            "FP_V111_NEXT_SELECTED first=%p off=0x%X score=%d\n",
            reinterpret_cast<void*>(first), bestOff, bestScore);
        FP_Log(line);
        return true;
    }

    FP_Log("FP_V111_NEXT_FAIL no coherent UField chain candidate.\n");
    return false;
}

enum FP_V111ValueKind
{
    FP_V111_Value_Object,
    FP_V111_Value_Vector
};

static bool FP_V111ValidateLiveOffset(uintptr_t owner, int32_t off,
                                      FP_V111ValueKind kind,
                                      char* detail, size_t detailCap)
{
    if (off < 0 || off >= 0x10000)
        return false;

    if (kind == FP_V111_Value_Vector)
    {
        FVectorUE3 v{};
        if (!FP_TryReadVector(reinterpret_cast<void*>(owner + static_cast<uintptr_t>(off)), v))
            return false;
        if (fabsf(v.X) > 10000000.0f || fabsf(v.Y) > 10000000.0f || fabsf(v.Z) > 10000000.0f)
            return false;
        sprintf_s(detail, detailCap, "vector=(%.3f %.3f %.3f)", v.X, v.Y, v.Z);
        return true;
    }

    uintptr_t p = 0;
    if (!FP_V18ReadPtr(owner + static_cast<uintptr_t>(off), p) || !FP_V111PlausiblePtr(p))
        return false;

    char n[96] = {}, cn[96] = {};
    if (!FP_V111LooksLikeNamedObject(p, n, sizeof(n), cn, sizeof(cn)))
        return false;

    sprintf_s(detail, detailCap, "object=%p name=\"%s\" class=\"%s\"",
              reinterpret_cast<void*>(p), n, cn);
    return true;
}

static bool FP_V111DiscoverPropertyOffset(uintptr_t property, uintptr_t liveOwner,
                                          FP_V111ValueKind kind,
                                          int32_t& offsetOut)
{
    offsetOut = -1;
    char line[768] = {};
    int validCount = 0;
    int32_t sole = -1;

    // property tail and validate each candidate against the live owner.
    for (unsigned int memberOff = 0x70; memberOff <= 0xD0; memberOff += 4)
    {
        int32_t candidate = -1;
        if (!FP_V18ReadI32(property + memberOff, candidate))
            continue;

        char detail[256] = {};
        if (!FP_V111ValidateLiveOffset(liveOwner, candidate, kind, detail, sizeof(detail)))
            continue;

        ++validCount;
        sole = candidate;
        sprintf_s(line, sizeof(line),
            "FP_V111_PROP_OFFSET_CANDIDATE property=%p memberOff=0x%X "
            "value=0x%X(%d) %s\n",
            reinterpret_cast<void*>(property), memberOff,
            static_cast<unsigned int>(candidate), candidate, detail);
        FP_Log(line);
    }

    if (validCount == 1)
    {
        offsetOut = sole;
        sprintf_s(line, sizeof(line),
            "FP_V111_PROP_OFFSET_SELECTED property=%p value=0x%X(%d)\n",
            reinterpret_cast<void*>(property),
            static_cast<unsigned int>(sole), sole);
        FP_Log(line);
        return true;
    }

    sprintf_s(line, sizeof(line),
        "FP_V111_PROP_OFFSET_UNRESOLVED property=%p candidates=%d\n",
        reinterpret_cast<void*>(property), validCount);
    FP_Log(line);
    return false;
}

static bool FP_V111FindField(uintptr_t startClass, const char* wanted,
                             uintptr_t& fieldOut, unsigned int& nextOffOut)
{
    fieldOut = 0;
    nextOffOut = 0;
    char line[768] = {};
    uintptr_t cls = startClass;

    for (int depth = 0; cls && depth < 32; ++depth)
    {
        char clsName[128] = {};
        FP_V110ObjectName(cls, clsName, sizeof(clsName));

        uintptr_t children = 0, super = 0;
        FP_V18ReadPtr(cls + 0x80, children);
        FP_V18ReadPtr(cls + 0x78, super);

        sprintf_s(line, sizeof(line),
            "FP_V111_CLASS depth=%d class=%p name=\"%s\" children=%p super=%p\n",
            depth, reinterpret_cast<void*>(cls), clsName,
            reinterpret_cast<void*>(children), reinterpret_cast<void*>(super));
        FP_Log(line);

        if (FP_V111PlausiblePtr(children))
        {
            unsigned int nextOff = 0;
            if (FP_V111DiscoverNextOffset(children, nextOff))
            {
                uintptr_t field = children;
                uintptr_t seen[4096] = {};
                int seenCount = 0;

                for (int guard = 0; field && guard < 4096; ++guard)
                {
                    if (!FP_V111PlausiblePtr(field))
                        break;
                    bool duplicate = false;
                    for (int j = 0; j < seenCount; ++j)
                        if (seen[j] == field) { duplicate = true; break; }
                    if (duplicate) break;
                    if (seenCount < 4096) seen[seenCount++] = field;

                    char fieldName[128] = {}, typeName[128] = {};
                    if (!FP_V111LooksLikeNamedObject(field, fieldName, sizeof(fieldName),
                                                     typeName, sizeof(typeName)))
                        break;

                    if (guard < 12 || strcmp(fieldName, wanted) == 0)
                    {
                        sprintf_s(line, sizeof(line),
                            "FP_V111_FIELD owner=\"%s\" n=%d field=%p "
                            "name=\"%s\" type=\"%s\" nextOff=0x%X\n",
                            clsName, guard, reinterpret_cast<void*>(field),
                            fieldName, typeName, nextOff);
                        FP_Log(line);
                    }

                    if (strcmp(fieldName, wanted) == 0)
                    {
                        fieldOut = field;
                        nextOffOut = nextOff;
                        sprintf_s(line, sizeof(line),
                            "FP_V111_FIELD_MATCH wanted=\"%s\" owner=\"%s\" "
                            "field=%p type=\"%s\" nextOff=0x%X\n",
                            wanted, clsName, reinterpret_cast<void*>(field),
                            typeName, nextOff);
                        FP_Log(line);
                        return true;
                    }

                    uintptr_t next = 0;
                    if (!FP_V18ReadPtr(field + nextOff, next) || next == field)
                        break;
                    field = next;
                }
            }
        }

        if (super == cls || !FP_V111PlausiblePtr(super))
            break;
        cls = super;
    }

    return false;
}

static void FP_DiscoverPawnCandidates(
    void* playerController,
    const FVectorUE3& cameraLocation,
    const FRotatorUE3& cameraRotation)
{
    char line[768] = {};
    sprintf_s(line, sizeof(line),
        "\nFP_V111_SCAN pc=%p camera=(%.3f %.3f %.3f) rot=(%d %d %d)\n",
        playerController, cameraLocation.X, cameraLocation.Y, cameraLocation.Z,
        cameraRotation.Pitch, cameraRotation.Yaw, cameraRotation.Roll);
    FP_Log(line);

    if (!FP_V18FindGNames())
    {
        FP_Log("FP_V111_REFLECTION namesReady=0\n");
        return;
    }
    FP_Log("FP_V111_REFLECTION namesReady=1\n");

    const uintptr_t pc = reinterpret_cast<uintptr_t>(playerController);
    FP_V110DescribeObject(pc, "PlayerController");

    uintptr_t pcClass = 0;
    if (!FP_V110ObjectClass(pc, pcClass))
    {
        FP_Log("FP_V111_FAIL no valid PlayerController class pointer.\n");
        return;
    }

    uintptr_t pawnProp = 0;
    unsigned int pawnNextOff = 0;
    if (!FP_V111FindField(pcClass, "Pawn", pawnProp, pawnNextOff))
    {
        FP_Log("FP_V111_FAIL reflected field Pawn not found.\n");
        return;
    }

    int32_t pawnOffset = -1;
    if (!FP_V111DiscoverPropertyOffset(pawnProp, pc, FP_V111_Value_Object, pawnOffset))
    {
        FP_Log("FP_V111_FAIL Pawn property data offset unresolved.\n");
        return;
    }

    uintptr_t pawn = 0;
    if (!FP_V18ReadPtr(pc + static_cast<uintptr_t>(pawnOffset), pawn) ||
        !FP_V111PlausiblePtr(pawn))
    {
        FP_Log("FP_V111_FAIL Pawn pointer unreadable after selected offset.\n");
        return;
    }

    sprintf_s(line, sizeof(line),
        "FP_V111_PAWN SUCCESS property=%p dataOffset=0x%X pawn=%p nextOff=0x%X\n",
        reinterpret_cast<void*>(pawnProp), pawnOffset,
        reinterpret_cast<void*>(pawn), pawnNextOff);
    FP_Log(line);
    FP_V110DescribeObject(pawn, "Pawn");

    uintptr_t pawnClass = 0;
    if (!FP_V110ObjectClass(pawn, pawnClass))
        return;

    uintptr_t locProp = 0, meshProp = 0;
    unsigned int locNext = 0, meshNext = 0;
    int32_t locOffset = -1, meshOffset = -1;

    const bool locField = FP_V111FindField(pawnClass, "Location", locProp, locNext);
    const bool meshField = FP_V111FindField(pawnClass, "Mesh", meshProp, meshNext);

    bool gotLoc = false, gotMesh = false;
    if (locField)
        gotLoc = FP_V111DiscoverPropertyOffset(locProp, pawn, FP_V111_Value_Vector, locOffset);
    if (meshField)
        gotMesh = FP_V111DiscoverPropertyOffset(meshProp, pawn, FP_V111_Value_Object, meshOffset);

    FVectorUE3 pawnLocation{};
    bool locReadable = gotLoc &&
        FP_TryReadVector(reinterpret_cast<void*>(pawn + static_cast<uintptr_t>(locOffset)), pawnLocation);

    uintptr_t mesh = 0;
    bool meshReadable = gotMesh &&
        FP_V18ReadPtr(pawn + static_cast<uintptr_t>(meshOffset), mesh);

    sprintf_s(line, sizeof(line),
        "FP_V111_RESULT pawn=%p pawnOffset=0x%X "
        "Location(field=%d off=0x%X read=%d value=(%.3f %.3f %.3f)) "
        "Mesh(field=%d off=0x%X read=%d ptr=%p) "
        "cameraDelta=(%.3f %.3f %.3f)\n",
        reinterpret_cast<void*>(pawn), pawnOffset,
        locField ? 1 : 0, locOffset, locReadable ? 1 : 0,
        pawnLocation.X, pawnLocation.Y, pawnLocation.Z,
        meshField ? 1 : 0, meshOffset, meshReadable ? 1 : 0,
        reinterpret_cast<void*>(mesh),
        cameraLocation.X - pawnLocation.X,
        cameraLocation.Y - pawnLocation.Y,
        cameraLocation.Z - pawnLocation.Z);
    FP_Log(line);

    FP_Log("FP_V111_SCAN_END -- targeted UField/UProperty discovery; camera unchanged.\n");
}

static bool FP_V112ResolvePawnAndLocation(
    void* playerController,
    uintptr_t& pawnOut,
    FVectorUE3& pawnLocationOut)
{
    pawnOut = 0;
    pawnLocationOut = {};
    if (!playerController)
        return false;

    //   Controller.Pawn  = +0x2FC
    //   Actor.Location   = +0x080
    constexpr uintptr_t kControllerPawnOffset = 0x2FC;
    constexpr uintptr_t kActorLocationOffset  = 0x080;

    uintptr_t pawn = 0;
    if (!FP_TryReadPointer(
            reinterpret_cast<const unsigned char*>(playerController) + kControllerPawnOffset,
            pawn) || !pawn)
        return false;

    // Controller+0x2FC is Hat_Player_HatKid. Keep only cheap pointer/range
    // validation here so this remains safe during unpossess/transitions.
    if (pawn < 0x10000ULL || pawn >= 0x0000800000000000ULL)
        return false;

    FVectorUE3 pawnLocation{};
    if (!FP_TryReadVector(
            reinterpret_cast<const unsigned char*>(pawn) + kActorLocationOffset,
            pawnLocation))
        return false;

    // Reject nonsense while still allowing AHiT's large world coordinates.
    constexpr float kMaxAbsWorld = 10000000.0f;
    if (fabsf(pawnLocation.X) > kMaxAbsWorld ||
        fabsf(pawnLocation.Y) > kMaxAbsWorld ||
        fabsf(pawnLocation.Z) > kMaxAbsWorld)
        return false;

    pawnOut = pawn;
    pawnLocationOut = pawnLocation;
    return true;
}

static bool FP_V115ReadActorRotation(uintptr_t actor, FRotatorUE3& rotationOut)
{
    // Actor.Location is +0x80 in this exact build. Actor.Rotation follows at +0x8C.
    constexpr uintptr_t kActorRotationOffset = 0x08C;
    if (actor < 0x10000ULL || actor >= 0x0000800000000000ULL)
        return false;

    FRotatorUE3 r{};
    if (!FP_ReadMemory(reinterpret_cast<const void*>(actor + kActorRotationOffset),
                       &r, sizeof(r)))
        return false;

    constexpr int kReasonable = 0x01000000;
    if (r.Pitch < -kReasonable || r.Pitch > kReasonable ||
        r.Yaw   < -kReasonable || r.Yaw   > kReasonable ||
        r.Roll  < -kReasonable || r.Roll  > kReasonable)
        return false;

    rotationOut = r;
    return true;
}

static bool FP_V115FacePawnToController(uintptr_t pawn, int controllerYaw)
{
    // match FaceCamera behavior here: keep pitch/roll and use the controller yaw.
    // only used by first person.
    constexpr uintptr_t kActorRotationOffset = 0x08C;

    FRotatorUE3 pawnRot{};
    if (!FP_V115ReadActorRotation(pawn, pawnRot))
        return false;

    pawnRot.Yaw = controllerYaw;

    SIZE_T written = 0;
    return WriteProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<void*>(pawn + kActorRotationOffset),
        &pawnRot,
        sizeof(pawnRot),
        &written) != FALSE && written == sizeof(pawnRot);
}

struct FP_V116TArrayPtrs
{
    uintptr_t Data;
    int32_t Count;
    int32_t Max;
};

static int32_t FP_V117FindPropertyDataOffset(uintptr_t object, const char* wanted)
{
    // Reuse the reflection walker that already proved itself on this exact
    // executable. In particular, do NOT assume UField::Next is always +0x60:
    // FP_V111FindField discovers the correct link offset per class chain.
    uintptr_t cls = 0;
    if (!FP_V110ObjectClass(object, cls))
        return -1;

    uintptr_t property = 0;
    unsigned int nextOff = 0;
    if (!FP_V111FindField(cls, wanted, property, nextOff) ||
        !FP_V111PlausiblePtr(property))
        return -1;

    // UProperty::Offset is hard-validated at +0x8C for this build.
    int32_t dataOffset = -1;
    if (!FP_V18ReadI32(property + 0x8C, dataOffset) ||
        dataOffset < 0 || dataOffset >= 0x10000)
        return -1;

    char line[384] = {};
    sprintf_s(line, sizeof(line),
        "FP_V117_PROPERTY wanted=\"%s\" property=%p nextOff=0x%X dataOffset=0x%X\n",
        wanted, reinterpret_cast<void*>(property), nextOff,
        static_cast<unsigned int>(dataOffset));
    FP_Log(line);
    return dataOffset;
}

static bool FP_V123FindRawNameIndex(const char* wanted, int32_t& indexOut);
static bool FP_V123ObjectRawNameIndex(uintptr_t object, int32_t& indexOut);
static bool FP_V123FindFieldByRawNameIndex(
    uintptr_t startClass, int32_t wantedIndex, uintptr_t& fieldOut);
static int32_t FP_V124RawPropertyOffset(uintptr_t object, const char* wanted);
static bool FP_V125RawNameFromIndex(
    int32_t index, char* out, size_t outSize);
static int32_t FP_V124FindIsRemovedOffsetCached(uintptr_t mode);

static int32_t g_fpV116PlayerCameraOffset = -2;
static int32_t g_fpV116CameraModesOffset = -2;
static uintptr_t g_fpV116Camera = 0;

static bool g_fpV131AutoSuspended = false;
static bool g_fpV131SavedTheaterMode = false;
static int g_fpV131TakeoverFrames = 0;
static int g_fpV131ReturnFrames = 0;
static int32_t g_fpV131CameraPriorityNameIndex = -2;
static int32_t g_fpV131CameraPriorityOffset = -2;
static ULONGLONG g_fpV131LastFocusLog = 0;
static int32_t FP_V131CameraPriorityOffset(uintptr_t mode)
{
    if (g_fpV131CameraPriorityOffset != -2)
        return g_fpV131CameraPriorityOffset;

    g_fpV131CameraPriorityOffset = -1;
    if (g_fpV131CameraPriorityNameIndex == -2)
    {
        g_fpV131CameraPriorityNameIndex = -1;
        FP_V123FindRawNameIndex("CameraPriority",
            g_fpV131CameraPriorityNameIndex);
    }
    if (g_fpV131CameraPriorityNameIndex < 0 ||
        !FP_V111PlausiblePtr(mode))
        return -1;

    uintptr_t cls=0, prop=0;
    if (FP_V18ReadPtr(mode+0x50,cls) && FP_V111PlausiblePtr(cls) &&
        FP_V123FindFieldByRawNameIndex(
            cls,g_fpV131CameraPriorityNameIndex,prop) &&
        FP_V111PlausiblePtr(prop))
    {
        int32_t off=-1;
        if (FP_V18ReadI32(prop+0x8C,off) && off>=0 && off<0x10000)
            g_fpV131CameraPriorityOffset=off;
    }
    return g_fpV131CameraPriorityOffset;
}

static uint64_t g_fpV133LastModeSignature = 0;
static uintptr_t g_fpV133LastCamera = 0;

static void FP_V133TraceCameraModeTransitions(uintptr_t camera)
{
    if (!FP_V111PlausiblePtr(camera) || g_fpV116CameraModesOffset < 0)
        return;

    FP_V116TArrayPtrs arr{};
    const uintptr_t aa = camera + (uintptr_t)g_fpV116CameraModesOffset;
    if (!FP_ReadMemory((const void*)aa,&arr,sizeof(arr)) ||
        arr.Count < 0 || arr.Count > 256 || arr.Max < arr.Count ||
        (arr.Count > 0 && !FP_V111PlausiblePtr(arr.Data)))
        return;

    uint64_t sig = 1469598103934665603ULL;
    for (int i=0;i<arr.Count;++i)
    {
        uintptr_t mode=0;
        FP_V18ReadPtr(arr.Data+(uintptr_t)i*8,mode);
        sig ^= (uint64_t)mode; sig *= 1099511628211ULL;
        if (FP_V111PlausiblePtr(mode))
        {
            const int32_t po=FP_V131CameraPriorityOffset(mode);
            int32_t pri=-9999;
            if(po>=0) FP_V18ReadI32(mode+(uintptr_t)po,pri);
            sig ^= (uint32_t)pri; sig *= 1099511628211ULL;
            const int32_t ro=FP_V124FindIsRemovedOffsetCached(mode);
            unsigned char removed=0;
            if(ro>=0) FP_ReadMemory((const void*)(mode+(uintptr_t)ro),&removed,1);
            sig ^= removed; sig *= 1099511628211ULL;
        }
    }

    if(camera==g_fpV133LastCamera && sig==g_fpV133LastModeSignature)
        return;
    g_fpV133LastCamera=camera;
    g_fpV133LastModeSignature=sig;

    char line[512]{};
    sprintf_s(line,sizeof(line),
        "FP_V133_CAMERA_MODE_CHANGE camera=%p count=%d sig=%016llX\n",
        (void*)camera,arr.Count,(unsigned long long)sig);
    FP_Log(line);

    for(int i=0;i<arr.Count;++i)
    {
        uintptr_t mode=0, cls=0;
        if(!FP_V18ReadPtr(arr.Data+(uintptr_t)i*8,mode) ||
           !FP_V111PlausiblePtr(mode))
            continue;
        char name[160]{};
        FP_V18ReadPtr(mode+0x50,cls);
        if(FP_V111PlausiblePtr(cls))
            FP_V110ObjectName(cls,name,sizeof(name));
        const int32_t po=FP_V131CameraPriorityOffset(mode);
        int32_t pri=-9999;
        if(po>=0) FP_V18ReadI32(mode+(uintptr_t)po,pri);
        const int32_t ro=FP_V124FindIsRemovedOffsetCached(mode);
        unsigned char removed=0;
        if(ro>=0) FP_ReadMemory((const void*)(mode+(uintptr_t)ro),&removed,1);
        sprintf_s(line,sizeof(line),
            "FP_V133_MODE i=%d mode=%p class=\"%s\" priority=%d removed=%d\n",
            i,(void*)mode,name,pri,removed?1:0);
        FP_Log(line);
    }
}

static bool FP_V131HigherPriorityCameraOwnsFocus(
    uintptr_t camera, int* priorityOut=nullptr,
    char* classOut=nullptr, size_t classOutSize=0)
{
    if (priorityOut) *priorityOut=-1;
    if (classOut && classOutSize) classOut[0]=0;
    if (!FP_V111PlausiblePtr(camera) || g_fpV116CameraModesOffset < 0)
        return false;

    FP_V116TArrayPtrs arr{};
    const uintptr_t aa=camera+(uintptr_t)g_fpV116CameraModesOffset;
    if (!FP_ReadMemory((const void*)aa,&arr,sizeof(arr)) ||
        !FP_V111PlausiblePtr(arr.Data) || arr.Count<0 || arr.Count>256 ||
        arr.Max<arr.Count || arr.Max>512)
        return false;

    int best=-2147483647;
    uintptr_t bestMode=0;
    for(int i=0;i<arr.Count;++i)
    {
        uintptr_t mode=0;
        if(!FP_V18ReadPtr(arr.Data+(uintptr_t)i*8,mode) ||
           !FP_V111PlausiblePtr(mode))
            continue;

        const int32_t roff=FP_V124FindIsRemovedOffsetCached(mode);
        if(roff>=0)
        {
            unsigned char removed=0;
            if(FP_ReadMemory((const void*)(mode+(uintptr_t)roff),&removed,1) &&
               removed)
                continue;
        }

        const int32_t poff=FP_V131CameraPriorityOffset(mode);
        int32_t priority=0;
        if(poff<0 || !FP_V18ReadI32(mode+(uintptr_t)poff,priority))
            continue;

        if(priority>best){ best=priority; bestMode=mode; }
    }

    if(priorityOut) *priorityOut=best;
    if(bestMode && classOut && classOutSize)
    {
        uintptr_t cls=0; int32_t ni=-1;
        if(FP_V18ReadPtr(bestMode+0x50,cls) && FP_V111PlausiblePtr(cls) &&
           FP_V123ObjectRawNameIndex(cls,ni))
            FP_V125RawNameFromIndex(ni,classOut,classOutSize);
    }

    return best > 150;
}

// ProcessEvent helper and avatar globals in this one-TU .inl.
static bool FP_V127CallUFunctionParams(
    uintptr_t object, uintptr_t function, void* params);
extern bool g_avV29ProbeEnabled;
extern volatile LONG g_avV210CaptureRemaining;

static uintptr_t g_fpV137GetFocusCameraModeFn = 0;
static bool g_fpV137GetFocusCameraModeResolved = false;
static int32_t g_fpV137ReturnValueNameIndex = -2;
static int32_t g_fpV137ReturnValueOffset = -2;

static bool FP_V137ResolveGetFocusCameraMode(uintptr_t camera)
{
    if (g_fpV137GetFocusCameraModeResolved)
        return FP_V111PlausiblePtr(g_fpV137GetFocusCameraModeFn) &&
               g_fpV137ReturnValueOffset >= 0;

    g_fpV137GetFocusCameraModeResolved = true;

    int32_t fnName = -1;
    if (!FP_V123FindRawNameIndex("GetFocusCameraMode", fnName) || fnName < 0)
        return false;

    uintptr_t cls = 0, fn = 0;
    if (!FP_V18ReadPtr(camera + 0x50, cls) || !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, fnName, fn) ||
        !FP_V111PlausiblePtr(fn))
        return false;

    if (g_fpV137ReturnValueNameIndex == -2)
    {
        g_fpV137ReturnValueNameIndex = -1;
        FP_V123FindRawNameIndex("ReturnValue", g_fpV137ReturnValueNameIndex);
    }
    if (g_fpV137ReturnValueNameIndex < 0)
        return false;

    uintptr_t children = 0;
    if (!FP_V18ReadPtr(fn + 0x80, children) || !FP_V111PlausiblePtr(children))
        return false;

    uintptr_t queue[128] = {};
    int qr = 0, qw = 0;
    queue[qw++] = children;
    int32_t returnOff = -1;

    while (qr < qw && qr < 128)
    {
        const uintptr_t field = queue[qr++];
        if (!FP_V111PlausiblePtr(field))
            continue;

        int32_t ni = -1;
        if (FP_V123ObjectRawNameIndex(field, ni) &&
            ni == g_fpV137ReturnValueNameIndex)
        {
            if (FP_V18ReadI32(field + 0x8C, returnOff) &&
                returnOff >= 0 && returnOff <= 0xF8)
                break;
            returnOff = -1;
        }

        const unsigned offsets[2] = {0x58, 0x60};
        for (unsigned off : offsets)
        {
            uintptr_t next = 0;
            if (FP_V18ReadPtr(field + off, next) &&
                FP_V111PlausiblePtr(next) && next != field && qw < 128)
                queue[qw++] = next;
        }
    }

    if (returnOff < 0)
        return false;

    g_fpV137GetFocusCameraModeFn = fn;
    g_fpV137ReturnValueOffset = returnOff;

    char line[320]{};
    sprintf_s(line, sizeof(line),
        "FP_V137_GET_FOCUS_BOUND fn=%p returnOff=0x%X camera=%p\n",
        reinterpret_cast<void*>(fn), (unsigned)returnOff,
        reinterpret_cast<void*>(camera));
    FP_Log(line);
    return true;
}

static bool FP_V137GetActualFocusMode(
    uintptr_t camera, uintptr_t& modeOut, int& priorityOut,
    char* classOut, size_t classOutSize)
{
    modeOut = 0;
    priorityOut = -1;
    if (classOut && classOutSize) classOut[0] = 0;

    if (!FP_V137ResolveGetFocusCameraMode(camera))
        return false;

    alignas(16) unsigned char params[256] = {};
    if (!FP_V127CallUFunctionParams(
            camera, g_fpV137GetFocusCameraModeFn, params))
        return false;

    uintptr_t mode = 0;
    memcpy(&mode, params + g_fpV137ReturnValueOffset, sizeof(mode));
    if (!FP_V111PlausiblePtr(mode))
        return true;

    modeOut = mode;

    const int32_t priorityOff = FP_V131CameraPriorityOffset(mode);
    if (priorityOff >= 0)
        FP_V18ReadI32(mode + (uintptr_t)priorityOff, priorityOut);

    if (classOut && classOutSize)
    {
        uintptr_t modeClass = 0;
        int32_t ni = -1;
        if (FP_V18ReadPtr(mode + 0x50, modeClass) &&
            FP_V111PlausiblePtr(modeClass) &&
            FP_V123ObjectRawNameIndex(modeClass, ni))
            FP_V125RawNameFromIndex(ni, classOut, classOutSize);
    }
    return true;
}

static void FP_V137UpdateWorkshopEquivalentFocus(uintptr_t camera)
{
    if (!g_fpV1Enabled || !FP_V111PlausiblePtr(camera))
        return;

    uintptr_t focusMode = 0;
    int priority = -1;
    char cls[128] = {};
    if (!FP_V137GetActualFocusMode(
            camera, focusMode, priority, cls, sizeof(cls)))
        return;

    // Workshop mode priority is exactly 150. Because GetFocusCameraMode()
    // already applied AHiT's IsRelevant/IsRemoved/focus rules, this comparison
    // is against the game's ACTUAL focus owner, not every live array entry.
    const bool shouldSuspend = focusMode && priority > 150;
    if (shouldSuspend == g_fpV131AutoSuspended)
        return;

    g_fpV131AutoSuspended = shouldSuspend;

    if (shouldSuspend)
    {
        g_fpV131SavedTheaterMode = g_theaterMode;
        g_avV29ProbeEnabled = false;
        if (g_autoTheaterCutscenes)
            g_theaterMode = true;

        char line[384]{};
        sprintf_s(line, sizeof(line),
            "FP_V137_LEAVE_FOCUS gameFocus=%p priority=%d mode=\"%s\" autoTheater=%d theater=%d\n",
            reinterpret_cast<void*>(focusMode), priority, cls,
            g_autoTheaterCutscenes ? 1 : 0, g_theaterMode ? 1 : 0);
        FP_Log(line);
    }
    else
    {
        g_avV29ProbeEnabled = true;
        if (g_autoTheaterCutscenes)
            g_theaterMode = g_fpV131SavedTheaterMode;

        InterlockedExchange(&g_avV210CaptureRemaining, 180);

        char line[320]{};
        sprintf_s(line, sizeof(line),
            "FP_V137_GAIN_FOCUS gameFocus=%p priority=%d mode=\"%s\" theater=%d\n",
            reinterpret_cast<void*>(focusMode), priority, cls,
            g_theaterMode ? 1 : 0);
        FP_Log(line);
    }
}

static void FP_V131UpdateWorkshopFocusBridge(uintptr_t camera)
{
    if(!g_fpV1Enabled || !FP_V111PlausiblePtr(camera))
        return;

    int priority=-1;
    char cls[128]="";
    const bool takeover=
        FP_V131HigherPriorityCameraOwnsFocus(camera,&priority,cls,sizeof(cls));

    if(takeover)
    {
        ++g_fpV131TakeoverFrames;
        g_fpV131ReturnFrames=0;
    }
    else
    {
        g_fpV131TakeoverFrames=0;
        ++g_fpV131ReturnFrames;
    }

    // Small debounce avoids a single transitional camera-list frame flashing
    // Theater Mode or the avatar.
    if(!g_fpV131AutoSuspended && g_fpV131TakeoverFrames>=2)
    {
        g_fpV131AutoSuspended=true;
        g_fpV131SavedTheaterMode=g_theaterMode;
        if(g_autoTheaterCutscenes)
            g_theaterMode=true;

        char line[320]{};
        sprintf_s(line,sizeof(line),
            "FP_V131_LEAVE_FOCUS priority=%d mode=\"%s\" autoTheater=%d theater=%d -- avatar/camera suspended\n",
            priority,cls,g_autoTheaterCutscenes?1:0,g_theaterMode?1:0);
        FP_Log(line);
    }
    else if(g_fpV131AutoSuspended && g_fpV131ReturnFrames>=4)
    {
        g_fpV131AutoSuspended=false;
        if(g_autoTheaterCutscenes)
            g_theaterMode=g_fpV131SavedTheaterMode;

        FP_Log("FP_V131_GAIN_FOCUS -- first-person camera/avatar restored\n");
    }

    const ULONGLONG now=GetTickCount64();
    if((takeover || g_fpV131AutoSuspended) &&
       now-g_fpV131LastFocusLog>=1000)
    {
        g_fpV131LastFocusLog=now;
        char line[256]{};
        sprintf_s(line,sizeof(line),
            "FP_V131_FOCUS_STATE takeover=%d suspended=%d priority=%d mode=\"%s\"\n",
            takeover?1:0,g_fpV131AutoSuspended?1:0,priority,cls);
        FP_Log(line);
    }
}

static int32_t FP_V124RawPropertyOffset(uintptr_t object, const char* wanted)
{
    if (!FP_V111PlausiblePtr(object) || !wanted)
        return -1;

    int32_t wantedIndex = -1;
    if (!FP_V123FindRawNameIndex(wanted, wantedIndex) || wantedIndex < 0)
        return -1;

    uintptr_t cls = 0, prop = 0;
    if (!FP_V18ReadPtr(object + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, wantedIndex, prop) ||
        !FP_V111PlausiblePtr(prop))
        return -1;

    int32_t off = -1;
    if (!FP_V18ReadI32(prop + 0x8C, off) || off < 0 || off >= 0x10000)
        return -1;
    return off;
}

static int32_t g_fpV124ModeNameIndices[4] = { -2, -2, -2, -2 };
static int32_t g_fpV124IsRemovedNameIndex = -2;

static void FP_V124EnsureModeNameIndices()
{
    static const char* kNames[4] = {
        "Hat_CamMode_InWaterAngle",
        "Hat_CamMode_TurnCameraToFaceMovement",
        "Hat_CamMode_ForcedRotation",
        "Hat_CamMode_ForcedRotation_TwoPoints"
    };
    for (int i = 0; i < 4; ++i)
    {
        if (g_fpV124ModeNameIndices[i] != -2) continue;
        g_fpV124ModeNameIndices[i] = -1;
        FP_V123FindRawNameIndex(kNames[i], g_fpV124ModeNameIndices[i]);
    }
    if (g_fpV124IsRemovedNameIndex == -2)
    {
        g_fpV124IsRemovedNameIndex = -1;
        FP_V123FindRawNameIndex("IsRemoved", g_fpV124IsRemovedNameIndex);
    }
}

static bool FP_V124IsWorkshopMode(uintptr_t mode, bool& restoreOnDisable)
{
    restoreOnDisable = false;
    if (!FP_V111PlausiblePtr(mode)) return false;

    FP_V124EnsureModeNameIndices();

    uintptr_t cls = 0;
    int32_t actual = -1;
    if (!FP_V18ReadPtr(mode + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123ObjectRawNameIndex(cls, actual))
        return false;

    if (actual == g_fpV124ModeNameIndices[0])
    { restoreOnDisable = true; return true; }
    if (actual == g_fpV124ModeNameIndices[1])
    { restoreOnDisable = true; return true; }
    if (actual == g_fpV124ModeNameIndices[2]) return true;
    if (actual == g_fpV124ModeNameIndices[3]) return true;
    return false;
}

static int32_t FP_V124FindIsRemovedOffsetCached(uintptr_t mode)
{
    FP_V124EnsureModeNameIndices();
    if (g_fpV124IsRemovedNameIndex < 0) return -1;

    // All Hat_CamMode instances share the inherited IsRemoved property layout.
    static int32_t cachedOffset = -2;
    if (cachedOffset != -2) return cachedOffset;

    cachedOffset = -1;
    uintptr_t cls = 0, prop = 0;
    if (FP_V18ReadPtr(mode + 0x50, cls) &&
        FP_V111PlausiblePtr(cls) &&
        FP_V123FindFieldByRawNameIndex(cls, g_fpV124IsRemovedNameIndex, prop) &&
        FP_V111PlausiblePtr(prop))
    {
        int32_t off = -1;
        if (FP_V18ReadI32(prop + 0x8C, off) && off >= 0 && off < 0x10000)
            cachedOffset = off;
    }
    return cachedOffset;
}

struct FP_V124SuppressedMode
{
    uintptr_t mode;
    int32_t isRemovedOffset;
    bool restoreOnDisable;
};

static FP_V124SuppressedMode g_fpV124Suppressed[16] = {};
static int g_fpV124SuppressedCount = 0;

static bool FP_V124WriteIsRemoved(uintptr_t mode, int32_t offset, bool value)
{
    if (!FP_V111PlausiblePtr(mode) || offset < 0) return false;
    unsigned char b = value ? 1 : 0;
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(mode + static_cast<uintptr_t>(offset)),
        &b, sizeof(b), &written) != FALSE && written == sizeof(b);
}

static bool FP_V125RawNameFromIndex(int32_t index, char* out, size_t outSize)
{
    if (!out || outSize == 0 || index < 0) return false;
    out[0] = 0;

    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uintptr_t data = 0;
    int32_t count = 0;
    if (!exe ||
        !FP_V18ReadPtr(exe + 0x125CCE8, data) ||
        !FP_V18ReadI32(exe + 0x125CCF0, count) ||
        !FP_V111PlausiblePtr(data) || index >= count)
        return false;

    uintptr_t entry = 0;
    if (!FP_V18ReadPtr(data + static_cast<uintptr_t>(index) * 8, entry) ||
        !FP_V111PlausiblePtr(entry))
        return false;

    int32_t encoded = 0;
    if (!FP_V18ReadI32(entry + 0x08, encoded) || (encoded & 1))
        return false;

    const size_t n = (outSize - 1 < 127) ? outSize - 1 : 127;
    if (!FP_ReadMemory(reinterpret_cast<const void*>(entry + 0x14), out, n))
        return false;
    out[n] = 0;
    return out[0] != 0;
}

static void FP_V125DumpCameraModes(uintptr_t camera, const FP_V116TArrayPtrs& arr)
{
    static uintptr_t lastCamera = 0;
    static DWORD lastDumpMs = 0;
    const DWORD now = GetTickCount();

    // Dump once immediately for a new camera, then at most twice per second.
    if (camera == lastCamera && (now - lastDumpMs) < 500)
        return;
    lastCamera = camera;
    lastDumpMs = now;

    char head[192] = {};
    sprintf_s(head, sizeof(head),
        "FP_V125_MODE_DUMP camera=%p count=%d\n",
        reinterpret_cast<void*>(camera), arr.Count);
    FP_Log(head);

    for (int n = 0; n < arr.Count; ++n)
    {
        uintptr_t mode = 0, cls = 0;
        int32_t classNameIndex = -1;
        char className[128] = "<unknown>";
        int removed = -1;
        int32_t removedOffset = -1;

        if (FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(n) * 8, mode) &&
            FP_V111PlausiblePtr(mode) &&
            FP_V18ReadPtr(mode + 0x50, cls) &&
            FP_V111PlausiblePtr(cls) &&
            FP_V123ObjectRawNameIndex(cls, classNameIndex))
        {
            FP_V125RawNameFromIndex(classNameIndex, className, sizeof(className));

            removedOffset = FP_V124FindIsRemovedOffsetCached(mode);
            if (removedOffset >= 0)
            {
                unsigned char b = 0;
                if (FP_ReadMemory(reinterpret_cast<const void*>(
                        mode + static_cast<uintptr_t>(removedOffset)), &b, 1))
                    removed = b ? 1 : 0;
            }
        }

        char line[384] = {};
        sprintf_s(line, sizeof(line),
            "FP_V125_MODE [%02d] mode=%p class=%p nameIndex=%d name=\"%s\" IsRemoved=%d off=0x%X\n",
            n, reinterpret_cast<void*>(mode), reinterpret_cast<void*>(cls),
            classNameIndex, className, removed,
            static_cast<unsigned int>(removedOffset));
        FP_Log(line);
    }
}

static bool FP_V127CallUFunctionParams(
    uintptr_t object, uintptr_t function, void* params);

static bool g_fpV129ApplyCameraModesDumped = false;

static bool FP_V129GetExeTextRange(uintptr_t& beginOut, uintptr_t& endOut)
{
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!FP_V111PlausiblePtr(base))
        return false;

    IMAGE_DOS_HEADER dos{};
    if (!FP_ReadMemory(reinterpret_cast<const void*>(base), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    IMAGE_NT_HEADERS64 nt{};
    if (!FP_ReadMemory(reinterpret_cast<const void*>(base + dos.e_lfanew),
                       &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return false;

    const uintptr_t sectionTable =
        base + dos.e_lfanew + sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;

    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        IMAGE_SECTION_HEADER sh{};
        if (!FP_ReadMemory(
                reinterpret_cast<const void*>(
                    sectionTable + static_cast<uintptr_t>(i) * sizeof(sh)),
                &sh, sizeof(sh)))
            continue;

        if (memcmp(sh.Name, ".text", 5) == 0)
        {
            beginOut = base + sh.VirtualAddress;
            endOut = beginOut + sh.Misc.VirtualSize;
            return endOut > beginOut;
        }
    }
    return false;
}

static void FP_V129DiscoverApplyCameraModes(uintptr_t camera)
{
    if (g_fpV129ApplyCameraModesDumped || !FP_V111PlausiblePtr(camera))
        return;

    int32_t nameIndex = -1;
    uintptr_t cls = 0, fn = 0;
    if (!FP_V123FindRawNameIndex("ApplyCameraModes", nameIndex) ||
        nameIndex < 0 ||
        !FP_V18ReadPtr(camera + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, nameIndex, fn) ||
        !FP_V111PlausiblePtr(fn))
        return;

    uintptr_t textBegin = 0, textEnd = 0;
    if (!FP_V129GetExeTextRange(textBegin, textEnd))
        return;

    const uintptr_t exeBase =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    char head[320] = {};
    sprintf_s(head, sizeof(head),
        "FP_V129_APPLY_CAMERA_MODES UFunction=%p nameIndex=%d class=%p text=[%p,%p)\n",
        reinterpret_cast<void*>(fn), nameIndex,
        reinterpret_cast<void*>(cls),
        reinterpret_cast<void*>(textBegin),
        reinterpret_cast<void*>(textEnd));
    FP_Log(head);

    // UFunction layouts differ between UE3 builds. Rather than assuming the
    // native Func member offset, report every aligned executable pointer in the
    // first 0x120 bytes. In practice this isolates the execApplyCameraModes
    // thunk and gives us an exact ASLR-safe offset for the real stage hook.
    int candidates = 0;
    for (uintptr_t off = 0; off <= 0x118; off += sizeof(uintptr_t))
    {
        uintptr_t p = 0;
        if (!FP_V18ReadPtr(fn + off, p))
            continue;
        if (p < textBegin || p >= textEnd)
            continue;

        char line[256] = {};
        sprintf_s(line, sizeof(line),
            "FP_V129_NATIVE_PTR ufuncOff=0x%llX ptr=%p rva=0x%llX\n",
            static_cast<unsigned long long>(off),
            reinterpret_cast<void*>(p),
            static_cast<unsigned long long>(p - exeBase));
        FP_Log(line);
        ++candidates;
    }

    char tail[192] = {};
    sprintf_s(tail, sizeof(tail),
        "FP_V129_NATIVE_PTR_DONE candidates=%d\n", candidates);
    FP_Log(tail);
    g_fpV129ApplyCameraModesDumped = true;
}

// Exact HatinTimeGame.exe disassembly:
//   exec thunk RVA 0xAD4A70 -> call native RVA 0xA89B70
using FP_V130ApplyCameraModesFn = void(__fastcall*)(
    void*, void*, float, FVectorUE3*, FRotatorUE3*, float*, void*);

static FP_V130ApplyCameraModesFn g_fpV130OriginalApplyCameraModes = nullptr;
static bool g_fpV130HookInstalled = false;
static ULONGLONG g_fpV130LastLog = 0;

static void __fastcall FP_V130HookedApplyCameraModes(
    void* camera, void* pawn, float deltaTime,
    FVectorUE3* camEnd, FRotatorUE3* camRotation,
    float* fov, void* dynamicCameraInfo)
{

    g_fpV130OriginalApplyCameraModes(
        camera, pawn, deltaTime, camEnd, camRotation, fov, dynamicCameraInfo);

    if (!g_fpV1Enabled ||
        !pawn || !camEnd || !camRotation)
        return;


    FVectorUE3 pawnLocation{};
    if (!FP_TryReadVector(
            reinterpret_cast<const unsigned char*>(pawn) + 0x80,
            pawnLocation))
        return;

    camEnd->X = pawnLocation.X;
    camEnd->Y = pawnLocation.Y;
    camEnd->Z = pawnLocation.Z + 28.0f;

    // continues running after ApplyCameraModes; if AHiT later replaces this
    // value, that is the already-working camera yield we want to follow.

    // Deliberately leave camRotation alone: the workshop mode does too.
    // Anything Hat_PlayerCamera does after ApplyCameraModes can now naturally
    // take camera ownership again.
    const ULONGLONG now = GetTickCount64();
    if (now - g_fpV130LastLog >= 500)
    {
        g_fpV130LastLog = now;
        char line[320] = {};
        sprintf_s(line, sizeof(line),
            "FP_V130_STAGE camera=%p pawn=%p dt=%.4f camEnd=(%.3f %.3f %.3f) camRot=(%d %d %d)\n",
            camera, pawn, deltaTime,
            camEnd->X, camEnd->Y, camEnd->Z,
            camRotation->Pitch, camRotation->Yaw, camRotation->Roll);
        FP_Log(line);
    }
}

static bool FP_V130InstallApplyCameraModesHook()
{
    if (g_fpV130HookInstalled)
        return true;

    const uintptr_t exe =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!exe)
        return false;

    constexpr uintptr_t kNativeRva = 0xA89B70;
    void* target = reinterpret_cast<void*>(exe + kNativeRva);

    MH_STATUS st = MH_CreateHook(
        target, &FP_V130HookedApplyCameraModes,
        reinterpret_cast<void**>(&g_fpV130OriginalApplyCameraModes));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
    {
        char line[192] = {};
        sprintf_s(line, sizeof(line),
            "FP_V130_HOOK_CREATE_FAIL target=%p status=%d\n",
            target, static_cast<int>(st));
        FP_Log(line);
        return false;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
    {
        char line[192] = {};
        sprintf_s(line, sizeof(line),
            "FP_V130_HOOK_ENABLE_FAIL target=%p status=%d\n",
            target, static_cast<int>(st));
        FP_Log(line);
        return false;
    }

    g_fpV130HookInstalled = true;
    char line[224] = {};
    sprintf_s(line, sizeof(line),
        "FP_V130_HOOK_INSTALLED target=%p rva=0x%llX original=%p\n",
        target, static_cast<unsigned long long>(kNativeRva),
        reinterpret_cast<void*>(g_fpV130OriginalApplyCameraModes));
    FP_Log(line);
    return true;
}

static uintptr_t g_fpV127RemoveCameraModeClassFn = 0;
static bool g_fpV127RemoveCameraModeClassResolved = false;
static bool g_fpV127WorkshopRemoveApplied = false;

struct FP_V127RemoveCameraModeClassParams
{
    uintptr_t CameraModeClass;
    unsigned char AllowSubClass;
    unsigned char Pad[7];
};

static bool FP_V127ResolveRemoveCameraModeClass(uintptr_t camera)
{
    if (g_fpV127RemoveCameraModeClassResolved)
        return FP_V111PlausiblePtr(g_fpV127RemoveCameraModeClassFn);

    int32_t nameIndex = -1;
    uintptr_t cls = 0, fn = 0;
    if (!FP_V123FindRawNameIndex("RemoveCameraModeClass", nameIndex) ||
        nameIndex < 0 ||
        !FP_V18ReadPtr(camera + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, nameIndex, fn) ||
        !FP_V111PlausiblePtr(fn))
        return false;

    g_fpV127RemoveCameraModeClassFn = fn;
    g_fpV127RemoveCameraModeClassResolved = true;

    char line[256] = {};
    sprintf_s(line, sizeof(line),
        "FP_V127_UFUNCTION RemoveCameraModeClass=%p nameIndex=%d camera=%p\n",
        reinterpret_cast<void*>(fn), nameIndex, reinterpret_cast<void*>(camera));
    FP_Log(line);
    return true;
}

static void FP_V116RemoveWorkshopCameraModes(void* playerController)
{
    const uintptr_t pc = reinterpret_cast<uintptr_t>(playerController);
    if (!FP_V111PlausiblePtr(pc))
        return;

    if (g_fpV116PlayerCameraOffset == -2)
    {
        g_fpV116PlayerCameraOffset = FP_V124RawPropertyOffset(pc, "PlayerCamera");
        char line[192] = {};
        sprintf_s(line, sizeof(line), "FP_V127_PLAYER_CAMERA offset=0x%X\n",
            static_cast<unsigned int>(g_fpV116PlayerCameraOffset));
        FP_Log(line);
    }
    if (g_fpV116PlayerCameraOffset < 0)
        return;

    uintptr_t camera = 0;
    if (!FP_V18ReadPtr(pc + static_cast<uintptr_t>(g_fpV116PlayerCameraOffset), camera) ||
        !FP_V111PlausiblePtr(camera))
        return;

    FP_V129DiscoverApplyCameraModes(camera);

    if (g_fpV116Camera != camera)
    {
        g_fpV116Camera = camera;
        g_fpV116CameraModesOffset = -2;
        g_fpV124SuppressedCount = 0;
        g_fpV127WorkshopRemoveApplied = false;
        g_fpV127RemoveCameraModeClassResolved = false;
        g_fpV127RemoveCameraModeClassFn = 0;
    }

    if (g_fpV116CameraModesOffset == -2)
    {
        g_fpV116CameraModesOffset = FP_V124RawPropertyOffset(camera, "CameraModes");
        char line[192] = {};
        sprintf_s(line, sizeof(line),
            "FP_V127_CAMERA_MODES offset=0x%X camera=%p\n",
            static_cast<unsigned int>(g_fpV116CameraModesOffset),
            reinterpret_cast<void*>(camera));
        FP_Log(line);
    }
    if (g_fpV116CameraModesOffset < 0 || g_fpV127WorkshopRemoveApplied)
        return;

    // User-facing compatibility toggle. When OFF, retain the game's forced/
    // locked gameplay camera modes; focus/cutscene observation still works.
    if (!g_overrideLockedGameplayCameras)
        return;

    FP_V116TArrayPtrs arr{};
    const uintptr_t arrAddress =
        camera + static_cast<uintptr_t>(g_fpV116CameraModesOffset);
    if (!FP_ReadMemory(reinterpret_cast<const void*>(arrAddress), &arr, sizeof(arr)) ||
        !FP_V111PlausiblePtr(arr.Data) || arr.Count < 0 || arr.Count > 256 ||
        arr.Max < arr.Count || arr.Max > 512)
        return;

    if (!FP_V127ResolveRemoveCameraModeClass(camera))
        return;

    // Exact workshop route:
    //   camera.RemoveCameraModeClass(class'Hat_CamMode_...')
    // we obtain the UClass* from each matching live camera-mode instance and
    // pass that class object through ProcessEvent. AllowSubClass is omitted/
    // false exactly like the workshop calls.
    int calls = 0;
    for (int i = 0; i < arr.Count; ++i)
    {
        uintptr_t mode = 0, modeClass = 0;
        if (!FP_V18ReadPtr(arr.Data + static_cast<uintptr_t>(i) * 8, mode) ||
            !FP_V111PlausiblePtr(mode))
            continue;

        bool restoreOnDisable = false;
        if (!FP_V124IsWorkshopMode(mode, restoreOnDisable))
            continue;

        if (!FP_V18ReadPtr(mode + 0x50, modeClass) ||
            !FP_V111PlausiblePtr(modeClass))
            continue;

        FP_V127RemoveCameraModeClassParams params{};
        params.CameraModeClass = modeClass;
        params.AllowSubClass = 0;

        if (FP_V127CallUFunctionParams(
                camera, g_fpV127RemoveCameraModeClassFn, &params))
        {
            ++calls;

            // Track the two classes the workshop adds back when disabled.
            if (restoreOnDisable && g_fpV124SuppressedCount < 16)
            {
                const int32_t off = FP_V124FindIsRemovedOffsetCached(mode);
                g_fpV124Suppressed[g_fpV124SuppressedCount++] =
                    { mode, off, true };
            }
        }
    }

    // if matching live instances were found, the real UnrealScript function
    // has now performed RemoveCameraMode()/IsRemoved itself.
    if (calls > 0)
    {
        g_fpV127WorkshopRemoveApplied = true;
        char line[256] = {};
        sprintf_s(line, sizeof(line),
            "FP_V127_REMOVE_CAMERA_MODE_CLASS calls=%d count=%d fn=%p\n",
            calls, arr.Count,
            reinterpret_cast<void*>(g_fpV127RemoveCameraModeClassFn));
        FP_Log(line);
    }
}

static void FP_V116RestoreWorkshopCameraModes()
{
    int restored = 0;
    for (int i = 0; i < g_fpV124SuppressedCount; ++i)
    {
        const FP_V124SuppressedMode& m = g_fpV124Suppressed[i];
        if (m.restoreOnDisable &&
            FP_V124WriteIsRemoved(m.mode, m.isRemovedOffset, false))
            ++restored;
    }
    if (g_fpV124SuppressedCount > 0)
    {
        char line[192] = {};
        sprintf_s(line, sizeof(line),
            "FP_V124_CAMERA_MODES_RESTORE restored=%d tracked=%d\n",
            restored, g_fpV124SuppressedCount);
        FP_Log(line);
    }
    g_fpV124SuppressedCount = 0;
    g_fpV127WorkshopRemoveApplied = false;
}

static bool FP_V119RefreshExactGNamesIfNeeded(uintptr_t object)
{
    char probe[128] = {};
    uintptr_t cls = 0;
    if (FP_V110ObjectClass(object, cls) &&
        FP_V110ObjectName(cls, probe, sizeof(probe)) && probe[0])
        return true;

    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!exe)
        return false;

    uintptr_t data = 0;
    int32_t count = 0, max = 0;
    if (!FP_V18ReadPtr(exe + 0x125CCE8, data) ||
        !FP_V18ReadI32(exe + 0x125CCF0, count) ||
        !FP_V18ReadI32(exe + 0x125CCF4, max) ||
        !FP_V111PlausiblePtr(data) || count <= 0 || max < count)
        return false;

    // Force the existing exact-name discovery path to consume the current
    // globals again. This matters after workshop package/object churn.
    g_fpV18GNamesData = 0;
    g_fpV18GNamesCount = 0;
    g_fpV18NamesAttempted = false;

    char line[320] = {};
    sprintf_s(line, sizeof(line),
        "FP_V119_GNAMES_REFRESH data=%p count=%d max=%d\n",
        reinterpret_cast<void*>(data), count, max);
    FP_Log(line);

    // Existing lookup lazily repopulates GNames.
    probe[0] = 0;
    cls = 0;
    const bool ok = FP_V110ObjectClass(object, cls) &&
                    FP_V110ObjectName(cls, probe, sizeof(probe)) && probe[0];
    sprintf_s(line, sizeof(line),
        "FP_V119_GNAMES_RESULT ok=%d class=%p name=\"%s\"\n",
        ok ? 1 : 0, reinterpret_cast<void*>(cls), probe);
    FP_Log(line);
    return ok;
}

typedef void (__fastcall *FP_V120ProcessEventFn)(void*, void*, void*, void*);
static uintptr_t g_fpV120FaceCameraFunction = 0;
static bool g_fpV120FaceCameraResolved = false;
static bool g_fpV120BridgeLogged = false;

static bool FP_V120CallUFunctionNoParams(uintptr_t object, uintptr_t function)
{
    uintptr_t vt = 0, pe = 0;
    if (!FP_V111PlausiblePtr(object) || !FP_V111PlausiblePtr(function) ||
        !FP_V18ReadPtr(object, vt) || !FP_V111PlausiblePtr(vt) ||
        !FP_V18ReadPtr(vt + 0x210, pe) || !FP_V111PlausiblePtr(pe))
        return false;

    // HatinTimeGame.exe, PlayerController::GetPlayerViewPoint RVA 0x402EC0:
    // after resolving its UFunction, the executable dispatches it through
    // call qword ptr [UObject.vtable + 0x210], with UObject/UFunction/params/
    // result in RCX/RDX/R8/R9. This identifies ProcessEvent as virtual slot 66.
    reinterpret_cast<FP_V120ProcessEventFn>(pe)(
        reinterpret_cast<void*>(object),
        reinterpret_cast<void*>(function), nullptr, nullptr);
    return true;
}

static bool FP_V127CallUFunctionParams(
    uintptr_t object, uintptr_t function, void* params)
{
    uintptr_t vt = 0, pe = 0;
    if (!FP_V111PlausiblePtr(object) || !FP_V111PlausiblePtr(function) ||
        !FP_V18ReadPtr(object, vt) || !FP_V111PlausiblePtr(vt) ||
        !FP_V18ReadPtr(vt + 0x210, pe) || !FP_V111PlausiblePtr(pe))
        return false;

    reinterpret_cast<FP_V120ProcessEventFn>(pe)(
        reinterpret_cast<void*>(object),
        reinterpret_cast<void*>(function),
        params, nullptr);
    return true;
}

static bool FP_V123EntryNameEquals(uintptr_t entry, const char* wanted)
{
    if (!FP_V111PlausiblePtr(entry) || !wanted)
        return false;

    int32_t encoded = 0;
    if (!FP_V18ReadI32(entry + 0x08, encoded))
        return false;

    const bool wide = (encoded & 1) != 0;
    if (wide)
        return false; // FaceCamera is ANSI in this build.

    char name[128] = {};
    if (!FP_ReadMemory(reinterpret_cast<const void*>(entry + 0x14),
                       name, sizeof(name) - 1))
        return false;
    name[sizeof(name) - 1] = 0;
    return strcmp(name, wanted) == 0;
}

static bool FP_V123FindRawNameIndex(const char* wanted, int32_t& indexOut)
{
    indexOut = -1;
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uintptr_t data = 0;
    int32_t count = 0;
    if (!exe ||
        !FP_V18ReadPtr(exe + 0x125CCE8, data) ||
        !FP_V18ReadI32(exe + 0x125CCF0, count) ||
        !FP_V111PlausiblePtr(data) || count <= 0 || count > 1000000)
        return false;

    for (int32_t i = 0; i < count; ++i)
    {
        uintptr_t entry = 0;
        if (!FP_V18ReadPtr(data + static_cast<uintptr_t>(i) * 8, entry))
            continue;
        if (FP_V123EntryNameEquals(entry, wanted))
        {
            indexOut = i;
            return true;
        }
    }
    return false;
}

static bool FP_V123ObjectRawNameIndex(uintptr_t object, int32_t& indexOut)
{
    indexOut = -1;
    // UObject::Name is FName at +0x48. First DWORD is ComparisonIndex.
    return FP_V18ReadI32(object + 0x48, indexOut) &&
           indexOut >= 0 && indexOut < 1000000;
}

static bool FP_V123FindFieldByRawNameIndex(
    uintptr_t startClass, int32_t wantedIndex, uintptr_t& fieldOut)
{
    fieldOut = 0;
    uintptr_t cls = startClass;

    for (int depth = 0; FP_V111PlausiblePtr(cls) && depth < 32; ++depth)
    {
        uintptr_t children = 0, super = 0;
        FP_V18ReadPtr(cls + 0x80, children);
        FP_V18ReadPtr(cls + 0x78, super);

        if (FP_V111PlausiblePtr(children))
        {
            // do not guess one UField::Next layout for the whole chain.
            // Walk both layouts observed in this exact game (+0x58/+0x60).
            uintptr_t queue[8192] = {};
            int qRead = 0, qWrite = 0;
            queue[qWrite++] = children;

            while (qRead < qWrite && qRead < 8192)
            {
                const uintptr_t field = queue[qRead++];
                if (!FP_V111PlausiblePtr(field))
                    continue;

                bool duplicate = false;
                for (int i = 0; i < qRead - 1; ++i)
                    if (queue[i] == field) { duplicate = true; break; }
                if (duplicate)
                    continue;

                int32_t nameIndex = -1;
                if (FP_V123ObjectRawNameIndex(field, nameIndex) &&
                    nameIndex == wantedIndex)
                {
                    fieldOut = field;
                    return true;
                }

                const unsigned int offsets[2] = { 0x58, 0x60 };
                for (unsigned int off : offsets)
                {
                    uintptr_t next = 0;
                    if (FP_V18ReadPtr(field + off, next) &&
                        FP_V111PlausiblePtr(next) && next != field &&
                        qWrite < 8192)
                        queue[qWrite++] = next;
                }
            }
        }

        if (!FP_V111PlausiblePtr(super) || super == cls)
            break;
        cls = super;
    }
    return false;
}

static void FP_V123PrimeFaceCameraRaw(uintptr_t pawn)
{
    if (g_fpV120FaceCameraResolved || !FP_V111PlausiblePtr(pawn))
        return;

    static int32_t faceCameraNameIndex = -2;
    static bool loggedName = false;

    if (faceCameraNameIndex == -2)
    {
        faceCameraNameIndex = -1;
        FP_V123FindRawNameIndex("FaceCamera", faceCameraNameIndex);
        if (!loggedName)
        {
            loggedName = true;
            char line[256] = {};
            sprintf_s(line, sizeof(line),
                "FP_V123_RAW_NAME FaceCamera index=%d\n", faceCameraNameIndex);
            FP_Log(line);
        }
    }

    if (faceCameraNameIndex < 0)
        return;

    uintptr_t cls = 0, fn = 0;
    if (!FP_V18ReadPtr(pawn + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, faceCameraNameIndex, fn) ||
        !FP_V111PlausiblePtr(fn))
        return;

    g_fpV120FaceCameraFunction = fn;
    g_fpV120FaceCameraResolved = true;

    char line[320] = {};
    sprintf_s(line, sizeof(line),
        "FP_V123_PRIMED_RAW FaceCamera=%p nameIndex=%d pawn=%p\n",
        reinterpret_cast<void*>(fn), faceCameraNameIndex,
        reinterpret_cast<void*>(pawn));
    FP_Log(line);
}

static void FP_V121PrimeFaceCameraWhileNamesHealthy(uintptr_t pawn)
{
    if (g_fpV120FaceCameraResolved || !FP_V111PlausiblePtr(pawn))
        return;

    char className[128] = {};
    uintptr_t cls = 0;
    if (!FP_V110ObjectClass(pawn, cls) ||
        !FP_V110ObjectName(cls, className, sizeof(className)) ||
        !className[0])
        return;

    uintptr_t fn = 0;
    unsigned int nextOff = 0;
    if (!FP_V111FindField(cls, "FaceCamera", fn, nextOff) ||
        !FP_V111PlausiblePtr(fn))
        return;

    g_fpV120FaceCameraFunction = fn;
    g_fpV120FaceCameraResolved = true;

    char line[384] = {};
    sprintf_s(line, sizeof(line),
        "FP_V121_PRIMED FaceCamera=%p pawn=%p class=\"%s\" nextOff=0x%X\n",
        reinterpret_cast<void*>(fn), reinterpret_cast<void*>(pawn),
        className, nextOff);
    FP_Log(line);
}

static bool FP_V120FaceCamera(uintptr_t pawn)
{
    if (!g_fpV120FaceCameraResolved)
    {
        static bool s_lateResolveAttempted = false;
        if (s_lateResolveAttempted)
            return false;
        s_lateResolveAttempted = true;

        if (!FP_V119RefreshExactGNamesIfNeeded(pawn))
            return false;
        uintptr_t cls = 0, fn = 0;
        unsigned int nextOff = 0;
        if (!FP_V110ObjectClass(pawn, cls) ||
            !FP_V111FindField(cls, "FaceCamera", fn, nextOff) ||
            !FP_V111PlausiblePtr(fn))
            return false;
        g_fpV120FaceCameraFunction = fn;
        g_fpV120FaceCameraResolved = true;
    }

    if (!g_fpV120BridgeLogged)
    {
        g_fpV120BridgeLogged = true;
        uintptr_t vt = 0, pe = 0;
        FP_V18ReadPtr(pawn, vt);
        if (FP_V111PlausiblePtr(vt)) FP_V18ReadPtr(vt + 0x210, pe);
        uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        char line[384] = {};
        sprintf_s(line, sizeof(line),
            "FP_V120_SCRIPT_BRIDGE FaceCamera=%p ProcessEvent=%p rva=0x%llX slot=66\n",
            reinterpret_cast<void*>(g_fpV120FaceCameraFunction),
            reinterpret_cast<void*>(pe),
            static_cast<unsigned long long>((exe && pe >= exe) ? pe - exe : 0));
        FP_Log(line);
    }

    return FP_V120CallUFunctionNoParams(pawn, g_fpV120FaceCameraFunction);
}

static bool g_fpV118ScriptBridgeDumped = false;

static void FP_V118DiscoverScriptBridge(uintptr_t pawn)
{
    if (g_fpV118ScriptBridgeDumped || !FP_V111PlausiblePtr(pawn))
        return;
    if (!FP_V119RefreshExactGNamesIfNeeded(pawn))
        return;
    g_fpV118ScriptBridgeDumped = true;

    uintptr_t pawnClass = 0;
    if (!FP_V110ObjectClass(pawn, pawnClass))
    {
        FP_Log("FP_V118_SCRIPT_FAIL no pawn class.\n");
        return;
    }

    uintptr_t faceCameraFn = 0;
    unsigned int faceNextOff = 0;
    if (!FP_V111FindField(pawnClass, "FaceCamera", faceCameraFn, faceNextOff))
    {
        FP_Log("FP_V118_SCRIPT_FAIL FaceCamera UFunction not found.\n");
        return;
    }

    char fnType[128] = {};
    uintptr_t fnClass = 0;
    FP_V110ObjectClass(faceCameraFn, fnClass);
    FP_V110ObjectName(fnClass, fnType, sizeof(fnType));

    char line[512] = {};
    sprintf_s(line, sizeof(line),
        "FP_V118_UFUNCTION FaceCamera=%p type=\"%s\" nextOff=0x%X\n",
        reinterpret_cast<void*>(faceCameraFn), fnType, faceNextOff);
    FP_Log(line);

    // ProcessEvent is a virtual UObject dispatch function. Do not guess its
    // slot and call an arbitrary game function. Dump the live Hat_Player
    // vtable as module RVAs to identify the exact ProcessEvent entry
    // against this executable and then use it as the UnrealScript bridge.
    uintptr_t vtable = 0;
    if (!FP_V18ReadPtr(pawn, vtable) || !FP_V111PlausiblePtr(vtable))
    {
        FP_Log("FP_V118_SCRIPT_FAIL pawn vtable unreadable.\n");
        return;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
    sprintf_s(line, sizeof(line),
        "FP_V118_VTABLE pawn=%p vtable=%p exeBase=%p\n",
        reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(vtable),
        reinterpret_cast<void*>(exeBase));
    FP_Log(line);

    for (int i = 0; i < 128; ++i)
    {
        uintptr_t fn = 0;
        if (!FP_V18ReadPtr(vtable + static_cast<uintptr_t>(i) * sizeof(uintptr_t), fn))
            break;

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(fn), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT)
            continue;

        const bool inExe = exeBase && fn >= exeBase && fn < exeBase + 0x20000000ull;
        if (inExe)
        {
            sprintf_s(line, sizeof(line),
                "FP_V118_VFUNC slot=%d fn=%p rva=0x%llX protect=0x%lX\n",
                i, reinterpret_cast<void*>(fn),
                static_cast<unsigned long long>(fn - exeBase),
                static_cast<unsigned long>(mbi.Protect));
        }
        else
        {
            sprintf_s(line, sizeof(line),
                "FP_V118_VFUNC slot=%d fn=%p rva=external protect=0x%lX\n",
                i, reinterpret_cast<void*>(fn),
                static_cast<unsigned long>(mbi.Protect));
        }
        FP_Log(line);
    }

    FP_Log("FP_V118_SCRIPT_READY FaceCamera reflected; ProcessEvent slot intentionally awaiting exact identification.\n");
}

// camera behavior; it tells us whether the strafe lean exists in the game's
// finished POV, PlayerController.Rotation, or is introduced after this hook.
static ULONGLONG g_fpV126LastRotTraceTick = 0;
static FRotatorUE3 g_fpV126PrevOriginalRot{};
static FRotatorUE3 g_fpV126PrevControllerRot{};
static bool g_fpV126HavePrevRot = false;

static int FP_V126RotDelta(int a, int b)
{
    int d = (a - b) & 0xFFFF;
    if (d > 32767) d -= 65536;
    return d;
}

#include "../include_order/avatar_system_order.inl"

// check whether the first person anchor survives the rest of AHiT's camera pipeline
// when GetPlayerViewPoint returns.
static void FP_V136UpdateCameraYieldFromFinishedPOV(const FVectorUE3&)
{
    // from the ApplyCameraModes serial around ORIGINAL GetPlayerViewPoint.
}

// absence of ApplyCameraModes.
//
// Hat_PlayerCamera.SetFirstPersonMode() begins its non-forced eligibility test
// with:
//     if (ViewTarget.Target != None && ViewTarget.Target != PCOwner.Pawn) return;
//
// that is an immediate game-state transition, not a timer/heartbeat heuristic.
// During an external/cinematic view target HatVR should yield presentation too.
static int32_t g_fpV143ViewTargetOffset = -2;

static bool FP_V143ExternalViewTarget(
    void* playerController,
    bool& externalOut,
    uintptr_t* targetOut = nullptr,
    uintptr_t* pawnOut = nullptr)
{
    externalOut = false;
    if (targetOut) *targetOut = 0;
    if (pawnOut) *pawnOut = 0;

    const uintptr_t pc = reinterpret_cast<uintptr_t>(playerController);
    if (!FP_V111PlausiblePtr(pc))
        return false;

    uintptr_t pawn = 0;
    if (!FP_V18ReadPtr(pc + 0x2FC, pawn))
        return false;
    if (pawnOut) *pawnOut = pawn;

    // Resolve PlayerCamera exactly the same way as the existing workshop-mode
    // removal path. This remains valid whether HatVR first person is ON or OFF.
    if (g_fpV116PlayerCameraOffset == -2)
        g_fpV116PlayerCameraOffset = FP_V124RawPropertyOffset(pc, "PlayerCamera");
    if (g_fpV116PlayerCameraOffset < 0)
        return false;

    uintptr_t camera = 0;
    if (!FP_V18ReadPtr(
            pc + static_cast<uintptr_t>(g_fpV116PlayerCameraOffset),
            camera) ||
        !FP_V111PlausiblePtr(camera))
        return false;

    if (g_fpV143ViewTargetOffset == -2)
    {
        g_fpV143ViewTargetOffset =
            FP_V124RawPropertyOffset(camera, "ViewTarget");

        char line[224] = {};
        sprintf_s(line, sizeof(line),
            "FP_V143_VIEWTARGET offset=0x%X camera=%p\n",
            static_cast<unsigned int>(g_fpV143ViewTargetOffset),
            reinterpret_cast<void*>(camera));
        FP_Log(line);
    }
    if (g_fpV143ViewTargetOffset < 0)
        return false;

    // UE3 FTViewTarget begins with Actor* Target. We intentionally use the
    // reflected ViewTarget property offset rather than a hard-coded camera
    // member offset.
    uintptr_t target = 0;
    if (!FP_V18ReadPtr(
            camera + static_cast<uintptr_t>(g_fpV143ViewTargetOffset),
            target))
        return false;

    if (targetOut) *targetOut = target;

    // this mirrors Hat_PlayerCamera's own SetFirstPersonMode gate exactly:
    // None is allowed; the possessed pawn is allowed; another Actor owns view.
    externalOut =
        FP_V111PlausiblePtr(target) &&
        FP_V111PlausiblePtr(pawn) &&
        target != pawn;

    return true;
}

// Menu-facing first-person switch.  This deliberately enables/disables both
// halves of HatVR first person together: the camera anchor and the VR avatar/IK.
// toggles, but normal users no longer need to know about the two-stage setup.
// HatVR First Person: disable AHiT's native Near Dither Culling.
//
// AHiT packs Actor3DScale in XYZ and NearDitherCulling in W. Both native
// mesh-material shader paths contain an existing disabled branch which zeros
// the NearDither value with `xorps xmm6,xmm6`. While HatVR First Person is on,
// redirect the decision into that native zero path. Restore vanilla bytes as
// soon as First Person is disabled.
//
// Build 25577316 verification: both sites and instruction sequences are
// unchanged from the pre-hotfix executable. Byte validation intentionally
// makes this fail closed if a later game build changes either site.
static bool FP_SetNearDitherCullingDisabled(bool disabled)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe)
        return false;

    struct Site { uintptr_t rva; };
    static const Site sites[] = {
        { 0x752BE1ULL },
        { 0x753861ULL },
    };

    const unsigned char original[2] = { 0xF6, 0x47 }; // test byte ptr [rdi+58h],20h
    const unsigned char forceOff[2] = { 0xEB, 0x15 }; // jmp AHiT's xorps xmm6,xmm6

    bool allOk = true;
    for (const Site& site : sites)
    {
        auto* address = reinterpret_cast<unsigned char*>(exe) + site.rva;
        const bool isOriginal = memcmp(address, original, sizeof(original)) == 0;
        const bool isForcedOff = memcmp(address, forceOff, sizeof(forceOff)) == 0;

        if ((disabled && isForcedOff) || (!disabled && isOriginal))
            continue;

        // Never patch an unknown executable state/build.
        if (!isOriginal && !isForcedOff)
        {
            char line[192] = {};
            sprintf_s(line, sizeof(line),
                "FP_NEAR_DITHER REFUSED rva=0x%llX bytes=%02X %02X\n",
                static_cast<unsigned long long>(site.rva),
                static_cast<unsigned int>(address[0]),
                static_cast<unsigned int>(address[1]));
            FP_Log(line);
            allOk = false;
            continue;
        }

        const unsigned char* target = disabled ? forceOff : original;
        DWORD oldProtect = 0;
        if (!VirtualProtect(address, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            allOk = false;
            continue;
        }

        memcpy(address, target, sizeof(original));
        FlushInstructionCache(GetCurrentProcess(), address, sizeof(original));

        DWORD ignored = 0;
        VirtualProtect(address, sizeof(original), oldProtect, &ignored);
    }

    FP_Log(disabled
        ? (allOk ? "FP_NEAR_DITHER OFF -- native Near Dither Culling bypass active\n"
                 : "FP_NEAR_DITHER OFF -- one or more native sites were not patched\n")
        : (allOk ? "FP_NEAR_DITHER RESTORED -- vanilla Near Dither Culling active\n"
                 : "FP_NEAR_DITHER RESTORE -- one or more native sites were not restored\n"));
    return allOk;
}

static void SetUnifiedFirstPersonEnabled(bool enabled)
{
    g_firstPersonConfigured = enabled;
    g_fpV1Enabled = enabled;
    FP_SetNearDitherCullingDisabled(enabled);
    g_avV29ProbeEnabled = enabled;

    // Force the avatar system to rebuild its frozen/reference data when the
    // combined mode changes, matching the important reset work done by F5.
    g_avV221BaselineValid = false;
    g_avV241BaselineLeftValid = false;
    g_avV241PublishedLeftValid = false;
    g_avV259BaselineAllValid = false;
    g_avV259PublishedArmValid = false;
    memset(g_avV259PublishedArmMask, 0, sizeof(g_avV259PublishedArmMask));
    g_avV242PublishedHeadChainValid = false;

    if (enabled)
    {
                    g_fpV131AutoSuspended = false;
        InterlockedExchange(&g_avV210CaptureRemaining, 180);
        FP_Log("HATVR FIRST PERSON ON -- camera anchor + VR avatar enabled together\n");
    }
    else
    {
        if (g_fpV136CameraYielded && g_fpV136ForcedTheater)
            g_theaterMode = g_fpV136SavedTheater;
            
        FP_V116RestoreWorkshopCameraModes();
        FP_Log("HATVR FIRST PERSON OFF -- camera anchor + VR avatar disabled together\n");
    }
}

static void FP_OnPlayerViewPoint(
    void* playerController,
    FVectorUE3& location,
    FRotatorUE3& rotation)
{

    // the healthy reflection window; waiting for the toggle was too late.
    uintptr_t pawn = 0;
    FVectorUE3 pawnLocation{};
    const bool pawnResolved =
        FP_V112ResolvePawnAndLocation(playerController, pawn, pawnLocation);

    if (pawnResolved)
    {
        AV_OnPawnDiscovered(pawn);

        // current GNames array directly, gets FaceCamera's raw FName index,
        // then walks both observed UField::Next layouts by index.
        FP_V123PrimeFaceCameraRaw(pawn);
        if (!g_fpV120FaceCameraResolved)
            FP_V121PrimeFaceCameraWhileNamesHealthy(pawn);
    }

    FP_V130InstallApplyCameraModesHook();

    if (!g_fpV1Enabled)
        return;

    FP_V116RemoveWorkshopCameraModes(playerController);

    // this branch has always preserved AHiT's original camera whenever the
    // normal possessed player pawn cannot be resolved.  Do not invent a
    // second cutscene detector: the avatar and optional Theater Mode now
    // follow this exact same condition.
    if (!pawnResolved)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_fpV1LastScanTick >= 1000ULL)
        {
            g_fpV1LastScanTick = now;
            char failLine[320] = {};
            uintptr_t rawPawn = 0;
            FP_TryReadPointer(
                reinterpret_cast<const unsigned char*>(playerController) + 0x2FC,
                rawPawn);
            sprintf_s(failLine, sizeof(failLine),
                "FP_V115_ANCHOR_FAIL pc=%p rawPawn=%p -- preserving original camera.\n",
                playerController, reinterpret_cast<void*>(rawPawn));
            FP_Log(failLine);
        }
        return;
    }

    // FaceCamera should already have been primed before FP was enabled.
    const FVectorUE3 originalLocation = location;
    const FRotatorUE3 originalRotation = rotation;


    FRotatorUE3 controllerRotation{};
    const bool controllerRotationValid =
        FP_V115ReadActorRotation(reinterpret_cast<uintptr_t>(playerController),
                                controllerRotation);

    // behavior unchanged for this test.
    if (controllerRotationValid)
    {
        const ULONGLONG traceNow = GetTickCount64();
        if (traceNow - g_fpV126LastRotTraceTick >= 50ULL)
        {
            g_fpV126LastRotTraceTick = traceNow;

            const int oDP = g_fpV126HavePrevRot ? FP_V126RotDelta(originalRotation.Pitch, g_fpV126PrevOriginalRot.Pitch) : 0;
            const int oDY = g_fpV126HavePrevRot ? FP_V126RotDelta(originalRotation.Yaw,   g_fpV126PrevOriginalRot.Yaw)   : 0;
            const int oDR = g_fpV126HavePrevRot ? FP_V126RotDelta(originalRotation.Roll,  g_fpV126PrevOriginalRot.Roll)  : 0;
            const int cDP = g_fpV126HavePrevRot ? FP_V126RotDelta(controllerRotation.Pitch, g_fpV126PrevControllerRot.Pitch) : 0;
            const int cDY = g_fpV126HavePrevRot ? FP_V126RotDelta(controllerRotation.Yaw,   g_fpV126PrevControllerRot.Yaw)   : 0;
            const int cDR = g_fpV126HavePrevRot ? FP_V126RotDelta(controllerRotation.Roll,  g_fpV126PrevControllerRot.Roll)  : 0;

            char trace[512] = {};
            sprintf_s(trace, sizeof(trace),
                "FP_V126_ROT original=(%d %d %d) d=(%d %d %d) controller=(%d %d %d) d=(%d %d %d) originalMinusController=(%d %d %d)\n",
                originalRotation.Pitch, originalRotation.Yaw, originalRotation.Roll,
                oDP, oDY, oDR,
                controllerRotation.Pitch, controllerRotation.Yaw, controllerRotation.Roll,
                cDP, cDY, cDR,
                FP_V126RotDelta(originalRotation.Pitch, controllerRotation.Pitch),
                FP_V126RotDelta(originalRotation.Yaw, controllerRotation.Yaw),
                FP_V126RotDelta(originalRotation.Roll, controllerRotation.Roll));
            FP_Log(trace);

            g_fpV126PrevOriginalRot = originalRotation;
            g_fpV126PrevControllerRot = controllerRotation;
            g_fpV126HavePrevRot = true;
        }
    }

    bool faceCameraApplied = false;
    if (controllerRotationValid)
    {
        // Mirror Hat_Player.FaceCamera(): Hat Kid follows controller/camera yaw,
        // never the HMD's additional physical head rotation.
        // Invoke the game's real UnrealScript Hat_Player.FaceCamera().
        // Fall back to the old raw-yaw imitation only during transient
        // reflection/package states.
        faceCameraApplied = FP_V120FaceCamera(pawn);
        if (!faceCameraApplied)
            faceCameraApplied = FP_V115FacePawnToController(
                pawn, controllerRotation.Yaw);
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_fpV1LastScanTick >= 1000ULL)
    {
        g_fpV1LastScanTick = now;
        char line[512] = {};
        sprintf_s(line, sizeof(line),
            "FP_V115_ANCHOR pawn=%p pawnLoc=(%.3f %.3f %.3f) originalLoc=(%.3f %.3f %.3f) finalBase=(%.3f %.3f %.3f) originalRot=(%d %d %d) controllerRotValid=%d controllerRot=(%d %d %d) faceCamera=%d finalBaseRot=(%d %d %d)\n",
            reinterpret_cast<void*>(pawn),
            pawnLocation.X, pawnLocation.Y, pawnLocation.Z,
            originalLocation.X, originalLocation.Y, originalLocation.Z,
            location.X, location.Y, location.Z,
            originalRotation.Pitch, originalRotation.Yaw, originalRotation.Roll,
            controllerRotationValid ? 1 : 0,
            controllerRotation.Pitch, controllerRotation.Yaw, controllerRotation.Roll,
            faceCameraApplied ? 1 : 0,
            rotation.Pitch, rotation.Yaw, rotation.Roll);
        FP_Log(line);
    }
}

