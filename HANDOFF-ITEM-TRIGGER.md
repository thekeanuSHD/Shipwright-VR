# Handoff: trigger-activates-held-item (Alyx selector mode)

Branch `MotionControls-2` (submodule `libultraship` on `vr-port`). NOTE: the working tree has a
large UNCOMMITTED batch (item selector, shield work, turning, celestial billboards, projectile
fire fixes — everything since the bone-capsule commit `3d2df38a2` / LUS `7152fa93`). Build:
`cmake --build build\x64 --config Debug --target soh --parallel 6` (new source files need a
`cmake build\x64` regen first — the glob has no CONFIGURE_DEPENDS). Working agreements:
Claude builds (background, surface errors only), the user tests in-headset and reports back,
ask instead of guessing, root-cause over patching. `// SOH [VR]` marks decomp patches.

## The feature

When the Half-Life-Alyx-style item selector is enabled (`gVrItemSelect`, default on):
items are NOT activated by their C buttons anymore. Instead: select the item with the
selector (hold input → flick → release equips it), then press the TRIGGER of whatever hand
the item is HELD in to activate/use it. I.e. the selector equips; the trigger fires/uses.

## How things work today (read these files)

### `soh/soh/Enhancements/vr-combat/VrItemSelect.cpp` — the selector
Hold `gVrItemSelInput` (default stick click) on `gVrItemSelHand` (default sword hand) →
compass of equipped item icons → flick ≥ `gVrItemSelDistance` cm → release. `ExecuteSector()`
is where selection becomes action: C-item sectors call
`GameInteractor::RawAction::EmulateButtonPress(BTN_CLEFT/CDOWN/CRIGHT)` (one-frame press
through the real input path), up = `EmulateButtonPress(BTN_B)` if sword sheathed, center =
`Player_UseItem(play, player, ITEM_NONE)` (empty hands). `VrItemSelect_ConsumesInput()` makes
the selector's input DEDICATED — padmgr never feeds it to bindings. While open, the hand's
thumbstick is suppressed at the source (`VR_SetStickSuppressed`).

### `soh/src/code/padmgr.c` — VR controller → N64 pad (~line 323)
`sVrBindCvars`/`sVrBindDefaults`: 6 inputs × 2 hands → N64 button masks (defaults: L trigger=Z,
L grip=R, X=C-left, Y=C-right, L stick=Start; R trigger=B, A=A, B=C-down). The skip list at the
top of the bind loop is the DEDICATION precedent: `VrItemSelect_ConsumesInput(...)` and
`VrCombat_AimTriggerConsumed(...)` both `continue` past an input's binding while their feature
owns it. Right-stick C-buttons exist ONLY in flat-screen menus and third person. The new
feature will need the held-item hand's trigger dedicated the same way (note the conflicts:
R trigger = BTN_B = sword, L trigger = BTN_Z = Z-target).

### `soh/soh/Enhancements/vr-combat/VrCombat.cpp` + `VrCombat.h`
`VrCombat.h` is the C surface for every decomp shim — declarations + docs live there; padmgr
and z_player include it. In VrCombat.cpp: `AimHand()` (the projectile-aiming hand =
`gVrLeftHanded ? RIGHT : LEFT` controller — see hand mapping below), a per-tick trigger
rising-edge sampler (`sProjTriggerPressed`, sampled in `OnPlayerUpdateVrCombat` regardless of
the phys-combat toggle), `VrCombat_ProjectileFirePressed()` (consumed by the bow aim action),
`VrCombat_AimTriggerConsumed()` (dedicates the aim-hand trigger while
`PLAYER_STATE1_READY_TO_FIRE`). The new feature likely GENERALIZES these: "trigger of the
held item's hand activates the item" subsumes projectile trigger-fire.

### `soh/src/overlays/actors/ovl_player_actor/z_player.c` — item activation internals
- `Player_UseItem` (~3455): THE item activation entry. With the item already held, the tail
  branch sets `sUseHeldItem = sHeldItemButtonIsHeldDown = true` — that IS "use/fire the held
  item". Both statics are file-internal, per-frame; `sUseHeldItem` is press-frame-scoped.
- `Player_UpdateItems` (~2655): gates `Player_ProcessItemButtons` on
  `(heldItemAction == itemAction) || SHIELDING` and `!START_CHANGING_HELD_ITEM` — presses die
  during item changes (a click-eating gate we've been bitten by twice).
- `player->heldItemButton`: which button slot (0=B, 1..3=C-left/down/right) the held item came
  from. Emulating THAT button's press = "use held item" through the honest path; the C masks
  are `BTN_B/BTN_CLEFT/BTN_CDOWN/BTN_CRIGHT`, equip slots `gSaveContext.equips.buttonItems[0..3]`.
- Projectile flow (bow/slingshot/hookshot): nock `func_8083442C` (~2702, spawns EN_ARROW as
  child/heldActor), aim upper action `func_808351D4` (~3079) — its fire gate accepts
  `VrCombat_ProjectileFirePressed() || sUseHeldItem || button-release`; post-shot
  `func_808353D8` stays shouldered in VR (`VR_IsInitialized && VR_GetFirstPerson` added to its
  keep-ready gate). `func_80834EB8` (~2976) is the VR bypass that keeps aiming an upper-body
  action so walking continues (never the standstill aim stance).
- Boomerang: `func_80835884`/`func_808358F0` upper actions (~3251+), throw on button release.

### `soh/src/code/z_player_lib.c` — which hand holds what
VR limb override (~1553-1592): Link's L_HAND limb ↔ sword-hand controller
(`gVrLeftHanded ? LEFT : RIGHT`), R_HAND limb ↔ the other. Bow/slingshot/hookshot are
RIGHT-hand models → held in the player's LEFT controller (right-handed default). One-hand
melee + Deku stick + boomerang are LEFT-hand models → the player's RIGHT controller. So "the
trigger of the hand the item is held in" = R_HAND-model items → left trigger, L_HAND-model
items → right trigger. `Player_VrAimHeldProjectile` (~1894) is the aim-ray override.

### `soh/soh/SohGui/SohMenuVRSettings.cpp` — menu
Selector widgets live after the `VrInputBindings` custom widget on `buttonsPath` (search
"Item Select (Half-Life: Alyx Style)"). Tooltips that mention C buttons activating items will
need updating; the bindings-editor defaults may deserve a note when selector mode owns
activation.

### `libultraship/include/vr_interface.h` (only if needed)
`VR_GetControllerButton(hand)` + `VR_BTN_TRIGGER/...` masks, `VR_GetTrigger(hand)` analog,
`VR_SetStickSuppressed`. Trigger edge detection precedent is game-side (VrCombat.cpp), so LUS
changes are probably unnecessary.

## Design notes / decisions for the new chat

- Activation mechanism: for "press trigger → use held item", the honest path is emulating the
  held item's own button (`heldItemButton` → mask) via `EmulateButtonPress`, OR setting the
  use through `Player_UseItem(play, player, heldItemId)`'s held-branch semantics. Emulation
  reuses every vanilla rule (ammo, magic, bottles, ocarina) for free.
- Trigger dedication: while an item is held AND selector mode on, the holding hand's trigger
  must not emit its normal binding (B-sword / Z-target). Follow the
  `VrCombat_AimTriggerConsumed` pattern in padmgr. Decide what happens to sword/Z-target
  access while e.g. a bottle is held (the OTHER hand's trigger still has its binding).
- Should C-button bindings stop activating items entirely in selector mode ("instead of")?
  Probably suppress item C presses in first person while `gVrItemSelect` is on — but the
  pause menu must keep C presses for ASSIGNING items, and third person/menus stay stock
  (precedent: the stick C-button rules in padmgr).
- The existing projectile trigger-fire (`VrCombat_ProjectileFirePressed`, aim hand) overlaps
  this feature — unify rather than stack: bow "activation" = fire, and the first trigger
  press on an un-readied bow should ready/nock it (today the C press does that).
- Items with hold semantics (boomerang aim = hold + release throws) need a decision: trigger
  hold = aim, release = throw, mirroring the vanilla button.
- `gVrItemSelect` off must restore today's behavior exactly.

## CVars in play
`gVrItemSelect` 1 · `gVrItemSelHand` 0 (sword hand) · `gVrItemSelInput` 16 (stick click) ·
`gVrItemSelDistance` 5 cm · `gVrLeftHanded` 0 · `gVrBind*` (padmgr defaults above).

## Recent gotchas worth knowing (all fixed, all instructive)
- `Player_UpdateItems`' gate eats presses when `itemAction` desyncs or during item changes.
- Clicking a held item's button re-marks it "held", which BLOCKED release-to-fire — that was
  the "bow won't shoot" bug; press-to-fire in `func_808351D4` fixed it.
- The un-targeted path used to lower the bow after each shot; VR now stays shouldered.
- One-frame emulated presses (`EmulateButtonPress`) work through the full item system.
