#pragma once

namespace wowvr
{
    // Resolves every entry point in the system d3d9.dll that we forward to.
    // Returns false if the real DLL is unusable, in which case loading should fail
    // loudly rather than leaving the game with a half-wired graphics API.
    bool InitProxyExports();
}
