/* BSI_QPCFAST (notes/HLE_HOT.md, item H5): QueryPerformanceCounter / QueryPerformanceFrequency straight from the ARM
   generic timer.  #included by hle_fast.c and by regress/hle/hle_fast_test.c.

   Old: QPC = clock_gettime(CLOCK_MONOTONIC) through the vDSO, scaled to ns (QPF = 1e9).  New (aarch64 only): QPC =
   CNTVCT_EL0 (the counter the vDSO itself reads, readable from EL0 on every arm64 Linux), QPF = CNTFRQ_EL0
   (19.2 MHz on Qualcomm parts; Windows 10 reports 10 MHz, so the game sees a normal value).  One "isb; mrs" instead of
   the vDSO's seqlock + scaling; no 64-bit multiply in the game's own QPC->seconds math either.
   Evidence: __kernel_clock_gettime is 0.14-0.36 % of the GameThread and ~1 % of the RenderingThread on the Quest
   (v0.8.4b/c in-app profiles; part of the RT share is the GetData spin, notes/GETDATA_SPIN.md), and QPC has 154 call
   sites + 37 cached-pointer sites in the exe (UE3 appSeconds/appCycles, stats, timers).
   Safety: at init the counter is checked against CLOCK_MONOTONIC over ~2 ms (rate within 2 %, monotonic); otherwise
   the old handlers stay.  Wrap-around: UE3's appCycles() truncates QPC to 32 bits; at 19.2 MHz that wraps every 223 s
   instead of every 4.3 s with the old 1 GHz, so cycle deltas are safer, not worse.  GetTickCount / timeGetTime are
   left alone (ms resolution, rarely called).  BSI_QPCFAST=0: old handlers.  Other hosts (x86-64 cloud): not installed. */
#include <stdint.h>
#include <time.h>

#if defined(__aarch64__)
static inline uint64_t qpc_cntvct(void){ uint64_t v; __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static inline uint64_t qpc_cntfrq(void){ uint64_t v; __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#define QPC_HAVE 1
#else
#define QPC_HAVE 0
static inline uint64_t qpc_cntvct(void){ return 0; }
static inline uint64_t qpc_cntfrq(void){ return 0; }
#endif
static uint64_t g_qpc_frq;

static void hf_QueryPerformanceCounter(CPU*c){ W64(ARG(0),qpc_cntvct()); RET_STD(1,1); }
static void hf_QueryPerformanceFrequency(CPU*c){ W64(ARG(0),g_qpc_frq); RET_STD(1,1); }

static uint64_t qpc_mono_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
/* returns 1 if the fast counter can be used */
static int qpc_selftest(FILE *log){
  if(!QPC_HAVE) return 0;
  uint64_t f=qpc_cntfrq(); if(f<1000000ull || f>2000000000ull){ if(log) fprintf(log,"[qpcfast] CNTFRQ %llu Hz out of range: off\n",(unsigned long long)f); return 0; }
  uint64_t n0=qpc_mono_ns(), c0=qpc_cntvct(), prev=c0;
  for(;;){ uint64_t c=qpc_cntvct(); if(c<prev){ if(log) fprintf(log,"[qpcfast] counter went backwards: off\n"); return 0; } prev=c; if(qpc_mono_ns()-n0>=2000000ull) break; }
  uint64_t n1=qpc_mono_ns(), c1=qpc_cntvct();
  double rate=(double)(c1-c0)*1e9/(double)(n1-n0)/(double)f;
  if(rate<0.98 || rate>1.02){ if(log) fprintf(log,"[qpcfast] counter rate %.3f x CNTFRQ: off\n",rate); return 0; }
  g_qpc_frq=f;
  if(log) fprintf(log,"[qpcfast] QPC = CNTVCT_EL0, QPF = %llu Hz (rate check %.4f)\n",(unsigned long long)f,rate);
  return 1;
}
