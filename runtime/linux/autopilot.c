/* autopilot.c (build 78, chat 2 "Quest lab"): unattended scripted runs on the Quest.

   Active only when /sdcard/BSI/autopilot.txt exists at game start. The file is ONE-SHOT: it is renamed to
   autopilot_last.txt as soon as it is read (unless it contains a line "keep"), so a normal launch by the user
   afterwards is never autopiloted, even if the PC lab runner died mid-job.

   It drives the game like a person would, with nobody wearing the headset:
   - a synthetic gamepad merged into android_pad() (android_glue.c), so the game and the mods see a real pad
     (works while the OpenXR session is not FOCUSED too, when the Touch pad is zeroed);
   - a scripted head pose (xr_glue.c asks autopilot_head() every XR frame; stereo eye poses keep the real
     IPD / FOV), level and repeatable instead of wherever the headset happens to point on the desk;
   - A/B switches by copying files inside /sdcard/BSI (vr_stereo.txt etc. are re-read every 2 s);
   - markers and a status line in bsi.log ("[autopilot] ..."), and a clean quit (xrRequestExitSession,
     fallback _exit after 12 s).

   Script: one command per line, '#' starts a comment. Commands run in order on a timeline thread:
     log <text>                      write a line into bsi.log
     wait <sec>                      sleep
     wait_map <Name> [timeout]       until the current map (outermost package of GWorld's level) is Name
     wait_map_not <Name> [timeout]   until it is something else (e.g. leave XEntry_P after Continue)
     wait_presents <n> [timeout]     until n more frames were presented (rendering is alive)
     wait_log <timeout> <text...>    until bsi.log contains the text (exact, case-sensitive)
     press_until <BTN> <every_s> <timeout> <text...>   tap BTN every every_s seconds until bsi.log contains the text
     press <BTN>[+BTN..] [ms]        tap buttons (default 150 ms) then 200 ms released
                                     BTN: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT
     hold <BTN>[+BTN..] <sec>        hold buttons
     stick <lx> <ly> <rx> <ry> [sec] sticks -1..1 (ly>0 = forward); with sec: held, then centred; without: stays
     trigger <lt> <rt> [sec]         triggers 0..1, same rule
     head real                       use the real headset pose
     head static [yaw] [pitch]       fixed level view, degrees (yaw>0 = left, pitch>0 = up). DEFAULT from start
     head sweep <yaw_amp> <period> [pitch_amp]   sinusoidal look left/right (and up/down), degrees / seconds
     head turn <deg_per_s>           constant turn (full circles)
     mark <name>                     marker line with time, present count, map
     copy <src> <dst>                copy a file inside /sdcard/BSI (atomic rename), e.g. A/B config variants
     on_timeout quit|continue        what a timed-out wait does (default quit)
     quit                            clean exit
     keep                            (anywhere) do not rename the script after reading it
   Waits without a timeout use 600 s. */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include "android_plat.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define LOGP "[autopilot] "
#define G_GNAMES 0x0139DFECu
#define G_GWORLD 0x013AC3ECu
#define UO_OUTER 0x14u
#define UO_NAME  0x18u

extern volatile long g_br_presents;   /* bridge.c */

static int g_on;
static char g_dir[512];
static double g_t0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static AndroidPad g_pad;                 /* synthetic pad (g_mu) */
static char g_map[128] = "-";            /* written by the game thread (g_mu) */
static volatile int g_quit;
static int g_on_timeout_quit = 1;

/* head (read by the XR thread every frame, written by the timeline thread) */
enum { H_REAL, H_STATIC, H_SWEEP, H_TURN };
static volatile int g_hmode = H_STATIC, g_hgen = 1;
static volatile float g_h_yaw, g_h_pitch, g_h_amp_yaw, g_h_amp_pitch, g_h_period = 10.f, g_h_rate;
static volatile double g_h_t0;

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static double tt(void){ return now_s() - g_t0; }

/* ---------------- game thread: current map ---------------- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";
  return (const char *)MEMP(e + 0x10);
}
static const char *oname(uint32_t o){ return gptr(o) ? fname_str(R32(o + UO_NAME)) : "None"; }
static const char *top_name(uint32_t o){ if(!gptr(o)) return "-"; for(int k = 0; k < 8 && gptr(R32(o + UO_OUTER)); k++) o = R32(o + UO_OUTER); return oname(o); }
static uint32_t world_info(void){
  uint32_t w = R32(G_GWORLD); if(!gptr(w)) return 0;
  uint32_t lvl = R32(w + 0x3C); if(!gptr(lvl)) return 0;
  uint32_t act = R32(lvl + 0x28); if(!gptr(act)) return 0;
  uint32_t wi = R32(act); return gptr(wi) ? wi : 0;
}
void autopilot_game_tick(void){   /* patches.c, UXGameEngine::Tick (0xaf0480) */
  if(!g_on) return;
  static double last; double n = now_s(); if(n - last < 0.25) return; last = n;
  const char *m = top_name(world_info());
  char buf[128]; snprintf(buf, sizeof buf, "%s", m && *m ? m : "-");
  pthread_mutex_lock(&g_mu); int changed = strcmp(buf, g_map) != 0; if(changed) memcpy(g_map, buf, sizeof g_map); pthread_mutex_unlock(&g_mu);
  if(changed) fprintf(stderr, LOGP "map -> %s (t=%.1f s, present %ld)\n", buf, tt(), (long)g_br_presents);
}
static void cur_map(char *out, size_t n){ pthread_mutex_lock(&g_mu); snprintf(out, n, "%s", g_map); pthread_mutex_unlock(&g_mu); }

/* ---------------- pad (android_glue.c android_pad) ---------------- */
int autopilot_pad(AndroidPad *p){
  if(!g_on) return 0;
  pthread_mutex_lock(&g_mu); *p = g_pad; pthread_mutex_unlock(&g_mu);
  return 1;
}
static void pad_buttons(uint16_t b){ pthread_mutex_lock(&g_mu); g_pad.buttons = b; pthread_mutex_unlock(&g_mu); }
static int16_t ax(float v){ if(v > 1) v = 1; if(v < -1) v = -1; return (int16_t)(v * 32767.f); }
static uint8_t tr(float v){ if(v > 1) v = 1; if(v < 0) v = 0; return (uint8_t)(v * 255.f); }

/* ---------------- head (xr_glue.c) ---------------- */
/* Returns 1 when the autopilot supplies the head orientation (quaternion x,y,z,w in OpenXR LOCAL space: +Y up,
   -Z forward); *gen changes whenever the mode changes so xr_glue can recenter vr_head. */
int autopilot_head(float q[4], int *gen){
  if(!g_on) return 0;
  *gen = g_hgen;
  int m = g_hmode; if(m == H_REAL) return 0;
  double t = now_s() - g_h_t0;
  float yaw = g_h_yaw, pitch = g_h_pitch;
  if(m == H_SWEEP && g_h_period > 0.05f){ double ph = 2.0 * M_PI * t / g_h_period; yaw += g_h_amp_yaw * (float)sin(ph); pitch += g_h_amp_pitch * (float)sin(2.0 * ph); }
  else if(m == H_TURN) yaw += (float)fmod(g_h_rate * t, 360.0);
  float hy = yaw * (float)(M_PI / 360.0), hp = pitch * (float)(M_PI / 360.0);
  /* q = yaw(Y) * pitch(X) */
  float cy = cosf(hy), sy = sinf(hy), cp = cosf(hp), sp = sinf(hp);
  q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;
  return 1;
}
int autopilot_quit_requested(void){ return g_on && g_quit; }

/* ---------------- script ---------------- */
#define MAXL 400
static char g_lines[MAXL][256]; static int g_nl;

static uint16_t parse_buttons(const char *s, int *ok){
  static const struct { const char *n; uint16_t b; } tab[] = {
    {"UP",0x1},{"DOWN",0x2},{"LEFT",0x4},{"RIGHT",0x8},{"START",0x10},{"BACK",0x20},{"LS",0x40},{"RS",0x80},
    {"LB",0x100},{"RB",0x200},{"A",0x1000},{"B",0x2000},{"X",0x4000},{"Y",0x8000} };
  uint16_t b = 0; *ok = 1; char tmp[64]; snprintf(tmp, sizeof tmp, "%s", s);
  char *sv = NULL; for(char *tok = strtok_r(tmp, "+", &sv); tok; tok = strtok_r(NULL, "+", &sv)){
    int f = 0; for(unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) if(!strcasecmp(tok, tab[i].n)){ b |= tab[i].b; f = 1; }
    if(!f) *ok = 0; }
  return b;
}
static void sleep_s(double s){ double end = now_s() + s; while(!g_quit){ double r = end - now_s(); if(r <= 0) break; usleep(r > 0.1 ? 100000 : (useconds_t)(r * 1e6)); } }
static int safe_name(const char *s){ if(!*s || strlen(s) > 64) return 0; for(; *s; s++) if(!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-') return 0; return 1; }
static int copy_file(const char *src, const char *dst){
  if(!safe_name(src) || !safe_name(dst)) return -1;
  char a[600], b[600], t[620]; snprintf(a, sizeof a, "%s/%s", g_dir, src); snprintf(b, sizeof b, "%s/%s", g_dir, dst); snprintf(t, sizeof t, "%s.aptmp", b);
  FILE *fi = fopen(a, "rb"); if(!fi) return -2;
  FILE *fo = fopen(t, "wb"); if(!fo){ fclose(fi); return -3; }
  char buf[4096]; size_t n; while((n = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, n, fo);
  fclose(fi); fclose(fo); return rename(t, b) == 0 ? 0 : -4;
}
static void do_quit(const char *why){
  if(g_quit) return;
  fprintf(stderr, LOGP "quit (%s) t=%.1f s, present %ld\n", why, tt(), (long)g_br_presents); fflush(stderr);
  g_quit = 1;
}
/* wait helper: cond() true -> 1; timeout -> 0 */
typedef int (*cond_fn)(const char *arg, long base);
static int c_map(const char *a, long b){ (void)b; char m[128]; cur_map(m, sizeof m); return !strcasecmp(m, a); }
static int c_map_not(const char *a, long b){ (void)b; char m[128]; cur_map(m, sizeof m); return strcasecmp(m, a) != 0 && strcmp(m, "-") != 0; }
static int c_presents(const char *a, long b){ return g_br_presents - b >= atol(a); }
static int wait_for(cond_fn f, const char *arg, double timeout, const char *what){
  long base = g_br_presents; double end = now_s() + timeout;
  while(!g_quit){ if(f(arg, base)) return 1; if(now_s() > end) break; usleep(100000); }
  if(g_quit) return 0;
  char m[128]; cur_map(m, sizeof m);
  fprintf(stderr, LOGP "TIMEOUT %s %s after %.0f s (map %s, present %ld)\n", what, arg, timeout, m, (long)g_br_presents);
  if(g_on_timeout_quit) do_quit("wait timed out");
  return 0;
}

/* bsi.log scan (our own stderr): new bytes since the last call, with an overlap so a line split across reads is found */
typedef struct { long off; char text[160]; } LogScan;
static int log_scan(LogScan *ls){
  fflush(stderr);
  char f[600]; snprintf(f, sizeof f, "%s/bsi.log", g_dir);
  FILE *fp = fopen(f, "rb"); if(!fp) return 0;
  size_t tl = strlen(ls->text); long start = ls->off > (long)tl ? ls->off - (long)tl : 0;
  fseek(fp, 0, SEEK_END); long end = ftell(fp);
  if(end < start){ start = 0; }   /* rotated / truncated */
  int found = 0; static char buf[1 << 16];
  while(!found && start < end){ fseek(fp, start, SEEK_SET); size_t n = fread(buf, 1, sizeof buf - 1, fp); if(!n) break; buf[n] = 0;
    for(size_t i = 0; i < n; i++) if(!buf[i]) buf[i] = ' ';
    for(char *m = strstr(buf, ls->text); m; m = strstr(m + 1, ls->text)){   /* ignore our own "[autopilot]" lines (they quote the text) */
      char *ln = m; while(ln > buf && ln[-1] != '\n') ln--;
      if(strncmp(ln, "[autopilot]", 11) && strncmp(ln, "[xr] autopilot", 14)){ found = 1; break; } }
    if(n < sizeof buf - 1) { start += (long)n; break; }
    start += (long)n - (long)tl; }
  ls->off = end; fclose(fp); return found;
}
static int wait_log(const char *text, double timeout, uint16_t press, double every){
  LogScan ls; memset(&ls, 0, sizeof ls); snprintf(ls.text, sizeof ls.text, "%s", text);
  double end = now_s() + timeout, next = now_s();
  while(!g_quit){
    if(log_scan(&ls)) return 1;
    if(press && now_s() >= next){ pad_buttons(press); sleep_s(0.15); pad_buttons(0); next = now_s() + every;
      fprintf(stderr, LOGP "press_until: pressed, waiting for \"%s\"\n", text); }
    if(now_s() > end) break;
    sleep_s(0.5); }
  if(g_quit) return 0;
  char m[128]; cur_map(m, sizeof m);
  fprintf(stderr, LOGP "TIMEOUT waiting for log text \"%s\" after %.0f s (map %s, present %ld)\n", text, timeout, m, (long)g_br_presents);
  if(g_on_timeout_quit) do_quit("wait timed out");
  return 0;
}

static void *status_thread(void *p){ (void)p;
  long lastp = g_br_presents; double lastt = now_s();
  while(!g_quit){ sleep_s(10);
    if(g_quit) break;
    long pr = g_br_presents; double n = now_s(); char m[128]; cur_map(m, sizeof m);
    fprintf(stderr, LOGP "status t=%.0f s map %s presents %ld (%.1f fps) head %s\n", tt(), m, pr, (pr - lastp) / (n - lastt),
            g_hmode == H_REAL ? "real" : g_hmode == H_STATIC ? "static" : g_hmode == H_SWEEP ? "sweep" : "turn");
    lastp = pr; lastt = n; }
  return NULL;
}
static void *quit_fallback(void *p){ (void)p;
  while(!g_quit) usleep(200000);
  sleep(12);
  fprintf(stderr, LOGP "quit fallback: process still alive 12 s after the exit request, _exit(0)\n"); fflush(stderr);
  _exit(0);
  return NULL;
}

static void *timeline(void *p){ (void)p;
  sleep_s(0.5);
  for(int i = 0; i < g_nl && !g_quit; i++){
    char line[256]; snprintf(line, sizeof line, "%s", g_lines[i]);
    char *argv[8] = {0}; int argc = 0; char *rest = NULL;
    for(char *s = line; *s && argc < 8; ){ while(*s == ' ' || *s == '\t') s++; if(!*s) break;
      if(argc == 1) rest = s;
      argv[argc++] = s; while(*s && *s != ' ' && *s != '\t') s++; if(*s) *s++ = 0; }
    if(!argc) continue;
    const char *c = argv[0];
    fprintf(stderr, LOGP "step %d/%d t=%.1f s: %s\n", i + 1, g_nl, tt(), g_lines[i]);
    if(!strcasecmp(c, "log")) fprintf(stderr, LOGP "log: %s\n", rest ? g_lines[i] + (rest - line) : "");
    else if(!strcasecmp(c, "wait") && argc >= 2) sleep_s(atof(argv[1]));
    else if(!strcasecmp(c, "wait_map") && argc >= 2) wait_for(c_map, argv[1], argc >= 3 ? atof(argv[2]) : 600, "wait_map");
    else if(!strcasecmp(c, "wait_map_not") && argc >= 2) wait_for(c_map_not, argv[1], argc >= 3 ? atof(argv[2]) : 600, "wait_map_not");
    else if(!strcasecmp(c, "wait_log") && argc >= 3){   /* wait_log <timeout> <text...> */
      const char *txt = g_lines[i] + (argv[2] - line); wait_log(txt, atof(argv[1]), 0, 0); }
    else if(!strcasecmp(c, "press_until") && argc >= 5){   /* press_until <BTN> <every_s> <timeout> <text...> */
      int ok; uint16_t b = parse_buttons(argv[1], &ok);
      if(!ok){ fprintf(stderr, LOGP "bad button name in: %s\n", g_lines[i]); continue; }
      const char *txt = g_lines[i] + (argv[4] - line); wait_log(txt, atof(argv[3]), b, atof(argv[2])); }
    else if(!strcasecmp(c, "wait_presents") && argc >= 2) wait_for(c_presents, argv[1], argc >= 3 ? atof(argv[2]) : 600, "wait_presents");
    else if((!strcasecmp(c, "press") || !strcasecmp(c, "hold")) && argc >= 2){
      int ok; uint16_t b = parse_buttons(argv[1], &ok);
      if(!ok){ fprintf(stderr, LOGP "bad button name in: %s\n", g_lines[i]); continue; }
      double hold = !strcasecmp(c, "press") ? (argc >= 3 ? atof(argv[2]) / 1000.0 : 0.15) : (argc >= 3 ? atof(argv[2]) : 1.0);
      pad_buttons(b); sleep_s(hold); pad_buttons(0); sleep_s(0.2); }
    else if(!strcasecmp(c, "stick") && argc >= 5){
      pthread_mutex_lock(&g_mu); g_pad.lx = ax(atof(argv[1])); g_pad.ly = ax(atof(argv[2])); g_pad.rx = ax(atof(argv[3])); g_pad.ry = ax(atof(argv[4])); pthread_mutex_unlock(&g_mu);
      if(argc >= 6){ sleep_s(atof(argv[5])); pthread_mutex_lock(&g_mu); g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0; pthread_mutex_unlock(&g_mu); } }
    else if(!strcasecmp(c, "trigger") && argc >= 3){
      pthread_mutex_lock(&g_mu); g_pad.lt = tr(atof(argv[1])); g_pad.rt = tr(atof(argv[2])); pthread_mutex_unlock(&g_mu);
      if(argc >= 4){ sleep_s(atof(argv[3])); pthread_mutex_lock(&g_mu); g_pad.lt = g_pad.rt = 0; pthread_mutex_unlock(&g_mu); } }
    else if(!strcasecmp(c, "head") && argc >= 2){
      const char *m = argv[1];
      if(!strcasecmp(m, "real")) g_hmode = H_REAL;
      else if(!strcasecmp(m, "static")){ g_h_yaw = argc >= 3 ? atof(argv[2]) : 0; g_h_pitch = argc >= 4 ? atof(argv[3]) : 0; g_hmode = H_STATIC; }
      else if(!strcasecmp(m, "sweep") && argc >= 4){ g_h_amp_yaw = atof(argv[2]); g_h_period = atof(argv[3]); g_h_amp_pitch = argc >= 5 ? atof(argv[4]) : 0; g_h_t0 = now_s(); g_hmode = H_SWEEP; }
      else if(!strcasecmp(m, "turn") && argc >= 3){ g_h_rate = atof(argv[2]); g_h_t0 = now_s(); g_hmode = H_TURN; }
      else { fprintf(stderr, LOGP "bad head command: %s\n", g_lines[i]); continue; }
      g_hgen++; }
    else if(!strcasecmp(c, "mark") && argc >= 2){ char m[128]; cur_map(m, sizeof m);
      fprintf(stderr, LOGP "MARK %s t=%.2f s present %ld map %s\n", argv[1], tt(), (long)g_br_presents, m); }
    else if(!strcasecmp(c, "copy") && argc >= 3){ int r = copy_file(argv[1], argv[2]);
      fprintf(stderr, LOGP "copy %s -> %s: %s\n", argv[1], argv[2], r == 0 ? "ok" : r == -1 ? "refused (plain file names in /sdcard/BSI only)" : r == -2 ? "source missing" : "write failed"); }
    else if(!strcasecmp(c, "on_timeout") && argc >= 2) g_on_timeout_quit = strcasecmp(argv[1], "continue") != 0;
    else if(!strcasecmp(c, "quit")){ do_quit("script"); break; }
    else if(!strcasecmp(c, "keep")) ;
    else fprintf(stderr, LOGP "unknown command (skipped): %s\n", g_lines[i]);
  }
  if(!g_quit) fprintf(stderr, LOGP "script finished t=%.1f s (no quit: the game keeps running, head stays scripted)\n", tt());
  return NULL;
}

void autopilot_start(const char *files){   /* android_glue.c, before bsi_main */
  snprintf(g_dir, sizeof g_dir, "%s", files);
  char f[600]; snprintf(f, sizeof f, "%s/autopilot.txt", files);
  FILE *fp = fopen(f, "r"); if(!fp) return;
  int keep = 0; char l[256];
  while(g_nl < MAXL && fgets(l, sizeof l, fp)){
    char *h = strchr(l, '#'); if(h) *h = 0;
    char *e = l + strlen(l); while(e > l && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    char *s = l; while(*s == ' ' || *s == '\t') s++;
    if(!*s) continue;
    if(!strcasecmp(s, "keep")) keep = 1;
    snprintf(g_lines[g_nl++], sizeof g_lines[0], "%s", s); }
  fclose(fp);
  if(!keep){ char d[620]; snprintf(d, sizeof d, "%s/autopilot_last.txt", files); rename(f, d); }
  g_t0 = now_s(); g_h_t0 = g_t0; g_on = 1;
  fprintf(stderr, LOGP "ON: %d commands from %s%s; head static (level) until a head command; pad synthetic\n", g_nl, f,
          keep ? " (keep: file stays)" : " (one-shot: renamed to autopilot_last.txt)");
  pthread_t t;
  if(pthread_create(&t, NULL, timeline, NULL) == 0) pthread_detach(t);
  if(pthread_create(&t, NULL, status_thread, NULL) == 0) pthread_detach(t);
  if(pthread_create(&t, NULL, quit_fallback, NULL) == 0) pthread_detach(t);
}
