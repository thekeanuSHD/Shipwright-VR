# VR Physical Combat — Handoff

Branch `MotionControls-2` (submodule `libultraship` on `vr-port`). This documents the
physics-based VR combat overhaul: what exists, how it works, how to test it, and what's next.
The original approved plan lives at `C:\Users\William\.claude\plans\ok-take-a-look-temporal-stallman.md`
(milestones M0–M8).

## What this is

Replaces OoT's animation-driven combat with physical VR melee behind the `gVrPhysCombat`
toggle (VR Settings → Physical Combat): swings damage by real hand velocity (velocity tiers map
to vanilla slash/jump-slash damage classes, so every enemy's authored damage table still works),
the blade physically collides with the world, and enemies/NPCs react like punching bags.
Design references: Ancient Dungeon VR (velocity gating), Until You Fall (hit feel with
animation-driven enemies), GORN/Blade & Sorcery (limb manipulation).

## Milestone state

- **M0 Foundations** — DONE, committed. Hand velocities (XrSpaceVelocity), haptics, sim
  skeleton, debug overlay.
- **M1 Sword damage, rigid blade** — DONE, committed. Swing tiers, swept damage quads,
  meleeWeaponState mirror (enemy windup AI + Ganondorf final blow), B-attack suppression.
- **M2 Inertia + blade-stop** — DONE, committed (`577ec8996` + libultraship `f863c875`).
- **M3 Knockback + hit feel** — IN PROGRESS, **uncommitted** (everything below since
  `577ec8996`). Working: knockback, press-push, limb puppetry (translate + rotate + resist),
  flesh materials, pacifist test mode. Remaining: AT_BOUNCED armor clank, Lizalfos/Stalfos
  AI regression pass, user's collider-shift defaults, pacifist tooltip fix (says "damage still
  lands" — it doesn't: frozen enemies register no hurtboxes).
- **M4 Physical shield, M5 pots, M6 bow, M7 two-hand heavies, M8 polish** — not started.

## Architecture (three layers)

### Layer 0 — libultraship sim (`libultraship/src/fast/vr_physics.cpp`, headset rate)

Self-contained held-object simulation in RAW tracking space (meters), stepped per XR frame
from `vr_begin_frame`. C ABI in `include/vr_interface.h`, **interface version 11** (asserted
game-side at init; bump on any struct change).

- **Position-based solver, not impulses**: a spring-damper (implicit/backward-Euler,
  unconditionally stable) drives a collision-free TARGET pose; the blade walks toward it in
  substeps ≤ blade radius and is depenetrated geometrically. No stored contact energy → no
  ring, no jitter, pressing harder changes nothing. (History: an impulse solver preceded this
  and was unfixable — "the spring and contacts are adversaries".)
- **Pivot-only response** (`pivotOnly`): contacts ROTATE the blade about the grip, never
  translate it — the hand is never pushed back. Contacts without a usable lever arm are let
  through (a dead-straight stab with the hand pushed past a wall clips through by design).
- **Flat-blade collider**: spine + two long edges (each `bladeRadiusM` thick) converging to a
  point over `tipTaperFrac` of the length; width vector from the damage cross-quad, rolled by
  Blade Collider Roll and shifted by the three Collider Shift sliders.
- **Convex-edge (ledge lip) handling**: triangle-adjacency "ghost collision" filtering (the
  Box2D/Bullet lesson): behind-the-plane contacts near a boundary edge are vetoed when the
  neighbouring face shows the blade is wrapping a lip; blade-vs-triangle-EDGE contacts give the
  continuous rounded-lip rest. Never remove these — the ledge-lip scenario in the suite guards
  them.
- **Swing-through** (`passthroughSpeedMps`, default 2.2 = the Hit At tier): above this
  mid-blade speed (from HAND velocities, so a snagged blade can't gate its own release; 70%
  hysteresis) contacts disengage — committed swings cut through. While passing through
  something, **cut drag** (`cutDragFlesh`/`cutDragWorld`) holds the blade back
  (fraction of catch-up distance retained per 90 Hz step, frame-rate normalized) with
  continuous haptics.
- **Position-based friction**: a fraction of the HAND-driven tangential slide is removed per
  step at each contact (measuring the blade's own motion instead feeds back and winds up —
  don't).
- **Visual-mesh collision (experimental, ON by default — `gVrPhysVisualMesh`)**: the
  interpreter harvests every triangle it draws near the weapon hand in WORLD space
  (model matrices are world-space here because view+projection live in the P matrix — see
  `interpreter.cpp` `GfxSpVertex`/`GfxSpTri1`) and the sim collides with the nearest 32:
  the geometry you SEE, interpolated enemy poses included. Includes back-face rejection,
  sticky selection, cross-eye dedup (all three required — removing any brings back flat-wall
  jitter). Exclusions via the **`G_VRPHYS_MASK` display-list marker opcode `0x4b`**
  (NOT 0x4a — that's `G_SETTILESIZE_LERP`; colliding with it desyncs scrolling-texture room
  DLs and crashes): w1 = 0 unmask / 1 mask / 2 flesh-material on / 3 flesh off. Masked:
  player's own model, sword trail (EffectBlure), debug overlay, in-world lock-on
  reticle/arrow (`Attention_Draw`), ITEMACTION+MISC actors (Navi, sparkles, pickups).
  Depth-write is required for harvest (kills particles/glows/water generically). Enemy/NPC/BOSS
  actor draws are bracketed as FLESH → silent contacts, no sparks.
- **Flight recorder**: `gVrPhysLog` + menu button dumps per-step CSV
  (`x64/Debug/vr_phys_log.csv`); `tools/vrphys-suite/analyze_log.py` attributes jitter
  (spring vs contacts vs discovery churn).

### Layer 1 — soh bridges (`soh/soh/Enhancements/vr-combat/`, 20 Hz)

- `VrCombat.cpp/.h` — `VrCombat_Active()` master predicate, interface-version check, tick
  hook, mesh mask/flesh marker emitters, C shims for decomp patches.
- `VrSwing.cpp` — everything melee: swing tiers (blade-midpoint speed from runtime-filtered
  velocities + min hand speed floor), swept damage quads + strike quads, contact gathering
  (scene polys with back-face rejection & sticky ranking [collision-mesh mode only], dyna
  probes, enemy AC capsules/spheres, AC_HARD), material impact SFX, knockback/press impulses
  (QUEUED at draw, applied in the player's update — displacement written at draw time is
  zeroed by the damage pass before consumption; vanilla `colChkInfo.mass` is IGNORED — it
  means "can't be walk-pushed", a Stalchild is MASS_HEAVY), **limb puppet system** (below),
  pacifist mode, per-sword blade length, debug accessors.
- `VrCombatDebug.cpp` — overlay: contact prims, harvested-mesh magenta wireframe, cyan blade
  rectangle, puppet limb dots (white = tracked, red+bigger = being displaced), velocity
  arrows, damage quads. The overlay masks itself from the harvest (it once collided with its
  own blade outline).
- Decomp patches (all `// SOH [VR]`): `z_player.c` (B/spin suppression, mask push around
  Player_Draw, damage-cancel OR), `z_player_lib.c` (L_HAND feed seam), `z_eff_blure.c`
  (trail mask), `z_actor.c` (harvest masks/flesh by category, Attention_Draw mask),
  `z_skelanime.c` (**10 limb-warp hook sites** across ALL skeleton draw variants — Opa, Lod,
  FlexOpa, and the Gfx* `SkelAnime_DrawFlex` used via `func_80034BA0/CC4` by most NPCs; if an
  actor type ignores the puppet, its draw path is missing hooks — check here first).

### Limb puppetry (the "ragdoll") — `VrSwing.cpp`

Per tracked actor (nearest 4 enemies/NPCs, from AC list, OC list for NPCs, or the actor list
in pacifist mode): limb world positions are recorded during draw by the warp hooks (the clean
animated pose, BEFORE offsets — no feedback), then each tick every limb is solved against the
blade as a sphere (`gVrPhysLimbRadius`). Blade intrusion pushes the limb out (rides the blade
while touching) and ROTATES it about its joint (bone direction approximated from the next
limb's position — skeleton arrays are parent-child ordered; angle = displacement/lever).
`gVrPhysLimbResist` makes limbs yield partially and lag. Offsets apply at draw time only —
animation, hitboxes, and AI never see them. Hits kick limb offsets through the same channel.
NPCs: limbs move, bodies stay planted (no knockback/press impulses, no contact prim — their
bump cylinder is far fatter than the visible body).

Frame interpolation is SAFE for all of this: it snapshots final limb matrices (after our
mutation) and lerps them — verified by deep-dive; the `gVrPhysFlinchTest <units>` console
lever (lifts all enemy/NPC limbs) proves the pipeline end-to-end in seconds.

### Layer 2 — menu (`soh/soh/SohGui/SohMenuVRSettings.cpp`)

VR Settings → Physical Combat: speed tiers, blade collider (lengths per sword, width,
thickness, roll −90° default, 3 shift sliders, tip taper), swing-through speed, cut
resistance ×2, knockback, blade push, hit flinch amount, limb resistance, blade physics
(snappiness ×2, contact reach, impact tolerance, bounce, friction), visual-mesh checkbox,
diagnostics (debug overlay, physics log, pacifist mode, haptic tests, live readout).

## CVars quick reference (defaults)

`gVrPhysCombat` 0 · `gVrPhysCombatDebug` 0 · `gVrPhysVisualMesh` 1 · `gVrPhysPacifist` 0
Speeds: Arm 1.2 / Hit 2.2 / Heavy 4.0 / ReArm 0.8 / MinHand 0.6 (m/s)
Blade: LenKokiri 30 / LenMaster 40 / LenBiggoron 55 / Width 4 / Thickness 0.4 / Roll −90 /
ShiftFwd 0 / ShiftEdge 0 / ShiftFlat 0 / TipTaper 0.2
Physics: Sword1HFreq 14 / SwordAngFreq 30 / MaxAccel 400 (**do not raise — jitter**) /
MaxAngAccel 3000 / ContactReach 1.6 / TouchTolerance 0.3 / Bounce 0.25 / Friction 0.3 /
PivotOnly 1
Feel: PassthroughSpeed 2.2 / CutDragFlesh 0.55 / CutDragWorld 0.2 / KnockbackScale 1.0 /
KnockbackCap 8.0 / PressPush 2.5 / FlinchAmount 18 / LimbResist 0.5 / LimbRadius 9 /
LimbPushMax 22 / MeshRadius 150
Debug: `gVrPhysFlinchTest` 0 (console only; N = lift all limbs N units) / `gVrPhysLog` 0

## Testing methodology (IMPORTANT — this is how every physics bug here got solved)

**Never guess-and-headset-test.** Two tools:

1. **Numerical suite** — `tools/vrphys-suite/` (`build_suite.bat` then `vrphys_suite.exe`).
   Compiles `vr_physics.cpp` standalone; 22+ scenarios (concave corners, flat-wall shoves,
   wrist rotation, convex LEDGE LIP, flat/edge-on blade rest, pass-through sweep) at
   72/90/120 Hz. A healthy sim reads ~0.000 mm HF jitter, 0.00 mm grip dev, zero
   zero-contact steps on held cases. Extend it FIRST for any new physics work; geometry
   classes fail independently (concave ≠ seam ≠ convex lip).
2. **Flight recorder** — enable `gVrPhysLog`, reproduce in-headset, dump CSV, run
   `analyze_log.py`: attributes jitter to spring/contacts/discovery churn and prints contact
   normal histograms (opposing normals = gather bug, churn = selection instability).

Debug overlay legend: blue-gray tris = collision-mesh prims · magenta wires = harvested
visual-mesh tris actually fed to the solver · cyan rectangle = the physical blade · yellow
markers = active contacts · white/red dots = puppet limbs (red = being displaced) · red
quads = damage quads.

## Known issues / quirks

- Puppet limb offsets translate+rotate but don't constrain children → visible joint gaps and
  "head separation" on hard pushes. Accepted for now; proper fix = jointTable rotation
  offsets (see the frame-interpolation/skelanime analysis in the session history — jointTable
  edits post-update render fine; never touch jointTable[0], it's root translation).
- Straight stabs with the hand pushed past a wall clip through (pivot-only geometry: nothing
  can shorten the blade). Accepted trade-off; ghost-blade option discussed if it bothers.
- Visual-mesh mode: harvested contacts carry generic materials except enemy/NPC flesh;
  collision-mesh material sounds (wood/dirt/stone) only apply in collision-mesh mode or via
  AC_HARD colliders. Could be fixed by sampling the collision mesh under contact points.
- Corner/edge churn in visual-mesh mode is much improved but can still flicker (32-tri
  selection membership). Collision-mesh mode (`gVrPhysVisualMesh 0`) is the stable fallback.
- Frozen (pacified) enemies take no damage (no hurtbox registration) — tooltip wrongly says
  otherwise; fix pending.
- `Boss_Sst` (Bongo hand) walks its joint table up to 4× per draw — puppet warp is
  save/restore idempotent so it's safe, but remember if changing the warp to additive.
- Build: `--parallel 6` normally; C1060 "out of heap space" = machine memory pressure (game
  + SteamVR running) — check free RAM, retry, or drop to `--parallel 2`. LNK1168 = game still
  running. Touching `gbi.h`/`interpreter.h` = near-full rebuild.

## Commit state & immediate next steps

Committed through M2 (`577ec8996` soh / `f863c875` libultraship, both local-only, not pushed).
**All M3 work is uncommitted** across both repos (VrSwing/VrCombat/debug, z_actor, z_skelanime,
z_eff_blure, interpreter, vr_physics, menu, this file, tools/).

1. Get the user's final Collider Shift numbers → bake as defaults (roll −90 already done).
2. Fix pacifist tooltip.
3. Commit M3 (submodule first, then soh — same pattern as previous commits).
4. M3 remainder: AT_BOUNCED armor clank + heavy haptic; knockback regression pass on
   parry-timing enemies (Lizalfos, Stalfos); Dark Link sanity check.
5. Then M4 (physical shield) per the plan file.

## Working agreements (user preferences)

- Claude builds (Debug, target soh, background); only surface build ERRORS — the user tests
  in-headset and reports back. Don't confirm clean builds.
- Root-cause over patching: when something misbehaves, reproduce numerically or capture a
  log; research online for established solutions (ghost collisions, VR melee design) rather
  than inventing.
- The user tunes feel via menu sliders and reports numbers to bake as defaults.
- Ask when direction is ambiguous ("ask me any question you have, don't make guesses").
