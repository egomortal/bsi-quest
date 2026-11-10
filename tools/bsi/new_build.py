#!/usr/bin/env python3
"""The build ritual in two commands (token-saver: no hand-written backups, md5 lists or version bumps).

  python new_build.py start 120 --files src_b1n/linux/rhi_b1_dxvk.cpp src_b1n/linux/rhi_b1_sb87.inc
      before editing: copies those kit files + build_b1n_vr_link.py to build\\bsi_b0_v091\\history\\backups\\b120_b119_backup\\kit\\
  ... edit the kit files ...
  python new_build.py finish 120 --note "one line: what changed" [--add src_b1n/linux/new_file.inc]
      bumps the version string in build_b1n_vr_link.py (b1n119vr -> b1n120vr) and adds the note to its docstring,
      writes MD5SUMS_kit120.txt (the previous list + --add files, all re-hashed), keeps the previous build's README / MD5SUMS at the top and moves older ones into history\\,
      writes README_B120.md (skeleton with the note and the changed files' md5s) unless it exists, prints what changed.
  python new_build.py status        latest build number, kit files that differ from its MD5SUMS
The kit has a LOCAL git history (kit\.git, whitelist .gitignore, no remote - never add one): finish commits the build;
`git -C build\bsi_b0_v091\kit diff` / `log -p` shows exactly what changed between builds.
Kit paths are relative to build\\bsi_b0_v091\\kit (forward slashes fine). Then queue: make_jobs.py --from <last build job> --id jNNNN --build 120.
"""
import argparse, datetime, glob, hashlib, os, re, shutil, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BD = os.path.join(ROOT, 'build', 'bsi_b0_v091'); KIT = os.path.join(BD, 'kit'); HIST = os.path.join(BD, 'history')
LINK = os.path.join(KIT, 'build_b1n_vr_link.py')


def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def latest():
    ns = [int(m.group(1)) for p in glob.glob(os.path.join(BD, 'MD5SUMS_kit*.txt')) for m in [re.search(r'MD5SUMS_kit(\d+)\.txt$', p)] if m]
    if not ns:
        sys.exit('no MD5SUMS_kitNN.txt at the top of ' + BD)
    return max(ns)


def read_sums(n):
    p = os.path.join(BD, 'MD5SUMS_kit%d.txt' % n)
    if not os.path.exists(p):
        p = os.path.join(HIST, 'md5sums', 'MD5SUMS_kit%d.txt' % n)
    out = []
    for line in open(p, encoding='utf-8'):
        m = re.match(r'([0-9a-fA-F]{32})\s+\*?(.+)$', line.strip())
        if m:
            out.append((m.group(1).lower(), m.group(2).strip()))
    return out


def rel(f):
    f = f.replace('\\', '/')
    return f if f.startswith('./') else './' + f


def cmd_status(a):
    n = latest(); diff = []
    for h, f in read_sums(n):
        p = os.path.join(KIT, f[2:] if f.startswith('./') else f)
        if not os.path.exists(p):
            diff.append(f + ' (missing)')
        elif md5(p) != h:
            diff.append(f + ' (changed)')
    print('latest build: %d; kit files differing from MD5SUMS_kit%d.txt: %s' % (n, n, ', '.join(diff) or 'none'))


def cmd_start(a):
    prev = latest()
    if a.n <= prev:
        sys.exit('build %d already has MD5SUMS (latest %d)' % (a.n, prev))
    dst = os.path.join(HIST, 'backups', 'b%d_b%d_backup' % (a.n, prev), 'kit')
    for f in list(a.files) + ['build_b1n_vr_link.py']:
        r = rel(f)[2:]; src = os.path.join(KIT, r)
        if not os.path.exists(src):
            print('new file (nothing to back up):', r); continue
        d = os.path.join(dst, r); os.makedirs(os.path.dirname(d), exist_ok=True)
        if not os.path.exists(d):
            shutil.copy2(src, d)
        print('backup %s  %s' % (md5(d), r))
    print('backups in', dst)


def cmd_finish(a):
    prev = latest()
    if a.n <= prev:
        sys.exit('build %d already finished (latest MD5SUMS is %d)' % (a.n, prev))
    s = open(LINK, encoding='utf-8').read()
    old = '0.9.1-b1n%dvr' % prev
    if s.count(old) != 1:
        sys.exit('version string %s not found exactly once in build_b1n_vr_link.py' % old)
    s = s.replace(old, '0.9.1-b1n%dvr' % a.n)
    end = s.index('"""', s.index('"""') + 3)
    try:   # New York time, like the board
        from zoneinfo import ZoneInfo
        stamp = datetime.datetime.now(ZoneInfo('America/New_York')).strftime('%Y-%m-%d %H:%M %Z')
    except Exception:
        stamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')
    s = s[:end].rstrip('\n') + '\nb%d (%s): version b1n%d(vr). %s See README_B%d.md.' % (a.n, stamp, a.n, a.note, a.n) + s[end:]
    with open(LINK, 'w', encoding='utf-8', newline='') as f:
        f.write(s)
    files = [f for _, f in read_sums(prev)] + [rel(x) for x in a.add]
    seen, out, changed = set(), [], []
    old_h = dict((f, h) for h, f in read_sums(prev))
    for f in files:
        if f in seen:
            continue
        seen.add(f); p = os.path.join(KIT, f[2:])
        if not os.path.exists(p):
            sys.exit('listed kit file missing: ' + f)
        h = md5(p); out.append((h, f))
        if old_h.get(f) != h:
            changed.append((f, old_h.get(f, 'new')[:8], h[:8]))
    sums = os.path.join(BD, 'MD5SUMS_kit%d.txt' % a.n)
    with open(sums, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join('%s  %s\n' % x for x in out))
    # keep the PREVIOUS build's MD5SUMS/README at the top too (its queued lab jobs still verify against it); move only older ones
    for p in glob.glob(os.path.join(BD, 'MD5SUMS_kit*.txt')) + glob.glob(os.path.join(BD, 'README_B*.md')):
        m = re.search(r'(?:MD5SUMS_kit|README_B)(\d+)\.(?:txt|md)$', p)
        if m and int(m.group(1)) < prev - 2:   # keep the last 3 builds at the top: queued lab jobs of older builds still verify against them
            sub = 'md5sums' if 'MD5SUMS' in p else 'readme'
            os.makedirs(os.path.join(HIST, sub), exist_ok=True); shutil.move(p, os.path.join(HIST, sub, os.path.basename(p)))
    rd = os.path.join(BD, 'README_B%d.md' % a.n)
    if not os.path.exists(rd):
        with open(rd, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Build %d (%s) = build %d + %s\n\nFiles: %s. MD5SUMS_kit%d.txt; build %d copies in history\\backups\\b%d_b%d_backup\\kit\\.\n'
                    % (a.n, stamp, prev, a.note, ', '.join('%s %s (was %s)' % (os.path.basename(f), n, o) for f, o, n in changed), a.n, prev, a.n, prev))
    print('build %d finished: %s; changed: %s' % (a.n, os.path.basename(sums), ', '.join('%s %s->%s' % c for c in changed)))
    if os.path.isdir(os.path.join(KIT, '.git')):   # local-only history (never pushed): one commit per build
        import subprocess
        subprocess.run(['git', '-C', KIT, 'add', '-A'], check=False)
        r = subprocess.run(['git', '-C', KIT, 'commit', '-q', '-m', 'build %d: %s' % (a.n, a.note)], check=False)
        print('git commit', 'done' if r.returncode == 0 else 'skipped (nothing to commit?)')


def main():
    ap = argparse.ArgumentParser(); sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('start'); p.add_argument('n', type=int); p.add_argument('--files', nargs='+', required=True); p.set_defaults(fn=cmd_start)
    p = sp.add_parser('finish'); p.add_argument('n', type=int); p.add_argument('--note', required=True); p.add_argument('--add', nargs='*', default=[]); p.set_defaults(fn=cmd_finish)
    p = sp.add_parser('status'); p.set_defaults(fn=cmd_status)
    a = ap.parse_args(); a.fn(a); return 0


if __name__ == '__main__':
    sys.exit(main())
