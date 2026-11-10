/* vr_ui (VR stereo, playability step 1; notes/VR_STEREO.md §11): the HUD, menus, subtitles and console as their own layer.

   Problem: with the stereo back buffer [ left | right ], UGameViewportClient::Draw (f_0066a3e0) lays the UI out over the
   whole buffer: the HUD canvas gets view 0's rect (left eye only), Scaleform movies get the full FViewport (cut in half,
   one half per eye), subtitles and the console use the full render-target size.

   Route chosen: a third column in the SAME back buffer, [ left | right | UI ], and every UI producer is told (at game
   level) that its screen is a 16:9 rect in that column. xr_glue then shows that rect as a quad layer (head-locked, world-
   locked or lazily following) over the projection layer, from the very same swapchain image (route A: imageRect; route B:
   copied into its own swapchain). Why not a separate render target redirected at the RHI level:
     - the compositor only ever receives the presented image (route A: the game renders straight into the compositor's
       Android-surface swapchain; route B: the AImageReader only gets Presents), so a separate UI texture would have to be
       copied back into the presented image or presented through a second swapchain that DXVK does not have;
     - redirecting the render target alone does not fix the layout: Scaleform would still lay out for a 2:1 (or 3:1)
       viewport and the canvas for the eye rect, so game-level changes are needed anyway, and with those the UI already
       lands in its own rect: no copy, no extra texture, no extra GPU pass.
   The only RHI-level piece is the alpha channel (linux/vr_ui_rhi.c): UE3 keeps the back buffer's alpha when blending, so the
   UI column's alpha is made "coverage" there, giving a premultiplied UI image for the compositor.

   Game-level hooks (all inside the hooked part007 / part012, regress/vr/vr_hook_parts.sh; evidence = disassembly):
     T  DrawTile (f_0073ab10) for the areas no view covers, at 0x66ae03 (no-world branch) and 0x66b060 (per rect): the part
        inside the UI column is drawn as transparent black (color 0,0,0,0, no texture, opaque blend), i.e. the column is
        cleared to alpha 0 every frame; the eye part stays black.
     C  after the cover tiles (PopTransform call at 0x66ae0f; BeginRenderingViewFamily at 0x66ab87 has already copied the
        views for the renderer): view 0 (the one in PlayerViewMap) gets X/Y/SizeX/SizeY = the UI rect, so the per-player
        HUD canvas (origin + ClipX/Y from the view, 0x66b0a2..0x66b1c0) lands there; with ui_proj=1 also View/Projection/
        ViewProjection/InvViewProjection/InvView (+0x70/+0xB0/+0x240/+0x280/+0x2C0) = head view + the UI quad's frustum,
        so Canvas.Project / DeProject (FSceneView::Project = f_006dd4e0 reads +0x240) and Scaleform's own copy of them
        (GGFxEngine 0x13ed290 +0x30/+0x70/+0xB0/+0xF0..+0xFC, written by f_00850790 at 0x66a940) put world markers where the
        target appears on the quad. A Draw without a world view is tagged UI-only for the XR side.
     S  DisplaySubtitles (f_006d67f0, call 0x66b32a): its FIntRect (fractions x full render-target size) is mapped into the rect.
     F  the full-viewport canvas (UCanvas::Update f_006d9d90, call 0x66b36f; used by GameViewportClient.PostRender and the
        console): SizeX/SizeY = rect size, plus an absolute translation to the rect pushed on the frame's FCanvas.
     G  Scaleform: FGFxEngine's viewport layout (f_00856ca0, every movie, called on resize / movie start / UpdateScaleform-
        ViewportLayout): the GViewport passed to GFxMovieView::SetViewport (vf 0x64) at 0x856fbd (full-viewport movies) and
        0x857157 (bFillOnlyPlayerOwnerViewport movies) gets Left/Top/Width/Height (and the scissor) mapped into the rect;
        BufferWidth/Height stay the full buffer. 0x856e5a (movies whose viewport is kept, probably render-to-texture) is
        only counted. Idempotent per movie view.
   Off unless the stereo layout started with ui=1 (vr_stereo.c); every hook is a pass-through otherwise. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vr_stereo.h"

/* ---------------- pure math (host-tested in regress/vr/vr_math_test.c) ---------------- */
void vs_mat4_mul(const float a[16], const float b[16], float out[16]){
  float r[16];
  for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++){
    double s = 0; for(int k = 0; k < 4; k++) s += (double)a[i*4 + k] * b[k*4 + j];
    r[i*4 + j] = (float)s; }
  memcpy(out, r, sizeof r);
}
int vs_mat4_inverse(const float m[16], float out[16]){
  double a[4][8];
  for(int i = 0; i < 4; i++) for(int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i*4 + j] : (j - 4 == i);
  for(int c = 0; c < 4; c++){
    int p = c; for(int r = c + 1; r < 4; r++) if(fabs(a[r][c]) > fabs(a[p][c])) p = r;
    if(fabs(a[p][c]) < 1e-30) return 0;
    if(p != c) for(int j = 0; j < 8; j++){ double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
    double d = a[c][c]; for(int j = 0; j < 8; j++) a[c][j] /= d;
    for(int r = 0; r < 4; r++) if(r != c){ double f = a[r][c]; if(f != 0) for(int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
  }
  for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) out[i*4 + j] = (float)a[i][j + 4];
  return 1;
}
/* symmetric perspective for the UI quad (UE3 row-vector layout, x right / y up / z forward): half-width tangent tan_hx,
   aspect = width / height of the UI rect; depth terms from the game's projection */
void vs_ui_proj(const float Pg[16], double tan_hx, double aspect, float P[16]){
  memset(P, 0, 16 * sizeof *P);
  P[0] = (float)(1.0 / tan_hx); P[5] = (float)(aspect / tan_hx);
  P[10] = Pg[10]; P[11] = Pg[11]; P[14] = Pg[14];
}
/* a rect in full-viewport pixels (x, y, w, h) -> the same place inside the UI rect */
void vs_map_rect(const VsLayout *L, int vw, int vh, const float in[4], float out[4]){
  double sx = (double)L->ui_w / vw, sy = (double)L->ui_h / vh;
  out[0] = (float)(L->ui_x + in[0] * sx); out[1] = (float)(L->ui_y + in[1] * sy);
  out[2] = (float)(in[2] * sx); out[3] = (float)(in[3] * sy);
}
/* "follow" UI placement: the quad's yaw stays put until the head turns more than thresh away, then it eases back toward
   the head (rate rad/s) until within thresh/6. Angles in radians. */
float vs_follow_yaw(float cur, float head, float thresh, float dt, float rate, int *moving){
  float d = head - cur;
  while(d > (float)M_PI) d -= 2.f * (float)M_PI;
  while(d < -(float)M_PI) d += 2.f * (float)M_PI;
  float ad = fabsf(d);
  if(ad > thresh) *moving = 1;
  if(*moving){
    float step = rate * dt; if(step < ad * 0.15f) step = ad * 0.15f;   /* at least 15 % per frame: far turns catch up quickly */
    if(step >= ad - thresh / 6.f){ step = ad - thresh / 6.f; if(step < 0) step = 0; *moving = 0; }
    cur += d > 0 ? step : -step;
    if(cur > (float)M_PI) cur -= 2.f * (float)M_PI;
    if(cur < -(float)M_PI) cur += 2.f * (float)M_PI;
  }
  return cur;
}

/* decide what one cover tile becomes: 0 = unchanged; else up to two tiles, the column part with clear = 1 */
int vr_ui_tile(float x, float y, float w, float h, int vw, int vh, VrTile out[2]){
  VsLayout L; if(!vr_stereo_ui_layout(vw, vh, &L)) return 0;
  float cx = (float)(2 * L.eye_w);
  if(x + w <= cx + 0.5f || w <= 0 || h <= 0) return 0;
  int n = 0;
  if(x < cx - 0.5f){ out[n].x = x; out[n].y = y; out[n].w = cx - x; out[n].h = h; out[n].clear = 0; n++; }
  float x0 = x > cx ? x : cx;
  out[n].x = x0; out[n].y = y; out[n].w = x + w - x0; out[n].h = h; out[n].clear = 1; n++;
  return n;
}

#ifndef VS_NO_GUEST
extern uint8_t *M;
uint32_t gmalloc(uint32_t n);
void vr_ui_rhi_frame(int on) __attribute__((weak));   /* vr_ui_rhi.c (optional) */
#define GGFX_ENGINE 0x13ed290u   /* FGFxEngine* (UXGameViewportClient::UpdateScaleformViewportLayout reads it, 0xaf11e0) */
/* FSceneView (ctor f_0072ce80) */
#define SV_RECT     0x34         /* X, Y, SizeX, SizeY floats */
#define SV_VIEW     0x70
#define SV_PROJ     0xB0
#define SV_VIEWPROJ 0x240
#define SV_INVVP    0x280
#define SV_INVVIEW  0x2C0
#define UCANVAS_SIZEX 0x54

static void rd(uint32_t a, void *p, size_t n){ memcpy(p, M + a, n); }
static void wr(uint32_t a, const void *p, size_t n){ memcpy(M + a, p, n); }
static uint32_t r32(uint32_t a){ uint32_t v; memcpy(&v, M + a, 4); return v; }
static int ok(uint32_t a){ return a >= 0x10000u && a < 0xC0000000u; }
static uint32_t galign(uint32_t n){ uint32_t p = gmalloc(n + 16); if(!p) return 0; p = (p + 15) & ~15u; memset(M + p, 0, n); return p; }

static uint32_t g_zero, g_mat;
static unsigned long g_n_views, g_n_uionly, g_n_sub, g_n_canvas, g_n_gfx[3];
uint32_t vr_ui_zero_color(void){ if(!g_zero) g_zero = galign(16); return g_zero; }

void vr_ui_after_cover(int vw, int vh){
  VsLayout L; if(!vr_stereo_ui_layout(vw, vh, &L)) return;
  const VsCfg *cfg = vr_stereo_cfg();
  /* coverage alpha: linux/vr_ui_rhi.c with the D3D11 bridge; with the native renderer rhi_b1 does it in the D3D11 stream
     (linux/rhi_b1_vr.inc, vr69), so the UE3 RHI vtable stays untouched */
  if(&vr_ui_rhi_frame) vr_ui_rhi_frame(cfg->ui_alpha == 1 && !vr_stereo_native());
  float Vh[16], Pg[16]; uint32_t view = vr_stereo_take_pair(Vh, Pg);
  if(!view || !ok(view)){ vr_stereo_mark_uionly(); if(!g_n_uionly++) fprintf(stderr, "[vr_ui] first UI-only frame (no world view)\n"); return; }
  float r[4] = { (float)L.ui_x, (float)L.ui_y, (float)L.ui_w, (float)L.ui_h };
  wr(view + SV_RECT, r, 16);
  if(cfg->ui_proj){
    float P[16], VP[16], iVP[16], iV[16];
    vs_ui_proj(Pg, (cfg->ui_width * 0.5) / cfg->ui_dist, (double)L.ui_w / L.ui_h, P);
    vs_mat4_mul(Vh, P, VP);
    if(vs_mat4_inverse(VP, iVP) && vs_mat4_inverse(Vh, iV)){
      wr(view + SV_VIEW, Vh, 64); wr(view + SV_PROJ, P, 64); wr(view + SV_VIEWPROJ, VP, 64);
      wr(view + SV_INVVP, iVP, 64); wr(view + SV_INVVIEW, iV, 64); } }
  /* Scaleform's copy of the view (f_00850790, made right after CalcSceneView); same guard as f_00852ea0 */
  uint32_t gfx = r32(GGFX_ENGINE);
  if(ok(gfx)){ uint32_t o = r32(gfx + 0x31c);
    if(ok(o) && r32(o + 0x58)){
      uint8_t m[64]; rd(view + SV_VIEWPROJ, m, 64); wr(gfx + 0x30, m, 64);
      rd(view + SV_INVVP, m, 64); wr(gfx + 0x70, m, 64);
      rd(view + SV_INVVIEW, m, 64); wr(gfx + 0xB0, m, 64);
      float s[4] = { r[2], r[3], r[0], r[1] }; wr(gfx + 0xF0, s, 16); } }   /* SizeX, SizeY, X, Y */
  if(!g_n_views++) fprintf(stderr, "[vr_ui] HUD view 0x%08x -> UI rect %d,%d %dx%d of %dx%d (ui_proj %d)\n", view, L.ui_x, L.ui_y, L.ui_w, L.ui_h, vw, vh, cfg->ui_proj);
}

void vr_ui_subtitle_rect(uint32_t rect, int vw, int vh){
  VsLayout L; if(!ok(rect) || !vr_stereo_ui_layout(vw, vh, &L)) return;
  int32_t q[4]; rd(rect, q, 16);
  float in[4] = { (float)q[0], (float)q[1], (float)(q[2] - q[0]), (float)(q[3] - q[1]) }, o[4];
  vs_map_rect(&L, vw, vh, in, o);
  q[0] = (int32_t)lrintf(o[0]); q[1] = (int32_t)lrintf(o[1]); q[2] = (int32_t)lrintf(o[0] + o[2]); q[3] = (int32_t)lrintf(o[1] + o[3]);
  wr(rect, q, 16);
  if(!g_n_sub++) fprintf(stderr, "[vr_ui] subtitles -> %d,%d..%d,%d\n", q[0], q[1], q[2], q[3]);
}

int vr_ui_full_canvas(uint32_t ucanvas, int vw, int vh, uint32_t *matrix){
  VsLayout L; *matrix = 0;
  if(!ok(ucanvas) || !vr_stereo_ui_layout(vw, vh, &L)) return 0;
  if(!g_mat && !(g_mat = galign(64))) return 0;
  int32_t sz[2] = { L.ui_w, L.ui_h }; wr(ucanvas + UCANVAS_SIZEX, sz, 8);
  float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, (float)L.ui_x, (float)L.ui_y, 0, 1 };
  wr(g_mat, T, 64); *matrix = g_mat;
  if(!g_n_canvas++) fprintf(stderr, "[vr_ui] full-viewport canvas -> %dx%d at %d,%d\n", L.ui_w, L.ui_h, L.ui_x, L.ui_y);
  return 1;
}

/* GViewport (Scaleform GFx 3.3): BufferWidth, BufferHeight, Left, Top, Width, Height, ScissorLeft, ScissorTop,
   ScissorWidth, ScissorHeight (int32), ... */
#define GFX_MAXV 64
static struct { uint32_t view; int32_t r[4]; } g_gfx[GFX_MAXV]; static int g_ngfx;
void vr_ui_gfx_viewport(uint32_t pview, uint32_t vp, int site){
  if(site >= 0 && site < 3) g_n_gfx[site]++;
  if(site == 0 || !ok(vp) || !vr_stereo_ui_on()) return;
  int32_t v[10]; rd(vp, v, 40);
  VsLayout L; if(!vr_stereo_ui_layout(v[0], v[1], &L)) return;
  int k; for(k = 0; k < g_ngfx; k++) if(g_gfx[k].view == pview) break;
  if(k < g_ngfx && !memcmp(g_gfx[k].r, v + 2, 16)) return;    /* already ours (a re-layout read it back) */
  float in[4] = { (float)v[2], (float)v[3], (float)v[4], (float)v[5] }, o[4];
  vs_map_rect(&L, v[0], v[1], in, o);
  v[2] = (int32_t)lrintf(o[0]); v[3] = (int32_t)lrintf(o[1]); v[4] = (int32_t)lrintf(o[2]); v[5] = (int32_t)lrintf(o[3]);
  if(v[8] > 0 && v[9] > 0){ float si[4] = { (float)v[6], (float)v[7], (float)v[8], (float)v[9] }, so[4];
    vs_map_rect(&L, v[0], v[1], si, so);
    v[6] = (int32_t)lrintf(so[0]); v[7] = (int32_t)lrintf(so[1]); v[8] = (int32_t)lrintf(so[2]); v[9] = (int32_t)lrintf(so[3]); }
  wr(vp, v, 40);
  if(k == g_ngfx && g_ngfx < GFX_MAXV){ g_gfx[k].view = pview; g_ngfx++; }
  if(k < GFX_MAXV) memcpy(g_gfx[k].r, v + 2, 16);
  if(k < 8 || (g_n_gfx[1] + g_n_gfx[2]) % 200 == 0)
    fprintf(stderr, "[vr_ui] scaleform movie view 0x%08x (site %d) -> %d,%d %dx%d in %dx%d; calls: kept %lu full %lu owner %lu\n", pview, site,
            v[2], v[3], v[4], v[5], v[0], v[1], g_n_gfx[0], g_n_gfx[1], g_n_gfx[2]);
}
#endif
