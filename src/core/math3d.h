#pragma once

namespace wowvr
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Row-vector 4x4 matrix, matching Direct3D's convention: a point is transformed
    // as v' = v * M, and m[3] is the translation row. OpenVR uses the opposite
    // (column-vector) convention, so anything crossing that boundary is transposed
    // by Mat4FromOpenVR.
    struct Mat4
    {
        float m[4][4] = {};
    };

    Mat4 Mat4Identity();
    Mat4 Mat4Multiply(const Mat4& a, const Mat4& b);   // a then b
    Mat4 Mat4Translation(float x, float y, float z);
    Mat4 Mat4RotationX(float radians);
    Mat4 Mat4RotationY(float radians);

    // Yaw of a rotation matrix about the Y axis, in radians.
    float Mat4YawOf(const Mat4& a);

    // Wraps an angle into [-pi, pi]. Body-locking compares angles across the wrap
    // point, where a naive subtraction spins the panel the long way round.
    float WrapRadians(float radians);
    Mat4 Mat4Transpose(const Mat4& a);

    // General inverse. Returns false and leaves 'out' untouched for a singular matrix.
    bool Mat4Inverse(const Mat4& a, Mat4& out);

    // Builds an off-centre perspective projection from the half-angle tangents that
    // OpenVR reports for an eye. The tangents are signed, with left and top negative.
    Mat4 Mat4PerspectiveTangents(float tanLeft, float tanRight, float tanTop, float tanBottom,
                                 float nearPlane, float farPlane);

    // Converts an OpenVR 3x4 pose (column-vector, row-major) into our convention.
    Mat4 Mat4FromOpenVR(const float source[3][4]);

    Vec3 Mat4TranslationOf(const Mat4& a);

    // v * M for a point (w = 1) and for a direction (w = 0), row-vector convention.
    Vec3 Mat4TransformPoint(const Vec3& v, const Mat4& a);
    Vec3 Mat4TransformDirection(const Vec3& v, const Mat4& a);

    // True when the bottom column is (0,0,0,1), i.e. the matrix is affine rather than
    // projective. Used in the stereo path to confirm that a candidate world matrix
    // really is one.
    bool Mat4IsAffine(const Mat4& a, float tolerance = 1.0e-4f);
}
