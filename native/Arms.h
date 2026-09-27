#pragma once
#include <cstdint>
#include <vector>

// New Vegas animates its first-person skeleton for every weapon (grip, firing, reloading).
// Minecraft's arms follow that skeleton: each frame the upper arm, forearm and hand bones of both
// arms are read in the view space of New Vegas' camera.
namespace vegas::arms {
struct Bone { float pos[3]; float rot[3][3]; };  // camera space: x right, y up, z backwards (OpenGL)
struct Arm { Bone upper, fore, hand; };
// False when the first-person skeleton or its bones aren't available.
bool sample(void* player, Arm out[2]);  // [0] right, [1] left
// Development log of the bones, the camera and the first-person camera bone.
void report(void* player);
// Per frame: sends the bones of the first- or third-person skeleton to Minecraft
// (proto::ArmSpace), or marks them invalid once (kArmsNone).
void publish(void* player, std::uint32_t space);
// Third person: New Vegas draws the wielded weapon in the hand of Minecraft's avatar. Its own
// third-person body keeps only the skeleton the weapon hangs on, and the weapon bone is placed
// at `hand` (McState::hand: relative to the feet, Minecraft axes, blocks) above `feet` (New Vegas
// units). Inactive: the body is restored.
// Minecraft (re)attached: New Vegas may be left in third person / auto vanity by its idle time; back to first.
void leaveThirdPerson(void* player);
void holdInThirdPerson(void* player, bool active, const float hand[12], const float feet[3], bool melee);
// First person, a melee weapon: its first-person model at Minecraft's first-person item pose
// (McState::hand), turned with that pose (Minecraft's swing).
void holdInFirstPerson(void* player, bool active, const float hand[12]);
// Development: the melee model's offset in Minecraft's item frame, first (1) or third (3) person.
void setMeleeOffset(int view, const float rotDeg[3], const float shift[3]);
void setMeleeSize(float size);
void logHeld(unsigned ms);
// Places that weapon: called just before New Vegas draws the world, after its own update has
// put the weapon back on the third-person hand bone.
void applyHeld();
// Development log of the third-person arm bones (world, relative to the player's position).
void reportThird(void* player);
// Development: the player's first- and third-person AnimData side by side.
void reportAnim(void* player);
// Development offset of the weapon in the avatar's hand: rotation (degrees about x, y, z of the
// hand frame) then translation (blocks in the hand frame).
void setWeaponOffset(const float rotDeg[3], const float shift[3]);
// Rigid visible weapon triangles in camera space (NV units, x right/y up/z forward).
bool weaponDepth(std::vector<float>& triangles);
void clear();
}
