/* vr_aa.h -- job aa76: opt-in VR anti-aliasing, pure parts (header-only; host-tested in aa76/tests/aa76_test.c).
   The renderer side is rhi_b1_aa.inc (native renderer only); the render-thread scene hook is one weak call in vr_ui_rhi.c.

   Switch (vr_stereo.txt, re-read every 2 s by rhi_b1_aa.inc; env BSI_VR_AA=<same words> wins, cloud also BSI_VR_STEREO_OPTS):
     aa=off      (default; also game / fxaa / 0) the game's own FXAA, nothing changes (the command stream is the build-75 one)
     aa=none     no anti-aliasing at all (FXAA's input copied to the eyes; for comparisons)
     aa=smaa     SMAA 1x (Jimenez et al., reference code, MIT) per eye instead of FXAA
     aa=taa      temporal AA per eye instead of FXAA: sub-pixel jitter of each eye's projection (Halton 2,3), history per eye
                 reprojected with depth + each eye's current / previous view-projection, YCoCg variance clipping, optional
                 sharpening (CAS-style) of the displayed picture
   TAA tuning: aa_alpha=0.1 (weight of the new frame, 0.03..1), aa_clip=1.0 (variance clip width in sigmas, 0.5..4),
   aa_sharpen=0.25 (0..1), aa_jitter=1 (0 = no projection jitter), aa_filter=1 (0 = the raw jittered sample enters the
   history instead of the 3x3 reconstruction at the pixel centre), aa_hist=cubic|linear (history filter),
   aa_reset_deg=45 / aa_reset_m=2 (a camera rotation / move larger than this between two frames resets the history),
   aa_log=1 (one status line every 10 s while stereo frames are anti-aliased).
   History resets: camera cut (rotation / move above), projection change, mode change, a stereo frame without TAA (menu /
   UI-only frame, fallback), and more than VA_MAX_GAP presents in a row without a scene (loading screens, movies).

   UE3 conventions (notes/VR_STEREO.md §3): row vectors (v' = v M), D3D clip space (x right, y up, z 0..1), so a pixel
   offset of the image is a change of the projection's third row: ndc.x += P[8] (P[2][0]) for every point (clip w = view z,
   P[11] = 1), i.e. jx pixels to the right = P[8] += 2 jx / eye width, jy pixels up = P[9] += 2 jy / height. */
#ifndef VR_AA_H
#define VR_AA_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VA_GAME = 0, VA_NONE = 1, VA_SMAA = 2, VA_TAA = 3 };
typedef struct {
  int mode;            /* VA_* */
  float alpha;         /* TAA: weight of the new frame */
  float clip;          /* TAA: variance clip half-width in standard deviations */
  float sharpen;       /* TAA: CAS-style sharpening of the displayed picture (0 = off) */
  int jitter;          /* TAA: sub-pixel jitter of the eye projections */
  int jitter_n;        /* Halton sequence length (8) */
  int hist_cubic;      /* TAA: Catmull-Rom history fetch (1) or bilinear (0) */
  int filter;          /* TAA: the new frame reconstructed at the pixel centre from its 3x3 jittered samples (1) or raw (0) */
  float reset_deg;     /* rotation between two frames of one eye that resets its history */
  float reset_m;       /* camera move (metres at 100 UU/m) between two frames that resets */
  int log;
} VaCfg;

static inline void va_cfg_defaults(VaCfg *c){
  memset(c, 0, sizeof *c);
  c->mode = VA_GAME; c->alpha = 0.1f; c->clip = 1.0f; c->sharpen = 0.25f; c->jitter = 1; c->jitter_n = 8; c->hist_cubic = 1; c->filter = 1;
  c->reset_deg = 45.f; c->reset_m = 2.f; c->log = 1;
}
static inline const char *va_mode_name(int m){ return m == VA_NONE ? "none" : m == VA_SMAA ? "smaa" : m == VA_TAA ? "taa" : "off"; }
/* words "key=value" separated by blanks; unknown words ignored (vr_stereo.c ignores ours the same way) */
static inline void va_parse_opts(VaCfg *c, const char *s){
  char w[64]; int n;
  while(s && sscanf(s, "%63s%n", w, &n) == 1){
    s += n; char *eq = strchr(w, '='); if(!eq) continue; *eq = 0; const char *v = eq + 1; float f = strtof(v, NULL);
    if(!strcmp(w, "aa")){ int m = !strcmp(v, "taa") ? VA_TAA : !strcmp(v, "smaa") ? VA_SMAA : !strcmp(v, "none") ? VA_NONE
                                  : !strcmp(v, "game") || !strcmp(v, "fxaa") || !strcmp(v, "off") || !strcmp(v, "0") ? VA_GAME : -1; if(m >= 0) c->mode = m; }
    else if(!strcmp(w, "aa_alpha") && f >= 0.03f && f <= 1.f) c->alpha = f;
    else if(!strcmp(w, "aa_clip") && f >= 0.5f && f <= 4.f) c->clip = f;
    else if(!strcmp(w, "aa_sharpen") && f >= 0.f && f <= 1.f) c->sharpen = f;
    else if(!strcmp(w, "aa_jitter")) c->jitter = atoi(v) != 0;
    else if(!strcmp(w, "aa_filter")) c->filter = atoi(v) != 0;
    else if(!strcmp(w, "aa_hist")){ if(!strcmp(v, "cubic")) c->hist_cubic = 1; else if(!strcmp(v, "linear")) c->hist_cubic = 0; }
    else if(!strcmp(w, "aa_reset_deg") && f > 0.f && f <= 180.f) c->reset_deg = f;
    else if(!strcmp(w, "aa_reset_m") && f > 0.f && f <= 100.f) c->reset_m = f;
    else if(!strcmp(w, "aa_log")) c->log = atoi(v);
  }
}

/* Halton radical inverse, index >= 1 (index 0 would give 0 for every base) */
static inline double va_halton(uint32_t i, uint32_t base){
  double f = 1.0, r = 0.0;
  while(i > 0){ f /= base; r += f * (i % base); i /= base; }
  return r;
}
/* jitter of frame k (pixels, x right, y up), in (-0.5, 0.5): Halton(2,3) points 1..n, centred */
static inline void va_jitter(uint32_t k, int n, double *jx, double *jy){
  if(n < 1) n = 1;
  uint32_t i = (k % (uint32_t)n) + 1u;
  *jx = va_halton(i, 2) - 0.5; *jy = va_halton(i, 3) - 0.5;
}

/* 4x4 helpers (row-major, row vectors), double */
static inline void va_m4_mul(const double *a, const double *b, double *o){
  double t[16];
  for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++){ double s = 0; for(int k = 0; k < 4; k++) s += a[r*4 + k] * b[k*4 + c]; t[r*4 + c] = s; }
  memcpy(o, t, sizeof t); }
static inline int va_m4_inv(const double *m, double *o){   /* Gauss-Jordan, partial pivoting */
  double a[4][8];
  for(int r = 0; r < 4; r++) for(int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r*4 + c] : (c - 4 == r);
  for(int c = 0; c < 4; c++){
    int p = c; for(int r = c + 1; r < 4; r++) if(fabs(a[r][c]) > fabs(a[p][c])) p = r;
    if(fabs(a[p][c]) < 1e-300) return 0;
    if(p != c) for(int k = 0; k < 8; k++){ double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
    double iv = 1.0 / a[c][c]; for(int k = 0; k < 8; k++) a[c][k] *= iv;
    for(int r = 0; r < 4; r++) if(r != c){ double f = a[r][c]; if(f != 0) for(int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; } }
  for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++) o[r*4 + c] = a[r][c + 4];
  return 1; }
static inline void va_ld16(const uint8_t *p, double *d){ float f[16]; memcpy(f, p, 64); for(int i = 0; i < 16; i++) d[i] = f[i]; }
static inline void va_st16(uint8_t *p, const double *d){ float f[16]; for(int i = 0; i < 16; i++) f[i] = (float)d[i]; memcpy(p, f, 64); }

/* FViewInfo / FSceneView fields (BSI; notes/VR_STEREO.md §19.2, the same offsets vr_stereo.c's late latch uses) */
#define VA_VI_X      0x34    /* X (float), Y, SizeX, SizeY */
#define VA_VI_VIEW   0x70
#define VA_VI_PROJ   0xB0
#define VA_VI_TVIEW  0x170
#define VA_VI_TVP    0x1B0
#define VA_VI_ITVP   0x1F0
#define VA_VI_PVT    0x230   /* PreViewTranslation (FVector) */
#define VA_VI_VP     0x240
#define VA_VI_IPROJ  0x280
#define VA_VI_IVIEW  0x2C0
#define VA_VI_IVP    0x300
#define VA_VI_STW    0x340
#define VA_VI_SIZE   0x11A0

/* jitter one FViewInfo (host pointer to its first byte): the projection's third row gets (dx, dy) in NDC, and every field
   derived from the projection is recomputed the way the FSceneView ctor / the late latch derive them: ViewProjection,
   TranslatedViewProjection and its inverse, the inverse projection, InvViewProjection and ScreenToTranslatedWorld. The
   view matrices, the eye position, the near clipping plane (ViewProjection's third column does not depend on P[8], P[9])
   and the culling frustum stay. */
static inline void va_jitter_view(uint8_t *vi, double dx, double dy){
  double V[16], TV[16], P[16], IP[16], IV[16], t[16], u[16];
  va_ld16(vi + VA_VI_VIEW, V); va_ld16(vi + VA_VI_TVIEW, TV); va_ld16(vi + VA_VI_PROJ, P); va_ld16(vi + VA_VI_IVIEW, IV);
  P[8] += dx; P[9] += dy;
  va_st16(vi + VA_VI_PROJ, P); va_ld16(vi + VA_VI_PROJ, P);   /* use the float values the GPU will see */
  if(va_m4_inv(P, IP)) va_st16(vi + VA_VI_IPROJ, IP); else va_ld16(vi + VA_VI_IPROJ, IP);
  va_m4_mul(V, P, t); va_st16(vi + VA_VI_VP, t);
  va_m4_mul(TV, P, t); va_st16(vi + VA_VI_TVP, t);
  { double tf[16]; va_ld16(vi + VA_VI_TVP, tf); if(va_m4_inv(tf, u)) va_st16(vi + VA_VI_ITVP, u); else va_ld16(vi + VA_VI_ITVP, u); }
  va_m4_mul(IP, IV, t); va_st16(vi + VA_VI_IVP, t);
  { double S[16] = { 1,0,0,0, 0,1,0,0, 0,0,P[10],1, 0,0,P[14],0 }; va_m4_mul(S, u, t); va_st16(vi + VA_VI_STW, t); }
}

/* the matrices TAA needs of one eye view, taken BEFORE the jitter: TVP = TranslatedView * Projection (double, from the
   float fields), PVT = PreViewTranslation, R = the view's rotation rows, P = the projection */
typedef struct { double tvp[16], pvt[3], rot[9], proj[16]; int ok; } VaEye;
static inline void va_eye_read(const uint8_t *vi, VaEye *e){
  double TV[16], P[16]; float pv[3];
  va_ld16(vi + VA_VI_TVIEW, TV); va_ld16(vi + VA_VI_PROJ, P); memcpy(pv, vi + VA_VI_PVT, 12);
  va_m4_mul(TV, P, e->tvp); memcpy(e->proj, P, sizeof P);
  for(int i = 0; i < 3; i++) e->pvt[i] = pv[i];
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) e->rot[r*3 + c] = TV[r*4 + c];
  e->ok = 1;
}
/* reprojection of one eye, current frame -> previous frame, for the TAA shader: a pixel's (ndc x, ndc y, device z, 1) times
   M gives the previous frame's clip position of the same world point.
   tw = translated world of this frame = world + PVT_cur; the previous frame's translated world is tw + (PVT_prev - PVT_cur):
   M = inverse(TVP_cur) * Translate(PVT_prev - PVT_cur) * TVP_prev, both TVP without jitter (the history holds a
   jitter-free picture, so a static scene maps every pixel onto itself). Returns 0 if TVP_cur is singular. */
static inline int va_reproj(const VaEye *cur, const VaEye *prev, double M[16]){
  double I[16], T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, t[16];
  if(!va_m4_inv(cur->tvp, I)) return 0;
  for(int i = 0; i < 3; i++) T[12 + i] = prev->pvt[i] - cur->pvt[i];
  va_m4_mul(I, T, t); va_m4_mul(t, prev->tvp, M);
  return 1;
}
/* rotation angle (degrees) between two eye orientations (view rotation rows) */
static inline double va_rot_deg(const double *a, const double *b){
  double tr = 0; for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) tr += a[r*3 + c] * b[r*3 + c];   /* trace(A B^T) */
  double x = (tr - 1.0) * 0.5; if(x > 1) x = 1; if(x < -1) x = -1;
  return acos(x) * 180.0 / M_PI;
}
/* why the history of an eye must be reset this frame (0 = keep). uu_per_m: the stereo world scale (vr_stereo.txt uu, 100). */
enum { VA_R_NONE = 0, VA_R_FIRST, VA_R_GAP, VA_R_ROT, VA_R_MOVE, VA_R_PROJ, VA_R_SIZE, VA_R_MODE };
static inline int va_reset_reason(const VaEye *cur, const VaEye *prev, const VaCfg *c, double uu_per_m){
  if(!prev || !prev->ok) return VA_R_FIRST;
  if(va_rot_deg(cur->rot, prev->rot) > c->reset_deg) return VA_R_ROT;
  double d2 = 0; for(int i = 0; i < 3; i++){ double d = cur->pvt[i] - prev->pvt[i]; d2 += d * d; }
  if(sqrt(d2) > c->reset_m * uu_per_m) return VA_R_MOVE;
  /* a projection change other than the jitter (fov / near plane): P[0], P[5], P[10], P[14] */
  static const int k[4] = { 0, 5, 10, 14 };
  for(int i = 0; i < 4; i++){ double a = cur->proj[k[i]], b = prev->proj[k[i]]; if(fabs(a - b) > 1e-4 * (fabs(a) + fabs(b)) + 1e-9) return VA_R_PROJ; }
  return VA_R_NONE;
}
/* the history state at the end of a present (pure; rhi_b1_aa.inc frame_end):
   ran_both  = TAA resolved both eyes this frame (the history slot just written is valid),
   any_scene = the frame drew a scene, an FXAA draw or a tone map into the back buffer.
   A present without any scene (loading screens, Bink movies, an extra UI present) keeps the history unless more than
   VA_MAX_GAP of them come in a row; a scene frame without TAA (UI-only menu, fallback) always drops it.
   Returns 1 if the history slots must be swapped (the new history becomes the previous one). */
#define VA_MAX_GAP 2
typedef struct { int ok, gap; long n_gap_resets; } VaHist;
static inline int va_hist_end(VaHist *h, int ran_both, int any_scene){
  if(ran_both){ h->ok = 1; h->gap = 0; return 1; }
  if(!any_scene){ if(++h->gap > VA_MAX_GAP && h->ok){ h->ok = 0; h->n_gap_resets++; } return 0; }
  h->ok = 0; h->gap = 0; return 0;
}
static inline const char *va_reset_name(int r){
  static const char *n[] = { "kept", "first frame", "frame gap", "rotation", "camera move", "projection", "size", "mode" };
  return r >= 0 && r <= VA_R_MODE ? n[r] : "?";
}

/* the per-frame constants all aa76 shaders read (std140, 208 bytes); see aa76/aa76_common.glsl */
enum { VA_F_RESET0 = 1, VA_F_RESET1 = 2, VA_F_CUBIC = 4, VA_F_HIST = 8, VA_F_NODEPTH = 16, VA_F_NOFILTER = 32 };
typedef struct {
  float rt[4];      /* 1/W, 1/H, W, H of the back buffer */
  int32_t eye[4];   /* eye width, height, x0 of the area to process, x1 (exclusive) */
  int32_t prm[4];   /* mode (VA_*), flags (VA_F_*), frame, bb mode (1: no FXAA pass, the input is a copy of the back buffer's eyes) */
  float taa[4];     /* alpha, clip, sharpen, 0 */
  float jit[4];     /* this frame's jitter (pixels, x right, y up), 0, 0 */
  float m[2][16];   /* per eye: reprojection rows (cur -> prev clip), row-major */
} VaUbo;

/* which eye columns a full-screen draw covers: its area = viewport (x, y, w, h) intersected with the scissor rectangle
   (left, top, right, bottom; sc = NULL: no scissor). want = eyes covered whole (bit 0 left, bit 1 right), part = eyes
   touched but not covered whole (aa76 then leaves the draw alone). The game draws each eye view's post-process with the
   whole buffer as viewport and that view's column as scissor. */
static inline void va_cover(int ew, int h, const float *vp, const int32_t *sc, int *want, int *part){
  float ax0 = vp[0], ay0 = vp[1], ax1 = vp[0] + vp[2], ay1 = vp[1] + vp[3];
  if(sc){ if((float)sc[0] > ax0) ax0 = (float)sc[0]; if((float)sc[1] > ay0) ay0 = (float)sc[1]; if((float)sc[2] < ax1) ax1 = (float)sc[2]; if((float)sc[3] < ay1) ay1 = (float)sc[3]; }
  *want = 0; *part = 0;
  for(int e = 0; e < 2; e++){
    float c0 = (float)(e * ew), c1 = (float)((e + 1) * ew);
    int whole = ax0 <= c0 + 0.5f && ax1 >= c1 - 0.5f && ay0 <= 0.5f && ay1 >= h - 0.5f;
    int touch = ax0 < c1 - 0.5f && ax1 > c0 + 0.5f && ay0 < h - 0.5f && ay1 > 0.5f;
    if(whole) *want |= 1 << e; else if(touch) *part |= 1 << e; }
}
/* the x range (NDC) a post-process quad covers: UE3's DrawDenormalizedQuad writes 4 (strip) or 6 (list) vertices of
   FFilterVertex { float4 Position (clip space, w = 1); float2 UV } into a dynamic vertex buffer, and the post vertex shaders
   pass POSITION through as SV_Position (FXEdgeDetectAntiAliasVertexShader: mov o2, v0). v = the first vertex, stride bytes
   apart. Returns 0 unless every vertex has w ~ 1 and x/w within [-1.1, 1.1] (anything else: not such a quad). */
static inline int va_quad_xrange(const uint8_t *v, int count, int stride, float *nx0, float *nx1){
  if(count < 3 || count > 6 || stride < 16) return 0;
  float lo = 1e9f, hi = -1e9f;
  for(int i = 0; i < count; i++){
    float x, w; memcpy(&x, v + i * stride, 4); memcpy(&w, v + i * stride + 12, 4);
    if(!(w > 0.99f && w < 1.01f) || !(x / w >= -1.1f && x / w <= 1.1f)) return 0;
    if(x / w < lo) lo = x / w;
    if(x / w > hi) hi = x / w; }
  *nx0 = lo; *nx1 = hi; return hi > lo;
}
/* pixel of the back buffer -> eye (0/1) and the eye column's x range, -1 for the UI column (no AA) */
static inline int va_eye_of(int x, int ew, int *x0, int *x1){
  if(ew <= 0 || x < 0 || x >= 2 * ew) return -1;
  int e = x >= ew; *x0 = e * ew; *x1 = *x0 + ew; return e;
}
/* the reprojection the shader does, on the CPU (tests): pixel centre (px, py) of an eye column of width ew, height h,
   device depth d -> previous-frame pixel position (buffer x, y; texel centres at .5); returns 0 if w is ~0. As the shader,
   a negative w (sky: depth 1.0 lies beyond the infinite-far projection's limit P[10]) is used as is: same direction. */
/* as the shader computes it, in float (zb = 0; a nonzero zb evaluates (x, y, d - zb, 1) M) */
static inline int va_reproj_px_z(const float *M, float zb, double px, double py, float d, int x0, int ew, int h, double *qx, double *qy){
  float nx = (float)((px - x0) / ew * 2.0 - 1.0), ny = (float)(1.0 - py / h * 2.0), v[4] = { nx, ny, d - zb, 1.0f }, c[4] = { 0, 0, 0, 0 };
  for(int j = 0; j < 4; j++) for(int i = 0; i < 4; i++) c[j] += v[i] * M[i*4 + j];
  if(!(fabsf(c[3]) > 1e-12f)) return 0;
  double ux = c[0] / c[3], uy = c[1] / c[3];
  *qx = x0 + (ux * 0.5 + 0.5) * ew; *qy = (0.5 - uy * 0.5) * h;
  return 1;
}
static inline int va_reproj_px(const float *M, double px, double py, double d, int x0, int ew, int h, double *qx, double *qy){
  return va_reproj_px_z(M, 0.0f, px, py, (float)d, x0, ew, h, qx, qy); }
#endif
