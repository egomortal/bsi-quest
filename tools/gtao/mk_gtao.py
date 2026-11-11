#!/usr/bin/env python3
"""mk_gtao.py <dumpdir> <outdir> <preset> [label] [KEY=VAL ...]

Builds a BSI_SHADER_REPLACE variant directory that replaces the game's HDAO main pass(es) with gtao.comp:
  <outdir>/<cs name>.txt  ("variant <source hash> <file> dispatch_scale X Y 1") + the SPIR-V.
<dumpdir> = DXVK_SHADER_DUMP_PATH output of the device that will run it (needs cs.3ed0bfa7...spv = HDAO main HQ and/or
cs.3c7a4717...spv = main NQ; the hash keys the variant to exactly that device + DXVK build, like gen_repl.sh).
The blurs are left alone (game blurs), or add the hdao lane's bit-exact faster blurs (k32id/t16id) next to it.
Presets: see PRESETS below; KEY=VAL overrides any compile-time parameter of gtao.comp."""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = ['cs.3ed0bfa754047750e778d92529a0d856', 'cs.3c7a4717d6f99499a9529e89117d0a9b']

# fitted on the captured frames (notes/GTAO.md section 3); 'match' = closest to the game's HDAO, 'improved' = closer
# to ground truth. Values are filled in by fit.py results.
PRESETS = {
    'const1': dict(CONST_AO=1.0), 'const0': dict(CONST_AO=0.0), 'const05': dict(CONST_AO=0.5),
    'match':    dict(GX=8, GY=8, SLICES=2, STEPS=3, JITTER=2, SCREENRAD=1, RADIUS_PX=12.0, FALLOFF=0.615, DISTPOW=2.0,
                     FINALPOW=2.2, INTENSITY=1.0, OUTPOW=2.0),
    'improved': dict(GX=8, GY=8, SLICES=2, STEPS=4, JITTER=2, SCREENRAD=0, RADIUS_W=40.0, RMIN_PX=4.0, RMAX_PX=48.0,
                     FALLOFF=0.615, DISTPOW=1.5, FINALPOW=2.2, INTENSITY=1.0, OUTPOW=2.0),
}

def fnv(path):
    h = 0xcbf29ce484222325
    for b in open(path, 'rb').read():
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return '%016x' % h

def build(params, spv_out):
    defs = []
    for k, v in params.items():
        if isinstance(v, float):
            v = repr(float(v))
        defs.append('-D%s=%s' % (k, v))
    with tempfile.TemporaryDirectory() as t:
        raw = os.path.join(t, 'g.spv')
        subprocess.check_call(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', raw] + defs +
                              [os.path.join(HERE, 'gtao.comp')], stdout=subprocess.DEVNULL)
        subprocess.check_call(['spirv-opt', '-O', raw, '-o', spv_out])
        subprocess.check_call(['spirv-val', '--target-env', 'vulkan1.1', spv_out])

def main():
    dump, out, preset = sys.argv[1:4]
    label = sys.argv[4] if len(sys.argv) > 4 and '=' not in sys.argv[4] else 'dev'
    params = dict(PRESETS[preset]) if preset in PRESETS else {}
    for a in sys.argv[4:]:
        if '=' in a:
            k, v = a.split('=', 1)
            params[k] = float(v) if '.' in v else int(v)
    os.makedirs(out, exist_ok=True)
    gx, gy = int(params.get('GX', 8)), int(params.get('GY', 8))
    half = int(params.get('HALF', 0))
    sx, sy = 32 // gx // (2 if half else 1), 32 // gy // (2 if half else 1)
    if sx * gx * (2 if half else 1) != 32 or sy * gy * (2 if half else 1) != 32:
        sys.exit('group size must divide 32 (16 in HALF mode)')
    n = 0
    for name in MAIN:
        src = os.path.join(dump, name + '.spv')
        if not os.path.exists(src):
            continue
        fn = '%s.gtao_%s.%s.spv' % (name, preset, label)
        build(params, os.path.join(out, fn))
        txt = os.path.join(out, name + '.txt')
        lines = [l for l in (open(txt).read().split('\n') if os.path.exists(txt) else []) if l and fn not in l]
        lines.append('variant %s %s dispatch_scale %d %d 1' % (fnv(src), fn, sx, sy))
        open(txt, 'w').write('\n'.join(lines) + '\n')
        n += 1
    open(os.path.join(out, 'params_%s.txt' % preset), 'w').write(' '.join('%s=%s' % kv for kv in params.items()) + '\n')
    print('mk_gtao: %s -> %s (%d shaders, %s)' % (preset, out, n, ' '.join('%s=%s' % kv for kv in params.items())))

if __name__ == '__main__':
    main()
