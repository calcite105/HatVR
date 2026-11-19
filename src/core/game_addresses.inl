// finds HatinTimeGame.exe addresses at runtime.
//
// hatvr hooks, patches and reads ~60 spots inside the game exe.
// instead of hardcoded rvas (which break every time the game updates), each spot
// gets found again at startup from a byte signature made from a build I know works.
// the bytes that change between builds (relative call/jump targets, rip relative
// displacements) are wildcarded, so normal code/data moving around between game
// builds is handled automatically.
//
// kinds:
//   Function  pattern at the start of a function (searched at .pdata starts, or at
//             padding boundaries for leaf functions that don't have .pdata).
//             optional call checks tell apart functions that look the same.
//   Site      spot inside a function, the pattern is searched inside the
//             function it's in, return sites also check the call right before it.
//   Ref       spot found through an instruction that points at it
//             (target = match + endOff + int32 at match + dispOff), with an
//             optional verify pattern for function targets.
//
// anything that can't be found for sure is left at 0 and the feature
// that needs it gets turned off. if a "core" entry is missing hatvr stays off
// so the game just runs normally in flatscreen instead of crashing.

//   Field     a struct field offset, read from the displacement of a game
//             instruction that uses that field (like `mov rcx,[rdi+0x34]`).
//             the displacement is wildcarded in the pattern, so if a class layout
//             changes in an update it gets picked up automatically. dispOffset is where
//             the displacement is in the instruction, endOffset is its size.

enum class GameAddrKind : int { Function, Site, Ref, Field };

struct GameAddrConstraint
{
    int relOffset;   // offset of a rel32 call/jmp operand inside the pattern
    int target;      // GameAddrId the call has to land on (-1 = not used)
};

struct GameAddrSpec
{
    const char* name;
    GameAddrKind kind;
    uint32_t referenceRva;
    const char* group;
    int hidden;               // helper entry (the function it's in / the function it calls)
    const char* pattern;
    const char* verify;       // ref to a function: the target's own first bytes
    int leaf;
    int container;            // GameAddrId or -1
    int offset;
    int dispOffset;
    int endOffset;
    int expect;               // how many matches to expect (duplicated code)
    int index;                // which match to use (address order) when expect > 1
    int callsTo;              // GameAddrId the call ending at a Site has to target, or -1
    GameAddrConstraint constraints[2];
};

#include "game_signatures.inl"

namespace game_addr_detail
{
    struct Pattern
    {
        unsigned char bytes[256];
        bool fixed[256];
        int length = 0;
        int firstFixed = -1;
    };

    struct Image
    {
        uintptr_t base = 0;
        uint32_t sizeOfImage = 0;
        uint32_t timeDateStamp = 0;
        uint32_t textRva = 0;
        uint32_t textSize = 0;
        const RUNTIME_FUNCTION* pdata = nullptr;
        uint32_t pdataCount = 0;
    };

    enum State : int { Unresolved = 0, Resolving = 1, Resolved = 2, Failed = 3 };

    static Image g_image;
    static uintptr_t g_resolved[GA_Count] = {};
    static int g_state[GA_Count] = {};
    static volatile LONG g_resolveOnce = 0;   // 0 = not run yet, 1 = running, 2 = done
    static bool g_coreReady = false;
    static bool g_referenceBuild = false;

    static bool ParsePattern(const char* text, Pattern& out)
    {
        out = Pattern{};
        if (!text)
            return false;
        const char* p = text;
        while (*p)
        {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            if (out.length >= static_cast<int>(sizeof(out.bytes)))
                return false;
            if (p[0] == '?' && p[1] == '?')
            {
                out.bytes[out.length] = 0;
                out.fixed[out.length] = false;
            }
            else
            {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return -1;
                };
                const int hi = hex(p[0]);
                const int lo = hex(p[1]);
                if (hi < 0 || lo < 0)
                    return false;
                out.bytes[out.length] = static_cast<unsigned char>(hi * 16 + lo);
                out.fixed[out.length] = true;
                if (out.firstFixed < 0)
                    out.firstFixed = out.length;
            }
            ++out.length;
            p += 2;
        }
        return out.length > 0 && out.firstFixed >= 0;
    }

    // test hook: lets an offline test map a game exe as an image and point
    // the resolver at it. always 0 in the actual game.
    static uintptr_t g_imageBaseOverride = 0;

    static bool LoadImage(Image& img)
    {
        const uintptr_t base = g_imageBaseOverride ? g_imageBaseOverride
            : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;

        img.base = base;
        img.sizeOfImage = nt->OptionalHeader.SizeOfImage;
        img.timeDateStamp = nt->FileHeader.TimeDateStamp;

        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            if (memcmp(sec->Name, ".text", 6) == 0)
            {
                img.textRva = sec->VirtualAddress;
                img.textSize = (std::min)(sec->Misc.VirtualSize, sec->SizeOfRawData);
            }
        }

        const IMAGE_DATA_DIRECTORY& exc =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc.VirtualAddress && exc.Size >= sizeof(RUNTIME_FUNCTION))
        {
            img.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(base + exc.VirtualAddress);
            img.pdataCount = exc.Size / sizeof(RUNTIME_FUNCTION);
        }
        return img.textRva != 0 && img.textSize != 0;
    }

    static bool InText(const Image& img, uint32_t rva, uint32_t length)
    {
        return rva >= img.textRva &&
            static_cast<uint64_t>(rva) + length <= static_cast<uint64_t>(img.textRva) + img.textSize;
    }

    static bool MatchAt(const Image& img, uint32_t rva, const Pattern& pat)
    {
        if (!InText(img, rva, static_cast<uint32_t>(pat.length)))
            return false;
        const auto* p = reinterpret_cast<const unsigned char*>(img.base + rva);
        for (int i = 0; i < pat.length; ++i)
            if (pat.fixed[i] && p[i] != pat.bytes[i])
                return false;
        return true;
    }

    static bool IsBoundary(const Image& img, uint32_t rva)
    {
        if (rva <= img.textRva)
            return false;
        const unsigned char before = *reinterpret_cast<const unsigned char*>(img.base + rva - 1);
        return before == 0xCC || before == 0xC3 || before == 0x90;
    }

    // .pdata end of the function starting at `start`, or start + 0x4000 for
    // leaf functions (no unwind entry). .pdata is sorted by BeginAddress.
    static uint32_t FunctionEnd(const Image& img, uint32_t start)
    {
        uint32_t lo = 0, hi = img.pdataCount;
        while (lo < hi)
        {
            const uint32_t mid = (lo + hi) / 2;
            if (img.pdata[mid].BeginAddress < start) lo = mid + 1;
            else hi = mid;
        }
        if (lo < img.pdataCount && img.pdata[lo].BeginAddress == start)
            return img.pdata[lo].EndAddress;
        return start + 0x4000;
    }

    // grab up to maxHits matching rvas in [lo, hi) (in address order).
    static int ScanRange(const Image& img, uint32_t lo, uint32_t hi, const Pattern& pat,
        bool boundary, uint32_t* hits, int maxHits)
    {
        lo = (std::max)(lo, img.textRva);
        hi = (std::min)(hi, img.textRva + img.textSize);
        if (hi <= lo || static_cast<uint32_t>(pat.length) > hi - lo)
            return 0;
        const auto* text = reinterpret_cast<const unsigned char*>(img.base);
        const unsigned char first = pat.bytes[pat.firstFixed];
        const uint32_t last = hi - static_cast<uint32_t>(pat.length);
        int count = 0;
        uint32_t rva = lo;
        while (rva <= last)
        {
            const void* found = memchr(text + rva + pat.firstFixed, first, last - rva + 1);
            if (!found)
                break;
            rva = static_cast<uint32_t>(static_cast<const unsigned char*>(found) - text) - pat.firstFixed;
            if (MatchAt(img, rva, pat) && (!boundary || IsBoundary(img, rva)))
            {
                if (count < maxHits)
                    hits[count] = rva;
                ++count;
                if (count >= maxHits)
                    break;
            }
            ++rva;
        }
        return count;
    }

    static int ScanFunctionStarts(const Image& img, const Pattern& pat, uint32_t* hits, int maxHits)
    {
        int count = 0;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < img.pdataCount; ++i)
        {
            const uint32_t start = img.pdata[i].BeginAddress;
            if (start == previous)
                continue;
            previous = start;
            if (MatchAt(img, start, pat))
            {
                if (count < maxHits)
                    hits[count] = start;
                ++count;
                if (count >= maxHits)
                    break;
            }
        }
        return count;
    }

    static int32_t ReadRel32(const Image& img, uint32_t rva)
    {
        int32_t v = 0;
        memcpy(&v, reinterpret_cast<const void*>(img.base + rva), sizeof(v));
        return v;
    }

    static uint32_t Resolve(int id);

    static uint32_t ResolveFunction(const GameAddrSpec& s, const Pattern& pat)
    {
        const Image& img = g_image;
        const bool constrained = s.constraints[0].target >= 0;
        uint32_t hits[256];
        const int maxHits = constrained ? 256 : 2;
        const int found = s.leaf
            ? ScanRange(img, img.textRva, img.textRva + img.textSize, pat, true, hits, maxHits)
            : ScanFunctionStarts(img, pat, hits, maxHits);
        const int n = (std::min)(found, maxHits);
        if (!constrained)
            return found == 1 ? hits[0] : 0;

        uint32_t match = 0;
        int good = 0;
        for (int h = 0; h < n; ++h)
        {
            bool ok = true;
            for (const GameAddrConstraint& c : s.constraints)
            {
                if (c.target < 0)
                    continue;
                const uint32_t want = Resolve(c.target);
                const uint32_t operand = hits[h] + static_cast<uint32_t>(c.relOffset);
                if (!want || !InText(img, operand, 4) ||
                    operand + 4 + ReadRel32(img, operand) != want)
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
            {
                match = hits[h];
                ++good;
            }
        }
        return good == 1 ? match : 0;
    }

    static uint32_t ResolveInner(int id)
    {
        const GameAddrSpec& s = kGameAddrSpecs[id];
        const Image& img = g_image;
        Pattern pat;
        if (!ParsePattern(s.pattern, pat))
            return 0;

        if (s.kind == GameAddrKind::Function)
            return ResolveFunction(s, pat);

        uint32_t lo = img.textRva, hi = img.textRva + img.textSize;
        if (s.container >= 0)
        {
            lo = Resolve(s.container);
            if (!lo)
                return 0;
            hi = FunctionEnd(img, lo);
        }
        uint32_t hits[8];
        const int maxHits = (std::min)(s.expect + 2, 8);
        const int found = ScanRange(img, lo, hi, pat, false, hits, maxHits);
        if (found != s.expect || s.index < 0 || s.index >= found)
            return 0;
        const uint32_t match = hits[s.index];

        if (s.kind == GameAddrKind::Field)
        {
            // the "address" here is the instruction that has the field offset in it.
            const uint32_t insn = match + static_cast<uint32_t>(s.offset);
            if (!InText(img, insn + static_cast<uint32_t>(s.dispOffset), static_cast<uint32_t>(s.endOffset)))
                return 0;
            return insn;
        }

        if (s.kind == GameAddrKind::Site)
        {
            const uint32_t address = match + static_cast<uint32_t>(s.offset);
            if (s.callsTo >= 0)
            {
                // a return site has to come right after a direct call to the function I expect.
                const uint32_t want = Resolve(s.callsTo);
                const uint32_t call = address - 5;
                if (!InText(img, call, 5) ||
                    *reinterpret_cast<const unsigned char*>(img.base + call) != 0xE8)
                    return 0;
                if (want && address + ReadRel32(img, call + 1) != want)
                    return 0;
            }
            return address;
        }

        // Ref
        const uint32_t operand = match + static_cast<uint32_t>(s.dispOffset);
        if (!InText(img, operand, 4))
            return 0;
        const uint32_t target = match + static_cast<uint32_t>(s.endOffset) + ReadRel32(img, operand);
        if (target == 0 || target >= img.sizeOfImage)
            return 0;
        if (s.verify && s.verify[0])
        {
            Pattern verify;
            if (!ParsePattern(s.verify, verify) || !MatchAt(img, target, verify))
                return 0;
        }
        return target;
    }

    static uint32_t Resolve(int id)
    {
        if (id < 0 || id >= GA_Count)
            return 0;
        if (g_state[id] == Resolved)
            return static_cast<uint32_t>(g_resolved[id] - g_image.base);
        if (g_state[id] != Unresolved)
            return 0;   // failed, or a dependency loop
        g_state[id] = Resolving;
        uint32_t rva = ResolveInner(id);
#ifndef GAME_ADDR_NO_REFERENCE_FALLBACK
        // on the exact reference build, a spot another mod already
        // hooked/patched won't match anymore, but the known rva is still right there.
        if (!rva && g_referenceBuild)
            rva = kGameAddrSpecs[id].referenceRva;
#endif
        if (rva)
        {
            g_resolved[id] = g_image.base + rva;
            g_state[id] = Resolved;
        }
        else
        {
            g_state[id] = Failed;
        }
        return rva;
    }
}

// finds every address once (safe to call again, thread safe). returns true when all
// the "core" entries were found, so hatvr's vr rendering can actually run.
static bool ResolveGameAddresses()
{
    using namespace game_addr_detail;
    LONG prior = InterlockedCompareExchange(&g_resolveOnce, 1, 0);
    if (prior == 1)
    {
        for (int i = 0; i < 2000 && InterlockedCompareExchange(&g_resolveOnce, 1, 1) == 1; ++i)
            Sleep(1);
        return g_coreReady;
    }
    if (prior == 2)
        return g_coreReady;

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    if (!LoadImage(g_image))
    {
        LogCategory("ADDR", "game image could not be parsed; HatVR will stay inactive");
        InterlockedExchange(&g_resolveOnce, 2);
        return false;
    }
    g_referenceBuild = g_image.timeDateStamp == kGameRefTimeDateStamp &&
        g_image.sizeOfImage == kGameRefSizeOfImage;

    int resolved = 0, missing = 0;
    bool coreReady = true;
    for (int id = 0; id < GA_Count; ++id)
    {
        Resolve(id);
        const GameAddrSpec& s = kGameAddrSpecs[id];
        if (s.hidden)
            continue;
        if (g_state[id] == Resolved)
        {
            ++resolved;
            const uint32_t rva = static_cast<uint32_t>(g_resolved[id] - g_image.base);
            if (rva != s.referenceRva)
                LogCategory("ADDR", "%-28s moved 0x%07X -> 0x%07X", s.name, s.referenceRva, rva);
        }
        else
        {
            ++missing;
            // "stereo" hooks only work as a full set, so they're required too.
            if (strcmp(s.group, "core") == 0 || strcmp(s.group, "stereo") == 0)
                coreReady = false;
            LogCategory("ADDR", "%-28s NOT FOUND (feature '%s' disabled)", s.name, s.group);
        }
    }

    QueryPerformanceCounter(&t1);
    const double ms = freq.QuadPart
        ? (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0) / static_cast<double>(freq.QuadPart)
        : 0.0;
    LogCategory("ADDR",
        "game build ts=0x%08X size=0x%X (%s): resolved %d/%d in %.1f ms; core %s",
        g_image.timeDateStamp, g_image.sizeOfImage,
        g_referenceBuild ? "reference build" : "different build",
        resolved, resolved + missing, ms, coreReady ? "READY" : "MISSING - VR disabled");

    g_coreReady = coreReady;
    InterlockedExchange(&g_resolveOnce, 2);
    return coreReady;
}

// absolute address of a found spot, or 0 if it couldn't be found.
// cheap after the first call (used in hot hook paths for return site checks).
static uintptr_t GameAddr(GameAddrId id)
{
    if (game_addr_detail::g_resolveOnce != 2)
        ResolveGameAddresses();
    if (id < 0 || id >= GA_Count || game_addr_detail::g_state[id] != game_addr_detail::Resolved)
        return 0;
    return game_addr_detail::g_resolved[id];
}

template <typename T>
static T GameAddrAs(GameAddrId id)
{
    return reinterpret_cast<T>(GameAddr(id));
}

// rva of a found spot (for comparing return addresses), or 0.
static uintptr_t GameRva(GameAddrId id)
{
    const uintptr_t a = GameAddr(id);
    return a ? a - game_addr_detail::g_image.base : 0;
}

// struct field offset for a Field entry (read from the game's own code), or -1.
static int GameFieldOffset(GameAddrId id)
{
    const uintptr_t insn = GameAddr(id);
    if (!insn || kGameAddrSpecs[id].kind != GameAddrKind::Field)
        return -1;
    const GameAddrSpec& s = kGameAddrSpecs[id];
    const auto* disp = reinterpret_cast<const unsigned char*>(insn + s.dispOffset);
    if (s.endOffset == 1)
        return static_cast<int>(static_cast<int8_t>(disp[0]));
    int32_t value = 0;
    memcpy(&value, disp, sizeof(value));
    return static_cast<int>(value);
}

// true when every non helper entry in a feature group was found.
static bool GameGroupAvailable(const char* group)
{
    ResolveGameAddresses();
    for (int id = 0; id < GA_Count; ++id)
    {
        const GameAddrSpec& s = kGameAddrSpecs[id];
        if (!s.hidden && strcmp(s.group, group) == 0 &&
            game_addr_detail::g_state[id] != game_addr_detail::Resolved)
            return false;
    }
    return true;
}

static uint32_t GameImageSize()
{
    ResolveGameAddresses();
    return game_addr_detail::g_image.sizeOfImage;
}
