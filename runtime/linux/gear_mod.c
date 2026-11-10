/* gear_mod.c -- BSI-Quest gear mod prototype: custom gear items + two extra runtime gear slots (Sky-Hook, Gun).

   WHAT IT DOES
   - Reads a text config (gear_mod.txt, format in /home/claude/bsi/gearmod/README.md) that defines custom items and which
     item sits in each of six "slots": hat, shirt, pants, boots (the game's four) plus hook (Sky-Hook) and gun (new).
   - Passive effects ("stat" lines): adjust FXDynamicGameplayAttribute BaseValues on the player pawn (AXPawn, 203
     attributes such as MeleeDamageModifierAttrib, WeaponReloadTimeModifierAttrib) or on every AXWeapon the player
     carries (ReloadTimeModifierAttrib, MaxAmmoCountAttrib ...). The change is applied while the item is equipped,
     re-applied when the game resets the value or the pawn changes, and undone when the item is unequipped.
     GetValue (f_00c40bf0) computes (BaseValue + sum adds) * product muls, so stacking with real gear works.
   - Event effects ("on" lines): the AXPawn damage handler (IXDamagee vtable slot, f_00971fa0, thiscall
     this=pawn+0x39c, args (Instigator, DamageInfo, X), ret 0xc) is wrapped. After the original runs we know:
     attacker, victim, melee (DamageInfo+0x28 & 8, the same test the game's own melee procs use in f_00939680) and
     kill (victim Health went from >0 to <=0). Matching effects fire one of the game's own gameplay procs through
     AXGameReplicationInfo::TriggerProcByName (f_0092a4e0, thiscall GRI, (FName, Instigator, Victim), ret 0x10) on the
     victim, the player, the attacker, or the nearest other enemy (chain).
   - "tune" lines retune a proc instance in the GRI proc list (property by name), e.g. turn the unused
     HackersDelight RestoreHealth proc into a 50% shield refill.
   - "donor" (optional): the item's name/description is written, in place, over an unused (cut MP/trash)
     XGearStaticEffectComponent of GlobalXItemDatabase, so a later UI step can hand that component out with the custom
     text. Longer strings are reallocated with the game's own allocator (appMalloc/appFree), so the engine frees them normally.

   WHAT IT DOES NOT DO (prototype)
   - No gear-menu UI: the Sky-Hook and Gun slots and the custom items exist only in the runtime, configured from the
     text file. The real gear menu keeps showing the four game slots and the real items.
   - It does not give the custom items to the inventory (the donor text is prepared for that next step).

   INTEGRATION (no in-place edits of the main tree; see the files in gearmod/patches/)
   - patches.c patch_call: 0xaf0480 (UXGameEngine::Tick) -> weak gear_mod_tick(c) before local_engine_tick_call;
     0x971fa0 -> weak gear_mod_call(c,t).
   - rt_linux.c ft_init: weak gear_mod_skip(a) keeps 0x971fa0 off the flat call table (so its vtable calls reach
     patch_call) when the mod is on.
   A build without gear_mod.o behaves exactly as before (all references weak).

   CONFIG FILE: $BSI_GEAR_MOD, else $BSI_GEAR_FILE, else <dir of $BSI_BLOOM_FILE>/gear_mod.txt, else
   /sdcard/BSI/gear_mod.txt. Re-read every 2 s when its mtime changes (adb push applies live). BSI_GEAR=0: off.
   BSI_PATCHES=0 also turns it off. Log lines start with "[gear]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

void call_indirect(CPU *c, uint32_t t);
typedef void (*gear_gfn)(CPU *);
gear_gfn rt_lookup(uint32_t a);

/* ---- guest addresses (BioShockInfinite.exe, Steam build used by the recompiler) ---- */
#define G_GNAMES      0x0139DFECu   /* TArray<FNameEntry*>; ANSI text at entry+0x10 */
#define G_GOBJECTS    0x0138C15Cu   /* TArray<UObject*> */
#define G_GENGINE     0x013AA024u   /* UEngine*: +0x1B0 GamePlayers (TArray<ULocalPlayer*>), LocalPlayer+0x2C Actor (PC) */
#define G_GWORLD      0x013AC3ECu   /* UWorld*: +0x3C PersistentLevel, Level+0x28 Actors.Data, Actors[0] = WorldInfo */
#define G_ITEMDB      0x01406B48u   /* UXItemDatabase* (GItemDatabase) */
#define F_DAMAGE      0x00971FA0u   /* AXPawn IXDamagee damage handler (vtables 0x11b13c8, 0x1259ac0, 0x1263ee8) */
#define F_TRIGGERPROC 0x0092A4E0u   /* AXGameReplicationInfo::TriggerProcByName(FName, Pawn* Instigator, Actor* Victim) */
#define PAWN_FROM_DAMAGEE 0x39Cu    /* this (IXDamagee subobject) = pawn + 0x39C */
#define WI_GAME       0x40Cu        /* AWorldInfo.Game */
#define GAME_GRI      0x2C0u        /* AGameInfo.GameReplicationInfo (as used by f_009391c0) */
#define GRI_PROCS     0x374u        /* AXGameReplicationInfo.GameplayProcs TArray<UXGameplayProc*> */
#define PROC_ID       0x28u         /* UXGameplayProc.ProcID (FName) */
/* UObject / reflection layout (sdk/Basic.h, tools/uobj.py) */
#define UO_OUTER 0x14u
#define UO_NAME  0x18u
#define UO_CLASS 0x20u
#define US_SUPER 0x34u
#define US_CHILDREN 0x38u
#define UF_NEXT 0x28u
#define UP_OFFSET 0x48u
#define UP_BOOLMASK 0x58u
/* XGearStaticEffectComponent */
#define GC_LOOKUPID 0x28u
#define GC_FRIENDLY 0x30u
#define GC_EFFNAME  0x3Cu
#define GC_EFFDESC  0x48u
#define GC_SHORTDESC 0x54u
/* FXDynamicGameplayAttribute */
#define DA_RECACHE 0x0u
#define DA_BASE    0x8u
#define DA_CURRENT 0xCu

#define LOGP "[gear] "

/* ---- config model ---- */
enum { SL_HAT, SL_SHIRT, SL_PANTS, SL_BOOTS, SL_HOOK, SL_GUN, SL_N };
static const char *const slot_name[SL_N] = { "hat", "shirt", "pants", "boots", "hook", "gun" };
static const int slot_cgs[SL_N] = { 10, 11, 13, 12, 14, 7 };   /* CharacterGearSlot: Brain, Heart, Lungs, Arms, Stomach, Pocket */
enum { EV_MELEE_HIT, EV_GUN_HIT, EV_ANY_HIT, EV_MELEE_KILL, EV_GUN_KILL, EV_ANY_KILL, EV_HURT, EV_N };
static const char *const ev_name[EV_N] = { "melee_hit", "gun_hit", "any_hit", "melee_kill", "gun_kill", "any_kill", "hurt" };
enum { TG_VICTIM, TG_SELF, TG_NEAREST, TG_ATTACKER };
static const char *const tg_name[] = { "victim", "self", "nearest", "attacker" };
enum { OP_ADD, OP_MUL, OP_SET };

typedef struct {
  int kind;                 /* 1 = stat, 2 = proc */
  int on_weapon;            /* stat: 0 pawn, 1 every carried AXWeapon */
  char attr[64]; int op; float val;
  int ev; float chance; char proc[64]; int target; float radius; float cooldown; double last;
} Eff;
typedef struct { char name[64]; char desc[200]; char donor[96]; int slot; int ne; Eff e[8]; } Item;
typedef struct { char proc[64]; char field[64]; float val; } Tune;
typedef struct {
  int enable, verbose, dbtext, nitems, ntunes;
  Item items[48]; Tune tunes[16];
  int equip[SL_N];          /* item index or -1 */
  char chain_exclude[4][32]; int nexcl;
  char selftest[64];        /* test only: fire this proc once on the player when a player pawn first appears */
} Config;

static Config g_cfg_a, g_cfg_b, *g_cfg;   /* double buffer: parse into the spare one, then swap */
static char g_file[512];
static time_t g_mtime; static off_t g_fsize;
static int g_state = -1;                 /* -1 not initialised, 0 off, 1 on */
static int g_hook_installed;             /* 0x971fa0 is off the flat table */
/* ext72 extension layer (ext_menu.c): built-in defaults when the config file is missing, menu switch (all weak) */
int ext_mod_default_on(const char *mod) __attribute__((weak));
int ext_mod_file_allowed(const char *mod) __attribute__((weak));
const char *ext_mod_builtin(const char *mod) __attribute__((weak));
void ext_mod_started(const char *mod, int on) __attribute__((weak));
static int g_builtin;        /* ext72: running with the built-in config (no file) */
static int g_menu_en = -1;   /* ext72: mod menu switch (-1 none, 0 off, 1 on) */
static uint32_t g_rng = 0x9E3779B9u;
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float frand(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng >> 8) * (1.0f / 16777216.0f); }

/* ---- config parsing ---- */
static int ntok(char *s, char **tok, int max){   /* whitespace split; "..." groups; key="a b" keeps the key */
  int n = 0;
  while(*s && n < max){
    while(*s && isspace((unsigned char)*s)) s++;
    if(!*s || *s == '#') break;
    char *start = s, *w = s;
    while(*s && !isspace((unsigned char)*s)){
      if(*s == '"'){ s++; while(*s && *s != '"') *w++ = *s++; if(*s == '"') s++; }
      else *w++ = *s++;
    }
    if(*s) s++;
    *w = 0; tok[n++] = start;
  }
  return n;
}
static int parse_slot(const char *s){
  static const struct { const char *a; int s; } al[] = {
    {"hat",SL_HAT},{"head",SL_HAT},{"brain",SL_HAT},{"shirt",SL_SHIRT},{"chest",SL_SHIRT},{"heart",SL_SHIRT},
    {"pants",SL_PANTS},{"legs",SL_PANTS},{"lungs",SL_PANTS},{"boots",SL_BOOTS},{"feet",SL_BOOTS},{"arms",SL_BOOTS},
    {"hook",SL_HOOK},{"skyhook",SL_HOOK},{"sky-hook",SL_HOOK},{"melee",SL_HOOK},{"gun",SL_GUN},{"weapon",SL_GUN} };
  for(unsigned i = 0; i < sizeof al / sizeof *al; i++) if(!strcasecmp(s, al[i].a)) return al[i].s;
  return -1;
}
static int parse_event(const char *s, int slot){
  if(!strcasecmp(s, "hit"))  return slot == SL_HOOK ? EV_MELEE_HIT : slot == SL_GUN ? EV_GUN_HIT : EV_ANY_HIT;
  if(!strcasecmp(s, "kill")) return slot == SL_HOOK ? EV_MELEE_KILL : slot == SL_GUN ? EV_GUN_KILL : EV_ANY_KILL;
  for(int i = 0; i < EV_N; i++) if(!strcasecmp(s, ev_name[i])) return i;
  return -1;
}
static const char *kv(char **tok, int n, const char *key){
  size_t k = strlen(key);
  for(int i = 0; i < n; i++) if(!strncasecmp(tok[i], key, k) && tok[i][k] == '=') return tok[i] + k + 1;
  return 0;
}
static int find_item(Config *c, const char *name){
  for(int i = 0; i < c->nitems; i++) if(!strcasecmp(c->items[i].name, name)) return i;
  return -1;
}
static void parse_config(Config *c, FILE *f){
  memset(c, 0, sizeof *c); c->enable = 1; c->verbose = 1; c->dbtext = 1;
  for(int s = 0; s < SL_N; s++) c->equip[s] = -1;
  char line[1024], *tok[24]; int ln = 0; Item *cur = 0;
  char pend_eq[SL_N][64]; memset(pend_eq, 0, sizeof pend_eq);
  while(fgets(line, sizeof line, f)){
    ln++;
    int n = ntok(line, tok, 24); if(!n) continue;
    if(!strcasecmp(tok[0], "enable") && n > 1) c->enable = atoi(tok[1]);
    else if(!strcasecmp(tok[0], "verbose") && n > 1) c->verbose = atoi(tok[1]);
    else if(!strcasecmp(tok[0], "dbtext") && n > 1) c->dbtext = atoi(tok[1]);
    else if(!strcasecmp(tok[0], "selftest") && n > 1) snprintf(c->selftest, sizeof c->selftest, "%s", tok[1]);
    else if(!strcasecmp(tok[0], "chain_exclude")){ for(int i = 1; i < n && c->nexcl < 4; i++) snprintf(c->chain_exclude[c->nexcl++], 32, "%s", tok[i]); }
    else if(!strcasecmp(tok[0], "item") && n > 1){
      if(c->nitems >= (int)(sizeof c->items / sizeof *c->items)){ fprintf(stderr, LOGP "line %d: too many items\n", ln); cur = 0; continue; }
      cur = &c->items[c->nitems]; memset(cur, 0, sizeof *cur);
      snprintf(cur->name, sizeof cur->name, "%s", tok[1]);
      const char *v = kv(tok, n, "slot"); cur->slot = v ? parse_slot(v) : -1;
      if(cur->slot < 0){ fprintf(stderr, LOGP "line %d: item \"%s\" needs slot=hat|shirt|pants|boots|hook|gun\n", ln, cur->name); cur = 0; continue; }
      if((v = kv(tok, n, "desc"))) snprintf(cur->desc, sizeof cur->desc, "%s", v);
      if((v = kv(tok, n, "donor"))) snprintf(cur->donor, sizeof cur->donor, "%s", v);
      c->nitems++;
    }
    else if(!strcasecmp(tok[0], "stat")){
      if(!cur || cur->ne >= 8 || n < 5){ fprintf(stderr, LOGP "line %d: 'stat pawn|weapon <Attrib> add|mul|set <value>' must follow an item\n", ln); continue; }
      Eff *e = &cur->e[cur->ne]; memset(e, 0, sizeof *e); e->kind = 1;
      e->on_weapon = !strcasecmp(tok[1], "weapon");
      snprintf(e->attr, sizeof e->attr, "%s", tok[2]);
      e->op = !strcasecmp(tok[3], "mul") ? OP_MUL : !strcasecmp(tok[3], "set") ? OP_SET : OP_ADD;
      e->val = (float)atof(tok[4]); cur->ne++;
    }
    else if(!strcasecmp(tok[0], "on")){
      if(!cur || cur->ne >= 8 || n < 3){ fprintf(stderr, LOGP "line %d: 'on <event> proc=<ProcID> ...' must follow an item\n", ln); continue; }
      Eff *e = &cur->e[cur->ne]; memset(e, 0, sizeof *e); e->kind = 2;
      e->ev = parse_event(tok[1], cur->slot);
      const char *v = kv(tok, n, "proc");
      if(e->ev < 0 || !v){ fprintf(stderr, LOGP "line %d: bad event or missing proc=\n", ln); continue; }
      snprintf(e->proc, sizeof e->proc, "%s", v);
      e->chance = (v = kv(tok, n, "chance")) ? (float)atof(v) : 1.0f;
      e->target = TG_VICTIM;
      if((v = kv(tok, n, "target"))){ for(int i = 0; i < 4; i++) if(!strcasecmp(v, tg_name[i])) e->target = i; }
      e->radius = (v = kv(tok, n, "radius")) ? (float)atof(v) : 1200.0f;
      e->cooldown = (v = kv(tok, n, "cooldown")) ? (float)atof(v) : 0.0f;
      cur->ne++;
    }
    else if(!strcasecmp(tok[0], "tune") && n > 3){
      if(c->ntunes < 16){ Tune *t = &c->tunes[c->ntunes++]; snprintf(t->proc, 64, "%s", tok[1]); snprintf(t->field, 64, "%s", tok[2]); t->val = (float)atof(tok[3]); }
    }
    else if(!strcasecmp(tok[0], "equip") && n > 2){
      int s = parse_slot(tok[1]);
      if(s < 0){ fprintf(stderr, LOGP "line %d: unknown slot %s\n", ln, tok[1]); continue; }
      snprintf(pend_eq[s], 64, "%s", tok[2]);
    }
    else fprintf(stderr, LOGP "line %d: unknown directive '%s'\n", ln, tok[0]);
  }
  for(int s = 0; s < SL_N; s++){
    if(!pend_eq[s][0] || !strcasecmp(pend_eq[s], "none")) continue;
    int i = find_item(c, pend_eq[s]);
    if(i < 0) fprintf(stderr, LOGP "equip %s: no item named \"%s\"\n", slot_name[s], pend_eq[s]);
    else if(c->items[i].slot != s) fprintf(stderr, LOGP "equip %s: \"%s\" is a %s item\n", slot_name[s], pend_eq[s], slot_name[c->items[i].slot]);
    else c->equip[s] = i;
  }
}
static void log_config(Config *c){
  fprintf(stderr, LOGP "config %s: %s, %d items, %d tunes\n", g_file, c->enable ? "enabled" : "disabled", c->nitems, c->ntunes);
  if(c->verbose > 1) for(int i = 0; i < c->nitems; i++){
    Item *it = &c->items[i];
    fprintf(stderr, LOGP "  item \"%s\" (%s, %d effects)%s%s\n", it->name, slot_name[it->slot], it->ne, it->donor[0] ? " donor " : "", it->donor);
  }
  for(int s = 0; s < SL_N; s++)
    fprintf(stderr, LOGP "  slot %-5s: %s\n", slot_name[s], c->equip[s] >= 0 ? c->items[c->equip[s]].name : "(empty)");
}
static void find_config_path(void){
  const char *e = getenv("BSI_GEAR_MOD"); if(!e || !*e) e = getenv("BSI_GEAR_FILE");
  if(e && *e){ snprintf(g_file, sizeof g_file, "%s", e); return; }
  const char *b = getenv("BSI_BLOOM_FILE");
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl){ snprintf(g_file, sizeof g_file, "%.*s/gear_mod.txt", (int)(sl - b), b); return; } }
  snprintf(g_file, sizeof g_file, "/sdcard/BSI/gear_mod.txt");
}
/* 1 = (re)loaded */
static int load_config(int force){
  struct stat st;
  if(stat(g_file, &st) != 0 && g_builtin){   /* ext72: built-in config (the example gear_mod.txt) */
    if(!force) return 0;
    const char *t = ext_mod_builtin ? ext_mod_builtin("gear") : 0; FILE *f = t ? fmemopen((void *)t, strlen(t), "r") : 0; if(!f) return 0;
    Config *spare = g_cfg == &g_cfg_a ? &g_cfg_b : &g_cfg_a;
    parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;
    g_cfg = spare; fprintf(stderr, LOGP "built-in config (ext72, no %s)\n", g_file); log_config(spare);
    return 1;
  }
  if(stat(g_file, &st) != 0){ if(g_cfg && g_cfg->enable){ fprintf(stderr, LOGP "%s gone: mod idle\n", g_file); g_cfg->enable = 0; } return 0; }
  g_builtin = 0;
  if(!force && st.st_mtime == g_mtime && st.st_size == g_fsize) return 0;
  FILE *f = fopen(g_file, "r"); if(!f) return 0;
  Config *spare = g_cfg == &g_cfg_a ? &g_cfg_b : &g_cfg_a;
  parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;   /* ext72 menu switch */
  /* keep proc cooldown timestamps across reloads */
  g_mtime = st.st_mtime; g_fsize = st.st_size; g_cfg = spare;
  log_config(spare);
  return 1;
}
static void gear_init(void){
  if(g_state >= 0) return;
  g_state = 0;
  const char *p = getenv("BSI_PATCHES"); if(p && !atoi(p)){ fprintf(stderr, LOGP "BSI_PATCHES=0: gear mod off\n"); return; }
  const char *e = getenv("BSI_GEAR"); if(e && !atoi(e)){ fprintf(stderr, LOGP "BSI_GEAR=0: gear mod off\n"); return; }
  find_config_path();
  struct stat st;
  if(stat(g_file, &st) != 0){
    if(!(ext_mod_default_on && ext_mod_default_on("gear"))){ fprintf(stderr, LOGP "no config at %s: gear mod off (set BSI_GEAR_MOD or push gear_mod.txt)\n", g_file); if(ext_mod_started) ext_mod_started("gear", 0); return; }
    g_builtin = 1;   /* ext72: on with the built-in config */
  } else if(ext_mod_file_allowed && !ext_mod_file_allowed("gear")){ if(ext_mod_started) ext_mod_started("gear", 0); return; }
  /* exe identity: the hooked entry points must hold the expected prologues */
  static const uint8_t dmg[] = { 0x6A, 0xFF, 0x68, 0x18, 0xB3, 0x04, 0x01 };   /* push -1; push 0x104b318 */
  static const uint8_t trg[] = { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x18 };   /* sub esp,8; push ebx; mov ebx,[esp+0x18] */
  for(unsigned i = 0; i < sizeof dmg; i++) if(R8(F_DAMAGE + i) != dmg[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: gear mod off\n", F_DAMAGE); return; }
  for(unsigned i = 0; i < sizeof trg; i++) if(R8(F_TRIGGERPROC + i) != trg[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: gear mod off\n", F_TRIGGERPROC); return; }
  static const uint8_t mal[] = { 0x8B, 0x0D, 0xC8, 0x1C, 0x37, 0x01 };   /* mov ecx,[GMalloc] (appMalloc and appFree) */
  for(unsigned i = 0; i < sizeof mal; i++) if(R8(0x482A80u + i) != mal[i] || R8(0x482AE0u + i) != mal[i]){ fprintf(stderr, LOGP "exe identity check failed at 482a80/482ae0: gear mod off\n"); return; }
  g_rng ^= (uint32_t)(now_s() * 1000.0);
  load_config(1);
  g_state = 1; if(ext_mod_started) ext_mod_started("gear", 1);
  fprintf(stderr, LOGP "gear mod loaded: 6 slots (hat shirt pants boots + runtime hook/gun), damage hook %08x, proc trigger %08x\n", F_DAMAGE, F_TRIGGERPROC);
}

/* ---- host-side UObject reflection (reads only) ---- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";   /* name entries are pool-packed: not 4-aligned */
  return (const char *)MEMP(e + 0x10);
}
static int fname_find(const char *s){   /* FName index, -1 if the name table does not have it (case-insensitive) */
  static struct { char s[64]; int idx; uint32_t n; } cache[64]; static int nc;
  uint32_t n = R32(G_GNAMES + 4);
  for(int i = 0; i < nc; i++) if(!strcasecmp(cache[i].s, s) && (cache[i].idx >= 0 || cache[i].n == n)) return cache[i].idx;
  uint32_t gn = R32(G_GNAMES); int found = -1;
  if(gptr(gn)) for(uint32_t i = 0; i < n; i++){ uint32_t e = R32(gn + 4 * i); if(e >= 0x10000u && e < 0xFFFF0000u && !strcasecmp((const char *)MEMP(e + 0x10), s)){ found = (int)i; break; } }
  int slot = -1; for(int i = 0; i < nc; i++) if(!strcasecmp(cache[i].s, s)) slot = i;
  if(slot < 0 && nc < 64) slot = nc++;
  if(slot >= 0){ snprintf(cache[slot].s, 64, "%s", s); cache[slot].idx = found; cache[slot].n = n; }
  return found;
}
/* UE3 FName text -> {Index, Number}: a trailing "_<digits>" (no leading zero) is split off as Number = digits + 1 */
static int fname_parse(const char *s, uint32_t *num){
  *num = 0;
  const char *u = strrchr(s, '_'); size_t L = strlen(s);
  if(u && u[1] && (u[1] != '0' || !u[2]) && strspn(u + 1, "0123456789") == strlen(u + 1) && L - (size_t)(u - s) <= 10){
    char base[64]; size_t bl = (size_t)(u - s); if(bl >= sizeof base) bl = sizeof base - 1; memcpy(base, s, bl); base[bl] = 0;
    int idx = fname_find(base);
    if(idx >= 0){ *num = (uint32_t)atoi(u + 1) + 1; return idx; }
  }
  return fname_find(s);
}
static const char *oname(uint32_t o){ return gptr(o) ? fname_str(R32(o + UO_NAME)) : "None"; }
static const char *cname(uint32_t o){ return gptr(o) ? oname(R32(o + UO_CLASS)) : "None"; }
static int is_a(uint32_t o, const char *cls){
  if(!gptr(o)) return 0;
  for(uint32_t c = R32(o + UO_CLASS), k = 0; gptr(c) && k < 40; c = R32(c + US_SUPER), k++) if(!strcmp(oname(c), cls)) return 1;
  return 0;
}
/* property of class cls (or a superclass): returns the UProperty, 0 if none */
static uint32_t find_prop(uint32_t cls, const char *name){
  static struct { uint32_t cls; char n[64]; uint32_t p; } cache[256]; static unsigned nc;
  for(unsigned i = 0; i < nc && i < 256; i++) if(cache[i].cls == cls && !strcmp(cache[i].n, name)) return cache[i].p;
  uint32_t found = 0;
  for(uint32_t c = cls, k = 0; gptr(c) && k < 40 && !found; c = R32(c + US_SUPER), k++)
    for(uint32_t f = R32(c + US_CHILDREN), m = 0; gptr(f) && m < 8000; f = R32(f + UF_NEXT), m++)
      if(!strcasecmp(oname(f), name) && strstr(cname(f), "Property")){ found = f; break; }
  unsigned slot = nc++ & 255; cache[slot].cls = cls; snprintf(cache[slot].n, 64, "%s", name); cache[slot].p = found;
  return found;
}
static int prop_off(uint32_t obj, const char *name, int dflt){
  uint32_t p = gptr(obj) ? find_prop(R32(obj + UO_CLASS), name) : 0;
  return p ? (int)R32(p + UP_OFFSET) : dflt;
}
static float rdf(uint32_t a){ return RF32(a); }
static void wrf(uint32_t a, float v){ WF32(a, v); }

/* ---- world access ---- */
static uint32_t world_info(void){
  uint32_t w = R32(G_GWORLD); if(!gptr(w)) return 0;
  uint32_t lvl = R32(w + 0x3C); if(!gptr(lvl)) return 0;
  uint32_t act = R32(lvl + 0x28); if(!gptr(act)) return 0;
  uint32_t wi = R32(act); return gptr(wi) ? wi : 0;
}
static uint32_t game_gri(void){   /* GameInfo.GameReplicationInfo (the path f_009391c0 uses), else WorldInfo.GRI */
  uint32_t wi = world_info(); if(!wi) return 0;
  uint32_t g = R32(wi + WI_GAME);
  uint32_t gri = gptr(g) ? R32(g + GAME_GRI) : 0;
  if(gptr(gri) && is_a(gri, "XGameReplicationInfo")) return gri;
  gri = R32(wi + 0x3E0);
  return gptr(gri) && is_a(gri, "XGameReplicationInfo") ? gri : 0;
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
static const char *top_name(uint32_t o){ if(!gptr(o)) return "-"; for(int k = 0; k < 8 && gptr(R32(o + UO_OUTER)); k++) o = R32(o + UO_OUTER); return oname(o); }
static float pawn_health(uint32_t p){ return rdf(p + prop_off(p, "Health", 0x2AC)); }
static void pawn_loc(uint32_t p, float v[3]){ int o = prop_off(p, "XPrivateLocalLocation", 0x44); v[0] = rdf(p + o); v[1] = rdf(p + o + 4); v[2] = rdf(p + o + 8); }
static uint32_t as_pawn(uint32_t a){   /* instigator may be a pawn or a controller */
  if(!gptr(a)) return 0;
  if(is_a(a, "Pawn")) return a;
  if(is_a(a, "Controller")){ uint32_t p = R32(a + prop_off(a, "Pawn", 0x1FC)); return gptr(p) ? p : 0; }
  return 0;
}

/* ---- passive stat enforcement ---- */
typedef struct { uint32_t obj, off; float orig, written; int live; char attr[48]; } Rec;
static Rec g_rec[128]; static int g_nrec;
static uint32_t g_last_pawn;
static unsigned g_stat_writes;

static int carried_weapons(uint32_t pawn, uint32_t *out, int max){
  int n = 0;
  uint32_t im = R32(pawn + prop_off(pawn, "InvManager", 0x314)); if(!gptr(im)) return 0;
  uint32_t it = R32(im + prop_off(im, "InventoryChain", 0x1FC));
  for(int k = 0; gptr(it) && k < 64 && n < max; k++){
    if(is_a(it, "XWeapon")) out[n++] = it;
    it = R32(it + prop_off(it, "Inventory", 0x1F8));
  }
  return n;
}
static void enforce_stats(Config *c){
  uint32_t pawn = player_pawn();
  if(pawn != g_last_pawn){
    if(g_nrec && c->verbose) fprintf(stderr, LOGP "player pawn changed %08x -> %08x: %d stat records dropped (re-applied on the new pawn)\n", g_last_pawn, pawn, g_nrec);
    g_nrec = 0; g_last_pawn = pawn;
    if(pawn && c->verbose) fprintf(stderr, LOGP "player pawn %08x (%s), health %.0f\n", pawn, cname(pawn), pawn_health(pawn));
  }
  if(!pawn) return;
  uint32_t objs[17]; int nobj = 0; objs[nobj++] = pawn; nobj += carried_weapons(pawn, objs + 1, 16);
  /* aggregate the equipped items' stat effects per (object, attribute) */
  struct { uint32_t obj, off; float add, mul, set; int hasset; const char *attr; } agg[96]; int na = 0;
  if(c->enable) for(int s = 0; s < SL_N; s++){
    if(c->equip[s] < 0) continue;
    Item *it = &c->items[c->equip[s]];
    for(int k = 0; k < it->ne; k++){
      Eff *e = &it->e[k]; if(e->kind != 1) continue;
      for(int o = e->on_weapon ? 1 : 0; o < (e->on_weapon ? nobj : 1); o++){
        uint32_t obj = objs[o]; uint32_t p = find_prop(R32(obj + UO_CLASS), e->attr);
        if(!p || strcmp(cname(p), "StructProperty")){
          static char warned[32][64]; static int nw; int seen = 0;
          for(int w = 0; w < nw; w++) if(!strcmp(warned[w], e->attr)) seen = 1;
          if(!seen && nw < 32){ snprintf(warned[nw++], 64, "%s", e->attr); fprintf(stderr, LOGP "\"%s\": %s has no attribute %s\n", it->name, cname(obj), e->attr); }
          continue;
        }
        uint32_t off = R32(p + UP_OFFSET); int a;
        for(a = 0; a < na; a++) if(agg[a].obj == obj && agg[a].off == off) break;
        if(a == na){ if(na >= 96) continue; agg[na].obj = obj; agg[na].off = off; agg[na].add = 0; agg[na].mul = 1; agg[na].hasset = 0; agg[na].attr = e->attr; na++; }
        if(e->op == OP_ADD) agg[a].add += e->val; else if(e->op == OP_MUL) agg[a].mul *= e->val; else { agg[a].set = e->val; agg[a].hasset = 1; }
      }
    }
  }
  for(int r = 0; r < g_nrec; r++) g_rec[r].live = 0;
  for(int a = 0; a < na; a++){
    int r; for(r = 0; r < g_nrec; r++) if(g_rec[r].obj == agg[a].obj && g_rec[r].off == agg[a].off) break;
    uint32_t base = agg[a].obj + agg[a].off + DA_BASE;
    float cur = rdf(base);
    if(r == g_nrec){ if(g_nrec >= 128) continue; g_nrec++; g_rec[r].obj = agg[a].obj; g_rec[r].off = agg[a].off; g_rec[r].orig = cur; g_rec[r].written = cur;
      snprintf(g_rec[r].attr, sizeof g_rec[r].attr, "%s", agg[a].attr); }
    else if(fabsf(cur - g_rec[r].written) > 1e-5f) g_rec[r].orig = cur;   /* the game re-initialised it */
    float want = agg[a].hasset ? agg[a].set : (g_rec[r].orig + agg[a].add) * agg[a].mul;
    if(cur != want){
      wrf(base, want); wrf(agg[a].obj + agg[a].off + DA_RECACHE, 0.0f);   /* ReCacheTime 0: next GetValue recomputes */
      g_stat_writes++;
      if(c->verbose) fprintf(stderr, LOGP "stat %s.%s base %g -> %g (orig %g)\n", cname(agg[a].obj), agg[a].attr, cur, want, g_rec[r].orig);
    }
    g_rec[r].written = want; g_rec[r].live = 1;
  }
  /* unequipped: restore, but only on objects still carried by the current pawn and only if the game did not touch it */
  for(int r = 0; r < g_nrec; ){
    if(g_rec[r].live){ r++; continue; }
    int carried = 0; for(int o = 0; o < nobj; o++) if(objs[o] == g_rec[r].obj) carried = 1;
    uint32_t base = g_rec[r].obj + g_rec[r].off + DA_BASE;
    if(carried && fabsf(rdf(base) - g_rec[r].written) <= 1e-5f){
      wrf(base, g_rec[r].orig); wrf(g_rec[r].obj + g_rec[r].off + DA_RECACHE, 0.0f);
      if(c->verbose) fprintf(stderr, LOGP "stat %s.%s restored to %g (unequipped)\n", cname(g_rec[r].obj), g_rec[r].attr, g_rec[r].orig);
    }
    g_rec[r] = g_rec[--g_nrec];
  }
}

/* ---- gameplay procs ---- */
static uint32_t g_last_gri; static int g_gri_nprocs;
/* apply every matching "tune" line to one UXGameplayProc object (GRI instance or loaded archetype) */
static void tune_proc(Config *c, uint32_t pr, int *done){
  uint32_t id = R32(pr + PROC_ID), idn = R32(pr + PROC_ID + 4);
  for(int t = 0; t < c->ntunes; t++){
    Tune *tu = &c->tunes[t]; uint32_t num; int idx = fname_parse(tu->proc, &num);
    if(idx < 0 || id != (uint32_t)idx || idn != num) continue;
    uint32_t p = find_prop(R32(pr + UO_CLASS), tu->field); if(!p) continue;
    uint32_t a = pr + R32(p + UP_OFFSET); const char *k = cname(p);
    if(!strcmp(k, "FloatProperty")){ fprintf(stderr, LOGP "tune %s (%s %s).%s %g -> %g\n", tu->proc, cname(pr), oname(pr), tu->field, rdf(a), tu->val); wrf(a, tu->val); done[t] = 1; }
    else if(!strcmp(k, "IntProperty")){ W32(a, (uint32_t)(int)tu->val); done[t] = 1; fprintf(stderr, LOGP "tune %s (%s).%s = %d\n", tu->proc, oname(pr), tu->field, (int)tu->val); }
    else if(!strcmp(k, "ByteProperty")){ W8(a, (uint8_t)(int)tu->val); done[t] = 1; fprintf(stderr, LOGP "tune %s (%s).%s = %d\n", tu->proc, oname(pr), tu->field, (int)tu->val); }
    else if(!strcmp(k, "BoolProperty")){ uint32_t m = R32(p + UP_BOOLMASK); if(m){ uint32_t o = R32(a); W32(a, tu->val != 0 ? (o | m) : (o & ~m)); done[t] = 1;
        fprintf(stderr, LOGP "tune %s (%s).%s %d -> %d (mask %x)\n", tu->proc, oname(pr), tu->field, (o & m) != 0, tu->val != 0, m); } }
  }
}
static int scan_gri(Config *c, uint32_t gri){
  uint32_t data = R32(gri + GRI_PROCS), n = R32(gri + GRI_PROCS + 4);
  g_gri_nprocs = gptr(data) && n < 4096 ? (int)n : 0;
  if(!g_gri_nprocs) return 0;   /* not filled yet: retried on the next pass */
  fprintf(stderr, LOGP "GRI %08x (%s, map %s): %d gameplay procs\n", gri, cname(gri), top_name(world_info()), g_gri_nprocs);
  if(c->verbose > 1){ char buf[2048]; int l = 0; buf[0] = 0;
    for(int i = 0; i < g_gri_nprocs; i++){ uint32_t pr = R32(data + 4 * i); if(!gptr(pr)) continue;
      l += snprintf(buf + l, sizeof buf - l > 0 ? sizeof buf - l : 0, "%s ", fname_str(R32(pr + PROC_ID))); if(l > 1900) break; }
    fprintf(stderr, LOGP "  procs: %s\n", buf); }
  /* every proc an equipped/defined item names must exist */
  for(int i = 0; i < c->nitems; i++) for(int k = 0; k < c->items[i].ne; k++){
    Eff *e = &c->items[i].e[k]; if(e->kind != 2) continue;
    uint32_t num; int idx = fname_parse(e->proc, &num), ok = 0;
    for(int j = 0; j < g_gri_nprocs && idx >= 0; j++){ uint32_t pr = R32(data + 4 * j); if(gptr(pr) && R32(pr + PROC_ID) == (uint32_t)idx && R32(pr + PROC_ID + 4) == num) ok = 1; }
    fprintf(stderr, LOGP "  \"%s\" %s -> proc %s: %s\n", c->items[i].name, ev_name[e->ev], e->proc, ok ? "found" : idx < 0 ? "NO SUCH NAME" : "not in this GRI's proc list");
  }
  /* retune */
  int done[16] = {0};
  for(int j = 0; j < g_gri_nprocs; j++){ uint32_t pr = R32(data + 4 * j); if(gptr(pr)) tune_proc(c, pr, done); }
  for(int t = 0; t < c->ntunes; t++) if(!done[t]) fprintf(stderr, LOGP "tune %s.%s: proc or field not found in this GRI\n", c->tunes[t].proc, c->tunes[t].field);
  return g_gri_nprocs;
}
static unsigned g_ev_count[EV_N], g_fired, g_fire_fail;
static int g_firing;
static void fire_proc(CPU *c, const char *proc, uint32_t inst, uint32_t victim){
  uint32_t gri = game_gri(), num; int idx = fname_parse(proc, &num);
  if(!gri || idx < 0 || !gptr(inst) || !gptr(victim)){ g_fire_fail++; return; }
  CPU save = *c;
  PUSH32(c, victim); PUSH32(c, inst); PUSH32(c, num); PUSH32(c, (uint32_t)idx);   /* FName {Index, Number} by value */
  c->ecx = gri;
  g_firing++; guest_call(c, F_TRIGGERPROC); g_firing--;   /* thiscall, ret 0x10 */
  uint32_t ok = c->eax;
  *c = save;
  g_fired++;
  if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "proc %s: instigator %08x -> %08x (%s) %s\n", proc, inst, victim, cname(victim), ok ? "triggered" : "not triggered");
}
static int excluded(Config *c, uint32_t p){
  const char *k = cname(p);
  uint32_t ctl = R32(p + prop_off(p, "Controller", 0x218));
  const char *kc = gptr(ctl) ? cname(ctl) : "";
  if(strstr(k, "Elizabeth") || strstr(kc, "Elizabeth")) return 1;   /* never chain onto Elizabeth */
  for(int i = 0; i < c->nexcl; i++) if(strstr(k, c->chain_exclude[i]) || strstr(kc, c->chain_exclude[i])) return 1;
  return 0;
}
static uint32_t nearest_enemy(Config *c, uint32_t from, uint32_t player, float radius){
  uint32_t wi = world_info(); if(!wi) return 0;
  float o[3]; pawn_loc(from, o);
  uint32_t best = 0; float bd = radius * radius;
  uint32_t p = R32(wi + prop_off(wi, "PawnList", 0x428));
  for(int k = 0; gptr(p) && k < 512; k++){
    if(p != from && p != player && is_a(p, "XPawn") && pawn_health(p) > 0 && !excluded(c, p)){
      float v[3]; pawn_loc(p, v); float dx = v[0] - o[0], dy = v[1] - o[1], dz = v[2] - o[2], d = dx * dx + dy * dy + dz * dz;
      if(d < bd){ bd = d; best = p; }
    }
    p = R32(p + prop_off(p, "NextPawn", 0x21C));
  }
  return best;
}
static void dispatch_event(CPU *c, Config *cf, int ev, uint32_t player, uint32_t victim, uint32_t attacker){
  g_ev_count[ev]++;
  if(g_firing || !cf->enable) return;
  double t = now_s();
  for(int s = 0; s < SL_N; s++){
    if(cf->equip[s] < 0) continue;
    Item *it = &cf->items[cf->equip[s]];
    for(int k = 0; k < it->ne; k++){
      Eff *e = &it->e[k]; if(e->kind != 2) continue;
      int match = e->ev == ev || (e->ev == EV_ANY_HIT && (ev == EV_MELEE_HIT || ev == EV_GUN_HIT)) ||
                  (e->ev == EV_ANY_KILL && (ev == EV_MELEE_KILL || ev == EV_GUN_KILL));
      if(!match || (e->cooldown > 0 && t - e->last < e->cooldown) || frand() >= e->chance) continue;
      uint32_t tgt = e->target == TG_SELF ? player : e->target == TG_ATTACKER ? attacker :
                     e->target == TG_NEAREST ? nearest_enemy(cf, victim, player, e->radius) : victim;
      if(!tgt){ if(cf->verbose) fprintf(stderr, LOGP "\"%s\" %s: no %s target\n", it->name, ev_name[ev], tg_name[e->target]); continue; }
      e->last = t;
      if(cf->verbose) fprintf(stderr, LOGP "\"%s\" (%s slot) on %s -> %s on %s\n", it->name, slot_name[s], ev_name[ev], e->proc, tg_name[e->target]);
      fire_proc(c, e->proc, player, tgt);
    }
  }
}

/* ---- donor text in GlobalXItemDatabase (in place, never grows an allocation) ---- */
static int g_db_done; static uint32_t g_db_ptr;
/* the game's allocator (GMalloc at [0x1371cc8]): appMalloc(size, align) 0x482a80, appFree(ptr) 0x482ae0, both cdecl */
#define F_APPMALLOC 0x00482A80u
#define F_APPFREE   0x00482AE0u
static uint32_t game_malloc(CPU *c, uint32_t n){ CPU save = *c; PUSH32(c, 8); PUSH32(c, n); guest_call(c, F_APPMALLOC); uint32_t p = c->eax; *c = save; return p; }
static void game_free(CPU *c, uint32_t p){ CPU save = *c; PUSH32(c, p); guest_call(c, F_APPFREE); *c = save; }
/* FString = TArray<TCHAR> {Data, Num (incl. terminator), Max}: overwrite in place when it fits, else reallocate with the
   game's own allocator (so the engine can free it later as usual) */
static void put_fstring(CPU *c, uint32_t f, const char *s){
  uint32_t d = R32(f), mx = R32(f + 8), n = (uint32_t)strlen(s);
  if(!d || mx < n + 1){
    uint32_t nd = game_malloc(c, 2 * (n + 1)); if(!gptr(nd)) return;
    W32(f, nd); W32(f + 8, n + 1);
    if(d) game_free(c, d);
    d = nd;
  }
  for(uint32_t i = 0; i < n; i++) W16(d + 2 * i, (uint8_t)s[i]);
  W16(d + 2 * n, 0); W32(f + 4, n + 1);
}
static void get_fstring(uint32_t f, char *out, int max){ uint32_t d = R32(f), n = R32(f + 4); if(!d || !n){ out[0] = 0; return; } g2host_w(d, out, max < (int)n + 1 ? max : (int)n + 1); }
static void db_patch(CPU *cpu, Config *c){
  uint32_t db = R32(G_ITEMDB);
  if(!gptr(db)) return;
  if(g_db_done && db == g_db_ptr) return;
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  int ncomp = 0, used[256]; memset(used, 0, sizeof used);
  int nproc = 0, tdone[16] = {0}; static uint32_t procs_seen[512], procs_num[512]; int nseen = 0;
  for(uint32_t i = 0; i < n && gptr(go); i++){
    uint32_t o = R32(go + 4 * i); if(!gptr(o)) continue;
    const char *k = cname(o);
    if(strstr(k, "GameplayProc") && is_a(o, "XGameplayProc")){   /* loaded proc objects (XEntry_p Arc_XGameplayProcs.*) */
      nproc++; if(nseen < 512){ procs_seen[nseen] = R32(o + PROC_ID); procs_num[nseen++] = R32(o + PROC_ID + 4); }
      tune_proc(c, o, tdone);
      continue;
    }
    if(strcmp(k, "XGearStaticEffectComponent")) continue;
    ncomp++; used[R8(o + GC_LOOKUPID)] = 1;
    if(!c->dbtext) continue;
    for(int k = 0; k < c->nitems; k++){
      Item *it = &c->items[k]; if(!it->donor[0]) continue;
      const char *dot = strchr(it->donor, '.');
      const char *nm = dot ? dot + 1 : it->donor;
      if(strcasecmp(oname(o), nm)) continue;
      if(dot && strncasecmp(oname(R32(o + UO_OUTER)), it->donor, (size_t)(dot - it->donor))) continue;
      char old[64]; get_fstring(o + GC_FRIENDLY, old, sizeof old);
      uint32_t id = R8(o + GC_LOOKUPID);
      put_fstring(cpu, o + GC_FRIENDLY, it->name); put_fstring(cpu, o + GC_EFFNAME, it->name);
      put_fstring(cpu, o + GC_EFFDESC, it->desc[0] ? it->desc : it->name); put_fstring(cpu, o + GC_SHORTDESC, it->desc[0] ? it->desc : it->name);
      char now[64]; get_fstring(o + GC_FRIENDLY, now, sizeof now);
      uint64_t gid = (1ull << 48) | ((uint64_t)id << 21) | ((uint64_t)slot_cgs[it->slot] << 3);
      fprintf(stderr, LOGP "db: donor %s.%s (static id %u) \"%s\" -> \"%s\"; gear id for slot %s would be %016llx\n",
              oname(R32(o + UO_OUTER)), oname(o), id, old, now, slot_name[it->slot], (unsigned long long)gid);
    }
  }
  if(!ncomp) return;   /* gear component packages not loaded yet: retried on the next pass */
  fprintf(stderr, LOGP "%d gameplay proc objects loaded\n", nproc);
  for(int t = 0; t < c->ntunes; t++) if(!tdone[t]) fprintf(stderr, LOGP "tune %s.%s: no loaded proc/field matches\n", c->tunes[t].proc, c->tunes[t].field);
  for(int i = 0; i < c->nitems; i++) for(int k = 0; k < c->items[i].ne; k++){
    Eff *e = &c->items[i].e[k]; if(e->kind != 2) continue;
    uint32_t num; int idx = fname_parse(e->proc, &num), ok = 0;
    for(int j = 0; j < nseen && idx >= 0; j++) if(procs_seen[j] == (uint32_t)idx && procs_num[j] == num) ok = 1;
    fprintf(stderr, LOGP "  \"%s\" %s -> proc %s: %s\n", c->items[i].name, ev_name[e->ev], e->proc, ok ? "loaded" : idx < 0 ? "NO SUCH NAME" : "name exists but no proc object has it");
  }
  g_db_done = 1; g_db_ptr = db;
  int nfree = 0; for(int i = 0; i < 256; i++) if(!used[i]) nfree++;
  fprintf(stderr, LOGP "item database %08x (%s): %d static effect components loaded, %d free static ids\n", db, cname(db), ncomp, nfree);
}

/* ---- entry points ---- */
/* rt_linux.c ft_init: 1 = keep this guest function off the flat call table so its calls reach patch_call */
int gear_mod_skip(uint32_t a){
  if(a != F_DAMAGE) return 0;
  gear_init();
  if(g_state != 1) return 0;
  g_hook_installed = 1;
  return 1;
}

/* patches.c patch_call: wrapped AXPawn damage handler */
int gear_mod_call(CPU *c, uint32_t t){
  if(t != F_DAMAGE || g_state != 1) return 0;
  gear_gfn orig = rt_lookup(t); if(!orig) return 0;
  uint32_t self = c->ecx, victim = self - PAWN_FROM_DAMAGEE;
  uint32_t inst = R32(c->esp + 4), info = R32(c->esp + 8);
  Config *cf = g_cfg;
  int vp = gptr(victim) && cf && cf->enable;
  float hp0 = vp ? pawn_health(victim) : 0;
  orig(c);                                   /* original: pops ret + 3 args; nested damage re-enters this wrapper */
  if(!vp) return 1;
  uint32_t player = player_pawn(); if(!player) return 1;
  uint32_t attacker = as_pawn(inst);
  float hp1 = pawn_health(victim);
  int melee = gptr(info) && (R32(info + 0x28) & 8);
  int killed = hp0 > 0 && hp1 <= 0;
  if(victim == player){
    if(attacker && attacker != player) dispatch_event(c, cf, EV_HURT, player, victim, attacker);
  } else if(attacker == player || inst == player_pc()){
    dispatch_event(c, cf, melee ? EV_MELEE_HIT : EV_GUN_HIT, player, victim, player);
    if(killed) dispatch_event(c, cf, melee ? EV_MELEE_KILL : EV_GUN_KILL, player, victim, player);
    if(cf->verbose > 1) fprintf(stderr, LOGP "hit %s %s health %.0f -> %.0f%s\n", melee ? "melee" : "ranged", cname(victim), hp0, hp1, killed ? " KILL" : "");
  }
  return 1;
}

/* patches.c patch_call: called on every UXGameEngine::Tick (game thread) before the original runs */
void gear_mod_tick(CPU *c){
  if(g_state < 0) gear_init();
  if(g_state != 1) return;
  static double next_poll, next_work, next_status; static long ticks; ticks++;
  double t = now_s();
  if(t >= next_poll){ next_poll = t + 2.0; if(load_config(0)){ g_db_done = 0; g_last_gri = 0; } }
  Config *cf = g_cfg; if(!cf) return;
  if(t < next_work) return;
  next_work = t + 0.5;
  if(ticks < 3) return;
  static double next_db; if(!g_db_done && t >= next_db){ next_db = t + 5.0; db_patch(c, cf); }
  uint32_t gri = game_gri();
  if(gri && gri != g_last_gri && scan_gri(cf, gri)) g_last_gri = gri;
  enforce_stats(cf);
  static uint32_t selftest_pawn;
  if(cf->selftest[0] && g_last_pawn && g_last_pawn != selftest_pawn && g_last_gri){
    selftest_pawn = g_last_pawn;
    fprintf(stderr, LOGP "selftest: firing %s on the player pawn %08x\n", cf->selftest, g_last_pawn);
    fire_proc(c, cf->selftest, g_last_pawn, g_last_pawn);
  }
  if(t >= next_status){
    next_status = t + 15.0;
    { static int dumped; uint32_t pc = player_pc();
      if(pc && dumped < 2){ dumped++; uint32_t cls = R32(pc + UO_CLASS), idx = R32(cls + UO_NAME), gn = R32(G_GNAMES), n = R32(G_GNAMES + 4), e = idx < n ? R32(gn + 4 * idx) : 0;
        fprintf(stderr, LOGP "names: GNames %08x n=%u; PC %08x class %08x name idx %u entry %08x -> \"%s\"\n", gn, n, pc, cls, idx, e, fname_str(idx)); } }
    { uint32_t wi = world_info(), pc = player_pc(), rp = pc ? R32(pc + prop_off(pc, "Pawn", 0x1FC)) : 0;
      fprintf(stderr, LOGP "world %s (%s), GRI %08x (%d procs), item db %08x %s, PC %08x (%s), PC.Pawn %08x (%s)\n",
              top_name(wi), wi ? cname(wi) : "-", game_gri(), g_gri_nprocs, R32(G_ITEMDB), g_db_done ? "patched" : "waiting for gear components",
              pc, pc ? cname(pc) : "-", rp, gptr(rp) ? cname(rp) : "-"); }
    fprintf(stderr, LOGP "status: tick %ld, hook %s, pawn %08x, stat records %d (writes %u), events melee_hit %u gun_hit %u melee_kill %u gun_kill %u hurt %u, procs fired %u (failed %u)\n",
            ticks, g_hook_installed ? "on" : "off", g_last_pawn, g_nrec, g_stat_writes, g_ev_count[EV_MELEE_HIT], g_ev_count[EV_GUN_HIT],
            g_ev_count[EV_MELEE_KILL], g_ev_count[EV_GUN_KILL], g_ev_count[EV_HURT], g_fired, g_fire_fail);
  }
}
/* ext72 mod menu: live switch (1 = applied now; 0 = the mod is not running, a restart is needed) */
int gear_mod_set_enabled(int on){
  if(g_state != 1) return 0;
  g_menu_en = on ? 1 : 0;
  if(!on){ if(g_cfg) g_cfg->enable = 0; } else load_config(1);
  return 1;
}
