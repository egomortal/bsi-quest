/* economy.c -- BSI-Quest economy mod prototype: money multipliers (with an extra bonus for optional pickups) and
   vending-machine price multipliers, configured from a text file that is re-read every 2 s.

   MONEY (all amounts below are Silver Eagles)
   - Every money item the game hands out (placed piles and coins, registers, purses, crates, trash cans, enemy drops,
     scripted XSeqAct_AwardLoot) is rolled by UXLootDropItemDefinition::vf035 (f_00c14900, thiscall, reached only through
     a vtable). For XMoneyItem it computes amount = rint(2*RandRange(Min,Max)*CashMultiplier + 0.5) >> 1 with
     CashMultiplier = WorldInfo.GRI(+0x3E0).IntensityRuleSet(+0x270)+0x2C (1.0 on every difficulty).
   - The wrapper sets CashMultiplier to (game value * our multiplier) for the duration of that one roll and restores it,
     so the game's own rounding is used and the money item, its pickup message and the amount added all agree.
     The multiplier is money_mult times one category multiplier:
       explore  (default): containers, registers, purses, placed coins/piles  -> explore_bonus
       enemy:   the roll runs inside a loot TABLE whose name matches an enemy_table pattern (default "WeakAI",
                LootTable_WeakAI_Money_Base), or the item name contains "AIDrop"           -> enemy_mult
       scripted: Kismet XSeqAct_AwardLoot / SpawnLootPickup, award procs, end-of-round loot -> scripted_mult
     The source is found by scanning the guest stack above the roll for the return address of one of the 13 known
     callers of the loot-list roller f_00c13b50 (table k_src; pawn death drops count as enemy). scripted_site <hex>
     adds return addresses that count as scripted. The table context comes from wrapping
     UXLootDropTableDefinition::vf035 (f_00c15470, vtable only).
     The roll's result is read back from the output TArray (entries 0x28 bytes, amount at +8) and logged as
     "original -> adjusted"; the original is recovered from [MinAmount +0x88, MaxAmount +0x8C].
   - Scripted AdjustCurrency (+/- money from Kismet, f_00c10f60) is NOT scaled (no hook; it is a direct call).
   - UXMoneyItem::vf040 (f_00c184a0, args (PC, amount, ?), ret 0xC, vtable only) is wrapped as an OBSERVER: it logs every
     money gain with the wallet before/after. It never changes the amount (so nothing is scaled twice).

   PRICES
   - AXVendingMachine (Dollar Bill, Veni! Vidi! Vigor!, Minuteman's Armory): ItemList (+0x1438, {LootItem, ItemCost,
     ItemsAvailable}, stride 12). GetItemCost(i) (f_00bcfc60) returns ItemList[i].ItemCost when >= 0, else
     qty*ItemValue (*HackCostModifier 0.8 when the machine is possessed). It feeds both the price shown and the
     purchase check/charge, and is only called directly, so instead of hooking it we rewrite ItemCost on every PLACED
     machine (outer = Level) when it is loaded: ItemCost = round(base * price_mult * category * machine * item), where
     base = the original ItemCost, or (for computed prices) the game's own GetItemCost(i) called once as a guest call.
     Shown price == charged price automatically. For items whose price was computed, the possession discount is
     re-applied by the mod (IXHackable vtbl[7] polled every 0.5 s, HackCostModifier from the GRI DifficultyRuleSet).
   - Archetypes and class defaults are never written (instances copy their list from them: that would double-scale).
   - A wallet observer logs every money decrease as "spent" and names the vending item whose price matches.

   INTEGRATION (no in-place edits of the main tree; see economy/patches/)
   - patches.c patch_call: first line of the body calls weak economy_call(c,t). For 0xaf0480 (UXGameEngine::Tick) it
     runs the tick and returns 0 (the gear mod / weapon perks / save list code still runs); for 0xc14900, 0xc15470,
     0xc184a0 it wraps the original and returns 1.
   - rt_linux.c ft_init: weak economy_skip(a) keeps those three off the flat call table so their vtable calls reach
     patch_call. A build without economy.o behaves exactly as before.

   CONFIG: $BSI_ECONOMY (path; "0" = off), else <dir of $BSI_BLOOM_FILE>/economy.txt, else /sdcard/BSI/economy.txt.
   No file at startup: hooks not installed. BSI_PATCHES=0 also turns it off. Log lines start with "[econ]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

void call_indirect(CPU *c, uint32_t t);
typedef void (*econ_gfn)(CPU *);
econ_gfn rt_lookup(uint32_t a);

#define G_GNAMES    0x0139DFECu
#define G_GOBJECTS  0x0138C15Cu
#define G_GENGINE   0x013AA024u
#define G_GWORLD    0x013AC3ECu
#define F_TICK      0x00AF0480u
#define F_ROLL      0x00C14900u   /* UXLootDropItemDefinition::vf035 (loot item roll) */
#define F_TABLE     0x00C15470u   /* UXLootDropTableDefinition::vf035 (loot table roll) */
#define F_PICKUP    0x00C184A0u   /* UXMoneyItem::vf040 (give money to PC) */
#define F_ITEMCOST  0x00BCFC60u   /* AXVendingMachine::GetItemCost(int), thiscall, ret 4 */
#define WI_GRI      0x3E0u
#define GRI_INTENS  0x270u
#define GRI_DIFF    0x274u
#define INT_CASH    0x2Cu
#define DIFF_HACK   0xC4u
#define DEF_MIN     0x88u
#define DEF_MAX     0x8Cu
#define PC_CHARDEF  0x4B4u
#define CD_MONEY    0x54u
#define VM_HACK     0x22Cu
#define VM_ITEMLIST 0x1438u
#define VM_AVAIL    0x1444u
#define UO_OUTER 0x14u
#define UO_NAME  0x18u
#define UO_CLASS 0x20u
#define US_SUPER 0x34u
#define US_CHILDREN 0x38u
#define UF_NEXT 0x28u
#define UP_OFFSET 0x48u
#define LOGP "[econ] "

enum { CAT_EXPLORE, CAT_ENEMY, CAT_SCRIPTED };
static const char *const cat_name[] = { "explore", "enemy", "scripted" };
enum { PC_VIGOR, PC_WEAPON, PC_DOLLAR };
static const char *const pcat_name[] = { "vigor", "weapon", "dollar" };

typedef struct { char pat[80]; float mult; int fixed; } Rule;   /* fixed >= 0: absolute price */
typedef struct {
  int enable, verbose;
  float money_mult, explore_bonus, enemy_mult, scripted_mult;
  char enemy_tbl[8][48]; int nenemy;
  uint32_t scripted_site[16]; int nsites;
  float price_mult, pcat[3];
  Rule machine[32]; int nmachine;
  Rule item[64]; int nitem;
  int min_price, possess_discount, prices_on;
  int selftest_money; int selftest_price;
} Config;

static Config g_ca, g_cb, *g_cfg;
static char g_file[512];
static time_t g_mtime; static off_t g_fsize;
static int g_state = -1, g_hooks;
static unsigned g_cfg_gen;
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

/* ---------------- config ---------------- */
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
static void parse_rule(Rule *r, const char *pat, const char *v){
  snprintf(r->pat, sizeof r->pat, "%s", pat);
  r->fixed = -1; r->mult = 1.0f;
  if(v[0] == '=' || v[0] == '$') r->fixed = atoi(v + 1);
  else r->mult = (float)atof(v);
}
static void parse_config(Config *c, FILE *f){
  memset(c, 0, sizeof *c);
  c->enable = 1; c->verbose = 1; c->money_mult = 1; c->explore_bonus = 1; c->enemy_mult = 1; c->scripted_mult = 1;
  c->price_mult = 1; c->pcat[0] = c->pcat[1] = c->pcat[2] = 1; c->min_price = 1; c->possess_discount = 1; c->prices_on = 1;
  snprintf(c->enemy_tbl[0], 48, "WeakAI"); c->nenemy = 1; int enemy_set = 0;
  char line[1024], *tok[24]; int ln = 0;
  while(fgets(line, sizeof line, f)){
    ln++;
    int n = ntok(line, tok, 24); if(!n) continue;
    const char *k = tok[0];
    #define ISK(s) (!strcasecmp(k, s) && n > 1)
    if(ISK("enable")) c->enable = atoi(tok[1]);
    else if(ISK("verbose")) c->verbose = atoi(tok[1]);
    else if(ISK("money_mult")) c->money_mult = (float)atof(tok[1]);
    else if(ISK("explore_bonus")) c->explore_bonus = (float)atof(tok[1]);
    else if(ISK("enemy_mult")) c->enemy_mult = (float)atof(tok[1]);
    else if(ISK("scripted_mult")) c->scripted_mult = (float)atof(tok[1]);
    else if(ISK("enemy_table")){ if(!enemy_set){ c->nenemy = 0; enemy_set = 1; }
      for(int i = 1; i < n && c->nenemy < 8; i++) snprintf(c->enemy_tbl[c->nenemy++], 48, "%s", tok[i]); }
    else if(ISK("scripted_site")){ for(int i = 1; i < n && c->nsites < 16; i++) c->scripted_site[c->nsites++] = (uint32_t)strtoul(tok[i], 0, 16); }
    else if(ISK("price_mult")) c->price_mult = (float)atof(tok[1]);
    else if(!strcasecmp(k, "price") && n > 2){
      int i = !strcasecmp(tok[1], "vigor") ? PC_VIGOR : !strcasecmp(tok[1], "weapon") ? PC_WEAPON : !strcasecmp(tok[1], "dollar") ? PC_DOLLAR : -1;
      if(i < 0) fprintf(stderr, LOGP "line %d: price vigor|weapon|dollar <mult>\n", ln); else c->pcat[i] = (float)atof(tok[2]);
    }
    else if(!strcasecmp(k, "machine") && n > 2){ if(c->nmachine < 32) parse_rule(&c->machine[c->nmachine++], tok[1], tok[2]); }
    else if(!strcasecmp(k, "item") && n > 2){ if(c->nitem < 64) parse_rule(&c->item[c->nitem++], tok[1], tok[2]); }
    else if(ISK("min_price")) c->min_price = atoi(tok[1]);
    else if(ISK("possess_discount")) c->possess_discount = atoi(tok[1]);
    else if(ISK("prices")) c->prices_on = atoi(tok[1]);
    else if(ISK("selftest_money")) c->selftest_money = atoi(tok[1]);
    else if(ISK("selftest_price")) c->selftest_price = atoi(tok[1]);
    else fprintf(stderr, LOGP "line %d: unknown directive '%s'\n", ln, k);
    #undef ISK
  }
}
static void log_config(Config *c){
  fprintf(stderr, LOGP "config %s: %s; money x%g (explore x%g, enemy x%g, scripted x%g); prices x%g (vigor x%g, weapon x%g, dollar x%g), %d machine rules, %d item rules, possession discount %s\n",
          g_file, c->enable ? "enabled" : "DISABLED", c->money_mult, c->explore_bonus, c->enemy_mult, c->scripted_mult, c->price_mult,
          c->pcat[0], c->pcat[1], c->pcat[2], c->nmachine, c->nitem, c->possess_discount ? "kept" : "off");
  if(c->verbose > 1){
    for(int i = 0; i < c->nitem; i++) fprintf(stderr, LOGP "  item rule \"%s\" %s%g\n", c->item[i].pat, c->item[i].fixed >= 0 ? "= $" : "x", c->item[i].fixed >= 0 ? (float)c->item[i].fixed : c->item[i].mult);
    for(int i = 0; i < c->nmachine; i++) fprintf(stderr, LOGP "  machine rule \"%s\" %s%g\n", c->machine[i].pat, c->machine[i].fixed >= 0 ? "= $" : "x", c->machine[i].fixed >= 0 ? (float)c->machine[i].fixed : c->machine[i].mult);
  }
}
static int find_config_path(void){
  const char *e = getenv("BSI_ECONOMY");
  if(e && *e){ if(!strcmp(e, "0")) return 0; snprintf(g_file, sizeof g_file, "%s", e); return 1; }
  const char *b = getenv("BSI_BLOOM_FILE");
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl){ snprintf(g_file, sizeof g_file, "%.*s/economy.txt", (int)(sl - b), b); return 1; } }
  snprintf(g_file, sizeof g_file, "/sdcard/BSI/economy.txt"); return 1;
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
  if(stat(g_file, &st) != 0 && g_builtin){   /* ext72: built-in config (the example economy.txt) */
    if(!force) return 0;
    const char *t = ext_mod_builtin ? ext_mod_builtin("economy") : 0; FILE *f = t ? fmemopen((void *)t, strlen(t), "r") : 0; if(!f) return 0;
    Config *spare = g_cfg == &g_ca ? &g_cb : &g_ca;
    parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;
    g_cfg = spare; g_cfg_gen++; fprintf(stderr, LOGP "built-in config (ext72, no %s)\n", g_file); log_config(spare);
    return 1;
  }
  if(stat(g_file, &st) == 0) g_builtin = 0;
  if(stat(g_file, &st) != 0){ if(g_cfg && g_cfg->enable){ fprintf(stderr, LOGP "%s gone: economy mod idle (prices restored)\n", g_file); g_cfg->enable = 0; g_cfg_gen++; } return 0; }
  if(!force && st.st_mtime == g_mtime && st.st_size == g_fsize) return 0;
  FILE *f = fopen(g_file, "r"); if(!f) return 0;
  Config *spare = g_cfg == &g_ca ? &g_cb : &g_ca;
  parse_config(spare, f); fclose(f); if(g_menu_en == 0) spare->enable = 0;   /* ext72 menu switch */
  g_mtime = st.st_mtime; g_fsize = st.st_size; g_cfg = spare; g_cfg_gen++;
  log_config(spare);
  return 1;
}
static void econ_init(void){
  if(g_state >= 0) return;
  g_state = 0;
  const char *p = getenv("BSI_PATCHES"); if(p && !atoi(p)){ fprintf(stderr, LOGP "BSI_PATCHES=0: economy mod off\n"); return; }
  if(!find_config_path()){ fprintf(stderr, LOGP "BSI_ECONOMY=0: economy mod off\n"); return; }
  struct stat st;
  if(stat(g_file, &st) != 0){
    if(!(ext_mod_default_on && ext_mod_default_on("economy"))){ fprintf(stderr, LOGP "no config at %s: economy mod off (set BSI_ECONOMY or push economy.txt)\n", g_file); if(ext_mod_started) ext_mod_started("economy", 0); return; }
    g_builtin = 1;   /* ext72: on with the built-in config */
  } else if(ext_mod_file_allowed && !ext_mod_file_allowed("economy")){ if(ext_mod_started) ext_mod_started("economy", 0); return; }
  /* exe identity: prologues of the hooked/called functions */
  static const struct { uint32_t a; uint8_t b[8]; int n; } chk[] = {
    { F_ROLL,     { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC }, 8 },   /* push ebp; mov ebp,esp; and esp,-16; sub esp,.. */
    { F_PICKUP,   { 0x8B, 0x4C, 0x24, 0x04, 0x56 }, 5 },                     /* mov ecx,[esp+4]; push esi */
    { F_ITEMCOST, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC }, 8 },
  };
  for(unsigned i = 0; i < sizeof chk / sizeof *chk; i++)
    for(int j = 0; j < chk[i].n; j++) if(R8(chk[i].a + j) != chk[i].b[j]){ fprintf(stderr, LOGP "exe identity check failed at %08x: economy mod off\n", chk[i].a); return; }
  load_config(1);
  g_state = 1; if(ext_mod_started) ext_mod_started("economy", 1);
  fprintf(stderr, LOGP "economy mod loaded: roll hook %08x, table hook %08x, pickup observer %08x, vending GetItemCost %08x\n", F_ROLL, F_TABLE, F_PICKUP, F_ITEMCOST);
}

/* ---------------- host-side reflection (reads only) ---------------- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";
  return (const char *)MEMP(e + 0x10);
}
static const char *oname(uint32_t o){   /* full FName text: {Index, Number} -> "Name_<Number-1>" when Number > 0 */
  if(!gptr(o)) return "None";
  uint32_t num = R32(o + UO_NAME + 4); const char *b = fname_str(R32(o + UO_NAME));
  if(!num) return b;
  static char buf[8][128]; static unsigned k; char *r = buf[k++ & 7];
  snprintf(r, 128, "%s_%u", b, num - 1); return r;
}
static const char *bname(uint32_t o){ return gptr(o) ? fname_str(R32(o + UO_NAME)) : "None"; }
static const char *cname(uint32_t o){ return gptr(o) ? bname(R32(o + UO_CLASS)) : "None"; }
static int is_a(uint32_t o, const char *cls){
  if(!gptr(o)) return 0;
  for(uint32_t c = R32(o + UO_CLASS), k = 0; gptr(c) && k < 40; c = R32(c + US_SUPER), k++) if(!strcmp(bname(c), cls)) return 1;
  return 0;
}
static uint32_t find_prop(uint32_t cls, const char *name){
  static struct { uint32_t cls; char n[48]; uint32_t p; } cache[64]; static unsigned nc;
  for(unsigned i = 0; i < nc && i < 64; i++) if(cache[i].cls == cls && !strcmp(cache[i].n, name)) return cache[i].p;
  uint32_t found = 0;
  for(uint32_t c = cls, k = 0; gptr(c) && k < 40 && !found; c = R32(c + US_SUPER), k++)
    for(uint32_t f = R32(c + US_CHILDREN), m = 0; gptr(f) && m < 8000; f = R32(f + UF_NEXT), m++)
      if(!strcasecmp(bname(f), name) && strstr(cname(f), "Property")){ found = f; break; }
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
static const char *top_name(uint32_t o){ if(!gptr(o)) return "-"; for(int k = 0; k < 8 && gptr(R32(o + UO_OUTER)); k++) o = R32(o + UO_OUTER); return oname(o); }
static uint32_t player_pc(void){
  uint32_t eng = R32(G_GENGINE); if(!gptr(eng)) return 0;
  uint32_t gp = R32(eng + 0x1B0); if(!gptr(gp) || !R32(eng + 0x1B4)) return 0;
  uint32_t lp = R32(gp); if(!gptr(lp)) return 0;
  uint32_t pc = R32(lp + 0x2C); return gptr(pc) && is_a(pc, "XPlayerController") ? pc : 0;
}
static uint32_t chardef(uint32_t pc){ if(!gptr(pc)) return 0; uint32_t d = R32(pc + PC_CHARDEF); return gptr(d) ? d : pc + PC_CHARDEF + 4; }   /* f_00975cc0 */
static int wallet(uint32_t pc){ uint32_t d = chardef(pc); return d ? (int)R32(d + CD_MONEY) : -1; }
static uint32_t cash_mult_addr(void){   /* WorldInfo.GRI.IntensityRuleSet.CashMultiplier, the value f_00c14900 reads */
  uint32_t wi = world_info(); if(!wi) return 0;
  uint32_t gri = R32(wi + WI_GRI); if(!gptr(gri)) return 0;
  uint32_t rs = R32(gri + GRI_INTENS); if(!gptr(rs)) return 0;
  return rs + INT_CASH;
}
static int ci_has(const char *s, const char *pat){   /* case-insensitive substring */
  size_t n = strlen(pat); if(!n) return 1;
  for(; *s; s++) if(!strncasecmp(s, pat, n)) return 1;
  return 0;
}

/* ---------------- money: roll hook, table context, pickup observer ---------------- */
static uint32_t g_tbl[8]; static int g_ntbl;
static int g_inroll;
static unsigned g_rolls, g_money_rolls, g_pickups; static long g_raw_sum, g_adj_sum, g_pick_sum;
static struct { int adj, raw; float m; char cat; } g_pend[64]; static unsigned g_npend;
static struct { uint32_t site; unsigned n; int cat; } g_sites[32]; static int g_nsites;

/* (int)rint(2*v*m + 0.5) >> 1, in float with round-to-nearest-even like cvtss2si under the default MXCSR */
static int game_round(int v, float m){ float x = (float)v * m * 2.0f + 0.5f; return (int)lrintf(x) >> 1; }

typedef struct { uint32_t arr, data, num; int v8[64]; } Snap;
static int snap_arr(Snap *s, uint32_t arr){
  s->arr = 0; if(!gptr(arr)) return 0;
  uint32_t d = R32(arr), n = R32(arr + 4), mx = R32(arr + 8);
  if(n > 256 || mx < n || mx > 65536 || (n && !gptr(d))) return 0;
  s->arr = arr; s->data = d; s->num = n;
  for(uint32_t i = 0; i < n && i < 64; i++) s->v8[i] = (int)R32(d + 0x28 * i + 8);
  return 1;
}
static int snap_delta(Snap *s, int *ok){   /* sum of the +8 changes (new entries count from 0) */
  *ok = 0; if(!s->arr) return 0;
  uint32_t d = R32(s->arr), n = R32(s->arr + 4);
  if(n > 256 || (n && !gptr(d))) return 0;
  long sum = 0;
  for(uint32_t i = 0; i < n && i < 64; i++){
    int now = (int)R32(d + 0x28 * i + 8);
    int was = (d == s->data && i < s->num) ? s->v8[i] : 0;
    if(d != s->data && i < s->num) was = s->v8[i];   /* reallocated: same order */
    sum += now - was;
  }
  *ok = 1; return (int)sum;
}
/* Every money roll seen so far comes through the loot-list roller f_00c13270 (return 0xc13470), called by f_00c13b50,
   which has 13 direct callers. Their return addresses identify the source; the first one found on the guest stack
   above the roll wins. Names from the vtable symbol list (names_all.tsv). */
static const struct { uint32_t ret; int cat; const char *what; } k_src[] = {
  { 0x968592u, CAT_ENEMY,    "pawn death drop (AXPawn::vf316 -> f_009683d0)" },
  { 0x969cc7u, CAT_ENEMY,    "pawn loot (AXPawn::vf314/vf316 -> f_00969b80)" },
  { 0x96877fu, CAT_SCRIPTED, "award-loot gameplay proc / Return to Sender (f_009686b0)" },
  { 0x9b6d08u, CAT_SCRIPTED, "AXWorldInfo::execAwardEndOfRoundLoot" },
  { 0x9e5488u, CAT_SCRIPTED, "Kismet XSeqAct_AwardLoot" },
  { 0x9f27a9u, CAT_SCRIPTED, "Kismet XSeqAct_SpawnLootPickup" },
  { 0xb267b2u, CAT_EXPLORE,  "container search (UXContainerSearchScreen)" },
  { 0xbc7c7eu, CAT_EXPLORE,  "reactive actor / placed pickup (AXReactiveActor::vf086 -> f_00bc7bb0)" },
  { 0xbd1dcbu, CAT_EXPLORE,  "vending machine stock (f_00bd1c60)" },
  { 0xc1b19cu, CAT_EXPLORE,  "meta item (UXMetaItem::vf044)" },
  { 0xc4a0feu, CAT_EXPLORE,  "spawn-loot gameplay proc (UXSpawnLootGameplayProc)" },
  { 0xc9f411u, CAT_EXPLORE,  "reaction spawn loot (UXReactionSpawnLootItem)" },
  { 0xcd8691u, CAT_SCRIPTED, "DLC weapon (AXDLCCWeapon::vf202)" },
};
static int g_src_last = -1;
static int stack_source(Config *cf, CPU *c, uint32_t *ret_out){
  *ret_out = 0;
  for(uint32_t a = c->esp, end = c->esp + 0x1000; a < end; a += 4){
    uint32_t v = R32(a);
    for(int i = 0; i < cf->nsites; i++) if(cf->scripted_site[i] == v){ *ret_out = v; return 100 + i; }
    for(unsigned i = 0; i < sizeof k_src / sizeof *k_src; i++) if(k_src[i].ret == v){ *ret_out = v; return (int)i; }
  }
  return -1;
}
static int roll_category(Config *cf, uint32_t def, uint32_t site){
  for(int i = 0; i < cf->nsites; i++) if(cf->scripted_site[i] == site) return CAT_SCRIPTED;
  if(g_src_last >= 100) return CAT_SCRIPTED;
  if(g_src_last >= 0 && k_src[g_src_last].cat != CAT_EXPLORE) return k_src[g_src_last].cat;
  if(ci_has(oname(def), "AIDrop")) return CAT_ENEMY;
  for(int t = 0; t < g_ntbl; t++){ const char *tn = oname(g_tbl[t]); for(int i = 0; i < cf->nenemy; i++) if(ci_has(tn, cf->enemy_tbl[i])) return CAT_ENEMY; }
  return CAT_EXPLORE;
}
static int econ_roll(CPU *c){
  econ_gfn orig = rt_lookup(F_ROLL); if(!orig) return 0;
  Config *cf = g_cfg;
  uint32_t def = c->ecx, site = R32(c->esp), a0 = R32(c->esp + 4), a1 = R32(c->esp + 8);
  g_rolls++;
  if(!cf || !cf->enable || g_inroll || !gptr(def) || !strstr(oname(def), "Money")){ g_inroll++; orig(c); g_inroll--; return 1; }
  uint32_t src_ret; g_src_last = stack_source(cf, c, &src_ret);
  int cat = roll_category(cf, def, site);
  const char *src_name = g_src_last >= 100 ? "scripted_site from config" : g_src_last >= 0 ? k_src[g_src_last].what : "unknown source";
  float m = cf->money_mult * (cat == CAT_EXPLORE ? cf->explore_bonus : cat == CAT_ENEMY ? cf->enemy_mult : cf->scripted_mult);
  uint32_t cm = cash_mult_addr(); float cm0 = cm ? RF32(cm) : 1.0f;
  if(!(cm0 > 0.01f && cm0 < 100.0f)){ cm = 0; cm0 = 1.0f; }
  Snap s0, s1; snap_arr(&s0, a0); snap_arr(&s1, a1);
  int changed = cm && fabsf(m - 1.0f) > 1e-6f;
  if(changed) WF32(cm, cm0 * m);
  g_inroll++; orig(c); g_inroll--;
  if(changed) WF32(cm, cm0);
  int ok0, ok1, d0 = snap_delta(&s0, &ok0), d1 = snap_delta(&s1, &ok1);
  int adj = d0 > 0 ? d0 : d1 > 0 ? d1 : 0;
  int mn = (int)R32(def + DEF_MIN), mx = (int)R32(def + DEF_MAX), raw = -1;
  float meff = changed ? cm0 * m : cm0;
  if(adj > 0 && mn >= 0 && mx >= mn && mx - mn < 100000){
    for(int v = mn; v <= mx; v++) if(game_round(v, meff) == adj){ raw = game_round(v, cm0); break; }
  }
  g_money_rolls++;
  if(adj > 0){ g_adj_sum += adj; g_raw_sum += raw > 0 ? raw : adj;
    unsigned k = g_npend++ & 63; g_pend[k].adj = adj; g_pend[k].raw = raw; g_pend[k].m = meff; g_pend[k].cat = (char)cat; }
  uint32_t key = src_ret ? src_ret : site;
  int si; for(si = 0; si < g_nsites; si++) if(g_sites[si].site == key) break;
  if(si == g_nsites && g_nsites < 32){ g_sites[si].site = key; g_sites[si].n = 0; g_sites[si].cat = cat; g_nsites++;
    fprintf(stderr, LOGP "new money source %08x = %s (direct caller %08x, first item %s, table %s): category %s\n", key, src_name, site, oname(def), g_ntbl ? oname(g_tbl[g_ntbl - 1]) : "-", cat_name[cat]); }
  if(si < 32) g_sites[si].n++;
  if(cf->verbose){
    if(adj > 0) fprintf(stderr, LOGP "money roll %s [%s x%g, %08x]: $%s%d -> $%d%s\n", oname(def), cat_name[cat], meff, key,
                        raw < 0 ? "~" : "", raw < 0 ? (int)lrintf(adj / (meff > 0 ? meff : 1)) : raw, adj, cm ? "" : " (no CashMultiplier found: unchanged)");
    else if(cf->verbose > 1) fprintf(stderr, LOGP "money roll %s [%s]: no amount seen in out arrays (a0 %08x ok %d d %d, a1 %08x ok %d d %d)\n", oname(def), cat_name[cat], a0, ok0, d0, a1, ok1, d1);
  }
  return 1;
}
static int econ_table(CPU *c){
  econ_gfn orig = rt_lookup(F_TABLE); if(!orig) return 0;
  int pushed = 0; if(g_ntbl < 8){ g_tbl[g_ntbl++] = c->ecx; pushed = 1; }
  orig(c);
  if(pushed) g_ntbl--;
  return 1;
}
static int econ_pickup(CPU *c){
  econ_gfn orig = rt_lookup(F_PICKUP); if(!orig) return 0;
  uint32_t pc = R32(c->esp + 4); int amt = (int)R32(c->esp + 8);
  int w0 = gptr(pc) ? wallet(pc) : -1;
  orig(c);
  int w1 = gptr(pc) ? wallet(pc) : -1;
  g_pickups++; g_pick_sum += amt;
  Config *cf = g_cfg;
  if(cf && cf->verbose){
    int raw = -1; char cat = -1; float m = 1;
    for(unsigned i = 0; i < 64 && i < g_npend; i++){ unsigned k = (g_npend - 1 - i) & 63; if(g_pend[k].adj == amt){ raw = g_pend[k].raw; cat = g_pend[k].cat; m = g_pend[k].m; g_pend[k].adj = -1; break; } }
    if(raw >= 0) fprintf(stderr, LOGP "money gain +$%d (original $%d, %s x%g): wallet $%d -> $%d\n", amt, raw, cat_name[(int)cat], m, w0, w1);
    else fprintf(stderr, LOGP "money gain +$%d: wallet $%d -> $%d\n", amt, w0, w1);
  }
  return 1;
}

/* ---------------- prices ---------------- */
typedef struct {
  uint32_t vm; int idx; int orig; int base; int written; int computed; int cat; int hacked;
  char item[80]; char where[96];
} PRec;
static PRec g_pr[512]; static int g_npr;
static uint32_t g_vms[64]; static int g_nvm;
static unsigned g_price_writes;

static int item_category(const char *n){
  if(ci_has(n, "StableTonic") || ci_has(n, "Plasmid") || ci_has(n, "Vigor_Upgrade")) return ci_has(n, "VigorEnergy") ? PC_DOLLAR : PC_VIGOR;
  if(!strncasecmp(n, "Loot_Upgrade_", 13)) return PC_WEAPON;
  return PC_DOLLAR;
}
static int rule_apply(Rule *r, int n, const char *s, float *mult){   /* last match wins; returns fixed price or -1 */
  int fixed = -1, hit = 0; float m = 1;
  for(int i = 0; i < n; i++) if(ci_has(s, r[i].pat)){ hit = 1; if(r[i].fixed >= 0){ fixed = r[i].fixed; m = 1; } else { fixed = -1; m = r[i].mult; } }
  if(hit) *mult *= m;
  return fixed;
}
static int target_price(Config *cf, PRec *p){
  if(!cf->enable || !cf->prices_on) return p->base;
  float m = cf->price_mult * cf->pcat[p->cat];
  int f1 = rule_apply(cf->machine, cf->nmachine, p->where, &m);
  int f2 = rule_apply(cf->item, cf->nitem, p->item, &m);
  int v = f2 >= 0 ? f2 : f1 >= 0 ? f1 : (int)floorf(p->base * m + 0.5f);
  if(v < cf->min_price && p->base > 0) v = cf->min_price;
  return v;
}
static int guest_item_cost(CPU *c, uint32_t vm, int i){
  CPU save = *c; PUSH32(c, (uint32_t)i); c->ecx = vm; guest_call(c, F_ITEMCOST); int v = (int)c->eax; *c = save; return v;
}
static int guest_hacked(CPU *c, uint32_t vm){
  uint32_t vt = R32(vm + VM_HACK); if(!gptr(vt)) return 0;
  uint32_t fn = R32(vt + 0x1C); if(fn < 0x401000u || fn > 0x1400000u) return 0;
  CPU save = *c; c->ecx = vm + VM_HACK; guest_call(c, fn); int v = (int)(c->eax & 0xFF); *c = save; return v;
}
static float hack_mod(void){
  uint32_t wi = world_info(); if(!wi) return 1;
  uint32_t gri = R32(wi + WI_GRI); if(!gptr(gri)) return 1;
  uint32_t d = R32(gri + GRI_DIFF); if(!gptr(d)) return 1;
  float v = RF32(d + DIFF_HACK); return v > 0.05f && v <= 1.5f ? v : 1;
}
/* placed instance: outer chain reaches a Level before the package */
static int placed(uint32_t o){ uint32_t out = R32(o + UO_OUTER); return gptr(out) && !strcmp(cname(out), "Level") && strncmp(oname(o), "Default__", 9); }

static void write_price(Config *cf, PRec *p, int v, const char *why){
  uint32_t list = R32(p->vm + VM_ITEMLIST), n = R32(p->vm + VM_ITEMLIST + 4);
  if(!gptr(list) || (uint32_t)p->idx >= n) return;
  uint32_t a = list + 12 * p->idx + 4;
  int cur = (int)R32(a);
  /* computed items keep -1 when the target equals the game's own price and no discount is needed (game logic kept) */
  int want = v;
  if(p->computed && v == p->base && !p->hacked) want = -1;
  if(cur == want){ p->written = want; return; }
  W32(a, (uint32_t)want); p->written = want; g_price_writes++;
  if(cf->verbose) fprintf(stderr, LOGP "price %s [%s] %s: $%d -> $%d (shown and charged)%s%s\n", p->where, pcat_name[p->cat], p->item, p->base, v, why[0] ? " " : "", why);
}
static int scan_vendors(CPU *c, Config *cf, int dry_archetypes){
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  if(!gptr(go)) return 0;
  int nvm = 0, nnew = 0, pending = 0;
  for(uint32_t i = 0; i < n; i++){
    uint32_t o = R32(go + 4 * i); if(!gptr(o)) continue;
    const char *k = cname(o);
    if(!strstr(k, "VendingMachine") || !is_a(o, "XVendingMachine")) continue;
    int pl = placed(o);
    int il = prop_off(o, "ItemList", VM_ITEMLIST);
    uint32_t list = R32(o + il), cnt = R32(o + il + 4);
    if(!pl){
      static uint32_t dry_done[128]; static int ndry; int dd = 0;
      for(int q = 0; q < ndry; q++) if(dry_done[q] == o) dd = 1;
      if(dry_archetypes && !dd && ndry < 128 && gptr(list) && cnt < 64){
        dry_done[ndry++] = o;
        for(uint32_t j = 0; j < cnt; j++){
          uint32_t li = R32(list + 12 * j); int cost = (int)R32(list + 12 * j + 4);
          PRec t; memset(&t, 0, sizeof t); snprintf(t.item, sizeof t.item, "%s", oname(li)); snprintf(t.where, sizeof t.where, "%s.%s", top_name(o), oname(o));
          t.cat = item_category(t.item); t.base = cost;
          if(cost >= 0) fprintf(stderr, LOGP "dry run (archetype, not written) %s [%s] %s: $%d -> $%d\n", t.where, pcat_name[t.cat], t.item, cost, target_price(cf, &t));
          else fprintf(stderr, LOGP "dry run (archetype, not written) %s [%s] %s: computed price (qty*ItemValue), x%g at runtime\n", t.where, pcat_name[t.cat], t.item, cf->price_mult * cf->pcat[t.cat]);
        }
      }
      continue;
    }
    if(!gptr(list) || cnt > 64) continue;
    nvm++;
    int seen = 0; for(int v = 0; v < g_nvm; v++) if(g_vms[v] == o) seen = 1;
    if(!seen && g_nvm < 64){ g_vms[g_nvm++] = o;
      fprintf(stderr, LOGP "vending machine %s.%s (%s) at %08x: %u items, currency %u\n", top_name(o), oname(o), k, o, cnt, R8(o + 0x1404)); }
    uint32_t avail = R32(o + VM_AVAIL), navail = R32(o + VM_AVAIL + 4);
    for(uint32_t j = 0; j < cnt; j++){
      int r; for(r = 0; r < g_npr; r++) if(g_pr[r].vm == o && g_pr[r].idx == (int)j) break;
      int cur = (int)R32(list + 12 * j + 4);
      uint32_t li = R32(list + 12 * j);
      if(r < g_npr && cur != g_pr[r].written){ g_pr[r] = g_pr[--g_npr]; r = g_npr; }   /* re-created or reset by the game: start over */
      if(r == g_npr){
        if(g_npr >= 512) continue;
        PRec *p = &g_pr[r]; memset(p, 0, sizeof *p);
        p->vm = o; p->idx = (int)j; p->orig = cur; p->computed = cur < 0;
        snprintf(p->item, sizeof p->item, "%s", gptr(li) ? oname(li) : "None");
        snprintf(p->where, sizeof p->where, "%s.%s", top_name(o), oname(o));
        p->cat = item_category(p->item);
        if(p->computed){
          if(!gptr(avail) || j >= navail){ pending++; continue; }   /* AvailableItems not built yet: retry */
          p->base = guest_item_cost(c, o, (int)j);
          p->hacked = 0;
          if(p->base <= 0){ pending++; continue; }
        } else p->base = cur;
        p->written = cur; g_npr++; nnew++;
      }
    }
  }
  /* apply targets (also after a config change) */
  for(int r = 0; r < g_npr; r++){ PRec *p = &g_pr[r]; write_price(cf, p, target_price(cf, p), ""); }
  if(nnew && cf->verbose) fprintf(stderr, LOGP "%d placed vending machines, %d price records (%d new)%s\n", nvm, g_npr, nnew, pending ? ", some computed prices wait for AvailableItems" : "");
  return pending;
}
static void possess_check(CPU *c, Config *cf){
  if(!cf->possess_discount) return;
  float hm = hack_mod();
  for(int v = 0; v < g_nvm; v++){
    uint32_t vm = g_vms[v]; int any = 0;
    for(int r = 0; r < g_npr; r++) if(g_pr[r].vm == vm && g_pr[r].computed) any = 1;
    if(!any) continue;
    int h = guest_hacked(c, vm);
    for(int r = 0; r < g_npr; r++){
      PRec *p = &g_pr[r]; if(p->vm != vm || !p->computed || p->hacked == h) continue;
      p->hacked = h;
      int t = target_price(cf, p);
      write_price(cf, p, h ? (int)floorf(t * hm + 0.5f) : t, h ? "(machine possessed: discount kept)" : "(possession ended)");
    }
  }
}
static void restore_all(Config *cf){
  for(int r = 0; r < g_npr; r++){ PRec *p = &g_pr[r]; uint32_t list = R32(p->vm + VM_ITEMLIST);
    if(gptr(list) && (int)R32(list + 12 * p->idx + 4) == p->written){ W32(list + 12 * p->idx + 4, (uint32_t)p->orig); p->written = p->orig; } }
  if(cf && cf->verbose) fprintf(stderr, LOGP "prices restored on %d records\n", g_npr);
}

/* ---------------- price self test (cloud harness: no vending machine is loaded before the town) ----------------
   selftest_price 2: build a scratch machine from Default__XVendingMachine (its vtables, so the IXHackable call is real)
   with a two-entry ItemList {Bucking Bronco $375, Possession Aid $50}, ask the game's own GetItemCost before and after the
   mod writes the prices, and query the possession state. The scratch memory comes from the game's appMalloc and is
   never registered with the engine (test only, about 5 KB, not freed). */
static uint32_t game_malloc(CPU *c, uint32_t n){ CPU save = *c; PUSH32(c, 8); PUSH32(c, n); guest_call(c, 0x00482A80u); uint32_t p = c->eax; *c = save; return p; }
static void selftest_vendor(CPU *c, Config *cf){
  static const uint8_t mal[] = { 0x8B, 0x0D, 0xC8, 0x1C, 0x37, 0x01 };   /* appMalloc: mov ecx,[GMalloc] */
  for(unsigned i = 0; i < sizeof mal; i++) if(R8(0x482A80u + i) != mal[i]){ fprintf(stderr, LOGP "price selftest: appMalloc prologue mismatch, skipped\n"); return; }
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4), cdo = 0;
  for(uint32_t i = 0; i < n && gptr(go) && !cdo; i++){ uint32_t o = R32(go + 4 * i); if(gptr(o) && !strcmp(oname(o), "Default__XVendingMachine")) cdo = o; }
  if(!cdo){ fprintf(stderr, LOGP "price selftest: Default__XVendingMachine not loaded yet\n"); return; }
  uint32_t fake = game_malloc(c, 0x1500), arr = game_malloc(c, 32);
  if(!gptr(fake) || !gptr(arr)){ fprintf(stderr, LOGP "price selftest: appMalloc failed\n"); return; }
  for(uint32_t k = 0; k < 0x14E8; k += 4) W32(fake + k, R32(cdo + k));
  W32(arr + 0, 0); W32(arr + 4, 375); W32(arr + 8, 0xFFFFFFFFu);    /* {LootItem, ItemCost, ItemsAvailable} */
  W32(arr + 12, 0); W32(arr + 16, 50); W32(arr + 20, 1);
  W32(fake + VM_ITEMLIST, arr); W32(fake + VM_ITEMLIST + 4, 2); W32(fake + VM_ITEMLIST + 8, 2);
  W32(fake + VM_AVAIL, 0); W32(fake + VM_AVAIL + 4, 0); W32(fake + VM_AVAIL + 8, 0);
  W32(fake + 0xA4, world_info());
  static const char *const nm[2] = { "Loot_Consumable_Plasmid_RollingThunder_Founder_Vendor", "Loot_StableTonic_Mesmerize_MezHumans" };
  int before[2], after[2];
  for(int i = 0; i < 2; i++) before[i] = guest_item_cost(c, fake, i);
  int h = guest_hacked(c, fake);
  for(int i = 0; i < 2; i++){
    PRec p; memset(&p, 0, sizeof p); p.vm = fake; p.idx = i; p.orig = p.base = p.written = (int)R32(arr + 12 * i + 4);
    snprintf(p.item, sizeof p.item, "%s", nm[i]); snprintf(p.where, sizeof p.where, "selftest.ScratchMachine(copy of %s)", oname(cdo));
    p.cat = item_category(p.item);
    write_price(cf, &p, target_price(cf, &p), "(selftest)");
    after[i] = guest_item_cost(c, fake, i);
  }
  fprintf(stderr, LOGP "price selftest: game GetItemCost(0) %d -> %d, GetItemCost(1) %d -> %d; IXHackable::vtbl[7] (possessed?) returned %d, HackCostModifier %g\n",
          before[0], after[0], before[1], after[1], h, hack_mod());
}

/* ---------------- entry points ---------------- */
int economy_skip(uint32_t a){
  if(a != F_ROLL && a != F_TABLE && a != F_PICKUP) return 0;
  econ_init();
  if(g_state != 1) return 0;
  g_hooks |= a == F_ROLL ? 1 : a == F_TABLE ? 2 : 4;
  return 1;
}
static void economy_tick(CPU *c);
int economy_call(CPU *c, uint32_t t){
  if(t == F_TICK){ economy_tick(c); return 0; }
  if(g_state != 1) return 0;
  if(t == F_ROLL) return econ_roll(c);
  if(t == F_TABLE) return econ_table(c);
  if(t == F_PICKUP) return econ_pickup(c);
  return 0;
}
static void economy_tick(CPU *c){
  if(g_state < 0) econ_init();
  if(g_state != 1) return;
  static double next_poll, next_work, next_scan, next_status; static long ticks; ticks++;
  static unsigned seen_gen; static uint32_t last_nobj; static int pending, was_enabled = 1;
  double t = now_s();
  if(t >= next_poll){ next_poll = t + 2.0; load_config(0); }
  Config *cf = g_cfg; if(!cf) return;
  if(t < next_work || ticks < 3) return;
  next_work = t + 0.5;
  if(!cf->enable){ if(was_enabled){ restore_all(cf); was_enabled = 0; } }
  else was_enabled = 1;
  /* wallet observer */
  static int last_w = -1; static uint32_t last_pc;
  uint32_t pc = player_pc(); int w = pc ? wallet(pc) : -1;
  if(pc != last_pc){ last_pc = pc; if(pc) fprintf(stderr, LOGP "player controller %08x (%s): wallet $%d\n", pc, cname(pc), w); last_w = w; }
  else if(w >= 0 && last_w >= 0 && w != last_w){
    if(w < last_w){
      int d = last_w - w; const char *hit = 0, *where = 0;
      for(int r = 0; r < g_npr; r++){ int pr = g_pr[r].written >= 0 ? g_pr[r].written : g_pr[r].base; if(pr == d){ hit = g_pr[r].item; where = g_pr[r].where; } }
      if(hit) fprintf(stderr, LOGP "spent $%d: wallet $%d -> $%d (charged = current vending price of %s at %s)\n", d, last_w, w, hit, where);
      else fprintf(stderr, LOGP "spent $%d: wallet $%d -> $%d\n", d, last_w, w);
    } else if(cf->verbose > 1) fprintf(stderr, LOGP "wallet $%d -> $%d\n", last_w, w);
    last_w = w;
  } else if(w >= 0) last_w = w;
  /* vending prices */
  uint32_t nobj = R32(G_GOBJECTS + 4);
  if(t >= next_scan && (nobj != last_nobj || seen_gen != g_cfg_gen || pending)){
    next_scan = t + 2.0; last_nobj = nobj;
    seen_gen = g_cfg_gen;
    pending = scan_vendors(c, cf, cf->selftest_price > 0);
  }
  possess_check(c, cf);
  static int stp_done; if(cf->selftest_price == 2 && !stp_done && ticks > 50){ stp_done = 1; selftest_vendor(c, cf); }
  /* self test: one money pickup through the hooked UXMoneyItem::vf040 */
  static int st_done;
  if(cf->selftest_money > 0 && !st_done && pc){
    uint32_t pawn = R32(pc + prop_off(pc, "Pawn", 0x1FC));
    if(gptr(pawn)){
      st_done = 1;
      uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4), item = 0;
      for(uint32_t i = 0; i < n && gptr(go) && !item; i++){ uint32_t o = R32(go + 4 * i); if(gptr(o) && !strcmp(cname(o), "XMoneyItem")) item = o; }
      int amt = cf->selftest_money, raw = amt;
      float m = cf->money_mult * cf->explore_bonus; int adj = game_round(raw, m);
      unsigned k = g_npend++ & 63; g_pend[k].adj = adj; g_pend[k].raw = raw; g_pend[k].m = m; g_pend[k].cat = CAT_EXPLORE;
      fprintf(stderr, LOGP "selftest: $%d explore pickup at x%g = $%d (the game's rounding), given through UXMoneyItem::vf040 (item %s %08x) to PC %08x\n", raw, m, adj, item ? oname(item) : "none", item, pc);
      CPU save = *c; PUSH32(c, 0); PUSH32(c, (uint32_t)adj); PUSH32(c, pc); c->ecx = item; guest_call(c, F_PICKUP); int got = (int)c->eax; *c = save;
      fprintf(stderr, LOGP "selftest: vf040 returned %d, wallet now $%d\n", got, wallet(pc));
      last_w = wallet(pc);
    }
  }
  if(t >= next_status){
    next_status = t + 30.0;
    uint32_t cm = cash_mult_addr();
    fprintf(stderr, LOGP "status: tick %ld, hooks %s%s%s, map %s, wallet $%d, CashMultiplier %s%g, rolls %u (money %u: $%ld -> $%ld), pickups %u (+$%ld), machines %d, price records %d (writes %u), roll sites %d\n",
            ticks, g_hooks & 1 ? "roll " : "", g_hooks & 2 ? "table " : "", g_hooks & 4 ? "pickup" : "", top_name(world_info()), w, cm ? "" : "(none) ", cm ? RF32(cm) : 0.0f,
            g_rolls, g_money_rolls, g_raw_sum, g_adj_sum, g_pickups, g_pick_sum, g_nvm, g_npr, g_price_writes, g_nsites);
  }
}
/* ext72 mod menu: live switch (1 = applied now; 0 = the mod is not running, a restart is needed). Off restores the prices
   within about 0.5 s (the economy tick's enable check); money already collected stays. */
int economy_set_enabled(int on){
  if(g_state != 1) return 0;
  g_menu_en = on ? 1 : 0;
  if(!on){ if(g_cfg){ g_cfg->enable = 0; g_cfg_gen++; } } else load_config(1);
  return 1;
}
