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

/* shared with gate.S (main thread only for now) */
uint32_t g_host_esp, g_gate_target, g_gate_ret, g_gate_eflags, g_fpu_n;
CPU *g_gate_cpu;
uint16_t g_gate_fcw = 0x27f, g_fsw_before, g_fsw_after;
double g_fpu_in[8], g_fpu_out[8];
void gate_call(CPU *c, uint32_t target);
void bail_native(CPU *c, uint32_t addr) __attribute__((noreturn));
void fpu_store_n(int n);

static CPU g_cpu;
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
static void fpu_out_prep(CPU*c){ /* emulated x87 -> g_fpu_in */
  uint32_t n=(8-c->top)&7; g_fpu_n=n;
  for(uint32_t k=0;k<n;k++) g_fpu_in[k]=c->st[(c->top+k)&7];
  c->top=0; g_gate_fcw=c->fcw;
}
static void fpu_back(CPU*c){ /* real FPU (after native code) -> emulated x87 */
  int before=(g_fsw_before>>11)&7, after=(g_fsw_after>>11)&7;
  int loaded=g_fpu_n, now=(loaded + ((before-after)&7))&7;   /* entries on the real stack now */
  fpu_store_n(now);
  c->top=(8-now)&7;
  for(int k=0;k<now;k++) c->st[(c->top+k)&7]=g_fpu_out[k];
  c->fcw=g_gate_fcw;
}

void call_native(CPU*c, uint32_t target){
  n_gate++;
  g_gate_eflags=pack_flags(c); fpu_out_prep(c);
  gate_call(c,target);
  unpack_flags(c,g_gate_eflags); fpu_back(c);
}
static int g_trace_imports;
static uint32_t g_iat_RaiseException;
uint32_t import_iat(const char* name);
const char* import_name(uint32_t iat);
void call_import(CPU*c, uint32_t iat){
  if(g_trace_imports) LOG("import %s esp=%08x\n", import_name(iat), c->esp);
  if(iat==g_iat_RaiseException && R32(c->esp+4)==0x406D1388u){   /* MSVC SetThreadName: debugger-only, skip */
    c->esp+=4+16; c->eax=0; return;
  }
  call_native(c, R32(iat));
  if(g_trace_imports>1) LOG("  -> eax=%08x\n", c->eax);
}
void call_indirect(CPU*c, uint32_t t){
  gfn f=lookup(t);
  if(f){ n_indirect_recomp++; f(c); return; }
  n_indirect_native++;
  { static uint32_t seen[4096]; static int ns; int k; for(k=0;k<ns;k++) if(seen[k]==t) break;
    if(k==ns && ns<4096){ seen[ns++]=t; if(t>=0x401000u && t<0x10d4000u) LOG("native indirect target in exe: %08x (from ret %08x)\n", t, R32(c->esp)); } }
  call_native(c,t);
}
void unimpl(CPU*c, uint32_t addr, const char* what){
  LOG("BAIL to native at %08x (%s)  gates=%ld ind_recomp=%ld ind_native=%ld\n",addr,what,n_gate,n_indirect_recomp,n_indirect_native);
  g_gate_eflags=pack_flags(c); fpu_out_prep(c);
  bail_native(c,addr);
}
void host_cpuid(CPU*c){ uint32_t a=c->eax,b,cc=c->ecx,d; __asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cc),"=d"(d)); c->eax=a;c->ebx=b;c->ecx=cc;c->edx=d; }
void host_rdtsc(CPU*c){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); c->eax=lo; c->edx=hi; }
void host_ldmxcsr(CPU*c,uint32_t v){ __asm__ volatile("ldmxcsr %0"::"m"(v)); }
uint32_t host_stmxcsr(CPU*c){ uint32_t v; __asm__ volatile("stmxcsr %0":"=m"(v)); return v; }
double fpu_round(CPU*c,double v){ switch((c->fcw>>10)&3){case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v);} }
void TRACE(CPU*c,uint32_t a){}

extern void f_00fc789d(CPU*c);   /* mainCRTStartup (behind the SteamStub DRM entry) */

__declspec(dllexport) void bsi_start(void){
  char path[MAX_PATH]; GetModuleFileNameA(NULL,path,MAX_PATH);
  char *s=strrchr(path,'\\'); if(s) strcpy(s+1,"bsirecomp.log");
  g_log=fopen(path,"w");
  LOG("bsirecomp: %u recompiled functions\n", g_functab_n);
  CPU *c=&g_cpu; memset(c,0,sizeof *c);
  c->fcw=0x27f; c->top=0;
  uint32_t teb; __asm__("movl %%fs:0x18,%0":"=r"(teb)); c->fs_base=teb;
  /* guest stack: lower part of this thread's (fully committed 5MB) stack, 1MB left for host C frames */
  uint32_t here=(uint32_t)(uintptr_t)&path;
  c->esp=(here-0x100000u)&~0xfu;
  c->ebp=0;
  PUSH32(c,0);                     /* fake return address: mainCRTStartup never returns */
  LOG("guest esp=%08x teb=%08x\n",c->esp,teb);
  g_iat_RaiseException=import_iat("KERNEL32.dll!RaiseException");
  if(getenv("BSI_TRACE_IMPORTS")) g_trace_imports=atoi(getenv("BSI_TRACE_IMPORTS"));
  if(getenv("BSI_NATIVE")){ LOG("BSI_NATIVE set: running original code\n"); unimpl(c,0xfc789d,"native mode"); }
  f_00fc789d(c);
  LOG("mainCRTStartup returned?!\n");
  ExitProcess(0);
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p){ return TRUE; }
