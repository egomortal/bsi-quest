/* vr_aim (VR, playability step 2; notes/VR_STEREO.md §11): the gun aims with the right Touch controller, the view stays
   on the head.

   How UE3 aims: a weapon fires from Instigator.GetWeaponStartTraceLocation() along GetAdjustedAim(), which for a human
   player is Controller.GetPlayerViewPoint() = the camera's CameraCache.POV (APlayerController::GetPlayerViewPoint f_005e10c0
   reads it). The first-person arms/weapon are also placed from the view rotation. So "where the camera looks" and "where
   the gun points" are one value in the game, and vr_head.c puts the head there (head-look = head-aim).

   Decision: the CAMERA (POV.Rotation, i.e. the gun) follows the controller, and only the RENDERED view goes back to the
   head. Rotating PlayerController.Rotation instead would also turn the body/movement and would still not separate aim
   from view (the camera adds the head on top). Rendering is the only place where the head must win, and the hooked
   CalcSceneView already owns the view matrix: vr_stereo_view_ctor_args() rotates the game's view by
       Q = S^T M(A) M(H)^T S        (A = camera rotation with aim, H = the same with the head; M = FRotationMatrix,
                                     S = UE3's (X fwd, Y right, Z up) -> view (x right, y up, z fwd) swizzle)
   about the camera position, before the eye offsets. Valid for any world-side rotation/translation in the game's matrix.
   Consequences (by design): bullets, the weapon model and Canvas/Scaleform aim follow the controller; the picture, the
   stereo eyes and the UI layer follow the head; audio listener follows the camera (= the gun) - small, noted.
   Safety: aim mode only starts while the hooked CalcSceneView is seen applying the fix (vr_stereo_view_fix_alive), so a
   build without the hook never turns the picture toward the controller. Toggling fades over 0.3 s.

   Aim ray: the game traces from the camera (eye) position, not from the hand. aim_conv=D aims the camera ray through the
   point D metres along the controller's ray (exact at D, < 1 deg off beyond ~2 D/3 for a 0.3 m hand offset); conv=0 =
   parallel to the controller. The reticle (a small quad layer) is drawn on the camera ray, i.e. where the shot goes.

   Switch: /sdcard/BSI/vr_aim.txt (exists = on, re-read every 2 s; BSI_VR_AIM=1/0 forces; BSI_VR_AIM_FILE = other path).
   Options: conv=8  stickpitch=0  reticle=1.2 (deg, 0 = none)  reticle_dist=6 (m)  log=1.
   Needs head-look (vr_head.txt) and the hooked part007 from regress/vr/vr_hook_parts.sh (stereo or not). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#ifdef BSI_ANDROID
#include <dlfcn.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#define VR_AIM_XR
#endif
#include "vr_aim.h"
#include "vr_stereo.h"   /* vr71: vs_pick_swapchain_format */

/* ---------------- pure math ---------------- */
void va_parse_opts(VaCfg *c, const char *s){
  char w[64]; int n;
  while(s && sscanf(s, "%63s%n", w, &n) == 1){
    s += n; char *eq = strchr(w, '='); if(!eq) continue; *eq = 0; const char *v = eq + 1; float f = strtof(v, NULL);
    if(!strcmp(w, "conv") && f >= 0 && f <= 1000) c->conv = f;
    else if(!strcmp(w, "stickpitch")) c->stickpitch = atoi(v) != 0;
    else if(!strcmp(w, "reticle") && f >= 0 && f <= 10) c->reticle_deg = f;
    else if(!strcmp(w, "reticle_dist") && f >= 0.3f && f <= 100) c->reticle_dist = f;
    else if(!strcmp(w, "log")) c->log = atoi(v);
  }
}
/* gun direction in LOCAL: the controller's -Z, or (conv > 0) from the head through the controller ray's point at conv m */
void va_aim_dir(const float pa[3], const float qa[4], const float ph[3], float conv, double d[3]){
  double x = qa[0], y = qa[1], z = qa[2], w = qa[3], n = x*x + y*y + z*z + w*w, s = n > 1e-12 ? 2.0 / n : 0;
  double f[3] = { -s * (x*z + y*w), -s * (y*z - x*w), -(1 - s * (x*x + y*y)) };
  if(conv > 0){
    double t[3] = { pa[0] + f[0] * conv - ph[0], pa[1] + f[1] * conv - ph[1], pa[2] + f[2] * conv - ph[2] };
    double l = sqrt(t[0]*t[0] + t[1]*t[1] + t[2]*t[2]);
    if(l > 0.05){ d[0] = t[0] / l; d[1] = t[1] / l; d[2] = t[2] / l; return; }
  }
  double l = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]); if(l < 1e-9) l = 1;
  d[0] = f[0] / l; d[1] = f[1] / l; d[2] = f[2] / l;
}
/* same yaw/pitch convention as vr_head.c's YXZ decomposition of the head quaternion (forward = -Z) */
void va_dir_angles(const double d[3], double *yaw, double *pitch){
  *yaw = atan2(-d[0], -d[2]);
  double s = d[1]; if(s > 1) s = 1; if(s < -1) s = -1;
  *pitch = asin(s);
}
void va_rot_matrix(const int32_t r[3], double M[9]){
  const double k = M_PI / 32768.0;
  double SP = sin(r[0] * k), CP = cos(r[0] * k), SY = sin(r[1] * k), CY = cos(r[1] * k), SR = sin(r[2] * k), CR = cos(r[2] * k);
  M[0] = CP * CY;                 M[1] = CP * SY;                 M[2] = SP;
  M[3] = SR * SP * CY - CR * SY;  M[4] = SR * SP * SY + CR * CY;  M[5] = -SR * CP;
  M[6] = -(CR * SP * CY + SR * SY); M[7] = CY * SR - CR * SP * SY; M[8] = CR * CP;
}
/* Q = S^T M(A) M(H)^T S: UE3 view rotation R(X) = M(X)^T S, so R(A) Q = R(H) */
void va_view_q(const int32_t A[3], const int32_t H[3], double Q[9]){
  double MA[9], MH[9], T[9];
  va_rot_matrix(A, MA); va_rot_matrix(H, MH);
  for(int i = 0; i < 3; i++) for(int j = 0; j < 3; j++){ double s = 0; for(int k = 0; k < 3; k++) s += MA[i*3 + k] * MH[j*3 + k]; T[i*3 + j] = s; }
  /* S maps local (X,Y,Z) to view (z,x,y): S = [[0,0,1],[1,0,0],[0,1,0]]; (S^T T S)[i][j] = T[p(i)][p(j)] with p = view->local axis */
  static const int p[3] = { 1, 2, 0 };   /* view x = local Y, view y = local Z, view z = local X */
  for(int i = 0; i < 3; i++) for(int j = 0; j < 3; j++) Q[i*3 + j] = T[p[i]*3 + p[j]];
}
void va_quat_yaw_pitch(double yaw, double pitch, float q[4]){
  double cy = cos(yaw * 0.5), sy = sin(yaw * 0.5), cx = cos(pitch * 0.5), sx = sin(pitch * 0.5);
  q[0] = (float)(cy * sx); q[1] = (float)(sy * cx); q[2] = (float)(-sy * sx); q[3] = (float)(cy * cx);
}

/* ---------------- state ---------------- */
#ifndef VA_NO_RUNTIME
int vr_stereo_view_fix_alive(void) __attribute__((weak));   /* vr_stereo.c */
static VaCfg g_cfg = { 0, 8.f, 0, 1.2f, 6.f, 1 };
static double g_next_cfg;
static volatile uint32_t g_seq; static float g_pa[3], g_qa[4], g_ph[3]; static double g_pose_t; static volatile int g_have;
static float g_wa;                      /* aim weight 0..1 (fades over 0.3 s) */
static double g_last_t;
static int32_t g_A[3], g_H[3]; static int g_qvalid;
static unsigned long g_n; static int g_state_logged = -1;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void read_cfg(void){
  const char *p = getenv("BSI_VR_AIM_FILE"); if(!p || !*p) p = "/sdcard/BSI/vr_aim.txt";
  const char *e = getenv("BSI_VR_AIM");
  FILE *f = fopen(p, "r");
  int on = e && *e ? atoi(e) != 0 : f != NULL;
  if(f){ char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); va_parse_opts(&g_cfg, buf); }
  va_parse_opts(&g_cfg, getenv("BSI_VR_AIM_OPTS"));
  if(on != g_cfg.on) fprintf(stderr, "[vr_aim] controller aim %s (conv %.1f m, stick pitch %s, reticle %.1f deg at %.1f m)\n", on ? "ON" : "off",
                             g_cfg.conv, g_cfg.stickpitch ? "kept" : "off", g_cfg.reticle_deg, g_cfg.reticle_dist);
  g_cfg.on = on;
}

/* XR thread */
void vr_aim_set_pose(const float pa[3], const float qa[4], int valid, const float ph[3]){
  if(!valid) return;
  g_seq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy(g_pa, pa, 12); memcpy(g_qa, qa, 16); memcpy(g_ph, ph, 12); g_pose_t = now_s();
  __atomic_thread_fence(__ATOMIC_RELEASE); g_seq++;
  g_have = 1;
}
static int get_pose(float pa[3], float qa[4], float ph[3], double *t){
  if(!g_have) return 0;
  uint32_t s0, s1; int k = 0;
  do { s0 = g_seq; __atomic_thread_fence(__ATOMIC_ACQUIRE); memcpy(pa, g_pa, 12); memcpy(qa, g_qa, 16); memcpy(ph, g_ph, 12); *t = g_pose_t;
       __atomic_thread_fence(__ATOMIC_ACQUIRE); s1 = g_seq; } while((s0 != s1 || (s0 & 1)) && ++k < 16);
  return s0 == s1 && !(s0 & 1);
}
int vr_aim_stick_pitch_off(void){ return g_cfg.on && !g_cfg.stickpitch && g_wa > 0; }
int vr_aim_active(void){ return g_qvalid; }
/* game thread (vr_head.c): head-look off, no pose, or the game owns the camera (scripted look): camera = game, no fix */
void vr_aim_reset(void){ g_qvalid = 0; g_wa = 0; g_last_t = 0; }

static int32_t rad2ue(double r){ return (int32_t)lrint(r * (32768.0 / M_PI)); }

/* game thread, from vr_head_apply() after it added the head (see vr_aim.h) */
int vr_aim_camera(const int32_t base[3], int32_t r[3], float yaw0, float w){
  double t = now_s();
  if(t >= g_next_cfg){ g_next_cfg = t + 2.0; read_cfg(); }
  float pa[3], qa[4], ph[3]; double pt = 0;
  int pose = get_pose(pa, qa, ph, &pt) && t - pt < 0.25;
  int fix = &vr_stereo_view_fix_alive && vr_stereo_view_fix_alive();
  int want = g_cfg.on && pose && fix;
  if(want != g_state_logged && (g_cfg.on || g_state_logged == 1)){
    g_state_logged = want;
    fprintf(stderr, "[vr_aim] %s%s%s\n", want ? "aiming with the right controller" : "aim inactive:",
            want ? "" : (!pose ? " no controller aim pose" : ""), want ? "" : (!fix ? " no view fix (needs the hooked part007 from regress/vr)" : "")); }
  float dt = g_last_t > 0 ? (float)(t - g_last_t) : 0.f; g_last_t = t; if(dt > 0.1f) dt = 0.1f;
  g_wa += (want ? dt : -dt) / 0.3f; if(g_wa < 0) g_wa = 0; if(g_wa > 1) g_wa = 1;
  if(g_wa <= 0){ g_qvalid = 0; return 0; }
  double d[3], yaw = 0, pitch = 0;
  if(pose){ va_aim_dir(pa, qa, ph, g_cfg.conv, d); va_dir_angles(d, &yaw, &pitch); }
  else { yaw = yaw0; pitch = 0; }
  double dy = yaw - yaw0; while(dy > M_PI) dy -= 2 * M_PI; while(dy < -M_PI) dy += 2 * M_PI;
  int32_t hy = r[1] - base[1], hp = (int16_t)(uint16_t)(r[0] - base[0]), hr = r[2] - base[2];
  int32_t ay = rad2ue(-dy * w), ap = rad2ue(pitch * w);
  int32_t A[3];
  A[0] = (int16_t)(uint16_t)(base[0] + hp + (int32_t)lrintf(g_wa * (float)(ap - hp)));
  if(A[0] > 16000) A[0] = 16000;
  if(A[0] < -16000) A[0] = -16000;
  A[1] = base[1] + hy + (int32_t)lrintf(g_wa * (float)(ay - hy));
  A[2] = base[2] + (int32_t)lrintf((1.f - g_wa) * (float)hr);
  memcpy(g_H, r, 12); memcpy(g_A, A, 12); g_qvalid = 1;
  memcpy(r, A, 12);
  if(g_cfg.log && (++g_n & 0xFFF) == 1)
    fprintf(stderr, "[vr_aim] camera = aim: yaw %+.1f pitch %+.1f deg (head yaw %+.1f pitch %+.1f), weight %.2f\n",
            -ay * (180.0 / 32768.0), ap * (180.0 / 32768.0), -hy * (180.0 / 32768.0), hp * (180.0 / 32768.0), g_wa);
  return 1;
}
/* game thread, from the FSceneView ctor hook (vr_stereo.c): rotate the aim view back to the head view */
int vr_aim_view_q(double Q[9]){
  if(!g_qvalid) return 0;
  va_view_q(g_A, g_H, Q);
  return 1;
}

#ifdef VR_AIM_XR
/* ---------------- XR thread: reticle layer ---------------- */
#define GLR(X) X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) X(glViewport) X(glClearColor) X(glClear) X(glEnable) X(glDisable) X(glScissor) X(glFinish)
#define GLR_DECL(f) static __typeof__(f) *r_##f;
GLR(GLR_DECL)
static XrSwapchain g_rsc; static int g_rs = 0; static int g_rw = 64;
static void rect_clear(int x0, int y0, int x1, int y1, float c){ r_glScissor(x0, y0, x1 - x0, y1 - y0); r_glClearColor(c, c, c, 1.f); r_glClear(GL_COLOR_BUFFER_BIT); }
static void draw_cross(int k){   /* k = 0: dark outline, 1: white inside (shrunk by one pixel) */
  float c = k ? 1.f : 0.f; int s = k;
  rect_clear(6 + s, 29 + s, 26 - s, 35 - s, c); rect_clear(38 + s, 29 + s, 58 - s, 35 - s, c);
  rect_clear(29 + s, 6 + s, 35 - s, 26 - s, c); rect_clear(29 + s, 38 + s, 35 - s, 58 - s, c);
  rect_clear(30 + s, 30 + s, 34 - s, 34 - s, c);
}
static const VaXrFns *g_rfns;   /* vr71 */
static int reticle_enum(void *ctx, uint32_t cap, uint32_t *n, int64_t *out){ (void)ctx; return (int)g_rfns->formats(g_rfns->sess, cap, n, out); }
static int reticle_init(const VaXrFns *f){
  void *h = dlopen("libGLESv3.so", RTLD_NOW); if(!h) h = dlopen("libGLESv2.so", RTLD_NOW);
  if(!h){ fprintf(stderr, "[vr_aim] reticle: no GLES library\n"); return -1; }
#define GLR_LOAD(fn) if(!(r_##fn = (__typeof__(r_##fn))dlsym(h, #fn))){ fprintf(stderr, "[vr_aim] reticle: missing %s\n", #fn); return -1; }
  GLR(GLR_LOAD)
#undef GLR_LOAD
  /* vr71: two-call enumeration (vr_stereo.c; a fixed 64-entry array failed with XR_ERROR_SIZE_INSUFFICIENT on the Quest 3's
     71 formats, so the reticle never appeared) */
  uint32_t nf = 0; int64_t fmt = 0; g_rfns = f;
  int fr = vs_pick_swapchain_format(reticle_enum, NULL, &fmt, &nf, NULL, 0);
  if(!fmt){ fprintf(stderr, "[vr_aim] reticle: no GL_SRGB8_ALPHA8 / GL_RGBA8 swapchain format (xrEnumerateSwapchainFormats %d, %u formats)\n", fr, nf); return -1; }
  XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
  ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = fmt; ci.sampleCount = 1; ci.width = g_rw; ci.height = g_rw; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
  XrResult r = f->create(f->sess, &ci, &g_rsc); if(r != XR_SUCCESS){ fprintf(stderr, "[vr_aim] reticle swapchain: %d\n", r); return -1; }
  XrSwapchainImageOpenGLESKHR im[4]; uint32_t ni = 0; for(int i = 0; i < 4; i++){ im[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR; im[i].next = NULL; }
  { uint32_t cnt = 0; XrResult er = f->images(g_rsc, 0, &cnt, NULL);   /* vr71: two-call idiom */
    if(er != XR_SUCCESS || !cnt || cnt > 4){ fprintf(stderr, "[vr_aim] reticle swapchain images: %d, %u images\n", er, cnt); return -1; } }
  if(f->images(g_rsc, 4, &ni, (XrSwapchainImageBaseHeader*)im) != XR_SUCCESS || !ni) return -1;
  uint32_t idx = 0; XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
  if(f->acquire(g_rsc, &ai, &idx) != XR_SUCCESS) return -1;
  XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION; f->wait(g_rsc, &wi);
  GLuint fbo; r_glGenFramebuffers(1, &fbo); r_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  r_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im[idx].image, 0);
  r_glViewport(0, 0, g_rw, g_rw); r_glDisable(GL_SCISSOR_TEST); r_glClearColor(0, 0, 0, 0); r_glClear(GL_COLOR_BUFFER_BIT);
  r_glEnable(GL_SCISSOR_TEST); draw_cross(0); draw_cross(1); r_glDisable(GL_SCISSOR_TEST);
  r_glFinish(); r_glBindFramebuffer(GL_FRAMEBUFFER, 0);
  XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; f->release(g_rsc, &ri);
  fprintf(stderr, "[vr_aim] reticle ready (%dx%d static swapchain, format 0x%llx)\n", g_rw, g_rw, (unsigned long long)fmt);
  return 1;
}
int vr_aim_reticle_layer(const VaXrFns *f, XrCompositionLayerQuad *q){
  if(!g_cfg.on || g_cfg.reticle_deg <= 0 || g_wa < 0.5f || !g_qvalid) return 0;
  if(g_rs == 0) g_rs = reticle_init(f);
  if(g_rs != 1) return 0;
  float pa[3], qa[4], ph[3]; double pt; if(!get_pose(pa, qa, ph, &pt)) return 0;
  double d[3], yaw, pitch; va_aim_dir(pa, qa, ph, g_cfg.conv, d); va_dir_angles(d, &yaw, &pitch);
  float dist = g_cfg.reticle_dist, qq[4]; va_quat_yaw_pitch(yaw, pitch, qq);
  memset(q, 0, sizeof *q); q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
  q->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; q->space = f->local; q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q->subImage.swapchain = g_rsc; q->subImage.imageRect.extent.width = g_rw; q->subImage.imageRect.extent.height = g_rw;
  q->pose.position.x = ph[0] + (float)d[0] * dist; q->pose.position.y = ph[1] + (float)d[1] * dist; q->pose.position.z = ph[2] + (float)d[2] * dist;
  q->pose.orientation.x = qq[0]; q->pose.orientation.y = qq[1]; q->pose.orientation.z = qq[2]; q->pose.orientation.w = qq[3];
  q->size.width = q->size.height = 2.f * dist * tanf(g_cfg.reticle_deg * (float)M_PI / 360.f);
  return 1;
}
#endif
#endif
