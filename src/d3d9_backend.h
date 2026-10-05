#pragma once

#include <d3d9.h>

namespace ahitvr
{
    // Direct3D 9 backend entry. The proven renderer stays behind this boundary.
    bool ActivateD3D9Backend(IDirect3D9* d3d9);
}
