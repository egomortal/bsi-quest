/* BSI_BRFAST=<mask> (included at the end of bridge.c): cut D3D11 bridge work on the RenderingThread before the normal
   bridge path (com_dispatch -> bridge_pre -> generated marshalling -> DXVK). Off unless set; bits:
   1  state objects (ID3D11SamplerState / BlendState / RasterizerState / DepthStencilState):
      - Create*State is cached by (device, descriptor bytes). The cache owns one DXVK reference per entry, so the
        DXVK object (and its guest wrapper) lives as long as the process, like D3D11's own state-object dedup but
        without the destroy/re-create churn when the game drops the last reference every frame (it creates ~30
        samplers per frame in the boat scene; before, each create-from-zero also allocated a new 64-byte wrapper).
      - The guest's references are counted in the wrapper (+56 magic, +60 count G): AddRef/Release are a local atomic
        op and never reach DXVK. A Release with G==0 (a reference the guest got from a Get-/QueryInterface, which
        AddRef'd the DXVK object) is forwarded; the cache reference keeps the DXVK count >= 1, so nothing is freed.
        Return values are G after the operation (exact unless Get*-references are outstanding).
   2  redundant-bind filter on the immediate context: *SetShaderResources / *SetSamplers / *SetConstantBuffers /
      *SetShader (no class instances) / IASetVertexBuffers / IASetIndexBuffer / IASetInputLayout /
      IASetPrimitiveTopology / RSSetState / OMSetBlendState / OMSetDepthStencilState are dropped when every argument
      equals what the previous forwarded call set (per stage and slot, compared as DXVK host pointers). DXVK makes the
      same comparison itself, so a skipped call is one DXVK would have ignored; we save the guest->host round trip.
      DXVK changes its bound state on its own only through hazard resolution (binding an RTV/DSV/UAV unbinds SRVs of the
      same resource) and ClearState: OMSetRenderTargets*, CSSetUnorderedAccessViews and SOSetTargets forget the shadowed
      non-null SRVs (hazard resolution only ever nulls a slot, so known-null slots stay known: UE3 clears slots one by
      one with null binds after changing render targets); ClearState, ExecuteCommandList, *SetConstantBuffers1 and a new device forget everything. A hazard-nulled
      SRV that is bound again before the outputs change is nulled again by DXVK, so skipping it is equivalent.
      Bound objects are referenced by DXVK, so a shadowed host pointer cannot be freed and reused while shadowed.
   4  ID3D11Buffer::GetDesc (UE3 calls it before every Map) is answered from a copy taken at the first call: buffer
      descriptors are immutable. The copy lives in a ring of 4096 entries; the wrapper holds a marker (+56 'BFBD') and
      the entry index (+60), and an entry is used only if it still names this wrapper and its DXVK object. A freed and
      reused wrapper is zeroed by newobj, and a destroyed object's wrapper loses its tag, so a stale copy is never used.
   A "[bf]" line every 10 s gives the per-frame counts. */
#define BF_MAGIC 0x54534642u            /* 'BFST' at wrapper+56: state object with guest-side refcount at +60 */
#define BF_UNK ((void*)~(uintptr_t)0)   /* shadow slot not known */
#define BF_BDMAGIC 0x44424642u          /* 'BFBD' at wrapper+56: buffer with a cached descriptor, ring index at +60 */
#define BF_NBD 4096
typedef struct { uint32_t w; void *host; uint8_t d[24]; } BFBd;
static BFBd bf_bd[BF_NBD]; static uint32_t bf_bdn;
int g_bf_on=-1;
static int bf_mask;
enum { BK_NONE, BK_ADDREF, BK_RELEASE, BK_CREATE, BK_SRV, BK_SMP, BK_CB, BK_SH, BK_VB, BK_IB, BK_IL, BK_TOPO, BK_RS, BK_BS, BK_DS,
       BK_INV_SRV, BK_INV_ALL, BK_BUFDESC, BK_N };
static uint8_t bf_kind[8192], bf_stage[8192]; static uint16_t bf_dlen[8192];
static const char *bf_kname[BK_N]={"","addref","release","create","srv","smp","cb","sh","vb","ib","il","topo","rs","bs","ds","inv_srv","inv_all","bufdesc"};
/* ---- shadow of the immediate context's bindings (host pointers) */
static struct {
  void *ctx;
  void *srv[6][128], *smp[6][16], *cb[6][14], *sh[6];
  void *vb[32]; uint32_t vbs[32], vbo[32];
  void *ib; uint32_t ibf, ibo; void *il; uint32_t topo; void *rs;
  void *bs; uint32_t bfac[4], bmask; void *ds; uint32_t dsref;
} B;
/* output binding changed: DXVK's hazard resolution can only turn a bound SRV into null, so slots known to be null stay known */
static void bf_inv_srv(void){ for(int s=0;s<6;s++) for(int k=0;k<128;k++) if(B.srv[s][k]) B.srv[s][k]=BF_UNK; }
static void bf_inv_all(void){
  for(int s=0;s<6;s++) for(int k=0;k<128;k++) B.srv[s][k]=BF_UNK;
  for(int s=0;s<6;s++){ for(int k=0;k<16;k++) B.smp[s][k]=BF_UNK; for(int k=0;k<14;k++) B.cb[s][k]=BF_UNK; B.sh[s]=BF_UNK; }
  for(int k=0;k<32;k++) B.vb[k]=BF_UNK;
  B.ib=B.il=B.rs=B.bs=B.ds=BF_UNK; B.topo=~0u; }
/* stats */
static uint64_t bf_skip[BK_N], bf_fwd[BK_N], bf_hits, bf_miss, bf_rel_fwd, bf_evict, bf_nocache; static int bf_nent;
static void bf_stat(void){
  static unsigned tick; if((++tick)&1023) return;   /* the clock is read every 1024th call only */
  static double t0; static long f0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double t=ts.tv_sec+ts.tv_nsec*1e-9;
  if(t0==0){ t0=t; f0=g_br_presents; return; } if(t-t0<10.0) return;
  long fr=g_br_presents-f0; double fd=fr>0?(double)fr:1.0; char b[900]; int n=0; uint64_t sk=0, fw=0;
  for(int k=BK_SRV;k<=BK_DS;k++){ sk+=bf_skip[k]; fw+=bf_fwd[k]; }
  n+=snprintf(b+n,sizeof b-n,"[bf] per frame: refs elided %.1f (forwarded %.1f) | state creates cached %.1f, new %.1f (%d entries, evicted %.1f, uncached %.1f) | binds skipped %.1f of %.1f |",
    (bf_skip[BK_ADDREF]+bf_skip[BK_RELEASE])/fd,bf_rel_fwd/fd,bf_hits/fd,bf_miss/fd,bf_nent,bf_evict/fd,bf_nocache/fd,sk/fd,(sk+fw)/fd);
  for(int k=BK_SRV;k<=BK_DS;k++) if(bf_skip[k]+bf_fwd[k]) n+=snprintf(b+n,sizeof b-n," %s %.0f/%.0f",bf_kname[k],bf_skip[k]/fd,(bf_skip[k]+bf_fwd[k])/fd);
  n+=snprintf(b+n,sizeof b-n," | invalidations srv %.1f all %.1f | buffer GetDesc cached %.1f of %.1f\n",bf_fwd[BK_INV_SRV]/fd,bf_fwd[BK_INV_ALL]/fd,bf_skip[BK_BUFDESC]/fd,(bf_skip[BK_BUFDESC]+bf_fwd[BK_BUFDESC])/fd);
  fputs(b,stderr);
  memset(bf_skip,0,sizeof bf_skip); memset(bf_fwd,0,sizeof bf_fwd); bf_hits=bf_miss=bf_rel_fwd=bf_evict=bf_nocache=0; t0=t; f0=g_br_presents; }
/* ---- state-object cache: (device, Create method, descriptor bytes) -> guest wrapper; one DXVK reference per entry.
   Bounded per state type (BSI_BF_CAP, default 512): DXVK's sampler pool holds at most 2048 live samplers (beyond that
   new ones fall back to a default sampler), and UE3 makes many distinct descriptors over time (per-texture LOD bias).
   When a type reaches the cap, entries the guest no longer references (G==0) and has not asked for in the last 120
   frames are dropped: G is CAS'd 0 -> BF_DEAD (so the AddRef/Release fast paths stop treating the wrapper as ours)
   and the cache's DXVK reference is released, which returns the object to normal D3D11 lifetime. */
#define BF_NC 4096
#define BF_DEAD 0x80000000u
typedef struct { void *dev, *host; uint32_t w, len, gi, last; uint8_t d[264]; } BFEnt;
static BFEnt *bf_c; static pthread_mutex_t bf_mu=PTHREAD_MUTEX_INITIALIZER; static uint16_t bf_cnt[8192]; static uint32_t bf_cap=512;
static uint32_t bf_hash(void *dev, int gi, const uint8_t *d, uint32_t n){ uint64_t h=(uint64_t)(uintptr_t)dev*0x9E3779B97F4A7C15ull^((uint64_t)gi<<32)^n;
  for(uint32_t k=0;k+4<=n;k+=4){ uint32_t v; memcpy(&v,d+k,4); h=(h^v)*0x100000001B3ull; } return (uint32_t)(h^(h>>29))&(BF_NC-1); }
static void bf_insert(BFEnt *src){ for(uint32_t i=0,k=bf_hash(src->dev,src->gi,src->d,src->len);i<BF_NC;i++,k=(k+1)&(BF_NC-1)) if(!bf_c[k].w){ bf_c[k]=*src; return; } }
/* drop unreferenced, not recently used entries of Create method gi (bf_mu held); the table is rebuilt (linear probing) */
static void bf_sweep(int gi){
  static BFEnt keep[BF_NC]; uint32_t nk=0, now=(uint32_t)g_br_presents; int ne=0;
  for(uint32_t k=0;k<BF_NC;k++){ BFEnt *e=&bf_c[k]; if(!e->w) continue;
    if((int)e->gi==gi && now-e->last>120){ uint32_t z=0;   /* (already dead: another entry of the same object was evicted) */
      if(__atomic_compare_exchange_n((uint32_t*)MEMP(e->w+60),&z,BF_DEAD,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) || (z&BF_DEAD)){
        if(g_brq_on){ uint64_t qs=q_stamp_get(e->w); if(qs && q_done()<qs) q_wait(qs); }   /* BSI_BRIDGE_THREAD: queued binds hold no DXVK reference */
        uint32_t r=((uint32_t(*)(void*))((*(void***)e->host)[2]))(e->host); if(!r) br_forget(e->host);
        bf_cnt[gi]--; bf_nent--; bf_evict++; continue; } }
    keep[nk++]=*e; }
  memset(bf_c,0,BF_NC*sizeof(BFEnt)); for(uint32_t k=0;k<nk;k++) bf_insert(&keep[k]); (void)ne; }
static void bf_init(void){
  const char *e=getenv("BSI_BRFAST"); bf_mask=e?(int)strtol(e,0,0):0;
  if(!g_bridge_on || getenv("BSI_FORCE_FADE") || getenv("BSI_TRAPCB") || getenv("BSI_LUTLOG")) bf_mask&=~2;   /* debug paths bind behind the guest's back */
  if(!g_bridge_on) bf_mask=0;
  for(int gi=0;gi<g_com_nmethods && gi<8192;gi++){
    const char *n=g_com_methods[gi].name, *s=strstr(n,"::"); if(!s) continue; s+=2; int k=BK_NONE, st=0;
    if((bf_mask&1) && (!strncmp(n,"ID3D11SamplerState",18)||!strncmp(n,"ID3D11BlendState",16)||!strncmp(n,"ID3D11RasterizerState",21)||!strncmp(n,"ID3D11DepthStencilState",23))){
      if(!strcmp(s,"AddRef")) k=BK_ADDREF; else if(!strcmp(s,"Release")) k=BK_RELEASE; }
    else if((bf_mask&1) && !strncmp(n,"ID3D11Device",12) && n[12]==':'){
      uint32_t dl=!strcmp(s,"CreateSamplerState")?52:!strcmp(s,"CreateBlendState")?264:!strcmp(s,"CreateRasterizerState")?40:!strcmp(s,"CreateDepthStencilState")?52:0;
      if(dl){ k=BK_CREATE; bf_dlen[gi]=(uint16_t)dl; } }
    else if((bf_mask&2) && !strncmp(n,"ID3D11DeviceContext::",21)){
      static const char *stg[6]={"VS","PS","GS","HS","DS","CS"};
      for(int j=0;j<6;j++) if(!strncmp(s,stg[j],2)){ const char *r=s+2; st=j;
        if(!strcmp(r,"SetShaderResources")) k=BK_SRV; else if(!strcmp(r,"SetSamplers")) k=BK_SMP; else if(!strcmp(r,"SetConstantBuffers")) k=BK_CB;
        else if(!strcmp(r,"SetShader")) k=BK_SH; else if(!strcmp(r,"SetUnorderedAccessViews")) k=BK_INV_SRV; else if(!strcmp(r,"SetConstantBuffers1")) k=BK_INV_ALL; }
      if(!strcmp(s,"IASetVertexBuffers")) k=BK_VB; else if(!strcmp(s,"IASetIndexBuffer")) k=BK_IB; else if(!strcmp(s,"IASetInputLayout")) k=BK_IL;
      else if(!strcmp(s,"IASetPrimitiveTopology")) k=BK_TOPO; else if(!strcmp(s,"RSSetState")) k=BK_RS; else if(!strcmp(s,"OMSetBlendState")) k=BK_BS;
      else if(!strcmp(s,"OMSetDepthStencilState")) k=BK_DS;
      else if(!strncmp(s,"OMSetRenderTargets",18) || !strcmp(s,"SOSetTargets")) k=BK_INV_SRV;
      else if(!strcmp(s,"ClearState") || !strcmp(s,"ExecuteCommandList") || !strcmp(s,"SwapDeviceContextState")) k=BK_INV_ALL; }
    if((bf_mask&4) && !strcmp(n,"ID3D11Buffer::GetDesc")) k=BK_BUFDESC;
    if((bf_mask&9)==8 && !strncmp(n,"ID3D11Device::Create",20) && strstr(s,"State")){ k=BK_CREATE; bf_dlen[gi]=0; }   /* debug: count wrappers only */
    bf_kind[gi]=(uint8_t)k; bf_stage[gi]=(uint8_t)st; }
  if(bf_mask&1){ bf_c=calloc(BF_NC,sizeof(BFEnt)); if(getenv("BSI_BF_CAP")) bf_cap=atoi(getenv("BSI_BF_CAP")); if(bf_cap<16) bf_cap=16; if(bf_cap>1024) bf_cap=1024; }
  bf_inv_all(); B.ctx=0;
  if(bf_mask) fprintf(stderr,"[bf] BSI_BRFAST=%d: %s%s%s\n",bf_mask,(bf_mask&1)?"state-object cache + guest-side refcounts, ":"",(bf_mask&2)?"redundant-bind filter, ":"",(bf_mask&4)?"buffer GetDesc cache":"");
  g_bf_on=bf_mask?1:0; }
/* a new D3D11 device (bridge_dyn): its context may reuse a destroyed context's address */
void bf_new_device(void){ if(g_bf_on>0){ bf_inv_all(); B.ctx=0; } }
static inline int bf_ret(CPU*c, int gi, uint32_t v){ c->eax=v; c->esp+=4+g_com_methods[gi].argbytes; return 1; }
static int bf_create(CPU*c, int gi, void *dev){
  uint32_t dp=ARGG(1), pp=ARGG(2), n=bf_dlen[gi];
  if(!n){ if(!pp) return 0; g_bridge[gi](c); uint32_t w=R32(pp); bf_miss++;   /* BSI_BRFAST=8 (debug, no cache): how many creates return a never-seen wrapper */
    if(br_host(w) && R32(w+56)!=0x4e534642u){ W32(w+56,0x4e534642u); bf_nocache++; } return 1; }
  if(!dp || !pp || !bf_c) return 0;
  const uint8_t *d=MEMP(dp); uint32_t h=bf_hash(dev,gi,d,n);
  pthread_mutex_lock(&bf_mu);
  for(uint32_t i=0,k=h;i<BF_NC;i++,k=(k+1)&(BF_NC-1)){ BFEnt *e=&bf_c[k]; if(!e->w) break;
    if(e->dev==dev && (int)e->gi==gi && !memcmp(e->d,d,n)){
      uint32_t w=e->w, *g=(uint32_t*)MEMP(w+60); if(br_host(w)!=e->host || R32(w+56)!=BF_MAGIC || (*g&BF_DEAD)) continue;   /* evicted duplicate */
      __atomic_add_fetch(g,1,__ATOMIC_ACQ_REL); e->last=(uint32_t)g_br_presents; pthread_mutex_unlock(&bf_mu);
      bf_hits++; W32(pp,w); return bf_ret(c,gi,0); } }
  /* miss: create through the normal bridge path; its DXVK reference becomes the entry's */
  { static uint32_t lastsw[4]; uint32_t now=(uint32_t)g_br_presents, *ls=&lastsw[gi&3];   /* (the 4 Create*State slots are consecutive) */
    if(bf_cnt[gi]>=bf_cap && now-*ls>60){ *ls=now; bf_sweep(gi); } }
  g_bridge[gi](c); int32_t hr=(int32_t)c->eax; uint32_t w=R32(pp); void *host=br_host(w); bf_miss++;
  if(hr>=0 && host){
    uint32_t *g=(uint32_t*)MEMP(w+60), mg=R32(w+56)==BF_MAGIC, gv=mg?__atomic_load_n(g,__ATOMIC_ACQUIRE):0;
    if(bf_cnt[gi]>=bf_cap || bf_nent>=BF_NC*3/4){   /* full: plain D3D11 semantics for this reference */
      bf_nocache++; if(mg && !(gv&BF_DEAD)){ __atomic_add_fetch(g,1,__ATOMIC_ACQ_REL); ((uint32_t(*)(void*))((*(void***)host)[2]))(host); }   /* (managed: count it in G, drop the extra DXVK ref; the entry keeps one) */
      pthread_mutex_unlock(&bf_mu); return 1; }
    /* already managed (another descriptor DXVK maps to the same object): the guest's reference goes to G, the new DXVK
       reference to the new entry. A dead wrapper DXVK hands out again (it was still referenced when evicted) is re-adopted. */
    if(mg && !(gv&BF_DEAD)) __atomic_add_fetch(g,1,__ATOMIC_ACQ_REL);
    else { __atomic_store_n(g,1,__ATOMIC_RELEASE); __atomic_store_n((uint32_t*)MEMP(w+56),BF_MAGIC,__ATOMIC_RELEASE); }
    BFEnt e={dev,host,w,n,(uint32_t)gi,(uint32_t)g_br_presents}; memcpy(e.d,d,n); bf_insert(&e); bf_cnt[gi]++; bf_nent++;
    { static int lg=-1; if(lg<0) lg=getenv("BSI_BF_LOGNEW")?atoi(getenv("BSI_BF_LOGNEW")):0;
      if(lg>0){ lg--; fprintf(stderr,"[bf] new %s (%u entries):",g_com_methods[gi].name+14,bf_cnt[gi]); for(uint32_t j=0;j<n && j<64;j+=4) fprintf(stderr," %08x",R32(dp+j)); fputc('\n',stderr); } } }
  pthread_mutex_unlock(&bf_mu); return 1; }
/* returns 1 when the call was completed here */
int bf_fast(CPU*c, int gi){
  if(g_bf_on<0){ static pthread_once_t once=PTHREAD_ONCE_INIT; pthread_once(&once,bf_init); if(!g_bf_on) return 0; }
  if(gi>=8192) return 0;
  int k=bf_kind[gi]; if(!k) return 0;
  uint32_t o=ARGG(0);
  switch(k){
  case BK_ADDREF: { if(!o || R32(o+56)!=BF_MAGIC) return 0; uint32_t *p=(uint32_t*)MEMP(o+60), g=__atomic_load_n(p,__ATOMIC_ACQUIRE);
    while(!(g&BF_DEAD)){ if(__atomic_compare_exchange_n(p,&g,g+1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){ bf_skip[k]++; return bf_ret(c,gi,g+1); } }
    return 0; }   /* evicted: DXVK refcount again */
  case BK_RELEASE: { if(!o || R32(o+56)!=BF_MAGIC) return 0; uint32_t *p=(uint32_t*)MEMP(o+60), g=__atomic_load_n(p,__ATOMIC_ACQUIRE);
    while(g && !(g&BF_DEAD)){ if(__atomic_compare_exchange_n(p,&g,g-1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){ bf_skip[k]++; return bf_ret(c,gi,g-1); } }
    bf_rel_fwd++; return 0; }   /* a reference from Get-/QueryInterface: DXVK's own Release (the cache reference keeps it alive) */
  }
  void *self=br_host(o); if(!self) return 0;
  if(k==BK_BUFDESC){ uint32_t out=ARGG(1); if(!out) return 0;
    if(R32(o+56)==BF_BDMAGIC){ BFBd *e=&bf_bd[R32(o+60)&(BF_NBD-1)];
      if(__atomic_load_n(&e->w,__ATOMIC_ACQUIRE)==o && e->host==self){ uint8_t t[24]; memcpy(t,e->d,24); __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(__atomic_load_n(&e->w,__ATOMIC_ACQUIRE)==o){ memcpy(MEMP(out),t,24); bf_skip[k]++; return bf_ret(c,gi,0); } } }
    g_bridge[gi](c); bf_fwd[k]++;   /* GetDesc returns void: record what DXVK wrote */
    uint32_t i=__atomic_fetch_add(&bf_bdn,1,__ATOMIC_RELAXED)&(BF_NBD-1); BFBd *e=&bf_bd[i];
    __atomic_store_n(&e->w,0,__ATOMIC_RELEASE); memcpy(e->d,MEMP(out),24); e->host=self; __atomic_store_n(&e->w,o,__ATOMIC_RELEASE);
    W32(o+60,i); __atomic_store_n((uint32_t*)MEMP(o+56),BF_BDMAGIC,__ATOMIC_RELEASE); return 1; }
  if(k==BK_CREATE){ int r=bf_create(c,gi,self); bf_stat(); return r; }
  if(self!=B.ctx){ bf_inv_all(); B.ctx=self; }
  int st=bf_stage[gi], same=1;
  switch(k){
  case BK_SRV: case BK_SMP: case BK_CB: {
    uint32_t s0=ARGG(1), n=ARGG(2), pp=ARGG(3), lim=k==BK_SRV?128:k==BK_SMP?16:14; void **a=k==BK_SRV?B.srv[st]:k==BK_SMP?B.smp[st]:B.cb[st];
    if(s0>=lim || n>lim-s0 || (n && !pp)){ for(uint32_t j=0;j<lim;j++) a[j]=BF_UNK; same=0; break; }
    { static int lg=-1; if(lg<0) lg=getenv("BSI_BF_LOGBIND")?atoi(getenv("BSI_BF_LOGBIND")):0;   /* debug: first N SRV/sampler/cb binds */
      if(lg>0 && g_br_presents>3000){ lg--; fprintf(stderr,"[bf] %s start %u n %u:",g_com_methods[gi].name+21,s0,n); for(uint32_t j=0;j<n && j<8;j++) fprintf(stderr," %p/%p",br_host(R32(pp+4*j)),a[s0+j]); fputc('\n',stderr); } }
    for(uint32_t j=0;j<n;j++){ void *h=br_host(R32(pp+4*j)); if(a[s0+j]!=h){ same=0; a[s0+j]=h; } }
    break; }
  case BK_SH: { void *h=br_host(ARGG(1)); if(ARGG(3)){ B.sh[st]=BF_UNK; same=0; break; } if(B.sh[st]!=h){ same=0; B.sh[st]=h; } break; }
  case BK_VB: { uint32_t s0=ARGG(1), n=ARGG(2), pp=ARGG(3), ps=ARGG(4), po=ARGG(5);
    if(s0>=32 || n>32-s0 || (n && (!pp||!ps||!po))){ for(int j=0;j<32;j++) B.vb[j]=BF_UNK; same=0; break; }
    for(uint32_t j=0;j<n;j++){ void *h=br_host(R32(pp+4*j)); uint32_t sd=R32(ps+4*j), of=R32(po+4*j);
      if(B.vb[s0+j]!=h || B.vbs[s0+j]!=sd || B.vbo[s0+j]!=of){ same=0; B.vb[s0+j]=h; B.vbs[s0+j]=sd; B.vbo[s0+j]=of; } }
    break; }
  case BK_IB: { void *h=br_host(ARGG(1)); uint32_t f=ARGG(2), of=ARGG(3); if(B.ib!=h || B.ibf!=f || B.ibo!=of){ same=0; B.ib=h; B.ibf=f; B.ibo=of; } break; }
  case BK_IL: { void *h=br_host(ARGG(1)); if(B.il!=h){ same=0; B.il=h; } break; }
  case BK_TOPO: { uint32_t t=ARGG(1); if(B.topo!=t){ same=0; B.topo=t; } break; }
  case BK_RS: { void *h=br_host(ARGG(1)); if(B.rs!=h){ same=0; B.rs=h; } break; }
  case BK_BS: { void *h=br_host(ARGG(1)); uint32_t fp=ARGG(2), m=ARGG(3), f[4]={0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u};
    if(fp) memcpy(f,MEMP(fp),16);
    if(B.bs!=h || memcmp(B.bfac,f,16) || B.bmask!=m){ same=0; B.bs=h; memcpy(B.bfac,f,16); B.bmask=m; } break; }
  case BK_DS: { void *h=br_host(ARGG(1)); uint32_t r=ARGG(2); if(B.ds!=h || B.dsref!=r){ same=0; B.ds=h; B.dsref=r; } break; }
  case BK_INV_SRV: bf_inv_srv(); bf_fwd[k]++; return 0;
  case BK_INV_ALL: bf_inv_all(); bf_fwd[k]++; return 0;
  default: return 0; }
  if(same){ bf_skip[k]++; bf_stat(); return bf_ret(c,gi,0); }
  bf_fwd[k]++; bf_stat(); return 0; }
