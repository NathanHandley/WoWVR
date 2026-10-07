#include "ui/ui_panel.h"

#include "game/interface_resolution.h"
#include "game/ui_canvas.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <cmath>

#include <d3d9.h>

namespace wowvr
{
    namespace
    {
        const float kDegreesToRadians = 3.14159265359f / 180.0f;
    }

    bool UiPanel::Create(IDirect3DDevice9* device, uint32_t width, uint32_t height)
    {
        Destroy();

        if (device == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        m_width = width;
        m_height = height;

        // A8R8G8B8 because the panel has to composite over the world: the alpha
        // channel is what says which parts of the screen the interface actually
        // covers. The back buffer's X8R8G8B8 has no alpha to work with.
        const HRESULT hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                                 D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                 &m_texture, nullptr);
        if (FAILED(hr) || m_texture == nullptr)
        {
            WOWVR_ERROR("Could not create the %ux%u UI texture (0x%08lx).", width, height, hr);
            Destroy();
            return false;
        }

        if (FAILED(m_texture->GetSurfaceLevel(0, &m_surface)))
        {
            WOWVR_ERROR("Could not get the UI texture's surface.");
            Destroy();
            return false;
        }

        RefreshGeometry();
        WOWVR_INFO("UI panel target created at %ux%u: %.2f x %.2f m on a %.2f m radius "
                   "(%.0f degrees across).", width, height, m_shape.radius * m_shape.arc,
                   m_shape.heightMetres, m_shape.radius, m_shape.arc / kDegreesToRadians);
        return true;
    }

    void UiPanel::Destroy()
    {
        if (m_surface != nullptr)
        {
            m_surface->Release();
            m_surface = nullptr;
        }
        if (m_texture != nullptr)
        {
            m_texture->Release();
            m_texture = nullptr;
        }
        m_width = 0;
        m_height = 0;
        // The placement is deliberately kept: a device reset rebuilds the texture, and
        // the panel jumping back to the middle on every resolution change or alt-tab
        // would undo a placement the player chose.
    }

    void UiPanel::RecenterAt(float headYaw, const Vec3& headPositionBody)
    {
        m_shape.yaw = headYaw;
        m_shape.centre = headPositionBody;
    }

    void UiPanel::RefreshGeometry()
    {
        float radius = Cfg().panelDistance;
        if (!(radius > 0.2f)) { radius = 0.2f; }

        float pixelsPerDegree = Cfg().panelPixelsPerDegree;
        if (!(pixelsPerDegree > 1.0f)) { pixelsPerDegree = 1.0f; }

        float maxArcDegrees = Cfg().panelMaxArcDegrees;
        if (!(maxArcDegrees > 10.0f)) { maxArcDegrees = 10.0f; }
        if (maxArcDegrees > 300.0f) { maxArcDegrees = 300.0f; }

        const float widthPixels = (m_width > 0) ? static_cast<float>(m_width) : 1920.0f;
        const float heightPixels = (m_height > 0) ? static_cast<float>(m_height) : 1080.0f;

        // Measured in the real window's pixels: a game running bigger than its window
        // (InterfaceResolution) spends the extra pixels on sharpness, not on size.
        float arcDegrees = widthPixels / InterfacePixelScale() / pixelsPerDegree;
        if (arcDegrees > maxArcDegrees)
        {
            // Too wide for the cap: the whole panel shrinks, keeping its aspect, rather
            // than squashing the interface horizontally.
            arcDegrees = maxArcDegrees;
        }

        // The interface canvas: the same screen stretched over more angle, the
        // interface on it drawn smaller to match (game/ui_canvas.h). The cap above
        // applies to the ordinary-size area, so that keeps its exact shape.
        const float canvas = Canvas().PanelScale();
        arcDegrees *= canvas;
        if (arcDegrees > 330.0f) { arcDegrees = 330.0f; }

        m_shape.radius = radius;
        m_shape.arc = arcDegrees * kDegreesToRadians;
        m_shape.metresPerPixel = (m_shape.radius * m_shape.arc) / widthPixels;
        m_shape.heightMetres = heightPixels * m_shape.metresPerPixel;
    }

    Mat4 UiPanel::BodyToEye(const Mat4& headRotation, const Vec3& headOffsetMetres,
                            const Vec3& eyeOffsetMetres)
    {
        const Mat4 displace = Mat4Translation(-headOffsetMetres.x, -headOffsetMetres.y,
                                              -headOffsetMetres.z);
        const Mat4 viewToEye = Mat4Translation(-eyeOffsetMetres.x, -eyeOffsetMetres.y,
                                               -eyeOffsetMetres.z);
        return Mat4Multiply(Mat4Multiply(displace, headRotation), viewToEye);
    }

    Mat4 UiPanel::PanelToEye(const Mat4& headRotation, const Vec3& headOffsetMetres,
                             const Vec3& eyeOffsetMetres) const
    {
        // Body space into the current view, exactly the correction the world gets, so
        // the panel stays put in the room while the head moves around it.
        const Mat4 displace = Mat4Translation(-headOffsetMetres.x, -headOffsetMetres.y,
                                              -headOffsetMetres.z);
        const Mat4 bodyToView = Mat4Multiply(displace, headRotation);

        // Each eye sits slightly off the head's centre.
        const Mat4 viewToEye = Mat4Translation(-eyeOffsetMetres.x, -eyeOffsetMetres.y,
                                               -eyeOffsetMetres.z);

        return Mat4Multiply(Mat4Multiply(m_shape.PanelToBody(), bodyToView), viewToEye);
    }

    Mat4 PanelShape::PanelToBody() const
    {
        return Mat4Multiply(Mat4RotationY(yaw), Mat4Translation(centre.x, centre.y, centre.z));
    }

    Vec3 PanelShape::LocalPoint(float u, float v) const
    {
        // Mat4RotationY's convention: a positive angle swings +Z towards +X, so u = 1
        // (the right edge of the texture) is the positive end of the arc.
        const float angle = (u - 0.5f) * arc;
        Vec3 point;
        point.x = radius * std::sin(angle);
        point.y = (0.5f - v) * heightMetres;
        point.z = radius * std::cos(angle);
        return point;
    }

    Vec3 PanelShape::LocalTangentU(float u) const
    {
        const float angle = (u - 0.5f) * arc;
        Vec3 tangent;
        tangent.x = std::cos(angle);
        tangent.y = 0.0f;
        tangent.z = -std::sin(angle);
        return tangent;
    }

    Vec3 PanelShape::BodyPoint(float u, float v) const
    {
        return Mat4TransformPoint(LocalPoint(u, v), PanelToBody());
    }

    bool PanelShape::IntersectBody(const Vec3& originBody, const Vec3& directionBody,
                                   float& u, float& v) const
    {
        if (!(arc > 0.0f) || !(heightMetres > 0.0f))
        {
            return false;
        }

        // Into panel-local space: undo the placement (a rotation and a translation, so
        // the inverse is the transposed rotation after the negated translation).
        const Vec3 shifted = { originBody.x - centre.x, originBody.y - centre.y,
                               originBody.z - centre.z };
        const Mat4 unrotate = Mat4RotationY(-yaw);
        const Vec3 o = Mat4TransformDirection(shifted, unrotate);
        const Vec3 d = Mat4TransformDirection(directionBody, unrotate);

        // |(o + t d).xz| = R, solved for t.
        const float a = d.x * d.x + d.z * d.z;
        if (a < 1.0e-12f)
        {
            return false;   // straight up or down: parallel to the axis
        }
        const float b = 2.0f * (o.x * d.x + o.z * d.z);
        const float c = o.x * o.x + o.z * o.z - radius * radius;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f)
        {
            return false;
        }

        // The far root: from inside the cylinder - where the head always is unless it
        // has walked more than a radius away - it is the only one in front of the eye.
        const float t = (-b + std::sqrt(discriminant)) / (2.0f * a);
        if (!(t > 0.0f))
        {
            return false;
        }

        const float x = o.x + t * d.x;
        const float y = o.y + t * d.y;
        const float z = o.z + t * d.z;

        u = std::atan2(x, z) / arc + 0.5f;
        v = 0.5f - y / heightMetres;
        return true;
    }
}
