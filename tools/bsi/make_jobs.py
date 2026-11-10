#!/usr/bin/env python3
"""Make lab jobs from a finished job used as the template (token-saver: one command instead of hand-written JSON).

  python make_jobs.py --from j0343 --id j0370 --title "b120 Fair COPYTRIM=1" --env BSI_B1_COPYTRIM=1
  python make_jobs.py --from j0345 --id j0371 --spot TC_Rooftops --title "b120 Rooftops default"
  python make_jobs.py --from j0357 --id j0372 --build 120 --title "build+install 120 @Fair"   (a build job; also writes j0372z install retry)
  options: --env K=V (repeatable; replaces the template's extra env, default env always kept)
           --prof lab\\scenarios\\g4_gpuprof_clears.txt   (gpuprof.txt to push)
           --pull /sdcard/BSI/file (repeatable: removed before launch, pulled after, removed at the end)
           --queue   move the new job(s) into lab\\inbox (default: lab\\tmp only, so you can look first)
Templates come from lab\\outbox\\<id>*\\job.json (or lab\\inbox / lab\\hold\\**). The job pulls bsi.log as <id>.log.
"""
import argparse, copy, glob, json, os, shutil, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
LAB = os.path.join(ROOT, 'lab')
BASE_ENV = ['BSI_HLEFAST=1', 'BSI_DLC=1', 'DXVK_LOG_PATH=/sdcard/BSI', 'BSI_RHI=b1n']


def find_template(t):
    t = t if t.startswith('j') else 'j' + t
    for pat in (os.path.join(LAB, 'outbox', t + '*', 'job.json'), os.path.join(LAB, 'inbox', t + '*.json'), os.path.join(LAB, 'hold', '**', t + '*.json')):
        hits = sorted(glob.glob(pat, recursive=True))
        if hits:
            with open(hits[0], encoding='utf-8-sig') as f:
                return json.load(f), hits[0]
    sys.exit('template %s not found' % t)


def fix_own_paths(job):
    """job steps that reference the template's own outbox folder (e.g. lab\\outbox\\j0485\\turnip_prev.so pushed back in
    'always') must point at the NEW job's folder, else a driver-experiment copy restores the template's old driver."""
    import re
    s = json.dumps(job)
    s = re.sub(r'(lab\\\\outbox\\\\)j\d{4}[^\\"]*(\\\\)', lambda m: m.group(1) + job['id'] + m.group(2), s)
    return json.loads(s)


def write(job, queue):
    job = fix_own_paths(job)
    tmp = os.path.join(LAB, 'tmp'); os.makedirs(tmp, exist_ok=True)
    p = os.path.join(tmp, job['id'] + '.json')
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(job, f, indent=1)
    if queue:
        dst = os.path.join(LAB, 'inbox', job['id'] + '.json')
        if os.path.exists(dst):
            sys.exit('already queued: ' + dst)
        shutil.move(p, dst); p = dst
    print('wrote', p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='src', required=True); ap.add_argument('--id', required=True); ap.add_argument('--title', required=True)
    ap.add_argument('--env', action='append', default=[]); ap.add_argument('--spot'); ap.add_argument('--prof'); ap.add_argument('--build', type=int)
    ap.add_argument('--pull', action='append', default=[]); ap.add_argument('--queue', action='store_true')
    a = ap.parse_args()
    tpl, path = find_template(a.src)
    j = copy.deepcopy(tpl); j['id'] = a.id; j['title'] = 'Chat 4: ' + a.title if not a.title.lower().startswith('chat') else a.title
    has_build = False
    for s in j['steps']:
        if s.get('do') == 'push_text' and s.get('dst') == '/sdcard/BSI/env.txt' and 'BSI_RHI=b1n' in (s.get('text') or []):
            s['text'] = BASE_ENV + a.env
        if a.spot and s.get('do') == 'save_spot':
            s['spot'] = a.spot
        if a.prof and s.get('do') == 'push' and s.get('dst') == '/sdcard/BSI/gpuprof.txt':
            s['src'] = a.prof
        if s.get('do') == 'build':
            has_build = True
            if a.build: s['version'] = a.build; s['verify'] = 'MD5SUMS_kit%d.txt' % a.build
        if s.get('do') == 'install' and a.build:
            s['expect_version'] = a.build
    if a.build and not has_build:
        sys.exit('--build needs a template that has a build step (e.g. the last build+install job)')
    if not a.build and has_build:
        sys.exit('template %s builds; pass --build N or use a measurement job as the template' % a.src)
    for s in j.get('always', []):
        if s.get('do') == 'pull' and s.get('src') == '/sdcard/BSI/bsi.log':
            s['dst'] = a.id + '.log'
    if a.pull:
        i = [k for k, s in enumerate(j['steps']) if s.get('do') == 'launch'][0]
        j['steps'].insert(i, {'do': 'rm', 'path': a.pull})
        for q in a.pull:
            j['always'].insert(1, {'do': 'pull', 'src': q, 'dst': q.split('/')[-1], 'optional': True})
        j['always'].append({'do': 'rm', 'path': a.pull})
    write(j, a.queue)
    if a.build:   # safety re-install from the artifact: the build job's own install has lost Wi-Fi adb several times
        z = {'id': a.id + 'z', 'title': 'Chat 4: safety re-install of build %d' % a.build, 'timeout_min': 50, 'keep_awake': True,
             'steps': [{'do': 'devices', 'timeout_sec': 3600}, {'do': 'info', 'dst': 'info_before.json'},
                       {'do': 'install', 'apk': 'build\\bsi_b0_v091\\artifacts\\b1n_%dvr\\bsi_b0probe.apk' % a.build, 'expect_version': a.build}],
             'always': [{'do': 'info', 'dst': 'info_after.json'}, {'do': 'guardian', 'mode': 'resume'}]}
        write(z, a.queue)
    return 0


if __name__ == '__main__':
    sys.exit(main())
