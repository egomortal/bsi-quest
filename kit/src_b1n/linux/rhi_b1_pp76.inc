/* rhi_b1_pp76.inc -- job pp76 (README /home/claude/bsi/pp76/README.md, notes/VR_STEREO.md §23): post-process per eye in VR
   stereo (light shafts, the sun shadow mask of the right eye, FXAA, bloom) and the cloud pass diagnostics. #included at the end
   of rhi_b1_vr.inc (same place in rhi_b1_dxvk.cpp as the code that was there in the first pp76 binaries). */

/* ---- pp76 diagnostics (cloud): per-pass target dumps and constant logs during the BSI_B1_VRPASS(FILE) frames ----
   BSI_B1_VRPASSFLAT=1: the pass log also runs in flat frames (the eye-alone reference BSI_VR_EYEREF=0).
   BSI_B1_VRPDUMP=<file>: lines "<ps hash prefix> <flags>" (flags: d = dump render target 0 after the last draw of every run
   of draws with that pixel shader / target / viewport, raw texels with a one-line header, to $BSI_B1_DUMPDIR/pd_f<frame>_d<draw>.raw;
   c = log the PS and VS constant buffers 0..1 of each such draw). BSI_B1_VRPDUMPN = how many logged frames dump (default 1). */
namespace b1vr {
  bool pass_flat; uint32_t vs_cb[4], ps_cb[4], vs_id;
  struct PdRule { std::string h; bool d, c; }; std::vector<PdRule> pd_rules; int pd_init = -1, pd_frames = 1;
  struct PdPrev { Rc<DxvkImage> img; uint32_t mip = 0, layer = 0, res = 0, ps = 0, rtv = 0; float vp[4]; long draw = 0, frame = 0; bool want = false; std::string name;
                  Rc<DxvkImage> mimg[3]; uint32_t mmip[3], mlayer[3], mres[3]; } pd;
  static void pd_load() {
    pd_init = 0; const char *f = std::getenv("BSI_B1_VRPDUMP"); if (!f) return; FILE *fp = std::fopen(f, "r"); if (!fp) return;
    char l[256]; while (std::fgets(l, sizeof l, fp)) { char h[80] = "", fl[16] = ""; if (std::sscanf(l, "%79s %15s", h, fl) < 1 || h[0] == '#') continue;
      PdRule r; r.h = h; r.d = std::strchr(fl, 'd') != nullptr; r.c = std::strchr(fl, 'c') != nullptr; pd_rules.push_back(r); }
    std::fclose(fp); pd_init = 1; pd_frames = env_i("BSI_B1_VRPDUMPN", 1);
    std::fprintf(stderr, "[b1vr] pass dumps: %zu shader rules from %s, %d frame(s)\n", pd_rules.size(), f, pd_frames); }
  static const PdRule *pd_rule(const std::string &name) {
    size_t p = name.find('.'); std::string h = p == std::string::npos ? name : name.substr(p + 1);
    for (auto &r : pd_rules) if (r.h == "*" || h.compare(0, r.h.size(), r.h) == 0) return &r; return nullptr; }
  static void pd_write1();
  static void pd_write() {   /* RT0, then RT1..3 (MRT) as pd_f<frame>_d<draw>_rt<k>.raw */
    if (!pd.want || pd.img == nullptr) { pd.want = false; pd.img = nullptr; for (auto &m : pd.mimg) m = nullptr; return; }
    Rc<DxvkImage> m0[3]; uint32_t mm[3], ml[3], mr[3]; for (int k = 0; k < 3; k++) { m0[k] = pd.mimg[k]; pd.mimg[k] = nullptr; mm[k] = pd.mmip[k]; ml[k] = pd.mlayer[k]; mr[k] = pd.mres[k]; }
    long d0 = pd.draw; std::string n0 = pd.name;
    pd_write1();
    for (int k = 0; k < 3; k++) if (m0[k] != nullptr) { pd.want = true; pd.img = m0[k]; pd.mip = mm[k]; pd.layer = ml[k]; pd.res = mr[k]; pd.draw = 1000000 + d0 * 10 + k + 1;   /* MRT k+1 of draw d0 */ pd.name = n0; pd_write1(); }
  }
  static void pd_write1() {
    if (!pd.want || pd.img == nullptr) { pd.want = false; pd.img = nullptr; return; }
    pd.want = false;
    try {
      Rc<DxvkImage> img = pd.img; pd.img = nullptr; VkFormat fmt = img->info().format; const DxvkFormatInfo *fi = lookupFormatInfo(fmt);
      if (fi->flags.test(DxvkFormatFlag::BlockCompressed) || !(fi->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) return;
      VkExtent3D ext = img->mipLevelExtent(pd.mip); uint32_t es = fi->elementSize; VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * es;
      DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 pass dump";
      Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, pd.mip, pd.layer, 1u };
      emit([rb, fmt, img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ ext.width, ext.height, 1u }); });
      ctx_flush(); cs_sync(); g_dev->waitForIdle();
      const char *dir = std::getenv("BSI_B1_DUMPDIR"); char path[512];
      std::snprintf(path, sizeof path, "%s/pd_f%ld_d%ld.raw", dir ? dir : "/tmp", pd.frame, pd.draw);
      FILE *f = std::fopen(path, "wb"); if (!f) return;
      std::fprintf(f, "B1RAW %u %u %d %u res %u draw %ld ps %s vp %.1f %.1f %.1f %.1f %s\n", ext.width, ext.height, (int)fmt, es, pd.res, pd.draw, pd.name.c_str(),
                   pd.vp[0], pd.vp[1], pd.vp[2], pd.vp[3], on ? "stereo" : "flat");
      std::fwrite(rb->mapPtr(0), 1, (size_t)sz, f); std::fclose(f);
      std::fprintf(stderr, "[b1vr] pdump: frame %ld draw %ld ps %.40s res %u %ux%u fmt %d -> %s\n", pd.frame, pd.draw, pd.name.c_str(), pd.res, ext.width, ext.height, (int)fmt, path);
    } catch (const DxvkError &e) { std::fprintf(stderr, "[b1vr] pdump failed: %s\n", e.message().c_str()); } }
  static void cb_print(const char *what, uint32_t id) {
    if (!id) return; auto it = cbsh.find(id); if (it == cbsh.end() || !it->second.n) { std::fprintf(stderr, "[b1vr] pcb: %s id %u unknown\n", what, id); return; }
    const float *f = it->second.f.data(); uint32_t n = it->second.n / 4; if (n > 96) n = 96;
    std::string o; char b[40]; for (uint32_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%s%.9g", i % 4 ? " " : (i ? " | " : ""), f[i]); o += b; }
    std::fprintf(stderr, "[b1vr] pcb: %s id %u (%u B): %s\n", what, id, it->second.n, o.c_str()); }
  static void pd_draw(const std::string &name) {
    if (pd_init < 0) pd_load(); if (pd_init <= 0) return;
    bool dumping = n_frames < pass_from + pd_frames;   /* the first pd_frames frames of every logged window */
    View *v = g_views.get(g_cs.rtv[0]);
    bool same = pd.want && pd.ps == ps_id && pd.rtv == g_cs.rtv[0] && g_cs.nvp && !std::memcmp(pd.vp, g_cs.vp[0], 16);
    if (pd.want && !same) pd_write();
    const PdRule *r = pd_rule(name); if (!r) { pd.want = false; pd.img = nullptr; return; }
    if (r->c) { char w[96]; for (int k = 0; k < 8; k++) { std::snprintf(w, sizeof w, "frame %ld draw %ld %s cb%d", n_frames, pass_draw, k < 4 ? "PS" : "VS", k & 3); cb_print(w, k < 4 ? ps_cb[k] : vs_cb[k & 3]); } }
    if (!r->d || !dumping || !v || v->type != 2 || v->iv == nullptr) { pd.want = false; pd.img = nullptr; return; }
    pd.want = true; pd.img = v->iv->image(); pd.mip = v->iv->info().mipIndex; pd.layer = v->iv->info().layerIndex; pd.res = v->res;
    pd.ps = ps_id; pd.rtv = g_cs.rtv[0]; if (g_cs.nvp) std::memcpy(pd.vp, g_cs.vp[0], 16); else std::memset(pd.vp, 0, 16);
    pd.draw = pass_draw; pd.frame = n_frames; pd.name = name;
    for (int k = 0; k < 3; k++) { View *mv = g_views.get(g_cs.rtv[k + 1]); pd.mimg[k] = nullptr;
      if (mv && mv->type == 2 && mv->iv != nullptr) { pd.mimg[k] = mv->iv->image(); pd.mmip[k] = mv->iv->info().mipIndex; pd.mlayer[k] = mv->iv->info().layerIndex; pd.mres[k] = mv->res; } } }
  static void pd_frame_end() { if (pd.want) pd_write(); }
}

/* ---- pp76: light shafts and the sun's shadow mask per eye (vr_stereo.txt ls_eye=1, shadow_eye=1; README of job pp76,
   notes/VR_STEREO.md §23) ----
   Both found by logging every draw of stereo frames and dumping each pass's target (cloud, BSI_VR_FAKE_SAME / BSI_VR_EYEREF).
   1. Light shafts (ls_eye). UE3 renders the light shafts of each eye view into one quarter-size target pair: view 0's chain
      (TDownsampleLightShafts, PASS_0, PASS_1 -> target B), then view 1's chain into the same targets, and only then the two
      tone-map passes (FUberPostProcessBlend, one per view, each writing its own eye column and reading B at t4). So the left
      eye read the right view's result, whose left column is empty: no sun rays / wrong occlusion in the left eye. Every blur
      tap is clamped to the view's own rectangle (cb0[7]), so view 0's result is exact for the left eye. With ls_eye=1 rhi_b1
      copies B when the second chain starts and binds the copy at t4 for the first tone-map pass of the frame.
   2. Sun shadow mask (shadow_eye). The whole-scene (cascaded) shadow projection of the dominant directional light is drawn by
      the game for the first view only (viewport = left eye column): the clear of the mask, the cascade's stencil volume,
      the projection (TXShadowProjectionPixelShader*NoLightClip*) and the stencil clear, once per cascade, interleaved with
      the cascade's depth render. The right eye's column of the mask is never written in a stereo frame; the bilateral
      upscale then hands the stale contents to the right eye's sun light. With shadow_eye=1 every such draw (target = the
      mask, viewport = the left eye rectangle) is drawn again right after itself with the right eye's viewport and the right
      view's constants: VS cb1 (view: TranslatedViewProjection + eye position, rows 0..3 re-based to the left view's
      translated space, since the volume vertices are), VS cb0 rows 0..2 (clip.xyw -> eye-relative position = inverse of the
      view's TVP columns x,y,w), PS cb2 (ScreenPositionScaleBias: the right column's), PS cb0 row 3 (the shadow coordinates of
      the eye position: + (E_R - E_L) * rows 0..2). The right view's view constants are taken from the game's own right-eye
      draws earlier in the frame (VS cb1 / PS cb2 contents at draws with the right eye's viewport) and checked against the
      right eye's later draws (logged). Pure math: vs_shadow_eye_consts (vr_stereo.c, host-tested). */
void apply_viewports();   /* rhi_b1_dxvk.cpp, right after this file */
extern "C" int vr_stereo_pp_eye(int which) __attribute__((weak));
extern "C" int vs_fx_copy_plan(int x0, int x1, int W, int m, int plan[][4], int max) __attribute__((weak));   /* pp76 */
extern "C" int vs_shadow_eye_consts(const float *vs1_l, const float *vs1_r, const float *vs0_l, const float *ps0_l, float *vs1_out, float *vs0_out, float *ps0_row3) __attribute__((weak));
namespace b1vr {
  enum { PK_NULL = 0, PK_OTHER, PK_SHPROJ_DIR, PK_SHPROJ, PK_ONECOLOR, PK_LSDOWN, PK_LSBLUR, PK_UBER, PK_FXAA };
  int cur_pk; uint32_t vs_cbid[4], ps_cbid[4]; uint32_t t4_res; Rc<DxvkImageView> t4_iv; uint32_t t0_res; Rc<DxvkImageView> t0_iv; uint32_t ps_tres[4];
  int lse, she, fxe, ble;                         /* frame-latched switches */
  bool pp_replace;                                /* this draw is replaced (b1dx_c_draw* calls pp_replace_draw instead of drawing) */
  bool ls_rebind;                                 /* the first tone-map pass got the light-shaft copy at t4 */
  static int pk_of(uint32_t id) {
    if (!id) return PK_NULL;
    Rc<DxvkShader> s = b1dx_shader_get(id); if (s == nullptr) return PK_OTHER;
    static std::unordered_map<DxvkShader*, int> k; auto it = k.find(s.ptr()); if (it != k.end()) return it->second;
    std::string n = s->debugName(); size_t p = n.find('.'); std::string h = p == std::string::npos ? n : n.substr(p + 1, 8);
    static const char *dir[] = { "8025632c", "2f76042b", "3d84bd78", "c0b538d6", "cd4a8dad", "236cf5c7" };   /* TXShadowProjection*NoLightClip* (SM4/SM5) */
    static const char *oth[] = { "a3f96983", "dab3e9f3", "d55f68d1", "f2e248c2", "2e583db5", "244e80a9", "cf008440", "02646036" };   /* spot / point clip */
    int v = PK_OTHER;
    for (auto d : dir) if (h == d) v = PK_SHPROJ_DIR;
    for (auto d : oth) if (h == d) v = PK_SHPROJ;
    if (h == "bd9a0c2c") v = PK_ONECOLOR;
    if (h == "718c4fd3" || h == "41f69c03") v = PK_LSDOWN;          /* TDownsampleLightShaftsPixelShader<TRUE/FALSE> */
    if (h == "215edd42" || h == "fd3747cc") v = PK_LSBLUR;          /* PASS_1_NORMAL / PASS_1_HIGH (final blur) */
    if (h == "076a2902" || h == "f0557f46") v = PK_UBER;            /* FUberPostProcessBlendPixelShader1 / 0 */
    if (h == "56e172d0" || h == "45fe1452") v = PK_FXAA;            /* FXEdgeDetectAntiAliasPixelShader (SM4 / SM5) */
    k[s.ptr()] = v; return v; }
  uint32_t fx_ps;   /* b124: the bound PS id (FXAA split twin lookup) */
  static void ps_set(uint32_t sh) { cur_pk = pk_of(sh); fx_ps = sh; }
  /* CPU copies needed: VS cb1 / PS cb2 (the view buffers, few writes) always while stereo; PS cb0 ($Globals, written per draw)
     only while a projection shader is bound */
  uint32_t ps0_gate, vs0_gate; std::unordered_map<uint32_t, char> cb_stale;
  static void cb_slot(int st, uint32_t slot, uint32_t id) {
    if (slot >= 4 || (st != 0 && st != 1)) return;
    (st ? ps_cbid : vs_cbid)[slot] = id;
    if (!on || !id) return;
    if ((st == 0 && slot == 1) || (st == 1 && slot == 2)) cb_bound(id); }
  static void cb_gate_ps0() { if (ps_cbid[0] && ps_cbid[0] != ps0_gate && ps_cbid[0] != cs_cb0) { ps0_gate = ps_cbid[0]; cb_bound(ps0_gate); } }
  static void cb_gates() {   /* at a projection draw: its $Globals buffers get CPU copies from now on (only while such shaders draw) */
    if (ps_cbid[0] && ps_cbid[0] != ps0_gate && ps_cbid[0] != cs_cb0) { ps0_gate = ps_cbid[0]; cb_bound(ps0_gate); }
    if (vs_cbid[0] && vs_cbid[0] != vs0_gate && vs_cbid[0] != cs_cb0) { vs0_gate = vs_cbid[0]; cb_bound(vs0_gate); } }
  static void cb_gate_vs0() { if (vs_cbid[0] && vs_cbid[0] != vs0_gate && vs_cbid[0] != cs_cb0) { vs0_gate = vs_cbid[0]; cb_bound(vs0_gate); } }
  static bool cb_gate(uint32_t id) {   /* cb_wrote: false = do not copy this write (the copy is marked stale) */
    bool open = pass_n > 0 || id == cs_cb0 || (id == ps0_gate && (cur_pk == PK_SHPROJ_DIR || cur_pk == PK_FXAA)) || (id == vs0_gate && (cur_pk == PK_SHPROJ_DIR || cur_pk == PK_NULL || cur_pk == PK_LSDOWN));
    if (id != ps0_gate && id != vs0_gate) open = true;
    if (open) { if (!cb_stale.empty()) cb_stale.erase(id); return true; }
    cb_stale[id] = 1; return false; }
  static const float *cb_fresh(uint32_t id, uint32_t need) { if (cb_stale.count(id)) return nullptr; return cb_get(id, need); }

  /* ---- 1. light shafts ---- */
  struct Ls { int prev_pk; bool have_blur, saved, used; uint32_t blur_res; Rc<DxvkImage> blur_img; uint32_t blur_mip, blur_layer;
              Rc<DxvkImage> copy; Rc<DxvkImageView> copy_view; long n_used, n_skip; int n_down; bool vs_restore; long n_ray, n_ray_fail; Rc<DxvkBuffer> vb; } ls;
  static void ls_frame() { ls.have_blur = ls.saved = ls.used = false; ls.blur_img = nullptr; ls.prev_pk = -1; ls.n_down = 0; }
  /* the downsample's VS rebuilds each pixel's view ray from its position in the target: o1 = ndc.x row0 + ndc.y row1 + row2 (VS
     cb0, the view's screen -> world rays). Its quad and viewport cover the whole [L|R|UI] target, so the ray of eye column x0
     (width ew of W) must use ndc_view = (ndc + 1) W / ew - 1 - 2 x0 / ew: row0' = a row0, row2' = row2 + b row0. Without it
     each eye's rays were squeezed into a third of its frustum and the shafts' colour (distance to the sun) came out empty in
     the left eye and too weak in the right one. */
  static void ls_ray(int view) {
    auto fail = [&](const char *why) { ls.n_ray_fail++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] light shafts per eye: downsample of view %d left alone (%s)\n", view, why); };
    cb_gate_vs0();
    View *v = g_views.get(g_cs.rtv[0]); if (!v || v->iv == nullptr || !g_cs.nvp || eye_w <= 0 || bb_w <= 0) return fail("no target");
    VkExtent3D e = v->iv->mipLevelExtent(0); double W = bb_w, ew = eye_w, x0 = view ? ew : 0.0;
    if (std::fabs(g_cs.vp[0][0]) > 0.5f || std::fabs(g_cs.vp[0][2] - (float)e.width) > 0.5f || std::fabs((double)e.width - W / 4) > 2) return fail("viewport is not the whole quarter-size target");
    const float *c = vs_cbid[0] == vs0_gate ? cb_fresh(vs_cbid[0], 48) : nullptr; if (!c) return fail("VS constants unknown (first frame)");
    auto it = cbsh.find(vs_cbid[0]); uint32_t n = it->second.n; std::vector<float> f(it->second.f.begin(), it->second.f.begin() + (n + 3) / 4);
    double a = W / ew, b = W / ew - 1.0 - 2.0 * x0 / ew;
    for (int k = 0; k < 4; k++) { double r0 = f[k], r2 = f[8 + k]; f[k] = (float)(a * r0); f[8 + k] = (float)(r2 + b * r0); }
    if (ls.vb == nullptr) { DxvkBufferCreateInfo bi = { }; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 light-shaft rays";
      ls.vb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    Rc<DxvkResourceAllocation> al = dyn_storage(*ls.vb); std::memcpy(al->mapPtr(), f.data(), n);
    emit([b = ls.vb, al] (DxvkContext* cx) mutable { cx->invalidateBuffer(b, std::move(al)); cx->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT, sidx(0) * 16 + 0, DxvkBufferSlice(b)); });
    ls.vs_restore = true; ls.n_ray++;
    if (ls.n_ray <= 2 || ls.n_ray % 12000 == 0) std::fprintf(stderr, "[b1vr] light shafts per eye: downsample of view %d gets its eye column's rays (a %.4g, b %.4g); %ld draws, %ld left alone\n", view, a, b, ls.n_ray, ls.n_ray_fail); }
  static void ls_pre() {   /* pre_draw, stereo on, ls_eye on */
    int k = cur_pk;
    if (k == PK_LSDOWN && sc_n == 2) { if (ls.prev_pk != PK_LSDOWN) ls.n_down++; if (ls.n_down <= 2) ls_ray(ls.n_down - 1); }
    if (k == PK_LSBLUR) { View *v = g_views.get(g_cs.rtv[0]);
      if (v && v->type == 2 && v->iv != nullptr) { ls.have_blur = true; ls.blur_res = v->res; ls.blur_img = v->iv->image(); ls.blur_mip = v->iv->info().mipIndex; ls.blur_layer = v->iv->info().layerIndex; } }
    else if (k == PK_LSDOWN && ls.prev_pk != PK_LSDOWN && ls.have_blur && !ls.saved && sc_n == 2) {   /* the second view's chain starts: keep the first's */
      Rc<DxvkImage> src = ls.blur_img; const DxvkImageCreateInfo &si = src->info();
      if (ls.copy == nullptr || ls.copy->info().format != si.format || ls.copy->info().extent.width != si.extent.width || ls.copy->info().extent.height != si.extent.height) {
        DxvkImageCreateInfo ii = si; ii.debugName = "B1 light shafts, first view"; ii.mipLevels = 1; ii.numLayers = 1;
        ii.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ls.copy = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        DxvkImageViewKey vk = { }; vk.viewType = VK_IMAGE_VIEW_TYPE_2D; vk.format = si.format; vk.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        vk.mipIndex = 0; vk.mipCount = 1; vk.layerIndex = 0; vk.layerCount = 1; vk.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ls.copy_view = ls.copy->createView(vk);
        std::fprintf(stderr, "[b1vr] light shafts per eye: copy target %ux%u fmt %d for the first view's result\n", si.extent.width, si.extent.height, (int)si.format); }
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, ls.blur_mip, ls.blur_layer, 1u }, dl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      VkExtent3D e = src->mipLevelExtent(ls.blur_mip);
      emit([d = ls.copy, dl, s = src, sl, e] (DxvkContext* c) { c->copyImage(d, dl, VkOffset3D{ 0, 0, 0 }, s, sl, VkOffset3D{ 0, 0, 0 }, e); });
      ls.saved = true; }
    else if (k == PK_UBER && ls.saved && !ls.used) {
      ls.used = true;
      if (t4_res == ls.blur_res && ls.copy_view != nullptr) {
        emit([v = ls.copy_view] (DxvkContext* c) mutable { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 0u * 128u + 4u, std::move(v)); });
        ls_rebind = true; ls.n_used++;
        if (ls.n_used <= 2 || ls.n_used % 6000 == 0) std::fprintf(stderr, "[b1vr] light shafts per eye: the first view's tone-map pass reads its own light shafts (res %u copy); %ld frames, %ld left alone\n", ls.blur_res, ls.n_used, ls.n_skip); }
      else { ls.n_skip++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] light shafts per eye: tone-map pass t4 is res %u, not the light-shaft result %u: left alone\n", t4_res, ls.blur_res); } }
    ls.prev_pk = k; }
  static void ls_post() {   /* the game's t4 / VS cb0 back (its bind cache thinks they are bound) */
    if (ls_rebind) { ls_rebind = false; emit([v = t4_iv] (DxvkContext* c) mutable { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, 0u * 128u + 4u, std::move(v)); }); }
    if (ls.vs_restore) { ls.vs_restore = false; Obj *o = obj(vs_cbid[0]); DxvkBufferSlice sl; if (o && o->kind == 1) sl = DxvkBufferSlice(o->buf);
      emit([sl] (DxvkContext* c) mutable { c->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT, sidx(0) * 16 + 0, std::move(sl)); }); } }

  /* ---- 2. sun shadow mask, right eye ---- */
  struct Sh { uint32_t mask = 0; bool replay; long n_rep, n_proj, n_fail, n_chk, n_chk_bad, n_conf; std::vector<float> r_vs1, r_ps2; bool have_r; bool checked; long rep_frame;
              Rc<DxvkBuffer> ub[3];
              /* the window of the frame's whole-scene (directional) shadow sequence in the mask target (see sh_pre) */
              int f_ord, f_pre, f_last, f_endo, f_run, f_pres, prev_pre = -1, prev_end = -1, prev_pres = -1; bool f_dir, f_end, f_conf; long n_win_skip, n_pred_bad;
              std::unordered_map<uint32_t, int> f_cnt; } sh;   /* left-eye clears / stencil volumes per target this frame */
  static bool rect_is(float x0) { return g_cs.nvp && std::fabs(g_cs.vp[0][0] - x0) < 0.5f && std::fabs(g_cs.vp[0][1]) < 0.5f &&
                                         std::fabs(g_cs.vp[0][2] - eye_w) < 0.5f && std::fabs(g_cs.vp[0][3] - bb_h) < 0.5f; }
  static void sh_frame() { sh.have_r = false; sh.checked = false; sh.rep_frame = 0;
    sh.prev_pre = sh.f_dir ? sh.f_pre : -1; sh.prev_end = sh.f_dir ? sh.f_endo : -1; sh.prev_pres = sh.f_dir ? sh.f_pres : -1;
    sh.f_ord = sh.f_pre = sh.f_last = sh.f_endo = sh.f_run = sh.f_pres = 0; sh.f_dir = sh.f_end = sh.f_conf = false; sh.f_cnt.clear(); }
  static void sh_pre() {   /* pre_draw, stereo on, shadow_eye on: decide whether this draw is replayed for the right eye */
    sh.replay = false;
    if (sc_n != 2 || sc_x[0] != 0 || sc_x[1] != eye_w || eye_w <= 0) return;
    View *v = g_views.get(g_cs.rtv[0]); uint32_t rt = v && v->type == 2 ? v->res : 0;
    if (rect_is((float)eye_w)) {   /* a right-eye draw: its view constants (latest), and the check of the previous replay's */
      const float *a = vs_cbid[1] ? cb_get(vs_cbid[1], 80) : nullptr, *b = ps_cbid[2] ? cb_get(ps_cbid[2], 32) : nullptr;
      if (a && b) {
        if (sh.rep_frame && !sh.checked && sh.have_r && cur_pk != PK_ONECOLOR && cur_pk != PK_NULL && rt != sh.mask) { sh.checked = true; sh.n_chk++;
          if (std::memcmp(a, sh.r_vs1.data(), 80) || std::memcmp(b, sh.r_ps2.data(), 32)) { sh.n_chk_bad++;
            if (sh.n_chk_bad <= 4) std::fprintf(stderr, "[b1vr] sun shadow, right eye: the right view's constants changed after the replay (%ld of %ld frames)\n", sh.n_chk_bad, sh.n_chk); } }
        sh.r_vs1.assign(a, a + 20); sh.r_ps2.assign(b, b + 8); sh.have_r = true; }
      if (rt && rt == sh.mask && cur_pk != PK_ONECOLOR) { sh.n_conf++; sh.f_conf = true; if (sh.f_dir) sh.f_end = true;   /* the game draws per eye into this target: another light's sequence (a clear per eye is harmless) */
        if (sh.n_conf <= 6) std::fprintf(stderr, "[b1vr] sun shadow, right eye: the game itself draws into the mask target's right column (frame %ld, shader kind %d, %ld replays this frame)\n", n_frames, cur_pk, sh.rep_frame); }
      return; }
    if (!rect_is(0.f) || !rt) return;
    int k = cur_pk;
    if (k == PK_SHPROJ_DIR) cb_gates();
    if (k == PK_SHPROJ_DIR && rt != sh.mask) { static int nl; if (nl++ < 8) std::fprintf(stderr, "[b1vr] sun shadow, right eye: mask target res %u (was %u)\n", rt, sh.mask);
      sh.mask = rt; sh.prev_pre = sh.prev_end = sh.prev_pres = -1; sh.f_ord = sh.f_cnt.count(rt) ? sh.f_cnt[rt] : 0; sh.f_run = 0; }   /* a new target: its clears / volumes so far count, no prediction this frame */
    if (rt != sh.mask) { if (k == PK_ONECOLOR || k == PK_NULL) { if (sh.f_cnt.size() < 64) sh.f_cnt[rt]++; } return; }
    /* UE3 shares the target between lights (light attenuation buffer): the directional light's sequence (per cascade: clear,
       stencil volume, projection, stencil clear) is drawn for the left eye only; spot / point light sequences go into the same
       target and are drawn per eye by the game itself. Only the directional sequence is replayed. Its projections are
       recognised by their shader; its clears and stencil volumes by their place among the frame's left-eye draws into the
       target, predicted from the previous frame: from the run of clears / volumes right before its first projection up to its last projection
       (+ the stencil clear right after it). Without a prediction (first frame, new target) the draws after the first
       projection are replayed until another kind of draw, or a right-eye draw of the game, goes into the target. */
    const int o = sh.f_ord; bool want = false;
    if (k == PK_SHPROJ_DIR) {
      if (!sh.f_dir) { sh.f_dir = true; sh.f_pre = o; sh.f_pres = sh.f_run;   /* its clear and volume = the run of clears / volumes right before it */
        if (sh.prev_pre >= 0 && sh.prev_pre != o) { sh.n_pred_bad++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] sun shadow, right eye: the sequence started after %d draws (previous frame %d): %s\n", o, sh.prev_pre, o > sh.prev_pre ? "some of the first cascade's clear / volume draws were not replayed" : "draws before the sequence were replayed"); } }
      else if (sh.prev_end >= 0 && o > sh.prev_end) { sh.n_pred_bad++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] sun shadow, right eye: a projection after the predicted end of the sequence (draw %d, previous frame's end %d): its volume was not replayed\n", o, sh.prev_end); }
      sh.f_last = sh.f_endo = o; want = true; }
    else if (k == PK_ONECOLOR || k == PK_NULL) {
      if (k == PK_ONECOLOR && sh.f_dir && o == sh.f_last + 1) sh.f_endo = o;   /* the stencil clear after a projection */
      if (sh.prev_end >= 0) want = (!sh.f_dir && o >= sh.prev_pres && o < sh.prev_pre) || (sh.f_dir && o <= sh.prev_end);
      else want = sh.f_dir && !sh.f_end;
      if (sh.f_end) want = false;
      if (!want) sh.n_win_skip++; }
    else if (sh.f_dir) sh.f_end = true;
    if (k != PK_ONECOLOR && k != PK_NULL) sh.f_run = o + 1;   /* a run of clears / volumes starts after any other draw */
    sh.f_ord++;
    { static int pr = -1; if (pr < 0) { const char *e = std::getenv("BSI_B1_SHEYEPROBE"); pr = e && std::atoi(e) > 0; } if (pr) want = false; }   /* b122 probe: no right-eye sun replay */
    sh.replay = want; }
  static Rc<DxvkBuffer> sh_buf(int i) {
    if (sh.ub[i] == nullptr) { DxvkBufferCreateInfo bi = { }; bi.size = 4096;
      bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 right-eye shadow constants";
      sh.ub[i] = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    return sh.ub[i]; }
  static void sh_bind(int st, uint32_t slot, int bi, const void *data, uint32_t n) {   /* our buffer (fresh storage) at a slot */
    Rc<DxvkBuffer> b = sh_buf(bi); Rc<DxvkResourceAllocation> a = dyn_storage(*b); std::memcpy(a->mapPtr(), data, n);
    emit([b, a, sb = sbit(st), s = sidx(st) * 16 + slot] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); c->bindUniformBuffer(sb, s, DxvkBufferSlice(b)); }); }
  static void sh_unbind(int st, uint32_t slot, uint32_t id) {   /* the game's buffer back */
    Obj *o = obj(id); DxvkBufferSlice sl; if (o && o->kind == 1) sl = DxvkBufferSlice(o->buf);
    emit([sb = sbit(st), s = sidx(st) * 16 + slot, sl] (DxvkContext* c) mutable { c->bindUniformBuffer(sb, s, std::move(sl)); }); }
  static void sh_post(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    sh.replay = false;
    auto fail = [&](const char *why) { sh.n_fail++; static int nl; if (nl++ < 8) std::fprintf(stderr, "[b1vr] sun shadow, right eye: draw not replayed (%s)\n", why); };
    if (!sh.have_r) return fail("no right-eye draw with its view constants seen yet this frame");
    if (!vs_shadow_eye_consts) return fail("vr_stereo.c without vs_shadow_eye_consts");
    const float *vs1 = vs_cbid[1] ? cb_get(vs_cbid[1], 80) : nullptr, *ps2 = ps_cbid[2] ? cb_get(ps_cbid[2], 32) : nullptr;
    if (!vs1 || !ps2) return fail("left view constants unknown");
    float vs1o[20], vs0r[12], ps0r3[4]; const float *ps0 = nullptr; const float *vs0 = nullptr;
    bool proj = cur_pk == PK_SHPROJ_DIR;
    if (proj) { ps0 = ps0_gate == ps_cbid[0] ? cb_fresh(ps_cbid[0], 64) : nullptr; vs0 = vs0_gate == vs_cbid[0] ? cb_fresh(vs_cbid[0], 48) : nullptr;
                if (!ps0 || !vs0) return fail("projection constants unknown"); }
    if (!vs_shadow_eye_consts(vs1, sh.r_vs1.data(), vs0, ps0, vs1o, proj ? vs0r : nullptr, proj ? ps0r3 : nullptr)) return fail("degenerate view matrix");
    /* viewport (and scissor) one eye to the right */
    float vp0[16][6]; int32_t sc0[16][4]; std::memcpy(vp0, g_cs.vp, sizeof vp0); std::memcpy(sc0, g_cs.sc, sizeof sc0);
    g_cs.vp[0][0] += (float)eye_w; if (g_cs.nsc) { g_cs.sc[0][0] += eye_w; g_cs.sc[0][2] += eye_w; }
    sb87::replay_pre(vs1o);   /* sb87: the right eye's volume bbox / projection scissor + depth bounds (right view's constants) */
    apply_viewports();
    { std::vector<float> b(sh.r_vs1.size()); std::memcpy(b.data(), vs1o, 80); sh_bind(0, 1, 0, b.data(), 80); }
    { float p2[8]; std::memcpy(p2, ps2, 32); p2[3] = sh.r_ps2[3]; p2[0] = sh.r_ps2[0]; p2[1] = sh.r_ps2[1]; p2[2] = sh.r_ps2[2]; sh_bind(1, 2, 1, p2, 32); }
    if (proj) {
      auto it = cbsh.find(ps_cbid[0]); uint32_t n = it->second.n; std::vector<float> p0(it->second.f.begin(), it->second.f.begin() + (n + 3) / 4);
      std::memcpy(&p0[12], ps0r3, 16); sh_bind(1, 0, 2, p0.data(), n);
      auto iv = cbsh.find(vs_cbid[0]); uint32_t m = iv->second.n; std::vector<float> v0(iv->second.f.begin(), iv->second.f.begin() + (m + 3) / 4);
      std::memcpy(&v0[0], vs0r, 48);   /* VS cb0: its own buffer */
      { static Rc<DxvkBuffer> vb; if (vb == nullptr) { DxvkBufferCreateInfo bi = { }; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
          bi.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 right-eye shadow VS cb0";
          vb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
        Rc<DxvkResourceAllocation> a = dyn_storage(*vb); std::memcpy(a->mapPtr(), v0.data(), m);
        emit([b = vb, a] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); c->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT, sidx(0) * 16 + 0, DxvkBufferSlice(b)); }); }   /* clang: no capture of a static */
      sh.n_proj++; }
    if (sb87::clr_take()) sb87::clr_emit();   /* b112: the right eye's stencil-clear quad as a clear */
    else if (sb87::tile_pending()) sb87::tile_emit();   /* b102: the right eye's tiled quad */
    else if (dc) { VkDrawIndirectCommand d = *dc; emit([d] (DxvkContext* c) { c->draw(1, &d); }); }
    else { VkDrawIndexedIndirectCommand d = *dic; emit([d] (DxvkContext* c) { c->drawIndexed(1, &d); }); }
    sh_unbind(0, 1, vs_cbid[1]); sh_unbind(1, 2, ps_cbid[2]);
    if (proj) { sh_unbind(1, 0, ps_cbid[0]); sh_unbind(0, 0, vs_cbid[0]); }
    sb87::replay_post();   /* sb87 */
    std::memcpy(g_cs.vp, vp0, sizeof vp0); std::memcpy(g_cs.sc, sc0, sizeof sc0); apply_viewports();
    sh.n_rep++; sh.rep_frame++; g_ndraw++;
    if (sh.n_proj == 1 && proj) std::fprintf(stderr, "[b1vr] sun shadow, right eye: first projection replayed (mask res %u, eye %d px)\n", sh.mask, eye_w); }

  /* ---- 3. FXAA per eye (fxaa_eye) ----
     The game's FXAA (FXAA 3.11 quality, luma in alpha) is drawn once per eye view: viewport = the whole [L|R|UI] buffer,
     rcpFrame = 1 / buffer size, and a quad that covers that view's eye column (UV = buffer position / buffer size). Per pixel
     that is the eye alone's FXAA, except where its taps and its edge-end search cross an eye border: the eye rendered alone
     clamps at its image edge (clamped sampler), the stereo buffer hands it the other eye's (or the UI column's) pixels.
     fxaa_eye=1: at the first FXAA draw of a frame, two buffer-sized copies of the input are made, one per eye, each holding
     that eye's column at its own place and, in a margin of FX_MARGIN px on each side of it, copies of the eye's edge column
     (what a clamped sampler returns past the edge of the eye rendered alone; the left eye's outer edge is the texture edge).
     Every FXAA draw of the game (its shader, constants, viewport and quad) is then drawn twice, scissored to each eye column
     with that eye's copy at t0, and once scissored to the UI column with the game's input: each eye pixel gets exactly the
     taps the eye alone gets (FXAA's reach is <= 30 px for every FXAA 3.11 preset), the quad decides which eye a draw covers.
     History: the first build of this (pp76 binaries e/f) drew into eye-sized copies with its own viewport: the quad, which
     covers one eye column, then filled only a third of the viewport; binary h skipped the second draw of a frame as a
     duplicate, but the two draws cover different eyes. Both found in the cloud runs (FAKE_SAME, FXAA check), replaced. */
  enum { FX_MARGIN = 128 };
  struct Fx { Rc<DxvkImage> img[2]; Rc<DxvkImageView> view[2]; long n, n_fail, prep_frame = -1; uint32_t prep_res; bool prep_ok; } fx;
  /* ---- b124: FXAA edge/stencil split (BSI_B1_FXAASPLIT; chat 4 task #23, README_B124.md) ----
     The game's FXAA shader spends most of its time in wave divergence: one edge pixel makes the whole wave run the long
     edge-end search. Split (per eye column, inside fx_draw): our own D24S8 stencil image (buffer-sized) is cleared to 0 and
     bound with the game's colour target; pass 1 = the twin (rhi_b1_fxsplit.h: the game's declarations + preamble unchanged,
     the edge block replaced by discard_nz of the same test) with stencil ALWAYS / REPLACE 1: non-edge pixels get exactly the
     original's value and are marked; pass 2 = the UNCHANGED game shader with stencil EQUAL 0: only the edge pixels, in dense
     waves. Every pixel runs the original instructions it ran before -> the same bits (proved by mode 2).
     BSI_B1_FXAASPLIT = 0 off (default) | 1 on | 2 CHECK: the picture is the game's path; every BSI_B1_FXAASPLIT_EVERY-th frame
     (300) the split is also drawn into a copy of the target and both eye columns are read back and compared byte for byte
     ([b1fxaa] CHECK lines; must report 0 differing pixels). The UI column always takes the game's path. */
  static int fxs_mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_FXAASPLIT"); v = e && *e ? std::atoi(e) : 0; if (v < 0 || v > 2) v = 0; } return v; }
  static int fxs_every() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_FXAASPLIT_EVERY"); v = e && *e ? std::atoi(e) : 300; if (v < 3) v = 3; } return v; }
  struct Fxs { std::mutex mu; std::unordered_map<uint32_t, uint32_t> twin;   /* game PS id -> twin id (b1fx_ps_created) */
               Rc<DxvkImage> st; Rc<DxvkImageView> stv; uint32_t sw = 0, sh = 0; VkFormat sf = VK_FORMAT_UNDEFINED;
               long frames = 0, n_split = 0, n_fb = 0; int fb_logged = 0;
               bool chk_frame = false, chk_copied = false, chk_have = false; Rc<DxvkImage> chk_img; Rc<DxvkImageView> chk_rtv; Rc<DxvkBuffer> rb[2]; VkDeviceSize rbsz = 0;
               uint32_t chk_w = 0, chk_h = 0, chk_cols = 0, chk_esz = 0; long chk_n = 0, chk_bad = 0; uint64_t chk_px = 0, chk_diff = 0; } fxs;
  static bool fxs_stencil(uint32_t W, uint32_t H) {
    if (fxs.st != nullptr && fxs.sw == W && fxs.sh == H) return true;
    try {
      VkFormat f = VK_FORMAT_D24_UNORM_S8_UINT;
      if (!(g_dev->getFormatFeatures(f).optimal & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) f = VK_FORMAT_D32_SFLOAT_S8_UINT;
      DxvkImageCreateInfo ii = { }; ii.type = VK_IMAGE_TYPE_2D; ii.format = f; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT; ii.extent = { W, H, 1u }; ii.numLayers = 1; ii.mipLevels = 1;
      ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; ii.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      ii.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; ii.debugName = "B1 FXAA split stencil";
      fxs.st = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = f; k.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
      k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1; k.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      fxs.stv = fxs.st->createView(k); fxs.sw = W; fxs.sh = H; fxs.sf = f;
      std::fprintf(stderr, "[b1fxaa] split stencil %ux%u fmt %d\n", W, H, (int)f); return true;
    } catch (const DxvkError &e) { fxs.st = nullptr; fxs.stv = nullptr; std::fprintf(stderr, "[b1fxaa] stencil image failed: %s\n", e.message().c_str()); return false; } }
  static bool fxs_chkimg(const Rc<DxvkImage> &rt) {   /* mode 2: a copy target like the game's FXAA target + two host buffers */
    VkExtent3D e = rt->mipLevelExtent(0); VkFormat fmt = rt->info().format; const DxvkFormatInfo *fi = lookupFormatInfo(fmt);
    if (!fi || !fi->elementSize || (fi->aspectMask & ~VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    try {
      if (fxs.chk_img == nullptr || fxs.chk_img->info().format != fmt || fxs.chk_w != e.width || fxs.chk_h != e.height) {
        DxvkImageCreateInfo ii = rt->info(); ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.mipLevels = 1; ii.numLayers = 1; ii.flags = 0; ii.viewFormatCount = 0; ii.viewFormats = nullptr; ii.debugName = "B1 FXAA split check copy";
        ii.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; ii.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ii.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; fxs.chk_img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = fmt; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
        k.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; fxs.chk_rtv = fxs.chk_img->createView(k);
        fxs.chk_w = e.width; fxs.chk_h = e.height; }
      VkDeviceSize sz = (VkDeviceSize)e.width * e.height * fi->elementSize;
      if (fxs.rb[0] == nullptr || fxs.rbsz != sz) for (int i = 0; i < 2; i++) {
        DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT;
        bi.debugName = "B1 FXAA split check readback"; fxs.rb[i] = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); fxs.rbsz = sz; }
      fxs.chk_esz = fi->elementSize; return true;
    } catch (const DxvkError &e2) { std::fprintf(stderr, "[b1fxaa] check setup failed: %s\n", e2.message().c_str()); return false; } }
  static void fxs_compare() {   /* the previous check frame's two read-backs (one GPU wait) */
    fxs.chk_have = false;
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint8_t *a = (const uint8_t *)fxs.rb[0]->mapPtr(0), *b = (const uint8_t *)fxs.rb[1]->mapPtr(0); if (!a || !b) return;
    const size_t esz = fxs.chk_esz, row = (size_t)fxs.chk_w * esz, cols = std::min<size_t>(fxs.chk_cols, fxs.chk_w); uint64_t px = 0, d = 0;
    for (uint32_t y = 0; y < fxs.chk_h; y++) { const uint8_t *pa = a + y * row, *pb = b + y * row;
      for (size_t x = 0; x < cols; x++) { px++; if (std::memcmp(pa + x * esz, pb + x * esz, esz)) d++; } }
    fxs.chk_n++; fxs.chk_px += px; fxs.chk_diff += d; if (d) fxs.chk_bad++;
    if (fxs.chk_n <= 3 || d || fxs.chk_n % 10 == 0)
      std::fprintf(stderr, "[b1fxaa] CHECK %ld: eye pixels %llu, differing %llu | total %ld checks, %ld with differences, %llu of %llu pixels differ; split draws %ld, fallbacks %ld\n",
                   fxs.chk_n, (unsigned long long)px, (unsigned long long)d, fxs.chk_n, fxs.chk_bad, (unsigned long long)fxs.chk_diff, (unsigned long long)fxs.chk_px, fxs.n_split, fxs.n_fb); }
  static void fx_frame() { fx.prep_ok = false;
    if (fxs_mode()) {
      if (fxs.chk_have) fxs_compare();
      fxs.frames++; fxs.chk_frame = fxs_mode() == 2 && fxs.frames % fxs_every() == 0; fxs.chk_copied = false;
      if (fxs_mode() == 1 && fxs.frames % 3600 == 0) std::fprintf(stderr, "[b1fxaa] split: %ld FXAA draws split, %ld left to the game's path\n", fxs.n_split, fxs.n_fb); } }
  static void fx_pre() {
    if (cur_pk != PK_FXAA || b1aa_owns_fxaa()) { fx.prep_ok = false; return; }   /* any other draw in between: the copies are made again; merge b76: aa76 owns the FXAA draw */
    auto fail = [&](const char *why) { fx.n_fail++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] FXAA per eye: draw left alone (%s)\n", why); };
    if (!vs_fx_copy_plan) return fail("vr_stereo.c without vs_fx_copy_plan");
    if (eye_w <= FX_MARGIN || bb_w < 2 * eye_w || !g_cs.nvp || std::fabs(g_cs.vp[0][0]) > 0.5f || std::fabs(g_cs.vp[0][2] - bb_w) > 0.5f || std::fabs(g_cs.vp[0][3] - bb_h) > 0.5f) return fail("viewport is not the whole buffer");
    Obj *so = obj(t0_res); if (!so || so->kind != 2 || t0_iv == nullptr) return fail("no input texture");
    VkExtent3D se = so->img->mipLevelExtent(0); if ((int)se.width != bb_w || (int)se.height != bb_h || so->img->info().mipLevels != 1) return fail("input is not buffer-sized");
    pp_replace = true; }
  static void fx_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    Obj *so = obj(t0_res); const DxvkImageCreateInfo &si = so->img->info();
    const int W = bb_w, H = bb_h, ew = eye_w, m = FX_MARGIN;
    if (!fx.prep_ok || fx.prep_frame != n_frames || fx.prep_res != t0_res) {   /* the two eye copies of this frame's input */
      for (int e = 0; e < 2; e++) {
        if (fx.img[e] == nullptr || fx.img[e]->info().format != si.format || (int)fx.img[e]->info().extent.width != W || (int)fx.img[e]->info().extent.height != H) {
          DxvkImageCreateInfo ii = si; ii.extent = { (uint32_t)W, (uint32_t)H, 1u }; ii.mipLevels = 1; ii.numLayers = 1; ii.debugName = "B1 FXAA eye input";
          ii.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
          fx.img[e] = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          DxvkImageViewKey vk = t0_iv->info(); vk.mipIndex = 0; vk.mipCount = 1; vk.layerIndex = 0; vk.layerCount = 1; fx.view[e] = fx.img[e]->createView(vk);
          if (e == 1) std::fprintf(stderr, "[b1vr] FXAA per eye: two input copies %dx%d fmt %d, margin %d px\n", W, H, (int)si.format, m); }
        std::vector<std::array<int, 4>> plan(40); int np = vs_fx_copy_plan(e * ew, (e + 1) * ew, W, m, reinterpret_cast<int (*)[4]>(plan.data()), 40);   /* vr_stereo.c, host-tested */
        plan.resize(np > 0 ? np : 0);
        emit([d = fx.img[e], s = so->img, plan = std::move(plan), H] (DxvkContext* c) {   /* the eye column + its clamped margins */
          VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
          for (const auto &p : plan) c->copyImage(d, sl, VkOffset3D{ p[2], 0, 0 }, p[0] ? s : d, sl, VkOffset3D{ p[1], 0, 0 }, VkExtent3D{ (uint32_t)p[3], (uint32_t)H, 1u }); }); }
      fx.prep_ok = true; fx.prep_frame = n_frames; fx.prep_res = t0_res; }
    float vp0[6]; std::memcpy(vp0, g_cs.vp[0], sizeof vp0);
    auto draw_it = [&]() { if (dc) { VkDrawIndirectCommand d = *dc; emit([d] (DxvkContext* c) { c->draw(1, &d); }); }
                           else { VkDrawIndexedIndirectCommand d = *dic; emit([d] (DxvkContext* c) { c->drawIndexed(1, &d); }); } };
    auto col = [&](int e) -> bool {   /* viewport / scissor of column e and its input at t0 (eyes: their copies; UI column: the game's input) */
      int x0 = e * ew, w = e < 2 ? ew : W - 2 * ew; if (w <= 0) return false;
      DxvkViewport v = { }; v.viewport.x = vp0[0]; v.viewport.y = vp0[3] + vp0[1]; v.viewport.width = vp0[2]; v.viewport.height = -vp0[3];
      v.viewport.minDepth = vp0[4]; v.viewport.maxDepth = vp0[5]; v.scissor.offset = { x0, 0 }; v.scissor.extent = { (uint32_t)w, (uint32_t)H };
      emit([v, iv = e < 2 ? fx.view[e] : t0_iv] (DxvkContext* c) mutable { c->setViewports(1, &v); c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, sidx(1) * 128 + 0, std::move(iv)); });
      return true; };
    /* b124: FXAA split (fxs_mode 1) / its check (mode 2, check frames) */
    const int fm = fxs_mode(); uint32_t tid = 0; const char *fbwhy = nullptr; Rc<DxvkImage> rti; DxvkRenderTargets real = { };
    if (fm) {
      { std::lock_guard<std::mutex> lk(fxs.mu); auto it = fxs.twin.find(fx_ps); if (it != fxs.twin.end()) tid = it->second; }
      View *rv = g_views.get(g_cs.rtv[0]);
      if (!tid) fbwhy = "no twin for the bound FXAA shader";
      else if (!rv || rv->type != 2 || rv->iv == nullptr) fbwhy = "no colour target";
      else { VkExtent3D te = rv->iv->mipLevelExtent(0); rti = rv->iv->image(); if ((int)te.width != W || (int)te.height != H || rv->iv->info().mipIndex || rv->iv->info().layerIndex) fbwhy = "target is not the buffer-sized mip 0"; }
      for (uint32_t i = 1; i < 8 && !fbwhy; i++) if (g_cs.rtv[i]) fbwhy = "more than one colour target";
      if (!fbwhy && !fxs_stencil((uint32_t)W, (uint32_t)H)) fbwhy = "no stencil image";
      if (!fbwhy) { for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) real.color[i].view = v->iv; }
                    View *dv = g_views.get(g_cs.dsv); if (dv && dv->type == 3) real.depth.view = dv->iv; }
      if (fbwhy) { fxs.n_fb++; if (fxs.fb_logged++ < 6) std::fprintf(stderr, "[b1fxaa] draw left to the game's path: %s\n", fbwhy); } }
    const bool split = fm == 1 && !fbwhy, check = fm == 2 && !fbwhy && fxs.chk_frame && fxs_chkimg(rti);
    if (check && !fxs.chk_copied) {   /* the target as it is before this frame's first FXAA draw -> the check copy */
      VkExtent3D e = rti->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      emit([di = fxs.chk_img, si = rti, sl, e] (DxvkContext* c) { c->copyImage(di, sl, VkOffset3D{ 0, 0, 0 }, si, sl, VkOffset3D{ 0, 0, 0 }, e); }); fxs.chk_copied = true; }
    if (!split) for (int e = 0; e < 3; e++) { if (col(e)) draw_it(); }   /* the game's path (in a check frame: the real target) */
    if (split || check) {
      DxvkRenderTargets alt = real; alt.depth.view = fxs.stv; if (check) alt.color[0].view = fxs.chk_rtv;
      DxvkDepthStencilState d1 = { }, d2 = { }; DxvkStencilOp o1, o2;
      o1.setFailOp(VK_STENCIL_OP_KEEP); o1.setPassOp(VK_STENCIL_OP_REPLACE); o1.setDepthFailOp(VK_STENCIL_OP_KEEP); o1.setCompareOp(VK_COMPARE_OP_ALWAYS); o1.setCompareMask(0xff); o1.setWriteMask(0xff);
      o2.setFailOp(VK_STENCIL_OP_KEEP); o2.setPassOp(VK_STENCIL_OP_KEEP); o2.setDepthFailOp(VK_STENCIL_OP_KEEP); o2.setCompareOp(VK_COMPARE_OP_EQUAL); o2.setCompareMask(0xff); o2.setWriteMask(0);
      d1.setDepthTest(false); d1.setDepthWrite(false); d1.setStencilTest(true); d1.setStencilOpFront(o1); d1.setStencilOpBack(o1); d1.normalize();
      d2.setDepthTest(false); d2.setDepthWrite(false); d2.setStencilTest(true); d2.setStencilOpFront(o2); d2.setStencilOpBack(o2); d2.normalize();
      Rc<DxvkShader> tw = b1dx_shader_get(tid), gps = b1dx_shader_get(fx_ps);
      VkClearValue cv = { }; cv.depthStencil.depth = 0.0f; cv.depthStencil.stencil = 0u;
      emit([alt, sv = fxs.stv, cv] (DxvkContext* c) mutable { DxvkAttachment a = { }; a.view = sv;
        c->bindRenderTargets(std::move(alt), 0u); c->clearRenderTarget(a, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, cv, 0u); });
      for (int e = 0; e < 2; e++) {
        if (!col(e)) continue;
        emit([tw, d1] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(tw)); c->setDepthStencilState(d1); c->setStencilReference(1u); });
        draw_it();   /* pass 1: non-edge pixels (the twin), marked 1 */
        emit([gps, d2] (DxvkContext* c) mutable { c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(gps)); c->setDepthStencilState(d2); c->setStencilReference(0u); });
        draw_it(); }  /* pass 2: the game's shader on the unmarked (edge) pixels */
      /* the game's state back: targets, PS, depth-stencil state + reference */
      DxvkDepthStencilState gd = { }; { DSS *d = g_dsss.get(g_cs.dss); if (d) gd = d->s; else { gd.setDepthTest(true); gd.setDepthWrite(true); gd.setDepthCompareOp(VK_COMPARE_OP_LESS); gd.normalize(); } }
      if (sb87_lrz_mod()) { gd.setStencilTest(false); gd.normalize(); }   /* s2 had dropped the stencil test of the game's state */
      emit([real, gps, gd, ref = g_cs.sref] (DxvkContext* c) mutable { DxvkRenderTargets r = real; c->bindRenderTargets(std::move(r), 0u);
        c->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(gps)); c->setDepthStencilState(gd); c->setStencilReference(ref & 0xffu); });
      if (split) { if (col(2)) draw_it(); fxs.n_split++; }   /* the UI column: the game's path */
      if (check) {   /* both results -> host buffers (overwritten by a later FXAA draw of the frame; compared at the next frame) */
        VkExtent3D e = rti->mipLevelExtent(0); VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u }; VkFormat f = rti->info().format;
        emit([b0 = fxs.rb[0], b1 = fxs.rb[1], ri = rti, ci = fxs.chk_img, sl, e, f] (DxvkContext* c) {
          c->copyImageToBuffer(b0, 0, 0, 0, f, ri, sl, VkOffset3D{ 0, 0, 0 }, e); c->copyImageToBuffer(b1, 0, 0, 0, f, ci, sl, VkOffset3D{ 0, 0, 0 }, e); });
        fxs.chk_have = true; fxs.chk_cols = (uint32_t)(2 * ew); } }
    emit([iv = t0_iv] (DxvkContext* c) mutable { c->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, sidx(1) * 128 + 0, std::move(iv)); });   /* t0 = the game's view again */
    g_cs.vpdirty = true; apply_viewports();   /* the game's viewport / scissor state back (t0 is the game's view again) */
    fx.n++; if (fx.n <= 2 || fx.n % 12000 == 0) std::fprintf(stderr, "[b1vr] FXAA per eye: draw %ld drawn per eye column (eye %dx%d, margin %d) + the UI column\n", fx.n, ew, H, m); }
  static void pp_replace_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) { pp_replace = false; fx_draw(dc, dic); }

  /* ---- 4. bloom / DOF blend chain of the right eye (bloom_eye) ----
     After the half-size scene copy (TFilterPixelShaderDepthInAlpha, both eyes) each view runs a quarter-size chain (downsample,
     separable blur, blend; MRTs, results copied to textures) right before its tone-map pass. The game gives BOTH views the
     first view's rectangle: every blur tap is clamped to the left eye's column (cb0[13] / cb0[10] = the left third) and the
     second view's downsample writes the left column only. So the second chain recomputes the LEFT eye's bloom and the right
     eye's tone-map pass read bloom that is not its own (cloud, FAKE_SAME: the second chain's right column is empty). bloom_eye=1:
     before the second chain rhi_b1 copies the right eye's column of the half-size input over its left column (the second
     chain then computes the right eye's bloom, with the same clamping the left eye's gets), and before the second tone-map
     pass it copies the left column of each quarter-size input (t1..t3) to the right column. Same work for the GPU, 5 small
     copies per frame. */
  struct Bl { int phase; long n, n_fail; Rc<DxvkImage> tmp; } bl;
  static void bl_frame() { bl.phase = 0; }
  static void bl_copy_col(uint32_t res, int from, int to, int colw, const char *what) {   /* column copy inside one texture, via a scratch image */
    Obj *o = obj(res); if (!o || o->kind != 2) return; const DxvkImageCreateInfo &si = o->img->info(); VkExtent3D e = o->img->mipLevelExtent(0);
    if (bl.tmp == nullptr || bl.tmp->info().format != si.format || bl.tmp->info().extent.width < (uint32_t)colw || bl.tmp->info().extent.height < e.height) {
      DxvkImageCreateInfo ii = si; ii.extent = { (uint32_t)colw, e.height, 1u }; ii.mipLevels = 1; ii.numLayers = 1; ii.debugName = "B1 bloom eye column";
      ii.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; bl.tmp = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); }
    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    emit([t = bl.tmp, i = o->img, sl, from, to, colw, h = e.height] (DxvkContext* c) {
      c->copyImage(t, sl, VkOffset3D{ 0, 0, 0 }, i, sl, VkOffset3D{ from, 0, 0 }, VkExtent3D{ (uint32_t)colw, h, 1u });
      c->copyImage(i, sl, VkOffset3D{ to, 0, 0 }, t, sl, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ (uint32_t)colw, h, 1u }); });
    (void)what; }
  static int bl_div(uint32_t res) {   /* 2 = half-size of the buffer, 4 = quarter-size, else 0 */
    Obj *o = obj(res); if (!o || o->kind != 2) return 0; VkExtent3D e = o->img->mipLevelExtent(0);
    for (int d : { 2, 4 }) if (std::abs((int)e.width * d - bb_w) <= d && std::abs((int)e.height * d - bb_h) <= d) return d; return 0; }
  static void bl_pre() {
    if (sc_n != 2 || eye_w <= 0) return;
    if (cur_pk == PK_UBER) {
      if (bl.phase == 0) { bl.phase = 1; return; }   /* the first view's tone map: nothing to do */
      if (bl.phase == 2) {   /* the second view's: its quarter-size inputs, left column -> right column */
        int k = 0; for (int i = 1; i <= 3; i++) if (bl_div(ps_tres[i]) == 4) { bl_copy_col(ps_tres[i], 0, eye_w / 4, eye_w / 4, "quarter"); k++; }
        bl.phase = 3; bl.n++;
        if (bl.n <= 2 || bl.n % 12000 == 0) std::fprintf(stderr, "[b1vr] bloom per eye: the second view's chain ran on the right eye's half-size column, %d quarter-size inputs copied to the right column; %ld frames, %ld left alone\n", k, bl.n, bl.n_fail);
        return; }
      if (bl.phase == 1) { bl.n_fail++; static int nl; if (nl++ < 6) std::fprintf(stderr, "[b1vr] bloom per eye: no second chain seen before the second tone-map pass: left alone\n"); bl.phase = 3; }
      return; }
    if (bl.phase == 1) {   /* the first draw of the second chain: reads the half-size scene copy, renders a quarter-size target */
      View *v = g_views.get(g_cs.rtv[0]); if (!v || v->type != 2) return;
      if (bl_div(t0_res) == 2 && bl_div(v->res) == 4) { bl_copy_col(t0_res, eye_w / 2, 0, eye_w / 2, "half"); bl.phase = 2; } } }

  /* draw(): before the draw is emitted; post(): right after it (b1dx_c_draw*) */
  static void pp_pre() { if (lse) ls_pre(); if (she) sh_pre(); if (fxe) fx_pre(); if (ble) bl_pre(); }
  static bool pp_post_wanted() { return ls_rebind || ls.vs_restore || sh.replay; }
  static void pp_post(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) { if (sh.replay) sh_post(dc, dic); if (ls_rebind || ls.vs_restore) ls_post(); }
  static void pp_frame() {   /* present (render thread): latch the switches, reset, status line */
    int l = on && vr_stereo_pp_eye ? vr_stereo_pp_eye(0) : 0, s = on && vr_stereo_pp_eye ? vr_stereo_pp_eye(1) : 0, x = on && vr_stereo_pp_eye ? vr_stereo_pp_eye(2) : 0,
        b = on && vr_stereo_pp_eye ? vr_stereo_pp_eye(3) : 0;
    static int last = -1; int now = on ? l + 2 * s + 4 * x + 8 * b : -1;
    if (now != last && on) std::fprintf(stderr, "[b1vr] post-process per eye: light shafts %s, sun shadow mask right eye %s, FXAA %s, bloom right eye %s\n", l ? "on (ls_eye=1)" : "off (build 75)",
                                        s ? "on (shadow_eye=1)" : "off (build 75)", x ? "per eye (fxaa_eye=1)" : "whole buffer (build 75)", b ? "on (bloom_eye=1)" : "off (build 75)");
    last = now; lse = l; she = s; fxe = x; ble = b; ls_frame(); sh_frame(); fx_frame(); bl_frame();
    static long lastn = -1; if (on && n_frames % 600 == 0 && sh.n_rep != lastn) { lastn = sh.n_rep;
      std::fprintf(stderr, "[b1vr] sun shadow, right eye so far: %ld draws replayed (%ld projections), %ld not (no constants), view check %ld ok / %ld changed, %ld right-eye draws into the target by the game (other lights), %ld clears/volumes outside the directional sequence left alone, %ld first-cascade predictions off\n",
                   sh.n_rep, sh.n_proj, sh.n_fail, sh.n_chk - sh.n_chk_bad, sh.n_chk_bad, sh.n_conf, sh.n_win_skip, sh.n_pred_bad); } }
}

/* ---- pp76: FXAA and game-settings diagnostics, flat and VR (BSI_B1_PPDIAG=0: off) ----
   Why: the game draws its FXAA pass only while GSystemSettings.EnableFXAA is 1, and it sets that value itself when the options
   apply the graphics preset (XUserOptionsManager f_009a99d0: the AppCompatBucket of GraphicsQualityLevel + 1; XEngine.ini's
   GraphicsQualityLevel 0 -> bucket 1 "very low", FXAA off; 5 -> bucket 6, FXAA on). One Quest log then says which preset the
   device runs, whether the FXAA pass is drawn and whether it changes the picture:
   - "[pp76] game settings": the guest's EnableFXAA, EnableLightShafts, GraphicsQualityLevel, DynamicShadowsLevel,
     AmbientOcclusionLevel and MaxShadowResolution at the first present and whenever one of them changes (max 24 lines);
   - "[pp76] FXAA check": 120 presents after the first FXAA draw and again 3600 and 10800 presents later (on the Quest about 1 and 4 minutes) (3 checks, each one GPU readback of the
     input and the output = a one-frame stall): which FXAA shader (SM4 / SM5), the input's format / view format / swizzle,
     the tone-map pass's target format and colour write mask, FXAA's target format and write mask; a 16-bin histogram of
     the input's alpha (FXAA's luma) and its correlation with the colour's luma; per eye column the share of pixels the FXAA
     draw changed (cloud, Quest preset: 5-8 % of an eye's pixels; ~0 % = FXAA has no effect on this device);
   - "[pp76] FXAA pass": per 600 presents, how many frames drew the FXAA pass and how many tone-mapped straight into the back
     buffer (no FXAA), and the first present of each change. */
extern "C" uint8_t *M;   /* rt_linux.c: guest memory base */
namespace b1vr {
  int fxd_on = -1;
  static int fxd_max() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_FXCHECK"); v = e && *e ? std::atoi(e) : 0; if (v < 0) v = 0; if (v > 3) v = 3; } return v; }   /* b106: the FXAA check readbacks (3 one-off >100 ms frames per run) are OFF by default; BSI_B1_FXCHECK=3 restores them (chat 5's request) */
  struct FxdSet { uint32_t a; const char *n; };
  static const FxdSet fxd_set[] = { { 0x13a4d14u, "EnableFXAA" }, { 0x13a4d10u, "EnableLightShafts" }, { 0x13a4d40u, "GraphicsQualityLevel" },
                                    { 0x13a4d30u, "DynamicShadowsLevel" }, { 0x13a4d34u, "AmbientOcclusionLevel" }, { 0x13a50b0u, "MaxShadowResolution" } };
  struct Fxd { uint32_t sv[6]; bool sv_ok; int sv_lines; long frames, fx_frames, bb_frames, last_fx, last_bb, next_check; int fx_now, bb_now, state, checks;
               uint32_t uber_rt_fmt, uber_wmask, uber_res; bool uber_seen, post; std::vector<uint8_t> in; uint32_t in_w, in_h; bool in_bgra; } fxd;
  static uint32_t guest32(uint32_t a) { uint32_t v = 0; if (M) std::memcpy(&v, M + a, 4); return v; }
  static void fxd_settings() {
    uint32_t v[6]; for (int i = 0; i < 6; i++) v[i] = guest32(fxd_set[i].a);
    if (fxd.sv_ok && !std::memcmp(v, fxd.sv, sizeof v)) return;
    if (fxd.sv_lines++ < 24) { std::string s; char b[80];
      for (int i = 0; i < 6; i++) { if (fxd.sv_ok && v[i] != fxd.sv[i]) std::snprintf(b, sizeof b, "%s%s %u -> %u", i ? ", " : "", fxd_set[i].n, fxd.sv[i], v[i]);
                                    else std::snprintf(b, sizeof b, "%s%s %u", i ? ", " : "", fxd_set[i].n, v[i]); s += b; }
      std::fprintf(stderr, "[pp76] game settings%s (present %ld): %s%s\n", fxd.sv_ok ? " changed" : "", fxd.frames, s.c_str(),
                   v[0] ? "" : " (EnableFXAA 0: the game does not draw its FXAA pass)"); }
    std::memcpy(fxd.sv, v, sizeof v); fxd.sv_ok = true; }
  static bool fxd_read(const Rc<DxvkImage> &img, std::vector<uint8_t> &out, uint32_t &w, uint32_t &h, bool &bgra) {   /* RGBA8 / BGRA8 image -> host (stall) */
    VkFormat fmt = img->info().format;
    bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB) return false;
    VkExtent3D ext = img->mipLevelExtent(0); VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * 4;
    DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "pp76 FXAA check readback";
    Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    emit([rb, fmt, img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ ext.width, ext.height, 1u }); });
    ctx_flush(); cs_sync(); g_dev->waitForIdle();
    const uint8_t *p = (const uint8_t *)rb->mapPtr(0); out.assign(p, p + sz); w = ext.width; h = ext.height; return true; }
  static void fxd_input() {   /* an FXAA draw (before it): formats, write masks, the input's alpha (= FXAA's luma) */
    fxd.checks++; fxd.post = false; fxd.in.clear();
    View *rv = g_views.get(g_cs.rtv[0]); Obj *so = t0_res ? obj(t0_res) : nullptr;
    const BS *bs = g_cs.bs ? g_bss.get(g_cs.bs) : nullptr; int fx_wm = bs ? (int)bs->m[0].writeMask() : 15;
    Rc<DxvkShader> psh = ps_id ? b1dx_shader_get(ps_id) : nullptr; std::string pn = psh != nullptr ? psh->debugName() : std::string("?");
    if (t0_iv == nullptr || !so || so->kind != 2) { std::fprintf(stderr, "[pp76] FXAA check %d: input t0 unknown\n", fxd.checks); return; }
    const DxvkImageViewKey &k = t0_iv->info();
    std::fprintf(stderr, "[pp76] FXAA check %d (present %ld): shader %s (%s) | input t0 res %u %ux%u fmt %d, view fmt %d swizzle 0x%x (0 = identity) | tone map target res %u fmt %d write mask 0x%x%s | FXAA target fmt %d write mask 0x%x\n",
                 fxd.checks, fxd.frames, pn.c_str(), pn.find("45fe1452") != std::string::npos ? "SM5" : pn.find("56e172d0") != std::string::npos ? "SM4" : "?",
                 t0_res, so->img->info().extent.width, so->img->info().extent.height, (int)so->img->info().format, (int)k.format, (unsigned)k.packedSwizzle,
                 fxd.uber_res, (int)fxd.uber_rt_fmt, fxd.uber_wmask, fxd.uber_seen && fxd.uber_res == t0_res ? "" : " (not the input: see res)",
                 rv && rv->type == 2 && obj(rv->res) ? (int)obj(rv->res)->img->info().format : -1, fx_wm);
    try {
      if (!fxd_read(so->img, fxd.in, fxd.in_w, fxd.in_h, fxd.in_bgra)) { std::fprintf(stderr, "[pp76] FXAA check %d: input format %d has no 8-bit alpha: FXAA (luma in alpha) cannot work as on PC\n", fxd.checks, (int)so->img->info().format); return; }
      const uint8_t *p = fxd.in.data(); long h[16] = { }, n = (long)fxd.in_w * fxd.in_h, a255 = 0, a0 = 0; bool bg = fxd.in_bgra;
      double sa = 0, sl2 = 0, saa = 0, sll = 0, sal = 0;
      for (long i = 0; i < n; i++) { const uint8_t *q = p + 4 * i; int r = bg ? q[2] : q[0], g = q[1], b = bg ? q[0] : q[2], a = q[3];
        h[a >> 4]++; a255 += a == 255; a0 += a == 0;
        double l = std::sqrt((0.299 * r + 0.587 * g + 0.114 * b) / 255.0) * 255.0;
        sa += a; sl2 += l; saa += (double)a * a; sll += l * l; sal += a * l; }
      double ca = saa - sa * sa / n, cl = sll - sl2 * sl2 / n, cov = sal - sa * sl2 / n, corr = ca > 0 && cl > 0 ? cov / std::sqrt(ca * cl) : 0;
      if (sl2 / n < 4.0 && fxd.checks <= 3) { fxd.checks--; fxd.next_check = fxd.frames + 60; fxd.in.clear();   /* a black frame (fade): try again later */
        static int nb; if (nb++ < 4) std::fprintf(stderr, "[pp76] FXAA check: the input is black (fade), check again in 60 presents\n"); return; }
      std::string hs; char b[16]; for (int i = 0; i < 16; i++) { std::snprintf(b, sizeof b, "%s%ld", i ? " " : "", h[i] * 1000 / (n ? n : 1)); hs += b; }
      std::fprintf(stderr, "[pp76] FXAA check %d: input alpha per mille in 16 bins [%s], alpha 255 %.1f%%, alpha 0 %.1f%%, mean sqrt-luma %.1f, corr(alpha, luma) %.3f -> %s\n", fxd.checks, hs.c_str(),
                   100.0 * a255 / n, 100.0 * a0 / n, sl2 / n, corr, corr > 0.9 ? "alpha holds luma as on PC" : "alpha does NOT hold luma: FXAA sees no edges");
      fxd.post = !b1aa_owns_fxaa() && rv && rv->type == 2 && obj(rv->res) != nullptr;   /* compare FXAA's output right after the draw (merge b76: not while aa76 owns the draw) */
    } catch (const DxvkError &e) { std::fprintf(stderr, "[pp76] FXAA check %d: readback failed: %s\n", fxd.checks, e.message().c_str()); } }
  static void fxd_post() {   /* right after the FXAA draw of a check: which share of each eye column did FXAA change */
    fxd.post = false;
    View *rv = g_views.get(g_cs.rtv[0]); Obj *ro = rv && rv->type == 2 ? obj(rv->res) : nullptr; if (!ro || ro->kind != 2 || fxd.in.empty()) return;
    std::vector<uint8_t> out; uint32_t w, h; bool bg;
    try { if (!fxd_read(ro->img, out, w, h, bg)) { std::fprintf(stderr, "[pp76] FXAA check %d: output format not RGBA8 / BGRA8\n", fxd.checks); return; } }
    catch (const DxvkError &e) { std::fprintf(stderr, "[pp76] FXAA check %d: output readback failed: %s\n", fxd.checks, e.message().c_str()); return; }
    if (w != fxd.in_w || h != fxd.in_h) { std::fprintf(stderr, "[pp76] FXAA check %d: output %ux%u vs input %ux%u\n", fxd.checks, w, h, fxd.in_w, fxd.in_h); fxd.in.clear(); return; }
    int ncol = on && eye_w > 0 ? 3 : 1; std::string res;
    for (int c = 0; c < ncol; c++) {
      uint32_t x0 = ncol == 1 ? 0 : (uint32_t)(c * eye_w), x1 = ncol == 1 ? w : c < 2 ? (uint32_t)((c + 1) * eye_w) : w; long n = 0, ch = 0, ch2 = 0;
      for (uint32_t y = 0; y < h; y++) for (uint32_t x = x0; x < x1; x++) { const uint8_t *a = &fxd.in[4 * ((size_t)y * w + x)], *b = &out[4 * ((size_t)y * w + x)];
        int ar = fxd.in_bgra ? a[2] : a[0], ab = fxd.in_bgra ? a[0] : a[2], br = bg ? b[2] : b[0], bb = bg ? b[0] : b[2];
        int d = std::max(std::abs(ar - br), std::max(std::abs((int)a[1] - (int)b[1]), std::abs(ab - bb))); n++; ch += d > 0; ch2 += d > 2; }
      char t[120]; std::snprintf(t, sizeof t, "%s%s %.2f%% (> 2 levels %.2f%%)", c ? ", " : "", ncol == 1 ? "picture" : c == 0 ? "left eye" : c == 1 ? "right eye" : "UI column",
                                 n ? 100.0 * ch / n : 0.0, n ? 100.0 * ch2 / n : 0.0); res += t; }
    std::fprintf(stderr, "[pp76] FXAA check %d: pixels changed by the FXAA draw: %s (cloud, Quest preset: 5-8%% of an eye)\n", fxd.checks, res.c_str());
    fxd.in.clear(); fxd.in.shrink_to_fit(); }
  static void fxd_pre() {   /* pre_draw */
    if (cur_pk == PK_UBER) { View *rv = g_views.get(g_cs.rtv[0]);
      if (rv && rv->type == 2) { const BS *bs = g_cs.bs ? g_bss.get(g_cs.bs) : nullptr; fxd.uber_seen = true; fxd.uber_res = rv->res;
        fxd.uber_rt_fmt = rv->iv != nullptr ? (uint32_t)rv->iv->info().format : 0; fxd.uber_wmask = bs ? bs->m[0].writeMask() : 15;
        if (is_bb_view(g_cs.rtv[0])) fxd.bb_now = 1; } }
    else if (cur_pk == PK_FXAA) { bool first = !fxd.fx_now; fxd.fx_now = 1;
      if (first && !fxd.next_check) fxd.next_check = fxd.frames + 120;   /* not the first FXAA frames (pipelines may still be compiling) */
      else if (first && fxd.checks < fxd_max() && fxd.frames >= fxd.next_check) { fxd_input(); fxd.next_check = fxd.checks == 1 ? fxd.frames + 3600 : fxd.frames + 7200; } } }
  static void fxd_present() {   /* every native present (render thread) */
    if (fxd_on < 0) { const char *e = std::getenv("BSI_B1_PPDIAG"); fxd_on = !(e && *e == '0'); }
    if (fxd_on <= 0) return;
    fxd_settings();
    int st = fxd.fx_now ? 1 : fxd.bb_now ? 2 : 0;
    fxd.fx_frames += fxd.fx_now; fxd.bb_frames += !fxd.fx_now && fxd.bb_now; fxd.frames++;
    if (st && st != fxd.state) { static int nl; if (nl++ < 12) std::fprintf(stderr, "[pp76] FXAA pass: %s from present %ld\n",
                                   st == 1 ? "drawn" : "NOT drawn (the tone map writes the back buffer directly)", fxd.frames); fxd.state = st; }
    if (fxd.frames % 600 == 0) { std::fprintf(stderr, "[pp76] FXAA pass: drawn in %ld of the last 600 presents, tone map straight into the back buffer in %ld (EnableFXAA %u)\n",
                                              fxd.fx_frames - fxd.last_fx, fxd.bb_frames - fxd.last_bb, guest32(0x13a4d14u)); fxd.last_fx = fxd.fx_frames; fxd.last_bb = fxd.bb_frames; }
    fxd.fx_now = fxd.bb_now = 0; fxd.uber_seen = false; fxd.post = false; }
}
