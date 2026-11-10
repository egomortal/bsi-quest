/* xr_stereo_copy (VR stereo prototype, route B; see notes/VR_STEREO.md): the game renders into an AImageReader surface
   instead of the compositor's Android-surface swapchain; the XR thread copies every finished frame into ordinary OpenXR
   GLES swapchains (one per eye, plus a small mono one for menus / loading screens) and submits a projection layer.
   Why: (1) a runtime may refuse an Android-surface swapchain inside a projection layer (route A); (2) here the XR thread
   knows exactly which game frame each image is (the Nth image = the Nth Present) and when the GPU finished it (acquire
   fence), so the reported eye pose always matches the image, with no lag guess; (3) per-eye swapchains keep each eye's
   last image, which is what alternate-eye rendering (aer=1) needs.
   UI layer (regress/vr, notes/VR_STEREO.md §11): with the [ L | R | UI ] layout the UI rect is copied into a 4th swapchain
   as premultiplied alpha (linearised per colour/alpha for the sRGB target; ui_alpha=luma derives alpha from brightness
   when the game's alpha is unusable); a mono view over the eye area (stereo off) goes into the mono swapchain.
   Cost: one full-screen copy per game frame on the GPU (2 x eye size; shader also linearises sRGB and flips), ~0.2-0.4 ms
   on Adreno 740 at 2x1440x1536 (estimate), and the swapchain memory (3 images each, about 60 MB at that size).
   vr69 (native renderer, notes/VR_STEREO.md §17):
     - which Present an image is comes from rhi_b1 (vr_stereo_image_present: image k = the k-th queued present, FIFO, so
       nothing is dropped); with the bridge (no rhi_b1 record) the old "Nth image = Nth Present" count is used;
     - only the newest finished image is copied per XR frame (older finished ones are released unseen: the compositor could
       never show them anyway), so a burst of game frames costs one copy;
     - no glFinish on the XR thread (it stalled the frame loop behind the game's GPU work): a copied image is released to
       the game once an EGL fence after its copy has signalled (polled, never waited on);
     - xsc_drain() releases finished images while the session is not running, so a FIFO game never blocks on the reader.
   vr70: every init step logs its API result and xsc_error() names the failing one (the build 69 Quest run fell back to
     route A with no trace); the reader keeps at most 4 images (fewer buffers in the game's swapchain, the XR thread never
     holds more than pending + copied <= 4 with fif=1); swapchain images are enumerated with the two-call idiom (up to 16);
     the external sampler has an explicit precision; the latency of every copied frame is reported (vr_stereo_lat_*).
   Needs: libmediandk (dlopen'ed, API 26+), EGL_ANDROID_get_native_client_buffer, EGL_KHR_image_base,
   GL_OES_EGL_image_external_essl3, an OpenGL ES 3 context current on the calling (XR) thread. */
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "vr_stereo.h"
#include "xr_stereo_copy.h"

static char g_err[256] = "not initialised";   /* vr70: xsc_error() */
/* GLES 3 entry points from libGLESv3.so at run time: libbsi.so links only -lEGL, so the link line stays as it is */
#define GL_FNS(X) X(glActiveTexture) X(glAttachShader) X(glBindFramebuffer) X(glBindTexture) X(glCheckFramebufferStatus) X(glCompileShader) X(glCreateProgram) X(glCreateShader) X(glDeleteShader) X(glDeleteTextures) X(glDisable) X(glDrawArrays) X(glFinish) X(glFramebufferTexture2D) X(glGenFramebuffers) X(glGenTextures) X(glGetProgramiv) X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetError) X(glGetString) X(glGetUniformLocation) X(glLinkProgram) X(glShaderSource) X(glTexParameteri) X(glUniform1f) X(glUniform1i) X(glUniform4f) X(glUseProgram) X(glViewport) X(glEnable) X(glIsEnabled) X(glReadPixels) X(glTexImage2D) X(glDeleteFramebuffers) X(glDeleteProgram) X(glPixelStorei) X(glGenSamplers) X(glBindSampler) X(glSamplerParameteri) X(glUniform2i)
#define GL_DECL(f) static __typeof__(f) *p_##f;
GL_FNS(GL_DECL)
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindFramebuffer p_glBindFramebuffer
#define glBindTexture p_glBindTexture
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glCompileShader p_glCompileShader
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glDeleteShader p_glDeleteShader
#define glDeleteTextures p_glDeleteTextures
#define glDisable p_glDisable
#define glDrawArrays p_glDrawArrays
#define glFinish p_glFinish
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glGenFramebuffers p_glGenFramebuffers
#define glGenTextures p_glGenTextures
#define glGetProgramiv p_glGetProgramiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetError p_glGetError
#define glGetString p_glGetString
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glShaderSource p_glShaderSource
#define glTexParameteri p_glTexParameteri
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform4f p_glUniform4f
#define glUseProgram p_glUseProgram
#define glViewport p_glViewport
#define glEnable p_glEnable
#define glIsEnabled p_glIsEnabled
#define glReadPixels p_glReadPixels
#define glTexImage2D p_glTexImage2D
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteProgram p_glDeleteProgram
#define glPixelStorei p_glPixelStorei
#define glGenSamplers p_glGenSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glUniform2i p_glUniform2i
static int load_gl(void){
  void *h = dlopen("libGLESv3.so", RTLD_NOW); if(!h) h = dlopen("libGLESv2.so", RTLD_NOW);
  if(!h){ snprintf(g_err, sizeof g_err, "no libGLESv3: %s", dlerror()); fprintf(stderr, "[xr_copy] %s\n", g_err); return -1; }
  int miss = 0;
#define GL_LOAD(f) if(!(p_##f = (__typeof__(p_##f))dlsym(h, #f))){ fprintf(stderr, "[xr_copy] missing %s\n", #f); snprintf(g_err, sizeof g_err, "libGLESv3 has no %s", #f); miss++; }
  GL_FNS(GL_LOAD)
#undef GL_LOAD
  return miss ? -1 : 0;
}
extern volatile long g_br_presents;   /* bridge.c */
#define LOGC(...) do{ fprintf(stderr, "[xr_copy] " __VA_ARGS__); fputc('\n', stderr); }while(0)
#define FAILC(...) do{ snprintf(g_err, sizeof g_err, __VA_ARGS__); LOGC("init FAILED: %s", g_err); return -1; }while(0)
const char *xsc_error(void){ return g_err; }
#define XSC_MAXIMG 4   /* AImageReader maxImages (vr70: 6 -> 4) */

/* libmediandk, resolved at run time so the link line does not change */
static media_status_t (*p_newWithUsage)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
static media_status_t (*p_getWindow)(AImageReader*, ANativeWindow**);
static media_status_t (*p_acquireNextAsync)(AImageReader*, AImage**, int*);
static media_status_t (*p_getHwb)(const AImage*, AHardwareBuffer**);
static void (*p_imgDelete)(AImage*);
static EGLClientBuffer (*p_getClientBuffer)(const struct AHardwareBuffer*);
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR; static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;
static PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR; static PFNEGLCLIENTWAITSYNCKHRPROC p_eglClientWaitSyncKHR; static PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR;
static void (*p_glFlush)(void);
uint32_t vr_stereo_image_present(uint32_t k) __attribute__((weak));   /* vr_stereo.c (vr69): image k -> Present index */
uint32_t vr_stereo_images_presented(void) __attribute__((weak));

static XscFns F; static AImageReader *g_reader; static int g_w, g_h, g_flip, g_lin;
enum { SC_LEFT, SC_RIGHT, SC_MONO, SC_UI, SC_N };
typedef struct { XrSwapchain sc; int w, h; uint32_t n; GLuint fbo[16]; GLuint tex[16]; const char *what; unsigned long n_ok, n_fail; int logged; } Sc;
static Sc g_sc[SC_N];
static GLuint g_prog; static GLint g_u_rect, g_u_lin, g_u_amode; static VsLayout g_L;
typedef struct { AHardwareBuffer *hb; EGLImageKHR img; GLuint tex; uint32_t used; } Cache;
static Cache g_cache[8]; static uint32_t g_use;
typedef struct { AImage *img; int fd; uint32_t k; } Pend;
static Pend g_pend[XSC_MAXIMG]; static int g_np; static uint32_t g_count;
/* vr69: copied images wait here until the GL fence after their copy has signalled, then go back to the game */
typedef struct { AImage *img; EGLSyncKHR sync; } Held;
static Held g_held[XSC_MAXIMG]; static int g_nh;
static unsigned long g_n_copied, g_n_skipped, g_n_mapped, g_n_unmapped;
static XscShown g_shown;
/* vr72: device-side proof of the copy chain. The build 71 Quest run (route B's first run on a device) was "fuzzy" and
   "static"; the copy shader itself is exact on the host's GLES (regress/vr/vr72 xsc_gl_test: every eye pixel byte-identical
   to the game's, also with an fp16 sampler), so what is left to prove is the Quest's side:
   - sRGB write self-test at init: linear 0.2159 drawn into an SRGB8_ALPHA8 target must be stored as 128 (read back by
     sampling, which always decodes: 55 = encode on, 10 = encode off). GL_FRAMEBUFFER_SRGB is now switched on explicitly
     (EXT_sRGB_write_control); if encoding is still off, the copy writes the game's values unchanged (lin 0), so the stored
     bytes are the game's bytes either way (an off encoder would have stored linear values: dark tones crushed and banded);
   - the first game frame's buffer: AHardwareBuffer_describe (size, format 1 = R8G8B8A8_UNORM, usage, stride) and the
     reader window's dataspace (what DXVK's Vulkan swapchain set);
   - chain check, at the first stereo copy and then every 10 s: the 64x64 block at the centre of the left eye swapchain
     image just written is read back and compared with the same game pixels copied raw (lin 0) into an RGBA8 target. */
static int g_srgb_ctl, g_srgb_enc = -1;     /* EXT_sRGB_write_control present; sRGB encode on write: 1 on, 0 off, -1 untested */
static GLuint g_tprog, g_tsmp; static GLint g_tu_col, g_tu_mode, g_tu_off;
static GLuint g_chk_fbo, g_chk_tex; static unsigned long g_chk_n, g_chk_bad; static double g_chk_next; static int g_chk_due;
static char g_chk_res[240] = "not run yet"; static int g_desc_done; static ANativeWindow *g_win;
static double mono_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static const char *VS =
  "#version 300 es\n"
  "out vec2 uv;\n"
  "void main(){ vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
  "  uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *FS =
  "#version 300 es\n"
  "#extension GL_OES_EGL_image_external_essl3 : require\n"
  "precision highp float;\n"
  "uniform mediump samplerExternalOES src; uniform vec4 rect; uniform float lin; uniform float amode;\n"
  "in vec2 uv; out vec4 o;\n"
  "vec3 tolin(vec3 c){ return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
  "void main(){ vec4 s = texture(src, rect.xy + uv * rect.zw); vec3 c = s.rgb;\n"
  "  /* the game writes sRGB-encoded values; an SRGB8_ALPHA8 target re-encodes on write, so linearise first */\n"
  "  if(amode < 0.5){ o = vec4(mix(c, tolin(c), lin), 1.0); return; }\n"
  "  /* UI rect: colour = UI over black (premultiplied), alpha = coverage (vr_ui_rhi.c) or, amode 2, from brightness */\n"
  "  float a = s.a; if(amode > 1.5) a = max(a, clamp(max(c.r, max(c.g, c.b)) * 4.0, 0.0, 1.0));\n"
  "  vec3 u = a > 0.0 ? c / a : vec3(0.0);\n"
  "  o = vec4(mix(u, tolin(min(u, vec3(1.0))), lin) * a, a); }\n";

static const char *TFS =   /* vr72: check program: mode 0 writes col; 1 fetches texel (x, y) + off of a 2D texture (sampling an sRGB
                                texture decodes it); 2 = 1 + exact sRGB encode in the shader (gives back an sRGB texture's stored bytes) */
  "#version 300 es\n"
  "precision highp float;\n"
  "uniform vec4 col; uniform int mode; uniform ivec2 off; uniform highp sampler2D t;\n"
  "out vec4 o;\n"
  "vec3 enc(vec3 l){ return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l)); }\n"
  "void main(){ if(mode == 0){ o = col; return; } vec4 v = texelFetch(t, ivec2(gl_FragCoord.xy) + off, 0);\n"
  "  o = mode == 2 ? vec4(enc(clamp(v.rgb, 0.0, 1.0)), v.a) : v; }\n";

static GLuint shader(GLenum t, const char *src){
  GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if(!ok){ char log[512]; log[0] = 0; glGetShaderInfoLog(s, sizeof log, NULL, log); LOGC("shader: %s", log);
           snprintf(g_err, sizeof g_err, "%s shader compile: %.180s", t == GL_VERTEX_SHADER ? "vertex" : "fragment", log); glDeleteShader(s); return 0; }
  return s;
}

/* vr71: the swapchain format, enumerated once with the OpenXR two-call idiom (vr_stereo.c vs_pick_swapchain_format; build 70
   passed a fixed 64-entry array and the Quest 3 runtime's 71 formats made xrEnumerateSwapchainFormats fail with -11) */
static int enum_fmt(void *ctx, uint32_t cap, uint32_t *n, int64_t *out){ (void)ctx; return (int)F.formats(F.sess, cap, n, out); }
static int64_t g_fmt; static int g_fmt_done;
static int make_sc(Sc *s, int w, int h, const char *what){
  if(!g_fmt_done){ g_fmt_done = 1;
    int64_t list[24]; uint32_t nf = 0; memset(list, 0, sizeof list);
    int fr = vs_pick_swapchain_format(enum_fmt, NULL, &g_fmt, &nf, list, 24);
    char b[400] = ""; size_t n = 0;
    for(uint32_t i = 0; i < nf && i < 24 && n + 12 < sizeof b; i++) n += snprintf(b + n, sizeof b - n, " 0x%llx", (unsigned long long)list[i]);
    LOGC("step swapchain formats: xrEnumerateSwapchainFormats %d (two-call), %u formats:%s%s -> %s", fr, nf, b, nf > 24 ? " ..." : "",
         g_fmt == 0x8C43 ? "GL_SRGB8_ALPHA8 (the copy linearises, the target re-encodes: the game's sRGB bytes, decoded once by the compositor)"
         : g_fmt == 0x8058 ? "GL_RGBA8 (no sRGB format: linear values in 8 bits, darker tones band)" : "none usable");
    if(!g_fmt) FAILC("no GL_SRGB8_ALPHA8 / GL_RGBA8 swapchain format (xrEnumerateSwapchainFormats %d, %u formats)", fr, nf); }
  int64_t fmt = g_fmt; if(!fmt) FAILC("no usable swapchain format");
  /* sRGB: the game surface holds display-ready (sRGB-encoded) RGBA8 UNORM values. SRGB8_ALPHA8: linearise in the shader,
     the target re-encodes on write -> the stored bytes are the game's bytes, the compositor decodes once (correct). RGBA8:
     the compositor treats UNORM as linear, so the shader must write linear values too (lin = 1) or the picture would be
     shown double-gamma'd (washed out); the cost is banding in dark tones. */
  g_lin = (fmt == 0x8C43 && g_srgb_enc == 0) ? 0 : 1;   /* vr72: sRGB encoder found off (self-test): store the game's values as they are */
  XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT; ci.format = fmt;
  ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
  XrResult r = F.create(F.sess, &ci, &s->sc); if(r != XR_SUCCESS) FAILC("xrCreateSwapchain %s %dx%d fmt 0x%llx: %d", what, w, h, (unsigned long long)fmt, r);
  uint32_t n = 0; XrResult er = F.images(s->sc, 0, &n, NULL);   /* two-call idiom: the runtime's image count first */
  if(er != XR_SUCCESS || !n || n > 16) FAILC("xrEnumerateSwapchainImages %s (count): %d, %u images", what, er, n);
  XrSwapchainImageOpenGLESKHR im[16]; for(int i = 0; i < 16; i++){ im[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR; im[i].next = NULL; }
  if((er = F.images(s->sc, n, &s->n, (XrSwapchainImageBaseHeader*)im)) != XR_SUCCESS || !s->n) FAILC("xrEnumerateSwapchainImages %s: %d", what, er);
  glGenFramebuffers(s->n, s->fbo);
  GLenum st = GL_FRAMEBUFFER_COMPLETE;
  for(uint32_t i = 0; i < s->n; i++){ s->tex[i] = im[i].image; glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex[i], 0);
    GLenum si = glCheckFramebufferStatus(GL_FRAMEBUFFER); if(si != GL_FRAMEBUFFER_COMPLETE) st = si; }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if(st != GL_FRAMEBUFFER_COMPLETE) FAILC("swapchain %s FBO incomplete: 0x%x", what, st);
  s->w = w; s->h = h; s->what = what;
  LOGC("step swapchain %s: %dx%d %s, usage color-attachment|sampled, %u images (runtime count %u): ok", what, w, h, fmt == 0x8C43 ? "SRGB8_ALPHA8" : "RGBA8", s->n, n);
  return 0;
}

/* vr72: one small 2D texture + FBO (format fmt, w x 1 or w x h) */
static GLuint mk_target(GLenum fmt, int w, int h, GLuint *fbo){
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, fbo); glBindFramebuffer(GL_FRAMEBUFFER, *fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  return t;
}
static void srgb_selftest(void){
  GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, TFS);
  if(!vs || !fs){ LOGC("step sRGB write self-test: shader failed (%s): untested", g_err); return; }
  g_tprog = glCreateProgram(); glAttachShader(g_tprog, vs); glAttachShader(g_tprog, fs); glLinkProgram(g_tprog); glDeleteShader(vs); glDeleteShader(fs);
  GLint ok = 0; glGetProgramiv(g_tprog, GL_LINK_STATUS, &ok); if(!ok){ LOGC("step sRGB write self-test: link failed: untested"); return; }
  g_tu_col = glGetUniformLocation(g_tprog, "col"); g_tu_mode = glGetUniformLocation(g_tprog, "mode"); g_tu_off = glGetUniformLocation(g_tprog, "off");
  glUseProgram(g_tprog); glUniform1i(glGetUniformLocation(g_tprog, "t"), 1); glUniform2i(g_tu_off, 0, 0);
  /* a sampler object for unit 1: texelFetch needs a complete texture, and the runtime's swapchain textures keep the default
     mipmapped min filter with one level; the sampler overrides that without touching the runtime's texture state */
  glGenSamplers(1, &g_tsmp); glSamplerParameteri(g_tsmp, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glSamplerParameteri(g_tsmp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  GLuint fa = 0, fb = 0; GLuint ta = mk_target(0x8C43 /* GL_SRGB8_ALPHA8 */, 4, 1, &fa), tb = mk_target(0x8058 /* GL_RGBA8 */, 4, 1, &fb);
  int en_before = g_srgb_ctl ? glIsEnabled(0x8DB9) : -1;
  if(g_srgb_ctl) glEnable(0x8DB9);   /* GL_FRAMEBUFFER_SRGB_EXT */
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
  unsigned char raw[16], smp[16]; memset(raw, 0, sizeof raw); memset(smp, 0, sizeof smp);
  glBindFramebuffer(GL_FRAMEBUFFER, fa); glViewport(0, 0, 4, 1);
  glUniform1i(g_tu_mode, 0); glUniform4f(g_tu_col, 0.2158605f, 0.2158605f, 0.2158605f, 1.f); glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(0, 0, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, raw);
  glBindFramebuffer(GL_FRAMEBUFFER, fb); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, ta); glBindSampler(1, g_tsmp);
  glUniform1i(g_tu_mode, 1); glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(0, 0, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, smp);
  glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE0); glBindFramebuffer(GL_FRAMEBUFFER, 0); glUseProgram(0);
  GLenum ge = glGetError();
  int v = smp[1]; g_srgb_enc = v >= 52 && v <= 58 ? 1 : v >= 7 && v <= 13 ? 0 : -1;
  LOGC("step sRGB write self-test: EXT_sRGB_write_control %d (FRAMEBUFFER_SRGB was %s%s), linear 0.2159 drawn into SRGB8_ALPHA8: read back %d (128 = stored encoded, "
       "or 55 if this driver decodes on read), sampled back %d -> sRGB encode on write: %s (GL error 0x%x)", g_srgb_ctl, en_before < 0 ? "not settable" : en_before ? "on" : "OFF", g_srgb_ctl ? ", now on" : "", raw[1], v,
       g_srgb_enc == 1 ? "ON (correct)" : g_srgb_enc == 0 ? "OFF: the copy writes the game's values unchanged instead (lin 0)" : "unclear (copy linearises as before)", ge);
  glDeleteFramebuffers(1, &fa); glDeleteFramebuffers(1, &fb); glDeleteTextures(1, &ta); glDeleteTextures(1, &tb);
}
/* the copy's rect uniform for source rect (u0, v0, du, dv) as copy_into sets it */
static void set_rect(float u0, float v0, float du, float dv){ if(g_flip) glUniform4f(g_u_rect, u0, v0 + dv, du, -dv); else glUniform4f(g_u_rect, u0, v0, du, dv); }
static double tolin_d(double c){ return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
/* vr72 chain check: called by copy_into right after the copy into swapchain image i of s (still acquired). The stored bytes
   are recovered by sampling the swapchain texture (an sRGB texture decodes on sampling, the check shader re-encodes exactly),
   which does not depend on whether this driver's glReadPixels decodes sRGB; the reference is the same game pixels copied raw
   (lin 0) into an RGBA8 target. Expected: SRGB8_ALPHA8 swapchain = the game's bytes; RGBA8 swapchain = linear(game). */
static void chain_check(Sc *s, uint32_t i, GLuint src, float u0, float v0, float du, float dv){
  enum { B = 64 };
  if(s->w < B || s->h < B || !g_tprog) return;
  static unsigned char a[B * B * 4], b[B * B * 4];
  int x0 = s->w / 2 - B / 2, y0 = s->h / 2 - B / 2;
  if(!g_chk_fbo) g_chk_tex = mk_target(0x8058, B, B, &g_chk_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g_chk_fbo); glViewport(0, 0, B, B);
  /* reference: the game's own bytes for that block */
  float us = u0 + du * x0 / s->w, dus = du * B / s->w, dvs = dv * B / s->h;
  float vs = g_flip ? v0 + dv - dv * (float)(y0 + B) / s->h : v0 + dv * (float)y0 / s->h;
  set_rect(us, vs, dus, dvs); glUniform1f(g_u_lin, 0.f); glUniform1f(g_u_amode, 0.f);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, src); glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(0, 0, B, B, GL_RGBA, GL_UNSIGNED_BYTE, b);
  /* what the swapchain image holds there */
  int srgb = g_fmt == 0x8C43;
  glUseProgram(g_tprog); glUniform1i(g_tu_mode, srgb ? 2 : 1); glUniform2i(g_tu_off, x0, y0);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s->tex[i]); glBindSampler(1, g_tsmp);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(0, 0, B, B, GL_RGBA, GL_UNSIGNED_BYTE, a);
  glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE0); glUniform2i(g_tu_off, 0, 0); glUseProgram(g_prog);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  long d = 0; int m = 0; double sum[3] = { 0, 0, 0 }, det = 0; long na = 0;
  for(int k0 = 0; k0 < B * B; k0++){
    for(int k = 0; k < 3; k++){ int g = b[k0 * 4 + k], want = srgb ? g : (int)(tolin_d(g / 255.0) * 255.0 + 0.5), dd = abs(a[k0 * 4 + k] - want);
      if(dd > 1) d++;
      if(dd > m) m = dd;
      sum[k] += g;
      if((k0 % B) + 1 < B) det += abs(b[(k0 + 1) * 4 + k] - g); }
    if(a[k0 * 4 + 3] != 255) na++; }
  g_chk_n++; if(d || na) g_chk_bad++;
  snprintf(g_chk_res, sizeof g_chk_res, "#%lu %s: %ld of %d values off by >1 (max %d)%s", g_chk_n, d || na ? "WRONG" : "exact", d, B * B * 3, m,
           g_chk_bad ? ", some checks WRONG" : "");
  if(g_chk_n <= 3 || d || na)
    LOGC("chain check #%lu (left eye, %dx%d block at the image centre, %s swapchain): %ld of %d values off by >1 (max %d), alpha != 255: %ld -> %s; "
         "game block mean RGB %.0f/%.0f/%.0f, detail (mean neighbour step) %.1f", g_chk_n, B, B, srgb ? "SRGB8_ALPHA8" : "RGBA8", d, B * B * 3, m, na,
         d || na ? "WRONG (the compositor gets other values than the game drew: colour/gamma or position error)" : "EXACT (the compositor gets the game's pixels)",
         sum[0] / (B * B), sum[1] / (B * B), sum[2] / (B * B), det / ((B - 1) * B * 3));
}

int xsc_init(const XscFns *f, int w, int h, int flip, ANativeWindow **win){
  F = *f; g_w = w & ~1; g_h = h; g_flip = flip;
  LOGC("init: game surface %dx%d, flip %d", g_w, g_h, g_flip);
  void *md = dlopen("libmediandk.so", RTLD_NOW);
  if(!md) FAILC("dlopen libmediandk.so: %s", dlerror());
  p_newWithUsage = dlsym(md, "AImageReader_newWithUsage"); p_getWindow = dlsym(md, "AImageReader_getWindow");
  p_acquireNextAsync = dlsym(md, "AImageReader_acquireNextImageAsync"); p_getHwb = dlsym(md, "AImage_getHardwareBuffer");
  p_imgDelete = dlsym(md, "AImage_delete");
  p_getClientBuffer = (EGLClientBuffer (*)(const struct AHardwareBuffer*))eglGetProcAddress("eglGetNativeClientBufferANDROID");
  p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  p_eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
  p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
  p_glFlush = (void (*)(void))eglGetProcAddress("glFlush");
  LOGC("step entry points: AImageReader_newWithUsage %p getWindow %p acquireNextImageAsync %p AImage_getHardwareBuffer %p AImage_delete %p | "
       "eglGetNativeClientBufferANDROID %p eglCreateImageKHR %p eglDestroyImageKHR %p glEGLImageTargetTexture2DOES %p eglCreateSyncKHR %p eglClientWaitSyncKHR %p glFlush %p",
       (void*)p_newWithUsage, (void*)p_getWindow, (void*)p_acquireNextAsync, (void*)p_getHwb, (void*)p_imgDelete, (void*)p_getClientBuffer, (void*)p_eglCreateImageKHR,
       (void*)p_eglDestroyImageKHR, (void*)p_glEGLImageTargetTexture2DOES, (void*)p_eglCreateSyncKHR, (void*)p_eglClientWaitSyncKHR, (void*)p_glFlush);
  if(!p_newWithUsage || !p_getWindow || !p_acquireNextAsync || !p_getHwb || !p_imgDelete || !p_getClientBuffer || !p_eglCreateImageKHR ||
     !p_eglDestroyImageKHR || !p_glEGLImageTargetTexture2DOES) FAILC("missing ImageReader / EGL image entry points (see the step line)");
  { EGLContext c = eglGetCurrentContext(); EGLDisplay d = eglGetCurrentDisplay(); EGLint ver = 0;
    if(c != EGL_NO_CONTEXT) eglQueryContext(d, c, EGL_CONTEXT_CLIENT_VERSION, &ver);
    LOGC("step GLES context: current %p (ES %d), display %p", (void*)c, ver, (void*)d);
    if(c == EGL_NO_CONTEXT) FAILC("no current GLES context on the XR thread (eglGetError 0x%x)", eglGetError()); }
  if(load_gl()) FAILC("libGLESv3: %s", g_err);
  { const char *ge = (const char *)glGetString(GL_EXTENSIONS);
    g_srgb_ctl = ge && strstr(ge, "GL_EXT_sRGB_write_control") != NULL;   /* vr72 */
    LOGC("step GL extensions: OES_EGL_image_external %d, OES_EGL_image_external_essl3 %d, EXT_sRGB_write_control %d",
         ge && strstr(ge, "GL_OES_EGL_image_external") != NULL, ge && strstr(ge, "GL_OES_EGL_image_external_essl3") != NULL, g_srgb_ctl); }
  GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS); if(!vs || !fs) FAILC("%s", g_err);
  g_prog = glCreateProgram(); glAttachShader(g_prog, vs); glAttachShader(g_prog, fs); glLinkProgram(g_prog);
  GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok); if(!ok) FAILC("copy program link failed");
  LOGC("step copy shader: ok");
  g_u_rect = glGetUniformLocation(g_prog, "rect"); g_u_lin = glGetUniformLocation(g_prog, "lin"); g_u_amode = glGetUniformLocation(g_prog, "amode");
  glUseProgram(g_prog); glUniform1i(glGetUniformLocation(g_prog, "src"), 0); glUseProgram(0);
  srgb_selftest();   /* vr72 */
  vr_stereo_get_layout_intent(g_w, g_h, &g_L);   /* swapchain sizes; the copy rects follow the layout the game renders */
  if(make_sc(&g_sc[SC_LEFT], g_L.eye_w, g_h, "left eye") || make_sc(&g_sc[SC_RIGHT], g_L.eye_w, g_h, "right eye") ||
     make_sc(&g_sc[SC_MONO], g_L.eye_w, g_h / 2, "mono")) return -1;
  if(g_L.ui && make_sc(&g_sc[SC_UI], g_L.ui_w, g_L.ui_h, "UI")) return -1;
  g_shown.L = g_L;
  media_status_t m = p_newWithUsage(g_w, g_h, AIMAGE_FORMAT_RGBA_8888,
                                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT, XSC_MAXIMG, &g_reader);
  if(m != AMEDIA_OK || !g_reader) FAILC("AImageReader_newWithUsage %dx%d RGBA_8888 usage GPU_SAMPLED|GPU_COLOR_OUTPUT max %d: %d", g_w, g_h, XSC_MAXIMG, m);
  LOGC("step AImageReader %dx%d RGBA_8888, max %d images: ok", g_w, g_h, XSC_MAXIMG);
  m = p_getWindow(g_reader, win);
  if(m != AMEDIA_OK || !*win) FAILC("AImageReader_getWindow: %d", m);
  g_win = *win;   /* vr72: its dataspace is logged once the game presents */
  LOGC("ready: game surface %dx%d -> eyes %dx%d + mono %dx%d + UI %dx%d, %s swapchains, flip %d", g_w, g_h, g_L.eye_w, g_h, g_L.eye_w, g_h / 2,
       g_L.ui_w, g_L.ui_h, g_lin ? "sRGB" : "UNORM", g_flip);
  snprintf(g_err, sizeof g_err, "ok");
  return 0;
}

static GLuint tex_for(AHardwareBuffer *hb){
  g_use++;
  for(int i = 0; i < 8; i++) if(g_cache[i].hb == hb){ g_cache[i].used = g_use; return g_cache[i].tex; }
  int v = 0; for(int i = 1; i < 8; i++) if(g_cache[i].used < g_cache[v].used) v = i;   /* evict the least recently used */
  EGLDisplay dpy = eglGetCurrentDisplay();
  if(g_cache[v].hb){ glDeleteTextures(1, &g_cache[v].tex); p_eglDestroyImageKHR(dpy, g_cache[v].img); memset(&g_cache[v], 0, sizeof g_cache[v]); }
  EGLint at[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
  EGLImageKHR img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, p_getClientBuffer(hb), at);
  if(img == EGL_NO_IMAGE_KHR){ LOGC("eglCreateImageKHR failed: 0x%x", eglGetError()); return 0; }
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_EXTERNAL_OES, t);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
  g_cache[v].hb = hb; g_cache[v].img = img; g_cache[v].tex = t; g_cache[v].used = g_use;
  return t;
}

/* draw source rect (u0, v0, du, dv in source UV, v0 = top) into one swapchain; amode 0 opaque, 1 premultiplied UI, 2 luma */
static void copy_into(Sc *s, GLuint src, float u0, float v0, float du, float dv, int amode){
  uint32_t i = 0; XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
  XrResult ar = F.acquire(s->sc, &ai, &i);
  if(ar != XR_SUCCESS || i >= s->n){ s->n_fail++;   /* vr71: never silent */
    if(s->logged < 3){ s->logged++; LOGC("copy into %s: xrAcquireSwapchainImage %d (image %u of %u)", s->what ? s->what : "?", ar, i, s->n); }
    if(ar == XR_SUCCESS){ XrSwapchainImageReleaseInfo ri0 = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; F.release(s->sc, &ri0); }
    return; }
  XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
  XrResult wr = F.wait(s->sc, &wi);
  if(!s->n_ok && !s->logged){ s->logged = 1; GLenum ge = glGetError ? glGetError() : 0;
    LOGC("first copy into %s: acquire %d (image %u), wait %d, GL error before 0x%x, source rect %.3f,%.3f %.3fx%.3f, alpha mode %d, %s", s->what ? s->what : "?",
         ar, i, wr, ge, u0, v0, du, dv, amode, g_lin ? "linearised for the sRGB target" : "raw"); }
  if(wr != XR_SUCCESS) s->n_fail++;
  if(wr == XR_SUCCESS){ s->n_ok++;
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[i]); glViewport(0, 0, s->w, s->h);
    glUseProgram(g_prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
    /* GL texture row 0 is the image's first row in memory = top of the game frame; OpenXR GL images put row 0 at the bottom */
    if(g_flip) glUniform4f(g_u_rect, u0, v0 + dv, du, -dv); else glUniform4f(g_u_rect, u0, v0, du, dv);
    glUniform1f(g_u_lin, g_lin ? 1.f : 0.f); glUniform1f(g_u_amode, (float)amode);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    if(g_srgb_ctl) glEnable(0x8DB9);   /* vr72: GL_FRAMEBUFFER_SRGB_EXT explicitly on (sRGB targets encode on write) */
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if(g_chk_due && s == &g_sc[SC_LEFT] && amode == 0){ g_chk_due = 0; chain_check(s, i, src, u0, v0, du, dv); }   /* vr72 */
  }
  XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; F.release(s->sc, &ri);
}

static int fence_done(int fd){
  if(fd < 0) return 1;
  struct pollfd p = { fd, POLLIN, 0 }; return poll(&p, 1, 0) > 0;
}

/* release copied images whose copy has finished on the GPU (vr69; without fence support: glFinish once, as before) */
static void release_held(int wait_all){
  EGLDisplay dpy = eglGetCurrentDisplay(); int k = 0;
  for(int i = 0; i < g_nh; i++){
    Held h = g_held[i]; int done = 1;
    if(h.sync != EGL_NO_SYNC_KHR && p_eglClientWaitSyncKHR){
      EGLint r = p_eglClientWaitSyncKHR(dpy, h.sync, wait_all ? EGL_SYNC_FLUSH_COMMANDS_BIT_KHR : 0, wait_all ? 50000000 : 0);
      done = r != EGL_TIMEOUT_EXPIRED_KHR; }
    if(done){ if(h.sync != EGL_NO_SYNC_KHR) p_eglDestroySyncKHR(dpy, h.sync); p_imgDelete(h.img); }
    else g_held[k++] = h;
  }
  g_nh = k;
}
static void hold(AImage *img){
  EGLSyncKHR sy = EGL_NO_SYNC_KHR;
  if(p_eglCreateSyncKHR) sy = p_eglCreateSyncKHR(eglGetCurrentDisplay(), EGL_SYNC_FENCE_KHR, NULL);
  if(sy == EGL_NO_SYNC_KHR){ glFinish(); p_imgDelete(img); return; }   /* no fences: the old, blocking way */
  if(p_glFlush) p_glFlush();
  if(g_nh == XSC_MAXIMG){ release_held(1); }
  if(g_nh == XSC_MAXIMG){ glFinish(); release_held(1); }
  g_held[g_nh].img = img; g_held[g_nh].sync = sy; g_nh++;
}
/* take every queued game frame (in queue order: image k = queued present k) */
static void take_images(void){
  for(;;){
    if(g_np == XSC_MAXIMG || g_np + g_nh >= XSC_MAXIMG) break;   /* the reader hands out at most maxImages images */
    AImage *img = NULL; int fd = -1;
    if(p_acquireNextAsync(g_reader, &img, &fd) != AMEDIA_OK || !img) break;
    g_pend[g_np].img = img; g_pend[g_np].fd = fd; g_pend[g_np].k = ++g_count; g_np++;
  }
}
/* vr70: the same mapping without counting (latency bookkeeping) */
static uint32_t present_of_peek(uint32_t k){ uint32_t p = vr_stereo_image_present ? vr_stereo_image_present(k) : 0; return p ? p : k; }
/* which Present image k shows: rhi_b1's record (native renderer), else the image count (bridge mode) */
static uint32_t present_of(uint32_t k){
  uint32_t p = vr_stereo_image_present ? vr_stereo_image_present(k) : 0;
  if(p){ g_n_mapped++; return p; }
  g_n_unmapped++; return k;
}
/* XR thread, session not running: hand finished images back without copying (a FIFO game would block otherwise) */
void xsc_drain(void){
  if(!g_reader) return;
  release_held(0); take_images();
  while(g_np && fence_done(g_pend[0].fd)){
    Pend p = g_pend[0]; memmove(g_pend, g_pend + 1, (--g_np) * sizeof *g_pend);
    if(p.fd >= 0) close(p.fd);
    p_imgDelete(p.img); g_n_skipped++;
  }
}

/* once per XR frame, before xrEndFrame */
const XscShown *xsc_frame(void){
  if(!g_reader) return &g_shown;
  release_held(0);
  take_images();
  /* the finished prefix of the queue (the GPU finishes frames in order); only the newest one is copied */
  int nf = 0; while(nf < g_np && fence_done(g_pend[nf].fd)) nf++;
  for(int i = 0; i < nf; i++) vr_stereo_lat_done(present_of_peek(g_pend[i].k));   /* vr70: GPU done (seen now) */
  for(int i = 0; i < nf; i++){
    Pend p = g_pend[i];
    if(p.fd >= 0) close(p.fd);
    if(i < nf - 1){ p_imgDelete(p.img); g_n_skipped++; continue; }   /* superseded before it could be shown */
    AHardwareBuffer *hb = NULL; GLuint t = 0;
    if(p_getHwb(p.img, &hb) == AMEDIA_OK && hb) t = tex_for(hb);
    if(hb && !g_desc_done){ g_desc_done = 1;   /* vr72: what DXVK's Vulkan swapchain really allocated on the reader's window */
      void (*desc)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = (void (*)(const AHardwareBuffer*, AHardwareBuffer_Desc*))dlsym(RTLD_DEFAULT, "AHardwareBuffer_describe");
      int32_t (*dsf)(ANativeWindow*) = (int32_t (*)(ANativeWindow*))dlsym(RTLD_DEFAULT, "ANativeWindow_getBuffersDataSpace");
      AHardwareBuffer_Desc d; memset(&d, 0, sizeof d); if(desc) desc(hb, &d);
      LOGC("game frame buffer: %ux%u, %u layer(s), format %u (%s), usage 0x%llx, stride %u px; reader window dataspace 0x%x (0 = unknown, 0x8810000 = sRGB: what the producer declared); "
           "copy %s, sRGB encode on write %s", d.width, d.height, d.layers, d.format, d.format == 1 ? "R8G8B8A8_UNORM, as expected" : d.format == 2 ? "R8G8B8X8_UNORM" : d.format == 4 ? "R5G6B5: 16-bit, banding!" :
           d.format == 0x2b ? "R10G10B10A2" : d.format == 0x16 ? "R16G16B16A16_FLOAT" : "unexpected", (unsigned long long)d.usage, d.stride, dsf && g_win ? (unsigned)dsf(g_win) : 0u,
           (d.width == (uint32_t)g_w && d.height == (uint32_t)g_h) ? "1:1 (eye swapchains = the eye columns)" : "SIZE MISMATCH with the reader (the game's frame is scaled)",
           g_srgb_enc == 1 ? "on" : g_srgb_enc == 0 ? "OFF (lin 0)" : "untested"); }
    int copied = 0;
    if(t){
      uint32_t pr = present_of(p.k);
      VsEye ev[2]; int mask = 0; long long xt = 0; int kind = vr_stereo_xr_entry3(pr, &mask, ev, &xt);
      { VsLayout cur; vr_stereo_get_layout(g_w, g_h, &cur); if(cur.ui || !g_L.ui) g_L = cur; else { g_L.eye_w = cur.eye_w; g_L.ui = 0; } }
      float ew = (float)g_L.eye_w / g_w;
      if(kind == VS_KIND_STEREO && mask && mono_s() >= g_chk_next){ g_chk_due = 1; g_chk_next = mono_s() + 10.0; }   /* vr72: chain check (left eye) */
      if(kind == VS_KIND_STEREO && mask){ for(int e = 0; e < 2; e++) if(mask >> e & 1){ copy_into(&g_sc[e], t, e * ew, 0.f, ew, 1.f, 0); g_shown.frame[e] = pr; g_shown.eye[e] = ev[e]; } }
      else if(kind == VS_KIND_MONOWIDE){ copy_into(&g_sc[SC_MONO], t, 0.f, 0.f, 2.f * ew, 1.f, 0); g_shown.frame[2] = pr; }
      else if(kind == VS_KIND_NONE){ copy_into(&g_sc[SC_MONO], t, 0.f, 0.f, 1.f, 1.f, 0); g_shown.frame[2] = pr; }
      if(g_L.ui && g_sc[SC_UI].sc && (kind != VS_KIND_NONE || vr_stereo_cfg()->mono_src == 1)){
        int am = vr_stereo_cfg()->ui_alpha == 2 ? 2 : 1;
        copy_into(&g_sc[SC_UI], t, (float)g_L.ui_x / g_w, (float)g_L.ui_y / g_h, (float)g_L.ui_w / g_w, (float)g_L.ui_h / g_h, am); g_shown.frame[3] = pr; }
      g_shown.stereo = kind == VS_KIND_STEREO && mask != 0; g_shown.kind = kind; g_shown.L = g_L; copied = 1; g_n_copied++;
      if(kind == VS_KIND_STEREO) g_shown.xr_time = xt;
      vr_stereo_lat_shown(pr, 2);   /* vr70 */
    }
    if(copied) hold(p.img); else p_imgDelete(p.img);   /* the copy must be done before the buffer goes back to the game */
  }
  if(nf){ memmove(g_pend, g_pend + nf, (g_np - nf) * sizeof *g_pend); g_np -= nf; }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  g_shown.count = g_count;
  { static struct timespec next; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if(ts.tv_sec >= next.tv_sec){ next.tv_sec = ts.tv_sec + 10;   /* images = queued presents (rhi_b1) = game Presents, give or take frames in flight */
      LOGC("game frames %u (queued presents %u, game Presents %ld): copied %lu, superseded %lu, Present map %lu / count fallback %lu; showing L %u R %u mono %u UI %u, kind %d, %d pending, %d held; "
           "swapchain copies ok/failed: L %lu/%lu R %lu/%lu mono %lu/%lu UI %lu/%lu; chain check %s",
           g_count, vr_stereo_images_presented ? vr_stereo_images_presented() : 0, (long)g_br_presents, g_n_copied, g_n_skipped, g_n_mapped, g_n_unmapped,
           g_shown.frame[0], g_shown.frame[1], g_shown.frame[2], g_shown.frame[3], g_shown.kind, g_np, g_nh,
           g_sc[0].n_ok, g_sc[0].n_fail, g_sc[1].n_ok, g_sc[1].n_fail, g_sc[2].n_ok, g_sc[2].n_fail, g_sc[3].n_ok, g_sc[3].n_fail, g_chk_res); } }
  return &g_shown;
}

XrSwapchain xsc_swapchain(int i, int *w, int *h){ if(w) *w = g_sc[i].w; if(h) *h = g_sc[i].h; return g_sc[i].sc; }
