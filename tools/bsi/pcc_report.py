#!/usr/bin/env python3
"""b139 PCSS census report: python tools/bsi/pcc_report.py <census job dir> [...]
Prints the last [b1pcc] draw/atlas lines, the last [b1eo] pixel-class + S1c CHECK line, and the DRAWPROF projection ms per shader."""
import sys, re, glob, os
for d in sys.argv[1:]:
    logs = [f for f in glob.glob(os.path.join(d, '*.log')) if os.path.basename(f).startswith(os.path.basename(d.rstrip('/\\'))[:5])]
    if not logs: print('==', d, 'no log'); continue
    L = open(logs[0], errors='replace').read().splitlines()
    print('==', d)
    pc = [l for l in L if l.startswith('[b1pcc]')]
    last_draw = {}
    for l in pc:
        m = re.match(r'\[b1pcc\] draws ps (\d+)', l)
        if m: last_draw[m.group(1)] = l
    for l in last_draw.values(): print(' ', l[:300])
    at = {}
    for l in pc:
        m = re.match(r'\[b1pcc\] atlas (\S+)', l)
        if m: at[m.group(1)] = l
    for l in at.values(): print(' ', l[:300])
    eo = [l for l in L if l.startswith('[b1eo] mode') and 'pixels:' in l]
    if eo: print('  S1:', eo[-1][eo[-1].find('| pixels'):][:600])
    gq = [l for l in L if '[b1gq] GPU ms/frame by pass' in l]
    if gq:
        parts = [p.strip() for p in gq[-1].split('|')][2:]
        rows = []
        for i in range(0, len(parts) - 1, 2):
            m = re.match(r'(\S+): ([\d.]+) ms \(([\d.]+)/frame, ([\d.]+) draws', parts[i + 1])
            if m and 'proj' in parts[i] + parts[i + 1]: rows.append((float(m.group(2)), parts[i], m.group(1)))
        for r in sorted(rows, reverse=True)[:10]: print('  gq %6.2f %s %s' % r)
