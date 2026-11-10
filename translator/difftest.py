"""Differential testing: original x86 (Unicorn) vs recompiled C, from identical random states."""
import sys, os, re, random, struct, ctypes, subprocess, collections, json, time
sys.path.insert(0, '/home/claude/bsi/recomp'); sys.path.insert(0, '/home/claude/bsi')
from x86c import Fn, load_bodies, load_imports
from lib import pe, IB, data
from unicorn import *
from unicorn.x86_const import *
from ucfs import setup_fs

F = load_bodies(); IMP = load_imports()
STACK, STACKSZ = 0x08000000, 0x100000
SCR, SCRSZ = 0x10000000, 0x400000
TIB = 0x7ffd0000
SENT = 0x0BAD0000
ESP0 = STACK + STACKSZ - 0x10000

# ---- image
IMG_LO = IB; IMG_HI = IB + pe.OPTIONAL_HEADER.SizeOfImage
image = bytearray(IMG_HI - IMG_LO)
for s in pe.sections:
    raw = s.get_data()[:s.Misc_VirtualSize]
    image[s.VirtualAddress:s.VirtualAddress + len(raw)] = raw
DATA = [s for s in pe.sections if s.Name.startswith(b'.data')][0]
DLO = IB + DATA.VirtualAddress; DHI = DLO + ((DATA.Misc_VirtualSize + 0xfff) & ~0xfff)
# runtime-initialised globals are zero at load: point them into scratch so code paths get exercised
_r = random.Random(7)
for off in range(DLO - IMG_LO, DHI - IMG_LO, 4):
    if image[off:off + 4] == b'\0\0\0\0' and _r.random() < 0.9:
        struct.pack_into('<I', image, off, (SCR + _r.randrange(0, SCRSZ - 0x10000)) & ~3)
LOW, LOWSZ = 0, 0x10000

def closure(roots, limit=400):
    seen = set(); todo = list(roots)
    for a in roots:
        if a in F: seen.add(a)
    todo = []
    for a in roots:
        if a not in F: continue
        fn = Fn(a, *F[a], None, IMP); fn.emit(); todo += list(fn.calls)
    while todo and len(seen) < limit:
        a = todo.pop()
        if a in seen or a not in F: continue
        seen.add(a)
        fn = Fn(a, *F[a], None, IMP); fn.emit()
        todo += [t for t in fn.calls if t not in seen]
    return seen

def build(funcs, name):
    srcs = []; defined = set(); referenced = set()
    for a in sorted(funcs):
        fn = Fn(a, *F[a], None, IMP); ok, src = fn.emit()
        srcs.append(src); defined.add(a); referenced |= fn.calls
    hdr = ['#include "cpu.h"']
    for a in sorted(defined | referenced): hdr.append(f'void f_{a:08x}(CPU*c);')
    stubs = [f'void f_{a:08x}(CPU*c){{ unimpl(c,0x{a:x}u,"not in closure"); }}' for a in sorted(referenced - defined)]
    path = f'/home/claude/bsi/recomp/build/{name}'
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path + '.c', 'w').write('\n'.join(hdr + stubs + srcs))
    fm = os.environ.get('BSI_DIFF_FIXED_M')   # e.g. 0x2000000000: compile with the fixed guest base (cpu.h BSI_FIXED_M)
    r = subprocess.run(['gcc', '-O1', '-shared', '-fPIC', '-w', '-fno-strict-aliasing', '-I/home/claude/bsi/recomp'] + ([f'-DBSI_FIXED_M={fm}ULL'] if fm else []) + [
                        '-o', path + '.so', path + '.c', '/home/claude/bsi/recomp/rt_test.c', '-lm'], capture_output=True, text=True)
    if r.returncode: raise RuntimeError(r.stderr[:3000])
    return path + '.so'

class CPU(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')] + \
               [(n, ctypes.c_uint8) for n in ('cf', 'zf', 'sf', 'of', 'pf', 'df')] + \
               [('_pad', ctypes.c_uint8 * 2), ('xmm', ctypes.c_uint8 * 128), ('st', ctypes.c_double * 8), ('top', ctypes.c_uint32),
                ('fcw', ctypes.c_uint16), ('fsw', ctypes.c_uint16), ('fs_base', ctypes.c_uint32)]

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UREG = dict(eax=UC_X86_REG_EAX, ecx=UC_X86_REG_ECX, edx=UC_X86_REG_EDX, ebx=UC_X86_REG_EBX, esp=UC_X86_REG_ESP,
            ebp=UC_X86_REG_EBP, esi=UC_X86_REG_ESI, edi=UC_X86_REG_EDI)
UXMM = [getattr(__import__('unicorn.x86_const', fromlist=['x']), f'UC_X86_REG_XMM{k}') for k in range(8)]

class Harness:
    def __init__(self, so):
        self.lib = ctypes.CDLL(so)
        self.lib.rt_init.restype = ctypes.c_void_p
        self.lib.rt_msg.restype = ctypes.c_char_p
        self.M = self.lib.rt_init()
        for lo, n in ((LOW, LOWSZ), (IMG_LO, len(image)), (STACK, STACKSZ), (SCR, SCRSZ), (TIB, 0x1000)):
            assert self.lib.rt_map(ctypes.c_uint32(lo), ctypes.c_uint32(n)) == 0
        ctypes.memmove(self.M + IMG_LO, bytes(image), len(image))
        self.dirty_img = False
        self.newuc()

    def newuc(self):
        mu = Uc(UC_ARCH_X86, UC_MODE_32)
        for lo, n in ((LOW, LOWSZ), (IMG_LO, len(image)), (STACK, STACKSZ), (SCR, SCRSZ), (TIB, 0x1000)): mu.mem_map(lo, n)
        mu.mem_write(IMG_LO, bytes(image)); setup_fs(mu, TIB)
        self.mu = mu

    def state(self, seed):
        rnd = random.Random(seed)
        def val():
            r = rnd.random()
            if r < 0.86: return SCR + rnd.randrange(0, SCRSZ - 0x10000) & ~3
            if r < 0.93: return rnd.randrange(0, 64)
            if r < 0.98: return struct.unpack('<I', struct.pack('<f', rnd.uniform(-100, 100)))[0]
            return rnd.getrandbits(32)
        import numpy as np
        words = np.array([val() for _ in range(8192)], dtype=np.uint32)
        scr = bytearray(np.tile(words, SCRSZ // 4 // 8192).tobytes())
        stk = bytearray(STACKSZ)
        for k in range(0, 0x100, 4): struct.pack_into('<I', stk, ESP0 - STACK + 4 + k, val())
        struct.pack_into('<I', stk, ESP0 - STACK, SENT)
        tib = bytearray(0x1000); struct.pack_into('<IIII', tib, 0, 0xffffffff, STACK + STACKSZ, STACK, 0); struct.pack_into('<I', tib, 0x18, TIB)
        regs = {r: val() for r in REGS}; regs['esp'] = ESP0; regs['ebp'] = ESP0 + 0x200
        xmm = [bytes(struct.pack('<4f', *[rnd.uniform(-10, 10) for _ in range(4)])) for _ in range(8)]
        return scr, stk, tib, regs, xmm

    def run_uc(self, fa, st, limit=2_000_000):
        scr, stk, tib, regs, xmm = st; mu = self.mu
        try: setup_fs(mu, TIB, remap=False)
        except UcError:
            self.newuc(); mu = self.mu
        mu.mem_write(IMG_LO, bytes(image[:DLO-IMG_LO])) if self.dirty_img else None
        mu.mem_write(DLO, bytes(image[DLO - IMG_LO:DHI - IMG_LO]))
        mu.mem_write(LOW, bytes(LOWSZ)); mu.mem_write(SCR, bytes(scr)); mu.mem_write(STACK, bytes(stk)); mu.mem_write(TIB, bytes(tib))
        for r, v in regs.items(): mu.reg_write(UREG[r], v)
        for k in range(8): mu.reg_write(UXMM[k], int.from_bytes(xmm[k], 'little'))
        mu.reg_write(UC_X86_REG_FPCW, 0x27f); mu.reg_write(UC_X86_REG_FPSW, 0); mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        try:
            mu.emu_start(fa, SENT, count=limit)
        except UcError as e:
            return None, str(e)
        if mu.reg_read(UC_X86_REG_EIP) != SENT: return None, 'timeout'
        out = {r: mu.reg_read(UREG[r]) for r in REGS}
        out['xmm'] = [mu.reg_read(UXMM[k]).to_bytes(16, 'little') for k in range(8)]
        out['top'] = (mu.reg_read(UC_X86_REG_FPSW) >> 11) & 7
        out['mem'] = (bytes(mu.mem_read(LOW, LOWSZ)), bytes(mu.mem_read(DLO, DHI - DLO)), bytes(mu.mem_read(STACK, STACKSZ)), bytes(mu.mem_read(SCR, SCRSZ)))
        if out['top'] != 0:
            st0 = mu.reg_read(UC_X86_REG_FP0 + ((out['top']) & 7) - 0) if False else None
        return out, None

    def run_c(self, fa, st):
        scr, stk, tib, regs, xmm = st; M = self.M
        ctypes.memmove(M + DLO, bytes(image[DLO - IMG_LO:DHI - IMG_LO]), DHI - DLO)
        ctypes.memset(M + LOW, 0, LOWSZ); ctypes.memmove(M + SCR, bytes(scr), SCRSZ); ctypes.memmove(M + STACK, bytes(stk), STACKSZ); ctypes.memmove(M + TIB, bytes(tib), 0x1000)
        c = CPU()
        for r, v in regs.items(): setattr(c, r, v)
        for k in range(8): ctypes.memmove(ctypes.addressof(c) + CPU.xmm.offset + 16 * k, xmm[k], 16)
        c.fcw = 0x27f; c.fs_base = TIB; c.top = 0
        fnp = getattr(self.lib, f'f_{fa:08x}')
        rc = self.lib.rt_run(ctypes.cast(fnp, ctypes.c_void_p), ctypes.byref(c))
        if rc: return None, self.lib.rt_msg().decode()
        out = {r: getattr(c, r) for r in REGS}
        out['xmm'] = [bytes(c.xmm[16 * k:16 * k + 16]) for k in range(8)]
        out['top'] = c.top
        out['mem'] = (ctypes.string_at(M + LOW, LOWSZ), ctypes.string_at(M + DLO, DHI - DLO), ctypes.string_at(M + STACK, STACKSZ), ctypes.string_at(M + SCR, SCRSZ))
        return out, None

def diff(u, c):
    d = []
    for r in REGS:
        if u[r] != c[r]: d.append(f'{r}: x86={u[r]:#x} c={c[r]:#x}')
    for k in range(8):
        if u['xmm'][k] != c['xmm'][k]:
            fu = struct.unpack('<4f', u['xmm'][k]); fc = struct.unpack('<4f', c['xmm'][k])
            if all((a == b) or (a != a and b != b) or abs(a - b) <= 1e-5 * max(1, abs(a)) for a, b in zip(fu, fc)): continue
            d.append(f'xmm{k}: x86={u["xmm"][k].hex()} c={c["xmm"][k].hex()}')
    if u['top'] != c['top']: d.append(f'fpu top x86={u["top"]} c={c["top"]}')
    for nm, a, b, base in zip(('low', 'data', 'stack', 'scratch'), u['mem'], c['mem'], (LOW, DLO, STACK, SCR)):
        if a != b:
            k = next(x for x in range(len(a)) if a[x] != b[x])
            if nm == 'stack' and k + base < ESP0 - 0x8000: pass
            d.append(f'{nm} @ {base + k:#x}: x86={a[k:k + 8].hex()} c={b[k:k + 8].hex()}')
    return d
