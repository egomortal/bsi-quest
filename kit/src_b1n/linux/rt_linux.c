/* BSI-Quest host runtime (Linux/Android): runs the recompiled BioShockInfinite.exe with NO Windows.
   Guest memory: 4 GB reservation at M; PE image mapped at its original addresses.
   Imports: every IAT slot holds a magic address (IMP_BASE + 4*idx); calls to it dispatch to host (HLE) handlers.
   Handler convention: guest already pushed its return address; args at [esp+4]...; handler sets eax (and st0 for
   float returns) and pops the return address + (stdcall) its args. */
#define _GNU_SOURCE
#include "cpu.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t *M;
#define IMP_BASE 0xE0000000u
#define HEAP_LO  0x20000000u
#define HEAP_HI  0x79000000u
#define TEB_ADDR 0x7ffd0000u
#define STACK_TOP 0x1fff0000u
#define STACK_SIZE 0x01000000u

static FILE *LOGF; static int g_trace=1; static long n_calls;
void stub_note(const char*n){ static const char *seen[1024]; static int ns; for(int k=0;k<ns;k++) if(seen[k]==n) return; if(ns<1024) seen[ns++]=n; fprintf(stderr,"[stub] %s\n",n); fprintf(LOGF,"STUB %s\n",n); }
#define LOG(...) do{ fprintf(LOGF,__VA_ARGS__); fflush(LOGF);}while(0)

/* ---------------- guest memory allocator ----------------
   small objects (< 1 MB): power-of-two size classes bump-allocated from [HEAP_LO, SMALL_HI), free lists per class
   pages / large blocks: first-fit range allocator over two arenas, 64 KB granularity; freed ranges are madvise'd
   (so reuse returns zero pages) and merged with neighbours */
#define SMALL_HI 0x40000000u
#define ARENA2_LO 0x80000000u
#define ARENA2_HI 0xDE000000u
static pthread_mutex_t heap_mu=PTHREAD_MUTEX_INITIALIZER;
static uint32_t heap_brk=HEAP_LO;
static uint32_t *fl_v[32]; static uint32_t fl_n[32], fl_cap[32];   /* free lists kept on the host side (guest UAF writes cannot corrupt them) */
void guest_map(uint32_t a, uint32_t n);
static int szclass(uint32_t n){ int k=4; while((1u<<k)<n+8) k++; return k; }
void guest_map(uint32_t a, uint32_t n){ mprotect(M+(a&~0xfffu),((a&0xfff)+n+0xfff)&~0xfffu,PROT_READ|PROT_WRITE); }
typedef struct { uint32_t a, n; } Rng;
static Rng fr[65536]; static int nfr=-1;       /* free ranges, sorted by address */
static uint32_t alloc_a[1<<16], alloc_n[1<<16]; /* open-addressed map: base -> size of page allocations */
static void amap_put(uint32_t a, uint32_t n){ uint32_t h=(a>>16)*2654435761u>>16; for(;;h=(h+1)&0xffff){ if(!alloc_a[h]||alloc_a[h]==1){ alloc_a[h]=a; alloc_n[h]=n; return; } } }
static uint32_t amap_take(uint32_t a){ uint32_t h=(a>>16)*2654435761u>>16; for(int k=0;k<65536;k++,h=(h+1)&0xffff){ if(!alloc_a[h]) return 0; if(alloc_a[h]==a){ alloc_a[h]=1; return alloc_n[h]; } } return 0; }
static void fr_init(void){ nfr=0; fr[nfr++]=(Rng){SMALL_HI,HEAP_HI-SMALL_HI}; fr[nfr++]=(Rng){ARENA2_LO,ARENA2_HI-ARENA2_LO}; }
static uint32_t page_alloc(uint32_t n){   /* caller holds heap_mu */
  if(nfr<0) fr_init();
  n=(n+0xffff)&~0xffffu;
  for(int k=0;k<nfr;k++) if(fr[k].n>=n){ uint32_t a=fr[k].a; fr[k].a+=n; fr[k].n-=n; if(!fr[k].n){ memmove(&fr[k],&fr[k+1],(nfr-k-1)*sizeof(Rng)); nfr--; } amap_put(a,n); return a; }
  fprintf(stderr,"guest address space exhausted (request %u MB)\n",n>>20); fflush(0); abort();
}
static void page_free(uint32_t a){   /* caller holds heap_mu */
  uint32_t n=amap_take(a); if(!n) return;
  madvise(M+a,n,MADV_DONTNEED);
  int k=0; while(k<nfr && fr[k].a<a) k++;
  memmove(&fr[k+1],&fr[k],(nfr-k)*sizeof(Rng)); fr[k]=(Rng){a,n}; nfr++;
  if(k+1<nfr && fr[k].a+fr[k].n==fr[k+1].a){ fr[k].n+=fr[k+1].n; memmove(&fr[k+1],&fr[k+2],(nfr-k-2)*sizeof(Rng)); nfr--; }
  if(k>0 && fr[k-1].a+fr[k-1].n==fr[k].a){ fr[k-1].n+=fr[k].n; memmove(&fr[k],&fr[k+1],(nfr-k-1)*sizeof(Rng)); nfr--; }
}
/* BSI_HEAPSTAT=1: live small-heap bytes per guest call site, printed every 15 s */
static int hs_on=-1; static uint32_t hs_a[8192]; static int64_t hs_b[8192], hs_c[8192];
static void hs_add(uint32_t caller, int64_t b, int d){ uint32_t h=(caller*2654435761u)>>19; for(int i=0;i<8192;i++,h=(h+1)&8191){ if(hs_a[h]==caller||!hs_a[h]){ hs_a[h]=caller; hs_b[h]+=b; hs_c[h]+=d; return; } } }
static void hs_print(void){ static time_t last; time_t t=time(0); if(t-last<15) return; last=t;
  fprintf(stderr,"[heapstat] brk %u MB\n",(heap_brk-HEAP_LO)>>20);
  for(int r=0;r<12;r++){ int best=-1; for(int i=0;i<8192;i++) if(hs_a[i] && hs_b[i]>0 && (best<0||hs_b[i]>hs_b[best])) best=i; if(best<0) break;
    fprintf(stderr,"[heapstat]   %08x %6ld KB %ld blocks\n",hs_a[best],(long)(hs_b[best]>>10),(long)hs_c[best]); hs_b[best]=-hs_b[best]; }
  for(int i=0;i<8192;i++) if(hs_b[i]<0) hs_b[i]=-hs_b[i]; }
uint32_t gmalloc_c(uint32_t n, uint32_t caller);
uint32_t gmalloc(uint32_t n){ return gmalloc_c(n,1); }
uint32_t gmalloc_c(uint32_t n, uint32_t caller){
  pthread_mutex_lock(&heap_mu);
  uint32_t r;
  if(n>=(1u<<20)-8){ uint32_t sz=n+8; r=page_alloc(sz); W32(r,sz); W32(r+4,0xffffffffu); r+=8; }
  else { int k=szclass(n);
    if(fl_n[k]){ r=fl_v[k][--fl_n[k]]; }
    else { uint32_t sz=1u<<k; heap_brk=(heap_brk+15)&~15u; r=heap_brk; heap_brk+=sz;
      if(heap_brk>=SMALL_HI){ fprintf(stderr,"guest small-object heap exhausted\n"); exit(6); } }
    W32(r,(uint32_t)k); W32(r+4,caller); r+=8;
    if(hs_on<0) hs_on=getenv("BSI_HEAPSTAT")!=0; if(hs_on){ hs_add(caller,1<<k,1); hs_print(); } }
  pthread_mutex_unlock(&heap_mu);
  return r;
}
void gfree(uint32_t p){
  if(!p) return;
  pthread_mutex_lock(&heap_mu);
  uint32_t h=p-8, k=R32(h);
  if(R32(h+4)==0xffffffffu) page_free(h);
  else if(k<32){ if(hs_on>0) hs_add(R32(h+4),-(int64_t)(1u<<k),-1); if(fl_n[k]==fl_cap[k]){ fl_cap[k]=fl_cap[k]?fl_cap[k]*2:1024; fl_v[k]=realloc(fl_v[k],fl_cap[k]*4); } fl_v[k][fl_n[k]++]=h; }
  pthread_mutex_unlock(&heap_mu);
}
uint32_t gsize(uint32_t p){ uint32_t h=p-8,k=R32(h); return R32(h+4)==0xffffffffu? k-8 : (1u<<k)-8; }
uint32_t gpages(uint32_t n){ /* VirtualAlloc-like: 64 KB aligned, zeroed */
  pthread_mutex_lock(&heap_mu); uint32_t r=page_alloc(n); pthread_mutex_unlock(&heap_mu); return r; }
void gpages_free(uint32_t a){ pthread_mutex_lock(&heap_mu); page_free(a); pthread_mutex_unlock(&heap_mu); }

/* BSI_ZC (zero-copy Map): guest-window ranges for DXVK's persistently mapped (host-visible) Vulkan memory. DXVK's local
   patch (dxvk_memory.cpp, namespace bsi) gets these two functions through BSI_GVA_HOOK and places its mapped chunks here
   (VK_EXT_map_memory_placed, or host-pointer import on lavapipe), so a guest Map() returns (mapPtr - M) with no shadow.
   Ranges are taken top-down from the page arenas (the game allocates bottom-up), bounded by BSI_ZC_MAXMB (default 768)
   and never leave less than BSI_ZC_KEEPMB (default 384) free for the game; on failure DXVK maps normally (shadow path). */
static uint64_t zc_live, zc_peak, zc_nalloc, zc_fail;
static uint64_t arena_free_locked(void){ if(nfr<0) fr_init(); uint64_t f=0; for(int k=0;k<nfr;k++) f+=fr[k].n; return f; }
void *bsi_gva_alloc(size_t n, size_t align){
  if(!n || align>0x10000 || n>0x40000000u) return 0;
  static uint64_t maxb, keepb; if(!maxb){ const char *e=getenv("BSI_ZC_MAXMB"); maxb=(uint64_t)(e?atoi(e):768)<<20; e=getenv("BSI_ZC_KEEPMB"); keepb=(uint64_t)(e?atoi(e):384)<<20; }
  uint32_t sz=(uint32_t)((n+0xffff)&~(size_t)0xffff), a=0;
  pthread_mutex_lock(&heap_mu);
  uint64_t fr_tot=arena_free_locked();
  if(zc_live+sz<=maxb && fr_tot>=keepb+sz)
    for(int k=nfr-1;k>=0;k--) if(fr[k].n>=sz){ a=fr[k].a+fr[k].n-sz; fr[k].n-=sz; if(!fr[k].n){ memmove(&fr[k],&fr[k+1],(nfr-k-1)*sizeof(Rng)); nfr--; } amap_put(a,sz); break; }
  if(a){ zc_live+=sz; if(zc_live>zc_peak) zc_peak=zc_live; zc_nalloc++; } else zc_fail++;
  pthread_mutex_unlock(&heap_mu);
  fprintf(stderr,"[zc] guest window %s %u KB at %08x (live %llu MB, peak %llu MB, arena free %llu MB)\n",a?"alloc":"REFUSED",sz>>10,a,
    (unsigned long long)(zc_live>>20),(unsigned long long)(zc_peak>>20),(unsigned long long)(fr_tot>>20));
  return a?(void*)(M+a):0; }
void bsi_gva_free(void *p, size_t n){
  if(!p) return; uint32_t a=(uint32_t)((uint8_t*)p-M), sz=(uint32_t)((n+0xffff)&~(size_t)0xffff);
  /* the driver left a PROT_NONE reservation (placed unmap) or our imported pages: put plain guest memory back */
  if(mmap(p,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED) fprintf(stderr,"[zc] remap of %08x failed\n",a);
  pthread_mutex_lock(&heap_mu); page_free(a); zc_live-=sz; pthread_mutex_unlock(&heap_mu);
  fprintf(stderr,"[zc] guest window free %u KB at %08x (live %llu MB)\n",sz>>10,a,(unsigned long long)(zc_live>>20)); }

/* ---------------- imports ---------------- */
typedef void (*hle_fn)(CPU*c);
typedef struct { uint32_t iat; const char *name; hle_fn fn; } Imp;
extern Imp g_imps[]; extern int g_nimps;
#define ARG(k) R32(c->esp+4+4*(k))
#define RET_STD(nargs, v) do{ c->eax=(uint32_t)(v); c->esp+=4+4*(nargs); return; }while(0)
#define RET_C(v) do{ c->eax=(uint32_t)(v); c->esp+=4; return; }while(0)

void guest_call(CPU*c, uint32_t fn){ PUSH32(c,0xDEADBEEFu); call_indirect(c,fn); }

typedef struct { uint32_t addr; gfn fn; } FEnt;
extern const FEnt g_functab[]; extern const uint32_t g_functab_n;
static gfn lookup(uint32_t a){
  uint32_t lo=0, hi=g_functab_n;
  while(lo<hi){ uint32_t mid=(lo+hi)/2; if(g_functab[mid].addr<a) lo=mid+1; else hi=mid; }
  return (lo<g_functab_n && g_functab[lo].addr==a) ? g_functab[lo].fn : 0;
}
Imp *imp_at(int idx); int imps_total(void); int dll_contains(uint32_t a);
int dll_owns(const char*full); uint32_t dll_resolve(CPU*c,const char*full);
static char g_lazy[1024];
static void dispatch(CPU*c, int idx){
  Imp *im=imp_at(idx); n_calls++;
  if(g_trace){ LOG("[%ld] %s(%08x,%08x,%08x,%08x)", (long)syscall(SYS_gettid), im->name, ARG(0),ARG(1),ARG(2),ARG(3));
    const char *n=strchr(im->name,'!'); n=n?n+1:im->name; int wi=-1, ai=-1;
    if(!strcmp(n,"CreateFileW")||!strcmp(n,"GetFileAttributesW")||!strcmp(n,"FindFirstFileW")||!strcmp(n,"LoadLibraryW")||!strcmp(n,"GetFileAttributesExW")||!strcmp(n,"DeleteFileW")||!strcmp(n,"CreateDirectoryW")) wi=0;
    else if(!strcmp(n,"LoadLibraryA")||!strcmp(n,"CreateFileA")) ai=0; else if(!strcmp(n,"GetProcAddress")) ai=1;
    if(wi>=0 && ARG(wi)>0x10000){ char t[300]; int k=0; for(;k<299;k++){ uint16_t ch=R16(ARG(wi)+2*k); if(!ch) break; t[k]=ch<128?ch:'?'; } t[k]=0; LOG(" L\"%s\"",t); }
    if(ai>=0 && ARG(ai)>0x10000){ char t[300]; int k=0; for(;k<299;k++){ uint8_t ch=R8(ARG(ai)+k); if(!ch) break; t[k]=ch; } t[k]=0; LOG(" \"%s\"",t); }
    LOG("\n"); }
  if(idx<g_nimps && g_lazy[idx]){ uint32_t a=dll_resolve(c,im->name); if(a){ g_lazy[idx]=0; W32(im->iat,a); LOG("bound %s -> %08x\n",im->name,a); call_indirect(c,a); return; } }
  if(!im->fn){ LOG("UNIMPLEMENTED import %s after %ld import calls\n", im->name, n_calls); fprintf(stderr,"UNIMPLEMENTED %s (call #%ld)\n",im->name,n_calls); exit(3); }
  im->fn(c);
  if(g_trace) LOG("  = %08x\n", c->eax);
}
#include "hle_fast.c"   /* BSI_HLEFAST fast paths: notes/HLE_HOT.md */
void call_import(CPU*c, uint32_t iat){ uint32_t v=R32(iat); if(hf_imp_fast(c,v)) return; if(v>=IMP_BASE && v<IMP_BASE+4*imps_total()) dispatch(c,(v-IMP_BASE)/4); else call_indirect(c,v); }
int magic_dispatch(CPU*c, uint32_t pc);
void emu_call(CPU*c, uint32_t t);
/* BSI_EMU_RANGE=lo:hi (hex, default off): guest functions entered by indirect call/thread start inside [lo,hi)
   run in the Unicorn emulator instead of the recompiled C (bisecting translation bugs; see emu.c BSI_EMU_NATIVE) */
static uint32_t g_emu_lo, g_emu_hi; static int g_emu_rinit;
static void emu_range_init(void){ if(g_emu_rinit) return; g_emu_rinit=1; const char*e=getenv("BSI_EMU_RANGE");
  if(e && sscanf(e,"%x:%x",&g_emu_lo,&g_emu_hi)==2) fprintf(stderr,"[emu] BSI_EMU_RANGE %08x-%08x runs in the emulator\n",g_emu_lo,g_emu_hi); else g_emu_lo=g_emu_hi=0; }
gfn rt_lookup(uint32_t a){ return lookup(a); }
__thread uint32_t t_emu_thread_start;   /* thread start address, set by thr_main: BSI_EMU_THREADS=0 keeps thread bodies native */
#include "cifast.c"   /* BSI_CIFAST fast paths (COM, imports, granule duplicates, thunks) + BSI_CISTAT */
static __attribute__((noinline)) void call_indirect_slow(CPU*c, uint32_t t){   /* noinline (step E): keeps call_indirect a small leaf-like function */
  if(__builtin_expect(g_cistat!=0,0)){ if(g_cistat<0) g_cistat=getenv("BSI_CISTAT")!=0; if(g_cistat) cistat(c,t); }
  emu_range_init(); if(t>=g_emu_lo && t<g_emu_hi){ static int thr=-1; if(thr<0){ const char*e=getenv("BSI_EMU_THREADS"); thr=e?atoi(e):1; }
    int ts=(t_emu_thread_start==t); if(ts) t_emu_thread_start=0; if(thr || !ts){ emu_call(c,t); return; } }
  if(t>=IMP_BASE && t<IMP_BASE+4*imps_total()){ dispatch(c,(t-IMP_BASE)/4); return; }
  if(t>=0xE8000000u && magic_dispatch(c,t)) return;
  { extern int ww_patch_call(CPU*,uint32_t) __attribute__((weak)); if(ww_patch_call && ww_patch_call(c,t)) return; }   /* weapon wheel (weaponwheel/src/weapon_wheel.c): engine tick + GetEquippedWeapon; honours BSI_PATCHES itself */
  { static int pat=-1; if(pat<0){ const char*e=getenv("BSI_PATCHES"); pat=e?atoi(e):1; } if(pat){ int patch_call(CPU*,uint32_t); if(patch_call(c,t)) return; } }
  static int emuall=-1; if(emuall<0) emuall=getenv("BSI_EMUALL")!=0;
  gfn f=emuall?0:lookup(t); if(f){ f(c); return; }
  if(emuall && t>=0x401000u && t<0x10d4000u){ emu_call(c,t); return; }
  if(t<0x401000u || (t>=0x1700000u && t<0x10000000u) || t>=0x70000000u){ fprintf(stderr,"bad indirect call target %08x (return %08x, ecx %08x)\n",t,R32(c->esp),c->ecx); fflush(0); abort(); }
  if(R8(t)==0xFF && R8(t+1)==0x25){   /* import thunk: jmp [IAT] */
    if(g_cifast>0 && t>=0x401000u && t<0x10d4000u && !(t>=g_emu_lo && t<g_emu_hi) && !emuall) ci_thunk_put(t,R32(t+2));
    call_import(c,R32(t+2)); return; }
  if(t>=0x401000u && t<0x10d4000u){ LOG("emulating untranslated code at %08x\n",t); emu_call(c,t); return; }
  if(dll_contains(t)){ emu_call(c,t); return; }
  LOG("call_indirect to unknown %08x (ret %08x)\n",t,R32(c->esp)); fprintf(stderr,"bad indirect %08x\n",t); exit(4);
}
/* ---- O(1) indirect-call dispatch: flat table indexed by guest address / 8 over [0x00400000, 0x10700000)
   (exe + rebased DLLs). Entry: bits 31..28 = 8|(addr&7), bits 27..0 = (host_fn - g_ft_base)>>g_ft_sh.
   g_ft_sh = 2 needs 4-byte-aligned host functions (always true on aarch64; on x86-64 -O1 builds ~76% of the functions
   are unaligned and missed the table); BSI_CIFAST (default on) uses g_ft_sh = 0 when the host text spans < 256 MB.
   136 MB reserved (NORESERVE), ~10 MB resident. Granules with 2+ function starts (427 functions), patched
   targets and BSI_EMUALL/BSI_NOFT use call_indirect_slow (the previous code, unchanged). */
#define FT_LO 0x00400000u
#define FT_HI 0x10700000u
static uint32_t *g_ft; static uintptr_t g_ft_base; static unsigned g_ft_sh=2;
#define CIC_N 4096u
static uint64_t g_cic[CIC_N]; static int g_cic_on;   /* c5b141 BSI_CICACHE (see call_indirect) */
static void ft_init(void){
  emu_range_init(); ci_init(g_emu_lo,g_emu_hi);
  if(getenv("BSI_EMUALL") || getenv("BSI_NOFT")){ g_ci_com_n=g_ci_imp_n=0; return; }   /* fast paths need the flat table (keeps those modes exact) */
  static const uint32_t patched[]={0xaf2950u,0xaf12b0u,0xaf3480u,0xaf1240u,0xaf0480u,0x1040f492u};   /* patch_call() targets (patches.c; 0xaf0480 = UXGameEngine::Tick, local_profile.c save list; 0x1040f492 = APEX FetchResults release race fix) */
  int pat=1; { const char*e=getenv("BSI_PATCHES"); if(e) pat=atoi(e); }
  int lcs=pat && getenv("BSI_LCSTAT")!=0;   /* notes/GAMETHREAD_HOT.md diagnostic */
  extern int wwise_tune_enabled(void) __attribute__((weak));
  int akt=pat && wwise_tune_enabled && wwise_tune_enabled();   /* BSI_AK_* (wwise_tune.c): its two patch_call targets */
  uintptr_t lo=~(uintptr_t)0, hi=0;
  for(uint32_t i=0;i<g_functab_n;i++){ uintptr_t f=(uintptr_t)g_functab[i].fn; if(f<lo) lo=f; if(f>hi) hi=f; }
  lo&=~(uintptr_t)3;   /* entries store (fn-lo)>>g_ft_sh */
  if(g_cifast>0 && hi-lo<(1ull<<28)) g_ft_sh=0;   /* BSI_CIFAST: byte-granular entries, unaligned functions included */
  if(hi-lo>=(1ull<<30)){ LOG("[ft] host text span too large: flat call table disabled\n"); return; }
  size_t bytes=(size_t)(FT_HI-FT_LO)/8*4;
  uint32_t *t=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); if(t==MAP_FAILED) return;
#ifdef MADV_HUGEPAGE
  madvise(t,bytes,MADV_HUGEPAGE);
#endif
  uint32_t n=0, dup=0; emu_range_init();
  for(uint32_t i=0;i<g_functab_n;i++){
    uint32_t a=g_functab[i].addr; uintptr_t f=(uintptr_t)g_functab[i].fn;
    if(a<FT_LO || a>=FT_HI || !f || (f&((1u<<g_ft_sh)-1))) continue;
    if(a>=g_emu_lo && a<g_emu_hi) continue;   /* BSI_EMU_RANGE: keep these on the slow path */
    if(pat){ int skip=0; for(unsigned k=0;k<sizeof patched/sizeof *patched;k++) if(a==patched[k]) skip=1; if(skip) continue; }
    { extern int ww_skip(uint32_t) __attribute__((weak)); if(pat && ww_skip && ww_skip(a)) continue; }   /* weapon_wheel.c: keeps 0x94b640 (AXPawn::GetEquippedWeapon) on the slow path */
    if(lcs && (a==0x6443c0u||a==0x6444a0u||a==0x6444f0u||a==0x644560u)) continue;   /* BSI_LCSTAT: keep them on the patch_call path */
    { extern int economy_skip(uint32_t) __attribute__((weak)); if(pat && economy_skip && economy_skip(a)) continue; }   /* economy.c: loot roll, loot table, money pickup hooks */
    if(akt && (a==0xc22160u||a==0xf6a4d0u)) continue;   /* wwise_tune.c: UXAKAudioDevice::Init, CAkRoomVerbFX::Init */
    { extern int mb_off_skip(uint32_t) __attribute__((weak)); if(pat && mb_off_skip && mb_off_skip(a)) continue; }   /* mb_off.c: motion blur + bloom switches */
    int wp_skip = 0; { extern int weapon_perks_skip(uint32_t) __attribute__((weak)); wp_skip = pat && weapon_perks_skip && weapon_perks_skip(a); }   /* weapon_perks.c: damage handler hook (asked first, applied after the gear mod's line, so both mods see the call) */
    { extern int gear_mod_skip(uint32_t) __attribute__((weak)); if(pat && gear_mod_skip && gear_mod_skip(a)) continue; }   /* gear_mod.c: damage handler hook */
    { extern int legendary_skip(uint32_t) __attribute__((weak)); if(pat && legendary_skip && legendary_skip(a)) wp_skip = 1; }   /* ext72: legendary.c damage handler hook (applied with the perks' skip) */
    if(wp_skip) continue;
    uint32_t *e=&t[(a-FT_LO)>>3]; if(*e){ dup++; ci_dup_add(a,(gfn)f); continue; }
    *e=((8u|(a&7))<<28)|(uint32_t)((f-lo)>>g_ft_sh); n++; }
  /* self-check: every table hit must equal the binary-search result (else keep the old path) */
  for(uint32_t i=0;i<g_functab_n;i++){ uint32_t a=g_functab[i].addr; if(a<FT_LO||a>=FT_HI) continue; uint32_t e=t[(a-FT_LO)>>3];
    if((e>>28)==(8u|(a&7)) && (gfn)(lo+((uintptr_t)(e&0x0fffffffu)<<g_ft_sh))!=lookup(a)){ LOG("[ft] self-check failed at %08x: disabled\n",a); munmap(t,bytes); return; } }
  if(g_ci_dup){ for(uint32_t i=0;i<CI_DUP_N;i++) if(g_ci_dup[i].a && ci_dup_find(g_ci_dup[i].a)!=lookup(g_ci_dup[i].a)){ LOG("[ci] duplicate-table self-check failed at %08x: off\n",g_ci_dup[i].a); g_ci_dup=0; break; } }
  ci_report();
  mprotect(t,bytes,PROT_READ);
  g_ft_base=lo; __atomic_store_n(&g_ft,t,__ATOMIC_RELEASE);
  { const char *e=getenv("BSI_CICACHE"); g_cic_on=e && atoi(e)>0; if(g_cic_on) fprintf(stderr,"[ft] BSI_CICACHE=1: %u-entry target cache in front of the flat table (c5b141)\n",CIC_N); }   /* c5b141: only once the table exists */
  LOG("[ft] flat call table: %u direct, %u granule duplicates, entry shift %u\n",n,dup,g_ft_sh); fprintf(stderr,"[ft] flat call table: %u direct, %u granule duplicates, entry shift %u\n",n,dup,g_ft_sh);
}
/* c5b141 BSI_CICACHE=1 (default 0 = off, the path below unchanged but for one predicted branch): a 4096-entry direct-mapped
   cache in front of the flat table, entry = (target << 32) | the flat-table entry for that target. Filled only from a flat
   table hit (the table is immutable after ft_init, mprotect'ed read-only), so a cache hit calls exactly the function the
   table names; the flat entry's top nibble is 8|(t&7) (bit 31 set), so an empty slot (0) never matches. One 64-bit
   relaxed store/load: tag and target always come from the same fill. Point: the flat table's hot entries are spread over
   ~6.5 MB (one 64-byte line and often one 4 KB page per hot target) while the cache packs them into 32 KB. Default off
   because the hit rate (and cross-thread fill traffic) is unmeasured: A/B with simpleperf before turning it on. */
void call_indirect(CPU*c, uint32_t t){
  uint32_t o=t-FT_LO; const uint32_t *ft=g_ft;
  if(__builtin_expect(g_cic_on,0)){   /* c5b141 */
    uint64_t v=__atomic_load_n(&g_cic[(t*0x9E3779B1u)>>20],__ATOMIC_RELAXED);
    if((uint32_t)(v>>32)==t && (int32_t)(uint32_t)v<0){ ((gfn)(g_ft_base+((uintptr_t)((uint32_t)v&0x0fffffffu)<<g_ft_sh)))(c); return; } }
  if(__builtin_expect(o<FT_HI-FT_LO && ft!=0,1)){
    uint32_t e=ft[o>>3];
    if(__builtin_expect((e>>28)==(8u|(t&7)),1)){
      if(__builtin_expect(g_cic_on,0)) __atomic_store_n(&g_cic[(t*0x9E3779B1u)>>20],((uint64_t)t<<32)|e,__ATOMIC_RELAXED);   /* c5b141 */
      ((gfn)(g_ft_base+((uintptr_t)(e&0x0fffffffu)<<g_ft_sh)))(c); return; }
    if(g_ci_dup){ gfn f=ci_dup_find(t); if(f){ f(c); return; } if(ci_thunk(c,t)) return; } }   /* BSI_CIFAST (cifast.c) */
  else if(t-CI_COM_BASE<g_ci_com_n){   /* BSI_CIFAST: COM method */
    extern uint8_t g_comfast[8192]; void com_dispatch_fast(CPU*,int); uint32_t gi=(t-CI_COM_BASE)>>2;   /* BSI_B1_COMFAST (com.c): armed hot context methods skip com_dispatch */
    if(__builtin_expect(gi<8192 && g_comfast[gi]!=0,0)){ com_dispatch_fast(c,(int)gi); return; }
    com_dispatch(c,t); return; }
  else if(t-IMP_BASE<g_ci_imp_n){ dispatch(c,(int)((t-IMP_BASE)/4)); return; }           /* BSI_CIFAST: import magic */
  call_indirect_slow(c,t);
}
void emu_call(CPU*c, uint32_t t); void emu_call_mid(CPU*c, uint32_t t); void emu_init(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
void dispatch_import(CPU*c, int idx){ dispatch(c,idx); } int imports_count(void){ return imps_total(); }
void unimpl(CPU*c, uint32_t addr, const char* what){ LOG("UNIMPL code at %08x: %s -> emulating\n",addr,what);
  /* continue this function in the emulator from the failing instruction; it returns to our caller's return address */
  emu_call_mid(c,addr); }
#if defined(__x86_64__) || defined(__i386__)
void host_cpuid(CPU*c){ uint32_t a=c->eax,b,cc=c->ecx,d; __asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cc),"=d"(d)); c->eax=a;c->ebx=b;c->ecx=cc;c->edx=d; }
void host_rdtsc(CPU*c){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); c->eax=lo; c->edx=hi; }
#else
/* non-x86 host (ARM64 / Quest): report a generic SSE2-class Intel CPU, 8 logical cores; TSC = 3 GHz from the monotonic clock */
void host_cpuid(CPU*c){ uint32_t leaf=c->eax; uint32_t a=0,b=0,cc=0,d=0;
  switch(leaf){
  case 0: a=0xd; b=0x756e6547; d=0x49656e69; cc=0x6c65746e; break;              /* "GenuineIntel" */
  case 1: a=0x000306c3; b=(8u<<16)|0x0800; cc=0x00000201|(1u<<19)|(1u<<20)|(1u<<23); d=0x178bfbff; break;   /* SSE..SSE4.2, POPCNT, HTT */
  case 4: a=(7u<<26)|0x121; break;
  case 0x80000000: a=0x80000004; break;
  case 0x80000001: cc=1; d=0x20100000; break;
  case 0x80000002: case 0x80000003: case 0x80000004: { static const char brand[49]="BSI-Quest ARM64 host                            "; const uint32_t *p=(const uint32_t*)brand+4*(leaf-0x80000002); a=p[0]; b=p[1]; cc=p[2]; d=p[3]; break; }
  }
  c->eax=a; c->ebx=b; c->ecx=cc; c->edx=d; }
void host_rdtsc(CPU*c){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); uint64_t t=((uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec)*3; c->eax=(uint32_t)t; c->edx=(uint32_t)(t>>32); }
#endif
/* MXCSR (C1 finding, v0.8.6): the guest's SSE rounding mode is applied to the host thread with fesetround(), so cvtps2dq /
   cvtss2si / cvtsd2si (cvt_f2i -> nearbyint) and SSE arithmetic round the way the game asked. The game switches to
   round-toward-zero (or eax,0x6000) inside f_0067f1xx and restores it afterwards. Per thread, like the real MXCSR.
   FTZ/DAZ (0x8040, used around f_00f73780) are only stored, not applied (denormals are full speed on the A78), unless BSI_FTZ=1.
   BSI_MXCSR=0 turns the rounding part off (old behaviour). */
#include <fenv.h>
static __thread uint32_t t_mxcsr = 0x1f80u;
static int g_mxcsr_rc = -1;
/* BSI_FTZ=1 (Fable micro D, notes/FABLE_REVIEW.md; default off = FTZ/DAZ only stored, as before): mirror the guest's
   MXCSR FTZ (bit 15) / DAZ (bit 6) into AArch64 FPCR.FZ (bit 24, flushes denormal inputs AND outputs = FTZ+DAZ, which is
   what Wwise sets, 0x8040) + FPCR.FZ16 (bit 19), per thread like MXCSR. Numerically faithful to the PC for the audio DSP;
   no expected speed change (the A78 handles denormals in hardware). Caveat: FZ also applies to the x87 code (doubles) that
   runs on that thread, where x86 never flushes; only matters for |x| < 2.2e-308.
   With the switch on, host_fp_thread_start() (hle_more.c thr_main) also gives each new guest thread the Windows default FP
   state (round-to-nearest, no flush): Linux threads inherit FPCR from their creator, Windows threads start at MXCSR 0x1f80. */
static int g_mxcsr_ftz = -1;
static int mxcsr_ftz_on(void){ if(__builtin_expect(g_mxcsr_ftz < 0, 0)){ const char *e = getenv("BSI_FTZ"); g_mxcsr_ftz = e && *e && *e != '0'; } return g_mxcsr_ftz; }
#if defined(__aarch64__)
static void host_apply_ftz(uint32_t v){
  const uint64_t FZ = (1ull << 24) | (1ull << 19); uint64_t fpcr, want = (v & 0x8040u) ? FZ : 0;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  if((fpcr & FZ) != want) __asm__ volatile("msr fpcr, %0" :: "r"((fpcr & ~FZ) | want));
}
static void host_fp_reset(void){
  const uint64_t CLR = (1ull << 24) | (1ull << 19) | (3ull << 22);   /* FZ, FZ16, RMode (00 = round to nearest) */
  uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  if(fpcr & CLR) __asm__ volatile("msr fpcr, %0" :: "r"(fpcr & ~CLR));
}
#else
static void host_apply_ftz(uint32_t v){ (void)v; }
static void host_fp_reset(void){ fesetround(FE_TONEAREST); }
#endif
void host_fp_thread_start(void){ if(mxcsr_ftz_on()){ host_fp_reset(); t_mxcsr = 0x1f80u; } }
void host_ldmxcsr(CPU*c,uint32_t v){
  uint32_t old = t_mxcsr; t_mxcsr = v;
  if(__builtin_expect((old ^ v) & 0x8040u, 0) && mxcsr_ftz_on()) host_apply_ftz(v);
  if(!((old ^ v) & 0x6000u)) return;
  if(g_mxcsr_rc < 0){ const char *e = getenv("BSI_MXCSR"); g_mxcsr_rc = !(e && *e == '0'); }
  if(!g_mxcsr_rc) return;
  static const int fe[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };   /* x86 RC 00 01 10 11 */
  fesetround(fe[(v >> 13) & 3]);
}
uint32_t host_stmxcsr(CPU*c){ return t_mxcsr; }
/* x87 rounding uses the FPU control word only, never the host mode (which may now follow MXCSR): explicit ties-to-even */
double fpu_round(CPU*c,double v){ switch((c->fcw>>10)&3){case 0: return __builtin_roundeven(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v);} }
extern __thread CPU *t_cpu; __thread uint32_t t_trace[16]; __thread int t_trace_i;
void TRACE(CPU*c,uint32_t a){ t_trace[t_trace_i++&15]=a; static int lg=-1; if(lg<0) lg=getenv("BSI_TRACELOG")!=0; if(lg) fprintf(stderr,"T %08x esp=%08x eax=%08x ecx=%08x edi=%08x\n",a,c->esp,c->eax,c->ecx,c->edi); }
#include <signal.h>
int bridge_segv(void *addr) __attribute__((weak));
#if defined(__aarch64__) && defined(BSI_ANDROID)
#include <dlfcn.h>
#include <ucontext.h>
static void crash_sym(const char *tag, void *a){ Dl_info di;
  if(a && dladdr(a,&di) && di.dli_sname) fprintf(stderr,"%s %p %s+0x%lx\n",tag,a,di.dli_sname,(long)((char*)a-(char*)di.dli_saddr));
  else fprintf(stderr,"%s %p\n",tag,a); }
#endif
static void on_segv(int sig, siginfo_t *si, void *uc){
  if(sig==SIGSEGV && bridge_segv && bridge_segv(si->si_addr)) return;   /* dirty-tracked GPU upload buffer */
  CPU *c=t_cpu; fprintf(stderr,"\n*** host signal %d at host addr %p (guest addr %08lx)\n",sig,si->si_addr,(long)((uint8_t*)si->si_addr-M));
#ifndef BSI_ANDROID   /* bionic has backtrace() only from API 33 */
  { const char *bt=getenv("BSI_HOSTBT"); if(bt && *bt=='1'){ extern int backtrace(void**,int); extern void backtrace_symbols_fd(void*const*,int,int); void *fr[48]; int n=backtrace(fr,48); fprintf(stderr,"[hostbt] %d frames:\n",n); backtrace_symbols_fd(fr,n,2); } }   /* BSI_HOSTBT=1: host stack of the fault */
#endif
  if(c) fprintf(stderr,"guest eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",c->eax,c->ecx,c->edx,c->ebx,c->esp,c->ebp,c->esi,c->edi);
  fprintf(stderr,"last traced guest instructions:"); for(int k=0;k<16;k++){ uint32_t v=t_trace[(t_trace_i+k)&15]; if(v) fprintf(stderr," %08x",v); } fprintf(stderr,"\n");
#if defined(__aarch64__) && defined(BSI_ANDROID)
  { /* v0.8.4d: name the crashing recompiled function (f_<guest address>) and its callers via the frame-pointer chain */
    ucontext_t *u=(ucontext_t*)uc; char tn[32]={0}; pthread_getname_np(pthread_self(),tn,sizeof tn);
    fprintf(stderr,"thread '%s' tid %d\n",tn,(int)gettid());
    crash_sym("host pc",(void*)u->uc_mcontext.pc); crash_sym("host lr",(void*)u->uc_mcontext.regs[30]);
    uint64_t *fp=(uint64_t*)u->uc_mcontext.regs[29];
    for(int k=0;k<16 && fp && !((uintptr_t)fp&7);k++){ crash_sym("  caller",(void*)fp[1]); uint64_t *nf=(uint64_t*)fp[0];
      if(nf<=fp || (char*)nf-(char*)fp>(1<<20)) break; fp=nf; } }
#endif
  signal(sig,SIG_DFL); raise(sig); }
void install_segv(void){ struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_segv; sa.sa_flags=SA_SIGINFO|SA_ONSTACK; sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); }

/* ---------------- PE loading ---------------- */
static uint32_t g_image_base, g_entry;
static void load_pe(const char *path){
  FILE *f=fopen(path,"rb"); if(!f){ perror(path); exit(1); }
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); uint8_t *b=malloc(n); fread(b,1,n,f); fclose(f);
  uint32_t pe=*(uint32_t*)(b+0x3c); uint16_t nsec=*(uint16_t*)(b+pe+6); uint16_t optsz=*(uint16_t*)(b+pe+20);
  uint8_t *opt=b+pe+24; g_image_base=*(uint32_t*)(opt+28); uint32_t imgsz=*(uint32_t*)(opt+56);
  guest_map(g_image_base,(imgsz+0xfff)&~0xfffu);
  memcpy(M+g_image_base,b,0x400);
  uint8_t *sec=opt+optsz;
  for(int i=0;i<nsec;i++,sec+=40){
    uint32_t vsz=*(uint32_t*)(sec+8), va=*(uint32_t*)(sec+12), rsz=*(uint32_t*)(sec+16), raw=*(uint32_t*)(sec+20);
    memcpy(M+g_image_base+va,b+raw,rsz<vsz||!vsz?rsz:vsz);
  }
  free(b);
}
static void patch_iat(void){
  for(int k=0;k<g_nimps;k++){
    const char *n=g_imps[k].name; uint32_t v=IMP_BASE+4*k;
    /* data imports: point the slot at guest memory holding the variable */
    if(!strcmp(n,"MSVCR90.dll!_adjust_fdiv")){ v=gmalloc(4); W32(v,0); }
    else if(!strcmp(n,"MSVCR90.dll!_acmdln")){ v=gmalloc(4); uint32_t s=gmalloc(64); strcpy((char*)M+s,"BioShockInfinite.exe"); W32(v,s); }
    else if(!strcmp(n,"MSVCR90.dll!_iob")){ v=gmalloc(32*3); memset(M+v,0,96); }
    else if(strstr(n,"MSVCP90.dll!?npos@")){ v=gmalloc(4); W32(v,0xffffffffu); }
    W32(g_imps[k].iat, v);
    if(k<1024 && dll_owns(n)) g_lazy[k]=1;
  }
}

/* ---------------- main ---------------- */
static uint32_t g_teb;
__thread CPU *t_cpu;
static pthread_mutex_t teb_mu=PTHREAD_MUTEX_INITIALIZER; static uint32_t next_teb=TEB_ADDR-0x10000;
void thread_setup_teb(CPU *c, uint32_t *teb_out, uint32_t *stk_lo){
  pthread_mutex_lock(&teb_mu); uint32_t teb=next_teb; next_teb-=0x10000; pthread_mutex_unlock(&teb_mu);
  guest_map(teb,0x2000);
  uint32_t stk=gpages(0x400000); /* 4 MB guest stack */
  W32(teb+0,0xffffffffu); W32(teb+4,stk+0x400000); W32(teb+8,stk); W32(teb+0x18,teb); W32(teb+0x30,teb+0x1000);
  { extern __thread uint32_t t_tid; W32(teb+0x20,0x1000); W32(teb+0x24,t_tid?t_tid:0x1004); }   /* ClientId: pid, tid */
  c->fs_base=teb; c->esp=stk+0x400000-0x40; *teb_out=teb; *stk_lo=stk;
}
/* Fixed guest memory base (step 4, OVERNIGHT.md). Generated code built with -DBSI_FIXED_M=<addr> uses that address as a
   compile-time constant and defines bsi_gen_fixed_base (weak); its objects also reference bsi_rt_fixed_m_ok, so they only
   link against a runtime that has this function. Then the 4 GB guest window must sit exactly at that address: reserve it
   with MAP_FIXED_NOREPLACE (never clobbers an existing mapping) and abort with a clear message if it is unavailable.
   Without the flag the window goes wherever the kernel puts it, as before. BSI_MBASE=<hex> requests a fixed base for a
   variable-base build too (testing an address on a device without rebuilding). */
__attribute__((visibility("hidden"))) const uint8_t bsi_rt_fixed_m_ok = 1;
extern const uint64_t bsi_gen_fixed_base __attribute__((weak,visibility("hidden")));
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifdef BSI_ANDROID
#include <android/log.h>
#endif
/* BSI_PROMO_EA64 (cpu.h EA()): generated code adds displacements of up to 64 KB in 64-bit arithmetic, so an x86 address
   that would have wrapped past 4 GB or below 0 lands just outside the window. Keep PROT_NONE guards of GUARD bytes on
   both sides so such an access faults like the original would (the first and last 64 KB of a 32-bit Windows process are
   never mapped) instead of reading whatever the kernel placed next to the window. Advisory: if a neighbour is already
   mapped the reservation is skipped and logged; the window itself is unaffected. */
#define GUARD 0x100000ull
static void guest_guards(uint8_t *base){
  void *lo=mmap(base-GUARD,GUARD,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
  void *hi=mmap(base+0x100000000ull,GUARD,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
  if(lo!=MAP_FAILED && lo!=(void*)(base-GUARD)){ munmap(lo,GUARD); lo=MAP_FAILED; }                 /* kernel without NOREPLACE */
  if(hi!=MAP_FAILED && hi!=(void*)(base+0x100000000ull)){ munmap(hi,GUARD); hi=MAP_FAILED; }
  LOG("[mbase] guard pages: below %s, above %s\n", lo==MAP_FAILED?"NOT reserved":"ok", hi==MAP_FAILED?"NOT reserved":"ok");
}
static uint8_t *guest_reserve(void){
  uint64_t want = &bsi_gen_fixed_base ? bsi_gen_fixed_base : 0; int compiled = want!=0;
  const char *e=getenv("BSI_MBASE");
  if(!compiled && e && *e) want=strtoull(e,0,16);
  if(!want){ uint8_t *p=mmap(0,0x100000000ull+2*GUARD,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    if(p!=MAP_FAILED) p+=GUARD;   /* the outer GUARD bytes on each side stay PROT_NONE: the guards for EA() */
    LOG("[mbase] guest memory at %p (variable base, guarded)\n",p); return p; }
  void *p=mmap((void*)(uintptr_t)want,0x100000000ull,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
  if(p==(void*)(uintptr_t)want){ LOG("[mbase] guest memory at %p (fixed base, %s)\n",p,compiled?"compiled in":"BSI_MBASE"); guest_guards(p); return p; }
  { char msg[256]; snprintf(msg,sizeof msg,"[mbase] FATAL: cannot reserve the 4 GB guest window at fixed address 0x%llx "
      "(mmap returned %p, errno %d). %s", (unsigned long long)want, p, errno, compiled ?
      "The generated code was built with this constant base (BSI_FIXED_M); rebuild it with another FIXED_M or none." :
      "BSI_MBASE request ignored, using a variable base.");
    LOG("%s\n",msg); fprintf(stderr,"%s\n",msg);
#ifdef BSI_ANDROID
    __android_log_print(ANDROID_LOG_FATAL,"bsi","%s",msg);
#endif
  }
  if(p!=MAP_FAILED) munmap(p,0x100000000ull);   /* kernel without NOREPLACE: treated the address as a hint and placed it elsewhere */
  if(!compiled){ uint8_t *q=mmap(0,0x100000000ull+2*GUARD,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); return q==MAP_FAILED?q:q+GUARD; }
  abort();
}
/* build 62: XEngine.ini fixes applied before the game reads its config (every launch, so an in-game menu change that rewrites
   the ini can't undo them). The game sizes its texture pool from video memory it reads through ddraw / WMI, which we don't
   provide, so it fell back to a tiny pool (LowPCTexturePoolSizeMB=256): blurry, "not loaded" textures (confirmed on the
   Quest 13 Oct with PoolSize=1500).
   <data folder>/texpool.txt = pool MB (Quest default 1500, 0 = leave the ini alone; Linux default off, or BSI_TEXPOOL=<MB>).
   <data folder>/mindesiredfps.txt = MinDesiredFrameRate (absent = leave alone; the game drops detail below it, default 35). */
static int ini_read_num(const char *dir, const char *name, int *v){ char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,name); FILE *f=fopen(p,"r"); if(!f) return 0; int ok=fscanf(f,"%d",v)==1; fclose(f); return ok; }
/* build 65: <data folder>/ini_overrides.txt, one "[Section] Key=Value" per line ('#' comments): replaces that key's value in
   the user's XEngine.ini at every launch (only keys the file already has). For trying engine settings without a rebuild,
   e.g. "[TextureStreaming] LoadMapTimeLimit=10". Delete the file to stop (the last values stay in XEngine.ini until the
   game or the menu rewrites them). */
static struct { char sec[64], key[64], val[64]; int hit; } g_ovr[64]; static int g_novr;
static void ovr_load(const char *dir){
  char p[1100]; snprintf(p,sizeof p,"%s/ini_overrides.txt",dir); FILE *f=fopen(p,"rb"); if(!f) return;
  char line[256];
  while(g_novr<64 && fgets(line,sizeof line,f)){
    char *a=line; while(*a==' '||*a=='\t') a++; if(*a!='[') continue;
    char *b=strchr(a,']'); if(!b) continue; char *k=b+1; while(*k==' '||*k=='\t') k++;
    char *eq=strchr(k,'='); if(!eq || eq==k) continue; char *v=eq+1; char *z=v+strcspn(v,"\r\n#"); while(z>v && (z[-1]==' '||z[-1]=='\t')) z--; *z=0;
    char *ke=eq; while(ke>k && (ke[-1]==' '||ke[-1]=='\t')) ke--;
    if(b-a+1>=63 || ke-k>=63 || z-v>=63) continue;
    snprintf(g_ovr[g_novr].sec,64,"%.*s",(int)(b-a+1),a); snprintf(g_ovr[g_novr].key,64,"%.*s",(int)(ke-k),k); snprintf(g_ovr[g_novr].val,64,"%s",v);
    g_novr++; }
  fclose(f); fprintf(stderr,"[ini] %s: %d override(s)\n",p,g_novr);
}
static void ini_tweaks(void){
  const char *root=getenv("BSI_ROOT")?getenv("BSI_ROOT"):"/home/claude/bsigame", *user=getenv("BSI_USER")?getenv("BSI_USER"):"/home/claude/bsiuser";   /* as hle_init */
#ifdef BSI_ANDROID
  int pool=1500;
#else
  int pool=getenv("BSI_TEXPOOL")?atoi(getenv("BSI_TEXPOOL")):0;
#endif
  int mdf=-1; { char sd[1024]; const char *bf=getenv("BSI_BLOOM_FILE");   /* the data folder (Quest: /sdcard/BSI, where bloom.txt / ao_sel.txt live) */
    if(getenv("BSI_INI_DIR")) snprintf(sd,sizeof sd,"%s",getenv("BSI_INI_DIR"));
    else if(bf && strrchr(bf,'/')){ snprintf(sd,sizeof sd,"%.*s",(int)(strrchr(bf,'/')-bf),bf); } else snprintf(sd,sizeof sd,"%s",root);
    ini_read_num(sd,"texpool.txt",&pool); ini_read_num(sd,"mindesiredfps.txt",&mdf);
    ovr_load(sd); }
  if(pool<0) pool=0; if(pool>4096) pool=4096;
  if(!pool && mdf<0 && !g_novr) return;
  char path[1200]; snprintf(path,sizeof path,"%s/My Games/BioShock Infinite/XGame/Config/XEngine.ini",user);
  FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"[ini] %s not there yet (first launch): fixes apply from the next launch\n",path); return; }
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n<=0||n>4000000){ fclose(f); return; }
  char *in=malloc(n+1), *out=malloc(n+4096); if(!in||!out){ fclose(f); free(in); free(out); return; }
  if(fread(in,1,n,f)!=(size_t)n){ fclose(f); free(in); free(out); return; } fclose(f); in[n]=0;
  char sec[128]=""; long o=0; int changes=0; char ps[16]; snprintf(ps,sizeof ps,"%d",pool); char ms[32]; snprintf(ms,sizeof ms,"%d.000000",mdf);
  for(char *l=in; *l; ){
    char *e=strchr(l,'\n'); long len=e?(long)(e-l+1):(long)strlen(l); int cr=(len>=2 && l[len-2]=='\r' && e) ? 1 : 0;
    long body=len-(e?1:0)-cr;   /* line without \r\n */
    if(l[0]=='['){ long k=body<127?body:127; memcpy(sec,l,k); sec[k]=0; }
    const char *val=0; long klen=0;
    #define KEY(K,V,COND) if(!val && (COND) && body>(long)strlen(K) && !strncmp(l,K "=",strlen(K)+1)){ val=V; klen=(long)strlen(K)+1; }
    int ts=!strncmp(sec,"[TextureStreaming]",18);
    KEY("PoolSize",ps,pool && ts) KEY("LowPCTexturePoolSizeMB",ps,pool && ts) KEY("TexturePoolSizeReductionMB","0",pool && ts)
    KEY("UseTextureFileCache","False",pool && ts) KEY("MinDesiredFrameRate",ms,mdf>=0)
    for(int q=0;q<g_novr && !val;q++){ size_t kl=strlen(g_ovr[q].key);   /* ini_overrides.txt: "[Section] Key=Value" */
      if(!strcmp(sec,g_ovr[q].sec) && body>(long)kl && !strncmp(l,g_ovr[q].key,kl) && l[kl]=='='){ val=g_ovr[q].val; klen=(long)kl+1; g_ovr[q].hit=1; } }
    #undef KEY
    if(val && (body-klen!=(long)strlen(val) || strncmp(l+klen,val,strlen(val)))){
      memcpy(out+o,l,klen); o+=klen; memcpy(out+o,val,strlen(val)); o+=strlen(val); if(cr) out[o++]='\r'; if(e) out[o++]='\n';
      changes++; fprintf(stderr,"[ini] %s %.*s -> %s\n",sec,(int)(klen-1),l,val);
    } else { memcpy(out+o,l,len); o+=len; }
    l+=len; if(o>n+4000) break;
  }
  if(changes){ char tmp[1300]; snprintf(tmp,sizeof tmp,"%s.bsi_tmp",path); FILE *w=fopen(tmp,"wb");
    if(w && fwrite(out,1,o,w)==(size_t)o && fclose(w)==0){ if(rename(tmp,path)==0) fprintf(stderr,"[ini] XEngine.ini updated (%d changes; texture pool %d MB%s)\n",changes,pool,mdf>=0?", MinDesiredFrameRate set":""); }
    else { if(w) fclose(w); remove(tmp); fprintf(stderr,"[ini] could not write %s\n",path); } }
  else fprintf(stderr,"[ini] XEngine.ini already has the fixes (texture pool %d MB)\n",pool);
  for(int q=0;q<g_novr;q++) if(!g_ovr[q].hit) fprintf(stderr,"[ini] override %s %s: key not in XEngine.ini (skipped)\n",g_ovr[q].sec,g_ovr[q].key);
  free(in); free(out);
}
void hle_init(int argc,char**argv); extern const char *g_exe_path;
extern void f_00fc789d(CPU*c);
#ifdef BSI_ANDROID
#define main bsi_main
#endif
int main(int argc, char **argv){
  LOGF=fopen(getenv("BSI_LOG")?getenv("BSI_LOG"):"bsi_linux.log","w");
  g_trace = getenv("BSI_NOTRACE")?0:1;
  M=guest_reserve();
  guest_map(HEAP_LO,HEAP_HI-HEAP_LO); guest_map(ARENA2_LO,ARENA2_HI-ARENA2_LO);
  { void install_segv(void); if(!getenv("BSI_NOSEGV")) install_segv(); }
  hle_init(argc,argv);
  ini_tweaks();
  { int bridge_init(void); bridge_init(); }
  { void memdump_init(void); memdump_init(); }
  { extern void prof_start_raw(const char*) __attribute__((weak)); const char *pd=getenv("BSI_PROF");   /* cloud sampling profiler (prof_android.c), off unless set */
    if(pd && *pd){ if(prof_start_raw) prof_start_raw(pd); else fprintf(stderr,"[prof] BSI_PROF set but profiler not linked\n"); } }
  load_pe(g_exe_path);
  { extern void imps_fill_stubs(void); if(!getenv("BSI_NOSTUBS")) imps_fill_stubs(); }
  patch_iat();
  hf_init();   /* BSI_HLEFAST (hle_fast.c): before any guest code runs */
  ft_init();
  guest_map(TEB_ADDR,0x2000); g_teb=TEB_ADDR; W32(TEB_ADDR+0x20,0x1000); W32(TEB_ADDR+0x24,0x1004);
  W32(g_teb+0,0xffffffffu); W32(g_teb+4,STACK_TOP); W32(g_teb+8,STACK_TOP-STACK_SIZE); W32(g_teb+0x18,g_teb); W32(g_teb+0x30,g_teb+0x1000);
  guest_map(STACK_TOP-STACK_SIZE,STACK_SIZE);
  emu_init(g_image_base,0x1300000,HEAP_LO,HEAP_HI,STACK_TOP-STACK_SIZE,STACK_SIZE,TEB_ADDR);
  { void emu_add_region(uint32_t,uint32_t); emu_add_region(ARENA2_LO,ARENA2_HI-ARENA2_LO); }
  static CPU cpu; CPU *c=&cpu; memset(c,0,sizeof *c);
  c->fcw=0x27f; c->fs_base=g_teb; c->esp=STACK_TOP-0x100; t_cpu=c;
  PUSH32(c,0xDEADBEEFu);
  LOG("image %08x, %d imports, %u functions\n", g_image_base, g_nimps, g_functab_n);
  if(getenv("BSI_EMUALL")) emu_call(c,0xfc789d); else f_00fc789d(c);
  LOG("mainCRTStartup returned eax=%08x after %ld import calls\n",c->eax,n_calls);
  return 0;
}
