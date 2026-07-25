#pragma once

struct IDirect3D9;

namespace wowvr
{
    // Hooks IDirect3D9::CreateDevice so we get hold of the device the game renders
    // with. Safe to call for every IDirect3D9 the game creates; only the first call
    // does anything, because the vtable is shared.
    void InstallD3D9Hooks(IDirect3D9* d3d9);

    // Releases everything we own. Called on process detach.
    void ShutdownRenderer();
}
