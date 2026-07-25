#include "core/paths.h"

#include <windows.h>

#include <iterator>

namespace wowvr
{
    namespace
    {
        std::wstring g_moduleDirectory;
        std::wstring g_systemD3D9Path;
        std::wstring g_hostProcessName;

        std::wstring DirectoryOf(const std::wstring& filePath)
        {
            const size_t slash = filePath.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return std::wstring();
            }
            return filePath.substr(0, slash + 1);
        }

        std::wstring FileNameOf(const std::wstring& filePath)
        {
            const size_t slash = filePath.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return filePath;
            }
            return filePath.substr(slash + 1);
        }

        std::wstring QueryModulePath(HMODULE module)
        {
            wchar_t buffer[MAX_PATH * 2];
            const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
            if (length == 0 || length >= std::size(buffer))
            {
                return std::wstring();
            }
            return std::wstring(buffer, length);
        }
    }

    void InitPaths(void* selfModule)
    {
        const std::wstring selfPath = QueryModulePath(static_cast<HMODULE>(selfModule));
        g_moduleDirectory = DirectoryOf(selfPath);

        g_hostProcessName = FileNameOf(QueryModulePath(nullptr));

        // GetSystemDirectoryW honours WOW64 redirection, so a 32-bit process
        // gets SysWOW64 here, which is exactly the d3d9.dll we want to forward to.
        wchar_t systemDirectory[MAX_PATH];
        const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
        if (length > 0 && length < std::size(systemDirectory))
        {
            g_systemD3D9Path.assign(systemDirectory, length);
            if (g_systemD3D9Path.back() != L'\\')
            {
                g_systemD3D9Path += L'\\';
            }
            g_systemD3D9Path += L"d3d9.dll";
        }
        else
        {
            g_systemD3D9Path = L"C:\\Windows\\SysWOW64\\d3d9.dll";
        }
    }

    const std::wstring& ModuleDirectory()
    {
        return g_moduleDirectory;
    }

    std::wstring ModuleFile(const wchar_t* fileName)
    {
        return g_moduleDirectory + fileName;
    }

    const std::wstring& SystemD3D9Path()
    {
        return g_systemD3D9Path;
    }

    const std::wstring& HostProcessName()
    {
        return g_hostProcessName;
    }
}
