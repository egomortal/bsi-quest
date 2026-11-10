/* vr_head (VR phase 5, step 1 prototype): head-look.
   The HMD orientation (from xr_glue, once per XR frame) is added to the game's final camera rotation inside
   ACamera::FillCameraCache (f_005d6420, wrapped in the hooked part005 object; see notes/ROADMAP.md item 26).
   - Off unless /sdcard/BSI/vr_head.txt exists (checked every ~2 s, so it can be toggled while playing).
     Optional contents, any order:  yaw=1 pitch=1 roll=0 scale=1.0 recenter=1 reproject=0
     reproject=1 (vr69): xr_glue world-locks the flat screen to the head orientation each frame was rendered with (instead of
     locking it to the head), so the compositor's reprojection keeps the picture steady during fast head turns; the edge of
     the screen can then show for a moment while the game catches up. Off by default (see notes/VR_STEREO.md §17.4).
     fif=N (vr70): frames in flight while head-look is on in FLAT mode (stereo has its own fif= in vr_stereo.txt). 0 (default)
     = dxvk.conf's value (3 on the Quest), exactly build 69. 1 or 2 shorten the time between the head sample and the display
     (the build 69 head-turn log measured 59-87 ms frame age at 31 ms frames): fif=1 about -1 to -2 frames of latency for a
     few % fps when the GPU and the render thread take turns (see vr70/README.md).
   - Only the camera rotation changes; Controller.Rotation (movement / body) is untouched. Weapons should follow the
     head, because UE3's GetPlayerViewPoint reads CameraCache.POV.
   - Idempotent: FillCameraCache rewrites POV.Rotation from NewPOV on every call (2 per frame); we add on top.
   - Yaw is relative to the recenter yaw (first pose after enabling, or vr_head_recenter()).
   Coordinate mapping: OpenXR is right-handed, +Y up, -Z forward; UE3 FRotator is int32 with 65536 = 360 deg,
   Pitch up = +, Yaw right = +, Roll right = +. So UE yaw = -XR yaw, UE pitch = +XR pitch, UE roll = -XR roll. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
extern uint8_t *M;
/* vr_aim.c (controller aim, regress/vr): may turn the camera (= the gun) to the right controller after the head is added */
int  vr_aim_camera(const int32_t base[3], int32_t r[3], float yaw0, float w) __attribute__((weak));
void vr_aim_reset(void) __attribute__((weak));

#define CAM_ROT 0x3C4   /* CameraCache.POV.Rotation: Pitch, Yaw, Roll (int32 each) */
#define CAM_PCOWNER 0x1FC   /* ACamera.PCOwner (SDK Engine_classes.h) */
#define PC_CINEMATIC 0x248  /* APlayerController.bCinematicMode: dword bit 0x00020000 */
#define PC_IGNORELOOK 0x26E /* APlayerController.bIgnoreLookInput: byte counter, >0 while the game owns the view */

static volatile uint32_t g_seq;                  /* seqlock: odd while the writer is updating */
static volatile float g_yaw, g_pitch, g_roll;    /* radians, XR frame */
static volatile int g_have_pose;
static volatile int g_recenter = 1;
static float g_yaw0;
static int g_on = -1, g_use_yaw = 1, g_use_pitch = 1, g_use_roll = 0, g_reproj = 0, g_fif = 0;
/* vr69: the LOCAL orientation the camera got in the last vr_head_apply (vr_head_rendered), for the flat-screen pose */
static float g_rq[4]; static double g_rq_t = -10; static int g_rq_ok;
static float g_scale = 1.0f;
static double g_next_check;
static unsigned long g_applied;
/* Scripted look (v0.8.7, user report: the box handed over on the boat, reading a telegram): while the game ignores look input or is in
   cinematic mode, it is aiming the camera itself, so the head offset fades out (0.35 s) and the head-locked screen shows exactly the
   game's framing. When control returns, forward is recentred on the current head yaw and the offset fades back in: no snap. */
static float g_w = 1.f; static int g_lock = -1; static double g_last_t;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void read_cfg(void){
  FILE *f = fopen("/sdcard/BSI/vr_head.txt", "r");
  int on = f != NULL;
  if(on && g_on != 1) g_recenter = 1;            /* re-enabled: forward = where you look now */
  g_on = on;
  if(!f) return;
  g_reproj = 0; g_fif = 0;   /* (re-read: the option must be in the file) */
  char w[64];
  while(fscanf(f, "%63s", w) == 1){
    char *eq = strchr(w, '='); if(!eq) continue; *eq = 0; const char *v = eq + 1;
    if(!strcmp(w, "yaw")) g_use_yaw = atoi(v);
    else if(!strcmp(w, "pitch")) g_use_pitch = atoi(v);
    else if(!strcmp(w, "roll")) g_use_roll = atoi(v);
    else if(!strcmp(w, "scale")){ float s = strtof(v, NULL); if(s > 0 && s <= 4) g_scale = s; }
    else if(!strcmp(w, "recenter") && atoi(v)) g_recenter = 1;
    else if(!strcmp(w, "reproject")) g_reproj = atoi(v) != 0;
    else if(!strcmp(w, "fif")){ int n = atoi(v); if(n >= 0 && n <= 8) g_fif = n; }   /* vr70 */
  }
  fclose(f);
}

/* Called by xr_glue after xrLocateSpace(VIEW in LOCAL) each frame. */
void vr_head_set_orientation(float qx, float qy, float qz, float qw){
  /* YXZ (yaw, then pitch, then roll) decomposition for a +Y-up frame */
  float yaw   = atan2f(2.f * (qw * qy + qx * qz), 1.f - 2.f * (qx * qx + qy * qy));
  float sp    = 2.f * (qw * qx - qy * qz); if(sp > 1.f) sp = 1.f; if(sp < -1.f) sp = -1.f;
  float pitch = asinf(sp);
  float roll  = atan2f(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qx * qx + qz * qz));
  g_seq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  g_yaw = yaw; g_pitch = pitch; g_roll = roll;
  __atomic_thread_fence(__ATOMIC_RELEASE); g_seq++;
  g_have_pose = 1;
}
void vr_head_recenter(void){ g_recenter = 1; }
int vr_head_enabled(void){ return g_on == 1; }  /* xr_glue: head-lock the screen quad while head-look is on */
int vr_head_reproject(void){ return g_on == 1 && g_reproj; }   /* xr_glue: world-lock the flat screen to the rendered pose */
int vr_head_frames_in_flight(void){ return g_on == 1 ? g_fif : 0; }   /* vr70, rhi_b1 present (flat mode): 0 = keep dxvk.conf */
/* vr69, game thread (vr_stereo.c, at CalcSceneView from Draw): the camera orientation of this frame in LOCAL space, built
   like the camera got it (yaw0 + scaled yaw offset, scaled pitch, roll if roll=1). 0 = head-look did not drive this frame. */
int vr_head_rendered(float q[4]){
  if(!g_rq_ok || now_s() - g_rq_t > 0.25) return 0;
  memcpy(q, g_rq, sizeof g_rq); return 1; }
static void rq_set(float yaw, float pitch, float roll){   /* q = Ry(yaw) Rx(pitch) Rz(roll), the inverse of the YXZ split above */
  float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f), cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
  float ax = cy * sp, ay = sy * cp, az = -sy * sp, aw = cy * cp;              /* Ry * Rx */
  g_rq[0] = ax * cr + ay * sr; g_rq[1] = ay * cr - ax * sr; g_rq[2] = aw * sr + az * cr; g_rq[3] = aw * cr - az * sr;   /* (Ry Rx) * Rz */
  g_rq_t = now_s(); g_rq_ok = 1; }

static int32_t rad2ue(float r){ return (int32_t)lrintf(r * (32768.f / (float)M_PI)); }

void vr_head_apply(uint32_t cam){
  double t = now_s();
  if(t >= g_next_check){ g_next_check = t + 2.0; read_cfg(); }
  if(g_on != 1 || !g_have_pose){ g_rq_ok = 0; if(&vr_aim_reset) vr_aim_reset(); return; }
  float yaw, pitch, roll; uint32_t s0, s1; int tries = 0;
  do { s0 = g_seq; __atomic_thread_fence(__ATOMIC_ACQUIRE); yaw = g_yaw; pitch = g_pitch; roll = g_roll;
       __atomic_thread_fence(__ATOMIC_ACQUIRE); s1 = g_seq; } while((s0 != s1 || (s0 & 1)) && ++tries < 8);
  { int lock = 0, il = 0, cin = 0; uint32_t pc; memcpy(&pc, M + cam + CAM_PCOWNER, 4);
    if(pc >= 0x10000u && pc < 0xC0000000u){ uint32_t f; memcpy(&f, M + pc + PC_CINEMATIC, 4); cin = (f & 0x20000u) != 0; il = M[pc + PC_IGNORELOOK]; lock = cin || il; }
    if(lock != g_lock){ if(g_lock == 1 && !lock) g_recenter = 1;
      fprintf(stderr, "[vr_head] scripted look %s (ignoreLook=%d cinematic=%d)\n", lock ? "ON: head offset fades out" : "off: recentred", il, cin); g_lock = lock; }
    float dt = g_last_t > 0 ? (float)(t - g_last_t) : 0.f; g_last_t = t; if(dt > 0.1f) dt = 0.1f;
    g_w += (lock ? -dt : dt) / 0.35f; if(g_w < 0.f) g_w = 0.f; if(g_w > 1.f) g_w = 1.f;
    if(g_recenter && !lock){ g_yaw0 = yaw; g_recenter = 0; }
    if(g_w <= 0.f){ rq_set(g_yaw0, 0.f, 0.f); if(&vr_aim_reset) vr_aim_reset(); return; } }
  float dy = yaw - g_yaw0; if(dy > (float)M_PI) dy -= 2.f * (float)M_PI; if(dy < -(float)M_PI) dy += 2.f * (float)M_PI;
  int32_t r[3]; memcpy(r, M + cam + CAM_ROT, 12);
  int32_t base[3]; memcpy(base, r, 12);
  if(g_use_pitch){
    int32_t p = (int16_t)(uint16_t)(r[0] + rad2ue(pitch * g_scale * g_w));   /* normalise to -32768..32767 */
    if(p > 16000) p = 16000; if(p < -16000) p = -16000;                /* stay short of +-90 deg (16384) */
    r[0] = p;
  }
  if(g_use_yaw) r[1] += rad2ue(-dy * g_scale * g_w);
  if(g_use_roll) r[2] += rad2ue(-roll * g_scale * g_w);
  rq_set(g_yaw0 + (g_use_yaw ? dy * g_scale * g_w : 0.f), g_use_pitch ? pitch * g_scale * g_w : 0.f, g_use_roll ? roll * g_scale * g_w : 0.f);
  if(&vr_aim_camera) vr_aim_camera(base, r, g_yaw0, g_w);   /* controller aim: camera = aim, the view fix keeps the picture on the head */
  memcpy(M + cam + CAM_ROT, r, 12);
  if((++g_applied & 0xFFF) == 1)
    fprintf(stderr, "[vr_head] applied %lu  head yaw %.1f pitch %.1f roll %.1f deg\n", g_applied, dy * 57.2958f, pitch * 57.2958f, roll * 57.2958f);
}
