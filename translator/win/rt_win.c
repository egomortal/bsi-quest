/* BSI-Quest Windows runtime for the recompiled BioShockInfinite.exe (hybrid: recompiled C + original native code). */
#define IDENTITY 1
#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "cpu.h"

_Static_assert(offsetof(CPU,eax)==0 && offsetof(CPU,esp)==16 && offsetof(CPU,edi)==28, "regs");
_Static_assert(offsetof(CPU,cf)==32 && offsetof(CPU,df)==37, "flags");
_Static_assert(offsetof(CPU,xmm)==40, "xmm");

/* per-thread context (layout shared with gate.S) */
typedef struct TCtx {
  CPU cpu;                 /* 0 .. 243 */
  uint32_t host_esp;       /* 244 */
  uint32_t gate_target;    /* 248 */
  uint32_t gate_ret;       /* 252 */
  uint32_t gate_eflags;    /* 256 */
  uint32_t fpu_n;          /* 260 */
  uint16_t fcw;            /* 264 */
  uint16_t fsw_before;     /* 266 */
  uint16_t fsw_after;      /* 268 */
  uint16_t _pad;
  double fpu_in[8];        /* 272 */
  double fpu_out[8];       /* 336 */
} TCtx;
_Static_assert(sizeof(CPU)==248, "cpu size");
_Static_assert(offsetof(TCtx,host_esp)==248 && offsetof(TCtx,gate_eflags)==260 && offsetof(TCtx,fpu_n)==264, "tctx a");
_Static_assert(offsetof(TCtx,fcw)==268 && offsetof(TCtx,fsw_after)==272 && offsetof(TCtx,fpu_in)==280 && offsetof(TCtx,fpu_out)==344, "tctx b");
DWORD g_tls_index;
static inline TCtx* T(CPU*c){ return (TCtx*)c; }
void gate_call(TCtx *t, uint32_t target);
void bail_native(TCtx *t, uint32_t addr) __attribute__((noreturn));
void fpu_store_n(TCtx *t, int n);
static TCtx g_main;
static FILE *g_log;
static long n_gate, n_indirect_native, n_indirect_recomp;
#define LOG(...) do{ if(g_log){ fprintf(g_log,__VA_ARGS__); fflush(g_log);} }while(0)

/* table of recompiled functions (generated: functab.c) */
typedef struct { uint32_t addr; gfn fn; } FEnt;
extern const FEnt g_functab[]; extern const uint32_t g_functab_n;
static gfn lookup(uint32_t a){
  uint32_t lo=0, hi=g_functab_n;
  while(lo<hi){ uint32_t mid=(lo+hi)/2; if(g_functab[mid].addr<a) lo=mid+1; else hi=mid; }
  return (lo<g_functab_n && g_functab[lo].addr==a) ? g_functab[lo].fn : 0;
}

static uint32_t pack_flags(CPU*c){ return 0x202u|c->cf|(c->pf<<2)|(c->zf<<6)|(c->sf<<7)|(c->df<<10)|(c->of<<11); }
static void unpack_flags(CPU*c,uint32_t f){ c->cf=f&1; c->pf=(f>>2)&1; c->zf=(f>>6)&1; c->sf=(f>>7)&1; c->df=(f>>10)&1; c->of=(f>>11)&1; }
static void fpu_out_prep(CPU*c){ /* emulated x87 -> fpu_in */
  TCtx*t=T(c); uint32_t n=(8-c->top)&7; t->fpu_n=n;
  for(uint32_t k=0;k<n;k++) t->fpu_in[k]=c->st[(c->top+k)&7];
  c->top=0; t->fcw=c->fcw;
}
static void fpu_back(CPU*c){ /* real FPU (after native code) -> emulated x87 */
  TCtx*t=T(c); int before=(t->fsw_before>>11)&7, after=(t->fsw_after>>11)&7;
  int loaded=t->fpu_n, now=(loaded + ((before-after)&7))&7;
  fpu_store_n(t,now);
  c->top=(8-now)&7;
  for(int k=0;k<now;k++) c->st[(c->top+k)&7]=t->fpu_out[k];
  c->fcw=t->fcw;
}

void call_native(CPU*c, uint32_t target){
  n_gate++;
  T(c)->gate_eflags=pack_flags(c); fpu_out_prep(c);
  gate_call(T(c),target);
  unpack_flags(c,T(c)->gate_eflags); fpu_back(c);
}
static int g_trace_imports;
static uint32_t g_iat_RaiseException, g_iat_CreateThread, g_iat_beginthreadex, g_iat_CxxThrow;
static int g_no_threads; static long n_threads;
#define THREAD_STACK 0x400000u
typedef struct { uint32_t start, param; } ThreadStart;
static DWORD WINAPI thread_tramp(void *p);
static gfn lookup(uint32_t a);
uint32_t import_iat(const char* name);
const char* import_name(uint32_t iat);
void call_import(CPU*c, uint32_t iat){
  if(g_trace_imports) LOG("import %s esp=%08x\n", import_name(iat), c->esp);
  if((iat==g_iat_CreateThread || iat==g_iat_beginthreadex) && !g_no_threads){
    uint32_t start=R32(c->esp+12), param=R32(c->esp+16);
    if(lookup(start)){
      ThreadStart *ts=(ThreadStart*)malloc(sizeof *ts); ts->start=start; ts->param=param;
      W32(c->esp+12,(uint32_t)(uintptr_t)(iat==g_iat_CreateThread?(void*)thread_tramp:(void*)thread_tramp));
      W32(c->esp+16,(uint32_t)(uintptr_t)ts);
      if(R32(c->esp+8) < THREAD_STACK) W32(c->esp+8, THREAD_STACK);   /* recompiled code needs a bigger stack */
      n_threads++;
      LOG("thread start %08x param %08x -> recompiled\n", start, param);
    }
  }
  if(iat==g_iat_CxxThrow){ eh_throw(c); return; }   /* C++ throw: dispatched in guest terms so catch blocks run recompiled */
  if(iat==g_iat_RaiseException && R32(c->esp+4)==0x406D1388u){   /* MSVC SetThreadName: debugger-only, skip */
    c->esp+=4+16; c->eax=0; return;
  }
  call_native(c, R32(iat));
  if(g_trace_imports>1) LOG("  -> eax=%08x\n", c->eax);
}
void call_indirect(CPU*c, uint32_t t){
  gfn f=lookup(t);
  if(f){ n_indirect_recomp++; f(c); return; }
  if(t>=0x401000u && t<0x10d4000u && *(uint16_t*)(uintptr_t)t==0x25FF){ call_import(c,*(uint32_t*)(uintptr_t)(t+2)); return; }
  n_indirect_native++;
  { static uint32_t seen[4096]; static int ns; int k; for(k=0;k<ns;k++) if(seen[k]==t) break;
    if(k==ns && ns<4096){ seen[ns++]=t; if(t>=0x401000u && t<0x10d4000u) LOG("native indirect target in exe: %08x (from ret %08x)\n", t, R32(c->esp)); } }
  call_native(c,t);
}
void unimpl(CPU*c, uint32_t addr, const char* what){
  LOG("BAIL to native at %08x (%s)  gates=%ld ind_recomp=%ld ind_native=%ld\n",addr,what,n_gate,n_indirect_recomp,n_indirect_native);
  T(c)->gate_eflags=pack_flags(c); fpu_out_prep(c);
  bail_native(T(c),addr);
}
void host_cpuid(CPU*c){ uint32_t a=c->eax,b,cc=c->ecx,d; __asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cc),"=d"(d)); c->eax=a;c->ebx=b;c->ecx=cc;c->edx=d; }
void host_rdtsc(CPU*c){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); c->eax=lo; c->edx=hi; }
void host_ldmxcsr(CPU*c,uint32_t v){ __asm__ volatile("ldmxcsr %0"::"m"(v)); }
uint32_t host_stmxcsr(CPU*c){ uint32_t v; __asm__ volatile("stmxcsr %0":"=m"(v)); return v; }
double fpu_round(CPU*c,double v){ switch((c->fcw>>10)&3){case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v);} }
void TRACE(CPU*c,uint32_t a){}


/* ---- sampling profiler: where does each thread spend time? (recompiled DLL / original exe code / system DLLs) */
#define MAXT 64
static HANDLE s_th[MAXT]; static DWORD s_tid[MAXT]; static uint32_t s_start[MAXT]; static volatile LONG s_nth;
static long s_cnt[MAXT][3];
#define NHIST 4096
static uint32_t h_eip[NHIST]; static long h_n[NHIST];
static uint32_t g_dll_lo, g_dll_hi;
static void reg_thread(uint32_t start){
  LONG k=InterlockedIncrement(&s_nth)-1; if(k>=MAXT) return;
  HANDLE h; DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&h,THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0);
  s_tid[k]=GetCurrentThreadId(); s_start[k]=start; s_th[k]=h;
}
static void hist_add(uint32_t e){
  uint32_t k=(e*2654435761u)>>20;
  for(int p=0;p<64;p++){ uint32_t j=(k+p)&(NHIST-1); if(h_eip[j]==e){ h_n[j]++; return; } if(!h_eip[j]){ h_eip[j]=e; h_n[j]=1; return; } }
}
static void dump_prof(int final){
  long tot[3]={0,0,0}; int n=s_nth<MAXT?s_nth:MAXT;
  LOG("---- profile t=%lums gates=%ld ind_recomp=%ld ind_native=%ld\n",GetTickCount(),n_gate,n_indirect_recomp,n_indirect_native);
  for(int k=0;k<n;k++){ long a=s_cnt[k][0],b=s_cnt[k][1],d=s_cnt[k][2]; tot[0]+=a;tot[1]+=b;tot[2]+=d;
    if(a+b+d) LOG("  thread %5lu start %08x: recomp %ld  exe-native %ld  system %ld\n",s_tid[k],s_start[k],a,b,d); }
  long all=tot[0]+tot[1]+tot[2]; if(!all) return;
  LOG("  TOTAL recomp %.1f%%  exe-native %.1f%%  system/driver %.1f%%\n",100.0*tot[0]/all,100.0*tot[1]/all,100.0*tot[2]/all);
  if(final||1){ /* top native exe addresses */
    for(int r=0;r<12;r++){ long best=0; int bj=-1; for(int j=0;j<NHIST;j++) if(h_n[j]>best){best=h_n[j];bj=j;}
      if(bj<0) break; LOG("    native hot %08x x%ld\n",h_eip[bj],h_n[bj]); h_n[bj]=-h_n[bj]; }
    for(int j=0;j<NHIST;j++) if(h_n[j]<0) h_n[j]=-h_n[j];
  }
}
static DWORD WINAPI sampler(void*p){
  int tick=0;
  for(;;){
    Sleep(5);
    int n=s_nth<MAXT?s_nth:MAXT;
    for(int k=0;k<n;k++){ HANDLE h=s_th[k]; if(!h) continue;
      if(SuspendThread(h)==(DWORD)-1){ s_th[k]=0; continue; }
      CONTEXT cx; cx.ContextFlags=CONTEXT_CONTROL; BOOL ok=GetThreadContext(h,&cx); ResumeThread(h);
      if(!ok) continue;
      uint32_t e=cx.Eip; int cls = (e>=g_dll_lo&&e<g_dll_hi)?0 : (e>=0x401000u&&e<0x10d4000u)?1 : 2;
      s_cnt[k][cls]++; if(cls==1) hist_add(e);
    }
    if(++tick%2000==0) dump_prof(0);   /* ~every 10 s */
  }
  return 0;
}
static void start_sampler(void){
  MEMORY_BASIC_INFORMATION mb; VirtualQuery((void*)&sampler,&mb,sizeof mb);
  HMODULE hm=(HMODULE)mb.AllocationBase; g_dll_lo=(uint32_t)(uintptr_t)hm;
  IMAGE_NT_HEADERS *nt=(IMAGE_NT_HEADERS*)((char*)hm+((IMAGE_DOS_HEADER*)hm)->e_lfanew);
  g_dll_hi=g_dll_lo+nt->OptionalHeader.SizeOfImage;
  LOG("bsirecomp.dll at %08x-%08x\n",g_dll_lo,g_dll_hi);
  if(!getenv("BSI_NO_PROFILE")) CreateThread(0,0,sampler,0,0,0);
}

extern void f_00fc789d(CPU*c);
static DWORD WINAPI thread_tramp(void *p){
  ThreadStart ts=*(ThreadStart*)p; free(p);
  TCtx *t=(TCtx*)calloc(1,sizeof(TCtx)); TlsSetValue(g_tls_index,t);
  CPU *c=&t->cpu; c->fcw=0x27f;
  uint32_t teb; __asm__("movl %%fs:0x18,%0":"=r"(teb)); c->fs_base=teb;
  uint32_t here=(uint32_t)(uintptr_t)&ts;
  c->esp=(here-0x100000u)&~0xfu;
  PUSH32(c,ts.param); PUSH32(c,0);   /* arg + fake return address */
  reg_thread(ts.start);
  gfn f=lookup(ts.start);
  f(c);
  DWORD rc=c->eax; free(t); return rc;
}   /* mainCRTStartup (behind the SteamStub DRM entry) */

__declspec(dllexport) void bsi_start(void){
  char path[MAX_PATH]; GetModuleFileNameA(NULL,path,MAX_PATH);
  char *s=strrchr(path,'\\'); if(s) strcpy(s+1,"bsirecomp.log");
  g_log=fopen(path,"w");
  LOG("bsirecomp: %u recompiled functions\n", g_functab_n);
  g_tls_index=TlsAlloc(); TlsSetValue(g_tls_index,&g_main);
  if(g_tls_index>=64){ LOG("TLS index %lu too high for fast path\n",g_tls_index); }
  CPU *c=&g_main.cpu; memset(&g_main,0,sizeof g_main);
  c->fcw=0x27f; c->top=0;
  uint32_t teb; __asm__("movl %%fs:0x18,%0":"=r"(teb)); c->fs_base=teb;
  /* guest stack: lower part of this thread's (fully committed 5MB) stack, 1MB left for host C frames */
  uint32_t here=(uint32_t)(uintptr_t)&path;
  c->esp=(here-0x100000u)&~0xfu;
  c->ebp=0;
  PUSH32(c,0);                     /* fake return address: mainCRTStartup never returns */
  LOG("guest esp=%08x teb=%08x\n",c->esp,teb);
  g_iat_RaiseException=import_iat("KERNEL32.dll!RaiseException");
  g_iat_CreateThread=import_iat("KERNEL32.dll!CreateThread");
  g_iat_beginthreadex=import_iat("MSVCR90.dll!_beginthreadex");
  g_iat_CxxThrow=getenv("BSI_NATIVE_EH")?0:import_iat("MSVCR90.dll!_CxxThrowException");
  g_no_threads = getenv("BSI_NATIVE_THREADS")!=0;
  if(getenv("BSI_TRACE_IMPORTS")) g_trace_imports=atoi(getenv("BSI_TRACE_IMPORTS"));
  if(getenv("BSI_NATIVE")){ LOG("BSI_NATIVE set: running original code\n"); unimpl(c,0xfc789d,"native mode"); }
  reg_thread(0xfc789d); start_sampler();
  f_00fc789d(c);
  LOG("mainCRTStartup returned?!\n");
  ExitProcess(0);
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p){ return TRUE; }
