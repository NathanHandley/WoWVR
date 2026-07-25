#pragma once

#include "core/math3d.h"

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

namespace wowvr
{
    // The game's interface, lifted out of the eye buffer and hung in space.
    //
    // The UI is laid out for a flat screen at the client's own resolution, so it is
    // rendered once at that size into a texture of its own rather than being squeezed
    // into an eye viewport. It is then drawn as a quad in each eye, which also stops
    // the compositor reprojecting it as a flat depthless sheet - the cause of the
    // swimming that made it feel liquid.
    //
    // Body-locked: the panel holds still while you glance around and only follows
    // once your head passes a dead zone, so you can look past it at the world without
    // losing track of the cursor.
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

        // Advances the body-lock. 'headYaw' is the head's yaw relative to the
        // recentred origin, in radians.
        void Update(float headYaw, float deltaSeconds);

        // Puts the panel straight ahead again.
        void Recenter(float headYaw);

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

        float m_bodyYaw = 0.0f;
        bool m_initialised = false;
    };
}
