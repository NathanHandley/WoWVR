#include "core/math3d.h"

#include <cmath>

namespace wowvr
{
    Mat4 Mat4Identity()
    {
        Mat4 result;
        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][3] = 1.0f;
        return result;
    }

    Mat4 Mat4Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 result;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                result.m[row][column] =
                    a.m[row][0] * b.m[0][column] +
                    a.m[row][1] * b.m[1][column] +
                    a.m[row][2] * b.m[2][column] +
                    a.m[row][3] * b.m[3][column];
            }
        }
        return result;
    }

    Mat4 Mat4Translation(float x, float y, float z)
    {
        Mat4 result = Mat4Identity();
        result.m[3][0] = x;
        result.m[3][1] = y;
        result.m[3][2] = z;
        return result;
    }

    // Same convention as the Y rotation beside it: left-handed, row-vector, so that
    // undoing the game camera's pitch composes correctly with undoing its yaw.
    Mat4 Mat4RotationX(float radians)
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);

        Mat4 result = Mat4Identity();
        result.m[1][1] = c;
        result.m[1][2] = s;
        result.m[2][1] = -s;
        result.m[2][2] = c;
        return result;
    }

    Mat4 Mat4RotationY(float radians)
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);

        Mat4 result = Mat4Identity();
        result.m[0][0] = c;
        result.m[0][2] = -s;
        result.m[2][0] = s;
        result.m[2][2] = c;
        return result;
    }

    float Mat4YawOf(const Mat4& a)
    {
        return std::atan2(a.m[2][0], a.m[2][2]);
    }

    float WrapRadians(float radians)
    {
        const float twoPi = 6.28318530718f;
        while (radians > 3.14159265359f)  { radians -= twoPi; }
        while (radians < -3.14159265359f) { radians += twoPi; }
        return radians;
    }

    Mat4 Mat4Transpose(const Mat4& a)
    {
        Mat4 result;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                result.m[row][column] = a.m[column][row];
            }
        }
        return result;
    }

    bool Mat4Inverse(const Mat4& a, Mat4& out)
    {
        const float* s = &a.m[0][0];
        float inverse[16];

        inverse[0]  =  s[5]*s[10]*s[15] - s[5]*s[11]*s[14] - s[9]*s[6]*s[15] + s[9]*s[7]*s[14] + s[13]*s[6]*s[11] - s[13]*s[7]*s[10];
        inverse[4]  = -s[4]*s[10]*s[15] + s[4]*s[11]*s[14] + s[8]*s[6]*s[15] - s[8]*s[7]*s[14] - s[12]*s[6]*s[11] + s[12]*s[7]*s[10];
        inverse[8]  =  s[4]*s[9]*s[15]  - s[4]*s[11]*s[13] - s[8]*s[5]*s[15] + s[8]*s[7]*s[13] + s[12]*s[5]*s[11] - s[12]*s[7]*s[9];
        inverse[12] = -s[4]*s[9]*s[14]  + s[4]*s[10]*s[13] + s[8]*s[5]*s[14] - s[8]*s[6]*s[13] - s[12]*s[5]*s[10] + s[12]*s[6]*s[9];

        inverse[1]  = -s[1]*s[10]*s[15] + s[1]*s[11]*s[14] + s[9]*s[2]*s[15] - s[9]*s[3]*s[14] - s[13]*s[2]*s[11] + s[13]*s[3]*s[10];
        inverse[5]  =  s[0]*s[10]*s[15] - s[0]*s[11]*s[14] - s[8]*s[2]*s[15] + s[8]*s[3]*s[14] + s[12]*s[2]*s[11] - s[12]*s[3]*s[10];
        inverse[9]  = -s[0]*s[9]*s[15]  + s[0]*s[11]*s[13] + s[8]*s[1]*s[15] - s[8]*s[3]*s[13] - s[12]*s[1]*s[11] + s[12]*s[3]*s[9];
        inverse[13] =  s[0]*s[9]*s[14]  - s[0]*s[10]*s[13] - s[8]*s[1]*s[14] + s[8]*s[2]*s[13] + s[12]*s[1]*s[10] - s[12]*s[2]*s[9];

        inverse[2]  =  s[1]*s[6]*s[15]  - s[1]*s[7]*s[14]  - s[5]*s[2]*s[15] + s[5]*s[3]*s[14] + s[13]*s[2]*s[7]  - s[13]*s[3]*s[6];
        inverse[6]  = -s[0]*s[6]*s[15]  + s[0]*s[7]*s[14]  + s[4]*s[2]*s[15] - s[4]*s[3]*s[14] - s[12]*s[2]*s[7]  + s[12]*s[3]*s[6];
        inverse[10] =  s[0]*s[5]*s[15]  - s[0]*s[7]*s[13]  - s[4]*s[1]*s[15] + s[4]*s[3]*s[13] + s[12]*s[1]*s[7]  - s[12]*s[3]*s[5];
        inverse[14] = -s[0]*s[5]*s[14]  + s[0]*s[6]*s[13]  + s[4]*s[1]*s[14] - s[4]*s[2]*s[13] - s[12]*s[1]*s[6]  + s[12]*s[2]*s[5];

        inverse[3]  = -s[1]*s[6]*s[11]  + s[1]*s[7]*s[10]  + s[5]*s[2]*s[11] - s[5]*s[3]*s[10] - s[9]*s[2]*s[7]   + s[9]*s[3]*s[6];
        inverse[7]  =  s[0]*s[6]*s[11]  - s[0]*s[7]*s[10]  - s[4]*s[2]*s[11] + s[4]*s[3]*s[10] + s[8]*s[2]*s[7]   - s[8]*s[3]*s[6];
        inverse[11] = -s[0]*s[5]*s[11]  + s[0]*s[7]*s[9]   + s[4]*s[1]*s[11] - s[4]*s[3]*s[9]  - s[8]*s[1]*s[7]   + s[8]*s[3]*s[5];
        inverse[15] =  s[0]*s[5]*s[10]  - s[0]*s[6]*s[9]   - s[4]*s[1]*s[10] + s[4]*s[2]*s[9]  + s[8]*s[1]*s[6]   - s[8]*s[2]*s[5];

        float determinant = s[0]*inverse[0] + s[1]*inverse[4] + s[2]*inverse[8] + s[3]*inverse[12];
        if (std::fabs(determinant) < 1.0e-20f)
        {
            return false;
        }

        determinant = 1.0f / determinant;
        float* destination = &out.m[0][0];
        for (int i = 0; i < 16; ++i)
        {
            destination[i] = inverse[i] * determinant;
        }
        return true;
    }

    Mat4 Mat4PerspectiveTangents(float tanLeft, float tanRight, float tanTop, float tanBottom,
                                 float nearPlane, float farPlane)
    {
        const float width = tanRight - tanLeft;
        const float height = tanBottom - tanTop;

        // Derivation, so the signs are not guesswork:
        //   ndc.x = (x/z - centreX) * 2/(r-l)  with centreX = (l+r)/2
        //         = (2/(r-l)) * x/z  -  (l+r)/(r-l)
        // In row-vector form clip.x = X*m00 + Z*m20 and clip.w = Z, so
        // ndc.x = m00*(x/z) + m20, giving m20 = -(l+r)/(r-l). The offset terms are
        // NEGATIVE. Getting this wrong mirrors the frustum asymmetry, which is
        // invisible on a symmetric frustum and violent on a headset, where each eye
        // is canted outward.
        Mat4 result;
        result.m[0][0] = 2.0f / width;
        result.m[1][1] = 2.0f / height;
        result.m[2][0] = -(tanRight + tanLeft) / width;
        result.m[2][1] = -(tanBottom + tanTop) / height;
        result.m[2][2] = farPlane / (farPlane - nearPlane);
        result.m[2][3] = 1.0f;
        result.m[3][2] = -nearPlane * farPlane / (farPlane - nearPlane);
        return result;
    }

    Mat4 Mat4FromOpenVR(const float source[3][4])
    {
        // OpenVR stores a column-vector transform as 3 rows of 4. Transposing it
        // into a 4x4 gives the row-vector form the rest of the code expects.
        Mat4 result = Mat4Identity();
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                result.m[column][row] = source[row][column];
            }
        }
        result.m[0][3] = 0.0f;
        result.m[1][3] = 0.0f;
        result.m[2][3] = 0.0f;
        result.m[3][3] = 1.0f;
        return result;
    }

    Vec3 Mat4TranslationOf(const Mat4& a)
    {
        Vec3 result;
        result.x = a.m[3][0];
        result.y = a.m[3][1];
        result.z = a.m[3][2];
        return result;
    }

    bool Mat4IsAffine(const Mat4& a, float tolerance)
    {
        return std::fabs(a.m[0][3]) < tolerance
            && std::fabs(a.m[1][3]) < tolerance
            && std::fabs(a.m[2][3]) < tolerance
            && std::fabs(a.m[3][3] - 1.0f) < tolerance;
    }
}
