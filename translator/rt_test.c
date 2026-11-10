// test runtime: guest memory reservation + fault trapping
#define _GNU_SOURCE
#include "cpu.h"
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/time.h>
#ifdef BSI_FIXED_M
const uint8_t bsi_rt_fixed_m_ok = 1;   /* fixed guest base (difftest with BSI_DIFF_FIXED_M) */
#else
uint8_t *M;
#endif
static sigjmp_buf jb; static volatile int fault; static char msg[256];
static void alrm(int s){ fault=5; snprintf(msg,sizeof msg,"timeout"); siglongjmp(jb,1); }
static void segv(int s, siginfo_t*si, void*u){ fault=1; snprintf(msg,sizeof msg,"segv at host %p (guest %lx)",si->si_addr,(long)((uint8_t*)si->si_addr-M)); siglongjmp(jb,1); }
uint8_t* rt_init(void){
#ifdef BSI_FIXED_M
  if(mmap(M,0x100000000ull,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|0x100000/*MAP_FIXED_NOREPLACE*/,-1,0)!=(void*)M) return 0;
#else
  M=mmap(0,0x100000000ull,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
#endif
  struct sigaction sa={0}; sa.sa_sigaction=segv; sa.sa_flags=SA_SIGINFO|SA_NODEFER; sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); signal(SIGALRM,alrm);
  return M;
}
int rt_map(uint32_t a, uint32_t n){ return mprotect(M+a,n,PROT_READ|PROT_WRITE); }
void unimpl(CPU*c, uint32_t addr, const char* what){ fault=2; snprintf(msg,sizeof msg,"unimpl %x %s",addr,what); siglongjmp(jb,1); }
void call_indirect(CPU*c, uint32_t t){ fault=3; snprintf(msg,sizeof msg,"call_indirect %x",t); siglongjmp(jb,1); }
void call_import(CPU*c, uint32_t t){ fault=4; snprintf(msg,sizeof msg,"call_import %x",t); siglongjmp(jb,1); }
void host_cpuid(CPU*c){ c->eax=0; c->ebx=c->ecx=c->edx=0; }
void host_ldmxcsr(CPU*c,uint32_t v){}
uint32_t host_stmxcsr(CPU*c){ return 0x1f80u; }
void host_rdtsc(CPU*c){ c->eax=0; c->edx=0; }
double fpu_round(CPU*c,double v){ switch((c->fcw>>10)&3){case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v);} }
typedef void(*gf)(CPU*);
int rt_run(gf f, CPU*c){ fault=0; msg[0]=0; struct itimerval t={{0,0},{2,0}}; if(sigsetjmp(jb,1)==0){ setitimer(ITIMER_REAL,&t,0); f(c); } struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0); return fault; }
const char* rt_msg(void){ return msg; }

void TRACE(CPU*c, uint32_t a){ fprintf(stderr,"T %x eax=%x ecx=%x edx=%x ebx=%x esp=%x ebp=%x esi=%x edi=%x top=%u st0=%.9g st1=%.9g\n",a,c->eax,c->ecx,c->edx,c->ebx,c->esp,c->ebp,c->esi,c->edi,c->top,c->st[c->top&7],c->st[(c->top+1)&7]); }
