/* BSI_BRIDGE_THREAD=1: bridge pipelining (included at the end of bridge.c, so it shares the shadow-buffer internals).
   Calls on the immediate ID3D11DeviceContext whose result the guest cannot observe (state setters, Draw*, Dispatch, Clear*,
   Copy*, UpdateSubresource, Begin/End, Unmap) plus IDXGISwapChain::Present are recorded with fully copied arguments into a
   single-producer / single-consumer byte ring and executed in order by a dedicated host thread ("BridgeThread"), which is
   then the only thread driving DXVK's D3D11 frontend for them.
   - Entries carry 64-bit sequence numbers; the consumer publishes `done` (entries completed). Every guest COM wrapper that
     a queued entry references gets the entry's sequence number at wrapper+48 ("stamp"), so lifetime and ordering checks
     are a compare against `done` instead of an AddRef/Release pair per referenced object.
   - Calls that return data or have out-parameters drain the ring first (wait until done == last published) and then run
     the normal synchronous bridge code on the calling thread. DXVK's context lock (ID3D10Multithread protection) is
     switched on, so the few calls that only wait for one object's stamp (GetData waits for its query's End) are free of
     data races with the consumer.
   - Release on an object referenced by a pending entry: AddRef first; if other references remain, the two releases happen
     now (exact return value); if this is the last one, wait until the referencing entries have executed (DXVK then holds
     its own private references for bound state) and release synchronously.
   - Map(WRITE_DISCARD / WRITE_NO_OVERWRITE) of a resource that was mapped before returns the guest shadow immediately; the
     host Map + copy of the dirty pages + Unmap is one queued entry. Tracked (write-protected) shadows are copied by the
     consumer straight from guest memory: their pages are re-locked at Unmap, so the next guest write faults and the fault
     handler waits for the shadow's pending copy. Untracked (< 64 KB) shadows are copied into the entry.
   - Present is queued and returns S_OK; before queuing frame N's Present the producer waits for frame N-1's Present, so
     at most one frame is in flight. Debug paths that read back from the context (per-draw dumps, LUT traps) run synchronously; back-buffer-only
     dumps (BSI_BBONLY, the cloud harness) keep the frame queued and drain before the readback at Present.
   Tunables: BSI_BRQ_MB (ring size, default 32), BSI_BRQ_SPIN_US (consumer/producer spin before sleeping, default 0),
   BSI_BRQ_BATCH (entries queued before a sleeping consumer is woken, default 512; Present always wakes it).
   Spinning (20 us / batch 32) cost ~10% fps on the 2-core cloud VM: with more busy threads than cores the spinner steals
   the core the producer needs. [brq] stats every 10 s. */
#include <sys/syscall.h>
#include <linux/futex.h>
#include <limits.h>

int g_brq_on;
static void *q_ctx;                 /* host immediate context */
enum { OP_PAD=0, OP_CALL, OP_CDSV, OP_MINLOD, OP_UNMAP, OP_PRESENT, OP_NOP };
typedef struct { uint32_t len; uint16_t op; uint8_t slot, nargs; uint32_t bmask, pad; } EH;   /* 16 bytes, entries 8-aligned */
static struct {
  _Alignas(64) uint64_t wseq;             /* producer: entries published */
  _Alignas(64) uint64_t done, rpos;       /* consumer: entries completed, byte position */
  _Alignas(64) uint32_t csleep;           /* consumer sleeping on wseq */
  _Alignas(64) uint32_t pwait;            /* someone sleeping on done */
  _Alignas(64) uint8_t *buf; uint64_t cap, mask, maxent;
  _Alignas(64) uint64_t p_wpos, p_seq, p_kick, p_rpos, last_present; int prod; uint32_t batch; uint64_t spin_ns;
} Q;
static inline uint32_t *lo32(uint64_t *p){ return (uint32_t*)p; }   /* little-endian low word for futex */
static long q_futex(uint32_t *a, int op, uint32_t v, const struct timespec *to){ return syscall(SYS_futex, a, op, v, to, 0, 0); }
static inline void q_relax(void){
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}
/* ---- stats */
static uint64_t qs_async, qs_sync, qs_drains, qs_waits, qs_wait_ns, qs_relpend, qs_relwait, qs_amap, qs_smap, qs_big, qs_fault_waits, qs_wakes;
static unsigned qs_drain_by[8192], qs_sync_by[8192];
static uint64_t q_now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }

static inline uint64_t q_done(void){ return __atomic_load_n(&Q.done,__ATOMIC_ACQUIRE); }
/* stamps: 64-bit at guest wrapper +48 (wrappers are 8-aligned) */
static inline void q_stamp(uint32_t g, uint64_t s){ if(g) __atomic_store_n((uint64_t*)MEMP(g+48),s,__ATOMIC_RELEASE); }
static inline uint64_t q_stamp_get(uint32_t g){ return g?__atomic_load_n((uint64_t*)MEMP(g+48),__ATOMIC_ACQUIRE):0; }
static inline int q_pending(uint64_t s){ return s && q_done()<s; }

static void q_kick(void){   /* wake a sleeping consumer */
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if(__atomic_load_n(&Q.csleep,__ATOMIC_RELAXED)){ __atomic_store_n(&Q.csleep,0,__ATOMIC_RELAXED); q_futex(lo32(&Q.wseq),FUTEX_WAKE_PRIVATE,1,0); qs_wakes++; }
}
/* wait until entry s has executed (any thread; async-signal-safe: atomics + futex only) */
static void q_wait(uint64_t s){
  if(q_done()>=s) return;
  uint64_t t0=q_now_ns(); __atomic_add_fetch(&qs_waits,1,__ATOMIC_RELAXED);
  q_kick();
  for(uint64_t te=t0+Q.spin_ns;;){ for(int i=0;i<64;i++){ if(q_done()>=s) goto out; q_relax(); } if(q_now_ns()>=te) break; }
  for(;;){ uint64_t d=q_done(); if(d>=s) break;
    __atomic_store_n(&Q.pwait,1,__ATOMIC_RELAXED); __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if(q_done()>=s) break;
    q_kick();
    struct timespec to={0,2000000}; q_futex(lo32(&Q.done),FUTEX_WAIT_PRIVATE,(uint32_t)d,&to); }
out: { uint64_t dt=q_now_ns()-t0; __atomic_add_fetch(&qs_wait_ns,dt,__ATOMIC_RELAXED); __atomic_add_fetch(&g_ftw_tot_ns,dt,__ATOMIC_RELAXED); }
}
void brq_drain(void){ if(!g_brq_on) return; uint64_t w=__atomic_load_n(&Q.wseq,__ATOMIC_ACQUIRE); if(q_done()<w){ qs_drains++; g_ftw_drains++; q_wait(w); } }

/* ---- producer: reserve / commit (only the producer thread) */
static void q_space(uint64_t end){   /* wait until the byte range up to `end` is free */
  if(end-Q.p_rpos<=Q.cap) return;
  Q.p_rpos=__atomic_load_n(&Q.rpos,__ATOMIC_ACQUIRE); if(end-Q.p_rpos<=Q.cap) return;
  /* the ring is full: wait for entries to retire */
  while(end-(Q.p_rpos=__atomic_load_n(&Q.rpos,__ATOMIC_ACQUIRE))>Q.cap){ uint64_t d=q_done(); q_wait(d+1); }
}
static void q_publish(void){
  __atomic_store_n(&Q.wseq,Q.p_seq,__ATOMIC_RELEASE);
  if(Q.p_seq-Q.p_kick>=Q.batch){ Q.p_kick=Q.p_seq; q_kick(); }
}
/* returns the entry (len/op set); its sequence number is Q.p_seq+1 until q_commit */
static EH *q_reserve(uint32_t n, int op){
  n=(n+7)&~7u;
  uint64_t off=Q.p_wpos&Q.mask;
  if(off+n>Q.cap){ uint32_t pad=(uint32_t)(Q.cap-off); q_space(Q.p_wpos+pad); EH *p=(EH*)(Q.buf+off); p->len=pad; p->op=OP_PAD;
    Q.p_wpos+=pad; Q.p_seq++; q_publish(); off=0; }
  q_space(Q.p_wpos+n);
  EH *e=(EH*)(Q.buf+off); e->len=n; e->op=op; e->slot=0; e->nargs=0; e->bmask=0; return e; }
static uint64_t q_commit(EH *e){ Q.p_wpos+=e->len; Q.p_seq++; q_publish(); qs_async++; return Q.p_seq; }

/* ---- consumer */
typedef void (*QFN)(void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef void (*brfn)(CPU*); extern const brfn g_bridge[];
typedef struct { void *res; uint32_t sub, deferred, type, mflags; void *host; uint8_t *gbase; uint32_t nruns, inl, cur, pad; } QUnmap;
typedef struct { void *sc; uint32_t sync, flags; } QPresent;
/* BSI_BENCH_FT: consumer-side frame log ("<file>.c": t_s frame_ms busy_ms), busy = time spent executing entries */
static uint64_t qc_busy_ns;
static void q_ftlog(void){
  static FILE *f; static int tr; static uint64_t last, lastbusy, lf;
  if(!tr){ tr=1; const char *fp=getenv("BSI_BENCH_FT"); if(fp&&*fp){ char b[600]; snprintf(b,sizeof b,"%s.c",fp); f=fopen(b,"w"); } }
  if(!f) return; uint64_t t=q_now_ns();
  struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
  if(last) fprintf(f,"%.6f %.3f %.3f\n",rt.tv_sec+rt.tv_nsec*1e-9,(t-last)/1e6,(qc_busy_ns-lastbusy)/1e6);
  last=t; lastbusy=qc_busy_ns; if(t-lf>2000000000ull){ fflush(f); lf=t; } }
static void q_exec(EH *e){
  switch(e->op){
  case OP_CALL: { uint64_t a[8]={0}; const uint64_t *s=(const uint64_t*)(e+1);
    for(int i=0;i<e->nargs;i++) a[i]=s[i]+(((e->bmask>>i)&1)?(uint64_t)(uintptr_t)e:0);
    pt_slot_pre(q_ctx,e->slot);   /* BSI_PASSTIME (consumer side) */
    ((QFN)((*(void***)q_ctx)[e->slot]))(q_ctx,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
    pt_slot_post(q_ctx,e->slot); break; }
  case OP_CDSV: { void *v=*(void**)(e+1); const uint32_t *u=(const uint32_t*)((uint8_t*)(e+1)+8); float d; memcpy(&d,&u[1],4);
    ((void(*)(void*,void*,uint32_t,float,uint8_t))((*(void***)q_ctx)[53]))(q_ctx,v,u[0],d,(uint8_t)u[2]); break; }
  case OP_MINLOD: { void *r=*(void**)(e+1); float f; memcpy(&f,(uint8_t*)(e+1)+8,4); ((void(*)(void*,void*,float))((*(void***)q_ctx)[55]))(q_ctx,r,f); break; }
  case OP_UNMAP: { QUnmap *u=(QUnmap*)(e+1); uint8_t *host=u->host;
    if(u->deferred){ HMAP m={0}; int32_t hr=((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,HMAP*))((*(void***)q_ctx)[14]))(q_ctx,u->res,u->sub,u->type,u->mflags,&m);
      if(hr<0||!m.pData){ static int n; if(n++<20) fprintf(stderr,"[brq] deferred Map failed %08x\n",(uint32_t)hr); break; } host=m.pData; }
    if(host && u->type!=1){
      if(u->inl==1) memcpy(host,(uint8_t*)(u+1),u->cur);
      else if(u->inl==2){ const uint32_t *r=(const uint32_t*)(u+1); const uint8_t *d=(const uint8_t*)(r+2*u->nruns); for(uint32_t k=0;k<u->nruns;k++){ memcpy(host+r[2*k],d,r[2*k+1]); d+=r[2*k+1]; } }
      else { const uint32_t *r=(const uint32_t*)(u+1); for(uint32_t k=0;k<u->nruns;k++) memcpy(host+r[2*k],u->gbase+r[2*k],r[2*k+1]); } }
    ((void(*)(void*,void*,uint32_t))((*(void***)q_ctx)[15]))(q_ctx,u->res,u->sub); break; }
  case OP_PRESENT: { QPresent *p=(QPresent*)(e+1);
    pt_present(q_ctx);   /* BSI_PASSTIME: end of frame on the consumer */
    int32_t hr=((int32_t(*)(void*,uint32_t,uint32_t))((*(void***)p->sc)[8]))(p->sc,p->sync,p->flags);
    q_ftlog();
    if(hr<0){ static int n; if(n++<20) fprintf(stderr,"[brq] Present failed %08x\n",(uint32_t)hr); } break; }
  default: break; }
}
static void q_retire(uint64_t seq, uint64_t pos){
  __atomic_store_n(&Q.rpos,pos,__ATOMIC_RELEASE); __atomic_store_n(&Q.done,seq,__ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if(__atomic_load_n(&Q.pwait,__ATOMIC_RELAXED)){ __atomic_store_n(&Q.pwait,0,__ATOMIC_RELAXED); q_futex(lo32(&Q.done),FUTEX_WAKE_PRIVATE,INT_MAX,0); }
}
static void *q_thread(void *arg){
  (void)arg;
#ifdef __linux__
  pthread_setname_np(pthread_self(),"BridgeThread");
#endif
  { void prof_thread_start(void) __attribute__((weak)); if(prof_thread_start) prof_thread_start(); }
  uint64_t seq=0, pos=0;
  for(;;){
    uint64_t w=__atomic_load_n(&Q.wseq,__ATOMIC_ACQUIRE);
    if(w==seq){
      int got=0; for(uint64_t te=q_now_ns()+Q.spin_ns;!got;){ for(int i=0;i<64;i++) if(__atomic_load_n(&Q.wseq,__ATOMIC_ACQUIRE)!=seq){ got=1; break; } else q_relax();
        if(!got && q_now_ns()>=te) break; }
      if(got) continue;
      __atomic_store_n(&Q.csleep,1,__ATOMIC_RELAXED); __atomic_thread_fence(__ATOMIC_SEQ_CST);
      if(__atomic_load_n(&Q.wseq,__ATOMIC_ACQUIRE)==seq){ struct timespec to={0,20000000}; q_futex(lo32(&Q.wseq),FUTEX_WAIT_PRIVATE,(uint32_t)seq,&to); }
      __atomic_store_n(&Q.csleep,0,__ATOMIC_RELAXED); continue; }
    uint64_t tb=q_now_ns();
    while(seq<w){ EH *e=(EH*)(Q.buf+(pos&Q.mask)); q_exec(e); pos+=e->len; seq++; if(!(seq&15)) q_retire(seq,pos); }
    q_retire(seq,pos); qc_busy_ns+=q_now_ns()-tb;
  }
  return 0;
}

/* ---- call classes */
enum { QC_PASS=0, QC_SYNC, QC_RELEASE, QC_PRESENT, QC_CTX };
static uint8_t q_class[8192];
typedef struct { uint8_t k, c, sz; } QA;   /* k: 1 value, 2 object, 3 object array (count = arg c), 4 blob sz bytes, 5 blob count(arg c)*sz bytes */
typedef struct { int8_t n; QA a[8]; } QDesc;
#define U {1,0,0}
#define O {2,0,0}
#define OA(c) {3,c,0}
#define BF(s) {4,0,s}
#define BC(c,s) {5,c,s}
static QDesc q_desc[115];
static void q_desc_init(void){
  for(int i=0;i<115;i++) q_desc[i].n=-1;
#define D(slot,...) { QA t[]={__VA_ARGS__}; q_desc[slot].n=sizeof t/sizeof t[0]; memcpy(q_desc[slot].a,t,sizeof t); }
#define D0(slot) q_desc[slot].n=0;
  int setarr[]={7,8,10,16,22,25,26,31,32,59,61,62,63,65,66,67,70,71}; for(unsigned i=0;i<sizeof setarr/sizeof *setarr;i++) D(setarr[i],U,U,OA(1));
  int shd[]={9,11,23,60,64,69}; for(unsigned i=0;i<sizeof shd/sizeof *shd;i++) D(shd[i],O,OA(2),U);
  D(12,U,U,U) D(13,U,U) D(17,O) D(18,U,U,OA(1),BC(1,4),BC(1,4)) D(19,O,U,U) D(20,U,U,U,U,U) D(21,U,U,U,U)
  D(24,U) D(27,O) D(28,O) D(30,O,U) D(33,U,OA(0),O) D(34,U,OA(0),O,U,U,OA(4),BC(4,4)) D(35,O,BF(16),U) D(36,O,U)
  D(37,U,OA(0),BC(0,4)) D0(38) D(39,O,U) D(40,O,U) D(41,U,U,U) D(42,O,U) D(43,O) D(44,U,BC(0,24)) D(45,U,BC(0,16))
  D(46,O,U,U,U,U,O,U,BF(24)) D(47,O,O) D(49,O,U,O) D(50,O,BF(16)) D(51,O,BF(16)) D(52,O,BF(16)) D(54,O) D(57,O,U,O,U,U)
  D(68,U,U,OA(1),BC(1,4)) D0(110) D0(111)
#undef D
#undef D0
}
static void q_class_init(void){
  q_desc_init();
  for(int gi=0;gi<g_com_nmethods && gi<8192;gi++){ const char *n=g_com_methods[gi].name, *s=strstr(n,"::"); s=s?s+2:n;
    uint8_t k=QC_PASS;
    if(!strncmp(n,"ID3D11DeviceContext::",21)){ int slot=g_com_methods[gi].idx; k=(slot<=3||slot==112||slot==113)?QC_PASS:QC_CTX; }
    else if(!strcmp(n,"IDXGISwapChain::Present")||!strcmp(n,"IDXGISwapChain1::Present")) k=QC_PRESENT;
    else if(!strcmp(s,"Release")) k=QC_RELEASE;
    else if(!strncmp(n,"IDXGISwapChain",14) && strcmp(s,"AddRef") && strcmp(s,"QueryInterface") && strncmp(s,"GetDesc",7) && strcmp(s,"GetBuffer") && strcmp(s,"GetFullscreenState")) k=QC_SYNC;
    else if(!strncmp(n,"ID3D11Device::",14) && strncmp(s,"Create",6) && strncmp(s,"Check",5) && strncmp(s,"Get",3) && strcmp(s,"AddRef") && strcmp(s,"QueryInterface")
            && strcmp(s,"SetPrivateData") && strcmp(s,"SetPrivateDataInterface")) k=QC_SYNC;
    else if(strstr(n,"DeviceContext")) k=QC_SYNC;   /* deferred contexts, ID3D11DeviceContext1.. : keep ordering simple */
    q_class[gi]=k; }
}

/* ---- generic encoder for OP_CALL. Returns 0 if the call must take the synchronous path. */
static int q_call(CPU*c, int slot, uint32_t argbytes){
  const QDesc *d=&q_desc[slot]; if(d->n<0) return 0;
  uint32_t g[8], cnt[8]; uint32_t size=sizeof(EH)+8u*d->n;
  for(int i=0;i<d->n;i++) g[i]=ARGG(1+i);
  for(int i=0;i<d->n;i++){ const QA *a=&d->a[i]; cnt[i]=0;
    if(a->k==3||a->k==5){ uint32_t n=g[a->c]; if(n>128) return 0; cnt[i]=n; }
    if(a->k==3 && g[i]) size+=8*cnt[i];
    else if(a->k==4 && g[i]) size+=(a->sz+7)&~7u;
    else if(a->k==5 && g[i]) size+=(cnt[i]*a->sz+7)&~7u; }
  EH *e=q_reserve(size,OP_CALL); uint64_t seq=Q.p_seq+1; e->slot=slot; e->nargs=d->n;
  uint64_t *av=(uint64_t*)(e+1); uint32_t off=sizeof(EH)+8u*d->n;
  for(int i=0;i<d->n;i++){ const QA *a=&d->a[i];
    switch(a->k){
    case 1: av[i]=g[i]; break;
    case 2: av[i]=(uint64_t)(uintptr_t)br_hostc(g[i]); q_stamp(av[i]?g[i]:0,seq); break;
    case 3: if(!g[i]){ av[i]=0; break; } { void **h=(void**)((uint8_t*)e+off); for(uint32_t k=0;k<cnt[i];k++){ uint32_t o=R32(g[i]+4*k); h[k]=br_hostc(o); if(h[k]) q_stamp(o,seq); }
              av[i]=off; e->bmask|=1u<<i; off+=8*cnt[i]; } break;
    case 4: if(!g[i]){ av[i]=0; break; } memcpy((uint8_t*)e+off,MEMP(g[i]),a->sz); av[i]=off; e->bmask|=1u<<i; off+=(a->sz+7)&~7u; break;
    case 5: if(!g[i]){ av[i]=0; break; } memcpy((uint8_t*)e+off,MEMP(g[i]),cnt[i]*a->sz); av[i]=off; e->bmask|=1u<<i; off+=(cnt[i]*a->sz+7)&~7u; break; } }
  q_commit(e); c->esp+=4+argbytes; return 1;
}
/* bytes per element and block width of a DXGI format (0 = unknown) */
static int q_fmt(uint32_t f, int *bw){ *bw=1;
  if(f>=1&&f<=4) return 16; if(f>=5&&f<=8) return 12; if(f>=9&&f<=22) return 8; if(f>=23&&f<=47) return 4; if(f>=48&&f<=59) return 2; if(f>=60&&f<=65) return 1;
  if(f>=67&&f<=69) return 4; if(f>=85&&f<=86) return 2; if(f>=87&&f<=93) return 4; if(f==115) return 2;
  *bw=4; if((f>=70&&f<=72)||(f>=79&&f<=81)) return 8; if((f>=73&&f<=78)||(f>=82&&f<=84)||(f>=94&&f<=99)) return 16;
  *bw=1; return 0; }
static int q_update(CPU*c){   /* UpdateSubresource(dst, sub, pBox, pSrc, rowPitch, depthPitch): copy the source rectangle into the entry */
  uint32_t gd=ARGG(1), sub=ARGG(2), gb=ARGG(3), gs=ARGG(4), rp=ARGG(5), dp=ARGG(6);
  void *res=br_host(gd); if(!res||!gs) return 0;
  uint32_t dim=0; ((void(*)(void*,uint32_t*))((*(void***)res)[7]))(res,&dim);
  uint64_t size;
  if(dim==1){ uint32_t d[6]; ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d); size=gb?(uint64_t)(R32(gb+12)-R32(gb)):d[0]; if(gb&&R32(gb+12)<R32(gb)) size=0; }
  else if(dim>=2&&dim<=4){ uint32_t d[11]; ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d);
    uint32_t w,h=1,z=1,mips,fmt;
    if(dim==2){ w=d[0]; mips=d[1]; fmt=d[3]; } else if(dim==3){ w=d[0]; h=d[1]; mips=d[2]; fmt=d[4]; } else { w=d[0]; h=d[1]; z=d[2]; mips=d[3]; fmt=d[4]; }
    uint32_t mip=sub%(mips?mips:1); w>>=mip; h>>=mip; z>>=mip; if(!w) w=1; if(!h) h=1; if(!z) z=1;
    if(gb){ int32_t bx[6]; memcpy(bx,MEMP(gb),24); if(bx[3]<=bx[0]||bx[4]<=bx[1]||bx[5]<=bx[2]) return 0; w=bx[3]-bx[0]; h=bx[4]-bx[1]; z=bx[5]-bx[2]; }
    int bw, bpe=q_fmt(fmt,&bw); if(!bpe) return 0;
    uint32_t cols=(w+bw-1)/bw, rows=(h+bw-1)/bw;
    size=(uint64_t)(z-1)*dp+(uint64_t)(rows-1)*rp+(uint64_t)cols*bpe; }
  else return 0;
  if(!size||size+256>Q.maxent){ if(size) qs_big++; return 0; }
  EH *e=q_reserve(sizeof(EH)+48+24+(uint32_t)size,OP_CALL); uint64_t seq=Q.p_seq+1; e->slot=48; e->nargs=6;
  uint64_t *av=(uint64_t*)(e+1); uint32_t off=sizeof(EH)+48;
  av[0]=(uint64_t)(uintptr_t)res; q_stamp(gd,seq); av[1]=sub;
  if(gb){ memcpy((uint8_t*)e+off,MEMP(gb),24); av[2]=off; e->bmask|=4; off+=24; } else av[2]=0;
  memcpy((uint8_t*)e+off,MEMP(gs),size); av[3]=off; e->bmask|=8; av[4]=rp; av[5]=dp;
  q_commit(e); c->esp+=4+28; return 1;
}
static int q_release(CPU*c, void *self){
  uint32_t g=ARGG(0); uint64_t st=q_stamp_get(g);
  if(!q_pending(st)) return 0;
  qs_relpend++;
  uint32_t r=((uint32_t(*)(void*))((*(void***)self)[1]))(self);   /* AddRef: r = references incl. this one + 1 */
  if(r>=3){ ((uint32_t(*)(void*))((*(void***)self)[2]))(self); r=((uint32_t(*)(void*))((*(void***)self)[2]))(self); }
  else { qs_relwait++; g_ftw_relwait++; q_wait(st); ((uint32_t(*)(void*))((*(void***)self)[2]))(self); r=((uint32_t(*)(void*))((*(void***)self)[2]))(self); if(!r) br_forget(self); }
  c->eax=r; c->esp+=4+4; return 1;
}
static int br_dbg_sync(void);
/* BSI_BRIDGE_THREAD: Map(WRITE_DISCARD / WRITE_NO_OVERWRITE, no flags) of a (resource, subresource) that has been mapped
   synchronously before returns its guest shadow at once (the pitches are known); the host Map happens in the queued Unmap
   entry. Anything else drains the queue and takes the synchronous path above. */
static int brq_map(CPU*c, void *self){
  uint32_t gres=ARGG(1), sub=ARGG(2), type=ARGG(3), flags=ARGG(4), out=ARGG(5); void *res=br_host(gres); (void)self;
  if((type==4||type==5) && !flags && res && !lut_cb){
    pthread_mutex_lock(&sh_mu); Shadow *s=shadow_find(res,sub);
    if(s && s->gobj==gres && s->rp_known && !s->host && !s->deferred){
      s->deferred=1; s->type=type; s->mflags=flags; uint32_t g=s->guest, rp=s->rp, dp=s->dp; st_maps++; qs_amap++;
      pthread_mutex_unlock(&sh_mu);
      W32(out,g); W32(out+4,rp); W32(out+8,dp); c->eax=0; c->esp+=4+20+4; return 1; }
    pthread_mutex_unlock(&sh_mu); }
  qs_smap++; brq_drain(); return 0; }
/* queued Unmap: dirty-page runs (tracked shadows: the consumer copies straight from the re-locked guest pages) or the whole
   small shadow copied into the entry; the entry also does the deferred host Map */
static uint32_t *q_runs; static uint32_t q_runs_cap;
static int brq_unmap(CPU*c, void *self){
  uint32_t gres=ARGG(1), sub=ARGG(2); void *res=br_host(gres); (void)self; if(!res) return 0;
  pthread_mutex_lock(&sh_mu);
  Shadow *s=shadow_find(res,sub);
  if(!s || (!s->host && !s->deferred) || (!s->tracked && s->cur+4096>Q.maxent)){ pthread_mutex_unlock(&sh_mu); return 0; }
  uint32_t nr=0, inl=!s->tracked && s->type!=1, n=0; static int zc=-1; if(zc<0) zc=getenv("BSI_BRQ_ZC")&&atoi(getenv("BSI_BRQ_ZC"))>0;
  if(s->tracked){ dt_stat(s);
    dt_pick_keep(s,!zc && (uint64_t)s->npg*(g_pgsz+8)+4096<=Q.maxent);   /* keep only when the runs are copied into the entry (inl=2) */
    if(q_runs_cap<s->npg+2){ q_runs_cap=s->npg+64; q_runs=realloc(q_runs,8u*q_runs_cap); }
    for(uint32_t p=0;p<s->npg;){ if(!s->dirty[p]){ p++; continue; } uint32_t q=p; while(q<s->npg && s->dirty[q]) q++;
      uint32_t off=p*(uint32_t)g_pgsz, end=q*(uint32_t)g_pgsz; if(end>s->cur) end=s->cur;
      if(off<end && s->type!=1){ q_runs[2*nr]=off; q_runs[2*nr+1]=end-off; nr++; n+=end-off; }
      dt_relock(s,p,q); p=q; }
    st_bytes+=n; if(s->type!=1) st_saved+=s->cur-n; }
  else if(inl){ st_bytes+=s->cur; st_full++; }
  if(s->tracked && nr && !zc && (uint64_t)n+8u*nr+4096<=Q.maxent) inl=2;   /* dirty runs copied into the entry: the guest may rewrite the pages at once */
  EH *e=q_reserve(sizeof(EH)+sizeof(QUnmap)+(inl==1?s->cur:8u*nr+(inl==2?n:0)),OP_UNMAP); uint64_t seq=Q.p_seq+1;
  QUnmap *u=(QUnmap*)(e+1); u->res=res; u->sub=sub; u->deferred=s->deferred; u->type=s->type; u->mflags=s->mflags; u->host=s->host;
  u->gbase=MEMP(s->guest); u->nruns=nr; u->inl=inl; u->cur=s->cur;
  if(inl==1) memcpy(u+1,MEMP(s->guest),s->cur);
  else if(nr){ memcpy(u+1,q_runs,8u*nr);
    if(inl==2){ uint8_t *d=(uint8_t*)(u+1)+8u*nr; for(uint32_t k=0;k<nr;k++){ memcpy(d,MEMP(s->guest+q_runs[2*k]),q_runs[2*k+1]); d+=q_runs[2*k+1]; } } }
  if(s->tracked && nr && inl!=2) s->stamp=seq;   /* the consumer reads these guest pages: next guest write must wait (bridge_segv) */
  q_stamp(gres,seq); q_commit(e);
  s->host=0; s->deferred=0;
  pthread_mutex_unlock(&sh_mu);
  unmap_stat();
  c->esp+=4+8+4; return 1; }

static void q_stats(void){
  static time_t last; time_t t=time(0); if(!last){ last=t; return; } if(t-last<10) return;
  static long f0; long fr=g_br_presents-f0; f0=g_br_presents; double fd=fr>0?fr:1; last=t;
  fprintf(stderr,"[brq] per frame: async %.0f, sync %.0f, drains %.1f, waits %.1f (%.2f ms), release-pending %.1f (waited %.1f), map async %.1f / sync %.1f, big %.1f, fault waits %.1f, wakes %.1f\n",
    qs_async/fd,qs_sync/fd,qs_drains/fd,qs_waits/fd,qs_wait_ns/fd/1e6,qs_relpend/fd,qs_relwait/fd,qs_amap/fd,qs_smap/fd,qs_big/fd,qs_fault_waits/fd,qs_wakes/fd);
  for(int w=0;w<2;w++){ unsigned *tab=w?qs_sync_by:qs_drain_by; char line[600]; int n=0; line[0]=0;
    for(int r=0;r<6;r++){ int b=-1; unsigned bv=0; for(int k=0;k<8192&&k<g_com_nmethods;k++) if(tab[k]>bv){ bv=tab[k]; b=k; } if(b<0) break;
      n+=snprintf(line+n,sizeof line-n," %s %.1f",g_com_methods[b].name,bv/fd); tab[b]=0; if(n>=(int)sizeof line-80) break; }
    memset(tab,0,8192*sizeof *tab); if(line[0]) fprintf(stderr,"[brq] %s by:%s\n",w?"sync":"drained",line); }
  qs_async=qs_sync=qs_drains=qs_waits=qs_wait_ns=qs_relpend=qs_relwait=qs_amap=qs_smap=qs_big=qs_fault_waits=qs_wakes=0;
}
static void q_sync(int gi){ uint64_t w=__atomic_load_n(&Q.wseq,__ATOMIC_ACQUIRE); qs_sync++; qs_sync_by[gi]++; if(q_done()<w){ qs_drain_by[gi]++; brq_drain(); } }
/* producer-side mirror of the bound render targets / viewports (as DXVK stores them) so OMGetRenderTargets and
   RSGetViewports, which UE3 calls ~8 times a frame, are answered without draining the queue */
static struct { int known, vknown; uint32_t rtv[8], dsv, nvp; uint8_t vp[16*24]; } QS;
static void q_track(int slot, CPU*c){
  switch(slot){
  case 33: case 34: { uint32_t n=ARGG(1), pp=ARGG(2); if(slot==34 && n==0xffffffffu) break; if(n>8){ QS.known=0; break; }
    for(uint32_t k=0;k<8;k++) QS.rtv[k]=(k<n&&pp)?R32(pp+4*k):0; QS.dsv=ARGG(3); QS.known=1; break; }
  case 44: { uint32_t n=ARGG(1); if(n>16) break; QS.nvp=n; if(n) memcpy(QS.vp,MEMP(ARGG(2)),24*n); QS.vknown=1; break; }
  case 110: memset(&QS,0,sizeof QS); QS.known=QS.vknown=1; break; } }
static int q_getrt(CPU*c, int gi){
  if(!QS.known) return 0;
  uint32_t n=ARGG(1), pp=ARGG(2), pd=ARGG(3);
  for(uint32_t k=0;k<n&&k<8;k++) if(QS.rtv[k] && !br_host(QS.rtv[k])) return 0;
  if(QS.dsv && !br_host(QS.dsv)) return 0;
  if(pp) for(uint32_t k=0;k<n;k++){ uint32_t g=k<8?QS.rtv[k]:0; if(g){ void *h=br_host(g); ((uint32_t(*)(void*))((*(void***)h)[1]))(h); } W32(pp+4*k,g); }
  if(pd){ uint32_t g=QS.dsv; if(g){ void *h=br_host(g); ((uint32_t(*)(void*))((*(void***)h)[1]))(h); } W32(pd,g); }
  c->esp+=4+g_com_methods[gi].argbytes; return 1; }
static int q_getvp(CPU*c, int gi){
  if(!QS.vknown) return 0;
  uint32_t pn=ARGG(1), pv=ARGG(2), nw=QS.nvp;
  if(pv){ uint32_t want=R32(pn); if(want<nw) nw=want; for(uint32_t k=0;k<want;k++){ if(k<QS.nvp) memcpy(MEMP(pv+24*k),QS.vp+24*k,24); else memset(MEMP(pv+24*k),0,24); } }
  W32(pn,nw); c->esp+=4+g_com_methods[gi].argbytes; return 1; }
/* the immediate context is used by one guest thread at a time (D3D11 rule); when that thread changes, drain first */
static __thread int q_mytid; static int q_ntid;
static void q_adopt(void){ if(!q_mytid) q_mytid=__atomic_add_fetch(&q_ntid,1,__ATOMIC_RELAXED); if(q_mytid!=Q.prod){ brq_drain(); Q.prod=q_mytid; } }
/* returns 1 if the call was handled (queued or completed); 0: run the normal synchronous bridge path (ordering ensured) */
int brq_dispatch(CPU*c, int gi, void *self){
  if(gi>=8192) return 0;
  switch(q_class[gi]){
  case QC_PASS: return 0;
  case QC_SYNC: q_sync(gi); return 0;
  case QC_RELEASE: return q_release(c,self);
  case QC_PRESENT: {
    if(br_dbg_sync()){ q_sync(gi); return 0; }
    q_adopt();
    { uint64_t t0=g_ftw_tot_ns; q_wait(Q.last_present); g_ftw_present_ns+=g_ftw_tot_ns-t0; }
    if(br_dump_due()) brq_drain();   /* bridge_pre dumps the back buffer of this frame from this thread */
    bridge_pre(c,g_com_methods[gi].name,self);
    if(br_dbg_sync()){ q_sync(gi); g_bridge[gi](c); return 1; }   /* bridge_pre just armed a dump (snap request): this frame runs synchronously */
    EH *e=q_reserve(sizeof(EH)+sizeof(QPresent),OP_PRESENT); QPresent *p=(QPresent*)(e+1); p->sc=self; p->sync=ARGG(1); p->flags=ARGG(2);
    q_stamp(ARGG(0),Q.p_seq+1); Q.last_present=q_commit(e); q_kick(); q_stats();
    c->eax=0; c->esp+=4+g_com_methods[gi].argbytes; return 1; }
  case QC_CTX: {
    int slot=g_com_methods[gi].idx;
    if(self==q_ctx) q_track(slot,c);
    if(self!=q_ctx || br_dbg_sync()){ q_sync(gi); return 0; }
    q_adopt();
    switch(slot){
    case 89: if(q_getrt(c,gi)) return 1; q_sync(gi); return 0;
    case 95: if(q_getvp(c,gi)) return 1; q_sync(gi); return 0;
    case 14: if(brq_map(c,self)) return 1; qs_drain_by[gi]++; return 0;           /* (brq_map drained) */
    case 15: if(brq_unmap(c,self)) return 1; q_sync(gi); return 0;
    case 29: q_wait(q_stamp_get(ARGG(1))); return 0;                              /* GetData: only its query's End must have run */
    case 48: if(q_update(c)) return 1; q_sync(gi); return 0;
    case 53: { EH *e=q_reserve(sizeof(EH)+24,OP_CDSV); uint64_t seq=Q.p_seq+1; uint8_t *p=(uint8_t*)(e+1); void *v=br_hostc(ARGG(1)); memcpy(p,&v,8); if(v) q_stamp(ARGG(1),seq);
      uint32_t u[3]={ARGG(2),ARGG(3),ARGG(4)}; memcpy(p+8,u,12); q_commit(e); c->esp+=4+g_com_methods[gi].argbytes; return 1; }
    case 55: { EH *e=q_reserve(sizeof(EH)+16,OP_MINLOD); uint64_t seq=Q.p_seq+1; uint8_t *p=(uint8_t*)(e+1); void *v=br_hostc(ARGG(1)); memcpy(p,&v,8); if(v) q_stamp(ARGG(1),seq);
      uint32_t f=ARGG(2); memcpy(p+8,&f,4); q_commit(e); c->esp+=4+g_com_methods[gi].argbytes; return 1; }
    default:
      if(slot<115 && q_desc[slot].n>=0 && q_call(c,slot,g_com_methods[gi].argbytes)) return 1;
      q_sync(gi); return 0; } }
  }
  return 0;
}
static void brq_init(void *ctx){
  const char *e=getenv("BSI_BRIDGE_THREAD"); if(!e||atoi(e)<=0||!ctx) return;
  /* DXVK context lock on: the producer may call GetData (and the drained synchronous calls) while the consumer runs */
  static const uint8_t iid_mt[16]={0x00,0x4e,0x7e,0x9b,0x2c,0x34,0x06,0x41,0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0};   /* ID3D10Multithread */
  void *mt=0; int32_t hr=((int32_t(*)(void*,const void*,void**))((*(void***)ctx)[0]))(ctx,iid_mt,&mt);
  if(hr<0||!mt){ fprintf(stderr,"[brq] no ID3D10Multithread on the context (%08x): bridge thread off\n",(uint32_t)hr); brq_drain(); g_brq_on=0; return; }
  ((int32_t(*)(void*,int32_t))((*(void***)mt)[5]))(mt,1); ((uint32_t(*)(void*))((*(void***)mt)[2]))(mt);
  /* the game creates several devices (feature-level probes); the queue follows the newest immediate context */
  if(Q.buf){ brq_drain(); q_ctx=ctx; g_lastctx=ctx; fprintf(stderr,"[brq] queue now targets context %p\n",ctx); return; }
  uint64_t mb=getenv("BSI_BRQ_MB")?strtoull(getenv("BSI_BRQ_MB"),0,0):32; uint64_t cap=1; while(cap<(mb<<20)) cap<<=1; if(cap<(1u<<20)) cap=1u<<20;
  Q.buf=mmap(0,cap,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(Q.buf==MAP_FAILED){ Q.buf=0; fprintf(stderr,"[brq] ring alloc failed\n"); return; }
  Q.cap=cap; Q.mask=cap-1; Q.maxent=cap/4;
  Q.spin_ns=1000ull*(getenv("BSI_BRQ_SPIN_US")?atoi(getenv("BSI_BRQ_SPIN_US")):0); Q.batch=getenv("BSI_BRQ_BATCH")?atoi(getenv("BSI_BRQ_BATCH")):512; if(!Q.batch) Q.batch=1;
  q_ctx=ctx; g_lastctx=ctx; q_class_init();
  pthread_t t; if(pthread_create(&t,0,q_thread,0)){ fprintf(stderr,"[brq] thread create failed\n"); return; } pthread_detach(t);
  g_brq_on=1;
  fprintf(stderr,"[brq] bridge thread on: ring %llu MB, spin %u us, batch %u, DXVK context lock on\n",(unsigned long long)(cap>>20),(unsigned)(Q.spin_ns/1000),Q.batch);
}
