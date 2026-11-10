/* audio_native.c -- native replacements for the hot Wwise DSP kernels + Wwise setting hooks (notes/AUDIO_NATIVE.md).

   The Wwise audio thread (CAkAudioThread, start 0xFA57D0) was ~8-10 % of all app CPU in the v0.8.6 Quest profile
   (notes/HOTCODE.md). Its hot leaves are plain float DSP loops; recompiled, every SSE op goes through c->xmm[] memory and
   every x87 op through c->st[]. This file re-implements them in C (vector parts with GCC/clang vector extensions, which
   become NEON on aarch64) with the SAME operation order, so results are bit-identical to the recompiled originals
   (IEEE single/double ops, no FMA contraction: see the pragmas below). Host test: regress/audio/run.sh.

   Hooking: nothing here runs unless the hooked part objects are linked (regress/audio/audio_parts.sh rebuilds
   part038/039/040 from gen_promo/ with each target renamed to f_XXXXXXXX_x86 and a small wrapper
   `f_XXXXXXXX(c){ if(aud_native_try && aud_native_try(K,c)) return; f_XXXXXXXX_x86(c); }` added; direct AND indirect
   calls reach the wrapper because the functab keeps the f_XXXXXXXX symbol). Without this file the wrappers see a NULL
   weak symbol and always run the original.

   Kernels (guest address, what it is, convention, notes):
     fdn   0xF6C970  RoomVerb reverb unit: 4 delay lines + 1-pole damping + Householder (-0.5) feedback.  thiscall, ret 0xC
     dly   0xF6BD60  RoomVerb delay line: swap a block with the ring buffer.                              thiscall, ret 8
     ap    0xF6D060  RoomVerb input diffusion all-pass (x/y pairs in one ring).                           thiscall, ret 8
     er    0xF6C240  RoomVerb early reflections: 2x up to 64 taps, 4 frames per step (SSE).               thiscall, ret 0x10
     imdct 0xFC2D10  Vorbis inverse MDCT (Wwise's SSE float port of Tremor mdct_backward).                cdecl (n, x)
     lim   0xF660D0  PeakLimiter Execute body (x87 log10 / table dB->lin, look-ahead delay).             thiscall, ret 4
     win   0xFC3910  Vorbis window + overlap-add + float->int16 (Tremor mdct_unroll_lap, float version).  cdecl, 12 args
     book  0xFC6270  Vorbis codebook entry decode (Tremor-lowmem decode_packed_entry_number, Wwise bit reader
                     0xFC4210 look / 0xFC4290 adv inlined).                          eax = codebook*, cdecl (oggpack_buffer*)
     bookv 0xFC6440  Vorbis decode_map (entry -> dequantised int32 vector), calls book.  cdecl (book, b, int32 *v, point)
     lpf   0xFB16C0  per-voice low-pass biquad (CAkSrcLpFilter mono kernel).     eax = coefs[4], esi = state[4], cdecl (float **pbuf, n)
     mix   0xFADC40  bus mixer: dst += src * gain, constant or ramped gain, 8 samples per step (SSE).    thiscall, ret 0x10
     peq   0xF64F30  ParametricEQ biquad bank, all channels of a buffer.   eax = coefs[5], cdecl (AkAudioBuffer*, float st[4][nch], nch)
     bq    0xF4A3D0  biquad used by RoomVerb tone controls (and GuitarDistortion), state in the object.  thiscall, ret 8
   What a native reproduces: every byte the original writes that outlives the call (output buffers, delay lines, object
   state), esp after `ret`, eax for value-returning functions, and ebx/esi/edi/ebp. Not reproduced: the dead stack frame
   below the final esp, argument slots the callee reuses as scratch (callee-popped or immediately discarded by the
   caller: checked in notes/AUDIO_NATIVE.md), and ecx/edx/xmm/flags (and eax, except for book/bookv) on exit --
   regress/audio/liveout.py shows that no caller reads any of them before writing it. The x87 stack (c->st/c->top) is
   never touched: ParametricEQ callers keep st0 live across the call. x87 fld/fstp copies are raw 32-bit copies here
   (differs only for signalling NaNs, which audio data never contains).

   Env (read once):
     BSI_AUDIO_NATIVE=0|1|all|<list>   kernels to run natively (comma-separated list: fdn,dly,ap,er,imdct,lim,win,book,bookv,lpf,mix,peq,bq). Default: off.
     BSI_AUDIO_NATIVE_CHECK=k          differential check on every k-th call of each enabled kernel: run the original,
                                       snapshot what it wrote, restore, run the native, compare, keep the ORIGINAL's
                                       result; mismatches are logged ("[aknat] MISMATCH ...") and counted.
     BSI_AUDIO_STAT=1                  every 10 s: per-kernel calls / native / ns, Wwise voice counts ("[aknat] ...").
     BSI_AK_VOICES=n                   AK::SoundEngine::SetMaxNumVoicesLimit(n) right after SoundEngine::Init (L1).
     BSI_AK_VOLTHR=dB                  AK::SoundEngine::SetVolumeThreshold(dB) right after Init (L1; -96.3..0).
     BSI_AK_LOWQ=1                     AkPlatformInitSettings.eAudioQuality = Low (24 kHz pipeline) before Init (L2).
     BSI_AK_REFILLS=n                  AkPlatformInitSettings.uNumRefillsInVoice (2..) before Init (L2 companion).
     BSI_AK_REVERB_UNITS=n             cap RoomVerb "Quality" (reverb units) at Init of each RoomVerb instance (L3).
     BSI_AK_ER=0                       turn RoomVerb early reflections off at Init (L3, changes the room sound).
     BSI_AK_THREADNAME=0               do not name the Wwise audio thread "AkAudioThread" (default: named, so Quest
                                       simpleperf/profile.txt show it instead of the process name). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pthread_setname_np */
#endif
#include "hle.h"
#include <time.h>
#include <pthread.h>
#include <stddef.h>
#include <math.h>
#ifdef __clang__
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

typedef float v4f __attribute__((vector_size(16), aligned(4), may_alias));
typedef float af32 __attribute__((may_alias));
typedef uint16_t au16 __attribute__((may_alias));
typedef uint32_t au32 __attribute__((may_alias));
#define FP(a) ((af32*)MEMP(a))
#define SHUF(a, b, i, j, k, l) __builtin_shufflevector((a), (b), (i), (j), (k), (l))
static inline v4f LDV(const af32 *p){ v4f v; memcpy(&v, p, 16); return v; }
static inline void STV(af32 *p, v4f v){ memcpy(p, &v, 16); }

enum { AK_FDN = 0, AK_DLY, AK_AP, AK_ER, AK_IMDCT, AK_LIM, AK_WIN, AK_BOOK, AK_BOOKV, AK_LPF, AK_MIX, AK_PEQ, AK_BQ, AK_N };
static const char *const ak_name[AK_N] = { "fdn", "dly", "ap", "er", "imdct", "lim", "win", "book", "bookv", "lpf", "mix", "peq", "bq" };
static int g_mask = -1, g_check, g_stat;
static int s_voices = -1, s_lowq, s_refills, s_units, s_er = -1, s_tname = 1; static float s_volthr; static int s_has_volthr;

static void ak_init(void){
  int m = 0; const char *e = getenv("BSI_AUDIO_NATIVE");
  if(e){ if(!strcmp(e, "1") || !strcmp(e, "all")) m = (1 << AK_N) - 1;
    else for(const char *t = e; *t; ){ size_t l = strcspn(t, ",; "); for(int k = 0; k < AK_N; k++) if(l == strlen(ak_name[k]) && !strncmp(t, ak_name[k], l)) m |= 1 << k; t += l; if(*t) t++; } }
  e = getenv("BSI_AUDIO_NATIVE_CHECK"); g_check = e ? atoi(e) : 0; if(g_check < 0) g_check = 0;
  e = getenv("BSI_AUDIO_STAT"); g_stat = e && atoi(e);
  e = getenv("BSI_AK_VOICES"); if(e && atoi(e) > 0) s_voices = atoi(e);
  e = getenv("BSI_AK_VOLTHR"); if(e && *e){ s_volthr = (float)atof(e); s_has_volthr = 1; }
  e = getenv("BSI_AK_LOWQ"); s_lowq = e && atoi(e);
  e = getenv("BSI_AK_REFILLS"); if(e && atoi(e) >= 2) s_refills = atoi(e);
  e = getenv("BSI_AK_REVERB_UNITS"); if(e && atoi(e) >= 2) s_units = atoi(e) & ~1;   /* units alternate L/R: keep even */
  e = getenv("BSI_AK_ER"); if(e) s_er = atoi(e) != 0;
  e = getenv("BSI_AK_THREADNAME"); if(e) s_tname = atoi(e) != 0;
  fprintf(stderr, "[aknat] Wwise natives:");
  for(int k = 0; k < AK_N; k++) fprintf(stderr, " %s=%s", ak_name[k], (m >> k) & 1 ? "native" : "x86");
  fprintf(stderr, "%s; voices %d volthr %s%.1f lowq %d refills %d units %d er %d\n", g_check ? " (check on)" : "",
          s_voices, s_has_volthr ? "" : "(off)", s_volthr, s_lowq, s_refills, s_units, s_er);
  __atomic_store_n(&g_mask, m, __ATOMIC_RELEASE);
}
static inline int ak_mask(void){ int m = __atomic_load_n(&g_mask, __ATOMIC_ACQUIRE); if(__builtin_expect(m < 0, 0)){ ak_init(); m = g_mask; } return m; }

/* ================= kernels: 1 = done (full effect incl. ret), 0 = declined (caller runs the original) ================= */

/* ---- dly 0xF6BD60: thiscall(line{len,buf,idx}, float *io, uint n), ret 8 ---- */
static int nk_dly(CPU *c){
  uint32_t L = c->ecx, io = R32(c->esp + 4), n = R32(c->esp + 8);
  uint32_t len = R32(L), buf = R32(L + 4), idx = R32(L + 8), room = len - idx;
  af32 *x = FP(io), *b = FP(buf) + idx;
  if(room > n){
    for(uint32_t i = 0; i < n; i++){ float t = b[i]; b[i] = x[i]; x[i] = t; }
    W32(L + 8, idx + n);
  } else if(n){
    uint32_t rem = n, p = idx;
    do{
      uint32_t k = rem >= room ? room : rem;
      for(uint32_t i = 0; i < k; i++){ float t = b[i]; b[i] = x[i]; x[i] = t; }
      x += k; b += k; p += k;
      if(p == len){ p = 0; b = FP(buf); }
      rem -= k; room = len - p;
    } while(rem);
    W32(L + 8, p);
  }
  c->esp += 12; return 1;
}

/* ---- ap 0xF6D060: thiscall(ap{len, buf (x,y pairs), idx, g}, float *io, uint n), ret 8 ---- */
static int nk_ap(CPU *c){
  uint32_t A = c->ecx, io = R32(c->esp + 4), n = R32(c->esp + 8);
  uint32_t len = R32(A), buf = R32(A + 4), idx = R32(A + 8), room = len - idx;
  float g = RF32(A + 12);
  af32 *x = FP(io), *e = FP(buf) + 2 * (size_t)idx;
  if(room > n){
    for(uint32_t i = 0; i < n; i++, e += 2){ float o = e[0], in = x[i]; e[0] = in; float y = (in - e[1]) * g + o; e[1] = y; x[i] = y; }
    W32(A + 8, idx + n);
  } else {
    uint32_t rem = n;
    do{
      uint32_t k = rem < room ? rem : room;
      for(uint32_t i = 0; i < k; i++, e += 2){ float o = e[0], in = x[i]; e[0] = in; float y = (in - e[1]) * g + o; e[1] = y; x[i] = y; }
      x += k;
      uint32_t p = R32(A + 8) + k; W32(A + 8, p);
      if(p == len){ e = FP(buf); W32(A + 8, 0); }
      rem -= k; room = len - R32(A + 8);
    } while(rem);
  }
  c->esp += 12; return 1;
}

/* ---- fdn 0xF6C970: thiscall(unit, const float *in, float *out, uint n), ret 0xC ----
   unit: 4 x {len, buf, idx} at +0/+0xC/+0x18/+0x24, then 4 x {state, b, a} at +0x30/+0x3C/+0x48/+0x54 */
static float k_fdn_h;   /* -0.5 at 0x12CD910 */
static int nk_fdn(CPU *c){
  uint32_t U = c->ecx, gin = R32(c->esp + 4), gout = R32(c->esp + 8), n = R32(c->esp + 12);
  uint32_t len0 = R32(U), len1 = R32(U + 0xc), len2 = R32(U + 0x18), len3 = R32(U + 0x24);
  uint32_t i0 = R32(U + 8), i1 = R32(U + 0x14), i2 = R32(U + 0x20), i3 = R32(U + 0x2c);
  float s0 = RF32(U + 0x30), s1 = RF32(U + 0x3c), s2 = RF32(U + 0x48), s3 = RF32(U + 0x54);
  const float b0 = RF32(U + 0x34), a0 = RF32(U + 0x38), b1 = RF32(U + 0x40), a1 = RF32(U + 0x44);
  const float b2 = RF32(U + 0x4c), a2 = RF32(U + 0x50), b3 = RF32(U + 0x58), a3 = RF32(U + 0x5c);
  af32 *d0 = FP(R32(U + 4)), *d1 = FP(R32(U + 0x10)), *d2 = FP(R32(U + 0x1c)), *d3 = FP(R32(U + 0x28));
  const af32 *in = FP(gin); af32 *out = FP(gout); const float h = k_fdn_h;
  for(uint32_t i = 0; i < n; i++){
    float y0 = b0 * d0[i0] - a0 * s0, y1 = b1 * d1[i1] - a1 * s1;
    float y2 = b2 * d2[i2] - a2 * s2, y3 = b3 * d3[i3] - a3 * s3;
    out[i] = (((y0 - y1) + y2) - y3) + out[i];
    float f = (((y3 + y2) + y1) + y0) * h, v = in[i];
    s0 = y0; s1 = y1; s2 = y2; s3 = y3;
    d0[i0] = (y1 + f) + v; d1[i1] = (y2 + f) + v; d2[i2] = (y3 + f) + v; d3[i3] = (y0 + f) + v;
    if(++i0 == len0) i0 = 0;
    if(++i1 == len1) i1 = 0;
    if(++i2 == len2) i2 = 0;
    if(++i3 == len3) i3 = 0;
  }
  W32(U + 0x2c, i3); W32(U + 0x20, i2); W32(U + 0x14, i1); W32(U + 8, i0);
  WF32(U + 0x30, s0); WF32(U + 0x3c, s1); WF32(U + 0x48, s2); WF32(U + 0x54, s3);
  c->esp += 16; return 1;
}

/* ---- er 0xF6C240: thiscall(er, const float *in, float *outL, float *outR, uint n), ret 0x10 ----
   er: +0 len, +4 buf (16-aligned ring), +8 write idx, +0xC gains L[64], +0x10C gains R[64], +0x20C u16 posL[64],
   +0x28C u16 posR[64], +0x30C u16 next-wrap tap L, +0x30E same R, +0x310 u16 nL, +0x312 u16 nR. */
static int nk_er(CPU *c){
  uint32_t E = c->ecx, gin = R32(c->esp + 4), goL = R32(c->esp + 8), goR = R32(c->esp + 12), n = R32(c->esp + 16);
  uint32_t nL = R16(E + 0x310), nR = R16(E + 0x312);
  if(!nL || !nR) return 0;                           /* the original divides by them on wrap: keep its behaviour */
  uint32_t len = R32(E), buf = R32(E + 4);
  af32 *ring = FP(buf); const af32 *gL = FP(E + 0xc), *gR = FP(E + 0x10c);
  au16 *pL = (au16 *)MEMP(E + 0x20c), *pR = (au16 *)MEMP(E + 0x28c);
  uint32_t m4 = (nL < nR ? nL : nR) >> 2;            /* taps done 4 + 4 at a time */
  const af32 *in = FP(gin); af32 *oL = FP(goL), *oR = FP(goR);
  while(n){
    uint32_t dL = len - pL[R16(E + 0x30c)], dR = len - pR[R16(E + 0x30e)], d = dL < dR ? dL : dR, w = len - R32(E + 8);
    if(w < d) d = w;
    uint32_t chunk = n < d ? n : d;
    for(uint32_t blk = chunk >> 2; blk; blk--){
      uint32_t wi = R32(E + 8);
      STV(ring + wi, LDV(in)); in += 4; W32(E + 8, wi + 4);
      v4f aL = { 0, 0, 0, 0 }, aR = { 0, 0, 0, 0 };
      uint32_t t = 0;
      for(uint32_t g = 0; g < m4; g++, t += 4){
        aL = LDV(ring + pL[t]) * (v4f){ gL[t], gL[t], gL[t], gL[t] } + aL;
        aL = aL + LDV(ring + pL[t + 1]) * (v4f){ gL[t + 1], gL[t + 1], gL[t + 1], gL[t + 1] };
        aL = aL + LDV(ring + pL[t + 2]) * (v4f){ gL[t + 2], gL[t + 2], gL[t + 2], gL[t + 2] };
        aL = aL + LDV(ring + pL[t + 3]) * (v4f){ gL[t + 3], gL[t + 3], gL[t + 3], gL[t + 3] };
        aR = LDV(ring + pR[t]) * (v4f){ gR[t], gR[t], gR[t], gR[t] } + aR;
        aR = aR + LDV(ring + pR[t + 1]) * (v4f){ gR[t + 1], gR[t + 1], gR[t + 1], gR[t + 1] };
        aR = aR + LDV(ring + pR[t + 2]) * (v4f){ gR[t + 2], gR[t + 2], gR[t + 2], gR[t + 2] };
        aR = aR + LDV(ring + pR[t + 3]) * (v4f){ gR[t + 3], gR[t + 3], gR[t + 3], gR[t + 3] };
        for(int k = 0; k < 4; k++){ pL[t + k] += 4; pR[t + k] += 4; }
      }
      for(uint32_t k = t; k < nL; k++){ aL = LDV(ring + pL[k]) * (v4f){ gL[k], gL[k], gL[k], gL[k] } + aL; pL[k] += 4; }
      for(uint32_t k = t; k < nR; k++){ aR = LDV(ring + pR[k]) * (v4f){ gR[k], gR[k], gR[k], gR[k] } + aR; pR[k] += 4; }
      STV(oL, aL); STV(oR, aR); oL += 4; oR += 4;
    }
    uint32_t cl = R16(E + 0x30c);
    if(pL[cl] == len){ pL[cl] = 0; W16(E + 0x30c, (uint16_t)((uint16_t)(cl + 1) % nL)); }
    uint32_t cr = R16(E + 0x30e);
    if(pR[cr] == len){ pR[cr] = 0; W16(E + 0x30e, (uint16_t)((uint16_t)(cr + 1) % nR)); }
    if(R32(E + 8) == len) W32(E + 8, 0);
    n -= chunk;                                      /* the original drops chunk % 4 frames the same way */
  }
  c->esp += 20; return 1;
}

/* ---- imdct 0xFC2D10: cdecl(int n, float *x), in place ----
   Tremor's mdct_backward (presymmetric, butterflies, bitreverse, step7, step8) as Wwise vectorised it; the trig
   tables sincos_lookup0 (1026 floats) at 0x1348EA0 / sincos_lookup1 at 0x1349EA8, bitrev4 bytes at 0x134AF00 and the
   SSE sign/twiddle constants are read from the exe image in guest memory. */
#define T0ADDR 0x1348ea0u
#define T1ADDR 0x1349ea8u
static v4f K_pm, K_mp, K_mppm, K_pmmp, K_c2, K_c3c1, K_c1c3, K_ec, K_ef;   /* 0x12B20D0/E0/F0/2100, 0x134AEB0..EF0 */
static float K_half, K_quarter;
static void imdct_consts(void){
  K_pm = LDV(FP(0x12b20d0)); K_mp = LDV(FP(0x12b20e0)); K_mppm = LDV(FP(0x12b20f0)); K_pmmp = LDV(FP(0x12b2100));
  K_c2 = LDV(FP(0x134aeb0)); K_c3c1 = LDV(FP(0x134aec0)); K_ec = LDV(FP(0x134aed0)); K_c1c3 = LDV(FP(0x134aee0)); K_ef = LDV(FP(0x134aef0));
  K_half = RF32(0x12a8a60); K_quarter = RF32(0x1224038);
}
/* one 32-point block (0xFC304A..0xFC337B), p = x + 12: the SSE block transliterated op for op (regress/audio/sse2c.py) */
static inline void imdct_b32(af32 *p){
  v4f x0, x1, x2, x3, x7 = K_c2; const v4f x4 = K_mp, x5 = K_pmmp, x6 = K_mppm;
  x2 = LDV(p-12); x1 = LDV(p+4); x0 = x1; x0 = SHUF(x0, x2, 0, 2, 5, 7); x1 = SHUF(x1, x2, 1, 3, 4, 6); x2 = x1;
  x1 = x0; x1 = x1 - x2; x0 = SHUF(x0, x0, 0, 2, 5, 7); x3 = x2; x3 = SHUF(x3, x2, 0, 2, 5, 7); x3 = x3 + x0; x0 = x1;
  x0 = SHUF(x0, x1, 0, 2, 5, 7); x0 = x0 * K_c3c1; x2 = x1; x2 = SHUF(x2, x1, 1, 3, 4, 6); x2 = x2 * K_ef; x0 = x0 + x2;
  x2 = LDV(p-8); STV(p-12, x0); x0 = LDV(p+8); x1 = x0; x1 = SHUF(x1, x2, 0, 2, 5, 7); x0 = SHUF(x0, x2, 1, 3, 4, 6);
  x2 = x0; x0 = x1; x0 = x0 - x2; STV(p+4, x3); x3 = x2; x3 = SHUF(x3, x2, 0, 2, 5, 7); x2 = LDV(p-4);
  x1 = SHUF(x1, x1, 0, 2, 5, 7); x3 = x3 + x1; x1 = x0; x1 = SHUF(x1, x0, 1, 2, 5, 6); x0 = SHUF(x0, x0, 0, 3, 4, 7);
  x1 = x1 * x6; x1 = x1 + x0; x1 = x1 * x7; STV(p-8, x1); x1 = LDV(p+12); x0 = x1; x0 = SHUF(x0, x2, 0, 2, 5, 7);
  x1 = SHUF(x1, x2, 1, 3, 4, 6); STV(p+8, x3); x2 = x1; x3 = x2; x3 = SHUF(x3, x2, 0, 2, 5, 7); x1 = x0; x1 = x1 - x2;
  x0 = SHUF(x0, x0, 0, 2, 5, 7); x3 = x3 + x0; x0 = x1; x0 = SHUF(x0, x1, 0, 2, 5, 7); x0 = x0 * K_c1c3; x2 = x1;
  x2 = SHUF(x2, x1, 1, 3, 4, 6); x2 = x2 * K_ec; x0 = x0 + x2; STV(p+12, x3); x3 = LDV(p); STV(p-4, x0); x0 = LDV(p+16);
  x1 = x0; x1 = SHUF(x1, x3, 0, 2, 4, 7); x2 = x0; x2 = SHUF(x2, x3, 1, 3, 5, 6); x1 = x1 - x2; x2 = x0;
  x2 = SHUF(x2, x3, 0, 2, 5, 7); x0 = SHUF(x0, x3, 1, 3, 4, 6); x3 = x0; x3 = SHUF(x3, x0, 0, 2, 5, 7); x0 = LDV(p-4);
  x2 = SHUF(x2, x2, 0, 2, 5, 7); x3 = x3 + x2; STV(p+16, x3); x3 = LDV(p-12); x2 = x0; x2 = SHUF(x2, x3, 0, 2, 5, 7);
  x0 = SHUF(x0, x3, 1, 3, 4, 6); x3 = x0; x0 = x2; x2 = SHUF(x2, x2, 0, 2, 5, 7); x0 = x0 - x3;
  x1 = SHUF(x1, x1, 0, 3, 5, 6); x7 = x3; x7 = SHUF(x7, x3, 0, 2, 5, 7); x3 = LDV(p-8); x7 = x7 + x2; x2 = x0;
  x2 = SHUF(x2, x0, 1, 3, 5, 6); x2 = x2 * x6; x0 = SHUF(x0, x0, 0, 2, 4, 7); x2 = x2 + x0; STV(p-4, x7); x7 = K_c2;
  x2 = x2 * x7; STV(p-12, x2); x2 = x3; x2 = SHUF(x2, x1, 1, 2, 5, 7); x0 = x3; x0 = SHUF(x0, x1, 0, 3, 4, 6);
  x0 = x0 - x2; x2 = x1; x1 = SHUF(x1, x3, 1, 3, 4, 6); x2 = SHUF(x2, x3, 0, 2, 5, 7); x3 = x1;
  x3 = SHUF(x3, x1, 0, 2, 5, 7); x1 = LDV(p-12); x2 = SHUF(x2, x2, 0, 2, 5, 7); x3 = x3 + x2; x2 = x1;
  x0 = SHUF(x0, x0, 2, 1, 7, 4); x1 = SHUF(x1, x0, 0, 2, 4, 6); x2 = SHUF(x2, x0, 1, 3, 5, 7); x0 = x1; x1 = x1 - x2;
  x0 = x0 + x2; x2 = x1; x2 = SHUF(x2, x1, 1, 0, 5, 4); x1 = SHUF(x1, x1, 2, 3, 6, 7); x2 = x2 * x5; x2 = x2 + x1;
  x1 = x0; x1 = SHUF(x1, x0, 0, 1, 4, 5); STV(p-12, x2); x1 = x1 * x4; x0 = SHUF(x0, x0, 2, 3, 6, 7); x1 = x1 + x0;
  STV(p-8, x1); STV(p, x3); x0 = LDV(p-4); x1 = LDV(p); x2 = x0; x2 = SHUF(x2, x1, 1, 3, 5, 7);
  x0 = SHUF(x0, x1, 0, 2, 4, 6); x1 = x0; x0 = x0 + x2; x1 = x1 - x2; x2 = x1; x2 = SHUF(x2, x1, 1, 0, 5, 4);
  x1 = SHUF(x1, x1, 2, 3, 6, 7); x2 = x2 * x5; x2 = x2 + x1; x1 = x0; x1 = SHUF(x1, x0, 0, 1, 4, 5);
  x0 = SHUF(x0, x0, 2, 3, 6, 7); x1 = x1 * x4; x1 = x1 + x0; STV(p, x1); STV(p-4, x2); x2 = LDV(p+4); x0 = LDV(p+12);
  x1 = x0; x0 = SHUF(x0, x2, 1, 3, 4, 6); x1 = SHUF(x1, x2, 0, 2, 5, 7); x2 = x0; x0 = x1; x0 = x0 - x2; x3 = x2;
  x1 = SHUF(x1, x1, 0, 2, 5, 7); x3 = SHUF(x3, x2, 0, 2, 5, 7); x3 = x3 + x1; x1 = x0; x1 = SHUF(x1, x0, 1, 3, 5, 6);
  x1 = x1 * x6; x0 = SHUF(x0, x0, 0, 2, 4, 7); x1 = x1 + x0; x1 = x1 * x7; STV(p+4, x1); x1 = LDV(p+16); STV(p+12, x3);
  x3 = LDV(p+8); x2 = x3; x2 = SHUF(x2, x1, 1, 2, 5, 7); x0 = x3; x0 = SHUF(x0, x1, 0, 3, 4, 6); x0 = x0 - x2; x2 = x1;
  x1 = SHUF(x1, x3, 1, 3, 4, 6); x2 = SHUF(x2, x3, 0, 2, 5, 7); x3 = x1; x3 = SHUF(x3, x1, 0, 2, 5, 7); x1 = LDV(p+4);
  x2 = SHUF(x2, x2, 0, 2, 5, 7); x3 = x3 + x2; x2 = x1; x0 = SHUF(x0, x0, 2, 1, 7, 4); x1 = SHUF(x1, x0, 0, 2, 4, 6);
  x2 = SHUF(x2, x0, 1, 3, 5, 7); x0 = x1; x1 = x1 - x2; x0 = x0 + x2; x2 = x1; x2 = SHUF(x2, x1, 1, 0, 5, 4);
  x1 = SHUF(x1, x1, 2, 3, 6, 7); x2 = x2 * x5; x2 = x2 + x1; STV(p+4, x2); x1 = x0; x1 = SHUF(x1, x0, 0, 1, 4, 5);
  x1 = x1 * x4; x0 = SHUF(x0, x0, 2, 3, 6, 7); x1 = x1 + x0; STV(p+8, x1); STV(p+16, x3); x0 = LDV(p+12);
  x1 = LDV(p+16); x2 = x0; x2 = SHUF(x2, x1, 1, 3, 5, 7); x0 = SHUF(x0, x1, 0, 2, 4, 6); x1 = x0; x1 = x1 - x2;
  x0 = x0 + x2; x2 = x1; x2 = SHUF(x2, x1, 1, 0, 5, 4); x1 = SHUF(x1, x1, 2, 3, 6, 7); x2 = x2 * x5; x2 = x2 + x1;
  x1 = x0; x1 = SHUF(x1, x0, 0, 1, 4, 5); x1 = x1 * x4; x0 = SHUF(x0, x0, 2, 3, 6, 7); x1 = x1 + x0; STV(p+12, x2);
  STV(p+16, x1);
}
static int nk_imdct(CPU *c){
  int32_t n = (int32_t)R32(c->esp + 4); uint32_t gx = R32(c->esp + 8);
  if(n < 64 || n > 8192 || (n & (n - 1)) || (gx & 15)) return 0;      /* only the block sizes Vorbis uses */
  int shift = 4; while(!(n & (1 << shift))) shift++;
  shift = 13 - shift; int step = 2 << shift, n2 = n >> 1, n4 = n2 >> 1;
  af32 *x = FP(gx); const af32 *T0 = FP(T0ADDR), *T1 = FP(T1ADDR);
  /* presymmetric */
  { af32 *aX = x + n2 - 3; const af32 *T = T0;
    do{ float r0 = aX[0], r2 = aX[2]; aX[0] = T[1] * r2 + T[0] * r0; aX[2] = T[0] * r2 - T[1] * r0; aX -= 4; T += step; } while(aX >= x + n4);
    do{ float r0 = aX[0], r2 = aX[2]; aX[0] = T[0] * r2 + T[1] * r0; aX[2] = T[1] * r2 - T[0] * r0; aX -= 4; T -= step; } while(aX >= x);
    aX = x + n2 - 4; af32 *bX = x; T = T0;
    do{ float ri0 = aX[0], ri2 = aX[2], ro0 = bX[0], ro2 = bX[2];
        aX[2] = T[0] * ro2 + T[1] * ro0; aX[0] = T[1] * ro2 - T[0] * ro0; T += step;
        bX[0] = T[0] * ri2 - T[1] * ri0; bX[2] = T[1] * ri2 + T[0] * ri0;
        aX -= 4; bX += 4; } while(aX >= x + n4); }
  /* generic butterfly stages */
  for(int i = 0, stages = 7 - shift; stages > 0; i++, stages--){
    int pts = n2 >> i, tstep = 4 << (i + shift);
    for(int j = 0; j < (1 << i); j++){
      af32 *bx = x + (size_t)pts * j, *x1 = bx + pts - 4, *x2 = bx + (pts >> 1) - 4; const af32 *T = T0;
      do{ float r0 = x1[0] - x1[1], r1 = x1[3] - x1[2], r2 = x2[1] - x2[0], r3 = x2[3] - x2[2];   /* XPROD31 x2 */
          x1[0] = x1[1] + x1[0]; x1[1] = x2[0] + x2[1]; x1[2] = x1[3] + x1[2]; x1[3] = x2[2] + x2[3];
          x2[0] = T[1] * r0 + T[0] * r1; x2[1] = T[1] * r3 + T[0] * r2; x2[2] = T[0] * r0 - T[1] * r1; x2[3] = T[0] * r3 - T[1] * r2;
          T += tstep; x1 -= 4; x2 -= 4; } while(T < T0 + 1024);
      do{ float r0 = x1[0] - x1[1], r1 = x1[2] - x1[3], r2 = x2[0] - x2[1], r3 = x2[3] - x2[2];   /* XNPROD31 x2 */
          x1[0] = x1[1] + x1[0]; x1[1] = x2[0] + x2[1]; x1[2] = x1[3] + x1[2]; x1[3] = x2[2] + x2[3];
          x2[0] = T[0] * r0 - T[1] * r1; x2[1] = T[0] * r3 - T[1] * r2; x2[2] = T[1] * r0 + T[0] * r1; x2[3] = T[1] * r3 + T[0] * r2;
          T -= tstep; x1 -= 4; x2 -= 4; } while(T > T0);
    }
  }
  for(uint32_t k = ((uint32_t)n2 - 1) >> 5, j = 0; j <= k; j++) imdct_b32(x + 12 + 32 * (size_t)j);
  /* bitreverse */
  { const uint8_t *br = (const uint8_t *)MEMP(0x134af00u); af32 *w = x + n2; uint32_t bit = 0;
    do{ uint32_t b = ((uint32_t)br[(bit >> 4) & 15] << 4 | (uint32_t)br[bit & 15] << 8 | br[bit >> 8]);
        af32 *xx = x + ((int32_t)b >> shift); bit++; w -= 2;
        if(w > xx){ float r = xx[0]; xx[0] = w[0]; w[0] = r; r = xx[1]; xx[1] = w[1]; w[1] = r; }
    } while(w > x); }
  /* step7 */
  { af32 *w0 = x, *w1 = x + n2; const af32 *T = step >= 4 ? T0 + (step >> 1) : T1, *Ttop = T + 1024; const float h = K_half;
    do{ w1 -= 2;
        float r0 = w0[0] + w1[0], r1 = w1[1] - w0[1];
        float r2 = (T[0] * r1 + T[1] * r0) * h, r3 = (T[1] * r1 - T[0] * r0) * h;
        float q0 = (w0[1] + w1[1]) * h, q1 = (w0[0] - w1[0]) * h;
        w0[0] = r2 + q0; w0[1] = r3 + q1; w1[0] = q0 - r2; w1[1] = r3 - q1;
        T += step; w0 += 2; } while(T < Ttop);
    do{ w1 -= 2; T -= step;
        float r0 = w0[0] + w1[0], r1 = w1[1] - w0[1];
        float r2 = (r1 * T[1] + T[0] * r0) * h, r3 = (T[0] * r1 - r0 * T[1]) * h;
        float q0 = (w0[1] + w1[1]) * h, q1 = (w0[0] - w1[0]) * h;
        w0[0] = r2 + q0; w0[1] = r3 + q1; w1[0] = q0 - r2; w1[1] = r3 - q1;
        w0 += 2; } while(w0 < w1); }
  /* step8 */
  { int st = step >> 2; af32 *p = x; af32 *iX = x + n2;
    if(st >= 2){
      const af32 *T = st >= 4 ? T0 + (st >> 1) : T1;
      do{ float r0 = p[0], r1 = -p[1]; p[0] = T[1] * r1 + T[0] * r0; p[1] = T[0] * r1 - T[1] * r0; p += 2; T += st; } while(p < iX);
    } else if(st == 1){
      const float h = K_half; const af32 *T = T0 + 2, *V = T1; float t0 = T0[0] * h, t1 = T0[1] * h;
      do{ float r0 = p[0], r1 = -p[1], v0 = V[0] * h, v1 = V[1] * h; V += 2;
          float u0 = v0 + t0, u1 = v1 + t1;
          p[0] = r1 * u1 + r0 * u0; p[1] = r1 * u0 - r0 * u1;
          r0 = p[2]; r1 = -p[3]; t1 = T[1] * h; t0 = T[0] * h; T += 2;
          float w0 = v0 + t0, w1 = v1 + t1;
          p[2] = w1 * r1 + w0 * r0; p[3] = w0 * r1 - w1 * r0;
          p += 4; } while(p < iX);
    } else {
      const float q = K_quarter; const af32 *T = T0 + 2, *V = T1; float t0 = T0[0], t1 = T0[1];
      do{ float v0 = V[0], v1 = V[1]; V += 2;
          float q0 = (v0 - t0) * q, q1 = (v1 - t1) * q, a0 = q0 + t0, a1 = q1 + t1;
          float r0 = p[0], r1 = -p[1];
          p[0] = r1 * a1 + r0 * a0; p[1] = r1 * a0 - r0 * a1;
          float b0 = v0 - q0, b1 = v1 - q1; r0 = p[2]; r1 = -p[3];
          p[3] = r1 * b0 - r0 * b1; p[2] = r1 * b1 + r0 * b0;
          t0 = T[0]; t1 = T[1]; T += 2;
          q0 = (t0 - v0) * q; q1 = (t1 - v1) * q; a0 = q0 + v0; a1 = q1 + v1;
          r0 = p[4]; r1 = -p[5];
          p[4] = r1 * a1 + r0 * a0;                    /* the x[5] result of this step is dead in the original */
          b0 = t0 - q0; b1 = t1 - q1; r0 = p[6]; r1 = -p[7];
          p[5] = r1 * b1 + r0 * b0; p[6] = r1 * b0 - r0 * b1;   /* Tremor's XPROD31(..., x+5, x+6): x[7] untouched */
          p += 8; } while(p < iX);
    }
  }
  c->esp += 4; return 1;
}

/* ---- lim 0xF660D0: thiscall(fx, AkAudioBuffer *buf), ret 4 (PeakLimiter with look-ahead) ----
   fx: +8 params {+4 threshold dB, +8 ratio}, +0x18 channel mask, +0x24 look-ahead frames, +0x28 state {gain dB,
   peak, hold}, +0x2C delay ring (frames x channels), +0x30 ring frame index, +0x3C/+0x40 smoothing coefficients,
   +0x44 first-call flag. buf: +0 data, +0xC u16 channel stride (frames), +0xE u16 valid frames. */
double fpu_round(CPU *c, double v);
static int nk_lim(CPU *c){
  uint32_t F = c->ecx, B = R32(c->esp + 4), P = R32(F + 8);
  uint32_t mask = R32(F + 0x18) & 0x3ffff, nch = 0; for(uint32_t m = mask; m; m &= m - 1) nch++;
  if(nch == 0 || nch > 6) return 0;                  /* 0: the original divides by it; >6: overruns its stack scratch */
  const float thr = RF32(P + 4), one = RF32(0x10fa160), sl = one / RF32(P + 8) - one;
  const float c40 = RF32(F + 0x40), c3c = RF32(F + 0x3c), floor_db = RF32(0x122c840);
  const uint32_t la = R32(F + 0x24), base = R32(F + 0x2c), S = R32(F + 0x28);
  uint32_t ptr = base + R32(F + 0x30) * nch * 4; const uint32_t end = base + la * nch * 4;
  const uint32_t n = R16(B + 0xe), stride = R16(B + 0xc), data = R32(B);
  float g = RF32(S), peak = RF32(S + 4); uint32_t hold = R32(S + 8);
  if(R8(F + 0x44)){
    uint32_t m = la < n ? la : n;
    for(uint32_t ch = 0; ch < nch; ch++){
      const af32 *xs = FP(data + ch * stride * 4);
      for(uint32_t k = 0; k < m; k++){ float a = fabsf(xs[k]); if(a > peak){ peak = a; hold = m - k; } }
    }
    W8(F + 0x44, 0);
  }
  float scratch[6];
  const double lg2 = 0.30102999566398119521, k20 = RF32(0x1110630), k97 = RF32(0x12a7764), khalf = RF32(0x12a8a60), k2p32 = RF32(0x12cd6c0);
  uint32_t i = 0;
  while(i < n){
    uint32_t room = (uint32_t)((int32_t)(end - ptr) >> 2) / nch, cnt = n - i;
    if(cnt >= room) cnt = room;
    while(cnt){
      cnt--;
      float fp = 0.0f;
      af32 *d = FP(ptr);
      for(uint32_t ch = 0; ch < nch; ch++){
        float x = RF32(data + (ch * stride + i) * 4);
        scratch[ch] = d[ch]; d[ch] = x;
        float a = fabsf(x); if(a > fp) fp = a;
      }
      ptr += nch * 4;
      if(hold == 0 || fp > peak){ hold = la; peak = fp; } else hold--;
      double v = lg2 * log2((double)peak); v = v * k20; v = v - (double)thr;
      float t = v > 0.0 ? (float)v : 0.0f;
      float coef = (t - g >= 0.0f) ? c40 : c3c;
      g = (g - t) * coef + t;
      float gdb = g * sl, lin;
      if(floor_db >= gdb) lin = 0.0f;
      else if(gdb >= 0.0f) lin = one;
      else {
        float tt = (float)((double)gdb + k97);
        double r = fpu_round(c, (double)tt - khalf);
        int32_t k = (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : (int32_t)0x80000000;
        double kd = (double)k; if(k < 0) kd = kd + k2p32;
        double fr = (double)tt - kd;
        double a = (1.0 - fr) * (double)RF32(0x12a88d0u + (uint32_t)k * 4);
        double b = fr * (double)RF32(0x12a88d4u + (uint32_t)k * 4);
        lin = (float)(b + a);
      }
      for(uint32_t ch = 0; ch < nch; ch++) WF32(data + (ch * stride + i) * 4, scratch[ch] * lin);
      i++;
    }
    if(ptr == end) ptr = base;
  }
  WF32(S, g); WF32(S + 4, peak); W32(S + 8, hold);
  W32(F + 0x30, (uint32_t)((int32_t)(ptr - base) >> 2) / nch);
  c->esp += 8; return 1;
}

/* ---- win 0xFC3910: cdecl(n0, n1, lW, W, float *in, float *right, float *w0, float *w1, int16 *out, step, start, end)
   Tremor mdct_unroll_lap, float version: ">>9" is "* 1/512" (0x111900C; postLap uses -1/512 at 0x12B2110), CLIP_TO_15
   is cvttss2si + clamp to [-32768, 32767]. */
static inline int16_t clip15(float v){ int32_t i = cvtt_f2i(v); return (int16_t)(i < -32768 ? -32768 : i > 32767 ? 32767 : i); }
static int nk_win(CPU *c){
  uint32_t sp = c->esp;
  int32_t n0 = (int32_t)R32(sp + 4), n1 = (int32_t)R32(sp + 8), lW = (int32_t)R32(sp + 12), W = (int32_t)R32(sp + 16);
  uint32_t gin = R32(sp + 20), gright = R32(sp + 24), gw0 = R32(sp + 28), gw1 = R32(sp + 32), gout = R32(sp + 36);
  int32_t step = (int32_t)R32(sp + 40), start = (int32_t)R32(sp + 44), end = (int32_t)R32(sp + 48);
  const float K = RF32(0x111900c), KN = RF32(0x12b2110);
  af32 *l = FP(gin) + ((W && lW ? n1 : n0) >> 1);
  af32 *r = FP(gright) + ((lW ? n1 : n0) >> 2);
  const af32 *wR = (W && lW) ? FP(gw1) + (n1 >> 1) : FP(gw0) + (n0 >> 1);
  const af32 *wL = (W && lW) ? FP(gw1) : FP(gw0);
  int32_t preLap = (lW && !W) ? (n1 >> 2) - (n0 >> 2) : 0, halfLap = (lW && W) ? (n1 >> 2) : (n0 >> 2);
  int32_t postLap = (!lW && W) ? (n1 >> 2) - (n0 >> 2) : 0, nn, off;
  au16 *out = (au16 *)MEMP(gout); const ptrdiff_t ostep = step;
  if(preLap){
    nn = end < preLap ? end : preLap; off = start < preLap ? start : preLap;
    af32 *post = r - nn; r -= off; start -= off; end -= nn;
    while(r > post){ *out = (uint16_t)clip15(*--r * K); out += ostep; }
  }
  nn = end < halfLap ? end : halfLap; off = start < halfLap ? start : halfLap;
  { af32 *post = r - nn; r -= off; l -= off * 2; start -= off; wR -= off; wL += off; end -= nn;
    while(r > post){ l -= 2; --wR; --r; float a = l[0] * wL[0]; float b = wR[0] * r[0]; wL++; *out = (uint16_t)clip15((a + b) * K); out += ostep; } }
  nn = end < halfLap ? end : halfLap; off = start < halfLap ? start : halfLap;
  { af32 *post = r + nn; r += off; l += off * 2; start -= off; end -= nn; wR -= off; wL += off;
    while(r < post){ float a = wR[-1] * r[0]; --wR; float b = l[0] * wL[0]; r++; wL++; *out = (uint16_t)clip15((a - b) * K); out += ostep; l += 2; } }
  if(postLap){
    nn = end < postLap ? end : postLap; off = start < postLap ? start : postLap;
    af32 *post = l + nn * 2; l += off * 2;
    while(l < post){ *out = (uint16_t)clip15(l[0] * KN); out += ostep; l += 2; }
  }
  c->esp += 4; return 1;
}

/* ---- book 0xFC6270 / bookv 0xFC6440: Vorbis codebook decode (integer: exact by construction) ----
   oggpack_buffer: +0 endbit (0..7), +4 byte ptr, +8 bytes left. codebook: +0 dim, +0xC dec_maxlength, +0x10 dec_table,
   +0x14 dec_nodeb (1/2/else 4), +0x18 dec_leafw, +0x1C dec_type, +0x20 q_min, +0x24 q_minp, +0x28 q_del, +0x2C q_delp,
   +0x30 q_bits, +0x34 q_pack, +0x38 q_val. Wwise's reader never fails (no EOP checks): reads past the end like the
   original. Shift counts are masked to 5 bits like x86. */
static inline uint32_t ak_look(uint32_t b, uint32_t bits){
  uint32_t m = R32(0x134af10u + bits * 4), eb = R32(b), p = R32(b + 4), tot = bits + eb;
  uint32_t v = (uint32_t)R8(p) >> (eb & 31);
  if((int32_t)tot > 8){ v |= (uint32_t)R8(p + 1) << ((8 - eb) & 31);
    if((int32_t)tot > 16){ v |= (uint32_t)R8(p + 2) << ((16 - eb) & 31);
      if((int32_t)tot > 24){ v |= (uint32_t)R8(p + 3) << ((24 - eb) & 31);
        if((int32_t)tot > 32 && eb) v |= (uint32_t)R8(p + 4) << ((32 - eb) & 31); } } }
  return v & m;
}
static inline void ak_adv(uint32_t b, uint32_t bits){
  int32_t t = (int32_t)(bits + R32(b)); W32(b + 8, R32(b + 8) - (uint32_t)(t >> 3)); W32(b + 4, R32(b + 4) + (uint32_t)(t >> 3)); W32(b, (uint32_t)t & 7);
}
static uint32_t ak_decode(uint32_t book, uint32_t b){
  int32_t read = (int32_t)R32(book + 0xc), i = 0;
  uint32_t lok = ak_look(b, (uint32_t)read), bit = lok & 1, chase = 0, t = R32(book + 0x10), nodeb = R32(book + 0x14), leafw = R32(book + 0x18);
#define NEXTBIT() (lok = (uint32_t)((int32_t)lok >> 1), bit = lok & 1)
  if(nodeb == 1){
    if(leafw == 1){
      for(; i < read; i++){ chase = R8(t + chase * 2 + bit); if(chase & 0x80) break; NEXTBIT(); }
      chase &= 0x7f;
    } else {
      for(; i < read; i++){ uint32_t nx = R8(t + bit + chase);
        if(nx & 0x80){ uint32_t off = (!bit || (R8(t + chase) & 0x80)) ? 1 : 0; chase = nx << 8 | R8(t + off + bit + chase + 1); break; }
        NEXTBIT(); chase = nx; }
      chase &= 0x7fff;
    }
  } else if(nodeb == 2){
    if(leafw == 1){
      for(; i < read; i++){ chase = R16(t + (bit + chase * 2) * 2); if(chase & 0x8000) break; NEXTBIT(); }
      chase &= 0x7fff;
    } else {
      for(; i < read; i++){ uint32_t nx = R16(t + (bit + chase) * 2);
        if(nx & 0x8000){ uint32_t off = (!bit || (R16(t + chase * 2) & 0x8000)) ? 1 : 0; chase = nx << 16 | R16(t + (off + bit + chase) * 2 + 2); break; }
        NEXTBIT(); chase = nx; }
      chase &= 0x7fffffff;
    }
  } else {
    for(; i < read; i++){ chase = R32(t + (bit + chase * 2) * 4); if((int32_t)chase < 0) break; NEXTBIT(); }
    chase &= 0x7fffffff;
  }
#undef NEXTBIT
  ak_adv(b, (uint32_t)i + 1);
  return chase;
}
static int nk_book(CPU *c){ c->eax = ak_decode(c->eax, R32(c->esp + 4)); c->esp += 4; return 1; }
static int nk_bookv(CPU *c){
  uint32_t book = R32(c->esp + 4), b = R32(c->esp + 8), v = R32(c->esp + 12); int32_t point = (int32_t)R32(c->esp + 16);
  uint32_t e = ak_decode(book, b); int32_t dim = (int32_t)R32(book), i = 0;
  if(R32(book + 0x1c) == 1){
    uint32_t qb = R32(book + 0x30) & 31, mask = (1u << qb) - 1;
    do{ W32(v + 4 * i, e & mask); e >>= qb; } while(++i < dim);
  } else {
    uint32_t qp = R32(book + 0x34) & 31, qv = R32(book + 0x38), mask = (1u << qp) - 1;
    if((int32_t)R32(book + 0x30) > 8) do{ W32(v + 4 * i, R16(qv + (e & mask) * 2)); e >>= qp; } while(++i < dim);
    else do{ W32(v + 4 * i, R8(qv + (e & mask))); e >>= qp; } while(++i < dim);
  }
  int32_t add = (int32_t)R32(book + 0x20), sa = point - (int32_t)R32(book + 0x24), sm = point - (int32_t)R32(book + 0x2c);
  uint32_t qd = R32(book + 0x28);
  if(sa > 0) add >>= (sa & 31); else add = (int32_t)((uint32_t)add << ((-sa) & 31));
  i = 0;
  if(sm > 0) do{ W32(v + 4 * i, (uint32_t)(((int32_t)(R32(v + 4 * i) * qd) >> (sm & 31)) + add)); } while(++i < dim);
  else do{ W32(v + 4 * i, ((R32(v + 4 * i) * qd) << ((-sm) & 31)) + (uint32_t)add); } while(++i < dim);
  c->eax = (uint32_t)i; c->esp += 4; return 1;
}

/* ---- lpf 0xFB16C0: y = c0*(x + x[-2]) + c1*x[-1] + c3*y[-2] + c2*y[-1], in place; state {x1, x2, y1, y2} ---- */
static int nk_lpf(CPU *c){
  uint32_t C = c->eax, S = c->esi, pp = R32(c->esp + 4), n = R32(c->esp + 8);
  const float c0 = RF32(C), c1 = RF32(C + 4), c2 = RF32(C + 8), c3 = RF32(C + 12);
  float s0 = RF32(S), s1 = RF32(S + 4), s2 = RF32(S + 8), s3 = RF32(S + 12);
  uint32_t p = R32(pp), end = p + n * 4;
#define LPF1(q) { float x = *(af32 *)MEMP(q); float t = (x + s1) * c0 + c1 * s0; float y = (c3 * s3 + t) + c2 * s2; \
                  s1 = s0; s0 = x; s3 = s2; s2 = y; *(af32 *)MEMP(q) = y; }
  if(p < end){
    int32_t cnt = (int32_t)(end - p + 3) / 4;
    if(cnt >= 4){ uint32_t lim = end - 12; do{ LPF1(p) LPF1(p + 4) LPF1(p + 8) LPF1(p + 12) p += 16; } while((int32_t)p < (int32_t)lim); }
    while(p < end){ LPF1(p) p += 4; }
  }
#undef LPF1
  WF32(S + 4, s1); W32(pp, p); WF32(S, s0); WF32(S + 8, s2); WF32(S + 12, s3);
  c->esp += 4; return 1;
}

/* ---- mix 0xFADC40: thiscall(hdr {u16 frames}, const float *src, float *dst, float gain, float delta), ret 0x10 ----
   writes its gain vector to the static scratch at 0x14132E0 like the original; runs at least one 8-sample step. */
static int nk_mix(CPU *c){
  uint32_t sp = c->esp, src = R32(sp + 4), dst = R32(sp + 8), end = src + (uint32_t)R16(c->ecx) * 4;
  float vol = RF32(sp + 12), d = RF32(sp + 16);
  v4f A, B, D8;
  if(d == 0.0f){
    if(vol == 0.0f){ c->esp += 20; return 1; }
    A = (v4f){ vol, vol, vol, vol }; B = A; D8 = (v4f){ 0, 0, 0, 0 };
    STV(FP(0x14132e0u), A);
    do{ v4f d0 = LDV(FP(dst)), s0 = LDV(FP(src)), d1 = LDV(FP(dst + 16)), s1 = LDV(FP(src + 16));
        STV(FP(dst), s0 * A + d0); STV(FP(dst + 16), s1 * A + d1); src += 32; dst += 32; } while(src < end);
  } else {
    float k4 = d * RF32(0x110ff38u);
    A = (v4f){ vol, vol + d, d * RF32(0x10fab74u) + vol, d * RF32(0x12cd7c0u) + vol };
    STV(FP(0x14132e0u), A);
    v4f K4 = { k4, k4, k4, k4 }; B = A + K4; D8 = K4 + K4;
    do{ v4f d0 = LDV(FP(dst)), s0 = LDV(FP(src)), d1 = LDV(FP(dst + 16)), s1 = LDV(FP(src + 16));
        STV(FP(dst), s0 * A + d0); STV(FP(dst + 16), s1 * B + d1); src += 32; dst += 32;
        v4f nb = B + D8; A = A + D8; B = nb; } while(src < end);
  }
  (void)D8; c->esp += 20; return 1;
}

/* ---- peq 0xF64F30: per channel y = c3*y1 + (c4*y2 + (c1*x1 + (c2*x2 + c0*x))), state {x1, x2, y1, y2} per channel.
   Callers keep a value in x87 st0 across this call (LTCG): natives never touch c->st / c->top. ---- */
static int nk_peq(CPU *c){
  uint32_t C = c->eax, B = R32(c->esp + 4), St = R32(c->esp + 8), nch = R32(c->esp + 12);
  const float c0 = RF32(C), c1 = RF32(C + 4), c2 = RF32(C + 8), c3 = RF32(C + 12), c4 = RF32(C + 16);
  for(uint32_t ch = 0; ch < nch; ch++){
    uint32_t S = St + 16 * ch, p = R32(B) + (uint32_t)R16(B + 0xc) * ch * 4, end = p + (uint32_t)R16(B + 0xe) * 4;
    float x1 = RF32(S), x2 = RF32(S + 4), y1 = RF32(S + 8), y2 = RF32(S + 12);
#define PEQ1(q) { float x = *(af32 *)MEMP(q); float y = c3 * y1 + (c4 * y2 + (c1 * x1 + (c2 * x2 + c0 * x))); \
                  x2 = x1; x1 = x; y2 = y1; y1 = y; *(af32 *)MEMP(q) = y; }
    if(p < end){
      if((int32_t)(end - p + 3) / 4 >= 4){ uint32_t lim = end - 12; do{ PEQ1(p) PEQ1(p + 4) PEQ1(p + 8) PEQ1(p + 12) p += 16; } while((int32_t)p < (int32_t)lim); }
      while(p < end){ PEQ1(p) p += 4; }
    }
#undef PEQ1
    WF32(S, x1); WF32(S + 4, x2); WF32(S + 8, y1); WF32(S + 12, y2);
  }
  c->esp += 4; return 1;
}

/* ---- bq 0xF4A3D0: thiscall(filter {c0..c4, x1, x2, y1, y2}, float *buf, uint n), ret 8;
   y = c3*y1 + ((c1*x1 + (c0*x + c2*x2)) + c4*y2) ---- */
static int nk_bq(CPU *c){
  uint32_t F = c->ecx, p = R32(c->esp + 4), end = p + R32(c->esp + 8) * 4;
  const float c0 = RF32(F), c1 = RF32(F + 4), c2 = RF32(F + 8), c3 = RF32(F + 12), c4 = RF32(F + 16);
  float x1 = RF32(F + 0x14), x2 = RF32(F + 0x18), y1 = RF32(F + 0x1c), y2 = RF32(F + 0x20);
#define BQ1(q) { float x = *(af32 *)MEMP(q); float y = c3 * y1 + ((c1 * x1 + (c0 * x + c2 * x2)) + c4 * y2); \
                 x2 = x1; x1 = x; y2 = y1; y1 = y; *(af32 *)MEMP(q) = y; }
  if(p < end){
    if((int32_t)(end - p + 3) / 4 >= 4){ uint32_t lim = end - 12; do{ BQ1(p) BQ1(p + 4) BQ1(p + 8) BQ1(p + 12) p += 16; } while((int32_t)p < (int32_t)lim); }
    while(p < end){ BQ1(p) p += 4; }
  }
#undef BQ1
  WF32(F + 0x14, x1); WF32(F + 0x18, x2); WF32(F + 0x1c, y1); WF32(F + 0x20, y2);
  c->esp += 12; return 1;
}

/* ================= dispatch, differential check, stats ================= */
extern void f_00f6c970_x86(CPU *) __attribute__((weak));
extern void f_00f6bd60_x86(CPU *) __attribute__((weak));
extern void f_00f6d060_x86(CPU *) __attribute__((weak));
extern void f_00f6c240_x86(CPU *) __attribute__((weak));
extern void f_00fc2d10_x86(CPU *) __attribute__((weak));
extern void f_00f660d0_x86(CPU *) __attribute__((weak));
extern void f_00fc3910_x86(CPU *) __attribute__((weak));
extern void f_00fc6270_x86(CPU *) __attribute__((weak));
extern void f_00fc6440_x86(CPU *) __attribute__((weak));
extern void f_00fb16c0_x86(CPU *) __attribute__((weak));
extern void f_00fadc40_x86(CPU *) __attribute__((weak));
extern void f_00f64f30_x86(CPU *) __attribute__((weak));
extern void f_00f4a3d0_x86(CPU *) __attribute__((weak));
static gfn const ak_x86[AK_N] = { f_00f6c970_x86, f_00f6bd60_x86, f_00f6d060_x86, f_00f6c240_x86, f_00fc2d10_x86, f_00f660d0_x86, f_00fc3910_x86, f_00fc6270_x86, f_00fc6440_x86, f_00fb16c0_x86, f_00fadc40_x86, f_00f64f30_x86, f_00f4a3d0_x86 };
static int (*const ak_nat[AK_N])(CPU *) = { nk_fdn, nk_dly, nk_ap, nk_er, nk_imdct, nk_lim, nk_win, nk_book, nk_bookv, nk_lpf, nk_mix, nk_peq, nk_bq };

typedef struct { uint32_t a, n; uint8_t *pre, *ref; int nocmp; } AReg;
#define AMAXR 12
typedef struct { AReg r[AMAXR]; int nr; } ARegs;
static void ar_add(ARegs *g, uint32_t a, uint32_t n){
  if(g->nr < AMAXR && n && n <= (16u << 20) && (uint64_t)a + n <= 0x100000000ull){
    AReg *r = &g->r[g->nr++]; r->a = a; r->n = n; r->nocmp = 0; r->pre = malloc(n); r->ref = malloc(n); memcpy(r->pre, MEMP(a), n); } }
static void ar_free(ARegs *g){ for(int i = 0; i < g->nr; i++){ free(g->r[i].pre); free(g->r[i].ref); } g->nr = 0; }
/* every guest byte the original may write and that outlives the call (from the current arguments) */
static const uint8_t ak_argbytes[AK_N] = { 12, 8, 8, 16, 8, 4, 48, 4, 16, 8, 16, 12, 8 };
static void ak_regions(int k, CPU *c, ARegs *g){
  uint32_t sp = c->esp, o = c->ecx; g->nr = 0;
  /* the originals reuse their argument slots as scratch (e.g. ap/dly/fdn/er/win): restore them before the native
     runs, but do not compare them (callee-popped or discarded by the caller right after the call) */
  ar_add(g, sp, 4u + ak_argbytes[k]); g->r[0].nocmp = 1;
  switch(k){
  case AK_FDN: ar_add(g, o, 0x60); ar_add(g, R32(sp + 8), R32(sp + 12) * 4);
    for(int j = 0; j < 4; j++) ar_add(g, R32(o + 12 * j + 4), R32(o + 12 * j) * 4); break;
  case AK_DLY: ar_add(g, o, 12); ar_add(g, R32(sp + 4), R32(sp + 8) * 4); ar_add(g, R32(o + 4), R32(o) * 4); break;
  case AK_AP: ar_add(g, o, 16); ar_add(g, R32(sp + 4), R32(sp + 8) * 4); ar_add(g, R32(o + 4), R32(o) * 8); break;
  case AK_ER: ar_add(g, o, 0x314); ar_add(g, R32(o + 4), R32(o) * 4 + 16); ar_add(g, R32(sp + 8), R32(sp + 16) * 4); ar_add(g, R32(sp + 12), R32(sp + 16) * 4); break;
  case AK_IMDCT: ar_add(g, R32(sp + 8), R32(sp + 4) * 4); break;
  case AK_LIM: { uint32_t B = R32(sp + 4), nch = 0; for(uint32_t m = R32(o + 0x18) & 0x3ffff; m; m &= m - 1) nch++;
    ar_add(g, o, 0x48); ar_add(g, R32(o + 0x28), 12); ar_add(g, R32(o + 0x2c), R32(o + 0x24) * nch * 4);
    ar_add(g, R32(B), (uint32_t)R16(B + 0xc) * nch * 4); break; }
  case AK_BOOK: ar_add(g, R32(sp + 4), 12); break;
  case AK_BOOKV: { int32_t d = (int32_t)R32(R32(sp + 4)); ar_add(g, R32(sp + 8), 12); ar_add(g, R32(sp + 12), d > 0 ? (uint32_t)d * 4 : 4); break; }
  case AK_LPF: ar_add(g, c->esi, 16); ar_add(g, R32(sp + 4), 4); ar_add(g, R32(R32(sp + 4)), R32(sp + 8) * 4); break;
  case AK_MIX: { uint32_t f = R16(o); ar_add(g, 0x14132e0u, 16); ar_add(g, R32(sp + 8), (f ? (f + 7) & ~7u : 8) * 4); break; }
  case AK_PEQ: { uint32_t B = R32(sp + 4), n = R32(sp + 12); ar_add(g, R32(sp + 8), n * 16); ar_add(g, R32(B), (uint32_t)R16(B + 0xc) * n * 4 + 4); break; }
  case AK_BQ: ar_add(g, o + 0x14, 16); ar_add(g, R32(sp + 4), R32(sp + 8) * 4); break;
  case AK_WIN: { int32_t e = (int32_t)R32(sp + 48), s = (int32_t)R32(sp + 40); if(e > 0) ar_add(g, R32(sp + 36), (uint32_t)(e * (s < 0 ? -s : s) * 2 + 2)); break; }
  }
}
static uint64_t st_calls[AK_N], st_nat[AK_N], st_ns[AK_N], ck_n[AK_N], ck_bad[AK_N];
static inline uint64_t ak_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
/* run the original and the native on the same state; leaves the original's result. 1 = identical */
static int ak_diff_once(int k, CPU *c){
  ARegs g; ak_regions(k, c, &g); CPU c0 = *c, cx;
  ak_x86[k](c); cx = *c;
  for(int i = 0; i < g.nr; i++){ memcpy(g.r[i].ref, MEMP(g.r[i].a), g.r[i].n); memcpy(MEMP(g.r[i].a), g.r[i].pre, g.r[i].n); }
  *c = c0; int ok = 1; char why[160] = "";
  if(!ak_nat[k](c)){ *c = cx; for(int i = 0; i < g.nr; i++) memcpy(MEMP(g.r[i].a), g.r[i].ref, g.r[i].n); ar_free(&g); return 1; }
  if(c->esp != cx.esp || c->ebx != cx.ebx || c->esi != cx.esi || c->edi != cx.edi || c->ebp != cx.ebp){ ok = 0; snprintf(why, sizeof why, "registers (esp %x/%x)", cx.esp, c->esp); }
  for(int i = 0; ok && i < g.nr; i++) if(!g.r[i].nocmp && memcmp(MEMP(g.r[i].a), g.r[i].ref, g.r[i].n)){
    uint32_t j = 0; while(((uint8_t *)MEMP(g.r[i].a))[j] == g.r[i].ref[j]) j++;
    ok = 0; snprintf(why, sizeof why, "region %d (%#x+%u) byte %u: x86 %02x native %02x", i, g.r[i].a, g.r[i].n, j, g.r[i].ref[j], ((uint8_t *)MEMP(g.r[i].a))[j]); }
  if(!ok && __atomic_fetch_add(&ck_bad[k], 1, __ATOMIC_RELAXED) < 20) fprintf(stderr, "[aknat] MISMATCH %s: %s\n", ak_name[k], why);
  *c = cx; for(int i = 0; i < g.nr; i++) memcpy(MEMP(g.r[i].a), g.r[i].ref, g.r[i].n);   /* keep the original's result */
  ar_free(&g); return ok;
}
static void ak_report(uint64_t t);
static uint32_t ak_once;
static void ak_consts(void){ k_fdn_h = RF32(0x12cd910); imdct_consts(); }
/* entry from the part wrappers: 1 = handled (natively, or the original already ran under the check) */
int aud_native_try(int k, CPU *c){
  int m = ak_mask();
  if(!((m >> k) & 1) || !ak_x86[k]) return 0;
  if(__builtin_expect(!__atomic_load_n(&ak_once, __ATOMIC_ACQUIRE), 0)){ ak_consts(); __atomic_store_n(&ak_once, 1, __ATOMIC_RELEASE); }
  uint64_t t0 = g_stat ? ak_now() : 0;
  uint64_t nc = __atomic_add_fetch(&st_calls[k], 1, __ATOMIC_RELAXED);
  int r;
  if(g_check && nc % (uint64_t)g_check == 0){ __atomic_add_fetch(&ck_n[k], 1, __ATOMIC_RELAXED); ak_diff_once(k, c); r = 1; }
  else { r = ak_nat[k](c); if(r) __atomic_add_fetch(&st_nat[k], 1, __ATOMIC_RELAXED); }
  if(g_stat){ uint64_t t1 = ak_now(); __atomic_add_fetch(&st_ns[k], t1 - t0, __ATOMIC_RELAXED); ak_report(t1); }
  return r;
}
static void ak_report(uint64_t t){
  static uint64_t last; static int lock;
  if(t - last < 10000000000ull || __atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) return;
  if(t - last >= 10000000000ull){
    last = t;
    fprintf(stderr, "[aknat] voices %u (virtual %u)", R32(0x1412878u), R32(0x141286cu));
    for(int k = 0; k < AK_N; k++) if(st_calls[k])
      fprintf(stderr, " | %s %llu/%llu %.2f ms%s", ak_name[k], (unsigned long long)st_nat[k], (unsigned long long)st_calls[k], st_ns[k] / 1e6,
              ck_n[k] ? (ck_bad[k] ? " CHECK-BAD" : " check-ok") : "");
    fprintf(stderr, "\n");
  }
  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

/* ================= Wwise settings (L1-L3 of notes/HOTCODE.md) and audio-thread hooks ================= */
extern void f_00f71070(CPU *);   /* AK::SoundEngine::SetMaxNumVoicesLimit(AkUInt16), cdecl */
extern void f_00f71050(CPU *);   /* AK::SoundEngine::SetVolumeThreshold(AkReal32 dB), cdecl */
static void ak_gcall1(CPU *c, gfn f, uint32_t arg){
  CPU s = *c; PUSH32(c, arg); PUSH32(c, 0xdead0000u); f(c);
  uint32_t r = c->eax; *c = s; (void)r; }
/* before AK::SoundEngine::Init(AkInitSettings*, AkPlatformInitSettings*) 0xF73330 (cdecl) */
void aud_hook_init_pre(CPU *c){
  ak_mask();
  uint32_t ps = R32(c->esp + 8); if(!ps) return;
  if(s_lowq){ fprintf(stderr, "[aknat] eAudioQuality %u -> 1 (24 kHz)\n", R32(ps + 0x28)); W32(ps + 0x28, 1); }
  if(s_refills){ fprintf(stderr, "[aknat] uNumRefillsInVoice %u -> %d\n", R16(ps + 0x24), s_refills); W16(ps + 0x24, (uint16_t)s_refills); }
}
/* after Init returned (eax = AKRESULT, 1 = success) */
void aud_hook_init_post(CPU *c){
  ak_mask();
  if(c->eax != 1) return;
  if(s_voices > 0){ ak_gcall1(c, f_00f71070, (uint32_t)s_voices); fprintf(stderr, "[aknat] SetMaxNumVoicesLimit(%d) -> limit %u\n", s_voices, R16(0x134200cu)); }
  if(s_has_volthr){ uint32_t b; memcpy(&b, &s_volthr, 4); ak_gcall1(c, f_00f71050, b); fprintf(stderr, "[aknat] SetVolumeThreshold(%.1f dB)\n", s_volthr); }
}
/* before CAkRoomVerbFX::Init 0xF6A4D0 (thiscall, params at [esp+0xC]) */
void aud_hook_rv_init_pre(CPU *c){
  ak_mask();
  uint32_t p = R32(c->esp + 0xc); if(!p) return;
  if(s_units && R32(p + 0x70) > (uint32_t)s_units) W32(p + 0x70, (uint32_t)s_units);
  if(s_er == 0) W8(p + 0x95, 0);
}
/* CAkLEngine::Perform 0xF73780 entry (audio thread, once per buffer) */
void aud_hook_perform(CPU *c){
  (void)c;
  static __thread int named;
  if(__builtin_expect(!named, 0)){ named = 1; ak_mask(); if(s_tname) pthread_setname_np(pthread_self(), "AkAudioThread"); }
  if(g_stat) ak_report(ak_now());
}
