/* RHI M1 natives (notes/RHI_MAP.md §10.5 / §11): host implementations of the D3D-issuing FD3D11DynamicRHI slots that the M0
   census found hot, calling DXVK's ID3D11DeviceContext directly (host vtable calls on the host objects behind the guest COM
   wrappers, br_host()) instead of going guest code -> COM magic -> com_dispatch -> bridge marshalling -> DXVK.
   Installed through linux/rhi_hook.c's copied vtable (BSI_RHI=native; regress/rhi_m1/rhi_m1.patch). Default off.

   Rules every native here follows:
   - Same D3D11 call stream as the original body: same methods, same order, same arguments (guest COM wrappers translated
     with br_hostc() exactly as the generated bridge code does), no filtering. A native never reorders or drops a call,
     except Draw* under BSI_FASTDRAW, which it skips exactly when the bridge would (bridge_skip()).
   - Same guest-visible state: every field of the RHI object, the constant-buffer objects and the out-parameters the
     original writes is written with the same value. Groups that share fields or helpers are hooked whole:
       draw   126 DrawPrimitive, 127 DrawIndexedPrimitive, 129-134 Begin/End/Draw(Indexed)PrimitiveUP and the constant
              commit f_00891c40 + FD3D11ConstantBuffer::Commit f_0088e450 + the dynamic VB/IB helpers f_0088b690 /
              f_0088b5a0 (the commit and the UP helpers have no callers outside this group; checked with a call scan).
              128 is left on its original body: it is `jmp [vtbl+0x1fc]`, i.e. it already lands on native 127.
       ss     71 SetStreamSource
       bss    77 SetBoundShaderState (the BSS reference ring f_0088d2c0 is called as guest code: it calls the
              Add/RemoveResourceRef slots through the vtable, which stay guest)
       tex    34 UnlockTexture2D (TSet find/remove f_00894de0/f_008988e0 and appFree f_00482ae0 stay guest calls; the R/B
              swizzle f_00889a50 and the UpdateSubresource are native, the swizzle with NEON/SSE2)
   - Rare or error paths run the original body instead (call_indirect(orig), before any side effect): unknown primitive
     type or tessellation with a non-list type (appErrorf), a null resource argument, the UP scratch buffer growing past
     its capacity (appMalloc/appFree), an RHI object that is not the hooked one. Creating / releasing the dynamic VB/IB
     (first use, growth past 4 MB) goes through the guest COM wrappers (call_indirect on the wrapper vtable), so the
     bridge's wrap/forget bookkeeping stays exactly as before.
   - Thread: the natives run on the calling thread, as the originals do (the census saw these slots 99.9 % on the RT;
     the immediate context is not thread-safe either way).
   Refused (natives off, census still possible) when: the bridge is off, the RHI's device/context are not bridged,
   BSI_BRIDGE_THREAD is on (queued bridge calls; a direct call would overtake them), BSI_BRFAST has bit 2 (its bind shadow
   would not see the natives' binds), or a debug env that hooks bridge draws is set (BSI_FORCE_FADE, BSI_TRAPCB,
   BSI_LUTLOG). Per-draw frame dumps (BSI_DUMPFRAME / BSI_DUMPAT without BSI_BBONLY) do not see native draws (warning).

   BSI_RHI_SLOTS (default "all"): comma list of group names (draw, ss, bss, tex) and/or slot numbers; a slot number
   enables its whole group. "-" prefix removes (e.g. "all,-tex"). BSI_RHI_STAT=1: "[rhinat]" line every 10 s with calls,
   fallbacks and dynamic-buffer bytes per frame. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#ifndef GP
#define GP(a) ((a)?(void*)MEMP(a):(void*)0)
#endif
typedef void (*rn_fn)(CPU*c);
void call_indirect(CPU*c, uint32_t t);
void *br_host(uint32_t g); void *br_hostc(uint32_t g);
int bridge_skip(const char *name);
extern int g_bridge_on; extern int g_brq_on;

#define RN_GLOBAL 0x13a17dcu    /* GDynamicRHI */
#define RN_SHADER_PLATFORM 0x1330514u   /* == 4: SM5 path in SetBoundShaderState (HS/DS/GS bound) */
#define RN_N 232

/* ID3D11DeviceContext vtable indices (= guest COM vtable offset / 4) */
enum { CX_VSSetConstantBuffers=7, CX_PSSetShader=9, CX_VSSetShader=11, CX_DrawIndexed=12, CX_Draw=13, CX_Map=14, CX_Unmap=15,
       CX_PSSetConstantBuffers=16, CX_IASetInputLayout=17, CX_IASetVertexBuffers=18, CX_IASetIndexBuffer=19,
       CX_DrawIndexedInstanced=20, CX_DrawInstanced=21, CX_GSSetConstantBuffers=22, CX_GSSetShader=23,
       CX_IASetPrimitiveTopology=24, CX_UpdateSubresource=48, CX_HSSetShader=60, CX_HSSetConstantBuffers=62,
       CX_DSSetShader=64, CX_DSSetConstantBuffers=66 };
#define HVT(o,i) ((*(void***)(o))[i])
typedef struct { void *pData; uint32_t RowPitch, DepthPitch; } RnMap;
typedef void (*fn_setcb)(void*,uint32_t,uint32_t,void*const*);
typedef void (*fn_setsh)(void*,void*,void*const*,uint32_t);
typedef void (*fn_upd)(void*,void*,uint32_t,const void*,const void*,uint32_t,uint32_t);

static uint32_t g_rn_orig[RN_N];
static uint32_t g_rn_obj, g_rn_vptr;
static int g_rn_fastdraw;
/* stats (RenderingThread only; plain counters) */
static uint64_t g_rn_calls[RN_N], g_rn_fb[RN_N], g_rn_upbytes, g_rn_cbupd, g_rn_swzpx;
static int g_rn_stat;
/* BSI_RHI_STAT=2: UnlockTexture2D split (swizzle ns + minor page faults taken inside it, UpdateSubresource ns) */
static uint64_t g_rn_swz_ns, g_rn_swz_flt, g_rn_upd_ns;
static inline uint64_t rn_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static inline uint64_t rn_minflt(void){ struct rusage u; getrusage(RUSAGE_THREAD,&u); return (uint64_t)u.ru_minflt; }
/* BSI_RHI_SWZ=scratch: swizzle into a host buffer and upload from there instead of rewriting the guest lock buffer in
   place (the guest buffer is freed right after the upload, so its content is not observable; not the default: the
   default keeps the original's exact memory effects) */
static int g_rn_swz_scratch; static uint8_t *g_rn_scr; static size_t g_rn_scrn;

/* ---- helpers ---- */
static inline void *rn_ctx(uint32_t self){ return br_host(R32(self+0x4c)); }
#define ARGS(k) R32(c->esp+4+4*(k))
static inline void rn_ret(CPU*c, uint32_t argbytes){ c->esp+=4+argbytes; }
/* run the original body: the guest stack is still exactly as the caller left it */
static inline void rn_orig(CPU*c, int s){ g_rn_fb[s]++; call_indirect(c,g_rn_orig[s]); }
/* call guest code from a native: args left to right, fake return address, ecx = this. Works for thiscall/stdcall (callee
   pops) and cdecl (we restore esp). The native's own guest-stack locals must lie above the pushed area. */
static uint32_t rn_gcall(CPU*c, uint32_t fn, uint32_t ecx, int n, const uint32_t *a){
  uint32_t sp=c->esp;
  for(int k=n-1;k>=0;k--) PUSH32(c,a[k]);
  PUSH32(c,0xDEADBEEFu); c->ecx=ecx;
  call_indirect(c,fn);
  c->esp=sp; return c->eax; }
/* topology, f_0088b530 (non-error cases only; the callers check rn_pt_ok first) */
static inline uint32_t rn_topo(uint32_t pt, uint32_t tess){ return tess ? 0x23 : pt==0 ? 4 : pt==1 ? 5 : 2; }
static inline int rn_pt_ok(uint32_t pt, uint32_t tess){ return pt<=2 && (!tess || pt==0); }
static inline uint32_t rn_count(uint32_t pt, uint32_t n){ return pt==0 ? n*3 : pt==1 ? n+2 : n*2; }
static inline int rn_skipdraw(const char *name){ return g_rn_fastdraw && bridge_skip(name); }

/* ---- constant commit: f_0088e450 (FD3D11ConstantBuffer::CommitConstantsToDevice) + f_00891c40 ---- */
static int rn_cb_commit(uint32_t cb, uint32_t disc, uint32_t self, void *sctx){
  if(!R32(cb+0x30)) return 0;
  uint32_t t=R32(cb+0x40);
  if(!disc){ uint32_t tot=R32(cb+0x44); if(t<tot) t=tot; }
  W32(cb+0x44,t);
  uint32_t sz=R32(cb+0x2c)>>1, idx=1;
  if(sz>=t){ uint32_t num=R32(cb+0x3c); while(idx<num){ idx++; sz>>=1; if(sz<t) break; } }
  idx--; W32(cb+0x38,idx);
  uint32_t owner=R32(cb+0x1c); void *ctx=owner==self?sctx:rn_ctx(owner);
  uint32_t n2=sz*2;
  ((fn_upd)HVT(ctx,CX_UpdateSubresource))(ctx,br_hostc(R32(R32(cb+0x20)+4*idx)),0,0,GP(R32(cb+0x34)),n2,n2);
  W32(cb+0x30,0); W32(cb+0x40,0); g_rn_cbupd++;
  return 1; }
static void rn_commit(uint32_t self, void *ctx){
  static const struct { uint16_t arr, vt; } st[5]={{0x3d8,CX_VSSetConstantBuffers},{0x3e4,CX_HSSetConstantBuffers},{0x3f0,CX_DSSetConstantBuffers},
                                                    {0x408,CX_GSSetConstantBuffers},{0x3fc,CX_PSSetConstantBuffers}};
  uint32_t disc=R32(self+0x2ac);
  for(int k=0;k<5;k++){
    if(k==1 && !R32(self+0x2b0)){ k=2; continue; }          /* HS + DS only with tessellation */
    for(uint32_t i=0;i<6;i++){
      uint32_t cb=R32(R32(self+st[k].arr)+4*i); if(!cb) continue;
      if(!rn_cb_commit(cb,disc,self,ctx)) continue;
      void *b=br_hostc(R32(R32(cb+0x20)+4*R32(cb+0x38)));
      ((fn_setcb)HVT(ctx,st[k].vt))(ctx,i,1,&b); } }
  W32(self+0x2ac,0); }

/* ---- dynamic VB/IB for the UP draws: f_0088b5a0 (buffer) + f_0088b690 (upload) ---- */
typedef struct { uint32_t g; void *h; uint32_t width; } RnBufw;
static RnBufw g_rn_bw[2];
/* ByteWidth of a bridged buffer (immutable per D3D object: cached by guest wrapper + host object) */
static uint32_t rn_bufwidth(uint32_t g, void *h, int k){
  if(g_rn_bw[k].g==g && g_rn_bw[k].h==h) return g_rn_bw[k].width;
  uint32_t d[6]={0}; ((void(*)(void*,uint32_t*))HVT(h,10))(h,d);   /* ID3D11Buffer::GetDesc */
  g_rn_bw[k].g=g; g_rn_bw[k].h=h; g_rn_bw[k].width=d[0]; return d[0]; }
/* guest COM call on a wrapper (rare paths: Release, GetDesc of a non-bridged wrapper, CreateBuffer) */
static uint32_t rn_com(CPU*c, uint32_t obj, int m, int n, const uint32_t *a){
  uint32_t aa[8]; aa[0]=obj; for(int k=0;k<n;k++) aa[1+k]=a[k];
  return rn_gcall(c,R32(R32(obj)+4*m),c->ecx,n+1,aa); }
static uint32_t rn_up_buffer(CPU*c, uint32_t self, uint32_t count, uint32_t stride, uint32_t bind){
  int vb=bind&1;
  uint32_t arr=self+(vb?0x2b4:0x2b8), ip=self+(vb?0x2bc:0x2c0);
  uint32_t need=count*stride;
  uint32_t slot=arr+4*R32(ip);
  uint32_t g=R32(slot);
  if(g){
    void *h=br_host(g); uint32_t w;
    if(h) w=rn_bufwidth(g,h,!vb);
    else { uint32_t sp=c->esp; c->esp-=32; uint32_t d=c->esp; c->esp-=16; uint32_t a[1]={d}; rn_com(c,g,10,1,a); w=R32(d); c->esp=sp; }
    if(w>=need) goto ret;
    slot=arr+4*R32(ip); uint32_t o=R32(slot); W32(slot,0); if(o) rn_com(c,o,2,0,0); }
  { /* CreateBuffer(desc, NULL, &slot) through the guest device wrapper (the bridge wraps the new DXVK buffer) */
    uint32_t sp=c->esp; c->esp-=32; uint32_t d=c->esp; c->esp-=16;
    W32(d,need<0x400000u?0x400000u:need); W32(d+4,2); W32(d+8,bind); W32(d+12,0x10000); W32(d+16,0); W32(d+20,0);
    slot=arr+4*R32(ip); uint32_t o=R32(slot); W32(slot,0); if(o) rn_com(c,o,2,0,0);
    uint32_t a[3]={d,0,slot}; rn_com(c,R32(self+0x48),3,3,a);
    c->esp=sp; }
ret:
  { uint32_t i=R32(ip), b=R32(arr+4*i); i++; W32(ip,i>=1?0:i); return b; } }
static uint32_t rn_up_upload(CPU*c, uint32_t self, void *ctx, uint32_t count, uint32_t stride, uint32_t data, uint32_t bind, uint32_t *off){
  uint32_t cur=self+((bind&1)?0x2c4:0x2c8);
  uint32_t size=count*stride, v=(R32(cur)+3)&~3u, mt=5;
  W32(cur,v);
  uint32_t end=v+size;
  if(end>=0x400000u){ W32(cur,0); end=size; mt=4; }
  *off=R32(cur); W32(cur,end);
  uint32_t buf=rn_up_buffer(c,self,count,stride,bind);
  void *h=br_hostc(buf); RnMap m={0};
  int32_t hr=((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,RnMap*))HVT(ctx,CX_Map))(ctx,h,0,mt,0,&m);
  if(hr>=0 && m.pData) memcpy((uint8_t*)m.pData+*off,MEMP(data),size);
  else { static int nw; if(nw++<10) fprintf(stderr,"[rhinat] dynamic %s Map failed (%08x): upload of %u bytes dropped\n",(bind&1)?"VB":"IB",(uint32_t)hr,size); }
  ((void(*)(void*,void*,uint32_t))HVT(ctx,CX_Unmap))(ctx,h,0);
  g_rn_upbytes+=size;
  return buf; }

/* ---- draw group ---- */
/* 134 DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, NumPrimitives, IndexData, IndexDataStride,
   VertexData, VertexDataStride), ret 0x20 */
static void rn_134(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0); void *ctx=rn_ctx(self);
  if(!ctx || !rn_pt_ok(pt,R32(self+0x2b0))){ rn_orig(c,134); return; }
  g_rn_calls[134]++;
  uint32_t minv=ARGS(1), nverts=ARGS(2), nprims=ARGS(3), idata=ARGS(4), istride=ARGS(5), vdata=ARGS(6), vstride=ARGS(7);
  uint32_t nidx=rn_count(pt,nprims), voff, ioff;
  uint32_t vb=rn_up_upload(c,self,ctx,nverts,vstride,vdata,1,&voff);
  uint32_t ib=rn_up_upload(c,self,ctx,nidx,istride,idata,2,&ioff);
  rn_commit(self,ctx);
  { void *h=br_hostc(vb); ((void(*)(void*,uint32_t,uint32_t,void*const*,const uint32_t*,const uint32_t*))HVT(ctx,CX_IASetVertexBuffers))(ctx,0,1,&h,&vstride,&voff); }
  ((void(*)(void*,void*,uint32_t,uint32_t))HVT(ctx,CX_IASetIndexBuffer))(ctx,br_hostc(ib),istride!=2?42:57,ioff);
  ((void(*)(void*,uint32_t))HVT(ctx,CX_IASetPrimitiveTopology))(ctx,rn_topo(pt,R32(self+0x2b0)));
  if(!rn_skipdraw("ID3D11DeviceContext::DrawIndexed")) ((void(*)(void*,uint32_t,uint32_t,int32_t))HVT(ctx,CX_DrawIndexed))(ctx,nidx,0,(int32_t)minv);
  rn_ret(c,0x20); }
/* 131 DrawPrimitiveUP(PrimitiveType, NumPrimitives, VertexData, VertexDataStride), ret 0x10 */
static void rn_131(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0); void *ctx=rn_ctx(self);
  if(!ctx || !rn_pt_ok(pt,R32(self+0x2b0))){ rn_orig(c,131); return; }
  g_rn_calls[131]++;
  uint32_t nprims=ARGS(1), vdata=ARGS(2), vstride=ARGS(3);
  uint32_t nv=rn_count(pt,nprims), voff;
  uint32_t vb=rn_up_upload(c,self,ctx,nv,vstride,vdata,1,&voff);
  rn_commit(self,ctx);
  { void *h=br_hostc(vb); ((void(*)(void*,uint32_t,uint32_t,void*const*,const uint32_t*,const uint32_t*))HVT(ctx,CX_IASetVertexBuffers))(ctx,0,1,&h,&vstride,&voff); }
  ((void(*)(void*,uint32_t))HVT(ctx,CX_IASetPrimitiveTopology))(ctx,rn_topo(pt,R32(self+0x2b0)));
  if(!rn_skipdraw("ID3D11DeviceContext::Draw")) ((void(*)(void*,uint32_t,uint32_t))HVT(ctx,CX_Draw))(ctx,nv,0);
  rn_ret(c,0x10); }
/* 132 BeginDrawIndexedPrimitiveUP(PrimitiveType, NumPrimitives, NumVertices, VertexDataStride, OutVertexData,
   MinVertexIndex, NumIndices, IndexDataStride, OutIndexData), ret 0x24 */
static void rn_132(CPU*c){
  uint32_t self=c->ecx;
  uint32_t nv=ARGS(2), vs=ARGS(3), ni=ARGS(6), is=ARGS(7);
  uint32_t vbytes=nv*vs, total=ni*is+vbytes;
  if(total>R32(self+0x28c)){ rn_orig(c,132); return; }        /* scratch grows: appFree/appMalloc in the original */
  g_rn_calls[132]++;
  uint32_t buf=R32(self+0x288);
  W32(self+0x27c,buf); W32(ARGS(4),buf);
  W32(self+0x290,R32(self+0x27c)+vbytes); W32(ARGS(8),R32(self+0x290));
  W32(self+0x294,ARGS(0)); W32(self+0x298,ARGS(1)); W32(self+0x280,nv); W32(self+0x29c,ARGS(5)); W32(self+0x2a0,is); W32(self+0x284,vs);
  rn_ret(c,0x24); }
/* 129 BeginDrawPrimitiveUP(PrimitiveType, NumPrimitives, NumVertices, VertexDataStride, OutVertexData), ret 0x14 */
static void rn_129(CPU*c){
  uint32_t self=c->ecx;
  uint32_t nv=ARGS(2), vs=ARGS(3);
  if(nv*vs>R32(self+0x28c)){ rn_orig(c,129); return; }
  g_rn_calls[129]++;
  uint32_t buf=R32(self+0x288);
  W32(self+0x27c,buf); W32(ARGS(4),buf);
  W32(self+0x280,nv); W32(self+0x284,vs); W32(self+0x294,ARGS(0)); W32(self+0x298,ARGS(1));
  rn_ret(c,0x14); }
/* 133 EndDrawIndexedPrimitiveUP(): commit, then GDynamicRHI->DrawIndexedPrimitiveUP(saved args) through the vtable (native
   134 or whatever the table holds), then clear the two data pointers */
static void rn_133(CPU*c){
  uint32_t self=c->ecx; void *ctx=rn_ctx(self);
  if(!ctx){ rn_orig(c,133); return; }
  g_rn_calls[133]++;
  rn_commit(self,ctx);
  uint32_t g=R32(RN_GLOBAL);
  uint32_t a[8]={R32(self+0x294),R32(self+0x29c),R32(self+0x280),R32(self+0x298),R32(self+0x290),R32(self+0x2a0),R32(self+0x27c),R32(self+0x284)};
  rn_gcall(c,R32(R32(g)+0x218),g,8,a);
  W32(self+0x290,0); W32(self+0x27c,0);
  rn_ret(c,0); }
/* 130 EndDrawPrimitiveUP(): commit, GDynamicRHI->DrawPrimitiveUP(saved args), clear the data pointer */
static void rn_130(CPU*c){
  uint32_t self=c->ecx; void *ctx=rn_ctx(self);
  if(!ctx){ rn_orig(c,130); return; }
  g_rn_calls[130]++;
  rn_commit(self,ctx);
  uint32_t g=R32(RN_GLOBAL);
  uint32_t a[4]={R32(self+0x294),R32(self+0x298),R32(self+0x27c),R32(self+0x284)};
  rn_gcall(c,R32(R32(g)+0x20c),g,4,a);
  W32(self+0x27c,0);
  rn_ret(c,0); }
/* 127 DrawIndexedPrimitive(IndexBuffer, PrimitiveType, BaseVertexIndex, MinIndex, NumVertices, StartIndex, NumPrimitives,
   +2 unused), ret 0x24. IndexBuffer handle - 8 = FD3D11IndexBuffer: +8 ID3D11Buffer wrapper, +0xc stride */
static void rn_127(CPU*c){
  uint32_t self=c->ecx, ibh=ARGS(0), pt=ARGS(1); void *ctx=rn_ctx(self);
  if(!ctx || !ibh || !rn_pt_ok(pt,R32(self+0x2b0))){ rn_orig(c,127); return; }
  g_rn_calls[127]++;
  uint32_t ib=ibh-8, base=ARGS(2), start=ARGS(5), nprims=ARGS(6);
  rn_commit(self,ctx);
  uint32_t fmt=R32(ib+0xc)!=2?42:57, n=rn_count(pt,nprims);
  ((void(*)(void*,void*,uint32_t,uint32_t))HVT(ctx,CX_IASetIndexBuffer))(ctx,br_hostc(R32(ib+8)),fmt,0);
  ((void(*)(void*,uint32_t))HVT(ctx,CX_IASetPrimitiveTopology))(ctx,rn_topo(pt,R32(self+0x2b0)));
  uint32_t inst=R32(self+0xc8);
  if(inst>1){ if(!rn_skipdraw("ID3D11DeviceContext::DrawIndexedInstanced"))
      ((void(*)(void*,uint32_t,uint32_t,uint32_t,int32_t,uint32_t))HVT(ctx,CX_DrawIndexedInstanced))(ctx,n,inst,start,(int32_t)base,0); }
  else if(!rn_skipdraw("ID3D11DeviceContext::DrawIndexed")) ((void(*)(void*,uint32_t,uint32_t,int32_t))HVT(ctx,CX_DrawIndexed))(ctx,n,start,(int32_t)base);
  rn_ret(c,0x24); }
/* 126 DrawPrimitive(PrimitiveType, BaseVertexIndex, NumPrimitives), ret 0xc */
static void rn_126(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0); void *ctx=rn_ctx(self);
  if(!ctx || !rn_pt_ok(pt,R32(self+0x2b0))){ rn_orig(c,126); return; }
  g_rn_calls[126]++;
  uint32_t base=ARGS(1), nprims=ARGS(2);
  rn_commit(self,ctx);
  uint32_t n=rn_count(pt,nprims);
  ((void(*)(void*,uint32_t))HVT(ctx,CX_IASetPrimitiveTopology))(ctx,rn_topo(pt,R32(self+0x2b0)));
  uint32_t inst=R32(self+0xc8);
  if(inst>1){ if(!rn_skipdraw("ID3D11DeviceContext::DrawInstanced")) ((void(*)(void*,uint32_t,uint32_t,uint32_t,uint32_t))HVT(ctx,CX_DrawInstanced))(ctx,n,inst,base,0); }
  else if(!rn_skipdraw("ID3D11DeviceContext::Draw")) ((void(*)(void*,uint32_t,uint32_t))HVT(ctx,CX_Draw))(ctx,n,base);
  rn_ret(c,0xc); }

/* ---- 71 SetStreamSource(StreamIndex, VertexBuffer, Stride, ?, ?, NumInstances), ret 0x18. The VB handle - 4 is the
   ID3D11Buffer wrapper itself (TD3D11Resource: D3D object + empty RHI base at +4); offset is always 0 ---- */
static void rn_71(CPU*c){
  uint32_t self=c->ecx; void *ctx=rn_ctx(self);
  if(!ctx){ rn_orig(c,71); return; }
  g_rn_calls[71]++;
  uint32_t vbh=ARGS(1), stride=ARGS(2), off=0;
  void *h=br_hostc(vbh?vbh-4:0);
  ((void(*)(void*,uint32_t,uint32_t,void*const*,const uint32_t*,const uint32_t*))HVT(ctx,CX_IASetVertexBuffers))(ctx,ARGS(0),1,&h,&stride,&off);
  W32(self+0xc8,ARGS(5));
  rn_ret(c,0x18); }

/* ---- 77 SetBoundShaderState(BSS), ret 4. BSS handle - 8: +0x34 input layout, +0x38 VS, +0x3c PS, +0x40 HS, +0x44 DS,
   +0x48 GS (guest COM wrappers) ---- */
static void rn_77(CPU*c){
  uint32_t self=c->ecx, h=ARGS(0); void *ctx=rn_ctx(self);
  if(!ctx || !h){ rn_orig(c,77); return; }
  g_rn_calls[77]++;
  uint32_t b=h-8;
  ((void(*)(void*,void*))HVT(ctx,CX_IASetInputLayout))(ctx,br_hostc(R32(b+0x34)));
  ((fn_setsh)HVT(ctx,CX_VSSetShader))(ctx,br_hostc(R32(b+0x38)),0,0);
  ((fn_setsh)HVT(ctx,CX_PSSetShader))(ctx,br_hostc(R32(b+0x3c)),0,0);
  if(R32(RN_SHADER_PLATFORM)==4){
    ((fn_setsh)HVT(ctx,CX_HSSetShader))(ctx,br_hostc(R32(b+0x40)),0,0);
    ((fn_setsh)HVT(ctx,CX_DSSetShader))(ctx,br_hostc(R32(b+0x44)),0,0);
    ((fn_setsh)HVT(ctx,CX_GSSetShader))(ctx,br_hostc(R32(b+0x48)),0,0);
    W32(self+0x2b0,(R32(b+0x40)&&R32(b+0x44))?1:0); }
  else W32(self+0x2b0,0);
  W32(self+0x2ac,1);
  { uint32_t a[1]={b+8}; rn_gcall(c,0x88d2c0u,self+0x420,1,a); }   /* TDynamicRHIResourceReference<BSS>::operator= (ring of 1024) */
  rn_ret(c,4); }

/* ---- 34 UnlockTexture2D(Texture, MipIndex, bLockWithinMiptail), ret 0xc -> TD3D11Texture2D::Unlock(Mip, ArrayIndex 0)
   f_00899aa0. Texture handle - 8: +0x5c SizeX, +0x60 SizeY, +0x68 NumMips, +0x6c format class (2 = R/B swizzle),
   +0x70 owning RHI, +0x74 ID3D11Texture2D wrapper. Lock map: TSet at RHI+4, element 0x24 bytes, locked data at +8
   {staging resource, data, pitch}. ---- */
/* f_00889a50: swap bytes 0 and 2 of n 32-bit pixels in place (both of the original's paths compute exactly this) */
static void rn_swz(uint8_t *p, uint32_t n){
  uint32_t i=0;
#if defined(__aarch64__)
  static const uint8_t ix[16]={2,1,0,3,6,5,4,7,10,9,8,11,14,13,12,15};
  uint8x16_t t=vld1q_u8(ix);
  for(;i+16<=n;i+=16){ uint8_t *q=p+4*i;
    uint8x16_t a=vld1q_u8(q), b=vld1q_u8(q+16), d=vld1q_u8(q+32), e=vld1q_u8(q+48);
    vst1q_u8(q,vqtbl1q_u8(a,t)); vst1q_u8(q+16,vqtbl1q_u8(b,t)); vst1q_u8(q+32,vqtbl1q_u8(d,t)); vst1q_u8(q+48,vqtbl1q_u8(e,t)); }
  for(;i+4<=n;i+=4){ uint8_t *q=p+4*i; vst1q_u8(q,vqtbl1q_u8(vld1q_u8(q),t)); }
#elif defined(__SSE2__)
  const __m128i ag=_mm_set1_epi32((int)0xff00ff00u), lo=_mm_set1_epi32(0xff);
  for(;i+8<=n;i+=8){ __m128i *q=(__m128i*)(p+4*i);
    __m128i a=_mm_loadu_si128(q), b=_mm_loadu_si128(q+1);
    a=_mm_or_si128(_mm_and_si128(a,ag),_mm_or_si128(_mm_and_si128(_mm_srli_epi32(a,16),lo),_mm_slli_epi32(_mm_and_si128(a,lo),16)));
    b=_mm_or_si128(_mm_and_si128(b,ag),_mm_or_si128(_mm_and_si128(_mm_srli_epi32(b,16),lo),_mm_slli_epi32(_mm_and_si128(b,lo),16)));
    _mm_storeu_si128(q,a); _mm_storeu_si128(q+1,b); }
#endif
  for(;i<n;i++){ uint32_t x; memcpy(&x,p+4*i,4); x=(x&0xff00ff00u)|((x>>16)&0xffu)|((x&0xffu)<<16); memcpy(p+4*i,&x,4); } }
/* swizzle n pixels from s to d (d != s) */
static void rn_swz2(uint8_t *d, const uint8_t *s, uint32_t n){
  uint32_t i=0;
#if defined(__aarch64__)
  static const uint8_t ix[16]={2,1,0,3,6,5,4,7,10,9,8,11,14,13,12,15};
  uint8x16_t t=vld1q_u8(ix);
  for(;i+16<=n;i+=16){ const uint8_t *q=s+4*i; uint8_t *o=d+4*i;
    uint8x16_t a=vld1q_u8(q), b=vld1q_u8(q+16), e=vld1q_u8(q+32), f=vld1q_u8(q+48);
    vst1q_u8(o,vqtbl1q_u8(a,t)); vst1q_u8(o+16,vqtbl1q_u8(b,t)); vst1q_u8(o+32,vqtbl1q_u8(e,t)); vst1q_u8(o+48,vqtbl1q_u8(f,t)); }
  for(;i+4<=n;i+=4) vst1q_u8(d+4*i,vqtbl1q_u8(vld1q_u8(s+4*i),t));
#elif defined(__SSE2__)
  const __m128i ag=_mm_set1_epi32((int)0xff00ff00u), lo=_mm_set1_epi32(0xff);
  for(;i+4<=n;i+=4){ __m128i a=_mm_loadu_si128((const __m128i*)(s+4*i));
    a=_mm_or_si128(_mm_and_si128(a,ag),_mm_or_si128(_mm_and_si128(_mm_srli_epi32(a,16),lo),_mm_slli_epi32(_mm_and_si128(a,lo),16)));
    _mm_storeu_si128((__m128i*)(d+4*i),a); }
#endif
  for(;i<n;i++){ uint32_t x; memcpy(&x,s+4*i,4); x=(x&0xff00ff00u)|((x>>16)&0xffu)|((x&0xffu)<<16); memcpy(d+4*i,&x,4); } }
static void rn_34(CPU*c){
  uint32_t h=ARGS(0);
  if(!h){ rn_orig(c,34); return; }
  uint32_t t=h-8, mip=ARGS(1), rhi=R32(t+0x70);
  void *ctx=rn_ctx(rhi);
  if(!ctx){ rn_orig(c,34); return; }
  uint32_t sub=mip;                                         /* + ArrayIndex(0) * NumMips */
  uint32_t sp=c->esp; c->esp-=32; uint32_t key=c->esp, out=c->esp+8; c->esp-=16;
  W32(key,R32(t+0x74)); W32(key+4,sub);
  { uint32_t a[2]={out,key}; rn_gcall(c,0x894de0u,rhi+4,2,a); }   /* TSet::FindId(&out, &key) */
  int32_t id=(int32_t)R32(out);
  uint32_t ld = id==-1 ? 0 : R32(rhi+4)+(uint32_t)id*0x24;
  if(!ld){ c->esp=sp; rn_orig(c,34); return; }              /* not locked: the original faults; let it */
  g_rn_calls[34]++;
  ld+=8;
  if(!R32(ld)){
    const void *src=GP(R32(ld+4));
    if(R8(t+0x6c)==2){
      uint32_t w=R32(t+0x5c)>>(mip&31), hh=R32(t+0x60)>>(mip&31); if(w<1) w=1; if(hh<1) hh=1;
      uint64_t t0=0, f0=0; if(g_rn_stat>=2){ t0=rn_ns(); f0=rn_minflt(); }
      uint32_t pitch=R32(ld+8);
      if(g_rn_swz_scratch && (size_t)pitch*hh<=(64u<<20)){
        size_t need=(size_t)pitch*hh; if(need>g_rn_scrn){ free(g_rn_scr); g_rn_scr=malloc(need); g_rn_scrn=g_rn_scr?need:0; }
        if(g_rn_scr){ uint32_t row=w*4u<pitch?w*4u:pitch;   /* rows: swizzled texels + the padding bytes as they are */
          for(uint32_t y=0;y<hh;y++){ const uint8_t *q=MEMP(pitch*y+R32(ld+4)); uint8_t *o=g_rn_scr+(size_t)pitch*y;
            rn_swz2(o,q,row/4); if(pitch>row) memcpy(o+row,q+row,pitch-row); }
          src=g_rn_scr; }
        else for(uint32_t y=0;y<hh;y++) rn_swz(MEMP(pitch*y+R32(ld+4)),w); }
      else for(uint32_t y=0;y<hh;y++) rn_swz(MEMP(pitch*y+R32(ld+4)),w);
      if(g_rn_stat>=2){ g_rn_swz_ns+=rn_ns()-t0; g_rn_swz_flt+=rn_minflt()-f0; }
      g_rn_swzpx+=(uint64_t)w*hh; }
    { uint64_t t0=g_rn_stat>=2?rn_ns():0;
      ((fn_upd)HVT(ctx,CX_UpdateSubresource))(ctx,br_hostc(R32(t+0x74)),sub,0,src,R32(ld+8),0);
      if(g_rn_stat>=2) g_rn_upd_ns+=rn_ns()-t0; }
    { uint32_t a[1]={R32(ld+4)}; rn_gcall(c,0x482ae0u,c->ecx,1,a); } }   /* appFree(data) */
  { uint32_t a[1]={key}; rn_gcall(c,0x8988e0u,rhi+4,1,a); }  /* TSet::Remove(&key) */
  c->esp=sp;
  rn_ret(c,0xc); }

/* ---- setup / install / stats (called by rhi_hook.c) ---- */
typedef struct { const char *name; const int *slots; } RnGroup;
static const int rn_g_draw[]={126,127,129,130,131,132,133,134,-1}, rn_g_ss[]={71,-1}, rn_g_bss[]={77,-1}, rn_g_tex[]={34,-1};
static const RnGroup rn_groups[]={{"draw",rn_g_draw},{"ss",rn_g_ss},{"bss",rn_g_bss},{"tex",rn_g_tex}};
#define RN_NG ((int)(sizeof rn_groups/sizeof *rn_groups))
static rn_fn rn_impl(int s){
  switch(s){ case 126: return rn_126; case 127: return rn_127; case 129: return rn_129; case 130: return rn_130; case 131: return rn_131;
    case 132: return rn_132; case 133: return rn_133; case 134: return rn_134; case 71: return rn_71; case 77: return rn_77; case 34: return rn_34; }
  return 0; }
static int rn_group_of(int s){ for(int g=0;g<RN_NG;g++) for(const int *p=rn_groups[g].slots;*p>=0;p++) if(*p==s) return g; return -1; }
/* "all", group names, slot numbers (-> their group), "-x" removes */
static unsigned rn_parse_slots(const char *e){
  unsigned m=0; if(!e || !*e) e="all";
  char b[256]; snprintf(b,sizeof b,"%s",e);
  for(char *sv=0,*t=strtok_r(b,", ",&sv);t;t=strtok_r(0,", ",&sv)){
    int neg=*t=='-'; if(neg) t++;
    unsigned v=0;
    if(!strcmp(t,"all")) v=(1u<<RN_NG)-1;
    else { for(int g=0;g<RN_NG;g++) if(!strcmp(t,rn_groups[g].name)) v|=1u<<g;
      if(!v && *t>='0' && *t<='9'){ int g=rn_group_of(atoi(t)); if(g>=0) v=1u<<g; else fprintf(stderr,"[rhinat] BSI_RHI_SLOTS: slot %s has no native\n",t); } }
    if(neg) m&=~v; else m|=v; }
  return m; }
int rhi_native_setup(uint32_t obj, const uint32_t *orig, rn_fn *nat){
  const char *why=0;
  if(!g_bridge_on) why="the D3D11 bridge is off";
  else if(!br_host(R32(obj+0x4c)) || !br_host(R32(obj+0x48))) why="the RHI's device/context are not bridged DXVK objects";
  else if(g_brq_on) why="BSI_BRIDGE_THREAD is on (queued bridge calls)";
  else if(getenv("BSI_BRFAST") && (strtol(getenv("BSI_BRFAST"),0,0)&2)) why="BSI_BRFAST bit 2 (bind shadow) is on";
  else if(getenv("BSI_FORCE_FADE")||getenv("BSI_TRAPCB")||getenv("BSI_LUTLOG")) why="a bridge draw debug hook is set (BSI_FORCE_FADE/TRAPCB/LUTLOG)";
  if(why){ fprintf(stderr,"[rhinat] natives refused: %s\n",why); return 0; }
  for(int s=0;s<RN_N;s++) g_rn_orig[s]=orig[s];
  unsigned m=rn_parse_slots(getenv("BSI_RHI_SLOTS"));
  int n=0; char list[256]; int lp=0; list[0]=0;
  for(int g=0;g<RN_NG;g++) if(m&(1u<<g)){
    lp+=snprintf(list+lp,sizeof list-lp,"%s%s(",lp?" ":"",rn_groups[g].name);
    for(const int *p=rn_groups[g].slots;*p>=0;p++){ nat[*p]=rn_impl(*p); n++; lp+=snprintf(list+lp,sizeof list-lp,"%s%d",p==rn_groups[g].slots?"":",",*p); }
    lp+=snprintf(list+lp,sizeof list-lp,")"); }
  g_rn_fastdraw=getenv("BSI_FASTDRAW")!=0;
  g_rn_stat=getenv("BSI_RHI_STAT")?atoi(getenv("BSI_RHI_STAT")):0;
  g_rn_swz_scratch=getenv("BSI_RHI_SWZ") && !strcmp(getenv("BSI_RHI_SWZ"),"scratch");
  fprintf(stderr,"[rhinat] %d native slots: %s%s\n",n,list,g_rn_fastdraw?" (BSI_FASTDRAW: native draws skipped like bridged ones)":"");
  if((getenv("BSI_DUMPFRAME")||getenv("BSI_DUMPAT")) && !getenv("BSI_BBONLY") && (m&1))
    fprintf(stderr,"[rhinat] note: per-draw frame dumps (BSI_DUMPFRAME/BSI_DUMPAT without BSI_BBONLY) do not see native draws\n");
  return n; }
void rhi_native_installed(uint32_t obj, uint32_t vptr){ g_rn_obj=obj; g_rn_vptr=vptr; g_rn_bw[0].g=g_rn_bw[1].g=0; }
void rhi_native_tick(double now){
  if(!g_rn_stat) return;
  static double t0; static long fr; static uint64_t pc[RN_N], pf[RN_N], pup, pcb, psw, psn, psf, pun;
  fr++; if(t0==0){ t0=now; fr=0; return; } if(now-t0<10) return;
  double f=fr>0?(double)fr:1; char b[1024]; int p=0;
  for(int s=0;s<RN_N;s++){ uint64_t k=g_rn_calls[s]-pc[s], x=g_rn_fb[s]-pf[s]; if(k||x) p+=snprintf(b+p,sizeof b-p," %d:%.1f",s,k/f); if(x) p+=snprintf(b+p,sizeof b-p,"(fb %.2f)",x/f); pc[s]=g_rn_calls[s]; pf[s]=g_rn_fb[s]; if(p>900) break; }
  fprintf(stderr,"[rhinat] %ld frames, calls/frame%s | UP upload %.1f KB/f, cb updates %.1f/f, swizzled %.1f Kpx/f",fr,b,
    (g_rn_upbytes-pup)/1024.0/f,(g_rn_cbupd-pcb)/f,(g_rn_swzpx-psw)/1000.0/f);
  if(g_rn_stat>=2) fprintf(stderr," | UnlockTexture2D: swizzle %.1f us/f (%s, %.1f minor faults/f), UpdateSubresource %.1f us/f",
    (g_rn_swz_ns-psn)/1e3/f,g_rn_swz_scratch?"scratch":"in place",(g_rn_swz_flt-psf)/f,(g_rn_upd_ns-pun)/1e3/f);
  fprintf(stderr,"\n");
  pup=g_rn_upbytes; pcb=g_rn_cbupd; psw=g_rn_swzpx; psn=g_rn_swz_ns; psf=g_rn_swz_flt; pun=g_rn_upd_ns; t0=now; fr=0; }
