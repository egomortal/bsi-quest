/* vr_stereo (VR phase 5, step 2 prototype, ROADMAP item 27): render the scene once per eye.

   Route (see notes/VR_STEREO.md): UE3's own multi-view path. UGameViewportClient::Draw (f_0066a3e0) calls
   ULocalPlayer::CalcSceneView (f_006633b0) once per player; each call builds an FSceneView (ctor f_0072ce80, View @+0x70,
   Projection @+0xB0) and appends it to the FSceneViewFamily, and BeginRenderingViewFamily then renders every view of the
   family into its own rectangle of the one back buffer (this is how UE3 does split screen). So the stereo step is:
     - the hooked part007 (regress/vr_stereo_part007.sh) wraps f_006633b0. For the call from Draw (return address 0x66a91c)
       it runs the original twice: eye 0 with LocalPlayer.Origin/Size = left half, eye 1 = right half and a second
       FSceneViewState (allocated once with f_00731760 = AllocateViewState), so occlusion history, motion-blur
       "previous view" and eye adaptation are per eye. Both views land in the same family: one BeginRenderingViewFamily,
       one Present (UE3 still builds whole-scene shadows per view). The back buffer is one double-wide image [ left | right ].
     - inside each run, the FSceneView ctor call (0x664119) goes through vr_stereo_view_ctor_args(): the game's ViewMatrix
       and ProjectionMatrix (locals of CalcSceneView, passed by pointer) are replaced by the per-eye ones:
         View  = game view moved by the eye offset (head-relative, from OpenXR) and rotated by the eye's relative rotation;
         Proj  = asymmetric off-axis projection straight from the OpenXR fov tangents (z terms kept from the game:
                 near plane 10, infinite far). FrameDelayedFOV (one frame late) is therefore irrelevant.
       Everything derived in the ctor (ViewOrigin, PreViewTranslation, ViewProjection, InvViewProjection, frustum) follows.
     - the head ORIENTATION still comes from vr_head.c (FillCameraCache), so aiming, weapon and audio follow the head; this
       file only adds the per-eye offset + projection on top of the game's (head-rotated) camera.
     - every stereo frame is tagged with the index of the Present that will show it (bridge.c g_br_presents) and the exact
       eye poses/fovs it was rendered with; xr_glue (regress/vr_stereo_xr_glue.patch) submits a projection layer with
       those poses once that Present has happened, so the compositor's reprojection (ATW) is consistent.

   Switches:
     BSI_VR_STEREO=1 / 0     force the double-wide stereo layout on / off at startup (else: /sdcard/BSI/vr_stereo.txt exists)
     /sdcard/BSI/vr_stereo.txt  (BSI_VR_STEREO_FILE overrides the path) re-read every 2 s; deleting it returns to mono
                             rendering of the whole wide buffer (shown as a wide flat quad). Options, any order:
        ipd=1.0          IPD scale (1 = the headset's real eye distance)
        uu=100           world scale, Unreal units per metre. BSI looks like ~1 cm/UU, not UE3's usual 2 cm/UU: the eye is
                         105 UU above the pawn's centre and XPawn.JogSpeed is 700 UU/s (7 m/s at 1 cm/UU, 14 m/s at 2).
                         Calibrate on the Quest (notes/VR_STEREO.md T5): too small -> the world looks giant, too big -> toy-like
        pos=1 posmax=0.35  positional head tracking relative to the LOCAL origin (recenter), clamped (m); pos=0 = IPD only
        near=0           override the near plane in UU (0 = keep the game's 10)
        vs2=1            separate FSceneViewState for the right eye (0 = share the player's; A/B for artefacts)
        lag_ms=0         XR side: wait this long after a Present before switching the layer pose to that frame
        pred_ms=0        XR side: extra pose prediction added to predictedDisplayTime (measured latency is logged)
        log=1            [vr_stereo] log lines (every 10 s + first frames)
        gpu_ts=eye       (vr69) eye: the GPU timestamps the game reads run at half speed while both eyes are rendered,
                         so its GPU-time-driven detail scaling keeps the mono choice; frame: the real (doubled) frame time
        pad=3            (vr69, vr70 default 6, perf73 default 3) degrees of extra fov on every side of each eye, rendered
                         and reported in the projection layer: the compositor's reprojection then has picture at the edges
                         during head turns instead of black. The eye image then spreads its pixels over a wider fov: on the
                         Quest 3 pad=6 widens the image plane by about 25 % per axis (pixels per degree -20 %), pad=3 by
                         about 12 %. pad=0 = the exact display fov (build 69). perf73: with the late latch (vr71) the
                         picture is ~1-2 deg from the head on average (b72 log), so 3 deg covers most frames; fast turns
                         (>100 deg/s, ~10-12 deg) were beyond 6 as well.
        res=auto         (perf73) the VR back-buffer size, independent of resolution.txt (which is the FLAT size and is
                         ignored in stereo): auto = one eye image of eye_mpix megapixels (default 1.04 = build 72's 960x1080
                         per eye, ~the same GPU cost) in the shape of the runtime's recommended eye image (Quest 3:
                         1680x1760), never above the recommended size: Quest 3 -> 2976x1040 (eyes 992x1040 + UI column).
                         res=WxH = this exact buffer (W = 3 x eye width with the UI column). The choice is logged
                         ("[xr] stereo: VR back buffer ...") with its centre sharpness in pixels per degree.
        eye_mpix=1.04    (perf73) res=auto pixel budget per eye (0.25..6): 1.5 = ~45 % more GPU pixel work, ~20 % sharper.
        fif=1            (vr70) frames in flight while the stereo layout is on (rhi_b1 waits for the GPU to finish frame N-fif
                         before it starts frame N+1). The Quest dxvk.conf says 3, which queued up to 3 GPU frames (~45 ms each
                         in stereo) between the head sample and the display. 0 = keep dxvk.conf; BSI_B1_LATENCY wins over both.
        ao_crop=128      (vr70) the game's AO compute passes cover the whole back buffer once per eye view, the UI column
                         included. With the UI column, the two HDAO blurs now stop this many pixels right of the eye area
                         in world frames (rhi_b1_vr.inc; the eye pixels do not change, cloud shadow-mode check). The AO main
                         pass is not cut (the horizontal blur reads it across the left edge with wrap-around). 0 = off.
        late=1           (vr71) late head-pose latch: the rendering thread re-poses each frame's eye views with the newest head
                         sample right before the scene is rendered and reports that pose to the compositor (about one game
                         frame less head-to-display latency). late=0 = build 70. late_pred_ms=0 (0..100) asks xrLocateViews
                         for the display time + this much instead of the newest sample (opt-in prediction); late_max=30
                         (deg): larger changes are not latched (recenter, tracking glitches).
        ao_eye=2         (ao74) ambient occlusion per eye. UE3 runs its AO compute passes once per eye view, each over the
                         whole [L|R|UI] buffer, and the game's AO maps the pixel position to the view through the AO
                         constant buffer's render-target size (the whole buffer) and the view's tan(half fov): in stereo
                         that is a frustum 3x too wide, so the AO saw the scene squashed 3:1 sideways (dark halos along
                         vertical silhouettes = the "outlines"). 2: rhi_b1 gives each eye's AO main pass the mapping of
                         its own column (its own view's constants), the right eye view's pass over the whole buffer, then
                         the left eye view's pass over the left column; the blurs once (rhi_b1_vr.inc).
                         1 = builds 71-74 (the right eye view's AO for both eyes), 0 = build 70 (both views' passes whole).
        ls_eye=1         (pp76) light shafts per eye: UE3 renders each eye view's light-shaft passes into one shared
                         quarter-size target before either eye's tone-map pass, so both eyes got the RIGHT view's shafts
                         (the left eye's column of it is empty: no sun rays in the left eye). 1: rhi_b1 keeps a copy of the
                         left view's result and gives it to the left view's tone-map pass. 0 = build 75.
        shadow_eye=1     (pp76) sun shadows in the right eye: the whole-scene (cascaded) shadow projection is drawn by the
                         game for the first view only, so the right eye's part of the shadow mask was stale (frozen / wrong
                         sun shadows in the right eye). 1: rhi_b1 replays each projection draw (and its stencil volume and
                         clears) for the right eye column with the right view's constants. 0 = build 75.
        fxaa_eye=1       (pp76) FXAA per eye: the game's FXAA runs over the whole buffer, so at each eye's border its taps and
                         its edge search read the other eye (or the UI column): the eye's outermost column and long flat
                         edges near it got a different blend than the eye alone. 1: each eye column is anti-aliased on its
                         own (eye-sized copy of the input, clamped at the eye's edges like the eye alone). 0 = build 75.
        bloom_eye=1      (pp76) bloom / DOF blend of the right eye: the game runs each eye view's quarter-size bloom chain with
                         the FIRST view's rectangle, so the second chain recomputed the left eye's bloom and the right eye
                         did not get its own (rhi_b1_vr.inc). 1: the second chain is fed the right eye's column. 0 = build 75.
        sharpen=0        (vr71) 1: the projection layer also allows the compositor's sharpening filter (opt-in).
                         vr72: an alias of filter (sharpen=1 = filter=sharpen, sharpen=2 = filter=sharpen_normal, 0 = none).
        filter=none      (vr72) the projection layer's compositor filter: none (default; no layer settings on the eye
                         images, which the compositor upscales ~4x), auto (super-sampling + automatic filter = builds 70/71),
                         sharpen (+ quality sharpening = Meta Quest Super Resolution = build 71 sharpen=1), sharpen_normal.
        route=surface    surface = the game renders straight into the compositor's Android-surface swapchain (route A);
                         copy = into an AImageReader, the XR thread copies each eye into its own OpenXR swapchain
                         (route B, linux/xr_stereo_copy.c; startup only). Needed if the runtime rejects route A.
                         auto = copy with the native renderer. vr72: the default is surface again (builds 69-71: auto);
                         route B ran on a Quest for the first time in build 71 and that run looked wrong (vr72 README).
        aer=0            alternate-eye rendering (route B only): one eye per frame, the other eye keeps its last image
                         (no second scene render: ~mono cost, each eye updates at half the game fps)
     BSI_VR_STEREO_FAKE=1    no OpenXR (Linux cloud harness): synthetic Quest-3-like eye views, head fixed at the origin.
   Off by default; nothing here runs unless the hooked part007 is linked AND stereo is enabled.

   Playability additions (regress/vr, notes/VR_STEREO.md §11), all options in the same file, read at startup unless noted:
        ui=1             UI layer: the back buffer becomes [ left eye | right eye | UI column ]; the HUD canvas, subtitles,
                         console and every Scaleform movie are laid out into a 16:9 rect in the UI column (linux/vr_ui.c) and
                         xr_glue shows that rect as its own quad layer. ui=WxH sets the rect (default: width/3, 16:9).
                         The buffer is then 3 x eye width (res=auto does this; perf73: resolution.txt is not used in stereo).
                         ui=0 = the old two-column layout.
        ui_lock=head     head | world | follow  (live)      ui_dist=1.8 ui_width=1.6 ui_y=-0.1 (m)  ui_follow=30 (deg)
        ui_proj=1        HUD / Scaleform world->screen projection = the UI quad's frustum, so markers point at their target
        ui_blend=alpha   alpha | add | opaque (live)        ui_alpha=rhi   game | rhi | luma
        mono_aspect=1.778  shape of frames that fill the whole buffer (Bink movies, loading)   mono_src=full|ui (live)
   Controller aim has its own switch file, /sdcard/BSI/vr_aim.txt (linux/vr_aim.c); this file only keeps the rendered view
   on the head while the camera (= the gun) follows the controller (vr_stereo_view_ctor_args).

   Native renderer (BSI_RHI=b1n, job vr69, notes/VR_STEREO.md §17). The game side above is renderer-independent; b1n adds:
     - route=auto (default): route B (copy) when the native renderer is on, route A (surface) with the bridge. Route B is
       exact: the XR thread takes the presented images in order and asks rhi_b1 which Present each one is
       (vr_stereo_image_presented / vr_stereo_image_present), and rhi_b1 presents FIFO into the AImageReader (no frame is
       dropped, so the Nth image is the Nth queued present). route=surface / route=copy force a route.
     - rhi_b1 checks the tags: every native Present reports whether the frame drew a right-eye scene viewport; that is
       compared with the ring entry the game thread made for that Present ("[vr_stereo] present tags: ..." every 10 s).
     - the UI column's coverage alpha is made in rhi_b1 (D3D11 blend state while the back buffer is render target 0,
       linux/rhi_b1_vr.inc) instead of linux/vr_ui_rhi.c (UE3 RHI vtable patch), which stays the bridge-mode path.
     - mono frames (no vr_stereo.txt) with head-look on are tagged with the head orientation they were rendered with
       (VS_KIND_MONOHEAD), so xr_glue can world-lock the flat screen to that orientation (vr_head.txt reproject=1) and log
       how far the shown image is from the head during turns (head-turn artefact of build 67). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "vr_stereo.h"

#ifndef VS_NO_GUEST
extern uint8_t *M;
extern volatile long g_br_presents;                     /* bridge.c: swapchain Present count */
int vr_head_enabled(void) __attribute__((weak));       /* vr_head.c */
int vr_aim_view_q(double Q[9]) __attribute__((weak));  /* vr_aim.c: controller aim (view stays on the head) */
int vr_head_rendered(float q[4]) __attribute__((weak)); /* vr_head.c: LOCAL orientation the camera got this frame (vr69) */
int vr_late_install(void) __attribute__((weak));        /* vr_ui_rhi.c (vr71): FDrawSceneCommand::Execute hook */
int xr_locate_eyes(long long xr_time, VsViews *out) __attribute__((weak));   /* xr_glue.c (vr71): late_pred_ms */
#endif

/* ---------------- guest layout (BSI, Win32 VAs / offsets; PC lab 02/03, sdk/VR_NOTES.md) ---------------- */
#define CSV_RET_FROM_DRAW 0x0066a91cu   /* return address of the CalcSceneView call in UGameViewportClient::Draw */
#define LP_ACTOR      0x2C    /* UPlayer.Actor (PlayerController) */
#define LP_ORIGIN     0x54    /* ULocalPlayer.Origin  FVector2D */
#define LP_SIZE       0x5C    /* ULocalPlayer.Size    FVector2D */
#define LP_VIEWSTATE  0x74    /* ULocalPlayer.ViewState (FSceneViewStateInterface*) */
#define PC_CAMERA     0x240   /* APlayerController.PlayerCamera */
#define CAM_FLAGS     0x20C   /* ACamera: bLockedFOV 1, bConstrainAspectRatio 2, bEnableFading 4, bEnableColorScaling 8 */
#define CTOR_ARG_VIEW 13      /* FSceneView ctor (thiscall, ret 0x60): arg 13 = const FMatrix* ViewMatrix (-> +0x70) */
#define CTOR_ARG_PROJ 14      /*                                      arg 14 = const FMatrix* ProjectionMatrix (-> +0xB0) */

/* ---------------- math (pure; host-tested in regress/vr_stereo_test.c) ---------------- */
/* quaternion (x,y,z,w) -> 3x3 rotation, column-vector convention, row-major storage R[r*3+c] */
void vs_quat_to_mat(const float q[4], double R[9]){
  double x = q[0], y = q[1], z = q[2], w = q[3], n = x*x + y*y + z*z + w*w;
  if(n < 1e-12){ memset(R, 0, 9 * sizeof *R); R[0] = R[4] = R[8] = 1; return; }
  double s = 2.0 / n;
  R[0] = 1 - s*(y*y + z*z); R[1] = s*(x*y - z*w);     R[2] = s*(x*z + y*w);
  R[3] = s*(x*y + z*w);     R[4] = 1 - s*(x*x + z*z); R[5] = s*(y*z - x*w);
  R[6] = s*(x*z - y*w);     R[7] = s*(y*z + x*w);     R[8] = 1 - s*(x*x + y*y);
}

/* Per-eye offset in UE3 view space (x right, y up, z forward; UU) and the eye's rotation relative to the head as a
   row-vector matrix Q (p_eye = p_head_view * Q).  OpenXR head space is x right, y up, z BACK, so  M = diag(1,1,-1).
     d_v = M * R_head^T * (p_eye - anchor) * scale,   anchor = p_head - clamp(p_head)  (pos=1)  or  p_head  (pos=0)
     Q   = M * (R_head^T R_eye) * M                                                                           */
void vs_eye_offset(const VsPose *head, const VsPose *eye, const VsCfg *cfg, double d_v[3], double Q[9]){
  double Rh[9], Re[9]; vs_quat_to_mat(head->q, Rh); vs_quat_to_mat(eye->q, Re);
  double rel[3] = { eye->p[0] - head->p[0], eye->p[1] - head->p[1], eye->p[2] - head->p[2] };
  for(int i = 0; i < 3; i++) rel[i] *= cfg->ipd_scale;   /* scales the eye separation (eyes sit symmetrically about the head) */
  if(cfg->pos){
    double h[3] = { head->p[0], head->p[1], head->p[2] }, len = sqrt(h[0]*h[0] + h[1]*h[1] + h[2]*h[2]);
    if(len > cfg->pos_max && len > 0){ double k = cfg->pos_max / len; h[0] *= k; h[1] *= k; h[2] *= k; }
    for(int i = 0; i < 3; i++) rel[i] += h[i];
  }
  double v[3];   /* R_head^T * rel */
  for(int r = 0; r < 3; r++) v[r] = Rh[0*3 + r] * rel[0] + Rh[1*3 + r] * rel[1] + Rh[2*3 + r] * rel[2];
  d_v[0] = v[0] * cfg->uu_per_m; d_v[1] = v[1] * cfg->uu_per_m; d_v[2] = -v[2] * cfg->uu_per_m;
  double Rr[9];  /* R_head^T R_eye */
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++)
    Rr[r*3 + c] = Rh[0*3 + r] * Re[0*3 + c] + Rh[1*3 + r] * Re[1*3 + c] + Rh[2*3 + r] * Re[2*3 + c];
  static const double sg[3] = { 1, 1, -1 };
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) Q[r*3 + c] = sg[r] * Rr[r*3 + c] * sg[c];
}

/* Game view V (row-vector, world->view: rows 0-2 = R, row 3 = t = -O*R) -> eye view  Ve = [ R*Q ; (t - d_v)*Q ].
   Derivation: Ve = T(-(O+D)) * R * Q with the world offset D = d_v * R^T, and -(O+D)*R = t - d_v.
   Works in whatever space the game's matrix is in (BSI floating sections), because only V's own rows are used. */
void vs_eye_view(const float V[16], const double d_v[3], const double Q[9], float Ve[16]){
  double R[9], t[3];
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) R[r*3 + c] = V[r*4 + c];
  for(int c = 0; c < 3; c++) t[c] = (double)V[12 + c] - d_v[c];
  for(int r = 0; r < 3; r++){
    for(int c = 0; c < 3; c++) Ve[r*4 + c] = (float)(R[r*3 + 0] * Q[0*3 + c] + R[r*3 + 1] * Q[1*3 + c] + R[r*3 + 2] * Q[2*3 + c]);
    Ve[r*4 + 3] = V[r*4 + 3];
  }
  for(int c = 0; c < 3; c++) Ve[12 + c] = (float)(t[0] * Q[0*3 + c] + t[1] * Q[1*3 + c] + t[2] * Q[2*3 + c]);
  Ve[15] = V[15];
}

/* Off-axis projection from the eye's fov angles (radians; left/down negative), UE3 row-vector layout:
     clip.x = x*P00 + z*P20,  clip.y = y*P11 + z*P21,  clip.w = z      (view space x right, y up, z forward)
   P00 = 2/(tr-tl), P20 = -(tr+tl)/(tr-tl), P11 = 2/(tu-td), P21 = -(tu+td)/(tu-td).
   Depth terms (P22, P23, P32) are kept from the game (near 10, infinite far, Z_PRECISION 0.001), or rebuilt for
   near_override > 0 as P32 = -near * P22. */
void vs_eye_proj(const float Pg[16], const float fov[4], float near_override, float Pe[16]){
  double tl = tan(fov[0]), tr = tan(fov[1]), tu = tan(fov[2]), td = tan(fov[3]);
  memset(Pe, 0, 16 * sizeof *Pe);
  Pe[0]  = (float)(2.0 / (tr - tl));
  Pe[8]  = (float)(-(tr + tl) / (tr - tl));
  Pe[5]  = (float)(2.0 / (tu - td));
  Pe[9]  = (float)(-(tu + td) / (tu - td));
  Pe[10] = Pg[10]; Pe[11] = Pg[11]; Pe[14] = Pg[14];
  if(near_override > 0) Pe[14] = -near_override * Pg[10];
}

/* Rotate a view about its own camera position by the view-space rotation Q (row vectors): Vr = V * diag(Q, 1), i.e.
   [ R*Q ; t*Q ]. Used for the controller-aim correction (aim view -> head view), see vr_aim.c. */
void vs_view_rotate(const float V[16], const double Q[9], float Vr[16]){
  static const double z[3] = { 0, 0, 0 };
  vs_eye_view(V, z, Q, Vr);
}

/* Back-buffer layout. ui off: [ L | R ], eye = W/2. ui on: [ L | R | column ], UI rect centred in the column (centred
   vertically too, so the vertical flip of the layer image does not move it). Auto UI size: a third of the width, 16:9. */
void vs_layout(int W, int H, const VsCfg *c, VsLayout *L){
  memset(L, 0, sizeof *L); L->W = W; L->H = H; L->eye_w = W / 2;
  if(!c->ui || W < 64 || H < 64) return;
  int uw = c->ui_w > 0 ? c->ui_w : W / 3, uh = c->ui_h > 0 ? c->ui_h : (uw * 9 + 8) / 16;
  if(uw > W / 2) uw = W / 2;
  if(uh > H){ uh = H; if(!c->ui_w) uw = (uh * 16 + 4) / 9; }
  int ew = (W - uw) / 2;
  if(ew < 64 || uw < 64 || uh < 36) return;
  L->ui = 1; L->eye_w = ew; L->ui_w = uw; L->ui_h = uh;
  L->ui_x = 2 * ew + (W - 2 * ew - uw) / 2; L->ui_y = (H - uh) / 2;
}

/* Present tagging (GT, at the end of a stereo CalcSceneView): the frame will be shown by the first Present after every
   frame enqueued before it.  UE3 lets the game thread run at most one frame ahead of the rendering thread, so:
     assigned = max(last_assigned + 1, presents_now + 1)
   - GT one frame ahead (frame N-1 not presented yet): presents_now = last - 1  ->  last + 1
   - GT not ahead (N-1 already presented):            presents_now = last      ->  last + 1
   - extra Presents without a stereo view (loading, movies, menus without world): presents_now > last -> presents_now + 1 */
uint32_t vs_assign_present(uint32_t last_assigned, uint32_t presents_now){
  uint32_t a = last_assigned + 1, b = presents_now + 1;
  return (int32_t)(b - a) > 0 ? b : a;
}

/* XR side: the newest Present index first seen at least lag_s ago (history p[]/t[] of n entries, oldest first).
   lag_s covers the time between the game's Present call and the buffer being latched by the compositor (GPU still
   rendering it). Returns 0 if none is old enough. */
int vs_pick_present(const uint32_t *p, const double *t, int n, double now, double lag_s, uint32_t *out){
  for(int i = n - 1; i >= 0; i--) if(now - t[i] >= lag_s){ *out = p[i]; return 1; }
  return 0;
}

/* vr70, AO crop (rhi_b1_vr.inc): a compute dispatch of x * y groups writing a uw x uh UAV. If the UAV has the back buffer's size
   (UE3 may round scene targets up by a few pixels) and the dispatch is "ceil(size / group size)" with power-of-two group
   sizes in both axes, return the group count in x that still covers drawn_x + margin pixels (never more than x) and the
   group width in *gx; otherwise *gx = 0 and x is returned (the dispatch is left alone). */
uint32_t vs_crop_groups(uint32_t x, uint32_t y, uint32_t uw, uint32_t uh, int bb_w, int bb_h, int drawn_x, int margin, uint32_t *gx){
  *gx = 0;
  if(!x || !y || bb_w <= 0 || bb_h <= 0 || drawn_x <= 0 || margin < 0) return x;
  if((int)uw < bb_w || (int)uw > bb_w + 32 || (int)uh < bb_h || (int)uh > bb_h + 32) return x;
  uint64_t g = 1, h = 1;
  while(g * x < uw && g < 4096) g <<= 1;
  while(h * y < uh && h < 4096) h <<= 1;
  if(g * x < uw || g * x - uw >= g || h * y < uh || h * y - uh >= h) return x;
  uint64_t want = (uint64_t)drawn_x + (uint64_t)margin; if(want > uw) want = uw;
  uint64_t nx = (want + g - 1) / g;
  *gx = (uint32_t)g;
  return nx < x ? (uint32_t)nx : x;
}

/* ---- vr71: late head-pose latch, pure parts (host-tested in regress/vr/vr71/vr71_test.c) ----
   The game thread builds each eye's view with the head pose it sampled at CalcSceneView; the render thread starts that
   frame's scene about one game frame later. At that point vr_stereo_late_latch() takes the newest eye pose and turns
   every view matrix of the frame's FViewInfo (UE3's render-thread copy of the FSceneView) about the eye by the change in
   eye orientation, then reports the new orientation with the frame. Eye positions stay those the frame was rendered from.
   D below is that change as a UE3 view-space rotation (row vectors, x right, y up, z forward): for a world point the new
   view-space coordinates are the old ones times D, so  View' = View * diag(D, 1). */
double vs_late_delta(const float q0[4], const float q1[4], double D[9]){
  double R0[9], R1[9], Rr[9]; vs_quat_to_mat(q0, R0); vs_quat_to_mat(q1, R1);
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++)   /* R0^T R1 (OpenXR, column vectors: new eye -> old eye) */
    Rr[r*3 + c] = R0[0*3 + r] * R1[0*3 + c] + R0[1*3 + r] * R1[1*3 + c] + R0[2*3 + r] * R1[2*3 + c];
  static const double sg[3] = { 1, 1, -1 };             /* M = diag(1,1,-1): OpenXR z back -> UE3 z forward (as vs_eye_offset's Q) */
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) D[r*3 + c] = sg[r] * Rr[r*3 + c] * sg[c];
  double tr = (Rr[0] + Rr[4] + Rr[8] - 1.0) * 0.5; if(tr > 1) tr = 1; if(tr < -1) tr = -1;
  return acos(tr) * 180.0 / M_PI;
}
/* FSceneView fields (BSI, from the ctor f_0072ce80; FViewInfo = FSceneView copy + render-thread data, 0x11A0 bytes) */
#define VI_VIEW   0x70    /* ViewMatrix */
#define VI_PROJ   0xB0    /* ProjectionMatrix */
#define VI_TVIEW  0x170   /* TranslatedViewMatrix = Translate(ViewOrigin) * View */
#define VI_TVP    0x1B0   /* TranslatedViewProjectionMatrix */
#define VI_ITVP   0x1F0   /* InvTranslatedViewProjectionMatrix */
#define VI_VP     0x240   /* ViewProjectionMatrix */
#define VI_IPROJ  0x280   /* inverse(Projection) */
#define VI_IVIEW  0x2C0   /* InvViewMatrix */
#define VI_IVP    0x300   /* InvViewProjectionMatrix = InvProj * InvView */
#define VI_STW    0x340   /* ScreenToTranslatedWorld = [1;1;(0,0,P22,1);(0,0,P32,0)] * InvTranslatedViewProjection */
#define VI_HASNEAR 0x4B0  /* bHasNearClippingPlane */
#define VI_NEAR   0x4C0   /* NearClippingPlane (from ViewProjection's third column) */
/* not touched: ViewOrigin 0x380 / PreViewTranslation 0x230 (the eye does not move), the culling frustum 0x390 (the game
   thread's pose: the overscan pad covers the difference), everything derived from the projection or the view rect */
static void m4_mul(const double *a, const double *b, double *o){
  for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++){ double t = 0; for(int k = 0; k < 4; k++) t += a[r*4 + k] * b[k*4 + c]; o[r*4 + c] = t; } }
static int m4_inv(const double *m, double *o){   /* Gauss-Jordan with partial pivoting */
  double a[4][8];
  for(int r = 0; r < 4; r++) for(int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r*4 + c] : (c - 4 == r);
  for(int c = 0; c < 4; c++){
    int p = c; for(int r = c + 1; r < 4; r++) if(fabs(a[r][c]) > fabs(a[p][c])) p = r;
    if(fabs(a[p][c]) < 1e-30) return 0;
    if(p != c) for(int k = 0; k < 8; k++){ double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
    double iv = 1.0 / a[c][c]; for(int k = 0; k < 8; k++) a[c][k] *= iv;
    for(int r = 0; r < 4; r++) if(r != c){ double f = a[r][c]; if(f != 0) for(int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; } }
  for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++) o[r*4 + c] = a[r][c + 4];
  return 1; }
static void ld16(const uint8_t *p, double *d){ float f[16]; memcpy(f, p, 64); for(int i = 0; i < 16; i++) d[i] = f[i]; }
static void st16(uint8_t *p, const double *d){ float f[16]; for(int i = 0; i < 16; i++) f[i] = (float)d[i]; memcpy(p, f, 64); }
void vs_late_patch_view(uint8_t *vi, const double D[9]){
  float Vf[16], TVf[16], Vn[16], TVn[16]; static const double z3[3] = { 0, 0, 0 };
  memcpy(Vf, vi + VI_VIEW, 64); memcpy(TVf, vi + VI_TVIEW, 64);
  vs_eye_view(Vf, z3, D, Vn); vs_eye_view(TVf, z3, D, TVn);   /* [R*D ; t*D] (the eye-view builder with a zero offset) */
  double V[16], TV[16], P[16], IP[16], IV[16], t[16], u[16];
  for(int i = 0; i < 16; i++){ V[i] = Vn[i]; TV[i] = TVn[i]; }
  ld16(vi + VI_PROJ, P); ld16(vi + VI_IPROJ, IP); ld16(vi + VI_IVIEW, IV);
  memcpy(vi + VI_VIEW, Vn, 64); memcpy(vi + VI_TVIEW, TVn, 64);
  m4_mul(TV, P, t); st16(vi + VI_TVP, t);
  { double tf[16]; ld16(vi + VI_TVP, tf); if(m4_inv(tf, u)) st16(vi + VI_ITVP, u); else ld16(vi + VI_ITVP, u); }
  m4_mul(V, P, t); st16(vi + VI_VP, t);
  { double nv[16]; memcpy(nv, IV, sizeof nv);   /* InvView = [R^T ; O]: rotation rows become D^T R^T, the origin row stays */
    for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) nv[r*4 + c] = D[0*3 + r] * IV[0*4 + c] + D[1*3 + r] * IV[1*4 + c] + D[2*3 + r] * IV[2*4 + c];
    st16(vi + VI_IVIEW, nv); ld16(vi + VI_IVIEW, IV); }
  m4_mul(IP, IV, t); st16(vi + VI_IVP, t);
  { double S[16] = { 1,0,0,0, 0,1,0,0, 0,0,P[10],1, 0,0,P[14],0 }; m4_mul(S, u, t); st16(vi + VI_STW, t); }
  uint32_t hn; memcpy(&hn, vi + VI_HASNEAR, 4);
  if(hn == 1){ double vp[16]; ld16(vi + VI_VP, vp);
    double x = vp[2], y = vp[6], zz = vp[10], w = vp[14], l2 = x*x + y*y + zz*zz;
    if(l2 > 0){ double k = 1.0 / sqrt(l2); float pl[4] = { (float)(-x * k), (float)(-y * k), (float)(-zz * k), (float)(w * k) }; memcpy(vi + VI_NEAR, pl, 16); } }
}

/* vr71: route B / reticle swapchain format, OpenXR two-call idiom (build 70 passed a fixed 64-entry array; the Quest 3 runtime
   lists 71 formats, so xrEnumerateSwapchainFormats returned XR_ERROR_SIZE_INSUFFICIENT and route B never started).
   Format: GL_SRGB8_ALPHA8 first. The game's back buffer is RGBA8 UNORM holding display-ready (sRGB-encoded) values; the
   compositor treats an SRGB8 swapchain as sRGB-encoded (decodes when sampling) and a UNORM one as linear. So the copy
   linearises and the SRGB8 render target re-encodes on write: the stored bytes are the game's bytes (round trip, highp)
   and the compositor decodes them once (correct). A UNORM swapchain would need linear values in 8 bits (dark banding);
   if it is the only choice the copy also writes linear values (lin=1 is kept for it, see xr_stereo_copy.c). */
int vs_pick_swapchain_format(vs_enum_fmt_fn enumf, void *ctx, int64_t *fmt, uint32_t *count, int64_t *list, uint32_t cap){
  *fmt = 0; *count = 0;
  int r = 0;
  for(int attempt = 0; attempt < 3; attempt++){
    uint32_t n = 0; r = enumf(ctx, 0, &n, NULL);
    if(r != 0){ *count = n; return r; }
    if(!n || n > 4096){ *count = n; return r; }
    int64_t *buf = (int64_t *)calloc(n, sizeof *buf); if(!buf) return -1;
    uint32_t got = 0; r = enumf(ctx, n, &got, buf);
    if(r == -11){ free(buf); continue; }   /* XR_ERROR_SIZE_INSUFFICIENT: the list grew between the calls, ask again */
    if(r == 0){
      if(got > n) got = n;
      *count = got;
      for(uint32_t i = 0; i < got; i++){ if(buf[i] == 0x8C43){ *fmt = 0x8C43; break; } if(buf[i] == 0x8058 && !*fmt) *fmt = 0x8058; }
      if(list) for(uint32_t i = 0; i < got && i < cap; i++) list[i] = buf[i];
    }
    free(buf); return r;
  }
  return r;
}

void vs_parse_opts(VsCfg *c, const char *s){
  char w[64]; int n;
  while(s && sscanf(s, "%63s%n", w, &n) == 1){
    s += n; char *eq = strchr(w, '='); if(!eq) continue; *eq = 0; const char *v = eq + 1; float f = strtof(v, NULL);
    if(!strcmp(w, "ipd") && f >= 0 && f <= 4) c->ipd_scale = f;
    else if(!strcmp(w, "uu") && f > 1 && f < 1000) c->uu_per_m = f;
    else if(!strcmp(w, "pos")) c->pos = atoi(v) != 0;
    else if(!strcmp(w, "posmax") && f >= 0 && f <= 2) c->pos_max = f;
    else if(!strcmp(w, "near") && f >= 0 && f <= 100) c->near_override = f;
    else if(!strcmp(w, "vs2")) c->vs2 = atoi(v) != 0;
    else if(!strcmp(w, "lag_ms") && f >= 0 && f <= 200) c->lag_ms = f;
    else if(!strcmp(w, "pred_ms") && f >= -50 && f <= 200) c->pred_ms = f;
    else if(!strcmp(w, "log")) c->log = atoi(v);
    else if(!strcmp(w, "route")) c->copy = !strcmp(v, "copy") ? 1 : !strcmp(v, "surface") ? 0 : -1;   /* -1 auto (vr69) */
    else if(!strcmp(w, "aer")) c->aer = atoi(v) != 0;
    else if(!strcmp(w, "ui")){ int a, b; if(sscanf(v, "%dx%d", &a, &b) == 2 && a >= 160 && b >= 90 && a <= 8192 && b <= 8192){ c->ui = 1; c->ui_w = a; c->ui_h = b; }
                               else { c->ui = atoi(v) != 0; c->ui_w = c->ui_h = 0; } }
    else if(!strcmp(w, "ui_lock")) c->ui_lock = !strcmp(v, "world") ? 1 : !strcmp(v, "follow") ? 2 : 0;
    else if(!strcmp(w, "ui_dist") && f >= 0.3f && f <= 20) c->ui_dist = f;
    else if(!strcmp(w, "ui_width") && f >= 0.1f && f <= 20) c->ui_width = f;
    else if(!strcmp(w, "ui_y") && f >= -5 && f <= 5) c->ui_y = f;
    else if(!strcmp(w, "ui_follow") && f >= 0 && f <= 180) c->ui_follow_deg = f;
    else if(!strcmp(w, "ui_proj")) c->ui_proj = atoi(v) != 0;
    else if(!strcmp(w, "ui_blend")) c->ui_blend = !strcmp(v, "add") ? 1 : !strcmp(v, "opaque") ? 2 : 0;
    else if(!strcmp(w, "ui_alpha")) c->ui_alpha = !strcmp(v, "game") ? 0 : !strcmp(v, "luma") ? 2 : 1;
    else if(!strcmp(w, "mono_aspect") && f >= 0.5f && f <= 4) c->mono_aspect = f;
    else if(!strcmp(w, "mono_src")) c->mono_src = !strcmp(v, "ui");
    else if(!strcmp(w, "pad") && f >= 0 && f <= 15) c->fov_pad = f;   /* vr69 */
    else if(!strcmp(w, "gpu_ts")) c->gpu_ts_eye = strcmp(v, "frame") != 0;   /* vr69: eye (default) | frame */
    else if(!strcmp(w, "fif") && f >= 0 && f <= 8) c->fif = (int)f;            /* vr70 */
    else if(!strcmp(w, "ao_crop") && f >= 0 && f <= 4096) c->ao_crop = (int)f; /* vr70 */
    else if(!strcmp(w, "late")) c->late = atoi(v) != 0;                          /* vr71 */
    else if(!strcmp(w, "late_pred_ms") && f >= 0 && f <= 100) c->late_pred_ms = f;
    else if(!strcmp(w, "late_max") && f > 0 && f <= 90) c->late_max_deg = f;
    else if(!strcmp(w, "ao_eye")){ int k = atoi(v); c->ao_eye = k < 0 ? 0 : k > 2 ? 2 : k; }   /* ao74: 0 build 70, 1 builds 71-74, 2 per eye */
    else if(!strcmp(w, "ls_eye")) c->ls_eye = atoi(v) != 0;           /* pp76 */
    else if(!strcmp(w, "shadow_eye")) c->shadow_eye = atoi(v) != 0;   /* pp76 */
    else if(!strcmp(w, "fxaa_eye")) c->fxaa_eye = atoi(v) != 0;       /* pp76 */
    else if(!strcmp(w, "bloom_eye")) c->bloom_eye = atoi(v) != 0;     /* pp76 */
    else if(!strcmp(w, "sharpen")){ int k = atoi(v); c->sharpen = k == 1 ? 1 : k == 2 ? 2 : 0;   /* vr72: alias of filter */
                                    c->filter = k == 1 ? VS_FILTER_SHARPEN : k == 2 ? VS_FILTER_SHARPEN_NORMAL : VS_FILTER_NONE; }
    else if(!strcmp(w, "filter")){ int k = !strcmp(v, "auto") || !strcmp(v, "supersample") ? VS_FILTER_AUTO : !strcmp(v, "sharpen") ? VS_FILTER_SHARPEN :
                                           !strcmp(v, "sharpen_normal") ? VS_FILTER_SHARPEN_NORMAL : !strcmp(v, "none") ? VS_FILTER_NONE : -1;   /* vr72 */
                                   if(k >= 0){ c->filter = k; c->sharpen = k == VS_FILTER_SHARPEN ? 1 : k == VS_FILTER_SHARPEN_NORMAL ? 2 : 0; } }
    else if(!strcmp(w, "res")){ int a, b; if(sscanf(v, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 360 && a <= 8192 && b <= 4096){ c->res_w = a & ~1; c->res_h = b & ~1; }   /* perf73 */
                                else if(!strcmp(v, "auto")) c->res_w = c->res_h = 0; }
    else if(!strcmp(w, "eye_mpix") && f >= 0.25f && f <= 6.f) c->eye_mpix = f;   /* perf73 */
  }
}

/* perf73: VR back-buffer size. Explicit res=WxH wins. Auto: one eye image of eye_mpix megapixels with the runtime's
   recommended aspect (its eye images are shaped for the lens/fov; Quest 3: 1680x1760), never larger than the recommended
   size, multiples of 8, the buffer = [ left | right | UI column (eye width, or ui=WxH's width) ]. */
int vs_vr_size(const VsCfg *c, int rec_w, int rec_h, int *W, int *H){
  if(c->res_w >= 640 && c->res_h >= 360){ *W = c->res_w; *H = c->res_h; return 1; }
  int how = rec_w >= 64 && rec_h >= 64 ? 2 : 3;
  double aspect = how == 2 ? (double)rec_w / rec_h : 0.925;   /* 3: Quest 3 display fov tangents (54+40 x 44+55 deg) */
  double mp = c->eye_mpix >= 0.25f ? c->eye_mpix : 1.04;
  double eh = sqrt(mp * 1e6 / aspect);
  if(how == 2 && eh > rec_h) eh = rec_h;
  if(how == 2 && eh * aspect > rec_w) eh = rec_w / aspect;
  int h = ((int)eh) & ~7; if(h < 360) h = 360;
  int ew = ((int)(aspect * h)) & ~7; if(ew < 320) ew = 320;
  int uw = !c->ui ? 0 : c->ui_w > 0 ? c->ui_w : ew;
  int w = 2 * ew + uw; if(w > 8192) w = 8192;
  if(h > 4096) h = 4096;
  *W = w & ~1; *H = h; return how;
}
/* perf73: centre pixels per degree: the projection maps tangent t to pixels linearly, so at the centre one degree is
   (pi/180) tangent units. a0 = left/down angle (negative), a1 = right/up (positive); the pad widens both, capped at 80 deg. */
double vs_px_per_deg(int eye_px, float a0, float a1, float pad_deg){
  double p = pad_deg * M_PI / 180.0, lim = 80.0 * M_PI / 180.0, lo = a0 - p, hi = a1 + p;
  if(lo < -lim) lo = -lim;
  if(hi > lim) hi = lim;
  double span = tan(hi) - tan(lo); return span > 0 ? eye_px / span * M_PI / 180.0 : 0; }

/* ao74: the game's AO main pass maps pixel p (buffer x) to view x / z = (2 (p + .5) inv - 1) tan, with inv = 1 / the width of its
   render target W and tan = its tan(half fov x). The game does not take tan from the view's projection: it is
   tan(22.5 deg * W / H) (cloud logs: 768x272 -> 2.008271, 256x272 -> 0.387402, tan y = tan 22.5 deg always), i.e. the
   half-angle grows with the target's aspect. A mono view of one eye column (width ew) would therefore get
   t_eye = tan(atan(tan) * ew / W) and inv = 1 / ew. For the column at x0 the same mapping, (2 (p + .5 - x0) / ew - 1) t_eye,
   is inv' = 1 / (2 x0 + ew), tan' = t_eye (2 x0 + ew) / ew (x0 = 0: exactly the mono view's 1/ew and t_eye). */
float vs_ao_eye_tan(float tan_game, float W, int ew){
  if(!(W > 0) || ew <= 0 || !(tan_game > 0)) return tan_game;
  return (float)tan(atan((double)tan_game) * (double)ew / (double)W); }
void vs_ao_eye_map(float tan_game, float W, int x0, int ew, float *inv, float *tan_x){
  double w = 2.0 * x0 + ew; float te = vs_ao_eye_tan(tan_game, W, ew);
  *inv = (float)(1.0 / w); *tan_x = x0 ? (float)((double)te * w / ew) : te; }

/* pp76: constants for drawing the sun's whole-scene shadow projection again for the right eye (rhi_b1_vr.inc, shadow_eye).
   The game draws it for the left view only. Its VS (FShadowProjectionVertexShader) computes clip = v * TVP (VS cb1 rows 0..3:
   the view's TranslatedViewProjection, row 4 the eye position O) for volume vertices v in the left view's translated space
   (p - O_L), and o1 = clip.xyw * M3 (VS cb0 rows 0..2: the inverse of TVP's x,y,w columns), i.e. the position relative to the
   eye centre E; the PS scales that by the scene depth and maps it with PS cb0 rows 0..3 (rows 0..2: world -> shadow, row 3: the
   shadow coordinates of E). For the right view: TVP_R re-based to the left translated space (row 3 -= (O_R - O_L) * rows 0..2),
   M3 = inverse(TVP_R x,y,w), row 3 += (E_R - E_L) * rows 0..2 with E = O + e, e = -t * inverse(A) (t = TVP row 3 x,y,w: the
   small offset between the translated origin and the eye). When both views are bit-identical (cloud FAKE_SAME) the left
   view's values are returned unchanged. vs0_l / ps0_l may be NULL (clears, stencil volumes: only vs1_out). 0 = degenerate. */
static int inv3(const double a[9], double o[9]){
  double c0 = a[4]*a[8] - a[5]*a[7], c1 = a[5]*a[6] - a[3]*a[8], c2 = a[3]*a[7] - a[4]*a[6], det = a[0]*c0 + a[1]*c1 + a[2]*c2;
  if(!(fabs(det) > 1e-30)) return 0;
  double k = 1.0 / det;
  o[0] = c0*k; o[1] = (a[2]*a[7] - a[1]*a[8])*k; o[2] = (a[1]*a[5] - a[2]*a[4])*k;
  o[3] = c1*k; o[4] = (a[0]*a[8] - a[2]*a[6])*k; o[5] = (a[2]*a[3] - a[0]*a[5])*k;
  o[6] = c2*k; o[7] = (a[1]*a[6] - a[0]*a[7])*k; o[8] = (a[0]*a[4] - a[1]*a[3])*k; return 1; }
static int eye_of(const float *vs1, double M3[9], double e[3]){   /* A = TVP rows 0..2, columns x,y,w; e = -t A^-1 */
  double A[9]; static const int col[3] = { 0, 1, 3 };
  for(int r = 0; r < 3; r++) for(int c = 0; c < 3; c++) A[r*3 + c] = vs1[r*4 + col[c]];
  if(!inv3(A, M3)) return 0;
  double t[3] = { vs1[12], vs1[13], vs1[15] };
  for(int c = 0; c < 3; c++) e[c] = -(t[0]*M3[0*3 + c] + t[1]*M3[1*3 + c] + t[2]*M3[2*3 + c]);
  return 1; }
int vs_shadow_eye_consts(const float vs1_l[20], const float vs1_r[20], const float vs0_l[12], const float ps0_l[16],
                         float vs1_out[20], float vs0_out[12], float ps0_row3[4]){
  if(!memcmp(vs1_l, vs1_r, 20 * sizeof(float))){   /* the same view: the game's own values */
    memcpy(vs1_out, vs1_l, 20 * sizeof(float));
    if(vs0_l && vs0_out) memcpy(vs0_out, vs0_l, 12 * sizeof(float));
    if(ps0_l && ps0_row3) memcpy(ps0_row3, ps0_l + 12, 4 * sizeof(float));
    return 1; }
  double ML[9], MR[9], eL[3], eR[3];
  if(!eye_of(vs1_l, ML, eL) || !eye_of(vs1_r, MR, eR)) return 0;
  double d[3], D[3];
  for(int i = 0; i < 3; i++){ d[i] = (double)vs1_r[16 + i] - (double)vs1_l[16 + i]; D[i] = d[i] + eR[i] - eL[i]; }
  memcpy(vs1_out, vs1_r, 20 * sizeof(float));
  for(int c = 0; c < 4; c++) vs1_out[12 + c] = (float)((double)vs1_r[12 + c] - (d[0]*vs1_r[0*4 + c] + d[1]*vs1_r[1*4 + c] + d[2]*vs1_r[2*4 + c]));
  if(vs0_out){ for(int r = 0; r < 3; r++){ for(int c = 0; c < 3; c++) vs0_out[r*4 + c] = (float)MR[r*3 + c]; vs0_out[r*4 + 3] = 0.f; } }
  if(ps0_l && ps0_row3) for(int c = 0; c < 4; c++)
    ps0_row3[c] = (float)((double)ps0_l[12 + c] + D[0]*ps0_l[0*4 + c] + D[1]*ps0_l[1*4 + c] + D[2]*ps0_l[2*4 + c]);
  return 1; }

/* pp76: copy plan of the per-eye FXAA input (rhi_b1_pp76.inc, fxaa_eye): a buffer-sized copy holding the eye column [x0, x1)
   of the input at its own place, and in a margin of m columns on each side (clipped to [0, W)) copies of the eye's first /
   last column (what a clamped sampler returns past the edge of the eye rendered alone). Entries {from_input, src x, dst x,
   width}: from_input 1 = from the game's input, 0 = inside the copy itself (regions never overlap; the source is already
   filled). Doubling copies: 1 + log2(m) per margin. Returns the number of entries (<= max), 0 if the arguments are bad. */
int vs_fx_copy_plan(int x0, int x1, int W, int m, int plan[][4], int max){
  int n = 0;
  if(x0 < 0 || x1 <= x0 || x1 > W || m < 0 || !plan) return 0;
#define VS_FXP(a, b, c, d) do{ if(n >= max) return 0; plan[n][0] = (a); plan[n][1] = (b); plan[n][2] = (c); plan[n][3] = (d); n++; }while(0)
  VS_FXP(1, x0, x0, x1 - x0);
  int lo = x0 - m < 0 ? 0 : x0 - m;
  if(x0 > lo){ VS_FXP(1, x0, x0 - 1, 1);
    for(int k = 1; x0 - k > lo; k *= 2){ int w = k < x0 - k - lo ? k : x0 - k - lo; VS_FXP(0, x0 - k, x0 - k - w, w); } }
  int hi = x1 + m > W ? W : x1 + m;
  if(hi > x1){ VS_FXP(1, x1 - 1, x1, 1);
    for(int k = 1; x1 + k < hi; k *= 2){ int w = k < hi - x1 - k ? k : hi - x1 - k; VS_FXP(0, x1, x1 + k, w); } }
#undef VS_FXP
  return n; }

void vs_cfg_defaults(VsCfg *c){
  memset(c, 0, sizeof *c);
  c->ipd_scale = 1.f; c->uu_per_m = 100.f; c->pos = 1; c->pos_max = 0.35f; c->vs2 = 1; c->log = 1;
  c->ui = 1; c->ui_lock = 0; c->ui_dist = 1.8f; c->ui_width = 1.6f; c->ui_y = -0.1f; c->ui_follow_deg = 30.f; c->ui_proj = 1;
  c->ui_blend = 0; c->ui_alpha = 1; c->mono_aspect = 16.f / 9.f;
  c->copy = 0;    /* vr72: route=surface (A) by default again; route=auto (vr69-71 default) = copy (B) with the native renderer */
  c->gpu_ts_eye = 1;
  c->fov_pad = 3.f; c->fif = 1; c->ao_crop = 128;   /* vr70 (notes/VR_STEREO.md §18); perf73: pad 6 -> 3 (late latch, §21) */
  c->res_w = c->res_h = 0; c->eye_mpix = 1.04f;      /* perf73: res=auto, ~build 72's per-eye pixel count */
  c->late = 1; c->late_pred_ms = 0.f; c->late_max_deg = 30.f;   /* vr71 (notes/VR_STEREO.md §19) */
  c->ao_eye = 2;   /* ao74: each eye's AO with its own view's constants (rhi_b1_vr.inc); vr71's 1 = one AO chain for both eyes */
  c->ls_eye = 1; c->shadow_eye = 1; c->fxaa_eye = 1; c->bloom_eye = 1;   /* pp76: light shafts, sun shadow mask and FXAA per eye (rhi_b1_vr.inc) */
  c->filter = VS_FILTER_NONE;   /* vr72: no compositor filter on the projection layer (build 71: auto; sharpen=1 opt-in) */
}

#ifndef VS_NO_GUEST
/* ---------------- shared state ---------------- */
static VsCfg g_cfg; static int g_cfg_ok;
static void cfg_init(void){ if(!g_cfg_ok){ vs_cfg_defaults(&g_cfg); g_cfg_ok = 1; } }
static int g_layout = -1;        /* double-wide layout chosen at startup: -1 unknown, 0 mono, 1 stereo */
static int g_ui0 = -1, g_ui_w0, g_ui_h0;   /* UI column chosen at startup with the layout (the swapchain size is fixed then) */
static volatile int g_ui_game;   /* the game side really uses the UI column (new hook with the viewport size): XR may rely on it */
static int g_active;             /* stereo rendering on right now (file toggle) */
static volatile int g_xr_off;    /* UI layout, but the XR side cannot show stereo: one mono view over the eye area */
static int g_fake = -1;
static double g_next_cfg;

/* latest eye views from the XR thread (seqlock) */
static volatile uint32_t g_vseq; static VsViews g_views; static volatile int g_have_views;

/* frames in flight: GT writes, XR thread reads (per-slot seqlock) */
#define VS_RING 16
typedef struct { volatile uint32_t seq; uint32_t present; int mask, kind; VsViews v; double t_sample;
                 float ve[2][16]; int have_ve; double t_gt; } VsSlot;   /* vr71: each eye's view matrix (late latch), game-thread sample time */
static VsSlot g_ring[VS_RING]; static volatile uint32_t g_ring_n; static uint32_t g_last_assigned;

/* GT state of the current CalcSceneView pair */
static float g_cur_ve[2][16]; static int g_cur_have_ve;   /* vr71: the eye view matrices this pair's ctor calls got */
static int g_cur_eye = -1, g_cur_mask; static VsViews g_cur; static uint32_t g_vs2, g_frame_no; static int g_route_copy;
static uint8_t g_lp_save[0x10 + 4]; static uint32_t g_cam_save, g_cam_addr;
static unsigned long g_ctor_inexact;
static unsigned long g_pairs, g_monos, g_uionly, g_ctor_edits, g_ctor_misses, g_aim_edits, g_xr_hits, g_xr_miss; static double g_next_log;
static double g_lat_sum; static unsigned long g_lat_n;
static int g_mono_draw;                  /* plain CalcSceneView from Draw (mask 0): only the aim view correction applies */
static VsLayout g_L; static int g_lay_ui;   /* layout of the current pair */
static uint32_t g_pair_view0; static int g_pair_pending;   /* view returned to Draw by this frame's pair (for vr_ui.c) */
static double g_pair_t;                  /* when it was made: the view is freed at the end of that Draw */
static float g_vhead[16], g_pgame[16]; static int g_have_vhead;   /* head view (no eye offset) + the game's projection */
static double g_ctor_t = -10;            /* last FSceneView ctor seen from Draw (the aim view fix is in place) */

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static const char *cfg_path(void){ const char *p = getenv("BSI_VR_STEREO_FILE"); return p && *p ? p : "/sdcard/BSI/vr_stereo.txt"; }

static void read_cfg(void){
  cfg_init();
  const char *e = getenv("BSI_VR_STEREO");
  FILE *f = fopen(cfg_path(), "r");
  int on = e && *e ? atoi(e) != 0 : f != NULL;
  if(f){ char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); vs_parse_opts(&g_cfg, buf); }
  vs_parse_opts(&g_cfg, getenv("BSI_VR_STEREO_OPTS"));
  if(g_fake < 0){ const char *fk = getenv("BSI_VR_STEREO_FAKE"); g_fake = fk && atoi(fk); }
  if(on != g_active && g_layout == 1)
    fprintf(stderr, "[vr_stereo] stereo %s (ipd x%.2f, %.0f UU/m, pos %d max %.2f m, near %.1f, vs2 %d)\n", on ? "ON" : "off: mono view",
            g_cfg.ipd_scale, g_cfg.uu_per_m, g_cfg.pos, g_cfg.pos_max, g_cfg.near_override, g_cfg.vs2);
  g_active = on;
}

/* XR thread, once before creating the screen swapchain: 1 = make it double-wide and use projection layers. */
int vr_stereo_layout(void){
  if(g_layout < 0){ read_cfg(); g_layout = g_active; g_next_cfg = now_s() + 2.0;
    g_ui0 = g_layout == 1 && g_cfg.ui; g_ui_w0 = g_cfg.ui_w; g_ui_h0 = g_cfg.ui_h;
    fprintf(stderr, "[vr_stereo] layout: %s%s\n", g_layout ? "stereo (double-wide back buffer, projection layer)" : "mono",
            g_ui0 ? ", UI column (ui=1: [ left | right | UI ], buffer = 3 x eye width; size from vr_stereo.txt res=, not resolution.txt)" : ""); }
  return g_layout;
}
const VsCfg *vr_stereo_cfg(void){ cfg_init(); return &g_cfg; }
static void layout_for(int W, int H, int ui, VsLayout *L){
  VsCfg c = *vr_stereo_cfg(); c.ui = ui; c.ui_w = g_ui_w0; c.ui_h = g_ui_h0; vs_layout(W, H, &c, L); }
/* XR side: the layout the game is rendering (the UI column only once the game side has used it: an old part007 hook
   without the viewport size keeps rendering two halves) */
void vr_stereo_get_layout(int W, int H, VsLayout *L){ layout_for(W, H, g_ui0 == 1 && g_ui_game, L); }
/* XR side, at startup (route B swapchain sizes): the layout asked for */
void vr_stereo_get_layout_intent(int W, int H, VsLayout *L){ layout_for(W, H, g_ui0 == 1, L); }
int vr_stereo_ui_layout(int vw, int vh, VsLayout *L){
  if(g_layout != 1 || g_ui0 != 1 || vw <= 0 || vh <= 0) return 0;
  layout_for(vw, vh, 1, L);
  if(L->ui && !g_ui_game) g_ui_game = 1;
  return L->ui; }
int vr_stereo_ui_on(void){ return g_layout == 1 && g_ui0 == 1; }
/* native renderer (BSI_RHI=b1n): xr_glue passes what /sdcard/BSI/env.txt says (the XR thread decides the route before the
   game thread has applied env.txt); otherwise the environment decides */
static int g_native_hint = -1;
void vr_stereo_set_native(int on){ g_native_hint = on; }
int vr_stereo_native(void){
  if(g_native_hint < 0){ const char *e = getenv("BSI_RHI"); g_native_hint = e && strstr(e, "b1n") != NULL; }
  return g_native_hint; }
int vr_stereo_route_copy(void){ return g_layout == 1 && (g_cfg.copy > 0 || (g_cfg.copy < 0 && vr_stereo_native())); }
void vr_stereo_set_route(int copy){ g_route_copy = copy;
  fprintf(stderr, "[vr_stereo] route: %s%s\n", copy ? "copy (AImageReader -> per-eye OpenXR swapchains)" : "surface (projection layer on the game surface)",
          g_cfg.aer ? (copy ? ", alternate-eye rendering" : ", aer=1 ignored (needs route=copy)") : ""); }
/* XR thread: the runtime cannot show stereo (no image-layout flip, projection layer rejected): render mono from now on.
   With the UI column the layout must stay (the swapchain is fixed): one mono view over the eye area, shown flat. */
void vr_stereo_disable(const char *why){
  if(g_layout == 1 && g_ui0 == 1){ if(!g_xr_off) fprintf(stderr, "[vr_stereo] stereo disabled: %s (one flat view over the eye area + the UI layer)\n", why);
    g_xr_off = 1; return; }
  if(g_layout == 1) fprintf(stderr, "[vr_stereo] stereo disabled: %s (the wide back buffer is shown as a flat quad)\n", why);
  g_layout = 0; }

/* XR thread, every frame: head (VIEW in LOCAL) and both eye views (xrLocateViews in LOCAL) for one display time. */
void vr_stereo_xr_set_views(const VsViews *v){
  g_vseq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  g_views = *v;
  __atomic_thread_fence(__ATOMIC_RELEASE); g_vseq++;
  g_have_views = 1;
}

/* vr71, cloud harness only: BSI_VR_LATE_TEST=rot:<deg> (positive control: the latch turns every eye by <deg> of yaw
   on top of the new sample) | oracle:<deg> (fake views: both eyes at the head with the same symmetric fov, eye 0 yawed by
   <deg>, eye 1 not; the latch turns eye 1 by the same <deg>, so both eye columns must show the same picture: every view
   field the latch fails to update shows up as a left/right difference). Returns 1 rot, 2 oracle, 0 off. */
#if !defined(BSI_ANDROID)
static void yaw_quat(double deg, float q[4]){ double h = deg * M_PI / 360.0; q[0] = 0; q[1] = (float)sin(h); q[2] = 0; q[3] = (float)cos(h); }
static int late_test(double *deg){
  static int mode = -1; static double d;
  if(mode < 0){ const char *e = getenv("BSI_VR_LATE_TEST"); mode = 0;
    if(e && !strncmp(e, "rot:", 4)){ mode = 1; d = atof(e + 4); } else if(e && !strncmp(e, "oracle:", 7)){ mode = 2; d = atof(e + 7); }
    if(mode) fprintf(stderr, "[vr_stereo] BSI_VR_LATE_TEST: %s %.2f deg (cloud test)\n", mode == 1 ? "rot" : "oracle", d); }
  *deg = d; return mode; }
#endif
/* synthetic views for hosts without OpenXR: head at the origin looking down -Z, 63 mm IPD, Quest-3-like asymmetric fov */
static void fake_views(VsViews *v){
  memset(v, 0, sizeof *v);
  v->head.q[3] = 1;
  /* ao74 (cloud): BSI_VR_FAKE_FOV="out,in,up,down" degrees of the LEFT eye (right eye mirrored), e.g. the Quest 3's
     "54,40,44,55" (b73_vr.log); BSI_VR_FAKE_IPD=metres (Quest log: 0.0648); BSI_VR_FAKE_SAME=1: both eyes get the left
     eye's pose and fov (an in-run oracle: per-view-correct passes must then give identical eye columns) */
  static float fo = 50.f, fi = 42.f, fu = 47.f, fd = 52.f, ipd = 0.063f; static int same = -1;
#if !defined(BSI_ANDROID)
  if(same < 0){ const char *e = getenv("BSI_VR_FAKE_FOV"); float a, b, c, d;
    if(e && sscanf(e, "%f,%f,%f,%f", &a, &b, &c, &d) == 4){ fo = a; fi = b; fu = c; fd = d; }
    e = getenv("BSI_VR_FAKE_IPD"); if(e && atof(e) > 0) ipd = (float)atof(e);
    e = getenv("BSI_VR_FAKE_SAME"); same = e && atoi(e);
    fprintf(stderr, "[vr_stereo] fake views: left eye fov out/in/up/down %.1f/%.1f/%.1f/%.1f deg, IPD %.1f mm%s\n", fo, fi, fu, fd, ipd * 1000.f,
            same ? ", BOTH eyes = the left eye's pose and fov (BSI_VR_FAKE_SAME)" : ""); }
#else
  same = 0;
#endif
  for(int e = 0; e < 2; e++){
    int k = same ? 0 : e;
    v->eye[e].pose.q[3] = 1; v->eye[e].pose.p[0] = k ? 0.5f * ipd : -0.5f * ipd;
    float out = fo * (float)M_PI / 180.f, in = fi * (float)M_PI / 180.f;
    v->eye[e].fov[0] = k ? -in : -out; v->eye[e].fov[1] = k ? out : in;          /* left, right */
    v->eye[e].fov[2] = fu * (float)M_PI / 180.f; v->eye[e].fov[3] = -fd * (float)M_PI / 180.f;   /* up, down */
  }
  v->valid = 1;
  v->xr_time = (long long)((now_s() + 0.022) * 1e9);   /* vr70: as if predicted 22 ms ahead (fake XR thread uses the same clock) */
#if !defined(BSI_ANDROID)
  { double deg; if(late_test(&deg) == 2){   /* vr71 cloud oracle: both eyes at the head, symmetric fov, eye 0 yawed by deg */
      for(int e = 0; e < 2; e++){ v->eye[e].pose.p[0] = 0; float a = 45.f * (float)M_PI / 180.f;
        v->eye[e].fov[0] = -a; v->eye[e].fov[1] = a; v->eye[e].fov[2] = a; v->eye[e].fov[3] = -a; }
      yaw_quat(deg, v->eye[0].pose.q); } }
#endif
}

static int get_views(VsViews *out){
  if(!g_have_views){ if(g_fake){ fake_views(out); return 1; } return 0; }
  uint32_t s0, s1; int tries = 0;
  do { s0 = g_vseq; __atomic_thread_fence(__ATOMIC_ACQUIRE); *out = g_views; __atomic_thread_fence(__ATOMIC_ACQUIRE); s1 = g_vseq; }
  while((s0 != s1 || (s0 & 1)) && ++tries < 16);
  return out->valid && s0 == s1 && !(s0 & 1);
}

static void rd(uint32_t a, void *p, size_t n){ memcpy(p, M + a, n); }
static void wr(uint32_t a, const void *p, size_t n){ memcpy(M + a, p, n); }
static int guest_ptr_ok(uint32_t a){ return a >= 0x10000u && a < 0xC0000000u; }

/* one Present-ring entry for the frame the game thread is building now */
static uint32_t ring_put(int kind, int mask, const VsViews *v){
  uint32_t a = vs_assign_present(g_last_assigned, (uint32_t)g_br_presents); g_last_assigned = a;
  uint32_t n = g_ring_n; VsSlot *sl = &g_ring[n % VS_RING];
  sl->seq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  sl->present = a; sl->mask = mask; sl->kind = kind;
  if(v){ sl->v = *v; sl->t_sample = v->t_wall; } else { memset(&sl->v, 0, sizeof sl->v); sl->t_sample = now_s(); }
  sl->t_gt = sl->t_sample;
  if(kind == VS_KIND_STEREO){ memcpy(sl->ve, g_cur_ve, sizeof sl->ve); sl->have_ve = g_cur_have_ve; } else sl->have_ve = 0;   /* vr71 */
  __atomic_thread_fence(__ATOMIC_RELEASE); sl->seq++;
  __atomic_store_n(&g_ring_n, n + 1, __ATOMIC_RELEASE);
  return a;
}

/* vr70, cloud harness only: BSI_VR_FAKE_XR=1 starts a stand-in for the XR thread (no OpenXR on Linux): every 11 ms it sets a
   display time 22 ms ahead and runs route A's frame pick (vr_stereo_xr_frame), so the completion pick and the latency lines run
   on real rhi_b1 frames. */
static volatile int g_nat_w, g_nat_h;
#if !defined(BSI_ANDROID)
#include <pthread.h>
static void *fake_xr_thread(void *a){
  (void)a;
  for(;;){ struct timespec ts = { 0, 11111111 }; nanosleep(&ts, NULL);
    double t = now_s(); vr_stereo_xr_display_time(t + 0.022, (long long)((t + 0.022) * 1e9));
    if(g_nat_w > 0 && g_layout == 1 && !g_route_copy){ VsXrFrame f; vr_stereo_xr_frame(g_nat_w, g_nat_h, &f); } }
  return NULL; }
static void fake_xr_start(void){
  const char *e = getenv("BSI_VR_FAKE_XR"); if(!e || !atoi(e)) return;
  pthread_t t; if(pthread_create(&t, NULL, fake_xr_thread, NULL) == 0) pthread_detach(t);
  fprintf(stderr, "[vr_stereo] BSI_VR_FAKE_XR=1: a stand-in XR thread runs route A's frame pick every 11 ms (cloud harness)\n"); }
#else
static void fake_xr_start(void){}
#endif

/* ---------------- game thread: CalcSceneView pair (called from the hooked part007) ---------------- */
/* returns the eyes to render this frame (bit 0 left, bit 1 right; 3 = both), 4 = one mono view over the eye area
   (UI layout without stereo), 0 = pass through. vw/vh = viewport size (0 = unknown: old hook, no UI column). */
int vr_stereo_csv_begin2(uint32_t lp, uint32_t ret, int vw, int vh){
  int from_draw = ret == CSV_RET_FROM_DRAW;
  g_mono_draw = 0;
  if(g_layout < 0 && from_draw){   /* no XR thread (Linux): decide once, only for the fake-view test mode */
    const char *fk = getenv("BSI_VR_STEREO_FAKE"); if(fk && atoi(fk)) vr_stereo_layout(); else { g_layout = 0; g_ui0 = 0; }
    /* vr69 (cloud harness): BSI_VR_FAKE_COPY=1 pretends route B runs, so the native renderer's FIFO present and image map run too */
    { const char *fc = getenv("BSI_VR_FAKE_COPY"); if(g_layout == 1 && fc && atoi(fc)) vr_stereo_set_route(1); }
    if(g_layout == 1) fake_xr_start(); }
  if(g_layout != 1) return 0;
  double t = now_s();
  if(t >= g_next_cfg){ g_next_cfg = t + 2.0; read_cfg(); }
  if(!from_draw || !guest_ptr_ok(lp)) return 0;
  VsLayout L; int ui = vr_stereo_ui_layout(vw, vh, &L);
  int stereo = g_active && !g_xr_off && get_views(&g_cur);
  if(!stereo && !ui) return 0;
  uint32_t pc; rd(lp + LP_ACTOR, &pc, 4); if(!guest_ptr_ok(pc)) return 0;
  g_L = L; g_lay_ui = ui; g_have_vhead = 0;
  rd(lp + LP_ORIGIN, g_lp_save, 0x10); rd(lp + LP_VIEWSTATE, g_lp_save + 0x10, 4);
  g_cam_addr = 0;
  if(!stereo){ g_cur_mask = 4; return 4; }     /* keep the game's own camera (letterbox etc.): it is a flat view */
  g_cur.t_wall = t; g_cur_have_ve = 0;
  if(g_cfg.fov_pad > 0){   /* vr69: overscan; the ring (and so the projection layer) carries the padded fov too */
    float p = g_cfg.fov_pad * (float)M_PI / 180.f, lim = 80.f * (float)M_PI / 180.f;
    for(int e = 0; e < 2; e++){ float *fv = g_cur.eye[e].fov;
      fv[0] = fmaxf(fv[0] - p, -lim); fv[1] = fminf(fv[1] + p, lim); fv[2] = fminf(fv[2] + p, lim); fv[3] = fmaxf(fv[3] - p, -lim); } }
  { uint32_t cam; rd(pc + PC_CAMERA, &cam, 4);
    if(guest_ptr_ok(cam)){ g_cam_addr = cam; rd(cam + CAM_FLAGS, &g_cam_save, 4);
      uint32_t f = g_cam_save & ~2u; wr(cam + CAM_FLAGS, &f, 4); } }   /* no letterbox: each eye uses its full half */
  if(&vr_head_enabled && !vr_head_enabled() && (g_pairs & 0x3FF) == 0)
    fprintf(stderr, "[vr_stereo] warning: head-look (vr_head.txt) is off; the image will not follow the head correctly\n");
  g_cur_mask = (g_cfg.aer && g_route_copy) ? 1 << (g_frame_no++ & 1) : 3;
  return g_cur_mask;
}
int vr_stereo_csv_begin(uint32_t lp, uint32_t ret){ return vr_stereo_csv_begin2(lp, ret, 0, 0); }
/* mask 0 path of the new hook: a plain CalcSceneView. From Draw, the ctor still gets the aim-view correction. */
static unsigned long g_monoheads;
void vr_stereo_csv_mono(uint32_t ret){
  g_mono_draw = ret == CSV_RET_FROM_DRAW && &vr_aim_view_q != NULL;
  /* vr69: flat frames with head-look on carry the orientation the camera got (FillCameraCache ran just before, in the same
     Draw), so xr_glue can world-lock the flat screen to it and measure the image-vs-head error during turns */
  if(ret == CSV_RET_FROM_DRAW && g_layout != 1 && &vr_head_rendered){
    VsViews v; memset(&v, 0, sizeof v);
    if(vr_head_rendered(v.head.q)){ v.t_wall = now_s(); v.valid = 1; ring_put(VS_KIND_MONOHEAD, 0, &v); g_monoheads++; } }
}
int vr_stereo_need_viewstate2(void){ return g_cfg.vs2 && !g_vs2 && g_cur_mask != 4; }
void vr_stereo_set_viewstate2(uint32_t vs){
  if(!vs){ g_cfg.vs2 = 0; fprintf(stderr, "[vr_stereo] AllocateViewState failed: both eyes share the player's FSceneViewState\n"); return; }
  g_vs2 = vs; fprintf(stderr, "[vr_stereo] right-eye FSceneViewState allocated: 0x%08x\n", vs); }

void vr_stereo_csv_eye(uint32_t lp, int eye){
  float o[2], s[2]; memcpy(o, g_lp_save, 8); memcpy(s, g_lp_save + 8, 8);
  /* one eye's share of the back-buffer width; CalcSceneView truncates Origin/Size x viewport size (cvttss2si at 0x66345b..),
     so the UI layout's fractions carry +0.25 px to land exactly on the eye column */
  float fx = g_lay_ui ? ((float)g_L.eye_w + 0.25f) / (float)g_L.W : 0.5f;
  float no[2], ns[2];
  if(eye == 2){ no[0] = o[0]; no[1] = o[1]; ns[0] = s[0] * 2.f * fx; ns[1] = s[1]; }
  else { no[0] = o[0] + (eye ? s[0] * fx : 0.f); no[1] = o[1]; ns[0] = s[0] * fx; ns[1] = s[1]; }
  wr(lp + LP_ORIGIN, no, 8); wr(lp + LP_SIZE, ns, 8);
  uint32_t vs; memcpy(&vs, g_lp_save + 0x10, 4);
  if(eye == 1 && g_cfg.vs2 && g_vs2) vs = g_vs2;
  wr(lp + LP_VIEWSTATE, &vs, 4);
  g_cur_eye = eye;
}

void vr_stereo_csv_end(uint32_t lp, uint32_t view0, uint32_t view1){
  wr(lp + LP_ORIGIN, g_lp_save, 0x10); wr(lp + LP_VIEWSTATE, g_lp_save + 0x10, 4);
  if(g_cam_addr) wr(g_cam_addr + CAM_FLAGS, &g_cam_save, 4);
  g_cur_eye = -1;
  if(g_cur_mask == 4){   /* mono view over the eye area (UI layout) */
    if(!view0) return;
    ring_put(VS_KIND_MONOWIDE, 0, NULL); g_monos++;
    g_pair_view0 = view0; g_pair_pending = 1; g_pair_t = now_s();
    return;
  }
  if(((g_cur_mask & 1) && !view0) || ((g_cur_mask & 2) && !view1)) return;
  uint32_t a = ring_put(VS_KIND_STEREO, g_cur_mask, &g_cur);
  g_pair_view0 = (g_cur_mask & 1) ? view0 : view1; g_pair_pending = 1; g_pair_t = now_s();
  g_pairs++;
  { static int inst; if((g_cfg.late || g_cfg.ao_eye) && !inst){ inst = &vr_late_install && vr_late_install() ? 1 : -1;   /* vr71: render-thread hook, once (late latch, ao_eye's view order) */
      if(inst < 0) fprintf(stderr, "[vr_stereo] late latch: render-thread hook not available (%s): the game thread's head pose is used\n",
                           &vr_late_install ? "install failed" : "vr_ui_rhi.o has no hook"); } }
  if(g_cfg.log && (g_pairs <= 3 || (g_pairs & 0x3FF) == 0))
    fprintf(stderr, "[vr_stereo] pair %lu: views 0x%08x 0x%08x, present #%u, ctor edits %lu misses %lu (inexact w %lu), aim fixes %lu%s\n", g_pairs, view0, view1, a,
            g_ctor_edits, g_ctor_misses, g_ctor_inexact, g_aim_edits, g_lay_ui ? ", UI column" : "");
}

/* vr_ui.c, after BeginRenderingViewFamily (the renderer has copied the views): the view Draw got from this frame's pair,
   with the head view (no eye offset, aim-corrected) and the game's projection. Once per pair. */
uint32_t vr_stereo_take_pair(float Vhead[16], float Pgame[16]){
  if(!g_pair_pending) return 0;
  g_pair_pending = 0;
  if(!g_have_vhead || now_s() - g_pair_t > 0.25) return 0;   /* (Draw always reaches the cover tiles after the pair; defensive) */
  memcpy(Vhead, g_vhead, sizeof g_vhead); memcpy(Pgame, g_pgame, sizeof g_pgame);
  return g_pair_view0;
}
/* vr_ui.c: this Draw had no world view (menus, loading screens drawn by the game): show the UI rect as the screen */
void vr_stereo_mark_uionly(void){
  if(g_layout != 1 || g_ui0 != 1) return;
  ring_put(VS_KIND_UIONLY, 0, NULL); g_uionly++;
}

/* FSceneView ctor call inside CalcSceneView (0x664119): esp = guest esp at the ctor's entry ([esp] = return address) */
void vr_stereo_view_ctor_args(uint32_t esp){
  int eye = g_cur_eye;
  if(eye < 0 && !g_mono_draw) return;
  g_mono_draw = 0; g_ctor_t = now_s();
  uint32_t pv, pp; rd(esp + 4 + 4 * CTOR_ARG_VIEW, &pv, 4); rd(esp + 4 + 4 * CTOR_ARG_PROJ, &pp, 4);
  if(!guest_ptr_ok(pv) || !guest_ptr_ok(pp)){
    if(g_cfg.log && g_ctor_misses < 4) fprintf(stderr, "[vr_stereo] ctor miss (eye %d): matrix pointers 0x%08x 0x%08x\n", eye, pv, pp);
    g_ctor_misses++; return; }
  float V[16], P[16], Vh[16], Ve[16], Pe[16]; rd(pv, V, 64); rd(pp, P, 64);
  /* perspective projection (w = z) and an affine view. The view check has a tolerance: a camera that composes its view
     with another matrix (CalcSceneView lets [[GWorld]+0xE0]+0xFC rewrite it, vf +0x2C, 0x663bc7) can leave V[15] at
     1 +- 1 ulp, and an exact compare then dropped every front-end frame to mono (C1, notes/VR_STEREO.md §16). */
  int v_aff = fabsf(V[3]) < 1e-4f && fabsf(V[7]) < 1e-4f && fabsf(V[11]) < 1e-4f && fabsf(V[15] - 1.f) < 1e-4f;
  if(v_aff && V[15] != 1.f){
    if(g_cfg.log && g_ctor_inexact < 4)
      fprintf(stderr, "[vr_stereo] ctor (eye %d): view matrix w column %.9g %.9g %.9g %.9g accepted as affine\n", eye, V[3], V[7], V[11], V[15]);
    g_ctor_inexact++; V[3] = V[7] = V[11] = 0.f; V[15] = 1.f; }
  if(!(P[11] > 0.99f && P[11] < 1.01f) || !v_aff){   /* not a perspective view: leave it */
    if(g_cfg.log && g_ctor_misses < 4)
      fprintf(stderr, "[vr_stereo] ctor miss (eye %d): V 0x%08x [15]=%g, P 0x%08x [0]=%g [5]=%g [10]=%g [11]=%g [14]=%g [15]=%g\n",
              eye, pv, V[15], pp, P[0], P[5], P[10], P[11], P[14], P[15]);
    g_ctor_misses++; return; }
  /* controller aim (vr_aim.c): the game's camera = the gun's aim; the picture must stay on the head */
  double Qa[9]; int aimq = &vr_aim_view_q && vr_aim_view_q(Qa);
  if(aimq){ vs_view_rotate(V, Qa, Vh); g_aim_edits++; } else memcpy(Vh, V, sizeof V);
#if !defined(BSI_ANDROID)
  { static int ref = -2; if(ref == -2){ const char *r = getenv("BSI_VR_EYEREF"); ref = r && *r ? atoi(r) : -1;
      if(ref >= 0) fprintf(stderr, "[vr_stereo] BSI_VR_EYEREF=%d: flat views get fake eye %d's view and projection (cloud mono reference)\n", ref, ref); }
    if(eye < 0 && ref >= 0 && ref < 2 && g_layout != 1){   /* ao74: the eye rendered alone, same camera / projection as in stereo */
      VsViews fv; fake_views(&fv); cfg_init();
      if(g_cfg.fov_pad > 0){ float p = g_cfg.fov_pad * (float)M_PI / 180.f; float *f = fv.eye[ref].fov; f[0] -= p; f[1] += p; f[2] += p; f[3] -= p; }
      double d_v[3], Q[9]; vs_eye_offset(&fv.head, &fv.eye[ref].pose, &g_cfg, d_v, Q);
      vs_eye_view(Vh, d_v, Q, Ve); vs_eye_proj(P, fv.eye[ref].fov, g_cfg.near_override, Pe);
      wr(pv, Ve, 64); wr(pp, Pe, 64);
      static int nl; if(nl++ < 2) fprintf(stderr, "[vr_stereo] eye reference %d: offset (%.2f, %.2f, %.2f) UU, proj x %.4f %+.4f y %.4f %+.4f\n",
                                          ref, d_v[0], d_v[1], d_v[2], Pe[0], Pe[8], Pe[5], Pe[9]);
      return; } }
#endif
  if(eye < 0 || eye == 2){   /* plain mono view (no stereo) or the mono view over the eye area: rotation fix only */
    if(eye == 2 && !g_have_vhead){ memcpy(g_vhead, Vh, sizeof Vh); memcpy(g_pgame, P, sizeof P); g_have_vhead = 1; }
    if(aimq) wr(pv, Vh, 64);
    return;
  }
  if(!g_have_vhead){ memcpy(g_vhead, Vh, sizeof Vh); memcpy(g_pgame, P, sizeof P); g_have_vhead = 1; }
  const VsEye *e = &g_cur.eye[eye];
  double d_v[3], Q[9]; vs_eye_offset(&g_cur.head, &e->pose, &g_cfg, d_v, Q);
  vs_eye_view(Vh, d_v, Q, Ve); vs_eye_proj(P, e->fov, g_cfg.near_override, Pe);
  wr(pv, Ve, 64); wr(pp, Pe, 64);
  memcpy(g_cur_ve[eye], Ve, sizeof Ve); g_cur_have_ve |= 1 << eye;   /* vr71: the late latch finds the eye's FViewInfo by it */
  g_ctor_edits++;
  if(g_cfg.log && g_ctor_edits <= 2)
    fprintf(stderr, "[vr_stereo] eye %d: offset (%.2f, %.2f, %.2f) UU, proj x %.4f %+.4f y %.4f %+.4f (game x %.4f y %.4f, z %.4f %.3f)\n",
            eye, d_v[0], d_v[1], d_v[2], Pe[0], Pe[8], Pe[5], Pe[9], P[0], P[5], P[10], P[14]);
}

/* ---------------- XR thread: which frame is on screen, and its eye poses ---------------- */
static int ring_find(uint32_t present, VsSlot *out){
  uint32_t n = __atomic_load_n(&g_ring_n, __ATOMIC_ACQUIRE);
  for(uint32_t k = 0; k < VS_RING && k < n; k++){
    VsSlot *sl = &g_ring[(n - 1 - k) % VS_RING]; uint32_t s0 = sl->seq; __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(s0 & 1) continue;
    out->present = sl->present; out->mask = sl->mask; out->kind = sl->kind; out->v = sl->v; out->t_sample = sl->t_sample;
    __atomic_thread_fence(__ATOMIC_ACQUIRE); if(sl->seq != s0) continue;
    if(out->present == present) return 1;
  }
  return 0;
}
/* Presents the game did not tag but that drew only into the UI column (loading-screen movies): shown as UI-only frames */
#define VS_RTU 32
static volatile uint32_t g_rtu[VS_RTU];
static int rt_uionly(uint32_t present){ return present && g_rtu[present % VS_RTU] == present; }
/* route B: the eye mask and views of the frame shown by Present #present (0 = not a stereo frame) */
int vr_stereo_xr_entry(uint32_t present, VsEye out[2]){
  VsSlot sl; if(g_layout != 1 || !ring_find(present, &sl) || sl.kind != VS_KIND_STEREO) return 0;
  out[0] = sl.v.eye[0]; out[1] = sl.v.eye[1]; return sl.mask;
}
int vr_stereo_xr_entry2(uint32_t present, int *mask, VsEye out[2]){
  VsSlot sl; *mask = 0; if(g_layout != 1) return VS_KIND_NONE;
  if(!ring_find(present, &sl)) return rt_uionly(present) ? VS_KIND_UIONLY : VS_KIND_NONE;
  *mask = sl.mask; if(sl.kind == VS_KIND_STEREO){ out[0] = sl.v.eye[0]; out[1] = sl.v.eye[1]; }
  return sl.kind;
}
int vr_stereo_xr_entry3(uint32_t present, int *mask, VsEye out[2], long long *xr_time){
  VsSlot sl; *mask = 0; *xr_time = 0; if(g_layout != 1) return VS_KIND_NONE;
  if(!ring_find(present, &sl)) return rt_uionly(present) ? VS_KIND_UIONLY : VS_KIND_NONE;
  *mask = sl.mask; if(sl.kind == VS_KIND_STEREO){ out[0] = sl.v.eye[0]; out[1] = sl.v.eye[1]; *xr_time = sl.v.xr_time; }
  return sl.kind;
}

/* ---- vr70: which frame the compositor can show (route A), and the latency of every shown frame ----
   rhi_b1 reports every queued present (presenter frame id -> game Present index, vr_stereo_frame_queued) and the presenter's
   frame signal (b1dx_frames_completed) says which frames the GPU has finished: DXVK signals frame N from its finish thread
   after all of N's command lists completed, i.e. when the buffer becomes latchable for the compositor. Build 69 took the
   newest Present the game had *called* (g_br_presents), up to 3 GPU frames (~45 ms each in stereo) before that frame could
   be on screen, so the projection layer reported poses 1-3 frames newer than the picture: the compositor's reprojection
   then moved the picture by the head motion of those frames (swimming, black edges during turns). */
unsigned long long b1dx_frames_completed(void) __attribute__((weak));   /* rhi_b1_dxvk.cpp: presenter frames finished on the GPU */
#define VS_FQ 64
static volatile unsigned long long g_fq_fid[VS_FQ]; static volatile uint32_t g_fq_p[VS_FQ]; static volatile double g_fq_t[VS_FQ];
void vr_stereo_frame_queued(unsigned long long fid, uint32_t present){
  unsigned i = (unsigned)(fid % VS_FQ); g_fq_p[i] = present; g_fq_t[i] = now_s(); __atomic_store_n(&g_fq_fid[i], fid, __ATOMIC_RELEASE); }
static uint32_t fq_present(unsigned long long fid){
  unsigned i = (unsigned)(fid % VS_FQ); if(!fid || __atomic_load_n(&g_fq_fid[i], __ATOMIC_ACQUIRE) != fid) return 0; return g_fq_p[i]; }
static double fq_time(uint32_t present){
  for(int i = 0; i < VS_FQ; i++) if(g_fq_fid[i] && g_fq_p[i] == present) return g_fq_t[i];
  return 0; }
/* XR thread: the display time of the frame being submitted (wall clock), set before the pick / the route B copy */
static double g_t_disp; static long long g_x_disp;
void vr_stereo_xr_display_time(double t_wall, long long xr_time){ g_t_disp = t_wall; g_x_disp = xr_time; }
/* latency per shown frame: sample (game thread) -> present call (render thread) -> GPU done -> first shown -> display */
#define VS_LT 32
static uint32_t g_lt_p[VS_LT]; static double g_lt_t[VS_LT];
void vr_stereo_lat_done(uint32_t present){ if(g_lt_p[present % VS_LT] != present){ g_lt_p[present % VS_LT] = present; g_lt_t[present % VS_LT] = now_s(); } }
static struct { double sq, qd, dsh, shd, tot, late, tot_max; unsigned long nq, nd, nsh, ntot, nlate, n; int route; } g_lt;
static double g_lt_next; static void lat_log(double t);
void vr_stereo_lat_shown(uint32_t present, int route){
  VsSlot sl; if(!present || !ring_find(present, &sl)) return;
  double t = now_s(), tq = fq_time(present), td = g_lt_p[present % VS_LT] == present ? g_lt_t[present % VS_LT] : 0;
  g_lt.n++; g_lt.route = route;
  if(tq > 0 && tq >= sl.t_sample){ g_lt.sq += tq - sl.t_sample; g_lt.nq++; }
  if(tq > 0 && td >= tq){ g_lt.qd += td - tq; g_lt.nd++; }
  if(td > 0 && t >= td){ g_lt.dsh += t - td; g_lt.nsh++; }
  if(g_t_disp > t){ double tot = g_t_disp - sl.t_sample; g_lt.shd += g_t_disp - t; g_lt.tot += tot; g_lt.ntot++; if(tot > g_lt.tot_max) g_lt.tot_max = tot; }
  if(sl.kind == VS_KIND_STEREO && sl.v.xr_time && g_x_disp > sl.v.xr_time){ g_lt.late += (g_x_disp - sl.v.xr_time) * 1e-6; g_lt.nlate++; }
  lat_log(t);
}
static void lat_log(double t){
  if(!g_cfg.log || t < g_lt_next) return;
  if(!g_lt_next){ g_lt_next = t + 10.0; return; }
  g_lt_next = t + 10.0;
  if(!g_lt.n) return;
#define AVG(s, n) ((n) ? 1000.0 * (s) / (n) : -1.0)
  fprintf(stderr, "[vr_stereo] latency (%s, %lu frames shown): sample->present %.0f ms, present->GPU done %.0f, done->first shown %.0f, shown->display %.0f; "
          "sample->display avg %.0f max %.0f ms; shown %.0f ms later than the pose prediction (render pose vs display time)%s\n",
          g_lt.route == 2 ? "route B copy" : g_lt.route == 1 ? "route A, GPU completion pick" : g_lt.route == 3 ? "flat screen" : "route A, Present-call pick (no completion info)",
          g_lt.n, AVG(g_lt.sq, g_lt.nq), AVG(g_lt.qd, g_lt.nd), AVG(g_lt.dsh, g_lt.nsh), AVG(g_lt.shd, g_lt.ntot), AVG(g_lt.tot, g_lt.ntot), 1000.0 * g_lt.tot_max,
          g_lt.nlate ? g_lt.late / g_lt.nlate : -1.0, g_lt.ntot ? "" : " (display time unknown)");
#undef AVG
  memset(&g_lt, 0, sizeof g_lt);
}
/* route A: the newest Present the compositor can latch (vr70: the newest one the GPU finished; build 69: the newest called),
   first seen at least lag_ms ago, and its ring entry */
static int g_pick_mode;   /* 1 = GPU completion, 0 = Present call (bridge / no presenter) */
static int xr_pick(VsSlot *sl, uint32_t *present, double *t_now){
  enum { H = 8 }; static uint32_t hp[H]; static double ht[H]; static int hn; static uint32_t last_shown;
  uint32_t p = 0, eff_p = 0; double t = now_s();
  unsigned long long done = b1dx_frames_completed ? b1dx_frames_completed() : 0;
  uint32_t pd = done ? fq_present(done) : 0;
  if(pd){ p = pd; g_pick_mode = 1; } else { p = (uint32_t)g_br_presents; g_pick_mode = 0; }
  if(!hn || hp[hn - 1] != p){ if(hn == H){ memmove(hp, hp + 1, (H - 1) * sizeof *hp); memmove(ht, ht + 1, (H - 1) * sizeof *ht); hn--; }
    hp[hn] = p; ht[hn] = t; hn++;
    if(g_pick_mode) vr_stereo_lat_done(p); }
  if(!vs_pick_present(hp, ht, hn, t, g_cfg.lag_ms / 1000.0, &eff_p)) eff_p = hp[0];
  *present = eff_p; *t_now = t;
  if(eff_p != last_shown){ last_shown = eff_p; vr_stereo_lat_shown(eff_p, g_layout == 1 ? g_pick_mode : 3); }
  return ring_find(eff_p, sl);
}
static void xr_stats(int stereo_found, const VsSlot *sl, double t){
  if(stereo_found){ static uint32_t last_pr; if(sl->present != last_pr){ last_pr = sl->present; g_lat_sum += t - sl->t_sample; g_lat_n++; } }
  if(stereo_found) g_xr_hits++; else g_xr_miss++;
  if(g_cfg.log && t >= g_next_log){ g_next_log = t + 10.0;
    fprintf(stderr, "[vr_stereo] xr: projection %lu / other %lu frames; game: %lu stereo pairs, %lu mono, %lu UI-only; sample->first shown %.1f ms avg; pick: %s\n",
            g_xr_hits, g_xr_miss, g_pairs, g_monos, g_uionly, g_lat_n ? 1000.0 * g_lat_sum / g_lat_n : 0.0,
            g_pick_mode ? "newest frame finished on the GPU" : "newest Present called (build 69 rule)");
    g_xr_hits = g_xr_miss = 0; g_lat_sum = 0; g_lat_n = 0; }
}
/* Fills the projection-layer views for the newest presented stereo frame. Returns 1 if there is one, 0 = show the quad. */
int vr_stereo_xr_layer(VsEye out[2]){
  if(g_layout != 1) return 0;
  VsSlot sl; uint32_t pr; double t; int found = xr_pick(&sl, &pr, &t) && sl.kind == VS_KIND_STEREO && sl.mask == 3;
  if(found){ out[0] = sl.v.eye[0]; out[1] = sl.v.eye[1]; }
  xr_stats(found, &sl, t);
  return found;
}
/* route A with the UI layer: what the newest presented frame is (W x H = the game surface) */
int vr_stereo_xr_frame(int W, int H, VsXrFrame *f){
  memset(f, 0, sizeof *f);
  vr_stereo_get_layout(W, H, &f->L);
  if(g_layout != 1) return VS_KIND_NONE;
  VsSlot sl; uint32_t pr; double t; int found = xr_pick(&sl, &pr, &t);
  f->present = pr;
  if(!found && rt_uionly(pr)) f->kind = VS_KIND_UIONLY;
  if(found){ f->kind = sl.kind; f->mask = sl.mask; f->t_sample = sl.t_sample;
    if(sl.kind == VS_KIND_STEREO){ f->eye[0] = sl.v.eye[0]; f->eye[1] = sl.v.eye[1]; f->xr_time = sl.v.xr_time;
      if(sl.mask != 3) f->kind = VS_KIND_NONE; } }   /* aer frames need route B */
  xr_stats(f->kind == VS_KIND_STEREO, &sl, t);
  return f->kind;
}
long long vr_stereo_pred_ns(void){ return (long long)(g_cfg.pred_ms * 1e6); }
int vr_stereo_view_fix_alive(void){ return now_s() - g_ctor_t < 2.0; }

/* ---------------- vr69: flat screen (no stereo layout) with head-look ---------------- */
/* XR thread: orientation (LOCAL) the newest presented flat frame was rendered with; 0 = none (not a head-look frame).
   *age_ms = time since that frame's head sample (diagnostics). Same Present pick (lag_ms) as the stereo route A. */
int vr_stereo_xr_mono_head(float q[4], double *age_ms){
  if(g_layout == 1) return 0;
  VsSlot sl; uint32_t pr; double t;
  if(!xr_pick(&sl, &pr, &t) || sl.kind != VS_KIND_MONOHEAD) return 0;
  memcpy(q, sl.v.head.q, sizeof sl.v.head.q); if(age_ms) *age_ms = (t - sl.t_sample) * 1000.0;
  return 1;
}

/* ---------------- vr69: native renderer (rhi_b1, linux/rhi_b1_vr.inc) ---------------- */
/* rhi_b1, at every native Present (render thread), with the back-buffer size: 1 = the stereo layout is on (eye column
   width in *eye_w), *ui_alpha = 1 when rhi_b1 must make the UI column's alpha coverage (ui=1, ui_alpha=rhi) */
int vr_stereo_native_frame(int W, int H, int *eye_w, int *ui_alpha){
  *eye_w = 0; *ui_alpha = 0;
  if(g_layout != 1 || W <= 0 || H <= 0) return 0;
  VsLayout L; vr_stereo_get_layout(W, H, &L);
  *eye_w = L.eye_w; *ui_alpha = L.ui && g_cfg.ui_alpha == 1;
  g_nat_w = W; g_nat_h = H;   /* vr70: for the cloud's stand-in XR thread */
  return 1;
}
/* rhi_b1, once when the stereo layout first shows up: should the GPU timestamps the game reads run at half speed?
   The game scales its detail by the GPU frame time it measures with them (rhi_b1_dxvk.cpp, 13 Oct note: a wrong GPU time
   dropped it to low detail). Two eye views roughly double that time, so with gpu_ts=eye (default) the game sees a per-eye
   time and keeps the detail it picks in mono. Not with aer=1 (one eye per frame) or gpu_ts=frame. */
int vr_stereo_native_ts_half(void){ return g_layout == 1 && g_active && g_cfg.gpu_ts_eye && !(g_cfg.aer && g_route_copy); }
/* vr70: rhi_b1, while the stereo layout is on: frames in flight (vr_stereo.txt fif=, default 1; 0 = dxvk.conf) and the AO crop margin */
int vr_stereo_frames_in_flight(void){ return g_layout == 1 ? g_cfg.fif : 0; }
int vr_stereo_ao_crop(void){ return g_layout == 1 && g_ui0 == 1 ? g_cfg.ao_crop : 0; }
/* vr71: ao_eye (rhi_b1_vr.inc): wanted? and is the frame being rendered a stereo pair (eye width, else 0) */
int vr_stereo_ao_eye(void){ return g_layout == 1 && g_ui0 == 1 ? g_cfg.ao_eye : 0; }   /* ao74: the mode (0, 1, 2) */
int vr_stereo_pp_eye(int which){ if(g_layout != 1 || g_ui0 != 1) return 0; return which == 0 ? g_cfg.ls_eye : which == 1 ? g_cfg.shadow_eye : which == 2 ? g_cfg.fxaa_eye : which == 3 ? g_cfg.bloom_eye : 0; }   /* pp76 */
int vr_stereo_eye_frame(int W, int H, uint32_t present){
  if(g_layout != 1 || g_ui0 != 1 || !g_ui_game || W <= 0 || H <= 0) return 0;
  VsSlot sl; if(!ring_find(present, &sl) || sl.kind != VS_KIND_STEREO || sl.mask != 3) return 0;
  VsLayout L; vr_stereo_get_layout(W, H, &L); return L.ui ? L.eye_w : 0; }
/* rhi_b1, at an AO dispatch of the frame that will be Present #present: the right edge of the eye area (2 x eye width) when
   that frame is tagged as a world frame inside the eye columns (stereo pair or mono view over the eye area), else 0. The
   game side puts every view of such a frame into the eye columns (vr_stereo_csv_eye), so nothing of the scene lies right
   of that edge; frames without a tag (movies, loading) or before the UI column is in use are never cut. */
int vr_stereo_crop_edge(int W, int H, uint32_t present){
  if(g_layout != 1 || g_ui0 != 1 || !g_ui_game || W <= 0 || H <= 0) return 0;
  VsSlot sl; if(!ring_find(present, &sl) || (sl.kind != VS_KIND_STEREO && sl.kind != VS_KIND_MONOWIDE)) return 0;
  VsLayout L; vr_stereo_get_layout(W, H, &L);
  return L.ui ? 2 * L.eye_w : 0; }
/* rhi_b1 decides the route-B presentation mode: FIFO, so the AImageReader receives every frame in order */
int vr_stereo_present_fifo(void){ return g_layout == 1 && g_route_copy; }
/* presented image k (1-based, in queue order) -> the Present index it came from (rhi_b1 writes, XR thread reads) */
#define VS_IMG 64
static volatile uint32_t g_img_n, g_img_p[VS_IMG];
void vr_stereo_image_presented(uint32_t present){
  uint32_t n = g_img_n; g_img_p[n % VS_IMG] = present; __atomic_store_n(&g_img_n, n + 1, __ATOMIC_RELEASE); }
uint32_t vr_stereo_image_present(uint32_t k){
  uint32_t n = __atomic_load_n(&g_img_n, __ATOMIC_ACQUIRE);
  if(!k || k > n || n - k >= VS_IMG - 8) return 0;
  return g_img_p[(k - 1) % VS_IMG]; }
uint32_t vr_stereo_images_presented(void){ return __atomic_load_n(&g_img_n, __ATOMIC_ACQUIRE); }
/* rhi_b1, at the native Present of frame #present: did that frame draw a scene pass into the right eye's viewport?
   Compared with the game thread's tag for the same Present (exactness of the pose / image association). */
static unsigned long g_rtc_ok, g_rtc_bad, g_rtc_untag, g_rtc_bad_logged, g_rtc_uionly; static double g_rtc_next;
void vr_stereo_rt_check(uint32_t present, int saw_right){
  if(g_layout != 1) return;
  VsSlot sl; int found = ring_find(present, &sl);
  int tag_right = found && sl.kind == VS_KIND_STEREO && (sl.mask & 2);
  if(!found){ g_rtc_untag++; if(saw_right) g_rtc_bad++; }
  else if(tag_right == saw_right) g_rtc_ok++;
  else { g_rtc_bad++;
    if(g_rtc_bad_logged++ < 6) fprintf(stderr, "[vr_stereo] present tag mismatch: Present #%u tagged kind %d mask %d, the renderer %s a right-eye view\n",
                                       present, sl.kind, sl.mask, saw_right ? "drew" : "did not draw"); }
  double t = now_s();
  if(g_cfg.log && t >= g_rtc_next){ g_rtc_next = t + 10.0;
    fprintf(stderr, "[vr_stereo] present tags (render thread check): %lu match, %lu mismatch, %lu Presents without a game tag (movies/loading; %lu of them UI-only)\n",
            g_rtc_ok, g_rtc_bad, g_rtc_untag, g_rtc_uionly); }
}
/* rhi_b1 at every native Present: the tag check, plus which columns the frame drew into */
void vr_stereo_rt_frame(uint32_t present, int saw_right, int drew_eye, int drew_ui){
  if(g_layout != 1) return;
  VsSlot sl;
  if(drew_ui && !drew_eye && g_ui0 == 1 && !ring_find(present, &sl)){
    g_rtu[present % VS_RTU] = present; g_rtc_uionly++;
    if(g_rtc_uionly == 1) fprintf(stderr, "[vr_stereo] Present #%u: no game tag and drawn only into the UI column: shown as a UI-only frame\n", present); }
  vr_stereo_rt_check(present, saw_right);
}
/* ---------------- vr71: late head-pose latch (render thread) ----------------
   Called by the FDrawSceneCommand::Execute hook (vr_ui_rhi.c) right before FSceneRenderer::Render of a frame, i.e. before
   InitViews: everything the render thread derives from the views (visibility, shadows, every shader's view constants,
   the view state's "previous view" for the next frame) then sees the latched view. sr = the FSceneRenderer (+0x50 views
   TArray<FViewInfo>, +0x54 count, 0x11A0 bytes each). The frame's ring entry is found by the eye view matrices the game
   thread's ctor calls got (bit-exact), so neither the Present count nor the view order is assumed. Per eye: new orientation
   = the newest XR sample (or xrLocateViews at the display time + late_pred_ms); if it equals the rendered one bit for bit,
   nothing is written (latch off or an unchanged pose = the build 70 frame); otherwise vs_late_patch_view turns the view
   about the eye and the ring entry gets the new orientation, so the compositor is told exactly what was rendered. */
static struct { unsigned long frames, eyes, same, big, nomatch, nopose; double gt_sum, gt_max, deg_sum, deg_max; unsigned long ngt; double next; } g_ll;
static int ll_find(uint32_t base, uint32_t nv, VsSlot **out, int vi_eye[4]){
  uint32_t n = __atomic_load_n(&g_ring_n, __ATOMIC_ACQUIRE);
  for(uint32_t k = 0; k < VS_RING && k < n; k++){
    VsSlot *sl = &g_ring[(n - 1 - k) % VS_RING]; uint32_t s0 = sl->seq; __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if((s0 & 1) || sl->kind != VS_KIND_STEREO || !sl->have_ve) continue;
    int hit = 0;
    for(uint32_t i = 0; i < nv && i < 4; i++){ vi_eye[i] = -1;
      for(int e = 0; e < 2; e++) if((sl->have_ve >> e & 1) && !memcmp(M + base + i * 0x11A0u + VI_VIEW, sl->ve[e], 64)){ vi_eye[i] = e; hit++; break; } }
    __atomic_thread_fence(__ATOMIC_ACQUIRE); if(sl->seq != s0) continue;
    if(hit){ *out = sl; return hit; }
  }
  return 0; }
static void ll_log(double t){
  if(!g_cfg.log || t < g_ll.next) return;
  if(!g_ll.next){ g_ll.next = t + 10.0; return; }
  g_ll.next = t + 10.0;
  if(!g_ll.frames) return;
  fprintf(stderr, "[vr_stereo] late latch: %lu frames; eyes turned %lu, unchanged %lu, over late_max %lu; no matching view %lu, no pose %lu; "
          "head sample moved later by avg %.0f ms (max %.0f); orientation change at the latch avg %.2f max %.2f deg%s\n",
          g_ll.frames, g_ll.eyes, g_ll.same, g_ll.big, g_ll.nomatch, g_ll.nopose, g_ll.ngt ? 1000.0 * g_ll.gt_sum / g_ll.ngt : 0.0, 1000.0 * g_ll.gt_max,
          g_ll.eyes ? g_ll.deg_sum / g_ll.eyes : 0.0, g_ll.deg_max, g_cfg.late_pred_ms > 0 ? (xr_locate_eyes ? ", + late_pred_ms prediction" : ", late_pred_ms ignored (no xr_glue hook)") : "");
  memset(&g_ll, 0, sizeof g_ll); g_ll.next = t + 10.0; }
int vr_stereo_late_latch(uint32_t sr){
  if(g_layout != 1 || !g_cfg.late || !guest_ptr_ok(sr)) return 0;
  uint32_t base, nv; rd(sr + 0x50, &base, 4); rd(sr + 0x54, &nv, 4);
  if(nv < 1 || nv > 4 || !guest_ptr_ok(base)) return 0;
  double t = now_s(); g_ll.frames++;
  VsSlot *sl = NULL; int vi_eye[4]; if(!ll_find(base, nv, &sl, vi_eye)){ g_ll.nomatch++; ll_log(t); return 0; }
  VsViews nw; int ok = 0;
  if(g_cfg.late_pred_ms > 0 && xr_locate_eyes && g_x_disp) ok = xr_locate_eyes(g_x_disp + (long long)(g_cfg.late_pred_ms * 1e6), &nw);
  if(!ok) ok = get_views(&nw);
  if(!ok){ g_ll.nopose++; ll_log(t); return 0; }
  float q1[2][4]; memcpy(q1[0], nw.eye[0].pose.q, 16); memcpy(q1[1], nw.eye[1].pose.q, 16);
#if !defined(BSI_ANDROID)
  { double deg; int m = late_test(&deg);
    if(m == 1) for(int e = 0; e < 2; e++){ float y[4], a[4]; yaw_quat(deg, y); memcpy(a, q1[e], 16);   /* q1 * yaw */
      q1[e][0] = a[3]*y[0] + a[0]*y[3] + a[1]*y[2] - a[2]*y[1]; q1[e][1] = a[3]*y[1] - a[0]*y[2] + a[1]*y[3] + a[2]*y[0];
      q1[e][2] = a[3]*y[2] + a[0]*y[1] - a[1]*y[0] + a[2]*y[3]; q1[e][3] = a[3]*y[3] - a[0]*y[0] - a[1]*y[1] - a[2]*y[2]; }
    else if(m == 2) memcpy(q1[1], nw.eye[0].pose.q, 16); }   /* oracle: eye 1 takes eye 0's (yawed) orientation */
#endif
  int turned = 0; float qn[2][4]; memcpy(qn[0], sl->v.eye[0].pose.q, 16); memcpy(qn[1], sl->v.eye[1].pose.q, 16);
  for(uint32_t i = 0; i < nv && i < 4; i++){ int e = vi_eye[i]; if(e < 0) continue;
    if(!memcmp(sl->v.eye[e].pose.q, q1[e], 16)){ g_ll.same++; continue; }
    double D[9], deg = vs_late_delta(sl->v.eye[e].pose.q, q1[e], D);
    if(!(deg <= g_cfg.late_max_deg)){ g_ll.big++; continue; }
    vs_late_patch_view(M + base + i * 0x11A0u, D);
    memcpy(qn[e], q1[e], 16); turned |= 1 << e; g_ll.eyes++; g_ll.deg_sum += deg; if(deg > g_ll.deg_max) g_ll.deg_max = deg; }
  { double gt = t - sl->t_gt; if(gt >= 0 && gt < 2.0){ g_ll.gt_sum += gt; g_ll.ngt++; if(gt > g_ll.gt_max) g_ll.gt_max = gt; } }
  /* the ring entry now describes the latched frame: new orientations (eye positions unchanged), the sample's time */
  sl->seq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  for(int e = 0; e < 2; e++) if(turned >> e & 1) memcpy(sl->v.eye[e].pose.q, qn[e], 16);
  if(nw.xr_time) sl->v.xr_time = nw.xr_time;
  sl->t_sample = t;
  __atomic_thread_fence(__ATOMIC_RELEASE); sl->seq++;
  { static int nl; if(nl < 2 && (turned || g_ll.same)){ nl++;
      fprintf(stderr, "[vr_stereo] late latch: frame of Present #%u, %u views, eyes turned mask %d, %.0f ms after the game thread's sample\n",
              sl->present, nv, turned, 1000.0 * (t - sl->t_gt)); } }
  ll_log(t);
  return turned != 0;
}
#endif
