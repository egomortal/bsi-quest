/* ---- tl126 (build 126, chat 4 Phase 0b): measurement tooling, every feature OFF by default (default behaviour = build 125) ----
   1. BSI_B1_ABAB=<switch>:<N> (warm-up BSI_B1_ABAB_WARM frames, default 600; first BSI_B1_ABAB_SKIP frames of each block not counted,
      default 8): after the warm-up the named switch is flipped in blocks of N presents in the order A B B A A B B A ... (A = as the env
      says, B = flipped). Supported switches (runtime-safe): BSI_B1_GPUPROF (B = profiler off when it is on), BSI_B1_SLACK (B = the
      injection below off). Per block: [b1abab] <switch> blk <k> <A|B> frames <n> frame_ms <mean present-to-present> gts_ms <mean
      GPU span the game measured> . Analysis: tools\bsi\abab.py <log> (paired A/B block differences, bootstrap 95 % interval).
   2. BSI_B1_SLACK=<point>:<n>[,<point>:<n>...] (b126): n full clears of a 1024x1024 RGBA16F scratch image (8 MB writes each) at a
      point of every frame: start (after present), present (before the present blit), rtN (the N-th render-target set of the frame,
      1-based), csN (the N-th dispatch of the frame). How much of the injected GPU time reaches the frame time = the slack at that
      point (r09 devil's advocate "slack map"). BSI_B1_SLACK_TS=1: timestamps around each injection, [b1slack] reports the injected GPU
      ms (calibration run only: the timestamps are drains on Turnip). [b1slack] also lists the render-target sets of one frame once.
   3. BSI_B1_GPUTS_K=<factor> (b126): overrides the speed of the GPU timestamps the game reads (stereo default 0.5, gpu_ts=eye). The
      game scales its detail by the GPU time it measures; a probe with e.g. 0.25 / 1.0 shows whether the scaler changes the frame.
      [b1perf] already prints the game-measured GPU span; [b1gts] adds it per 600 presents with the factor.
   4. BSI_B1_VISHASH=1 (b126): [b1vis] every 600 presents: hash of the per-frame draw stream (draw count + bound shaders + index counts)
      and of the occlusion-query answers the game read, to see whether two builds / switches change what the engine draws or
      sees (visibility drift: r12 risk review). */
namespace tl126 {
  static int env_i(const char *n, int d) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : d; }
  /* ---- 1. ABAB ---- */
  struct Ab { bool init = false, on = false; std::string name; int n = 72, warm = 600, skip = 8; bool b = false; long fr = 0, blk = 0, inblk = 0;
              uint64_t tlast = 0; double sum = 0, gsum = 0; long cnt = 0, gcnt = 0; };
  static Ab ab;
  static void ab_init() {
    if (ab.init) return; ab.init = true; const char *e = std::getenv("BSI_B1_ABAB"); if (!e || !*e) return;
    std::string s = e; size_t c = s.find(':'); ab.name = s.substr(0, c); if (c != std::string::npos) ab.n = std::atoi(s.c_str() + c + 1);
    if (ab.n < 8) ab.n = 8; ab.warm = env_i("BSI_B1_ABAB_WARM", 600); ab.skip = env_i("BSI_B1_ABAB_SKIP", 8); if (ab.skip >= ab.n) ab.skip = ab.n / 4;
    ab.on = ab.name == "BSI_B1_GPUPROF" || ab.name == "BSI_B1_SLACK" || ab.name == "BSI_B1_SUNNL";   /* b129: SUNNL B = no skip */
    std::fprintf(stderr, "[b1abab] %s: switch %s, blocks of %d presents (first %d not counted), warm-up %d, order A B B A\n",
                 ab.on ? "on" : "NOT SUPPORTED (off)", ab.name.c_str(), ab.n, ab.skip, ab.warm); }
  static inline bool flipped(const char *name) { if (!ab.init) ab_init(); return ab.on && ab.b && ab.name == name; }
  /* at the end of present (after the profiler's frame start): frame interval, block bookkeeping, next phase */
  static void ab_present(double gts_ms) {
    if (!ab.init) ab_init(); if (!ab.on) return;
    uint64_t t = now_us(); double dt = ab.tlast ? (double)(t - ab.tlast) / 1000.0 : 0.0; ab.tlast = t; ab.fr++;
    if (ab.fr <= ab.warm) { if (ab.fr == ab.warm) { ab.blk = 0; ab.inblk = 0; ab.b = false; } return; }
    if (ab.inblk >= ab.skip && dt > 0) { ab.sum += dt; ab.cnt++; } if (ab.inblk >= ab.skip && gts_ms > 0) { ab.gsum += gts_ms; ab.gcnt++; }
    if (++ab.inblk >= ab.n) {
      std::fprintf(stderr, "[b1abab] %s blk %ld %c frames %ld frame_ms %.3f gts_ms %.3f\n", ab.name.c_str(), ab.blk, ab.b ? 'B' : 'A', ab.cnt,
                   ab.cnt ? ab.sum / ab.cnt : 0.0, ab.gcnt ? ab.gsum / ab.gcnt : 0.0);
      ab.blk++; ab.inblk = 0; ab.sum = ab.gsum = 0; ab.cnt = ab.gcnt = 0; ab.b = ((ab.blk + 1) / 2) % 2 == 1; } }
  /* ---- 2. slack injection ---- */
  struct Pt { int kind = 0; int idx = 0; int n = 0; };   /* kind: 1 start, 2 present, 3 rt, 4 cs */
  static std::vector<Pt> pts; static int sl_init = -1, sl_ts; static int rt_n, cs_n; static long sl_frames, sl_inj;
  static Rc<DxvkImage> sl_img; static Rc<DxvkImageView> sl_view;
  static Rc<DxvkQuery> sl_q[8][2]; static bool sl_live[8]; static int sl_cur; static double sl_ms; static long sl_ms_n;
  static std::string rt_seen; static int rt_logged;
  static bool sl_on() {
    if (sl_init >= 0) return sl_init > 0; sl_init = 0; const char *e = std::getenv("BSI_B1_SLACK"); if (!e || !*e) return false;
    std::string s = e; size_t p = 0;
    while (p < s.size()) { size_t q = s.find(',', p); std::string t = s.substr(p, q == std::string::npos ? std::string::npos : q - p); p = q == std::string::npos ? s.size() : q + 1;
      size_t c = t.find(':'); Pt pt; pt.n = c != std::string::npos ? std::atoi(t.c_str() + c + 1) : 1; std::string k = t.substr(0, c);
      if (k == "start") pt.kind = 1; else if (k == "present") pt.kind = 2;
      else if (k.rfind("rt", 0) == 0) { pt.kind = 3; pt.idx = std::atoi(k.c_str() + 2); } else if (k.rfind("cs", 0) == 0) { pt.kind = 4; pt.idx = std::atoi(k.c_str() + 2); }
      if (pt.kind && pt.n > 0) pts.push_back(pt); }
    sl_ts = env_i("BSI_B1_SLACK_TS", 0) > 0;
    if (pts.empty()) { std::fprintf(stderr, "[b1slack] BSI_B1_SLACK=%s: no valid point (start | present | rtN | csN, :count)\n", e); return false; }
    try {
      DxvkImageCreateInfo ii = { }; ii.type = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16G16B16A16_SFLOAT; ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      ii.extent = { 1024u, 1024u, 1u }; ii.numLayers = 1; ii.mipLevels = 1; ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
      ii.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; ii.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.layout = VK_IMAGE_LAYOUT_GENERAL; ii.debugName = "b126 slack";
      sl_img = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = ii.format; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1;
      k.layerIndex = 0; k.layerCount = 1; k.usage = VK_IMAGE_USAGE_STORAGE_BIT; k.layout = VK_IMAGE_LAYOUT_GENERAL; sl_view = sl_img->createView(k);
      emit([i = sl_img] (DxvkContext* c) { c->initImage(i, VK_IMAGE_LAYOUT_UNDEFINED); });
    } catch (const DxvkError &x) { std::fprintf(stderr, "[b1slack] scratch image failed: %s (off)\n", x.message().c_str()); return false; }
    std::string d; for (const Pt &p2 : pts) { char b[48]; std::snprintf(b, sizeof b, " %s%d x%d", p2.kind == 1 ? "start" : p2.kind == 2 ? "present" : p2.kind == 3 ? "rt" : "cs", p2.idx, p2.n); d += b; }
    std::fprintf(stderr, "[b1slack] on:%s (each = one clear of a 1024x1024 RGBA16F scratch image)%s\n", d.c_str(), sl_ts ? ", timestamps around each injection (calibration)" : "");
    sl_init = 1; return true; }
  static void inject(int n) {
    if (flipped("BSI_B1_SLACK")) return;
    sl_inj++; bool ts = sl_ts != 0; Rc<DxvkQuery> q0, q1;
    if (ts) { if (sl_q[sl_cur][0] == nullptr) { sl_q[sl_cur][0] = g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0); sl_q[sl_cur][1] = g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0); }
              q0 = sl_q[sl_cur][0]; q1 = sl_q[sl_cur][1]; sl_live[sl_cur] = true; }
    emit([v = sl_view, n, q0, q1] (DxvkContext* c) {
      if (q0 != nullptr) c->writeTimestamp(q0);
      for (int i = 0; i < n; i++) { VkClearValue cv = { }; cv.color.float32[0] = (float)(i & 7); c->clearImageView(v, VkOffset3D{ 0, 0, 0 }, VkExtent3D{ 1024u, 1024u, 1u }, VK_IMAGE_ASPECT_COLOR_BIT, cv); }
      if (q1 != nullptr) c->writeTimestamp(q1); }); }
  static inline void sl_point(int kind, int idx) { if (!sl_on()) return; for (const Pt &p : pts) if (p.kind == kind && (kind <= 2 || p.idx == idx)) inject(p.n); }
  static void on_rts(const char *label) {   /* every render-target set (b1dx_c_rts) */
    if (!sl_on()) return; rt_n++;
    if (rt_logged == 0 && sl_frames == 120) { char b[96]; std::snprintf(b, sizeof b, " | rt%d %s", rt_n, label); rt_seen += b; }
    sl_point(3, rt_n); }
  static inline void on_dispatch() { if (!sl_on()) return; cs_n++; sl_point(4, cs_n); }
  static void on_present_start() { sl_point(2, 0); }
  static void on_frame_start() {
    if (!sl_on()) return;
    if (sl_frames == 120 && !rt_logged) { rt_logged = 1; std::fprintf(stderr, "[b1slack] render-target sets of frame 120 (%d sets, %d dispatches)%s\n", rt_n, cs_n, rt_seen.c_str()); }
    rt_n = 0; cs_n = 0; sl_frames++;
    if (sl_ts) { int old = (sl_cur + 4) & 7; if (sl_live[old]) { DxvkQueryData a = { }, b = { };
        if (sl_q[old][0]->getData(a) == DxvkGpuQueryStatus::Available && sl_q[old][1]->getData(b) == DxvkGpuQueryStatus::Available) {
          sl_ms += (double)(b.timestamp.time - a.timestamp.time) * g_dev->properties().core.properties.limits.timestampPeriod * 1e-6; sl_ms_n++; }
        sl_live[old] = false; }
      sl_cur = (sl_cur + 1) & 7; }
    sl_point(1, 0);
    if (sl_frames % 600 == 0) { std::fprintf(stderr, "[b1slack] %ld frames, %ld injections%s", sl_frames, sl_inj, sl_ts ? "" : "\n");
      if (sl_ts) std::fprintf(stderr, ", injected GPU %.3f ms per injection frame (%ld timed)\n", sl_ms_n ? sl_ms / sl_ms_n : 0.0, sl_ms_n);
      sl_ms = 0; sl_ms_n = 0; } }
  static void rt_label(const DxvkRenderTargets &rts, char *b, size_t bn) {
    int nc = 0; VkExtent3D e = { 0, 0, 0 }; int f0 = 0;
    for (uint32_t i = 0; i < 8; i++) if (rts.color[i].view != nullptr) { if (!nc) { e = rts.color[i].view->mipLevelExtent(0); f0 = (int)rts.color[i].view->info().format; } nc++; }
    bool d = rts.depth.view != nullptr; if (d && !nc) e = rts.depth.view->mipLevelExtent(0);
    std::snprintf(b, bn, "%ux%u fmt%d x%d%s", e.width, e.height, f0, nc, d ? " +depth" : ""); }
  /* ---- 3. game GPU timestamp speed ---- */
  static double gts_k(double k) { static double v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_GPUTS_K"); v = e && *e ? std::atof(e) : 0.0;
      if (v > 0) std::fprintf(stderr, "[b1gts] BSI_B1_GPUTS_K=%.3f: the game's GPU timestamps run at this speed (default stereo 0.5)\n", v); }
    return v > 0 ? v : k; }
  static double gts_sum; static long gts_n, gts_fr;
  static void gts_frame(double ms, double k) { if (ms > 0) { gts_sum += ms; gts_n++; }
    if (++gts_fr % 600 == 0 && (std::getenv("BSI_B1_GPUTS_K") || std::getenv("BSI_B1_VISHASH"))) {
      std::fprintf(stderr, "[b1gts] game-measured GPU span %.3f ms (%ld frames with timestamps of 600), timestamp speed %.3f\n", gts_n ? gts_sum / gts_n : 0.0, gts_n, k); gts_sum = 0; gts_n = 0; } }
  /* ---- 4. visibility / draw-stream fingerprint ---- */
  static int vis_on() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_VISHASH", 0) > 0; if (v) std::fprintf(stderr, "[b1vis] on: draw-stream + occlusion-answer fingerprints every 600 presents\n"); } return v; }
  static uint64_t vh_draw = 1469598103934665603ull, vh_occ = 1469598103934665603ull; static long vh_ndraw, vh_nocc, vh_fr;
  static std::map<uint64_t, long> vh_frames;   /* per-frame draw-stream hash -> count (repeats show a stable scene) */
  static inline void vis_draw(uint32_t a, uint32_t b, uint32_t c) { if (!vis_on()) return; vh_draw = (vh_draw ^ a) * 1099511628211ull; vh_draw = (vh_draw ^ b) * 1099511628211ull; vh_draw = (vh_draw ^ c) * 1099511628211ull; vh_ndraw++; }
  static inline void vis_occ(uint64_t samples) { if (!vis_on()) return; vh_occ = (vh_occ ^ (samples ? 1u : 0u)) * 1099511628211ull; vh_nocc++; }
  static void vis_frame() {
    if (!vis_on()) return; vh_frames[vh_draw ^ (vh_occ << 1)]++; vh_fr++;
    if (vh_fr % 600 == 0) { long rep = 0; for (auto &e : vh_frames) if (e.second > 1) rep += e.second;
      std::fprintf(stderr, "[b1vis] 600 frames: %zu distinct draw+visibility fingerprints (%ld frames repeat one), last draw hash %016llx occ hash %016llx, %.1f draws %.1f occlusion answers/frame\n",
                   vh_frames.size(), rep, (unsigned long long)vh_draw, (unsigned long long)vh_occ, vh_ndraw / 600.0, vh_nocc / 600.0);
      vh_frames.clear(); vh_ndraw = 0; vh_nocc = 0; }
    vh_draw = 1469598103934665603ull; vh_occ = 1469598103934665603ull; }
}
