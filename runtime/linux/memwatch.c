/* memwatch (v0.8.4g diagnostic): every 30 s, on its own low-priority thread, append to <files>/memwatch.txt:
   - process RSS / swap,
   - how much of the 4 GB guest window is resident (mincore) -> game-side memory,
   - the top mappings by RSS+swap from /proc/self/smaps, grouped by name -> host-side memory (DXVK, driver, our code).
   Used to find the ~55 MB/min growth that got the app killed by lmkd after ~40 min. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
extern uint8_t *M;
static char g_mw_path[600];
typedef struct { char name[96]; long kb; long swap; } Grp;
static Grp g_grp[512]; static int g_ngrp;
static void grp_add(const char *nm, long kb, long sw){
  for(int i = 0; i < g_ngrp; i++) if(!strcmp(g_grp[i].name, nm)){ g_grp[i].kb += kb; g_grp[i].swap += sw; return; }
  if(g_ngrp < 512){ snprintf(g_grp[g_ngrp].name, sizeof g_grp[0].name, "%s", nm); g_grp[g_ngrp].kb = kb; g_grp[g_ngrp].swap = sw; g_ngrp++; }
}
static int cmpg(const void *a, const void *b){ long x = ((const Grp*)a)->kb + ((const Grp*)a)->swap, y = ((const Grp*)b)->kb + ((const Grp*)b)->swap; return (y > x) - (y < x); }
static void *mw_thread(void *p){
  (void)p; setpriority(PRIO_PROCESS, 0, 10); pthread_setname_np(pthread_self(), "BSI-memwatch");
  static unsigned char vec[(1u << 20)];   /* 4 GB / 4 KB pages */
  time_t t0 = time(0);
  for(;;){
    sleep(30);
    FILE *o = fopen(g_mw_path, "a"); if(!o) continue;
    long rss = 0, swp = 0; char b[512];
    FILE *f = fopen("/proc/self/status", "r"); if(f){ while(fgets(b, sizeof b, f)){ sscanf(b, "VmRSS: %ld", &rss); sscanf(b, "VmSwap: %ld", &swp); } fclose(f); }
    long guest = 0;
    if(M){ for(uint64_t off = 0; off < (1ull << 32); off += (256ull << 20)){   /* 256 MB chunks */
        if(mincore(M + off, 256u << 20, vec) == 0) for(unsigned i = 0; i < (256u << 20) / 4096; i++) guest += vec[i] & 1; } }
    g_ngrp = 0;
    f = fopen("/proc/self/smaps", "r");
    if(f){ char cur[96] = "[anon]"; long kb, sw;
      while(fgets(b, sizeof b, f)){
        unsigned long lo, hi; char perms[8];
        if(sscanf(b, "%lx-%lx %7s", &lo, &hi, perms) == 3 && strchr(b, ' ')){
          char *nm = NULL; int fld = 0; for(char *q = b; *q; q++) if(*q == ' ' && q[1] != ' ' && ++fld == 5){ nm = q + 1; break; }
          if(nm){ nm[strcspn(nm, "\n")] = 0; while(*nm == ' ') nm++; }
          if((uint8_t*)lo >= M && (uint8_t*)lo < M + (1ull << 32)) snprintf(cur, sizeof cur, "[guest window]");
          else if(nm && *nm){ char *sl = strrchr(nm, '/'); snprintf(cur, sizeof cur, "%s", (sl && nm[0] == '/') ? sl + 1 : nm); }
          else snprintf(cur, sizeof cur, "[anon]");
        } else if(sscanf(b, "Rss: %ld kB", &kb) == 1) grp_add(cur, kb, 0);
        else if(sscanf(b, "Swap: %ld kB", &sw) == 1) grp_add(cur, 0, sw);
      }
      fclose(f); }
    qsort(g_grp, g_ngrp, sizeof(Grp), cmpg);
    fprintf(o, "[memwatch] +%lds rss %ld MB swap %ld MB | guest window resident %ld MB\n", (long)(time(0) - t0), rss >> 10, swp >> 10, (guest * 4) >> 10);
    for(int i = 0; i < g_ngrp && i < 12; i++) fprintf(o, "  %-40s rss %6ld MB swap %6ld MB\n", g_grp[i].name, g_grp[i].kb >> 10, g_grp[i].swap >> 10);
    fclose(o);
  }
  return NULL;
}
void memwatch_start(const char *files){
  snprintf(g_mw_path, sizeof g_mw_path, "%s/memwatch.txt", files);
  pthread_t t; if(pthread_create(&t, NULL, mw_thread, NULL) == 0) pthread_detach(t);
}
