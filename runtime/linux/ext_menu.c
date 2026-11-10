/* ext_menu.c -- ext72: the "extension" layer of the BSI-Quest gameplay mods: built-in defaults, one switch file and an in-game
   mod menu drawn by our own renderer (flat and VR).

   DEFAULTS (no file needed): every mod has a built-in default (on/off) and, when it is on without its own config file, a
   built-in config text (the mod's example config). Precedence, per mod:
     1. kill switch in the environment (/sdcard/BSI/env.txt): BSI_GEAR=0, BSI_PERKS=0, BSI_WHEEL=0, BSI_ECONOMY=0, BSI_LEGENDARY=0
     2. <dir of $BSI_BLOOM_FILE>/mods.txt (written by the menu; optional): lines "<mod> 0|1"
     3. the mod's own config file (/sdcard/BSI/<mod>.txt): present = on, with those settings (its "enable 0" still works)
     4. the built-in default of this build: legendary ON, weapon wheel ON, economy ON, weapon perks ON, gear mod OFF
   Mods: gear, perks, wheel, economy, legendary.

   MENU: hold Back for 0.6 s (Quest Touch: left stick click + Y = Back, held; keyboard: F10). A short Back tap still reaches the
   game. Up/down (stick or D-pad, keyboard arrows) selects, A (Enter) toggles, B / Back / Esc (F10) closes. While it is open the
   sticks, A, B and the D-pad are hidden from the game. The menu is a panel of coloured rectangles + a 5x7 pixel font drawn with
   the loot beam's mechanism (clear rectangles in rhi_b1 right before Present): in VR it goes into the UI column (the head-locked
   UI quad), flat it is centred on the screen. Toggles apply live where the mod's hooks are already installed (legendary, beam,
   wheel, economy, perks, gear when they were on at start); a mod that was off at start needs a restart (shown as "RESTART").
   Log lines "[ext]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

#define LOGP "[ext] "

/* ---------------- defaults + switch file ---------------- */
typedef struct { const char *mod, *env, *label; int dflt; int want; int started; } ModSw;
static ModSw g_mods[] = {
  { "legendary", "BSI_LEGENDARY", "LEGENDARY DROPS", 1, -1, -1 },
  { "wheel",     "BSI_WHEEL",     "WEAPON WHEEL",    1, -1, -1 },
  { "economy",   "BSI_ECONOMY",   "ECONOMY",         0, -1, -1 },   /* b74: off by default (b72+b73 Quest crashes: guest pure virtual call inside the loot roll called through economy_call) */
  { "perks",     "BSI_PERKS",     "WEAPON PERKS",    1, -1, -1 },
  { "gear",      "BSI_GEAR",      "GEAR MOD",        0, -1, -1 },
};
#define NMODS (int)(sizeof g_mods / sizeof g_mods[0])
static char g_swfile[512]; static int g_sw_loaded;
static void sw_path(void){
  if(g_swfile[0]) return;
  const char *e = getenv("BSI_MODS_FILE"), *b = getenv("BSI_BLOOM_FILE");
  if(e && *e){ snprintf(g_swfile, sizeof g_swfile, "%s", e); return; }
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl){ snprintf(g_swfile, sizeof g_swfile, "%.*s/mods.txt", (int)(sl - b), b); return; } }
  snprintf(g_swfile, sizeof g_swfile, "/sdcard/BSI/mods.txt");
}
static void sw_load(void){
  if(g_sw_loaded) return; g_sw_loaded = 1; sw_path();
  FILE *f = fopen(g_swfile, "r"); if(!f) return;
  char ln[128]; while(fgets(ln, sizeof ln, f)){ char k[32]; int v; if(sscanf(ln, "%31s %d", k, &v) == 2) for(int i = 0; i < NMODS; i++) if(!strcmp(k, g_mods[i].mod)) g_mods[i].want = v ? 1 : 0; }
  fclose(f);
  fprintf(stderr, LOGP "switch file %s read\n", g_swfile);
}
static void sw_save(void){
  sw_path(); char tmp[520]; snprintf(tmp, sizeof tmp, "%s.tmp", g_swfile);
  FILE *f = fopen(tmp, "w"); if(!f){ fprintf(stderr, LOGP "cannot write %s\n", tmp); return; }
  fprintf(f, "# BSI-Quest mod switches (ext72, written by the in-game mod menu). <mod> 0|1. Delete this file for the built-in defaults.\n");
  for(int i = 0; i < NMODS; i++) if(g_mods[i].want >= 0) fprintf(f, "%s %d\n", g_mods[i].mod, g_mods[i].want);
  fclose(f); rename(tmp, g_swfile);
}
static ModSw *sw_find(const char *mod){ for(int i = 0; i < NMODS; i++) if(!strcmp(g_mods[i].mod, mod)) return &g_mods[i]; return 0; }
/* the mods ask this at startup when their own config file is missing: 1 = run with the built-in config */
int ext_mod_default_on(const char *mod){
  ModSw *m = sw_find(mod); if(!m) return 0;
  sw_load();
  const char *e = getenv(m->env); if(e && *e && !atoi(e)) return 0;
  int on = m->want >= 0 ? m->want : m->dflt;
  fprintf(stderr, LOGP "%s: no config file, %s (%s)\n", mod, on ? "ON with the built-in settings" : "off", m->want >= 0 ? "mods.txt" : "built-in default of this build");
  return on;
}
/* ... and this when their config file exists: 0 = the menu switched the mod off (mods.txt) */
int ext_mod_file_allowed(const char *mod){
  ModSw *m = sw_find(mod); if(!m) return 1;
  sw_load();
  if(m->want == 0){ fprintf(stderr, LOGP "%s: config file present but switched off in %s\n", mod, g_swfile); return 0; }
  return 1;
}
/* built-in config texts: the build-68 example configs (ext_builtin.h) */
#include "ext_builtin.h"
const char *ext_mod_builtin(const char *mod){
  if(!strcmp(mod, "gear")) return EXT_BUILTIN_GEAR;
  if(!strcmp(mod, "perks")) return EXT_BUILTIN_PERKS;
  if(!strcmp(mod, "wheel")) return EXT_BUILTIN_WHEEL;
  if(!strcmp(mod, "economy")) return EXT_BUILTIN_ECONOMY;
  return 0;
}
/* live state the mods report (for the menu) */
void ext_mod_started(const char *mod, int on){ ModSw *m = sw_find(mod); if(m) m->started = on; }

/* ---------------- menu ---------------- */
int legendary_get(const char *what) __attribute__((weak));
void legendary_set(const char *what, int v) __attribute__((weak));
const char *legendary_item_name(void) __attribute__((weak));
const char *legendary_item_desc(void) __attribute__((weak));
int ww_set_enabled(int on) __attribute__((weak));        /* weapon_wheel.c: returns 1 if applied live */
int economy_set_enabled(int on) __attribute__((weak));
int weapon_perks_set_enabled(int on) __attribute__((weak));
int gear_mod_set_enabled(int on) __attribute__((weak));
int vr_stereo_ui_layout(int vw, int vh, void *L) __attribute__((weak));

static volatile int m_open, m_sel, m_toggle, m_close, m_step;   /* written by the input hooks */
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static int g_menu_on = -1;   /* BSI_EXT_MENU=0 disables the menu (hooks passive) */
static int menu_enabled(void){ if(g_menu_on < 0){ const char *e = getenv("BSI_EXT_MENU"), *p = getenv("BSI_PATCHES"); g_menu_on = !((e && !atoi(e)) || (p && !atoi(p))); } return g_menu_on; }

/* rows: 0 legendary, 1 beam, 2 wheel, 3 economy, 4 perks, 5 gear */
#define NROWS 6
static int row_state(int r, int *restart){
  *restart = 0;
  switch(r){
    case 0: return legendary_get ? legendary_get("on") : 0;
    case 1: return legendary_get ? legendary_get("beam") : 0;
    default: { ModSw *m = &g_mods[r - 1]; int want = m->want >= 0 ? m->want : (m->started > 0); if(want && m->started == 0) *restart = 1; return want; }
  }
}
static void row_toggle(int r){
  int rs; int cur = row_state(r, &rs), v = !cur;
  if(r == 0){ if(legendary_set) legendary_set("on", v); g_mods[0].want = v; sw_save(); return; }
  if(r == 1){ if(legendary_set) legendary_set("beam", v); return; }
  ModSw *m = &g_mods[r - 1]; m->want = v; sw_save();
  int live = 0;
  /* b73: switching a mod back ON live re-initialises it in the middle of play (build 72 Quest crash: perks OFF -> ON from
     the menu, then a guest pure virtual call seconds later). Turning OFF stays live (the mod only goes idle); turning ON
     takes effect at the next game start. */
  if(v){ fprintf(stderr, LOGP "menu: %s ON (applies after a restart; live re-enable disabled since build 73)\n", m->mod); return; }
  if(!strcmp(m->mod, "wheel") && ww_set_enabled) live = ww_set_enabled(v);
  else if(!strcmp(m->mod, "economy") && economy_set_enabled) live = economy_set_enabled(v);
  else if(!strcmp(m->mod, "perks") && weapon_perks_set_enabled) live = weapon_perks_set_enabled(v);
  else if(!strcmp(m->mod, "gear") && gear_mod_set_enabled) live = gear_mod_set_enabled(v);
  fprintf(stderr, LOGP "menu: %s %s (%s)\n", m->mod, v ? "ON" : "OFF", live ? "applied now" : "applies after a restart");
}

/* input: keyboard (hle_input.c key()) and pad (XInputGetState) */
static volatile int k_down; static double pad_t0; static int pad_was, pad_replay; static double replay_until; static int pad_open_latch;
void legendary_use_pressed(void) __attribute__((weak));   /* legendary.c: the player pressed use (F / hold X) */
int ext_menu_key_hook(uint32_t vk, int down){
  if(vk == 70 && down && legendary_use_pressed) legendary_use_pressed();   /* F = UseOrHoldToHack (observed, not swallowed) */
  if(!menu_enabled()) return 0;
  if(vk == 0x79){ if(down){ if(m_open) m_close = 1; else m_open = 1; } return 1; }   /* F10 */
  if(!m_open) return 0;
  if(vk == 38 || vk == 40){ if(down) m_step += vk == 40 ? 1 : -1; return 1; }
  if(vk == 13){ if(down) m_toggle = 1; return 1; }
  if(vk == 27){ if(down) m_close = 1; return 1; }
  return 0;
}
void ext_menu_pad_hook(uint32_t st){
  if(!st) return;
  { static double x0; static int xw, xsent; uint16_t bx = R16(st + 4); double tx = now_s();   /* X held 0.25 s = use (ReloadOrHoldToHackOrUse) */
    int xd = (bx & 0x4000) != 0; if(xd && !xw){ x0 = tx; xsent = 0; } if(xd && !xsent && tx - x0 >= 0.25){ xsent = 1; if(legendary_use_pressed) legendary_use_pressed(); } xw = xd; }
  if(!menu_enabled()) return;
  uint16_t b = R16(st + 4); double t = now_s();
  int dn = (b & 0x0020) != 0;   /* Back */
  if(dn && !pad_was) pad_t0 = t;
  if(dn && !pad_open_latch && t - pad_t0 >= 0.6){ pad_open_latch = 1; if(m_open) m_close = 1; else m_open = 1; }
  if(!dn && pad_was){ if(!pad_open_latch){ pad_replay = 1; replay_until = t + 0.1; } pad_open_latch = 0; }
  pad_was = dn; b &= (uint16_t)~0x0020;
  if(pad_replay){ if(t < replay_until) b |= 0x0020; else pad_replay = 0; }
  if(m_open){
    static int dl, al, bl; int16_t ly = (int16_t)R16(st + 10); static int sl;
    int d = b & 3; if(d && !dl) m_step += (b & 2) ? 1 : -1; dl = d;                 /* D-pad up 1 / down 2 */
    int s = ly > 20000 ? -1 : ly < -20000 ? 1 : 0; if(s && !sl) m_step += s; sl = s;   /* stick */
    int a = (b & 0x1000) != 0; if(a && !al) m_toggle = 1; al = a;
    int bb = (b & 0x2000) != 0; if(bb && !bl) m_close = 1; bl = bb;
    b &= (uint16_t)~(0x1000 | 0x2000 | 0x000F);
    W16(st + 8, 0); W16(st + 10, 0); W16(st + 12, 0); W16(st + 14, 0);
  }
  W16(st + 4, b);
}
/* game thread (legendary_tick calls it): consume the input */
static int g_open_state;
void ext_menu_tick(void){
  if(!menu_enabled()) return;
  if(m_open && !g_open_state){ g_open_state = 1; m_open = 1; m_close = 0; m_step = 0; m_toggle = 0; fprintf(stderr, LOGP "mod menu opened\n"); }
  if(!g_open_state){ m_open = 0; return; }
  if(m_close){ m_close = 0; m_open = 0; g_open_state = 0; fprintf(stderr, LOGP "mod menu closed\n"); return; }
  if(m_step){ int s = m_step; m_step = 0; m_sel = ((m_sel + s) % NROWS + NROWS) % NROWS; }
  if(m_toggle){ m_toggle = 0; row_toggle(m_sel); }
}
int ext_menu_is_open(void){ return g_open_state; }

/* ---- 5x7 font (columns of 7 bits, bit 0 = top) ---- */
static const struct { char c; uint8_t col[5]; } FONT[] = {
  {'A',{0x7E,0x09,0x09,0x09,0x7E}},{'B',{0x7F,0x49,0x49,0x49,0x36}},{'C',{0x3E,0x41,0x41,0x41,0x22}},{'D',{0x7F,0x41,0x41,0x22,0x1C}},
  {'E',{0x7F,0x49,0x49,0x49,0x41}},{'F',{0x7F,0x09,0x09,0x09,0x01}},{'G',{0x3E,0x41,0x49,0x49,0x7A}},{'H',{0x7F,0x08,0x08,0x08,0x7F}},
  {'I',{0x00,0x41,0x7F,0x41,0x00}},{'J',{0x20,0x40,0x41,0x3F,0x01}},{'K',{0x7F,0x08,0x14,0x22,0x41}},{'L',{0x7F,0x40,0x40,0x40,0x40}},
  {'M',{0x7F,0x02,0x0C,0x02,0x7F}},{'N',{0x7F,0x04,0x08,0x10,0x7F}},{'O',{0x3E,0x41,0x41,0x41,0x3E}},{'P',{0x7F,0x09,0x09,0x09,0x06}},
  {'Q',{0x3E,0x41,0x51,0x21,0x5E}},{'R',{0x7F,0x09,0x19,0x29,0x46}},{'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7F,0x01,0x01}},
  {'U',{0x3F,0x40,0x40,0x40,0x3F}},{'V',{0x1F,0x20,0x40,0x20,0x1F}},{'W',{0x3F,0x40,0x38,0x40,0x3F}},{'X',{0x63,0x14,0x08,0x14,0x63}},
  {'Y',{0x07,0x08,0x70,0x08,0x07}},{'Z',{0x61,0x51,0x49,0x45,0x43}},{'0',{0x3E,0x51,0x49,0x45,0x3E}},{'1',{0x00,0x42,0x7F,0x40,0x00}},
  {'2',{0x42,0x61,0x51,0x49,0x46}},{'3',{0x21,0x41,0x45,0x4B,0x31}},{'4',{0x18,0x14,0x12,0x7F,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
  {'6',{0x3C,0x4A,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},{'8',{0x36,0x49,0x49,0x49,0x36}},{'9',{0x06,0x49,0x49,0x29,0x1E}},
  {'.',{0x00,0x60,0x60,0x00,0x00}},{',',{0x00,0x80,0x60,0x00,0x00}},{':',{0x00,0x36,0x36,0x00,0x00}},{'-',{0x08,0x08,0x08,0x08,0x08}},
  {'/',{0x20,0x10,0x08,0x04,0x02}},{'(',{0x00,0x1C,0x22,0x41,0x00}},{')',{0x00,0x41,0x22,0x1C,0x00}},{'>',{0x41,0x22,0x14,0x08,0x00}},
  {'"',{0x00,0x07,0x00,0x07,0x00}},{'\'',{0x00,0x05,0x03,0x00,0x00}},{'!',{0x00,0x00,0x5F,0x00,0x00}},{'[',{0x00,0x7F,0x41,0x41,0x00}},
  {']',{0x00,0x41,0x41,0x7F,0x00}},{'+',{0x08,0x08,0x3E,0x08,0x08}},{'?',{0x02,0x01,0x51,0x09,0x06}},{'*',{0x2A,0x1C,0x7F,0x1C,0x2A}},
};
static const uint8_t *glyph(char c){ c = (char)toupper((unsigned char)c); for(unsigned i = 0; i < sizeof FONT / sizeof FONT[0]; i++) if(FONT[i].c == c) return FONT[i].col; return 0; }

/* render-thread rectangle list (built at Present) */
static int32_t *o_r; static float *o_c; static int o_n, o_max;
static void orect(int x, int y, int w, int h, float r, float g, float b){
  if(w <= 0 || h <= 0 || o_n >= o_max) return;
  o_r[4 * o_n] = x; o_r[4 * o_n + 1] = y; o_r[4 * o_n + 2] = w; o_r[4 * o_n + 3] = h; o_c[3 * o_n] = r; o_c[3 * o_n + 1] = g; o_c[3 * o_n + 2] = b; o_n++;
}
static int otext(int x, int y, int s, const char *t, float r, float g, float b){   /* returns the end x; one rect per vertical run */
  for(; *t; t++, x += 6 * s){
    const uint8_t *gl = glyph(*t); if(!gl) continue;
    for(int cx = 0; cx < 5; cx++){ uint8_t col = gl[cx]; int y0 = -1;
      for(int cy = 0; cy <= 7; cy++){ int on = cy < 7 && (col >> cy & 1);
        if(on && y0 < 0) y0 = cy; else if(!on && y0 >= 0){ orect(x + cx * s, y + y0 * s, s, (cy - y0) * s, r, g, b); y0 = -1; } } }
  }
  return x;
}
typedef struct { int W, H, eye_w, ui, ui_x, ui_y, ui_w, ui_h; } MenuLayout;   /* VsLayout prefix (vr_stereo.h) */
/* legendary.c pickup prompt (game thread sets, render thread draws while the menu is closed) */
static char g_pr1[64], g_pr2[64]; static volatile int g_pr_on;
void ext_prompt_set(const char *l1, const char *l2){
  if(!l1){ g_pr_on = 0; return; }
  if(g_pr_on && !strcmp(g_pr1, l1)) return;
  g_pr_on = 0; snprintf(g_pr1, sizeof g_pr1, "%s", l1); snprintf(g_pr2, sizeof g_pr2, "%s", l2 ? l2 : ""); g_pr_on = 1;
}
static void ui_rect(uint32_t w, uint32_t h, int *rx, int *ry, int *rw, int *rh){
  *rx = 0; *ry = 0; *rw = (int)w; *rh = (int)h;
  if(vr_stereo_ui_layout){ struct { int W, H, eye_w, ui, ui_x, ui_y, ui_w, ui_h; int pad[16]; } L; memset(&L, 0, sizeof L);
    if(vr_stereo_ui_layout((int)w, (int)h, &L) && L.ui && L.ui_w > 32 && L.ui_h > 32){ *rx = L.ui_x; *ry = L.ui_y; *rw = L.ui_w; *rh = L.ui_h; } }
}
int ext_menu_rects(uint32_t w, uint32_t h, int32_t *xywh, float *rgb, int max){
  if(!g_open_state){
    if(!g_pr_on) return 0;
    o_r = xywh; o_c = rgb; o_n = 0; o_max = max;
    int rx, ry, rw, rh; ui_rect(w, h, &rx, &ry, &rw, &rh);
    int s = rh / 180; if(s < 1) s = 1;
    int l1 = (int)strlen(g_pr1), l2 = (int)strlen(g_pr2), lw = (l1 > l2 ? l1 : l2) * 6 * s + 8 * s;
    int px = rx + (rw - lw) / 2, py = ry + rh * 62 / 100;
    orect(px, py, lw, 22 * s, 0.02f, 0.02f, 0.03f); orect(px, py, lw, s, 1.0f, 0.55f, 0.05f);
    otext(px + 4 * s + (lw - 8 * s - l1 * 6 * s) / 2, py + 3 * s, s, g_pr1, 1.0f, 0.62f, 0.10f);
    otext(px + 4 * s + (lw - 8 * s - l2 * 6 * s) / 2, py + 12 * s, s, g_pr2, 0.85f, 0.75f, 0.55f);
    return o_n;
  }
  o_r = xywh; o_c = rgb; o_n = 0; o_max = max;
  int rx, ry, rw, rh; ui_rect(w, h, &rx, &ry, &rw, &rh);
  int s = rh / 150; if(s < 1) s = 1;                     /* font pixel size */
  int lh = 10 * s, pw = 44 * 6 * s + 8 * s, ph = (NROWS + 6) * lh + 12 * s;
  if(pw > rw) pw = rw;
  int px = rx + (rw - pw) / 2, py = ry + (rh - ph) / 2; if(py < ry) py = ry;
  orect(px, py, pw, ph, 0.02f, 0.02f, 0.03f);                              /* panel */
  orect(px, py, pw, s, 1.0f, 0.55f, 0.05f); orect(px, py + ph - s, pw, s, 1.0f, 0.55f, 0.05f);   /* orange frame */
  orect(px, py, s, ph, 1.0f, 0.55f, 0.05f); orect(px + pw - s, py, s, ph, 1.0f, 0.55f, 0.05f);
  int x = px + 4 * s, y = py + 3 * s;
  otext(x, y, s, "BSI EXTENSIONS", 1.0f, 0.62f, 0.10f); y += lh + 2 * s;
  static const char *const lab[NROWS] = { "LEGENDARY DROPS", "  LOOT BEAM", "WEAPON WHEEL", "ECONOMY", "WEAPON PERKS", "GEAR MOD" };
  for(int r = 0; r < NROWS; r++){
    int rs, on = row_state(r, &rs);
    if(r == m_sel) orect(px + s, y - s, pw - 2 * s, lh, 0.20f, 0.12f, 0.02f);
    otext(x, y, s, r == m_sel ? ">" : " ", 1.0f, 0.8f, 0.3f);
    otext(x + 2 * 6 * s, y, s, lab[r], 0.92f, 0.92f, 0.92f);
    int xe = otext(x + 24 * 6 * s, y, s, on ? "ON" : "OFF", on ? 0.35f : 0.75f, on ? 0.95f : 0.30f, on ? 0.35f : 0.25f);
    if(rs) otext(xe + 2 * s * 6, y, s, "RESTART", 0.95f, 0.85f, 0.25f);
    y += lh;
  }
  y += s * 3; orect(px + 4 * s, y, pw - 8 * s, s, 0.4f, 0.4f, 0.4f); y += 3 * s;
  otext(x, y, s, "LEGENDARIES", 1.0f, 0.62f, 0.10f); y += lh;
  int st = legendary_get ? legendary_get("state") : 0, pk = legendary_get ? legendary_get("pickup") : 0;
  const char *nm = legendary_item_name ? legendary_item_name() : "THE CHOSEN ONE";
  if(st == 2){ int xe = otext(x, y, s, nm, 1.0f, 0.50f, 0.0f); otext(xe + 6 * s, y, s, "- OWNED", 0.7f, 0.7f, 0.7f); y += lh;
    char d[64]; snprintf(d, sizeof d, "\"%s\"", legendary_item_desc ? legendary_item_desc() : ""); otext(x + 2 * 6 * s, y, s, d, 0.85f, 0.75f, 0.55f); y += lh; }
  else { otext(x, y, s, pk ? "??? - DROPPED NEARBY, FOLLOW THE BEAM" : "??? - NOT FOUND YET", 0.55f, 0.55f, 0.55f); y += lh;
    otext(x + 2 * 6 * s, y, s, "\"THE PROPHET'S FLOCK KNEELS...\"", 0.5f, 0.45f, 0.35f); y += lh; }
  y += s * 2;
  otext(x, y, s, "UP/DOWN SELECT  A TOGGLE  B CLOSE", 0.6f, 0.6f, 0.6f);
  return o_n;
}
