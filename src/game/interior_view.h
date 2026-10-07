#pragma once

namespace wowvr
{
    // Decides "is the camera inside a building" from the CHARACTER's position instead of
    // the camera's (Cfg().interiorFromCharacter).
    //
    // The client draws a building's interior only when its camera is inside it: each
    // world update, 0x00795D40 drops a ray from the eye position (0x00CD8F5C) and the
    // building group that ray lands in becomes the start of the portal walk (0x00CD87A4,
    // groups at 0x00CDB0D4). With camera collision off, zooming out puts the camera in
    // the hillside above a cave, the ray finds no group, and the whole interior - and the
    // character with it - is culled away.
    //
    // The three loads of the eye in that one function are pointed at a position WoWVR
    // keeps at the character, a yard above the feet. Only this decision moves: the view
    // is still rendered from the camera, and every other reader of the eye still reads
    // the camera.
    class InteriorView
    {
    public:
        // Checks and patches the client's code once; false (logged) if it is not the
        // build this was written against.
        bool Install();

        // Once a frame from the render thread: where the decision is made from.
        void Update(bool fromCharacter);

        bool Installed() const { return m_installed; }

    private:
        bool m_installed = false;
        bool m_failed = false;
        bool m_lastFromCharacter = false;
    };

    InteriorView& Interior();
}
