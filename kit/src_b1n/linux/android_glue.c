/* BSI-Quest Android entry point (NativeActivity, no Java code).
   - ANativeActivity_onCreate starts the game thread, which calls the runtime's bsi_main()
   - game data:  <external files dir>/BioShock Infinite   (= /sdcard/Android/data/<package>/files/BioShock Infinite)
     save/config: <external files dir>/user, log: <external files dir>/bsi.log
   - input: an input thread drains the activity's AInputQueue (keyboard, mouse, gamepad, Quest controllers as gamepad)
   - audio: AAudio stream, float32 stereo 48 kHz (fed by xaudio.c) */
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#include "android_plat.h"
#ifndef BSI_VERSION
#define BSI_VERSION "dev"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BSIQuest", __VA_ARGS__)

int bsi_main(int argc, char **argv);
void bsi_dxvk_core_probe(void);
void bsi_vk_caps_probe(void *loader);

static ANativeActivity *g_act;
static ANativeWindow *g_win;
static void *g_vulkan_loader;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_started;

/* ---------------- window ---------------- */
void xr_start(ANativeActivity *a, const char *files); ANativeWindow *xr_window(int *w, int *h);
static int g_xr_mode = 1;
void *android_window(void){
  if(g_xr_mode){ ANativeWindow *xw = xr_window(NULL, NULL); if(xw) return xw; g_xr_mode = 0; fprintf(stderr, "[android] immersive mode unavailable, using the panel window\n"); }
  pthread_mutex_lock(&g_mu);
  while(!g_win) pthread_cond_wait(&g_cv, &g_mu);
  ANativeWindow *w = g_win;
  pthread_mutex_unlock(&g_mu);
  return w;
}
static void onNativeWindowCreated(ANativeActivity *a, ANativeWindow *w){
  pthread_mutex_lock(&g_mu); g_win = w; ANativeWindow_acquire(w); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
  LOGI("window %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
}
static void onNativeWindowDestroyed(ANativeActivity *a, ANativeWindow *w){
  /* The game keeps rendering into the old surface; when the panel is closed the process is ended. */
  LOGI("window destroyed");
}

/* ---------------- input ---------------- */
#define QN 1024
static struct { int t, a, b; } q[QN]; static int qh, qt; static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static void qpush(int t, int a, int b){ pthread_mutex_lock(&qmu); int n=(qt+1)%QN; if(n!=qh){ q[qt].t=t; q[qt].a=a; q[qt].b=b; qt=n; } pthread_mutex_unlock(&qmu); }
int android_input_next(int *t, int *a, int *b){
  pthread_mutex_lock(&qmu); if(qh==qt){ pthread_mutex_unlock(&qmu); return 0; }
  *t=q[qh].t; *a=q[qh].a; *b=q[qh].b; qh=(qh+1)%QN; pthread_mutex_unlock(&qmu); return 1;
}
static AndroidPad g_pad; static int g_pad_on; static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static AndroidPad g_xrpad; static int g_xrpad_on;
void android_pad_set(const AndroidPad *p){ pthread_mutex_lock(&pmu); g_xrpad = *p; g_xrpad_on = 1; pthread_mutex_unlock(&pmu); }
static int16_t bigger(int16_t a, int16_t b){ return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }
int android_pad(AndroidPad *p){   /* Touch controllers (OpenXR) merged with any Bluetooth gamepad */
  pthread_mutex_lock(&pmu); AndroidPad a = g_pad, b = g_xrpad; int on = g_pad_on || g_xrpad_on; pthread_mutex_unlock(&pmu);
  p->buttons = a.buttons | b.buttons; p->lt = a.lt > b.lt ? a.lt : b.lt; p->rt = a.rt > b.rt ? a.rt : b.rt;
  p->lx = bigger(a.lx, b.lx); p->ly = bigger(a.ly, b.ly); p->rx = bigger(a.rx, b.rx); p->ry = bigger(a.ry, b.ry);
  { extern int autopilot_pad(AndroidPad*) __attribute__((weak)); AndroidPad c;   /* b78: autopilot.c synthetic pad (lab runs), merged like a third pad */
    if(autopilot_pad && autopilot_pad(&c)){ on = 1; p->buttons |= c.buttons; if(c.lt > p->lt) p->lt = c.lt; if(c.rt > p->rt) p->rt = c.rt;
      p->lx = bigger(p->lx, c.lx); p->ly = bigger(p->ly, c.ly); p->rx = bigger(p->rx, c.rx); p->ry = bigger(p->ry, c.ry); } }
  return on; }
extern float g_xr_rumble_l, g_xr_rumble_r;
void android_rumble(float l, float r){ g_xr_rumble_l = l; g_xr_rumble_r = r; }

static uint32_t key2vk(int k){
  if(k>=AKEYCODE_A && k<=AKEYCODE_Z) return 'A'+(k-AKEYCODE_A);
  if(k>=AKEYCODE_0 && k<=AKEYCODE_9) return '0'+(k-AKEYCODE_0);
  if(k>=AKEYCODE_F1 && k<=AKEYCODE_F12) return 112+(k-AKEYCODE_F1);
  switch(k){
  case AKEYCODE_ENTER: return 13; case AKEYCODE_ESCAPE: return 27; case AKEYCODE_SPACE: return 32; case AKEYCODE_TAB: return 9; case AKEYCODE_DEL: return 8;
  case AKEYCODE_DPAD_LEFT: return 37; case AKEYCODE_DPAD_UP: return 38; case AKEYCODE_DPAD_RIGHT: return 39; case AKEYCODE_DPAD_DOWN: return 40;
  case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return 16; case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return 17;
  case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return 18; }
  return 0;
}
static uint16_t padbit(int k){
  switch(k){
  case AKEYCODE_DPAD_UP: return 0x0001; case AKEYCODE_DPAD_DOWN: return 0x0002; case AKEYCODE_DPAD_LEFT: return 0x0004; case AKEYCODE_DPAD_RIGHT: return 0x0008;
  case AKEYCODE_BUTTON_START: return 0x0010; case AKEYCODE_BUTTON_SELECT: return 0x0020; case AKEYCODE_BUTTON_THUMBL: return 0x0040; case AKEYCODE_BUTTON_THUMBR: return 0x0080;
  case AKEYCODE_BUTTON_L1: return 0x0100; case AKEYCODE_BUTTON_R1: return 0x0200;
  case AKEYCODE_BUTTON_A: return 0x1000; case AKEYCODE_BUTTON_B: return 0x2000; case AKEYCODE_BUTTON_X: return 0x4000; case AKEYCODE_BUTTON_Y: return 0x8000; }
  return 0;
}
static int16_t ax16(float v){ if(v>1) v=1; if(v<-1) v=-1; if(v>-0.12f && v<0.12f) v=0; return (int16_t)(v*32767); }
static FILE *g_inlog; static int g_inlog_n;
static void log_event(AInputEvent *e){
  if(!g_inlog || g_inlog_n > 5000) return; g_inlog_n++;
  int type = AInputEvent_getType(e), src = AInputEvent_getSource(e), dev = AInputEvent_getDeviceId(e);
  if(type == AINPUT_EVENT_TYPE_KEY) fprintf(g_inlog, "KEY dev %d src %08x code %d action %d meta %x\n", dev, src, AKeyEvent_getKeyCode(e), AKeyEvent_getAction(e), AKeyEvent_getMetaState(e));
  else if(type == AINPUT_EVENT_TYPE_MOTION){
    fprintf(g_inlog, "MOT dev %d src %08x act %x btn %x x %.1f y %.1f", dev, src, AMotionEvent_getAction(e), AMotionEvent_getButtonState(e), AMotionEvent_getX(e,0), AMotionEvent_getY(e,0));
    static const int axes[] = { AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y,
      AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_GAS, AMOTION_EVENT_AXIS_BRAKE, AMOTION_EVENT_AXIS_VSCROLL, AMOTION_EVENT_AXIS_HSCROLL,
      AMOTION_EVENT_AXIS_RELATIVE_X, AMOTION_EVENT_AXIS_RELATIVE_Y, AMOTION_EVENT_AXIS_RX, AMOTION_EVENT_AXIS_RY, AMOTION_EVENT_AXIS_PRESSURE };
    for(unsigned i = 0; i < sizeof axes/sizeof axes[0]; i++){ float v = AMotionEvent_getAxisValue(e, axes[i], 0); if(v != 0) fprintf(g_inlog, " a%d=%.3f", axes[i], v); }
    fprintf(g_inlog, "\n"); }
  fflush(g_inlog);
}
static int handle(AInputEvent *e){
  log_event(e);
  int type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
  int gamepad = (src & AINPUT_SOURCE_GAMEPAD)==AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK;
  if(type == AINPUT_EVENT_TYPE_KEY){
    int k = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e)==AKEY_EVENT_ACTION_DOWN;
    if(AKeyEvent_getAction(e)==AKEY_EVENT_ACTION_MULTIPLE) return 1;
    uint16_t b = padbit(k);
    if(b && (gamepad || k>=AKEYCODE_BUTTON_A)){
      pthread_mutex_lock(&pmu); g_pad_on=1; if(down) g_pad.buttons|=b; else g_pad.buttons&=~b; pthread_mutex_unlock(&pmu); return 1; }
    if(k==AKEYCODE_BACK){ qpush(AIN_KEY, 27, down); return 1; }   /* Back = Esc (pause menu) */
    uint32_t vk = key2vk(k); if(vk){ if(AKeyEvent_getRepeatCount(e)==0 || down) qpush(AIN_KEY, (int)vk, down); return 1; }
    return 0;
  }
  if(type == AINPUT_EVENT_TYPE_MOTION){
    if(gamepad){
      pthread_mutex_lock(&pmu); g_pad_on=1;
      g_pad.lx = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0));
      g_pad.ly = ax16(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0));
      g_pad.rx = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0));
      g_pad.ry = ax16(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));
      float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
      if(lt==0) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
      if(rt==0) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
      g_pad.lt = (uint8_t)(lt*255); g_pad.rt = (uint8_t)(rt*255);
      float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
      g_pad.buttons &= ~0x000f; if(hy<-0.5f) g_pad.buttons|=1; if(hy>0.5f) g_pad.buttons|=2; if(hx<-0.5f) g_pad.buttons|=4; if(hx>0.5f) g_pad.buttons|=8;
      pthread_mutex_unlock(&pmu); return 1;
    }
    /* mouse / Quest controller laser pointer: absolute position over the window, buttons from the button state */
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    float x = AMotionEvent_getX(e,0), y = AMotionEvent_getY(e,0);
    float rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_X, 0), ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
    if(rx!=0 || ry!=0) qpush(AIN_MOUSE, (int)rx, (int)ry);    /* captured physical mouse */
    else if(g_win){ int ww = ANativeWindow_getWidth(g_win), wh = ANativeWindow_getHeight(g_win);
      if(ww>0 && wh>0){ int nx = (int)(x*65536/ww), ny = (int)(y*65536/wh); if(nx<0) nx=0; if(ny<0) ny=0; if(nx>65535) nx=65535; if(ny>65535) ny=65535; qpush(AIN_MOUSEABS, nx, ny); } }
    static int lastbs; int bs = AMotionEvent_getButtonState(e);
    if(act==AMOTION_EVENT_ACTION_DOWN && !(bs & 7)) bs |= 1;      /* touch-style press (controller trigger) */
    if(act==AMOTION_EVENT_ACTION_UP || act==AMOTION_EVENT_ACTION_CANCEL) bs &= ~1;
    if(act==AMOTION_EVENT_ACTION_HOVER_MOVE || act==AMOTION_EVENT_ACTION_HOVER_ENTER) bs &= ~1;
    for(int k = 0; k < 3; k++){ int m = k==0 ? AMOTION_EVENT_BUTTON_PRIMARY : k==1 ? AMOTION_EVENT_BUTTON_SECONDARY : AMOTION_EVENT_BUTTON_TERTIARY;
      if((bs ^ lastbs) & m) qpush(AIN_BUTTON, k, (bs & m) != 0); }
    lastbs = bs;
    if(act==AMOTION_EVENT_ACTION_SCROLL){ float v = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0); if(v!=0) qpush(AIN_WHEEL, (int)(v*120), 0); }
    return 1;
  }
  return 0;
}
static AInputQueue *g_queue; static ALooper *g_in_looper;
static void *input_thread(void *p){
  g_in_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  for(;;){
    pthread_mutex_lock(&g_mu); AInputQueue *qq = g_queue; pthread_mutex_unlock(&g_mu);
    if(!qq){ usleep(20000); continue; }
    int ident = ALooper_pollOnce(100, NULL, NULL, NULL);
    if(ident == 1){ AInputEvent *e = NULL;
      while(AInputQueue_getEvent(qq, &e) >= 0){
        if(AInputQueue_preDispatchEvent(qq, e)) continue;
        int h = handle(e); AInputQueue_finishEvent(qq, e, h); } }
  }
  return NULL;
}
static void onInputQueueCreated(ANativeActivity *a, AInputQueue *qq){
  while(!g_in_looper) usleep(1000);
  pthread_mutex_lock(&g_mu); g_queue = qq; AInputQueue_attachLooper(qq, g_in_looper, 1, NULL, NULL); pthread_mutex_unlock(&g_mu);
}
static void onInputQueueDestroyed(ANativeActivity *a, AInputQueue *qq){
  pthread_mutex_lock(&g_mu); AInputQueue_detachLooper(qq); g_queue = NULL; pthread_mutex_unlock(&g_mu);
}

/* ---------------- audio ---------------- */
static AAudioStream *g_as;
/* Audio: AAudio pulls from a ring buffer in its data callback (it never stalls on our side); xaudio.c fills the ring
   and keeps ~120 ms queued (android_audio_queued / android_audio_target). Underruns play silence. */
#define RING_FR (48000/2)
static float g_ring[RING_FR*2]; static volatile uint32_t g_rd, g_wr;   /* frame counters (wrap via modulo) */
static volatile int64_t g_played, g_under; static int g_burst;
static FILE *g_outwav; static char g_files_audio[512];
#define OUTW_FR (48000*2)
static float g_outbuf[OUTW_FR*2]; static volatile uint32_t g_ow, g_owr;   /* copy of what the device consumed (debug) */
static void outwav_flush(void){ if(!g_outwav) return; uint32_t w = __atomic_load_n(&g_ow, __ATOMIC_ACQUIRE);
  while(g_owr != w){ uint32_t k = g_owr % OUTW_FR, n = w - g_owr; if(n > OUTW_FR - k) n = OUTW_FR - k; fwrite(&g_outbuf[2*k], 8, n, g_outwav); g_owr += n; } }
static aaudio_data_callback_result_t audio_cb(AAudioStream *st, void *ud, void *data, int32_t n){
  float *o = (float*)data; uint32_t rd = g_rd, wr = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE); uint32_t avail = wr - rd, take = avail < (uint32_t)n ? avail : (uint32_t)n;
  for(uint32_t i = 0; i < take; i++){ uint32_t k = (rd + i) % RING_FR; o[2*i] = g_ring[2*k]; o[2*i+1] = g_ring[2*k+1]; }
  if(take < (uint32_t)n){ memset(o + 2*take, 0, (n - take) * 2 * sizeof(float)); if(avail == 0 || take < (uint32_t)n) g_under++; }
  if(g_outwav){ for(int i = 0; i < n; i++){ uint32_t k = (g_ow + i) % OUTW_FR; g_outbuf[2*k] = o[2*i]; g_outbuf[2*k+1] = o[2*i+1]; } __atomic_store_n(&g_ow, g_ow + n, __ATOMIC_RELEASE); }
  __atomic_store_n(&g_rd, rd + take, __ATOMIC_RELEASE); g_played += n;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void audio_err(AAudioStream *st, void *ud, aaudio_result_t e);
static int audio_reopen;
static int audio_open(void){
  static int tried; if(g_as) return 1; if(tried && !audio_reopen) return 0; tried = 1; audio_reopen = 0;
  AAudioStreamBuilder *b; if(AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT); AAudioStreamBuilder_setChannelCount(b, 2); AAudioStreamBuilder_setSampleRate(b, 48000);
  { char lf[600]; snprintf(lf, sizeof lf, "%s/audio_lowlatency.txt", g_files_audio);
    int ll = access(lf, R_OK) == 0;   /* default: the normal (mixer) route; the low-latency fast path added a 21 ms echo on the Quest */
    AAudioStreamBuilder_setPerformanceMode(b, ll ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME); }
  AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL); AAudioStreamBuilder_setErrorCallback(b, audio_err, NULL);
  if(AAudioStreamBuilder_openStream(b, &g_as) != AAUDIO_OK){ g_as = NULL; AAudioStreamBuilder_delete(b); fprintf(stderr, "[audio] AAudio open failed\n"); return 0; }
  AAudioStreamBuilder_delete(b);
  g_burst = AAudioStream_getFramesPerBurst(g_as);
  aaudio_result_t r = AAudioStream_requestStart(g_as);
  fprintf(stderr, "[audio] AAudio callback stream: burst %d, capacity %d, rate %d, perf mode %d, sharing %d, start %d\n", g_burst, AAudioStream_getBufferCapacityInFrames(g_as),
    AAudioStream_getSampleRate(g_as), (int)AAudioStream_getPerformanceMode(g_as), (int)AAudioStream_getSharingMode(g_as), (int)r);
  { const char *w = getenv("BSI_AUDIO_WAV"); if(w){ char p2[700]; snprintf(p2, sizeof p2, "%s.out.raw", w); g_outwav = fopen(p2, "wb"); } }
  return 1;
}
static void audio_err(AAudioStream *st, void *ud, aaudio_result_t e){ fprintf(stderr, "[audio] stream error %d -> will reopen\n", (int)e); audio_reopen = 1; }
int android_audio_write(const float *frames, int n){   /* push into the ring (drops what does not fit) */
  outwav_flush();
  if(audio_reopen && g_as){ AAudioStream *old = g_as; g_as = NULL; AAudioStream_close(old); }
  if(!audio_open()) return 0;
  uint32_t wr = g_wr, rd = __atomic_load_n(&g_rd, __ATOMIC_ACQUIRE), space = RING_FR - (wr - rd); if((uint32_t)n > space) n = space;
  for(int i = 0; i < n; i++){ uint32_t k = (wr + i) % RING_FR; g_ring[2*k] = frames[2*i]; g_ring[2*k+1] = frames[2*i+1]; }
  __atomic_store_n(&g_wr, wr + n, __ATOMIC_RELEASE); return n;
}
int android_audio_target(void){ int t = 48000*12/100; if(t < g_burst + 2400) t = g_burst + 2400; if(t > RING_FR - 960) t = RING_FR - 960; return t; }
int android_audio_queued(void){   /* frames waiting in the ring; -1 = no output */
  if(!audio_open()) return -1;
  { static time_t last; time_t t = time(0); if(t - last >= 10){ last = t;
      fprintf(stderr, "[audio] state %d played %lld underruns %lld queued %u\n", (int)AAudioStream_getState(g_as), (long long)g_played, (long long)g_under, g_wr - g_rd); } }
  return (int)(g_wr - g_rd);
}

/* ---------------- game thread ---------------- */
static char g_files[512], g_extfiles[512], g_readtest[900];
#include <errno.h>

/* ---------------- driver / library setup ---------------- */
#define ADRENOTOOLS_DRIVER_CUSTOM 1
typedef void *(*adrenotools_open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
static int copy_file(const char *src, const char *dst){
  struct stat a, b; if(stat(src, &a) != 0) return -1;
  if(stat(dst, &b) == 0 && a.st_size == b.st_size && b.st_mtime >= a.st_mtime) return 0;   /* already up to date */
  FILE *i = fopen(src, "rb"); if(!i) return -1;
  char tmp[700]; snprintf(tmp, sizeof tmp, "%s.tmp", dst);
  FILE *o = fopen(tmp, "wb"); if(!o){ fclose(i); return -1; }
  static char buf[1<<20]; size_t n; int ok = 1;
  while((n = fread(buf, 1, sizeof buf, i)) > 0) if(fwrite(buf, 1, n, o) != n){ ok = 0; break; }
  fclose(i); if(fclose(o) != 0) ok = 0;
  if(!ok){ unlink(tmp); return -1; }
  chmod(tmp, 0755); return rename(tmp, dst);
}
/* Fast iteration: DXVK libraries placed in /sdcard/BSI/override/ replace the packaged ones (copied to internal storage, since
   Android refuses to load code from shared storage). */
static void setup_override_libs(void){
  const char *names[] = { "libdxvk_dxgi.so", "libdxvk_d3d11.so" };
  char src[700], dir[600], dst[700]; int have = 1;
  for(int k = 0; k < 2; k++){ snprintf(src, sizeof src, "%s/override/%s", g_files, names[k]); if(access(src, R_OK) != 0) have = 0; }
  if(!have) return;
  snprintf(dir, sizeof dir, "%s/override", g_act->internalDataPath); mkdir(g_act->internalDataPath, 0777); mkdir(dir, 0777);
  for(int k = 0; k < 2; k++){
    snprintf(src, sizeof src, "%s/override/%s", g_files, names[k]); snprintf(dst, sizeof dst, "%s/%s", dir, names[k]);
    if(copy_file(src, dst) != 0){ fprintf(stderr, "[android] override copy failed: %s\n", src); return; } }
  setenv("BSI_DXVK_DIR", dir, 1); fprintf(stderr, "[android] using override DXVK libraries from %s/override\n", g_files);
}
/* GPU driver: the Quest's built-in Adreno driver lacks features DXVK needs, so by default we load the Mesa Turnip driver
   (packaged as libvulkan_turnip.so, or /sdcard/BSI/turnip/vulkan.ad07xx.so to try another build) through adrenotools.
   Create /sdcard/BSI/use_stock_driver.txt to use the built-in driver instead. */
static void setup_turnip(void){
  char flag[600]; snprintf(flag, sizeof flag, "%s/use_stock_driver.txt", g_files);
  if(access(flag, R_OK) == 0){ fprintf(stderr, "[android] using the built-in Vulkan driver (use_stock_driver.txt)\n"); return; }
  char libdir[600]; snprintf(libdir, sizeof libdir, "%s/", getenv("BSI_DXVK_DIR"));
  { Dl_info di; if(dladdr((void*)setup_turnip, &di) && di.dli_fname){ snprintf(libdir, sizeof libdir, "%s", di.dli_fname); char *sl = strrchr(libdir, '/'); if(sl) sl[1] = 0; } }
  char drvdir[600], drvname[128] = "libvulkan_turnip.so", src[700];
  snprintf(drvdir, sizeof drvdir, "%s", libdir);
  snprintf(src, sizeof src, "%s/turnip/vulkan.ad07xx.so", g_files);
  { /* driver picker: /sdcard/BSI/driver.txt names a folder in /sdcard/BSI/drivers/<name>/vulkan.ad07xx.so */
    char df[600]; snprintf(df, sizeof df, "%s/driver.txt", g_files); FILE *f = fopen(df, "r");
    if(f){ char nm[128] = ""; if(fgets(nm, sizeof nm, f)){ nm[strcspn(nm, "\r\n \t")] = 0;
        if(nm[0]){ char cand[700]; snprintf(cand, sizeof cand, "%s/drivers/%s/vulkan.ad07xx.so", g_files, nm);
          if(access(cand, R_OK) == 0) snprintf(src, sizeof src, "%s", cand); else fprintf(stderr, "[android] driver.txt names '%s' but %s is missing\n", nm, cand); } }
      fclose(f); } }
  { char sf[600]; snprintf(sf, sizeof sf, "%s/session.txt", g_files); FILE *f = fopen(sf, "a"); if(f){ fprintf(f, "driver: %s\n", access(src, R_OK) == 0 ? src : "packaged libvulkan_turnip.so"); fclose(f); } }
  if(access(src, R_OK) == 0){
    char d[600], dst[700]; snprintf(d, sizeof d, "%s/turnip", g_act->internalDataPath); mkdir(g_act->internalDataPath, 0777); mkdir(d, 0777);
    snprintf(dst, sizeof dst, "%s/vulkan.ad07xx.so", d);
    if(copy_file(src, dst) == 0){ snprintf(drvdir, sizeof drvdir, "%s/", d); snprintf(drvname, sizeof drvname, "vulkan.ad07xx.so");
      fprintf(stderr, "[android] using Turnip from %s\n", src); }
  }
  char chk[800]; snprintf(chk, sizeof chk, "%s%s", drvdir, drvname);
  if(access(chk, R_OK) != 0){ fprintf(stderr, "[android] no Turnip driver found (%s), using the built-in driver\n", chk); return; }
  char atp[700]; snprintf(atp, sizeof atp, "%slibadrenotools.so", libdir);
  void *at = dlopen(atp, RTLD_NOW); adrenotools_open_fn adrenotools_open_libvulkan = at ? (adrenotools_open_fn)dlsym(at, "adrenotools_open_libvulkan") : NULL;
  if(!adrenotools_open_libvulkan){ fprintf(stderr, "[android] libadrenotools missing (%s): using the built-in driver\n", dlerror()); return; }
  void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, libdir, drvdir, drvname, NULL, NULL);
  if(!h){ fprintf(stderr, "[android] adrenotools could not load %s%s (hooks in %s): using the built-in driver\n", drvdir, drvname, libdir); return; }
  g_vulkan_loader = h;
  char hs[32]; snprintf(hs, sizeof hs, "%llx", (unsigned long long)(uintptr_t)h); setenv("BSI_VULKAN_HANDLE", hs, 1);
  fprintf(stderr, "[android] Turnip driver ready: %s%s\n", drvdir, drvname);
}
/* keep the last 3 sessions' logs/profiles/audio captures: <files>/sessions/prev1 (newest) .. prev3 */
static void rotate_sessions(const char *files){
  static const char *names[] = {"bsi.log", "profile.txt", "input.log", "game_audio.wav", "game_audio.wav.out.raw", "dsp_probe.txt", "session.txt", "volog.txt", "bench.txt", "memwatch.txt", "leakwatch.txt", "timeline.csv", 0};
  char d[700], a[800], b[800];
  snprintf(d, sizeof d, "%s/sessions", files); mkdir(d, 0777);
  for(int k = 0; names[k]; k++){ snprintf(a, sizeof a, "%s/prev3/%s", d, names[k]); unlink(a); }
  snprintf(a, sizeof a, "%s/prev3", d); rmdir(a);
  snprintf(a, sizeof a, "%s/prev2", d); snprintf(b, sizeof b, "%s/prev3", d); rename(a, b);
  snprintf(a, sizeof a, "%s/prev1", d); snprintf(b, sizeof b, "%s/prev2", d); rename(a, b);
  snprintf(a, sizeof a, "%s/prev1", d); mkdir(a, 0777);
  for(int k = 0; names[k]; k++){ snprintf(a, sizeof a, "%s/%s", files, names[k]); snprintf(b, sizeof b, "%s/prev1/%s", d, names[k]); rename(a, b); }
  snprintf(a, sizeof a, "%s/session.txt", files); FILE *f = fopen(a, "w");
  if(f){ time_t t = time(0); fprintf(f, "build %s %s, started %s", BSI_VERSION, __DATE__, ctime(&t)); fclose(f); }
}
extern volatile int g_game_tid;
#include <sys/syscall.h>
/* Live console (from the GitHub PR #1, merged): a tiny line-based TCP server on 127.0.0.1:27183, started only when
   /sdcard/BSI/console.txt exists. Reach it from a PC with: adb forward tcp:27183 tcp:27183 && nc localhost 27183
   Commands: fps | threads | set <key> <value> | dump bench | help | quit
   Loopback only (nothing on the Wi-Fi can connect); one client at a time. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <stdarg.h>
extern volatile long g_br_presents;
static char g_console_files[512];

static void con_printf(int fd, const char *fmt, ...){
  char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  if(n > (int)sizeof b - 1) n = sizeof b - 1;
  for(int o = 0; o < n; ){ ssize_t w = send(fd, b + o, n - o, MSG_NOSIGNAL); if(w <= 0) return; o += (int)w; }
}
static void con_fps(int fd){ /* presents counted over a 1 s window */
  long a = g_br_presents; struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  struct timespec sl = { 1, 0 }; nanosleep(&sl, NULL);
  long b = g_br_presents; clock_gettime(CLOCK_MONOTONIC, &t1);
  double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
  con_printf(fd, "fps %.1f (%ld presents in %.2f s, total %ld)\n", dt > 0 ? (b - a) / dt : 0.0, b - a, dt, b);
}
static void con_threads(int fd){
  DIR *d = opendir("/proc/self/task"); if(!d){ con_printf(fd, "error: %s\n", strerror(errno)); return; }
  long hz = sysconf(_SC_CLK_TCK); if(hz <= 0) hz = 100;
  con_printf(fd, "%7s %-16s %c %9s %5s\n", "tid", "name", 'S', "cpu_s", "cpu");
  struct dirent *e; int n = 0;
  while((e = readdir(d))){
    if(!isdigit((unsigned char)e->d_name[0])) continue;
    char p[300], buf[512]; snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
    FILE *f = fopen(p, "r"); if(!f) continue;
    size_t len = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[len] = 0;
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')'); if(!lp || !rp) continue;
    *rp = 0; char st = 0; unsigned long ut = 0, stm = 0; int cpu = -1;
    /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... (field 39 = processor) */
    char *q = rp + 2; st = *q;
    int fld = 3; for(char *s = q; *s; s++) if(*s == ' '){ fld++; if(fld == 14) ut = strtoul(s + 1, NULL, 10); if(fld == 15) stm = strtoul(s + 1, NULL, 10); if(fld == 39) cpu = atoi(s + 1); }
    con_printf(fd, "%7s %-16s %c %9.2f %5d%s\n", e->d_name, lp + 1, st, (double)(ut + stm) / hz, cpu, atoi(e->d_name) == g_game_tid ? " <- game" : "");
    n++;
  }
  closedir(d); con_printf(fd, "%d threads\n", n);
}
static void con_dump_bench(int fd){
  char p[600]; snprintf(p, sizeof p, "%s/bench.txt", g_console_files);
  FILE *f = fopen(p, "r"); if(!f){ con_printf(fd, "no bench file yet (%s: %s)\n", p, strerror(errno)); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); long from = sz > 8192 ? sz - 8192 : 0; fseek(f, from, SEEK_SET); /* last 8 KB */
  char buf[1024]; size_t n; if(from) con_printf(fd, "... (showing last 8 KB of %ld)\n", sz);
  while((n = fread(buf, 1, sizeof buf, f)) > 0) send(fd, buf, n, MSG_NOSIGNAL);
  fclose(f);
}
/* "set <key> <value>":
   - dxvk.* / dxgi.* / d3d9.* ... (key contains '.'): written into dxvk.conf, applied the next time the game starts (DXVK reads it once)
   - BSI_* / DXVK_* : written as KEY=VALUE into <files>/env.txt, applied by the env.txt loader at the next game start
     (never setenv() from here: other threads call getenv() concurrently, which is not thread-safe) */
static int con_set(int fd, const char *key, const char *val){
  for(const char *c = key; *c; c++) if(!isalnum((unsigned char)*c) && *c != '.' && *c != '_'){ con_printf(fd, "error: bad key\n"); return -1; }
  for(const char *c = val; *c; c++) if(!isalnum((unsigned char)*c) && !strchr(".,_-+:/", *c)){ con_printf(fd, "error: bad value (allowed: letters digits . , _ - + : /)\n"); return -1; }
  if(!*key || !*val){ con_printf(fd, "usage: set <key> <value>\n"); return -1; }
  int isenv = !strncmp(key, "BSI_", 4) || !strncmp(key, "DXVK_", 5) || !strncmp(key, "MESA_", 5) || !strncmp(key, "TU_", 3)
              || !strncmp(key, "IR3_", 4) || !strncmp(key, "FD_", 3) || !strncmp(key, "tu_", 3);   /* DXVK_TUNING.md */
  if(isenv && strchr(key, '.')){ con_printf(fd, "error: bad env name\n"); return -1; }
  if(!isenv && !strchr(key, '.')){ con_printf(fd, "error: key must be BSI_*, DXVK_* or a dotted dxvk.conf option such as dxgi.maxFrameRate\n"); return -1; }
  char cf[600], tf[610]; snprintf(cf, sizeof cf, "%s/%s", g_console_files, isenv ? "env.txt" : "dxvk.conf"); snprintf(tf, sizeof tf, "%s.tmp", cf);
  const char *sep = isenv ? "=" : " = "; /* env.txt: KEY=VALUE; dxvk.conf: key = value */
  FILE *in = fopen(cf, "r"), *out = fopen(tf, "w"); if(!out){ if(in) fclose(in); con_printf(fd, "error: cannot write %s: %s\n", tf, strerror(errno)); return -1; }
  char line[512]; int done = 0; size_t kl = strlen(key);
  while(in && fgets(line, sizeof line, in)){
    const char *s = line; while(*s == ' ' || *s == '\t') s++;
    if(!strncmp(s, key, kl) && (s[kl] == ' ' || s[kl] == '=' || s[kl] == '\t')){ if(!done) fprintf(out, "%s%s%s\n", key, sep, val); done = 1; continue; }
    fputs(line, out); }
  if(!done) fprintf(out, "%s%s%s\n", key, sep, val);
  if(in) fclose(in);
  int bad = fclose(out) != 0 || rename(tf, cf) != 0;
  if(bad){ con_printf(fd, "error: updating %s failed: %s\n", cf, strerror(errno)); unlink(tf); return -1; }
  con_printf(fd, "ok: %s%s%s saved to %s (takes effect on next game start)\n", key, sep, val, cf); return 0;
}
static int con_line(int fd, char *l){ /* returns 1 to close the connection */
  char *tok[4]; int nt = 0; for(char *s = strtok(l, " \t\r\n"); s && nt < 4; s = strtok(NULL, " \t\r\n")) tok[nt++] = s;
  if(!nt) return 0;
  if(!strcmp(tok[0], "fps")) con_fps(fd);
  else if(!strcmp(tok[0], "threads")) con_threads(fd);
  else if(!strcmp(tok[0], "set")){ if(nt == 3) con_set(fd, tok[1], tok[2]); else con_printf(fd, "usage: set <key> <value>\n"); }
  else if(!strcmp(tok[0], "dump") && nt == 2 && !strcmp(tok[1], "bench")) con_dump_bench(fd);
  else if(!strcmp(tok[0], "quit") || !strcmp(tok[0], "exit")) return 1;
  else con_printf(fd, "commands: fps | threads | set <key> <value> | dump bench | quit\n");
  return 0;
}
static void *console_thread(void *p){
  int ls = socket(AF_INET, SOCK_STREAM, 0); if(ls < 0) return NULL;
  int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(27183); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0){ fprintf(stderr, "[console] bind/listen failed: %s\n", strerror(errno)); close(ls); return NULL; }
  fprintf(stderr, "[console] listening on 127.0.0.1:27183 (adb forward tcp:27183 tcp:27183)\n");
  for(;;){
    int c = accept(ls, NULL, NULL); if(c < 0){ if(errno == EINTR) continue; break; }
    struct timeval tv = { 300, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); /* drop idle clients */
    con_printf(c, "BSI-Quest console (%s). commands: fps | threads | set <key> <value> | dump bench | quit\n> ", BSI_VERSION);
    char buf[512]; int len = 0, quit = 0;
    while(!quit){
      ssize_t n = recv(c, buf + len, sizeof buf - 1 - len, 0); if(n <= 0) break; len += (int)n;
      char *nl; while(!quit && (nl = memchr(buf, '\n', len))){
        *nl = 0; quit = con_line(c, buf); int rest = len - (int)(nl + 1 - buf); memmove(buf, nl + 1, rest); len = rest;
        if(!quit) con_printf(c, "> "); }
      if(len >= (int)sizeof buf - 1){ con_printf(c, "error: line too long\n"); len = 0; }
    }
    close(c);
  }
  close(ls); return NULL;
}
static void console_start(const char *files){
  if(access("/sdcard/BSI/console.txt", F_OK) != 0) return;
  snprintf(g_console_files, sizeof g_console_files, "%s", files);
  pthread_t t; if(pthread_create(&t, NULL, console_thread, NULL) == 0) pthread_detach(t);
}

#include <ftw.h>
static long g_cache_bytes, g_cache_files;
static int cache_sum(const char *path, const struct stat *st, int type, struct FTW *f){ (void)path; (void)f;
  if(type == FTW_F){ g_cache_bytes += st->st_size; g_cache_files++; } return 0; }
static void *game_thread(void *p){
  g_game_tid = (int)syscall(SYS_gettid);
  char root[600], user[600], log[600], bin[700], exe[800];
  snprintf(root, sizeof root, "%s/BioShock Infinite", g_files);
  { /* game data in the app's own external folder (Android/data/<pkg>/files/game) is read without the FUSE layer: prefer it */
    char alt[600], exe[700]; snprintf(alt, sizeof alt, "%s/game", g_extfiles); snprintf(exe, sizeof exe, "%s/Binaries/Win32/BioShockInfinite.exe", alt);
    if(g_extfiles[0] && access(exe, R_OK) == 0) snprintf(root, sizeof root, "%s", alt);
    char rt[700]; snprintf(rt, sizeof rt, "%s/readtest.txt", alt); FILE *tf = fopen(rt, "r"); char tb[64] = "";
    if(tf){ if(!fgets(tb, sizeof tb, tf)) tb[0] = 0; fclose(tf); }
    snprintf(g_readtest, sizeof g_readtest, "[android] app-folder read test %s: %s %s", rt, tf ? "OK" : "FAILED", tf ? tb : strerror(errno)); }
  snprintf(user, sizeof user, "%s/user", g_files); mkdir(user, 0777);
  rotate_sessions(g_files);
  snprintf(log, sizeof log, "%s/bsi.log", g_files);
  freopen(log, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0);
  fprintf(stderr, "[android] BSI-Quest starting, game data: %s\n%s\n", root, g_readtest); snprintf(g_files_audio, sizeof g_files_audio, "%s", g_files);
  setenv("BSI_ROOT", root, 1); setenv("BSI_USER", user, 1);
  setenv("BSI_DXVK", "1", 1);
  { /* DXVK libraries are packaged next to libbsi.so (the APK's native library dir) */
    Dl_info di; char d[600] = "."; if(dladdr((void*)game_thread, &di) && di.dli_fname){ snprintf(d, sizeof d, "%s", di.dli_fname); char *sl = strrchr(d, '/'); if(sl) *sl = 0; }
    setenv("BSI_DXVK_DIR", d, 1); fprintf(stderr, "[android] native libs: %s\n", d); }
  setup_override_libs();
  setup_turnip();
  setenv("BSI_NOTRACE", "1", 1); setenv("DXVK_WSI_DRIVER", "Android", 1);
  { char lg[600]; snprintf(lg, sizeof lg, "%s/bsi_imports.log", g_files); setenv("BSI_LOG", lg, 1); }
  { char dl[600]; snprintf(dl, sizeof dl, "%s/dxvk", g_files); mkdir(dl, 0777); setenv("DXVK_LOG_PATH", dl, 1); setenv("DXVK_STATE_CACHE_PATH", dl, 1); setenv("DXVK_SHADER_CACHE_PATH", dl, 1); }
  snprintf(bin, sizeof bin, "%s/Binaries/Win32", root);
  if(chdir(bin) != 0){ fprintf(stderr, "[android] game folder missing: %s\n", bin); return NULL; }
  snprintf(exe, sizeof exe, "%s/BioShockInfinite.exe", bin);
  { FILE *a = fopen(exe, "rb"); if(!a){ fprintf(stderr, "[android] %s not found - copy the game to %s\n", exe, root); return NULL; } fclose(a); }
  /* command line: user can put extra arguments in <files>/args.txt (default windowed at the panel size) */
  ANativeWindow *w0 = android_window();
  int gw = 1920, gh = 1080; (void)w0;   /* render resolution; DXVK scales it to the panel */
  /* perf73: in stereo the XR side chose the VR size (vr_stereo.txt res=, default auto) and made the surface that size;
     resolution.txt is the FLAT size and is not used then (b72: a flat 1600x900 there gave 533x900 eyes) */
  extern int xr_stereo_game_size(int *w, int *h) __attribute__((weak)); int vr_sz = 0;
  if(g_xr_mode && xr_stereo_game_size && xr_stereo_game_size(&gw, &gh)) vr_sz = 1;
  { char rf[600]; snprintf(rf, sizeof rf, "%s/resolution.txt", g_files); FILE *f = fopen(rf, "r"); if(f){ int a, b; if(fscanf(f, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 200){
      if(!vr_sz){ gw = a; gh = b; } else fprintf(stderr, "[android] stereo: game resolution %dx%d from the VR sizing (vr_stereo.txt res=, default auto); resolution.txt %dx%d is the flat size, not used in VR\n", gw, gh, a, b); } fclose(f); } }
  if(vr_sz) fprintf(stderr, "[android] stereo: game resolution %dx%d (VR surface size)\n", gw, gh);
  char args[512]; snprintf(args, sizeof args, "-windowed -ResX=%d -ResY=%d", gw, gh); { char af[600]; snprintf(af, sizeof af, "%s/args.txt", g_files); FILE *f = fopen(af, "r");
    if(f){ if(!fgets(args, sizeof args, f)) args[0] = 0; fclose(f); char *nl = strchr(args, '\n'); if(nl) *nl = 0;
      if(vr_sz){ /* perf73: the surface is the VR size: args.txt keeps its other arguments, -ResX/-ResY are ours */
        char t[512]; int o = 0; t[0] = 0; char *sp = NULL; for(char *tk = strtok_r(args, " ", &sp); tk; tk = strtok_r(NULL, " ", &sp))
          if(strncmp(tk, "-ResX=", 6) && strncmp(tk, "-ResY=", 6) && o < (int)sizeof t - 2) o += snprintf(t + o, sizeof t - o, "%s%s", o ? " " : "", tk);
        snprintf(args, sizeof args, "%s -ResX=%d -ResY=%d", t, gw, gh); fprintf(stderr, "[android] stereo: args.txt with the VR size: %s\n", args); } } }
  setenv("BSI_ARGS", args, 1);
  if(g_xr_mode) setenv("BSI_CURSOR_PARK", "1", 1);   /* controller play: keep the hidden mouse cursor off the menus */
  { const char *r = strstr(args, "-ResX="), *r2 = strstr(args, "-ResY="); char t[16];
    if(r){ snprintf(t, sizeof t, "%d", atoi(r+6)); setenv("BSI_GAME_W", t, 1); } if(r2){ snprintf(t, sizeof t, "%d", atoi(r2+6)); setenv("BSI_GAME_H", t, 1); } }
  { /* DXVK tuning for the Quest (edit /sdcard/BSI/dxvk.conf to experiment; delete it to get these defaults back) */
    char cf[600]; snprintf(cf, sizeof cf, "%s/dxvk.conf", g_files);
    const char *tag = "# BSI-Quest DXVK settings v3"; int fresh = 1;
    { FILE *f = fopen(cf, "r"); if(f){ char l[128] = ""; if(fgets(l, sizeof l, f) && strncmp(l, "# BSI-Quest DXVK settings", 25) == 0 && strncmp(l, tag, strlen(tag)) != 0) fresh = 1; else fresh = 0; fclose(f); } }
    if(fresh){ FILE *f = fopen(cf, "w"); if(f){ fprintf(f,
      "%s (see https://github.com/doitsujin/dxvk/blob/master/dxvk.conf)\n"
      "# shader compiler threads (they run at idle priority during play, so they mostly use spare time)\n"
      "dxvk.numCompilerThreads = 3\n"
      "# keep compiled pipeline libraries (True made DXVK recompile them on first use = hitches)\n"
      "dxvk.trackPipelineLifetime = False\n"
      "# fast-linked pipeline libraries only: no extra background optimised recompiles competing for the 3 cores\n"
      "dxvk.enableGraphicsPipelineLibrary = True\n"
      "# report 2 GB of video memory so the game keeps its texture pool modest (Quest: 5.75 GB per app, shared with the GPU)\n"
      "dxgi.maxDeviceMemory = 2048\n"
      "dxgi.maxFrameLatency = 1\n"
      "# low-latency frame pacing (less input lag)\n"
      "dxvk.latencySleep = True\n"
      "# even frame pacing on the 120 Hz display: every game frame is shown exactly twice (60) - try 40 if it dips a lot\n"
      "dxgi.maxFrameRate = 60\n", tag); fclose(f); } }
    setenv("DXVK_CONFIG_FILE", cf, 1); }
  { char mc[600]; snprintf(mc, sizeof mc, "%s/mesa_cache", g_act->internalDataPath); mkdir(mc, 0777);
    setenv("MESA_SHADER_CACHE_DIR", mc, 1); setenv("MESA_SHADER_CACHE_DISABLE", "false", 1); setenv("MESA_SHADER_CACHE_MAX_SIZE", "1G", 1);
    /* v0.8.5: log how big the shader cache already is, to tell first-run shader-compile hitches from other stalls */
    extern int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
    g_cache_bytes = 0; g_cache_files = 0; nftw(mc, cache_sum, 16, 1 /* FTW_PHYS */);
    fprintf(stderr, "[android] mesa shader cache %s: %ld files, %.1f MB\n", mc, g_cache_files, g_cache_bytes / 1048576.0); }
  { char af[600]; snprintf(af, sizeof af, "%s/record_audio.txt", g_files); if(access(af, R_OK) == 0){ char w[600]; snprintf(w, sizeof w, "%s/game_audio.wav", g_files); setenv("BSI_AUDIO_WAV", w, 1); } }
  { /* v0.8.8: DXVK HUD OFF by default (Fable G4: the HUD costs a full-screen composition pass). <files>/hud.txt turns it on
       (its first line, if any, is the DXVK_HUD value; default fps,frametimes,compiler); nohud.txt still forces it off. */
    char hf[600], nf[600]; snprintf(hf, sizeof hf, "%s/hud.txt", g_files); snprintf(nf, sizeof nf, "%s/nohud.txt", g_files);
    if(access(hf, R_OK) == 0 && access(nf, R_OK) != 0){ char v[128] = "fps,frametimes,compiler"; FILE *f = fopen(hf, "r");
      if(f){ char l[128]; if(fgets(l, sizeof l, f)){ l[strcspn(l, "\r\n")] = 0; if(l[0]) snprintf(v, sizeof v, "%s", l); } fclose(f); }
      setenv("DXVK_HUD", v, 0); fprintf(stderr, "[android] DXVK HUD on (hud.txt): %s\n", v); } }
  { char vl[600]; snprintf(vl, sizeof vl, "%s/volog.txt", g_files); setenv("BSI_VOLOG", vl, 1); }
  { char mf[600]; snprintf(mf, sizeof mf, "%s/motionblur.txt", g_files); setenv("BSI_MB_FILE", mf, 0); }   /* mb_off.c: motion blur OFF unless this file says 1 */
  { char bf[600]; snprintf(bf, sizeof bf, "%s/bloom.txt", g_files); setenv("BSI_BLOOM_FILE", bf, 0); }   /* mb_off.c: bloom ON unless this file says 0 */
  { char bl[600]; snprintf(bl, sizeof bl, "%s/bench.txt", g_files); setenv("BSI_BENCH", bl, 1); }
  { char il[600]; snprintf(il, sizeof il, "%s/input.log", g_files); g_inlog = fopen(il, "w"); }
  { /* skip the Bink startup logo movies (hle_file.c BSI_SKIP_INTRO). v0.8.7: OFF by default on the Quest - skipping them left the
       screen black forever on v0.8.6 (fine in the cloud). <files>/skip_intro.txt turns it on for testing a fix. */
    char kf[600]; snprintf(kf, sizeof kf, "%s/skip_intro.txt", g_files); int skip = access(kf, R_OK) == 0;
    if(skip) setenv("BSI_SKIP_INTRO", "1", 1); fprintf(stderr, "[android] startup movies: %s\n", skip ? "skipped (skip_intro.txt)" : "kept"); }
  { /* shader dump / override (bridge.c br_shader_create): <files>/shader_dump.txt = write every distinct shader's DXBC once to
       <files>/shaders/dump (+ shaders.txt index); a <files>/shaders/override folder = its <stage>_<hash16>.dxbc files replace the
       game's shaders at creation. Both only set the env if env.txt (below) does not; no file/folder = no change. */
    char sd[600], t[620]; snprintf(sd, sizeof sd, "%s/shaders", g_files);
    snprintf(t, sizeof t, "%s/shader_dump.txt", g_files);
    if(access(t, R_OK) == 0){ mkdir(sd, 0777); snprintf(t, sizeof t, "%s/dump", sd); mkdir(t, 0777); setenv("BSI_SHADER_DUMP_DIR", t, 0); fprintf(stderr, "[android] shader_dump.txt: dumping shaders to %s\n", t); }
    snprintf(t, sizeof t, "%s/override", sd);
    if(access(t, R_OK) == 0){ setenv("BSI_SHADER_OVERRIDE_DIR", t, 0); fprintf(stderr, "[android] shader overrides from %s\n", t); } }
  /* <files>/env.txt: optional KEY=VALUE lines (BSI_*, DXVK_*, MESA_*, TU_*, IR3_*, FD_*, tu_* only; '#' comments) for on-device A/B tests of
     env-gated features, e.g. BSI_BRIDGE_THREAD=1. Applied last, so they override the defaults above. */
  { char ef[600]; snprintf(ef, sizeof ef, "%s/env.txt", g_files); FILE *f = fopen(ef, "r");
    if(f){ char l[512]; while(fgets(l, sizeof l, f)){ char *e = l + strcspn(l, "\r\n"); *e = 0; char *k = l; while(*k == ' ' || *k == '\t') k++;
        char *eq = strchr(k, '='); if(!eq || *k == '#') continue; *eq = 0;
        /* DXVK_TUNING.md: TU_* (TU_DEBUG, TU_AUTOTUNE_ALGO, TU_AUTOTUNE_FLAGS), IR3_* and FD_* (Mesa freedreno debug) and lower-case Turnip
           driconf names (tu_autotune_algorithm, tu_allow_concurrent_binning): Mesa lets an environment variable named
           like a driconf option override it, including the built-in "DXVK|vkd3d" engine entry (prefer_sysmem). */
        if(strncmp(k, "BSI_", 4) && strncmp(k, "DXVK_", 5) && strncmp(k, "MESA_", 5) && strncmp(k, "TU_", 3)
           && strncmp(k, "IR3_", 4) && strncmp(k, "FD_", 3) && strncmp(k, "tu_", 3)) continue;
        setenv(k, eq + 1, 1); fprintf(stderr, "[android] env.txt: %s=%s\n", k, eq + 1); }
      fclose(f); } }
  { const char *probe = getenv("BSI_VK_PROBE");
    if(probe && strcmp(probe, "1") == 0) bsi_vk_caps_probe(g_vulkan_loader); }
  { /* v0.9.1 final-copy defaults: the cloud-validated speed switches are ON unless env.txt already set them (e.g. BSI_HLEFAST=0
       turns one off again). <files>/nodefaults.txt = none of them (the exact v0.9.0 behaviour). BSI_RHI=native is skipped when
       env.txt asks for BSI_BRIDGE_THREAD or BSI_BRFAST, which it cannot run with. */
    char nd[600]; snprintf(nd, sizeof nd, "%s/nodefaults.txt", g_files);
    if(access(nd, R_OK) == 0) fprintf(stderr, "[android] defaults: off (nodefaults.txt)\n");
    else {
      static const char *const defs[][2] = { {"BSI_HLEFAST","1"}, {"BSI_SYNCFAST","1"}, {"BSI_AUDIO_NATIVE","all"},
        {"BSI_DIRCACHE","1"}, {"BSI_PREWARM","1"}, {"BSI_BOOST_BURST","1"}, {"BSI_RHI","native"} };
      char msg[400]; int n = 0; msg[0] = 0;
      for(unsigned i = 0; i < sizeof defs / sizeof defs[0]; i++){
        if(getenv(defs[i][0])) continue;
        if(!strcmp(defs[i][0], "BSI_RHI") && (getenv("BSI_BRIDGE_THREAD") || getenv("BSI_BRFAST"))) continue;
        setenv(defs[i][0], defs[i][1], 0);
        n += snprintf(msg + n, sizeof msg - n, " %s=%s", defs[i][0], defs[i][1]); if(n >= (int)sizeof msg) n = sizeof msg - 1; }
      fprintf(stderr, "[android] defaults:%s\n", msg[0] ? msg : " (all set by env.txt)"); } }
  { /* AO switch (notes/GTAO.md §10). <files>/ao/{k32id,t16id,gtao_match,gtao_improved} = DXVK BSI_SHADER_ALT sets 1..4 for the
       game's HDAO compute passes (DXVK patches dxvk-bsi-shader-replace + -shader-alt + -ao-switch); <files>/ao_sel.txt = the active
       set, re-read by DXVK whenever the file changes, so it switches live (adb shell echo 0 > /sdcard/BSI/ao_sel.txt):
         0 = game HDAO   1 = k32id   2 = t16id (both bit-exact HDAO, faster)   3 = GTAO match (default)   4 = GTAO improved
       All four paths are always passed so the numbers never shift; a missing folder (or one whose shaders do not match this
       device) means that number runs the game's HDAO. ao_sel.txt is created with 3 if missing; a missing/unreadable file while
       running = 0. No <files>/ao folder = nothing set (game HDAO). Runs after env.txt and never overrides it: BSI_SHADER_ALT /
       BSI_SHADER_SEL lines there win. */
    static const char *ao_names[4] = { "k32id", "t16id", "gtao_match", "gtao_improved" };
    char ad[600]; struct stat ast; snprintf(ad, sizeof ad, "%s/ao", g_files);
    if(stat(ad, &ast) == 0 && S_ISDIR(ast.st_mode)){
      char alt[3000] = "", have[160] = "", sf[600]; int nh = 0, sel = 0;
      for(int k = 0; k < 4; k++){ char d[700]; size_t n = strlen(alt); snprintf(d, sizeof d, "%s/%s", ad, ao_names[k]);
        snprintf(alt + n, sizeof alt - n, "%s%s", k ? ":" : "", d);
        if(stat(d, &ast) == 0 && S_ISDIR(ast.st_mode)){ n = strlen(have); snprintf(have + n, sizeof have - n, "%s%d=%s", nh++ ? " " : "", k + 1, ao_names[k]); } }
      snprintf(sf, sizeof sf, "%s/ao_sel.txt", g_files);
      const char *ea = getenv("BSI_SHADER_ALT"), *es = getenv("BSI_SHADER_SEL");
      if(!es && access(sf, F_OK) != 0){ FILE *f = fopen(sf, "w"); if(f){ fputs("3\n", f); fclose(f); fprintf(stderr, "[ao] created %s with 3 (GTAO match)\n", sf); } }
      if(!ea) setenv("BSI_SHADER_ALT", alt, 0);
      if(!es) setenv("BSI_SHADER_SEL", sf, 0);
      { FILE *f = fopen(es ? es : sf, "r"); if(f){ if(fscanf(f, "%d", &sel) != 1) sel = 0; fclose(f); } }
      const char *nm = sel == 0 ? "game HDAO" : (sel >= 1 && sel <= 4) ? ao_names[sel - 1] : "no such set = game HDAO";
      fprintf(stderr, "[ao] selector %d (%s), sets: %s%s%s\n", sel, nm, nh ? have : "none",
        ea ? " [BSI_SHADER_ALT from env.txt]" : "", es ? " [BSI_SHADER_SEL from env.txt]" : "");
      if(!ea && sel >= 1 && sel <= 4 && !strstr(have, ao_names[sel - 1])) fprintf(stderr, "[ao] set %d (%s) missing in %s: game HDAO\n", sel, ao_names[sel - 1], ad);
    } }
  ANativeWindow *w = android_window();
  { char sz[64]; snprintf(sz, sizeof sz, "%dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); setenv("BSI_ANDROID_SIZE", sz, 1);
    snprintf(sz, sizeof sz, "%d", ANativeWindow_getWidth(w)); setenv("BSI_WIN_W", sz, 1); snprintf(sz, sizeof sz, "%d", ANativeWindow_getHeight(w)); setenv("BSI_WIN_H", sz, 1); }
  { /* Read the opt-in file directly as well as the parsed process environment.
       The Quest diagnostic build uses this gate to distinguish an env.txt
       parsing/launch issue from a DXVK core or Android WSI failure. */
    const char *probe = getenv("BSI_DXVK_CORE_PROBE");
    char ef[600]; int file_probe = 0;
    snprintf(ef, sizeof ef, "%s/env.txt", g_files);
    FILE *f = fopen(ef, "r");
    if(f){
      char l[512];
      while(fgets(l, sizeof l, f)){
        char *e = l + strcspn(l, "\r\n"); *e = 0;
        char *k = l; while(*k == ' ' || *k == '\t') k++;
        char *eq = strchr(k, '='); if(!eq) continue;
        *eq = 0; char *ke = eq; while(ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        char *v = eq + 1; while(*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v); while(ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if(!strcmp(k, "BSI_DXVK_CORE_PROBE") && !strcmp(v, "1")) file_probe = 1;
      }
      fclose(f);
    }
    fprintf(stderr, "[android] DXVK core probe gate: env=%s file=%s (%s)\n",
      probe ? probe : "<unset>", file_probe ? "1" : "not set", ef);
    if((probe && strcmp(probe, "1") == 0) || file_probe) bsi_dxvk_core_probe(); }
  { void prof_start(const char *files); prof_start(g_files); }
  { void memwatch_start(const char *files); memwatch_start(g_files); }   /* memory diagnostic */
  { /* leakwatch (LEAK.md): OFF unless /sdcard/BSI/leakwatch_on.txt exists (v0.8.5b crashed with it on by default) */ char lo[600], lw[600]; snprintf(lo, sizeof lo, "%s/leakwatch_on.txt", g_files);
    if(access(lo, F_OK) == 0){ snprintf(lw, sizeof lw, "%s/leakwatch.txt", g_files); void leakwatch_start(const char *path); leakwatch_start(lw); } }
  console_start(g_files);   /* live console, only if /sdcard/BSI/console.txt exists */
  { extern void autopilot_start(const char*) __attribute__((weak)); if(autopilot_start) autopilot_start(g_files); }   /* b78: lab autopilot, only if /sdcard/BSI/autopilot.txt exists (one-shot) */
  { void dsp_probe_start(void); dsp_probe_start(); }
  char *argv[] = { "bsi", exe, NULL };
  int r = bsi_main(2, argv);
  fprintf(stderr, "[android] game exited (%d)\n", r);
  ANativeActivity_finish(g_act);
  return NULL;
}

static void onDestroy(ANativeActivity *a){ LOGI("onDestroy"); _exit(0); }

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *a, void *savedState, size_t savedStateSize){
  g_act = a;
  a->callbacks->onNativeWindowCreated = onNativeWindowCreated;
  a->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
  a->callbacks->onInputQueueCreated = onInputQueueCreated;
  a->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
  a->callbacks->onDestroy = onDestroy;
  if(g_started) return; g_started = 1;
  /* game data: /sdcard/BSI (shared storage, filled with "adb push" / SideQuest) when present, else the app's own folder */
  snprintf(g_extfiles, sizeof g_extfiles, "%s", a->externalDataPath ? a->externalDataPath : "");
  if(access("/sdcard/BSI/BioShock Infinite/Binaries/Win32/BioShockInfinite.exe", R_OK)==0 || access("/sdcard/BSI/bsi.log", R_OK)==0) snprintf(g_files, sizeof g_files, "/sdcard/BSI");
  else snprintf(g_files, sizeof g_files, "%s", a->externalDataPath ? a->externalDataPath : "/sdcard/BSI");
  mkdir(g_files, 0777);
  { char pf[600]; snprintf(pf, sizeof pf, "%s/panel_mode.txt", g_files); if(access(pf, R_OK) == 0) g_xr_mode = 0; }
  if(g_xr_mode) xr_start(a, g_files);   /* immersive: game on a big virtual screen, Touch controllers as gamepad */
  pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64<<20);
  pthread_create(&t, NULL, input_thread, NULL); pthread_detach(t);
  pthread_create(&t, &at, game_thread, NULL); pthread_detach(t);
}
