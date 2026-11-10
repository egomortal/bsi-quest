#!/usr/bin/env python3
"""One compact table for lab jobs (token-saver: read this, not the raw logs).

Usage (from anywhere; paths are found from this file's location, BSI-Quest\\tools\\bsi\\):
  python analyze_jobs.py j0342-j0349            range
  python analyze_jobs.py j0356 j0358 j0362       list
  python analyze_jobs.py --last 8                newest 8 finished jobs
  options: --rows "clear R32F,submit"   also print the [b1gq] rows whose label contains these words (ms/frame, count/frame)
           --tag b1rep                  also print the last line of a log tag (cut to --width chars)
           --width 300                  cut long lines
Columns: job | state | ver (game versionCode after the job) | spot | env (non-default only) | frame ms after the yaw marks
         (all samples) | GPU (mean per-frame GPU total, [b1gq]) | RT (render thread ms) | submits/frame | passes/frame
"""
import argparse, glob, json, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'lab', 'outbox')
DEFAULT_ENV = {'BSI_HLEFAST=1', 'BSI_DLC=1', 'DXVK_LOG_PATH=/sdcard/BSI', 'BSI_RHI=b1n'}


def jload(p):
    try:
        with open(p, encoding='utf-8-sig') as f:
            return json.load(f)
    except Exception:
        return None


def job_dirs(args):
    if args.last:
        ds = [d for d in glob.glob(os.path.join(OUT, 'j*')) if os.path.isdir(d) and os.path.exists(os.path.join(d, 'status.json'))]
        ds.sort(key=lambda d: os.path.getmtime(os.path.join(d, 'status.json')))
        return ds[-args.last:]
    want = []
    for a in args.jobs:
        m = re.fullmatch(r'j?(\d+)-j?(\d+)', a)
        if m:
            want += ['j%04d' % i for i in range(int(m.group(1)), int(m.group(2)) + 1)]
        else:
            want.append(a if a.startswith('j') else 'j' + a)
    res = []
    for w in want:
        res += sorted(d for d in glob.glob(os.path.join(OUT, w + '*')) if os.path.isdir(d))
    return res


def find_log(d):
    name = os.path.basename(d)
    own = os.path.join(d, name + '.log')
    if os.path.exists(own):
        return own
    for p in sorted(glob.glob(os.path.join(d, 'j*.log'))):   # older jobs copied the pull name from their template
        return p
    return None


def env_of(job):
    for s in (job or {}).get('steps', []):
        if s.get('do') == 'push_text' and s.get('dst') == '/sdcard/BSI/env.txt':
            t = s.get('text'); t = t if isinstance(t, list) else [t]
            return [x for x in t if x not in DEFAULT_ENV]
    return []


def spot_of(job):
    for s in (job or {}).get('steps', []):
        if s.get('do') == 'save_spot':
            return s.get('spot', '').replace('TC_', '')
        if s.get('do') == 'build':
            pass
    return '-'


def summarize(d, args):
    name = os.path.basename(d)
    st = jload(os.path.join(d, 'status.json')) or {}
    job = jload(os.path.join(d, 'job.json'))
    ia = jload(os.path.join(d, 'info_after.json')) or {}
    ver = ia.get('game_version') or ia.get('version') or ''
    state = st.get('state', '?')
    err = (st.get('error') or '')[:60]
    built = ''
    for s in (job or {}).get('steps', []):
        if s.get('do') == 'build':
            built = 'build %s' % s.get('version')
        if s.get('do') == 'install' and not built:
            built = 'install'
    row = {'job': name, 'state': state + (' (' + err + ')' if err and state != 'ok' else ''), 'ver': str(ver), 'spot': spot_of(job),
           'env': ' '.join(env_of(job)) + ((' [' + built + ']') if built else ''), 'frame': '', 'gpu': '', 'rt': '', 'sub': '', 'rp': '', 'extra': []}
    log = find_log(d) if state == 'ok' else None   # a failed job's pulled log is the PREVIOUS run's bsi.log: never show its numbers
    if not log:
        return row
    perf, gq, dxs, tags = [], None, None, {}
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('[b1perf]'):
                perf.append(line)
            elif line.startswith('[b1gq]'):
                gq = line
            elif line.startswith('[b1dxs]'):
                dxs = line
            if args.tag and line.startswith('[' + args.tag + ']'):
                tags[args.tag] = line.rstrip()
    if perf:
        row['frame'] = ' '.join(re.match(r'\[b1perf\] ([0-9.]+)', p).group(1) for p in perf[-4:])
        m = re.search(r'RenderingThread ([0-9.]+)', perf[-1]); row['rt'] = m.group(1) if m else ''
    if gq:
        m = re.search(r'mean total ([0-9.]+)', gq); row['gpu'] = m.group(1) if m else ''
        if args.rows:
            keys = [k.strip() for k in args.rows.split(',') if k.strip()]
            parts = gq.split(' | ')
            for i, p in enumerate(parts[:-1]):
                nxt = parts[i + 1]
                if any(k in p for k in keys):
                    m = re.match(r'([^:]*): ([0-9.]+) ms \(([0-9.]+)/frame', nxt)
                    if m:
                        row['extra'].append('%s %s | %s ms, %s/frame' % (p.strip(), m.group(1), m.group(2), m.group(3)))
    if dxs:
        m = re.search(r'queue submits ([0-9.]+)', dxs); row['sub'] = m.group(1) if m else ''
        m = re.search(r'render passes ([0-9.]+)', dxs); row['rp'] = m.group(1) if m else ''
    if args.tag and args.tag in tags:
        row['extra'].append(tags[args.tag][:args.width])
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jobs', nargs='*')
    ap.add_argument('--last', type=int, default=0)
    ap.add_argument('--rows', default='')
    ap.add_argument('--tag', default='')
    ap.add_argument('--width', type=int, default=300)
    args = ap.parse_args()
    ds = job_dirs(args)
    if not ds:
        print('no job folders found under', OUT); return 1
    cols = [('job', 'job'), ('state', 'state'), ('ver', 'ver'), ('spot', 'spot'), ('env', 'env / build'), ('frame', 'frame ms (yaw marks)'),
            ('gpu', 'GPU'), ('rt', 'RT'), ('sub', 'subm'), ('rp', 'passes')]
    print('| ' + ' | '.join(c[1] for c in cols) + ' |')
    print('|' + '---|' * len(cols))
    extras = []
    for d in ds:
        r = summarize(d, args)
        print('| ' + ' | '.join(str(r[c[0]]) for c in cols) + ' |')
        for e in r['extra']:
            extras.append('%s: %s' % (r['job'][:5], e))
    for e in extras:
        print(e)
    return 0


if __name__ == '__main__':
    sys.exit(main())
