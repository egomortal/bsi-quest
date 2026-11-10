/* BSI_FASTCS (notes/HLE_HOT.md, item H1): native Win32 critical sections that live in the guest CRITICAL_SECTION itself.
   #included by hle_fast.c (which is #included by rt_linux.c); also #included directly by regress/hle/hle_fast_test.c.

   Old path (hle_k32.c): every Enter/Leave/TryEnter hashes the guest address under ONE process-wide mutex (cs_tab_mu)
   and then locks a host PTHREAD_MUTEX_RECURSIVE.  That is 2 mutex round trips + a recursive-mutex lock per call, all
   threads serialise on cs_tab_mu, and a contended bionic mutex goes straight to a futex syscall.  GMalloc's
   FMallocThreadSafeProxy (vtable 0x1192228) does Enter+Leave around every guest appMalloc/appFree/appRealloc, so the
   GameThread, RenderingThread and XJob workers all meet on that path.

   New path: the lock state is kept in the guest's own 24-byte CRITICAL_SECTION (Win32 layout):
     +0x04 LockCount      -> lock word: 0 free, 1 held, 2 held and maybe waiters (Drepper's futex mutex)
     +0x08 RecursionCount -> recursion depth of the owner (1 when held once, like Windows)
     +0x0C OwningThread   -> owner id (the Win32 thread id for guest threads, see fcs_me), 0 when free
   Uncontended Enter = one CAS (acquire), Leave = one exchange (release); no hash, no host mutex, no syscall.
   Contended: spin BSI_FASTCS_SPIN times (default 64, with a CPU yield hint), then FUTEX_WAIT on the guest word.
   InitializeCriticalSection already zeroes the 24 bytes (all-zero = free), so a CS that the game never initialised
   (static zero memory) also works.  Misaligned CS (address not 4-aligned) keep the old pthread path, decided per
   address, so one CS never mixes the two implementations.

   Semantics kept from the old code: Leave by a thread that does not own the CS is ignored (pthread recursive unlock
   returned EPERM, which was ignored); DeleteCriticalSection is a no-op.  New but Windows-like: OwningThread and
   RecursionCount now hold real values (they were always 0).  Windows' own LockCount encoding (-1 = free) is NOT
   reproduced; nothing in the exe reads it (only kernel32 does).
   Switch: BSI_FASTCS=0 keeps the old handlers (the override is installed once, before any guest code runs).
   Stats: BSI_FASTCS_STAT=1 counts enters / contended enters / futex waits / foreign leaves (printed by hle_fast.c). */
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <errno.h>
#include <time.h>

extern __thread uint32_t t_tid;                 /* hle_more.c: Win32 thread id (0 on the main thread and on host threads) */
/* owner id: the Win32 thread id for guest threads; any thread without one (the main thread, a host thread that runs
   guest callbacks) gets a unique 0x8000xxxx id instead of the shared 0x1004, because ownership must be exact per host
   thread (the old pthread mutexes were keyed by host thread too). */
static __thread uint32_t t_fcs_id; static uint32_t g_fcs_next_id;
static inline uint32_t fcs_me(void){
  uint32_t id=t_fcs_id; if(__builtin_expect(id!=0,1)) return id;
  id=t_tid; if(!id) id=0x80000000u|(__atomic_add_fetch(&g_fcs_next_id,1,__ATOMIC_RELAXED)&0x7fffffffu);
  return t_fcs_id=id; }

static int g_fcs_spin=64;
static int g_fcs_stat;
static uint64_t g_fcs_n_enter, g_fcs_n_cont, g_fcs_n_wait, g_fcs_n_foreign, g_fcs_n_try_fail;   /* relaxed counters */
#define FCS_ST(x) do{ if(__builtin_expect(g_fcs_stat,0)) __atomic_fetch_add(&(x),1,__ATOMIC_RELAXED); }while(0)

static inline void fcs_cpu_relax(void){
#if defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}
static inline long fcs_futex(uint32_t *w, int op, uint32_t val){ return syscall(SYS_futex, w, op, val, (void*)0, (void*)0, 0); }

static inline uint32_t *fcs_lk(uint32_t a){ return (uint32_t*)MEMP(a+4); }
static inline uint32_t *fcs_rec(uint32_t a){ return (uint32_t*)MEMP(a+8); }
static inline uint32_t *fcs_own(uint32_t a){ return (uint32_t*)MEMP(a+12); }

static void fcs_lock_slow(uint32_t *lk){
  FCS_ST(g_fcs_n_cont);
  for(int i=0;i<g_fcs_spin;i++){
    fcs_cpu_relax();
    uint32_t z=0;
    if(__atomic_load_n(lk,__ATOMIC_RELAXED)==0 && __atomic_compare_exchange_n(lk,&z,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
  }
  /* Drepper "mutex 2": mark contended, sleep while the word stays 2 */
  uint32_t c=__atomic_exchange_n(lk,2,__ATOMIC_ACQUIRE);
  while(c!=0){
    FCS_ST(g_fcs_n_wait);
    fcs_futex(lk,FUTEX_WAIT_PRIVATE,2);
    c=__atomic_exchange_n(lk,2,__ATOMIC_ACQUIRE);
  }
}
/* core API (guest address of the CRITICAL_SECTION); the address must be 4-aligned */
static inline void fcs_enter(uint32_t a){
  uint32_t me=fcs_me(), *own=fcs_own(a);
  FCS_ST(g_fcs_n_enter);
  if(__atomic_load_n(own,__ATOMIC_RELAXED)==me){ (*fcs_rec(a))++; return; }   /* only this thread ever stores its own id */
  uint32_t *lk=fcs_lk(a), z=0;
  if(__builtin_expect(!__atomic_compare_exchange_n(lk,&z,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED),0)) fcs_lock_slow(lk);
  __atomic_store_n(own,me,__ATOMIC_RELAXED); *fcs_rec(a)=1;
}
static inline int fcs_try(uint32_t a){
  uint32_t me=fcs_me(), *own=fcs_own(a);
  if(__atomic_load_n(own,__ATOMIC_RELAXED)==me){ (*fcs_rec(a))++; return 1; }
  uint32_t *lk=fcs_lk(a), z=0;
  if(!__atomic_compare_exchange_n(lk,&z,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){ FCS_ST(g_fcs_n_try_fail); return 0; }
  __atomic_store_n(own,me,__ATOMIC_RELAXED); *fcs_rec(a)=1; return 1;
}
static inline void fcs_leave(uint32_t a){
  uint32_t me=fcs_me(), *own=fcs_own(a);
  if(__builtin_expect(__atomic_load_n(own,__ATOMIC_RELAXED)!=me,0)){ FCS_ST(g_fcs_n_foreign); return; }   /* not the owner: ignored (old behaviour) */
  uint32_t *rec=fcs_rec(a);
  if(*rec>1){ (*rec)--; return; }
  *rec=0; __atomic_store_n(own,0,__ATOMIC_RELAXED);
  if(__atomic_exchange_n(fcs_lk(a),0,__ATOMIC_RELEASE)==2) fcs_futex(fcs_lk(a),FUTEX_WAKE_PRIVATE,1);
}
static inline void fcs_init(uint32_t a){ memset(MEMP(a),0,24); }

/* HLE handlers (stdcall); misaligned addresses go to the saved old handlers */
typedef void (*fcs_hfn)(CPU*);
static fcs_hfn fcs_old_enter, fcs_old_leave, fcs_old_try, fcs_old_init, fcs_old_init_spin;
static void hf_EnterCriticalSection(CPU*c){ uint32_t a=ARG(0); if(__builtin_expect(a&3,0)){ fcs_old_enter(c); return; } fcs_enter(a); RET_STD(1,0); }
static void hf_LeaveCriticalSection(CPU*c){ uint32_t a=ARG(0); if(__builtin_expect(a&3,0)){ fcs_old_leave(c); return; } fcs_leave(a); RET_STD(1,0); }
static void hf_TryEnterCriticalSection(CPU*c){ uint32_t a=ARG(0); if(__builtin_expect(a&3,0)){ fcs_old_try(c); return; } RET_STD(1,fcs_try(a)); }
static void hf_InitializeCriticalSection(CPU*c){ uint32_t a=ARG(0); if(a&3){ fcs_old_init(c); return; } fcs_init(a); RET_STD(1,0); }
static void hf_InitializeCriticalSectionAndSpinCount(CPU*c){ uint32_t a=ARG(0); if(a&3){ fcs_old_init_spin(c); return; } fcs_init(a); RET_STD(2,1); }

static void fcs_config(void){
  const char *s=getenv("BSI_FASTCS_SPIN"); if(s&&*s) g_fcs_spin=atoi(s); if(g_fcs_spin<0) g_fcs_spin=0;
  s=getenv("BSI_FASTCS_STAT"); g_fcs_stat= s&&*s&&*s!='0';
}
static void fcs_report(FILE *f){
  if(!g_fcs_stat) return;
  fprintf(f,"[fastcs] enters %llu, contended %llu, futex waits %llu, try fails %llu, foreign leaves %llu\n",
    (unsigned long long)g_fcs_n_enter,(unsigned long long)g_fcs_n_cont,(unsigned long long)g_fcs_n_wait,
    (unsigned long long)g_fcs_n_try_fail,(unsigned long long)g_fcs_n_foreign);
}
