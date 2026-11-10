/* weapon_perks.c -- BSI-Quest weapon perks prototype: every weapon gets perk slots (2 by default) that hold enhancements.

   WHAT IT DOES
   - Reads a text config (weapon_perks.txt, format in /home/claude/bsi/weaponperks/README.md) that defines perks and
     assigns up to two perks to each weapon class (archetype name: PistolFounder, CarbineVP, SkyhookMelee ...).
   - Perks are active only while their weapon is in hand: the held gun (InvManager.InventoryItems[EquippedWeaponIndex])
     and the Sky-Hook (pawn.CurrentDedicatedMeleeWeapon, always in hand for melee). The held gun is polled every tick,
     so a switch by the game, by the weapon wheel or by a test override removes the old gun's perks and applies the new
     gun's perks within one frame.
   - Passive perk lines ("stat weapon|pawn <Attrib> add|mul|set <v>") change FXDynamicGameplayAttribute BaseValues on the
     held weapon or on the player pawn. FireMode_<n>_<Attrib> names (FireTimeAttrib, BurstShotsNum, ShotCostAttrib,
     WeaponCooldownTimeAttrib) are resolved inside AXWeapon.FireMode[n], which the gear mod cannot reach.
   - Event perk lines ("on <event> ...") react to hits, kills, headshots, the last round of a clip, reloads, equips and
     getting hurt, and fire the game's procs (TriggerProcByName) or native actions: extra damage on the victim or the
     nearest other enemy (the game's DealDamage), a radial blast (DealRadialDamage), an ammo refund, slow motion
     (WorldInfo.TimeDilation) or a timed stat boost.

   COEXISTENCE
   - Stat layering with the gear mod: both mods write BaseValues. To keep them from compounding each other's changes,
     this mod removes its own changes before gear_mod_tick runs and re-applies them right after (weapon_perks_tick wraps
     gear_mod_tick; nothing else runs in between), so each mod only ever sees its own writes.
   - Damage hook: weapon_perks_call wraps gear_mod_call for 0x971fa0 (which runs the original), so both see every hit.
   - Weapon wheel: no hook of its own is needed. A wheel swap changes EquippedWeaponIndex, which the poll sees. Records on
     a weapon object are only written back while that object is still in the player's inventory.

   INTEGRATION (patches/weapon_perks_*.patch, applied after the gear mod's patches; all references weak)
   - patches.c 0xaf0480: weapon_perks_tick(c) replaces the gear_mod_tick(c) call (it calls gear_mod_tick itself);
     0x971fa0: weapon_perks_call(c,t) is tried before gear_mod_call (it calls gear_mod_call itself).
   - rt_linux.c ft_init: weapon_perks_skip(a) keeps 0x971fa0 off the flat call table.

   CONFIG FILE: $BSI_WEAPON_PERKS, else <dir of $BSI_BLOOM_FILE>/weapon_perks.txt, else /sdcard/BSI/weapon_perks.txt.
   Re-read every 2 s when it changes. BSI_PERKS=0 or BSI_PATCHES=0: off. Log lines start with "[perk]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

typedef void (*perk_gfn)(CPU *);
perk_gfn rt_lookup(uint32_t a);

/* ---- guest addresses ---- */
#define G_GNAMES      0x0139DFECu
#define G_GOBJECTS    0x0138C15Cu
#define G_GENGINE     0x013AA024u
#define G_GWORLD      0x013AC3ECu
#define F_DAMAGE      0x00971FA0u   /* AXPawn IXDamagee handler: thiscall(this=pawn+0x39C, Instigator, UDamageType*, DamageEvent*), ret 0xC */
#define F_TRIGGERPROC 0x0092A4E0u   /* AXGameReplicationInfo::TriggerProcByName(FName, Pawn* Instigator, Actor* Victim), ret 0x10 */
#define F_DEALDAMAGE  0x007E49A0u   /* WorldInfo thiscall (InstigatorPawn, DamageType, Target, float Scale, HitInfo*[0x100]), ret 0x14 */
#define F_DEALRADIAL  0x007EEA10u   /* WorldInfo thiscall (InstigatorPawn, DamageType, FVector*, float Radius, float Scale, 0, 0, 0), ret 0x20 */
#define F_ACQUIRE     0x00967EE0u   /* AXPawn::AcquireWeapon thiscall (Archetype, bool DoNotEquip), ret 8 (test_give only) */
#define F_LOADPKG     0x0050C880u   /* UObject::LoadPackage cdecl (Outer, const TCHAR* File, LoadFlags, 0) (test_give only) */
#define F_APPMALLOC   0x00482A80u
#define F_APPFREE     0x00482AE0u
#define PAWN_FROM_DAMAGEE 0x39Cu
#define WI_GAME 0x40Cu
#define GAME_GRI 0x2C0u
#define WI_TIMEDILATION 0x398u
/* UObject / reflection */
#define UO_OUTER 0x14u
#define UO_NAME  0x18u
#define UO_CLASS 0x20u
#define UO_ARCH  0x24u
#define US_SUPER 0x34u
#define US_CHILDREN 0x38u
#define UF_NEXT 0x28u
#define UP_OFFSET 0x48u
/* AXPawn / inventory / AXWeapon (sdk/XCore_classes.h) */
#define PAWN_INVMGR  0x314u
#define PAWN_MELEE   0x1DF0u      /* CurrentDedicatedMeleeWeapon (Sky-Hook) */
#define PAWN_LOCKS   0x4CACu      /* WeaponInputLockedFlags */
#define IM_ITEMS     0x1FCu       /* InventoryItems[36] */
#define IM_EQ        0x2A0u       /* EquippedWeaponIndex */
#define W_FIREMODE   0x220u       /* FXWeaponFireMode FireMode[2], 0xE0 each */
#define FM_SIZE      0xE0u
#define FM_DAMAGETYPE 0x08u
#define FM_PROJARCH  0xCCu
#define PROJ_DAMAGETYPE 0x210u    /* AXProjectile.MyDamageType */
#define W_FLAGS      0x3F4u       /* 0x2 bIsHeldByPlayer, 0x4000 bHasBegunReloading */
#define W_AMMO       0x7C4u
#define W_MAXAMMO    0x7C8u       /* MaxAmmoCountAttrib */
#define W_SPARE      0x7ECu
#define W_MAXSPARE   0x7F0u
#define DT_FLAGS     0x28u        /* UDamageType: bIsMelee = 8 (the gear mod's test) */
#define DT_VIGORTRAP 0x9Cu        /* bIsVigorTrapDamage = 0x80 */
#define EV_REGION    1u           /* DamageEvent+1: EXSkeletalRegion, 1 = head */
/* FXDynamicGameplayAttribute */
#define DA_RECACHE 0x0u
#define DA_BASE    0x8u
#define DA_CURRENT 0xCu

#define LOGP "[perk] "
#define NSLOT 2

/* ---- config model ---- */
enum { EV_HIT, EV_KILL, EV_GUN_HIT, EV_GUN_KILL, EV_MELEE_HIT, EV_MELEE_KILL, EV_ANY_HIT, EV_ANY_KILL,
       EV_HEADSHOT, EV_HEADSHOT_KILL, EV_LAST_ROUND, EV_RELOAD, EV_EQUIP, EV_HURT, EV_N };
static const char *const ev_name[EV_N] = { "hit", "kill", "gun_hit", "gun_kill", "melee_hit", "melee_kill", "any_hit",
  "any_kill", "headshot", "headshot_kill", "last_round", "reload", "equip", "hurt" };
enum { TG_VICTIM, TG_SELF, TG_NEAREST, TG_ATTACKER };
static const char *const tg_name[] = { "victim", "self", "nearest", "attacker" };
enum { OP_ADD, OP_MUL, OP_SET };
enum { K_STAT = 1, K_PROC, K_DAMAGE, K_BLAST, K_REFUND, K_SLOWMO, K_BOOST };
static const char *const k_name[] = { "?", "stat", "proc", "damage", "blast", "refund", "slowmo", "boost" };

typedef struct {
  int kind, ev;
  int on_weapon; char attr[64]; int op; float val; float duration;   /* stat / boost */
  char proc[64]; int target; float radius, chance, cooldown, scale; int head; int ammo, spare; char dtype[64];
  double last;
} Eff;
typedef struct { char name[48]; char desc[200]; int ne; Eff e[8]; } Perk;
typedef struct { char pat[64]; char pname[NSLOT][48]; int perk[NSLOT]; } Slot;
typedef struct {
  int enable, verbose, loose, nperks, nslots;
  Perk perks[48]; Slot slots[64];
  /* test-only directives */
  char give[8][64]; int give_equip[8]; int ngive;
  char hold[64];                       /* force this archetype as the held gun ("" = real detection) */
  char cycle[6][64]; int ncycle; float cycle_s;
  float hit_s;                         /* test_hit: deal a test hit every N s */
} Config;

static Config g_cfg_a, g_cfg_b, *g_cfg;
static char g_file[512];
static time_t g_mtime; static off_t g_fsize;
static int g_state = -1;
static int g_hook_installed;
static int g_busy;                     /* inside our own guest calls: our own damage never starts further perk events */
static uint32_t g_rng = 0x2545F491u;
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float frand(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng >> 8) * (1.0f / 16777216.0f); }
static uint32_t f2u(float f){ union { float f; uint32_t u; } x; x.f = f; return x.u; }

/* ---- config parsing ---- */
static int ntok(char *s, char **tok, int max){
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
static const char *kv(char **tok, int n, const char *key){
  size_t k = strlen(key);
  for(int i = 0; i < n; i++) if(!strncasecmp(tok[i], key, k) && tok[i][k] == '=') return tok[i] + k + 1;
  return 0;
}
static float kvf(char **tok, int n, const char *key, float d){ const char *v = kv(tok, n, key); return v ? (float)atof(v) : d; }
static int parse_event(const char *s){ for(int i = 0; i < EV_N; i++) if(!strcasecmp(s, ev_name[i])) return i; return -1; }
static int find_perk(Config *c, const char *name){ for(int i = 0; i < c->nperks; i++) if(!strcasecmp(c->perks[i].name, name)) return i; return -1; }
static int parse_op(const char *s){ return !strcasecmp(s, "mul") ? OP_MUL : !strcasecmp(s, "set") ? OP_SET : OP_ADD; }

static void parse_config(Config *c, FILE *f){
  memset(c, 0, sizeof *c); c->enable = 1; c->verbose = 1;
  char line[1024], *tok[24]; int ln = 0; Perk *cur = 0;
  while(fgets(line, sizeof line, f)){
    ln++;
    int n = ntok(line, tok, 24); if(!n) continue;
    if(!strcasecmp(tok[0], "enable") && n > 1) c->enable = atoi(tok[1]);
    else if(!strcasecmp(tok[0], "verbose") && n > 1) c->verbose = atoi(tok[1]);
    else if(!strcasecmp(tok[0], "match") && n > 1) c->loose = !strcasecmp(tok[1], "loose");
    else if(!strcasecmp(tok[0], "perk") && n > 1){
      if(c->nperks >= 48){ fprintf(stderr, LOGP "line %d: too many perks\n", ln); cur = 0; continue; }
      cur = &c->perks[c->nperks++]; memset(cur, 0, sizeof *cur);
      snprintf(cur->name, sizeof cur->name, "%s", tok[1]);
      const char *v = kv(tok, n, "desc"); if(v) snprintf(cur->desc, sizeof cur->desc, "%s", v);
    }
    else if(!strcasecmp(tok[0], "stat")){
      if(!cur || cur->ne >= 8 || n < 5){ fprintf(stderr, LOGP "line %d: 'stat weapon|pawn <Attrib> add|mul|set <v>' must follow a perk\n", ln); continue; }
      Eff *e = &cur->e[cur->ne++]; memset(e, 0, sizeof *e); e->kind = K_STAT;
      e->on_weapon = strcasecmp(tok[1], "pawn") != 0;
      snprintf(e->attr, sizeof e->attr, "%s", tok[2]); e->op = parse_op(tok[3]); e->val = (float)atof(tok[4]);
    }
    else if(!strcasecmp(tok[0], "on")){
      if(!cur || cur->ne >= 8 || n < 3){ fprintf(stderr, LOGP "line %d: 'on <event> ...' must follow a perk\n", ln); continue; }
      Eff e; memset(&e, 0, sizeof e);
      e.ev = parse_event(tok[1]);
      if(e.ev < 0){ fprintf(stderr, LOGP "line %d: unknown event '%s'\n", ln, tok[1]); continue; }
      e.chance = kvf(tok, n, "chance", 1.0f); e.cooldown = kvf(tok, n, "cooldown", 0.0f);
      e.radius = kvf(tok, n, "radius", 1200.0f); e.scale = kvf(tok, n, "scale", 0.5f);
      e.target = e.ev == EV_RELOAD || e.ev == EV_EQUIP ? TG_SELF : TG_VICTIM;
      const char *v;
      if((v = kv(tok, n, "target"))) for(int i = 0; i < 4; i++) if(!strcasecmp(v, tg_name[i])) e.target = i;
      if((v = kv(tok, n, "dtype"))) snprintf(e.dtype, sizeof e.dtype, "%s", v);
      if((v = kv(tok, n, "proc"))){ e.kind = K_PROC; snprintf(e.proc, sizeof e.proc, "%s", v); }
      else if(!strcasecmp(tok[2], "damage")){ e.kind = K_DAMAGE; e.head = (int)kvf(tok, n, "headshot", 0); }
      else if(!strcasecmp(tok[2], "blast")){ e.kind = K_BLAST; e.radius = kvf(tok, n, "radius", 400.0f); e.scale = kvf(tok, n, "scale", 1.0f); }
      else if(!strcasecmp(tok[2], "refund")){ e.kind = K_REFUND; e.ammo = (int)kvf(tok, n, "ammo", 0); e.spare = (int)kvf(tok, n, "spare", 0); }
      else if(!strcasecmp(tok[2], "slowmo")){ e.kind = K_SLOWMO; e.scale = kvf(tok, n, "scale", 0.35f); e.duration = kvf(tok, n, "time", 2.0f); }
      else if(!strcasecmp(tok[2], "boost") && n >= 7){ e.kind = K_BOOST; e.on_weapon = strcasecmp(tok[3], "pawn") != 0;
        snprintf(e.attr, sizeof e.attr, "%s", tok[4]); e.op = parse_op(tok[5]); e.val = (float)atof(tok[6]); e.duration = kvf(tok, n, "duration", 3.0f); }
      else { fprintf(stderr, LOGP "line %d: 'on %s' needs proc=<ProcID> or damage|blast|refund|slowmo|boost\n", ln, tok[1]); continue; }
      cur->e[cur->ne++] = e;
    }
    else if(!strcasecmp(tok[0], "slot") && n > 2){
      if(c->nslots >= 64) continue;
      Slot *s = &c->slots[c->nslots++]; memset(s, 0, sizeof *s);
      snprintf(s->pat, sizeof s->pat, "%s", tok[1]);
      for(int i = 2; i < n; i++){
        int k = tok[i][0] - '1';
        if(k < 0 || k >= NSLOT || tok[i][1] != '='){ fprintf(stderr, LOGP "line %d: slot entries are 1=<Perk> or 2=<Perk>\n", ln); continue; }
        snprintf(s->pname[k], sizeof s->pname[k], "%s", tok[i] + 2);
      }
    }
    else if(!strcasecmp(tok[0], "test_give")){
      for(int i = 1; i < n && c->ngive < 8; i++){
        if(!strcasecmp(tok[i], "equip")){ if(c->ngive) c->give_equip[c->ngive - 1] = 1; continue; }
        snprintf(c->give[c->ngive++], 64, "%s", tok[i]);
      }
    }
    else if(!strcasecmp(tok[0], "test_hold") && n > 1){ if(strcasecmp(tok[1], "auto")) snprintf(c->hold, sizeof c->hold, "%s", tok[1]); }
    else if(!strcasecmp(tok[0], "test_cycle") && n > 2){ c->cycle_s = (float)atof(tok[1]); for(int i = 2; i < n && c->ncycle < 6; i++) snprintf(c->cycle[c->ncycle++], 64, "%s", tok[i]); }
    else if(!strcasecmp(tok[0], "test_hit") && n > 1) c->hit_s = (float)atof(tok[1]);
    else fprintf(stderr, LOGP "line %d: unknown directive '%s'\n", ln, tok[0]);
  }
  for(int i = 0; i < c->nslots; i++) for(int k = 0; k < NSLOT; k++){
    Slot *s = &c->slots[i]; s->perk[k] = -1;
    if(!s->pname[k][0] || !strcasecmp(s->pname[k], "none") || !strcasecmp(s->pname[k], "empty")) continue;
    s->perk[k] = find_perk(c, s->pname[k]);
    if(s->perk[k] < 0) fprintf(stderr, LOGP "slot %s %d: no perk named \"%s\"\n", s->pat, k + 1, s->pname[k]);
  }
}
static void log_config(Config *c){
  fprintf(stderr, LOGP "config %s: %s, %d perks, %d slot lines, damage match %s\n", g_file, c->enable ? "enabled" : "disabled",
          c->nperks, c->nslots, c->loose ? "loose (any non-melee player damage)" : "strict (held gun's damage types)");
  for(int i = 0; i < c->nperks; i++){
    Perk *p = &c->perks[i]; char b[256]; int l = 0; b[0] = 0;
    for(int k = 0; k < p->ne && l < 230; k++){ Eff *e = &p->e[k];
      l += snprintf(b + l, sizeof b - l, "%s%s%s%s", k ? ", " : "", e->kind == K_STAT ? "" : ev_name[e->ev], e->kind == K_STAT ? "stat " : "->", e->kind == K_STAT || e->kind == K_BOOST ? e->attr : e->kind == K_PROC ? e->proc : k_name[e->kind]); }
    fprintf(stderr, LOGP "  perk \"%s\": %s\n", p->name, b);
  }
  for(int i = 0; i < c->nslots; i++){
    Slot *s = &c->slots[i];
    fprintf(stderr, LOGP "  slot %-20s 1=%s 2=%s\n", s->pat, s->perk[0] >= 0 ? c->perks[s->perk[0]].name : "(empty)", s->perk[1] >= 0 ? c->perks[s->perk[1]].name : "(empty)");
  }
  if(c->ngive || c->hold[0] || c->ncycle || c->hit_s > 0) fprintf(stderr, LOGP "  TEST directives: give %d, hold %s, cycle %d every %.0f s, test hit every %.0f s\n",
          c->ngive, c->hold[0] ? c->hold : "auto", c->ncycle, c->cycle_s, c->hit_s);
}
static void find_config_path(void){
  const char *e = getenv("BSI_WEAPON_PERKS");
  if(e && *e){ snprintf(g_file, sizeof g_file, "%s", e); return; }
  const char *b = getenv("BSI_BLOOM_FILE");
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl){ snprintf(g_file, sizeof g_file, "%.*s/weapon_perks.txt", (int)(sl - b), b); return; } }
  snprintf(g_file, sizeof g_file, "/sdcard/BSI/weapon_perks.txt");
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
  if(stat(g_file, &st) != 0 && g_builtin){   /* ext72: built-in config (the example weapon_perks.txt) */
    if(!force) return 0;
    const char *t = ext_mod_builtin ? ext_mod_builtin("perks") : 0; FILE *f = t ? fmemopen((void *)t, strlen(t), "r") : 0; if(!f) return 0;
    Config *spare = g_cfg == &g_cfg_a ? &g_cfg_b : &g_cfg_a;
    parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;
    g_cfg = spare; fprintf(stderr, LOGP "built-in config (ext72, no %s)\n", g_file); log_config(spare);
    return 1;
  }
  if(stat(g_file, &st) != 0){ if(g_cfg && g_cfg->enable){ fprintf(stderr, LOGP "%s gone: perks idle\n", g_file); g_cfg->enable = 0; } return 0; }
  g_builtin = 0;
  if(!force && st.st_mtime == g_mtime && st.st_size == g_fsize) return 0;
  FILE *f = fopen(g_file, "r"); if(!f) return 0;
  Config *spare = g_cfg == &g_cfg_a ? &g_cfg_b : &g_cfg_a;
  parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;   /* ext72 menu switch */
  g_mtime = st.st_mtime; g_fsize = st.st_size; g_cfg = spare;
  log_config(spare);
  return 1;
}
static void perks_init(void){
  if(g_state >= 0) return;
  g_state = 0;
  const char *p = getenv("BSI_PATCHES"); if(p && !atoi(p)){ fprintf(stderr, LOGP "BSI_PATCHES=0: weapon perks off\n"); return; }
  const char *e = getenv("BSI_PERKS"); if(e && !atoi(e)){ fprintf(stderr, LOGP "BSI_PERKS=0: weapon perks off\n"); return; }
  find_config_path();
  struct stat st;
  if(stat(g_file, &st) != 0){
    if(!(ext_mod_default_on && ext_mod_default_on("perks"))){ fprintf(stderr, LOGP "no config at %s: weapon perks off (set BSI_WEAPON_PERKS or push weapon_perks.txt)\n", g_file); if(ext_mod_started) ext_mod_started("perks", 0); return; }
    g_builtin = 1;   /* ext72: on with the built-in config */
  } else if(ext_mod_file_allowed && !ext_mod_file_allowed("perks")){ if(ext_mod_started) ext_mod_started("perks", 0); return; }
  static const uint8_t dmg[] = { 0x6A, 0xFF, 0x68, 0x18, 0xB3, 0x04, 0x01 };
  static const uint8_t trg[] = { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x18 };
  static const uint8_t dd[]  = { 0x83, 0xEC, 0x7C, 0x56, 0x57, 0x6A, 0x7C };   /* DealDamage and DealRadialDamage share this prologue */
  for(unsigned i = 0; i < sizeof dmg; i++) if(R8(F_DAMAGE + i) != dmg[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: perks off\n", F_DAMAGE); return; }
  for(unsigned i = 0; i < sizeof trg; i++) if(R8(F_TRIGGERPROC + i) != trg[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: perks off\n", F_TRIGGERPROC); return; }
  for(unsigned i = 0; i < sizeof dd; i++) if(R8(F_DEALDAMAGE + i) != dd[i] || R8(F_DEALRADIAL + i) != dd[i]){ fprintf(stderr, LOGP "exe identity check failed at 7e49a0/7eea10: perks off\n"); return; }
  g_rng ^= (uint32_t)(now_s() * 1000.0);
  load_config(1);
  g_state = 1; if(ext_mod_started) ext_mod_started("perks", 1);
  { extern int gear_mod_call(CPU *, uint32_t) __attribute__((weak));
    fprintf(stderr, LOGP "weapon perks loaded: %d slots per weapon, damage hook %08x, gear mod %s\n", NSLOT, F_DAMAGE,
            gear_mod_call ? "linked (chained)" : "not linked"); }
}

/* ---- host-side reflection (reads only; same scheme as gear_mod.c) ---- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";
  return (const char *)MEMP(e + 0x10);
}
static int fname_find(const char *s){
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
static const char *aname(uint32_t w){ return gptr(w) ? oname(R32(w + UO_ARCH)) : "None"; }   /* archetype name = weapon class key */
static int is_a(uint32_t o, const char *cls){
  if(!gptr(o)) return 0;
  for(uint32_t c = R32(o + UO_CLASS), k = 0; gptr(c) && k < 40; c = R32(c + US_SUPER), k++) if(!strcmp(oname(c), cls)) return 1;
  return 0;
}
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
static uint32_t world_info(void){
  uint32_t w = R32(G_GWORLD); if(!gptr(w)) return 0;
  uint32_t lvl = R32(w + 0x3C); if(!gptr(lvl)) return 0;
  uint32_t act = R32(lvl + 0x28); if(!gptr(act)) return 0;
  uint32_t wi = R32(act); return gptr(wi) ? wi : 0;
}
static uint32_t game_gri(void){
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
static float pawn_health(uint32_t p){ return RF32(p + prop_off(p, "Health", 0x2AC)); }
static void pawn_loc(uint32_t p, float v[3]){ int o = prop_off(p, "XPrivateLocalLocation", 0x44); v[0] = RF32(p + o); v[1] = RF32(p + o + 4); v[2] = RF32(p + o + 8); }
static uint32_t as_pawn(uint32_t a){
  if(!gptr(a)) return 0;
  if(is_a(a, "Pawn")) return a;
  if(is_a(a, "Controller")){ uint32_t p = R32(a + prop_off(a, "Pawn", 0x1FC)); return gptr(p) ? p : 0; }
  return 0;
}

/* ---- held weapon detection ---- */
static int inventory(uint32_t pawn, uint32_t *out, int max){
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im)) return 0;
  int n = 0;
  for(int i = 0; i < 36 && n < max; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(gptr(it) && is_a(it, "XWeapon")) out[n++] = it; }
  return n;
}
static uint32_t inv_find(uint32_t pawn, const char *arch){
  uint32_t w[36]; int n = inventory(pawn, w, 36);
  for(int i = 0; i < n; i++) if(!strcasecmp(aname(w[i]), arch)) return w[i];
  return 0;
}
static int g_override;   /* the held gun came from test_hold / test_cycle */
static uint32_t held_gun(Config *c, uint32_t pawn, const char *forced){
  g_override = 0;
  if(forced && forced[0]){ uint32_t w = inv_find(pawn, forced); if(w){ g_override = 1; return w; } }
  uint32_t im = R32(pawn + PAWN_INVMGR); if(!gptr(im)) return 0;
  int idx = (int)R32(im + IM_EQ); if(idx < 0 || idx >= 36) return 0;
  uint32_t w = R32(im + IM_ITEMS + 4 * idx);
  return gptr(w) && is_a(w, "XWeapon") && !is_a(w, "XWeaponDedicatedMelee") ? w : 0;
}
static uint32_t held_hook(uint32_t pawn){
  uint32_t w = R32(pawn + PAWN_MELEE);
  if(gptr(w) && is_a(w, "XWeaponDedicatedMelee")) return w;
  uint32_t inv[36]; int n = inventory(pawn, inv, 36);
  for(int i = 0; i < n; i++) if(is_a(inv[i], "XWeaponDedicatedMelee")) return inv[i];
  return 0;
}
/* slot line for a weapon: exact archetype name > longest "Prefix*" > "*"; among equals the last line wins */
static Slot *slot_for(Config *c, uint32_t w){
  if(!gptr(w)) return 0;
  const char *a = aname(w); Slot *best = 0; int bl = -1;
  for(int i = 0; i < c->nslots; i++){
    Slot *s = &c->slots[i]; size_t L = strlen(s->pat);
    if(!strcasecmp(s->pat, a)){ best = s; bl = 1 << 20; continue; }   /* a later line for the same weapon wins */
    if(L && s->pat[L - 1] == '*' && !strncasecmp(s->pat, a, L - 1) && (int)L - 1 >= bl){ best = s; bl = (int)L - 1; }
  }
  return best;
}

/* ---- stat layering ---- */
typedef struct { uint32_t obj, off; float orig, written; int live, restored; char attr[48]; } Rec;
static Rec g_rec[96]; static int g_nrec;
static uint32_t g_pawn;
static unsigned g_stat_writes, g_restores;
typedef struct { uint32_t weapon; int on_weapon; char attr[64]; int op; float val; double until; char perk[48]; } Boost;
static Boost g_boost[16]; static int g_nboost;

/* attribute offset inside obj, 0 if none. FireMode_<n>_<Attr> resolves inside AXWeapon.FireMode[n]. */
static uint32_t attr_off(uint32_t obj, const char *attr){
  if(!strncasecmp(attr, "FireMode_", 9) && (attr[9] == '0' || attr[9] == '1') && attr[10] == '_'){
    static const struct { const char *n; uint32_t o; } fm[] = { {"FireTimeAttrib",0x18}, {"BurstShotsNum",0x44}, {"WeaponCooldownTimeAttrib",0x68}, {"ShotCostAttrib",0x94} };
    if(!is_a(obj, "XWeapon") || prop_off(obj, "FireMode", -1) != (int)W_FIREMODE) return 0;
    for(unsigned i = 0; i < sizeof fm / sizeof *fm; i++) if(!strcasecmp(attr + 11, fm[i].n)) return W_FIREMODE + (attr[9] - '0') * FM_SIZE + fm[i].o;
    return 0;
  }
  uint32_t p = find_prop(R32(obj + UO_CLASS), attr);
  if(!p || strcmp(cname(p), "StructProperty")) return 0;
  return R32(p + UP_OFFSET);
}
static int obj_valid(uint32_t obj, uint32_t pawn, uint32_t *inv, int ninv){
  if(obj == pawn) return 1;
  for(int i = 0; i < ninv; i++) if(inv[i] == obj) return 1;
  return 0;
}
/* MaxAmmoCount went down: move the rounds above the new clip size back to the reserve */
static void clamp_clip(uint32_t w, float ratio){
  float curmax = RF32(w + W_MAXAMMO + DA_CURRENT);
  int nmax = (int)floorf(curmax * ratio + 0.5f), ammo = (int)R32(w + W_AMMO);
  if(nmax > 0 && ammo > nmax){ W32(w + W_AMMO, (uint32_t)nmax); W32(w + W_SPARE, R32(w + W_SPARE) + (uint32_t)(ammo - nmax));
    if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "clip %s %d -> %d (extra rounds back to the reserve)\n", aname(w), ammo, nmax); }
}
/* before gear_mod_tick: take our changes out, so the gear mod sees only its own writes */
static void layer_remove(void){
  if(!g_nrec) return;
  uint32_t pawn = player_pawn();
  if(pawn != g_pawn){ for(int r = 0; r < g_nrec; r++) g_rec[r].restored = 0; return; }   /* old pawn: records are dropped in layer_apply */
  uint32_t inv[36]; int ninv = inventory(pawn, inv, 36);
  for(int r = 0; r < g_nrec; r++){
    Rec *x = &g_rec[r]; x->restored = 0;
    if(!obj_valid(x->obj, pawn, inv, ninv)){ x->obj = 0; continue; }   /* weapon object gone: never write to it */
    uint32_t b = x->obj + x->off + DA_BASE; float cur = RF32(b);
    if(fabsf(cur - x->written) <= 1e-5f){ WF32(b, x->orig); x->restored = 1; }
    else x->orig = cur;   /* the game re-initialised it: that is the new original */
  }
}
typedef struct { uint32_t obj, off; float add, mul, set; int hasset; const char *attr; } Agg;
static void agg_add(Agg *agg, int *na, uint32_t obj, const char *attr, int op, float val, const char *who){
  uint32_t off = attr_off(obj, attr);
  if(!off){
    static char warned[32][64]; static int nw; int seen = 0;
    for(int w = 0; w < nw; w++) if(!strcmp(warned[w], attr)) seen = 1;
    if(!seen && nw < 32){ snprintf(warned[nw++], 64, "%s", attr); fprintf(stderr, LOGP "\"%s\": %s has no attribute %s (ignored)\n", who, cname(obj), attr); }
    return;
  }
  int a; for(a = 0; a < *na; a++) if(agg[a].obj == obj && agg[a].off == off) break;
  if(a == *na){ if(*na >= 64) return; agg[a].obj = obj; agg[a].off = off; agg[a].add = 0; agg[a].mul = 1; agg[a].hasset = 0; agg[a].attr = attr; (*na)++; }
  if(op == OP_ADD) agg[a].add += val; else if(op == OP_MUL) agg[a].mul *= val; else { agg[a].set = val; agg[a].hasset = 1; }
}
/* after gear_mod_tick: apply the stats of the perks on the weapons in hand */
static void layer_apply(Config *c, uint32_t pawn, uint32_t gun, uint32_t hook){
  if(pawn != g_pawn){
    if(g_nrec && c->verbose) fprintf(stderr, LOGP "player pawn changed %08x -> %08x: %d stat records dropped\n", g_pawn, pawn, g_nrec);
    g_nrec = 0; g_nboost = 0; g_pawn = pawn;
  }
  Agg agg[64]; int na = 0;
  double t = now_s();
  if(pawn && c->enable){
    uint32_t ws[2] = { gun, hook };
    for(int k = 0; k < 2; k++){
      Slot *s = slot_for(c, ws[k]); if(!s) continue;
      for(int j = 0; j < NSLOT; j++){
        if(s->perk[j] < 0) continue;
        Perk *p = &c->perks[s->perk[j]];
        for(int e = 0; e < p->ne; e++) if(p->e[e].kind == K_STAT) agg_add(agg, &na, p->e[e].on_weapon ? ws[k] : pawn, p->e[e].attr, p->e[e].op, p->e[e].val, p->name);
      }
    }
    for(int b = 0; b < g_nboost; ){
      Boost *x = &g_boost[b];
      if(t >= x->until){ if(c->verbose) fprintf(stderr, LOGP "boost \"%s\" %s ended\n", x->perk, x->attr); g_boost[b] = g_boost[--g_nboost]; continue; }
      if(x->weapon == gun || x->weapon == hook) agg_add(agg, &na, x->on_weapon ? x->weapon : pawn, x->attr, x->op, x->val, x->perk);
      b++;
    }
  }
  for(int r = 0; r < g_nrec; r++) g_rec[r].live = 0;
  for(int a = 0; a < na; a++){
    int r; for(r = 0; r < g_nrec; r++) if(g_rec[r].obj == agg[a].obj && g_rec[r].off == agg[a].off) break;
    uint32_t b = agg[a].obj + agg[a].off + DA_BASE;
    float cur = RF32(b);   /* clean value: our change was removed in layer_remove */
    if(r == g_nrec){ if(g_nrec >= 96) continue; g_nrec++; memset(&g_rec[r], 0, sizeof g_rec[r]); g_rec[r].obj = agg[a].obj; g_rec[r].off = agg[a].off;
      g_rec[r].written = cur; snprintf(g_rec[r].attr, sizeof g_rec[r].attr, "%s", agg[a].attr); }
    float prev = g_rec[r].written;
    g_rec[r].orig = cur;
    float want = agg[a].hasset ? agg[a].set : (cur + agg[a].add) * agg[a].mul;
    WF32(b, want);
    if(fabsf(want - prev) > 1e-6f || !g_rec[r].restored){   /* net change since last frame: let GetValue recompute */
      WF32(agg[a].obj + agg[a].off + DA_RECACHE, 0.0f);
      if(fabsf(want - prev) > 1e-6f){ g_stat_writes++;
        if(c->verbose) fprintf(stderr, LOGP "stat %s%s%s.%s base %g -> %g\n", cname(agg[a].obj), agg[a].obj == pawn ? "" : ":", agg[a].obj == pawn ? "" : aname(agg[a].obj), agg[a].attr, cur, want); }
    }
    g_rec[r].written = want; g_rec[r].live = 1;
  }
  for(int r = 0; r < g_nrec; ){
    Rec *x = &g_rec[r];
    if(x->live){ r++; continue; }
    if(x->obj && x->restored){   /* removed in layer_remove and not wanted any more: the clean value stays */
      WF32(x->obj + x->off + DA_RECACHE, 0.0f); g_restores++;
      if(x->off == W_MAXAMMO && x->written > 0) clamp_clip(x->obj, x->orig / x->written);
      if(c->verbose) fprintf(stderr, LOGP "stat %s%s%s.%s restored to %g\n", cname(x->obj), x->obj == pawn ? "" : ":", x->obj == pawn ? "" : aname(x->obj), x->attr, x->orig);
    }
    *x = g_rec[--g_nrec];
  }
}

/* ---- actions ---- */
static unsigned g_ev_count[EV_N], g_actions, g_action_fail;
static void fire_proc(CPU *c, const char *proc, uint32_t inst, uint32_t victim){
  uint32_t gri = game_gri(), num; int idx = fname_parse(proc, &num);
  if(!gri || idx < 0 || !gptr(inst) || !gptr(victim)){ g_action_fail++; if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "proc %s: cannot fire (gri %08x, name %d)\n", proc, gri, idx); return; }
  CPU save = *c;
  PUSH32(c, victim); PUSH32(c, inst); PUSH32(c, num); PUSH32(c, (uint32_t)idx);
  c->ecx = gri;
  g_busy++; guest_call(c, F_TRIGGERPROC); g_busy--;
  uint32_t ok = c->eax; *c = save; g_actions++;
  if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "proc %s: %08x -> %08x (%s) %s\n", proc, inst, victim, cname(victim), ok ? "triggered" : "not triggered");
}
static uint32_t find_dtype(const char *name){   /* a UDamageType object by name (cached) */
  static char last[64]; static uint32_t lastp;
  if(!strcasecmp(last, name) && gptr(lastp) && !strcasecmp(oname(lastp), name)) return lastp;
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  for(uint32_t i = 0; i < n && gptr(go); i++){ uint32_t o = R32(go + 4 * i);
    if(gptr(o) && !strcasecmp(oname(o), name) && is_a(o, "DamageType")){ snprintf(last, sizeof last, "%s", name); lastp = o; return o; } }
  return 0;
}
static uint32_t gun_dtype(uint32_t gun){
  if(!gptr(gun)) return 0;
  uint32_t mode = R8(gun + prop_off(gun, "CurrentFireMode", 0x3E0)) & 1;
  uint32_t d = R32(gun + W_FIREMODE + mode * FM_SIZE + FM_DAMAGETYPE);
  if(!gptr(d)) d = R32(gun + W_FIREMODE + FM_DAMAGETYPE);
  if(!gptr(d)){ uint32_t pa = R32(gun + W_FIREMODE + FM_PROJARCH); if(gptr(pa)) d = R32(pa + PROJ_DAMAGETYPE); }
  return gptr(d) && is_a(d, "DamageType") ? d : 0;
}
static void deal_damage(CPU *c, uint32_t inst, uint32_t dt, uint32_t target, float scale, int head){
  uint32_t wi = world_info();
  if(!wi || !gptr(dt) || !gptr(target) || !gptr(inst)){ g_action_fail++; return; }
  CPU save = *c;
  c->esp = (c->esp - 0x100) & ~15u; uint32_t hit = c->esp;
  for(uint32_t i = 0; i < 0x100; i += 4) W32(hit + i, 0);
  if(head){ W8(hit, 2); W8(hit + 1, 1); }   /* what XSeqAct_CauseDamageToActor does for bPerformHeadshotIfPawn */
  PUSH32(c, hit); PUSH32(c, f2u(scale)); PUSH32(c, target); PUSH32(c, dt); PUSH32(c, inst); c->ecx = wi;
  g_busy++; guest_call(c, F_DEALDAMAGE); g_busy--;
  *c = save; g_actions++;
}
static void deal_radial(CPU *c, uint32_t inst, uint32_t dt, const float loc[3], float radius, float scale){
  uint32_t wi = world_info();
  if(!wi || !gptr(dt) || !gptr(inst)){ g_action_fail++; return; }
  CPU save = *c;
  c->esp = (c->esp - 16) & ~15u; uint32_t v = c->esp;
  WF32(v, loc[0]); WF32(v + 4, loc[1]); WF32(v + 8, loc[2]); W32(v + 12, 0);
  PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, f2u(scale)); PUSH32(c, f2u(radius)); PUSH32(c, v); PUSH32(c, dt); PUSH32(c, inst); c->ecx = wi;
  g_busy++; guest_call(c, F_DEALRADIAL); g_busy--;
  *c = save; g_actions++;
}
static uint32_t nearest_enemy(uint32_t from, uint32_t player, float radius){
  uint32_t wi = world_info(); if(!wi || !gptr(from)) return 0;
  float o[3]; pawn_loc(from, o);
  uint32_t best = 0; float bd = radius * radius;
  uint32_t p = R32(wi + prop_off(wi, "PawnList", 0x428));
  for(int k = 0; gptr(p) && k < 512; k++){
    if(p != from && p != player && is_a(p, "XPawn") && pawn_health(p) > 0){
      uint32_t ctl = R32(p + prop_off(p, "Controller", 0x218));
      int liz = strstr(cname(p), "Elizabeth") || (gptr(ctl) && strstr(cname(ctl), "Elizabeth"));
      if(!liz){ float v[3]; pawn_loc(p, v); float dx = v[0] - o[0], dy = v[1] - o[1], dz = v[2] - o[2], d = dx * dx + dy * dy + dz * dz;
        if(d < bd){ bd = d; best = p; } }
    }
    p = R32(p + prop_off(p, "NextPawn", 0x21C));
  }
  return best;
}
/* slow motion: WorldInfo.TimeDilation; restored after real seconds unless something else (a cutscene) changed it */
static uint32_t g_slow_wi; static float g_slow_val; static double g_slow_until;
static void slowmo_start(float scale, float secs){
  uint32_t wi = world_info(); if(!wi) return;
  float cur = RF32(wi + WI_TIMEDILATION);
  if(g_slow_wi == wi && fabsf(cur - g_slow_val) < 1e-6f){ g_slow_until = now_s() + secs; return; }   /* extend */
  if(fabsf(cur - 1.0f) > 1e-3f){ if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "slowmo: TimeDilation is %g (not ours), skipped\n", cur); return; }
  WF32(wi + WI_TIMEDILATION, scale); g_slow_wi = wi; g_slow_val = scale; g_slow_until = now_s() + secs; g_actions++;
  if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "slowmo: TimeDilation 1 -> %g for %.1f s\n", scale, secs);
}
static void slowmo_tick(void){
  if(!g_slow_wi || now_s() < g_slow_until) return;
  uint32_t wi = world_info();
  if(wi == g_slow_wi && fabsf(RF32(wi + WI_TIMEDILATION) - g_slow_val) < 1e-6f){ WF32(wi + WI_TIMEDILATION, 1.0f); if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "slowmo: TimeDilation restored to 1\n"); }
  g_slow_wi = 0;
}
static void refund(uint32_t gun, int ammo, int spare){
  if(!gptr(gun)) return;
  if(ammo){ int cur = (int)R32(gun + W_AMMO), mx = (int)floorf(RF32(gun + W_MAXAMMO + DA_CURRENT) + 0.5f); if(mx <= 0) mx = (int)RF32(gun + W_MAXAMMO + DA_BASE);
    int nw = cur + ammo; if(mx > 0 && nw > mx) nw = mx; if(nw > cur){ W32(gun + W_AMMO, (uint32_t)nw); } }
  if(spare){ int cur = (int)R32(gun + W_SPARE), mx = (int)floorf(RF32(gun + W_MAXSPARE + DA_CURRENT) + 0.5f);
    int nw = cur + spare; if(mx > 0 && nw > mx) nw = mx; if(nw > cur) W32(gun + W_SPARE, (uint32_t)nw); }
  g_actions++;
  if(g_cfg && g_cfg->verbose) fprintf(stderr, LOGP "refund %s: clip %d spare %d\n", aname(gun), (int)R32(gun + W_AMMO), (int)R32(gun + W_SPARE));
}

/* deliver one event to the perks of the weapons in hand. src = the weapon that caused it (0 for hurt) */
static uint32_t g_gun, g_hook;
static int ev_match(int want, int ev, int melee_weapon){
  if(want == ev) return 1;
  if(want == EV_HIT) return ev == (melee_weapon ? EV_MELEE_HIT : EV_GUN_HIT);
  if(want == EV_KILL) return ev == (melee_weapon ? EV_MELEE_KILL : EV_GUN_KILL);
  if(want == EV_ANY_HIT) return ev == EV_GUN_HIT || ev == EV_MELEE_HIT;
  if(want == EV_ANY_KILL) return ev == EV_GUN_KILL || ev == EV_MELEE_KILL;
  return 0;
}
static void dispatch(CPU *c, Config *cf, int ev, uint32_t player, uint32_t victim, uint32_t attacker, uint32_t dt){
  g_ev_count[ev]++;
  if(g_busy || !cf->enable) return;
  double t = now_s();
  uint32_t ws[2] = { g_gun, g_hook };
  for(int k = 0; k < 2; k++){
    Slot *s = slot_for(cf, ws[k]); if(!s) continue;
    int melee_w = k == 1;
    for(int j = 0; j < NSLOT; j++){
      if(s->perk[j] < 0) continue;
      Perk *p = &cf->perks[s->perk[j]];
      for(int i = 0; i < p->ne; i++){
        Eff *e = &p->e[i]; if(e->kind == K_STAT || !ev_match(e->ev, ev, melee_w)) continue;
        if((e->cooldown > 0 && t - e->last < e->cooldown) || frand() >= e->chance) continue;
        uint32_t tgt = e->target == TG_SELF ? player : e->target == TG_ATTACKER ? attacker :
                       e->target == TG_NEAREST ? nearest_enemy(victim ? victim : player, player, e->radius) : victim;
        if(!tgt && e->kind != K_REFUND && e->kind != K_SLOWMO && e->kind != K_BOOST){ if(cf->verbose) fprintf(stderr, LOGP "\"%s\" on %s: no %s target\n", p->name, ev_name[ev], tg_name[e->target]); continue; }
        e->last = t;
        if(cf->verbose) fprintf(stderr, LOGP "\"%s\" (%s slot %d) on %s -> %s%s%s\n", p->name, aname(ws[k]), j + 1, ev_name[ev], k_name[e->kind], e->kind == K_PROC ? " " : "", e->kind == K_PROC ? e->proc : "");
        uint32_t wdt = e->dtype[0] ? find_dtype(e->dtype) : 0;
        if(!wdt) wdt = k == 0 ? (gptr(dt) ? dt : gun_dtype(ws[k])) : (gptr(dt) ? dt : 0);
        switch(e->kind){
        case K_PROC: fire_proc(c, e->proc, player, tgt); break;
        case K_DAMAGE:
          if(!wdt){ g_action_fail++; break; }
          { float h0 = pawn_health(tgt); deal_damage(c, player, wdt, tgt, e->scale, e->head);
            if(cf->verbose) fprintf(stderr, LOGP "  damage x%.2f (%s) on %08x (%s) health %.0f -> %.0f\n", e->scale, oname(wdt), tgt, cname(tgt), h0, pawn_health(tgt)); }
          break;
        case K_BLAST:
          if(!wdt){ g_action_fail++; break; }
          { float v[3]; pawn_loc(tgt, v); float h0 = pawn_health(tgt); deal_radial(c, player, wdt, v, e->radius, e->scale);
            if(cf->verbose) fprintf(stderr, LOGP "  blast r=%.0f x%.2f (%s) at %08x (%s) health %.0f -> %.0f\n", e->radius, e->scale, oname(wdt), tgt, cname(tgt), h0, pawn_health(tgt)); }
          break;
        case K_REFUND: refund(g_gun, e->ammo, e->spare); break;
        case K_SLOWMO: slowmo_start(e->scale, e->duration); break;
        case K_BOOST:
          if(g_nboost < 16){ Boost *b = &g_boost[g_nboost++]; b->weapon = ws[k]; b->on_weapon = e->on_weapon; snprintf(b->attr, sizeof b->attr, "%s", e->attr);
            b->op = e->op; b->val = e->val; b->until = t + e->duration; snprintf(b->perk, sizeof b->perk, "%s", p->name);
            if(cf->verbose) fprintf(stderr, LOGP "  boost %s %s %s %g for %.1f s\n", e->on_weapon ? "weapon" : "pawn", e->attr, e->op == OP_MUL ? "mul" : e->op == OP_SET ? "set" : "add", e->val, e->duration); }
          break;
        }
      }
    }
  }
}

/* is this damage type the held gun's? (FireMode[0/1].DamageType or its projectile's MyDamageType) */
static int gun_owns(uint32_t gun, uint32_t dt){
  if(!gptr(gun) || !gptr(dt)) return 0;
  for(uint32_t m = 0; m < 2; m++){
    uint32_t b = gun + W_FIREMODE + m * FM_SIZE;
    if(R32(b + FM_DAMAGETYPE) == dt) return 1;
    uint32_t pa = R32(b + FM_PROJARCH); if(gptr(pa) && R32(pa + PROJ_DAMAGETYPE) == dt) return 1;
  }
  return 0;
}

/* ---- entry points ---- */
int weapon_perks_skip(uint32_t a){
  if(a != F_DAMAGE) return 0;
  perks_init();
  if(g_state != 1) return 0;
  g_hook_installed = 1;
  return 1;
}

static uint32_t g_lastround_gun; static int g_lastround_done;
static int g_selfhit;   /* test_hit with no other pawn: the player's own hit on itself is classified like a hit on an enemy */
static unsigned g_hits_seen, g_hits_unmatched;
/* patches.c patch_call 0x971fa0: tried first; wraps the gear mod's wrapper (or the original) */
int weapon_perks_call(CPU *c, uint32_t t){
  if(t != F_DAMAGE) return 0;
  if(g_state < 0) perks_init();
  if(g_state != 1) return 0;
  uint32_t self = c->ecx, victim = self - PAWN_FROM_DAMAGEE;
  uint32_t inst = R32(c->esp + 4), dt = R32(c->esp + 8), ev = R32(c->esp + 12);
  Config *cf = g_cfg;
  int vp = gptr(victim) && cf && cf->enable;
  float hp0 = vp ? pawn_health(victim) : 0;
  extern int gear_mod_call(CPU *, uint32_t) __attribute__((weak));
  if(!(gear_mod_call && gear_mod_call(c, t))){   /* gear mod off or absent: run the original ourselves */
    perk_gfn orig = rt_lookup(t); if(!orig) return 0;
    orig(c);
  }
  if(!vp) return 1;
  uint32_t player = player_pawn(); if(!player) return 1;
  uint32_t attacker = as_pawn(inst);
  float hp1 = pawn_health(victim);
  int killed = hp0 > 0 && hp1 <= 0;
  if(victim == player && !(g_selfhit && attacker == player)){
    if(attacker && attacker != player) dispatch(c, cf, EV_HURT, player, victim, attacker, 0);
    return 1;
  }
  if(victim == player) fprintf(stderr, LOGP "TEST self-hit reached the damage hook: type %s, region byte %u, condition byte %u, health %.0f -> %.0f, held gun owns it: %s\n",
                               oname(dt), gptr(ev) ? R8(ev + EV_REGION) : 255u, gptr(ev) ? R8(ev + 2) : 255u, hp0, hp1, gun_owns(g_gun, dt) ? "yes" : "no");
  if(attacker != player && inst != player_pc()) return 1;
  g_hits_seen++;
  int melee = gptr(dt) && (R32(dt + DT_FLAGS) & 8);
  int head = gptr(ev) && R8(ev + EV_REGION) == 1;
  if(cf->verbose > 1) fprintf(stderr, LOGP "hit %s by %s (%s) health %.0f -> %.0f%s%s\n", cname(victim), oname(dt), melee ? "melee" : "ranged", hp0, hp1, head ? " HEAD" : "", killed ? " KILL" : "");
  if(melee){
    dispatch(c, cf, EV_MELEE_HIT, player, victim, player, dt);
    if(killed) dispatch(c, cf, EV_MELEE_KILL, player, victim, player, dt);
    return 1;
  }
  if(gptr(dt) && (R8(dt + DT_VIGORTRAP) & 0x80)) return 1;
  if(!g_gun || !(gun_owns(g_gun, dt) || cf->loose)){
    g_hits_unmatched++;
    static uint32_t seen[32]; static int ns; int k; for(k = 0; k < ns; k++) if(seen[k] == dt) break;
    if(k == ns && ns < 32){ seen[ns++] = dt; fprintf(stderr, LOGP "player damage %s is not from the held gun %s (its types %s / %s): no gun perk events (set 'match loose' to count it)\n",
      oname(dt), aname(g_gun), g_gun ? oname(R32(g_gun + W_FIREMODE + FM_DAMAGETYPE)) : "-", g_gun ? oname(R32(g_gun + W_FIREMODE + FM_SIZE + FM_DAMAGETYPE)) : "-"); }
    return 1;
  }
  dispatch(c, cf, EV_GUN_HIT, player, victim, player, dt);
  if(head) dispatch(c, cf, EV_HEADSHOT, player, victim, player, dt);
  if(killed){ dispatch(c, cf, EV_GUN_KILL, player, victim, player, dt); if(head) dispatch(c, cf, EV_HEADSHOT_KILL, player, victim, player, dt); }
  if(R32(g_gun + W_AMMO) == 0 && !(g_lastround_gun == g_gun && g_lastround_done)){
    g_lastround_gun = g_gun; g_lastround_done = 1;
    dispatch(c, cf, EV_LAST_ROUND, player, victim, player, dt);
  }
  return 1;
}

/* test only: give weapons (LoadPackage CoalescedItems + AcquireWeapon in the same tick, before the archetypes are GC'd) */
static uint32_t gwstr(CPU *c, const char *s){
  uint32_t n = (uint32_t)strlen(s);
  CPU save = *c; PUSH32(c, 8); PUSH32(c, 2 * (n + 1)); guest_call(c, F_APPMALLOC); uint32_t b = c->eax; *c = save;
  if(gptr(b)) for(uint32_t i = 0; i <= n; i++) W16(b + 2 * i, (uint8_t)s[i]);
  return b;
}
static uint32_t find_arch(const char *name){
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  for(uint32_t i = 0; i < n && gptr(go); i++){ uint32_t o = R32(go + 4 * i);
    if(gptr(o) && !strcmp(oname(o), name) && !strcmp(oname(R32(o + UO_OUTER)), "PreCoalescedItemAssets") && is_a(o, "XWeapon")) return o; }
  return 0;
}
static void test_give(CPU *c, Config *cf, uint32_t pawn){
  int need = 0; for(int i = 0; i < cf->ngive; i++) if(!inv_find(pawn, cf->give[i]) && !find_arch(cf->give[i])) need = 1;
  if(need){
    uint32_t fb = gwstr(c, "CoalescedItems");
    CPU save = *c; PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, fb); PUSH32(c, 0); guest_call(c, F_LOADPKG); uint32_t pk = c->eax; *c = save;
    { CPU s2 = *c; PUSH32(c, fb); guest_call(c, F_APPFREE); *c = s2; }
    fprintf(stderr, LOGP "TEST give: LoadPackage CoalescedItems -> %08x (%s)\n", pk, oname(pk));
  }
  for(int i = 0; i < cf->ngive; i++){
    if(inv_find(pawn, cf->give[i])){ fprintf(stderr, LOGP "TEST give %s: already carried\n", cf->give[i]); continue; }
    uint32_t a = find_arch(cf->give[i]);
    if(!a){ fprintf(stderr, LOGP "TEST give %s: archetype not loaded\n", cf->give[i]); continue; }
    uint32_t locks = R32(pawn + PAWN_LOCKS); W32(pawn + PAWN_LOCKS, 0);
    CPU save = *c; PUSH32(c, cf->give_equip[i] ? 0u : 1u); PUSH32(c, a); c->ecx = pawn;
    g_busy++; guest_call(c, F_ACQUIRE); g_busy--; *c = save;
    W32(pawn + PAWN_LOCKS, locks);
    uint32_t w = inv_find(pawn, cf->give[i]);
    fprintf(stderr, LOGP "TEST give %s (%s): instance %08x clip %d spare %d\n", cf->give[i], cf->give_equip[i] ? "Equip" : "DoNotEquip", w, w ? (int)R32(w + W_AMMO) : -1, w ? (int)R32(w + W_SPARE) : -1);
    if(w && !R32(w + W_AMMO)){ W32(w + W_AMMO, 12); W32(w + W_SPARE, 48); }   /* so refund/last-round have something to work with */
  }
}
/* test only: a hit from the player with the held gun's damage type on the nearest other pawn (alternating headshot) */
static void test_hit(CPU *c, uint32_t pawn){
  static int n;
  uint32_t dt = gun_dtype(g_gun), tgt = nearest_enemy(pawn, pawn, 1e6f);
  float scale = 0.3f;
  if(!tgt){ static int said; if(!said++){ uint32_t wi = world_info(); int np = 0; char b[512]; int l = 0; b[0] = 0;
    for(uint32_t p = wi ? R32(wi + prop_off(wi, "PawnList", 0x428)) : 0; gptr(p) && np < 16; p = R32(p + prop_off(p, "NextPawn", 0x21C)), np++)
      l += snprintf(b + l, sizeof b - l > 0 ? sizeof b - l : 0, "%s(%.0f) ", cname(p), pawn_health(p));
    fprintf(stderr, LOGP "TEST hit: no other living pawn (pawn list: %s): hitting the player itself, treated as a gun hit\n", b); }
    if(pawn_health(pawn) < 300){ fprintf(stderr, LOGP "TEST hit: player health %.0f, skipped\n", pawn_health(pawn)); return; }
    tgt = pawn; scale = 0.02f; g_selfhit = 1; }
  int head = n++ & 1;
  fprintf(stderr, LOGP "TEST hit #%d: %s -> %08x (%s, health %.0f)%s\n", n, oname(dt), tgt, cname(tgt), pawn_health(tgt), head ? " as headshot" : "");
  if(!dt) return;
  /* not g_busy: this hit must go through the hook like a real one */
  uint32_t wi = world_info(); if(!wi) return;
  CPU save = *c;
  c->esp = (c->esp - 0x100) & ~15u; uint32_t hit = c->esp;
  for(uint32_t i = 0; i < 0x100; i += 4) W32(hit + i, 0);
  if(head){ W8(hit, 2); W8(hit + 1, 1); }
  PUSH32(c, hit); PUSH32(c, f2u(scale)); PUSH32(c, tgt); PUSH32(c, dt); PUSH32(c, pawn); c->ecx = wi;
  guest_call(c, F_DEALDAMAGE);
  *c = save; g_selfhit = 0;
  fprintf(stderr, LOGP "TEST hit #%d done: target health %.0f\n", n, pawn_health(tgt));
}

static void describe_hold(Config *c, const char *why){
  Slot *sg = slot_for(c, g_gun), *sh = slot_for(c, g_hook);
  fprintf(stderr, LOGP "%s: gun %s %08x%s [1=%s 2=%s], Sky-Hook %s %08x [1=%s 2=%s]\n", why,
          g_gun ? aname(g_gun) : "(none)", g_gun, g_override ? " (TEST override)" : "",
          sg && sg->perk[0] >= 0 ? c->perks[sg->perk[0]].name : "-", sg && sg->perk[1] >= 0 ? c->perks[sg->perk[1]].name : "-",
          g_hook ? aname(g_hook) : "(none)", g_hook,
          sh && sh->perk[0] >= 0 ? c->perks[sh->perk[0]].name : "-", sh && sh->perk[1] >= 0 ? c->perks[sh->perk[1]].name : "-");
}

/* patches.c patch_call 0xaf0480 (UXGameEngine::Tick): replaces the gear_mod_tick call and calls it itself */
void weapon_perks_tick(CPU *c){
  extern void gear_mod_tick(CPU *) __attribute__((weak));
  if(g_state < 0) perks_init();
  if(g_state == 1) layer_remove();
  if(gear_mod_tick) gear_mod_tick(c);
  if(g_state != 1) return;
  static double next_poll, next_status, next_hit, cycle_t0; static long ticks; static int cyc = -1;
  double t = now_s(); ticks++;
  if(t >= next_poll){ next_poll = t + 2.0; load_config(0); }
  Config *cf = g_cfg; if(!cf) return;
  uint32_t pawn = player_pawn();
  static uint32_t gave_pawn;
  if(pawn && gave_pawn != pawn && ticks > 3 && (cf->ngive || cf->ncycle || cf->hit_s > 0)){   /* test directives start 15 s after a new pawn */
    static double t_pawn; static uint32_t tp;
    if(tp != pawn){ tp = pawn; t_pawn = t + 15.0; }
    if(t >= t_pawn){ gave_pawn = pawn; if(cf->ngive) test_give(c, cf, pawn); cycle_t0 = t; }
  }
  const char *forced = cf->hold;
  if(cf->ncycle && cf->cycle_s > 0 && gave_pawn == pawn && pawn){
    int k = (int)((t - cycle_t0) / cf->cycle_s) % cf->ncycle;
    if(k != cyc){ cyc = k; fprintf(stderr, LOGP "TEST cycle: holding %s\n", cf->cycle[k]); }
    forced = cf->cycle[k];
  }
  uint32_t gun = pawn ? held_gun(cf, pawn, forced) : 0, hook = pawn ? held_hook(pawn) : 0;
  if(gun != g_gun || hook != g_hook){
    uint32_t old = g_gun; g_gun = gun; g_hook = hook;
    describe_hold(cf, "in hand");
    if(gun && gun != old && pawn) dispatch(c, cf, EV_EQUIP, pawn, pawn, pawn, 0);
    g_lastround_gun = 0; g_lastround_done = 0;
  }
  layer_apply(cf, pawn, gun, hook);
  if(gun){
    static uint32_t rgun, rflags;
    uint32_t fl = R32(gun + W_FLAGS);
    if(rgun == gun && !(rflags & 0x4000) && (fl & 0x4000)) dispatch(c, cf, EV_RELOAD, pawn, pawn, pawn, 0);
    rgun = gun; rflags = fl;
    if(g_lastround_gun == gun && R32(gun + W_AMMO) > 0) g_lastround_done = 0;
  }
  slowmo_tick();
  if(cf->hit_s > 0 && pawn && g_gun && gave_pawn == pawn && t >= next_hit){ if(next_hit == 0){ next_hit = t + 4; } else { next_hit = t + cf->hit_s; test_hit(c, pawn); } }
  if(t >= next_status){
    next_status = t + 15.0;
    fprintf(stderr, LOGP "status: tick %ld, hook %s, pawn %08x, gun %s, Sky-Hook %s, stat records %d (writes %u, restores %u), boosts %d, hits %u (not the held gun %u), "
            "events gun_hit %u gun_kill %u melee_hit %u headshot %u last_round %u reload %u equip %u hurt %u, actions %u (failed %u)\n",
            ticks, g_hook_installed ? "on" : "off", pawn, aname(g_gun), aname(g_hook), g_nrec, g_stat_writes, g_restores, g_nboost, g_hits_seen, g_hits_unmatched,
            g_ev_count[EV_GUN_HIT], g_ev_count[EV_GUN_KILL], g_ev_count[EV_MELEE_HIT], g_ev_count[EV_HEADSHOT], g_ev_count[EV_LAST_ROUND],
            g_ev_count[EV_RELOAD], g_ev_count[EV_EQUIP], g_ev_count[EV_HURT], g_actions, g_action_fail);
  }
}
/* ext72 mod menu: live switch (1 = applied now; 0 = the mod is not running, a restart is needed) */
int weapon_perks_set_enabled(int on){
  if(g_state != 1) return 0;
  g_menu_en = on ? 1 : 0;
  if(!on){ if(g_cfg) g_cfg->enable = 0; } else load_config(1);
  return 1;
}
