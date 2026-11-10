/* rhi_b1_m_state.inc: B1.5 COM-seam state tables, viewports, es86 eye scissor, dc88 dead-clear elision (includes rhi_b1_vr.inc, rhi_b1_sb87.inc). Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
/* =====================================================================================================================
   B1.5 at the COM seam: a small D3D11 subset implemented directly on the DxvkContext.
   Fed by rhi_b1_com.c from the game's own D3D11 calls (the ~45 device/context methods the UE3 RHI uses), so it needs no
   knowledge of engine structures. Shadow mode: the bridge still renders and presents; we render the same stream
   offscreen. B1.6 swaps the roles. Objects are ids (per type tables); every entry point takes g_rmu.
   ===================================================================================================================== */
extern Rc<DxvkShader> b1dx_shader_get(uint32_t id);   /* rhi_b1_shader.cpp */
extern "C" void b1fp16_stats(char *out, int n);   /* rhi_b1_shader.cpp: b103 fp16 materials */
extern Rc<DxvkShader> b1dx_shader_alt(uint32_t id, uint32_t scale[3]);   /* rhi_b1_shader.cpp: AO switch alternate for this Dispatch */
extern Rc<DxvkShader> b1dx_shader_alt2(uint32_t id, uint32_t scale[3], bool *gtao);   /* ao74: + is it a GTAO build */
static void query_stats(long *pend, long *flush);
static void ts_stats(double *avg_ms, long *frames);

namespace {
  template<class T> struct Table {
    struct E { bool live = false; T t; };
    std::vector<E> v; std::vector<uint32_t> fr;
    uint32_t add(T&& t) {
      if (!fr.empty()) { uint32_t id = fr.back(); fr.pop_back(); v[id - 1].t = std::move(t); v[id - 1].live = true; return id; }
      v.push_back(E()); v.back().t = std::move(t); v.back().live = true; return (uint32_t)v.size(); }
    T *get(uint32_t id) { return (id && id <= v.size() && v[id - 1].live) ? &v[id - 1].t : nullptr; }
    void del(uint32_t id) { if (get(id)) { v[id - 1].t = T(); v[id - 1].live = false; fr.push_back(id); } }
  };
  struct View { int type = 0; uint32_t res = 0; Rc<DxvkImageView> iv; };   /* 1 SRV, 2 RTV, 3 DSV, 4 UAV */
  struct IL   { DxvkVertexInput in[64]; uint32_t na = 0, nb = 0; };
  struct BS   { DxvkBlendMode m[8]; bool a2c = false; DxvkLogicOpState lo; };
  struct DSS  { DxvkDepthStencilState s; };
  struct RS   { DxvkRasterizerState s; DxvkDepthBias b = { }; bool scissor = false; };
  struct SS   { Rc<DxvkSampler> s; };
  Table<View> g_views; Table<IL> g_ils; Table<BS> g_bss; Table<DSS> g_dsss; Table<RS> g_rss; Table<SS> g_sss;

  struct CState {
    float vp[16][6]; uint32_t nvp = 0; int32_t sc[16][4]; uint32_t nsc = 0; bool vpdirty = true;
    uint32_t rs = 0, bs = 0, dss = 0, sref = 0, mask = 0xffffffffu; float bf[4] = { 1, 1, 1, 1 };
    uint32_t rtv[8] = { }, dsv = 0;
  } g_cs;
  /* b111 audit: the bound state of the draw being marked (viewport / scissor 0, depth-stencil, blend of RT0, cull, targets) */
  static void audit_meta(gp::Meta &m) {
    m.topo = (uint8_t)g_m79_topo; m.il = g_m79_il;
    if (g_cs.nvp) { m.vx = (int16_t)g_cs.vp[0][0]; m.vy = (int16_t)g_cs.vp[0][1]; m.vw = (int16_t)g_cs.vp[0][2]; m.vh = (int16_t)g_cs.vp[0][3]; }
    if (g_cs.nsc) { m.sx = (int16_t)g_cs.sc[0][0]; m.sy = (int16_t)g_cs.sc[0][1]; m.sw = (int16_t)g_cs.sc[0][2]; m.shh = (int16_t)g_cs.sc[0][3]; }
    DSS *d = g_dsss.get(g_cs.dss); m.dtest = d ? d->s.depthTest() : 1; m.dwrite = d ? d->s.depthWrite() : 1; m.dfunc = d ? (uint8_t)d->s.depthCompareOp() : 1; m.stest = d ? d->s.stencilTest() : 0;
    BS *b = g_bss.get(g_cs.bs); m.blend = b ? b->m[0].blendEnable() : 0; m.wmask = b ? (uint8_t)b->m[0].writeMask() : 0xf;
    RS *r = g_rss.get(g_cs.rs); m.cull = r ? (uint8_t)r->s.cullMode() : 2;
    for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2 && v->iv != nullptr) { if (!m.ncol) m.rtfmt = (uint32_t)v->iv->info().format; m.ncol++; } }
    { View *v = g_views.get(g_cs.dsv); m.dsv = v && v->type == 3 ? v->res : 0; }
  }
  static struct AuditInit { AuditInit() { gp::meta_fn = audit_meta; } } g_audit_init;
  long g_ndraw, g_ncerr, g_nupd, g_nmapw, g_ncopy;
  uint32_t g_cur_cs;   /* compute shader id bound now (AO switch lookup per Dispatch) */
  uint64_t g_cbytes;

  inline uint32_t sidx(int st) { static const uint32_t m[6] = { 1, 0, 2, 3, 4, 5 }; return st >= 0 && st < 6 ? m[st] : 0; }   /* VS PS GS HS DS CS -> DXVK stage index */
  inline VkShaderStageFlags sbit(int st) {
    static const VkShaderStageFlags m[6] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
      VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT };
    return m[st]; }
  void cerr(const char *what, const DxvkError& e) { if (g_ncerr++ < 20) std::fprintf(stderr, "[b1] %s failed: %s\n", what, e.message().c_str()); }

  /* DXGI -> VkFormat for vertex attributes and views (typed formats only) */
  VkFormat dxgi_typed(uint32_t f) {
    switch (f) {
      case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;  case 3: return VK_FORMAT_R32G32B32A32_UINT;  case 4: return VK_FORMAT_R32G32B32A32_SINT;
      case 6: return VK_FORMAT_R32G32B32_SFLOAT;     case 7: return VK_FORMAT_R32G32B32_UINT;     case 8: return VK_FORMAT_R32G32B32_SINT;
      case 10: return VK_FORMAT_R16G16B16A16_SFLOAT; case 11: return VK_FORMAT_R16G16B16A16_UNORM; case 12: return VK_FORMAT_R16G16B16A16_UINT;
      case 13: return VK_FORMAT_R16G16B16A16_SNORM;  case 14: return VK_FORMAT_R16G16B16A16_SINT;
      case 16: return VK_FORMAT_R32G32_SFLOAT;       case 17: return VK_FORMAT_R32G32_UINT;       case 18: return VK_FORMAT_R32G32_SINT;
      case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 25: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
      case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
      case 28: return VK_FORMAT_R8G8B8A8_UNORM; case 29: return VK_FORMAT_R8G8B8A8_SRGB; case 30: return VK_FORMAT_R8G8B8A8_UINT;
      case 31: return VK_FORMAT_R8G8B8A8_SNORM; case 32: return VK_FORMAT_R8G8B8A8_SINT;
      case 34: return VK_FORMAT_R16G16_SFLOAT;  case 35: return VK_FORMAT_R16G16_UNORM; case 36: return VK_FORMAT_R16G16_UINT;
      case 37: return VK_FORMAT_R16G16_SNORM;   case 38: return VK_FORMAT_R16G16_SINT;
      case 41: return VK_FORMAT_R32_SFLOAT;     case 42: return VK_FORMAT_R32_UINT;     case 43: return VK_FORMAT_R32_SINT;
      case 49: return VK_FORMAT_R8G8_UNORM;     case 50: return VK_FORMAT_R8G8_UINT;    case 51: return VK_FORMAT_R8G8_SNORM; case 52: return VK_FORMAT_R8G8_SINT;
      case 54: return VK_FORMAT_R16_SFLOAT;     case 56: return VK_FORMAT_R16_UNORM;    case 57: return VK_FORMAT_R16_UINT;
      case 58: return VK_FORMAT_R16_SNORM;      case 59: return VK_FORMAT_R16_SINT;
      case 61: return VK_FORMAT_R8_UNORM;       case 62: return VK_FORMAT_R8_UINT;      case 63: return VK_FORMAT_R8_SNORM;   case 64: return VK_FORMAT_R8_SINT;
      case 65: return VK_FORMAT_R8_UNORM;
      case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
      case 74: return VK_FORMAT_BC2_UNORM_BLOCK;      case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
      case 77: return VK_FORMAT_BC3_UNORM_BLOCK;      case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
      case 80: return VK_FORMAT_BC4_UNORM_BLOCK;      case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
      case 83: return VK_FORMAT_BC5_UNORM_BLOCK;      case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
      case 87: case 88: return VK_FORMAT_B8G8R8A8_UNORM; case 91: case 93: return VK_FORMAT_B8G8R8A8_SRGB;
      case 115: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
      default: return VK_FORMAT_UNDEFINED;
    }
  }
  bool is_depth(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM; }

  VkBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
      case 1: return VK_BLEND_FACTOR_ZERO;  case 2: return VK_BLEND_FACTOR_ONE;
      case 3: return VK_BLEND_FACTOR_SRC_COLOR;  case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case 5: return VK_BLEND_FACTOR_SRC_ALPHA;  case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case 7: return VK_BLEND_FACTOR_DST_ALPHA;  case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case 9: return VK_BLEND_FACTOR_DST_COLOR;  case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      case 14: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
      case 15: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      case 16: return VK_BLEND_FACTOR_SRC1_COLOR; case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
      case 18: return VK_BLEND_FACTOR_SRC1_ALPHA; case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
      default: return VK_BLEND_FACTOR_ZERO;
    }
  }
  VkBlendOp blend_op(uint32_t o) {
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
  }
  VkCompareOp cmp_op(uint32_t c) {
    switch (c) { case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS; case 3: return VK_COMPARE_OP_EQUAL; case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
                 case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL; case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS; }
  }
  VkStencilOp stencil_op(uint32_t o) {
    switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                 case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
  }
  VkSamplerAddressMode addr_mode(uint32_t a) {
    switch (a) { case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
  }

  DxvkRasterizerState default_rs() {
    DxvkRasterizerState s = { };
    s.setPolygonMode(VK_POLYGON_MODE_FILL); s.setCullMode(VK_CULL_MODE_BACK_BIT); s.setFrontFace(VK_FRONT_FACE_CLOCKWISE);
    s.setDepthClip(true); s.setConservativeMode(VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT); s.setSampleCount(0);
    s.setFlatShading(false); s.setLineMode(VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT);
    return s;
  }
  /* ---- vr69: VR stereo pieces of the native renderer (inert unless the VR game side reports the stereo layout) ---- */
bool sb87_lrz_mod();   /* b124: used by pp76's FXAA split (defined in rhi_b1_m_s2.inc) */
namespace sb87 { void replay_pre(const float *vs1o); void replay_post(); bool tile_pending(); void tile_emit(); bool clr_take(); void clr_emit(); }   /* sb87 (rhi_b1_sb87.inc, below): used by pp76's right-eye replay; b102 tiled quads */
#include "rhi_b1_vr.inc"
#include "rhi_b1_sb87.inc"   /* sb87: shadow projection scissor / depth bounds from the stencil volumes (BSI_B1_SHBBOX, BSI_B1_SHDB) */
  /* es86: eye-column scissor (BSI_B1_EYESCIS, default 1; g4_notes/EYESCIS.md). Stereo layout [L|R|UI]: nothing displayed
     ever reads the UI column of a back-buffer-sized SCENE target (the UI column shown is the final back buffer's, drawn by
     the game's UI pass), so while such a target is bound every scissor the game sets (and the implicit full-target one) is
     capped at x < 2 * eye_w + margin; the margin covers every neighbourhood filter that reads across the right eye's right
     edge (AO blurs, FXAA, bloom / DOF chains; same 128 px as the AO crop of rhi_b1_vr.inc). Left alone: rects that start in
     the UI column (x0 >= 2 * eye_w: the game's UI viewports / pp76 fx_draw), and every draw while the final back buffer is
     bound (the UI pass; the cover tiles; the per-eye quads of tone map / FXAA cost nothing in the UI column anyway).
     Done where the viewports reach the context, so BSI_B1_EYESCIS=0 is bit-exactly the old command stream.
     Clears of such targets (BSI_B1_EYESCIS_CLEARS, default 0): the rect [0, limit) x [0, h) via clearImageView instead of
     the whole target. Default off: DXVK folds a whole-target clearRenderTarget into the next pass's loadOp CLEAR (free on a
     tiler), while a partial clearImageView of an unbound view is its own render pass (CLEAR + STORE of 2/3) and the next
     pass then LOADs the target: more memory traffic, not less. Measurement switch only. */
  namespace es86 {
    static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_EYESCIS"); v = e && *e ? std::atoi(e) != 0 : 1; } return v; }
    static int clears() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_EYESCIS_CLEARS"); v = e && *e ? std::atoi(e) != 0 : 0; } return v; }
    static int margin() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_EYESCIS_MARGIN"); v = e && *e ? std::atoi(e) : 128; if (v < 0) v = 0; } return v; }
    static int lim;          /* x limit for draws into the bound targets (0 = no clamp) */
    static int lim_applied;  /* the limit apply_viewports last emitted with */
    static int rtk;          /* bound targets: 0 not back-buffer-sized (or off), 1 scene target (lim > 0), 3 the final back buffer */
    static int vpk;          /* apply_viewports' decision for viewport 0: 0 none, 1 clamped, 2 UI rect left alone, 3 final back buffer */
    static long n_clamp, n_ui, n_bb, n_other, n_cl_sc, n_cl_whole; static bool logged;
    static bool bb_sized(const Rc<DxvkImageView> &iv) {
      if (iv == nullptr || b1vr::bb_w <= 0 || b1vr::bb_h <= 0) return false;
      VkExtent3D e = iv->mipLevelExtent(0);
      return (int)e.width >= b1vr::bb_w && (int)e.width <= b1vr::bb_w + 32 && (int)e.height >= b1vr::bb_h && (int)e.height <= b1vr::bb_h + 32; }
    /* the x limit for a view of the given size (0 = none): stereo on, back-buffer-sized, not the final back buffer */
    static int limit_for(const View *v) {
      if (!on() || !b1vr::on || b1vr::eye_w <= 0 || !v || v->iv == nullptr) return 0;
      if (g_bb && v->res == g_bb) return 0;
      if (!bb_sized(v->iv)) return 0;
      int l = 2 * b1vr::eye_w + margin(); if (l >= (int)v->iv->mipLevelExtent(0).width) return 0;
      if (!logged) { logged = true; std::fprintf(stderr, "[b1es] eye scissor: on, eye %d px, limit x < %d (margin %d), clears %s\n", b1vr::eye_w, l, margin(), clears() ? "scissored" : "whole"); }
      return l; }
    /* b1dx_c_rts (g_rmu held, after b1vr::rts): the limit for the new target set; a change re-emits the viewports */
    static void rts() {
      int l = 0, k = 0;
      if (on() && b1vr::on && b1vr::eye_w > 0) {
        bool isbb = false; const View *first = nullptr;
        for (uint32_t i = 0; i < 8; i++) { const View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2 && v->iv != nullptr) { if (g_bb && v->res == g_bb) isbb = true; if (!first) first = v; } }
        if (!first) { const View *v = g_views.get(g_cs.dsv); if (v && v->type == 3 && v->iv != nullptr) first = v; }
        if (isbb) k = 3; else if (first) { l = limit_for(first); k = l ? 1 : 0; }
      }
      lim = l; rtk = k;
      if (lim != lim_applied) g_cs.vpdirty = true; }
    static void count_draw() { switch (vpk) { case 1: n_clamp++; break; case 2: n_ui++; break; case 3: n_bb++; break; default: n_other++; } }
    static void stats() {
      if (!n_clamp && !n_ui && !n_bb && !n_cl_sc && !n_cl_whole) return;
      std::fprintf(stderr, "[b1es] eye scissor (eye %d, limit x < %d): draws clamped %ld, UI rect left alone %ld, final back buffer %ld, not back-buffer-sized %ld; clears scissored %ld, whole %ld\n",
                   b1vr::eye_w, b1vr::eye_w > 0 ? 2 * b1vr::eye_w + margin() : 0, n_clamp, n_ui, n_bb, n_other, n_cl_sc, n_cl_whole);
      n_clamp = n_ui = n_bb = n_other = n_cl_sc = n_cl_whole = 0; }
  }
  void apply_viewports() {
    RS *rs = g_rss.get(g_cs.rs); bool sc = rs && rs->scissor;
    DxvkViewport vps[16]; uint32_t n = g_cs.nvp;
    const int es_lim = es86::lim; es86::lim_applied = es_lim; es86::vpk = es_lim ? 1 : (es86::rtk == 3 ? 3 : 0);   /* es86 */
    if (!n) { DxvkViewport v = { }; emit([v] (DxvkContext* c) { c->setViewports(1, &v); }); g_cs.vpdirty = false; return; }
    for (uint32_t i = 0; i < n; i++) {
      const float *v = g_cs.vp[i]; DxvkViewport &d = vps[i]; d = { };
      d.viewport.x = v[0]; d.viewport.y = v[3] + v[1]; d.viewport.width = v[2]; d.viewport.height = -v[3];
      d.viewport.minDepth = v[4]; d.viewport.maxDepth = v[5];
      if (!sc) { d.scissor.offset = { 0, 0 }; d.scissor.extent = { 16384u, 16384u }; }
      else if (i < g_cs.nsc) { const int32_t *r = g_cs.sc[i];
        d.scissor.offset = { r[0] > 0 ? r[0] : 0, r[1] > 0 ? r[1] : 0 };
        d.scissor.extent = { (uint32_t)(r[2] > r[0] ? r[2] - (r[0] > 0 ? r[0] : 0) : 0), (uint32_t)(r[3] > r[1] ? r[3] - (r[1] > 0 ? r[1] : 0) : 0) }; }
      if (es_lim > 0) {   /* es86: cap x_end at the limit; a rect that starts in the UI column (the game's UI viewport, x >= 2 * eye_w) is left alone */
        float x0 = sc && (float)d.scissor.offset.x > v[0] ? (float)d.scissor.offset.x : v[0];   /* where the draw's pixels start: max(viewport x, scissor x) */
        if (x0 >= (float)(2 * b1vr::eye_w) - 1.5f) { if (i == 0) es86::vpk = 2; }
        else if ((int64_t)d.scissor.offset.x + (int64_t)d.scissor.extent.width > es_lim) d.scissor.extent.width = d.scissor.offset.x < es_lim ? (uint32_t)(es_lim - d.scissor.offset.x) : 0u; }
      if (i == 0) sb87::on_vp(d);   /* sb87: record the effective rect (after es86's cap); an armed bbox shrinks the scissor further (intersection) */
    }
    emit_data<DxvkViewport>(vps, n, [] (DxvkContext* c, const DxvkViewport *v, size_t k) { c->setViewports((uint32_t)k, v); }); g_cs.vpdirty = false;
  }
  Obj *robj(uint32_t id) { return (id && id <= g_objs.size() && g_objs[id - 1].kind) ? &g_objs[id - 1] : nullptr; }
  namespace sb87 { void apply_vp() { apply_viewports(); } }   /* sb87 */
}
void b1sb_vs_created(uint32_t id, const void *code, uint32_t len) { sb87::vs_created(id, code, len); }   /* sb87: rhi_b1_shader.cpp, stage 0 */
/* dc88: dead-clear elision (BSI_B1_DEADCLEAR, default 1; g4_notes/DEADCLEAR.md). Measured: UE3 clears the back-buffer-sized
   R32F depth-copy target twice a frame (0.67 ms each on the Quest 3) and at once draws a full-viewport quad into it that
   writes every texel. A colour clear of a 2D single-mip target is RECORDED here instead of emitted, and emitted (in its
   original place relative to everything that follows) before anything that could observe the target: an SRV / UAV bind of
   the resource, a copy / upload / mip generation / UAV clear touching it, a release, present / frame end, a readback, every
   .inc code path that reads images itself (s2 pending copies, pp76 per-eye work, aa76, sb87 CHECK, S1 A/B, g4), and the
   first draw into a target set holding it. At that draw the clear is DROPPED iff the draw provably overwrites every texel
   the clear would have set: the viewport and the (es86-capped) scissor contain the clear rect, the blend mode of that
   attachment writes RGBA unblended (or ONE/ZERO ADD), no alpha-to-coverage, sample mask bit 0 set, no depth / stencil test
   (or no depth attachment), fill mode, cull NONE, no GS / HS / DS, a pixel shader whose DXBC has no discard and no oMask
   (rhi_b1_shader.cpp: b1dx_shader_has_discard), one instance, 3 / 4 / 6 vertices, and (BSI_B1_DEADCLEAR_GEOM=1, default)
   the vertex data (attribute location 0, from sb87's ring of dynamic VB / IB writes) is a +-1 clip-space quad of two
   triangles sharing a diagonal, or one triangle containing the +-1 square, all w = 1 and z inside [0, 1]. GEOM=0 trusts the
   vertex count alone (not exact: a 4-vertex mesh need not cover the viewport). BSI_B1_DEADCLEAR=0: the old stream, bit for
   bit. BSI_B1_DEADCLEAR=2: like 1, but a stereo scene target's clear rect is es86's [0, limit) x [0, h) even when the
   clear itself would have been whole (EYESCIS_CLEARS=0): exact under es86's invariant that nothing reads the UI column of
   a scene target, not bit-exact in that column. */
extern "C" int b1dx_shader_has_discard(uint32_t id);   /* rhi_b1_shader.cpp (dc88) */
static bool dc88_ext_busy();   /* after the .inc includes at the end of this file: S1 twin / A-B state that reads targets at a draw */
static bool dc88_g4_on();      /* g4 (shadow mask coalescing) enabled: its two command streams reorder emits, dc88 stays off */
namespace {
namespace dc88 {
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_DEADCLEAR"); v = e && *e ? std::atoi(e) : 1; if (v < 0 || v > 2) v = 1; } return v; }
  static int geom() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_DEADCLEAR_GEOM"); v = e && *e ? std::atoi(e) != 0 : 1; } return v; }
  struct P { uint32_t vid, res; Rc<DxvkImageView> iv; VkClearValue cv; int es_cl; uint64_t seq; VkFormat fmt; uint32_t w, h;
             int64_t cov; };   /* x prefix [0, cov) x [0, h) already overwritten by qualifying draws (VR: the eye quads come one eye at a time) */
  static std::vector<P> pend;                    /* at most one per resource */
  static uint32_t srv_res[6][128], uav_res[64], sh[6];   /* resource bound in each SRV / UAV slot; shader bound per stage (own copies: the bind cache may be off) */
  static std::unordered_map<uint32_t, int> bound_cnt;    /* resource -> number of SRV / UAV slots holding it (the seam does not replicate D3D11's RT-vs-SRV auto-unbind) */
  static void set_slot(uint32_t &slot, uint32_t res) { if (slot == res) return; if (slot) { auto it = bound_cnt.find(slot); if (it != bound_cnt.end() && --it->second <= 0) bound_cnt.erase(it); } slot = res; if (res) bound_cnt[res]++; }
  static bool is_bound(uint32_t res) { return !bound_cnt.empty() && bound_cnt.count(res) != 0; }
  static std::unordered_map<uint32_t, int8_t> disc;      /* PS id -> b1dx_shader_has_discard */
  static long n_pend, n_elided, n_draw, n_srv, n_copy, n_bound, n_imm_kind, n_logged, n_partial, n_rem;   /* n_other, n_present: declared at the top of the file */
  static std::map<VkFormat, std::pair<long, long>> byfmt;   /* pending, elided */
  enum { DW_OK = 0, DW_PP76, DW_SHADERS, DW_INST, DW_COUNT, DW_A2C, DW_SMASK, DW_WMASK, DW_BLEND, DW_DS, DW_RS, DW_AREA, DW_DISCARD, DW_NOGEOM, DW_NOTQUAD, DW_NOVP, DW_ROWS, DW_SCIS, DW_EDGE, DW_PREFIX, DW_N };
  static const char *const wname[DW_N] = { "ok", "pp76 draw", "shaders (no VS/PS or GS/HS/DS)", "instances != 1", "vertex count / topology", "alpha-to-coverage", "sample mask", "colour write mask", "blend",
                                           "depth/stencil test", "rasterizer (fill/cull)", "render area", "PS may discard", "vertices unknown", "not a quad", "no viewport", "rows / scissor rows", "columns empty", "non-integer edge", "not contiguous prefix" };
  static long n_why[DW_N], n_why_r32f[DW_N], n_fail_logged;
  static long n_bind_refuse, n_lsblur, n_ovf;   /* "at other" split by hook (n_s2 / n_relref / n_upload / n_rdbk: top of the file) */
  static int find(uint32_t res) { for (size_t i = 0; i < pend.size(); i++) if (pend[i].res == res) return (int)i; return -1; }
  static void emit_one(const P &p) {   /* exactly what b1dx_c_clear_rtv emitted before dc88 (es86 partial / c5 raw / whole); after a partial cover only the remainder */
    if (p.cov > 0) { int64_t xe = p.es_cl > 0 ? p.es_cl : (int64_t)p.w; if (p.cov >= xe) return; n_rem++;   /* [0, cov) was overwritten by draws that touched nothing beyond it */
      emit([iv = p.iv, cv = p.cv, x0 = (int32_t)p.cov, xw = (uint32_t)(xe - p.cov)] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { x0, 0, 0 }, VkExtent3D { xw, iv->mipLevelExtent(0).height, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv); }); return; }
    if (p.es_cl > 0) { emit([iv = p.iv, cv = p.cv, es_cl = p.es_cl] (DxvkContext* c) { c->clearImageView(iv, VkOffset3D { 0, 0, 0 }, VkExtent3D { (uint32_t)es_cl, iv->mipLevelExtent(0).height, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv); }); }
    else if (c5_raw()) c5_clear_rt(p.iv.ptr(), VK_IMAGE_ASPECT_COLOR_BIT, p.cv);   /* c5 */
    else { DxvkAttachment a = { }; a.view = p.iv; emit([a, cv = p.cv] (DxvkContext* c) { c->clearRenderTarget(a, VK_IMAGE_ASPECT_COLOR_BIT, cv, 0u); }); } }
  static void flush_at(int i, long &ctr) { emit_one(pend[i]); pend.erase(pend.begin() + i); ctr++; }
  static void flush_res(uint32_t res, long &ctr) { if (pend.empty() || !res) return; int i = find(res); if (i >= 0) flush_at(i, ctr); }
  static void flush_all(long &ctr) { while (!pend.empty()) flush_at(0, ctr); }   /* in recording order */
  static void flush_view(uint32_t vid) { for (size_t i = 0; i < pend.size(); i++) if (pend[i].vid == vid) { flush_at((int)i, n_relref); return; } }
  static bool ext_now() {   /* a code path that may read or copy images at this draw outside this file's entry points: flush everything, elide nothing */
    return g4_mode || g4_cap || g_aa_live || sb87::mode() == 2 || b1vr::pass_n > 0 || dc88_ext_busy(); }
  /* pp76 handles THIS draw itself: replaced by per-column draws (pp_replace, FXAA per eye) or replayed / rebound right after it
     (sh.replay, ls_rebind, ls.vs_restore). Its own draws never pass through on_draw, so a pending clear of the bound target is emitted
     before the game's draw instead (never elided, never a partial cover). The per-eye features being merely ON does not matter: their
     other work reads SRV-bound resources (never pending) and ls_pre's copy source is flushed in on_draw_pre. */
  static bool pp76_this_draw() { return b1vr::pp_replace || (b1vr::on && b1vr::pp_post_wanted()); }
  /* b1dx_c_clear_rtv (g4 declined, g_rmu held): true = recorded, nothing emitted now */
  static bool record(View *v, uint32_t vid, const VkClearValue &cv, int es_cl) {
    if (!on()) return false;
    long *why = nullptr;   /* refused: the caller emits the clear now, so an older pending clear of the same image (a remainder) must go out first */
    Obj *o = robj(v->res);
    if (g4_mode || g4_cap || dc88_g4_on() || v->res == g_bb || !o || o->kind != 2) why = &n_imm_kind;
    else { const DxvkImageCreateInfo &ii = o->img->info(); const DxvkImageViewKey &vk = v->iv->info();
      if (ii.type != VK_IMAGE_TYPE_2D || ii.mipLevels != 1 || ii.numLayers != 1 || ii.sampleCount != VK_SAMPLE_COUNT_1_BIT
       || vk.mipIndex || vk.layerIndex || vk.mipCount != 1 || vk.layerCount != 1 || (lookupFormatInfo(ii.format)->aspectMask & ~VK_IMAGE_ASPECT_COLOR_BIT)) why = &n_imm_kind; }
    if (why) { (*why)++; flush_res(v->res, n_bind_refuse); return false; }
    const DxvkImageCreateInfo &ii = o->img->info();
    if (pend.size() >= 64) flush_at(0, n_ovf);
    int i = find(v->res); VkExtent3D e = v->iv->mipLevelExtent(0);
    P p = { vid, v->res, v->iv, cv, es_cl, g_cs_seq, ii.format, e.width, e.height, 0 };
    if (i >= 0) pend[i] = p; else pend.push_back(p);   /* a second clear of the same target replaces the value */
    n_pend++; byfmt[ii.format].first++; return true; }
  static void bound_flush() { for (size_t i = 0; i < pend.size(); ) { if (is_bound(pend[i].res)) flush_at((int)i, n_bound); else i++; } }   /* a draw / dispatch may sample it: before draws and dispatches */
  static bool ps_rejects(uint32_t ps) { auto it = disc.find(ps); if (it != disc.end()) return it->second != 0; int8_t r = (int8_t)(b1dx_shader_has_discard(ps) ? 1 : 0); disc[ps] = r; return r != 0; }
  static void shader_released(uint32_t id) { if (!disc.empty()) disc.erase(id); }
  static int64_t rect_x1(const P &p) {   /* the x end of the rect the clear would have set (rows always [0, h)) */
    int lim = (es86::lim && !b1vr::on) ? 0 : es86::lim;
    if (p.es_cl > 0) return p.es_cl;
    if (on() == 2 && lim > 0 && lim < (int64_t)p.w && es86::limit_for(g_views.get(p.vid)) == lim) return lim;   /* mode 2: the eye columns only (es86's invariant) */
    return p.w; }
  /* viewport 0 and its scissor as apply_viewports will emit them for this draw (es86 cap included; sb87 arms only stencil-tested draws, which never
     elide): true if the draw's pixel rect spans every row of the target; [x0, x1) its columns, clamped to the target */
  static int draw_xrange(const P &p, int64_t &x0, int64_t &x1) {   /* DW_OK or the failing condition */
    if (!g_cs.nvp) return DW_NOVP;
    RS *rs = g_rss.get(g_cs.rs); bool sc = rs && rs->scissor; const float *v = g_cs.vp[0]; if (!(v[2] > 0.f) || !(v[3] > 0.f)) return DW_NOVP;
    int64_t sx = 0, sy = 0, sw = 16384, shh = 16384;
    if (sc) { if (!g_cs.nsc) return DW_SCIS; const int32_t *r = g_cs.sc[0]; sx = r[0] > 0 ? r[0] : 0; sy = r[1] > 0 ? r[1] : 0; sw = r[2] > r[0] ? r[2] - sx : 0; shh = r[3] > r[1] ? r[3] - sy : 0; }
    int lim = (es86::lim && !b1vr::on) ? 0 : es86::lim;   /* pre_draw drops the limit when the stereo layout went off */
    if (lim > 0) { float x0 = sc && (float)sx > v[0] ? (float)sx : v[0]; if (!(x0 >= (float)(2 * b1vr::eye_w) - 1.5f) && sx + sw > lim) sw = sx < lim ? lim - sx : 0; }
    if (!(v[1] <= 0.f) || !((double)v[1] + (double)v[3] >= (double)p.h) || sy > 0 || sy + shh < (int64_t)p.h) return DW_ROWS;   /* every row */
    double vx0 = v[0], vx1 = (double)v[0] + (double)v[2]; if (!(vx0 == vx0) || !(vx1 == vx1)) return DW_NOVP;
    double a = std::max(vx0, (double)sx), b = std::min(vx1, (double)(sx + sw)); if (!(b > a)) return DW_SCIS;
    double fa = std::floor(a), fb = std::ceil(b); if (fa != a || fb != b) return DW_EDGE;   /* integer edges: pixel centres are never on an edge */
    x0 = (int64_t)std::max(fa, 0.0); x1 = (int64_t)std::min(fb, (double)p.w); return x1 > x0 ? DW_OK : DW_SCIS; }
  /* the draw's vertices (attribute location 0 as position: float2 / 3 / 4, vertex rate) from sb87's ring of CPU writes; 0 = unknown */
  static int fetch(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, std::vector<double> &v) {
    v.clear(); uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) return 0; IL *il = g_ils.get(ilid); if (!il || !il->na) return 0;
    int ai = -1; for (uint32_t k = 0; k < il->na; k++) if (il->in[k].attribute().location == 0) { ai = (int)k; break; } if (ai < 0) return 0;
    DxvkVertexAttribute a = il->in[ai].attribute(); int nc = a.format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : a.format == VK_FORMAT_R32G32_SFLOAT ? 2 : 0; if (!nc) return 0;
    bool rate_ok = false; for (uint32_t k = 0; k < il->nb; k++) { DxvkVertexBinding b = il->in[il->na + k].binding(); if (b.binding == a.binding) rate_ok = b.inputRate == VK_VERTEX_INPUT_RATE_VERTEX; } if (!rate_ok) return 0;
    uint32_t slot = a.binding; if (slot >= 32) return 0; uint32_t vb = g_bc.vb[slot], st = g_bc.vbs[slot], bo = g_bc.vbo[slot]; if (!vb || vb == BC_UNK || st == BC_UNK || !st || bo == BC_UNK) return 0;
    std::vector<uint32_t> idx;
    if (dic) { uint32_t count = dic->indexCount; if (!count || count > 64) return 0; uint32_t ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK) return 0;
      uint32_t isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; uint64_t io = (uint64_t)g_bc.ibo + (uint64_t)dic->firstIndex * isz; if (io + (uint64_t)count * isz > 0xffffffffu) return 0;
      const uint8_t *ip = sb87::find(ib, (uint32_t)io, count * isz); if (!ip) return 0;
      for (uint32_t i = 0; i < count; i++) { uint32_t x = isz == 4 ? sb87::r32(ip + 4 * i) : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8)); if (x == (isz == 4 ? 0xffffffffu : 0xffffu)) return 0;   /* strip cut: not a plain quad */
        int64_t vi = (int64_t)x + dic->vertexOffset; if (vi < 0 || vi > 0xffffffffll) return 0; idx.push_back((uint32_t)vi); } }
    else { uint32_t count = dc->vertexCount; if (!count || count > 64) return 0; for (uint32_t i = 0; i < count; i++) idx.push_back(dc->firstVertex + i); }
    uint32_t mn = idx[0], mx = idx[0]; for (uint32_t x : idx) { mn = std::min(mn, x); mx = std::max(mx, x); }
    uint64_t a0 = (uint64_t)bo + (uint64_t)mn * st + a.offset, a1 = (uint64_t)bo + (uint64_t)mx * st + a.offset + 4u * nc; if (a1 > 0xffffffffu || a0 >= a1) return 0;
    const uint8_t *p = sb87::find(vb, (uint32_t)a0, (uint32_t)(a1 - a0)); if (!p) return 0;
    for (uint32_t x : idx) { float f[4] = { 0.f, 0.f, 0.f, 1.f }; std::memcpy(f, p + (uint64_t)(x - mn) * st, 4u * nc); for (int c = 0; c < 4; c++) v.push_back((double)f[c]); }
    return 1; }
  /* one triangle (w = 1) containing the four corners of the +-1 square (inclusive, 1e-5), after dc / dic expansion in draw order */
  static bool tri_covers(const std::vector<double> &v) {
    if (v.size() != 12) return false; double x[3], y[3]; for (int i = 0; i < 3; i++) { if (std::fabs(v[4 * i + 3] - 1.0) > 1e-6) return false; x[i] = v[4 * i]; y[i] = v[4 * i + 1]; }
    double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]); if (std::fabs(area) < 1e-9) return false; double sg = area > 0 ? 1.0 : -1.0;
    static const double cx[4] = { -1, 1, 1, -1 }, cy[4] = { -1, -1, 1, 1 };
    for (int c = 0; c < 4; c++) for (int e = 0; e < 3; e++) { int j = (e + 1) % 3; double s = (x[j] - x[e]) * (cy[c] - y[e]) - (cx[c] - x[e]) * (y[j] - y[e]); if (s * sg < -1e-5) return false; }
    return true; }
  static bool quad_confirmed(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, bool depth_clip) {
    static std::vector<double> v; if (!fetch(dc, dic, v)) { return false; }   /* no CPU copy of the vertices (sb87's ring: BSI_B1_SHBBOX=0, or not a dynamic buffer) */
    if (depth_clip) for (size_t i = 0; i + 3 < v.size(); i += 4) { double w = v[i + 3]; if (!(w > 0) || !(v[i + 2] >= 0.0) || !(v[i + 2] <= w)) return false; }   /* z/w in [0, 1]: nothing clipped */
    for (size_t i = 0; i + 3 < v.size(); i += 4) if (!(v[i] == v[i]) || !(v[i + 1] == v[i + 1])) return false;
    bool ok = sb87::cover(v, g_m79_topo, g_rss.get(g_cs.rs)) || (g_m79_topo == 4u || g_m79_topo == 5u ? tri_covers(v) : false); return ok; }
  static bool geom_known(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) { static std::vector<double> v; return fetch(dc, dic, v) != 0; }
  /* the draw into attachment i overwrites every texel of its pixel rect (the exactness conditions of DEADCLEAR.md, except the rect itself) */
  static uint32_t fmt_channels(VkFormat f) {   /* colour write mask bits the format stores; a mask covering them writes every texel completely */
    switch (f) { case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R32_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R8_UINT: return 0x1u;
                 case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R16G16_UINT: return 0x3u;
                 case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32G32B32_SFLOAT: return 0x7u; default: return 0xfu; } }
  static int state_ok(uint32_t i, const P &p, const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {   /* DW_OK or the failing condition */
    if (!g_m79_ps || !g_m79_vs || sh[2] || sh[3] || sh[4]) return DW_SHADERS;
    uint32_t cnt = dc ? dc->vertexCount : dic->indexCount, inst = dc ? dc->instanceCount : dic->instanceCount; if (inst != 1) return DW_INST;   /* 0 draws nothing */
    if (g_m79_topo == 4u) { if (cnt != 3 && cnt != 6) return DW_COUNT; } else if (g_m79_topo == 5u) { if (cnt != 3 && cnt != 4) return DW_COUNT; } else return DW_COUNT;
    BS *bs = g_bss.get(g_cs.bs); if (bs && bs->a2c) return DW_A2C; if (!(g_cs.mask & 1u)) return DW_SMASK;
    if (bs) { const DxvkBlendMode &m = bs->m[i]; uint32_t need = fmt_channels(p.fmt); if ((m.writeMask() & need) != need) return DW_WMASK;
      if (m.blendEnable() && !(m.colorSrcFactor() == VK_BLEND_FACTOR_ONE && m.colorDstFactor() == VK_BLEND_FACTOR_ZERO && m.colorBlendOp() == VK_BLEND_OP_ADD
                             && m.alphaSrcFactor() == VK_BLEND_FACTOR_ONE && m.alphaDstFactor() == VK_BLEND_FACTOR_ZERO && m.alphaBlendOp() == VK_BLEND_OP_ADD)) return DW_BLEND; }
    { View *dv = g_views.get(g_cs.dsv); if (dv && dv->type == 3 && dv->iv != nullptr) { DSS *d = g_dsss.get(g_cs.dss); if (!d || d->s.depthTest() || d->s.stencilTest()) return DW_DS; } }   /* no DSS = D3D default: depth test on */
    RS *rs = g_rss.get(g_cs.rs); if (!rs || rs->s.polygonMode() != VK_POLYGON_MODE_FILL || rs->s.cullMode() != VK_CULL_MODE_NONE) return DW_RS;   /* no RS = D3D default: cull back */
    for (uint32_t k = 0; k < 9; k++) { View *o = g_views.get(k < 8 ? g_cs.rtv[k] : g_cs.dsv); if (!o || o->iv == nullptr || o->type != (k < 8 ? 2 : 3)) continue;   /* the render area is the smallest attachment */
      VkExtent3D e = o->iv->mipLevelExtent(0); if (e.width < p.w || e.height < p.h) return DW_AREA; }
    if (ps_rejects(g_m79_ps)) return DW_DISCARD;
    if (geom() && !quad_confirmed(dc, dic, rs->s.depthClip())) return geom_known(dc, dic) ? DW_NOTQUAD : DW_NOGEOM;
    return DW_OK; }
  static void log_fail(const P &p, uint32_t i, int w, const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {   /* the first 5 failures on an R32F target: full state */
    if (p.fmt != VK_FORMAT_R32_SFLOAT || n_fail_logged >= 5) return; n_fail_logged++;
    char ps[64], vs[64]; if (!b1dx_shader_name(g_m79_ps, ps, sizeof ps)) std::snprintf(ps, sizeof ps, "ps#%u", g_m79_ps); if (!b1dx_shader_name(g_m79_vs, vs, sizeof vs)) std::snprintf(vs, sizeof vs, "vs#%u", g_m79_vs);
    BS *bs = g_bss.get(g_cs.bs); DSS *d = g_dsss.get(g_cs.dss); RS *rs = g_rss.get(g_cs.rs); View *dv = g_views.get(g_cs.dsv); const float *v = g_cs.vp[0];
    int nrt = 0; for (uint32_t k = 0; k < 8; k++) if (g_cs.rtv[k]) nrt = (int)k + 1;
    std::fprintf(stderr, "[b1dc] not elided %ld (%s): res %u R32F %ux%u es_cl %d cov %ld, attachment %u of %d, dsv %u (%s), draw %u %s x%u first %u topo %u, PS %s (discard %d), VS %s, GS/HS/DS %u/%u/%u, "
                         "blend id %u%s en %d src %d dst %d op %d wmask %x a2c %d smask %x, DSS id %u%s depth %d stencil %d, RS id %u%s fill %d cull %u scissor %d, vp[%u] %g,%g %gx%g sc[%u] %d,%d-%d,%d es86 lim %d vpk %d, il %u pp76 %d\n",
                 n_fail_logged, w >= 0 && w < DW_N ? wname[w] : "?", p.res, p.w, p.h, p.es_cl, (long)p.cov, i, nrt, g_cs.dsv, dv && dv->type == 3 ? "bound" : "none",
                 dc ? dc->vertexCount : dic->indexCount, dc ? "vertices" : "indices", dc ? dc->instanceCount : dic->instanceCount, dc ? dc->firstVertex : dic->firstIndex, g_m79_topo, ps, g_m79_ps ? b1dx_shader_has_discard(g_m79_ps) : -1, vs, sh[2], sh[3], sh[4],
                 g_cs.bs, bs ? "" : "(default)", bs ? (int)bs->m[i].blendEnable() : 0, bs ? (int)bs->m[i].colorSrcFactor() : -1, bs ? (int)bs->m[i].colorDstFactor() : -1, bs ? (int)bs->m[i].colorBlendOp() : -1, bs ? bs->m[i].writeMask() : 0xfu, bs ? (int)bs->a2c : 0, g_cs.mask,
                 g_cs.dss, d ? "" : "(default)", d ? (int)d->s.depthTest() : 1, d ? (int)d->s.stencilTest() : 0, g_cs.rs, rs ? "" : "(default)", rs ? (int)rs->s.polygonMode() : 0, rs ? (unsigned)rs->s.cullMode() : 2u, rs ? (int)rs->scissor : 0,
                 g_cs.nvp, v[0], v[1], v[2], v[3], g_cs.nsc, g_cs.nsc ? g_cs.sc[0][0] : 0, g_cs.nsc ? g_cs.sc[0][1] : 0, g_cs.nsc ? g_cs.sc[0][2] : 0, g_cs.nsc ? g_cs.sc[0][3] : 0, es86::lim, es86::vpk, g_bc.il == BC_UNK ? 0u : g_bc.il, (int)pp76_this_draw()); }
  /* b1dx_c_draw*, first thing (before aa76 / g4 / S1 / s2 / pp76 see the draw): the code paths that read images on their own */
  static void on_draw_pre() {
    if (pend.empty()) return;
    if (ext_now()) { flush_all(n_other); return; }
    bound_flush();
    if (b1vr::on && b1vr::lse) flush_res(b1vr::ls.blur_res, n_lsblur);   /* pp76 ls_pre may copy the first view's blur target (a former rt0) */ }
  /* b1dx_c_draw*, right after pre_draw (viewports emitted, pp76's decision for this draw made), before the draw: resolve every pending
     clear of a bound target */
  static void on_draw_post(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    if (pend.empty()) return;
    bool pp = pp76_this_draw();
    for (uint32_t i = 0; i < 8; i++) { uint32_t vid = g_cs.rtv[i]; if (!vid) continue; View *v = g_views.get(vid); if (!v || v->type != 2) continue; int k = find(v->res); if (k < 0) continue;
      int64_t x0 = 0, x1 = 0; P &p = pend[k];
      int w = pp ? DW_PP76 : state_ok(i, p, dc, dic); if (w == DW_OK) w = draw_xrange(p, x0, x1); if (w == DW_OK && x0 > p.cov) w = DW_PREFIX;
      if (w != DW_OK) { n_why[w]++; if (p.fmt == VK_FORMAT_R32_SFLOAT) n_why_r32f[w]++; log_fail(p, i, w, dc, dic); flush_at(k, n_draw); continue; }   /* not a full-cover draw, or not contiguous with the covered prefix */
      if (x1 > p.cov) p.cov = x1;
      if (p.cov < rect_x1(p)) { n_partial++; continue; }   /* VR: the other eye's quad comes next; else the remainder is cleared at the next observer */
      { n_elided++; byfmt[pend[k].fmt].second++;
        if (n_logged < 5) { n_logged++; char ps[64], vs[64]; if (!b1dx_shader_name(g_m79_ps, ps, sizeof ps)) std::snprintf(ps, sizeof ps, "ps#%u", g_m79_ps); if (!b1dx_shader_name(g_m79_vs, vs, sizeof vs)) std::snprintf(vs, sizeof vs, "vs#%u", g_m79_vs);
          std::fprintf(stderr, "[b1dc] elided %ld: res %u %s %ux%u rect x < %d, attachment %u, draw %u %s x%u topo %u, PS %s, VS %s, geometry %s\n", n_logged, pend[k].res, gp::fmtname(pend[k].fmt), pend[k].w, pend[k].h,
                       (int)rect_x1(pend[k]), i, dc ? dc->vertexCount : dic->indexCount, dc ? "vertices" : "indices", dc ? dc->instanceCount : dic->instanceCount, g_m79_topo, ps, vs, geom() ? "confirmed" : "not checked (GEOM=0)"); }
        pend.erase(pend.begin() + k); } } }
  static void stats() {
    if (!on() || (!n_pend && !n_imm_kind)) return;
    std::string f; for (auto &e : byfmt) { char b[96]; std::snprintf(b, sizeof b, " %s %ld/%ld", gp::fmtname(e.first), e.second.second, e.second.first); f += b; }
    std::string w; for (int k = 1; k < DW_N; k++) if (n_why[k]) { char b[96]; std::snprintf(b, sizeof b, " %s %ld (R32F %ld)", wname[k], n_why[k], n_why_r32f[k]); w += b; }
    std::fprintf(stderr, "[b1dc] dead clears: pending %ld, elided %ld (%.2f/frame), emitted at draw %ld, at srv %ld, at copy %ld, at present %ld, at other %ld (s2 copy %ld, bind refused %ld, release %ld, upload %ld, readback/diag %ld, ls blur %ld, overflow %ld); "
                         "partial covers %ld, remainder clears %ld, still SRV/UAV-bound at draw %ld; not deferred (kind) %ld; by format (elided/pending):%s; at draw because:%s\n",
                 n_pend, n_elided, g_frames > 0 ? (double)n_elided / (double)g_frames : 0.0, n_draw, n_srv, n_copy, n_present, n_other, n_s2, n_bind_refuse, n_relref, n_upload, n_rdbk, n_lsblur, n_ovf,
                 n_partial, n_rem, n_bound, n_imm_kind, f.c_str(), w.empty() ? " -" : w.c_str()); }
}
}

/* vr70, cloud only: BSI_B1_VRSHADOW=1 runs the per-Present VR step (layout latch, tag check, AO crop setup) at b1's frame end in
   shadow mode (BSI_RHI=b1: the D3D11 bridge presents the real frame and b1 renders the same command stream next to it), so the
   AO crop can be compared pixel by pixel against the bridge's uncut frame (vr70/README.md). Off: nothing happens here. */
static void b1aa_frame_end(); static void b1aa_frame_begin();   /* aa76 (rhi_b1_aa.inc) */
static void b1vr_shadow_frame() {
  static int on = -1; if (on < 0) { const char *e = std::getenv("BSI_B1_VRSHADOW"); on = e && std::atoi(e) > 0; }
  if (!on) return;
  Obj *bb = obj(g_bb);
  b1aa_frame_end();   /* aa76: also in the cloud exactness check */
  if (bb) b1vr::present((uint32_t)g_br_presents, bb->img->info().extent.width, bb->img->info().extent.height);
  b1aa_frame_begin();   /* aa76 */
}
