"""BSI-Quest static recompiler prototype: x86-32 (MSVC) -> C.
Each guest function becomes `void f_XXXXXXXX(CPU *c)` operating on guest registers and guest memory (M + addr).
"""
import sys, re, collections
sys.path.insert(0, '/home/claude/bsi')
from lib import data, va2off, IB, TLO, THI
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import *

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True

REG32 = {'eax','ecx','edx','ebx','esp','ebp','esi','edi'}
R16 = {'ax':'eax','cx':'ecx','dx':'edx','bx':'ebx','sp':'esp','bp':'ebp','si':'esi','di':'edi'}
R8L = {'al':'eax','cl':'ecx','dl':'edx','bl':'ebx'}
R8H = {'ah':'eax','ch':'ecx','dh':'edx','bh':'ebx'}
FL = ('cf','zf','sf','of','pf')
CC = {  # condition -> (C expr, flags read)
 'o':('c->of',{'of'}),'no':('!c->of',{'of'}),'b':('c->cf',{'cf'}),'ae':('!c->cf',{'cf'}),'e':('c->zf',{'zf'}),'ne':('!c->zf',{'zf'}),
 'be':('(c->cf|c->zf)',{'cf','zf'}),'a':('!(c->cf|c->zf)',{'cf','zf'}),'s':('c->sf',{'sf'}),'ns':('!c->sf',{'sf'}),
 'p':('c->pf',{'pf'}),'np':('!c->pf',{'pf'}),'l':('(c->sf!=c->of)',{'sf','of'}),'ge':('(c->sf==c->of)',{'sf','of'}),
 'le':('(c->zf||c->sf!=c->of)',{'zf','sf','of'}),'g':('(!c->zf&&c->sf==c->of)',{'zf','sf','of'})}
CCALIAS = {'c':'b','nae':'b','nc':'ae','nb':'ae','z':'e','nz':'ne','na':'be','nbe':'a','pe':'p','po':'np','nge':'l','nl':'ge','ng':'le','nle':'g'}
def ccof(suffix):
    s = CCALIAS.get(suffix, suffix)
    return CC.get(s)

class Unsupported(Exception): pass
TRACE = False
TRACE_FUNCS = set()   # functions to build with per-instruction TRACE(c,addr) (debugging)
FUNC_STARTS = set()
EH_CONTS = {}   # function entry -> C++ catch continuation addresses (resume points after a caught exception)

def T(bits): return {8:'uint8_t',16:'uint16_t',32:'uint32_t',64:'uint64_t'}[bits]
def ST_(bits): return {8:'int8_t',16:'int16_t',32:'int32_t',64:'int64_t'}[bits]
def MASK(bits): return {8:'0xffu',16:'0xffffu',32:'0xffffffffu'}[bits]
def MSB(bits): return {8:'0x80u',16:'0x8000u',32:'0x80000000u'}[bits]

class Fn:
    def __init__(self, entry, ranges, switches, known_funcs, imports):
        self.entry = entry; self.ranges = ranges; self.switches = switches
        self.known = known_funcs; self.imports = imports
        self.insns = []
        seen_end = 0
        # merge ranges separated by tiny gaps (bogus function starts inside an instruction steal bytes from the body)
        rs = sorted(ranges); merged = []
        for lo, hi in rs:
            if merged and lo <= merged[-1][1] + 16: merged[-1][1] = max(merged[-1][1], hi)
            else: merged.append([lo, hi])
        for lo, hi in merged:
            o = va2off(lo)
            for i in md.disasm(data[o:o + (hi - lo + 1) + 16], lo):
                if i.address > hi: break
                if i.address < seen_end: continue
                self.insns.append(i); seen_end = i.address + i.size
        self.insns.sort(key=lambda i: i.address)
        # body truncated by a bogus function start: keep decoding until a terminator
        if self.insns:
            last = self.insns[-1]
            if not (last.mnemonic.startswith('ret') or last.mnemonic in ('jmp', 'repz ret', 'rep ret', 'int3')):
                a = last.address + last.size; o = va2off(a)
                for i in md.disasm(data[o:o + 256], a):
                    self.insns.append(i)
                    if i.mnemonic.startswith('ret') or i.mnemonic in ('jmp', 'repz ret', 'int3'): break
        self.addrs = set(i.address for i in self.insns)
        self.switches = dict(self.switches)
        self.recover_tables()
        # flow closure: pull in code reached by direct jumps that is not a known function start
        if FUNC_STARTS:
            work = True; rounds = 0
            seeds = set(EH_CONTS.get(entry, ()))
            while work and rounds < 64:
                work = False; rounds += 1
                targets = set(t for t in seeds if t not in self.addrs); seeds = set()
                for i in self.insns:   # fall-through into bytes not decoded yet (body ranges with holes / truncated bodies)
                    if i.mnemonic.startswith('ret') or i.mnemonic in ('jmp', 'int3', 'hlt'): continue
                    nx = i.address + i.size
                    if nx not in self.addrs and TLO <= nx < THI: targets.add(nx)
                for i in self.insns:
                    if i.group(X86_GRP_JUMP) and i.operands and i.operands[0].type == X86_OP_IMM:
                        t = i.operands[0].imm & 0xffffffff
                        if t not in self.addrs and t not in FUNC_STARTS and TLO <= t < THI: targets.add(t)
                for sa, tg in self.switches.items():
                    for t in tg:
                        if t not in self.addrs and t not in FUNC_STARTS and TLO <= t < THI: targets.add(t)
                for t in sorted(targets):
                    if t in self.addrs: continue
                    o = va2off(t); added = 0
                    for i in md.disasm(data[o:o + 0x2000], t):
                        if i.address in self.addrs: break
                        self.insns.append(i); self.addrs.add(i.address); added += 1
                        if i.mnemonic.startswith('ret') or i.mnemonic in ('jmp', 'int3') or added > 4000: break
                    work = work or added > 0
            self.insns.sort(key=lambda i: i.address)
        self.unsup = collections.Counter()

    def recover_tables(self):
        """jump tables Ghidra left unresolved: jmp [reg*4+table] -> read the table (count from a preceding cmp reg,imm)"""
        import struct
        ins = self.insns
        for k, i in enumerate(ins):
            if i.mnemonic != 'jmp' or not i.operands or i.operands[0].type != X86_OP_MEM: continue
            m = i.operands[0].mem
            if m.base or not m.index or m.scale != 4: continue
            if self.switches.get(i.address): continue
            tbl = m.disp & 0xffffffff
            o = va2off(tbl)
            if o is None: continue
            idx = i.reg_name(m.index); cnt = None
            for j in range(k - 1, max(-1, k - 10), -1):
                p = ins[j]
                if p.mnemonic == 'cmp' and len(p.operands) == 2 and p.operands[0].type == X86_OP_REG and p.operands[1].type == X86_OP_IMM:
                    if p.reg_name(p.operands[0].reg) == idx: cnt = (p.operands[1].imm & 0xffffffff) + 1; break
                if p.mnemonic == 'movzx' and p.operands[0].type == X86_OP_REG and p.reg_name(p.operands[0].reg) == idx and p.operands[1].type == X86_OP_MEM:
                    # byte index table: count = max byte + 1 (scan the byte table up to the jump table)
                    bt = p.operands[1].mem.disp & 0xffffffff; bo = va2off(bt)
                    if bo is not None and bt < tbl and tbl - bt <= 1024: cnt = max(data[bo:bo + (tbl - bt)]) + 1
                    break
            lim = cnt if cnt and cnt <= 1024 else 256
            tg = []
            for n in range(lim):
                if o + 4 * n + 4 > len(data): break
                v = struct.unpack_from('<I', data, o + 4 * n)[0]
                if not (TLO <= v < THI and abs(v - i.address) < 0x20000):
                    if cnt: tg = []
                    break
                tg.append(v)
            if tg: self.switches[i.address] = tg

    def inbody(self, a): return a in self.addrs

    # ---------- operands
    def reg(self, i, r):
        n = i.reg_name(r)
        if n in REG32: return f'c->{n}', 32
        if n in R16: return f'(uint16_t)c->{R16[n]}', 16
        if n in R8L: return f'(uint8_t)c->{R8L[n]}', 8
        if n in R8H: return f'(uint8_t)(c->{R8H[n]}>>8)', 8
        if n in ('cs','ds','es','ss','fs','gs'): return {'cs':'0x23u','ss':'0x2bu','ds':'0x2bu','es':'0x2bu','fs':'0x53u','gs':'0x2bu'}[n], 16
        raise Unsupported('reg ' + n)

    def wreg(self, i, r, expr):
        n = i.reg_name(r)
        if n in REG32: return f'c->{n}=(uint32_t)({expr});'
        if n in R16: return f'c->{R16[n]}=(c->{R16[n]}&0xffff0000u)|(uint16_t)({expr});'
        if n in R8L: return f'c->{R8L[n]}=(c->{R8L[n]}&0xffffff00u)|(uint8_t)({expr});'
        if n in R8H: return f'c->{R8H[n]}=(c->{R8H[n]}&0xffff00ffu)|((uint32_t)(uint8_t)({expr})<<8);'
        if n in ('cs','ds','es','ss','fs','gs'): return f'(void)({expr}); /* seg write ignored */'
        raise Unsupported('wreg ' + n)

    def addr(self, i, op):
        m = op.mem; parts = []
        if m.base: parts.append(f'c->{i.reg_name(m.base)}')
        if m.index: parts.append(f'c->{i.reg_name(m.index)}*{m.scale}')
        if m.disp or not parts: parts.append(f'0x{m.disp & 0xffffffff:x}u')
        if m.segment and i.reg_name(m.segment) == 'fs': parts.append('c->fs_base')
        for p in parts:
            if 'eip' in p: raise Unsupported('eip-rel')
        return 'A32(' + '+'.join(parts) + ')'

    def rd(self, i, op, bits=None):
        if op.type == X86_OP_REG:
            e, b = self.reg(i, op.reg); return e
        if op.type == X86_OP_IMM:
            b = bits or op.size * 8
            return f'0x{op.imm & ((1 << b) - 1):x}u'
        if op.type == X86_OP_MEM:
            b = bits or op.size * 8
            return f'R{b}({self.addr(i, op)})'
        raise Unsupported('op')

    def wr(self, i, op, expr, bits=None):
        if op.type == X86_OP_REG: return self.wreg(i, op.reg, expr)
        if op.type == X86_OP_MEM:
            b = bits or op.size * 8
            return f'W{b}({self.addr(i, op)},({T(b)})({expr}));'
        raise Unsupported('wr')

    def isx(self, i, op): return op.type == X86_OP_REG and i.reg_name(op.reg).startswith('xmm')
    def xn(self, i, op):
        n = i.reg_name(op.reg) if op.type == X86_OP_REG else None
        if not n or not n.startswith('xmm'): raise Unsupported('mmx/other vector reg ' + str(n))
        return int(n[3:])

    # ---------- flags
    def fl_arith(self, kind, a, b, r, bits, need):
        s = []
        if 'zf' in need: s.append(f'c->zf=(({T(bits)}){r})==0;')
        if 'sf' in need: s.append(f'c->sf=((({T(bits)}){r})&{MSB(bits)})!=0;')
        if 'pf' in need: s.append(f'c->pf=PAR({r});')
        if kind == 'add':
            if 'cf' in need: s.append(f'c->cf=({T(bits)}){r}<({T(bits)}){a};')
            if 'of' in need: s.append(f'c->of=(((({a})^({r}))&(({b})^({r})))&{MSB(bits)})!=0;')
        elif kind == 'sub':
            if 'cf' in need: s.append(f'c->cf=({T(bits)}){a}<({T(bits)}){b};')
            if 'of' in need: s.append(f'c->of=(((({a})^({b}))&(({a})^({r})))&{MSB(bits)})!=0;')
        elif kind == 'logic':
            if 'cf' in need: s.append('c->cf=0;')
            if 'of' in need: s.append('c->of=0;')
        return ' '.join(s)

    # ---------- instruction semantics. returns (code, flags_written, flags_read)
    def sem(self, i, need):
        mn = i.mnemonic; ops = i.operands; A = i.address; nxt = A + i.size
        pre = ''
        if mn.startswith('lock '): mn = mn[5:]
        if mn.startswith('rep ') or mn.startswith('repe ') or mn.startswith('repne ') or mn.startswith('repz ') or mn.startswith('repnz '):
            mn = mn.replace('repz ','repe ').replace('repnz ','repne ')
            return self.rep(i, mn), set(), set()
        # --- data movement
        if mn == 'mov':
            return self.wr(i, ops[0], self.rd(i, ops[1], ops[0].size * 8)), set(), set()
        if mn in ('movzx', 'movsx'):
            sb = ops[1].size * 8; db = ops[0].size * 8
            v = self.rd(i, ops[1])
            e = f'({T(db)})({T(sb)})({v})' if mn == 'movzx' else f'({T(db)})({ST_(db)})({ST_(sb)})({v})'
            return self.wr(i, ops[0], e), set(), set()
        if mn == 'lea':
            return self.wr(i, ops[0], self.addr(i, ops[1]).replace('+c->fs_base', '')), set(), set()
        if mn == 'push':
            v = self.rd(i, ops[0], 32) if ops[0].type != X86_OP_IMM else f'0x{ops[0].imm & 0xffffffff:x}u'
            return f'{{uint32_t _v={v}; PUSH32(c,_v);}}', set(), set()
        if mn == 'pop':
            return f'{{uint32_t _v=POP32(c); {self.wr(i, ops[0], "_v")}}}', set(), set()
        if mn == 'xchg':
            a, b = self.rd(i, ops[0]), self.rd(i, ops[1])
            bits = ops[0].size * 8
            return f'{{{T(bits)} _a={a},_b={b}; {self.wr(i, ops[0], "_b")} {self.wr(i, ops[1], "_a")}}}', set(), set()
        if mn == 'leave':
            return 'c->esp=c->ebp; c->ebp=POP32(c);', set(), set()
        if mn in ('nop', 'fnop', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetchnta', 'pause', 'lfence', 'sfence', 'mfence', 'fwait', 'wait'):
            return '', set(), set()
        if mn == 'cdq': return 'c->edx=((int32_t)c->eax<0)?0xffffffffu:0;', set(), set()
        if mn == 'cwde': return 'c->eax=(uint32_t)(int32_t)(int16_t)c->eax;', set(), set()
        if mn == 'cbw': return 'c->eax=(c->eax&0xffff0000u)|(uint16_t)(int16_t)(int8_t)c->eax;', set(), set()
        if mn == 'bswap':
            v = self.rd(i, ops[0]); return self.wr(i, ops[0], f'__builtin_bswap32({v})'), set(), set()
        # --- arithmetic
        if mn in ('add', 'sub', 'cmp', 'adc', 'sbb'):
            bits = ops[0].size * 8
            a = self.rd(i, ops[0]); b = self.rd(i, ops[1], bits)
            W = set(FL); R = set()
            if mn in ('adc', 'sbb'): R = {'cf'}
            op = '+' if mn in ('add', 'adc') else '-'
            carry = '+c->cf' if mn == 'adc' else ('-c->cf' if mn == 'sbb' else '')
            code = f'{{{T(bits)} _a={a},_b={b},_r=({T(bits)})(_a{op}_b{carry});'
            if mn in ('adc', 'sbb'):
                cfexpr = f'(({T(bits)})(_a+_b)<_a)||(({T(bits)})(_a+_b+c->cf)<({T(bits)})(_a+_b))' if mn == 'adc' else f'(_a<_b)||(({T(bits)})(_a-_b)<c->cf)'
                fl = self.fl_arith('add' if mn == 'adc' else 'sub', '_a', '_b', '_r', bits, need - {'cf'})
                if 'cf' in need: fl += f' c->cf={cfexpr};'
            else:
                fl = self.fl_arith('add' if mn == 'add' else 'sub', '_a', '_b', '_r', bits, need)
            if mn != 'cmp': code += ' ' + self.wr(i, ops[0], '_r')
            return code + ' ' + fl + '}', W, R
        if mn in ('and', 'or', 'xor', 'test'):
            bits = ops[0].size * 8
            if mn == 'xor' and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
                a = b = None; code = f'{{{T(bits)} _r=0; {self.wr(i, ops[0], "0")}'
            else:
                a = self.rd(i, ops[0]); b = self.rd(i, ops[1], bits)
                o = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
                code = f'{{{T(bits)} _r=({T(bits)})(({a}){o}({b}));'
                if mn != 'test': code += ' ' + self.wr(i, ops[0], '_r')
            return code + ' ' + self.fl_arith('logic', None, None, '_r', bits, need) + '}', set(FL), set()
        if mn in ('inc', 'dec'):
            bits = ops[0].size * 8; a = self.rd(i, ops[0])
            op = '+' if mn == 'inc' else '-'
            code = f'{{{T(bits)} _a={a},_r=({T(bits)})(_a{op}1); {self.wr(i, ops[0], "_r")}'
            fl = self.fl_arith('logic', None, None, '_r', bits, need - {'cf', 'of'})
            if 'of' in need: fl += f' c->of=(_r=={MSB(bits) if mn == "inc" else "(" + MSB(bits) + "-1u)"});'
            return code + ' ' + fl + '}', {'zf', 'sf', 'of', 'pf'}, set()
        if mn == 'neg':
            bits = ops[0].size * 8; a = self.rd(i, ops[0])
            code = f'{{{T(bits)} _a={a},_r=({T(bits)})(0u-_a); {self.wr(i, ops[0], "_r")}'
            fl = self.fl_arith('sub', '0u', '_a', '_r', bits, need - {'cf'})
            if 'cf' in need: fl += ' c->cf=_a!=0;'
            return code + ' ' + fl + '}', set(FL), set()
        if mn == 'not':
            bits = ops[0].size * 8
            return self.wr(i, ops[0], f'~({self.rd(i, ops[0])})'), set(), set()
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror'):
            bits = ops[0].size * 8
            a = self.rd(i, ops[0])
            cnt = self.rd(i, ops[1], 8) if len(ops) > 1 else '1u'
            code = f'{{uint32_t _n=({cnt})&31; if(_n){{ {T(bits)} _a={a},_r;'
            if mn in ('shl', 'sal'):
                code += f' _r=({T(bits)})((uint32_t)_a<<_n); c->cf=_n<={bits}?(((uint32_t)_a>>({bits}-_n))&1):0; c->of=((_r&{MSB(bits)})!=0)^c->cf;'
            elif mn == 'shr':
                code += f' _r=({T(bits)})((uint32_t)_a>>_n); c->cf=((uint32_t)_a>>(_n-1))&1; c->of=(_a&{MSB(bits)})!=0;'
            elif mn == 'sar':
                code += f' _r=({T(bits)})((int32_t)({ST_(bits)})_a>>(_n>={bits}?{bits - 1}:_n)); c->cf=((int32_t)({ST_(bits)})_a>>(_n-1>={bits}?{bits - 1}:_n-1))&1; c->of=0;'
            elif mn == 'rol':
                code += f' uint32_t _m=_n%{bits}; _r=({T(bits)})((_a<<_m)|(_a>>(({bits}-_m)%{bits}))); c->cf=_r&1; c->of=((_r&{MSB(bits)})!=0)^c->cf;'
            elif mn == 'ror':
                code += f' uint32_t _m=_n%{bits}; _r=({T(bits)})((_a>>_m)|(_a<<(({bits}-_m)%{bits}))); c->cf=(_r&{MSB(bits)})!=0;'
            code += ' ' + self.wr(i, ops[0], '_r')
            if mn not in ('rol', 'ror'):
                code += f' c->zf=_r==0; c->sf=(_r&{MSB(bits)})!=0; c->pf=PAR(_r);'
            code += '}}'
            # shifts by 0 leave flags alone -> they conditionally write; treat as read+write (conservative)
            return code, set(FL) if mn not in ('rol', 'ror') else {'cf', 'of'}, set(FL)
        if mn in ('shld', 'shrd'):
            a = self.rd(i, ops[0]); b = self.rd(i, ops[1]); cnt = self.rd(i, ops[2], 8)
            if mn == 'shld':
                e = '((_a<<_n)|((uint32_t)_b>>(32-_n)))'; cf = '(_a>>(32-_n))&1'
            else:
                e = '((_a>>_n)|((uint32_t)_b<<(32-_n)))'; cf = '(_a>>(_n-1))&1'
            return (f'{{uint32_t _n=({cnt})&31; if(_n){{ uint32_t _a={a},_b={b},_r={e}; c->cf={cf}; {self.wr(i, ops[0], "_r")}'
                    f' c->zf=_r==0; c->sf=_r>>31; c->pf=PAR(_r); c->of=0;}}}}'), set(FL), set(FL)
        if mn == 'imul':
            if len(ops) == 1:
                bits = ops[0].size * 8
                if bits == 8:
                    return (f'{{int16_t _r=(int16_t)(int8_t)c->eax*(int16_t)(int8_t)({self.rd(i, ops[0])}); c->eax=(c->eax&0xffff0000u)|(uint16_t)_r;'
                            f' c->cf=c->of=(_r!=(int16_t)(int8_t)_r);}}'), set(FL), set()
                if bits != 32: raise Unsupported('imul1 ' + str(bits))
                return (f'{{int64_t _r=(int64_t)(int32_t)c->eax*(int64_t)(int32_t)({self.rd(i, ops[0])}); c->eax=(uint32_t)_r; c->edx=(uint32_t)(_r>>32);'
                        f' c->cf=c->of=(_r!=(int64_t)(int32_t)_r);}}'), set(FL), set()
            bits = ops[0].size * 8
            a = self.rd(i, ops[1] if len(ops) == 3 else ops[0]); b = self.rd(i, ops[2] if len(ops) == 3 else ops[1], bits)
            return (f'{{int64_t _r=(int64_t)({ST_(bits)})({a})*(int64_t)({ST_(bits)})({b}); {self.wr(i, ops[0], "(" + T(bits) + ")_r")}'
                    f' c->cf=c->of=(_r!=(int64_t)({ST_(bits)})_r); c->zf=({T(bits)})_r==0; c->sf=(({T(bits)})_r&{MSB(bits)})!=0;}}'), set(FL), set()
        if mn == 'mul':
            bits = ops[0].size * 8
            if bits != 32: raise Unsupported('mul ' + str(bits))
            return (f'{{uint64_t _r=(uint64_t)c->eax*(uint64_t)({self.rd(i, ops[0])}); c->eax=(uint32_t)_r; c->edx=(uint32_t)(_r>>32);'
                    f' c->cf=c->of=(c->edx!=0);}}'), set(FL), set()
        if mn in ('div', 'idiv'):
            bits = ops[0].size * 8
            if bits != 32: raise Unsupported('div ' + str(bits))
            if mn == 'div':
                return (f'{{uint64_t _n=((uint64_t)c->edx<<32)|c->eax; uint32_t _d={self.rd(i, ops[0])}; if(!_d) unimpl(c,0x{A:x},"div0");'
                        f' else {{ c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%_d);}} }}'), set(FL), set()
            return (f'{{int64_t _n=(int64_t)(((uint64_t)c->edx<<32)|c->eax); int32_t _d=(int32_t){self.rd(i, ops[0])}; if(!_d) unimpl(c,0x{A:x},"div0");'
                    f' else {{ c->eax=(uint32_t)(int32_t)(_n/_d); c->edx=(uint32_t)(int32_t)(_n%_d);}} }}'), set(FL), set()
        if mn.startswith('set'):
            cc = ccof(mn[3:])
            if cc: return self.wr(i, ops[0], f'({cc[0]})?1:0', 8), set(), cc[1]
        if mn.startswith('cmov'):
            cc = ccof(mn[4:])
            if cc: return f'if({cc[0]}) {{ {self.wr(i, ops[0], self.rd(i, ops[1]))} }}', set(), cc[1]
        if mn in ('bt', 'bts', 'btr', 'btc'):
            if ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG: raise Unsupported('bt mem,reg')
            bits = ops[0].size * 8
            a = self.rd(i, ops[0]); b = self.rd(i, ops[1], 8 if ops[1].type == X86_OP_IMM else None)
            code = f'{{uint32_t _b=({b})%{bits}; {T(bits)} _a={a}; c->cf=(_a>>_b)&1;'
            if mn == 'bts': code += ' ' + self.wr(i, ops[0], f'_a|(({T(bits)})1<<_b)')
            if mn == 'btr': code += ' ' + self.wr(i, ops[0], f'_a&~(({T(bits)})1<<_b)')
            if mn == 'btc': code += ' ' + self.wr(i, ops[0], f'_a^(({T(bits)})1<<_b)')
            return code + '}', {'cf'}, set()
        if mn in ('bsf', 'bsr'):
            v = self.rd(i, ops[1])
            e = '__builtin_ctz(_v)' if mn == 'bsf' else '(31-__builtin_clz(_v))'
            return f'{{uint32_t _v={v}; c->zf=_v==0; if(_v){{ {self.wr(i, ops[0], e)} }}}}', {'zf'}, set()
        if mn in ('xadd', 'cmpxchg'):
            bits = ops[0].size * 8
            if mn == 'xadd':
                a = self.rd(i, ops[0]); b = self.rd(i, ops[1])
                return (f'{{{T(bits)} _a={a},_b={b},_r=({T(bits)})(_a+_b); {self.wr(i, ops[1], "_a")} {self.wr(i, ops[0], "_r")} '
                        + self.fl_arith('add', '_a', '_b', '_r', bits, need) + '}'), set(FL), set()
            a = self.rd(i, ops[0]); b = self.rd(i, ops[1]); acc = {8: '(uint8_t)c->eax', 16: '(uint16_t)c->eax', 32: 'c->eax'}[bits]
            wacc = {32: 'c->eax=_a;', 16: 'c->eax=(c->eax&0xffff0000u)|_a;', 8: 'c->eax=(c->eax&0xffffff00u)|_a;'}[bits]
            return (f'{{{T(bits)} _a={a},_x={acc},_r=({T(bits)})(_x-_a); ' + self.fl_arith('sub', '_x', '_a', '_r', bits, need | {'zf'}) +
                    f' if(c->zf){{ {self.wr(i, ops[0], b)} }} else {{ {wacc} }} }}'), set(FL), set()
        if mn == 'cld': return 'c->df=0;', set(), set()
        if mn == 'stc': return 'c->cf=1;', {'cf'}, set()
        if mn == 'clc': return 'c->cf=0;', {'cf'}, set()
        if mn == 'cmc': return 'c->cf=!c->cf;', {'cf'}, {'cf'}
        if mn == 'cpuid': return 'host_cpuid(c);', set(), set()
        if mn == 'rdtsc': return 'host_rdtsc(c);', set(), set()
        if mn == 'pushfd': return '{uint32_t _f=2|c->cf|(c->pf<<2)|(c->zf<<6)|(c->sf<<7)|(c->df<<10)|(c->of<<11); PUSH32(c,_f);}', set(), set(FL)
        if mn == 'popfd': return '{uint32_t _f=POP32(c); c->cf=_f&1; c->pf=(_f>>2)&1; c->zf=(_f>>6)&1; c->sf=(_f>>7)&1; c->df=(_f>>10)&1; c->of=(_f>>11)&1;}', set(FL), set()
        if mn in ('rcr','rcl'):
            bits = ops[0].size * 8; a = self.rd(i, ops[0]); cnt = self.rd(i, ops[1], 8) if len(ops) > 1 else '1u'
            if mn == 'rcr': step = f'{{uint32_t _lo=_a&1; _a=(_a>>1)|((uint32_t)c->cf<<({bits}-1)); c->cf=_lo;}}'
            else: step = f'{{uint32_t _hi=(_a>>({bits}-1))&1; _a=((_a<<1)|c->cf)&{MASK(bits)}; c->cf=_hi;}}'
            return f'{{uint32_t _n=({cnt})&31; uint32_t _a={a}; for(uint32_t _k=0;_k<_n%({bits}+1);_k++) {step} {self.wr(i, ops[0], "_a")}}}', {'cf'}, {'cf'}
        if mn == 'std': return 'c->df=1;', set(), set()
        if mn == 'lahf': return 'c->eax=(c->eax&0xffff00ffu)|((uint32_t)((c->sf<<7)|(c->zf<<6)|(c->pf<<2)|2|c->cf)<<8);', set(), set(FL)
        if mn == 'sahf': return '{uint32_t _h=c->eax>>8; c->sf=(_h>>7)&1; c->zf=(_h>>6)&1; c->pf=(_h>>2)&1; c->cf=_h&1;}', {'cf', 'zf', 'sf', 'pf'}, set()
        if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsd', 'scasb') and (mn != 'movsd' or not any(self.isx(i, o) for o in ops)):
            return self.strop(i, mn, 1), set(), set()
        if mn == 'int3': return f'unimpl(c,0x{A:x},"int3");', set(), set()
        # --- SSE / MMX
        import vec_ops
        if any(vec_ops.is_mm(i, o) for o in ops) or mn == 'emms':
            r = vec_ops.Vec(self).emit(i, mn, ops)
            if r is not None: return r, set(), set()
            raise Unsupported(mn)
        r = self.sse(i, mn, ops, need)
        if r is not None: return r
        r = vec_ops.Vec(self).emit(i, mn, ops)
        if r is not None: return r, set(), set()
        # --- x87
        r = self.x87(i, mn, ops, need)
        if r is not None: return r
        raise Unsupported(mn)

    def strop(self, i, mn, count_expr):
        sz = {'b': 1, 'w': 2, 'd': 4}[mn[-1]]; bits = sz * 8; st = f'(c->df?-{sz}:{sz})'
        base = mn[:-1]
        if base == 'movs': return f'W{bits}(c->edi,R{bits}(c->esi)); c->esi+={st}; c->edi+={st};'
        if base == 'stos': return f'W{bits}(c->edi,({T(bits)})c->eax); c->edi+={st};'
        if base == 'lods': return f'c->eax={"(c->eax&0xffffff00u)|" if bits == 8 else ""}R{bits}(c->esi); c->esi+={st};'
        raise Unsupported(mn)

    def rep(self, i, mn):
        pfx, op = mn.split(' ', 1)
        if op in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd') and pfx == 'rep':
            return f'while(c->ecx){{ {self.strop(i, op, 1)} c->ecx--; }}'
        if op in ('cmpsb', 'cmpsd', 'scasb') and pfx in ('repe', 'repne'):
            sz = {'b': 1, 'd': 4}[op[-1]]; bits = sz * 8; st = f'(c->df?-{sz}:{sz})'
            if op.startswith('cmps'):
                body = f'{T(bits)} _a=R{bits}(c->esi),_b=R{bits}(c->edi); c->esi+={st}; c->edi+={st};'
            else:
                body = f'{T(bits)} _a=({T(bits)})c->eax,_b=R{bits}(c->edi); c->edi+={st};'
            body += f' {T(bits)} _r=_a-_b; c->zf=_r==0; c->cf=_a<_b; c->sf=(_r&{MSB(bits)})!=0; c->of=(((_a^_b)&(_a^_r))&{MSB(bits)})!=0; c->ecx--;'
            cond = 'c->zf' if pfx == 'repe' else '!c->zf'
            return f'while(c->ecx){{ {body} if(!({cond})) break; }}'
        raise Unsupported(mn)

    # ---------- SSE
    def xr(self, i, op, lanes='f'):
        return f'c->xmm[{self.xn(i, op)}]'
    def sse(self, i, mn, ops, need):
        isx = lambda o: self.isx(i, o)
        def src32f(o):
            return f'c->xmm[{self.xn(i, o)}].f[0]' if isx(o) else f'RF32({self.addr(i, o)})'
        def src64f(o):
            return f'c->xmm[{self.xn(i, o)}].d[0]' if isx(o) else f'RF64({self.addr(i, o)})'
        def src128(o):  # returns C statement loading into XMM _s
            return f'XMM _s=c->xmm[{self.xn(i, o)}];' if isx(o) else f'XMM _s; memcpy(&_s,MEMP({self.addr(i, o)}),16);'
        if mn in ('movss',):
            if isx(ops[0]) and isx(ops[1]): return f'c->xmm[{self.xn(i, ops[0])}].u32[0]=c->xmm[{self.xn(i, ops[1])}].u32[0];', set(), set()
            if isx(ops[0]): return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; _d->u64[0]=R32({self.addr(i, ops[1])}); _d->u64[1]=0;}}', set(), set()
            return f'W32({self.addr(i, ops[0])},c->xmm[{self.xn(i, ops[1])}].u32[0]);', set(), set()
        if mn == 'movsd':
            if isx(ops[0]) and isx(ops[1]): return f'c->xmm[{self.xn(i, ops[0])}].u64[0]=c->xmm[{self.xn(i, ops[1])}].u64[0];', set(), set()
            if isx(ops[0]): return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; _d->u64[0]=R64({self.addr(i, ops[1])}); _d->u64[1]=0;}}', set(), set()
            return f'W64({self.addr(i, ops[0])},c->xmm[{self.xn(i, ops[1])}].u64[0]);', set(), set()
        if mn in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd', 'movntps', 'movntpd', 'movntdq', 'lddqu'):
            if isx(ops[0]): return f'{{{src128(ops[1])} c->xmm[{self.xn(i, ops[0])}]=_s;}}', set(), set()
            return f'memcpy(MEMP({self.addr(i, ops[0])}),&c->xmm[{self.xn(i, ops[1])}],16);', set(), set()
        if mn in ('movq',):
            if isx(ops[0]) and isx(ops[1]): return f'c->xmm[{self.xn(i, ops[0])}].u64[0]=c->xmm[{self.xn(i, ops[1])}].u64[0]; c->xmm[{self.xn(i, ops[0])}].u64[1]=0;', set(), set()
            if isx(ops[0]): return f'c->xmm[{self.xn(i, ops[0])}].u64[0]=R64({self.addr(i, ops[1])}); c->xmm[{self.xn(i, ops[0])}].u64[1]=0;', set(), set()
            return f'W64({self.addr(i, ops[0])},c->xmm[{self.xn(i, ops[1])}].u64[0]);', set(), set()
        if mn == 'movd':
            if isx(ops[0]): return f'c->xmm[{self.xn(i, ops[0])}].u64[0]=(uint32_t)({self.rd(i, ops[1], 32)}); c->xmm[{self.xn(i, ops[0])}].u64[1]=0;', set(), set()
            return self.wr(i, ops[0], f'c->xmm[{self.xn(i, ops[1])}].u32[0]', 32), set(), set()
        if mn in ('movlps', 'movhps', 'movlpd', 'movhpd'):
            h = 1 if 'h' in mn[3] else 0
            if isx(ops[0]): return f'c->xmm[{self.xn(i, ops[0])}].u64[{h}]=R64({self.addr(i, ops[1])});', set(), set()
            return f'W64({self.addr(i, ops[0])},c->xmm[{self.xn(i, ops[1])}].u64[{h}]);', set(), set()
        if mn == 'movlhps': return f'c->xmm[{self.xn(i, ops[0])}].u64[1]=c->xmm[{self.xn(i, ops[1])}].u64[0];', set(), set()
        if mn == 'movhlps': return f'c->xmm[{self.xn(i, ops[0])}].u64[0]=c->xmm[{self.xn(i, ops[1])}].u64[1];', set(), set()
        ss = {'addss': '+', 'subss': '-', 'mulss': '*', 'divss': '/'}
        if mn in ss: return f'c->xmm[{self.xn(i, ops[0])}].f[0]=c->xmm[{self.xn(i, ops[0])}].f[0]{ss[mn]}{src32f(ops[1])};', set(), set()
        sd = {'addsd': '+', 'subsd': '-', 'mulsd': '*', 'divsd': '/'}
        if mn in sd: return f'c->xmm[{self.xn(i, ops[0])}].d[0]=c->xmm[{self.xn(i, ops[0])}].d[0]{sd[mn]}{src64f(ops[1])};', set(), set()
        if mn in ('minss', 'maxss'):
            cmp = '<' if mn == 'minss' else '>'
            return f'{{float _a=c->xmm[{self.xn(i, ops[0])}].f[0],_b={src32f(ops[1])}; c->xmm[{self.xn(i, ops[0])}].f[0]=(_a{cmp}_b)?_a:_b;}}', set(), set()
        if mn == 'sqrtss': return f'c->xmm[{self.xn(i, ops[0])}].f[0]=sqrtf({src32f(ops[1])});', set(), set()
        if mn == 'sqrtsd': return f'c->xmm[{self.xn(i, ops[0])}].d[0]=sqrt({src64f(ops[1])});', set(), set()
        if mn == 'rcpss': return f'c->xmm[{self.xn(i, ops[0])}].f[0]=1.0f/({src32f(ops[1])});', set(), set()
        if mn == 'rsqrtss': return f'c->xmm[{self.xn(i, ops[0])}].f[0]=1.0f/sqrtf({src32f(ops[1])});', set(), set()
        ps = {'addps': '+', 'subps': '-', 'mulps': '*', 'divps': '/'}
        if mn in ps:
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->f[_k]=_d->f[_k]{ps[mn]}_s.f[_k];}}', set(), set()
        if mn in ('minps', 'maxps'):
            cmp = '<' if mn == 'minps' else '>'
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->f[_k]=(_d->f[_k]{cmp}_s.f[_k])?_d->f[_k]:_s.f[_k];}}', set(), set()
        if mn in ('sqrtps', 'rsqrtps', 'rcpps'):
            e = {'sqrtps': 'sqrtf(_s.f[_k])', 'rsqrtps': '1.0f/sqrtf(_s.f[_k])', 'rcpps': '1.0f/_s.f[_k]'}[mn]
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->f[_k]={e};}}', set(), set()
        bw = {'xorps': '^', 'xorpd': '^', 'pxor': '^', 'andps': '&', 'andpd': '&', 'pand': '&', 'orps': '|', 'orpd': '|', 'por': '|'}
        if mn in bw:
            if mn in ('xorps', 'pxor', 'xorpd') and isx(ops[1]) and ops[0].reg == ops[1].reg:
                return f'memset(&c->xmm[{self.xn(i, ops[0])}],0,16);', set(), set()
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; _d->u64[0]{bw[mn]}=_s.u64[0]; _d->u64[1]{bw[mn]}=_s.u64[1];}}', set(), set()
        if mn in ('andnps', 'andnpd', 'pandn'):
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; _d->u64[0]=~_d->u64[0]&_s.u64[0]; _d->u64[1]=~_d->u64[1]&_s.u64[1];}}', set(), set()
        if mn in ('shufps', 'pshufd'):
            imm = ops[2].imm & 0xff
            if mn == 'shufps':
                return (f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; _t.u32[0]=_d->u32[{imm & 3}]; _t.u32[1]=_d->u32[{(imm >> 2) & 3}];'
                        f' _t.u32[2]=_s.u32[{(imm >> 4) & 3}]; _t.u32[3]=_s.u32[{(imm >> 6) & 3}]; *_d=_t;}}'), set(), set()
            return (f'{{{src128(ops[1])} XMM _t; for(int _k=0;_k<4;_k++) _t.u32[_k]=_s.u32[({imm}>>(2*_k))&3]; c->xmm[{self.xn(i, ops[0])}]=_t;}}'), set(), set()
        if mn in ('unpcklps', 'unpckhps'):
            h = 2 if mn == 'unpckhps' else 0
            return (f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; _t.u32[0]=_d->u32[{h}]; _t.u32[1]=_s.u32[{h}]; _t.u32[2]=_d->u32[{h + 1}]; _t.u32[3]=_s.u32[{h + 1}]; *_d=_t;}}'), set(), set()
        if mn in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
            s = src32f if mn.endswith('ss') else src64f
            a = f'c->xmm[{self.xn(i, ops[0])}].{"f" if mn.endswith("ss") else "d"}[0]'
            return (f'{{double _a={a},_b={s(ops[1])}; if(_a!=_a||_b!=_b){{c->zf=c->pf=c->cf=1;}} else {{c->pf=0; c->zf=_a==_b; c->cf=_a<_b;}} c->of=c->sf=0;}}'), set(FL), set()
        if mn == 'cvtsi2ss': return f'c->xmm[{self.xn(i, ops[0])}].f[0]=(float)(int32_t)({self.rd(i, ops[1], 32)});', set(), set()
        if mn == 'cvtsi2sd': return f'c->xmm[{self.xn(i, ops[0])}].d[0]=(double)(int32_t)({self.rd(i, ops[1], 32)});', set(), set()
        if mn == 'cvttss2si': return self.wr(i, ops[0], f'(uint32_t)cvtt_f2i({src32f(ops[1])})'), set(), set()
        if mn == 'cvtss2si': return self.wr(i, ops[0], f'(uint32_t)cvt_f2i({src32f(ops[1])})'), set(), set()
        if mn == 'cvttsd2si': return self.wr(i, ops[0], f'(uint32_t)cvtt_f2i({src64f(ops[1])})'), set(), set()
        if mn == 'cvtsd2si': return self.wr(i, ops[0], f'(uint32_t)cvt_f2i({src64f(ops[1])})'), set(), set()
        if mn == 'cvtss2sd': return f'c->xmm[{self.xn(i, ops[0])}].d[0]=(double)({src32f(ops[1])});', set(), set()
        if mn == 'cvtsd2ss': return f'c->xmm[{self.xn(i, ops[0])}].f[0]=(float)({src64f(ops[1])});', set(), set()
        if mn == 'cvtdq2ps': return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->f[_k]=(float)_s.i32[_k];}}', set(), set()
        if mn == 'cvttps2dq': return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->i32[_k]=cvtt_f2i(_s.f[_k]);}}', set(), set()
        if mn == 'movmskps': return self.wr(i, ops[0], f'(c->xmm[{self.xn(i, ops[1])}].u32[0]>>31)|((c->xmm[{self.xn(i, ops[1])}].u32[1]>>31)<<1)|((c->xmm[{self.xn(i, ops[1])}].u32[2]>>31)<<2)|((c->xmm[{self.xn(i, ops[1])}].u32[3]>>31)<<3)'), set(), set()
        if mn in ('cmpss', 'cmpps') or re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ss|ps)', mn):
            m = re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(ss|ps)', mn)
            pred = m.group(1) if m.group(1) else ['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord'][ops[2].imm & 7]
            e = {'eq': '_a==_b', 'lt': '_a<_b', 'le': '_a<=_b', 'unord': '(_a!=_a||_b!=_b)', 'neq': '!(_a==_b)', 'nlt': '!(_a<_b)', 'nle': '!(_a<=_b)', 'ord': '!(_a!=_a||_b!=_b)'}[pred]
            n = 1 if m.group(2) == 'ss' else 4
            srcld = f'XMM _s; _s.f[0]={src32f(ops[1])};' if n == 1 else src128(ops[1])
            return f'{{{srcld} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<{n};_k++){{ float _a=_d->f[_k],_b=_s.f[_k]; _d->u32[_k]=({e})?0xffffffffu:0; }}}}', set(), set()
        if mn in ('paddd', 'psubd'):
            o = '+' if mn == 'paddd' else '-'
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->u32[_k]{o}=_s.u32[_k];}}', set(), set()
        if mn in ('pslld', 'psrld', 'psrad', 'psllq', 'psrlq') and ops[1].type == X86_OP_IMM:
            n = ops[1].imm
            if mn == 'pslld': e = f'_d->u32[_k]=(n>31)?0:_d->u32[_k]<<n'.replace('n', str(n))
            elif mn == 'psrld': e = f'_d->u32[_k]=({n}>31)?0:_d->u32[_k]>>{n}'
            elif mn == 'psrad': e = f'_d->i32[_k]=_d->i32[_k]>>({n}>31?31:{n})'
            elif mn == 'psllq': return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<2;_k++) _d->u64[_k]=({n}>63)?0:_d->u64[_k]<<{n};}}', set(), set()
            else: return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<2;_k++) _d->u64[_k]=({n}>63)?0:_d->u64[_k]>>{n};}}', set(), set()
            return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) {e};}}', set(), set()
        if mn in ('punpckldq', 'unpcklpd', 'punpcklqdq'):
            if mn == 'punpckldq':
                return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; _t.u32[0]=_d->u32[0]; _t.u32[1]=_s.u32[0]; _t.u32[2]=_d->u32[1]; _t.u32[3]=_s.u32[1]; *_d=_t;}}', set(), set()
            return f'{{{src128(ops[1])} c->xmm[{self.xn(i, ops[0])}].u64[1]=_s.u64[0];}}', set(), set()
        if mn == 'cvtps2pd':
            ld = f'XMM _s; _s.u64[0]=R64({self.addr(i, ops[1])});' if not isx(ops[1]) else f'XMM _s=c->xmm[{self.xn(i, ops[1])}];'
            return f'{{{ld} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; double _x=_s.f[0],_y=_s.f[1]; _d->d[0]=_x; _d->d[1]=_y;}}', set(), set()
        if mn == 'cvtpd2ps':
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; _d->f[0]=(float)_s.d[0]; _d->f[1]=(float)_s.d[1]; _d->u64[1]=0;}}', set(), set()
        if mn == 'cvtps2dq': return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->i32[_k]=cvt_f2i(_s.f[_k]);}}', set(), set()
        if mn in ('punpcklbw','punpcklwd','punpckhwd','punpckhbw'):
            w = 1 if 'bw' in mn else 2; hi = 'h' in mn[5]
            n = 16//w//2; off = n if hi else 0
            return (f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; for(int _k=0;_k<{n};_k++){{ memcpy(&_t.u8[2*_k*{w}],&_d->u8[({off}+_k)*{w}],{w}); memcpy(&_t.u8[(2*_k+1)*{w}],&_s.u8[({off}+_k)*{w}],{w}); }} *_d=_t;}}'), set(), set()
        if mn in ('pcmpeqd','pcmpgtd'):
            o = '==' if mn=='pcmpeqd' else '>'
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<4;_k++) _d->u32[_k]=(_d->i32[_k]{o}_s.i32[_k])?0xffffffffu:0;}}', set(), set()
        if mn == 'pcmpgtw':
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<8;_k++) _d->u16[_k]=((int16_t)_d->u16[_k]>(int16_t)_s.u16[_k])?0xffff:0;}}', set(), set()
        if mn == 'paddw':
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; for(int _k=0;_k<8;_k++) _d->u16[_k]+=_s.u16[_k];}}', set(), set()
        if mn in ('packuswb','packusdw','packssdw'):
            if mn=='packuswb': n,sw,dw,lo,hi,st,dt=8,2,1,0,255,'int16_t','uint8_t'
            elif mn=='packusdw': n,sw,dw,lo,hi,st,dt=4,4,2,0,65535,'int32_t','uint16_t'
            else: n,sw,dw,lo,hi,st,dt=4,4,2,-32768,32767,'int32_t','int16_t'
            return (f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; for(int _k=0;_k<{2*n};_k++){{ {st} _v; if(_k<{n}) memcpy(&_v,&_d->u8[_k*{sw}],{sw}); else memcpy(&_v,&_s.u8[(_k-{n})*{sw}],{sw});'
                    f' if(_v<{lo}) _v={lo}; if(_v>{hi}) _v={hi}; {dt} _o=({dt})_v; memcpy(&_t.u8[_k*{dw}],&_o,{dw}); }} *_d=_t;}}'), set(), set()
        if mn == 'pslldq':
            n = ops[1].imm
            return f'{{XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; XMM _t; memset(&_t,0,16); if({n}<16) memcpy(&_t.u8[{n}],_d->u8,16-{n}); *_d=_t;}}', set(), set()
        if mn in ('psllq','psrlq') and ops[1].type != X86_OP_IMM:
            o = '<<' if mn=='psllq' else '>>'
            return f'{{{src128(ops[1])} XMM*_d=&c->xmm[{self.xn(i, ops[0])}]; uint64_t _n=_s.u64[0]; for(int _k=0;_k<2;_k++) _d->u64[_k]=(_n>63)?0:_d->u64[_k]{o}_n;}}', set(), set()
        if mn == 'ldmxcsr': return f'host_ldmxcsr(c,R32({self.addr(i, ops[0])}));', set(), set()
        if mn == 'stmxcsr': return f'W32({self.addr(i, ops[0])},host_stmxcsr(c));', set(), set()
        return None


    # ---------- x87 (values kept as double)
    def fmem(self, i, op):
        b = op.size * 8
        if b == 32: return f'(double)RF32({self.addr(i, op)})'
        if b == 64: return f'RF64({self.addr(i, op)})'
        if b == 80: return f'RF80({self.addr(i, op)})'
        raise Unsupported('fmem80')
    def fst_idx(self, i, op):
        n = i.reg_name(op.reg)
        m = re.fullmatch(r'st\((\d)\)', n)
        if not m: raise Unsupported('streg ' + n)
        return int(m.group(1))
    def x87(self, i, mn, ops, need):
        if not mn.startswith('f'): return None
        FS = '(c->fsw&~0x4500u)'
        def fcmp(a, b):
            return f'{{double _a={a},_b={b}; c->fsw={FS}|((_a!=_a||_b!=_b)?0x4500u:(_a<_b?0x100u:(_a==_b?0x4000u:0)));}}'
        if mn == 'fld':
            if ops[0].type == X86_OP_MEM: return f'FPUSH(c,{self.fmem(i, ops[0])});', set(), set()
            return f'{{double _v=ST(c,{self.fst_idx(i, ops[0])}); FPUSH(c,_v);}}', set(), set()
        if mn == 'fild':
            b = ops[0].size * 8
            return f'FPUSH(c,(double)({ST_(b)})R{b}({self.addr(i, ops[0])}));', set(), set()
        if mn in ('fld1', 'fldz', 'fldpi', 'fldl2e', 'fldln2'):
            v = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': 'M_PI', 'fldl2e': 'M_LOG2E', 'fldln2': 'M_LN2'}[mn]
            return f'FPUSH(c,{v});', set(), set()
        if mn in ('fst', 'fstp'):
            pop = ' FPOP(c);' if mn == 'fstp' else ''
            if ops[0].type == X86_OP_MEM:
                b = ops[0].size * 8
                if b == 32: return f'WF32({self.addr(i, ops[0])},(float)ST(c,0));{pop}', set(), set()
                if b == 64: return f'WF64({self.addr(i, ops[0])},ST(c,0));{pop}', set(), set()
                if b == 80: return f'WF80({self.addr(i, ops[0])},ST(c,0));{pop}', set(), set()
                raise Unsupported('fst80')
            return f'ST(c,{self.fst_idx(i, ops[0])})=ST(c,0);{pop}', set(), set()
        if mn in ('fist', 'fistp', 'fisttp'):
            b = ops[0].size * 8; pop = ' FPOP(c);' if mn != 'fist' else ''
            e = 'trunc(ST(c,0))' if mn == 'fisttp' else 'fpu_round(c,ST(c,0))'
            return f'W{b}({self.addr(i, ops[0])},({T(b)})({ST_(b)})({e}));{pop}', set(), set()
        if mn == 'fxch':
            m = re.findall(r'st\((\d)\)', i.op_str); k = max([int(x) for x in m] or [1])
            return f'{{double _t=ST(c,0); ST(c,0)=ST(c,{k}); ST(c,{k})=_t;}}', set(), set()
        un = {'fchs': '-ST(c,0)', 'fabs': 'fabs(ST(c,0))', 'fsqrt': 'sqrt(ST(c,0))', 'fsin': 'sin(ST(c,0))', 'fcos': 'cos(ST(c,0))', 'frndint': 'fpu_round(c,ST(c,0))'}
        if mn in un: return f'ST(c,0)={un[mn]};', set(), set()
        if mn == 'fpatan': return 'ST(c,1)=atan2(ST(c,1),ST(c,0)); FPOP(c);', set(), set()
        if mn == 'fyl2x': return 'ST(c,1)=ST(c,1)*log2(ST(c,0)); FPOP(c);', set(), set()
        if mn == 'f2xm1': return 'ST(c,0)=exp2(ST(c,0))-1.0;', set(), set()
        if mn == 'fscale': return 'ST(c,0)=ldexp(ST(c,0),(int)trunc(ST(c,1)));', set(), set()
        if mn == 'fprem': return 'ST(c,0)=fmod(ST(c,0),ST(c,1)); c->fsw&=~0x400u;', set(), set()
        if mn == 'fsincos': return '{double _v=ST(c,0); ST(c,0)=sin(_v); FPUSH(c,cos(_v));}', set(), set()
        if mn == 'fptan': return 'ST(c,0)=tan(ST(c,0)); FPUSH(c,1.0);', set(), set()
        ar = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': 'r-', 'fdivr': 'r/'}
        base = mn[:-1] if mn.endswith('p') and mn[:-1] in ar else mn
        if base in ar or (base.startswith('fi') and 'f' + base[2:] in ar):
            isint = base.startswith('fi') and base not in ar
            opx = ar['f' + base[2:]] if isint else ar[base]
            rev = opx.startswith('r'); o = opx[-1]
            pop = mn.endswith('p') and not isint
            def comb(d, s):
                return f'({s}{o}{d})' if rev else f'({d}{o}{s})'
            if isint:
                b = ops[0].size * 8
                return f'ST(c,0)={comb("ST(c,0)", f"(double)({ST_(b)})R{b}({self.addr(i, ops[0])})")};', set(), set()
            if len(ops) == 1 and ops[0].type == X86_OP_MEM:
                return f'ST(c,0)={comb("ST(c,0)", self.fmem(i, ops[0]))};', set(), set()
            if len(ops) == 0:  # faddp with no operands = st1 op st0, pop
                return f'ST(c,1)={comb("ST(c,1)", "ST(c,0)")}; FPOP(c);', set(), set()
            if len(ops) == 1:
                k = self.fst_idx(i, ops[0])
                d, s = (k, 0) if pop else (0, k)
            else:
                d, s = self.fst_idx(i, ops[0]), self.fst_idx(i, ops[1])
            code = f'ST(c,{d})={comb(f"ST(c,{d})", f"ST(c,{s})")};'
            if pop: code += ' FPOP(c);'
            return code, set(), set()
        if mn in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp', 'ficom', 'ficomp', 'ftst'):
            if mn == 'ftst': return fcmp('ST(c,0)', '0.0'), set(), set()
            if mn.startswith('fi'):
                b = ops[0].size * 8; src = f'(double)({ST_(b)})R{b}({self.addr(i, ops[0])})'
            elif ops and ops[0].type == X86_OP_MEM: src = self.fmem(i, ops[0])
            elif ops: src = f'ST(c,{self.fst_idx(i, ops[-1])})'
            else: src = 'ST(c,1)'
            code = fcmp('ST(c,0)', src)
            if mn.endswith('pp'): code += ' FPOP(c); FPOP(c);'
            elif mn.endswith('p'): code += ' FPOP(c);'
            return code, set(), set()
        if mn == 'fldlg2': return 'FPUSH(c,0.30102999566398119521);', set(), set()
        if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip', 'fcompi', 'fucompi'):
            k = self.fst_idx(i, ops[-1])
            code = f'{{double _a=ST(c,0),_b=ST(c,{k}); if(_a!=_a||_b!=_b){{c->zf=c->pf=c->cf=1;}} else {{c->pf=0; c->zf=_a==_b; c->cf=_a<_b;}} c->of=c->sf=0;}}'
            if mn.endswith('p') or mn.endswith('pi'): code += ' FPOP(c);'
            return code, set(FL), set()
        if mn == 'fnstsw':
            v = '(uint16_t)((c->fsw&~0x3800u)|((c->top&7)<<11))'
            if ops[0].type == X86_OP_REG: return f'c->eax=(c->eax&0xffff0000u)|{v};', set(), set()
            return f'W16({self.addr(i, ops[0])},{v});', set(), set()
        if mn == 'fnstcw': return f'W16({self.addr(i, ops[0])},c->fcw);', set(), set()
        if mn == 'fldcw': return f'c->fcw=R16({self.addr(i, ops[0])});', set(), set()
        if mn == 'fxam':
            return '{double _v=ST(c,0); uint16_t _s=(_v!=_v)?0x100:(isinf(_v)?0x500:(_v==0?0x4000:0x400)); if(signbit(_v)) _s|=0x200; c->fsw=(c->fsw&~0x4700u)|_s;}', set(), set()
        if mn in ('ffree', 'fincstp', 'fdecstp', 'fninit', 'fnclex'):
            if mn == 'fincstp': return 'FPOP(c);', set(), set()
            if mn == 'fdecstp': return 'c->top=(c->top-1)&7;', set(), set()
            if mn == 'fninit': return 'c->top=0; c->fcw=0x27f; c->fsw=0;', set(), set()
            return '', set(), set()
        return None

    # ---------- whole function
    def translate(self):
        insns = self.insns
        # labels: all intra-function jump targets + switch targets
        labels = set()
        for i in insns:
            if i.group(X86_GRP_JUMP) and i.operands and i.operands[0].type == X86_OP_IMM:
                t = i.operands[0].imm & 0xffffffff
                if self.inbody(t): labels.add(t)
        for sa, tg in self.switches.items():
            for t in tg:
                if self.inbody(t): labels.add(t)
        self.eh_conts = sorted(t for t in EH_CONTS.get(self.entry, ()) if self.inbody(t))
        labels |= set(self.eh_conts)
        entry_jump = bool(insns) and insns[0].address != self.entry and self.inbody(self.entry)
        if entry_jump: labels.add(self.entry)   # code pulled in below the entry (flow closure): start at the entry
        # per-flag liveness within straight-line runs (block boundary => all live)
        n = len(insns)
        sems = [None] * n
        live = set(FL)
        need_after = [None] * n
        for k in range(n - 1, -1, -1):
            i = insns[k]
            nxt_is_label = (k + 1 < n and insns[k + 1].address in labels)
            if nxt_is_label or i.group(X86_GRP_JUMP) or i.group(X86_GRP_RET): live = set(FL)
            if i.group(X86_GRP_CALL): live = set()
            need_after[k] = set(live)
            if i.group(X86_GRP_JUMP) or i.group(X86_GRP_RET) or i.mnemonic in ('ret', 'retn', 'repz ret', 'rep ret'):
                W, R = set(), set()
            elif i.group(X86_GRP_CALL):
                W, R = set(FL), set()
            else:
                try:
                    code, W, R = self.sem(i, set(FL))  # probe W/R with full need
                except Unsupported as e:
                    W, R = set(FL), set(FL)
            if i.group(X86_GRP_JUMP) and i.mnemonic.startswith('j') and i.mnemonic not in ('jmp', 'jecxz', 'jcxz'):
                cc = ccof(i.mnemonic[1:]); R = cc[1] if cc else set(FL)
            live = (live - W) | R
        out = []
        ok = True
        if entry_jump: out.append(f'goto L_{self.entry:x};')
        for k, i in enumerate(insns):
            A = i.address; nxt = A + i.size
            if A in labels: out.append(f'L_{A:x}:;')
            mn = i.mnemonic
            try:
                if mn == 'call':
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        out.append(f'PUSH32(c,0x{nxt:x}u); f_{t:08x}(c); /* call */')
                        self.calls.add(t)
                    elif op.type == X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xffffffff) in self.imports:
                        out.append(f'PUSH32(c,0x{nxt:x}u); call_import(c,0x{op.mem.disp & 0xffffffff:x}u); /* {self.imports[op.mem.disp & 0xffffffff]} */')
                    else:
                        out.append(f'{{uint32_t _t={self.rd(i, op, 32)}; PUSH32(c,0x{nxt:x}u); call_indirect(c,_t);}}')
                    continue
                if mn in ('ret', 'retn', 'repz ret', 'rep ret'):
                    extra = i.operands[0].imm if i.operands else 0
                    out.append(f'c->esp+={4 + extra}; return;')
                    continue
                if mn == 'jmp':
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if self.inbody(t): out.append(f'BARRIER(); goto L_{t:x};' if t <= A else f'goto L_{t:x};')
                        else:
                            out.append(f'f_{t:08x}(c); return; /* tail */'); self.calls.add(t)
                        continue
                    if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xffffffff) in self.imports:
                        out.append(f'call_import(c,0x{op.mem.disp & 0xffffffff:x}u); return; /* tail import {self.imports[op.mem.disp & 0xffffffff]} */'); continue
                    if A in self.switches and [t for t in self.switches[A] if self.inbody(t)]:
                        v = self.rd(i, op, 32)
                        cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in sorted(set(self.switches[A])) if self.inbody(t))
                        out.append(f'switch({v}){{ {cases} default: unimpl(c,0x{A:x},"switch"); return; }}')
                        continue
                    if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xffffffff) in self.imports:
                        out.append(f'call_import(c,0x{op.mem.disp & 0xffffffff:x}u); return; /* tail import */'); continue
                    out.append(f'{{uint32_t _t={self.rd(i, op, 32)}; call_indirect(c,_t); return;}} /* tail indirect */')
                    continue
                if mn.startswith('j'):
                    t = i.operands[0].imm & 0xffffffff
                    if mn in ('jecxz', 'jcxz'): cond = 'c->ecx==0'
                    else:
                        cc = ccof(mn[1:])
                        if not cc: raise Unsupported(mn)
                        cond = cc[0]
                    tgt = (f'{{ BARRIER(); goto L_{t:x}; }}' if t <= A else f'goto L_{t:x};') if self.inbody(t) else f'{{ f_{t:08x}(c); return; }}'
                    if not self.inbody(t): self.calls.add(t)
                    out.append(f'if({cond}) {tgt}')
                    continue
                tr = TRACE or self.entry in TRACE_FUNCS
                code, W, R = self.sem(i, need_after[k] if not tr else set(FL))
                out.append((f'TRACE(c,0x{A:x}); ' if tr else '') + code + f' /* {A:x}: {mn} {i.op_str} */')
                if (k + 1 >= len(insns) or insns[k + 1].address != nxt) and mn not in ('int3', 'hlt'):
                    # next instruction in address order is not the fall-through target: continue there explicitly
                    if self.inbody(nxt): out.append(f'goto L_{nxt:x};'); labels.add(nxt)
                    else: out.append(f'f_{nxt:08x}(c); return; /* fall into next function */'); self.calls.add(nxt)
            except Unsupported as e:
                self.unsup[str(e)] += 1; ok = False
                out.append(f'unimpl(c,0x{A:x},"{mn} {i.op_str}"); return;')
        # fallthrough past end of body
        out.append(f'unimpl(c,0x{self.entry:x},"fell off end"); return;')
        return ok, out

    def emit(self):
        self.calls = set()
        ok, body = self.translate()
        if self.eh_conts:   # has C++ catch blocks: body gets a resume entry, wrapper registers a host frame (EH_FRAME)
            cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in self.eh_conts)
            e = f'{self.entry:08x}'
            src = (f'static void b_{e}(CPU*c, uint32_t _resume){{\n  if(_resume){{ switch(_resume){{ {cases} }} unimpl(c,_resume,"bad eh resume"); return; }}\n  '
                   + '\n  '.join(body) + f'\n}}\nvoid f_{e}(CPU*c){{ EH_FRAME(c,b_{e}); }}\n')
        else:
            src = f'void f_{self.entry:08x}(CPU*c){{\n  ' + '\n  '.join(body) + '\n}\n'
        return ok, src


def load_bodies(path='/home/claude/bsi/bodies.tsv'):
    F = {}
    for l in open(path):
        a, rs, sw, cc, purge = l.rstrip('\n').split('\t')
        ranges = []
        for r in rs.split(','):
            if not r or ':' in r: continue
            lo, hi = r.split('-')
            ranges.append((int(lo, 16), int(hi, 16)))
        switches = {}
        for s in sw.split(';'):
            if not s: continue
            at, tg = s.split(':', 1)
            switches[int(at, 16)] = [int(x, 16) for x in tg.split('/') if x and ':' not in x]
        F[int(a, 16)] = (ranges, switches)
    return F

def load_imports():
    import pefile
    import os
    pe2 = pefile.PE(os.environ.get('BSI_IMAGE_ORIG', os.environ.get('BSI_IMAGE', '/home/claude/bsi/BioShockInfinite.exe')))
    delta = int(os.environ.get('BSI_IMAGE_BASE', '0'), 16) - pe2.OPTIONAL_HEADER.ImageBase if 'BSI_IMAGE_BASE' in os.environ else 0
    imp = {}
    for d in pe2.DIRECTORY_ENTRY_IMPORT:
        for x in d.imports:
            imp[x.address + delta] = d.dll.decode() + '!' + (x.name.decode() if x.name else str(x.ordinal))
    return imp
