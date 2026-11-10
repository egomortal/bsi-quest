/* mb_off.c -- switch the game's camera motion blur OFF (default) and, optionally, bloom OFF, through patches.c's patch_call.
   No generated code changes.

   Where BioShock Infinite decides motion blur (exe addresses; notes in regress/mb_off/README.md):
   - The blur is UE3 "cheap motion blur", part of UUberPostProcessEffect (the only effect class that does it). Settings come from
     the blended FPostProcessSettings (post-process volumes / world defaults / camera): bEnableCheapMotionBlur = +0x08 bit 3,
     CheapMotionBlur_MinDepth/MaxDepth/Scale/TangentialScale/Clamp = +0x64/+0x68/+0x6C/+0x70/+0x74.
   - Game thread, per view per frame: FSceneView ctor (FUN_00731e80, loop at 0x732556) calls effect->IsShown (vf036, slot +0x90),
     then effect->CreateSceneProxy(WorldSettings) (vf035, slot +0x8C). UUberPostProcessEffect::CreateSceneProxy = 0x5CC660
     allocates the 0xD4-byte proxy (vtable 0x112FFD8, or 0x112FFE8 with the HDR tonemapper) via FUN_005cae60/005cafe0, whose
     base ctor FUN_0051d1e0 copies the CheapMotionBlur values to proxy+0x68..0x78 and, if bEnableCheapMotionBlur is clear,
     stores 0.0 to proxy+0x70 (Scale) at 0x51d44d.
   - Render thread: the Uber proxy's Render (0x5CF730 / 0x5D02E0, vtable slot 1) computes bMotion = proxy+0x70 > 0.0
     (0x5cf867 / 0x5d044e) and passes it to the DOF/bloom gather step FUN_00573550 (4th arg; picks the
     TDOFBloomMotionGatherPixelShader variant with motion) and to the Gaussian blur FUN_0071fc60 (BLUR_DOF_AND_MOTIONBLUR filter
     variants). Nothing else reads it.

   What this file does for motion blur: wraps 0x5CC660. After the original returns, if the result is an Uber proxy (vtable
   check), it stores 0.0 to proxy+0x70 -- exactly what the game's own ctor does for a volume with bEnableCheapMotionBlur off.
   DOF, bloom, colour grading/tonemapping and radial blur are untouched. The settings structs and post-process volumes are never
   written.

   Switch (default OFF = no motion blur):
     env BSI_MOTIONBLUR=1   motion blur ON, and the motion blur hook is not even installed (exact original dispatch unless the
                            bloom switch below needs the same hook). =0 forces OFF and ignores the file.
     file $BSI_MB_FILE (default /sdcard/BSI/motionblur.txt): first number 1 = ON, anything else / missing = OFF. Re-read every
                            2 s, so it switches live:  adb shell 'echo 1 > /sdcard/BSI/motionblur.txt'  /  rm it.
     BSI_PATCHES=0          disables all patch_call patches, this one included (motion blur ON, bloom ON).
   Diagnostics: BSI_MBSTAT=1 also counts, on the render thread, how many Uber Render calls had a motion input > 0 (i.e. would
   select the motion gather/filter variants). Log lines start with "[mb_off]" (stderr = bsi.log on the Quest).

   BLOOM (default ON = game unchanged). Where the game decides it:
   - FPostProcessSettings +0x08 bit 1 = bEnableBloom, +0x0C Bloom_Scale, +0x10 Bloom_Threshold, +0x14 Bloom_Tint (FColor),
     +0x18 Bloom_ScreenBlendThreshold (sdk/Engine_structs.h). The shared proxy base ctor FUN_0051d1e0 copies them to
     proxy+0x4C (Scale), +0x50 (Threshold), +0x54..0x63 (Tint as FLinearColor, FUN_00481db0), +0x64 (ScreenBlendThreshold),
     and stores 0.0 to proxy+0x4C at 0x51d442 when bEnableBloom is clear -- the game's own "bloom off".
   - Render thread: both Uber Render variants (call sites 0x5cf8af / 0x5d0496) and UDOFAndBloomEffect's (0x573cb4) call the
     gather step FUN_00573550(View, bDOF=1, bBloom=1, bMotion, ...) with bBloom hard-wired to 1. The gather picks one of 7
     TDOFBloomMotionGatherPixelShader<Bloom,Motion,DOF> variants (FUN_0056ba30..FUN_00570170) and uploads
     (proxy+0x4C, proxy+0x50) as BloomScaleAndThreshold: the bloom term written to the half-res gather buffer is
     (excess luminance over Threshold) * Scale, so Scale 0 adds exactly nothing. Tint and ScreenBlendThreshold are not read on
     the Uber render path. DOF, the Uber scene colour/tonemap pass and the blur passes do not use the bloom values.
     Bloom=False in XEngine.ini only feeds the IsShown check 0x50dd70 (it does nothing while DepthOfField=True).
   - The bloom-free gather variants (Bloom=FALSE) are in the shader caches but are never selected by shipped code, and the three
     calls to 0x573550 are direct calls (compiled as plain C calls in the generated parts), so patch_call cannot change bBloom; that would
     need generated-code edits for a few ALU ops per half-res pixel (the texture taps are shared with DOF). Not done.
   What this file does for bloom: in the same CreateSceneProxy wrapper (and in UDOFAndBloomEffect::CreateSceneProxy 0x51DD10,
   proxy vtable 0x111878C, for maps that use the plain effect) it stores 0.0 to proxy+0x4C of each proxy whose scale is > 0 --
   the same state the game's ctor makes for bEnableBloom = FALSE. A number other than 0/1 in bloom.txt multiplies the scale
   instead (strength; e.g. 0.5 = half, 8 = test amplification).
   Switch:
     env BSI_BLOOM=0        forces bloom OFF and ignores the file; =1 forces ON and the bloom part is not installed.
     file $BSI_BLOOM_FILE (default /sdcard/BSI/bloom.txt): first number 0 = OFF; 1 / missing / not a number = ON (game value);
                            any other positive number = strength multiplier. Re-read every 2 s together with motionblur.txt:
                            adb shell 'echo 0 > /sdcard/BSI/bloom.txt'  /  rm it.
   Log lines start with "[bloom]".

   Integration (regress/mb_off/mb_off.patch): patches.c patch_call -> weak mb_off_call(); rt_linux.c ft_init keeps the hooked
   addresses off the flat call table when weak mb_off_skip() says so; this file in the runtime source lists. A build without
   mb_off.o is unchanged (weak references are NULL). */
#include "hle.h"
#include <time.h>
#include <unistd.h>

void call_indirect(CPU *c, uint32_t t);

#define MB_CREATE   0x5CC660u   /* UUberPostProcessEffect::CreateSceneProxy (vf035), thiscall(WorldSettings), ret 4 */
#define MB_RENDER1  0x5CF730u   /* Uber proxy Render (vtable 0x112FFD8 slot 1) */
#define MB_RENDER2  0x5D02E0u   /* Uber proxy Render, HDR tonemapper variant (vtable 0x112FFE8 slot 1) */
#define MB_VT1      0x112FFD8u
#define MB_VT2      0x112FFE8u
#define MB_SCALE    0x70u       /* proxy offset of CheapMotionBlur_Scale (masked by bEnableCheapMotionBlur) */
#define BL_CREATE2  0x51DD10u   /* UDOFAndBloomEffect::CreateSceneProxy (vf035, vftable 0x11180C8), thiscall(WorldSettings), ret 4 */
#define BL_VT       0x111878Cu  /* its proxy vtable (Render 0x573B60) */
#define BL_SCALE    0x4Cu       /* proxy offset of Bloom_Scale (masked by bEnableBloom) */

static int s_init, s_hook, s_stat, s_env = -1, s_id;
static int s_mb_hook;                     /* motion blur part active (the hook of the motion-blur-only version) */
static volatile int s_on;                 /* 1 = motion blur on (game behaviour), 0 = forced off */
static double s_next_poll, s_next_log;
static char s_file[512];
static int s_bl_env = -1, s_bl_id, s_bl_hook;
static volatile float s_bl_mul = 1.0f;    /* bloom: 1 = game value (on), 0 = off, other = strength multiplier */
static char s_bl_file[512];
/* counters (exported for tests) */
volatile unsigned mb_proxies, mb_motion, mb_zeroed, mb_renders, mb_motion_renders;
volatile unsigned bl_proxies, bl_scaled, bl_zeroed, bl_scaledmul, bl_plain;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* exe identity: Uber vtable slots 35 / 1 and the FName string, all read-only data of the loaded image */
static int identity_ok(void){
  static const char nm[] = "CheapMotionBlur_Scale";
  if(R32(0x112CE48u + 35 * 4) != MB_CREATE) return 0;          /* UUberPostProcessEffect::vftable slot 35 */
  if(R32(MB_VT1 + 4) != MB_RENDER1 || R32(MB_VT2 + 4) != MB_RENDER2) return 0;
  for(unsigned i = 0; i < sizeof nm; i++) if(R8(0x114C9D0u + i) != (uint8_t)nm[i]) return 0;
  return 1;
}
/* bloom identity: the Bloom_Scale FName string, UDOFAndBloomEffect vftable slot 35 / its proxy's Render, and the base ctor's
   own bloom-off store "movss [edi+0x4c], xmm0" at 0x51d442 (so +0x4C really is Bloom_Scale in this exe) */
static int bloom_identity_ok(void){
  static const char nm[] = "Bloom_Scale";
  static const uint8_t st[] = { 0xF3, 0x0F, 0x11, 0x47, 0x4C };
  for(unsigned i = 0; i < sizeof nm; i++) if(R8(0x114C7A0u + i) != (uint8_t)nm[i]) return 0;
  for(unsigned i = 0; i < sizeof st; i++) if(R8(0x51D442u + i) != st[i]) return 0;
  return R32(0x11180C8u + 35 * 4) == BL_CREATE2 && R32(BL_VT + 4) == 0x573B60u;
}

static int read_file_on(void){
  FILE *f = fopen(s_file, "r"); if(!f) return 0;
  int v = 0; if(fscanf(f, "%d", &v) != 1) v = 0; fclose(f);
  return v == 1;
}
/* bloom.txt: first number 0 = off, 1 / missing / not a number = on (game value), other positive = strength multiplier */
static float read_bloom_file(void){
  FILE *f = fopen(s_bl_file, "r"); if(!f) return 1.0f;
  float v = 1.0f; if(fscanf(f, "%f", &v) != 1 || !(v >= 0.0f) || v > 1000.0f) v = 1.0f; fclose(f);
  return v;
}
static const char *bl_state(float m, char *b, size_t n){
  if(m == 0.0f) return "OFF"; if(m == 1.0f) return "ON"; snprintf(b, n, "ON x%g", m); return b; }

static void mb_init(void){
  if(s_init) return;
  const char *e = getenv("BSI_MOTIONBLUR"); s_env = e && *e ? atoi(e) : -1;
  const char *p = getenv("BSI_PATCHES");
  int pat = p ? atoi(p) : 1;
  const char *ff = getenv("BSI_MB_FILE"); snprintf(s_file, sizeof s_file, "%s", ff && *ff ? ff : "/sdcard/BSI/motionblur.txt");
  s_stat = getenv("BSI_MBSTAT") && atoi(getenv("BSI_MBSTAT")) != 0;
  s_id = identity_ok();
  s_mb_hook = pat && s_env != 1 && s_id;
  s_on = s_env == 0 ? 0 : s_env == 1 ? 1 : read_file_on();
  const char *be = getenv("BSI_BLOOM"); s_bl_env = be && *be ? atoi(be) != 0 : -1;
  const char *bf = getenv("BSI_BLOOM_FILE"); snprintf(s_bl_file, sizeof s_bl_file, "%s", bf && *bf ? bf : "/sdcard/BSI/bloom.txt");
  s_bl_id = s_id && bloom_identity_ok();
  s_bl_hook = pat && s_bl_env != 1 && s_bl_id;
  s_bl_mul = s_bl_env == 0 ? 0.0f : s_bl_env == 1 ? 1.0f : read_bloom_file();
  s_hook = s_mb_hook || s_bl_hook;
  s_next_poll = s_next_log = now_s() + 2.0;
  s_init = 1;
  if(!s_id) fprintf(stderr, "[mb_off] exe identity check failed: hook not installed, motion blur unchanged\n");
  else if(!pat) fprintf(stderr, "[mb_off] BSI_PATCHES=0: hook not installed, motion blur ON\n");
  else if(s_env == 1) fprintf(stderr, s_bl_hook ? "[mb_off] BSI_MOTIONBLUR=1: motion blur ON (game default; hook kept only for the bloom switch)\n"
                                                : "[mb_off] BSI_MOTIONBLUR=1: hook not installed, motion blur ON (game default)\n");
  else fprintf(stderr, "[mb_off] motion blur %s (%s)%s\n", s_on ? "ON" : "OFF",
                 s_env == 0 ? "BSI_MOTIONBLUR=0" : s_on ? s_file : "default; put 1 in motionblur.txt to restore",
                 s_stat ? ", BSI_MBSTAT render counters on" : "");
  char sb[32];
  if(!s_id) fprintf(stderr, "[bloom] exe identity check failed: bloom switch not installed, bloom unchanged\n");
  else if(!pat) fprintf(stderr, "[bloom] BSI_PATCHES=0: bloom switch not installed, bloom ON\n");
  else if(!s_bl_id) fprintf(stderr, "[bloom] exe identity check failed: bloom switch not installed, bloom unchanged\n");
  else if(s_bl_env == 1) fprintf(stderr, "[bloom] BSI_BLOOM=1: bloom switch not installed, bloom ON (game default)\n");
  else fprintf(stderr, "[bloom] bloom %s (%s)\n", bl_state(s_bl_mul, sb, sizeof sb),
                 s_bl_env == 0 ? "BSI_BLOOM=0" : s_bl_mul == 1.0f ? "default; put 0 in bloom.txt to turn it off" : s_bl_file);
}

/* rt_linux.c ft_init: 1 = keep this guest function off the flat call table so its calls reach patch_call */
int mb_off_skip(uint32_t a){
  if(a != MB_CREATE && a != MB_RENDER1 && a != MB_RENDER2 && a != BL_CREATE2) return 0;
  mb_init();
  if(!s_hook) return 0;
  if(a == MB_CREATE) return 1;
  if(a == BL_CREATE2) return s_bl_hook;
  return s_mb_hook && s_stat;
}

static void mb_poll_and_log(void){
  double n = now_s();
  if(n >= s_next_poll){
    s_next_poll = n + 2.0;
    if(s_mb_hook && s_env < 0){ int v = read_file_on(); if(v != s_on){ s_on = v; fprintf(stderr, "[mb_off] %s: motion blur %s\n", s_file, v ? "ON" : "OFF"); } }
    if(s_bl_hook && s_bl_env < 0){ float v = read_bloom_file();
      if(v != s_bl_mul){ char sb[32]; s_bl_mul = v; fprintf(stderr, "[bloom] %s: bloom %s\n", s_bl_file, bl_state(v, sb, sizeof sb)); } }
  }
  if(n >= s_next_log){
    s_next_log = n + 10.0;
    static unsigned lp, lb;
    unsigned pr = mb_proxies;
    if(s_mb_hook && pr != lp){ lp = pr;
      if(s_stat) fprintf(stderr, "[mb_off] uber proxies %u, with motion scale>0 %u, zeroed %u | renders %u, with motion input %u\n",
                         pr, mb_motion, mb_zeroed, mb_renders, mb_motion_renders);
      else fprintf(stderr, "[mb_off] uber proxies %u, with motion scale>0 %u, zeroed %u\n", pr, mb_motion, mb_zeroed); }
    unsigned bp = bl_proxies;
    if(s_bl_hook && bp != lb){ lb = bp;
      fprintf(stderr, "[bloom] proxies %u (plain DOFAndBloom %u), with bloom scale>0 %u, zeroed %u, scaled %u\n",
              bp, bl_plain, bl_scaled, bl_zeroed, bl_scaledmul); }
  }
}

static float rdf(uint32_t a){ uint32_t b = R32(a); float s; memcpy(&s, &b, 4); return s; }
static void wrf(uint32_t a, float s){ uint32_t b; memcpy(&b, &s, 4); W32(a, b); }

/* bloom part on a fresh proxy (game thread, right after CreateSceneProxy; the render thread has not seen it yet) */
static void bloom_proxy(uint32_t px){
  float b = rdf(px + BL_SCALE), m = s_bl_mul;
  __atomic_add_fetch(&bl_proxies, 1, __ATOMIC_RELAXED);
  if(!(b > 0.0f)) return;
  unsigned k = __atomic_add_fetch(&bl_scaled, 1, __ATOMIC_RELAXED);
  if(m == 0.0f){
    W32(px + BL_SCALE, 0u);                         /* 0.0f: same as bEnableBloom = FALSE */
    unsigned z = __atomic_add_fetch(&bl_zeroed, 1, __ATOMIC_RELAXED);
    if(z <= 3) fprintf(stderr, "[bloom] proxy %08x: Bloom_Scale %g -> 0 (bloom off; threshold %g)\n", px, b, rdf(px + 0x50));
  } else if(m != 1.0f){
    wrf(px + BL_SCALE, b * m);
    unsigned z = __atomic_add_fetch(&bl_scaledmul, 1, __ATOMIC_RELAXED);
    if(z <= 3) fprintf(stderr, "[bloom] proxy %08x: Bloom_Scale %g -> %g (x%g; threshold %g)\n", px, b, b * m, m, rdf(px + 0x50));
  } else if(k <= 3) fprintf(stderr, "[bloom] proxy %08x: Bloom_Scale %g kept (bloom on; threshold %g)\n", px, b, rdf(px + 0x50));
}

int mb_off_call(CPU *c, uint32_t t){
  if(!s_init) mb_init();
  if(!s_hook) return 0;
  if(t == MB_CREATE || t == BL_CREATE2){
    if(t == BL_CREATE2 && !s_bl_hook) return 0;
    static __thread int in;
    if(in) return 0;                                   /* re-entry: patch_call falls through to the original */
    in = 1; call_indirect(c, t); in = 0;               /* original CreateSceneProxy (pops ret + 1 arg); eax = proxy or 0 */
    uint32_t px = c->eax;
    mb_poll_and_log();                                 /* picks up a motionblur.txt / bloom.txt change before this frame's decision */
    if(px){
      uint32_t vt = R32(px);
      if(vt == MB_VT1 || vt == MB_VT2){
        if(s_mb_hook){
          float s = rdf(px + MB_SCALE);
          __atomic_add_fetch(&mb_proxies, 1, __ATOMIC_RELAXED);
          if(s > 0.0f){
            unsigned m = __atomic_add_fetch(&mb_motion, 1, __ATOMIC_RELAXED);
            if(!s_on){
              W32(px + MB_SCALE, 0u);                     /* 0.0f: same as bEnableCheapMotionBlur = FALSE */
              unsigned z = __atomic_add_fetch(&mb_zeroed, 1, __ATOMIC_RELAXED);
              if(z <= 3) fprintf(stderr, "[mb_off] proxy %08x: CheapMotionBlur_Scale %g -> 0 (motion blur off)\n", px, s);
            } else if(m <= 3) fprintf(stderr, "[mb_off] proxy %08x: CheapMotionBlur_Scale %g kept (motion blur on)\n", px, s);
          }
        }
        if(s_bl_hook) bloom_proxy(px);
      } else if(vt == BL_VT && s_bl_hook){             /* plain UDOFAndBloomEffect proxy (no motion blur) */
        __atomic_add_fetch(&bl_plain, 1, __ATOMIC_RELAXED);
        bloom_proxy(px);
      }
    }
    return 1;
  }
  if(t == MB_RENDER1 || t == MB_RENDER2){            /* BSI_MBSTAT only: observe the render-thread decision input */
    if(!s_mb_hook) return 0;
    uint32_t px = c->ecx; float s = rdf(px + MB_SCALE);
    __atomic_add_fetch(&mb_renders, 1, __ATOMIC_RELAXED);
    if(s > 0.0f){ unsigned k = __atomic_add_fetch(&mb_motion_renders, 1, __ATOMIC_RELAXED);
      if(k <= 3) fprintf(stderr, "[mb_off] render %08x: motion input %g (motion gather/filter variant selected)\n", px, s); }
    return 0;                                          /* the original Render runs unchanged */
  }
  return 0;
}
