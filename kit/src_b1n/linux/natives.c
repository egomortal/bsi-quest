/* natives.c -- tier-2 native replacements for statically linked library leaf functions (step 5 of OVERNIGHT.md).

   Mechanism: genall.py (promoted mode) emits the recompiled body of each function in NATIVE_FUNCS as f_XXXXXXXX_x86 instead
   of f_XXXXXXXX. This file defines f_XXXXXXXX as a weak dispatcher, so every direct call (any part file) and every
   indirect call (functab / flat call table) goes through it. The dispatcher runs the native version when enabled, else the
   recompiled one. If a gen tree without the rename is linked, its strong f_XXXXXXXX wins and this file is inert.

   Functions (guest address, what it is, how it was identified):
     0x00e106e0 lzo   lzo1x_decompress_safe (LZO 2.x; UE3 package decompression on the PoolThreads; only caller 0x48fa67
                      = appUncompressMemory COMPRESS_LZO). cdecl (in, in_len, out, *out_len, wrkmem) -> 0 / LZO_E_*.
     0x00ddc170 infl  zlib 1.2.8 inflate_fast(strm, start) (the exe embeds "inflate 1.2.8"; state offsets, TYPE=11, BAD=29
                      and the msg string addresses read from the disassembly). cdecl, void.
     0x00480850 cmp   memcmp-style compare (a, b, n) -> 0 / -1 / +1 (MSVC "or eax,1" form). cdecl. Hot on the RenderingThread.
     0x00889a50 swz   R<->B channel swap of n 32-bit pixels (src, dst, n) with an SSE2 path for n%4==0 && src 16-aligned.
                      cdecl, void. 24% of RenderingThread CPU in the cloud profile (every SSE op goes through c->xmm[]).

   Exactness: each native reproduces the guest-visible effects of the original: all memory it writes (output buffers,
   *out_len, strm/state fields, the incoming-argument slots the original reuses as scratch, the push slots of the saved
   registers), the return registers eax/ecx/edx, the arithmetic flags of the last flag-setting instruction, esp (ret) and,
   for swz, xmm0-xmm4. Not reproduced: inflate_fast's dead locals below the caller's esp and its "start" argument slot
   (used as a scratch variable by MSVC; a cdecl caller never reads it). lzo decodes into a host scratch buffer and copies
   to the guest only on success; any error status (or overlapping in/out, or in_len < 3) re-runs the recompiled original
   from the untouched state, so error paths are bit-exact by construction.

   Env (read once):
     BSI_NATIVE=0|1|all|<list>   functions to run natively (list: lzo,infl,cmp,swz). Default: off.
     BSI_NATIVE_CHECK=1          differential check on live calls: run the recompiled original, snapshot the result,
                                 restore, run the native, compare registers/flags/xmm and every written byte; on a
                                 mismatch log it and keep the original's result. BSI_NATIVE_CHECK_EVERY=k checks every
                                 k-th call per function (default 1).
     BSI_NATIVE_FUZZ=n           with CHECK: also run n mutated variants per checked call (truncated/flipped input,
                                 short output, other lengths/offsets) through both versions (first BSI_NATIVE_FUZZ_CALLS
                                 calls per function, default 200).
     BSI_NATIVE_STAT=1           per-function call counts, bytes and wall time per 10 s on stderr ("[native] ..."). */
#include "hle.h"
#include <time.h>
#include <pthread.h>

enum { NF_LZO=0, NF_INFL, NF_CMP, NF_SWZ, NF_N };
static const char *const nf_name[NF_N]={"lzo","infl","cmp","swz"};
static int g_mask=-1, g_check, g_check_every=1, g_fuzz, g_fuzz_calls=200, g_stat;
static const char *swz_kind(void);   /* "neon" / "sse2" / "scalar" (+ BSI_NATIVE_SWZ=scalar) */
static void nat_init(void){
  int m=0; const char *e=getenv("BSI_NATIVE");   /* default: off (recompiled originals) */
#ifdef BSI_ANDROID
  /* build 65: on the Quest the package / texture-cache decompressors (LZO for every .xxx/.tfc chunk, zlib for GFx movies)
     run natively by default: texture streaming and level loads decode faster. Cloud BSI_NATIVE_CHECK: 1199 LZO + 3905 zlib
     calls, 0 mismatches (cmp/swz differ only in flags, left off). BSI_NATIVE=0 turns it off, BSI_NATIVE=all adds cmp/swz. */
  if(!e) e="lzo,infl";
#endif
  if(e){ if(!strcmp(e,"0")||!*e) m=0; else if(!strcmp(e,"1")||!strcmp(e,"all")) m=(1<<NF_N)-1; else { m=0; for(int k=0;k<NF_N;k++) if(strstr(e,nf_name[k])) m|=1<<k; } }
  e=getenv("BSI_NATIVE_CHECK"); g_check=e&&atoi(e);
  e=getenv("BSI_NATIVE_CHECK_EVERY"); if(e&&atoi(e)>0) g_check_every=atoi(e);
  e=getenv("BSI_NATIVE_FUZZ"); g_fuzz=e?atoi(e):0;
  e=getenv("BSI_NATIVE_FUZZ_CALLS"); if(e) g_fuzz_calls=atoi(e);
  e=getenv("BSI_NATIVE_STAT"); g_stat=e&&atoi(e);
  fprintf(stderr,"[native] library natives:"); for(int k=0;k<NF_N;k++) fprintf(stderr," %s=%s",nf_name[k],(m>>k)&1?"native":"x86");
  fprintf(stderr," [swz %s]%s%s\n",swz_kind(),g_check?" (differential check on)":"",g_fuzz?" (fuzz on)":"");
  __atomic_store_n(&g_mask,m,__ATOMIC_RELEASE);
}
static inline int nat_on(int k){ int m=__atomic_load_n(&g_mask,__ATOMIC_ACQUIRE); if(__builtin_expect(m<0,0)){ nat_init(); m=g_mask; } return (m>>k)&1; }

/* ---------------- stats ---------------- */
static uint64_t st_calls[NF_N], st_nat[NF_N], st_bytes[NF_N], st_ns[NF_N], st_fallback[NF_N], ck_n[NF_N], ck_bad[NF_N], fz_n[NF_N], fz_bad[NF_N];
static inline uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
static void stat_report(uint64_t t){
  static uint64_t last; static int lock;
  if(t-last<10000000000ull || __atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)) return;
  if(t-last>=10000000000ull){ last=t;
    char b[1024]; int n=snprintf(b,sizeof b,"[native] t=%.1f",t*1e-9);
    for(int k=0;k<NF_N;k++) n+=snprintf(b+n,sizeof b-n," | %s calls %llu native %llu fallback %llu bytes %llu time %.1f ms",nf_name[k],
      (unsigned long long)st_calls[k],(unsigned long long)st_nat[k],(unsigned long long)st_fallback[k],(unsigned long long)st_bytes[k],st_ns[k]*1e-6);
    if(g_check) for(int k=0;k<NF_N;k++) n+=snprintf(b+n,sizeof b-n," | check %s %llu bad %llu fuzz %llu bad %llu",nf_name[k],
      (unsigned long long)ck_n[k],(unsigned long long)ck_bad[k],(unsigned long long)fz_n[k],(unsigned long long)fz_bad[k]);
    fprintf(stderr,"%s\n",b); }
  __atomic_store_n(&lock,0,__ATOMIC_RELEASE);
}
#define ADD(v,x) __atomic_fetch_add(&(v),(x),__ATOMIC_RELAXED)

static inline void set_flags_logic(CPU*c, uint32_t r){ c->cf=0; c->of=0; c->zf=r==0; c->sf=r>>31; c->pf=PAR(r); }

/* ================= lzo1x_decompress_safe (0x00e106e0) ================= */
/* Decodes [in,in+in_len) into out[0..cap); returns 0 and *olen on success, <0 on any error (caller falls back).
   v0.8.7: 8-byte "wild" copies for literal runs and matches. They may write up to 7 bytes past the end of a copy, so the
   output buffer must have LZO_SLACK spare bytes after cap (nat_lzo's scratch buffer has; only [0,olen) is copied to the
   guest), and input is read 8 bytes at a time only while at least n+8 input bytes remain. Short-distance matches (< 8)
   keep the byte loop (overlapping run semantics). */
#define LZO_SLACK 32
static inline void lzo_copy8(uint8_t *d, const uint8_t *s){ uint64_t v; memcpy(&v,s,8); memcpy(d,&v,8); }
static int lzo1x_dec(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t cap, uint32_t *olen){
  const uint8_t *ip=in, *const ip_end=in+in_len, *m_pos; uint8_t *op=out, *const op_end=out+cap; uint32_t t;
#define NEED_IP(x) if((uint32_t)(ip_end-ip)<(uint32_t)(x)) return -4
#define NEED_OP(x) if((uint32_t)(op_end-op)<(uint32_t)(x)) return -5
#define TEST_LB(m) if((m)<out || (m)>=op) return -6
  NEED_IP(3);
  if(*ip>17){ t=*ip++-17; if(t<4) goto match_next; NEED_OP(t); NEED_IP(t+3); do *op++=*ip++; while(--t>0); goto first_literal_run; }
  for(;;){
    NEED_IP(3); t=*ip++;
    if(t>=16) goto match;
    if(t==0){ while(*ip==0){ t+=255; ip++; if(t>0xffff0000u) return -1; NEED_IP(1); } t+=15+*ip++; }
    NEED_OP(t+3); NEED_IP(t+6);
    { uint32_t n=t+3;
      if((uint32_t)(ip_end-ip)>=n+8){ uint8_t *d=op, *e=op+n; const uint8_t *s=ip; do{ lzo_copy8(d,s); d+=8; s+=8; }while(d<e); }
      else memcpy(op,ip,n);
      op+=n; ip+=n; }
first_literal_run:
    t=*ip++;
    if(t>=16) goto match;
    m_pos=op-(1+0x0800); m_pos-=t>>2; m_pos-=*ip++<<2;
    TEST_LB(m_pos); NEED_OP(3);
    op[0]=m_pos[0]; op[1]=m_pos[1]; op[2]=m_pos[2]; op+=3;
    goto match_done;
    for(;;){
match:
      if(t>=64){ m_pos=op-1; m_pos-=(t>>2)&7; m_pos-=*ip++<<3; t=(t>>5)-1; TEST_LB(m_pos); NEED_OP(t+3-1); goto copy_match; }
      else if(t>=32){
        t&=31;
        if(t==0){ while(*ip==0){ t+=255; ip++; if(t>0xffff0000u) return -1; NEED_IP(1); } t+=31+*ip++; NEED_IP(2); }
        m_pos=op-1; m_pos-=(ip[0]|(ip[1]<<8))>>2; ip+=2;
      } else if(t>=16){
        m_pos=op; m_pos-=(t&8)<<11; t&=7;
        if(t==0){ while(*ip==0){ t+=255; ip++; if(t>0xffff0000u) return -1; NEED_IP(1); } t+=7+*ip++; NEED_IP(2); }
        m_pos-=(ip[0]|(ip[1]<<8))>>2; ip+=2;
        /* end marker: the game's decoder (a per-opcode jump-table build) only accepts the canonical 0x11 0x00 0x00;
           a zero-distance M4 with any other length (0x10 + extension, 0x12..0x17) is decoded as a 0x4000-back match,
           usually a look-behind overrun (-6). Plain LZO 2.02 would stop here instead (regress/natlib T1 'corrupt'). */
        if(m_pos==op && t==1) goto eof_found;
        m_pos-=0x4000;
      } else {
        m_pos=op-1; m_pos-=t>>2; m_pos-=*ip++<<2; TEST_LB(m_pos); NEED_OP(2);
        op[0]=m_pos[0]; op[1]=m_pos[1]; op+=2; goto match_done;
      }
      TEST_LB(m_pos); NEED_OP(t+3-1);
copy_match:
      { uint32_t n=t+2, dist=(uint32_t)(op-m_pos);
        if(dist>=8){ uint8_t *d=op, *e=op+n; const uint8_t *s=m_pos; do{ lzo_copy8(d,s); d+=8; s+=8; }while(d<e); op+=n; }
        else { const uint8_t *s=m_pos; do *op++=*s++; while(--n); } }
match_done:
      t=ip[-2]&3;
      if(t==0) break;
match_next:
      NEED_OP(t); NEED_IP(t+3);
      *op++=*ip++; if(t>1){ *op++=*ip++; if(t>2) *op++=*ip++; }
      t=*ip++;
    }
  }
eof_found:
  *olen=(uint32_t)(op-out);
  return ip==ip_end?0:(ip<ip_end?-8:-4);
#undef NEED_IP
#undef NEED_OP
#undef TEST_LB
}
extern void f_00e106e0_x86(CPU*) __attribute__((weak));
static __thread uint8_t *t_lzo_buf; static __thread uint32_t t_lzo_cap;
/* returns 1 if handled natively, 0 if the caller must run the original */
static int nat_lzo(CPU*c){
  uint32_t sp=c->esp, in=R32(sp+4), in_len=R32(sp+8), out=R32(sp+12), plen=R32(sp+16), cap=R32(plen);
  if(in_len<3 || cap>(64u<<20)) return 0;
  if((uint64_t)in+in_len>0x100000000ull || (uint64_t)out+cap>0x100000000ull) return 0;
  if(in<out+cap && out<in+in_len) return 0;                     /* overlapping buffers: original semantics, original code */
  if(cap>t_lzo_cap){ free(t_lzo_buf); t_lzo_cap=(cap+0xffff)&~0xffffu; t_lzo_buf=malloc(t_lzo_cap+LZO_SLACK); if(!t_lzo_buf){ t_lzo_cap=0; return 0; } }
  uint32_t olen=0;
  if(lzo1x_dec(MEMP(in),in_len,t_lzo_buf,cap,&olen)!=0) return 0;
  memcpy(MEMP(out),t_lzo_buf,olen);
  /* the original's other writes: push slots, reused argument slots, *out_len */
  W32(sp-4,c->ebx); W32(sp-8,c->ebp); W32(sp-12,c->esi); W32(sp-16,c->edi);
  W32(sp+4,in+in_len); W32(sp+8,out+cap); W32(plen,olen);
  c->eax=0; c->ecx=in+in_len; c->edx=plen; set_flags_logic(c,0); c->esp=sp+4;
  ADD(st_bytes[NF_LZO],olen);
  return 1;
}

/* ================= zlib 1.2.8 inflate_fast (0x00ddc170) ================= */
#define ZS_MSG_TOOFAR 0x1280b88u   /* "invalid distance too far back" */
#define ZS_MSG_DIST   0x1280ba8u   /* "invalid distance code" */
#define ZS_MSG_LEN    0x1280bc0u   /* "invalid literal/length code" */
static void nat_infl(CPU*c){
  uint32_t sp=c->esp, strm=R32(sp+4), start=R32(sp+8);
  uint32_t state=R32(strm+0x1c);
  /* guest addresses, 32-bit arithmetic exactly as the original (pointers are 32-bit there) */
  uint32_t in=R32(strm+0)-1, last=in+(R32(strm+4)-5);
  uint32_t out=R32(strm+12)-1, beg=out-(start-R32(strm+16)), end=out+(R32(strm+16)-257);
  uint32_t wsize=R32(state+0x28), whave=R32(state+0x2c), wnext=R32(state+0x30), window=R32(state+0x34);
  uint32_t hold=R32(state+0x38), bits=R32(state+0x3c), lcode=R32(state+0x4c), dcode=R32(state+0x50);
  uint32_t lmask=(1u<<R32(state+0x54))-1, dmask=(1u<<R32(state+0x58))-1;
  uint32_t here, op, len, dist, from;
  uint8_t *const m=MEMP(0);
  uint32_t out0=out;
#define PUPI() (m[(uint32_t)++in])
#define CP() do{ m[(uint32_t)++out]=m[(uint32_t)++from]; }while(0)
  do {
    if(bits<15){ hold+=(uint32_t)PUPI()<<bits; bits+=8; hold+=(uint32_t)PUPI()<<bits; bits+=8; }
    here=R32(lcode+4*(hold&lmask));
  dolen:
    op=(here>>8)&0xff; hold>>=op; bits-=op; op=here&0xff;
    if(op==0){ m[(uint32_t)++out]=(uint8_t)(here>>16); }
    else if(op&16){
      len=here>>16; op&=15;
      if(op){ if(bits<op){ hold+=(uint32_t)PUPI()<<bits; bits+=8; } len+=hold&((1u<<op)-1); hold>>=op; bits-=op; }
      if(bits<15){ hold+=(uint32_t)PUPI()<<bits; bits+=8; hold+=(uint32_t)PUPI()<<bits; bits+=8; }
      here=R32(dcode+4*(hold&dmask));
    dodist:
      op=(here>>8)&0xff; hold>>=op; bits-=op; op=here&0xff;
      if(op&16){
        dist=here>>16; op&=15;
        if(bits<op){ hold+=(uint32_t)PUPI()<<bits; bits+=8; if(bits<op){ hold+=(uint32_t)PUPI()<<bits; bits+=8; } }
        dist+=hold&((1u<<op)-1); hold>>=op; bits-=op;
        op=out-beg;
        if(dist>op){
          op=dist-op;
          if(op>whave){ if(R32(state+0x1bc0)){ W32(strm+0x18,ZS_MSG_TOOFAR); W32(state,29); break; } }
          from=window-1;
          if(wnext==0){ from+=wsize-op; if(op<len){ len-=op; do CP(); while(--op); from=out-dist; } }
          else if(wnext<op){
            from+=wsize+wnext-op; op-=wnext;
            if(op<len){ len-=op; do CP(); while(--op); from=window-1;
              if(wnext<len){ op=wnext; len-=op; do CP(); while(--op); from=out-dist; } }
          } else { from+=wnext-op; if(op<len){ len-=op; do CP(); while(--op); from=out-dist; } }
          while(len>2){ CP(); CP(); CP(); len-=3; }
          if(len){ CP(); if(len>1) CP(); }
        } else {
          from=out-dist;
          if(dist>=len && len>=3){          /* non-overlapping run: the byte loop below writes exactly these len bytes */
            memcpy(m+(uint32_t)(out+1),m+(uint32_t)(from+1),len); out+=len; from+=len; len=0;
          } else {
            do { CP(); CP(); CP(); len-=3; } while(len>2);
            if(len){ CP(); if(len>1) CP(); }
          }
        }
      }
      else if((op&64)==0){ here=R32(dcode+4*((here>>16)+(hold&((1u<<op)-1)))); goto dodist; }
      else { W32(strm+0x18,ZS_MSG_DIST); W32(state,29); break; }
    }
    else if((op&64)==0){ here=R32(lcode+4*((here>>16)+(hold&((1u<<op)-1)))); goto dolen; }
    else if(op&32){ W32(state,11); break; }
    else { W32(strm+0x18,ZS_MSG_LEN); W32(state,29); break; }
  } while(in<last && out<end);
#undef PUPI
#undef CP
  len=bits>>3; in-=len; bits-=len<<3; hold&=(1u<<bits)-1;
  W32(strm+0,in+1); W32(strm+12,out+1);
  uint32_t ain=last-in+5, a=end-out, aout=a+0x101;
  W32(strm+4,ain); W32(strm+16,aout);
  W32(state+0x38,hold); W32(state+0x3c,bits);
  /* the original's push slots (saved ebp; ebx/esi/edi below its 0x3c-byte frame) */
  W32(sp-4,c->ebp); W32(sp-0x44,c->ebx); W32(sp-0x48,c->esi); W32(sp-0x4c,c->edi);
  c->eax=state; c->ecx=strm; c->edx=ain;
  c->cf=aout<a; c->zf=aout==0; c->sf=aout>>31; c->of=((~(a^0x101u))&(a^aout))>>31; c->pf=PAR(aout);
  c->esp=sp+4;
  ADD(st_bytes[NF_INFL],out-out0);
}

/* ================= memcmp-style compare (0x00480850) ================= */
static void nat_cmp(CPU*c){
  uint32_t sp=c->esp, a=R32(sp+4), b=R32(sp+8), n=R32(sp+12);
  const uint8_t *pa=MEMP(a), *pb=MEMP(b); uint32_t i=0;
  W32(sp-4,c->esi); W32(sp-8,c->edi);
  while(i+8<=n){ uint64_t x,y; memcpy(&x,pa+i,8); memcpy(&y,pb+i,8); if(x!=y) break; i+=8; }
  while(i<n && pa[i]==pb[i]) i++;
  if(i==n){ uint32_t j=n&~3u; c->eax=0; c->edx=a+j; c->ecx=b+j; set_flags_logic(c,0); }
  else { uint32_t j=i&~3u; uint32_t r=pa[i]<pb[i]?0xffffffffu:1u; c->eax=r; c->edx=a+j; c->ecx=(i-j==3)?pb[i]:b+j; set_flags_logic(c,r); }
  c->esp=sp+4;
}

/* ================= R<->B pixel swap (0x00889a50) ================= */
static inline uint32_t swz1(uint32_t p){ return (p&0xff00ff00u)|((p&0xffu)<<16)|((p>>16)&0xffu); }
/* swz_px: pd[k] = swz1(ps[k]) for k < n, for EQUAL (in place) or DISJOINT ranges only (each step loads its whole block
   before storing it, so in place is safe; partial overlap is not, and stays on nat_swz's ordered path).
   v0.8.8: NEON (aarch64: one TBL byte shuffle per 16 bytes) / SSE2 (x86-64: and/shift/or, no SSSE3 needed) / scalar
   tail and fallback. Byte-exact by construction (a pure per-byte permutation of each 4-byte pixel).
   BSI_NATIVE_SWZ=scalar forces the scalar loop (Quest A/B); -DBSI_SWZ_NOSIMD compiles the scalar loop only. */
#if defined(__aarch64__) && !defined(BSI_SWZ_NOSIMD)
#include <arm_neon.h>
#define SWZ_SIMD "neon"
#elif defined(__SSE2__) && !defined(BSI_SWZ_NOSIMD)
#include <emmintrin.h>
#define SWZ_SIMD "sse2"
#else
#define SWZ_SIMD "scalar"
#endif
static int g_swz_scalar=-1;
static void swz_px_scalar(uint8_t *pd, const uint8_t *ps, uint64_t n){
  for(uint64_t k=0;k<n;k++){ uint32_t p; memcpy(&p,ps+4*k,4); p=swz1(p); memcpy(pd+4*k,&p,4); } }
static void swz_px(uint8_t *pd, const uint8_t *ps, uint64_t n){
  if(__builtin_expect(g_swz_scalar<0,0)){ const char *e=getenv("BSI_NATIVE_SWZ"); g_swz_scalar=e&&!strcmp(e,"scalar"); }
  uint64_t k=0;
  if(!g_swz_scalar){
#if defined(__aarch64__) && !defined(BSI_SWZ_NOSIMD)
    static const uint8_t ix[16]={2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15};
    const uint8x16_t t=vld1q_u8(ix);
    for(;k+16<=n;k+=16){ const uint8_t *s=ps+4*k; uint8_t *d=pd+4*k;
      uint8x16_t a=vld1q_u8(s), b=vld1q_u8(s+16), c=vld1q_u8(s+32), e=vld1q_u8(s+48);
      vst1q_u8(d,vqtbl1q_u8(a,t)); vst1q_u8(d+16,vqtbl1q_u8(b,t)); vst1q_u8(d+32,vqtbl1q_u8(c,t)); vst1q_u8(d+48,vqtbl1q_u8(e,t)); }
    for(;k+4<=n;k+=4) vst1q_u8(pd+4*k,vqtbl1q_u8(vld1q_u8(ps+4*k),t));
#elif defined(__SSE2__) && !defined(BSI_SWZ_NOSIMD)
    const __m128i ag=_mm_set1_epi32((int)0xff00ff00u), lo=_mm_set1_epi32(0xff);
#define SWZ4(x) _mm_or_si128(_mm_and_si128((x),ag),_mm_or_si128(_mm_slli_epi32(_mm_and_si128((x),lo),16),_mm_and_si128(_mm_srli_epi32((x),16),lo)))
    for(;k+16<=n;k+=16){ const __m128i *s=(const __m128i*)(ps+4*k); __m128i *d=(__m128i*)(pd+4*k);
      __m128i a=_mm_loadu_si128(s), b=_mm_loadu_si128(s+1), c=_mm_loadu_si128(s+2), e=_mm_loadu_si128(s+3);
      _mm_storeu_si128(d,SWZ4(a)); _mm_storeu_si128(d+1,SWZ4(b)); _mm_storeu_si128(d+2,SWZ4(c)); _mm_storeu_si128(d+3,SWZ4(e)); }
    for(;k+4<=n;k+=4){ __m128i a=_mm_loadu_si128((const __m128i*)(ps+4*k)); _mm_storeu_si128((__m128i*)(pd+4*k),SWZ4(a)); }
#undef SWZ4
#endif
  }
  swz_px_scalar(pd+4*k,ps+4*k,n-k);
}
static const char *swz_kind(void){ const char *e=getenv("BSI_NATIVE_SWZ"); return e&&!strcmp(e,"scalar")?"scalar (BSI_NATIVE_SWZ)":SWZ_SIMD; }
static void nat_swz(CPU*c){
  uint32_t sp=c->esp, src=R32(sp+4), dst=R32(sp+8), n=R32(sp+12);
  uint32_t ebp1=sp-4, esp1=(ebp1&~15u)-0x14, esp2=esp1-12;
  W32(sp-4,c->ebp); W32(esp1-4,c->ebx); W32(esp1-8,c->esi); W32(esp1-12,c->edi);
  int simd=!(n&3) && !(src&15);
  uint64_t bytes=(uint64_t)n*4;
  uint8_t *ps=MEMP(src), *pd=MEMP(dst);
  int disjoint = src==dst || (uint64_t)src+bytes<=dst || (uint64_t)dst+bytes<=src;
  uint32_t lastp=0;
  if(simd){ for(int k=0;k<4;k++) W32(esp2+0x10+4*k,0xffu); }
  if(n && (uint64_t)src+bytes<=0x100000000ull && (uint64_t)dst+bytes<=0x100000000ull && disjoint){
    /* last source pixel / block, read before the loop: equal to what the original reads last (src==dst writes it last) */
    uint32_t lb[4]; memcpy(&lastp,ps+bytes-4,4); if(simd) memcpy(lb,ps+bytes-16,16);
    swz_px(pd,ps,n);
    if(simd){ for(int k=0;k<4;k++){ uint32_t p=lb[k];
        c->xmm[0].u32[k]=(p>>16)&0xff; c->xmm[1].u32[k]=0xff; c->xmm[4].u32[k]=(p&0xff00u)|((p>>16)&0xff);
        c->xmm[3].u32[k]=((p&0xff)<<16)|(p&0xff00u)|((p>>16)&0xff); c->xmm[2].u32[k]=swz1(p); } }
  } else if(n){
    /* overlapping or wrapping ranges: the original's exact order (16-byte blocks or single pixels, read before write) */
    if(simd){ for(uint32_t blk=0;blk<n/4;blk++){ uint32_t lb[4]; for(int k=0;k<4;k++) lb[k]=R32(src+16*blk+4*k);
        for(int k=0;k<4;k++) W32(dst+16*blk+4*k,swz1(lb[k]));
        if(blk==n/4-1) for(int k=0;k<4;k++){ uint32_t p=lb[k];
          c->xmm[0].u32[k]=(p>>16)&0xff; c->xmm[1].u32[k]=0xff; c->xmm[4].u32[k]=(p&0xff00u)|((p>>16)&0xff);
          c->xmm[3].u32[k]=((p&0xff)<<16)|(p&0xff00u)|((p>>16)&0xff); c->xmm[2].u32[k]=swz1(p); } } }
    else for(uint32_t k=0;k<n;k++){ uint32_t p=R32(src+4*k); lastp=p; W32(dst+4*k,swz1(p)); }
  }
  c->eax=0; c->ecx=src+4*n; c->edx=dst+4*n;
  if(!simd && n) c->eax=lastp&0xff00ff00u;   /* scalar path: eax = last source pixel & 0xff00ff00 */
  set_flags_logic(c,0);
  c->esp=sp+4;
  ADD(st_bytes[NF_SWZ],bytes);
}

/* ================= differential check + fuzz (BSI_NATIVE_CHECK / BSI_NATIVE_FUZZ) ================= */
typedef struct { uint32_t a, n; uint8_t *pre, *ref; int nocmp; } Reg;
#define MAXR 8
typedef struct { Reg r[MAXR]; int nr; } Regs;
static void regs_add(Regs *g, uint32_t a, uint32_t n){ if(g->nr<MAXR && n && n<=(64u<<20) && (uint64_t)a+n<=0x100000000ull){ Reg *r=&g->r[g->nr++]; r->a=a; r->n=n; r->nocmp=0; r->pre=malloc(n); r->ref=malloc(n); memcpy(r->pre,MEMP(a),n); } }
static void regs_free(Regs *g){ for(int i=0;i<g->nr;i++){ free(g->r[i].pre); free(g->r[i].ref); } g->nr=0; }
/* the guest regions a call may write, from its (current) arguments */
static void regions(int k, CPU*c, Regs *g){
  uint32_t sp=c->esp; g->nr=0;
  switch(k){
  case NF_LZO: { uint32_t out=R32(sp+12), pl=R32(sp+16); regs_add(g,sp-16,16+20); regs_add(g,pl,4); regs_add(g,out,R32(pl)); break; }
  case NF_INFL: { uint32_t strm=R32(sp+4), st=R32(strm+0x1c); regs_add(g,sp-4,4); regs_add(g,sp-0x4c,12); regs_add(g,sp+4,4);
    regs_add(g,sp+8,4); if(g->nr) g->r[g->nr-1].nocmp=1;   /* 'start' slot: MSVC scratch in the original, restored but not compared */
    regs_add(g,strm,0x20); regs_add(g,st,0x40); regs_add(g,R32(strm+12),R32(strm+16)); break; }
  case NF_CMP: regs_add(g,sp-8,8+16); break;
  case NF_SWZ: { uint32_t lo=((sp-4)&~15u)-0x20; regs_add(g,lo,sp+16-lo); regs_add(g,R32(sp+8),R32(sp+12)*4); break; }
  }
}
static int cpu_same(int k, const CPU*a, const CPU*b, char *why, size_t wn){
#define CK(f) if(a->f!=b->f){ snprintf(why,wn,#f " x86 %x native %x",(unsigned)a->f,(unsigned)b->f); return 0; }
  CK(eax) CK(ecx) CK(edx) CK(ebx) CK(esp) CK(ebp) CK(esi) CK(edi) CK(cf) CK(zf) CK(sf) CK(of) CK(pf)
#undef CK
  for(int x=0;x<8;x++) if(memcmp(&a->xmm[x],&b->xmm[x],16)){ snprintf(why,wn,"xmm%d differs",x); return 0; }
  return 1;
}
static int native_run(int k, CPU*c);   /* 1 = handled natively */
static void x86_run(int k, CPU*c);
/* runs the original and the native on the same state; leaves the original's result in place. Returns 1 if equal. */
static int diff_once(int k, CPU*c, const char *tag){
  Regs g; regions(k,c,&g); CPU c0=*c, cx, cn;
  x86_run(k,c); cx=*c;
  for(int i=0;i<g.nr;i++){ memcpy(g.r[i].ref,MEMP(g.r[i].a),g.r[i].n); memcpy(MEMP(g.r[i].a),g.r[i].pre,g.r[i].n); }
  *c=c0; int ok=1; char why[200]="";
  if(!native_run(k,c)){ /* native declined (fallback): result is the original's by construction */ *c=cx; for(int i=0;i<g.nr;i++) memcpy(MEMP(g.r[i].a),g.r[i].ref,g.r[i].n); regs_free(&g); return 1; }
  cn=*c;
  if(!cpu_same(k,&cx,&cn,why,sizeof why)) ok=0;
  for(int i=0;ok && i<g.nr;i++) if(!g.r[i].nocmp && memcmp(MEMP(g.r[i].a),g.r[i].ref,g.r[i].n)){ uint32_t j=0; while(MEMP(g.r[i].a)[j]==g.r[i].ref[j]) j++;
    snprintf(why,sizeof why,"memory %08x (+%x of region %08x len %x): x86 %02x native %02x",g.r[i].a+j,j,g.r[i].a,g.r[i].n,g.r[i].ref[j],MEMP(g.r[i].a)[j]); ok=0; }
  if(!ok){ static int nlog; if(nlog++<50) fprintf(stderr,"[native] MISMATCH %s (%s): %s | args %08x %08x %08x %08x eax x86 %08x\n",nf_name[k],tag,why,
      R32(c0.esp+4),R32(c0.esp+8),R32(c0.esp+12),R32(c0.esp+16),cx.eax); }
  *c=cx; for(int i=0;i<g.nr;i++) memcpy(MEMP(g.r[i].a),g.r[i].ref,g.r[i].n);
  regs_free(&g); return ok;
}
static uint32_t rnd(uint64_t *s){ *s^=*s<<13; *s^=*s>>7; *s^=*s<<17; return (uint32_t)(*s>>11); }
/* fuzz variants of one live call: build a call frame below the live one, run both versions, restore everything */
static void fuzz(int k, CPU*c, int nvar){
  static __thread uint64_t seed; if(!seed) seed=0x9e3779b97f4a7c15ull^now_ns();
  uint32_t sp=c->esp, a1=R32(sp+4), a2=R32(sp+8), a3=R32(sp+12), a4=R32(sp+16), a5=R32(sp+20);
  uint32_t f=((sp-0x800)&~15u)-4;   /* fake frame: [ret][args...][scratch], well below the live frame */
  for(int v=0;v<nvar;v++){
    CPU save=*c; uint32_t A[6]={a1,a2,a3,a4,a5,0}; uint32_t mut_a=0, mut_n=0; uint8_t *mut_save=0; uint32_t outlen_slot=f+0x40;
    uint32_t r=rnd(&seed);
    switch(k){
    case NF_LZO: { uint32_t cap=R32(a4); W32(outlen_slot,cap); A[3]=outlen_slot;
      switch(r%5){
      case 0: A[1]=a2?rnd(&seed)%a2:0; break;                          /* truncated input */
      case 1: W32(outlen_slot,cap?rnd(&seed)%cap:0); break;            /* output too small */
      case 2: case 3: mut_a=a1; mut_n=a2; break;                       /* corrupted input */
      default: A[1]=a2+1+rnd(&seed)%16; break;                         /* trailing bytes (input not consumed) */
      } break; }
    case NF_INFL: { uint32_t st=R32(a1+0x1c), ain=R32(a1+4); (void)st; mut_a=R32(a1); mut_n=ain; break; }
    case NF_CMP: { uint32_t n=a3; A[2]=n?rnd(&seed)%(n+1):0;
      if(r&3){ A[2]=A[2]>=4?A[2]-4:0; if(r&1) A[0]=a1+(rnd(&seed)%4); if(r&2) A[1]=a2+(rnd(&seed)%4); }   /* stay inside both buffers */
      break; }
    case NF_SWZ: { uint32_t n=a3; if(!n) continue; uint32_t s=rnd(&seed)%n, t=rnd(&seed)%n; uint32_t m=n-(s>t?s:t); A[2]=rnd(&seed)%(m+1);
      if(r&1) A[0]=a1+4*s;
      if(r&2) A[1]=a2+4*t;
      if(r&4) A[2]&=~3u;
      break; }
    }
    Regs ms={0};
    if(mut_n && mut_n<(16u<<20)){ mut_save=malloc(mut_n); memcpy(mut_save,MEMP(mut_a),mut_n);
      int flips=1+rnd(&seed)%4; for(int q=0;q<flips;q++) MEMP(mut_a)[rnd(&seed)%mut_n]^=(uint8_t)(1u<<(rnd(&seed)%8)); }
    if(k==NF_INFL){ /* inflate_fast mutates strm/state: snapshot them and the output, restore after */
      regs_add(&ms,a1,0x20); regs_add(&ms,R32(a1+0x1c),0x40); regs_add(&ms,R32(a1+12),R32(a1+16)); }
    if(k==NF_LZO){ regs_add(&ms,a3,R32(a4)); }
    if(k==NF_SWZ){ regs_add(&ms,a2,a3*4); }
    W32(f,0xdead0000u); for(int q=0;q<5;q++) W32(f+4+4*q,A[q]);
    c->esp=f; int ok=diff_once(k,c,"fuzz");
    ADD(fz_n[k],1); if(!ok) ADD(fz_bad[k],1);
    for(int i=0;i<ms.nr;i++) memcpy(MEMP(ms.r[i].a),ms.r[i].pre,ms.r[i].n);
    regs_free(&ms);
    if(mut_save){ memcpy(MEMP(mut_a),mut_save,mut_n); free(mut_save); }
    *c=save;
  }
}

/* ================= dispatchers ================= */
extern void f_00ddc170_x86(CPU*) __attribute__((weak));
extern void f_00480850_x86(CPU*) __attribute__((weak));
extern void f_00889a50_x86(CPU*) __attribute__((weak));
static void x86_run(int k, CPU*c){
  switch(k){ case NF_LZO: f_00e106e0_x86(c); break; case NF_INFL: f_00ddc170_x86(c); break; case NF_CMP: f_00480850_x86(c); break; case NF_SWZ: f_00889a50_x86(c); break; } }
static int native_run(int k, CPU*c){
  switch(k){ case NF_LZO: return nat_lzo(c); case NF_INFL: nat_infl(c); return 1; case NF_CMP: nat_cmp(c); return 1; case NF_SWZ: nat_swz(c); return 1; }
  return 0; }
static void dispatch_nat(int k, CPU*c){
  if(!nat_on(k)){
    if(__builtin_expect(g_stat,0)){   /* build 64: time the recompiled originals too, so a log shows what the natives would save */
      uint32_t olp=k==NF_LZO?R32(c->esp+16):0; uint64_t t0=now_ns(); x86_run(k,c); uint64_t t1=now_ns();
      ADD(st_calls[k],1); ADD(st_ns[k],t1-t0); if(olp) ADD(st_bytes[k],R32(olp)); stat_report(t1); return; }
    x86_run(k,c); return; }
  uint64_t t0=g_stat?now_ns():0;
  ADD(st_calls[k],1);
  if(__builtin_expect(g_check,0)){
    uint64_t n=ADD(ck_n[k],1);
    if(n%g_check_every==0){
      if(g_fuzz && n/g_check_every<(uint64_t)g_fuzz_calls) fuzz(k,c,g_fuzz);
      if(!diff_once(k,c,"live")) ADD(ck_bad[k],1);
      ADD(st_nat[k],1);
      if(g_stat){ ADD(st_ns[k],now_ns()-t0); stat_report(now_ns()); }
      return; }
  }
  if(native_run(k,c)) ADD(st_nat[k],1); else { ADD(st_fallback[k],1); x86_run(k,c); }
  if(g_stat){ uint64_t t1=now_ns(); ADD(st_ns[k],t1-t0); stat_report(t1); }
}
/* weak: a gen tree without the _x86 rename defines these strongly and this file is not used */
__attribute__((weak)) void f_00e106e0(CPU*c){ dispatch_nat(NF_LZO,c); }
__attribute__((weak)) void f_00ddc170(CPU*c){ dispatch_nat(NF_INFL,c); }
__attribute__((weak)) void f_00480850(CPU*c){ dispatch_nat(NF_CMP,c); }
__attribute__((weak)) void f_00889a50(CPU*c){ dispatch_nat(NF_SWZ,c); }
