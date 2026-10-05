#include <windows.h>
#include <d3d9.h>

static void V3CProxyLog(const char*) {}

static void V3CProxyMarkActiveOnce()
{
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return;

    V3CProxyLog("DXVK-V4: PROXY BUILD ACTIVE\r\n");

    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&V3CProxyMarkActiveOnce), &self) &&
        GetModuleFileNameA(self, path, MAX_PATH))
    {
        V3CProxyLog("DXVK-V3D: actual proxy module path: ");
        V3CProxyLog(path);
        V3CProxyLog("\r\n");
    }
}

namespace
{
    HMODULE g_realD3D9 = nullptr;
    HMODULE g_hatVR = nullptr;
    FARPROC g_exports[23] = {};

    bool IsHatInTimeGameProcess()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return false;

        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                name = p + 1;
        }

        return _wcsicmp(name, L"HatinTimeGame.exe") == 0;
    }

    using HatVRHookCreateDeviceFn = BOOL(WINAPI*)(IDirect3D9*);
    HatVRHookCreateDeviceFn g_hatVRHookCreateDevice = nullptr;

    void ProxyLog(const char*) {}

    bool GetProxyDirectory(char (&directory)[MAX_PATH])
    {
        HMODULE selfModule = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&GetProxyDirectory),
                &selfModule))
            return false;

        if (!GetModuleFileNameA(selfModule, directory, MAX_PATH))
            return false;

        char* lastSlash = nullptr;
        for (char* p = directory; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                lastSlash = p;
        }
        if (!lastSlash)
            return false;

        *(lastSlash + 1) = '\0';
        return true;
    }

    using HatVREarlyBootstrapFn = BOOL (WINAPI*)();

    void RunHatVREarlyBootstrap()
    {
        static volatile LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) != 0)
            return;

        V3CProxyLog("DXVK-V4: EARLY BOOTSTRAP requested\r\n");

        if (!g_hatVR)
        {
            V3CProxyLog("DXVK-V4: EARLY BOOTSTRAP failed: HatVR.dll handle is null\r\n");
            return;
        }

        auto bootstrap = reinterpret_cast<HatVREarlyBootstrapFn>(
            GetProcAddress(g_hatVR, "HatVR_EarlyBootstrap"));

        if (!bootstrap)
        {
            V3CProxyLog("DXVK-V4: EARLY BOOTSTRAP failed: export not found\r\n");
            return;
        }

        V3CProxyLog("DXVK-V4: HatVR_EarlyBootstrap resolved; calling now\r\n");
        const BOOL ok = bootstrap();
        V3CProxyLog(ok
            ? "DXVK-V4: HatVR_EarlyBootstrap returned TRUE\r\n"
            : "DXVK-V4: HatVR_EarlyBootstrap returned FALSE\r\n");
    }

    bool EnsureHatVR()
    {
        if (!IsHatInTimeGameProcess())
            return false;

        if (g_hatVRHookCreateDevice)
            return true;

        if (!g_hatVR)
        {
            char path[MAX_PATH] = {};
            if (!GetProxyDirectory(path))
            {
                ProxyLog("HatVR: failed to resolve proxy directory\n");
                return false;
            }

            lstrcatA(path, "HatVR.dll");

            ProxyLog("DXVK-V3D: attempting HatVR load from: ");
            ProxyLog(path);
            ProxyLog("\r\n");

            g_hatVR = LoadLibraryA(path);
            if (!g_hatVR)
            {
                ProxyLog("HatVR: LoadLibraryA(HatVR.dll) FAILED\n");
                return false;
            }

            ProxyLog("HatVR.dll LOADED\n");

            char loadedHatVRPath[MAX_PATH] = {};
            if (GetModuleFileNameA(g_hatVR, loadedHatVRPath, MAX_PATH))
            {
                ProxyLog("DXVK-V3D: actual HatVR module path: ");
                ProxyLog(loadedHatVRPath);
                ProxyLog("\r\n");
            }
        }

        RunHatVREarlyBootstrap();

        g_hatVRHookCreateDevice = reinterpret_cast<HatVRHookCreateDeviceFn>(
            GetProcAddress(g_hatVR, "HatVR_HookCreateDevice"));

        if (!g_hatVRHookCreateDevice)
        {
            ProxyLog("HatVR: HatVR_HookCreateDevice export NOT FOUND\n");
            return false;
        }

        ProxyLog("HatVR_HookCreateDevice resolved\n");
        return true;
    }

    bool EnsureRealD3D9()
    {
        if (g_realD3D9)
            return true;

        char systemDir[MAX_PATH] = {};
        if (!GetSystemDirectoryA(systemDir, MAX_PATH))
        {
            ProxyLog("GetSystemDirectoryA FAILED\n");
            return false;
        }

        char path[MAX_PATH] = {};
        lstrcpyA(path, systemDir);
        lstrcatA(path, "\\d3d9.dll");

        g_realD3D9 = LoadLibraryA(path);
        if (!g_realD3D9)
        {
            ProxyLog("LoadLibraryA(real d3d9.dll) FAILED\n");
            return false;
        }

        ProxyLog("Real system d3d9.dll LOADED\n");

        // Bootstrap HatVR as soon as AHiT touches the local D3D9 proxy.
        // Backend activation remains tied to an actual renderer event:
        // Direct3DCreate9 hands us a real IDirect3D9; Vulkan will have its
        // own activation path inside HatVR.dll.
        EnsureHatVR();

        return true;
    }

    void InstallHatVRHooks(IDirect3D9* d3d9)
    {
        if (!d3d9)
            return;

        if (!EnsureHatVR())
        {
            ProxyLog("HatVR hook skipped because HatVR.dll could not be initialized\n");
            return;
        }

        const BOOL result = g_hatVRHookCreateDevice(d3d9);
        ProxyLog(result ? "HatVR_HookCreateDevice returned TRUE\n"
                        : "HatVR_HookCreateDevice returned FALSE\n");
    }
}

extern "C" void* ResolveD3D9Export(unsigned int ordinal)
{
    if (ordinal < 16 || ordinal > 38)
        return nullptr;

    const unsigned int index = ordinal - 16;
    if (g_exports[index])
        return reinterpret_cast<void*>(g_exports[index]);

    if (!EnsureRealD3D9())
        return nullptr;

    FARPROC proc = GetProcAddress(
        g_realD3D9,
        MAKEINTRESOURCEA(static_cast<WORD>(ordinal)));

    if (!proc)
        return nullptr;

    g_exports[index] = proc;
    return reinterpret_cast<void*>(proc);
}

extern "C" IDirect3D9* WINAPI Hook_Direct3DCreate9On12(
    UINT sdkVersion,
    void* pOverrideList,
    UINT numOverrideEntries)
{
    V3CProxyMarkActiveOnce();
    V3CProxyMarkActiveOnce();
    using Direct3DCreate9On12Fn = IDirect3D9*(WINAPI*)(UINT, void*, UINT);

    auto realFn = reinterpret_cast<Direct3DCreate9On12Fn>(ResolveD3D9Export(20));
    if (!realFn)
        return nullptr;

    IDirect3D9* d3d9 = realFn(sdkVersion, pOverrideList, numOverrideEntries);
    InstallHatVRHooks(d3d9);
    return d3d9;
}

extern "C" IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdkVersion)
{
    using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

    auto realFn = reinterpret_cast<Direct3DCreate9Fn>(ResolveD3D9Export(37));
    if (!realFn)
        return nullptr;

    IDirect3D9* d3d9 = realFn(sdkVersion);
    InstallHatVRHooks(d3d9);
    return d3d9;
}
