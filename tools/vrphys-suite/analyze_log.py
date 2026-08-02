"""Analyze a vr_phys_log.csv capture and attribute jitter to a specific cause.

Usage: python analyze_log.py <path-to-vr_phys_log.csv>

It answers, for the noisiest window in the capture:
  - is the HAND itself moving (player motion) or held still?
  - how much does the SPRING move the blade per step vs how much do CONTACTS move it back?
  - is the contact SET churning (count/normals/ids changing frame to frame)?
  - is the collision GEOMETRY set churning at the 20 Hz tick (prim hash)?
  - what's the dominant oscillation frequency?
"""
import sys
import math
from collections import Counter


def v(a, b, c):
    return (float(a), float(b), float(c))


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def norm(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def main(path):
    steps = []
    contacts = {}
    scale = 35.0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('#'):
                if 'world_scale=' in line:
                    try:
                        scale = float(line.split('world_scale=')[1].split()[0])
                    except (IndexError, ValueError):
                        pass
                continue
            p = line.split(',')
            if p[0] == 'S':
                steps.append({
                    'i': int(p[1]), 't': int(p[2]), 'dt': float(p[3]),
                    'hand': v(p[4], p[5], p[6]),
                    'spring': v(p[7], p[8], p[9]),
                    'final': v(p[14], p[15], p[16]),
                    'fq': tuple(float(x) for x in p[17:21]),
                    'vel': v(p[21], p[22], p[23]),
                    'root': v(p[27], p[28], p[29]),
                    'tip': v(p[30], p[31], p[32]),
                    'nc': int(p[33]), 'nprims': int(p[34]), 'hash': int(p[35]),
                })
            elif p[0] == 'C':
                contacts.setdefault(int(p[1]), []).append({
                    'n': v(p[3], p[4], p[5]), 'pen': float(p[6]), 'id': int(p[7]),
                    'p': v(p[8], p[9], p[10]),
                })

    if len(steps) < 10:
        print(f"Only {len(steps)} samples — not enough to analyze.")
        return

    n = len(steps)
    print(f"{n} samples, world_scale={scale:.1f} units/m")
    dt = sum(s['dt'] for s in steps) / n
    print(f"mean dt={dt*1000:.2f} ms  ({1/dt:.0f} Hz)\n")

    # Per-step metrics, all in millimeters of real-world motion.
    rows = []
    for k in range(1, n):
        a, b = steps[k - 1], steps[k]
        rows.append({
            'i': b['i'],
            'hand_mm': norm(sub(b['hand'], a['hand'])) * 1000,
            # how far the spring pushed from last step's resolved pose
            'spring_mm': norm(sub(b['spring'], a['final'])) * 1000,
            # how far contacts moved it after the spring, this step
            'contact_mm': norm(sub(b['final'], b['spring'])) * 1000,
            # net blade motion (what the eye sees), in game units at the tip
            'tip_units': norm(sub(b['tip'], a['tip'])),
            'nc': b['nc'], 'hash': b['hash'], 'nprims': b['nprims'],
        })

    # Find the worst 1-second window by tip motion.
    win = max(10, int(1.0 / dt))
    best_i, best_sum = 0, -1.0
    for k in range(0, len(rows) - win):
        s = sum(r['tip_units'] for r in rows[k:k + win])
        if s > best_sum:
            best_sum, best_i = s, k
    w = rows[best_i:best_i + win]
    print(f"=== WORST {win}-sample window (samples {w[0]['i']}..{w[-1]['i']}) ===")
    print(f"  hand movement     : {sum(r['hand_mm'] for r in w)/len(w):7.3f} mm/step  "
          f"(is the player moving? >1 = yes)")
    print(f"  spring push       : {sum(r['spring_mm'] for r in w)/len(w):7.3f} mm/step")
    print(f"  contact push-back : {sum(r['contact_mm'] for r in w)/len(w):7.3f} mm/step")
    print(f"  NET blade tip     : {sum(r['tip_units'] for r in w)/len(w):7.3f} game units/step "
          f"<- the visible jitter")
    print(f"  contact count     : min={min(r['nc'] for r in w)} max={max(r['nc'] for r in w)} "
          f"changes={sum(1 for a,b in zip(w,w[1:]) if a['nc']!=b['nc'])}/{len(w)}")
    hashes = Counter(r['hash'] for r in w)
    hchg = sum(1 for a, b in zip(w, w[1:]) if a['hash'] != b['hash'])
    print(f"  geometry set      : {len(hashes)} distinct fingerprints, {hchg} changes "
          f"({'CHURNING' if len(hashes) > 3 else 'stable'})")
    print(f"  prim count        : min={min(r['nprims'] for r in w)} max={max(r['nprims'] for r in w)}")

    # Oscillation: sign flips of tip motion along its dominant axis.
    idx = [r['i'] for r in w]
    tips = [steps[i]['tip'] for i in idx if i < len(steps)]
    if len(tips) > 4:
        for axis, label in ((0, 'x'), (1, 'y'), (2, 'z')):
            series = [t[axis] for t in tips]
            mean = sum(series) / len(series)
            dev = [s - mean for s in series]
            flips = sum(1 for a, b in zip(dev, dev[1:]) if a * b < 0)
            amp = (max(series) - min(series))
            if amp > 0.05:
                print(f"  tip {label}: amplitude {amp:6.2f} units, {flips} sign flips "
                      f"(~{flips/2/ (len(series)*dt):.0f} Hz oscillation)")

    # Contact normal stability inside the window.
    ids = Counter()
    normals = []
    for r in w:
        for c in contacts.get(r['i'], []):
            ids[c['id']] += 1
            normals.append(c['n'])
    if normals:
        print(f"  contact prim ids  : {dict(ids)}")
        # cluster normals crudely by dominant axis+sign
        nk = Counter()
        for nn in normals:
            ax = max(range(3), key=lambda j: abs(nn[j]))
            nk[('xyz'[ax], '+' if nn[ax] > 0 else '-')] += 1
        print(f"  contact normals   : {dict(nk)}")

    print("\n=== VERDICT HINTS ===")
    hm = sum(r['hand_mm'] for r in w) / len(w)
    sm = sum(r['spring_mm'] for r in w) / len(w)
    cm = sum(r['contact_mm'] for r in w) / len(w)
    if hm > 1.5:
        print("  * The HAND is moving a lot — some of this is real player motion.")
    if cm > 2.0 and sm > 2.0:
        print("  * Spring and contacts are fighting hard every step (push-in/push-out cycle).")
    if len(hashes) > 3:
        print("  * Collision geometry set is CHURNING -> discovery instability, not the solver.")
    if sum(1 for a, b in zip(w, w[1:]) if a['nc'] != b['nc']) > len(w) * 0.25:
        print("  * Contact count is flickering -> contacts appearing/disappearing each step.")
    if cm < 0.5 and sm < 0.5 and sum(r['tip_units'] for r in w) / len(w) > 0.5:
        print("  * Blade barely moves in sim space but the TIP moves a lot -> rotation, or "
              "something OUTSIDE the sim (hand matrix / rendering) is moving it.")


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'vr_phys_log.csv')
