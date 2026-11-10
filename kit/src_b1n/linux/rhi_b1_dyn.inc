/* rhi_b1_dyn.inc -- dyn103 (BSI_B1_DYNRING=1, off by default): persistent host-visible ring for D3D11 dynamic buffers.
   Included by rhi_b1_dxvk.cpp just before write_buffer(); the hooks in rhi_b1_dxvk.cpp carry a "dyn103" comment.

   Today a MAP_WRITE_DISCARD of a dynamic buffer (map_discard, a full-buffer Unmap / UpdateSubresource) allocates a new
   DxvkResourceAllocation (dyn_storage -> DxvkBuffer::allocateStorage, ~1,570 per frame) and emits invalidateBuffer to the
   CS worker, which re-assigns the storage and dirties every slot the buffer is bound to. Here a DISCARD instead takes a
   slice of a big persistent DxvkBuffer (VERTEX | INDEX | UNIFORM | TRANSFER, HOST_VISIBLE | HOST_COHERENT, bump allocated
   in chunks); the game's bytes are memcpy'd once into the mapped slice; every bind of that object emits
   DxvkBufferSlice(ring, off, len) instead of the object's own buffer; no allocation, no invalidateBuffer, no CS-side copy.

   Exactness (the GPU reads the same bytes at the same draws):
   - the slice has the object's full size, so NO_OVERWRITE maps / per-draw bufwrites land at the same offsets as before;
   - invalidateBuffer replacement: DXVK's invalidateBuffer re-points every slot that holds the buffer at the new storage.
     We cannot prove the game re-binds after every Map, so after a DISCARD we re-emit the bind for every slot of g_bc that
     holds this id (resync); the bind filter's invariant becomes "slot holds id => the stream holds the id's current
     slice", which every bind emit and every resync maintain, so the cache keys need not change;
   - foreign rebinds: pp76 (sun-shadow replay / light shafts), sb87 tile draws and the VR AO per-eye dispatch restore
     "the game's buffer" with DxvkBufferSlice(o->buf); after those calls dyn103_foreign() re-emits the ring slices for every
     bound ring-backed id. g4 (BSI_B1_SHCOAL) rebinds and snapshots o->dyn directly: the ring stays off when g4 is enabled;
   - retirement: a chunk is reused only when (a) no live object still holds a slice in it (refs == 0; NO_OVERWRITE writes
     may come at any time while an object holds its slice) and (b) b1dx_frames_completed() >= chunk.last, where last is the
     id of the present whose fence covers the last touch (bind, write, copy) of the chunk. A slice that stays bound across
     frames without a new bind is touched at every present (slot scan), so a chunk the GPU may still read is never handed
     out; DXVK's frame fence is signalled by the finish thread after every earlier submission completed (dxvk_queue.cpp).
   Fallbacks (the old path, byte-identical): ring off, g4 on, slice > BSI_B1_DYNRING_MAX (default 4 MB), ring full
   (all chunks in use or in flight and the cap BSI_B1_DYNRING_CHUNKS x BSI_B1_DYNRING_CHUNK_MB reached), no fence yet
   (shadow mode: the ring fills once and everything falls back). Switching an object back to the old path drops its slice
   and re-emits plain binds before the old path's invalidateBuffer. */
extern "C" unsigned long long b1dx_frames_completed(void);
namespace dyn103 {
  static int on = -1;
  struct Chunk { Rc<DxvkBuffer> buf; uint8_t *base = nullptr; VkDeviceSize size = 0, off = 0; uint64_t last = 0; long refs = 0; };
  static std::vector<Chunk> chunks;   /* never freed: raw pointers to the buffers may sit in queued CS chunks (RAWCAP) */
  static int cur = -1;
  static uint64_t frame_next = 1;      /* id of the present whose fence covers the work being recorded now */
  static VkDeviceSize chunk_bytes = 16u << 20, max_slice = 4u << 20, align_ = 256; static int max_chunks = 4;
  static int16_t slot_cb[6][14], slot_vb[32], slot_ib;   /* chunk index our emits left in each DXVK slot, -1 none */
  /* stats */
  static long n_maps, n_nodata, n_bytes, n_direct, n_ordered, n_fb_full, n_fb_big, n_fb_nofence, n_resync, n_foreign, n_copy, n_grow, n_pres, max_inflight, n_zombie;
  static inline void touch(int k) { chunks[k].last = frame_next; }
  /* c5b141 (BSI_B1_DYNRAW, default 1; 0 = the b140 captures): the ring's bind emits capture the chunk buffer as a raw pointer and
     build the Rc on the worker (as RAWCAP does for these emits). The chunks are never freed (see `chunks`), so the pointer is
     valid in any queued chunk, the g4 deferred stream included. The DXVK call is the same (bindUniformBuffer / bindVertexBuffer /
     bindIndexBuffer with DxvkBufferSlice(Rc(buf), off, len)); the render thread no longer increments the ring buffer's use count
     (one shared, worker-decremented atomic per emit, ~4-5k per frame). The resync scan skips stage rows of g_bc.cb that do not hold
     the id with one vectorisable test per row (same slots, same order). */
  static int raw = 1;
  static inline bool rawcap() { return c5_raw() || raw; }
  static void init() {
    { const char *r = std::getenv("BSI_B1_DYNRAW"); raw = !(r && std::atoi(r) == 0); }   /* c5b141 */
    const char *e = std::getenv("BSI_B1_DYNRING"); on = !e || std::atoi(e) > 0;   /* b109 (chat 5): default ON (measured exact, -1.9..-3 ms RT, -1.3 ms CS; BSI_B1_DYNRING=0 = off) */
    if (!on) return;
    { const char *g = std::getenv("BSI_B1_SHCOAL"); int gm = g ? std::atoi(g) : 0; if (gm > 0 && gm <= 2) { on = 0;
        std::fprintf(stderr, "[b1dyn] dynamic ring OFF: BSI_B1_SHCOAL=%d (g4 snapshots / rebinds the objects' own storage)\n", gm); return; } }
    if ((e = std::getenv("BSI_B1_DYNRING_CHUNK_MB")) && std::atoi(e) > 0) chunk_bytes = (VkDeviceSize)std::atoi(e) << 20;
    max_chunks = 8;   /* b109: default 8 chunks (4 overflowed at the Fair: 17-66 fallbacks/frame; 8 settles at 5-6 used) */
    if ((e = std::getenv("BSI_B1_DYNRING_CHUNKS")) && std::atoi(e) > 0) max_chunks = std::min(std::atoi(e), 32);
    if ((e = std::getenv("BSI_B1_DYNRING_MAX")) && std::atoi(e) > 0) max_slice = (VkDeviceSize)std::atoi(e) << 10;   /* KB */
    const auto &lim = g_dev->properties().core.properties.limits;
    align_ = std::max<VkDeviceSize>(16, lim.minUniformBufferOffsetAlignment); if ((e = std::getenv("BSI_B1_DYNRING_ALIGN")) && std::atoi(e) >= 16) align_ = std::atoi(e);
    if (align_ & (align_ - 1)) align_ = 256;
    if (max_slice > chunk_bytes) max_slice = chunk_bytes;
    std::memset(slot_cb, 0xff, sizeof slot_cb); std::memset(slot_vb, 0xff, sizeof slot_vb); slot_ib = -1;
    std::fprintf(stderr, "[b1dyn] dynamic ring ON: up to %d chunks of %llu MB, slices <= %llu KB, offsets aligned to %llu (minUniformBufferOffsetAlignment %llu), %s\n",
                 max_chunks, (unsigned long long)(chunk_bytes >> 20), (unsigned long long)(max_slice >> 10), (unsigned long long)align_, (unsigned long long)lim.minUniformBufferOffsetAlignment, cs_on() ? "CS worker" : "direct");
  }
  static bool make_chunk() {
    DxvkBufferCreateInfo bi = { };
    bi.size = chunk_bytes;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    bi.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    bi.debugName = "B1 dynamic ring";
    try {
      Chunk c; c.buf = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      c.base = (uint8_t *)c.buf->mapPtr(0); c.size = chunk_bytes; if (!c.base) return false;
      if (raw) (void)new Rc<DxvkBuffer>(c.buf);   /* c5b141: one reference never dropped, so a raw capture queued at process exit outlives `chunks`' static destructor */
      chunks.push_back(std::move(c)); n_grow++; return true;
    } catch (const DxvkError& e) { static int nl; if (nl++ < 4) std::fprintf(stderr, "[b1dyn] ring chunk create failed: %s\n", e.message().c_str()); return false; }
  }
  /* a slice of size bytes; false = caller takes the old path */
  static bool alloc(VkDeviceSize size, int *ci, VkDeviceSize *off) {
    if (size > max_slice) { n_fb_big++; return false; }
    for (int pass = 0; pass < 2; pass++) {
      if (cur >= 0) { Chunk &c = chunks[cur]; VkDeviceSize o = (c.off + align_ - 1) & ~(align_ - 1);
        if (o + size <= c.size) { *ci = cur; *off = o; c.off = o + size; c.refs++; touch(cur); return true; } }
      if (pass) break;
      uint64_t done = b1dx_frames_completed(); int pick = -1;
      for (size_t k = 0; k < chunks.size(); k++) if ((int)k != cur && chunks[k].refs == 0 && chunks[k].last <= done) { pick = (int)k; break; }
      if (pick < 0 && (int)chunks.size() < max_chunks && make_chunk()) pick = (int)chunks.size() - 1;
      if (pick < 0) { if (!done && !chunks.empty()) n_fb_nofence++; n_fb_full++; return false; }
      chunks[pick].off = 0; cur = pick;
    }
    n_fb_full++; return false;
  }
  static inline bool backed(const Obj &o) { return on > 0 && o.dchunk >= 0; }
  static void drop(Obj &o) { if (o.dchunk >= 0) { chunks[o.dchunk].refs--; o.dchunk = -1; o.doff = 0; } }
  /* bind emits (Rc capture; RAWCAP: raw pointer, the ring buffers are never freed) */
  static void emit_cb(int st, uint32_t slot, int k, VkDeviceSize off, VkDeviceSize len) {
    touch(k); slot_cb[st][slot] = (int16_t)k; VkShaderStageFlags sb = sbit(st); uint32_t s = sidx(st) * 16 + slot;
    if (rawcap()) emit([sb, s, b = chunks[k].buf.ptr(), off, len] (DxvkContext* c) { c->bindUniformBuffer(sb, s, DxvkBufferSlice(Rc<DxvkBuffer>(b), off, len)); });   /* c5b141: raw also without RAWCAP */
    else emit([sb, s, sl = DxvkBufferSlice(chunks[k].buf, off, len)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, s, std::move(sl)); });
  }
  static void emit_vb(uint32_t s, int k, VkDeviceSize off, VkDeviceSize len, uint32_t stride) {
    touch(k); slot_vb[s] = (int16_t)k;
    if (rawcap()) c5_bind_vb(s, chunks[k].buf.ptr(), off, len, stride);   /* c5b141 */
    else emit([s, sl = DxvkBufferSlice(chunks[k].buf, off, len), stride] (DxvkContext* c) mutable { c->bindVertexBuffer(s, std::move(sl), stride); });
  }
  static void emit_ib(int k, VkDeviceSize off, VkDeviceSize len, VkIndexType t) {
    touch(k); slot_ib = (int16_t)k;
    if (rawcap()) c5_bind_ib(chunks[k].buf.ptr(), off, len, t);   /* c5b141 */
    else emit([sl = DxvkBufferSlice(chunks[k].buf, off, len), t] (DxvkContext* c) mutable { c->bindIndexBuffer(std::move(sl), t); });
  }
  /* plain (old-path) binds of an object that just left the ring */
  static void emit_plain_cb(Obj &o, int st, uint32_t slot) { slot_cb[st][slot] = -1; if (c5_raw()) c5_bind_cb(sbit(st), sidx(st) * 16 + slot, o.buf.ptr());
    else emit([sb = sbit(st), s = sidx(st) * 16 + slot, sl = DxvkBufferSlice(o.buf)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, s, std::move(sl)); }); }
  static void emit_plain_vb(Obj &o, uint32_t s, uint32_t off, uint32_t stride) { slot_vb[s] = -1; VkDeviceSize sz = o.buf->info().size; if (off >= sz) return;
    if (c5_raw()) c5_bind_vb(s, o.buf.ptr(), off, sz - off, stride); else emit([s, sl = DxvkBufferSlice(o.buf, off, sz - off), stride] (DxvkContext* c) mutable { c->bindVertexBuffer(s, std::move(sl), stride); }); }
  static void emit_plain_ib(Obj &o, uint32_t off, VkIndexType t) { slot_ib = -1; VkDeviceSize sz = o.buf->info().size; if (off >= sz) return;
    if (c5_raw()) c5_bind_ib(o.buf.ptr(), off, sz - off, t); else emit([sl = DxvkBufferSlice(o.buf, off, sz - off), t] (DxvkContext* c) mutable { c->bindIndexBuffer(std::move(sl), t); }); }
  /* re-emit every slot of the bind filter that holds id (what invalidateBuffer did for the old storage) */
  static void resync(uint32_t id, Obj &o) {
    struct Ep { long n0; ~Ep() { if (n_resync != n0) { bep(0); bep(1); bep(4); } } } ep_ { n_resync };   /* c5b141 epoch: slots re-pointed (VB / IB / CB) */
    VkDeviceSize sz = o.buf->info().size; bool r = o.dchunk >= 0;
    for (int st = 0; st < 6; st++) {
      { const uint32_t *row = g_bc.cb[st]; uint32_t hit = 0; for (uint32_t i = 0; i < 14; i++) hit |= (uint32_t)(row[i] == id); if (raw && !hit) continue; }   /* c5b141: row test first (exact: the row holds no slot to re-emit) */
      for (uint32_t i = 0; i < 14; i++) if (g_bc.cb[st][i] == id) { n_resync++; if (r) emit_cb(st, i, o.dchunk, o.doff, sz); else emit_plain_cb(o, st, i); } }
    { uint32_t hit = 0; for (uint32_t s = 0; s < 32; s++) hit |= (uint32_t)(g_bc.vb[s] == id); if (hit || !raw)   /* c5b141 */
    for (uint32_t s = 0; s < 32; s++) if (g_bc.vb[s] == id && g_bc.vbo[s] != BC_UNK && g_bc.vbo[s] < sz) { n_resync++; uint32_t str = g_bc.vbs[s] == BC_UNK ? 0 : g_bc.vbs[s];
      if (r) emit_vb(s, o.dchunk, o.doff + g_bc.vbo[s], sz - g_bc.vbo[s], str); else emit_plain_vb(o, s, g_bc.vbo[s], str); } }
    if (g_bc.ib == id && g_bc.ibo != BC_UNK && g_bc.ibo < sz) { n_resync++; VkIndexType t = g_bc.ibf == BC_UNK ? VK_INDEX_TYPE_UINT16 : (VkIndexType)g_bc.ibf;
      if (r) emit_ib(o.dchunk, o.doff + g_bc.ibo, sz - g_bc.ibo, t); else emit_plain_ib(o, g_bc.ibo, t); }
  }
  /* the DISCARD: a fresh slice (data = the whole buffer, or null: filled by bufwrites); false = old path (slice dropped) */
  static bool discard(Obj &o, uint32_t id, const void *data, uint32_t size) {
    bool had = o.dchunk >= 0; drop(o);
    int k; VkDeviceSize off; VkDeviceSize sz = o.buf->info().size;
    if (!alloc(sz, &k, &off)) { if (had) resync(id, o); return false; }   /* plain binds first; the old path's invalidateBuffer then redirects them */
    o.dchunk = k; o.doff = (uint32_t)off;
    if (data) { if (size > sz) size = (uint32_t)sz; std::memcpy(chunks[k].base + off, data, size); } else n_nodata++;
    n_maps++; n_bytes += (long)sz;
    resync(id, o);
    return true;
  }
  /* per-draw bufwrite (D3D11 DISCARD / NO_OVERWRITE contract: the range is not in use by the GPU): straight into the slice */
  static bool bufwrite(Obj &o, uint32_t id, uint32_t off, uint32_t size, const void *data) {
    if (on <= 0) return false;
    if (o.dchunk < 0) { if (o.dyn != nullptr) return false;   /* an old-path object keeps its storage */
      if (!discard(o, id, nullptr, 0)) return false; }
    std::memcpy(chunks[o.dchunk].base + o.doff + off, data, size); touch(o.dchunk); n_direct++; return true;
  }
  /* ordered partial write (UpdateSubresource box / non-DISCARD Unmap through write_buffer): into the ring at the stream position */
  static bool write(Obj &o, uint32_t off, uint32_t size, const void *data) {
    if (!backed(o)) return false;
    int k = o.dchunk; VkDeviceSize ro = (VkDeviceSize)o.doff + off; touch(k); n_ordered++;
    if (size <= 65536u && !(size & 3u) && !(ro & 3u)) {
      if (c5_raw()) c5_update_buffer(chunks[k].buf.ptr(), (uint32_t)ro, (const uint8_t *)data, size); else
      emit_data<uint8_t>((const uint8_t *)data, size, [b = chunks[k].buf, ro] (DxvkContext* c, const uint8_t *d, size_t n) { c->updateBuffer(b, ro, n, d); });
    } else {
      Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
      if (c5_raw()) c5_copy_buffer(chunks[k].buf.ptr(), ro, st.b, st.o, size); else emit([b = chunks[k].buf, ro, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(b, ro, sb, so, size); });
      up_maybe_flush(size);
    }
    return true;
  }
  /* GPU copy from a staging slice into the object (b1dx_buffer_write) */
  static bool copy_in(Obj &o, uint32_t off, const Rc<DxvkBuffer> &sb, VkDeviceSize so, uint32_t size) {
    if (!backed(o)) return false;
    int k = o.dchunk; VkDeviceSize ro = (VkDeviceSize)o.doff + off; touch(k); n_copy++;
    if (c5_raw()) c5_copy_buffer(chunks[k].buf.ptr(), ro, sb, so, size); else emit([b = chunks[k].buf, ro, sb, so, size] (DxvkContext* c) { c->copyBuffer(b, ro, sb, so, size); });
    return true;
  }
  /* CopyResource / CopySubresourceRegion between buffers when either side lives in the ring */
  static bool copy(Obj *d, uint32_t x, Obj *s, uint32_t so, uint32_t sz) {
    if (on <= 0 || (d->dchunk < 0 && s->dchunk < 0)) return false;
    Rc<DxvkBuffer> db = d->dchunk >= 0 ? chunks[d->dchunk].buf : d->buf, sb = s->dchunk >= 0 ? chunks[s->dchunk].buf : s->buf;
    VkDeviceSize doff = (d->dchunk >= 0 ? d->doff : 0u) + (VkDeviceSize)x, soff = (s->dchunk >= 0 ? s->doff : 0u) + (VkDeviceSize)so;
    if (d->dchunk >= 0) touch(d->dchunk); if (s->dchunk >= 0) touch(s->dchunk); n_copy++;
    if (c5_raw()) c5_copy_buffer2(db.ptr(), doff, sb.ptr(), soff, sz); else emit([db, doff, sb, soff, sz] (DxvkContext* c) { c->copyBuffer(db, doff, sb, soff, sz); });
    return true;
  }
  /* after a foreign restore bind (pp76 / sb87 / VR AO): the ring slices of every bound ring-backed id again */
  static void foreign() {
    if (on <= 0 || chunks.empty()) return; n_foreign++;
    bep(0); bep(1); bep(4);   /* c5b141 epoch: after a foreign restore the ring slices are re-emitted */
    for (int st = 0; st < 6; st++) for (uint32_t i = 0; i < 14; i++) { uint32_t id = g_bc.cb[st][i]; if (!id || id == BC_UNK) continue; Obj *o = robj(id); if (o && o->kind == 1 && o->dchunk >= 0) emit_cb(st, i, o->dchunk, o->doff, o->buf->info().size); }
    for (uint32_t s = 0; s < 32; s++) { uint32_t id = g_bc.vb[s]; if (!id || id == BC_UNK || g_bc.vbo[s] == BC_UNK) continue; Obj *o = robj(id); VkDeviceSize sz;
      if (o && o->kind == 1 && o->dchunk >= 0 && g_bc.vbo[s] < (sz = o->buf->info().size)) emit_vb(s, o->dchunk, o->doff + g_bc.vbo[s], sz - g_bc.vbo[s], g_bc.vbs[s] == BC_UNK ? 0 : g_bc.vbs[s]); }
    { uint32_t id = g_bc.ib; Obj *o = (id && id != BC_UNK && g_bc.ibo != BC_UNK) ? robj(id) : nullptr; VkDeviceSize sz;
      if (o && o->kind == 1 && o->dchunk >= 0 && g_bc.ibo < (sz = o->buf->info().size)) emit_ib(o->dchunk, o->doff + g_bc.ibo, sz - g_bc.ibo, g_bc.ibf == BC_UNK ? VK_INDEX_TYPE_UINT16 : (VkIndexType)g_bc.ibf); }
  }
  /* present: the next present id; slots still holding ring slices keep their chunks in flight; stats every 600 */
  static void present(uint64_t pframe) {
    if (on <= 0) return;
    frame_next = pframe + 1;
    for (int st = 0; st < 6; st++) for (int i = 0; i < 14; i++) if (slot_cb[st][i] >= 0) touch(slot_cb[st][i]);
    for (int s = 0; s < 32; s++) if (slot_vb[s] >= 0) touch(slot_vb[s]);
    if (slot_ib >= 0) touch(slot_ib);
    uint64_t done = b1dx_frames_completed(); long inflight = 0, pinned = 0; for (auto &c : chunks) { if (c.last > done) inflight++; if (c.refs) pinned++; }
    if (inflight > max_inflight) max_inflight = inflight;
    if ((++n_pres % 600) == 0) {
      std::fprintf(stderr, "[b1dyn] ring: %ld maps served (%ld without data, %.2f MB; %.1f/frame), direct writes %ld, ordered writes %ld, copies %ld, resync binds %ld, foreign resyncs %ld; "
                           "chunks %zu x %llu MB (grown %ld, %ld pinned by live slices, %ld in flight now, max %ld); fallbacks: full %ld (no fence %ld), too big %ld; frames done %llu / next %llu\n",
                   n_maps, n_nodata, n_bytes / 1048576.0, n_maps / 600.0, n_direct, n_ordered, n_copy, n_resync, n_foreign, chunks.size(), (unsigned long long)(chunk_bytes >> 20), n_grow, pinned, inflight, max_inflight,
                   n_fb_full, n_fb_nofence, n_fb_big, (unsigned long long)done, (unsigned long long)frame_next);
      n_maps = n_nodata = n_bytes = n_direct = n_ordered = n_copy = n_resync = n_foreign = n_fb_full = n_fb_nofence = n_fb_big = 0; max_inflight = 0; }
  }
}
/* ---- hooks used by rhi_b1_dxvk.cpp (declared near the c5 declarations) ---- */
static void dyn103_init() { dyn103::init(); }
static inline bool dyn103_on() { return dyn103::on > 0; }
static bool dyn103_discard(Obj &o, uint32_t id, const void *data, uint32_t size) { return dyn103::on > 0 && dyn103::discard(o, id, data, size); }
static bool dyn103_bufwrite(Obj &o, uint32_t id, uint32_t off, uint32_t size, const void *data) { return dyn103::bufwrite(o, id, off, size, data); }
static bool dyn103_write(Obj &o, uint32_t off, uint32_t size, const void *data) { return dyn103::write(o, off, size, data); }
static bool dyn103_copy_in(Obj &o, uint32_t off, const Rc<DxvkBuffer> &sb, VkDeviceSize so, uint32_t size) { return dyn103::copy_in(o, off, sb, so, size); }
static bool dyn103_copy(Obj *d, uint32_t x, Obj *s, uint32_t so, uint32_t sz) { return dyn103::copy(d, x, s, so, sz); }
static void dyn103_release(Obj &o) { if (dyn103::on > 0) dyn103::drop(o); }   /* the slot scan at present keeps a still-bound slice's chunk in flight */
static void dyn103_present(uint64_t pframe) { dyn103::present(pframe); }
static void dyn103_foreign() { dyn103::foreign(); }
static const uint8_t *dyn103_read_ptr(Obj &o) { return dyn103::backed(o) ? dyn103::chunks[o.dchunk].base + o.doff : nullptr; }
/* bind hooks: true = a ring slice was emitted (the caller skips its own emit); false = not ring-backed (slot noted as plain) */
static bool dyn103_bind_cb(Obj *o, uint32_t key, int st, uint32_t slot) {
  if (dyn103::on <= 0 || st < 0 || st >= 6) return false;
  if (key && o->dchunk >= 0) { dyn103::emit_cb(st, slot, o->dchunk, o->doff, o->buf->info().size); return true; }
  dyn103::slot_cb[st][slot] = -1; return false;
}
static bool dyn103_bind_vb(Obj *o, bool ok, uint32_t s, uint32_t off, uint32_t stride) {
  if (dyn103::on <= 0) return false;
  if (ok && o->dchunk >= 0) { dyn103::emit_vb(s, o->dchunk, o->doff + off, o->buf->info().size - off, stride); return true; }
  dyn103::slot_vb[s] = -1; return false;
}
static bool dyn103_bind_ib(Obj *o, bool ok, uint32_t off, VkIndexType t) {
  if (dyn103::on <= 0) return false;
  if (ok && o->dchunk >= 0) { dyn103::emit_ib(o->dchunk, o->doff + off, o->buf->info().size - off, t); return true; }
  dyn103::slot_ib = -1; return false;
}
