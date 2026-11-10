"""Packed-integer (MMX + SSE2) and packed-double ops for x86c. mm registers are stored in the x87 slots c->st[n] (bit copies).

Two emitter classes produce the same semantics from the same instruction:
  Vec   lane loops over the scalar members of the XMM union (the original translation, byte-identical to before);
  VecV  the same operations written on the vector views of the union (cpu.h: vf/vd/vu/vi/vq/vsq/vw/vsw/vb/vsb and the
        XSHUF*/xmm_* helpers), selected by x86c with BSI_PROMO_XMM=1 (or BSI_XMM_VEC=1 alone). One whole-register expression
        per instruction lets the C compiler keep a promoted register (local `XMM x_k`) in one SIMD register; for an mm
        destination the same code runs on a 16-byte temporary of which the low 8 bytes are copied back. Lane loops remain only
        for saturating adds/packs, high multiplies and psadbw (no vector-extension equivalent).
"""
import re
from capstone.x86 import X86_OP_REG, X86_OP_MEM, X86_OP_IMM
LT = {1: 'u8', 2: 'u16', 4: 'u32', 8: 'u64'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
SFX = {'b': 1, 'w': 2, 'd': 4, 'q': 8}
VU = {1: 'vb', 2: 'vw', 4: 'vu', 8: 'vq'}        # unsigned vector view per lane width (VecV)
VS = {1: 'vsb', 2: 'vsw', 4: 'vi', 8: 'vsq'}      # signed vector view per lane width (VecV)
VT = {'vb': 'v16u', 'vw': 'v8u', 'vu': 'v4u', 'vq': 'v2u', 'vsb': 'v16i', 'vsw': 'v8i', 'vi': 'v4i', 'vsq': 'v2i'}

def rn(i, o): return i.reg_name(o.reg) if o.type == X86_OP_REG else None
def is_mm(i, o): n = rn(i, o); return bool(n) and re.fullmatch(r'mm\d', n) is not None
def is_x(i, o): n = rn(i, o); return bool(n) and n.startswith('xmm')
def shuf(ne, a, b, idx):
    """XSHUF<ne>(a, b, idx...) padded to ne indices (indices beyond the lanes that matter repeat the first one)"""
    idx = list(idx) + [idx[0]] * (ne - len(idx))
    return f'XSHUF{ne}({a},{b},{",".join(str(k) for k in idx)})'

class Vec:
    def __init__(self, fn): self.fn = fn
    def src(self, i, o, W):
        if is_x(i, o): return f'XMM _s=c->xmm[{rn(i,o)[3:]}];'
        if is_mm(i, o): return f'XMM _s; memset(&_s,0,16); memcpy(&_s,&c->st[{rn(i,o)[2:]}],8);'
        if o.type == X86_OP_MEM: return f'XMM _s; memset(&_s,0,16); memcpy(&_s,MEMP({self.fn.addr(i, o)}),{W});'
        raise ValueError('vec src')
    def dopen(self, i, o):
        if is_x(i, o): return f'XMM*_d=&c->xmm[{rn(i,o)[3:]}];', ''
        if is_mm(i, o): n = rn(i, o)[2:]; return f'XMM _dv; memset(&_dv,0,16); memcpy(&_dv,&c->st[{n}],8); XMM*_d=&_dv;', f' memcpy(&c->st[{n}],_d,8);'
        raise ValueError('vec dst')
    def width(self, i, o): return 16 if is_x(i, o) else 8

    def emit(self, i, mn, ops):
        anymm = any(is_mm(i, o) for o in ops)
        if mn == 'emms': return ';'
        if mn in ('movd', 'movq') and anymm:
            d, s = ops[0], ops[1]
            if is_mm(i, d):
                n = rn(i, d)[2:]
                if mn == 'movd': return f'{{uint64_t _v=(uint32_t)({self.fn.rd(i, s, 32)}); memcpy(&c->st[{n}],&_v,8);}}'
                if is_mm(i, s): return f'memcpy(&c->st[{n}],&c->st[{rn(i,s)[2:]}],8);'
                if is_x(i, s): return f'memcpy(&c->st[{n}],&c->xmm[{rn(i,s)[3:]}],8);'
                return f'{{uint64_t _v=R64({self.fn.addr(i, s)}); memcpy(&c->st[{n}],&_v,8);}}'
            n = rn(i, s)[2:]
            if mn == 'movd': return f'{{uint32_t _v; memcpy(&_v,&c->st[{n}],4); {self.fn.wr(i, d, "_v", 32)}}}'
            if is_x(i, d): return f'{{XMM*_x=&c->xmm[{rn(i,d)[3:]}]; memcpy(&_x->u64[0],&c->st[{n}],8); _x->u64[1]=0;}}'
            return f'{{uint64_t _v; memcpy(&_v,&c->st[{n}],8); W64({self.fn.addr(i, d)},_v);}}'
        if mn in ('movq2dq',): return f'{{XMM*_x=&c->xmm[{rn(i,ops[0])[3:]}]; memcpy(&_x->u64[0],&c->st[{rn(i,ops[1])[2:]}],8); _x->u64[1]=0;}}'
        if mn in ('movdq2q',): return f'memcpy(&c->st[{rn(i,ops[0])[2:]}],&c->xmm[{rn(i,ops[1])[3:]}],8);'
        if not ops or not (is_x(i, ops[0]) or is_mm(i, ops[0]) or mn in ('pextrw', 'pmovmskb', 'movmskpd')): return None
        # ----- packed double (xmm only)
        pdop = {'addpd': '+', 'subpd': '-', 'mulpd': '*', 'divpd': '/'}
        if mn in pdop:
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->d[0]=_d->d[0]{pdop[mn]}_s.d[0]; _d->d[1]=_d->d[1]{pdop[mn]}_s.d[1];}}'
        if mn in ('minpd', 'maxpd'):
            cmp = '<' if mn == 'minpd' else '>'
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; for(int _k=0;_k<2;_k++) _d->d[_k]=(_d->d[_k]{cmp}_s.d[_k])?_d->d[_k]:_s.d[_k];}}'
        if mn in ('minsd', 'maxsd'):
            cmp = '<' if mn == 'minsd' else '>'
            ld = self.src(i, ops[1], 8)
            return f'{{{ld} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->d[0]=(_d->d[0]{cmp}_s.d[0])?_d->d[0]:_s.d[0];}}'
        if mn == 'sqrtpd': return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->d[0]=sqrt(_s.d[0]); _d->d[1]=sqrt(_s.d[1]);}}'
        if mn == 'unpckhpd': return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->u64[0]=_d->u64[1]; _d->u64[1]=_s.u64[1];}}'
        if mn == 'punpckhqdq': return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->u64[0]=_d->u64[1]; _d->u64[1]=_s.u64[1];}}'
        if mn == 'shufpd':
            imm = ops[2].imm & 3
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; uint64_t _a=_d->u64[{imm & 1}], _b=_s.u64[{(imm >> 1) & 1}]; _d->u64[0]=_a; _d->u64[1]=_b;}}'
        if mn == 'movmskpd': return self.fn.wr(i, ops[0], f'(uint32_t)((c->xmm[{rn(i,ops[1])[3:]}].u64[0]>>63)|((c->xmm[{rn(i,ops[1])[3:]}].u64[1]>>63)<<1))', 32)
        if mn == 'cvtdq2pd':
            ld = self.src(i, ops[1], 8)
            return f'{{{ld} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; double _x=(double)_s.i32[0],_y=(double)_s.i32[1]; _d->d[0]=_x; _d->d[1]=_y;}}'
        if mn in ('cvttpd2dq', 'cvtpd2dq'):
            f = 'cvtt_f2i' if mn == 'cvttpd2dq' else 'cvt_f2i'
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; int32_t _x={f}(_s.d[0]),_y={f}(_s.d[1]); _d->i32[0]=_x; _d->i32[1]=_y; _d->u64[1]=0;}}'
        m = re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(sd|pd)', mn)
        if m and not (mn == 'cmpsd' and len(ops) < 3):
            pred = m.group(1) if m.group(1) else ['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord'][ops[2].imm & 7]
            e = {'eq': '_a==_b', 'lt': '_a<_b', 'le': '_a<=_b', 'unord': '(_a!=_a||_b!=_b)', 'neq': '!(_a==_b)', 'nlt': '!(_a<_b)', 'nle': '!(_a<=_b)', 'ord': '!(_a!=_a||_b!=_b)'}[pred]
            n = 1 if m.group(2) == 'sd' else 2
            return f'{{{self.src(i,ops[1],8*n)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; for(int _k=0;_k<{n};_k++){{ double _a=_d->d[_k],_b=_s.d[_k]; _d->u64[_k]=({e})?~0ull:0; }}}}'
        # ----- packed integer (xmm or mm)
        if mn == 'pextrw':
            s = ops[1]; imm = ops[2].imm
            if is_x(i, s): v = f'c->xmm[{rn(i,s)[3:]}].u16[{imm & 7}]'
            else: v = f'((uint16_t*)&c->st[{rn(i,s)[2:]}])[{imm & 3}]'
            return self.fn.wr(i, ops[0], f'(uint32_t){v}', 32)
        if mn == 'pmovmskb':
            s = ops[1]
            if is_x(i, s): return self.fn.wr(i, ops[0], f'({{uint32_t _m=0; for(int _k=0;_k<16;_k++) _m|=((c->xmm[{rn(i,s)[3:]}].u8[_k]>>7)&1u)<<_k; _m;}})', 32)
            return self.fn.wr(i, ops[0], f'({{uint32_t _m=0; uint8_t*_p=(uint8_t*)&c->st[{rn(i,s)[2:]}]; for(int _k=0;_k<8;_k++) _m|=((_p[_k]>>7)&1u)<<_k; _m;}})', 32)
        W = self.width(i, ops[0])
        dop, dcl = self.dopen(i, ops[0])
        if mn == 'pinsrw':
            imm = ops[2].imm & (7 if W == 16 else 3)
            return f'{{{dop} _d->u16[{imm}]=(uint16_t)({self.fn.rd(i, ops[1], 16) if ops[1].type == X86_OP_MEM else self.fn.rd(i, ops[1], 32)});{dcl}}}'
        if mn in ('pshufw', 'pshuflw', 'pshufhw'):
            imm = ops[2].imm & 0xff
            if mn == 'pshufw': return f'{{{self.src(i,ops[1],8)} {dop} for(int _k=0;_k<4;_k++) _d->u16[_k]=_s.u16[({imm}>>(2*_k))&3];{dcl}}}'
            base = 0 if mn == 'pshuflw' else 4
            return f'{{{self.src(i,ops[1],16)} {dop} *_d=_s; for(int _k=0;_k<4;_k++) _d->u16[{base}+_k]=_s.u16[{base}+(({imm}>>(2*_k))&3)];{dcl}}}'
        bw = {'pand': '&', 'por': '|', 'pxor': '^'}
        if mn in bw:
            if mn == 'pxor' and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg: return f'{{{dop} memset(_d,0,16);{dcl}}}'
            return f'{{{self.src(i,ops[1],W)} {dop} _d->u64[0]{bw[mn]}=_s.u64[0]; _d->u64[1]{bw[mn]}=_s.u64[1];{dcl}}}'
        if mn == 'pandn': return f'{{{self.src(i,ops[1],W)} {dop} _d->u64[0]=~_d->u64[0]&_s.u64[0]; _d->u64[1]=~_d->u64[1]&_s.u64[1];{dcl}}}'
        m = re.fullmatch(r'p(add|sub)(s|us)?([bwdq])', mn)
        if m:
            op, sat, lb = m.group(1), m.group(2), SFX[m.group(3)]; n = W // lb; T = LT[lb]
            o = '+' if op == 'add' else '-'
            if sat == 's':
                lo, hi = -(1 << (8 * lb - 1)), (1 << (8 * lb - 1)) - 1
                e = f'int64_t _r=(int64_t)({ST[lb]})_d->{T}[_k]{o}(int64_t)({ST[lb]})_s.{T}[_k]; if(_r<{lo}) _r={lo}; if(_r>{hi}) _r={hi}; _d->{T}[_k]=({UT[lb]})_r;'
            elif sat == 'us':
                hi = (1 << (8 * lb)) - 1
                e = f'int64_t _r=(int64_t)_d->{T}[_k]{o}(int64_t)_s.{T}[_k]; if(_r<0) _r=0; if(_r>{hi}) _r={hi}; _d->{T}[_k]=({UT[lb]})_r;'
            else: e = f'_d->{T}[_k]=({UT[lb]})(_d->{T}[_k]{o}_s.{T}[_k]);'
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++){{ {e} }}{dcl}}}'
        if mn in ('pmullw', 'pmulhw', 'pmulhuw'):
            n = W // 2
            if mn == 'pmullw': e = '_d->u16[_k]=(uint16_t)((int32_t)(int16_t)_d->u16[_k]*(int32_t)(int16_t)_s.u16[_k]);'
            elif mn == 'pmulhw': e = '_d->u16[_k]=(uint16_t)(((int32_t)(int16_t)_d->u16[_k]*(int32_t)(int16_t)_s.u16[_k])>>16);'
            else: e = '_d->u16[_k]=(uint16_t)(((uint32_t)_d->u16[_k]*(uint32_t)_s.u16[_k])>>16);'
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++){{ {e} }}{dcl}}}'
        if mn == 'pmuludq':
            n = W // 8
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++){{ _d->u64[_k]=(uint64_t)_d->u32[2*_k]*(uint64_t)_s.u32[2*_k]; }}{dcl}}}'
        if mn == 'pmaddwd':
            n = W // 4
            return f'{{{self.src(i,ops[1],W)} {dop} XMM _t; for(int _k=0;_k<{n};_k++) _t.u32[_k]=(uint32_t)((int32_t)(int16_t)_d->u16[2*_k]*(int16_t)_s.u16[2*_k])+(uint32_t)((int32_t)(int16_t)_d->u16[2*_k+1]*(int16_t)_s.u16[2*_k+1]); for(int _k=0;_k<{n};_k++) _d->i32[_k]=_t.i32[_k];{dcl}}}'
        if mn in ('pavgb', 'pavgw'):
            lb = 1 if mn == 'pavgb' else 2; T = LT[lb]; n = W // lb
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++) _d->{T}[_k]=({UT[lb]})(((uint32_t)_d->{T}[_k]+_s.{T}[_k]+1)>>1);{dcl}}}'
        if mn in ('pminub', 'pmaxub', 'pminsw', 'pmaxsw'):
            lb = 1 if mn.endswith('ub') else 2; T = LT[lb]; n = W // lb
            cast = UT[lb] if lb == 1 else 'int16_t'; cmp = '<' if 'min' in mn else '>'
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++){{ {cast} _a=({cast})_d->{T}[_k],_b=({cast})_s.{T}[_k]; _d->{T}[_k]=({UT[lb]})((_a{cmp}_b)?_a:_b); }}{dcl}}}'
        if mn == 'psadbw':
            n = W // 8
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _q=0;_q<{n};_q++){{ uint32_t _sum=0; for(int _k=0;_k<8;_k++){{ int _v=(int)_d->u8[8*_q+_k]-(int)_s.u8[8*_q+_k]; _sum+=_v<0?-_v:_v; }} _d->u64[_q]=_sum; }}{dcl}}}'
        m = re.fullmatch(r'pcmp(eq|gt)([bwd])', mn)
        if m:
            lb = SFX[m.group(2)]; T = LT[lb]; n = W // lb; o = '==' if m.group(1) == 'eq' else '>'
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++) _d->{T}[_k]=((({ST[lb]})_d->{T}[_k]){o}(({ST[lb]})_s.{T}[_k]))?({UT[lb]})~0ull:0;{dcl}}}'
        m = re.fullmatch(r'ps(ll|rl|ra)([wdq])', mn)
        if m:
            kind, lb = m.group(1), SFX[m.group(2)]; T = LT[lb]; n = W // lb; bits = 8 * lb
            if ops[1].type == X86_OP_IMM: cnt = f'uint64_t _n={ops[1].imm & 0xff};'
            else: cnt = f'{self.src(i,ops[1],8)} uint64_t _n=_s.u64[0];'
            if kind == 'll': e = f'_d->{T}[_k]=(_n>{bits-1})?0:({UT[lb]})(_d->{T}[_k]<<_n);'
            elif kind == 'rl': e = f'_d->{T}[_k]=(_n>{bits-1})?0:({UT[lb]})(_d->{T}[_k]>>_n);'
            else: e = f'_d->{T}[_k]=({UT[lb]})((({ST[lb]})_d->{T}[_k])>>(_n>{bits-1}?{bits-1}:_n));'
            return f'{{{cnt} {dop} for(int _k=0;_k<{n};_k++){{ {e} }}{dcl}}}'
        if mn == 'psrldq':
            n = ops[1].imm
            return f'{{{dop} XMM _t; memset(&_t,0,16); if({n}<16) memcpy(_t.u8,&_d->u8[{n}],16-{n}); *_d=_t;{dcl}}}'
        m = re.fullmatch(r'punpck(l|h)(bw|wd|dq|qdq)', mn)
        if m:
            hi = m.group(1) == 'h'; lb = {'bw': 1, 'wd': 2, 'dq': 4, 'qdq': 8}[m.group(2)]; n = W // lb // 2; off = n if hi else 0
            return (f'{{{self.src(i,ops[1],W)} {dop} XMM _t; memset(&_t,0,16); for(int _k=0;_k<{n};_k++){{ memcpy(&_t.u8[2*_k*{lb}],&_d->u8[({off}+_k)*{lb}],{lb}); memcpy(&_t.u8[(2*_k+1)*{lb}],&_s.u8[({off}+_k)*{lb}],{lb}); }} *_d=_t;{dcl}}}')
        if mn in ('packsswb', 'packuswb', 'packssdw'):
            if mn == 'packsswb': sb, db, lo, hi, st, dt = 2, 1, -128, 127, 'int16_t', 'int8_t'
            elif mn == 'packuswb': sb, db, lo, hi, st, dt = 2, 1, 0, 255, 'int16_t', 'uint8_t'
            else: sb, db, lo, hi, st, dt = 4, 2, -32768, 32767, 'int32_t', 'int16_t'
            n = W // sb
            return (f'{{{self.src(i,ops[1],W)} {dop} XMM _t; memset(&_t,0,16); for(int _k=0;_k<{2*n};_k++){{ {st} _v; if(_k<{n}) memcpy(&_v,&_d->u8[_k*{sb}],{sb}); else memcpy(&_v,&_s.u8[(_k-{n})*{sb}],{sb});'
                    f' if(_v<{lo}) _v={lo}; if(_v>{hi}) _v={hi}; {dt} _o=({dt})_v; memcpy(&_t.u8[_k*{db}],&_o,{db}); }} *_d=_t;{dcl}}}')
        return None


class VecV(Vec):
    """vector-view emitters (see the module docstring); same instruction coverage and semantics as Vec"""
    def src(self, i, o, W):
        if is_x(i, o): return f'XMM _s=c->xmm[{rn(i,o)[3:]}];'
        if is_mm(i, o): return f'XMM _s; _s.vq=(v2u){{0,0}}; memcpy(&_s,&c->st[{rn(i,o)[2:]}],8);'
        if o.type == X86_OP_MEM:
            if W == 16: return f'XMM _s; memcpy(&_s,MEMP({self.fn.addr(i, o)}),16);'
            return f'XMM _s; _s.vq=(v2u){{R64({self.fn.addr(i, o)}),0}};'
        raise ValueError('vec src')
    def dopen(self, i, o):
        if is_x(i, o): return f'XMM*_d=&c->xmm[{rn(i,o)[3:]}];', ''
        if is_mm(i, o): n = rn(i, o)[2:]; return f'XMM _dv; _dv.vq=(v2u){{0,0}}; memcpy(&_dv,&c->st[{n}],8); XMM*_d=&_dv;', f' memcpy(&c->st[{n}],_d,8);'
        raise ValueError('vec dst')

    def emit(self, i, mn, ops):
        anymm = any(is_mm(i, o) for o in ops)
        if mn == 'emms': return ';'
        if mn in ('movd', 'movq') and anymm:
            d, s = ops[0], ops[1]
            if is_mm(i, d):
                n = rn(i, d)[2:]
                if mn == 'movd': return f'{{uint64_t _v=(uint32_t)({self.fn.rd(i, s, 32)}); memcpy(&c->st[{n}],&_v,8);}}'
                if is_mm(i, s): return f'memcpy(&c->st[{n}],&c->st[{rn(i,s)[2:]}],8);'
                if is_x(i, s): return f'memcpy(&c->st[{n}],&c->xmm[{rn(i,s)[3:]}],8);'
                return f'{{uint64_t _v=R64({self.fn.addr(i, s)}); memcpy(&c->st[{n}],&_v,8);}}'
            n = rn(i, s)[2:]
            if mn == 'movd': return f'{{uint32_t _v; memcpy(&_v,&c->st[{n}],4); {self.fn.wr(i, d, "_v", 32)}}}'
            if is_x(i, d): return f'{{uint64_t _v; memcpy(&_v,&c->st[{n}],8); c->xmm[{rn(i,d)[3:]}].vq=(v2u){{_v,0}};}}'
            return f'{{uint64_t _v; memcpy(&_v,&c->st[{n}],8); W64({self.fn.addr(i, d)},_v);}}'
        if mn in ('movq2dq',): return f'{{uint64_t _v; memcpy(&_v,&c->st[{rn(i,ops[1])[2:]}],8); c->xmm[{rn(i,ops[0])[3:]}].vq=(v2u){{_v,0}};}}'
        if mn in ('movdq2q',): return f'memcpy(&c->st[{rn(i,ops[0])[2:]}],&c->xmm[{rn(i,ops[1])[3:]}],8);'
        if not ops or not (is_x(i, ops[0]) or is_mm(i, ops[0]) or mn in ('pextrw', 'pmovmskb', 'movmskpd')): return None
        # ----- packed double (xmm only)
        pdop = {'addpd': '+', 'subpd': '-', 'mulpd': '*', 'divpd': '/'}
        if mn in pdop:
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vd=_d->vd{pdop[mn]}_s.vd;}}'
        if mn in ('minpd', 'maxpd'):
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vd=xmm_{mn}(_d->vd,_s.vd);}}'
        if mn in ('minsd', 'maxsd'):
            cmp = '<' if mn == 'minsd' else '>'
            ld = self.src(i, ops[1], 8)
            return f'{{{ld} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->d[0]=(_d->d[0]{cmp}_s.d[0])?_d->d[0]:_s.d[0];}}'
        if mn == 'sqrtpd': return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vd=xmm_sqrtpd(_s.vd);}}'
        if mn in ('unpckhpd', 'punpckhqdq'): return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vq=XSHUF2(_d->vq,_s.vq,1,3);}}'
        if mn == 'shufpd':
            imm = ops[2].imm & 3
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vq=XSHUF2(_d->vq,_s.vq,{imm & 1},{2 + ((imm >> 1) & 1)});}}'
        if mn == 'movmskpd': return self.fn.wr(i, ops[0], f'xmm_movmskpd(c->xmm[{rn(i,ops[1])[3:]}].vq)', 32)
        if mn == 'cvtdq2pd':
            ld = self.src(i, ops[1], 8)
            return f'{{{ld} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vd=(v2d){{(double)_s.i32[0],(double)_s.i32[1]}};}}'
        if mn in ('cvttpd2dq', 'cvtpd2dq'):
            f = 'cvtt_f2i' if mn == 'cvttpd2dq' else 'cvt_f2i'
            return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; _d->vi=(v4i){{{f}(_s.d[0]),{f}(_s.d[1]),0,0}};}}'
        m = re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(sd|pd)', mn)
        if m and not (mn == 'cmpsd' and len(ops) < 3):
            pred = m.group(1) if m.group(1) else ['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord'][ops[2].imm & 7]
            if m.group(2) == 'pd':
                e = {'eq': '(v2i)(_a==_b)', 'lt': '(v2i)(_a<_b)', 'le': '(v2i)(_a<=_b)', 'unord': '((v2i)(_a!=_a)|(v2i)(_b!=_b))', 'neq': '~(v2i)(_a==_b)',
                     'nlt': '~(v2i)(_a<_b)', 'nle': '~(v2i)(_a<=_b)', 'ord': '~((v2i)(_a!=_a)|(v2i)(_b!=_b))'}[pred]
                return f'{{{self.src(i,ops[1],16)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; v2d _a=_d->vd,_b=_s.vd; _d->vsq={e};}}'
            e = {'eq': '_a==_b', 'lt': '_a<_b', 'le': '_a<=_b', 'unord': '(_a!=_a||_b!=_b)', 'neq': '!(_a==_b)', 'nlt': '!(_a<_b)', 'nle': '!(_a<=_b)', 'ord': '!(_a!=_a||_b!=_b)'}[pred]
            return f'{{{self.src(i,ops[1],8)} XMM*_d=&c->xmm[{rn(i,ops[0])[3:]}]; double _a=_d->d[0],_b=_s.d[0]; _d->u64[0]=({e})?~0ull:0;}}'
        # ----- packed integer (xmm or mm)
        if mn == 'pextrw':
            s = ops[1]; imm = ops[2].imm
            if is_x(i, s): v = f'c->xmm[{rn(i,s)[3:]}].u16[{imm & 7}]'
            else: v = f'((uint16_t*)&c->st[{rn(i,s)[2:]}])[{imm & 3}]'
            return self.fn.wr(i, ops[0], f'(uint32_t){v}', 32)
        if mn == 'pmovmskb':
            s = ops[1]
            if is_x(i, s): return self.fn.wr(i, ops[0], f'xmm_pmovmskb(c->xmm[{rn(i,s)[3:]}].vb)', 32)
            return self.fn.wr(i, ops[0], f'({{uint32_t _m=0; uint8_t*_p=(uint8_t*)&c->st[{rn(i,s)[2:]}]; for(int _k=0;_k<8;_k++) _m|=((_p[_k]>>7)&1u)<<_k; _m;}})', 32)
        W = self.width(i, ops[0])
        dop, dcl = self.dopen(i, ops[0])
        if mn == 'pinsrw':
            imm = ops[2].imm & (7 if W == 16 else 3)
            return f'{{{dop} _d->u16[{imm}]=(uint16_t)({self.fn.rd(i, ops[1], 16) if ops[1].type == X86_OP_MEM else self.fn.rd(i, ops[1], 32)});{dcl}}}'
        if mn in ('pshufw', 'pshuflw', 'pshufhw'):
            imm = ops[2].imm & 0xff; ix = [(imm >> (2 * k)) & 3 for k in range(4)]
            if mn == 'pshufw': return f'{{{self.src(i,ops[1],8)} {dop} _d->vw={shuf(8, "_s.vw", "_s.vw", ix)};{dcl}}}'
            idx = ix + [4, 5, 6, 7] if mn == 'pshuflw' else [0, 1, 2, 3] + [4 + k for k in ix]
            return f'{{{self.src(i,ops[1],16)} {dop} _d->vw={shuf(8, "_s.vw", "_s.vw", idx)};{dcl}}}'
        bw = {'pand': '&', 'por': '|', 'pxor': '^'}
        if mn in bw:
            if mn == 'pxor' and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg: return f'{{{dop} _d->vq=(v2u){{0,0}};{dcl}}}'
            return f'{{{self.src(i,ops[1],W)} {dop} _d->vq{bw[mn]}=_s.vq;{dcl}}}'
        if mn == 'pandn': return f'{{{self.src(i,ops[1],W)} {dop} _d->vq=~_d->vq&_s.vq;{dcl}}}'
        m = re.fullmatch(r'p(add|sub)(s|us)?([bwdq])', mn)
        if m:
            op, sat, lb = m.group(1), m.group(2), SFX[m.group(3)]; n = W // lb; T = LT[lb]
            o = '+' if op == 'add' else '-'
            vu, vs = VU[lb], VS[lb]
            if not sat: return f'{{{self.src(i,ops[1],W)} {dop} _d->{vu}{o}=_s.{vu};{dcl}}}'
            v = vu if sat == 'us' else vs      # saturating forms: cpu.h helpers (elementwise builtins / SSE2 builtins / generic)
            return f'{{{self.src(i,ops[1],W)} {dop} _d->{v}=xmm_{mn}(_d->{v},_s.{v});{dcl}}}'
        if mn == 'pmullw': return f'{{{self.src(i,ops[1],W)} {dop} _d->vw*=_s.vw;{dcl}}}'
        if mn in ('pmulhw', 'pmulhuw'):
            n = W // 2
            if mn == 'pmulhw': e = '_d->u16[_k]=(uint16_t)(((int32_t)(int16_t)_d->u16[_k]*(int32_t)(int16_t)_s.u16[_k])>>16);'
            else: e = '_d->u16[_k]=(uint16_t)(((uint32_t)_d->u16[_k]*(uint32_t)_s.u16[_k])>>16);'
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _k=0;_k<{n};_k++){{ {e} }}{dcl}}}'
        if mn == 'pmuludq':
            return f'{{{self.src(i,ops[1],W)} {dop} _d->vq=((_d->vq<<32)>>32)*((_s.vq<<32)>>32);{dcl}}}'
        if mn == 'pmaddwd': return f'{{{self.src(i,ops[1],W)} {dop} _d->vi=xmm_pmaddwd(_d->vi,_s.vi);{dcl}}}'
        if mn in ('pavgb', 'pavgw'):
            v = 'vb' if mn == 'pavgb' else 'vw'   # (a|b) - ((a^b)>>1) == (a+b+1)>>1 without overflow
            return f'{{{self.src(i,ops[1],W)} {dop} _d->{v}=(_d->{v}|_s.{v})-((_d->{v}^_s.{v})>>1);{dcl}}}'
        if mn in ('pminub', 'pmaxub', 'pminsw', 'pmaxsw'):
            v = 'vb' if mn.endswith('ub') else 'vsw'
            return f'{{{self.src(i,ops[1],W)} {dop} _d->{v}=xmm_{mn}(_d->{v},_s.{v});{dcl}}}'
        if mn == 'psadbw':
            n = W // 8
            return f'{{{self.src(i,ops[1],W)} {dop} for(int _q=0;_q<{n};_q++){{ uint32_t _sum=0; for(int _k=0;_k<8;_k++){{ int _v=(int)_d->u8[8*_q+_k]-(int)_s.u8[8*_q+_k]; _sum+=_v<0?-_v:_v; }} _d->u64[_q]=_sum; }}{dcl}}}'
        m = re.fullmatch(r'pcmp(eq|gt)([bwd])', mn)
        if m:
            lb = SFX[m.group(2)]; o = '==' if m.group(1) == 'eq' else '>'; v = VS[lb]
            return f'{{{self.src(i,ops[1],W)} {dop} _d->{v}=({VT[v]})(_d->{v}{o}_s.{v});{dcl}}}'
        m = re.fullmatch(r'ps(ll|rl|ra)([wdq])', mn)
        if m:
            kind, lb = m.group(1), SFX[m.group(2)]; bits = 8 * lb
            v = VS[lb] if kind == 'ra' else VU[lb]
            if ops[1].type == X86_OP_IMM:
                n = ops[1].imm & 0xff
                if kind == 'ra': e = f'_d->{v}=_d->{v}>>{min(n, bits - 1)};'
                elif n >= bits: e = f'_d->vq=(v2u){{0,0}};'
                else: e = f'_d->{v}=_d->{v}{"<<" if kind == "ll" else ">>"}{n};'
                return f'{{{dop} {e}{dcl}}}'
            return f'{{{self.src(i,ops[1],8)} {dop} _d->{v}=xmm_{mn}(_d->{v},_s.u64[0]);{dcl}}}'
        if mn == 'psrldq':
            n = ops[1].imm & 0xff
            if n >= 16: return f'{{{dop} _d->vq=(v2u){{0,0}};{dcl}}}'
            ix = [k + n if k + n < 16 else 16 for k in range(16)]   # index 16.. = the zero vector
            return f'{{{dop} XMM _z; _z.vq=(v2u){{0,0}}; _d->vb={shuf(16, "_d->vb", "_z.vb", ix)};{dcl}}}'
        m = re.fullmatch(r'punpck(l|h)(bw|wd|dq|qdq)', mn)
        if m:
            hi = m.group(1) == 'h'; lb = {'bw': 1, 'wd': 2, 'dq': 4, 'qdq': 8}[m.group(2)]; n = W // lb // 2; off = n if hi else 0; ne = 16 // lb
            ix = [x for k in range(n) for x in (off + k, ne + off + k)]
            v = VU[lb]
            return f'{{{self.src(i,ops[1],W)} {dop} _d->{v}={shuf(ne, f"_d->{v}", f"_s.{v}", ix)};{dcl}}}'
        if mn in ('packsswb', 'packuswb', 'packssdw'):
            if mn == 'packsswb': sb, db, lo, hi, st, dt = 2, 1, -128, 127, 'int16_t', 'int8_t'
            elif mn == 'packuswb': sb, db, lo, hi, st, dt = 2, 1, 0, 255, 'int16_t', 'uint8_t'
            else: sb, db, lo, hi, st, dt = 4, 2, -32768, 32767, 'int32_t', 'int16_t'
            n = W // sb
            return (f'{{{self.src(i,ops[1],W)} {dop} XMM _t; _t.vq=(v2u){{0,0}}; for(int _k=0;_k<{2*n};_k++){{ {st} _v; if(_k<{n}) memcpy(&_v,&_d->u8[_k*{sb}],{sb}); else memcpy(&_v,&_s.u8[(_k-{n})*{sb}],{sb});'
                    f' if(_v<{lo}) _v={lo}; if(_v>{hi}) _v={hi}; {dt} _o=({dt})_v; memcpy(&_t.u8[_k*{db}],&_o,{db}); }} *_d=_t;{dcl}}}')
        return None
