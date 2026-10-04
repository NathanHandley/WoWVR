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
        // The tracking space is read once, here: the overlay is posed from the
        // presenter thread, which must not call into the compositor while the game
        // thread may be inside WaitGetPoses.
        m_trackingSpace = static_cast<int>(vr::VRCompositor()->GetTrackingSpace());
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

    HeadPoseStamp VrSession::CurrentPoseStamp() const
    {
        HeadPoseStamp stamp = {};
        stamp.valid = m_headPoseValid;
        if (stamp.valid)
        {
            const vr::HmdMatrix34_t& pose =
                g_poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
            memcpy(stamp.m, pose.m, sizeof(stamp.m));
        }
        return stamp;
    }

    bool VrSession::SubmitEye(int eye, void* texture, const HeadPoseStamp* renderPose)
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

        return SubmitTexture(eye, submission, nullptr, renderPose);
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
                                  const vr::VRTextureBounds_t* bounds,
                                  const HeadPoseStamp* renderPose)
    {
        // A stamped submission tells the compositor which pose the pixels were
        // rendered with, so it reprojects from THAT pose to the display pose. The
        // pipelined readback submits a frame-old image; unstamped, the compositor
        // assumes it belongs to the current pose and head movement jitters by one
        // frame's rotation.
        vr::VRTextureWithPose_t stamped;
        const vr::Texture_t* submission = &texture;
        vr::EVRSubmitFlags flags = vr::Submit_Default;
        if (renderPose != nullptr && renderPose->valid)
        {
            static_cast<vr::Texture_t&>(stamped) = texture;
            memcpy(stamped.mDeviceToAbsoluteTracking.m, renderPose->m,
                   sizeof(stamped.mDeviceToAbsoluteTracking.m));
            submission = &stamped;
            flags = vr::Submit_TextureWithPose;
        }

        const vr::EVRCompositorError error =
            vr::VRCompositor()->Submit(ToOpenVREye(eye), submission, bounds, flags);

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

    bool VrSession::ShowInterfaceOverlay(void* d3d11Texture, const float trackingToOverlay[3][4],
                                         float widthMetres, float curvature, bool premultiplied)
    {
        if (!m_active || d3d11Texture == nullptr || m_overlayCreateFailed)
        {
            return false;
        }
        vr::IVROverlay* overlay = vr::VROverlay();
        if (overlay == nullptr)
        {
            return false;
        }

        if (m_overlayHandle == vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
            const vr::EVROverlayError created =
                overlay->CreateOverlay("wowvr.interface", "WoWVR Interface", &handle);
            if (created != vr::VROverlayError_None)
            {
                m_overlayCreateFailed = true;
                WOWVR_ERROR("Could not create the interface overlay (%s); the interface stays "
                            "in the eye images.", overlay->GetOverlayErrorNameFromEnum(created));
                return false;
            }
            m_overlayHandle = handle;
            overlay->SetOverlayTexelAspect(handle, 1.0f);
            WOWVR_INFO("Interface overlay created.");
        }

        const vr::VROverlayHandle_t handle = m_overlayHandle;
        overlay->SetOverlayFlag(handle, vr::VROverlayFlags_IsPremultiplied, premultiplied);
        overlay->SetOverlayWidthInMeters(handle, widthMetres);
        overlay->SetOverlayCurvature(handle, curvature);

        vr::HmdMatrix34_t pose = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                pose.m[row][column] = trackingToOverlay[row][column];
            }
        }
        overlay->SetOverlayTransformAbsolute(handle, static_cast<vr::ETrackingUniverseOrigin>(m_trackingSpace),
                                             &pose);

        vr::Texture_t texture = {};
        texture.handle = d3d11Texture;
        texture.eType = vr::TextureType_DirectX;
        texture.eColorSpace = vr::ColorSpace_Gamma;
        const vr::EVROverlayError error = overlay->SetOverlayTexture(handle, &texture);
        if (static_cast<int>(error) != m_lastOverlayError)
        {
            m_lastOverlayError = static_cast<int>(error);
            if (error != vr::VROverlayError_None)
            {
                WOWVR_ERROR("Interface overlay texture update failed (%s).",
                            overlay->GetOverlayErrorNameFromEnum(error));
            }
        }

        if (!m_overlayVisible)
        {
            overlay->ShowOverlay(handle);
            m_overlayVisible = true;
            WOWVR_INFO("Interface overlay shown: %.2f m wide, curvature %.3f.", widthMetres,
                       curvature);
        }
        return error == vr::VROverlayError_None;
    }

    void VrSession::HideInterfaceOverlay()
    {
        if (!m_overlayVisible || m_overlayHandle == vr::k_ulOverlayHandleInvalid)
        {
            return;
        }
        if (vr::IVROverlay* overlay = vr::VROverlay())
        {
            overlay->HideOverlay(m_overlayHandle);
        }
        m_overlayVisible = false;
        WOWVR_INFO("Interface overlay hidden.");
    }

    void VrSession::PostSubmit()
    {
        if (!m_active)
        {
            return;
        }
        vr::VRCompositor()->PostPresentHandoff();
    }

    void VrSession::LogCompositorStats()
    {
        if (!m_active || vr::VRCompositor() == nullptr)
        {
            return;
        }

        static vr::Compositor_CumulativeStats previous = {};
        static bool havePrevious = false;
        vr::Compositor_CumulativeStats stats = {};
        vr::VRCompositor()->GetCumulativeStats(&stats, sizeof(stats));
        if (havePrevious && stats.m_nPid == previous.m_nPid)
        {
            WOWVR_INFO("Compositor since last report: %u submits, %u presents, %u dropped "
                       "(old frame shown, no reprojection), %u reprojected, %u timed out.",
                       stats.m_nNumFrameSubmits - previous.m_nNumFrameSubmits,
                       stats.m_nNumFramePresents - previous.m_nNumFramePresents,
                       stats.m_nNumDroppedFrames - previous.m_nNumDroppedFrames,
                       stats.m_nNumReprojectedFrames - previous.m_nNumReprojectedFrames,
                       stats.m_nNumFramePresentsTimedOut - previous.m_nNumFramePresentsTimedOut);
        }
        previous = stats;
        havePrevious = true;

        // The most recent frames, one record per compositor frame.
        static vr::Compositor_FrameTiming timings[128];
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, 128);
        if (count == 0)
        {
            return;
        }
        uint32_t motion = 0, async = 0, throttled = 0, predicted = 0, cpuReason = 0, gpuReason = 0;
        uint32_t misPresented = 0, dropped = 0, multiPresented = 0, longIntervals = 0;
        float maxInterval = 0.0f, sumInterval = 0.0f, maxSubmit = 0.0f, maxRenderGpu = 0.0f;
        const float frameMs = 1000.0f / (m_displayFrequency > 1.0f ? m_displayFrequency : 90.0f);
        for (uint32_t i = 0; i < count; ++i)
        {
            const vr::Compositor_FrameTiming& t = timings[i];
            const uint32_t flags = t.m_nReprojectionFlags;
            if (flags & vr::VRCompositor_ReprojectionMotion) { ++motion; }
            if (flags & vr::VRCompositor_ReprojectionAsync) { ++async; }
            if (flags & vr::VRCompositor_ReprojectionReason_Cpu) { ++cpuReason; }
            if (flags & vr::VRCompositor_ReprojectionReason_Gpu) { ++gpuReason; }
            if (flags & vr::VRCompositor_ThrottleMask) { ++throttled; }
            if (flags & vr::VRCompositor_PredictionMask) { ++predicted; }
            misPresented += t.m_nNumMisPresented;
            dropped += t.m_nNumDroppedFrames;
            if (t.m_nNumFramePresents > 1) { ++multiPresented; }
            sumInterval += t.m_flClientFrameIntervalMs;
            if (t.m_flClientFrameIntervalMs > maxInterval) { maxInterval = t.m_flClientFrameIntervalMs; }
            if (t.m_flClientFrameIntervalMs > frameMs * 2.5f) { ++longIntervals; }
            if (t.m_flSubmitFrameMs > maxSubmit) { maxSubmit = t.m_flSubmitFrameMs; }
            if (t.m_flTotalRenderGpuMs > maxRenderGpu) { maxRenderGpu = t.m_flTotalRenderGpuMs; }
        }
        WOWVR_INFO("Compositor last %u frames (%.0f Hz): app interval avg %.2f max %.2f ms, %u "
                   "longer than 2.5 vsyncs; shown more than once %u, mispresented %u, dropped %u; "
                   "reprojection: motion smoothing %u, async %u, cpu-reason %u, gpu-reason %u, "
                   "throttled %u, extra prediction %u; Submit max %.2f ms; total render gpu max "
                   "%.2f ms; last flags 0x%X.",
                   count, 1000.0f / frameMs, sumInterval / count, maxInterval, longIntervals,
                   multiPresented, misPresented, dropped, motion, async, cpuReason, gpuReason,
                   throttled, predicted, maxSubmit, maxRenderGpu,
                   timings[count - 1].m_nReprojectionFlags);
    }

    VrSession& Vr()
    {
        return g_session;
    }
}
