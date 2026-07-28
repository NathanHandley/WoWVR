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
        // Settle last frame's votes before starting a new count. The camera is the
        // position the most draws agreed on; a lone vote is one model's placement and
        // says nothing, so a clear majority is required before it is believed.
        int best = -1;
        int bestCount = 0;
        int total = 0;
        for (int i = 0; i < m_positionVoteCount; ++i)
        {
            total += m_positionVotes[i].count;
            if (m_positionVotes[i].count > bestCount)
            {
                bestCount = m_positionVotes[i].count;
                best = i;
            }
        }
        if (best >= 0 && bestCount >= 4 && bestCount * 2 > total)
        {
            m_cameraPosition = m_positionVotes[best].position;
            m_haveCameraPosition = true;
        }

        int bestYaw = -1;
        int bestYawCount = 0;
        for (int i = 0; i < m_yawVoteCount; ++i)
        {
            if (m_yawVotes[i].count > bestYawCount)
            {
                bestYawCount = m_yawVotes[i].count;
                bestYaw = i;
            }
        }
        if (bestYaw >= 0 && bestYawCount >= 8)
        {
            m_cameraYaw = m_yawVotes[bestYaw].yaw;
            m_cameraYawWeight = bestYawCount;
            m_haveCameraYaw = true;
        }
        m_yawVoteCount = 0;

        // Kept so the tally can be reported: when the winner is not the camera, what
        // else was in the running is the thing worth seeing.
        m_lastVoteCount = m_positionVoteCount;
        for (int i = 0; i < m_positionVoteCount; ++i)
        {
            m_lastVotes[i] = m_positionVotes[i];
        }
        m_positionVoteCount = 0;

        const Mat4 headRotation = RotationOnly(headToStage);

        // Synthetic head displacement, in metres, added to the POSE for the same reason the
        // synthetic rotation is: so it travels the identical path a real headset's position
        // takes - into the neutral frame, through the left-handed flip - rather than being
        // spliced in downstream where a broken consumer would still look fine.
        Vec3 headPosition = Mat4TranslationOf(headToStage);
        headPosition.x += m_fakeHeadOffset.x;
        headPosition.y += m_fakeHeadOffset.y;
        headPosition.z += m_fakeHeadOffset.z;

        if (m_recenterRequested)
        {
            // Neutral keeps only the head's yaw. The stage frame is gravity-aligned,
            // so pitch and roll have an absolute reference; folding them into the
            // neutral would bake a tilted head into the origin and tilt the horizon
            // for the rest of the session. Yaw is the only axis with no absolute
            // reference, so it is the only axis recentring resets.
            m_neutralInverse = Mat4RotationY(-Mat4YawOf(headRotation));
            m_neutralPosition = headPosition;
            m_recenterRequested = false;
            WOWVR_INFO("Head tracking recentred (yaw only, horizon stays level).");
        }

        // Rotation of the head relative to neutral.
        Mat4 relative = Mat4Multiply(headRotation, m_neutralInverse);

        // A synthetic head turn, injected into the POSE rather than into the values derived
        // from it.
        //
        // It used to be added further down, to m_headRotation and m_headYaw together. That
        // made the two agree by construction, which is exactly the bug it needed to be able
        // to expose: the real path extracts the scalar and builds the matrix separately, and
        // they disagreed in sign for yaw. Every synthetic sweep passed while a real head
        // turn steered the game camera the wrong way.
        //
        // It also made pitch tests vacuous. m_headPitch was assigned from the pose a few
        // lines after the fake was added to it, so the fake was overwritten and pitch aiming
        // never ran at all - while the captures looked like a clean result.
        //
        // Injected here, every value downstream comes from one source through the same code
        // a headset goes through, so a sign error has nowhere left to hide.
        if (m_fakeHeadYaw != 0.0f)
        {
            // About the stage's vertical, which is what turning on the spot does.
            relative = Mat4Multiply(Mat4RotationY(m_fakeHeadYaw), relative);
        }
        if (m_fakeHeadPitch != 0.0f)
        {
            // About the head's own right axis, which is what nodding does.
            relative = Mat4Multiply(Mat4RotationX(m_fakeHeadPitch), relative);
        }

        // Inverted: rotating the head one way has to rotate the world the other way.
        m_headRotation = ToLeftHanded(TransposeRotation(relative));

        // NOT negated.
        //
        // It was, with a note about converting from OpenVR's right-handed frame to the
        // game's left-handed one - but that conversion is already inside the line above.
        // ToLeftHanded negates [2][0] and TransposeRotation swaps it with [0][2], so for a
        // pure yaw theta the matrix that gets used carries +theta, and a scalar of -theta
        // contradicts it.
        //
        // The contradiction was survivable as long as nothing else read the scalar: the
        // compensation removed -theta from a matrix holding +theta, double-rotating by
        // +2*theta, which cancelled against a game camera turned the wrong way by -theta and
        // displayed a correct view. Two errors making a right picture. The moment the scalar
        // started steering the client's culling, the wrong one showed.
        // Pitch and roll say whether the yaw being extracted means anything: a headset
        // rotating about an axis that is not its own vertical sweeps through both as it
        // turns, and its "yaw" then stops corresponding to the direction it is facing.
        const float sinPitch = -relative.m[2][1] < -1.0f ? -1.0f
                             : (-relative.m[2][1] > 1.0f ? 1.0f : -relative.m[2][1]);
        m_headPitch = asinf(sinPitch);
        m_headRoll = atan2f(relative.m[0][1], relative.m[1][1]);

        // Yaw is held near vertical, because there it does not exist.
        //
        // The extraction is atan2(cosPitch * sinYaw, cosPitch * cosYaw). The cosines cancel
        // and it returns the yaw exactly - right up until cosPitch reaches zero, where both
        // arguments are zero and the result is whatever the noise in the pose says. Measured
        // looking straight down: asking for yaws of 0.0, 0.5, 1.0 and 2.0 rad produced 62,
        // 78, 76 and 67 degrees, which is not a relationship.
        //
        // Feeding that to the camera made the client chase a value jumping tens of degrees a
        // frame; it smooths, so it lagged, so the compensation removed a rotation the client
        // had not finished applying and the ground swam with the head.
        //
        // Looking straight down there IS no facing to speak of - turning your head there is
        // a roll, not a turn - so holding the last well-conditioned yaw is not an
        // approximation, it is the right answer. Hysteresis so the boundary cannot chatter.
        const float cosPitch = sqrtf(1.0f - sinPitch * sinPitch);
        const float kResume = 0.26f;    // |pitch| < 75 degrees: trustworthy
        const float kFreeze = 0.17f;    // |pitch| > 80 degrees: noise

        if (cosPitch > kResume) { m_yawIsStable = true; }
        else if (cosPitch < kFreeze) { m_yawIsStable = false; }

        if (m_yawIsStable || !m_haveHeldYaw)
        {
            m_headYaw = Mat4YawOf(relative);
            m_haveHeldYaw = true;
        }

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

    // The head rotation with whatever the game's own camera has already been turned by taken
    // back out of it.
    //
    // The client bakes its view transform into the vertices on the CPU, so by the time
    // geometry reaches us the camera's rotation has already happened to it. Applying the
    // head rotation in full on top would turn the world twice as far as the head moved.
    //
    // ORDER MATTERS, and only when both angles are non-zero.
    //
    // WoW is Z-up and builds its camera the way any first-person camera is built: yaw about
    // the world's vertical, then pitch about the right axis that yaw produced. So the
    // rotation baked into the geometry is R_turn = R_yaw * R_pitch, and undoing it needs
    // R_turn^-1 = R_pitch^-1 * R_yaw^-1 - the yaw unwound first, then the pitch.
    //
    // This used to unwind pitch first, which computes (R_pitch * R_yaw)^-1 instead. With
    // either angle at zero the two are identical, which is exactly why sweeping yaw and
    // pitch separately - as every test here did - could never tell them apart. Combine them
    // and the difference is a residual ROLL that grows with both angles: look up while
    // turned, and the horizon tilts.
    //
    // It cancels because m_headRotation decomposes the same way, as R_yaw * R_pitch, so
    // R_pitch^-1 * R_yaw^-1 * R_yaw * R_pitch is the identity.
    // Putting the camera back where it was before it swung round the character.
    //
    // In third person the camera does not pivot in place - it rides a sphere of radius R
    // centred on the character, so aiming it at the head MOVES it. The old correction handled
    // only yaw, which is why looking up flew the camera into the sky and looking down buried
    // it in the ground: the vertical half of the swing was never accounted for at all.
    //
    // Derived generally, in the view space of the turned camera. The character sits at
    // (0, 0, R) in front of it. The camera used to sit at that centre minus R times the OLD
    // forward direction, and the old forward expressed in the new camera's frame is
    // R_turn^-1 applied to z. So the old eye position was R * (R_turn^-1 . z) - (0, 0, R),
    // and the geometry has to be shifted by the negative of that to look as though the
    // camera never left.
    //
    // The sign is switchable because the previous attempt was, by its own comment, derived
    // rather than measured - and it was inverted, which doubles the swing instead of
    // cancelling it. Measured this time.
    void ProjectionPatch::ApplyOrbitCompensation(float& offsetX, float& offsetY,
                                                 float& offsetZ) const
    {
        if (m_gameCameraOrbitRadius <= 0.0f
            || (m_gameCameraYaw == 0.0f && m_gameCameraPitch == 0.0f))
        {
            return;
        }

        // Measured by the caller from the camera's own position and basis; see
        // GameCamera::OrbitDisplacementView.

        // SUBTRACTED, and in the same units it arrives in.
        //
        // offsetX/Y/Z say where the eye is relative to the game's camera, in world units, on
        // the D3D view axes - right, up, forward. m_orbitDisplacement says how far the
        // camera MOVED from where it would have been, on those same axes, already in world
        // units because it came from a radius in yards. So the eye belongs at minus that,
        // and there is no metres-to-yards conversion to apply and no world scale: scaling it
        // would move the viewpoint by an amount the camera did not move by.
        //
        // The per-axis signs are gone. They existed because the measurement they were
        // correcting was wrong twice over - a left vector read as a right vector, and an
        // un-aimed direction reconstructed on the wrong side of the real one - and while the
        // camera was also being hauled about by collision, no test could tell any of it
        // apart. With collision off and both errors fixed the correction is a subtraction
        // with nothing left to choose. The single overall sign stays as a field switch.
        const float scale = m_orbitSign;

        offsetX -= m_orbitDisplacement.x * scale;
        offsetY -= m_orbitDisplacement.y * scale;
        offsetZ -= m_orbitDisplacement.z * scale;
    }

    Mat4 ProjectionPatch::HeadCorrection() const
    {
        Mat4 corrected = m_headRotation;

        if (m_unwindPitchFirst)
        {
            // The old, wrong order. Kept only so the two can be measured against each
            // other in one session rather than across two builds.
            if (m_gameCameraPitch != 0.0f)
            {
                corrected = Mat4Multiply(Mat4RotationX(-m_gameCameraPitch), corrected);
            }
            if (m_gameCameraYaw != 0.0f)
            {
                corrected = Mat4Multiply(Mat4RotationY(-m_gameCameraYaw), corrected);
            }
            return corrected;
        }

        if (m_gameCameraYaw != 0.0f)
        {
            corrected = Mat4Multiply(Mat4RotationY(-m_gameCameraYaw), corrected);
        }
        if (m_gameCameraPitch != 0.0f)
        {
            corrected = Mat4Multiply(Mat4RotationX(-m_gameCameraPitch), corrected);
        }
        return corrected;
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
        m_sceneMatrix = decoded.projection;
        m_haveSceneMatrix = true;
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

                if (Cfg().symmetricEyeProjection)
                {
                    const float wide = (fabsf(tanLeft) > fabsf(tanRight))
                        ? fabsf(tanLeft) : fabsf(tanRight);
                    const float tall = (fabsf(tanTop) > fabsf(tanBottom))
                        ? fabsf(tanTop) : fabsf(tanBottom);
                    tanLeft = -wide; tanRight = wide;
                    tanTop = -tall;  tanBottom = tall;
                }

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

            float offsetX = (m_headOffset.x + eyeOffset.x) * unitsPerMetre;
            float offsetY = (m_headOffset.y + eyeOffset.y) * unitsPerMetre;
            float offsetZ = (m_headOffset.z - eyeOffset.z) * unitsPerMetre;

            // This path never had the orbit correction at all - the same one-of-two-copies
            // gap that left the sky uncompensated.
            ApplyOrbitCompensation(offsetX, offsetY, offsetZ);

            if (Cfg().eyeProjectionPassThrough)
            {
                // Straight back out again, through every other step of the path.
                const Mat4 unchanged = decoded.wasTransposed
                    ? Mat4Transpose(decoded.projection) : decoded.projection;
                MatrixTo(unchanged, eye == EyeLeft ? outLeft : outRight);
                continue;
            }

            // HeadCorrection(), never m_headRotation.
            //
            // This path substitutes the projection register, which is what the sky dome and
            // the distant backdrop terrain are drawn through; the near world goes through
            // the combined transform and BuildEyeProjection. Only that one compensated for
            // the rotation already applied to the game's own camera, so once the camera
            // started following the head, the two paths disagreed: the near world sat still
            // and the sky and far terrain swung with every head movement.
            //
            // Sharing one helper is the point. Two copies of a correction is what let them
            // drift apart in the first place.
            const Mat4 headRotation = HeadCorrection();

            if (m_infiniteDistance)
            {
                // Rotation only. Translating the sky is what makes it feel like a
                // painted wall a few metres away instead of a horizon.
                if (Cfg().headTracking)
                {
                    replacement = Mat4Multiply(headRotation, replacement);
                }
            }
            else if (Cfg().headTracking)
            {
                // A point at P in the old view frame sits at (P - d) * R in the new
                // one, so the correction ahead of the projection is T(-d) then R.
                const Mat4 translation = Mat4Translation(-offsetX, -offsetY, -offsetZ);
                const Mat4 correction = Mat4Multiply(translation, headRotation);
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

    Mat4 ProjectionPatch::BuildEyeProjection(int eye, float nearPlane, float farPlane,
                                             const Mat4& original) const
    {
        Mat4 replacement = original;

        if (Vr().IsActive())
        {
            float tanLeft = 0.0f;
            float tanRight = 0.0f;
            float tanTop = 0.0f;
            float tanBottom = 0.0f;
            Vr().EyeTangents(eye, tanLeft, tanRight, tanTop, tanBottom);

                if (Cfg().symmetricEyeProjection)
                {
                    const float wide = (fabsf(tanLeft) > fabsf(tanRight))
                        ? fabsf(tanLeft) : fabsf(tanRight);
                    const float tall = (fabsf(tanTop) > fabsf(tanBottom))
                        ? fabsf(tanTop) : fabsf(tanBottom);
                    tanLeft = -wide; tanRight = wide;
                    tanTop = -tall;  tanBottom = tall;
                }

            replacement = Mat4PerspectiveTangents(tanLeft, tanRight, tanTop, tanBottom,
                                                  nearPlane, farPlane);

            // Depth terms carried over verbatim; see the note in TryPatch.
            replacement.m[2][2] = original.m[2][2];
            replacement.m[3][2] = original.m[3][2];
            replacement.m[2][3] = original.m[2][3];
            replacement.m[3][3] = original.m[3][3];
        }

        const float unitsPerMetre = Cfg().unitsPerMetre * Cfg().worldScale;
        const Vec3 eyeOffset = Mat4TranslationOf(Vr().EyeToHead(eye));

        // Not const: the orbit correction is a view-space shift like these, so it folds in
        // here rather than being applied separately. Y is no longer exempt - the vertical
        // half of the swing is exactly what was missing.
        float offsetX = (m_headOffset.x + eyeOffset.x) * unitsPerMetre;
        float offsetY = (m_headOffset.y + eyeOffset.y) * unitsPerMetre;
        float offsetZ = (m_headOffset.z - eyeOffset.z) * unitsPerMetre;

        ApplyOrbitCompensation(offsetX, offsetY, offsetZ);

        const Mat4 headRotation = HeadCorrection();

        if (m_infiniteDistance)
        {
            return Cfg().headTracking ? Mat4Multiply(headRotation, replacement) : replacement;
        }

        const Mat4 translation = Mat4Translation(-offsetX, -offsetY, -offsetZ);
        const Mat4 correction = Cfg().headTracking
            ? Mat4Multiply(translation, headRotation)
            : translation;

        return Mat4Multiply(correction, replacement);
    }

    namespace
    {
        // A world placement scales and rotates, it does not shear. Requiring the three
        // axes to be mutually perpendicular and the same length rejects the arrays of
        // bone and lighting constants that pass a bare affine test by chance.
        bool Mat4IsNearIdentity(const Mat4& m, float tolerance)
        {
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    const float expected = (row == col) ? 1.0f : 0.0f;
                    if (fabsf(m.m[row][col] - expected) > tolerance)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool AllFiniteMatrix(const Mat4& m);
        bool Mat4IsNearIdentity(const Mat4& m, float tolerance);

        // A light's view-projection is orthographic: no perspective column.
        bool LooksLikeLightMatrix(const Mat4& m)
        {
            if (!AllFiniteMatrix(m))
            {
                return false;
            }
            const bool orthographic = fabsf(m.m[3][3] - 1.0f) < 1.0e-3f
                                   && fabsf(m.m[0][3]) < 1.0e-3f
                                   && fabsf(m.m[1][3]) < 1.0e-3f
                                   && fabsf(m.m[2][3]) < 1.0e-3f;
            if (!orthographic)
            {
                return false;
            }
            // An identity product means the constant was just the plain inverse, which
            // carries no light transform.
            return !Mat4IsNearIdentity(m, 1.0e-3f);
        }

        bool AllFiniteMatrix(const Mat4& m)
        {
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    const float v = m.m[r][c];
                    if (!(v > -1.0e30f && v < 1.0e30f)) { return false; }
                }
            }
            return true;
        }

        bool LooksLikeRigidPlacement(const Mat4& m)
        {
            float length[3];
            for (int row = 0; row < 3; ++row)
            {
                length[row] = sqrtf(m.m[row][0] * m.m[row][0]
                                    + m.m[row][1] * m.m[row][1]
                                    + m.m[row][2] * m.m[row][2]);
                if (length[row] < 1.0e-3f || length[row] > 1.0e4f)
                {
                    return false;
                }
            }

            if (fabsf(length[0] - length[1]) > 0.02f * length[0]
                || fabsf(length[0] - length[2]) > 0.02f * length[0])
            {
                return false;
            }

            for (int a = 0; a < 3; ++a)
            {
                const int b = (a + 1) % 3;
                const float dot = m.m[a][0] * m.m[b][0] + m.m[a][1] * m.m[b][1]
                                + m.m[a][2] * m.m[b][2];
                if (fabsf(dot) > 0.02f * length[a] * length[b])
                {
                    return false;
                }
            }
            return true;
        }
    }

    namespace
    {
        // Row-vector D3D: a perspective projection puts z into w, so m[2][3] is 1 and
        // m[3][3] is 0. An orthographic one is the other way round. That distinction is
        // what separates world geometry from the interface, and it does not care about
        // aspect ratio or field of view.
        bool LooksPerspective(const Mat4& m)
        {
            return fabsf(m.m[3][3]) < 1.0e-3f && fabsf(m.m[2][3]) > 0.5f;
        }
    }

    bool ProjectionPatch::PatchAnyPerspective(const float* uploaded,
                                              float* outLeft, float* outRight)
    {
        if (!Vr().IsActive())
        {
            return false;
        }

        Mat4 projection = MatrixFrom(uploaded);
        bool wasTransposed = false;

        if (!LooksPerspective(projection))
        {
            const Mat4 transposed = Mat4Transpose(projection);
            if (!LooksPerspective(transposed))
            {
                return false;
            }
            projection = transposed;
            wasTransposed = true;
        }

        // The near and far arguments are immaterial: BuildEyeProjection copies the depth
        // terms straight out of the matrix it is given, so only x and y are rebuilt.
        for (int eye = 0; eye < EyeCount; ++eye)
        {
            const Mat4 result = BuildEyeProjection(eye, 1.0f, 1000.0f, projection);
            MatrixTo(wasTransposed ? Mat4Transpose(result) : result,
                     eye == EyeLeft ? outLeft : outRight);
        }

        return true;
    }

    bool ProjectionPatch::TryPatchCombinedStrict(const float* uploaded,
                                                 float* outLeft, float* outRight)
    {
        m_requireRigidResidual = true;
        const bool ok = TryPatchCombined(uploaded, outLeft, outRight);
        m_requireRigidResidual = false;
        return ok;
    }

    // A world-to-view matrix places the camera at the origin, so the camera's world
    // position is the point the matrix sends there: undo the translation, then undo the
    // rotation. The rotation is orthonormal for a genuine view matrix, so its transpose
    // is its inverse and no general solve is needed.
    void ProjectionPatch::NoteWorldView(const Mat4& worldView)
    {
        ++m_residualsSeen;

        // Position turned out to be unrecoverable - the client hands the GPU geometry
        // that is already camera-relative, so every residual translation is zero. The
        // rotation survives that, though: with the translation gone, what is left for
        // world-space geometry is exactly the world-to-view rotation, which is the
        // camera's own orientation. Models that had a placement baked in contribute
        // their own rotation instead, so the same agreement counting applies - the
        // orientation the most draws share is the camera's.
        {
            const float yaw = Mat4YawOf(worldView);
            bool counted = false;
            for (int i = 0; i < m_yawVoteCount && !counted; ++i)
            {
                if (fabsf(WrapRadians(m_yawVotes[i].yaw - yaw)) < 0.01f)
                {
                    ++m_yawVotes[i].count;
                    counted = true;
                }
            }
            if (!counted && m_yawVoteCount < kPositionVotes)
            {
                m_yawVotes[m_yawVoteCount].yaw = yaw;
                m_yawVotes[m_yawVoteCount].count = 1;
                ++m_yawVoteCount;
            }
        }

        // The camera register itself divides out to the identity, and an identity
        // residual puts the camera at the world origin - which is what the first run of
        // this swamped the vote with. Those uploads carry no camera information, so they
        // are not entitled to a vote.
        //
        // Rigidity is deliberately NOT required. A first attempt insisted on it and
        // rejected every one of seven million residuals, which says the matrices
        // reaching this path carry scale - model placements rather than the plain view.
        const float translationLength =
            fabsf(worldView.m[3][0]) + fabsf(worldView.m[3][1]) + fabsf(worldView.m[3][2]);
        if (translationLength < 1.0f)
        {
            ++m_residualsNearOrigin;
            return;
        }
        if (LooksLikeRigidPlacement(worldView))
        {
            ++m_residualsRigid;
        }

        Vec3 position;
        position.x = -(worldView.m[3][0] * worldView.m[0][0]
                     + worldView.m[3][1] * worldView.m[0][1]
                     + worldView.m[3][2] * worldView.m[0][2]);
        position.y = -(worldView.m[3][0] * worldView.m[1][0]
                     + worldView.m[3][1] * worldView.m[1][1]
                     + worldView.m[3][2] * worldView.m[1][2]);
        position.z = -(worldView.m[3][0] * worldView.m[2][0]
                     + worldView.m[3][1] * worldView.m[2][1]
                     + worldView.m[3][2] * worldView.m[2][2]);

        for (int i = 0; i < m_positionVoteCount; ++i)
        {
            const Vec3& seen = m_positionVotes[i].position;
            if (fabsf(seen.x - position.x) < 0.05f
                && fabsf(seen.y - position.y) < 0.05f
                && fabsf(seen.z - position.z) < 0.05f)
            {
                ++m_positionVotes[i].count;
                return;
            }
        }

        if (m_positionVoteCount < kPositionVotes)
        {
            m_positionVotes[m_positionVoteCount].position = position;
            m_positionVotes[m_positionVoteCount].count = 1;
            ++m_positionVoteCount;
        }
    }

    bool ProjectionPatch::CameraWorldPosition(Vec3& out) const
    {
        if (!m_haveCameraPosition)
        {
            return false;
        }
        out = m_cameraPosition;
        return true;
    }

    bool ProjectionPatch::TryPatchCombined(const float* uploaded, float* outLeft, float* outRight)
    {
        ++m_combinedTried;

        if (!m_haveSceneMatrix || !Vr().IsActive())
        {
            ++m_combinedNoScene;
            return false;
        }

        Mat4 inverseScene;
        if (!Mat4Inverse(m_sceneMatrix, inverseScene))
        {
            return false;
        }

        // The upload may be either way round; whichever orientation leaves an affine
        // residual is the right reading.
        const Mat4 asUploaded = MatrixFrom(uploaded);
        const Mat4 transposed = Mat4Transpose(asUploaded);

        Mat4 combined;
        bool wasTransposed = false;

        if (Mat4IsAffine(Mat4Multiply(asUploaded, inverseScene), 1.0e-3f))
        {
            combined = asUploaded;
        }
        else if (Mat4IsAffine(Mat4Multiply(transposed, inverseScene), 1.0e-3f))
        {
            combined = transposed;
            wasTransposed = true;
        }
        else
        {
            ++m_combinedNotAffine;
            return false;
        }

        // Everything the game baked in ahead of the projection: world, view, and any
        // per-object placement. It is kept exactly as-is.
        const Mat4 worldView = Mat4Multiply(combined, inverseScene);
        NoteWorldView(worldView);

        // NOTE: rejecting an identity residual here (which is what the camera register
        // itself produces, since P * inverse(P) = I) was tried and is NOT correct as a
        // blanket rule: it stopped every shader from resolving at all, including the
        // water's genuine combined transform at c0. It also did not fix the flat-green
        // model corruption, which persists with zero shaders resolved and therefore has
        // another cause entirely.

        if (m_requireRigidResidual && !LooksLikeRigidPlacement(worldView))
        {
            ++m_combinedNotAffine;
            return false;
        }

        for (int eye = 0; eye < EyeCount; ++eye)
        {
            const Mat4 eyeProjection = BuildEyeProjection(eye, m_sceneNear, m_sceneFar,
                                                          m_sceneMatrix);
            const Mat4 result = Mat4Multiply(worldView, eyeProjection);
            MatrixTo(wasTransposed ? Mat4Transpose(result) : result,
                     eye == EyeLeft ? outLeft : outRight);
        }

        ++m_patched;
        ++m_combinedPatched;
        return true;
    }

    bool ProjectionPatch::TryPatchInverseDerived(const float* uploaded,
                                                 float* outLeft, float* outRight)
    {
        if (!m_haveSceneMatrix || !Vr().IsActive())
        {
            return false;
        }

        // The upload may be either way round; whichever orientation makes scene * W an
        // orthographic light matrix is the right reading.
        const Mat4 asUploaded = MatrixFrom(uploaded);
        const Mat4 transposed = Mat4Transpose(asUploaded);

        Mat4 candidate;
        Mat4 light;
        bool wasTransposed = false;

        light = Mat4Multiply(m_sceneMatrix, asUploaded);
        if (LooksLikeLightMatrix(light))
        {
            candidate = asUploaded;
        }
        else
        {
            light = Mat4Multiply(m_sceneMatrix, transposed);
            if (!LooksLikeLightMatrix(light))
            {
                return false;
            }
            candidate = transposed;
            wasTransposed = true;
        }

        (void)candidate;

        for (int eye = 0; eye < EyeCount; ++eye)
        {
            // Built against exactly what will be written into the projection register
            // for this eye, since that is what the shader reconstructs from.
            const Mat4 substituted =
                BuildEyeProjection(eye, m_sceneNear, m_sceneFar, m_sceneMatrix);

            Mat4 inverseEye;
            if (!Mat4Inverse(substituted, inverseEye))
            {
                return false;
            }

            const Mat4 rebuilt = Mat4Multiply(inverseEye, light);
            MatrixTo(wasTransposed ? Mat4Transpose(rebuilt) : rebuilt,
                     eye == EyeLeft ? outLeft : outRight);
        }

        ++m_inverseDerivedPatched;
        return true;
    }

    void ProjectionPatch::LogLastDecision() const
    {
        WOWVR_INFO("Inverse-derived constants rebuilt: %llu", m_inverseDerivedPatched);

        WOWVR_INFO("Combined transform path: %llu offered, %llu rewritten, %llu skipped for "
                   "no scene camera yet, %llu rejected as not affine",
                   m_combinedTried, m_combinedPatched, m_combinedNoScene, m_combinedNotAffine);

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
