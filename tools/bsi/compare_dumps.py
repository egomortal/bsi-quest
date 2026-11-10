#!/usr/bin/env python3
"""Compare back-buffer dumps of two lab jobs (BSI_B1_DUMP=N, BSI_B1_DUMPALPHA=1): one line per frame and region.

  python compare_dumps.py j0374 j0375 [--ui-x 1984]
For each b1_frameN.ppm (+ .alpha.pgm) found in both job folders: % of differing pixels and max channel difference, separately for
the eye columns [0, ui-x) and the UI column [ui-x, width). Animation and head motion differ between runs, so the eye columns
normally differ; the UI column (floating HUD layer) must be identical for an exactness check of a UI-column change.
"""
import argparse, glob, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'lab', 'outbox')


def read_pnm(p):
    with open(p, 'rb') as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:   # magic, width, height, maxval (no comments in our dumps)
        while data[i:i + 1].isspace(): i += 1
        j = i
        while not data[j:j + 1].isspace(): j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2]); ch = 3 if parts[0] == b'P6' else 1
    return w, h, ch, data[i:i + w * h * ch]


def diff(a, b, x0, x1):
    wa, ha, ch, pa = a; wb, hb, _, pb = b
    if (wa, ha) != (wb, hb): return 'size differs'
    n = bad = mx = 0
    for y in range(ha):
        ra = pa[(y * wa + x0) * ch:(y * wa + x1) * ch]; rb = pb[(y * wa + x0) * ch:(y * wa + x1) * ch]
        if ra == rb: n += (x1 - x0); continue
        for k in range(0, len(ra), ch):
            n += 1; d = max(abs(ra[k + c] - rb[k + c]) for c in range(ch))
            if d: bad += 1; mx = max(mx, d)
    return '%.3f %% differ (max %d)' % (100.0 * bad / max(1, n), mx)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('--ui-x', type=int, default=1984)
    a = ap.parse_args()
    da = sorted(glob.glob(os.path.join(OUT, a.a + '*')))[0]; db = sorted(glob.glob(os.path.join(OUT, a.b + '*')))[0]
    found = 0
    for fa in sorted(glob.glob(os.path.join(da, 'b1_frame*.ppm')) + glob.glob(os.path.join(da, 'b1_frame*.alpha.pgm'))):
        fb = os.path.join(db, os.path.basename(fa))
        if not os.path.exists(fb): continue
        A, B = read_pnm(fa), read_pnm(fb); found += 1
        print('%-28s eyes: %-26s UI column: %s' % (os.path.basename(fa), diff(A, B, 0, a.ui_x), diff(A, B, a.ui_x, A[0])))
    if not found: print('no dump pairs found in', da, 'and', db)
    return 0


if __name__ == '__main__':
    sys.exit(main())
