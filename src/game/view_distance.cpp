#include "game/view_distance.h"

#include "core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;
        const uintptr_t kSiteRva = 0x0078144Fu - kPublishedImageBase;
        const uintptr_t kFarClipRva = 0x00CD7748u - kPublishedImageBase;

        // add esp, 8 ; fld dword ptr [0x009E8D84]  - two whole instructions, nine bytes.
        // The fld's operand is an absolute address, so it is rebased before comparing
        // and the trampoline is built from the live bytes, not from this table.
        const uint8_t kSiteBytes[9] = { 0x83u, 0xC4u, 0x08u, 0xD9u, 0x05u,
                                        0x84u, 0x8Du, 0x9Eu, 0x00u };
        const uintptr_t kFldOperandPublished = 0x009E8D84u;
        const size_t kFldOperandOffset = 5;

        // Read by the naked detour. Plain globals: it cannot reach a this-pointer.
        float g_scale = 1.0f;
        float g_clientFar = 0.0f;
        uintptr_t g_farClipAddress = 0;
        uint8_t* g_trampoline = nullptr;
        bool g_installed = false;
        bool g_failed = false;

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        // Entered with the client's far still in st(0), right after it was stored.
        __declspec(naked) void DrawDistanceDetour()
        {
            __asm
            {
                fst  dword ptr [g_clientFar]
                fmul dword ptr [g_scale]
                push eax
                mov  eax, g_farClipAddress
                fst  dword ptr [eax]
                pop  eax
                jmp  dword ptr [g_trampoline]
            }
        }
    }

    bool ViewDistance::Install()
    {
        if (g_installed)
        {
            return true;
        }
        if (g_failed)
        {
            return false;
        }

        uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + kSiteRva);
        uint8_t expected[sizeof(kSiteBytes)];
        memcpy(expected, kSiteBytes, sizeof(expected));
        const uint32_t operand =
            static_cast<uint32_t>(kFldOperandPublished - kPublishedImageBase + ImageBase());
        memcpy(expected + kFldOperandOffset, &operand, 4);

        if (memcmp(site, expected, sizeof(expected)) != 0)
        {
            g_failed = true;
            WOWVR_WARN("View distance: 0x%08X does not hold the expected bytes; the client's "
                       "own draw distance is left alone.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }

        uint8_t* stub = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
        {
            g_failed = true;
            WOWVR_WARN("View distance: could not allocate the trampoline.");
            return false;
        }
        size_t n = 0;
        memcpy(stub, site, sizeof(expected));
        n += sizeof(expected);
        stub[n++] = 0xE9u;
        const uintptr_t resume = reinterpret_cast<uintptr_t>(site) + sizeof(expected);
        const int32_t back = static_cast<int32_t>(resume - (reinterpret_cast<uintptr_t>(stub) + n + 4));
        memcpy(stub + n, &back, 4);
        n += 4;
        FlushInstructionCache(GetCurrentProcess(), stub, n);

        g_farClipAddress = ImageBase() + kFarClipRva;
        g_trampoline = stub;

        uint8_t patched[sizeof(expected)];
        patched[0] = 0xE9u;
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&DrawDistanceDetour) - (reinterpret_cast<uintptr_t>(site) + 5));
        memcpy(patched + 1, &rel, 4);
        for (size_t i = 5; i < sizeof(patched); ++i)
        {
            patched[i] = 0x90u;
        }

        DWORD previous = 0;
        if (!VirtualProtect(site, sizeof(patched), PAGE_EXECUTE_READWRITE, &previous))
        {
            g_failed = true;
            WOWVR_WARN("View distance: 0x%08X could not be made writable.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }
        memcpy(site, patched, sizeof(patched));
        VirtualProtect(site, sizeof(patched), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(patched));

        g_installed = true;
        WOWVR_INFO("View distance: hooked the client's draw-distance update at 0x%08X.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
        return true;
    }

    void ViewDistance::SetScale(float scale)
    {
        if (!(scale >= 1.0f)) { scale = 1.0f; }
        if (scale > 4.0f) { scale = 4.0f; }
        g_scale = scale;
    }

    float ViewDistance::ClientDistance() const
    {
        return g_clientFar;
    }

    float ViewDistance::DrawDistance() const
    {
        return g_clientFar * g_scale;
    }

    ViewDistance& DrawRange()
    {
        static ViewDistance instance;
        return instance;
    }
}
