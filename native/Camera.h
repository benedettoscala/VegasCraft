#pragma once
#include "Bridge.h"
namespace vegas::camera {
struct Transform { float rot[3][3]; float pos[3]; float scale; };
// Pure camera calculation shared by the runtime and its fixture.
Transform orbit(double x, double y, double z, float yaw, float pitch, unsigned mode, float distance);
void update(const proto::McState& mc, double x, double y, double z, float yaw, float pitch);
void apply();
void clear();
}
