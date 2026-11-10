/* vr_aim.h: controller aim (linux/vr_aim.c). Pure math + the game-thread / XR-thread entry points.
   The reticle layer part needs the OpenXR headers: include <openxr/openxr.h> first and define VR_AIM_XR. */
#pragma once
#include <stdint.h>

typedef struct { int on; float conv; int stickpitch; float reticle_deg, reticle_dist; int log; } VaCfg;

/* pure math (host-tested in regress/vr/vr_math_test.c) */
void va_parse_opts(VaCfg *c, const char *s);
void va_aim_dir(const float pa[3], const float qa[4], const float ph[3], float conv, double d[3]);   /* LOCAL, unit */
void va_dir_angles(const double d[3], double *yaw, double *pitch);    /* OpenXR: yaw about +Y (left +), pitch up + */
void va_rot_matrix(const int32_t r[3], double M[9]);                  /* UE3 FRotationMatrix (rows X fwd, Y right, Z up) */
void va_view_q(const int32_t A[3], const int32_t H[3], double Q[9]);  /* view-space rotation: camera A's view -> camera H's */
void va_quat_yaw_pitch(double yaw, double pitch, float q[4]);         /* OpenXR quaternion: -Z forward turned to (yaw, pitch) */

/* XR thread (xr_glue.c): right-hand aim pose and head pose, LOCAL space, for the display time vr_head gets */
void vr_aim_set_pose(const float pa[3], const float qa[4], int valid, const float ph[3]);
int  vr_aim_stick_pitch_off(void);     /* aim mode: zero the right stick's Y (pitch comes from head + controller) */
/* game thread (vr_head.c, inside FillCameraCache): base = the game's camera rotation, r = base + head offsets (in/out:
   becomes base + aim offsets when aim mode is on), yaw0 = vr_head's recenter yaw (rad), w = vr_head's scripted-look fade */
int  vr_aim_camera(const int32_t base[3], int32_t r[3], float yaw0, float w);
int  vr_aim_active(void);
void vr_aim_reset(void);

#ifdef VR_AIM_XR
typedef struct {
  XrSession sess; XrSpace local;
  PFN_xrCreateSwapchain create; PFN_xrEnumerateSwapchainFormats formats; PFN_xrEnumerateSwapchainImages images;
  PFN_xrAcquireSwapchainImage acquire; PFN_xrWaitSwapchainImage wait; PFN_xrReleaseSwapchainImage release;
} VaXrFns;
/* XR thread, once per frame: a small quad layer along the gun's aim (0 = nothing to show). GLES context must be current. */
int vr_aim_reticle_layer(const VaXrFns *f, XrCompositionLayerQuad *q);
#endif
