/* rhi_b1.c -- B1 native renderer front end, guest side (work log "B1 plan"). BSI_RHI=b1 (off unless set).

   B1.2 = shadow mode. linux/rhi_hook.c installs its 232-slot table early (the first bridged COM call after RHIInit stored
   GDynamicRHI at 0x536029), so the D3D11 viewport, its swapchain and every resource are created after our table is in
   place. Every slot still runs its original body first (the bridge renders and presents exactly as before); this file
   then mirrors the call onto our own DXVK device (rhi_b1_dxvk.cpp), which renders into offscreen targets only.
   Later B1 steps add slot after slot to the mirror; B1.6 flips: the originals stop, the mirror presents.

   B1.2 mirror: CreateViewport (68) / ResizeViewport (69) record the size; EndDrawingViewport (59) = one shadow frame
   (clear + submit). Every 10 s on the Present path: one "[b1]" line (game frames, shadow frames, viewport size).
   Arguments are read BEFORE the original body runs (it pops them: thiscall with `ret N`). */
#include "com.h"
#include "hle.h"
#include <time.h>
#include <pthread.h>
extern __thread uint32_t t_tid;

int  b1dx_init(char *msg, int n);
uint32_t b1dx_backbuffer(uint32_t w, uint32_t h);
void b1dx_set_rts(const uint32_t *ids, uint32_t ds);
void b1dx_set_viewport(uint32_t x0, uint32_t y0, float z0, uint32_t x1, uint32_t y1, float z1);
void b1dx_clear(int color, const float *rgba, int depth, float d, int stencil, uint32_t st);
int  b1dx_end_frame(void);
void b1dx_render_stats(char *out, int n);
long b1dx_frames(void);
uint32_t b1dx_buffer_create(uint32_t size, uint32_t bind, const void *data);
int  b1dx_buffer_write(uint32_t id, uint32_t offset, uint32_t size, const void *data);
uint32_t b1dx_tex2d_create(uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, uint32_t dxgi, uint32_t bind, uint32_t misc,
                           const void *const *subdata, const uint32_t *subpitch);
int  b1dx_tex_write(uint32_t id, uint32_t mip, uint32_t layer, const void *data, uint32_t pitch);
int  b1dx_tex_write_rect(uint32_t id, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data, uint32_t pitch);
void b1dx_release(uint32_t id);
void b1dx_upload_flush(void);
void b1dx_res_stats(char *out, int n);
uint32_t b1dx_shader_create(int stage, const void *code, uint32_t len, int compile_now);
void b1dx_shader_release(uint32_t id);
void b1dx_shader_stats(char *out, int n);

int b1com_active(void); void b1com_create_pre(const char *name, CPU *c); void b1com_stats(char *out, int n); void b1dx_com_stats(char *out, int n);   /* rhi_b1_com.c */
static int g_b1_ok;   /* defined below (-1 not tried, 0 failed, 1 up) */
/* ---- B1.3 resources: guest handle -> shadow ids (open addressing; key 0 = empty, ~0u = deleted) ---- */
#define B1H_N (1u << 17)
typedef struct { uint32_t h, id[2], kind; uint32_t lptr, loff, lsize, lmip, llayer, lpitch, lwrite; } B1Res;
static B1Res g_res[B1H_N];
static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;
static long g_res_full, g_unmapped_locks, g_upd2d, g_freed;
static B1Res *res_find(uint32_t h, int create) {
  uint32_t i = (h * 2654435761u) >> 15, tomb = ~0u;
  for (uint32_t n = 0; n < B1H_N; n++, i = (i + 1) & (B1H_N - 1)) {
    uint32_t k = g_res[i].h;
    if (k == h) return &g_res[i];
    if (k == ~0u) { if (tomb == ~0u) tomb = i; continue; }
    if (k == 0) { if (!create) return 0; if (tomb != ~0u) i = tomb; memset(&g_res[i], 0, sizeof g_res[i]); g_res[i].h = h; return &g_res[i]; }
  }
  if (create && tomb != ~0u) { memset(&g_res[tomb], 0, sizeof g_res[tomb]); g_res[tomb].h = h; return &g_res[tomb]; }
  g_res_full++; return 0;
}
/* creations seen in the bridge while a b1 creation slot runs (per thread) */
static __thread uint32_t t_pend[4]; static __thread int t_npend, t_capture, t_pend_kind;
static void id_release(uint32_t kind, uint32_t id) { if (!id || kind == 3) return; if (kind == 2) b1dx_shader_release(id); else b1dx_release(id); }

/* bridge.c bridge_pre: every bridged D3D11 call (name = "ID3D11Device::CreateBuffer" ...); args at esp+4 = this */
void rhi_b1_capture(const char *name, CPU *c) {
  if (b1com_active()) { b1com_create_pre(name, c); return; }   /* B1.5: the COM seam owns resources (rhi_b1_com.c) */
  if (!t_capture || g_b1_ok != 1 || name[0] != 'I' || strncmp(name, "ID3D11Device::Create", 20)) return;
  const char *m = name + 20; uint32_t id = 0;
  if (!strcmp(m, "Buffer")) {
    uint32_t d = R32(c->esp + 8), init = R32(c->esp + 12);
    uint32_t size = R32(d), bind = R32(d + 8), sys = init ? R32(init) : 0;
    id = b1dx_buffer_create(size, bind, sys ? MEMP(sys) : 0);
  } else if (!strcmp(m, "Texture2D")) {
    uint32_t d = R32(c->esp + 8), init = R32(c->esp + 12);
    uint32_t w = R32(d), h = R32(d + 4), mips = R32(d + 8), arr = R32(d + 12), fmt = R32(d + 16), smp = R32(d + 20);
    uint32_t bind = R32(d + 32), misc = R32(d + 40);
    if (smp > 1) return;   /* MSAA: later */
    const void *sub[256]; uint32_t pitch[256]; uint32_t n = (mips ? mips : 1) * (arr ? arr : 1);
    int have = init && mips && n <= 256;
    if (have) for (uint32_t k = 0; k < n; k++) { uint32_t p = R32(init + 12 * k); sub[k] = p ? MEMP(p) : 0; pitch[k] = R32(init + 12 * k + 4); }
    id = b1dx_tex2d_create(w, h, mips, arr, fmt, bind, misc, have ? sub : 0, have ? pitch : 0);
  } else if (!strcmp(m, "VertexShader") || !strcmp(m, "PixelShader") || !strcmp(m, "GeometryShader") ||
             !strcmp(m, "HullShader") || !strcmp(m, "DomainShader") || !strcmp(m, "ComputeShader")) {
    int st = m[0] == 'V' ? 0 : m[0] == 'P' ? 1 : m[0] == 'G' ? 2 : m[0] == 'H' ? 3 : m[0] == 'D' ? 4 : 5;
    static int now = -1; if (now < 0) { const char *e = getenv("BSI_B1_COMPILE"); now = e ? atoi(e) : 1; }
    uint32_t code = R32(c->esp + 8), len = R32(c->esp + 12);
    id = b1dx_shader_create(st, code ? MEMP(code) : 0, len, now);
    if (id) t_pend_kind = 2;
  } else return;
  if (id && t_npend < 4) t_pend[t_npend++] = id;
}
static int is_create_slot(int s) { return s == 19 || s == 22 || s == 26 || s == 27 || s == 44 || s == 47 || s == 48 || s == 150
                                         || s == 8 || s == 9 || s == 10 || s == 11 || s == 15 || s == 16; }   /* + shaders (B1.4) */
static void res_bind(uint32_t h, int slot) {     /* after a creation slot: pending shadow ids belong to guest handle h */
  if (!t_npend) {   /* no D3D11 creation inside: remember the handle (kind 4, id[1] = creating slot) for diagnostics */
    if (!h || t_pend_kind) return;
    pthread_mutex_lock(&g_res_mu); B1Res *r = res_find(h, 1);
    if (r && (r->kind == 0 || r->kind == 4)) { r->kind = 4; r->id[0] = 0; r->id[1] = (uint32_t)slot; }
    pthread_mutex_unlock(&g_res_mu); return; }
  uint32_t kind = t_pend_kind ? t_pend_kind : 1;
  if (!h) { for (int k = 0; k < t_npend; k++) id_release(kind, t_pend[k]); t_npend = 0; return; }
  pthread_mutex_lock(&g_res_mu);
  B1Res *r = res_find(h, 1);
  if (r) { id_release(r->kind, r->id[0]); id_release(r->kind, r->id[1]);
           r->id[0] = t_pend[0]; r->id[1] = t_npend > 1 ? t_pend[1] : 0; r->kind = kind; r->lptr = 0; }
  else for (int k = 0; k < t_npend; k++) id_release(kind, t_pend[k]);
  pthread_mutex_unlock(&g_res_mu);
  t_npend = 0;
}
static void res_lock(uint32_t h, uint32_t ptr, uint32_t off, uint32_t size, uint32_t mip, uint32_t layer, uint32_t pitch, uint32_t write) {
  pthread_mutex_lock(&g_res_mu);
  B1Res *r = res_find(h, 0);
  if (r && r->id[0]) { r->lptr = ptr; r->loff = off; r->lsize = size; r->lmip = mip; r->llayer = layer; r->lpitch = pitch; r->lwrite = write; }
  else g_unmapped_locks++;
  pthread_mutex_unlock(&g_res_mu);
}
static void res_unlock(uint32_t h, int tex) {   /* BEFORE the original Unlock: the guest pointer is still valid */
  pthread_mutex_lock(&g_res_mu);
  B1Res *r = res_find(h, 0);
  if (r && r->kind == 1 && r->id[0] && r->lptr && r->lwrite) {
    if (tex) b1dx_tex_write(r->id[0], r->lmip, r->llayer, MEMP(r->lptr), r->lpitch);
    else b1dx_buffer_write(r->id[0], r->loff, r->lsize, MEMP(r->lptr));
  }
  if (r) r->lptr = 0;
  pthread_mutex_unlock(&g_res_mu);
}
static void res_release_check(uint32_t h, uint32_t before) {   /* after a RemoveResourceRef slot; before = [h-4] read before it */
  if (!h || before != 1) return;   /* the guest refcount at [h-4] was 1, so this release deleted the object */
  pthread_mutex_lock(&g_res_mu);
  B1Res *r = res_find(h, 0);
  if (r) { id_release(r->kind, r->id[0]); id_release(r->kind, r->id[1]); r->h = ~0u; g_freed++; }
  pthread_mutex_unlock(&g_res_mu);
}

/* BSI_B1_INV=1: which D3D11 COM calls each RHI slot issues (caller thread, before the bridge/queue) -> printed once */
static __thread int t_slot = -1;
typedef struct { int16_t slot; uint16_t gi; uint32_t n; } B1Inv;
static B1Inv g_inv[1 << 14]; static int g_inv_on = -1, g_inv_done;
void b1_inv_count(CPU *c, int gi) {
  (void)c;
  if (g_inv_on < 0) g_inv_on = getenv("BSI_B1_INV") != 0;
  if (!g_inv_on || g_inv_done || gi >= 65536) return;
  uint32_t key = ((uint32_t)(t_slot + 1) << 16) | (uint32_t)gi, i = (key * 2654435761u) >> 18;
  for (int n = 0; n < (1 << 14); n++, i = (i + 1) & ((1 << 14) - 1)) {
    if (g_inv[i].n && g_inv[i].slot == t_slot && g_inv[i].gi == gi) { __atomic_fetch_add(&g_inv[i].n, 1, __ATOMIC_RELAXED); return; }
    if (!g_inv[i].n) { g_inv[i].slot = t_slot; g_inv[i].gi = gi; __atomic_store_n(&g_inv[i].n, 1, __ATOMIC_RELEASE); return; }   /* racy insert: counts only */
  }
}
static void inv_dump(void) {
  g_inv_done = 1;
  for (int i = 0; i < (1 << 14); i++) if (g_inv[i].n)
    fprintf(stderr, "[b1inv] slot %4d  %-55s %u\n", g_inv[i].slot, g_inv[i].gi < g_com_nmethods ? g_com_methods[g_inv[i].gi].name : "?", g_inv[i].n);
}
static int g_b1_ok = -1;               /* -1 device not tried yet, 0 failed, 1 up */
static uint32_t g_vp_w, g_vp_h, g_vp_obj, g_bb_id;
static long g_rt_unmapped, g_unm_why[256];   /* why: 0 unknown handle, slot = created by that slot without a D3D11 resource */
static uint32_t res_id(uint32_t h) {   /* guest surface/texture handle -> shadow id (0 = none/unknown) */
  if (!h) return 0;
  pthread_mutex_lock(&g_res_mu); B1Res *r = res_find(h, 0); uint32_t id = (r && r->kind != 2) ? r->id[0] : 0;
  int why = !r ? 0 : r->kind == 4 ? (int)(r->id[1] & 255) : 255; pthread_mutex_unlock(&g_res_mu);
  if (!id) { g_rt_unmapped++; __atomic_fetch_add(&g_unm_why[why], 1, __ATOMIC_RELAXED); }
  return id;
}
static long g_end_vp, g_slot_calls;

static pthread_mutex_t g_dev_mu = PTHREAD_MUTEX_INITIALIZER;
static void b1_dev_once_locked(void);
static void b1_dev_once(void) { if (__atomic_load_n(&g_b1_ok, __ATOMIC_ACQUIRE) >= 0) return; pthread_mutex_lock(&g_dev_mu); b1_dev_once_locked(); pthread_mutex_unlock(&g_dev_mu); }
int rhi_b1_dev(void) { b1_dev_once(); return g_b1_ok == 1; }
static void b1_dev_once_locked(void) {
  if (g_b1_ok >= 0) return;
  char msg[256]; g_b1_ok = b1dx_init(msg, sizeof msg);
  fprintf(stderr, "[b1] shadow device %s: %s\n", g_b1_ok ? "up" : "FAILED (shadow off, game unaffected)", msg);
}

/* rhi_hook.c calls this for every RHI slot when BSI_RHI has "b1" (orig = original slot body) */
void rhi_b1_slot(CPU *c, int s, uint32_t orig) {
  uint32_t a[8]; for (int k = 0; k < 8; k++) a[k] = R32(c->esp + 4 + 4 * k);
  { static int tr = -1; static unsigned char seen[256]; if (tr < 0) tr = getenv("BSI_B1_TRACE") != 0;
    if (tr && s < 256 && !seen[s]) { seen[s] = 1; fprintf(stderr, "[b1] first call: slot %d args %08x %08x %08x %08x %08x\n", s, a[0], a[1], a[2], a[3], a[4]); } }
  static long ntr = -1; if (ntr < 0) { const char *e = getenv("BSI_B1_TRACE"); ntr = (e && atoi(e) >= 2) ? 4000 : 0; }
  int trace_this = ntr > 0 && __atomic_fetch_sub(&ntr, 1, __ATOMIC_RELAXED) > 0;
  if (trace_this) fprintf(stderr, "[b1] > slot %d tid %u ecx %08x args %08x %08x %08x %08x %08x\n", s, t_tid, c->ecx, a[0], a[1], a[2], a[3], a[4]);
  int com = b1com_active();
  if (g_b1_ok == 1 && !com) {   /* pre-hooks: data written into a lock must be copied before the original Unlock runs */
    switch (s) {
    case 21: case 24: res_unlock(a[0], 0); break;          /* UnlockIndexBuffer / UnlockVertexBuffer (Buffer) */
    case 34: case 36: case 46: res_unlock(a[0], 1); break; /* UnlockTexture2D / 2DArray / CubeFace */
    }
  }
  uint32_t rc_before = (s >= 182 && s <= 206 && a[0] > 4) ? R32(a[0] - 4) : 0;
  int cap = g_b1_ok == 1 && !com && is_create_slot(s);
  if (cap) { t_npend = 0; t_pend_kind = 0; t_capture++; }
  int prev_slot = t_slot; t_slot = s;
  call_indirect(c, orig);
  t_slot = prev_slot;
  if (cap) t_capture--;
  if (trace_this) fprintf(stderr, "[b1] < slot %d eax %08x\n", s, c->eax);
  if (g_b1_ok == 1 && !com) {
    if (cap) res_bind(c->eax ? R32(c->eax) : 0, s);   /* creation slots return the hidden ref pointer; the ref holds the handle */
    switch (s) {
    case 20: res_lock(a[0], c->eax, a[1], a[2], 0, 0, 0, 1); break;              /* LockIndexBuffer(IB, Offset, Size) */
    case 23: res_lock(a[0], c->eax, a[1], a[2], 0, 0, 0, !a[3]); break;          /* LockVertexBuffer(VB, Offset, Size, bReadOnly) */
    case 33: res_lock(a[0], c->eax, 0, 0, a[1], 0, a[3] ? R32(a[3]) : 0, a[2]); break;   /* LockTexture2D(T, Mip, bWrite, &Stride, bMiptail) */
    case 35: case 45: res_lock(a[0], c->eax, 0, 0, a[2], a[1], a[4] ? R32(a[4]) : 0, a[3]); break;   /* LockTextureCubeFace(T, Face, Mip, bWrite, &Stride, ..) */
    case 37: {   /* UpdateTexture2D(T, Mip, NumRegions, Regions[{DestX,DestY,SrcX,SrcY,W,H}], SrcPitch, SrcBpp, SrcData): guest data stays valid */
      pthread_mutex_lock(&g_res_mu); B1Res *r = res_find(a[0], 0); uint32_t id = r ? r->id[0] : 0; pthread_mutex_unlock(&g_res_mu);
      if (id && a[3]) {
        uint32_t srcdata = a[6], pitch = a[4], bpp = a[5];
        for (uint32_t k = 0; k < a[2] && k < 64; k++) { uint32_t g = a[3] + 24 * k;
          uint32_t dx = R32(g), dy = R32(g + 4), sx = R32(g + 8), sy = R32(g + 12), w = R32(g + 16), h = R32(g + 20);
          if (srcdata) b1dx_tex_write_rect(id, a[1], 0, dx, dy, w, h, MEMP(srcdata + sy * pitch + sx * bpp), pitch); }
      }
      g_upd2d++; break; }
    default: if (s >= 182 && s <= 206) res_release_check(a[0], rc_before); break;   /* RemoveResourceRef(*) */
    }
  }
  uint32_t ret = c->eax;
  __atomic_fetch_add(&g_slot_calls, 1, __ATOMIC_RELAXED);
  switch (s) {
  case 68:   /* CreateViewport: [hidden ret ptr], WindowHandle, SizeX, SizeY, bIsFullscreen -> ret = ptr to the new ref */
    g_vp_w = a[2]; g_vp_h = a[3]; g_vp_obj = ret ? R32(ret) : 0;
    fprintf(stderr, "[b1] CreateViewport hwnd %08x %ux%u fullscreen %u -> viewport %08x\n", a[1], a[2], a[3], a[4], g_vp_obj);
    b1_dev_once();
    if (g_b1_ok == 1) g_bb_id = b1dx_backbuffer(g_vp_w, g_vp_h);
    break;
  case 69:   /* ResizeViewport: Viewport, SizeX, SizeY, bIsFullscreen */
    if (a[0] == g_vp_obj || !g_vp_obj) { g_vp_w = a[1]; g_vp_h = a[2]; if (g_b1_ok == 1) g_bb_id = b1dx_backbuffer(g_vp_w, g_vp_h); }
    fprintf(stderr, "[b1] ResizeViewport %08x %ux%u fullscreen %u\n", a[0], a[1], a[2], a[3]);
    break;
  case 61:   /* GetViewportBackBuffer: [hidden ret ptr], Viewport -> ref holds the back-buffer surface handle */
    if (g_b1_ok == 1 && !com && ret && g_bb_id) { uint32_t h = R32(ret);
      if (h) { pthread_mutex_lock(&g_res_mu); B1Res *r = res_find(h, 1);
               if (r && r->id[0] != g_bb_id) { id_release(r->kind, r->id[0]); id_release(r->kind, r->id[1]); r->id[0] = g_bb_id; r->id[1] = 0; r->kind = 3; }
               pthread_mutex_unlock(&g_res_mu); } }
    break;
  case 114:  /* SetRenderTargets(RT0..RT3, DepthStencil) */
    if (g_b1_ok == 1 && !com) { uint32_t ids[4]; for (int k = 0; k < 4; k++) ids[k] = res_id(a[k]); b1dx_set_rts(ids, res_id(a[4])); }
    break;
  case 74: { /* SetViewport(MinX, MinY, MinZ(float), MaxX, MaxY, MaxZ(float)) */
    float z0, z1; memcpy(&z0, &a[2], 4); memcpy(&z1, &a[5], 4);
    if (g_b1_ok == 1 && !com) b1dx_set_viewport(a[0], a[1], z0, a[3], a[4], z1);
    break; }
  case 137: { /* Clear(bColor, const FLinearColor*, bDepth, float Depth, bStencil, Stencil) */
    float d; memcpy(&d, &a[3], 4);
    if (g_b1_ok == 1 && !com) b1dx_clear(a[0] & 0xff, a[1] ? (const float *)MEMP(a[1]) : 0, a[2] & 0xff, d, a[4] & 0xff, a[5]);
    break; }
  case 59:   /* EndDrawingViewport: Viewport, bPresent, bLockToVsync -> one shadow frame */
    g_end_vp++;
    b1_dev_once();
    if (g_b1_ok == 1 && !com) b1dx_end_frame();   /* COM mode: IDXGISwapChain::Present ends the frame */
    break;
  default:
    break;
  }
}

/* rhi_hook.c, every swapchain Present (RenderingThread) */
void rhi_b1_tick(double now) {
  static double last;
  if (last == 0) { last = now; return; }
  if (now - last < 10.0) return;
  last = now;
  if (g_inv_on > 0 && !g_inv_done) { static long th = -1; if (th < 0) { const char *e = getenv("BSI_B1_INV"); th = e && atol(e) > 1 ? atol(e) : 4000; } if (g_end_vp > th) inv_dump(); }
  { char w[256]; int o = 0; w[0] = 0; for (int k = 0; k < 256 && o < 200; k++) if (g_unm_why[k]) o += snprintf(w + o, sizeof w - o, " %s%d:%ld", k == 0 ? "unknown" : k == 255 ? "other" : "slot", k == 0 || k == 255 ? 0 : k, g_unm_why[k]);
    if (o) fprintf(stderr, "[b1] unmapped RT handles by origin:%s\n", w); }
  char rs[256], ss[128], vs[192]; rs[0] = ss[0] = vs[0] = 0; if (g_b1_ok == 1) { b1dx_res_stats(rs, sizeof rs); b1dx_shader_stats(ss, sizeof ss); b1dx_render_stats(vs, sizeof vs); }
  if (g_b1_ok == 1 && b1com_active()) { char a[400], b[640]; b1com_stats(a, sizeof a); b1dx_com_stats(b, sizeof b); fprintf(stderr, "[b1] %s | %s\n", a, b); }
  fprintf(stderr, "[b1] shadow: %ld EndDrawingViewport, %ld shadow frames, viewport %ux%u, %ld RHI calls mirrored, device %s | %s | %s | %s, unmapped RT handles %ld | freed %ld, locks on unknown handles %ld, UpdateTexture2D %ld%s\n",
          g_end_vp, b1dx_frames(), g_vp_w, g_vp_h, g_slot_calls, g_b1_ok == 1 ? "up" : g_b1_ok == 0 ? "failed" : "not started",
          rs, ss, vs, g_rt_unmapped, g_freed, g_unmapped_locks, g_upd2d, g_res_full ? " HANDLE TABLE FULL" : "");
}
