/* b125 shclass: SHADOW PIXEL CLASSES (measurement only; BSI_B1_SHCLASS=1, default 0 = no work, build 124 behaviour).
   Sizes brainstorm round 2 items R2 (projection stencil split), R3 (tighter S1 window cover) and the spot / point pre-test part
   of R1 (plans\bsi-brainstorm-2026-11-02.md). Every BSI_B1_SHCLASS_EVERY (120) frames the k-th shadow-projection draw of the
   frame that uses an S1 twin (k rotates over the frame's projection draws) is captured just BEFORE it runs: the shadow atlas
   (PS t1, D32), the scene depth (PS t0), the stencil of the bound depth-stencil target, and CPU copies of PS cb0 / cb2 and
   VS cb0 (sb87's CPU constant store). At frame end (one GPU wait) every pixel the quad shades (viewport / scissor rect and
   the quad's stencil test) is replayed on the CPU with the kernel's own math (cd4a8dad / 2e583db5 disassembly):
     uv = ndc * cb2[0].xy + cb2[0].wz, d = min(depth(uv), 0.999), w = 1 / (d * cb2[1].z - cb2[1].w),
     p = (VS cb0[0] * ndc.x + cb0[1] * ndc.y + cb0[2]) * w, s = p.x cb0[0] + p.y cb0[1] + p.z cb0[2] + cb0[3],
     recv = min(s.z, 0.999), fl = floor(s.xy / s.w * D.xy + 0.5) (D = the kernel's dims row), window = texels fl-8 .. fl+7.
   Classes: P = skipped by the kernel's own light pre-test (per-object 0.001 cone x falloff test; point: falloff only, an
   approximation), 0 = settled by S1 today (L2 = min/max over 6x6 L1 blocks of 4x4 texels from block(fl-9)), 1 = full path
   today but the exact 16x16 window is uniform (count 0 or 256: what a tight cover, R3, would also settle), 2 = penumbra
   (0 < count < 256), M = window leaves the atlas (S1 does not decide; counted with the full path).
   Wave estimate: per eye-local 8x8 and 16x8 pixel block, the drawn pixels of blocks that contain a full-path pixel (today
   every lane of such a wave pays the search) vs the full-path pixels themselves (R2: a split pass B runs only those) vs the
   class-2 pixels (R2 + R3). Reported per kernel kind ([b1shclass] lines, cumulative). Timing during capture frames is not
   valid (GPU wait + CPU replay); the picture is unchanged (copies only). */
/* b127 SHCLASS v2 (same switch; still measurement only, picture unchanged):
   - captures start at frame BSI_B1_SHCLASS_START (3300: after the autopilot's level load, b125 captured the title scene
     XEntry_P at frames 720-960 and its GPU waits desynced the menu presses into Quit), every BSI_B1_SHCLASS_EVERY (300);
   - S1 emulation now mirrors the twin's wrap shift (window shifted by k atlas sizes, |k| < 64, then must lie inside):
     off-map splits into 'straddle' (window crosses the atlas seam after the shift; exact wrapped window uniform lit /
     shadowed / penumbra reported) and 'far' (|k| >= 64, NaN or huge coordinates);
   - exact N.L sizing for the sun: the first sun light draw (PS d0e72728ca566e90 = fs.5dc7e57b, light pre-pass) of the same
     eye after the captured cascade projection gives the G-buffer normal (t0) and cb0[1] (the light vector of
     dp3 + mov_sat); per drawn pixel n = normalize(t0.xyz * 2 - 1), d = dot(cb0[1].xyz, n): 'zero' d <= -1e-3 (both light
     outputs are att * colour * sat(d) * ... = 0 whatever the attenuation), 'edge' |d| < 1e-3, 'lit' d >= 1e-3;
   - census of every draw that samples the attenuation render target after the captured projection in that frame (PS keys). */
namespace {
namespace shclass {
  static int mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHCLASS"); v = e && *e ? std::atoi(e) : 0; if (v < 0) v = 0; } return v; }
  static int every() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHCLASS_EVERY"); v = e && *e ? std::atoi(e) : 300; if (v < 2) v = 2; } return v; }
  static long start() { static long v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHCLASS_START"); v = e && *e ? std::atol(e) : 3300; if (v < 0) v = 0; } return v; }
  static const unsigned long long kSunLight = 0xd0e72728ca566e90ull;   /* fs.5dc7e57b, the sun light of the light pre-pass */
  static std::unordered_map<uint32_t, unsigned long long> pskey;          /* every PS id -> dxbc key (mu) */
  static uint32_t sun_id; static uint32_t att_res;
  static bool lcap; static Rc<DxvkBuffer> nbuf; static VkDeviceSize nsz; static uint32_t NW, NH; static VkFormat nfmt; static float lcb[12];
  static std::unordered_map<unsigned long long, long> census; static long census_frames;
  static std::mutex mu; static std::unordered_map<uint32_t, int> kinds;   /* game PS id -> 0 cascade (c29c9e4e), 1 per-object (d108de46), 2 point (59c8d61b) */
  static void note_any_ps(uint32_t id, const void *code, uint32_t len) {
    if (!mode() || !id || !code) return; unsigned long long h = 0xcbf29ce484222325ull; const uint8_t *b = (const uint8_t *)code; for (uint32_t i = 0; i < len; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    h ^= (unsigned long long)len; h *= 0x100000001b3ull; std::lock_guard<std::mutex> lk(mu); pskey[id] = h; if (h == kSunLight) sun_id = id; }
  static const char *kname[3] = { "cascade/sun", "per-object", "point" };
  static int dims_row(int k) { return k == 0 ? 6 : k == 1 ? 9 : 8; }
  static void note_ps(uint32_t id, unsigned long long key) {
    if (!mode()) return; int k = key == 0xc29c9e4e876e162bull ? 0 : key == 0xd108de4649b4e7c0ull ? 1 : key == 0x59c8d61ba14cdddcull ? 2 : -1;
    if (k < 0) return; std::lock_guard<std::mutex> lk(mu); kinds[id] = k; }
  /* the pending capture */
  static Rc<DxvkBuffer> abuf, dbuf, sbuf; static VkDeviceSize asz, dsz, ssz; static bool pending;
  static uint32_t AW, AH, DW, DH, SW, SH; static VkFormat dfmt, sfmt; static int kind; static float ps0[48], ps2[8], vs0[12], vp[6]; static int32_t rect[4];
  static uint32_t s_ref, s_mask; static VkCompareOp s_op; static long cap_frame;
  static long f_frames, n_now, n_prev, rot; static long target = -1; static long n_skip[6];
  static const char *skipwhy[6] = { "no views", "formats", "constants unknown", "no stencil test", "kind unknown", "" };
  struct Acc { long caps = 0, drawn = 0, pre = 0, c0 = 0, c1lit = 0, c1sh = 0, c2 = 0, off = 0;
               long sh_c0 = 0, st_lit = 0, st_sh = 0, st_pen = 0, far = 0;        /* b127: settled after the twin's shift; straddle classes; far */
               long nlx_caps = 0, nlx_px = 0, nlx_zero = 0, nlx_edge = 0, nlx_lit = 0, nlx_full = 0, nlx_full_zero = 0; double axis_dot = 0;   /* b127 exact N.L */
               long b8_full = 0, b8_px = 0, b16_full = 0, b16_px = 0, b8_c2 = 0, b8_c2px = 0, b16_c2 = 0, b16_c2px = 0;
               long nl_known = 0, nl_back = 0, nl_back_full = 0; };   /* R1 approximation: geometric normal from the depth buffer */
  static Acc acc[3];
  static bool is_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
  static bool has_stencil(VkFormat f) { return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
  static void mk(Rc<DxvkBuffer> &b, VkDeviceSize &have, VkDeviceSize sz, const char *nm) {
    if (b != nullptr && have == sz) return; DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = nm; b = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); have = sz; }
  static const float *cbcopy(uint32_t id, uint32_t need) {   /* sb87's CPU store of constant-buffer writes */
    if (!id || id == BC_UNK) return nullptr; sb87::cbwant.insert(id); auto it = sb87::cbs.find(id); return it == sb87::cbs.end() || it->second.n < need ? nullptr : (const float *)it->second.b.data(); }
  static void capture(int k) {
    View *v0 = g_views.get(g_bc.srv[1][0] == BC_UNK ? 0 : g_bc.srv[1][0]), *v1 = g_views.get(g_bc.srv[1][1] == BC_UNK ? 0 : g_bc.srv[1][1]), *dv = g_views.get(g_cs.dsv);
    if (!v0 || v0->type != 1 || !v1 || v1->type != 1 || !dv || dv->type != 3) { n_skip[0]++; return; }
    Obj *o0 = robj(v0->res), *o1 = robj(v1->res), *od = robj(dv->res);
    if (!o0 || o0->kind != 2 || !o1 || o1->kind != 2 || !od || od->kind != 2 || o1->fmt != VK_FORMAT_D32_SFLOAT || !has_stencil(od->fmt)) { n_skip[1]++; return; }
    if (!is_depth(o0->fmt) && o0->fmt != VK_FORMAT_R32_SFLOAT) { if (n_skip[1]++ < 3) std::fprintf(stderr, "[b1shclass] scene depth t0 format %d not handled\n", (int)o0->fmt); return; }
    int dr = dims_row(k); const float *c0 = cbcopy(g_bc.cb[1][0], (uint32_t)(dr + 1) * 16), *c2 = cbcopy(g_bc.cb[1][2], 32), *cv = cbcopy(g_bc.cb[0][0], 48);
    if (!c0 || !c2 || !cv) { n_skip[2]++; return; }
    DSS *d = g_dsss.get(g_cs.dss); if (!d || !d->s.stencilTest()) { n_skip[3]++; return; }
    std::memcpy(ps0, c0, sizeof(float) * 4 * (dr + 1)); std::memcpy(ps2, c2, sizeof ps2); std::memcpy(vs0, cv, sizeof vs0);
    s_op = d->s.stencilOpFront().compareOp(); s_mask = d->s.stencilOpFront().compareMask(); s_ref = g_cs.sref & 0xffu;
    if (!g_cs.nvp) { n_skip[0]++; return; } for (int i = 0; i < 6; i++) vp[i] = g_cs.vp[0][i];
    rect[0] = (int32_t)std::ceil(vp[0]); rect[1] = (int32_t)std::ceil(vp[1]); rect[2] = (int32_t)std::floor(vp[0] + vp[2]); rect[3] = (int32_t)std::floor(vp[1] + vp[3]);
    { RS *rs = g_rss.get(g_cs.rs); if (rs && rs->scissor && g_cs.nsc) { const int32_t *r = g_cs.sc[0]; rect[0] = std::max(rect[0], r[0]); rect[1] = std::max(rect[1], r[1]); rect[2] = std::min(rect[2], r[2]); rect[3] = std::min(rect[3], r[3]); } }
    VkExtent3D ae = o1->img->mipLevelExtent(0), de = o0->img->mipLevelExtent(0), se = od->img->mipLevelExtent(0);
    AW = ae.width; AH = ae.height; DW = de.width; DH = de.height; SW = se.width; SH = se.height; dfmt = o0->fmt; sfmt = od->fmt;
    rect[0] = std::max(rect[0], 0); rect[1] = std::max(rect[1], 0); rect[2] = std::min(rect[2], (int32_t)SW); rect[3] = std::min(rect[3], (int32_t)SH);
    uint32_t db = (dfmt == VK_FORMAT_D16_UNORM || dfmt == VK_FORMAT_D16_UNORM_S8_UINT) ? 2u : 4u;
    mk(abuf, asz, (VkDeviceSize)AW * AH * 4, "B1 shclass atlas"); mk(dbuf, dsz, (VkDeviceSize)DW * DH * db, "B1 shclass depth"); mk(sbuf, ssz, (VkDeviceSize)SW * SH, "B1 shclass stencil");
    VkImageSubresourceLayers al = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u }, dl = { (VkImageAspectFlags)(is_depth(dfmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0u, 0u, 1u },
      sl = { VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 0u, 1u };
    emit([ab = abuf, af = o1->fmt, ai = o1->img, al, ae, dbb = dbuf, df = o0->fmt, di = o0->img, dl, de, sb = sbuf, sf = od->fmt, si = od->img, sl, se] (DxvkContext* c) {
      c->copyImageToBuffer(ab, 0, 0, 0, af, ai, al, VkOffset3D{ 0, 0, 0 }, ae);
      c->copyImageToBuffer(dbb, 0, 0, 0, df, di, dl, VkOffset3D{ 0, 0, 0 }, de);
      c->copyImageToBuffer(sb, 0, 0, 0, sf, si, sl, VkOffset3D{ 0, 0, 0 }, se); });
    { View *rv = g_views.get(g_cs.rtv[0]); att_res = (rv && rv->type == 2) ? rv->res : 0; }
    lcap = false; census_frames++;
    kind = k; pending = true; cap_frame = f_frames; }
  /* b127: every draw while a capture is pending: census of attenuation readers; the sun light draw of the captured eye */
  static void on_draw(uint32_t ps) {
    if (!mode() || !pending || !att_res) return;
    bool reads = false; for (uint32_t i = 0; i < 16; i++) { uint32_t sv = g_bc.srv[1][i]; if (!sv || sv == BC_UNK) continue; View *v = g_views.get(sv); if (v && v->type == 1 && v->res == att_res) { reads = true; break; } }
    if (reads) { unsigned long long k = 0; { std::lock_guard<std::mutex> lk(mu); auto it = pskey.find(ps); if (it != pskey.end()) k = it->second; } census[k]++; }
    if (kind != 0 || lcap || !ps || ps != sun_id || !g_cs.nvp) return;
    if (g_cs.vp[0][0] > vp[0] + 0.5f || g_cs.vp[0][0] + g_cs.vp[0][2] < vp[0] + vp[2] - 0.5f) return;   /* a draw covering the captured eye's column */
    View *nv = g_views.get(g_bc.srv[1][0] == BC_UNK ? 0 : g_bc.srv[1][0]); if (!nv || nv->type != 1) return; Obj *no = robj(nv->res); if (!no || no->kind != 2) return;
    if (no->fmt != VK_FORMAT_R8G8B8A8_UNORM && no->fmt != VK_FORMAT_B8G8R8A8_UNORM && no->fmt != VK_FORMAT_A2B10G10R10_UNORM_PACK32) { static int w; if (w++ < 3) std::fprintf(stderr, "[b1shclass] sun light t0 format %d not handled\n", (int)no->fmt); return; }
    const float *c = cbcopy(g_bc.cb[1][0], 48); if (!c) return; std::memcpy(lcb, c, sizeof lcb);
    VkExtent3D ne = no->img->mipLevelExtent(0); NW = ne.width; NH = ne.height; nfmt = no->fmt; mk(nbuf, nsz, (VkDeviceSize)NW * NH * 4, "B1 shclass normal");
    VkImageSubresourceLayers nl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    emit([nb = nbuf, nf = no->fmt, ni = no->img, nl, ne] (DxvkContext* cx) { cx->copyImageToBuffer(nb, 0, 0, 0, nf, ni, nl, VkOffset3D{ 0, 0, 0 }, ne); });
    lcap = true; }
  /* eo::pre_draw: a projection draw with an S1 twin available (cur_ps = the game's shader) */
  static void on_proj(uint32_t ps) {
    if (!mode()) return; long idx = n_now++; if (idx != target || pending) return; target = -1;
    int k = -1; { std::lock_guard<std::mutex> lk(mu); auto it = kinds.find(ps); if (it != kinds.end()) k = it->second; }
    if (k < 0) { n_skip[4]++; return; } capture(k); }
  static bool stest(uint32_t s) {
    uint32_t a = s_ref & s_mask, b = s & s_mask;
    switch (s_op) { case VK_COMPARE_OP_NEVER: return false; case VK_COMPARE_OP_LESS: return a < b; case VK_COMPARE_OP_EQUAL: return a == b; case VK_COMPARE_OP_LESS_OR_EQUAL: return a <= b;
      case VK_COMPARE_OP_GREATER: return a > b; case VK_COMPARE_OP_NOT_EQUAL: return a != b; case VK_COMPARE_OP_GREATER_OR_EQUAL: return a >= b; default: return true; } }
  static void analyse() {
    const float *A = (const float *)abuf->mapPtr(0); const uint8_t *D = (const uint8_t *)dbuf->mapPtr(0), *S = (const uint8_t *)sbuf->mapPtr(0);
    const uint8_t *N = (lcap && nbuf != nullptr) ? (const uint8_t *)nbuf->mapPtr(0) : nullptr; if (!N) lcap = false;
    const int W = (int)AW, H = (int)AH;
    /* exact 16x16 window min / max (window origin = top-left texel), wrap handled by the slow path */
    std::vector<float> rmn((size_t)W * H), rmx((size_t)W * H), wmn((size_t)W * H), wmx((size_t)W * H);
    for (int y = 0; y < H; y++) for (int x = 0; x + 15 < W; x++) { float mn = A[(size_t)y * W + x], mx = mn; for (int i = 1; i < 16; i++) { float t = A[(size_t)y * W + x + i]; mn = std::min(mn, t); mx = std::max(mx, t); } rmn[(size_t)y * W + x] = mn; rmx[(size_t)y * W + x] = mx; }
    for (int y = 0; y + 15 < H; y++) for (int x = 0; x + 15 < W; x++) { float mn = rmn[(size_t)y * W + x], mx = rmx[(size_t)y * W + x]; for (int j = 1; j < 16; j++) { mn = std::min(mn, rmn[(size_t)(y + j) * W + x]); mx = std::max(mx, rmx[(size_t)(y + j) * W + x]); } wmn[(size_t)y * W + x] = mn; wmx[(size_t)y * W + x] = mx; }
    /* S1: L1 = 4x4 blocks, L2[b] = blocks b .. b+5 per axis */
    const int BW = (W + 3) / 4, BH = (H + 3) / 4; std::vector<float> l1n((size_t)BW * BH), l1x((size_t)BW * BH), l2n((size_t)BW * BH), l2x((size_t)BW * BH);
    for (int by = 0; by < BH; by++) for (int bx = 0; bx < BW; bx++) { float mn = INFINITY, mx = -INFINITY; for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { int x = bx * 4 + i, y = by * 4 + j; if (x < W && y < H) { float t = A[(size_t)y * W + x]; mn = std::min(mn, t); mx = std::max(mx, t); } } l1n[(size_t)by * BW + bx] = mn; l1x[(size_t)by * BW + bx] = mx; }
    for (int by = 0; by < BH; by++) for (int bx = 0; bx < BW; bx++) { float mn = INFINITY, mx = -INFINITY; for (int j = 0; j < 6; j++) for (int i = 0; i < 6; i++) { int x = bx + i, y = by + j; if (x < BW && y < BH) { mn = std::min(mn, l1n[(size_t)y * BW + x]); mx = std::max(mx, l1x[(size_t)y * BW + x]); } } l2n[(size_t)by * BW + bx] = mn; l2x[(size_t)by * BW + bx] = mx; }
    auto depth = [&] (float u, float v) -> float {   /* point sample of t0 */
      int x = (int)std::floor(u * (float)DW), y = (int)std::floor(v * (float)DH); x = std::min(std::max(x, 0), (int)DW - 1); y = std::min(std::max(y, 0), (int)DH - 1); size_t i = (size_t)y * DW + x;
      if (dfmt == VK_FORMAT_D16_UNORM || dfmt == VK_FORMAT_D16_UNORM_S8_UINT) { uint16_t t; std::memcpy(&t, D + i * 2, 2); return (float)t / 65535.0f; }
      uint32_t t; std::memcpy(&t, D + i * 4, 4); if (dfmt == VK_FORMAT_D24_UNORM_S8_UINT || dfmt == VK_FORMAT_X8_D24_UNORM_PACK32) return (float)(t & 0xffffffu) / 16777215.0f;
      float f; std::memcpy(&f, &t, 4); return f; };
    const int dr = dims_row(kind); const float *M = ps0, *Dm = ps0 + dr * 4; Acc a; a.caps = 1;
    const int x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3]; if (x1 <= x0 || y1 <= y0) { acc[kind].caps++; return; }
    const int cw = x1 - x0, ch = y1 - y0; std::vector<uint8_t> cls((size_t)cw * ch, 0xff);   /* 0xff not drawn, 'P', 0, 1, 2, 'M' */
    auto posf = [&] (int x, int y, float *p) {   /* the kernel's camera-relative receiver position at pixel (x, y) */
      float nx = ((float)x + 0.5f - vp[0]) / vp[2] * 2.0f - 1.0f, ny = 1.0f - ((float)y + 0.5f - vp[1]) / vp[3] * 2.0f;
      float u = nx * ps2[0] + ps2[3], v = ny * ps2[1] + ps2[2]; float dd = std::min(depth(u, v), 0.999f); float w = 1.0f / (dd * ps2[6] - ps2[7]);
      for (int k = 0; k < 3; k++) p[k] = (vs0[k] * nx + vs0[4 + k] * ny + vs0[8 + k]) * w; };
    /* R1 approximation: geometric normal (smaller-step neighbour per axis) facing the camera; the light direction is
       -grad(shadow depth) for the cascades (the light-space z axis of cb0 rows 0..2), (light pos - p) for per-object / point.
       N.L < -0.05 (margin for normal maps) counts as 'surely faces away' (the light pre-pass multiplies by saturate(N.L)). */
    auto nl_back = [&] (int x, int y, const float *p, bool &known) -> bool {
      known = false; if (x <= 0 || y <= 0 || x + 1 >= (int)SW || y + 1 >= (int)SH) return false;
      float a[3], b[3], c[3], d[3]; posf(x - 1, y, a); posf(x + 1, y, b); posf(x, y - 1, c); posf(x, y + 1, d);
      float dx[3], dy[3]; float la = 0, lb = 0, lc = 0, ld = 0; for (int k = 0; k < 3; k++) { la += (p[k] - a[k]) * (p[k] - a[k]); lb += (b[k] - p[k]) * (b[k] - p[k]); lc += (p[k] - c[k]) * (p[k] - c[k]); ld += (d[k] - p[k]) * (d[k] - p[k]); }
      for (int k = 0; k < 3; k++) { dx[k] = la < lb ? p[k] - a[k] : b[k] - p[k]; dy[k] = lc < ld ? p[k] - c[k] : d[k] - p[k]; }
      float n[3] = { dx[1] * dy[2] - dx[2] * dy[1], dx[2] * dy[0] - dx[0] * dy[2], dx[0] * dy[1] - dx[1] * dy[0] };
      float nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (!(nn > 0.0f) || !std::isfinite(nn)) return false;
      if (n[0] * p[0] + n[1] * p[1] + n[2] * p[2] > 0.0f) nn = -nn;   /* face the camera */
      float l[3]; if (kind == 0) { l[0] = -ps0[2]; l[1] = -ps0[6]; l[2] = -ps0[10]; } else { l[0] = ps0[16] - p[0]; l[1] = ps0[17] - p[1]; l[2] = ps0[18] - p[2]; }
      float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]); if (!(ll > 0.0f)) return false;
      known = true; return (n[0] * l[0] + n[1] * l[1] + n[2] * l[2]) / (nn * ll) < -0.05f; };
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
      if (!stest(S[(size_t)y * SW + x])) continue; a.drawn++;
      float nx = ((float)x + 0.5f - vp[0]) / vp[2] * 2.0f - 1.0f, ny = 1.0f - ((float)y + 0.5f - vp[1]) / vp[3] * 2.0f;
      float u = nx * ps2[0] + ps2[3], v = ny * ps2[1] + ps2[2];
      float dd = std::min(depth(u, v), 0.999f); float w = 1.0f / (dd * ps2[6] - ps2[7]);
      float p[3]; for (int k = 0; k < 3; k++) p[k] = (vs0[k] * nx + vs0[4 + k] * ny + vs0[8 + k]) * w;
      uint8_t c;
      bool pre = false;
      if (kind >= 1) {   /* the kernel's light pre-test: x = cone^2 * falloff (per-object), falloff (point); 0.001 < x runs the shadow */
        float l[3] = { M[16] - p[0], M[17] - p[1], M[18] - p[2] }, r2[3] = { l[0] * M[19], l[1] * M[19], l[2] * M[19] };
        float t = std::max(1.0f - (r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]), 0.0f); float fall = std::exp2(std::log2(t) * M[20]);
        float x2 = fall;
        if (kind == 1) { float il = 1.0f / std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]); float cn = std::min(std::max(l[0] * il * M[24] + l[1] * il * M[25] + l[2] * il * M[26] - M[27], 0.0f), 1.0f); x2 = cn * cn * fall; }
        pre = !(0.001f < x2); }
      if (pre) { c = 'P'; a.pre++; }
      else {
        float s[4]; for (int k = 0; k < 4; k++) s[k] = p[1] * M[4 + k] + M[0 + k] * p[0] + M[8 + k] * p[2] + M[12 + k];
        float sx = s[0] / s[3], sy = s[1] / s[3], recv = std::min(s[2], 0.999f);
        float fxf = std::floor(Dm[0] * sx + 0.5f), fyf = std::floor(Dm[1] * sy + 0.5f);
        if (!(fxf == fxf) || !(fyf == fyf) || std::fabs(fxf) > 1e8f || std::fabs(fyf) > 1e8f || !(recv == recv)) { c = 'M'; a.off++; }
        else {
          long fx = (long)fxf, fy = (long)fyf; long ox = fx - 8, oy = fy - 8;   /* exact window: texels ox .. ox+15 */
          bool inside = ox >= 0 && oy >= 0 && ox + 15 < W && oy + 15 < H; float wn, wx;
          if (inside) { wn = wmn[(size_t)oy * W + ox]; wx = wmx[(size_t)oy * W + ox]; }
          else { wn = INFINITY; wx = -INFINITY; for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) { long tx = ((ox + i) % W + W) % W, ty = ((oy + j) % H + H) % H; float t = A[(size_t)ty * W + tx]; wn = std::min(wn, t); wx = std::max(wx, t); } }
          /* S1 as the twin does it (b127): shift the cover by k atlas sizes (|k| < 64), then it must lie inside */
          long lo_x = fx - 9, lo_y = fy - 9, hi_x = fx + 8, hi_y = fy + 8; int s1 = -1;
          long kx = (long)std::floor((double)lo_x / W), ky = (long)std::floor((double)lo_y / H);
          bool far = std::labs(kx) >= 64 || std::labs(ky) >= 64;
          long slx = lo_x - kx * W, sly = lo_y - ky * H, shx = hi_x - kx * W, shy = hi_y - ky * H;
          bool cover_in = !far && slx >= 0 && sly >= 0 && shx < W && shy < H;
          if (cover_in) { long bx = slx >> 2, by = sly >> 2; if ((shx >> 2) - bx <= 5 && (shy >> 2) - by <= 5) {
              float mn = l2n[(size_t)by * BW + bx], mx = l2x[(size_t)by * BW + bx]; if (mn >= recv) s1 = 0; else if (!(mx >= recv)) s1 = 1; } }
          bool unshifted = lo_x >= 0 && lo_y >= 0 && hi_x < W && hi_y < H;
          if (s1 >= 0) { c = 0; a.c0++; if (!unshifted) a.sh_c0++; }
          else if (far) { c = 'M'; a.off++; a.far++; }
          else if (!cover_in) { c = 'M'; a.off++; if (wn >= recv) a.st_lit++; else if (!(wx >= recv)) a.st_sh++; else a.st_pen++; }
          else { if (!inside) { long sox = ox - kx * W, soy = oy - ky * H; if (sox >= 0 && soy >= 0 && sox + 15 < W && soy + 15 < H) { wn = wmn[(size_t)soy * W + sox]; wx = wmx[(size_t)soy * W + sox]; } }
            if (wn >= recv) { c = 1; a.c1lit++; }
            else if (!(wx >= recv)) { c = 1; a.c1sh++; }
            else { c = 2; a.c2++; } } } }
      if (kind == 0 && lcap && c != 'P') {   /* b127 exact N.L of the sun light draw */
        long nxp = (long)((double)x * NW / SW), nyp = (long)((double)y * NH / SH);
        if (nxp >= 0 && nyp >= 0 && nxp < (long)NW && nyp < (long)NH) { uint32_t t; std::memcpy(&t, N + ((size_t)nyp * NW + nxp) * 4, 4); float n[3];
          if (nfmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { n[0] = (float)(t & 1023u) / 1023.0f; n[1] = (float)((t >> 10) & 1023u) / 1023.0f; n[2] = (float)((t >> 20) & 1023u) / 1023.0f; }
          else if (nfmt == VK_FORMAT_B8G8R8A8_UNORM) { n[2] = (float)(t & 255u) / 255.0f; n[1] = (float)((t >> 8) & 255u) / 255.0f; n[0] = (float)((t >> 16) & 255u) / 255.0f; }
          else { n[0] = (float)(t & 255u) / 255.0f; n[1] = (float)((t >> 8) & 255u) / 255.0f; n[2] = (float)((t >> 16) & 255u) / 255.0f; }
          for (int k = 0; k < 3; k++) n[k] = n[k] * 2.0f - 1.0f;
          float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
          if (nn > 0.0f) { float r = 1.0f / std::sqrt(nn); float d = (lcb[4] * n[0] + lcb[5] * n[1] + lcb[6] * n[2]) * r; a.nlx_px++;
            bool full = (c == 1 || c == 2 || c == 'M');
            if (full) a.nlx_full++;
            if (d <= -1e-3f) { a.nlx_zero++; if (full) a.nlx_full_zero++; } else if (d < 1e-3f) a.nlx_edge++; else a.nlx_lit++; } } }
      if (c != 'P') { bool known; bool back = nl_back(x, y, p, known); if (known) { a.nl_known++; if (back) { a.nl_back++; if (c == 1 || c == 2 || c == 'M') a.nl_back_full++; } } }
      cls[(size_t)(y - y0) * cw + (x - x0)] = c; }
    /* wave blocks: 8x8 and 16x8, aligned to the eye column origin (viewport x) */
    int ex = (int)std::floor(vp[0]), ey = (int)std::floor(vp[1]);
    for (int pass = 0; pass < 2; pass++) { int bw = pass ? 16 : 8, bh = 8;
      int gx0 = ex + (int)std::floor((double)(x0 - ex) / bw) * bw, gy0 = ey + (int)std::floor((double)(y0 - ey) / bh) * bh;
      for (int by = gy0; by < y1; by += bh) for (int bx = gx0; bx < x1; bx += bw) { long drawn = 0, full = 0, pen = 0;
        for (int y = std::max(by, y0); y < std::min(by + bh, y1); y++) for (int x = std::max(bx, x0); x < std::min(bx + bw, x1); x++) { uint8_t c = cls[(size_t)(y - y0) * cw + (x - x0)];
          if (c == 0xff) continue; drawn++; if (c == 1 || c == 2 || c == 'M') full++; if (c == 2 || c == 'M') pen++; }
        if (full) { (pass ? a.b16_full : a.b8_full) += drawn; (pass ? a.b16_px : a.b8_px) += full; }
        if (pen) { (pass ? a.b16_c2 : a.b8_c2) += drawn; (pass ? a.b16_c2px : a.b8_c2px) += pen; } } }
    Acc &t = acc[kind]; t.caps += a.caps; t.drawn += a.drawn; t.pre += a.pre; t.c0 += a.c0; t.c1lit += a.c1lit; t.c1sh += a.c1sh; t.c2 += a.c2; t.off += a.off;
    t.nl_known += a.nl_known; t.nl_back += a.nl_back; t.nl_back_full += a.nl_back_full;
    t.sh_c0 += a.sh_c0; t.st_lit += a.st_lit; t.st_sh += a.st_sh; t.st_pen += a.st_pen; t.far += a.far;
    if (kind == 0 && lcap) { t.nlx_caps++; t.nlx_px += a.nlx_px; t.nlx_zero += a.nlx_zero; t.nlx_edge += a.nlx_edge; t.nlx_lit += a.nlx_lit; t.nlx_full += a.nlx_full; t.nlx_full_zero += a.nlx_full_zero;
      float ax[3] = { -ps0[2], -ps0[6], -ps0[10] }, al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), ll = std::sqrt(lcb[4] * lcb[4] + lcb[5] * lcb[5] + lcb[6] * lcb[6]);
      double dd = (al > 0 && ll > 0) ? (ax[0] * lcb[4] + ax[1] * lcb[5] + ax[2] * lcb[6]) / (al * ll) : 0.0; t.axis_dot = dd;
      std::fprintf(stderr, "[b1shclass] sun light cb0[1] = (%.6f %.6f %.6f %.6f), |L| %.6f, cos to the cascade axis %.8f, normal t0 %ux%u fmt %d\n", lcb[4], lcb[5], lcb[6], lcb[7], ll, dd, NW, NH, (int)nfmt); }
    t.b8_full += a.b8_full; t.b8_px += a.b8_px; t.b16_full += a.b16_full; t.b16_px += a.b16_px; t.b8_c2 += a.b8_c2; t.b8_c2px += a.b8_c2px; t.b16_c2 += a.b16_c2; t.b16_c2px += a.b16_c2px;
    auto pc = [] (long x, long n) { return n ? 100.0 * (double)x / (double)n : 0.0; };
    std::fprintf(stderr, "[b1shclass] capture frame %ld %s: atlas %ux%u, rect %d,%d..%d,%d, drawn %ld px: pre-test %.1f %% S1 %.1f %% uniform-missed %.1f %% penumbra %.1f %% off-map %.1f %%\n",
                 cap_frame, kname[kind], AW, AH, x0, y0, x1, y1, a.drawn, pc(a.pre, a.drawn), pc(a.c0, a.drawn), pc(a.c1lit + a.c1sh, a.drawn), pc(a.c2, a.drawn), pc(a.off, a.drawn));
    for (int k = 0; k < 3; k++) { const Acc &q = acc[k]; if (!q.caps) continue; long full = q.c1lit + q.c1sh + q.c2 + q.off;
      std::fprintf(stderr, "[b1shclass] total %s: R1 approx (geometric normal, N.L < -0.05): %.1f %% of the drawn non-pre-test px with a normal, %.1f %% of the full-path px\n", kname[k], pc(q.nl_back, q.nl_known), pc(q.nl_back_full, full));
      std::fprintf(stderr, "[b1shclass] total %s (v2): settled by S1 only thanks to the wrap shift %.1f %% of drawn | off-map split: straddle uniform lit %.1f %% shadowed %.1f %% penumbra %.1f %%, far/NaN %.1f %% (of the full path)\n",
                   kname[k], pc(q.sh_c0, q.drawn), pc(q.st_lit, full), pc(q.st_sh, full), pc(q.st_pen, full), pc(q.far, full));
      if (k == 0 && q.nlx_caps) std::fprintf(stderr, "[b1shclass] total %s EXACT N.L (sun light G-buffer normal, %ld captures, %ld px): zero (d <= -1e-3) %.1f %% | edge %.1f %% | lit %.1f %% || of the full-path px (%ld): zero %.1f %% || cos(L, cascade axis) %.8f\n",
                   kname[k], q.nlx_caps, q.nlx_px, pc(q.nlx_zero, q.nlx_px), pc(q.nlx_edge, q.nlx_px), pc(q.nlx_lit, q.nlx_px), q.nlx_full, pc(q.nlx_full_zero, q.nlx_full), q.axis_dot);
      std::fprintf(stderr, "[b1shclass] total %s: %ld captures, drawn %ld px | pre-test %.1f %% | S1-settled %.1f %% | full path %.1f %% (of it: uniform window lit %.1f %% shadowed %.1f %%, penumbra %.1f %%, off-map %.1f %%) | "
                   "waves 8x8: lanes in full-path waves %ld, full-path px %ld (dense %.1f %%), penumbra-only %ld of %ld (dense %.1f %%) | 16x8: %ld / %ld (%.1f %%), %ld / %ld (%.1f %%) | skips: views %ld fmt %ld cb %ld stencil %ld kind %ld\n",
                   kname[k], q.caps, q.drawn, pc(q.pre, q.drawn), pc(q.c0, q.drawn), pc(full, q.drawn), pc(q.c1lit, full), pc(q.c1sh, full), pc(q.c2, full), pc(q.off, full),
                   q.b8_full, q.b8_px, pc(q.b8_px, q.b8_full), q.b8_c2px, q.b8_c2, pc(q.b8_c2px, q.b8_c2), q.b16_full, q.b16_px, pc(q.b16_px, q.b16_full), q.b16_c2px, q.b16_c2, pc(q.b16_c2px, q.b16_c2),
                   n_skip[0], n_skip[1], n_skip[2], n_skip[3], n_skip[4]); } }
  static void frame() {
    if (!mode()) return;
    if (pending) { pending = false; ctx_flush(); cs_sync(); g_dev->waitForIdle(); analyse(); lcap = false;
      if (census_frames % 4 == 0 || census_frames <= 2) { std::string cl; for (auto &e : census) { char b[64]; std::snprintf(b, sizeof b, " %016llx%s x%ld", e.first, e.first == kSunLight ? "(sun light)" : "", e.second); cl += b; }
        std::fprintf(stderr, "[b1shclass] attenuation RT readers after the captured projection (%ld capture frames):%s\n", census_frames, cl.empty() ? " none" : cl.c_str()); } }
    n_prev = n_now; n_now = 0; f_frames++;
    if (f_frames >= start() && f_frames % every() == 0 && n_prev > 0) target = rot++ % n_prev; else target = -1; }
}
}
