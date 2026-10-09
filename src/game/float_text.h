#pragma once

#include "core/math3d.h"

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wowvr
{
    // Floating combat text in the world. The client's own (Blizzard_CombatText: "+350"
    // as a heal lands, "-120" as a hit does, "<Regrowth>" as a buff arrives) scrolls up
    // the middle of the interface, which in a headset is a sheet some way off and not
    // where the character is. While this is on, every message that add-on would show is
    // taken from it as it is added, its own copy is hidden, and the same text, in the
    // same colour, rises and fades over the character instead.
    //
    // Taking the messages: a Lua hook on CombatText_AddMessage appends each one to the
    // global WOWVR_FCT_OUT, which is read here once a frame through the client's own
    // Lua API (lua_getfield / lua_tolstring - nothing is registered with the client)
    // and cleared. Both run on the client's main thread, which is the one that draws.
    struct FloatMessage
    {
        IDirect3DTexture9* texture = nullptr;
        float aspect = 1.0f;        // texture width / height
        float age = 0.0f;           // seconds since it arrived
        float side = 0.0f;          // across, in text heights (alternates like the client's)
        float start = 0.0f;         // down from the anchor, in text heights (stacking)
        bool crit = false;          // pops larger and stays put, as the client's do
    };

    class FloatText
    {
    public:
        // Once a frame on the main thread. Installs the Lua side while in the world (and
        // again after a /reload), takes new messages, ages the old ones. 'enabled' false
        // puts the client's own text back and drops everything here.
        void Update(bool enabled, float seconds);

        // The point the text rises from - the character's position plus 'heightYards' -
        // in body metres, for this frame. False if it cannot be had (not in the world,
        // pointing inactive).
        bool AnchorBody(float heightYards, Vec3& body) const;

        int Count() const { return m_count; }
        // Builds the message's texture on first use (on 'device').
        FloatMessage& Message(int index, IDirect3DDevice9* device);

        // Textures belong to a device: dropped with it.
        void ReleaseTextures();

        // Seconds a message lives.
        static constexpr float kLifetime = 2.2f;

    private:
        void Take(const char* queue);
        void Add(const wchar_t* text, unsigned rgb, bool crit);

        static const int kMax = 24;
        struct Pending
        {
            wchar_t text[96] = {};
            unsigned rgb = 0xFFFFFF;
        };
        FloatMessage m_messages[kMax];
        Pending m_pending[kMax];
        int m_count = 0;
        float m_side = 1.0f;
        float m_sinceLast = 10.0f;
        int m_recentStack = 0;
        bool m_luaOn = false;
        unsigned m_framesUntilInstall = 0;
        unsigned long long m_taken = 0;
    };

    FloatText& FloatTexts();

    // A text's texture (its colour, a dark outline, straight alpha), built once and kept
    // for reuse - the same numbers come up again and again. 'rgb' 0xRRGGBB. Null if it
    // could not be built. Released with FloatText::ReleaseTextures.
    IDirect3DTexture9* CachedTextTexture(IDirect3DDevice9* device, const char* utf8, unsigned rgb,
                                         float& aspect);

    // A chat bubble's texture: the text wrapped in a dark rounded box with a light
    // border and a tail at the bottom middle, built once per text and colour.
    // 'lineFraction' is one line of text as a fraction of the texture's height, for
    // sizing it in the world. Released with FloatText::ReleaseTextures.
    IDirect3DTexture9* CachedBubbleTexture(IDirect3DDevice9* device, const char* utf8, unsigned rgb,
                                           float& aspect, float& lineFraction);
}
