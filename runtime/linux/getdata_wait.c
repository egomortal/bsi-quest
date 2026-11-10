/* getdata_wait.c -- BSI_GETDATA_WAIT: block instead of spinning when the guest polls a D3D11 query (ROADMAP item 11a).
 * Included at the end of bridge.c (regress/getdata_wait.patch); provides the hand-written bridge for
 * ID3D11DeviceContext::GetData (listed in gen_bridge.py MANUAL by the same patch).  Notes: notes/GETDATA_SPIN.md.
 *
 * What the guest does (BioShockInfinite.exe, UE3 D3D11 RHI):
 *   FD3D11DynamicRHI::GetQueryData (0x88bad0, poll at 0x88bb44, appSleep(0) returns to 0x88bb7a) calls
 *   GetData(query, data, size, 0) and, while it returns S_FALSE and bWait is set, loops QPC + GetData + appSleep(0) with a
 *   0.5 s QPC timeout.  Callers: GetOcclusionQueryResult (RHI slot 0xE4 = 0x88eed0; per-primitive occlusion reads at
 *   0x665640 / 0x81d6d0 and two more at 0x712190 / 0x712220, all bWait=1) and RHIEndDrawingViewport (0x88fe90): after
 *   Present it waits on the frame event query (D3D11_QUERY_EVENT) issued one frame earlier = "wait until the GPU finished
 *   the previous frame".  The GPU frame timer (FD3D11BufferedGPUTiming::GetTiming 0x88c0f0) first probes with
 *   D3D11_ASYNC_GETDATA_DONOTFLUSH and, when its ring is full, busy-loops GetData(flags 0) + QPC without any Sleep.
 *   Every iteration costs a bridge round trip, ~3 clock_gettime (QPC, sleep0 bookkeeping) and a sched_yield syscall.
 *
 * Policy (BSI_GETDATA_WAIT=<budget us>, 0/unset = off = exactly the generated bridge):
 *   - a GetData that returns S_FALSE without DONOTFLUSH starts or continues a "streak" on this thread; it continues when
 *     it polls the same query as the previous S_FALSE less than BSI_GETDATA_GAP_US (2000) ago;
 *   - from the 2nd poll of a streak on (the guest is clearly waiting; a one-off probe is never delayed) the call sleeps
 *     and re-polls inside the host until the query is done or the per-call budget is used up (then it returns S_FALSE and
 *     the guest polls again, so its 0.5 s timeout still works);
 *   - sleep schedule: step = time waited so far / 4, clamped to [BSI_GETDATA_MIN_US (50), BSI_GETDATA_STEP_US (200)], so
 *     a wait ends at most one step (<= 200 us + timer slack; <= 50 us for short waits) after the query completes, with
 *     ~5 wake-ups per ms of waiting instead of hundreds of spin iterations.  Optional predictor (BSI_GETDATA_PRED_PCT, default 0 = off; e.g. 50): the first
 *     sleep of a call is that % of min(EMA, last) of the previous complete waits of this query class (event / occlusion /
 *     timestamp / other, per thread), capped at BSI_GETDATA_PRED_MAX_US (4000).  Fewer wake-ups, but when a wait suddenly
 *     drops below that % of the previous one the RT wakes late once (bounded by the cap): off by default.
 *   - DONOTFLUSH polls are never delayed (the GPU timer's non-blocking probes).
 * BSI_GETDATA_WAIT=1 means the default budget (4000 us).  BSI_GETDATA_STAT=1 (also works with the wait off) prints a
 * [getdata] line per thread every 10 s: polls, S_FALSE by query class, time spent waiting, blocks, wake-ups, budget-outs.
 * Cost when off and stat off: one int test per GetData.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>

#define GW_S_OK 0
#define GW_S_FALSE 1
#define GW_DONOTFLUSH 1u
enum { GW_T_EVENT, GW_T_OCC, GW_T_TS, GW_T_OTHER, GW_NT };   /* query classes for the predictor and the stats */

typedef struct { uint32_t budget_us, gap_us, min_us, step_us, pred_pct, pred_max_us; } GwCfg;
typedef struct { uint64_t ema_ns, last_ns; } GwPred;           /* past complete waits of one query class */
typedef struct {                                                /* per-thread state */
  const void *lastq;                 /* query of the current streak (host pointer), 0 = none */
  uint64_t last_t, t_streak;         /* last S_FALSE of the streak returned at; streak began at */
  uint32_t streak, cls;
  GwPred pred[GW_NT];
} GwThr;
typedef struct { uint64_t t0, calls, fals[GW_NT], wait_ns[GW_NT], waits[GW_NT], blocks, block_ns, wakes, outs, max_ns, lastsleep_ns, dnf; } GwStat;
typedef struct {
  int (*poll)(void *u);                        /* re-poll the query (DONOTFLUSH) */
  uint64_t (*now)(void *u);                    /* CLOCK_MONOTONIC ns */
  void (*sleep)(void *u, uint64_t ns);
  uint32_t (*qtype)(void *u, const void *q);   /* D3D11_QUERY of q */
  void *u;
} GwOps;

/* ---- pure policy (unit-tested in regress/getdata_wait_test.c) ---- */

/* D3D11_QUERY -> class (EVENT 0, OCCLUSION 1, TIMESTAMP 2, TIMESTAMP_DISJOINT 3, OCCLUSION_PREDICATE 5) */
static int gw_class(uint32_t q){ return q == 0 ? GW_T_EVENT : (q == 1 || q == 5) ? GW_T_OCC : (q == 2 || q == 3) ? GW_T_TS : GW_T_OTHER; }

/* does this S_FALSE poll continue the streak (same query, back to back)? */
static int gw_same_streak(const void *q, const void *last_q, uint64_t now, uint64_t last_t, uint32_t gap_us){
  return q && q == last_q && last_t && now >= last_t && now - last_t <= (uint64_t)gap_us * 1000u;
}

/* next sleep (ns) inside a wait that has run for waited_ns since the first S_FALSE of the streak; nwake = sleeps done so
   far in this call.  Step = waited/4 clamped to [min, step]: short waits stay precise (a 100 us wait ends <= 50 us late),
   long waits wake every `step` (200 us) at most. */
static uint64_t gw_next_sleep(const GwCfg *cf, const GwPred *p, uint64_t waited_ns, uint32_t nwake){
  uint64_t mn = (uint64_t)cf->min_us * 1000u, mx = (uint64_t)cf->step_us * 1000u;
  if(nwake == 0 && cf->pred_pct && p->ema_ns){
    uint64_t base = p->last_ns && p->last_ns < p->ema_ns ? p->last_ns : p->ema_ns;   /* react at once to shorter waits */
    uint64_t target = base * cf->pred_pct / 100u, cap = (uint64_t)cf->pred_max_us * 1000u;
    if(target > cap) target = cap;
    if(target > waited_ns + mn) return target - waited_ns;
  }
  uint64_t s = waited_ns / 4;
  return s < mn ? mn : s > mx ? mx : s;
}

/* a wait finished after total_ns (first S_FALSE of the streak -> done) */
static void gw_learn(GwPred *p, uint64_t total_ns){
  p->ema_ns = p->ema_ns ? (p->ema_ns * 3 + total_ns) / 4 : total_ns;
  p->last_ns = total_ns;
}

/* blocking part of one GetData call: sleep + poll until poll() != S_FALSE or budget_ns has passed in this call */
static int gw_block(const GwCfg *cf, GwPred *p, const GwOps *o, uint64_t t_streak, uint64_t budget_ns, uint32_t *nwake, uint64_t *last_sleep){
  uint64_t t0 = o->now(o->u); int r = GW_S_FALSE; *nwake = 0; *last_sleep = 0;
  for(;;){
    uint64_t now = o->now(o->u);
    if(now - t0 >= budget_ns) break;
    uint64_t s = gw_next_sleep(cf, p, now - t_streak, *nwake), left = budget_ns - (now - t0);
    if(s > left) s = left;
    o->sleep(o->u, s); (*nwake)++; *last_sleep = s;
    r = o->poll(o->u);
    if(r != GW_S_FALSE) break;
  }
  return r;
}

/* a streak (wait) ended at `now`: done = the query completed (learn the wait); otherwise it was abandoned (another
   query was polled, or the guest stopped polling) */
static void gw_streak_end(GwThr *t, GwStat *st, uint64_t now, int done){
  uint64_t tot = now - t->t_streak;
  if(done) gw_learn(&t->pred[t->cls], tot);
  if(st){ st->waits[t->cls]++; st->wait_ns[t->cls] += tot; if(tot > st->max_ns) st->max_ns = tot; }
  t->lastq = 0; t->streak = 0;
}

/* everything after the guest's GetData reached the host: r = its result, now = time it returned.  on = blocking
   allowed (BSI_GETDATA_WAIT), st = stats or 0.  Returns the result to give the guest. */
static int gw_after(GwThr *t, const GwCfg *cf, int on, GwStat *st, const void *q, uint32_t flags, int r, uint64_t now, const GwOps *o){
  if(st) st->calls++;
  if(r != GW_S_FALSE){
    if(t->streak && t->lastq == q) gw_streak_end(t, st, now, r == GW_S_OK);   /* the guest's own poll saw it complete */
    return r;
  }
  if(flags & GW_DONOTFLUSH){ if(st) st->dnf++; return r; }
  if(gw_same_streak(q, t->lastq, now, t->last_t, cf->gap_us)) t->streak++;
  else {
    if(t->streak) gw_streak_end(t, st, t->last_t, 0);
    t->lastq = q; t->streak = 1; t->t_streak = now; t->cls = (uint32_t)gw_class(o->qtype(o->u, q));
  }
  if(st) st->fals[t->cls]++;
  if(on && t->streak >= 2){
    uint32_t nw; uint64_t ls;
    r = gw_block(cf, &t->pred[t->cls], o, t->t_streak, (uint64_t)cf->budget_us * 1000u, &nw, &ls);
    uint64_t t1 = o->now(o->u);
    if(st){ st->blocks++; st->block_ns += t1 - now; st->wakes += nw; st->lastsleep_ns += ls; if(r == GW_S_FALSE) st->outs++; }
    now = t1;
    if(r != GW_S_FALSE){ gw_streak_end(t, st, now, r == GW_S_OK); return r; }
  }
  t->last_t = now;
  return r;
}

static void gw_cfg_env(GwCfg *cf){
  const char *e;
  cf->budget_us = (e = getenv("BSI_GETDATA_WAIT")) ? (uint32_t)atoi(e) : 0; if(cf->budget_us == 1) cf->budget_us = 4000;
  cf->gap_us = (e = getenv("BSI_GETDATA_GAP_US")) ? (uint32_t)atoi(e) : 2000;
  cf->min_us = (e = getenv("BSI_GETDATA_MIN_US")) ? (uint32_t)atoi(e) : 50; if(!cf->min_us) cf->min_us = 1;
  cf->step_us = (e = getenv("BSI_GETDATA_STEP_US")) ? (uint32_t)atoi(e) : 200; if(cf->step_us < cf->min_us) cf->step_us = cf->min_us;
  cf->pred_pct = (e = getenv("BSI_GETDATA_PRED_PCT")) ? (uint32_t)atoi(e) : 0; if(cf->pred_pct > 95) cf->pred_pct = 95;
  cf->pred_max_us = (e = getenv("BSI_GETDATA_PRED_MAX_US")) ? (uint32_t)atoi(e) : 4000;
}

#ifndef GW_NO_BRIDGE
/* ---- bridge glue ---- */
static int gw_on = -1, gw_stat;
static GwCfg gw_cf;
static __thread GwThr gw_t;
static __thread GwStat gws;

static uint64_t gw_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void gw_init(void){
  gw_cfg_env(&gw_cf);
  gw_stat = getenv("BSI_GETDATA_STAT") ? atoi(getenv("BSI_GETDATA_STAT")) : 0;
  gw_on = gw_cf.budget_us > 0;
  if(gw_on || gw_stat) fprintf(stderr, "[getdata] wait %s budget %u us, gap %u us, steps %u..%u us, predict %u %% (max %u us), stat %d\n",
    gw_on ? "on" : "off", gw_cf.budget_us, gw_cf.gap_us, gw_cf.min_us, gw_cf.step_us, gw_cf.pred_pct, gw_cf.pred_max_us, gw_stat);
}

typedef int32_t (*gw_getdata_fn)(void*, void*, void*, uint32_t, uint32_t);
typedef struct { gw_getdata_fn fn; void *ctx, *q, *data; uint32_t size, flags; } GwPoll;
static int gw_poll_host(void *u){ GwPoll *p = (GwPoll*)u; return p->fn(p->ctx, p->q, p->data, p->size, p->flags); }
static uint64_t gw_now_u(void *u){ (void)u; return gw_now(); }
static void gw_sleep_u(void *u, uint64_t ns){ (void)u; struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) }; nanosleep(&ts, 0); }
static uint32_t gw_qtype_u(void *u, const void *q){   /* ID3D11Query::GetDesc (slot 8; same slot and 8-byte layout on Predicate / Counter) */
  (void)u; uint32_t d[2] = {~0u, 0}; ((void(*)(const void*, void*))((*(void***)q)[8]))(q, d); return d[0];
}

static void gw_stat_tick(uint64_t now){
  if(!gws.t0){ gws.t0 = now; return; }
  if(now - gws.t0 < 10000000000ull) return;
  double dt = (now - gws.t0) / 1e9; char nm[32] = {0}; prctl(PR_GET_NAME, nm, 0, 0, 0);
  uint64_t fa = 0, wt = 0, wn = 0; for(int k = 0; k < GW_NT; k++){ fa += gws.fals[k]; wt += gws.wait_ns[k]; wn += gws.waits[k]; }
  extern __thread uint32_t t_tid;
  fprintf(stderr, "[getdata] tid %04x '%s' polls %.0f/s, S_FALSE %.0f/s (event %.0f occl %.0f ts %.0f other %.0f, dnf %.0f), waits %.1f/s,"
    " in-wait %.2f ms/s (event %.2f occl %.2f ts %.2f), blocked %.2f ms/s in %.1f blocks/s, wakes %.0f/s, last-step %.0f us, max wait %.2f ms, budget-outs %llu\n",
    t_tid, nm, gws.calls / dt, fa / dt, gws.fals[0] / dt, gws.fals[1] / dt, gws.fals[2] / dt, gws.fals[3] / dt, gws.dnf / dt, wn / dt,
    wt / dt / 1e6, gws.wait_ns[0] / dt / 1e6, gws.wait_ns[1] / dt / 1e6, gws.wait_ns[2] / dt / 1e6,
    gws.block_ns / dt / 1e6, gws.blocks / dt, gws.wakes / dt, gws.blocks ? gws.lastsleep_ns / 1e3 / gws.blocks : 0.0, gws.max_ns / 1e6,
    (unsigned long long)gws.outs);
  memset(&gws, 0, sizeof gws); gws.t0 = now;
}

void bridge_ID3D11DeviceContext_GetData(CPU*c){
  void *self = br_host(R32(c->esp+4)); if(!self){ br_null(c, "ID3D11DeviceContext::GetData", 20); return; }
  void *a0 = br_hostc(R32(c->esp+8));
  void *a1 = GP(R32(c->esp+12));
  uint32_t a2 = R32(c->esp+16);
  uint32_t a3 = R32(c->esp+20);
  gw_getdata_fn fn = (gw_getdata_fn)((*(void***)self)[29]);
  int32_t r = fn(self, a0, a1, a2, a3);
  if(gw_on < 0) gw_init();
  if((gw_on || gw_stat) && a0 && r >= 0){
    uint64_t now = gw_now();
    if(gw_stat) gw_stat_tick(now);
    GwPoll p = { fn, self, a0, a1, a2, a3 | GW_DONOTFLUSH };   /* re-polls: the guest's own poll already flushed / noted the stall */
    GwOps o = { gw_poll_host, gw_now_u, gw_sleep_u, gw_qtype_u, &p };
    r = gw_after(&gw_t, &gw_cf, gw_on, gw_stat ? &gws : 0, a0, a3, r, now, &o);
  }
  if(r < 0) br_fail("ID3D11DeviceContext::GetData", r);
  c->eax = (uint32_t)r;
  c->esp += 4+20;
}
#endif
