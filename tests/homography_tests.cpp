#include "Homography.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

// The Pip-Boy page's mapping: unit square <-> screen quad, and the cursor going back.
using namespace vegas::homography;
static int failures = 0;
static void check(bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; } }
static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

int main() {
    // A skewed screen (like the Pip-Boy seen at an angle): corners clockwise from the top left.
    const float quad[8] = {599, 117, 1520, 109, 1540, 822, 586, 820};
    const Mat3 map = squareToQuad(quad);
    double x, y;
    apply(map, 0, 0, x, y); check(near(x, 599) && near(y, 117), "top left corner");
    apply(map, 1, 0, x, y); check(near(x, 1520) && near(y, 109), "top right corner");
    apply(map, 1, 1, x, y); check(near(x, 1540) && near(y, 822), "bottom right corner");
    apply(map, 0, 1, x, y); check(near(x, 586) && near(y, 820), "bottom left corner");
    // The way back for any point of the square.
    const Mat3 back = inverse(map);
    for (double u : {0.0, 0.25, 0.5, 0.9, 1.0})
        for (double v : {0.0, 0.3, 0.5, 0.8, 1.0}) {
            double px, py, bu, bv;
            apply(map, u, v, px, py);
            apply(back, px, py, bu, bv);
            check(near(bu, u, 1e-6) && near(bv, v, 1e-6), "round trip");
        }
    // An axis-aligned rectangle maps affinely: the centre stays the centre.
    const float rect[8] = {100, 100, 300, 100, 300, 200, 100, 200};
    apply(squareToQuad(rect), 0.5, 0.5, x, y); check(near(x, 200) && near(y, 150), "rectangle centre");
    // The part the page covers: shrunk by the inset on every side.
    float shrunk[8];
    inner(rect, 0.1f, shrunk);
    check(near(shrunk[0], 120) && near(shrunk[1], 110) && near(shrunk[4], 280) && near(shrunk[5], 190), "inset");
    float r[4];
    insideRect(quad, r);
    check(r[0] >= 599 && r[2] <= 1520 && r[1] >= 117 && r[3] <= 820, "the layout rectangle lies inside the quad");
    if (!failures) std::puts("PASS homography: corners, round trip, inset, layout rectangle");
    return failures ? 1 : 0;
}
