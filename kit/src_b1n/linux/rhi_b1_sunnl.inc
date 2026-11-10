/* rhi_b1_sunnl.inc -- build 129 (CHECK: 135 STORAGE_BUFFER usage for the counters (raw SSBO atomics), readers by declared slot + channel masks, every reader compared, garbage only in the cascade channels; 133 counter buffers declare HOST access (the CPU read 0 before) + compare SELFTEST; 131 mask twin counters + BSI_B1_SUNNL_CTL=1 positive control; 132 the mask is recorded BY the real cascade draw, no redraw): BSI_B1_SUNNL, the sun N.L skip of the cascade (sun) shadow projection, plus the census
   of the attenuation copy pass. #included at the end of rhi_b1_shadoweo.inc (uses eo:: and shclass:: internals).
   DXBC side and the proof: rhi_b1_sunnl.h.

   BSI_B1_SUNNL = 0 off (= build 128) | 1 skip (centre pixel; DEFAULT since build 138) | 3 skip (3x3 neighbourhood, safe for a filtering
   copy pass) | 2 CHECK of variant 1 | 4 CHECK of variant 3 | 5 (b137) IN-SHADER CHECK of variant 1: the real sun light draw uses a
   twin that also recomputes its result with garbage attenuation where the skip would apply and counts bit mismatches
   (rhi_b1_sunnl.h patch_sunlight_check; no skip, no redraw, same picture; BSI_B1_SUNNL_CTL=1 = positive control). BSI_B1_SUNNL_EVERY = N (CHECK frame period, default 10).
   Timing: BSI_B1_ABAB=BSI_B1_SUNNL:72 with BSI_B1_SUNNL=1 or 3: A blocks skip, B blocks do not (abab.py: B-A > 0 = the skip saves).
   CHECK never changes the picture (the real draws run without the skip). In a CHECK frame:
     - every cascade projection draw is drawn again with the mask twin (writes no target, every pixel discards): it
       stores 1 into a mask image (RGBA8 storage, cleared to 0, PS u5) where the skip variant would skip;
     - at each attenuation copy draw (PS fs.c675b895, key fdc87d99194d396c): alt = real attenuation with every channel of a masked
       texel replaced by 0.5 (any value must give the same light), then the copy draw again with t0 = alt into altcopy
       (a copy of the copy target taken at the first copy draw of the frame);
     - at each sun light draw (fs.5dc7e57b, key d0e72728ca566e90): both light buffers are snapshotted before the draw,
       the draw runs again with t1 = altcopy into the snapshots, and both are compared byte for byte with the real ones.
     [b1sunnl] CHECK lines: compares, differing light-buffer texels (must be 0), masked texels (the skip's coverage).
   Census (every mode >= 1): the copy draws' source / target extents and formats, viewport, s0 filter and address modes,
   and every PS that reads the copy target ([b1sunnl] copy / readers lines). */
#include "rhi_b1_sunnl.h"
namespace sunnl {
  static int mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SUNNL"); v = e && *e ? std::atoi(e) : 1; /* b138: default ON (user approved 4 Nov 2026; proven exact by b137 SUNNL=5) */ if (v < 0 || v > 5) v = 0; } return v; }
  static int every() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SUNNL_EVERY"); v = e && *e ? std::atoi(e) : 10; if (v < 1) v = 1; } return v; }
  static bool checkmode() { return mode() == 2 || mode() == 4; }
  constexpr unsigned long long kCascade = 0xc29c9e4e876e162bull, kSunLight = 0xd0e72728ca566e90ull, kCopy = 0xfdc87d99194d396cull;
  constexpr uint32_t kMarkN = 0xfffffffcu, kMarkL = 0xfffffffbu;   /* g_bc.srv[1][125] / [124] while ours are bound */
  static std::mutex mu;
  static std::unordered_map<uint32_t, unsigned long long> pskey;          /* every PS id -> FNV key */
  struct Var { uint32_t twin = 0, var = 0; Rc<DxvkShader> tw, vs; };       /* twin id -> sunnl variant (skip or mask) */
  static std::vector<Var> vars;
  static unsigned long long cur_key; static uint32_t cur_ps;
  static long n_mirror, n_abb, n_frames, n_cascade, n_skipdraws, n_noready, n_checkframes, n_mask, n_copy, n_light, n_cmp, n_fail[8];
  /* sun light state (from the previous sun light draw) */
  static Rc<DxvkImageView> nview; static uint32_t nres; static float L[3]; static int lstable; static bool lup;
  static Rc<DxvkBuffer> lbuf; static Rc<DxvkBufferView> lview;
  /* CHECK images */
  struct Im { Rc<DxvkImage> img; Rc<DxvkImageView> rtv, srv, uav; VkFormat f = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0; VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT; };
  static Im mask, alt, altcopy, altl[8];
  /* b135: readers of the copy target by DECLARED PS slot, their cb0[0] channel masks, the cascade's write mask */
  struct Rd { long draws = 0, touch = 0, cmp = 0, replay = 0, depthw = 0, early = 0; int slot = -1; uint32_t chan = 0; float m[4] = { 0, 0, 0, 0 }; };
  static std::map<unsigned long long, Rd> rds; static uint32_t casc_wm; static int rd_slot = -1, snap_n; static unsigned long long rd_key;
  static Rc<DxvkBuffer> cnt2; static Rc<DxvkBufferView> cntv2;
  static Rc<DxvkShader> cscmpm;   /* b136 masked / unmasked compare */
  static Rc<DxvkBuffer> cnt3; static Rc<DxvkBufferView> cntv3;   /* b136: sun light compares ([1] outside / [2] inside the skip mask) */
  static Im dsnap; static bool ds_ok;   /* b136: depth-stencil snapshot (the real draw may change the stencil) */
  /* b136: 64-bit totals from per-report 32-bit deltas */
  static uint64_t tot[16]; static uint32_t last[16];
  static uint64_t acc(int k, uint32_t now) { tot[k] += (uint32_t)(now - last[k]); last[k] = now; return tot[k]; }   /* compares of the readers other than the sun light */
  static std::unordered_map<DxvkImage*, Rc<DxvkImageView>> realsrv;         /* sampled views of the real light buffers */
  static bool mask_bound;   /* b132: u5 / u6 bound for the real cascade draw */
  static Rc<DxvkBuffer> pcnt; static Rc<DxvkBufferView> pcntv; static long n_mask_ds;   /* b131: mask twin counters (PS u6) */
  static int ctl() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SUNNL_CTL"); v = e ? std::atoi(e) : 0; if (v < 0 || v > 2) v = 0; } return v; }   /* 1 garbage everywhere (must differ), 2 b136 identity: no garbage (must be 0) */
  static Rc<DxvkShader> csmerge, cscmp; static Rc<DxvkBuffer> cnt, prm; static Rc<DxvkBufferView> cntv; static bool cs_ok, cs_failed;
  static bool chk, mask_ready, copy_done, snap_ok, bound_now; static Rc<DxvkShader> pend_mask, pend_use; static Rc<DxvkImage> mask_for;
  /* b137 mode 5: the in-shader check twin of the sun light, its counters (PS u6) and per-report 64-bit totals */
  static Rc<DxvkShader> lt_twin; static uint32_t lt_id; static Rc<DxvkBuffer> lt_cnt; static Rc<DxvkBufferView> lt_cntv; static bool lt_bound; static long n_lt_draws, n_lt_notready, n_lt_frames;
  static uint64_t lt_tot[8]; static uint32_t lt_last[8];
  /* census */
  static uint32_t copy_res; static std::unordered_map<unsigned long long, long> readers; static long census_draws;

  void note_ps(uint32_t id, const void *code, uint32_t len) {
    if (!mode() || !id || !code) return; unsigned long long k = (unsigned long long)b1eo::dxbc_key(code, len);
    { std::lock_guard<std::mutex> lk(mu); pskey[id] = k; }
    if (mode() == 5 && k == kSunLight && lt_twin == nullptr) {   /* b137: the in-shader check twin */
      b1eo::SunResult sr; int c1 = 0; { const char *e = std::getenv("BSI_B1_SUNNL_CTL"); c1 = e && std::atoi(e) == 1; }
      try { sr = b1eo::patch_sunlight_check(code, len, c1 != 0); } catch (...) { sr = b1eo::SunResult(); sr.why = "exception"; }
      if (!sr.ok) { std::fprintf(stderr, "[b1sunnl] INSHADER twin NOT built: %s\n", sr.why.c_str()); return; }
      eo::creating = true; uint32_t vid = b1dx_shader_create(1, sr.blob.data(), (uint32_t)sr.blob.size(), 1); eo::creating = false;
      if (!vid) { std::fprintf(stderr, "[b1sunnl] INSHADER twin failed to compile\n"); return; }
      lt_id = vid; lt_twin = b1dx_shader_get(vid);
      std::fprintf(stderr, "[b1sunnl] INSHADER twin %u of the sun light %u (%u -> %zu bytes)%s\n", vid, id, len, sr.blob.size(), c1 ? " POSITIVE CONTROL (F everywhere)" : ""); } }
  /* after an S1 / S1c twin of a projection shader was created: its sunnl variant (cascade only) */
  void twin_created(unsigned long long gamekey, uint32_t twin, const std::vector<uint8_t> &blob) {
    if (!mode() || gamekey != kCascade || !twin) return;
    int variant = mode() == 1 ? b1eo::V_SKIP : mode() == 3 ? b1eo::V_SKIP3 : mode() == 2 ? b1eo::V_MASK : b1eo::V_MASK3;
    b1eo::SunResult sr; try { sr = b1eo::patch_sunnl(blob.data(), blob.size(), variant); } catch (...) { sr = b1eo::SunResult(); sr.why = "exception"; }
    if (!sr.ok) { std::fprintf(stderr, "[b1sunnl] variant %d of twin %u NOT built: %s\n", variant, twin, sr.why.c_str()); return; }
    eo::creating = true; uint32_t vid = b1dx_shader_create(1, sr.blob.data(), (uint32_t)sr.blob.size(), 1); eo::creating = false;
    if (!vid) { std::fprintf(stderr, "[b1sunnl] variant %d of twin %u failed to compile\n", variant, twin); return; }
    Var v; v.twin = twin; v.var = vid; v.tw = b1dx_shader_get(twin); v.vs = b1dx_shader_get(vid);
    { std::lock_guard<std::mutex> lk(mu); vars.push_back(v); }
    std::fprintf(stderr, "[b1sunnl] twin %u: %s variant %u (%zu -> %zu bytes)\n", twin, variant == b1eo::V_SKIP ? "skip" : variant == b1eo::V_SKIP3 ? "skip 3x3" : variant == b1eo::V_MASK ? "CHECK mask" : "CHECK mask 3x3", vid, blob.size(), sr.blob.size()); }
  void twin_released(uint32_t twin) {
    uint32_t rel = 0; { std::lock_guard<std::mutex> lk(mu); for (size_t i = 0; i < vars.size(); i++) if (vars[i].twin == twin) { rel = vars[i].var; vars.erase(vars.begin() + i); break; } }
    if (rel) b1dx_shader_release_raw(rel); }
  static Rc<DxvkShader> variant_of(const Rc<DxvkShader> &use) {
    std::lock_guard<std::mutex> lk(mu); for (auto &v : vars) if (v.tw.ptr() == use.ptr()) return v.vs; return nullptr; }
  void ps_set(uint32_t sh) { cur_ps = sh; cur_key = 0; if (!mode() || !sh) return; std::lock_guard<std::mutex> lk(mu); auto it = pskey.find(sh); if (it != pskey.end()) cur_key = it->second; }

  static void ensure_lbuf() {
    if (lbuf != nullptr) return;
    DxvkBufferCreateInfo bi = { }; bi.size = 32; bi.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;   /* b137: [1] = garbage values of the mode-5 check */
    bi.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 sunnl L";
    lbuf = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32G32B32A32_SFLOAT; vk.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 32; lview = lbuf->createView(vk);
    float z[8] = { 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.5f, 0.f }; emit([b = lbuf, z] (DxvkContext* c) { c->updateBuffer(b, 0, 32, z); }); }
  static bool ready() { return nview != nullptr && lup && lstable >= 2; }
  static void unbind_inputs() {   /* the G-buffer normal must not stay bound while it is a render target again */
    if (!bound_now) return; bound_now = false;
    emit([] (DxvkContext* c) { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 125u, nullptr); c->bindResourceBufferView(VK_SHADER_STAGE_FRAGMENT_BIT, 124u, nullptr); });
    g_bc.srv[1][125] = 0; g_bc.srv[1][124] = 0; }
  static void bind_inputs() {
    bound_now = true;
    if (g_bc.srv[1][125] != kMarkN) { emit([v = nview] (DxvkContext* c) mutable { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 125u, std::move(v)); }); g_bc.srv[1][125] = kMarkN; }
    if (g_bc.srv[1][124] != kMarkL) { emit([v = lview] (DxvkContext* c) mutable { c->bindResourceBufferView(VK_SHADER_STAGE_FRAGMENT_BIT, 124u, std::move(v)); }); g_bc.srv[1][124] = kMarkL; } }

  static Im mkim(VkFormat f, uint32_t w, uint32_t h, bool rt, bool storage, const char *nm) {
    Im m; DxvkImageCreateInfo ii = { };
    ii.type = VK_IMAGE_TYPE_2D; ii.format = f; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT; ii.extent = { w, h, 1u }; ii.numLayers = 1; ii.mipLevels = 1;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u) | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    ii.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    ii.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | (storage ? VK_ACCESS_SHADER_WRITE_BIT : 0u);
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = storage ? VK_IMAGE_LAYOUT_GENERAL : rt ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; ii.debugName = nm;
    m.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); m.f = f; m.w = w; m.h = h;
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = f; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    if (rt) { k.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; m.rtv = m.img->createView(k); }
    if (storage) { k.usage = VK_IMAGE_USAGE_STORAGE_BIT; k.layout = VK_IMAGE_LAYOUT_GENERAL; m.uav = m.img->createView(k); }
    k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED; m.srv = m.img->createView(k);
    std::fprintf(stderr, "[b1sunnl] CHECK image %s %ux%u fmt %d\n", nm, w, h, (int)f);
    return m; }
  static bool fit(Im &m, const Rc<DxvkImage> &like, bool storage, const char *nm) {
    VkExtent3D e = like->mipLevelExtent(0); VkFormat f = like->info().format;
    if (m.img == nullptr || m.f != f || m.w != e.width || m.h != e.height) m = mkim(f, e.width, e.height, true, storage, nm);
    return m.img != nullptr; }
  /* b136: depth-stencil snapshot image (same format / size, attachment + transfer), its DSV in rtv */
  static bool fit_ds(Im &m, const Rc<DxvkImage> &like) {
    VkExtent3D e = like->mipLevelExtent(0); VkFormat f = like->info().format;
    if (m.img != nullptr && m.f == f && m.w == e.width && m.h == e.height) return true;
    const DxvkFormatInfo *fi = lookupFormatInfo(f); if (!fi) return false;
    DxvkImageCreateInfo ii = { }; ii.type = VK_IMAGE_TYPE_2D; ii.format = f; ii.sampleCount = like->info().sampleCount; ii.extent = { e.width, e.height, 1u }; ii.numLayers = 1; ii.mipLevels = 1;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    ii.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; ii.debugName = "B1 sunnl depth-stencil snapshot";
    m = Im(); m.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (m.img == nullptr) return false;
    m.f = f; m.w = e.width; m.h = e.height; m.asp = fi->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = f; k.aspects = m.asp; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    k.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; m.rtv = m.img->createView(k);
    std::fprintf(stderr, "[b1sunnl] CHECK image depth-stencil snapshot %ux%u fmt %d aspects %x\n", e.width, e.height, (int)f, (unsigned)m.asp);
    return true; }
  static Rc<DxvkImageView> srv_of(const Rc<DxvkImage> &img) {
    auto it = realsrv.find(img.ptr()); if (it != realsrv.end()) return it->second;
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = img->info().format; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED; Rc<DxvkImageView> v = img->createView(k); realsrv[img.ptr()] = v; return v; }
  /* b133: a counter buffer the CPU can read. DXVK 3.1.1 emits the shader-write -> host-read barrier only when the buffer's
     declared access has a HOST bit (DxvkContext::accessBuffer uses info().stages / access as the barrier destination,
     DxvkBarrierBatch::finalize adds the host barrier only then); without it Turnip leaves the atomics in the GPU caches and
     the CPU read 0 (b129-b132 CHECK, S1 stats j0136 / j0155). TRANSFER_DST for the self-test's pattern write. */
  static Rc<DxvkBuffer> mkcnt(const char *nm, Rc<DxvkBufferView> &view) {
    DxvkBufferCreateInfo ai = { }; ai.size = 16; ai.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;   /* b135: STORAGE_BUFFER: on Turnip (minStorageBufferAlignment <= 4) dxbc-spirv lowers typed-buffer atomics to raw SSBO (useRawForTypedAtomic) and DXVK binds bufferView->getDescriptor(raw), which exists only with this usage; without it the atomics hit a null descriptor (b129-b134) */
    ai.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    ai.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT; ai.debugName = nm;
    Rc<DxvkBuffer> b = g_dev->createBuffer(ai, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); std::memset(b->mapPtr(0), 0, 16);
    DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 16; view = b->createView(vk);
    return b; }
  static void gpu_sync() { ctx_flush(); cs_sync(); g_dev->waitForIdle(); }   /* as the dumps: submit, drain the CS thread, wait for the GPU */
  static void cmp_views(Rc<DxvkImageView> ra, Rc<DxvkImageView> rb, uint32_t w, uint32_t h, Rc<DxvkBufferView> cv);
  static void selftest();
  static void cmp_masked(Rc<DxvkImageView> ra, Rc<DxvkImageView> rb, uint32_t w, uint32_t h, Rc<DxvkBufferView> cv);
  static bool init_cs() {
    if (cs_ok || cs_failed) return cs_ok;
    auto m = b1eo::build_cs_sunnl_merge(ctl() == 1); uint32_t im = b1dx_shader_create(5, m.data(), (uint32_t)m.size(), 1);
    auto c = b1eo::build_cs_compare(); uint32_t ic = b1dx_shader_create(5, c.data(), (uint32_t)c.size(), 1);
    { auto cm2 = b1eo::build_cs_compare_masked(); uint32_t i2 = b1dx_shader_create(5, cm2.data(), (uint32_t)cm2.size(), 1); if (i2) cscmpm = b1dx_shader_get(i2); }
    if (!im || !ic) { cs_failed = true; std::fprintf(stderr, "[b1sunnl] CHECK compute shaders failed to build: CHECK off\n"); return false; }
    csmerge = b1dx_shader_get(im); cscmp = b1dx_shader_get(ic);
    cnt = mkcnt("B1 sunnl counters", cntv); pcnt = mkcnt("B1 sunnl mask counters", pcntv); cnt2 = mkcnt("B1 sunnl other readers", cntv2); cnt3 = mkcnt("B1 sunnl sun light compares", cntv3);
    DxvkBufferCreateInfo bi = { }; bi.size = 256; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 sunnl params";
    prm = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cs_ok = true; selftest(); return cs_ok; }
  static void redraw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    if (dc) { VkDrawIndirectCommand x = *dc; emit([x] (DxvkContext* c) { c->draw(1, &x); }); }
    else { VkDrawIndexedIndirectCommand x = *dic; emit([x] (DxvkContext* c) { c->drawIndexed(1, &x); }); } }
  static void compare(const Rc<DxvkImage> &real, const Im &a) { VkExtent3D e = real->mipLevelExtent(0); cmp_views(srv_of(real), a.srv, e.width, e.height, cntv); n_cmp++; }
  /* u61[1] of cv += texels of ra and rb that differ */
  static void cmp_views(Rc<DxvkImageView> ra_, Rc<DxvkImageView> rb_, uint32_t w, uint32_t h, Rc<DxvkBufferView> cv_) {
    struct P { int32_t v[4]; } p = { { (int32_t)w - 1, (int32_t)h - 1, 0, 0 } };
    Rc<DxvkShader> game = b1dx_shader_get(g_cur_cs);
    emit([cs = cscmp, game, pb = prm, p, ra = std::move(ra_), b = std::move(rb_), cv = std::move(cv_), gx = (w + 7) / 8, gy = (h + 7) / 8] (DxvkContext* c) mutable {
      const uint32_t cbs = 5u * 16u + 13u;
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
      c->updateBuffer(pb, 0, sizeof p, &p);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 0, 64));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, std::move(ra));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, std::move(b));
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, std::move(cv));
      c->dispatch(gx, gy, 1);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, nullptr);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, nullptr);
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, nullptr);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice());
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); }); }
  /* b136: [1] differing texels outside the skip mask, [2] inside it */
  static void cmp_masked(Rc<DxvkImageView> ra_, Rc<DxvkImageView> rb_, uint32_t w, uint32_t h, Rc<DxvkBufferView> cv_) {
    if (cscmpm == nullptr) { cmp_views(std::move(ra_), std::move(rb_), w, h, std::move(cv_)); return; }
    struct P { int32_t v[4]; } p = { { (int32_t)w - 1, (int32_t)h - 1, (mask.img != nullptr && mask.w == w && mask.h == h) ? -1 : 0, 0 } };
    Rc<DxvkShader> game = b1dx_shader_get(g_cur_cs);
    emit([cs = cscmpm, game, pb = prm, p, ra = std::move(ra_), b = std::move(rb_), mk = mask.srv, cv = std::move(cv_), gx = (w + 7) / 8, gy = (h + 7) / 8] (DxvkContext* c) mutable {
      const uint32_t cbs = 5u * 16u + 13u;
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
      c->updateBuffer(pb, 64, sizeof p, &p);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 64, 64));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, std::move(ra));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, std::move(b));
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 123u, std::move(mk));
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, std::move(cv));
      c->dispatch(gx, gy, 1);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, nullptr);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, nullptr);
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 123u, nullptr);
      c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, nullptr);
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice());
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); }); }
  /* b133 positive control of the compare + readback, independent of the game: two known different images must give 64*64
     differing texels, two identical ones 0, and a transfer-written pattern must read back */
  static void selftest() {
    static bool done; if (done) return; done = true;
    Im a = mkim(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, true, false, "selftest a"), b = mkim(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, true, false, "selftest b"),
       c = mkim(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, true, false, "selftest c");
    Rc<DxvkBufferView> v1, v2; Rc<DxvkBuffer> b1 = mkcnt("B1 sunnl selftest 1", v1), b2 = mkcnt("B1 sunnl selftest 2", v2);
    VkClearValue z = { }, one = { }; one.color.float32[0] = one.color.float32[1] = one.color.float32[2] = one.color.float32[3] = 1.f;
    const uint32_t pat = 0xa5a5a5a5u;
    emit([ia = a.img, ib = b.img, ic = c.img, va = a.rtv, vb = b.rtv, vc = c.rtv, z, one] (DxvkContext* x) {
      VkExtent3D e = { 64u, 64u, 1u };
      x->initImage(ia, VK_IMAGE_LAYOUT_UNDEFINED); x->initImage(ib, VK_IMAGE_LAYOUT_UNDEFINED); x->initImage(ic, VK_IMAGE_LAYOUT_UNDEFINED);
      x->clearImageView(va, VkOffset3D{ 0, 0, 0 }, e, VK_IMAGE_ASPECT_COLOR_BIT, z); x->clearImageView(vb, VkOffset3D{ 0, 0, 0 }, e, VK_IMAGE_ASPECT_COLOR_BIT, one);
      x->clearImageView(vc, VkOffset3D{ 0, 0, 0 }, e, VK_IMAGE_ASPECT_COLOR_BIT, z); });
    cmp_views(a.srv, b.srv, 64, 64, v1); cmp_views(a.srv, c.srv, 64, 64, v2);
    emit([b1, pat] (DxvkContext* x) { x->updateBuffer(b1, 12, 4, &pat); });
    gpu_sync();
    const uint32_t *p1 = (const uint32_t *)b1->mapPtr(0), *p2 = (const uint32_t *)b2->mapPtr(0);
    bool ok = p1[1] == 64u * 64u && p2[1] == 0u && p1[3] == pat;
    std::fprintf(stderr, "[b1sunnl] SELFTEST %s: compare of two different images %u texels (must be 4096), of two identical images %u (must be 0), transfer pattern %08x (must be a5a5a5a5)%s\n",
      ok ? "PASS" : "FAIL", p1[1], p2[1], p1[3], ok ? "" : ": the CHECK cannot see differences, CHECK off");
    if (!ok) { cs_ok = false; cs_failed = true; } }
  static Rc<DxvkImage> rt_img(int i) { View *v = g_views.get(g_cs.rtv[i]); if (!v || v->type != 2) return nullptr; Obj *o = robj(v->res); return (o && o->kind == 2) ? o->img : nullptr; }
  static Rc<DxvkImageView> srv_at(uint32_t slot) { uint32_t s = g_bc.srv[1][slot]; if (!s || s == BC_UNK) return nullptr; View *v = g_views.get(s); return (v && v->type == 1) ? v->iv : nullptr; }

  /* eo::pre_draw_inner, a cascade projection draw with an S1 / S1c twin chosen (use): the variant for this draw */
  Rc<DxvkShader> cascade(const Rc<DxvkShader> &use) {
    if (!mode() || mode() == 5 || cur_key != kCascade) return use;   /* b137 mode 5: the cascade draws unchanged */
    n_cascade++;
    { BS *bs = g_bss.get(g_cs.bs); casc_wm |= bs ? (uint32_t)bs->m[0].writeMask() : 0xfu; }   /* b135: channels the projection writes */
    if (!ready()) { n_noready++; return use; }
    Rc<DxvkShader> v = variant_of(use); if (v == nullptr) { n_fail[0]++; return use; }
    if (checkmode()) {   /* b132 CHECK: the REAL draw uses the mask twin (= original code + mask store; same picture), u5 / u6 bound for it */
      if (!chk || !cs_ok) return use;
      Rc<DxvkImage> r = rt_img(0); if (r == nullptr) { n_fail[5]++; return use; }
      VkExtent3D e = r->mipLevelExtent(0);
      if (mask.img == nullptr || mask.w != e.width || mask.h != e.height) mask = mkim(VK_FORMAT_R8G8B8A8_UNORM, e.width, e.height, false, true, "mask");
      if (mask_for != r || !mask_ready) {   /* first cascade draw of the frame: mask = 0 */
        VkClearValue cv = { };
        emit([i = mask.img, mv = mask.uav, e, cv] (DxvkContext* x) { x->initImage(i, VK_IMAGE_LAYOUT_UNDEFINED); x->clearImageView(mv, VkOffset3D{ 0, 0, 0 }, e, VK_IMAGE_ASPECT_COLOR_BIT, cv); });
        mask_for = r; mask_ready = true; }
      bind_inputs();
      emit([u = mask.uav, pv = pcntv] (DxvkContext* x) mutable { x->bindResourceImageView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 5u, std::move(u)); x->bindResourceBufferView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 6u, std::move(pv)); });
      mask_bound = true; n_mask++; return v; }
    if (tl126::flipped("BSI_B1_SUNNL")) { n_abb++; return use; }   /* ABAB B blocks: no skip */
    bind_inputs(); n_skipdraws++; return v; }
  /* every draw, before it (after eo's shader choice): the sun light and the copy pass */
  void pre_draw() {
    if (!mode()) return;
    /* b135: readers of the copy target = draws whose PS DECLARES an SRV slot that holds it (b129-b134 counted any bound slot) */
    rd_slot = -1; snap_ok = false;
    if (copy_res && cur_ps && cur_key != kCopy) { uint64_t sm = 0; uint32_t cbm = 0, ssm = 0;
      if (b1dx_shader_slots(cur_ps, &sm, &cbm, &ssm)) for (int i = 0; i < 64; i++) { if (!((sm >> i) & 1ull)) continue; uint32_t sv = g_bc.srv[1][i]; if (!sv || sv == BC_UNK || sv >= kMarkL) continue;
        View *v = g_views.get(sv); if (v && v->type == 1 && v->res == copy_res) { rd_slot = i; break; } }
      else n_fail[3]++; }
    if (rd_slot >= 0) {
      Rd &d = rds[cur_key]; d.draws++; d.slot = rd_slot; rd_key = cur_key; readers[cur_key]++; census_draws++;
      const float *m = shclass::cbcopy(g_bc.cb[1][0], 16);   /* the lights read the attenuation only as dp4(att, cb0[0]) */
      bool t = false;
      if (m) { std::memcpy(d.m, m, 16); for (int k = 0; k < 4; k++) if (m[k] != 0.f) d.chan |= 1u << k; }
      if (!casc_wm) d.early++;   /* b136: before the first cascade draw the projection's channels are unknown (b135 counted these as exposed) */
      else if (!m) t = true; else for (int k = 0; k < 4; k++) if ((casc_wm >> k & 1u) && m[k] != 0.f) t = true;
      if (t) d.touch++;
      DSS *ds = g_dsss.get(g_cs.dss); bool dw = ds && ds->s.depthWrite();
      if (dw) d.depthw++;
      if (chk && copy_done && cs_ok && !dw) {   /* CHECK: snapshot every bound colour target before the real draw */
        snap_ok = true; snap_n = 0;
        for (int i = 0; i < 8; i++) { Rc<DxvkImage> r = rt_img(i); if (r == nullptr) continue; static const char *nm[8] = { "reader rt0 alt", "reader rt1 alt", "reader rt2 alt", "reader rt3 alt", "reader rt4 alt", "reader rt5 alt", "reader rt6 alt", "reader rt7 alt" };
          if (!fit(altl[i], r, false, nm[i])) { n_fail[3]++; snap_ok = false; break; }
          VkExtent3D e = r->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
          emit([dd = altl[i].img, r, sl, e] (DxvkContext* x) { x->copyImage(dd, sl, VkOffset3D{ 0, 0, 0 }, r, sl, VkOffset3D{ 0, 0, 0 }, e); }); snap_n = i + 1; }
        ds_ok = false;   /* b136: the depth-stencil too, so the redraw sees the pre-draw stencil / depth and writes only into snapshots */
        if (snap_ok) { View *dv = g_views.get(g_cs.dsv); if (dv && dv->type == 3 && dv->iv != nullptr) { Rc<DxvkImage> di = dv->iv->image();
          if (fit_ds(dsnap, di)) { VkExtent3D e = di->mipLevelExtent(0); VkImageSubresourceLayers sl = { dsnap.asp, 0u, 0u, 1u };
            emit([dd = dsnap.img, di, sl, e] (DxvkContext* x) { x->copyImage(dd, sl, VkOffset3D{ 0, 0, 0 }, di, sl, VkOffset3D{ 0, 0, 0 }, e); }); ds_ok = true; }
          else { n_fail[4]++; snap_ok = false; } } } } }
    if (cur_key == kSunLight) {
      ensure_lbuf();
      View *nv = g_views.get(g_bc.srv[1][0] == BC_UNK ? 0 : g_bc.srv[1][0]); Obj *no = nv && nv->type == 1 ? robj(nv->res) : nullptr;
      if (no && no->kind == 2 && (no->fmt == VK_FORMAT_R8G8B8A8_UNORM || no->fmt == VK_FORMAT_B8G8R8A8_UNORM || no->fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        if (nres != nv->res) { nres = nv->res; nview = nv->iv; lstable = 0; } }
      else { nview = nullptr; n_fail[1]++; }
      const float *c = shclass::cbcopy(g_bc.cb[1][0], 32);
      if (c) { if (std::memcmp(L, c + 4, 12) == 0) { if (lstable < 1000) lstable++; } else { std::memcpy(L, c + 4, 12); lstable = 0; lup = false; }
        if (!lup && lstable >= 1) { float u[4] = { L[0], L[1], L[2], 1.f }; emit([b = lbuf, u] (DxvkContext* x) { x->updateBuffer(b, 0, 16, u); }); lup = true; } }
      else n_fail[2]++;
      if (mode() == 5 && lt_twin != nullptr) {   /* b137: the real draw with the in-shader check twin (same picture), every draw of a check frame */
        if (!chk) { }
        else if (!ready()) n_lt_notready++;
        else {
          if (lt_cnt == nullptr) { DxvkBufferCreateInfo ai = { }; ai.size = 32; ai.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;   /* as mkcnt (b133 HOST, b135 STORAGE_BUFFER), 8 counters */
            ai.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
            ai.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT; ai.debugName = "B1 sunnl in-shader counters";
            lt_cnt = g_dev->createBuffer(ai, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); std::memset(lt_cnt->mapPtr(0), 0, 32);
            DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = 0; vk.size = 32; lt_cntv = lt_cnt->createView(vk); }
          bind_inputs();
          emit([t = lt_twin, cv = lt_cntv] (DxvkContext* x) mutable { x->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(t)); x->bindResourceBufferView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 6u, std::move(cv)); });
          lt_bound = true; n_lt_draws++; } }
    }
    if (cur_key == kCopy) {
      n_copy++;
      View *rv = g_views.get(g_cs.rtv[0]); if (rv && rv->type == 2) copy_res = rv->res;
      if (n_copy <= 4 || n_copy % 3000 == 0) {   /* census of the copy pass */
        View *sv = g_views.get(g_bc.srv[1][0] == BC_UNK ? 0 : g_bc.srv[1][0]); Obj *so = sv && sv->type == 1 ? robj(sv->res) : nullptr; Obj *ro = rv && rv->type == 2 ? robj(rv->res) : nullptr;
        SS *ss = g_sss.get(g_bc.ss[1][0] == BC_UNK ? 0 : g_bc.ss[1][0]);
        VkExtent3D se = so && so->kind == 2 ? so->img->mipLevelExtent(0) : VkExtent3D{ 0, 0, 0 }, re = ro && ro->kind == 2 ? ro->img->mipLevelExtent(0) : VkExtent3D{ 0, 0, 0 };
        std::fprintf(stderr, "[b1sunnl] copy draw %ld: src %ux%u fmt %d -> dst %ux%u fmt %d, viewport %.1f %.1f %.1f %.1f, s0 %s min %u mag %u mip %u addr %u %u\n", n_copy,
          se.width, se.height, so ? (int)so->fmt : -1, re.width, re.height, ro ? (int)ro->fmt : -1, g_cs.nvp ? g_cs.vp[0][0] : -1.f, g_cs.nvp ? g_cs.vp[0][1] : -1.f, g_cs.nvp ? g_cs.vp[0][2] : -1.f, g_cs.nvp ? g_cs.vp[0][3] : -1.f,
          ss ? "set" : "none", ss ? (unsigned)ss->s->key().u.p.minFilter : 0u, ss ? (unsigned)ss->s->key().u.p.magFilter : 0u, ss ? (unsigned)ss->s->key().u.p.mipMode : 0u,
          ss ? (unsigned)ss->s->key().u.p.addressU : 0u, ss ? (unsigned)ss->s->key().u.p.addressV : 0u); }
      if (chk && mask_ready && cs_ok && !copy_done) {   /* CHECK: altcopy = the copy target before the frame's first copy draw */
        Rc<DxvkImage> r = rt_img(0);
        if (r != nullptr && fit(altcopy, r, false, "copy alt")) { VkExtent3D e = r->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
          emit([d = altcopy.img, r, sl, e] (DxvkContext* x) { x->copyImage(d, sl, VkOffset3D{ 0, 0, 0 }, r, sl, VkOffset3D{ 0, 0, 0 }, e); }); copy_done = true; }
        else n_fail[4]++; }
    }
  }
  /* every draw, after it */
  void post_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    if (!mode()) return;
    struct Un { ~Un() { unbind_inputs(); } } un_;   /* after the real draw and any CHECK redraw */
    if (lt_bound) {   /* b137: after the real sun light draw (and pp76's right-eye replay) with the check twin: the game's shader back */
      lt_bound = false;
      emit([s = b1dx_shader_get(cur_ps)] (DxvkContext* x) mutable { x->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); x->bindResourceBufferView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 6u, nullptr); });
      return; }
    if (mask_bound) {   /* b132: after the real cascade draw (and pp76's right-eye replay) with the mask twin */
      mask_bound = false;
      emit([] (DxvkContext* x) { x->bindResourceImageView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 5u, nullptr); x->bindResourceBufferView(VK_SHADER_STAGE_ALL_GRAPHICS, kSrvTotal + 6u, nullptr); });
      return; }
    if (!chk || !cs_ok) return;
    if (cur_key == kCopy && copy_done) {   /* alt = real attenuation with the masked channels replaced; the copy again into altcopy */
      Rc<DxvkImageView> att = srv_at(0); Obj *ao = nullptr; { View *sv = g_views.get(g_bc.srv[1][0] == BC_UNK ? 0 : g_bc.srv[1][0]); if (sv && sv->type == 1) ao = robj(sv->res); }
      if (att == nullptr || !ao || ao->kind != 2) { n_fail[6]++; return; }
      if (ao->img != mask_for) {   /* a copy from something else (e.g. a 1x1 default attenuation): mirror it into altcopy unchanged */
        DxvkRenderTargets rts = eo::cur_rts(), alt_ = rts; alt_.color[0].view = altcopy.rtv;
        emit([alt_] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(alt_), 0u); });
        redraw(dc, dic);
        emit([rts] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(rts), 0u); });
        n_mirror++; return; }
      if (ao->fmt != VK_FORMAT_R8G8B8A8_UNORM) { static int w; if (w++ < 2) std::fprintf(stderr, "[b1sunnl] CHECK: attenuation format %d is not RGBA8 UNORM (no storage alt): CHECK off\n", (int)ao->fmt); cs_ok = false; return; }
      if (!fit(alt, ao->img, true, "attenuation alt")) { n_fail[6]++; return; }
      VkExtent3D e = ao->img->mipLevelExtent(0); Rc<DxvkShader> game = b1dx_shader_get(g_cur_cs);
      struct CM { uint32_t v[4]; } cm; { uint32_t wm = ctl() == 2 ? 0u : casc_wm ? casc_wm : 0xfu; for (int k = 0; k < 4; k++) cm.v[k] = (wm >> k & 1u) ? 0xffffffffu : 0u; }   /* b135; b136 CTL=2: no garbage */
      emit([cs = csmerge, game, pb = prm, cm, a = srv_of(ao->img), m = mask.srv, u = alt.uav, cv = cntv, gx = (e.width + 7) / 8, gy = (e.height + 7) / 8] (DxvkContext* x) mutable {
        x->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
        x->updateBuffer(pb, 128, sizeof cm, &cm); x->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 16u + 13u, DxvkBufferSlice(pb, 128, 64));
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, std::move(a));
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, std::move(m));
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 60u, std::move(u));
        x->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, std::move(cv));
        x->dispatch(gx, gy, 1);
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 127u, nullptr);
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 128u + 125u, nullptr);
        x->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 60u, nullptr);
        x->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, kUavCompute + 61u, nullptr);
        x->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, 5u * 16u + 13u, DxvkBufferSlice());
        x->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(game)); });
      DxvkRenderTargets rts = eo::cur_rts(), alt_ = rts; alt_.color[0].view = altcopy.rtv;
      emit([alt_, s = alt.srv] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(alt_), 0u); x->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 0u, std::move(s)); });
      redraw(dc, dic);
      emit([rts, att] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(rts), 0u); x->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 0u, std::move(att)); });
      return; }
    if (rd_slot >= 0 && copy_done && snap_ok) {   snap_ok = false;   /* b135: EVERY reader again from altcopy into the snapshots, then compare */
      Rd &d = rds[rd_key];
      if (b1vr::on && b1vr::pp_post_wanted()) { d.replay++; return; }   /* replayed for the right eye: one redraw would not reproduce it */
      Rc<DxvkImageView> orig = srv_at((uint32_t)rd_slot); if (orig == nullptr) { n_fail[7]++; return; }
      DxvkRenderTargets rts = eo::cur_rts(), alt_ = rts; for (int i = 0; i < snap_n; i++) if (rt_img(i) != nullptr) alt_.color[i].view = altl[i].rtv;
      if (ds_ok && alt_.depth.view != nullptr) alt_.depth.view = dsnap.rtv;   /* b136 */
      emit([alt_, s = altcopy.srv, sl = (uint32_t)rd_slot] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(alt_), 0u); x->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, sl, std::move(s)); });
      redraw(dc, dic);
      emit([rts, orig, sl = (uint32_t)rd_slot] (DxvkContext* x) mutable { x->bindRenderTargets(std::move(rts), 0u); x->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, sl, std::move(orig)); });
      for (int i = 0; i < snap_n; i++) { Rc<DxvkImage> r = rt_img(i); if (r == nullptr) continue; VkExtent3D e = r->mipLevelExtent(0);
        cmp_masked(srv_of(r), altl[i].srv, e.width, e.height, rd_key == kSunLight ? cntv3 : cntv2); n_cmp++; }
      d.cmp++; if (rd_key == kSunLight) n_light++; }
  }
  static void report() {
    std::string rd; for (auto &kv : readers) { char b[48]; std::snprintf(b, sizeof b, " %016llx x%ld", kv.first, kv.second); rd += b; }
    std::fprintf(stderr, "[b1sunnl] mode %d, %ld frames: cascade draws %ld (skip variant %ld, ABAB-B %ld, not ready %ld), L %.6f %.6f %.6f stable %d, normal view %s | copy draws %ld | copy-target readers (%ld draws):%s | fails %ld %ld %ld %ld %ld %ld %ld %ld\n",
      mode(), n_frames, n_cascade, n_skipdraws, n_abb, n_noready, L[0], L[1], L[2], lstable, nview != nullptr ? "yes" : "no", n_copy, census_draws, rd.empty() ? " none" : rd.c_str(),
      n_fail[0], n_fail[1], n_fail[2], n_fail[3], n_fail[4], n_fail[5], n_fail[6], n_fail[7]);
    if (checkmode() && cnt != nullptr && cnt3 != nullptr && cnt2 != nullptr && pcnt != nullptr) { gpu_sync();   /* b133: read after the GPU finished; b136: 64-bit totals from 32-bit deltas */
      const uint32_t *p = (const uint32_t *)cnt->mapPtr(0), *q = (const uint32_t *)pcnt->mapPtr(0), *s3 = (const uint32_t *)cnt3->mapPtr(0), *s2 = (const uint32_t *)cnt2->mapPtr(0);
      uint64_t masked = acc(0, p[2]), tw0 = acc(1, q[0]), tw1 = acc(2, q[1]), tw2 = acc(3, q[2]), su = acc(4, s3[1]), sm = acc(5, s3[2]), ou = acc(6, s2[1]), om = acc(7, s2[2]);
      const char *want = ctl() == 1 ? "POSITIVE CONTROL: garbage everywhere, must be > 0" : ctl() == 2 ? "IDENTITY CONTROL: no garbage, must be 0" : "must be 0";
      std::fprintf(stderr, "[b1sunnl] CHECK (variant %s, %s): %ld check frames, mask draws %ld, light compares %ld (images %ld) | sun light texels that DIFFER: outside the skip mask %llu, inside %llu | other readers DIFFER: outside %llu, inside %llu | masked attenuation texels %llu | mask twin pixels %llu, enabled %llu, would skip %llu\n",
        mode() == 2 ? "centre" : "3x3", want, n_checkframes, n_mask, n_light, n_cmp, (unsigned long long)su, (unsigned long long)sm, (unsigned long long)ou, (unsigned long long)om,
        (unsigned long long)masked, (unsigned long long)tw0, (unsigned long long)tw1, (unsigned long long)tw2); }
    for (auto &kv : rds) { const Rd &d = kv.second;   /* b135 reader census: a reader whose cb0[0] mask touches the cascade's channels sees the skip */
      std::fprintf(stderr, "[b1sunnl] reader %016llx%s: slot t%d, %ld draws, cb0[0] mask %g %g %g %g, channels read %x, draws before the cascade mask was known %ld, draws touching the cascade channels (wm %x) %ld%s, CHECK compares %ld, replayed (not compared) %ld, depth-writing (not compared) %ld\n",
        kv.first, kv.first == kSunLight ? " (sun light)" : "", d.slot, d.draws, d.m[0], d.m[1], d.m[2], d.m[3], d.chan, d.early, casc_wm, d.touch, kv.first != kSunLight && d.touch ? " = EXPOSED TO THE SKIP" : "", d.cmp, d.replay, d.depthw); } }
  void frame() {
    if (!mode()) return;
    n_frames++; mask_ready = false; copy_done = false; pend_mask = nullptr; pend_use = nullptr;
    chk = checkmode() && n_frames > 300 && n_frames % every() == 0; if (chk) { if (init_cs()) n_checkframes++; else chk = false; }
    if (mode() == 5) { chk = n_frames > 300 && n_frames % every() == 0; if (chk) n_lt_frames++;
      if (n_frames % 60 == 0 && lt_cnt != nullptr) { gpu_sync();
        const uint32_t *q = (const uint32_t *)lt_cnt->mapPtr(0); uint64_t v[6];
        for (int k = 0; k < 6; k++) { lt_tot[k] += (uint32_t)(q[k] - lt_last[k]); lt_last[k] = q[k]; v[k] = lt_tot[k]; }
        std::fprintf(stderr, "[b1sunnl] INSHADER CHECK%s: compares %llu, mismatches %llu (must be %s) | check frames %ld, light draws %ld (not ready %ld) | light pixels %llu | sample != own texel %llu (must be 0) | sample off texel centre %llu | F with this frame's sat(d) > 0 %llu\n",
          ctl() == 1 ? " (POSITIVE CONTROL)" : "", (unsigned long long)v[1], (unsigned long long)v[2], ctl() == 1 ? "> 0" : "0", n_lt_frames, n_lt_draws, n_lt_notready,
          (unsigned long long)v[0], (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5]); } }
    if (n_frames % 600 == 0) report(); }
}
