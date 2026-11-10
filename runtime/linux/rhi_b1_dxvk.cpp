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
#include <string>
#include <sys/stat.h>
#include <time.h>

#include "dxvk_instance.h"
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
/* V4 HDAO blur twins (rhi_b1_aoblur.inc, included at the end of this file; BSI_B1_AOBLUR) */
static void b1ab_hook_frame(); static void b1ab_cs_released(uint32_t id);

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
  };
  std::mutex g_rmu;
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
}
static inline bool cs_on() { return g_csthr != nullptr; }
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
  if (likely(!g_csthr)) {
    if (unlikely(pp::on())) { DxvkContext *c = g_ctx.ptr(); bool a = pp::rpa(c); f(c); pp::note(a, pp::rpa(c), line); return; }
    f(g_ctx.ptr()); return; }
  auto cmd = [fn = std::forward<F>(f)] (DxvkContext* c) mutable { try { fn(c); } catch (const DxvkError& e) { cs_err(e); } };
  if (unlikely(!g_chunk->push(cmd))) { cs_dispatch(); g_chunk->push(cmd); }
}
/* the same with a copy of n trivially copyable items stored in the chunk: f(DxvkContext*, const M*, size_t) */
template<class M, class F> static inline void emit_data(const M *src, size_t n, F&& f, int line = __builtin_LINE()) {
  if (likely(!g_csthr)) {
    if (unlikely(pp::on())) { DxvkContext *c = g_ctx.ptr(); bool a = pp::rpa(c); f(c, src, n); pp::note(a, pp::rpa(c), line); return; }
    f(g_ctx.ptr(), src, n); return; }
  auto cmd = [fn = std::forward<F>(f)] (DxvkContext* c, M* d, size_t k) mutable { try { fn(c, (const M *)d, k); } catch (const DxvkError& e) { cs_err(e); } };
  if (n * sizeof(M) + 512 > DxvkCsChunkSize || n > 65535) {   /* larger than a chunk: own heap copy */
    std::vector<M> v(src, src + n);
    emit([c2 = std::move(cmd), v = std::move(v)] (DxvkContext* c) mutable { c2(c, v.data(), v.size()); });
    return;
  }
  DxvkCsDataBlock *b = g_chunk->pushCmd<M>(cmd, n ? n : 1);
  if (unlikely(!b)) { cs_dispatch(); b = g_chunk->pushCmd<M>(cmd, n ? n : 1); }
  if (n) std::memcpy(b->first(), src, n * sizeof(M));
  else std::memset(b->first(), 0, sizeof(M));
}
static inline void note_flush() { g_nflush++; g_flushseq[g_nflush & 1023] = g_cs_seq + 1; }   /* after emitting a flush */
static inline void ctx_flush() { emit([] (DxvkContext* c) { c->flushCommandList(nullptr, nullptr); }); note_flush(); }

/* GPU pass profiler (build 66; BSI_B1_GPUPROF=0 turns it off): a GPU timestamp where a new set of render targets gets its
   first draw, where a run of dispatches starts, and at present. The timestamp goes outside any render pass (the pass that
   ends there was ending anyway, since its targets changed). Results are read 4 frames later without waiting; a frame whose
   timestamps are not ready is skipped. Every 600 presents [b1gp] lists GPU ms per frame by target set. */
namespace gp {
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_GPUPROF"); v = e ? std::atoi(e) != 0 : 1; } return v; }
  struct Slot { std::vector<Rc<DxvkQuery>> q; std::vector<int> lab; std::vector<long> dr; int n = 0; bool live = false; int tag = 0; };
  /* s1b: per-tag sums for the S1/AO on/off A/B alternation (rhi_b1_shadoweo.inc sets tag_cur before on_present) */
  static int tag_cur; static long tag_frames[8]; static double tag_tot[8]; static std::vector<double> tag_ms[8];   /* s1c71: 8 states (bit 2 = S1c off) */
  static Slot ring[8]; static int cur;
  static std::vector<std::string> labels; static std::vector<double> ms; static std::vector<long> cnt, draws;
  static long frames, skipped; static double tot_ms;
  static int curlab = -1, plab = -1; static bool in_compute; static long ndraw; static uint64_t lastkey, pkey;
  static int lab_id(const std::string &l) { for (size_t i = 0; i < labels.size(); i++) if (labels[i] == l) return (int)i;
    labels.push_back(l); ms.push_back(0); cnt.push_back(0); draws.push_back(0); return (int)labels.size() - 1; }
  static const char *fmtname(VkFormat f) {
    switch ((int)f) { case 9: return "R8"; case 16: return "RG8"; case 37: return "RGBA8"; case 43: return "RGBA8s"; case 44: return "BGRA8"; case 50: return "BGRA8s";
      case 64: return "RGB10A2"; case 76: return "R16F"; case 83: return "RG16F"; case 97: return "RGBA16F"; case 100: return "R32F"; case 103: return "RG32F";
      case 109: return "RGBA32F"; case 122: return "R11G11B10F"; case 124: return "D16"; case 126: return "D32"; case 129: return "D24S8"; case 130: return "D32S8"; }
    static char b[16]; std::snprintf(b, sizeof b, "fmt%d", (int)f); return b; }
  /* label + key of a target set (called from b1dx_c_rts) */
  static void set_targets(const DxvkRenderTargets &rts) {
    uint64_t k = 1469598103934665603ull; int nc = 0; VkExtent3D e = { 0, 0, 0 }; VkFormat f0 = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < 8; i++) if (rts.color[i].view != nullptr) { k = (k ^ (uintptr_t)rts.color[i].view->image()) * 1099511628211ull;
      if (!nc) { e = rts.color[i].view->mipLevelExtent(0); f0 = rts.color[i].view->info().format; } nc++; }
    bool d = rts.depth.view != nullptr; if (d) { k = (k ^ (uintptr_t)rts.depth.view->image()) * 1099511628211ull; if (!nc) e = rts.depth.view->mipLevelExtent(0); }
    char b[96];
    if (nc) std::snprintf(b, sizeof b, "%ux%u %s%s%s", e.width, e.height, fmtname(f0), nc > 1 ? (std::string(" x") + std::to_string(nc)).c_str() : "", d ? " +depth" : "");
    else std::snprintf(b, sizeof b, "%ux%u depth only", e.width, e.height);
    pkey = k; plab = lab_id(b); }
  static void mark(int lab) {
    Slot &s = ring[cur];
    if (s.n >= (int)s.q.size()) { s.q.push_back(g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0)); s.lab.push_back(0); s.dr.push_back(0); }
    Rc<DxvkQuery> q = s.q[s.n]; s.lab[s.n] = lab; s.dr[s.n] = ndraw; s.n++;
    emit([q] (DxvkContext* c) { if (pp::rpa(c)) (c->*get(pp::EndTag()))(false); c->writeTimestamp(q); });
  }
  static inline void on_draw() { if (in_compute || pkey != lastkey) { lastkey = pkey; curlab = plab; in_compute = false; mark(curlab); } ndraw++; }
  /* build 67: every dispatch gets its own segment, labelled by its group counts (HDAO main pass vs blurs) */
  static inline void on_compute_label(const char *l) { in_compute = true; mark(lab_id(l)); }
  static inline void on_dispatch(uint32_t x, uint32_t y, bool alt) { in_compute = true; char b[64]; std::snprintf(b, sizeof b, "compute %ux%u%s", x, y, alt ? " (AO switch)" : ""); mark(lab_id(b)); }
  /* GPU clock (Quest: kgsl sysfs, readable by apps on most builds), sampled every 30 presents */
  static double clk_sum, clk_min = 1e30, clk_max; static long clk_n; static int clk_ok = -1;
  static void clk_sample() {
    static int k; if (clk_ok == 0 || (k++ % 30)) return;
    static const char *paths[] = { "/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/kernel/gpu/gpu_clock" };
    for (const char *p : paths) { FILE *f = std::fopen(p, "r"); if (!f) continue; double v = 0; int r = std::fscanf(f, "%lf", &v); std::fclose(f);
      if (r == 1 && v > 0) { if (v > 1e5) v /= 1e6; clk_sum += v; clk_n++; if (v < clk_min) clk_min = v; if (v > clk_max) clk_max = v; clk_ok = 1; return; } }
    if (clk_ok < 0) clk_ok = 0; }
  static void resolve(Slot &s) {
    s.live = false; if (s.n < 2) return;
    std::vector<uint64_t> t(s.n);
    for (int i = 0; i < s.n; i++) { DxvkQueryData d = { }; if (s.q[i]->getData(d) != DxvkGpuQueryStatus::Available) { skipped++; return; } t[i] = d.timestamp.time; }
    double per = g_dev->properties().core.properties.limits.timestampPeriod * 1e-6;
    for (int i = 1; i < s.n; i++) { int l = s.lab[i - 1]; if (l < 0) l = lab_id("(frame start)"); ms[l] += (double)(t[i] - t[i - 1]) * per; cnt[l]++; draws[l] += s.dr[i] - s.dr[i - 1];
      std::vector<double> &tm = tag_ms[s.tag & 7];   /* s1c71: & 7 */ if (tm.size() < labels.size()) tm.resize(labels.size(), 0.0); tm[l] += (double)(t[i] - t[i - 1]) * per; }   /* s1b */
    tot_ms += (double)(t[s.n - 1] - t[0]) * per; frames++; tag_tot[s.tag & 7] += (double)(t[s.n - 1] - t[0]) * per; tag_frames[s.tag & 7]++; }   /* s1c71: & 7 */
  /* at present: close the frame, start the next slot, read the slot from 4 frames ago */
  static void on_present() {
    clk_sample(); mark(-1); ring[cur].live = true;
    cur = (cur + 1) & 7; if (ring[cur].live) { skipped++; ring[cur].live = false; } ring[cur].n = 0; ring[cur].tag = tag_cur;   /* s1b: tag */
    Slot &old = ring[(cur + 4) & 7]; if (old.live) resolve(old); }
  static void on_frame_start() { mark(curlab); in_compute = false; }
  static void report() {
    if (!frames) { if (skipped) std::fprintf(stderr, "[b1gp] GPU pass timing: no complete frame yet (%ld skipped)\n", skipped); return; }
    std::vector<int> idx; for (size_t i = 0; i < labels.size(); i++) if (cnt[i]) idx.push_back((int)i);
    std::sort(idx.begin(), idx.end(), [] (int a, int b) { return ms[a] > ms[b]; });
    std::string t; char b[160];
    for (size_t j = 0; j < idx.size() && j < 14; j++) { int i = idx[j];
      std::snprintf(b, sizeof b, " | %s: %.2f ms (%.1f passes, %.0f draws)", labels[i].c_str(), ms[i] / frames, cnt[i] / (double)frames, draws[i] / (double)frames); t += b; }
    char ck[96] = ""; if (clk_n) std::snprintf(ck, sizeof ck, ", GPU clock %.0f MHz (min %.0f, max %.0f)", clk_sum / clk_n, clk_min, clk_max);
    else if (clk_ok == 0) std::snprintf(ck, sizeof ck, ", GPU clock not readable");
    std::fprintf(stderr, "[b1gp] GPU ms/frame by render target (%ld frames timed, %ld skipped%s): total %.2f ms%s\n", frames, skipped, ck, tot_ms / frames, t.c_str());
    clk_sum = 0; clk_n = 0; clk_min = 1e30; clk_max = 0;
    for (size_t i = 0; i < labels.size(); i++) { ms[i] = 0; cnt[i] = 0; draws[i] = 0; } frames = 0; skipped = 0; tot_ms = 0; }
}
/* implicit flush state (pre_draw, b1dx_c_rts) */
static long g_draws_since_flush, g_nflush_deferred; static int g_flush_every = -1, g_flush_pending;
static int tiler_flush() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_TILERFLUSH");
  v = e ? std::atoi(e) > 0 : (g_dev != nullptr && g_dev->perfHints().preferRenderPassOps); } return v; }
/* process exit: let the worker finish what was dispatched before static destructors run (best effort, g_rmu bounded) */
static void cs_atexit() {
  if (!g_csthr) return;
  std::unique_lock<std::mutex> lk(g_rmu, std::try_to_lock);
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
extern "C" int b1dx_init(char *msg, int n) {
  try {
    own_shader_cache();
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

/* DXGI_FORMAT -> VkFormat for the formats a UE3 D3D11 RHI uses (typeless formats take their UNORM/FLOAT/depth view) */
static VkFormat dxgi2vk(uint32_t f, bool depth) {
  switch (f) {
    case 2:  return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 9: case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 11: return VK_FORMAT_R16G16B16A16_UNORM;
    case 13: return VK_FORMAT_R16G16B16A16_SNORM;
    case 15: case 16: return VK_FORMAT_R32G32_SFLOAT;
    case 19: case 20: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 23: case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 27: case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 29: return VK_FORMAT_R8G8B8A8_SRGB;
    case 33: case 34: return VK_FORMAT_R16G16_SFLOAT;
    case 35: return VK_FORMAT_R16G16_UNORM;
    case 39: case 40: case 41: return depth || f == 40 ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
    case 44: case 45: case 46: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 49: return VK_FORMAT_R8G8_UNORM;
    case 51: return VK_FORMAT_R8G8_SNORM;
    case 53: case 55: return depth || f == 55 ? VK_FORMAT_D16_UNORM : VK_FORMAT_R16_UNORM;
    case 54: return VK_FORMAT_R16_SFLOAT;
    case 56: return VK_FORMAT_R16_UNORM;
    case 60: case 61: return VK_FORMAT_R8_UNORM;
    case 65: return VK_FORMAT_R8_UNORM;
    case 70: case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 73: case 74: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 76: case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 79: case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 82: case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 87: case 90: return VK_FORMAT_B8G8R8A8_UNORM;
    case 88: return VK_FORMAT_B8G8R8A8_UNORM;
    case 91: case 93: return VK_FORMAT_B8G8R8A8_SRGB;
    case 92: return VK_FORMAT_B8G8R8A8_UNORM;
    case 115: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    default: return VK_FORMAT_UNDEFINED;
  }
}

static VkFormat srgb_of(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB:  return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:  return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK:  return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK:  return VK_FORMAT_BC3_UNORM_BLOCK;
    default: return f;
  }
}
static uint32_t new_id(Obj&& o) {
  g_nlive++;
  if (!g_freeids.empty()) { uint32_t id = g_freeids.back(); g_freeids.pop_back(); g_objs[id - 1] = std::move(o); return id; }
  g_objs.push_back(std::move(o)); return (uint32_t)g_objs.size();
}
/* new storage for a host-visible buffer (DISCARD rename); caller holds g_rmu */
static Rc<DxvkResourceAllocation> dyn_storage(DxvkBuffer &b) {
  static int off = -1; if (off < 0) { const char *e = std::getenv("BSI_B1_NOALLOCCACHE"); off = e && std::atoi(e) > 0; }
  if (off) return b.allocateStorage();
  if (!g_acache) g_acache = new DxvkLocalAllocationCache(g_dev->createAllocationCache(
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  return b.allocateStorage(g_acache);
}

static Rc<DxvkBuffer> staging(VkDeviceSize size) {
  DxvkBufferCreateInfo bi = { };
  bi.size = size ? size : 4;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
  bi.access = VK_ACCESS_TRANSFER_READ_BIT;
  bi.debugName = "B1 staging";
  return g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

/* build 65: upload staging comes from a ring of 16 MB host-visible chunks (DXVK's DxvkStagingBuffer, the allocator its
   own D3D11 UpdateSubresource uses) instead of one new DxvkBuffer per upload. Texture streaming uploads hundreds of
   small mips (each used to cost a buffer object + a memory-allocator call + a mapping on the render thread); a chunk is
   released when full and DXVK keeps it alive until the GPU has read it. BSI_B1_STAGERING=0: the old per-upload buffers. */
struct Stage { Rc<DxvkBuffer> b; VkDeviceSize o; void *p; };
static DxvkStagingBuffer *g_stage; static long g_stage_n, g_stage_big;
static Stage stage_alloc(VkDeviceSize size) {   /* caller holds g_rmu */
  static int on = -1; if (on < 0) { const char *e = std::getenv("BSI_B1_STAGERING"); on = !(e && std::atoi(e) == 0); }
  if (!size) size = 4;
  if (!on) { Rc<DxvkBuffer> b = staging(size); return { b, 0, b->mapPtr(0) }; }
  if (!g_stage) g_stage = new DxvkStagingBuffer(g_dev, 16u << 20);
  DxvkBufferSlice sl = g_stage->alloc(size); g_stage_n++; if (2 * size > (16u << 20)) g_stage_big++;
  return { sl.buffer(), (VkDeviceSize)sl.offset(), sl.mapPtr(0) };
}
static void up_ctx() {   /* one recording context for everything (uploads from any thread + rendering), always under g_rmu */
  if (g_up == nullptr) g_up = g_ctx;
}
/* build 66: on tilers an upload-triggered flush in the middle of a render pass made the GPU store the pass's targets and
   load them again (the pass probe found ~0.8 such resumes per frame on the boat, the biggest single source). Like the
   draw-count flush (build 64), it now waits for the next render-target change; a hard flush still happens at 128 MB
   staged so staging memory stays bounded. BSI_B1_UPFLUSH=0 restores the old immediate flush. */
static long g_nupflush_deferred;
static int upflush_defer() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_UPFLUSH"); v = e ? std::atoi(e) != 0 : 1; } return v; }
static void up_maybe_flush(VkDeviceSize added) {
  g_staged += added; g_bytes_total += added;
  if (g_staged >= (32u << 20)) {
    if (tiler_flush() && upflush_defer() && g_staged < (128u << 20)) { if (!g_flush_pending) { g_flush_pending = 1; g_nupflush_deferred++; } }
    else { ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }
  }
}

/* buffer: size bytes, initial data optional (host pointer, valid only during the call) */
extern "C" uint32_t b1dx_buffer_create2(uint32_t size, uint32_t bind, uint32_t usage, const void *data);
extern "C" uint32_t b1dx_buffer_create(uint32_t size, uint32_t bind, const void *data) { return b1dx_buffer_create2(size, bind, 0, data); }
extern "C" uint32_t b1dx_buffer_create2(uint32_t size, uint32_t bind, uint32_t usage, const void *data) {
  if (g_dev == nullptr || !size) return 0;
  bool hostvis = usage == 2 || (bind & 0x4u);
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    up_ctx();
    DxvkBufferCreateInfo bi = { };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | ((bind & 0x4u) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0u);
    bi.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    bi.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    bi.debugName = "B1 buffer";
    Obj o; o.kind = 1; o.dxgi = bind;
    o.buf = g_dev->createBuffer(bi, hostvis ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (data && hostvis) {
      std::memcpy(o.buf->mapPtr(0), data, size);
    } else if (data) {
      Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
      emit([b = o.buf, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(b, 0, sb, so, size); }); up_maybe_flush(size);
    } else if (!hostvis) emit([b = o.buf] (DxvkContext* c) { c->initBuffer(b); });
    g_nbuf++;
    return new_id(std::move(o));
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] buffer create %u failed: %s\n", size, e.message().c_str()); }
  return 0;
}

extern "C" int b1dx_buffer_write(uint32_t id, uint32_t offset, uint32_t size, const void *data) {
  std::lock_guard<std::mutex> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 1 || !size) return 0;
  try {
    Rc<DxvkBuffer> dst = g_objs[id - 1].buf;
    if ((uint64_t)offset + size > dst->info().size) return 0;
    Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
    emit([dst, offset, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(dst, offset, sb, so, size); }); up_maybe_flush(size); g_nwrites++;
    return 1;
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] buffer write failed: %s\n", e.message().c_str()); }
  return 0;
}

/* 2D texture / array / cube (misc 0x4 = TEXTURECUBE). sub = mips*layers {host ptr, row pitch} pairs in D3D11 order
   (layer-major: index = mip + layer * mips), or null. bind: 0x8 SRV, 0x20 RTV, 0x40 DSV, 0x80 UAV. */
static int texlog_on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_TEXLOG"); v = e ? std::atoi(e) : 0; } return v; }
extern "C" uint32_t b1dx_tex2d_create(uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, uint32_t dxgi,
                                      uint32_t bind, uint32_t misc, const void *const *subdata, const uint32_t *subpitch) {
  if (g_dev == nullptr || !w || !h) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  bool depth = (bind & 0x40u) != 0;
  VkFormat fmt = dxgi2vk(dxgi, depth);
  { static long fc[128]; static long tot; if (dxgi < 128) fc[dxgi]++;
    if ((++tot % 500) == 0) { char b[600]; int o = 0; for (int i = 0; i < 128 && o < 560; i++) if (fc[i]) o += std::snprintf(b + o, sizeof b - o, " %d:%ld", i, fc[i]);
      std::fprintf(stderr, "[b1] texture formats (DXGI:count):%s\n", b); } }
  if (fmt == VK_FORMAT_UNDEFINED) {
    bool seen = false; for (int i = 0; i < g_nbadfmt; i++) seen |= g_badfmt[i] == dxgi;
    if (!seen && g_nbadfmt < 8) { g_badfmt[g_nbadfmt++] = dxgi; std::fprintf(stderr, "[b1] texture format DXGI %u not mapped yet (skipped)\n", dxgi); }
    return 0;
  }
  try {
    up_ctx();
    if (!mips) { mips = 1; for (uint32_t m = (w > h ? w : h); m > 1; m >>= 1) mips++; }
    const DxvkFormatInfo *fi = lookupFormatInfo(fmt);
    DxvkImageCreateInfo ii = { };
    ii.type = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    ii.extent = { w, h, 1u }; ii.numLayers = layers ? layers : 1u; ii.mipLevels = mips;
    ii.flags = (misc & 0x4u) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
    VkFormat vf[2] = { fmt, srgb_of(fmt) };
    if (vf[1] != fmt && (dxgi == 27 || dxgi == 70 || dxgi == 73 || dxgi == 76 || dxgi == 90 || dxgi == 92 || dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 91)) {
      ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ii.viewFormatCount = 2; ii.viewFormats = vf; }
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
             | ((bind & 0x20u) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u) | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0u)
             | ((bind & 0x80u) ? VK_IMAGE_USAGE_STORAGE_BIT : 0u)
             | ((misc & 0x1u) && !depth ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);   /* GENERATE_MIPS: render-pass fallback of generateMipmaps */
    ii.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
              | ((bind & 0x20u) ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0u)
              | (depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0u)
              | ((bind & 0x80u) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0u);
    ii.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
              | ((bind & 0x20u) ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u)
              | (depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u)
              | ((bind & 0x80u) ? VK_ACCESS_SHADER_WRITE_BIT : 0u);
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
              : (bind & 0x20u) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii.debugName = "B1 texture";
    Obj o; o.kind = 2; o.fmt = fmt; o.dxgi = dxgi; if (bind & 0x60u) o.wmask = ~0u;
    o.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    { static int nl; if (texlog_on() && nl < texlog_on() && w >= 128) { nl++; std::fprintf(stderr, "[b1tex] create id %u %ux%u mips %u layers %u dxgi %u bind %x misc %x init %d pitch0 %u\n", (unsigned)(g_objs.size() + 1), w, h, mips, ii.numLayers, dxgi, bind, misc, subdata && subdata[0] ? 1 : 0, subdata && subpitch ? subpitch[0] : 0); } }
    if (subdata && subdata[0] && !depth) {
      VkDeviceSize total = 0;
      for (uint32_t m = 0; m < mips; m++)
        total += ii.numLayers * align(util::computeImageDataSize(fmt, o.img->mipLevelExtent(m), fi->aspectMask), CACHE_LINE_SIZE);
      Stage st = stage_alloc(total); VkDeviceSize off = 0; uint32_t nnull = 0;
      for (uint32_t m = 0; m < mips; m++) for (uint32_t l = 0; l < ii.numLayers; l++) {
        uint32_t idx = m + l * mips; VkExtent3D ext = o.img->mipLevelExtent(m);
        VkDeviceSize sz = util::computeImageDataSize(fmt, ext, fi->aspectMask);
        if (subdata[idx]) util::packImageData((uint8_t *)st.p + off, subdata[idx], subpitch[idx], 0, 0, 0, VK_IMAGE_TYPE_2D, ext, 1, fi, fi->aspectMask);
        else { std::memset((uint8_t *)st.p + off, 0, sz); nnull++; }   /* 13 Oct (build 60): the staging memory is recycled; a mip without data uploaded old bytes */
        off += align(sz, CACHE_LINE_SIZE);
      }
      if (nnull) { static int nl; if (nl++ < 10) std::fprintf(stderr, "[b1] texture %ux%u DXGI %u: %u of %u subresources without initial data (zero-filled)\n", w, h, dxgi, nnull, mips * ii.numLayers); }
      emit([img = o.img, sb = st.b, so = st.o, fmt] (DxvkContext* c) { c->uploadImage(img, sb, so, CACHE_LINE_SIZE, fmt); }); up_maybe_flush(total); o.wmask = ~0u;
    } else emit([img = o.img] (DxvkContext* c) { c->initImage(img, VK_IMAGE_LAYOUT_UNDEFINED); });
    g_ntex++;
    return new_id(std::move(o));
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] texture %ux%u DXGI %u create failed: %s\n", w, h, dxgi, e.message().c_str()); }
  return 0;
}

/* 3D texture (colour-grading LUTs): whole-mip writes only */
extern "C" uint32_t b1dx_tex3d_create(uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t dxgi, uint32_t bind,
                                      const void *const *subdata, const uint32_t *rowp, const uint32_t *slicep);
extern "C" int b1dx_tex3d_write(uint32_t id, uint32_t mip, const void *data, uint32_t rowp, uint32_t slicep) {
  std::lock_guard<std::mutex> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 2 || !data) return 0;
  try {
    Obj &o = g_objs[id - 1]; if (o.img->info().type != VK_IMAGE_TYPE_3D || mip >= o.img->info().mipLevels) return 0;
    VkExtent3D ext = o.img->mipLevelExtent(mip); const DxvkFormatInfo *fi = lookupFormatInfo(o.fmt);
    VkDeviceSize sz = util::computeImageDataSize(o.fmt, ext, fi->aspectMask);
    s2_written(id);
    Stage st = stage_alloc(sz);
    util::packImageData(st.p, data, rowp, slicep, 0, 0, VK_IMAGE_TYPE_3D, ext, 1, fi, fi->aspectMask);
    VkImageSubresourceLayers sl = { fi->aspectMask, mip, 0u, 1u };
    emit([img = o.img, sl, ext, sb = st.b, so = st.o, fmt = o.fmt] (DxvkContext* c) { c->copyBufferToImage(img, sl, VkOffset3D{ 0, 0, 0 }, ext, sb, so, 0, 0, fmt); });
    up_maybe_flush(sz); g_nwrites++;
    return 1;
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] 3D texture write failed: %s\n", e.message().c_str()); }
  return 0;
}
extern "C" uint32_t b1dx_tex3d_create(uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t dxgi, uint32_t bind,
                                      const void *const *subdata, const uint32_t *rowp, const uint32_t *slicep) {
  if (g_dev == nullptr || !w || !h || !d) return 0;
  VkFormat fmt = dxgi2vk(dxgi, false);
  if (fmt == VK_FORMAT_UNDEFINED) { std::fprintf(stderr, "[b1] 3D texture format DXGI %u not mapped yet (skipped)\n", dxgi); return 0; }
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lk(g_rmu);
    try {
      if (!mips) { mips = 1; for (uint32_t m = std::max(w, std::max(h, d)); m > 1; m >>= 1) mips++; }
      DxvkImageCreateInfo ii = { };
      ii.type = VK_IMAGE_TYPE_3D; ii.format = fmt; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT; ii.extent = { w, h, d }; ii.numLayers = 1; ii.mipLevels = mips;
      ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | ((bind & 0x20u) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
      ii.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      ii.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; ii.debugName = "B1 texture 3D";
      Obj o; o.kind = 2; o.fmt = fmt; o.dxgi = dxgi; o.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      up_ctx(); emit([img = o.img] (DxvkContext* c) { c->initImage(img, VK_IMAGE_LAYOUT_UNDEFINED); });
      g_ntex++; id = new_id(std::move(o));
    } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] 3D texture create failed: %s\n", e.message().c_str()); return 0; }
  }
  if (subdata) for (uint32_t m = 0; m < mips; m++) if (subdata[m]) b1dx_tex3d_write(id, m, subdata[m], rowp[m], slicep[m]);
  return id;
}

/* a rectangle of one mip/layer written by the game (w = h = 0: the whole mip): data with the given row pitch */
extern "C" int b1dx_tex_write_rect(uint32_t id, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   const void *data, uint32_t pitch) {
  std::lock_guard<std::mutex> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 2 || !data) return 0;
  if (g_eo_any) b1eo_hook_written(id);
  try {
    Obj &o = g_objs[id - 1];
    if (mip >= o.img->info().mipLevels || layer >= o.img->info().numLayers) return 0;
    VkExtent3D full = o.img->mipLevelExtent(mip);
    if (!w || !h) { x = y = 0; w = full.width; h = full.height; }
    if (x >= full.width || y >= full.height) return 0;
    if (x + w > full.width) w = full.width - x;
    if (y + h > full.height) h = full.height - y;
    VkExtent3D ext = { w, h, 1u };
    const DxvkFormatInfo *fi = lookupFormatInfo(o.fmt);
    uint32_t rows = (ext.height + fi->blockSize.height - 1) / fi->blockSize.height;
    uint32_t rowbytes = (ext.width + fi->blockSize.width - 1) / fi->blockSize.width * fi->elementSize;
    if (!pitch) pitch = rowbytes;
    VkDeviceSize sz = (VkDeviceSize)pitch * (rows ? rows - 1 : 0) + rowbytes;
    { static int nl; if (texlog_on() && nl < texlog_on() && full.width >= 128) { nl++; std::fprintf(stderr, "[b1tex] write id %u fmt %d mip %u layer %u rect %u,%u %ux%u of %ux%u pitch %u rowbytes %u rows %u\n", id, (int)o.fmt, mip, layer, x, y, w, h, full.width, full.height, pitch, rowbytes, rows); } }
    s2_written(id);
    Stage st = stage_alloc(sz); std::memcpy(st.p, data, sz); if (mip < 32) o.wmask |= 1u << mip;
    VkImageSubresourceLayers sl = { fi->aspectMask, mip, layer, 1u };
    emit([img = o.img, sl, x, y, ext, sb = st.b, so = st.o, pitch, fmt = o.fmt] (DxvkContext* c) { c->copyBufferToImage(img, sl, VkOffset3D{ (int32_t)x, (int32_t)y, 0 }, ext, sb, so, pitch, 0, fmt); });
    up_maybe_flush(sz); g_nwrites++;
    return 1;
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] texture write failed: %s\n", e.message().c_str()); }
  return 0;
}

extern "C" int b1dx_tex_write(uint32_t id, uint32_t mip, uint32_t layer, const void *data, uint32_t pitch) {
  return b1dx_tex_write_rect(id, mip, layer, 0, 0, 0, 0, data, pitch);
}

namespace { void b1vr_cb_forget(uint32_t id); }   /* ao74, rhi_b1_vr.inc */
extern "C" void b1dx_release(uint32_t id) {
  std::lock_guard<std::mutex> lk(g_rmu);
  if (!id || id > g_objs.size() || !g_objs[id - 1].kind) return;
  if (g_objs[id - 1].kind == 1) { BindCache::forget(&g_bc.cb[0][0], 6 * 14, id); BindCache::forget(g_bc.vb, 32, id); BindCache::forget(&g_bc.ib, 1, id); b1vr_cb_forget(id); }
  else { b1eo_hook_released(id); s2_on_release(id); }
  g_objs[id - 1] = Obj(); g_freeids.push_back(id); g_nlive--;
}

/* flush pending uploads (called at every shadow frame, render thread) */
extern "C" void b1dx_upload_flush(void) {
  std::lock_guard<std::mutex> lk(g_rmu);
  if (g_up != nullptr && g_staged) { try { ctx_flush(); } catch (...) { } g_staged = 0; }
}

extern "C" void b1dx_res_stats(char *out, int n) {
  std::lock_guard<std::mutex> lk(g_rmu);
  std::snprintf(out, n, "buffers %ld, textures %ld, live %ld, writes %ld, uploaded %.1f MB, failures %ld",
                g_nbuf, g_ntex, g_nlive, g_nwrites, g_bytes_total / 1048576.0, g_rfail);
}

/* ---------------- B1.5: render targets, viewport, clear, frame end ---------------- */
static Obj *obj(uint32_t id) { return (id && id <= g_objs.size() && g_objs[id - 1].kind) ? &g_objs[id - 1] : nullptr; }

static Rc<DxvkImageView> make_view(Obj &o, VkImageUsageFlags usage) {
  const DxvkFormatInfo *fi = lookupFormatInfo(o.fmt);
  DxvkImageViewKey vk = { };
  vk.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vk.usage = usage;
  vk.format = o.fmt;
  vk.aspects = fi->aspectMask;
  vk.mipIndex = 0u; vk.mipCount = 1u;
  vk.layerIndex = 0u; vk.layerCount = 1u;
  return o.img->createView(vk);
}
static Rc<DxvkImageView> rtv(uint32_t id) {
  Obj *o = obj(id); if (!o || o->kind != 2 || !(o->img->info().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return nullptr;
  if (o->rtv == nullptr) o->rtv = make_view(*o, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  return o->rtv;
}
static Rc<DxvkImageView> dsv(uint32_t id) {
  Obj *o = obj(id); if (!o || o->kind != 2 || !(o->img->info().usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return nullptr;
  if (o->dsv == nullptr) o->dsv = make_view(*o, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
  return o->dsv;
}

/* the shadow back buffer (RGBA8, the swapchain's format 28), created / resized with the game's viewport */
extern "C" uint32_t b1dx_backbuffer(uint32_t w, uint32_t h) {
  if (g_dev == nullptr || !w || !h) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    Obj *o = obj(g_bb);
    if (o && o->img->info().extent.width == w && o->img->info().extent.height == h) return g_bb;
    DxvkImageCreateInfo ii = { };
    ii.type = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    ii.extent = { w, h, 1u }; ii.numLayers = 1u; ii.mipLevels = 1u;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    ii.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ii.debugName = "B1 shadow back buffer";
    /* build 62: tag the back buffer as sRGB, as DXVK's D3D11 swap chain does for its back buffers (ensureImageCompatibility
       with the swap chain colour space). Untagged (MAX_ENUM), DXVK's present blitter took the picture as linear and
       sRGB-encoded it a second time on the way to the sRGB swap image: the "ashy", washed-out native picture on the Quest
       (identical frame contents, lifted blacks only after present). */
    ii.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    Obj n; n.kind = 2; n.fmt = ii.format; n.dxgi = 28;
    n.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    emit([img = n.img] (DxvkContext* c) { c->initImage(img, VK_IMAGE_LAYOUT_UNDEFINED); });
    if (o) { *o = std::move(n); } else { g_bb = new_id(std::move(n)); }
    return g_bb;
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] back buffer %ux%u failed: %s\n", w, h, e.message().c_str()); }
  return 0;
}

/* SetRenderTargets: up to 4 colour targets + depth (ids, 0 = none) */
extern "C" void b1dx_set_rts(const uint32_t *ids, uint32_t ds) {
  if (g_ctx == nullptr) return;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    s2_flush_all();
    DxvkRenderTargets rts = { };
    for (int i = 0; i < 4; i++) { g_rt[i] = ids[i]; Rc<DxvkImageView> v = rtv(ids[i]); if (v != nullptr) rts.color[i].view = v; }
    g_ds = ds; { Rc<DxvkImageView> v = dsv(ds); if (v != nullptr) rts.depth.view = v; }
    emit([rts = std::move(rts)] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(rts), 0u); });
    g_nrt++;
  } catch (const DxvkError& e) { if (g_fail++ < 10) std::fprintf(stderr, "[b1] SetRenderTargets failed: %s\n", e.message().c_str()); }
}

extern "C" void b1dx_set_viewport(uint32_t x0, uint32_t y0, float z0, uint32_t x1, uint32_t y1, float z1) {
  if (g_ctx == nullptr) return;
  std::lock_guard<std::mutex> lk(g_rmu);
  g_vp = { };
  g_vp.viewport.x = (float)x0; g_vp.viewport.y = (float)y0;
  g_vp.viewport.width = (float)(x1 > x0 ? x1 - x0 : 0); g_vp.viewport.height = (float)(y1 > y0 ? y1 - y0 : 0);
  g_vp.viewport.minDepth = z0; g_vp.viewport.maxDepth = z1;
  g_vp.scissor.offset = { (int32_t)x0, (int32_t)y0 };
  g_vp.scissor.extent = { x1 > x0 ? x1 - x0 : 0u, y1 > y0 ? y1 - y0 : 0u };
  if (g_vp.viewport.width > 0 && g_vp.viewport.height > 0) emit([vp = g_vp] (DxvkContext* c) { c->setViewports(1, &vp); });
}

/* Clear(bColor, rgba, bDepth, depth, bStencil, stencil) on the bound targets, limited to the viewport rectangle */
static void clear_view(const Rc<DxvkImageView>& v, VkImageAspectFlags aspect, VkClearValue cv) {
  VkExtent3D full = v->mipLevelExtent(0);
  int32_t x = g_vp.scissor.offset.x, y = g_vp.scissor.offset.y;
  uint32_t w = g_vp.scissor.extent.width, h = g_vp.scissor.extent.height;
  if (!w || !h || (x <= 0 && y <= 0 && x + (int32_t)w >= (int32_t)full.width && y + (int32_t)h >= (int32_t)full.height)) {
    DxvkAttachment a = { }; a.view = v;
    emit([a, aspect, cv] (DxvkContext* c) { c->clearRenderTarget(a, aspect, cv, 0u); });
  } else {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= full.width || (uint32_t)y >= full.height) return;
    if (x + w > full.width) w = full.width - x;
    if (y + h > full.height) h = full.height - y;
    emit([v, x, y, w, h, aspect, cv] (DxvkContext* c) { c->clearImageView(v, VkOffset3D{ x, y, 0 }, VkExtent3D{ w, h, 1u }, aspect, cv); });
  }
}
extern "C" void b1dx_clear(int color, const float *rgba, int depth, float d, int stencil, uint32_t st) {
  if (g_ctx == nullptr) return;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    s2_flush_all(); if (depth || stencil) s2_written(g_ds);
    if (color) for (int i = 0; i < 4; i++) { Rc<DxvkImageView> v = rtv(g_rt[i]); if (v == nullptr) continue;
      VkClearValue cv = { }; for (int k = 0; k < 4; k++) cv.color.float32[k] = rgba[k]; clear_view(v, VK_IMAGE_ASPECT_COLOR_BIT, cv); }
    if (depth || stencil) { Rc<DxvkImageView> v = dsv(g_ds);
      if (v != nullptr) { VkImageAspectFlags asp = 0;
        const DxvkFormatInfo *fi = lookupFormatInfo(v->info().format);
        if (depth) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (stencil && (fi->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        VkClearValue cv = { }; cv.depthStencil.depth = d; cv.depthStencil.stencil = st;
        if (asp) clear_view(v, asp, cv); } }
    g_nclear++;
  } catch (const DxvkError& e) { if (g_fail++ < 10) std::fprintf(stderr, "[b1] Clear failed: %s\n", e.message().c_str()); }
}

/* frame end: submit; BSI_B1_DUMP=<frame> writes the shadow back buffer of that frame to BSI_B1_DUMPDIR (default /tmp) as PPM */
static int g_dump_tag = -1; static std::string g_dump_file;
extern "C" void b1dx_dump_path(const char *path) { std::lock_guard<std::mutex> lk(g_rmu); g_dump_file = path; g_dump_tag = 0; }
extern "C" void b1dx_dump_request(int tag) { std::lock_guard<std::mutex> lk(g_rmu); g_dump_tag = tag; }
static void dump_bb(long frame) {
  Obj *o = obj(g_bb); if (!o) return;
  VkExtent3D ext = o->img->info().extent;
  VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * 4u;
  DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 readback";
  Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
  emit([rb, fmt = o->fmt, img = o->img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
  ctx_flush(); cs_sync();   /* CS: readback = sync point */
  g_dev->waitForIdle();
  const char *dir = std::getenv("BSI_B1_DUMPDIR"); char path[512];
#ifdef __ANDROID__
  if (!dir) dir = "/sdcard/BSI";
#endif
  if (!g_dump_file.empty()) { std::snprintf(path, sizeof path, "%s", g_dump_file.c_str()); g_dump_file.clear(); }
  else if (g_dump_tag >= 0) std::snprintf(path, sizeof path, "%s/b1_f%d.ppm", dir ? dir : "/tmp", g_dump_tag);
  else std::snprintf(path, sizeof path, "%s/b1_frame%ld.ppm", dir ? dir : "/tmp", frame);
  FILE *f = std::fopen(path, "wb"); if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", ext.width, ext.height);
  const uint8_t *p = (const uint8_t *)rb->mapPtr(0);
  for (VkDeviceSize i = 0; i < (VkDeviceSize)ext.width * ext.height; i++) std::fwrite(p + 4 * i, 1, 3, f);
  std::fclose(f);
  if (std::getenv("BSI_B1_DUMPALPHA")) { std::string ap = std::string(path) + ".alpha.pgm"; FILE *fa = std::fopen(ap.c_str(), "wb");   /* vr69 diagnostics */
    if (fa) { std::fprintf(fa, "P5\n%u %u\n255\n", ext.width, ext.height);
              for (VkDeviceSize i = 0; i < (VkDeviceSize)ext.width * ext.height; i++) std::fwrite(p + 4 * i + 3, 1, 1, fa); std::fclose(fa); } }
  std::fprintf(stderr, "[b1] dumped shadow frame %ld (%ux%u) to %s\n", frame, ext.width, ext.height, path);
}
/* ext72: legendary loot beam (linux/legendary.c projects the pickup with each view's ViewProjection in FDrawSceneCommand and hands
   back screen rectangles); drawn as colour clears into the shadow back buffer right before it is dumped / presented (g_rmu held) */
extern "C" int ext72_beam_rects(uint32_t w, uint32_t h, int32_t *xywh, float *rgb, int max) __attribute__((weak));
extern "C" int ext_menu_rects(uint32_t w, uint32_t h, int32_t *xywh, float *rgb, int max) __attribute__((weak));   /* ext72: mod menu (ext_menu.c) */
static void ext72_beam() {
  if (!ext72_beam_rects && !ext_menu_rects) return;
  Obj *o = obj(g_bb); if (!o) return;
  VkExtent3D e = o->img->info().extent; static int32_t r[4 * 2048]; static float col[3 * 2048];
  int n = ext72_beam_rects ? ext72_beam_rects(e.width, e.height, r, col, 96) : 0; if (n < 0) n = 0;
  if (ext_menu_rects) { int m = ext_menu_rects(e.width, e.height, r + 4 * n, col + 3 * n, 2048 - n); if (m > 0) n += m; }
  if (n <= 0) return;
  Rc<DxvkImageView> v = rtv(g_bb); if (v == nullptr) return;
  for (int i = 0; i < n; i++) {
    VkClearValue cv = { }; cv.color.float32[0] = col[3 * i]; cv.color.float32[1] = col[3 * i + 1]; cv.color.float32[2] = col[3 * i + 2]; cv.color.float32[3] = 1.0f;
    VkOffset3D off = { r[4 * i], r[4 * i + 1], 0 }; VkExtent3D ex = { (uint32_t)r[4 * i + 2], (uint32_t)r[4 * i + 3], 1u };
    emit([v, off, ex, cv] (DxvkContext* c) { c->clearImageView(v, off, ex, VK_IMAGE_ASPECT_COLOR_BIT, cv); });
  }
}
static void b1vr_shadow_frame();   /* vr70, rhi_b1_vr.inc: BSI_B1_VRSHADOW=1 (cloud exactness check of the VR renderer pieces) */
static bool b1aa_owns_fxaa();   /* aa76 (rhi_b1_aa.inc): true while aa76 replaces the game's FXAA draw (stereo, aa != off); merge note: per-eye FXAA work must stand aside then */
static bool g_aa_live;   /* aa76: false with aa=off (the default) and in flat play */
static void b1aa_vbwrite(uint32_t id, uint32_t off, uint32_t size, const void *data);   /* aa76: small dynamic vertex data (post-process quads) */
extern "C" int b1dx_end_frame(void) {
  if (g_ctx == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    if (g_dump_at == -2) { const char *e = std::getenv("BSI_B1_DUMP"); g_dump_at = e ? std::atoi(e) : -1; }
    g_frames++; b1eo_hook_frame(); b1ab_hook_frame();
    shsig_frame();   /* perf73 diagnostic */ b1vr_shadow_frame();
    ext72_beam();   /* ext72 */
    if (g_dump_tag >= 0 || (g_dump_at > 0 && g_frames % g_dump_at == 0 && g_frames / g_dump_at <= 4)) { dump_bb(g_frames); g_dump_tag = -1; }   /* frames N..4N, or with the bridge's dump */
    else { ctx_flush(); cs_dispatch(); }
    g_staged = 0;
    return 1;
  } catch (const DxvkError& e) { if (g_fail++ < 5) std::fprintf(stderr, "[b1] frame failed: %s\n", e.message().c_str()); }
  return 0;
}
extern "C" long b1dx_frames(void) { return g_frames; }
extern "C" void b1dx_render_stats(char *out, int n) {
  std::snprintf(out, n, "SetRenderTargets %ld, clears %ld, render errors %ld", g_nrt, g_nclear, g_fail);
}

/* accessor for rhi_b1_shader.cpp (C++ linkage, hidden like everything here) */
Rc<DxvkDevice> b1dx_device() { return g_dev; }

/* =====================================================================================================================
   B1.5 at the COM seam: a small D3D11 subset implemented directly on the DxvkContext.
   Fed by rhi_b1_com.c from the game's own D3D11 calls (the ~45 device/context methods the UE3 RHI uses), so it needs no
   knowledge of engine structures. Shadow mode: the bridge still renders and presents; we render the same stream
   offscreen. B1.6 swaps the roles. Objects are ids (per type tables); every entry point takes g_rmu.
   ===================================================================================================================== */
extern Rc<DxvkShader> b1dx_shader_get(uint32_t id);   /* rhi_b1_shader.cpp */
extern Rc<DxvkShader> b1dx_shader_alt(uint32_t id, uint32_t scale[3]);   /* rhi_b1_shader.cpp: AO switch alternate for this Dispatch */
extern Rc<DxvkShader> b1dx_shader_alt2(uint32_t id, uint32_t scale[3], bool *gtao);   /* ao74: + is it a GTAO build */
static void query_stats(long *pend, long *flush);
static void ts_stats(double *avg_ms, long *frames);

namespace {
  template<class T> struct Table {
    struct E { bool live = false; T t; };
    std::vector<E> v; std::vector<uint32_t> fr;
    uint32_t add(T&& t) {
      if (!fr.empty()) { uint32_t id = fr.back(); fr.pop_back(); v[id - 1].t = std::move(t); v[id - 1].live = true; return id; }
      v.push_back(E()); v.back().t = std::move(t); v.back().live = true; return (uint32_t)v.size(); }
    T *get(uint32_t id) { return (id && id <= v.size() && v[id - 1].live) ? &v[id - 1].t : nullptr; }
    void del(uint32_t id) { if (get(id)) { v[id - 1].t = T(); v[id - 1].live = false; fr.push_back(id); } }
  };
  struct View { int type = 0; uint32_t res = 0; Rc<DxvkImageView> iv; };   /* 1 SRV, 2 RTV, 3 DSV, 4 UAV */
  struct IL   { DxvkVertexInput in[64]; uint32_t na = 0, nb = 0; };
  struct BS   { DxvkBlendMode m[8]; bool a2c = false; DxvkLogicOpState lo; };
  struct DSS  { DxvkDepthStencilState s; };
  struct RS   { DxvkRasterizerState s; DxvkDepthBias b = { }; bool scissor = false; };
  struct SS   { Rc<DxvkSampler> s; };
  Table<View> g_views; Table<IL> g_ils; Table<BS> g_bss; Table<DSS> g_dsss; Table<RS> g_rss; Table<SS> g_sss;

  struct CState {
    float vp[16][6]; uint32_t nvp = 0; int32_t sc[16][4]; uint32_t nsc = 0; bool vpdirty = true;
    uint32_t rs = 0, bs = 0, dss = 0, sref = 0, mask = 0xffffffffu; float bf[4] = { 1, 1, 1, 1 };
    uint32_t rtv[8] = { }, dsv = 0;
  } g_cs;
  long g_ndraw, g_ncerr, g_nupd, g_nmapw, g_ncopy;
  uint32_t g_cur_cs;   /* compute shader id bound now (AO switch lookup per Dispatch) */
  uint64_t g_cbytes;

  inline uint32_t sidx(int st) { static const uint32_t m[6] = { 1, 0, 2, 3, 4, 5 }; return st >= 0 && st < 6 ? m[st] : 0; }   /* VS PS GS HS DS CS -> DXVK stage index */
  inline VkShaderStageFlags sbit(int st) {
    static const VkShaderStageFlags m[6] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
      VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT };
    return m[st]; }
  void cerr(const char *what, const DxvkError& e) { if (g_ncerr++ < 20) std::fprintf(stderr, "[b1] %s failed: %s\n", what, e.message().c_str()); }

  /* DXGI -> VkFormat for vertex attributes and views (typed formats only) */
  VkFormat dxgi_typed(uint32_t f) {
    switch (f) {
      case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;  case 3: return VK_FORMAT_R32G32B32A32_UINT;  case 4: return VK_FORMAT_R32G32B32A32_SINT;
      case 6: return VK_FORMAT_R32G32B32_SFLOAT;     case 7: return VK_FORMAT_R32G32B32_UINT;     case 8: return VK_FORMAT_R32G32B32_SINT;
      case 10: return VK_FORMAT_R16G16B16A16_SFLOAT; case 11: return VK_FORMAT_R16G16B16A16_UNORM; case 12: return VK_FORMAT_R16G16B16A16_UINT;
      case 13: return VK_FORMAT_R16G16B16A16_SNORM;  case 14: return VK_FORMAT_R16G16B16A16_SINT;
      case 16: return VK_FORMAT_R32G32_SFLOAT;       case 17: return VK_FORMAT_R32G32_UINT;       case 18: return VK_FORMAT_R32G32_SINT;
      case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 25: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
      case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
      case 28: return VK_FORMAT_R8G8B8A8_UNORM; case 29: return VK_FORMAT_R8G8B8A8_SRGB; case 30: return VK_FORMAT_R8G8B8A8_UINT;
      case 31: return VK_FORMAT_R8G8B8A8_SNORM; case 32: return VK_FORMAT_R8G8B8A8_SINT;
      case 34: return VK_FORMAT_R16G16_SFLOAT;  case 35: return VK_FORMAT_R16G16_UNORM; case 36: return VK_FORMAT_R16G16_UINT;
      case 37: return VK_FORMAT_R16G16_SNORM;   case 38: return VK_FORMAT_R16G16_SINT;
      case 41: return VK_FORMAT_R32_SFLOAT;     case 42: return VK_FORMAT_R32_UINT;     case 43: return VK_FORMAT_R32_SINT;
      case 49: return VK_FORMAT_R8G8_UNORM;     case 50: return VK_FORMAT_R8G8_UINT;    case 51: return VK_FORMAT_R8G8_SNORM; case 52: return VK_FORMAT_R8G8_SINT;
      case 54: return VK_FORMAT_R16_SFLOAT;     case 56: return VK_FORMAT_R16_UNORM;    case 57: return VK_FORMAT_R16_UINT;
      case 58: return VK_FORMAT_R16_SNORM;      case 59: return VK_FORMAT_R16_SINT;
      case 61: return VK_FORMAT_R8_UNORM;       case 62: return VK_FORMAT_R8_UINT;      case 63: return VK_FORMAT_R8_SNORM;   case 64: return VK_FORMAT_R8_SINT;
      case 65: return VK_FORMAT_R8_UNORM;
      case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
      case 74: return VK_FORMAT_BC2_UNORM_BLOCK;      case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
      case 77: return VK_FORMAT_BC3_UNORM_BLOCK;      case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
      case 80: return VK_FORMAT_BC4_UNORM_BLOCK;      case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
      case 83: return VK_FORMAT_BC5_UNORM_BLOCK;      case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
      case 87: case 88: return VK_FORMAT_B8G8R8A8_UNORM; case 91: case 93: return VK_FORMAT_B8G8R8A8_SRGB;
      case 115: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
      default: return VK_FORMAT_UNDEFINED;
    }
  }
  bool is_depth(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM; }

  VkBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
      case 1: return VK_BLEND_FACTOR_ZERO;  case 2: return VK_BLEND_FACTOR_ONE;
      case 3: return VK_BLEND_FACTOR_SRC_COLOR;  case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case 5: return VK_BLEND_FACTOR_SRC_ALPHA;  case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case 7: return VK_BLEND_FACTOR_DST_ALPHA;  case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case 9: return VK_BLEND_FACTOR_DST_COLOR;  case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      case 14: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
      case 15: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      case 16: return VK_BLEND_FACTOR_SRC1_COLOR; case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
      case 18: return VK_BLEND_FACTOR_SRC1_ALPHA; case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
      default: return VK_BLEND_FACTOR_ZERO;
    }
  }
  VkBlendOp blend_op(uint32_t o) {
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
  }
  VkCompareOp cmp_op(uint32_t c) {
    switch (c) { case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS; case 3: return VK_COMPARE_OP_EQUAL; case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
                 case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL; case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS; }
  }
  VkStencilOp stencil_op(uint32_t o) {
    switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                 case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
  }
  VkSamplerAddressMode addr_mode(uint32_t a) {
    switch (a) { case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
  }

  DxvkRasterizerState default_rs() {
    DxvkRasterizerState s = { };
    s.setPolygonMode(VK_POLYGON_MODE_FILL); s.setCullMode(VK_CULL_MODE_BACK_BIT); s.setFrontFace(VK_FRONT_FACE_CLOCKWISE);
    s.setDepthClip(true); s.setConservativeMode(VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT); s.setSampleCount(0);
    s.setFlatShading(false); s.setLineMode(VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT);
    return s;
  }
  /* ---- vr69: VR stereo pieces of the native renderer (inert unless the VR game side reports the stereo layout) ---- */
#include "rhi_b1_vr.inc"
  void apply_viewports() {
    RS *rs = g_rss.get(g_cs.rs); bool sc = rs && rs->scissor;
    DxvkViewport vps[16]; uint32_t n = g_cs.nvp;
    if (!n) { DxvkViewport v = { }; emit([v] (DxvkContext* c) { c->setViewports(1, &v); }); g_cs.vpdirty = false; return; }
    for (uint32_t i = 0; i < n; i++) {
      const float *v = g_cs.vp[i]; DxvkViewport &d = vps[i]; d = { };
      d.viewport.x = v[0]; d.viewport.y = v[3] + v[1]; d.viewport.width = v[2]; d.viewport.height = -v[3];
      d.viewport.minDepth = v[4]; d.viewport.maxDepth = v[5];
      if (!sc) { d.scissor.offset = { 0, 0 }; d.scissor.extent = { 16384u, 16384u }; }
      else if (i < g_cs.nsc) { const int32_t *r = g_cs.sc[i];
        d.scissor.offset = { r[0] > 0 ? r[0] : 0, r[1] > 0 ? r[1] : 0 };
        d.scissor.extent = { (uint32_t)(r[2] > r[0] ? r[2] - (r[0] > 0 ? r[0] : 0) : 0), (uint32_t)(r[3] > r[1] ? r[3] - (r[1] > 0 ? r[1] : 0) : 0) }; }
    }
    emit_data<DxvkViewport>(vps, n, [] (DxvkContext* c, const DxvkViewport *v, size_t k) { c->setViewports((uint32_t)k, v); }); g_cs.vpdirty = false;
  }
  Obj *robj(uint32_t id) { return (id && id <= g_objs.size() && g_objs[id - 1].kind) ? &g_objs[id - 1] : nullptr; }
}

/* vr70, cloud only: BSI_B1_VRSHADOW=1 runs the per-Present VR step (layout latch, tag check, AO crop setup) at b1's frame end in
   shadow mode (BSI_RHI=b1: the D3D11 bridge presents the real frame and b1 renders the same command stream next to it), so the
   AO crop can be compared pixel by pixel against the bridge's uncut frame (vr70/README.md). Off: nothing happens here. */
static void b1aa_frame_end(); static void b1aa_frame_begin();   /* aa76 (rhi_b1_aa.inc) */
static void b1vr_shadow_frame() {
  static int on = -1; if (on < 0) { const char *e = std::getenv("BSI_B1_VRSHADOW"); on = e && std::atoi(e) > 0; }
  if (!on) return;
  Obj *bb = obj(g_bb);
  b1aa_frame_end();   /* aa76: also in the cloud exactness check */
  if (bb) b1vr::present((uint32_t)g_br_presents, bb->img->info().extent.width, bb->img->info().extent.height);
  b1aa_frame_begin();   /* aa76 */
}

/* ---- creation (desc = host pointer to the game's D3D11 desc struct; 32-bit layout, all fields 4 bytes) ---- */
/* SRV/RTV/DSV: dim = D3D11 ViewDimension of that view kind; a,b,c,d = the union fields; fmt 0 = resource format */
extern "C" uint32_t b1dx_view_create(int type, uint32_t res, uint32_t fmt, uint32_t dim, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int hasdesc) {
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  Obj *o = robj(res); if (!o || o->kind != 2) return 0;   /* buffer views: not used by the UE3 RHI's graphics path */
  try {
    const DxvkImageCreateInfo &ii = o->img->info();
    DxvkImageViewKey k = { };
    VkFormat vf = fmt ? dxgi_typed(fmt) : o->fmt;
    VkFormat imgf = o->fmt;
    VkImageAspectFlags asp = lookupFormatInfo(imgf)->aspectMask;
    if (is_depth(imgf)) { vf = imgf; asp = (fmt == 47 || fmt == 22) ? VK_IMAGE_ASPECT_STENCIL_BIT : (type == 3 ? asp : VK_IMAGE_ASPECT_DEPTH_BIT); }
    else if (vf == VK_FORMAT_UNDEFINED || (vf != imgf && !(ii.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))) vf = imgf;
    k.format = vf; k.aspects = asp;
    if (type == 1) {   /* component swizzles as DXVK's DXGI format table: A8 lives in R8, X8 channels read as 1 */
      uint32_t f = fmt ? fmt : o->dxgi;
      if (f == 65) k.packedSwizzle = DxvkImageViewKey::packSwizzle({ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R });
      else if (f == 88 || f == 93) k.packedSwizzle = DxvkImageViewKey::packSwizzle({ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE });
    }
    bool cube = (ii.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
    uint32_t mips = ii.mipLevels, layers = ii.numLayers;
    k.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    k.mipIndex = 0; k.mipCount = mips; k.layerIndex = 0; k.layerCount = layers;
    if (type == 4) {   /* UAV (13 Oct, build 60): typed texture UAVs, as DXVK's D3D11UnorderedAccessView. dim: 2 1D, 3 1D array,
                          4 2D, 5 2D array, 8 3D; a = MipSlice, b/c = FirstArraySlice/ArraySize (or FirstWSlice/WSize) */
      if (!(ii.usage & VK_IMAGE_USAGE_STORAGE_BIT)) return 0;
      k.usage = VK_IMAGE_USAGE_STORAGE_BIT; k.layout = VK_IMAGE_LAYOUT_GENERAL; k.mipCount = 1; k.packedSwizzle = 0;
      if (vf != imgf && !(ii.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) k.format = imgf;
      if (srgb_of(k.format) != k.format && (k.format == VK_FORMAT_R8G8B8A8_SRGB || k.format == VK_FORMAT_B8G8R8A8_SRGB)) k.format = srgb_of(k.format);   /* no sRGB storage */
      switch (hasdesc ? dim : 4u) {
        case 5: k.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; k.mipIndex = a; k.layerIndex = b; k.layerCount = c; break;
        case 8: k.viewType = VK_IMAGE_VIEW_TYPE_3D; k.mipIndex = a; k.layerIndex = 0; k.layerCount = 1; break;
        default: k.viewType = layers > 1 && !hasdesc ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; k.mipIndex = hasdesc ? a : 0; if (hasdesc || layers <= 1) { k.layerIndex = 0; k.layerCount = 1; } break;
      }
      if (ii.type == VK_IMAGE_TYPE_3D) { k.viewType = VK_IMAGE_VIEW_TYPE_3D; k.layerIndex = 0; k.layerCount = 1; }
      if (k.mipIndex >= mips) return 0;
      View v; v.type = 4; v.res = res; v.iv = o->img->createView(k);
      return g_views.add(std::move(v));
    }
    if (type == 1) {
      k.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      if (!hasdesc) { if (cube) k.viewType = VK_IMAGE_VIEW_TYPE_CUBE; if (ii.type == VK_IMAGE_TYPE_3D) k.viewType = VK_IMAGE_VIEW_TYPE_3D; }
      else switch (dim) {
        case 4: k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.mipIndex = a; k.mipCount = b; k.layerCount = 1; break;
        case 5: k.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; k.mipIndex = a; k.mipCount = b; k.layerIndex = c; k.layerCount = d; break;
        case 9: k.viewType = VK_IMAGE_VIEW_TYPE_CUBE; k.mipIndex = a; k.mipCount = b; k.layerIndex = 0; k.layerCount = 6; break;
        case 10: k.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; k.mipIndex = a; k.mipCount = b; k.layerIndex = c; k.layerCount = d * 6; break;
        case 8: k.viewType = VK_IMAGE_VIEW_TYPE_3D; k.mipIndex = a; k.mipCount = b; k.layerCount = 1; break;
        default: break;
      }
      if (k.mipCount == 0xffu || (uint32_t)k.mipIndex + k.mipCount > mips || !k.mipCount) k.mipCount = mips - k.mipIndex;
    } else {
      k.usage = type == 2 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      k.layout = type == 2 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      k.mipCount = 1;
      bool arr = type == 2 ? dim == 5 : dim == 4;
      bool tex2d = type == 2 ? dim == 4 : dim == 3;
      if (hasdesc && tex2d) { k.mipIndex = a; k.layerCount = 1; k.viewType = VK_IMAGE_VIEW_TYPE_2D; }
      else if (hasdesc && arr) { k.mipIndex = a; k.layerIndex = b; k.layerCount = c; k.viewType = c > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; }
      else { k.layerCount = 1; k.viewType = VK_IMAGE_VIEW_TYPE_2D; }
      if (!(ii.usage & k.usage)) return 0;
    }
    View v; v.type = type; v.res = res; v.iv = o->img->createView(k);
    return g_views.add(std::move(v));
  } catch (const DxvkError& e) { cerr("view create", e); }
  return 0;
}

/* elems: n x {semantic register (already resolved by the caller: -1 = not in the signature), format, slot, offset, perinstance, step} */
extern "C" uint32_t b1dx_il_create(const int32_t *elems, uint32_t n) {
  if (g_dev == nullptr || n > 32) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  DxvkVertexAttribute attrs[32]; DxvkVertexBinding binds[32]; uint32_t na = 0, defined = 0, used = 0;
  DxvkVertexBinding bl[32] = { };
  for (uint32_t i = 0; i < n; i++) {
    const int32_t *e = elems + 6 * i;
    VkFormat f = dxgi_typed((uint32_t)e[1]); if (f == VK_FORMAT_UNDEFINED) { if (g_ncerr++ < 20) std::fprintf(stderr, "[b1] input layout: vertex format DXGI %d not mapped\n", e[1]); return 0; }
    uint32_t slot = (uint32_t)e[2] & 31u, off = (uint32_t)e[3];
    const DxvkFormatInfo *fi = lookupFormatInfo(f);
    if (off == 0xffffffffu) {   /* D3D11_APPEND_ALIGNED_ELEMENT */
      off = 0;
      for (uint32_t j = na; j-- > 0; ) if (attrs[j].binding == slot) { off = align(attrs[j].offset + lookupFormatInfo(attrs[j].format)->elementSize, std::min<VkDeviceSize>(fi->elementSize, 4)); break; }
    }
    DxvkVertexBinding b = { }; b.binding = slot; b.divisor = (uint32_t)e[5];
    b.inputRate = e[4] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    b.extent = e[0] >= 0 ? off + fi->elementSize : 0u;
    if (defined & (1u << slot)) bl[slot].extent = std::max(bl[slot].extent, b.extent); else { bl[slot] = b; defined |= 1u << slot; }
    if (e[0] >= 0) { DxvkVertexAttribute a = { }; a.location = (uint32_t)e[0]; a.binding = slot; a.format = f; a.offset = off; attrs[na++] = a; used |= 1u << slot; }
  }
  IL il; uint32_t nb = 0;
  for (uint32_t s = 0; s < 32; s++) if (used & (1u << s)) binds[nb++] = bl[s];
  for (uint32_t i = 0; i < na; i++) il.in[i] = DxvkVertexInput(attrs[i]);
  for (uint32_t i = 0; i < nb; i++) il.in[na + i] = DxvkVertexInput(binds[i]);
  il.na = na; il.nb = nb;
  return g_ils.add(std::move(il));
}

extern "C" uint32_t b1dx_blend_create(const uint32_t *d) {   /* D3D11_BLEND_DESC: a2c, independent, RT[8] x 8 dwords */
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  BS bs; bs.a2c = d[0] != 0;
  for (int i = 0; i < 8; i++) {
    const uint32_t *r = d + 2 + 8 * (d[1] ? i : 0);
    DxvkBlendMode m = { };
    m.setBlendEnable(r[0] != 0);
    m.setColorOp(blend_factor(r[1], false), blend_factor(r[2], false), blend_op(r[3]));
    m.setAlphaOp(blend_factor(r[4], true), blend_factor(r[5], true), blend_op(r[6]));
    m.setWriteMask(r[7] & 0xfu);
    m.normalize(); bs.m[i] = m;
  }
  bs.lo.setLogicOp(false, VK_LOGIC_OP_NO_OP);
  return g_bss.add(std::move(bs));
}
extern "C" uint32_t b1dx_dss_create(const uint32_t *d) {   /* D3D11_DEPTH_STENCIL_DESC (52 bytes) */
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  DSS o; DxvkDepthStencilState &s = o.s;
  s.setDepthTest(d[0] != 0); s.setDepthWrite(d[1] == 1); s.setStencilTest(d[3] != 0); s.setDepthCompareOp(cmp_op(d[2]));
  uint8_t rmask = ((const uint8_t *)d)[16], wmask = ((const uint8_t *)d)[17];
  for (int face = 0; face < 2; face++) {
    const uint32_t *f = d + 5 + 4 * face; DxvkStencilOp op = { };
    if (d[3]) { op.setFailOp(stencil_op(f[0])); op.setDepthFailOp(stencil_op(f[1])); op.setPassOp(stencil_op(f[2])); op.setCompareOp(cmp_op(f[3]));
                op.setCompareMask(rmask); op.setWriteMask(wmask); }
    if (face == 0) s.setStencilOpFront(op); else s.setStencilOpBack(op);
  }
  s.normalize();
  return g_dsss.add(std::move(o));
}
extern "C" uint32_t b1dx_rs_create(const uint32_t *d) {   /* D3D11_RASTERIZER_DESC (40 bytes) */
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  RS o; o.s = default_rs();
  o.s.setPolygonMode(d[0] == 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
  o.s.setCullMode(d[1] == 2 ? VK_CULL_MODE_FRONT_BIT : d[1] == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
  o.s.setFrontFace(d[2] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
  o.s.setDepthClip(d[6] != 0);
  float f4, f5; std::memcpy(&f4, &d[4], 4); std::memcpy(&f5, &d[5], 4);
  o.b.depthBiasConstant = (float)(int32_t)d[3]; o.b.depthBiasClamp = f4; o.b.depthBiasSlope = f5;
  o.scissor = d[7] != 0;
  return g_rss.add(std::move(o));
}
extern "C" uint32_t b1dx_sampler_create(const uint32_t *d) {   /* D3D11_SAMPLER_DESC (52 bytes) */
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    DxvkSamplerKey k = { };
    uint32_t fb = d[0];
    VkFilter mn = (fb & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, mg = (fb & 0x04) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    k.setFilter(mn, mg, (fb & 0x01) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
    float bias, mnl, mxl; std::memcpy(&bias, &d[4], 4); std::memcpy(&mnl, &d[11], 4); std::memcpy(&mxl, &d[12], 4);
    { /* UE3's mip fade-in makes a new sampler desc per frame step (continuous MinLOD): ~1800 distinct descs in a boat run,
         and DXVK keeps at most 2048 live samplers, then hands out its default (CLAMP, no aniso) = flat, smeared surfaces.
         Quantise MinLOD to 1/BSI_B1_LODQ mip (default 8; 0 = off) so equal-looking samplers share one DXVK sampler. */
      static int q = -1; if (q < 0) { const char *e = std::getenv("BSI_B1_LODQ"); q = e ? std::atoi(e) : 8; }
      if (q > 0 && mnl > 0.0f && mnl < 1000.0f) mnl = std::floor(mnl * (float)q + 0.5f) / (float)q; }
    k.setLodRange(mnl, mxl, bias);
    { static int na = -1; if (na < 0) { const char *e = std::getenv("BSI_B1_NOANISO"); na = e && std::atoi(e) > 0; } k.setAniso((fb & 0x40) && !na ? d[5] : 0u); }
    k.setAddressModes(addr_mode(d[1]), addr_mode(d[2]), addr_mode(d[3]));
    k.setDepthCompare((fb & 0x180) == 0x80, cmp_op(d[6]));
    VkClearColorValue bc; std::memcpy(bc.float32, &d[7], 16); k.setBorderColor(bc);
    SS o; o.s = g_dev->createSampler(k);
    { uint32_t live = g_dev->getSamplerStats().liveCount; static int lv;   /* DXVK pool: 2048 live samplers, then its default (clamp) sampler */
      if ((live >= 1536 && lv == 0) || (live >= 2040 && lv == 1)) { lv++; std::fprintf(stderr, "[b1] DXVK sampler pool: %u live samplers of 2048%s\n", live, live >= 2040 ? " - FULL, new samplers fall back to DXVK's default (clamp)" : ""); } }
    return g_sss.add(std::move(o));
  } catch (const DxvkError& e) { cerr("sampler create", e); }
  return 0;
}
/* kind: 3 view, 4 input layout, 5 blend, 6 depth-stencil, 7 rasterizer, 8 sampler */
extern "C" void b1dx_cobj_release(int kind, uint32_t id) {
  std::lock_guard<std::mutex> lk(g_rmu);
  switch (kind) { case 3: g_views.del(id); BindCache::forget(&g_bc.srv[0][0], 6 * 128, id); s2_view_released(id); break;
                  case 4: g_ils.del(id); BindCache::forget(&g_bc.il, 1, id); break;
                  case 5: g_bss.del(id); BindCache::forget(&g_bc.bs, 1, id); break;
                  case 6: g_dsss.del(id); BindCache::forget(&g_bc.dss, 1, id); break;
                  case 7: g_rss.del(id); BindCache::forget(&g_bc.rs, 1, id); break;
                  case 8: g_sss.del(id); BindCache::forget(&g_bc.ss[0][0], 6 * 16, id); break; }
}
/* shaders: the id table lives in rhi_b1_shader.cpp (g_smu); released here under g_rmu first (lock order g_rmu -> g_smu,
   as b1dx_c_shader) so a reused id can never meet a stale bind-cache entry */
extern "C" void b1dx_shader_release_raw(uint32_t id);   /* rhi_b1_shader.cpp */
extern "C" void b1dx_shader_release(uint32_t id) {
  std::lock_guard<std::mutex> lk(g_rmu);
  BindCache::forget(g_bc.sh, 6, id);
  b1eo_ps_released(id);
  b1ab_cs_released(id);
  b1dx_shader_release_raw(id);
}

/* =====================================================================================================================
   S2 (14 Oct; Fable notes 1 and 6): exact savings in this immediate-mode backend. Every entry point below runs under g_rmu.
   (1) BSI_B1_COPYALIAS (copy aliasing / lazy copy). UE3 copies every render target into a separate "resolve" texture before
       sampling it (whole subresource, same format and size). Such a copy is not executed at once but recorded as pending
       (invariant: dst's content == src's current content). While pending, every SRV bind of a view of dst binds the same
       view of src (alias), so readers see the same bytes. The copy is executed for real at the last moment it is still
       exact, i.e. before anything could make the alias observable:
         - before src or dst is written: a draw with it bound as render/depth target, a clear, a copy into it, an upload,
           a UAV bind, a mip generation;
         - before dst is read other than through an SRV (copy source, mip generation, alias view not creatable);
         - before src is released;
       and then every slot holding an alias is rebound to dst's own view. A write that overwrites ALL of dst (whole copy,
       whole clear) instead drops the pending copy: it was never observable. Mode 2 adds the storage hand-over: when src is
       about to be overwritten completely (whole clear / whole copy into it), dst takes src's current backing storage
       (DxvkContext::invalidateImage) and src gets new storage, so the copy is never executed - dst then holds exactly the
       bytes the copy would have produced (only between images with the same create info, dst's usage/stages/access a
       subset of src's, so the release barrier DXVK recorded for src's last write covers dst's readers).
       Eligible: colour formats, 1 mip, 1 layer, same VkFormat and size, no box or a box covering the image, neither image
       bound as UAV, not the back buffer. A copy is never skipped on a guess: a dead copy (never read before its next
       overwrite) can only be dropped when that overwrite comes before any write to src - no look-ahead.
   (2) BSI_B1_LRZFIX (stencil test drop). Per depth-stencil image: the value of the whole stencil plane when known - set by a
       stencil clear of the whole image (1 mip, 1 layer), unknown after any draw with it bound whose state can write
       stencil (normalized write mask != 0), a partial clear, a copy or upload into it. A draw whose stencil test writes
       nothing and is true for that value on both faces gets the same state with the stencil test disabled: no fragment
       was rejected by that test and nothing was written, so the output is identical; Turnip keeps LRZ writes on for it
       (tu6_calculate_lrz_state turns LRZ writes off for any stencil compare other than ALWAYS/NEVER).
   (3) LOAD_OP_DONT_CARE: not implemented - see s2_fix/README.md (needs a coverage proof or look-ahead).
   ===================================================================================================================== */
namespace s2 {
  static int env_int(const char *n, int def) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : def; }
  static int ca_mode() { static int v = -1; if (v < 0) v = env_int("BSI_B1_COPYALIAS", 2); return v; }
  static int lrz_mode() { static int v = -1; if (v < 0) v = env_int("BSI_B1_LRZFIX", 1); return v; }
  static int ca_log() { static int v = -1; if (v < 0) v = env_int("BSI_B1_COPYALIAS_LOG", 0); return v; }
  /* BSI_B1_S2LOG=<frame> (or BSI_B1_LRZFIX_LOG): copy and stencil events of that frame and the next, repeats folded */
  static long lrz_logf() { static long v = -2; if (v == -2) v = env_int("BSI_B1_S2LOG", env_int("BSI_B1_LRZFIX_LOG", -1)); return v; }
  static std::string lg_last; static long lg_rep;
  static void lg(const char *t) {
    if (lg_last == t) { lg_rep++; return; }
    if (lg_rep) std::fprintf(stderr, "[b1s2log]   (x%ld)\n", lg_rep + 1);
    lg_rep = 0; lg_last = t; std::fprintf(stderr, "[b1s2log] f%ld %s\n", g_frames, t); }
  static inline bool lg_on() { long f = lrz_logf(); return f >= 0 && (g_frames == f || g_frames == f + 1); }
  struct Pend { uint32_t dst, src; Rc<DxvkImage> di, si; };
  static std::vector<Pend> pend;                 /* pending copies (at most one per dst) */
  static uint32_t al_dst[6][128];                /* stage/slot: dst id whose alias view is bound there (0 = none) */
  static Rc<DxvkImageView> al_real[6][128];      /* ... and the view the game bound there (rebound when the alias ends) */
  static long n_al;                              /* slots holding an alias */
  struct AV { Rc<DxvkImageView> orig, alias; };
  static std::unordered_map<uint32_t, AV> avc;   /* alias view (same key on src's image) by SRV view id */
  static uint32_t uav_res[64];                   /* resource of the UAV bound in each compute slot */
  static long c_def, c_alias, c_wr, c_rd, c_drop, c_swap, c_noswap, c_inel;
  /* stencil tracking */
  static std::vector<int16_t> stv;               /* by resource id: -1 unknown, else the value of the whole stencil plane */
  static DxvkDepthStencilState lrz_ds; static uint32_t lrz_ref; static bool lrz_have, lrz_mod;
  static long c_lrz_drop, c_lrz_sw, c_st_clear, c_st_inval;
  /* by resource id: a clear may still be queued inside DxvkContext (deferred clears are applied when the image is next
     used; a storage hand-over would move such a clear to the wrong storage). Set by clears, cleared by a draw with the
     image bound as target or a real copy from/into it (both flush DXVK's deferred clear). */
  static std::vector<uint8_t> clrp;
  static inline bool clrp_get(uint32_t id) { return id < clrp.size() && clrp[id]; }
  static inline void clrp_set(uint32_t id, bool v) { if (id >= clrp.size()) { if (!v) return; clrp.resize(id + 64, 0); } clrp[id] = v; }
  static inline int stv_get(uint32_t id) { return id && id < stv.size() ? stv[id] : -1; }
  static inline void stv_set(uint32_t id, int v) {
    if (!id) return;
    if (id >= stv.size()) { if (v < 0) return; stv.resize(id + 64, -1); }
    if (v < 0 && stv[id] >= 0) c_st_inval++;
    stv[id] = (int16_t)v; }
}
static int s2_find_dst(uint32_t id) { for (size_t i = 0; i < s2::pend.size(); i++) if (s2::pend[i].dst == id) return (int)i; return -1; }
static void s2_bind_slot(int st, uint32_t slot, Rc<DxvkImageView> iv) {
  emit([sb = sbit(st), s = sidx(st) * 128 + slot, iv = std::move(iv)] (DxvkContext* c) mutable { c->bindResourceImageView(sb, s, std::move(iv)); });
}
static void s2_restore_slots(uint32_t dst) {   /* the alias of dst ends: every slot holding it gets dst's own view back */
  using namespace s2;
  if (!n_al) return;
  for (int st = 0; st < 6; st++) for (uint32_t s = 0; s < 128; s++) if (al_dst[st][s] == dst) {
    Rc<DxvkImageView> iv = std::move(al_real[st][s]); al_real[st][s] = nullptr; al_dst[st][s] = 0; n_al--;
    s2_bind_slot(st, s, std::move(iv)); }
}
static bool s2_can_swap(const s2::Pend &p) {
  const DxvkImageCreateInfo &a = p.si->info(), &b = p.di->info();
  if (a.type != b.type || a.format != b.format || a.flags != b.flags || a.sampleCount != b.sampleCount || a.tiling != b.tiling
   || a.extent.width != b.extent.width || a.extent.height != b.extent.height || a.extent.depth != b.extent.depth
   || a.numLayers != b.numLayers || a.mipLevels != b.mipLevels || a.shared || b.shared || a.viewFormatCount != b.viewFormatCount) return false;
  for (uint32_t i = 0; i < a.viewFormatCount; i++) if (a.viewFormats[i] != b.viewFormats[i]) return false;
  if (s2::clrp_get(p.src) || s2::clrp_get(p.dst)) return false;   /* a deferred clear might still target the old storage */
  return !(b.usage & ~a.usage) && !(b.stages & ~a.stages) && !(b.access & ~a.access);
}
/* end pending copy i. how: 0 execute (a write follows), 1 execute (a read needs it), 2 drop (dst is overwritten
   completely), 3 hand src's storage over to dst (src is overwritten completely) */
static void s2_end(int i, int how) {
  using namespace s2;
  Pend p = pend[i]; pend.erase(pend.begin() + i);
  if (how <= 1) {
    VkImageSubresourceLayers l = { lookupFormatInfo(p.si->info().format)->aspectMask, 0u, 0u, 1u }; VkExtent3D e = p.si->mipLevelExtent(0);
    emit([di = p.di, si = p.si, l, e] (DxvkContext* c) { c->copyImage(di, l, VkOffset3D{ 0, 0, 0 }, si, l, VkOffset3D{ 0, 0, 0 }, e); });
    (how ? c_rd : c_wr)++; clrp_set(p.src, false); clrp_set(p.dst, false);
  } else if (how == 2) c_drop++;
  else {
    emit([di = p.di, si = p.si] (DxvkContext* c) {
      Rc<DxvkResourceAllocation> cur = si->storage();
      c->invalidateImage(si, si->allocateStorage(), VK_IMAGE_LAYOUT_UNDEFINED);   /* src: new storage (about to be overwritten) */
      c->invalidateImage(di, std::move(cur), si->info().layout); });              /* dst: src's bytes, in src's default layout */
    c_swap++;
  }
  if (lg_on()) { char t[120]; VkExtent3D e = p.si->info().extent; std::snprintf(t, sizeof t, "copy %u -> %u (%ux%u fmt %d) %s", p.src, p.dst, e.width, e.height, (int)p.si->info().format,
      how == 0 ? "executed before a write" : how == 1 ? "executed for a read" : how == 2 ? "dropped (dst overwritten)" : "storage hand-over (not executed)"); lg(t); }
  if (ca_log() && (c_wr + c_rd + c_drop + c_swap) <= (long)ca_log())
    std::fprintf(stderr, "[s2] pending copy %u -> %u %s\n", p.src, p.dst, how == 0 ? "executed before a write" : how == 1 ? "executed for a read" : how == 2 ? "dropped (dst overwritten)" : "replaced by a storage hand-over");
  s2_restore_slots(p.dst);
}
/* image id is about to be written; full = every texel of every aspect is overwritten without being read */
static void s2_before_write(uint32_t id, bool full) {
  using namespace s2;
  if (pend.empty() || !id) return;
  int i = s2_find_dst(id); if (i >= 0) s2_end(i, full ? 2 : 0);
  bool src = false; for (const Pend &p : pend) if (p.src == id) { src = true; break; }
  if (!src) return;
  uint32_t sw = 0;
  if (full && ca_mode() >= 2) { for (const Pend &p : pend) if (p.src == id && s2_can_swap(p)) { sw = p.dst; break; } if (!sw) c_noswap++; }
  for (size_t k = 0; k < pend.size(); ) { if (pend[k].src == id && pend[k].dst != sw) s2_end((int)k, 0); else k++; }   /* these read src first */
  if (sw) s2_end(s2_find_dst(sw), 3);
}
static inline void s2_before_read(uint32_t id) { if (!s2::pend.empty()) { int i = s2_find_dst(id); if (i >= 0) s2_end(i, 1); } }
static void s2_written(uint32_t id) { s2::stv_set(id, -1); s2_before_write(id, false); }
static void s2_flush_all() { while (!s2::pend.empty()) s2_end(0, 0); }
static void s2_on_release(uint32_t id) {
  using namespace s2;
  stv_set(id, -1); clrp_set(id, false);
  if (pend.empty()) return;
  int i = s2_find_dst(id); if (i >= 0) s2_end(i, 2);   /* dst gone: nobody can read the copy */
  for (size_t k = 0; k < pend.size(); ) { if (pend[k].src == id) s2_end((int)k, 0); else k++; }
}
static void s2_view_released(uint32_t id) { if (!s2::avc.empty()) s2::avc.erase(id); }
/* the view to bind for SRV view vid (v, real view iv) in stage st / slot s: an alias of src while v's resource is a pending dst */
static Rc<DxvkImageView> s2_alias_for(int st, uint32_t s, uint32_t vid, View *v, const Rc<DxvkImageView> &iv) {
  using namespace s2;
  if (pend.empty() || !v || v->type != 1) return nullptr;
  int i = s2_find_dst(v->res); if (i < 0) return nullptr;
  const Pend &p = pend[i];
  Rc<DxvkImageView> a;
  auto it = avc.find(vid);
  if (it != avc.end() && it->second.orig == iv && it->second.alias->image() == p.si.ptr()) a = it->second.alias;
  else {
    DxvkImageViewKey k = iv->info();
    if ((k.format == p.si->info().format || p.si->isViewCompatible(k.format)) && (p.si->info().usage & k.usage) == k.usage) {
      a = p.si->createView(k); avc[vid] = { iv, a }; }
  }
  if (a == nullptr) { s2_end(i, 1); return nullptr; }   /* cannot alias: execute the copy now */
  al_dst[st][s] = v->res; al_real[st][s] = iv; n_al++; c_alias++;
  return a;
}
/* hook in b1dx_c_srvs: the view actually bound for (st, slot) */
static Rc<DxvkImageView> s2_srv(int st, uint32_t s, uint32_t vid, View *v, Rc<DxvkImageView> iv) {
  using namespace s2;
  if (st < 0 || st > 5 || s >= 128) return iv;
  if (al_dst[st][s]) { al_dst[st][s] = 0; al_real[st][s] = nullptr; n_al--; }
  Rc<DxvkImageView> a = s2_alias_for(st, s, vid, v, iv);
  return a != nullptr ? a : iv;
}
/* hook in b1dx_c_copy_region (images): true = recorded as pending, the caller skips the copy */
static bool s2_copy(uint32_t dst, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint32_t src, uint32_t ssub, const uint32_t *box) {
  using namespace s2;
  stv_set(dst, -1);
  if (!ca_mode()) return false;
  Obj *d = robj(dst), *s = robj(src);
  s2_before_read(src);
  bool el = d && s && d->kind == 2 && s->kind == 2 && dst != src && dst != g_bb && src != g_bb && pend.size() < 64;
  if (el) {
    const DxvkImageCreateInfo &di = d->img->info(), &si = s->img->info();
    el = di.type == VK_IMAGE_TYPE_2D && si.type == VK_IMAGE_TYPE_2D && di.mipLevels == 1 && si.mipLevels == 1 && di.numLayers == 1 && si.numLayers == 1
      && d->fmt == s->fmt && di.format == si.format && di.sampleCount == si.sampleCount
      && !(lookupFormatInfo(d->fmt)->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
      && di.extent.width == si.extent.width && di.extent.height == si.extent.height
      && !dsub && !ssub && !x && !y && !z
      && (!box || (!box[0] && !box[1] && !box[2] && box[3] == si.extent.width && box[4] == si.extent.height && box[5] == 1u));
    for (int k = 0; k < 64 && el; k++) if (uav_res[k] && (uav_res[k] == dst || uav_res[k] == src)) el = false;
  }
  if (!el) { c_inel++; s2_before_write(dst, false);
    if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "copy %u -> %u not eligible (executed)", src, dst); lg(t); }
    return false; }
  s2_before_write(dst, true);                       /* a pending copy into dst is dropped; dst as a pending source hands over / executes */
  pend.push_back({ dst, src, d->img, s->img }); c_def++;
  if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "copy %u -> %u deferred", src, dst); lg(t); }
  if (ca_log() && c_def <= ca_log()) {
    const DxvkImageCreateInfo &di = d->img->info(), &si = s->img->info();
    std::fprintf(stderr, "[s2] copy %u -> %u (%ux%u fmt %d) deferred; src usage %x stages %x layout %d, dst usage %x stages %x layout %d, hand-over %s\n", src, dst,
      si.extent.width, si.extent.height, (int)si.format, si.usage, si.stages, (int)si.layout, di.usage, di.stages, (int)di.layout, s2_can_swap(pend.back()) ? "possible" : "not possible");
  }
  /* slots that hold a view of dst now read src */
  for (int st = 0; st < 6; st++) for (uint32_t k = 0; k < 128; k++) {
    uint32_t vid = g_bc.srv[st][k]; if (!vid || vid == BC_UNK) continue;
    View *v = g_views.get(vid); if (!v || v->type != 1 || v->res != dst) continue;
    if (al_dst[st][k]) { al_dst[st][k] = 0; al_real[st][k] = nullptr; n_al--; }
    Rc<DxvkImageView> a = s2_alias_for(st, k, vid, v, v->iv);
    if (a == nullptr) break;   /* the copy was executed instead */
    s2_bind_slot(st, k, std::move(a));
    if (s2_find_dst(dst) < 0) break;
  }
  return true;
}
static void s2_uav(uint32_t slot, View *v) {   /* hook in b1dx_c_uavs: compute may write the resource from now on */
  if (slot >= 64) return;
  s2::uav_res[slot] = v ? v->res : 0;
  if (v) s2_written(v->res);
}
static void s2_clear_rtv(View *v) {
  Obj *o = robj(v->res); bool whole = o && o->kind == 2 && o->img->info().mipLevels == 1 && o->img->info().numLayers == 1;
  s2_before_write(v->res, whole); if (s2::ca_mode() >= 2) s2::clrp_set(v->res, true);
}
static void s2_clear_dsv(View *v, VkImageAspectFlags asp, uint32_t stencil) {
  using namespace s2;
  s2_before_write(v->res, false); if (ca_mode() >= 2) clrp_set(v->res, true);
  if (!(asp & VK_IMAGE_ASPECT_STENCIL_BIT)) return;   /* depth-only clear: the stencil plane keeps its value */
  Obj *o = robj(v->res); bool whole = o && o->kind == 2 && o->img->info().mipLevels == 1 && o->img->info().numLayers == 1;
  if (lrz_mode() && whole) { stv_set(v->res, (int)(stencil & 0xffu)); c_st_clear++; } else stv_set(v->res, -1);
  if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "clear res %u aspects %x stencil %u whole %d", v->res, asp, stencil & 0xffu, whole); lg(t); }
}
/* hook in b1dx_c_dss, after the state was emitted */
static void s2_dss(const DxvkDepthStencilState &ds, uint32_t ref) { s2::lrz_ds = ds; s2::lrz_ref = ref & 0xffu; s2::lrz_have = true; s2::lrz_mod = false; }
static inline bool s2_stencil_true(const DxvkStencilOp &op, uint32_t ref, uint32_t val) {
  uint32_t m = op.compareMask(), r = ref & m, s = val & m;   /* Vulkan / D3D11: (ref & mask) OP (stored & mask) */
  switch (op.compareOp()) {
    case VK_COMPARE_OP_ALWAYS: return true;            case VK_COMPARE_OP_EQUAL: return r == s;
    case VK_COMPARE_OP_NOT_EQUAL: return r != s;       case VK_COMPARE_OP_LESS: return r < s;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return r <= s;   case VK_COMPARE_OP_GREATER: return r > s;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return r >= s; default: return false; }
}
/* hook in pre_draw: pending copies whose src/dst is a bound target are executed; stencil test drop */
static void s2_pre_draw() {
  using namespace s2;
  if (!pend.empty() || !clrp.empty()) {
    for (int i = 0; i < 8; i++) if (g_cs.rtv[i]) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) { s2_before_write(v->res, false); clrp_set(v->res, false); } }
    if (g_cs.dsv) { View *v = g_views.get(g_cs.dsv); if (v && v->type == 3) { s2_before_write(v->res, false); clrp_set(v->res, false); } }
  }
  if (!lrz_have || !lrz_ds.stencilTest()) return;   /* (a state without stencil test is never modified: lrz_mod is false) */
  uint32_t r = 0; if (g_cs.dsv) { View *v = g_views.get(g_cs.dsv); if (v && v->type == 3) r = v->res; }
  DxvkStencilOp f = lrz_ds.stencilOpFront(), b = lrz_ds.stencilOpBack();
  bool writes = f.writeMask() || b.writeMask();      /* normalized: write mask != 0 iff some op != KEEP */
  if (writes) stv_set(r, -1);
  int val = stv_get(r);
  bool triv = lrz_mode() && !writes && r && val >= 0 && s2_stencil_true(f, lrz_ref, (uint32_t)val) && s2_stencil_true(b, lrz_ref, (uint32_t)val);
  if (triv != lrz_mod) {
    DxvkDepthStencilState e = lrz_ds; if (triv) { e.setStencilTest(false); e.normalize(); }
    emit([e] (DxvkContext* c) { c->setDepthStencilState(e); }); lrz_mod = triv; c_lrz_sw++;
  }
  if (triv) c_lrz_drop++;
  if (lg_on()) { char t[200]; std::snprintf(t, sizeof t, "draw ds res %u known %d ref %u front op %d mask %x wm %x back op %d mask %x wm %x depth %d/%d -> %s",
      r, val, lrz_ref, (int)f.compareOp(), f.compareMask(), f.writeMask(), (int)b.compareOp(), b.compareMask(), b.writeMask(), lrz_ds.depthTest(), lrz_ds.depthWrite(),
      triv ? "stencil test dropped" : writes ? "writes stencil" : "kept"); lg(t); }
}
static void s2_stats() {   /* own log line (the [b1] COM: line is already at its buffer size in shadow mode) */
  using namespace s2;
  std::fprintf(stderr, "[b1s2] copyalias %d: deferred %ld (alias binds %ld), executed before a write %ld / for a read %ld, dropped %ld, storage hand-overs %ld (full overwrites without %ld), pending %zu, not eligible %ld | lrzfix %d: stencil test dropped on %ld draws (%ld state switches), whole stencil clears %ld, invalidations %ld\n",
    ca_mode(), c_def, c_alias, c_wr, c_rd, c_drop, c_swap, c_noswap, pend.size(), c_inel, lrz_mode(), c_lrz_drop, c_lrz_sw, c_st_clear, c_st_inval);
}

/* ---- perf73 research diagnostic (BSI_B1_SHADOWSIG=1, off by default; nothing runs without it): are the shadow-depth passes of
   a stereo frame rendered twice (once per eye view, identical) so that the second could be skipped? Every pass into a
   depth-only square target >= 512 gets a signature of everything that decides its result: per draw the shaders, input
   layout, vertex/index buffers + offsets, draw arguments, viewport, rasterizer/depth state ids and the CONTENT of the
   VS/PS constant buffers in slots 0-3 (hashed when written; buffer ids are not used, the game may rename them), plus its
   clears. Two passes with the same signature into the same depth target render the same image. One line per frame for
   the first 40 frames with shadow passes, then every 300 frames, plus a running summary. ---- */
namespace shsig {
  static int on = -1; static std::vector<uint64_t> cbh; static uint32_t cb[2][4], sh[2], il, vb[4], vbs[4], vbo[4], ib, ibo, ibf;
  static bool active; static uint64_t h; static uint32_t dsv, nd; static long frames, frames_sh, passes, dup_passes, dup_frames;
  struct P { uint32_t dsv, nd; uint64_t h; float vp[4]; }; static std::vector<P> fr;
  static inline bool enabled() { if (on < 0) { const char *e = std::getenv("BSI_B1_SHADOWSIG"); on = e && std::atoi(e) > 0; } return on > 0; }
  static inline uint64_t mix(uint64_t x, uint64_t v) { x ^= v + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2); return x * 1099511628211ull; }
  static uint64_t hdata(const void *d, size_t n) { uint64_t x = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)d; for (size_t i = 0; i < n; i++) x = (x ^ p[i]) * 1099511628211ull; return x; }
  static void wrote(uint32_t id, uint32_t off, uint32_t n, const void *d) { if (!enabled() || !id) return; if (cbh.size() <= id) cbh.resize(id + 1024, 0);
    cbh[id] = off == 0 ? hdata(d, n) : mix(cbh[id], mix(off, hdata(d, n))); }
  static void close() { if (active) { P p = { dsv, nd, h, { 0, 0, 0, 0 } }; if (g_cs.nvp) for (int i = 0; i < 4; i++) p.vp[i] = g_cs.vp[0][i]; fr.push_back(p); } active = false; }
  static void rts(uint32_t n, const uint32_t *rtvs, uint32_t d) {
    if (!enabled()) return; close();
    for (uint32_t i = 0; i < n && i < 8; i++) if (rtvs[i]) return;
    View *v = g_views.get(d); if (!v || v->type != 3) return;
    VkExtent3D e = v->iv->mipLevelExtent(0); if (e.width != e.height || e.width < 512) return;
    active = true; dsv = d; nd = 0; h = mix(1469598103934665603ull, d); }
  static void clear(uint32_t d, uint32_t fl, float depth, uint32_t st) { if (!enabled() || !active) return; uint32_t df; std::memcpy(&df, &depth, 4); h = mix(mix(mix(h, 0xC1EA4ull ^ d), fl), ((uint64_t)df << 32) | st); }
  static void draw(const uint32_t *args, int na) {
    if (!enabled() || !active) return;
    uint64_t x = mix(mix(mix(h, sh[0]), sh[1]), il);
    for (int i = 0; i < 4; i++) x = mix(mix(mix(x, vb[i]), vbs[i]), vbo[i]);
    x = mix(mix(mix(x, ib), ibo), ibf);
    for (int i = 0; i < na; i++) x = mix(x, args[i]);
    for (uint32_t i = 0; i < g_cs.nvp && i < 1; i++) for (int k = 0; k < 6; k++) { uint32_t f; std::memcpy(&f, &g_cs.vp[i][k], 4); x = mix(x, f); }
    x = mix(mix(x, g_cs.rs), g_cs.dss);
    for (int st = 0; st < 2; st++) for (int s2 = 0; s2 < 4; s2++) { uint32_t id = cb[st][s2]; x = mix(x, id && id < cbh.size() ? cbh[id] : 0); }
    h = x; nd++; }
  static void present() {
    if (!enabled()) return; close(); frames++;
    if (fr.empty()) return;
    frames_sh++; passes += (long)fr.size(); int dup = 0;
    for (size_t i = 0; i < fr.size(); i++) for (size_t j = 0; j < i; j++) if (fr[i].h == fr[j].h && fr[i].dsv == fr[j].dsv && fr[i].nd) { dup++; break; }
    dup_passes += dup; if (dup) dup_frames++;
    if (frames_sh <= 40 || frames_sh % 300 == 0) {
      std::string t; char b[160];
      for (size_t i = 0; i < fr.size(); i++) { std::snprintf(b, sizeof b, " | #%zu dsv %u vp %.0f,%.0f %.0fx%.0f draws %u sig %016llx", i, fr[i].dsv, fr[i].vp[0], fr[i].vp[1], fr[i].vp[2], fr[i].vp[3], fr[i].nd, (unsigned long long)fr[i].h); t += b; }
      std::fprintf(stderr, "[b1sig] frame %ld (stereo %d): %zu shadow-depth passes, %d identical to an earlier pass of the frame%s\n", frames, (int)b1vr::on, fr.size(), dup, t.c_str());
      std::fprintf(stderr, "[b1sig] so far: %ld frames with shadow passes, %ld passes, %ld duplicates in %ld frames\n", frames_sh, passes, dup_passes, dup_frames); }
    fr.clear(); }
}
static void shsig_frame() { if (shsig::enabled()) shsig::present(); }

/* ---- context ---- */
#define CTX_GUARD() if (g_ctx == nullptr) return; std::lock_guard<std::mutex> lk(g_rmu); try {
#define CTX_END(what) } catch (const DxvkError& e) { cerr(what, e); }

/* diagnostics: dump the current render target 0 after a draw (shadow mode, with the bridge's per-draw dump frame) so
   the two renderers can be compared draw by draw. Conversion as bridge.c dump_tex (half/float: v/(1+v), gamma 2.2). */
static float b1_h2f(uint16_t h){ uint32_t s=(h>>15)&1, e=(h>>10)&31, m=h&1023; uint32_t f;
  if(!e){ if(!m) f=s<<31; else { e=127-15+1; while(!(m&1024)){ m<<=1; e--; } m&=1023; f=(s<<31)|(e<<23)|(m<<13); } }
  else if(e==31) f=(s<<31)|0x7f800000|(m<<13); else f=(s<<31)|((e+112)<<23)|(m<<13); float r; std::memcpy(&r,&f,4); return r; }
static uint8_t b1_tob(float v){ if(!(v>0)) return 0; v=v/(1+v); v=std::pow(v,1/2.2f); return v>=1?255:(uint8_t)(v*255); }
extern "C" int b1dx_dump_rt0(const char *path) {
  if (g_ctx == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    View *v = g_views.get(g_cs.rtv[0]); if (!v || v->type != 2) return 0;
    Rc<DxvkImage> img = v->iv->image(); const auto &vi = v->iv->info();
    VkFormat fmt = img->info().format; const DxvkFormatInfo *fi = lookupFormatInfo(fmt);
    if (fi->flags.test(DxvkFormatFlag::BlockCompressed)) return 0;
    VkExtent3D ext = img->mipLevelExtent(vi.mipIndex); uint32_t es = fi->elementSize;
    VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * es;
    DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 rt readback";
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageSubresourceLayers sl = { fi->aspectMask, vi.mipIndex, vi.layerIndex, 1u };
    emit([rb, fmt, img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ ext.width, ext.height, 1u }); });
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    FILE *f = std::fopen(path, "wb"); if (!f) return 0;
    std::fprintf(f, "P6\n%u %u\n255\n", ext.width, ext.height);
    const uint8_t *p = (const uint8_t *)rb->mapPtr(0); std::vector<uint8_t> row(ext.width * 3);
    for (uint32_t y = 0; y < ext.height; y++) { const uint8_t *r = p + (size_t)y * ext.width * es;
      for (uint32_t x = 0; x < ext.width; x++) { uint8_t *o = &row[3 * x]; const uint8_t *q = r + (size_t)x * es;
        switch (fmt) {
          case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; break;
          case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0]=q[2]; o[1]=q[1]; o[2]=q[0]; break;
          case VK_FORMAT_R16G16B16A16_SFLOAT: { const uint16_t *h=(const uint16_t*)q; o[0]=b1_tob(b1_h2f(h[0])); o[1]=b1_tob(b1_h2f(h[1])); o[2]=b1_tob(b1_h2f(h[2])); break; }
          case VK_FORMAT_R32G32B32A32_SFLOAT: { const float *h=(const float*)q; o[0]=b1_tob(h[0]); o[1]=b1_tob(h[1]); o[2]=b1_tob(h[2]); break; }
          case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { uint32_t w; std::memcpy(&w,q,4); uint32_t rr=w&2047,gg=(w>>11)&2047,bb=w>>22;
            o[0]=b1_tob(b1_h2f((uint16_t)(rr<<4))); o[1]=b1_tob(b1_h2f((uint16_t)(gg<<4))); o[2]=b1_tob(b1_h2f((uint16_t)(bb<<5))); break; }
          case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t w; std::memcpy(&w,q,4); o[0]=(w&1023)>>2; o[1]=((w>>10)&1023)>>2; o[2]=((w>>20)&1023)>>2; break; }
          case VK_FORMAT_R32_SFLOAT: { float h; std::memcpy(&h,q,4); o[0]=o[1]=o[2]=b1_tob(h); break; }
          case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16_SFLOAT: { const uint16_t *h=(const uint16_t*)q; o[0]=o[1]=o[2]=b1_tob(b1_h2f(h[0])); break; }
          default: o[0]=o[1]=o[2]=q[0]; } }
      std::fwrite(row.data(), 1, row.size(), f); }
    std::fclose(f);
    static int nl; if (nl++ < 4) std::fprintf(stderr, "[b1] per-draw dump %s (%ux%u fmt %d)\n", path, ext.width, ext.height, (int)fmt);
    return 1;
  } catch (const DxvkError& e) { std::fprintf(stderr, "[b1] per-draw dump failed: %s\n", e.message().c_str()); }
  return 0;
}
extern "C" void b1dx_c_shader(int st, uint32_t sh) {
  CTX_GUARD()
  if (st < 0 || st > 5) return;
  if (st == 5) g_cur_cs = sh;
  if (st == 1) b1vr::ps_id = sh;   /* ao74 diagnostics (BSI_B1_VRPASS) */
  if (st == 0) b1vr::vs_id = sh;   /* pp76 diagnostics */
  if (st == 1 && (b1vr::on || b1vr::fxd_on > 0)) b1vr::ps_set(sh);   /* pp76: kind of pixel shader (light shafts, tone map, sun shadow projection, FXAA) */
  if (st == 1) b1eo_hook_ps_set(sh);
  if (shsig::on > 0 && st < 2) shsig::sh[st] = sh;   /* perf73 diagnostic */
  if (bc_same(g_bc.sh[st], sh)) { if (!bc_verify()) return; Rc<DxvkShader> c = sh ? b1dx_shader_get(sh) : nullptr; if (bc_keep(g_bp.sh[st], c.ptr())) return; }
  Rc<DxvkShader> s = sh ? b1dx_shader_get(sh) : nullptr;
  g_bp.sh[st] = s.ptr();
  emit([st, s = std::move(s)] (DxvkContext* c) mutable {
    switch (st) {
      case 0: c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(s)); break;
      case 1: c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); break;
      case 2: c->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(std::move(s)); break;
      case 3: c->bindShader<VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT>(std::move(s)); break;
      case 4: c->bindShader<VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT>(std::move(s)); break;
      case 5: c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(s)); break;
      default: break;
    } });
  if (st == 1) b1eo_hook_ps_bound_orig();
  CTX_END("bind shader")
}
/* ---- compute (13 Oct, build 60): b1 had no compute at all, so in b1n the game's HDAO (and every other Dispatch) never ran
   and the AO texture stayed at its initial value. Bindings as DXVK's D3D11: UAV u# -> UavIndexCompute + #, counters unused. ---- */
namespace { constexpr uint32_t kSrvTotal = 128u * 6u, kUavPerPipeline = 64u, kUavCompute = kSrvTotal + kUavPerPipeline * 2u; long g_ndisp, g_nuavclr, g_ngenmips, g_ndispalt; }
static bool b1ab_hook_dispatch(uint32_t x, uint32_t y, uint32_t z); static void b1ab_hook_uav(uint32_t slot, View *v);   /* rhi_b1_aoblur.inc */
static bool b1ab_is_blur(uint32_t cs);   /* rhi_b1_aoblur.inc (vr70) */
static bool b1ab_last_emitted();         /* rhi_b1_aoblur.inc (ao74) */
extern "C" void b1dx_c_uavs(uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  for (uint32_t i = 0; i < n && start + i < kUavPerPipeline; i++) {
    View *v = g_views.get(ids[i]); Rc<DxvkImageView> iv = (v && v->type == 4) ? v->iv : nullptr;
    s2_uav(start + i, (v && v->type == 4) ? v : nullptr); b1ab_hook_uav(start + i, (v && v->type == 4) ? v : nullptr);
    if (b1vr::on) b1vr::uav(start + i, v);   /* vr70: AO crop */
    if (start + i == 0 && b1vr::aodump_n > 0) b1vr::uav0_any = iv;   /* ao74 diagnostics */
    emit([slot = kUavCompute + start + i, ctr = kUavCompute + kUavPerPipeline + start + i, iv = std::move(iv)] (DxvkContext* c) mutable {
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, slot, std::move(iv)); c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, ctr, nullptr); });
  }
  CTX_END("bind UAVs")
}
static bool b1dx_dispatch_body(uint32_t x, uint32_t y, uint32_t z);
extern "C" void b1dx_c_dispatch(uint32_t x, uint32_t y, uint32_t z) {
  CTX_GUARD()
  if (!x || !y || !z) return;
  bool emitted = b1dx_dispatch_body(x, y, z);
  if (b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::ao_diag(emitted, x, y, b1ab_is_blur(g_cur_cs));   /* ao74 diagnostics (cloud) */
  CTX_END("dispatch")
}
/* (g_rmu held) false = not dispatched (ao_eye skip) */
static bool b1dx_dispatch_body(uint32_t x, uint32_t y, uint32_t z) {
  g_ndisp++;
  if (b1vr::on) { b1vr::disp_x = x; b1vr::disp_log(x, y, z, g_cur_cs, b1ab_is_blur(g_cur_cs), false); }   /* vr71 */
  if (b1vr::crop_m && b1ab_is_blur(g_cur_cs)) x = b1vr::crop_x(x, y);   /* vr70: AO crop (stereo layout with the UI column; the HDAO blurs only) */
  if (b1ab_hook_dispatch(x, y, z)) return b1ab_last_emitted();   /* V4 HDAO blur twin (rhi_b1_aoblur.inc) */
  uint32_t sc[3] = { 1, 1, 1 }; bool gtao = false; Rc<DxvkShader> alt = b1dx_shader_alt2(g_cur_cs, sc, &gtao);
  if (gp::on()) gp::on_dispatch(x, y, alt != nullptr);
  if (alt != nullptr) {   /* AO switch: the replacement for this Dispatch, then the game's shader back (as the bridge's DXVK) */
    g_ndispalt++; Rc<DxvkShader> norm = b1dx_shader_get(g_cur_cs);
    if (b1vr::eyeb) { int k = b1vr::ao_kind(norm, b1ab_is_blur(g_cur_cs));   /* vr71: ao_eye (left eye view's AO passes are dead work) */
      if (k == 1 && b1vr::eyeb == 2) { if (b1vr::eye2(x, y, z, alt, norm, sc, gtao)) return false; }   /* ao74: AO per eye */
      else if (k >= 0 && b1vr::eye_emit(x, y, k, k ? "AO switch main" : "AO switch blur")) return false; }
    emit([alt = std::move(alt), norm = std::move(norm), x = x * sc[0], y = y * sc[1], z = z * sc[2]] (DxvkContext* c) mutable {
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(alt)); c->dispatch(x, y, z); c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(norm)); });
  } else {
    if (b1vr::eyeb) { int k = b1vr::ao_kind(b1dx_shader_get(g_cur_cs), b1ab_is_blur(g_cur_cs));   /* vr71: ao_eye */
      if (k == 1 && b1vr::eyeb == 2) { if (b1vr::eye2(x, y, z, nullptr, nullptr, sc, false)) return false; }   /* ao74: AO per eye */
      else if (k >= 0 && b1vr::eye_emit(x, y, k, k ? "HDAO main" : "AO blur")) return false; }
    emit([x, y, z] (DxvkContext* c) { c->dispatch(x, y, z); }); }
  return true;
}
extern "C" void b1dx_c_clear_uav(uint32_t view, const uint32_t *vals, int isfloat) {
  CTX_GUARD()
  View *v = g_views.get(view); if (!v || v->type != 4 || !vals) return;
  const DxvkFormatInfo *fi = v->iv->formatInfo(); bool intfmt = fi->flags.any(DxvkFormatFlag::SampledSInt, DxvkFormatFlag::SampledUInt);
  if (isfloat && intfmt) return;
  VkClearValue cv = { };
  if (isfloat || !intfmt) { for (int i = 0; i < 4; i++) { float f; if (isfloat) std::memcpy(&f, &vals[i], 4); else f = (float)vals[i]; cv.color.float32[i] = f; } }
  else for (int i = 0; i < 4; i++) cv.color.uint32[i] = vals[i];
  g_nuavclr++; s2_before_write(v->res, false);
  emit([iv = v->iv, cv] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { 0, 0, 0 }, iv->mipLevelExtent(0), VK_IMAGE_ASPECT_COLOR_BIT, cv); });
  CTX_END("clear UAV")
}
extern "C" void b1dx_c_genmips(uint32_t srv) {
  CTX_GUARD()
  View *v = g_views.get(srv); if (!v || v->type != 1 || v->iv->info().mipCount < 2) return;
  g_ngenmips++; s2_before_read(v->res); s2_before_write(v->res, false);
  emit([iv = v->iv] (DxvkContext* c) { c->generateMipmaps(iv, VK_FILTER_LINEAR); });
  CTX_END("generate mips")
}
extern "C" void b1dx_compute_stats(long *disp, long *clr, long *gm) { *disp = g_ndisp; *clr = g_nuavclr; *gm = g_ngenmips; }
extern "C" long b1dx_compute_alt_count(void) { return g_ndispalt; }
extern "C" void b1dx_c_cbs(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  if (shsig::on > 0 && st < 2) for (uint32_t i = 0; i < n && start + i < 4; i++) shsig::cb[st][start + i] = ids[i];   /* perf73 diagnostic */
  if (st == 5 && start == 0 && n) { b1vr::cs_cb0 = ids[0]; if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_bound(ids[0]); }   /* ao74: AO constants (rhi_b1_vr.inc) */
  if (st == 1 && start == 0 && n) { b1vr::ps_cb0 = ids[0]; if (b1vr::pass_armed || b1vr::pass_n > 0) b1vr::cb_bound(ids[0]); }   /* ao74 diagnostics */
  if ((st == 0 || st == 1) && b1vr::on) for (uint32_t i = 0; i < n; i++) b1vr::cb_slot(st, start + i, ids[i]);   /* pp76: bound VS/PS constant buffers */
  if ((st == 0 || st == 1) && (b1vr::pass_armed || b1vr::pass_n > 0)) for (uint32_t i = 0; i < n && start + i < 4; i++) {   /* pp76 diagnostics */
    (st ? b1vr::ps_cb : b1vr::vs_cb)[start + i] = ids[i]; b1vr::cb_bound(ids[i]); }
  for (uint32_t i = 0; i < n && start + i < 14; i++) {
    Obj *o = robj(ids[i]); DxvkBufferSlice sl; uint32_t key = 0;
    if (o && o->kind == 1) { sl = DxvkBufferSlice(o->buf); key = ids[i]; }
    if (st >= 0 && st < 6) { const void *pp = key ? (const void *)o->buf.ptr() : nullptr;
      if (bc_same(g_bc.cb[st][start + i], key) && (!bc_verify() || bc_keep(g_bp.cb[st][start + i], pp))) continue;
      g_bp.cb[st][start + i] = pp; }
    emit([sb = sbit(st), slot = sidx(st) * 16 + start + i, sl = std::move(sl)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, slot, std::move(sl)); });
  }
  CTX_END("bind constant buffers")
}
extern "C" void b1dx_c_srvs(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  if (st == 1 && (b1vr::pass_n > 0 || b1vr::pass_armed)) for (uint32_t i = 0; i < n && start + i < 8; i++) b1vr::ps_srv[start + i] = ids[i];   /* ao74 diagnostics (pp76: 8 slots) */
  for (uint32_t i = 0; i < n && start + i < 128; i++) {
    View *v = g_views.get(ids[i]); bool ok = v && v->type == 1;
    if (ok && texlog_on()) { Obj *o = robj(v->res); if (o && o->kind == 2) { const auto &vi = v->iv->info(); uint32_t need = 0;
        for (uint32_t m = vi.mipIndex; m < vi.mipIndex + vi.mipCount && m < 32; m++) need |= 1u << m;
        if ((o->wmask & need) != need) { static uint32_t seen[64]; static int ns; int k; for (k = 0; k < ns; k++) if (seen[k] == v->res) break;
          if (k == ns && ns < 64) { seen[ns++] = v->res; VkExtent3D e = o->img->mipLevelExtent(0);
            std::fprintf(stderr, "[b1tex] SAMPLED UNWRITTEN: res %u %ux%u fmt %d mips %u written %08x view mips %u+%u\n", v->res, e.width, e.height, (int)o->fmt, o->img->info().mipLevels, o->wmask, vi.mipIndex, vi.mipCount); } } } }
    if (st >= 0 && st < 6) { const void *pp = ok ? (const void *)v->iv.ptr() : nullptr;
      if (bc_same(g_bc.srv[st][start + i], ok ? ids[i] : 0) && (!bc_verify() || bc_keep(g_bp.srv[st][start + i], pp))) continue;
      g_bp.srv[st][start + i] = pp; }
    Rc<DxvkImageView> iv = s2_srv(st, start + i, ids[i], ok ? v : nullptr, ok ? v->iv : nullptr);
    if (st == 1 && start + i == 4 && b1vr::on) { b1vr::t4_res = ok ? v->res : 0; b1vr::t4_iv = iv; }   /* pp76: light shafts per eye */
    if (st == 1 && start + i == 0 && (b1vr::on || b1vr::fxd_on > 0)) { b1vr::t0_res = ok ? v->res : 0; b1vr::t0_iv = iv; }   /* pp76: FXAA per eye / diagnostics */
    if (st == 1 && start + i < 4 && b1vr::on) b1vr::ps_tres[start + i] = ok ? v->res : 0;   /* pp76: bloom per eye */
    emit([sb = sbit(st), slot = sidx(st) * 128 + start + i, iv = std::move(iv)] (DxvkContext* c) mutable { c->bindResourceImageView(sb, slot, std::move(iv)); });
  }
  CTX_END("bind SRVs")
}
extern "C" void b1dx_c_samplers(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  for (uint32_t i = 0; i < n && start + i < 16; i++) {
    SS *s = g_sss.get(ids[i]);
    if (st >= 0 && st < 6) { const void *pp = s ? (const void *)s->s.ptr() : nullptr;
      if (bc_same(g_bc.ss[st][start + i], s ? ids[i] : 0) && (!bc_verify() || bc_keep(g_bp.ss[st][start + i], pp))) continue;
      g_bp.ss[st][start + i] = pp; }
    Rc<DxvkSampler> sp = s ? s->s : nullptr;
    emit([sb = sbit(st), slot = sidx(st) * 16 + start + i, sp = std::move(sp)] (DxvkContext* c) mutable { c->bindResourceSampler(sb, slot, std::move(sp)); });
  }
  CTX_END("bind samplers")
}
extern "C" void b1dx_c_il(uint32_t id) {
  CTX_GUARD()
  IL *il = g_ils.get(id);
  if (shsig::on > 0) shsig::il = id;   /* perf73 diagnostic */
  if (bc_same(g_bc.il, il ? id : 0)) return;
  if (il) emit_data<DxvkVertexInput>(il->in, il->na + il->nb, [na = il->na, nb = il->nb] (DxvkContext* c, const DxvkVertexInput* in, size_t) { c->setInputLayout(na, in, nb, in + na); });
  else emit([] (DxvkContext* c) { c->setInputLayout(0, nullptr, 0, nullptr); });
  CTX_END("input layout")
}
extern "C" void b1dx_c_vbs(uint32_t start, uint32_t n, const uint32_t *ids, const uint32_t *strides, const uint32_t *offsets) {
  CTX_GUARD()
  for (uint32_t i = 0; i < n && start + i < 32; i++) {
    Obj *o = robj(ids[i]); uint32_t s = start + i;
    if (shsig::on > 0 && s < 4) { shsig::vb[s] = ids[i]; shsig::vbs[s] = strides[i]; shsig::vbo[s] = offsets[i]; }   /* perf73 diagnostic */
    bool ok = o && o->kind == 1 && offsets[i] < o->buf->info().size;
    uint32_t key = ok ? ids[i] : 0, kst = ok ? strides[i] : 0, kof = ok ? offsets[i] : 0;
    const void *pp = ok ? (const void *)o->buf.ptr() : nullptr;
    if (g_bc.vbs[s] == kst && g_bc.vbo[s] == kof && bc_same(g_bc.vb[s], key) && (!bc_verify() || bc_keep(g_bp.vb[s], pp))) continue;
    g_bc.vb[s] = key; g_bc.vbs[s] = kst; g_bc.vbo[s] = kof; g_bp.vb[s] = pp;
    DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, offsets[i], o->buf->info().size - offsets[i]) : DxvkBufferSlice();
    emit([s, sl = std::move(sl), str = ok ? strides[i] : 0u] (DxvkContext* c) mutable { c->bindVertexBuffer(s, std::move(sl), str); });
  }
  CTX_END("vertex buffers")
}
extern "C" void b1dx_c_ib(uint32_t id, uint32_t fmt, uint32_t off) {
  CTX_GUARD()
  Obj *o = robj(id); VkIndexType t = fmt == 42 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  if (shsig::on > 0) { shsig::ib = id; shsig::ibo = off; shsig::ibf = fmt; }   /* perf73 diagnostic */
  bool ok = o && o->kind == 1 && off < o->buf->info().size;
  uint32_t key = ok ? id : 0, kf = (uint32_t)t, ko = ok ? off : 0;
  const void *pp = ok ? (const void *)o->buf.ptr() : nullptr;
  if (g_bc.ibf == kf && g_bc.ibo == ko && bc_same(g_bc.ib, key) && (!bc_verify() || bc_keep(g_bp.ib, pp))) return;
  g_bc.ib = key; g_bc.ibf = kf; g_bc.ibo = ko; g_bp.ib = pp;
  DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, off, o->buf->info().size - off) : DxvkBufferSlice();
  emit([sl = std::move(sl), t] (DxvkContext* c) mutable { c->bindIndexBuffer(std::move(sl), t); });
  CTX_END("index buffer")
}
extern "C" void b1dx_c_topology(uint32_t t) {
  CTX_GUARD()
  VkPrimitiveTopology vt;
  switch (t) { case 1: vt = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break; case 2: vt = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break; case 3: vt = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
               case 5: vt = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break; default: vt = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break; }
  if (bc_same(g_bc.topo, (uint32_t)vt)) return;
  emit([vt] (DxvkContext* c) { c->setInputAssemblyState(DxvkInputAssemblyState(vt, false)); });
  CTX_END("topology")
}
extern "C" void b1dx_c_rs(uint32_t id) {
  CTX_GUARD()
  RS *rs = g_rss.get(id); RS *old = g_rss.get(g_cs.rs);
  if (bc_same(g_bc.rs, rs ? id : 0)) { g_cs.rs = id; return; }
  emit([s = rs ? rs->s : default_rs(), b = rs ? rs->b : DxvkDepthBias()] (DxvkContext* c) { c->setRasterizerState(s); c->setDepthBias(b); });
  if ((old && old->scissor) != (rs && rs->scissor)) g_cs.vpdirty = true;
  g_cs.rs = id;
  CTX_END("rasterizer state")
}
extern "C" void b1dx_c_viewports(uint32_t n, const float *v) {
  if (g_ctx == nullptr) return; std::lock_guard<std::mutex> lk(g_rmu);
  uint32_t nn = n > 16 ? 16 : n;
  if (bc_on() && !g_cs.vpdirty && nn == g_cs.nvp && !std::memcmp(g_cs.vp, v, nn * 24)) { g_bc_skip++; return; }   /* unchanged */
  g_cs.nvp = nn; std::memcpy(g_cs.vp, v, g_cs.nvp * 24); g_cs.vpdirty = true;
}
extern "C" void b1dx_c_scissors(uint32_t n, const int32_t *r) {
  if (g_ctx == nullptr) return; std::lock_guard<std::mutex> lk(g_rmu);
  uint32_t nn = n > 16 ? 16 : n;
  if (bc_on() && !g_cs.vpdirty && nn == g_cs.nsc && !std::memcmp(g_cs.sc, r, nn * 16)) { g_bc_skip++; return; }
  g_cs.nsc = nn; std::memcpy(g_cs.sc, r, g_cs.nsc * 16); g_cs.vpdirty = true;
}
extern "C" void b1dx_c_blend(uint32_t id, const float *factor, uint32_t mask) {
  CTX_GUARD()
  BS *bs = g_bss.get(id);
  float f[4] = { 1, 1, 1, 1 }; if (factor) std::memcpy(f, factor, 16);
  if (b1vr::alpha) b1vr::note_blend(id, f, mask);   /* vr69 */
  if (bc_on() && g_bc.bs == (bs ? id : 0) && g_bc.bsmask == mask && !std::memcmp(g_bc.bf, f, 16)) { g_bc_skip++; g_cs.bs = id; return; }
  g_bc.bs = bs ? id : 0; g_bc.bsmask = mask; std::memcpy(g_bc.bf, f, 16); g_bc_bind++;
  struct BlendCmd { DxvkMultisampleState ms; DxvkBlendMode m[8]; DxvkLogicOpState lo; DxvkBlendConstants bc; } bcmd;
  DxvkMultisampleState &ms = bcmd.ms; ms = { }; ms.setSampleMask((uint16_t)mask); ms.setAlphaToCoverage(bs && bs->a2c);
  for (uint32_t i = 0; i < 8; i++) {
    DxvkBlendMode m = { };
    if (bs) m = bs->m[i];
    else { m.setBlendEnable(false); m.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
           m.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); m.setWriteMask(0xf); m.normalize(); }
    bcmd.m[i] = m;
  }
  if (b1vr::alpha && b1vr::bbrt) b1vr::cover(bcmd.m[0]);   /* vr69: UI column coverage alpha */
  DxvkLogicOpState lo = { }; lo.setLogicOp(false, VK_LOGIC_OP_NO_OP);
  bcmd.lo = bs ? bs->lo : lo; bcmd.bc = DxvkBlendConstants{ f[0], f[1], f[2], f[3] };
  emit([bcmd] (DxvkContext* c) {
    for (uint32_t i = 0; i < 8; i++) c->setBlendMode(i, bcmd.m[i]);
    c->setMultisampleState(bcmd.ms); c->setLogicOpState(bcmd.lo); c->setBlendConstants(bcmd.bc); });
  g_cs.bs = id;
  CTX_END("blend state")
}
extern "C" void b1dx_c_dss(uint32_t id, uint32_t ref) {
  CTX_GUARD()
  DSS *d = g_dsss.get(id);
  if (bc_on() && g_bc.dss == (d ? id : 0) && g_bc.dssref == ref) { g_bc_skip++; g_cs.dss = id; g_cs.sref = ref; return; }
  g_bc.dss = d ? id : 0; g_bc.dssref = ref; g_bc_bind++;
  DxvkDepthStencilState ds = { };
  if (d) ds = d->s;
  else { ds.setDepthTest(true); ds.setDepthWrite(true); ds.setDepthCompareOp(VK_COMPARE_OP_LESS); ds.normalize(); }
  emit([ds, ref] (DxvkContext* c) { c->setDepthStencilState(ds); c->setStencilReference(ref & 0xffu); }); s2_dss(ds, ref);
  g_cs.dss = id; g_cs.sref = ref;
  CTX_END("depth-stencil state")
}
extern "C" void b1dx_c_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv) {
  CTX_GUARD()
  if (g_flush_pending) { ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }   /* render pass boundary (build 64) */
  if (g_eo_any) b1eo_hook_rts(dsv);
  shsig::rts(n, rtvs, dsv);   /* perf73 diagnostic (off: one test) */
  DxvkRenderTargets rts = { };
  for (uint32_t i = 0; i < 8; i++) { g_cs.rtv[i] = i < n ? rtvs[i] : 0; View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) rts.color[i].view = v->iv; }
  g_cs.dsv = dsv; { View *v = g_views.get(dsv); if (v && v->type == 3) rts.depth.view = v->iv; }
  if (gp::on()) gp::set_targets(rts);
  if (pp::on()) { uint64_t k = 1469598103934665603ull; double mp = 0;
    for (uint32_t i = 0; i < 8; i++) { uintptr_t q = rts.color[i].view != nullptr ? (uintptr_t)rts.color[i].view->image() : 0; k = (k ^ q) * 1099511628211ull;
                                        if (q) { VkExtent3D e = rts.color[i].view->mipLevelExtent(0); mp += e.width * (double)e.height / 1e6; } }
    if (rts.depth.view != nullptr) { k = (k ^ (uintptr_t)rts.depth.view->image()) * 1099511628211ull; VkExtent3D e = rts.depth.view->mipLevelExtent(0); mp += e.width * (double)e.height / 1e6; }
    pp::rtkey = k; pp::rtmpix = mp; }
  emit([rts = std::move(rts)] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(rts), 0u); });
  g_nrt++;
  if (b1vr::on || b1vr::bbrt) b1vr::rts(g_cs.rtv[0]);   /* vr69 */
  CTX_END("render targets")
}
extern "C" void b1dx_c_clear_rtv(uint32_t id, const float *rgba) {
  CTX_GUARD()
  View *v = g_views.get(id); if (!v || v->type != 2) return;
  if (b1vr::pass_n > 0 && (b1vr::on || b1vr::pass_flat) && b1vr::n_frames >= b1vr::pass_from && b1vr::n_frames < b1vr::pass_to)   /* pp76 diagnostics */
    std::fprintf(stderr, "[b1vr] pass: frame %ld after draw %ld clear res %u to %g %g %g %g\n", b1vr::n_frames, b1vr::pass_draw, v->res, rgba[0], rgba[1], rgba[2], rgba[3]);
  s2_clear_rtv(v);
  VkClearValue cv = { }; std::memcpy(cv.color.float32, rgba, 16);
  DxvkAttachment a = { }; a.view = v->iv; emit([a, cv] (DxvkContext* c) { c->clearRenderTarget(a, VK_IMAGE_ASPECT_COLOR_BIT, cv, 0u); }); g_nclear++; pp::clr = 1;
  CTX_END("clear RTV")
}
extern "C" void b1dx_c_clear_dsv(uint32_t id, uint32_t flags, float depth, uint32_t stencil) {
  CTX_GUARD()
  View *v = g_views.get(id); if (!v || v->type != 3) return;
  if (shsig::on > 0) shsig::clear(id, flags, depth, stencil);   /* perf73 diagnostic */
  VkImageAspectFlags have = v->iv->info().aspects, asp = 0;
  if (flags & 1) asp |= have & VK_IMAGE_ASPECT_DEPTH_BIT;
  if (flags & 2) asp |= have & VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!asp) return;
  s2_clear_dsv(v, asp, stencil);
  if (g_eo_any && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) b1eo_hook_written(v->res);
  VkClearValue cv = { }; cv.depthStencil.depth = depth; cv.depthStencil.stencil = stencil & 0xffu;
  DxvkAttachment a = { }; a.view = v->iv; emit([a, asp, cv] (DxvkContext* c) { c->clearRenderTarget(a, asp, cv, 0u); }); g_nclear++; pp::clr = 1;
  CTX_END("clear DSV")
}
/* data written to a resource: UpdateSubresource (box: left,top,front,right,bottom,back or null) and Unmap of a write map */
static void write_buffer(Obj &o, uint32_t off, uint32_t size, const void *data, bool discard) {
  VkDeviceSize bsz = o.buf->info().size;
  if (off >= bsz) return;
  if (off + size > bsz) size = (uint32_t)(bsz - off);
  if (discard && off == 0 && size == bsz && (o.buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
    o.dyn = dyn_storage(*o.buf); std::memcpy(o.dyn->mapPtr(), data, size);
    emit([b = o.buf, s = o.dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(s)); });
  } else if (size <= 65536u && !(size & 3u) && !(off & 3u)) {
    /* CS: the bytes go into the chunk (<= 8 KB) or a heap copy (emit_data) */
    emit_data<uint8_t>((const uint8_t *)data, size, [b = o.buf, off] (DxvkContext* c, const uint8_t *d, size_t n) { c->updateBuffer(b, off, n, d); });
  } else {
    Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
    emit([b = o.buf, off, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(b, off, sb, so, size); }); up_maybe_flush(size);
  }
  g_cbytes += size;
}
static int img_info(uint32_t id, uint32_t *mips, bool *is3d) {
  std::lock_guard<std::mutex> lk(g_rmu); Obj *o = robj(id); if (!o || o->kind != 2) return 0;
  *mips = o->img->info().mipLevels; *is3d = o->img->info().type == VK_IMAGE_TYPE_3D; return 1;
}
static void tex_data(uint32_t id, uint32_t sub, const uint32_t *box, const void *data, uint32_t rowp, uint32_t slicep) {
  uint32_t mips; bool is3d; if (!img_info(id, &mips, &is3d)) return;
  uint32_t mip = sub % mips, layer = sub / mips;
  if (is3d) { if (!box) b1dx_tex3d_write(id, mip, data, rowp, slicep); return; }
  if (box) b1dx_tex_write_rect(id, mip, layer, box[0], box[1], box[3] - box[0], box[4] - box[1], data, rowp);
  else b1dx_tex_write_rect(id, mip, layer, 0, 0, 0, 0, data, rowp);
}
extern "C" void b1dx_c_update(uint32_t id, uint32_t sub, const uint32_t *box, const void *data, uint32_t rowpitch, uint32_t slicepitch) {
  if (!data) return;
  { std::lock_guard<std::mutex> lk(g_rmu); Obj *o = robj(id); if (!o) return;
    if (o->kind == 1) { if (shsig::on > 0) shsig::wrote(id, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data);   /* perf73 diagnostic */
                        if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data);   /* ao74 */
                        try { if (box) write_buffer(*o, box[0], box[3] - box[0], data, false); else write_buffer(*o, 0, (uint32_t)o->buf->info().size, data, true); g_nupd++; }
                        catch (const DxvkError& e) { cerr("update buffer", e); } return; } }
  tex_data(id, sub, box, data, rowpitch, slicepitch); g_nupd++;
}
extern "C" void b1dx_c_unmap(uint32_t id, uint32_t sub, const void *data, uint32_t rowpitch, uint32_t slicepitch, int discard) {
  if (!data) return;
  { std::lock_guard<std::mutex> lk(g_rmu); Obj *o = robj(id); if (!o) return;
    if (o->kind == 1) { if (shsig::on > 0) shsig::wrote(id, 0, (uint32_t)o->buf->info().size, data);   /* perf73 diagnostic */
                        if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, 0, (uint32_t)o->buf->info().size, data);   /* ao74 */
                        if (g_aa_live) b1aa_vbwrite(id, 0, (uint32_t)o->buf->info().size, data);   /* aa76 */
                        try { write_buffer(*o, 0, (uint32_t)o->buf->info().size, data, discard != 0); g_nmapw++; } catch (const DxvkError& e) { cerr("unmap buffer", e); } return; } }
  tex_data(id, sub, 0, data, rowpitch, slicepitch); g_nmapw++;
}
extern "C" void b1dx_c_copy_region(uint32_t dst, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint32_t src, uint32_t ssub, const uint32_t *box) {
  CTX_GUARD()
  if (b1vr::pass_n > 0 && (b1vr::on || b1vr::pass_flat) && b1vr::n_frames >= b1vr::pass_from && b1vr::n_frames < b1vr::pass_to)   /* pp76 diagnostics */
    std::fprintf(stderr, "[b1vr] pass: frame %ld after draw %ld copy res %u -> res %u at %u,%u box %d %d %d %d\n", b1vr::n_frames, b1vr::pass_draw, src, dst, x, y,
                 box ? (int)box[0] : -1, box ? (int)box[1] : -1, box ? (int)box[3] : -1, box ? (int)box[4] : -1);
  Obj *d = robj(dst), *s = robj(src); if (!d || !s || d->kind != s->kind) return;
  if (d->kind == 1) {
    uint32_t so = box ? box[0] : 0, sz = box ? box[3] - box[0] : (uint32_t)s->buf->info().size;
    if (so + sz <= s->buf->info().size && x + sz <= d->buf->info().size) emit([db = d->buf, x, sb = s->buf, so, sz] (DxvkContext* c) { c->copyBuffer(db, x, sb, so, sz); });
  } else {
    uint32_t dm = d->img->info().mipLevels, sm = s->img->info().mipLevels;
    VkImageAspectFlags da = lookupFormatInfo(d->fmt)->aspectMask, sa = lookupFormatInfo(s->fmt)->aspectMask;
    VkImageSubresourceLayers dl = { da, dsub % dm, dsub / dm, 1u }, sl = { sa, ssub % sm, ssub / sm, 1u };
    VkExtent3D se = s->img->mipLevelExtent(sl.mipLevel), de = d->img->mipLevelExtent(dl.mipLevel);
    bool d3 = s->img->info().type == VK_IMAGE_TYPE_3D;
    if (d3) { dl.baseArrayLayer = 0; sl.baseArrayLayer = 0; dl.mipLevel = dsub % dm; sl.mipLevel = ssub % sm; }
    VkOffset3D so = { 0, 0, 0 }; VkExtent3D ext = se;
    if (box) { so = { (int32_t)box[0], (int32_t)box[1], d3 ? (int32_t)box[2] : 0 }; ext = { box[3] - box[0], box[4] - box[1], d3 ? box[5] - box[2] : 1u }; }
    if (x + ext.width > de.width) ext.width = de.width > x ? de.width - x : 0;
    if (y + ext.height > de.height) ext.height = de.height > y ? de.height - y : 0;
    if (!ext.width || !ext.height || d->fmt != s->fmt) { if (d->fmt != s->fmt && g_ncerr++ < 20) std::fprintf(stderr, "[b1] copy between formats %d -> %d skipped\n", s->fmt, d->fmt); return; }
    if (d3 && z + ext.depth > de.depth) ext.depth = de.depth > z ? de.depth - z : 0;
    if (!ext.depth) return;
    { static int nl; if (texlog_on() && nl < texlog_on() && ext.width >= 64) { nl++; std::fprintf(stderr, "[b1tex] copy %u(m%u l%u) -> %u(m%u l%u) at %u,%u ext %ux%u fmt %d src %ux%u dst %ux%u mips %u->%u box %d\n", src, sl.mipLevel, sl.baseArrayLayer, dst, dl.mipLevel, dl.baseArrayLayer, x, y, ext.width, ext.height, (int)d->fmt, se.width, se.height, de.width, de.height, sm, dm, box ? 1 : 0); } }
    if (dl.mipLevel < 32) d->wmask |= 1u << dl.mipLevel;
    if (g_eo_any) b1eo_hook_written(dst);
    if (!s2_copy(dst, dsub, x, y, z, src, ssub, box))
    emit([di = d->img, dl, doff = VkOffset3D{ (int32_t)x, (int32_t)y, d3 ? (int32_t)z : 0 }, si = s->img, sl, so, ext] (DxvkContext* c) { c->copyImage(di, dl, doff, si, sl, so, ext); });
  }
  g_ncopy++;
  CTX_END("copy region")
}
/* implicit flush (as DXVK's D3D11 ConsiderFlush): submit every BSI_B1_FLUSH draws (default 256) so the GPU starts on the
   frame while the render thread is still recording; UE3 waits on last frame's occlusion results, so a single submit at
   Present serialised CPU and GPU. */
static inline void pre_draw() {
  if (g_eo_any) b1eo_hook_draw();
  s2_pre_draw();
  if (g_cs.vpdirty) apply_viewports();
  if (b1vr::on) b1vr::draw();   /* vr69: which columns of the stereo back buffer this frame draws into */
  else if (b1vr::pass_flat && b1vr::pass_n) b1vr::pass_diag();   /* pp76 diagnostics: pass log in flat frames (eye-alone reference) */
  if (b1vr::fxd_on > 0) b1vr::fxd_pre();   /* pp76: FXAA / settings diagnostics (flat and VR) */
  if (gp::on()) gp::on_draw();
  if (g_flush_every < 0) { const char *e = std::getenv("BSI_B1_FLUSH"); g_flush_every = e ? std::atoi(e) : 256; }
  if (g_flush_every > 0 && ++g_draws_since_flush >= g_flush_every) {
    /* build 64: on tilers (Turnip) a submit ends the render pass, so every attachment is written out of tile memory and
       read back in (4 G-buffer targets + depth at 1600x900: ~100 MB of traffic, ~1-2 ms). Like DXVK's D3D11 on tilers
       (NotifyRenderPassBoundary), the flush waits for the next render-target change; a very long pass still flushes at 8x. */
    if (tiler_flush() && g_draws_since_flush < 8 * g_flush_every) { if (!g_flush_pending) { g_flush_pending = 1; g_nflush_deferred++; } }
    else { ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }
  }
}
/* aa76 (rhi_b1_aa.inc): opt-in VR anti-aliasing (vr_stereo.txt aa=); g_aa_live is false with aa=off (default) */
static bool b1aa_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic);
static void b1aa_frame_end(); static void b1aa_frame_begin();
extern "C" void b1dx_c_draw(uint32_t count, uint32_t inst, uint32_t first, uint32_t firstinst) {
  CTX_GUARD()
  VkDrawIndirectCommand dc = { count, inst, first, firstinst };
  if (g_aa_live && b1aa_draw(&dc, nullptr)) return;   /* aa76 (merge b76: before pre_draw, then pp76) */
  pre_draw();
  if (b1vr::pp_replace) b1vr::pp_replace_draw(&dc, nullptr); else emit([dc] (DxvkContext* c) { c->draw(1, &dc); }); g_ndraw++;   /* pp76: FXAA per eye */
  if (b1vr::on && b1vr::pp_post_wanted()) b1vr::pp_post(&dc, nullptr);   /* pp76: right-eye replay / t4 back */
  if (b1vr::fxd.post) b1vr::fxd_post();   /* pp76: FXAA check (diagnostics, 3 frames per session) */
  if (shsig::on > 0) { uint32_t a[5] = { 1, count, inst, first, firstinst }; shsig::draw(a, 5); }   /* perf73 diagnostic */
  if (g_eo_any) b1eo_hook_post_draw(&dc, nullptr);
  CTX_END("draw")
}
extern "C" void b1dx_c_draw_indexed(uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t firstinst) {
  CTX_GUARD()
  VkDrawIndexedIndirectCommand dc = { count, inst, first, base, firstinst };
  if (g_aa_live && b1aa_draw(nullptr, &dc)) return;   /* aa76 (merge b76: before pre_draw, then pp76) */
  pre_draw();
  if (b1vr::pp_replace) b1vr::pp_replace_draw(nullptr, &dc); else emit([dc] (DxvkContext* c) { c->drawIndexed(1, &dc); }); g_ndraw++;   /* pp76: FXAA per eye */
  if (b1vr::on && b1vr::pp_post_wanted()) b1vr::pp_post(nullptr, &dc);   /* pp76: right-eye replay / t4 back */
  if (b1vr::fxd.post) b1vr::fxd_post();   /* pp76: FXAA check (diagnostics, 3 frames per session) */
  if (shsig::on > 0) { uint32_t a[6] = { 2, count, inst, first, (uint32_t)base, firstinst }; shsig::draw(a, 6); }   /* perf73 diagnostic */
  if (g_eo_any) b1eo_hook_post_draw(nullptr, &dc);
  CTX_END("draw indexed")
}
/* dynamic buffers (COM seam): Map(DISCARD) renames the storage; the bytes a draw actually uses are then copied straight
   into the current storage (host-visible, never in use by the GPU by D3D11's DISCARD / NO_OVERWRITE rules). */
extern "C" int b1dx_c_map_discard(uint32_t id) {
  if (g_ctx == nullptr) return 0; std::lock_guard<std::mutex> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1 || !(o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 0;
  try { o->dyn = dyn_storage(*o->buf); emit([b = o->buf, a = o->dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); }); return 1; }
  catch (const DxvkError& e) { cerr("map discard", e); }
  return 0;
}
extern "C" uint32_t b1dx_buf_info(uint32_t id, int *hostvis) {
  std::lock_guard<std::mutex> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1) return 0;
  if (hostvis) *hostvis = (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && !(o->dxgi & 0x4u);   /* constant buffers: whole write at Unmap */
  return (uint32_t)o->buf->info().size;
}
extern "C" void b1dx_c_bufwrite(uint32_t id, uint32_t off, uint32_t size, const void *data) {
  if (g_ctx == nullptr || !data || !size) return; std::lock_guard<std::mutex> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1) return;
  VkDeviceSize bsz = o->buf->info().size; if (off >= bsz) return; if (off + size > bsz) size = (uint32_t)(bsz - off);
  try {
    if (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      if (o->dyn == nullptr) { o->dyn = dyn_storage(*o->buf); emit([b = o->buf, a = o->dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); }); }
      std::memcpy((uint8_t *)o->dyn->mapPtr() + off, data, size);
    } else write_buffer(*o, off, size, data, false);
    if (shsig::on > 0) shsig::wrote(id, off ? off : 1, size, data);   /* perf73 diagnostic (dynamic VB/IB ranges) */
    if (g_aa_live) b1aa_vbwrite(id, off, size, data);   /* aa76 */
    if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, off, size, data);   /* ao74 */
    g_cbytes += size; g_nmapw++;
  } catch (const DxvkError& e) { cerr("buffer write", e); }
}
extern "C" uint32_t b1dx_bb_id(void) { return g_bb; }
extern "C" void b1dx_com_stats(char *out, int n) {
  DxvkStatCounters sc; if (g_dev != nullptr) sc = g_dev->getStatCounters();
  long qp = 0, qf = 0; query_stats(&qp, &qf);
  std::snprintf(out, n, "queries pending-polls %ld flushes %ld | draws %ld, updates %ld, map writes %ld, copies %ld, %.1f MB written, views %zu, ctx errors %ld | dxvk: pipelines %llu, render passes %llu, barriers %llu, submits %llu, gpu syncs %llu",
                qp, qf, g_ndraw, g_nupd, g_nmapw, g_ncopy, g_cbytes / 1048576.0, g_views.v.size() - g_views.fr.size(), g_ncerr,
                (unsigned long long)sc.getCtr(DxvkStatCounter::PipeCountGraphics), (unsigned long long)sc.getCtr(DxvkStatCounter::CmdRenderPassCount),
                (unsigned long long)sc.getCtr(DxvkStatCounter::CmdBarrierCount), (unsigned long long)sc.getCtr(DxvkStatCounter::QueueSubmitCount),
                (unsigned long long)sc.getCtr(DxvkStatCounter::GpuSyncCount));
  size_t l = std::strlen(out);
  if (l + 48 < (size_t)n) std::snprintf(out + l, n - l, " | binds %ld, redundant skipped %ld", g_bc_bind, g_bc_skip);
  if (tiler_flush()) { l = std::strlen(out); if (l + 64 < (size_t)n) std::snprintf(out + l, n - l, ", flushes moved to pass end %ld (uploads %ld)", g_nflush_deferred, g_nupflush_deferred); }
  if (g_stage) { l = std::strlen(out); if (l + 48 < (size_t)n) std::snprintf(out + l, n - l, " | upload staging ring: %ld allocs (%ld large)", g_stage_n, g_stage_big); }
  s2_stats();
  if (bc_verify()) { l = std::strlen(out); if (l + 32 < (size_t)n) std::snprintf(out + l, n - l, ", verify mismatches %ld", g_bc_bad); }
  { double tsm; long tsn; ts_stats(&tsm, &tsn); l = std::strlen(out); if (tsn && l + 40 < (size_t)n) std::snprintf(out + l, n - l, " | gpu-ts span %.1f ms/frame", tsm); }
  if (gp::on()) gp::report();
  if (pp::on()) { std::string t; long tot = 0; for (int i = 0; i < 4096; i++) if (pp::ends[i]) { tot += pp::ends[i]; t += " L" + std::to_string(i) + ":" + std::to_string(pp::ends[i]); }
                  std::fprintf(stderr, "[b1pp] render pass ends %ld by emit line:%s\n", tot, t.c_str());
                  t.clear(); tot = 0; double mp = 0; char tmp[64];
                  for (int i = 0; i < 4096; i++) if (pp::resumed[i]) { tot += pp::resumed[i]; mp += pp::resumed_mpix[i]; std::snprintf(tmp, sizeof tmp, " L%d:%ld(%.0f Mpix)", i, pp::resumed[i], pp::resumed_mpix[i]); t += tmp; }
                  std::fprintf(stderr, "[b1pp] passes resumed on the same targets %ld (%.0f Mpix of attachments; %ld more began with a clear) after:%s\n", tot, mp, pp::fresh, t.c_str()); }
  if (cs_on()) { l = std::strlen(out); if (l + 96 < (size_t)n) std::snprintf(out + l, n - l, " | cs: chunks %ld, syncs %ld (waited %ld, %.1f ms), worker errors %ld",
                                                                            g_cs_chunks, g_cs_syncs, g_cs_waits, g_cs_wait_us / 1000.0, g_cs_err.load()); }
}

/* =====================================================================================================================
   B1.6 native: b1 presents to the game's window (bridge off). hostwnd = SDL_Window* (Linux) or ANativeWindow* (Quest),
   as the bridge's br_handle() gives it; DXVK's WSI (DXVK_WSI_DRIVER) takes it as the HWND.
   ===================================================================================================================== */
#include "dxvk_presenter.h"
#include "dxvk_swapchain_blitter.h"
#include "../wsi/wsi_window.h"
#include "../util/sync/sync_signal.h"
namespace {
  Rc<Presenter> g_pres; Rc<DxvkSwapchainBlitter> g_blit; Rc<sync::Fence> g_fence; uint64_t g_pframe; void *g_wnd;
  long g_npres, g_npresfail;
  /* diagnostics: span of the GPU timestamps the game read per presented frame (it drives the game's detail scaling) */
  uint64_t g_ts_lo = ~0ull, g_ts_hi; double g_ts_ms; long g_ts_n;
}
extern "C" int b1dx_present_init(void *hostwnd, uint32_t w, uint32_t h) {
  if (g_dev == nullptr || !hostwnd) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    g_wnd = hostwnd;
    g_fence = new sync::Fence(0);
    PresenterDesc desc = { };
    Rc<DxvkInstance> inst = g_inst;
    g_pres = new Presenter(g_dev, g_fence, desc, [inst, hostwnd] (VkSurfaceKHR *surface) {
      return wsi::createSurface(reinterpret_cast<HWND>(hostwnd), inst->vki()->getLoaderProc(), inst->handle(), surface); });
    /* 13 Oct (build 59): ask for R8G8B8A8_UNORM, the format of the shadow back buffer and what the bridge's DXVK swap
       chain gets on the Quest. B8G8R8A8 is not offered by the Android surface, so the Presenter fell back to
       A2B10G10R10 (10-bit) there. BSI_B1_SWAPFMT=bgra: the old request. */
    { const char *e = std::getenv("BSI_B1_SWAPFMT"); bool bgra = e && !std::strcmp(e, "bgra");
      g_pres->setSurfaceFormat({ bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }); }
    g_pres->setSurfaceExtent({ w, h });
    g_pres->setSyncInterval(0);
    g_blit = new DxvkSwapchainBlitter(g_dev, nullptr);
    std::fprintf(stderr, "[b1] presenter ready (%ux%u)\n", w, h);
    return 1;
  } catch (const DxvkError& e) { std::fprintf(stderr, "[b1] presenter init failed: %s\n", e.message().c_str()); }
  g_pres = nullptr;
  return 0;
}
/* vr70: presenter frames finished on the GPU (DXVK's frame signal, given from its finish thread); vr_stereo.c pairs it with
   vr_stereo_frame_queued to know which game frame the compositor can latch (route A) */
extern "C" unsigned long long b1dx_frames_completed(void) { sync::Fence *f = g_fence.ptr(); return f ? f->value() : 0; }   /* set once, never freed */
extern "C" void b1dx_present_resize(uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> lk(g_rmu);
  cs_sync();   /* CS: no present in flight on the worker while the surface changes */
  if (g_pres != nullptr) g_pres->setSurfaceExtent({ w, h });
}
/* end of frame in native mode: blit the shadow back buffer to the window and present */
static int b1dx_present_inner(uint32_t sync_interval);
extern "C" int b1dx_present(uint32_t sync_interval) {
  uint64_t t0 = now_us(); int r = b1dx_present_inner(sync_interval); g_wt_pres += now_us() - t0; return r;
}
static int b1dx_present_inner(uint32_t sync_interval) {
  if (g_ctx == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    if (g_dump_at == -2) { const char *e = std::getenv("BSI_B1_DUMP"); g_dump_at = e ? std::atoi(e) : -1; }
    g_frames++; b1eo_hook_frame(); b1ab_hook_frame();
    shsig_frame();   /* perf73 diagnostic */
    ext72_beam();   /* ext72 */
    if (g_dump_tag >= 0 || (g_dump_at > 0 && g_frames % g_dump_at == 0 && g_frames / g_dump_at <= 4)) { dump_bb(g_frames); g_dump_tag = -1; }
    Obj *bb = obj(g_bb);
    b1aa_frame_end();   /* aa76 */
    if (bb) b1vr::present((uint32_t)g_br_presents, bb->img->info().extent.width, bb->img->info().extent.height);   /* vr69 */
    b1aa_frame_begin();   /* aa76 */
    static int noblit = -1; if (noblit < 0) { const char *e = std::getenv("BSI_B1_NOBLIT"); noblit = e && std::atoi(e) > 0; }
    if (g_pres == nullptr || !bb || noblit) { ctx_flush(); cs_dispatch(); g_staged = 0; return 0; }
    /* 13 Oct (build 59): dxgi.syncInterval from DXVK_CONFIG_FILE overrides the game's value, as in the bridge's DXGI
       (Quest dxvk.conf: 0 -> MAILBOX; we used the game's 1 -> FIFO). BSI_B1_SYNC=<n> overrides both. */
    { static int forced = -2;
      if (forced == -2) { forced = Config::getUserConfig().getOption<int32_t>("dxgi.syncInterval", -1);
        const char *e = std::getenv("BSI_B1_SYNC"); if (e && *e) forced = std::atoi(e);
        std::fprintf(stderr, "[b1] present: sync interval %s%d\n", forced >= 0 ? "forced " : "from the game, first ", forced >= 0 ? forced : (int)sync_interval); }
      if (forced >= 0) sync_interval = (uint32_t)forced; }
    if (vr_stereo_present_fifo && vr_stereo_present_fifo()) {   /* vr69: route B, every frame must reach the AImageReader */
      static int nl; if (!nl++) std::fprintf(stderr, "[b1vr] present: FIFO (VR route B: no frame dropped, image k = queued present k)\n");
      sync_interval = 1; }
    g_pres->setSyncInterval(sync_interval);
    PresenterSync sync = { }; Rc<DxvkImage> swap;
    VkResult st = g_pres->acquireNextImage(sync, swap);
    if (st != VK_SUCCESS || swap == nullptr) {
      if (g_npresfail++ < 10) std::fprintf(stderr, "[b1] acquire failed: %d\n", st);
      ctx_flush(); cs_dispatch(); g_staged = 0; return 0; }
    DxvkImageViewKey k = { };
    k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; k.format = swap->info().format;
    k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipCount = 1; k.layerCount = 1;
    Rc<DxvkImageView> dst = swap->createView(k);
    DxvkImageViewKey sk = { };
    sk.viewType = VK_IMAGE_VIEW_TYPE_2D; sk.usage = VK_IMAGE_USAGE_SAMPLED_BIT; sk.format = bb->fmt;
    sk.aspects = VK_IMAGE_ASPECT_COLOR_BIT; sk.mipCount = 1; sk.layerCount = 1;
    Rc<DxvkImageView> src = bb->img->createView(sk);
    VkExtent3D se = bb->img->info().extent; VkExtent3D de = swap->info().extent;
    { static int nl; if (nl++ < 2 || (de.width != se.width || de.height != se.height) && nl < 6)
        std::fprintf(stderr, "[b1] present: back buffer %ux%u fmt %d -> swap image %ux%u fmt %d colour space %d (%s)\n", se.width, se.height, (int)bb->fmt, de.width, de.height,
                     (int)swap->info().format, (int)swap->info().colorSpace, (de.width == se.width && de.height == se.height) ? "1:1 copy" : "SCALED: the blitter filters"); }   /* vr72: + colour space, 1:1 or scaled */
    VkRect2D srect = { { 0, 0 }, { se.width, se.height } }, drect = { { 0, 0 }, { de.width, de.height } };
    VkClearColorValue cc = { };
    g_pframe++;
    if (gp::on()) gp::on_present();
    /* CS: acquire stays here (as DXVK's D3D11SwapChain::PresentImage), the blit + submit + present run on the worker */
    emit([dst, drect, src, srect, cc, sync, fid = g_pframe, blit = g_blit, pres = g_pres, dev = g_dev, fence = g_fence] (DxvkContext* c) {
      try {
        Rc<DxvkCommandList> cl = c->beginExternalRendering();
        blit->present(cl, cc, dst, drect, src, srect);
        c->synchronizeWsi(sync);
        c->flushCommandList(nullptr, nullptr);
        dev->presentImage(pres, nullptr, fid, 0, nullptr, nullptr);
      } catch (const DxvkError& e) {   /* CS: the frame id is spent; release the producer's fence wait on it */
        if (fence->value() < fid) fence->signal(fid);
        throw;
      } });
    note_flush();
    if (b1vr::on && vr_stereo_image_presented) vr_stereo_image_presented((uint32_t)g_br_presents);   /* vr69: image -> Present */
    if (vr_stereo_frame_queued) vr_stereo_frame_queued(g_pframe, (uint32_t)g_br_presents);   /* vr70: frame id -> Present (route A pick, latency) */
    if (gp::on()) gp::on_frame_start();
    cs_dispatch(); g_staged = 0;
    if (g_cs_mode == 2) cs_sync();
    /* frames in flight (CS: also bounds how far the producer runs ahead). Build 62: dxgi.maxFrameLatency from DXVK_CONFIG_FILE
       as the bridge's DXGI uses it (Quest dxvk.conf: 3), else 2 as before; BSI_B1_LATENCY=<n> overrides. */
    { static int lat = -1; static bool lat_env;
      if (lat < 0) { lat = Config::getUserConfig().getOption<int32_t>("dxgi.maxFrameLatency", 0); if (lat <= 0) lat = 2;
        const char *e = std::getenv("BSI_B1_LATENCY"); if (e && std::atoi(e) > 0) { lat = std::atoi(e); lat_env = true; }
        if (lat > 8) lat = 8; std::fprintf(stderr, "[b1] present: up to %d frames in flight\n", lat); }
      int fl = b1vr::fif(lat, lat_env);   /* vr70: stereo fif= (vr_stereo.txt), flat head-look fif= (vr_head.txt); else lat */
      if (g_pframe > (uint64_t)fl) { uint64_t t1 = now_us(); g_fence->wait(g_pframe - fl); g_wt_lat += now_us() - t1; } }
    g_npres++;
    if (g_ts_hi > g_ts_lo) { g_ts_ms += (double)(g_ts_hi - g_ts_lo) * g_dev->properties().core.properties.limits.timestampPeriod * 1e-6; g_ts_n++; }
    g_ts_lo = ~0ull; g_ts_hi = 0;
    return 1;
  } catch (const DxvkError& e) { if (g_npresfail++ < 10) std::fprintf(stderr, "[b1] present failed: %s\n", e.message().c_str()); }
  return 0;
}

static bool cpu_ts() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_CPUTS"); v = e && std::atoi(e) > 0; } return v > 0; }
/* ---- queries (b1n): occlusion from the GPU (UE3 culls with them; the null device's fake "always visible" answer made the
   game draw ~4x more), events complete when their frame was presented, timestamps answered on the CPU side ---- */
namespace {
  struct Q { uint32_t type = 0; Rc<DxvkQuery> q, q2; Rc<DxvkEvent> ev; uint64_t endframe = 0; bool ended = false; long d0 = 0, nd = -1;
             uint64_t endseq = 0, flushno = 0; };   /* CS: chunk that holds the last End; number of the first flush after it */
  long g_qhist[6];   /* draws inside an occlusion query: 0, 1, 2, 3-9, 10-99, 100+ */
  Table<Q> g_qs; long g_nqpend, g_nqflush;
  /* CS: before reading a query / event the worker must have executed its End (as D3D11Query's m_resetCtr) */
  /* CS: before reading a query / event the worker must have executed its End and the flush that submits it, so the
     bounded GPU wait below means what it meant without CS (else a worker that lags > 100 ms, e.g. while compiling
     pipelines, would turn into S_FALSE answers the game never saw before) */
  void q_sync(const Q *q) {
    if (!cs_on()) return;
    uint64_t s = q->endseq;
    if (g_nflush >= q->flushno) s = std::max(s, g_nflush - q->flushno < 1000 ? g_flushseq[q->flushno & 1023] : s);
    if (s > g_csthr->lastSequenceNumber()) cs_sync(s);
  }
  void q_timeout_log(const char *what) { static int nl; if (cs_on() && nl++ < 20) std::fprintf(stderr, "[b1cs] %s result still pending after the bounded GPU wait\n", what); }
}
extern "C" uint32_t b1dx_query_create(uint32_t type) {   /* D3D11_QUERY: 0 event, 1 occlusion, 5 occlusion predicate, others: CPU answers */
  if (g_dev == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_rmu);
  try {
    Q q; q.type = type;
    if (type == 1 || type == 5) q.q = g_dev->createGpuQuery(VK_QUERY_TYPE_OCCLUSION, type == 1 ? VK_QUERY_CONTROL_PRECISE_BIT : 0, 0);
    if (type == 0) q.ev = g_dev->createGpuEvent();   /* event: signalled by the GPU when the work before End is done */
    /* timestamps as DXVK's D3D11 (d3d11_query.cpp): TIMESTAMP = one GPU timestamp at End; TIMESTAMP_DISJOINT = GPU
       timestamps at Begin (q2) and End (q), Disjoint = end < begin, Frequency = 1e9 / timestampPeriod.
       13 Oct: before this the null device answered timestamps with the CPU clock in ns while the disjoint frequency
       came from the GPU (Turnip 19.2 MHz): the game's GPU-time-driven detail scaling saw a 52x too slow GPU and
       dropped to low detail on the Quest (low-poly, blurry). BSI_B1_CPUTS=1: old behaviour. */
    if ((type == 2 || type == 3) && !cpu_ts()) { q.q = g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0); if (type == 3) q.q2 = g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0); }
    return g_qs.add(std::move(q));
  } catch (const DxvkError& e) { cerr("query create", e); }
  return 0;
}
extern "C" void b1dx_query_release(uint32_t id) { std::lock_guard<std::mutex> lk(g_rmu); g_qs.del(id); }
extern "C" void b1dx_c_query_begin(uint32_t id) {
  CTX_GUARD()
  Q *q = g_qs.get(id); if (!q) return;
  if (q->type == 3) { if (q->q2 != nullptr) emit([qq = q->q2] (DxvkContext* c) { c->writeTimestamp(qq); }); }
  else if (q->type == 2) { }
  else if (q->q != nullptr) emit([qq = q->q] (DxvkContext* c) { c->beginQuery(qq); });
  q->ended = false; q->d0 = g_ndraw;
  CTX_END("query begin")
}
extern "C" void b1dx_c_query_end(uint32_t id) {
  CTX_GUARD()
  Q *q = g_qs.get(id); if (!q) return;
  if (q->ev != nullptr) emit([ev = q->ev] (DxvkContext* c) { c->signalGpuEvent(ev); });
  if ((q->type == 2 || q->type == 3) && q->q != nullptr) emit([qq = q->q] (DxvkContext* c) { c->writeTimestamp(qq); });
  else if (q->q != nullptr) { emit([qq = q->q] (DxvkContext* c) { c->endQuery(qq); }); long n = g_ndraw - q->d0; q->nd = n; g_qhist[n <= 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n < 10 ? 3 : n < 100 ? 4 : 5]++;
    static long t; if ((++t % 20000) == 0) std::fprintf(stderr, "[b1q] draws per occlusion query: 0:%ld 1:%ld 2:%ld 3-9:%ld 10-99:%ld 100+:%ld\n", g_qhist[0], g_qhist[1], g_qhist[2], g_qhist[3], g_qhist[4], g_qhist[5]); }
  q->endframe = g_pframe + 1; q->ended = true;   /* completes once this frame is presented */
  q->endseq = g_cs_seq + 1;                      /* CS: the chunk being recorded */
  q->flushno = g_nflush + 1;
  CTX_END("query end")
}
/* returns 0 = data written, 1 = not ready (S_FALSE), -1 = not ours */
/* returns 0 = data written, 1 = not ready (S_FALSE), -1 = not ours.
   Waiting policy: a query whose frame was already submitted is waited for here (bounded), instead of answering S_FALSE:
   UE3 treats an unavailable occlusion result as "visible" (3x the draws) and spins on events (~250k polls a frame). */
extern "C" int b1dx_c_query_data(uint32_t id, void *out, uint32_t size, uint32_t flags) {
  std::unique_lock<std::mutex> lk(g_rmu);
  Q *q = g_qs.get(id);
  { static long cnt[8]; uint32_t t = q ? (q->type < 7 ? q->type : 7) : 7; cnt[t]++;
    static long tot; if ((++tot % 50000) == 0) std::fprintf(stderr, "[b1q] GetData by type: event %ld occl %ld ts %ld disjoint %ld stats %ld pred %ld other %ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6] + cnt[7]); }
  if (!q) return -1;
  uint32_t type = q->type;
  if (type == 3 && q->q != nullptr) {   /* TIMESTAMP_DISJOINT with GPU timestamps: the game asks exactly once per session
      (ts2: 1 GetData in 500k) and takes the first answer, so answer now with what DXVK reports once the query is done:
      the GPU timestamp frequency, not disjoint. */
    if (out && size >= 12) { uint64_t f = (uint64_t)(1000000000.0f / g_dev->properties().core.properties.limits.timestampPeriod); uint32_t dj = 0;
      std::memcpy(out, &f, 8); std::memcpy((uint8_t *)out + 8, &dj, 4);
      std::fprintf(stderr, "[b1q] timestamp disjoint answered: frequency %llu Hz, not disjoint (GPU timestamps)\n", (unsigned long long)f); }
    return 0;
  }
  if ((type == 2 || type == 3) && q->q != nullptr) {   /* GPU timestamps (see b1dx_query_create) */
    if (g_pres == nullptr) return -1;
    if (!q->ended) return 1;
    if (q->endframe > g_pframe) {
      if (flags & 1u) { g_nqpend++; return 1; }
      try { ctx_flush(); cs_sync(); g_staged = 0; g_nqflush++; } catch (const DxvkError& e) { cerr("query flush", e); }
      q->endframe = g_pframe;
    }
    q_sync(q);
    Rc<DxvkQuery> a = q->q, b = q->q2; DxvkQueryData da = { }, db = { };
    /* build 62: as DXVK's D3D11 (d3d11_context_imm.cpp GetData): a timestamp that isn't done yet answers S_FALSE right away
       (the flush above already submitted it). We used to wait here for the GPU (up to 100 ms); in the cloud the render
       thread sat 54-138 ms per frame in this wait (the cloud is CPU-saturated, so its fps can't show the gain).
       BSI_B1_TSWAIT=1: the old wait. */
    { static int tw = -1; if (tw < 0) { const char *e = std::getenv("BSI_B1_TSWAIT"); tw = e && std::atoi(e) > 0; }
      if (!tw) { DxvkGpuQueryStatus sa = a->getData(da), sb = b != nullptr ? b->getData(db) : DxvkGpuQueryStatus::Available;
        if (sa == DxvkGpuQueryStatus::Pending || sb == DxvkGpuQueryStatus::Pending) { g_nqpend++; g_ts_sfalse++; return 1; } } }
    lk.unlock(); uint64_t t0 = now_us();
    DxvkGpuQueryStatus sa = a->getData(da), sb = b != nullptr ? b->getData(db) : DxvkGpuQueryStatus::Available;
    for (int i = 0; i < 2000 && (sa == DxvkGpuQueryStatus::Pending || sb == DxvkGpuQueryStatus::Pending); i++) {
      struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr);
      if (sa == DxvkGpuQueryStatus::Pending) sa = a->getData(da);
      if (sb == DxvkGpuQueryStatus::Pending) sb = b->getData(db); }
    lk.lock(); g_wt_ts += now_us() - t0;
    if (sa == DxvkGpuQueryStatus::Pending || sb == DxvkGpuQueryStatus::Pending) { g_nqpend++; q_timeout_log("timestamp"); return 1; }
    if (sa != DxvkGpuQueryStatus::Available || sb != DxvkGpuQueryStatus::Available) return -1;
    g_ts_reads++;
    if (type == 2) { if (out && size >= 8) { uint64_t tv = b1vr::ts(da.timestamp.time); std::memcpy(out, &tv, 8); }   /* vr69: gpu_ts=eye */
                     if (da.timestamp.time < g_ts_lo) g_ts_lo = da.timestamp.time; if (da.timestamp.time > g_ts_hi) g_ts_hi = da.timestamp.time; }
    else if (out && size >= 12) { uint64_t f = (uint64_t)(1000000000.0f / g_dev->properties().core.properties.limits.timestampPeriod); uint32_t dj = da.timestamp.time < db.timestamp.time;
      std::memcpy(out, &f, 8); std::memcpy((uint8_t *)out + 8, &dj, 4);
      static int nl; if (nl++ < 3) std::fprintf(stderr, "[b1q] timestamp disjoint: frequency %llu Hz, disjoint %u, span %.3f ms\n", (unsigned long long)f, dj, (double)(da.timestamp.time - db.timestamp.time) * 1000.0 / (double)f); }
    return 0;
  }
  if (type == 3) {   /* TIMESTAMP_DISJOINT (BSI_B1_CPUTS=1 only): as DXVK's D3D11 answers it (Disjoint = begin < end, i.e. TRUE): the game then
                        ignores GPU timings. The null device's "1 GHz, not disjoint" + CPU-clock timestamps made the game
                        measure a ~0 ms GPU and raise its detail (3-5x the draws). */
    if (out && size >= 12) { uint64_t f = (uint64_t)(1000000000.0f / g_dev->properties().core.properties.limits.timestampPeriod); uint32_t dj = 1;
                             std::memcpy(out, &f, 8); std::memcpy((uint8_t *)out + 8, &dj, 4); }
    return 0;
  }
  if (type != 0 && type != 1 && type != 5) return -1;
  if ((type == 1 || type == 5) && q->q == nullptr) return -1;
  if (g_pres == nullptr) return -1;   /* offscreen (no presenter): the null device answers */
  if (q->ended && q->endframe > g_pframe) {   /* ended in the frame being recorded */
    if (flags & 1u) { g_nqpend++; return 1; }  /* DONOTFLUSH */
    try { ctx_flush(); cs_sync(); g_staged = 0; g_nqflush++; } catch (const DxvkError& e) { cerr("query flush", e); }
    q->endframe = g_pframe;
  }
  if (type == 0) {
    if (q->ev == nullptr || !q->ended) { if (out && size >= 4) { uint32_t v = 1; std::memcpy(out, &v, 4); } return 0; }
    q_sync(q);
    Rc<DxvkEvent> ev = q->ev; bool cur = q->endframe > g_pframe;
    DxvkGpuEventStatus st = ev->test();
    if (st == DxvkGpuEventStatus::Pending && !cur) {   /* submitted: wait here (bounded) instead of letting the game spin */
      lk.unlock(); uint64_t t0 = now_us();
      for (int i = 0; i < 4000 && st == DxvkGpuEventStatus::Pending; i++) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr); st = ev->test(); }
      lk.lock(); g_wt_ev += now_us() - t0;
    }
    if (st == DxvkGpuEventStatus::Pending) { g_nqpend++; if (!cur) q_timeout_log("event"); return 1; }
    if (out && size >= 4) { uint32_t v = 1; std::memcpy(out, &v, 4); }
    return 0;
  }
  if (cs_on()) { if (!q->ended) return -1;   /* begun / never used: what DXVK answers in place (Invalid), without racing the worker's begin */
                 q_sync(q); }
  Rc<DxvkQuery> dq = q->q; DxvkQueryData d = { };
  DxvkGpuQueryStatus st = dq->getData(d);
  if (st == DxvkGpuQueryStatus::Pending) {
    lk.unlock(); uint64_t t0 = now_us();
    for (int i = 0; i < 2000 && st == DxvkGpuQueryStatus::Pending; i++) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr); st = dq->getData(d); }   /* <= 100 ms */
    lk.lock(); g_wt_oq += now_us() - t0;
    if (st == DxvkGpuQueryStatus::Pending) { g_nqpend++; q_timeout_log("occlusion"); return 1; }
  }
  if (st != DxvkGpuQueryStatus::Available) return -1;
  { static int z = -1; if (z < 0) { const char *e = std::getenv("BSI_B1_OCCL0"); z = e && std::atoi(e) > 0; } if (z) d.occlusion.samplesPassed = 0; }   /* experiment */
  { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_OCCL1"); v = e && std::atoi(e) > 0; if (v) std::fprintf(stderr, "[b1q] BSI_B1_OCCL1: every occlusion query answers visible\n"); }
    if (v && type == 1) d.occlusion.samplesPassed = 100000; }   /* build 62 experiment: rules out wrong 0-sample answers (objects culled) */
  { if (type == 1) { if (d.occlusion.samplesPassed) g_oq_vis++; else g_oq_zero++; } }
  { static int nl; if (nl < 12 && type == 1) { nl++; std::fprintf(stderr, "[b1q] result: query id %u samples %llu\n", id, (unsigned long long)d.occlusion.samplesPassed); } }
  if (out && size >= 8 && type == 1) { uint64_t v = d.occlusion.samplesPassed; std::memcpy(out, &v, 8); }
  else if (out && size >= 4) { uint32_t v = d.occlusion.samplesPassed != 0; std::memcpy(out, &v, 4); }
  return 0;
}
/* perf diagnostics (build 62): microseconds the render thread spent blocked, for the [b1perf] line (rhi_b1_com.c) */
extern "C" void b1dx_ts_stats(uint64_t *reads, uint64_t *sfalse) { *reads = g_ts_reads; *sfalse = g_ts_sfalse; }
extern "C" void b1dx_oq_stats(uint64_t *vis, uint64_t *zero) { *vis = g_oq_vis; *zero = g_oq_zero; }
extern "C" void b1dx_wait_stats(uint64_t *ev, uint64_t *oq, uint64_t *ts, uint64_t *pres, uint64_t *lat, uint64_t *cs) { *ev = g_wt_ev; *oq = g_wt_oq; *ts = g_wt_ts; *pres = g_wt_pres; *lat = g_wt_lat; *cs = g_cs_wait_us; }
static void query_stats(long *pend, long *flush) { *pend = g_nqpend; *flush = g_nqflush; }
static void ts_stats(double *avg_ms, long *frames) { *avg_ms = g_ts_n ? g_ts_ms / g_ts_n : 0.0; *frames = g_ts_n; g_ts_ms = 0; g_ts_n = 0; }

/* b1n: IDXGIAdapter::GetDesc from our Vulkan adapter, as the bridge's DXVK DXGI reports it (the null device's fake
   GTX 680 made the game pick other defaults) */
extern "C" int b1dx_adapter_desc(char *name, int n, uint32_t *ven, uint32_t *dev, uint64_t *vram, uint64_t *shared) {
  if (g_dev == nullptr) return 0;
  Rc<DxvkAdapter> a = g_dev->adapter(); auto info = a->info();
  std::snprintf(name, n, "%s", info.deviceName); *ven = info.vendorId; *dev = info.deviceId;
  /* exactly as DXVK's DXGI (dxgi_adapter.cpp GetDesc) reports it under the bridge: largest device-local heap, sum of
     the others, dxgi.maxDeviceMemory / dxgi.maxSharedMemory from DXVK_CONFIG_FILE (Quest default 2048 MB), and the
     32-bit clamp. 13 Oct: we reported 2047.99 MB (0x7fffffff) on the Quest vs the bridge's 2048 MB and the game looked
     low-detail in b1n; the game sizes its detail/texture settings from this value. */
  const VkPhysicalDeviceMemoryProperties &mp = a->memoryProperties(); uint64_t dl = 0, sys = 0;
  for (uint32_t i = 0; i < mp.memoryHeapCount; i++) { if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) { if (mp.memoryHeaps[i].size > dl) dl = mp.memoryHeaps[i].size; } else sys += mp.memoryHeaps[i].size; }
  if (!sys) sys = dl;
  const uint64_t adj = 32ull << 20;
  if (dl && !(dl & 0xffffffffull)) dl -= adj;
  if (sys && !(sys & 0xffffffffull)) sys -= adj;
  { Config cfg = Config::getUserConfig();
    uint64_t md = uint64_t(cfg.getOption<int32_t>("dxgi.maxDeviceMemory", 0)) << 20, ms = uint64_t(cfg.getOption<int32_t>("dxgi.maxSharedMemory", 0)) << 20;
    { const char *e = std::getenv("BSI_B1_VRAM_MB"); if (e && std::atoi(e) > 0) md = uint64_t(std::atoi(e)) << 20; }
    if (md && md < dl) dl = md;
    if (ms && ms < sys) sys = ms; }
  /* then exactly as the bridge hands it to the 32-bit game (bridge.c ad_getdesc): each value clamped to 0x7fffffff */
  if (dl > 0x7fffffffull) dl = 0x7fffffffull;
  if (sys > 0x7fffffffull) sys = 0x7fffffffull;
  *vram = dl; *shared = sys; return 1;
}

/* diagnostics (shadow mode, BSI_B1_QCMP=1): our occlusion result for a query, waited for (flushes if needed) */
extern "C" int b1dx_query_peek(uint32_t id, uint64_t *samples) {
  std::unique_lock<std::mutex> lk(g_rmu);
  Q *q = g_qs.get(id); if (!q || q->q == nullptr || !q->ended) return -1;
  try { ctx_flush(); cs_sync(); g_staged = 0; } catch (...) { return -1; }
  Rc<DxvkQuery> dq = q->q; lk.unlock();
  DxvkQueryData d = { }; DxvkGpuQueryStatus st = dq->getData(d);
  for (int i = 0; i < 4000 && st == DxvkGpuQueryStatus::Pending; i++) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr); st = dq->getData(d); }
  if (st != DxvkGpuQueryStatus::Available) return -1;
  *samples = d.occlusion.samplesPassed; return 0;
}
extern "C" int b1dx_view_info(uint32_t id, uint32_t *res, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *mip, uint32_t *layer) {
  std::lock_guard<std::mutex> lk(g_rmu);
  View *v = g_views.get(id); if (!v) return 0;
  *res = v->res; VkExtent3D e = v->iv->mipLevelExtent(0); *w = e.width; *h = e.height; *fmt = v->iv->info().format; *mip = v->iv->info().mipIndex; *layer = v->iv->info().layerIndex;
  return v->type;
}
/* diagnostics: fraction of the bound depth target nearer than 0.999 (and min depth), read back now */
static int g_depth_dumped;
extern "C" void b1dx_depth_rearm(void) { g_depth_dumped = 0; }
extern "C" int b1dx_depth_probe(float *frac, float *mind) {
  std::lock_guard<std::mutex> lk(g_rmu);
  View *v = g_views.get(g_cs.dsv); if (!v || v->type != 3) return 0;
  Obj *o = robj(v->res); if (!o) return 0;
  try {
    VkExtent3D ext = o->img->mipLevelExtent(0); VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * 4;
    DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
    emit([rb, fmt = o->fmt, img = o->img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint32_t *p = (const uint32_t *)rb->mapPtr(0); size_t n = (size_t)ext.width * ext.height, near = 0; float mn = 1.f;
    bool d24 = o->fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    for (size_t i = 0; i < n; i++) { float d; if (d24) d = (p[i] & 0xffffff) / 16777215.f; else std::memcpy(&d, &p[i], 4); if (d < 0.999f) near++; if (d < mn) mn = d; }
    *frac = (float)near / (float)n; *mind = mn;
    { const char *dd = std::getenv("BSI_B1_DUMPDIR"); if (!g_depth_dumped++ && dd) {   /* depth image: (1-d)*K stretched */
        char path[512]; std::snprintf(path, sizeof path, "%s/depth_at_query_%ld.pgm", dd, g_frames); FILE *f = std::fopen(path, "wb");
        if (f) { std::fprintf(f, "P5\n%u %u\n255\n", ext.width, ext.height);
          for (size_t i = 0; i < n; i++) { float d = d24 ? (p[i] & 0xffffff) / 16777215.f : 0.f; int v = (int)((1.f - d) * 255.f * 40.f); std::fputc(v > 255 ? 255 : v, f); }
          std::fclose(f); std::fprintf(stderr, "[b1q] depth image written to %s\n", path); } } }
    return 1;
  } catch (const DxvkError& e) { cerr("depth probe", e); }
  return 0;
}
extern "C" void b1dx_state_dump(void) {
  std::lock_guard<std::mutex> lk(g_rmu);
  DSS *d = g_dsss.get(g_cs.dss); RS *r = g_rss.get(g_cs.rs);
  std::fprintf(stderr, "[b1q]   state: dss id %u %s test %d write %d op %d | rs id %u %s cull %u depthclip %d bias %.1f/%.2f scissor %d | vp n %u [%.0f %.0f %.0f %.0f z %.3f..%.3f] dirty %d\n",
    g_cs.dss, d ? "" : "(default)", d ? d->s.depthTest() : -1, d ? d->s.depthWrite() : -1, d ? (int)d->s.depthCompareOp() : -1,
    g_cs.rs, r ? "" : "(default)", r ? r->s.cullMode() : 99u, r ? r->s.depthClip() : -1, r ? r->b.depthBiasConstant : 0.f, r ? r->b.depthBiasSlope : 0.f, r ? r->scissor : -1,
    g_cs.nvp, g_cs.nvp ? g_cs.vp[0][0] : 0, g_cs.nvp ? g_cs.vp[0][1] : 0, g_cs.nvp ? g_cs.vp[0][2] : 0, g_cs.nvp ? g_cs.vp[0][3] : 0, g_cs.nvp ? g_cs.vp[0][4] : 0, g_cs.nvp ? g_cs.vp[0][5] : 0, g_cs.vpdirty);
}
extern "C" int b1dx_buf_read(uint32_t id, uint32_t off, uint32_t n, void *out) {   /* diagnostics: current contents (waits for the GPU) */
  std::lock_guard<std::mutex> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1 || !(o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 0;
  ctx_flush(); cs_sync(); g_dev->waitForIdle();
  if (off + n > o->buf->info().size) return 0;
  std::memcpy(out, (const uint8_t *)o->buf->mapPtr(0) + off, n); return 1;
}

#include "rhi_b1_shadoweo.inc"   /* S1 shadow early-out */
#include "rhi_b1_aoblur.inc"     /* V4 HDAO blur twins */
#include "rhi_b1_aa.inc"         /* aa76: opt-in VR anti-aliasing (TAA / SMAA) */
