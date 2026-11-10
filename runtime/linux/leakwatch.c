/* leakwatch (memory-leak diagnostic, LEAK.md): env-gated sampler, no effect unless BSI_LEAKWATCH=<file> is set.
   Every BSI_LEAKWATCH_S seconds (default 15) a low-priority thread appends one block to <file>:
   - VmRSS / VmSwap of the process,
   - /proc/self/smaps outside the guest window, by kind: unnamed anon (count of VMAs, the largest few with perms, so
     e.g. an rwx Unicorn code buffer or a ---p driver reservation is recognisable), [heap], named anon ([anon:...]),
     device/dmabuf mappings, file mappings,
   - glibc malloc totals (mallinfo2: in-use arena bytes + mmapped blocks; lavapipe's device memory lives here),
   - guest-window residency per region (mincore): image+stacks (<0x20000000), small-object heap (0x20000000-0x40000000,
     gmalloc size classes), page arena 1 (0x40000000-0x79000000, VirtualAlloc/large blocks, bottom-up), page arena 2
     (0x80000000-0xDE000000, zero-copy DXVK chunks are taken top-down from here), the rest,
   - a walk of the small-object heap (bump-allocated, so blocks are contiguous; header = size class, caller):
     carved bytes per class, and the bridge's 64-byte COM wrappers (caller tag 2): total, live (tag 'BRDG' at +40),
     forgotten (tag cleared by br_forget, never freed), and the live ones by interface (D3D11 objects whose host refcount
     never reached 0 through the guest -> refcount leak candidates).
   Linux and Android. No locks are taken: everything read is either kernel data or guest memory read racily. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <malloc.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include "com.h"
extern uint8_t *M;
#define LW_TAG 0x47445242u
static char lw_path[600]; static int lw_period = 15;
void leakwatch_start(const char *path);
static long mincore_mb(uint64_t lo, uint64_t hi){
  static unsigned char vec[(64u << 20) / 4096]; long pg = 0;
  for(uint64_t a = lo; a < hi; a += (64u << 20)){ uint64_t n = hi - a < (64u << 20) ? hi - a : (64u << 20);
    if(mincore(M + a, n, vec) == 0) for(uint64_t i = 0; i < n / 4096; i++) pg += vec[i] & 1; }
  return (pg * 4096) >> 20; }
typedef struct { uint64_t lo, hi; char perms[8]; long rss, anon; char prev[48]; } Big;
static void lw_sample(FILE *o, long t){
  long rss = 0, swp = 0; char b[640];
  FILE *f = fopen("/proc/self/status", "r"); if(f){ while(fgets(b, sizeof b, f)){ sscanf(b, "VmRSS: %ld", &rss); sscanf(b, "VmSwap: %ld", &swp); } fclose(f); }
  /* smaps outside the window */
  long k_anon = 0, k_anon_sw = 0, k_anon_anon = 0, n_anon = 0, k_heap = 0, k_named = 0, k_dev = 0, k_file = 0, k_win = 0; int kind = 0; Big big[6]; memset(big, 0, sizeof big); Big cur = {0};
  long bk_n[5] = {0}, bk_rss[5] = {0}; char prevname[48] = "";   /* unnamed VMAs by size: <1M, 1-4M, 4-64M, 64M-1G, >=1G */
  f = fopen("/proc/self/smaps", "r");
  if(f){ while(fgets(b, sizeof b, f)){
      unsigned long lo, hi; char perms[8];
      if(sscanf(b, "%lx-%lx %7s", &lo, &hi, perms) == 3 && b[0] != ' ' && strchr(b, '-') && strchr(b, '-') < strchr(b, ' ')){
        char *nm = NULL; int fld = 0; for(char *q = b; *q; q++) if(*q == ' ' && q[1] != ' ' && ++fld == 5){ nm = q + 1; break; }
        if(nm){ nm[strcspn(nm, "\n")] = 0; while(*nm == ' ') nm++; }
        if(cur.hi && kind == 1){ for(int i = 0; i < 6; i++) if(cur.rss > big[i].rss){ memmove(&big[i + 1], &big[i], (5 - i) * sizeof(Big)); big[i] = cur; break; } }
        if(cur.hi && kind == 1){ uint64_t sz = cur.hi - cur.lo; int bi = sz < (1u<<20) ? 0 : sz < (4u<<20) ? 1 : sz < (64u<<20) ? 2 : sz < (1ull<<30) ? 3 : 4; bk_n[bi]++; bk_rss[bi] += cur.rss; }
        cur.lo = lo; cur.hi = hi; snprintf(cur.perms, sizeof cur.perms, "%s", perms); cur.rss = 0; cur.anon = 0; snprintf(cur.prev, sizeof cur.prev, "%s", prevname);
        if(M && (uint8_t*)lo >= M && (uint8_t*)lo < M + (1ull << 32)) kind = 7;
        else if(!nm || !*nm){ kind = 1; n_anon++; }
        else if(!strcmp(nm, "[heap]")) kind = 2;
        else if(!strncmp(nm, "[anon:", 6)) kind = 3;
        else if(!strncmp(nm, "/dev/", 5) || strstr(nm, "dmabuf") || strstr(nm, "kgsl") || strstr(nm, "anon_inode")) kind = 4;
        else kind = 5;
        if(nm && *nm && kind != 7){ const char *sl = strrchr(nm, '/'); snprintf(prevname, sizeof prevname, "%s", (sl && nm[0] == '/') ? sl + 1 : nm); }
        continue; }
      long kb;
      if(sscanf(b, "Rss: %ld kB", &kb) == 1){ cur.rss += kb;
        switch(kind){ case 1: k_anon += kb; break; case 2: k_heap += kb; break; case 3: k_named += kb; break; case 4: k_dev += kb; break; case 5: k_file += kb; break; case 7: k_win += kb; break; } }
      else if(sscanf(b, "Anonymous: %ld kB", &kb) == 1 && kind == 1){ k_anon_anon += kb; cur.anon += kb; }
      else if(sscanf(b, "Swap: %ld kB", &kb) == 1 && kind == 1) k_anon_sw += kb; }
    if(cur.hi && kind == 1){ for(int i = 0; i < 6; i++) if(cur.rss > big[i].rss){ memmove(&big[i + 1], &big[i], (5 - i) * sizeof(Big)); big[i] = cur; break; }
      uint64_t sz = cur.hi - cur.lo; int bi = sz < (1u<<20) ? 0 : sz < (4u<<20) ? 1 : sz < (64u<<20) ? 2 : sz < (1ull<<30) ? 3 : 4; bk_n[bi]++; bk_rss[bi] += cur.rss; }
    fclose(f); }
  int nthr = 0, nfd = 0; { char p2[64]; for(int pass = 0; pass < 2; pass++){ snprintf(p2, sizeof p2, pass ? "/proc/self/fd" : "/proc/self/task"); DIR *d = opendir(p2); if(!d) continue; struct dirent *de; while((de = readdir(d))) if(de->d_name[0] != '.') ++*(pass ? &nfd : &nthr); closedir(d); } }
  long ma_inuse = 0, ma_mmap = 0, ma_free = 0;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  { struct mallinfo2 mi = mallinfo2(); ma_inuse = (long)(mi.uordblks >> 20); ma_mmap = (long)(mi.hblkhd >> 20); ma_free = (long)(mi.fordblks >> 20); }
#elif defined(__ANDROID__)
  { struct mallinfo mi = mallinfo(); ma_inuse = (long)((size_t)mi.uordblks >> 20); ma_free = (long)((size_t)mi.fordblks >> 20); }
#endif
  fprintf(o, "[lw] +%lds rss %ld MB swap %ld MB | outside window: unnamed anon %ld MB (+%ld swap, %ld vmas) heap %ld named-anon %ld dev %ld files %ld | in window (smaps) %ld MB | malloc in-use %ld MB mmapped %ld MB free %ld MB\n",
    t, rss >> 10, swp >> 10, k_anon >> 10, k_anon_sw >> 10, n_anon, k_heap >> 10, k_named >> 10, k_dev >> 10, k_file >> 10, k_win >> 10, ma_inuse, ma_mmap, ma_free);
  fprintf(o, "[lw]   unnamed anon: Anonymous %ld MB of Rss %ld MB | by VMA size <1M %ld/%ldMB 1-4M %ld/%ldMB 4-64M %ld/%ldMB 64M-1G %ld/%ldMB >=1G %ld/%ldMB (count/rss) | threads %d fds %d\n",
    k_anon_anon >> 10, k_anon >> 10, bk_n[0], bk_rss[0] >> 10, bk_n[1], bk_rss[1] >> 10, bk_n[2], bk_rss[2] >> 10, bk_n[3], bk_rss[3] >> 10, bk_n[4], bk_rss[4] >> 10, nthr, nfd);
  fprintf(o, "[lw]   largest unnamed anon:"); for(int i = 0; i < 6 && big[i].rss; i++) fprintf(o, " %lx+%luMB %s rss %ldMB anon %ldMB after '%s';", big[i].lo, (unsigned long)((big[i].hi - big[i].lo) >> 20), big[i].perms, big[i].rss >> 10, big[i].anon >> 10, big[i].prev); fputc('\n', o);
  if(!M) return;
  /* guest regions */
  fprintf(o, "[lw]   guest resident MB: low(img/stacks) %ld | small heap %ld | arena1 %ld | arena2 %ld (zc top-down) | high %ld\n",
    mincore_mb(0x10000, 0x20000000u), mincore_mb(0x20000000u, 0x40000000u), mincore_mb(0x40000000u, 0x79000000u), mincore_mb(0x80000000u, 0xDE000000u), mincore_mb(0xDE000000u, 0x100000000ull));
  /* small-object heap walk */
  uint64_t cls_b[32] = {0}, h = 0x20000000u, wr_tot = 0, wr_live = 0, wr_dead = 0, wr_null = 0; uint32_t live_if[128] = {0}; int bad = 0; void *dev_host = NULL;
  while(h < 0x40000000u){ uint32_t k; memcpy(&k, M + h, 4); if(k < 4 || k > 19){ if(k) bad = 1; break; }
    uint32_t caller; memcpy(&caller, M + h + 4, 4); cls_b[k] += 1u << k;
    if(caller == 2 && k == 7){ uint32_t o0 = (uint32_t)h + 8, vt, tag, ii; uint64_t hp; memcpy(&vt, M + o0, 4); memcpy(&tag, M + o0 + 40, 4); memcpy(&hp, M + o0 + 32, 8); memcpy(&ii, M + o0 + 8, 4);
      wr_tot++; if(tag == LW_TAG){ wr_live++; if(ii < 128){ live_if[ii]++; if(!dev_host && ii < (uint32_t)g_com_nifaces && !strcmp(g_com_ifaces[ii].name, "ID3D11Device")) dev_host = (void*)(uintptr_t)hp; } } else if(hp) wr_dead++; else wr_null++; }
    h += 1u << k; }
  fprintf(o, "[lw]   small heap carved %llu MB (walk %s at %08llx):", (unsigned long long)((h - 0x20000000u) >> 20), bad ? "stopped on a bad header" : "ended", (unsigned long long)h);
  for(int k = 4; k < 20; k++) if(cls_b[k] >= (1u << 20)) fprintf(o, " %uB:%lluMB", 1u << k, (unsigned long long)(cls_b[k] >> 20)); fputc('\n', o);
  fprintf(o, "[lw]   COM wrappers (128 B blocks, caller 2): %llu total = %llu live bridged + %llu forgotten (br_forget, never freed) + %llu null-renderer/other | live by iface:",
    (unsigned long long)wr_tot, (unsigned long long)wr_live, (unsigned long long)wr_dead, (unsigned long long)wr_null);
  for(int r = 0; r < 10; r++){ int bi = -1; uint32_t bv = 0; for(int i = 0; i < 128 && i < g_com_nifaces; i++) if(live_if[i] > bv){ bv = live_if[i]; bi = i; } if(bi < 0) break;
    fprintf(o, " %s %u", g_com_ifaces[bi].name, bv); live_if[bi] = 0; }
  fputc('\n', o);
  /* DXVK's own device-memory accounting (IDXGIAdapter3::QueryVideoMemoryInfo, local + non-local segment groups): on the
     Quest a leaked texture is KGSL memory that never shows up in RSS, so this is the D3D-object-leak detector.
     Off with BSI_LW_NODXGI=1. Device -> QI(IDXGIDevice) -> GetAdapter (slot 7) -> QI(IDXGIAdapter3) -> slot 14. */
  /* v0.8.5c: opt-in only (BSI_LW_DXGI=1). Calling into DXVK's COM objects from this background thread with a pointer
     recovered by a racy heap walk crashed v0.8.5b on the Quest; also require the vtable to point into a loaded library. */
  if(dev_host && !getenv("BSI_LW_DXGI")) dev_host = NULL;
  if(dev_host){ /* the pointer comes from a racy walk of recyclable guest blocks: read its vtable safely first */
    void *vt = NULL; struct iovec l = { &vt, sizeof vt }, r = { dev_host, sizeof vt }; Dl_info vi;
    if(process_vm_readv(getpid(), &l, 1, &r, 1, 0) != (ssize_t)sizeof vt || !vt || !dladdr(vt, &vi) || !vi.dli_fname || !strstr(vi.dli_fname, "dxvk")) dev_host = NULL; }
  if(dev_host){
    static const uint8_t iid_dxgidev[16] = {0xfa,0x77,0xec,0x54,0x77,0x13,0xe6,0x44,0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c};
    static const uint8_t iid_adapter3[16] = {0xa4,0x67,0x59,0x64,0x92,0x13,0x10,0x43,0xa7,0x98,0x80,0x53,0xce,0x3e,0x93,0xfd};
    typedef int32_t (*QI)(void*, const void*, void**); typedef uint32_t (*REL)(void*);
    void *dd = 0, *ad = 0, *a3 = 0; struct { uint64_t budget, usage, avail, resv; } mi[2]; memset(mi, 0, sizeof mi);
    if(((QI)((*(void***)dev_host)[0]))(dev_host, iid_dxgidev, &dd) >= 0 && dd){
      if(((int32_t(*)(void*, void**))((*(void***)dd)[7]))(dd, &ad) >= 0 && ad){
        if(((QI)((*(void***)ad)[0]))(ad, iid_adapter3, &a3) >= 0 && a3){
          for(int g = 0; g < 2; g++) ((int32_t(*)(void*, uint32_t, uint32_t, void*))((*(void***)a3)[14]))(a3, 0, (uint32_t)g, &mi[g]);
          ((REL)((*(void***)a3)[2]))(a3); }
        ((REL)((*(void***)ad)[2]))(ad); }
      ((REL)((*(void***)dd)[2]))(dd); }
    fprintf(o, "[lw]   DXVK video memory: local usage %llu MB (budget %llu MB), non-local usage %llu MB (budget %llu MB)\n",
      (unsigned long long)(mi[0].usage >> 20), (unsigned long long)(mi[0].budget >> 20), (unsigned long long)(mi[1].usage >> 20), (unsigned long long)(mi[1].budget >> 20)); }
  /* optional: runtime statics by link-time offset (PIE), BSI_LW_SYMS="mh=<hex>,heap_brk=<hex>,fl_n=<hex>,zc_live=<hex>" from nm */
  { static int init; static uint8_t *base; static uint64_t o_mh, o_brk, o_fln, o_zc; const char *e = getenv("BSI_LW_SYMS");
    if(!init){ init = 1; Dl_info di; if(e && dladdr((void*)leakwatch_start, &di)) base = (uint8_t*)di.dli_fbase;
      for(const char *q = e; q && *q; ){ char nm[32]; unsigned long long v; int n = 0; if(sscanf(q, "%31[^=]=%llx%n", nm, &v, &n) < 2) break;
        if(!strcmp(nm, "mh")) o_mh = v; else if(!strcmp(nm, "heap_brk")) o_brk = v; else if(!strcmp(nm, "fl_n")) o_fln = v; else if(!strcmp(nm, "zc_live")) o_zc = v;
        q += n; if(*q == ',') q++; } }
    if(base){ fprintf(o, "[lw]   runtime:");
      if(o_brk) fprintf(o, " small-heap brk %u MB;", (*(uint32_t*)(base + o_brk) - 0x20000000u) >> 20);
      if(o_fln){ const uint32_t *fl = (const uint32_t*)(base + o_fln); uint64_t fb = 0; for(int k = 4; k < 20; k++) fb += (uint64_t)fl[k] << k; fprintf(o, " on free lists %llu MB;", (unsigned long long)(fb >> 20)); }
      if(o_zc) fprintf(o, " zc live %llu MB;", (unsigned long long)(*(uint64_t*)(base + o_zc) >> 20));
      if(o_mh){ void *const *mh = (void *const *)(base + o_mh); uint32_t live = 0, tomb = 0; for(uint32_t i = 0; i < (1u << 18); i++){ if(mh[i] == (void*)1) tomb++; else if(mh[i]) live++; }
        fprintf(o, " bridge map (2^18 slots): %u live, %u tombstones, %u empty;", live, tomb, (1u << 18) - live - tomb); }
      fputc('\n', o); } } }
static void *lw_thread(void *p){
  (void)p; setpriority(PRIO_PROCESS, 0, 10); pthread_setname_np(pthread_self(), "BSI-leakwatch");
  time_t t0 = time(0);
  for(;;){ sleep(lw_period); FILE *o = fopen(lw_path, "a"); if(!o) continue; lw_sample(o, (long)(time(0) - t0)); fclose(o); }
  return NULL; }
void leakwatch_start(const char *path){
  static int started; if(__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;   /* once (ctor on Linux, android_glue on the Quest) */
  snprintf(lw_path, sizeof lw_path, "%s", path); const char *e = getenv("BSI_LEAKWATCH_S"); if(e && atoi(e) > 0) lw_period = atoi(e);
  pthread_t t; if(pthread_create(&t, NULL, lw_thread, NULL) == 0) pthread_detach(t); }
__attribute__((constructor)) static void leakwatch_ctor(void){ const char *e = getenv("BSI_LEAKWATCH"); if(e && *e) leakwatch_start(e); }
