/* Null D3D11/DXGI: guest COM objects with vtables of magic addresses (COM_BASE + 4*method). */
#include "hle.h"
#include "com.h"
uint32_t gmalloc_c(uint32_t n, uint32_t caller); void gfree(uint32_t p);
uint8_t *di_keystate(void); int di_keyevent(uint8_t*,int*); void di_mouse(int*,int*,int*,uint8_t*);
#include <strings.h>
#include <time.h>
#define COM_BASE 0xF0000000u
#define DYN_BASE 0xE8000000u
static uint32_t vtbl_of[128];
static int iface_idx(const char *n){ for(int i=0;i<g_com_nifaces;i++) if(!strcmp(g_com_ifaces[i].name,n)) return i; return -1; }
static uint32_t vtbl(int i){ if(!vtbl_of[i]){ const ComIface *f=&g_com_ifaces[i]; uint32_t v=gmalloc(4*f->n); for(int k=0;k<f->n;k++) W32(v+4*k,COM_BASE+4*(f->first+k)); vtbl_of[i]=v; } return vtbl_of[i]; }
/* object: +0 vtbl, +4 refs, +8 iface, +12 desc ptr, +16 aux, +20 aux2 */
static uint32_t newobj(int i){ uint32_t o=gmalloc_c(64,2); memset(MEMP(o),0,64); W32(o,vtbl(i)); W32(o+4,1); W32(o+8,i); return o; }
static uint32_t newobjn(const char*n){ int i=iface_idx(n); if(i<0){ fprintf(stderr,"[com] unknown iface %s\n",n); exit(11);} return newobj(i); }
static uint32_t copydesc(uint32_t src, uint32_t n){ if(!src) return 0; uint32_t d=gmalloc_c(n,3); memcpy(MEMP(d),MEMP(src),n); return d; }

/* null-renderer objects are freed when their count drops to 0 (the game creates/destroys resources every frame) */
/* perf: the reference count is updated atomically (b1n shares deduplicated state objects between the game and render
   threads, rhi_b1_com.c), and the per-interface release class (persistent / view / buffer-or-texture) is computed once
   per interface instead of seven strstr calls per Release. */
static uint8_t relcls_of[128];   /* 0 unknown, bit0 known, bit1 persistent, bit2 view, bit3 buffer/texture */
/* build 64: views hold their resource with a PRIVATE reference (obj+40), as DXVK's D3D11 does (ResourceAddRefPrivate).
   They used to take a public one, so ID3D11Texture2D::Release() returned 2+ while views were alive. UE3's D3D11 texture
   destructor (f_00894be0: AddRef, then "if (Release() == 1) TextureMemory -= size") then never subtracted the texture's
   size: the game's texture-memory counter ([0x13a17e0]) only grew, the streaming budget (pool - counter) ran out, and the
   streamer dropped mips on every texture - native's "fuzzy textures that get worse over a session".
   An object is freed when both counts are 0 (obj+44: freed flag, so the two paths can't both free it).
   BSI_VIEWREF_PUBLIC=1: the old behaviour (A/B). */
static int viewref_private(void){ static int v=-1; if(v<0){ const char*e=getenv("BSI_VIEWREF_PUBLIC"); v=!(e&&atoi(e)>0); } return v; }
static uint8_t rel_class(uint32_t o){
  uint32_t ii=R32(o+8); uint8_t k=ii<128? relcls_of[ii] : 0;
  if(!k){ const char *n= ii<(uint32_t)g_com_nifaces? g_com_ifaces[ii].name : "";
    k=1|((strstr(n,"Device")||strstr(n,"Factory")||strstr(n,"Adapter")||strstr(n,"Output")||strstr(n,"SwapChain")||strstr(n,"Context")||strstr(n,"DirectInput"))?2:0)
       |(strstr(n,"View")?4:0)|((strstr(n,"Buffer")||strstr(n,"Texture"))?8:0);
    if(ii<128) relcls_of[ii]=k; }
  return k;
}
static uint32_t com_release(uint32_t o);
static void priv_addref(uint32_t r){ __atomic_add_fetch((uint32_t*)MEMP(r+40),1,__ATOMIC_ACQ_REL); }
static void obj_free(uint32_t o, uint8_t k){
  uint32_t z=0; if(!__atomic_compare_exchange_n((uint32_t*)MEMP(o+44),&z,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) return;   /* already freed */
  if((k&4) && R32(o+16)){ uint32_t r=R32(o+16);
    if(viewref_private()){ uint32_t left=__atomic_sub_fetch((uint32_t*)MEMP(r+40),1,__ATOMIC_ACQ_REL);
      if(!left && !__atomic_load_n((uint32_t*)MEMP(r+4),__ATOMIC_ACQUIRE)) obj_free(r,rel_class(r)); }
    else com_release(r); }
  if((k&8) && R32(o+20)) gfree(R32(o+20));
  if((k&8) && R32(o+32)){ uint32_t t=R32(o+32); for(int i=0;i<256;i++) if(R32(t+4*i)) gfree(R32(t+4*i)); gfree(t); }   /* per-subresource map buffers */
  if(R32(o+12)) gfree(R32(o+12));
  { extern void rhi_b1_on_free(uint32_t) __attribute__((weak)); if(rhi_b1_on_free) rhi_b1_on_free(o); }   /* b1n: drop the shadow object now */
  W32(o,0); gfree(o);
}
static uint32_t com_release(uint32_t o){
  uint32_t *rp=(uint32_t*)MEMP(o+4), r=__atomic_load_n(rp,__ATOMIC_RELAXED);
  do{ if(!r) return 0; }while(!__atomic_compare_exchange_n(rp,&r,r-1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
  if(--r) return r;
  uint8_t k=rel_class(o);
  if(k&2) return 0;
  if(__atomic_load_n((uint32_t*)MEMP(o+40),__ATOMIC_ACQUIRE)) return 0;   /* views still hold it (private): freed with the last one */
  obj_free(o,k); return 0;
}
static int find_iid(uint32_t riid){ for(int i=0;i<g_com_nifaces;i++) if(!memcmp(MEMP(riid),&g_com_ifaces[i].iid,16) && g_com_ifaces[i].iid.d1) return i; return -1; }
static long n_present;
#define A(k) R32(c->esp+8+4*(k))   /* args after This */
#define THIS R32(c->esp+4)
static void ret(CPU*c,const ComMethod*m,uint32_t v){ c->eax=v; c->esp+=4+m->argbytes; }

/* size of the creation descriptor and which argument holds it (views: arg 1, others: arg 0) */
static void desc_size_for(const char *mn, uint32_t *n, int *arg){
  *arg=0;
  if(strstr(mn,"CreateTexture2D")) *n=44; else if(strstr(mn,"CreateTexture3D")) *n=36; else if(strstr(mn,"CreateTexture1D")) *n=32;
  else if(strstr(mn,"CreateBuffer")) *n=24; else if(strstr(mn,"CreateSamplerState")) *n=52; else if(strstr(mn,"CreateRasterizerState")) *n=40;
  else if(strstr(mn,"CreateBlendState")) *n=264; else if(strstr(mn,"CreateDepthStencilState")) *n=52;
  else if(strstr(mn,"CreateShaderResourceView")){ *n=24; *arg=1; }
  else if(strstr(mn,"CreateRenderTargetView")){ *n=20; *arg=1; }
  else if(strstr(mn,"CreateDepthStencilView")){ *n=24; *arg=1; }
  else if(strstr(mn,"CreateUnorderedAccessView")){ *n=20; *arg=1; }
  else if(strstr(mn,"CreateQuery")||strstr(mn,"CreatePredicate")||strstr(mn,"CreateCounter")){ *n=8; }
  else *n=0;
}
uint32_t com_newobj_named(const char *n){ return newobjn(n); }
int com_iface_idx(const char *n){ return iface_idx(n); }
extern int g_bridge_on; typedef void (*brfn)(CPU*); extern const brfn g_bridge[]; int bridge_dyn(CPU*c,const char*n); void *br_host(uint32_t g); void bridge_pre(CPU*,const char*,void*); void bridge_post(CPU*,const char*,void*);
/* BSI_BRSTAT=2: per-frame COM call counts over 10 s windows (all calls through com_dispatch, bridged or null);
   prints "[brstat2] t=<epoch> frames=<n> calls/frame=<x>" then the top 40 methods as "[brstat2]  <calls/frame> <name>" */
extern volatile long g_br_presents;
static void brstat_window(int gi){
  static unsigned cnt[8192]; static double t0; static long f0; static int busy;
  __atomic_add_fetch(&cnt[gi],1,__ATOMIC_RELAXED);
  static unsigned tick; if((__atomic_add_fetch(&tick,1,__ATOMIC_RELAXED)&255)) return;
  struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); double t=ts.tv_sec+ts.tv_nsec*1e-9;
  if(t0==0){ t0=t; f0=g_br_presents; return; }
  if(t-t0<10.0 || __atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return;
  long fr=g_br_presents-f0; static unsigned snap[8192]; unsigned long tot=0;
  for(int k=0;k<8192;k++){ snap[k]=__atomic_exchange_n(&cnt[k],0,__ATOMIC_RELAXED); tot+=snap[k]; }
  double fd=fr>0?(double)fr:1.0;
  fprintf(stderr,"[brstat2] t=%.1f dt=%.1f frames=%ld calls/frame=%.1f\n",t,t-t0,fr,tot/fd);
  for(int r=0;r<40;r++){ int b=-1; unsigned bv=0; for(int k=0;k<8192 && k<g_com_nmethods;k++) if(snap[k]>bv){ bv=snap[k]; b=k; } if(b<0) break;
    fprintf(stderr,"[brstat2]  %9.2f %s\n",bv/fd,g_com_methods[b].name); snap[b]=0; }
  t0=t; f0=g_br_presents; __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
}
static void com_dispatch_inner(CPU*c, uint32_t pc);
/* perf (b1n): handlers of the frequent null-device methods, reached directly through g_nfast after the first call */
static uint8_t g_nfast[8192];   /* per method: 1 default (S_OK, no output), 2 Map, 3 OMSetRenderTargets, 4 generic creation, 5 AddRef, 6 Release, 7 Unmap */
/* texture Map (b1n): one guest buffer PER SUBRESOURCE with the real D3D11 layout of that mip (block-compressed rows for
   BC formats). 13 Oct fix: every subresource used to share the one buffer at res+20 with pitch = width0*16, so a game
   that maps several mips (or array slices) before unmapping them got its data overwritten -> garbage BC blocks in the
   streamed top mips (cloud 960x540: checkered characters/buildings; Quest: grey, flat, "low-poly" surfaces). */
static void dxgi_block(uint32_t f, uint32_t *bw, uint32_t *bb){   /* block width (1 or 4) and bytes per block/pixel */
  *bw=1;
  if((f>=70&&f<=72)||(f>=79&&f<=81)){ *bw=4; *bb=8; return; }                   /* BC1, BC4 */
  if((f>=73&&f<=78)||(f>=82&&f<=84)||(f>=94&&f<=99)){ *bw=4; *bb=16; return; }  /* BC2, BC3, BC5, BC6H, BC7 */
  if(f>=1&&f<=4){ *bb=16; return; } if(f>=5&&f<=8){ *bb=12; return; } if(f>=9&&f<=22){ *bb=8; return; }
  if(f>=23&&f<=47){ *bb=4; return; } if(f>=48&&f<=59){ *bb=2; return; } if(f>=60&&f<=65){ *bb=1; return; }
  if(f>=85&&f<=93){ *bb=(f==85||f==86)?2:4; return; } if(f==115){ *bb=2; return; }
  *bb=16;
}
static void null_map(CPU*c, const ComMethod*m){
  static uint8_t rk[128];   /* per resource interface: 1 other, 2 buffer, 3 tex1d, 4 tex2d, 5 tex3d */
  uint32_t res=A(0), sub=A(1), out=A(4); uint32_t d=R32(res+12); uint32_t ifc=R32(res+8);
  int k=ifc<128? rk[ifc] : 0;
  if(!k){ const char*in=ifc<(uint32_t)g_com_nifaces? g_com_ifaces[ifc].name : "";
    k=strstr(in,"Buffer")?2: strstr(in,"Texture1D")?3: strstr(in,"Texture2D")?4: strstr(in,"Texture3D")?5: 1; if(ifc<128) rk[ifc]=(uint8_t)k; }
  static int old=-1; if(old<0){ const char*e=getenv("BSI_NULLMAP_OLD"); old=e&&atoi(e)>0; }
  if(d && k>=3 && !old){
    /* desc: Texture1D W,Mips,Array,Fmt ; Texture2D W,H,Mips,Array,Fmt ; Texture3D W,H,D,Mips,Fmt */
    uint32_t W=R32(d), H=k>=4?R32(d+4):1, D=k==5?R32(d+8):1;
    uint32_t mips=k==3?R32(d+4): k==4?R32(d+8): R32(d+12), fmt=k==3?R32(d+12): k==4?R32(d+16): R32(d+16);
    if(!mips){ uint32_t mx=W>H?W:H; if(D>mx) mx=D; mips=1; while(mx>1){ mx>>=1; mips++; } }
    uint32_t mip=sub%mips, w=W>>mip, h=H>>mip, dd=D>>mip; if(!w) w=1; if(!h) h=1; if(!dd) dd=1;
    uint32_t bw, bb; dxgi_block(fmt,&bw,&bb);
    uint32_t pitch=((w+bw-1)/bw)*bb, rows=(h+bw-1)/bw, dpitch=pitch*rows, size=dpitch*dd; if(!size) size=16;
    uint32_t tab=R32(res+32); if(!tab){ tab=gmalloc(4*256); memset(MEMP(tab),0,4*256); W32(res+32,tab); }
    uint32_t si=sub<256?sub:255, p=R32(tab+4*si);
    if(p && gsize(p)<size+64){ gfree(p); p=0; }
    if(!p){ p=gmalloc_c(size+64,4); W32(tab+4*si,p); if(!p){ static int nl; if(nl++<10) fprintf(stderr,"[null] texture Map: guest allocation of %u bytes failed (sub %u, %ux%u fmt %u)\n",size,sub,w,h,fmt); } }
    W32(out,p); W32(out+4,pitch); W32(out+8,dpitch); ret(c,m,0); return;
  }
  uint32_t pitch=4096, dpitch=0x100000, size=0x100000;
  if(d && k==2){ size=R32(d); pitch=dpitch=size; }
  else if(d && k==3){ pitch=R32(d)*16; dpitch=size=pitch; }
  else if(d && k==4){ pitch=R32(d)*16; dpitch=pitch*R32(d+4); size=dpitch; }
  else if(d && k==5){ pitch=R32(d)*16; dpitch=pitch*R32(d+4); size=dpitch*R32(d+8); }
  if(!size) size=16;
  if(!R32(res+20)){ W32(res+20,gmalloc_c(size+4096,4)); W32(res+28,size); if(!R32(res+20)){ static int nl; if(nl++<10) fprintf(stderr,"[null] Map: guest allocation of %u bytes failed\n",size); } }
  W32(out,R32(res+20)); W32(out+4,pitch); W32(out+8,dpitch); ret(c,m,0);
}
/* texture Unmap: nothing to do (the per-subresource buffers live as long as the resource, like D3D11 staging memory) */
static void null_unmap(CPU*c, const ComMethod*m){
  uint32_t res=A(0), sub=A(1), tab=res? R32(res+32) : 0;
  (void)res; (void)sub; (void)tab;   /* kept: a later Map(WRITE) of a staging texture must see the previous contents
                                        (partial updates; freeing here gave garbage in the cloud run m56a) */
  ret(c,m,0);
}
static void null_omsetrt(CPU*c, const ComMethod*m, uint32_t self){
  uint32_t st=R32(self+20); if(!st){ st=gmalloc(64); memset(MEMP(st),0,64); W32(self+20,st); }   /* [0..7] RTVs, [8] DSV */
  uint32_t n=A(0), pp=A(1); for(int k=0;k<8;k++) W32(st+4*k,(k<(int)n && pp)?R32(pp+4*k):0); W32(st+32,A(2)); ret(c,m,0);
}
static void null_create(CPU*c, const ComMethod*m, int gi){   /* generic: methods returning an interface via last out-pointer */
  uint32_t pp=R32(c->esp+m->argbytes);   /* last argument (esp -> ret, +4 this, ..., last at esp+argbytes) */
  static uint16_t dsz[8192]; static uint8_t dinfo[8192];   /* memo per method: desc size, bit0 known, bit1 desc arg 1, bit2 view creation */
  uint32_t dn; int da, isview;
  if(gi<8192 && (dinfo[gi]&1)){ dn=dsz[gi]; da=(dinfo[gi]>>1)&1; isview=(dinfo[gi]>>2)&1; }
  else{ const char *mn=m->name, *s=strstr(mn,"::")+2; desc_size_for(mn,&dn,&da); isview=(strstr(mn,"View") && strstr(s,"Create"))!=0;
    if(gi<8192){ dsz[gi]=(uint16_t)dn; dinfo[gi]=(uint8_t)(1|(da?2:0)|(isview?4:0)); } }
  uint32_t o=newobj(m->outiface); if(dn && A(da)){ W32(o+12,copydesc(A(da),dn)); W32(o+24,dn); }
  if(isview){ W32(o+16,A(0)); if(A(0)){ if(viewref_private()) priv_addref(A(0)); else __atomic_add_fetch((uint32_t*)MEMP(A(0)+4),1,__ATOMIC_ACQ_REL); } }   /* view remembers (and holds, privately) its resource */
  if(pp) W32(pp,o);
  ret(c,m,0);
}
void com_dispatch(CPU*c, uint32_t pc){   /* + BSI_RHI=b1n post hook (rhi_b1_com.c): creations on the null device bind their shadow objects */
  extern void rhi_b1_com_post(CPU*,int,uint32_t) __attribute__((weak)); uint32_t e0=c->esp;
  com_dispatch_inner(c,pc); if(rhi_b1_com_post) rhi_b1_com_post(c,(int)((pc-COM_BASE)/4),e0); }
/* perf73: name-chain branch per method (0 unknown, 0x8000 = reached through the DeviceContext block) and the offset of "::"+2 */
static uint32_t g_nbr[8192];   /* one word per method (threads race to write the same value): bits 0-14 branch, 15 via the context block, 16+ offset */
#define NB(k) do{ if(nf && !nbset && gi<8192) __atomic_store_n(&g_nbr[gi], (uint32_t)(k)|(inctx && (k)<100?0x8000u:0u)|((uint32_t)(s-mn)<<16), __ATOMIC_RELAXED); nbset=1; }while(0)
static void com_dispatch_inner(CPU*c, uint32_t pc){
  int gi=(pc-COM_BASE)/4;
  { extern int rhi_b1_com(CPU*,int) __attribute__((weak)); if(rhi_b1_com && rhi_b1_com(c,gi)) return; }   /* 1 = b1n answered the call */   /* BSI_RHI=b1 (rhi_b1.c): slot -> D3D11 call inventory */
  { static int st=-1; static unsigned cnt[8192][2]; static time_t last; if(st<0) st=getenv("BSI_BRSTAT")?(atoi(getenv("BSI_BRSTAT"))==2?2:1):0;
    if(st==2 && gi<8192){ brstat_window(gi); }
    else if(st && gi<8192){ int br=g_bridge_on && g_bridge[gi] && br_host(R32(c->esp+4)); __atomic_add_fetch(&cnt[gi][br?0:1],1,__ATOMIC_RELAXED);
      time_t now=time(0); if(now-last>=15){ last=now; fprintf(stderr,"[brstat] ---\n"); for(int k=0;k<8192;k++) if(cnt[k][0]+cnt[k][1]>50) fprintf(stderr,"[brstat] %-50s br=%u null=%u\n",g_com_methods[k].name,cnt[k][0],cnt[k][1]); } } }
  { extern int g_bf_on; int bf_fast(CPU*,int); if(g_bf_on && g_bridge_on && bf_fast(c,gi)) return; }   /* BSI_BRFAST (bridge_fast.c) */
  if(g_bridge_on && g_bridge[gi] && br_host(R32(c->esp+4))){ static int dbg=-1; if(dbg<0) dbg=getenv("BSI_BRDBG")?atoi(getenv("BSI_BRDBG")):0;
    { int bridge_skip(const char*); if(bridge_skip(g_com_methods[gi].name)){ c->eax=0; c->esp+=4+g_com_methods[gi].argbytes; return; } }
    uint32_t e0=c->esp; void *bself=br_host(R32(c->esp+4));
    { extern int g_brq_on; int brq_dispatch(CPU*,int,void*); if(g_brq_on && brq_dispatch(c,gi,bself)) return; }   /* BSI_BRIDGE_THREAD: queued */
    bridge_pre(c,g_com_methods[gi].name,bself); g_bridge[gi](c); bridge_post(c,g_com_methods[gi].name,bself); if(dbg>0){ dbg--; fprintf(stderr,"[br] %s esp %08x->%08x eax=%08x\n",g_com_methods[gi].name,e0,c->esp,c->eax); } return; } const ComMethod *m=&g_com_methods[gi]; const char *mn=m->name;
  { static int dbg=-1; static long cnt[2048]; static time_t last; if(dbg<0) dbg=getenv("BSI_COMSTAT")!=0;
    if(dbg){ cnt[gi]++; time_t now=time(0); if(now-last>=5){ last=now; fprintf(stderr,"[com] top methods:"); for(int r=0;r<6;r++){ int b=-1; long bv=0; for(int k=0;k<g_com_nmethods&&k<2048;k++) if(cnt[k]>bv){bv=cnt[k];b=k;} if(b<0) break; fprintf(stderr," %s=%ld",g_com_methods[b].name,bv); cnt[b]=-cnt[b]; } for(int k=0;k<2048;k++) if(cnt[k]<0) cnt[k]=0; fprintf(stderr,"\n"); } } }
  { static int d2=-1; if(d2<0) d2=getenv("BSI_BRDBG")?atoi(getenv("BSI_BRDBG")):0; if(d2>0){ d2--; fprintf(stderr,"[null] %s this=%08x esp=%08x\n",mn,R32(c->esp+4),c->esp); } }
  /* perf: methods whose handling below depends on the method name only (all the context Set* / Draw* calls in b1n,
     Map / Unmap, OMSetRenderTargets, the creations) skip the strcmp/strstr chain after their first call */
  static int nf=-1; if(nf<0){ const char*e=getenv("BSI_NULLFAST"); nf=!(e && atoi(e)==0); }   /* BSI_NULLFAST=0: always take the name chain */
  if(nf && gi<8192) switch(g_nfast[gi]){ case 1: ret(c,m,0); return; case 2: null_map(c,m); return; case 3: null_omsetrt(c,m,THIS); return; case 4: null_create(c,m,gi); return; case 7: null_unmap(c,m); return;
    case 5: ret(c,m,__atomic_add_fetch((uint32_t*)MEMP(THIS+4),1,__ATOMIC_ACQ_REL)); return; case 6: ret(c,m,com_release(THIS)); return; default: break; }
  /* perf73: the name chain below runs once per method; later calls jump straight to the branch it took (g_nbr), so the
     strstr/strcmp chain (twoway_strstr ~2 % of the Quest render thread: state Get* calls, GetDesc, QueryInterface ...) is
     not evaluated per call. Every predicate except GetDesc's stored-desc test depends only on the method name; that test
     is still made per call (and a GetDesc method keeps its GetDesc entry). A branch first reached through the
     DeviceContext block replays that block's state allocation. BSI_NULLFAST=0 still takes the whole chain every time. */
  uint32_t self=THIS; int ctxk=0, inctx=0, nbset=0; const char *s;
  uint32_t nbv=nf && gi<8192 ? __atomic_load_n(&g_nbr[gi],__ATOMIC_RELAXED) : 0;
  if(nbv){ uint32_t v=nbv&0xffff; nbset=1; s=mn+(nbv>>16);
    if(v&0x8000){ uint32_t st=R32(self+20); if(!st){ st=gmalloc(64); memset(MEMP(st),0,64); W32(self+20,st); } inctx=1; }
    v&=0x7fff; if(v>100){ ctxk=v; goto LCTX; }
    switch(v){ case 1: goto L1; case 2: goto L2; case 3: goto L3; case 4: goto L4; case 5: goto L5; case 6: goto L6; case 7: goto L7; case 8: goto L8; case 9: goto L9; case 10: goto L10; case 11: goto L11; case 12: goto L12; case 13: goto L13; case 14: goto L14; case 15: goto L15; case 16: goto L16; case 17: goto L17; case 18: goto L18; case 19: goto L19; case 20: goto L20; case 21: goto L21; case 22: goto L22; case 23: goto L23; case 24: goto L24; case 25: goto L25; case 26: goto L26; case 27: goto L27; case 28: goto L28; case 29: goto L29; case 30: goto L30; case 31: goto L31; case 32: goto L32; default: break; } }
  s=strstr(mn,"::")+2;
  if(!strcmp(s,"QueryInterface")){ NB(1); L1:; int i=find_iid(A(0)); if(i<0){ W32(A(1),0); ret(c,m,0x80004002u); return; } uint32_t o=newobj(i); W32(o+12,R32(self+24)?copydesc(R32(self+12),R32(self+24)):0); W32(o+16,R32(self+16)); if(R32(self+16)){ if(viewref_private() && (rel_class(o)&4)) priv_addref(R32(self+16)); else __atomic_add_fetch((uint32_t*)MEMP(R32(self+16)+4),1,__ATOMIC_ACQ_REL); } W32(o+24,R32(self+24)); W32(A(1),o); ret(c,m,0); return; }
  if(!strcmp(s,"AddRef")){ NB(2); L2:; if(gi<8192) g_nfast[gi]=5; ret(c,m,__atomic_add_fetch((uint32_t*)MEMP(self+4),1,__ATOMIC_ACQ_REL)); return; }
  if(!strcmp(s,"Release")){ NB(3); L3:; if(gi<8192) g_nfast[gi]=6; uint32_t r=com_release(self); ret(c,m,r); return; }
  /* DirectInput 8 (no devices attached; keyboard/mouse report nothing pressed) */
  if(!strncmp(mn,"IDirectInput8W::",16) || !strncmp(mn,"IDirectInput8A::",16)){ NB(4); L4:;
    { static int n; if(n++<20) fprintf(stderr,"[dinput] %s\n",mn); }
    if(!strcmp(s,"CreateDevice")){ uint32_t o=newobjn(strstr(mn,"8A")?"IDirectInputDevice8A":"IDirectInputDevice8W"); uint32_t g=A(0);
      /* GUID_SysMouse 6F1D2B60-..., GUID_SysKeyboard 6F1D2B61-... */
      W32(o+20, g? (R32(g)==0x6F1D2B61u?1: R32(g)==0x6F1D2B60u?2:0) : 0); W32(A(1),o); ret(c,m,0); return; }
    ret(c,m,0); return; }
  if(strstr(mn,"IDirectInputDevice8")){ NB(5); L5:;
    uint32_t kind=R32(self+20); { static int lg=-1; static unsigned n; if(lg<0) lg=getenv("BSI_INPUTLOG")!=0; if(lg && (n++%500)==0) fprintf(stderr,"[input] dinput %s kind %u (%u calls)\n",s,kind,n); }
    if(!strcmp(s,"GetDeviceState")){ memset(MEMP(A(1)),0,A(0));
      if(kind==1){ uint8_t *k=di_keystate(); memcpy(MEMP(A(1)),k,A(0)<256?A(0):256); }
      if(kind==2 && A(0)>=16){ int x,y,z; uint8_t b[8]; di_mouse(&x,&y,&z,b); W32(A(1),x); W32(A(1)+4,y); W32(A(1)+8,z); memcpy(MEMP(A(1)+12),b,A(0)>=20?8:4); }
      ret(c,m,0); return; }
    if(!strcmp(s,"GetDeviceData")){ uint32_t sz=A(0), buf=A(1), pn=A(2), fl=A(3), cap=pn?R32(pn):0, n=0;
      if(kind==1 && buf && !(fl&1)){ uint8_t code; int down; static uint32_t seq; while(n<cap && di_keyevent(&code,&down)){ uint32_t e=buf+n*sz; memset(MEMP(e),0,sz); W32(e,code); W32(e+4,down?0x80:0); W32(e+8,0); W32(e+12,++seq); n++; } }
      if(kind==2 && buf && !(fl&1)){ static uint8_t pb[8]; static uint32_t seq2; int x,y,z; uint8_t bt[8]; di_mouse(&x,&y,&z,bt);
        int vals[3]={x,y,z}; for(int k=0;k<3 && n<cap;k++) if(vals[k]){ uint32_t e=buf+n*sz; memset(MEMP(e),0,sz); W32(e,4*k); W32(e+4,(uint32_t)vals[k]); W32(e+12,++seq2); n++; }
        for(int k=0;k<8 && n<cap;k++) if((bt[k]&0x80)!=(pb[k]&0x80)){ uint32_t e=buf+n*sz; memset(MEMP(e),0,sz); W32(e,12+k); W32(e+4,bt[k]&0x80); W32(e+12,++seq2); n++; pb[k]=bt[k]; }
        { static int lg=-1; if(lg<0) lg=getenv("BSI_INPUTLOG")!=0; if(lg && (x||y)) fprintf(stderr,"[input] dinput mouse dx %d dy %d\n",x,y); } }
      if(pn) W32(pn,n); ret(c,m,0); return; }
    if(!strcmp(s,"GetCapabilities")){ uint32_t p=A(0), n=R32(p); if(n>4 && n<256) memset(MEMP(p+4),0,n-4); ret(c,m,0); return; }
    ret(c,m,0); return; }
  /* DXGI enumeration */
  if(!strcmp(s,"EnumAdapters")||!strcmp(s,"EnumAdapters1")){ NB(6); L6:; if(A(0)){ ret(c,m,0x887A0002u); return;} W32(A(1),newobjn(!strcmp(s,"EnumAdapters1")?"IDXGIAdapter1":"IDXGIAdapter")); ret(c,m,0); return; }
  if(!strcmp(s,"EnumOutputs")){ NB(7); L7:; if(A(0)){ ret(c,m,0x887A0002u); return;} W32(A(1),newobjn("IDXGIOutput")); ret(c,m,0); return; }
  if(strstr(mn,"IDXGIAdapter") && (!strcmp(s,"GetDesc")||!strcmp(s,"GetDesc1"))){ NB(8); L8:; uint32_t d=A(0); memset(MEMP(d),0,!strcmp(s,"GetDesc1")?296:292); const char*n="Adreno (TM) 740 (BSI-Quest null device)"; for(int k=0;n[k];k++) W16(d+2*k,n[k]); W32(d+256,0x10de); W32(d+260,0x1180); W32(d+268,0xa1); W32(d+272,2048u<<20); W32(d+280,4096u<<20); W32(d+284,0x1234); ret(c,m,0); return; }
  if(!strcmp(s,"CheckInterfaceSupport")){ NB(9); L9:; if(A(1)) W64(A(1),0x0009001200000000ull); ret(c,m,0); return; }
  if(strstr(mn,"IDXGIOutput") && !strcmp(s,"GetDesc")){ NB(10); L10:; uint32_t d=A(0); memset(MEMP(d),0,92); const char*n="\\\\.\\DISPLAY1"; for(int k=0;n[k];k++) W16(d+2*k,n[k]); W32(d+64+8,1920); W32(d+64+12,1080); W32(d+80,1); ret(c,m,0); return; }
  if(!strcmp(s,"GetDisplayModeList")){ NB(11); L11:; uint32_t pn=A(2), pd=A(3); if(!pd){ W32(pn,1); ret(c,m,0); return; } if(R32(pn)>=1){ memset(MEMP(pd),0,28); W32(pd,1920); W32(pd+4,1080); W32(pd+8,60); W32(pd+12,1); W32(pd+16,A(0)); } W32(pn,1); ret(c,m,0); return; }
  if(!strcmp(s,"FindClosestMatchingMode")){ NB(12); L12:; uint32_t in=A(0), out=A(1); memcpy(MEMP(out),MEMP(in),28); if(!R32(out)) { W32(out,1920); W32(out+4,1080);} if(!R32(out+8)){ W32(out+8,60); W32(out+12,1);} ret(c,m,0); return; }
  if(!strcmp(s,"CreateSwapChain")){ NB(13); L13:; uint32_t o=newobjn("IDXGISwapChain"); W32(o+12,copydesc(A(1),60)); W32(o+24,60); W32(o+16,A(0)); W32(A(2),o); ret(c,m,0); return; }
  if(!strcmp(s,"GetBuffer")){ NB(14); L14:; int i=find_iid(A(1)); if(i<0) i=iface_idx("ID3D11Texture2D"); uint32_t o=newobj(i); uint32_t sd=R32(self+12); uint32_t d=gmalloc(44); memset(MEMP(d),0,44); if(sd){ W32(d,R32(sd)); W32(d+4,R32(sd+4)); W32(d+16,R32(sd+16)); } W32(d+8,1); W32(d+12,1); W32(d+20,1); W32(d+28,0x28); W32(o+12,d); W32(o+24,44); W32(A(2),o); ret(c,m,0); return; }
  if(!strcmp(s,"Present")){ NB(15); L15:; n_present++; if(n_present%60==1) fprintf(stderr,"[d3d] Present #%ld\n",n_present); ret(c,m,0); return; }
  if(strstr(mn,"IDXGISwapChain") && !strcmp(s,"GetDesc")){ NB(16); L16:; uint32_t sd=R32(self+12); if(sd) memcpy(MEMP(A(0)),MEMP(sd),60); else memset(MEMP(A(0)),0,60); ret(c,m,0); return; }
  if(!strcmp(s,"GetFullscreenState")){ NB(17); L17:; if(A(0)) W32(A(0),0); if(A(1)) W32(A(1),0); ret(c,m,0); return; }
  if(!strcmp(s,"GetContainingOutput")){ NB(18); L18:; W32(A(0),newobjn("IDXGIOutput")); ret(c,m,0); return; }
  if(!strcmp(s,"GetParent")||!strcmp(s,"GetAdapter")){ NB(19); L19:; int i=m->outiface>=0?m->outiface:find_iid(A(0)); if(!strcmp(s,"GetParent")){ i=find_iid(A(0)); if(i<0) i=iface_idx("IDXGIFactory"); W32(A(1),newobj(i)); } else W32(A(0),newobj(i)); ret(c,m,0); return; }
  /* D3D11 device */
  if(!strcmp(s,"GetFeatureLevel")){ NB(20); L20:; ret(c,m,0xb000); return; }
  if(!strcmp(s,"CheckFormatSupport")){ NB(21); L21:; W32(A(1),0x0fffffffu); ret(c,m,0); return; }
  if(!strcmp(s,"CheckMultisampleQualityLevels")){ NB(22); L22:; W32(A(2),A(1)<=1?1:0); ret(c,m,0); return; }
  if(!strcmp(s,"CheckFeatureSupport")){ NB(23); L23:; memset(MEMP(A(1)),0,A(2)); ret(c,m,0); return; }
  if(!strcmp(s,"GetImmediateContext")){ NB(24); L24:; uint32_t ctx=R32(self+16); if(!ctx){ ctx=newobjn("ID3D11DeviceContext"); W32(self+16,ctx);} W32(A(0),ctx); ret(c,m,0); return; }
  if(!strcmp(s,"Map")){ NB(25); L25:; if(gi<8192) g_nfast[gi]=2; null_map(c,m); return; }
  if(!strcmp(s,"Unmap")){ NB(26); L26:; if(gi<8192) g_nfast[gi]=7; null_unmap(c,m); return; }
  if(!strcmp(s,"GetDesc")){ NB(27); L27:; if(R32(self+12)){ uint32_t n=R32(self+24); if(n) memcpy(MEMP(A(0)),MEMP(R32(self+12)),n); ret(c,m,0); return; } }   /* perf73: the stored-desc test stays per call */
  /* ---- ID3D11DeviceContext state queries: return what was bound (render targets) or empty */
  if(strstr(mn,"DeviceContext")){ LCTX:; inctx=1;
    uint32_t st=R32(self+20); if(!st){ st=gmalloc(64); memset(MEMP(st),0,64); W32(self+20,st); }   /* [0..7] RTVs, [8] DSV */
    if(ctxk) switch(ctxk){ case 101: goto L101; case 102: goto L102; case 103: goto L103; case 104: goto L104; case 105: goto L105; case 106: goto L106; case 107: goto L107; case 108: goto L108; case 109: goto L109; case 110: goto L110; case 111: goto L111; case 112: goto L112; case 113: goto L113; default: break; }   /* perf73: memoized context branch */
    if(!strcmp(s,"OMSetRenderTargets")){ NB(101); L101:; if(gi<8192) g_nfast[gi]=3; null_omsetrt(c,m,self); return; }
    if(!strcmp(s,"OMGetRenderTargets")){ NB(102); L102:; uint32_t n=A(0), pp=A(1), pd=A(2);
      if(pp) for(uint32_t k=0;k<n;k++){ uint32_t v=k<8?R32(st+4*k):0; if(v) W32(v+4,R32(v+4)+1); W32(pp+4*k,v); }
      if(pd){ uint32_t v=R32(st+32); if(v) W32(v+4,R32(v+4)+1); W32(pd,v); } ret(c,m,0); return; }
    if(!strcmp(s,"OMGetRenderTargetsAndUnorderedAccessViews")){ NB(103); L103:; uint32_t n=A(0), pp=A(1), pd=A(2), uo=A(3), un=A(4), pu=A(5);
      if(pp) for(uint32_t k=0;k<n;k++) W32(pp+4*k,0); if(pd) W32(pd,0); if(pu) for(uint32_t k=0;k<un;k++) W32(pu+4*k,0); (void)uo; ret(c,m,0); return; }
    if(strstr(s,"Get") && (strstr(s,"ShaderResources")||strstr(s,"Samplers")||strstr(s,"ConstantBuffers")||strstr(s,"UnorderedAccessViews"))){ NB(104); L104:; uint32_t n=A(1), pp=A(2); if(pp) for(uint32_t k=0;k<n;k++) W32(pp+4*k,0); ret(c,m,0); return; }
    if(strstr(s,"GetShader")){ NB(105); L105:; if(A(0)) W32(A(0),0); if(A(2)) W32(A(2),0); ret(c,m,0); return; }
    if(!strcmp(s,"IAGetInputLayout")||!strcmp(s,"RSGetState")||!strcmp(s,"GetPredication")){ NB(106); L106:; if(A(0)) W32(A(0),0); if(!strcmp(s,"GetPredication") && A(1)) W32(A(1),0); ret(c,m,0); return; }
    if(!strcmp(s,"IAGetVertexBuffers")){ NB(107); L107:; uint32_t n=A(1); for(uint32_t k=0;k<n;k++){ if(A(2)) W32(A(2)+4*k,0); if(A(3)) W32(A(3)+4*k,0); if(A(4)) W32(A(4)+4*k,0);} ret(c,m,0); return; }
    if(!strcmp(s,"IAGetIndexBuffer")){ NB(108); L108:; if(A(0)) W32(A(0),0); if(A(1)) W32(A(1),0); if(A(2)) W32(A(2),0); ret(c,m,0); return; }
    if(!strcmp(s,"IAGetPrimitiveTopology")){ NB(109); L109:; if(A(0)) W32(A(0),0); ret(c,m,0); return; }
    if(!strcmp(s,"OMGetBlendState")){ NB(110); L110:; if(A(0)) W32(A(0),0); if(A(1)) for(int k=0;k<4;k++) W32(A(1)+4*k,0x3f800000u); if(A(2)) W32(A(2),0xffffffffu); ret(c,m,0); return; }
    if(!strcmp(s,"OMGetDepthStencilState")){ NB(111); L111:; if(A(0)) W32(A(0),0); if(A(1)) W32(A(1),0); ret(c,m,0); return; }
    if(!strcmp(s,"RSGetViewports")||!strcmp(s,"RSGetScissorRects")){ NB(112); L112:; if(A(0)) W32(A(0),0); ret(c,m,0); return; }
    if(!strcmp(s,"SOGetTargets")){ NB(113); L113:; for(uint32_t k=0;k<A(0);k++) if(A(1)) W32(A(1)+4*k,0); ret(c,m,0); return; }
  }
  if(!strcmp(s,"GetResource")){ NB(28); L28:; uint32_t r=R32(self+16); if(!r) r=newobjn("ID3D11Texture2D"); else W32(r+4,R32(r+4)+1); W32(A(0),r); ret(c,m,0); return; }
  if(!strcmp(s,"GetData")){ NB(29); L29:; uint32_t pd=A(1), n=A(2);   /* ID3D11DeviceContext::GetData(pAsync, pData, DataSize, Flags) */
    if(pd && n){ memset(MEMP(pd),0,n); struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); uint64_t ns=(uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
      if(n==16) W64(pd,1000000000ull);      /* D3D11_QUERY_DATA_TIMESTAMP_DISJOINT: 1 GHz, not disjoint */
      else if(n==8) W64(pd,ns);             /* timestamp / occlusion count */
      else if(n==4) W32(pd,1); }             /* D3D11_QUERY_EVENT: finished */            /* timestamp / occlusion count */
    ret(c,m,0); return; }   /* queries: "done", zeros */
  if(!strcmp(s,"GetDevice")){ NB(30); L30:; W32(A(0),newobjn("ID3D11Device")); ret(c,m,0); return; }
  /* generic: methods returning an interface via last out-pointer */
  if(m->outiface>=0){ NB(31); L31:; if(gi<8192 && strcmp(s,"GetDesc")) g_nfast[gi]=4; null_create(c,m,gi); return; }
  /* default */
  NB(32); L32:;   /* perf73 */
  if(gi<8192 && strcmp(s,"GetDesc")) g_nfast[gi]=1;   /* GetDesc lands here only when the object has no stored desc */
  ret(c,m,0);
}
/* dynamic exports for LoadLibrary/GetProcAddress */
static const char *dyn_names[]={"CreateDXGIFactory","CreateDXGIFactory1","D3D11CreateDevice","D3D11CreateDeviceAndSwapChain",0};
uint32_t dyn_lookup(const char *n){ for(int k=0;dyn_names[k];k++) if(!strcmp(dyn_names[k],n)) return DYN_BASE+4*k; return 0; }
void dyn_dispatch(CPU*c, uint32_t pc){
  int k=(pc-DYN_BASE)/4; const char *n=dyn_names[k];
  fprintf(stderr,"[d3d] %s\n",n);
  if(bridge_dyn(c,n)) return;
  if(!strcmp(n,"CreateDXGIFactory")||!strcmp(n,"CreateDXGIFactory1")){ int i=find_iid(ARG(0)); if(i<0) i=iface_idx("IDXGIFactory"); W32(ARG(1),newobj(i)); RET_STD(2,0); }
  if(!strcmp(n,"D3D11CreateDevice")){ uint32_t dev=newobjn("ID3D11Device"), ctx=newobjn("ID3D11DeviceContext"); W32(dev+16,ctx);
    if(ARG(7)) W32(ARG(7),dev); if(ARG(8)) W32(ARG(8),0xb000); if(ARG(9)) W32(ARG(9),ctx); RET_STD(10,0); }
  if(!strcmp(n,"D3D11CreateDeviceAndSwapChain")){ uint32_t dev=newobjn("ID3D11Device"), ctx=newobjn("ID3D11DeviceContext"); W32(dev+16,ctx);
    uint32_t sc=newobjn("IDXGISwapChain"); W32(sc+12,copydesc(ARG(7),60)); W32(sc+24,60);
    if(ARG(8)) W32(ARG(8),sc); if(ARG(9)) W32(ARG(9),dev); if(ARG(10)) W32(ARG(10),0xb000); if(ARG(11)) W32(ARG(11),ctx); RET_STD(12,0); }
  RET_STD(0,0);
}
int magic_dispatch(CPU*c, uint32_t pc){
  { int xaudio_dispatch(CPU*,uint32_t); if(pc>=0xE9000000u && pc<0xE9010000u) return xaudio_dispatch(c,pc); }
  if(pc>=COM_BASE && pc<COM_BASE+4u*g_com_nmethods){ com_dispatch(c,pc); return 1; }
  if(pc>=DYN_BASE && pc<DYN_BASE+64){ dyn_dispatch(c,pc); return 1; }
  return 0;
}

/* dinput8.dll!DirectInput8Create(hinst, version, riid, ppvOut, punkOuter) */
HLE(dinput8,DirectInput8Create){ int i=find_iid(ARG(2)); if(i<0) i=iface_idx("IDirectInput8W"); W32(ARG(3),newobj(i)); fprintf(stderr,"[dinput] DirectInput8Create -> null device\n"); RET_STD(5,0); }
