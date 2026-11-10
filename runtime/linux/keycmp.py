"""Compare main-thread KEY calls (file/module/thread/event/D3D) between native Wine relay and the Linux log."""
import re, sys
KEY = {'createfilew','getfileattributesw','findfirstfilew','loadlibraryw','loadlibrarya','getprocaddress','createthread',
       '_beginthreadex','_cxxthrowexception','createdxgifactory','d3d11createdevice','createwindowexw','getfileattributesexw',
       'deletefilew','createdirectoryw','regopenkeyexw','messageboxw','createeventw','setevent'}
def norm_path(s):
    s = s.replace('\\\\', '\\').lower()
    for p in ('z:\\home\\claude\\bsigame\\', 'c:\\bsi\\', 'c:\\users\\root\\documents\\', 'c:\\bsiuser\\'): s = s.replace(p, '<r>')
    b = s.split('\\')[-1]
    return 'pipe' if b.endswith('cout') else b
def native(path, limit):
    out = []
    rx = re.compile(r'^0024:Call (?:[\w]+)\.(\w+)\((.*)\) ret=([0-9a-f]+)')
    with open(path, 'rb') as f:
        for n, l in enumerate(f):
            if n > limit: break
            if not l.startswith(b'0024:Call '): continue
            m = rx.match(l.decode('latin1', 'replace'))
            if not m: continue
            nm = m.group(1).lower()
            if nm not in KEY: continue
            ra = int(m.group(3), 16)
            if not (0x401000 <= ra < 0x1700000): continue
            s = re.search(r'L?"([^"]*)"', m.group(2)); out.append((nm, norm_path(s.group(1)) if s else ''))
    return out
def linux(path):
    out = []; main = None
    rx = re.compile(r'^\[(\d+)\] [^!]+!(\w+)\(([^)]*)\)(?: L?"(.*)")?')
    for l in open(path, errors='replace'):
        if not l.startswith('['): continue
        m = rx.match(l)
        if not m: continue
        if main is None: main = m.group(1)
        if m.group(1) != main: continue
        nm = m.group(2).lower()
        if nm in KEY: out.append((nm, norm_path(m.group(4)) if m.group(4) else ''))
    return out
A = native(sys.argv[1], int(sys.argv[3]) if len(sys.argv) > 3 else 10**9); B = linux(sys.argv[2])
print('native key calls', len(A), 'linux', len(B))
i = j = 0; shown = 0
while i < len(A) and j < len(B):
    if A[i] == B[j]: i += 1; j += 1; continue
    # resync
    best = None
    for d in range(1, 60):
        for di, dj in ((d, 0), (0, d), (d, d)):
            if i+di < len(A) and j+dj < len(B) and A[i+di:i+di+3] == B[j+dj:j+dj+3]: best = (di, dj); break
        if best: break
    print(f'--- diff at native #{i} linux #{j}:'); 
    for k in range(i, min(i + (best[0] if best else 6), len(A))): print('   N', A[k])
    for k in range(j, min(j + (best[1] if best else 6), len(B))): print('   L', B[k])
    shown += 1
    if not best or shown > 25: break
    i += best[0]; j += best[1]
print('stopped at native', i, 'linux', j)
