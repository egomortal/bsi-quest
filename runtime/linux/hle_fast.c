/* hle_fast.c: native fast paths for the hottest Win32 HLE calls (notes/HLE_HOT.md).  #included by rt_linux.c just
   before call_import() (regress/hle/rt_linux_hlefast.patch), so it sees Imp, g_imps, g_lazy, g_trace, LOGF, the
   arena constants and nfr.  Nothing here changes behaviour unless its switch is on; hf_init() runs once in main()
   after patch_iat(), before any guest code, and only swaps Imp.fn pointers (exe imports).  DLL imports and
   GetProcAddress results copy those pointers by name when they are bound (hle_dll.c resolve()), so every caller of
   an API gets the same implementation (required for critical sections).

   Switches (env; android env.txt passes BSI_*):
     BSI_HLEFAST        master switch, default 0 (job3 2026-10-05: default OFF for v0.8.8): unset/0 = none of the
                        below is installed, call_import is unchanged (one compare added); =1 = the defaults below
     BSI_IMPFAST        default 1: direct import table in call_import (skips imp_at, the trace/lazy checks and the
                        global n_calls++ that every core writes on every import call); off while BSI_NOTRACE is unset
     BSI_FASTCS         default 1: hle_fastcs.c   inline critical sections (no hash lock, no pthread mutex)
     BSI_VMCACHE        default 1: hle_vmcache.c  VirtualAlloc/VirtualFree reuse cache + MEM_DECOMMIT
     BSI_THRFREE        default 1: hle_vmcache.c  free the 4 MB guest stack of exited threads
     BSI_PATHCACHE      default 1: hle_pathcache.c memoised attribute/stat probes under C:\BSI
     BSI_QPCFAST        default 1: hle_qpc.c      QPC from CNTVCT_EL0 (aarch64 only, self-checked)
   Diagnostics (off): BSI_HLESTAT=1 per-import calls per frame per thread class (GT/RT/other) every 10 s;
     BSI_FASTCS_STAT, BSI_VMCACHE_STAT, BSI_PATHCACHE_STAT (printed with the same 10 s report). */
#include "hle.h"
#include "hle_fastcs.c"
#include "hle_vmcache.c"
#include "hle_pathcache.c"
#include "hle_qpc.c"

static hle_fn *g_hf_tab; static uint32_t g_hf_n;          /* import index -> handler, exe imports only */
static int g_hf_stat, g_hf_anystat;
static __thread int t_hf_seen;
extern volatile long g_br_presents __attribute__((weak));

/* ---- BSI_HLESTAT: per-thread counters, summed by the reporter (racy reads are fine for statistics) */
typedef struct HfThr { uint32_t *cnt; int ktid, cls; struct HfThr *next; } HfThr;
static HfThr *g_hf_thr; static pthread_mutex_t g_hf_thr_mu=PTHREAD_MUTEX_INITIALIZER; static int g_hf_gt_done;
static __thread uint32_t *t_hf_cnt;
static uint64_t g_hf_rep_ns; static long g_hf_rep_frames; static uint32_t g_hf_tick;

static uint64_t hf_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static __attribute__((noinline)) void hf_thread_first(CPU*c){
  t_hf_seen=1;
  thrfree_note(c);
  if(g_hf_stat){
    HfThr *t=calloc(1,sizeof *t); if(!t) return; t->cnt=calloc(g_hf_n?g_hf_n:1,4); if(!t->cnt){ free(t); return; }
    t->ktid=(int)syscall(SYS_gettid);
    pthread_mutex_lock(&g_hf_thr_mu); extern __thread uint32_t t_tid;
    t->cls= (!g_hf_gt_done && !t_tid) ? 1 : 0; if(t->cls) g_hf_gt_done=1;   /* first guest thread without a Win32 id = GameThread */
    t->next=g_hf_thr; g_hf_thr=t; pthread_mutex_unlock(&g_hf_thr_mu);
    t_hf_cnt=t->cnt;
  }
}
static int hf_cls_name(const HfThr *t){
  if(t->cls==1) return 1;
  char p[64], nm[32]={0}; snprintf(p,sizeof p,"/proc/self/task/%d/comm",t->ktid); FILE *f=fopen(p,"r");
  if(f){ if(!fgets(nm,sizeof nm,f)) nm[0]=0; fclose(f); }
  return strncmp(nm,"RenderingThread",15)==0 ? 2 : 0;
}
static void hf_report(void){
  uint64_t now=hf_now(); if(!g_hf_rep_ns){ g_hf_rep_ns=now; g_hf_rep_frames=&g_br_presents?g_br_presents:0; return; }
  if(now-g_hf_rep_ns<10000000000ull) return;
  static pthread_mutex_t rep_mu=PTHREAD_MUTEX_INITIALIZER; if(pthread_mutex_trylock(&rep_mu)) return;
  if(now-g_hf_rep_ns<10000000000ull){ pthread_mutex_unlock(&rep_mu); return; }
  double secs=(now-g_hf_rep_ns)/1e9; long fr=&g_br_presents?g_br_presents:0; long df=fr-g_hf_rep_frames; g_hf_rep_ns=now; g_hf_rep_frames=fr;
  double per= df>0 ? (double)df : secs; const char *unit= df>0 ? "frame" : "s";
  if(g_hf_stat && g_hf_n){
    uint64_t *tot=calloc(g_hf_n,8*4);   /* [idx][0 all,1 GT,2 RT,3 other] */
    static uint64_t *prev; if(!prev) prev=calloc(g_hf_n,8*4);
    if(tot && prev){
      pthread_mutex_lock(&g_hf_thr_mu);
      for(HfThr *t=g_hf_thr;t;t=t->next){ int cl=hf_cls_name(t), col= cl==1?1: cl==2?2:3; for(uint32_t i=0;i<g_hf_n;i++){ uint32_t v=__atomic_load_n(&t->cnt[i],__ATOMIC_RELAXED); tot[4*i]+=v; tot[4*i+col]+=v; } }
      pthread_mutex_unlock(&g_hf_thr_mu);
      uint64_t all=0; for(uint32_t i=0;i<g_hf_n;i++){ for(int k=0;k<4;k++){ uint64_t v=tot[4*i+k]; tot[4*i+k]=v-prev[4*i+k]; prev[4*i+k]=v; } all+=tot[4*i]; }
      fprintf(stderr,"[hlestat] %.1f s, %ld frames: %.0f import calls per %s (fast path only); top: per %s  all / GT / RT / other\n",secs,df,all/per,unit,unit);
      for(int r=0;r<30;r++){ uint32_t best=0; uint64_t bv=0; for(uint32_t i=0;i<g_hf_n;i++) if(tot[4*i]>bv){ bv=tot[4*i]; best=i; } if(!bv) break;
        const char *n=g_imps[best].name, *b=strchr(n,'!');
        fprintf(stderr,"[hlestat]   %9.1f %9.1f %9.1f %9.1f  %s\n",tot[4*best]/per,tot[4*best+1]/per,tot[4*best+2]/per,tot[4*best+3]/per,b?b+1:n); tot[4*best]=0; }
    }
    free(tot);
    pthread_mutex_lock(&heap_mu); int nf=nfr; uint32_t brk=heap_brk; pthread_mutex_unlock(&heap_mu);
    fprintf(stderr,"[hlestat] page arena free ranges %d, small heap %u MB\n",nf,(brk-HEAP_LO)>>20);
  }
  fcs_report(stderr); vmc_report(stderr); pc_report(stderr);
  pthread_mutex_unlock(&rep_mu);
}
/* call_import fast path: returns 1 if handled */
static inline int hf_imp_fast(CPU*c, uint32_t v){
  uint32_t i=(v-IMP_BASE)>>2;
  if(__builtin_expect(i>=g_hf_n || (v&3),0)) return 0;
  hle_fn f=g_hf_tab[i]; if(__builtin_expect(!f,0)) return 0;
  if(__builtin_expect(!t_hf_seen,0)) hf_thread_first(c);
  if(__builtin_expect(g_hf_anystat,0)){ if(t_hf_cnt) t_hf_cnt[i]++; if(!(++g_hf_tick&0xffff)) hf_report(); }
  f(c); return 1;
}

static int hf_env(const char *n, int def){ const char *s=getenv(n); return s&&*s ? atoi(s)!=0 : def; }
/* swap the handler of "DLL!name" (case-insensitive) in the exe import table; returns the old handler */
static hle_fn hf_swap(const char *full, hle_fn nf){
  hle_fn old=0;
  for(int k=0;k<g_nimps;k++) if(!strcasecmp(g_imps[k].name,full)){ if(!old) old=g_imps[k].fn; g_imps[k].fn=nf; }
  return old;
}
#define HF_SWAP(full,nf,oldvar) do{ hle_fn o_=hf_swap(full,(hle_fn)(nf)); if(o_) oldvar=(void*)o_; else fprintf(stderr,"[hlefast] %s not imported\n",full); }while(0)
static void hf_init(void){
  if(!hf_env("BSI_HLEFAST",0)){ fprintf(stderr,"[hlefast] off (BSI_HLEFAST unset or 0)\n"); return; }
  thrfree_init();
  int cs=hf_env("BSI_FASTCS",1), vm=hf_env("BSI_VMCACHE",1), pc=hf_env("BSI_PATHCACHE",1), qp=hf_env("BSI_QPCFAST",1), imp=hf_env("BSI_IMPFAST",1);
  if(cs){ fcs_config();
    /* all three lock entry points must switch together (they share the CS memory protocol) */
    hle_fn e=hf_swap("KERNEL32.dll!EnterCriticalSection",0), l=hf_swap("KERNEL32.dll!LeaveCriticalSection",0), t=hf_swap("KERNEL32.dll!TryEnterCriticalSection",0);
    if(e && l && t){ fcs_old_enter=e; fcs_old_leave=l; fcs_old_try=t;
      hf_swap("KERNEL32.dll!EnterCriticalSection",hf_EnterCriticalSection); hf_swap("KERNEL32.dll!LeaveCriticalSection",hf_LeaveCriticalSection);
      hf_swap("KERNEL32.dll!TryEnterCriticalSection",hf_TryEnterCriticalSection);
      /* Initialize*: optional swaps (the old handlers also just zero the 24 bytes, which is the free state here) */
      hle_fn o=hf_swap("KERNEL32.dll!InitializeCriticalSection",hf_InitializeCriticalSection); if(o) fcs_old_init=o;
      o=hf_swap("KERNEL32.dll!InitializeCriticalSectionAndSpinCount",hf_InitializeCriticalSectionAndSpinCount); if(o) fcs_old_init_spin=o;
      fprintf(stderr,"[hlefast] FASTCS on (spin %d)\n",g_fcs_spin); }
    else { if(e) hf_swap("KERNEL32.dll!EnterCriticalSection",e); if(l) hf_swap("KERNEL32.dll!LeaveCriticalSection",l); if(t) hf_swap("KERNEL32.dll!TryEnterCriticalSection",t);
      fprintf(stderr,"[hlefast] FASTCS not installed (lock imports missing)\n"); } }
  if(vm){ vmc_config(); hle_fn a=hf_swap("KERNEL32.dll!VirtualAlloc",hf_VirtualAlloc), f=hf_swap("KERNEL32.dll!VirtualFree",hf_VirtualFree);
    if(a && f){ vmc_old_alloc=a; vmc_old_free=f; fprintf(stderr,"[hlefast] VMCACHE on (%llu MB, <= %u KB blocks, age %llu ms)\n",(unsigned long long)(g_vmc_cap>>20),g_vmc_maxsz>>10,(unsigned long long)(g_vmc_age_ns/1000000)); }
    else { if(a) hf_swap("KERNEL32.dll!VirtualAlloc",a); if(f) hf_swap("KERNEL32.dll!VirtualFree",f); } }
  if(pc){ pc_config(); int ok=1;
    hle_fn o;
#define PCS(full,nf,var) o=hf_swap(full,nf); if(o) var=o; else ok=0;
    PCS("KERNEL32.dll!CreateFileW",hf_CreateFileW,pc_old_openw) PCS("KERNEL32.dll!DeleteFileW",hf_DeleteFileW,pc_old_del)
    PCS("KERNEL32.dll!CreateDirectoryW",hf_CreateDirectoryW,pc_old_mkdir) PCS("KERNEL32.dll!SetCurrentDirectoryW",hf_SetCurrentDirectoryW,pc_old_cwd)
    PCS("KERNEL32.dll!GetFileAttributesW",hf_GetFileAttributesW,pc_old_attr) PCS("MSVCR90.dll!_wstat64i32",hf__wstat64i32,pc_old_wstat)
    PCS("MSVCR90.dll!_stat64i32",hf__stat64i32,pc_old_stata)
#undef PCS
    o=hf_swap("KERNEL32.dll!GetFileAttributesExW",hf_GetFileAttributesExW); if(o) pc_old_attrex=o;
    o=hf_swap("KERNEL32.dll!CreateFileA",hf_CreateFileA); if(o) pc_old_opena=o;
    fprintf(stderr,"[hlefast] PATHCACHE on%s\n",ok?"":" (some imports missing)"); }
  if(qp && qpc_selftest(stderr)){ hf_swap("KERNEL32.dll!QueryPerformanceCounter",hf_QueryPerformanceCounter); hf_swap("KERNEL32.dll!QueryPerformanceFrequency",hf_QueryPerformanceFrequency); }
  g_hf_stat=hf_env("BSI_HLESTAT",0);
  g_hf_anystat=g_hf_stat||g_fcs_stat||g_vmc_stat||g_pc_stat;
  if(g_trace){ fprintf(stderr,"[hlefast] import fast path off: import trace is on (BSI_NOTRACE unset); THRFREE and HLESTAT need it\n"); return; }
  if(imp || g_hf_anystat || g_thrfree_on){
    hle_fn *t=calloc(g_nimps?g_nimps:1,sizeof *t);
    if(t){ for(int k=0;k<g_nimps;k++) if(!(k<1024 && g_lazy[k])) t[k]=g_imps[k].fn;
      g_hf_tab=t; __atomic_store_n(&g_hf_n,(uint32_t)g_nimps,__ATOMIC_RELEASE);
      fprintf(stderr,"[hlefast] import fast path on (%d imports)%s\n",g_nimps,imp?"":" [stats/thrfree only]"); }
  }
}
