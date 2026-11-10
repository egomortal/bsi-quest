#!/usr/bin/env python3
"""Paired A/B analysis of a build-126 ABAB run (BSI_B1_ABAB=<switch>:<N>).

  python tools\\bsi\\abab.py lab\\outbox\\j0467*\\j0467*.log [--metric frame_ms|gts_ms] [--boot 5000]

Reads the [b1abab] block lines (order A B B A A B B A ...), pairs each B block with its neighbouring A blocks
(B minus the mean of the A block before and after it, which cancels linear drift), and reports the mean
difference with a bootstrap 95 % interval. B = the named switch flipped. Negative = B is faster.
"""
import argparse, glob, random, re, statistics, sys

RX = re.compile(r'\[b1abab\] (\S+) blk (\d+) ([AB]) frames (\d+) frame_ms ([\d.]+) gts_ms ([\d.]+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', nargs='+')
    ap.add_argument('--metric', default='frame_ms', choices=['frame_ms', 'gts_ms'])
    ap.add_argument('--boot', type=int, default=5000)
    a = ap.parse_args()
    files = [f for p in a.log for f in glob.glob(p)]
    if not files:
        sys.exit('no log file')
    for f in files:
        blocks = []
        for line in open(f, encoding='utf-8', errors='replace'):
            m = RX.search(line)
            if m:
                v = float(m.group(5) if a.metric == 'frame_ms' else m.group(6))
                if int(m.group(4)) > 0 and v > 0:
                    blocks.append((int(m.group(2)), m.group(3), v, m.group(1)))
        if len(blocks) < 4:
            print('%s: only %d blocks' % (f, len(blocks)))
            continue
        blocks.sort()
        diffs = []
        for i, (k, ph, v, _) in enumerate(blocks):
            if ph != 'B':
                continue
            nb = [blocks[j][2] for j in (i - 1, i + 1) if 0 <= j < len(blocks) and blocks[j][1] == 'A' and abs(blocks[j][0] - k) == 1]
            if nb:
                diffs.append(v - sum(nb) / len(nb))
        A = [v for _, ph, v, _ in blocks if ph == 'A']
        B = [v for _, ph, v, _ in blocks if ph == 'B']
        if not diffs:
            print('%s: no B block with an A neighbour' % f)
            continue
        md = statistics.mean(diffs)
        boots = sorted(statistics.mean(random.choices(diffs, k=len(diffs))) for _ in range(a.boot))
        lo, hi = boots[int(0.025 * a.boot)], boots[int(0.975 * a.boot) - 1]
        print('%s: switch %s, %s: A %.3f (%d blocks), B %.3f (%d blocks); B-A paired %.3f ms, 95%% CI [%.3f, %.3f]%s'
              % (f, blocks[0][3], a.metric, statistics.mean(A), len(A), statistics.mean(B), len(B), md, lo, hi,
                 '  (significant)' if lo > 0 or hi < 0 else ''))


if __name__ == '__main__':
    main()
