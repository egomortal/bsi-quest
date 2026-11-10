/* rhi_b1_shadoweo.inc -- S1 shadow early-out, runtime side. #included at the end of rhi_b1_dxvk.cpp (it uses that file's
   object tables, emit() and g_rmu); the DXBC work is in rhi_b1_shadoweo.h. README: /home/claude/bsi/s1_fix/README.md.

   BSI_B1_SHADOWEO = 0 off | 1 early-out (default) | 2 early-out + per-pixel counters | 3 check: never skips, counts the
   pixels whose early-out decision would disagree with the shader's own blocker count (must stay 0) | 4 A/B: every
   projection draw runs with the game's shader into its target and again with the mode-1 twin into a copy of the target
   taken just before it; a compute pass counts the texels where the two results differ (must stay 0).
   Flow: the two allowlisted projection shaders get a patched twin at creation (b1eo_ps_created). At a draw with such a
   shader bound, the shadow depth texture bound at PS t1 gets a block min/max map (built by three dispatches when the
   atlas changed since the last build: draws with it as depth target (viewport rect), clears, copies, uploads), the twin
   is bound instead of the game's shader, the map at PS t127 and the atlas "no NaN" flag at PS t126. When a condition
   does not hold (t1 not mip 0 of a 2D D32 texture, s0 not wrapping, ...), the game's own shader runs for that draw.
   Diagnostics: BSI_B1_SHADOWEO_SELFTEST=N (every N-th map build: GPU map vs CPU recomputation), BSI_B1_SHADOWEO_BLOCK
   (log2 of the L1 block, default 2 = 4x4 texels), BSI_B1_SHADOWEO_FORCE=1 (rebuild the whole map before every draw).
   Job s1b (README /home/claude/bsi/s1b/README.md): third shader (ps 59c8d61b, point light) allowlisted; map kernels v2
   (BSI_B1_SHADOWEO_MAPV=2 default, 1 = build 67's); build accounting (builds no early-out draw used); on/off A/B
   alternation with per-state GPU time (BSI_B1_SHADOWEO_AB=N or /sdcard/BSI/s1ab.txt; AO blur twin: BSI_B1_AOBLUR_AB=N or
   /sdcard/BSI/aoab.txt); BSI_B1_SHADOWEO_DRAWPROF=1 times every shadow-projection draw by shader (diagnostic, splits passes).
   Job s1c71 (README /home/claude/bsi/s1c71/README.md): S1c, the PCF's 64 gather4_c taps taken from the blocker search's own
   compare bits (Fable note 11, rhi_b1_shadoweo_pcf.h), bit-exact. BSI_B1_SHADOWEO_PCF = 1 on (default) | 0 off | 2 check
   (the taps stay and are compared with the bits: "[b1eo] ... S1c CHECK ... mismatching taps 0" must hold);
   BSI_B1_SHADOWEO_PCFAB=N or /sdcard/BSI/s1cab.txt: S1c on/off alternation (gp tag bit 2) with "S1c saves X ms/frame";
   BSI_B1_SHADOWEO_PCFBREAK=1|2: deliberately wrong S1c twins (negative control for PCF=2 and for mode 4). In mode 4 with
   PCF=1 the per-draw A/B compares the game's shader with the S1c twin (with the S1 twin where S1c is not applicable). */
#include "rhi_b1_shadoweo.h"
#include "rhi_b1_shadoweo_map.h"
#include "rhi_b1_shadoweo_pcf.h"   /* S1c: PCF from the search bits (Fable note 11) */
#include <unordered_map>
extern "C" uint32_t b1dx_shader_create(int stage, const void *code, uint32_t len, int compile_now);   /* rhi_b1_shader.cpp */

namespace eo {
  static int mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO"); v = e ? std::atoi(e) : 1; if (v < 0 || v > 4) v = 0; } return v; }
  static std::mutex mu;                                    /* variant table: written on the creating thread, read under g_rmu */
  static std::unordered_map<uint32_t, uint32_t> variant;   /* game PS id -> patched PS id */
  /* S1c (BSI_B1_SHADOWEO_PCF = 1 on, default since build 71 | 0 off | 2 check): second-level twins = S1 twin + PCF taps from
     the search bits, one for a LESS_OR_EQUAL comparison sampler and one for LESS; chosen per draw from the comparison
     sampler the taps use (s2 in the game's shaders; s1c71: Fable's version read s1) in pre_draw_inner */
  static int pcfmode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_PCF"); v = e && *e ? std::atoi(e) : 1; if (v < 0 || v > 2) v = 0; } return v; }   /* s1c71: default 1 */
  static int pcfbreak() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_PCFBREAK"); v = e ? std::atoi(e) : 0; if (v < 0 || v > 2) v = 0; } return v; }   /* s1c71: negative control */
  static std::unordered_map<uint32_t, uint32_t> variant_le, variant_lt, variant_smp;   /* game PS id -> S1c twin ids, the taps' sampler slot (eo::mu) */
  static long n_pcf_draws, n_pcf_fallback, n_pcf_off; static int pcf_why_logged;
  static uint64_t pcf_tot[6];   /* s1c71: cumulative S1c check counters: safe px, unsafe px, mismatching taps, mismatching px, draws, fallbacks */
  static bool pcf_on();         /* s1c71: pcfmode() and not in the A/B "S1c off" phase (abx below) */
  static thread_local bool creating;
  /* render state (g_rmu) */
  constexpr uint32_t kMapMarker = 0xfffffffeu;                 /* g_bc.srv[1][127] while our map is bound there */
  static b1eo::Geo geo() { static int sh = -1; if (sh < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_BLOCK"); sh = e ? std::atoi(e) : 2; if (sh < 1 || sh > 4) sh = 2; } return b1eo::geo_for((uint32_t)sh); }   /* log2 L1 block */
  constexpr uint32_t kGlobMarker = 0xfffffffdu;                /* g_bc.srv[1][126] while the atlas flag is bound there */
  static const void *bound_glob;
  static uint32_t cur_ps; static Rc<DxvkShader> cur_eo, cur_orig; static bool bound_eo; static const void *bound_map;
  static Rc<DxvkShader> cur_pcf_le, cur_pcf_lt; static const void *bound_ptr; static uint32_t cur_pcf_smp;   /* S1c twins of cur_ps, their sampler slot; which twin is bound */
  struct Atlas {
    uint32_t res = 0; Rc<DxvkImage> img; uint32_t w = 0, h = 0, bw = 0, bh = 0;
    Rc<DxvkImage> l1, l2; Rc<DxvkImageView> src, l1u, l1s, l2u, l2s;
    Rc<DxvkBuffer> glob; Rc<DxvkBufferView> globu, globs;      /* [0] = 1: no NaN (no invalid block) anywhere in the atlas */
    int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;              /* dirty texel rect (inclusive); empty when x1 < x0 */
    /* frame-coherent eager builds: episode = the n-th time per frame the atlas stops being the depth target while dirty.
       An episode is built eagerly (between passes) unless last frame had an episode n that no early-out draw consumed
       (e.g. the far cascade, whose projection uses another shader). Exactness never depends on this: a dirty map is
       always rebuilt at the projection draw itself. */
    uint32_t seq = 0; uint64_t used_prev = ~0ull, used_cur = 0, seen_prev = 0, seen_cur = 0;
    bool built_unused = false;   /* s1b: the last build has not been used by an early-out draw yet */
  };
  static std::vector<Atlas> atlases;
  static b1eomap::Kernels K; static bool cs_failed; static Rc<DxvkBuffer> params, stats; static Rc<DxvkBufferView> statsv;
  static int mapv() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_MAPV"); v = e ? std::atoi(e) : 2; if (v != 1) v = 2; } return v; }
  static long n_eo_draws, n_orig_draws, n_red, n_red_eager, n_red_skipped, n_frames, n_red_total; static std::atomic<long> n_patched; static double red_mtex; static int why_logged;
  static long n_red_unused, n_off_draws;   /* s1b: builds replaced before any early-out draw used them; draws in the A/B "S1 off" phase */
  static bool ab_off();                    /* s1b: A/B alternation, S1 off phase (abx below) */
  static uint64_t last_stats[11]; static uint64_t last_pcf[4];
  /* mode 4 (A/B) */
  struct Dual { Rc<DxvkImage> rt, img; Rc<DxvkImageView> rtv, srv, rtsrv; };
  static Rc<DxvkShader> dual_twin;   /* s1c71: the twin the mode-4 second draw uses (S1c where applicable, else the S1 twin) */
  static std::vector<Dual> duals; static Rc<DxvkShader> cscmp; static Rc<DxvkBuffer> abbuf; static Rc<DxvkBufferView> abview;
  static Dual *dual_pending; static Atlas *dual_atlas; static long n_dual, n_dual_skip; static uint32_t last_ab;

  static void mark(Atlas &a, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (int32_t)a.w - 1) x1 = a.w - 1; if (y1 > (int32_t)a.h - 1) y1 = a.h - 1;
    if (x1 < x0 || y1 < y0) return;
    if (a.x1 < a.x0) { a.x0 = x0; a.y0 = y0; a.x1 = x1; a.y1 = y1; return; }
    a.x0 = std::min(a.x0, x0); a.y0 = std::min(a.y0, y0); a.x1 = std::max(a.x1, x1); a.y1 = std::max(a.y1, y1); }
  static void mark_all(Atlas &a) { a.x0 = 0; a.y0 = 0; a.x1 = a.w - 1; a.y1 = a.h - 1; }
  static Atlas *find(uint32_t res) {
    Obj *o = robj(res); if (!o || o->kind != 2) return nullptr;
    for (auto &a : atlases) if (a.res == res) { if (a.img == o->img) return &a; a = Atlas(); a.res = 0; bound_map = bound_glob = nullptr; return nullptr; }   /* id reused: forget */
    return nullptr; }
  static int trace() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_TRACE"); v = e ? std::atoi(e) : 0; } return v; }   /* frames to trace */
  static bool tr_on() { return trace() > 0 && n_frames >= 1000 && n_frames < 1000 + trace(); }
  static void log_once(const char *what) { if (why_logged++ < 12) std::fprintf(stderr, "[b1eo] not used for a draw: %s\n", what); }

  static Rc<DxvkImage> map_image(uint32_t w, uint32_t h, const char *name) {
    DxvkImageCreateInfo ii = { };
    ii.type = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R32G32_UINT; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    ii.extent = { w, h, 1u }; ii.numLayers = 1; ii.mipLevels = 1;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ii.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    ii.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = VK_IMAGE_LAYOUT_GENERAL; ii.debugName = name;
    return g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); }
  static Rc<DxvkImageView> view(const Rc<DxvkImage> &img, VkFormat f, VkImageAspectFlags asp, bool storage) {
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = f; k.aspects = asp; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    k.usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT; if (storage) k.layout = VK_IMAGE_LAYOUT_GENERAL;
    return img->createView(k); }
  static bool init_shared() {
    if (cs_failed) return false;
    if (K.cs1 != nullptr) return true;
    if (!b1eomap::make(K, mapv(), geo(), g_dev, [] (const std::vector<uint8_t> &c) { uint32_t id = b1dx_shader_create(5, c.data(), (uint32_t)c.size(), 1); return id ? b1dx_shader_get(id) : Rc<DxvkShader>(); })) {
      K = b1eomap::Kernels(); cs_failed = true; std::fprintf(stderr, "[b1eo] block-map compute shaders failed to build: early-out off\n"); return false; }
    DxvkBufferCreateInfo bi = { }; bi.size = 768; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 S1 params";
    params = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mode() == 4) {
      auto cc = b1eo::build_cs_compare(); uint32_t ic = b1dx_shader_create(5, cc.data(), (uint32_t)cc.size(), 1);
      if (!ic) { cs_failed = true; std::fprintf(stderr, "[b1eo] compare shader failed to build\n"); return false; }
      cscmp = b1dx_shader_get(ic);
      DxvkBufferCreateInfo ai = { }; ai.size = 16; ai.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
      ai.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; ai.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; ai.debugName = "B1 S1 A/B";
      abbuf = g_dev->createBuffer(ai, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      std::memset(abbuf->mapPtr(0), 0, 16);
      DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 16;
      abview = abbuf->createView(vk);
    }
    if (mode() == 2 || mode() == 3 || pcfmode() == 2) {
      DxvkBufferCreateInfo si = { }; si.size = 256; si.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
      si.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; si.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; si.debugName = "B1 S1 stats";
      stats = g_dev->createBuffer(si, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      std::memset(stats->mapPtr(0), 0, 256);
      DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 256;
      statsv = stats->createView(vk);
    }
    std::fprintf(stderr, "[b1eo] block-map compute shaders ready (L1 %ux%u texel blocks, L2 = %ux%u L1 blocks, kernels v%d%s)\n", geo().block(), geo().block(), geo().span, geo().span, mapv(),
      mapv() == 2 ? ": gather4 L1, groupshared L2, wide flag pass" : "");
    return true; }
  static Atlas *track(uint32_t res) {
    if (Atlas *a = find(res)) return a;
    Obj *o = robj(res); if (!o || o->kind != 2) return nullptr;
    VkExtent3D e = o->img->mipLevelExtent(0); if (!e.width || !e.height) return nullptr;
    Atlas a; a.res = res; a.img = o->img; a.w = e.width; a.h = e.height; a.bw = (e.width + geo().block() - 1) >> geo().shift; a.bh = (e.height + geo().block() - 1) >> geo().shift;
    a.l1 = map_image(a.bw, a.bh, "B1 S1 L1"); a.l2 = map_image(a.bw, a.bh, "B1 S1 L2");
    a.src = view(o->img, o->fmt, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    a.l1u = view(a.l1, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, true); a.l1s = view(a.l1, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, false);
    a.l2u = view(a.l2, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, true); a.l2s = view(a.l2, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, false);
    { DxvkBufferCreateInfo gi = { }; gi.size = 16; gi.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      gi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      gi.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT; gi.debugName = "B1 S1 atlas flag";
      a.glob = g_dev->createBuffer(gi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.offset = 0; vk.size = 16;
      vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; a.globu = a.glob->createView(vk);
      vk.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT; a.globs = a.glob->createView(vk); }
    emit([l1 = a.l1, l2 = a.l2, g = a.glob] (DxvkContext* c) { c->initImage(l1, VK_IMAGE_LAYOUT_UNDEFINED); c->initImage(l2, VK_IMAGE_LAYOUT_UNDEFINED); c->clearBuffer(g, 0, 16, 0u); });
    mark_all(a);
    for (auto &s : atlases) if (!s.res) { s = std::move(a); goto done; }
    atlases.push_back(std::move(a));
  done:
    std::fprintf(stderr, "[b1eo] tracking shadow depth texture %u (%ux%u): block map %ux%u\n", res, e.width, e.height, (e.width + geo().block() - 1) >> geo().shift, (e.height + geo().block() - 1) >> geo().shift);
    return find(res); }
  static void selftest(Atlas &a);
  /* rebuild the map over the dirty rect: L1 blocks touching it, then the L2 texels whose span x span cell touches those
     blocks, then the whole-atlas "no NaN" flag (CS3, reads the L2 at stride span) */
  static void reduce(Atlas &a, bool eager) {
    if (a.x1 < a.x0) return;
    b1eomap::Maps m; m.src = a.src; m.l1u = a.l1u; m.l1s = a.l1s; m.l2u = a.l2u; m.l2s = a.l2s; m.glob = a.glob; m.globu = a.globu; m.w = a.w; m.h = a.h; m.bw = a.bw; m.bh = a.bh;
    b1eomap::Plan p = b1eomap::plan(K, m, a.x0, a.y0, a.x1, a.y1);
    if (gp::on()) gp::on_compute_label("S1 shadow block map (compute)");
    Rc<DxvkShader> game = b1dx_shader_get(g_cur_cs);
    /* v2 binds a sampler at CS s15; afterwards the game's sampler there (if it ever bound one) is restored */
    Rc<DxvkSampler> keep; { uint32_t sid = g_bc.ss[5][15]; if (sid && sid != BC_UNK) if (SS *ss = g_sss.get(sid)) keep = ss->s;
      static bool told; if (!told && sid && sid != BC_UNK) { told = true; std::fprintf(stderr, "[b1eo] note: the game uses CS sampler s15; it is restored after each map build\n"); } }
    emit([k = K, m, p, pb = params, game, keep] (DxvkContext* c) mutable {
      b1eomap::record(c, k, m, p, pb, kUavCompute, keep);
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); });
    if (a.built_unused) n_red_unused++;
    a.built_unused = true;
    n_red++; n_red_total++; if (eager) n_red_eager++; red_mtex += (double)(a.x1 - a.x0 + 1) * (a.y1 - a.y0 + 1) / 1e6;
    a.x0 = 0; a.y0 = 0; a.x1 = -1; a.y1 = -1;
    selftest(a); }

  /* BSI_B1_SHADOWEO_SELFTEST=N: after the N-th map build, read back the atlas and both map levels and recompute the map
     on the CPU (diagnostics; stalls the GPU once) */
  static Rc<DxvkBuffer> readback(const Rc<DxvkImage> &img, VkFormat fmt, VkImageAspectFlags asp, uint32_t esz) {
    VkExtent3D e = img->mipLevelExtent(0);
    DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)e.width * e.height * esz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 S1 selftest";
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageSubresourceLayers sl = { asp, 0u, 0u, 1u };
    emit([rb, fmt, img, sl, e] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, e); });
    return rb; }
  static void selftest(Atlas &a) {
    static long n = -2; if (n == -2) { const char *e = std::getenv("BSI_B1_SHADOWEO_SELFTEST"); n = e ? std::atol(e) : -1; }
    if (n <= 0 || n_red_total % n) return;
    Rc<DxvkBuffer> rd = readback(a.img, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 4), r1 = readback(a.l1, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 8),
                   r2 = readback(a.l2, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 8);
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint32_t *d = (const uint32_t *)rd->mapPtr(0), *g1 = (const uint32_t *)r1->mapPtr(0), *g2 = (const uint32_t *)r2->mapPtr(0);
    std::vector<uint32_t> c1(2 * a.bw * a.bh), c2(2 * a.bw * a.bh); long bad1 = 0, bad2 = 0, inval = 0; double sum = 0;
    for (uint32_t by = 0; by < a.bh; by++) for (uint32_t bx = 0; bx < a.bw; bx++) { uint32_t mn = 0xffffffffu, mx = 0;
      const uint32_t B = geo().block(), S = geo().span;
      for (uint32_t y = by * B; y < by * B + B; y++) for (uint32_t x = bx * B; x < bx * B + B; x++) { uint32_t v = d[std::min(y, a.h - 1) * a.w + std::min(x, a.w - 1)]; mn = std::min(mn, v); mx = std::max(mx, v); }
      if (mx > 0x7f800000u) { mn = mx = 0xffffffffu; inval++; }
      c1[2 * (by * a.bw + bx)] = mn; c1[2 * (by * a.bw + bx) + 1] = mx; }
    for (uint32_t by = 0; by < a.bh; by++) for (uint32_t bx = 0; bx < a.bw; bx++) { uint32_t mn = 0xffffffffu, mx = 0;
      const uint32_t S = geo().span;
      for (uint32_t y = by; y < by + S; y++) for (uint32_t x = bx; x < bx + S; x++) { size_t i = 2 * (std::min(y, a.bh - 1) * a.bw + std::min(x, a.bw - 1)); mn = std::min(mn, c1[i]); mx = std::max(mx, c1[i + 1]); }
      if (mx > 0x7f800000u) mn = mx = 0xffffffffu;
      c2[2 * (by * a.bw + bx)] = mn; c2[2 * (by * a.bw + bx) + 1] = mx; }
    for (size_t i = 0; i < c1.size(); i++) { if (!(i & 1)) { c1[i] = ~c1[i]; c2[i] = ~c2[i]; } if (c1[i] != g1[i]) { if (bad1 < 4) std::fprintf(stderr, "[b1eo] selftest L1[%zu] gpu %08x cpu %08x\n", i, g1[i], c1[i]); bad1++; }
                                             if (c2[i] != g2[i]) { if (bad2 < 4) std::fprintf(stderr, "[b1eo] selftest L2[%zu] gpu %08x cpu %08x\n", i, g2[i], c2[i]); bad2++; } }
    for (size_t i = 0; i < (size_t)a.w * a.h; i++) { float f; std::memcpy(&f, &d[i], 4); sum += f; }
    uint32_t gflag = 0; { DxvkBufferCreateInfo bi = { }; bi.size = 16; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 S1 selftest flag";
      Rc<DxvkBuffer> fb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      emit([fb, g = a.glob] (DxvkContext* c) { c->copyBuffer(fb, 0, g, 0, 16); }); ctx_flush(); cs_sync(); g_dev->waitForIdle(); std::memcpy(&gflag, fb->mapPtr(0), 4); }
    std::fprintf(stderr, "[b1eo] SELFTEST after map build %ld (kernels v%d): atlas %ux%u (mean depth %.4f), L1/L2 %ux%u, invalid blocks %ld, GPU vs CPU mismatches: L1 %ld, L2 %ld, atlas flag gpu %u cpu %u%s\n",
      n_red_total, mapv(), a.w, a.h, sum / ((double)a.w * a.h), a.bw, a.bh, inval, bad1, bad2, gflag, inval ? 0u : 1u, (bad1 || bad2 || gflag != (inval ? 0u : 1u)) ? " MISMATCH" : ""); }

  static void bind_maps(Atlas &a); static void ab_prepare(Atlas &a);
  /* s1b: diagnostic per-shader timing of the projection draws (BSI_B1_SHADOWEO_DRAWPROF=1): each draw with a shadow
     projection shader gets its own gp segment ("proj <key> early-out" / "... game shader"). Every gp mark ends the
     render pass on a tiler, so this changes what it measures: diagnostics only. */
  static int drawprof() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_DRAWPROF"); v = e && std::atoi(e) > 0; } return v; }
  static std::unordered_map<uint32_t, std::string> diag_name;   /* PS id -> label (eo::mu) */
  static bool diag_open;
  static std::vector<int> projlab;                               /* gp labels of the passes with projection draws (A/B report) */
  static void pre_draw_inner();
  static void pre_draw() {
    if (cur_eo != nullptr && gp::on()) { int l = gp::plab; if (l >= 0 && std::find(projlab.begin(), projlab.end(), l) == projlab.end()) projlab.push_back(l); }
    pre_draw_inner();
    if (drawprof() && gp::on()) { std::string nm; { std::lock_guard<std::mutex> lk(mu); auto it = diag_name.find(cur_ps); if (it != diag_name.end()) nm = it->second; }
      if (!nm.empty()) { nm += bound_eo ? " early-out (diag)" : " game shader (diag)"; gp::on_draw(); gp::ndraw--; gp::mark(gp::lab_id(nm)); diag_open = true; } } }
  /* a draw (pre_draw): the depth target's dirty rect, then the shader choice for the projection draws */
  static void pre_draw_inner() {
    if (!atlases.empty() && g_cs.dsv) { View *dv = g_views.get(g_cs.dsv);
      if (dv && dv->type == 3) if (Atlas *a = find(dv->res)) {
        if (!g_cs.nvp) mark_all(*a);
        else for (uint32_t i = 0; i < g_cs.nvp; i++) { const float *v = g_cs.vp[i];   /* x, y, w, h, minz, maxz; +-1 texel for edge rules */
          if (!(v[2] > 0.f && v[3] > 0.f && v[0] > -1e8f && v[1] > -1e8f && v[2] < 1e8f && v[3] < 1e8f)) { mark_all(*a); break; }
          mark(*a, (int32_t)std::floor(v[0]) - 1, (int32_t)std::floor(v[1]) - 1, (int32_t)std::ceil(v[0] + v[2]) + 1, (int32_t)std::ceil(v[1] + v[3]) + 1); } } }
    if (cur_eo == nullptr) return;
    if (ab_off()) { n_off_draws++;   /* s1b A/B alternation, S1 off phase: the game's shader, no map work (dirty tracking above continues) */
      if (bound_eo) { emit([s = cur_orig] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); }); bound_eo = false; }
      return; }
    const char *why = nullptr; Atlas *a = nullptr;
    View *v = g_views.get(g_bc.srv[1][1] == BC_UNK ? 0 : g_bc.srv[1][1]);
    if (!v || v->type != 1) why = "no shadow depth view at t1";
    else { const auto &vi = v->iv->info(); Obj *o = robj(v->res);
      if (!o || o->kind != 2 || o->fmt != VK_FORMAT_D32_SFLOAT || vi.mipIndex != 0 || vi.layerIndex != 0 || vi.viewType != VK_IMAGE_VIEW_TYPE_2D || o->img->info().type != VK_IMAGE_TYPE_2D)
        why = "t1 is not mip 0 of a 2D D32 image";
      else if (!init_shared()) why = "compute shaders unavailable";
      else if (!g_sss.get(g_bc.ss[1][0] == BC_UNK ? 0 : g_bc.ss[1][0])) why = "no sampler at s0";
      else if (SS *ss = g_sss.get(g_bc.ss[1][0])) { const auto &k = ss->s->key();   /* s0: a NaN border colour could be gathered */
        if (k.u.p.addressU != VK_SAMPLER_ADDRESS_MODE_REPEAT || k.u.p.addressV != VK_SAMPLER_ADDRESS_MODE_REPEAT) why = "s0 does not wrap (the twin assumes REPEAT)";
        else if (k.u.p.hasBorder && std::isnan(k.borderColor.float32[0])) why = "s0 has a NaN border colour";
      }
      if (!why && !(a = track(v->res))) why = "cannot track t1"; }
    if (why) { log_once(why); n_orig_draws++;
      if (bound_eo) { emit([s = cur_orig] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); }); bound_eo = false; }
      return; }
    { static int force = -1; if (force < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_FORCE"); force = e && std::atoi(e) > 0; } if (force) mark_all(*a); }   /* diagnostics */
    if (tr_on()) std::fprintf(stderr, "[b1eo-tr] f%ld EO draw seq %u%s dirty %d,%d-%d,%d\n", n_frames, a->seq, a->x1 >= a->x0 ? " LAZY BUILD" : "", a->x0, a->y0, a->x1, a->y1);
    if (a->x1 >= a->x0) reduce(*a, false);
    a->used_cur |= 1ull << (a->seq ? (a->seq - 1 < 63 ? a->seq - 1 : 63) : 0);
    a->built_unused = false;
    /* S1c: the PCF-from-bits twin when the comparison sampler the taps use allows it (else the S1 twin) */
    Rc<DxvkShader> use = cur_eo;
    if (pcf_on() && (cur_pcf_le != nullptr || cur_pcf_lt != nullptr)) {
      const char *pw = nullptr; Rc<DxvkShader> pick;
      uint32_t sid = cur_pcf_smp < 16 ? g_bc.ss[1][cur_pcf_smp] : 0;
      SS *s0 = g_sss.get(g_bc.ss[1][0] == BC_UNK ? 0 : g_bc.ss[1][0]); SS *sc = g_sss.get(sid == BC_UNK ? 0 : sid);
      if (!sc || !s0) pw = "no comparison sampler bound at the taps' slot";
      else { const auto &k0 = s0->s->key(), &k1 = sc->s->key();
        if (!k1.u.p.compareEnable) pw = "the taps' sampler is not a comparison sampler";
        else if (k1.u.p.compareOp != VK_COMPARE_OP_LESS_OR_EQUAL && k1.u.p.compareOp != VK_COMPARE_OP_LESS) pw = "the taps' compare op is not LESS_OR_EQUAL / LESS";
        else if (k1.u.p.addressU != k0.u.p.addressU || k1.u.p.addressV != k0.u.p.addressV) pw = "the taps' sampler address modes differ from s0";
        else if (!a->w || !a->h || (a->w & (a->w - 1)) || (a->h & (a->h - 1))) pw = "atlas size is not a power of two";
        else { pick = k1.u.p.compareOp == VK_COMPARE_OP_LESS_OR_EQUAL ? cur_pcf_le : cur_pcf_lt; if (pick == nullptr) pw = "no S1c twin for this compare op"; } }
      if (pick != nullptr) { use = pick; n_pcf_draws++; }
      else { n_pcf_fallback++; if (pw && pcf_why_logged++ < 8) std::fprintf(stderr, "[b1eo] S1c twin not used for a draw: %s\n", pw); }
    } else if (pcfmode() && cur_pcf_le != nullptr) n_pcf_off++;   /* A/B "S1c off" phase */
    if (mode() == 4) { dual_twin = use; ab_prepare(*a); return; }   /* s1c71: mode 4 compares the twin chosen above */
    if (!bound_eo || bound_ptr != use.ptr()) { emit([s = use] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); }); bound_eo = true; bound_ptr = use.ptr(); }
    bind_maps(*a);
    if (statsv != nullptr) emit([sv = statsv] (DxvkContext* c) mutable { c->bindResourceBufferView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 7u, std::move(sv)); });
    n_eo_draws++; }
  /* PS t127 / t126: the game may bind or clear these slots too (UE3 clears all 128 SRV slots), so the bind-cache entries
     hold a marker while our views are bound; any game bind to the slot replaces it and the next draw binds ours again */
  static void bind_maps(Atlas &a) {
    if (bound_map != a.l2s.ptr() || g_bc.srv[1][127] != kMapMarker) {
      emit([m = a.l2s] (DxvkContext* c) mutable { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 127u, std::move(m)); }); bound_map = a.l2s.ptr(); g_bc.srv[1][127] = kMapMarker; }
    if (bound_glob != a.globs.ptr() || g_bc.srv[1][126] != kGlobMarker) {
      emit([m = a.globs] (DxvkContext* c) mutable { c->bindResourceBufferView(VK_SHADER_STAGE_FRAGMENT_BIT, 126u, std::move(m)); }); bound_glob = a.globs.ptr(); g_bc.srv[1][126] = kGlobMarker; } }
  static DxvkRenderTargets cur_rts() {
    DxvkRenderTargets rts = { };
    for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) rts.color[i].view = v->iv; }
    View *d = g_views.get(g_cs.dsv); if (d && d->type == 3) rts.depth.view = d->iv;
    return rts; }
  /* mode 4, before the draw: the game's shader draws into the real target; snapshot the target into the twin's copy */
  static void ab_prepare(Atlas &a) {
    if (bound_eo) { emit([s = cur_orig] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); }); bound_eo = false; }
    DxvkRenderTargets rts = cur_rts(); int nc = 0; for (uint32_t i = 0; i < 8; i++) nc += rts.color[i].view != nullptr;
    if (nc != 1 || rts.color[0].view == nullptr || rts.color[0].view->info().mipIndex || rts.color[0].view->info().layerIndex) { n_dual_skip++; return; }
    Rc<DxvkImage> rt = rts.color[0].view->image(); Dual *d = nullptr;
    for (auto &x : duals) if (x.rt == rt) d = &x;
    if (!d) {
      DxvkImageCreateInfo ii = rt->info(); ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      ii.mipLevels = 1; ii.numLayers = 1; ii.flags = 0; ii.viewFormatCount = 0; ii.viewFormats = nullptr; ii.debugName = "B1 S1 A/B copy";
      ii.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      ii.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      ii.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      Dual nd; nd.rt = rt; nd.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = ii.format; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
      k.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; nd.rtv = nd.img->createView(k);
      k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED; nd.srv = nd.img->createView(k);
      k.format = rt->info().format; nd.rtsrv = rt->createView(k);
      duals.push_back(std::move(nd)); d = &duals.back();
      std::fprintf(stderr, "[b1eo] A/B: copy target for %ux%u fmt %d\n", ii.extent.width, ii.extent.height, (int)ii.format); }
    VkExtent3D e = rt->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    emit([di = d->img, si = rt, sl, e] (DxvkContext* c) { c->copyImage(di, sl, VkOffset3D{ 0, 0, 0 }, si, sl, VkOffset3D{ 0, 0, 0 }, e); });
    dual_pending = d; dual_atlas = &a; n_dual++; }
  /* mode 4, after the draw: the same draw with the twin into the copy, then count the texels that differ */
  static void ab_after(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    Dual *d = dual_pending; dual_pending = nullptr; if (!d) return;
    DxvkRenderTargets rts = cur_rts(), alt = rts; alt.color[0].view = d->rtv;
    bind_maps(*dual_atlas);
    emit([alt, s = dual_twin != nullptr ? dual_twin : cur_eo] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(alt), 0u); c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); }); dual_twin = nullptr;
    if (dc) { VkDrawIndirectCommand x = *dc; emit([x] (DxvkContext* c) { c->draw(1, &x); }); }
    else { VkDrawIndexedIndirectCommand x = *dic; emit([x] (DxvkContext* c) { c->drawIndexed(1, &x); }); }
    VkExtent3D e = d->rt->mipLevelExtent(0);
    struct P { int32_t v[4]; } p = { { (int32_t)e.width - 1, (int32_t)e.height - 1, 0, 0 } };
    Rc<DxvkShader> game = b1dx_shader_get(g_cur_cs);
    emit([rts, o = cur_orig, cs = cscmp, game, pb = params, p, a = d->rtsrv, b = d->srv, ab = abview, gx = (e.width + 7) / 8, gy = (e.height + 7) / 8] (DxvkContext* c) mutable {
      const uint32_t cbs = 5u * 16u + 13u;
      c->bindRenderTargets(std::move(rts), 0u); c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(o));
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
      c->updateBuffer(pb, 512 + 128, sizeof p, &p);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 512 + 128, 64));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, std::move(a));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, std::move(b));
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, std::move(ab));
      c->dispatch(gx, gy, 1);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, nullptr);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, nullptr);
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, nullptr);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice());
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); });
  }
  /* render targets change: if the atlas just stops being the depth target, build its map now (between passes, so the
     projection pass is not split by a dispatch) */
  static void rts(uint32_t newdsv) {
    if (atlases.empty() || !g_cs.dsv || newdsv == g_cs.dsv) return;
    View *ov = g_views.get(g_cs.dsv); if (!ov || ov->type != 3) return;
    View *nv = g_views.get(newdsv); if (nv && nv->res == ov->res) return;
    if (ab_off()) return;   /* s1b: A/B "S1 off" phase: no builds (the atlas stays dirty; re-enabling marks it all dirty anyway) */
    if (Atlas *a = find(ov->res)) if (a->x1 >= a->x0 && K.cs1 != nullptr) {
      uint32_t e = a->seq < 63 ? a->seq : 63; a->seq++; a->seen_cur |= 1ull << e;
      bool eager = !(a->seen_prev >> e & 1) || (a->used_prev >> e & 1);
      if (tr_on()) std::fprintf(stderr, "[b1eo-tr] f%ld rts away: episode %u dirty %d,%d-%d,%d -> %s\n", n_frames, e, a->x0, a->y0, a->x1, a->y1, eager ? "build" : "skip");
      if (eager) reduce(*a, true); else n_red_skipped++; } }
  static void written(uint32_t res) { if (!atlases.empty()) if (Atlas *a = find(res)) { mark_all(*a); if (tr_on()) std::fprintf(stderr, "[b1eo-tr] f%ld atlas cleared/written\n", n_frames); } }
  static void released(uint32_t res) { for (auto &a : atlases) if (a.res == res) { a = Atlas(); a.res = 0; bound_map = bound_glob = nullptr; } }
  static void ps_set(uint32_t sh) {
    if (sh == cur_ps) return;
    cur_ps = sh; cur_eo = nullptr; cur_orig = nullptr; cur_pcf_le = nullptr; cur_pcf_lt = nullptr; cur_pcf_smp = ~0u;
    uint32_t pid = 0, ple = 0, plt = 0; { std::lock_guard<std::mutex> lk(mu); auto it = variant.find(sh); if (it != variant.end()) pid = it->second;
      auto il = variant_le.find(sh); if (il != variant_le.end()) ple = il->second; auto ig = variant_lt.find(sh); if (ig != variant_lt.end()) plt = ig->second;
      auto is = variant_smp.find(sh); if (is != variant_smp.end()) cur_pcf_smp = is->second; }
    if (pid) { cur_eo = b1dx_shader_get(pid); cur_orig = b1dx_shader_get(sh); if (ple) cur_pcf_le = b1dx_shader_get(ple); if (plt) cur_pcf_lt = b1dx_shader_get(plt); } }
  static void frame() {
    for (auto &a : atlases) if (a.res) { a.used_prev = a.used_cur; a.used_cur = 0; a.seen_prev = a.seen_cur; a.seen_cur = 0; a.seq = 0; }
    static long every = -1; if (every < 0) { const char *e = std::getenv("BSI_B1_SHADOWEO_REPORT"); every = e && std::atol(e) > 0 ? std::atol(e) : 600; }
    if (++n_frames % every || (!n_patched && !n_eo_draws)) return;
    char st[900] = "";   /* s1c71: 400 -> 900 (S1c part) */
    if (stats != nullptr && (mode() == 2 || mode() == 3)) { const uint32_t *p = (const uint32_t *)stats->mapPtr(0); uint64_t d[8];   /* s1c71: not when only the S1c check made the buffer */
      for (int i = 0; i < 8; i++) { d[i] = (uint32_t)(p[i] - (uint32_t)last_stats[i]); last_stats[i] = p[i]; }
      uint64_t dinf = (uint32_t)(p[21] - (uint32_t)last_stats[8]), dnan = (uint32_t)(p[22] - (uint32_t)last_stats[9]); last_stats[8] = p[21]; last_stats[9] = p[22];
      uint64_t dnanc = (uint32_t)(p[25] - (uint32_t)last_stats[10]); last_stats[10] = p[25];   /* windows with NaN coordinates (incl. the decided recv -inf ones) */
      double t = (double)d[0] + d[1] + d[2] + d[5] + d[6] + d[7]; int o;
      o = std::snprintf(st, sizeof st, " | pixels: lit early-out %.1f%% (sky, recv -inf: %.1f%%), shadowed early-out %.1f%% (recv NaN: %.1f%%), full search %.1f%% (map undecided %.1f%%, window off the map %.1f%% [NaN coordinates %.1f%%], too wide %.1f%%, invalid %.1f%%) of %.2f M px",
        t ? 100.0 * d[0] / t : 0, t ? 100.0 * dinf / t : 0, t ? 100.0 * d[1] / t : 0, t ? 100.0 * dnan / t : 0, t ? 100.0 * (d[2] + d[5] + d[6] + d[7]) / t : 0, t ? 100.0 * d[2] / t : 0, t ? 100.0 * d[5] / t : 0, t ? 100.0 * ((double)dnanc - dinf - dnan) / t : 0, t ? 100.0 * d[6] / t : 0, t ? 100.0 * d[7] / t : 0, t / 1e6);
      if (mode() == 3 && o > 0 && o < (int)sizeof st) std::snprintf(st + o, sizeof st - o, ", CHECK mismatches %llu", (unsigned long long)d[3]);
      static bool shown; if (mode() == 3 && p[4] && !shown) { shown = true; float f[12]; std::memcpy(f, p + 8, sizeof f);
        std::fprintf(stderr, "[b1eo] first mismatch: uv %.7f %.7f recv %.7f (r2.w %g, cmp %.7f) map min %.7f max %.7f (bits %08x %08x) size %g x %g block %d %d count %g\n",
          f[0], f[1], f[2], f[3], f[4], f[5], f[6], p[13], p[14], f[7], f[8], (int)p[17], (int)p[18], f[11]); } }
    if (pcfmode()) { int o = (int)strlen(st);   /* s1c71: S1c draws always; the check counters in mode 2 (per interval and cumulative) */
      pcf_tot[4] += n_pcf_draws; pcf_tot[5] += n_pcf_fallback;
      if (o >= 0 && o < (int)sizeof st) o += std::snprintf(st + o, sizeof st - o, " | S1c: %ld draws with the PCF-from-bits twin, %ld fell back to the S1 twin%s", n_pcf_draws, n_pcf_fallback,
        n_pcf_off ? (", " + std::to_string(n_pcf_off) + " in the A/B S1c-off phase").c_str() : "");
      if (pcfmode() == 2 && stats != nullptr && o >= 0 && o < (int)sizeof st) { const uint32_t *p = (const uint32_t *)stats->mapPtr(0); uint64_t d[4];
        for (int i = 0; i < 4; i++) { d[i] = (uint32_t)(p[28 + i] - (uint32_t)last_pcf[i]); last_pcf[i] = p[28 + i]; pcf_tot[i] += d[i]; }
        std::snprintf(st + o, sizeof st - o, ", CHECK: %llu PCF pixels (%.1f%% safe), taps compared %llu, mismatching taps %llu (pixels %llu); cumulative: taps compared %llu, mismatching %llu",
          (unsigned long long)(d[0] + d[1]), d[0] + d[1] ? 100.0 * d[0] / (double)(d[0] + d[1]) : 0.0, (unsigned long long)(64 * d[0]), (unsigned long long)d[2], (unsigned long long)d[3],
          (unsigned long long)(64 * pcf_tot[0]), (unsigned long long)pcf_tot[2]);
        static bool shown; if (p[32] && !shown) { shown = true; float f[3]; std::memcpy(f, p + 33, sizeof f);
          std::fprintf(stderr, "[b1eo] S1c first mismatching pixel: uv %.9g %.9g (bits %08x %08x) ref %.9g (bits %08x), %u mismatching taps\n", f[0], f[1], p[33], p[34], f[2], p[35], p[36]); } }
      n_pcf_draws = n_pcf_fallback = n_pcf_off = 0; }
    if (abbuf != nullptr) { const uint32_t *p = (const uint32_t *)abbuf->mapPtr(0); int o = (int)strlen(st);
      std::snprintf(st + o, sizeof st - o, " | A/B: %ld draws compared (%ld skipped), texels differing so far %u", n_dual, n_dual_skip, p[1]); n_dual = n_dual_skip = 0; }
    std::fprintf(stderr, "[b1eo] mode %d, frame %ld, last %ld frames: projection draws with early-out %ld, with the game's shader %ld, block-map builds %ld (%ld between passes, %ld skipped: unused last frame, %.1f Mtexels, %ld replaced before any early-out draw used them)%s%s\n",
      mode(), n_frames, every, n_eo_draws, n_orig_draws, n_red, n_red_eager, n_red_skipped, red_mtex, n_red_unused, st,
      n_off_draws ? (std::string(" | A/B S1-off phase: ") + std::to_string(n_off_draws) + " projection draws with the game's shader").c_str() : "");
    n_eo_draws = n_orig_draws = n_red = n_red_eager = n_red_skipped = n_red_unused = n_off_draws = 0; red_mtex = 0; }
  /* ---- s1b: on/off A/B alternation (diagnostic; both states render exactly the same picture) ----
     tag bit 0 = S1 off (projection draws use the game's shaders, no map builds), bit 1 = AO blur twin off. With both on,
     AO flips every second S1 flip, so all four combinations get frames. gp (rhi_b1_dxvk.cpp) sums the GPU time of every
     frame by the tag it was rendered with; the cumulative per-state averages are printed every 600 frames.
     Switches: BSI_B1_SHADOWEO_AB=N / BSI_B1_AOBLUR_AB=N (period in presents), or the files /sdcard/BSI/s1ab.txt and
     /sdcard/BSI/aoab.txt (existence, polled every 600 presents; period 300). S1 alternation needs BSI_B1_SHADOWEO=1. */
  namespace abx {
    static int s1p, aop, pcp, tag; static long cnt, polls, rep_frames;   /* s1c71: pcp = S1c on/off period (tag bit 2) */
    static bool exists(const char *p) { struct stat st; return p && ::stat(p, &st) == 0; }
    static int envn(const char *n) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : -1; }
    static void reset() { for (int t = 0; t < 8; t++) { gp::tag_frames[t] = 0; gp::tag_tot[t] = 0; gp::tag_ms[t].clear(); } rep_frames = 0; }   /* s1c71: 8 states */
    static void poll() {
      const char *f1 = std::getenv("BSI_B1_SHADOWEO_ABFILE"), *f2 = std::getenv("BSI_B1_AOBLUR_ABFILE");
      int s = envn("BSI_B1_SHADOWEO_AB"), a = envn("BSI_B1_AOBLUR_AB");
      if (s < 0) s = exists(f1 ? f1 : "/sdcard/BSI/s1ab.txt") ? 300 : 0;
      if (a < 0) a = exists(f2 ? f2 : "/sdcard/BSI/aoab.txt") ? 300 : 0;
      /* s1c71: S1c on/off (BSI_B1_SHADOWEO_PCFAB=N or /sdcard/BSI/s1cab.txt); needs S1 mode 1 and S1c on (PCF=1) */
      const char *f3 = std::getenv("BSI_B1_SHADOWEO_PCFABFILE"); int c = envn("BSI_B1_SHADOWEO_PCFAB");
      if (c < 0) c = exists(f3 ? f3 : "/sdcard/BSI/s1cab.txt") ? 300 : 0;
      if (mode() != 1) s = 0;
      if (mode() != 1 || pcfmode() != 1) c = 0;
      /* every switch that runs flips at twice the period of the previous one, so all combinations get frames */
      if (s > 0 && c > 0) c = 2 * s;
      if (a > 0) { int m = c > 0 ? c : s; if (m > 0) a = 2 * m; }
      if (s == s1p && a == aop && c == pcp) return;
      s1p = s; aop = a; pcp = c; cnt = 0; reset();
      std::fprintf(stderr, "[b1eo] A/B alternation: S1 %s, AO blur twin %s, S1c %s%s\n", s1p ? ("on/off every " + std::to_string(s1p) + " presents").c_str() : "not alternated",
        aop ? ("on/off every " + std::to_string(aop) + " presents").c_str() : "not alternated", pcp ? ("on/off every " + std::to_string(pcp) + " presents").c_str() : "not alternated",
        gp::on() ? "" : " (BSI_B1_GPUPROF=0: no timing)"); }
    static void report() {
      static const char *nm[8] = { "S1 on/AO on", "S1 off/AO on", "S1 on/AO off", "S1 off/AO off", "S1 on, S1c off/AO on", "S1 off (S1c off)/AO on", "S1 on, S1c off/AO off", "S1 off (S1c off)/AO off" };
      const int maplab = gp::lab_id("S1 shadow block map (compute)");
      double T[8] = { }, P[8] = { }, M[8] = { }, C[8] = { }; long F[8] = { }; std::string o; char b[256];
      for (int t = 0; t < 8; t++) { F[t] = gp::tag_frames[t]; if (!F[t]) continue; const std::vector<double> &v = gp::tag_ms[t];
        auto at = [&] (int l) { return l >= 0 && l < (int)v.size() ? v[l] : 0.0; };
        T[t] = gp::tag_tot[t] / F[t]; M[t] = at(maplab) / F[t];
        for (int l : projlab) P[t] += at(l) / F[t];
        for (size_t l = 0; l < v.size() && l < gp::labels.size(); l++) if (!gp::labels[l].compare(0, 7, "compute")) C[t] += v[l] / F[t];
        std::snprintf(b, sizeof b, " | %s: %ld frames, GPU %.2f ms/frame (projection passes %.2f + block map %.2f, compute %.2f)", nm[t], F[t], T[t], P[t], M[t], C[t]); o += b; }
      double s1t = 0, s1pm = 0, aot = 0, aoc = 0, pct = 0, pcpm = 0; int ns = 0, na = 0, nc = 0;
      for (int x = 0; x < 8; x += 2) if (F[x] && F[x | 1]) { s1t += T[x | 1] - T[x]; s1pm += P[x | 1] - P[x] - M[x]; ns++; }
      for (int x = 0; x < 8; x++) if (!(x & 2) && F[x] && F[x | 2]) { aot += T[x | 2] - T[x]; aoc += C[x | 2] - C[x]; na++; }
      for (int x = 0; x < 4; x += 2) if (F[x] && F[x | 4]) { pct += T[x | 4] - T[x]; pcpm += P[x | 4] - P[x]; nc++; }   /* s1c71: S1 on, S1c on vs off */
      if (ns) { std::snprintf(b, sizeof b, " || S1 saves %.2f ms/frame of GPU time (projection passes minus map: %.2f)", s1t / ns, s1pm / ns); o += b; }
      if (na) { std::snprintf(b, sizeof b, " || AO twin saves %.2f ms/frame (compute: %.2f)", aot / na, aoc / na); o += b; }
      if (nc) { std::snprintf(b, sizeof b, " || S1c saves %.2f ms/frame of GPU time on top of S1 (projection passes: %.2f)", pct / nc, pcpm / nc); o += b; }
      std::fprintf(stderr, "[b1eo] A/B on/off, cumulative%s\n", o.c_str()); }
    static void frame() {
      if (polls++ % 600 == 0) poll();
      int nt = 0;
      if (s1p > 0 || aop > 0 || pcp > 0) { cnt++; if (s1p > 0 && ((cnt / s1p) & 1)) nt |= 1; if (aop > 0 && ((cnt / aop) & 1)) nt |= 2; if (pcp > 0 && ((cnt / pcp) & 1)) nt |= 4; }
      if ((tag & 1) && !(nt & 1)) for (auto &a : atlases) if (a.res) mark_all(a);   /* S1 back on: the whole map is rebuilt */
      tag = nt; gp::tag_cur = tag;
      if ((s1p > 0 || aop > 0 || pcp > 0) && ++rep_frames % 600 == 0 && gp::on()) report(); }
  }
  static bool ab_off() { return (abx::tag & 1) != 0; }
  static bool pcf_on() { return pcfmode() != 0 && !(abx::tag & 4); }
}

/* rhi_b1_shader.cpp, after a pixel shader was created (no lock held) */
void b1eo_ps_created(uint32_t id, const void *code, uint32_t len) {
  if (!eo::mode() || eo::creating || !id || !code) return;
  b1eo::PatchResult pr;
  const char *brk = std::getenv("BSI_B1_SHADOWEO_BREAK");   /* negative control of the A/B test: a deliberately wrong twin */
  try { pr = b1eo::patch_ps(code, len, eo::mode() == 4 ? b1eo::M_EO : eo::mode(), eo::geo(), b1eo::kLoopHash, true, brk && std::atoi(brk) > 0); } catch (...) { return; }
  if (eo::drawprof()) { unsigned long long k = (unsigned long long)b1eo::dxbc_key(code, len);   /* diagnostic draw timing: name the shadow-projection shaders */
    if (pr.candidate || k == 0x7803d1cb16398b3aull) { char nm[64]; std::snprintf(nm, sizeof nm, "proj %08llx%s", k >> 32, pr.candidate ? "" : " 1-tap"); std::lock_guard<std::mutex> lk(eo::mu); eo::diag_name[id] = nm; } }
  if (!pr.candidate) return;
  unsigned long long key = (unsigned long long)b1eo::dxbc_key(code, len);
  if (!pr.ok) { std::fprintf(stderr, "[b1eo] ps %016llx has the shadow blocker-search loop but is NOT patched: %s\n", key, pr.why.c_str()); return; }
  eo::creating = true; uint32_t pid = b1dx_shader_create(1, pr.blob.data(), (uint32_t)pr.blob.size(), 1); eo::creating = false;
  if (!pid) { std::fprintf(stderr, "[b1eo] ps %016llx: patched shader failed to compile, early-out off for it\n", key); return; }
  { std::lock_guard<std::mutex> lk(eo::mu); eo::variant[id] = pid; }
  eo::n_patched++;
  const char *nm = "?"; for (auto &t : b1eo::kTargets) if (t.key == key) nm = t.name;
  std::fprintf(stderr, "[b1eo] ps %016llx %s: early-out twin %u for shader %u (mode %d, %zu -> %zu bytes)\n", key, nm, pid, id, eo::mode(), (size_t)len, pr.blob.size());
  /* S1c: the PCF-from-bits twins on top of the S1 twin (one per comparison op the game may use; the game's is LESS_OR_EQUAL) */
  if (eo::pcfmode() && eo::mode() != 3) {
    for (int strict = 0; strict < 2; strict++) {
      b1eo::PcfResult pc;
      try { pc = b1eo::patch_ps_pcf(pr.blob.data(), pr.blob.size(), eo::pcfmode() == 2 ? b1eo::P_CHECK : b1eo::P_BITS, strict != 0, eo::pcfbreak()); } catch (...) { pc = b1eo::PcfResult(); pc.why = "exception"; }
      if (!pc.ok) { std::fprintf(stderr, "[b1eo] ps %016llx: S1c twin (%s) NOT built: %s\n", key, strict ? "LESS" : "LESS_OR_EQUAL", pc.why.c_str()); continue; }
      eo::creating = true; uint32_t qid = b1dx_shader_create(1, pc.blob.data(), (uint32_t)pc.blob.size(), 1); eo::creating = false;
      if (!qid) { std::fprintf(stderr, "[b1eo] ps %016llx: S1c twin (%s) failed to compile\n", key, strict ? "LESS" : "LESS_OR_EQUAL"); continue; }
      { std::lock_guard<std::mutex> lk(eo::mu); (strict ? eo::variant_lt : eo::variant_le)[id] = qid; eo::variant_smp[id] = pc.cmp_sampler; }
      std::fprintf(stderr, "[b1eo] ps %016llx %s: S1c twin %u (%s comparison at s%u, %s%s, %zu -> %zu bytes, search unrolled x%u, %u taps from bits)\n", key, nm, qid,
        strict ? "LESS" : "LESS_OR_EQUAL", pc.cmp_sampler, eo::pcfmode() == 2 ? "check" : "on", eo::pcfbreak() ? ", DELIBERATELY BROKEN (negative control)" : "", pr.blob.size(), pc.blob.size(), pc.unrolled, pc.taps);
    }
  }
}
/* b1dx_shader_release (g_rmu held) */
static void b1eo_ps_released(uint32_t id) {
  uint32_t pid = 0, ple = 0, plt = 0; { std::lock_guard<std::mutex> lk(eo::mu); auto it = eo::variant.find(id); if (it != eo::variant.end()) { pid = it->second; eo::variant.erase(it); }
    auto il = eo::variant_le.find(id); if (il != eo::variant_le.end()) { ple = il->second; eo::variant_le.erase(il); }
    auto ig = eo::variant_lt.find(id); if (ig != eo::variant_lt.end()) { plt = ig->second; eo::variant_lt.erase(ig); } eo::variant_smp.erase(id); }
  if (eo::cur_ps == id) { eo::cur_ps = 0; eo::cur_eo = nullptr; eo::cur_orig = nullptr; eo::cur_pcf_le = nullptr; eo::cur_pcf_lt = nullptr; eo::cur_pcf_smp = ~0u; eo::bound_eo = false; eo::bound_ptr = nullptr; }
  if (pid) b1dx_shader_release_raw(pid);
  if (ple) b1dx_shader_release_raw(ple);
  if (plt) b1dx_shader_release_raw(plt);
}

/* hooks (forward-declared near the top of rhi_b1_dxvk.cpp) */
static void b1eo_hook_draw() { eo::pre_draw(); g_eo_any = !eo::atlases.empty() || eo::cur_eo != nullptr || eo::drawprof(); }
static void b1eo_hook_rts(uint32_t newdsv) { eo::rts(newdsv); }
static void b1eo_hook_written(uint32_t res) { eo::written(res); }
static void b1eo_hook_released(uint32_t res) { eo::released(res); }
static void b1eo_hook_ps_set(uint32_t sh) { eo::ps_set(sh); g_eo_any = !eo::atlases.empty() || eo::cur_eo != nullptr || eo::drawprof(); }
static void b1eo_hook_ps_bound_orig() { eo::bound_eo = false; eo::bound_ptr = nullptr; }
static void b1eo_hook_frame() { eo::frame(); eo::abx::frame(); }
static void b1eo_hook_post_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
  if (eo::dual_pending) eo::ab_after(dc, dic);
  if (eo::diag_open) { eo::diag_open = false; gp::mark(gp::curlab); } }   /* diagnostic draw timing: back to the pass's label */
/* AO blur twin on/off alternation (rhi_b1_aoblur.inc, included after this file) */
static bool b1ab_ao_off_phase() { return (eo::abx::tag & 2) != 0; }
