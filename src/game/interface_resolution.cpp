#include "game/interface_resolution.h"

#include "core/config.h"
#include "core/log.h"
#include "proxy/iat_hook.h"

#include <windows.h>

#include <cstring>

namespace wowvr
{
    namespace
    {
        typedef BOOL(WINAPI* EnumDisplaySettingsAFn)(LPCSTR, DWORD, DEVMODEA*);
        typedef BOOL(WINAPI* GetMonitorInfoAFn)(HMONITOR, LPMONITORINFO);
        typedef HWND(WINAPI* CreateWindowExAFn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                               HWND, HMENU, HINSTANCE, LPVOID);
        typedef BOOL(WINAPI* GetClientRectFn)(HWND, LPRECT);

        EnumDisplaySettingsAFn g_realEnumDisplaySettings = nullptr;
        GetMonitorInfoAFn g_realGetMonitorInfo = nullptr;
        CreateWindowExAFn g_realCreateWindowEx = nullptr;
        GetClientRectFn g_realGetClientRect = nullptr;

        // The game window, once it has been created smaller than the resolution the game
        // runs at. The client's own GetClientRect on it answers the full resolution, which
        // is what sizes its back buffer and lays out its interface.
        //
        // Mouse positions are deliberately NOT scaled. Measured with the client's own
        // GetCursorPosition: it reads the cursor through GetCursorPos + ScreenToClient and
        // normalises against the real window size itself, so real coordinates come out
        // exact (0.250 for a quarter of the way across), and scaling them to the virtual
        // area put every position off by the ratio (0.321).
        HWND g_gameWindow = nullptr;
        WNDPROC g_gameWindowProc = nullptr;

        // Set while CreateWindowExA is making the shrunk game window. The client asks about
        // the window from inside that call, before the handle comes back, so during
        // creation the window is recognised by its class instead.
        bool g_creatingGameWindow = false;

        // Room the window frame needs around the client area. Generous on purpose: the
        // work area only has to be big enough for the client's fit to leave the size alone.
        const LONG kFrameAllowance = 128;

        uint32_t Width() { return Cfg().interfaceWidth; }
        uint32_t Height() { return Cfg().interfaceHeight; }

        bool IsGameWindowHandle(HWND window)
        {
            if (window == nullptr)
            {
                return false;
            }
            if (window == g_gameWindow)
            {
                return true;
            }
            if (!g_creatingGameWindow || g_gameWindow != nullptr)
            {
                return false;
            }
            char name[32] = {};
            if (GetClassNameA(window, name, sizeof(name)) > 0
                && strncmp(name, "GxWindowClass", 13) == 0)
            {
                g_gameWindow = window;
                return true;
            }
            return false;
        }

        // The configured size is offered once, immediately after the last real mode. A mode
        // the display already has is not offered twice.
        BOOL WINAPI HookedEnumDisplaySettingsA(LPCSTR device, DWORD index, DEVMODEA* mode)
        {
            const BOOL real = g_realEnumDisplaySettings(device, index, mode);
            if (real || mode == nullptr || index == ENUM_CURRENT_SETTINGS
                || index == ENUM_REGISTRY_SETTINGS || index == 0)
            {
                return real;
            }

            // Only the first index past the end: the one whose predecessor still exists.
            DEVMODEA probe = {};
            probe.dmSize = sizeof(probe);
            if (!g_realEnumDisplaySettings(device, index - 1, &probe))
            {
                return FALSE;
            }
            for (DWORD i = 0; ; ++i)
            {
                DEVMODEA existing = {};
                existing.dmSize = sizeof(existing);
                if (!g_realEnumDisplaySettings(device, i, &existing))
                {
                    break;
                }
                if (existing.dmPelsWidth == Width() && existing.dmPelsHeight == Height())
                {
                    return FALSE;
                }
            }

            // The current mode with the size swapped: colour depth and refresh rate stay
            // ones the display really has.
            const WORD size = mode->dmSize;
            const WORD extra = mode->dmDriverExtra;
            DEVMODEA current = {};
            current.dmSize = sizeof(current);
            if (!g_realEnumDisplaySettings(device, ENUM_CURRENT_SETTINGS, &current))
            {
                return FALSE;
            }
            memcpy(mode, &current, size < sizeof(current) ? size : sizeof(current));
            mode->dmSize = size;
            mode->dmDriverExtra = extra;
            mode->dmPelsWidth = Width();
            mode->dmPelsHeight = Height();
            return TRUE;
        }

        BOOL WINAPI HookedGetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info)
        {
            const BOOL real = g_realGetMonitorInfo(monitor, info);
            if (!real || info == nullptr)
            {
                return real;
            }
            const LONG wantWidth = static_cast<LONG>(Width()) + kFrameAllowance;
            const LONG wantHeight = static_cast<LONG>(Height()) + kFrameAllowance;
            if (info->rcWork.right - info->rcWork.left < wantWidth)
            {
                info->rcWork.right = info->rcWork.left + wantWidth;
            }
            if (info->rcWork.bottom - info->rcWork.top < wantHeight)
            {
                info->rcWork.bottom = info->rcWork.top + wantHeight;
            }
            if (info->rcMonitor.right - info->rcMonitor.left < wantWidth)
            {
                info->rcMonitor.right = info->rcMonitor.left + wantWidth;
            }
            if (info->rcMonitor.bottom - info->rcMonitor.top < wantHeight)
            {
                info->rcMonitor.bottom = info->rcMonitor.top + wantHeight;
            }
            return real;
        }

        BOOL WINAPI HookedGetClientRect(HWND window, LPRECT rect)
        {
            const BOOL real = g_realGetClientRect(window, rect);
            if (real && rect != nullptr && IsGameWindowHandle(window)
                && rect->right > 0 && rect->bottom > 0)
            {
                rect->left = 0;
                rect->top = 0;
                rect->right = static_cast<LONG>(Width());
                rect->bottom = static_cast<LONG>(Height());
            }
            return real;
        }

        // The client hears the full resolution in WM_SIZE too, so a size message never
        // tells it something its GetClientRect contradicts.
        LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_SIZE && wParam != SIZE_MINIMIZED
                && LOWORD(lParam) > 0 && HIWORD(lParam) > 0)
            {
                lParam = MAKELPARAM(static_cast<WORD>(Width()), static_cast<WORD>(Height()));
            }
            return CallWindowProcA(g_gameWindowProc, window, message, wParam, lParam);
        }

        bool IsGameWindow(LPCSTR className, LPCSTR windowName)
        {
            // An atom, not a string, is never the game window's class.
            if (className == nullptr || (reinterpret_cast<uintptr_t>(className) >> 16) == 0
                || windowName == nullptr)
            {
                return false;
            }
            return strncmp(className, "GxWindowClass", 13) == 0
                && strcmp(windowName, "World of Warcraft") == 0;
        }

        HWND WINAPI HookedCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR windowName,
                                          DWORD style, int x, int y, int width, int height,
                                          HWND parent, HMENU menu, HINSTANCE instance,
                                          LPVOID parameter)
        {
            bool shrunk = false;
            if (IsGameWindow(className, windowName) && width > 0 && height > 0
                && (style & WS_CAPTION) == WS_CAPTION)
            {
                // The real work area of the monitor the window opens on, by our own call -
                // the hook above only changes the client's answer.
                POINT at = { x, y };
                MONITORINFO real = {};
                real.cbSize = sizeof(real);
                const HMONITOR monitor = MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY);
                if (GetMonitorInfoW(monitor, &real))
                {
                    const int areaWidth = real.rcWork.right - real.rcWork.left;
                    const int areaHeight = real.rcWork.bottom - real.rcWork.top;
                    if (width > areaWidth || height > areaHeight)
                    {
                        // Scale the client area to fit, aspect kept, then add the frame back.
                        RECT frame = { 0, 0, 0, 0 };
                        AdjustWindowRectEx(&frame, style, FALSE, exStyle);
                        const int frameWidth = frame.right - frame.left;
                        const int frameHeight = frame.bottom - frame.top;
                        const double clientWidth = static_cast<double>(width - frameWidth);
                        const double clientHeight = static_cast<double>(height - frameHeight);
                        double scale = static_cast<double>(areaWidth - frameWidth) / clientWidth;
                        const double scaleY =
                            static_cast<double>(areaHeight - frameHeight) / clientHeight;
                        if (scaleY < scale) { scale = scaleY; }
                        width = static_cast<int>(clientWidth * scale) + frameWidth;
                        height = static_cast<int>(clientHeight * scale) + frameHeight;
                        x = real.rcWork.left;
                        y = real.rcWork.top;
                        shrunk = true;
                        WOWVR_INFO("Interface resolution: the game runs at %ux%u; its window is "
                                   "%dx%d to fit the screen (Present scales into it).",
                                   Width(), Height(), width - frameWidth, height - frameHeight);
                    }
                }
            }
            g_creatingGameWindow = shrunk;
            const HWND window = g_realCreateWindowEx(exStyle, className, windowName, style, x, y,
                                                     width, height, parent, menu, instance,
                                                     parameter);
            g_creatingGameWindow = false;
            if (shrunk && window != nullptr)
            {
                g_gameWindow = window;
                g_gameWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                    window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWindowProc)));
                SendMessageA(window, WM_SIZE, SIZE_RESTORED,
                             MAKELPARAM(static_cast<WORD>(Width()), static_cast<WORD>(Height())));
            }
            return window;
        }

        template <typename Fn>
        bool Hook(HMODULE game, const char* name, void* replacement, Fn& original)
        {
            void* previous = nullptr;
            if (!HookImport(game, "user32.dll", name, replacement, &previous) || previous == nullptr)
            {
                WOWVR_WARN("Interface resolution: could not hook %s.", name);
                return false;
            }
            original = reinterpret_cast<Fn>(previous);
            return true;
        }
    }

    float InterfacePixelScale()
    {
        RECT real = {};
        if (g_gameWindow == nullptr || !GetClientRect(g_gameWindow, &real) || real.right <= 0)
        {
            return 1.0f;
        }
        return static_cast<float>(Width()) / static_cast<float>(real.right);
    }

    void InstallInterfaceResolution()
    {
        if (Width() == 0 || Height() == 0)
        {
            return;
        }
        HMODULE game = GetModuleHandleW(nullptr);

        // GetClientRect goes in first and the window is only ever shrunk (CreateWindowExA,
        // last) once it is in place: a shrunk window the client measures at its real size
        // would get a back buffer of that size and gain nothing.
        const bool measured = Hook(game, "GetClientRect",
                                   reinterpret_cast<void*>(&HookedGetClientRect),
                                   g_realGetClientRect);
        const bool listed = Hook(game, "EnumDisplaySettingsA",
                                 reinterpret_cast<void*>(&HookedEnumDisplaySettingsA),
                                 g_realEnumDisplaySettings);
        const bool fitted = Hook(game, "GetMonitorInfoA",
                                 reinterpret_cast<void*>(&HookedGetMonitorInfoA),
                                 g_realGetMonitorInfo);
        const bool windowed = measured
            && Hook(game, "CreateWindowExA", reinterpret_cast<void*>(&HookedCreateWindowExA),
                    g_realCreateWindowEx);
        WOWVR_INFO("Interface resolution %ux%u: %s.", Width(), Height(),
                   (measured && listed && fitted && windowed)
                       ? "hooks in place"
                       : "some hooks could not be placed - the game may fall back to a "
                         "resolution the screen has");
    }
}
