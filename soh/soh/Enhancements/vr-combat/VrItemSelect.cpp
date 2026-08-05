extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h" // gItemIcons, gMtxClear
extern PlayState* gPlayState;
// Both live in z_player.c with no header declaration of their own.
void Player_UseItem(PlayState* play, Player* player, s32 item);
s8 Player_ItemToItemAction(s32 item);
}

#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// Alyx-style item selector (VR first person, independent of physical combat): HOLD the
// configured VR input, a compass of item icons appears anchored where your hand was, FLICK the
// hand toward one, RELEASE to take it. Up = sword (and the shield, which rides the off hand on
// its own), left/right/down = the three equipped C items, no movement = empty hands. This is
// Valve's Half-Life: Alyx weapon menu almost verbatim (on Touch controllers: hold weapon-hand
// stick-click, move the HAND, release) — chosen there over holsters precisely because it never
// misses and never drops anything.
//
// Selection is by HAND DISPLACEMENT from the hold-start anchor, measured in the head's frame
// (lateral = camera right, vertical = world up), with hysteresis on the center boundary and a
// haptic tick on every highlight change. Execution goes through the real input path
// (EmulateButtonPress), so items behave exactly as if their C button had been pressed; center
// uses the vanilla put-away flow. While the selector is open, its hand's thumbstick is
// suppressed at the source (no turning/C-buttons from the thumb resting on a clicked stick)
// and padmgr skips the held input's normal button binding via VrItemSelect_ConsumesInput.
//
// SELECTOR MODE also changes how items are USED. The selector equips; the TRIGGER of the hand
// the item ended up in fires it. That is two rules, both below:
//
//  1. No button may CHANGE the held item (VB_CHANGE_HELD_ITEM_AND_USE_ITEM below). One rule
//     covers "C buttons no longer pull out items" AND "B no longer draws the sword" — drawing
//     and stowing belong to the selector alone. A press whose item is ALREADY held still goes
//     through, because that is the trigger mirror asking to use it. C presses keep working
//     everywhere the item system isn't the consumer: the pause menu still assigns items, third
//     person is untouched, and ocarina notes (a separate consumer of the same buttons) are
//     unaffected.
//  2. The holding hand's trigger mirrors that item's own button as raw pad STATE (padmgr,
//     VrItemSelect_TriggerItemMask). State rather than a one-frame emulated press is what makes
//     hold semantics work: press nocks the bow, holding keeps it drawn, RELEASE looses the
//     arrow — identical to holding the item's C button on a controller, so every vanilla rule
//     (ammo, magic, bottles, aim-and-throw) applies with nothing re-implemented. Both triggers
//     are reserved outright in selector mode (VrItemSelect_TriggerConsumed) rather than falling
//     back to a binding while the hands are empty: an input that changes meaning with hidden
//     state is worse in VR than an idle one. The selector-mode binding profile in padmgr moves
//     Z-target and the rest onto the grips and face buttons to pay for it.

namespace {

enum SelSector { SEC_CENTER = 0, SEC_UP, SEC_DOWN, SEC_LEFT, SEC_RIGHT };

bool sOpen = false;
int sHand = VR_HAND_RIGHT;
int sSector = SEC_CENTER;
Vec3f sAnchor;    // world units, the compass position THIS tick (body position + sAnchorOff)
Vec3f sAnchorOff; // hand-at-hold-start relative to Link's BODY: the compass rides along when
                  // the player keeps moving with the stick (or walks physically) mid-hold, and
                  // locomotion never reads as a flick — only hand motion relative to the body.
Vec3f sHeadRight; // camera right captured at open: stable targets, "left is left as you see it"

// Game ticks left in which a button-driven held-item CHANGE is still allowed: the selector's own
// EmulateButtonPress is consumed by the next pad poll, a tick or two after ExecuteSector runs, so
// the block below has to let its own press through by more than a same-frame stamp.
int sEquipGrace = 0;

int SwordHand() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

int SelectorHand() {
    const int swordHand = SwordHand();
    return CVarGetInteger("gVrItemSelHand", 0) == 0 ? swordHand : (1 - swordHand);
}

// Selector mode owns item activation: the toggle, in VR first person. Third person and flat
// screen keep stock behavior (there the right thumbstick is the C-stick, as it always was).
bool SelectorModeInPlay() {
    return CVarGetInteger("gVrItemSelect", 1) && VR_IsInitialized() && VR_GetFirstPerson() &&
           !VR_IsFlatScreen();
}

// Which VR controller physically holds the current held item, or -1 when Link's hands are empty.
// Link's L_HAND limb rides the sword-hand controller and R_HAND the other (the motion-hands limb
// override in z_player_lib.c), so the live model-group hand types ARE the answer — no table of
// items to keep in sync. Bow, slingshot, hookshot and the ocarinas are right-hand models, so they
// sit in the OFF hand; sword, Deku stick, boomerang, hammer, bottle and every open-handed item
// (bombs, nuts, magic) are left-hand models and sit in the sword hand.
int HeldItemVrHand(Player* player) {
    if (player == NULL || player->heldItemAction <= PLAYER_IA_NONE) {
        return -1;
    }
    switch (player->rightHandType) {
        case PLAYER_MODELTYPE_RH_BOW_SLINGSHOT:
        case PLAYER_MODELTYPE_RH_BOW_SLINGSHOT_2:
        case PLAYER_MODELTYPE_RH_HOOKSHOT:
        case PLAYER_MODELTYPE_RH_OCARINA:
        case PLAYER_MODELTYPE_RH_OOT:
            return SwordHand() ^ 1;
        default:
            return SwordHand();
    }
}

// The N64 button the held item answers to. heldItemButton is an index into z_player.c's
// sItemButtons, mirrored here in the same order (the D-pad tail is the DpadEquips enhancement).
uint16_t HeldItemButtonMask(Player* player) {
    static const uint16_t kItemButtons[] = { BTN_B,   BTN_CLEFT, BTN_CDOWN, BTN_CRIGHT,
                                             BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
    const int slot = player->heldItemButton;
    return (slot >= 0 && slot < (int)(sizeof(kItemButtons) / sizeof(kItemButtons[0]))) ? kItemButtons[slot] : 0;
}

uint16_t SelectorMask() {
    return (uint16_t)CVarGetInteger("gVrItemSelInput", VR_BTN_THUMBCLICK);
}

float PickThresholdUnits() {
    float ws = VR_GetWorldScale();
    if (ws < 1.0f) {
        ws = 35.0f;
    }
    return CVarGetFloat("gVrItemSelDistance", 5.0f) * 0.01f * ws; // cm -> game units
}

bool SelectorAvailable() {
    if (!CVarGetInteger("gVrItemSelect", 1)) {
        return false;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen()) {
        return false;
    }
    if (!GameInteractor::IsSaveLoaded(true) || gPlayState == NULL) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || Player_InBlockingCsMode(gPlayState, player)) {
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->csCtx.state != CS_STATE_IDLE) {
        return false;
    }
    return true;
}

void CloseSelector() {
    if (sOpen) {
        VR_SetStickSuppressed(sHand, 0);
        sOpen = false;
        sSector = SEC_CENTER;
    }
}

void ExecuteSector(int sector) {
    Player* player = (gPlayState != NULL) ? GET_PLAYER(gPlayState) : NULL;
    // The selector is the ONE thing allowed to change the held item in selector mode; open the
    // window before emulating, since the press lands a tick or two later (see sEquipGrace).
    sEquipGrace = 3;
    switch (sector) {
        case SEC_LEFT:
            GameInteractor::RawAction::EmulateButtonPress(BTN_CLEFT);
            break;
        case SEC_RIGHT:
            GameInteractor::RawAction::EmulateButtonPress(BTN_CRIGHT);
            break;
        case SEC_DOWN:
            GameInteractor::RawAction::EmulateButtonPress(BTN_CDOWN);
            break;
        case SEC_UP:
            // Sword and shield: draw the sword via the same path B uses. Already drawn = keep
            // it (never emulate B with the sword out — outside physical combat that swings).
            if (player != NULL && Player_GetMeleeWeaponHeld(player) == 0) {
                GameInteractor::RawAction::EmulateButtonPress(BTN_B);
            }
            break;
        default:
            // Empty hands: the vanilla put-away flow (sheathes the sword, stows the item; the
            // physical shield stays on the off hand under its own rules).
            if (player != NULL) {
                Player_UseItem(gPlayState, player, ITEM_NONE);
            }
            break;
    }
}

void ItemSelectTick() {
    if (sEquipGrace > 0) {
        sEquipGrace--;
    }
    const bool avail = SelectorAvailable();

    if (!sOpen) {
        if (avail && (VR_GetControllerButton(SelectorHand()) & SelectorMask())) {
            float pos[3];
            float quat[4];
            float eye[3];
            float fwd[3];
            float up[3];
            const int hand = SelectorHand();
            if (VR_GetHandPose(hand, pos, quat)) {
                VR_GetCameraPose(eye, fwd, up);
                // camera right = fwd x up, flattened to the horizon so "left/right" stays
                // level even when looking up or down.
                Vec3f right = { fwd[1] * up[2] - fwd[2] * up[1], 0.0f, fwd[0] * up[1] - fwd[1] * up[0] };
                const float rl = sqrtf(right.x * right.x + right.z * right.z);
                if (rl > 1e-3f) {
                    right.x /= rl;
                    right.z /= rl;
                } else {
                    right = { 1.0f, 0.0f, 0.0f };
                }
                Player* player = GET_PLAYER(gPlayState);
                sAnchor = { pos[0], pos[1], pos[2] };
                sAnchorOff = { pos[0] - player->actor.world.pos.x, pos[1] - player->actor.world.pos.y,
                               pos[2] - player->actor.world.pos.z };
                sHeadRight = right;
                sHand = hand;
                sSector = SEC_CENTER;
                sOpen = true;
                VR_SetStickSuppressed(sHand, 1);
                VR_TriggerHaptic(sHand, 0.3f, 0.0f, 20.0f);
            }
        }
        return;
    }

    // Open: lost availability (cutscene, menu, mode off) cancels without executing.
    if (!avail) {
        CloseSelector();
        return;
    }

    const bool held = (VR_GetControllerButton(sHand) & SelectorMask()) != 0;
    // The compass rides Link's body: re-derive the anchor from the current body position so
    // stick movement (and physical walking) carries it along instead of leaving it behind.
    {
        Player* player = GET_PLAYER(gPlayState);
        sAnchor = { player->actor.world.pos.x + sAnchorOff.x, player->actor.world.pos.y + sAnchorOff.y,
                    player->actor.world.pos.z + sAnchorOff.z };
    }
    float pos[3];
    float quat[4];
    if (VR_GetHandPose(sHand, pos, quat)) {
        const float dxw = pos[0] - sAnchor.x;
        const float dyw = pos[1] - sAnchor.y;
        const float dzw = pos[2] - sAnchor.z;
        const float lat = dxw * sHeadRight.x + dzw * sHeadRight.z; // camera-right component
        const float vert = dyw;                                    // world up
        const float r = sqrtf(lat * lat + vert * vert);
        const float th = PickThresholdUnits();

        int newSector = sSector;
        if (sSector == SEC_CENTER) {
            if (r >= th) {
                newSector = (fabsf(lat) >= fabsf(vert)) ? (lat > 0.0f ? SEC_RIGHT : SEC_LEFT)
                                                        : (vert > 0.0f ? SEC_UP : SEC_DOWN);
            }
        } else if (r < th * 0.65f) {
            newSector = SEC_CENTER; // hysteresis: come well back before it reads as "no pick"
        } else {
            // Re-derive the dominant axis, but require a 15% lead to leave the current sector
            // so the boundary between adjacent directions doesn't flicker.
            const bool curLateral = (sSector == SEC_LEFT || sSector == SEC_RIGHT);
            const float curMag = curLateral ? fabsf(lat) : fabsf(vert);
            const float othMag = curLateral ? fabsf(vert) : fabsf(lat);
            if (othMag > curMag * 1.15f) {
                newSector = curLateral ? (vert > 0.0f ? SEC_UP : SEC_DOWN) : (lat > 0.0f ? SEC_RIGHT : SEC_LEFT);
            } else if (curLateral) {
                newSector = lat > 0.0f ? SEC_RIGHT : SEC_LEFT;
            } else {
                newSector = vert > 0.0f ? SEC_UP : SEC_DOWN;
            }
        }
        if (newSector != sSector) {
            sSector = newSector;
            VR_TriggerHaptic(sHand, 0.4f, 0.0f, 25.0f); // the Alyx confirmation tick
        }
    }

    if (!held) {
        const int pick = sSector;
        CloseSelector();
        ExecuteSector(pick);
    }
}

// ---- In-world compass rendering (OnPlayDrawEnd, colViewer pattern) ----

constexpr int kSelMaxGfx = 192;
constexpr int kSelMaxVtx = 48;
Gfx sSelDl[kSelMaxGfx];
Vtx sSelVtx[kSelMaxVtx];
int sSelVtxUsed = 0;

#define SEL_VTX(x, y, z, s, t)                                                                    \
    {                                                                                             \
        .v = {.ob = { (s16)(x), (s16)(y), (s16)(z) }, .flag = 0, .tc = { (s16)(s), (s16)(t) },    \
              .cn = { 255, 255, 255, 255 } }                                                      \
    }

// Camera-facing quad centered at `at`, half-size `hs`, using camera right/up axes. Returns the
// base vertex index or -1 when the pool is full.
int PushBillboardVtx(const Vec3f& at, const Vec3f& camRight, const Vec3f& camUp, float hs) {
    if (sSelVtxUsed + 4 > kSelMaxVtx) {
        return -1;
    }
    const int base = sSelVtxUsed;
    const float rx = camRight.x * hs, ry = camRight.y * hs, rz = camRight.z * hs;
    const float ux = camUp.x * hs, uy = camUp.y * hs, uz = camUp.z * hs;
    // Order: top-left, top-right, bottom-left, bottom-right (t grows downward in the texture).
    sSelVtx[base + 0] = SEL_VTX(at.x - rx + ux, at.y - ry + uy, at.z - rz + uz, 0, 0);
    sSelVtx[base + 1] = SEL_VTX(at.x + rx + ux, at.y + ry + uy, at.z + rz + uz, 31 << 5, 0);
    sSelVtx[base + 2] = SEL_VTX(at.x - rx - ux, at.y - ry - uy, at.z - rz - uz, 0, 31 << 5);
    sSelVtx[base + 3] = SEL_VTX(at.x + rx - ux, at.y + ry - uy, at.z + rz - uz, 31 << 5, 31 << 5);
    sSelVtxUsed += 4;
    return base;
}

} // namespace

// extern "C" linkage is load-bearing: OPEN_DISPS/CLOSE_DISPS re-declare the
// FrameInterpolation_Record* functions at BLOCK scope, and a block-scope declaration inherits
// the enclosing function's language linkage — inside a C++ function it would mangle and fail
// to link against the C definitions (same note as VrCombat_DrawDebugOverlay).
extern "C" void VrItemSelect_Draw(void) {
    if (!sOpen || gPlayState == NULL) {
        return;
    }

    Gfx* p = sSelDl;
    sSelVtxUsed = 0;

    // Camera axes for billboarding.
    float eyeArr[3];
    float fwdArr[3];
    float upArr[3];
    VR_GetCameraPose(eyeArr, fwdArr, upArr);
    const Vec3f camUp = { upArr[0], upArr[1], upArr[2] };
    Vec3f camRight = { fwdArr[1] * upArr[2] - fwdArr[2] * upArr[1], fwdArr[2] * upArr[0] - fwdArr[0] * upArr[2],
                       fwdArr[0] * upArr[1] - fwdArr[1] * upArr[0] };
    const float crl = sqrtf(camRight.x * camRight.x + camRight.y * camRight.y + camRight.z * camRight.z);
    if (crl > 1e-3f) {
        camRight.x /= crl;
        camRight.y /= crl;
        camRight.z /= crl;
    }

    const float th = PickThresholdUnits();
    // Icons sit just past the pick threshold (flick INTO them), with size floors so a short
    // flick distance doesn't shrink the compass into unreadability.
    const float ringR = fmaxf(th * 1.5f, 4.5f);
    const float iconHs = fmaxf(th * 0.55f, 1.6f);

    // Direction -> LOCAL offset from the anchor (lateral along the captured head-right,
    // vertical along world up) and the item shown there. equips.buttonItems: 0 = B (sword),
    // 1/2/3 = C-left/down/right.
    struct SelTarget {
        int sector;
        Vec3f at;
        u8 item;
    };
    const SelTarget targets[4] = {
        { SEC_UP, { 0.0f, ringR, 0.0f }, gSaveContext.equips.buttonItems[0] },
        { SEC_DOWN, { 0.0f, -ringR, 0.0f }, gSaveContext.equips.buttonItems[2] },
        { SEC_LEFT, { -sHeadRight.x * ringR, 0.0f, -sHeadRight.z * ringR }, gSaveContext.equips.buttonItems[1] },
        { SEC_RIGHT, { sHeadRight.x * ringR, 0.0f, sHeadRight.z * ringR }, gSaveContext.equips.buttonItems[3] },
    };

    // Setup: textured XLU billboards, no Z compare or write — the compass is UI, never occluded
    // (and never harvested: the visual-mesh gather requires depth-write). The anchor rides in
    // a RECORDED matrix (vertices are anchor-local) so frame interpolation glides the compass
    // at render rate alongside the world instead of stepping it at the 20 Hz game rate.
    FrameInterpolation_RecordOpenChild((const void*)sSelDl, 0);
    Matrix_Translate(sAnchor.x, sAnchor.y, sAnchor.z, MTXMODE_NEW);
    gSPMatrix(p++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    FrameInterpolation_RecordCloseChild();
    gDPPipeSync(p++);
    gDPSetCycleType(p++, G_CYC_1CYCLE);
    gDPSetRenderMode(p++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetTextureFilter(p++, G_TF_BILERP);
    gSPClearGeometryMode(p++, G_CULL_BOTH | G_LIGHTING);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);

    for (int i = 0; i < 4; i++) {
        const bool selected = (sSector == targets[i].sector);
        const float hs = selected ? iconHs * 1.35f : iconHs;
        const int base = PushBillboardVtx(targets[i].at, camRight, camUp, hs);
        if (base < 0) {
            break;
        }
        if (targets[i].item < 158) {
            gDPSetCombineMode(p++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            if (selected) {
                gDPSetPrimColor(p++, 0, 0, 255, 255, 255, 255);
            } else {
                gDPSetPrimColor(p++, 0, 0, 165, 165, 165, 185);
            }
            gDPLoadTextureBlock(p++, gItemIcons[targets[i].item], G_IM_FMT_RGBA, G_IM_SIZ_32b, 32, 32, 0,
                                G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 5, 5, G_TX_NOLOD,
                                G_TX_NOLOD);
        } else {
            // Empty slot (or empty B): a dim placeholder diamond.
            gDPSetCombineMode(p++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
            gDPSetPrimColor(p++, 0, 0, 120, 120, 120, selected ? 160 : 90);
        }
        gSPVertex(p++, (uintptr_t)&sSelVtx[base], 4, 0);
        gSP2Triangles(p++, 0, 1, 2, 0, 2, 1, 3, 0);
    }

    // Center: a small ring marker at the anchor (empty hands), brighter while it is the pick.
    {
        const int base = PushBillboardVtx({ 0.0f, 0.0f, 0.0f }, camRight, camUp,
                                          (sSector == SEC_CENTER) ? iconHs * 0.55f : iconHs * 0.35f);
        if (base >= 0) {
            gDPSetCombineMode(p++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
            if (sSector == SEC_CENTER) {
                gDPSetPrimColor(p++, 0, 0, 255, 250, 210, 235);
            } else {
                gDPSetPrimColor(p++, 0, 0, 190, 190, 190, 130);
            }
            gSPVertex(p++, (uintptr_t)&sSelVtx[base], 4, 0);
            gSP2Triangles(p++, 0, 1, 2, 0, 2, 1, 3, 0);
        }
    }

    gSPEndDisplayList(p++);

    OPEN_DISPS(gPlayState->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, sSelDl);
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

extern "C" bool VrItemSelect_ConsumesInput(int32_t vrHand, uint16_t vrBtnMask) {
    // The configured selector input is DEDICATED while the feature is enabled: padmgr never
    // feeds it to the button bindings, so the opening click can't leak its normal action.
    if (!CVarGetInteger("gVrItemSelect", 1)) {
        return false;
    }
    return vrHand == SelectorHand() && (vrBtnMask & SelectorMask()) != 0;
}

extern "C" bool VrItemSelect_ModeActive(void) {
    return SelectorModeInPlay();
}

extern "C" uint16_t VrItemSelect_TriggerItemMask(int32_t vrHand) {
    if (!SelectorModeInPlay() || gPlayState == NULL) {
        return 0;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || HeldItemVrHand(player) != vrHand) {
        return 0;
    }
    // Physical combat owns the weapons it covers: the swing IS the attack, so the sword hand's
    // trigger stays idle rather than also emitting B. Weapons physical combat does NOT cover
    // (Deku stick, hammer, Biggoron's) keep button attacks, so they keep the mirror.
    if (VrCombat_Active() && VrCombat_MeleeCovered(player)) {
        return 0;
    }
    return HeldItemButtonMask(player);
}

extern "C" bool VrItemSelect_TriggerConsumed(int32_t vrHand, uint16_t vrBtnMask) {
    // Both triggers belong to item use while the mode is on, held item or not — see the header
    // comment on why they don't fall back to a binding when the hands are empty. Not gated on
    // first person: the selector profile leaves the triggers unbound anyway, so reserving them
    // everywhere keeps the meaning of a trigger squeeze the same in every view.
    (void)vrHand;
    return CVarGetInteger("gVrItemSelect", 1) && (vrBtnMask & VR_BTN_TRIGGER) != 0;
}

static void RegisterVrItemSelect() {
    COND_HOOK(OnPlayerUpdate, CVarGetInteger("gVrItemSelect", 1), ItemSelectTick);
    COND_HOOK(OnPlayDrawEnd, CVarGetInteger("gVrItemSelect", 1), VrItemSelect_Draw);

    // Rule 1 (see the file header): in selector mode no button may CHANGE what's in Link's hands
    // — that is the selector's job. A press for the item ALREADY held passes through untouched,
    // which is exactly what the trigger mirror emits, so use/fire keeps the full vanilla path.
    COND_VB_SHOULD(VB_CHANGE_HELD_ITEM_AND_USE_ITEM, CVarGetInteger("gVrItemSelect", 1), {
        int32_t item = va_arg(args, int32_t);
        Player* player = (gPlayState != NULL) ? GET_PLAYER(gPlayState) : NULL;
        if (SelectorModeInPlay() && (sEquipGrace == 0) && (player != NULL) &&
            (Player_ItemToItemAction(item) != player->heldItemAction)) {
            *should = false;
        }
    });
}

static RegisterShipInitFunc initVrItemSelect(RegisterVrItemSelect, { "gVrItemSelect" });
