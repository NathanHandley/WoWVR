#include "game/field_watch.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace wowvr
{
    volatile long FieldWatch::s_siteCount = 0;
    FieldWatch::Site FieldWatch::s_sites[FieldWatch::kMaxSites];
    volatile long FieldWatch::s_totalHits = 0;
    const void* FieldWatch::s_address = nullptr;

    namespace
    {
        PVOID g_handler = nullptr;
        uintptr_t g_imageBase = 0;

        uintptr_t ImageBase()
        {
            if (g_imageBase == 0)
            {
                g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            }
            return g_imageBase;
        }

        // Runs on the watched thread, inside the trap, with whatever locks the client happened
        // to be holding. Everything here is a plain load/store on a fixed array for that
        // reason: no logging, no allocation, no CRT.
        LONG CALLBACK OnDebugTrap(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            CONTEXT* ctx = info->ContextRecord;

            // Bit 0 of DR6 means DR0 matched. Anything else is a single-step or another
            // breakpoint that is not ours, and must be left for whoever set it.
            if ((ctx->Dr6 & 0x1) == 0)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            ctx->Dr6 = 0;

            const uintptr_t eip = static_cast<uintptr_t>(ctx->Eip);
            float value = 0.0f;
            if (FieldWatch::s_address != nullptr)
            {
                value = *static_cast<const float*>(FieldWatch::s_address);
            }

            const long count = FieldWatch::s_siteCount;
            for (long i = 0; i < count && i < FieldWatch::kMaxSites; ++i)
            {
                if (FieldWatch::s_sites[i].eip == eip)
                {
                    FieldWatch::s_sites[i].hits++;
                    FieldWatch::s_sites[i].lastValue = value;
                    FieldWatch::s_totalHits++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            if (count < FieldWatch::kMaxSites)
            {
                FieldWatch::s_sites[count].eip = eip;
                FieldWatch::s_sites[count].hits = 1;
                FieldWatch::s_sites[count].lastValue = value;
                FieldWatch::s_siteCount = count + 1;
            }
            FieldWatch::s_totalHits++;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // DR7 for "DR0 is a 4-byte write watch, enabled locally".
        //   bit 0      L0
        //   bits 16-17 R/W0 = 01 (data write)
        //   bits 18-19 LEN0 = 11 (four bytes)
        const DWORD kDr7Enable = 0x1u | (0x1u << 16) | (0x3u << 18);

        bool SetDebugRegisters(const void* address, bool enable)
        {
            // Setting debug registers on the running thread through its own handle is the
            // documented-enough path and is what every in-process watchpoint does; the
            // control registers in the fetched context are not meaningful for a running
            // thread, but the debug registers written back are applied.
            CONTEXT ctx;
            ZeroMemory(&ctx, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (!GetThreadContext(GetCurrentThread(), &ctx))
            {
                return false;
            }

            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            ctx.Dr0 = enable ? reinterpret_cast<DWORD_PTR>(address) : 0;
            ctx.Dr6 = 0;
            ctx.Dr7 = enable ? kDr7Enable : 0;
            return SetThreadContext(GetCurrentThread(), &ctx) != FALSE;
        }
    }

    bool FieldWatch::Arm(const void* address)
    {
        if (address == nullptr)
        {
            return false;
        }

        // A four-byte watch only fires when the address is four-byte aligned; the CPU
        // ignores a misaligned DR0 rather than reporting an error, which would look exactly
        // like "nothing ever writes it".
        if ((reinterpret_cast<uintptr_t>(address) & 0x3u) != 0)
        {
            WOWVR_WARN("Field watch: 0x%p is not 4-byte aligned, the CPU would silently "
                       "never match it.", address);
            return false;
        }

        if (g_handler == nullptr)
        {
            g_handler = AddVectoredExceptionHandler(1, OnDebugTrap);
            if (g_handler == nullptr)
            {
                WOWVR_WARN("Field watch: could not install the exception handler.");
                return false;
            }
        }

        s_address = address;
        Reset();

        if (!SetDebugRegisters(address, true))
        {
            WOWVR_WARN("Field watch: could not set the debug registers (%s).",
                       LogSystemError(GetLastError()));
            return false;
        }

        m_address = address;
        m_armed = true;
        WOWVR_INFO("Field watch armed on 0x%p (client+0x%06X if it is in the image).",
                   address, static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - ImageBase()));
        return true;
    }

    void FieldWatch::Disarm()
    {
        if (!m_armed)
        {
            return;
        }
        SetDebugRegisters(nullptr, false);
        m_armed = false;
        WOWVR_INFO("Field watch disarmed.");
    }

    void FieldWatch::Reset()
    {
        s_siteCount = 0;
        s_totalHits = 0;
        for (int i = 0; i < kMaxSites; ++i)
        {
            s_sites[i].eip = 0;
            s_sites[i].hits = 0;
            s_sites[i].lastValue = 0.0f;
        }
    }

    void FieldWatch::Report()
    {
        if (!m_armed)
        {
            WOWVR_INFO("Field watch is not armed.");
            return;
        }

        const long count = s_siteCount;
        WOWVR_INFO("Field watch on 0x%p: %ld writes from %ld distinct instructions.",
                   m_address, s_totalHits, count);
        if (count == 0)
        {
            WOWVR_INFO("  Nothing wrote it. Either the value moves from another thread, or "
                       "the field is not the one that changes.");
            return;
        }

        const uintptr_t base = ImageBase();
        for (long i = 0; i < count && i < kMaxSites; ++i)
        {
            // The trap reports the instruction AFTER the store, so the write site is just
            // below this address. Both are printed rather than guessing an instruction
            // length: the disassembly makes it obvious which one is meant.
            const uintptr_t rva = s_sites[i].eip - base;
            WOWVR_INFO("  after 0x%08X (client 0x00%06X)  %u writes, last value %.4f",
                       static_cast<unsigned>(s_sites[i].eip),
                       static_cast<unsigned>(rva + 0x400000u),
                       s_sites[i].hits, s_sites[i].lastValue);
        }
    }

    FieldWatch& Watch()
    {
        static FieldWatch instance;
        return instance;
    }
}
