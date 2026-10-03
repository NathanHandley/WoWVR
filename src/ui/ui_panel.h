#pragma once

#include "core/math3d.h"

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

namespace wowvr
{
    // Where the panel is and what shape it has, with no device objects attached, so
    // it can be copied freely - the world-pointing hooks keep their own copy.
    //
    // A section of an upright cylinder. Panel-local space has the cylinder's axis on
    // Y through the origin and the panel's middle at (0, 0, radius); u runs left to
    // right round the arc, v top to bottom.
    struct PanelShape
    {
        float yaw = 0.0f;           // radians, Mat4RotationY convention
        Vec3 centre;                // body metres, on the cylinder's axis
        float radius = 1.6f;
        float arc = 1.2f;           // radians across u
        float heightMetres = 1.0f;
        float metresPerPixel = 0.001f;

        // Panel-local to body space, metres.
        Mat4 PanelToBody() const;

        // A point on the surface for u, v (0..1, v down), and the surface's unit
        // tangent along u, in panel-local space.
        Vec3 LocalPoint(float u, float v) const;
        Vec3 LocalTangentU(float u) const;

        // The body-space point for u, v.
        Vec3 BodyPoint(float u, float v) const;

        // Where a ray in BODY space meets the cylinder, as u, v. False when it misses
        // or would have to go backwards. u and v come back even outside 0..1, so a
        // caller can tell "off the panel" apart from "never got there".
        bool IntersectBody(const Vec3& originBody, const Vec3& directionBody,
                           float& u, float& v) const;
    };

    // The game's interface, lifted out of the eye buffer and hung in space.
    //
    // The UI is laid out for a flat screen at the client's own resolution, so it is
    // rendered once at that size into a texture of its own rather than being squeezed
    // into an eye viewport. It is then drawn in each eye, which also stops the
    // compositor reprojecting it as a flat depthless sheet - the cause of the swimming
    // that made it feel liquid.
    //
    // Shape: a section of an upright cylinder centred on where the head was when the
    // panel was last placed, so every part of it sits the same distance from the eyes.
    // Its size comes from the texture's pixel count at a fixed angular density
    // (PixelsPerDegree), which is what lets a higher game resolution buy more room
    // rather than smaller elements.
    //
    // World-locked: it does not follow the head. It moves only when it is placed again
    // (RecenterAt), which faces it the way the head is looking - yaw only, never pitch -
    // with its middle at eye height.
    class UiPanel
    {
    public:
        bool Create(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        void Destroy();

        bool IsReady() const { return m_surface != nullptr; }

        IDirect3DSurface9* Surface() const { return m_surface; }
        IDirect3DTexture9* Texture() const { return m_texture; }

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }

        // Places the panel straight ahead of a head at 'headPositionBody' (metres, body
        // frame) looking along 'headYaw' (radians, the same sign convention the panel
        // has always been handed: the negated ProjectionPatch::HeadYaw).
        void RecenterAt(float headYaw, const Vec3& headPositionBody);

        // Re-reads the size settings. Cheap; called once a frame so an INI reload
        // reshapes the panel without a restart.
        void RefreshGeometry();

        const PanelShape& Shape() const { return m_shape; }

        // Model-to-eye transform, in metres.
        //
        // Takes the full head correction rather than just the yaw. Compensating only
        // yaw leaves the panel welded to the head in pitch, roll and position, and the
        // compositor's reprojection then makes it swim about whenever the head moves.
        Mat4 PanelToEye(const Mat4& headRotation, const Vec3& headOffsetMetres,
                        const Vec3& eyeOffsetMetres) const;

    private:
        IDirect3DTexture9* m_texture = nullptr;
        IDirect3DSurface9* m_surface = nullptr;

        uint32_t m_width = 0;
        uint32_t m_height = 0;

        PanelShape m_shape;
    };
}
