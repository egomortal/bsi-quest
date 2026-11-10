#define _GNU_SOURCE
/* One-shot probe: can this app (untrusted_app SELinux domain) reach the
 * Hexagon DSP (FastRPC) and Adreno OpenCL?  Writes /sdcard/BSI/dsp_probe.txt.
 * Read-only checks; skipped if /sdcard/BSI/nodspprobe.txt exists. */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>

static void probe_dev(FILE *f, const char *p) {
  int fd = open(p, O_RDONLY | O_NONBLOCK);
  fprintf(f, "open(%s, RDONLY) = %d errno=%s\n", p, fd, fd < 0 ? strerror(errno) : "-");
  if (fd >= 0) close(fd);
  fd = open(p, O_RDWR | O_NONBLOCK);
  fprintf(f, "open(%s, RDWR)   = %d errno=%s\n", p, fd, fd < 0 ? strerror(errno) : "-");
  if (fd >= 0) close(fd);
}
static void probe_lib(FILE *f, const char *l, const char **syms) {
  void *h = dlopen(l, RTLD_NOW | RTLD_LOCAL);
  fprintf(f, "dlopen(%s) = %p %s\n", l, h, h ? "" : dlerror());
  if (!h) return;
  for (; *syms; syms++) fprintf(f, "  %s = %p\n", *syms, dlsym(h, *syms));
}
static void *probe_thr(void *arg) {
  (void)arg;
  if (access("/sdcard/BSI/nodspprobe.txt", F_OK) == 0) return 0;
  FILE *f = fopen("/sdcard/BSI/dsp_probe.txt", "w");
  if (!f) return 0;
  probe_dev(f, "/dev/adsprpc-smd");
  probe_dev(f, "/dev/adsprpc-smd-secure");
  probe_dev(f, "/dev/kgsl-3d0");
  probe_dev(f, "/dev/dma_heap/system");
  const char *rpc[] = {"remote_handle64_open", "remote_handle_control", "remote_session_control", "rpcmem_alloc", 0};
  probe_lib(f, "libcdsprpc.so", rpc);
  probe_lib(f, "libadsprpc.so", rpc);
  const char *cl[] = {"clGetPlatformIDs", 0};
  probe_lib(f, "libOpenCL.so", cl);
  void *h = dlopen("libOpenCL.so", RTLD_NOW);
  if (h) {
    int (*gp)(uint32_t, void *, uint32_t *) = (int (*)(uint32_t, void *, uint32_t *))dlsym(h, "clGetPlatformIDs");
    uint32_t n = 0; int r = gp ? gp(0, 0, &n) : -999;
    fprintf(f, "clGetPlatformIDs -> %d, %u platforms\n", r, n);
  }
  cpu_set_t cs; CPU_ZERO(&cs);
  if (sched_getaffinity(0, sizeof cs, &cs) == 0) {
    fprintf(f, "affinity:"); for (int i = 0; i < 8; i++) if (CPU_ISSET(i, &cs)) fprintf(f, " %d", i); fprintf(f, "\n");
  }
  fclose(f);
  return 0;
}
void dsp_probe_start(void) { pthread_t t; if (pthread_create(&t, 0, probe_thr, 0) == 0) pthread_detach(t); }
