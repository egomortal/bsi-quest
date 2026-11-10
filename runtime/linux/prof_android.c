/* BSI-Quest sampling profiler (Android): SIGPROF at 500 Hz of process CPU time; the handler stores the interrupted
   PC + thread id in a ring; a reporter thread resolves PCs to symbols (f_XXXXXXXX = recompiled guest function,
   exported from libbsi.so) and writes the hottest functions, overall and per thread, to <files>/profile.txt every 10 s.
   Off by default; create <files>/profile_on.txt to enable. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>
#include <time.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#define RING (1<<16)
static uintptr_t r_pc[RING]; static int r_tid[RING]; static uint16_t r_w[RING]; static volatile uint32_t r_head;
/* r_w = 1 + timer overruns: thread CPU-time timers fire from the scheduler tick, so at HZ < 500 several 2 ms periods
   elapse per delivered signal; weighting by the overrun count keeps per-thread totals equal to real CPU time. */
/* BSI_PROF_BT=1 (cloud raw mode only): also a short call stack per sample (_Unwind_Backtrace from the handler; the frames
   up to and including the interrupted PC are dropped offline), so libc/lock time can be charged to its caller. */
#ifndef __ANDROID__
#include <unwind.h>
#define BTN 14
static uintptr_t r_bt[RING][BTN]; static uint8_t r_btn[RING]; static int g_bt_on;
struct bt_st { uintptr_t *p; int n; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *a){ struct bt_st *s = (struct bt_st*)a;
  if(s->n >= BTN) return _URC_END_OF_STACK; s->p[s->n++] = (uintptr_t)_Unwind_GetIP(ctx); return _URC_NO_REASON; }
#define BT_SAMPLE(i) do{ if(g_bt_on){ struct bt_st st = { r_bt[i], 0 }; _Unwind_Backtrace(bt_cb, &st); r_btn[i] = (uint8_t)st.n; } else r_btn[i] = 0; }while(0)
#else
#define BT_SAMPLE(i) do{}while(0)
#endif
static void on_prof(int sig, siginfo_t *si, void *uc_){
  ucontext_t *uc = (ucontext_t*)uc_; uint32_t i = __atomic_fetch_add(&r_head, 1, __ATOMIC_RELAXED) & (RING-1);
  BT_SAMPLE(i);
#if defined(__aarch64__)
  r_pc[i] = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
  r_pc[i] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
  r_pc[i] = 0;
#endif
  r_tid[i] = (int)syscall(SYS_gettid); r_w[i] = (uint16_t)(1 + (si->si_overrun > 1000 ? 1000 : si->si_overrun));
}
typedef struct { const char *name; uintptr_t base; int n; int tid_n[16]; int tids[16]; } Ent;
static int cmpent(const void *a, const void *b){ return ((const Ent*)b)->n - ((const Ent*)a)->n; }
static char g_prof_path[600];
static void thread_name(int tid, char *out, int n){ char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); FILE *f = fopen(p, "r"); out[0] = 0;
  if(f){ if(fgets(out, n, f)){ char *nl = strchr(out, '\n'); if(nl) *nl = 0; } fclose(f); } }
static void *reporter(void *p){
  static Ent ents[8192]; uint32_t tail = 0; long total_all = 0;
  static struct { int tid; long n; } th[256]; int nth = 0;
  int nent = 0;
  for(;;){
    sleep(10);
    uint32_t head = r_head; if(head - tail > RING) tail = head - RING;
    for(; tail != head; tail++){
      uint32_t i = tail & (RING-1); uintptr_t pc = r_pc[i]; int tid = r_tid[i]; Dl_info di; const char *nm = "?"; uintptr_t base = 0;
      if(dladdr((void*)pc, &di)){ if(di.dli_sname){ nm = di.dli_sname; base = (uintptr_t)di.dli_saddr; } else { nm = di.dli_fname ? di.dli_fname : "?"; base = (uintptr_t)di.dli_fbase + (((pc - (uintptr_t)di.dli_fbase) >> 6) << 6); } }
      int k; for(k = 0; k < nent; k++) if(ents[k].base == base && ents[k].name == nm) break;
      if(k == nent){ if(nent >= 8192) continue; ents[k].name = nm; ents[k].base = base; ents[k].n = 0; memset(ents[k].tids, 0, sizeof ents[k].tids); memset(ents[k].tid_n, 0, sizeof ents[k].tid_n); nent++; }
      int w = r_w[i]; ents[k].n += w; total_all += w;
      for(int j = 0; j < 16; j++){ if(ents[k].tids[j] == tid || ents[k].tids[j] == 0){ ents[k].tids[j] = tid; ents[k].tid_n[j] += w; break; } }
      int t; for(t = 0; t < nth; t++) if(th[t].tid == tid) break; if(t == nth && nth < 256){ th[nth].tid = tid; th[nth].n = 0; nth++; } if(t < nth) th[t].n += w;
    }
    static Ent sorted[8192]; memcpy(sorted, ents, nent * sizeof(Ent)); qsort(sorted, nent, sizeof(Ent), cmpent);
    FILE *f = fopen(g_prof_path, "w"); if(!f) continue;
    fprintf(f, "# BSI-Quest profile: %ld samples (500/s of CPU time, game threads only), cumulative since start\n# threads:\n", total_all);
    for(int t = 0; t < nth; t++){ char nm[32]; thread_name(th[t].tid, nm, sizeof nm); fprintf(f, "#   tid %d %-16s %6.2f%%\n", th[t].tid, nm, 100.0 * th[t].n / (total_all ? total_all : 1)); }
    fprintf(f, "# top functions:\n");
    for(int k = 0; k < nent && k < 150; k++){
      { Dl_info d2; long off = -1; if(dladdr((void*)sorted[k].base, &d2) && !d2.dli_sname) off = (long)(sorted[k].base - (uintptr_t)d2.dli_fbase);
      fprintf(f, "%6.2f%% %7d %s", 100.0 * sorted[k].n / (total_all ? total_all : 1), sorted[k].n, sorted[k].name); if(off >= 0) fprintf(f, "+0x%lx", off); }
      for(int j = 0; j < 16 && sorted[k].tids[j]; j++) fprintf(f, " [%d:%d]", sorted[k].tids[j], sorted[k].tid_n[j]);
      fputc('\n', f); }
    fclose(f);
  }
  return NULL;
}
/* Per-thread CPU-time timers aimed only at game (guest) threads: the Quest's audio/XR/system threads never get SIGPROF. */
static volatile int g_prof_on;
void prof_thread_start(void){
  if(!g_prof_on) return;
  struct sigevent se; memset(&se, 0, sizeof se); se.sigev_notify = SIGEV_THREAD_ID; se.sigev_signo = SIGPROF; se._sigev_un._tid = (int)syscall(SYS_gettid);
  timer_t t; if(timer_create(CLOCK_THREAD_CPUTIME_ID, &se, &t) != 0) return;
  struct itimerspec its = { { 0, 2000000 }, { 0, 2000000 } }; timer_settime(t, 0, &its, NULL);
}
void prof_start(const char *files){
  /* v0.8.7: OFF by default. simpleperf works now (profileable manifest), and this profiler's reporter (dladdr on every sample)
     cost ~5.7 % of the app's CPU in the v0.8.6 Quest profile. <files>/profile_on.txt turns it on. */
  char on[600]; snprintf(on, sizeof on, "%s/profile_on.txt", files); if(access(on, R_OK) != 0) return;
  snprintf(g_prof_path, sizeof g_prof_path, "%s/profile.txt", files);
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
  g_prof_on = 1; prof_thread_start();   /* the calling (main game) thread; guest threads register themselves when they start */
  pthread_t t; pthread_create(&t, NULL, reporter, NULL); pthread_detach(t);
  fprintf(stderr, "[prof] sampling profiler on -> %s\n", g_prof_path);
}

/* Linux cloud mode (BSI_PROF=<dir>, started from rt_linux main): same 500 Hz per-game-thread sampling, but the samples are
   written raw for offline attribution with the full symbol table (static functions included) by prof_split.py:
   <dir>/samples.txt ("T <epoch>" markers every 10 s, then "<tid> <pc hex> [weight]"), <dir>/maps.txt (/proc/self/maps),
   <dir>/threads.txt ("<tid> <name>"). */
static char g_raw_dir[512];
static void copy_file(const char *from, const char *to){ FILE *a = fopen(from, "r"), *b = fopen(to, "w"); char buf[4096]; size_t n;
  if(a && b) while((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b); if(a) fclose(a); if(b) fclose(b); }
static void *raw_reporter(void *p){
  char sp[600], mp[600], tp[600]; snprintf(sp, sizeof sp, "%s/samples.txt", g_raw_dir); snprintf(mp, sizeof mp, "%s/maps.txt", g_raw_dir); snprintf(tp, sizeof tp, "%s/threads.txt", g_raw_dir);
  FILE *sf = fopen(sp, "w"); if(!sf) return NULL; uint32_t tail = 0; static int tids[1024]; static char names[1024][20]; int ntid = 0;
  for(;;){
    sleep(10);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); fprintf(sf, "T %.1f\n", ts.tv_sec + ts.tv_nsec * 1e-9);
    uint32_t head = r_head; if(head - tail > RING) tail = head - RING;
    for(; tail != head; tail++){ uint32_t i = tail & (RING-1); int tid = r_tid[i];
#ifndef __ANDROID__
      if(r_btn[i]){ fprintf(sf, "%d %lx %u B", tid, (unsigned long)r_pc[i], r_w[i]); for(int k = 0; k < r_btn[i] && k < BTN; k++) fprintf(sf, " %lx", (unsigned long)r_bt[i][k]); fputc('\n', sf); }
      else
#endif
      if(r_w[i] > 1) fprintf(sf, "%d %lx %u\n", tid, (unsigned long)r_pc[i], r_w[i]); else fprintf(sf, "%d %lx\n", tid, (unsigned long)r_pc[i]);
      int k; for(k = 0; k < ntid; k++) if(tids[k] == tid) break; if(k == ntid && ntid < 1024){ tids[ntid++] = tid; names[k][0] = 0; } }
    fflush(sf);
    for(int k = 0; k < ntid; k++){ char nm[20]; thread_name(tids[k], nm, sizeof nm); if(nm[0]) memcpy(names[k], nm, sizeof nm); }
    FILE *tf = fopen(tp, "w"); if(tf){ for(int k = 0; k < ntid; k++) fprintf(tf, "%d %s\n", tids[k], names[k][0] ? names[k] : "?"); fclose(tf); }
    copy_file("/proc/self/maps", mp);
  }
  return NULL;
}
void prof_start_raw(const char *dir){
  snprintf(g_raw_dir, sizeof g_raw_dir, "%s", dir);
  char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); if(system(cmd)){}
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
#ifndef __ANDROID__
  { const char *b = getenv("BSI_PROF_BT"); if(b && atoi(b) > 0){ uintptr_t x[BTN]; struct bt_st st = { x, 0 }; _Unwind_Backtrace(bt_cb, &st); g_bt_on = 1; } }   /* first call loads the unwinder outside a handler */
#endif
  g_prof_on = 1; prof_thread_start();
  pthread_t t; pthread_create(&t, NULL, raw_reporter, NULL); pthread_detach(t);
  fprintf(stderr, "[prof] raw sampling profiler on -> %s\n", dir);
}
