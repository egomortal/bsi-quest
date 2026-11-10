/* rhi_b1_shadoweo_map.h -- S1 block map: planning and recording of one map build on a DxvkContext (job s1b).
   Shared by the runtime (rhi_b1_shadoweo.inc, inside an emit() lambda) and the offline A/B test (tools/eoab.cpp), so
   the test runs exactly the runtime's dispatch sequence. Needs the DXVK headers and rhi_b1_shadoweo.h first.

   v1 (BSI_B1_SHADOWEO_MAPV=1): build 67's kernels: CS1 (B^2 ld per block), CS2 (S^2 ld per L2 texel), clearBuffer + CS3 (1 group).
   v2 (default): CS1g (gather4), CS2s (groupshared, also sets the flag to 1), CS3w (wide). Same map, see rhi_b1_shadoweo.h.
   Optional constant fill (cleared atlas, see Plan::fill): L1 and L2 are first set to (~c, c) everywhere with
   clearImageView, then the dirty rect is rebuilt as usual. */
#pragma once

namespace b1eomap {
  using namespace dxvk;

  struct Kernels { Rc<DxvkShader> cs1, cs2, cs3; Rc<DxvkSampler> smp; int ver = 2; b1eo::Geo geo; };
  struct Maps { Rc<DxvkImageView> src, l1u, l1s, l2u, l2s; Rc<DxvkBuffer> glob; Rc<DxvkBufferView> globu; uint32_t w = 0, h = 0, bw = 0, bh = 0; };
  struct P8 { int32_t v[8]; };
  struct Plan { P8 p1, p2, p3; uint32_t g1x, g1y, g2x, g2y, g3x, g3y; bool fill = false; uint32_t fillbits = 0;
                uint64_t texels = 0, l1blocks = 0, l2texels = 0; };   /* statistics: atlas texels read (CS1), L1 blocks written, L2 texels written */

  /* compile the kernels (create(stage 5, dxbc) -> shader) and the sampler of CS1g */
  template<class Create> static bool make(Kernels &k, int ver, b1eo::Geo geo, const Rc<DxvkDevice> &dev, Create create) {
    k.ver = ver; k.geo = geo;
    std::vector<uint8_t> c1, c2, c3;
    if (ver == 1) { c1 = b1eo::build_cs_reduce(1, geo); c2 = b1eo::build_cs_reduce(2, geo); c3 = b1eo::build_cs_nanfree(); }
    else { c1 = b1eo::build_cs_l1_gather(geo); c2 = b1eo::build_cs_l2_lds(geo); c3 = b1eo::build_cs_nanfree_wide();
      DxvkSamplerKey sk = { };
      sk.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
      sk.setLodRange(0.0f, 0.0f, 0.0f); sk.setAniso(0u);
      sk.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
      sk.setDepthCompare(false, VK_COMPARE_OP_ALWAYS);
      k.smp = dev->createSampler(sk); }
    k.cs1 = create(c1); k.cs2 = create(c2); k.cs3 = create(c3);
    return k.cs1 != nullptr && k.cs2 != nullptr && k.cs3 != nullptr && (ver == 1 || k.smp != nullptr); }

  /* dirty texel rect (inclusive, non-empty, inside the atlas) -> dispatch parameters */
  static Plan plan(const Kernels &k, const Maps &m, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    Plan p; const int32_t sh = (int32_t)k.geo.shift, sp = (int32_t)k.geo.span;
    int32_t bx0 = x0 >> sh, by0 = y0 >> sh, bx1 = x1 >> sh, by1 = y1 >> sh;
    int32_t cx0 = std::max(0, bx0 - (sp - 1)), cy0 = std::max(0, by0 - (sp - 1));
    float iw = 1.0f / (float)m.w, ih = 1.0f / (float)m.h; int32_t biw, bih; std::memcpy(&biw, &iw, 4); std::memcpy(&bih, &ih, 4);
    p.p1 = { { bx0, by0, bx1, by1, (int32_t)m.w - 1, (int32_t)m.h - 1, k.ver == 1 ? 0 : biw, k.ver == 1 ? 0 : bih } };
    p.p2 = { { cx0, cy0, bx1, by1, (int32_t)m.bw - 1, (int32_t)m.bh - 1, 0, 0 } };
    p.g1x = (bx1 - bx0 + 8) / 8; p.g1y = (by1 - by0 + 8) / 8; p.g2x = (bx1 - cx0 + 8) / 8; p.g2y = (by1 - cy0 + 8) / 8;
    int32_t nx = ((int32_t)m.bw + sp - 1) / sp, ny = ((int32_t)m.bh + sp - 1) / sp;
    if (k.ver == 1) { p.p3 = { { nx * ny, nx, sp, 0, (int32_t)m.bw - 1, (int32_t)m.bh - 1, 0, 0 } }; p.g3x = 1; p.g3y = 1; }
    else { p.p3 = { { nx, ny, sp, 0, (int32_t)m.bw - 1, (int32_t)m.bh - 1, 0, 0 } }; p.g3x = (nx + 15) / 16; p.g3y = (ny + 15) / 16; }
    p.l1blocks = (uint64_t)(bx1 - bx0 + 1) * (by1 - by0 + 1); p.texels = p.l1blocks << (2 * sh); p.l2texels = (uint64_t)(bx1 - cx0 + 1) * (by1 - cy0 + 1);
    return p; }

  /* record one build. Slots: CS cb13, t127, u63, u62, s15 (v2). Afterwards they are unbound, except s15, which gets
     `restore` (the game's sampler there, normally none). pb: 768-byte parameter buffer (3 x 256). */
  static void record(DxvkContext *c, const Kernels &k, const Maps &m, const Plan &p, const Rc<DxvkBuffer> &pb, uint32_t uavCompute, const Rc<DxvkSampler> &restore) {
    const uint32_t cbs = 5u * 16u + 13u, srv = 5u * 128u + 127u, uav = uavCompute + 63u, smp = 5u * 16u + 15u;
    if (p.fill) {   /* cleared atlas: every block (and so every L2 texel) is (c, c); the dirty rect is rebuilt below */
      VkClearValue cv = { }; cv.color.uint32[0] = ~p.fillbits; cv.color.uint32[1] = p.fillbits;
      c->clearImageView(m.l1u, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ m.bw, m.bh, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv);
      c->clearImageView(m.l2u, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ m.bw, m.bh, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv); }
    c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(k.cs1));
    c->updateBuffer(pb, 0, sizeof p.p1, &p.p1);
    c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 0, 256));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv, Rc<DxvkImageView>(m.src));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav, Rc<DxvkImageView>(m.l1u));
    if (k.ver != 1) c->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, smp, Rc<DxvkSampler>(k.smp));
    c->dispatch(p.g1x, p.g1y, 1);
    c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(k.cs2));
    c->updateBuffer(pb, 256, sizeof p.p2, &p.p2);
    c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 256, 256));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv, Rc<DxvkImageView>(m.l1s));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav, Rc<DxvkImageView>(m.l2u));
    if (k.ver != 1) c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, uavCompute + 62u, Rc<DxvkBufferView>(m.globu));
    c->dispatch(p.g2x, p.g2y, 1);
    c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(k.cs3));
    if (k.ver == 1) c->clearBuffer(m.glob, 0, 4, 1u);
    c->updateBuffer(pb, 512, sizeof p.p3, &p.p3);
    c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice(pb, 512, 256));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv, Rc<DxvkImageView>(m.l2s));
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav, nullptr);
    c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, uavCompute + 62u, Rc<DxvkBufferView>(m.globu));
    c->dispatch(p.g3x, p.g3y, 1);
    c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, uavCompute + 62u, nullptr);
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv, nullptr);
    c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav, nullptr);
    c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, cbs, DxvkBufferSlice());
    if (k.ver != 1) c->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, smp, Rc<DxvkSampler>(restore)); }
}
