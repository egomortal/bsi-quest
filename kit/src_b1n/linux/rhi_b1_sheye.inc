/* b123 sheye: SHARED-EYE SHADOW CHECK (diagnostic only; BSI_B1_SHEYE=2, default 0 = no work, nothing changes).
   Question (user, 2 Nov): the right eye's shadow projection costs ~6.5-7.8 ms (b122 SHEYEPROBE ceiling). The projection kernels
   (TXShadowProjection spot 2e583db5 / sun cd4a8dad) compute the attenuation only from the pixel's position in light space
   (scene depth x the camera-relative ray -> cb0 rows 0..3 -> PCSS over the shadow map, pow 1/2.2); there is no screen-position
   dither. So a right-eye pixel that sees the same surface point as a left-eye pixel would get the same value up to the
   sub-pixel offset between the two pixel grids. How many right-eye pixels would come out BIT-IDENTICAL if copied from the left?
   Every BSI_B1_SHEYE_EVERY (300) frames, the k-th render pass that contains shadow projection quads of BOTH eyes (k rotates)
   is copied at its end (colour target 0 = the light attenuation buffer, and its depth-stencil = the scene depth) to host
   buffers; at frame end (one GPU wait) every right-eye pixel p is mapped to the left eye: scene depth -> clip -> world with the
   right eye's stencil-volume constants (clip = (v + PVT) * M, cb1 rows 0..3 + row 4, the convention of vs_shadow_eye_consts)
   -> clip of the left eye -> pixel q (nearest). 'Same surface' = the world point rebuilt from the LEFT depth at q lies within
   1 % (strict: 0.2 %) of the view distance of p's point. Then the projection's written channels (blend write mask of the quads)
   at p and q are compared. [b1sheye] reports, cumulatively: right pixels, far / outside the left eye / other surface, same
   surface; of those the 'touched' ones (either side not fully lit 0xff): exact, off by 1, worse, max diff; split by 'flat' q
   (the 3x3 around q all equal) and by strict surface match. The first passes' draw order (L/R per projection quad) is logged.
   Hooks: sb87 (volumes: per-eye constants; projection quads + pp76 right-eye replays: eye + write mask), b1dx_c_rts, frame end. */
namespace {
namespace sheye {
  static int mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHEYE"); v = e && *e ? std::atoi(e) : 0; if (v < 0) v = 0; } return v; }
  static int every() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHEYE_EVERY"); v = e && *e ? std::atoi(e) : 300; if (v < 2) v = 2; } return v; }
  struct Eye { double m[16]; double pvt[3]; double vp[6]; bool have = false, pvton = false; };
  struct P { uint32_t rtv = 0, rt_res = 0, ds_res = 0; int nl = 0, nr = 0; uint32_t wmask = 0; std::string order; Eye e[2]; };
  static P cur; static bool open;
  static long f_frames, f_logged; static int nq_now, nq_prev, target = -1; static long rot;
  /* the pending copy */
  static Rc<DxvkBuffer> cbuf, dbuf; static VkDeviceSize csz, dsz; static bool pending; static P snap; static VkFormat cfmt, dfmt; static uint32_t W, H; static long snap_frame;
  /* totals */
  struct Acc { long cycles = 0, right = 0, far = 0, outside = 0, other = 0, same = 0, bothlit = 0, touched = 0, exact = 0, off1 = 0, worse = 0, flat = 0, flat_exact = 0, nonflat = 0, nonflat_exact = 0, strict = 0, strict_exact = 0; int maxd = 0; };
  static Acc acc; static long n_skip_fmt, n_skip_eye, n_pass_both;

  static void on_vol(const float *cb, bool pvt, const float *vp, bool right) {
    if (mode() != 2 || !open || !cb || !vp) return; Eye &e = cur.e[right ? 1 : 0];
    for (int i = 0; i < 16; i++) e.m[i] = cb[i]; for (int i = 0; i < 3; i++) e.pvt[i] = pvt ? cb[16 + i] : 0.0; for (int i = 0; i < 6; i++) e.vp[i] = vp[i]; e.pvton = pvt; e.have = true; }
  static void on_proj(bool right, uint32_t wmask) {
    if (mode() != 2 || !open) return; (right ? cur.nr : cur.nl)++; cur.wmask |= wmask; if (cur.order.size() < 48) cur.order += right ? 'R' : 'L'; }
  static uint32_t fmt_bytes(VkFormat f) {
    switch (f) { case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32_SFLOAT: return 4;
      case VK_FORMAT_R16G16B16A16_SFLOAT: return 8; default: return 0; } }
  static bool is8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; }
  static void copy_pass(const P &p) {
    Obj *co = robj(p.rt_res), *dobj = robj(p.ds_res); if (!co || co->kind != 2 || co->img == nullptr || !dobj || dobj->kind != 2 || dobj->img == nullptr) return;
    VkExtent3D ce = co->img->mipLevelExtent(0), de = dobj->img->mipLevelExtent(0);
    uint32_t cb = fmt_bytes(co->fmt); bool d16 = dobj->fmt == VK_FORMAT_D16_UNORM || dobj->fmt == VK_FORMAT_D16_UNORM_S8_UINT;
    if (!cb || ce.width != de.width || ce.height != de.height) { if (n_skip_fmt++ < 4) std::fprintf(stderr, "[b1sheye] pass skipped: colour format %d (%u B) %ux%u, depth format %d %ux%u\n", (int)co->fmt, cb, ce.width, ce.height, (int)dobj->fmt, de.width, de.height); return; }
    VkDeviceSize cs = (VkDeviceSize)ce.width * ce.height * cb, ds = (VkDeviceSize)de.width * de.height * (d16 ? 2u : 4u);
    auto mk = [] (Rc<DxvkBuffer> &b, VkDeviceSize &have, VkDeviceSize sz, const char *nm) { if (b == nullptr || have != sz) { DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = nm; b = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); have = sz; } };
    mk(cbuf, csz, cs, "B1 sheye colour"); mk(dbuf, dsz, ds, "B1 sheye depth");
    VkImageSubresourceLayers cl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u }, dl = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
    emit([rb = cbuf, fmt = co->fmt, img = co->img, cl, ce, db = dbuf, dfm = dobj->fmt, dimg = dobj->img, dl, de] (DxvkContext* c) {
      c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, cl, VkOffset3D{ 0, 0, 0 }, ce); c->copyImageToBuffer(db, 0, 0, 0, dfm, dimg, dl, VkOffset3D{ 0, 0, 0 }, de); });
    snap = p; cfmt = co->fmt; dfmt = dobj->fmt; W = ce.width; H = ce.height; pending = true; snap_frame = f_frames; }
  static void close() {
    if (!open) return; open = false;
    if (cur.nl && cur.nr) { n_pass_both++; int idx = nq_now++;
      if (f_logged < 24 && f_frames >= (long)every()) { f_logged++; std::fprintf(stderr, "[b1sheye] order frame %ld pass %d: rt res %u ds res %u, quads L %d R %d: %s%s (write mask %x, constants L %d R %d)\n", f_frames, idx, cur.rt_res, cur.ds_res, cur.nl, cur.nr, cur.order.c_str(), cur.nl + cur.nr > 48 ? "..." : "", cur.wmask, (int)cur.e[0].have, (int)cur.e[1].have); }
      if (idx == target && !pending) { target = -1; if (!cur.e[0].have || !cur.e[1].have) n_skip_eye++; else copy_pass(cur); } }
    cur = P(); }
  static void on_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv) {
    if (mode() != 2) return; close();
    if (!n || !rtvs || !rtvs[0]) return; View *rv = g_views.get(rtvs[0]), *dv = g_views.get(dsv);
    if (!rv || rv->type != 2 || !dv || dv->type != 3) return;
    cur = P(); cur.rtv = rtvs[0]; cur.rt_res = rv->res; cur.ds_res = dv->res; open = true; }
  static bool inv4(const double *m, double *o) {   /* Gauss-Jordan, row-major 4x4 */
    double a[4][8]; for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) { int p = c; for (int r = c + 1; r < 4; r++) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r; if (!(std::fabs(a[p][c]) > 1e-30)) return false;
      if (p != c) for (int j = 0; j < 8; j++) std::swap(a[p][j], a[c][j]); double d = a[c][c]; for (int j = 0; j < 8; j++) a[c][j] /= d;
      for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; if (f != 0.0) for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; } }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4]; return true; }
  /* row vector times matrix: o = v * M */
  static void vm(const double *v, const double *M, double *o) { for (int k = 0; k < 4; k++) o[k] = v[0] * M[0 * 4 + k] + v[1] * M[1 * 4 + k] + v[2] * M[2 * 4 + k] + v[3] * M[3 * 4 + k]; }
  static double depth_at(const uint8_t *d, uint32_t x, uint32_t y) {
    if (dfmt == VK_FORMAT_D16_UNORM || dfmt == VK_FORMAT_D16_UNORM_S8_UINT) { uint16_t v; std::memcpy(&v, d + ((size_t)y * W + x) * 2, 2); return v / 65535.0; }
    uint32_t v; std::memcpy(&v, d + ((size_t)y * W + x) * 4, 4);
    if (dfmt == VK_FORMAT_D32_SFLOAT || dfmt == VK_FORMAT_D32_SFLOAT_S8_UINT) { float f; std::memcpy(&f, &v, 4); return f; }
    return (v & 0xffffffu) / 16777215.0; }   /* X8_D24 / D24S8 */
  /* pixel (x + 0.5, y + 0.5) with device depth z of eye e -> world (false: far / degenerate) */
  static bool unproject(const Eye &e, const double *inv, double x, double y, double z, double *w) {
    double nx = (x - e.vp[0]) / e.vp[2] * 2.0 - 1.0, ny = 1.0 - (y - e.vp[1]) / e.vp[3] * 2.0, dz = e.vp[5] - e.vp[4]; if (dz == 0.0) return false;
    double c[4] = { nx, ny, (z - e.vp[4]) / dz, 1.0 }, p[4]; vm(c, inv, p); if (!(std::fabs(p[3]) > 1e-12)) return false;
    for (int k = 0; k < 3; k++) w[k] = p[k] / p[3] - e.pvt[k]; return std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]); }
  static void analyse() {
    const uint8_t *C = (const uint8_t *)cbuf->mapPtr(0), *D = (const uint8_t *)dbuf->mapPtr(0); uint32_t cb = fmt_bytes(cfmt); const Eye &L = snap.e[0], &R = snap.e[1];
    double iL[16], iR[16]; if (!inv4(L.m, iL) || !inv4(R.m, iR)) { std::fprintf(stderr, "[b1sheye] degenerate eye matrix (frame %ld)\n", snap_frame); return; }
    /* channel byte mask from the quads' colour write mask (bit 0 R .. bit 3 A); BGRA swaps R and B */
    uint8_t bm[8] = { 0 }; bool bgra = cfmt == VK_FORMAT_B8G8R8A8_UNORM || cfmt == VK_FORMAT_B8G8R8A8_SRGB; uint32_t wm = snap.wmask ? snap.wmask : 0xfu;
    if (is8(cfmt)) { for (int ch = 0; ch < 4; ch++) if (wm >> ch & 1u) { int b = ch; if (bgra && ch != 3) b = 2 - ch; bm[b] = 0xff; } } else for (uint32_t b = 0; b < cb; b++) bm[b] = 0xff;
    auto val = [&] (uint32_t x, uint32_t y, uint8_t *o) { const uint8_t *s = C + ((size_t)y * W + x) * cb; for (uint32_t b = 0; b < cb; b++) o[b] = s[b] & bm[b]; };
    auto lit = [&] (const uint8_t *v) { if (!is8(cfmt)) return false; for (uint32_t b = 0; b < cb; b++) if (bm[b] && v[b] != 0xff) return false; return true; };
    int lx0 = std::max(0, (int)std::ceil(L.vp[0])), lx1 = std::min((int)W, (int)std::floor(L.vp[0] + L.vp[2])), ly0 = std::max(0, (int)std::ceil(L.vp[1])), ly1 = std::min((int)H, (int)std::floor(L.vp[1] + L.vp[3]));
    int rx0 = std::max(0, (int)std::ceil(R.vp[0])), rx1 = std::min((int)W, (int)std::floor(R.vp[0] + R.vp[2])), ry0 = std::max(0, (int)std::ceil(R.vp[1])), ry1 = std::min((int)H, (int)std::floor(R.vp[1] + R.vp[3]));
    Acc a; a.cycles = 1;
    for (int y = ry0; y < ry1; y++) for (int x = rx0; x < rx1; x++) { a.right++;
      double wp[3]; if (!unproject(R, iR, x + 0.5, y + 0.5, depth_at(D, x, y), wp)) { a.far++; continue; }
      double v[4] = { wp[0] + L.pvt[0], wp[1] + L.pvt[1], wp[2] + L.pvt[2], 1.0 }, c[4]; vm(v, L.m, c);
      if (!(c[3] > 1e-9)) { a.outside++; continue; }
      double qx = L.vp[0] + (c[0] / c[3] + 1.0) * 0.5 * L.vp[2], qy = L.vp[1] + (1.0 - c[1] / c[3]) * 0.5 * L.vp[3];
      int ix = (int)std::floor(qx), iy = (int)std::floor(qy); if (!(qx == qx) || ix < lx0 || ix >= lx1 || iy < ly0 || iy >= ly1) { a.outside++; continue; }
      double wq[3]; if (!unproject(L, iL, ix + 0.5, iy + 0.5, depth_at(D, ix, iy), wq)) { a.other++; continue; }
      double dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), dd = std::sqrt((wq[0] - wp[0]) * (wq[0] - wp[0]) + (wq[1] - wp[1]) * (wq[1] - wp[1]) + (wq[2] - wp[2]) * (wq[2] - wp[2]));
      if (!(dd <= 0.01 * dist)) { a.other++; continue; }
      a.same++; uint8_t pv[8], qv[8]; val(x, y, pv); val(ix, iy, qv);
      if (lit(pv) && lit(qv)) { a.bothlit++; continue; }
      a.touched++; int md = 0; bool ex = true; for (uint32_t b = 0; b < cb; b++) { if (pv[b] != qv[b]) ex = false; md = std::max(md, std::abs((int)pv[b] - (int)qv[b])); }
      if (!is8(cfmt) && !ex) md = 255;
      if (ex) a.exact++; else if (md <= 1) a.off1++; else a.worse++; a.maxd = std::max(a.maxd, md);
      bool flat = ix > lx0 && ix + 1 < lx1 && iy > ly0 && iy + 1 < ly1;
      for (int j = -1; j <= 1 && flat; j++) for (int i = -1; i <= 1 && flat; i++) { uint8_t nv[8]; val(ix + i, iy + j, nv); if (std::memcmp(nv, qv, cb)) flat = false; }
      if (flat) { a.flat++; if (ex) a.flat_exact++; } else { a.nonflat++; if (ex) a.nonflat_exact++; }
      if (dd <= 0.002 * dist) { a.strict++; if (ex) a.strict_exact++; } }
    acc.cycles += a.cycles; acc.right += a.right; acc.far += a.far; acc.outside += a.outside; acc.other += a.other; acc.same += a.same; acc.bothlit += a.bothlit; acc.touched += a.touched;
    acc.exact += a.exact; acc.off1 += a.off1; acc.worse += a.worse; acc.flat += a.flat; acc.flat_exact += a.flat_exact; acc.nonflat += a.nonflat; acc.nonflat_exact += a.nonflat_exact;
    acc.strict += a.strict; acc.strict_exact += a.strict_exact; acc.maxd = std::max(acc.maxd, a.maxd);
    auto pc = [] (long x, long n) { return n ? 100.0 * (double)x / (double)n : 0.0; };
    std::fprintf(stderr, "[b1sheye] cycle frame %ld (rt res %u fmt %d, depth fmt %d, %ux%u, quads L %d R %d, mask %x): right %ld, same surface %.1f %%, touched %ld: exact %.2f %% off-by-1 %.2f %% worse %.2f %% (max %d); flat q %ld exact %.2f %%, non-flat %ld exact %.2f %%\n",
                 snap_frame, snap.rt_res, (int)cfmt, (int)dfmt, W, H, snap.nl, snap.nr, snap.wmask, a.right, pc(a.same, a.right), a.touched, pc(a.exact, a.touched), pc(a.off1, a.touched), pc(a.worse, a.touched), a.maxd, a.flat, pc(a.flat_exact, a.flat), a.nonflat, pc(a.nonflat_exact, a.nonflat));
    std::fprintf(stderr, "[b1sheye] total %ld cycles: right px %ld: far %.1f %% outside left %.1f %% other surface %.1f %% same %.1f %% (both lit %.1f %%); touched %.1f %% of right: exact %.2f %% off-by-1 %.2f %% worse %.2f %% max %d | flat %.1f %% of touched, exact %.3f %% | non-flat exact %.2f %% | strict-surface exact %.2f %% | passes with both eyes %ld, skipped (constants %ld, format %ld)\n",
                 acc.cycles, acc.right, pc(acc.far, acc.right), pc(acc.outside, acc.right), pc(acc.other, acc.right), pc(acc.same, acc.right), pc(acc.bothlit, acc.right), pc(acc.touched, acc.right),
                 pc(acc.exact, acc.touched), pc(acc.off1, acc.touched), pc(acc.worse, acc.touched), acc.maxd, pc(acc.flat, acc.touched), pc(acc.flat_exact, acc.flat), pc(acc.nonflat_exact, acc.nonflat), pc(acc.strict_exact, acc.strict), n_pass_both, n_skip_eye, n_skip_fmt); }
  static void frame() {
    if (mode() != 2) return; close();
    if (pending) { pending = false; ctx_flush(); cs_sync(); g_dev->waitForIdle(); analyse(); }
    nq_prev = nq_now; nq_now = 0; f_frames++;
    if (f_frames % every() == 0 && nq_prev > 0) target = (int)(rot++ % nq_prev); else target = -1; }
}
}
