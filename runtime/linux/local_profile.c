#include "hle.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifdef BSI_ANDROID
#define PROFILE_FILE "/sdcard/BSI/user/profile.bin"
#define PROFILE_TMP  "/sdcard/BSI/user/profile.bin.tmp"
#define PROFILE_DIR  "/sdcard/BSI/user"
#else   /* cloud harness: BSI_PROFILE_DIR (default /tmp/claude-0/bsi_user) */
static char g_pdir[256], g_pfile[300], g_ptmp[300];
static const char *pdir(void){ if(!g_pdir[0]){ const char *e=getenv("BSI_PROFILE_DIR"); snprintf(g_pdir,sizeof g_pdir,"%s",e&&*e?e:"/tmp/claude-0/bsi_user");
  snprintf(g_pfile,sizeof g_pfile,"%s/profile.bin",g_pdir); snprintf(g_ptmp,sizeof g_ptmp,"%s/profile.bin.tmp",g_pdir); } return g_pdir; }
#define PROFILE_DIR  pdir()
#define PROFILE_FILE (pdir(), g_pfile)
#define PROFILE_TMP  (pdir(), g_ptmp)
#endif
#define PROFILE_THUNK_BASE 0x01600000u
#define PROFILE_THUNK_COUNT 55u

static pthread_once_t profile_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t profile_file_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t profile_interface;
static uint32_t profile_vtable;
static uint32_t profile_filename;
static __thread int profile_call_reentry;

static const uint8_t profile_method_argc[PROFILE_THUNK_COUNT] = {
  3, 3, 3, 3, 4, 1, 1, 1, 2, 1, 4, 2, 2, 1, 1, 1, 1, 1,
  0, 2, 2, 0, 0, 1, 3, 4, 6, 6, 0, 1, 8, 2, 3, 3, 3, 3,
  3, 3, 2, 3, 2, 1, 2, 1, 2, 3, 2, 3, 2, 5, 9, 3, 2, 6,
  4
};

static void profile_init_once(void) {
  profile_interface = gmalloc(4);
  profile_vtable = gmalloc(PROFILE_THUNK_COUNT * 4);
  profile_filename = gmalloc(sizeof("profile.bin"));
  if (!profile_interface || !profile_vtable || !profile_filename) {
    fprintf(stderr, "[profile] unable to allocate guest interface state\n");
    profile_interface = profile_vtable = profile_filename = 0;
    return;
  }
  for (uint32_t i = 0; i < PROFILE_THUNK_COUNT; i++) {
    W32(profile_vtable + i * 4, PROFILE_THUNK_BASE + i * 8);
  }
  W32(profile_interface, profile_vtable);
  memcpy(MEMP(profile_filename), "profile.bin", sizeof("profile.bin"));
  fprintf(stderr, "[profile] local SteamRemoteStorage014 shim ready\n");
}

static int profile_ready(void) {
  pthread_once(&profile_once, profile_init_once);
  return profile_interface != 0;
}

/* ---- SteamUser020: an offline local user (logged on, fixed SteamID, user data folder). Licence/ownership calls
   (UserHasLicenseForApp etc.) are NOT answered positively: they return "no auth" / 0 as before. BSI_FAKE_USER=0: off. ---- */
#define USER_THUNK_BASE 0x01601000u
#define USER_THUNK_COUNT 32u
static const uint8_t user_argc[USER_THUNK_COUNT] = {
  0, 0, 1, 7, 2, 4, 2, 0, 0, 3, 9, 6, 0, 3, 4, 2, 1, 3, 0, 4, 2, 3, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
static uint32_t user_interface, user_vtable;
static int user_ready(void) {
  static int tried; if (tried) return user_interface != 0; tried = 1;
  user_interface = gmalloc(4); user_vtable = gmalloc(USER_THUNK_COUNT * 4);
  if (!user_interface || !user_vtable) { user_interface = 0; return 0; }
  for (uint32_t i = 0; i < USER_THUNK_COUNT; i++) W32(user_vtable + i * 4, USER_THUNK_BASE + i * 8);
  W32(user_interface, user_vtable);
  fprintf(stderr, "[profile] local SteamUser020 (offline local user) ready\n");
  return 1;
}
static volatile int g_front_end;   /* set at the first BLoggedOn query from the front end (title screen up) */
static int user_call(CPU *c, uint32_t target) {
  uint32_t slot = (target - USER_THUNK_BASE) / 8, argc = user_argc[slot], r = 0, rhi = 0, ra = R32(c->esp);
  uint32_t a0 = argc > 0 ? ARG(0) : 0, a1 = argc > 1 ? ARG(1) : 0;
  switch (slot) {
    case 0: r = 1; break;                                         /* GetHSteamUser */
    case 1: r = 1; if (ra == 0x8b11b9u) g_front_end = 1; break;   /* BLoggedOn (0x8b11b9: front end sign-in check) */
    case 2: if (a0) { W32(a0, 0x00000001u); W32(a0 + 4, 0x01100001u); } r = a0; break;   /* GetSteamID -> hidden return CSteamID */
    case 6: {                                                     /* GetUserDataFolder(char *buf, int size) */
      char d[300]; snprintf(d, sizeof d, "%s", PROFILE_DIR); uint32_t n = (uint32_t)strlen(d) + 1;
      if (a0 && (int)a1 >= (int)n) { memcpy(MEMP(a0), d, n); r = 1; } break; }
    case 17: r = 2; break;                                        /* UserHasLicenseForApp -> k_EUserHasLicenseResultNoAuth (unchanged meaning: unknown) */
    case 23: r = 1; break;                                        /* GetPlayerSteamLevel */
    default: r = 0; break;
  }
  c->eax = r; c->edx = rhi; c->esp += 4 + 4 * argc;
  { static unsigned cnt[USER_THUNK_COUNT]; unsigned n = ++cnt[slot]; if (n <= 10 || (n % 100000) == 0) fprintf(stderr, "[profile] SteamUser slot %u args %08x %08x ret %08x from %08x (call %u)\n", slot, a0, a1, r, ra, n); }
  return 1;
}

uint32_t local_profile_find_interface(uint32_t version_ptr) {
  char version[96];
  const char *patches = getenv("BSI_PATCHES");
  if (patches && atoi(patches) == 0) return 0;
  if (!profile_ready()) return 0;
  g2host_a(version_ptr, version, sizeof(version));
  if (!strcmp(version, "SteamUser020")) { const char *f = getenv("BSI_FAKE_USER"); if (f && atoi(f) == 0) return 0; return user_ready() ? user_interface : 0; }
  if (strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION014") != 0) return 0;
  return profile_interface;
}

/* ---- storage: every file the game writes through ISteamRemoteStorage lives in PROFILE_DIR/cloud/, one host file per
   game file name (escaped: anything but [A-Za-z0-9._-] becomes %XX). FileDelete moves the file into cloud/.deleted/. ---- */
static const char *cloud_dir(void) { static char d[320]; if (!d[0]) snprintf(d, sizeof d, "%s/cloud", PROFILE_DIR); return d; }
static int cloud_ready(void) {
  struct stat st;
  if (stat(PROFILE_DIR, &st) != 0) mkdir(PROFILE_DIR, 0777);
  if (stat(cloud_dir(), &st) == 0) return S_ISDIR(st.st_mode);
  if (mkdir(cloud_dir(), 0777) == 0 || errno == EEXIST) return 1;
  fprintf(stderr, "[profile] create %s failed: %s\n", cloud_dir(), strerror(errno));
  return 0;
}
static int esc_name(uint32_t guest_name, char *out, int n) {   /* 0 = bad name */
  char name[260]; if (!guest_name) return 0;
  g2host_a(guest_name, name, sizeof name); if (!name[0]) return 0;
  int o = 0;
  for (int i = 0; name[i] && o < n - 4; i++) { unsigned char ch = (unsigned char)name[i];
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || (ch == '.' && i > 0)) out[o++] = (char)ch;
    else o += snprintf(out + o, n - o, "%%%02X", ch); }
  out[o] = 0; return 1;
}
static void unesc_name(const char *in, char *out, int n) {
  int o = 0; for (int i = 0; in[i] && o < n - 1; i++) {
    if (in[i] == '%' && in[i + 1] && in[i + 2]) { unsigned v; sscanf(in + i + 1, "%2x", &v); out[o++] = (char)v; i += 2; } else out[o++] = in[i]; }
  out[o] = 0;
}
static int cloud_path(uint32_t guest_name, char *path, int n) {
  char e[400]; if (!esc_name(guest_name, e, sizeof e) || !cloud_ready()) return 0;
  snprintf(path, n, "%s/%s", cloud_dir(), e); return 1;
}
static int cloud_size(uint32_t guest_name) {   /* -1 = missing */
  char p[800]; struct stat st;
  if (!cloud_path(guest_name, p, sizeof p) || stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 0x7fffffff) return -1;
  return (int)st.st_size;
}
static void cloud_list_dirty(void);
static int cloud_write(uint32_t name_ptr, uint32_t data_ptr, uint32_t length) {
  cloud_list_dirty();
  char p[800], t[820]; if (!cloud_path(name_ptr, p, sizeof p) || length > 64u * 1024u * 1024u) return 0;
  snprintf(t, sizeof t, "%s.tmp", p);
  int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { fprintf(stderr, "[profile] open %s failed: %s\n", t, strerror(errno)); return 0; }
  const uint8_t *data = (const uint8_t *)MEMP(data_ptr); size_t done = 0;
  while (done < length) { ssize_t k = write(fd, data + done, length - done); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; done += (size_t)k; }
  int ok = done == length && fsync(fd) == 0; int se = errno;
  if (close(fd) != 0 && ok) { ok = 0; se = errno; }
  if (ok && rename(t, p) != 0) { ok = 0; se = errno; }
  if (!ok) { unlink(t); fprintf(stderr, "[profile] write %s failed after %zu/%u bytes: %s\n", p, done, length, strerror(se)); return 0; }
  fprintf(stderr, "[profile] wrote %u bytes to %s\n", length, p);
  return 1;
}
static int cloud_read(uint32_t name_ptr, uint32_t data_ptr, uint32_t length) {
  char p[800]; if (!cloud_path(name_ptr, p, sizeof p) || length > 64u * 1024u * 1024u) return 0;
  int fd = open(p, O_RDONLY); if (fd < 0) return 0;
  uint8_t *data = (uint8_t *)MEMP(data_ptr); size_t done = 0;
  while (done < length) { ssize_t k = read(fd, data + done, length - done); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; done += (size_t)k; }
  close(fd);
  fprintf(stderr, "[profile] loaded %zu bytes from %s\n", done, p);
  return (int)done;
}
static int cloud_delete(uint32_t name_ptr) {   /* never destroys data: moved to cloud/.deleted/<name>.<time> */
  cloud_list_dirty();
  char p[800], dd[400], q[900]; struct stat st;
  if (!cloud_path(name_ptr, p, sizeof p) || stat(p, &st) != 0) return 0;
  snprintf(dd, sizeof dd, "%s/.deleted", cloud_dir()); mkdir(dd, 0777);
  const char *b = strrchr(p, '/'); snprintf(q, sizeof q, "%s/%s.%ld", dd, b ? b + 1 : p, (long)time(0));
  int ok = rename(p, q) == 0; fprintf(stderr, "[profile] FileDelete: %s -> %s (%s)\n", p, q, ok ? "kept" : strerror(errno));
  return ok;
}
/* enumeration: regular files in cloud/ (not *.tmp), sorted by name. Up to CLOUD_MAX files: the old cap of 64 kept the
   first 64 files in readdir (directory-hash) order, so with more than 32 saves (.sav + .bkm each) an arbitrary subset,
   often including the newest save, was missing from GetFileCount/GetFileNameAndSize and Continue loaded an older save
   (cloud runs swq2 vs swq3, 80 files). */
#include <dirent.h>
#define CLOUD_MAX 2048
static char g_names[CLOUD_MAX][400];   /* filled by cloud_list(); callers hold profile_file_lock */
static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int g_list_n = -1; static time_t g_list_t;   /* listing cache: GetFileNameAndSize(i) is called once per file */
static void cloud_list_dirty(void) { g_list_n = -1; }
static int cloud_list(void) {
  if (g_list_n >= 0 && time(0) - g_list_t <= 2) return g_list_n;
  int n = 0, total = 0; if (!cloud_ready()) return 0;
  DIR *d = opendir(cloud_dir()); if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    size_t L = strlen(e->d_name); if (L > 4 && !strcmp(e->d_name + L - 4, ".tmp")) continue;
    char p[800]; struct stat st; snprintf(p, sizeof p, "%s/%s", cloud_dir(), e->d_name);
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    total++; if (n < CLOUD_MAX) snprintf(g_names[n++], 400, "%s", e->d_name);
  }
  closedir(d);
  qsort(g_names, n, sizeof g_names[0], name_cmp);
  g_list_n = n; g_list_t = time(0);
  if (total > n) { static int w; if (w++ < 3) fprintf(stderr, "[profile] cloud folder has %d files; only %d are listed\n", total, n); }
  return n;
}
static uint32_t g_name_buf[CLOUD_MAX];   /* guest copies of names handed out by GetFileNameAndSize */

static void profile_set_return(CPU *c, uint32_t value, uint32_t argc) {
  c->eax = value;
  c->edx = 0;
  c->esp += 4 + 4 * argc;
}

static uint32_t g_ra;
static int profile_remote_call(CPU *c, uint32_t target) {
  g_ra = R32(c->esp);
  uint32_t slot = (target - PROFILE_THUNK_BASE) / 8;
  uint32_t argc;
  uint32_t a0, a1, a2;
  int result = 0;
  int handled = 1;

  if (target < PROFILE_THUNK_BASE ||
      target >= PROFILE_THUNK_BASE + PROFILE_THUNK_COUNT * 8) return 0;
  argc = profile_method_argc[slot];
  a0 = argc > 0 ? ARG(0) : 0;
  a1 = argc > 1 ? ARG(1) : 0;
  a2 = argc > 2 ? ARG(2) : 0;

  if (profile_call_reentry) {
    profile_set_return(c, 0, argc);
    fprintf(stderr, "[profile] slot %u args %08x %08x %08x ret %08x\n",
            slot, a0, a1, a2, c->eax);
    return 1;
  }
  profile_call_reentry = 1;
  pthread_mutex_lock(&profile_file_lock);
  switch (slot) {
    case 0: /* FileWrite(name, data, size) */
      result = cloud_write(a0, a1, a2);
      break;
    case 1: /* FileRead(name, buffer, size) -> bytes read */
      result = cloud_read(a0, a1, a2);
      break;
    case 5: /* FileForget(name): keep the local copy */
      result = cloud_size(a0) >= 0;
      break;
    case 6: /* FileDelete(name) */
      result = cloud_delete(a0);
      break;
    case 8: /* SetSyncPlatforms(name, platforms) */
      result = cloud_size(a0) >= 0;
      break;
    case 13: /* FileExists */
    case 14: /* FilePersisted */
      result = cloud_size(a0) >= 0;
      break;
    case 15: /* GetFileSize -> 0 when missing */
      { int sz = cloud_size(a0); result = sz > 0 ? sz : 0; }
      break;
    case 16: { /* GetFileTimestamp -> int64 */
      char p[800]; struct stat st; result = 0; c->edx = 0;
      if (cloud_path(a0, p, sizeof p) && stat(p, &st) == 0) result = (int)st.st_mtime;
      break; }
    case 17: /* GetSyncPlatforms -> k_ERemoteStoragePlatformAll */
      result = cloud_size(a0) >= 0 ? (int)0xffffffff : 0;
      break;
    case 18: { /* GetFileCount */
      result = cloud_list();
      break; }
    case 19: { /* GetFileNameAndSize(index, int32 *size) -> const char * (guest) */
      int n = cloud_list(); result = 0; char (*names)[400] = g_names;
      if ((int)a0 >= 0 && (int)a0 < n) {
        char un[400], p[800]; struct stat st; unesc_name(names[a0], un, sizeof un);
        snprintf(p, sizeof p, "%s/%s", cloud_dir(), names[a0]);
        if (a1) W32(a1, stat(p, &st) == 0 ? (uint32_t)st.st_size : 0);
        if (!g_name_buf[a0]) g_name_buf[a0] = gmalloc(400);
        if (g_name_buf[a0]) { memcpy(MEMP(g_name_buf[a0]), un, strlen(un) + 1); result = (int)g_name_buf[a0]; }
      }
      break; }
    case 20: { /* GetQuota(uint64 *total, uint64 *available): a 0-byte quota made the game wait forever for space */
      int n = cloud_list(); char (*names)[400] = g_names; uint64_t used = 0, total = 1024ull << 20;
      for (int i = 0; i < n; i++) { char p[800]; struct stat st; snprintf(p, sizeof p, "%s/%s", cloud_dir(), names[i]); if (stat(p, &st) == 0) used += (uint64_t)st.st_size; }
      if (a0) W64(a0, total);
      if (a1) W64(a1, total - used);
      result = 1;
      break; }
    case 21: /* IsCloudEnabledForAccount */
    case 22: /* IsCloudEnabledForApp */
      result = 1;   /* our "cloud" is the local folder */
      break;
    case 23: /* SetCloudEnabledForApp(bool): nothing to change */
      result = 0;
      break;
    default:
      handled = 0;
      result = 0;
      break;
  }
  pthread_mutex_unlock(&profile_file_lock);
  profile_call_reentry = 0;
  profile_set_return(c, (uint32_t)result, argc);
  { static unsigned cnt[PROFILE_THUNK_COUNT]; unsigned n = __atomic_add_fetch(&cnt[slot], 1, __ATOMIC_RELAXED);
    if (n <= 20 || (n % 100000) == 0) {   /* polls (GetFileCount/GetQuota every frame) would fill the log */
      if (!handled) fprintf(stderr, "[profile] UNHANDLED slot %u\n", slot);
      fprintf(stderr, "[profile] slot %u args %08x %08x %08x ret %08x from %08x (call %u)\n", slot, a0, a1, a2, c->eax, g_ra, n); } }
  return 1;
}

int local_profile_call(CPU *c, uint32_t target) {
  if (target >= USER_THUNK_BASE && target < USER_THUNK_BASE + USER_THUNK_COUNT * 8) return user_call(c, target);
  if (target >= PROFILE_THUNK_BASE &&
      target < PROFILE_THUNK_BASE + PROFILE_THUNK_COUNT * 8) {
    return profile_remote_call(c, target);
  }
  return 0;
}

/* ---- Steam callbacks (offline): the game registers CCallback objects; with no Steam client nothing ever fires, so the
   online subsystem never sees "connected/logged on". We record registrations (diagnostics) and, with BSI_STEAM_CB=1,
   deliver SteamServersConnected_t (101) once. ---- */
#define CB_N 512
static struct { uint32_t obj; int id; } g_cb[CB_N];
static int g_ncb;
static pthread_mutex_t g_cb_mu = PTHREAD_MUTEX_INITIALIZER;
HLE(steam_api,SteamAPI_RegisterCallback){
  uint32_t o = ARG(0); int id = (int)ARG(1);
  pthread_mutex_lock(&g_cb_mu);
  if (g_ncb < CB_N) { g_cb[g_ncb].obj = o; g_cb[g_ncb].id = id; g_ncb++; }
  pthread_mutex_unlock(&g_cb_mu);
  { static int n; if (n++ < 80) { uint32_t vt = o ? R32(o) : 0;
      fprintf(stderr, "[steam] RegisterCallback id %d obj %08x vtable %08x [%08x %08x %08x] flags %02x\n", id, o, vt, vt ? R32(vt) : 0, vt ? R32(vt + 4) : 0, vt ? R32(vt + 8) : 0, o ? R8(o + 4) : 0); } }
  RET_C(0);
}
HLE(steam_api,SteamAPI_UnregisterCallback){
  uint32_t o = ARG(0);
  pthread_mutex_lock(&g_cb_mu);
  for (int i = 0; i < g_ncb; i++) if (g_cb[i].obj == o) { g_cb[i] = g_cb[--g_ncb]; break; }
  pthread_mutex_unlock(&g_cb_mu);
  RET_C(0);
}
static void cb_deliver(CPU *c, int id, int vslot, uint32_t data) {
  uint32_t objs[64]; int n = 0;
  pthread_mutex_lock(&g_cb_mu);
  for (int i = 0; i < g_ncb && n < 64; i++) if (g_cb[i].id == id) objs[n++] = g_cb[i].obj;
  pthread_mutex_unlock(&g_cb_mu);
  for (int i = 0; i < n; i++) {
    uint32_t o = objs[i], fn = R32(R32(o) + 4 * vslot);
    uint32_t sa = c->eax, sc = c->ecx, sd = c->edx, sesp = c->esp;
    fprintf(stderr, "[steam] delivering callback %d to %08x via vtable[%d] = %08x\n", id, o, vslot, fn);
    c->ecx = o; PUSH32(c, data); guest_call(c, fn);
    c->esp = sesp; c->eax = sa; c->ecx = sc; c->edx = sd;
  }
}
HLE(steam_api,SteamAPI_RunCallbacks){
  static long n; static int on = -1, slot = 1;
  if (on < 0) { const char *e = getenv("BSI_STEAM_CB"); on = e && atoi(e) > 0; const char *s = getenv("BSI_STEAM_CB_SLOT"); if (s) slot = atoi(s); }
  if (on && ++n == 60) {   /* ~1 s after the first pump: SteamServersConnected_t (empty struct) */
    uint32_t data = gmalloc(16); if (data) memset(MEMP(data), 0, 16);
    cb_deliver(c, 101, slot, data);
  }
  RET_C(0);
}

/* ---- login status: with no Steam client the Steamworks subsystem never logs in (status 0 = LS_NotLoggedIn), so the
   front end shows "Sign in is required" and the profile/continue paths stay off. Steam's own offline mode reports
   LS_UsingLocalProfile (1) (the game's f_0087f8f0 does exactly that when SteamUser()->BLoggedOn() is false). We report 1
   for the local player when the native says 0. No ownership/licence answer is involved. BSI_LOCAL_LOGIN=0: off. ---- */
int local_login_status_call(CPU *c) {
  static int on = -1; if (on < 0) { const char *e = getenv("BSI_LOCAL_LOGIN"); on = !e || atoi(e) != 0; }
  if (!on) return 0;
  static __thread int in; if (in) return 0;          /* re-entry: patch_call falls through to the original */
  uint32_t result = R32(c->esp + 8);                  /* execGetLoginStatus(FFrame &Stack, RESULT_DECL) thiscall */
  in = 1; call_indirect(c, 0x874d30u); in = 0;
  { static int m; if (m++ < 8) fprintf(stderr, "[profile] GetLoginStatus call: result %08x status %d ret %08x\n", result, result ? R8(result) : -1, R32(c->esp)); }
  if (result && R8(result) == 0) { W8(result, 1);
    static int n; if (n++ < 5) fprintf(stderr, "[profile] GetLoginStatus: not logged in -> LS_UsingLocalProfile (offline local profile)\n"); }
  return 1;
}

/* ---- save list at startup: the front end's saved-game list is filled by UXGameEngine's populate task
   (f_00aee160 -> task vtable 0x11e4738 -> f_00af0450 -> f_00aedc60: RemoteStorage GetFileCount/GetFileNameAndSize/
   FileRead). On PC the script starts it from the login/storage-device flow, which never runs without a Steam client,
   so "Continue" stays off even though our saves exist. Hook UXGameEngine::Tick (0xaf0480): log the save-state fields
   when they change and, once the title screen is up (BSI_SAVE_POPULATE unset = auto, 0 = off, N = at tick N), start the populate task once,
   exactly as execPopulateSavedGameInfoFromSelectedStorageDevice does. No ownership/licence logic is touched. ---- */
int local_engine_tick_call(CPU *c) {
  static __thread int in; if (in) return 0;
  static int pop = -1; static long ticks; static uint32_t last[5];
  static long fe_tick, done;
  if (pop < 0) { const char *e = getenv("BSI_SAVE_POPULATE"); pop = e ? atoi(e) : -2; }   /* unset: auto; 0: off; N: at tick N */
  uint32_t eng = c->ecx; ticks++;
  if (eng) {
    uint32_t v[5] = { R32(eng + 0x4a8), R32(eng + 0x534), R32(eng + 0x528), R32(eng + 0x4d4), R8(eng + 0x506) };
    if (memcmp(v, last, sizeof v)) { static int n; if (n++ < 40)
        fprintf(stderr, "[profile] engine tick %ld: flags %08x task %08x dev %08x/%08x state %u\n", ticks, v[0], v[1], v[2], v[3], v[4]);
      memcpy(last, v, sizeof v); }
    if (pop == -2 && g_front_end && !fe_tick) fe_tick = ticks;
    int go = !done && ((pop > 0 && ticks == pop) || (pop == -2 && fe_tick && ticks == fe_tick + 30));
    /* auto = 30 ticks after the title screen's first sign-in check. Starting it during the startup movies froze the
       game thread (cloud run sav17, tick 1500); at the title it is safe (sav18). */
    if (go) { done = 1;
      CPU save = *c;
      fprintf(stderr, "[profile] tick %ld: starting the saved-game populate task (flags %08x task %08x)\n", ticks, v[0], v[1]);
      c->esp -= 4; W32(c->esp, 0xdeadbeefu); c->ecx = eng;
      in = 1; call_indirect(c, 0xaee160u); in = 0;
      *c = save;
    }
  }
  in = 1; call_indirect(c, 0xaf0480u); in = 0;
  return 1;
}
