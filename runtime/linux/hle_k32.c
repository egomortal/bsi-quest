/* HLE: KERNEL32 / ntdll / winmm / user32 subset */
#include "hle.h"
uint32_t gmalloc_c(uint32_t n, uint32_t caller);
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

uint32_t g_last_error;
#define SETERR(e) (g_last_error=(e))
HLE(kernel32,GetLastError){ RET_STD(0,g_last_error); }
HLE(kernel32,SetLastError){ g_last_error=ARG(0); RET_STD(1,0); }

HLE(kernel32,GetStartupInfoA){ uint32_t p=ARG(0); memset(MEMP(p),0,68); W32(p,68); RET_STD(1,0); }
HLE(kernel32,GetStartupInfoW){ uint32_t p=ARG(0); memset(MEMP(p),0,68); W32(p,68); RET_STD(1,0); }
HLE(kernel32,SetUnhandledExceptionFilter){ RET_STD(1,0); }
HLE(kernel32,IsDebuggerPresent){ RET_STD(0,0); }

/* interlocked (guest memory is shared between host threads) */
HLE(kernel32,InterlockedCompareExchange){ uint32_t *p=(uint32_t*)MEMP(ARG(0)); uint32_t old=__sync_val_compare_and_swap(p,ARG(2),ARG(1)); RET_STD(3,old); }
HLE(kernel32,InterlockedExchange){ uint32_t *p=(uint32_t*)MEMP(ARG(0)); RET_STD(2,__sync_lock_test_and_set(p,ARG(1))); }
HLE(kernel32,InterlockedIncrement){ uint32_t *p=(uint32_t*)MEMP(ARG(0)); RET_STD(1,__sync_add_and_fetch(p,1)); }
HLE(kernel32,InterlockedDecrement){ uint32_t *p=(uint32_t*)MEMP(ARG(0)); RET_STD(1,__sync_sub_and_fetch(p,1)); }
HLE(kernel32,InterlockedExchangeAdd){ uint32_t *p=(uint32_t*)MEMP(ARG(0)); RET_STD(2,__sync_fetch_and_add(p,ARG(1))); }

/* critical sections: host recursive mutex per guest CRITICAL_SECTION address */
#define CS_MAX 65536
static struct { uint32_t addr; pthread_mutex_t mu; } cs_tab[CS_MAX]; static pthread_mutex_t cs_tab_mu=PTHREAD_MUTEX_INITIALIZER;
/* BSI_SYNCFAST (notes/GAMETHREAD_HOT.md; default 0 = previous code, 1 = lock-free lookup):  Slots are never freed and an
   address is published (release store) only after its mutex is initialised, so a reader that finds its address can use
   the mutex without cs_tab_mu; a reader that reaches an empty slot falls back to the locked insert path, which rescans.
   Previously every Enter/Leave/TryEnter took the process-wide cs_tab_mu twice (GMalloc's thread-safe proxy alone does an
   Enter+Leave per guest malloc/free, and Wwise uses 220 call sites), so all threads contended on one lock. */
static int g_cs_fast=-1;
static pthread_mutex_t* cs_get(uint32_t a){
  uint32_t h=(a>>3)*2654435761u % CS_MAX;
  if(__builtin_expect(g_cs_fast<0,0)){ const char*s=getenv("BSI_SYNCFAST"); g_cs_fast= s&&*s ? atoi(s)!=0 : 0; }
  if(g_cs_fast) for(uint32_t i=h;;i=(i+1)%CS_MAX){ uint32_t x=__atomic_load_n(&cs_tab[i].addr,__ATOMIC_ACQUIRE); if(x==a) return &cs_tab[i].mu; if(!x) break; }
  pthread_mutex_lock(&cs_tab_mu);
  for(;;h=(h+1)%CS_MAX){
    if(cs_tab[h].addr==a) break;
    if(!cs_tab[h].addr){ pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&cs_tab[h].mu,&at);
      __atomic_store_n(&cs_tab[h].addr,a,__ATOMIC_RELEASE);   /* publish after init (lock-free readers) */
      /* log-only watermark (FABLE_REVIEW): slots are never freed (DeleteCriticalSection is a no-op) and this probe loops
         forever once all CS_MAX slots are used, so say so long before that happens. BSI_FASTCS (hle_fastcs.c) has no table. */
      static unsigned cs_used; cs_used++;
      if(cs_used==CS_MAX/4 || cs_used==CS_MAX/2 || cs_used==CS_MAX*3/4 || cs_used==CS_MAX*15/16)
        fprintf(stderr,"[cs] critical-section table: %u of %u slots used (never freed; full = hang in cs_get)\n",cs_used,(unsigned)CS_MAX);
      break; }
  }
  pthread_mutex_unlock(&cs_tab_mu);
  return &cs_tab[h].mu;
}
static void cs_reinit(uint32_t a){ pthread_mutex_t *m=cs_get(a); pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(m,&at); }
HLE(kernel32,InitializeCriticalSection){ memset(MEMP(ARG(0)),0,24); cs_reinit(ARG(0)); RET_STD(1,0); }
HLE(kernel32,InitializeCriticalSectionAndSpinCount){ memset(MEMP(ARG(0)),0,24); cs_reinit(ARG(0)); RET_STD(2,1); }
HLE(kernel32,SetCriticalSectionSpinCount){ RET_STD(2,0); }
HLE(kernel32,DeleteCriticalSection){ RET_STD(1,0); }
HLE(kernel32,EnterCriticalSection){ pthread_mutex_lock(cs_get(ARG(0))); RET_STD(1,0); }
HLE(kernel32,LeaveCriticalSection){ pthread_mutex_unlock(cs_get(ARG(0))); RET_STD(1,0); }
HLE(kernel32,TryEnterCriticalSection){ RET_STD(1, pthread_mutex_trylock(cs_get(ARG(0)))==0); }

/* TLS: slots in the guest TEB (fs:0xE10) */
static uint32_t tls_used;
HLE(kernel32,TlsAlloc){ for(int k=0;k<64;k++) if(!(tls_used&(1u<<k)) && k<32){ tls_used|=1u<<k; RET_STD(0,k);} RET_STD(0,0xffffffffu); }
HLE(kernel32,TlsFree){ if(ARG(0)<32) tls_used&=~(1u<<ARG(0)); RET_STD(1,1); }
HLE(kernel32,TlsGetValue){ g_last_error=0; RET_STD(1,R32(c->fs_base+0xE10+4*ARG(0))); }
HLE(kernel32,TlsSetValue){ W32(c->fs_base+0xE10+4*ARG(0),ARG(1)); RET_STD(2,1); }

/* memory */
int sched_policy_ncpu(void) __attribute__((weak));
void sched_policy_win_request(uint32_t h, int kind, int32_t val) __attribute__((weak));
/* processor count: 4 unless BSI_NCPU (sched_policy.c).  UE3 sizes PoolThread and XJob workers as max(n-2,1), a third
   pool as min(n-2,4), and starts the RenderingThread only for n > 1 (notes/SCHED.md) */
HLE(kernel32,GetSystemInfo){ uint32_t p=ARG(0); int n=sched_policy_ncpu?sched_policy_ncpu():0; if(n<1) n=4; memset(MEMP(p),0,36); W32(p+4,4096); W32(p+8,0x10000); W32(p+12,0x7ffeffff); W32(p+16,(1u<<n)-1); W32(p+20,n); W32(p+24,586); W32(p+28,0x10000); W16(p+32,6); RET_STD(1,0); }
HLE(kernel32,VirtualAlloc){ uint32_t a=ARG(0), n=ARG(1); if(a) RET_STD(4,a); RET_STD(4,gpages(n)); }
void gpages_free(uint32_t a);
HLE(kernel32,VirtualFree){ if(ARG(2)&0x8000) gpages_free(ARG(0)); RET_STD(3,1); }
HLE(kernel32,VirtualProtect){ if(ARG(3)) W32(ARG(3),4); RET_STD(4,1); }
HLE(kernel32,GlobalMemoryStatusEx){ uint32_t p=ARG(0); W32(p+4,30); W64(p+8,8ull<<30); W64(p+16,6ull<<30); W64(p+24,16ull<<30); W64(p+32,12ull<<30); W64(p+40,0x7ffe0000ull); W64(p+48,0x7ff00000ull); W64(p+56,0); RET_STD(1,1); }
HLE(kernel32,GetProcessHeap){ RET_STD(0,0x00150000); }
HLE(kernel32,HeapAlloc){ uint32_t p=gmalloc_c(ARG(2),R32(c->esp)); if(ARG(1)&8) memset(MEMP(p),0,ARG(2)); RET_STD(3,p); }
HLE(kernel32,HeapFree){ gfree(ARG(2)); RET_STD(3,1); }
HLE(kernel32,HeapSize){ RET_STD(3,gsize(ARG(2))); }
HLE(kernel32,HeapReAlloc){ uint32_t p=ARG(2),n=ARG(3),o=gsize(p); uint32_t q=gmalloc_c(n,R32(c->esp)); memcpy(MEMP(q),MEMP(p),o<n?o:n); gfree(p); RET_STD(4,q); }
HLE(kernel32,LocalAlloc){ uint32_t p=gmalloc_c(ARG(1),R32(c->esp)); if(ARG(0)&0x40) memset(MEMP(p),0,ARG(1)); RET_STD(2,p); }
HLE(kernel32,LocalFree){ gfree(ARG(0)); RET_STD(1,0); }

/* strings */
HLE(kernel32,lstrlenW){ RET_STD(1, ARG(0)?gwcslen(ARG(0)):0); }
HLE(kernel32,lstrlenA){ RET_STD(1, ARG(0)?gstrlen(ARG(0)):0); }
HLE(kernel32,WideCharToMultiByte){ /* (cp,flags,src,srclen,dst,dstlen,defchar,useddef) */
  uint32_t src=ARG(2); int32_t n=(int32_t)ARG(3); uint32_t dst=ARG(4), cap=ARG(5);
  if(n<0) n=gwcslen(src)+1;
  if(!cap) RET_STD(8,n);
  uint32_t m=(uint32_t)n<cap?(uint32_t)n:cap; for(uint32_t k=0;k<m;k++){ uint16_t ch=R16(src+2*k); W8(dst+k,ch<256?ch:'?'); }
  RET_STD(8,m);
}
HLE(kernel32,MultiByteToWideChar){ /* (cp,flags,src,srclen,dst,dstlen) */
  uint32_t src=ARG(2); int32_t n=(int32_t)ARG(3); uint32_t dst=ARG(4), cap=ARG(5);
  if(n<0) n=gstrlen(src)+1;
  if(!cap) RET_STD(6,n);
  uint32_t m=(uint32_t)n<cap?(uint32_t)n:cap; for(uint32_t k=0;k<m;k++) W16(dst+2*k,R8(src+k));
  RET_STD(6,m);
}

/* time */
static struct timespec t0; static int t0set;
static uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
HLE(kernel32,QueryPerformanceFrequency){ W64(ARG(0),1000000000ull); RET_STD(1,1); }
HLE(kernel32,QueryPerformanceCounter){ W64(ARG(0),now_ns()); RET_STD(1,1); }
HLE(kernel32,GetTickCount){ RET_STD(0,(uint32_t)(now_ns()/1000000)); }
HLE(winmm,timeGetTime){ RET_STD(0,(uint32_t)(now_ns()/1000000)); }
HLE(winmm,timeBeginPeriod){ RET_STD(1,0); }
HLE(winmm,timeEndPeriod){ RET_STD(1,0); }
#include "sleep0.c"   /* Step B: adaptive Sleep(0), BSI_SLEEP0 */
HLE(kernel32,Sleep){ uint32_t ms=ARG(0); if(ms){ s0_note_wait(); usleep(ms*1000); } else sleep0(R32(c->esp),R32(c->esp+16)); RET_STD(1,0); }
HLE(kernel32,GetLocalTime){ uint32_t p=ARG(0); time_t t=time(0); struct tm tm; localtime_r(&t,&tm);
  W16(p,tm.tm_year+1900); W16(p+2,tm.tm_mon+1); W16(p+4,tm.tm_wday); W16(p+6,tm.tm_mday); W16(p+8,tm.tm_hour); W16(p+10,tm.tm_min); W16(p+12,tm.tm_sec); W16(p+14,0); RET_STD(1,0); }
HLE(kernel32,GetSystemTimeAsFileTime){ uint64_t ft=((uint64_t)time(0)+11644473600ull)*10000000ull; W64(ARG(0),ft); RET_STD(1,0); }
extern __thread uint32_t t_tid;
HLE(kernel32,GetCurrentThreadId){ if(!t_tid) t_tid=0x1004; RET_STD(0,t_tid); }   /* main thread: fixed id; others set in thr_main */
HLE(kernel32,GetCurrentProcessId){ RET_STD(0,1234); }
HLE(kernel32,GetCurrentProcess){ RET_STD(0,0xffffffffu); }
HLE(kernel32,GetCurrentThread){ RET_STD(0,0xfffffffeu); }

/* process info */
extern const char *g_exe_path;
const char *g_guest_exe = "C:\\BSI\\Binaries\\Win32\\BioShockInfinite.exe";
static uint32_t g_cmdline_w, g_cmdline_a;
static const char *cmdline(void){ static char b[2048]; if(!b[0]) snprintf(b,sizeof b,"\"C:\\BSI\\Binaries\\Win32\\BioShockInfinite.exe\"%s%s",getenv("BSI_ARGS")?" ":"",getenv("BSI_ARGS")?getenv("BSI_ARGS"):""); return b; }
HLE(kernel32,GetCommandLineW){ if(!g_cmdline_w) g_cmdline_w=galloc_wstr(cmdline()); RET_STD(0,g_cmdline_w); }
HLE(kernel32,GetCommandLineA){ if(!g_cmdline_a) g_cmdline_a=galloc_str(cmdline()); RET_STD(0,g_cmdline_a); }
HLE(kernel32,GetModuleFileNameW){ uint32_t d=ARG(1), n=ARG(2); const char*s=g_guest_exe; uint32_t k=0; for(;s[k]&&k+1<n;k++) W16(d+2*k,s[k]); W16(d+2*k,0); RET_STD(3,k); }
HLE(kernel32,GetModuleFileNameA){ uint32_t d=ARG(1), n=ARG(2); const char*s=g_guest_exe; uint32_t k=0; for(;s[k]&&k+1<n;k++) W8(d+k,s[k]); W8(d+k,0); RET_STD(3,k); }
uint32_t dll_handle(const char*n);
HLE(kernel32,GetModuleHandleW){ if(!ARG(0)) RET_STD(1,0x400000); char t[512]; g2host_w(ARG(0),t,512); RET_STD(1, dll_handle(t)); }
HLE(kernel32,GetModuleHandleA){ if(!ARG(0)) RET_STD(1,0x400000); char t[512]; g2host_a(ARG(0),t,512); RET_STD(1, dll_handle(t)); }
HLE(kernel32,GetVersionExW){ uint32_t p=ARG(0); W32(p+4,6); W32(p+8,1); W32(p+12,7601); W32(p+16,2); RET_STD(1,1); }
HLE(kernel32,GetVersionExA){ uint32_t p=ARG(0); W32(p+4,6); W32(p+8,1); W32(p+12,7601); W32(p+16,2); RET_STD(1,1); }

/* sync objects (minimal: handles are small ints) */
static uint32_t next_handle=0x100;
int hle_file_close(uint32_t h);
int ev_close(uint32_t h);
HLE(kernel32,CloseHandle){ if(!ev_close(ARG(0))) hle_file_close(ARG(0)); RET_STD(1,1); }
HLE(kernel32,SetStdHandle){ RET_STD(2,1); }
HLE(kernel32,SetThreadPriority){ if(sched_policy_win_request) sched_policy_win_request(ARG(0),0,(int32_t)ARG(1)); RET_STD(2,1); }
HLE(kernel32,SetThreadIdealProcessor){ if(sched_policy_win_request) sched_policy_win_request(ARG(0),2,(int32_t)ARG(1)); RET_STD(2,0); }
HLE(kernel32,RaiseException){
  if(ARG(0)==0x406D1388u){ /* MSVC SetThreadName: THREADNAME_INFO {type 0x1000, name, thread id (-1 = caller), flags} -> host thread name */
    uint32_t a=ARG(3); if(a && ARG(2)>=3 && R32(a)==0x1000 && R32(a+4)){ char nm[16]; uint32_t s=R32(a+4); int i=0; for(;i<15;i++){ nm[i]=(char)R8(s+i); if(!nm[i]) break; } nm[i]=0;
      void hle_thread_setname(uint32_t wintid, const char *nm); hle_thread_setname(R32(a+8), nm); }
    RET_STD(4,0); }
  fprintf(stderr,"guest RaiseException %08x\n",ARG(0)); { void guest_stackscan(CPU*c, const char*why); guest_stackscan(c,"RaiseException"); if(getenv("BSI_MEMDUMP_ON_EXC")){ void memdump_snap(int); memdump_snap(99); } } fflush(0); exit(8);
}
