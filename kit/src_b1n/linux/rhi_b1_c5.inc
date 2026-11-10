/* rhi_b1_c5.inc -- c5 (chat 5, CPU): raw-pointer command captures + CS-sequence graveyard. Included by rhi_b1_dxvk.cpp at the
   end of the file; OFF unless BSI_B1_RAWCAP=1 (read once in b1dx_init via c5_init). RAWCAP_REPORT.md has the design.

   Problem (simpleperf on the Quest): every recorded command captured Rc<> copies of table-owned objects (shaders, image
   views, buffers, samplers, dynamic storage): the render thread incremented the refcount, the dxvk-cs worker decremented it
   when the chunk died, ~10k inc/dec pairs a frame bouncing the objects' cache lines between two cores.

   With RAWCAP the hot emits capture RAW POINTERS; the Rc the DXVK context API needs is built inside the lambda, on the
   worker (Rc<T>(ptr) increments there; the context stores it or the command list tracks it, and releases on the worker).
   Lifetime: every raw-captured object is owned by a table in rhi_b1_dxvk.cpp (g_objs[].buf/img/rtv/dsv/dyn, g_views[].iv,
   g_sss[].s, rhi_b1_shader.cpp's g_shaders). A release on the producer no longer drops that owning Rc: it is moved into the
   graveyard below, tagged with the sequence number of the chunk being recorded (the last chunk that can hold a raw pointer
   to it), and dropped only once the worker's lastSequenceNumber() >= tag, i.e. after that chunk EXECUTED (DxvkCsThread
   stores the sequence number after executeAll(); by then the DXVK context / command list own their own references).
   Pruned at present and when the graveyard grows past c5::kPrune entries.

   Not raw (unchanged Rc captures): emits recorded into the g4 deferred stream (its chunks are dispatched later with
   sequence numbers the tag cannot know: c5_raw() is false while g4_cap && !g4_live), the s2 alias views (owned by the s2
   alias cache, not a table), staging-ring buffers (owned by nothing but the lambda once the ring moves on), the s2
   pending-copy images, the AO-switch alternates, everything in the other .inc files, and the low-frequency creation /
   readback / query emits. CS off (BSI_B1_CS=0): emit() runs the lambda at once on the producer, so raw captures are
   trivially valid and the graveyard frees immediately. BSI_B1_CS=2: as 1 (the present's cs_sync then empties it). */
#include <deque>

namespace c5 {
  struct Grave {   /* one released table entry: whichever members are non-null */
    uint64_t seq;
    Rc<DxvkBuffer> buf; Rc<DxvkImage> img; Rc<DxvkImageView> v0, v1; Rc<DxvkSampler> smp; Rc<DxvkShader> sh; Rc<DxvkResourceAllocation> dyn;
  };
  static std::deque<Grave> yard;          /* tags are nondecreasing (see tag()), so pruning pops from the front */
  static std::vector<DxvkShader *> shptr;  /* shader id -> raw pointer, filled from b1dx_shader_get, cleared at b1dx_shader_release */
  static long buried, freed, n_raw, n_fallback, presents; static size_t peak; static uint64_t max_age;
  constexpr size_t kPrune = 2048;
  /* the sequence number of the chunk being recorded = the last chunk that can reference an object released now. The chunk
     g_chunk gets g_cs_seq + 1 at its dispatch (only cs_dispatch and the g4 flush dispatch, both update g_cs_seq); an empty
     g_chunk holds nothing, so g_cs_seq (already dispatched) suffices and the entry can go at the next prune. */
  static inline uint64_t tag() { return (g_chunk && !g_chunk->empty()) ? g_cs_seq + 1 : g_cs_seq; }
  static void prune() {
    if (yard.empty() || !g_csthr) { yard.clear(); return; }
    uint64_t last = g_csthr->lastSequenceNumber();
    while (!yard.empty() && yard.front().seq <= last) {
      uint64_t age = g_cs_seq - yard.front().seq; if (age > max_age) max_age = age;
      yard.pop_front(); freed++;
    }
  }
  static inline void push(Grave &&g) {
    if (!g_csthr) return;   /* CS off: the lambdas already ran; the Rc members die here */
    g.seq = tag(); yard.push_back(std::move(g)); buried++;
    if (yard.size() > peak) peak = yard.size();
    if (yard.size() >= kPrune) prune();
  }
}
/* ---- switches / lifecycle ---- */
static void c5_init() {   /* b1dx_init, after the CS setup */
  const char *e = std::getenv("BSI_B1_RAWCAP"); g_rawcap = e && std::atoi(e) > 0;
  if (g_rawcap) std::fprintf(stderr, "[b1rc5] RAWCAP on: hot commands capture raw pointers, released table objects wait in a CS-sequence graveyard (CS %s)\n",
                             g_csthr ? (g_cs_mode == 2 ? "worker + full sync" : "worker") : "off: freed at once");
}
/* true when the emit about to be recorded goes into g_chunk (or runs at once): raw captures are safe there */
static inline bool c5_raw() {
  if (!g_rawcap) return false;
  if (unlikely(g4_cap && !g4_live)) { c5::n_fallback++; return false; }   /* g4 deferred stream: dispatched later, Rc captures */
  c5::n_raw++; return true;
}
/* called at present (b1dx_present_inner / b1dx_end_frame, g_rmu held): prune + the stats line every 600 presents */
static void c5_present() {
  c5::prune();
  if ((++c5::presents % 600) == 0)
    std::fprintf(stderr, "[b1rc5] rawcap: graveyard %zu entries (peak %zu), buried %ld, freed %ld, max age %llu chunks, raw emits %ld, g4 fallbacks %ld, shader ptr cache %zu\n",
                 c5::yard.size(), c5::peak, c5::buried, c5::freed, (unsigned long long)c5::max_age, c5::n_raw, c5::n_fallback, c5::shptr.size());
}
/* ---- burial (producer, g_rmu held): called by the release paths BEFORE the table drops its Rc ---- */
static void c5_bury_obj(Obj &o) {   /* b1dx_release, b1dx_backbuffer (old back buffer) */
  if (o.buf == nullptr && o.img == nullptr && o.dyn == nullptr && o.rtv == nullptr && o.dsv == nullptr) return;
  c5::Grave g; g.buf = std::move(o.buf); g.img = std::move(o.img); g.v0 = std::move(o.rtv); g.v1 = std::move(o.dsv); g.dyn = std::move(o.dyn);
  c5::push(std::move(g));
}
static void c5_bury(Rc<DxvkImageView> &&v) { if (v != nullptr) { c5::Grave g; g.v0 = std::move(v); c5::push(std::move(g)); } }   /* b1dx_cobj_release kind 3 */
static void c5_bury(Rc<DxvkSampler> &&s)   { if (s != nullptr) { c5::Grave g; g.smp = std::move(s); c5::push(std::move(g)); } }  /* b1dx_cobj_release kind 8 */
/* the storage a DISCARD rename replaces: a queued invalidateBuffer may still hold it raw */
static void c5_bury_dyn(Obj &o) { if (o.dyn != nullptr) { c5::Grave g; g.dyn = std::move(o.dyn); c5::push(std::move(g)); } }
/* shaders: the table is rhi_b1_shader.cpp's (g_smu, lock order g_rmu -> g_smu as b1dx_c_shader); the pointer cache below makes
   the per-draw bind free of b1dx_shader_get's Rc copy. An id maps to one object from its creation to its release
   (b1dx_shader_release); the twins the .inc files release with b1dx_shader_release_raw never pass through b1dx_c_shader. */
static DxvkShader *c5_shader_ptr(uint32_t id) {
  if (id < c5::shptr.size() && c5::shptr[id]) return c5::shptr[id];
  Rc<DxvkShader> s = b1dx_shader_get(id); DxvkShader *p = s.ptr();
  if (p) { if (id >= c5::shptr.size()) c5::shptr.resize(id + 256, nullptr); c5::shptr[id] = p; }   /* nulls are not cached: the id may be created later */
  return p;
}
static void c5_shader_released(uint32_t id) {   /* b1dx_shader_release, before b1dx_shader_release_raw */
  if (id < c5::shptr.size()) c5::shptr[id] = nullptr;
  Rc<DxvkShader> s = b1dx_shader_get(id);
  if (s != nullptr) { c5::Grave g; g.sh = std::move(s); c5::push(std::move(g)); }
}
/* ---- raw-capture emits (the Rc is built on the worker, inside the lambda) ---- */
static void c5_bind_shader(int st, DxvkShader *p) {
  emit([st, p] (DxvkContext* c) {
    Rc<DxvkShader> s(p);
    switch (st) {
      case 0: c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(s)); break;
      case 1: c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); break;
      case 2: c->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(std::move(s)); break;
      case 3: c->bindShader<VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT>(std::move(s)); break;
      case 4: c->bindShader<VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT>(std::move(s)); break;
      case 5: c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(s)); break;
      default: break;
    } });
}
static void c5_bind_cb(VkShaderStageFlags sb, uint32_t slot, DxvkBuffer *b) {   /* whole buffer, as DxvkBufferSlice(o->buf) */
  emit([sb, slot, b] (DxvkContext* c) { c->bindUniformBuffer(sb, slot, b ? DxvkBufferSlice(Rc<DxvkBuffer>(b)) : DxvkBufferSlice()); });
}
static void c5_bind_srv(VkShaderStageFlags sb, uint32_t slot, DxvkImageView *v) {
  emit([sb, slot, v] (DxvkContext* c) { c->bindResourceImageView(sb, slot, Rc<DxvkImageView>(v)); });
}
static void c5_bind_sampler(VkShaderStageFlags sb, uint32_t slot, DxvkSampler *s) {
  emit([sb, slot, s] (DxvkContext* c) { c->bindResourceSampler(sb, slot, Rc<DxvkSampler>(s)); });
}
static void c5_bind_vb(uint32_t slot, DxvkBuffer *b, VkDeviceSize off, VkDeviceSize len, uint32_t stride) {
  emit([slot, b, off, len, stride] (DxvkContext* c) { c->bindVertexBuffer(slot, b ? DxvkBufferSlice(Rc<DxvkBuffer>(b), off, len) : DxvkBufferSlice(), stride); });
}
static void c5_bind_ib(DxvkBuffer *b, VkDeviceSize off, VkDeviceSize len, VkIndexType t) {
  emit([b, off, len, t] (DxvkContext* c) { c->bindIndexBuffer(b ? DxvkBufferSlice(Rc<DxvkBuffer>(b), off, len) : DxvkBufferSlice(), t); });
}
static void c5_bind_uav(uint32_t slot, uint32_t ctr, DxvkImageView *v) {
  emit([slot, ctr, v] (DxvkContext* c) { c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, slot, Rc<DxvkImageView>(v)); c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, ctr, nullptr); });
}
/* render targets: the 8 colour views + depth as raw pointers; DxvkRenderTargets (Rc<DxvkImageView> attachments) rebuilt on the worker */
static void c5_bind_rts(const DxvkRenderTargets &rts) {
  struct Raw { DxvkImageView *col[MaxNumRenderTargets]; DxvkImageView *depth; } r;
  for (uint32_t i = 0; i < MaxNumRenderTargets; i++) r.col[i] = rts.color[i].view.ptr();
  r.depth = rts.depth.view.ptr();
  emit([r] (DxvkContext* c) {
    DxvkRenderTargets t = { };
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) if (r.col[i]) t.color[i].view = Rc<DxvkImageView>(r.col[i]);
    if (r.depth) t.depth.view = Rc<DxvkImageView>(r.depth);
    c->bindRenderTargets(std::move(t), 0u); });
}
static void c5_clear_rt(DxvkImageView *v, VkImageAspectFlags asp, VkClearValue cv) {   /* DxvkAttachment rebuilt on the worker */
  emit([v, asp, cv] (DxvkContext* c) { DxvkAttachment a = { }; a.view = Rc<DxvkImageView>(v); c->clearRenderTarget(a, asp, cv, 0u); });
}
/* data paths. The staging source stays an Rc: the staging ring's buffer is owned by nobody else once the ring moves on. */
static void c5_invalidate(DxvkBuffer *b, DxvkResourceAllocation *a) {   /* DISCARD rename: Obj.dyn holds the storage until replaced (then buried) or released */
  emit([b, a] (DxvkContext* c) { c->invalidateBuffer(Rc<DxvkBuffer>(b), Rc<DxvkResourceAllocation>(a)); });
}
static void c5_update_buffer(DxvkBuffer *b, uint32_t off, const uint8_t *data, size_t size) {
  emit_data<uint8_t>(data, size, [b, off] (DxvkContext* c, const uint8_t *d, size_t n) { c->updateBuffer(Rc<DxvkBuffer>(b), off, n, d); });
}
static void c5_copy_buffer(DxvkBuffer *dst, VkDeviceSize doff, const Rc<DxvkBuffer> &sb, VkDeviceSize so, VkDeviceSize n) {   /* staging -> table buffer */
  emit([dst, doff, sb, so, n] (DxvkContext* c) { c->copyBuffer(Rc<DxvkBuffer>(dst), doff, sb, so, n); });
}
static void c5_copy_buffer2(DxvkBuffer *dst, VkDeviceSize doff, DxvkBuffer *src, VkDeviceSize so, VkDeviceSize n) {   /* table buffer -> table buffer */
  emit([dst, doff, src, so, n] (DxvkContext* c) { c->copyBuffer(Rc<DxvkBuffer>(dst), doff, Rc<DxvkBuffer>(src), so, n); });
}
static void c5_copy_to_image(DxvkImage *img, VkImageSubresourceLayers sl, VkOffset3D off, VkExtent3D ext, const Rc<DxvkBuffer> &sb, VkDeviceSize so, uint32_t pitch, VkFormat fmt) {
  emit([img, sl, off, ext, sb, so, pitch, fmt] (DxvkContext* c) { c->copyBufferToImage(Rc<DxvkImage>(img), sl, off, ext, sb, so, pitch, 0, fmt); });
}
static void c5_copy_image(DxvkImage *di, VkImageSubresourceLayers dl, VkOffset3D doff, DxvkImage *si, VkImageSubresourceLayers sl, VkOffset3D soff, VkExtent3D ext) {
  emit([di, dl, doff, si, sl, soff, ext] (DxvkContext* c) { c->copyImage(Rc<DxvkImage>(di), dl, doff, Rc<DxvkImage>(si), sl, soff, ext); });
}
/* b1dx_c_srvs: true when s2_srv(st, s, ...) would be a pass-through (no pending copy, no alias in that slot), so the table's
   own view is what gets bound and can be captured raw. Mirrors s2_srv's early-outs exactly (an alias view is owned by the
   s2 alias cache, not a table: it keeps the Rc capture). */
static inline bool c5_srv_plain(int st, uint32_t s) { return s2::pend.empty() && (st < 0 || st > 5 || s >= 128 || !s2::al_dst[st][s]); }
