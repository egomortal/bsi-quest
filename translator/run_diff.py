import sys, random, collections, json, time
sys.path.insert(0,'/home/claude/bsi/recomp')
from difftest import *
sys.path.insert(0,'/home/claude/bsi'); from lib import load_graph
G = load_graph()
mods = json.load(open('/home/claude/bsi/modules.json'))
def pick(n, leaf, seed):
    rnd = random.Random(seed)
    cands = [a for a,f in G.items() if a in F and not f['name'].startswith(('Unwind','Catch')) and ((not f['calls']) == leaf)]
    return rnd.sample(cands, n)
def main(n, leaf, seed, trials=6, tag='t'):
    funcs = pick(n, leaf, seed)
    cl = closure(funcs, limit=4000) if not leaf else set(funcs)
    t=time.time(); so = build(cl, tag); print('built', len(cl), 'funcs in', round(time.time()-t), 's')
    H = Harness(so)
    res = collections.Counter(); fails = []; skipwhy=collections.Counter()
    for a in funcs:
        verdict = 'skip'; why=''
        for k in range(trials):
            st = H.state(a * 31 + k)
            u, ue = H.run_uc(a, st)
            if u is None: skipwhy[ue.split('(')[0]]+=1; continue
            c, ce = H.run_c(a, st)
            if c is None:
                verdict = 'cfault'; why = ce; break
            d = diff(u, c)
            if d: verdict = 'FAIL'; why = '; '.join(d[:4]); break
            verdict = 'pass'
        res[verdict] += 1
        if sum(res.values())%25==0: print(sum(res.values()), dict(res), flush=True)
        if verdict in ('FAIL', 'cfault'): fails.append((hex(a), mods.get(f'{a:08x}'), why))
    print(dict(res)); print('skip reasons', dict(skipwhy))
    for f in fails[:40]: print(f)
    json.dump(fails, open(f'/home/claude/bsi/recomp/fails_{tag}.json','w'))
if __name__ == '__main__':
    main(int(sys.argv[1]), sys.argv[2] == 'leaf', int(sys.argv[3]), tag=sys.argv[4])
