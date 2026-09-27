#pragma once
#include "Bridge.h"
#include <vector>
namespace vegas::blocklights {
struct Point { float x, y, z, r, g, b, radius, score; unsigned kind; std::uint64_t id; };
// Writes verified NiPointLight fields. Does not register the light with the scene.
void configure(void* light, const Point& point, float time);
void onLights(const std::uint8_t* data, std::uint32_t bytes);
std::vector<Point> nearby(const Position& player);
void update(const Position& player, bool enabled);
// The clusters of the last update (strongest first), in Minecraft coordinates.
const std::vector<Point>& active();
void clear();
// Diagnostics: multiplies every light's radius and brightness (1 = normal).
void setBoost(float radius, float brightness);
#ifdef VEGAS_TESTING
struct Api {
    void* (__cdecl* scene)();
    void* (__cdecl* create)();
    void (__cdecl* attenuation)(void*, unsigned);
    void* (__thiscall* add)(void*, void*, bool);
    void (__thiscall* remove)(void*, void**);
};
void setApiForTests(const Api& api);
#endif
}
