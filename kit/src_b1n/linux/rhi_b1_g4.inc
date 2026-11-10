/* rhi_b1_g4.inc -- g4: sun-shadow mask coalescing (BSI_B1_SHCOAL). #included at the end of rhi_b1_dxvk.cpp, after
   rhi_b1_shadoweo.inc (it uses the object tables, emit(), g_rmu, the gp profiler, s2 and S1's eo:: internals).
   Notes: /home/claude/bsi/g4_notes/SHCOAL.md.

   Problem (Quest 3, Turnip): UE3 renders the dominant light's shadow as [atlas depth pass][mask-target draws] x 10-15 per
   frame; every atlas pass ends the mask target's render pass, so the tiler stores and reloads 2976x1040 RGBA8 + D24S8 each
   time (~8 ms/frame of pass-break overhead measured). Here all work that targets the MASK target is recorded into a
   deferred command stream (DXVK CS chunks of our own) while the atlas passes run live; the deferred stream is replayed
   back-to-back when the game binds any other target (or at present / on anything unexpected), so the mask target sees
   1-2 render passes. Each atlas pass of the sequence renders into its own ring slot (slot 0 = the game's atlas image,
   slots 1..N-1 private 2048x2048 D32 images registered as ordinary resources with their own DSV/SRV view ids), so the
   deferred projection draws can sample "the atlas as it was after pass k" at replay time.

   Recording level = (B) the emit() lambdas themselves, captured by emit()/emit_data() while g4_cap && !g4_live (see the
   G4_DEFER scopes in the bind/draw/clear/rts entry points of rhi_b1_dxvk.cpp). Everything emitted inside those entry
   points while the mask target is bound goes to the deferred stream: the game's state binds, apply_viewports, s2's
   stencil-drop state, S1's twin shader + map binds, pp76's right-eye replay draws. Live by construction: resource
   writes and uploads (other entry points), ctx_flush, the S1 map build (G4Live in eo::reduce), queries, compute.
   Per-draw exactness fixes applied at record time (g4_draw_pre, before pre_draw): the draw's constant buffers are GPU-
   copied LIVE into a private uniform ring and the deferred stream binds those slices; host-visible (dynamic) vertex /
   index buffers are bound through a private shadow DxvkBuffer whose storage is set (invalidateBuffer) to the game
   buffer's storage allocation of record time, so later DISCARD renames by the game cannot change what the deferred
   draw reads. The two streams' DxvkContext state diverges while a sequence is open, so at every switch (mask <-> other
   target) and after a replay the destination stream gets the game's complete current state re-emitted from the bind
   cache ids (g4_resync). */
#include <unordered_set>
namespace g4 {
  /* ---- switches ---- */
  static int mode_env() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHCOAL"); v = e ? std::atoi(e) : 0; if (v < 0 || v > 2) v = 0;   /* b85: default OFF (j0096/j0097 A/B: net +1.5..4 ms per frame, CHECK not exact) */ } return v; }
  static int ring_n() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHCOAL_RING"); v = e ? std::atoi(e) : 12;   /* 12: 8-12 atlas passes per frame measured (chat 3) */ if (v < 2) v = 2; if (v > 32) v = 32; } return v; }
  static bool enabled() {
    static int v = -1;
    if (v < 0) {
      v = mode_env() > 0;
      if (v && (eo::mode() == 4 || eo::drawprof())) { v = 0; std::fprintf(stderr, "[b1g4] shadow mask coalescing off: S1 mode 4 / DRAWPROF diagnostics are per-draw\n"); }
      std::fprintf(stderr, "[b1g4] shadow mask coalescing: BSI_B1_SHCOAL=%d (%s), ring of %d atlas slots (slot 0 = the game's atlas), %s\n",
                   mode_env(), mode_env() == 0 ? "off" : mode_env() == 1 ? "on" : "CHECK: every 60th frame also drawn live into a copy and compared", ring_n(), cs_on() ? "CS worker" : "direct");
    }
    return v != 0; }

  /* ---- statistics ---- */
  static long n_seq, n_draws, n_redirect, n_copies, n_clones, n_mat, n_cb_copies, n_cb_reused, n_vb_sub, n_ib_sub, n_s2open, n_cb_fallback, n_vb_fallback, n_chk_frames; static uint64_t cb_bytes;
  static long hw_ring, hw_cb, hw_cmds, n_resync, n_err;
  enum { F_TARGET = 0, F_PRESENT, F_RING, F_CBRING, F_COPY, F_COMPUTE, F_QUERY, F_RELEASE, F_UPLOAD, F_BUFWRITE, F_ATLAS, F_N };
  static const char *fname[F_N] = { "target", "present", "ring", "cbring", "copy", "compute", "query", "release", "upload", "bufwrite", "atlas" };
  static long nflush[F_N]; static uint64_t chk_total, chk_texels; static long chk_cmp;

  /* ---- atlas rings: one per shadow depth texture the game renders into (UE3 may use more than one size class) ---- */
  struct SlotView { DxvkImageViewKey key; int type; uint32_t id; };
  struct Slot { uint32_t res = 0; Rc<DxvkImage> img; std::vector<SlotView> views; };
  struct Ring {
    uint32_t res = 0; Rc<DxvkImage> img; VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t dxgi = 0;   /* the game's atlas = slot 0 */
    std::vector<Slot> slots; int cur = 0, next = 0, prev = -1; bool copy_pending = false;
  };
  constexpr int MAX_RINGS = 4;
  static std::vector<Ring> rings; static bool any_pending, atlas_seen;   /* any_pending: some ring has copy_pending (pre_draw fast path) */
  static Ring *ring_of_res(uint32_t res) { if (!res) return nullptr; for (auto &r : rings) for (auto &s : r.slots) if (s.res == res) return &r; return nullptr; }
  static Ring *ring_of_image(const DxvkImage *img) { for (auto &r : rings) for (auto &s : r.slots) if (s.img.ptr() == img) return &r; return nullptr; }
  static inline bool is_atlas_res(uint32_t res) { return !rings.empty() && ring_of_res(res) != nullptr; }
  static bool ensure_slot(Ring &r, int k) {
    if (k < 0 || k >= (int)r.slots.size()) return false;
    if (r.slots[k].img != nullptr) return true;
    try {
      DxvkImageCreateInfo ii = r.img->info(); ii.debugName = "B1 g4 atlas ring";
      static VkFormat vf[2]; vf[0] = r.fmt; vf[1] = srgb_of(r.fmt);
      if (ii.viewFormatCount) { ii.viewFormatCount = 2; ii.viewFormats = vf; }   /* the game image's list lives on a dead stack frame */
      ii.sharing = { }; ii.shared = VK_FALSE;
      Rc<DxvkImage> img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      { G4Live l; emit([img] (DxvkContext* c) { c->initImage(img, VK_IMAGE_LAYOUT_UNDEFINED); }); }
      Obj o; o.kind = 2; o.img = img; o.fmt = r.fmt; o.dxgi = r.dxgi; o.wmask = ~0u;
      uint32_t id = new_id(std::move(o));
      r.slots[k].res = id; r.slots[k].img = img;
      return true;
    } catch (const DxvkError& e) { std::fprintf(stderr, "[b1g4] ring slot %d: image creation failed (%s); coalescing continues with fewer slots\n", k, e.message().c_str()); return false; }
  }
  /* a view of slot k with the same key as one of the game's views of the atlas; type 1 SRV, 3 DSV */
  static uint32_t slot_view(Ring &r, int k, const DxvkImageViewKey &key, int type) {
    if (!ensure_slot(r, k)) return 0;
    Slot &s = r.slots[k];
    for (auto &v : s.views) if (v.type == type && v.key.eq(key)) return v.id;
    View v; v.type = type; v.res = s.res;
    try { v.iv = s.img->createView(key); } catch (const DxvkError& e) { if (n_err++ < 8) std::fprintf(stderr, "[b1g4] ring view failed: %s\n", e.message().c_str()); return 0; }
    uint32_t id = g_views.add(std::move(v)); s.views.push_back({ key, type, id });
    return id; }
  static Ring *ring_add(uint32_t res, Obj *o) {
    if ((int)rings.size() >= MAX_RINGS) { static int nl; if (nl++ < 4) std::fprintf(stderr, "[b1g4] more than %d shadow depth textures: res %u is rendered live (not coalesced)\n", MAX_RINGS, res); return nullptr; }
    rings.emplace_back(); Ring &r = rings.back();
    r.res = res; r.img = o->img; r.fmt = o->fmt; r.dxgi = o->dxgi; r.slots.resize(ring_n()); r.slots[0].res = res; r.slots[0].img = o->img;
    VkExtent3D e = o->img->mipLevelExtent(0);
    std::fprintf(stderr, "[b1g4] shadow atlas %zu: res %u %ux%u fmt %d, %d ring slots\n", rings.size(), res, e.width, e.height, (int)o->fmt, ring_n());
    return &r; }
  static void update_pending() { any_pending = false; for (auto &r : rings) any_pending |= r.copy_pending; }
  /* the game's atlas res is gone (released / re-created): free the ring's private images and view ids */
  static void ring_drop(uint32_t res) {
    for (size_t i = 0; i < rings.size(); i++) if (rings[i].res == res) {
      Ring &r = rings[i];
      for (size_t k = 1; k < r.slots.size(); k++) { Slot &sl = r.slots[k];
        for (auto &v : sl.views) { BindCache::forget(&g_bc.srv[0][0], 6 * 128, v.id); s2_view_released(v.id); g_views.del(v.id); }
        if (sl.res) { b1eo_hook_released(sl.res); s2_on_release(sl.res); g_objs[sl.res - 1] = Obj(); g_freeids.push_back(sl.res); g_nlive--; } }
      for (auto &v : r.slots[0].views) { BindCache::forget(&g_bc.srv[0][0], 6 * 128, v.id); s2_view_released(v.id); g_views.del(v.id); }
      rings.erase(rings.begin() + i); update_pending(); return; } }

  /* S1: slot dst gets slot src's block maps and dirty rect (its content was just copied from src) */
  static void clone_maps(Ring &r, int dst, int src) {
    if (eo::atlases.empty()) return;
    eo::Atlas *d = eo::find(r.slots[dst].res), *s = eo::find(r.slots[src].res);
    if (!d) return;
    if (!s || s->l1 == nullptr || d->l1 == nullptr || s->bw != d->bw || s->bh != d->bh) { eo::mark_all(*d); return; }
    VkImageSubresourceLayers l = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u }; VkExtent3D e = s->l1->mipLevelExtent(0);
    G4Live lv;
    emit([d1 = d->l1, m1 = s->l1, d2 = d->l2, m2 = s->l2, dg = d->glob, sg = s->glob, l, e] (DxvkContext* c) {
      c->copyImage(d1, l, VkOffset3D{ 0, 0, 0 }, m1, l, VkOffset3D{ 0, 0, 0 }, e);
      c->copyImage(d2, l, VkOffset3D{ 0, 0, 0 }, m2, l, VkOffset3D{ 0, 0, 0 }, e);
      c->copyBuffer(dg, 0, sg, 0, 16); });
    d->x0 = s->x0; d->y0 = s->y0; d->x1 = s->x1; d->y1 = s->y1;   /* what src still owes gets rebuilt on dst too */
    n_clones++; }
  /* the pending whole-atlas copy into the current slot (a redirected pass that did not clear the atlas first) */
  static void finish_copy(Ring &r) {
    if (!r.copy_pending) return; r.copy_pending = false; update_pending();
    if (r.prev < 0 || r.prev == r.cur || r.prev >= (int)r.slots.size() || r.slots[r.prev].img == nullptr || r.slots[r.cur].img == nullptr) return;
    VkImageSubresourceLayers l = { lookupFormatInfo(r.fmt)->aspectMask, 0u, 0u, 1u }; VkExtent3D e = r.img->mipLevelExtent(0);
    { G4Live lv; emit([di = r.slots[r.cur].img, si = r.slots[r.prev].img, l, e] (DxvkContext* c) { c->copyImage(di, l, VkOffset3D{ 0, 0, 0 }, si, l, VkOffset3D{ 0, 0, 0 }, e); }); }
    n_copies++; clone_maps(r, r.cur, r.prev); }
  static void finish_all_copies() { if (any_pending) for (auto &r : rings) finish_copy(r); }
  /* the game wants the atlas image itself (copy / upload / readback): bring the current slot's content into it */
  static void materialise(Ring &r) {
    if (r.cur == 0) return;
    finish_copy(r);
    VkImageSubresourceLayers l = { lookupFormatInfo(r.fmt)->aspectMask, 0u, 0u, 1u }; VkExtent3D e = r.img->mipLevelExtent(0);
    { G4Live lv; emit([di = r.slots[0].img, si = r.slots[r.cur].img, l, e] (DxvkContext* c) { c->copyImage(di, l, VkOffset3D{ 0, 0, 0 }, si, l, VkOffset3D{ 0, 0, 0 }, e); }); }
    clone_maps(r, 0, r.cur); r.prev = r.cur; r.cur = 0; n_mat++; }

  /* ---- the open sequence ---- */
  static bool seq_open; static uint32_t mask_res, mask_ds_res; static DxvkRenderTargets mask_rts; static uint32_t first_ps = BC_UNK, first_vs; static long seq_draws;
  static std::unordered_set<uint32_t> referenced;   /* device-local VB/IB ids a recorded draw reads (a write to one flushes) */
  static std::vector<uint32_t> gens;                 /* buffer id -> content generation (any write bumps it) */
  static inline uint32_t gen_of(uint32_t id) { return id < gens.size() ? gens[id] : 0; }

  /* constant-buffer snapshots: 256 KB device-local uniform chunks, bump-allocated, reset at every flush */
  constexpr VkDeviceSize CB_CHUNK = 256u << 10; constexpr size_t CB_MAX = 64;   /* 16 MB */
  struct CbChunk { Rc<DxvkBuffer> buf; VkDeviceSize used = 0; };
  static std::vector<CbChunk> cbc; static size_t cbi; static uint64_t cb_epoch = 1;
  struct CbSnap { uint32_t gen; uint64_t epoch; VkDeviceSize size; DxvkBufferSlice sl; };
  static std::unordered_map<uint32_t, CbSnap> cbsnap;
  static VkDeviceSize cb_align() { static VkDeviceSize a; if (!a) { a = g_dev->properties().core.properties.limits.minUniformBufferOffsetAlignment; if (a < 64) a = 64; } return a; }
  static Rc<DxvkBuffer> cb_chunk() {
    DxvkBufferCreateInfo bi = { }; bi.size = CB_CHUNK; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT
              | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 g4 constant snapshots";
    return g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); }
  static void cb_reset() { cb_epoch++; cbi = 0; for (auto &c : cbc) c.used = 0; }
  static bool cb_low() { return cbc.size() >= CB_MAX && cbi + 1 >= cbc.size() && CB_CHUNK - cbc[cbi].used < (512u << 10); }
  /* the CB's contents as of now, in the ring (one GPU copy per written generation); empty slice = ring full */
  static DxvkBufferSlice cb_snapshot(uint32_t id, Obj &o) {
    VkDeviceSize size = o.buf->info().size; uint32_t gen = gen_of(id);
    auto it = cbsnap.find(id);
    if (it != cbsnap.end() && it->second.gen == gen && it->second.epoch == cb_epoch && it->second.size == size) { n_cb_reused++; return it->second.sl; }
    VkDeviceSize need = (size + cb_align() - 1) / cb_align() * cb_align();
    while (cbi < cbc.size() && cbc[cbi].used + need > CB_CHUNK) cbi++;
    if (cbi >= cbc.size()) { if (cbc.size() >= CB_MAX || need > CB_CHUNK) return DxvkBufferSlice();
      try { cbc.push_back({ cb_chunk(), 0 }); } catch (const DxvkError& e) { if (n_err++ < 8) std::fprintf(stderr, "[b1g4] CB snapshot chunk failed: %s\n", e.message().c_str()); return DxvkBufferSlice(); }
      cbi = cbc.size() - 1; }
    CbChunk &ch = cbc[cbi]; VkDeviceSize off = ch.used; ch.used += need;
    { G4Live lv; emit([db = ch.buf, off, sb = o.buf, size] (DxvkContext* c) { c->copyBuffer(db, off, sb, 0, size); }); }
    n_cb_copies++; cb_bytes += size; if ((long)cbi + 1 > hw_cb) hw_cb = (long)cbi + 1;
    DxvkBufferSlice sl(ch.buf, off, size); cbsnap[id] = { gen, cb_epoch, size, sl };
    return sl; }

  /* dynamic (host-visible) vertex / index buffers: a shadow DxvkBuffer per game buffer id; the deferred stream points it at
     the storage allocation the game buffer had when the draw was recorded */
  struct Shadow { Rc<DxvkBuffer> buf; VkDeviceSize size; };
  static std::unordered_map<uint32_t, Shadow> shadows;
  static Rc<DxvkBuffer> shadow_of(uint32_t id, Obj &o) {
    auto it = shadows.find(id);
    if (it != shadows.end() && it->second.size == o.buf->info().size) return it->second.buf;
    DxvkBufferCreateInfo bi = o.buf->info(); bi.debugName = "B1 g4 dynamic shadow";
    Rc<DxvkBuffer> b;
    try { b = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    catch (const DxvkError& e) { if (n_err++ < 8) std::fprintf(stderr, "[b1g4] shadow buffer failed: %s\n", e.message().c_str()); return nullptr; }
    shadows[id] = { b, bi.size }; return b; }
  static inline bool hostvis(Obj &o) { return (o.buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0; }

  /* ---- CHECK mode (BSI_B1_SHCOAL=2) ---- */
  static Rc<DxvkImage> chk_col, chk_ds; static Rc<DxvkImageView> chk_rtv, chk_dsv; static Rc<DxvkImageView> real_rtv, real_dsv;
  static Rc<DxvkBuffer> chk_rb[2]; static bool chk_pending; static long chk_frame; static VkExtent3D chk_ext; static VkFormat chk_fmt;
  static bool chk_frame_now() { return mode_env() == 2 && (g_frames % 60) == 0; }
  static Rc<DxvkImage> chk_image(const Rc<DxvkImage> &like, const char *name) {
    DxvkImageCreateInfo ii = like->info(); ii.debugName = name; ii.sharing = { }; ii.shared = VK_FALSE;
    static VkFormat vf[2]; vf[0] = ii.format; vf[1] = srgb_of(ii.format); if (ii.viewFormatCount) { ii.viewFormatCount = 2; ii.viewFormats = vf; }
    ii.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ii.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT; ii.access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    Rc<DxvkImage> img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    { G4Live l; emit([img] (DxvkContext* c) { c->initImage(img, VK_IMAGE_LAYOUT_UNDEFINED); }); }
    return img; }
  static void chk_copy(const Rc<DxvkImage> &dst, const Rc<DxvkImage> &src) {
    VkImageSubresourceLayers l = { lookupFormatInfo(src->info().format)->aspectMask, 0u, 0u, 1u }; VkExtent3D e = src->mipLevelExtent(0);
    G4Live lv; emit([dst, src, l, e] (DxvkContext* c) { c->copyImage(dst, l, VkOffset3D{ 0, 0, 0 }, src, l, VkOffset3D{ 0, 0, 0 }, e); }); }
  /* at the open of a sequence in a check frame: copies of the mask target (colour + depth/stencil) for the live reference */
  static bool chk_begin() {
    Obj *mc = robj(mask_res), *md = robj(mask_ds_res); if (!mc || !md || mc->kind != 2 || md->kind != 2) return false;
    try {
      if (chk_col == nullptr || chk_col->info().extent.width != mc->img->info().extent.width || chk_col->info().extent.height != mc->img->info().extent.height || chk_col->info().format != mc->img->info().format) {
        chk_col = chk_image(mc->img, "B1 g4 check colour"); chk_rtv = nullptr; }
      if (chk_ds == nullptr || chk_ds->info().extent.width != md->img->info().extent.width || chk_ds->info().extent.height != md->img->info().extent.height || chk_ds->info().format != md->img->info().format) {
        chk_ds = chk_image(md->img, "B1 g4 check depth"); chk_dsv = nullptr; }
      real_rtv = mask_rts.color[0].view; real_dsv = mask_rts.depth.view;
      chk_rtv = chk_col->createView(real_rtv->info()); chk_dsv = chk_ds->createView(real_dsv->info());
      chk_copy(chk_col, mc->img); chk_copy(chk_ds, md->img);
      chk_ext = mc->img->mipLevelExtent(0); chk_fmt = mc->img->info().format;
      return true;
    } catch (const DxvkError& e) { std::fprintf(stderr, "[b1g4] check: setup failed: %s\n", e.message().c_str()); return false; } }
  static DxvkRenderTargets chk_rts() { DxvkRenderTargets r = { }; r.color[0].view = chk_rtv; r.depth.view = chk_dsv; return r; }
  static Rc<DxvkImageView> chk_view(const Rc<DxvkImageView> &v) {   /* the check image's view with the same key (any view of the mask's images) */
    if (v == real_rtv) return chk_rtv; if (v == real_dsv) return chk_dsv;
    if (real_rtv != nullptr && v->image() == real_rtv->image()) return chk_col->createView(v->info());
    if (real_dsv != nullptr && v->image() == real_dsv->image()) return chk_ds->createView(v->info());
    return v; }
  /* after the replay of a check frame's sequence: both colour images to host buffers; compared at a later present */
  static void chk_end() {
    g4_dup = 0;
    Obj *mc = robj(mask_res); if (!mc || mc->kind != 2 || chk_pending) return;
    try {
      VkDeviceSize sz = (VkDeviceSize)chk_ext.width * chk_ext.height * 4;
      for (int i = 0; i < 2; i++) if (chk_rb[i] == nullptr || chk_rb[i]->info().size != sz) {
        DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 g4 check readback";
        chk_rb[i] = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
      VkImageSubresourceLayers l = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      G4Live lv;
      emit([a = chk_rb[0], b = chk_rb[1], ri = mc->img, ci = chk_col, l, e = chk_ext, f = chk_fmt] (DxvkContext* c) {
        c->copyImageToBuffer(a, 0, 0, 0, f, ri, l, VkOffset3D{ 0, 0, 0 }, e); c->copyImageToBuffer(b, 0, 0, 0, f, ci, l, VkOffset3D{ 0, 0, 0 }, e); });
      chk_pending = true; chk_frame = g_frames; n_chk_frames++;
    } catch (const DxvkError& e) { std::fprintf(stderr, "[b1g4] check: readback failed: %s\n", e.message().c_str()); } }
  static void chk_compare() {   /* at a later present: wait for the GPU (diagnostic mode), compare texel by texel */
    if (!chk_pending || g_frames < chk_frame + 2) return;
    chk_pending = false;
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint32_t *a = (const uint32_t *)chk_rb[0]->mapPtr(0), *b = (const uint32_t *)chk_rb[1]->mapPtr(0);
    size_t n = (size_t)chk_ext.width * chk_ext.height, bad = 0; for (size_t i = 0; i < n; i++) bad += a[i] != b[i];
    chk_total += bad; chk_texels += n; chk_cmp++;
    std::fprintf(stderr, "[b1g4] check: frame %ld: %zu texels differ (of %zu); running total %llu of %llu in %ld checks%s\n", chk_frame, bad, n,
                 (unsigned long long)chk_total, (unsigned long long)chk_texels, chk_cmp, bad ? "  <-- NOT EXACT" : ""); }

  /* ---- state re-emission (both streams keep the game's complete state) ---- */
  struct CapSet { int p; CapSet(int v) : p(g4_cap) { g4_cap = v; } ~CapSet() { g4_cap = p; } };
  struct NoDup { int p; NoDup() : p(g4_nodup) { g4_nodup = 1; } ~NoDup() { g4_nodup = p; } };
  /* every piece of state the bind cache knows, from the game's ids, into the active stream (g4_cap set by the caller).
     atlas_target: the live stream is about to render into the atlas; SRV slots that hold the atlas get a null view there
     (the game's D3D11 runtime would have unbound them; they are re-bound, remapped to the current slot, at the next switch). */
  static void resync(bool to_def, bool atlas_target, DxvkRenderTargets rts) {
    n_resync++;
    {   /* render targets (CHECK: the live copy of the deferred stream binds the check images instead) */
      if (to_def && g4_dup) { { NoDup nd; emit([rts] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(rts), 0u); }); }
        CapSet live(0); DxvkRenderTargets cr = chk_rts(); emit([cr] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(cr), 0u); }); }
      else emit([rts] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(rts), 0u); }); }
    for (int st = 0; st < 6; st++) { uint32_t id = g_bc.sh[st]; if (id == BC_UNK) continue;
      Rc<DxvkShader> s = id ? b1dx_shader_get(id) : nullptr;
      emit([st, s = std::move(s)] (DxvkContext* c) mutable {
        switch (st) { case 0: c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(s)); break; case 1: c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); break;
                      case 2: c->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(std::move(s)); break; case 3: c->bindShader<VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT>(std::move(s)); break;
                      case 4: c->bindShader<VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT>(std::move(s)); break; default: c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(s)); break; } }); }
    eo::bound_eo = false; eo::bound_ptr = nullptr; eo::bound_map = nullptr; eo::bound_glob = nullptr;   /* S1 re-binds its twin / maps at its next draw */
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 14; i++) { uint32_t id = g_bc.cb[st][i]; if (id == BC_UNK) continue;
      Obj *o = robj(id); DxvkBufferSlice sl; if (o && o->kind == 1) sl = DxvkBufferSlice(o->buf);
      emit([sb = sbit(st), slot = sidx(st) * 16 + i, sl = std::move(sl)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, slot, std::move(sl)); }); }
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 128; i++) { uint32_t id = g_bc.srv[st][i]; if (id == BC_UNK || id == eo::kMapMarker || id == eo::kGlobMarker) continue;
      View *v = g_views.get(id); bool ok = v && v->type == 1; Rc<DxvkImageView> iv;
      if (Ring *r = ok ? ring_of_res(v->res) : nullptr) {
        if (atlas_target) iv = nullptr;
        else { uint32_t nid = slot_view(*r, r->cur, v->iv->info(), 1); if (nid) { g_bc.srv[st][i] = nid; v = g_views.get(nid); } iv = v->iv; } }
      else iv = s2_srv(st, i, id, ok ? v : nullptr, ok ? v->iv : nullptr);
      emit([sb = sbit(st), slot = sidx(st) * 128 + i, iv = std::move(iv)] (DxvkContext* c) mutable { c->bindResourceImageView(sb, slot, std::move(iv)); }); }
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 16; i++) { uint32_t id = g_bc.ss[st][i]; if (id == BC_UNK) continue;
      SS *s = g_sss.get(id); Rc<DxvkSampler> sp = s ? s->s : nullptr;
      emit([sb = sbit(st), slot = sidx(st) * 16 + i, sp = std::move(sp)] (DxvkContext* c) mutable { c->bindResourceSampler(sb, slot, std::move(sp)); }); }
    if (g_bc.il != BC_UNK) { IL *il = g_ils.get(g_bc.il);
      if (il) emit_data<DxvkVertexInput>(il->in, il->na + il->nb, [na = il->na, nb = il->nb] (DxvkContext* c, const DxvkVertexInput* in, size_t) { c->setInputLayout(na, in, nb, in + na); });
      else emit([] (DxvkContext* c) { c->setInputLayout(0, nullptr, 0, nullptr); }); }
    if (g_bc.topo != BC_UNK) { VkPrimitiveTopology vt = (VkPrimitiveTopology)g_bc.topo; emit([vt] (DxvkContext* c) { c->setInputAssemblyState(DxvkInputAssemblyState(vt, false)); }); }
    if (g_bc.rs != BC_UNK) { RS *rs = g_rss.get(g_cs.rs); emit([s = rs ? rs->s : default_rs(), b = rs ? rs->b : DxvkDepthBias()] (DxvkContext* c) { c->setRasterizerState(s); c->setDepthBias(b); }); }
    if (g_bc.bs != BC_UNK) {   /* as b1dx_c_blend */
      BS *bs = g_bss.get(g_cs.bs);
      struct BlendCmd { DxvkMultisampleState ms; DxvkBlendMode m[8]; DxvkLogicOpState lo; DxvkBlendConstants bc; } bcmd;
      DxvkMultisampleState &ms = bcmd.ms; ms = { }; ms.setSampleMask((uint16_t)g_bc.bsmask); ms.setAlphaToCoverage(bs && bs->a2c);
      for (uint32_t i = 0; i < 8; i++) { DxvkBlendMode m = { };
        if (bs) m = bs->m[i];
        else { m.setBlendEnable(false); m.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); m.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); m.setWriteMask(0xf); m.normalize(); }
        bcmd.m[i] = m; }
      if (b1vr::alpha && b1vr::bbrt) b1vr::cover(bcmd.m[0]);
      DxvkLogicOpState lo = { }; lo.setLogicOp(false, VK_LOGIC_OP_NO_OP);
      float bf[4]; std::memcpy(bf, g_bc.bf, 16);   /* g_bc.bf holds the float bits */
      bcmd.lo = bs ? bs->lo : lo; bcmd.bc = DxvkBlendConstants{ bf[0], bf[1], bf[2], bf[3] };
      emit([bcmd] (DxvkContext* c) { for (uint32_t i = 0; i < 8; i++) c->setBlendMode(i, bcmd.m[i]); c->setMultisampleState(bcmd.ms); c->setLogicOpState(bcmd.lo); c->setBlendConstants(bcmd.bc); }); }
    if (g_bc.dss != BC_UNK) { DSS *d = g_dsss.get(g_cs.dss); DxvkDepthStencilState ds = { };
      if (d) ds = d->s; else { ds.setDepthTest(true); ds.setDepthWrite(true); ds.setDepthCompareOp(VK_COMPARE_OP_LESS); ds.normalize(); }
      emit([ds, ref = g_cs.sref] (DxvkContext* c) { c->setDepthStencilState(ds); c->setStencilReference(ref & 0xffu); }); s2::lrz_mod = false; }
    for (uint32_t s = 0; s < 32; s++) { uint32_t id = g_bc.vb[s]; if (id == BC_UNK) continue;
      Obj *o = robj(id); bool ok = o && o->kind == 1 && g_bc.vbo[s] < o->buf->info().size;
      DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, g_bc.vbo[s], o->buf->info().size - g_bc.vbo[s]) : DxvkBufferSlice();
      emit([s, sl = std::move(sl), str = ok ? g_bc.vbs[s] : 0u] (DxvkContext* c) mutable { c->bindVertexBuffer(s, std::move(sl), str); }); }
    if (g_bc.ib != BC_UNK) { Obj *o = robj(g_bc.ib); bool ok = o && o->kind == 1 && g_bc.ibo < o->buf->info().size;
      DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, g_bc.ibo, o->buf->info().size - g_bc.ibo) : DxvkBufferSlice();
      emit([sl = std::move(sl), t = (VkIndexType)(g_bc.ibf == BC_UNK ? VK_INDEX_TYPE_UINT16 : g_bc.ibf)] (DxvkContext* c) mutable { c->bindIndexBuffer(std::move(sl), t); }); }
    g_cs.vpdirty = true;   /* viewports: applied by the next pre_draw of the stream */
  }

  /* ---- the sequence: open, switch, flush (= replay) ---- */
  /* (b1dx_c_rts has not stored the new targets in g_cs yet when rts() below runs, and S1's eager build needs the old g_cs.dsv:
     the new target set is passed in explicitly) */
  static void open(const DxvkRenderTargets &rts, uint32_t mres, uint32_t dres) {
    seq_open = true; n_seq++; seq_draws = 0; first_ps = BC_UNK; first_vs = 0; referenced.clear();
    mask_res = mres; mask_ds_res = dres; mask_rts = rts;
    /* s2: a pending copy into / out of the mask's images is executed now (s2_pre_draw would do it at the first mask draw,
       which would land in the deferred stream); anything else pending is executed too so no alias view can outlive its
       source across the two streams */
    if (!s2::pend.empty() || !s2::clrp.empty()) { G4Live lv;
      if (mask_res) { s2_before_write(mask_res, false); s2::clrp_set(mask_res, false); }
      if (mask_ds_res) { s2_before_write(mask_ds_res, false); s2::clrp_set(mask_ds_res, false); }
      if (!s2::pend.empty()) { n_s2open++; s2_flush_all(); } }
    if (chk_frame_now() && chk_begin()) g4_dup = 1;
  }
  static void to_deferred(const DxvkRenderTargets &rts) { g4_mode = 1; CapSet cs(1); resync(true, false, rts); }
  static void to_live(bool atlas_target) { g4_mode = 0; CapSet cs(0); resync(false, atlas_target, eo::cur_rts()); }
  static void flush(int why) {
    if (!seq_open) return;
    nflush[why]++;
    CapSet cs(0);
    /* gp: the mask pass is timed where it runs (one segment, keyed by the first deferred draw's shaders) */
    if (gp::on() && seq_draws) { gp::set_targets(mask_rts); gp::lastkey = gp::pkey; gp::curlab = gp::plab; gp::in_compute = false;
      gp::mk_sh = first_ps == BC_UNK ? 0 : first_ps; gp::mk_vs = first_vs; gp::mark(gp::curlab); gp::ndraw += seq_draws; }
    if (g4_chunk && !g4_chunk->empty()) g4_chunks.push_back(std::move(g4_chunk));
    if ((long)g4_chunks.size() > hw_cmds) hw_cmds = (long)g4_chunks.size();
    if (g_csthr) { cs_dispatch(); for (auto &ch : g4_chunks) { g_cs_seq = g_csthr->dispatchChunk(std::move(ch)); g_cs_chunks++; } }
    else for (auto &ch : g4_chunks) ch->executeAll(g_ctx.ptr());
    g4_chunks.clear();
    if (gp::on()) gp::set_targets(eo::cur_rts());
    seq_open = false; g4_mode = 0;
    bool atlas_target = false; { View *dv = g_views.get(g_cs.dsv); atlas_target = dv && dv->type == 3 && !g_cs.rtv[0] && is_atlas_res(dv->res); }
    resync(false, atlas_target, eo::cur_rts());
    cb_reset(); referenced.clear();
    if (g4_dup) chk_end();
  }
  /* from inside a deferred draw's record (ring full): replay what is recorded, then continue deferring this draw */
  static void reopen(int why) { int pc = g4_cap; g4_cap = 0; flush(why); View *rv = g_views.get(g_cs.rtv[0]), *dv = g_views.get(g_cs.dsv);
    open(eo::cur_rts(), rv ? rv->res : 0, dv ? dv->res : 0); to_deferred(eo::cur_rts()); g4_cap = pc; }

  /* ---- target classification (b1dx_c_rts) ---- */
  enum { K_OTHER = 0, K_MASK, K_ATLAS };
  static int classify(uint32_t n, const uint32_t *rtvs, uint32_t dsv, Obj **atlas_obj) {
    int nc = 0; View *r0 = nullptr;
    for (uint32_t i = 0; i < n && i < 8; i++) { View *v = g_views.get(rtvs[i]); if (v && v->type == 2) { if (!nc) r0 = v; nc++; } }
    View *dv = g_views.get(dsv); if (!dv || dv->type != 3) return K_OTHER;
    Obj *d = robj(dv->res); if (!d || d->kind != 2) return K_OTHER;
    if (!nc) {   /* depth only: the shadow atlas = a square D32 / D16 texture >= 1024, one mip, one layer, sampled */
      const DxvkImageCreateInfo &ii = d->img->info();
      if (ii.type != VK_IMAGE_TYPE_2D || ii.extent.width != ii.extent.height || ii.extent.width < 1024 || ii.mipLevels != 1 || ii.numLayers != 1) return K_OTHER;
      if (ii.format != VK_FORMAT_D32_SFLOAT && ii.format != VK_FORMAT_D16_UNORM) return K_OTHER;
      if (!(ii.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return K_OTHER;
      *atlas_obj = d; return K_ATLAS; }
    if (nc != 1 || !r0 || r0->res == g_bb) return K_OTHER;
    Obj *c = robj(r0->res), *bb = robj(g_bb); if (!c || c->kind != 2 || !bb || bb->kind != 2) return K_OTHER;
    VkExtent3D ce = c->img->mipLevelExtent(0), be = bb->img->mipLevelExtent(0);
    if (ce.width != be.width || ce.height != be.height) return K_OTHER;
    VkFormat f = c->img->info().format;
    if (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB) return K_OTHER;
    return K_MASK; }
  /* the atlas SRV slots of the live stream, after an atlas pass: the remapped views back (they were null during the pass) */
  static void live_atlas_srvs_back() {
    if (rings.empty()) return;
    CapSet cs(0);
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 128; i++) { uint32_t id = g_bc.srv[st][i]; if (!id || id == BC_UNK || id == eo::kMapMarker || id == eo::kGlobMarker) continue;
      View *v = g_views.get(id); Ring *r = v && v->type == 1 ? ring_of_res(v->res) : nullptr; if (!r) continue;
      uint32_t nid = slot_view(*r, r->cur, v->iv->info(), 1); if (nid) { g_bc.srv[st][i] = nid; v = g_views.get(nid); }
      emit([sb = sbit(st), slot = sidx(st) * 128 + i, iv = v->iv] (DxvkContext* c) mutable { c->bindResourceImageView(sb, slot, std::move(iv)); }); } }
  static void live_atlas_srvs_null() {
    CapSet cs(0);
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 128; i++) { uint32_t id = g_bc.srv[st][i]; if (!id || id == BC_UNK || id == eo::kMapMarker || id == eo::kGlobMarker) continue;
      View *v = g_views.get(id); if (!v || v->type != 1 || !is_atlas_res(v->res)) continue;
      emit([sb = sbit(st), slot = sidx(st) * 128 + i] (DxvkContext* c) { c->bindResourceImageView(sb, slot, nullptr); }); } }
  static bool live_nulled;

  static void rts(uint32_t n, const uint32_t *rtvs, uint32_t *dsv) {
    Obj *ao = nullptr; int k = classify(n, rtvs, *dsv, &ao);
    if (k == K_ATLAS) {
      View *dv = g_views.get(*dsv);
      Ring *r = nullptr; for (auto &x : rings) if (x.res == dv->res) r = &x;
      if (r && r->img != ao->img) { if (seq_open) flush(F_ATLAS); ring_drop(dv->res); r = nullptr; }   /* re-created under the same id */
      if (!r) r = ring_add(dv->res, ao);
      finish_all_copies();   /* a previous redirected pass had no draw and no clear */
      if (seq_open && g4_mode == 1) to_live(true);
      if (!r) { if (seq_open) flush(F_ATLAS); return; }   /* too many atlases: this one is rendered live, outside any sequence */
      if (r->next >= (int)r->slots.size()) { if (seq_open) flush(F_RING); r->next = 0; }
      r->prev = r->cur; r->cur = r->next++; r->copy_pending = true; update_pending(); atlas_seen = true; n_redirect += r->cur != 0; if (r->cur + 1 > hw_ring) hw_ring = r->cur + 1;
      if (r->cur != 0) { uint32_t nid = slot_view(*r, r->cur, dv->iv->info(), 3); if (nid) *dsv = nid; else { if (seq_open) flush(F_RING); r->cur = 0; } }   /* no slot image: the game's atlas, after replaying what reads slot 0 */
      if (!live_nulled) { live_atlas_srvs_null(); live_nulled = true; }
      return; }
    finish_all_copies();
    if (k == K_MASK && atlas_seen && !rings.empty()) {
      DxvkRenderTargets nrts = { }; uint32_t mres = 0;
      for (uint32_t i = 0; i < n && i < 8; i++) { View *v = g_views.get(rtvs[i]); if (v && v->type == 2) { nrts.color[i].view = v->iv; if (!mres) mres = v->res; } }
      View *dv = g_views.get(*dsv); nrts.depth.view = dv->iv;
      if (!seq_open) open(nrts, mres, dv->res);
      if (g4_mode == 0) to_deferred(nrts);
      live_nulled = false;   /* the deferred resync re-bound them (remapped) */
      if (g4_dup) { CapSet live(0); DxvkRenderTargets cr = chk_rts(); emit([cr] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(cr), 0u); }); }
      return; }
    if (seq_open) flush(F_TARGET);
    if (live_nulled) { live_atlas_srvs_back(); live_nulled = false; }
  }

  /* ---- per deferred draw (b1dx_c_draw*, before pre_draw) ---- */
  static void draw_pre() {
    n_draws++; seq_draws++;
    if (first_ps == BC_UNK) { first_ps = g_m79_ps; first_vs = g_m79_vs; }
    if (cb_low()) reopen(F_CBRING);
    NoDup nd;
    for (int st = 0; st < 5; st++) for (uint32_t i = 0; i < 14; i++) { uint32_t id = g_bc.cb[st][i]; if (!id || id == BC_UNK) continue;
      Obj *o = robj(id); if (!o || o->kind != 1) continue;
      DxvkBufferSlice sl = cb_snapshot(id, *o); if (!sl.defined()) { n_cb_fallback++; continue; }
      emit([sb = sbit(st), slot = sidx(st) * 16 + i, sl = std::move(sl)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, slot, std::move(sl)); }); }
    for (uint32_t s = 0; s < 32; s++) { uint32_t id = g_bc.vb[s]; if (!id || id == BC_UNK) continue;
      Obj *o = robj(id); if (!o || o->kind != 1) continue;
      if (!hostvis(*o)) { referenced.insert(id); continue; }
      Rc<DxvkBuffer> sh = shadow_of(id, *o); if (sh == nullptr) { n_vb_fallback++; continue; }
      Rc<DxvkResourceAllocation> a = o->dyn != nullptr ? o->dyn : o->buf->storage();
      VkDeviceSize sz = o->buf->info().size, off = g_bc.vbo[s]; if (off >= sz) continue;
      emit([sh, a, s, off, sz, str = g_bc.vbs[s]] (DxvkContext* c) mutable {
        if (sh->storage() != a) c->invalidateBuffer(sh, Rc<DxvkResourceAllocation>(a));
        c->bindVertexBuffer(s, DxvkBufferSlice(sh, off, sz - off), str); });
      n_vb_sub++; }
    if (g_bc.ib && g_bc.ib != BC_UNK) { uint32_t id = g_bc.ib; Obj *o = robj(id);
      if (o && o->kind == 1) {
        if (!hostvis(*o)) referenced.insert(id);
        else if (Rc<DxvkBuffer> sh = shadow_of(id, *o); sh == nullptr) n_vb_fallback++;
        else { Rc<DxvkResourceAllocation> a = o->dyn != nullptr ? o->dyn : o->buf->storage();
          VkDeviceSize sz = o->buf->info().size, off = g_bc.ibo; VkIndexType t = (VkIndexType)(g_bc.ibf == BC_UNK ? VK_INDEX_TYPE_UINT16 : g_bc.ibf);
          if (off < sz) { emit([sh, a, off, sz, t] (DxvkContext* c) mutable {
              if (sh->storage() != a) c->invalidateBuffer(sh, Rc<DxvkResourceAllocation>(a));
              c->bindIndexBuffer(DxvkBufferSlice(sh, off, sz - off), t); }); n_ib_sub++; } } } }
  }

  static void report() {
    std::string fl; char b[64]; for (int i = 0; i < F_N; i++) if (nflush[i]) { std::snprintf(b, sizeof b, " %s %ld", fname[i], nflush[i]); fl += b; }
    std::fprintf(stderr, "[b1g4] stats: sequences %ld, draws deferred %ld, atlas passes redirected %ld (whole-atlas copies %ld, map clones %ld, materialised %ld), CB snapshots %ld (%.1f MB, reused %ld, NOT snapshotted %ld), VB/IB substitutions %ld/%ld (NOT substituted %ld), resyncs %ld, s2 flushed at open %ld, errors %ld; flushes:%s; high water: ring %ld slots, CB %ld chunks, %ld deferred chunks; check: %ld frames, %llu texels differ of %llu\n",
                 n_seq, n_draws, n_redirect, n_copies, n_clones, n_mat, n_cb_copies, cb_bytes / 1048576.0, n_cb_reused, n_cb_fallback, n_vb_sub, n_ib_sub, n_vb_fallback, n_resync, n_s2open, n_err, fl.empty() ? " none" : fl.c_str(),
                 hw_ring, hw_cb, hw_cmds, n_chk_frames, (unsigned long long)chk_total, (unsigned long long)chk_texels); }
}

/* ---- hooks (rhi_b1_dxvk.cpp; all under g_rmu) ---- */
static void g4_rts(uint32_t n, const uint32_t *rtvs, uint32_t *dsv) { if (g4::enabled()) g4::rts(n, rtvs, dsv); }
/* a clear while the mask is bound: deferred (CHECK: the live copy clears the check images); a full atlas clear cancels the slot copy */
static bool g4_clear(const Rc<DxvkImageView> &v, VkImageAspectFlags asp, VkClearValue cv) {
  g4::Ring *r = g4::rings.empty() ? nullptr : g4::ring_of_image(v->image());
  if (r && r->copy_pending && (asp & VK_IMAGE_ASPECT_DEPTH_BIT) && v->image() == r->slots[r->cur].img.ptr()) { r->copy_pending = false; g4::update_pending(); }   /* a full clear: no copy needed */
  if (!g4_cap || g4_live) return false;
  if (r) { G4Live l; DxvkAttachment a = { }; a.view = v; emit([a, asp, cv] (DxvkContext* c) { c->clearRenderTarget(a, asp, cv, 0u); }); return true; }   /* atlas work is live */
  { g4::NoDup nd; DxvkAttachment a = { }; a.view = v; emit([a, asp, cv] (DxvkContext* c) { c->clearRenderTarget(a, asp, cv, 0u); }); }
  if (g4_dup) { g4::CapSet live(0); DxvkAttachment a = { }; a.view = g4::chk_view(v); emit([a, asp, cv] (DxvkContext* c) { c->clearRenderTarget(a, asp, cv, 0u); }); }
  return true; }
static uint32_t g4_remap_dsv(uint32_t id) {
  if (g4::rings.empty()) return id;
  View *v = g_views.get(id); g4::Ring *r = v && v->type == 3 ? g4::ring_of_res(v->res) : nullptr; if (!r || (r->cur == 0 && v->res == r->res)) return id;
  uint32_t nid = g4::slot_view(*r, r->cur, v->iv->info(), 3); return nid ? nid : id; }
static const uint32_t *g4_remap_srvs(const uint32_t *ids, uint32_t n, uint32_t *tmp) {
  if (g4::rings.empty() || !n) return ids;
  bool any = false;
  for (uint32_t i = 0; i < n; i++) { View *v = g_views.get(ids[i]); g4::Ring *r = v && v->type == 1 ? g4::ring_of_res(v->res) : nullptr; if (!r) continue;
    if (r->cur == 0 && v->res == r->res) continue;
    uint32_t nid = g4::slot_view(*r, r->cur, v->iv->info(), 1); if (!nid || nid == ids[i]) continue;
    if (!any) { std::memcpy(tmp, ids, n * sizeof *ids); any = true; } tmp[i] = nid; }
  return any ? tmp : ids; }
static void g4_draw_pre() { g4::draw_pre(); }
static void g4_pre_draw_live() { if (g4::any_pending) { View *v = g_views.get(g_cs.dsv); g4::Ring *r = v && v->type == 3 ? g4::ring_of_image(v->iv->image()) : nullptr; if (r && r->copy_pending && v->iv->image() == r->slots[r->cur].img.ptr()) g4::finish_copy(*r); } }
/* a buffer's contents change: how 0 = NO_OVERWRITE write into the current storage, 1 = DISCARD rename (new storage), 2 = in place (GPU) */
static void g4_wrote(uint32_t id, int how) {
  if (g_c5b_gate && !g4::enabled()) return;   /* c5b141 HOOKGATE: gens / seq_open are read only while g4 is enabled (latched once) */
  if (id >= g4::gens.size()) g4::gens.resize(id + 256, 0); g4::gens[id]++;
  if (how == 2 && g4::seq_open && (g4::referenced.count(id) || g4::shadows.count(id))) g4::flush(g4::F_BUFWRITE); }
static void g4_copy_image(uint32_t dst, uint32_t src) {
  if (g4::seq_open) g4::flush(g4::F_COPY);
  if (g4::rings.empty()) return;
  if (g4::Ring *r = g4::ring_of_res(dst)) g4::materialise(*r);
  if (g4::Ring *r = g4::ring_of_res(src)) g4::materialise(*r); }
static void g4_flush_if_open(const char *why) {
  if (!g4::seq_open) return;
  int w = g4::F_COMPUTE; if (!std::strcmp(why, "query")) w = g4::F_QUERY;
  g4::flush(w); }
static void g4_released(uint32_t id) {
  g4::shadows.erase(id); g4::cbsnap.erase(id); if (id < g4::gens.size()) g4::gens[id]++;   /* (buffers: the id may be reused) */
  if (g4::rings.empty()) return;
  if (g4::seq_open && (id == g4::mask_res || id == g4::mask_ds_res || g4::is_atlas_res(id))) g4::flush(g4::F_RELEASE);
  g4::ring_drop(id); g4::update_pending(); }
static void g4_touch_image(uint32_t id) {
  if (g4::rings.empty()) return;
  if (g4::seq_open && (id == g4::mask_res || id == g4::mask_ds_res || g4::is_atlas_res(id))) g4::flush(g4::F_UPLOAD);
  if (g4::Ring *r = g4::ring_of_res(id)) g4::materialise(*r); }
static void g4_present() {
  if (!g4::enabled()) return;
  if (g4::seq_open) g4::flush(g4::F_PRESENT);
  g4::finish_all_copies();
  if (g4::live_nulled) { g4::live_atlas_srvs_back(); g4::live_nulled = false; }
  g4::atlas_seen = false; for (auto &r : g4::rings) r.next = 0;
  if (g4::chk_pending) g4::chk_compare();
  static long np; if (++np % 600 == 0) g4::report(); }
