/* evlog.c -- Step D (OVERNIGHT.md): event/wait trace for startup-hang hunting.  #included by hle_more.c after the Ev
 * table (no build-script change).  Off unless BSI_EVLOG is set; then every Create/Set/Reset/Close of an event,
 * semaphore, mutex or thread handle and every WaitForSingleObject / WaitForMultipleObjects entry and exit is recorded
 * in a ring buffer (1<<17 records) plus a per-slot history (last EVL_HIST ops of each handle slot, so the history of a
 * hung handle survives the ring wrapping).  Each record has a CLOCK_MONOTONIC timestamp, the Win32 thread id, the
 * handle, the op, the guest return address of the API call and the first code address above it on the guest stack
 * (normally the caller of the FEvent wrapper).
 *   BSI_EVLOG=1          dump to stderr (last BSI_EVLOG_TAIL ring records, default 3000)
 *   BSI_EVLOG=<path>     dump to that file (appended; the whole ring)
 *   BSI_EVLOG_HANG=<s>   watchdog: dump when the GameThread has been in one INFINITE (or >= 10 s) wait for <s> seconds
 *                        (default 20; 0 = no watchdog).  One dump per stuck wait.
 *   SIGUSR2              dump now (e.g. kill -USR2 <pid> from a harness).  ExitProcess also dumps.
 * Cost when off: one predictable branch per sync call.  When on: ~50-100 ns per call (clock read + two stores).
 */
#include <signal.h>
#include <sys/syscall.h>

enum { EVO_NEW=1, EVO_CLOSE, EVO_SET, EVO_RESET, EVO_WAIT, EVO_WAITED, EVO_WFMO, EVO_WFMOED, EVO_SEMREL, EVO_MTXREL,
       EVO_THREXIT, EVO_RESUME, EVO_STALE_SET, EVO_STALE_RESET, EVO_STALE_WAIT, EVO_STALE_CLOSE };
static const char *evo_name[] = { "?", "new", "close", "set", "reset", "wait", "waited", "wfmo", "wfmo-ret", "semrel",
  "mtxrel", "threxit", "resume", "STALE-set", "STALE-reset", "STALE-wait", "STALE-close" };
typedef struct { uint64_t t; uint32_t h, ra, ra2, tid, arg; uint8_t op, kind, st, res; } EvRec;
#define EVL_RING (1u<<17)
#define EVL_HIST 16
static int g_evl = -1;                   /* -1 unknown, 0 off, 1 on */
static EvRec *g_evr; static unsigned g_evr_n;
static EvRec (*g_evh)[EVL_HIST]; static uint8_t g_evh_i[NEV];
static const char *g_evl_path; static int g_evl_tail = 3000, g_evl_hang = 20;
static volatile sig_atomic_t g_evl_sig;
/* threads currently in a blocking wait (for the watchdog and the dump) */
#define EVL_NT 512
static struct { uint32_t tid; int ktid; volatile int active; uint32_t h[4]; int n; uint32_t ms, ra, ra2; uint64_t since; } g_wt[EVL_NT];
static int g_wt_n; static __thread int t_wi = -1;

static uint64_t evl_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000u+ts.tv_nsec/1000; }
static void evl_dump(const char *why);
static void evl_sigusr2(int s){ (void)s; g_evl_sig = 1; }
static void *evl_watchdog(void *p){ (void)p; uint64_t dumped_since = 0; uint32_t dumped_tid = ~0u;
  pthread_setname_np(pthread_self(), "BSI-evlog");
  for(;;){ sleep(1);
    if(g_evl_sig){ g_evl_sig = 0; evl_dump("SIGUSR2"); }
    if(!g_evl_hang) continue;
    uint64_t now = evl_now();
    for(int i = 0; i < g_wt_n; i++){ if(!g_wt[i].active) continue;
      uint32_t tid = g_wt[i].tid; if(tid && tid != 0x1004) continue;           /* GameThread only */
      if(g_wt[i].ms < 10000) continue;
      if(now - g_wt[i].since < (uint64_t)g_evl_hang*1000000u) continue;
      if(dumped_since == g_wt[i].since && dumped_tid == tid) continue;
      dumped_since = g_wt[i].since; dumped_tid = tid;
      char b[96]; snprintf(b, sizeof b, "HANG: GameThread in one wait for %.1f s", (now - g_wt[i].since)/1e6); evl_dump(b); } } return 0; }
static int evl_on(void){
  if(__builtin_expect(g_evl >= 0, 1)) return g_evl;
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&m);
  if(g_evl < 0){ const char *e = getenv("BSI_EVLOG"); int on = e && *e && strcmp(e,"0");
    if(on){ g_evr = calloc(EVL_RING, sizeof(EvRec)); g_evh = calloc(NEV, sizeof *g_evh); if(!g_evr || !g_evh) on = 0; }
    if(on){ if(strcmp(e,"1")) g_evl_path = e;
      const char *t = getenv("BSI_EVLOG_TAIL"); if(t) g_evl_tail = atoi(t);
      t = getenv("BSI_EVLOG_HANG"); if(t) g_evl_hang = atoi(t);
      struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler = evl_sigusr2; sa.sa_flags = SA_RESTART; sigaction(SIGUSR2,&sa,0);
      pthread_t th; pthread_create(&th, 0, evl_watchdog, 0); pthread_detach(th);
      fprintf(stderr, "[evlog] on: ring %u records, per-slot history %d, hang watchdog %d s, dump to %s\n", EVL_RING, EVL_HIST, g_evl_hang, g_evl_path ? g_evl_path : "stderr"); }
    __atomic_store_n(&g_evl, on, __ATOMIC_RELEASE); }
  pthread_mutex_unlock(&m); return g_evl; }
/* guest caller: return address of the API call, and the first code address above it on the stack */
static void evl_ra(uint32_t *ra, uint32_t *ra2){ extern __thread CPU *t_cpu; CPU *c = t_cpu; *ra = *ra2 = 0; if(!c || !c->esp) return;
  *ra = R32(c->esp); for(uint32_t a = c->esp+4; a < c->esp+0x80; a += 4){ uint32_t v = R32(a); if(v >= 0x401000u && v < 0x10d4000u){ *ra2 = v; break; } } }
static void evl_rec(int op, uint32_t h, Ev *e, uint32_t arg, int res){
  if(!evl_on()) return;
  EvRec r; r.t = evl_now(); r.h = h; evl_ra(&r.ra, &r.ra2); r.tid = t_tid; r.arg = arg; r.op = op; r.res = (uint8_t)res;
  r.kind = e ? e->kind : 0xff; r.st = e ? (e->kind==1 ? (uint8_t)e->count : e->kind==3 ? (uint8_t)e->count : (uint8_t)(e->state | e->manual<<1)) : 0xff;
  unsigned i = __atomic_fetch_add(&g_evr_n, 1, __ATOMIC_RELAXED); g_evr[i & (EVL_RING-1)] = r;
  if(h >= 0x40000u && h < 0x40000u+4*NEV){ unsigned k = (h-0x40000u)/4; unsigned j = __atomic_fetch_add(&g_evh_i[k], 1, __ATOMIC_RELAXED); g_evh[k][j % EVL_HIST] = r; } }
static void evl_wait_begin(const uint32_t *hs, int n, uint32_t ms){
  if(!evl_on() || !ms) return;
  if(t_wi < 0){ int i = __atomic_fetch_add(&g_wt_n, 1, __ATOMIC_RELAXED); if(i >= EVL_NT){ g_wt_n = EVL_NT; t_wi = EVL_NT; return; }
    t_wi = i; g_wt[i].ktid = (int)syscall(SYS_gettid); }
  if(t_wi >= EVL_NT) return;
  int i = t_wi; g_wt[i].tid = t_tid; g_wt[i].n = n > 4 ? 4 : n; for(int k = 0; k < g_wt[i].n; k++) g_wt[i].h[k] = hs[k];
  g_wt[i].ms = ms; evl_ra(&g_wt[i].ra, &g_wt[i].ra2); g_wt[i].since = evl_now(); __atomic_store_n(&g_wt[i].active, 1, __ATOMIC_RELEASE); }
static void evl_wait_end(void){ if(t_wi >= 0 && t_wi < EVL_NT) __atomic_store_n(&g_wt[t_wi].active, 0, __ATOMIC_RELEASE); }
static void evl_pr(FILE *f, const EvRec *r, uint64_t t0){
  fprintf(f, "  %10.6f %-11s tid %04x h %05x kind %d st %d arg %08x res %3d ra %08x ra2 %08x\n", (r->t - t0)/1e6, r->op < sizeof evo_name/sizeof *evo_name ? evo_name[r->op] : "?",
    r->tid, r->h, r->kind == 0xff ? -1 : r->kind, r->st == 0xff ? -1 : r->st, r->arg, r->res, r->ra, r->ra2); }
static void evl_slot(FILE *f, uint32_t h, uint64_t t0){
  if(h < 0x40000u || h >= 0x40000u+4*NEV){ fprintf(f, "  (handle %08x outside the event table)\n", h); return; }
  unsigned k = (h-0x40000u)/4; Ev *e = &EV[k];
  fprintf(f, "[evlog] slot %u (h %05x): used %d kind %d manual %d state %d count %d max %d owner %04x waiters %d; last ops:\n", k, h, e->used, e->kind, e->manual, e->state, e->count, e->max, e->owner, e->waiters);
  unsigned j = g_evh_i[k]; for(unsigned x = 0; x < EVL_HIST; x++){ const EvRec *r = &g_evh[k][(j + x) % EVL_HIST]; if(r->op) evl_pr(f, r, t0); } }
static void evl_dump(const char *why){
  if(g_evl != 1) return;
  static pthread_mutex_t dm = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&dm);
  FILE *f = g_evl_path ? fopen(g_evl_path, "a") : stderr; if(!f) f = stderr;
  uint64_t now = evl_now(), t0 = 0; unsigned n = g_evr_n; unsigned first = n > EVL_RING ? n - EVL_RING : 0; t0 = g_evr[first & (EVL_RING-1)].t;
  fprintf(f, "[evlog] ===== DUMP (%s): %u ops recorded, times relative to op #%u =====\n", why, n, first);
  fprintf(f, "[evlog] threads in a blocking wait now:\n");
  for(int i = 0; i < g_wt_n && i < EVL_NT; i++){ if(!g_wt[i].active) continue;
    char nm[32] = "?", p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", g_wt[i].ktid); FILE *cf = fopen(p, "r"); if(cf){ if(fgets(nm, sizeof nm, cf)) nm[strcspn(nm,"\n")] = 0; fclose(cf); }
    fprintf(f, "[evlog]   tid %04x (%s) for %.2f s, ms %u, ra %08x ra2 %08x, handles", g_wt[i].tid, nm, (now - g_wt[i].since)/1e6, g_wt[i].ms, g_wt[i].ra, g_wt[i].ra2);
    for(int k = 0; k < g_wt[i].n; k++) fprintf(f, " %05x", g_wt[i].h[k]); fprintf(f, "\n"); }
  for(int i = 0; i < g_wt_n && i < EVL_NT; i++){ if(!g_wt[i].active || g_wt[i].ms < 10000) continue;
    for(int k = 0; k < g_wt[i].n; k++) evl_slot(f, g_wt[i].h[k], t0); }
  unsigned tail = g_evl_path ? EVL_RING : (unsigned)g_evl_tail; unsigned s = n - first > tail ? n - tail : first;
  fprintf(f, "[evlog] last %u ops:\n", n - s);
  for(unsigned i = s; i < n; i++) evl_pr(f, &g_evr[i & (EVL_RING-1)], t0);
  fprintf(f, "[evlog] ===== END DUMP =====\n"); fflush(f); if(f != stderr) fclose(f);
  pthread_mutex_unlock(&dm); }
