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
    // The walk starts from the camera's groups AND the character's (a yard above the
    // feet): the three loads of the eye in that function read a position WoWVR sets, and
    // the one call to it (0x00783381) goes through a wrapper that runs it from the
    // character, then from the camera (so everything it sets is stock), then appends the
    // character's groups to the camera's lists with the client's own 0x00792FC0. Only
    // this decision moves: the view is still rendered from the camera, and every other
    // reader of the eye still reads the camera.
    class InteriorView
    {
    public:
        // Checks and patches the client's code once; false (logged) if it is not the
        // build this was written against.
        bool Install();

        // Once a frame from the render thread: where the decision is made from.
        // Within cameraReachYards of the character the camera's surroundings are drawn as
        // well as the character's; beyond it, only the character's.
        void Update(bool fromCharacter, float cameraReachYards);

        bool Installed() const { return m_installed; }

    private:
        bool m_installed = false;
        bool m_failed = false;
        bool m_lastFromCharacter = false;
    };

    InteriorView& Interior();
}
