/* setsweep: live A/B of renderer settings inside ONE Quest session (notes/INI_AUDIT.md section 5).
   NEW FILE, not linked yet. Integration: add linux-side object to link_android.sh (same pattern as leakwatch.c); nothing
   else changes. Off unless <dir>/setsweep.txt exists when the game starts (dir = $BSI_SETSWEEP_DIR or /sdcard/BSI).

   Why: the ini can only be changed with the game closed, so every Quest A/B costs a restart and a walk back to the same
   spot. But most of the expensive Ultra features are plain UBOOL/int fields of GSystemSettings (0x13a4c60) that the
   renderer reads every frame, and several of them have developer console toggles in UGameEngine::Exec (0x65dcc0) that do
   nothing more than flip the field (TOGGLEHDAO, TOGGLEAOFILTERING, TOGGLESM5SHADOWS, TOGGLESM5SHADOWBLUR,
   TOGGLEDIFFUSIONDOF, TOGGLESM5FXAA). This thread does the same from the host side, step by step, while the player
   stands still, and logs wall-clock step times so regress/ini_audit/sweep_report.py can cut the per-frame file written by
   BSI_BENCH_FT (bridge.c bench_frame: realtime, frame ms, RT/GT CPU ms) into per-step fps / GPU-vs-CPU numbers.

   Safety rules (nothing here can turn ON a feature that was off at startup, so no resource that was never created is used):
   - every bool knob may only take its startup value or its "saving" value (0 for Enable*, 1 for DisableAOFiltering);
   - int knobs are clamped to a small table range; floats are not supported (they are applied at load/attach time);
   - before the first write, the exe identity is checked (UTF-16 key names at their known addresses, read with
     process_vm_readv so an unmapped page cannot crash the game) and the startup values must look sane;
   - all knobs are restored after every step, at the end, and when <dir>/setsweep_stop.txt appears.

   Plan file <dir>/setsweep.txt (whitespace separated, '#' starts a comment):
     hold=30            seconds per step (default 30, 5..600)
     settle=5           seconds at the start of each step that the report ignores (default 5; only written to the log)
     repeat=2           run the step list N times (default 1, max 10): A/B/A/B shows drift (heat, streaming)
     autostart=0        seconds after the game starts to begin by itself (0 = wait for <dir>/setsweep_go.txt)
     step base                                        (no assignments = everything at startup values)
     step hdao_nq HDAOHighQuality=0
     step chs_off EnableSM5ShadowSystem=0
   Run: stand at the test spot, then `adb shell touch /sdcard/BSI/setsweep_go.txt`. The go file is renamed to
   setsweep_done.txt when the run ends. Log: <dir>/setsweep.log (+ "[setsweep]" lines in bsi.log).
   Needs env.txt: BSI_BENCH_FT=/sdcard/BSI/ft.txt for the per-frame data the report uses. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/uio.h>

extern uint8_t *M;

typedef struct {
  const char *name; uint32_t addr;
  int kind;          /* 0 = UBOOL, may be startup or 'save'; 1 = int clamped to [lo,hi] */
  int save, lo, hi;
  const char *why;   /* where the renderer reads it (for the log) */
} Knob;

/* Addresses = GSystemSettings 0x13a4c60 + 4 + loader offset (FUN_005c0d30 key table, verified against the console
   toggles and against the earlier EnableGPUOcclusionQueries = 0x13a4cc4 finding). */
static const Knob K[] = {
  { "EnableHDAO",                        0x13a4cdc, 0, 0, 0, 1,  "console TOGGLEHDAO; AO render 0x7e6a7d" },
  { "HDAOHighQuality",                   0x13a4ce0, 0, 0, 0, 1,  "AO render 0x7e6ab7 (HQ vs normal HDAO compute shader)" },
  { "DisableAOFiltering",                0x13a4cd8, 0, 1, 0, 1,  "console TOGGLEAOFILTERING; AO blur passes 0x7e6c52" },
  { "EnableScreenSpaceAmbientShadows",   0x13a4ccc, 0, 0, 0, 1,  "AO master switch 0x7e732d (measurement only)" },
  { "EnableSM5ShadowSystem",             0x13a4ce8, 0, 0, 0, 1,  "console TOGGLESM5SHADOWS (AMD contact hardening shadows)" },
  { "EnableSM5ShadowBlur",               0x13a4cec, 0, 0, 0, 1,  "console TOGGLESM5SHADOWBLUR" },
  { "DiffusionDepthOfField",             0x13a4c84, 0, 0, 0, 1,  "console TOGGLEDIFFUSIONDOF" },
  { "UseHighQualityLightShaftRendering", 0x13a4cd4, 0, 0, 0, 1,  "light shaft render 0x723f19/0x72402b" },
  { "DepthOfField",                      0x13a4c80, 0, 0, 0, 1,  "UDOFEffect/UDOFAndBloomEffect IsShown" },
  { "ShadowFilterQualityBias",           0x13a50a8, 1, 0, 0, 3,  "shadow projection shader choice 0x7221a3.." },
  { "LightFadeResolution",               0x13a50ec, 1, 0, 0, 64, "light visibility 0x81a5e5" },
  { "LightMinResolution",                0x13a50f0, 1, 0, 0, 64, "light visibility 0x81a605" },
  { "CullCastersSmallerThanTexels",      0x13a50b8, 1, 0, 0, 64, "shadow caster cull 0x81fa8c" },
};
#define NK ((int)(sizeof K / sizeof K[0]))

/* exe identity: UTF-16 key strings of the SystemSettings loader table */
static const struct { uint32_t addr; const char *s; } ID[] = {
  { 0x1121044, "EnableSM5ShadowBlur" }, { 0x11210dc, "HDAOHighQuality" }, { 0x11212dc, "MaxAnisotropy" },
};

#define MAXSTEP 32
#define MAXAS 8
typedef struct { char label[32]; int n; int k[MAXAS]; int v[MAXAS]; } Step;

static char g_dir[512];
static FILE *g_log;
static int g_start[NK];

static double now_rt(void){ struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double now_mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void path(char *o, size_t n, const char *f){ snprintf(o, n, "%s/%s", g_dir, f); }
static int exists(const char *f){ char p[600]; path(p, sizeof p, f); return access(p, F_OK) == 0; }

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...){
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  fprintf(stderr, "[setsweep] %s\n", b);
  if(g_log){ fprintf(g_log, "%s\n", b); fflush(g_log); }
}

/* safe read of guest memory: process_vm_readv on ourselves returns EFAULT instead of SIGSEGV for unmapped pages */
static int safe_read(uint32_t gaddr, void *dst, size_t n){
  if(!M) return -1;
  struct iovec l = { dst, n }, r = { M + gaddr, n };
  ssize_t got = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
  return got == (ssize_t)n ? 0 : -1;
}

static int identity_ok(void){
  for(unsigned i = 0; i < sizeof ID / sizeof ID[0]; i++){
    size_t n = strlen(ID[i].s); uint16_t w[64];
    if(n + 1 > 64 || safe_read(ID[i].addr, w, (n + 1) * 2)) return 0;
    for(size_t j = 0; j <= n; j++) if(w[j] != (uint16_t)(j < n ? ID[i].s[j] : 0)) return 0;
  }
  return 1;
}

static int find_knob(const char *n){ for(int i = 0; i < NK; i++) if(!strcasecmp(K[i].name, n)) return i; return -1; }
static volatile int32_t *kp(int i){ return (volatile int32_t*)(M + K[i].addr); }
static void kset(int i, int v){ __atomic_store_n(kp(i), v, __ATOMIC_RELAXED); }
static void restore_all(void){ for(int i = 0; i < NK; i++) kset(i, g_start[i]); }

static int allowed(int i, int v){
  if(K[i].kind == 0) return v == g_start[i] || v == K[i].save;
  return v >= K[i].lo && v <= K[i].hi;
}

typedef struct { int hold, settle, repeat, autostart, nstep; Step st[MAXSTEP]; } Plan;

static int parse_plan(Plan *p){
  char pp[600]; path(pp, sizeof pp, "setsweep.txt");
  FILE *f = fopen(pp, "r"); if(!f) return -1;
  memset(p, 0, sizeof *p); p->hold = 30; p->settle = 5; p->repeat = 1;
  char line[512]; int ln = 0, bad = 0;
  while(fgets(line, sizeof line, f)){
    ln++; char *c = strchr(line, '#'); if(c) *c = 0;
    char *save = NULL, *tok = strtok_r(line, " \t\r\n", &save);
    if(!tok) continue;
    if(!strcmp(tok, "step")){
      if(p->nstep >= MAXSTEP){ logf_("plan line %d: too many steps", ln); bad = 1; break; }
      Step *s = &p->st[p->nstep++]; memset(s, 0, sizeof *s);
      char *lab = strtok_r(NULL, " \t\r\n", &save); snprintf(s->label, sizeof s->label, "%s", lab ? lab : "step");
      for(char *a; (a = strtok_r(NULL, " \t\r\n", &save)); ){
        char *eq = strchr(a, '='); if(!eq){ logf_("plan line %d: '%s' is not Name=value", ln, a); bad = 1; continue; }
        *eq = 0; int k = find_knob(a);
        if(k < 0){ logf_("plan line %d: unknown knob '%s'", ln, a); bad = 1; continue; }
        if(s->n >= MAXAS){ logf_("plan line %d: too many assignments", ln); bad = 1; continue; }
        s->k[s->n] = k; s->v[s->n] = atoi(eq + 1); s->n++;
      }
      continue;
    }
    for(; tok; tok = strtok_r(NULL, " \t\r\n", &save)){   /* option line: any number of key=value tokens */
      char *eq = strchr(tok, '='); if(!eq){ logf_("plan line %d: '%s' ignored", ln, tok); continue; }
      *eq = 0; int v = atoi(eq + 1);
      if(!strcmp(tok, "hold")) p->hold = v < 5 ? 5 : v > 600 ? 600 : v;
      else if(!strcmp(tok, "settle")) p->settle = v < 0 ? 0 : v;
      else if(!strcmp(tok, "repeat")) p->repeat = v < 1 ? 1 : v > 10 ? 10 : v;
      else if(!strcmp(tok, "autostart")) p->autostart = v < 0 ? 0 : v;
      else logf_("plan line %d: unknown option '%s'", ln, tok);
    }
  }
  fclose(f);
  if(!p->nstep){ logf_("plan has no steps"); bad = 1; }
  return bad ? -1 : 0;
}

/* sleep up to s seconds; returns 1 if the stop file appeared */
static int hold_for(double s){
  double end = now_mono() + s;
  while(now_mono() < end){ if(exists("setsweep_stop.txt")) return 1; usleep(250000); }
  return 0;
}

static void run(Plan *p){
  if(!identity_ok()){ logf_("exe identity check failed (wrong build or guest memory not mapped): nothing written"); return; }
  for(int i = 0; i < NK; i++){
    int32_t v; if(safe_read(K[i].addr, &v, 4)){ logf_("cannot read %s: nothing written", K[i].name); return; }
    if(v < 0 || v > 4096 || (K[i].kind == 0 && v > 1)){ logf_("%s has odd startup value %d: nothing written", K[i].name, v); return; }
    g_start[i] = v;
  }
  for(int s = 0; s < p->nstep; s++) for(int a = 0; a < p->st[s].n; a++){
    int k = p->st[s].k[a], v = p->st[s].v[a];
    if(!allowed(k, v)){ logf_("step %s: %s=%d not allowed (startup %d; bools may only go to %d): nothing written",
                              p->st[s].label, K[k].name, v, g_start[k], K[k].save); return; }
  }
  { char b[400]; int o = 0; for(int i = 0; i < NK && o < (int)sizeof b - 40; i++) o += snprintf(b + o, sizeof b - o, "%s=%d ", K[i].name, g_start[i]);
    logf_("START rt=%.6f hold=%d settle=%d repeat=%d steps=%d startup: %s", now_rt(), p->hold, p->settle, p->repeat, p->nstep, b); }
  int stop = 0;
  for(int r = 0; r < p->repeat && !stop; r++) for(int s = 0; s < p->nstep && !stop; s++){
    Step *st = &p->st[s]; restore_all();
    char b[300]; int o = 0; b[0] = 0;
    for(int a = 0; a < st->n; a++){ kset(st->k[a], st->v[a]); o += snprintf(b + o, sizeof b - o, " %s=%d", K[st->k[a]].name, st->v[a]); }
    logf_("STEP rt=%.6f rep=%d idx=%d label=%s settle=%d%s", now_rt(), r, s, st->label, p->settle, b);
    stop = hold_for(p->hold);
  }
  restore_all();
  logf_("END rt=%.6f%s", now_rt(), stop ? " (stopped by setsweep_stop.txt)" : "");
}

static void *thr(void *u){
  (void)u;
  char lp[600]; path(lp, sizeof lp, "setsweep.log"); g_log = fopen(lp, "a");
  Plan p; if(parse_plan(&p)){ logf_("plan rejected, sweep disabled"); return NULL; }
  logf_("armed: %d steps x %d s x %d; %s", p.nstep, p.hold, p.repeat,
        p.autostart ? "autostart" : "waiting for setsweep_go.txt");
  double t0 = now_mono();
  for(;;){
    sleep(2);
    int go = exists("setsweep_go.txt") || (p.autostart && now_mono() - t0 >= p.autostart);
    if(!go || !M) continue;
    if(parse_plan(&p)){ logf_("plan rejected at start"); }
    else run(&p);
    char a[600], b[600]; path(a, sizeof a, "setsweep_go.txt"); path(b, sizeof b, "setsweep_done.txt");
    if(rename(a, b)){ FILE *d = fopen(b, "w"); if(d) fclose(d); }   /* autostart run: no go file to rename */
    p.autostart = 0;   /* autostart runs once; further runs need the go file */
  }
  return NULL;
}

int setsweep_start(void){
  const char *d = getenv("BSI_SETSWEEP_DIR"); snprintf(g_dir, sizeof g_dir, "%s", d && *d ? d : "/sdcard/BSI");
  if(!exists("setsweep.txt")) return 0;
  pthread_t t; if(pthread_create(&t, NULL, thr, NULL)) return -1;
  pthread_detach(t); return 1;
}

#ifndef SETSWEEP_NO_CTOR
__attribute__((constructor)) static void setsweep_ctor(void){ setsweep_start(); }
#endif
