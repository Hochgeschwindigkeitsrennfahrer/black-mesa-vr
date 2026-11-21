#pragma once
#include <cmath>
namespace bmvr_grip
{
struct V
{
    float x, y, z;
    V operator+(V b) const
    {
        return {x + b.x, y + b.y, z + b.z};
    }
    V operator-(V b) const
    {
        return {x - b.x, y - b.y, z - b.z};
    }
    V operator*(float s) const
    {
        return {x * s, y * s, z * s};
    }
};
inline float dot(V a, V b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V cross(V a, V b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool unit(V &a)
{
    float q = dot(a, a);
    if (!std::isfinite(q) || q < 1e-10f)
        return false;
    a = a * (1.f / std::sqrt(q));
    return true;
}
// A model-defined anatomical approximation, not an authored grip attachment.
// +Z runs index -> little finger; +Y runs knuckle row -> wrist, orthogonal
// to Z; +X completes the right-handed frame. For a neutral closed hand,
// OpenXR requires +X away from the left palm / into the right palm. Using
// wrist -> knuckles for +Y reverses that normal on both hands (a half-turn
// around grip Z). This construction remains an anatomical approximation,
// not proof that a runtime's published pose follows the OpenXR grip frame.
// The origin is midway between wrist and the mean of three knuckle roots.
inline bool frame(V wrist, V index, V middle, V pinky, float out[3][4])
{
    V knuckles = (index + middle + pinky) * (1.f / 3.f);
    V z = pinky - index;
    V y = wrist - knuckles;
    if (!unit(z))
        return false;
    y = y - z * dot(y, z);
    if (!unit(y))
        return false;
    V x = cross(y, z);
    if (!unit(x))
        return false;
    y = cross(z, x);
    V p = (wrist + knuckles) * .5f;
    float v[3][4] = {{x.x, y.x, z.x, p.x}, {x.y, y.y, z.y, p.y}, {x.z, y.z, z.z, p.z}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
        {
            if (!std::isfinite(v[i][j]))
                return false;
            out[i][j] = v[i][j];
        }
    return true;
}
inline void inverse(const float a[3][4], float b[3][4])
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            b[i][j] = a[j][i];
        b[i][3] = -(b[i][0] * a[0][3] + b[i][1] * a[1][3] + b[i][2] * a[2][3]);
    }
}
inline void mul(const float a[3][4], const float b[3][4], float c[3][4])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
        {
            c[i][j] = j == 3 ? a[i][3] : 0.f;
            for (int k = 0; k < 3; k++)
                c[i][j] += a[i][k] * b[k][j];
        }
}
} // namespace bmvr_grip
