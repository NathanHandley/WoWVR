#pragma once

#include <windows.h>

// Access to the genuine system d3d9.dll that this proxy sits in front of.
namespace wowvr
{
    bool LoadRealD3D9();
    void UnloadRealD3D9();

    HMODULE RealD3D9Module();

    FARPROC RealProc(const char* name);
    FARPROC RealProcByOrdinal(WORD ordinal);
}
