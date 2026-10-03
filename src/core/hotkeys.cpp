#include "core/hotkeys.h"

#include <windows.h>

namespace wowvr
{
    namespace
    {
        struct HotkeyBinding
        {
            int virtualKey;
            bool wasDown;
        };

        HotkeyBinding g_bindings[static_cast<int>(Hotkey::Count)] = {
            { VK_F9,  false },
            { VK_F10, false },
            { VK_F11, false },
            { VK_F12, false },
            { VK_F8,  false },
        };
    }

    bool HotkeyPressed(Hotkey key)
    {
        const int index = static_cast<int>(key);
        if (index < 0 || index >= static_cast<int>(Hotkey::Count))
        {
            return false;
        }

        HotkeyBinding& binding = g_bindings[index];

        // GetAsyncKeyState rather than the message queue: WoW consumes its own input,
        // and we only need an edge, not a proper key handler.
        // Ctrl+Alt is required because the bare function keys are already bound in
        // WoW - F9 opens a bag - and a diagnostic hotkey that also does something in
        // the game is a nuisance while testing.
        const bool modifiers = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
                            && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        const bool isDown = modifiers && (GetAsyncKeyState(binding.virtualKey) & 0x8000) != 0;
        const bool pressed = isDown && !binding.wasDown;
        binding.wasDown = isDown;
        return pressed;
    }
}
