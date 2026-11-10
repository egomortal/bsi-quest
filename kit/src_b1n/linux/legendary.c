/* legendary.c -- ext72: Borderlands-style legendary drops for the BSI-Quest static recompilation.
   First (and so far only) legendary: "The Chosen One".

   WHAT IT DOES (all of it inside the running game: no config file, no adb needed)
   - Watches the praying soldiers of the Monument Island Gateway (S_TWN3_P, sublevel S_TWN_Blockade_C_int_Game: the 16
     XReactiveSkeletalMeshActors whose archetype is Arc_STownCenter_Placeable.Soldier_Pray{Kneeling,Standing}_{A,B}, the
     "Checkpoint_Save_Outside_Prayer_Building" / "Play_VO_ComstockMiraclePrayerShoot" scene). When the LAST of them dies,
     a real weapon pickup is spawned at that soldier's position with the game's own SpawnActor (f_0062ca80, the native of
     Actor.Spawn): template PreCoalescedItemAssets.Pickup_Weapon_HandCannon_Founder_MaxAmmo (XLootContainerKAsset), i.e. the
     same pickup actor the game places for a Hand Cannon, in the soldier's floating section.
   - The pickup (and, while you own it, the gun) is renamed for our item only: the pickup's LootDisplayString, the item
     database entry of the Hand Cannon (GItemDatabase ItemLookupTable[LookupID 0x11a]: ItemName / ItemDescription / Quality)
     and the XWeaponConsumableItem Consumable_Weapon_HandCannon_Founder get "THE CHOSEN ONE" / "Not just the men." (the
     originals come back when the legendary is not in play).
   - An orange Borderlands loot beam stands on the pickup until it is taken: the game thread publishes the pickup's world
     position (its collision component's LocalToWorld), the render thread projects it with every view's own
     ViewProjection (FViewInfo +0x240, read in FDrawSceneCommand::Execute, after the VR late latch) and rhi_b1 draws a few
     clear rectangles into the back buffer right before Present: same picture in flat and in both stereo eyes, no shader.
   - While The Chosen One is the equipped gun (AXWeapon archetype HandCannonFounder and the legendary was picked up):
       gun hits: 12% (cooldown 4 s) a single-target Bucking Bronco lift on the victim ("Force choke": the unused
         IceFleckedBuckingBronco single-target proc with its DamageType pointed at Arc_XDamageTypes.RollingThunderDamage,
         the Bucking Bronco damage type that BroncoStorm also uses);
       melee hits: 100% (cooldown 1.5 s) set the victim on fire ("lightsaber": FistsOfFire, Burning Halo's proc);
       kills: 15% (cooldown 6 s) refill the held gun's ammo (RefillHeldWeaponAmmo).
     Procs are fired through AXGameReplicationInfo::TriggerProcByName with the player as instigator (gear mod method).
   - State (none / dropped / owned) is kept in <dir of $BSI_BLOOM_FILE>/legendary_state.txt (Quest: /sdcard/BSI/). A drop
     happens whenever the last worshipper dies and the player does not already carry a Hand Cannon, so reloading a save from
     before the scene gives the drop again (the world was rolled back too), and a save made after picking it up keeps it
     (the game saves the gun; the state file says it is the legendary).

   INTEGRATION (all weak references; a build without legendary.o behaves exactly as before)
   - patches.c patch_call: 0xaf0480 -> legendary_tick(c) (observe), 0x971fa0 -> legendary_call(c,t) first (it runs the
     weapon perks / gear mod chain or the original itself).
   - rt_linux.c ft_init: legendary_skip(a) keeps 0x971fa0 off the flat call table while the legendary system is on.
   - vr_ui_rhi.c FDrawSceneCommand::Execute thunk: ext72_scene_views(sr) after the late latch. Without vr_ui_rhi.o (non-VR
     APK) this file installs the same vtable-slot thunk itself (slot 2 of vtable 0x1163a94).
   - rhi_b1_dxvk.cpp b1dx_present_inner / b1dx_end_frame: ext72_beam_rects() -> clearImageView rectangles.

   SWITCHES: built-in defaults (ON). Optional override file <dir of $BSI_BLOOM_FILE>/legendary.txt (or $BSI_LEGENDARY),
   lines: enable 0|1, verbose 0|1|2, beam 0|1, lift <chance> <cooldown>, ignite <chance> <cooldown>, refill <chance> <cooldown>,
   group <archetype-prefix> <level-package> (test). Env: BSI_LEGENDARY=0 off, BSI_PATCHES=0 off, BSI_LEGEND_TEST=<list> (cloud
   test mode, see test_*). The mod menu (ext_menu.c) toggles enable/beam live through legendary_set(). Log lines "[legend]". */
#include "hle.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

void call_indirect(CPU *c, uint32_t t);
typedef void (*lg_gfn)(CPU *);
lg_gfn rt_lookup(uint32_t a);

/* ---- guest addresses (BioShockInfinite.exe, Steam build used by the recompiler) ---- */
#define G_GNAMES      0x0139DFECu
#define G_GOBJECTS    0x0138C15Cu
#define G_GENGINE     0x013AA024u
#define G_GWORLD      0x013AC3ECu
#define G_ITEMDB      0x01406B48u   /* UXItemDatabase* */
#define F_DAMAGE      0x00971FA0u   /* AXPawn IXDamagee handler: thiscall(this=pawn+0x39C, Instigator, UDamageType*, DamageEvent*), ret 0xC */
#define F_TRIGGERPROC 0x0092A4E0u   /* AXGameReplicationInfo::TriggerProcByName(FName, Pawn*, Actor*), ret 0x10 */
#define F_SPAWN       0x0062CA80u   /* UWorld spawn (the call in AActor::execSpawn 0x642f6c): thiscall GWorld, 15 dwords, ret 0x3c */
#define F_LOADPKG     0x0050C880u   /* UObject::LoadPackage cdecl (Outer, TCHAR*, flags, 0) */
#define F_DEALDAMAGE  0x007E49A0u   /* WorldInfo thiscall (InstigatorPawn, DamageType, Target, float Scale, HitInfo*[0x100]), ret 0x14 */
#define F_APPMALLOC   0x00482A80u
#define F_APPFREE     0x00482AE0u
#define F_ACQUIRE_W   0x00967EE0u   /* AXPawn::AcquireWeapon(archetype, byte DoNotEquip), thiscall, ret 8 */
#define F_DESTROY     0x0061C690u   /* UWorld::DestroyActor(Actor, bNetForce, bShouldModifyLevel, 0), thiscall GWorld, ret 0x10 */
#define LL_VT         0x01163A94u   /* FDrawSceneCommand vtable (vr_ui_rhi.c vr71) */
#define LL_EXEC       0x0073F640u
#define UR_IMP_BASE   0xE0000000u
#define PAWN_FROM_DAMAGEE 0x39Cu
/* UObject / reflection */
#define UO_OUTER 0x14u
#define UO_NAME  0x18u
#define UO_CLASS 0x20u
#define UO_ARCH  0x24u
#define US_SUPER 0x34u
#define US_CHILDREN 0x38u
#define UF_NEXT 0x28u
#define UP_OFFSET 0x48u
/* gameplay (sdk/*.h; reflection is used where it can) */
#define A_LOC     0x44u     /* AActor.XPrivateLocalLocation */
#define A_ROT     0x50u
#define A_FLAGS   0x5Cu     /* bHidden 4, bDeleteMe 0x20 */
#define A_SECTION 0xBCu     /* AActor.XFloatingSectionIndex */
#define A_COLL    0x13Cu    /* AActor.CollisionComponent */
#define RA_HEALTH 0x4B0u    /* AXReactiveActor.CurrentHealth */
#define RA_LOOTSTR 0x6F8u   /* AXReactiveActor.LootDisplayString */
#define PAWN_INVMGR 0x314u
#define IM_ITEMS 0x1FCu
#define IM_EQ    0x2A0u
#define W_TYPE   0x3E7u
#define DT_FLAGS 0x28u      /* UDamageType: bIsMelee = 8 */
#define IDB_TABLE 0x2Cu     /* UXItemDatabase.ItemLookupTable TArray<FXItemCompositeData> (0x5C each) */
#define IC_STRIDE 0x5Cu
#define IC_QUALITY 0x02u
#define IC_NAME   0x10u
#define IC_DESC   0x28u
#define IT_LOOKUP 0x2Cu     /* UXItem.ItemLookupID (qword) */
#define IT_QUALITY 0x34u
#define IT_NAME   0x38u
#define IT_DESC   0x50u
#define VI_STRIDE 0x11A0u   /* FViewInfo */
#define VI_X      0x34u     /* X, Y, SizeX, SizeY floats */
#define VI_VP     0x240u    /* ViewProjectionMatrix */

#define LOGP "[legend] "
#define MAXM 40

/* ---- the item ---- */
static const char *const LG_NAME = "THE CHOSEN ONE";
static const char *const LG_DESC = "Not just the men.";
static const char *const LG_BASE_ARCH = "HandCannonFounder";                          /* PreCoalescedItemAssets.<>, AXWeapon */
static const char *const LG_PICKUP_ARCH = "Pickup_Weapon_HandCannon_Founder_MaxAmmo"; /* PreCoalescedItemAssets.<>, XLootContainerKAsset */
static const char *const LG_ITEM = "Consumable_Weapon_HandCannon_Founder";           /* PreCoalescedItemAssets.<>, XWeaponConsumableItem */

/* ---- config ---- */
typedef struct { float chance, cooldown; double last; } Proc;
typedef struct {
  int enable, verbose, beam;
  Proc lift, ignite, refill;
  char grp_arch[64], grp_level[96], grp_map[64];
  float beam_height, beam_radius;
} Config;
static Config g_cfg;
static char g_file[512], g_state_file[512];
static time_t g_mtime; static off_t g_fsize; static int g_have_file;
static int g_state = -1, g_hooked;
static char g_test[512];
static uint32_t g_rng = 0x2545F491u;
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float frand(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng >> 8) * (1.0f / 16777216.0f); }
static int test_has(const char *k){ const char *p = strstr(g_test, k); return p && (p == g_test || p[-1] == ',') ; }
static const char *test_val(const char *k){   /* value of "k:<v>" in the test list (rotating buffers: several values can be held at once) */
  static char vb[4][160]; static int vi; char *v = vb[vi++ & 3]; const char *p = strstr(g_test, k); if(!p) return 0; p += strlen(k); if(*p != ':' && *p != '=') return 0; p++;
  int n = 0; while(p[n] && p[n] != ',' && n < 159){ v[n] = p[n]; n++; } v[n] = 0; return v; }

static void cfg_defaults(Config *c){
  memset(c, 0, sizeof *c);
  c->enable = 1; c->verbose = 1; c->beam = 1;
  c->lift.chance = 0.12f; c->lift.cooldown = 4.0f;
  c->ignite.chance = 1.0f; c->ignite.cooldown = 1.5f;
  c->refill.chance = 0.15f; c->refill.cooldown = 6.0f;
  snprintf(c->grp_arch, sizeof c->grp_arch, "Soldier_Pray");
  snprintf(c->grp_level, sizeof c->grp_level, "S_TWN_Blockade_C_int_Game");
  snprintf(c->grp_map, sizeof c->grp_map, "S_TWN3_P");
  c->beam_height = 2600.f; c->beam_radius = 8.f;
}
static void parse_config(Config *c, FILE *f){
  char ln[256];
  while(fgets(ln, sizeof ln, f)){
    char *h = strchr(ln, '#'); if(h) *h = 0;
    char k[32], a[96], b[96]; int n = sscanf(ln, "%31s %95s %95s", k, a, b); if(n < 2) continue;
    if(!strcmp(k, "enable")) c->enable = atoi(a);
    else if(!strcmp(k, "verbose")) c->verbose = atoi(a);
    else if(!strcmp(k, "beam")) c->beam = atoi(a);
    else if(!strcmp(k, "beam_height")) c->beam_height = (float)atof(a);
    else if(!strcmp(k, "lift") && n > 2){ c->lift.chance = (float)atof(a); c->lift.cooldown = (float)atof(b); }
    else if(!strcmp(k, "ignite") && n > 2){ c->ignite.chance = (float)atof(a); c->ignite.cooldown = (float)atof(b); }
    else if(!strcmp(k, "refill") && n > 2){ c->refill.chance = (float)atof(a); c->refill.cooldown = (float)atof(b); }
    else if(!strcmp(k, "group") && n > 2){ snprintf(c->grp_arch, sizeof c->grp_arch, "%s", a); snprintf(c->grp_level, sizeof c->grp_level, "%s", b); }
    else if(!strcmp(k, "group_map")) snprintf(c->grp_map, sizeof c->grp_map, "%s", a);
    else fprintf(stderr, LOGP "config: unknown line '%s'\n", k);
  }
}
static void find_paths(void){
  const char *e = getenv("BSI_LEGENDARY_FILE"), *b = getenv("BSI_BLOOM_FILE"); char dir[400] = "/sdcard/BSI";
  if(b && *b){ const char *sl = strrchr(b, '/'); if(sl) snprintf(dir, sizeof dir, "%.*s", (int)(sl - b), b); }
  if(e && *e) snprintf(g_file, sizeof g_file, "%s", e); else snprintf(g_file, sizeof g_file, "%s/legendary.txt", dir);
  const char *s = getenv("BSI_LEGENDARY_STATE");
  if(s && *s) snprintf(g_state_file, sizeof g_state_file, "%s", s); else snprintf(g_state_file, sizeof g_state_file, "%s/legendary_state.txt", dir);
}
static int load_config(int force){   /* the file only overrides the built-in defaults */
  struct stat st;
  if(stat(g_file, &st) != 0){ if(g_have_file){ g_have_file = 0; int en = g_cfg.enable, bm = g_cfg.beam; cfg_defaults(&g_cfg); g_cfg.enable = en; g_cfg.beam = bm;
      fprintf(stderr, LOGP "%s removed: built-in defaults\n", g_file); } return 0; }
  if(!force && g_have_file && st.st_mtime == g_mtime && st.st_size == g_fsize) return 0;
  FILE *f = fopen(g_file, "r"); if(!f) return 0;
  Config c; cfg_defaults(&c); parse_config(&c, f); fclose(f);
  c.lift.last = g_cfg.lift.last; c.ignite.last = g_cfg.ignite.last; c.refill.last = g_cfg.refill.last;
  g_cfg = c; g_mtime = st.st_mtime; g_fsize = st.st_size; g_have_file = 1;
  fprintf(stderr, LOGP "override file %s: enable %d beam %d lift %.2f/%.1fs ignite %.2f/%.1fs refill %.2f/%.1fs group %s in %s\n", g_file, c.enable, c.beam,
          c.lift.chance, c.lift.cooldown, c.ignite.chance, c.ignite.cooldown, c.refill.chance, c.refill.cooldown, c.grp_arch, c.grp_level);
  return 1;
}

/* ---- persistent state ---- */
enum { ST_NONE, ST_DROPPED, ST_OWNED };
static const char *const st_name[] = { "none", "dropped", "owned" };
static int g_lstate; static long g_lstate_t;
static void state_load(void){
  FILE *f = fopen(g_state_file, "r"); if(!f){ fprintf(stderr, LOGP "no state file at %s (The Chosen One not found yet)\n", g_state_file); return; }
  char ln[200];
  while(fgets(ln, sizeof ln, f)){ char k[32], s[16]; long t = 0;
    if(sscanf(ln, "%31s %15s %ld", k, s, &t) >= 2 && !strcmp(k, "chosen_one")) for(int i = 0; i < 3; i++) if(!strcmp(s, st_name[i])){ g_lstate = i; g_lstate_t = t; } }
  fclose(f);
  fprintf(stderr, LOGP "state %s: The Chosen One %s\n", g_state_file, st_name[g_lstate]);
}
static void state_set(int s){
  if(s == g_lstate) return;
  g_lstate = s; g_lstate_t = (long)time(0);
  char tmp[520]; snprintf(tmp, sizeof tmp, "%s.tmp", g_state_file);
  FILE *f = fopen(tmp, "w"); if(!f){ fprintf(stderr, LOGP "cannot write %s\n", tmp); return; }
  fprintf(f, "# legendary state v1 (ext72): <item> none|dropped|owned <unix time>. Delete to forget.\nchosen_one %s %ld\n", st_name[s], g_lstate_t);
  fclose(f); rename(tmp, g_state_file);
  fprintf(stderr, LOGP "state -> %s (saved to %s)\n", st_name[s], g_state_file);
}

/* ---- host-side reflection (reads only; same approach as gear_mod.c) ---- */
static int gptr(uint32_t p){ return p >= 0x10000u && p < 0xFFFF0000u && !(p & 3); }
static const char *fname_str(uint32_t idx){
  uint32_t gn = R32(G_GNAMES), n = R32(G_GNAMES + 4);
  if(!gptr(gn) || idx >= n) return "?";
  uint32_t e = R32(gn + 4 * idx); if(e < 0x10000u || e >= 0xFFFF0000u) return "?";
  return (const char *)MEMP(e + 0x10);
}
static int fname_find(const char *s){
  static struct { char s[64]; int idx; uint32_t n; } cache[48]; static int nc;
  uint32_t n = R32(G_GNAMES + 4);
  for(int i = 0; i < nc; i++) if(!strcmp(cache[i].s, s) && (cache[i].idx >= 0 || cache[i].n == n)) return cache[i].idx;
  uint32_t gn = R32(G_GNAMES); int found = -1;
  if(gptr(gn)) for(uint32_t i = 0; i < n; i++){ uint32_t e = R32(gn + 4 * i); if(e >= 0x10000u && e < 0xFFFF0000u && !strcasecmp((const char *)MEMP(e + 0x10), s)){ found = (int)i; break; } }
  int slot = -1; for(int i = 0; i < nc; i++) if(!strcmp(cache[i].s, s)) slot = i;
  if(slot < 0 && nc < 48) slot = nc++;
  if(slot >= 0){ snprintf(cache[slot].s, 64, "%s", s); cache[slot].idx = found; cache[slot].n = n; }
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
  static struct { uint32_t cls; char n[40]; uint32_t p; } cache[64]; static unsigned nc;
  for(unsigned i = 0; i < nc && i < 64; i++) if(cache[i].cls == cls && !strcmp(cache[i].n, name)) return cache[i].p;
  uint32_t found = 0;
  for(uint32_t c = cls, k = 0; gptr(c) && k < 40 && !found; c = R32(c + US_SUPER), k++)
    for(uint32_t f = R32(c + US_CHILDREN), m = 0; gptr(f) && m < 8000; f = R32(f + UF_NEXT), m++)
      if(!strcasecmp(oname(f), name) && strstr(cname(f), "Property")){ found = f; break; }
  unsigned slot = nc++ & 63; cache[slot].cls = cls; snprintf(cache[slot].n, 40, "%s", name); cache[slot].p = found;
  return found;
}
static int prop_off(uint32_t obj, const char *name, int dflt){
  uint32_t p = gptr(obj) ? find_prop(R32(obj + UO_CLASS), name) : 0;
  return p ? (int)R32(p + UP_OFFSET) : dflt;
}
static const char *top_name(uint32_t o){ if(!gptr(o)) return "-"; for(int k = 0; k < 8 && gptr(R32(o + UO_OUTER)); k++) o = R32(o + UO_OUTER); return oname(o); }
static uint32_t world_info(void){
  uint32_t w = R32(G_GWORLD); if(!gptr(w)) return 0;
  uint32_t lvl = R32(w + 0x3C); if(!gptr(lvl)) return 0;
  uint32_t act = R32(lvl + 0x28); if(!gptr(act)) return 0;
  uint32_t wi = R32(act); return gptr(wi) ? wi : 0;
}
static uint32_t game_gri(void){
  uint32_t wi = world_info(); if(!wi) return 0;
  uint32_t g = R32(wi + 0x40C), gri = gptr(g) ? R32(g + 0x2C0) : 0;
  if(gptr(gri) && is_a(gri, "XGameReplicationInfo")) return gri;
  gri = R32(wi + 0x3E0); return gptr(gri) && is_a(gri, "XGameReplicationInfo") ? gri : 0;
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
static uint32_t as_pawn(uint32_t a){
  if(!gptr(a)) return 0;
  if(is_a(a, "Pawn")) return a;
  if(is_a(a, "Controller")){ uint32_t p = R32(a + prop_off(a, "Pawn", 0x1FC)); return gptr(p) ? p : 0; }
  return 0;
}
static float pawn_health(uint32_t p){ return RF32(p + prop_off(p, "Health", 0x2AC)); }
static float ra_health(uint32_t a){ return RF32(a + prop_off(a, "CurrentHealth", RA_HEALTH)); }
static int deleted(uint32_t a){ return !gptr(a) || (R32(a + A_FLAGS) & 0x20); }
/* object in PreCoalescedItemAssets (or anywhere when outer is 0) by name + class; cached, revalidated */
static uint32_t find_obj(const char *name, const char *outer, const char *cls){
  static struct { char n[64]; uint32_t o; } cache[24]; static int nc;
  for(int i = 0; i < nc; i++) if(!strcmp(cache[i].n, name)){ uint32_t o = cache[i].o; if(gptr(o) && !strcmp(oname(o), name) && is_a(o, cls)) return o; }
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
  for(uint32_t i = 0; i < n && gptr(go); i++){
    uint32_t o = R32(go + 4 * i); if(!gptr(o)) continue;
    if(strcmp(oname(o), name) || (outer && strcmp(oname(R32(o + UO_OUTER)), outer)) || !is_a(o, cls)) continue;
    int s = -1; for(int k = 0; k < nc; k++) if(!strcmp(cache[k].n, name)) s = k;
    if(s < 0 && nc < 24) s = nc++;
    if(s >= 0){ snprintf(cache[s].n, 64, "%s", name); cache[s].o = o; }
    return o;
  }
  return 0;
}
/* world position the renderer uses: the collision/mesh component's LocalToWorld translation (handles floating sections) */
static int actor_world_pos(uint32_t a, float v[3]){
  if(!gptr(a)) return 0;
  uint32_t cc = R32(a + prop_off(a, "CollisionComponent", A_COLL));
  if(gptr(cc)){ int o = prop_off(cc, "LocalToWorld", -1);
    if(o > 0){ float x = RF32(cc + o + 48), y = RF32(cc + o + 52), z = RF32(cc + o + 56), w = RF32(cc + o + 60);
      if(w == 1.f && isfinite(x) && isfinite(y) && isfinite(z) && (x != 0.f || y != 0.f || z != 0.f)){ v[0] = x; v[1] = y; v[2] = z; return 2; } } }
  v[0] = RF32(a + A_LOC); v[1] = RF32(a + A_LOC + 4); v[2] = RF32(a + A_LOC + 8);
  return 1;
}

/* ---- guest calls ---- */
static uint32_t game_malloc(CPU *c, uint32_t n){ CPU save = *c; PUSH32(c, 8); PUSH32(c, n); guest_call(c, F_APPMALLOC); uint32_t p = c->eax; *c = save; return p; }
static void game_free(CPU *c, uint32_t p){ CPU save = *c; PUSH32(c, p); guest_call(c, F_APPFREE); *c = save; }
static void get_fstring(uint32_t f, char *out, int max){ uint32_t d = R32(f), n = R32(f + 4); if(!gptr(d) || !n){ out[0] = 0; return; } g2host_w(d, out, max < (int)n + 1 ? max : (int)n + 1); }
static void put_fstring(CPU *c, uint32_t f, const char *s){   /* in place when it fits, else the game's allocator */
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
static int g_pkg_tries;
static void load_items_package(CPU *c){
  if(g_pkg_tries >= 2) return; g_pkg_tries++;
  uint32_t n0 = R32(G_GOBJECTS + 4), n = 14, fb = game_malloc(c, 2 * (n + 1)); if(!gptr(fb)) return;
  const char *s = "CoalescedItems"; for(uint32_t i = 0; i <= n; i++) W16(fb + 2 * i, i < n ? (uint8_t)s[i] : 0);
  double t = now_s();
  CPU save = *c; PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, fb); PUSH32(c, 0); guest_call(c, F_LOADPKG); uint32_t pk = c->eax; *c = save;
  game_free(c, fb);
  fprintf(stderr, LOGP "LoadPackage(CoalescedItems) -> %08x in %.0f ms, objects %u -> %u\n", pk, (now_s() - t) * 1000, n0, R32(G_GOBJECTS + 4));
}
static unsigned g_procs_fired, g_procs_failed;
static int g_firing;
static int fire_proc(CPU *c, const char *proc, uint32_t inst, uint32_t victim, const char *why){
  uint32_t gri = game_gri(); int idx = fname_find(proc);
  if(!gri || idx < 0 || !gptr(inst) || !gptr(victim)){ g_procs_failed++; fprintf(stderr, LOGP "proc %s (%s): cannot fire (gri %08x name %d)\n", proc, why, gri, idx); return 0; }
  CPU save = *c;
  PUSH32(c, victim); PUSH32(c, inst); PUSH32(c, 0); PUSH32(c, (uint32_t)idx); c->ecx = gri;
  g_firing++; guest_call(c, F_TRIGGERPROC); g_firing--;
  uint32_t ok = c->eax; *c = save;
  g_procs_fired++;
  fprintf(stderr, LOGP "PROC %s (%s): %08x -> %08x (%s) %s\n", proc, why, inst, victim, cname(victim), ok ? "triggered" : "not triggered");
  return (int)ok;
}
static uint32_t spawn_actor(CPU *c, uint32_t cls, uint32_t tmpl, const float loc[3], const int32_t rot[3], int section, uint32_t instigator){
  uint32_t w = R32(G_GWORLD); if(!gptr(w) || !gptr(cls)) return 0;
  CPU save = *c;
  c->esp = (c->esp - 0x40) & ~15u; uint32_t vl = c->esp, vr = c->esp + 0x10;
  WF32(vl, loc[0]); WF32(vl + 4, loc[1]); WF32(vl + 8, loc[2]); W32(vr, (uint32_t)rot[0]); W32(vr + 4, (uint32_t)rot[1]); W32(vr + 8, (uint32_t)rot[2]);
  PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, (uint32_t)section); PUSH32(c, 0); PUSH32(c, 0);
  PUSH32(c, instigator); PUSH32(c, 0 /* owner */); PUSH32(c, 0 /* bRemoteOwned */); PUSH32(c, 1 /* bNoCollisionFail */); PUSH32(c, tmpl);
  PUSH32(c, vr); PUSH32(c, vl); PUSH32(c, 0); PUSH32(c, 0 /* FName None */); PUSH32(c, cls);
  c->ecx = w; guest_call(c, F_SPAWN);
  uint32_t a = c->eax; *c = save;
  return gptr(a) ? a : 0;
}
static void deal_damage(CPU *c, uint32_t inst, uint32_t dt, uint32_t target, float scale){
  uint32_t wi = world_info(); if(!wi || !gptr(dt) || !gptr(target) || !gptr(inst)) return;
  CPU save = *c;
  c->esp = (c->esp - 0x100) & ~15u; uint32_t hit = c->esp;
  for(uint32_t i = 0; i < 0x100; i += 4) W32(hit + i, 0);
  float s = scale; uint32_t su; memcpy(&su, &s, 4);
  PUSH32(c, hit); PUSH32(c, su); PUSH32(c, target); PUSH32(c, dt); PUSH32(c, inst); c->ecx = wi;
  guest_call(c, F_DEALDAMAGE);
  *c = save;
}

/* ---- names: our item only while the legendary is in play ---- */
static int g_names_on; static uint32_t g_names_db, g_names_item; static int g_names_idx = -1;
static char g_orig_dbname[80], g_orig_dbdesc[200], g_orig_itname[80], g_orig_itdesc[200]; static uint8_t g_orig_dbq, g_orig_itq;
static uint32_t lg_item(void){ return find_obj(LG_ITEM, "PreCoalescedItemAssets", "XItem"); }
static int db_index(uint32_t db, uint32_t lookup){
  uint32_t data = R32(db + IDB_TABLE), n = R32(db + IDB_TABLE + 4);
  if(!gptr(data) || n > 4096 || lookup >= n) return -1;
  return (int)lookup;   /* ItemLookupTable is indexed by the low 32 bits of ItemLookupID (ItemLookupTable[282] = HAND CANNON in GlobalXItemDatabase.INT) */
}
static void names_apply(CPU *c, int on, const char *why){
  if(on == g_names_on) return;
  uint32_t db = R32(G_ITEMDB), it = lg_item();
  if(on){
    if(!gptr(db) || !gptr(it)){ static int nl; if(nl++ < 3) fprintf(stderr, LOGP "names: item database %08x or item %s %08x not loaded yet\n", db, LG_ITEM, it); return; }
    int idx = db_index(db, R32(it + IT_LOOKUP)); if(idx < 0){ fprintf(stderr, LOGP "names: lookup id %u out of the item table\n", R32(it + IT_LOOKUP)); return; }
    uint32_t e = R32(db + IDB_TABLE) + IC_STRIDE * (uint32_t)idx;
    get_fstring(e + IC_NAME, g_orig_dbname, sizeof g_orig_dbname); get_fstring(e + IC_DESC, g_orig_dbdesc, sizeof g_orig_dbdesc); g_orig_dbq = R8(e + IC_QUALITY);
    get_fstring(it + IT_NAME, g_orig_itname, sizeof g_orig_itname); get_fstring(it + IT_DESC, g_orig_itdesc, sizeof g_orig_itdesc); g_orig_itq = R8(it + IT_QUALITY);
    put_fstring(c, e + IC_NAME, LG_NAME); put_fstring(c, e + IC_DESC, LG_DESC); W8(e + IC_QUALITY, 5 /* IQ_Epic, the highest */);
    put_fstring(c, it + IT_NAME, LG_NAME); put_fstring(c, it + IT_DESC, LG_DESC); W8(it + IT_QUALITY, 5);
    g_names_db = db; g_names_item = it; g_names_idx = idx; g_names_on = 1;
    fprintf(stderr, LOGP "names (%s): item database [%d] \"%s\" -> \"%s\", %s \"%s\" -> \"%s\" (description \"%s\")\n", why, idx, g_orig_dbname, LG_NAME, LG_ITEM, g_orig_itname, LG_NAME, LG_DESC);
  } else {
    if(gptr(g_names_db) && R32(G_ITEMDB) == g_names_db && g_names_idx >= 0){ uint32_t e = R32(g_names_db + IDB_TABLE) + IC_STRIDE * (uint32_t)g_names_idx;
      put_fstring(c, e + IC_NAME, g_orig_dbname); put_fstring(c, e + IC_DESC, g_orig_dbdesc); W8(e + IC_QUALITY, g_orig_dbq); }
    if(gptr(g_names_item) && g_names_item == lg_item()){ put_fstring(c, g_names_item + IT_NAME, g_orig_itname); put_fstring(c, g_names_item + IT_DESC, g_orig_itdesc); W8(g_names_item + IT_QUALITY, g_orig_itq); }
    g_names_on = 0;
    fprintf(stderr, LOGP "names (%s): \"%s\" restored\n", why, g_orig_dbname);
  }
}

/* ---- the Force choke proc: IceFleckedBuckingBronco (single target, unused) with the Bucking Bronco damage type ---- */
static uint32_t g_tuned_gri;
static int tune_lift(void){
  uint32_t rt = find_obj("RollingThunderDamage", 0, "DamageType"); if(!rt) return 0;
  int idx = fname_find("IceFleckedBuckingBronco"); if(idx < 0) return 0;
  int n = 0;
  uint32_t gri = game_gri();
  if(gri){ uint32_t data = R32(gri + 0x374), cnt = R32(gri + 0x378);
    for(uint32_t i = 0; gptr(data) && i < cnt && i < 4096; i++){ uint32_t pr = R32(data + 4 * i); if(!gptr(pr) || R32(pr + 0x28) != (uint32_t)idx) continue;
      int o = prop_off(pr, "DamageType", -1); if(o < 0) continue;
      if(R32(pr + o) != rt){ fprintf(stderr, LOGP "tune: %s (%s).DamageType %s -> %s (single-target Bucking Bronco lift)\n", oname(pr), cname(pr), oname(R32(pr + o)), oname(rt)); W32(pr + o, rt); }
      n++; } }
  return n;
}

/* ---- the worshippers ---- */
typedef struct { uint32_t a; int alive, dead, hidden; float hp; float pos[3]; float lpos[3]; int section; int32_t yaw; char nm[48]; } Member;
static Member g_m[MAXM]; static int g_nm; static uint32_t g_grp_world; static double g_next_scan;
static int g_total_seen, g_total_dead, g_dropped_here, g_scan_added;
static uint32_t g_pickup; static float g_pickup_pos[3]; static double g_pickup_t;
static int member_match(uint32_t o){
  if(!is_a(o, "XReactiveSkeletalMeshActor")) return 0;
  uint32_t ar = R32(o + UO_ARCH); if(!gptr(ar)) return 0;
  const char *an = oname(ar); if(strncmp(an, g_cfg.grp_arch, strlen(g_cfg.grp_arch))) return 0;
  if(g_cfg.grp_level[0] && strcmp(g_cfg.grp_level, "*") && strcmp(top_name(o), g_cfg.grp_level)) return 0;
  return 1;
}
/* incremental: at most SCAN_STEP objects per call (every 0.2 s), so the game thread never pays for a full GObjects walk in one
   frame (about 300k objects; a full walk would be a 10-30 ms hitch on the Quest) */
#define SCAN_STEP 20000u
static uint32_t g_scan_pos;
static int scan_group(void){   /* returns 1 when a full pass has completed */
  uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4); int added = 0, done = 0;
  uint32_t i0 = g_scan_pos < n ? g_scan_pos : 0, i1 = i0 + SCAN_STEP; if(i1 >= n){ i1 = n; done = 1; }
  g_scan_pos = done ? 0 : i1;
  for(uint32_t i = i0; i < i1 && gptr(go); i++){
    uint32_t o = R32(go + 4 * i); if(!gptr(o) || deleted(o)) continue;
    const char *k = cname(o); if(k[0] != 'X' || !strstr(k, "Actor")) continue;
    if(!member_match(o)) continue;
    int have = 0; for(int j = 0; j < g_nm; j++) if(g_m[j].a == o) have = 1;
    if(have || g_nm >= MAXM) continue;
    Member *m = &g_m[g_nm++]; memset(m, 0, sizeof *m); m->a = o; m->hp = ra_health(o); m->alive = m->hp > 0; m->dead = !m->alive;
    snprintf(m->nm, sizeof m->nm, "%s", oname(o)); m->hidden = (R32(o + A_FLAGS) & 4) != 0;
    actor_world_pos(o, m->pos); added++;
    if(g_cfg.verbose > 1 || g_nm <= 2) fprintf(stderr, LOGP "worshipper %s (%s, archetype %s, level %s): health %.0f%s at (%.0f %.0f %.0f) section %d\n", m->nm, cname(o),
            oname(R32(o + UO_ARCH)), top_name(o), m->hp, m->hidden ? " hidden" : "", m->pos[0], m->pos[1], m->pos[2], (int)R32(o + A_SECTION));
  }
  if(added){ g_total_seen += added; g_scan_added += added; }
  if(done && g_scan_added){ int al = 0, hid = 0; for(int j = 0; j < g_nm; j++){ al += g_m[j].alive; hid += g_m[j].hidden; }
    fprintf(stderr, LOGP "worshipper group (%s in %s): %d found, %d alive, %d hidden\n", g_cfg.grp_arch, g_cfg.grp_level, g_nm, al, hid); g_scan_added = 0; }
  return done;
}
static int carries_base(uint32_t pawn){
  uint32_t im = gptr(pawn) ? R32(pawn + PAWN_INVMGR) : 0; if(!gptr(im)) return 0;
  for(int i = 0; i < 36; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(gptr(it) && is_a(it, "XWeapon") && !strcmp(oname(R32(it + UO_ARCH)), LG_BASE_ARCH)) return 1; }
  return 0;
}
static uint32_t held_gun(uint32_t pawn){
  uint32_t im = gptr(pawn) ? R32(pawn + PAWN_INVMGR) : 0; if(!gptr(im)) return 0;
  int eq = (int)R32(im + IM_EQ); if(eq < 0 || eq >= 36) return 0;
  uint32_t it = R32(im + IM_ITEMS + 4 * eq); return gptr(it) && is_a(it, "XWeapon") ? it : 0;
}
static int holding_legendary(uint32_t pawn){
  uint32_t w = held_gun(pawn);
  return w && g_lstate == ST_OWNED && !strcmp(oname(R32(w + UO_ARCH)), LG_BASE_ARCH);
}

/* ---- the drop ---- */
static void drop_legendary(CPU *c, const Member *m, const char *why){
  uint32_t pawn = player_pawn();
  if(carries_base(pawn)){ fprintf(stderr, LOGP "drop (%s): the player already carries a %s: no second legendary\n", why, LG_BASE_ARCH); return; }
  uint32_t tmpl = find_obj(LG_PICKUP_ARCH, "PreCoalescedItemAssets", "XReactiveActor");
  if(!tmpl){ load_items_package(c); tmpl = find_obj(LG_PICKUP_ARCH, "PreCoalescedItemAssets", "XReactiveActor"); }
  if(!tmpl){ fprintf(stderr, LOGP "drop (%s): pickup archetype %s not loaded: no drop\n", why, LG_PICKUP_ARCH); return; }
  float loc[3] = { m->lpos[0], m->lpos[1], m->lpos[2] + 60.f }; int32_t rot[3] = { 0, m->yaw, 0 };
  uint32_t a = spawn_actor(c, R32(tmpl + UO_CLASS), tmpl, loc, rot, m->section, pawn);
  if(!a){ fprintf(stderr, LOGP "drop (%s): SpawnActor(%s, template %s) failed at local (%.0f %.0f %.0f) section %d\n", why, cname(tmpl), oname(tmpl), loc[0], loc[1], loc[2], m->section); return; }
  /* a spawned pickup has not rolled its loot (placed ones roll when their level loads): roll it now with the game's own
     AXReactiveActor loot roll (f_00bc7bb0, thiscall, arg = LootSearchType == 0, ret 4; what f_00bc9090 does for level actors) */
  { int ls = prop_off(a, "CurrentLootableState", 0x47D), lt = prop_off(a, "LootSearchType", 0x47E), ll = prop_off(a, "LootList", 0x624);
    uint8_t st0 = R8(a + ls);
    if(st0 == 0 && R8(0x00BC7BB0u) == 0x6A && R8(0x00BC7BB2u) == 0x68){ CPU save = *c; PUSH32(c, R8(a + lt) == 0 ? 1u : 0u); c->ecx = a; guest_call(c, 0x00BC7BB0u); *c = save; }
    fprintf(stderr, LOGP "drop: loot state %u -> %u, LootList[0] item %u x%d, [1] item %u x%d\n", st0, R8(a + ls), R32(a + ll), (int)R32(a + ll + 8), R32(a + ll + 0x28), (int)R32(a + ll + 0x30));
    /* the roll validates the weapon against the player (a Hand Cannon the player has not met yet comes out as its ammo, item 487,
       which cannot be taken without the gun: no prompt). Our drop must be the gun itself: LootList[0] = the weapon consumable item
       (Consumable_Weapon_HandCannon_Founder, its ItemLookupID), the rolled ammo moves to LootList[1]; the cached prompt data
       (CachedLootCompositeData[0..1].bIsCached) and the item's cached-lookup bits are cleared so the game rebuilds them. */
    uint32_t it = lg_item(); uint32_t want = it ? R32(it + IT_LOOKUP) : 282u;
    if(R32(a + ll) != want){
      if(R32(a + ll + 0x28) == want && R32(a + ll + 0x2C) == 0){   /* the roll put the gun second: swap, the ammo stays as [1] */
        uint8_t tmp[0x28]; memcpy(tmp, MEMP(a + ll), 0x28); memcpy(MEMP(a + ll), MEMP(a + ll + 0x28), 0x28); memcpy(MEMP(a + ll + 0x28), tmp, 0x28); }
      else if(R32(a + ll + 0x28) == 0 && R32(a + ll + 0x2C) == 0){ memcpy(MEMP(a + ll + 0x28), MEMP(a + ll), 0x28); }
      W32(a + ll + 0x28 + 0x18, R32(a + ll + 0x28 + 0x18) & ~0x60u);
      W32(a + ll, want); W32(a + ll + 4, 0); W32(a + ll + 8, 1); W32(a + ll + 0x18, R32(a + ll + 0x18) & ~0x60u);
      int cc = prop_off(a, "CachedLootCompositeData", 0x7B0); W32(a + cc, R32(a + cc) & ~1u); W32(a + cc + 0x60, R32(a + cc + 0x60) & ~1u);
      fprintf(stderr, LOGP "drop: loot set to the gun: LootList[0] item %u x1, [1] item %u x%d (prompt cache cleared)\n", R32(a + ll), R32(a + ll + 0x28), (int)R32(a + ll + 0x30));
    } }
  put_fstring(c, a + prop_off(a, "LootDisplayString", RA_LOOTSTR), LG_NAME);
  char ls[64]; get_fstring(a + prop_off(a, "LootDisplayString", RA_LOOTSTR), ls, sizeof ls);
  g_pickup = a; g_pickup_t = now_s(); actor_world_pos(a, g_pickup_pos); g_dropped_here = 1;
  state_set(ST_DROPPED);
  names_apply(c, 1, "drop");
  fprintf(stderr, LOGP "DROP (%s): \"%s\" = %s %08x (template %s) at world (%.0f %.0f %.0f), section %d, prompt \"%s\"\n", why, LG_NAME, cname(a), a, oname(tmpl),
          g_pickup_pos[0], g_pickup_pos[1], g_pickup_pos[2], m->section, ls);
}

/* ---- the pickup interaction ----
   The game's own use path does not take a spawned loot container in the cloud harness (no use prompt appears and
   ServerAcceptLootFromContainer gives nothing; see README section 11). So the pickup is also taken by the mod: when the player
   presses use (F, or holds X / Touch X 0.25 s) within 170 UU of the pickup and looks at it (30 degrees), the gun is given with
   the game's AcquireWeapon (equipped; the old gun is dropped by the game unless the weapon wheel keeps it), its clip filled and
   the rolled ammo put in reserve, and the pickup actor is destroyed with UWorld::DestroyActor. While in range a prompt
   "TAKE THE CHOSEN ONE" / "Not just the men." is drawn (ext_menu.c). If the game's own take works (Quest), it wins: the pickup
   disappears and the gun is carried, which the tick sees the same way. */
void ext_prompt_set(const char *l1, const char *l2) __attribute__((weak));
static volatile double g_use_t;
void legendary_use_pressed(void){ g_use_t = now_s(); }
static int pickup_in_reach(uint32_t pawn, float *dist){
  if(!gptr(pawn) || !gptr(g_pickup)) return 0;
  float a[3], b[3];
  if(R32(pawn + A_SECTION) == R32(g_pickup + A_SECTION)){   /* same floating section: compare Locations (a component's LocalToWorld can lag a moved pawn) */
    for(int k = 0; k < 3; k++){ a[k] = RF32(pawn + A_LOC + 4 * k); b[k] = RF32(g_pickup + A_LOC + 4 * k); } }
  else { actor_world_pos(pawn, a); actor_world_pos(g_pickup, b); }
  float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], d = sqrtf(dx * dx + dy * dy + dz * dz); *dist = d;
  if(d > 170.f) return 0;
  uint32_t pc = player_pc(); if(!pc) return 1;
  if(R32(pawn + A_SECTION) != R32(g_pickup + A_SECTION)) return 1;   /* different floating sections: distance only */
  float la[3] = { RF32(pawn + A_LOC), RF32(pawn + A_LOC + 4), RF32(pawn + A_LOC + 8) + 60.f }, lb[3] = { RF32(g_pickup + A_LOC), RF32(g_pickup + A_LOC + 4), RF32(g_pickup + A_LOC + 8) };
  float ex = lb[0] - la[0], ey = lb[1] - la[1], ez = lb[2] - la[2], el = sqrtf(ex * ex + ey * ey + ez * ez); if(el < 1.f) return 1;
  float pi = (float)M_PI / 32768.f, yaw = (float)(int32_t)R32(pc + A_ROT + 4) * pi, pitch = (float)(int16_t)(R32(pc + A_ROT) & 0xFFFF) * pi;
  float fx = cosf(pitch) * cosf(yaw), fy = cosf(pitch) * sinf(yaw), fz = sinf(pitch);
  return (fx * ex + fy * ey + fz * ez) / el > 0.866f || d < 90.f;
}
static uint32_t g_take_w; static int g_take_clip, g_take_spare; static double g_take_t;
static void take_legendary(CPU *c, uint32_t pawn, const char *why){
  uint32_t arch = find_obj(LG_BASE_ARCH, "PreCoalescedItemAssets", "XWeapon");
  if(!arch){ load_items_package(c); arch = find_obj(LG_BASE_ARCH, "PreCoalescedItemAssets", "XWeapon"); }
  if(!arch){ fprintf(stderr, LOGP "take (%s): %s not loaded\n", why, LG_BASE_ARCH); return; }
  int ll = prop_off(g_pickup, "LootList", 0x624); int ammo = 0;
  for(int k = 0; k < 5; k++){ uint32_t id = R32(g_pickup + ll + 0x28 * k); if(id && id != 282u) ammo += (int)R32(g_pickup + ll + 0x28 * k + 8); }
  CPU save = *c; PUSH32(c, 0); PUSH32(c, arch); c->ecx = pawn; guest_call(c, F_ACQUIRE_W); *c = save;
  uint32_t im = R32(pawn + PAWN_INVMGR), w = 0;
  for(int i = 0; gptr(im) && i < 36; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(gptr(it) && is_a(it, "XWeapon") && R32(it + UO_ARCH) == arch) w = it; }
  if(!w){ fprintf(stderr, LOGP "take (%s): AcquireWeapon gave no %s (weapons locked by a scene?)\n", why, LG_BASE_ARCH); return; }
  int mo = prop_off(w, "MaxAmmoCountAttrib", -1); float mc = mo > 0 ? RF32(w + mo + 0xC) : 0.f; if(!(mc >= 1.f && mc < 100.f)) mc = mo > 0 ? RF32(w + mo + 8) : 6.f; if(!(mc >= 1.f && mc < 100.f)) mc = 6.f;
  W32(w + 0x7C4, (uint32_t)(int)mc); if(ammo > 0) W32(w + 0x7EC, R32(w + 0x7EC) + (uint32_t)ammo);
  g_take_w = w; g_take_clip = (int)mc; g_take_spare = (int)R32(w + 0x7EC); g_take_t = now_s();   /* the equip that follows can reset the new instance's ammo: re-applied once (tick) */
  uint32_t pk = g_pickup;
  W32(pk + A_FLAGS, R32(pk + A_FLAGS) & ~0x10u);   /* the template carries bNoDelete (placed pickups): DestroyActor refuses such actors */
  CPU s2 = *c; PUSH32(c, 0); PUSH32(c, 1); PUSH32(c, 0); PUSH32(c, pk); c->ecx = R32(G_GWORLD); guest_call(c, F_DESTROY); *c = s2;
  fprintf(stderr, LOGP "TAKE (%s): %s %08x given (ammo %d/%d), pickup %08x %s\n", why, LG_BASE_ARCH, w, (int)R32(w + 0x7C4), (int)R32(w + 0x7EC), pk, deleted(pk) ? "destroyed" : "still there");
}

/* ---- beam: game thread publishes, render thread projects (seqlock) ---- */
static volatile uint32_t g_bseq; static float g_bpos[3]; static volatile int g_bactive; static float g_bh, g_br;
static void beam_publish(int on, const float p[3]){
  g_bseq++; __atomic_thread_fence(__ATOMIC_RELEASE);
  if(on){ g_bpos[0] = p[0]; g_bpos[1] = p[1]; g_bpos[2] = p[2]; g_bh = g_cfg.beam_height; g_br = g_cfg.beam_radius; }
  g_bactive = on;
  __atomic_thread_fence(__ATOMIC_RELEASE); g_bseq++;
}
/* render thread: rectangles of this frame (x, y, w, h + colour) */
#define MAXR 96
static int32_t g_r[MAXR][4]; static float g_rc[MAXR][3]; static volatile int g_nr; static volatile int g_scene_seen; static unsigned long g_beam_frames;
static void add_rect(int x0, int y0, int x1, int y1, const int clip[4], float r, float g, float b){
  if(x0 < clip[0]) x0 = clip[0]; if(y0 < clip[1]) y0 = clip[1]; if(x1 > clip[2]) x1 = clip[2]; if(y1 > clip[3]) y1 = clip[3];
  if(x1 <= x0 || y1 <= y0 || g_nr >= MAXR) return;
  int k = g_nr; g_r[k][0] = x0; g_r[k][1] = y0; g_r[k][2] = x1 - x0; g_r[k][3] = y1 - y0; g_rc[k][0] = r; g_rc[k][1] = g; g_rc[k][2] = b; g_nr = k + 1;
}
static void project(const float *VP, const float p[3], float o[4]){   /* row vector [x y z 1] * VP */
  for(int j = 0; j < 4; j++) o[j] = p[0] * VP[j] + p[1] * VP[4 + j] + p[2] * VP[8 + j] + VP[12 + j];
}
static void beam_view(const float *VP, float vx, float vy, float vw, float vh, const float base[3], float H, float R, double t){
  float top[3] = { base[0], base[1], base[2] + H }, a[4], b[4];
  project(VP, base, a); project(VP, top, b);
  const float wn = 1.0f;   /* clip the segment against w >= wn (in front of the camera) */
  if(a[3] < wn && b[3] < wn) return;
  if(a[3] < 120.f && a[3] > -120.f) return;   /* closer than ~1.2 m: no beam (as in Borderlands, the beam is for finding the loot) */
  if(a[3] < wn){ float s = (wn - a[3]) / (b[3] - a[3]); for(int j = 0; j < 4; j++) a[j] += (b[j] - a[j]) * s; }
  if(b[3] < wn){ float s = (wn - b[3]) / (a[3] - b[3]); for(int j = 0; j < 4; j++) b[j] += (a[j] - b[j]) * s; }
  float ax = vx + (a[0] / a[3] * 0.5f + 0.5f) * vw, ay = vy + (0.5f - a[1] / a[3] * 0.5f) * vh;
  float bx = vx + (b[0] / b[3] * 0.5f + 0.5f) * vw, by = vy + (0.5f - b[1] / b[3] * 0.5f) * vh;
  int clip[4] = { (int)vx, (int)vy, (int)(vx + vw), (int)(vy + vh) };
  /* width from the depth at the base: R world units at w; P00 = VP column 0 length-ish -> use the projected offset of a point R to the side */
  float side[3] = { base[0] + R, base[1], base[2] }, s4[4], side2[3] = { base[0], base[1] + R, base[2] }, s5[4]; project(VP, side, s4); project(VP, side2, s5);
  float px = 0.f;
  if(s4[3] > wn){ float d = fabsf((s4[0] / s4[3] - a[0] / a[3]) * 0.5f * vw); if(d > px) px = d; }
  if(s5[3] > wn){ float d = fabsf((s5[0] / s5[3] - a[0] / a[3]) * 0.5f * vw); if(d > px) px = d; }
  float pulse = 1.0f + 0.15f * sinf((float)t * 4.0f);
  float hw = px * pulse; if(hw < 1.5f) hw = 1.5f; if(hw > vw * 0.03f + 2.f) hw = vw * 0.03f + 2.f;
  /* the beam is near-vertical on screen (vertical world line); draw it as horizontal slabs following the projected line */
  int steps = 12; float yt = by, yb = ay;
  if(fabsf(yb - yt) < 2.f) return;
  for(int i = 0; i < steps; i++){
    float f0 = (float)i / steps, f1 = (float)(i + 1) / steps;           /* 0 = base */
    float y0 = ay + (by - ay) * f1, y1 = ay + (by - ay) * f0; if(y0 > y1){ float tt = y0; y0 = y1; y1 = tt; }
    float xc = ax + (bx - ax) * (f0 + f1) * 0.5f;
    float fade = 1.0f - f0;                                          /* thinner towards the top */
    float core = hw * (0.45f + 0.55f * fade);
    add_rect((int)(xc - core), (int)y0, (int)(xc + core + 1), (int)y1 + 1, clip, 1.0f, 0.80f, 0.35f);   /* hot core */
    /* halo: sparse vertical stripes (every other pixel), shorter towards the top: reads as a translucent glow */
    if(f0 < 0.7f){ int hs = (int)(hw * 1.6f * fade) + 1; for(int s = 2; s <= hs && g_nr < MAXR - 8; s += 2){
        add_rect((int)(xc - core) - s, (int)y0, (int)(xc - core) - s + 1, (int)y1 + 1, clip, 1.0f, 0.45f, 0.02f);
        add_rect((int)(xc + core) + s, (int)y0, (int)(xc + core) + s + 1, (int)y1 + 1, clip, 1.0f, 0.45f, 0.02f); } }
    if(g_nr >= MAXR - 8) break;
  }
  /* ground glow at the base */
  float gw = hw * 4.f; add_rect((int)(ax - gw), (int)ay - 1, (int)(ax + gw), (int)ay + 2, clip, 1.0f, 0.55f, 0.05f);
  add_rect((int)(ax - gw * 0.6f), (int)ay - 2, (int)(ax + gw * 0.6f), (int)ay + 3, clip, 1.0f, 0.70f, 0.20f);
}
/* FDrawSceneCommand::Execute (render thread), sr = FSceneRenderer: +0x50 FViewInfo[] (0x11A0 each), +0x54 count */
static volatile int g_bhide;   /* game thread: a full-screen UI is up (weapon wheel, mod menu, pause) */
void ext72_scene_views(uint32_t sr){
  g_scene_seen = 1;
  if(!g_bactive || g_bhide){ g_nr = 0; return; }
  if(!gptr(sr)) return;
  uint32_t base = R32(sr + 0x50), nv = R32(sr + 0x54); if(!gptr(base) || nv < 1 || nv > 4) return;
  float p[3], H, R; uint32_t s0; int tries = 0;
  do { s0 = g_bseq; __atomic_thread_fence(__ATOMIC_ACQUIRE); memcpy(p, g_bpos, sizeof p); H = g_bh; R = g_br; __atomic_thread_fence(__ATOMIC_ACQUIRE); } while((s0 != g_bseq || (s0 & 1)) && ++tries < 8);
  g_nr = 0; double t = now_s();
  for(uint32_t i = 0; i < nv; i++){
    uint32_t vi = base + i * VI_STRIDE; float VP[16], xy[4]; memcpy(VP, MEMP(vi + VI_VP), 64); memcpy(xy, MEMP(vi + VI_X), 16);
    if(!(xy[2] > 8.f && xy[3] > 8.f) || !isfinite(VP[0])) continue;
    beam_view(VP, xy[0], xy[1], xy[2], xy[3], p, H, R, t);
  }
  { static int nl; if(nl < 3 && g_nr){ nl++; fprintf(stderr, LOGP "beam: %u view(s), %d rectangles, first at (%d,%d) %dx%d\n", nv, g_nr, g_r[0][0], g_r[0][1], g_r[0][2], g_r[0][3]); } }
}
/* rhi_b1 (render thread, right before Present): the rectangles of the frame being presented */
int ext72_beam_rects(uint32_t w, uint32_t h, int32_t *xywh, float *rgb, int max){
  int seen = g_scene_seen; g_scene_seen = 0;
  if(!g_bactive || !seen) return 0;
  int n = g_nr < max ? g_nr : max, k = 0;
  for(int i = 0; i < n; i++){
    int32_t x = g_r[i][0], y = g_r[i][1], ww = g_r[i][2], hh = g_r[i][3];
    if(x < 0){ ww += x; x = 0; } if(y < 0){ hh += y; y = 0; }
    if(x + ww > (int32_t)w) ww = (int32_t)w - x; if(y + hh > (int32_t)h) hh = (int32_t)h - y;
    if(ww <= 0 || hh <= 0) continue;
    xywh[4 * k] = x; xywh[4 * k + 1] = y; xywh[4 * k + 2] = ww; xywh[4 * k + 3] = hh; memcpy(rgb + 3 * k, g_rc[i], 12); k++;
  }
  if(k) g_beam_frames++;
  return k;
}
/* the FDrawSceneCommand::Execute hook: shared with vr_ui_rhi.c (VR builds) or our own thunk (non-VR builds) */
typedef struct { uint32_t iat; const char *name; lg_gfn fn; } LgImp;   /* = Imp in hle_dll.c */
extern LgImp g_ximps[]; extern int g_nximps; extern int g_nimps;
int vr_late_install(void) __attribute__((weak));
static int g_scene_hook;   /* 1 shared VR thunk, 2 own thunk, -1 failed */
static void t_draw_scene(CPU *c){
  uint32_t cmd = c->ecx;
  if(gptr(cmd)) ext72_scene_views(R32(cmd + 4));
  call_indirect(c, LL_EXEC);
}
static void scene_hook_install(void){
  if(g_scene_hook) return;
  if(vr_late_install){ g_scene_hook = vr_late_install() ? 1 : -1;
    fprintf(stderr, LOGP "beam: scene-view hook %s (vr_ui_rhi.c FDrawSceneCommand thunk)\n", g_scene_hook > 0 ? "shared" : "NOT available"); return; }
  uint32_t cur = R32(LL_VT + 8);
  if(cur != LL_EXEC){ g_scene_hook = -1; fprintf(stderr, LOGP "beam: FDrawSceneCommand slot holds %08x, not %08x: no beam\n", cur, LL_EXEC); return; }
  int x = __atomic_load_n(&g_nximps, __ATOMIC_ACQUIRE);
  if(x + 1 > 4096){ g_scene_hook = -1; return; }
  g_ximps[x].iat = 0; g_ximps[x].name = "legendary!FDrawSceneCommand::Execute"; g_ximps[x].fn = t_draw_scene;
  __atomic_store_n(&g_nximps, x + 1, __ATOMIC_RELEASE);
  uint32_t magic = UR_IMP_BASE + 4u * (uint32_t)(g_nimps + x);
  __atomic_store_n((uint32_t *)MEMP(LL_VT + 8), magic, __ATOMIC_RELEASE);
  g_scene_hook = 2;
  fprintf(stderr, LOGP "beam: own scene-view hook installed (FDrawSceneCommand vtable %08x slot 2 -> %08x)\n", LL_VT, magic);
}

/* ---- init ---- */
static void lg_init(void){
  if(g_state >= 0) return;
  g_state = 0;
  const char *p = getenv("BSI_PATCHES"); if(p && !atoi(p)){ fprintf(stderr, LOGP "BSI_PATCHES=0: legendary drops off\n"); return; }
  const char *e = getenv("BSI_LEGENDARY"); if(e && !atoi(e)){ fprintf(stderr, LOGP "BSI_LEGENDARY=0: legendary drops off\n"); return; }
  const char *t = getenv("BSI_LEGEND_TEST"); if(t) snprintf(g_test, sizeof g_test, "%s", t);
  static const struct { uint32_t a; uint8_t b[8]; } id[] = {
    { F_SPAWN,       { 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x44 } },
    { F_LOADPKG,     { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xE1, 0x49 } },
    { F_TRIGGERPROC, { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x18 } },
    { F_DEALDAMAGE,  { 0x83, 0xEC, 0x7C, 0x56, 0x57, 0x6A, 0x7C, 0x8D } },
    { F_APPMALLOC,   { 0x8B, 0x0D, 0xC8, 0x1C, 0x37, 0x01, 0x85, 0xC9 } },
    { F_APPFREE,     { 0x8B, 0x0D, 0xC8, 0x1C, 0x37, 0x01, 0x85, 0xC9 } },
    { F_DAMAGE,      { 0x6A, 0xFF, 0x68, 0x18, 0xB3, 0x04, 0x01, 0x64 } },
    { F_DESTROY,     { 0x6A, 0xFF, 0x68, 0x10, 0xDD, 0x02, 0x01, 0x64 } } };
  for(unsigned k = 0; k < sizeof id / sizeof id[0]; k++) for(int i = 0; i < 8; i++)
    if(R8(id[k].a + i) != id[k].b[i]){ fprintf(stderr, LOGP "exe identity check failed at %08x: legendary drops off\n", id[k].a); return; }
  cfg_defaults(&g_cfg); find_paths(); load_config(1); state_load();
  g_rng ^= (uint32_t)(now_s() * 1000.0);
  g_state = 1;
  fprintf(stderr, LOGP "legendary drops loaded (built-in defaults%s): \"%s\" (%s) drops when the last praying soldier (%s* in %s) dies; state %s%s%s\n",
          g_have_file ? " + override file" : "", LG_NAME, LG_BASE_ARCH, g_cfg.grp_arch, g_cfg.grp_level, st_name[g_lstate], g_test[0] ? "; TEST " : "", g_test);
}

/* ---- damage handler wrapper (chained in front of weapon perks / gear mod) ---- */
static unsigned g_hits, g_melee_hits, g_kills;
int legendary_skip(uint32_t a){
  if(a != F_DAMAGE) return 0;
  lg_init();
  if(g_state != 1) return 0;
  if(!g_hooked) fprintf(stderr, LOGP "damage hook installed (%08x kept on the slow path)\n", F_DAMAGE);
  g_hooked = 1; return 1;
}
static int chance(Proc *p, double t){ if(p->cooldown > 0 && t - p->last < p->cooldown) return 0; if(frand() >= p->chance) return 0; p->last = t; return 1; }
int legendary_call(CPU *c, uint32_t t){
  if(t != F_DAMAGE || g_state != 1) return 0;
  uint32_t victim = c->ecx - PAWN_FROM_DAMAGEE, inst = R32(c->esp + 4), dt = R32(c->esp + 8);
  int vp = gptr(victim) && g_cfg.enable && !g_firing;
  float hp0 = vp ? pawn_health(victim) : 0;
  extern int weapon_perks_call(CPU *, uint32_t) __attribute__((weak));
  extern int gear_mod_call(CPU *, uint32_t) __attribute__((weak));
  if(!(weapon_perks_call && weapon_perks_call(c, t)) && !(gear_mod_call && gear_mod_call(c, t))){ lg_gfn orig = rt_lookup(t); if(!orig) return 0; orig(c); }
  if(!vp) return 1;
  uint32_t player = player_pawn(); if(!player || victim == player) return 1;
  if(as_pawn(inst) != player && inst != player_pc()) return 1;
  if(!holding_legendary(player)) return 1;
  float hp1 = pawn_health(victim); int killed = hp0 > 0 && hp1 <= 0;
  int melee = gptr(dt) && (R32(dt + DT_FLAGS) & 8);
  double now = now_s();
  g_hits++; if(melee) g_melee_hits++; if(killed) g_kills++;
  if(g_cfg.verbose) fprintf(stderr, LOGP "The Chosen One %s hit on %s (%s): health %.0f -> %.0f%s\n", melee ? "melee" : "gun", cname(victim), oname(dt), hp0, hp1, killed ? " KILL" : "");
  if(melee){ if(!killed && chance(&g_cfg.ignite, now)) fire_proc(c, "FistsOfFire", player, victim, "lightsaber: melee ignite"); }
  else if(!killed && hp1 > 0 && chance(&g_cfg.lift, now)){ tune_lift(); fire_proc(c, "IceFleckedBuckingBronco", player, victim, "force choke: Bucking Bronco lift"); }
  if(killed && chance(&g_cfg.refill, now)) fire_proc(c, "RefillHeldWeaponAmmo", player, player, "kill: ammo refill");
  return 1;
}

/* ---- cloud test mode (BSI_LEGEND_TEST, comma list) ----
   travel:<map>  write GEngine.TravelURL=<map> (TravelType 0) once a player pawn has existed for travel_after s (default 25)
   tp            teleport the player next to the first worshipper found (same floating section)
   kill          deal damage (player instigator, the held gun's damage type or a pistol's) to one living worshipper every 2 s
   fakegroup     treat every XReactiveSkeletalMeshActor / XPawn-less chump near the player (any archetype, any level) as the group
   dropnow       spawn the drop 300 UU in front of the player 20 s after the pawn appears (no worshippers needed)
   own           pretend the drop was picked up (state owned) once the base gun is carried
   give          give the base gun (AcquireWeapon) 10 s after the drop (in place of the pickup interaction)
   stream:<a+b>  force-load these sublevels (LevelStreaming flags), for a teleport into a scene Kismet has not streamed yet
   face          after the drop, stand next to the pickup and look at it (the harness then presses the use key) */
#define F_ACQUIRE 0x00967EE0u
static double g_pawn_t; static uint32_t g_tpawn;
static uint32_t test_dtype(uint32_t pawn){
  uint32_t w = held_gun(pawn);
  if(w){ int o = prop_off(w, "WeaponFireModes", -1); (void)o; }
  uint32_t d = find_obj("HandCannonFounder_Damage", 0, "DamageType"); if(!d) d = find_obj("PistolFounder_Damage", 0, "DamageType");
  if(!d) d = find_obj("Proc_ShortCircuit", 0, "DamageType");
  return d;
}
static void test_tick(CPU *c, uint32_t pawn, double t){
  if(!g_test[0]) return;
  if(pawn != g_tpawn){ g_tpawn = pawn; g_pawn_t = t; }
  const char *tr = test_val("travel");
  static int travelled; uint32_t eng = R32(G_GENGINE);
  if(tr && !travelled && pawn && t - g_pawn_t > (test_val("travel_after") ? atof(test_val("travel_after")) : 25.0) && gptr(eng) && strcmp(top_name(world_info()), tr)){
    travelled = 1; int o = prop_off(eng, "TravelURL", 0x398), ot = prop_off(eng, "TravelType", 0x3A4);
    put_fstring(c, eng + o, tr); W8(eng + ot, 0);
    fprintf(stderr, LOGP "TEST travel: GEngine (%s) TravelURL(+%x) = \"%s\", TravelType(+%x) = 0 (from %s)\n", cname(eng), o, tr, ot, top_name(world_info()));
  }
  /* stream:<pkg>+<pkg>...  ask the engine to load + show these sublevels (LevelStreaming bShouldBeLoaded|bShouldBeVisible) */
  const char *sv = test_val("stream"); static double next_stream;
  if(sv && pawn && t >= next_stream && !strcmp(top_name(world_info()), tr ? tr : top_name(world_info()))){ next_stream = t + 2.0;
    char lst[160]; snprintf(lst, sizeof lst, "%s", sv);
    uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4); static int logged;
    for(uint32_t i = 0; i < n && gptr(go); i++){ uint32_t o = R32(go + 4 * i); if(!gptr(o) || strncmp(cname(o), "LevelStreaming", 14)) continue;
      const char *pk = fname_str(R32(o + 0x28)); char *q = lst; int hit = 0;
      while(*q){ char *e2 = strchr(q, '+'); size_t L = e2 ? (size_t)(e2 - q) : strlen(q); if(strlen(pk) == L && !strncmp(pk, q, L)) hit = 1; if(!e2) break; q = e2 + 1; }
      if(!hit) continue;
      uint32_t f = R32(o + 0x4C);
      if((f & 0x30) != 0x30){ W32(o + 0x4C, f | 0x30); if(logged < 12){ logged++; fprintf(stderr, LOGP "TEST stream: %s (%s %08x) flags %x -> %x, section %d, loaded level %08x\n", pk, cname(o), o, f, f | 0x30, (int)R32(o + 0x5C), R32(o + 0x30)); } } } }
  if(!pawn) return;
  /* face: after the drop, stand 160 UU from the pickup (its floating section) and look at it, every 2 s until it is taken */
  static double next_face;
  if((test_has("face") || test_val("face")) && g_pickup && t >= next_face && t - g_pickup_t > 3.0){ next_face = t + 2.0;
    float pl[3] = { RF32(g_pickup + A_LOC), RF32(g_pickup + A_LOC + 4), RF32(g_pickup + A_LOC + 8) };
    float fd = test_val("face") ? (float)atof(test_val("face")) : 0.f; if(fd < 20.f) fd = 80.f;
    static float me[3]; static uint32_t placed;
    if(placed != g_pickup){ placed = g_pickup; me[0] = pl[0] - fd; me[1] = pl[1]; me[2] = pl[2] + 100.f;   /* once: stand fd UU away (capsule above the floor) */
      W32(pawn + A_SECTION, R32(g_pickup + A_SECTION)); WF32(pawn + A_LOC, me[0]); WF32(pawn + A_LOC + 4, me[1]); WF32(pawn + A_LOC + 8, me[2]);
      int vo = prop_off(pawn, "Velocity", -1); if(vo > 0){ WF32(pawn + vo, 0); WF32(pawn + vo + 4, 0); WF32(pawn + vo + 8, 0); } }
    me[0] = RF32(pawn + A_LOC); me[1] = RF32(pawn + A_LOC + 4); me[2] = RF32(pawn + A_LOC + 8);
    uint32_t pc = player_pc(); float dx = pl[0] - me[0], dy = pl[1] - me[1], dz = (pl[2] - 10.f) - (me[2] + 64.f);
    int32_t yaw = (int32_t)(atan2f(dy, dx) * 32768.f / (float)M_PI), pitch = (int32_t)(atan2f(dz, sqrtf(dx * dx + dy * dy)) * 32768.f / (float)M_PI);
    if(pc){ W32(pc + A_ROT, (uint32_t)pitch); W32(pc + A_ROT + 4, (uint32_t)yaw); }
    if(pc){ int fo = prop_off(pc, "bIsFocusingEnabled", 0x68C); uint32_t fv = R32(pc + fo);   /* use prompts need focusing on; our teleport skips the scripts that turn it on */
      static int fl; if(fl++ < 2) fprintf(stderr, LOGP "TEST face: PC focusing flag word +%x = %08x (bIsFocusingEnabled %d)%s\n", fo, fv, (fv >> 31) & 1, test_has("focus") ? ", forcing it on" : "");
      if(test_has("focus")) W32(pc + fo, fv | 0x80000000u); }
    static int nl; if(nl++ < 3) fprintf(stderr, LOGP "TEST face: player at local (%.0f %.0f %.0f) section %d looking at the pickup (yaw %d pitch %d)\n", me[0], me[1], me[2], (int)R32(pawn + A_SECTION), yaw, pitch);
  }
  static uint32_t tp_done;
  if(test_has("tp") && g_nm && tp_done != pawn){ tp_done = pawn;
    Member *m = &g_m[0]; float l[3] = { m->lpos[0] + 220.f, m->lpos[1] + 60.f, m->lpos[2] + 120.f };
    W32(pawn + A_SECTION, (uint32_t)m->section); WF32(pawn + A_LOC, l[0]); WF32(pawn + A_LOC + 4, l[1]); WF32(pawn + A_LOC + 8, l[2]);
    int vo = prop_off(pawn, "Velocity", -1); if(vo > 0){ WF32(pawn + vo, 0); WF32(pawn + vo + 4, 0); WF32(pawn + vo + 8, 0); }
    fprintf(stderr, LOGP "TEST tp: player %08x -> local (%.0f %.0f %.0f) section %d next to %s\n", pawn, l[0], l[1], l[2], m->section, m->nm);
  }
  static double next_kill;
  if(test_has("kill") && g_nm && t >= next_kill && t - g_pawn_t > 8.0){ next_kill = t + 2.0;
    for(int i = 0; i < g_nm; i++){ Member *m = &g_m[i]; if(!m->alive || deleted(m->a)) continue;
      uint32_t d = test_dtype(pawn); float h0 = ra_health(m->a);
      deal_damage(c, pawn, d, m->a, 1000.f);
      fprintf(stderr, LOGP "TEST kill: %s (%s) health %.0f -> %.0f with %s (instigator player)\n", m->nm, cname(m->a), h0, ra_health(m->a), oname(d));
      break; } }
  static uint32_t dn_done;
  if(test_has("dropnow") && dn_done != pawn && t - g_pawn_t > 20.0 && !g_pickup){ dn_done = pawn;
    Member m; memset(&m, 0, sizeof m);
    int32_t yaw = (int32_t)R32(pawn + A_ROT + 4); float a = yaw * (float)(M_PI / 32768.0);
    m.lpos[0] = RF32(pawn + A_LOC) + 300.f * cosf(a); m.lpos[1] = RF32(pawn + A_LOC + 4) + 300.f * sinf(a); m.lpos[2] = RF32(pawn + A_LOC + 8);
    m.section = (int)R32(pawn + A_SECTION); m.yaw = yaw;
    drop_legendary(c, &m, "TEST dropnow");
  }
  /* equip: once the legendary is carried, make it the held gun (InvManager EquipWeapon) */
  static int equipped;
  if(test_has("equip") && !equipped && carries_base(pawn) && g_lstate == ST_OWNED && !holding_legendary(pawn)){
    uint32_t im = R32(pawn + PAWN_INVMGR), w = 0;
    for(int i = 0; gptr(im) && i < 36; i++){ uint32_t it = R32(im + IM_ITEMS + 4 * i); if(gptr(it) && is_a(it, "XWeapon") && !strcmp(oname(R32(it + UO_ARCH)), LG_BASE_ARCH)) w = it; }
    if(w){ equipped = 1; CPU save = *c; PUSH32(c, 0); PUSH32(c, 0); PUSH32(c, w); c->ecx = im; guest_call(c, 0x00932E60u); *c = save;
      fprintf(stderr, LOGP "TEST equip: EquipWeapon(%s %08x) -> holding legendary %d (eq %d)\n", LG_BASE_ARCH, w, holding_legendary(pawn), (int)R32(im + IM_EQ)); } }
  /* prochit: every 2.5 s damage the nearest living AI pawn as the player (gun hit, melee hit, and every 4th a kill) */
  static double next_ph; static int phn;
  if(test_has("prochit") && holding_legendary(pawn) && t >= next_ph){ next_ph = t + 2.5;
    uint32_t wi = world_info(), best = 0; float bd = 1e18f, me[3]; actor_world_pos(pawn, me);
    for(uint32_t p2 = wi ? R32(wi + prop_off(wi, "PawnList", 0x428)) : 0, k = 0; gptr(p2) && k < 512; k++, p2 = R32(p2 + prop_off(p2, "NextPawn", 0x21C))){
      if(p2 == pawn || !is_a(p2, "XPawn") || pawn_health(p2) <= 0 || strstr(cname(p2), "Elizabeth")) continue;
      float v[3]; actor_world_pos(p2, v); float d = (v[0]-me[0])*(v[0]-me[0]) + (v[1]-me[1])*(v[1]-me[1]) + (v[2]-me[2])*(v[2]-me[2]); if(d < bd){ bd = d; best = p2; } }
    if(best){ int kind = phn++ % 4; uint32_t d = 0;
      if(kind == 1){ d = find_obj("SkyhookPlayerMeleeDamage", "Arc_XDamageTypes", "DamageType");   /* the Sky-Hook melee damage type */
        static int nl; if(nl++ < 1 && d) fprintf(stderr, LOGP "TEST prochit: melee damage type %s flags %x (bIsMelee = 8)\n", oname(d), R32(d + DT_FLAGS)); }
      if(!d) d = find_obj("HandCannonFounder_Damage", 0, "DamageType");
      float h0 = pawn_health(best);
      deal_damage(c, pawn, d, best, kind == 3 ? 50.f : 0.05f);
      fprintf(stderr, LOGP "TEST prochit #%d (%s): %s %08x health %.0f -> %.0f with %s\n", phn, kind == 1 ? "melee" : kind == 3 ? "kill" : "gun", cname(best), best, h0, pawn_health(best), oname(d));
    } else { static int nl; if(nl++ < 3) fprintf(stderr, LOGP "TEST prochit: no living AI pawn in the world\n"); } }
  static uint32_t cmp_done;
  if(test_has("cmpdump") && g_pickup && cmp_done != g_pickup && t - g_pickup_t > 5.0){ cmp_done = g_pickup;
    uint32_t go = R32(G_GOBJECTS), n = R32(G_GOBJECTS + 4);
    for(uint32_t i = 0; i < n && gptr(go); i++){ uint32_t o = R32(go + 4 * i); if(!gptr(o) || deleted(o) || strcmp(cname(o), "XLootContainerKAsset")) continue;
      if(!strstr(oname(R32(o + UO_ARCH)), "Pickup_Weapon")) continue;
      uint32_t cc = R32(o + A_COLL);
      fprintf(stderr, LOGP "TEST cmpdump %s %08x (%s, level %s): flags5c %08x 60 %08x phys %u role %u lootstate %u search %u subst %u ra250 %08x 254 %08x 258 %08x loot0 %u x%d loot1 %u x%d coll %08x(%s) cc+0x?\n",
              o == g_pickup ? "OURS" : "placed", o, oname(R32(o + UO_ARCH)), top_name(o), R32(o + 0x5C), R32(o + 0x60), R8(o + 0x84), R8(o + 0x86), R8(o + 0x47D), R8(o + 0x47E), R8(o + 0x47F),
              R32(o + 0x250), R32(o + 0x254), R32(o + 0x258), R32(o + 0x624), (int)R32(o + 0x62C), R32(o + 0x64C), (int)R32(o + 0x654), cc, gptr(cc) ? cname(cc) : "-"); } }
  /* usekey: at drop + 8/11/14/17 s press the use key (F, VK 70) through the harness key file, as a player would */
  static int uk_n; static uint32_t uk_pick;
  if(test_has("usekey") && g_pickup){ if(uk_pick != g_pickup){ uk_pick = g_pickup; uk_n = 0; }
    /* #0: walk forward 0.25 s (W) so the engine moves the pawn into the pickup's use cylinder (a teleport by memory write
       generates no touch events); then F taps, then an F hold */
    if(uk_n < 5 && t - g_pickup_t > 6.0 + 3.0 * uk_n){ const char *kf = getenv("BSI_KEYFILE"); FILE *f = kf ? fopen(kf, "a") : 0;
      if(f){ fprintf(f, uk_n == 0 ? "k 87 0.25\n" : uk_n == 4 ? "k 70 0.8\n" : "k 70\n"); fclose(f); } uk_n++; fprintf(stderr, LOGP "TEST usekey: F pressed (#%d) at drop + %.0f s, pickup %08x, player carries %d\n", uk_n, t - g_pickup_t, g_pickup, carries_base(pawn)); } }
  /* take:<s>  s seconds after the drop, the player accepts the pickup's loot through the game's own server function
     AXPlayerController::ServerAcceptLootFromContainer(Container, LootIndex 0) (0x97e950, thiscall PC, ret 8) */
  static uint32_t taken;
  if(test_val("take") && g_pickup && taken != g_pickup && t - g_pickup_t > atof(test_val("take")) && !carries_base(pawn)){
    taken = g_pickup; uint32_t pc = player_pc();
    if(pc && R8(0x0097E950u) == 0x56 && R8(0x0097E951u) == 0x8B){ CPU save = *c; PUSH32(c, 0); PUSH32(c, g_pickup); c->ecx = pc; guest_call(c, 0x0097E950u); *c = save; }
    fprintf(stderr, LOGP "TEST take: ServerAcceptLootFromContainer(%08x, 0) -> pickup %s, player carries %s: %d\n", taken, deleted(taken) ? "destroyed" : "still there", LG_BASE_ARCH, carries_base(pawn)); }
  /* dummy: once the legendary is held, spawn a controller-less XHuman (Arc_XBasePawns.XBaseHuman) 300 UU ahead as a proc victim */
  static uint32_t dummy;
  if(test_has("dummy") && !gptr(dummy) && holding_legendary(pawn)){
    uint32_t tm = find_obj("XBaseHuman", "Arc_XBasePawns", "XPawn");
    if(tm){ int32_t yaw = (int32_t)R32(pawn + A_ROT + 4); uint32_t pc = player_pc(); if(pc) yaw = (int32_t)R32(pc + A_ROT + 4); float a = yaw * (float)(M_PI / 32768.0);
      float l[3] = { RF32(pawn + A_LOC) + 300.f * cosf(a), RF32(pawn + A_LOC + 4) + 300.f * sinf(a), RF32(pawn + A_LOC + 8) }; int32_t r[3] = { 0, yaw + 32768, 0 };
      dummy = spawn_actor(c, R32(tm + UO_CLASS), tm, l, r, (int)R32(pawn + A_SECTION), 0);
      if(dummy){ int ho = prop_off(dummy, "Health", 0x2AC), hm = prop_off(dummy, "HealthMax", -1); WF32(dummy + ho, 3000.f); if(hm > 0) WF32(dummy + hm, 3000.f); }
      fprintf(stderr, LOGP "TEST dummy: %s %08x spawned 300 UU ahead (health %.0f)\n", dummy ? cname(dummy) : "nothing", dummy, dummy ? pawn_health(dummy) : 0.f); }
    else { static int nl; if(nl++ < 2) fprintf(stderr, LOGP "TEST dummy: Arc_XBasePawns.XBaseHuman not loaded\n"); dummy = 1; } }
  /* refilltest: 25 s after the legendary is first held, empty its reserve and fire the kill proc (RefillHeldWeaponAmmo) on the
     player, logging the ammo before and after (the proc's effect; the kill trigger itself needs a real kill) */
  static double held_t; static int rf_done;
  if(holding_legendary(pawn)){ if(!held_t) held_t = t; } else held_t = 0;
  if(test_has("refilltest") && !rf_done && held_t && t - held_t > 25.0){ rf_done = 1;
    uint32_t w = held_gun(pawn); W32(w + 0x7C4, 1); W32(w + 0x7EC, 0);
    int c0 = (int)R32(w + 0x7C4), s0 = (int)R32(w + 0x7EC);
    fire_proc(c, "RefillHeldWeaponAmmo", pawn, pawn, "TEST refilltest");
    fprintf(stderr, LOGP "TEST refilltest: The Chosen One ammo %d/%d -> %d/%d right after the proc\n", c0, s0, (int)R32(w + 0x7C4), (int)R32(w + 0x7EC)); }
  if(rf_done == 1 && held_t && t - held_t > 28.0){ rf_done = 2; uint32_t w = held_gun(pawn);
    if(w) fprintf(stderr, LOGP "TEST refilltest: ammo 3 s later %d/%d\n", (int)R32(w + 0x7C4), (int)R32(w + 0x7EC)); }
  if(test_val("give") && g_pickup && t - g_pickup_t > atof(test_val("give")) && !carries_base(pawn)){
    static int given; if(!given){ given = 1;
      uint32_t a = find_obj(LG_BASE_ARCH, "PreCoalescedItemAssets", "XWeapon");
      if(a){ CPU save = *c; PUSH32(c, 0); PUSH32(c, a); c->ecx = pawn; guest_call(c, F_ACQUIRE); *c = save; }
      fprintf(stderr, LOGP "TEST give: AcquireWeapon(%s %08x) -> carried %d\n", LG_BASE_ARCH, a, carries_base(pawn)); } }
}

/* ---- tick (game thread, every UXGameEngine::Tick) ---- */
static unsigned long g_ticks;
static void status_log(double t, uint32_t pawn){
  static double next; if(t < next) return; next = t + 20.0;
  int al = 0, dd = 0; for(int i = 0; i < g_nm; i++){ al += g_m[i].alive; dd += g_m[i].dead; }
  fprintf(stderr, LOGP "status: map %s, enable %d, state %s, group %d (alive %d dead %d), pickup %08x, beam %s (%lu frames drawn), names %s, holding %s, hits %u melee %u kills %u, procs %u (failed %u)\n",
          top_name(world_info()), g_cfg.enable, st_name[g_lstate], g_nm, al, dd, g_pickup, g_bactive ? "on" : "off", g_beam_frames, g_names_on ? "on" : "off",
          holding_legendary(pawn) ? "The Chosen One" : "-", g_hits, g_melee_hits, g_kills, g_procs_fired, g_procs_failed);
}
void legendary_tick(CPU *c){
  if(g_state < 0) lg_init();
  if(g_state != 1) return;
  g_ticks++;
  double t = now_s();
  static double next_cfg; if(t >= next_cfg){ next_cfg = t + 2.0; load_config(0); }
  static double next_work; if(t < next_work) return; next_work = t + 0.2;
  uint32_t pawn = player_pawn();
  uint32_t w = R32(G_GWORLD);
  if(w != g_grp_world){   /* new world: forget the group and the pickup actor (the game destroyed it) */
    if(g_nm || g_pickup) fprintf(stderr, LOGP "world changed (%s): group of %d and pickup %08x forgotten\n", top_name(world_info()), g_nm, g_pickup);
    g_grp_world = w; g_nm = 0; g_pickup = 0; g_next_scan = 0; g_scan_pos = 0; g_scan_added = 0; beam_publish(0, 0); g_pkg_tries = 0; g_tuned_gri = 0; if(ext_prompt_set) ext_prompt_set(0, 0);
    if(g_lstate == ST_DROPPED) state_set(ST_NONE);   /* the dropped pickup did not survive the level change / load */
  }
  { static uint32_t reset_world; if(w != reset_world && pawn){ reset_world = w;   /* a new playthrough starts in the lighthouse (Master_P) */
      if(!strcmp(top_name(world_info()), "Master_P") && g_lstate != ST_NONE && !carries_base(pawn)){ fprintf(stderr, LOGP "new playthrough (Master_P): The Chosen One can drop again\n"); state_set(ST_NONE); } } }
  test_tick(c, pawn, t);
  if(!g_cfg.enable){ if(g_bactive) beam_publish(0, 0); if(g_names_on) names_apply(c, 0, "disabled"); if(ext_prompt_set) ext_prompt_set(0, 0); return; }
  /* the worshippers */
  /* the group lives in the Monument Island Gateway only (group_map, default S_TWN3_P; "*" = any map): no scanning elsewhere,
     and none once the whole group is dead */
  { int all_dead = g_nm > 0; for(int i = 0; i < g_nm; i++) if(!g_m[i].dead) all_dead = 0;
    int map_ok = !strcmp(g_cfg.grp_map, "*") || !strcmp(top_name(world_info()), g_cfg.grp_map);
    if(pawn && map_ok && !all_dead && (g_scan_pos || t >= g_next_scan)){ if(scan_group()) g_next_scan = t + (g_nm ? 5.0 : 2.0); } }
  int alive = 0; Member *last = 0;
  for(int i = 0; i < g_nm; i++){
    Member *m = &g_m[i];
    if(m->dead) continue;
    if(deleted(m->a) || R32(m->a + UO_CLASS) == 0){ if(m->alive){ m->alive = 0; fprintf(stderr, LOGP "worshipper %s removed while alive (streamed out / destroyed): not a kill\n", m->nm); } continue; }
    float hp = ra_health(m->a);
    float wp[3]; actor_world_pos(m->a, wp); memcpy(m->pos, wp, sizeof wp);
    m->lpos[0] = RF32(m->a + A_LOC); m->lpos[1] = RF32(m->a + A_LOC + 4); m->lpos[2] = RF32(m->a + A_LOC + 8);
    m->section = (int)R32(m->a + A_SECTION); m->yaw = (int32_t)R32(m->a + A_ROT + 4);
    if(m->alive && hp <= 0){
      m->alive = 0; m->dead = 1; g_total_dead++; last = m;
      uint32_t kb = m->a + prop_off(m->a, "KillingBlowHitInfo", 0x132C);
      int left = 0; for(int j = 0; j < g_nm; j++) left += g_m[j].alive;
      fprintf(stderr, LOGP "worshipper %s died (health %.0f -> %.0f, killing blow %s): %d of the group left\n", m->nm, m->hp, hp, oname(R32(kb)), left);
    }
    m->hp = hp; alive += m->alive;
  }
  if(last && alive == 0 && !g_pickup) drop_legendary(c, last, "last worshipper died");
  /* the pickup */
  if(g_pickup){
    int gone = deleted(g_pickup) || !is_a(g_pickup, "XReactiveActor");
    if(!gone){ float p[3]; actor_world_pos(g_pickup, p); memcpy(g_pickup_pos, p, sizeof p);
      if(g_cfg.beam){ scene_hook_install(); beam_publish(1, p); } else if(g_bactive) beam_publish(0, 0);
      { extern int ww_is_open(void) __attribute__((weak)); extern int ext_menu_is_open(void) __attribute__((weak)); uint32_t wi = world_info();
        g_bhide = (ww_is_open && ww_is_open()) || (ext_menu_is_open && ext_menu_is_open()) || (wi && gptr(R32(wi + prop_off(wi, "Pauser", 0x3C0)))); }
      uint32_t pc = player_pc(); (void)pc; }
    float dist = 0.f; int reach = !gone && g_cfg.enable && pickup_in_reach(pawn, &dist);
    if(ext_prompt_set) ext_prompt_set(reach ? "[USE] TAKE THE CHOSEN ONE" : 0, LG_DESC);
    if(!reach && g_use_t > 0 && t - g_use_t < 0.3 && g_cfg.verbose){ static int nl; if(nl++ < 6) fprintf(stderr, LOGP "use pressed but the pickup is out of reach (%.0f UU, or not looked at)\n", dist); }
    if(reach && g_use_t > 0 && t - g_use_t < 0.6){ g_use_t = 0; fprintf(stderr, LOGP "use pressed %.0f UU from the pickup\n", dist); take_legendary(c, pawn, "use");
      gone = deleted(g_pickup); if(ext_prompt_set) ext_prompt_set(0, 0); }
    if(gone || carries_base(pawn)){
      fprintf(stderr, LOGP "PICKUP: %s (pickup %08x %s, player carries %s: %d)\n", carries_base(pawn) ? "The Chosen One taken" : "pickup gone without the gun", g_pickup,
              gone ? "destroyed" : "still there", LG_BASE_ARCH, carries_base(pawn));
      if(carries_base(pawn)) state_set(ST_OWNED);
      g_pickup = 0; beam_publish(0, 0); if(ext_prompt_set) ext_prompt_set(0, 0);
    }
  }
  if(g_take_w && t - g_take_t > 0.6){ uint32_t w = g_take_w; g_take_w = 0;
    if(gptr(w) && is_a(w, "XWeapon") && R32(w + 0x7C4) == 0 && R32(w + 0x7EC) == 0){ W32(w + 0x7C4, (uint32_t)g_take_clip); W32(w + 0x7EC, (uint32_t)g_take_spare);
      fprintf(stderr, LOGP "take: ammo re-applied after the equip (%d/%d)\n", g_take_clip, g_take_spare); } }
  if(test_has("own") && g_lstate != ST_OWNED && carries_base(pawn)){ fprintf(stderr, LOGP "TEST own: state owned\n"); state_set(ST_OWNED); }
  /* names follow ownership: on while the drop is out or the legendary is carried */
  int want = (g_pickup != 0) || (g_lstate == ST_OWNED && carries_base(pawn));
  if(pawn || !want) names_apply(c, want, want ? (g_pickup ? "drop in the world" : "carried") : "not in play");
  /* lift proc retune after each map load (the GRI's proc instances are per map) */
  uint32_t gri = game_gri(); if(gri && gri != g_tuned_gri && holding_legendary(pawn)){ if(tune_lift()) g_tuned_gri = gri; }
  status_log(t, pawn);
}

/* ---- mod menu / extension API (ext_menu.c) ---- */
int legendary_get(const char *what){
  lg_init();
  if(!strcmp(what, "on")) return g_state == 1 && g_cfg.enable;
  if(!strcmp(what, "beam")) return g_cfg.beam;
  if(!strcmp(what, "state")) return g_lstate;
  if(!strcmp(what, "pickup")) return g_pickup != 0;
  return -1;
}
void legendary_set(const char *what, int v){
  if(!strcmp(what, "on")){ g_cfg.enable = v; fprintf(stderr, LOGP "menu: legendary drops %s\n", v ? "on" : "off"); }
  else if(!strcmp(what, "beam")){ g_cfg.beam = v; fprintf(stderr, LOGP "menu: loot beam %s\n", v ? "on" : "off"); }
}
const char *legendary_item_name(void){ return LG_NAME; }
const char *legendary_item_desc(void){ return LG_DESC; }
