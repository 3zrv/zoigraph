#include "render/draw.h"

#include <raymath.h>

#include <cmath>

namespace zg::render {

void draw_jagged_line(Vector3 a, Vector3 b, Color color, double time, long long seed) {
    constexpr int   kSegments  = 8;
    constexpr float kAmplitude = 0.45f;

    const Vector3 dir    = Vector3Subtract(b, a);
    const float   length = Vector3Length(dir);
    if (length < 0.01f) {
        DrawLine3D(a, b, color);
        return;
    }
    const Vector3 dir_n = Vector3Scale(dir, 1.0f / length);
    const Vector3 ref   = (std::fabs(dir_n.y) < 0.9f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    const Vector3 perp1 = Vector3Normalize(Vector3CrossProduct(dir_n, ref));
    const Vector3 perp2 = Vector3Normalize(Vector3CrossProduct(dir_n, perp1));

    Vector3 prev = a;
    for (int i = 1; i < kSegments; ++i) {
        const float t   = static_cast<float>(i) / kSegments;
        const float seg = static_cast<float>(i) * 37.0f
                        + static_cast<float>(seed % 997)
                        + static_cast<float>(time) * 7.0f;
        const float p1off = std::sin(seg)         * kAmplitude;
        const float p2off = std::cos(seg * 1.31f) * kAmplitude;
        const Vector3 base   = Vector3Lerp(a, b, t);
        const Vector3 offset = Vector3Add(Vector3Scale(perp1, p1off),
                                          Vector3Scale(perp2, p2off));
        const Vector3 jagged = Vector3Add(base, offset);
        DrawLine3D(prev, jagged, color);
        prev = jagged;
    }
    DrawLine3D(prev, b, color);
}

}  // namespace zg::render
