/* rhi_b1_dxvk.cpp -- B1 native renderer: the DXVK-core side (C++), behind a small C API used by rhi_b1.c.
   Work log "B1 plan". Compiled with -fvisibility=hidden and linked with --exclude-libs,ALL: none of these symbols (nor
   DXVK's) may be exported, or the bridge's own libdxvk_d3d11.so would bind to them.

   Shadow mode (B1.2-B1.5): one DxvkDevice beside the bridge. Everything is recorded into ONE DxvkContext under g_rmu,
   because the game creates resources on its game thread and draws on its render thread; the context is flushed at
   every frame end and whenever 32 MB were staged. Nothing is presented (the bridge still owns the window).
     B1.2 device + one submit per frame        B1.3 buffers / textures / locks / updates / frees
     B1.4 shaders (rhi_b1_shader.cpp)          B1.5 render targets, viewport, clear, then state + draws */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <array>   /* pp76 */
#include <map>     /* m79 */
#include <tuple>   /* m79 */
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <atomic>        /* b140 RmuLock */
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <linux/membarrier.h>   /* c5b141 RTLOCK */
#include <pthread.h>

#include "dxvk_instance.h"
#include "dxvk_shader_spirv.h"   /* b102: sb87 tile-mask compute shader (DxvkSpirvShader) */
#include "dxvk_adapter.h"
#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_staging.h"
#include "dxvk_format.h"
#include "dxvk_util.h"
#include "dxvk_shader_cache.h"
#include "../util/log/log.h"

/* pass probe (build 66 research, BSI_B1_PASSPROBE=1): which emit ends DXVK's active render pass, by source line.
   m_flags is private; the explicit-instantiation access trick reads it without changing DXVK's layout. */
namespace pp {
  template<typename Tag, typename Tag::type M> struct Rob { friend typename Tag::type get(Tag) { return M; } };
  struct FlagsTag { typedef dxvk::DxvkContextFlags dxvk::DxvkContext::*type; friend type get(FlagsTag); };
  template struct Rob<FlagsTag, &dxvk::DxvkContext::m_flags>;
  struct EndTag { typedef void (dxvk::DxvkContext::*type)(bool); friend type get(EndTag); };
  template struct Rob<EndTag, &dxvk::DxvkContext::endCurrentPass>;
  /* g4 (chat 4, build 82): the context's command list, for vkCmdDispatchBase (rhi_b1_vr.inc eye2: AO right-view crop) */
  struct CmdTag { typedef dxvk::Rc<dxvk::DxvkCommandList> dxvk::DxvkContext::*type; friend type get(CmdTag); };
  template struct Rob<CmdTag, &dxvk::DxvkContext::m_cmd>;
  struct CmdBufTag { typedef VkCommandBuffer (dxvk::DxvkCommandList::*type)() const; friend type get(CmdBufTag); };
  template struct Rob<CmdBufTag, static_cast<CmdBufTag::type>(&dxvk::DxvkCommandList::getCmdBuffer)>;
  static inline VkCommandBuffer cmdbuf(dxvk::DxvkContext *c) { return ((c->*get(CmdTag())).ptr()->*get(CmdBufTag()))(); }
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_PASSPROBE"); v = e && *e == '1'; } return v; }
  static inline bool rpa(dxvk::DxvkContext *c) { return (c->*get(FlagsTag())).test(dxvk::DxvkContextFlag::GpRenderPassActive); }
  static long ends[4096], resumed[4096]; static double resumed_mpix[4096];
  static uint64_t rtkey, passkey; static double rtmpix; static int lastend, clr; static long fresh;
  /* after an emit: a pass ended inside it (reason = line) or a pass began inside it; a pass that begins on the same
     images as the previous one is a resume, i.e. the tiler stored its attachments and now loads them again */
  static inline void note(bool a, bool b, int line) {
    if (a && !b) { ends[line & 4095]++; lastend = line & 4095; }
    else if (!a && b) { if (rtkey == passkey && !clr) { resumed[lastend]++; resumed_mpix[lastend] += rtmpix; } else if (rtkey == passkey) fresh++; passkey = rtkey; clr = 0; }
  }
}
/* S1 shadow early-out (rhi_b1_shadoweo.inc, included at the end of this file; BSI_B1_SHADOWEO) */
static bool g_eo_any;
static void b1eo_hook_draw(); static void b1eo_hook_rts(uint32_t newdsv); static void b1eo_hook_written(uint32_t res); static void b1eo_hook_released(uint32_t res);
static void b1eo_hook_ps_set(uint32_t sh); static void b1eo_hook_ps_bound_orig(); static void b1eo_hook_frame(); static void b1eo_ps_released(uint32_t id);
static void b1eo_hook_post_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic);
static void b1fx_ps_released(uint32_t id);   /* b124 FXAA split twins (defined after rhi_b1_shadoweo.inc) */
/* V4 HDAO blur twins (rhi_b1_aoblur.inc, included at the end of this file; BSI_B1_AOBLUR) */
static void b1ab_hook_frame(); static void b1ab_cs_released(uint32_t id);
/* g4 (rhi_b1_g4.inc, included at the end of this file; BSI_B1_SHCOAL): sun-shadow mask coalescing. g4_cap: the entry point
   being run defers its emits (mask target bound in an open sequence); g4_live > 0 forces live emission inside such a scope;
   g4_dup: CHECK mode, the deferred commands are also run live into a copy of the target (g4_nodup: not this one). */
static int g4_cap, g4_live, g4_dup, g4_nodup, g4_mode;
struct G4Live { G4Live() { g4_live++; } ~G4Live() { g4_live--; } };
struct G4Defer { int prev, pd; G4Defer(bool nodup = false) : prev(g4_cap), pd(g4_nodup) { g4_cap = g4_mode; if (nodup) g4_nodup = 1; } ~G4Defer() { g4_cap = prev; g4_nodup = pd; } };
#define G4_DEFER() G4Defer g4_defer_scope;
static void g4_rts(uint32_t n, const uint32_t *rtvs, uint32_t *dsv); static bool g4_clear(const dxvk::Rc<dxvk::DxvkImageView> &v, VkImageAspectFlags asp, VkClearValue cv);
static uint32_t g4_remap_dsv(uint32_t id); static const uint32_t *g4_remap_srvs(const uint32_t *ids, uint32_t n, uint32_t *tmp);
static void g4_draw_pre(); static void g4_pre_draw_live(); static void g4_wrote(uint32_t id, int how); static void g4_copy_image(uint32_t dst, uint32_t src);
static void g4_flush_if_open(const char *why); static void g4_released(uint32_t id); static void g4_touch_image(uint32_t id); static void g4_present();

#ifndef BSI_B1_NO_LOGGER   /* the Quest kit also links dxvk_core_probe.cpp, which defines it */
namespace dxvk {
  /* DXVK frontends each define their own logger; ours stays private to this binary (hidden visibility). */
  Logger Logger::s_instance("bsi_b1.log");
}
#endif

using namespace dxvk;

namespace {
  Rc<DxvkInstance> g_inst;
  Rc<DxvkDevice>   g_dev;
  Rc<DxvkContext>  g_ctx;
  Rc<DxvkContext>  g_up;          /* alias of g_ctx (kept for the resource code) */
  long g_frames, g_fail;

  struct Obj {
    int kind = 0;                   /* 1 buffer, 2 image */
    Rc<DxvkBuffer> buf; Rc<DxvkImage> img;
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t dxgi = 0;
    Rc<DxvkImageView> rtv, dsv;     /* created on first use as a render target / depth target */
    Rc<DxvkResourceAllocation> dyn; /* host-visible buffers: the storage the game's current map writes into */
    uint32_t wmask = 0;             /* diagnostics (BSI_B1_TEXLOG): mips written/copied/initialised (bit per mip, layer 0) */
    int32_t dchunk = -1; uint32_t doff = 0;   /* dyn103 (BSI_B1_DYNRING): ring chunk + offset of the current DISCARD slice, -1 = own storage */
  };
  /* b140 (chat 4 fork34): g_rmu is an inline futex lock (Drepper's three-state mutex: 0 free, 1 held, 2 held + waiters)
     instead of std::mutex. Every context entry takes it (~13.6 k lock/unlock pairs per frame at the Fair), and the bionic
     pthread_mutex_lock/unlock calls were 3.8 % of the render thread's samples (j0781a). Same mutual exclusion, only the
     synchronisation code changes: the command stream is identical. BSI_B1_FASTLOCK=0: std::mutex exactly as before. */
  /* c5b141 RTLOCK (BSI_B1_RTLOCK, default 1; 0 = the b140 lock above exactly): render-thread-biased g_rmu. The thread that
     presents (b1dx_present_inner claims it once, while it holds the inner lock) enters and leaves g_rmu with plain stores:
       enter: rt_in = 1; compiler fence; if (want, acquire) -> slow path (rt_in = 0, wake, take the inner lock)
       leave: rt_in = 0 (release); compiler fence; if (want) futex-wake the waiter
     Every other thread takes the inner lock (b140 futex / std::mutex) and then, once biased: want = 1; membarrier(PRIVATE_
     EXPEDITED); wait (futex, 1 ms timeout per round) until rt_in == 0; it clears want (release) before it unlocks the inner lock.
     membarrier is the asymmetric fence of the Dekker pair (rt_in store -> want load on the render thread; want store -> rt_in
     load on the other thread): it runs a full barrier on every running thread of the process, so either the render thread's
     store is visible to the waiter or the render thread's load sees want. The render thread holds NOTHING between entries, so
     it can block anywhere outside our entries (guest waits on the game thread) without deadlock; inside an entry it blocks
     exactly where it blocked holding the b140 lock (same critical sections). Synchronisation only: the command stream is the
     same. Not biased (b140 lock) until the first present, when membarrier is unavailable (query / register fails), or with
     BSI_B1_RTLOCK=0. Recursion on the render thread is counted (a re-lock that would have deadlocked before is a no-op). */
  static long rl_mb(int cmd) { return syscall(__NR_membarrier, cmd, 0, 0); }
  struct RmuLock {
    std::atomic<int> st{0}; std::mutex m; std::atomic<int> fast{-1};
    bool on() { int f = fast.load(std::memory_order_relaxed);
      if (__builtin_expect(f < 0, 0)) { const char *e = std::getenv("BSI_B1_FASTLOCK"); f = !(e && std::atoi(e) == 0); fast.store(f, std::memory_order_relaxed); }
      return f != 0; }
    static long fx(std::atomic<int> *a, int op, int v) { return syscall(SYS_futex, reinterpret_cast<int *>(a), op, v, nullptr, nullptr, 0); }
    void ilock() {
      if (!on()) { m.lock(); return; }
      int c = 0; if (st.compare_exchange_strong(c, 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
      if (c != 2) c = st.exchange(2, std::memory_order_acquire);
      while (c != 0) { fx(&st, FUTEX_WAIT_PRIVATE, 2); c = st.exchange(2, std::memory_order_acquire); }
    }
    void iunlock() {
      if (!on()) { m.unlock(); return; }
      if (st.fetch_sub(1, std::memory_order_release) != 1) { st.store(0, std::memory_order_release); fx(&st, FUTEX_WAKE_PRIVATE, 1); }
    }
    bool itry() { if (!on()) return m.try_lock(); int c = 0; return st.compare_exchange_strong(c, 1, std::memory_order_acquire, std::memory_order_relaxed); }
    /* c5b141 RTLOCK state. rt: the render thread's id once biased (written once, by that thread, under the inner lock).
       rt_depth / rt_inner / n_rt*: render thread only. f_want / n_f*: the foreign holder, under the inner lock. */
    alignas(64) std::atomic<uintptr_t> rt{0};   /* own cache line with the render thread's state (st / m are written by every inner lock) */
    std::atomic<int> rt_in{0}; std::atomic<int> want{0}; int rt_depth = 0, rt_inner = 0, unbias = 0; long n_rt_fast = 0, n_rt_slow = 0;
    alignas(64) int f_want = 0; long n_foreign = 0, n_fwait = 0, n_mberr = 0, n_cand = 0;
    std::atomic<uintptr_t> cand{0};   /* the presenting thread (set at the first claim attempt): its unbiased acquisitions are n_cand */
    int pol_t = 0, pol_n = 0, pol_flips = 0, pol_ok = 0, pol_maxf = 150; long w_rt = 0, w_for = 0, w_wait = 0, w_slow = 0;   /* present-thread policy state (under g_rmu) */
    static inline uintptr_t me() { return (uintptr_t)pthread_self(); }
    inline bool is_rt() { uintptr_t r = rt.load(std::memory_order_relaxed); return r != 0 && r == me(); }
    static void wake(std::atomic<int> *a) { fx(a, FUTEX_WAKE_PRIVATE, 1); }
    void rt_lock() {
      if (rt_depth++) return;
      rt_in.store(1, std::memory_order_relaxed);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      if (__builtin_expect(!want.load(std::memory_order_acquire), 1)) { n_rt_fast++; return; }
      rt_in.store(0, std::memory_order_release); wake(&rt_in);   /* a waiter may sleep on rt_in == 1 */
      ilock(); rt_inner = 1; n_rt_slow++;                         /* the foreign holder clears want before it unlocks */
    }
    void rt_unlock() {
      if (--rt_depth) return;
      if (__builtin_expect(unbias != 0, 0)) { do_unbias(); return; }
      if (rt_inner) { rt_inner = 0; iunlock(); return; }
      rt_in.store(0, std::memory_order_release);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      if (__builtin_expect(want.load(std::memory_order_relaxed) != 0, 0)) wake(&rt_in);
    }
    /* the render thread leaves biased mode for good (present, see b1dx_present_inner): from its outermost unlock on, everyone takes
       the inner lock. It takes the inner lock first (a foreign waiter is released through rt_in = 0), then clears rt under it, so a
       thread that gets the inner lock afterwards reads rt == 0 and does not wait for rt_in. */
    void do_unbias() {
      unbias = 0;
      if (!rt_inner) { rt_in.store(0, std::memory_order_release); wake(&rt_in); ilock(); }
      rt.store(0, std::memory_order_relaxed); rt_inner = 0; iunlock();
    }
    void f_bias() {   /* foreign thread, inner lock held: wait until the render thread is outside */
      if (!rt.load(std::memory_order_relaxed)) return;   /* not biased (yet): the inner lock alone, as b140 */
      want.store(1, std::memory_order_relaxed);
      if (__builtin_expect(rl_mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED) != 0, 0)) n_mberr++;   /* never seen after the registration test; counted, and present un-biases */
      if (rt_in.load(std::memory_order_acquire)) {
        n_fwait++;
        while (rt_in.load(std::memory_order_acquire)) { struct timespec ts = { 0, 1000000 };
          syscall(SYS_futex, reinterpret_cast<int *>(&rt_in), FUTEX_WAIT_PRIVATE, 1, &ts, nullptr, 0); }
      }
      f_want = 1;
    }
    void lock() { if (is_rt()) { rt_lock(); return; } ilock(); f_bias(); n_foreign++; if (cand.load(std::memory_order_relaxed) == me()) n_cand++; }
    void unlock() {
      if (is_rt()) { rt_unlock(); return; }
      if (f_want) { f_want = 0; want.store(0, std::memory_order_release); }
      iunlock();
    }
    bool try_lock() {
      if (is_rt()) {
        if (rt_depth) { rt_depth++; return true; }
        rt_in.store(1, std::memory_order_relaxed); std::atomic_signal_fence(std::memory_order_seq_cst);
        if (!want.load(std::memory_order_acquire)) { rt_depth = 1; n_rt_fast++; return true; }
        rt_in.store(0, std::memory_order_release); wake(&rt_in);
        if (itry()) { rt_depth = 1; rt_inner = 1; n_rt_slow++; return true; }
        return false; }
      if (!itry()) return false;
      if (rt.load(std::memory_order_relaxed)) {
        want.store(1, std::memory_order_relaxed); if (rl_mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED) != 0) n_mberr++;
        if (rt_in.load(std::memory_order_acquire)) { want.store(0, std::memory_order_release); iunlock(); return false; }
        f_want = 1; }
      n_foreign++; if (cand.load(std::memory_order_relaxed) == me()) n_cand++; return true;
    }
    /* b1dx_present_inner, inner lock held by the calling thread (rt == 0): that thread becomes the render thread */
    void rt_claim() {
      if (rt.load(std::memory_order_relaxed)) return;
      static int tried; if (tried) return; tried = 1;
      const char *e = std::getenv("BSI_B1_RTLOCK"); if (e && std::atoi(e) == 0) { std::fprintf(stderr, "[b1lock] RTLOCK off (BSI_B1_RTLOCK=0): b140 lock\n"); return; }
      long q = rl_mb(MEMBARRIER_CMD_QUERY);
      if (q < 0 || !(q & MEMBARRIER_CMD_PRIVATE_EXPEDITED) || rl_mb(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) != 0 || rl_mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED) != 0) {
        std::fprintf(stderr, "[b1lock] RTLOCK unavailable (membarrier query %ld): b140 lock\n", q); return; }
      rt_depth = 1; rt_inner = 1; f_want = 0;   /* this hold came through the inner lock: its unlock releases it */
      rt.store(me(), std::memory_order_relaxed); cand.store(me(), std::memory_order_relaxed); pol_ok = 1;
      { const char *m = std::getenv("BSI_B1_RTLOCK_MAXF"); if (m && std::atoi(m) > 0) pol_maxf = std::atoi(m); }
      std::fprintf(stderr, "[b1lock] RTLOCK on: render thread %#lx enters g_rmu with plain stores; other threads: inner lock + membarrier (BSI_B1_RTLOCK=0 = b140 lock)\n", (unsigned long)me());
    }
    /* every present (b1dx_present_inner, g_rmu held by the presenting thread). Every 60 presents: biased and (a membarrier failed, or the
       other threads took g_rmu more than 1/4 as often as the render thread or more than BSI_B1_RTLOCK_MAXF (default 150) times per
       frame: each of those pays a membarrier, an IPI to the running threads) -> unbias at this hold's unlock; unbiased (after such a
       switch) and the others took it less than 1/64 as often and under MAXF/4 per frame -> bias again (this hold came through the inner
       lock, as at the claim). At most one switch per 60 presents; a membarrier failure stops it for good. Synchronisation only, either way. */
    void rt_present() {
      /* fork38 b143: UE3 can restart its RenderingThread (a new 'RenderingThread' after the menu / save load). The biased owner
         claimed at the first present is then a dead or idle thread: the new render thread is 'foreign' and pays inner lock +
         membarrier on every entry (~54k/frame -> Fair at 12 fps), and unbias is only acted on by the old owner, so it never ends.
         Here the presenting thread holds the inner lock (lock() -> ilock + f_bias waited until rt_in == 0), so the old owner is
         outside every entry: drop the bias and make the presenter the candidate; the normal resume rule below re-biases to it.
         BSI_B1_RTFOLLOW=0: b142 behaviour. */
      { static int follow = -1; if (follow < 0) { const char *e = std::getenv("BSI_B1_RTFOLLOW"); follow = !(e && std::atoi(e) == 0); }
        uintptr_t r = rt.load(std::memory_order_relaxed);
        if (follow && pol_ok && r != 0 && r != me()) {
          rt.store(0, std::memory_order_relaxed); rt_depth = 0; rt_inner = 0; unbias = 0;
          cand.store(me(), std::memory_order_relaxed); n_cand = 0; pol_t = 0;
          std::fprintf(stderr, "[b1lock] RTLOCK: render thread changed (%#lx -> %#lx): bias dropped, presenter is the new candidate\n",
                       (unsigned long)r, (unsigned long)me());
          return; }
        if (follow && pol_ok && r == 0 && cand.load(std::memory_order_relaxed) != me()) {
          cand.store(me(), std::memory_order_relaxed); n_cand = 0; pol_t = 0;
          std::fprintf(stderr, "[b1lock] RTLOCK: presenting thread changed while paused (-> %#lx): new candidate\n", (unsigned long)me());
          return; } }
      if (!pol_ok || ++pol_t % 60) return;
      bool biased = rt.load(std::memory_order_relaxed) != 0;
      long rtn = biased ? n_rt_fast + n_rt_slow : n_cand, oth = n_foreign - (biased ? 0 : n_cand);
      w_rt += rtn; w_for += oth; w_wait += n_fwait; w_slow += n_rt_slow;
      if (biased && (n_mberr || 4 * oth > rtn || oth > 60L * pol_maxf)) { pol_flips++; unbias = 1;   /* (always allowed; only resuming is capped) */
        std::fprintf(stderr, "[b1lock] RTLOCK paused (%s: render thread %ld, others %ld in 60 presents)\n", n_mberr ? "membarrier failed" : "other threads take g_rmu often", rtn, oth);
        if (n_mberr) pol_flips = 1000; }
      else if (!biased && 64 * oth < rtn && 4 * oth < 60L * pol_maxf && rtn > 0 && pol_flips < 1000 && cand.load(std::memory_order_relaxed) == me()) { pol_flips++;
        rt_depth = 1; rt_inner = 1; f_want = 0; rt.store(me(), std::memory_order_relaxed);
        std::fprintf(stderr, "[b1lock] RTLOCK resumed (render thread %ld, others %ld)\n", rtn, oth); }
      if (++pol_n % 10 == 0) {
        std::fprintf(stderr, "[b1lock] RTLOCK %s: last 600 presents: render thread entries %.0f/frame (slow %ld), other threads %.1f acquisitions/frame (%ld waited for the render thread), switches %d\n",
                     biased ? "on" : "paused", w_rt / 600.0, w_slow, w_for / 600.0, w_wait, pol_flips);
        w_rt = w_for = w_wait = w_slow = 0; }
      n_rt_fast = n_rt_slow = 0; n_foreign = n_fwait = n_mberr = n_cand = 0;
    }
  };
  RmuLock g_rmu;
  std::vector<Obj> g_objs;          /* index = id - 1 */
  std::vector<uint32_t> g_freeids;
  uint64_t g_staged, g_bytes_total;
  long g_nbuf, g_ntex, g_nlive, g_rfail, g_nwrites;
  uint32_t g_badfmt[8]; int g_nbadfmt;

  /* B1.5 render state */
  uint32_t g_bb;                    /* id of the shadow back buffer */
  uint32_t g_rt[4], g_ds;           /* bound targets (ids) */
  DxvkViewport g_vp;
  long g_nclear, g_nrt;
  int  g_dump_at = -2;

  /* perf: redundant-bind filter, as DXVK's D3D11 context does (it compares with its bound state before calling into the
     DxvkContext). UE3 re-sets most state for every draw; each DxvkContext bind marks state dirty, so the next draw redid
     the descriptor / vertex-buffer / pipeline-state work. This mirrors what was last handed to g_ctx, by object id, and
     skips a bind that changes nothing. Ids are reused after release, so every release path clears the entries that name
     the released id (under g_rmu, like the binds). BC_UNK = unknown (always bind). BSI_B1_NOBINDCACHE=1 turns it off. */
  constexpr uint32_t BC_UNK = 0xffffffffu;
  struct BindCache {
    uint32_t sh[6], il, topo, rs, bs, bsmask, dss, dssref; uint32_t bf[4];
    uint32_t cb[6][14], srv[6][128], ss[6][16];
    uint32_t vb[32], vbs[32], vbo[32], ib, ibf, ibo;
    BindCache() { reset(); }
    void reset() { std::memset(this, 0xff, sizeof *this); }
    static void forget(uint32_t *a, size_t n, uint32_t id) { for (size_t i = 0; i < n; i++) if (a[i] == id) a[i] = BC_UNK; }
  } g_bc;
  long g_bc_skip, g_bc_bind;
  /* c5b141: bind epochs for the C side (b1dx_bind_epoch, defined at the end of this file): kind 0 VB, 1 IB, 2 IL, 3 shader, 4 CB,
     5 SRV, 6 sampler, 7 RS/OM state. Bumped (under g_rmu) wherever the C++ side changes or forgets what is bound for that kind
     other than through a standard bind entry; the sites carry "c5b141 epoch". Relaxed load/store pairs (no RMW): the writers
     are serialised by g_rmu, the reader is lock-free. */
  std::atomic<uint32_t> g_bind_epoch[8];
  inline void bep(int k) { g_bind_epoch[k].store(g_bind_epoch[k].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
  inline void bep_all() { for (int k = 0; k < 8; k++) bep(k); }
  inline bool bc_on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_NOBINDCACHE"); v = !(e && std::atoi(e) > 0); } return v != 0; }
  /* returns true when slot already holds id (and counts); otherwise records id */
  inline bool bc_same(uint32_t &slot, uint32_t id) { if (slot == id && bc_on()) { g_bc_skip++; return true; } slot = id; g_bc_bind++; return false; }
  /* BSI_B1_BCVERIFY=1 (check mode): every skipped bind also resolves the object it would have bound and compares it with
     the one last handed to g_ctx in that slot; a difference is counted ("verify mismatches") and bound anyway */
  struct BindPtrs { const void *sh[6], *cb[6][14], *srv[6][128], *ss[6][16], *vb[32], *ib; } g_bp;
  long g_bc_bad;
  inline bool bc_verify() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_BCVERIFY"); v = e && std::atoi(e) > 0; } return v != 0; }
  inline bool bc_keep(const void *&stored, const void *now) { if (stored == now) return true; g_bc_bad++; stored = now; return false; }   /* false: bind */

  /* perf: DISCARD renames take their storage from a context-local allocation cache (as DXVK's D3D11 context does,
     m_allocationCache) instead of the locked general allocator; only touched under g_rmu */
  DxvkLocalAllocationCache *g_acache;
}
/* S2 (copy aliasing + stencil-test drop; the s2 block before "context" below): hooks used before their definition */
static void s2_written(uint32_t id);       /* image id is about to be written by an upload / unknown partial write */
static void s2_on_release(uint32_t id);    /* image id is about to be released */
static void s2_flush_all();                /* perform every pending copy (legacy B1.5 entry points) */
namespace { namespace dc88 { static long n_other, n_present, n_upload, n_relref, n_rdbk, n_s2; static void flush_res(uint32_t res, long &ctr); static void flush_all(long &ctr); } }   /* dc88 (below) */
static void s2_view_released(uint32_t id); /* a view id was released (alias view cache) */
static void shsig_frame();                 /* perf73: BSI_B1_SHADOWSIG diagnostic, end of frame (namespace shsig below) */

/* ---- CS worker thread (BSI_B1_CS; perf73: default on, BSI_B1_CS=0 = off; Quest flat b70: render thread 23.7 -> 16.4 ms) ----
   As DXVK's own D3D11 immediate context: the threads that call us (the game's render thread, and the game thread for
   resource creation) only capture commands into DxvkCsChunks (DXVK's CS machinery, dxvk_cs.h); one DxvkCsThread
   ("dxvk-cs") owns g_ctx and executes the chunks in order, so DxvkContext recording + state commit leave the render
   thread. Rules:
   - every entry point still takes g_rmu, so the emits of both producer threads are totally ordered and the chunk being
     recorded (g_chunk) is producer state under g_rmu. The worker never takes g_rmu / g_smu (lock order: g_rmu, then the
     CS thread's own mutexes; g_rmu -> g_smu as before).
   - ids are resolved on the producer (object tables, bind filter, lazily created views) and commands capture Rc<>
     references, so a release on the producer cannot free what a queued command still uses. The worker never touches
     g_objs or the tables. The bind filter (g_bc) stays producer state: it mirrors what was handed to the stream.
   - guest memory is copied at call time on the producer (staging buffers, the renamed dynamic storage, or the chunk).
   - sync points dispatch the current chunk and wait for the worker (cs_sync): query results whose End the worker has
     not executed yet, readbacks (dumps, diagnostics), presenter resize, process exit.
   Off: emit() runs the command at once on g_ctx (the old code path, the command is inlined). */
#include "dxvk_cs.h"
#include <atomic>
#include <chrono>
namespace {
  DxvkCsChunkPool *g_cspool;        /* never freed: the worker must outlive static destructors at exit */
  DxvkCsThread    *g_csthr;         /* null = CS off */
  int              g_cs_mode;       /* BSI_B1_CS: 1 = worker thread, 2 = worker + full sync at every present (diagnostics) */
  DxvkCsChunkRef   g_chunk;         /* chunk being recorded (producer, under g_rmu) */
  uint64_t g_cs_seq;                /* sequence number of the last dispatched chunk (= DxvkCsThread's, only we dispatch) */
  long g_cs_chunks, g_cs_syncs, g_cs_waits; uint64_t g_cs_wait_us;
  std::atomic<long> g_cs_err;
  /* every flushCommandList handed to the stream, numbered, with the chunk that holds it: a query is on the GPU once the
     worker ran the first flush after its End (direct mode: as soon as the render thread passed that flush) */
  uint64_t g_nflush, g_flushseq[1024];
  /* g4: the deferred stream = CS chunks of our own, dispatched (or executed, CS off) in order at the flush */
  DxvkCsChunkPool *g4_pool; DxvkCsChunkRef g4_chunk; std::vector<DxvkCsChunkRef> g4_chunks; long g4_ncmd;
}
static inline bool cs_on() { return g_csthr != nullptr; }
/* g4: append a command to the deferred stream (caller holds g_rmu) */
static void g4_new_chunk() {
  if (!g4_pool) g4_pool = g_cspool ? g_cspool : new DxvkCsChunkPool();
  if (g4_chunk && !g4_chunk->empty()) g4_chunks.push_back(std::move(g4_chunk));
  g4_chunk = DxvkCsChunkRef(g4_pool->allocChunk(DxvkCsChunkFlag::SingleUse), g4_pool);
}
template<class T> static inline void g4_push(T& cmd) {
  if (unlikely(!g4_chunk || !g4_chunk->push(cmd))) { g4_new_chunk(); g4_chunk->push(cmd); }
  g4_ncmd++;
}
template<class M, class T> static inline void g4_push_data(T& cmd, const M *src, size_t n) {
  DxvkCsDataBlock *b = g4_chunk ? g4_chunk->pushCmd<M>(cmd, n ? n : 1) : nullptr;
  if (unlikely(!b)) { g4_new_chunk(); b = g4_chunk->pushCmd<M>(cmd, n ? n : 1); }
  if (n) std::memcpy(b->first(), src, n * sizeof(M)); else std::memset(b->first(), 0, sizeof(M));
  g4_ncmd++;
}
static uint64_t g_wt_ev, g_wt_oq, g_wt_ts, g_wt_pres, g_wt_lat, g_ts_sfalse, g_ts_reads, g_oq_vis, g_oq_zero;   /* perf diagnostics: blocked microseconds (b1dx_wait_stats) */
static inline uint64_t now_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void cs_err(const DxvkError& e) { if (g_cs_err.fetch_add(1) < 20) std::fprintf(stderr, "[b1cs] command failed on the CS thread: %s\n", e.message().c_str()); }
static void cs_dispatch() {   /* hand the chunk being recorded to the worker (no-op when empty); caller holds g_rmu */
  if (!g_csthr || g_chunk->empty()) return;
  g_cs_seq = g_csthr->dispatchChunk(std::move(g_chunk));
  g_chunk = DxvkCsChunkRef(g_cspool->allocChunk(DxvkCsChunkFlag::SingleUse), g_cspool);
  g_cs_chunks++;
}
/* wait until the worker executed chunk seq (default: everything recorded so far); caller holds g_rmu */
static void cs_sync(uint64_t seq = DxvkCsThread::SynchronizeAll) {
  if (!g_csthr) return;
  if (seq > g_cs_seq) cs_dispatch();
  if (seq > g_cs_seq) seq = g_cs_seq;
  g_cs_syncs++;
  if (seq > g_csthr->lastSequenceNumber()) {
    auto t0 = std::chrono::steady_clock::now();
    g_csthr->synchronize(seq);
    g_cs_waits++; g_cs_wait_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  }
}
/* record a context command: f(DxvkContext*). Captures must be by value (Rc<> for objects); guest data copied already. */
template<class F> static inline void emit(F&& f, int line = __builtin_LINE()) {
  if (unlikely(g4_cap && !g4_live)) {   /* g4: deferred stream (CHECK mode: a copy also runs live) */
    auto cmd = [fn = std::forward<F>(f)] (DxvkContext* c) mutable { try { fn(c); } catch (const DxvkError& e) { cs_err(e); } };
    if (g4_dup && !g4_nodup) { auto d = cmd; if (g_csthr) { if (unlikely(!g_chunk->push(d))) { cs_dispatch(); g_chunk->push(d); } } else d(g_ctx.ptr()); }
    g4_push(cmd); return; }
  if (likely(!g_csthr)) {
    if (unlikely(pp::on())) { DxvkContext *c = g_ctx.ptr(); bool a = pp::rpa(c); f(c); pp::note(a, pp::rpa(c), line); return; }
    f(g_ctx.ptr()); return; }
  auto cmd = [fn = std::forward<F>(f)] (DxvkContext* c) mutable { try { fn(c); } catch (const DxvkError& e) { cs_err(e); } };
  if (unlikely(!g_chunk->push(cmd))) { cs_dispatch(); g_chunk->push(cmd); }
}
/* the same with a copy of n trivially copyable items stored in the chunk: f(DxvkContext*, const M*, size_t) */
template<class M, class F> static inline void emit_data(const M *src, size_t n, F&& f, int line = __builtin_LINE()) {
  if (likely(!g_csthr) && !(g4_cap && !g4_live)) {   /* g4: a captured command takes the chunk path below even in direct mode */
    if (unlikely(pp::on())) { DxvkContext *c = g_ctx.ptr(); bool a = pp::rpa(c); f(c, src, n); pp::note(a, pp::rpa(c), line); return; }
    f(g_ctx.ptr(), src, n); return; }
  auto cmd = [fn = std::forward<F>(f)] (DxvkContext* c, M* d, size_t k) mutable { try { fn(c, (const M *)d, k); } catch (const DxvkError& e) { cs_err(e); } };
  if (n * sizeof(M) + 512 > DxvkCsChunkSize || n > 65535) {   /* larger than a chunk: own heap copy */
    std::vector<M> v(src, src + n);
    emit([c2 = std::move(cmd), v = std::move(v)] (DxvkContext* c) mutable { c2(c, v.data(), v.size()); });
    return;
  }
  if (unlikely(g4_cap && !g4_live)) {   /* g4: deferred stream (CHECK mode: a copy also runs live) */
    if (g4_dup && !g4_nodup) { auto d = cmd; if (g_csthr) { DxvkCsDataBlock *b = g_chunk->pushCmd<M>(d, n ? n : 1); if (unlikely(!b)) { cs_dispatch(); b = g_chunk->pushCmd<M>(d, n ? n : 1); }
        if (n) std::memcpy(b->first(), src, n * sizeof(M)); else std::memset(b->first(), 0, sizeof(M)); } else d(g_ctx.ptr(), (M *)src, n); }
    g4_push_data<M>(cmd, src, n); return; }
  DxvkCsDataBlock *b = g_chunk->pushCmd<M>(cmd, n ? n : 1);
  if (unlikely(!b)) { cs_dispatch(); b = g_chunk->pushCmd<M>(cmd, n ? n : 1); }
  if (n) std::memcpy(b->first(), src, n * sizeof(M));
  else std::memset(b->first(), 0, sizeof(M));
}
static inline void note_flush() { g_nflush++; g_flushseq[g_nflush & 1023] = g_cs_seq + 1; }   /* after emitting a flush */
/* b118: every submit signals g_subf with its sequence number, so the render thread can see whether the GPU has finished ALL submitted work
   (adaptive submits, BSI_B1_FLUSH_ADAPT) */
static Rc<sync::Fence> g_subf; static uint64_t g_subn;
static inline void ctx_flush() { G4Live g4l; /* g4: a submit is never deferred */
  if (g_subf == nullptr) g_subf = new sync::Fence(0);
  emit([f = g_subf, v = ++g_subn] (DxvkContext* c) { c->signal(f, v); c->flushCommandList(nullptr, nullptr); }); note_flush(); }
/* c5 (rhi_b1_c5.inc, included at the end of this file; BSI_B1_RAWCAP=1, off by default): the hot emits capture raw pointers
   instead of Rc<> copies and build the Rc on the worker; a released table entry's Rc goes to a CS-sequence graveyard
   (freed once the worker executed the chunk that may still reference it). c5_raw() = RAWCAP on and not in the g4
   deferred stream. Hooks: `if (c5_raw()) c5_xxx(raw...) else <old emit>`; the release paths call c5_bury_* first. */
static int g_rawcap;
static void c5_init(); static inline bool c5_raw(); static void c5_present();
static void c5_bury_obj(Obj &o); static void c5_bury(Rc<DxvkImageView> &&v); static void c5_bury(Rc<DxvkSampler> &&s); static void c5_bury_dyn(Obj &o);
static DxvkShader *c5_shader_ptr(uint32_t id); static void c5_shader_released(uint32_t id);
static void c5_bind_shader(int st, DxvkShader *p); static void c5_bind_cb(VkShaderStageFlags sb, uint32_t slot, DxvkBuffer *b);
static void c5_bind_srv(VkShaderStageFlags sb, uint32_t slot, DxvkImageView *v); static void c5_bind_sampler(VkShaderStageFlags sb, uint32_t slot, DxvkSampler *s);
static void c5_bind_vb(uint32_t slot, DxvkBuffer *b, VkDeviceSize off, VkDeviceSize len, uint32_t stride); static void c5_bind_ib(DxvkBuffer *b, VkDeviceSize off, VkDeviceSize len, VkIndexType t);
static void c5_bind_uav(uint32_t slot, uint32_t ctr, DxvkImageView *v); static void c5_bind_rts(const DxvkRenderTargets &rts); static void c5_clear_rt(DxvkImageView *v, VkImageAspectFlags asp, VkClearValue cv);
static void c5_invalidate(DxvkBuffer *b, DxvkResourceAllocation *a); static void c5_update_buffer(DxvkBuffer *b, uint32_t off, const uint8_t *data, size_t size);
static void c5_copy_buffer(DxvkBuffer *dst, VkDeviceSize doff, const Rc<DxvkBuffer> &sb, VkDeviceSize so, VkDeviceSize n);
static void c5_copy_buffer2(DxvkBuffer *dst, VkDeviceSize doff, DxvkBuffer *src, VkDeviceSize so, VkDeviceSize n);
static void c5_copy_to_image(DxvkImage *img, VkImageSubresourceLayers sl, VkOffset3D off, VkExtent3D ext, const Rc<DxvkBuffer> &sb, VkDeviceSize so, uint32_t pitch, VkFormat fmt);
static void c5_copy_image(DxvkImage *di, VkImageSubresourceLayers dl, VkOffset3D doff, DxvkImage *si, VkImageSubresourceLayers sl, VkOffset3D soff, VkExtent3D ext);
static inline bool c5_srv_plain(int st, uint32_t s);
/* dyn103 (rhi_b1_dyn.inc, included before write_buffer; BSI_B1_DYNRING=1, off by default): DISCARD renames of dynamic buffers take a
   slice of a persistent host-visible ring instead of a new allocation + invalidateBuffer; binds emit the slice. Hooks: `if (dyn103_xxx(...)) ... else <old>`. */
/* c5b141 (chat 5, render-thread CPU): switches read once in b1dx_init (c5b_init). BSI_B1_HOOKGATE (default 1): per-write / per-draw hooks
   of features that are off are not called (shrep, g4), repeated identical pixel-shader sets skip the classification hooks whose result
   cannot change (b1vr::ps_set, b1eo_hook_ps_set). BSI_B1_ENTRYFAST (default 1, only with the bind cache on and BCVERIFY off): bind entries
   skip a slot the bind cache already holds before resolving the object (the decision the bind cache takes anyway). 0 = b140. */
static int g_c5b_gate = 1, g_c5b_fast = 1; static uint32_t g_c5b_shrel;   /* g_c5b_shrel: shader releases so far (g_rmu) */
static bool c5b_eo_ps_noop(uint32_t sh);   /* end of this file: b1eo_hook_ps_set(sh) would change nothing now */
static void dyn103_init(); static inline bool dyn103_on();
static bool dyn103_discard(Obj &o, uint32_t id, const void *data, uint32_t size); static bool dyn103_bufwrite(Obj &o, uint32_t id, uint32_t off, uint32_t size, const void *data);
static bool dyn103_write(Obj &o, uint32_t off, uint32_t size, const void *data); static bool dyn103_copy_in(Obj &o, uint32_t off, const Rc<DxvkBuffer> &sb, VkDeviceSize so, uint32_t size);
static bool dyn103_copy(Obj *d, uint32_t x, Obj *s, uint32_t so, uint32_t sz); static void dyn103_release(Obj &o); static void dyn103_present(uint64_t pframe); static void dyn103_foreign();
static const uint8_t *dyn103_read_ptr(Obj &o);
static bool dyn103_bind_cb(Obj *o, uint32_t key, int st, uint32_t slot); static bool dyn103_bind_vb(Obj *o, bool ok, uint32_t s, uint32_t off, uint32_t stride); static bool dyn103_bind_ib(Obj *o, bool ok, uint32_t off, VkIndexType t);

#include "rhi_b1_tool.inc"   /* b126 Phase 0b tooling: ABAB, slack injection, game timestamp speed, visibility fingerprint (all off by default) */
#include "rhi_b1_m_prof.inc"   /* b120 module split: m79 live profiler controls + gp GPU pass timing ([b1gp]/[b1gq]/[b1gs]/[b1audit]) */
/* implicit flush state (pre_draw, b1dx_c_rts) */
static long g_draws_since_flush, g_nflush_deferred; static int g_flush_every = -1, g_flush_pending;
/* b118 adaptive submit (BSI_B1_FLUSH_ADAPT=N, default 0 = off): at a render-target change, if at least N draws are unsubmitted and the GPU has
   completed every command list we submitted (it is about to starve), submit now instead of waiting for the 1024-draw point. While the GPU
   still has work queued nothing changes, so the number of submits only rises where they shorten an idle gap. Exact (same commands, order). */
static int flush_adapt() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_FLUSH_ADAPT"); v = e && *e ? std::atoi(e) : 0; if (v < 0) v = 0; } return v; }
static long g_nflush_adapt, g_nflush_adapt_busy;
static int tiler_flush() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_TILERFLUSH");
  v = e ? std::atoi(e) > 0 : (g_dev != nullptr && g_dev->perfHints().preferRenderPassOps); } return v; }
/* process exit: let the worker finish what was dispatched before static destructors run (best effort, g_rmu bounded) */
static void cs_atexit() {
  if (!g_csthr) return;
  std::unique_lock<RmuLock> lk(g_rmu, std::try_to_lock);
  for (int i = 0; i < 200 && !lk.owns_lock(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, nullptr); (void)lk.try_lock(); }
  if (lk.owns_lock()) { try { ctx_flush(); cs_sync(); } catch (...) { } }
}

/* Our DXVK copy keeps its own shader cache instance; it must not share the bridge DXVK's cache file (two writers on one
   .dxvk.bin corrupted it: bad_alloc on the next start). Use <bridge cache dir>/b1 for the time our cache is created. */
static void own_shader_cache() {
  const char *e = std::getenv("DXVK_SHADER_CACHE_PATH"); std::string old = e ? e : "", base = old;
  if (base.empty()) { const char *x = std::getenv("XDG_CACHE_HOME"), *h = std::getenv("HOME");
    if (x && *x) base = std::string(x) + "/dxvk"; else if (h && *h) base = std::string(h) + "/.cache/dxvk"; }
  if (base.empty()) return;
  std::string mine = base + "/b1";
  ::mkdir(base.c_str(), 0755); ::mkdir(mine.c_str(), 0755);
  ::setenv("DXVK_SHADER_CACHE_PATH", mine.c_str(), 1);
  DxvkShaderCache::getInstance();
  if (e) ::setenv("DXVK_SHADER_CACHE_PATH", old.c_str(), 1); else ::unsetenv("DXVK_SHADER_CACHE_PATH");
}
/* b114 (chat 4): DXVK's graphics pipeline libraries (separately compiled VS / PS, fast-linked) stayed in use on Turnip (990 libraries vs
   645 pipelines at the Fair); full linked pipelines only measured -1.9 / -3.2 ms at the Fair and -2.4 / -1.9 ms at Gondola (j0305-j0308),
   same hitch count, pixels identical. Default: GPL off via DXVK_CONFIG unless the environment already says otherwise; BSI_B1_GPL=1 keeps it. */
static void b114_dxvk_config() {
  const char *g = std::getenv("BSI_B1_GPL"); if (g && std::atoi(g) > 0) return;
  const char *c = std::getenv("DXVK_CONFIG"); std::string v = c ? c : "";
  if (v.find("enableGraphicsPipelineLibrary") != std::string::npos) return;
  if (!v.empty()) v += ";"; v += "dxvk.enableGraphicsPipelineLibrary=False"; ::setenv("DXVK_CONFIG", v.c_str(), 1);
  std::fprintf(stderr, "[b1] DXVK_CONFIG=%s (b114: full linked pipelines; BSI_B1_GPL=1 restores pipeline libraries)\n", v.c_str()); }
extern "C" int b1dx_init(char *msg, int n) {
  try {
    own_shader_cache();
    b114_dxvk_config();
    g_inst = new DxvkInstance(0);
    if (!g_inst->adapterCount()) { std::snprintf(msg, n, "no Vulkan adapter"); return 0; }
    Rc<DxvkAdapter> adapter = g_inst->enumAdapters(0);
    g_dev = adapter->createDevice();
    g_ctx = g_dev->createContext();
    g_ctx->beginRecording(g_dev->createCommandList());
    { const char *e = std::getenv("BSI_B1_CS"); int csm = e && *e ? std::atoi(e) : 1;   /* perf73: ON by default (flat and VR); BSI_B1_CS=0 = off */
      if (csm > 0) {   /* CS worker thread (see emit) */
        g_cspool = new DxvkCsChunkPool();
        g_chunk = DxvkCsChunkRef(g_cspool->allocChunk(DxvkCsChunkFlag::SingleUse), g_cspool);
        g_csthr = new DxvkCsThread(g_dev, g_ctx); g_cs_mode = csm;
        std::atexit(cs_atexit);
        std::fprintf(stderr, "[b1cs] CS worker thread on%s: DxvkContext recording runs on thread dxvk-cs (BSI_B1_CS=0 turns it off)\n", e && *e ? "" : " (default)"); }
      else std::fprintf(stderr, "[b1cs] CS worker thread off (BSI_B1_CS=0): DxvkContext recording on the calling thread\n"); }
    c5_init();   /* c5: BSI_B1_RAWCAP (read once) */
    dyn103_init();   /* dyn103: BSI_B1_DYNRING (read once) */
    { const char *e = std::getenv("BSI_B1_HOOKGATE"); g_c5b_gate = !(e && std::atoi(e) == 0);   /* c5b141 */
      e = std::getenv("BSI_B1_ENTRYFAST"); g_c5b_fast = !(e && std::atoi(e) == 0) && bc_on() && !bc_verify();
      std::fprintf(stderr, "[b1] c5b141: HOOKGATE %d, ENTRYFAST %d, DYNRAW (ring raw captures, see [b1dyn]); RTLOCK at the first present\n", g_c5b_gate, g_c5b_fast); }
    auto info = adapter->info();
    std::snprintf(msg, n, "adapter %s (vendor %04x device %04x)", info.deviceName, info.vendorId, info.deviceId);
    return 1;
  } catch (const DxvkError& e) {
    std::snprintf(msg, n, "DxvkError: %s", e.message().c_str());
  } catch (const std::exception& e) {
    std::snprintf(msg, n, "exception: %s", e.what());
  } catch (...) {
    std::snprintf(msg, n, "unknown exception");
  }
  g_ctx = nullptr; g_dev = nullptr; g_inst = nullptr;
  return 0;
}

#include "rhi_b1_m_res.inc"   /* b120 module split: formats, buffers, textures, views of the B1 path, frame end, dumps */

/* accessor for rhi_b1_shader.cpp (C++ linkage, hidden like everything here) */
Rc<DxvkDevice> b1dx_device() { return g_dev; }

#include "rhi_b1_m_state.inc"   /* b120 module split: B1.5 COM-seam state tables, viewports, es86 eye scissor, dc88 dead-clear elision (includes rhi_b1_vr.inc, rhi_b1_sb87.inc) */

#include "rhi_b1_m_objs.inc"   /* b120 module split: object creation: views, input layouts, blend / depth-stencil / rasterizer / sampler states, releases */

#include "rhi_b1_m_s2.inc"   /* b120 module split: s2 copy aliasing / storage hand-over, b119 copy trim, s2 stencil, shsig diagnostics */

#include "rhi_b1_m_ctx.inc"   /* b120 module split: context entry points: shaders, binds, rts, clears, updates, copies, submits, draws (includes rhi_b1_shrep.inc, rhi_b1_dyn.inc) */

#include "rhi_b1_m_present.inc"   /* b120 module split: B1.6 present, queries, adapter description, diagnostics readbacks */

#include "rhi_b1_shadoweo.inc"   /* S1 shadow early-out */
#include "rhi_b1_fxsplit.h"     /* b124: FXAA split twin (DXBC patch); runtime in rhi_b1_pp76.inc fx_draw */
/* rhi_b1_shader.cpp, after a pixel shader was created (no lock held): the game's FXAA shader gets its split twin */
static thread_local bool g_fx_creating;
void b1fx_ps_created(uint32_t id, const char *name, const void *code, uint32_t len) {
  if (!b1vr::fxs_mode() || g_fx_creating || !id || !code || !name) return;
  if (!std::strstr(name, "56e172d0") && !std::strstr(name, "45fe1452")) return;
  b1fxs::Res r; try { r = b1fxs::make_twin(code, len); } catch (...) { r.ok = false; r.why = "exception"; }
  if (!r.ok) { std::fprintf(stderr, "[b1fxaa] %s (id %u): NO split twin: %s\n", name, id, r.why.c_str()); return; }
  g_fx_creating = true; uint32_t tid = b1dx_shader_create(1, r.blob.data(), (uint32_t)r.blob.size(), 1); g_fx_creating = false;
  if (!tid) { std::fprintf(stderr, "[b1fxaa] %s (id %u): split twin failed to compile\n", name, id); return; }
  { std::lock_guard<std::mutex> lk(b1vr::fxs.mu); b1vr::fxs.twin[id] = tid; }
  std::fprintf(stderr, "[b1fxaa] %s (id %u): split twin %u (mode %d; preamble %u instructions, 1 sample_l + 2 gather4; edge block of %u instructions -> discard_nz; %u -> %zu bytes)\n",
               name, id, tid, b1vr::fxs_mode(), r.n_pre, r.n_block, len, r.blob.size()); }
static void b1fx_ps_released(uint32_t id) {   /* b1dx_shader_release (g_rmu held) */
  uint32_t tid = 0; { std::lock_guard<std::mutex> lk(b1vr::fxs.mu); auto it = b1vr::fxs.twin.find(id); if (it == b1vr::fxs.twin.end()) return; tid = it->second; b1vr::fxs.twin.erase(it); }
  if (tid) b1dx_shader_release_raw(tid); }
#include "rhi_b1_aoblur.inc"     /* V4 HDAO blur twins */
#include "rhi_b1_aa.inc"         /* aa76: opt-in VR anti-aliasing (TAA / SMAA) */
#include "rhi_b1_g4.inc"         /* g4: sun-shadow mask coalescing (BSI_B1_SHCOAL) */
#include "rhi_b1_c5.inc"         /* c5: raw-pointer captures + CS-sequence graveyard (BSI_B1_RAWCAP) */
static bool dc88_ext_busy() { return eo::cur_eo != nullptr || eo::dual_pending || eo::drawprof(); }   /* dc88: S1 twin bound (may discard) / A-B copy of the target / per-draw timing */
static bool dc88_g4_on() { return g4::enabled(); }
/* c5b141 HOOKGATE: b1dx_c_shader skips b1eo_hook_ps_set(sh) when the previous call had the same sh with no shader released since
   (caller) and this returns true. Then the call is a no-op: eo::ps_set returns at sh == cur_ps; sunnl::ps_set (its only caller is the
   hook) would store cur_ps = sh (already) and cur_key = pskey[sh] (already: pskey[id] is written only when id is created, and an id is
   re-created only after a release, which the caller's release count excludes); g_eo_any would be recomputed to its current value. */
static bool c5b_eo_ps_noop(uint32_t sh) {
  return eo::cur_ps == sh && g_eo_any == (!eo::atlases.empty() || eo::cur_eo != nullptr || eo::drawprof() || shclass::mode() || sunnl::mode() || skc::on()); }
/* c5b141: bind epochs (see g_bind_epoch). With g4 (BSI_B1_SHCOAL) on, the SRV / DSV ids are remapped per atlas pass inside the entries:
   every read returns a new value, so a C-side filter never matches. */
extern "C" uint32_t b1dx_bind_epoch(int kind) {
  if (kind < 0 || kind > 7) return 0;
  if (g4::mode_env() > 0) { static std::atomic<uint32_t> n; return n.fetch_add(1, std::memory_order_relaxed); }
  return g_bind_epoch[kind].load(std::memory_order_relaxed); }
