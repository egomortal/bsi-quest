/* BSI_VMCACHE (notes/HLE_HOT.md, item H2): reuse cache for VirtualAlloc/VirtualFree blocks, MEM_DECOMMIT support, and
   (BSI_THRFREE) release of an exited thread's 4 MB guest stack.  #included by hle_fast.c (rt_linux.c) and by the host
   test regress/hle/hle_fast_test.c.

   Evidence: the exe calls VirtualAlloc/VirtualFree only from FMallocWindows (Malloc 0x8B9D10, Free 0x8B9950):
   every allocation of more than 32 KB (cmp ecx,0x8001) is its own VirtualAlloc(0, roundup(size,page), MEM_COMMIT) and
   its own VirtualFree(MEM_RELEASE); a small-object pool (one 64 KB VirtualAlloc) is released as soon as its last
   object is freed.  Our HLE turns each release into page_free() = madvise(MADV_DONTNEED) + a sorted-array insert
   under heap_mu (the same mutex every CRT malloc/free takes), and each re-use into fresh zero-page faults.
   madvise is 0.5-0.65 % of the RenderingThread on the Quest (in-app profiles v0.8.4b/c, crash3, profit2) and 2.9 %
   of it in the cloud boat scene (stepE_full); the re-faults land in game code / kernel time and are not counted there.

   Cache: blocks the HLE handed out for VirtualAlloc(0, n <= BSI_VMCACHE_MAXKB (1024), MEM_COMMIT) are registered.
   VirtualFree(MEM_RELEASE) of a registered block parks it in a per-size LIFO (64 KB buckets) instead of madvise'ing it;
   VirtualAlloc of the same 64 KB-rounded size takes it back.  Windows guarantees zeroed memory, so a re-used block is
   zeroed before it is returned: at release we ask mincore() which of its committed pages are resident (pages never
   touched are still zero) and memset only those at re-use (BSI_VMCACHE_MINCORE=0: memset the whole committed length).
   Bounds: BSI_VMCACHE_MB (default 32) of parked blocks; parked blocks older than BSI_VMCACHE_AGE_MS (default 3000)
   are really freed (madvise, RSS returned) on the next VirtualAlloc/VirtualFree.  Own mutex: a hit never takes heap_mu.
   All blocks still come from page_alloc, so FMallocWindows' 64 KB alignment (its pool table is indexed by addr>>16)
   is kept.  BSI_VMCACHE=0: the old handlers (installed once before guest code runs).

   MEM_DECOMMIT (0x4000), which the old handler ignored (LEAK.md item 5): madvise(MADV_DONTNEED) on the page range
   (size 0 = the whole registered block), only inside the page arenas.  Windows makes decommitted pages fault and
   recommitted pages zero; here they read as zero.  Part of BSI_VMCACHE (BSI_VMCACHE_DECOMMIT=0 to keep ignoring it).

   BSI_THRFREE (default 1 with hle_fast): each guest thread's TEB stack range (TEB+8 .. TEB+4, the 4 MB gpages block
   from thread_setup_teb) is freed by a pthread key destructor when the host thread exits (after its last guest
   instruction).  The main thread's stack is not a gpages block and is never touched. */
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

uint32_t gpages(uint32_t n); void gpages_free(uint32_t a);

#define VMC_NB 17                      /* bucket k = k*64 KB, k = 1..16 */
#define VMC_REG_BITS 15
#define VMC_REG (1<<VMC_REG_BITS)       /* registry slots (linear probing, backward-shift delete: no tombstones) */
#define VMC_MAXPARK 1024
typedef struct { uint32_t base, len; uint64_t t; uint32_t res[8]; } VmcEnt;   /* res: resident-page bitmap, 256 pages max */
static struct { uint32_t base; uint32_t size; uint32_t len; } g_vmc_reg[VMC_REG];   /* base 0 = empty */
static VmcEnt g_vmc_park[VMC_NB][VMC_MAXPARK/4]; static int g_vmc_np[VMC_NB];
static pthread_mutex_t g_vmc_mu=PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_vmc_maxsz=1u<<20; static uint64_t g_vmc_cap=32ull<<20, g_vmc_age_ns=3000000000ull, g_vmc_parked;
static int g_vmc_reg_used; static int g_vmc_mincore=1, g_vmc_decommit=1, g_vmc_stat;
static uint64_t g_vmc_n_alloc, g_vmc_n_hit, g_vmc_n_free, g_vmc_n_park, g_vmc_n_evict, g_vmc_n_decommit, g_vmc_zero_bytes;
static uint64_t g_vmc_hist[24];        /* VirtualAlloc sizes, log2 KB */

static uint64_t vmc_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static uint32_t vmc_h(uint32_t a){ return ((a>>16)*2654435761u)>>(32-VMC_REG_BITS); }
static int vmc_reg_find(uint32_t a){ uint32_t h=vmc_h(a)&(VMC_REG-1); for(int i=0;i<VMC_REG;i++,h=(h+1)&(VMC_REG-1)){ uint32_t b=g_vmc_reg[h].base; if(b==a) return (int)h; if(!b) return -1; } return -1; }
static int vmc_reg_put(uint32_t a, uint32_t size, uint32_t len){ uint32_t h=vmc_h(a)&(VMC_REG-1);
  if(g_vmc_reg_used>=VMC_REG*3/4) return 0;   /* nearly full: block is simply not cached */
  for(int i=0;i<VMC_REG;i++,h=(h+1)&(VMC_REG-1)){ uint32_t b=g_vmc_reg[h].base; if(!b||b==a){ if(!b) g_vmc_reg_used++; g_vmc_reg[h].base=a; g_vmc_reg[h].size=size; g_vmc_reg[h].len=len; return 1; } }
  return 0; }
static void vmc_reg_del(int i){   /* backward-shift deletion keeps every probe chain intact */
  uint32_t m=VMC_REG-1, j=(uint32_t)i;
  for(;;){ j=(j+1)&m; if(!g_vmc_reg[j].base) break; uint32_t k=vmc_h(g_vmc_reg[j].base)&m;
    if((uint32_t)i<=j ? ((uint32_t)i<k && k<=j) : ((uint32_t)i<k || k<=j)) continue;
    g_vmc_reg[i]=g_vmc_reg[j]; i=(int)j; }
  g_vmc_reg[i].base=0; g_vmc_reg_used--; }
/* registered block containing a (for commit-in-place / decommit); linear only over the 16 possible 64 KB bases */
static int vmc_reg_containing(uint32_t a){ uint32_t a0=a&~0xffffu;
  for(uint32_t k=0;k<16 && (k<<16)<=a0;k++){ uint32_t b=a0-(k<<16); if(b<=1) break; int i=vmc_reg_find(b); if(i>=0) return a-b<g_vmc_reg[i].size ? i : -1; }
  return -1; }

static void vmc_config(void){
  const char *s;
  if((s=getenv("BSI_VMCACHE_MB"))&&*s) g_vmc_cap=(uint64_t)atoi(s)<<20;
  if((s=getenv("BSI_VMCACHE_MAXKB"))&&*s){ int k=atoi(s); if(k<64) k=64; if(k>1024) k=1024; g_vmc_maxsz=(uint32_t)k<<10; }
  if((s=getenv("BSI_VMCACHE_AGE_MS"))&&*s) g_vmc_age_ns=(uint64_t)atoi(s)*1000000ull;
  if((s=getenv("BSI_VMCACHE_MINCORE"))&&*s) g_vmc_mincore=atoi(s)!=0;
  if((s=getenv("BSI_VMCACHE_DECOMMIT"))&&*s) g_vmc_decommit=atoi(s)!=0;
  if((s=getenv("BSI_VMCACHE_STAT"))&&*s) g_vmc_stat=atoi(s)!=0;
}
#define VMC_ST(x,v) do{ if(__builtin_expect(g_vmc_stat,0)) __atomic_fetch_add(&(x),(v),__ATOMIC_RELAXED); }while(0)

/* zero what a parked block's previous owner may have written */
static void vmc_zero(const VmcEnt *e){
  uint32_t np=(e->len+0xfff)>>12;
  for(uint32_t p=0;p<np;){
    if(!(e->res[p>>5]>>(p&31)&1)){ p++; continue; }
    uint32_t q=p; while(q<np && (e->res[q>>5]>>(q&31)&1)) q++;
    memset(MEMP(e->base+(p<<12)),0,(size_t)(q-p)<<12); VMC_ST(g_vmc_zero_bytes,(uint64_t)(q-p)<<12); p=q;
  }
}
static void vmc_resident(VmcEnt *e){
  uint32_t np=(e->len+0xfff)>>12; memset(e->res,0,sizeof e->res);
  unsigned char v[256];
  if(g_vmc_mincore && np<=256 && mincore(MEMP(e->base),(size_t)np<<12,v)==0){ for(uint32_t p=0;p<np;p++) if(v[p]&1) e->res[p>>5]|=1u<<(p&31); }
  else for(uint32_t p=0;p<np;p++) e->res[p>>5]|=1u<<(p&31);
}
/* collect parked blocks to free (age / cap); caller holds g_vmc_mu, frees after unlocking */
static int vmc_collect(uint64_t now, uint32_t *out, int max, uint64_t need){
  int n=0;
  for(;n<max;){
    int bk=-1; uint64_t bt=~0ull;
    for(int k=1;k<VMC_NB;k++) if(g_vmc_np[k] && g_vmc_park[k][0].t<bt){ bt=g_vmc_park[k][0].t; bk=k; }
    if(bk<0) break;
    if(!(now-bt>g_vmc_age_ns || g_vmc_parked+need>g_vmc_cap)) break;
    out[n++]=g_vmc_park[bk][0].base; g_vmc_parked-=(uint64_t)bk<<16;
    int r=vmc_reg_find(g_vmc_park[bk][0].base); if(r>=0) vmc_reg_del(r);
    memmove(&g_vmc_park[bk][0],&g_vmc_park[bk][1],(size_t)(--g_vmc_np[bk])*sizeof(VmcEnt));
    VMC_ST(g_vmc_n_evict,1);
  }
  return n;
}
static uint32_t vmc_alloc(uint32_t n){
  uint32_t sz=(n+0xffffu)&~0xffffu, len=(n+0xfffu)&~0xfffu, r=0; uint32_t ev[32]; int nev;
  VMC_ST(g_vmc_n_alloc,1);
  if(g_vmc_stat){ int b=0; while(b<23 && (1u<<(b+10))<n) b++; __atomic_fetch_add(&g_vmc_hist[b],1,__ATOMIC_RELAXED); }
  uint64_t now=vmc_now(); VmcEnt e; int hit=0;
  pthread_mutex_lock(&g_vmc_mu);
  int k=(int)(sz>>16);
  if(g_vmc_np[k]){ e=g_vmc_park[k][--g_vmc_np[k]]; g_vmc_parked-=sz; hit=1;
    int i=vmc_reg_find(e.base); if(i>=0) g_vmc_reg[i].len=len; }
  nev=vmc_collect(now,ev,32,0);
  pthread_mutex_unlock(&g_vmc_mu);
  for(int i=0;i<nev;i++) gpages_free(ev[i]);
  if(hit){ vmc_zero(&e); VMC_ST(g_vmc_n_hit,1); return e.base; }
  r=gpages(n);
  pthread_mutex_lock(&g_vmc_mu); vmc_reg_put(r,sz,len); pthread_mutex_unlock(&g_vmc_mu);
  return r;
}
/* returns 1 if the block was parked (nothing else to do), 0 if the caller must free it the old way */
static int vmc_release(uint32_t a){
  uint32_t ev[32]; int nev=0, parked=0; uint64_t now=vmc_now();
  VMC_ST(g_vmc_n_free,1);
  pthread_mutex_lock(&g_vmc_mu);
  int i=vmc_reg_find(a);
  if(i>=0){
    uint32_t sz=g_vmc_reg[i].size; int k=(int)(sz>>16);
    nev=vmc_collect(now,ev,32,sz);
    if(k>0 && k<VMC_NB && g_vmc_np[k]<VMC_MAXPARK/4 && g_vmc_parked+sz<=g_vmc_cap){
      VmcEnt *e=&g_vmc_park[k][g_vmc_np[k]++]; e->base=a; e->len=g_vmc_reg[i].len; e->t=now; g_vmc_parked+=sz; parked=1;
      vmc_resident(e);   /* one mincore() syscall, under the lock but short */
    } else vmc_reg_del(i);
  } else nev=vmc_collect(now,ev,32,0);
  pthread_mutex_unlock(&g_vmc_mu);
  for(int j=0;j<nev;j++) gpages_free(ev[j]);
  if(parked) VMC_ST(g_vmc_n_park,1);
  return parked;
}
#ifndef VMC_ARENA_OK
#define VMC_ARENA_OK(a,n) ( ((a)>=SMALL_HI && (uint64_t)(a)+(n)<=HEAP_HI) || ((a)>=ARENA2_LO && (uint64_t)(a)+(n)<=ARENA2_HI) )
#endif
static void vmc_decommit(uint32_t a, uint32_t n){
  if(!n){ pthread_mutex_lock(&g_vmc_mu); int i=vmc_reg_find(a); n= i>=0 ? g_vmc_reg[i].size : 0; pthread_mutex_unlock(&g_vmc_mu); if(!n) return; }
  uint32_t lo=a&~0xfffu; uint64_t hi=((uint64_t)a+n+0xfff)&~0xfffull;
  if(hi<=lo || !VMC_ARENA_OK(lo,(uint32_t)(hi-lo))) return;
  madvise(MEMP(lo),(size_t)(hi-lo),MADV_DONTNEED); VMC_ST(g_vmc_n_decommit,1);
}

typedef void (*vmc_hfn)(CPU*);
static vmc_hfn vmc_old_alloc, vmc_old_free;
static void hf_VirtualAlloc(CPU*c){ uint32_t a=ARG(0), n=ARG(1), type=ARG(2);
  if(!a && (type&0x1000u) && n && n<=g_vmc_maxsz){ RET_STD(4,vmc_alloc(n)); }
  if(a){ pthread_mutex_lock(&g_vmc_mu); int i=vmc_reg_containing(a); if(i>=0) g_vmc_reg[i].len=g_vmc_reg[i].size; pthread_mutex_unlock(&g_vmc_mu); }   /* commit in place: whole block may be dirty */
  vmc_old_alloc(c); }
static void hf_VirtualFree(CPU*c){ uint32_t a=ARG(0), n=ARG(1), type=ARG(2);
  if(type&0x8000u){ if(vmc_release(a)) RET_STD(3,1); vmc_old_free(c); return; }
  if((type&0x4000u) && g_vmc_decommit) vmc_decommit(a,n);
  RET_STD(3,1); }

static void vmc_report(FILE *f){
  if(!g_vmc_stat) return;
  fprintf(f,"[vmcache] VirtualAlloc %llu (hits %llu), releases %llu (parked %llu), evicted %llu, decommits %llu, parked now %llu KB, zeroed %llu MB\n",
    (unsigned long long)g_vmc_n_alloc,(unsigned long long)g_vmc_n_hit,(unsigned long long)g_vmc_n_free,(unsigned long long)g_vmc_n_park,
    (unsigned long long)g_vmc_n_evict,(unsigned long long)g_vmc_n_decommit,(unsigned long long)(g_vmc_parked>>10),(unsigned long long)(g_vmc_zero_bytes>>20));
  fprintf(f,"[vmcache] VirtualAlloc sizes:"); for(int b=0;b<24;b++) if(g_vmc_hist[b]) fprintf(f," <=%uK:%llu",1u<<b,(unsigned long long)g_vmc_hist[b]); fprintf(f,"\n");
}

/* ---- BSI_THRFREE: free the guest stack of an exited thread */
static pthread_key_t g_thrfree_key; static int g_thrfree_on;
static uint64_t g_thrfree_n;
static void thrfree_dtor(void *p){
  uint32_t stk=(uint32_t)(uintptr_t)p; if(!stk) return;
  gpages_free(stk); __atomic_fetch_add(&g_thrfree_n,1,__ATOMIC_RELAXED);
}
/* called once per host thread from the import fast path (hle_fast.c) */
static void thrfree_note(CPU *c){
  if(!g_thrfree_on || !c || !c->fs_base) return;
  uint32_t teb=c->fs_base, base=R32(teb+4), lo=R32(teb+8);
  if(base-lo!=0x400000u || !VMC_ARENA_OK(lo,0x400000u)) return;   /* not a thread_setup_teb stack (main thread) */
  pthread_setspecific(g_thrfree_key,(void*)(uintptr_t)lo);
}
static void thrfree_init(void){
  const char *s=getenv("BSI_THRFREE"); g_thrfree_on= !(s&&*s=='0');
  if(g_thrfree_on && pthread_key_create(&g_thrfree_key,thrfree_dtor)) g_thrfree_on=0;
}
