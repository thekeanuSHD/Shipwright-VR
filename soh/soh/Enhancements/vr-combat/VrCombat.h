#pragma once

// VR Physical Combat (gVrPhysCombat): motion-driven melee, shield, carrying and archery,
// replacing the animation-driven systems while active. This header is the module's surface for
// both the C decomp code (master predicate + per-frame shims, added milestone by milestone) and
// the module's own C++ files (soh/soh/Enhancements/vr-combat/).
//
// Game types appear only as pointers to tagged structs (struct Player / struct PlayState), so
// the header stays includable from any TU; consumers that pass real objects naturally already
// include the game headers.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Master predicate: true only when every physical-combat patch should take the VR path THIS
// frame — VR first person with the toggle on, a save loaded, and no state where the game owns
// Link's hands (cutscene, forced action, screen transition). Decomp patches call this and fall
// back to vanilla when false, so scripted sequences behave stock frame-by-frame.
bool VrCombat_Active(void);

// --- Physical melee (VrSwing.cpp) ---

// The held melee weapon is handled by physical combat: 1H swords (Master/Kokiri) and the broken
// Giant's Knife. Sticks, hammer and the full Biggoron Sword keep vanilla button combat until
// their own milestones. Also false for the co-op partner (only the real player swings).
bool VrCombat_MeleeCovered(struct Player* player);

// Draw-time feed, called from the Player L_HAND PostLimbDraw seam with the live (controller)
// matrix on the stack and the blade length already configured (D_80126080.x). Tracks swing
// speed, mirrors meleeWeaponState for enemy AI, feeds the sword trail, and registers velocity-
// gated swept sub-quads as AT colliders.
void VrCombat_FeedMelee(struct PlayState* play, struct Player* player);

// True while any physical-melee quad has an AT hit recorded this tick. OR'd into the vanilla
// "landing an attack cancels incoming damage this frame" check, which only knows about the
// player's own two quads.
bool VrCombat_MeleeQuadsHit(void);

// Implemented in z_player.c (// SOH [VR]): forwards to the file-internal melee-weapon-state
// setter func_80833A20, which plays the swing SFX / voice and counts the swing stat on the
// 0 -> nonzero edge. The vr-combat module drives the state from real hand motion.
void VrCombat_SetMeleeWeaponState(struct Player* player, int32_t newState);

// Visual-mesh harvest exclusion: brackets a display-list section the physical blade must NOT
// collide with (the player's own arms/weapon, the sword trail) by emitting mask marker
// commands into the OPA and XLU buckets. No-ops while physical combat is inactive.
void VrCombat_MeshMaskPush(struct GraphicsContext* gfxCtx);
void VrCombat_MeshMaskPop(struct GraphicsContext* gfxCtx);
// Marks a display-list section as FLESH for the visual-mesh harvest (enemy/NPC bodies):
// contacts stay silent (no stone clank/sparks) and read as bodies to the impact pipeline.
void VrCombat_MeshFleshPush(struct GraphicsContext* gfxCtx);
void VrCombat_MeshFleshPop(struct GraphicsContext* gfxCtx);

// Limb puppetry: the blade manipulates individual limbs of nearby NPC/enemy skeletons —
// rest the blade under an arm and the arm rides it; push a head and it tilts away; hits kick
// the limbs near the impact. The Warp pair brackets each limb's matrix conversion in
// z_skelanime.c: Begin records this limb's world position for the contact solve (the clean
// animated pose) and then offsets the matrix translation by the limb's current puppet offset;
// End restores it so child limbs compute cleanly. Purely visual: animation, hitboxes and AI
// never see the offsets. `actorArg` is compared before any dereference against actors the
// contact gather validated this tick.
void VrCombat_FlinchWarpBegin(struct PlayState* play, void* actorArg, int32_t limbIndex);
void VrCombat_FlinchWarpEnd(void);

#ifdef __cplusplus
}

// C++-side module internals (swing tracking, debug overlay, menu readouts).
#include <vr_interface.h>

// Global-scope forward declarations, NOT `struct X` inside the namespace: an elaborated type
// specifier in a namespace-scope parameter would silently declare a fresh VrCombat::X in any TU
// that includes this header before the game headers.
struct PlayState;
struct Player;

namespace VrCombat {

// Hand path drained this game tick: world-space samples at headset rate, oldest first. Drained
// exactly once per tick into this snapshot so every consumer sees the same data. 24 covers a
// 20 Hz tick of 120 Hz XR frames (~6) with generous slack for hitches.
struct TickPath {
    VrHandSample samples[24];
    int count;
};
const TickPath& GetTickPath(int hand);

// Per-tick swing bookkeeping (VrSwing.cpp), driven from VrCombat's OnPlayerUpdate hook:
// reads back last tick's quad results (hit haptics, one-hit-per-swing demotion), then resets
// the quads' AT flags for the coming draw.
void Swing_OnPlayerUpdate(PlayState* play, Player* player);
// Falling edge of VrCombat_Active(): hand melee state back to vanilla (state 0, trail off).
void Swing_Deactivate(PlayState* play, Player* player);

// Debug-overlay snapshots (this tick): the blade's contact-primitive set, and this draw's
// registered damage quads as 12 floats each (4 verts x xyz). Plain float arrays so this header
// never needs the game's Vec3f type.
int Swing_GetDebugContactPrims(VrContactPrim* out, int maxPrims);
int Swing_GetDebugQuads(float* outVerts12PerQuad, int maxQuads);
// Physical blade rectangle outline (5 world-unit points: rootA, cornerA, tip, cornerB, rootB);
// returns 0 when the sim blade is inactive.
int Swing_GetDebugBladeOutline(float* outPts5x3);
// Puppet-tracked limbs (world positions + current offset magnitude per limb). Overlay proof
// of which bodies the blade can manipulate.
int Swing_GetDebugPuppetLimbs(float* outPos3PerLimb, float* outOffsetMag, int maxLimbs);

} // namespace VrCombat
#endif
