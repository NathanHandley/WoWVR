#include "ui/ui_panel.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <cmath>

#include <d3d9.h>

namespace wowvr
{
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

        WOWVR_INFO("UI panel target created at %ux%u.", width, height);
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
        m_initialised = false;
    }

    void UiPanel::Recenter(float headYaw)
    {
        m_bodyYaw = headYaw;
        m_initialised = true;
    }

    void UiPanel::Update(float headYaw, float deltaSeconds)
    {
        if (!m_initialised)
        {
            Recenter(headYaw);
            return;
        }

        const float deadzone = Cfg().panelDeadzoneDegrees * 3.14159265359f / 180.0f;
        const float offset = WrapRadians(headYaw - m_bodyYaw);

        if (std::fabs(offset) <= deadzone)
        {
            return;
        }

        // Outside the dead zone the panel is dragged along, but only far enough to sit
        // at the edge of it, so it trails the head rather than snapping to it.
        const float target = headYaw - ((offset > 0.0f) ? deadzone : -deadzone);

        float follow = Cfg().panelFollowSpeed * deltaSeconds;
        if (follow > 1.0f) { follow = 1.0f; }
        if (follow < 0.0f) { follow = 0.0f; }

        m_bodyYaw = WrapRadians(m_bodyYaw + WrapRadians(target - m_bodyYaw) * follow);
    }

    Mat4 UiPanel::PanelToEye(const Mat4& headRotation, const Vec3& headOffsetMetres,
                             const Vec3& eyeOffsetMetres) const
    {
        // Where the panel sits in body space: pushed out to the viewing distance, then
        // swung round to the direction the body is facing.
        const Mat4 outward = Mat4Translation(0.0f, 0.0f, Cfg().panelDistance);
        const Mat4 panelToBody = Mat4Multiply(outward, Mat4RotationY(m_bodyYaw));

        // Body space into the current view, exactly the correction the world gets, so
        // the panel stays put in the room while the head moves around it.
        const Mat4 displace = Mat4Translation(-headOffsetMetres.x, -headOffsetMetres.y,
                                              -headOffsetMetres.z);
        const Mat4 bodyToView = Mat4Multiply(displace, headRotation);

        // Each eye sits slightly off the head's centre.
        const Mat4 viewToEye = Mat4Translation(-eyeOffsetMetres.x, -eyeOffsetMetres.y,
                                               -eyeOffsetMetres.z);

        return Mat4Multiply(Mat4Multiply(panelToBody, bodyToView), viewToEye);
    }
}
