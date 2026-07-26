#pragma once

#include <windows.h>

namespace wowvr
{
    // Redirects a function the given module imports from another DLL, by rewriting
    // its import address table entry.
    //
    // This is how we observe calls the client makes to Windows itself. Unlike a COM
    // vtable patch it needs no object to hang off, and unlike an inline hook it does
    // not rewrite any code, so there is nothing to get wrong about instruction
    // lengths and nothing another hook can trip over.
    //
    // Returns false if the module does not import that function at all, which is a
    // normal answer rather than an error - the caller decides whether it matters.
    // On success 'original' receives the address that was there before.
    bool HookImport(HMODULE module, const char* fromDll, const char* functionName,
                    void* replacement, void** original);
}
