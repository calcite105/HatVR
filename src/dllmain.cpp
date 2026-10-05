#include <windows.h>

namespace
{
    void AppendText(HANDLE file, const char* text)
    {
        if (file == INVALID_HANDLE_VALUE || !text)
            return;

        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    }

    void WriteBootstrapIdentity(HMODULE) {}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        WriteBootstrapIdentity(module);
    }

    return TRUE;
}
