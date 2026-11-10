/* vr_stereo.h: shared types for linux/vr_stereo.c (stereo prototype), linux/vr_ui.c (UI layer), linux/vr_aim.c (controller
   aim), xr_glue.c (regress/vr/vr_ui.patch) and the host tests in regress/vr/ and regress/.
   No OpenXR types here, so the game-side code builds on hosts without the OpenXR headers. */
#pragma once
#include <stdint.h>

typedef struct { float p[3]; float q[4]; } VsPose;          /* OpenXR pose in LOCAL space: metres, quaternion x,y,z,w */
typedef struct { VsPose pose; float fov[4]; } VsEye;        /* fov angles in radians: left, right, up, down (XrFovf order) */
typedef struct { VsPose head; VsEye eye[2]; long long xr_time; double t_wall; int valid; } VsViews;
typedef struct {
  float ipd_scale, uu_per_m; int pos; float pos_max, near_override; int vs2; float lag_ms, pred_ms; int log; int copy, aer;
  /* UI layer (regress/vr, notes/VR_STEREO.md §11) */
  int ui, ui_w, ui_h;          /* ui=0 off, 1 on; ui=WxH sets the UI rect size (0 = auto: a third of the width, 16:9) */
  int ui_lock;                 /* 0 head-locked, 1 world-locked (LOCAL), 2 follow (world-locked, re-centres lazily on yaw) */
  float ui_dist, ui_width, ui_y, ui_follow_deg;   /* quad distance / width / height offset (m), follow threshold (deg) */
  int ui_proj;                 /* 1: HUD/GFx world->screen projection = the UI quad's own frustum (markers line up) */
  int ui_blend;                /* 0 alpha (premultiplied), 1 additive (XR_FB_composition_layer_alpha_blend), 2 opaque */
  int ui_alpha;                /* 0 game alpha as is, 1 RHI coverage alpha (linux/vr_ui_rhi.c), 2 luma key (route B copy only) */
  float mono_aspect;           /* display aspect of frames that fill the whole buffer (movies, loading): 16:9 */
  int mono_src;                /* frames without a Draw tag (movies, loading): 0 show the whole buffer, 1 show the UI rect */
  int gpu_ts_eye;              /* vr69: 1 = the game's GPU timestamps run at half speed while two eyes are rendered, so its
                                  GPU-time-driven detail scaling sees a per-eye frame (mono detail); 0 = the real frame time */
  float fov_pad;               /* vr69: degrees added to every side of each eye's fov (rendered and reported), so the
                                  compositor's reprojection has picture to show at the edges during fast head turns
                                  (vr70: default 6 with stereo) */
  int fif;                     /* vr70: frames in flight while the stereo layout is on (rhi_b1 present; 0 = dxvk.conf's value) */
  int ao_crop;                 /* vr70: the HDAO blur dispatches stop this many pixels right of the eye area (0 = off) */
  int late;                    /* vr71: late head-pose latch on the render thread (1 default, 0 = build 70) */
  float late_pred_ms;          /* vr71: extra prediction (ms, 0..100) for the late pose (opt-in, needs xr_glue's locate hook) */
  float late_max_deg;          /* vr71: larger orientation changes are not latched (recenter, tracking loss) */
  int ao_eye;                  /* vr71: 1 = the left eye view's AO passes are not dispatched; ao74: 2 (default) = each eye's AO
                                  main pass with its own view's constants and column mapping (rhi_b1_vr.inc); 0 = build 70 */
  int sharpen;                 /* vr71: projection layer also allows the compositor's sharpening filter (xr_glue.c; 0 = off).
                                  vr72: kept in step with filter (1 = filter sharpen, 2 = filter sharpen_normal) */
  int filter;                  /* vr72: the projection layer's compositor filter (xr_glue.c), VS_FILTER_*; default none */
  int res_w, res_h;            /* perf73: VR back-buffer size (res=WxH, whole buffer incl. the UI column); 0 = auto. In stereo
                                  resolution.txt (the FLAT size) is ignored: xr_glue.c sizes the surface, android_glue.c passes
                                  it to the game (vs_vr_size) */
  float eye_mpix;              /* perf73: res=auto pixel budget per eye in megapixels (default 1.04 = build 72's 960x1080 per
                                  eye, i.e. about the same GPU cost), shaped like the runtime's recommended eye image */
  int ls_eye;                  /* pp76: 1 (default) = the left eye's tone-map pass reads the light shafts of its own view (the
                                  right view's light-shaft passes overwrite the shared result first); 0 = build 75 */
  int shadow_eye;              /* pp76: 1 (default) = the sun's whole-scene shadow projection (drawn by the game for the left eye
                                  only) is replayed for the right eye with the right view's constants; 0 = build 75 (stale mask) */
  int fxaa_eye;                /* pp76: 1 (default) = FXAA per eye column (clamped at the eye's edges like the eye alone); 0 = build 75 */
  int bloom_eye;               /* pp76: 1 (default) = the second view's bloom chain gets the right eye's column; 0 = build 75 */
} VsCfg;
/* vr72: projection layer filter (XR_FB_composition_layer_settings). none = no settings struct on the projection layer (the
   eye images are upscaled ~4x by the compositor: super-sampling would only blur, sharpening amplifies aliasing/noise);
   auto = super-sampling + XR_META_automatic_layer_filter (builds 70/71 default); sharpen = + quality sharpening (= Meta Quest
   Super Resolution; build 71 sharpen=1); sharpen_normal = normal (temporal 3-tap) sharpening + auto. Quads keep their flags. */
enum { VS_FILTER_NONE = 0, VS_FILTER_AUTO = 1, VS_FILTER_SHARPEN = 2, VS_FILTER_SHARPEN_NORMAL = 3 };

/* Back buffer layout: [ left eye | right eye | UI column ]. Without the UI layer: [ left | right ]. */
typedef struct { int W, H, eye_w, ui, ui_x, ui_y, ui_w, ui_h; } VsLayout;
/* frame kinds in the Present ring */
enum { VS_KIND_NONE = 0, VS_KIND_STEREO = 1, VS_KIND_MONOWIDE = 2, VS_KIND_UIONLY = 3,
       VS_KIND_MONOHEAD = 4 };   /* vr69: flat frame (no stereo layout) rendered with head-look; v.head.q = its camera orientation */
/* what the XR thread shows for one Present */
typedef struct { int kind; int mask; VsEye eye[2]; VsLayout L; uint32_t present;
                 long long xr_time; double t_sample; } VsXrFrame;   /* vr70: display time the poses were predicted for, GT sample time */

/* pure math (host-testable) */
void     vs_quat_to_mat(const float q[4], double R[9]);
void     vs_eye_offset(const VsPose *head, const VsPose *eye, const VsCfg *cfg, double d_v[3], double Q[9]);
void     vs_eye_view(const float V[16], const double d_v[3], const double Q[9], float Ve[16]);
void     vs_view_rotate(const float V[16], const double Q[9], float Vr[16]);
void     vs_eye_proj(const float Pg[16], const float fov[4], float near_override, float Pe[16]);
uint32_t vs_assign_present(uint32_t last_assigned, uint32_t presents_now);
int      vs_pick_present(const uint32_t *p, const double *t, int n, double now, double lag_s, uint32_t *out);
void     vs_parse_opts(VsCfg *c, const char *s);
void     vs_cfg_defaults(VsCfg *c);
void     vs_layout(int W, int H, const VsCfg *c, VsLayout *L);
/* perf73: the VR back-buffer size (XR thread, before the surface is made). rec_w/rec_h = the runtime's recommended eye image
   (0 = unknown). Returns 1 explicit res=WxH, 2 auto from the recommended aspect, 3 auto with the Quest 3 fallback aspect. */
int      vs_vr_size(const VsCfg *c, int rec_w, int rec_h, int *W, int *H);
/* perf73: centre sharpness (pixels per degree) of an eye image eye_px wide/high over the fov tangents t0 + t1 widened by pad
   degrees per side (fov angles in radians, XrFovf convention: left/down negative) */
double   vs_px_per_deg(int eye_px, float a0, float a1, float pad_deg);
uint32_t vs_crop_groups(uint32_t x, uint32_t y, uint32_t uw, uint32_t uh, int bb_w, int bb_h, int drawn_x, int margin, uint32_t *gx);   /* vr70 */
/* vr71: late head-pose latch (pure parts, host-tested) */
double   vs_late_delta(const float q0[4], const float q1[4], double D[9]);   /* view-space rotation old eye -> new eye; returns degrees */
void     vs_late_patch_view(uint8_t *vi, const double D[9]);               /* rotate one FSceneView/FViewInfo (host pointer) by D */
/* vr71: OpenXR two-call enumeration of swapchain formats (route B, reticle); enumf = xrEnumerateSwapchainFormats(ctx, ...)
   returns the XrResult. Picks GL_SRGB8_ALPHA8 (0x8C43), else GL_RGBA8 (0x8058); *fmt = 0 if neither. Returns the last
   XrResult (0 = XR_SUCCESS); *count = the runtime's format count; up to cap formats are copied to list (may be NULL). */
typedef int (*vs_enum_fmt_fn)(void *ctx, uint32_t cap, uint32_t *n, int64_t *out);
int      vs_pick_swapchain_format(vs_enum_fmt_fn enumf, void *ctx, int64_t *fmt, uint32_t *count, int64_t *list, uint32_t cap);

/* XR thread (xr_glue.c) */
int       vr_stereo_layout(void);
void      vr_stereo_xr_set_views(const VsViews *v);
int       vr_stereo_xr_layer(VsEye out[2]);
int       vr_stereo_xr_frame(int W, int H, VsXrFrame *f);    /* route A: kind of the newest presented frame (VS_KIND_NONE = whole buffer) */
long long vr_stereo_pred_ns(void);
const VsCfg *vr_stereo_cfg(void);
void      vr_stereo_disable(const char *why);
int       vr_stereo_route_copy(void);                      /* route=copy requested (xr_stereo_copy.c) */
void      vr_stereo_set_route(int copy);                   /* XR glue: the copy route is (not) running */
int       vr_stereo_xr_entry(uint32_t present, VsEye out[2]);   /* eye mask (1 left, 2 right, 3 both) of that frame, 0 = mono */
int       vr_stereo_xr_entry2(uint32_t present, int *mask, VsEye out[2]);   /* kind of that frame (VS_KIND_NONE = no entry: whole buffer) */
void      vr_stereo_get_layout(int W, int H, VsLayout *L);      /* layout the game renders (UI column once the game used it) */
void      vr_stereo_get_layout_intent(int W, int H, VsLayout *L);   /* layout asked for at startup (route B swapchain sizes) */

/* game thread (hooked part007, regress/vr/vr_hook_parts.sh; old regress/vr_stereo_part007.sh still works) */
int  vr_stereo_csv_begin(uint32_t lp, uint32_t ret);
int  vr_stereo_csv_begin2(uint32_t lp, uint32_t ret, int vw, int vh);   /* + viewport size: 4 = one mono view over the eye area */
void vr_stereo_csv_mono(uint32_t ret);                                  /* mask 0: plain call (aim view fix only) */
int  vr_stereo_need_viewstate2(void);
void vr_stereo_set_viewstate2(uint32_t vs);
void vr_stereo_csv_eye(uint32_t lp, int eye);                           /* 0 left, 1 right, 2 mono over the eye area */
void vr_stereo_csv_end(uint32_t lp, uint32_t view0, uint32_t view1);
void vr_stereo_view_ctor_args(uint32_t esp);
/* used by vr_ui.c */
int  vr_stereo_ui_layout(int vw, int vh, VsLayout *L);    /* 1 if the UI column is in use for this viewport size */
int  vr_stereo_ui_on(void);                               /* stereo layout with the UI column (cheap; hooks ask first) */
uint32_t vr_stereo_take_pair(float Vhead[16], float Pgame[16]);   /* view0 of this Draw's CalcSceneView (0 = none) + head view */
void vr_stereo_mark_uionly(void);                         /* a Draw without a world view: tag its Present for the XR side */

/* UI layer, game thread (linux/vr_ui.c; called from the hooked part007/part012) */
typedef struct { float x, y, w, h; int clear; } VrTile;
int  vr_ui_tile(float x, float y, float w, float h, int vw, int vh, VrTile out[2]);
void vr_ui_after_cover(int vw, int vh);
void vr_ui_subtitle_rect(uint32_t rect, int vw, int vh);
int  vr_ui_full_canvas(uint32_t ucanvas, int vw, int vh, uint32_t *matrix);
void vr_ui_gfx_viewport(uint32_t pview, uint32_t vp, int site);
uint32_t vr_ui_zero_color(void);
void vr_ui_rhi_frame(int on);      /* linux/vr_ui_rhi.c: RHI coverage-alpha hook on/off (installs on first use) */
/* UI pure math */
void vs_ui_proj(const float Pg[16], double tan_hx, double aspect, float P[16]);
int  vs_mat4_inverse(const float m[16], float out[16]);
void vs_mat4_mul(const float a[16], const float b[16], float out[16]);
void vs_map_rect(const VsLayout *L, int vw, int vh, const float in[4], float out[4]);   /* full-viewport rect -> UI rect */
float vs_follow_yaw(float cur, float head, float thresh, float dt, float rate, int *moving);   /* radians */

/* vr69: native renderer (BSI_RHI=b1n; linux/rhi_b1_vr.inc calls these through weak references) and the flat-screen pose */
void     vr_stereo_set_native(int on);                    /* xr_glue: env.txt asks for BSI_RHI=b1n (route=auto -> copy) */
int      vr_stereo_native(void);
int      vr_stereo_native_frame(int W, int H, int *eye_w, int *ui_alpha);   /* rhi_b1 per Present: stereo layout on? */
int      vr_stereo_present_fifo(void);                    /* rhi_b1: route B runs, present FIFO (no dropped frames) */
void     vr_stereo_image_presented(uint32_t present);     /* rhi_b1: one more image queued, made by Present #present */
uint32_t vr_stereo_image_present(uint32_t k);             /* xr_stereo_copy.c: image k (1-based) -> Present index, 0 = unknown */
uint32_t vr_stereo_images_presented(void);
void     vr_stereo_rt_check(uint32_t present, int saw_right);   /* rhi_b1: tag check (right-eye scene viewport seen?) */
void     vr_stereo_rt_frame(uint32_t present, int saw_right, int drew_eye, int drew_ui);   /* + untagged UI-only Presents */
int      vr_stereo_native_ts_half(void);                  /* rhi_b1: report GPU timestamps at half speed (gpu_ts=eye) */
int      vr_stereo_xr_mono_head(float q[4], double *age_ms);   /* XR: orientation of the newest presented flat frame */
/* vr70: latency, frames in flight, AO crop, completion-based frame pick */
int      vr_stereo_frames_in_flight(void);                /* rhi_b1: frames in flight while the stereo layout is on (0 = keep) */
int      vr_stereo_ao_crop(void);                         /* rhi_b1: AO dispatch crop margin in pixels (0 = off) */
int      vr_stereo_crop_edge(int W, int H, uint32_t present);   /* rhi_b1: eye-area right edge for that frame's AO crop (0 = do not cut) */
void     vr_stereo_frame_queued(unsigned long long fid, uint32_t present);   /* rhi_b1: presenter frame fid = game Present #present */
void     vr_stereo_xr_display_time(double t_wall, long long xr_time);   /* XR thread, each frame: predicted display time (CLOCK_MONOTONIC s) */
void     vr_stereo_lat_done(uint32_t present);            /* route B: the image of Present #present finished on the GPU (seen now) */
void     vr_stereo_lat_shown(uint32_t present, int route);   /* the XR thread shows Present #present for the first time */
int      vr_stereo_xr_entry3(uint32_t present, int *mask, VsEye out[2], long long *xr_time);   /* entry2 + the predicted display time */
/* vr71: late head-pose latch */
int      vr_stereo_late_latch(uint32_t scene_renderer);
int      vr_stereo_ao_eye(void);                          /* rhi_b1: ao_eye wanted (stereo + UI column layout) */
int      vs_shadow_eye_consts(const float vs1_l[20], const float vs1_r[20], const float vs0_l[12], const float ps0_l[16],
                              float vs1_out[20], float vs0_out[12], float ps0_row3[4]);   /* pp76: right-eye sun shadow constants (pure) */
int      vs_fx_copy_plan(int x0, int x1, int W, int m, int plan[][4], int max);   /* pp76: per-eye FXAA input copies (pure) */
int      vr_stereo_pp_eye(int which);                     /* pp76, rhi_b1: 0 = ls_eye, 1 = shadow_eye, 2 = fxaa_eye, 3 = bloom_eye (stereo + UI column layout) */
float    vs_ao_eye_tan(float tan_game, float W, int ew);   /* ao74: the AO tan(half fov x) the game would give a mono view ew wide */
void     vs_ao_eye_map(float tan_game, float W, int x0, int ew, float *inv, float *tan_x);   /* ao74: AO constants of an eye column (pure) */
int      vr_stereo_eye_frame(int W, int H, uint32_t present);   /* rhi_b1: eye width if that frame is a stereo pair (both eyes), else 0 */   /* render thread, start of the scene command (vr_ui_rhi.c hook): 1 = latched */
int      vr_late_install(void);                           /* vr_ui_rhi.c: point FDrawSceneCommand::Execute at the latch (once) */
int      xr_locate_eyes(long long xr_time, VsViews *out); /* xr_glue.c (VR APK): eye views for a given XrTime (late_pred_ms) */

/* controller aim (linux/vr_aim.c, vr_aim.h) */
int  vr_aim_view_q(double Q[9]);      /* game thread: aim-view -> head-view correction for this frame (1 = apply) */
int  vr_stereo_view_fix_alive(void);  /* the hooked CalcSceneView can rotate the rendered view (needed before aim may start) */
