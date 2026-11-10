/* HLE batch 2: SSE2 libm, x87 CRT math, threads/events/waits, misc */
#define _GNU_SOURCE
#include "hle.h"
#include <math.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <strings.h>

/* __libm_sse2_*: arg in xmm0 (double), result in xmm0 (+ low of xmm1 for 2-arg) */
#define SSE2_1(nm,fn) HLE(msvcr90,__libm_sse2_##nm){ c->xmm[0].d[0]=fn(c->xmm[0].d[0]); c->esp+=4; }
#define SSE2_1F(nm,fn) HLE(msvcr90,__libm_sse2_##nm){ c->xmm[0].f[0]=fn(c->xmm[0].f[0]); c->esp+=4; }
SSE2_1(sin,sin) SSE2_1(cos,cos) SSE2_1(tan,tan) SSE2_1(asin,asin) SSE2_1(acos,acos) SSE2_1(atan,atan)
SSE2_1(exp,exp) SSE2_1(log,log) SSE2_1(log10,log10) SSE2_1(sqrt,sqrt)
SSE2_1F(sinf,sinf) SSE2_1F(cosf,cosf) SSE2_1F(tanf,tanf) SSE2_1F(asinf,asinf) SSE2_1F(acosf,acosf) SSE2_1F(atanf,atanf) SSE2_1F(expf,expf) SSE2_1F(logf,logf) SSE2_1F(log10f,log10f)
HLE(msvcr90,__libm_sse2_pow){ c->xmm[0].d[0]=pow(c->xmm[0].d[0],c->xmm[1].d[0]); c->esp+=4; }
HLE(msvcr90,__libm_sse2_powf){ c->xmm[0].f[0]=powf(c->xmm[0].f[0],c->xmm[1].f[0]); c->esp+=4; }
HLE(msvcr90,__libm_sse2_atan2){ c->xmm[0].d[0]=atan2(c->xmm[0].d[0],c->xmm[1].d[0]); c->esp+=4; }
HLE(msvcr90,__libm_sse2_atan2f){ c->xmm[0].f[0]=atan2f(c->xmm[0].f[0],c->xmm[1].f[0]); c->esp+=4; }
/* _CI*: x87 st0 in/out */
#define CI1(nm,fn) HLE(msvcr90,_CI##nm){ c->st[c->top&7]=fn(c->st[c->top&7]); c->esp+=4; }
CI1(sin,sin) CI1(cos,cos) CI1(tan,tan) CI1(asin,asin) CI1(acos,acos) CI1(atan,atan) CI1(exp,exp) CI1(log,log) CI1(log10,log10) CI1(sqrt,sqrt) CI1(sinh,sinh) CI1(cosh,cosh) CI1(tanh,tanh)
HLE(msvcr90,_CIfmod){ double y=c->st[c->top], x=c->st[(c->top+1)&7]; c->top=(c->top+1)&7; c->st[c->top]=fmod(x,y); c->esp+=4; }
HLE(msvcr90,_CIatan2){ double x=c->st[c->top], y=c->st[(c->top+1)&7]; c->top=(c->top+1)&7; c->st[c->top]=atan2(y,x); c->esp+=4; }
/* cdecl double-arg math returning in st0 */
#define DM1(nm,fn) HLE(msvcr90,nm){ double a=RF64(c->esp+4); c->top=(c->top-1)&7; c->st[c->top]=fn(a); c->esp+=4; }
DM1(floor,floor) DM1(ceil,ceil) DM1(fabs,fabs) DM1(sqrt,sqrt) DM1(sin,sin) DM1(cos,cos) DM1(exp,exp) DM1(log,log)
HLE(msvcr90,fmod){ double a=RF64(c->esp+4), b=RF64(c->esp+12); c->top=(c->top-1)&7; c->st[c->top]=fmod(a,b); c->esp+=4; }
HLE(msvcr90,pow){ double a=RF64(c->esp+4), b=RF64(c->esp+12); c->top=(c->top-1)&7; c->st[c->top]=pow(a,b); c->esp+=4; }
HLE(msvcr90,atan2){ double a=RF64(c->esp+4), b=RF64(c->esp+12); c->top=(c->top-1)&7; c->st[c->top]=atan2(a,b); c->esp+=4; }
HLE(msvcr90,ldexp){ double a=RF64(c->esp+4); int e=(int)R32(c->esp+12); c->top=(c->top-1)&7; c->st[c->top]=ldexp(a,e); c->esp+=4; }
HLE(msvcr90,_ftol2){ int64_t v=(int64_t)c->st[c->top&7]; c->top=(c->top+1)&7; c->eax=(uint32_t)v; c->edx=(uint32_t)(v>>32); c->esp+=4; }
HLE(msvcr90,_ftol2_sse){ int64_t v=(int64_t)c->st[c->top&7]; c->top=(c->top+1)&7; c->eax=(uint32_t)v; c->edx=(uint32_t)(v>>32); c->esp+=4; }
HLE(msvcr90,_isnan){ double a=RF64(c->esp+4); RET_C(isnan(a)); }
HLE(msvcr90,_finite){ double a=RF64(c->esp+4); RET_C(isfinite(a)); }
HLE(msvcr90,_clearfp){ RET_C(0); }
HLE(msvcr90,_controlfp){ RET_C(0x9001f); }

/* ---------------- threads ---------------- */
typedef struct { uint32_t start, param, h; } TS;
extern __thread CPU *t_cpu;
void thread_setup_teb(CPU *c, uint32_t *teb_out, uint32_t *stk_lo);
static uint32_t ev_new(int kind,int manual,int state);
/* WaitForMultipleObjects used to poll with usleep(500); now every signal bumps a generation counter and wakes multi-waiters */
static pthread_mutex_t g_any_mu=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_any_cv=PTHREAD_COND_INITIALIZER; static unsigned g_any_gen;
static void ev_poke(void){ pthread_mutex_lock(&g_any_mu); g_any_gen++; pthread_cond_broadcast(&g_any_cv); pthread_mutex_unlock(&g_any_mu); }
/* BSI_SYNCFAST (notes/GAMETHREAD_HOT.md; default 0 = the exact previous code paths, BSI_SYNCFAST=1 turns it on):
   - a release (SetEvent, ReleaseSemaphore, ReleaseMutex, thread exit) broadcasts the slot's condvar only when a thread is
     blocked in ev_wait on it (e->waiters, kept under e->mu, so the test is exact).  Bionic's pthread_cond_broadcast always
     makes a FUTEX_WAKE syscall, waiters or not.
   - ev_poke (wakes every WaitForMultipleObjects caller in the process) only runs when a WFMO call is registered on that
     slot (e->mwait).  WFMO registers on all its handles before its first readiness check and deregisters on return; the
     release changes the state under e->mu and reads mwait after unlocking, so either WFMO's check sees the new state or the
     release sees mwait > 0 (both sides are ordered by e->mu).  WFMO calls with more than 64 handles force pokes. */
static int g_syncfast=-1, g_wfmo_big;
static int sync_fast(void){ if(__builtin_expect(g_syncfast<0,0)){ const char*s=getenv("BSI_SYNCFAST"); g_syncfast= s&&*s ? atoi(s)!=0 : 0; } return g_syncfast; }
#define EV_BCAST(e) do{ if(!sync_fast() || (e)->waiters>0) pthread_cond_broadcast(&(e)->cv); }while(0)
#define EV_POKE(e)  do{ if(!sync_fast() || __atomic_load_n(&(e)->mwait,__ATOMIC_SEQ_CST)>0 || __atomic_load_n(&g_wfmo_big,__ATOMIC_SEQ_CST)>0) ev_poke(); }while(0)
static void thr_wait_resume(uint32_t h); static void thr_exited(uint32_t h); static uint32_t thr_resume(uint32_t h); static void thr_suspend_mark_impl(uint32_t h);
#define thr_suspend_mark thr_suspend_mark_impl
static __thread uint32_t t_thread_handle;
__thread uint32_t t_tid;   /* Win32 thread id: must match what CreateThread reported (UE3 compares it with GRenderingThreadId) */
/* Win32 thread id -> host thread (for SetThreadName and profiling) */
#include <sys/syscall.h>
/* v0.8.4d: keep the kernel tid, not the pthread_t. A pthread_t of an exited thread is a dangling pointer and bionic
   aborts ("invalid pthread_t passed to pthread_setname_np") when a later SetThreadName hits a recycled Win32 thread id.
   Other threads are now named through /proc/self/task/<tid>/comm, which just fails harmlessly if the thread is gone. */
static struct { uint32_t wtid; int ktid; } g_thr[512]; static int g_nthr, g_thr_next; static pthread_mutex_t g_thr_mu=PTHREAD_MUTEX_INITIALIZER;
static void thr_register(uint32_t wtid){ int kt=(int)syscall(SYS_gettid); pthread_mutex_lock(&g_thr_mu);
  int i; for(i=0;i<g_nthr;i++) if(g_thr[i].wtid==wtid) break;              /* recycled id: the newest thread wins */
  if(i==g_nthr){ if(g_nthr<512) g_nthr++; else i=g_thr_next++ % 512; }
  g_thr[i].wtid=wtid; g_thr[i].ktid=kt; pthread_mutex_unlock(&g_thr_mu); }
/* BSI_SCHED (sched_policy.c, included at the end of this file; notes/SCHED.md) */
void sched_policy_thread_start(uint32_t wtid, uint32_t start) __attribute__((weak));
void sched_policy_thread_named(int ktid, const char *name) __attribute__((weak));
void sched_policy_win_request(uint32_t h, int kind, int32_t val) __attribute__((weak));
void hle_thread_setname(uint32_t wtid, const char *nm){
  char n16[16]; snprintf(n16,sizeof n16,"%s",nm);                          /* Linux limit: 15 chars + NUL */
  if(wtid==0xffffffffu || wtid==t_tid){ pthread_setname_np(pthread_self(),n16); fprintf(stderr,"[thread] %04x named '%s'\n",t_tid,nm);
    if(sched_policy_thread_named) sched_policy_thread_named(0,n16); return; }
  int kt=0; pthread_mutex_lock(&g_thr_mu); for(int i=0;i<g_nthr;i++) if(g_thr[i].wtid==wtid){ kt=g_thr[i].ktid; break; } pthread_mutex_unlock(&g_thr_mu);
  if(!kt) return;
  char p[64]; snprintf(p,sizeof p,"/proc/self/task/%d/comm",kt); FILE *f=fopen(p,"w");
  if(f){ fputs(n16,f); fclose(f); fprintf(stderr,"[thread] %04x named '%s'\n",wtid,nm); if(sched_policy_thread_named) sched_policy_thread_named(kt,n16); } }
void prof_thread_start(void) __attribute__((weak));
static int g_thr_log=-1;
static int thr_log_on(void){ if(__builtin_expect(g_thr_log<0,0)){ const char*s=getenv("BSI_THRLOG"); g_thr_log= s&&*s&&*s!='0'; } return g_thr_log; }
static void *thr_main(void *p){
  TS ts=*(TS*)p; free(p);
  t_thread_handle=ts.h; t_tid=(ts.h>>2)&0xffff; thr_register(t_tid); if(prof_thread_start) prof_thread_start();
  if(sched_policy_thread_start) sched_policy_thread_start(t_tid, ts.start);
  thr_wait_resume(ts.h);
  CPU *c=calloc(1,sizeof(CPU)); t_cpu=c; c->fcw=0x27f;
  uint32_t teb,lo; thread_setup_teb(c,&teb,&lo);
  PUSH32(c,ts.param); PUSH32(c,0xDEADBEEFu);
  { extern void host_fp_thread_start(void) __attribute__((weak)); if(host_fp_thread_start) host_fp_thread_start(); }   /* BSI_FTZ (rt_linux.c) */
  /* BSI_THRLOG=1 (Fable micro C; default off): one line per guest thread with its kernel tid, so unnamed threads in
     simpleperf/perfetto (comm "m.bsiquest.game") can be joined to their start function (f_<addr> in the profile) */
  if(getenv("BSI_EMU_RANGE") || thr_log_on()) fprintf(stderr,"[thread] %04x start f_%08x param %08x ktid %d\n",t_tid,ts.start,ts.param,(int)syscall(SYS_gettid));
  { extern __thread uint32_t t_emu_thread_start; t_emu_thread_start=ts.start; }
  call_indirect(c,ts.start);
  thr_exited(ts.h);
  return 0;
}
static uint32_t thr_create(uint32_t start, uint32_t param, uint32_t flags, uint32_t tidp){
  uint32_t h=ev_new(2,1,0);              /* thread handle = manual-reset event signalled at exit */
  TS *ts=malloc(sizeof *ts); ts->start=start; ts->param=param; ts->h=h;
  if(flags&4) thr_suspend_mark(h);        /* CREATE_SUSPENDED */
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,8<<20);
  pthread_create(&th,&at,thr_main,ts); pthread_detach(th);
  if(tidp) W32(tidp,(h>>2)&0xffff);
  return h;
}
uint32_t hle_thread_create(uint32_t start, uint32_t param){ return thr_create(start,param,0,0); }
HLE(kernel32,CreateThread){ RET_STD(6,thr_create(ARG(2),ARG(3),ARG(4),ARG(5))); }
HLE(msvcr90,_beginthreadex){ RET_C(thr_create(ARG(2),ARG(3),ARG(4),ARG(5))); }
HLE(kernel32,ExitThread){ thr_exited(t_thread_handle); pthread_exit(0); }
HLE(msvcr90,_endthreadex){ thr_exited(t_thread_handle); pthread_exit(0); }
HLE(kernel32,ResumeThread){ RET_STD(1,thr_resume(ARG(0))); }
HLE(kernel32,SetThreadAffinityMask){ if(sched_policy_win_request) sched_policy_win_request(ARG(0),1,(int32_t)ARG(1)); RET_STD(2,1); }
HLE(kernel32,GetThreadPriority){ RET_STD(1,0); }
HLE(kernel32,SwitchToThread){ sched_yield(); RET_STD(0,1); }

/* ---------------- events / waits (handle -> host event) ---------------- */
/* Step D (OVERNIGHT.md), BSI_EVFIX (default 1; 0 = the old behaviour, kept for A/B):
   - each slot's mutex/condvar is initialised once and never again (the old ev_new re-ran pthread_mutex_init /
     pthread_cond_init on a freed slot, undefined behaviour if a thread was still inside a wait on it);
   - slots are handed out round-robin (next free slot after the last one) instead of lowest-first, so a stale handle (a
     handle used after CloseHandle) does not immediately alias the next new event, and a slot that still has a thread in
     ev_wait is not reused;
   - a reused slot gets count/max/owner cleared: a slot freed by a semaphore with lMaximumCount 1 kept max==1, and a
     thread created in it later never started (thr_wait_resume treats max==1 as CREATE_SUSPENDED);
   - slot 0x1004 (= thread id 4100) is never used: 0x1004 is the GameThread's Win32 thread id;
   - timed waits use CLOCK_MONOTONIC (wall-clock steps no longer stretch or cut timeouts);
   - WaitForMultipleObjects(bWaitAll=TRUE) checks and consumes all objects atomically (under all their locks);
   - SetEvent releases the threads that are waiting at that moment, as on Windows, even when ResetEvent follows before they
     run: manual-reset events count every SetEvent (sgen; a waiter that started before it returns), auto-reset events hand
     the release to one blocked waiter (tokens) instead of leaving it in state where a ResetEvent or another thread's poll
     could take it.  The old code only set state=1 and broadcast; a thread that did SetEvent+ResetEvent (the job queue at
     0xfe3cf0/0xfe39e0 does exactly this on its manual-reset "work" event) could clear it before the woken waiters re-took
     the lock, they went back to sleep, and the job was never run: the GameThread then waited forever in f_006b0c50.
   BSI_EVLOG: event/wait trace, see evlog.c. */
#define NEV 8192
typedef struct { int used, manual, state, kind; pthread_mutex_t mu; pthread_cond_t cv; int count, max; uint32_t owner; int inited, mono, waiters, tokens; unsigned sgen; int mwait; } Ev;   /* kind 0 event, 1 semaphore, 2 thread, 3 mutex */
static Ev EV[NEV]; static pthread_mutex_t ev_mu=PTHREAD_MUTEX_INITIALIZER;
static int g_evfix=-1, ev_next=1;
static int g_evdelay;   /* BSI_EVDELAY_US (debug only): a woken waiter sleeps this long before re-taking the event lock, which
                           widens the SetEvent -> ResetEvent window the old code lost wake-ups in (stress test for BSI_EVFIX) */
static int ev_fix(void){ if(__builtin_expect(g_evfix<0,0)){ const char*s=getenv("BSI_EVFIX"); g_evfix= s&&*s ? atoi(s)!=0 : 1;
    s=getenv("BSI_EVDELAY_US"); g_evdelay= s ? atoi(s) : 0; if(g_evdelay) fprintf(stderr,"[sync] BSI_EVDELAY_US=%d (debug)\n",g_evdelay); } return g_evfix; }
#include "evlog.c"
static void ev_slot_init(Ev*e){ pthread_mutex_init(&e->mu,0); pthread_condattr_t a; pthread_condattr_init(&a); e->mono=pthread_condattr_setclock(&a,CLOCK_MONOTONIC)==0;
  pthread_cond_init(&e->cv,&a); pthread_condattr_destroy(&a); e->inited=1; }
static uint32_t ev_new(int kind,int manual,int state){ pthread_mutex_lock(&ev_mu); uint32_t h=0; Ev*e=0;
  if(!ev_fix()){ for(int k=1;k<NEV;k++) if(!EV[k].used){ e=&EV[k]; e->used=1; e->kind=kind; e->manual=manual; e->state=state; pthread_mutex_init(&e->mu,0); pthread_cond_init(&e->cv,0); e->mono=0; e->inited=1; h=0x40000u+4*k; break; } }
  else for(int n=1;n<NEV;n++){ int k=ev_next; ev_next=k+1<NEV?k+1:1;
    if(k==0x1004) continue;
    e=&EV[k]; if(e->used || __atomic_load_n(&e->waiters,__ATOMIC_ACQUIRE)){ e=0; continue; }
    if(!e->inited) ev_slot_init(e);
    pthread_mutex_lock(&e->mu); e->kind=kind; e->manual=manual; e->state=state; e->count=0; e->max=0; e->owner=0; e->tokens=0; pthread_mutex_unlock(&e->mu);
    __atomic_store_n(&e->used,1,__ATOMIC_RELEASE); h=0x40000u+4*k; break; }
  pthread_mutex_unlock(&ev_mu);
  if(!h){ static int nf; if(nf++<5) fprintf(stderr,"[sync] out of event slots (%d)\n",NEV); }
  else if(g_evl) evl_rec(EVO_NEW,h,e,kind|manual<<8|state<<16,0);
  return h; }
static Ev* ev_get(uint32_t h){ if(h<0x40000u||h>=0x40000u+4*NEV) return 0; Ev*e=&EV[(h-0x40000u)/4]; return e->used?e:0; }
static void thr_suspend_mark_impl(uint32_t h){ Ev*e=ev_get(h); if(e){ pthread_mutex_lock(&e->mu); e->max=1; pthread_mutex_unlock(&e->mu);} }
static void thr_wait_resume(uint32_t h){ Ev*e=ev_get(h); if(!e) return; pthread_mutex_lock(&e->mu); while(e->max==1) pthread_cond_wait(&e->cv,&e->mu); pthread_mutex_unlock(&e->mu); }
static uint32_t thr_resume(uint32_t h){ Ev*e=ev_get(h); if(!e||e->kind!=2) return 0; pthread_mutex_lock(&e->mu); uint32_t prev=e->max==1; e->max=0; pthread_cond_broadcast(&e->cv); pthread_mutex_unlock(&e->mu); ev_poke(); if(g_evl) evl_rec(EVO_RESUME,h,e,prev,0); return prev; }
static void thr_exited(uint32_t h){ Ev*e=ev_get(h); if(!e) return; pthread_mutex_lock(&e->mu); e->state=1; e->sgen++; EV_BCAST(e); pthread_mutex_unlock(&e->mu); EV_POKE(e); if(g_evl) evl_rec(EVO_THREXIT,h,e,0,0); }
HLE(kernel32,CreateEventW){ RET_STD(4,ev_new(0,ARG(1)!=0,ARG(2)!=0)); }
HLE(kernel32,CreateEventA){ RET_STD(4,ev_new(0,ARG(1)!=0,ARG(2)!=0)); }
HLE(kernel32,CreateSemaphoreW){ uint32_t h=ev_new(1,0,0); Ev*e=ev_get(h); e->count=ARG(1); e->max=ARG(2); RET_STD(4,h); }
HLE(kernel32,CreateSemaphoreA){ uint32_t h=ev_new(1,0,0); Ev*e=ev_get(h); e->count=ARG(1); e->max=ARG(2); RET_STD(4,h); }
HLE(kernel32,ReleaseSemaphore){ Ev*e=ev_get(ARG(0)); if(!e) RET_STD(3,0); pthread_mutex_lock(&e->mu); if(ARG(2)) W32(ARG(2),e->count);
  if(e->max>0 && (int64_t)e->count+(int32_t)ARG(1)>e->max){ pthread_mutex_unlock(&e->mu); g_last_error=298; RET_STD(3,0); }   /* ERROR_TOO_MANY_POSTS */
  e->count+=ARG(1); EV_BCAST(e); pthread_mutex_unlock(&e->mu); EV_POKE(e); if(g_evl) evl_rec(EVO_SEMREL,ARG(0),e,ARG(1),0); RET_STD(3,1); }
/* ev_set: see the header comment (BSI_EVFIX).  Manual reset: signalled, and every current waiter is released (sgen).
   Auto reset: one blocked waiter without a pending release gets one (tokens) and the event stays non-signalled;
   with no such waiter the event becomes signalled. */
static void ev_set(Ev*e){ if(!g_evfix) e->state=1; else if(e->manual){ e->state=1; e->sgen++; } else if(!e->state && e->waiters>e->tokens) e->tokens++; else e->state=1; }
HLE(kernel32,SetEvent){ Ev*e=ev_get(ARG(0)); if(e){ ev_fix(); pthread_mutex_lock(&e->mu); ev_set(e); EV_BCAST(e); pthread_mutex_unlock(&e->mu); EV_POKE(e); }
  if(g_evl) evl_rec(e?EVO_SET:EVO_STALE_SET,ARG(0),e,0,0); RET_STD(1,1); }
HLE(kernel32,ResetEvent){ Ev*e=ev_get(ARG(0)); if(e){ pthread_mutex_lock(&e->mu); e->state=0; pthread_mutex_unlock(&e->mu);} if(g_evl) evl_rec(e?EVO_RESET:EVO_STALE_RESET,ARG(0),e,0,0); RET_STD(1,1); }
static void ev_deadline(Ev*e, uint32_t ms, struct timespec *ts){ clock_gettime(e->mono?CLOCK_MONOTONIC:CLOCK_REALTIME,ts);
  uint64_t ns=(uint64_t)ts->tv_nsec+(uint64_t)(ms%1000)*1000000ull; ts->tv_sec+=ms/1000+ns/1000000000ull; ts->tv_nsec=ns%1000000000ull; }
static int ev_wait(Ev*e, uint32_t ms){
  struct timespec ts; if(ms && ms!=0xffffffffu) ev_deadline(e,ms,&ts);
  if(ms){ void s0_note_wait(void); s0_note_wait(); }   /* Step B: a blocking wait ends a Sleep(0) streak */
  pthread_mutex_lock(&e->mu); int rc=0, w=0, fix=g_evfix>0, timed=0; unsigned g0=e->sgen;
  for(;;){
    if(e->kind==1){ if(e->count>0){ e->count--; break; } }
    else if(e->kind==3){ extern __thread uint32_t t_tid; uint32_t me=t_tid?t_tid:0x1004; if(!e->owner||e->owner==me){ e->owner=me; e->count++; break; } }
    else if(fix && w && e->tokens>0){ e->tokens--; break; }          /* auto reset: a SetEvent released this (blocked) waiter */
    else if(e->state){ if(!e->manual) e->state=0; break; }
    else if(fix && e->manual && e->sgen!=g0) break;                  /* manual reset: set (and reset again) since this wait began */
    if(ms==0 || timed){ rc=258; break; }
    if(!w){ w=1; __atomic_add_fetch(&e->waiters,1,__ATOMIC_ACQ_REL); }
    if(ms==0xffffffffu) pthread_cond_wait(&e->cv,&e->mu);
    else if(pthread_cond_timedwait(&e->cv,&e->mu,&ts)==ETIMEDOUT){ if(!fix){ rc=258; break; } timed=1; }   /* fix: one last check, a release may have raced the timeout */
    if(g_evdelay){ pthread_mutex_unlock(&e->mu); usleep(g_evdelay); pthread_mutex_lock(&e->mu); }   /* debug: widen the wake-up race */
  }
  if(w) __atomic_sub_fetch(&e->waiters,1,__ATOMIC_ACQ_REL);
  pthread_mutex_unlock(&e->mu); return rc;
}
HLE(kernel32,WaitForSingleObject){ Ev*e=ev_get(ARG(0)); if(!e){ static int nw; if(nw++<20) fprintf(stderr,"[sync] WaitForSingleObject on unknown handle %08x (ret %08x)\n",ARG(0),R32(c->esp)); if(g_evl) evl_rec(EVO_STALE_WAIT,ARG(0),0,ARG(1),0); if(ARG(1) && ARG(1)!=0xffffffffu) usleep(1000); RET_STD(2,0);} { static int lg=-1; if(lg<0) lg=getenv("BSI_SYNCLOG")!=0; if(lg && R32(c->esp)==0x58c8a5u){ static int n; if(n++<30) fprintf(stderr,"[sync] pool wait h=%08x kind=%d manual=%d state=%d count=%d\n",ARG(0),e->kind,e->manual,e->state,e->count); } }
  if(g_evl){ uint32_t h=ARG(0), ms=ARG(1); evl_rec(EVO_WAIT,h,e,ms,0); evl_wait_begin(&h,1,ms); int rc=ev_wait(e,ms); evl_wait_end(); evl_rec(EVO_WAITED,h,e,ms,rc==258?2:rc); RET_STD(2,rc); }
  RET_STD(2,ev_wait(e,ARG(1))); }
static void wfmo(CPU*c,int nargs);
HLE(kernel32,WaitForMultipleObjectsEx){ wfmo(c,5); }
HLE(kernel32,WaitForMultipleObjects){ wfmo(c,4); }
static int ev_ready(Ev*e){ extern __thread uint32_t t_tid; int r; pthread_mutex_lock(&e->mu);
  if(e->kind==1) r=e->count>0; else if(e->kind==3){ uint32_t me=t_tid?t_tid:0x1004; r=!e->owner||e->owner==me; } else r=e->state!=0;
  pthread_mutex_unlock(&e->mu); return r; }
/* bWaitAll=TRUE, BSI_EVFIX: lock every distinct object (address order, so no lock-order deadlock), test, and consume all of
   them or none.  The old path tested them one by one and then consumed them one by one, so another thread could take an
   auto-reset event or a semaphore count in between and the wait returned without owning it. */
static int cmp_evp(const void*a,const void*b){ uintptr_t x=(uintptr_t)*(Ev*const*)a, y=(uintptr_t)*(Ev*const*)b; return x<y?-1:x>y; }
static int wfmo_all_try(uint32_t n, uint32_t arr){ extern __thread uint32_t t_tid; Ev* es[64]; int m=0; uint32_t me=t_tid?t_tid:0x1004;
  for(uint32_t k=0;k<n && k<64;k++){ Ev*e=ev_get(R32(arr+4*k)); if(e) es[m++]=e; }
  qsort(es,m,sizeof *es,cmp_evp); int u=0; for(int i=0;i<m;i++) if(!u || es[u-1]!=es[i]) es[u++]=es[i]; m=u;
  for(int i=0;i<m;i++) pthread_mutex_lock(&es[i]->mu);
  int ok=1; for(int i=0;i<m && ok;i++){ Ev*e=es[i]; ok = e->kind==1 ? e->count>0 : e->kind==3 ? (!e->owner||e->owner==me) : e->state!=0; }
  if(ok) for(int i=0;i<m;i++){ Ev*e=es[i]; if(e->kind==1) e->count--; else if(e->kind==3){ e->owner=me; e->count++; } else if(!e->manual) e->state=0; }
  for(int i=m-1;i>=0;i--) pthread_mutex_unlock(&es[i]->mu);
  return ok; }
static void wfmo_log(int op, uint32_t n, uint32_t arr, uint32_t all, uint32_t ms, int res){ for(uint32_t k=0;k<n && k<8;k++){ uint32_t h=R32(arr+4*k); evl_rec(op,h,ev_get(h),op==EVO_WFMO?(ms):(k|all<<8|n<<16),res); } }
/* BSI_THRLOG=1: once per thread, the handle set of its first blocking multi-wait (identifies the WFMO-storm victim of the
   Fable note "win32-sync-cs-and-wfmo", ktid 8617 in the v0.8.4e trace; joins to the [thread] start line by Win32 id/ktid) */
static __thread int t_wfmo_logged;
static __attribute__((noinline)) void wfmo_first_log(uint32_t n, uint32_t arr, uint32_t all, uint32_t ms, uint32_t ra){
  static const char *kn[4]={"event","sem","thread","mutex"}; char b[400]; int o=0; t_wfmo_logged=1;
  for(uint32_t k=0;k<n && k<12 && o<(int)sizeof b-40;k++){ uint32_t h=R32(arr+4*k); Ev*e=ev_get(h);
    o+=snprintf(b+o,sizeof b-o," %08x/%s",h,!e?"?":e->kind>=0&&e->kind<4?(e->kind==0?(e->manual?"mevent":"aevent"):kn[e->kind]):"?"); }
  fprintf(stderr,"[wfmo] thread %04x (ktid %d) first multi-wait: n=%u all=%u ms=%d ret=%08x handles:%s%s\n",t_tid,(int)syscall(SYS_gettid),n,all,(int)ms,ra,b,n>12?" ...":""); }
static void wfmo(CPU*c,int nargs){ uint32_t n=ARG(0), arr=ARG(1), all=ARG(2), ms=ARG(3);
  if(__builtin_expect(!t_wfmo_logged,0) && ms && thr_log_on()) wfmo_first_log(n,arr,all,ms,R32(c->esp));
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0); int fix=ev_fix();
  Ev* reg[64]; int nreg=0, big=0;   /* BSI_SYNCFAST: register on every handle before the first readiness check */
  if(sync_fast()){ if(n>64){ big=1; __atomic_add_fetch(&g_wfmo_big,1,__ATOMIC_SEQ_CST); }
    else for(uint32_t k=0;k<n;k++){ Ev*e=ev_get(R32(arr+4*k)); if(e){ __atomic_add_fetch(&e->mwait,1,__ATOMIC_SEQ_CST); reg[nreg++]=e; } } }
  unsigned sg0[64]; if(fix && !all) for(uint32_t k=0;k<n && k<64;k++){ Ev*e=ev_get(R32(arr+4*k)); sg0[k]= e ? __atomic_load_n(&e->sgen,__ATOMIC_ACQUIRE) : 0; }   /* see ev_set */
  if(g_evl){ wfmo_log(EVO_WFMO,n,arr,all,ms,0); uint32_t hs[4]; for(uint32_t k=0;k<4;k++) hs[k]= k<n ? R32(arr+4*k) : 0; evl_wait_begin(hs,n,ms); }
  #define WF_UNREG() do{ for(int _i=0;_i<nreg;_i++) __atomic_sub_fetch(&reg[_i]->mwait,1,__ATOMIC_SEQ_CST); if(big) __atomic_sub_fetch(&g_wfmo_big,1,__ATOMIC_SEQ_CST); }while(0)
  #define WF_RET(v) do{ WF_UNREG(); if(g_evl){ evl_wait_end(); int _r=(v); evl_rec(EVO_WFMOED,_r>=0&&(uint32_t)_r<n?R32(arr+4*_r):0,0,n|all<<16,_r==258?0xfe:_r); } RET_STD(nargs,v); }while(0)
  for(;;){
    pthread_mutex_lock(&g_any_mu); unsigned gen=g_any_gen; pthread_mutex_unlock(&g_any_mu);
    if(!all){ for(uint32_t k=0;k<n;k++){ Ev*e=ev_get(R32(arr+4*k)); if(!e || ev_wait(e,0)==0) WF_RET(k);
        if(fix && k<64 && e->kind!=1 && e->kind!=3 && e->manual && __atomic_load_n(&e->sgen,__ATOMIC_ACQUIRE)!=sg0[k]) WF_RET(k); } }
    else if(fix){ if(wfmo_all_try(n,arr)) WF_RET(0); }
    else { int allr=1; for(uint32_t k=0;k<n;k++){ Ev*e=ev_get(R32(arr+4*k)); if(e && !ev_ready(e)){ allr=0; break; } }
      if(allr){ for(uint32_t k=0;k<n;k++){ Ev*e=ev_get(R32(arr+4*k)); if(e) ev_wait(e,0); } WF_RET(0); } }
    if(ms==0) WF_RET(258);
    pthread_mutex_lock(&g_any_mu);
    while(g_any_gen==gen){
      if(ms==0xffffffffu){ pthread_cond_wait(&g_any_cv,&g_any_mu); continue; }
      struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
      int64_t el=(now.tv_sec-t0.tv_sec)*1000LL+(now.tv_nsec-t0.tv_nsec)/1000000; if(el>=(int64_t)ms){ pthread_mutex_unlock(&g_any_mu); WF_RET(258); }
      struct timespec dl; clock_gettime(CLOCK_REALTIME,&dl); int64_t left=(int64_t)ms-el; uint64_t ns=(uint64_t)dl.tv_nsec+(uint64_t)(left%1000)*1000000ull; dl.tv_sec+=left/1000+ns/1000000000ull; dl.tv_nsec=ns%1000000000ull;
      pthread_cond_timedwait(&g_any_cv,&g_any_mu,&dl); }
    pthread_mutex_unlock(&g_any_mu); }
  #undef WF_RET
  #undef WF_UNREG
}

/* ---------------- misc kernel32 ---------------- */
HLE(kernel32,SetThreadExecutionState){ RET_STD(1,1); }
HLE(kernel32,OutputDebugStringA){ char t[1024]; g2host_a(ARG(0),t,1024); fprintf(stderr,"[dbg] %s",t); RET_STD(1,0); }
HLE(kernel32,OutputDebugStringW){ char t[1024]; g2host_w(ARG(0),t,1024); fprintf(stderr,"[dbg] %s",t); RET_STD(1,0); }
HLE(kernel32,FormatMessageW){ RET_STD(7,0); }
HLE(kernel32,GetEnvironmentVariableW){ g_last_error=203; RET_STD(3,0); }
HLE(kernel32,GetEnvironmentVariableA){ g_last_error=203; RET_STD(3,0); }
HLE(kernel32,SetEnvironmentVariableW){ RET_STD(2,1); }
HLE(kernel32,GetComputerNameW){ const char*s="QUEST"; uint32_t d=ARG(0); for(int k=0;k<6;k++) W16(d+2*k,(uint8_t)s[k]); W32(ARG(1),5); RET_STD(2,1); }
HLE(advapi32,GetUserNameW){ const char*s="player"; uint32_t d=ARG(0); for(int k=0;k<7;k++) W16(d+2*k,(uint8_t)s[k]); W32(ARG(1),7); RET_STD(2,1); }
void guest_map(uint32_t a, uint32_t n);
static uint32_t fake_module(int k){
  uint32_t b=0x7a000000u+0x10000u*k;
  static int made[16]; if(!made[k]){ made[k]=1; guest_map(b,0x1000); memset(MEMP(b),0,0x1000);
    W16(b,0x5a4d); W32(b+0x3c,0x80); W32(b+0x80,0x4550); W16(b+0x84,0x14c); W16(b+0x86,0); W32(b+0x88,0x4a000000u);
    W16(b+0x94,0xe0); W16(b+0x96,0x2102); W16(b+0x98,0x10b); W32(b+0x98+28,b); W32(b+0x98+56,0x1000); W32(b+0x98+92,16); }
  return b;
}
static int known_dll(const char*t){ const char*k[]={"dxgi.dll","d3d11.dll","kernel32.dll",0}; for(int i=0;k[i];i++) if(!strcasecmp(t,k[i])) return i+1; return 0; }
uint32_t dll_load(CPU*c,const char*n); uint32_t dll_handle(const char*n); uint32_t dll_export(uint32_t base,const char*name,uint32_t ord);
HLE(kernel32,LoadLibraryW){ char t[512]; g2host_w(ARG(0),t,512); int k=known_dll(t); if(k) RET_STD(1,fake_module(k)); { uint32_t h=dll_load(c,t); if(h) RET_STD(1,h); } fprintf(stderr,"LoadLibraryW(%s) -> NULL\n",t); g_last_error=126; RET_STD(1,0); }
HLE(kernel32,LoadLibraryA){ char t[512]; g2host_a(ARG(0),t,512); int k=known_dll(t); if(k) RET_STD(1,fake_module(k)); { uint32_t h=dll_load(c,t); if(h) RET_STD(1,h); } fprintf(stderr,"LoadLibraryA(%s) -> NULL\n",t); g_last_error=126; RET_STD(1,0); }
HLE(kernel32,FreeLibrary){ RET_STD(1,1); }
uint32_t sysmod_handle(const char*t){ const char*s=strrchr(t,'\\'); s=s?s+1:t; char b[128]; snprintf(b,sizeof b,"%s",s); if(!strchr(b,'.')) strcat(b,".dll"); int k=known_dll(b); return k?fake_module(k):0; }
uint32_t proc_for(const char*dll,const char*name);
uint32_t dyn_lookup(const char *n);
HLE(kernel32,GetProcAddress){ char t[256]; if(ARG(1)<0x10000){ RET_STD(2,dll_export(ARG(0),0,ARG(1))); } g2host_a(ARG(1),t,256);
  { uint32_t a=dll_export(ARG(0),t,0); if(a) RET_STD(2,a); }
  if(ARG(0)==0x7a030000u){ uint32_t a=proc_for("KERNEL32.dll",t); if(!a) fprintf(stderr,"GetProcAddress(kernel32,%s) -> NULL\n",t); RET_STD(2,a); }
  uint32_t a=dyn_lookup(t); if(!a) fprintf(stderr,"GetProcAddress(%08x,%s) -> NULL\n",ARG(0),t); RET_STD(2,a); }
HLE(kernel32,GetDiskFreeSpaceExW){ if(ARG(1)) W64(ARG(1),100ull<<30); if(ARG(2)) W64(ARG(2),200ull<<30); if(ARG(3)) W64(ARG(3),100ull<<30); RET_STD(4,1); }
HLE(kernel32,GetDriveTypeW){ RET_STD(1,3); }
HLE(kernel32,GetTempPathW){ const char*s="C:\\BSIUSER\\Temp\\"; uint32_t d=ARG(1); int k=0; for(;s[k];k++) W16(d+2*k,s[k]); W16(d+2*k,0); RET_STD(2,k); }
HLE(kernel32,GetUserDefaultLangID){ RET_STD(0,0x409); }
HLE(kernel32,GetUserDefaultLCID){ RET_STD(0,0x409); }
HLE(kernel32,GetSystemDefaultLangID){ RET_STD(0,0x409); }
HLE(kernel32,GetUserDefaultUILanguage){ RET_STD(0,0x409); }
HLE(kernel32,GetSystemDefaultUILanguage){ RET_STD(0,0x409); }
HLE(kernel32,GetLocaleInfoW){ RET_STD(4,0); }
HLE(kernel32,InitializeSListHead){ W64(ARG(0),0); RET_STD(1,0); }
static pthread_mutex_t slist_mu=PTHREAD_MUTEX_INITIALIZER;
HLE(kernel32,InterlockedPushEntrySList){ pthread_mutex_lock(&slist_mu); uint32_t h=ARG(0), e=ARG(1), old=R32(h); W32(e,old); W32(h,e); W16(h+4,R16(h+4)+1); W16(h+6,R16(h+6)+1); pthread_mutex_unlock(&slist_mu); RET_STD(2,old); }
HLE(kernel32,InterlockedPopEntrySList){ pthread_mutex_lock(&slist_mu); uint32_t h=ARG(0), e=R32(h); if(e){ W32(h,R32(e)); W16(h+4,R16(h+4)-1);} pthread_mutex_unlock(&slist_mu); RET_STD(1,e); }
HLE(kernel32,InterlockedFlushSList){ pthread_mutex_lock(&slist_mu); uint32_t h=ARG(0), e=R32(h); W32(h,0); W16(h+4,0); pthread_mutex_unlock(&slist_mu); RET_STD(1,e); }
HLE(kernel32,QueryDepthSList){ RET_STD(1,R16(ARG(0)+4)); }
HLE(ole32,CoInitialize){ RET_STD(1,0); }
HLE(ole32,CoInitializeEx){ RET_STD(2,0); }
HLE(ole32,CoUninitialize){ RET_STD(0,0); }
HLE(ole32,CoCreateInstance){ { uint8_t *g=MEMP(ARG(0)), *i=MEMP(ARG(3)); fprintf(stderr,"[com] CoCreateInstance clsid %08x-%04x-%04x iid %08x-%04x-%04x\n",*(uint32_t*)g,*(uint16_t*)(g+4),*(uint16_t*)(g+6),*(uint32_t*)i,*(uint16_t*)(i+4),*(uint16_t*)(i+6)); }
  { uint32_t xaudio_create(void); if(R32(ARG(0))==0x5a508685u && R32(ARG(0)+4)==0x4fbaa254u){ W32(ARG(4),xaudio_create()); fprintf(stderr,"[xaudio] XAudio2 2.7 created\n"); RET_STD(5,0); } }
  if(ARG(4)) W32(ARG(4),0); RET_STD(5,0x80040154u); }   /* REGDB_E_CLASSNOTREG */
HLE(oleaut32,SysAllocString){ uint32_t s=ARG(0); if(!s) RET_STD(1,0); uint32_t n=gwcslen(s); uint32_t p=gmalloc(2*n+6); W32(p,2*n); memcpy(MEMP(p+4),MEMP(s),2*n+2); RET_STD(1,p+4); }
HLE(oleaut32,SysFreeString){ if(ARG(0)) gfree(ARG(0)-4); RET_STD(1,0); }
HLE(powrprof,CallNtPowerInformation){ uint32_t out=ARG(3), n=ARG(4); memset(MEMP(out),0,n); for(uint32_t k=0;k+24<=n;k+=24){ W32(out+k,k/24); W32(out+k+4,3000); W32(out+k+8,3000); W32(out+k+12,3000);} RET_STD(5,0); }
HLE(ws2_32,WSAStartup){ uint32_t p=ARG(1); memset(MEMP(p),0,400); W16(p,0x202); W16(p+2,0x202); RET_STD(2,0); }
HLE(wsock32,WSAStartup){ uint32_t p=ARG(1); memset(MEMP(p),0,400); W16(p,0x202); W16(p+2,0x202); RET_STD(2,0); }
/* steam_api glue, offline: SteamAPI_Init reports failure (no Steam client), context init returns a valid (empty) context */
HLE(steam_api,SteamAPI_Init){ const char *e=getenv("BSI_STEAM_INIT"); int v=e?atoi(e):0; fprintf(stderr,"[steam] SteamAPI_Init -> %d\n",v); RET_C(v); }   /* BSI_STEAM_INIT=1: report success (offline local user, local_profile.c) */
HLE(steam_api,SteamAPI_RestartAppIfNecessary){ RET_C(0); }
uint32_t local_profile_find_interface(uint32_t version_ptr);
HLE(steam_api,SteamInternal_FindOrCreateUserInterface){
  { char v[96]; g2host_a(ARG(1), v, sizeof v); static int n; if(n++<40) fprintf(stderr,"[steam] FindOrCreateUserInterface(%u, %s) from %08x\n", ARG(0), v, R32(c->esp)); }
  RET_C(local_profile_find_interface(ARG(1)));
}
HLE(steam_api,SteamInternal_ContextInit){
  uint32_t d=ARG(0);                 /* { void (*pFn)(void*ctx); uint32 counter; ctx[...] } */
  if(R32(d+4)!=1){ W32(d+4,1); uint32_t fn=R32(d), ret=R32(c->esp); c->esp+=4; PUSH32(c,d+8); guest_call(c,fn); c->esp+=4; c->esp-=4; W32(c->esp,ret); }
  RET_C(d+8);
}

/* ---- process exit + diagnostics */
void guest_stackscan(CPU*c, const char*why){
  fprintf(stderr,"[%s] eip-ish stack scan from esp=%08x:\n",why,c->esp);
  int n=0;
  uint32_t top=(c->esp|0xFFFFu)+1u; if(top-c->esp>0x2000u) top=c->esp+0x2000u;   /* b74: stop at the stack top (64 KB aligned); b73 read past it and faulted */
  for(uint32_t a=c->esp; a<top && n<40; a+=4){ uint32_t v=R32(a);
    if(v>=0x401000u && v<0x10d4000u){ fprintf(stderr,"  [%08x] %08x\n",a,v); n++; } }
}
HLE(kernel32,TerminateProcess){ guest_stackscan(c,"TerminateProcess"); fprintf(stderr,"TerminateProcess(%08x,%08x)\n",ARG(0),ARG(1)); fflush(0); _exit(3); }
HLE(kernel32,ExitProcess){ fprintf(stderr,"ExitProcess(%u)\n",ARG(0)); if(g_evl==1) evl_dump("ExitProcess"); fflush(0); _exit(ARG(0)&0xff); }

/* Win32 mutexes: recursive, owned by a thread id */
extern __thread uint32_t t_tid;
static uint32_t mutex_new(int own){ uint32_t h=ev_new(3,0,0); Ev*e=ev_get(h); if(e){ e->owner=own?(t_tid?t_tid:0x1004):0; e->count=own?1:0; } return h; }
HLE(kernel32,CreateMutexW){ g_last_error=0; RET_STD(3,mutex_new(ARG(1)!=0)); }
HLE(kernel32,CreateMutexA){ g_last_error=0; RET_STD(3,mutex_new(ARG(1)!=0)); }
HLE(kernel32,ReleaseMutex){ Ev*e=ev_get(ARG(0)); if(!e||e->kind!=3) RET_STD(1,1); uint32_t me=t_tid?t_tid:0x1004; pthread_mutex_lock(&e->mu); int ok=0;
  if(e->owner==me && e->count>0){ ok=1; if(--e->count==0){ e->owner=0; EV_BCAST(e); } } pthread_mutex_unlock(&e->mu); if(ok) EV_POKE(e); if(g_evl) evl_rec(EVO_MTXREL,ARG(0),e,0,ok); RET_STD(1,ok); }
/* CloseHandle on events/semaphores/mutexes frees the slot (UE3 creates FEvents for every async task) */
int ev_close(uint32_t h){ Ev*e=ev_get(h); if(!e){ if(g_evl && h>=0x40000u && h<0x40000u+4*NEV) evl_rec(EVO_STALE_CLOSE,h,0,0,0); return 0; } if(g_evl) evl_rec(EVO_CLOSE,h,e,0,0); if(e->kind==2) return 1;
  pthread_mutex_lock(&ev_mu); e->used=0; pthread_mutex_unlock(&ev_mu); return 1; }

/* BSI_SCHED thread policy (env-gated, off by default): notes/SCHED.md */
#include "sched_policy.c"
