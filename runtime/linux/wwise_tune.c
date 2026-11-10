/* wwise_tune.c -- Wwise setting switches L1/L3 of notes/HOTCODE.md through patches.c's patch_call (no part rebuild).

   The game leaves Wwise wide open (Init.bnk: 256 physical voices, -75.3 dB volume threshold; 96 RoomVerb share-sets with
   2..10 reverb units and early reflections on). This file applies the cheaper settings at two virtual-call boundaries:

   1. UXAKAudioDevice::Init (vf036) 0xC22160, thiscall, no stack args, plain `ret`, eax = UBOOL (1 = ok; also 1 when
      already initialised: [this+0xF0] & 2). It reaches AK::SoundEngine::Init 0xF73330 through 0xC1BCA0. Wrapper: run
      the original (re-entry flag, like patches.c loadclose3), then, if it returned 1:
        BSI_AK_VOICES=n     AK::SoundEngine::SetMaxNumVoicesLimit(n)   0xF71070, cdecl, 1 arg (u16; 0 is rejected)
        BSI_AK_VOLTHR=dB    AK::SoundEngine::SetVolumeThreshold(dB)    0xF71050, cdecl, 1 float arg (-96.3 .. 0)
      Both use API priority 0, which beats Init (3) and the Init.bnk STMG values (2), whatever runs later.
      The caller sees exactly the state the original returned with (all registers, flags, xmm, x87 restored).
   2. CAkRoomVerbFX::Init 0xF6A4D0 (vtable slot 4), thiscall(alloc, ctx, params, format), ret 0x10. Pre-hook on the
      params block (R32(esp+0xC)), then the original runs and copies them to the FX instance once:
        BSI_AK_REVERB_UNITS=n   cap params+0x70 (u32 "Quality" = reverb units) to n (rounded down to even, >= 2;
                                units alternate L/R outputs; never raised)
        BSI_AK_REVERB_ER=0      clear params+0x95 (u8 early-reflections enable). Alias: BSI_AK_ER=0 (audio_native.c's name)
   3. Voice-count logger (BSI_AK_VOICELOG=1; on by default when BSI_AK_VOICES or BSI_AK_VOLTHR is set, =0 turns it off):
      a host thread started after the first successful vf036 samples [0x1412878] (all voices / PBIs) and [0x141286C]
      (virtual voices) every 250 ms and prints every BSI_AK_VOICELOG_SEC (default 10) seconds:
        [aktune] voices: now total/virtual/physical | window peak physical, peak total, mean physical | limit (0x134200C)
      Read-only plain loads from a host thread (the counters are 32-bit aligned words).

   Off unless one of the variables above is set: wwise_tune_enabled() == 0, rt_linux.c keeps both addresses in the flat
   call table, and patch_call never sees them. Hooking (regress/wwise_tune/wwise_tune.patch, not applied):
   - patches.c patch_call: `if(t==0xc22160u||t==0xf6a4d0u)` -> weak wwise_tune_call(c,t);
   - rt_linux.c ft_init: skip 0xC22160 / 0xF6A4D0 (the patched[] exclusion) when weak wwise_tune_enabled() says so;
   - this file in the runtime lists (build_linux_promo.sh, link_android.sh, link08x copies).
   A build without wwise_tune.o is unchanged (weak references are NULL).
   Coexistence with audio_native.c (rebuilt audio parts, same BSI_AK_VOICES/VOLTHR/REVERB_UNITS/ER switches at
   SoundEngine::Init and at the same RoomVerb Init): both edits are idempotent, so linking both just applies them twice.
   Host test: regress/wwise_tune/run.sh. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pthread_setname_np */
#endif
#include "hle.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void call_indirect(CPU *c, uint32_t t);
void guest_call(CPU *c, uint32_t fn);

#define WT_DEV_INIT   0xC22160u   /* UXAKAudioDevice::Init (vf036) */
#define WT_RV_INIT    0xF6A4D0u   /* CAkRoomVerbFX::Init */
#define WT_SET_VOICES 0xF71070u   /* AK::SoundEngine::SetMaxNumVoicesLimit */
#define WT_SET_VOLTHR 0xF71050u   /* AK::SoundEngine::SetVolumeThreshold */
#define WT_N_TOTAL    0x1412878u  /* all voices (PBIs) */
#define WT_N_VIRTUAL  0x141286Cu  /* virtual voices */
#define WT_LIMIT      0x134200Cu  /* u16 current max physical voices */

static pthread_once_t wt_once = PTHREAD_ONCE_INIT;
static int s_on, s_voices, s_has_thr, s_units, s_er = -1, s_log, s_log_sec = 10;
static float s_thr;
/* exported for the host test */
volatile unsigned wt_dev_hooks, wt_rv_hooks, wt_rv_changed, wt_reports;

static void wt_read_env(void){
  const char *e;
  if((e = getenv("BSI_AK_VOICES")) && atoi(e) > 0) s_voices = atoi(e) > 0xffff ? 0xffff : atoi(e);
  if((e = getenv("BSI_AK_VOLTHR")) && *e){ s_thr = (float)atof(e); s_has_thr = 1; }
  if((e = getenv("BSI_AK_REVERB_UNITS")) && atoi(e) >= 2) s_units = atoi(e) & ~1;
  if((e = getenv("BSI_AK_REVERB_ER")) || (e = getenv("BSI_AK_ER"))) s_er = atoi(e) != 0;
  s_log = s_voices || s_has_thr;
  if((e = getenv("BSI_AK_VOICELOG"))) s_log = atoi(e) != 0;
  if((e = getenv("BSI_AK_VOICELOG_SEC")) && atoi(e) > 0) s_log_sec = atoi(e);
  s_on = s_voices || s_has_thr || s_units || s_er == 0 || s_log;
  if(s_on) fprintf(stderr, "[aktune] Wwise tune: voices %d volthr %s%.1f reverb units cap %d ER %s voicelog %d (%d s)\n",
                   s_voices, s_has_thr ? "" : "(off) ", s_thr, s_units, s_er == 0 ? "off" : "unchanged", s_log, s_log_sec);
  if(s_has_thr && (s_thr < -96.3f || s_thr > 0.0f)) fprintf(stderr, "[aktune] BSI_AK_VOLTHR %.1f is outside -96.3..0: Wwise ignores it\n", s_thr);
}
/* 1 = some switch is set: rt_linux.c ft_init then keeps 0xC22160 / 0xF6A4D0 off the flat call table */
int wwise_tune_enabled(void){ pthread_once(&wt_once, wt_read_env); return s_on; }

static double wt_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void *wt_logger(void *arg){
  (void)arg;
  double t0 = wt_now(), last = t0; uint32_t pk_phys = 0, pk_tot = 0; double sum = 0; unsigned n = 0;
  for(;;){
    usleep(250000);
    uint32_t tot = R32(WT_N_TOTAL), virt = R32(WT_N_VIRTUAL), phys = tot >= virt ? tot - virt : 0;
    if(phys > pk_phys) pk_phys = phys;
    if(tot > pk_tot) pk_tot = tot;
    sum += phys; n++;
    double t = wt_now();
    if(t - last >= s_log_sec){
      fprintf(stderr, "[aktune] voices t=%.0f s: now total %u virtual %u physical %u | last %.0f s: peak physical %u, peak total %u, mean physical %.1f | limit %u\n",
              t - t0, tot, virt, phys, t - last, pk_phys, pk_tot, n ? sum / n : 0.0, (unsigned)R16(WT_LIMIT));
      __atomic_add_fetch(&wt_reports, 1, __ATOMIC_RELAXED);
      last = t; pk_phys = pk_tot = 0; sum = 0; n = 0;
    }
  }
  return 0;
}
static void wt_start_logger(void){
  static int started;
  if(!s_log || __atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
  pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&th, &a, wt_logger, 0)) fprintf(stderr, "[aktune] voice logger thread failed\n");
#if defined(__linux__)
  else pthread_setname_np(th, "AkVoiceLog");
#endif
  pthread_attr_destroy(&a);
}
/* cdecl guest call with one stack argument; the caller restores the CPU state */
static uint32_t wt_call1(CPU *c, uint32_t fn, uint32_t arg){ PUSH32(c, arg); guest_call(c, fn); c->esp += 4; return c->eax; }

/* from patches.c patch_call: 1 = handled (the original already ran), 0 = run the original as usual */
int wwise_tune_call(CPU *c, uint32_t t){
  if(!wwise_tune_enabled()) return 0;
  if(t == WT_DEV_INIT){
    static __thread int in;
    if(in) return 0;
    in = 1; call_indirect(c, t); in = 0;              /* the original vf036 (pops its return address) */
    __atomic_add_fetch(&wt_dev_hooks, 1, __ATOMIC_RELAXED);
    if(c->eax != 1) return 1;
    CPU s = *c;                                        /* the state the original returned with */
    if(s_voices){
      uint32_t r = wt_call1(c, WT_SET_VOICES, (uint32_t)s_voices);
      fprintf(stderr, "[aktune] SetMaxNumVoicesLimit(%d) = %u -> limit %u\n", s_voices, r, (unsigned)R16(WT_LIMIT));
    }
    if(s_has_thr){
      uint32_t b; memcpy(&b, &s_thr, 4);
      wt_call1(c, WT_SET_VOLTHR, b);
      fprintf(stderr, "[aktune] SetVolumeThreshold(%.1f dB)\n", s_thr);
    }
    *c = s;
    wt_start_logger();
    return 1;
  }
  if(t == WT_RV_INIT){
    uint32_t p = R32(c->esp + 0xC);
    __atomic_add_fetch(&wt_rv_hooks, 1, __ATOMIC_RELAXED);
    if(!p) return 0;
    uint32_t u0 = R32(p + 0x70), e0 = R8(p + 0x95), u1 = u0, e1 = e0;
    if(s_units && u0 > (uint32_t)s_units){ u1 = (uint32_t)s_units; W32(p + 0x70, u1); }
    if(s_er == 0 && e0){ e1 = 0; W8(p + 0x95, 0); }
    if(u1 != u0 || e1 != e0){
      unsigned k = __atomic_add_fetch(&wt_rv_changed, 1, __ATOMIC_RELAXED);
      if(k <= 8 || k % 100 == 0) fprintf(stderr, "[aktune] RoomVerb Init #%u (fx %08x): units %u -> %u, early reflections %u -> %u\n",
                                         k, c->ecx, u0, u1, e0, e1);
    }
    return 0;                                          /* the original Init runs with the edited params */
  }
  return 0;
}
