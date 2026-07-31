#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>
#include <vr_interface.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

VrCombat::TickPath sTickPath[2];

// The soh <-> libultraship physical-combat contract is versioned; refuse to run against a stale
// submodule build rather than misbehave subtly (the check is one call, cached forever).
bool InterfaceOk() {
    static bool checked = false;
    static bool ok = false;
    if (!checked) {
        checked = true;
        ok = (VR_PhysGetInterfaceVersion() == VR_PHYS_INTERFACE_VERSION);
        if (!ok) {
            SPDLOG_ERROR("[VrCombat] libultraship physical-combat interface v{} != expected v{} - "
                         "stale submodule build? Physical combat disabled.",
                         VR_PhysGetInterfaceVersion(), VR_PHYS_INTERFACE_VERSION);
        }
    }
    return ok;
}

bool sWasActive = false;

// Once per 20 Hz game tick, right after the player actor updates (and after the tick's
// CollisionCheck pass at the top of Play_Update, but before Play_Draw builds the display list):
//  1. read back last tick's melee quad results (hit haptics, one-hit-per-swing),
//  2. drain the headset-rate hand paths accumulated since the last tick, so the coming draw
//     feeds colliders from the freshest possible motion.
// The drain consumes the samples, so it happens exactly once here no matter how many systems
// read the snapshot.
void OnPlayerUpdateVrCombat() {
    if (gPlayState == NULL) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL) {
        return;
    }

    const bool active = VrCombat_Active();
    if (active) {
        VrCombat::Swing_OnPlayerUpdate(gPlayState, player);
    } else if (sWasActive) {
        VrCombat::Swing_Deactivate(gPlayState, player);
    }
    sWasActive = active;

    for (int hand = 0; hand < 2; hand++) {
        sTickPath[hand].count = VR_GetHandPath(hand, sTickPath[hand].samples, ARRAY_COUNT(sTickPath[hand].samples));
    }
}

} // namespace

extern "C" bool VrCombat_Active(void) {
    if (!CVarGetInteger("gVrPhysCombat", 0) || !InterfaceOk()) {
        return false;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson()) {
        return false;
    }
    if (!GameInteractor::IsSaveLoaded(true) || gPlayState == NULL) {
        return false;
    }
    // Any frame the game owns Link is a vanilla frame. This list grows as milestones need finer
    // exclusions (horse, swimming, crawlspaces, shooting galleries).
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || Player_InBlockingCsMode(gPlayState, player)) {
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->csCtx.state != CS_STATE_IDLE) {
        return false;
    }
    return true;
}

namespace VrCombat {

const TickPath& GetTickPath(int hand) {
    static const TickPath kEmpty = {};
    if (hand < 0 || hand > 1) {
        return kEmpty;
    }
    return sTickPath[hand];
}

} // namespace VrCombat

static void RegisterVrCombat() {
    // Always registered (not conditioned on gVrPhysCombat): flipping the toggle off mid-swing
    // must still run the falling-edge cleanup, or meleeWeaponState would stay stuck nonzero —
    // a permanently-hot vanilla sword collider. The body gates itself via VrCombat_Active().
    COND_HOOK(OnPlayerUpdate, true, OnPlayerUpdateVrCombat);
}

static RegisterShipInitFunc initFunc(RegisterVrCombat, {});
