#include "proxy/iat_hook.h"

#include "core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        // The import tables hold offsets from the module's load address, so every
        // pointer into them has to be rebased through the base first.
        template <typename T>
        T* AtOffset(HMODULE module, size_t offset)
        {
            return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(module) + offset);
        }

        const IMAGE_IMPORT_DESCRIPTOR* FirstImportDescriptor(HMODULE module)
        {
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return nullptr;
            }

            const IMAGE_NT_HEADERS* nt = AtOffset<IMAGE_NT_HEADERS>(module, dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return nullptr;
            }

            const IMAGE_DATA_DIRECTORY& directory =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (directory.VirtualAddress == 0 || directory.Size == 0)
            {
                return nullptr;
            }

            return AtOffset<IMAGE_IMPORT_DESCRIPTOR>(module, directory.VirtualAddress);
        }

        // Overwrites one import slot. The table is in a read-only section, so the
        // page has to be made writable and then put back exactly as it was - leaving
        // it writable would be a quiet weakening of the process's memory protection.
        bool WriteImportSlot(void** slot, void* value, void** previous)
        {
            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                return false;
            }

            *previous = *slot;
            *slot = value;

            DWORD restored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &restored);
            return true;
        }
    }

    bool HookImport(HMODULE module, const char* fromDll, const char* functionName,
                    void* replacement, void** original)
    {
        if (module == nullptr || fromDll == nullptr || functionName == nullptr
            || replacement == nullptr || original == nullptr)
        {
            return false;
        }

        const IMAGE_IMPORT_DESCRIPTOR* descriptor = FirstImportDescriptor(module);
        if (descriptor == nullptr)
        {
            return false;
        }

        for (; descriptor->Name != 0; ++descriptor)
        {
            const char* name = AtOffset<const char>(module, descriptor->Name);
            if (_stricmp(name, fromDll) != 0)
            {
                continue;
            }

            // OriginalFirstThunk keeps the names; FirstThunk holds the addresses the
            // loader filled in and is what actually gets called. They run in step, so
            // the name is searched in one and the patch applied at the same index in
            // the other. A bound import has no name table, and there is nothing to
            // match against in that case.
            if (descriptor->OriginalFirstThunk == 0)
            {
                continue;
            }

            const IMAGE_THUNK_DATA* names =
                AtOffset<IMAGE_THUNK_DATA>(module, descriptor->OriginalFirstThunk);
            IMAGE_THUNK_DATA* addresses =
                AtOffset<IMAGE_THUNK_DATA>(module, descriptor->FirstThunk);

            for (; names->u1.AddressOfData != 0; ++names, ++addresses)
            {
                // Imported by ordinal rather than by name: nothing to compare.
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    continue;
                }

                const IMAGE_IMPORT_BY_NAME* import =
                    AtOffset<IMAGE_IMPORT_BY_NAME>(module, names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(import->Name), functionName) != 0)
                {
                    continue;
                }

                void** slot = reinterpret_cast<void**>(&addresses->u1.Function);
                if (!WriteImportSlot(slot, replacement, original))
                {
                    WOWVR_WARN("Found %s!%s in the import table but could not make its "
                               "entry writable.", fromDll, functionName);
                    return false;
                }
                return true;
            }
        }

        return false;
    }
}
