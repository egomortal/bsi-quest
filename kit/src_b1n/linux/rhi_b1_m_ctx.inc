/* rhi_b1_m_ctx.inc: context entry points: shaders, binds, rts, clears, updates, copies, submits, draws (includes rhi_b1_shrep.inc, rhi_b1_dyn.inc). Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
/* ---- context ---- */
/* db103 (chat 5, BSI_B1_DRAWBATCH in rhi_b1_com.c): the C side issues deferred state sets in a burst between
   b1dx_c_begin_batch / b1dx_c_end_batch; g_rmu is held once for the burst and the per-entry guard below does not
   re-lock it on the batching thread. Entries called outside a batch lock exactly as before (t_batch_depth == 0). */
/* b140 (chat 4 fork34): the batching thread is recorded in a global (owner = pthread_self of the thread that holds g_rmu for a
   burst, depth touched only by that owner while it holds g_rmu) instead of a thread_local: the thread_local read in every
   entry went through the dynamic-TLS resolver (__tls_get_addr / tlsdesc_resolver_dynamic) on the Quest. Same rule as
   before: the owner does not re-lock, every other thread locks. Another thread can never read its own id as the owner. */
static std::atomic<uintptr_t> g_batch_owner{0};
static int g_batch_depth;
static inline uintptr_t self_id() { return (uintptr_t)pthread_self(); }
struct RmuGuard {
  bool own;
  RmuGuard() : own(g_batch_owner.load(std::memory_order_relaxed) != self_id()) { if (own) g_rmu.lock(); }
  ~RmuGuard() { if (own) g_rmu.unlock(); }
  RmuGuard(const RmuGuard&) = delete; RmuGuard& operator=(const RmuGuard&) = delete;
};
extern "C" void b1dx_c_begin_batch(void) {
  uintptr_t me = self_id();
  if (g_batch_owner.load(std::memory_order_relaxed) == me) { g_batch_depth++; return; }
  g_rmu.lock(); g_batch_depth = 1; g_batch_owner.store(me, std::memory_order_relaxed);
}
extern "C" void b1dx_c_end_batch(void) {
  if (g_batch_owner.load(std::memory_order_relaxed) != self_id() || g_batch_depth <= 0) return;
  if (--g_batch_depth == 0) { g_batch_owner.store(0, std::memory_order_relaxed); g_rmu.unlock(); }
}
#define CTX_GUARD() if (g_ctx == nullptr) return; RmuGuard lk; try {
#define CTX_END(what) } catch (const DxvkError& e) { cerr(what, e); }

/* diagnostics: dump the current render target 0 after a draw (shadow mode, with the bridge's per-draw dump frame) so
   the two renderers can be compared draw by draw. Conversion as bridge.c dump_tex (half/float: v/(1+v), gamma 2.2). */
static float b1_h2f(uint16_t h){ uint32_t s=(h>>15)&1, e=(h>>10)&31, m=h&1023; uint32_t f;
  if(!e){ if(!m) f=s<<31; else { e=127-15+1; while(!(m&1024)){ m<<=1; e--; } m&=1023; f=(s<<31)|(e<<23)|(m<<13); } }
  else if(e==31) f=(s<<31)|0x7f800000|(m<<13); else f=(s<<31)|((e+112)<<23)|(m<<13); float r; std::memcpy(&r,&f,4); return r; }
static uint8_t b1_tob(float v){ if(!(v>0)) return 0; v=v/(1+v); v=std::pow(v,1/2.2f); return v>=1?255:(uint8_t)(v*255); }
extern "C" int b1dx_dump_rt0(const char *path) {
  if (g_ctx == nullptr) return 0;
  std::lock_guard<RmuLock> lk(g_rmu);
  try {
    View *v = g_views.get(g_cs.rtv[0]); if (!v || v->type != 2) return 0;
    dc88::flush_all(dc88::n_rdbk);   /* dc88 */
    Rc<DxvkImage> img = v->iv->image(); const auto &vi = v->iv->info();
    VkFormat fmt = img->info().format; const DxvkFormatInfo *fi = lookupFormatInfo(fmt);
    if (fi->flags.test(DxvkFormatFlag::BlockCompressed)) return 0;
    VkExtent3D ext = img->mipLevelExtent(vi.mipIndex); uint32_t es = fi->elementSize;
    VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * es;
    DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 rt readback";
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageSubresourceLayers sl = { fi->aspectMask, vi.mipIndex, vi.layerIndex, 1u };
    emit([rb, fmt, img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ ext.width, ext.height, 1u }); });
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    FILE *f = std::fopen(path, "wb"); if (!f) return 0;
    std::fprintf(f, "P6\n%u %u\n255\n", ext.width, ext.height);
    const uint8_t *p = (const uint8_t *)rb->mapPtr(0); std::vector<uint8_t> row(ext.width * 3);
    for (uint32_t y = 0; y < ext.height; y++) { const uint8_t *r = p + (size_t)y * ext.width * es;
      for (uint32_t x = 0; x < ext.width; x++) { uint8_t *o = &row[3 * x]; const uint8_t *q = r + (size_t)x * es;
        switch (fmt) {
          case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; break;
          case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0]=q[2]; o[1]=q[1]; o[2]=q[0]; break;
          case VK_FORMAT_R16G16B16A16_SFLOAT: { const uint16_t *h=(const uint16_t*)q; o[0]=b1_tob(b1_h2f(h[0])); o[1]=b1_tob(b1_h2f(h[1])); o[2]=b1_tob(b1_h2f(h[2])); break; }
          case VK_FORMAT_R32G32B32A32_SFLOAT: { const float *h=(const float*)q; o[0]=b1_tob(h[0]); o[1]=b1_tob(h[1]); o[2]=b1_tob(h[2]); break; }
          case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { uint32_t w; std::memcpy(&w,q,4); uint32_t rr=w&2047,gg=(w>>11)&2047,bb=w>>22;
            o[0]=b1_tob(b1_h2f((uint16_t)(rr<<4))); o[1]=b1_tob(b1_h2f((uint16_t)(gg<<4))); o[2]=b1_tob(b1_h2f((uint16_t)(bb<<5))); break; }
          case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t w; std::memcpy(&w,q,4); o[0]=(w&1023)>>2; o[1]=((w>>10)&1023)>>2; o[2]=((w>>20)&1023)>>2; break; }
          case VK_FORMAT_R32_SFLOAT: { float h; std::memcpy(&h,q,4); o[0]=o[1]=o[2]=b1_tob(h); break; }
          case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16_SFLOAT: { const uint16_t *h=(const uint16_t*)q; o[0]=o[1]=o[2]=b1_tob(b1_h2f(h[0])); break; }
          default: o[0]=o[1]=o[2]=q[0]; } }
      std::fwrite(row.data(), 1, row.size(), f); }
    std::fclose(f);
    static int nl; if (nl++ < 4) std::fprintf(stderr, "[b1] per-draw dump %s (%ux%u fmt %d)\n", path, ext.width, ext.height, (int)fmt);
    return 1;
  } catch (const DxvkError& e) { std::fprintf(stderr, "[b1] per-draw dump failed: %s\n", e.message().c_str()); }
  return 0;
}
extern "C" void b1dx_c_shader(int st, uint32_t sh) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  if (st < 0 || st > 5) return;
  if (st == 5) g_cur_cs = sh;
  if (st == 0) g_m79_vs = sh; else if (st == 1) g_m79_ps = sh; else if (st == 5) g_m79_cs = sh;   /* m79 profiler keys */
  dc88::sh[st] = sh;   /* dc88 */
  if (st == 1) b1vr::ps_id = sh;   /* ao74 diagnostics (BSI_B1_VRPASS) */
  if (st == 0) b1vr::vs_id = sh;   /* pp76 diagnostics */
  if (st == 1 && (b1vr::on || b1vr::fxd_on > 0)) {   /* pp76: kind of pixel shader (light shafts, tone map, sun shadow projection, FXAA) */
    /* c5b141 HOOKGATE: ps_set is the only writer of b1vr::cur_pk / fx_ps and a pure function of the shader at id sh: the same sh with no
       shader released since (ids are re-created only after a release) stores the same values again */
    static uint32_t m_sh = 0, m_rel = ~0u;
    if (!g_c5b_gate || sh != m_sh || g_c5b_shrel != m_rel) { b1vr::ps_set(sh); m_sh = sh; m_rel = g_c5b_shrel; } }
  if (st == 1) {   /* c5b141 HOOKGATE: see c5b_eo_ps_noop (end of rhi_b1_dxvk.cpp) */
    static uint32_t e_sh = 0, e_rel = ~0u;
    if (!g_c5b_gate || sh != e_sh || g_c5b_shrel != e_rel || !c5b_eo_ps_noop(sh)) { b1eo_hook_ps_set(sh); e_sh = sh; e_rel = g_c5b_shrel; } }
  if (shsig::on > 0 && st < 2) shsig::sh[st] = sh;   /* perf73 diagnostic */
  if (bc_same(g_bc.sh[st], sh)) { if (!bc_verify()) return; Rc<DxvkShader> c = sh ? b1dx_shader_get(sh) : nullptr; if (bc_keep(g_bp.sh[st], c.ptr())) return; }
  if (c5_raw()) { DxvkShader *p = sh ? c5_shader_ptr(sh) : nullptr; g_bp.sh[st] = p; c5_bind_shader(st, p); } else {   /* c5 */
  Rc<DxvkShader> s = sh ? b1dx_shader_get(sh) : nullptr;
  g_bp.sh[st] = s.ptr();
  emit([st, s = std::move(s)] (DxvkContext* c) mutable {
    switch (st) {
      case 0: c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(s)); break;
      case 1: c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(s)); break;
      case 2: c->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(std::move(s)); break;
      case 3: c->bindShader<VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT>(std::move(s)); break;
      case 4: c->bindShader<VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT>(std::move(s)); break;
      case 5: c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(s)); break;
      default: break;
    } }); }
  if (st == 1) b1eo_hook_ps_bound_orig();
  CTX_END("bind shader")
}
/* ---- compute (13 Oct, build 60): b1 had no compute at all, so in b1n the game's HDAO (and every other Dispatch) never ran
   and the AO texture stayed at its initial value. Bindings as DXVK's D3D11: UAV u# -> UavIndexCompute + #, counters unused. ---- */
namespace { constexpr uint32_t kSrvTotal = 128u * 6u, kUavPerPipeline = 64u, kUavCompute = kSrvTotal + kUavPerPipeline * 2u; long g_ndisp, g_nuavclr, g_ngenmips, g_ndispalt; }
static bool b1ab_hook_dispatch(uint32_t x, uint32_t y, uint32_t z); static void b1ab_hook_uav(uint32_t slot, View *v);   /* rhi_b1_aoblur.inc */
static bool b1ab_is_blur(uint32_t cs);   /* rhi_b1_aoblur.inc (vr70) */
static bool b1ab_last_emitted();         /* rhi_b1_aoblur.inc (ao74) */
extern "C" void b1dx_c_uavs(uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  for (uint32_t i = 0; i < n && start + i < kUavPerPipeline; i++) {
    View *v = g_views.get(ids[i]); Rc<DxvkImageView> iv = (v && v->type == 4) ? v->iv : nullptr;
    dc88::set_slot(dc88::uav_res[start + i], (v && v->type == 4) ? v->res : 0); if (v && v->type == 4) dc88::flush_res(v->res, dc88::n_srv);   /* dc88 */
    s2_uav(start + i, (v && v->type == 4) ? v : nullptr); b1ab_hook_uav(start + i, (v && v->type == 4) ? v : nullptr);
    if (b1vr::on) b1vr::uav(start + i, v);   /* vr70: AO crop */
    if (start + i == 0 && b1vr::aodump_n > 0) b1vr::uav0_any = iv;   /* ao74 diagnostics */
    if (c5_raw()) c5_bind_uav(kUavCompute + start + i, kUavCompute + kUavPerPipeline + start + i, iv.ptr()); else   /* c5 */
    emit([slot = kUavCompute + start + i, ctr = kUavCompute + kUavPerPipeline + start + i, iv = std::move(iv)] (DxvkContext* c) mutable {
      c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, slot, std::move(iv)); c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, ctr, nullptr); });
  }
  CTX_END("bind UAVs")
}
static bool b1dx_dispatch_body(uint32_t x, uint32_t y, uint32_t z);
extern "C" void b1dx_c_dispatch(uint32_t x, uint32_t y, uint32_t z) {
  CTX_GUARD()
  if (!x || !y || !z) return;
  g4_flush_if_open("compute");   /* g4 */
  dc88::bound_flush();   /* dc88: the compute pass may sample a pending target */
  bool emitted = b1dx_dispatch_body(x, y, z);
  bep(3); bep(4); bep(5);   /* c5b141 epoch: AO switch / blur twins / per-eye AO rebind compute shader, constants, views */
  if (b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::ao_diag(emitted, x, y, b1ab_is_blur(g_cur_cs));   /* ao74 diagnostics (cloud) */
  CTX_END("dispatch")
}
/* (g_rmu held) false = not dispatched (ao_eye skip) */
static bool b1dx_dispatch_body(uint32_t x, uint32_t y, uint32_t z) {
  g_ndisp++;
  if (b1vr::on) { b1vr::disp_x = x; b1vr::disp_log(x, y, z, g_cur_cs, b1ab_is_blur(g_cur_cs), false); }   /* vr71 */
  if (b1vr::crop_m && b1ab_is_blur(g_cur_cs)) x = b1vr::crop_x(x, y);   /* vr70: AO crop (stereo layout with the UI column; the HDAO blurs only) */
  if (b1ab_hook_dispatch(x, y, z)) return b1ab_last_emitted();   /* V4 HDAO blur twin (rhi_b1_aoblur.inc) */
  uint32_t sc[3] = { 1, 1, 1 }; bool gtao = false; Rc<DxvkShader> alt = b1dx_shader_alt2(g_cur_cs, sc, &gtao);
  if (gp::on()) gp::on_dispatch(x, y, alt != nullptr);
  tl126::on_dispatch();   /* b126 slack csN */
  if (alt != nullptr) {   /* AO switch: the replacement for this Dispatch, then the game's shader back (as the bridge's DXVK) */
    g_ndispalt++; Rc<DxvkShader> norm = b1dx_shader_get(g_cur_cs);
    if (b1vr::eyeb) { int k = b1vr::ao_kind(norm, b1ab_is_blur(g_cur_cs));   /* vr71: ao_eye (left eye view's AO passes are dead work) */
      if (k == 1 && b1vr::eyeb == 2) { if (b1vr::eye2(x, y, z, alt, norm, sc, gtao)) { dyn103_foreign(); return false; } }   /* ao74: AO per eye */
      else if (k >= 0 && b1vr::eye_emit(x, y, k, k ? "AO switch main" : "AO switch blur")) return false; }
    emit([alt = std::move(alt), norm = std::move(norm), x = x * sc[0], y = y * sc[1], z = z * sc[2]] (DxvkContext* c) mutable {
      c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(alt)); c->dispatch(x, y, z); c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(norm)); });
  } else {
    if (b1vr::eyeb) { int k = b1vr::ao_kind(b1dx_shader_get(g_cur_cs), b1ab_is_blur(g_cur_cs));   /* vr71: ao_eye */
      if (k == 1 && b1vr::eyeb == 2) { if (b1vr::eye2(x, y, z, nullptr, nullptr, sc, false)) { dyn103_foreign(); return false; } }   /* ao74: AO per eye */
      else if (k >= 0 && b1vr::eye_emit(x, y, k, k ? "HDAO main" : "AO blur")) return false; }
    emit([x, y, z] (DxvkContext* c) { c->dispatch(x, y, z); }); }
  return true;
}
extern "C" void b1dx_c_clear_uav(uint32_t view, const uint32_t *vals, int isfloat) {
  CTX_GUARD()
  View *v = g_views.get(view); if (!v || v->type != 4 || !vals) return;
  g4_flush_if_open("compute");   /* g4 */
  const DxvkFormatInfo *fi = v->iv->formatInfo(); bool intfmt = fi->flags.any(DxvkFormatFlag::SampledSInt, DxvkFormatFlag::SampledUInt);
  if (isfloat && intfmt) return;
  VkClearValue cv = { };
  if (isfloat || !intfmt) { for (int i = 0; i < 4; i++) { float f; if (isfloat) std::memcpy(&f, &vals[i], 4); else f = (float)vals[i]; cv.color.float32[i] = f; } }
  else for (int i = 0; i < 4; i++) cv.color.uint32[i] = vals[i];
  g_nuavclr++; dc88::flush_res(v->res, dc88::n_other); /* dc88 */ s2_before_write(v->res, false);
  if (gp::on() && m79::clears) { VkExtent3D e = v->iv->mipLevelExtent(0); char b[64]; std::snprintf(b, sizeof b, "clear uav %s %ux%u", gp::fmtname(v->iv->info().format), e.width, e.height); gp::on_clear(b); }   /* es86 */
  emit([iv = v->iv, cv] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { 0, 0, 0 }, iv->mipLevelExtent(0), VK_IMAGE_ASPECT_COLOR_BIT, cv); });
  CTX_END("clear UAV")
}
extern "C" void b1dx_c_genmips(uint32_t srv) {
  CTX_GUARD()
  View *v = g_views.get(srv); if (!v || v->type != 1 || v->iv->info().mipCount < 2) return;
  g4_flush_if_open("compute");   /* g4 */
  g_ngenmips++; dc88::flush_res(v->res, dc88::n_other); /* dc88 */ s2_before_read(v->res); s2_before_write(v->res, false);
  emit([iv = v->iv] (DxvkContext* c) { c->generateMipmaps(iv, VK_FILTER_LINEAR); });
  CTX_END("generate mips")
}
extern "C" void b1dx_compute_stats(long *disp, long *clr, long *gm) { *disp = g_ndisp; *clr = g_nuavclr; *gm = g_ngenmips; }
extern "C" long b1dx_compute_alt_count(void) { return g_ndispalt; }
extern "C" void b1dx_c_cbs(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  if (shsig::on > 0 && st < 2) for (uint32_t i = 0; i < n && start + i < 4; i++) shsig::cb[st][start + i] = ids[i];   /* perf73 diagnostic */
  if (st == 5 && start == 0 && n) { b1vr::cs_cb0 = ids[0]; if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_bound(ids[0]); }   /* ao74: AO constants (rhi_b1_vr.inc) */
  if (st == 1 && start == 0 && n) { b1vr::ps_cb0 = ids[0]; if (b1vr::pass_armed || b1vr::pass_n > 0) b1vr::cb_bound(ids[0]); }   /* ao74 diagnostics */
  if ((st == 0 || st == 1) && b1vr::on) for (uint32_t i = 0; i < n; i++) b1vr::cb_slot(st, start + i, ids[i]);   /* pp76: bound VS/PS constant buffers */
  if (st == 0) for (uint32_t i = 0; i < n; i++) sb87::vs_cb_bound(start + i, ids[i]);   /* sb87: VS cb1 (VSOffsetConstants) gets a CPU copy */
  if ((st == 0 || st == 1) && (b1vr::pass_armed || b1vr::pass_n > 0)) for (uint32_t i = 0; i < n && start + i < 4; i++) {   /* pp76 diagnostics */
    (st ? b1vr::ps_cb : b1vr::vs_cb)[start + i] = ids[i]; b1vr::cb_bound(ids[i]); }
  bool c5r = c5_raw();   /* c5 */
  const bool fast = g_c5b_fast && st >= 0 && st < 6;   /* c5b141 ENTRYFAST */
  for (uint32_t i = 0; i < n && start + i < 14; i++) {
    /* c5b141 ENTRYFAST: the cache holds ids[i] in this slot -> the object is still the live buffer it was when cached (every release
       forgets its id) and the key computed below would be ids[i]: bc_same skips. id 0: key 0, the same test. */
    if (fast && g_bc.cb[st][start + i] == ids[i]) { g_bc_skip++; continue; }
    Obj *o = robj(ids[i]); DxvkBufferSlice sl; uint32_t key = 0;
    if (o && o->kind == 1) { if (!c5r) sl = DxvkBufferSlice(o->buf); key = ids[i]; }
    if (st >= 0 && st < 6) { const void *pp = key ? (const void *)o->buf.ptr() : nullptr;
      if (bc_same(g_bc.cb[st][start + i], key) && (!bc_verify() || bc_keep(g_bp.cb[st][start + i], pp))) continue;
      g_bp.cb[st][start + i] = pp; }
    if (dyn103_bind_cb(o, key, st, start + i)) continue;   /* dyn103: the id's current ring slice */
    if (c5r) c5_bind_cb(sbit(st), sidx(st) * 16 + start + i, key ? o->buf.ptr() : nullptr); else   /* c5 */
    emit([sb = sbit(st), slot = sidx(st) * 16 + start + i, sl = std::move(sl)] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, slot, std::move(sl)); });
  }
  CTX_END("bind constant buffers")
}
extern "C" void b1dx_c_srvs(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  uint32_t g4tmp[128]; if (n > 128) n = 128; ids = g4_remap_srvs(ids, n, g4tmp);   /* g4: shadow atlas -> ring slot */
  if (st == 1 && (b1vr::pass_n > 0 || b1vr::pass_armed)) for (uint32_t i = 0; i < n && start + i < 8; i++) b1vr::ps_srv[start + i] = ids[i];   /* ao74 diagnostics (pp76: 8 slots) */
  for (uint32_t i = 0; i < n && start + i < 128; i++) {
    View *v = g_views.get(ids[i]); bool ok = v && v->type == 1;
    if (st >= 0 && st < 6) { dc88::set_slot(dc88::srv_res[st][start + i], ok ? v->res : 0); if (ok) dc88::flush_res(v->res, dc88::n_srv); }   /* dc88: the resource is readable from now on (also when the bind cache skips the bind) */
    if (ok && texlog_on()) { Obj *o = robj(v->res); if (o && o->kind == 2) { const auto &vi = v->iv->info(); uint32_t need = 0;
        for (uint32_t m = vi.mipIndex; m < vi.mipIndex + vi.mipCount && m < 32; m++) need |= 1u << m;
        if ((o->wmask & need) != need) { static uint32_t seen[64]; static int ns; int k; for (k = 0; k < ns; k++) if (seen[k] == v->res) break;
          if (k == ns && ns < 64) { seen[ns++] = v->res; VkExtent3D e = o->img->mipLevelExtent(0);
            std::fprintf(stderr, "[b1tex] SAMPLED UNWRITTEN: res %u %ux%u fmt %d mips %u written %08x view mips %u+%u\n", v->res, e.width, e.height, (int)o->fmt, o->img->info().mipLevels, o->wmask, vi.mipIndex, vi.mipCount); } } } }
    if (st >= 0 && st < 6) { const void *pp = ok ? (const void *)v->iv.ptr() : nullptr;
      if (bc_same(g_bc.srv[st][start + i], ok ? ids[i] : 0) && (!bc_verify() || bc_keep(g_bp.srv[st][start + i], pp))) continue;
      g_bp.srv[st][start + i] = pp; }
    bool c5r = c5_raw() && c5_srv_plain(st, start + i);   /* c5: no s2 alias possible in this slot -> the table's own view, raw (iv kept only for the pp76 PS slots 0-4 below) */
    Rc<DxvkImageView> iv = c5r ? (ok && st == 1 && start + i < 5 ? v->iv : Rc<DxvkImageView>()) : s2_srv(st, start + i, ids[i], ok ? v : nullptr, ok ? v->iv : nullptr);
    if (st == 1 && start + i == 4 && b1vr::on) { b1vr::t4_res = ok ? v->res : 0; b1vr::t4_iv = iv; }   /* pp76: light shafts per eye */
    if (st == 1 && start + i == 0 && (b1vr::on || b1vr::fxd_on > 0)) { b1vr::t0_res = ok ? v->res : 0; b1vr::t0_iv = iv; }   /* pp76: FXAA per eye / diagnostics */
    if (st == 1 && start + i < 4 && b1vr::on) b1vr::ps_tres[start + i] = ok ? v->res : 0;   /* pp76: bloom per eye */
    if (c5r) c5_bind_srv(sbit(st), sidx(st) * 128 + start + i, ok ? v->iv.ptr() : nullptr); else   /* c5 */
    emit([sb = sbit(st), slot = sidx(st) * 128 + start + i, iv = std::move(iv)] (DxvkContext* c) mutable { c->bindResourceImageView(sb, slot, std::move(iv)); });
  }
  CTX_END("bind SRVs")
}
extern "C" void b1dx_c_samplers(int st, uint32_t start, uint32_t n, const uint32_t *ids) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  const bool fast = g_c5b_fast && st >= 0 && st < 6;   /* c5b141 ENTRYFAST (as in b1dx_c_cbs: a cached id is a live sampler) */
  for (uint32_t i = 0; i < n && start + i < 16; i++) {
    if (fast && g_bc.ss[st][start + i] == ids[i]) { g_bc_skip++; continue; }
    SS *s = g_sss.get(ids[i]);
    if (st >= 0 && st < 6) { const void *pp = s ? (const void *)s->s.ptr() : nullptr;
      if (bc_same(g_bc.ss[st][start + i], s ? ids[i] : 0) && (!bc_verify() || bc_keep(g_bp.ss[st][start + i], pp))) continue;
      g_bp.ss[st][start + i] = pp; }
    if (c5_raw()) { c5_bind_sampler(sbit(st), sidx(st) * 16 + start + i, s ? s->s.ptr() : nullptr); continue; }   /* c5 */
    Rc<DxvkSampler> sp = s ? s->s : nullptr;
    emit([sb = sbit(st), slot = sidx(st) * 16 + start + i, sp = std::move(sp)] (DxvkContext* c) mutable { c->bindResourceSampler(sb, slot, std::move(sp)); });
  }
  CTX_END("bind samplers")
}
extern "C" void b1dx_c_il(uint32_t id) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  if (g_c5b_fast && g_bc.il == id) { g_m79_il = id; if (shsig::on > 0) shsig::il = id; g_bc_skip++; return; }   /* c5b141 ENTRYFAST: a cached id is a live layout (id 0: none) */
  IL *il = g_ils.get(id);
  g_m79_il = il ? id : 0;   /* m79 census */
  if (shsig::on > 0) shsig::il = id;   /* perf73 diagnostic */
  if (bc_same(g_bc.il, il ? id : 0)) return;
  if (il) emit_data<DxvkVertexInput>(il->in, il->na + il->nb, [na = il->na, nb = il->nb] (DxvkContext* c, const DxvkVertexInput* in, size_t) { c->setInputLayout(na, in, nb, in + na); });
  else emit([] (DxvkContext* c) { c->setInputLayout(0, nullptr, 0, nullptr); });
  CTX_END("input layout")
}
extern "C" void b1dx_c_vbs(uint32_t start, uint32_t n, const uint32_t *ids, const uint32_t *strides, const uint32_t *offsets) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  for (uint32_t i = 0; i < n && start + i < 32; i++) {
    uint32_t s = start + i;
    if (shsig::on > 0 && s < 4) { shsig::vb[s] = ids[i]; shsig::vbs[s] = strides[i]; shsig::vbo[s] = offsets[i]; }   /* perf73 diagnostic */
    /* c5b141 ENTRYFAST: a cached (id, stride, offset) was cached with offset < size of the then live buffer, which the id still names
       (releases forget it): the key / stride / offset below would equal the cache. id 0 (or invalid): key, stride, offset all 0. */
    if (g_c5b_fast && g_bc.vb[s] == ids[i] && (ids[i] ? g_bc.vbs[s] == strides[i] && g_bc.vbo[s] == offsets[i] : g_bc.vbs[s] == 0 && g_bc.vbo[s] == 0)) { g_bc_skip++; continue; }
    Obj *o = robj(ids[i]);
    bool ok = o && o->kind == 1 && offsets[i] < o->buf->info().size;
    uint32_t key = ok ? ids[i] : 0, kst = ok ? strides[i] : 0, kof = ok ? offsets[i] : 0;
    const void *pp = ok ? (const void *)o->buf.ptr() : nullptr;
    if (g_bc.vbs[s] == kst && g_bc.vbo[s] == kof && bc_same(g_bc.vb[s], key) && (!bc_verify() || bc_keep(g_bp.vb[s], pp))) continue;
    g_bc.vb[s] = key; g_bc.vbs[s] = kst; g_bc.vbo[s] = kof; g_bp.vb[s] = pp;
    if (dyn103_bind_vb(o, ok, s, kof, kst)) continue;   /* dyn103 */
    if (c5_raw()) { c5_bind_vb(s, ok ? o->buf.ptr() : nullptr, kof, ok ? o->buf->info().size - offsets[i] : 0, kst); continue; }   /* c5 */
    DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, offsets[i], o->buf->info().size - offsets[i]) : DxvkBufferSlice();
    emit([s, sl = std::move(sl), str = ok ? strides[i] : 0u] (DxvkContext* c) mutable { c->bindVertexBuffer(s, std::move(sl), str); });
  }
  CTX_END("vertex buffers")
}
extern "C" void b1dx_c_ib(uint32_t id, uint32_t fmt, uint32_t off) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  VkIndexType t = fmt == 42 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  g_m79_ibt = fmt == 42 ? 32 : 16;   /* m79 census */
  if (shsig::on > 0) { shsig::ib = id; shsig::ibo = off; shsig::ibf = fmt; }   /* perf73 diagnostic */
  if (g_c5b_fast && g_bc.ib == id && g_bc.ibf == (uint32_t)t && g_bc.ibo == (id ? off : 0u)) { g_bc_skip++; return; }   /* c5b141 ENTRYFAST (as b1dx_c_vbs) */
  Obj *o = robj(id);
  bool ok = o && o->kind == 1 && off < o->buf->info().size;
  uint32_t key = ok ? id : 0, kf = (uint32_t)t, ko = ok ? off : 0;
  const void *pp = ok ? (const void *)o->buf.ptr() : nullptr;
  if (g_bc.ibf == kf && g_bc.ibo == ko && bc_same(g_bc.ib, key) && (!bc_verify() || bc_keep(g_bp.ib, pp))) return;
  g_bc.ib = key; g_bc.ibf = kf; g_bc.ibo = ko; g_bp.ib = pp;
  if (dyn103_bind_ib(o, ok, ko, t)) return;   /* dyn103 */
  if (c5_raw()) { c5_bind_ib(ok ? o->buf.ptr() : nullptr, ko, ok ? o->buf->info().size - off : 0, t); return; }   /* c5 */
  DxvkBufferSlice sl = ok ? DxvkBufferSlice(o->buf, off, o->buf->info().size - off) : DxvkBufferSlice();
  emit([sl = std::move(sl), t] (DxvkContext* c) mutable { c->bindIndexBuffer(std::move(sl), t); });
  CTX_END("index buffer")
}
extern "C" void b1dx_c_topology(uint32_t t) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  VkPrimitiveTopology vt; g_m79_topo = t;   /* m79 census */
  switch (t) { case 1: vt = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break; case 2: vt = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break; case 3: vt = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
               case 5: vt = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break; default: vt = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break; }
  if (bc_same(g_bc.topo, (uint32_t)vt)) return;
  emit([vt] (DxvkContext* c) { c->setInputAssemblyState(DxvkInputAssemblyState(vt, false)); });
  CTX_END("topology")
}
extern "C" void b1dx_c_rs(uint32_t id) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  RS *rs = g_rss.get(id); RS *old = g_rss.get(g_cs.rs);
  if (bc_same(g_bc.rs, rs ? id : 0)) { g_cs.rs = id; return; }
  emit([s = rs ? rs->s : default_rs(), b = rs ? rs->b : DxvkDepthBias()] (DxvkContext* c) { c->setRasterizerState(s); c->setDepthBias(b); });
  if ((old && old->scissor) != (rs && rs->scissor)) g_cs.vpdirty = true;
  g_cs.rs = id;
  CTX_END("rasterizer state")
}
extern "C" void b1dx_c_viewports(uint32_t n, const float *v) {
  if (g_ctx == nullptr) return; RmuGuard lk;
  uint32_t nn = n > 16 ? 16 : n;
  if (bc_on() && !g_cs.vpdirty && nn == g_cs.nvp && !std::memcmp(g_cs.vp, v, nn * 24)) { g_bc_skip++; return; }   /* unchanged */
  g_cs.nvp = nn; std::memcpy(g_cs.vp, v, g_cs.nvp * 24); g_cs.vpdirty = true;
}
extern "C" void b1dx_c_scissors(uint32_t n, const int32_t *r) {
  if (g_ctx == nullptr) return; RmuGuard lk;
  uint32_t nn = n > 16 ? 16 : n;
  if (bc_on() && !g_cs.vpdirty && nn == g_cs.nsc && !std::memcmp(g_cs.sc, r, nn * 16)) { g_bc_skip++; return; }
  g_cs.nsc = nn; std::memcpy(g_cs.sc, r, g_cs.nsc * 16); g_cs.vpdirty = true;
}
extern "C" void b1dx_c_blend(uint32_t id, const float *factor, uint32_t mask) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  BS *bs = g_bss.get(id);
  float f[4] = { 1, 1, 1, 1 }; if (factor) std::memcpy(f, factor, 16);
  if (b1vr::alpha) b1vr::note_blend(id, f, mask);   /* vr69 */
  if (bc_on() && g_bc.bs == (bs ? id : 0) && g_bc.bsmask == mask && !std::memcmp(g_bc.bf, f, 16)) { g_bc_skip++; g_cs.bs = id; return; }
  g_bc.bs = bs ? id : 0; g_bc.bsmask = mask; std::memcpy(g_bc.bf, f, 16); g_bc_bind++;
  struct BlendCmd { DxvkMultisampleState ms; DxvkBlendMode m[8]; DxvkLogicOpState lo; DxvkBlendConstants bc; } bcmd;
  DxvkMultisampleState &ms = bcmd.ms; ms = { }; ms.setSampleMask((uint16_t)mask); ms.setAlphaToCoverage(bs && bs->a2c);
  for (uint32_t i = 0; i < 8; i++) {
    DxvkBlendMode m = { };
    if (bs) m = bs->m[i];
    else { m.setBlendEnable(false); m.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
           m.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); m.setWriteMask(0xf); m.normalize(); }
    bcmd.m[i] = m;
  }
  if (b1vr::alpha && b1vr::bbrt) b1vr::cover(bcmd.m[0]);   /* vr69: UI column coverage alpha */
  DxvkLogicOpState lo = { }; lo.setLogicOp(false, VK_LOGIC_OP_NO_OP);
  bcmd.lo = bs ? bs->lo : lo; bcmd.bc = DxvkBlendConstants{ f[0], f[1], f[2], f[3] };
  emit([bcmd] (DxvkContext* c) {
    for (uint32_t i = 0; i < 8; i++) c->setBlendMode(i, bcmd.m[i]);
    c->setMultisampleState(bcmd.ms); c->setLogicOpState(bcmd.lo); c->setBlendConstants(bcmd.bc); });
  g_cs.bs = id;
  CTX_END("blend state")
}
extern "C" void b1dx_c_dss(uint32_t id, uint32_t ref) {
  CTX_GUARD()
  G4_DEFER()   /* g4 */
  DSS *d = g_dsss.get(id);
  if (bc_on() && g_bc.dss == (d ? id : 0) && g_bc.dssref == ref) { g_bc_skip++; g_cs.dss = id; g_cs.sref = ref; return; }
  g_bc.dss = d ? id : 0; g_bc.dssref = ref; g_bc_bind++;
  DxvkDepthStencilState ds = { };
  if (d) ds = d->s;
  else { ds.setDepthTest(true); ds.setDepthWrite(true); ds.setDepthCompareOp(VK_COMPARE_OP_LESS); ds.normalize(); }
  emit([ds, ref] (DxvkContext* c) { c->setDepthStencilState(ds); c->setStencilReference(ref & 0xffu); }); s2_dss(ds, ref);
  g_cs.dss = id; g_cs.sref = ref;
  CTX_END("depth-stencil state")
}
#include "rhi_b1_shrep.inc"   /* b115: shadow-map repeat counter (BSI_B1_SHREP=1) */
#include "rhi_b1_sheye.inc"   /* b123: shared-eye shadow CHECK (BSI_B1_SHEYE=2) */
extern "C" void b1dx_c_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv) {
  CTX_GUARD()
  if (g_flush_pending) { if (gp::on() && m79::clears) gp::on_clear("submit (deferred flush at a target change)");   /* b115: clears=1 also times the submit gap */
    ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }   /* render pass boundary (build 64) */
  else if (flush_adapt() && g_draws_since_flush >= flush_adapt() && g_subf != nullptr) {   /* b118 adaptive submit */
    if (g_subf->value() >= g_subn) { if (gp::on() && m79::clears) gp::on_clear("submit (adaptive: GPU idle)"); ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_nflush_adapt++; }
    else g_nflush_adapt_busy++; }
  { View *dv = g_views.get(dsv); if (dv && dv->type == 3) dc88::flush_res(dv->res, dc88::n_other); }   /* dc88 (generic) */
  g4_rts(n, rtvs, &dsv); G4Defer g4_defer_scope(true); shrep::on_rts(n, rtvs, dsv);   /* b115 */ sheye::on_rts(n, rtvs, dsv);   /* b123 */   /* g4: sequence open/close, atlas -> ring slot; the bind below is deferred while the mask is bound */
  if (g_eo_any) b1eo_hook_rts(dsv);
  shsig::rts(n, rtvs, dsv);   /* perf73 diagnostic (off: one test) */
  DxvkRenderTargets rts = { };
  for (uint32_t i = 0; i < 8; i++) { g_cs.rtv[i] = i < n ? rtvs[i] : 0; View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) rts.color[i].view = v->iv; }
  g_cs.dsv = dsv; { View *v = g_views.get(dsv); if (v && v->type == 3) rts.depth.view = v->iv; }
  if (gp::on()) gp::set_targets(rts);
  if (tl126::sl_on()) { char lb[96]; tl126::rt_label(rts, lb, sizeof lb); tl126::on_rts(lb); }   /* b126 slack rtN */
  if (pp::on()) { uint64_t k = 1469598103934665603ull; double mp = 0;
    for (uint32_t i = 0; i < 8; i++) { uintptr_t q = rts.color[i].view != nullptr ? (uintptr_t)rts.color[i].view->image() : 0; k = (k ^ q) * 1099511628211ull;
                                        if (q) { VkExtent3D e = rts.color[i].view->mipLevelExtent(0); mp += e.width * (double)e.height / 1e6; } }
    if (rts.depth.view != nullptr) { k = (k ^ (uintptr_t)rts.depth.view->image()) * 1099511628211ull; VkExtent3D e = rts.depth.view->mipLevelExtent(0); mp += e.width * (double)e.height / 1e6; }
    pp::rtkey = k; pp::rtmpix = mp; }
  if (c5_raw()) c5_bind_rts(rts); else   /* c5 */
  emit([rts = std::move(rts)] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(rts), 0u); });
  g_nrt++;
  if (b1vr::on || b1vr::bbrt) { b1vr::rts(g_cs.rtv[0]); bep(7); }   /* vr69 (c5b141 epoch: may re-issue the blend state) */
  es86::rts();   /* es86: eye-column scissor limit for this target set */
  CTX_END("render targets")
}
extern "C" void b1dx_c_clear_rtv(uint32_t id, const float *rgba) {
  CTX_GUARD()
  View *v = g_views.get(id); if (!v || v->type != 2) return;
  if (b1vr::pass_n > 0 && (b1vr::on || b1vr::pass_flat) && b1vr::n_frames >= b1vr::pass_from && b1vr::n_frames < b1vr::pass_to)   /* pp76 diagnostics */
    std::fprintf(stderr, "[b1vr] pass: frame %ld after draw %ld clear res %u to %g %g %g %g\n", b1vr::n_frames, b1vr::pass_draw, v->res, rgba[0], rgba[1], rgba[2], rgba[3]);
  s2_clear_rtv(v);
  VkClearValue cv = { }; std::memcpy(cv.color.float32, rgba, 16);
  if (gp::on() && m79::clears) { VkExtent3D e = v->iv->mipLevelExtent(0); char b[64]; std::snprintf(b, sizeof b, "clear %s %ux%u", gp::fmtname(v->iv->info().format), e.width, e.height); gp::on_clear(b); }   /* es86 */
  int es_cl = es86::clears() ? es86::limit_for(v) : 0;   /* es86: clear the eye area + margin only */
  DxvkAttachment a = { }; a.view = v->iv; if (!g4_clear(v->iv, VK_IMAGE_ASPECT_COLOR_BIT, cv)) { /* g4 */
    if (es_cl > 0) es86::n_cl_sc++; else es86::n_cl_whole++;   /* es86 (counted as requested, dc88 may elide the clear) */
    if (dc88::record(v, id, cv, es_cl)) { }   /* dc88: deferred; emitted (as below) before anything can observe the target, or dropped at a full-cover draw */
    else if (es_cl > 0) { emit([iv = v->iv, cv, es_cl] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { 0, 0, 0 }, VkExtent3D { (uint32_t)es_cl, iv->mipLevelExtent(0).height, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv); }); }   /* es86 */
    else { if (c5_raw()) c5_clear_rt(v->iv.ptr(), VK_IMAGE_ASPECT_COLOR_BIT, cv); else /* c5 */ emit([a, cv] (DxvkContext* c) { c->clearRenderTarget(a, VK_IMAGE_ASPECT_COLOR_BIT, cv, 0u); }); } } g_nclear++; pp::clr = 1;
  CTX_END("clear RTV")
}
extern "C" void b1dx_c_clear_dsv(uint32_t id, uint32_t flags, float depth, uint32_t stencil) {
  CTX_GUARD()
  id = g4_remap_dsv(id);   /* g4: shadow atlas -> ring slot */
  View *v = g_views.get(id); if (!v || v->type != 3) return;
  if (shsig::on > 0) shsig::clear(id, flags, depth, stencil);   /* perf73 diagnostic */
  VkImageAspectFlags have = v->iv->info().aspects, asp = 0;
  if (flags & 1) asp |= have & VK_IMAGE_ASPECT_DEPTH_BIT;
  if (flags & 2) asp |= have & VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!asp) return;
  dc88::flush_res(v->res, dc88::n_other);   /* dc88 (generic: a pending colour clear of the same image) */ shrep::on_clear_dsv(v->res, flags, depth, stencil);   /* b115 */
  s2_clear_dsv(v, asp, stencil);
  if (g_eo_any && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) b1eo_hook_written(v->res);
  VkClearValue cv = { }; cv.depthStencil.depth = depth; cv.depthStencil.stencil = stencil & 0xffu;
  if (gp::on() && m79::clears) { VkExtent3D e = v->iv->mipLevelExtent(0); char b[64];   /* es86 */
    std::snprintf(b, sizeof b, "clear %s %ux%u", asp == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : asp == VK_IMAGE_ASPECT_STENCIL_BIT ? "stencil-only" : "depth+stencil", e.width, e.height); gp::on_clear(b); }
  int es_cl = es86::clears() ? es86::limit_for(v) : 0;   /* es86 */
  sb87::clear_dsv(v, asp, stencil, es_cl);   /* sb87: stencil region reset (whole, value 0) / partial / poisoned */
  DxvkAttachment a = { }; a.view = v->iv; if (!g4_clear(v->iv, asp, cv)) { /* g4 */
    if (es_cl > 0) { es86::n_cl_sc++; emit([iv = v->iv, asp, cv, es_cl] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { 0, 0, 0 }, VkExtent3D { (uint32_t)es_cl, iv->mipLevelExtent(0).height, 1u }, asp, cv); }); }   /* es86 */
    else { es86::n_cl_whole++; if (c5_raw()) c5_clear_rt(v->iv.ptr(), asp, cv); else /* c5 */ emit([a, asp, cv] (DxvkContext* c) { c->clearRenderTarget(a, asp, cv, 0u); }); } } g_nclear++; pp::clr = 1;
  CTX_END("clear DSV")
}
#include "rhi_b1_dyn.inc"   /* dyn103: persistent host-visible ring for dynamic-buffer DISCARDs (BSI_B1_DYNRING) */
/* data written to a resource: UpdateSubresource (box: left,top,front,right,bottom,back or null) and Unmap of a write map */
static void write_buffer(Obj &o, uint32_t off, uint32_t size, const void *data, bool discard) {
  VkDeviceSize bsz = o.buf->info().size;
  if (off >= bsz) return;
  if (off + size > bsz) size = (uint32_t)(bsz - off);
  g4_wrote((uint32_t)(&o - g_objs.data()) + 1, discard && off == 0 && size == bsz && (o.buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 1 : 2);   /* g4 */
  if (discard && off == 0 && size == bsz && (o.buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
    if (dyn103_discard(o, (uint32_t)(&o - g_objs.data()) + 1, data, size)) { g_cbytes += size; return; }   /* dyn103: ring slice, one memcpy, binds re-emitted */
    if (g_rawcap) c5_bury_dyn(o); /* c5: the replaced storage may still be held raw by a queued invalidateBuffer */
    o.dyn = dyn_storage(*o.buf); std::memcpy(o.dyn->mapPtr(), data, size);
    if (c5_raw()) c5_invalidate(o.buf.ptr(), o.dyn.ptr()); else   /* c5 */
    emit([b = o.buf, s = o.dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(s)); });
  } else if (dyn103_write(o, off, size, data)) {   /* dyn103: ordered partial write into the ring slice */
  } else if (size <= 65536u && !(size & 3u) && !(off & 3u)) {
    /* CS: the bytes go into the chunk (<= 8 KB) or a heap copy (emit_data) */
    if (c5_raw()) c5_update_buffer(o.buf.ptr(), off, (const uint8_t *)data, size); else   /* c5 */
    emit_data<uint8_t>((const uint8_t *)data, size, [b = o.buf, off] (DxvkContext* c, const uint8_t *d, size_t n) { c->updateBuffer(b, off, n, d); });
  } else {
    Stage st = stage_alloc(size); std::memcpy(st.p, data, size);
    if (c5_raw()) c5_copy_buffer(o.buf.ptr(), off, st.b, st.o, size); else {   /* c5 */
    emit([b = o.buf, off, sb = st.b, so = st.o, size] (DxvkContext* c) { c->copyBuffer(b, off, sb, so, size); }); } up_maybe_flush(size);
  }
  g_cbytes += size;
}
static int img_info(uint32_t id, uint32_t *mips, bool *is3d) {
  std::lock_guard<RmuLock> lk(g_rmu); Obj *o = robj(id); if (!o || o->kind != 2) return 0;
  *mips = o->img->info().mipLevels; *is3d = o->img->info().type == VK_IMAGE_TYPE_3D; return 1;
}
static void tex_data(uint32_t id, uint32_t sub, const uint32_t *box, const void *data, uint32_t rowp, uint32_t slicep) {
  uint32_t mips; bool is3d; if (!img_info(id, &mips, &is3d)) return;
  { std::lock_guard<RmuLock> lk(g_rmu); g4_touch_image(id); }   /* g4 */
  uint32_t mip = sub % mips, layer = sub / mips;
  if (is3d) { if (!box) b1dx_tex3d_write(id, mip, data, rowp, slicep); return; }
  if (box) b1dx_tex_write_rect(id, mip, layer, box[0], box[1], box[3] - box[0], box[4] - box[1], data, rowp);
  else b1dx_tex_write_rect(id, mip, layer, 0, 0, 0, 0, data, rowp);
}
extern "C" void b1dx_c_update(uint32_t id, uint32_t sub, const uint32_t *box, const void *data, uint32_t rowpitch, uint32_t slicepitch) {
  if (!data) return;
  { std::lock_guard<RmuLock> lk(g_rmu); Obj *o = robj(id); if (!o) return;
    if (o->kind == 1) { if (shsig::on > 0) shsig::wrote(id, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data);   /* perf73 diagnostic */
                        if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data);   /* ao74 */
                        sb87::wrote(id, o, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data);   /* sb87 */
                        if (!g_c5b_gate || shrep::on()) shrep::wrote(id, box ? box[0] : 0, box ? box[3] - box[0] : (uint32_t)o->buf->info().size, data, (uint32_t)o->buf->info().size);   /* b115 (c5b141 HOOKGATE: it returns at !on()) */
                        try { if (box) write_buffer(*o, box[0], box[3] - box[0], data, false); else write_buffer(*o, 0, (uint32_t)o->buf->info().size, data, true); g_nupd++; }
                        catch (const DxvkError& e) { cerr("update buffer", e); } return; } }
  if (!g_c5b_gate || shrep::on()) shrep::tex_written(id); /* b115 (c5b141) */ tex_data(id, sub, box, data, rowpitch, slicepitch); g_nupd++;
}
extern "C" void b1dx_c_unmap(uint32_t id, uint32_t sub, const void *data, uint32_t rowpitch, uint32_t slicepitch, int discard) {
  if (!data) return;
  { std::lock_guard<RmuLock> lk(g_rmu); Obj *o = robj(id); if (!o) return;
    if (o->kind == 1) { if (shsig::on > 0) shsig::wrote(id, 0, (uint32_t)o->buf->info().size, data);   /* perf73 diagnostic */
                        if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, 0, (uint32_t)o->buf->info().size, data);   /* ao74 */
                        sb87::wrote(id, o, 0, (uint32_t)o->buf->info().size, data);   /* sb87 */ if (!g_c5b_gate || shrep::on()) shrep::wrote(id, 0, (uint32_t)o->buf->info().size, data, (uint32_t)o->buf->info().size);   /* b115 (c5b141) */
                        if (g_aa_live) b1aa_vbwrite(id, 0, (uint32_t)o->buf->info().size, data);   /* aa76 */
                        try { write_buffer(*o, 0, (uint32_t)o->buf->info().size, data, discard != 0); g_nmapw++; } catch (const DxvkError& e) { cerr("unmap buffer", e); } return; } }
  if (!g_c5b_gate || shrep::on()) shrep::tex_written(id); /* b115 (c5b141) */ tex_data(id, sub, 0, data, rowpitch, slicepitch); g_nmapw++;
}
extern "C" void b1dx_c_copy_region(uint32_t dst, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint32_t src, uint32_t ssub, const uint32_t *box) {
  CTX_GUARD()
  if (b1vr::pass_n > 0 && (b1vr::on || b1vr::pass_flat) && b1vr::n_frames >= b1vr::pass_from && b1vr::n_frames < b1vr::pass_to)   /* pp76 diagnostics */
    std::fprintf(stderr, "[b1vr] pass: frame %ld after draw %ld copy res %u -> res %u at %u,%u box %d %d %d %d\n", b1vr::n_frames, b1vr::pass_draw, src, dst, x, y,
                 box ? (int)box[0] : -1, box ? (int)box[1] : -1, box ? (int)box[3] : -1, box ? (int)box[4] : -1);
  Obj *d = robj(dst), *s = robj(src); if (!d || !s || d->kind != s->kind) return;
  if (d->kind == 2) { dc88::flush_res(src, dc88::n_copy); dc88::flush_res(dst, dc88::n_copy); }   /* dc88: the copy reads src and (partially) overwrites dst */
  if (d->kind == 1) g4_wrote(dst, 2); else g4_copy_image(dst, src);   /* g4 */
  if (d->kind == 1) sb87::buf_copied(dst); else sb87::poison(dst, sb87::P_COPY);   /* sb87: unknown bytes / stencil */
  if (d->kind == 1) shrep::gpu_wrote(dst); else shrep::tex_written(dst);   /* b115 */
  if (d->kind == 1) {
    uint32_t so = box ? box[0] : 0, sz = box ? box[3] - box[0] : (uint32_t)s->buf->info().size;
    if (so + sz <= s->buf->info().size && x + sz <= d->buf->info().size) { if (dyn103_copy(d, x, s, so, sz)) { } else /* dyn103: either side in the ring */ if (c5_raw()) c5_copy_buffer2(d->buf.ptr(), x, s->buf.ptr(), so, sz); else /* c5 */ emit([db = d->buf, x, sb = s->buf, so, sz] (DxvkContext* c) { c->copyBuffer(db, x, sb, so, sz); }); }
  } else {
    uint32_t dm = d->img->info().mipLevels, sm = s->img->info().mipLevels;
    VkImageAspectFlags da = lookupFormatInfo(d->fmt)->aspectMask, sa = lookupFormatInfo(s->fmt)->aspectMask;
    VkImageSubresourceLayers dl = { da, dsub % dm, dsub / dm, 1u }, sl = { sa, ssub % sm, ssub / sm, 1u };
    VkExtent3D se = s->img->mipLevelExtent(sl.mipLevel), de = d->img->mipLevelExtent(dl.mipLevel);
    bool d3 = s->img->info().type == VK_IMAGE_TYPE_3D;
    if (d3) { dl.baseArrayLayer = 0; sl.baseArrayLayer = 0; dl.mipLevel = dsub % dm; sl.mipLevel = ssub % sm; }
    VkOffset3D so = { 0, 0, 0 }; VkExtent3D ext = se;
    if (box) { so = { (int32_t)box[0], (int32_t)box[1], d3 ? (int32_t)box[2] : 0 }; ext = { box[3] - box[0], box[4] - box[1], d3 ? box[5] - box[2] : 1u }; }
    if (x + ext.width > de.width) ext.width = de.width > x ? de.width - x : 0;
    if (y + ext.height > de.height) ext.height = de.height > y ? de.height - y : 0;
    if (!ext.width || !ext.height || d->fmt != s->fmt) { if (d->fmt != s->fmt && g_ncerr++ < 20) std::fprintf(stderr, "[b1] copy between formats %d -> %d skipped\n", s->fmt, d->fmt); return; }
    if (d3 && z + ext.depth > de.depth) ext.depth = de.depth > z ? de.depth - z : 0;
    if (!ext.depth) return;
    { static int nl; if (texlog_on() && nl < texlog_on() && ext.width >= 64) { nl++; std::fprintf(stderr, "[b1tex] copy %u(m%u l%u) -> %u(m%u l%u) at %u,%u ext %ux%u fmt %d src %ux%u dst %ux%u mips %u->%u box %d\n", src, sl.mipLevel, sl.baseArrayLayer, dst, dl.mipLevel, dl.baseArrayLayer, x, y, ext.width, ext.height, (int)d->fmt, se.width, se.height, de.width, de.height, sm, dm, box ? 1 : 0); } }
    if (dl.mipLevel < 32) d->wmask |= 1u << dl.mipLevel;
    if (g_eo_any) b1eo_hook_written(dst);
    if (gp::on() && m79::clears) { char b[64]; std::snprintf(b, sizeof b, "copy %ux%u %s", ext.width, ext.height, gp::fmtname(d->fmt)); gp::on_clear(b); }   /* es86 */
    if (!s2_copy(dst, dsub, x, y, z, src, ssub, box)) {
    if (!d3 && sl.mipLevel == 0 && dl.mipLevel == 0 && (int32_t)x == so.x && (int32_t)y == so.y) { uint32_t nw = copytrim_w(se, de, so.x, ext.width); if (nw != ext.width) { if (!nw) { g_ncopy++; return; } ext.width = nw; } }   /* b119 */
    if (c5_raw()) c5_copy_image(d->img.ptr(), dl, VkOffset3D{ (int32_t)x, (int32_t)y, d3 ? (int32_t)z : 0 }, s->img.ptr(), sl, so, ext); else   /* c5 */
    emit([di = d->img, dl, doff = VkOffset3D{ (int32_t)x, (int32_t)y, d3 ? (int32_t)z : 0 }, si = s->img, sl, so, ext] (DxvkContext* c) { c->copyImage(di, dl, doff, si, sl, so, ext); }); }
  }
  g_ncopy++;
  CTX_END("copy region")
}
/* implicit flush (as DXVK's D3D11 ConsiderFlush): submit every BSI_B1_FLUSH draws (default 256) so the GPU starts on the
   frame while the render thread is still recording; UE3 waits on last frame's occlusion results, so a single submit at
   Present serialised CPU and GPU. */
static inline void pre_draw() {
  if (!g4_cap) g4_pre_draw_live();   /* g4: pending ring-slot copy before the first draw of a redirected atlas pass */
  if (g_eo_any) b1eo_hook_draw();
  s2_pre_draw();
  if (es86::lim && !b1vr::on) { es86::lim = 0; es86::rtk = 0; g_cs.vpdirty = true; }   /* es86: the stereo layout went off (frame-latched) before a new target bind */
  if (g_cs.vpdirty) apply_viewports();
  if (b1vr::on) { b1vr::draw();   /* vr69: which columns of the stereo back buffer this frame draws into */
    es86::count_draw(); }   /* es86 */
  else if (b1vr::pass_flat && b1vr::pass_n) b1vr::pass_diag();   /* pp76 diagnostics: pass log in flat frames (eye-alone reference) */
  if (b1vr::fxd_on > 0) b1vr::fxd_pre();   /* pp76: FXAA / settings diagnostics (flat and VR) */
  if (gp::on()) gp::on_draw();
  tl126::vis_draw(g_m79_vs, g_m79_ps, gp::a_count);   /* b126 [b1vis] */
  if (m79::census) { m79::draws_il[g_m79_il]++; m79::draws_topo[g_m79_topo]++; m79::ndraws++; }   /* m79 census */
  if (g_flush_every < 0) { const char *e = std::getenv("BSI_B1_FLUSH"); g_flush_every = e ? std::atoi(e) : 1024; }   /* b117: 256 -> 1024 (each mid-frame submit left the GPU idle ~0.67 ms; j0342-j0348: -0.3..-1.6 ms at Fair / Gondola / Rooftops / Cab ride, exact) */
  if (g_flush_every > 0 && ++g_draws_since_flush >= g_flush_every) {
    /* build 64: on tilers (Turnip) a submit ends the render pass, so every attachment is written out of tile memory and
       read back in (4 G-buffer targets + depth at 1600x900: ~100 MB of traffic, ~1-2 ms). Like DXVK's D3D11 on tilers
       (NotifyRenderPassBoundary), the flush waits for the next render-target change; a very long pass still flushes at 8x. */
    if (tiler_flush() && g_draws_since_flush < 8 * g_flush_every) { if (!g_flush_pending) { g_flush_pending = 1; g_nflush_deferred++; } }
    else { if (gp::on() && m79::clears) gp::on_clear("submit (immediate flush)");   /* b115 */ ctx_flush(); g_staged = 0; g_draws_since_flush = 0; g_flush_pending = 0; }
  }
}
/* aa76 (rhi_b1_aa.inc): opt-in VR anti-aliasing (vr_stereo.txt aa=); g_aa_live is false with aa=off (default) */
static bool b1aa_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic);
static void b1aa_frame_end(); static void b1aa_frame_begin();
extern "C" void b1dx_c_draw(uint32_t count, uint32_t inst, uint32_t first, uint32_t firstinst) {
  CTX_GUARD()
  VkDrawIndirectCommand dc = { count, inst, first, firstinst };
  gp::a_count = count; gp::a_inst = inst; gp::a_indexed = 0;   /* b111 */
  dc88::on_draw_pre();   /* dc88: image-reading side paths of this draw get the pending clears first */
  if (g_aa_live && b1aa_draw(&dc, nullptr)) { bep_all(); return; }   /* c5b141 epoch */   /* aa76 (merge b76: before pre_draw, then pp76) */
  G4_DEFER() if (g4_cap) g4_draw_pre();   /* g4 */
  pre_draw();
  dc88::on_draw_post(&dc, nullptr);   /* dc88: pending clears of the bound targets: elided, prefix-covered, or emitted here, before the draw */ if (!g_c5b_gate || shrep::on()) shrep::on_draw(&dc, nullptr);   /* b115 (c5b141) */
  sb87::draw_pre(&dc, nullptr);   /* sb87 */
  if (sb87::skip_draw) { sb87::draw_post(); return; }   /* b92: sb87 mode 4 probe skips the stencil volumes (inside the CTX try block) */
  if (m79::census) { m79::vtx_sum += (double)count * (inst ? inst : 1); m79::draws_ibt[0]++; }   /* m79 census */
  if (sb87::st_override) bep(7);   /* c5b141 epoch: sb87 emitted its own depth-stencil state */
  if (sb87::clr_take()) { sb87::clr_emit(); bep_all(); } else if (sb87::tile_pending()) { sb87::tile_emit(); dyn103_foreign(); bep_all(); } else if (b1vr::pp_replace) { b1vr::pp_replace_draw(&dc, nullptr); dyn103_foreign(); bep_all(); } else emit([dc] (DxvkContext* c) { c->draw(1, &dc); }); g_ndraw++;   /* c5b141 epoch: foreign binds */   /* dyn103: those restore o->buf binds */   /* b102 tiled quad; pp76: FXAA per eye */
  if (gp::on()) gp::on_draw_done();   /* b111 */
  sb87::draw_post();   /* sb87 */
  if (b1vr::on && b1vr::pp_post_wanted()) { b1vr::pp_post(&dc, nullptr); dyn103_foreign(); bep_all(); }   /* c5b141 epoch */   /* pp76: right-eye replay / t4 back */
  if (b1vr::fxd.post) { dc88::flush_all(dc88::n_rdbk); b1vr::fxd_post(); }   /* pp76: FXAA check (diagnostics, 3 frames per session; dc88: reads rt0) */
  if (shsig::on > 0) { uint32_t a[5] = { 1, count, inst, first, firstinst }; shsig::draw(a, 5); }   /* perf73 diagnostic */
  if (g_eo_any) b1eo_hook_post_draw(&dc, nullptr);
  CTX_END("draw")
}
extern "C" void b1dx_c_draw_indexed(uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t firstinst) {
  CTX_GUARD()
  VkDrawIndexedIndirectCommand dc = { count, inst, first, base, firstinst };
  gp::a_count = count; gp::a_inst = inst; gp::a_indexed = 1;   /* b111 */
  dc88::on_draw_pre();   /* dc88 */
  if (g_aa_live && b1aa_draw(nullptr, &dc)) { bep_all(); return; }   /* c5b141 epoch */   /* aa76 (merge b76: before pre_draw, then pp76) */
  G4_DEFER() if (g4_cap) g4_draw_pre();   /* g4 */
  pre_draw();
  dc88::on_draw_post(nullptr, &dc);   /* dc88 */ if (!g_c5b_gate || shrep::on()) shrep::on_draw(nullptr, &dc);   /* b115 (c5b141) */
  sb87::draw_pre(nullptr, &dc);   /* sb87 */
  if (sb87::skip_draw) { sb87::draw_post(); return; }   /* b92: sb87 mode 4 probe skips the stencil volumes (inside the CTX try block) */
  if (m79::census) { m79::idx_sum += (double)count * (inst ? inst : 1); m79::draws_ibt[g_m79_ibt]++; }   /* m79 census */
  if (sb87::st_override) bep(7);   /* c5b141 epoch */
  if (sb87::clr_take()) { sb87::clr_emit(); bep_all(); } else if (sb87::tile_pending()) { sb87::tile_emit(); dyn103_foreign(); bep_all(); } else if (b1vr::pp_replace) { b1vr::pp_replace_draw(nullptr, &dc); dyn103_foreign(); bep_all(); } else emit([dc] (DxvkContext* c) { c->drawIndexed(1, &dc); }); g_ndraw++;   /* c5b141 epoch */   /* dyn103: those restore o->buf binds */   /* b102 tiled quad; pp76: FXAA per eye */
  if (gp::on()) gp::on_draw_done();   /* b111 */
  sb87::draw_post();   /* sb87 */
  if (b1vr::on && b1vr::pp_post_wanted()) { b1vr::pp_post(nullptr, &dc); dyn103_foreign(); bep_all(); }   /* c5b141 epoch */   /* pp76: right-eye replay / t4 back */
  if (b1vr::fxd.post) { dc88::flush_all(dc88::n_rdbk); b1vr::fxd_post(); }   /* pp76: FXAA check (diagnostics, 3 frames per session; dc88: reads rt0) */
  if (shsig::on > 0) { uint32_t a[6] = { 2, count, inst, first, (uint32_t)base, firstinst }; shsig::draw(a, 6); }   /* perf73 diagnostic */
  if (g_eo_any) b1eo_hook_post_draw(nullptr, &dc);
  CTX_END("draw indexed")
}
/* dynamic buffers (COM seam): Map(DISCARD) renames the storage; the bytes a draw actually uses are then copied straight
   into the current storage (host-visible, never in use by the GPU by D3D11's DISCARD / NO_OVERWRITE rules). */
extern "C" int b1dx_c_map_discard(uint32_t id) {
  if (g_ctx == nullptr) return 0; std::lock_guard<RmuLock> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1 || !(o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 0;
  try { g4_wrote(id, 1); /* g4 */ if (dyn103_discard(*o, id, nullptr, 0)) return 1; /* dyn103: a ring slice, filled by the per-draw bufwrites */
        if (g_rawcap) c5_bury_dyn(*o); /* c5 */ o->dyn = dyn_storage(*o->buf);
        if (c5_raw()) c5_invalidate(o->buf.ptr(), o->dyn.ptr()); else /* c5 */ emit([b = o->buf, a = o->dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); }); return 1; }
  catch (const DxvkError& e) { cerr("map discard", e); }
  return 0;
}
extern "C" uint32_t b1dx_buf_info(uint32_t id, int *hostvis) {
  std::lock_guard<RmuLock> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1) return 0;
  if (hostvis) *hostvis = (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && !(o->dxgi & 0x4u);   /* constant buffers: whole write at Unmap */
  return (uint32_t)o->buf->info().size;
}
extern "C" void b1dx_c_bufwrite(uint32_t id, uint32_t off, uint32_t size, const void *data) {
  if (g_ctx == nullptr || !data || !size) return; std::lock_guard<RmuLock> lk(g_rmu);
  Obj *o = robj(id); if (!o || o->kind != 1) return;
  VkDeviceSize bsz = o->buf->info().size; if (off >= bsz) return; if (off + size > bsz) size = (uint32_t)(bsz - off);
  try {
    g4_wrote(id, (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 0 : 2);   /* g4 */
    if (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      if (dyn103_bufwrite(*o, id, off, size, data)) { } else {   /* dyn103: straight into the current ring slice */
      if (o->dyn == nullptr) { o->dyn = dyn_storage(*o->buf); if (c5_raw()) c5_invalidate(o->buf.ptr(), o->dyn.ptr()); else /* c5 */ emit([b = o->buf, a = o->dyn] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); }); }
      std::memcpy((uint8_t *)o->dyn->mapPtr() + off, data, size); }   /* dyn103: end of the old path */
    } else write_buffer(*o, off, size, data, false);
    if (shsig::on > 0) shsig::wrote(id, off ? off : 1, size, data);   /* perf73 diagnostic (dynamic VB/IB ranges) */
    if (g_aa_live) b1aa_vbwrite(id, off, size, data);   /* aa76 */
    if (b1vr::on || b1vr::aocb_n > 0 || b1vr::aodump_n > 0) b1vr::cb_wrote(id, o, off, size, data);   /* ao74 */
    sb87::wrote(id, o, off, size, data);   /* sb87 */ if (!g_c5b_gate || shrep::on()) shrep::wrote(id, off, size, data, (uint32_t)bsz);   /* b115 (c5b141) */
    g_cbytes += size; g_nmapw++;
  } catch (const DxvkError& e) { cerr("buffer write", e); }
}
/* m79 census: resident images / buffers by format and size (under g_rmu), draws by vertex layout / index type / topology
   since the last report. Formats are VkFormat numbers (vertex attributes "format@binding"). */
static void m79_census() {
  struct T { long n = 0; double mb = 0; };
  std::map<std::string, T> tex, rt, buf; long nimg = 0, nb = 0; double mbimg = 0, mbbuf = 0;
  { std::lock_guard<RmuLock> lk(g_rmu);
    for (auto &o : g_objs) {
      if (o.kind == 2 && o.img != nullptr) {
        const DxvkImageCreateInfo &ci = o.img->info(); const DxvkFormatInfo *fi = lookupFormatInfo(ci.format);
        double bytes = 0; for (uint32_t m = 0; m < ci.mipLevels; m++) { VkExtent3D e = o.img->mipLevelExtent(m);
          double bw = fi ? (double)((e.width + fi->blockSize.width - 1) / fi->blockSize.width) : e.width, bh = fi ? (double)((e.height + fi->blockSize.height - 1) / fi->blockSize.height) : e.height;
          bytes += bw * bh * e.depth * (fi ? fi->elementSize : 4); }
        bytes *= ci.numLayers;
        uint32_t mx = std::max(ci.extent.width, ci.extent.height); int bucket = mx <= 64 ? 64 : mx <= 128 ? 128 : mx <= 256 ? 256 : mx <= 512 ? 512 : mx <= 1024 ? 1024 : mx <= 2048 ? 2048 : 4096;
        bool att = ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        std::string k = "f" + std::to_string((int)ci.format) + (att ? "" : "/" + std::to_string(bucket)) + (ci.type == VK_IMAGE_TYPE_3D ? "/3d" : "") + (ci.numLayers > 1 ? "/L" + std::to_string(ci.numLayers) : "");
        if (att) k += "/" + std::to_string(ci.extent.width) + "x" + std::to_string(ci.extent.height);
        T &t = (att ? rt : tex)[k]; t.n++; t.mb += bytes / 1048576.0; nimg++; mbimg += bytes / 1048576.0;
      } else if (o.kind == 1 && o.buf != nullptr) {
        VkBufferUsageFlags u = o.buf->info().usage; std::string k = (u & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ? "vertex" : (u & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ? "index" : (u & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? "constant" : "other";
        if (o.buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) k += "/dynamic";
        T &t = buf[k]; t.n++; t.mb += o.buf->info().size / 1048576.0; nb++; mbbuf += o.buf->info().size / 1048576.0; } } }
  auto line = [] (const char *what, std::map<std::string, T> &m, int top) { std::vector<std::pair<std::string, T>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [] (const auto &a, const auto &b) { return a.second.mb > b.second.mb; }); std::string o; char b[160];
    for (size_t j = 0; j < v.size() && (int)j < top; j++) { std::snprintf(b, sizeof b, " | %s: %ld, %.1f MB", v[j].first.c_str(), v[j].second.n, v[j].second.mb); o += b; }
    std::fprintf(stderr, "[b1cen] %s (%zu kinds)%s\n", what, v.size(), o.c_str()); };
  std::fprintf(stderr, "[b1cen] resident: %ld images %.1f MB, %ld buffers %.1f MB | draws %ld, indices/frame-window %.0f, vertices (non-indexed) %.0f\n", nimg, mbimg, nb, mbbuf, m79::ndraws, m79::idx_sum, m79::vtx_sum);
  line("textures by format/size (VkFormat/max side)", tex, 60); line("render targets (VkFormat/size)", rt, 40); line("buffers", buf, 12);
  { std::vector<std::pair<uint32_t, long>> v(m79::draws_il.begin(), m79::draws_il.end()); std::sort(v.begin(), v.end(), [] (const auto &a, const auto &b) { return a.second > b.second; });
    std::map<std::string, long> by; for (auto &e : v) by[m79::ilsig.count(e.first) ? m79::ilsig[e.first] : std::string("?")] += e.second;
    std::vector<std::pair<std::string, long>> w(by.begin(), by.end()); std::sort(w.begin(), w.end(), [] (const auto &a, const auto &b) { return a.second > b.second; });
    std::string o; char b[200]; for (size_t j = 0; j < w.size() && j < 40; j++) { std::snprintf(b, sizeof b, " | %s: %ld", w[j].first.c_str(), w[j].second); o += b; }
    std::string ib; for (auto &e : m79::draws_ibt) { std::snprintf(b, sizeof b, " idx%u:%ld", e.first, e.second); ib += b; }
    std::string tp; for (auto &e : m79::draws_topo) { std::snprintf(b, sizeof b, " t%u:%ld", e.first, e.second); tp += b; }
    std::fprintf(stderr, "[b1cen] draws by vertex layout (VkFormat@binding, %zu layouts in use) | index type%s | topology%s%s\n", w.size(), ib.c_str(), tp.c_str(), o.c_str()); }
  m79::draws_il.clear(); m79::draws_ibt.clear(); m79::draws_topo.clear(); m79::idx_sum = 0; m79::vtx_sum = 0; m79::ndraws = 0;
}
extern "C" uint32_t b1dx_bb_id(void) { return g_bb; }
extern "C" void b1dx_com_stats(char *out, int n) {
  DxvkStatCounters sc; if (g_dev != nullptr) sc = g_dev->getStatCounters();
  long qp = 0, qf = 0; query_stats(&qp, &qf);
  std::snprintf(out, n, "queries pending-polls %ld flushes %ld | draws %ld, updates %ld, map writes %ld, copies %ld, %.1f MB written, views %zu, ctx errors %ld | dxvk: pipelines %llu, render passes %llu, barriers %llu, submits %llu, gpu syncs %llu",
                qp, qf, g_ndraw, g_nupd, g_nmapw, g_ncopy, g_cbytes / 1048576.0, g_views.v.size() - g_views.fr.size(), g_ncerr,
                (unsigned long long)sc.getCtr(DxvkStatCounter::PipeCountGraphics), (unsigned long long)sc.getCtr(DxvkStatCounter::CmdRenderPassCount),
                (unsigned long long)sc.getCtr(DxvkStatCounter::CmdBarrierCount), (unsigned long long)sc.getCtr(DxvkStatCounter::QueueSubmitCount),
                (unsigned long long)sc.getCtr(DxvkStatCounter::GpuSyncCount));
  size_t l = std::strlen(out);
  if (l + 48 < (size_t)n) std::snprintf(out + l, n - l, " | binds %ld, redundant skipped %ld", g_bc_bind, g_bc_skip);
  if (tiler_flush()) { l = std::strlen(out); if (l + 64 < (size_t)n) std::snprintf(out + l, n - l, ", flushes moved to pass end %ld (uploads %ld)", g_nflush_deferred, g_nupflush_deferred); }
  if (copytrim()) { l = std::strlen(out); if (l + 96 < (size_t)n) std::snprintf(out + l, n - l, ", copies trimmed to the eye columns %ld (%.1f Mpx skipped)", g_ncopytrim, g_copytrim_px / 1e6); }   /* b119 */
  if (flush_adapt()) { l = std::strlen(out); if (l + 96 < (size_t)n) std::snprintf(out + l, n - l, ", adaptive submits %ld (GPU busy, kept %ld; min %d draws)", g_nflush_adapt, g_nflush_adapt_busy, flush_adapt()); }   /* b118 */
  if (g_stage) { l = std::strlen(out); if (l + 48 < (size_t)n) std::snprintf(out + l, n - l, " | upload staging ring: %ld allocs (%ld large)", g_stage_n, g_stage_big); }
  s2_stats();
  if (bc_verify()) { l = std::strlen(out); if (l + 32 < (size_t)n) std::snprintf(out + l, n - l, ", verify mismatches %ld", g_bc_bad); }
  { double tsm; long tsn; ts_stats(&tsm, &tsn); l = std::strlen(out); if (tsn && l + 40 < (size_t)n) std::snprintf(out + l, n - l, " | gpu-ts span %.1f ms/frame", tsm); }
  if (gp::on()) gp::report();
  if (es86::on()) es86::stats();   /* es86 */
  dc88::stats();   /* dc88 */
  if (m79::census) m79_census();   /* m79 */
  if (pp::on()) { std::string t; long tot = 0; for (int i = 0; i < 4096; i++) if (pp::ends[i]) { tot += pp::ends[i]; t += " L" + std::to_string(i) + ":" + std::to_string(pp::ends[i]); }
                  std::fprintf(stderr, "[b1pp] render pass ends %ld by emit line:%s\n", tot, t.c_str());
                  t.clear(); tot = 0; double mp = 0; char tmp[64];
                  for (int i = 0; i < 4096; i++) if (pp::resumed[i]) { tot += pp::resumed[i]; mp += pp::resumed_mpix[i]; std::snprintf(tmp, sizeof tmp, " L%d:%ld(%.0f Mpix)", i, pp::resumed[i], pp::resumed_mpix[i]); t += tmp; }
                  std::fprintf(stderr, "[b1pp] passes resumed on the same targets %ld (%.0f Mpix of attachments; %ld more began with a clear) after:%s\n", tot, mp, pp::fresh, t.c_str()); }
  if (cs_on()) { l = std::strlen(out); if (l + 96 < (size_t)n) std::snprintf(out + l, n - l, " | cs: chunks %ld, syncs %ld (waited %ld, %.1f ms), worker errors %ld",
                                                                            g_cs_chunks, g_cs_syncs, g_cs_waits, g_cs_wait_us / 1000.0, g_cs_err.load()); }
}
