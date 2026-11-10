/* sched_policy.c -- BSI_SCHED: thread priority / placement policy for the app's 3 cores (Quest 3: cpu3-5).
 * Notes: notes/SCHED.md.  Integration: regress/sched/sched_policy.patch (hle_more.c includes this file at its end, like
 * hle_k32.c includes sleep0.c, so no build-script change; hle_k32.c gets two weak hooks).  Everything is off unless an
 * env variable below is set (env.txt passes BSI_*).
 *
 * Why (perfetto v0.8.4e, 30 s, live_v084e/bsi.pftrace):
 *   - every thread the game creates runs at nice -20 (kernel prio 100): the GameThread is registered with OpenXR as
 *     APPLICATION_MAIN before the game creates its threads, the runtime gives it nice -20, and Linux copies the creator's
 *     nice into every new thread.  So PoolThread, XJob, AsyncIO, Morpheme, dxvk-* and the RenderingThread all have the same
 *     weight as the GameThread: the priority hint is diluted to nothing.  (OVRInput / UI thread: -10, created elsewhere.)
 *   - the game asks for priorities itself (SetThreadPriority from FRunnableThreadWin 0x4919d0, Wwise AkCreateThread
 *     0xf82160, 0xff6290) but kernel32!SetThreadPriority is a no-op in our HLE.
 *   - cpu3-5 were all busy 25.5 % of the time; GT runnable-but-waiting 1.2 s (94 % of it with all 3 cores busy), RT 1.0 s,
 *     dxvk-cs 0.42 s.  Occupants of the core the waiting thread then ran on: XJob ~20 % (GT and RT), Pool 5-10 %, ...
 *
 * Env (all read once, at the first hook call or sched_policy_start()):
 *   BSI_SCHED=log   no changes; log per-thread CPU / run-queue wait every BSI_SCHED_LOG seconds + the game's requests
 *   BSI_SCHED=1     nice tiers by thread name (table below) + the log
 *   BSI_SCHED=2     tiers + background loaders (Pool, ShaderCreate, AsyncIO, save threads) kept on the last allowed core
 *   BSI_SCHED_RULES="Name=nice[b][@i,j];..."  extra / overriding rules (prefix match on the 15-char comm; "@game" = the
 *                   GameThread; "b" = SCHED_BATCH; "@i,j" = indices into the allowed CPU set, e.g. @2 = third app core)
 *   BSI_SCHED_WINPRI=1  also honour the game's SetThreadPriority (Win32 -15..15 -> small nice offset on top of the tier)
 *   BSI_SCHED_LOG=<s>   log period in seconds (default 10; 0 = no periodic log)
 *   BSI_SCHED_ADPF=<fps> Android ADPF hint session (GameThread + RenderingThread, target 1/fps), if the OS has it
 *   BSI_NCPU=<n>        processor count reported by GetSystemInfo (default: unchanged 4; needs the hle_k32.c hunk)
 *   BSI_SCHED_WWISE=<hex> start address of Wwise's audio thread (default fa57d0); it is named "AkAudioThread"
 * Cost: one 1 Hz monitor thread at nice 10 (reads /proc/self/task/<tid>/comm + schedstat, ~60 threads: < 1 ms/s).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#ifndef SCHED_BATCH   /* glibc hides these without _GNU_SOURCE (e.g. when a test includes <sys/mman.h> first) */
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifdef __ANDROID__
#include <dlfcn.h>
#endif

extern volatile int g_game_tid __attribute__((weak));     /* xr_glue.c (Android) */
extern volatile long g_br_presents __attribute__((weak)); /* bridge.c */

#define SP_MAXT 512
#define SP_MAXR 48
#define SP_UNSET 99
typedef struct { char pat[16]; int plen, nice, batch; uint32_t cpus; } SpRule;   /* cpus: bit i = i-th allowed CPU */
typedef struct {
  int tid; uint32_t wtid; char name[16]; int rule, nice_set, nice0, winpri, gen;   /* nice0: nice when first seen */
  uint64_t run0, wait0, sw0;      /* schedstat at the last log */
} SpThr;

static pthread_once_t sp_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t sp_mu = PTHREAD_MUTEX_INITIALIZER;
static int sp_mode;               /* 0 off, 1 log only, 2 tiers, 3 tiers + background core */
static int sp_winpri, sp_log_s = 10, sp_ncpu, sp_adpf_fps;
static uint32_t sp_wwise_start = 0xFA57D0;
static SpRule sp_r[SP_MAXR]; static int sp_nr;
static SpThr sp_t[SP_MAXT]; static int sp_nt;
static int sp_cpu[64], sp_ncpus;  /* allowed CPUs (sched_getaffinity at init) */
static int sp_gen;
static struct { uint32_t wtid; int pri; } sp_pend[64]; static int sp_npend;   /* SetThreadPriority before the thread ran */

static int sp_gettid(void){ return (int)syscall(SYS_gettid); }
/* the GameThread: Android = android_glue's game thread (g_game_tid); Linux host build = the process main thread */
static int sp_game_tid(void){ return (&g_game_tid && g_game_tid) ? g_game_tid : (int)getpid(); }

/* ---- rules ---- */
static const char *sp_rules_1 =
  /* tier A, nice -20 (what every game thread has today): the frame's critical path, audio deadline, GPU feed */
  "@game=-20;RenderingThread=-20;dxvk-cs=-20;dxvk-submit=-20;dxvk-queue=-20;dxvk-frame=-20;BridgeThread=-20;"
  "AkAudioThread=-20;"
  /* tier B: helpers the GT / RT dispatch to and may wait for (XJob: culling, line checks; Morpheme anim workers) */
  "XJobCPUExecutio=-14;MorphemeThread=-14;"
  /* tier C: streaming / IO helpers (LZO decompression, shader creation, audio streaming IO) */
  "AsyncIOSystem=-10;PoolThread=-10;ShaderCreateThr=-10;AsyncLoadSoundP=-10;CAkUnrealIOHook=-10;AkQueryThread=-10;"
  /* tier D: saves (compression + file write), never on the frame path */
  "AsyncSaveCompre=0b;CheckpointWrite=0b";
static const char *sp_rules_2 =   /* mode 2 adds: background loaders on the last allowed core (@9 = clamped to the last) */
  "AsyncIOSystem=-10@9;PoolThread=-10@9;ShaderCreateThr=-10@9;AsyncSaveCompre=0b@9;CheckpointWrite=0b@9";

static void sp_parse(const char *s){
  while(s && *s && sp_nr < SP_MAXR){
    while(*s == ';' || *s == ' ') s++;
    const char *eq = strchr(s, '='); if(!eq) break;
    const char *end = strchr(eq, ';'); if(!end) end = eq + strlen(eq);
    SpRule r; memset(&r, 0, sizeof r);
    int n = (int)(eq - s); if(n > 15) n = 15; memcpy(r.pat, s, n); r.pat[n] = 0; r.plen = n;
    if(n && r.pat[n - 1] == '*'){ r.pat[--n] = 0; r.plen = n; }
    const char *v = eq + 1; char *q;
    r.nice = (int)strtol(v, &q, 10); if(q == v) r.nice = SP_UNSET;
    for(; q < end; q++){
      if(*q == 'b') r.batch = 1;
      else if(*q == '@'){ q++; while(q < end && (isdigit((unsigned char)*q) || *q == ',')){ if(isdigit((unsigned char)*q)){ int i = (int)strtol(q, &q, 10); if(i < 32) r.cpus |= 1u << i; } else q++; } q--; }
    }
    if(r.nice != SP_UNSET && r.nice < -20) r.nice = -20;
    if(r.nice != SP_UNSET && r.nice > 19) r.nice = 19;
    sp_r[sp_nr++] = r; s = end;
  }
}
/* the last matching rule wins (BSI_SCHED_RULES are parsed after the presets) */
static int sp_match(int tid, const char *name){
  for(int i = sp_nr - 1; i >= 0; i--){
    if(!strcmp(sp_r[i].pat, "@game")){ if(tid == sp_game_tid()) return i; continue; }
    if(sp_r[i].plen && !strncmp(name, sp_r[i].pat, sp_r[i].plen)) return i;
  }
  return -1;
}
/* Win32 priority -> nice offset (small: the tier decides, the game's wish only orders threads inside it) */
static int sp_win_off(int p){
  return p >= 15 ? -6 : p >= 2 ? -4 : p == 1 ? -2 : p == 0 ? 0 : p == -1 ? 2 : p >= -2 ? 4 : 10;
}

/* ---- applying ---- */
static int sp_apply_one(SpThr *t){
  int ri = sp_match(t->tid, t->name); t->rule = ri;
  int nice = SP_UNSET, batch = 0; uint32_t cpus = 0;
  if(ri >= 0){ nice = sp_r[ri].nice; batch = sp_r[ri].batch; cpus = sp_r[ri].cpus; }
  if(sp_winpri && t->winpri != SP_UNSET){
    int base = nice != SP_UNSET ? nice : t->nice0 != SP_UNSET ? t->nice0 : 0;   /* no rule: the thread's original nice */
    nice = base + sp_win_off(t->winpri); if(nice < -20) nice = -20; if(nice > 19) nice = 19;
  }
  if(sp_mode < 2) return 0;   /* log only */
  int rc = 0;
  if(batch){ struct sched_param sp = { 0 }; if(sched_setscheduler(t->tid, SCHED_BATCH, &sp)) rc |= 1; }
  if(nice != SP_UNSET && nice != t->nice_set){ if(setpriority(PRIO_PROCESS, t->tid, nice)) rc |= 2; else t->nice_set = nice; }
  if(cpus && sp_ncpus){
    cpu_set_t cs; CPU_ZERO(&cs);
    for(int i = 0; i < 32; i++) if(cpus & (1u << i)) CPU_SET(sp_cpu[i < sp_ncpus ? i : sp_ncpus - 1], &cs);
    if(sched_setaffinity(t->tid, sizeof cs, &cs)) rc |= 4;
  }
  if(ri >= 0 || (sp_winpri && t->winpri != SP_UNSET) || rc)
    fprintf(stderr, "[sched] %-15s tid %d -> nice %s%d%s%s%s\n", t->name, t->tid, nice == SP_UNSET ? "(kept) " : "", nice == SP_UNSET ? 0 : nice,
            batch ? " batch" : "", cpus ? " pinned" : "", rc ? " (FAILED)" : "");
  return rc;
}
static SpThr *sp_find(int tid, int add){
  for(int i = 0; i < sp_nt; i++) if(sp_t[i].tid == tid) return &sp_t[i];
  if(!add) return NULL;
  SpThr *t = NULL;
  if(sp_nt < SP_MAXT) t = &sp_t[sp_nt++];
  else for(int i = 0; i < SP_MAXT; i++) if(sp_t[i].gen != sp_gen){ t = &sp_t[i]; break; }   /* reuse an exited thread */
  if(!t) return NULL;
  memset(t, 0, sizeof *t); t->tid = tid; t->rule = -2; t->nice_set = SP_UNSET; t->winpri = SP_UNSET; t->gen = sp_gen;
  errno = 0; t->nice0 = getpriority(PRIO_PROCESS, tid); if(errno) t->nice0 = SP_UNSET;
  return t;
}
static int sp_read(const char *path, char *buf, int n){
  FILE *f = fopen(path, "r"); if(!f) return -1;
  int k = (int)fread(buf, 1, n - 1, f); fclose(f); if(k < 0) k = 0; buf[k] = 0; return k;
}
/* name changed (or new thread): re-apply.  Called with sp_mu held. */
static void sp_note_name(SpThr *t, const char *nm){
  char n[16]; snprintf(n, sizeof n, "%s", nm); char *nl = strchr(n, '\n'); if(nl) *nl = 0;
  if(t->rule != -2 && !strcmp(n, t->name)){
    /* same name: re-assert if something else changed the nice (e.g. the VR runtime re-applying its own value) */
    if(sp_mode >= 2 && t->nice_set != SP_UNSET){ errno = 0; int cur = getpriority(PRIO_PROCESS, t->tid);
      if(!errno && cur != t->nice_set){ static int nre; if(nre++ < 50) fprintf(stderr, "[sched] %s tid %d: nice changed to %d outside BSI_SCHED, re-applying %d\n", t->name, t->tid, cur, t->nice_set);
        t->nice_set = SP_UNSET; sp_apply_one(t); } }
    return; }
  snprintf(t->name, sizeof t->name, "%s", n); sp_apply_one(t);
}

/* ---- ADPF (Android 13+ PerformanceHintManager), looked up at run time ---- */
#ifdef __ANDROID__
static void *(*pa_getManager)(void);
static void *(*pa_create)(void *, const int32_t *, size_t, int64_t);
static int (*pa_report)(void *, int64_t);
static int (*pa_setThreads)(void *, const int32_t *, size_t);
static void (*pa_close)(void *);
static void *pa_mgr, *pa_sess; static int pa_tids[2];
static void sp_adpf_init(void){
  void *h = dlopen("libandroid.so", RTLD_NOW); if(!h){ fprintf(stderr, "[sched] adpf: no libandroid\n"); return; }
  pa_getManager = (void *(*)(void))dlsym(h, "APerformanceHint_getManager");
  pa_create = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(h, "APerformanceHint_createSession");
  pa_report = (int (*)(void *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration");
  pa_setThreads = (int (*)(void *, const int32_t *, size_t))dlsym(h, "APerformanceHint_setThreads");
  pa_close = (void (*)(void *))dlsym(h, "APerformanceHint_closeSession");
  pa_mgr = pa_getManager ? pa_getManager() : NULL;
  fprintf(stderr, "[sched] adpf: manager %p (create %p report %p setThreads %p)\n", pa_mgr, (void *)pa_create, (void *)pa_report, (void *)pa_setThreads);
}
/* (re)create the session when the GT / RT tids are known or change (UE3 restarts the RenderingThread once at boot) */
static void sp_adpf_tids(int gt, int rt){
  if(!pa_mgr || !pa_create || (gt == pa_tids[0] && rt == pa_tids[1])) return;
  int32_t t[2]; int n = 0; if(gt) t[n++] = gt; if(rt) t[n++] = rt; if(!n) return;
  if(pa_sess && pa_setThreads && !pa_setThreads(pa_sess, t, n)){ pa_tids[0] = gt; pa_tids[1] = rt; return; }
  if(pa_sess && pa_close) pa_close(pa_sess);
  pa_sess = pa_create(pa_mgr, t, n, 1000000000LL / sp_adpf_fps); pa_tids[0] = gt; pa_tids[1] = rt;
  fprintf(stderr, "[sched] adpf: session %p for gt %d rt %d, target %.1f ms\n", pa_sess, gt, rt, 1000.0 / sp_adpf_fps);
}
static uint64_t sp_cputime(int tid){   /* ns, from schedstat */
  char p[64], b[128]; snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
  if(sp_read(p, b, sizeof b) <= 0) return 0;
  return strtoull(b, NULL, 10);
}
#endif

/* ---- monitor thread ---- */
static uint64_t sp_now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static int sp_cmp_run(const void *a, const void *b){ const uint64_t *x = a, *y = b; return x[0] < y[0] ? 1 : x[0] > y[0] ? -1 : 0; }
static void sp_scan(void){
  DIR *d = opendir("/proc/self/task"); if(!d) return;
  pthread_mutex_lock(&sp_mu); sp_gen++;
  struct dirent *e;
  while((e = readdir(d))){
    if(!isdigit((unsigned char)e->d_name[0])) continue;
    int tid = atoi(e->d_name); char p[64], nm[32];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    if(sp_read(p, nm, sizeof nm) <= 0) continue;
    SpThr *t = sp_find(tid, 1); if(!t) continue;
    t->gen = sp_gen; sp_note_name(t, nm);
  }
  pthread_mutex_unlock(&sp_mu); closedir(d);
}
static void sp_log(double tsec, double dt){
  /* rows: run_ns delta, wait_ns delta, switches delta, index */
  static uint64_t rows[SP_MAXT][4]; int n = 0; uint64_t trun = 0, twait = 0;
  pthread_mutex_lock(&sp_mu);
  for(int i = 0; i < sp_nt; i++){
    SpThr *t = &sp_t[i]; if(t->gen != sp_gen) continue;
    char p[64], b[128]; snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", t->tid);
    if(sp_read(p, b, sizeof b) <= 0) continue;
    unsigned long long r = 0, w = 0, s = 0; if(sscanf(b, "%llu %llu %llu", &r, &w, &s) != 3) continue;
    uint64_t dr = r - t->run0, dw = w - t->wait0, ds = s - t->sw0; int first = !t->run0 && !t->wait0;
    t->run0 = r; t->wait0 = w; t->sw0 = s; if(first) continue;
    rows[n][0] = dr; rows[n][1] = dw; rows[n][2] = ds; rows[n][3] = (uint64_t)i; n++; trun += dr; twait += dw;
  }
  qsort(rows, n, sizeof rows[0], sp_cmp_run);
  fprintf(stderr, "[sched] t=%.0fs app cpu %.0f%% (of one core) runq-wait %.1f%% threads %d\n", tsec, 100.0 * trun / dt, 100.0 * twait / dt, n);
  for(int k = 0; k < n && k < 10; k++){
    SpThr *t = &sp_t[rows[k][3]]; if(rows[k][0] < dt * 0.002) break;   /* < 0.2 % of a core */
    errno = 0; int nice = getpriority(PRIO_PROCESS, t->tid); int pol = sched_getscheduler(t->tid);
    fprintf(stderr, "[sched]   %-15s %6d%s nice %3d%s cpu %5.1f%% wait %4.1f%% (%5.0f us/switch) sw %5.0f/s\n", t->name, t->tid,
            t->tid == sp_game_tid() ? "*" : " ", errno ? 99 : nice, pol == SCHED_BATCH ? "b" : pol == SCHED_IDLE ? "i" : pol > 0 && pol < 3 ? "R" : " ",
            100.0 * rows[k][0] / dt, 100.0 * rows[k][1] / dt, rows[k][2] ? rows[k][1] / 1e3 / rows[k][2] : 0.0, rows[k][2] * 1e9 / dt);
  }
  pthread_mutex_unlock(&sp_mu);
}
static void *sp_thread(void *arg){
  (void)arg; setpriority(PRIO_PROCESS, 0, 10); pthread_setname_np(pthread_self(), "BSI-sched");
  uint64_t t0 = sp_now_ns(), tlog = t0; uint64_t gt0 = 0, rt0 = 0; long pr0 = 0; (void)gt0; (void)rt0; (void)pr0;
  for(;;){
    sp_scan();
#ifdef __ANDROID__
    if(sp_adpf_fps && pa_mgr){
      int gt = sp_game_tid(), rt = 0;
      pthread_mutex_lock(&sp_mu); for(int i = 0; i < sp_nt; i++) if(sp_t[i].gen == sp_gen && !strcmp(sp_t[i].name, "RenderingThread")) rt = sp_t[i].tid; pthread_mutex_unlock(&sp_mu);
      sp_adpf_tids(gt, rt);
      /* report per-frame work = the busier of GT / RT CPU time per Present over the last ~100 ms (10 reports per scan) */
      for(int k = 0; k < 10 && pa_sess && pa_report; k++){
        uint64_t g = gt ? sp_cputime(gt) : 0, r = rt ? sp_cputime(rt) : 0; long pr = (&g_br_presents) ? g_br_presents : 0;
        if(gt0 && pr > pr0){ uint64_t w = (g - gt0) > (r - rt0) ? (g - gt0) : (r - rt0); pa_report(pa_sess, (int64_t)(w / (uint64_t)(pr - pr0))); }
        gt0 = g; rt0 = r; pr0 = pr; usleep(100000);
      }
    } else
#endif
    sleep(1);
    uint64_t now = sp_now_ns();
    if(sp_log_s > 0 && now - tlog >= (uint64_t)sp_log_s * 1000000000ull){ sp_log((now - t0) / 1e9, (double)(now - tlog)); tlog = now; }
  }
  return NULL;
}

static void sp_init(void){
  const char *e = getenv("BSI_SCHED");
  if(e && *e) sp_mode = !strcmp(e, "log") ? 1 : atoi(e) >= 2 ? 3 : atoi(e) == 1 ? 2 : 0;
  if((e = getenv("BSI_SCHED_WINPRI"))) sp_winpri = atoi(e);
  if((e = getenv("BSI_SCHED_LOG"))) sp_log_s = atoi(e);
  if((e = getenv("BSI_SCHED_ADPF"))) sp_adpf_fps = atoi(e);
  if((e = getenv("BSI_SCHED_WWISE"))) sp_wwise_start = (uint32_t)strtoul(e, NULL, 16);
  if((e = getenv("BSI_NCPU"))){ sp_ncpu = atoi(e); if(sp_ncpu < 2) sp_ncpu = 2; if(sp_ncpu > 16) sp_ncpu = 16; }   /* >= 2: UE3 only starts the RenderingThread with > 1 CPU (0x8be9f4) */
  if(sp_mode >= 2) sp_parse(sp_rules_1);
  if(sp_mode >= 3) sp_parse(sp_rules_2);
  if(sp_mode >= 1) sp_parse(getenv("BSI_SCHED_RULES"));
  cpu_set_t cs; CPU_ZERO(&cs);
  if(!sched_getaffinity(0, sizeof cs, &cs)) for(int i = 0; i < 64 && sp_ncpus < 64; i++) if(CPU_ISSET(i, &cs)) sp_cpu[sp_ncpus++] = i;
  if(!sp_mode && !sp_ncpu && !sp_adpf_fps) return;
  char cl[160] = ""; for(int i = 0; i < sp_ncpus; i++) snprintf(cl + strlen(cl), sizeof cl - strlen(cl), "%s%d", i ? "," : "", sp_cpu[i]);
  fprintf(stderr, "[sched] mode %s, %d rules, winpri %d, log %d s, adpf %d fps, GetSystemInfo cpus %d, allowed cpus %s\n",
          sp_mode == 0 ? "off" : sp_mode == 1 ? "log" : sp_mode == 2 ? "tiers" : "tiers+bgcore", sp_nr, sp_winpri, sp_log_s, sp_adpf_fps, sp_ncpu ? sp_ncpu : 4, cl);
#ifdef __ANDROID__
  if(sp_adpf_fps > 0) sp_adpf_init(); else sp_adpf_fps = 0;
#else
  sp_adpf_fps = 0;
#endif
  if(sp_mode || sp_adpf_fps){ pthread_t t; if(!pthread_create(&t, NULL, sp_thread, NULL)) pthread_detach(t); }
}
static int sp_on(void){ pthread_once(&sp_once, sp_init); return sp_mode; }

/* ---- hooks (declared weak by the callers; see regress/sched/sched_policy.patch) ---- */
void sched_policy_start(void){ sp_on(); }
/* GetSystemInfo: processor count to report (0 = keep the default) */
int sched_policy_ncpu(void){ sp_on(); return sp_ncpu; }
/* thr_main (hle_more.c), in the new thread, before the guest entry point runs */
void sched_policy_thread_start(uint32_t wtid, uint32_t start){
  if(!sp_on()) return;
  if(start == sp_wwise_start) prctl(PR_SET_NAME, "AkAudioThread", 0, 0, 0);   /* HOTCODE.md §1: CAkAudioThread::EventMgrThreadFunc */
  int tid = sp_gettid(); char nm[32] = {0}; prctl(PR_GET_NAME, nm, 0, 0, 0);
  pthread_mutex_lock(&sp_mu);
  SpThr *t = sp_find(tid, 1);
  if(t){ t->wtid = wtid; t->gen = sp_gen;
    for(int i = 0; i < sp_npend; i++) if(sp_pend[i].wtid == wtid){ t->winpri = sp_pend[i].pri; sp_pend[i] = sp_pend[--sp_npend]; break; }
    t->rule = -2; sp_note_name(t, nm); }
  pthread_mutex_unlock(&sp_mu);
}
/* hle_thread_setname (hle_more.c): ktid 0 = the calling thread */
void sched_policy_thread_named(int ktid, const char *name){
  if(!sp_on()) return;
  if(!ktid) ktid = sp_gettid();
  pthread_mutex_lock(&sp_mu); SpThr *t = sp_find(ktid, 1); if(t){ t->gen = sp_gen; sp_note_name(t, name); } pthread_mutex_unlock(&sp_mu);
}
/* kernel32!SetThreadPriority / SetThreadAffinityMask / SetThreadIdealProcessor (handle as the guest passed it) */
void sched_policy_win_request(uint32_t h, int kind, int32_t val){
  if(!sp_on()) return;
  static int nlog; int self = h == 0xfffffffeu;
  uint32_t wtid = (h >> 2) & 0xffff; int tid = 0;
  pthread_mutex_lock(&sp_mu);
  if(self) tid = sp_gettid(); else for(int i = 0; i < sp_nt; i++) if(sp_t[i].wtid == wtid && sp_t[i].gen >= sp_gen - 1){ tid = sp_t[i].tid; break; }
  SpThr *t = tid ? sp_find(tid, 1) : NULL;
  if(nlog++ < 200) fprintf(stderr, "[sched] game %s(%s %04x '%s', %d)\n", kind == 0 ? "SetThreadPriority" : kind == 1 ? "SetThreadAffinityMask" : "SetThreadIdealProcessor",
                           self ? "self" : "thread", self ? 0 : wtid, t ? t->name : "(not started)", (int)val);
  if(kind == 0){
    if(t){ if(t->winpri != val){ t->winpri = val; if(sp_winpri) sp_apply_one(t); } }
    else if(sp_npend < 64){ sp_pend[sp_npend].wtid = wtid; sp_pend[sp_npend].pri = val; sp_npend++; }
  }
  /* affinity / ideal-processor requests refer to the 4 virtual CPUs of GetSystemInfo: logged, never applied */
  pthread_mutex_unlock(&sp_mu);
}
