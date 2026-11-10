import sys, os, subprocess
sys.path.insert(0,'/home/claude/bsi/recomp')
import x86c; x86c.TRACE=True
from difftest import *
from capstone import *
from lib import va2off, data
a=int(sys.argv[1],16); seed=int(sys.argv[2])
md=Cs(CS_ARCH_X86,CS_MODE_32)
H=Harness(build({a},'trace'))
st=H.state(seed)
ut=[]
def hook(mu,addr,size,ud):
    ut.append(addr)
H.mu.hook_add(UC_HOOK_CODE,hook)
# record state AFTER each instruction: use previous-address trick
rows=[]
import math
def f80(t):
    m,e=t; s=-1 if e&0x8000 else 1; e&=0x7fff
    if e==0 and m==0: return 0.0*s
    if e==0x7fff: return float('nan')
    return s*m*2.0**(e-16383-63)
def hook2(mu,addr,size,ud):
    top=mu.reg_read(UC_X86_REG_FPSW)>>11&7
    rows.append((addr, {r:mu.reg_read(UREG[r]) for r in REGS}, top, f80(mu.reg_read(UC_X86_REG_FP0+top))))
H.mu.hook_add(UC_HOOK_CODE,hook2)
u,ue=H.run_uc(a,st)
def isj(ad):
    i=next(md.disasm(bytes(H.mu.mem_read(ad,16)),ad)); return i.mnemonic.startswith('j') or i.mnemonic in ('call','ret')
keep=[k for k in range(len(rows)) if not isj(rows[k][0])]
rows=[(rows[k][0],)+rows[k+1][1:] if k+1<len(rows) else rows[k] for k in keep]
print('uc:',ue)
os.dup2(os.open('/tmp/claude-0/ctrace.txt',os.O_WRONLY|os.O_CREAT|os.O_TRUNC),2)
c,ce=H.run_c(a,st)
os.dup2(1,2)
print('c:',ce)
ct=[l.split() for l in open('/tmp/claude-0/ctrace.txt') if l.startswith('T ')]
# c trace line k = state after instr at addr; uc rows[k+1] = state before next instr == after instr k
for k,l in enumerate(ct):
    addr=int(l[1],16); cs={p.split('=')[0]:p.split('=')[1] for p in l[2:]}
    if k>=len(rows): break
    ua,ur,utop,ust0=rows[k]
    if rows[k][0]!=addr:
        print(f'PATH divergence at step {k}: c executed {addr:#x}, x86 executed {rows[k][0]:#x}; previous c line', ct[k-1][:2] if k else None, 'c st0/st1', ct[k-1][-2:] if k else None); break
    bad=[r for r in ('eax','ecx','edx','ebx','esp','ebp','esi','edi') if int(cs[r],16)!=ur[r]]
    if int(cs['top'])!=utop: bad.append('top')
    cst=float(cs['st0'])
    if not (cst==ust0 or (cst!=cst and ust0!=ust0) or abs(cst-ust0)<=1e-6*max(abs(cst),abs(ust0))): bad.append('st0'); ur['st0']=ust0
    if bad:
        i=next(md.disasm(data[va2off(addr):va2off(addr)+16],addr))
        print(f'first divergence after {addr:#x} {i.mnemonic} {i.op_str}:', [(r, cs.get(r), (hex(ur[r]) if isinstance(ur[r],int) else ur[r]) if r in ur else utop) for r in bad], 'c st0', cs['st0'], cs['st1']); break
else: print('no register divergence in trace; lens', len(ct), len(rows))
