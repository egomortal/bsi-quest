/* hitchlog.c -- HITCHES.md item H1: per-frame hitch attribution log.  Diagnostic, OFF unless BSI_HITCHLOG is set.
 *
 * Why: the frame timeline (bridge.c tl_*) splits a frame into Present wait / "pre" (render thread before its first draw) /
 * draw submission, but "pre" mixes three different causes: the render thread waiting for the game thread, the render thread
 * creating D3D resources (shaders, input layouts, textures), and the render thread blocked behind DXVK's CS thread while it
 * compiles pipelines.  In quest_logs/v086_run 91 of the 98 frames over 100 ms are "pre"; this log says which one it is.
 *
 * Use: env.txt line  BSI_HITCHLOG=/sdcard/BSI/hitch.csv  (+ timeline.txt for the stage split).  Analyse with
 * regress/hitch/hitch_report.py hitch.csv [timeline.csv].
 *
 * Hooks (see hitchlog_bridge.patch): hl_pre(name)/hl_post(name) at the top of bridge_pre()/bridge_post() in linux/bridge.c,
 * which run around every bridged COM call on the calling guest thread.  This file is #included at the end of bridge.c
 * (same pattern as evlog.c in hle_more.c), so no build-script change is needed.
 *
 * One CSV line per Present (written by the render thread, buffered, appended every ~4 s):
 *   t_ms        CLOCK_MONOTONIC ms at this Present
 *   frame_ms    time since the previous Present
 *   present_ms  duration of the previous Present call (DXVK frame-latency / GPU wait)
 *   rt,gt,cs,shc,ld,sct,job,sub  CPU ms run by: render thread, game thread (g_game_tid), dxvk-cs, dxvk-shader-* (all),
 *               PoolThread + AsyncIOSystem (package/texture loading, LZO), ShaderCreateThr, XJob*, dxvk-submit/queue/frame
 *   rt_rq,gt_rq run-queue wait ms (runnable but not running) of render and game thread
 *   proc_ms     CPU ms of the whole process (all threads, /proc/self/stat, 10 ms ticks: only meaningful summed over bursts)
 *   n_sh,n_il,n_tex,n_buf,n_view,n_oth   ID3D11Device::Create* calls in this frame on any thread, by kind
 *   cr_rt_ms,cr_oth_ms   time inside those Create* calls on the render thread / on other threads
 *   gd_rt_ms    time inside ID3D11DeviceContext::GetData on the render thread (GPU query polling, incl. the frame-event wait)
 *   map_rt_ms   time inside Map / UpdateSubresource on the render thread (CS-thread syncs show up here)
 *
 * Cost when off: one load + branch per COM call.  When on: two clock reads per Create/GetData/Map/Present call, ~16 preads
 * of /proc/self/task/<tid>/schedstat per frame (~50 us), a /proc/self/task scan every 2 s.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

extern volatile int g_game_tid __attribute__((weak));   /* xr_glue.c (Android); absent on the Linux build -> getpid() */

enum { HC_RT, HC_GT, HC_CS, HC_SHC, HC_LD, HC_SCT, HC_JOB, HC_SUB, HC_N };
enum { HK_NONE, HK_SH, HK_IL, HK_TEX, HK_BUF, HK_VIEW, HK_OTH, HK_GD, HK_MAP, HK_PRESENT };

static int hl_on = -1;
static FILE *hl_f;
static __thread uint64_t hl_t0; static __thread int hl_kind, hl_mytid;
static int hl_rt_tid;
static uint64_t hl_cnt[HK_PRESENT + 1];                 /* per-frame counters, atomically incremented */
static uint64_t hl_ns_rt[HK_PRESENT + 1], hl_ns_oth[HK_PRESENT + 1];
static uint64_t hl_present_ns;

#define HL_MAXT 24
static struct { int tid, cls, fd; uint64_t run, wq; } hl_t[HL_MAXT];
static int hl_nt; static uint64_t hl_scan_ns, hl_last_ns; static uint64_t hl_proc_ticks;
static char hl_buf[1 << 16]; static size_t hl_len; static uint64_t hl_flush_ns;

static uint64_t hl_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static int hl_gettid(void){ if(!hl_mytid) hl_mytid = (int)syscall(SYS_gettid); return hl_mytid; }

static int hl_class(const char *comm, int tid){
  int gt = &g_game_tid ? g_game_tid : getpid();
  if(tid == hl_rt_tid) return HC_RT;
  if(tid == gt) return HC_GT;
  if(!strncmp(comm, "dxvk-cs", 7)) return HC_CS;
  if(!strncmp(comm, "dxvk-shader", 11)) return HC_SHC;
  if(!strncmp(comm, "PoolThread", 10) || !strncmp(comm, "AsyncIOSystem", 13)) return HC_LD;
  if(!strncmp(comm, "ShaderCreate", 12)) return HC_SCT;
  if(!strncmp(comm, "XJob", 4)) return HC_JOB;
  if(!strncmp(comm, "dxvk-submit", 11) || !strncmp(comm, "dxvk-queue", 10) || !strncmp(comm, "dxvk-frame", 10)) return HC_SUB;
  return -1;
}

static int hl_read_sched(int fd, uint64_t *run, uint64_t *wq){
  char b[96]; ssize_t n = pread(fd, b, sizeof b - 1, 0); if(n <= 0) return 0; b[n] = 0;
  unsigned long long r = 0, w = 0; if(sscanf(b, "%llu %llu", &r, &w) != 2) return 0; *run = r; *wq = w; return 1;
}

/* (re)build the tracked-thread table: keep fds of known tids, add new matching threads */
static void hl_scan(void){
  DIR *d = opendir("/proc/self/task"); if(!d) return;
  struct dirent *e; int seen[HL_MAXT] = {0};
  while((e = readdir(d))){
    int tid = atoi(e->d_name); if(tid <= 0) continue;
    int k; for(k = 0; k < hl_nt; k++) if(hl_t[k].tid == tid) break;
    if(k < hl_nt){ seen[k] = 1; continue; }
    if(hl_nt >= HL_MAXT) continue;
    char p[64], comm[32] = {0}; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    int cf = open(p, O_RDONLY | O_CLOEXEC); if(cf < 0) continue; ssize_t n = read(cf, comm, sizeof comm - 1); close(cf);
    if(n > 0 && comm[n - 1] == '\n') comm[n - 1] = 0;
    int cls = hl_class(comm, tid); if(cls < 0) continue;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    int fd = open(p, O_RDONLY | O_CLOEXEC); if(fd < 0) continue;
    hl_t[hl_nt].tid = tid; hl_t[hl_nt].cls = cls; hl_t[hl_nt].fd = fd;
    hl_read_sched(fd, &hl_t[hl_nt].run, &hl_t[hl_nt].wq); seen[hl_nt] = 1; hl_nt++;
  }
  closedir(d);
  for(int k = 0; k < hl_nt; k++) if(!seen[k]){ close(hl_t[k].fd); hl_t[k] = hl_t[--hl_nt]; seen[k] = seen[hl_nt]; k--; }   /* thread exited */
}

static uint64_t hl_proc(void){
  static int fd = -1; if(fd < 0) fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC); if(fd < 0) return 0;
  char b[1024]; ssize_t n = pread(fd, b, sizeof b - 1, 0); if(n <= 0) return 0; b[n] = 0;
  char *p = strrchr(b, ')'); if(!p) return 0;
  unsigned long long ut = 0, st = 0;   /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
  if(sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
  return ut + st;
}

static int hl_enabled(void){
  if(hl_on >= 0) return hl_on;
  const char *p = getenv("BSI_HITCHLOG"); hl_on = 0;
  if(p && *p){ hl_f = fopen(p, "a");
    if(hl_f){ hl_on = 1; fputs("t_ms,frame_ms,present_ms,rt,gt,cs,shc,ld,sct,job,sub,rt_rq,gt_rq,proc_ms,n_sh,n_il,n_tex,n_buf,n_view,n_oth,"
                                "cr_rt_ms,cr_oth_ms,gd_rt_ms,map_rt_ms\n", hl_f); fflush(hl_f);
      fprintf(stderr, "[hitchlog] per-frame attribution -> %s\n", p); } }
  return hl_on;
}

static int hl_kind_of(const char *n){
  if(n[0] != 'I') return HK_NONE;
  if(!strncmp(n, "ID3D11Device::Create", 20)){
    const char *s = n + 20;
    if(strstr(s, "Shader") && strncmp(s, "ShaderResourceView", 18)) return HK_SH;   /* Vertex/Pixel/Geometry/Hull/Domain/ComputeShader */
    if(!strncmp(s, "InputLayout", 11)) return HK_IL;
    if(!strncmp(s, "Texture", 7)) return HK_TEX;
    if(!strncmp(s, "Buffer", 6)) return HK_BUF;
    if(strstr(s, "View")) return HK_VIEW;
    return HK_OTH;
  }
  if(!strncmp(n, "ID3D11DeviceContext::", 21)){
    const char *s = n + 21;
    if(!strcmp(s, "GetData")) return HK_GD;
    if(!strcmp(s, "Map") || !strcmp(s, "UpdateSubresource")) return HK_MAP;
    return HK_NONE;
  }
  if(n[1] == 'D' && n[2] == 'X' && strstr(n, "::Present") && !strstr(n, "LastPresent")) return HK_PRESENT;
  return HK_NONE;
}

static void hl_frame(uint64_t now){
  double cpu[HC_N] = {0}, rq_rt = 0, rq_gt = 0;
  if(!hl_scan_ns || now - hl_scan_ns > 2000000000ull){ hl_scan(); hl_scan_ns = now; }
  for(int k = 0; k < hl_nt; k++){
    uint64_t r, w; if(!hl_read_sched(hl_t[k].fd, &r, &w)) continue;
    double dr = (r - hl_t[k].run) / 1e6, dw = (w - hl_t[k].wq) / 1e6; hl_t[k].run = r; hl_t[k].wq = w;
    cpu[hl_t[k].cls] += dr;
    if(hl_t[k].cls == HC_RT) rq_rt += dw; else if(hl_t[k].cls == HC_GT) rq_gt += dw;
  }
  uint64_t pt = hl_proc(); double proc_ms = hl_proc_ticks ? (pt - hl_proc_ticks) * 1000.0 / sysconf(_SC_CLK_TCK) : 0; hl_proc_ticks = pt;
  uint64_t c[HK_PRESENT + 1], rt[HK_PRESENT + 1], ot[HK_PRESENT + 1];
  for(int i = 0; i <= HK_PRESENT; i++){ c[i] = __atomic_exchange_n(&hl_cnt[i], 0, __ATOMIC_RELAXED);
    rt[i] = __atomic_exchange_n(&hl_ns_rt[i], 0, __ATOMIC_RELAXED); ot[i] = __atomic_exchange_n(&hl_ns_oth[i], 0, __ATOMIC_RELAXED); }
  double cr_rt = 0, cr_ot = 0; for(int i = HK_SH; i <= HK_OTH; i++){ cr_rt += rt[i] / 1e6; cr_ot += ot[i] / 1e6; }
  if(hl_last_ns && hl_len < sizeof hl_buf - 400)
    hl_len += snprintf(hl_buf + hl_len, sizeof hl_buf - hl_len,
      "%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f\n",
      now / 1e6, (now - hl_last_ns) / 1e6, hl_present_ns / 1e6, cpu[HC_RT], cpu[HC_GT], cpu[HC_CS], cpu[HC_SHC], cpu[HC_LD], cpu[HC_SCT],
      cpu[HC_JOB], cpu[HC_SUB], rq_rt, rq_gt, proc_ms, (unsigned long long)c[HK_SH], (unsigned long long)c[HK_IL],
      (unsigned long long)c[HK_TEX], (unsigned long long)c[HK_BUF], (unsigned long long)c[HK_VIEW], (unsigned long long)c[HK_OTH],
      cr_rt, cr_ot, rt[HK_GD] / 1e6, rt[HK_MAP] / 1e6);
  hl_last_ns = now;
  if(now - hl_flush_ns > 4000000000ull && hl_len){ fwrite(hl_buf, 1, hl_len, hl_f); fflush(hl_f); hl_len = 0; hl_flush_ns = now; }
}

void hl_pre(const char *name){
  if(__builtin_expect(hl_on == 0, 1)) return;
  if(hl_on < 0 && !hl_enabled()) return;
  int k = hl_kind_of(name); hl_kind = k; if(k == HK_NONE) return;
  uint64_t now = hl_now(); hl_t0 = now;
  if(k == HK_PRESENT){ if(!hl_rt_tid){ hl_rt_tid = hl_gettid(); hl_scan_ns = 0; } hl_frame(now); }
}

void hl_post(const char *name){
  (void)name;
  if(__builtin_expect(hl_on <= 0, 1)) return;
  int k = hl_kind; if(k == HK_NONE) return; hl_kind = HK_NONE;
  uint64_t d = hl_now() - hl_t0;
  if(k == HK_PRESENT){ hl_present_ns = d; return; }
  __atomic_fetch_add(&hl_cnt[k], 1, __ATOMIC_RELAXED);
  if(hl_gettid() == hl_rt_tid) __atomic_fetch_add(&hl_ns_rt[k], d, __ATOMIC_RELAXED);
  else __atomic_fetch_add(&hl_ns_oth[k], d, __ATOMIC_RELAXED);
}
