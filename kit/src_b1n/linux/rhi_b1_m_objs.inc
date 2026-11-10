/* rhi_b1_m_objs.inc: object creation: views, input layouts, blend / depth-stencil / rasterizer / sampler states, releases. Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
/* ---- creation (desc = host pointer to the game's D3D11 desc struct; 32-bit layout, all fields 4 bytes) ---- */
/* SRV/RTV/DSV: dim = D3D11 ViewDimension of that view kind; a,b,c,d = the union fields; fmt 0 = resource format */
extern "C" uint32_t b1dx_view_create(int type, uint32_t res, uint32_t fmt, uint32_t dim, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int hasdesc) {
  if (g_dev == nullptr) return 0;
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::string sig; for (uint32_t i = 0; i < na; i++) { sig += (i ? "," : "") + std::to_string((int)attrs[i].format) + "@" + std::to_string(attrs[i].binding); }   /* m79 census */
  uint32_t nid = g_ils.add(std::move(il)); if (nid) m79::ilsig[nid] = sig.empty() ? std::string("none") : sig;
  return nid;
}

extern "C" uint32_t b1dx_blend_create(const uint32_t *d) {   /* D3D11_BLEND_DESC: a2c, independent, RT[8] x 8 dwords */
  if (g_dev == nullptr) return 0;
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
  std::lock_guard<RmuLock> lk(g_rmu);
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
    { /* rc80 (chat 6, asset re-cook / format lab, ceiling test): BSI_B1_LODBIAS=<float> adds that mip bias to every
         sampler that can reach a mip above 0. +1 = a quarter of the texel bytes per fetch, more than any texture
         re-encode (BC1 4 bpp -> ASTC 8x8 2 bpp) could save: the GPU time it recovers is the CEILING of the texture
         track (R5/F1). Not exact; measurement only. Off (0) by default. */
      static float lb = -1e9f; if (lb == -1e9f) { const char *e = std::getenv("BSI_B1_LODBIAS"); lb = e ? (float)std::atof(e) : 0.0f; }
      if (lb != 0.0f && mxl > 0.0f) { bias += lb; static long n; if (++n == 1 || n == 100 || n == 1000) std::fprintf(stderr, "[b1rc] LODBIAS %+.2f applied (sampler %ld)\n", lb, n); } }
    k.setLodRange(mnl, mxl, bias);
    { static int na = -1; if (na < 0) { const char *e = std::getenv("BSI_B1_NOANISO"); na = e && std::atoi(e) > 0; } k.setAniso((fb & 0x40) && !na ? d[5] : 0u); }
    { /* g4 (b85, chat 4): BSI_B1_MAXANISO=N caps the anisotropy of every sampler at N (8x costs ~2.6 ms/frame everywhere, chat 6 rc80r;
         an INVISIBLE-bar experiment for 4x / 2x: measurement + side-by-sides, not a shipping default). 0 / unset = the game's value. */
      static int ma = -1; if (ma < 0) { const char *e = std::getenv("BSI_B1_MAXANISO"); ma = e ? std::atoi(e) : 0; if (ma < 0) ma = 0; }
      if (ma && (fb & 0x40) && d[5] > (uint32_t)ma) { k.setAniso((uint32_t)ma); static long n; if (++n == 1 || n == 1000) std::fprintf(stderr, "[b1g4] MAXANISO %d applied (sampler %ld, game asked %u)\n", ma, n, d[5]); } }
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
  std::lock_guard<RmuLock> lk(g_rmu);
  if (kind == 3) { dc88::flush_view(id); for (int st = 0; st < 6; st++) for (int s = 0; s < 128; s++) if (dc88::srv_res[st][s] && g_bc.srv[st][s] == id) dc88::set_slot(dc88::srv_res[st][s], 0); }   /* dc88 */
  if (g_rawcap) { if (kind == 3) { if (View *v = g_views.get(id)) c5_bury(std::move(v->iv)); } else if (kind == 8) { if (SS *s = g_sss.get(id)) c5_bury(std::move(s->s)); } }   /* c5 */
  bep(kind == 3 ? 5 : kind == 4 ? 2 : kind == 8 ? 6 : 7);   /* c5b141 epoch: the id leaves the bind cache */
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
  std::lock_guard<RmuLock> lk(g_rmu);
  g_c5b_shrel++; bep(3);   /* c5b141: shader-set memos (b1dx_c_shader) + epoch */
  BindCache::forget(g_bc.sh, 6, id);
  dc88::shader_released(id); for (int st = 0; st < 6; st++) if (dc88::sh[st] == id) dc88::sh[st] = 0;   /* dc88 */
  sb87::shader_released(id);   /* sb87 */
  b1eo_ps_released(id);
  b1fx_ps_released(id);   /* b124 */
  b1ab_cs_released(id);
  if (g_rawcap) c5_shader_released(id);   /* c5: pointer cache + graveyard, before the table drops its Rc */
  b1dx_shader_release_raw(id);
}
