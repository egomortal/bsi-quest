// BSI-Quest recompiler runtime: guest CPU state + memory access
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
/* SIMD views of an XMM register (GCC/clang vector extensions), used by the vector-typed SSE translation (x86c.py
   BSI_PROMO_XMM=1 / BSI_XMM_VEC=1): whole-register operations on these members let the C compiler keep a promoted register
   (local `XMM x_k`) in one SIMD register instead of scalarising the union. aligned(8) keeps the union at its previous
   8-byte alignment, so the CPU struct layout (xmm at offset 40, sizeof 248 on LP64) is unchanged; accesses through the
   struct simply become unaligned vector loads/stores. The old scalar members keep working unchanged. */
typedef float    v4f  __attribute__((vector_size(16), aligned(8)));
typedef double   v2d  __attribute__((vector_size(16), aligned(8)));
typedef uint32_t v4u  __attribute__((vector_size(16), aligned(8)));
typedef int32_t  v4i  __attribute__((vector_size(16), aligned(8)));
typedef uint64_t v2u  __attribute__((vector_size(16), aligned(8)));
typedef int64_t  v2i  __attribute__((vector_size(16), aligned(8)));
typedef uint16_t v8u  __attribute__((vector_size(16), aligned(8)));
typedef int16_t  v8i  __attribute__((vector_size(16), aligned(8)));
typedef uint8_t  v16u __attribute__((vector_size(16), aligned(8)));
typedef int8_t   v16i __attribute__((vector_size(16), aligned(8)));
typedef union { float f[4]; double d[2]; uint32_t u32[4]; int32_t i32[4]; uint64_t u64[2]; uint16_t u16[8]; uint8_t u8[16];
                v4f vf; v2d vd; v4u vu; v4i vi; v2u vq; v2i vsq; v8u vw; v8i vsw; v16u vb; v16i vsb; } XMM;
_Static_assert(sizeof(XMM) == 16, "XMM union must stay 16 bytes");
/* lane shuffles: __builtin_shufflevector (clang, gcc >= 12) or gcc's older __builtin_shuffle with a mask vector */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
#define XSHUF2(a,b,i0,i1) __builtin_shufflevector((a),(b),i0,i1)
#define XSHUF4(a,b,i0,i1,i2,i3) __builtin_shufflevector((a),(b),i0,i1,i2,i3)
#define XSHUF8(a,b,i0,i1,i2,i3,i4,i5,i6,i7) __builtin_shufflevector((a),(b),i0,i1,i2,i3,i4,i5,i6,i7)
#define XSHUF16(a,b,i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15) __builtin_shufflevector((a),(b),i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15)
#else
#define XSHUF2(a,b,i0,i1) __builtin_shuffle((a),(b),(v2u){i0,i1})
#define XSHUF4(a,b,i0,i1,i2,i3) __builtin_shuffle((a),(b),(v4u){i0,i1,i2,i3})
#define XSHUF8(a,b,i0,i1,i2,i3,i4,i5,i6,i7) __builtin_shuffle((a),(b),(v8u){i0,i1,i2,i3,i4,i5,i6,i7})
#define XSHUF16(a,b,i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15) __builtin_shuffle((a),(b),(v16u){i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15})
#endif
/* min/max with the SSE operand rules (NaN or equal/±0 -> second operand): clang recognises the select as minps/maxps on
   x86 and fcmgt+bif on AArch64; unsigned byte / signed word forms for pminub/pmaxub/pminsw/pmaxsw */
static inline v4f xmm_minps(v4f a, v4f b){ v4i m=(v4i)(a<b); return (v4f)((m&(v4i)a)|(~m&(v4i)b)); }
static inline v4f xmm_maxps(v4f a, v4f b){ v4i m=(v4i)(a>b); return (v4f)((m&(v4i)a)|(~m&(v4i)b)); }
static inline v2d xmm_minpd(v2d a, v2d b){ v2i m=(v2i)(a<b); return (v2d)((m&(v2i)a)|(~m&(v2i)b)); }
static inline v2d xmm_maxpd(v2d a, v2d b){ v2i m=(v2i)(a>b); return (v2d)((m&(v2i)a)|(~m&(v2i)b)); }
static inline v16u xmm_pminub(v16u a, v16u b){ v16u m=(v16u)(a<b); return (m&a)|(~m&b); }
static inline v16u xmm_pmaxub(v16u a, v16u b){ v16u m=(v16u)(a>b); return (m&a)|(~m&b); }
static inline v8i xmm_pminsw(v8i a, v8i b){ v8i m=(v8i)(a<b); return (m&a)|(~m&b); }
static inline v8i xmm_pmaxsw(v8i a, v8i b){ v8i m=(v8i)(a>b); return (m&a)|(~m&b); }
/* On x86 hosts (Linux x86-64 gcc, the i686 mingw build) the few operations whose generic vector form gcc lowers poorly
   use the SSE2 builtins of the host, which have exactly the guest semantics. Everything else, and everything on AArch64,
   is plain vector-extension C that clang turns into single NEON instructions. */
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SSE2__) && !defined(XMM_NO_X86_BUILTINS)   /* -DXMM_NO_X86_BUILTINS: test the generic forms on x86 */
#define XMM_X86 1
typedef float     xmm_v4sf  __attribute__((vector_size(16)));
typedef double    xmm_v2df  __attribute__((vector_size(16)));
typedef short     xmm_v8hi  __attribute__((vector_size(16)));
typedef char      xmm_v16qi __attribute__((vector_size(16)));
#endif
#ifdef __has_builtin
#if __has_builtin(__builtin_elementwise_sqrt)
#define XMM_ELT_SQRT 1
#endif
#if __has_builtin(__builtin_elementwise_nearbyint)
#define XMM_ELT_NEARBYINT 1
#endif
#if __has_builtin(__builtin_elementwise_add_sat) && __has_builtin(__builtin_elementwise_sub_sat)
#define XMM_ELT_SAT 1
#endif
#endif
/* sqrt per lane (IEEE-exact on every path; the builtins avoid the errno path of sqrtf) */
#if defined(XMM_X86)
static inline v4f xmm_sqrtps(v4f a){ return (v4f)__builtin_ia32_sqrtps((xmm_v4sf)a); }
static inline v2d xmm_sqrtpd(v2d a){ return (v2d)__builtin_ia32_sqrtpd((xmm_v2df)a); }
#elif defined(XMM_ELT_SQRT)
static inline v4f xmm_sqrtps(v4f a){ return __builtin_elementwise_sqrt(a); }
static inline v2d xmm_sqrtpd(v2d a){ return __builtin_elementwise_sqrt(a); }
#else
static inline v4f xmm_sqrtps(v4f a){ return (v4f){sqrtf(a[0]),sqrtf(a[1]),sqrtf(a[2]),sqrtf(a[3])}; }
static inline v2d xmm_sqrtpd(v2d a){ return (v2d){sqrt(a[0]),sqrt(a[1])}; }
#endif
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
#define XMM_CVTDQ2PS(v) __builtin_convertvector((v), v4f)
#else
#define XMM_CVTDQ2PS(v) ((v4f){(float)(v)[0],(float)(v)[1],(float)(v)[2],(float)(v)[3]})
#endif
/* cvttps2dq / cvtps2dq: NaN and out-of-range lanes give the x86 "integer indefinite" 0x80000000. Generic form: those lanes
   are zeroed before the conversion (which is therefore always defined, no reliance on the host's saturation) and replaced
   afterwards; cvtps2dq rounds in the current rounding mode like cvt_f2i (nearbyint). */
#define XMM_INDEF4 ((v4i){(int32_t)0x80000000,(int32_t)0x80000000,(int32_t)0x80000000,(int32_t)0x80000000})
#if defined(XMM_X86)
static inline v4i xmm_cvttps2dq(v4f a){ return (v4i)__builtin_ia32_cvttps2dq((xmm_v4sf)a); }
static inline v4i xmm_cvtps2dq(v4f a){ return (v4i)__builtin_ia32_cvtps2dq((xmm_v4sf)a); }
#elif defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
static inline v4i xmm_cvt_ok4(v4f a){ return (v4i)((a>=-2147483648.0f)&(a<2147483648.0f)); }
static inline v4i xmm_cvttps2dq(v4f a){ v4i ok=xmm_cvt_ok4(a); v4i r=__builtin_convertvector((v4f)(ok&(v4i)a),v4i); return (ok&r)|(~ok&XMM_INDEF4); }
static inline v4i xmm_cvtps2dq(v4f a){ v4i ok=xmm_cvt_ok4(a); v4f c=(v4f)(ok&(v4i)a);
#ifdef XMM_ELT_NEARBYINT
  c=__builtin_elementwise_nearbyint(c);
#else
  c=(v4f){nearbyintf(c[0]),nearbyintf(c[1]),nearbyintf(c[2]),nearbyintf(c[3])};
#endif
  { v4i r=__builtin_convertvector(c,v4i); return (ok&r)|(~ok&XMM_INDEF4); } }
#else
static inline int32_t xmm_cvtt1(float v){ return (v>=-2147483648.0f&&v<2147483648.0f)?(int32_t)v:(int32_t)0x80000000; }
static inline int32_t xmm_cvt1(float v){ return (v>=-2147483648.0f&&v<2147483648.0f)?(int32_t)nearbyintf(v):(int32_t)0x80000000; }
static inline v4i xmm_cvttps2dq(v4f a){ return (v4i){xmm_cvtt1(a[0]),xmm_cvtt1(a[1]),xmm_cvtt1(a[2]),xmm_cvtt1(a[3])}; }
static inline v4i xmm_cvtps2dq(v4f a){ return (v4i){xmm_cvt1(a[0]),xmm_cvt1(a[1]),xmm_cvt1(a[2]),xmm_cvt1(a[3])}; }
#endif
/* pmaddwd: even/odd 16-bit lanes sign-extended to 32 bits, multiplied and added (the add wraps: (-32768)^2*2 = 0x80000000) */
#if defined(XMM_X86)
static inline v4i xmm_pmaddwd(v4i d, v4i s){ return (v4i)__builtin_ia32_pmaddwd128((xmm_v8hi)d,(xmm_v8hi)s); }
#else
static inline v4i xmm_pmaddwd(v4i d, v4i s){ v4i al=((v4i)((v4u)d<<16))>>16,ah=d>>16,bl=((v4i)((v4u)s<<16))>>16,bh=s>>16; return (v4i)((v4u)(al*bl)+(v4u)(ah*bh)); }
#endif
/* saturating packed add/sub (padds/paddus/psubs/psubus b/w): clang's elementwise builtins (sqadd/uqadd... on AArch64),
   the SSE2 builtins on gcc/x86, or the generic form: unsigned overflow iff r < a (add) / a < b (sub); signed overflow iff
   the operands agree in sign (add) / differ (sub) and the result's sign differs from a's, saturating towards a's sign */
#if defined(XMM_ELT_SAT)
static inline v16i xmm_paddsb(v16i a, v16i b){ return __builtin_elementwise_add_sat(a,b); }
static inline v8i  xmm_paddsw(v8i a, v8i b){ return __builtin_elementwise_add_sat(a,b); }
static inline v16i xmm_psubsb(v16i a, v16i b){ return __builtin_elementwise_sub_sat(a,b); }
static inline v8i  xmm_psubsw(v8i a, v8i b){ return __builtin_elementwise_sub_sat(a,b); }
static inline v16u xmm_paddusb(v16u a, v16u b){ return __builtin_elementwise_add_sat(a,b); }
static inline v8u  xmm_paddusw(v8u a, v8u b){ return __builtin_elementwise_add_sat(a,b); }
static inline v16u xmm_psubusb(v16u a, v16u b){ return __builtin_elementwise_sub_sat(a,b); }
static inline v8u  xmm_psubusw(v8u a, v8u b){ return __builtin_elementwise_sub_sat(a,b); }
#elif defined(XMM_X86) && !defined(__clang__)
static inline v16i xmm_paddsb(v16i a, v16i b){ return (v16i)__builtin_ia32_paddsb128((xmm_v16qi)a,(xmm_v16qi)b); }
static inline v8i  xmm_paddsw(v8i a, v8i b){ return (v8i)__builtin_ia32_paddsw128((xmm_v8hi)a,(xmm_v8hi)b); }
static inline v16i xmm_psubsb(v16i a, v16i b){ return (v16i)__builtin_ia32_psubsb128((xmm_v16qi)a,(xmm_v16qi)b); }
static inline v8i  xmm_psubsw(v8i a, v8i b){ return (v8i)__builtin_ia32_psubsw128((xmm_v8hi)a,(xmm_v8hi)b); }
static inline v16u xmm_paddusb(v16u a, v16u b){ return (v16u)__builtin_ia32_paddusb128((xmm_v16qi)a,(xmm_v16qi)b); }
static inline v8u  xmm_paddusw(v8u a, v8u b){ return (v8u)__builtin_ia32_paddusw128((xmm_v8hi)a,(xmm_v8hi)b); }
static inline v16u xmm_psubusb(v16u a, v16u b){ return (v16u)__builtin_ia32_psubusb128((xmm_v16qi)a,(xmm_v16qi)b); }
static inline v8u  xmm_psubusw(v8u a, v8u b){ return (v8u)__builtin_ia32_psubusw128((xmm_v8hi)a,(xmm_v8hi)b); }
#else
static inline v16i xmm_paddsb(v16i a, v16i b){ v16i r=(v16i)((v16u)a+(v16u)b), o=(~(a^b)&(a^r))>>7; return (r&~o)|(((a>>7)^0x7f)&o); }
static inline v8i  xmm_paddsw(v8i a, v8i b){ v8i r=(v8i)((v8u)a+(v8u)b), o=(~(a^b)&(a^r))>>15; return (r&~o)|(((a>>15)^0x7fff)&o); }
static inline v16i xmm_psubsb(v16i a, v16i b){ v16i r=(v16i)((v16u)a-(v16u)b), o=((a^b)&(a^r))>>7; return (r&~o)|(((a>>7)^0x7f)&o); }
static inline v8i  xmm_psubsw(v8i a, v8i b){ v8i r=(v8i)((v8u)a-(v8u)b), o=((a^b)&(a^r))>>15; return (r&~o)|(((a>>15)^0x7fff)&o); }
static inline v16u xmm_paddusb(v16u a, v16u b){ v16u r=a+b; return r|(v16u)(r<a); }
static inline v8u  xmm_paddusw(v8u a, v8u b){ v8u r=a+b; return r|(v8u)(r<a); }
static inline v16u xmm_psubusb(v16u a, v16u b){ v16u r=a-b; return r&(v16u)(a>b); }
static inline v8u  xmm_psubusw(v8u a, v8u b){ v8u r=a-b; return r&(v8u)(a>b); }
#endif
/* movmskps / movmskpd / pmovmskb: the sign bits of the lanes as an integer */
#if defined(XMM_X86)
static inline uint32_t xmm_movmskps(v4u x){ return (uint32_t)__builtin_ia32_movmskps((xmm_v4sf)x); }
static inline uint32_t xmm_movmskpd(v2u x){ return (uint32_t)__builtin_ia32_movmskpd((xmm_v2df)x); }
static inline uint32_t xmm_pmovmskb(v16u x){ return (uint32_t)__builtin_ia32_pmovmskb128((xmm_v16qi)x); }
#else
static inline uint32_t xmm_movmskps(v4u x){ v4u s=(x>>31)<<(v4u){0,1,2,3}; return s[0]+s[1]+s[2]+s[3]; }
static inline uint32_t xmm_movmskpd(v2u x){ v2u s=(x>>63)<<(v2u){0,1}; return (uint32_t)(s[0]+s[1]); }
static inline uint32_t xmm_pmovmskb(v16u x){ v16u s=(x>>7)<<(v16u){0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7}; v2u q=(v2u)s;   /* bit k of each half in byte k, then a horizontal byte sum */
  return (uint32_t)(((q[0]*0x0101010101010101ull)>>56)|(((q[1]*0x0101010101010101ull)>>56)<<8)); }
#endif
/* packed shifts by a run-time count (low 64 bits of an xmm/m64): counts >= lane width give 0 (logical) / sign (arith) */
static inline v8u xmm_psllw(v8u a, uint64_t n){ return n>15 ? (v8u){0} : a<<(int)n; }
static inline v8u xmm_psrlw(v8u a, uint64_t n){ return n>15 ? (v8u){0} : a>>(int)n; }
static inline v8i xmm_psraw(v8i a, uint64_t n){ return a>>(int)(n>15?15:n); }
static inline v4u xmm_pslld(v4u a, uint64_t n){ return n>31 ? (v4u){0} : a<<(int)n; }
static inline v4u xmm_psrld(v4u a, uint64_t n){ return n>31 ? (v4u){0} : a>>(int)n; }
static inline v4i xmm_psrad(v4i a, uint64_t n){ return a>>(int)(n>31?31:n); }
static inline v2u xmm_psllq(v2u a, uint64_t n){ return n>63 ? (v2u){0} : a<<(int)n; }
static inline v2u xmm_psrlq(v2u a, uint64_t n){ return n>63 ? (v2u){0} : a>>(int)n; }
typedef struct CPU {
  uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi;
  uint8_t cf,zf,sf,of,pf,df;
  XMM xmm[8];
  double st[8]; uint32_t top; uint16_t fcw, fsw;
  uint32_t fs_base;
} CPU;
#define A32(a) ((uint32_t)(a))
/* compiler barrier on loop back-edges: spin-waits on memory written by other threads must reload it */
#define BARRIER() __asm__ __volatile__("" ::: "memory")
#ifdef IDENTITY
/* Windows build: guest address == host address */
#define M ((uint8_t*)0)
#define HP(a) ((void*)(uintptr_t)(uint32_t)(a))
static inline uint8_t  R8 (uint32_t a){ return *(volatile uint8_t*)HP(a); }
static inline uint16_t R16(uint32_t a){ uint16_t v; memcpy(&v,HP(a),2); return v; }
static inline uint32_t R32(uint32_t a){ uint32_t v; memcpy(&v,HP(a),4); return v; }
static inline uint64_t R64(uint32_t a){ uint64_t v; memcpy(&v,HP(a),8); return v; }
static inline void W8 (uint32_t a,uint8_t v){ *(volatile uint8_t*)HP(a)=v; }
static inline void W16(uint32_t a,uint16_t v){ memcpy(HP(a),&v,2); }
static inline void W32(uint32_t a,uint32_t v){ memcpy(HP(a),&v,4); }
static inline void W64(uint32_t a,uint64_t v){ memcpy(HP(a),&v,8); }
static inline float  RF32(uint32_t a){ float v; memcpy(&v,HP(a),4); return v; }
static inline double RF64(uint32_t a){ double v; memcpy(&v,HP(a),8); return v; }
static inline void WF32(uint32_t a,float v){ memcpy(HP(a),&v,4); }
static inline void WF64(uint32_t a,double v){ memcpy(HP(a),&v,8); }
#define MEMP(a) HP(a)
#elif defined(BSI_TSO)
/* x86 memory-ordering emulation (TSO) for code shared between threads: every guest load is an acquire and every
   guest store a release (LDAPR/STLR on ARMv8.3, LDAR/STLR otherwise), so plain-mov publish/consume patterns that
   are safe on x86 stay safe on ARM. Unaligned accesses fall back to memcpy plus a fence. */
extern uint8_t *M;
#define MEMP(a) (M+(uint32_t)(a))
#define TSO_LD(T,a) do{ T v; if(!((a)&(sizeof(T)-1))) return __atomic_load_n((T*)(M+(a)),__ATOMIC_ACQUIRE); \
  memcpy(&v,M+(a),sizeof(T)); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; }while(0)
#define TSO_ST(T,a,v) do{ if(!((a)&(sizeof(T)-1))){ __atomic_store_n((T*)(M+(a)),(v),__ATOMIC_RELEASE); return; } \
  __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(M+(a),&(v),sizeof(T)); }while(0)
static inline uint8_t  R8 (uint32_t a){ return __atomic_load_n(M+a,__ATOMIC_ACQUIRE); }
static inline uint16_t R16(uint32_t a){ TSO_LD(uint16_t,a); }
static inline uint32_t R32(uint32_t a){ TSO_LD(uint32_t,a); }
static inline uint64_t R64(uint32_t a){ TSO_LD(uint64_t,a); }
static inline void W8 (uint32_t a,uint8_t v){ __atomic_store_n(M+a,v,__ATOMIC_RELEASE); }
static inline void W16(uint32_t a,uint16_t v){ TSO_ST(uint16_t,a,v); }
static inline void W32(uint32_t a,uint32_t v){ TSO_ST(uint32_t,a,v); }
static inline void W64(uint32_t a,uint64_t v){ TSO_ST(uint64_t,a,v); }
static inline float  RF32(uint32_t a){ uint32_t x=R32(a); float v; memcpy(&v,&x,4); return v; }
static inline double RF64(uint32_t a){ uint64_t x=R64(a); double v; memcpy(&v,&x,8); return v; }
static inline void WF32(uint32_t a,float v){ uint32_t x; memcpy(&x,&v,4); W32(a,x); }
static inline void WF64(uint32_t a,double v){ uint64_t x; memcpy(&x,&v,8); W64(a,x); }
#else
#ifdef BSI_FIXED_M
/* Fixed guest memory base (build flag -DBSI_FIXED_M=0x2000000000ULL, step 4 of OVERNIGHT.md): the generated code uses a
   compile-time constant instead of the global M, so it no longer reloads M after every guest store (with
   -fno-strict-aliasing any store may alias the global). The runtime sees bsi_gen_fixed_base (weak, defined here in every
   generated TU) and reserves guest memory at exactly that address with MAP_FIXED_NOREPLACE, aborting if it is taken.
   bsi_rt_fixed_m_ok is defined only by a runtime that knows this protocol, so linking these objects against an older
   runtime fails at link time instead of crashing at run time. Runtime sources are compiled without the flag and keep
   using the variable M, which then holds the same address. */
#define M ((uint8_t*)(uintptr_t)(BSI_FIXED_M))
extern const uint8_t bsi_rt_fixed_m_ok __attribute__((visibility("hidden")));
__attribute__((weak,used,visibility("hidden"))) const uint64_t bsi_gen_fixed_base = (BSI_FIXED_M);
__attribute__((weak,used,visibility("hidden"))) const uint8_t *const bsi_gen_fixed_req = &bsi_rt_fixed_m_ok;
#else
extern uint8_t *M;
#endif
#define MEMP(a) (M+(uint32_t)(a))
static inline uint8_t  R8 (uint32_t a){ return *(uint8_t*)(M+a); }
static inline uint16_t R16(uint32_t a){ uint16_t v; memcpy(&v,M+a,2); return v; }
static inline uint32_t R32(uint32_t a){ uint32_t v; memcpy(&v,M+a,4); return v; }
static inline uint64_t R64(uint32_t a){ uint64_t v; memcpy(&v,M+a,8); return v; }
static inline void W8 (uint32_t a,uint8_t v){ *(uint8_t*)(M+a)=v; }
static inline void W16(uint32_t a,uint16_t v){ memcpy(M+a,&v,2); }
static inline void W32(uint32_t a,uint32_t v){ memcpy(M+a,&v,4); }
static inline void W64(uint32_t a,uint64_t v){ memcpy(M+a,&v,8); }
static inline float  RF32(uint32_t a){ float v; memcpy(&v,M+a,4); return v; }
static inline double RF64(uint32_t a){ double v; memcpy(&v,M+a,8); return v; }
static inline void WF32(uint32_t a,float v){ memcpy(M+a,&v,4); }
static inline void WF64(uint32_t a,double v){ memcpy(M+a,&v,8); }
#endif

/* x87 80-bit extended <-> double (portable: no long double on ARM) */
static inline double RF80(uint32_t a){ uint64_t m=R64(a); uint16_t se=R16(a+8); int e=se&0x7fff; double s=(se&0x8000)?-1.0:1.0;
  if(e==0 && m==0) return s*0.0; if(e==0x7fff) return (m<<1)?NAN:s*INFINITY; return s*ldexp((double)m,e-16383-63); }
static inline void WF80(uint32_t a, double v){ uint16_t se=0; uint64_t m=0; if(signbit(v)){ se=0x8000; v=-v; }
  if(v!=v){ se|=0x7fff; m=0xC000000000000000ull; } else if(isinf(v)){ se|=0x7fff; m=0x8000000000000000ull; }
  else if(v!=0){ int e; double f=frexp(v,&e); m=(uint64_t)ldexp(f,64); se|=(uint16_t)(e-1+16383); }
  W64(a,m); W16(a+8,se); }
static inline uint8_t PAR(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return !(v&1); }
/* BSI_PROMO_EA64 (x86c.py): 64-bit effective addresses for [base+disp] operands. Today every guest access is
   M + (uint32_t)(base + disp): the 32-bit add must be done first (x86 addresses wrap at 4 GB), so the host code is
   `add w, w, #disp; add/orr x, M, w; ldr [x]` and nothing can be shared between [esi+0x10] and [esi+0x14]. With EA64 the
   translator emits EA(base, disp) = (host pointer of base) + disp instead: the wrap is kept for base (+index*scale),
   the displacement is added in 64-bit pointer arithmetic, so the compiler forms the host base once per guest base value
   and folds the displacement into the addressing mode (`ldr [x, #disp]`, ldp/stp pairs). Exact for every access whose
   x86 address does not wrap; a wrapped one (base within |disp| of 0 or 4 GB: an invalid pointer on Windows, where the
   first and last 64 KB are never mapped) lands in the PROT_NONE guard pages the runtime keeps on both sides of the
   window instead of silently wrapping (guest_reserve, rt_linux.c), so |disp| is limited to 64 KB in x86c.py.
   The empty asm pins the host base of `a` in a register: without it clang re-associates (M+a)+d into (M+d)+a and
   materialises a fresh 40-bit constant per displacement (2 instructions each); with it, identical bases are CSE'd into
   one register and each access is `ldr [x, #d]` (ldp/stp where adjacent). The R.., W.. and MEMP names below accept
   either a guest address (as before) or such a host pointer, chosen by type, so every emitter keeps its text. */
#define EA(a,d) ({ uint8_t *_eap_=MEMPA(a); __asm__("" : "+r"(_eap_)); _eap_+(int32_t)(d); })   /* MEMPA: the window pointer of any build variant */
#ifdef BSI_TSO
#define TSO_LDP(T,p) do{ T v; if(!((uintptr_t)(p)&(sizeof(T)-1))) return __atomic_load_n((const T*)(p),__ATOMIC_ACQUIRE); \
  memcpy(&v,(p),sizeof(T)); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; }while(0)
#define TSO_STP(T,p,v) do{ if(!((uintptr_t)(p)&(sizeof(T)-1))){ __atomic_store_n((T*)(p),(v),__ATOMIC_RELEASE); return; } \
  __atomic_thread_fence(__ATOMIC_RELEASE); memcpy((p),&(v),sizeof(T)); }while(0)
static inline uint8_t  R8P (const uint8_t*p){ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline uint16_t R16P(const uint8_t*p){ TSO_LDP(uint16_t,p); }
static inline uint32_t R32P(const uint8_t*p){ TSO_LDP(uint32_t,p); }
static inline uint64_t R64P(const uint8_t*p){ TSO_LDP(uint64_t,p); }
static inline void W8P (uint8_t*p,uint8_t v){ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static inline void W16P(uint8_t*p,uint16_t v){ TSO_STP(uint16_t,p,v); }
static inline void W32P(uint8_t*p,uint32_t v){ TSO_STP(uint32_t,p,v); }
static inline void W64P(uint8_t*p,uint64_t v){ TSO_STP(uint64_t,p,v); }
#else
static inline uint8_t  R8P (const uint8_t*p){ return *p; }
static inline uint16_t R16P(const uint8_t*p){ uint16_t v; memcpy(&v,p,2); return v; }
static inline uint32_t R32P(const uint8_t*p){ uint32_t v; memcpy(&v,p,4); return v; }
static inline uint64_t R64P(const uint8_t*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline void W8P (uint8_t*p,uint8_t v){ *p=v; }
static inline void W16P(uint8_t*p,uint16_t v){ memcpy(p,&v,2); }
static inline void W32P(uint8_t*p,uint32_t v){ memcpy(p,&v,4); }
static inline void W64P(uint8_t*p,uint64_t v){ memcpy(p,&v,8); }
#endif
static inline float  RF32P(const uint8_t*p){ uint32_t x=R32P(p); float v; memcpy(&v,&x,4); return v; }
static inline double RF64P(const uint8_t*p){ uint64_t x=R64P(p); double v; memcpy(&v,&x,8); return v; }
static inline void WF32P(uint8_t*p,float v){ uint32_t x; memcpy(&x,&v,4); W32P(p,x); }
static inline void WF64P(uint8_t*p,double v){ uint64_t x; memcpy(&x,&v,8); W64P(p,x); }
static inline double RF80P(const uint8_t*p){ uint64_t m=R64P(p); uint16_t se=R16P(p+8); int e=se&0x7fff; double s=(se&0x8000)?-1.0:1.0;
  if(e==0 && m==0) return s*0.0; if(e==0x7fff) return (m<<1)?NAN:s*INFINITY; return s*ldexp((double)m,e-16383-63); }
static inline void WF80P(uint8_t*p, double v){ uint16_t se=0; uint64_t m=0; if(signbit(v)){ se=0x8000; v=-v; }
  if(v!=v){ se|=0x7fff; m=0xC000000000000000ull; } else if(isinf(v)){ se|=0x7fff; m=0x8000000000000000ull; }
  else if(v!=0){ int e; double f=frexp(v,&e); m=(uint64_t)ldexp(f,64); se|=(uint16_t)(e-1+16383); }
  W64P(p,m); W16P(p+8,se); }
static inline uint8_t *MEMPA(uint32_t a){ return MEMP(a); }
#undef MEMP
/* default branch = the variant's original expression (not MEMPA), so code that only passes addresses -- the whole runtime --
   compiles exactly as before (regress/ea64: runtime objects byte/code-identical) */
#ifdef IDENTITY
#define MEMP_ADDR_(a) HP((uintptr_t)(a))
#else
#define MEMP_ADDR_(a) (M+(uint32_t)(uintptr_t)(a))
#endif
#define MEMP(a) _Generic((a), uint8_t*: (uint8_t*)(uintptr_t)(a), const uint8_t*: (uint8_t*)(uintptr_t)(a), default: MEMP_ADDR_(a))
#define R8(a)   _Generic((a), uint8_t*: R8P,   const uint8_t*: R8P,   default: R8)(a)
#define R16(a)  _Generic((a), uint8_t*: R16P,  const uint8_t*: R16P,  default: R16)(a)
#define R32(a)  _Generic((a), uint8_t*: R32P,  const uint8_t*: R32P,  default: R32)(a)
#define R64(a)  _Generic((a), uint8_t*: R64P,  const uint8_t*: R64P,  default: R64)(a)
#define RF32(a) _Generic((a), uint8_t*: RF32P, const uint8_t*: RF32P, default: RF32)(a)
#define RF64(a) _Generic((a), uint8_t*: RF64P, const uint8_t*: RF64P, default: RF64)(a)
#define RF80(a) _Generic((a), uint8_t*: RF80P, const uint8_t*: RF80P, default: RF80)(a)
#define W8(a,v)   _Generic((a), uint8_t*: W8P,   default: W8)((a),(v))
#define W16(a,v)  _Generic((a), uint8_t*: W16P,  default: W16)((a),(v))
#define W32(a,v)  _Generic((a), uint8_t*: W32P,  default: W32)((a),(v))
#define W64(a,v)  _Generic((a), uint8_t*: W64P,  default: W64)((a),(v))
#define WF32(a,v) _Generic((a), uint8_t*: WF32P, default: WF32)((a),(v))
#define WF64(a,v) _Generic((a), uint8_t*: WF64P, default: WF64)((a),(v))
#define WF80(a,v) _Generic((a), uint8_t*: WF80P, default: WF80)((a),(v))
#define PUSH32(c,v) do{ uint32_t _pv_=(uint32_t)(v); (c)->esp-=4; W32((c)->esp,_pv_); }while(0)
#define POP32(c) ((c)->esp+=4, R32((c)->esp-4))
#define ST(c,i) ((c)->st[((c)->top+(i))&7])
#define FPUSH(c,v) do{ double _fpv_=(v); (c)->top=((c)->top-1)&7; (c)->st[(c)->top]=_fpv_; }while(0)
#define FPOP(c) ((c)->top=((c)->top+1)&7)
typedef void (*gfn)(CPU*);
void call_indirect(CPU*c, uint32_t target);
/* BSI_PROMO_IC: per-site inline cache for guest indirect calls (x86c.py emits IC_CALL at every `call reg/mem` and
   tail-jump site with a block-scope `static uint64_t _ic`). Entry layout (one 64-bit word, so a reader never sees a torn
   pair): bits 63..32 guest target; bits 31..30 retarget count; bits 29..0 signed word offset of the host function from
   &_ic itself (PC-relative: the hit path needs no global and no GOT load -- only the adrp/add it already has for the
   load). 0 = empty. Only call_indirect_ic fills it (ic_fill below), and only on a flat-table hit (rt_linux.c), so an
   entry can only replay a lookup the flat table would have made; COM/import magic, thunks, emulated, patched and
   BSI_NOFT targets never reach a hit and are never cached. A site is written at most 4 times (fill + 3 retargets, then
   frozen): a polymorphic site costs one extra compare per call, never a cache line bouncing between threads.
   BSI_IC=0 (env) disables the fills: every site then takes the old path plus one compare. */
void call_indirect_ic(CPU*c, uint32_t target, uint64_t *ic);
#define IC_CALL(ic,t) do{ uint64_t _ice_=__atomic_load_n(&(ic),__ATOMIC_RELAXED); uint32_t _ict_=(t); \
  if(__builtin_expect((uint32_t)(_ice_>>32)==_ict_ && _ice_!=0,1)) \
    ((gfn)((uintptr_t)&(ic)+(intptr_t)((int64_t)(_ice_<<34)>>32)))(c); \
  else call_indirect_ic(c,_ict_,&(ic)); }while(0)
#define IC_MAX_WRITES 4
/* store (t -> f) into the site's cache unless it is frozen or f is unencodable; returns 1 if stored */
static inline int ic_fill(uint64_t *ic, uint32_t t, gfn f){
  uint64_t cur=__atomic_load_n(ic,__ATOMIC_RELAXED); uint32_t n=cur?((uint32_t)(cur>>30)&3u)+1u:0u;
  intptr_t d=(intptr_t)f-(intptr_t)ic;
  if(n>=IC_MAX_WRITES || (d&3) || d<-((intptr_t)1<<31) || d>=((intptr_t)1<<31)) return 0;
  __atomic_store_n(ic,((uint64_t)t<<32)|((uint64_t)n<<30)|((uint64_t)(uint32_t)(d>>2)&0x3fffffffu),__ATOMIC_RELAXED); return 1; }
void call_import(CPU*c, uint32_t iat);
void host_cpuid(CPU*c);
void host_ldmxcsr(CPU*c,uint32_t v);
uint32_t host_stmxcsr(CPU*c);
void host_rdtsc(CPU*c);
void TRACE(CPU*c,uint32_t a);
void unimpl(CPU*c, uint32_t addr, const char* what);
double fpu_round(CPU*c, double v);
static inline int32_t cvtt_f2i(double v){ if(!(v>-2147483649.0 && v<2147483648.0)) return (int32_t)0x80000000; return (int32_t)v; }
static inline int32_t cvt_f2i(double v){ if(!(v>-2147483649.0 && v<2147483648.0)) return (int32_t)0x80000000; return (int32_t)nearbyint(v); }
/* C++ exception resume support: functions with catch blocks run their body through EH_FRAME */
#if defined(__clang__) && !defined(__i386__) && !defined(__x86_64__)
/* clang on ARM64 has no __builtin_setjmp: use the C library's _setjmp/_longjmp (no signal mask) */
#include <setjmp.h>
typedef struct EHF { struct EHF *prev; uint32_t entry_esp; jmp_buf jb; } EHF;
#define EH_SETJMP(b) _setjmp(b)
#define EH_LONGJMP(b) _longjmp(b,1)
#else
typedef struct EHF { struct EHF *prev; uint32_t entry_esp; void *jb[5]; } EHF;
#define EH_SETJMP(b) __builtin_setjmp(b)
#define EH_LONGJMP(b) __builtin_longjmp(b,1)
#endif
extern __thread EHF *t_ehtop; extern __thread uint32_t t_eh_cont;
#define EH_FRAME(c,b) do{ CPU *volatile _c=(c); EHF _f; uint32_t _r=0; _f.prev=t_ehtop; _f.entry_esp=_c->esp; t_ehtop=&_f; \
  if(EH_SETJMP(_f.jb)){ _r=t_eh_cont; t_ehtop=&_f; } b(_c,_r); t_ehtop=_f.prev; }while(0)
void eh_throw(CPU*c);   /* guest _CxxThrowException(obj, throwinfo): args at esp+4/esp+8, return address at esp */

/* register-promoted code (BSI_PROMO=1): guest esp lives in the local r_esp */
#define PUSH32L(v) do{ uint32_t _pv_=(uint32_t)(v); r_esp-=4; W32(r_esp,_pv_); }while(0)
#define POP32L() (r_esp+=4, R32(r_esp-4))
