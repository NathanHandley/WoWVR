#include "game/float_text.h"

#include "core/log.h"
#include "game/ui_canvas.h"
#include "game/world_pointer.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // ClntObjMgrGetActivePlayerObj: the local player's object or null (as used by
        // game/interior_view.cpp); its position (feet) at +0x798.
        const uintptr_t kActivePlayerObjectRva = 0x004038F0u - kPublishedImageBase;
        typedef void*(__cdecl* ActivePlayerObjectFn)();
        const uintptr_t kUnitPosition = 0x798u;

        // The UI's Lua state, and the client's own Lua 5.1 API (3.3.5a 12340): the same
        // functions FrameScript_RegisterFunction (0x00817F90) is built from.
        const uintptr_t kLuaStateRva = 0x00D3F78Cu - kPublishedImageBase;
        const uintptr_t kGetFieldRva = 0x0084E590u - kPublishedImageBase;
        const uintptr_t kToLStringRva = 0x0084E0E0u - kPublishedImageBase;
        const uintptr_t kSetTopRva = 0x0084DBF0u - kPublishedImageBase;
        // Their first bytes, checked before any is called.
        const uint8_t kGetFieldEntry[6] = { 0x55u, 0x8Bu, 0xECu, 0x83u, 0xECu, 0x10u };
        const uint8_t kToLStringEntry[6] = { 0x55u, 0x8Bu, 0xECu, 0x56u, 0x8Bu, 0x75u };
        const uint8_t kSetTopEntry[6] = { 0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x4Du, 0x0Cu };
        const int kLuaGlobalsIndex = -10002;
        typedef void(__cdecl* GetFieldFn)(void* L, int index, const char* key);
        typedef const char*(__cdecl* ToLStringFn)(void* L, int index, size_t* length);
        typedef void(__cdecl* SetTopFn)(void* L, int index);

        // Installed once per Lua state (a /reload makes a new one, and WOWVR_FCT_HOOKED
        // with it). Blizzard_CombatText loads on demand, so until it has, this does
        // nothing and is simply tried again. Fields: text, r, g, b, display type,
        // separated by \31, messages ended by \30. Bounded, in case nothing reads it.
        const char* const kInstallLua =
            "if not CombatText_AddMessage then return end "
            "if not WOWVR_FCT_HOOKED then WOWVR_FCT_HOOKED=1 "
            "  hooksecurefunc('CombatText_AddMessage',function(m,f,r,g,b,t) "
            "    if not WOWVR_FCT_ON or not m then return end "
            "    local q=WOWVR_FCT_OUT or '' if #q>3000 then return end "
            "    WOWVR_FCT_OUT=q..tostring(m)..'\\31'..(r or 1)..'\\31'..(g or 1)..'\\31'"
            "      ..(b or 1)..'\\31'..(t or '')..'\\30' end) end ";
        const char* const kOnLua = "WOWVR_FCT_ON=true if CombatText then CombatText:SetAlpha(0) end";
        const char* const kOffLua =
            "WOWVR_FCT_ON=nil WOWVR_FCT_OUT=nil if CombatText then CombatText:SetAlpha(1) end";

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        bool LuaApiChecked()
        {
            static int state = 0;   // 0 unknown, 1 good, -1 bad
            if (state == 0)
            {
                const uintptr_t base = ImageBase();
                state = (memcmp(reinterpret_cast<const void*>(base + kGetFieldRva), kGetFieldEntry, 6) == 0
                         && memcmp(reinterpret_cast<const void*>(base + kToLStringRva), kToLStringEntry, 6) == 0
                         && memcmp(reinterpret_cast<const void*>(base + kSetTopRva), kSetTopEntry, 6) == 0)
                    ? 1 : -1;
                if (state < 0)
                {
                    WOWVR_WARN("Floating text: the client's Lua functions are not the 3.3.5a (12340) "
                               "code this was written against; combat text stays on the interface.");
                }
            }
            return state > 0;
        }

        void* PlayerObject()
        {
            __try
            {
                return reinterpret_cast<ActivePlayerObjectFn>(ImageBase() + kActivePlayerObjectRva)();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        // The queue's contents copied out (at most 'capacity' bytes), and true if there
        // was a string there. The stack is left as it was found.
        bool ReadQueue(char* out, size_t capacity)
        {
            __try
            {
                void* L = *reinterpret_cast<void**>(ImageBase() + kLuaStateRva);
                if (L == nullptr)
                {
                    return false;
                }
                reinterpret_cast<GetFieldFn>(ImageBase() + kGetFieldRva)(L, kLuaGlobalsIndex,
                                                                          "WOWVR_FCT_OUT");
                size_t length = 0;
                const char* text =
                    reinterpret_cast<ToLStringFn>(ImageBase() + kToLStringRva)(L, -1, &length);
                bool got = false;
                if (text != nullptr && length > 0)
                {
                    if (length >= capacity) { length = capacity - 1; }
                    memcpy(out, text, length);
                    out[length] = 0;
                    got = true;
                }
                reinterpret_cast<SetTopFn>(ImageBase() + kSetTopRva)(L, -2);
                return got;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The client's text font if it is beside the game, otherwise a bold Arial.
        const wchar_t* FontFace()
        {
            static const wchar_t* face = nullptr;
            if (face == nullptr)
            {
                face = L"Arial";
                wchar_t path[MAX_PATH] = {};
                const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
                wchar_t* slash = wcsrchr(path, L'\\');
                if (length > 0 && slash != nullptr)
                {
                    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"Fonts\\FRIZQT__.TTF");
                    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES
                        && AddFontResourceExW(path, FR_PRIVATE, nullptr) > 0)
                    {
                        face = L"Friz Quadrata TT";
                    }
                }
                WOWVR_INFO("Floating text: drawn in %s.", face == nullptr ? "?" : LogWide(face));
            }
            return face;
        }

        // The text in its colour with a dark outline, straight alpha, as a texture.
        IDirect3DTexture9* BuildTexture(IDirect3DDevice9* device, const wchar_t* text, unsigned rgb,
                                        float& aspect)
        {
            const int kHeight = 56;
            const int kOutline = 3;
            const int length = static_cast<int>(wcslen(text));
            if (length == 0)
            {
                return nullptr;
            }
            HDC dc = CreateCompatibleDC(nullptr);
            if (dc == nullptr)
            {
                return nullptr;
            }
            HFONT font = CreateFontW(-kHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, FontFace());
            HGDIOBJ previousFont = SelectObject(dc, font);
            SIZE size = {};
            GetTextExtentPoint32W(dc, text, length, &size);
            const int width = size.cx + kOutline * 2 + 4;
            const int height = size.cy + kOutline * 2 + 2;

            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap == nullptr || bits == nullptr)
            {
                SelectObject(dc, previousFont);
                DeleteObject(font);
                DeleteDC(dc);
                return nullptr;
            }
            HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
            SetBkMode(dc, TRANSPARENT);
            memset(bits, 0, static_cast<size_t>(width) * height * 4);
            // Outline coverage in green (the text stamped all round), the text in red.
            SetTextColor(dc, RGB(0, 255, 0));
            for (int dy = -kOutline; dy <= kOutline; ++dy)
            {
                for (int dx = -kOutline; dx <= kOutline; ++dx)
                {
                    if (dx * dx + dy * dy <= kOutline * kOutline && (dx != 0 || dy != 0))
                    {
                        TextOutW(dc, kOutline + 2 + dx, kOutline + 1 + dy, text, length);
                    }
                }
            }
            SetTextColor(dc, RGB(255, 0, 0));
            TextOutW(dc, kOutline + 2, kOutline + 1, text, length);
            GdiFlush();

            std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
            const uint32_t* coverage = static_cast<const uint32_t*>(bits);
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                const uint32_t textCover = (coverage[i] >> 16) & 0xFFu;
                const uint32_t edgeCover = (coverage[i] >> 8) & 0xFFu;
                const uint32_t alpha = textCover > edgeCover ? textCover : edgeCover;
                uint32_t colour = 0;
                for (int shift = 0; shift <= 16; shift += 8)
                {
                    // Over the black outline by the text's own coverage.
                    colour |= ((((rgb >> shift) & 0xFFu) * textCover) / 255u) << shift;
                }
                // Straight alpha: undo the darkening where the outline is only partial.
                if (alpha > 0 && textCover < alpha)
                {
                    uint32_t lifted = 0;
                    for (int shift = 0; shift <= 16; shift += 8)
                    {
                        lifted |= ((((colour >> shift) & 0xFFu) * 255u) / alpha) << shift;
                    }
                    colour = lifted;
                }
                pixels[i] = (alpha << 24) | colour;
            }
            SelectObject(dc, previousBitmap);
            SelectObject(dc, previousFont);
            DeleteObject(bitmap);
            DeleteObject(font);
            DeleteDC(dc);

            IDirect3DTexture9* texture = nullptr;
            if (FAILED(device->CreateTexture(static_cast<UINT>(width), static_cast<UINT>(height), 1, 0,
                                             D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
            {
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
            aspect = static_cast<float>(width) / static_cast<float>(height);
            return texture;
        }

        unsigned Channel(const char* text)
        {
            double value = atof(text);
            if (!(value >= 0.0)) { value = 0.0; }
            if (value > 1.0) { value = 1.0; }
            return static_cast<unsigned>(value * 255.0 + 0.5);
        }
    }

    void FloatText::Update(bool enabled, float seconds)
    {
        for (int i = 0; i < m_count; ++i)
        {
            m_messages[i].age += seconds;
        }
        m_sinceLast += seconds;
        // Expired ones out, keeping the order (oldest first).
        int kept = 0;
        for (int i = 0; i < m_count; ++i)
        {
            if (m_messages[i].age < kLifetime)
            {
                m_messages[kept] = m_messages[i];
                m_pending[kept] = m_pending[i];
                ++kept;
            }
            else if (m_messages[i].texture != nullptr)
            {
                m_messages[i].texture->Release();
            }
        }
        m_count = kept;

        const bool inWorld = PlayerObject() != nullptr;
        // Drawn through world pointing's view of the camera; without it the client's own
        // text stays.
        if (!enabled || !inWorld || !Pointer().IsActive() || !LuaApiChecked())
        {
            if (m_luaOn && inWorld)
            {
                Canvas().RunDebugScript(kOffLua);
            }
            m_luaOn = false;
            m_framesUntilInstall = 0;
            if (!enabled)
            {
                ReleaseTextures();
            }
            return;
        }

        // The hook and the switch, now and then: cheap, and it catches a /reload (a
        // new Lua state with neither) and the add-on loading later.
        if (m_framesUntilInstall == 0)
        {
            m_framesUntilInstall = 45;
            if (Canvas().RunDebugScript(kInstallLua) && Canvas().RunDebugScript(kOnLua))
            {
                if (!m_luaOn)
                {
                    WOWVR_INFO("Floating text: combat text shown over the character in 3D.");
                }
                m_luaOn = true;
            }
        }
        else
        {
            --m_framesUntilInstall;
        }
        if (!m_luaOn)
        {
            return;
        }

        static char queue[4096];
        if (ReadQueue(queue, sizeof(queue)))
        {
            Canvas().RunDebugScript("WOWVR_FCT_OUT=nil");
            Take(queue);
        }
    }

    void FloatText::Take(const char* queue)
    {
        const char* p = queue;
        while (*p != 0)
        {
            const char* end = strchr(p, '\x1E');
            if (end == nullptr)
            {
                break;
            }
            // text \31 r \31 g \31 b \31 type
            const char* field[5] = {};
            size_t fieldLength[5] = {};
            const char* f = p;
            int n = 0;
            while (n < 5 && f <= end)
            {
                const char* stop = static_cast<const char*>(memchr(f, '\x1F', static_cast<size_t>(end - f)));
                if (stop == nullptr) { stop = end; }
                field[n] = f;
                fieldLength[n] = static_cast<size_t>(stop - f);
                ++n;
                f = stop + 1;
            }
            p = end + 1;
            if (n < 4)
            {
                continue;
            }

            // The text without the client's colour and texture escapes.
            char plain[192] = {};
            size_t out = 0;
            for (size_t i = 0; i < fieldLength[0] && out + 1 < sizeof(plain); ++i)
            {
                const char c = field[0][i];
                if (c == '|' && i + 1 < fieldLength[0])
                {
                    const char k = field[0][i + 1];
                    if (k == 'c' && i + 9 < fieldLength[0]) { i += 9; continue; }
                    if (k == 'r') { i += 1; continue; }
                    if (k == '|') { plain[out++] = '|'; i += 1; continue; }
                }
                plain[out++] = c;
            }
            plain[out] = 0;

            char number[32] = {};
            unsigned channel[3] = {};
            for (int k = 0; k < 3; ++k)
            {
                const size_t len = fieldLength[k + 1] < sizeof(number) - 1 ? fieldLength[k + 1]
                                                                          : sizeof(number) - 1;
                memcpy(number, field[k + 1], len);
                number[len] = 0;
                channel[k] = Channel(number);
            }
            const bool crit = n >= 5 && fieldLength[4] == 4 && memcmp(field[4], "crit", 4) == 0;

            wchar_t wide[96] = {};
            if (MultiByteToWideChar(CP_UTF8, 0, plain, -1, wide, 96) <= 0)
            {
                continue;
            }
            Add(wide, (channel[0] << 16) | (channel[1] << 8) | channel[2], crit);
            ++m_taken;
        }
    }

    void FloatText::Add(const wchar_t* text, unsigned rgb, bool crit)
    {
        if (text[0] == 0)
        {
            return;
        }
        if (m_count >= kMax)
        {
            // Full: the oldest makes room.
            if (m_messages[0].texture != nullptr)
            {
                m_messages[0].texture->Release();
            }
            for (int i = 1; i < m_count; ++i)
            {
                m_messages[i - 1] = m_messages[i];
                m_pending[i - 1] = m_pending[i];
            }
            --m_count;
        }
        // Messages arriving together start one under another, as the client's do, and
        // alternate sides.
        m_recentStack = m_sinceLast < 0.35f ? m_recentStack + 1 : 0;
        if (m_recentStack > 4) { m_recentStack = 0; }
        m_sinceLast = 0.0f;
        m_side = -m_side;

        FloatMessage& message = m_messages[m_count];
        message = FloatMessage();
        message.crit = crit;
        message.side = crit ? 0.0f : m_side;
        message.start = static_cast<float>(m_recentStack);
        Pending& pending = m_pending[m_count];
        wcsncpy_s(pending.text, text, _TRUNCATE);
        pending.rgb = rgb;
        ++m_count;
    }

    FloatMessage& FloatText::Message(int index, IDirect3DDevice9* device)
    {
        FloatMessage& message = m_messages[index];
        if (message.texture == nullptr && m_pending[index].text[0] != 0 && device != nullptr)
        {
            message.texture = BuildTexture(device, m_pending[index].text, m_pending[index].rgb,
                                           message.aspect);
            if (message.texture == nullptr)
            {
                m_pending[index].text[0] = 0;   // not tried again every frame
            }
        }
        return message;
    }

    bool FloatText::AnchorBody(float heightYards, Vec3& body) const
    {
        void* player = PlayerObject();
        if (player == nullptr)
        {
            return false;
        }
        float position[3] = {};
        __try
        {
            memcpy(position, static_cast<const uint8_t*>(player) + kUnitPosition, sizeof(position));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        const Vec3 world = { position[0], position[1], position[2] + heightYards };
        return Pointer().WorldToBodyNow(world, body);
    }

    void FloatText::ReleaseTextures()
    {
        for (int i = 0; i < m_count; ++i)
        {
            if (m_messages[i].texture != nullptr)
            {
                m_messages[i].texture->Release();
            }
        }
        m_count = 0;
    }

    FloatText& FloatTexts()
    {
        static FloatText instance;
        return instance;
    }
}
