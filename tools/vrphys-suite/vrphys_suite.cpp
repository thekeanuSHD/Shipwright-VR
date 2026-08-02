// Scenario suite for the VR held-object physics. Each case drives the hand for 300 steps and
// reports the mean per-step blade movement once the hand is held still (or moving smoothly) —
// a stable sim should read ~0 mm for held cases and show no high-frequency component otherwise.
#include "fast/vr_physics.cpp"

#include <stdio.h>
#include <math.h>

static const float kScale = 35.0f;

static VrPhysContactPrim MakeTri(float ax, float ay, float az, float bx, float by, float bz, float cx,
                                 float cy, float cz, int id) {
    VrPhysContactPrim p{};
    p.type = VRPHYS_PRIM_TRI;
    p.a[0] = ax; p.a[1] = ay; p.a[2] = az;
    p.b[0] = bx; p.b[1] = by; p.b[2] = bz;
    p.c[0] = cx; p.c[1] = cy; p.c[2] = cz;
    p.radius = 0.0f;
    p.id = id;
    return p;
}

// Deterministic pseudo-noise standing in for controller tracking jitter (~0.3 mm).
static float Noise(int i, int salt) {
    const int n = (i * 1103515245 + salt * 12345 + 7919) & 0x7FFFFFFF;
    return ((float)(n % 2000) / 1000.0f - 1.0f) * 0.0003f;
}

struct Result {
    float heldJitterMm;
    float maxStepMm;
    int minContacts;
    int maxContacts;
    int zeroContactSteps; // steps with NO constraint while pressed — the lunge window
    float gripDevMm;      // max |grip - hand| over the hold: pivot-only promises ~0
};

// mode: 0 = tip into corner, 1 = blade CENTER across the corner edge, 2 = slide along the corner,
//       3 = FLAT WALL with the hand driven deep PAST the surface (what a player actually does:
//           the real hand is unconstrained and ends up inside the wall, so the spring runs at its
//           acceleration ceiling the whole time), 4 = same but the wrist also rotates
static float gTrailDeg = 0.0f; // blade angle lag vs hand orientation at end of run (friction probe)
static float gTipLagMm = 0.0f; // tip lag along the slide direction at end of run (friction probe)

static Result RunCase(float hz, int mode, bool handNoise, float angFreq = 30.0f,
                      float maxAngAccel = 3000.0f, float maxAccel = 400.0f, float friction = 0.15f,
                      float passSpeed = 0.0f) {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };

    VrPhysContactPrim prims[4];
    if (mode == 8 || mode == 10 || mode == 11) {
        // Ledge: floor at y=0 for x <= 35, then a sheer drop (side face at x=35, facing +x).
        prims[0] = MakeTri(-70, 0, -70, -70, 0, 70, 35, 0, -70, 10);
        prims[1] = MakeTri(35, 0, 70, 35, 0, -70, -70, 0, 70, 10);
        prims[2] = MakeTri(35, 0, -70, 35, 0, 70, 35, -70, -70, 11);
        prims[3] = MakeTri(35, -70, 70, 35, -70, -70, 35, 0, 70, 11);
    } else {
        prims[0] = MakeTri(35, -70, -70, 35, -70, 70, 35, 70, -70, 1);
        prims[1] = MakeTri(35, 70, 70, 35, 70, -70, 35, -70, 70, 1);
        prims[2] = MakeTri(-70, -70, 35, -70, 70, 35, 70, -70, 35, 2);
        prims[3] = MakeTri(70, 70, 35, 70, -70, 35, -70, 70, 35, 2);
    }
    vrphys_set_contact_prims(prims, 4);

    // Mirrors the game's live defaults (VrSwing.cpp) so the harness tests what actually ships.
    VrPhysObjectDesc desc{};
    desc.primary_hand = 1;
    desc.secondary_hand = -1;
    desc.lin_freq_hz = 14.0f;
    desc.lin_zeta = 1.0f;
    desc.ang_freq_hz = angFreq;
    desc.ang_zeta = 1.0f;
    desc.max_accel_mps2 = maxAccel;
    desc.blade_radius_m = 0.4f / kScale;
    desc.speculative_m = 1.6f / kScale;
    desc.touch_tolerance_m = 0.3f / kScale;
    desc.max_ang_accel = maxAngAccel;
    desc.pivot_only = true; // mirrors the game default (gVrPhysPivotOnly 1)
    // Flat blade: 4 game units wide (the Blade Width default), width along local Z,
    // pointed over the last 20% — mirrors the game's flat-rectangle collider.
    desc.grip_local_edge_m[2] = 2.0f / kScale;
    desc.tip_taper_frac = 0.2f;
    desc.passthrough_speed_mps = passSpeed;
    if (mode == 1 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 8 ||
        mode == 9 || mode == 10 || mode == 11 || mode == 12) {
        // Blade lies along +X, pointing at wall A.
        desc.grip_local_tip_m[0] = 1.13f;
    } else {
        desc.grip_local_tip_m[0] = 0.8f;
        desc.grip_local_tip_m[2] = 0.8f;
    }
    desc.contact_enabled = true;
    desc.restitution = 0.25f;
    desc.friction = friction;
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    const float dt = 1.0f / hz;
    const uint64_t dtns = (uint64_t)(dt * 1e9f);
    uint64_t t = 0;
    float hx = 0, hy = 0, hz_ = 0;
    if (mode == 1) {
        hx = 0.30f;
        hz_ = 0.50f; // start near wall B, blade pointing +X toward wall A
    } else if (mode == 3 || mode == 4) {
        hx = -0.60f; // blade tip starts short of wall A (x = 1.0)
        hz_ = -0.50f;
    } else if (mode == 5 || mode == 6 || mode == 7) {
        hx = -0.60f;
        hz_ = (mode == 7) ? 0.30f : -0.50f; // mode 7 also jams into wall B
    } else if (mode == 8) {
        hx = -0.55f;
        hy = 0.45f; // blade pitched 25 deg down: tip drags the floor, then crosses the lip
    } else if (mode == 9) {
        hx = 0.0f; // tip 13cm past the wall plane: pressed via pivot from the start
        hz_ = -0.50f;
    } else if (mode == 10 || mode == 11) {
        hx = -0.55f; // over the mode-8 floor, blade level: press straight down onto it
        hy = 0.05f;  // just above the surface; the press drives 19cm below it
        hz_ = -0.30f;
    } else if (mode == 12) {
        hx = 0.20f; // tip 33cm past the wall plane: a fast sweep must pass, a rest must pivot
        hz_ = -1.20f;
    }
    float quat[4] = { 0, 0, 0, 1 };
    if (mode == 8) {
        const float half = -25.0f * 3.14159265f / 180.0f * 0.5f;
        quat[2] = sinf(half);
        quat[3] = cosf(half);
    } else if (mode == 11) {
        // EDGE-ON: roll 90 deg about the blade axis so the thin edge faces the floor.
        const float half = 90.0f * 3.14159265f / 180.0f * 0.5f;
        quat[0] = sinf(half);
        quat[3] = cosf(half);
    }

    Result r{ 0, 0, 99, 0, 0, 0.0f };
    float prev[3] = { 0, 0, 0 };
    float pd[3] = { 0, 0, 0 };
    bool haveD = false;
    bool havePrev = false;
    double sum = 0;
    int samples = 0;
    const int drive = 90;

    for (int step = 0; step < 300; step++) {
        if (step < drive) {
            if (mode == 1) {
                hz_ += 0.006f; // press the blade's SIDE into wall B, tip near the corner
                hx += 0.004f;
            } else if (mode == 3 || mode == 4) {
                hx += 0.010f; // drive the HAND straight on and well past the wall plane
            } else if (mode == 5 || mode == 6 || mode == 7) {
                hx += 0.045f; // SHOVE: hand ends ~2.5 m beyond the wall, an absurd push
                if (mode == 7) {
                    hz_ += 0.010f;
                }
            } else if (mode == 8) {
                hx += 0.010f; // drag forward; the tip passes the lip around step 53
            } else if (mode == 10 || mode == 11) {
                // no drive phase: hold level over the floor, then press down
            } else if (mode == 12) {
                hz_ += 0.030f; // 2.7 m/s lateral sweep, tip buried past the wall plane
            } else {
                hx += 0.006f;
                hz_ += 0.006f;
            }
        } else if (mode == 2) {
            hy += 0.004f; // slide along the corner crease while pressed in
        } else if (mode == 8 && step < 150) {
            hy -= 0.005f; // press down onto the lip once the tip hangs past it
        } else if (mode == 9 && step >= drive) {
            hy += 0.004f; // drag the pressed tip sideways along the wall
        } else if ((mode == 10 || mode == 11) && step < 150) {
            hy -= 0.004f; // press the level blade down onto the floor (ends 30cm below it)
        }
        // Hand velocities must be CONSISTENT with the hand's motion — the runtime reports real
        // measured velocities, and the spring's damping term tracks them. Feeding zeros while
        // the pose moves makes the damper fight the motion, which is a property of the test
        // rather than of the game.
        float ang[3] = { 0, 0, 0 };
        if (mode == 4 || mode == 6 || mode == 7) {
            // Wrist rotation while pressed: yaw back and forth ~0.3 rad at ~1.5 Hz.
            const float w = 9.4f;
            const float a = 0.3f * sinf((float)step * dt * w);
            quat[0] = 0.0f;
            quat[1] = sinf(a * 0.5f);
            quat[2] = 0.0f;
            quat[3] = cosf(a * 0.5f);
            ang[1] = 0.3f * w * cosf((float)step * dt * w); // d(angle)/dt about Y
        }
        float pos[3] = { hx, hy, hz_ };
        if (handNoise) {
            pos[0] += Noise(step, 1);
            pos[1] += Noise(step, 2);
            pos[2] += Noise(step, 3);
        }
        float lin[3] = { 0, 0, 0 };
        if (step < drive) {
            if (mode == 1) {
                lin[0] = 0.004f / dt;
                lin[2] = 0.006f / dt;
            } else if (mode == 3 || mode == 4) {
                lin[0] = 0.010f / dt;
            } else if (mode == 5 || mode == 6 || mode == 7) {
                lin[0] = 0.045f / dt;
                if (mode == 7) {
                    lin[2] = 0.010f / dt;
                }
            } else if (mode == 8) {
                lin[0] = 0.010f / dt;
            } else if (mode == 10 || mode == 11) {
                // stationary during the (empty) drive phase
            } else if (mode == 12) {
                lin[2] = 0.030f / dt;
            } else {
                lin[0] = 0.006f / dt;
                lin[2] = 0.006f / dt;
            }
        } else if (mode == 2) {
            lin[1] = 0.004f / dt;
        } else if (mode == 8 && step < 150) {
            lin[1] = -0.005f / dt;
        } else if (mode == 9 && step >= drive) {
            lin[1] = 0.004f / dt;
        } else if ((mode == 10 || mode == 11) && step < 150) {
            lin[1] = -0.004f / dt;
        }
        t += dtns;
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);
#ifdef VRPHYS_TRACE_CORR
        g_vrphys_trace = (mode == 8 && hz > 80.0f && hz < 100.0f && !handNoise && step == 160);
        if (g_vrphys_trace) printf("  --- step %d ---\n", step);
#endif
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);

        float opos[3], oq[4], ov[3], oa[3];
        vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, opos, oq, ov, oa);
        float cp[12], cn[12];
        const int nc = vrphys_get_object_contacts(VRPHYS_SLOT_WEAPON, cp, cn, 4);

        // JITTER = the high-frequency component only: the discrete second difference of position
        // (how much the motion CHANGES direction/speed per step). Total per-step motion is the
        // wrong measure whenever the hand is deliberately moving or rotating — smooth tracking of
        // a moving hand would score as "jitter" and mask what we're hunting.
        if (havePrev && step > 20) {
            const float dx = (opos[0] - prev[0]) / kScale;
            const float dy = (opos[1] - prev[1]) / kScale;
            const float dz = (opos[2] - prev[2]) / kScale;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
            if (d > r.maxStepMm) {
                r.maxStepMm = d;
            }
            if (haveD) {
                const float jx = dx - pd[0], jy = dy - pd[1], jz = dz - pd[2];
                const float j = sqrtf(jx * jx + jy * jy + jz * jz) * 1000.0f;
                if (step >= 150) {
                    sum += j;
                    samples++;
                }
            }
            pd[0] = dx; pd[1] = dy; pd[2] = dz;
            haveD = true;
        }
        if (step >= 120) {
            if (nc < r.minContacts) r.minContacts = nc;
            if (nc > r.maxContacts) r.maxContacts = nc;
            if (nc == 0) r.zeroContactSteps++;
        }
        if (step >= 150) {
            const float gx = opos[0] / kScale - pos[0];
            const float gy = opos[1] / kScale - pos[1];
            const float gz = opos[2] / kScale - pos[2];
            const float gd = sqrtf(gx * gx + gy * gy + gz * gz) * 1000.0f;
            if (gd > r.gripDevMm) r.gripDevMm = gd;
        }
#ifdef TRACE_MODE8
        if (mode == 8 && hz > 80.0f && hz < 100.0f && !handNoise && ((step >= 40 && step % 10 == 0) || (step >= 118 && step <= 126))) {
            const float bl = 1.13f;
            float fwd[3] = { 1, 0, 0 };
            // rotate local +x by oq
            const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
            const float tx2 = 2*(qy*fwd[2]-qz*fwd[1]), ty2 = 2*(qz*fwd[0]-qx*fwd[2]), tz2 = 2*(qx*fwd[1]-qy*fwd[0]);
            const float dx = fwd[0]+qw*tx2+(qy*tz2-qz*ty2), dy = fwd[1]+qw*ty2+(qz*tx2-qx*ty2), dz = fwd[2]+qw*tz2+(qx*ty2-qy*tx2);
            printf("  s%3d hand(%.3f,%.3f) root(%.3f,%.3f) tip(%.3f,%.3f) nc=%d\n",
                   step, hx, hy, opos[0]/kScale, opos[1]/kScale,
                   opos[0]/kScale + dx*bl, opos[1]/kScale + dy*bl, nc);
        }
#endif

        havePrev = true;
        for (int i = 0; i < 3; i++) prev[i] = opos[i];
        if (step == 299) {
            // Blade trail: angle between simulated and hand orientation (friction probe).
            const float dq = fabsf(oq[0] * quat[0] + oq[1] * quat[1] + oq[2] * quat[2] + oq[3] * quat[3]);
            gTrailDeg = 2.0f * acosf(dq > 1.0f ? 1.0f : dq) * 180.0f / 3.14159265f;
            // Tip lag along the slide direction (y): how far the dragged tip trails the hand.
            const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
            const float fx = 1, fy = 0, fz = 0;
            const float tx2 = 2 * (qy * fz - qz * fy), ty2 = 2 * (qz * fx - qx * fz),
                        tz2 = 2 * (qx * fy - qy * fx);
            const float dy = fy + qw * ty2 + (qz * tx2 - qx * tz2);
            gTipLagMm = (hy - (opos[1] / kScale + dy * 1.13f)) * 1000.0f;
        }
    }
    r.heldJitterMm = (float)(sum / (samples ? samples : 1));
    return r;
}

int main() {
    struct Case {
        const char* name;
        float hz;
        int mode;
        bool noise;
    };
    const Case cases[] = {
        { "tip into corner        @ 90Hz", 90.0f, 0, false },
        { "tip into corner        @ 72Hz", 72.0f, 0, false },
        { "tip into corner        @120Hz", 120.0f, 0, false },
        { "BLADE CENTER at corner @ 90Hz", 90.0f, 1, false },
        { "BLADE CENTER at corner @ 72Hz", 72.0f, 1, false },
        { "BLADE CENTER + hand noise    ", 90.0f, 1, true },
        { "slide along corner     @ 90Hz", 90.0f, 2, false },
        { "slide along corner + noise   ", 90.0f, 2, true },
        { "FLAT WALL hand pushed through", 90.0f, 3, false },
        { "FLAT WALL through @120Hz     ", 120.0f, 3, false },
        { "FLAT WALL through + noise    ", 90.0f, 3, true },
        { "FLAT WALL + wrist rotation   ", 90.0f, 4, false },
        { "FLAT WALL + rotation @120Hz  ", 120.0f, 4, false },
        { "SHOVE 2.5m past wall         ", 90.0f, 5, false },
        { "SHOVE 2.5m + rotation        ", 90.0f, 6, false },
        { "SHOVE 2.5m + rotation @120   ", 120.0f, 6, false },
        { "SHOVE into CORNER + rotation ", 90.0f, 7, false },
        { "LEDGE LIP drag over @ 90Hz   ", 90.0f, 8, false },
        { "LEDGE LIP drag over @ 72Hz   ", 72.0f, 8, false },
        { "LEDGE LIP drag over + noise  ", 90.0f, 8, true },
        { "FLAT side pressed on floor   ", 90.0f, 10, false },
        { "FLAT side on floor + noise   ", 90.0f, 10, true },
        { "EDGE-ON pressed on floor     ", 90.0f, 11, false },
        { "EDGE-ON on floor + noise     ", 90.0f, 11, true },
    };
    const Case passCases[] = {
        { "SWEEP fast thru wall, then rest", 90.0f, 12, false },
    };
    printf("%-32s | HF jitter   | max step | grip dev  | contacts | zero-contact steps\n", "case");
    printf("---------------------------------+-------------+----------+-----------+----------+-------------------\n");
    for (const Case& c : cases) {
        const Result r = RunCase(c.hz, c.mode, c.noise);
        printf("%-32s | %7.3f mm  | %6.2f mm | %6.2f mm | %d-%d      | %d\n", c.name, r.heldJitterMm,
               r.maxStepMm, r.gripDevMm, r.minContacts, r.maxContacts, r.zeroContactSteps);
    }
    printf("\nHF jitter   should be ~0 for held cases; sliding cases move smoothly by design.\n");

    printf("\n=== PASS-THROUGH (threshold 2.2 m/s; sweep at 2.7 must NOT contact, rest must) ===\n");
    for (const Case& c : passCases) {
        const Result r = RunCase(c.hz, c.mode, c.noise, 30.0f, 3000.0f, 400.0f, 0.15f, 2.2f);
        printf("%-32s | HF %7.3f mm | grip dev %5.2f mm | contacts %d-%d | zero-contact %d\n",
               c.name, r.heldJitterMm, r.gripDevMm, r.minContacts, r.maxContacts, r.zeroContactSteps);
    }

    printf("\n=== FRICTION A/B (tip pressed on flat wall, hand drags sideways at 0.36 m/s) ===\n");
    const float frics[] = { 0.0f, 0.15f, 0.3f, 0.6f, 0.9f };
    for (float f : frics) {
        const Result fr = RunCase(90.0f, 9, false, 30.0f, 3000.0f, 400.0f, f);
        printf("  friction %.2f | tip lag %7.2f mm | HF %7.3f mm | grip dev %5.2f mm\n", f,
               gTipLagMm, fr.heldJitterMm, fr.gripDevMm);
    }

    printf("\n=== OLD defaults (the build that felt good) vs NEW defaults ===\n");
    printf("config                        | rot-press HF | rot zero | ctr-corner HF | ctr zero\n");
    printf("------------------------------+--------------+----------+---------------+---------\n");
    struct Cfg {
        const char* label;
        float angF;
        float maxAngA;
        float maxAcc;
    };
    const Cfg cfgs[] = {
        { "OLD  14 /   500 /  400", 14.0f, 500.0f, 400.0f },
        { "NEW  25 / 20000 / 2000", 25.0f, 20000.0f, 2000.0f },
        { "only maxAcc raised    ", 14.0f, 500.0f, 2000.0f },
        { "only angFreq raised   ", 25.0f, 500.0f, 400.0f },
        { "only angClamp raised  ", 14.0f, 20000.0f, 400.0f },
        { "CAND 25 /  3000 /  400", 25.0f, 3000.0f, 400.0f },
        { "CAND 30 /  3000 /  400", 30.0f, 3000.0f, 400.0f },
        { "CAND 25 /  3000 /  600", 25.0f, 3000.0f, 600.0f },
        { "CAND 25 /  3000 /  800", 25.0f, 3000.0f, 800.0f },
    };
    for (const Cfg& cf : cfgs) {
        const Result rot = RunCase(90.0f, 4, false, cf.angF, cf.maxAngA, cf.maxAcc);
        const Result ctr = RunCase(90.0f, 1, false, cf.angF, cf.maxAngA, cf.maxAcc);
        printf("%-29s | %9.2f mm | %8d | %10.2f mm | %d\n", cf.label, rot.heldJitterMm,
               rot.zeroContactSteps, ctr.heldJitterMm, ctr.zeroContactSteps);
    }
    return 0;
}
