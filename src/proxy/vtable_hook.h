#pragma once

// Minimal COM vtable patching.
//
// We hook by swapping entries in the interface's vtable rather than by wrapping
// the interface in a C++ class. D3D9 vtables live in the runtime and are shared by
// every object of that type, so one patch covers every device, and the game never
// sees a pointer that is not the one D3D9 handed it. That last part matters:
// WoW compares interface pointers in a few places, and a wrapper object would
// also have to reproduce 119 forwarding methods without a single mistake.

namespace wowvr
{
    // Returns the vtable of a COM object.
    void** VTableOf(void* comObject);

    // Replaces one vtable slot. The previous entry is written to *originalOut, which
    // is what the hook must tail-call. Returns false if the memory could not be made
    // writable. Patching a slot that already points at 'replacement' is a no-op that
    // still reports the original.
    bool HookVTableSlot(void* comObject, unsigned slot, void* replacement, void** originalOut);
}
