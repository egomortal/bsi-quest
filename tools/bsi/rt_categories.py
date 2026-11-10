import sys,re,collections
for f in sys.argv[1:]:
    c=collections.Counter(); tot=0
    for l in open(f,errors='replace'):
        m=re.match(r'\s*[\d.]+%\s+(\d+)\s+RenderingThread\s+(\S+)\s+(.*)$',l)
        if not m: continue
        n=int(m.group(1)); dso=m.group(2); sym=m.group(3).strip(); tot+=n
        if re.match(r'f_[0-9a-f]+',sym): k='game code (recompiled x86)'
        elif sym in('call_indirect',) or 'dispatch' in sym and 'com' not in sym: k='recompiler indirect-call dispatch'
        elif 'mutex' in sym or 'futex' in sym: k='locks (mutex)'
        elif 'memmove' in sym or 'memcpy' in sym or 'memset' in sym: k='memory copies'
        elif 'tlsdesc' in sym or 'linker' in dso: k='TLS lookups (linker)'
        elif 'kallsyms' in dso: k='kernel'
        elif 'vulkan' in dso: k='driver'
        elif 'dxvk::' in sym: k='DXVK front-end'
        elif 'libc' in dso: k='other libc'
        elif re.search(r'b1dx|rhi_|com_|ctx_method|co_get|ids_of|pre_draw|dyn103|emit_cb|b1_',sym): k='D3D11 seam layer (ours)'
        else: k='other libbsi'
        c[k]+=n
    print('==',f, 'RT samples',tot)
    for k,n in c.most_common(): print('%5.1f%%  %s'%(100*n/tot,k))
