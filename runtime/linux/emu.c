#include <pthread.h>
/* Fallback: run not-yet-translated guest code in Unicorn on the same guest memory (M).
   Import calls from emulated code (fetch at IMP_BASE magic addresses) are dispatched to HLE handlers. */
#include "cpu.h"
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define IMP_BASE 0xE0000000u
extern FILE *LOGF_RT;
void dispatch_import(CPU*c, int idx); int imports_count(void);
static __thread uc_engine *uc; static FILE *tf;
static uint32_t e_img,e_imgsz,e_hlo,e_hhi,e_slo,e_ssz,e_teb;
static uint32_t x_lo[32], x_n[32]; static volatile int x_cnt; static __thread int x_mapped;
void emu_add_region(uint32_t a, uint32_t n){ if(x_cnt<32){ x_lo[x_cnt]=a; x_n[x_cnt]=n; __sync_synchronize(); x_cnt++; } }
static const uint32_t REGIDS[8]={UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI};
static void map(uint32_t a, uint32_t n){ uc_err e=uc_mem_map_ptr(uc,a,n,UC_PROT_ALL,M+a); if(e) fprintf(stderr,"uc map %x %x: %s\n",a,n,uc_strerror(e)); }
static void gdt(uint32_t fs_base){
  uint64_t g[4]={0}; uint32_t GDT=0x7ff00000u;
  #define DESC(base,limit,acc,fl) ((uint64_t)((limit)&0xffff)|((uint64_t)((base)&0xffffff)<<16)|((uint64_t)(acc)<<40)|((uint64_t)(((limit)>>16)&0xf)<<48)|((uint64_t)(fl)<<52)|((uint64_t)(((base)>>24)&0xff)<<56))
  g[1]=DESC(0,0xfffff,0x9a,0xc); g[2]=DESC(0,0xfffff,0x92,0xc); g[3]=DESC(fs_base,0xfff,0x92,0x4);
  uc_mem_map(uc,GDT,0x1000,UC_PROT_ALL); uc_mem_write(uc,GDT,g,sizeof g);
  uc_x86_mmr gd={0,GDT,0xff,0}; uc_reg_write(uc,UC_X86_REG_GDTR,&gd);
  int fs=3<<3, ds=2<<3, cs=1<<3; uc_reg_write(uc,UC_X86_REG_FS,&fs);
  uc_reg_write(uc,UC_X86_REG_DS,&ds); uc_reg_write(uc,UC_X86_REG_ES,&ds); uc_reg_write(uc,UC_X86_REG_SS,&ds); uc_reg_write(uc,UC_X86_REG_GS,&ds); uc_reg_write(uc,UC_X86_REG_CS,&cs);
}
static void emu_open(uint32_t teb){
  if(uc_open(UC_ARCH_X86,UC_MODE_32,&uc)){ fprintf(stderr,"uc_open failed\n"); exit(9); }
  /* v0.8.7: cap the TCG code cache. Unicorn's default on 64-bit hosts is 1 GB per engine (one engine per thread), and on the Quest
     two of them grew to 160 + 44 MB resident in 14 min (the "unnamed rwxp 1023 MB" VMAs in leakwatch). With a cap, QEMU flushes and
     reuses the buffer when it is full. BSI_UC_TB_MB=<n> (0 = Unicorn default). Must be set before the engine initialises (first map). */
  { const char *e = getenv("BSI_UC_TB_MB"); uint32_t mb = e ? (uint32_t)atoi(e) : 48; if(mb){
#ifdef uc_ctl_set_tcg_buffer_size   /* Unicorn >= 2.0.2 headers; the cloud Linux build's system unicorn is older */
      uc_ctl_set_tcg_buffer_size(uc, mb << 20);
#else
      mb = 0;
#endif
    }
    static int said; if(!said){ said = 1; fprintf(stderr, "[emu] TCG code cache cap %u MB per engine\n", mb); } }
  map(e_img,(e_imgsz+0xfff)&~0xfffu); map(e_hlo,e_hhi-e_hlo); map(e_slo,e_ssz); map(e_teb,0x2000);
  if(teb!=e_teb) map(teb,0x2000);
  gdt(teb);
  uc_mem_map(uc,0xDEADB000u,0x1000,UC_PROT_ALL); { static uint8_t hlt[0x1000]; memset(hlt,0xf4,sizeof hlt); uc_mem_write(uc,0xDEADB000u,hlt,sizeof hlt); }
}
void emu_init(uint32_t img, uint32_t imgsz, uint32_t heap_lo, uint32_t heap_hi, uint32_t stk_lo, uint32_t stk_sz, uint32_t teb){
  e_img=img; e_imgsz=imgsz; e_hlo=heap_lo; e_hhi=heap_hi; e_slo=stk_lo; e_ssz=stk_sz; e_teb=teb;
  tf=fopen("emu_targets.txt","a");
}

static void d2f80(double v, uint8_t *o){ uint16_t se=0; uint64_t m=0; if(signbit(v)){ se=0x8000; v=-v; }
  if(v!=v){ se|=0x7fff; m=0xC000000000000000ull; } else if(isinf(v)){ se|=0x7fff; m=0x8000000000000000ull; }
  else if(v!=0){ int ex; double f=frexp(v,&ex); m=(uint64_t)ldexp(f,64); se|=(uint16_t)(ex-1+16383); }
  memcpy(o,&m,8); memcpy(o+8,&se,2); }
static double f802d(const uint8_t *p){ uint64_t m; uint16_t se; memcpy(&m,p,8); memcpy(&se,p+8,2); int ex=se&0x7fff; double s=(se&0x8000)?-1.0:1.0;
  if(ex==0 && m==0) return s*0.0; if(ex==0x7fff) return (m<<1)?NAN:s*INFINITY; return s*ldexp((double)m,ex-16383-63); }
static void put(CPU*c){ uint32_t *r=&c->eax; for(int k=0;k<8;k++) uc_reg_write(uc,REGIDS[k],&r[k]);
  uint32_t fl=0x202|c->cf|(c->pf<<2)|(c->zf<<6)|(c->sf<<7)|(c->df<<10)|(c->of<<11); uc_reg_write(uc,UC_X86_REG_EFLAGS,&fl);
  for(int k=0;k<8;k++) uc_reg_write(uc,UC_X86_REG_XMM0+k,&c->xmm[k]);
  uint16_t cw=c->fcw; uc_reg_write(uc,UC_X86_REG_FPCW,&cw);
  /* x87 stack: physical registers + TOP + tag word (float return values live in st0) */
  uint16_t sw=(c->fsw&~0x3800u)|((c->top&7)<<11); uc_reg_write(uc,UC_X86_REG_FPSW,&sw);
  int used=(8-c->top)&7; uint16_t tag=0xffff;
  for(int k=0;k<used;k++){ int ph=(c->top+k)&7; tag&=~(3u<<(2*ph)); }
  uc_reg_write(uc,UC_X86_REG_FPTAG,&tag);
  for(int k=0;k<8;k++){ uint8_t b[16]={0}; d2f80(c->st[k],b); uc_reg_write(uc,UC_X86_REG_FP0+k,b); } }
static void get(CPU*c){ uint32_t *r=&c->eax; for(int k=0;k<8;k++) uc_reg_read(uc,REGIDS[k],&r[k]);
  uint32_t fl; uc_reg_read(uc,UC_X86_REG_EFLAGS,&fl); c->cf=fl&1; c->pf=(fl>>2)&1; c->zf=(fl>>6)&1; c->sf=(fl>>7)&1; c->df=(fl>>10)&1; c->of=(fl>>11)&1;
  for(int k=0;k<8;k++) uc_reg_read(uc,UC_X86_REG_XMM0+k,&c->xmm[k]);
  uint16_t sw; uc_reg_read(uc,UC_X86_REG_FPSW,&sw); c->top=(sw>>11)&7; c->fsw=sw;
  uint16_t cw; uc_reg_read(uc,UC_X86_REG_FPCW,&cw); c->fcw=cw;
  for(int k=0;k<8;k++){ uint8_t b[16]={0}; uc_reg_read(uc,UC_X86_REG_FP0+k,b); c->st[k]=f802d(b); } }
static __thread uint32_t g_entry_esp, g_ret; static __thread int g_done, g_mid;
/* BSI_EMU_NATIVE=lo:hi[,lo:hi...] (hex, default off): inside emulated code, guest functions whose entry lies in one
   of these ranges are executed by the recompiled C instead (bisecting with BSI_EMU_RANGE). BSI_EMU_FNLOG=1 logs each
   distinct function entry seen by the emulator to emu_fn.txt. */
typedef void (*gfn_t)(CPU*); gfn_t rt_lookup(uint32_t a);
static uint32_t nat_lo[64], nat_hi[64]; static int nat_n=-1, fnlog; static __thread uint32_t g_native; static FILE *fnf;
static void nat_file(void);
static void nat_init(void){ if(nat_n>=0) return; nat_n=0; const char*e=getenv("BSI_EMU_NATIVE");
  while(e&&*e&&nat_n<64){ uint32_t a,b; int n; if(sscanf(e,"%x:%x%n",&a,&b,&n)<2) break; nat_lo[nat_n]=a; nat_hi[nat_n]=b; nat_n++; e+=n; if(*e==',') e++; }
  if(nat_n) fprintf(stderr,"[emu] %d native ranges\n",nat_n);
  nat_file();
  fnlog=getenv("BSI_EMU_FNLOG")!=0; if(fnlog) fnf=fopen("/tmp/emu_fn.txt","w"); }
static uint32_t *nat_set; static int nat_setn;   /* BSI_EMU_NATIVE_FILE=<file>: exact function entries (hex, one per line) */
static int u32cmp(const void*a,const void*b){ uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b; return x<y?-1:x>y; }
static void nat_file(void){ const char*e=getenv("BSI_EMU_NATIVE_FILE"); if(!e) return; FILE*f=fopen(e,"r"); if(!f) return; unsigned a; int cap=0;
  while(fscanf(f,"%x",&a)==1){ if(nat_setn==cap){ cap=cap?cap*2:1024; nat_set=realloc(nat_set,cap*4); } nat_set[nat_setn++]=a; } fclose(f);
  qsort(nat_set,nat_setn,4,u32cmp); fprintf(stderr,"[emu] %d native functions from %s\n",nat_setn,e); if(nat_setn && !nat_n){ nat_lo[0]=nat_hi[0]=0; nat_n=1; } }
static int is_native(uint32_t a){ for(int k=0;k<nat_n;k++) if(a>=nat_lo[k]&&a<nat_hi[k]) return 1;
  return nat_setn && bsearch(&a,nat_set,nat_setn,4,u32cmp)!=0; }
static void blk(uc_engine *u, uint64_t addr, uint32_t size, void *ud){
  if(nat_n>0 || fnlog){ uint32_t a=(uint32_t)addr;
    if(rt_lookup(a)){
      if(fnlog){ static uint32_t seen[8192]; static pthread_mutex_t fmu=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&fmu);
        for(int i=(a*2654435761u>>19)&8191,j=0;j<8192;j++,i=(i+1)&8191){ if(seen[i]==a) break; if(!seen[i]){ seen[i]=a; fprintf(fnf,"%08x\n",a); fflush(fnf); break; } }
        pthread_mutex_unlock(&fmu); }
      if(nat_n>0 && is_native(a)){ g_native=a; uc_emu_stop(u); return; } } }
  uint32_t esp; uc_reg_read(u,UC_X86_REG_ESP,&esp);
  if(esp<=g_entry_esp) return;
  if(!g_mid){ if((uint32_t)addr==g_ret){ g_done=1; uc_emu_stop(u); } return; }
  /* entered mid-function: stop at the first block whose address sits just below esp (a return, incl. ret N) */
  for(uint32_t k=4;k<=0x44;k+=4) if(R32(esp-k)==(uint32_t)addr){ g_done=1; uc_emu_stop(u); return; }
}
static __thread uc_hook hb, hm; static __thread int hooked; static __thread uint64_t bad_addr;
static bool on_unmapped(uc_engine *u, uc_mem_type t, uint64_t a, int size, int64_t v, void *ud){ bad_addr=a; return false; }
static void emu_run(CPU*c, uint32_t target);
#include <time.h>
static struct { uint32_t t; unsigned long n; double sec; } est[512]; static pthread_mutex_t est_mu=PTHREAD_MUTEX_INITIALIZER;
static double est_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static void est_add(uint32_t t, double dt){ static int on=-1; if(on<0) on=getenv("BSI_EMUSTAT")!=0; if(!on) return; static double last;
  pthread_mutex_lock(&est_mu); int k=(t*2654435761u>>23)&511; for(int j=0;j<512;j++){ int i=(k+j)&511; if(est[i].t==t||!est[i].t){ est[i].t=t; est[i].n++; est[i].sec+=dt; break; } }
  double now=est_now(); if(now-last>30){ last=now; double tot=0; for(int i=0;i<512;i++) tot+=est[i].sec; fprintf(stderr,"[emustat] total %.1f s in emulator; top:",tot);
    for(int r=0;r<12;r++){ int b=-1; for(int i=0;i<512;i++) if(est[i].t && est[i].sec>=0 && (b<0||est[i].sec>est[b].sec)) b=i; if(b<0) break; fprintf(stderr," %08x:%lu/%.2fs",est[b].t,est[b].n,est[b].sec); est[b].sec=-est[b].sec-1; }
    for(int i=0;i<512;i++) if(est[i].sec<0) est[i].sec=-est[i].sec-1; fprintf(stderr,"\n"); }
  pthread_mutex_unlock(&est_mu); }
/* v0.8.8: every distinct guest address that enters the emulator is logged ONCE to bsi.log with its caller and thread:
   "[emu] enter <target> from <return address> tid <tid> <thread name> [mid]". These are the untranslated leftovers to add
   to the translator (HANDOFF priority 4). Max 4000 distinct entries. BSI_EMU_ENTRYLOG=0 turns it off. */
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
static void entry_log(uint32_t t, uint32_t from, int mid){
  static int on=-1; if(on<0){ const char*e=getenv("BSI_EMU_ENTRYLOG"); on=!(e&&!strcmp(e,"0")); }
  if(!on) return;
  static uint32_t seen[4096]; static int n; static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mu);
  for(int i=(int)((t*2654435761u)>>20)&4095,j=0;j<4096;j++,i=(i+1)&4095){
    if(seen[i]==t) break;
    if(!seen[i]){ if(n<4000){ char nm[17]={0}; prctl(PR_GET_NAME,nm,0,0,0); seen[i]=t; n++;
        fprintf(stderr,"[emu] enter %08x from %08x tid %d %s%s (#%d)\n",t,from,(int)syscall(SYS_gettid),nm,mid?" mid":"",n); }
      break; } }
  pthread_mutex_unlock(&mu); }
void emu_call(CPU*c, uint32_t target){ uint32_t sr=g_ret; int sm=g_mid; g_ret=R32(c->esp); g_mid=0; entry_log(target,g_ret,0); double t0=est_now(); emu_run(c,target); est_add(target,est_now()-t0); g_ret=sr; g_mid=sm; }
void emu_call_mid(CPU*c, uint32_t target){ uint32_t sr=g_ret; int sm=g_mid; entry_log(target,R32(c->esp),1); g_ret=0; g_mid=1; emu_run(c,target); g_ret=sr; g_mid=sm; }
static void emu_run(CPU*c, uint32_t target){
  if(!uc) emu_open(c->fs_base);
  nat_init();
  while(x_mapped<x_cnt){ map(x_lo[x_mapped],x_n[x_mapped]); x_mapped++; }
  if(!hooked){ uc_hook_add(uc,&hb,UC_HOOK_BLOCK,blk,0,1,0); uc_hook_add(uc,&hm,UC_HOOK_MEM_UNMAPPED,on_unmapped,0,1,0); hooked=1; }
  uint32_t saved_entry=g_entry_esp; g_entry_esp=c->esp;
  if(tf){ static uint32_t seen[4096]; static pthread_mutex_t smu=PTHREAD_MUTEX_INITIALIZER; int isnew=1; pthread_mutex_lock(&smu);
    for(int i=(target*2654435761u>>20)&4095,j=0;j<4096;j++,i=(i+1)&4095){ if(seen[i]==target){ isnew=0; break; } if(!seen[i]){ seen[i]=target; break; } }
    pthread_mutex_unlock(&smu); if(isnew){ fprintf(tf,"%08x\n",target); fflush(tf); } }
  static int dbg=-1; if(dbg<0) dbg=getenv("BSI_EMUDBG")!=0;
  if(dbg) fprintf(stderr,"[emu] enter %08x esp=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x\n",target,c->esp,c->ebx,c->esi,c->edi,c->ebp);
  uint32_t pc=target;
  for(;;){
    put(c); g_done=0;
    uc_err e=uc_emu_start(uc,pc,0,0,0);
    get(c); uc_reg_read(uc,UC_X86_REG_EIP,&pc);
    if(g_native){ uint32_t a=g_native; g_native=0;
      if(pc!=a){ fprintf(stderr,"[emu] native switch: stopped at %08x, expected %08x\n",pc,a); exit(11); }
      uint32_t back=R32(c->esp), ee=g_entry_esp, er=g_ret; int em=g_mid;
      rt_lookup(a)(c);   /* recompiled function body, including its ret */
      g_entry_esp=ee; g_ret=er; g_mid=em; pc=back; continue; }
    if(g_done){ g_entry_esp=saved_entry; if(dbg) fprintf(stderr,"[emu] leave %08x at %08x esp=%08x eax=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x\n",target,pc,c->esp,c->eax,c->ebx,c->esi,c->edi,c->ebp); return; }
    if(pc>=IMP_BASE && pc<IMP_BASE+4*imports_count()){
      uint32_t back=R32(c->esp);
      uint32_t ee=g_entry_esp, er=g_ret; int em=g_mid;
      if(dbg) fprintf(stderr,"[emu]   import #%u esp=%08x edi=%08x\n",(pc-IMP_BASE)/4,c->esp,c->edi);
      dispatch_import(c,(pc-IMP_BASE)/4);   /* may re-enter emu_call (callbacks) */
      g_entry_esp=ee; g_ret=er; g_mid=em;
      pc=back; continue;
    }
    { extern int magic_dispatch(CPU*c, uint32_t pc); uint32_t back=R32(c->esp); uint32_t ee=g_entry_esp;
      if(pc>=0xE8000000u && magic_dispatch(c,pc)){ g_entry_esp=ee; pc=back; continue; } }
    if(pc>=0xDEADB000u && pc<0xDEADC000u){ g_entry_esp=saved_entry; return; }   /* returned to a host sentinel */
    if(e==UC_ERR_FETCH_UNMAPPED && nat_n>=0 && getenv("BSI_EMU_RANGE") && rt_lookup(pc)){   /* call into recompiled code outside the mapped image (DLLs) */
      uint32_t back=R32(c->esp), ee=g_entry_esp, er=g_ret; int em=g_mid; rt_lookup(pc)(c); g_entry_esp=ee; g_ret=er; g_mid=em; pc=back; continue; }
    fprintf(stderr,"emu stopped at %08x (%s) entered at %08x esp %08x, bad address %08llx, eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x\n",pc,uc_strerror(e),target,c->esp,(unsigned long long)bad_addr,c->eax,c->ecx,c->edx,c->ebx,c->esi,c->edi,c->ebp); exit(10);
  }
}
