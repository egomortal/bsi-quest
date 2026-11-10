/* rhi_b1_aoblur.inc -- V4 HDAO blur twins, runtime side. #included at the end of rhi_b1_dxvk.cpp (after rhi_b1_shadoweo.inc;
   uses that file's object tables, emit(), g_rmu and b1eo::build_cs_compare); the DXBC work is in rhi_b1_aoblur.h.
   README: /home/claude/bsi/aoblur_fix/README.md.

   BSI_B1_AOBLUR = 0 off | 1 twin (default) | 4 A/B: every blur Dispatch runs the game's shader into its UAV and again the twin
   into a copy of the UAV taken just before it; a compute pass counts the texels where the two results differ (must stay 0).
   Flow: the two allowlisted blur shaders get a twin at creation (b1ab_cs_created). At a Dispatch with such a shader bound the
   twin is bound instead (with its weights at CS cb13), then the game's shader again. The twin takes precedence over an
   AO-switch (BSI_SHADER_ALT) replacement of the same blur; the main pass keeps its AO-switch replacement (GTAO set 3).
   Diagnostics: BSI_B1_AOBLUR_SHARE=1 (one weight per tap position, shared by the thread's 4 outputs; default 0 = one per
   select, the original's data-flow graph exactly; ir3 puts either weight chain in the shader preamble, same main-body
   cost), BSI_B1_AOBLUR_BREAK=1 (deliberately wrong twin: the negative control for mode 4),
   BSI_B1_AOBLUR_REPORT=N (report interval, default 600 frames). */
#include "rhi_b1_aoblur.h"

namespace ab {
  static int env_i(const char *n, int d) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : d; }
  static int mode() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_AOBLUR", 1); if (v != 0 && v != 1 && v != 4) v = 0; } return v; }
  static bool share() { static int v = -1; if (v < 0) v = env_i("BSI_B1_AOBLUR_SHARE", 0) != 0; return v; }
  static bool brk() { static int v = -1; if (v < 0) v = env_i("BSI_B1_AOBLUR_BREAK", 0) > 0; return v; }
  static std::mutex mu;                                    /* twin table: written on the creating thread, read under g_rmu */
  static std::unordered_map<uint32_t, uint32_t> twin;      /* game CS id -> twin CS id */
  static thread_local bool creating;
  static std::atomic<long> n_twins;
  /* render state (g_rmu) */
  static Rc<DxvkBuffer> wbuf;                              /* cb13: the weights (64 x 0.125f, vec4-packed) */
  static Rc<DxvkImageView> uav0; static uint32_t uav0_res; /* the view the game bound at CS u0 */
  static long n_twin_disp, n_orig_disp, n_alt_over, n_frames; static bool failed;
  /* mode 4 */
  struct Dual { Rc<DxvkImage> src, img; Rc<DxvkImageView> uav, srv, srcsrv; };
  static std::vector<Dual> duals; static Rc<DxvkShader> cscmp; static Rc<DxvkBuffer> abbuf, pbuf; static Rc<DxvkBufferView> abview;
  static long n_ab, n_ab_skip; static uint64_t ab_total;

  /* BSI_B1_AOBLUR_STATS=N (mode 4, diagnostics): after every N-th compared dispatch, read back the blur's output (RGBA8) and
     log how much of it is occluded (.x < 255), so a "0 texels differ" can be read against non-trivial content (stalls once) */
  static long ab_dispatches;
  static void stats(Dual *d, uint32_t gx, uint32_t gy) {
    static long n = -2; if (n == -2) n = env_i("BSI_B1_AOBLUR_STATS", 0);
    if (n <= 0 || ab_dispatches % n) return;
    VkExtent3D e = d->src->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)e.width * e.height * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 AO blur stats";
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkFormat f = d->src->info().format;
    emit([rb, f, img = d->src, sl, e] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, f, img, sl, VkOffset3D{ 0, 0, 0 }, e); });
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint8_t *p = (const uint8_t *)rb->mapPtr(0); size_t np = (size_t)e.width * e.height, occ = 0, mn = 255, dist[8] = { };
    double sum = 0; std::vector<uint8_t> seen(256, 0); size_t nval = 0;
    for (size_t i = 0; i < np; i++) { uint8_t v = p[4 * i]; sum += v; if (v < 255) occ++; if (v < mn) mn = v; dist[v >> 5]++; if (!seen[v]) { seen[v] = 1; nval++; } }
    const uint32_t *ab = abbuf != nullptr ? (const uint32_t *)abbuf->mapPtr(0) : nullptr;
    std::fprintf(stderr, "[b1ab] STATS after compared dispatch %ld (%ux%u groups, output %ux%u): .x mean %.1f/255, min %zu, occluded (<255) %.1f%%, distinct values %zu, histogram/32: %zu %zu %zu %zu %zu %zu %zu %zu | texels differing so far %u\n",
      ab_dispatches, gx, gy, e.width, e.height, sum / np, mn, 100.0 * occ / np, nval, dist[0], dist[1], dist[2], dist[3], dist[4], dist[5], dist[6], dist[7], ab ? ab[1] : 0u); }

  static bool init() {
    if (failed) return false;
    if (wbuf != nullptr) return true;
    DxvkBufferCreateInfo bi = { }; bi.size = 256; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 AO blur weights";
    wbuf = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    struct W { float f[64]; } wd; for (float &f : wd.f) f = 0.125f;
    emit([b = wbuf, wd] (DxvkContext* c) { c->updateBuffer(b, 0, sizeof wd, &wd); });
    if (mode() == 4) {
      auto cc = b1eo::build_cs_compare(); uint32_t ic = b1dx_shader_create(5, cc.data(), (uint32_t)cc.size(), 1);
      if (!ic) { failed = true; std::fprintf(stderr, "[b1ab] compare shader failed to build: A/B off\n"); return false; }
      cscmp = b1dx_shader_get(ic);
      DxvkBufferCreateInfo ai = { }; ai.size = 16; ai.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;   /* b135: raw SSBO lowering of typed atomics */
      ai.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT; ai.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT; ai.debugName = "B1 AO blur A/B";   /* b133: HOST access declared, else no host barrier: the CPU read 0 */
      abbuf = g_dev->createBuffer(ai, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      std::memset(abbuf->mapPtr(0), 0, 16);
      DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 16;
      abview = abbuf->createView(vk);
      DxvkBufferCreateInfo pi = bi; pi.debugName = "B1 AO blur A/B params"; pbuf = g_dev->createBuffer(pi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    std::fprintf(stderr, "[b1ab] V4 blur twins active (mode %d, weights %s%s)\n", mode(), share() ? "per tap position" : "per select", brk() ? ", BROKEN twin (negative control)" : "");
    return true; }

  /* mode 4: the copy image for the UAV's image (same format/size; storage for the twin, sampled for the compare) */
  static Dual *dual_for(const Rc<DxvkImageView> &v) {
    Rc<DxvkImage> src = v->image();
    for (auto &d : duals) if (d.src == src) return &d;
    DxvkImageCreateInfo ii = src->info();
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.mipLevels = 1; ii.numLayers = 1; ii.flags = 0; ii.viewFormatCount = 0; ii.viewFormats = nullptr; ii.debugName = "B1 AO blur A/B copy";
    ii.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    ii.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    ii.layout = VK_IMAGE_LAYOUT_GENERAL; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    Dual nd; nd.src = src; nd.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = v->info().format; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    k.usage = VK_IMAGE_USAGE_STORAGE_BIT; k.layout = VK_IMAGE_LAYOUT_GENERAL; nd.uav = nd.img->createView(k);
    k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED; nd.srv = nd.img->createView(k);
    nd.srcsrv = src->createView(k);
    emit([i = nd.img] (DxvkContext* c) { c->initImage(i, VK_IMAGE_LAYOUT_UNDEFINED); });
    duals.push_back(std::move(nd));
    std::fprintf(stderr, "[b1ab] A/B: copy image for %ux%u fmt %d\n", ii.extent.width, ii.extent.height, (int)ii.format);
    return &duals.back(); }

  static void uav_set(uint32_t slot, View *v) { if (slot == 0) { uav0 = v ? v->iv : nullptr; uav0_res = v ? v->res : 0; } }
  static uint32_t twin_of(uint32_t cs) { if (!cs) return 0; std::lock_guard<std::mutex> lk(mu); auto it = twin.find(cs); return it == twin.end() ? 0 : it->second; }
  static void bind_w() { emit([b = wbuf] (DxvkContext* c) { c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 16u + 13u, DxvkBufferSlice(b, 0, 256)); }); }
  static void unbind_w() { emit([] (DxvkContext* c) { c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 16u + 13u, DxvkBufferSlice()); }); g_bc.cb[5][13] = BC_UNK; }

  /* b1dx_c_dispatch: true = handled here */
  static bool last_em;   /* ao74: the last handled dispatch was emitted (not skipped by ao_eye) */
  static bool dispatch(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t tid = twin_of(g_cur_cs); if (!tid || !mode() || !init()) return false;
    last_em = true;
    if (b1ab_ao_off_phase()) {   /* s1b: on/off A/B alternation, AO-off phase (rhi_b1_shadoweo.inc): the game's own blur (not the AO switch's replacement) */
      if (gp::on()) gp::on_dispatch(x, y, false);
      emit([x, y, z] (DxvkContext* c) { c->dispatch(x, y, z); }); n_orig_disp++; return true; }
    Rc<DxvkShader> tw = b1dx_shader_get(tid), game = b1dx_shader_get(g_cur_cs);
    if (tw == nullptr || game == nullptr) return false;
    { uint32_t sc[3]; if (b1dx_shader_alt(g_cur_cs, sc) != nullptr) n_alt_over++; }   /* the AO switch's blur replacement (if any) is not used */
    if (mode() == 1) {
      if (gp::on()) gp::on_compute_label(x == 1 && y == 1 ? "compute (AO blur V4)" : (std::string("compute ") + std::to_string(x) + "x" + std::to_string(y) + " (AO blur V4)").c_str());
      bind_w();
      last_em = !(b1vr::eyeb && b1vr::eye_emit(x, y, 0, "AO blur V4"));   /* vr71: ao_eye (left eye view's AO passes are dead work) */
      if (last_em)
        emit([tw = std::move(tw), game = std::move(game), x, y, z] (DxvkContext* c) mutable {
          c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(tw)); c->dispatch(x, y, z); c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); });
      unbind_w();
      n_twin_disp++; return true;
    }
    /* mode 4: game's shader into the real UAV, twin into a copy of it, then count differing texels */
    if (gp::on()) gp::on_dispatch(x, y, false);
    if (uav0 == nullptr || uav0->info().mipIndex || uav0->info().layerIndex || uav0->info().viewType != VK_IMAGE_VIEW_TYPE_2D) {
      emit([x, y, z] (DxvkContext* c) { c->dispatch(x, y, z); }); n_ab_skip++; n_orig_disp++; return true; }
    Dual *d = dual_for(uav0);
    VkExtent3D e = d->src->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    struct P { int32_t v[4]; } p = { { (int32_t)e.width - 1, (int32_t)e.height - 1, 0, 0 } };
    bind_w();
    emit([di = d->img, si = d->src, sl, e, x, y, z, tw = std::move(tw), game = std::move(game), du = d->uav, gu = uav0, cs = cscmp, pb = pbuf, p,
          a = d->srcsrv, b = d->srv, abv = abview, gx = (e.width + 7) / 8, gy = (e.height + 7) / 8] (DxvkContext* c) mutable {
      const uint32_t cbs = 5u * 16u + 13u;
      c->copyImage(di, sl, VkOffset3D{ 0, 0, 0 }, si, sl, VkOffset3D{ 0, 0, 0 }, e);   /* the UAV before the game's dispatch */
      c->dispatch(x, y, z);                                                               /* game's shader -> real UAV */
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(tw));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 0u, Rc<DxvkImageView>(du));
      c->dispatch(x, y, z);                                                               /* twin -> copy */
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 0u, Rc<DxvkImageView>(gu));
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
      c->updateBuffer(pb, 0, sizeof p, &p);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 0, 64));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, std::move(a));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, std::move(b));
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, std::move(abv));
      c->dispatch(gx, gy, 1);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, nullptr);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, nullptr);
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, nullptr);
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); });
    unbind_w(); g_bc.srv[5][127] = BC_UNK; g_bc.srv[5][125] = BC_UNK;
    n_ab++; n_orig_disp++; ab_dispatches++;
    stats(d, x, y);
    return true; }

  static void frame() {
    if (!n_twins) return;
    static long every = -1; if (every < 0) every = env_i("BSI_B1_AOBLUR_REPORT", 600) > 0 ? env_i("BSI_B1_AOBLUR_REPORT", 600) : 600;
    if (++n_frames % every) return;
    char st[200] = "";
    if (abbuf != nullptr) { const uint32_t *pp = (const uint32_t *)abbuf->mapPtr(0); ab_total = pp[1];
      std::snprintf(st, sizeof st, " | A/B: %ld blur dispatches compared (%ld skipped), texels differing so far %u", n_ab, n_ab_skip, pp[1]); }
    std::fprintf(stderr, "[b1ab] mode %d, frame %ld, last %ld frames: blur dispatches with the V4 twin %ld, with the game's shader %ld (AO-switch blur replacements not used %ld)%s\n",
      mode(), n_frames, every, n_twin_disp, n_orig_disp, n_alt_over, st);
    n_twin_disp = n_orig_disp = n_alt_over = 0; n_ab = n_ab_skip = 0; }
}

/* rhi_b1_shader.cpp, after a compute shader was created (no lock held) */
void b1ab_cs_created(uint32_t id, const void *code, uint32_t len) {
  if (!ab::mode() || ab::creating || !id || !code) return;
  b1ab::Result r;
  try { r = b1ab::rewrite(code, len, ab::share(), ab::brk()); } catch (...) { return; }
  if (!r.candidate) return;
  unsigned long long key = (unsigned long long)b1eo::dxbc_key(code, len);
  if (!r.ok) { std::fprintf(stderr, "[b1ab] cs %016llx has the HDAO blur structure but gets NO twin: %s\n", key, r.why.c_str()); return; }
  ab::creating = true; uint32_t tid = b1dx_shader_create(5, r.blob.data(), (uint32_t)r.blob.size(), 1); ab::creating = false;
  if (!tid) { std::fprintf(stderr, "[b1ab] cs %016llx: twin failed to compile, the game's blur runs\n", key); return; }
  { std::lock_guard<std::mutex> lk(ab::mu); ab::twin[id] = tid; }
  ab::n_twins++;
  const char *nm = "?"; for (auto &t : b1ab::kTargets) if (t.key == key) nm = t.name;
  std::fprintf(stderr, "[b1ab] cs %016llx %s: V4 twin %u for shader %u (instructions %u -> %u, groupshared %u -> %u bytes, %u taps, %u weights, %zu -> %zu bytes)\n",
    key, nm, tid, id, r.n_in, r.n_out, r.lds_in, r.lds_out, r.ntaps, r.nweights, (size_t)len, r.blob.size());
}
/* b1dx_shader_release (g_rmu held) */
static void b1ab_cs_released(uint32_t id) {
  uint32_t tid = 0; { std::lock_guard<std::mutex> lk(ab::mu); auto it = ab::twin.find(id); if (it != ab::twin.end()) { tid = it->second; ab::twin.erase(it); } }
  if (tid) b1dx_shader_release_raw(tid);
}
static bool b1ab_hook_dispatch(uint32_t x, uint32_t y, uint32_t z) { return ab::n_twins && ab::dispatch(x, y, z); }
static bool b1ab_last_emitted() { return ab::last_em; }   /* ao74 */
static void b1ab_hook_uav(uint32_t slot, View *v) { ab::uav_set(slot, v); }
static void b1ab_hook_frame() { ab::frame(); }
static bool b1ab_is_blur(uint32_t cs) { return ab::n_twins && ab::twin_of(cs) != 0; }   /* vr70: AO crop (rhi_b1_vr.inc) cuts only the two HDAO blurs */

/* vr71: vr_ui_rhi.c's scene command hook (FDrawSceneCommand::Execute, rendering thread, right before FSceneRenderer::Render):
   the scene's views and their x in the back buffer, in render order; ao_eye attributes the AO passes to views by it */
extern "C" void b1dx_vr_scene_begin(uint32_t n, const int32_t *x) {
  std::lock_guard<RmuLock> lk(g_rmu);
  b1vr::sc_n = n > 4 ? 0 : (int)n; b1vr::sc_next = 0; b1vr::sc_cur = -1;
  for (int i = 0; i < b1vr::sc_n; i++) b1vr::sc_x[i] = x[i];
}
