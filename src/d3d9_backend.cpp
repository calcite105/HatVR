#include "d3d9_backend.h"

#include "d3d9_hook.h"
#include "graphics_backend.h"

namespace ahitvr
{
    bool ActivateD3D9Backend(IDirect3D9* d3d9)
    {
        if (!d3d9)
            return false;

        // this is an actual renderer object, not a menu/config guess.
        NoteGraphicsApiInitialized(GraphicsApi::Direct3D9);
        return HookCreateDevice(d3d9);
    }
}
