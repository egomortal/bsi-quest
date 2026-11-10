#!/usr/bin/env python3
"""Compare ir3 native code of two lab jobs (IR3_SHADER_DEBUG=vs,fs,cs,nocache + MESA_LOG file = mesa_ir3.log), per shader name.

  python compare_ir3.py j0458a j0458b
Shaders are keyed by the DXVK name in "Native code for unnamed <STAGE> shader <name> with blake3 ..." (fs.<md5>, vs.<md5>, cs.<md5>).
For each shader present in both logs: identical instruction text (after stripping addresses / comments / blank lines) or not,
plus instr/nops/(sy)/max_waves of each side. Identical native code => identical math for that shader on the same GPU.
Summary: n shaders in both, n identical, list of the first differing ones (largest first) with their stat lines.
"""
import glob, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'lab', 'outbox')
HDR = re.compile(r'Native code(?: \(overridden\))? for unnamed (\w+) shader (\S+)')
STAT = re.compile(r'; (\w+) prog \d+/\d+: (\d+) instr, (\d+) nops.*?(\d+) \(sy\).*?(\d+) max_waves', re.S)


def find_log(job):
    ds = sorted(glob.glob(os.path.join(OUT, job + '*')))
    for d in ds:
        p = os.path.join(d, 'mesa_ir3.log')
        if os.path.exists(p):
            return p
    sys.exit('no mesa_ir3.log for ' + job)


def norm(line):
    line = re.sub(r'^\S*MESA: \w+: ', '', line)   # Mesa log prefix
    line = re.sub(r'^\s*:?\d+:\d+:\d+:\[[0-9a-fx]+\]\s*', '', line)   # address / raw-encoding prefixes
    line = re.sub(r'^\s*\[[0-9a-fx_]+\]\s*', '', line)
    line = re.sub(r'^\s*[0-9a-fx]{8,}_[0-9a-fx]{8,}\s*', '', line)
    return line.strip()


def parse(path):
    shaders, cur, body, stat = {}, None, [], ''
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = HDR.search(line)
            if m:
                if cur:
                    shaders.setdefault(cur, (body, stat))
                cur, body, stat = m.group(2), [], ''
                continue
            if cur is None:
                continue
            if 'NIR (final form)' in line or 'NIR (' in line[:40]:
                shaders.setdefault(cur, (body, stat)); cur, body, stat = None, [], ''
                continue
            if ' prog ' in line and 'instr' in line:
                stat = line.strip()
                continue
            t = norm(line)
            if t and not t.startswith(';') and not t.startswith('Native code') and 'shader' not in t.lower()[:12]:
                body.append(t)
    if cur:
        shaders.setdefault(cur, (body, stat))
    return shaders


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, b = parse(find_log(sys.argv[1])), parse(find_log(sys.argv[2]))
    both = sorted(set(a) & set(b))
    same = [k for k in both if a[k][0] == b[k][0]]
    diff = sorted((k for k in both if a[k][0] != b[k][0]), key=lambda k: -len(a[k][0]))
    print('%s: %d shaders, %s: %d shaders, in both %d, native code identical %d, different %d'
          % (sys.argv[1], len(a), sys.argv[2], len(b), len(both), len(same), len(diff)))
    for k in diff[:25]:
        print('  DIFF %-40s %5d vs %5d lines | A: %s | B: %s' % (k, len(a[k][0]), len(b[k][0]), a[k][1][:90], b[k][1][:90]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
