import sys, collections, pickle
sys.path.insert(0,'/home/claude/bsi/recomp')
from x86c import *
F = load_bodies(); IMP = load_imports()
from multiprocessing import Pool
keys = sorted(F)
def work(chunk):
    ok=0; bad=collections.Counter(); failed=[]
    for a in chunk:
        rg, sw = F[a]
        if not rg: continue
        try:
            fn = Fn(a, rg, sw, None, IMP); good, src = fn.emit()
        except Exception as e:
            bad['CRASH '+type(e).__name__+' '+str(e)[:40]]+=1; failed.append(a); continue
        if good: ok+=1
        else: bad.update(fn.unsup); failed.append(a)
    return ok, bad, failed
if __name__=='__main__':
    chunks=[keys[i::8] for i in range(8)]
    with Pool(2) as p: res=p.map(work, chunks)
    ok=sum(r[0] for r in res); bad=collections.Counter(); failed=[]
    for r in res: bad.update(r[1]); failed+=r[2]
    print('functions', len(keys), 'fully translated', ok, f'{100*ok/len(keys):.1f}%', 'with gaps', len(failed))
    print(bad.most_common(40))
    pickle.dump(failed, open('failed.pkl','wb'))
