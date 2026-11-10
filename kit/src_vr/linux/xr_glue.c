/* BSI-Quest immersive mode (OpenXR).
   The game (DXVK on Turnip) renders into an Android Surface that belongs to an OpenXR "Android surface swapchain";
   the Quest compositor shows it as a large screen (quad layer) in front of the player. The Touch controllers are read
   through OpenXR actions and exposed to the game as an Xbox (XInput) gamepad.
   Because the picture is a compositor layer, the headset's own capture/recording sees it.
   No graphics work happens here: a tiny GLES context only satisfies xrCreateSession's graphics binding. */
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_activity.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_TIMESPEC
#include <time.h>
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "android_plat.h"

#define LOGX(...) do{ __android_log_print(ANDROID_LOG_INFO, "BSIQuestXR", __VA_ARGS__); fprintf(stderr, "[xr] " __VA_ARGS__); fputc('\n', stderr); }while(0)

/* vr70: early log capture. The XR thread decides the route (and route B initialises) before the game thread opens bsi.log
   (android_glue.c: freopen(<files>/bsi.log, "w", stderr) after rotating the old logs), so those lines only reached logcat,
   and the build 69 Quest logcat had none of them. xr_start (onCreate, before the game thread exists) now points stderr at
   <files>/xr_early.log; every early [xr] / [vr_stereo] / [xr_copy] line lands there. Once the game thread has redirected
   stderr to bsi.log (fd 2 is then another file), the XR thread copies xr_early.log into bsi.log between two marker lines.
   xr_early.log stays on the card as well. */
#include <sys/stat.h>
#include <fcntl.h>
static char g_early_path[600]; static dev_t g_early_dev; static ino_t g_early_ino; static int g_early_state;   /* 0 off, 1 waiting, 2 done */
static void early_log_open(const char *files){
  snprintf(g_early_path, sizeof g_early_path, "%s/xr_early.log", files);
  struct stat st;
  int fd = open(g_early_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);   /* dup2 onto fd 2: the stderr FILE stays as it is */
  if(fd < 0) return;
  fflush(stderr);
  if(dup2(fd, 2) < 0 || fstat(2, &st) != 0){ close(fd); return; }
  close(fd);
  g_early_dev = st.st_dev; g_early_ino = st.st_ino; g_early_state = 1;
  fprintf(stderr, "[xr] early log: lines written before bsi.log is open (copied into bsi.log once it is)\n");
}
/* XR thread: once stderr is no longer xr_early.log, copy that file into the current stderr (bsi.log). Cheap (one fstat) while waiting. */
static int early_log_flush(void){
  if(g_early_state != 1) return g_early_state == 2;
  struct stat st; if(fstat(2, &st) != 0 || (st.st_dev == g_early_dev && st.st_ino == g_early_ino)) return 0;
  g_early_state = 2;
  FILE *f = fopen(g_early_path, "r"); if(!f){ fprintf(stderr, "[xr] early log %s unreadable\n", g_early_path); return 1; }
  char ln[1024]; int n = 0;
  fprintf(stderr, "[xr] ---- early log begin (%s: XR thread lines from before bsi.log was open) ----\n", g_early_path);
  while(fgets(ln, sizeof ln, f)){ fputs(ln, stderr); size_t l = strlen(ln); if(l && ln[l - 1] != '\n') fputc('\n', stderr); n++; }
  fclose(f);
  fprintf(stderr, "[xr] ---- early log end (%d lines) ----\n", n);
  return 1;
}

/* pad state shared with android_glue.c */
void android_pad_set(const AndroidPad *p);
extern float g_xr_rumble_l, g_xr_rumble_r;
static int nofilter, has_layout, flip, has_perf, has_refresh, has_lsettings, has_autofilter, has_threads;
volatile int g_game_tid;
static ANativeActivity *g_act_xr; static char g_files_xr[512];

static PFN_xrGetInstanceProcAddr gipa;
static XrInstance inst; static XrSession sess; static XrSystemId sysid; static XrSpace local_space, view_space;
/* vr_head.c (head-look prototype) */
void vr_head_set_orientation(float qx, float qy, float qz, float qw); void vr_head_recenter(void); int vr_head_enabled(void);
int vr_head_reproject(void) __attribute__((weak));   /* vr69: vr_head.txt reproject=1 (flat screen world-locked to the rendered pose) */
/* vr_stereo.c (stereo prototype, ROADMAP item 27). Weak: this file still links without vr_stereo.o (then: no stereo). */
#include "vr_stereo.h"
#pragma weak vr_stereo_layout
#pragma weak vr_stereo_xr_set_views
#pragma weak vr_stereo_xr_layer
#pragma weak vr_stereo_pred_ns
#pragma weak vr_stereo_disable
#pragma weak vr_stereo_route_copy
#pragma weak vr_stereo_set_route
#pragma weak vr_stereo_xr_frame
#pragma weak vr_stereo_cfg
#pragma weak vr_stereo_get_layout
#pragma weak vr_stereo_get_layout_intent
#pragma weak vs_vr_size
#pragma weak vs_px_per_deg
#pragma weak vs_follow_yaw
#include "xr_stereo_copy.h"
/* vr_aim.c (controller aim, regress/vr): right-hand aim pose in, reticle layer out. Weak as well. */
#define VR_AIM_XR
#include "vr_aim.h"
#pragma weak vr_aim_set_pose
#pragma weak vr_aim_stick_pitch_off
#pragma weak vr_aim_reticle_layer
#pragma weak xsc_init
#pragma weak xsc_frame
#pragma weak xsc_swapchain
#pragma weak xsc_drain
#pragma weak xsc_error
#pragma weak vr_stereo_set_native
#pragma weak vr_stereo_xr_mono_head
#pragma weak vr_stereo_xr_display_time
/* b78 autopilot (autopilot.c, lab runs with nobody wearing the headset): scripted head orientation + clean quit */
int autopilot_head(float q[4], int *gen); int autopilot_quit_requested(void);
#pragma weak autopilot_head
#pragma weak autopilot_quit_requested
static int g_ap_head_on;   /* set by the XR loop each frame */
static void ap_qmul(const float a[4], const float b[4], float o[4]){   /* (x,y,z,w) */
  float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
  float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
  o[0] = x; o[1] = y; o[2] = z; o[3] = w; }
static void ap_qrot(const float q[4], const float v[3], float o[3]){   /* o = q v q^-1 */
  float p[4] = { v[0], v[1], v[2], 0 }, c[4] = { -q[0], -q[1], -q[2], q[3] }, t[4], r[4];
  ap_qmul(q, p, t); ap_qmul(t, c, r); o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; }
static int stereo;      /* double-wide game surface shown through a projection layer (one view per half) */
static int g_native_xr; /* vr69: env.txt asks for the native renderer (BSI_RHI=b1n) */
static int copy_route;  /* route B: game -> AImageReader, XR thread copies into per-eye swapchains (xr_stereo_copy.c) */
static int noproj;      /* UI layout but the projection layer was refused: flat view over the eye area + UI quad */
static int ui_ok = 1;   /* the UI quad layer is accepted (dropped after an xrEndFrame error) */
static int has_alphablend;   /* XR_FB_composition_layer_alpha_blend (ui_blend=add) */
static int has_timespec;     /* vr70: XR_KHR_convert_timespec_time (latency breakdown in CLOCK_MONOTONIC) */
static int has_vismask;      /* g4 (b85): XR_KHR_visibility_mask (lens hidden-area mesh probe, vismask_probe) */
static EGLDisplay g_egl_dpy; static EGLContext g_egl_ctx; static EGLSurface g_egl_pb;   /* vr70: the XR thread's GLES context */
static XrSpace aim_space; static float g_head_yaw;
static struct { float yaw; int moving; double t; } ui_follow;
static XrSwapchain screen_sc; static ANativeWindow *screen_win; static int screen_w = 1920, screen_h = 1080;
static int g_vr_sized;   /* perf73: screen_w/h are the VR size (vs_vr_size), the game must use them (xr_stereo_game_size) */
static pthread_mutex_t xmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t xcv = PTHREAD_COND_INITIALIZER;
static int xr_state = 0;   /* 0 starting, 1 surface ready, -1 failed */
float g_xr_rumble_l, g_xr_rumble_r;

#define XF(name) static PFN_##name p_##name
XF(xrEnumerateInstanceExtensionProperties); XF(xrCreateInstance); XF(xrGetSystem); XF(xrCreateSession); XF(xrBeginSession); XF(xrEndSession);
XF(xrPollEvent); XF(xrWaitFrame); XF(xrBeginFrame); XF(xrEndFrame); XF(xrCreateReferenceSpace); XF(xrCreateActionSet); XF(xrCreateAction);
XF(xrStringToPath); XF(xrSuggestInteractionProfileBindings); XF(xrAttachSessionActionSets); XF(xrSyncActions); XF(xrGetActionStateFloat);
XF(xrGetActionStateBoolean); XF(xrGetActionStateVector2f); XF(xrApplyHapticFeedback); XF(xrGetOpenGLESGraphicsRequirementsKHR);
XF(xrCreateSwapchainAndroidSurfaceKHR); XF(xrInitializeLoaderKHR); XF(xrRequestExitSession); XF(xrLocateSpace); XF(xrCreateActionSpace); XF(xrPerfSettingsSetPerformanceLevelEXT); XF(xrEnumerateDisplayRefreshRatesFB); XF(xrRequestDisplayRefreshRateFB); XF(xrSetAndroidApplicationThreadKHR);
XF(xrLocateViews); XF(xrEnumerateViewConfigurationViews); XF(xrConvertTimeToTimespecTimeKHR); XF(xrGetVisibilityMaskKHR);
XF(xrCreateSwapchain); XF(xrEnumerateSwapchainFormats); XF(xrEnumerateSwapchainImages); XF(xrAcquireSwapchainImage); XF(xrWaitSwapchainImage); XF(xrReleaseSwapchainImage);
XF(xrGetCurrentInteractionProfile); XF(xrPathToString);   /* vr72: controller diagnostics */
static void loadfn(const char *n, PFN_xrVoidFunction *f){ if(gipa(inst, n, f) != XR_SUCCESS) *f = NULL; }
#define LF(name) loadfn(#name, (PFN_xrVoidFunction*)&p_##name)

/* ---- actions ---- */
enum { A_LSTICK, A_RSTICK, A_LTRIG, A_RTRIG, A_LGRIP, A_RGRIP, A_A, A_B, A_X, A_Y, A_LCLICK, A_RCLICK, A_MENU, A_HAPTIC, A_AIM, A_N };
static XrActionSet aset; static XrAction act[A_N]; static XrPath hand[2];

static float grip_on = 0.85f, grip_off = 0.6f;   /* vr72: right/left grip -> RB/LB with hysteresis (was > 0.5, no hysteresis) */
static XrPath P(const char *s){ XrPath p = 0; p_xrStringToPath(inst, s, &p); return p; }
static void mkaction(int i, const char *name, XrActionType t, int both){
  XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO }; ci.actionType = t;
  snprintf(ci.actionName, sizeof ci.actionName, "%s", name); snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", name);
  if(both){ ci.countSubactionPaths = 2; ci.subactionPaths = hand; }
  if(p_xrCreateAction(aset, &ci, &act[i]) != XR_SUCCESS) LOGX("action %s failed", name);
}
static void setup_actions(void){
  XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO }; strcpy(si.actionSetName, "game"); strcpy(si.localizedActionSetName, "Game");
  p_xrCreateActionSet(inst, &si, &aset);
  hand[0] = P("/user/hand/left"); hand[1] = P("/user/hand/right");
  mkaction(A_LSTICK, "lstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0); mkaction(A_RSTICK, "rstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
  mkaction(A_LTRIG, "ltrig", XR_ACTION_TYPE_FLOAT_INPUT, 0); mkaction(A_RTRIG, "rtrig", XR_ACTION_TYPE_FLOAT_INPUT, 0);
  mkaction(A_LGRIP, "lgrip", XR_ACTION_TYPE_FLOAT_INPUT, 0); mkaction(A_RGRIP, "rgrip", XR_ACTION_TYPE_FLOAT_INPUT, 0);
  mkaction(A_A, "btn_a", XR_ACTION_TYPE_BOOLEAN_INPUT, 0); mkaction(A_B, "btn_b", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
  mkaction(A_X, "btn_x", XR_ACTION_TYPE_BOOLEAN_INPUT, 0); mkaction(A_Y, "btn_y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
  mkaction(A_LCLICK, "lclick", XR_ACTION_TYPE_BOOLEAN_INPUT, 0); mkaction(A_RCLICK, "rclick", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
  mkaction(A_MENU, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0); mkaction(A_HAPTIC, "rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
  mkaction(A_AIM, "aim", XR_ACTION_TYPE_POSE_INPUT, 0);   /* controller aim (vr_aim.c) */
  struct { int a; const char *p; } b[] = {
    { A_LSTICK, "/user/hand/left/input/thumbstick" }, { A_RSTICK, "/user/hand/right/input/thumbstick" },
    { A_LTRIG, "/user/hand/left/input/trigger/value" }, { A_RTRIG, "/user/hand/right/input/trigger/value" },
    { A_LGRIP, "/user/hand/left/input/squeeze/value" }, { A_RGRIP, "/user/hand/right/input/squeeze/value" },
    { A_A, "/user/hand/right/input/a/click" }, { A_B, "/user/hand/right/input/b/click" },
    { A_X, "/user/hand/left/input/x/click" }, { A_Y, "/user/hand/left/input/y/click" },
    { A_LCLICK, "/user/hand/left/input/thumbstick/click" }, { A_RCLICK, "/user/hand/right/input/thumbstick/click" },
    { A_MENU, "/user/hand/left/input/menu/click" },
    { A_HAPTIC, "/user/hand/left/output/haptic" }, { A_HAPTIC, "/user/hand/right/output/haptic" },
    { A_AIM, "/user/hand/right/input/aim/pose" } };
  XrActionSuggestedBinding sb[16]; int n = sizeof b / sizeof b[0];
  for(int i = 0; i < n; i++){ sb[i].action = act[b[i].a]; sb[i].binding = P(b[i].p); }
  XrInteractionProfileSuggestedBinding pb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
  pb.interactionProfile = P("/interaction_profiles/oculus/touch_controller"); pb.countSuggestedBindings = n; pb.suggestedBindings = sb;
  XrResult r = p_xrSuggestInteractionProfileBindings(inst, &pb); LOGX("touch controller bindings: %d", r);
  XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO }; ai.countActionSets = 1; ai.actionSets = &aset;
  p_xrAttachSessionActionSets(sess, &ai);
  if(p_xrCreateActionSpace && act[A_AIM]){ XrActionSpaceCreateInfo asi = { XR_TYPE_ACTION_SPACE_CREATE_INFO }; asi.action = act[A_AIM];
    asi.poseInActionSpace.orientation.w = 1; XrResult ar = p_xrCreateActionSpace(sess, &asi, &aim_space); LOGX("right-hand aim pose space: %d", ar); }
  { char f[600]; snprintf(f, sizeof f, "%s/grip.txt", g_files_xr); FILE *g = fopen(f, "r");   /* vr72 */
    int gf = g != NULL; if(g){ float a, b; if(fscanf(g, "%f %f", &a, &b) == 2 && a > 0.05f && a <= 1.f && b >= 0.f && b < a){ grip_on = a; grip_off = b; } fclose(g); }
    LOGX("input: grip -> LB/RB pressed at >= %.2f, released at <= %.2f (%s); right grip = RB = the weapon wheel's hold button when the wheel is on",
         grip_on, grip_off, gf ? "grip.txt" : "default since build 72; build 71: > 0.5"); }
}
static float getf(int a){ XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = act[a]; XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
  return p_xrGetActionStateFloat(sess, &gi, &s) == XR_SUCCESS && s.isActive ? s.currentState : 0; }
static int getb(int a){ XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = act[a]; XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
  return p_xrGetActionStateBoolean(sess, &gi, &s) == XR_SUCCESS && s.isActive && s.currentState; }
static XrVector2f getv(int a){ XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = act[a]; XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
  XrVector2f z = { 0, 0 }; return p_xrGetActionStateVector2f(sess, &gi, &s) == XR_SUCCESS && s.isActive ? s.currentState : z; }
static int16_t ax(float v){ if(v > 1) v = 1; if(v < -1) v = -1; if(fabsf(v) < 0.1f) v = 0; return (int16_t)(v * 32767); }
/* vr72: input diagnostics ("i can't move ... my controller may have gone dead"). Per 10-s window: how often the actions
   were active (an action bound to a switched-off / sleeping / disconnected controller reports isActive = 0), how far the
   sticks / triggers / grips went, which buttons were seen, how long the right grip produced RB (the weapon wheel's hold
   button), XR frames without input focus (the pad is zeroed then: system menu, guardian, headset off the face). Logged in
   the [xr] status line; controller (in)activity and focus changes are logged when they happen. */
static struct { unsigned long polls, sync_fail, act_l, act_r, moved_l, moved_r, unfocused, frames, aim_n, aim_ok;
                float lmax, rmax, ltmax, rtmax, lgmax, rgmax; unsigned btn_or; double rb_t, rb_long, rb_since; int rb_on;
                int last_l, last_r; double l_off_t, r_off_t; int focused; } inp = { .last_l = -1, .last_r = -1, .focused = -1 };
static double now_mono(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static XrVector2f getv2(int a, int *active){ XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = act[a]; XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
  XrVector2f z = { 0, 0 }; int ok = p_xrGetActionStateVector2f(sess, &gi, &s) == XR_SUCCESS && s.isActive; if(active) *active = ok; return ok ? s.currentState : z; }
static void controller_profiles(char *b, size_t n){   /* vr72: current interaction profile per hand ("none" = no controller) */
  b[0] = 0; if(!p_xrGetCurrentInteractionProfile){ snprintf(b, n, "profiles unknown"); return; }
  for(int h = 0; h < 2; h++){ XrInteractionProfileState ps = { XR_TYPE_INTERACTION_PROFILE_STATE }; char nm[XR_MAX_PATH_LENGTH] = "none"; uint32_t ln = 0;
    XrResult r = p_xrGetCurrentInteractionProfile(sess, hand[h], &ps);
    if(r == XR_SUCCESS && ps.interactionProfile != XR_NULL_PATH && p_xrPathToString) p_xrPathToString(inst, ps.interactionProfile, sizeof nm, &ln, nm);
    else if(r != XR_SUCCESS) snprintf(nm, sizeof nm, "error %d", r);
    const char *t = strrchr(nm, '/'); size_t l = strlen(b); snprintf(b + l, n - l, "%s%s %s", h ? ", " : "", h ? "right" : "left", t ? t + 1 : nm); } }
static void poll_input(void){
  XrActiveActionSet as = { aset, XR_NULL_PATH }; XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &as;
  if(p_xrSyncActions(sess, &si) != XR_SUCCESS){ inp.sync_fail++; return; }
  AndroidPad p; memset(&p, 0, sizeof p);
  int al = 0, ar = 0; XrVector2f l = getv2(A_LSTICK, &al), r = getv2(A_RSTICK, &ar);
  { double t = now_mono(); inp.polls++; inp.act_l += al; inp.act_r += ar;   /* vr72 */
    for(int h = 0; h < 2; h++){ int a = h ? ar : al; int *last = h ? &inp.last_r : &inp.last_l; double *off = h ? &inp.r_off_t : &inp.l_off_t;
      if(a != *last){ if(*last >= 0 || !a){ if(!a){ *off = t; LOGX("input: %s controller inactive (switched off, asleep, out of tracking or not bound): its sticks/buttons read as released", h ? "right" : "left"); }
                                             else LOGX("input: %s controller active again (was inactive %.1f s)", h ? "right" : "left", *off > 0 ? t - *off : 0.0); }
                      *last = a; } }
    float ml = sqrtf(l.x * l.x + l.y * l.y), mr = sqrtf(r.x * r.x + r.y * r.y);
    if(ml > inp.lmax) inp.lmax = ml; if(mr > inp.rmax) inp.rmax = mr; if(ml > 0.2f) inp.moved_l++; if(mr > 0.2f) inp.moved_r++; }
  p.lx = ax(l.x); p.ly = ax(l.y); p.rx = ax(r.x); p.ry = ax(r.y);
  if(vr_aim_stick_pitch_off && vr_aim_stick_pitch_off()) p.ry = 0;   /* controller aim: pitch comes from head + controller */
  float ltv = getf(A_LTRIG), rtv = getf(A_RTRIG), lgv = getf(A_LGRIP), rgv = getf(A_RGRIP);
  p.lt = (uint8_t)(ltv * 255); p.rt = (uint8_t)(rtv * 255);
  /* vr72: grips are analog. Build <= 71 sent LB/RB while the grip value was > 0.5: a hand that merely holds the controller
     can rest above that, and with the weapon wheel on (hold RB 0.3 s = open; sticks drive the wheel; time pause) that
     reads as "can't move, the picture is frozen". Now: pressed above grip_on (0.85), released below grip_off (0.6);
     /sdcard/BSI/grip.txt "<on> <off>" overrides. */
  { static int lg, rg; lg = lg ? lgv > grip_off : lgv >= grip_on; rg = rg ? rgv > grip_off : rgv >= grip_on;
    if(lg) p.buttons |= 0x0100;  /* LB */
    if(rg) p.buttons |= 0x0200;  /* RB */
    double t = now_mono();
    if(rg && !inp.rb_on){ inp.rb_on = 1; inp.rb_since = t; }
    else if(!rg && inp.rb_on){ inp.rb_on = 0; double d = t - inp.rb_since; inp.rb_t += d; if(d > inp.rb_long) inp.rb_long = d; }
    if(ltv > inp.ltmax) inp.ltmax = ltv; if(rtv > inp.rtmax) inp.rtmax = rtv; if(lgv > inp.lgmax) inp.lgmax = lgv; if(rgv > inp.rgmax) inp.rgmax = rgv; }
  if(getb(A_A)) p.buttons |= 0x1000; if(getb(A_B)) p.buttons |= 0x2000; if(getb(A_X)) p.buttons |= 0x4000; if(getb(A_Y)) p.buttons |= 0x8000;
  if(getb(A_RCLICK)) p.buttons |= 0x0080;
  if(getb(A_MENU)) p.buttons |= 0x0010;          /* Start */
  /* Touch has no D-pad and no Back button, so the left stick click is a modifier (v0.8.4d):
       hold left stick click + push right stick = D-pad (down = voxophone/tear, up = nav arrow, left/right = quick toggle)
       hold left stick click + Y                = Back (map/objectives)
     The camera is frozen while it is held. A plain tap of the left stick click (no chord) is sent as LS click
     (sprint) for 120 ms after release, so sprint starts on release instead of on press. */
  { static int held, chord; static double pulse_until;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if(getb(A_LCLICK)){
      if(!held){ held = 1; chord = 0; }
      if(fabsf(r.x) > 0.5f || fabsf(r.y) > 0.5f){ chord = 1;
        if(fabsf(r.y) >= fabsf(r.x)) p.buttons |= r.y < 0 ? 0x0002 : 0x0001;   /* down : up */
        else                         p.buttons |= r.x < 0 ? 0x0004 : 0x0008; } /* left : right */
      if(p.buttons & 0x8000){ chord = 1; p.buttons = (p.buttons & ~0x8000) | 0x0020; }   /* Y -> Back */
      p.rx = p.ry = 0;
    } else if(held){ held = 0; if(!chord) pulse_until = now + 0.12; }
    if(now < pulse_until) p.buttons |= 0x0040; }
  inp.btn_or |= p.buttons;   /* vr72 */
  android_pad_set(&p);
  /* rumble */
  static float lastl = -1, lastr = -1;
  for(int h = 0; h < 2; h++){ float v = h ? g_xr_rumble_r : g_xr_rumble_l; float *last = h ? &lastr : &lastl; if(v == *last) continue; *last = v;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = act[A_HAPTIC]; hi.subactionPath = hand[h];
    XrHapticVibration hv = { XR_TYPE_HAPTIC_VIBRATION }; hv.amplitude = v; hv.duration = v > 0 ? 200000000 : XR_MIN_HAPTIC_DURATION; hv.frequency = XR_FREQUENCY_UNSPECIFIED;
    p_xrApplyHapticFeedback(sess, &hi, (XrHapticBaseHeader*)&hv); }
}

/* ---- screen placement (meters, LOCAL space = where the head was at start / last recenter) ---- */
static float scr_dist = 2.6f, scr_width = 3.2f, scr_y = 0.0f;
static void load_screen_cfg(const char *files){
  char f[600]; snprintf(f, sizeof f, "%s/screen.txt", files); FILE *s = fopen(f, "r");   /* "distance width height_offset" */
  if(s){ float a, b, c; int n = fscanf(s, "%f %f %f", &a, &b, &c); if(n >= 1 && a > 0.3f) scr_dist = a; if(n >= 2 && b > 0.3f) scr_width = b; if(n >= 3) scr_y = c; fclose(s); }
  snprintf(f, sizeof f, "%s/resolution.txt", files); s = fopen(f, "r");
  if(s){ int a, b; if(fscanf(s, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 200){ screen_w = a; screen_h = b; } fclose(s); }
}


/* CPU Boost while the game loads (Step B, ROADMAP item 12): boost_policy.h decides, with Meta's limits (45 s at a time,
   20 % of runtime); between boosts the CPU goes back to sustained-high.  /sdcard/BSI/no_boost.txt turns it off. */
#include "boost_policy.h"
#include "boost_burst.h"                     /* HITCHES.md H2: also boost during streaming hitch bursts (BSI_BOOST_BURST=1) */
extern volatile long g_br_presents;      /* bridge.c: game Present count */
extern volatile uint64_t g_s0_hint_ms;   /* sleep0.c: last async-read wait of the game thread */
static BoostPolicy g_bp; static int g_boost = -1;
static BurstBoost g_bb; static int g_bb_on = -1;
static void boost_frame(void){
  if(!has_perf || !p_xrPerfSettingsSetPerformanceLevelEXT) return;
  if(g_boost < 0){ char f[600]; snprintf(f, sizeof f, "%s/no_boost.txt", g_files_xr); g_boost = access(f, R_OK) != 0;
    LOGX("cpu boost during loading: %s", g_boost ? "on (no_boost.txt turns it off)" : "off (no_boost.txt)"); }
  if(!g_boost) return;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
  if(g_bb_on < 0){ const char *e = getenv("BSI_BOOST_BURST"); g_bb_on = e && *e == '1';
    LOGX("cpu boost during streaming bursts: %s", g_bb_on ? "on (BSI_BOOST_BURST=1)" : "off"); }
  int was_burst = g_bb.active;
  int ev = bb_tick(&g_bb, &g_bp, now, (uint32_t)g_br_presents, g_s0_hint_ms, g_bb_on);
  if(g_bb.active != was_burst) LOGX("streaming burst %s (#%u)", g_bb.active ? "start" : "end", g_bb.n_bursts);
  if(ev == BP_NONE) return;
  XrResult r = p_xrPerfSettingsSetPerformanceLevelEXT(sess, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, g_bp.on ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
  LOGX("cpu %s (%s): %d; boost #%u, total %.1f s of %.1f s runtime", g_bp.on ? "BOOST" : "sustained-high", bp_reason[ev], r, g_bp.n_boosts,
       g_bp.total_on / 1000.0, (now - g_bp.t0) / 1000.0);
}
/* stereo (vr_stereo.c): both eye views in LOCAL space for display time t, plus the head pose used for vr_head */
static void stereo_views(const XrPosef *head, XrTime t, const XrPosef *real){   /* b78: real != NULL = head is the autopilot's, eyes follow it */
  XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = t; li.space = local_space;
  XrViewState vs = { XR_TYPE_VIEW_STATE }; XrView xv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; uint32_t n = 0;
  int vok = p_xrLocateViews(sess, &li, &vs, 2, &n, xv) == XR_SUCCESS && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
  /* b78 autopilot: each eye relative to the real head (IPD, eye rotation, FOV) is cached and re-applied to the
     scripted head, so stereo stays correct when the desk headset's tracking is lost or the head path turns */
  static int ap_have; static XrPosef ap_rel[2]; static XrFovf ap_fov[2];
  if(vok && real){ float rc[4] = { -real->orientation.x, -real->orientation.y, -real->orientation.z, real->orientation.w };
    for(int e = 0; e < 2; e++){ float d[3] = { xv[e].pose.position.x - real->position.x, xv[e].pose.position.y - real->position.y, xv[e].pose.position.z - real->position.z }, dl[3], eq[4], rq[4];
      ap_qrot(rc, d, dl); eq[0] = xv[e].pose.orientation.x; eq[1] = xv[e].pose.orientation.y; eq[2] = xv[e].pose.orientation.z; eq[3] = xv[e].pose.orientation.w; ap_qmul(rc, eq, rq);
      ap_rel[e].position.x = dl[0]; ap_rel[e].position.y = dl[1]; ap_rel[e].position.z = dl[2];
      ap_rel[e].orientation.x = rq[0]; ap_rel[e].orientation.y = rq[1]; ap_rel[e].orientation.z = rq[2]; ap_rel[e].orientation.w = rq[3]; ap_fov[e] = xv[e].fov; }
    ap_have = 1; }
  if(g_ap_head_on){   /* scripted head this frame (real == NULL when the real head was not tracked) */
    if(!ap_have){ if(!vok) return;   /* nothing cached yet: no stereo views this frame */
      for(int e = 0; e < 2; e++){ ap_rel[e].orientation.x = ap_rel[e].orientation.y = ap_rel[e].orientation.z = 0; ap_rel[e].orientation.w = 1;
        ap_rel[e].position.x = (e ? 0.032f : -0.032f); ap_rel[e].position.y = ap_rel[e].position.z = 0; ap_fov[e] = xv[e].fov; } ap_have = 1; }
    float hq[4] = { head->orientation.x, head->orientation.y, head->orientation.z, head->orientation.w };
    for(int e = 0; e < 2; e++){ float d[3] = { ap_rel[e].position.x, ap_rel[e].position.y, ap_rel[e].position.z }, dw[3], rq[4] = { ap_rel[e].orientation.x, ap_rel[e].orientation.y, ap_rel[e].orientation.z, ap_rel[e].orientation.w }, eq[4];
      ap_qrot(hq, d, dw); ap_qmul(hq, rq, eq);
      xv[e].pose.position.x = head->position.x + dw[0]; xv[e].pose.position.y = head->position.y + dw[1]; xv[e].pose.position.z = head->position.z + dw[2];
      xv[e].pose.orientation.x = eq[0]; xv[e].pose.orientation.y = eq[1]; xv[e].pose.orientation.z = eq[2]; xv[e].pose.orientation.w = eq[3]; xv[e].fov = ap_fov[e]; }
    vok = 1; }
  if(!vok) return;
  VsViews v; memset(&v, 0, sizeof v);
#define VSPOSE(dst, src) do{ (dst).p[0] = (src).position.x; (dst).p[1] = (src).position.y; (dst).p[2] = (src).position.z; \
    (dst).q[0] = (src).orientation.x; (dst).q[1] = (src).orientation.y; (dst).q[2] = (src).orientation.z; (dst).q[3] = (src).orientation.w; }while(0)
  VSPOSE(v.head, *head);
  for(int e = 0; e < 2; e++){ VSPOSE(v.eye[e].pose, xv[e].pose);
    v.eye[e].fov[0] = xv[e].fov.angleLeft; v.eye[e].fov[1] = xv[e].fov.angleRight; v.eye[e].fov[2] = xv[e].fov.angleUp; v.eye[e].fov[3] = xv[e].fov.angleDown; }
#undef VSPOSE
  v.xr_time = t; v.valid = 1; vr_stereo_xr_set_views(&v);
  static int logged; if(!logged){ logged = 1;
    float tw = tanf(xv[0].fov.angleRight) - tanf(xv[0].fov.angleLeft), th = tanf(xv[0].fov.angleUp) - tanf(xv[0].fov.angleDown);
    VsLayout lay; memset(&lay, 0, sizeof lay); if(vr_stereo_get_layout_intent) vr_stereo_get_layout_intent(screen_w, screen_h, &lay);
    LOGX("stereo: back buffer %dx%d = eyes %dx%d + UI %dx%d at %d,%d", screen_w, screen_h, lay.eye_w, screen_h, lay.ui_w, lay.ui_h, lay.ui_x, lay.ui_y);
    LOGX("stereo: eye fov L/R/U/D %.1f/%.1f/%.1f/%.1f deg, IPD %.1f mm; square pixels need eye width:height = %.3f (e.g. resolution.txt %dx%d)",
         xv[0].fov.angleLeft * 57.2958f, xv[0].fov.angleRight * 57.2958f, xv[0].fov.angleUp * 57.2958f, xv[0].fov.angleDown * 57.2958f,
         1000.f * sqrtf((xv[1].pose.position.x - xv[0].pose.position.x) * (xv[1].pose.position.x - xv[0].pose.position.x) +
                        (xv[1].pose.position.y - xv[0].pose.position.y) * (xv[1].pose.position.y - xv[0].pose.position.y) +
                        (xv[1].pose.position.z - xv[0].pose.position.z) * (xv[1].pose.position.z - xv[0].pose.position.z)),
         tw / th, (lay.ui ? 3 : 2) * ((int)(1536 * tw / th + 0.5f) & ~1), 1536);
    { const VsCfg *sc = vr_stereo_cfg ? vr_stereo_cfg() : NULL; float pad = sc ? sc->fov_pad : 0.f;   /* perf73 */
      if(vs_px_per_deg) LOGX("stereo: centre sharpness %.1f px/deg across, %.1f px/deg up-down (eye %dx%d, pad %.0f deg; the Quest 3 panel shows ~25; "
                             "the runtime's recommended %s)", vs_px_per_deg(lay.eye_w, xv[0].fov.angleLeft, xv[0].fov.angleRight, pad),
                             vs_px_per_deg(screen_h, xv[0].fov.angleDown, xv[0].fov.angleUp, pad), lay.eye_w, screen_h, pad,
                             "image is ~13 at pad 0"); } }
}
/* vr71: eye views for a given XrTime, called from the game's rendering thread by the late head-pose latch when
   vr_stereo.txt late_pred_ms > 0 (vr_stereo.c). xrLocateViews has no externally synchronised parameter (OpenXR 1.0 §2.3:
   any thread may call it while the session exists; UE4's OpenXR plugin does the same from its render thread). */
int xr_locate_eyes(long long xr_time, VsViews *out){
  if(!sess || !local_space || !p_xrLocateViews || xr_time <= 0) return 0;
  XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = (XrTime)xr_time; li.space = local_space;
  XrViewState vs = { XR_TYPE_VIEW_STATE }; XrView xv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; uint32_t n = 0;
  if(p_xrLocateViews(sess, &li, &vs, 2, &n, xv) != XR_SUCCESS || n != 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return 0;
  memset(out, 0, sizeof *out);
  for(int e = 0; e < 2; e++){ out->eye[e].pose.p[0] = xv[e].pose.position.x; out->eye[e].pose.p[1] = xv[e].pose.position.y; out->eye[e].pose.p[2] = xv[e].pose.position.z;
    out->eye[e].pose.q[0] = xv[e].pose.orientation.x; out->eye[e].pose.q[1] = xv[e].pose.orientation.y; out->eye[e].pose.q[2] = xv[e].pose.orientation.z; out->eye[e].pose.q[3] = xv[e].pose.orientation.w;
    out->eye[e].fov[0] = xv[e].fov.angleLeft; out->eye[e].fov[1] = xv[e].fov.angleRight; out->eye[e].fov[2] = xv[e].fov.angleUp; out->eye[e].fov[3] = xv[e].fov.angleDown; }
  out->head.q[3] = 1; out->xr_time = xr_time; out->valid = 1;
  return 1;
}
/* performance: sustained-high CPU/GPU clocks, display refresh matched to the game, thread priorities */
#include <sys/syscall.h>
static void session_tuning(void){
  if(has_perf && p_xrPerfSettingsSetPerformanceLevelEXT){
    XrResult a = p_xrPerfSettingsSetPerformanceLevelEXT(sess, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    /* vr71: GPU sustained-high by default. Build <= 70 kept the GPU at sustained-low (from when the CPU was the bottleneck
       and the GPU had headroom); with the native renderer in stereo the GPU is the bottleneck (b70: 492 MHz, ~45-55 ms GPU
       per stereo frame). /sdcard/BSI/gpu_low.txt restores sustained-low; gpu_high.txt (the old opt-in) is still accepted
       and changes nothing. */
    char gf[600]; snprintf(gf, sizeof gf, "%s/gpu_low.txt", g_files_xr); int glow = access(gf, R_OK) == 0;
    char gh[600]; snprintf(gh, sizeof gh, "%s/gpu_high.txt", g_files_xr); int ghigh = access(gh, R_OK) == 0;
    XrResult b = p_xrPerfSettingsSetPerformanceLevelEXT(sess, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, glow ? XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    LOGX("perf levels: cpu sustained-high %d, gpu %s %d (%s%s)", a, glow ? "sustained-low" : "sustained-high", b,
         glow ? "gpu_low.txt" : "default since build 71", ghigh ? "; gpu_high.txt present: no effect any more" : "");
    if(g_bp.on){ g_bp.on = 0; LOGX("cpu boost reset by session (re)start"); } }
  if(has_refresh && p_xrEnumerateDisplayRefreshRatesFB && p_xrRequestDisplayRefreshRateFB){
    float rates[16]; uint32_t n = 0; p_xrEnumerateDisplayRefreshRatesFB(sess, 16, &n, rates);
    float want = 72;    /* b91: 72 Hz (was 120 since the 60-fps flat days): the compositor runs per refresh on our GPU; j0134 measured
                           -3 ms/frame at the Fair at 72 vs 120, and 36 fps = 72/2 is a phase-locked half rate. /sdcard/BSI/refresh.txt overrides (72/80/90/120) */
    char f[600]; snprintf(f, sizeof f, "%s/refresh.txt", g_files_xr); FILE *x = fopen(f, "r"); if(x){ float v; if(fscanf(x, "%f", &v) == 1) want = v; fclose(x); }
    float best = 0; for(uint32_t i = 0; i < n; i++){ LOGX("display rate available: %.0f", rates[i]); if(rates[i] <= want + 0.5f && rates[i] > best) best = rates[i]; }
    if(best > 0){ XrResult r = p_xrRequestDisplayRefreshRateFB(sess, best); LOGX("display refresh %.0f Hz: %d", best, r); } }
  if(has_threads && p_xrSetAndroidApplicationThreadKHR){
    int me = (int)syscall(SYS_gettid);
    XrResult a = p_xrSetAndroidApplicationThreadKHR(sess, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, me), b = -1;
    if(g_game_tid) b = p_xrSetAndroidApplicationThreadKHR(sess, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, g_game_tid);
    LOGX("thread hints: xr %d game %d", a, b); }
}
/* UI quad (regress/vr): placement (head-locked / world-locked / lazy follow), size from the UI rect, blend mode */
static void ui_place(XrCompositionLayerQuad *uq, const VsCfg *vc, const VsLayout *L, const void *chain, XrCompositionLayerAlphaBlendFB *ab, void *fs_){
  (void)fs_;
  float dist = vc ? vc->ui_dist : 1.8f, width = vc ? vc->ui_width : 1.6f, y = vc ? vc->ui_y : -0.1f;
  int lock = vc ? vc->ui_lock : 0, blend = vc ? vc->ui_blend : 0;
  int flipped = uq->pose.orientation.x != 0;   /* rotated-quad vertical flip (no image-layout extension) */
  uq->eyeVisibility = XR_EYE_VISIBILITY_BOTH; uq->size.width = width; uq->size.height = width * L->ui_h / (float)L->ui_w;
  float yaw = 0;
  if(lock == 0 && view_space){ uq->space = view_space; }
  else { uq->space = local_space;
    if(lock == 2){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double t = ts.tv_sec + ts.tv_nsec * 1e-9;
      float dt = ui_follow.t > 0 ? (float)(t - ui_follow.t) : 0.f; ui_follow.t = t; if(dt > 0.1f) dt = 0.1f;
      float th = (vc ? vc->ui_follow_deg : 30.f) * (float)M_PI / 180.f;
      if(vs_follow_yaw) ui_follow.yaw = vs_follow_yaw(ui_follow.yaw, g_head_yaw, th, dt, 2.0f, &ui_follow.moving); else ui_follow.yaw = g_head_yaw;
      yaw = ui_follow.yaw; } }
  uq->pose.position.x = -sinf(yaw) * dist; uq->pose.position.y = y; uq->pose.position.z = -cosf(yaw) * dist;
  float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
  if(flipped){ uq->pose.orientation.x = cy; uq->pose.orientation.y = 0; uq->pose.orientation.z = -sy; uq->pose.orientation.w = 0; }
  else { uq->pose.orientation.x = 0; uq->pose.orientation.y = sy; uq->pose.orientation.z = 0; uq->pose.orientation.w = cy; }
  uq->layerFlags = blend == 2 ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   /* premultiplied (vr_ui_rhi.c coverage alpha) */
  uq->next = chain;
  if(blend == 1 && has_alphablend){ ab->next = (void*)chain; ab->srcFactorColor = XR_BLEND_FACTOR_ONE_FB; ab->dstFactorColor = XR_BLEND_FACTOR_ONE_FB;
    ab->srcFactorAlpha = XR_BLEND_FACTOR_ZERO_FB; ab->dstFactorAlpha = XR_BLEND_FACTOR_ONE_FB; uq->next = ab; }
}
/* ---- vr69: head-turn diagnostics (build 67 report: "tearing / static" for a moment when the head turns fast) ----
   Per XR frame: head angular speed (from the predicted poses), whether a new game frame arrived, and - for flat frames with
   head-look - the angle between the head orientation the shown frame was rendered with and the head orientation at display
   time (what a head-locked screen shows as the world swimming with the head). Every 10 s with turns (else every 60 s):
     [xr] head turns: ...   and up to 8 single events per minute when that angle exceeds 6 deg. */
static void quat_rot(const float q[4], float *x, float *y, float *z){   /* v' = q v q* */
  float qx = q[0], qy = q[1], qz = q[2], qw = q[3], vx = *x, vy = *y, vz = *z;
  float tx = 2.f * (qy * vz - qz * vy), ty = 2.f * (qz * vx - qx * vz), tz = 2.f * (qx * vy - qy * vx);
  *x = vx + qw * tx + (qy * tz - qz * ty); *y = vy + qw * ty + (qz * tx - qx * tz); *z = vz + qw * tz + (qx * ty - qy * tx); }
static float quat_angle_deg(const float a[4], const float b[4]){
  float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); if(d > 1.f) d = 1.f;
  return 2.f * acosf(d) * 57.2958f; }
static struct { float q[4]; XrTime t; int have; long last_pr; int rep; double next_log, ev_window; int ev_n;
                unsigned long frames, fast, missed, noshow, reproj_frames, rep_hist[4], fast_rep_hist[4];
                double peak_w, err_sum_fast, err_max_fast, err_sum, age_sum; unsigned long err_n, err_n_fast; XrDuration period; } ht;
static void ht_frame(const XrFrameState *fs, const XrPosef *head, const float *qr, double age_ms){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
  if(!ht.next_log){ ht.next_log = now + 10.0; ht.ev_window = now + 60.0; }
  ht.frames++; if(!fs->shouldRender) ht.noshow++;
  if(ht.period && ht.t && fs->predictedDisplayTime - ht.t > ht.period * 3 / 2) ht.missed += (unsigned long)((fs->predictedDisplayTime - ht.t) / ht.period) - 1;
  ht.period = fs->predictedDisplayPeriod;
  long pr = (long)g_br_presents; if(pr != ht.last_pr){ ht.last_pr = pr; ht.rep = 1; } else ht.rep++;
  float w = 0;
  if(head){ float q[4] = { head->orientation.x, head->orientation.y, head->orientation.z, head->orientation.w };
    if(ht.have && fs->predictedDisplayTime > ht.t){ w = quat_angle_deg(ht.q, q) / ((fs->predictedDisplayTime - ht.t) * 1e-9f); if(w > ht.peak_w) ht.peak_w = w; }
    memcpy(ht.q, q, sizeof q); ht.have = 1;
    int r = ht.rep > 4 ? 3 : ht.rep - 1; ht.rep_hist[r]++;
    int fast = w > 100.f; if(fast){ ht.fast++; ht.fast_rep_hist[r]++; }
    if(qr){ float e = quat_angle_deg(qr, q); ht.err_sum += e; ht.err_n++; ht.age_sum += age_ms;
      if(fast){ ht.err_sum_fast += e; ht.err_n_fast++; if(e > ht.err_max_fast) ht.err_max_fast = e; }
      if(now > ht.ev_window){ ht.ev_window = now + 60.0; ht.ev_n = 0; }
      if(e > 6.f && ht.ev_n < 8){ ht.ev_n++;
        LOGX("head turn: %.0f deg/s, the shown frame is %.1f deg off the head (rendered %.0f ms before display, same game frame for %d XR frames)%s",
             w, e, age_ms, ht.rep, vr_head_reproject && vr_head_reproject() ? "; reprojected screen" : "; head-locked screen"); } } }
  ht.t = fs->predictedDisplayTime;
  if(now >= ht.next_log && (ht.fast || now >= ht.next_log + 50.0)){
    LOGX("head turns: %lu of %lu XR frames faster than 100 deg/s (peak %.0f deg/s); flat frame vs head: avg %.1f deg, during turns avg %.1f max %.1f deg, frame age avg %.0f ms; "
         "XR frames per game frame 1/2/3/4+: %lu/%lu/%lu/%lu (during turns %lu/%lu/%lu/%lu); missed XR frames %lu, not rendered %lu; screen %s",
         ht.fast, ht.frames, ht.peak_w, ht.err_n ? ht.err_sum / ht.err_n : 0.0, ht.err_n_fast ? ht.err_sum_fast / ht.err_n_fast : 0.0, ht.err_max_fast,
         ht.err_n ? ht.age_sum / ht.err_n : 0.0, ht.rep_hist[0], ht.rep_hist[1], ht.rep_hist[2], ht.rep_hist[3],
         ht.fast_rep_hist[0], ht.fast_rep_hist[1], ht.fast_rep_hist[2], ht.fast_rep_hist[3], ht.missed, ht.noshow,
         stereo ? "stereo projection" : ht.reproj_frames ? "reprojected (vr_head.txt reproject=1)" : "head-locked");
    ht.next_log = now + 10.0; ht.frames = ht.fast = ht.missed = ht.noshow = ht.reproj_frames = 0; ht.peak_w = ht.err_sum_fast = ht.err_max_fast = ht.err_sum = ht.age_sum = 0;
    ht.err_n = ht.err_n_fast = 0; memset(ht.rep_hist, 0, sizeof ht.rep_hist); memset(ht.fast_rep_hist, 0, sizeof ht.fast_rep_hist); }
}
/* ---- vr70: XrTime -> CLOCK_MONOTONIC seconds (XR_KHR_convert_timespec_time; else Meta's XrTime, which is CLOCK_MONOTONIC ns,
   accepted only when it is within 2 s of the monotonic clock). 0 = unknown. ---- */
static double xr_to_wall(XrTime t){
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); double n = now.tv_sec + now.tv_nsec * 1e-9;
  if(has_timespec && p_xrConvertTimeToTimespecTimeKHR){ struct timespec ts; if(p_xrConvertTimeToTimespecTimeKHR(inst, t, &ts) == XR_SUCCESS) return ts.tv_sec + ts.tv_nsec * 1e-9; }
  double d = t * 1e-9; return fabs(d - n) < 2.0 ? d : 0.0;
}
/* ---- vr70: stereo reprojection statistics. For every XR frame with the projection layer: the angle between the head
   orientation the shown frame was rendered with and the head at display time (= how far the compositor turns the picture;
   beyond the rendered fov (+ pad) that turn shows black at the leading edge), and how much later than its pose prediction
   the frame is shown. Every 10 s while the projection layer is up: [xr] stereo reprojection: ... ---- */
static struct { double next, sum, max, late_sum, late_max, fast_sum; unsigned long n, nfast, over_pad, nlate, over[5]; float lastq[4]; XrTime lastt; int have; } rp;
static const float rp_lim[5] = { 2.f, 3.f, 4.f, 6.f, 9.f };   /* perf73: frames beyond these angles (pad choice) */
static void rp_frame(const float *eq, const XrPosef *head, long long xr_time_shown, XrTime disp, float pad){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
  float hq[4] = { head->orientation.x, head->orientation.y, head->orientation.z, head->orientation.w };
  float w = 0; if(rp.have && disp > rp.lastt) w = quat_angle_deg(rp.lastq, hq) / ((disp - rp.lastt) * 1e-9f);
  memcpy(rp.lastq, hq, sizeof hq); rp.lastt = disp; rp.have = 1;
  if(eq){ float a = quat_angle_deg(eq, hq); rp.sum += a; rp.n++; if(a > rp.max) rp.max = a; if(w > 100.f){ rp.fast_sum += a; rp.nfast++; }
    if(a > pad) rp.over_pad++;
    for(int k = 0; k < 5; k++) if(a > rp_lim[k]) rp.over[k]++;
    if(xr_time_shown && disp > xr_time_shown){ double l = (disp - xr_time_shown) * 1e-6; rp.late_sum += l; rp.nlate++; if(l > rp.late_max) rp.late_max = l; } }
  if(!rp.next) rp.next = now + 10.0;
  if(now >= rp.next){ rp.next = now + 10.0;
    if(rp.n) LOGX("stereo reprojection: %lu frames, picture vs head avg %.1f max %.1f deg (during turns >100 deg/s avg %.1f deg, %lu frames); "
                  "%lu frames beyond pad=%.0f deg (black edge possible); shown %.0f ms (max %.0f) after the time its poses were predicted for; "
                  "frames beyond 2/3/4/6/9 deg: %lu/%lu/%lu/%lu/%lu",
                  rp.n, rp.sum / rp.n, rp.max, rp.nfast ? rp.fast_sum / rp.nfast : 0.0, rp.nfast, rp.over_pad, pad,
                  rp.nlate ? rp.late_sum / rp.nlate : 0.0, rp.late_max, rp.over[0], rp.over[1], rp.over[2], rp.over[3], rp.over[4]);
    rp.sum = rp.max = rp.late_sum = rp.late_max = rp.fast_sum = 0; rp.n = rp.nfast = rp.over_pad = rp.nlate = 0; memset(rp.over, 0, sizeof rp.over); }
}
/* ---- vr72: one compact status line every 10 s (and the first one after 3 s): which route and projection filter actually
   ran and whether the runtime accepted the layers, input focus, the controllers, what the sticks/buttons did, and the
   weapon wheel (weapon_wheel.c, weak). One build 72 log then answers "fuzzy / static / can't move" without guessing. ---- */
int ww_vr_status(char *b, int n) __attribute__((weak));   /* weapon_wheel.c (vr72): wheel state for the status line */
static void xr_status(XrResult er, int proj, int submitted, int route_b, int pf, unsigned long long pflags){
  static struct { unsigned long ok, bad, proj_ok, proj_bad, frames; double next; long pres0; XrResult last_bad; } xs;
  if(submitted){ if(er >= 0){ xs.ok++; if(proj) xs.proj_ok++; } else { xs.bad++; xs.last_bad = er; if(proj) xs.proj_bad++; } }
  xs.frames++;
  double t = now_mono(); if(!xs.next){ xs.next = t + 3.0; xs.pres0 = (long)g_br_presents; } if(t < xs.next) return;
  xs.next = t + 10.0;
  if(inp.rb_on){ double d = t - inp.rb_since; if(d > inp.rb_long) inp.rb_long = d; }
  char pb[200]; controller_profiles(pb, sizeof pb);
  char wb[300] = "wheel not linked"; if(ww_vr_status) ww_vr_status(wb, sizeof wb);
  double pp = inp.polls ? 100.0 / inp.polls : 0, fr = inp.frames ? 100.0 / inp.frames : 0;
  LOGX("status: %s, projection filter %s (flags 0x%llx), layers ok/refused %lu/%lu (projection %lu/%lu%s) | input focus %.0f%% of %lu XR frames | controllers: %s; "
       "actions active L %.0f%% R %.0f%%, aim pose valid %.0f%% | sticks max L %.2f R %.2f, moved L %.0f%% R %.0f%% | triggers max L %.2f R %.2f | grips max L %.2f R %.2f, "
       "RB from right grip %.1f s (longest %.1f s%s) | buttons seen 0x%04x | sync fails %lu | game frames +%ld | %s",
       !stereo ? "flat (no stereo)" : route_b ? "route B (copy)" : noproj ? "route A, projection refused (flat view)" : "route A (surface)",
       pf == 1 ? "auto" : pf == 2 ? "sharpen" : pf == 3 ? "sharpen_normal" : "none", pflags, xs.ok, xs.bad, xs.proj_ok, xs.proj_bad, "",
       inp.frames ? 100.0 - inp.unfocused * fr : 0.0, inp.frames, pb, inp.act_l * pp, inp.act_r * pp, inp.aim_n ? 100.0 * inp.aim_ok / inp.aim_n : 0.0,
       inp.lmax, inp.rmax, inp.moved_l * pp, inp.moved_r * pp, inp.ltmax, inp.rtmax, inp.lgmax, inp.rgmax, inp.rb_t + (inp.rb_on ? t - inp.rb_since : 0.0), inp.rb_long,
       inp.rb_on ? ", held now" : "", inp.btn_or, inp.sync_fail, (long)g_br_presents - xs.pres0, wb);
  if(xs.bad) LOGX("status: last refused xrEndFrame result %d", xs.last_bad);
  xs.ok = xs.bad = xs.proj_ok = xs.proj_bad = xs.frames = 0; xs.pres0 = (long)g_br_presents;
  inp.polls = inp.sync_fail = inp.act_l = inp.act_r = inp.moved_l = inp.moved_r = inp.unfocused = inp.frames = inp.aim_n = inp.aim_ok = 0;
  inp.lmax = inp.rmax = inp.ltmax = inp.rtmax = inp.lgmax = inp.rgmax = 0; inp.btn_or = 0; inp.rb_t = inp.rb_long = 0; if(inp.rb_on) inp.rb_since = t;
}
/* ---- XR thread ---- */
void tl_xr_end(void); /* bridge.c frame timeline */

/* g4 (build 85, chat 4): lens hidden-area probe. XR_KHR_visibility_mask gives, per eye, the mesh of pixels the lens never shows
   (HIDDEN_TRIANGLE_MESH, NDC [-1,1]). Logs the hidden fraction of each eye (sum of triangle areas / 4) = the ceiling of the lens-mask
   saving (G1), and writes the mesh to <data>/vismask_<eye>.txt ("v x y" lines, then "i a b c" lines) for the renderer-side mask later.
   Off with <data>/no_vismask.txt. Runs once after xrBeginSession (the mask can change: XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR is
   logged by the event loop if it ever comes). */
static void vismask_probe(void){
  if(!has_vismask || !p_xrGetVisibilityMaskKHR){ LOGX("vismask: XR_KHR_visibility_mask not available (ext %d fn %p)", has_vismask, (void*)p_xrGetVisibilityMaskKHR); return; }
  { char f[600]; snprintf(f, sizeof f, "%s/no_vismask.txt", g_files_xr); if(access(f, R_OK) == 0){ LOGX("vismask: probe off (no_vismask.txt)"); return; } }
  for(uint32_t eye = 0; eye < 2; eye++){
    XrVisibilityMaskKHR m = { XR_TYPE_VISIBILITY_MASK_KHR };
    XrResult r = p_xrGetVisibilityMaskKHR(sess, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
    if(r != XR_SUCCESS){ LOGX("vismask: eye %u: xrGetVisibilityMaskKHR (count) failed: %d", eye, r); continue; }
    uint32_t nv = m.vertexCountOutput, ni = m.indexCountOutput;
    if(!nv || !ni){ LOGX("vismask: eye %u: empty hidden mesh (%u vertices, %u indices): no lens mask on this headset/runtime", eye, nv, ni); continue; }
    XrVector2f *v = (XrVector2f*)calloc(nv, sizeof *v); uint32_t *ix = (uint32_t*)calloc(ni, sizeof *ix);
    if(!v || !ix){ free(v); free(ix); LOGX("vismask: eye %u: out of memory", eye); continue; }
    m.vertexCapacityInput = nv; m.vertices = v; m.indexCapacityInput = ni; m.indices = ix;
    r = p_xrGetVisibilityMaskKHR(sess, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
    if(r != XR_SUCCESS){ LOGX("vismask: eye %u: xrGetVisibilityMaskKHR (data) failed: %d", eye, r); free(v); free(ix); continue; }
    double area = 0; float minx = 9, maxx = -9, miny = 9, maxy = -9;
    for(uint32_t i = 0; i + 2 < ni; i += 3){ if(ix[i] >= nv || ix[i+1] >= nv || ix[i+2] >= nv) continue; XrVector2f a = v[ix[i]], b = v[ix[i+1]], c = v[ix[i+2]];
      area += fabs((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y)) * 0.5; }
    for(uint32_t i = 0; i < nv; i++){ if(v[i].x < minx) minx = v[i].x; if(v[i].x > maxx) maxx = v[i].x; if(v[i].y < miny) miny = v[i].y; if(v[i].y > maxy) maxy = v[i].y; }
    LOGX("vismask: eye %u: hidden mesh %u vertices, %u indices (%u triangles), hidden area %.2f %% of the eye (NDC bbox x %.3f..%.3f y %.3f..%.3f)",
         eye, nv, ni, ni / 3, 100.0 * area / 4.0, minx, maxx, miny, maxy);
    { char f[600]; snprintf(f, sizeof f, "%s/vismask_%u.txt", g_files_xr, eye); FILE *o = fopen(f, "w");
      if(o){ fprintf(o, "# XR_KHR_visibility_mask HIDDEN_TRIANGLE_MESH eye %u, NDC [-1,1]; hidden area %.4f %%\n", eye, 100.0 * area / 4.0);
        for(uint32_t i = 0; i < nv; i++) fprintf(o, "v %.6f %.6f\n", v[i].x, v[i].y);
        for(uint32_t i = 0; i + 2 < ni; i += 3) fprintf(o, "i %u %u %u\n", ix[i], ix[i+1], ix[i+2]);
        fclose(o); } }
    free(v); free(ix); }
}

static void *xr_thread(void *arg){
  JNIEnv *env = NULL; (*g_act_xr->vm)->AttachCurrentThread(g_act_xr->vm, &env, NULL);
  char lp[700]; Dl_info di; snprintf(lp, sizeof lp, "libopenxr_loader.so");
  if(dladdr((void*)xr_thread, &di) && di.dli_fname){ snprintf(lp, sizeof lp, "%s", di.dli_fname); char *sl = strrchr(lp, '/'); if(sl) strcpy(sl + 1, "libopenxr_loader.so"); }
  void *lib = dlopen(lp, RTLD_NOW);
  if(!lib || !(gipa = (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr"))){ LOGX("no OpenXR loader (%s)", dlerror()); goto fail; }
  inst = XR_NULL_HANDLE; LF(xrInitializeLoaderKHR); LF(xrEnumerateInstanceExtensionProperties); LF(xrCreateInstance);
  if(p_xrInitializeLoaderKHR){ XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR }; li.applicationVM = g_act_xr->vm; li.applicationContext = g_act_xr->clazz;
    XrResult r = p_xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR*)&li); LOGX("loader init: %d", r); }
  const char *exts[16] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME }; int next = 3;
  { uint32_t n = 0; static XrExtensionProperties ep[256]; for(int i = 0; i < 256; i++){ ep[i].type = XR_TYPE_EXTENSION_PROPERTIES; ep[i].next = NULL; }
    if(p_xrEnumerateInstanceExtensionProperties && p_xrEnumerateInstanceExtensionProperties(NULL, 256, &n, ep) == XR_SUCCESS)
      for(uint32_t i = 0; i < n; i++){ const char *e = ep[i].extensionName;
#define WANT(NAME, FLAG) if(!strcmp(e, NAME) && next < 16){ exts[next++] = NAME; FLAG = 1; }
        WANT(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME, has_layout)
        WANT(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, has_perf)
        WANT(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, has_refresh)
        WANT(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME, has_lsettings)
        WANT(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME, has_autofilter)
        WANT(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME, has_threads)
        WANT(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME, has_alphablend)
        WANT(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, has_timespec)
        WANT(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, has_vismask)
      } }
  LOGX("extensions: layout %d perf %d refresh %d layer-settings %d auto-filter %d threads %d alpha-blend %d timespec %d", has_layout, has_perf, has_refresh, has_lsettings, has_autofilter, has_threads, has_alphablend, has_timespec);
  { char f[600]; snprintf(f, sizeof f, "%s/noflip.txt", g_files_xr); flip = access(f, R_OK) != 0; snprintf(f, sizeof f, "%s/nofilter.txt", g_files_xr); nofilter = access(f, R_OK) == 0; }
  LOGX("vertical flip %s (%s)", flip ? "on" : "off", has_layout ? "layer flag" : "rotated quad");
  XrInstanceCreateInfoAndroidKHR ia = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR }; ia.applicationVM = g_act_xr->vm; ia.applicationActivity = g_act_xr->clazz;
  XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO }; ci.next = &ia; ci.enabledExtensionCount = next; ci.enabledExtensionNames = exts;
  strcpy(ci.applicationInfo.applicationName, "BSI-Quest"); ci.applicationInfo.applicationVersion = 4; strcpy(ci.applicationInfo.engineName, "BSI-Quest");
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
  XrResult r = p_xrCreateInstance(&ci, &inst); if(r != XR_SUCCESS){ LOGX("xrCreateInstance failed: %d", r); goto fail; }
  LF(xrGetSystem); LF(xrGetVisibilityMaskKHR); LF(xrCreateSession); LF(xrBeginSession); LF(xrEndSession); LF(xrPollEvent); LF(xrWaitFrame); LF(xrBeginFrame); LF(xrEndFrame);
  LF(xrCreateReferenceSpace); LF(xrCreateActionSet); LF(xrCreateAction); LF(xrStringToPath); LF(xrSuggestInteractionProfileBindings);
  LF(xrAttachSessionActionSets); LF(xrSyncActions); LF(xrGetActionStateFloat); LF(xrGetActionStateBoolean); LF(xrGetActionStateVector2f);
  LF(xrApplyHapticFeedback); LF(xrGetOpenGLESGraphicsRequirementsKHR); LF(xrCreateSwapchainAndroidSurfaceKHR); LF(xrRequestExitSession);
  LF(xrLocateSpace); LF(xrCreateActionSpace); LF(xrPerfSettingsSetPerformanceLevelEXT); LF(xrEnumerateDisplayRefreshRatesFB); LF(xrRequestDisplayRefreshRateFB); LF(xrSetAndroidApplicationThreadKHR);
  LF(xrLocateViews); LF(xrEnumerateViewConfigurationViews); if(has_timespec) LF(xrConvertTimeToTimespecTimeKHR);
  LF(xrCreateSwapchain); LF(xrEnumerateSwapchainFormats); LF(xrEnumerateSwapchainImages); LF(xrAcquireSwapchainImage); LF(xrWaitSwapchainImage); LF(xrReleaseSwapchainImage);
  LF(xrGetCurrentInteractionProfile); LF(xrPathToString);   /* vr72 */
  XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO }; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if((r = p_xrGetSystem(inst, &sg, &sysid)) != XR_SUCCESS){ LOGX("xrGetSystem failed: %d", r); goto fail; }
  /* minimal GLES context for the session's graphics binding */
  XrGraphicsRequirementsOpenGLESKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR }; if(p_xrGetOpenGLESGraphicsRequirementsKHR) p_xrGetOpenGLESGraphicsRequirementsKHR(inst, sysid, &gr);
  /* vr70: every EGL step is checked and logged (route B needs this context current on this thread for its copies); a failed
     pbuffer falls back to a surfaceless context (EGL_KHR_surfaceless_context) */
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint emaj = 0, emin = 0; EGLBoolean eok = eglInitialize(dpy, &emaj, &emin);
  EGLint ca[] = { EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
  EGLConfig cfg = NULL; EGLint nc = 0; EGLBoolean cok = eglChooseConfig(dpy, ca, &cfg, 1, &nc);
  if(!cok || nc < 1){ EGLint ca2[] = { EGL_RENDERABLE_TYPE, 0x40, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    LOGX("egl: no ES3 pbuffer config (0x%x), trying any ES3 config", eglGetError()); cok = eglChooseConfig(dpy, ca2, &cfg, 1, &nc); }
  EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE }; EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cx);
  EGLint ectx = eglGetError();
  EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE }; EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pa); EGLint epb = eglGetError();
  EGLBoolean mc = eglMakeCurrent(dpy, pb, pb, ctx); EGLint emc = eglGetError();
  if(!mc){ mc = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); emc = eglGetError(); if(mc) pb = EGL_NO_SURFACE; }
  LOGX("egl: init %d (EGL %d.%d), config %d (n %d), context %s (0x%x), pbuffer %s (0x%x), current %s%s (0x%x); exts: surfaceless %d native-client-buffer %d image-base %d fence-sync %d",
       eok, emaj, emin, cok, nc, ctx != EGL_NO_CONTEXT ? "ok" : "FAILED", ectx, pb != EGL_NO_SURFACE ? "ok" : "none", epb, mc ? "yes" : "NO",
       mc && pb == EGL_NO_SURFACE ? " (surfaceless)" : "", emc,
       strstr(eglQueryString(dpy, EGL_EXTENSIONS) ? eglQueryString(dpy, EGL_EXTENSIONS) : "", "EGL_KHR_surfaceless_context") != NULL,
       strstr(eglQueryString(dpy, EGL_EXTENSIONS) ? eglQueryString(dpy, EGL_EXTENSIONS) : "", "EGL_ANDROID_get_native_client_buffer") != NULL,
       strstr(eglQueryString(dpy, EGL_EXTENSIONS) ? eglQueryString(dpy, EGL_EXTENSIONS) : "", "EGL_KHR_image_base") != NULL,
       strstr(eglQueryString(dpy, EGL_EXTENSIONS) ? eglQueryString(dpy, EGL_EXTENSIONS) : "", "EGL_KHR_fence_sync") != NULL);
  XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR }; gb.display = dpy; gb.config = cfg; gb.context = ctx;
  XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = sysid;
  if((r = p_xrCreateSession(inst, &sci, &sess)) != XR_SUCCESS){ LOGX("xrCreateSession failed: %d", r); goto fail; }
  if(eglGetCurrentContext() != ctx){   /* the runtime may switch contexts inside xrCreateSession: ours must be current for route B */
    EGLBoolean m2 = eglMakeCurrent(dpy, pb, pb, ctx);
    LOGX("egl: context was not current after xrCreateSession (current %p): made current again: %d (0x%x)", (void*)eglGetCurrentContext(), m2, eglGetError()); }
  g_egl_dpy = dpy; g_egl_ctx = ctx; g_egl_pb = pb;
  XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1;
  p_xrCreateReferenceSpace(sess, &rs, &local_space);
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; p_xrCreateReferenceSpace(sess, &rs, &view_space);
  setup_actions();
  { /* vr69: the native renderer (env.txt BSI_RHI=b1n) makes route=auto pick route B. The game thread applies env.txt only
       after xr_window() returns, i.e. after this decision, so read the file here. */
    int native = 0; const char *e = getenv("BSI_RHI");
    if(e && strstr(e, "b1n")) native = 1;
    else { char f[600]; snprintf(f, sizeof f, "%s/env.txt", g_files_xr); FILE *ef = fopen(f, "r");
      if(ef){ char ln[512]; while(fgets(ln, sizeof ln, ef)){ char *k = ln; while(*k == ' ' || *k == '\t') k++;
                if(!strncmp(k, "BSI_RHI=", 8)) native = strstr(k + 8, "b1n") != NULL; }   /* last line wins, as in the loader */
              fclose(ef); } }
    if(vr_stereo_set_native) vr_stereo_set_native(native);
    g_native_xr = native;
    LOGX("renderer: %s", native ? "native (BSI_RHI=b1n)" : "D3D11 bridge"); }
  if(vr_stereo_layout && vr_stereo_layout()){
    int want_copy = vr_stereo_route_copy && vr_stereo_route_copy();   /* route B copies (and flips) itself */
    if(flip && !has_layout && !want_copy) vr_stereo_disable("no XR_FB_composition_layer_image_layout (needed for the vertical flip)");
    else if(!p_xrLocateViews) vr_stereo_disable("xrLocateViews missing");
    else stereo = 1;
    if(stereo){
      int rw = 0, rh = 0;
      if(p_xrEnumerateViewConfigurationViews){
        XrViewConfigurationView vv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } }; uint32_t nv = 0;
        XrResult vr = p_xrEnumerateViewConfigurationViews(inst, sysid, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vv);
        if(vr == XR_SUCCESS && nv >= 1){ rw = (int)vv[0].recommendedImageRectWidth; rh = (int)vv[0].recommendedImageRectHeight; }
        LOGX("stereo: %d views, recommended %ux%u per eye (max %ux%u)", vr == XR_SUCCESS ? (int)nv : -1,
             vv[0].recommendedImageRectWidth, vv[0].recommendedImageRectHeight, vv[0].maxImageRectWidth, vv[0].maxImageRectHeight); }
      /* perf73: the VR size does not come from resolution.txt (that is the FLAT size: a flat 1600x900 left there gave
         533x900 eyes in the first b72 run). vr_stereo.txt res=WxH, else res=auto (eye_mpix budget, recommended aspect). */
      const VsCfg *sc = vr_stereo_cfg ? vr_stereo_cfg() : NULL; int W = 0, H = 0;
      int how = sc && vs_vr_size ? vs_vr_size(sc, rw, rh, &W, &H) : 0;
      if(how){ int fw = screen_w, fh = screen_h; screen_w = W; screen_h = H; g_vr_sized = 1;
        VsLayout lay; memset(&lay, 0, sizeof lay); if(vr_stereo_get_layout_intent) vr_stereo_get_layout_intent(W, H, &lay);
        if(how == 1) LOGX("stereo: VR back buffer %dx%d (vr_stereo.txt res=%dx%d) = eyes %dx%d + UI %dx%d", W, H, W, H, lay.eye_w, H, lay.ui_w, lay.ui_h);
        else LOGX("stereo: VR back buffer %dx%d (res=auto: %.2f Mpix per eye shaped like the %s %dx%d) = eyes %dx%d + UI %dx%d", W, H,
                  sc->eye_mpix, how == 2 ? "runtime's recommended" : "Quest 3 default (no recommendation)", how == 2 ? rw : 925, how == 2 ? rh : 1000,
                  lay.eye_w, H, lay.ui_w, lay.ui_h);
        if(fw != W || fh != H) LOGX("stereo: resolution.txt / default %dx%d is the FLAT size and is not used in stereo", fw, fh); }
      else LOGX("stereo: VR size unavailable (vr_stereo.o too old?): game surface %dx%d from resolution.txt", screen_w, screen_h); } }
  /* stereo route B (route=copy in vr_stereo.txt): the game renders into an AImageReader, we copy into OpenXR swapchains.
     vr70: the decision and every init step are logged with their result (the build 69 Quest run fell back to route A
     without a trace in bsi.log). */
  { const VsCfg *rc = vr_stereo_cfg ? vr_stereo_cfg() : NULL; int want = stereo && vr_stereo_route_copy && vr_stereo_route_copy();
    LOGX("route decision: stereo %s, route=%s in vr_stereo.txt, renderer %s -> %s", stereo ? "on" : "off",
         !rc ? "?" : rc->copy > 0 ? "copy" : rc->copy == 0 ? "surface" : "auto", g_native_xr ? "native" : "bridge",
         !stereo ? "flat quad" : want ? "route B (copy) wanted" : "route A (surface)"); }
  if(stereo && vr_stereo_route_copy && vr_stereo_route_copy()){
    XscFns xf = { sess, p_xrCreateSwapchain, p_xrEnumerateSwapchainFormats, p_xrEnumerateSwapchainImages, p_xrAcquireSwapchainImage,
                  p_xrWaitSwapchainImage, p_xrReleaseSwapchainImage };
    ANativeWindow *cw = NULL; int xr_rc = -1;
    if(!xsc_init) LOGX("route B: xr_stereo_copy.o is not linked");
    else if(!xf.create || !xf.formats || !xf.images || !xf.acquire || !xf.wait || !xf.release)
      LOGX("route B: OpenXR swapchain functions missing (create %p formats %p images %p acquire %p wait %p release %p)", (void*)xf.create, (void*)xf.formats,
           (void*)xf.images, (void*)xf.acquire, (void*)xf.wait, (void*)xf.release);
    else {
      if(eglGetCurrentContext() != g_egl_ctx){ EGLBoolean m2 = eglMakeCurrent(g_egl_dpy, g_egl_pb, g_egl_pb, g_egl_ctx);
        LOGX("route B: XR thread context was not current: made current again: %d (0x%x)", m2, eglGetError()); }
      xr_rc = xsc_init(&xf, screen_w, screen_h, flip, &cw); }
    if(xr_rc == 0){ copy_route = 1; screen_win = cw; LOGX("stereo route B: game surface %dx%d is an AImageReader (%p)", screen_w, screen_h, (void*)cw); }
    else LOGX("stereo route B unavailable (%s): using route A (projection layer on the Android-surface swapchain)",
              xsc_init && xsc_error ? xsc_error() : "see the lines above");
  }
  if(stereo && vr_stereo_set_route) vr_stereo_set_route(copy_route);
  if(copy_route) goto screen_ready;
  /* the screen: an Android Surface owned by the compositor */
  jobject surf = NULL; r = XR_ERROR_FUNCTION_UNSUPPORTED;
  /* runtimes differ in which XrSwapchainCreateInfo fields they accept for Android surface swapchains: try the known variants */
  static const struct { int64_t fmt; XrSwapchainUsageFlags use; uint32_t one; } var[] = {
    { 0, 0, 0 }, { 0x8C43, 0, 0 }, { 0x8058, 0, 0 }, { 0, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 1 },
    { 0x8C43, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 1 }, { 0x8058, XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 1 }, { 0, 0, 1 } };
  if(!p_xrCreateSwapchainAndroidSurfaceKHR){ LOGX("xrCreateSwapchainAndroidSurfaceKHR missing"); goto fail; }
  for(unsigned k = 0; k < sizeof var / sizeof var[0]; k++){
    XrSwapchainCreateInfo sw = { XR_TYPE_SWAPCHAIN_CREATE_INFO }; sw.format = var[k].fmt; sw.usageFlags = var[k].use;
    sw.width = screen_w; sw.height = screen_h; sw.sampleCount = var[k].one; sw.faceCount = var[k].one; sw.arraySize = var[k].one; sw.mipCount = var[k].one;
    surf = NULL; r = p_xrCreateSwapchainAndroidSurfaceKHR(sess, &sw, &screen_sc, &surf);
    LOGX("surface swapchain variant %u (format 0x%llx usage 0x%llx counts %u): %d", k, (unsigned long long)sw.format, (unsigned long long)sw.usageFlags, var[k].one, r);
    if(r == XR_SUCCESS && surf) break;
  }
  if(r != XR_SUCCESS || !surf){ LOGX("android surface swapchain failed: %d", r); goto fail; }
  screen_win = ANativeWindow_fromSurface(env, surf);
  LOGX("screen surface %dx%d ready (%p)", screen_w, screen_h, (void*)screen_win);
screen_ready:
  pthread_mutex_lock(&xmu); xr_state = 1; pthread_cond_broadcast(&xcv); pthread_mutex_unlock(&xmu);

  /* frame loop */
  int running = 0; XrSessionState st = XR_SESSION_STATE_UNKNOWN;
  for(;;){
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while(p_xrPollEvent(inst, &ev) == XR_SUCCESS){
      if(ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){ st = ((XrEventDataSessionStateChanged*)&ev)->state; LOGX("session state %d", st);
        if(st == XR_SESSION_STATE_READY){ XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          r = p_xrBeginSession(sess, &bi); LOGX("begin session: %d", r); running = 1; session_tuning();
          { static int probed; if(!probed){ probed = 1; vismask_probe(); } }   /* g4 (b85) */
          /* vr69: the setup lines above are logged before bsi.log is open (logcat only); repeat the essentials here */
          LOGX("setup: renderer %s, stereo %s, route %s, image-layout flip %s, screen %dx%d, head-look %s%s", g_native_xr ? "native (b1n)" : "D3D11 bridge",
               stereo ? (noproj ? "on (projection refused: flat)" : "on") : "off", copy_route ? "copy (B, AImageReader)" : stereo ? "surface (A)" : "flat quad",
               flip ? (has_layout ? "layer flag" : "rotated quad") : "off", screen_w, screen_h, vr_head_enabled() ? "on" : "off",
               vr_head_reproject && vr_head_reproject() ? " (reproject=1)" : "");
          if(stereo && vr_stereo_cfg){ const VsCfg *c = vr_stereo_cfg();   /* vr70 */
            LOGX("setup: stereo options: pad %.0f deg, frames in flight %d%s, AO crop %d px, pred_ms %.0f, lag_ms %.0f, ui %s, route B %s, late latch %s (late_pred_ms %.0f, late_max %.0f deg)",
                 c->fov_pad, c->fif, c->fif ? "" : " (dxvk.conf)", c->ao_crop, c->pred_ms, c->lag_ms, c->ui ? "column" : "off",
                 copy_route ? "running" : xsc_error ? xsc_error() : "not linked", c->late ? "on" : "off", c->late_pred_ms, c->late_max_deg); } }
        else if(st == XR_SESSION_STATE_STOPPING){ p_xrEndSession(sess); running = 0; }
        else if(st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING){ LOGX("exit requested"); _exit(0); } }
      else if(ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) vr_head_recenter();   /* long-press Meta button = recenter */
      else if(ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED){ char pb[200]; controller_profiles(pb, sizeof pb);   /* vr72 */
        LOGX("input: controllers changed: %s (\"none\" = that controller is off, asleep or not connected)", pb); }
      else if(ev.type == XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT){   /* v0.8.8 (Fable #24): thermal / perf-level notifications were dropped; log them */
        const XrEventDataPerfSettingsEXT *p = (const XrEventDataPerfSettingsEXT*)&ev; struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
        static const char *dom[] = { "?", "cpu", "gpu" }, *sub[] = { "?", "compositing", "rendering", "thermal" }, *lvl[] = { "normal", "warning", "impaired" };
        LOGX("[thermal] t=%.0f s %s/%s: %s -> %s", tn.tv_sec + tn.tv_nsec * 1e-9, p->domain <= 2 ? dom[p->domain] : "?", p->subDomain <= 3 ? sub[p->subDomain] : "?",
             p->fromLevel == 0 ? lvl[0] : p->fromLevel == 25 ? lvl[1] : p->fromLevel == 75 ? lvl[2] : "?", p->toLevel == 0 ? lvl[0] : p->toLevel == 25 ? lvl[1] : p->toLevel == 75 ? lvl[2] : "?"); }
      ev.type = XR_TYPE_EVENT_DATA_BUFFER; ev.next = NULL;
    }
    early_log_flush();   /* vr70: once bsi.log is open, copy the early lines into it */
    if(autopilot_quit_requested && autopilot_quit_requested()){ static int asked;   /* b78 autopilot: clean quit -> STOPPING -> EXITING -> _exit(0) */
      if(!asked){ asked = 1; XrResult qr = running && p_xrRequestExitSession ? p_xrRequestExitSession(sess) : XR_ERROR_SESSION_NOT_RUNNING; LOGX("autopilot: exit session requested: %d", qr); } }
    if(!running){ if(copy_route && xsc_drain) xsc_drain(); usleep(10000); continue; }   /* vr69: a FIFO game must not block on the reader */
    XrFrameState fs = { XR_TYPE_FRAME_STATE }; XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    if(p_xrWaitFrame(sess, &wi, &fs) != XR_SUCCESS){ usleep(5000); continue; }
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO }; p_xrBeginFrame(sess, &bi);
    if(vr_stereo_xr_display_time) vr_stereo_xr_display_time(xr_to_wall(fs.predictedDisplayTime), (long long)fs.predictedDisplayTime);   /* vr70: latency breakdown */
    /* stereo: head (vr_head) and eye views (vr_stereo) for the same time, so the pose the game renders with and the pose
       the projection layer reports belong to one sample (pred_ms adds prediction for the game's longer latency) */
    XrTime tpose = fs.predictedDisplayTime + (stereo && vr_stereo_pred_ns ? (XrTime)vr_stereo_pred_ns() : 0);
    XrPosef headp; int have_head = 0;
    { XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };   /* head pose for vr_head.c */
      int lok = view_space && p_xrLocateSpace(view_space, local_space, tpose, &loc) == XR_SUCCESS && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
      /* b78 autopilot: scripted, level head orientation (position: last real one), also when tracking is lost on the desk */
      XrPosef realp = loc.pose; const XrPosef *realptr = NULL;
      { static XrVector3f ap_pos; static int ap_gen = -1; float aq[4]; int gen = 0;
        g_ap_head_on = autopilot_head && view_space && autopilot_head(aq, &gen);
        if(g_ap_head_on){ if(lok && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) ap_pos = loc.pose.position;
          if(lok) realptr = &realp;
          loc.pose.position = ap_pos; loc.pose.orientation.x = aq[0]; loc.pose.orientation.y = aq[1]; loc.pose.orientation.z = aq[2]; loc.pose.orientation.w = aq[3]; lok = 1; }
        if(autopilot_head && g_ap_head_on != ap_gen){ if(ap_gen >= 0) vr_head_recenter(); ap_gen = g_ap_head_on; } }   /* real <-> scripted: recenter */
      if(lok){
        headp = loc.pose; have_head = 1;
        { const XrQuaternionf *o = &loc.pose.orientation; g_head_yaw = atan2f(2.f * (o->w * o->y + o->x * o->z), 1.f - 2.f * (o->x * o->x + o->y * o->y)); }
        if(stereo) stereo_views(&loc.pose, tpose, realptr);
        vr_head_set_orientation(loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w); } }
    { int foc = st == XR_SESSION_STATE_FOCUSED; inp.frames++;   /* vr72: focus changes zero the pad: say so */
      if(foc != inp.focused){ if(inp.focused >= 0) LOGX("input: session %s (state %d)%s", foc ? "has input focus" : "lost input focus", st,
                                                                foc ? "" : ": the game gets a released pad until focus returns (system menu, guardian, headset off the face)");
                             inp.focused = foc; }
      if(!foc) inp.unfocused++; }
    if(st == XR_SESSION_STATE_FOCUSED) poll_input(); else { AndroidPad z; memset(&z, 0, sizeof z); android_pad_set(&z); }
    if(aim_space && have_head && vr_aim_set_pose){   /* right-hand aim pose (synced by poll_input) for the same display time */
      XrSpaceLocation al = { XR_TYPE_SPACE_LOCATION }; const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
      int ok = st == XR_SESSION_STATE_FOCUSED && p_xrLocateSpace(aim_space, local_space, tpose, &al) == XR_SUCCESS && (al.locationFlags & need) == need;
      float pa[3] = { al.pose.position.x, al.pose.position.y, al.pose.position.z }, qa[4] = { al.pose.orientation.x, al.pose.orientation.y, al.pose.orientation.z, al.pose.orientation.w };
      float ph[3] = { headp.position.x, headp.position.y, headp.position.z };
      inp.aim_n++; inp.aim_ok += ok;   /* vr72 */
      vr_aim_set_pose(pa, qa, ok, ph); }
    XrCompositionLayerQuad q = { XR_TYPE_COMPOSITION_LAYER_QUAD }; q.space = local_space; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = screen_sc; q.subImage.imageRect.extent.width = screen_w; q.subImage.imageRect.extent.height = screen_h;
    q.pose.orientation.w = 1; q.pose.position.z = -scr_dist; q.pose.position.y = scr_y;
    if(view_space && vr_head_enabled()){ q.space = view_space; q.pose.position.y = 0; }   /* head-look on: the screen follows the head */
    /* vr69: head-turn diagnostics and the reprojected flat screen (notes/VR_STEREO.md §17.4). The flat frame on screen was
       rendered with an older head orientation (qr) than the one the compositor shows it at: head-locked, the world in the
       picture lags the head by that difference during a turn and catches up in game-frame steps. reprojection=1 puts the
       screen where qr looks (LOCAL space), so the compositor's reprojection keeps the world in it steady. */
    int q_reproj = 0;
    { float qr[4]; double age = 0; int have_r = !stereo && have_head && vr_head_enabled() && vr_stereo_xr_mono_head && vr_stereo_xr_mono_head(qr, &age);
      ht_frame(&fs, have_head ? &headp : NULL, have_r ? qr : NULL, age);
      if(have_r && view_space && vr_head_reproject && vr_head_reproject()){
        float fx = 0, fy = 0, fz = -scr_dist; quat_rot(qr, &fx, &fy, &fz);
        q.space = local_space; q.pose.position.x = headp.position.x + fx; q.pose.position.y = headp.position.y + fy; q.pose.position.z = headp.position.z + fz;
        q.pose.orientation.x = qr[0]; q.pose.orientation.y = qr[1]; q.pose.orientation.z = qr[2]; q.pose.orientation.w = qr[3];
        if(flip && !has_layout){ float x = qr[0], y = qr[1], z = qr[2], w = qr[3];   /* qr * (180 deg about X): the flipped quad */
          q.pose.orientation.x = w; q.pose.orientation.y = z; q.pose.orientation.z = -y; q.pose.orientation.w = -x; }
        ht.reproj_frames++; q_reproj = 1; } }
    XrCompositionLayerImageLayoutFB il = { XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB }; il.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
    XrCompositionLayerSettingsFB ls = { XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB };
    ls.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB | (has_autofilter ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0);
    const void *chain = NULL;
    if(has_lsettings && !nofilter){ ls.next = chain; chain = &ls; }
    if(flip && has_layout){ il.next = chain; chain = &il; }
    q.next = chain;
    if(flip && !has_layout && !q_reproj){ q.pose.orientation.x = 1; q.pose.orientation.w = 0; }   /* 180 deg about X: shows the quad's back side = vertical flip only */
    /* vr72: the projection layer's compositor filter, vr_stereo.txt filter= (sharpen= is an alias), default none: the eye
       images (~6 px/deg) are magnified ~4x by the compositor onto the ~25 px/deg panel. Super-sampling is a minification
       filter (Meta: reduces flicker of layers *undersampled* by the compositor) - on a magnified layer it can only blur;
       sharpening (normal = temporal 3/5-tap, quality = Meta Quest Super Resolution) "counteracts blur when upsampling" but
       also amplifies aliasing and noise of a 960x1080 eye image into shimmer. Builds 70/71 sent super-sampling + automatic
       filter (the runtime decides) and build 71's sharpen=1 added quality sharpening; the build 71 VR run with sharpen=1
       was "fuzzy" and "static". none = no XrCompositionLayerSettingsFB on the projection layer (deterministic: what the
       runtime does by default). Quads (flat screen, UI, menus) keep their flags. A refused filter is dropped once (logged). */
    static int filter_ok[4] = { 1, 1, 1, 1 }, filter_logged = -1;
    XrCompositionLayerSettingsFB lsp = ls; XrCompositionLayerImageLayoutFB ilp = il; const void *pchain = NULL, *pnf = NULL;
    int pfilter = stereo && vr_stereo_cfg ? vr_stereo_cfg()->filter : 0; if(pfilter < 0 || pfilter > 3) pfilter = 0;
    int pf_eff = pfilter && has_lsettings && !nofilter && filter_ok[pfilter] ? pfilter : 0;
    lsp.next = NULL; lsp.layerFlags = 0;
    if(pf_eff == VS_FILTER_AUTO) lsp.layerFlags = ls.layerFlags;   /* builds 70/71: super-sampling (+ automatic filter) */
    else if(pf_eff == VS_FILTER_SHARPEN) lsp.layerFlags = ls.layerFlags | XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;   /* build 71 sharpen=1 */
    else if(pf_eff == VS_FILTER_SHARPEN_NORMAL) lsp.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB | (has_autofilter ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0);
    if(pf_eff){ pnf = &lsp; pchain = &lsp; }
    if(flip && has_layout){ ilp.next = (void*)pchain; pchain = &ilp; }
    int sharpen = pf_eff;   /* a refused projection layer drops the filter first */
    if(stereo && filter_logged != pf_eff){ filter_logged = pf_eff;
      static const char *fname[4] = { "none", "auto (super-sampling + automatic filter)", "sharpen (quality sharpening = Meta Quest Super Resolution + super-sampling + automatic filter)", "sharpen_normal (normal sharpening + automatic filter)" };
      LOGX("stereo: projection layer filter %s: layer settings flags 0x%llx%s%s", fname[pf_eff], (unsigned long long)lsp.layerFlags, pf_eff ? "" : " (no XrCompositionLayerSettingsFB on the projection layer)",
           pfilter != pf_eff ? (!has_lsettings || nofilter ? "; asked for a filter but layer settings are unavailable / nofilter.txt" : "; the asked filter was refused earlier") : ""); }
    q.size.width = scr_width; q.size.height = scr_width * screen_h / screen_w;
    const XrCompositionLayerBaseHeader *layers[4] = { (XrCompositionLayerBaseHeader*)&q };
    int nlayers = 1, proj = 0, uiq = 0;
    /* stereo frame on screen: one projection view per eye column of the surface, with the exact poses/fovs it was rendered
       with; plus the UI rect (regress/vr: [ L | R | UI ]) as its own quad over it. Route A uses sub-rects of the one image. */
    XrCompositionLayerProjectionView pv[2]; XrCompositionLayerProjection pl = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad uq = { XR_TYPE_COMPOSITION_LAYER_QUAD }; XrCompositionLayerAlphaBlendFB ab = { XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB };
    VsXrFrame fr; memset(&fr, 0, sizeof fr);
    const VsCfg *vc = stereo && vr_stereo_cfg ? vr_stereo_cfg() : NULL;
    if(stereo && !copy_route){
      if(vr_stereo_xr_frame) vr_stereo_xr_frame(screen_w, screen_h, &fr);
      else if(vr_stereo_xr_layer && vr_stereo_xr_layer(fr.eye)){ fr.kind = VS_KIND_STEREO; fr.mask = 3; fr.L.W = screen_w; fr.L.H = screen_h; fr.L.eye_w = screen_w / 2; }
      if(!fr.L.W){ fr.L.W = screen_w; fr.L.H = screen_h; fr.L.eye_w = screen_w / 2; }
      const VsLayout *L = &fr.L;
      if(fr.kind == VS_KIND_STEREO && !noproj){
        for(int e = 0; e < 2; e++){
          memset(&pv[e], 0, sizeof pv[e]); pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
          pv[e].pose.position.x = fr.eye[e].pose.p[0]; pv[e].pose.position.y = fr.eye[e].pose.p[1]; pv[e].pose.position.z = fr.eye[e].pose.p[2];
          pv[e].pose.orientation.x = fr.eye[e].pose.q[0]; pv[e].pose.orientation.y = fr.eye[e].pose.q[1]; pv[e].pose.orientation.z = fr.eye[e].pose.q[2]; pv[e].pose.orientation.w = fr.eye[e].pose.q[3];
          pv[e].fov.angleLeft = fr.eye[e].fov[0]; pv[e].fov.angleRight = fr.eye[e].fov[1]; pv[e].fov.angleUp = fr.eye[e].fov[2]; pv[e].fov.angleDown = fr.eye[e].fov[3];
          pv[e].subImage.swapchain = screen_sc; pv[e].subImage.imageArrayIndex = 0;
          pv[e].subImage.imageRect.offset.x = e * L->eye_w; pv[e].subImage.imageRect.offset.y = 0;
          pv[e].subImage.imageRect.extent.width = L->eye_w; pv[e].subImage.imageRect.extent.height = screen_h; }
        pl.space = local_space; pl.viewCount = 2; pl.views = pv; pl.next = pchain;   /* same flip / filter chain as the quad (vr71: + sharpening if asked) */
        layers[0] = (XrCompositionLayerBaseHeader*)&pl; proj = 1; }
      else if(fr.kind == VS_KIND_STEREO || fr.kind == VS_KIND_MONOWIDE){   /* flat view over the eye area */
        q.subImage.imageRect.extent.width = 2 * L->eye_w; q.size.height = scr_width * screen_h / (2.f * L->eye_w); }
      else if(fr.kind == VS_KIND_UIONLY && L->ui){   /* menus without a world: the UI rect is the screen (opaque) */
        q.subImage.imageRect.offset.x = L->ui_x; q.subImage.imageRect.offset.y = L->ui_y;
        q.subImage.imageRect.extent.width = L->ui_w; q.subImage.imageRect.extent.height = L->ui_h; q.size.height = scr_width * L->ui_h / L->ui_w; }
      else if(L->ui && vc && vc->mono_src == 1){   /* mono_src=ui: frames the game did not tag show the UI rect too */
        q.subImage.imageRect.offset.x = L->ui_x; q.subImage.imageRect.offset.y = L->ui_y;
        q.subImage.imageRect.extent.width = L->ui_w; q.subImage.imageRect.extent.height = L->ui_h; q.size.height = scr_width * L->ui_h / L->ui_w; }
      else if(L->ui && vc) q.size.height = scr_width / vc->mono_aspect;   /* whole buffer (Bink movies, loading): 16:9 */
      if(L->ui && ui_ok && (fr.kind == VS_KIND_STEREO || fr.kind == VS_KIND_MONOWIDE)){
        uq.subImage.swapchain = screen_sc; uq.subImage.imageRect.offset.x = L->ui_x; uq.subImage.imageRect.offset.y = L->ui_y;
        uq.subImage.imageRect.extent.width = L->ui_w; uq.subImage.imageRect.extent.height = L->ui_h;
        if(flip && !has_layout){ uq.pose.orientation.x = 1; uq.pose.orientation.w = 0; }
        ui_place(&uq, vc, L, chain, &ab, NULL);
        layers[nlayers++] = (XrCompositionLayerBaseHeader*)&uq; uiq = 1; }
    }
    const XscShown *sh_last = NULL;   /* vr70: for the reprojection statistics */
    if(copy_route){   /* route B: per-eye swapchains already hold the right images; no flip chain (the copy flips) */
      const XscShown *sh = xsc_frame(); sh_last = sh; const void *nf = (has_lsettings && !nofilter) ? (const void*)&ls : NULL;
      const VsLayout *L = &sh->L;
      q.pose.orientation.x = 0; q.pose.orientation.w = 1; q.next = nf;
      if(sh->stereo && sh->frame[0] && sh->frame[1]){
        for(int e = 0; e < 2; e++){ int w = 0, h = 0;
          memset(&pv[e], 0, sizeof pv[e]); pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
          pv[e].pose.position.x = sh->eye[e].pose.p[0]; pv[e].pose.position.y = sh->eye[e].pose.p[1]; pv[e].pose.position.z = sh->eye[e].pose.p[2];
          pv[e].pose.orientation.x = sh->eye[e].pose.q[0]; pv[e].pose.orientation.y = sh->eye[e].pose.q[1]; pv[e].pose.orientation.z = sh->eye[e].pose.q[2]; pv[e].pose.orientation.w = sh->eye[e].pose.q[3];
          pv[e].fov.angleLeft = sh->eye[e].fov[0]; pv[e].fov.angleRight = sh->eye[e].fov[1]; pv[e].fov.angleUp = sh->eye[e].fov[2]; pv[e].fov.angleDown = sh->eye[e].fov[3];
          pv[e].subImage.swapchain = xsc_swapchain(e, &w, &h); pv[e].subImage.imageRect.extent.width = w; pv[e].subImage.imageRect.extent.height = h; }
        pl.space = local_space; pl.viewCount = 2; pl.views = pv; pl.next = pnf; layers[0] = (XrCompositionLayerBaseHeader*)&pl; proj = 1; }
      else if((sh->kind == VS_KIND_UIONLY || (sh->kind == VS_KIND_NONE && vc && vc->mono_src == 1)) && L->ui && sh->frame[3]){ int w = 0, h = 0;
        q.subImage.swapchain = xsc_swapchain(3, &w, &h); q.subImage.imageRect.extent.width = w; q.subImage.imageRect.extent.height = h;
        q.size.height = scr_width * h / w; proj = 0; }
      else if(sh->frame[2]){ int w = 0, h = 0;
        q.subImage.swapchain = xsc_swapchain(2, &w, &h); q.subImage.imageRect.extent.width = w; q.subImage.imageRect.extent.height = h;
        q.size.height = sh->kind == VS_KIND_MONOWIDE ? scr_width * screen_h / (2.f * L->eye_w) : L->ui && vc ? scr_width / vc->mono_aspect : scr_width * h / w;
        proj = 0; }
      else nlayers = 0;   /* nothing rendered yet */
      if(nlayers && L->ui && ui_ok && sh->frame[3] && (sh->kind == VS_KIND_STEREO || sh->kind == VS_KIND_MONOWIDE)){ int w = 0, h = 0;
        uq.subImage.swapchain = xsc_swapchain(3, &w, &h); uq.subImage.imageRect.extent.width = w; uq.subImage.imageRect.extent.height = h;
        ui_place(&uq, vc, L, nf, &ab, NULL);
        layers[nlayers++] = (XrCompositionLayerBaseHeader*)&uq; uiq = 1; }
    }
    /* controller aim: a small reticle where the gun points (vr_aim.c) */
    XrCompositionLayerQuad rq;
    if(nlayers && vr_aim_reticle_layer && p_xrCreateSwapchain){
      VaXrFns af = { sess, local_space, p_xrCreateSwapchain, p_xrEnumerateSwapchainFormats, p_xrEnumerateSwapchainImages, p_xrAcquireSwapchainImage,
                     p_xrWaitSwapchainImage, p_xrReleaseSwapchainImage };
      if(af.formats && af.images && af.acquire && af.wait && af.release && vr_aim_reticle_layer(&af, &rq)) layers[nlayers++] = (XrCompositionLayerBaseHeader*)&rq; }
    /* vr70: how far the compositor has to turn the shown stereo frame (rendered head vs head at display) */
    if(have_head){ long long xt = 0; const float *eq = NULL; float eqv[4];
      if(proj && !copy_route){ xt = fr.xr_time; eqv[0] = fr.eye[0].pose.q[0]; eqv[1] = fr.eye[0].pose.q[1]; eqv[2] = fr.eye[0].pose.q[2]; eqv[3] = fr.eye[0].pose.q[3]; eq = eqv; }
      else if(proj && copy_route){ const XscShown *sh2 = sh_last; if(sh2){ xt = sh2->xr_time; memcpy(eqv, sh2->eye[0].pose.q, sizeof eqv); eq = eqv; } }
      rp_frame(eq, &headp, xt, fs.predictedDisplayTime, vc ? vc->fov_pad : 0.f); }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO }; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = fs.shouldRender ? nlayers : 0; ei.layers = layers;
    XrResult er = p_xrEndFrame(sess, &ei);
    xr_status(er, proj, fs.shouldRender && nlayers, copy_route, pf_eff, lsp.layerFlags);   /* vr72 */
    if(er < 0 && fs.shouldRender && nlayers){   /* the runtime refused a layer: drop the newest feature first, once each, and log it */
      static int fails; if(fails++ < 4) LOGX("xrEndFrame failed: %d (projection %d, UI quad %d, layers %d)", er, proj, uiq, nlayers);
      if(proj && sharpen){ filter_ok[sharpen] = 0; LOGX("stereo: xrEndFrame refused the projection layer with filter flags 0x%llx (%d): projection layer filter off", (unsigned long long)lsp.layerFlags, er); }
      else if(uiq){ ui_ok = 0; LOGX("stereo: UI quad layer rejected by the runtime: UI layer off (the UI column is not shown)"); }
      else if(proj && copy_route){ static int nb; if(nb++ < 3) LOGX("stereo route B: xrEndFrame refused the projection layer of the eye swapchains: %d (route=surface in vr_stereo.txt goes back to route A)", er); }
      else if(proj && !copy_route){
        LOGX("stereo: xrEndFrame with the projection layer failed: %d", er);
        if(fr.L.ui) noproj = 1; else stereo = 0;
        if(vr_stereo_disable) vr_stereo_disable("projection layer rejected by the runtime (see notes/VR_STEREO.md, route B)"); } }
    tl_xr_end();
    { static int hon = -1; static uint32_t hlast; static long hrun, hh[7], hx;   /* b126: refreshes per game frame (BSI_B1_XRHIST=1) */
      if(hon < 0){ const char *e = getenv("BSI_B1_XRHIST"); hon = e && atoi(e) > 0; if(hon) LOGX("hist: refreshes-per-game-frame histogram on (BSI_B1_XRHIST)"); }
      if(hon && fs.shouldRender){ uint32_t cur = (proj && sh_last) ? sh_last->frame[0] : 0;
        if(cur && cur == hlast) hrun++;
        else { if(hlast && hrun > 0) hh[hrun < 6 ? hrun : 6]++; hlast = cur; hrun = cur ? 1 : 0; }
        if(++hx % 1800 == 0){ long g = hh[1] + hh[2] + hh[3] + hh[4] + hh[5] + hh[6];
          LOGX("hist: refreshes per game frame over %ld game frames: 1:%ld 2:%ld 3:%ld 4:%ld 5:%ld 6+:%ld (36 fps at 72 Hz = all 2)", g, hh[1], hh[2], hh[3], hh[4], hh[5], hh[6]);
          for(int i = 0; i < 7; i++) hh[i] = 0; } } }
    boost_frame();
  }
fail:
  pthread_mutex_lock(&xmu); xr_state = -1; pthread_cond_broadcast(&xcv); pthread_mutex_unlock(&xmu);
  for(int i = 0; i < 600 && !early_log_flush(); i++) usleep(100000);   /* vr70: the game falls back to the panel window; keep the early lines */
  return NULL;
}

void xr_start(ANativeActivity *a, const char *files){
  g_act_xr = a; snprintf(g_files_xr, sizeof g_files_xr, "%s", files);
  { char nf[600]; snprintf(nf, sizeof nf, "%s/no_early_log.txt", files); if(access(nf, F_OK) != 0) early_log_open(files); }   /* vr70 */
  load_screen_cfg(files);
  pthread_t t; pthread_create(&t, NULL, xr_thread, NULL); pthread_detach(t);
}
/* the game's render target: the XR screen surface, or NULL if immersive mode failed (caller falls back to the panel window) */
/* perf73: android_glue.c, after xr_window(): 1 = stereo, the game must render at *w x *h (the VR surface size), whatever
   resolution.txt (flat) says */
int xr_stereo_game_size(int *w, int *h){ if(!g_vr_sized) return 0; *w = screen_w; *h = screen_h; return 1; }
ANativeWindow *xr_window(int *w, int *h){
  pthread_mutex_lock(&xmu); while(xr_state == 0) pthread_cond_wait(&xcv, &xmu); int ok = xr_state == 1; pthread_mutex_unlock(&xmu);
  if(!ok) return NULL; if(w) *w = screen_w; if(h) *h = screen_h; return screen_win;
}
