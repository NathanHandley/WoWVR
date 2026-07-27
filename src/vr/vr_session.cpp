#include "vr/vr_session.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <string>

#include <openvr.h>

namespace wowvr
{
    namespace
    {
        VrSession g_session;
        vr::IVRSystem* g_system = nullptr;
        vr::TrackedDevicePose_t g_poses[vr::k_unMaxTrackedDeviceCount];

        vr::EVREye ToOpenVREye(int eye)
        {
            return eye == EyeRight ? vr::Eye_Right : vr::Eye_Left;
        }

        // openvr_api.dll is delay-loaded so that a missing or broken runtime cannot
        // stop the game from starting. Touching it for the first time therefore has
        // to be guarded against the delay-load exception.
        bool TryInitOpenVR(vr::EVRInitError& error)
        {
            __try
            {
                g_system = vr::VR_Init(&error, vr::VRApplication_Scene);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                error = vr::VRInitError_Init_NotInitialized;
                return false;
            }
        }

        const char* CompositorErrorText(vr::EVRCompositorError error)
        {
            switch (error)
            {
            case vr::VRCompositorError_None:                        return "none";
            case vr::VRCompositorError_RequestFailed:               return "request failed";
            case vr::VRCompositorError_IncompatibleVersion:         return "incompatible OpenVR version";
            case vr::VRCompositorError_DoNotHaveFocus:              return "another application holds scene focus";
            case vr::VRCompositorError_InvalidTexture:              return "invalid texture";
            case vr::VRCompositorError_IsNotSceneApplication:       return "not registered as a scene application";
            case vr::VRCompositorError_TextureIsOnWrongDevice:      return "texture is on the wrong GPU";
            case vr::VRCompositorError_TextureUsesUnsupportedFormat: return "unsupported texture format";
            case vr::VRCompositorError_SharedTexturesNotSupported:  return "shared textures unsupported";
            case vr::VRCompositorError_IndexOutOfRange:             return "eye index out of range";
            case vr::VRCompositorError_AlreadySubmitted:            return "eye already submitted this frame";
            case vr::VRCompositorError_InvalidBounds:               return "invalid texture bounds";
            default:                                                return "unknown";
            }
        }

        const char* InitErrorText(vr::EVRInitError error)
        {
            __try
            {
                return vr::VR_GetVRInitErrorAsEnglishDescription(error);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return "openvr_api.dll could not be loaded";
            }
        }

        void LogHeadsetIdentity()
        {
            if (g_system == nullptr)
            {
                return;
            }

            char buffer[vr::k_unMaxPropertyStringSize];
            vr::ETrackedPropertyError propertyError = vr::TrackedProp_Success;

            g_system->GetStringTrackedDeviceProperty(
                vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String,
                buffer, sizeof(buffer), &propertyError);
            const std::string trackingSystem = (propertyError == vr::TrackedProp_Success) ? buffer : "?";

            g_system->GetStringTrackedDeviceProperty(
                vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String,
                buffer, sizeof(buffer), &propertyError);
            const std::string model = (propertyError == vr::TrackedProp_Success) ? buffer : "?";

            WOWVR_INFO("Headset: %s / %s", trackingSystem.c_str(), model.c_str());
        }
    }

    bool VrSession::Init()
    {
        if (m_active)
        {
            return true;
        }

        m_headToStage = Mat4Identity();
        m_eyeToHead[EyeLeft] = Mat4Identity();
        m_eyeToHead[EyeRight] = Mat4Identity();

        vr::EVRInitError error = vr::VRInitError_None;
        if (!TryInitOpenVR(error) || g_system == nullptr || error != vr::VRInitError_None)
        {
            WOWVR_WARN("OpenVR unavailable (%s). Continuing without VR output.", InitErrorText(error));
            g_system = nullptr;
            return false;
        }

        if (vr::VRCompositor() == nullptr)
        {
            WOWVR_WARN("OpenVR compositor unavailable. Continuing without VR output.");
            vr::VR_Shutdown();
            g_system = nullptr;
            return false;
        }

        uint32_t width = 0;
        uint32_t height = 0;
        g_system->GetRecommendedRenderTargetSize(&width, &height);

        const float scale = Cfg().renderScale;
        m_renderWidth = static_cast<uint32_t>(static_cast<float>(width) * scale);
        m_renderHeight = static_cast<uint32_t>(static_cast<float>(height) * scale);

        // Keep the dimensions even; some drivers dislike odd render target sizes.
        m_renderWidth &= ~1u;
        m_renderHeight &= ~1u;

        vr::ETrackedPropertyError propertyError = vr::TrackedProp_Success;
        const float frequency = g_system->GetFloatTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propertyError);
        if (propertyError == vr::TrackedProp_Success && frequency > 1.0f)
        {
            m_displayFrequency = frequency;
        }

        int32_t adapterIndex = -1;
        g_system->GetDXGIOutputInfo(&adapterIndex);
        m_adapterIndex = adapterIndex;

        for (int eye = 0; eye < EyeCount; ++eye)
        {
            const vr::HmdMatrix34_t eyeToHead = g_system->GetEyeToHeadTransform(ToOpenVREye(eye));
            m_eyeToHead[eye] = Mat4FromOpenVR(eyeToHead.m);

            g_system->GetProjectionRaw(ToOpenVREye(eye),
                                       &m_tangents[eye][0], &m_tangents[eye][1],
                                       &m_tangents[eye][2], &m_tangents[eye][3]);
        }

        LogHeadsetIdentity();
        WOWVR_INFO("OpenVR ready: %ux%u per eye at %.1f Hz, DXGI adapter %d",
                   m_renderWidth, m_renderHeight, m_displayFrequency, m_adapterIndex);
        WOWVR_INFO("Eye separation from runtime: %.4f m",
                   m_eyeToHead[EyeRight].m[3][0] - m_eyeToHead[EyeLeft].m[3][0]);

        // Logged rather than assumed: OpenVR's naming of top/bottom does not say
        // which one is numerically larger, and the sign convention decides whether
        // the vertical frustum offset helps or mirrors.
        for (int eye = 0; eye < EyeCount; ++eye)
        {
            WOWVR_INFO("  %s eye tangents: left %.4f right %.4f top %.4f bottom %.4f "
                       "(width %.4f, height %.4f, hcentre %.4f, vcentre %.4f)",
                       eye == EyeLeft ? "left " : "right",
                       m_tangents[eye][0], m_tangents[eye][1], m_tangents[eye][2], m_tangents[eye][3],
                       m_tangents[eye][1] - m_tangents[eye][0],
                       m_tangents[eye][3] - m_tangents[eye][2],
                       (m_tangents[eye][0] + m_tangents[eye][1]) * 0.5f,
                       (m_tangents[eye][2] + m_tangents[eye][3]) * 0.5f);
        }

        m_active = true;

        // The compositor hands out scene focus only once an application starts
        // waiting on it, and a Submit before that fails with DoNotHaveFocus. Priming
        // it here means the very first frame we push already has focus.
        WaitForFrame();

        return true;
    }

    void VrSession::Shutdown()
    {
        if (!m_active)
        {
            return;
        }

        WOWVR_INFO("Shutting down the OpenVR session.");
        vr::VR_Shutdown();
        g_system = nullptr;
        m_active = false;
    }

    void VrSession::WaitForFrame()
    {
        if (!m_active)
        {
            return;
        }

        vr::VRCompositor()->WaitGetPoses(g_poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);

        const vr::TrackedDevicePose_t& head = g_poses[vr::k_unTrackedDeviceIndex_Hmd];
        m_headPoseValid = head.bPoseIsValid && head.eTrackingResult == vr::TrackingResult_Running_OK;
        if (m_headPoseValid)
        {
            m_headToStage = Mat4FromOpenVR(head.mDeviceToAbsoluteTracking.m);
        }
    }

    bool VrSession::UserIsPresent() const
    {
        if (!m_active || g_system == nullptr)
        {
            return false;
        }

        const vr::EDeviceActivityLevel level =
            g_system->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
        return level == vr::k_EDeviceActivityLevel_UserInteraction
            || level == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
    }

    void VrSession::EyeTangents(int eye, float& left, float& right, float& top, float& bottom) const
    {
        left = m_tangents[eye][0];
        right = m_tangents[eye][1];
        top = m_tangents[eye][2];
        bottom = m_tangents[eye][3];
    }

    bool VrSession::SubmitEye(int eye, void* texture)
    {
        if (!m_active || texture == nullptr)
        {
            return false;
        }

        vr::Texture_t submission = {};
        submission.handle = texture;
        submission.eType = vr::TextureType_DirectX;
        // WoW writes plain gamma-space colour; telling the compositor otherwise
        // washes the whole image out.
        submission.eColorSpace = vr::ColorSpace_Gamma;

        return SubmitTexture(eye, submission, nullptr);
    }

    bool VrSession::SubmitEyeGl(int eye, uint32_t glTexture)
    {
        if (!m_active || glTexture == 0)
        {
            return false;
        }

        vr::Texture_t submission = {};
        submission.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(glTexture));
        submission.eType = vr::TextureType_OpenGL;
        submission.eColorSpace = vr::ColorSpace_Gamma;

        // GL addresses textures bottom-up, but the D3D9 image aliased into this
        // texture is top-down. Flipping the bounds keeps the world upright.
        vr::VRTextureBounds_t bounds;
        bounds.uMin = 0.0f;
        bounds.uMax = 1.0f;
        bounds.vMin = 1.0f;
        bounds.vMax = 0.0f;

        return SubmitTexture(eye, submission, &bounds);
    }

    bool VrSession::SubmitEyeD3D12(int eye, void* textureData)
    {
        if (!m_active || textureData == nullptr)
        {
            return false;
        }

        vr::Texture_t submission = {};
        submission.handle = textureData;
        submission.eType = vr::TextureType_DirectX12;
        submission.eColorSpace = vr::ColorSpace_Gamma;

        return SubmitTexture(eye, submission, nullptr);
    }

    bool VrSession::SubmitTexture(int eye, const vr::Texture_t& texture,
                                  const vr::VRTextureBounds_t* bounds)
    {
        const vr::EVRCompositorError error =
            vr::VRCompositor()->Submit(ToOpenVREye(eye), &texture, bounds);

        if (static_cast<int>(error) != m_lastSubmitError)
        {
            if (error == vr::VRCompositorError_None)
            {
                WOWVR_INFO("Compositor submit recovered after %llu failed frames.", m_submitErrorCount);
                m_submitErrorCount = 0;
            }
            else
            {
                WOWVR_ERROR("IVRCompositor::Submit failed for eye %d: %s (%d).",
                            eye, CompositorErrorText(error), static_cast<int>(error));
            }
            m_lastSubmitError = static_cast<int>(error);
        }

        if (error != vr::VRCompositorError_None)
        {
            ++m_submitErrorCount;
            return false;
        }

        return true;
    }

    void VrSession::PostSubmit()
    {
        if (!m_active)
        {
            return;
        }
        vr::VRCompositor()->PostPresentHandoff();
    }

    VrSession& Vr()
    {
        return g_session;
    }
}
