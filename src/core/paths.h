#pragma once

#include <string>

// Path helpers. Everything WoWVR reads or writes lives next to the proxy DLL,
// which in practice is the WoW client folder.
namespace wowvr
{
    // Must be called once from DllMain with our own module handle.
    void InitPaths(void* selfModule);

    // Folder containing our d3d9.dll, with a trailing backslash.
    const std::wstring& ModuleDirectory();

    // Full path to a file sitting next to our DLL.
    std::wstring ModuleFile(const wchar_t* fileName);

    // Absolute path of the genuine system d3d9.dll (SysWOW64 for a 32-bit process).
    const std::wstring& SystemD3D9Path();

    // Executable name of the host process, e.g. "Wow.exe".
    const std::wstring& HostProcessName();
}
