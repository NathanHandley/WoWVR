#pragma once

#include <cstdint>

namespace wowvr
{
    // Which instruction in the client writes a given float.
    //
    // Reading Camera.cpp statically finds every instruction that COULD write a field - there
    // are eight for the camera distance alone, spread over a thousand lines of transitions,
    // smoothing and zoom - but not which one actually runs when the value moves. Guessing
    // between them costs a client launch each, and a wrong guess looks exactly like a right
    // one that did not help.
    //
    // The CPU already answers this. A data breakpoint in DR0 traps AFTER the store retires,
    // so the reported EIP is the instruction that did it and the memory holds what it wrote.
    // One run over one head movement names the write site outright.
    //
    // Scope and limits, since both matter for trusting the output:
    //  - Debug registers are per-thread, and this arms only the calling thread. That is the
    //    thread that calls Present, which in this client is also the thread that updates the
    //    camera - if a write came from anywhere else it would simply not appear here, so an
    //    empty report is not proof of no writes.
    //  - A real debugger attached to the process owns DR0-DR3 and will fight this.
    //  - The watch is disarmed on demand and on unload; leaving it armed costs one trap per
    //    write, which for a once-a-frame field is nothing.
    class FieldWatch
    {
    public:
        // Watches the 4 bytes at `address` for writes. Re-arming with a different address
        // replaces the previous watch. Returns false if the debug registers could not be set.
        bool Arm(const void* address);
        void Disarm();

        bool Armed() const { return m_armed; }
        const void* Address() const { return m_address; }

        // Logs the distinct writing instructions seen so far, as client RVAs so they line up
        // with a disassembly of Wow.exe, with the value each one last left behind.
        void Report();

        // Forgets everything recorded, so a report covers one movement rather than the whole
        // session. Keeps the watch armed.
        void Reset();

    private:
        struct Site
        {
            uintptr_t eip = 0;
            uint32_t hits = 0;
            float lastValue = 0.0f;
        };

        bool m_armed = false;
        const void* m_address = nullptr;

    public:
        // Touched from the exception handler, which runs on the watched thread with no lock
        // held. Kept public and trivially copyable for that reason - the handler must not
        // call anything that could allocate, log or take a lock.
        static const int kMaxSites = 32;
        static volatile long s_siteCount;
        static Site s_sites[kMaxSites];
        static volatile long s_totalHits;
        static const void* s_address;
    };

    FieldWatch& Watch();
}
