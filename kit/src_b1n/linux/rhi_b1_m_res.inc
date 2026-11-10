/* rhi_b1_m_res.inc: formats, buffers, textures, views of the B1 path, frame end, dumps. Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
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
    else { if (gp::on() && m79::clears) gp::on_clear("submit (immediate flush)");   /* b115 */ ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }
  }
}

/* buffer: size bytes, initial data optional (host pointer, valid only during the call) */
extern "C" uint32_t b1dx_buffer_create2(uint32_t size, uint32_t bind, uint32_t usage, const void *data);
extern "C" uint32_t b1dx_buffer_create(uint32_t size, uint32_t bind, const void *data) { return b1dx_buffer_create2(size, bind, 0, data); }
namespace { namespace sb87 { void created(uint32_t id, uint32_t bind, uint32_t size, const void *data); } }   /* sb87 */
extern "C" uint32_t b1dx_buffer_create2(uint32_t size, uint32_t bind, uint32_t usage, const void *data) {
  if (g_dev == nullptr || !size) return 0;
  bool hostvis = usage == 2 || (bind & 0x4u);
  std::lock_guard<RmuLock> lk(g_rmu);
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
    { uint32_t nid = new_id(std::move(o)); if (data) sb87::created(nid, bind, size, data); /* sb87: CPU copy of small static VB / IB */ shrep::created(nid, size, data, hostvis); /* b115 */ return nid; }
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] buffer create %u failed: %s\n", size, e.message().c_str()); }
  return 0;
}

extern "C" int b1dx_buffer_write(uint32_t id, uint32_t offset, uint32_t size, const void *data) {
  std::lock_guard<RmuLock> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 1 || !size) return 0;
  try {
    Rc<DxvkBuffer> dst = g_objs[id - 1].buf;
    if ((uint64_t)offset + size > dst->info().size) return 0;
    g4_wrote(id, 2);   /* g4 */ shrep::wrote(id, offset, size, data, (uint32_t)dst->info().size);   /* b115 */
    Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
    if (dyn103_copy_in(g_objs[id - 1], offset, st.b, st.o, size)) { } else   /* dyn103: into the ring slice */
    if (c5_raw()) c5_copy_buffer(dst.ptr(), offset, st.b, st.o, size); else {   /* c5 */
    emit([dst, offset, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(dst, offset, sb, so, size); }); } up_maybe_flush(size); g_nwrites++;
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 2 || !data) return 0;
  try {
    Obj &o = g_objs[id - 1]; if (o.img->info().type != VK_IMAGE_TYPE_3D || mip >= o.img->info().mipLevels) return 0; shrep::tex_written(id);   /* b115 */
    VkExtent3D ext = o.img->mipLevelExtent(mip); const DxvkFormatInfo *fi = lookupFormatInfo(o.fmt);
    VkDeviceSize sz = util::computeImageDataSize(o.fmt, ext, fi->aspectMask);
    dc88::flush_res(id, dc88::n_upload);   /* dc88 */ s2_written(id);
    Stage st = stage_alloc(sz);
    util::packImageData(st.p, data, rowp, slicep, 0, 0, VK_IMAGE_TYPE_3D, ext, 1, fi, fi->aspectMask);
    VkImageSubresourceLayers sl = { fi->aspectMask, mip, 0u, 1u };
    if (c5_raw()) c5_copy_to_image(o.img.ptr(), sl, VkOffset3D{ 0, 0, 0 }, ext, st.b, st.o, 0, o.fmt); else   /* c5 */
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
    std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
  if (!id || id > g_objs.size() || g_objs[id - 1].kind != 2 || !data) return 0;
  if (g_eo_any) b1eo_hook_written(id);
  shrep::tex_written(id);   /* b115 */
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
    dc88::flush_res(id, dc88::n_upload);   /* dc88: an upload into a pending target */ s2_written(id);
    Stage st = stage_alloc(sz); std::memcpy(st.p, data, sz); if (mip < 32) o.wmask |= 1u << mip;
    VkImageSubresourceLayers sl = { fi->aspectMask, mip, layer, 1u };
    if (c5_raw()) c5_copy_to_image(o.img.ptr(), sl, VkOffset3D{ (int32_t)x, (int32_t)y, 0 }, ext, st.b, st.o, pitch, o.fmt); else   /* c5 */
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
namespace { namespace sb87 { void released(uint32_t id, int kind); void frame(); } }   /* sb87, rhi_b1_sb87.inc */
extern "C" void b1dx_release(uint32_t id) {
  std::lock_guard<RmuLock> lk(g_rmu);
  if (!id || id > g_objs.size() || !g_objs[id - 1].kind) return;
  if (g_objs[id - 1].kind == 2) dc88::flush_res(id, dc88::n_relref);   /* dc88: before the table drops the image */
  if (g_objs[id - 1].kind == 1) { bep(0); bep(1); bep(4); /* c5b141 epoch */ dyn103_release(g_objs[id - 1]); /* dyn103 */ BindCache::forget(&g_bc.cb[0][0], 6 * 14, id); BindCache::forget(g_bc.vb, 32, id); BindCache::forget(&g_bc.ib, 1, id); b1vr_cb_forget(id); g4_released(id); /* g4 */ }
  else { g4_released(id); /* g4 */ b1eo_hook_released(id); s2_on_release(id); }
  sb87::released(id, g_objs[id - 1].kind);   /* sb87 */ shrep::released(id);   /* b115 */
  if (g_rawcap) c5_bury_obj(g_objs[id - 1]);   /* c5: the Rcs wait for the chunks that may hold them raw */
  g_objs[id - 1] = Obj(); g_freeids.push_back(id); g_nlive--;
}

/* flush pending uploads (called at every shadow frame, render thread) */
extern "C" void b1dx_upload_flush(void) {
  std::lock_guard<RmuLock> lk(g_rmu);
  if (g_up != nullptr && g_staged) { try { ctx_flush(); } catch (...) { } g_staged = 0; }
}

extern "C" void b1dx_res_stats(char *out, int n) {
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
    if (o) { if (g_rawcap) c5_bury_obj(*o); /* c5 */ *o = std::move(n); } else { g_bb = new_id(std::move(n)); }
    return g_bb;
  } catch (const DxvkError& e) { if (g_rfail++ < 10) std::fprintf(stderr, "[b1] back buffer %ux%u failed: %s\n", w, h, e.message().c_str()); }
  return 0;
}

/* SetRenderTargets: up to 4 colour targets + depth (ids, 0 = none) */
extern "C" void b1dx_set_rts(const uint32_t *ids, uint32_t ds) {
  if (g_ctx == nullptr) return;
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
extern "C" void b1dx_dump_path(const char *path) { std::lock_guard<RmuLock> lk(g_rmu); g_dump_file = path; g_dump_tag = 0; }
extern "C" void b1dx_dump_request(int tag) { std::lock_guard<RmuLock> lk(g_rmu); g_dump_tag = tag; }
static void dump_bb(long frame) {
  Obj *o = obj(g_bb); if (!o) return; dc88::flush_all(dc88::n_rdbk);   /* dc88 */
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
  std::lock_guard<RmuLock> lk(g_rmu);
  try {
    if (g_dump_at == -2) { const char *e = std::getenv("BSI_B1_DUMP"); g_dump_at = e ? std::atoi(e) : -1; }
    dc88::flush_all(dc88::n_present);   /* dc88: frame end: everything still pending is emitted in order */
    g4_present();   /* g4 */ if (g_rawcap) c5_present();   /* c5: graveyard prune + [b1rc5] stats */
    g_frames++; b1eo_hook_frame(); b1ab_hook_frame(); sb87::frame();   /* sb87 */ shrep::frame();   /* b115 */ sheye::frame();   /* b123 */
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
