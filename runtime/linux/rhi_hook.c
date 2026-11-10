/* RHI census (M0 of notes/RHI_MAP.md §7): pass-through hook on UE3's render hardware interface object.
   BSI_RHI=census turns it on; unset / anything else = off (rhi_hook_present() returns after one cached flag test, nothing else
   happens: no allocation, no thread, no table).

   What it does when on:
   - BioShock Infinite reaches D3D11 through one FD3D11DynamicRHI object: GDynamicRHI = [0x13a17dc], vtable 0x1183290 with
     232 slots, and every RHI call in the exe is a virtual call through that pointer (RHI_MAP.md §1).
   - On the first swapchain Present seen by the bridge (the object exists by then; RHIInit writes 0x13a17dc at 0x536029),
     rhi_hook_present() (called on the RenderingThread, between frames):
       * registers 232 host thunks as extended import entries (g_ximps, hle_dll.c - the same mechanism GetProcAddress uses
         for HLE functions, proc_for()), so each slot gets its own guest-callable magic address IMP_BASE + 4*index.
         call_indirect -> call_indirect_slow -> dispatch() -> thunk; Unicorn-emulated callers reach it through emu.c's
         IMP_BASE fetch hook. No dispatcher change is needed.
       * builds a new guest-memory vtable: [-1] = the original RTTI locator, [0..231] = the thunk addresses, and stores it
         (release) as the object's vptr. The .rdata table is not touched. Every 10 s it re-checks the global and the vptr
         and re-installs on a new object if the game ever re-creates the RHI.
   - Each thunk: counts the call per slot and per thread class (RT = the thread that calls Present, GT = the guest main
     thread, Win32 id 0x1004, other = anything else), times it with CLOCK_MONOTONIC (inclusive, and self = inclusive
     minus nested RHI calls on the same thread), then forwards with call_indirect(c, original slot address). The guest
     return address is already on the stack, so the original body pops it and its thiscall args exactly as before.
   - Every BSI_RHI_SEC (default 10) seconds, on the Present path: appends a window block to BSI_RHI_OUT (default: next to
     BSI_BENCH, i.e. <files>/rhi_census.txt on Android, else ./rhi_census.txt; "%p" in BSI_RHI_OUT = pid): frames, fps, RHI calls/frame by thread,
     top-level RHI ms/frame by thread, the top 40 slots (calls/frame, thread split, inclusive and self ms/frame, us/call)
     and the top 10 by self time; rewrites <out>.total with the same numbers since install for every slot ever called;
     prints one "[rhi]" line to stderr.
   Cost when on: two clock_gettime per RHI call plus the import dispatch path (the header line reports the timer part).
   Integration: one guarded call line in bridge.c bench_frame() (regress/rhi_hook.patch); weak, so a build without this
   file links and behaves as before.

   BSI_RHI=native (M1, regress/rhi_m1/): host implementations of D3D-issuing slots from linux/rhi_native.c (weak hooks
   rhi_native_setup / rhi_native_installed / rhi_native_tick; BSI_RHI_SLOTS picks the groups). The copied table points
   native slots at their own import entry (handler = the native itself, no census thunk) and every other slot at its
   original body, so un-hooked slots cost nothing. BSI_RHI=native,census runs both: every slot gets the census thunk,
   which calls the native where there is one. A build without rhi_native.o (or a refused setup: no bridge, bridge thread,
   BRFAST bit 2) runs as census-only or off. */
#define _GNU_SOURCE
#include "hle.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define RHI_GLOBAL 0x13a17dcu   /* GDynamicRHI */
#define RHI_VTBL   0x1183290u   /* FD3D11DynamicRHI vftable (.rdata) */
#define RHI_N      232
#define RHI_IMP_BASE 0xE0000000u
#define RHI_XIMPS_CAP 4096      /* size of g_ximps[] in hle_dll.c */
#define RHI_TOP    40

typedef void (*rhi_hle_fn)(CPU*c);
typedef struct { uint32_t iat; const char *name; rhi_hle_fn fn; } RhiImp;   /* = Imp in rt_linux.c / hle_dll.c */
extern RhiImp g_ximps[]; extern int g_nximps; extern int g_nimps;
extern __thread uint32_t t_tid;   /* hle_more.c: Win32 thread id (0 or 0x1004 = the guest main thread) */
void call_indirect(CPU*c, uint32_t t);

/* Slot names from notes/RHI_MAP.md §2. Refcount slots 157..231: MSVC reversed the 25 overloads of each family, so slot
   157+25*f+j is resource type 25-j (census-confirmed: GetRefCount slot 209 = GetOcclusionQueryResult calls, AddRef slots
   180/179/177 = SetRasterizer/Depth/BlendState calls per frame); RHI_MAP's first table listed them in type order. */
static const char *const g_rhi_name[RHI_N]={
  /*  0*/ "~FD3D11DynamicRHI",
  /*  1*/ "CreateSamplerState",
  /*  2*/ "CreateRasterizerState",
  /*  3*/ "CreateDepthState",
  /*  4*/ "CreateStencilState",
  /*  5*/ "CreateBlendState",
  /*  6*/ "CreateVertexDeclaration(Name)",
  /*  7*/ "CreateVertexDeclaration",
  /*  8*/ "CreatePixelShader",
  /*  9*/ "CreateVertexShader",
  /* 10*/ "CreateHullShader",
  /* 11*/ "CreateDomainShader",
  /* 12*/ "CreateBoundShaderStateD3D11",
  /* 13*/ "SetShaderParameter(DS)#13",
  /* 14*/ "SetShaderParameter(HS)#14",
  /* 15*/ "CreateGeometryShader",
  /* 16*/ "CreateComputeShader",
  /* 17*/ "DispatchComputeShader",
  /* 18*/ "CreateBoundShaderState",
  /* 19*/ "CreateIndexBuffer",
  /* 20*/ "LockIndexBuffer",
  /* 21*/ "UnlockIndexBuffer",
  /* 22*/ "CreateVertexBuffer",
  /* 23*/ "LockVertexBuffer",
  /* 24*/ "UnlockVertexBuffer",
  /* 25*/ "GetTextureMemoryStats",
  /* 26*/ "CreateTexture2D",
  /* 27*/ "CreateTexture2DArray",
  /* 28*/ "ReallocateTexture2D",
  /* 29*/ "GetTextureSize",
  /* 30*/ "AsyncReallocateTexture2D",
  /* 31*/ "FinalizeAsyncReallocateTexture2D",
  /* 32*/ "CancelAsyncReallocateTexture2D",
  /* 33*/ "LockTexture2D",
  /* 34*/ "UnlockTexture2D",
  /* 35*/ "LockTexture2DArray",
  /* 36*/ "UnlockTexture2DArray",
  /* 37*/ "UpdateTexture2D",
  /* 38*/ "GetResolveTarget",
  /* 39*/ "GetMipTailIdx?",
  /* 40*/ "CopyTexture2D",
  /* 41*/ "CopyMipToMipAsync",
  /* 42*/ "TexUtility#42",
  /* 43*/ "unk_no_op#43",
  /* 44*/ "CreateTextureCube",
  /* 45*/ "LockTextureCubeFace",
  /* 46*/ "UnlockTextureCubeFace",
  /* 47*/ "CreateTargetableSurface",
  /* 48*/ "CreateTargetableCubeSurface",
  /* 49*/ "CopyToResolveTarget",
  /* 50*/ "CopyFromResolveTarget",
  /* 51*/ "CopyFromResolveTargetFast",
  /* 52*/ "CopyFromResolveTargetRectFast",
  /* 53*/ "ReadSurfaceData",
  /* 54*/ "ReadSurfaceFloatData",
  /* 55*/ "CreateOcclusionQuery",
  /* 56*/ "ResetOcclusionQuery",
  /* 57*/ "GetOcclusionQueryResult",
  /* 58*/ "BeginDrawingViewport",
  /* 59*/ "EndDrawingViewport",
  /* 60*/ "IsDrawingViewport",
  /* 61*/ "GetViewportBackBuffer",
  /* 62*/ "BeginFrame?",
  /* 63*/ "EndFrame?",
  /* 64*/ "BeginScene?",
  /* 65*/ "EndScene?",
  /* 66*/ "GetGPUFrameCycles",
  /* 67*/ "unk_returns_512#67",
  /* 68*/ "CreateViewport",
  /* 69*/ "ResizeViewport",
  /* 70*/ "Tick",
  /* 71*/ "SetStreamSource",
  /* 72*/ "SetRasterizerState",
  /* 73*/ "SetRasterizerStateImmediate",
  /* 74*/ "SetViewport",
  /* 75*/ "SetScissorRect",
  /* 76*/ "SetDepthBoundsTest?",
  /* 77*/ "SetBoundShaderState",
  /* 78*/ "SetSamplerStateOnly(CS)",
  /* 79*/ "SetSamplerStateOnly(PS)",
  /* 80*/ "SetTextureParameter(CS)",
  /* 81*/ "SetTextureParameter(PS)",
  /* 82*/ "SetSurfaceParameter(CS)",
  /* 83*/ "OMBindUnorderedAccessView",
  /* 84*/ "SetUAVParameter",
  /* 85*/ "SetSamplerState(HS)",
  /* 86*/ "SetSamplerState(DS)",
  /* 87*/ "SetSamplerState(CS)",
  /* 88*/ "SetSamplerState(GS)",
  /* 89*/ "SetSamplerState(VS)",
  /* 90*/ "SetSamplerState(PS)",
  /* 91*/ "unk_no_op#91",
  /* 92*/ "unk_no_op#92",
  /* 93*/ "SetVertexShaderParameter",
  /* 94*/ "SetVertexShaderBoolParameter",
  /* 95*/ "SetVertexShaderFloatArray",
  /* 96*/ "SetPixelShaderParameter",
  /* 97*/ "SetPixelShaderBoolParameter",
  /* 98*/ "SetShaderParameter(CS)",
  /* 99*/ "SetShaderParameter(GS)",
  /*100*/ "SetShaderParameter(DS)",
  /*101*/ "SetShaderParameter(HS)",
  /*102*/ "SetPixelShaderParameter#102",
  /*103*/ "SetVertexShaderParameter#103",
  /*104*/ "SetShaderBoolParameter(DS)",
  /*105*/ "SetShaderBoolParameter(HS)",
  /*106*/ "unk_no_op#106",
  /*107*/ "SetViewParameters",
  /*108*/ "unk_no_op#108",
  /*109*/ "SetShaderRegisterAllocation?",
  /*110*/ "SetDepthState",
  /*111*/ "SetStencilState",
  /*112*/ "SetBlendState",
  /*113*/ "SetMRTBlendState?",
  /*114*/ "SetRenderTargets",
  /*115*/ "SetColorWriteEnable",
  /*116*/ "SetColorWriteMask",
  /*117*/ "SetMRTColorWriteEnable",
  /*118*/ "SetMRTColorWriteMask",
  /*119*/ "unk_no_op_hi_stencil#119",
  /*120*/ "unk_no_op#120",
  /*121*/ "unk_no_op#121",
  /*122*/ "unk_no_op#122",
  /*123*/ "unk_no_op#123",
  /*124*/ "BeginOcclusionQuery",
  /*125*/ "EndOcclusionQuery",
  /*126*/ "DrawPrimitive",
  /*127*/ "DrawIndexedPrimitive",
  /*128*/ "stub_DrawMesh#128",
  /*129*/ "BeginDrawPrimitiveUP",
  /*130*/ "EndDrawPrimitiveUP",
  /*131*/ "DrawPrimitiveUP",
  /*132*/ "BeginDrawIndexedPrimitiveUP",
  /*133*/ "EndDrawIndexedPrimitiveUP",
  /*134*/ "DrawIndexedPrimitiveUP",
  /*135*/ "DrawSpriteParticles",
  /*136*/ "DrawSubUVParticles",
  /*137*/ "Clear",
  /*138*/ "unk_no_op#138",
  /*139*/ "unk_no_op#139",
  /*140*/ "unk_no_op_PushEvent#140",
  /*141*/ "unk_no_op_PopEvent#141",
  /*142*/ "unk_returns_0#142",
  /*143*/ "unk_no_op#143",
  /*144*/ "unk_no_op#144",
  /*145*/ "GetAvailableResolutions",
  /*146*/ "GetSupportedResolution",
  /*147*/ "unk_no_op#147",
  /*148*/ "unk_no_op#148",
  /*149*/ "unk_no_op#149",
  /*150*/ "BSI_CreateTex2D+SRV#150",
  /*151*/ "BSI_SetPrivateData#151",
  /*152*/ "CreateTexture3D",
  /*153*/ "LockTexture3D",
  /*154*/ "UnlockTexture3D",
  /*155*/ "UpdateTexture3D",
  /*156*/ "FillTexture3D",
  /*157*/ "AddRef(Texture3D)",
  /*158*/ "AddRef(Viewport)",
  /*159*/ "AddRef(OcclusionQuery)",
  /*160*/ "AddRef(Type22)",
  /*161*/ "AddRef(Type21)",
  /*162*/ "AddRef(Type20)",
  /*163*/ "AddRef(Type19)",
  /*164*/ "AddRef(Texture2D)",
  /*165*/ "AddRef(Texture)",
  /*166*/ "AddRef(Surface)",
  /*167*/ "AddRef(VertexBuffer)",
  /*168*/ "AddRef(IndexBuffer)",
  /*169*/ "AddRef(BoundShaderState)",
  /*170*/ "AddRef(ComputeShader)",
  /*171*/ "AddRef(GeometryShader)",
  /*172*/ "AddRef(PixelShader)",
  /*173*/ "AddRef(DomainShader)",
  /*174*/ "AddRef(HullShader)",
  /*175*/ "AddRef(VertexShader)",
  /*176*/ "AddRef(VertexDeclaration)",
  /*177*/ "AddRef(BlendState)",
  /*178*/ "AddRef(StencilState)",
  /*179*/ "AddRef(DepthState)",
  /*180*/ "AddRef(RasterizerState)",
  /*181*/ "AddRef(SamplerState)",
  /*182*/ "RemoveRef(Texture3D)",
  /*183*/ "RemoveRef(Viewport)",
  /*184*/ "RemoveRef(OcclusionQuery)",
  /*185*/ "RemoveRef(Type22)",
  /*186*/ "RemoveRef(Type21)",
  /*187*/ "RemoveRef(Type20)",
  /*188*/ "RemoveRef(Type19)",
  /*189*/ "RemoveRef(Texture2D)",
  /*190*/ "RemoveRef(Texture)",
  /*191*/ "RemoveRef(Surface)",
  /*192*/ "RemoveRef(VertexBuffer)",
  /*193*/ "RemoveRef(IndexBuffer)",
  /*194*/ "RemoveRef(BoundShaderState)",
  /*195*/ "RemoveRef(ComputeShader)",
  /*196*/ "RemoveRef(GeometryShader)",
  /*197*/ "RemoveRef(PixelShader)",
  /*198*/ "RemoveRef(DomainShader)",
  /*199*/ "RemoveRef(HullShader)",
  /*200*/ "RemoveRef(VertexShader)",
  /*201*/ "RemoveRef(VertexDeclaration)",
  /*202*/ "RemoveRef(BlendState)",
  /*203*/ "RemoveRef(StencilState)",
  /*204*/ "RemoveRef(DepthState)",
  /*205*/ "RemoveRef(RasterizerState)",
  /*206*/ "RemoveRef(SamplerState)",
  /*207*/ "GetRefCount(Texture3D)",
  /*208*/ "GetRefCount(Viewport)",
  /*209*/ "GetRefCount(OcclusionQuery)",
  /*210*/ "GetRefCount(Type22)",
  /*211*/ "GetRefCount(Type21)",
  /*212*/ "GetRefCount(Type20)",
  /*213*/ "GetRefCount(Type19)",
  /*214*/ "GetRefCount(Texture2D)",
  /*215*/ "GetRefCount(Texture)",
  /*216*/ "GetRefCount(Surface)",
  /*217*/ "GetRefCount(VertexBuffer)",
  /*218*/ "GetRefCount(IndexBuffer)",
  /*219*/ "GetRefCount(BoundShaderState)",
  /*220*/ "GetRefCount(ComputeShader)",
  /*221*/ "GetRefCount(GeometryShader)",
  /*222*/ "GetRefCount(PixelShader)",
  /*223*/ "GetRefCount(DomainShader)",
  /*224*/ "GetRefCount(HullShader)",
  /*225*/ "GetRefCount(VertexShader)",
  /*226*/ "GetRefCount(VertexDeclaration)",
  /*227*/ "GetRefCount(BlendState)",
  /*228*/ "GetRefCount(StencilState)",
  /*229*/ "GetRefCount(DepthState)",
  /*230*/ "GetRefCount(RasterizerState)",
  /*231*/ "GetRefCount(SamplerState)",
};

static int g_rhi_mode=-1;                 /* -1 not read yet, 0 off, else bit 0 census, bit 1 native, bit 2 b1 (native renderer, B1) */
/* linux/rhi_b1.c (weak: absent = no b1): every slot of the table goes through rhi_b1_slot, which runs the original body and
   mirrors the call onto our DXVK device (B1.2 shadow mode). rhi_b1_tick runs on every Present. */
void rhi_b1_slot(CPU *c, int s, uint32_t orig) __attribute__((weak));
void rhi_b1_tick(double now) __attribute__((weak));
static rhi_hle_fn g_rhi_nat[RHI_N];       /* native host implementation per slot (BSI_RHI=native), 0 = original body */
/* BSI_RHI=native,census + BSI_RHI_ALT=1: in-process A/B. The census thunk runs the natives only in every other report window
   (switched on the Present path, where no RHI call is in flight on the RT), so native and original windows of the same scene
   alternate under the same VM load; each window line says which side it was, plus the RT's CPU time from schedstat. */
static volatile int g_rhi_nat_live=1; static int g_rhi_alt;
static int g_rhi_nnat;
/* linux/rhi_native.c (weak: absent = no natives). setup fills nat[] once; installed() gets the object and table after every
   install; tick() runs on every Present (RenderingThread) for the natives' own stats line. */
int rhi_native_setup(uint32_t obj, const uint32_t *orig, rhi_hle_fn *nat) __attribute__((weak));
void rhi_native_installed(uint32_t obj, uint32_t vptr) __attribute__((weak));
void rhi_native_tick(double now) __attribute__((weak));
static uint32_t g_rhi_orig[RHI_N];        /* original slot targets */
static uint32_t g_rhi_obj, g_rhi_vptr;    /* hooked object, our vptr value (table base + 4) */
static int g_rhi_rt_ktid, g_rhi_installs;
static uint64_t g_rhi_n[RHI_N][3], g_rhi_incl[RHI_N][3], g_rhi_self[RHI_N];   /* [slot][0 RT,1 GT,2 other] */
static uint64_t g_rhi_top[3];             /* top-level (depth 0) inclusive ns per thread class */
static double g_rhi_clk_ns;               /* cost of one clock_gettime pair */

static inline uint64_t rhi_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
static __thread int t_rhi_ktid;
static inline int rhi_cls(void){
  int kt=t_rhi_ktid; if(!kt) kt=t_rhi_ktid=(int)syscall(SYS_gettid);
  if(kt==g_rhi_rt_ktid) return 0;
  return (t_tid==0 || t_tid==0x1004u) ? 1 : 2; }
/* per-thread nesting stack: esp at entry + time spent in nested RHI calls. A nested call has a lower esp than its parent,
   or the same esp for a tail jump through the vtable (slot 128 = `jmp [vtbl+0x1fc]` into slot 127). Frames whose esp is
   below the current esp were left behind by a guest exception unwind that skipped the thunk's epilogue: they are dropped
   on the next entry. */
#define RHI_DEPTH 32
typedef struct { uint32_t esp; uint64_t child; } RhiFr;
static __thread RhiFr t_rhi_fr[RHI_DEPTH]; static __thread int t_rhi_d;
#define RHI_ADD(p,v) __atomic_fetch_add((p),(v),__ATOMIC_RELAXED)
static void rhi_thunk(CPU*c, int s){
  uint32_t esp=c->esp; int cls=rhi_cls();
  int d=t_rhi_d; while(d>0 && d<=RHI_DEPTH && t_rhi_fr[d-1].esp<esp) d--;
  if(d<RHI_DEPTH){ t_rhi_fr[d].esp=esp; t_rhi_fr[d].child=0; }
  t_rhi_d=d+1;
  uint64_t t0=rhi_ns();
  if(g_rhi_mode&4) rhi_b1_slot(c,s,g_rhi_orig[s]);
  else if(g_rhi_nat[s] && g_rhi_nat_live) g_rhi_nat[s](c); else call_indirect(c,g_rhi_orig[s]);
  uint64_t dt=rhi_ns()-t0;
  t_rhi_d=d;
  uint64_t child = d<RHI_DEPTH ? t_rhi_fr[d].child : 0;
  if(d>0 && d<=RHI_DEPTH) t_rhi_fr[d-1].child+=dt;
  RHI_ADD(&g_rhi_n[s][cls],1); RHI_ADD(&g_rhi_incl[s][cls],dt); RHI_ADD(&g_rhi_self[s],dt>child?dt-child:0);
  if(d==0) RHI_ADD(&g_rhi_top[cls],dt);
}
/* 232 distinct host thunks (an import handler only receives the CPU, so the slot must be in the function) */
#define RHI_T1(n) static void rhi_t_##n(CPU*c){ rhi_thunk(c,n); }
#define RHI_T10(a) RHI_T1(a##0) RHI_T1(a##1) RHI_T1(a##2) RHI_T1(a##3) RHI_T1(a##4) RHI_T1(a##5) RHI_T1(a##6) RHI_T1(a##7) RHI_T1(a##8) RHI_T1(a##9)
RHI_T1(0) RHI_T1(1) RHI_T1(2) RHI_T1(3) RHI_T1(4) RHI_T1(5) RHI_T1(6) RHI_T1(7) RHI_T1(8) RHI_T1(9)
RHI_T10(1) RHI_T10(2) RHI_T10(3) RHI_T10(4) RHI_T10(5) RHI_T10(6) RHI_T10(7) RHI_T10(8) RHI_T10(9) RHI_T10(10) RHI_T10(11)
RHI_T10(12) RHI_T10(13) RHI_T10(14) RHI_T10(15) RHI_T10(16) RHI_T10(17) RHI_T10(18) RHI_T10(19) RHI_T10(20) RHI_T10(21) RHI_T10(22)
RHI_T1(230) RHI_T1(231)
#define RHI_F1(n) rhi_t_##n,
#define RHI_F10(a) RHI_F1(a##0) RHI_F1(a##1) RHI_F1(a##2) RHI_F1(a##3) RHI_F1(a##4) RHI_F1(a##5) RHI_F1(a##6) RHI_F1(a##7) RHI_F1(a##8) RHI_F1(a##9)
static const rhi_hle_fn g_rhi_thunk[RHI_N]={
  RHI_F1(0) RHI_F1(1) RHI_F1(2) RHI_F1(3) RHI_F1(4) RHI_F1(5) RHI_F1(6) RHI_F1(7) RHI_F1(8) RHI_F1(9)
  RHI_F10(1) RHI_F10(2) RHI_F10(3) RHI_F10(4) RHI_F10(5) RHI_F10(6) RHI_F10(7) RHI_F10(8) RHI_F10(9) RHI_F10(10) RHI_F10(11)
  RHI_F10(12) RHI_F10(13) RHI_F10(14) RHI_F10(15) RHI_F10(16) RHI_F10(17) RHI_F10(18) RHI_F10(19) RHI_F10(20) RHI_F10(21) RHI_F10(22)
  RHI_F1(230) RHI_F1(231) };

static uint32_t g_rhi_magic0;             /* magic address of slot 0 (IMP_BASE + 4*import index) */
static char g_rhi_out[600];
static int rhi_register(void){            /* extended import entries for the 232 thunks; 0 = no room */
  if(g_rhi_magic0) return 1;
  int x=__atomic_load_n(&g_nximps,__ATOMIC_ACQUIRE);
  if(x+RHI_N>RHI_XIMPS_CAP){ fprintf(stderr,"[rhi] no room in the extended import table (%d used): census off\n",x); return 0; }
  for(int s=0;s<RHI_N;s++){ static char nm[RHI_N][56]; snprintf(nm[s],sizeof nm[s],"rhi!%s",g_rhi_name[s]);
    g_ximps[x+s].iat=0; g_ximps[x+s].name=nm[s]; g_ximps[x+s].fn=(!(g_rhi_mode&1) && g_rhi_nat[s]) ? g_rhi_nat[s] : g_rhi_thunk[s]; }
  __atomic_store_n(&g_nximps,x+RHI_N,__ATOMIC_RELEASE);   /* entries are complete before the dispatcher's range covers them */
  g_rhi_magic0=RHI_IMP_BASE+4u*(uint32_t)(g_nimps+x);
  return 1; }
static int rhi_install(uint32_t obj){
  if(!g_rhi_vptr && (g_rhi_mode&2)){                        /* natives: chosen once, before the import entries are written */
    for(int s=0;s<RHI_N;s++) g_rhi_orig[s]=R32(RHI_VTBL+4*s);
    g_rhi_nnat = rhi_native_setup ? rhi_native_setup(obj,g_rhi_orig,g_rhi_nat) : 0;
    if(g_rhi_nnat<=0){ memset(g_rhi_nat,0,sizeof g_rhi_nat); g_rhi_nnat=0; g_rhi_mode&=~2;
      fprintf(stderr,"[rhi] native: %s\n",rhi_native_setup?"refused by rhi_native_setup (see [rhinat] line), RHI natives off":"this build has no rhi_native.o, RHI natives off");
      if(!g_rhi_mode) return 0; } }
  if(!rhi_register()) return 0;
  if(!g_rhi_vptr && (g_rhi_mode&4)){
    /* b1: patch the game's own .rdata table in place (guest memory is RW). Code that read the vptr before the install and
       keeps it in a register (engine init does this around CreateViewport) then still reaches our thunks; the object
       keeps vptr 0x1183290, so the re-install check below sees "ours" through g_rhi_vptr == RHI_VTBL. */
    for(int s=0;s<RHI_N;s++) g_rhi_orig[s]=R32(RHI_VTBL+4*s);
    for(int s=0;s<RHI_N;s++) __atomic_store_n((uint32_t*)MEMP(RHI_VTBL+4*s),g_rhi_magic0+4u*s,__ATOMIC_RELEASE);
    g_rhi_vptr=RHI_VTBL; }
  if(!g_rhi_vptr){
    for(int s=0;s<RHI_N;s++) g_rhi_orig[s]=R32(RHI_VTBL+4*s);
    uint32_t t=gmalloc(4*(RHI_N+1));
    W32(t,R32(RHI_VTBL-4));                                  /* MSVC: complete-object locator in front of the table */
    for(int s=0;s<RHI_N;s++) W32(t+4+4*s,((g_rhi_mode&5) || g_rhi_nat[s]) ? g_rhi_magic0+4u*s : g_rhi_orig[s]);
    g_rhi_vptr=t+4;
    uint64_t a=rhi_ns(); for(int k=0;k<4096;k++){ volatile uint64_t x=rhi_ns(); (void)x; volatile uint64_t y=rhi_ns(); (void)y; }
    g_rhi_clk_ns=(rhi_ns()-a)/4096.0; }
  if(obj) __atomic_store_n((uint32_t*)MEMP(obj),g_rhi_vptr,__ATOMIC_RELEASE);   /* table written before the vptr is visible */
  g_rhi_obj=obj; g_rhi_installs++;
  if(g_rhi_nnat && rhi_native_installed) rhi_native_installed(obj,g_rhi_vptr);
  if((g_rhi_mode&5)==4){ fprintf(stderr,"[rhi] b1 on (shadow): object %08x vptr %08x -> %08x, all %d slots mirrored%s\n",obj,RHI_VTBL,g_rhi_vptr,RHI_N,
      getenv("BSI_NOTRACE")?"":" WARNING: import trace on (BSI_NOTRACE unset), every RHI call is logged"); return 1; }
  if(!(g_rhi_mode&1)){ fprintf(stderr,"[rhi] native on: object %08x vptr %08x -> %08x, %d native slots%s\n",obj,RHI_VTBL,g_rhi_vptr,g_rhi_nnat,
      getenv("BSI_NOTRACE")?"":" WARNING: import trace on (BSI_NOTRACE unset), every native RHI call is logged"); return 1; }
  { FILE *f=fopen(g_rhi_out,"a"); if(f){ struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
      fprintf(f,"=== t=%ld install #%d: pid %d, object %08x, BSI_BENCH=%s ===\n",(long)rt.tv_sec,g_rhi_installs,(int)getpid(),obj,getenv("BSI_BENCH")?getenv("BSI_BENCH"):"-"); fclose(f); } }
  fprintf(stderr,"[rhi] census%s on: object %08x vptr %08x -> %08x (thunks %08x..%08x, RT tid %d, clock pair %.0f ns)%s\n",g_rhi_nnat?" + native":"",obj,RHI_VTBL,g_rhi_vptr,
    g_rhi_magic0,g_rhi_magic0+4*(RHI_N-1),g_rhi_rt_ktid,g_rhi_clk_ns,getenv("BSI_NOTRACE")?"":" WARNING: import trace on (BSI_NOTRACE unset), every RHI call is logged");
  return 1; }

/* ---- report ---- */
static uint64_t g_rp_n[RHI_N][3], g_rp_incl[RHI_N][3], g_rp_self[RHI_N], g_rp_top[3];   /* previous window snapshot */
static long g_rp_frames, g_rhi_frames; static double g_rp_t, g_rhi_t_install;
typedef struct { int s; double v; } RhiKV;
static int rhi_kv_cmp(const void *a, const void *b){ double x=((const RhiKV*)a)->v, y=((const RhiKV*)b)->v; return x<y?1:x>y?-1:0; }
static void rhi_block(FILE *f, const char *what, double dt, long frames, uint64_t n[RHI_N][3], uint64_t incl[RHI_N][3], uint64_t *self, uint64_t *top, int all){
  double fr=frames>0?(double)frames:1; uint64_t tot[3]={0,0,0}; int used=0;
  for(int s=0;s<RHI_N;s++){ uint64_t k=n[s][0]+n[s][1]+n[s][2]; if(k) used++; for(int t=0;t<3;t++) tot[t]+=n[s][t]; }
  uint64_t T=tot[0]+tot[1]+tot[2]; double Tp=T?100.0/T:0;
  fprintf(f,"[rhi] %s: %.1f s, %ld frames (%.1f fps, %.2f ms/frame) | RHI calls/frame %.1f (RT %.1f%%, GT %.1f%%, other %.1f%%) | slots used %d/%d\n",
    what,dt,frames,frames/(dt>0?dt:1),dt*1000/fr,T/fr,tot[0]*Tp,tot[1]*Tp,tot[2]*Tp,used,RHI_N);
  fprintf(f,"[rhi]   top-level RHI time ms/frame: RT %.3f, GT %.3f, other %.3f | census timer overhead est %.3f ms/frame (%.0f ns/call)\n",
    top[0]/1e6/fr,top[1]/1e6/fr,top[2]/1e6/fr,T*g_rhi_clk_ns/1e6/fr,g_rhi_clk_ns);
  RhiKV kv[RHI_N]; int m=0;
  for(int s=0;s<RHI_N;s++){ uint64_t k=n[s][0]+n[s][1]+n[s][2]; if(k){ kv[m].s=s; kv[m].v=(double)k; m++; } }
  qsort(kv,m,sizeof *kv,rhi_kv_cmp);
  int lim=all?m:(m<RHI_TOP?m:RHI_TOP);
  fprintf(f,"  rank slot name                              calls/frame   RT%%   GT%%  oth%%  incl_ms/f  self_ms/f  us/call\n");
  for(int i=0;i<lim;i++){ int s=kv[i].s; uint64_t k=n[s][0]+n[s][1]+n[s][2], in=incl[s][0]+incl[s][1]+incl[s][2];
    fprintf(f,"  %4d %4d %-33s %11.2f %5.1f %5.1f %5.1f %10.3f %10.3f %8.2f\n",i+1,s,g_rhi_name[s],k/fr,100.0*n[s][0]/k,100.0*n[s][1]/k,100.0*n[s][2]/k,
      in/1e6/fr,self[s]/1e6/fr,in/1e3/k); }
  for(int i=0;i<m;i++) kv[i].v=(double)self[kv[i].s];
  qsort(kv,m,sizeof *kv,rhi_kv_cmp);
  fprintf(f,"  top 10 by self time:");
  for(int i=0;i<m && i<10;i++) fprintf(f," %d %s %.3f ms/f;",kv[i].s,g_rhi_name[kv[i].s],kv[i].v/1e6/fr);
  fprintf(f,"\n");
}
static uint64_t rhi_rt_cpu_ns(void){   /* RenderingThread CPU time (this is called on the RT) */
  char p[64], b[96]; snprintf(p,sizeof p,"/proc/self/task/%d/schedstat",g_rhi_rt_ktid); FILE *f=fopen(p,"r"); if(!f) return 0;
  unsigned long long r=0; if(!fgets(b,sizeof b,f) || sscanf(b,"%llu",&r)!=1) r=0; fclose(f); return r; }
static void rhi_report(double now){
  static uint64_t n[RHI_N][3], incl[RHI_N][3], self[RHI_N], top[3];   /* RT only: one reporter */
  static uint64_t dn[RHI_N][3], dincl[RHI_N][3], dself[RHI_N], dtop[3];
  for(int s=0;s<RHI_N;s++){ for(int t=0;t<3;t++){ n[s][t]=__atomic_load_n(&g_rhi_n[s][t],__ATOMIC_RELAXED); incl[s][t]=__atomic_load_n(&g_rhi_incl[s][t],__ATOMIC_RELAXED);
      dn[s][t]=n[s][t]-g_rp_n[s][t]; dincl[s][t]=incl[s][t]-g_rp_incl[s][t]; }
    self[s]=__atomic_load_n(&g_rhi_self[s],__ATOMIC_RELAXED); dself[s]=self[s]-g_rp_self[s]; }
  for(int t=0;t<3;t++){ top[t]=__atomic_load_n(&g_rhi_top[t],__ATOMIC_RELAXED); dtop[t]=top[t]-g_rp_top[t]; }
  long fr=g_rhi_frames-g_rp_frames; double dt=now-g_rp_t;
  struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
  FILE *f=fopen(g_rhi_out,"a");
  { static uint64_t cpu0; uint64_t cpu=rhi_rt_cpu_ns(); double rtc=cpu0&&fr>0?(cpu-cpu0)/1e6/fr:0; cpu0=cpu;
    if(f){ fprintf(f,"=== t=%ld window natives %s rt_cpu_ms_per_frame %.3f ===\n",(long)rt.tv_sec,g_rhi_nnat?(g_rhi_nat_live?"on":"off"):"none",rtc); rhi_block(f,"window",dt,fr,dn,dincl,dself,dtop,0); fclose(f); } }
  if(g_rhi_alt && g_rhi_nnat) g_rhi_nat_live=!g_rhi_nat_live;   /* BSI_RHI_ALT: next window runs the other side */
  char tp[640]; snprintf(tp,sizeof tp,"%s.total",g_rhi_out); f=fopen(tp,"w");
  if(f){ fprintf(f,"=== t=%ld since install (%d install(s); thread classes: RT = Present caller, GT = Win32 tid 0x1004) ===\n",(long)rt.tv_sec,g_rhi_installs);
    rhi_block(f,"total",now-g_rhi_t_install,g_rhi_frames,n,incl,self,top,1); fclose(f); }
  { uint64_t T=0; int b=-1; uint64_t bk=0; for(int s=0;s<RHI_N;s++){ uint64_t k=dn[s][0]+dn[s][1]+dn[s][2]; T+=k; if(k>bk){ bk=k; b=s; } }
    double frd=fr>0?(double)fr:1;
    fprintf(stderr,"[rhi] %ld frames: %.0f RHI calls/frame, RT top-level %.2f ms/frame, top slot %d %s %.0f/frame -> %s\n",fr,T/frd,dtop[0]/1e6/frd,b,b>=0?g_rhi_name[b]:"-",bk/frd,g_rhi_out); }
  memcpy(g_rp_n,n,sizeof n); memcpy(g_rp_incl,incl,sizeof incl); memcpy(g_rp_self,self,sizeof self); memcpy(g_rp_top,top,sizeof top);
  g_rp_frames=g_rhi_frames; g_rp_t=now;
}

/* called by bridge.c bench_frame() on every swapchain Present (RenderingThread) */
static void rhi_mode_init(void){
  if(g_rhi_mode>=0) return;
  { const char *e=getenv("BSI_RHI");   /* "census", "native", both ("native,census"), or "b1" (+",census") */
    int m=e ? (strstr(e,"census")?1:0)|(strstr(e,"native")?2:0)|(strstr(e,"b1")?4:0) : 0;
    if((m&4) && strstr(e,"b1n") && !getenv("BSI_B1_INV")){ m&=~4; fprintf(stderr,"[rhi] b1n: RHI table left alone (the COM seam does the work; BSI_B1_INV=1 re-enables the slot hooks)\n"); }
    if((m&4) && !rhi_b1_slot){ fprintf(stderr,"[rhi] b1 requested but this build has no rhi_b1.o: b1 off\n"); m&=~4; }
    if((m&6)==6){ fprintf(stderr,"[rhi] b1 and native cannot run together: native off\n"); m&=~2; }
    if(!m){ g_rhi_mode=0; return; }
    const char *o=getenv("BSI_RHI_OUT"), *b=getenv("BSI_BENCH");
    if(o && *o){ const char *q=strstr(o,"%p");   /* "%p" in BSI_RHI_OUT = process id (one file per run) */
      if(q) snprintf(g_rhi_out,sizeof g_rhi_out,"%.*s%d%s",(int)(q-o),o,(int)getpid(),q+2); else snprintf(g_rhi_out,sizeof g_rhi_out,"%s",o); }
    else if(b && *b && strrchr(b,'/')) snprintf(g_rhi_out,sizeof g_rhi_out,"%.*s/rhi_census.txt",(int)(strrchr(b,'/')-b),b);
    else snprintf(g_rhi_out,sizeof g_rhi_out,"rhi_census.txt");
    { const char *a=getenv("BSI_RHI_ALT"); g_rhi_alt=a && atoi(a)>0 && (m&3)==3; }
    __atomic_store_n(&g_rhi_mode,m,__ATOMIC_RELEASE); } }
/* called by bridge.c for every bridged COM call (b1 only): install as soon as GDynamicRHI exists, i.e. before the
   game's first CreateViewport, so the D3D11 viewport and all resources are created through our table. */
void rhi_hook_bridge_call(void){
  if(__builtin_expect(g_rhi_mode==0,1) || g_rhi_vptr) return;
  if(g_rhi_mode<0){ rhi_mode_init(); if(!(g_rhi_mode&4)) return; }
  if(!(g_rhi_mode&4)) return;
  static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
  /* b1 patches the static table (0x1183290), so it does not need the object: install at the very first bridged call
     (inside RHIInit, before the object even exists), after a sanity check that the table is the one we know. */
  if(R32(RHI_VTBL)!=0x89e590u || R32(RHI_VTBL+4*68)!=0x894a40u){ static int w; if(!w++) fprintf(stderr,"[rhi] b1: table %08x does not look like the D3D11 RHI vftable (%08x / %08x): b1 off\n",RHI_VTBL,R32(RHI_VTBL),R32(RHI_VTBL+4*68)); g_rhi_mode=0; return; }
  pthread_mutex_lock(&mu);
  if(!g_rhi_vptr){ uint32_t obj=R32(RHI_GLOBAL); if(!rhi_install(obj)) g_rhi_mode=0; }
  pthread_mutex_unlock(&mu); }
/* called by bridge.c bench_frame() on every swapchain Present (RenderingThread) */
void rhi_hook_present(void){
  if(__builtin_expect(g_rhi_mode==0,1)) return;
  if(g_rhi_mode<0){ rhi_mode_init(); if(!g_rhi_mode) return; }
  if(!g_rhi_rt_ktid) g_rhi_rt_ktid=(int)syscall(SYS_gettid);
  static double period=-1, last_chk;
  if(period<0){ const char *e=getenv("BSI_RHI_SEC"); period=e?atof(e):10; if(period<1) period=1; }
  double now=rhi_ns()*1e-9;
  if(!g_rhi_obj && (g_rhi_mode&4) && g_rhi_vptr){           /* b1: table patched early; adopt the object once it exists */
    uint32_t obj=R32(RHI_GLOBAL); if(obj && R32(obj)==RHI_VTBL){ g_rhi_obj=obj; fprintf(stderr,"[rhi] b1: RHI object %08x adopted\n",obj); }
    g_rp_t=g_rhi_t_install=last_chk=now; g_rp_frames=g_rhi_frames=0; return; }
  if(!g_rhi_obj){                                           /* not installed yet */
    static int tries; uint32_t obj=R32(RHI_GLOBAL);
    if(!obj || R32(obj)!=RHI_VTBL){ if(tries++==0) fprintf(stderr,"[rhi] GDynamicRHI %08x vptr %08x: not the D3D11 RHI yet, retrying each Present\n",obj,obj?R32(obj):0); return; }
    if(!rhi_install(obj)){ g_rhi_mode=0; return; }
    g_rp_t=g_rhi_t_install=last_chk=now; g_rp_frames=g_rhi_frames=0; return; }
  g_rhi_frames++;
  if(now-last_chk>=1.0){ last_chk=now; uint32_t obj=R32(RHI_GLOBAL);   /* the game re-created the RHI object? */
    if(obj!=g_rhi_obj || (obj && R32(obj)!=g_rhi_vptr)){
      if(obj && R32(obj)==RHI_VTBL){ fprintf(stderr,"[rhi] RHI object changed %08x -> %08x: re-installing\n",g_rhi_obj,obj); rhi_install(obj); }
      else { static int warned; if(!warned++) fprintf(stderr,"[rhi] GDynamicRHI %08x vptr %08x is neither ours nor the D3D11 table: census paused\n",obj,obj?R32(obj):0); } } }
  if((g_rhi_mode&2) && rhi_native_tick) rhi_native_tick(now);
  if((g_rhi_mode&4) && rhi_b1_tick) rhi_b1_tick(now);
  if((g_rhi_mode&1) && now-g_rp_t>=period) rhi_report(now);
}
