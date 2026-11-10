/* rhi_b1_m_prof.inc: m79 live profiler controls + gp GPU pass timing ([b1gp]/[b1gq]/[b1gs]/[b1audit]). Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
/* m79 (build 79, chat 3 measurement): live profiler controls in /sdcard/BSI/gpuprof.txt (BSI_GPUPROF_FILE), re-read every
   120 presents; no file = everything below off (build 78 behaviour):
     pass=1    [b1gq] GPU ms per pass, keyed by target set + the pass's first shader (stable DXBC hash), not only by target shape
     draw=N    [b1gs] one frame in N times every draw on its own (each draw ends the render pass: a distorted frame, so these
               frames are left out of [b1gp]/[b1gq]); GPU ms per target + PS + VS, i.e. per material shader
     census=1  [b1cen] resident textures / buffers by format and size, draws by vertex layout, index type and topology
     top=N     entries per line (default 40) */
static uint32_t g_m79_ps, g_m79_vs, g_m79_cs, g_m79_il, g_m79_ibt, g_m79_topo;   /* bound now (b1dx_c_shader / _il / _ib / _topology) */
extern "C" int b1dx_shader_name(uint32_t id, char *out, int n);                    /* rhi_b1_shader.cpp: "ps.<hash>", 0 = none */
extern "C" int b1dx_shader_slots(uint32_t id, uint64_t *srv, uint32_t *cb, uint32_t *ss);   /* b116, rhi_b1_shader.cpp */
namespace { namespace shrep { static int on(); static void created(uint32_t id, uint32_t size, const void *data, bool hostvis); static void wrote(uint32_t id, uint32_t off, uint32_t size, const void *data, uint32_t bufsize);
  static void gpu_wrote(uint32_t id); static void tex_written(uint32_t id); static void released(uint32_t id); static void frame(); } }   /* b115 shrep (rhi_b1_shrep.inc): shadow-map repeat counter */
namespace { namespace sheye { static void on_vol(const float *cb, bool pvt, const float *vp, bool right); static void on_proj(bool right, uint32_t wmask); static void on_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv); static void frame(); } }   /* b123 sheye (rhi_b1_sheye.inc): shared-eye shadow CHECK (BSI_B1_SHEYE=2) */
namespace m79 {
  static int pass, draw, census, top = 40, next_check; static int audit;   /* b111: audit=1: every draw of each draw-level frame on its own [b1audit] line (state, rect, counts, GPU ms, fragments) */
  static int clears;   /* es86: clears=1: every clear (RTV / DSV / UAV) and image copy gets its own gp segment ([b1gp]/[b1gq] rows "clear ..." / "copy ..."; the mark ends the render pass at every clear: measurement only) */
  static std::unordered_map<uint32_t, long> draws_il, draws_ibt, draws_topo; static double idx_sum, vtx_sum; static long ndraws;
  static std::unordered_map<uint32_t, std::string> ilsig;   /* input layout id -> attribute formats (VkFormat numbers) */
  static std::string shname(uint32_t id) { if (!id) return "-"; char b[64]; if (!b1dx_shader_name(id, b, sizeof b)) return "?" + std::to_string(id);
    std::string r(b); if (r.size() > 19) r.resize(19); return r; }   /* "ps." + 16 hex digits */
  static void check(long presents) {
    if (presents < next_check) return; next_check = (int)presents + 120;
    const char *e = std::getenv("BSI_GPUPROF_FILE"); const char *path = e && *e ? e : "/sdcard/BSI/gpuprof.txt";
    int p = 0, d = 0, c = 0, t = 40, cl = 0; char buf[512] = "";
    if (FILE *f = std::fopen(path, "r")) { size_t n = std::fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; std::fclose(f);
      const char *q = buf; char w[64]; int k;
      while (std::sscanf(q, "%63s%n", w, &k) == 1) { q += k; int v = std::atoi(std::strchr(w, '=') ? std::strchr(w, '=') + 1 : "0");
        if (!std::strncmp(w, "pass=", 5)) p = v; else if (!std::strncmp(w, "draw=", 5)) d = v < 0 ? 0 : v; else if (!std::strncmp(w, "census=", 7)) c = v; else if (!std::strncmp(w, "audit=", 6)) audit = v;   /* b111 */
        else if (!std::strncmp(w, "clears=", 7)) cl = v != 0;   /* es86 */
        else if (!std::strncmp(w, "top=", 4) && v > 0 && v <= 200) t = v; } }
    if (p != pass || d != draw || c != census || t != top || cl != clears)
      std::fprintf(stderr, "[b1gq] gpuprof.txt at present %ld: pass=%d draw=%d census=%d top=%d clears=%d (was %d %d %d %d %d)\n", presents, p, d, c, t, cl, pass, draw, census, top, clears);
    if (cl && !clears) std::fprintf(stderr, "[b1gq] clears=1: every clear and image copy is its own timed segment; the timestamp ends the render pass at every clear (a perturbation: measurement only). A whole-target clear that DXVK folds into the next pass's loadOp is timed inside that pass, not in its own row.\n");   /* es86 */
    pass = p; draw = d; census = c; top = t; clears = cl; }
}
/* GPU pass profiler (build 66; BSI_B1_GPUPROF=0 turns it off): a GPU timestamp where a new set of render targets gets its
   first draw, where a run of dispatches starts, and at present. The timestamp goes outside any render pass (the pass that
   ends there was ending anyway, since its targets changed). Results are read 4 frames later without waiting; a frame whose
   timestamps are not ready is skipped. Every 600 presents [b1gp] lists GPU ms per frame by target set. */
namespace gp {
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_GPUPROF"); v = e ? std::atoi(e) != 0 : 1; } return v && !tl126::flipped("BSI_B1_GPUPROF"); }   /* b126: ABAB B blocks = off */
  struct Meta { uint32_t count = 0, inst = 0; uint8_t indexed = 0, topo = 0, dtest = 0, dwrite = 0, dfunc = 0, stest = 0, blend = 0, wmask = 0, cull = 0; int16_t vx = 0, vy = 0, vw = 0, vh = 0, sx = 0, sy = 0, sw = 0, shh = 0; uint32_t rtfmt = 0, ncol = 0, dsv = 0, il = 0; };   /* b111 audit */
  struct Slot { std::vector<Rc<DxvkQuery>> q; std::vector<int> lab; std::vector<long> dr; int n = 0; bool live = false; int tag = 0;
                std::vector<uint32_t> sh, vs; bool dp = false; std::vector<Rc<DxvkQuery>> oq; std::vector<Meta> mt; bool oq_open = false; };
  static void (*meta_fn)(Meta &) = nullptr; static uint32_t a_count, a_inst, a_indexed;   /* b111: set by the draw entry points before pre_draw */   /* m79: first shader of each segment; dp = draw-level frame */
  static uint32_t mk_sh, mk_vs;   /* m79: shaders recorded with the next mark */
  struct QAcc { double ms = 0; long cnt = 0, dr = 0; double fmin = 1e30, fsum = 0; long fseen = 0; };   /* b91: fmin = smallest per-frame sum (frames the key appears in); mean - min = time stolen by preemption / content */
  static std::map<std::pair<int, uint32_t>, QAcc> qacc;                        /* m79 [b1gq]: (label, first shader) */
  static std::map<std::tuple<int, uint32_t, uint32_t>, QAcc> dacc;            /* m79 [b1gs]: (label, PS or CS, VS) */
  static long d_frames, dp_count; static double d_tot;
  /* s1b: per-tag sums for the S1/AO on/off A/B alternation (rhi_b1_shadoweo.inc sets tag_cur before on_present) */
  static int tag_cur; static long tag_frames[8]; static double tag_tot[8]; static std::vector<double> tag_ms[8];   /* s1c71: 8 states (bit 2 = S1c off) */
  static Slot ring[8]; static int cur;
  static std::vector<std::string> labels; static std::vector<double> ms; static std::vector<long> cnt, draws;
  static long frames, skipped; static double tot_ms;
  static int curlab = -1, plab = -1; static bool in_compute; static long ndraw; static uint64_t lastkey, pkey;
  static int lab_id(const std::string &l) { for (size_t i = 0; i < labels.size(); i++) if (labels[i] == l) return (int)i;
    labels.push_back(l); ms.push_back(0); cnt.push_back(0); draws.push_back(0); return (int)labels.size() - 1; }
  static const char *fmtname(VkFormat f) {
    switch ((int)f) { case 9: return "R8"; case 16: return "RG8"; case 37: return "RGBA8"; case 43: return "RGBA8s"; case 44: return "BGRA8"; case 50: return "BGRA8s";
      case 64: return "RGB10A2"; case 76: return "R16F"; case 83: return "RG16F"; case 97: return "RGBA16F"; case 100: return "R32F"; case 103: return "RG32F";
      case 109: return "RGBA32F"; case 122: return "R11G11B10F"; case 124: return "D16"; case 126: return "D32"; case 129: return "D24S8"; case 130: return "D32S8"; }
    static char b[16]; std::snprintf(b, sizeof b, "fmt%d", (int)f); return b; }
  /* label + key of a target set (called from b1dx_c_rts) */
  static void set_targets(const DxvkRenderTargets &rts) {
    uint64_t k = 1469598103934665603ull; int nc = 0; VkExtent3D e = { 0, 0, 0 }; VkFormat f0 = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < 8; i++) if (rts.color[i].view != nullptr) { k = (k ^ (uintptr_t)rts.color[i].view->image()) * 1099511628211ull;
      if (!nc) { e = rts.color[i].view->mipLevelExtent(0); f0 = rts.color[i].view->info().format; } nc++; }
    bool d = rts.depth.view != nullptr; if (d) { k = (k ^ (uintptr_t)rts.depth.view->image()) * 1099511628211ull; if (!nc) e = rts.depth.view->mipLevelExtent(0); }
    char b[96];
    if (nc) std::snprintf(b, sizeof b, "%ux%u %s%s%s", e.width, e.height, fmtname(f0), nc > 1 ? (std::string(" x") + std::to_string(nc)).c_str() : "", d ? " +depth" : "");
    else std::snprintf(b, sizeof b, "%ux%u depth only", e.width, e.height);
    pkey = k; plab = lab_id(b); }
  static void mark(int lab) {
    Slot &s = ring[cur];
    if (s.n >= (int)s.q.size()) { s.q.push_back(g_dev->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0)); s.lab.push_back(0); s.dr.push_back(0); s.sh.push_back(0); s.vs.push_back(0); }
    Rc<DxvkQuery> q = s.q[s.n]; s.lab[s.n] = lab; s.dr[s.n] = ndraw; s.sh[s.n] = mk_sh; s.vs[s.n] = mk_vs;
    if (s.oq_open) { s.oq_open = false; Rc<DxvkQuery> po = s.oq[s.n - 1]; emit([po] (DxvkContext* c) { c->endQuery(po); }); }   /* b111: a segment without a draw end (dispatch / clear) */
    if (s.dp && m79::audit) {   /* b111: per-segment metadata + an occlusion query (fragments passing depth / stencil) */
      if (s.oq.size() < s.q.size()) { s.oq.resize(s.q.size()); s.mt.resize(s.q.size()); }
      if (s.oq[s.n] == nullptr) s.oq[s.n] = g_dev->createGpuQuery(VK_QUERY_TYPE_OCCLUSION, VK_QUERY_CONTROL_PRECISE_BIT, 0);
      Meta m; m.count = a_count; m.inst = a_inst; m.indexed = (uint8_t)a_indexed; if (meta_fn) meta_fn(m); s.mt[s.n] = m;
      Rc<DxvkQuery> oq = s.oq[s.n]; emit([oq] (DxvkContext* c) { c->beginQuery(oq); }); s.oq_open = true; }
    s.n++;
    emit([q] (DxvkContext* c) { if (pp::rpa(c)) (c->*get(pp::EndTag()))(false); c->writeTimestamp(q); });
  }
  static inline void on_draw_done() { Slot &s = ring[cur]; if (!s.oq_open) return; s.oq_open = false; Rc<DxvkQuery> oq = s.oq[s.n - 1]; emit([oq] (DxvkContext* c) { c->endQuery(oq); }); }   /* b111 */
  static inline void on_draw() { if (g4_cap) return;   /* g4: deferred draws are counted at their replay */
    if (ring[cur].dp || in_compute || pkey != lastkey) { lastkey = pkey; curlab = plab; in_compute = false; mk_sh = g_m79_ps; mk_vs = g_m79_vs; mark(curlab); } ndraw++; }
  /* build 67: every dispatch gets its own segment, labelled by its group counts (HDAO main pass vs blurs) */
  static inline void on_compute_label(const char *l) { in_compute = true; mk_sh = g_m79_cs; mk_vs = 0; mark(lab_id(l)); }
  /* es86 (m79::clears): a clear or an image copy as its own segment; in_compute makes the next draw mark again */
  static inline void on_clear(const char *l) { if (g4_cap) return; in_compute = true; mk_sh = 0; mk_vs = 0; mark(lab_id(l)); }
  static inline void on_dispatch(uint32_t x, uint32_t y, bool alt) { in_compute = true; char b[64]; std::snprintf(b, sizeof b, "compute %ux%u%s", x, y, alt ? " (AO switch)" : ""); mk_sh = g_m79_cs; mk_vs = 0; mark(lab_id(b)); }
  /* GPU clock (Quest: kgsl sysfs, readable by apps on most builds), sampled every 30 presents */
  static double clk_sum, clk_min = 1e30, clk_max; static long clk_n; static int clk_ok = -1;
  static void clk_sample() {
    static int k; if (clk_ok == 0 || (k++ % 30)) return;
    static const char *paths[] = { "/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/kernel/gpu/gpu_clock" };
    for (const char *p : paths) { FILE *f = std::fopen(p, "r"); if (!f) continue; double v = 0; int r = std::fscanf(f, "%lf", &v); std::fclose(f);
      if (r == 1 && v > 0) { if (v > 1e5) v /= 1e6; clk_sum += v; clk_n++; if (v < clk_min) clk_min = v; if (v > clk_max) clk_max = v; clk_ok = 1; return; } }
    if (clk_ok < 0) clk_ok = 0; }
  static void resolve(Slot &s) {
    s.live = false; if (s.n < 2) return;
    std::vector<uint64_t> t(s.n);
    for (int i = 0; i < s.n; i++) { DxvkQueryData d = { }; if (s.q[i]->getData(d) != DxvkGpuQueryStatus::Available) { skipped++; return; } t[i] = d.timestamp.time; }
    double per = g_dev->properties().core.properties.limits.timestampPeriod * 1e-6;
    if (s.dp) {   /* m79: draw-level frame: its own tables only */
      for (int i = 1; i < s.n; i++) { QAcc &a = dacc[std::make_tuple(s.lab[i - 1], s.sh[i - 1], s.vs[i - 1])]; a.ms += (double)(t[i] - t[i - 1]) * per; a.cnt++; a.dr += s.dr[i] - s.dr[i - 1]; }
      d_tot += (double)(t[s.n - 1] - t[0]) * per; d_frames++;
      if (m79::audit && (int)s.mt.size() >= s.n) {   /* b111: one line per segment */
        static long af; af++; std::fprintf(stderr, "[b1audit] FRAME %ld: %d segments, %.2f ms, draws %ld\n", af, s.n - 1, (double)(t[s.n - 1] - t[0]) * per, s.dr[s.n - 1]);
        for (int i = 1; i < s.n; i++) { const Meta &m = s.mt[i - 1]; long frag = -1; if (i - 1 < (int)s.oq.size() && s.oq[i - 1] != nullptr) { DxvkQueryData d = { }; if (s.oq[i - 1]->getData(d) == DxvkGpuQueryStatus::Available) frag = (long)d.occlusion.samplesPassed; }
          char ps[48] = "-", vs[48] = "-"; if (s.sh[i - 1]) b1dx_shader_name(s.sh[i - 1], ps, sizeof ps); if (s.vs[i - 1]) b1dx_shader_name(s.vs[i - 1], vs, sizeof vs);
          std::fprintf(stderr, "[b1audit] F%ld %d %.3f | %s | %s %s | d%u%u f%u s%u b%u w%x c%u | vp %d,%d %dx%d sc %d,%d %dx%d | %s %u x%u top %u il %u | ncol %u fmt %u dsv %u | frag %ld\n", af, i, (double)(t[i] - t[i - 1]) * per, s.lab[i - 1] >= 0 && s.lab[i - 1] < (int)labels.size() ? labels[s.lab[i - 1]].c_str() : "?", ps, vs,
                       m.dtest, m.dwrite, m.dfunc, m.stest, m.blend, m.wmask, m.cull, m.vx, m.vy, m.vw, m.vh, m.sx, m.sy, m.sw, m.shh, m.indexed ? "idx" : "vtx", m.count, m.inst, m.topo, m.il, m.ncol, m.rtfmt, m.dsv, frag); } }
      return; }
    if (m79::pass) { for (int i = 1; i < s.n; i++) { QAcc &a = qacc[std::make_pair(s.lab[i - 1], s.sh[i - 1])]; double d = (double)(t[i] - t[i - 1]) * per; a.ms += d; a.cnt++; a.dr += s.dr[i] - s.dr[i - 1]; a.fsum += d; }
      for (auto &e : qacc) if (e.second.fsum > 0) { if (e.second.fsum < e.second.fmin) e.second.fmin = e.second.fsum; e.second.fseen++; e.second.fsum = 0; } }   /* b91: per-frame minimum */
    for (int i = 1; i < s.n; i++) { int l = s.lab[i - 1]; if (l < 0) l = lab_id("(frame start)"); ms[l] += (double)(t[i] - t[i - 1]) * per; cnt[l]++; draws[l] += s.dr[i] - s.dr[i - 1];
      std::vector<double> &tm = tag_ms[s.tag & 7];   /* s1c71: & 7 */ if (tm.size() < labels.size()) tm.resize(labels.size(), 0.0); tm[l] += (double)(t[i] - t[i - 1]) * per; }   /* s1b */
    tot_ms += (double)(t[s.n - 1] - t[0]) * per; frames++; tag_tot[s.tag & 7] += (double)(t[s.n - 1] - t[0]) * per; tag_frames[s.tag & 7]++; }   /* s1c71: & 7 */
  /* at present: close the frame, start the next slot, read the slot from 4 frames ago */
  static void on_present() {
    clk_sample(); mark(-1); if (ring[cur].oq_open) { Slot &s = ring[cur]; s.oq_open = false; Rc<DxvkQuery> po = s.oq[s.n - 1]; emit([po] (DxvkContext* c) { c->endQuery(po); }); } ring[cur].live = true;   /* b111 */
    cur = (cur + 1) & 7; if (ring[cur].live) { skipped++; ring[cur].live = false; } ring[cur].n = 0; ring[cur].tag = tag_cur;   /* s1b: tag */
    { static long np; m79::check(++np); ring[cur].dp = m79::draw > 0 && (++dp_count % m79::draw) == 0; mk_sh = 0; mk_vs = 0; }   /* m79 */
    Slot &old = ring[(cur + 4) & 7]; if (old.live) resolve(old); }
  static void on_frame_start() { mark(curlab); in_compute = false; }
  static void report() {
    if (!frames) { if (skipped) std::fprintf(stderr, "[b1gp] GPU pass timing: no complete frame yet (%ld skipped)\n", skipped); return; }
    std::vector<int> idx; for (size_t i = 0; i < labels.size(); i++) if (cnt[i]) idx.push_back((int)i);
    std::sort(idx.begin(), idx.end(), [] (int a, int b) { return ms[a] > ms[b]; });
    std::string t; char b[160];
    for (size_t j = 0; j < idx.size() && j < 24; j++) { int i = idx[j];   /* m79: 24 (was 14) */
      std::snprintf(b, sizeof b, " | %s: %.2f ms (%.1f passes, %.0f draws)", labels[i].c_str(), ms[i] / frames, cnt[i] / (double)frames, draws[i] / (double)frames); t += b; }
    char ck[96] = ""; if (clk_n) std::snprintf(ck, sizeof ck, ", GPU clock %.0f MHz (min %.0f, max %.0f)", clk_sum / clk_n, clk_min, clk_max);
    else if (clk_ok == 0) std::snprintf(ck, sizeof ck, ", GPU clock not readable");
    std::fprintf(stderr, "[b1gp] GPU ms/frame by render target (%ld frames timed, %ld skipped%s): total %.2f ms%s\n", frames, skipped, ck, tot_ms / frames, t.c_str());
    clk_sum = 0; clk_n = 0; clk_min = 1e30; clk_max = 0;
    /* m79: per pass (target set + first shader) */
    if (!qacc.empty()) {
      std::map<std::string, QAcc> by; for (auto &e : qacc) { std::string k = (e.first.first >= 0 && e.first.first < (int)labels.size() ? labels[e.first.first] : std::string("(frame start)")) + " | " + m79::shname(e.first.second);
        QAcc &a = by[k]; a.ms += e.second.ms; a.cnt += e.second.cnt; a.dr += e.second.dr; if (e.second.fmin < a.fmin) a.fmin = e.second.fmin; a.fseen += e.second.fseen; }
      std::vector<std::pair<std::string, QAcc>> v(by.begin(), by.end()); std::sort(v.begin(), v.end(), [] (const auto &a, const auto &b) { return a.second.ms > b.second.ms; });
      std::string o; double minsum = 0;
      for (size_t j = 0; j < v.size() && (int)j < m79::top; j++) { const QAcc &a = v[j].second; double mn = a.fmin < 1e29 ? a.fmin : 0; minsum += mn;   /* b91: min per frame it appears in */
        std::snprintf(b, sizeof b, " | %s: %.3f ms (%.2f/frame, %.1f draws, min %.3f)", v[j].first.c_str(), a.ms / frames, a.cnt / (double)frames, a.dr / (double)frames, mn); o += b; }
      std::snprintf(b, sizeof b, " | [b91] sum of per-pass minima (top %d) %.2f ms vs mean total %.2f", m79::top, minsum, tot_ms / frames); o += b;
      std::fprintf(stderr, "[b1gq] GPU ms/frame by pass (target | first shader; %ld frames, %zu keys)%s\n", frames, v.size(), o.c_str()); qacc.clear(); }
    /* m79: draw-level frames (per material shader) */
    if (d_frames) {
      std::map<std::string, QAcc> by, byps; for (auto &e : dacc) { int l = std::get<0>(e.first); std::string lab = l >= 0 && l < (int)labels.size() ? labels[l] : std::string("(frame start)");
        std::string ps = m79::shname(std::get<1>(e.first)), k = lab + " | " + ps + " " + m79::shname(std::get<2>(e.first));
        QAcc &a = by[k]; a.ms += e.second.ms; a.cnt += e.second.cnt; a.dr += e.second.dr; QAcc &c = byps[ps]; c.ms += e.second.ms; c.cnt += e.second.cnt; c.dr += e.second.dr; }
      for (int pass2 = 0; pass2 < 2; pass2++) { std::map<std::string, QAcc> &m = pass2 ? byps : by;
        std::vector<std::pair<std::string, QAcc>> v(m.begin(), m.end()); std::sort(v.begin(), v.end(), [] (const auto &a, const auto &b) { return a.second.ms > b.second.ms; });
        std::string o; for (size_t j = 0; j < v.size() && (int)j < m79::top; j++) { std::snprintf(b, sizeof b, " | %s: %.3f ms (%.1f draws, %.1f us/draw)", v[j].first.c_str(), v[j].second.ms / d_frames, v[j].second.dr / (double)d_frames, v[j].second.dr ? 1000.0 * v[j].second.ms / v[j].second.dr : 0.0); o += b; }
        std::fprintf(stderr, "[b1gs] draw-level GPU ms per profiled frame by %s (%ld frames, whole frame %.2f ms; normal frames %.2f ms; %zu keys)%s\n", pass2 ? "PS/CS" : "target | PS/CS VS", d_frames, d_tot / d_frames, tot_ms / frames, v.size(), o.c_str()); }
      dacc.clear(); d_frames = 0; d_tot = 0; }
    for (size_t i = 0; i < labels.size(); i++) { ms[i] = 0; cnt[i] = 0; draws[i] = 0; } frames = 0; skipped = 0; tot_ms = 0; }
}
