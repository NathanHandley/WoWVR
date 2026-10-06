#include "ui/help_card.h"

#include "core/log.h"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <vector>

namespace wowvr
{
    namespace
    {
        struct HelpLine
        {
            const wchar_t* keys;
            const wchar_t* what;
        };

        // Keep in step with core/hotkeys.h and the Ctrl+Alt+C check in device_hooks.
        const HelpLine kLines[] = {
            { L"Ctrl+Alt+F1",       L"Show/hide this help" },
            { L"Ctrl+Alt+F8",       L"Recenter the interface" },
            { L"Ctrl+Alt+F11",      L"Recenter view and interface" },
            { L"Ctrl+Alt+Page Up",  L"Push interface away" },
            { L"Ctrl+Alt+Page Down", L"Pull interface closer" },
            { L"Ctrl+Alt+V",        L"Comfort vignette on/off" },
            { L"Ctrl+Alt+N",        L"Vignette size" },
            { L"Ctrl+Alt+F12",      L"Reload WoWVR.ini settings" },
            { L"Ctrl+Alt+F10",      L"Save eye screenshots" },
            { L"Ctrl+Alt+F9",       L"Log a frame report" },
            { L"Ctrl+Alt+C",        L"Calibrate the camera" },
        };

        const HelpLine kHintLines[] = {
            { L"Ctrl+Alt+F1", L"WoWVR command list" },
        };

        const int kFontHeight = 30;
        const int kTitleHeight = 36;
        const int kListPadding = 28;
        const int kHintPadding = 16;
        const int kLineGap = 12;
        const int kColumnGap = 40;

        const uint8_t kBackingAlpha = 0xD8;
        const uint32_t kBackingRgb = 0x101418;
        const uint32_t kBorderRgb = 0xC8A050;
        const uint32_t kKeyRgb = 0xFFD070;
        const uint32_t kTextRgb = 0xF0F0F0;
        const uint32_t kTitleRgb = 0xFFFFFF;
    }

    IDirect3DTexture9* TextCard::Texture(IDirect3DDevice9* device)
    {
        if (device == nullptr)
        {
            return nullptr;
        }
        if (m_texture != nullptr && m_device == device)
        {
            return m_texture;
        }
        if (m_failed && m_device == device)
        {
            return nullptr;
        }
        // A different device: the old texture belonged to it and goes with it.
        m_texture = nullptr;
        m_device = device;
        m_failed = true;

        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
        {
            return nullptr;
        }
        HFONT body = CreateFontW(-kFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT bold = CreateFontW(-kFontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT title = CreateFontW(-kTitleHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        const bool list = m_kind == CommandList;
        const wchar_t* const kTitle = list ? L"WoWVR commands" : nullptr;
        const HelpLine* const kLinesUsed = list ? kLines : kHintLines;
        const int count = list ? static_cast<int>(sizeof(kLines) / sizeof(kLines[0]))
                               : static_cast<int>(sizeof(kHintLines) / sizeof(kHintLines[0]));
        const int kPadding = list ? kListPadding : kHintPadding;

        // Measure: the widest key, the widest description, the title.
        int keyWidth = 0;
        int textWidth = 0;
        SIZE size = {};
        SelectObject(dc, bold);
        for (int i = 0; i < count; ++i)
        {
            GetTextExtentPoint32W(dc, kLinesUsed[i].keys, static_cast<int>(wcslen(kLinesUsed[i].keys)), &size);
            if (size.cx > keyWidth) { keyWidth = size.cx; }
        }
        SelectObject(dc, body);
        for (int i = 0; i < count; ++i)
        {
            GetTextExtentPoint32W(dc, kLinesUsed[i].what, static_cast<int>(wcslen(kLinesUsed[i].what)), &size);
            if (size.cx > textWidth) { textWidth = size.cx; }
        }
        int titleWidth = 0;
        if (kTitle != nullptr)
        {
            SelectObject(dc, title);
            GetTextExtentPoint32W(dc, kTitle, static_cast<int>(wcslen(kTitle)), &size);
            titleWidth = size.cx;
        }

        int width = kPadding * 2 + keyWidth + kColumnGap + textWidth;
        if (width < kPadding * 2 + titleWidth) { width = kPadding * 2 + titleWidth; }
        const int titleBlock = (kTitle != nullptr) ? kTitleHeight + kLineGap * 2 : 0;
        const int height = kPadding * 2 + titleBlock + count * (kFontHeight + kLineGap) - kLineGap;

        // One 32-bit DIB per colour of text would be wasteful; instead the text is drawn
        // white-on-black per colour class into a coverage DIB and composed below.
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;   // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr)
        {
            DeleteObject(body);
            DeleteObject(bold);
            DeleteObject(title);
            DeleteDC(dc);
            return nullptr;
        }
        HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);

        // Coverage is drawn in three channels at once: red for the title, green for the
        // keys, blue for the descriptions. Each glyph only lands in its own channel.
        memset(bits, 0, static_cast<size_t>(width) * height * 4);
        if (kTitle != nullptr)
        {
            SelectObject(dc, title);
            SetTextColor(dc, RGB(255, 0, 0));
            TextOutW(dc, kPadding, kPadding, kTitle, static_cast<int>(wcslen(kTitle)));
        }
        for (int i = 0; i < count; ++i)
        {
            const int y = kPadding + titleBlock + i * (kFontHeight + kLineGap);
            SelectObject(dc, bold);
            SetTextColor(dc, RGB(0, 255, 0));
            TextOutW(dc, kPadding, y, kLinesUsed[i].keys, static_cast<int>(wcslen(kLinesUsed[i].keys)));
            SelectObject(dc, body);
            SetTextColor(dc, RGB(0, 0, 255));
            TextOutW(dc, kPadding + keyWidth + kColumnGap, y, kLinesUsed[i].what,
                     static_cast<int>(wcslen(kLinesUsed[i].what)));
        }
        GdiFlush();

        // Compose: translucent backing with a thin border, then each text class laid
        // over it in its own colour by its coverage. Straight alpha.
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
        const uint32_t* coverage = static_cast<const uint32_t*>(bits);
        const int separatorY = (kTitle != nullptr) ? kPadding + kTitleHeight + kLineGap : -1;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t index = static_cast<size_t>(y) * width + x;
                const bool border = x < 2 || y < 2 || x >= width - 2 || y >= height - 2
                    || (y == separatorY && x >= kPadding && x < width - kPadding);
                uint32_t rgb = border ? kBorderRgb : kBackingRgb;
                uint32_t alpha = border ? 0xFF : kBackingAlpha;

                const uint32_t c = coverage[index];
                const uint32_t covers[3] = { (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF };
                const uint32_t colours[3] = { kTitleRgb, kKeyRgb, kTextRgb };
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t a = covers[k];
                    if (a == 0) { continue; }
                    uint32_t mixed = 0;
                    for (int shift = 0; shift <= 16; shift += 8)
                    {
                        const uint32_t from = (rgb >> shift) & 0xFF;
                        const uint32_t to = (colours[k] >> shift) & 0xFF;
                        mixed |= ((from * (255 - a) + to * a) / 255) << shift;
                    }
                    rgb = mixed;
                    alpha = alpha + ((255 - alpha) * a) / 255;
                }
                pixels[index] = (alpha << 24) | rgb;
            }
        }

        SelectObject(dc, previousBitmap);
        DeleteObject(bitmap);
        DeleteObject(body);
        DeleteObject(bold);
        DeleteObject(title);
        DeleteDC(dc);

        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(static_cast<UINT>(width), static_cast<UINT>(height), 1, 0,
                                         D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
        {
            WOWVR_WARN("Help card: could not create its texture.");
            return nullptr;
        }
        D3DLOCKED_RECT locked = {};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }
        for (int y = 0; y < height; ++y)
        {
            memcpy(static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                   pixels.data() + static_cast<size_t>(y) * width, static_cast<size_t>(width) * 4);
        }
        texture->UnlockRect(0);

        m_texture = texture;
        m_width = static_cast<uint32_t>(width);
        m_height = static_cast<uint32_t>(height);
        m_failed = false;
        return m_texture;
    }

    HelpCard& Help()
    {
        static HelpCard instance;
        return instance;
    }

    TextCard& LaunchHintCard()
    {
        static TextCard instance(TextCard::LaunchHint);
        return instance;
    }
}
