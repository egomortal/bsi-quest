/* vr_ui_rhi (VR UI layer, notes/VR_STEREO.md §11): coverage alpha in the back buffer, so the UI column can be composited.

   Why: the UI column is cleared to (0,0,0,0) each frame (vr_ui.c), but UE3 blends UI with the alpha channel left alone
   (translucent canvas items use AlphaSrc=Zero, AlphaDest=One), so after drawing, alpha says nothing about where the UI is.
   Fix: while the back buffer is the render target, every BLENDING state (D3D11 BlendEnable and DestBlend != ZERO) is
   replaced by a copy whose alpha part is  A = Asrc * ONE + Adst * INV_SRC_ALPHA  (coverage, "over"). Colour blending is not
   touched, opaque states (BlendEnable 0) already write the source alpha, and the eye halves are shown by an opaque
   projection layer, so the picture does not change; the UI column ends up premultiplied: colour = UI over black,
   alpha = coverage, which is what the compositor's alpha-blended quad layer expects.

   How (no recompiled code changes; same mechanism as linux/rhi_hook.c): BSI calls the D3D11 RHI only through the vtable of
   GDynamicRHI ([0x13a17dc], vtable 0x1183290). On first use this file copies the object's CURRENT table (so it chains
   behind rhi_hook's census/native table when that one is installed first) into guest memory and points four slots at host
   thunks (extended import entries, g_ximps):
     61  GetViewportBackBuffer(FSurfaceRHIRef *out, Viewport): remembers the back buffer surface (*out after the call; also
         learnt from the RHI's drawing viewport, RHI+0x5c -> +0x24, the first time it is bound);
     114 SetRenderTargets(rt0, rt1, rt2, rt3, depth): tracks "back buffer bound"; on a change re-applies the current blend
         state and colour-write mask so nothing leaks into other targets;
     112 SetBlendState(state): FD3D11BlendState = { vtbl 0x118314c, refs, D3D11_BLEND_DESC @+8 (RT0 @+0x10: BlendEnable,
         Src, Dest, Op, SrcAlpha @+0x20, DestAlpha @+0x24, OpAlpha @+0x28), blend factor @+0x110 }; the slot copies the desc
         (0x108 bytes) and the factor into the RHI and builds the D3D11 state from them (f_0089c620), so the substitute is
         one scratch copy in guest memory, rewritten per call;
     116 SetColorWriteMask(mask): forces the alpha bit while the back buffer is bound (if any colour bit is set).
   With BSI_RHI set (rhi_hook.c), installation waits until rhi_hook has replaced the original table (else rhi_hook would
   never install: it only installs over the .rdata table). Off: one flag test per slot call. */
#define _GNU_SOURCE
#include "hle.h"
#include <time.h>

#define UR_GLOBAL 0x13a17dcu
#define UR_VTBL   0x1183290u
#define UR_N      232
#define UR_BS_VTBL 0x118314cu
#define UR_IMP_BASE 0xE0000000u
enum { S_GETBB = 61, S_SETBLEND = 112, S_SETRT = 114, S_CWMASK = 116 };

typedef void (*ur_fn)(CPU*c);
typedef struct { uint32_t iat; const char *name; ur_fn fn; } UrImp;   /* = Imp in hle_dll.c */
extern UrImp g_ximps[]; extern int g_nximps; extern int g_nimps;
void call_indirect(CPU*c, uint32_t t);

static volatile int g_want;              /* GT: UI layer with ui_alpha=rhi this frame */
static int g_state;                      /* 0 not installed, 1 installed, -1 gave up */
static uint32_t g_obj, g_vptr, g_orig[UR_N], g_magic[4];
static uint32_t g_bb;                    /* back buffer surface param (FSurfaceRHIParamRef = object + 8) */
static int g_bound;                      /* back buffer is render target 0 */
static uint32_t g_last_bs, g_last_mask = 0xF, g_clone;
static unsigned long g_n_over, g_n_pass, g_n_rebind; static double g_next_log; static int g_waits;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int okp(uint32_t a){ return a >= 0x10000u && a < 0xC0000000u; }

/* the substitute for one blend state, or 0 = use it as it is */
uint32_t vr_ui_rhi_blend_subst(uint32_t param){
  if(!okp(param)) return 0;
  uint32_t o = param - 8;
  if(R32(o) != UR_BS_VTBL) return 0;
  uint32_t en = R32(o + 0x10), dst = R32(o + 0x18);
  if(!en || dst == 1 /* D3D11_BLEND_ZERO */) return 0;
  if(R32(o + 0x20) == 2 && R32(o + 0x24) == 6 && R32(o + 0x28) == 1) return 0;   /* already ONE / INV_SRC_ALPHA / ADD */
  if(!g_clone){ uint32_t p = gmalloc(0x120 + 16); if(!p) return 0; g_clone = (p + 15) & ~15u; }
  memcpy(MEMP(g_clone), MEMP(o), 0x120);
  W32(g_clone + 0x20, 2); W32(g_clone + 0x24, 6); W32(g_clone + 0x28, 1);   /* SrcBlendAlpha ONE, DestBlendAlpha INV_SRC_ALPHA, ADD */
  return g_clone + 8;
}
/* thiscall slot call from inside a thunk (RT), one dword argument */
static void call_slot1(CPU*c, int slot, uint32_t arg){
  CPU s = *c;
  c->ecx = g_obj; PUSH32(c, arg); PUSH32(c, R32(s.esp)); call_indirect(c, g_orig[slot]);
  *c = s;
}
static void apply_blend(CPU*c){
  if(!g_last_bs) return;
  uint32_t a = g_bound && g_want ? vr_ui_rhi_blend_subst(g_last_bs) : 0;
  call_slot1(c, S_SETBLEND, a ? a : g_last_bs);
}
static void apply_mask(CPU*c){
  uint32_t m = g_last_mask; if(g_bound && g_want && (m & 7)) m |= 8;
  call_slot1(c, S_CWMASK, m);
}

static void t_getbb(CPU*c){
  uint32_t out = R32(c->esp + 4);
  call_indirect(c, g_orig[S_GETBB]);
  if(okp(out)){ uint32_t bb = R32(out); if(bb && bb != g_bb){ g_bb = bb; fprintf(stderr, "[vr_ui_rhi] back buffer surface 0x%08x\n", bb); } }
}
static void t_setrt(CPU*c){
  uint32_t rt0 = R32(c->esp + 4), esp0 = c->esp;
  call_indirect(c, g_orig[S_SETRT]);
  if(!g_bb && okp(rt0)){   /* not seen slot 61 yet: the RHI's drawing viewport (+0x5c) keeps its back buffer object at +0x24
                              (slot 114 compares rt0 with it at 0x89d3fc..0x89d40b) */
    uint32_t dv = R32(g_obj + 0x5c); if(okp(dv)){ uint32_t bo = R32(dv + 0x24); if(okp(bo) && rt0 == bo + 8){ g_bb = rt0; fprintf(stderr, "[vr_ui_rhi] back buffer surface 0x%08x (drawing viewport)\n", rt0); } } }
  int b = g_bb && rt0 == g_bb;
  if(b != g_bound){
    g_bound = b;
    if(g_want){ CPU s = *c; c->esp = esp0; apply_blend(c); apply_mask(c); *c = s; g_n_rebind++; } }
}
static void t_setblend(CPU*c){
  uint32_t st = R32(c->esp + 4); g_last_bs = st;
  if(g_bound && g_want){ uint32_t a = vr_ui_rhi_blend_subst(st); if(a){ W32(c->esp + 4, a); g_n_over++; } else g_n_pass++; }
  call_indirect(c, g_orig[S_SETBLEND]);
  double t; if(g_want && (t = now_s()) >= g_next_log){ g_next_log = t + 10.0;
    fprintf(stderr, "[vr_ui_rhi] UI blend states with coverage alpha: %lu, unchanged: %lu, rebinds %lu\n", g_n_over, g_n_pass, g_n_rebind);
    g_n_over = g_n_pass = g_n_rebind = 0; }
}
static void t_cwmask(CPU*c){
  uint32_t m = R32(c->esp + 4) & 0xFF; g_last_mask = m & 0xF;
  if(g_bound && g_want && (m & 7)) W32(c->esp + 4, (R32(c->esp + 4) & ~0xFFu) | ((m | 8) & 0xFF));
  call_indirect(c, g_orig[S_CWMASK]);
}

static int install(void){
  uint32_t obj = R32(UR_GLOBAL); if(!okp(obj)) return 0;
  uint32_t vt = R32(obj); if(!okp(vt)) return 0;
  const char *rh = getenv("BSI_RHI");
  if(rh && *rh && vt == UR_VTBL && g_waits++ < 600) return 0;   /* let rhi_hook.c install first (it needs the .rdata table) */
  if(!g_magic[0]){
    int x = __atomic_load_n(&g_nximps, __ATOMIC_ACQUIRE);
    if(x + 4 > 4096){ fprintf(stderr, "[vr_ui_rhi] no room in the extended import table: UI alpha off\n"); g_state = -1; return 0; }
    static const char *nm[4] = { "vr_ui_rhi!GetViewportBackBuffer", "vr_ui_rhi!SetBlendState", "vr_ui_rhi!SetRenderTargets", "vr_ui_rhi!SetColorWriteMask" };
    ur_fn fn[4] = { t_getbb, t_setblend, t_setrt, t_cwmask };
    for(int k = 0; k < 4; k++){ g_ximps[x + k].iat = 0; g_ximps[x + k].name = nm[k]; g_ximps[x + k].fn = fn[k]; }
    __atomic_store_n(&g_nximps, x + 4, __ATOMIC_RELEASE);
    for(int k = 0; k < 4; k++) g_magic[k] = UR_IMP_BASE + 4u * (uint32_t)(g_nimps + x + k);
  }
  for(int s = 0; s < UR_N; s++) g_orig[s] = R32(vt + 4 * s);
  uint32_t t = gmalloc(4 * (UR_N + 1)); if(!t){ g_state = -1; return 0; }
  W32(t, R32(vt - 4));
  for(int s = 0; s < UR_N; s++) W32(t + 4 + 4 * s, g_orig[s]);
  W32(t + 4 + 4 * S_GETBB, g_magic[0]); W32(t + 4 + 4 * S_SETBLEND, g_magic[1]);
  W32(t + 4 + 4 * S_SETRT, g_magic[2]); W32(t + 4 + 4 * S_CWMASK, g_magic[3]);
  g_vptr = t + 4; g_obj = obj;
  __atomic_store_n((uint32_t*)MEMP(obj), g_vptr, __ATOMIC_RELEASE);
  fprintf(stderr, "[vr_ui_rhi] coverage alpha for the UI column: RHI object %08x vptr %08x -> %08x (chained behind %s)\n", obj, vt, g_vptr,
          vt == UR_VTBL ? "the original table" : "another hook's table");
  return 1;
}

/* game thread, once per Draw with the UI column (vr_ui.c) */
void vr_ui_rhi_frame(int on){
  g_want = on;
  if(!on || g_state < 0) return;
  if(g_state == 0){ if(install()) g_state = 1; return; }
  uint32_t obj = R32(UR_GLOBAL);   /* the game re-created the RHI, or another hook replaced ours: chain again */
  if(obj != g_obj || (okp(obj) && R32(obj) != g_vptr)){ g_waits = 0; if(install()) fprintf(stderr, "[vr_ui_rhi] re-installed\n"); }
}

/* ---- vr71: late head-pose latch hook (notes/VR_STEREO.md §19; the latch itself is vr_stereo.c vr_stereo_late_latch) ----
   BeginRenderingViewFamily (f_007402e0, game thread) builds the FSceneRenderer (0xE0 bytes, ctor f_0073b270: views copied
   into FViewInfos at +0x50) and enqueues a render command whose vtable is 0x1163a94; the rendering thread calls its slot 2
   (Execute, 0x73f640: mov eax,[ecx+4]; push eax; call f_0073f510 = RenderViewFamily_RenderThread -> FSceneRenderer::Render)
   about one game frame later. The slot (in .rdata, one class) is pointed at a host thunk (extended import entry, as above):
   the thunk lets vr_stereo.c re-pose the views, then runs the original Execute unchanged. Installed once, from the game
   thread, at the first stereo pair with late=1; a slot that does not hold 0x73f640 (other build) is left alone. With
   late=0 (live switch) or no stereo the thunk only forwards: one weak-pointer test and the vr_stereo.c early return. */
#define LL_VT     0x1163a94u
#define LL_EXEC   0x0073f640u
int vr_stereo_late_latch(uint32_t scene_renderer) __attribute__((weak));
static uint32_t g_ll_magic; static int g_ll_state;   /* 0 not tried, 1 installed, -1 refused */
static unsigned long g_ll_calls;
void b1dx_vr_scene_begin(uint32_t n, const int32_t *x) __attribute__((weak));   /* rhi_b1 (per-eye AO bounds) */
static void t_draw_scene(CPU*c){
  uint32_t cmd = c->ecx; g_ll_calls++;
  if(okp(cmd) && b1dx_vr_scene_begin){   /* the scene's views (FSceneRenderer +0x50 FViewInfo[0x11A0], X float at +0x34) */
    uint32_t sr = R32(cmd + 4), base = okp(sr) ? R32(sr + 0x50) : 0, n = okp(sr) ? R32(sr + 0x54) : 0; int32_t xs[4] = { 0, 0, 0, 0 };
    if(!okp(base) || n > 4) n = 0;
    for(uint32_t i = 0; i < n; i++){ float fx; memcpy(&fx, MEMP(base + i * 0x11A0u + 0x34), 4); xs[i] = (int32_t)(fx + (fx >= 0 ? 0.5f : -0.5f)); }
    b1dx_vr_scene_begin(n, xs); }
  if(okp(cmd) && vr_stereo_late_latch) vr_stereo_late_latch(R32(cmd + 4));
  { extern void b1dx_vr_aa_scene(uint8_t *vi, uint32_t n) __attribute__((weak));   /* aa76: TAA jitter + per-eye matrices (rhi_b1_aa.inc), after the latch */
    if(okp(cmd) && b1dx_vr_aa_scene){ uint32_t sr = R32(cmd + 4), base = okp(sr) ? R32(sr + 0x50) : 0, n = okp(sr) ? R32(sr + 0x54) : 0;
      if(okp(base) && n >= 1 && n <= 4) b1dx_vr_aa_scene((uint8_t *)MEMP(base), n); } }
  { extern void ext72_scene_views(uint32_t) __attribute__((weak)); if(okp(cmd) && ext72_scene_views) ext72_scene_views(R32(cmd + 4)); }   /* ext72: legendary loot beam (legendary.c), after the late latch */
  call_indirect(c, LL_EXEC);
}
int vr_late_install(void){
  if(g_ll_state) return g_ll_state > 0;
  uint32_t cur = R32(LL_VT + 8);
  if(cur != LL_EXEC){ g_ll_state = -1; fprintf(stderr, "[vr_late] FDrawSceneCommand vtable slot 2 holds 0x%08x, not 0x%08x: late latch off\n", cur, LL_EXEC); return 0; }
  int x = __atomic_load_n(&g_nximps, __ATOMIC_ACQUIRE);
  if(x + 1 > 4096){ g_ll_state = -1; fprintf(stderr, "[vr_late] no room in the extended import table: late latch off\n"); return 0; }
  g_ximps[x].iat = 0; g_ximps[x].name = "vr_late!FDrawSceneCommand::Execute"; g_ximps[x].fn = t_draw_scene;
  __atomic_store_n(&g_nximps, x + 1, __ATOMIC_RELEASE);
  g_ll_magic = UR_IMP_BASE + 4u * (uint32_t)(g_nimps + x);
  __atomic_store_n((uint32_t*)MEMP(LL_VT + 8), g_ll_magic, __ATOMIC_RELEASE);
  g_ll_state = 1;
  fprintf(stderr, "[vr_late] late head-pose latch hook installed: FDrawSceneCommand::Execute (vtable 0x%08x slot 2, 0x%08x) -> host thunk 0x%08x\n", LL_VT, LL_EXEC, g_ll_magic);
  return 1;
}
