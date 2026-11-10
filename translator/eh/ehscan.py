"""Scan the exe for MSVC C++ EH metadata: FuncInfo via handler stubs (mov eax,FuncInfo; jmp __CxxFrameHandler3),
catch funclets and their continuation addresses; map each to the owning function. Output eh_conts.json"""
import sys, struct, json, bisect, re
sys.path.insert(0,'/home/claude/bsi'); sys.path.insert(0,'/home/claude/bsi/recomp')
from lib import pe, IB
from capstone import *
from x86c import load_bodies
img = pe.get_memory_mapped_image(); md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
TEXT = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
tlo, thi = IB + TEXT.VirtualAddress, IB + TEXT.VirtualAddress + TEXT.Misc_VirtualSize
def u32(a): return struct.unpack_from('<I', img, a - IB)[0]
def i32(a): return struct.unpack_from('<i', img, a - IB)[0]
F = load_bodies(); starts = sorted(F)
def owner(a):
    k = bisect.bisect_right(starts, a) - 1
    while k >= 0 and k > bisect.bisect_right(starts, a) - 40:
        f = starts[k]
        if any(lo <= a < hi for lo, hi in F[f][0]): return f
        k -= 1
    return None
stubs = {}
text = img[tlo - IB:thi - IB]
for m in re.finditer(rb'(?=\xB8(....)\xE9)', text, re.S):
    fi = struct.unpack('<I', m.group(1))[0]
    if not (IB <= fi < IB + len(img) - 32): continue
    if u32(fi) not in (0x19930520, 0x19930521, 0x19930522): continue
    stubs[tlo + m.start()] = fi
print('handler stubs', len(stubs))
# functions that push each stub
pushers = {}
for m in re.finditer(rb'(?=\x68(....))', text, re.S):
    v = struct.unpack('<I', m.group(1))[0]
    if tlo <= v < thi:
        for d in range(0, 0x40):
            if v + d in stubs: pushers.setdefault(v + d, set()).add(tlo + m.start()); break
conts = {}; nfun = 0; bad = 0; ncatch = 0
for stub, fi in stubs.items():
    ntry, ptry = u32(fi + 12), u32(fi + 16)
    owners = {owner(p) for p in pushers.get(stub, ())} - {None}
    for t in range(ntry):
        e = ptry + 20 * t
        ncat, parr = u32(e + 12), u32(e + 16)
        for h in range(ncat):
            fn = u32(parr + 16 * h + 12); ncatch += 1
            cs = set(); seen = set(); work = [fn]
            while work and len(seen) < 4000:
                a0 = work.pop(); last = 0
                for i in md.disasm(img[a0 - IB:a0 - IB + 0x800], a0):
                    if i.address in seen: break
                    seen.add(i.address)
                    if i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x'): last = int(i.op_str[5:], 16)
                    elif i.mnemonic == 'lea' and i.op_str.startswith('eax, [0x'): last = int(i.op_str[9:-1], 16)
                    elif i.op_str.startswith('eax'): last = 0
                    if i.mnemonic in ('ret', 'retn'):
                        if tlo <= last < thi: cs.add(last)
                        break
                    if i.mnemonic == 'jmp':
                        if i.operands[0].type == 2: work.append(i.operands[0].imm & 0xffffffff)
                        break
                    if i.mnemonic.startswith('j') and i.operands[0].type == 2: work.append(i.operands[0].imm & 0xffffffff)
                    if i.mnemonic == 'int3': break
            for o in owners:
                for cadr in cs:
                    hi = max(h for l, h in F[o][0])
                    nxt = starts[bisect.bisect_right(starts, o)] if bisect.bisect_right(starts, o) < len(starts) else 1 << 32
                    if any(lo <= cadr < h2 for lo, h2 in F[o][0]) or (o < cadr < hi + 0x800): conts.setdefault(o, set()).add(cadr)
                    else: bad += 1; print('  far cont', hex(o), hex(cadr), 'body end', hex(hi))
print('catch handlers', ncatch, 'functions with continuations', len(conts), 'conts', sum(len(v) for v in conts.values()), 'outside owner', bad)
json.dump({f'{k:x}': sorted(f'{x:x}' for x in v) for k, v in conts.items()}, open('/home/claude/bsi/recomp/eh/eh_conts.json', 'w'))
