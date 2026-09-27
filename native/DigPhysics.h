#pragma once
#include "Game.h"

namespace vegas::digphysics {
// Incoming hkpCdPoint (not the collector's larger hkpRootCdPoint array element).
// Layout verified at CAB970/CD36A0 in FalloutNV 1.4.0.525.
struct Contact { float position[4], normal[4]; const void* bodyA; const void* bodyB; };
static_assert(sizeof(Contact) == 0x28);
bool onDugGround(const Contact& point);
bool abovePuppetFeet(const Contact& point);
// Havok position of the player's feet while Minecraft drives it. Besides the body match, ground
// contacts above the feet near the puppet are dropped: the landscape is a solid height field, so in a
// one-block hole the capsule's rim penetrates the neighbouring columns and Havok lifts it out.
void setPuppet(const void* collidable, float feetHavokZ, float havokX = 0, float havokY = 0);
bool groundAbovePuppet(const Contact& point);
// Only the actor movement rescue call is changed; terrain queries used to export material
// and collisions continue to return the original landscape height.
void adjustLandHeight(const game::NiPoint3& position, float& height);
// Diagnostics: logs the next `count` character contacts within 1.5 blocks (x, z) that were kept or dropped.
void traceContacts(unsigned count, float x, float z);
void install();
}
