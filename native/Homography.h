#pragma once
#include <cmath>

// Projective mapping between the unit square and a quadrilateral: the Pip-Boy's screen as seen
// from the camera (corners clockwise from the top left, back buffer pixels). The Minecraft page is
// drawn flat in a rectangle of the overlay frame and warped onto that quad; the mouse goes back.
namespace vegas::homography {
struct Mat3 { double m[9]; };
// The page leaves this share of the Pip-Boy's screen free all round (room for the screen's tilt).
inline constexpr float kPageInset = 0.02f;

// Unit square (0,0) (1,0) (1,1) (0,1) -> q[0..7] (x0 y0 x1 y1 x2 y2 x3 y3), Heckbert's solution.
inline Mat3 squareToQuad(const float* q) {
    const double x0 = q[0], y0 = q[1], x1 = q[2], y1 = q[3], x2 = q[4], y2 = q[5], x3 = q[6], y3 = q[7];
    const double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    const double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
    double g = 0, h = 0;
    if (std::fabs(dx3) > 1e-9 || std::fabs(dy3) > 1e-9) {
        const double det = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(det) > 1e-12) {
            g = (dx3 * dy2 - dx2 * dy3) / det;
            h = (dx1 * dy3 - dx3 * dy1) / det;
        }
    }
    return {{x1 - x0 + g * x1, x3 - x0 + h * x3, x0,
             y1 - y0 + g * y1, y3 - y0 + h * y3, y0,
             g, h, 1.0}};
}
inline void apply(const Mat3& a, double u, double v, double& x, double& y) {
    const double w = a.m[6] * u + a.m[7] * v + a.m[8];
    const double k = std::fabs(w) > 1e-12 ? 1.0 / w : 1.0;
    x = (a.m[0] * u + a.m[1] * v + a.m[2]) * k;
    y = (a.m[3] * u + a.m[4] * v + a.m[5]) * k;
}
inline Mat3 inverse(const Mat3& a) {
    const double* m = a.m;
    const double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    const double k = std::fabs(det) > 1e-18 ? 1.0 / det : 0.0;
    return {{c0 * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k,
             c1 * k, (m[0] * m[8] - m[2] * m[6]) * k, (m[2] * m[3] - m[0] * m[5]) * k,
             c2 * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k}};
}
// The part of the screen the page covers: the quad shrunk by `inset` (a share of its size) on every
// side, as a quad again (bilinear in the screen's corners).
inline void inner(const float* hole, float inset, float* out) {
    auto at = [&](float s, float t, float* o) {
        for (int k = 0; k < 2; ++k)
            o[k] = (1 - t) * ((1 - s) * hole[k] + s * hole[2 + k]) + t * ((1 - s) * hole[6 + k] + s * hole[4 + k]);
    };
    at(inset, inset, out); at(1 - inset, inset, out + 2); at(1 - inset, 1 - inset, out + 4); at(inset, 1 - inset, out + 6);
}
// The largest axis-aligned rectangle inside a quad that is (nearly) a rectangle: where the page is
// laid out. x0 y0 x1 y1.
inline void insideRect(const float* q, float* r) {
    r[0] = std::fmax(q[0], q[6]);   // left: the larger of the two left corners' x
    r[2] = std::fmin(q[2], q[4]);   // right
    r[1] = std::fmax(q[1], q[3]);   // top
    r[3] = std::fmin(q[7], q[5]);   // bottom
}
} // namespace vegas::homography
