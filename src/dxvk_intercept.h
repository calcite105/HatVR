#pragma once
#include <windows.h>

namespace ahitvr
{
    // Installs an early LoadLibraryA interception so AHiT's explicit
    // LoadLibraryA("dxvk.dll") can be observed before that call returns.
    bool StartDxvkEarlyLoadInterceptor();
    bool StartDxvkExistingDeviceDiscovery();
}
