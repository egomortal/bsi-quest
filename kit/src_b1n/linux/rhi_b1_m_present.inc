/* rhi_b1_m_present.inc: B1.6 present, queries, adapter description, diagnostics readbacks. Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
  g_rmu.rt_claim();   /* c5b141 RTLOCK: the presenting thread becomes the biased owner (once) */
  try {
    if (g_dump_at == -2) { const char *e = std::getenv("BSI_B1_DUMP"); g_dump_at = e ? std::atoi(e) : -1; }
    dc88::flush_all(dc88::n_present);   /* dc88: frame end: everything still pending is emitted in order */
    g4_present();   /* g4 */ if (g_rawcap) c5_present();   /* c5: graveyard prune + [b1rc5] stats */
    g_frames++; b1eo_hook_frame(); b1ab_hook_frame(); sb87::frame();   /* sb87 */ shrep::frame();   /* b115 */ sheye::frame();   /* b123 */
    shsig_frame();   /* perf73 diagnostic */
    ext72_beam();   /* ext72 */
    if (g_dump_tag >= 0 || (g_dump_at > 0 && g_frames % g_dump_at == 0 && g_frames / g_dump_at <= 4)) { dump_bb(g_frames); g_dump_tag = -1; }
    Obj *bb = obj(g_bb);
    b1aa_frame_end();   /* aa76 */
    if (bb) b1vr::present((uint32_t)g_br_presents, bb->img->info().extent.width, bb->img->info().extent.height);   /* vr69 */
    b1aa_frame_begin();   /* aa76 */
    bep_all();   /* c5b141 epoch: frame-end hooks (vr69 blend re-issue, g4 atlas SRVs back, eo / sb87 / aa frame state) */
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
    dyn103_present(g_pframe);   /* dyn103: ring retirement bookkeeping + [b1dyn] stats */
    tl126::on_present_start();   /* b126 slack "present" */
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
    { bool gpw = gp::on(); double gts = g_ts_hi > g_ts_lo ? (double)(g_ts_hi - g_ts_lo) * g_dev->properties().core.properties.limits.timestampPeriod * 1e-6 : 0.0;   /* b126 */
      tl126::ab_present(gts); tl126::gts_frame(gts, b1vr::ts_k); tl126::vis_frame();
      if (!gpw && gp::on()) { gp::ring[gp::cur].n = 0; gp::ring[gp::cur].live = false; gp::on_frame_start(); }   /* ABAB: profiler back on: fresh slot */
      tl126::on_frame_start(); }
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
    g_rmu.rt_present();   /* c5b141 RTLOCK: bias policy every 60 presents, [b1lock] stats every 600 (g_rmu held) */
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
extern "C" void b1dx_query_release(uint32_t id) { std::lock_guard<RmuLock> lk(g_rmu); g_qs.del(id); }
extern "C" void b1dx_c_query_begin(uint32_t id) {
  CTX_GUARD()
  Q *q = g_qs.get(id); if (!q) return;
  if (q->type == 1 || q->type == 5) g4_flush_if_open("query");   /* g4: an occlusion query must see the deferred draws in order */
  if (q->type == 3) { if (q->q2 != nullptr) emit([qq = q->q2] (DxvkContext* c) { c->writeTimestamp(qq); }); }
  else if (q->type == 2) { }
  else if (q->q != nullptr) emit([qq = q->q] (DxvkContext* c) { c->beginQuery(qq); });
  q->ended = false; q->d0 = g_ndraw;
  CTX_END("query begin")
}
extern "C" void b1dx_c_query_end(uint32_t id) {
  CTX_GUARD()
  Q *q = g_qs.get(id); if (!q) return;
  if (q->type == 1 || q->type == 5) g4_flush_if_open("query");   /* g4 */
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
  std::unique_lock<RmuLock> lk(g_rmu);
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
  if (type == 1) tl126::vis_occ(d.occlusion.samplesPassed);   /* b126 [b1vis] */
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
  std::unique_lock<RmuLock> lk(g_rmu);
  Q *q = g_qs.get(id); if (!q || q->q == nullptr || !q->ended) return -1;
  try { ctx_flush(); cs_sync(); g_staged = 0; } catch (...) { return -1; }
  Rc<DxvkQuery> dq = q->q; lk.unlock();
  DxvkQueryData d = { }; DxvkGpuQueryStatus st = dq->getData(d);
  for (int i = 0; i < 4000 && st == DxvkGpuQueryStatus::Pending; i++) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, nullptr); st = dq->getData(d); }
  if (st != DxvkGpuQueryStatus::Available) return -1;
  *samples = d.occlusion.samplesPassed; return 0;
}
extern "C" int b1dx_view_info(uint32_t id, uint32_t *res, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *mip, uint32_t *layer) {
  std::lock_guard<RmuLock> lk(g_rmu);
  View *v = g_views.get(id); if (!v) return 0;
  *res = v->res; VkExtent3D e = v->iv->mipLevelExtent(0); *w = e.width; *h = e.height; *fmt = v->iv->info().format; *mip = v->iv->info().mipIndex; *layer = v->iv->info().layerIndex;
  return v->type;
}
/* diagnostics: fraction of the bound depth target nearer than 0.999 (and min depth), read back now */
static int g_depth_dumped;
extern "C" void b1dx_depth_rearm(void) { g_depth_dumped = 0; }
extern "C" int b1dx_depth_probe(float *frac, float *mind) {
  std::lock_guard<RmuLock> lk(g_rmu);
  View *v = g_views.get(g_cs.dsv); if (!v || v->type != 3) return 0;
  Obj *o = robj(v->res); if (!o) return 0; dc88::flush_all(dc88::n_rdbk);   /* dc88 */
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
  std::lock_guard<RmuLock> lk(g_rmu);
  DSS *d = g_dsss.get(g_cs.dss); RS *r = g_rss.get(g_cs.rs);
  std::fprintf(stderr, "[b1q]   state: dss id %u %s test %d write %d op %d | rs id %u %s cull %u depthclip %d bias %.1f/%.2f scissor %d | vp n %u [%.0f %.0f %.0f %.0f z %.3f..%.3f] dirty %d\n",
    g_cs.dss, d ? "" : "(default)", d ? d->s.depthTest() : -1, d ? d->s.depthWrite() : -1, d ? (int)d->s.depthCompareOp() : -1,
    g_cs.rs, r ? "" : "(default)", r ? r->s.cullMode() : 99u, r ? r->s.depthClip() : -1, r ? r->b.depthBiasConstant : 0.f, r ? r->b.depthBiasSlope : 0.f, r ? r->scissor : -1,
    g_cs.nvp, g_cs.nvp ? g_cs.vp[0][0] : 0, g_cs.nvp ? g_cs.vp[0][1] : 0, g_cs.nvp ? g_cs.vp[0][2] : 0, g_cs.nvp ? g_cs.vp[0][3] : 0, g_cs.nvp ? g_cs.vp[0][4] : 0, g_cs.nvp ? g_cs.vp[0][5] : 0, g_cs.vpdirty);
}
extern "C" int b1dx_buf_read(uint32_t id, uint32_t off, uint32_t n, void *out) {   /* diagnostics: current contents (waits for the GPU) */
  std::lock_guard<RmuLock> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1 || !(o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 0;
  ctx_flush(); cs_sync(); g_dev->waitForIdle();
  if (off + n > o->buf->info().size) return 0;
  if (const uint8_t *rp = dyn103_read_ptr(*o)) { std::memcpy(out, rp + off, n); return 1; }   /* dyn103 */
  std::memcpy(out, (const uint8_t *)o->buf->mapPtr(0) + off, n); return 1;
}
