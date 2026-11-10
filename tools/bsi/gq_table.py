import sys,re
for f in sys.argv[1:]:
    L=[l for l in open(f,errors='replace') if '[b1gq] GPU ms/frame by pass' in l][-1]
    parts=[p.strip() for p in L.split('|')][2:]
    rows=[]
    for i in range(0,len(parts)-1,2):
        m=re.match(r'(\S+): ([\d.]+) ms \(([\d.]+)/frame, ([\d.]+) draws',parts[i+1])
        if m: rows.append((float(m.group(2)),parts[i],m.group(1),m.group(4)))
    rows.sort(reverse=True)
    print('==',f.split('/')[0],'sum %.1f'%sum(r[0] for r in rows))
    for r in rows[:12]: print('%6.2f  %-34s %-22s draws %s'%(r[0],r[1][:34],r[2],r[3]))
