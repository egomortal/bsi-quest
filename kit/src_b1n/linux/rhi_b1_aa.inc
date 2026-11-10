/* rhi_b1_aa.inc -- job aa76: opt-in anti-aliasing for VR stereo frames (native renderer, BSI_RHI=b1n). #included at the end of
   rhi_b1_dxvk.cpp (after rhi_b1_aoblur.inc). README: /home/claude/bsi/aa76/README.md. Switch and math: vr_aa.h.

   aa=off (default): nothing here runs (one bool test per draw); the frame is the build-75 frame.
   Otherwise, in stereo frames, the game's FXAA draws into the back buffer (FXEdgeDetectAntiAlias PS) are replaced. The game
   issues one per eye view, each with the whole buffer as viewport but a quad covering only that view's eye column; aa76 reads
   the quad's x range from its vertices (small dynamic vertex writes are kept while aa76 is live; fallback: the k-th such
   draw of the frame covers eye k) and anti-aliases exactly that eye from FXAA's own input (PS t0, the tone-mapped picture):
     aa=none  the input is copied (no AA)
     aa=smaa  SMAA 1x: edge detection and blending weights (two compute passes over the eye column), neighbourhood blending
              in the replacement draw (Jimenez et al., SMAA.hlsl, MIT; textures AreaTex / SearchTex)
     aa=taa   one compute pass resolves the eye's history (reprojected with the scene depth and that eye's current /
              previous view-projection, YCoCg variance clipping), the replacement draw shows it, optionally sharpened
   A later FXAA draw over an eye already done this frame is not issued. The UI column is never touched.
   The replacement draw is the game's own draw call (its vertex shader, viewport, render target and states) with an aa76
   fragment shader bound for it; the game's pixel shader is bound again right after. aa76 resources use slots the game does
   not use (PS/CS t120..t126, cb13, CS u60..u62); the bind filter forgets them afterwards.
   TAA's per-eye jitter and matrices come from the render-thread scene hook (vr_ui_rhi.c -> b1dx_vr_aa_scene, after the
   late head-pose latch): each eye's FViewInfo gets the frame's sub-pixel jitter (vr_aa.h va_jitter_view) before
   FSceneRenderer::Render, so everything the renderer derives from the view is jittered consistently.
   Depth: the R32F target of the game's depth copy (FXResolveDepthPixelShader) of this frame. Without it, TAA reprojects as if
   every pixel were far away (rotation only; logged).
   Fallbacks (logged, then the FXAA draw runs as issued): FXAA input unknown, viewport not covering whole eye columns,
   shader / resource creation failed.
   GPU time: with the pass profiler on (default; BSI_B1_GPUPROF=0 off) the [b1gp] lines get "aa76 <mode> compute" and
   "aa76 <mode> output" (the replacement draw); with aa=off the game's FXAA stays inside the back buffer's bucket.
   Cloud diagnostics (env, not for the Quest): BSI_AA_MOTION=<yaw deg>,<period frames>[,<pitch deg>] turns both eyes by a
   slow sinusoid (rotation about each eye, render thread), BSI_AA_SEQ=<trigger file> + BSI_AA_DUMPDIR=<dir> [BSI_AA_SEQN=48]:
   when the file appears, the motion restarts at phase 0 and the next N presents dump the eye columns (PPM) and log the
   reprojection matrices ([aa76] seq lines; aa76/scripts/metrics.py). These also run with aa=off (observer only). */
#include "dxvk_shader_spirv.h"
#include "vr_aa.h"
#include "rhi_b1_aa_spv.h"
#include "smaa_AreaTex.h"
#include "smaa_SearchTex.h"
extern "C" void vs_late_patch_view(uint8_t *vi, const double D[9]) __attribute__((weak));   /* vr_stereo.c (cloud motion) */

namespace aa {
  VaCfg cfg; bool cfg_ok; double next_cfg; std::string cfg_words;
  int mode;                  /* frame-latched cfg.mode */
  bool obs;                  /* cloud observer (motion / dump env set) */
  /* shaders and resources */
  Rc<DxvkShader> cs_taa, fs_out, cs_edge, cs_weight, fs_blend; bool sh_tried, sh_ok;
  Rc<DxvkBuffer> ubo;
  struct Img { Rc<DxvkImage> img; Rc<DxvkImageView> uav, srv; };
  Img hist[2], edges, blend, area, search; int tw, th; bool luts;
  int cur;                   /* history written this frame */
  VaHist hs;                 /* hs.ok: hist[cur ^ 1] holds the previous frame's TAA output (vr_aa.h va_hist_end) */
  /* per frame */
  uint32_t depth_res; Rc<DxvkImageView> depth_srv; uint32_t depth_srv_res;
  int done_mask; bool taa_ran, ran_any; long n_frames, n_proc, n_fb, n_skip_dup; int n_scene, n_fxaa, n_pairs, n_uber; long n_pres_noscene, n_bbmode;
  int n_fxbb;                /* FXAA draws into the back buffer this frame (order fallback) */
  /* small dynamic vertex writes (the post-process quads), newest last: the quad of an FXAA / tone map draw tells which eye
     column it covers (each eye view's draw covers only its own column, with the whole buffer as viewport) */
  struct VW { uint32_t id, off, size; uint8_t b[256]; } vw[64]; int vw_n; long n_geo, n_order, n_geo_miss;
  Img inp;                   /* bb mode: copy of the eye area of the back buffer (the tone-mapped picture when the game has no FXAA pass) */
  /* camera (scene hook) */
  bool cam_moved;            /* cloud dumps: the camera translated since the previous frame (> 0.5 UU) */
  VaEye prev[2], now[2]; bool scene_ok; float M[2][16]; int reset_flags, why[2]; double jx, jy; uint32_t jk; double uu = 100.0;
  long n_reset[8];
  /* cloud sequence */
  double mot_a, mot_p, mot_b; int seq_n = 48, seq_k = -1; long mot_k; double mot_scale = 1.0; std::string seq_file, dump_dir, seq_label;
  /* ps kinds: 1 FXAA, 2 depth copy, 3 tone map (FUberPostProcessBlendPixelShader0/1), 0 other */
  std::unordered_map<uint32_t, int> kinds; uint32_t last_ps = ~0u; int last_kind;

  static int env_i(const char *n, int d) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : d; }
  static int ps_kind(uint32_t id) {
    if (id == last_ps) return last_kind;
    auto it = kinds.find(id); int k = 0;
    if (it != kinds.end()) k = it->second;
    else { Rc<DxvkShader> s = id ? b1dx_shader_get(id) : nullptr; std::string n = s != nullptr ? s->debugName() : std::string();
      if (n.find("56e172d0a2b9597dd667ba16749e08af") != std::string::npos || n.find("45fe14520b3c3c65de5391e331a66d57") != std::string::npos) k = 1;
      else if (n.find("127cc27198087a8ca876cd0582532cd3") != std::string::npos) k = 2;
      else if (n.find("076a2902729a0e2f933de3d98dcda75a") != std::string::npos || n.find("f0557f4694a9c637e8de36ade9d07ddd") != std::string::npos) k = 3;
      kinds[id] = k; }
    last_ps = id; last_kind = k; return k; }

  /* config: vr_stereo.txt (the path vr_stereo.c uses) + BSI_VR_STEREO_OPTS + BSI_VR_AA, every 2 s (render thread, present) */
  static void read_cfg(double t) {
    if (cfg_ok && t < next_cfg) return;
    next_cfg = t + 2.0;
    VaCfg c; va_cfg_defaults(&c); std::string words;
    const char *p = std::getenv("BSI_VR_STEREO_FILE"); std::string path = p && *p ? p : "/sdcard/BSI/vr_stereo.txt";
    if (FILE *f = std::fopen(path.c_str(), "r")) { char b[2048]; size_t n = std::fread(b, 1, sizeof b - 1, f); b[n] = 0; std::fclose(f);
      for (char *q = b; *q; q++) if (*q == '\r' || *q == '\n' || *q == '\t') *q = ' ';
      va_parse_opts(&c, b); words += b; }
    if (const char *e = std::getenv("BSI_VR_STEREO_OPTS")) { va_parse_opts(&c, e); words += " | "; words += e; }
    if (const char *e = std::getenv("BSI_VR_AA")) { va_parse_opts(&c, e); words += " | "; words += e; }
    bool chg = !cfg_ok || std::memcmp(&c, &cfg, sizeof c) != 0;
    cfg = c; cfg_ok = true;
    if (chg) std::fprintf(stderr, "[aa76] anti-aliasing for VR stereo frames: aa=%s%s (alpha %.3f, clip %.2f, sharpen %.2f, jitter %d, filter %d, history %s, reset %.0f deg / %.1f m)\n",
                          va_mode_name(c.mode), c.mode == VA_GAME ? " (the game's FXAA, unchanged)" : "", c.alpha, c.clip, c.sharpen, c.jitter, c.filter, c.hist_cubic ? "cubic" : "linear", c.reset_deg, c.reset_m);
  }
  static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

  /* ---- shaders (SPIR-V from the GLSL in linux/aa76/, rhi_b1_aa_spv.h) ---- */
  constexpr uint32_t PS_CB = 0u * 16u + 13u, CS_CB = 5u * 16u + 13u;   /* DXVK resource slots: stage index * 16 + cb (PS = 0, CS = 5) */
  static uint32_t srv_slot(bool cs, uint32_t k) { return (cs ? 5u : 0u) * 128u + 120u + k; }   /* k: 0 color 1 depth 2 hist 3 edges 4 blend 5 area 6 search */
  static uint32_t uav_slot(uint32_t k) { return kUavCompute + 60u + k; }                       /* k: 0 hist 1 edges 2 blend */
  static DxvkBindingInfo b_ubo(bool cs) { DxvkBindingInfo b = { }; b.set = 0; b.binding = 0; b.resourceIndex = cs ? CS_CB : PS_CB; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1; b.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM; b.access = VK_ACCESS_UNIFORM_READ_BIT; b.flags.set(DxvkDescriptorFlag::UniformBuffer); return b; }
  static DxvkBindingInfo b_tex(bool cs, uint32_t binding, uint32_t k) { DxvkBindingInfo b = { }; b.set = 0; b.binding = binding; b.resourceIndex = srv_slot(cs, k);
    b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; b.descriptorCount = 1; b.viewType = VK_IMAGE_VIEW_TYPE_2D; b.access = VK_ACCESS_SHADER_READ_BIT; return b; }
  static DxvkBindingInfo b_img(uint32_t binding, uint32_t k) { DxvkBindingInfo b = { }; b.set = 0; b.binding = binding; b.resourceIndex = uav_slot(k);
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b.descriptorCount = 1; b.viewType = VK_IMAGE_VIEW_TYPE_2D; b.access = VK_ACCESS_SHADER_WRITE_BIT; return b; }
  template<size_t N> static Rc<DxvkShader> mk(const uint32_t (&code)[N], std::vector<DxvkBindingInfo> b, const char *name) {
    DxvkSpirvShaderCreateInfo ci = { }; ci.bindingCount = (uint32_t)b.size(); ci.bindings = b.data(); ci.debugName = name;
    Rc<DxvkShader> s = new DxvkSpirvShader(ci, SpirvCodeBuffer(N, code));
    s->getShaderMetadata(); return s; }
  static bool shaders() {
    if (sh_tried) return sh_ok;
    sh_tried = true;
    try {
      cs_taa    = mk(aa76_taa_comp,         { b_ubo(true), b_tex(true, 1, 0), b_tex(true, 2, 1), b_tex(true, 3, 2), b_img(4, 0) }, "aa76 TAA resolve");
      fs_out    = mk(aa76_out_frag,         { b_ubo(false), b_tex(false, 1, 0), b_tex(false, 3, 2) }, "aa76 output");
      cs_edge   = mk(aa76_smaa_edge_comp,   { b_ubo(true), b_tex(true, 1, 0), b_img(9, 1) }, "aa76 SMAA edges");
      cs_weight = mk(aa76_smaa_weight_comp, { b_ubo(true), b_tex(true, 5, 3), b_tex(true, 7, 5), b_tex(true, 8, 6), b_img(10, 2) }, "aa76 SMAA weights");
      fs_blend  = mk(aa76_smaa_blend_frag,  { b_ubo(false), b_tex(false, 1, 0), b_tex(false, 6, 4) }, "aa76 SMAA blend");
      DxvkBufferCreateInfo bi = { }; bi.size = 256; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "aa76 constants";
      ubo = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      sh_ok = true;
      std::fprintf(stderr, "[aa76] shaders ready (TAA %zu, output %zu, SMAA edges %zu / weights %zu / blend %zu SPIR-V words)\n",
                   sizeof aa76_taa_comp / 4, sizeof aa76_out_frag / 4, sizeof aa76_smaa_edge_comp / 4, sizeof aa76_smaa_weight_comp / 4, sizeof aa76_smaa_blend_frag / 4);
    } catch (const DxvkError &e) { std::fprintf(stderr, "[aa76] shader setup failed: %s (aa stays off)\n", e.message().c_str()); }
    return sh_ok; }
  /* an image with a storage view (layout GENERAL) and a sampled view */
  static Img mkimg(uint32_t w, uint32_t h, VkFormat f, bool storage, const char *name) {
    DxvkImageCreateInfo ii = { };
    ii.type = VK_IMAGE_TYPE_2D; ii.format = f; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT; ii.extent = { w, h, 1u }; ii.numLayers = 1; ii.mipLevels = 1;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    ii.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    ii.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | (storage ? VK_ACCESS_SHADER_WRITE_BIT : 0u);
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; ii.debugName = name;
    Img r; r.img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = f; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
    if (storage) { k.usage = VK_IMAGE_USAGE_STORAGE_BIT; k.layout = VK_IMAGE_LAYOUT_GENERAL; r.uav = r.img->createView(k); }
    k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED; r.srv = r.img->createView(k);
    return r; }
  static void clear_img(const Img &m) {
    VkClearValue cv = { }; VkExtent3D e = m.img->mipLevelExtent(0);
    emit([i = m.img, v = m.uav != nullptr ? m.uav : m.srv, e, cv] (DxvkContext* c) { c->initImage(i, VK_IMAGE_LAYOUT_UNDEFINED); c->clearImageView(v, VkOffset3D{ 0, 0, 0 }, e, VK_IMAGE_ASPECT_COLOR_BIT, cv); }); }
  static void upload(const Img &m, const void *data, uint32_t bytes) {
    Stage st = stage_alloc(bytes); std::memcpy(st.p, data, bytes);
    emit([img = m.img, sb = st.b, so = st.o, f = m.img->info().format] (DxvkContext* c) { c->uploadImage(img, sb, so, CACHE_LINE_SIZE, f); }); }
  /* targets of the eye area (2 eye widths x height) for the current mode */
  static bool targets(int mode_, int ew, int h) {
    try {
      if (tw != 2 * ew || th != h) { hist[0] = hist[1] = edges = blend = Img(); tw = 2 * ew; th = h; hs.ok = false; }
      if (mode_ == VA_TAA && hist[0].img == nullptr) {
        for (int i = 0; i < 2; i++) { hist[i] = mkimg(tw, th, VK_FORMAT_R16G16B16A16_SFLOAT, true, "aa76 TAA history"); clear_img(hist[i]); }
        hs.ok = false;
        std::fprintf(stderr, "[aa76] TAA history: 2 x %dx%d RGBA16F (%.1f MB)\n", tw, th, 2.0 * tw * th * 8 / 1048576.0); }
      if (mode_ == VA_SMAA && edges.img == nullptr) {
        edges = mkimg(tw, th, VK_FORMAT_R8G8B8A8_UNORM, true, "aa76 SMAA edges"); clear_img(edges);
        blend = mkimg(tw, th, VK_FORMAT_R8G8B8A8_UNORM, true, "aa76 SMAA weights"); clear_img(blend);
        std::fprintf(stderr, "[aa76] SMAA targets: 2 x %dx%d RGBA8 (%.1f MB)\n", tw, th, 2.0 * tw * th * 4 / 1048576.0); }
      if (mode_ == VA_SMAA && !luts) {
        area = mkimg(AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, false, "aa76 SMAA area");
        search = mkimg(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, false, "aa76 SMAA search");
        upload(area, areaTexBytes, AREATEX_SIZE); upload(search, searchTexBytes, SEARCHTEX_SIZE); luts = true; }
      return true;
    } catch (const DxvkError &e) { std::fprintf(stderr, "[aa76] target creation failed: %s\n", e.message().c_str()); return false; }
  }
  /* the depth copy's R32F target as a sampled view */
  static Rc<DxvkImageView> depth_view() {
    if (!depth_res) return nullptr;
    if (depth_srv != nullptr && depth_srv_res == depth_res) return depth_srv;
    Obj *o = obj(depth_res); if (!o || o->kind != 2 || !(o->img->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return nullptr;
    depth_srv = make_view(*o, VK_IMAGE_USAGE_SAMPLED_BIT); depth_srv_res = depth_res; return depth_srv; }

  static void vbwrite(uint32_t id, uint32_t off, uint32_t size, const void *data) {   /* g_rmu held */
    if (size > sizeof vw[0].b || !data) return;
    VW &w = vw[vw_n++ & 63]; w.id = id; w.off = off; w.size = size; std::memcpy(w.b, data, size); }
  /* the x range in buffer pixels of the quad this (non-indexed) draw reads from vertex buffer slot 0; false if unknown */
  static bool quad_px(const VkDrawIndirectCommand *dc, float *px0, float *px1) {
    if (!dc || !g_cs.nvp) return false;
    uint32_t id = g_bc.vb[0], st = g_bc.vbs[0], bo = g_bc.vbo[0]; if (!id || id == BC_UNK || !st) return false;
    uint32_t a = bo + dc->firstVertex * st, e = a + dc->vertexCount * st;
    for (int i = 0; i < 64; i++) { if (i >= vw_n) break; const VW &w = vw[(vw_n - 1 - i) & 63];
      if (w.id != id || a < w.off || e > w.off + w.size) continue;
      float n0, n1; if (!va_quad_xrange(w.b + (a - w.off), (int)dc->vertexCount, (int)st, &n0, &n1)) return false;
      *px0 = g_cs.vp[0][0] + (n0 + 1.f) * 0.5f * g_cs.vp[0][2]; *px1 = g_cs.vp[0][0] + (n1 + 1.f) * 0.5f * g_cs.vp[0][2]; return true; }
    return false; }
  /* ---- the FXAA draw (g_rmu held) ---- */
  static void fb(const char *why) { n_fb++; static int nl; if (nl < 10) { nl++; std::fprintf(stderr, "[aa76] FXAA draw runs as issued: %s\n", why); } }
  static void unbind_all(bool cs) {
    emit([cs] (DxvkContext* c) {
      VkShaderStageFlags st = cs ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
      for (uint32_t k = 0; k < 7; k++) c->bindResourceImageView(st, srv_slot(cs, k), nullptr);
      if (cs) for (uint32_t k = 0; k < 3; k++) c->bindResourceImageView(st, uav_slot(k), nullptr);
      c->bindUniformBuffer(st, cs ? CS_CB : PS_CB, DxvkBufferSlice()); });
    int s = cs ? 5 : 1;
    g_bc.cb[s][13] = BC_UNK; for (uint32_t k = 0; k < 7; k++) g_bc.srv[s][120 + k] = BC_UNK;
  }
  static bool draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    int k = ps_kind(b1vr::ps_id);
    if (k == 2) {   /* the depth copy: remember its target (the last one before FXAA is the post-base-pass depth) */
      View *v = g_views.get(g_cs.rtv[0]);
      if (v && v->type == 2) { Obj *o = obj(v->res); if (o && o->fmt == VK_FORMAT_R32_SFLOAT) { if (depth_res != v->res) { static int nl; if (nl++ < 2)
            std::fprintf(stderr, "[aa76] scene depth: R32F target %u (%ux%u) written by the game's depth copy\n", v->res, o->img->info().extent.width, o->img->info().extent.height); }
          depth_res = v->res; } }
      return false; }
    if (k == 1) n_fxaa++;
    if ((k != 1 && k != 3) || !b1vr::is_bb_view(g_cs.rtv[0])) return false;
    int ord = k == 3 ? n_uber++ : n_fxbb++;   /* k-th such draw of this frame (eye view k: the fallback when the quad is unknown) */
    if (mode == VA_GAME) return false;
    /* bb mode: the tone map writes the back buffer itself (the frame's post-process chain has no FXAA, e.g. the main menu):
       the game's draw runs, then its eye area is copied and anti-aliased like FXAA's input, and the draw is issued once more
       with the aa76 shader (eye columns only). The tone map is issued once per eye view over the whole buffer, so this runs
       at each of them (the later one rewrites the eyes; TAA writes the same history slot again: idempotent per frame). */
    bool bbm = k == 3;
    int ew = b1vr::eye_w, H = b1vr::bb_h;
    if (ew <= 0 || H <= 0 || !g_cs.nvp) { fb("no eye columns in this frame"); return false; }
    /* eye columns this draw covers whole: viewport AND scissor (va_cover), then narrowed to the eye of this draw (below) */
    const int32_t *scr = nullptr;
    { RS *rs = g_rss.get(g_cs.rs); if (rs && rs->scissor && g_cs.nsc) { scr = g_cs.sc[0];
        static int nl; if (nl++ < 2) std::fprintf(stderr, "[aa76] %s draw with scissor %d,%d..%d,%d (viewport %.0f,%.0f %.0fx%.0f): eye columns taken from viewport and scissor\n",
                                                  bbm ? "tone map" : "FXAA", scr[0], scr[1], scr[2], scr[3], g_cs.vp[0][0], g_cs.vp[0][1], g_cs.vp[0][2], g_cs.vp[0][3]); } }
    int want = 0, part = 0; va_cover(ew, H, g_cs.vp[0], scr, &want, &part);
    /* the quad's own x range: each eye view's post draw covers only its column although its viewport is the whole buffer
       (cloud run O2 of build h: with the viewport alone, aa76 took FXAA draw 1 for both eyes, its output reached the left
       column only and the right eye showed a stale picture; in scenes without FXAA the left eye's history was filtered
       twice). Unknown quad (indexed draw, vertices not seen): the k-th draw of the frame covers eye k. */
    { float q0, q1; const char *how;
      if (quad_px(dc, &q0, &q1)) { float vq[6] = { q0, g_cs.vp[0][1], q1 - q0, g_cs.vp[0][3], 0.f, 1.f }; int w2 = 0, p2 = 0; va_cover(ew, H, vq, scr, &w2, &p2);
        want &= w2; part |= p2; n_geo++; how = "its quad"; }
      else if (want == 3 && ord < 2) { want = 1 << ord; n_order++; n_geo_miss++; how = "the draw order (quad not found)"; }
      else { n_geo_miss++; how = "the viewport (quad not found)"; }
      static int nl; if (nl++ < 4) std::fprintf(stderr, "[aa76] %s draw %d of this frame: eye column(s) %s from %s\n", bbm ? "tone map" : "FXAA", ord, want == 3 ? "both" : want == 1 ? "left" : want == 2 ? "right" : "none", how); }
    if (part) { fb("the draw covers part of an eye column"); return false; }
    if (!want) { fb("the draw covers no whole eye column"); return false; }
    int todo = bbm ? want : want & ~done_mask;
    if (!todo) { n_skip_dup++; return true; }   /* every eye it covers is done this frame: the duplicate FXAA draw is not issued */
    Rc<DxvkImageView> col;
    if (!bbm) { uint32_t cid = g_bc.srv[1][0]; View *cv = (cid != BC_UNK) ? g_views.get(cid) : nullptr;
      if (!cv || cv->type != 1 || cv->iv == nullptr) { fb("FXAA's input (t0) unknown"); return false; }
      col = cv->iv; }
    if (!shaders()) { fb("aa76 shaders unavailable"); return false; }
    if (!targets(mode, ew, H)) { fb("aa76 targets unavailable"); return false; }
    int x0 = (todo & 1) ? 0 : ew, x1 = (todo & 2) ? 2 * ew : ew;
    if (bbm) {
      Obj *bb = obj(g_bb); if (!bb || bb->kind != 2) { fb("no back buffer"); return false; }
      try { if (inp.img == nullptr || inp.img->info().extent.width != (uint32_t)(2 * ew) || inp.img->info().extent.height != (uint32_t)H) {
              inp = mkimg(2 * ew, H, VK_FORMAT_R8G8B8A8_UNORM, false, "aa76 input (back buffer copy)");
              std::fprintf(stderr, "[aa76] this scene has no FXAA pass: the tone-mapped back buffer is anti-aliased instead (copy %dx%d RGBA8)\n", 2 * ew, H); } }
      catch (const DxvkError &e) { fb("input copy image failed"); return false; }
      pre_draw();   /* the game's tone map draw itself */
      if (dc) { VkDrawIndirectCommand d = *dc; emit([d] (DxvkContext* c) { c->draw(1, &d); }); }
      else { VkDrawIndexedIndirectCommand d = *dic; emit([d] (DxvkContext* c) { c->drawIndexed(1, &d); }); }
      g_ndraw++;
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      emit([di = inp.img, si = bb->img, sl, x0, x1, H] (DxvkContext* c) {
        c->copyImage(di, sl, VkOffset3D{ x0, 0, 0 }, si, sl, VkOffset3D{ x0, 0, 0 }, VkExtent3D{ (uint32_t)(x1 - x0), (uint32_t)H, 1u }); });
      col = inp.srv; n_bbmode++;
    }
    Rc<DxvkImageView> dep = mode == VA_TAA ? depth_view() : nullptr;
    VaUbo u; std::memset(&u, 0, sizeof u);
    u.rt[0] = 1.f / b1vr::bb_w; u.rt[1] = 1.f / H; u.rt[2] = (float)b1vr::bb_w; u.rt[3] = (float)H;
    u.eye[0] = ew; u.eye[1] = H; u.eye[2] = x0; u.eye[3] = x1;
    int flags = 0;
    if (mode == VA_TAA) {
      bool sc = scene_ok;
      flags = (cfg.hist_cubic ? VA_F_CUBIC : 0) | (cfg.filter ? 0 : VA_F_NOFILTER) | (hs.ok && sc ? VA_F_HIST : 0) | (sc ? reset_flags : 0) | (dep == nullptr ? VA_F_NODEPTH : 0);
      if (dep == nullptr) { static int nl; if (nl++ < 3) std::fprintf(stderr, "[aa76] TAA: no depth copy seen in this frame: history reprojected for rotation only\n"); }
      if (!sc) { static int nl; if (nl++ < 3) std::fprintf(stderr, "[aa76] TAA: no stereo scene matrices for this frame (scene hook missing?): history reset\n"); }
      std::memcpy(u.m, M, sizeof u.m); u.jit[0] = (float)jx; u.jit[1] = (float)jy;
    }
    u.prm[0] = mode; u.prm[1] = flags; u.prm[2] = (int32_t)jk; u.prm[3] = bbm ? 1 : 0;
    { static int si = -1; if (si < 0) si = env_i("BSI_AA_SHOWINPUT", 0) != 0; if (si) u.prm[3] |= 2; }   /* cloud: show the AA input (TAA: the jittered frame) */
    u.taa[0] = cfg.alpha; u.taa[1] = cfg.clip; u.taa[2] = cfg.sharpen;
    uint32_t gx = (uint32_t)(x1 - x0 + 7) / 8, gy = (uint32_t)(H + 7) / 8;
    char lab[48];
    emit([b = ubo, u] (DxvkContext* c) { c->updateBuffer(b, 0, sizeof u, &u); });
    /* compute passes */
    if (mode == VA_TAA || mode == VA_SMAA) {
      if (gp::on()) { std::snprintf(lab, sizeof lab, "aa76 %s compute", va_mode_name(mode)); gp::on_compute_label(lab); }
      if (mode == VA_TAA) {
        emit([s = cs_taa, b = ubo, col, dep, hp = hist[cur ^ 1].srv, ho = hist[cur].uav, gx, gy] (DxvkContext* c) mutable {
          c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(s));
          c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, CS_CB, DxvkBufferSlice(b, 0, 256));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 0), std::move(col));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 1), std::move(dep));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 2), std::move(hp));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav_slot(0), std::move(ho));
          c->dispatch(gx, gy, 1); });
        taa_ran = true;
      } else {
        emit([se = cs_edge, sw = cs_weight, b = ubo, col, eu = edges.uav, es = edges.srv, bu = blend.uav, as = area.srv, ss = search.srv, gx, gy] (DxvkContext* c) mutable {
          c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(se));
          c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, CS_CB, DxvkBufferSlice(b, 0, 256));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 0), std::move(col));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav_slot(1), std::move(eu));
          c->dispatch(gx, gy, 1);
          c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(sw));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 3), std::move(es));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 5), std::move(as));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, srv_slot(true, 6), std::move(ss));
          c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, uav_slot(2), std::move(bu));
          c->dispatch(gx, gy, 1); });
      }
      unbind_all(true);
      { Rc<DxvkShader> gcs = g_cur_cs ? b1dx_shader_get(g_cur_cs) : nullptr; emit([gcs] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(gcs)); }); }
    }
    /* the replacement draw: the game's FXAA draw call with an aa76 fragment shader */
    if (gp::on()) { std::snprintf(lab, sizeof lab, "aa76 %s output", va_mode_name(mode)); gp::on_compute_label(lab); gp::in_compute = false; gp::lastkey = gp::pkey; }
    emit([fs = mode == VA_SMAA ? fs_blend : fs_out, b = ubo, col, h = mode == VA_TAA ? hist[cur].srv : nullptr, bl = mode == VA_SMAA ? blend.srv : nullptr] (DxvkContext* c) mutable {
      c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(fs));
      c->bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT, PS_CB, DxvkBufferSlice(b, 0, 256));
      c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, srv_slot(false, 0), std::move(col));
      c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, srv_slot(false, 2), std::move(h));
      c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, srv_slot(false, 4), std::move(bl)); });
    pre_draw();
    if (dc) { VkDrawIndirectCommand d = *dc; emit([d] (DxvkContext* c) { c->draw(1, &d); }); }
    else { VkDrawIndexedIndirectCommand d = *dic; emit([d] (DxvkContext* c) { c->drawIndexed(1, &d); }); }
    g_ndraw++;
    if (gp::on()) gp::in_compute = true;   /* the next draw starts a new profiler segment */
    unbind_all(false);
    { Rc<DxvkShader> gps = b1vr::ps_id ? b1dx_shader_get(b1vr::ps_id) : nullptr; emit([gps] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(gps)); }); }
    done_mask |= todo; ran_any = true; n_proc++;
    if (n_proc == 1 || n_proc % 6000 == 0 || (bbm && n_bbmode == 1))
      std::fprintf(stderr, "[aa76] %s: %s (eyes %s, x %d..%d of %dx%d, depth %s, history %s); %ld frames, %ld duplicate FXAA draws not issued, %ld as issued\n",
                   va_mode_name(mode), bbm ? "tone map into the back buffer anti-aliased" : "FXAA draw replaced", todo == 3 ? "both" : todo == 1 ? "left" : "right", x0, x1, b1vr::bb_w, H, mode != VA_TAA ? "-" : dep != nullptr ? "R32F copy" : "none",
                   mode != VA_TAA ? "-" : (flags & VA_F_HIST) ? "valid" : "reset", n_proc, n_skip_dup, n_fb);
    return true;
  }

  /* ---- cloud sequence: dumps at present ---- */
  static void dump(int k) {
    Obj *bb = obj(g_bb); if (!bb || dump_dir.empty()) return;
    int ew = b1vr::eye_w, H = b1vr::bb_h; if (ew <= 0) return;
    try {
      VkExtent3D ext = { (uint32_t)(2 * ew), (uint32_t)H, 1u };
      DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)ext.width * ext.height * 4u; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "aa76 dump";
      Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      emit([rb, img = bb->img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
      ctx_flush(); cs_sync(); g_dev->waitForIdle();
      char path[600]; std::snprintf(path, sizeof path, "%s/%s%sseq_%03d.ppm", dump_dir.c_str(), seq_label.c_str(), seq_label.empty() ? "" : "/", k);
      FILE *f = std::fopen(path, "wb"); if (!f) return;
      std::fprintf(f, "P6\n%u %u\n255\n", ext.width, ext.height); const uint8_t *p = (const uint8_t *)rb->mapPtr(0);
      for (size_t i = 0; i < (size_t)ext.width * ext.height; i++) std::fwrite(p + 4 * i, 1, 3, f);
      std::fclose(f);
      /* the frame's scene depth (the game's R32F depth copy, device z) of the same eye area: seq_###.depth.f32, raw float32
         rows (metrics.py warps the previous frame with it, so a moving camera is measured too) */
      static int dd = -1; if (dd < 0) dd = env_i("BSI_AA_DUMPDEPTH", 0);   /* 1: always; default: only when the camera translated (disk) */
      Obj *dob = depth_res && (dd > 0 || cam_moved) ? obj(depth_res) : nullptr;
      if (dob && dob->kind == 2 && dob->fmt == VK_FORMAT_R32_SFLOAT && dob->img->info().extent.width >= ext.width && dob->img->info().extent.height >= ext.height) {
        Rc<DxvkBuffer> db = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        emit([db, img = dob->img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(db, 0, 0, 0, VK_FORMAT_R32_SFLOAT, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
        ctx_flush(); cs_sync(); g_dev->waitForIdle();
        std::snprintf(path, sizeof path, "%s/%s%sseq_%03d.depth.f32", dump_dir.c_str(), seq_label.c_str(), seq_label.empty() ? "" : "/", k);
        if (FILE *g = std::fopen(path, "wb")) { std::fwrite(db->mapPtr(0), 4, (size_t)ext.width * ext.height, g); std::fclose(g); } }
    } catch (const DxvkError &e) { std::fprintf(stderr, "[aa76] dump failed: %s\n", e.message().c_str()); }
  }
  static void frame_end() {
    if (!obs) { static int init; if (!init) { init = 1;
        const char *m = std::getenv("BSI_AA_MOTION"); if (m) { double a = 0, p = 48, b = 0; if (std::sscanf(m, "%lf,%lf,%lf", &a, &p, &b) >= 2 && p > 1) { mot_a = a; mot_p = p; mot_b = b; obs = true; } }
        const char *s = std::getenv("BSI_AA_SEQ"), *d = std::getenv("BSI_AA_DUMPDIR"); if (s && *s && d && *d) { seq_file = s; dump_dir = d; seq_n = env_i("BSI_AA_SEQN", 48); obs = true; }
        if (obs) std::fprintf(stderr, "[aa76] cloud sequence tools: motion yaw %.3f deg pitch %.3f deg period %.0f frames, trigger %s, %d frames to %s\n", mot_a, mot_b, mot_p, seq_file.c_str(), seq_n, dump_dir.c_str()); } }
    /* end of the frame just rendered */
    if (seq_k >= 0) {
      if (b1vr::on && scene_ok) {
        dump(seq_k);
        std::fprintf(stderr, "[aa76] seq %s%s%d mode %s jit %.4f %.4f hist %d reset %d", seq_label.c_str(), seq_label.empty() ? "" : " ", seq_k, va_mode_name(mode), jx, jy, mode == VA_TAA ? hs.ok : -1, reset_flags);
        for (int e = 0; e < 2; e++) { std::fprintf(stderr, " M%d", e); for (int i = 0; i < 16; i++) std::fprintf(stderr, " %.9g", M[e][i]); }
        for (int e = 0; e < 2; e++) { std::fprintf(stderr, " P%d", e); for (int i = 0; i < 16; i++) std::fprintf(stderr, " %.9g", now[e].proj[i]); }
        std::fprintf(stderr, "\n");
        if (++seq_k >= seq_n) { std::fprintf(stderr, "[aa76] seq done %s (%d frames)\n", seq_label.c_str(), seq_n); seq_k = -1; }
      }
    }
    { static int dbg = -2; if (dbg == -2) dbg = env_i("BSI_AA_DEBUG", 0);   /* cloud: per-present state of the first N stereo presents */
      if (dbg > 0 && b1vr::on && mode != VA_GAME) { dbg--; std::fprintf(stderr, "[aa76] debug present #%ld: scenes %d (pairs %d), FXAA draws %d, tone map draws into the back buffer %d, done %d, taa %d, hs.ok %d, reset %d (%s/%s)\n",
          (long)g_br_presents, n_scene, n_pairs, n_fxaa, n_uber, done_mask, taa_ran, hs.ok, reset_flags, va_reset_name(why[0]), va_reset_name(why[1])); } }
    if (mode == VA_TAA) {
      bool any = n_scene != 0 || n_fxaa != 0 || n_uber != 0;
      if (!any) n_pres_noscene++;   /* a present without a scene (loading, movie, an extra UI present): history kept up to VA_MAX_GAP in a row */
      long g0 = hs.n_gap_resets;
      if (va_hist_end(&hs, taa_ran && done_mask == 3, any)) cur ^= 1;
      if (hs.n_gap_resets != g0) { n_reset[VA_R_GAP]++; static int nl; if (nl++ < 4) std::fprintf(stderr, "[aa76] TAA history reset: more than %d presents without a scene (loading / movie)\n", VA_MAX_GAP); } }
    n_scene = 0; n_fxaa = 0; n_pairs = 0; n_uber = 0; n_fxbb = 0;
    if (!scene_ok && b1vr::on && mode == VA_TAA && n_frames % 600 == 0) std::fprintf(stderr, "[aa76] TAA: frame without a stereo scene hook call\n");
    n_frames++;
    done_mask = 0; taa_ran = false; ran_any = false; scene_ok = false; depth_res = 0;
  }
  static void frame_begin() {   /* after b1vr::present latched the stereo layout of the next frame */
    if (!b1vr::on && mode == VA_GAME && !obs) { g_aa_live = false; return; }   /* flat: nothing is read or changed */
    double t = now_s();
    read_cfg(t);
    int was = mode; mode = b1vr::on ? cfg.mode : VA_GAME;
    if (mode != was) { hs.ok = false; std::fprintf(stderr, "[aa76] mode %s -> %s%s\n", va_mode_name(was), va_mode_name(mode), b1vr::on ? "" : " (no stereo layout)"); }
    g_aa_live = b1vr::on && (mode != VA_GAME || obs);
    /* the trigger file's first word (optional) labels the sequence: dumps go to <dir>/<label>/ (several sequences per run,
       e.g. with BSI_VR_STEREO_FILE rewritten in between to switch aa= live) */
    if (!seq_file.empty() && seq_k < 0 && b1vr::on && n_frames % 8 == 0) { if (FILE *f = std::fopen(seq_file.c_str(), "r")) {
        char lb[64] = ""; double ms = 1.0; if (std::fscanf(f, "%63s %lf", lb, &ms) < 1) lb[0] = 0; std::fclose(f); std::remove(seq_file.c_str());
        mot_scale = ms;   /* second word: motion amplitude scale (0 = a still head) */
        seq_label = lb; if (!seq_label.empty()) ::mkdir((dump_dir + "/" + seq_label).c_str(), 0755);
        seq_k = 0; mot_k = 0; std::fprintf(stderr, "[aa76] seq start %s: %d frames, motion phase 0 x %.2f, aa=%s\n", seq_label.c_str(), seq_n, mot_scale, va_mode_name(mode)); } }
    { static double next; static long last;   /* one status line every 10 s while stereo frames are anti-aliased */
      if (cfg.log && t >= next) { next = t + 10.0; if (n_proc != last) { last = n_proc;
        std::fprintf(stderr, "[aa76] status: aa=%s, %ld stereo frames anti-aliased so far, %ld duplicate FXAA draws not issued, %ld FXAA draws as issued, %ld tone-map-only frames, eye columns from the quad %ld / draw order %ld; TAA resets: rotation %ld, move %ld, projection %ld, loading gap %ld, first %ld\n",
                     va_mode_name(mode), n_proc, n_skip_dup, n_fb, n_bbmode, n_geo, n_order, n_reset[VA_R_ROT], n_reset[VA_R_MOVE], n_reset[VA_R_PROJ], n_reset[VA_R_GAP], n_reset[VA_R_FIRST]); } } }
  }
  /* ---- scene hook (render thread, before FSceneRenderer::Render; g_rmu taken by the caller) ---- */
  static void scene(uint8_t *vi, uint32_t n) {
    int ew = b1vr::eye_w; n_scene++;
    if (!g_aa_live || n != 2 || ew <= 0) return;
    float x0f, x1f; std::memcpy(&x0f, vi + VA_VI_X, 4); std::memcpy(&x1f, vi + VA_VI_SIZE + VA_VI_X, 4);
    if ((int)std::lround(x0f) != 0 || (int)std::lround(x1f) != ew) return;   /* not the two eye columns of a stereo frame */
    /* cloud: a slow head motion (rotation about each eye; the game's view stays where the game put it) */
    if (mot_p > 1 && vs_late_patch_view && (seq_file.empty() || seq_k >= 0)) {   /* with a trigger file: still until the sequence starts */
      double ph = 2.0 * M_PI * (double)mot_k / mot_p, yaw = mot_scale * mot_a * std::sin(ph) * M_PI / 180.0, pit = mot_scale * mot_b * std::sin(2.0 * ph) * M_PI / 180.0;
      double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
      double Dy[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy }, Dp[9] = { 1, 0, 0, 0, cp, sp, 0, -sp, cp }, D[9];
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) D[r*3 + c] = Dy[r*3 + 0] * Dp[0*3 + c] + Dy[r*3 + 1] * Dp[1*3 + c] + Dy[r*3 + 2] * Dp[2*3 + c];
      for (int e = 0; e < 2; e++) vs_late_patch_view(vi + e * VA_VI_SIZE, D);
      mot_k++; }
    jk++;
    if (mode == VA_TAA && cfg.jitter) va_jitter(jk, cfg.jitter_n, &jx, &jy); else jx = jy = 0;
    reset_flags = 0;
    for (int e = 0; e < 2; e++) {
      uint8_t *v = vi + e * VA_VI_SIZE;
      va_eye_read(v, &now[e]);
      if (e == 0) { double d2 = 0; for (int i = 0; i < 3; i++) { double d = now[0].pvt[i] - prev[0].pvt[i]; d2 += d * d; } cam_moved = prev[0].ok && d2 > 0.25; }
      int r = va_reset_reason(&now[e], &prev[e], &cfg, uu); why[e] = r;
      double Md[16]; if (r == VA_R_NONE && !va_reproj(&now[e], &prev[e], Md)) r = VA_R_PROJ;
      if (r) { reset_flags |= e ? VA_F_RESET1 : VA_F_RESET0; if (mode == VA_TAA) n_reset[r]++;
        if (mode == VA_TAA && r != VA_R_FIRST) { static int nl; if (nl++ < 6) std::fprintf(stderr, "[aa76] TAA history of eye %d reset: %s\n", e, va_reset_name(r)); }
        for (int i = 0; i < 16; i++) M[e][i] = (i % 5 == 0) ? 1.f : 0.f; }
      else for (int i = 0; i < 16; i++) M[e][i] = (float)Md[i];
      prev[e] = now[e];
      if (jx != 0 || jy != 0) va_jitter_view(v, 2.0 * jx / ew, 2.0 * jy / b1vr::bb_h);
    }
    scene_ok = true; n_pairs++;
    if (n_pairs == 2) { static int nl; if (nl++ < 4) std::fprintf(stderr, "[aa76] two stereo scenes in one frame (Present #%ld): the last one's matrices are used\n", (long)g_br_presents + 1); }
  }
}
static bool b1aa_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) { return aa::draw(dc, dic); }
static bool b1aa_owns_fxaa() { return g_aa_live && aa::mode != VA_GAME; }   /* for other per-eye FXAA code (pp76 merge) */
static void b1aa_vbwrite(uint32_t id, uint32_t off, uint32_t size, const void *data) { aa::vbwrite(id, off, size, data); }
static void b1aa_frame_end() { aa::frame_end(); }
static void b1aa_frame_begin() { aa::frame_begin(); }
/* vr_ui_rhi.c's scene command hook (FDrawSceneCommand::Execute, rendering thread), after the late head-pose latch:
   vi = host pointer of the FSceneRenderer's first FViewInfo, n views (0x11A0 bytes each) */
extern "C" void b1dx_vr_aa_scene(uint8_t *vi, uint32_t n) {
  if (!g_aa_live || !vi) return;
  std::lock_guard<RmuLock> lk(g_rmu);
  aa::scene(vi, n);
}
