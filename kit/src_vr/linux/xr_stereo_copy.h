/* xr_stereo_copy.h: route B of the VR stereo prototype (linux/xr_stereo_copy.c), used by xr_glue.c (regress/vr_stereo_xr_glue.patch).
   Include after <openxr/openxr.h> and "vr_stereo.h". */
#pragma once
typedef struct {
  XrSession sess;
  PFN_xrCreateSwapchain create; PFN_xrEnumerateSwapchainFormats formats; PFN_xrEnumerateSwapchainImages images;
  PFN_xrAcquireSwapchainImage acquire; PFN_xrWaitSwapchainImage wait; PFN_xrReleaseSwapchainImage release;
} XscFns;
typedef struct {
  uint32_t frame[4];   /* game frame (Present #) currently in the left / right / mono / UI swapchain, 0 = none yet */
  VsEye eye[2];        /* pose + fov each eye's image was rendered with */
  int stereo;          /* newest finished frame was a stereo frame (show the projection layer), else the mono quad */
  uint32_t count;      /* game frames received so far (compare with the Present count) */
  int kind;            /* VS_KIND_* of the newest finished frame (VS_KIND_NONE = whole buffer in the mono swapchain) */
  VsLayout L;          /* back-buffer layout (eye width, UI rect) */
  long long xr_time;   /* vr70: display time the shown stereo frame's poses were predicted for (0 = unknown) */
} XscShown;
/* XR thread, GLES context current; w x h = the game's double-wide surface. Returns 0 and the window the game renders into. */
int xsc_init(const XscFns *f, int w, int h, int flip, ANativeWindow **win);
const XscShown *xsc_frame(void);                 /* once per XR frame before xrEndFrame: copies finished game frames */
void xsc_drain(void);                            /* vr69: session not running: release finished frames without copying */
XrSwapchain xsc_swapchain(int i, int *w, int *h);   /* 0 left eye, 1 right eye, 2 mono (half size, for menus/loading), 3 UI rect */
const char *xsc_error(void);                     /* vr70: why xsc_init failed (step and API result) */
