#include "stereo/projection_patch.h"

#include "core/config.h"
#include "core/log.h"
#include "vr/vr_session.h"

#include <cmath>
#include <cstring>

namespace wowvr
{
    namespace
    {
        ProjectionPatch g_patch;

        constexpr float kAspectTolerance = 0.03f;

        // Reads a 4x4 out of 16 consecutive floats.
        Mat4 MatrixFrom(const float* values)
        {
            Mat4 result;
            std::memcpy(&result.m[0][0], values, sizeof(float) * 16);
            return result;
        }

        void MatrixTo(const Mat4& source, float* values)
        {
            std::memcpy(values, &source.m[0][0], sizeof(float) * 16);
        }

        // A row-vector D3D perspective projection has m[2][3] == 1 and m[3][3] == 0.
        bool IsRowVectorPerspective(const Mat4& m)
        {
            return std::fabs(m.m[2][3] - 1.0f) < 0.01f
                && std::fabs(m.m[3][3]) < 0.01f
                && m.m[0][0] > 0.001f
                && m.m[1][1] > 0.001f;
        }

        // OpenVR is right-handed with -Z forward; WoW's view space is left-handed with
        // +Z forward. Conjugating by a Z flip converts a rotation between the two.
        Mat4 ToLeftHanded(const Mat4& rightHanded)
        {
            Mat4 result = rightHanded;
            result.m[0][2] = -rightHanded.m[0][2];
            result.m[1][2] = -rightHanded.m[1][2];
            result.m[2][0] = -rightHanded.m[2][0];
            result.m[2][1] = -rightHanded.m[2][1];
            result.m[3][2] = -rightHanded.m[3][2];
            return result;
        }

        Mat4 RotationOnly(const Mat4& source)
        {
            Mat4 result = Mat4Identity();
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    result.m[row][column] = source.m[row][column];
                }
            }
            return result;
        }

        Mat4 TransposeRotation(const Mat4& rotation)
        {
            Mat4 result = Mat4Identity();
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    result.m[row][column] = rotation.m[column][row];
                }
            }
            return result;
        }
    }

    void ProjectionPatch::SetSceneAspect(float aspect)
    {
        if (aspect > 0.01f)
        {
            m_sceneAspect = aspect;
        }
    }

    void ProjectionPatch::Recenter()
    {
        m_recenterRequested = true;
    }

    void ProjectionPatch::UpdateFromHeadPose(const Mat4& headToStage)
    {
        const Mat4 headRotation = RotationOnly(headToStage);
        const Vec3 headPosition = Mat4TranslationOf(headToStage);

        if (m_recenterRequested)
        {
            // Neutral is wherever the head was, and whichever way it pointed, when
            // recentre was asked for, so the game's own camera stays the origin.
            m_neutralInverse = TransposeRotation(headRotation);
            m_neutralPosition = headPosition;
            m_recenterRequested = false;
            WOWVR_INFO("Head tracking recentred.");
        }

        // Rotation of the head relative to neutral, then inverted: rotating the head
        // one way has to rotate the world the other way.
        const Mat4 relative = Mat4Multiply(headRotation, m_neutralInverse);
        m_headRotation = ToLeftHanded(TransposeRotation(relative));

        // Negated because the yaw is measured in OpenVR's right-handed frame and
        // consumed in the game's left-handed one.
        m_headYaw = -Mat4YawOf(relative);

        // Displacement since recentring, rotated out of stage space into the frame the
        // game's camera is looking along, then flipped to left-handed. This is what
        // makes leaning and stepping sideways actually move the viewpoint.
        const float dx = headPosition.x - m_neutralPosition.x;
        const float dy = headPosition.y - m_neutralPosition.y;
        const float dz = headPosition.z - m_neutralPosition.z;

        m_headOffset.x = dx * m_neutralInverse.m[0][0] + dy * m_neutralInverse.m[1][0] + dz * m_neutralInverse.m[2][0];
        m_headOffset.y = dx * m_neutralInverse.m[0][1] + dy * m_neutralInverse.m[1][1] + dz * m_neutralInverse.m[2][1];
        m_headOffset.z = -(dx * m_neutralInverse.m[0][2] + dy * m_neutralInverse.m[1][2] + dz * m_neutralInverse.m[2][2]);
    }

    bool ProjectionPatch::Decode(const float* uploaded, Decoded& out) const
    {
        const Mat4 asUploaded = MatrixFrom(uploaded);

        if (IsRowVectorPerspective(asUploaded))
        {
            out.projection = asUploaded;
            out.wasTransposed = false;
        }
        else
        {
            const Mat4 transposed = Mat4Transpose(asUploaded);
            if (!IsRowVectorPerspective(transposed))
            {
                return false;
            }
            out.projection = transposed;
            out.wasTransposed = true;
        }

        out.horizontalScale = out.projection.m[0][0];
        out.verticalScale = out.projection.m[1][1];

        // Q = far / (far - near) and m[3][2] = -near * Q.
        const float q = out.projection.m[2][2];
        if (q <= 0.0f || std::fabs(q - 1.0f) < 1e-6f)
        {
            out.nearPlane = 0.2f;
            out.farPlane = 1000.0f;
        }
        else
        {
            out.nearPlane = -out.projection.m[3][2] / q;
            out.farPlane = q * out.nearPlane / (q - 1.0f);
        }

        return true;
    }

    bool ProjectionPatch::TryPatch(const float* uploaded, float* outLeft, float* outRight)
    {
        Decoded decoded;
        if (!Decode(uploaded, decoded))
        {
            ++m_rejected;
            return false;
        }

        // The scene camera renders at the back buffer's aspect. The shadow-map pass
        // is square and the post-process passes have their own shapes, so this is
        // what keeps the patch off everything that is not the player's view.
        const float aspect = decoded.verticalScale / decoded.horizontalScale;
        m_lastAspect = aspect;
        m_lastNear = decoded.nearPlane;
        m_lastFar = decoded.farPlane;

        if (std::fabs(aspect - m_sceneAspect) > kAspectTolerance * m_sceneAspect)
        {
            // A perspective projection that is not the scene camera. Worth recording:
            // if a pass has its own camera we need to know before deciding whether to
            // leave it alone.
            bool known = false;
            for (int i = 0; i < m_rejectedProjectionCount; ++i)
            {
                RejectedProjection& entry = m_rejectedProjections[i];
                if (std::fabs(entry.aspect - aspect) < 0.01f
                    && std::fabs(entry.nearPlane - decoded.nearPlane) < 0.01f)
                {
                    ++entry.count;
                    known = true;
                    break;
                }
            }
            if (!known && m_rejectedProjectionCount < kMaxRejected)
            {
                RejectedProjection& entry = m_rejectedProjections[m_rejectedProjectionCount++];
                entry.aspect = aspect;
                entry.nearPlane = decoded.nearPlane;
                entry.farPlane = decoded.farPlane;
                entry.count = 1;
            }

            ++m_rejected;
            return false;
        }

        // Accepted: this is the player's camera, so remember what it is made of.
        m_sceneNear = decoded.nearPlane;
        m_sceneFar = decoded.farPlane;
        m_sceneVerticalScale = decoded.verticalScale;

        if (Cfg().useGameProjection)
        {
            // Both eyes get the game's matrix verbatim. Stereo separation is lost, but
            // everything else about the pipeline is unchanged.
            MatrixTo(MatrixFrom(uploaded), outLeft);
            MatrixTo(MatrixFrom(uploaded), outRight);
            ++m_patched;
            return true;
        }

        const float unitsPerMetre = Cfg().unitsPerMetre * Cfg().worldScale;

        for (int eye = 0; eye < EyeCount; ++eye)
        {
            Mat4 replacement = decoded.projection;

            if (Vr().IsActive())
            {
                // Per-eye frustum straight from the headset, keeping the game's own
                // depth range so fog, sky and depth precision behave as the client
                // expects. On an Index these two frusta are canted about 14 degrees
                // apart, which is why one image cannot serve both eyes.
                float tanLeft = 0.0f;
                float tanRight = 0.0f;
                float tanTop = 0.0f;
                float tanBottom = 0.0f;
                Vr().EyeTangents(eye, tanLeft, tanRight, tanTop, tanBottom);

                replacement = Mat4PerspectiveTangents(tanLeft, tanRight, tanTop, tanBottom,
                                                      decoded.nearPlane, decoded.farPlane);

                // Take the depth terms straight from the matrix being replaced rather
                // than rebuilding them from a decoded near and far.
                //
                // Depth in D3D is Q - n*Q/z: it depends only on view-space z and these
                // two terms, never on the field of view. So the eye frustum can change
                // x and y freely, but the moment these are recomputed, any error in the
                // decode silently rewrites the depth range. That is what put the sky in
                // front of the world: a sky dome's far plane is enormous, which makes Q
                // round to almost exactly 1 and trip the decode's fallback.
                replacement.m[2][2] = decoded.projection.m[2][2];
                replacement.m[3][2] = decoded.projection.m[3][2];
                replacement.m[2][3] = decoded.projection.m[2][3];
                replacement.m[3][3] = decoded.projection.m[3][3];
            }

            // The eye's own offset from the head, which is what actually produces
            // stereo separation. Combined with the head displacement since recentring.
            const Vec3 eyeOffset = Mat4TranslationOf(Vr().EyeToHead(eye));

            const float offsetX = (m_headOffset.x + eyeOffset.x) * unitsPerMetre;
            const float offsetY = (m_headOffset.y + eyeOffset.y) * unitsPerMetre;
            const float offsetZ = (m_headOffset.z - eyeOffset.z) * unitsPerMetre;

            if (Cfg().headTracking)
            {
                // A point at P in the old view frame sits at (P - d) * R in the new
                // one, so the correction ahead of the projection is T(-d) then R.
                const Mat4 translation = Mat4Translation(-offsetX, -offsetY, -offsetZ);
                const Mat4 correction = Mat4Multiply(translation, m_headRotation);
                replacement = Mat4Multiply(correction, replacement);
            }
            else
            {
                const Mat4 translation = Mat4Translation(-offsetX, -offsetY, -offsetZ);
                replacement = Mat4Multiply(translation, replacement);
            }

            const Mat4 finalMatrix = decoded.wasTransposed ? Mat4Transpose(replacement) : replacement;
            MatrixTo(finalMatrix, eye == EyeLeft ? outLeft : outRight);
        }

        ++m_patched;
        return true;
    }

    void ProjectionPatch::LogLastDecision() const
    {
        WOWVR_INFO("Projection patch: %llu patched, %llu left alone. Last seen aspect %.4f "
                   "(scene aspect %.4f), near %.3f, far %.1f",
                   m_patched, m_rejected, m_lastAspect, m_sceneAspect, m_lastNear, m_lastFar);

        for (int i = 0; i < m_rejectedProjectionCount; ++i)
        {
            const RejectedProjection& entry = m_rejectedProjections[i];
            WOWVR_INFO("  other perspective camera seen %llu times: aspect %.4f, near %.3f, far %.1f",
                       entry.count, entry.aspect, entry.nearPlane, entry.farPlane);
        }
    }

    ProjectionPatch& Projection()
    {
        return g_patch;
    }
}
