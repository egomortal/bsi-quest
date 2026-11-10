/* sleep0.c -- Step B (ROADMAP item 11): adaptive Sleep(0).  Included by hle_k32.c (no build-script change).
 *
 * Win32 Sleep(0) is a yield.  UE3 spin-waits with appSleep(0) (f_00491bf0) in a few places: the game thread waiting for the
 * rendering thread (f_005848xx), for async file reads (f_004eba70, loading) and the rendering thread polling GPU queries
 * (f_0088bbxx, ID3D11DeviceContext::GetData).  A spinning thread keeps a core busy that the thread it waits for (or the
 * DXVK / audio threads) could use; on the Quest the app only has 3 cores.
 * BSI_SLEEP0=<mode> backs off long spins.  A streak is a run of Sleep(0) calls that come back to back (each less than
 * BSI_S0_GAP_US, default 200 us, after the previous one returned, i.e. no real work in between); a longer gap or a
 * blocking wait (s0_note_wait(): hle_more.c ev_wait, Sleep(n>0)) ends it.  The first BSI_S0_N calls (32) and the first
 * BSI_S0_SPIN_US (200 us) of a streak stay plain sched_yield, so short waits keep their latency; after that the thread
 * sleeps 20 us per call, 50 us once the streak is 1 ms old, 100 us after 5 ms, 500 us after 20 ms and 1 ms after 50 ms,
 * capped at BSI_S0_MAXUS (100 us) for the GameThread and BSI_S0_MAXUS_BG (1000 us) for other threads.
 *   mode 2 (default): only threads other than the RenderingThread and the GameThread (main thread).  On the Quest an
 *          unnamed guest thread wakes ~7000x/s from Sleep(0) (v0.8.4e perfetto); in the cloud no such thread exists, so
 *          mode 2 changes nothing there (validated neutral with compare.sh, see OVERNIGHT.md Step B).
 *   mode 1: also the GameThread (it spin-waits for the rendering thread and for async reads while loading)
 *   0: off (plain sched_yield, the old behaviour).  BSI_S0_ALL=1: no exemptions (testing only)
 * The RenderingThread is never slowed: its GPU-query polls are short (< 1 ms) and on the critical path.
 * BSI_SLEEP0_STAT=1 prints a [sleep0] line per thread every 10 s: calls/s, spin and sleep time, streaks, gap buckets and
 * top callers (appSleep's caller for calls through appSleep).
 * Cost per Sleep(0) when no backoff applies: two clock reads and a few compares.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/prctl.h>

static int s0_mode = -1, s0_stat, s0_n = 32, s0_gap_ns = 200000, s0_spin_ns = 200000, s0_maxus = 100, s0_maxus_bg = 1000, s0_all;
static __thread uint32_t s0_streak, s0_exempt_chk;  /* exempt_chk: 0 unknown, else thread class + 1 (s0_class) */
static __thread uint64_t s0_last, s0_exempt_t, s0_t_start;   /* t_start: when the current streak began */
static __thread uint32_t s0_waits;                   /* bumped by s0_note_wait() */
static __thread uint32_t s0_waits_seen;

void s0_note_wait(void){ s0_waits++; }
/* loading hint for the CPU Boost policy (xr_glue.c, boost_policy.h): CLOCK_MONOTONIC ms of the last Sleep(0) from the
   game thread's async-read wait loop (f_004eba70, FArchiveAsync precache), i.e. the game is blocked on file I/O. */
volatile uint64_t g_s0_hint_ms;

static void s0_init(void){
  const char *e = getenv("BSI_SLEEP0"); s0_mode = e ? atoi(e) : 2;   /* default: mode 2 (background threads only) */
  s0_stat = getenv("BSI_SLEEP0_STAT") ? atoi(getenv("BSI_SLEEP0_STAT")) : 0;
  if((e = getenv("BSI_S0_N"))) s0_n = atoi(e);
  if((e = getenv("BSI_S0_GAP_US"))) s0_gap_ns = atoi(e) * 1000;
  if((e = getenv("BSI_S0_MAXUS"))) s0_maxus = atoi(e);
  if((e = getenv("BSI_S0_MAXUS_BG"))) s0_maxus_bg = atoi(e);
  if((e = getenv("BSI_S0_SPIN_US"))) s0_spin_ns = atoi(e) * 1000;
  s0_all = getenv("BSI_S0_ALL") ? atoi(getenv("BSI_S0_ALL")) : 0;
  if(s0_mode || s0_stat) fprintf(stderr, "[sleep0] mode %d stat %d n %d spin %d us gap %d us max %d us (background %d us) all %d\n", s0_mode, s0_stat, s0_n, s0_spin_ns / 1000, s0_gap_ns / 1000, s0_maxus, s0_maxus_bg, s0_all);
}

/* pure policy (unit-tested in regress/sleep0_test.c): sleep length in us for a Sleep(0) that is the streak-th call of a
   streak that has been spinning for spun_ns, 0 = plain yield */
static int s0_policy_us(uint32_t streak, uint64_t spun_ns, int n, int spin_ns, int maxus){
  if((int)streak <= n || spun_ns < (uint64_t)spin_ns) return 0;
  int us = spun_ns < 1000000 ? 20 : spun_ns < 5000000 ? 50 : spun_ns < 20000000 ? 100 : spun_ns < 50000000 ? 500 : 1000;
  return us < maxus ? us : maxus;
}

extern __thread uint32_t t_tid;
/* thread class for the policy: 0 = never slowed (RenderingThread), 1 = GameThread / main thread (cap BSI_S0_MAXUS, mode 1
   only), 2 = any other thread (cap BSI_S0_MAXUS_BG).  Names can change after a thread starts: re-checked every 1 s. */
static int s0_class(uint64_t now){
  if(s0_exempt_chk && now - s0_exempt_t < 1000000000ull) return s0_exempt_chk - 1;
  char nm[32] = {0}; prctl(PR_GET_NAME, nm, 0, 0, 0);   /* new threads inherit the creator's name, so only explicit names count */
  int cl = strstr(nm, "Rendering") ? (s0_all ? 1 : 0) : (!t_tid || t_tid == 0x1004 || strstr(nm, "GameThread")) ? 1 : 2;
  s0_exempt_chk = cl + 1; s0_exempt_t = now; return cl;
}
/* sleep length for this call: policy plus thread class and mode */
static int s0_sleep_us(uint32_t streak, uint64_t spun_ns, uint64_t now){
  if((int)streak <= s0_n || spun_ns < (uint64_t)s0_spin_ns) return 0;   /* cheap pre-check before the class lookup */
  int cl = s0_class(now);
  if(cl == 0 || (cl == 1 && s0_mode != 1 && !s0_all)) return 0;
  return s0_policy_us(streak, spun_ns, s0_n, s0_spin_ns, cl == 1 ? s0_maxus : s0_maxus_bg);
}

/* ---- statistics (BSI_SLEEP0_STAT) ---- */
static __thread struct { uint64_t t0, calls, slept, streaks, streak_sum, streak_max, gap[5], sleep_ns, spin_ns; uint32_t ca[4], cn[4]; } s0s;
static void s0_stat_note(uint64_t now, uint64_t gap, uint32_t ret, int newstreak){
  if(!s0s.t0) s0s.t0 = now;
  s0s.calls++; if(!newstreak) s0s.spin_ns += gap;
  int b = gap < 5000 ? 0 : gap < 20000 ? 1 : gap < 100000 ? 2 : gap < 1000000 ? 3 : 4; s0s.gap[b]++;
  if(newstreak){ s0s.streaks++; }
  if(s0_streak > s0s.streak_max) s0s.streak_max = s0_streak;
  int i; for(i = 0; i < 4; i++) if(s0s.ca[i] == ret){ s0s.cn[i]++; break; }
  if(i == 4){ int m = 0; for(i = 1; i < 4; i++) if(s0s.cn[i] < s0s.cn[m]) m = i; s0s.ca[m] = ret; s0s.cn[m] = 1; }
  if(now - s0s.t0 >= 10000000000ull){
    double dt = (now - s0s.t0) / 1e9; char nm[32] = {0}; prctl(PR_GET_NAME, nm, 0, 0, 0);
    fprintf(stderr, "[sleep0] tid %04x '%s' %.0f calls/s, slept %.0f/s (%.1f ms/s), spin %.1f ms/s, streaks %.0f/s mean %.1f max %llu, gap<5us %llu <20us %llu <100us %llu <1ms %llu >1ms %llu, callers %08x:%u %08x:%u %08x:%u %08x:%u\n",
      t_tid, nm, s0s.calls / dt, s0s.slept / dt, s0s.sleep_ns / dt / 1e6, s0s.spin_ns / dt / 1e6, s0s.streaks / dt, s0s.streaks ? (double)s0s.calls / s0s.streaks : 0.0, (unsigned long long)s0s.streak_max,
      (unsigned long long)s0s.gap[0], (unsigned long long)s0s.gap[1], (unsigned long long)s0s.gap[2], (unsigned long long)s0s.gap[3], (unsigned long long)s0s.gap[4],
      s0s.ca[0], s0s.cn[0], s0s.ca[1], s0s.cn[1], s0s.ca[2], s0s.cn[2], s0s.ca[3], s0s.cn[3]);
    memset(&s0s, 0, sizeof s0s); s0s.t0 = now;
  }
}

static uint64_t s0_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static void sleep0(uint32_t ret, uint32_t ret2){   /* ret2: appSleep's caller when ret is inside appSleep (0x491c22) */
  if(s0_mode < 0) s0_init();
  if(ret == 0x4ebabbu) g_s0_hint_ms = s0_now() / 1000000;   /* FArchiveAsync precache spin: the game thread waits for file data */
  if(!s0_mode && !s0_stat){ sched_yield(); return; }
  uint64_t now = s0_now(), gap = s0_last ? now - s0_last : ~0ull;
  int newstreak = gap > (uint64_t)s0_gap_ns || s0_waits != s0_waits_seen;
  if(newstreak){ s0_streak = 0; s0_waits_seen = s0_waits; s0_t_start = now; }
  s0_streak++;
  int us = s0_mode ? s0_sleep_us(s0_streak, now - s0_t_start, now) : 0;
  if(s0_stat) s0_stat_note(now, gap, ret == 0x491c22u ? ret2 : ret, newstreak);
  if(us){ struct timespec ts = {0, us * 1000}; nanosleep(&ts, 0); }
  else sched_yield();
  s0_last = s0_now();
  if(s0_stat){ s0s.spin_ns += s0_last - now; if(us){ s0s.slept++; s0s.sleep_ns += s0_last - now; } }
}
