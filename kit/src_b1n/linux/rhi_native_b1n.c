/* rhi_native_b1n.c -- RHI M1 natives for the b1n native renderer (BSI_RHI=b1n + BSI_RHI_NAT=1; default off).

   linux/rhi_native.c (M1) implements the hot FD3D11DynamicRHI slots in host code, but issues the D3D11 calls on the
   bridge's host DXVK objects (br_host / br_hostc + host vtable), which do not exist under BSI_RHI=b1n: there the guest's
   D3D11 COM calls land on the null device's magic vtables -> com_dispatch -> rhi_b1_com -> b1dx_c_* (rhi_b1_com.c /
   rhi_b1_dxvk.cpp). This file is the M1 natives with every host vtable call replaced by the same method call on the same
   guest COM proxy the translated body uses (the proxy pointers as the natives read them from the engine's structures,
   not resolved here: the b1 layer resolves them with co_get/ids_of exactly as it does for the translated bodies), so the
   b1dx_c_* stream, its arguments and every guest-visible side effect are those of the translated body, minus the cost of
   the translated body itself (f_0089c0d0 DrawIndexedPrimitive, f_00891c40 constant commit, f_0088e450
   FD3D11ConstantBuffer::Commit, f_0088b690 / f_0088b5a0 dynamic VB/IB, f_0089b7b0 SetBoundShaderState etc.).

   One COM call = rb_com(): push the args, this and a fake return address on the guest stack as the body's `call [vtbl+4*m]`
   does, then b1com_dispatch (rhi_b1_com.c: the com_dispatch work for the plain void context methods, no call_indirect),
   or call_indirect on the proxy's vtable entry for everything else (Map, Unmap, GetDesc, Release, CreateBuffer, any
   non-COM vtable entry, any com.c debug switch), which is bit for bit the body's own path (com.c com_dispatch +
   rhi_b1_com_post, hitch logger, Map bookkeeping / g_map_inflight).

   Slots (BSI_RHI_SLOTS as in rhi_native.c; default "all" = these three groups; "tex" is accepted and ignored with a note):
     draw  126 DrawPrimitive, 127 DrawIndexedPrimitive, 129-134 Begin/End/Draw(Indexed)PrimitiveUP + the constant commit
           and the dynamic VB/IB helpers (128 stays: `jmp [vtbl+0x1fc]` lands on native 127)
     ss    71 SetStreamSource
     bss   77 SetBoundShaderState (the BSS reference ring f_0088d2c0 and the UP re-entry through GDynamicRHI's vtable
           stay guest calls, as in M1)
   34 UnlockTexture2D (M1 "tex") is NOT ported: out of scope here; it stays on its original body.
   Fallbacks (the original body runs, before any side effect): unknown primitive type / tessellation with a non-list type,
   null IndexBuffer / BSS argument, UP scratch growing past its capacity, an RHI object without a context proxy.
   Refused at setup (b1n runs as before): the b1n COM seam is not active (BSI_RHI without b1n, BSI_B1_COM=0, or the b1
   device did not come up), or the D3D11 bridge is on.
   BSI_RHI_STAT=1: one "[rhinat]" line every 10 s (calls and fallbacks per slot per frame, UP upload bytes, cb updates,
   COM calls issued by the natives: fast = b1com_dispatch, slow = call_indirect). */
#define _GNU_SOURCE
#include "hle.h"
#include "com.h"
#include <stdint.h>
#include <time.h>

typedef void (*rb_fn)(CPU*c);
void call_indirect(CPU*c, uint32_t t);
int b1com_native_ok(void);          /* rhi_b1_com.c: BSI_RHI=b1n and the b1 device is up */
int b1com_dispatch(CPU *c, int gi);  /* rhi_b1_com.c: complete one plain context method as com_dispatch would; 0 = use call_indirect */
extern int g_bridge_on;

#define RB_GLOBAL 0x13a17dcu            /* GDynamicRHI */
#define RB_SHADER_PLATFORM 0x1330514u   /* == 4: SM5 path in SetBoundShaderState (HS/DS/GS bound) */
#define RB_N 232
#define RB_COM_BASE 0xF0000000u         /* com.c COM_BASE: guest COM vtable entry = COM_BASE + 4 * global method index */

/* ID3D11DeviceContext vtable indices (= guest COM vtable offset / 4; com_tab.c ID3D11DeviceContext::* idx) */
enum { CX_VSSetConstantBuffers=7, CX_PSSetShader=9, CX_VSSetShader=11, CX_DrawIndexed=12, CX_Draw=13, CX_Map=14, CX_Unmap=15,
       CX_PSSetConstantBuffers=16, CX_IASetInputLayout=17, CX_IASetVertexBuffers=18, CX_IASetIndexBuffer=19,
       CX_DrawIndexedInstanced=20, CX_DrawInstanced=21, CX_GSSetConstantBuffers=22, CX_GSSetShader=23,
       CX_IASetPrimitiveTopology=24, CX_UpdateSubresource=48, CX_HSSetShader=60, CX_HSSetConstantBuffers=62,
       CX_DSSetShader=64, CX_DSSetConstantBuffers=66 };
enum { DEV_CreateBuffer=3, UNK_Release=2, BUF_GetDesc=10 };

static uint32_t g_rb_orig[RB_N];
static uint32_t g_rb_obj, g_rb_vptr;
/* stats (natives run on the calling thread, RT in practice; plain counters as in M1) */
static uint64_t g_rb_calls[RB_N], g_rb_fb[RB_N], g_rb_upbytes, g_rb_cbupd, g_rb_com_fast, g_rb_com_slow;
static int g_rb_stat;

/* ---- helpers ---- */
#define ARGS(k) R32(c->esp+4+4*(k))
static inline void rb_ret(CPU*c, uint32_t argbytes){ c->esp+=4+argbytes; }
/* run the original body: the guest stack is still exactly as the caller left it */
static inline void rb_orig(CPU*c, int s){ g_rb_fb[s]++; call_indirect(c,g_rb_orig[s]); }
/* call guest code from a native: args left to right, fake return address, ecx = this (as rhi_native.c rn_gcall) */
static uint32_t rb_gcall(CPU*c, uint32_t fn, uint32_t ecx, int n, const uint32_t *a){
  uint32_t sp=c->esp;
  for(int k=n-1;k>=0;k--) PUSH32(c,a[k]);
  PUSH32(c,0xDEADBEEFu); c->ecx=ecx;
  call_indirect(c,fn);
  c->esp=sp; return c->eax; }
/* one guest COM method call on proxy obj: the stack is built as the translated body's `push args; push this; call [vtbl+4*m]`
   leaves it, then completed by b1com_dispatch or, when it declines, by call_indirect on the vtable entry (the body's path) */
static uint32_t rb_com(CPU*c, uint32_t obj, int m, int n, const uint32_t *a){
  uint32_t sp=c->esp;
  for(int k=n-1;k>=0;k--) PUSH32(c,a[k]);
  PUSH32(c,obj); PUSH32(c,0xDEADBEEFu);
  uint32_t t=R32(R32(obj)+4*(uint32_t)m);
  if(t-RB_COM_BASE<4u*(uint32_t)g_com_nmethods && b1com_dispatch(c,(int)((t-RB_COM_BASE)/4))) g_rb_com_fast++;
  else { g_rb_com_slow++; call_indirect(c,t); }
  c->esp=sp; return c->eax; }
/* topology, f_0088b530 (non-error cases only; the callers check rb_pt_ok first) */
static inline uint32_t rb_topo(uint32_t pt, uint32_t tess){ return tess ? 0x23 : pt==0 ? 4 : pt==1 ? 5 : 2; }
static inline int rb_pt_ok(uint32_t pt, uint32_t tess){ return pt<=2 && (!tess || pt==0); }
static inline uint32_t rb_count(uint32_t pt, uint32_t n){ return pt==0 ? n*3 : pt==1 ? n+2 : n*2; }
/* the RHI object's ID3D11DeviceContext proxy (+0x4c; +0x48 = the device proxy) */
static inline uint32_t rb_ctx(uint32_t self){ return R32(self+0x4c); }

/* ---- constant commit: f_0088e450 (FD3D11ConstantBuffer::CommitConstantsToDevice) + f_00891c40 ---- */
static int rb_cb_commit(CPU*c, uint32_t cb, uint32_t disc, uint32_t self, uint32_t sctx){
  if(!R32(cb+0x30)) return 0;
  uint32_t t=R32(cb+0x40);
  if(!disc){ uint32_t tot=R32(cb+0x44); if(t<tot) t=tot; }
  W32(cb+0x44,t);
  uint32_t sz=R32(cb+0x2c)>>1, idx=1;
  if(sz>=t){ uint32_t num=R32(cb+0x3c); while(idx<num){ idx++; sz>>=1; if(sz<t) break; } }
  idx--; W32(cb+0x38,idx);
  uint32_t owner=R32(cb+0x1c); uint32_t ctx=owner==self?sctx:rb_ctx(owner);
  uint32_t n2=sz*2;
  /* UpdateSubresource(pDst = buffer proxy [idx], Sub 0, pBox NULL, pSrc = shadow data, RowPitch n2, DepthPitch n2) */
  uint32_t a[6]={R32(R32(cb+0x20)+4*idx),0,0,R32(cb+0x34),n2,n2};
  rb_com(c,ctx,CX_UpdateSubresource,6,a);
  W32(cb+0x30,0); W32(cb+0x40,0); g_rb_cbupd++;
  return 1; }
static void rb_commit(CPU*c, uint32_t self, uint32_t ctx){
  static const struct { uint16_t arr, vt; } st[5]={{0x3d8,CX_VSSetConstantBuffers},{0x3e4,CX_HSSetConstantBuffers},{0x3f0,CX_DSSetConstantBuffers},
                                                    {0x408,CX_GSSetConstantBuffers},{0x3fc,CX_PSSetConstantBuffers}};
  uint32_t disc=R32(self+0x2ac);
  uint32_t sp=c->esp; c->esp-=16; uint32_t pb=c->esp;   /* guest local: the one-entry ID3D11Buffer* array */
  for(int k=0;k<5;k++){
    if(k==1 && !R32(self+0x2b0)){ k=2; continue; }          /* HS + DS only with tessellation */
    for(uint32_t i=0;i<6;i++){
      uint32_t cb=R32(R32(self+st[k].arr)+4*i); if(!cb) continue;
      if(!rb_cb_commit(c,cb,disc,self,ctx)) continue;
      W32(pb,R32(R32(cb+0x20)+4*R32(cb+0x38)));
      uint32_t a[3]={i,1,pb}; rb_com(c,ctx,st[k].vt,3,a); } }
  c->esp=sp;
  W32(self+0x2ac,0); }

/* ---- dynamic VB/IB for the UP draws: f_0088b5a0 (buffer) + f_0088b690 (upload) ---- */
static uint32_t rb_up_buffer(CPU*c, uint32_t self, uint32_t count, uint32_t stride, uint32_t bind){
  int vb=bind&1;
  uint32_t arr=self+(vb?0x2b4:0x2b8), ip=self+(vb?0x2bc:0x2c0);
  uint32_t need=count*stride;
  uint32_t slot=arr+4*R32(ip);
  uint32_t g=R32(slot);
  if(g){
    uint32_t w;
    { uint32_t sp=c->esp; c->esp-=32; uint32_t d=c->esp; c->esp-=16; uint32_t a[1]={d}; rb_com(c,g,BUF_GetDesc,1,a); w=R32(d); c->esp=sp; }   /* ID3D11Buffer::GetDesc */
    if(w>=need) goto ret;
    slot=arr+4*R32(ip); uint32_t o=R32(slot); W32(slot,0); if(o) rb_com(c,o,UNK_Release,0,0); }
  { /* CreateBuffer(desc, NULL, &slot) through the device proxy (the b1 seam binds the new shadow buffer to the new proxy) */
    uint32_t sp=c->esp; c->esp-=32; uint32_t d=c->esp; c->esp-=16;
    W32(d,need<0x400000u?0x400000u:need); W32(d+4,2); W32(d+8,bind); W32(d+12,0x10000); W32(d+16,0); W32(d+20,0);
    slot=arr+4*R32(ip); uint32_t o=R32(slot); W32(slot,0); if(o) rb_com(c,o,UNK_Release,0,0);
    uint32_t a[3]={d,0,slot}; rb_com(c,R32(self+0x48),DEV_CreateBuffer,3,a);
    c->esp=sp; }
ret:
  { uint32_t i=R32(ip), b=R32(arr+4*i); i++; W32(ip,i>=1?0:i); return b; } }
static uint32_t rb_up_upload(CPU*c, uint32_t self, uint32_t ctx, uint32_t count, uint32_t stride, uint32_t data, uint32_t bind, uint32_t *off){
  uint32_t cur=self+((bind&1)?0x2c4:0x2c8);
  uint32_t size=count*stride, v=(R32(cur)+3)&~3u, mt=5;
  W32(cur,v);
  uint32_t end=v+size;
  if(end>=0x400000u){ W32(cur,0); end=size; mt=4; }
  *off=R32(cur); W32(cur,end);
  uint32_t buf=rb_up_buffer(c,self,count,stride,bind);
  uint32_t sp=c->esp; c->esp-=16; uint32_t m=c->esp;    /* guest local: D3D11_MAPPED_SUBRESOURCE {pData, RowPitch, DepthPitch} */
  W32(m,0); W32(m+4,0); W32(m+8,0);
  { uint32_t a[5]={buf,0,mt,0,m}; int32_t hr=(int32_t)rb_com(c,ctx,CX_Map,5,a);
    uint32_t pd=R32(m);
    if(hr>=0 && pd) memcpy(MEMP(pd+*off),MEMP(data),size);
    else { static int nw; if(nw++<10) fprintf(stderr,"[rhinat] dynamic %s Map failed (%08x): upload of %u bytes dropped\n",(bind&1)?"VB":"IB",(uint32_t)hr,size); } }
  { uint32_t a[2]={buf,0}; rb_com(c,ctx,CX_Unmap,2,a); }
  c->esp=sp;
  g_rb_upbytes+=size;
  return buf; }
/* IASetVertexBuffers(StartSlot, 1, &vb, &stride, &offset) with the three one-entry arrays as guest locals */
static void rb_set_vb(CPU*c, uint32_t ctx, uint32_t start, uint32_t vb, uint32_t stride, uint32_t off){
  uint32_t sp=c->esp; c->esp-=16; uint32_t l=c->esp;
  W32(l,vb); W32(l+4,stride); W32(l+8,off);
  uint32_t a[5]={start,1,l,l+4,l+8}; rb_com(c,ctx,CX_IASetVertexBuffers,5,a);
  c->esp=sp; }

/* ---- draw group ---- */
/* 134 DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, NumPrimitives, IndexData, IndexDataStride,
   VertexData, VertexDataStride), ret 0x20 */
static void rb_134(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0), ctx=rb_ctx(self);
  if(!ctx || !rb_pt_ok(pt,R32(self+0x2b0))){ rb_orig(c,134); return; }
  g_rb_calls[134]++;
  uint32_t minv=ARGS(1), nverts=ARGS(2), nprims=ARGS(3), idata=ARGS(4), istride=ARGS(5), vdata=ARGS(6), vstride=ARGS(7);
  uint32_t nidx=rb_count(pt,nprims), voff, ioff;
  uint32_t vb=rb_up_upload(c,self,ctx,nverts,vstride,vdata,1,&voff);
  uint32_t ib=rb_up_upload(c,self,ctx,nidx,istride,idata,2,&ioff);
  rb_commit(c,self,ctx);
  rb_set_vb(c,ctx,0,vb,vstride,voff);
  { uint32_t a[3]={ib,istride!=2?42:57,ioff}; rb_com(c,ctx,CX_IASetIndexBuffer,3,a); }
  { uint32_t a[1]={rb_topo(pt,R32(self+0x2b0))}; rb_com(c,ctx,CX_IASetPrimitiveTopology,1,a); }
  { uint32_t a[3]={nidx,0,minv}; rb_com(c,ctx,CX_DrawIndexed,3,a); }
  rb_ret(c,0x20); }
/* 131 DrawPrimitiveUP(PrimitiveType, NumPrimitives, VertexData, VertexDataStride), ret 0x10 */
static void rb_131(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0), ctx=rb_ctx(self);
  if(!ctx || !rb_pt_ok(pt,R32(self+0x2b0))){ rb_orig(c,131); return; }
  g_rb_calls[131]++;
  uint32_t nprims=ARGS(1), vdata=ARGS(2), vstride=ARGS(3);
  uint32_t nv=rb_count(pt,nprims), voff;
  uint32_t vb=rb_up_upload(c,self,ctx,nv,vstride,vdata,1,&voff);
  rb_commit(c,self,ctx);
  rb_set_vb(c,ctx,0,vb,vstride,voff);
  { uint32_t a[1]={rb_topo(pt,R32(self+0x2b0))}; rb_com(c,ctx,CX_IASetPrimitiveTopology,1,a); }
  { uint32_t a[2]={nv,0}; rb_com(c,ctx,CX_Draw,2,a); }
  rb_ret(c,0x10); }
/* 132 BeginDrawIndexedPrimitiveUP(PrimitiveType, NumPrimitives, NumVertices, VertexDataStride, OutVertexData,
   MinVertexIndex, NumIndices, IndexDataStride, OutIndexData), ret 0x24. No D3D11 call: identical to M1. */
static void rb_132(CPU*c){
  uint32_t self=c->ecx;
  uint32_t nv=ARGS(2), vs=ARGS(3), ni=ARGS(6), is=ARGS(7);
  uint32_t vbytes=nv*vs, total=ni*is+vbytes;
  if(total>R32(self+0x28c)){ rb_orig(c,132); return; }        /* scratch grows: appFree/appMalloc in the original */
  g_rb_calls[132]++;
  uint32_t buf=R32(self+0x288);
  W32(self+0x27c,buf); W32(ARGS(4),buf);
  W32(self+0x290,R32(self+0x27c)+vbytes); W32(ARGS(8),R32(self+0x290));
  W32(self+0x294,ARGS(0)); W32(self+0x298,ARGS(1)); W32(self+0x280,nv); W32(self+0x29c,ARGS(5)); W32(self+0x2a0,is); W32(self+0x284,vs);
  rb_ret(c,0x24); }
/* 129 BeginDrawPrimitiveUP(PrimitiveType, NumPrimitives, NumVertices, VertexDataStride, OutVertexData), ret 0x14 */
static void rb_129(CPU*c){
  uint32_t self=c->ecx;
  uint32_t nv=ARGS(2), vs=ARGS(3);
  if(nv*vs>R32(self+0x28c)){ rb_orig(c,129); return; }
  g_rb_calls[129]++;
  uint32_t buf=R32(self+0x288);
  W32(self+0x27c,buf); W32(ARGS(4),buf);
  W32(self+0x280,nv); W32(self+0x284,vs); W32(self+0x294,ARGS(0)); W32(self+0x298,ARGS(1));
  rb_ret(c,0x14); }
/* 133 EndDrawIndexedPrimitiveUP(): commit, then GDynamicRHI->DrawIndexedPrimitiveUP(saved args) through the vtable (native
   134 or whatever the table holds), then clear the two data pointers */
static void rb_133(CPU*c){
  uint32_t self=c->ecx, ctx=rb_ctx(self);
  if(!ctx){ rb_orig(c,133); return; }
  g_rb_calls[133]++;
  rb_commit(c,self,ctx);
  uint32_t g=R32(RB_GLOBAL);
  uint32_t a[8]={R32(self+0x294),R32(self+0x29c),R32(self+0x280),R32(self+0x298),R32(self+0x290),R32(self+0x2a0),R32(self+0x27c),R32(self+0x284)};
  rb_gcall(c,R32(R32(g)+0x218),g,8,a);
  W32(self+0x290,0); W32(self+0x27c,0);
  rb_ret(c,0); }
/* 130 EndDrawPrimitiveUP(): commit, GDynamicRHI->DrawPrimitiveUP(saved args), clear the data pointer */
static void rb_130(CPU*c){
  uint32_t self=c->ecx, ctx=rb_ctx(self);
  if(!ctx){ rb_orig(c,130); return; }
  g_rb_calls[130]++;
  rb_commit(c,self,ctx);
  uint32_t g=R32(RB_GLOBAL);
  uint32_t a[4]={R32(self+0x294),R32(self+0x298),R32(self+0x27c),R32(self+0x284)};
  rb_gcall(c,R32(R32(g)+0x20c),g,4,a);
  W32(self+0x27c,0);
  rb_ret(c,0); }
/* 127 DrawIndexedPrimitive(IndexBuffer, PrimitiveType, BaseVertexIndex, MinIndex, NumVertices, StartIndex, NumPrimitives,
   +2 unused), ret 0x24. IndexBuffer handle - 8 = FD3D11IndexBuffer: +8 ID3D11Buffer proxy, +0xc stride */
static void rb_127(CPU*c){
  uint32_t self=c->ecx, ibh=ARGS(0), pt=ARGS(1), ctx=rb_ctx(self);
  if(!ctx || !ibh || !rb_pt_ok(pt,R32(self+0x2b0))){ rb_orig(c,127); return; }
  g_rb_calls[127]++;
  uint32_t ib=ibh-8, base=ARGS(2), start=ARGS(5), nprims=ARGS(6);
  rb_commit(c,self,ctx);
  uint32_t fmt=R32(ib+0xc)!=2?42:57, n=rb_count(pt,nprims);
  { uint32_t a[3]={R32(ib+8),fmt,0}; rb_com(c,ctx,CX_IASetIndexBuffer,3,a); }
  { uint32_t a[1]={rb_topo(pt,R32(self+0x2b0))}; rb_com(c,ctx,CX_IASetPrimitiveTopology,1,a); }
  uint32_t inst=R32(self+0xc8);
  if(inst>1){ uint32_t a[5]={n,inst,start,base,0}; rb_com(c,ctx,CX_DrawIndexedInstanced,5,a); }
  else { uint32_t a[3]={n,start,base}; rb_com(c,ctx,CX_DrawIndexed,3,a); }
  rb_ret(c,0x24); }
/* 126 DrawPrimitive(PrimitiveType, BaseVertexIndex, NumPrimitives), ret 0xc */
static void rb_126(CPU*c){
  uint32_t self=c->ecx, pt=ARGS(0), ctx=rb_ctx(self);
  if(!ctx || !rb_pt_ok(pt,R32(self+0x2b0))){ rb_orig(c,126); return; }
  g_rb_calls[126]++;
  uint32_t base=ARGS(1), nprims=ARGS(2);
  rb_commit(c,self,ctx);
  uint32_t n=rb_count(pt,nprims);
  { uint32_t a[1]={rb_topo(pt,R32(self+0x2b0))}; rb_com(c,ctx,CX_IASetPrimitiveTopology,1,a); }
  uint32_t inst=R32(self+0xc8);
  if(inst>1){ uint32_t a[4]={n,inst,base,0}; rb_com(c,ctx,CX_DrawInstanced,4,a); }
  else { uint32_t a[2]={n,base}; rb_com(c,ctx,CX_Draw,2,a); }
  rb_ret(c,0xc); }

/* ---- 71 SetStreamSource(StreamIndex, VertexBuffer, Stride, ?, ?, NumInstances), ret 0x18. The VB handle - 4 is the
   ID3D11Buffer proxy itself (TD3D11Resource: D3D object + empty RHI base at +4); offset is always 0 ---- */
static void rb_71(CPU*c){
  uint32_t self=c->ecx, ctx=rb_ctx(self);
  if(!ctx){ rb_orig(c,71); return; }
  g_rb_calls[71]++;
  uint32_t vbh=ARGS(1), stride=ARGS(2);
  rb_set_vb(c,ctx,ARGS(0),vbh?vbh-4:0,stride,0);
  W32(self+0xc8,ARGS(5));
  rb_ret(c,0x18); }

/* ---- 77 SetBoundShaderState(BSS), ret 4. BSS handle - 8: +0x34 input layout, +0x38 VS, +0x3c PS, +0x40 HS, +0x44 DS,
   +0x48 GS (guest COM proxies) ---- */
static void rb_77(CPU*c){
  uint32_t self=c->ecx, h=ARGS(0), ctx=rb_ctx(self);
  if(!ctx || !h){ rb_orig(c,77); return; }
  g_rb_calls[77]++;
  uint32_t b=h-8;
  { uint32_t a[1]={R32(b+0x34)}; rb_com(c,ctx,CX_IASetInputLayout,1,a); }
  { uint32_t a[3]={R32(b+0x38),0,0}; rb_com(c,ctx,CX_VSSetShader,3,a); }
  { uint32_t a[3]={R32(b+0x3c),0,0}; rb_com(c,ctx,CX_PSSetShader,3,a); }
  if(R32(RB_SHADER_PLATFORM)==4){
    { uint32_t a[3]={R32(b+0x40),0,0}; rb_com(c,ctx,CX_HSSetShader,3,a); }
    { uint32_t a[3]={R32(b+0x44),0,0}; rb_com(c,ctx,CX_DSSetShader,3,a); }
    { uint32_t a[3]={R32(b+0x48),0,0}; rb_com(c,ctx,CX_GSSetShader,3,a); }
    W32(self+0x2b0,(R32(b+0x40)&&R32(b+0x44))?1:0); }
  else W32(self+0x2b0,0);
  W32(self+0x2ac,1);
  { uint32_t a[1]={b+8}; rb_gcall(c,0x88d2c0u,self+0x420,1,a); }   /* TDynamicRHIResourceReference<BSS>::operator= (ring of 1024) */
  rb_ret(c,4); }

/* ---- setup / install / stats (called by rhi_hook.c) ---- */
typedef struct { const char *name; const int *slots; } RbGroup;
static const int rb_g_draw[]={126,127,129,130,131,132,133,134,-1}, rb_g_ss[]={71,-1}, rb_g_bss[]={77,-1};
static const RbGroup rb_groups[]={{"draw",rb_g_draw},{"ss",rb_g_ss},{"bss",rb_g_bss}};
#define RB_NG ((int)(sizeof rb_groups/sizeof *rb_groups))
static rb_fn rb_impl(int s){
  switch(s){ case 126: return rb_126; case 127: return rb_127; case 129: return rb_129; case 130: return rb_130; case 131: return rb_131;
    case 132: return rb_132; case 133: return rb_133; case 134: return rb_134; case 71: return rb_71; case 77: return rb_77; }
  return 0; }
static int rb_group_of(int s){ for(int g=0;g<RB_NG;g++) for(const int *p=rb_groups[g].slots;*p>=0;p++) if(*p==s) return g; return -1; }
/* "all", group names, slot numbers (-> their group), "-x" removes; "tex"/34 (M1's UnlockTexture2D) has no b1n native */
static unsigned rb_parse_slots(const char *e){
  unsigned m=0; if(!e || !*e) e="all";
  char b[256]; snprintf(b,sizeof b,"%s",e);
  for(char *sv=0,*t=strtok_r(b,", ",&sv);t;t=strtok_r(0,", ",&sv)){
    int neg=*t=='-'; if(neg) t++;
    unsigned v=0;
    if(!strcmp(t,"all")) v=(1u<<RB_NG)-1;
    else if(!strcmp(t,"tex") || !strcmp(t,"34")) fprintf(stderr,"[rhinat] BSI_RHI_SLOTS: %s has no b1n native (UnlockTexture2D stays on its original body)\n",t);
    else { for(int g=0;g<RB_NG;g++) if(!strcmp(t,rb_groups[g].name)) v|=1u<<g;
      if(!v && *t>='0' && *t<='9'){ int g=rb_group_of(atoi(t)); if(g>=0) v=1u<<g; else fprintf(stderr,"[rhinat] BSI_RHI_SLOTS: slot %s has no native\n",t); } }
    if(neg) m&=~v; else m|=v; }
  return m; }
int rhi_native_b1n_setup(uint32_t obj, const uint32_t *orig, rb_fn *nat){
  const char *why=0;
  if(g_bridge_on) why="the D3D11 bridge is on (these natives are for BSI_RHI=b1n)";
  else if(!b1com_native_ok()) why="the b1n COM seam is not active (BSI_RHI=b1n with the b1 device up is required)";
  else if(!obj || !R32(obj+0x4c) || !R32(obj+0x48)) why="the RHI object has no device/context proxy";
  if(why){ fprintf(stderr,"[rhinat] b1n natives refused: %s\n",why); return 0; }
  for(int s=0;s<RB_N;s++) g_rb_orig[s]=orig[s];
  unsigned m=rb_parse_slots(getenv("BSI_RHI_SLOTS"));
  int n=0; char list[256]; int lp=0; list[0]=0;
  for(int g=0;g<RB_NG;g++) if(m&(1u<<g)){
    lp+=snprintf(list+lp,sizeof list-lp,"%s%s(",lp?" ":"",rb_groups[g].name);
    for(const int *p=rb_groups[g].slots;*p>=0;p++){ nat[*p]=rb_impl(*p); n++; lp+=snprintf(list+lp,sizeof list-lp,"%s%d",p==rb_groups[g].slots?"":",",*p); }
    lp+=snprintf(list+lp,sizeof list-lp,")"); }
  g_rb_stat=getenv("BSI_RHI_STAT")?atoi(getenv("BSI_RHI_STAT")):0;
  fprintf(stderr,"[rhinat] b1n: %d native slots: %s (D3D11 calls go to the b1 COM seam; UnlockTexture2D not ported)\n",n,list);
  return n; }
void rhi_native_b1n_installed(uint32_t obj, uint32_t vptr){ g_rb_obj=obj; g_rb_vptr=vptr; }
void rhi_native_b1n_tick(double now){
  if(!g_rb_stat) return;
  static double t0; static long fr; static uint64_t pc[RB_N], pf[RB_N], pup, pcb, pcf, pcs;
  fr++; if(t0==0){ t0=now; fr=0; return; } if(now-t0<10) return;
  double f=fr>0?(double)fr:1; char b[1024]; int p=0;
  for(int s=0;s<RB_N;s++){ uint64_t k=g_rb_calls[s]-pc[s], x=g_rb_fb[s]-pf[s]; if(k||x) p+=snprintf(b+p,sizeof b-p," %d:%.1f",s,k/f); if(x) p+=snprintf(b+p,sizeof b-p,"(fb %.2f)",x/f); pc[s]=g_rb_calls[s]; pf[s]=g_rb_fb[s]; if(p>900) break; }
  fprintf(stderr,"[rhinat] b1n %ld frames, calls/frame%s | UP upload %.1f KB/f, cb updates %.1f/f | COM calls/f: fast %.1f, slow %.1f\n",fr,b,
    (g_rb_upbytes-pup)/1024.0/f,(g_rb_cbupd-pcb)/f,(g_rb_com_fast-pcf)/f,(g_rb_com_slow-pcs)/f);
  pup=g_rb_upbytes; pcb=g_rb_cbupd; pcf=g_rb_com_fast; pcs=g_rb_com_slow; t0=now; fr=0; }
