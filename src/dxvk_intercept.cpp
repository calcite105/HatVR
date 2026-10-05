#include "dxvk_intercept.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <cstdio>

#include "d3d9_backend.h"
#include "d3d9_hook.h"
#include "logger.h"
#include "dxvk_trace.h"

namespace ahitvr
{
namespace
{
    using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
    using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

    LoadLibraryAFn g_realLoadLibraryA = nullptr;
    GetProcAddressFn g_realGetProcAddress = nullptr;
    Direct3DCreate9Fn g_realDxvkCreate9 = nullptr;
    Direct3DCreate9ExFn g_realDxvkCreate9Ex = nullptr;

    std::atomic<bool> g_bootstrapHooksInstalled{ false };
    std::atomic<bool> g_dxvkHooksInstalled{ false };

    void V5Trace(const char* fmt, ...)
    {
        if (!fmt) return;
        va_list args;
        va_start(args, fmt);
        LogV("DXVK", fmt, args);
        va_end(args);
    }

    bool IsDxvkName(LPCSTR name)
    {
        if (!name || !*name)
            return false;
        const char* base = name;
        for (const char* p = name; *p; ++p)
            if (*p == '\\' || *p == '/')
                base = p + 1;
        return _stricmp(base, "dxvk.dll") == 0;
    }

    bool IsDxvkModule(HMODULE module)
    {
        if (!module)
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(module, path, MAX_PATH))
            return false;
        return IsDxvkName(path);
    }

    bool IsNamedProc(LPCSTR proc, const char* expected)
    {
        if (!proc || reinterpret_cast<ULONG_PTR>(proc) <= 0xFFFF)
            return false;
        return std::strcmp(proc, expected) == 0;
    }

    IDirect3D9* WINAPI HookDxvkCreate9(UINT sdkVersion)
    {
        V5Trace("DXVK-V5: dxvk!Direct3DCreate9 ENTERED sdk=%u", sdkVersion);
        IDirect3D9* d3d = g_realDxvkCreate9 ? g_realDxvkCreate9(sdkVersion) : nullptr;
        V5Trace("DXVK-V5: DXVK returned IDirect3D9=%p", d3d);
        if (d3d)
        {
            const bool ok = ActivateD3D9Backend(d3d);
            V5Trace("DXVK-V5: ActivateD3D9Backend=%s", ok ? "TRUE" : "FALSE");
        }
        return d3d;
    }

    HRESULT WINAPI HookDxvkCreate9Ex(UINT sdkVersion, IDirect3D9Ex** outD3D)
    {
        V5Trace("DXVK-V5: dxvk!Direct3DCreate9Ex ENTERED sdk=%u", sdkVersion);
        if (!g_realDxvkCreate9Ex)
            return E_FAIL;
        HRESULT hr = g_realDxvkCreate9Ex(sdkVersion, outD3D);
        V5Trace("DXVK-V5: Direct3DCreate9Ex hr=0x%08X object=%p",
            static_cast<unsigned>(hr),
            (SUCCEEDED(hr) && outD3D) ? *outD3D : nullptr);
        if (SUCCEEDED(hr) && outD3D && *outD3D)
        {
            const bool ok = ActivateD3D9Backend(static_cast<IDirect3D9*>(*outD3D));
            V5Trace("DXVK-V5: ActivateD3D9Backend=%s", ok ? "TRUE" : "FALSE");
        }
        return hr;
    }

    bool InstallDxvkCreationHooks(HMODULE dxvk)
    {
        if (!dxvk)
            return false;
        if (g_dxvkHooksInstalled.load(std::memory_order_acquire))
            return true;

        // Use the original GetProcAddress trampoline when available to avoid
        // recursively entering our own GetProcAddress hook.
        auto resolver = g_realGetProcAddress ? g_realGetProcAddress : ::GetProcAddress;
        void* create9 = reinterpret_cast<void*>(resolver(dxvk, "Direct3DCreate9"));
        void* create9Ex = reinterpret_cast<void*>(resolver(dxvk, "Direct3DCreate9Ex"));

        V5Trace("DXVK-V5: exports module=%p Create9=%p Create9Ex=%p",
            dxvk, create9, create9Ex);

        bool any = false;
        if (create9)
        {
            MH_STATUS st = MH_CreateHook(create9,
                reinterpret_cast<void*>(&HookDxvkCreate9),
                reinterpret_cast<void**>(&g_realDxvkCreate9));
            V5Trace("DXVK-V5: Create9 MH_CreateHook=%d", static_cast<int>(st));
            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED)
            {
                st = MH_EnableHook(create9);
                V5Trace("DXVK-V5: Create9 MH_EnableHook=%d", static_cast<int>(st));
                any |= (st == MH_OK || st == MH_ERROR_ENABLED);
            }
        }

        if (create9Ex)
        {
            MH_STATUS st = MH_CreateHook(create9Ex,
                reinterpret_cast<void*>(&HookDxvkCreate9Ex),
                reinterpret_cast<void**>(&g_realDxvkCreate9Ex));
            V5Trace("DXVK-V5: Create9Ex MH_CreateHook=%d", static_cast<int>(st));
            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED)
            {
                st = MH_EnableHook(create9Ex);
                V5Trace("DXVK-V5: Create9Ex MH_EnableHook=%d", static_cast<int>(st));
                any |= (st == MH_OK || st == MH_ERROR_ENABLED);
            }
        }

        if (any)
        {
            g_dxvkHooksInstalled.store(true, std::memory_order_release);
            V5Trace("DXVK-V5: DXVK D3D9 creation hooks ACTIVE");
        }
        return any;
    }

    HMODULE WINAPI HookLoadLibraryA(LPCSTR name)
    {
        HMODULE module = g_realLoadLibraryA ? g_realLoadLibraryA(name) : nullptr;
        if (IsDxvkName(name))
        {
            V5Trace("DXVK-V5: LoadLibraryA saw dxvk.dll -> %p", module);
            if (module)
                InstallDxvkCreationHooks(module);
        }
        return module;
    }

    FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR procName)
    {
        FARPROC result = g_realGetProcAddress
            ? g_realGetProcAddress(module, procName)
            : nullptr;

        if (!IsDxvkModule(module))
            return result;

        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(module, modulePath, MAX_PATH);

        if (IsNamedProc(procName, "Direct3DCreate9") ||
            IsNamedProc(procName, "Direct3DCreate9Ex"))
        {
            V5Trace("DXVK-V5: GetProcAddress saw module=%s request=%s result=%p",
                modulePath, procName, result);

            // Ensure the export itself is hooked before returning its address.
            InstallDxvkCreationHooks(module);

            if (IsNamedProc(procName, "Direct3DCreate9") && g_realDxvkCreate9)
            {
                V5Trace("DXVK-V5: DXVK Direct3DCreate9 intercepted");
                return reinterpret_cast<FARPROC>(&HookDxvkCreate9);
            }

            if (IsNamedProc(procName, "Direct3DCreate9Ex") && g_realDxvkCreate9Ex)
            {
                V5Trace("DXVK-V5: DXVK Direct3DCreate9Ex intercepted");
                return reinterpret_cast<FARPROC>(&HookDxvkCreate9Ex);
            }
        }
        return result;
    }
}

bool StartDxvkEarlyLoadInterceptor()
{
    if (g_bootstrapHooksInstalled.load(std::memory_order_acquire))
        return true;

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        V5Trace("DXVK-V5: MH_Initialize failed (%d)", static_cast<int>(init));
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32)
    {
        V5Trace("DXVK-V5: kernel32.dll not found");
        return false;
    }

    void* loadLibraryA = reinterpret_cast<void*>(::GetProcAddress(kernel32, "LoadLibraryA"));
    void* getProcAddress = reinterpret_cast<void*>(::GetProcAddress(kernel32, "GetProcAddress"));
    V5Trace("DXVK-V5: bootstrap targets LoadLibraryA=%p GetProcAddress=%p",
        loadLibraryA, getProcAddress);

    if (!loadLibraryA || !getProcAddress)
        return false;

    MH_STATUS stLoad = MH_CreateHook(loadLibraryA,
        reinterpret_cast<void*>(&HookLoadLibraryA),
        reinterpret_cast<void**>(&g_realLoadLibraryA));
    V5Trace("DXVK-V5: LoadLibraryA MH_CreateHook=%d", static_cast<int>(stLoad));
    if (stLoad != MH_OK && stLoad != MH_ERROR_ALREADY_CREATED)
        return false;

    MH_STATUS stProc = MH_CreateHook(getProcAddress,
        reinterpret_cast<void*>(&HookGetProcAddress),
        reinterpret_cast<void**>(&g_realGetProcAddress));
    V5Trace("DXVK-V5: GetProcAddress MH_CreateHook=%d", static_cast<int>(stProc));
    if (stProc != MH_OK && stProc != MH_ERROR_ALREADY_CREATED)
        return false;

    MH_STATUS enLoad = MH_EnableHook(loadLibraryA);
    MH_STATUS enProc = MH_EnableHook(getProcAddress);
    V5Trace("DXVK-V5: LoadLibraryA MH_EnableHook=%d", static_cast<int>(enLoad));
    V5Trace("DXVK-V5: GetProcAddress MH_EnableHook=%d", static_cast<int>(enProc));

    const bool loadOk = (enLoad == MH_OK || enLoad == MH_ERROR_ENABLED);
    const bool procOk = (enProc == MH_OK || enProc == MH_ERROR_ENABLED);
    if (!loadOk || !procOk)
        return false;

    g_bootstrapHooksInstalled.store(true, std::memory_order_release);
    V5Trace("DXVK-V5: GetProcAddress hook ACTIVE");
    V5Trace("DXVK-V5: LoadLibraryA hook ACTIVE");

    if (HMODULE existing = GetModuleHandleW(L"dxvk.dll"))
    {
        V5Trace("DXVK-V5: dxvk.dll already resident at %p", existing);
        InstallDxvkCreationHooks(existing);
    }

    return true;
}

namespace
{
    std::atomic<bool> g_v6DiscoveryStarted{ false };
    std::atomic<bool> g_v6DeviceAttached{ false };

    bool V6IsExecutableAddress(void* p)
    {
        if (!p)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        const DWORD x = mbi.Protect & 0xFF;
        return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ ||
               x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
    }

    bool V6AddressInModule(void* p, HMODULE module)
    {
        if (!p || !module)
            return false;
        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        auto* q = reinterpret_cast<unsigned char*>(p);
        return q >= base && q < base + nt->OptionalHeader.SizeOfImage;
    }

    bool V6LooksLikeDxvkDeviceVtable(void** vt, HMODULE dxvk)
    {
        if (!vt || !dxvk)
            return false;

        // Exact fingerprint for HatVR's shipped Sep-15-2026 DXVK build.
        // dxvk+0x662BA0.  Generic "many entries point into DXVK" validation
        // also accepts unrelated DXVK objects (notably vtable +0x664070), so
        // never call a candidate unless its vtable and several method RVAs
        // match the actual D3D9 device implementation.
        __try
        {
            auto* base = reinterpret_cast<unsigned char*>(dxvk);
            auto* vtBytes = reinterpret_cast<unsigned char*>(vt);
            if (vtBytes - base != 0x662BA0)
                return false;

            struct MethodFingerprint { int slot; uintptr_t rva; };
            const MethodFingerprint fp[] = {
                { 0,  0x035F40 }, // QueryInterface
                { 3,  0x00A410 }, // TestCooperativeLevel
                { 17, 0x035E30 }, // Present
                { 42, 0x021EA0 },
                { 81, 0x028E70 },
                { 82, 0x0291D0 }, // DrawIndexedPrimitive
                { 83, 0x0295A0 },
            };
            for (const auto& m : fp)
            {
                auto* fn = reinterpret_cast<unsigned char*>(vt[m.slot]);
                if (fn - base != static_cast<ptrdiff_t>(m.rva) || !V6IsExecutableAddress(fn))
                    return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool V8VerifyLiveD3D9Device(IDirect3DDevice9* device)
    {
        if (!device)
            return false;

        D3DDEVICE_CREATION_PARAMETERS cp{};
        HRESULT hrCreation = E_FAIL;
        IDirect3DSwapChain9* swap = nullptr;
        HRESULT hrSwap = E_FAIL;
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hrBack = E_FAIL;
        D3DSURFACE_DESC desc{};

        LogCategory("DXVK", "HOTFIX-VERIFY V00 enter device=%p", device);
        MEMORY_BASIC_INFORMATION devMbi{};
        if (!VirtualQuery(device, &devMbi, sizeof(devMbi)))
        {
            LogCategory("DXVK", "HOTFIX-VERIFY V01 VirtualQuery(device) FAILED device=%p", device);
            return false;
        }
        LogCategory("DXVK", "HOTFIX-VERIFY V01 device region base=%p allocBase=%p type=0x%08lX protect=0x%08lX state=0x%08lX",
            devMbi.BaseAddress, devMbi.AllocationBase, devMbi.Type, devMbi.Protect, devMbi.State);

        // the caller has already matched this candidate against the exact
        // shipped DXVK IDirect3DDevice9 vtable fingerprint.  Do not require
        // MEM_PRIVATE here: allocation type is not the identity check anymore.
        LogCategory("DXVK", "HOTFIX-VERIFY V02 exact device-vtable fingerprint accepted device=%p type=0x%08lX", device, devMbi.Type);

        __try
        {
            LogCategory("DXVK", "HOTFIX-VERIFY V03 before GetCreationParameters device=%p", device);
            hrCreation = device->GetCreationParameters(&cp);
            LogCategory("DXVK", "HOTFIX-VERIFY V04 after GetCreationParameters hr=0x%08X", static_cast<unsigned>(hrCreation));
            V5Trace("DXVK-V8: candidate=%p GetCreationParameters hr=0x%08X adapter=%u type=%u hwnd=%p behavior=0x%08X",
                device, static_cast<unsigned>(hrCreation),
                SUCCEEDED(hrCreation) ? cp.AdapterOrdinal : 0,
                SUCCEEDED(hrCreation) ? static_cast<unsigned>(cp.DeviceType) : 0,
                SUCCEEDED(hrCreation) ? cp.hFocusWindow : nullptr,
                SUCCEEDED(hrCreation) ? cp.BehaviorFlags : 0);

            if (FAILED(hrCreation))
                return false;

            LogCategory("DXVK", "HOTFIX-VERIFY V05 before GetSwapChain device=%p", device);
            hrSwap = device->GetSwapChain(0, &swap);
            LogCategory("DXVK", "HOTFIX-VERIFY V06 after GetSwapChain hr=0x%08X swap=%p", static_cast<unsigned>(hrSwap), swap);
            V5Trace("DXVK-V8: candidate=%p GetSwapChain(0) hr=0x%08X swap=%p",
                device, static_cast<unsigned>(hrSwap), swap);
            if (FAILED(hrSwap) || !swap)
                return false;

            LogCategory("DXVK", "HOTFIX-VERIFY V07 before GetBackBuffer swap=%p", swap);
            hrBack = swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
            LogCategory("DXVK", "HOTFIX-VERIFY V08 after GetBackBuffer hr=0x%08X surface=%p", static_cast<unsigned>(hrBack), backBuffer);
            V5Trace("DXVK-V8: candidate=%p GetBackBuffer hr=0x%08X surface=%p",
                device, static_cast<unsigned>(hrBack), backBuffer);
            if (FAILED(hrBack) || !backBuffer)
            {
                swap->Release();
                return false;
            }

            LogCategory("DXVK", "HOTFIX-VERIFY V09 before GetDesc surface=%p", backBuffer);
            HRESULT hrDesc = backBuffer->GetDesc(&desc);
            LogCategory("DXVK", "HOTFIX-VERIFY V10 after GetDesc hr=0x%08X size=%ux%u", static_cast<unsigned>(hrDesc), SUCCEEDED(hrDesc) ? desc.Width : 0, SUCCEEDED(hrDesc) ? desc.Height : 0);
            V5Trace("DXVK-V8: candidate=%p BackBuffer GetDesc hr=0x%08X size=%ux%u format=%u multisample=%u",
                device, static_cast<unsigned>(hrDesc),
                SUCCEEDED(hrDesc) ? desc.Width : 0,
                SUCCEEDED(hrDesc) ? desc.Height : 0,
                SUCCEEDED(hrDesc) ? static_cast<unsigned>(desc.Format) : 0,
                SUCCEEDED(hrDesc) ? static_cast<unsigned>(desc.MultiSampleType) : 0);

            LogCategory("DXVK", "HOTFIX-VERIFY V11 before releases surface=%p swap=%p", backBuffer, swap);
            backBuffer->Release();
            swap->Release();
            LogCategory("DXVK", "HOTFIX-VERIFY V12 releases complete");
            backBuffer = nullptr;
            swap = nullptr;

            if (FAILED(hrDesc) || desc.Width == 0 || desc.Height == 0)
                return false;

            // A real game rendering device must have a usable primary swapchain
            // and non-zero backbuffer. We intentionally do not require a
            // particular window size or format.
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            const DWORD verifyCode = GetExceptionCode();
            LogCategory("DXVK", "HOTFIX-VERIFY V90 SEH code=0x%08lX device=%p swap=%p surface=%p", verifyCode, device, swap, backBuffer);
            V5Trace("DXVK-V8: candidate=%p raised exception during live-device verification", device);
            if (backBuffer) backBuffer->Release();
            if (swap) swap->Release();
            return false;
        }
    }

    DWORD WINAPI V6DiscoveryThread(LPVOID)
    {
        V5Trace("DXVK-V8: verified live-device discovery thread START");
        LogCategory("DXVK", "HOTFIX-DIAG D01 discovery thread START");

        HMODULE dxvk = nullptr;
        for (int i = 0; i < 200 && !dxvk; ++i)
        {
            dxvk = GetModuleHandleW(L"dxvk.dll");
            if (!dxvk)
                Sleep(25);
        }
        if (!dxvk)
        {
            V5Trace("DXVK-V6: dxvk.dll not resident; discovery stopped");
            return 0;
        }

        V5Trace("DXVK-V8: scanning for structurally valid DXVK D3D9 objects; dxvk=%p", dxvk);

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        auto* addr = reinterpret_cast<unsigned char*>(si.lpMinimumApplicationAddress);
        auto* maxAddr = reinterpret_cast<unsigned char*>(si.lpMaximumApplicationAddress);
        SIZE_T regions = 0, words = 0;

        while (addr < maxAddr && !g_v6DeviceAttached.load(std::memory_order_acquire))
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
                break;

            auto* next = addr + mbi.RegionSize;
            if (next <= addr)
                break;

            const DWORD p = mbi.Protect & 0xFF;
            const bool readableWritable =
                p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;

            if (mbi.State == MEM_COMMIT && readableWritable &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                mbi.RegionSize <= (256ull * 1024ull * 1024ull))
            {
                ++regions;
                __try
                {
                    auto** begin = reinterpret_cast<void**>(mbi.BaseAddress);
                    SIZE_T count = mbi.RegionSize / sizeof(void*);
                    for (SIZE_T i = 0; i < count; ++i)
                    {
                        void** vt = reinterpret_cast<void**>(begin[i]);
                        ++words;
                        if (!V6AddressInModule(vt, dxvk))
                            continue;

                        // A COM object's first machine word is its vtable pointer.
                        // Validate the vtable structurally before treating this
                        // address as IDirect3DDevice9.
                        if (!V6LooksLikeDxvkDeviceVtable(vt, dxvk))
                            continue;

                        auto* candidate = reinterpret_cast<IDirect3DDevice9*>(&begin[i]);
                        V5Trace("DXVK-V8: structural candidate object=%p vtable=%p Present=%p DIP=%p",
                            candidate, vt, vt[17], vt[82]);

                        LogCategory("DXVK", "HOTFIX-DIAG D02 structural candidate=%p; about to verify live device", candidate);
                        if (!V8VerifyLiveD3D9Device(candidate))
                        {
                            V5Trace("DXVK-V8: candidate REJECTED by live-device verification object=%p", candidate);
                            continue;
                        }

                        LogCategory("DXVK", "HOTFIX-DIAG D03 VERIFIED candidate=%p; about to call AttachExistingD3D9Device", candidate);
                        V5Trace("DXVK-V11: VERIFIED LIVE IDirect3DDevice9 object=%p; installing normal core hooks + device hooks", candidate);
                        const bool ok = AttachExistingD3D9Device(candidate);
                        LogCategory("DXVK", "HOTFIX-DIAG D04 AttachExistingD3D9Device returned=%s candidate=%p", ok ? "TRUE" : "FALSE", candidate);
                        V5Trace("DXVK-V11: AttachExistingD3D9Device=%s object=%p",
                            ok ? "TRUE" : "FALSE", candidate);

                        if (ok)
                        {
                            g_v6DeviceAttached.store(true, std::memory_order_release);
                            V5Trace("DXVK-V11: VERIFIED LIVE DXVK DEVICE ATTACHED WITH CORE INIT");
                            return 0;
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    const DWORD sehCode = GetExceptionCode();
                    LogCategory("DXVK", "HOTFIX-DIAG D90 scanner SEH caught code=0x%08lX region=%p; continuing", sehCode, mbi.BaseAddress);
                    // Region changed while scanning OR attachment faulted; continue with next region.
                }
            }
            addr = next;
        }

        V5Trace("DXVK-V8: discovery finished without verified attachment regions=%llu words=%llu",
            static_cast<unsigned long long>(regions),
            static_cast<unsigned long long>(words));
        return 0;
    }
}

bool StartDxvkExistingDeviceDiscovery()
{
    bool expected = false;
    if (!g_v6DiscoveryStarted.compare_exchange_strong(expected, true))
        return true;

    HANDLE thread = CreateThread(nullptr, 0, &V6DiscoveryThread, nullptr, 0, nullptr);
    if (!thread)
    {
        g_v6DiscoveryStarted.store(false);
        V5Trace("DXVK-V8: CreateThread failed error=%lu", GetLastError());
        return false;
    }
    CloseHandle(thread);
    V5Trace("DXVK-V8: verified live-device discovery ARMED");
    return true;
}

}
