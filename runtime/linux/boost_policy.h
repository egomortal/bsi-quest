/* boost_policy.h -- Step B (ROADMAP item 12): when to ask the Quest for CPU Boost (XR_PERF_SETTINGS_LEVEL_BOOST_EXT).
 * Pure logic, no OpenXR or Android dependency, so it is unit-tested on the host (regress/boost_test.c simulates the timing).
 * Used by xr_glue.c, called once per XR frame.
 *
 * Loading = no game Present for more than BP_GAP_MS (250 ms), or a loading hint from the runtime that is less than
 * BP_GAP_MS old (the game thread spinning on async file reads, see sleep0.c).  Boost while loading, with Meta's limits:
 *   - at most BP_MAX_ON_MS (45 s) in one go, then at least BP_COOLDOWN_MS (15 s) at sustained-high;
 *   - total boost time at most BP_SHARE (20 %) of the runtime so far, at every moment (never over the ratio);
 *     a new boost needs BP_MIN_BUDGET_MS (2 s) of budget, so it does not flicker on and off at the limit;
 *   - off again once Presents have been flowing for BP_EXIT_MS (500 ms) with no loading hint.
 */
#ifndef BOOST_POLICY_H
#define BOOST_POLICY_H
#include <stdint.h>

#define BP_GAP_MS        250
#define BP_EXIT_MS       500
#define BP_MAX_ON_MS     45000
#define BP_COOLDOWN_MS   15000
#define BP_SHARE_PCT     20
#define BP_MIN_BUDGET_MS 2000

enum { BP_NONE = 0, BP_START, BP_STOP_DONE, BP_STOP_45S, BP_STOP_BUDGET };
static const char *const bp_reason[] = { "none", "loading", "loading over", "45 s limit", "20% budget" };

typedef struct {
  int on;
  uint64_t t0, on_since, total_on, last_present_ms, busy_since, cooldown_until, last_tick;
  uint32_t last_presents;
  int started;
  uint32_t n_boosts;
} BoostPolicy;

/* now_ms: monotonic ms.  presents: running Present counter.  hint_ms: time of the last runtime loading hint (0 = none).
   Returns a BP_* transition code (BP_NONE = no change); p->on is the level to apply (1 = boost, 0 = sustained-high). */
static int bp_tick(BoostPolicy *p, uint64_t now_ms, uint32_t presents, uint64_t hint_ms){
  if(!p->started){ p->started = 1; p->t0 = p->last_present_ms = p->last_tick = now_ms; p->last_presents = presents; p->busy_since = 0; }
  if(p->on) p->total_on += now_ms - p->last_tick;
  p->last_tick = now_ms;
  if(presents != p->last_presents){ p->last_presents = presents; p->last_present_ms = now_ms; }
  int hint = hint_ms && now_ms - hint_ms < BP_GAP_MS;
  int loading = now_ms - p->last_present_ms > BP_GAP_MS || hint;
  /* "rendering normally" streak: presents flowing and no hint */
  if(loading) p->busy_since = 0; else if(!p->busy_since) p->busy_since = now_ms;
  uint64_t allowed = (now_ms - p->t0) * BP_SHARE_PCT / 100;
  if(p->on){
    if(p->total_on >= allowed){ p->on = 0; return BP_STOP_BUDGET; }
    if(now_ms - p->on_since >= BP_MAX_ON_MS){ p->on = 0; p->cooldown_until = now_ms + BP_COOLDOWN_MS; return BP_STOP_45S; }
    if(!loading && now_ms - p->busy_since >= BP_EXIT_MS){ p->on = 0; return BP_STOP_DONE; }
    return BP_NONE;
  }
  if(loading && now_ms >= p->cooldown_until && allowed >= p->total_on + BP_MIN_BUDGET_MS){ p->on = 1; p->on_since = now_ms; p->n_boosts++; return BP_START; }
  return BP_NONE;
}
#endif
