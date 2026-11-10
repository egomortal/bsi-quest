/* weapon_wheel.c -- BioShock 1/2 style weapon wheel for the BSI-Quest static recompilation (prototype).

   WHAT IT DOES
   - Keeps every gun type the player picks up. BSI itself already keeps one AXWeapon instance per gun type in
     InvManager.InventoryItems[36]; only the "equipped" and "backup" indices are held. The game drops a world copy of
     the old gun when a third one is taken: the mod suppresses that drop (AXPawn::GetEquippedWeapon 0x94b640 returns
     NULL for the call inside AcquireWeapon, return address 0x967f13), so the old gun simply stays in the inventory,
     not held, with its own clip + reserve ammo.
   - Records every gun type (archetype name + clip + reserve) in a table, saved to a small text file, and gives the
     missing ones back (AcquireWeapon DoNotEquip + ammo write) a few seconds after a new player pawn appears (level
     change, save load, game restart).
   - The wheel: hold Tab (keyboard) or hold RB (pad; Quest: right grip) to open, pick with keys 1-8 / arrows or a
     stick direction, release to equip. B / Esc cancels. A short RB tap is replayed to the game (normal swap).
     The wheel is the game's own radial menu (UXWeaponRadialScreen, the vigor wheel's class, also used by Burial at
     Sea for guns): its archetype's 8 segments are pointed at the gun archetypes for the open call, then restored.
     More than 8 guns: pages (arrows past the end, D-pad left/right).
   - Equipping is InvManager EquipWeapon(inst, 0, 0) (0x932e60): the chosen gun replaces the equipped one; the
     replaced gun stays in the inventory with its ammo.

   HOOKS (all weak symbols in copies of the runtime files, see weaponwheel/patches/):
   - rt_linux.c call_indirect_slow: ww_patch_call(c, t) before patch_call  -> UXGameEngine::Tick 0xaf0480 (tick, game
     thread) and AXPawn::GetEquippedWeapon 0x94b640 (drop suppression).
   - rt_linux.c ft_init: ww_skip(a) keeps 0x94b640 off the flat call table so its vtable calls reach the slow path.
   - hle_input.c key(): ww_key_hook(vk, down) (1 = swallow);  XInputGetState: ww_pad_hook(XINPUT_STATE*).

   CONFIG: $BSI_WEAPON_WHEEL, else <dir of $BSI_BLOOM_FILE>/weapon_wheel.txt, else /sdcard/BSI/weapon_wheel.txt.
   No file = mod off (hooks stay passive). BSI_WHEEL=0 or BSI_PATCHES=0: off. Re-read every 2 s when changed.
   All log lines start with "[wheel]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

/* ---- guest addresses (BioShockInfinite.exe, Steam build used by the recompiler) ---- */
#define G_GNAMES      0x0139DFECu
#define G_GOBJECTS    0x0138C15Cu
#define G_GENGINE     0x013AA024u
#define G_GWORLD      0x013AC3ECu
#define G_WEAKGEN     0x0139DEC0u   /* weak object reference generation counter */
#define F_TICK        0x00AF0480u   /* UXGameEngine::Tick */
#define F_GETEQUIPPED 0x0094B640u   /* AXPawn::GetEquippedWeapon(spec), vtable +0x3b0, ret 4 */
#define RA_ACQUIRE_GETEQ 0x00967F13u
#define F_ACQUIRE     0x00967EE0u   /* AXPawn::AcquireWeapon(archetype, byte DoNotEquip), ret 8 */
#define F_EQUIP       0x00932E60u   /* AXInventoryManager EquipWeapon(XWeapon*, FullBody, TransferToBackup), ret 0xc */
#define F_FINDARCH    0x00954E40u   /* AXPawn find inventory by archetype(arch, bool chain), ret 8 */
#define F_LOADPKG     0x0050C880u   /* UObject::LoadPackage, cdecl (Outer, TCHAR* file, flags, 0) */
#define F_APPMALLOC   0x00482A80u
#define F_APPFREE     0x00482AE0u
#define F_OPENSCREEN  0x00981EA0u   /* thiscall (PC; archetype, -1) -> screen, ret 8 (XStartVigorRadialMenu without its checks) */
#define F_SETSEG      0x00C9C460u   /* UXWeaponRadialScreen set active segment (idx), ret 4 */
#define VT_RADIAL     0x0124AAF0u   /* UXWeaponRadialScreen vtable */
#define VF_POPULATE   0x12Cu
#define VF_CLOSE      0x144u
/* UObject layout */
#define UO_FLAGS 0x5Cu
#define UO_OUTER 0x14u
#define UO_NAME  0x18u
#define UO_CLASS 0x20u
#define UO_ARCH  0x24u
#define US_SUPER 0x34u
#define US_CHILDREN 0x38u
#define UF_NEXT 0x28u
#define UP_OFFSET 0x48u
/* gameplay offsets (sdk headers, checked at runtime by the inventory probe; reflection used where it can) */
#define PAWN_CONTROLLER 0x218u
#define PAWN_INVMGR 0x314u
#define PAWN_LOCKS  0x4CACu       /* WeaponInputLockedFlags */
#define PAWN_RHF    0x4CB0u       /* RightHandedWeaponFlags */
#define PC_FLAGS690 0x690u        /* bit 0x4000 blocks equips */
#define PC_ABILITY  0x1B10u       /* bit 0x200 bSwitchWeapon allowed */
#define PC_RADIAL_ARCH 0x17ECu    /* VigorRadialMenuArchetype */
#define IM_ITEMS    0x1FCu        /* InventoryItems[36] */
#define IM_EQ       0x2A0u
#define IM_BK       0x2A4u
#define IM_UNEQUIP  0x29Cu        /* bit0: weapons put away (EquipWeapon then only records the choice in PreviousWeaponState +0x290) */
#define IM_PREV_EQ  0x290u
#define W_TYPE      0x3E7u        /* 0 plasmid, 1-4 guns, 5 melee, 6 extra */
#define W_FLAGS     0x3F4u        /* 2 bIsHeldByPlayer */
#define W_AMMO      0x7C4u
#define W_SPARE     0x7ECu
#define SCR_SEGS    0x208u        /* RadialSegments TArray (stride 0x98) */
#define SCR_ACTIVE  0x214u
#define SCR_STATE   0x184u        /* 3 = closing */
#define ARCH_PAUSE  0xE8u         /* bit0 bPauseGameWhileActive */
#define SEG_STRIDE  0x98u
#define SEG_ANGLE   0x00u
#define SEG_WEAK    0x04u         /* weak ref: +0 pkg FName, +8 group FName, +0x10 object FName, +0x18 gen, +0x1c ptr */
#define SEG_ACTIVEF 0x60u

#define LOGP "[wheel] "
#define MAXW 32

/* ---- config ---- */
typedef struct { char arch[64]; int clip, spare; } Give;
typedef struct {
  int enable, verbose, key_open, pad_button, hold_ms, time_mode, unlock_all, no_drop, persist, force, native_ui;
  int max_open_ms;   /* vr72: the open wheel closes (nothing chosen) after this long without a selection change; 0 = never */
  float slow_factor, restore_delay; int unlock_clip, unlock_spare;
  Give give[24]; int ngive;
  char state_file[512];
} Config;
enum { TM_PAUSE, TM_SLOW, TM_NORMAL };
static Config g_cfg;
static char g_file[512];
static time_t g_mtime; static off_t g_fsize;
static int g_state = -1, g_hooked, g_dbg = -1;   /* g_dbg: BSI_WHEEL_DBG bits (test only): 1 no drop hook, 2 no tick, 4 no input hooks */
static int dbg(int bit){ if(g_dbg < 0){ const char *e = getenv("BSI_WHEEL_DBG"); g_dbg = e ? atoi(e) : 0; } return g_dbg & bit; }
/* values the input hooks read (input thread may differ from the game thread) */
static volatile int v_on, v_key = 9, v_padbtn = 0x200, v_hold_ms = 300;

/* ---- input state written by the hooks, consumed by the tick ---- */
static volatile int in_kb_down, in_pad_open, in_cancel, in_pick = -1, in_step, in_page, in_open;
static volatile float in_ang = -1.f;   /* stick angle in degrees (0 = up, clockwise), -1 = none */

/* ---- weapon table ---- */
typedef struct { char arch[64]; int clip, spare; int seen; } WEnt;
static WEnt g_w[MAXW]; static int g_nw, g_dirty; static double g_dirty_t;

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

/* ---- reflection helpers (same approach as gearmod/src/gear_mod.c) ---- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";
  return (const char *)MEMP(e + 0x10);
}
static int fname_find(const char *s){
  static struct { char s[64]; int idx; } cache[64]; static int nc;
  for(int i = 0; i < nc; i++) if(!strcasecmp(cache[i].s, s) && cache[i].idx >= 0) return cache[i].idx;
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4); int found = -1;
  if(gptr(gn)) for(uint32_t i = 0; i < n; i++){ uint32_t e = R32(gn + 4 * i); if(e >= 0x10000u && e < 0xFFFF0000u && !strcasecmp((const char *)MEMP(e + 0x10), s)){ found = (int)i; break; } }
  if(found >= 0 && nc < 64){ snprintf(cache[nc].s, 64, "%s", s); cache[nc].idx = found; nc++; }
  return found;
}
static const char *oname(uint32_t o){ return gptr(o) ? fname_str(R32(o + UO_NAME)) : "None"; }
static const char *cname(uint32_t o){ return gptr(o) ? oname(R32(o + UO_CLASS)) : "None"; }
static int is_a(uint32_t o, const char *cls){
  if(!gptr(o)) return 0;
  for(uint32_t c = R32(o + UO_CLASS), k = 0; gptr(c) && k < 40; c = R32(c + US_SUPER), k++) if(!strcmp(oname(c), cls)) return 1;
  return 0;
}
static uint32_t find_prop(uint32_t cls, const char *name){
  static struct { uint32_t cls; char n[48]; uint32_t p; } cache[64]; static unsigned nc;
  for(unsigned i = 0; i < nc && i < 64; i++) if(cache[i].cls == cls && !strcmp(cache[i].n, name)) return cache[i].p;
  uint32_t found = 0;
  for(uint32_t c = cls, k = 0; gptr(c) && k < 40 && !found; c = R32(c + US_SUPER), k++)
    for(uint32_t f = R32(c + US_CHILDREN), m = 0; gptr(f) && m < 8000; f = R32(f + UF_NEXT), m++)
      if(!strcasecmp(oname(f), name) && strstr(cname(f), "Property")){ found = f; break; }
  unsigned slot = nc++ & 63; cache[slot].cls = cls; snprintf(cache[slot].n, 48, "%s", name); cache[slot].p = found;
  return found;
}
static int prop_off(uint32_t obj, const char *name, int dflt){
  uint32_t p = gptr(obj) ? find_prop(R32(obj + UO_CLASS), name) : 0;
  return p ? (int)R32(p + UP_OFFSET) : dflt;
}
static uint32_t world_info(void){
  uint32_t w = R32(G_GWORLD); if(!gptr(w)) return 0;
  uint32_t lvl = R32(w + 0x3C); if(!gptr(lvl)) return 0;
  uint32_t act = R32(lvl + 0x28); if(!gptr(act)) return 0;
  uint32_t wi = R32(act); return gptr(wi) ? wi : 0;
}
static uint32_t player_pc(void){
  uint32_t eng = R32(G_GENGINE); if(!gptr(eng)) return 0;
  uint32_t gp = R32(eng + 0x1B0); if(!gptr(gp) || !R32(eng + 0x1B4)) return 0;
  uint32_t lp = R32(gp); if(!gptr(lp)) return 0;
  uint32_t pc = R32(lp + 0x2C); return gptr(pc) ? pc : 0;
}
static uint32_t player_pawn(void){
  uint32_t pc = player_pc(); if(!pc) return 0;
  uint32_t p = R32(pc + prop_off(pc, "Pawn", 0x1FC));
  return gptr(p) && is_a(p, "XPawn") ? p : 0;
}
static int is_gun(uint32_t it){ if(!gptr(it) || !is_a(it, "XWeapon")) return 0; int t = R8(it + W_TYPE); return t >= 1 && t <= 4; }
static const char *arch_name(uint32_t it){ return oname(R32(it + UO_ARCH)); }
/* host-side: the inventory instance whose archetype is PreCoalescedItemAssets.<name> */
static uint32_t inv_find(uint32_t pawn, const char *name){
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im)) return 0;
  for(int i = 0; i < 36; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(is_gun(it) && !strcmp(arch_name(it), name)) return it; }
  return 0;
}
static uint32_t find_arch(const char *name){   /* PreCoalescedItemAssets.<name>, an XWeapon archetype */
  static struct { char n[64]; uint32_t o; } cache[MAXW]; static int nc;
  for(int i = 0; i < nc; i++) if(!strcmp(cache[i].n, name)){ uint32_t o = cache[i].o; if(gptr(o) && !strcmp(oname(o), name) && is_a(o, "XWeapon")) return o; }
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  for(uint32_t i = 0; i < n && gptr(go); i++){
    uint32_t o = R32(go + 4 * i); if(!gptr(o)) continue;
    if(strcmp(oname(o), name) || strcmp(oname(R32(o + UO_OUTER)), "PreCoalescedItemAssets") || !is_a(o, "XWeapon")) continue;
    int s = -1; for(int k = 0; k < nc; k++) if(!strcmp(cache[k].n, name)) s = k;
    if(s < 0 && nc < MAXW) s = nc++;
    if(s >= 0){ snprintf(cache[s].n, 64, "%s", name); cache[s].o = o; }
    return o;
  }
  return 0;
}

/* ---- guest calls ---- */
static uint32_t game_malloc(CPU *c, uint32_t n){ CPU save = *c; PUSH32(c, 8); PUSH32(c, n); guest_call(c, F_APPMALLOC); uint32_t p = c->eax; *c = save; return p; }
static void game_free(CPU *c, uint32_t p){ CPU save = *c; PUSH32(c, p); guest_call(c, F_APPFREE); *c = save; }
static uint32_t gwstr(CPU *c, const char *s){ uint32_t n = (uint32_t)strlen(s), b = game_malloc(c, 2 * (n + 1)); if(gptr(b)) for(uint32_t i = 0; i <= n; i++) W16(b + 2 * i, (uint8_t)s[i]); return b; }
static int g_pkg_loads;   /* per pawn */
static void load_items_package(CPU *c){
  if(g_pkg_loads >= 2) return; g_pkg_loads++;
  uint32_t n0 = R32(G_GOBJECTS + 4), fb = gwstr(c, "CoalescedItems"); double t = now_s();
  CPU save = *c; PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, fb); PUSH32(c, 0); guest_call(c, F_LOADPKG); uint32_t pk = c->eax; *c = save;
  game_free(c, fb);
  fprintf(stderr, LOGP "LoadPackage(CoalescedItems) -> %08x in %.0f ms, objects %u -> %u\n", pk, (now_s() - t) * 1000, n0, R32(G_GOBJECTS + 4));
}
static uint32_t g_saved_rhf, g_saved_pcf, g_saved_locks, g_saved_unequip;
static void unlock(uint32_t pawn){   /* force mode only: lift the scene's weapon locks around one call */
  g_saved_locks = R32(pawn + PAWN_LOCKS); W32(pawn + PAWN_LOCKS, 0);
  g_saved_rhf = R32(pawn + PAWN_RHF); W32(pawn + PAWN_RHF, 0);
  uint32_t pc = R32(pawn + PAWN_CONTROLLER); g_saved_pcf = gptr(pc) ? R32(pc + PC_FLAGS690) : 0; if(gptr(pc)) W32(pc + PC_FLAGS690, g_saved_pcf & ~0x4000u);
  uint32_t im = R32(pawn + PAWN_INVMGR); g_saved_unequip = gptr(im) ? R32(im + IM_UNEQUIP) : 0; if(gptr(im)) W32(im + IM_UNEQUIP, g_saved_unequip & ~1u);
}
static void relock(uint32_t pawn){
  W32(pawn + PAWN_LOCKS, g_saved_locks); W32(pawn + PAWN_RHF, g_saved_rhf);
  uint32_t pc = R32(pawn + PAWN_CONTROLLER); if(gptr(pc)) W32(pc + PC_FLAGS690, (R32(pc + PC_FLAGS690) & ~0x4000u) | (g_saved_pcf & 0x4000u));
  uint32_t im = R32(pawn + PAWN_INVMGR); if(gptr(im)) W32(im + IM_UNEQUIP, (R32(im + IM_UNEQUIP) & ~1u) | (g_saved_unequip & 1u));
}
static int ammo_max(uint32_t w, const char *attr, int dflt){   /* FXDynamicGameplayAttribute: +8 BaseValue, +0xC CurrentValue */
  int o = prop_off(w, attr, -1); if(o < 0) return dflt;
  float v = RF32(w + o + 0xC); if(!(v >= 1.f && v <= 2000.f)) v = RF32(w + o + 8);
  return v >= 1.f && v <= 2000.f ? (int)v : dflt;
}
/* give a gun type (DoNotEquip) and set its ammo; returns the instance */
static uint32_t give_weapon(CPU *c, uint32_t pawn, const char *name, int clip, int spare, const char *why){
  uint32_t inst = inv_find(pawn, name);
  if(!inst){
    uint32_t a = find_arch(name);
    if(!a){ load_items_package(c); a = find_arch(name); }
    if(!a){ fprintf(stderr, LOGP "give %s (%s): archetype PreCoalescedItemAssets.%s not loaded\n", name, why, name); return 0; }
    unlock(pawn);   /* the probe showed AcquireWeapon(DoNotEquip) works with the locks lifted; restored right after */
    CPU save = *c; PUSH32(c, 1); PUSH32(c, a); c->ecx = pawn; guest_call(c, F_ACQUIRE); *c = save;
    relock(pawn);
    inst = inv_find(pawn, name);
    if(!inst){ fprintf(stderr, LOGP "give %s (%s): AcquireWeapon made no instance\n", name, why); return 0; }
    if(clip < 0) clip = ammo_max(inst, "MaxAmmoCountAttrib", 12);
    if(spare < 0) spare = ammo_max(inst, "MaxSpareAmmoCountAttrib", 60);
    W32(inst + W_AMMO, (uint32_t)clip); W32(inst + W_SPARE, (uint32_t)spare);
    fprintf(stderr, LOGP "gave %s (%s): instance %08x ammo %d/%d\n", name, why, inst, clip, spare);
  }
  return inst;
}
static void equip_weapon(CPU *c, uint32_t pawn, uint32_t w){
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im) || !gptr(w)) return;
  int eq0 = (int)R32(im + IM_EQ); uint32_t old = eq0 >= 0 && eq0 < 36 ? R32(im + IM_ITEMS + 4 * eq0) : 0;
  int deferred = !g_cfg.force && eq0 < 0 && (R32(im + IM_UNEQUIP) & 1);
  if(g_cfg.force) unlock(pawn);
  CPU save = *c; PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, w); c->ecx = im; guest_call(c, F_EQUIP); *c = save;
  if(g_cfg.force) relock(pawn);
  fprintf(stderr, LOGP "equip %s (ammo %d/%d) replacing %s (kept in inventory, ammo %d/%d) -> eq %d bk %d%s\n",
          arch_name(w), (int)R32(w + W_AMMO), (int)R32(w + W_SPARE), gptr(old) ? arch_name(old) : "nothing",
          gptr(old) ? (int)R32(old + W_AMMO) : 0, gptr(old) ? (int)R32(old + W_SPARE) : 0,
          (int)R32(im + IM_EQ), (int)R32(im + IM_BK), deferred ? " (weapons put away: recorded in PreviousWeaponState, applied when the game restores them)" : "");
  if(deferred) fprintf(stderr, LOGP "  PreviousWeaponState: equipped id %08x backup id %08x\n", R32(im + IM_PREV_EQ + 4), R32(im + IM_PREV_EQ + 8));
}

/* ---- weapon table + persistence ---- */
static int tab_find(const char *a){ for(int i = 0; i < g_nw; i++) if(!strcmp(g_w[i].arch, a)) return i; return -1; }
static int tab_add(const char *a, int clip, int spare){
  int i = tab_find(a); if(i >= 0) return i;
  if(g_nw >= MAXW) return -1;
  i = g_nw++; snprintf(g_w[i].arch, 64, "%s", a); g_w[i].clip = clip; g_w[i].spare = spare; g_dirty = 1;
  return i;
}
static void state_path(char *out, int n){
  if(g_cfg.state_file[0]){ snprintf(out, n, "%s", g_cfg.state_file); return; }
  const char *e = getenv("BSI_WEAPON_WHEEL_STATE"); if(e && *e){ snprintf(out, n, "%s", e); return; }
  const char *sl = strrchr(g_file, '/'); if(sl) snprintf(out, n, "%.*s/weapon_wheel_state.txt", (int)(sl - g_file), g_file);
  else snprintf(out, n, "weapon_wheel_state.txt");
}
static void state_load(void){
  char p[512]; state_path(p, sizeof p); FILE *f = fopen(p, "r");
  if(!f){ fprintf(stderr, LOGP "no saved state at %s\n", p); return; }
  char ln[256]; int n = 0;
  while(fgets(ln, sizeof ln, f)){ char a[64]; int cl, sp; if(sscanf(ln, "w %63s %d %d", a, &cl, &sp) == 3){ int i = tab_add(a, cl, sp); if(i >= 0){ g_w[i].clip = cl; g_w[i].spare = sp; n++; } } }
  fclose(f); g_dirty = 0;
  fprintf(stderr, LOGP "loaded %d weapon(s) from %s\n", n, p);
}
static void state_save(void){
  if(!g_cfg.persist) return;
  char p[512], tmp[520]; state_path(p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
  FILE *f = fopen(tmp, "w"); if(!f){ fprintf(stderr, LOGP "cannot write %s\n", tmp); return; }
  fprintf(f, "# weapon wheel state v1: w <archetype> <clip> <reserve>   (delete this file to forget every weapon)\n");
  for(int i = 0; i < g_nw; i++) fprintf(f, "w %s %d %d\n", g_w[i].arch, g_w[i].clip, g_w[i].spare);
  fclose(f); rename(tmp, p); g_dirty = 0;
  if(g_cfg.verbose) fprintf(stderr, LOGP "saved %d weapon(s) to %s\n", g_nw, p);
}
/* scan the inventory: new gun types join the table, ammo of every instance is mirrored */
static void scan_inventory(uint32_t pawn){
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im)) return;
  for(int i = 0; i < 36; i++){
    uint32_t it = R32(im + IM_ITEMS + 4 * i); if(!is_gun(it)) continue;
    const char *a = arch_name(it); int cl = (int)R32(it + W_AMMO), sp = (int)R32(it + W_SPARE);
    int k = tab_find(a);
    if(k < 0){ k = tab_add(a, cl, sp); if(k >= 0) fprintf(stderr, LOGP "new weapon type %s (ammo %d/%d): %d type(s) owned\n", a, cl, sp, g_nw); continue; }
    if(g_w[k].clip != cl || g_w[k].spare != sp){ g_w[k].clip = cl; g_w[k].spare = sp; if(!g_dirty){ g_dirty = 1; g_dirty_t = now_s(); } }
  }
}

/* ---- config ---- */
static int ntok(char *s, char **tok, int max){
  int n = 0;
  while(*s && n < max){ while(*s && isspace((unsigned char)*s)) s++; if(!*s || *s == '#') break;
    tok[n++] = s; while(*s && !isspace((unsigned char)*s)) s++; if(*s) *s++ = 0; }
  return n;
}
static int parse_key(const char *s){
  if(!strcasecmp(s, "tab")) return 9; if(!strcasecmp(s, "capslock")) return 20; if(!strcasecmp(s, "q")) return 'Q';
  if(!strcasecmp(s, "alt")) return 18; if(!strcasecmp(s, "shift")) return 16; if(!strcasecmp(s, "ctrl")) return 17;
  if(strlen(s) == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
  return (int)strtol(s, 0, 0);
}
static int parse_pad(const char *s){
  static const struct { const char *n; int b; } t[] = { {"rb",0x200},{"lb",0x100},{"rs",0x80},{"ls",0x40},{"back",0x20},{"y",0x8000},{"x",0x4000},{"dpad_up",1},{"dpad_down",2},{"none",0} };
  for(unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if(!strcasecmp(s, t[i].n)) return t[i].b;
  return (int)strtol(s, 0, 0);
}
static void parse_config(Config *c, FILE *f){
  memset(c, 0, sizeof *c);
  c->enable = 1; c->key_open = 9; c->pad_button = 0x200; c->hold_ms = 300; c->time_mode = TM_PAUSE; c->slow_factor = 0.25f;
  c->no_drop = 1; c->persist = 1; c->native_ui = 1; c->restore_delay = 5.f; c->unlock_clip = -1; c->unlock_spare = -1;
  c->max_open_ms = 5000;   /* vr72 */
  char ln[512];
  while(fgets(ln, sizeof ln, f)){
    char *t[8]; int n = ntok(ln, t, 8); if(!n) continue;
    if(!strcmp(t[0], "enable") && n > 1) c->enable = atoi(t[1]);
    else if(!strcmp(t[0], "verbose") && n > 1) c->verbose = atoi(t[1]);
    else if(!strcmp(t[0], "key") && n > 1) c->key_open = parse_key(t[1]);
    else if(!strcmp(t[0], "pad_button") && n > 1) c->pad_button = parse_pad(t[1]);
    else if(!strcmp(t[0], "hold_ms") && n > 1) c->hold_ms = atoi(t[1]);
    else if(!strcmp(t[0], "time") && n > 1) c->time_mode = !strcmp(t[1], "slow") ? TM_SLOW : !strcmp(t[1], "normal") ? TM_NORMAL : TM_PAUSE;
    else if(!strcmp(t[0], "slow_factor") && n > 1) c->slow_factor = (float)atof(t[1]);
    else if(!strcmp(t[0], "unlock_all") && n > 1) c->unlock_all = atoi(t[1]);
    else if(!strcmp(t[0], "unlock_ammo") && n > 2){ c->unlock_clip = atoi(t[1]); c->unlock_spare = atoi(t[2]); }
    else if(!strcmp(t[0], "give") && n > 1 && c->ngive < 24){ Give *g = &c->give[c->ngive++]; snprintf(g->arch, 64, "%s", t[1]); g->clip = n > 2 ? atoi(t[2]) : -1; g->spare = n > 3 ? atoi(t[3]) : -1; }
    else if(!strcmp(t[0], "no_drop") && n > 1) c->no_drop = atoi(t[1]);
    else if(!strcmp(t[0], "persist") && n > 1) c->persist = atoi(t[1]);
    else if(!strcmp(t[0], "restore_delay") && n > 1) c->restore_delay = (float)atof(t[1]);
    else if(!strcmp(t[0], "force") && n > 1) c->force = atoi(t[1]);
    else if(!strcmp(t[0], "native_ui") && n > 1) c->native_ui = atoi(t[1]);
    else if(!strcmp(t[0], "max_open_ms") && n > 1) c->max_open_ms = atoi(t[1]);   /* vr72 */
    else if(!strcmp(t[0], "state_file") && n > 1) snprintf(c->state_file, sizeof c->state_file, "%s", t[1]);
    else fprintf(stderr, LOGP "config: unknown line '%s'\n", t[0]);
  }
}
static void find_config_path(void){
  const char *e = getenv("BSI_WEAPON_WHEEL");
  if(e && *e){ snprintf(g_file, sizeof g_file, "%s", e); return; }
  const char *b = getenv("BSI_BLOOM_FILE");
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl){ snprintf(g_file, sizeof g_file, "%.*s/weapon_wheel.txt", (int)(sl - b), b); return; } }
  snprintf(g_file, sizeof g_file, "/sdcard/BSI/weapon_wheel.txt");
}
/* ext72 extension layer (ext_menu.c): built-in defaults when the config file is missing, menu switch (all weak) */
int ext_mod_default_on(const char *mod) __attribute__((weak));
int ext_mod_file_allowed(const char *mod) __attribute__((weak));
const char *ext_mod_builtin(const char *mod) __attribute__((weak));
void ext_mod_started(const char *mod, int on) __attribute__((weak));
static int g_builtin;        /* ext72: running with the built-in config (no file) */
static int g_menu_en = -1;   /* ext72: mod menu switch (-1 none, 0 off, 1 on) */
static int load_config(int force){
  struct stat st;
  if(stat(g_file, &st) != 0 && g_builtin){   /* ext72: built-in config (the example weapon_wheel.txt) */
    if(!force) return 0;
    const char *t = ext_mod_builtin ? ext_mod_builtin("wheel") : 0; FILE *f = t ? fmemopen((void *)t, strlen(t), "r") : 0; if(!f) return 0;
    parse_config(&g_cfg, f); fclose(f); if(g_menu_en == 0) g_cfg.enable = 0;
    v_key = g_cfg.key_open; v_padbtn = g_cfg.pad_button; v_hold_ms = g_cfg.hold_ms; v_on = g_cfg.enable;
    fprintf(stderr, LOGP "built-in config (ext72, no %s): enable %d key %d pad_button %04x hold %d ms no_drop %d persist %d native_ui %d\n",
            g_file, g_cfg.enable, g_cfg.key_open, g_cfg.pad_button, g_cfg.hold_ms, g_cfg.no_drop, g_cfg.persist, g_cfg.native_ui);
    return 1;
  }
  if(stat(g_file, &st) == 0) g_builtin = 0;
  if(stat(g_file, &st) != 0){ if(g_cfg.enable){ fprintf(stderr, LOGP "%s gone: wheel idle\n", g_file); g_cfg.enable = 0; v_on = 0; } return 0; }
  if(!force && st.st_mtime == g_mtime && st.st_size == g_fsize) return 0;
  FILE *f = fopen(g_file, "r"); if(!f) return 0;
  parse_config(&g_cfg, f); fclose(f); g_mtime = st.st_mtime; g_fsize = st.st_size; if(g_menu_en == 0) g_cfg.enable = 0;   /* ext72 menu switch */
  v_key = g_cfg.key_open; v_padbtn = g_cfg.pad_button; v_hold_ms = g_cfg.hold_ms; v_on = g_cfg.enable;
  fprintf(stderr, LOGP "config %s: enable %d key %d pad_button %04x hold %d ms time %s%s unlock_all %d gives %d no_drop %d persist %d force %d native_ui %d\n",
          g_file, g_cfg.enable, g_cfg.key_open, g_cfg.pad_button, g_cfg.hold_ms, g_cfg.time_mode == TM_SLOW ? "slow" : g_cfg.time_mode == TM_NORMAL ? "normal" : "pause",
          g_cfg.time_mode == TM_SLOW ? "" : "", g_cfg.unlock_all, g_cfg.ngive, g_cfg.no_drop, g_cfg.persist, g_cfg.force, g_cfg.native_ui);
  return 1;
}
static void ww_init(void){
  if(g_state >= 0) return;
  g_state = 0;
  const char *p = getenv("BSI_PATCHES"); if(p && !atoi(p)) return;
  const char *e = getenv("BSI_WHEEL"); if(e && !atoi(e)){ fprintf(stderr, LOGP "BSI_WHEEL=0: weapon wheel off\n"); return; }
  find_config_path();
  struct stat st;
  if(stat(g_file, &st) != 0){
    if(!(ext_mod_default_on && ext_mod_default_on("wheel"))){ fprintf(stderr, LOGP "no config at %s: weapon wheel off\n", g_file); if(ext_mod_started) ext_mod_started("wheel", 0); return; }
    g_builtin = 1;   /* ext72: on with the built-in config */
  } else if(ext_mod_file_allowed && !ext_mod_file_allowed("wheel")){ if(ext_mod_started) ext_mod_started("wheel", 0); return; }
  /* exe identity: prologues of the functions the mod calls or hooks */
  static const struct { uint32_t a; uint8_t b[6]; } id[] = {
    { F_ACQUIRE,     { 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x86 } },
    { F_EQUIP,       { 0x8B, 0x44, 0x24, 0x0C, 0x56, 0x57 } },
    { F_GETEQUIPPED, { 0x0F, 0xB6, 0x54, 0x24, 0x04, 0x33 } },
    { F_FINDARCH,    { 0x8B, 0x81, 0x14, 0x03, 0x00, 0x00 } },
    { F_OPENSCREEN,  { 0xE8, 0x5B, 0xC5, 0xFF, 0xFF, 0x8B } },
    { F_SETSEG,      { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56 } },
    { F_LOADPKG,     { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 } } };
  for(unsigned k = 0; k < sizeof id / sizeof id[0]; k++) for(int i = 0; i < 6; i++)
    if(R8(id[k].a + i) != id[k].b[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: weapon wheel off\n", id[k].a); return; }
  if(R32(VT_RADIAL + VF_POPULATE) != 0xC9EC00u || R32(VT_RADIAL + VF_CLOSE) != 0xB28A20u){ fprintf(stderr, LOGP "radial screen vtable mismatch: weapon wheel off\n"); return; }
  load_config(1);
  state_load();
  g_state = 1; if(ext_mod_started) ext_mod_started("wheel", 1);
  fprintf(stderr, LOGP "weapon wheel loaded: %d remembered weapon(s)\n", g_nw);
}

/* ---- input hooks (called from hle_input.c) ---- */
static double in_pad_t0; static int in_pad_was, in_pad_replay; static double in_replay_until;
int ww_key_hook(uint32_t vk, int down){
  if(!v_on || dbg(4)) return 0;
  if((int)vk == v_key){ in_kb_down = down; return 1; }
  if(!in_open) return 0;
  if(vk >= '1' && vk <= '8'){ if(down) in_pick = (int)(vk - '1'); return 1; }
  if(vk == 37 || vk == 39){ if(down) in_step += vk == 39 ? 1 : -1; return 1; }   /* arrows: previous / next weapon (pages) */
  if(vk == 27){ if(down) in_cancel = 1; return 1; }
  return 0;
}
void ww_pad_hook(uint32_t st){
  if(!v_on || !st || dbg(4)) return;
  uint16_t b = R16(st + 4); double t = now_s(); int pb = v_padbtn;
  if(pb){
    int dn = (b & pb) != 0;
    if(dn && !in_pad_was) in_pad_t0 = t;
    if(dn && !in_pad_open && t - in_pad_t0 >= v_hold_ms * 0.001) in_pad_open = 1;
    if(!dn && in_pad_was){ if(in_pad_open) in_pad_open = 0; else { in_pad_replay = 1; in_replay_until = t + 0.1; static int nl; if(nl++ < 10) fprintf(stderr, LOGP "pad: short tap (%.0f ms), replayed to the game as a 100 ms press\n", (t - in_pad_t0) * 1000); } }   /* tap: replay to the game */
    in_pad_was = dn;
    b &= (uint16_t)~pb;
    if(in_pad_replay){ if(t < in_replay_until) b |= (uint16_t)pb; else in_pad_replay = 0; }
  }
  if(in_open){
    int16_t lx = (int16_t)R16(st + 8), ly = (int16_t)R16(st + 10), rx = (int16_t)R16(st + 12), ry = (int16_t)R16(st + 14);
    float m1 = sqrtf((float)lx * lx + (float)ly * ly), m2 = sqrtf((float)rx * rx + (float)ry * ry);
    float x = m1 >= m2 ? lx : rx, y = m1 >= m2 ? ly : ry, m = m1 >= m2 ? m1 : m2;
    if(m > 16000.f){ float a = atan2f(x, y) * 57.29578f; if(a < 0) a += 360.f; in_ang = a; }
    if(b & 0x2000){ in_cancel = 1; }
    static int dl; int d = b & 0xC; if(d && !dl) in_page += (b & 8) ? 1 : -1; dl = d;
    b &= (uint16_t)~(0x2000 | 0xC);
    W16(st + 8, 0); W16(st + 10, 0); W16(st + 12, 0); W16(st + 14, 0);   /* sticks drive the wheel, not the camera */
  }
  W16(st + 4, b);
}

static void dump_inv(uint32_t pawn, const char *why);
/* ---- the wheel ---- */
static uint32_t g_screen;         /* live UXWeaponRadialScreen we opened (0 = native UI not shown) */
static double g_open_t;
static int g_open, g_page, g_sel, g_npage, g_latched;   /* g_latched: wait for the open input to be released */
static uint32_t g_pg_inst[8]; static int g_pg_tab[8];
static float g_seg_ang[8]; static int g_nseg;
static float g_dil_set; static int g_dil_on;

static int owned_list(uint32_t pawn, int *idx){   /* table entries that currently have an inventory instance */
  int n = 0; for(int i = 0; i < g_nw && n < MAXW; i++) if(inv_find(pawn, g_w[i].arch)) idx[n++] = i;
  return n;
}
static int screen_alive(void){ return gptr(g_screen) && R32(g_screen) == VT_RADIAL && R8(g_screen + SCR_STATE) != 3; }
static void close_screen(CPU *c){
  if(screen_alive()){ CPU save = *c; c->ecx = g_screen; guest_call(c, R32(VT_RADIAL + VF_CLOSE)); *c = save; }
  g_screen = 0;
}
static void set_time(int open){
  uint32_t wi = world_info(); if(!wi) return;
  int o = prop_off(wi, "TimeDilation", 0x398);
  if(open && g_cfg.time_mode == TM_SLOW){ g_dil_set = g_cfg.slow_factor > 0.01f ? g_cfg.slow_factor : 0.25f; WF32(wi + o, g_dil_set); g_dil_on = 1; }
  else if(!open && g_dil_on){ if(RF32(wi + o) == g_dil_set) WF32(wi + o, 1.f); g_dil_on = 0; }
}
static void log_screen(const char *why){
  if(!gptr(g_screen)) return;
  uint32_t segs = R32(g_screen + SCR_SEGS); int n = (int)R32(g_screen + SCR_SEGS + 4), act = 0;
  char buf[900]; int l = 0; buf[0] = 0;
  for(int i = 0; i < n && i < 8 && gptr(segs); i++){
    uint32_t s = segs + SEG_STRIDE * i; uint32_t w = R32(s + SEG_WEAK + 0x1C); int a = R32(s + SEG_ACTIVEF) & 1; act += a;
    l += snprintf(buf + l, sizeof buf - l, " [%d %.0fdeg %s%s]", i, RF32(s + SEG_ANGLE), gptr(w) ? oname(w) : "-", a ? " active" : "");
  }
  fprintf(stderr, LOGP "%s: screen %08x (%s) vt %08x state %d selected %d, %d segment(s), %d active:%s\n", why, g_screen, cname(g_screen), R32(g_screen),
          R8(g_screen + SCR_STATE), (int)R32(g_screen + SCR_ACTIVE), n, act, buf);
}
/* point the radial archetype (or the open screen) segments at this page's guns */
typedef struct { uint8_t b[0x20]; } WeakRef;
static void write_segments(uint32_t segs, int nseg, WeakRef *save){
  uint32_t gen = R32(G_WEAKGEN) & 0x7FFFFFFFu;
  for(int i = 0; i < nseg; i++){
    uint32_t wr = segs + SEG_STRIDE * i + SEG_WEAK;
    if(save) memcpy(save[i].b, MEMP(wr), 0x20);
    uint32_t keep = R32(wr + 0x18) & 1u;
    if(i < g_npage){
      uint32_t a = R32(g_pg_inst[i] + UO_ARCH); int fi = fname_find(oname(a));
      if(fi >= 0){ W32(wr + 0x10, (uint32_t)fi); W32(wr + 0x14, 0); }
      W32(wr + 0x1C, a);
    } else W32(wr + 0x1C, 0);
    W32(wr + 0x18, (gen << 1) | keep);
  }
}
static int build_page(uint32_t pawn){
  int idx[MAXW]; int n = owned_list(pawn, idx);
  int pages = (n + 7) / 8; if(pages < 1) pages = 1;
  if(g_page >= pages) g_page = 0; if(g_page < 0) g_page = pages - 1;
  g_npage = 0;
  for(int i = g_page * 8; i < n && g_npage < 8; i++){ g_pg_tab[g_npage] = idx[i]; g_pg_inst[g_npage] = inv_find(pawn, g_w[idx[i]].arch); g_npage++; }
  return n;
}
static int open_page(CPU *c, uint32_t pawn, uint32_t pc){
  int total = build_page(pawn);
  if(!g_npage){ fprintf(stderr, LOGP "open: no guns owned yet\n"); return 0; }
  /* default selection: the equipped gun if it is on this page */
  uint32_t im = R32(pawn + PAWN_INVMGR); int eq = (int)R32(im + IM_EQ); uint32_t cur = eq >= 0 && eq < 36 ? R32(im + IM_ITEMS + 4 * eq) : 0;
  if(g_sel < 0 || g_sel >= g_npage){ g_sel = 0; for(int i = 0; i < g_npage; i++) if(g_pg_inst[i] == cur) g_sel = i; }
  for(int i = 0; i < 8; i++) g_seg_ang[i] = 45.f * i;
  g_nseg = 8;
  char list[600]; int l = 0; list[0] = 0;
  for(int i = 0; i < g_npage; i++) l += snprintf(list + l, sizeof list - l, " %d:%s%s(%d/%d)", i + 1, i == g_sel ? "*" : "", g_w[g_pg_tab[i]].arch, (int)R32(g_pg_inst[i] + W_AMMO), (int)R32(g_pg_inst[i] + W_SPARE));
  fprintf(stderr, LOGP "wheel open, page %d of %d (%d gun types):%s\n", g_page + 1, (total + 7) / 8, total, list);
  g_screen = 0;
  if(!g_cfg.native_ui) return 1;
  int ao = prop_off(pc, "VigorRadialMenuArchetype", PC_RADIAL_ARCH);
  uint32_t arch = R32(pc + ao);
  if(!gptr(arch) || !is_a(arch, "XWeaponRadialScreen")){ fprintf(stderr, LOGP "no radial menu archetype at PC+%x (%08x %s): list-only wheel\n", ao, arch, cname(arch)); return 1; }
  int so = prop_off(arch, "RadialSegments", SCR_SEGS);
  uint32_t segs = R32(arch + so); int nseg = (int)R32(arch + so + 4); if(nseg > 8) nseg = 8;
  if(!gptr(segs) || nseg <= 0){ fprintf(stderr, LOGP "radial archetype %s has no segments: list-only wheel\n", oname(arch)); return 1; }
  g_nseg = nseg; for(int i = 0; i < nseg; i++) g_seg_ang[i] = RF32(segs + SEG_STRIDE * i + SEG_ANGLE);
  { static int once; if(!once){ once = 1; uint32_t wr = segs + SEG_WEAK;
      fprintf(stderr, LOGP "radial archetype %s.%s (%s) segments at +%x: %d, seg0 weak ref pkg %s group %s obj %s gen %08x ptr %08x\n", oname(R32(arch + UO_OUTER)), oname(arch), cname(arch), so, nseg,
              fname_str(R32(wr)), fname_str(R32(wr + 8)), fname_str(R32(wr + 0x10)), R32(wr + 0x18), R32(wr + 0x1C)); } }
  WeakRef save[8];
  write_segments(segs, nseg, save);
  /* populate only lights segments whose gun is held (or a vigor): mark this page's guns held just for the open call */
  uint32_t marked[8]; int nm = 0;
  for(int i = 0; i < g_npage && i < nseg; i++){ uint32_t w = g_pg_inst[i]; if(!(R32(w + W_FLAGS) & 2)){ W32(w + W_FLAGS, R32(w + W_FLAGS) | 2); marked[nm++] = w; } }
  uint32_t pause0 = R32(arch + ARCH_PAUSE);
  if(g_cfg.time_mode != TM_PAUSE) W32(arch + ARCH_PAUSE, pause0 & ~1u);
  CPU sv = *c; PUSH32(c, 0xFFFFFFFFu); PUSH32(c, arch); c->ecx = pc; guest_call(c, F_OPENSCREEN); uint32_t scr = c->eax; *c = sv;
  W32(arch + ARCH_PAUSE, pause0);
  for(int i = 0; i < nseg; i++) memcpy(MEMP(segs + SEG_STRIDE * i + SEG_WEAK), save[i].b, 0x20);
  g_screen = scr; g_open_t = now_s();
  if(!screen_alive()){ for(int i = 0; i < nm; i++) W32(marked[i] + W_FLAGS, R32(marked[i] + W_FLAGS) & ~2u);
    fprintf(stderr, LOGP "radial screen did not open (%08x %s): list-only wheel\n", scr, cname(scr)); g_screen = 0; return 1; }
  /* the screen copies the segments from the archetype; if populate ran before the copy took our values, patch and repopulate */
  uint32_t ss = R32(g_screen + SCR_SEGS); int sn = (int)R32(g_screen + SCR_SEGS + 4), ok = 0;
  for(int i = 0; i < g_npage && i < sn && gptr(ss); i++) if(R32(ss + SEG_STRIDE * i + SEG_WEAK + 0x1C) == R32(g_pg_inst[i] + UO_ARCH) && (R32(ss + SEG_STRIDE * i + SEG_ACTIVEF) & 1)) ok++;
  if(ok < g_npage && gptr(ss)){
    write_segments(ss, sn > 8 ? 8 : sn, 0);
    CPU s2 = *c; c->ecx = g_screen; guest_call(c, R32(VT_RADIAL + VF_POPULATE)); *c = s2;
    fprintf(stderr, LOGP "screen segments repopulated (%d of %d were ready)\n", ok, g_npage);
  }
  for(int i = 0; i < nm; i++) W32(marked[i] + W_FLAGS, R32(marked[i] + W_FLAGS) & ~2u);
  log_screen("native radial");
  if(!screen_alive()){ fprintf(stderr, LOGP "radial screen closed itself: list-only wheel\n"); g_screen = 0; }
  return 1;
}
static void highlight(CPU *c){
  if(!screen_alive() || g_sel < 0) return;
  int cur = (int)R32(g_screen + SCR_ACTIVE); if(cur == g_sel) return;
  /* f_00c9c460 calls into the segments' movie widgets (+0x5C); right after the open they do not exist yet (run ww11 crashed
     on a NULL widget when the highlight was set in the same tick as the open) */
  uint32_t segs = R32(g_screen + SCR_SEGS); int n = (int)R32(g_screen + SCR_SEGS + 4);
  if(!gptr(segs) || g_sel >= n || now_s() - g_open_t < 0.2) return;
  if(!gptr(R32(segs + SEG_STRIDE * g_sel + 0x5C)) || (cur >= 0 && cur < n && !gptr(R32(segs + SEG_STRIDE * cur + 0x5C)))) return;
  CPU sv = *c; PUSH32(c, (uint32_t)g_sel); c->ecx = g_screen; guest_call(c, F_SETSEG); *c = sv;
}
static int gate(uint32_t pawn, uint32_t pc, char *why, int n){
  uint32_t im = R32(pawn + PAWN_INVMGR);
  if(!gptr(im)){ snprintf(why, n, "no inventory manager"); return 0; }
  uint32_t wi = world_info();
  if(wi && gptr(R32(wi + prop_off(wi, "Pauser", 0x3C0)))){ snprintf(why, n, "game paused"); return 0; }
  if(g_cfg.force) return 1;
  if(R32(pawn + PAWN_LOCKS)){ snprintf(why, n, "weapon input locked (%x)", R32(pawn + PAWN_LOCKS)); return 0; }
  if(R32(pawn + PAWN_RHF)){ snprintf(why, n, "weapons put away by the scene (RH flags %x)", R32(pawn + PAWN_RHF)); return 0; }
  if(!(R32(pc + PC_ABILITY) & 0x200)){ snprintf(why, n, "weapon switching not allowed here"); return 0; }
  return 1;
}
/* vr72: why the wheel opened / closed, for the log and xr_glue's [xr] status line (ww_vr_status). In VR the right grip is
   RB: a hand resting on the grip used to read as a held RB (build <= 71: grip value > 0.5), the wheel opened after 0.3 s,
   took both sticks and paused the game ("can't move"). xr_glue now needs a firm squeeze; here the open wheel closes by
   itself after max_open_ms (default 5 s) without a selection change, and every open/close says why. */
static double v72_act_t, v72_open_since; static int v72_opens, v72_cl[5]; static const char *v72_last = "-";
enum { CL_RELEASED, CL_CANCEL, CL_TIMEOUT, CL_OTHER, CL_NOTOPENED };
static void v72_closed(int why, const char *txt){ v72_cl[why]++; v72_last = txt;
  if(why != CL_RELEASED) fprintf(stderr, LOGP "close: %s (open %.1f s)\n", txt, v72_open_since > 0 ? now_s() - v72_open_since : 0.0); v72_open_since = 0; }
int ww_vr_status(char *b, int n){
  int k = snprintf(b, n, "wheel %s (pad button %04x, hold %d ms, time %s, max open %d ms)", v_on ? "on" : "off", v_padbtn, v_hold_ms,
                   g_cfg.time_mode == TM_SLOW ? "slow" : g_cfg.time_mode == TM_NORMAL ? "normal" : "pause", g_cfg.max_open_ms);
  if(k > 0 && k < n) k += snprintf(b + k, n - k, ": %s, opened %d, closed released/cancelled/timeout/other %d/%d/%d/%d, not opened %d (last: %s)%s",
                                   g_open ? "OPEN now" : "closed", v72_opens, v72_cl[0], v72_cl[1], v72_cl[2], v72_cl[3], v72_cl[4], v72_last,
                                   in_pad_open ? ", pad button held past the hold time" : "");
  if(g_open && v72_open_since > 0 && k > 0 && k < n) snprintf(b + k, n - k, ", open for %.1f s", now_s() - v72_open_since);
  v72_opens = 0; memset(v72_cl, 0, sizeof v72_cl);
  return 1;
}
static void wheel_tick(CPU *c, uint32_t pawn){
  int want = in_kb_down || in_pad_open;
  if(!want) g_latched = 0;
  if(!g_open){
    in_cancel = 0; in_pick = -1; in_step = 0; in_page = 0; in_ang = -1.f;
    if(!want || g_latched) return;
    g_latched = 1;
    uint32_t pc = player_pc(); char why[96];
    if(!pc || !gate(pawn, pc, why, sizeof why)){ fprintf(stderr, LOGP "wheel not opened: %s\n", pc ? why : "no player controller"); v72_cl[CL_NOTOPENED]++; v72_last = "not opened"; return; }
    g_sel = -1; g_page = 0;
    if(!open_page(c, pawn, pc)){ v72_cl[CL_NOTOPENED]++; v72_last = "not opened (no guns)"; return; }
    g_open = 1; in_open = 1; set_time(1); highlight(c);
    v72_opens++; v72_open_since = v72_act_t = now_s();   /* vr72 */
    fprintf(stderr, LOGP "open: %s held %d ms (time %s while open; closes by itself after %d ms without a choice)\n", in_kb_down ? "key" : "pad button", v_hold_ms,
            g_cfg.time_mode == TM_SLOW ? "slow" : g_cfg.time_mode == TM_NORMAL ? "normal" : "paused", g_cfg.max_open_ms);
    return;
  }
  /* open */
  if(gptr(g_screen) && !screen_alive()){ fprintf(stderr, LOGP "radial screen went away (closed by the game)\n"); g_screen = 0; }
  if(in_cancel){ in_cancel = 0; close_screen(c); g_open = 0; in_open = 0; set_time(0); g_latched = 1; fprintf(stderr, LOGP "wheel cancelled\n"); v72_closed(CL_CANCEL, "cancelled (B)"); return; }
  int repage = 0, old = g_sel;
  if(in_pick >= 0){ int p = in_pick; in_pick = -1; if(p < g_npage) g_sel = p; }
  if(in_step){ int s = in_step; in_step = 0; g_sel += s;
    if(g_sel >= g_npage){ g_page++; g_sel = 0; repage = 1; } else if(g_sel < 0){ g_page--; g_sel = 7; repage = 1; } }
  if(in_page){ g_page += in_page; in_page = 0; g_sel = 0; repage = 1; }
  if(in_ang >= 0.f){ float a = in_ang; in_ang = -1.f; float best = 1e9f; int bi = -1;
    for(int i = 0; i < g_npage && i < g_nseg; i++){ float d = fabsf(a - g_seg_ang[i]); if(d > 180.f) d = 360.f - d; if(d < best){ best = d; bi = i; } }
    if(bi >= 0) g_sel = bi; }
  if(repage){ close_screen(c); uint32_t pc = player_pc(); if(pc) open_page(c, pawn, pc); if(g_sel >= g_npage) g_sel = g_npage - 1; old = -2; }
  if(g_sel != old) v72_act_t = now_s();   /* vr72: a selection change restarts the max_open_ms timer */
  if(g_cfg.max_open_ms > 0 && now_s() - v72_act_t > g_cfg.max_open_ms * 0.001){   /* vr72: never stay open (paused, sticks taken) indefinitely */
    close_screen(c); g_open = 0; in_open = 0; set_time(0); g_latched = 1;
    v72_closed(CL_TIMEOUT, "timeout: no selection change for max_open_ms; release the button to use the wheel again"); return; }
  if(g_sel != old && g_sel >= 0 && g_sel < g_npage)
    fprintf(stderr, LOGP "selected %d: %s (ammo %d/%d)\n", g_sel + 1, g_w[g_pg_tab[g_sel]].arch, (int)R32(g_pg_inst[g_sel] + W_AMMO), (int)R32(g_pg_inst[g_sel] + W_SPARE));
  highlight(c);
  if(want) return;
  /* released: equip the selection */
  close_screen(c); g_open = 0; in_open = 0; set_time(0); v72_closed(CL_RELEASED, "released");
  if(g_sel < 0 || g_sel >= g_npage){ fprintf(stderr, LOGP "wheel closed, nothing selected\n"); return; }
  uint32_t w = inv_find(pawn, g_w[g_pg_tab[g_sel]].arch);
  if(!w) w = give_weapon(c, pawn, g_w[g_pg_tab[g_sel]].arch, g_w[g_pg_tab[g_sel]].clip, g_w[g_pg_tab[g_sel]].spare, "restore on select");
  if(w){ equip_weapon(c, pawn, w); if(g_cfg.verbose) dump_inv(pawn, "after equip"); }
}

/* ---- tick ---- */
static uint32_t g_pawn; static double g_pawn_t, g_restore_at; static int g_restored;
static void dump_inv(uint32_t pawn, const char *why){
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im)) return;
  char buf[1200]; int l = 0; buf[0] = 0;
  for(int i = 0; i < 36; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(!is_gun(it)) continue;
    l += snprintf(buf + l, sizeof buf - l, " [%d]%s%s %d/%d", i, arch_name(it), (R32(it + W_FLAGS) & 2) ? "(held)" : "", (int)R32(it + W_AMMO), (int)R32(it + W_SPARE)); }
  fprintf(stderr, LOGP "inventory (%s): eq %d bk %d guns:%s\n", why, (int)R32(im + IM_EQ), (int)R32(im + IM_BK), l ? buf : " none");
}
static void restore_and_give(CPU *c, uint32_t pawn){
  int n = 0;
  if(g_cfg.persist) for(int i = 0; i < g_nw; i++) if(!inv_find(pawn, g_w[i].arch) && give_weapon(c, pawn, g_w[i].arch, g_w[i].clip, g_w[i].spare, "restore")) n++;
  static const char *const all[] = { "PistolFounder", "CarbineFounder", "MachineGunFounder", "ShotgunFounder", "HandCannonFounder", "SniperRifleFounder",
                                     "RPGFounder", "FlakCannonFounder", "CarbineVP", "MachineGunVP", "ShotgunVP", "FlakCannonVP", "GatlingGun" };
  for(int i = 0; i < g_cfg.ngive; i++) if(!inv_find(pawn, g_cfg.give[i].arch) && give_weapon(c, pawn, g_cfg.give[i].arch, g_cfg.give[i].clip, g_cfg.give[i].spare, "give")) n++;
  if(g_cfg.unlock_all) for(unsigned i = 0; i < sizeof all / sizeof all[0]; i++) if(!inv_find(pawn, all[i]) && give_weapon(c, pawn, all[i], g_cfg.unlock_clip, g_cfg.unlock_spare, "unlock_all")) n++;
  fprintf(stderr, LOGP "restore/give pass: %d weapon(s) added\n", n);
  scan_inventory(pawn);
  dump_inv(pawn, "after restore");
}
static void ww_tick(CPU *c){
  ww_init();
  if(g_state != 1) return;
  double t = now_s();
  static double next_cfg; if(t >= next_cfg){ next_cfg = t + 2.0; load_config(0); }
  if(!g_cfg.enable){ if(g_open){ close_screen(c); g_open = 0; in_open = 0; set_time(0); v72_closed(CL_OTHER, "wheel disabled"); } return; }
  uint32_t pawn = player_pawn();
  if(!pawn){ if(g_open){ g_screen = 0; g_open = 0; in_open = 0; v72_closed(CL_OTHER, "no player pawn (level change / death)"); } if(g_pawn){ g_pawn = 0; if(g_dirty) state_save(); } return; }
  if(pawn != g_pawn){
    g_pawn = pawn; g_pawn_t = t; g_restored = 0; g_pkg_loads = 0; g_restore_at = t + g_cfg.restore_delay;
    fprintf(stderr, LOGP "player pawn %08x (%s); restore/give in %.0f s\n", pawn, cname(pawn), g_cfg.restore_delay);
  }
  if(!g_restored && t >= g_restore_at){ g_restored = 1; restore_and_give(c, pawn); }
  static double next_scan; if(g_restored && t >= next_scan){ next_scan = t + 0.5; scan_inventory(pawn);
    if(g_dirty && t - g_dirty_t >= 2.0) state_save();
    if(g_cfg.verbose >= 2){ static double nd; if(t >= nd){ nd = t + 10; dump_inv(pawn, "periodic"); } } }
  if(g_restored) wheel_tick(c, pawn);
}

/* ---- runtime entry points (weak references in the patched rt_linux.c) ---- */
int ww_skip(uint32_t a){
  if(a != F_GETEQUIPPED) return 0;
  ww_init(); if(g_state != 1 || dbg(1)) return 0;
  if(!g_hooked) fprintf(stderr, LOGP "pickup drop hook installed (AXPawn::GetEquippedWeapon %08x kept on the slow path)\n", F_GETEQUIPPED);
  g_hooked = 1; return 1;
}
int ww_patch_call(CPU *c, uint32_t t){
  if(t == F_TICK){ if(!dbg(2)) ww_tick(c); return 0; }   /* observe only: the engine tick itself runs normally */
  if(t != F_GETEQUIPPED || !g_hooked) return 0;
  /* BioShock 1 rule: taking a new gun never drops the old one (it stays in the inventory, not held) */
  if(g_state == 1 && g_cfg.enable && g_cfg.no_drop && R32(c->esp) == RA_ACQUIRE_GETEQ && c->ecx == g_pawn && g_pawn){
    static unsigned n; if(n++ < 20) fprintf(stderr, LOGP "pickup: old gun kept in the inventory (world drop suppressed)\n");
    c->eax = 0; c->esp += 8;   /* ret 4 */
    return 1;
  }
  return 0;
}
/* ext72 mod menu: live switch (1 = applied now; 0 = the mod is not running, a restart is needed). Off: the wheel stops opening
   and a taken gun drops the old one again (game rule); guns already kept stay in the inventory. */
int ww_set_enabled(int on){
  if(g_state != 1) return 0;
  g_menu_en = on ? 1 : 0;
  if(!on){ g_cfg.enable = 0; v_on = 0; } else load_config(1);
  return 1;
}
int ww_is_open(void){ return g_open; }   /* ext72: the loot beam hides while the wheel is open */
