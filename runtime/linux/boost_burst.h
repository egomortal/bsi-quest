/* boost_burst.h -- HITCHES.md item H2: CPU Boost during streaming hitch bursts (proposal, not linked anywhere yet).
 *
 * Wraps linux/boost_policy.h without changing it.  The Step B policy boosts only while no frame is presented for 250 ms
 * (loading screens) or while the game thread spins on async reads.  In the v0.8.6 Quest run (quest_logs/v086_run) it fired
 * once, at startup, while 86 of the 98 frames over 100 ms came in streaming bursts where the game keeps presenting at
 * 8-10 fps (100-130 ms frames for 3-25 s).  Those frames are CPU-side ("pre" stage, Present wait ~0), so a higher CPU clock
 * shortens them; the GPU-bound slow frames of heavy Ultra scenes (50-90 ms, Present wait dominant) must NOT trigger it.
 *
 * Detection, from the Present counter only (the XR thread sees it at ~120 Hz):
 *   slow frame  = more than BB_SLOW_MS between two Present-counter changes;
 *   burst start = at least BB_MIN_SLOW slow frames within the last BB_WINDOW_MS;
 *   burst end   = no slow frame for BB_HOLD_MS.
 * While a burst is active, bb_tick() feeds bp_tick() a fresh loading hint, so all of Meta's limits (45 s max, 15 s
 * cooldown, 20 % of runtime, 2 s minimum budget) still come from boost_policy.h unchanged.
 * Host test: regress/hitch/boost_burst_test.c (also replays a timeline.csv). */
#ifndef BOOST_BURST_H
#define BOOST_BURST_H
#include "boost_policy.h"

#define BB_SLOW_MS    90
#define BB_MIN_SLOW   3
#define BB_WINDOW_MS  2000
#define BB_HOLD_MS    3000

typedef struct {
  uint64_t slow[8];          /* times of the last slow frames (ring) */
  unsigned nslow;
  uint64_t last_change_ms;   /* last time the Present counter changed */
  uint32_t last_presents;
  uint64_t last_slow_ms;
  int started, active;
  uint32_t n_bursts;
} BurstBoost;

static int bb_tick(BurstBoost *b, BoostPolicy *p, uint64_t now_ms, uint32_t presents, uint64_t hint_ms, int enabled){
  if(!b->started){ b->started = 1; b->last_change_ms = now_ms; b->last_presents = presents; }
  if(presents != b->last_presents){
    uint64_t gap = now_ms - b->last_change_ms;
    /* gaps above the loading threshold are loading screens, already handled by boost_policy.h */
    if(gap > BB_SLOW_MS && gap <= 2000){ b->slow[b->nslow++ % 8] = now_ms; b->last_slow_ms = now_ms; }
    b->last_presents = presents; b->last_change_ms = now_ms;
  }
  if(enabled){
    unsigned n = 0, k = b->nslow < 8 ? b->nslow : 8;
    for(unsigned i = 0; i < k; i++) if(now_ms - b->slow[i] <= BB_WINDOW_MS) n++;
    if(!b->active && n >= BB_MIN_SLOW){ b->active = 1; b->n_bursts++; }
    else if(b->active && now_ms - b->last_slow_ms >= BB_HOLD_MS) b->active = 0;
    if(b->active && (!hint_ms || hint_ms < now_ms)) hint_ms = now_ms;
  } else b->active = 0;
  return bp_tick(p, now_ms, presents, hint_ms);
}
#endif
