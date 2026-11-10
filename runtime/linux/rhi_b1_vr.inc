/* rhi_b1_vr.inc -- VR stereo pieces of the native renderer (BSI_RHI=b1n). Job vr69, notes/VR_STEREO.md §17.
   Included once by rhi_b1_dxvk.cpp (after the B1.5 state tables, before the context entry points). The game side of the
   stereo prototype (linux/vr_stereo.c, linux/vr_ui.c and the hooked part007/part012) is renderer-independent; this file
   adds what only the native renderer can do. Every vr_stereo.c function is a weak reference, so a link without the VR
   files (build_b1n_link.py, the harness's build_full.sh) keeps working and all of this stays off. It also stays off in a
   VR build until vr_stereo.c reports the stereo layout (vr_stereo.txt / BSI_VR_STEREO): with stereo off, the picture and
   the command stream are those of build 67 (one pointer test per blend state, render-target bind, viewport and present).

   1. UI coverage alpha (ui=1, ui_alpha=rhi). The UI column of the back buffer is cleared to (0,0,0,0) every frame by the
      game-level cover-tile hook (vr_ui.c), but UE3 blends UI with the destination alpha kept, so after the frame the
      column's alpha says nothing about where the UI is. While the back buffer is render target 0, every blending state
      (blend enabled, colour DestBlend != ZERO) gets the alpha op ONE / INV_SRC_ALPHA / ADD ("over" = coverage) and every
      state that writes colour also writes alpha. Colour blending is untouched; the eye columns go to an opaque layer, so
      the picture does not change; the column becomes a premultiplied UI image (colour = UI over black, alpha = coverage).
      Same rule as linux/vr_ui_rhi.c (bridge mode: a UE3 RHI vtable patch), but here at the D3D11 stream, where b1 sees
      every blend state and every render-target bind: no guest memory, no vtable chain, no extended imports.
   2. Present tag check. A stereo frame renders view 1 into the right eye column, so a full-size scene pass gets a viewport
      at x = eye width. At every Present the renderer reports whether it saw one; vr_stereo.c compares that with the tag
      the game thread gave that Present (the ring the XR side uses for the eye poses). It also reports whether the frame
      drew into the eye columns and/or the UI column: a Present the game did not tag (no UGameViewportClient::Draw, e.g.
      the loading screen's Scaleform movie) that drew only into the UI column is then shown as a UI-only frame (the UI
      rect as the screen) instead of the whole wide buffer.
   3. Route B (copy) presentation: FIFO (sync interval 1) into the AImageReader, so no frame is dropped, and every queued
      image is reported with its Present index (vr_stereo_image_presented): the XR thread knows exactly which game frame,
      hence which eye poses, each image is.
   4. GPU timestamps for the game (gpu_ts=eye, default with stereo): the game scales its detail by the GPU frame time it
      measures with TIMESTAMP queries; two eye views double that time, so the values it reads run at half speed (decided
      once, when the stereo layout first appears, so the timestamp sequence stays monotonic) and it keeps the mono detail.
      The renderer's own statistics stay unscaled.
   5. BSI_B1_DUMPALPHA=1 (diagnostics): back-buffer dumps also write <file>.alpha.pgm (the UI column's coverage).
   vr70 (notes/VR_STEREO.md §18):
   6. Frames in flight while the stereo layout is on: vr_stereo.txt fif= (default 1) instead of dxvk.conf's
      dxgi.maxFrameLatency (3 on the Quest); in flat mode vr_head.txt fif= (default 0 = unchanged). BSI_B1_LATENCY still wins.
      The wait is on the presenter's frame signal, which DXVK gives from its finish thread once all of the frame's command
      lists completed on the GPU (the Quest has no present-wait), so fif = GPU frames queued behind the one being built.
   7. Every queued present reports (presenter frame id, game Present index) to vr_stereo.c, and b1dx_frames_completed()
      returns the presenter's frame signal: route A shows the poses of the newest frame the GPU finished (= what the
      compositor can latch) instead of the newest Present the game called. Two stores per frame; no rendering change.
   8. AO crop (vr_stereo.txt ao_crop=, default 128 px, UI column layouts only): the game's AO compute dispatches are sized to
      the whole back buffer and run once per eye view, so a third of their work is the UI column. The two HDAO blurs (the
      shaders that have a V4 twin, rhi_b1_aoblur.inc; game shader or twin) are cut to the eye area (2 x eye width) plus the
      margin, in whole thread groups, when their UAV u0 has the back buffer's size and the frame is tagged as a world frame
      in the eye columns. Every group that still runs computes exactly what it computed before; the vertical blur reads
      the horizontal blur's output in its own column and the final output is read per pixel, so the eye pixels do not
      change. The AO main pass (HDAO / the AO switch's GTAO) is NOT cut: the horizontal blur reads its output across the
      left image edge with wrap-around, i.e. from the right end of the buffer (cloud shadow-mode check: cutting it changed
      the left eye's first 22 columns). (A first version took the eye area from the viewports drawn so far; a 1-pixel
      viewport before the AO cut a dispatch to 160 px: replaced by the frame's tag.)
   9. Layout changes after the first Present are logged (eye column / UI coverage alpha), not only on/off. */
extern "C" {
  int  vr_stereo_native_frame(int W, int H, int *eye_w, int *ui_alpha) __attribute__((weak));
  int  vr_stereo_native_ts_half(void) __attribute__((weak));
  void vr_stereo_rt_frame(uint32_t present, int saw_right, int drew_eye, int drew_ui) __attribute__((weak));
  int  vr_stereo_present_fifo(void) __attribute__((weak));
  void vr_stereo_image_presented(uint32_t present) __attribute__((weak));
  void vr_stereo_rt_check(uint32_t present, int saw_right) __attribute__((weak));
  extern volatile long g_br_presents;   /* bridge.c: game Presents (b1n: counted in br_native_present just before ours) */
  int  vr_stereo_frames_in_flight(void) __attribute__((weak));   /* vr70 */
  int  vr_stereo_ao_crop(void) __attribute__((weak));
  int  vr_stereo_crop_edge(int W, int H, uint32_t present) __attribute__((weak));
  void vr_stereo_frame_queued(unsigned long long fid, uint32_t present) __attribute__((weak));
  int  vr_head_frames_in_flight(void) __attribute__((weak));
  uint32_t vs_crop_groups(uint32_t x, uint32_t y, uint32_t uw, uint32_t uh, int bb_w, int bb_h, int drawn_x, int margin, uint32_t *gx) __attribute__((weak));
  int  vr_stereo_ao_eye(void) __attribute__((weak));   /* vr71 */
  int  vr_stereo_eye_frame(int W, int H, uint32_t present) __attribute__((weak));
  void vs_ao_eye_map(float tan_game, float W, int x0, int ew, float *inv, float *tan_x) __attribute__((weak));   /* ao74 */
}
namespace b1vr {
  bool on;                  /* stereo layout reported at the last Present (frame-latched) */
  bool alpha;               /* UI coverage alpha wanted (frame-latched) */
  bool bbrt;                /* the back buffer is render target 0 now */
  int  eye_w, bb_w, bb_h;   /* layout of the current back buffer */
  bool saw_right;           /* this frame: a back-buffer-size viewport at x = eye width */
  uint32_t last_bs, last_mask; float last_bf[4] = { 1, 1, 1, 1 }; bool have_bs;
  long n_cov, n_rebind, n_frames;
  double ts_k = 1.0; bool ts_decided;   /* timestamp speed for the game (4. above) */
  int drawn_x;              /* vr70: right edge of the eye area of the frame being rendered (AO crop) */
  int crop_m;               /* vr70: AO crop margin for this frame (0 = off) */
  Rc<DxvkImageView> uav0;   /* vr70: CS u0 while the layout is on */
  long n_crop, n_crop_skip; uint64_t crop_px_saved, crop_px_total;
  float last_vx = -1.f;     /* vr71: x of the last back-buffer-size viewport drawn this frame (diagnostics only) */
  int   sc_n = -1, sc_x[4], sc_next, sc_cur = -1;   /* vr71: views of the scene being rendered (b1dx_vr_scene_begin), next AO main pass's view */
  int   eyeb;               /* vr71: frame-latched: ao_eye on (skip the left eye view's AO passes) */
  uint32_t disp_x;          /* vr71: the game's group count in x of the current Dispatch (before the vr70 crop; logs) */
  long  n_eye, n_eye_skip; uint64_t eye_px, eye_px_total;
  static inline uint64_t ts(uint64_t t) { return ts_k == 1.0 ? t : (uint64_t)((double)t * ts_k); }

  /* blend state for RT0 while the back buffer is bound: alpha = coverage (see 1. above) */
  static void cover(DxvkBlendMode &m) {
    VkColorComponentFlags wm = m.writeMask();
    if (m.blendEnable() && m.colorDstFactor() != VK_BLEND_FACTOR_ZERO &&
        !(m.alphaSrcFactor() == VK_BLEND_FACTOR_ONE && m.alphaDstFactor() == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && m.alphaBlendOp() == VK_BLEND_OP_ADD)) {
      m.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD); n_cov++; }
    if (wm & (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)) wm |= VK_COLOR_COMPONENT_A_BIT;
    m.setWriteMask(wm); m.normalize();
  }
  /* is view id (an RTV) a view of the back buffer? */
  static bool is_bb_view(uint32_t rtv) { View *v = g_views.get(rtv); return v && v->type == 2 && g_bb && v->res == g_bb; }
  /* per frame: which columns of the stereo back buffer the draws hit (scene targets are back-buffer size; UE3 may round
     them up a little). rt_bb = the bound target has that size (set at every render-target bind while the layout is on). */
  bool rt_bb, drew_eye, drew_ui;
  static void rt_size() {
    View *v = g_views.get(g_cs.rtv[0]); if (!v || v->iv == nullptr) v = g_views.get(g_cs.dsv);
    rt_bb = false; if (!v || v->iv == nullptr || bb_w <= 0) return;
    VkExtent3D e = v->iv->mipLevelExtent(0);
    rt_bb = (int)e.width >= bb_w && (int)e.width <= bb_w + 32 && (int)e.height >= bb_h && (int)e.height <= bb_h + 32; }
  /* pre_draw (g_rmu held), stereo layout on: classify this draw by the first viewport's x */
  static void pass_diag();   /* ao74 (BSI_B1_VRPASS) */
  extern int pass_n; extern long pass_from, pass_to;
  static void pp_pre(); extern int lse, she, fxe, ble;   /* pp76 (end of this file) */
  static void draw() {
    if (pass_n) pass_diag();
    if (lse || she || fxe || ble) pp_pre();   /* pp76: light shafts / sun shadow mask / FXAA / bloom per eye */
    if (!rt_bb || !g_cs.nvp || eye_w <= 0) return;
    float x = g_cs.vp[0][0];
    if (x >= (float)eye_w - 1.5f && x <= (float)eye_w + 1.5f) saw_right = true;   /* view 1 = the right eye column */
    last_vx = x;   /* vr71: the eye the next compute pass belongs to (AO eye bounds) */
    if (x < (float)(2 * eye_w) - 1.5f) drew_eye = true; else drew_ui = true;
  }
  /* the last blend state handed over (b1dx_c_blend), so a render-target change can re-issue it with / without coverage */
  static void note_blend(uint32_t id, const float *f, uint32_t mask) { last_bs = id; std::memcpy(last_bf, f, 16); last_mask = mask; have_bs = true; }
  static void emit_blend() {   /* = b1dx_c_blend's command for (last_bs, last_bf, last_mask), with coverage if bbrt; g_rmu held */
    BS *bs = g_bss.get(last_bs);
    struct BlendCmd { DxvkMultisampleState ms; DxvkBlendMode m[8]; DxvkLogicOpState lo; DxvkBlendConstants bc; } bcmd;
    bcmd.ms = { }; bcmd.ms.setSampleMask((uint16_t)last_mask); bcmd.ms.setAlphaToCoverage(bs && bs->a2c);
    for (uint32_t i = 0; i < 8; i++) {
      DxvkBlendMode m = { };
      if (bs) m = bs->m[i];
      else { m.setBlendEnable(false); m.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
             m.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); m.setWriteMask(0xf); m.normalize(); }
      bcmd.m[i] = m;
    }
    if (alpha && bbrt) cover(bcmd.m[0]);
    DxvkLogicOpState lo = { }; lo.setLogicOp(false, VK_LOGIC_OP_NO_OP);
    bcmd.lo = bs ? bs->lo : lo; bcmd.bc = DxvkBlendConstants{ last_bf[0], last_bf[1], last_bf[2], last_bf[3] };
    emit([bcmd] (DxvkContext* c) {
      for (uint32_t i = 0; i < 8; i++) c->setBlendMode(i, bcmd.m[i]);
      c->setMultisampleState(bcmd.ms); c->setLogicOpState(bcmd.lo); c->setBlendConstants(bcmd.bc); });
    g_bc.bs = bs ? last_bs : 0; g_bc.bsmask = last_mask; std::memcpy(g_bc.bf, last_bf, 16);   /* the bind filter now mirrors it */
    n_rebind++;
  }
  /* b1dx_c_rts (g_rmu held): track "back buffer is RT0"; on a change re-issue the blend state so the coverage rule follows */
  static void rts(uint32_t rtv0) {
    if (on) rt_size();
    bool b = alpha && is_bb_view(rtv0);
    if (b == bbrt) return;
    bbrt = b;
    if (have_bs) emit_blend();
  }
  /* the frame-latched switch went off while the rule was applied: the next blend bind must be re-issued plainly */
  static void alpha_off() { if (bbrt) { bbrt = false; if (have_bs) emit_blend(); } }
  /* vr70: CS u0 (b1dx_c_uavs, layout on) */
  static void uav(uint32_t slot, View *v) { if (slot == 0) uav0 = (v && v->type == 4) ? v->iv : nullptr; }
  /* vr70: AO crop (8. above). x/y = the game's group counts; returns the group count in x to dispatch. */
  static void crop_skip(const char *why, uint32_t x, uint32_t y, uint32_t uw, uint32_t uh) {
    n_crop_skip++;
    static int nl; if (nl < 8) { nl++; std::fprintf(stderr, "[b1vr] AO crop: dispatch %ux%u left alone (%s; UAV u0 %ux%u, back buffer %dx%d, eye area drawn to x=%d)\n",
                                                    x, y, why, uw, uh, bb_w, bb_h, drawn_x); } }
  static uint32_t crop_x(uint32_t x, uint32_t y) {
    if (crop_m <= 0 || bb_w <= 0 || !vs_crop_groups || !vr_stereo_crop_edge) return x;
    if (uav0 == nullptr) { crop_skip("no UAV at u0", x, y, 0, 0); return x; }
    drawn_x = vr_stereo_crop_edge(bb_w, bb_h, (uint32_t)g_br_presents + 1);   /* the frame being rendered = the next Present */
    if (drawn_x <= 0) { VkExtent3D e0 = uav0->mipLevelExtent(0); crop_skip("frame not tagged as a world frame in the eye columns", x, y, e0.width, e0.height); return x; }
    static int force = -2; if (force == -2) { const char *fe = std::getenv("BSI_B1_VRCROP_FORCE"); force = fe ? std::atoi(fe) : -1; }   /* negative control (cloud) */
    VkExtent3D e = uav0->mipLevelExtent(0); uint32_t gx = 0;
    uint32_t nx = vs_crop_groups(x, y, e.width, e.height, bb_w, bb_h, force >= 0 ? force : drawn_x, force >= 0 ? 0 : crop_m, &gx);   /* vr_stereo.c (pure, host-tested) */
    if (!gx) { crop_skip("not a back-buffer-size UAV / not ceil(size / group size)", x, y, e.width, e.height); return x; }
    if (nx >= x) { crop_skip("the eye area + margin already reaches the UAV's right edge", x, y, e.width, e.height); return x; }
    n_crop++; crop_px_saved += (uint64_t)(x - nx) * gx * e.height; crop_px_total += (uint64_t)x * gx * e.height;
    if (n_crop == 1 || n_crop % 6000 == 0)
      std::fprintf(stderr, "[b1vr] AO crop: dispatch %ux%u (%u px groups) on a %ux%u UAV cut to %u groups (eye area drawn to x=%d + %d px); %ld cut, %ld left, %.0f%% of their pixels saved\n",
                   x, y, gx, e.width, e.height, nx, drawn_x, crop_m, n_crop, n_crop_skip, crop_px_total ? 100.0 * crop_px_saved / crop_px_total : 0.0);
    return nx;
  }
  /* vr70: frames in flight for this present (6. above); lat = dxvk.conf / default, env = BSI_B1_LATENCY given */
  static int fif(int lat, bool env) {
    static int last = -1; int v = lat; const char *why = "dxvk.conf / default";
    if (!env) {
      if (on && vr_stereo_frames_in_flight) { int f = vr_stereo_frames_in_flight(); if (f > 0) { v = f; why = "stereo, vr_stereo.txt fif="; } }
      else if (!on && vr_head_frames_in_flight) { int f = vr_head_frames_in_flight(); if (f > 0) { v = f; why = "flat with head-look, vr_head.txt fif="; } }
    } else why = "BSI_B1_LATENCY";
    if (v > 8) v = 8;
    if (v != last) { if (last >= 0 || v != lat) std::fprintf(stderr, "[b1vr] frames in flight: %d (%s)\n", v, why); last = v; }   /* silent only for the unchanged default */
    return v;
  }
  /* at every native Present (render thread, g_rmu held): report the frame, latch the state for the next one */
  static void diag_init();   /* ao74 */
  extern int eyem; extern int pass_n; extern long pass_from, pass_to; extern uint32_t ps_srv[8]; extern bool pass_armed;
  static void pd_frame_end();   /* pp76 diagnostics */
  static void fxd_present();    /* pp76 */
  static void pp_frame();       /* pp76 */
  extern bool pass_flat;
  static void present(uint32_t present_idx, uint32_t w, uint32_t h) {
    diag_init(); pd_frame_end(); fxd_present();   /* pp76: fxd_present = FXAA / settings diagnostics (flat and VR) */
    if (!vr_stereo_native_frame) return;
    { static const char *pf = std::getenv("BSI_B1_VRPASSFILE"); pass_armed = pf != nullptr;   /* ao74 diagnostics: the file appears -> log the next 2 stereo frames' draws */
      FILE *fp = (pf && (on || pass_flat) && n_frames % 16 == 0) ? std::fopen(pf, "r") : nullptr;   /* pp76: also flat (BSI_B1_VRPASSFLAT=1) */
      if (fp) { std::fclose(fp); std::remove(pf); pass_from = n_frames + 1; pass_to = n_frames + 3; pass_n = 1; } }
    if (on && vr_stereo_rt_frame) vr_stereo_rt_frame(present_idx, saw_right, drew_eye, drew_ui);
    else if (on && vr_stereo_rt_check) vr_stereo_rt_check(present_idx, saw_right ? 1 : 0);
    saw_right = drew_eye = drew_ui = false;
    int ew = 0, ua = 0; bool was = on; int was_ew = eye_w;
    on = vr_stereo_native_frame((int)w, (int)h, &ew, &ua) != 0;
    bool was_alpha = alpha;
    alpha = on && ua; eye_w = ew; bb_w = (int)w; bb_h = (int)h; n_frames++;
    pp_frame();   /* pp76 */
    drawn_x = 0; crop_m = on && vr_stereo_ao_crop ? vr_stereo_ao_crop() : 0;
    last_vx = -1.f; eyeb = (on && vr_stereo_ao_eye) ? vr_stereo_ao_eye() : 0;   /* vr71; ao74: the mode (1 vr71, 2 per eye) */
    eyem = eyeb;
    { static long last_e; if (on && n_frames % 600 == 0 && n_eye != last_e) { last_e = n_eye;
        std::fprintf(stderr, "[b1vr] AO eye so far: %ld AO dispatches of the left eye view skipped, %ld run for another reason\n", n_eye, n_eye_skip); } }
    { static long last_n; if (on && n_frames % 600 == 0 && n_crop != last_n) { last_n = n_crop;   /* vr70: AO crop summary */
        std::fprintf(stderr, "[b1vr] AO crop so far: %ld blur dispatches cut, %ld left alone, %.0f%% of the cut dispatches' pixels saved\n",
                     n_crop, n_crop_skip, crop_px_total ? 100.0 * crop_px_saved / crop_px_total : 0.0); } }
    if (!on) uav0 = nullptr;
    if (was_alpha && !alpha) alpha_off();
    if (on != was) std::fprintf(stderr, "[b1vr] stereo layout %s: back buffer %ux%u, eye column %d, UI coverage alpha %s\n",
                                on ? "on" : "off", w, h, ew, alpha ? "on (rhi_b1)" : "off");
    else if (on && (ew != was_ew || alpha != was_alpha))   /* vr70: e.g. the UI column appears once the game side used it */
      std::fprintf(stderr, "[b1vr] stereo layout change at Present #%u: eye column %d -> %d, UI coverage alpha %s -> %s\n",
                   present_idx, was_ew, ew, was_alpha ? "on" : "off", alpha ? "on" : "off");
    if (on && !ts_decided) { ts_decided = true;
      if (vr_stereo_native_ts_half && vr_stereo_native_ts_half()) ts_k = 0.5;
      std::fprintf(stderr, "[b1vr] GPU timestamps for the game: %s\n", ts_k != 1.0 ? "half speed (gpu_ts=eye: its detail scaling sees a per-eye frame time)"
                                                                                       : "real speed (gpu_ts=frame or one eye per frame)"); }
  }
}

/* ---- vr71: AO passes of the first eye view skipped (vr_stereo.txt ao_eye=1, opt-in; README of job vr71, notes/VR_STEREO.md §19) ----
   In a stereo frame UE3 runs the AO passes (main + horizontal + vertical blur, all into the same back-buffer-size targets)
   once per eye view, and every dispatch covers the whole double-wide buffer. The cloud's shadow-mode comparisons (job
   vr71) showed: (e1) a per-eye split changed the left eye wherever the right eye's passes no longer wrote the full chain,
   so the left eye is shaded from the AO targets as the RIGHT eye view's passes leave them; (e2) not dispatching the left
   eye view's passes leaves every frame identical; (e3) neither does skipping the right eye view's passes instead: both
   views' passes compute the same whole-buffer result. So one AO chain per stereo frame is enough and the left eye
   view's chain is dead work (everything it writes is rewritten before anything reads it). With ao_eye=1 the AO
   passes of every view but the last of a scene (views in render order, from the scene command hook: vr_ui_rhi.c ->
   b1dx_vr_scene_begin) are not dispatched; the last view's passes run whole. Only for scenes whose views are the two eye
   columns (x 0 and eye width) of a stereo frame; anything else runs as before.
   Cloud checks: BSI_B1_VREYE_SKIPLAST=1 skips the LAST view's passes instead (the cloud showed the same picture: both
   views' AO passes compute identical results), =2 skips all of them (negative control: the picture must change),
   BSI_B1_VRDISP=N logs the first N dispatches of stereo frames. */
namespace b1vr {
  static int env_i(const char *n, int d) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : d; }
  /* which AO pass a game shader is: 1 = the HDAO main pass (DXVK key cs.3ed0bfa7..., the shader the AO switch replaces),
     0 = one of the two HDAO blurs (rhi_b1_aoblur.inc allowlist), -1 = anything else (never touched) */
  static int ao_kind(const Rc<DxvkShader> &s, bool blur) {
    if (blur) return 0; if (s == nullptr) return -1;
    static std::unordered_map<DxvkShader*, int> k; auto it = k.find(s.ptr()); if (it != k.end()) return it->second;
    int v = s->debugName().find("3ed0bfa754047750e778d92529a0d856") != std::string::npos ? 1 : -1; k[s.ptr()] = v; return v; }
  /* b1dx_c_dispatch / ab::dispatch (g_rmu held), for an AO pass (main = main pass): true = do not dispatch it (dead work) */
  static bool eye_emit(uint32_t x, uint32_t y, int main, const char *what) {
    if (!eyeb || !on || !vr_stereo_eye_frame) return false;
    int view;
    if (main) { if (sc_n > 0 && sc_next < sc_n) view = sc_cur = sc_next++; else { sc_cur = -1; view = -1; } } else view = sc_cur;
    int ew = vr_stereo_eye_frame(bb_w, bb_h, (uint32_t)g_br_presents + 1);
    static int nskip; auto whole = [&](const char *why) { n_eye_skip++; if (nskip < 8) { nskip++;
      std::fprintf(stderr, "[b1vr] AO eye: %s dispatch %ux%u runs (%s; scene views %d, view %d)\n", what, x, y, why, sc_n, view); } return false; };
    if (sc_n < 0) return whole("no scene command seen (hook missing)");
    if (sc_n != 2 || ew <= 0) return false;   /* mono / one-view scenes and non-stereo frames: as before, silently */
    if (view < 0) return whole("more AO passes than views in this scene");
    if (sc_x[0] != 0 || sc_x[1] != ew) return whole("the scene's views are not the two eye columns");
    if (uav0 == nullptr) return whole("no UAV at u0");
    VkExtent3D e = uav0->mipLevelExtent(0);
    if ((int)e.width < bb_w || (int)e.width > bb_w + 32 || (int)e.height < bb_h || (int)e.height > bb_h + 32) return whole("not a back-buffer-size UAV");
    static int last = -2; if (last == -2) { last = env_i("BSI_B1_VREYE_SKIPLAST", 0);
      if (last) std::fprintf(stderr, "[b1vr] AO eye: BSI_B1_VREYE_SKIPLAST=%d, %s AO passes are skipped (cloud control)\n", last, last == 2 ? "ALL views'" : "the LAST view's"); }
    bool skip = last == 2 ? true : last ? view == sc_n - 1 : view < sc_n - 1;
    if (!skip) return false;
    n_eye++; eye_px += x; eye_px_total += x;
    if (n_eye <= 3 || n_eye % 6000 == 0)
      std::fprintf(stderr, "[b1vr] AO eye: %s dispatch %ux%u of view %d (x %d) of %d not dispatched (overwritten by the right eye's passes before use); %ld skipped\n",
                   what, x, y, view, sc_x[view], sc_n, n_eye);
    return true;
  }
  /* BSI_B1_VRDISP=N (cloud diagnostics): the first N dispatches of stereo frames */
  static void disp_log(uint32_t x, uint32_t y, uint32_t z, uint32_t cs, bool blur, bool alt) {
    static long n = -2; if (n == -2) n = env_i("BSI_B1_VRDISP", 0); if (n <= 0 || !on) return;
    n--; VkExtent3D e = uav0 != nullptr ? uav0->mipLevelExtent(0) : VkExtent3D{ 0, 0, 0 };
    std::fprintf(stderr, "[b1vr] dispatch: frame %ld present #%ld cs %u %ux%ux%u blur %d alt %d UAV %ux%u last viewport x %.1f eye frame %d scene views %d next %d\n",
                 n_frames, (long)g_br_presents + 1, cs, x, y, z, blur, alt, e.width, e.height, last_vx, vr_stereo_eye_frame ? vr_stereo_eye_frame(bb_w, bb_h, (uint32_t)g_br_presents + 1) : -1, sc_n, sc_next);
  }
}

/* ---- ao74: AO per eye (vr_stereo.txt ao_eye=2, default; README of job ao74, notes/VR_STEREO.md §22) ----
   The game's AO main pass (HDAO, or the AO switch's GTAO) rebuilds view-space positions from the pixel position with its
   constant buffer cb0: ndc.x = 2 (p + .5) c4.z - 1, x_view = ndc.x c5.x z, with c4 = (render-target size, 1 / size) and
   c5.x = tan(half fov x). The game takes c5 from the TARGET, not from the view: c5.y = tan 22.5 deg, c5.x = tan(22.5 deg *
   W / H) (cloud logs: 768x272 -> 2.008271, 256x272 -> 0.387402). In stereo every view's pass covers the whole [L|R|UI]
   buffer with c4 = the whole buffer and c5.x from its 2.8:1 aspect, so each eye's AO used a frustum built for the whole
   buffer: x positions about 1.7x too wide per pixel and sheared (the left eye's centre sat at x/z = -1.34 instead of 0),
   i.e. the AO saw every depth edge at the wrong angle: extra / missing occlusion along vertical edges (cloud: 53 % of the
   left eye's AO texels differ from the eye rendered alone, up to 49/255). What a mono view of one eye column (width ew)
   gets is c4.z = 1 / ew, c5.x = tan(atan(c5.x) * ew / W); for the column at x0 the same mapping is c4.z' = 1 / (2 x0 + ew),
   c5.x' = t_eye (2 x0 + ew) / ew (vs_ao_eye_map, vr_stereo.c, host-tested; the left eye's values are then bit-identical
   to the mono view's). The views are known from the scene command hook (sc_x: x 0 and eye width).
   ao_eye=2: the first (left) view's main pass is not dispatched where it is issued; at the last (right) view's main pass
   rhi_b1 dispatches the right view's pass over the whole buffer with the right column's constants, then the left view's
   pass (its own constants, saved at its own Dispatch, remapped) over the left eye column only (whole thread groups from x 0,
   so groups and per-pixel results are those of a full dispatch), then rebinds the game's constant buffer. The blurs run
   once (the last view's, as with ao_eye=1). Nothing reads the AO targets between the two views' passes and both passes read
   the same depth (vr71 e2/e3). Cost: one main pass over the buffer + one over an eye column (builds 71-74: one pass).
   The game HDAO and its exact replacements (AO switch 0..2) also use c4.zw for their Gather coordinates: there only c5.x
   can change for the right eye (c4.z stays when the buffer is 3 eye widths) and the left eye keeps the old constants
   (logged). GTAO (AO switch 3/4, the Quest default) uses texelFetch: both eyes get their mono constants.
   Diagnostics (cloud): BSI_B1_VRAOCB=N logs the first N AO main passes' cb0 (also flat), BSI_B1_VRAODUMP=N writes the AO
   result (the second blur's output, red channel) every N frames to $BSI_B1_DUMPDIR/ao_f<frame>_<k>.pgm,
   BSI_B1_VRPASS=<frame>,<n> / BSI_B1_VRPASSFILE=<file> log every draw of stereo frames (pixel shader, viewport, target). */
namespace b1vr {
  Rc<DxvkImageView> uav0_any;    /* diagnostics: CS u0 also in flat frames (BSI_B1_VRAODUMP) */
  int eyem;                      /* frame-latched ao_eye mode: 0 off (build 70), 1 vr71 (left view's AO skipped), 2 per eye */
  uint32_t cs_cb0;               /* the compute stage's constant buffer 0 (b1dx_c_cbs) */
  struct CbSh { std::vector<float> f; uint32_t n = 0; };   /* CPU copy of a constant buffer's last contents (n = valid bytes from 0) */
  std::unordered_map<uint32_t, CbSh> cbsh;
  int aocb_n = -2, aodump_n = -2;
  extern bool pass_flat;
  static void diag_init() { if (aocb_n == -2) { aocb_n = env_i("BSI_B1_VRAOCB", 0); aodump_n = env_i("BSI_B1_VRAODUMP", 0); pass_flat = env_i("BSI_B1_VRPASSFLAT", 0) > 0; } }
  static bool cb_track() { diag_init(); return on || aocb_n > 0 || aodump_n > 0; }
  /* b1dx_c_update / b1dx_c_unmap / b1dx_c_bufwrite (g_rmu held): the buffers bound as CS cb0 (the AO constants; one hash lookup
     per buffer write otherwise), stereo (or diagnostics) only. The first write before the first bind is missed: that scene's
     AO then runs as issued (logged) and the next frame has the contents. */
  std::unordered_map<uint32_t, char> cb_want;   /* buffers ever bound as CS cb0 (or PS cb0 for the pass diagnostics) */
  static void cb_bound(uint32_t id) { if (id && cb_track() && !cb_want.count(id)) cb_want[id] = 1; }
  extern uint32_t ps0_gate, vs0_gate; static bool cb_gate(uint32_t id);   /* pp76 (below) */
  static void cb_wrote(uint32_t id, Obj *o, uint32_t off, uint32_t size, const void *data) {
    if (!o || o->kind != 1 || !(o->dxgi & 0x4u) || cb_want.empty() || !cb_want.count(id) || !cb_track()) return;
    if ((ps0_gate || vs0_gate) && !cb_gate(id)) return;   /* pp76: $Globals copies only around the sun's shadow projection */
    uint32_t bsz = (uint32_t)o->buf->info().size; if (bsz > 65536 || off >= bsz) return;   /* UE3 binds 4 KB global buffers */
    if (off + size > bsz) size = bsz - off;
    CbSh &c = cbsh[id]; if (c.f.size() * 4 < bsz) c.f.resize((bsz + 3) / 4);
    if (off > c.n) return;   /* a gap: unknown bytes before it */
    std::memcpy((uint8_t *)c.f.data() + off, data, size); if (off + size > c.n) c.n = off + size; }
  static void cb_forget(uint32_t id) { if (!cbsh.empty()) cbsh.erase(id); if (!cb_want.empty()) cb_want.erase(id); }
  static const float *cb_get(uint32_t id, uint32_t need = 96) { auto it = cbsh.find(id); return it == cbsh.end() || it->second.n < need ? nullptr : it->second.f.data(); }
  /* the AO main pass constants for the eye column at x0, width ew (see above): cb0 floats 18 = c4.z, 20 = c5.x */
  static void ao_patch(float *f, int x0, int ew) { vs_ao_eye_map(f[20], f[16], x0, ew, &f[18], &f[20]); }   /* vr_stereo.c (pure, host-tested) */
  struct AoL { bool have; float cb[24]; Rc<DxvkImageView> uav; } aol;
  Rc<DxvkBuffer> aobuf; long n_e2, n_e2_fb, n_e2_left_old; bool aobuf_failed;
  static void cb_log(const char *what, const float *f) {
    std::fprintf(stderr, "[b1vr] AO cb0 %s: c0 %.6g %.6g %.6g %.6g | rot %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f | c4 size %.1f %.1f inv %.9g %.9g | c5 tan %.9g %.9g %.6g %.6g\n",
                 what, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[8], f[9], f[10], f[12], f[13], f[14], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]); }
  /* ao_eye=2, at an AO main pass (b1dx_c_dispatch, g_rmu held): true = handled (dispatched here, or deferred to the last view) */
  static bool eye2(uint32_t x, uint32_t y, uint32_t z, const Rc<DxvkShader> &alt, const Rc<DxvkShader> &norm, const uint32_t *sc, bool gtao) {
    if (!eyeb || !on || !vr_stereo_eye_frame || !vs_ao_eye_map) return false;
    int view; if (sc_n > 0 && sc_next < sc_n) view = sc_cur = sc_next++; else { sc_cur = -1; view = -1; }
    int ew = vr_stereo_eye_frame(bb_w, bb_h, (uint32_t)g_br_presents + 1);
    static int nfb; auto fb = [&](const char *why) { n_e2_fb++; aol.have = false; aol.uav = nullptr; if (nfb < 12) { nfb++;
      std::fprintf(stderr, "[b1vr] AO per eye: main dispatch %ux%u of view %d runs as issued (%s; scene views %d)\n", x, y, view, why, sc_n); } return false; };
    if (sc_n < 0) return fb("no scene command seen (hook missing)");
    if (sc_n != 2 || ew <= 0) return false;   /* mono / one-view scenes and non-stereo frames: as before, silently */
    if (view < 0) return fb("more AO passes than views in this scene");
    if (sc_x[0] != 0 || sc_x[1] != ew) return fb("the scene's views are not the two eye columns");
    if (uav0 == nullptr) return fb("no UAV at u0");
    VkExtent3D e = uav0->mipLevelExtent(0);
    if ((int)e.width < bb_w || (int)e.width > bb_w + 32 || (int)e.height < bb_h || (int)e.height > bb_h + 32) return fb("not a back-buffer-size UAV");
    const float *cb = cb_get(cs_cb0);
    if (view == 0) {   /* left view: keep its constants, dispatch it with the right view's pass */
      if (!cb) return fb("left view: constant buffer contents unknown");
      aol.have = true; std::memcpy(aol.cb, cb, sizeof aol.cb); aol.uav = uav0; n_eye++; return true; }
    if (!aol.have) { n_e2_fb++; return false; }   /* (logged at the left view) the right view's pass whole = ao_eye 1 */
    if (!cb) return fb("right view: constant buffer contents unknown");
    if (aol.uav != uav0) return fb("the two views' passes write different targets");
    if (!sc[0] || 32u % sc[0]) return fb("unexpected dispatch scale");
    if (aobuf == nullptr && !aobuf_failed) {
      DxvkBufferCreateInfo bi = { }; bi.size = 512; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      bi.debugName = "B1 AO per-eye constants";
      try { aobuf = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); } catch (const DxvkError &) { aobuf_failed = true; } }
    if (aobuf == nullptr) return fb("no buffer for the per-eye constants");
    struct C { float f[24]; } pr, pl; std::memcpy(pr.f, cb, sizeof pr.f); std::memcpy(pl.f, aol.cb, sizeof pl.f);
    aol.have = false; aol.uav = nullptr;
    ao_patch(pr.f, ew, ew);
    bool right_ok = gtao || std::fabs(pr.f[18] - cb[18]) <= 1e-6f * std::fabs(cb[18]);   /* Gather shaders: c4.z must not change */
    if (!right_ok) std::memcpy(pr.f, cb, sizeof pr.f);
    if (gtao) ao_patch(pl.f, 0, ew); else n_e2_left_old++;
    uint32_t gpx = 32u / sc[0], nxl = (uint32_t)ew / gpx;
    uint32_t xs = x * sc[0], ys = y * sc[1], zs = z * sc[2];
    if (nxl > xs) nxl = xs;
    Rc<DxvkBuffer> gb; { Obj *o = obj(cs_cb0); if (o && o->kind == 1) gb = o->buf; }
    n_e2++;
    static int nl; if (nl < 2 || n_e2 % 6000 == 0) { nl++;
      std::fprintf(stderr, "[b1vr] AO per eye: %s main pass: right view %ux%u groups (c4.z %.9g -> %.9g, c5.x %.9g -> %.9g%s), left view %u of %u groups = x 0..%u (c4.z %.9g, c5.x %.9g%s)%s; %ld scenes, %ld as issued\n",
                   alt != nullptr ? (gtao ? "GTAO" : "AO switch") : "HDAO", xs, ys, cb[18], pr.f[18], cb[20], pr.f[20], right_ok ? "" : ": NOT remapped, Gather shader and the buffer is not 3 eyes wide",
                   nxl, xs, nxl * gpx, pl.f[18], pl.f[20], gtao ? "" : ": the left eye keeps the whole-buffer mapping (Gather shader)",
                   (uint32_t)ew % gpx ? " (eye width not a multiple of the group width: the last columns keep the right view's mapping)" : "", n_e2, n_e2_fb); }
    if (aocb_n > 0) { aocb_n--; cb_log("right view (game)", cb); cb_log("right view, sent", pr.f); cb_log("left view, sent", pl.f); }
    emit([b = aobuf, gb = std::move(gb), pr, pl, a = alt, nm = norm, xs, ys, zs, nxl] (DxvkContext* c) mutable {
      const uint32_t slot = 5u * 16u + 0u;
      if (a != nullptr) c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(a));
      c->updateBuffer(b, 0, sizeof pr, &pr); c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, slot, DxvkBufferSlice(b, 0, 256));
      c->dispatch(xs, ys, zs);
      if (nxl) { c->updateBuffer(b, 256, sizeof pl, &pl); c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, slot, DxvkBufferSlice(b, 256, 256)); c->dispatch(nxl, ys, zs); }
      c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, slot, gb != nullptr ? DxvkBufferSlice(gb) : DxvkBufferSlice());
      if (nm != nullptr) c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(nm)); });
    return true;
  }
  /* diagnostics (BSI_B1_VRAOCB / BSI_B1_VRAODUMP): called for every Dispatch after it was emitted (or not) */
  long ao_blurs_since_main = -1, ao_chain;
  static void ao_diag(bool emitted, uint32_t x, uint32_t y, bool blur) {
    diag_init(); if (aocb_n <= 0 && aodump_n <= 0) return;
    int k = blur ? 0 : ao_kind(b1dx_shader_get(g_cur_cs), false);
    if (k == 1) { ao_blurs_since_main = 0; ao_chain++;
      if (aocb_n > 0 && !(on && eyem == 2)) { const float *f = cb_get(cs_cb0); if (f) { aocb_n--; char w[96]; std::snprintf(w, sizeof w, "%s main %ux%u%s", on ? "stereo" : "flat", x, y, emitted ? "" : " (not dispatched)"); cb_log(w, f); } } }
    if (k != 0 || !blur || ao_blurs_since_main < 0) return;
    if (++ao_blurs_since_main != 2 || !emitted || aodump_n <= 0 || uav0_any == nullptr || (g_frames % aodump_n) != 0) return;
    try {
      Rc<DxvkImage> img = uav0_any->image(); VkFormat fmt = img->info().format; VkExtent3D ext = img->mipLevelExtent(0);
      if (lookupFormatInfo(fmt)->elementSize != 4) return;
      DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)ext.width * ext.height * 4u; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 AO readback";
      Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      emit([rb, fmt, img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
      ctx_flush(); cs_sync(); g_dev->waitForIdle();
      const char *dir = std::getenv("BSI_B1_DUMPDIR"); char path[512];
      std::snprintf(path, sizeof path, "%s/ao_f%ld_%ld.pgm", dir ? dir : "/tmp", g_frames, ao_chain);
      FILE *f = std::fopen(path, "wb"); if (!f) return;
      std::fprintf(f, "P5\n%u %u\n255\n", ext.width, ext.height); const uint8_t *p = (const uint8_t *)rb->mapPtr(0);
      for (VkDeviceSize i = 0; i < (VkDeviceSize)ext.width * ext.height; i++) std::fputc(p[4 * i], f);
      std::fclose(f);
      std::fprintf(stderr, "[b1vr] AO dump %s (%ux%u fmt %d, %s, ao_eye %d)\n", path, ext.width, ext.height, (int)fmt, on ? "stereo" : "flat", on ? eyem : -1);
    } catch (const DxvkError &e) { std::fprintf(stderr, "[b1vr] AO dump failed: %s\n", e.message().c_str()); }
  }
}
void b1vr_cb_forget(uint32_t id) { b1vr::cb_forget(id); }   /* ao74: b1dx_release */
/* ao74 diagnostics, BSI_B1_VRPASS=<first stereo frame>,<frames>: every draw of those stereo frames (pixel shader, viewport,
   render target) and, for the game's FXAA (FXEdgeDetectAntiAlias PS, SM4 56e172d0 / SM5 45fe1452), its constants */
namespace b1vr {
  uint32_t ps_id, ps_cb0, ps_srv[8]; int pass_n = -1; bool pass_armed; long pass_from, pass_to, pass_draw, pass_frame = -1;
  static void pd_draw(const std::string &name);   /* pp76 diagnostics (below) */
  static void pd_load(); extern int pd_init; extern uint32_t vs_id;
  extern bool pass_flat;
  static void pass_diag() {
    if (pd_init < 0) pd_load();   /* pp76 */
    if (pass_n < 0) { const char *e = std::getenv("BSI_B1_VRPASS"); long a = 0, b = 0;
      if (e && std::sscanf(e, "%ld,%ld", &a, &b) == 2 && b > 0) { pass_from = a; pass_to = a + b; pass_n = 1; } else { pass_n = 0; return; } }
    if ((!on && !pass_flat) || n_frames < pass_from) return;
    if (n_frames >= pass_to) { pass_n = 0; return; }
    if (pass_frame != n_frames) { pass_frame = n_frames; pass_draw = 0; }
    pass_draw++;
    static std::unordered_map<uint32_t, std::string> nm; std::string &name = nm[ps_id];
    if (name.empty()) { Rc<DxvkShader> sh = ps_id ? b1dx_shader_get(ps_id) : nullptr; name = sh != nullptr ? sh->debugName() : std::string("none"); }
    View *v = g_views.get(g_cs.rtv[0]); uint32_t rw = 0, rh = 0; bool bb = is_bb_view(g_cs.rtv[0]);
    if (v && v->iv != nullptr) { VkExtent3D e = v->iv->mipLevelExtent(0); rw = e.width; rh = e.height; }
    float vx = g_cs.nvp ? g_cs.vp[0][0] : -1, vy = g_cs.nvp ? g_cs.vp[0][1] : -1, vw = g_cs.nvp ? g_cs.vp[0][2] : -1, vh = g_cs.nvp ? g_cs.vp[0][3] : -1;
    char srv[320] = ""; int k = 0;
    for (int i = 0; i < 8; i++) { View *t = g_views.get(ps_srv[i]); if (!t || t->type != 1 || t->iv == nullptr) continue; VkExtent3D e = t->iv->mipLevelExtent(0);
      k += std::snprintf(srv + k, sizeof srv - k, " t%d=%u:%ux%u", i, t->res, e.width, e.height); if (k >= (int)sizeof srv - 1) break; }
    std::string vsn; if (pd_init > 0) { Rc<DxvkShader> vsh = vs_id ? b1dx_shader_get(vs_id) : nullptr; vsn = vsh != nullptr ? " vs " + vsh->debugName().substr(0, 11) : std::string(" vs none"); }   /* pp76 */
    std::fprintf(stderr, "[b1vr] pass: frame %ld draw %ld ps %.40s vp %.1f %.1f %.1f %.1f rt %ux%u%s res %u%s%s\n", n_frames, pass_draw, name.c_str(), vx, vy, vw, vh, rw, rh, bb ? " (back buffer)" : "", v ? v->res : 0u, srv, vsn.c_str());
    pd_draw(name);   /* pp76 */
    if (name.find("56e172d0") != std::string::npos || name.find("45fe1452") != std::string::npos) {
      const float *f = cb_get(ps_cb0, 80);
      if (f) std::fprintf(stderr, "[b1vr] pass: FXAA cb0 %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g\n",
                          f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]);
      else std::fprintf(stderr, "[b1vr] pass: FXAA cb0 contents unknown (id %u)\n", ps_cb0);
    }
  }
}

#include "rhi_b1_pp76.inc"   /* pp76: post-process per eye + pass diagnostics */
