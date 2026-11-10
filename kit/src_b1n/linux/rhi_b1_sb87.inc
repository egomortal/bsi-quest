  /* ---- sb87: scissor the shadow projection quads to the screen bbox of their stencil volumes (BSI_B1_SHBBOX; g4_notes/SHBBOX.md) ----
     UE3 draws every shadow (cascade or per-object) as [stencil clear] -> stencil volume(s) (no PS, VS vs.3863988d: clip = v * TVP,
     stencil IncWrap/DecWrap on z-fail) -> the projection quad (TXShadowProjection* PS, stencil test NotEqual 0, whole eye
     column). Outside the rasterised area of the volumes the stencil still holds the cleared value 0, so the quad's stencil
     test fails there and it writes nothing: scissoring the quad to a conservative bbox of the volumes changes no pixel.
     Tracking: per depth-stencil resource a set of rects outside which the stencil is known to be 0 ("the region"):
       reset (whole, value 0) by a stencil clear of the image; grown by every draw that can leave a non-zero stencil
       (stencil test on, write mask != 0, an op other than KEEP / ZERO / REPLACE-with-(ref & wmask) == 0): the bbox of a
       recognised volume draw (projection VS, no PS, position-only float2/3/4 layout, CPU copy of the vertex / index bytes from
       our own write path, every clip.w > eps) or else "anywhere" (full); shrunk by the game's stencil clear quads (OneColor
       PS, all ops zeroing, write mask 0xff, cull none, two triangles sharing a diagonal of the +-1 square: the whole
       viewport / scissor rect becomes 0; BSI_B1_SHBBOX_CLEARQUAD=0 turns that off).
     At a projection quad (stencil write mask 0, stencil test false for the value 0 on both faces, s2 did not drop the
     test) the scissor becomes the game's scissor (after es86's eye-column cap) ∩ bbox(region rects ∩ viewport), rounded
     outward by BSI_B1_SHBBOX_MARGIN (default 4, min 2) px. pp76's right-eye replay of the same draws (sh_post) goes through
     replay_pre / replay_post with the right view's constants and viewport.
     Depth bounds (BSI_B1_SHDB, default 1): the same volume vertices give the volume's depth range (NDC z through the draw's
     viewport depth mapping): a pixel's stencil is non-zero only where the scene depth lies between two volume faces (z-fail
     counting: depth test on, a less/greater compare, writes on depth fail only), so its depth is inside that range; the
     projection quad gets a Vulkan depth-bounds test [zmin - eps, zmax + eps] (independent of its depth test enable; the
     device must report depthBounds), restored to the default (0, 1) after the quad. Dropped (counted) when a volume's
     depth-stencil state is not that scheme, when depth was written into the image since the volumes, or when w <= eps.
     The game never sets depth bounds through this seam (no SetDepthBoundsTest path), so the previous state is (0, 1).
     Modes: BSI_B1_SHBBOX=0 off, 1 on (default), 3 NOT EXACT measurement (every projection quad scissored to 2x2 px), 4 NOT EXACT ceiling probe (b92: stencil volumes not drawn, projection quads without stencil test), 2 CHECK: every BSI_B1_SHBBOX_CHECK_EVERY-th frame (30) the first scissored
     quad is also drawn unscissored into a copy of its target and both are read back and compared ([b1sb] check: ...).
     VS convention: the one of vs_shadow_eye_consts (vr_stereo.c): clip = (x, y, z, w) * cb1 rows 0..3 (row vector, D3D).
     Whether the VS adds PreViewTranslation (cb1[4].xyz) first is read from its DXBC at creation (a cb1[4] operand in the
     code) and can be forced with BSI_B1_SHBBOX_PVT=0/1. A VS that does not read cb1[0..3] is never treated as the volume VS. */
  bool sb87_lrz_mod();   /* defined after namespace s2 (rhi_b1_dxvk.cpp): s2 dropped the stencil test of the current state */
  namespace sb87 {
    static int env_i(const char *n, int d) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : d; }
    static int mode() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_SHBBOX", 1); if (v < 0) v = 0; } return v; }
    static int margin() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_SHBBOX_MARGIN", 4); if (v < 2) v = 2; } return v; }
    static int pvt_env() { static int v = -2; if (v == -2) { v = env_i("BSI_B1_SHBBOX_PVT", -1); if (v > 1) v = 1; } return v; }
    static int shda() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHDEPTHALWAYS", 1) > 0; return v; }   /* b97: projection quads get depth test ALWAYS (no write) so the early depth/stencil stage runs */
    static int markers() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHMARK", 0) > 0; return v; }   /* b107: default OFF (GPU gain ~0, render-thread cost) */   /* b99: bound the subject markers (material VS/PS stencil writes) by their projected vertices */
    static int shclr() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHCLR", 0) > 0; return v; }   /* b112: proven stencil-clear quads become stencil clears (EXACT) */
    static int clearquad() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHBBOX_CLEARQUAD", 1); return v; }
    static int shdb() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHDB", 1); return v; }
    static int check_every() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_SHBBOX_CHECK_EVERY", 30); if (v < 1) v = 1; } return v; }

    static void tile_released(uint32_t res);   /* b102 */
    struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; double zlo = 0, zhi = 1; bool zok = false;   /* z: depth range of the volume that made it (zok: usable for depth bounds) */
                  bool empty() const { return x1 <= x0 || y1 <= y0; }
                  double area() const { return empty() ? 0.0 : (double)(x1 - x0) * (double)(y1 - y0); } };
    static Rect isect(const Rect &a, const Rect &b) { Rect r = { std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1) }; if (r.empty()) r = Rect(); return r; }
    static Rect runion(const Rect &a, const Rect &b) { if (a.empty()) return b; if (b.empty()) return a;
      return Rect { std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.zlo, b.zlo), std::max(a.zhi, b.zhi), a.zok && b.zok }; }
    static bool inside(const Rect &a, const Rect &b) { return a.empty() || (!b.empty() && a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1); }
    static int clampi(double v) { return v < -1e9 ? -1000000000 : v > 1e9 ? 1000000000 : (int)v; }

    /* ---- the region: where a non-zero stencil may be, per depth-stencil resource id ----
       full = false: only inside the rects r. full = true: anywhere except inside one of the `cleared` rects (clear quads /
       partial clears since the last unknown write: one per eye column, up to 8), plus the rects r. An id without an entry
       is full with nothing cleared. */
    struct Reg { bool full = true; std::vector<Rect> cleared; std::vector<Rect> r; };   /* each rect carries the depth range of its volume (zok false after a depth write) */
    static std::unordered_map<uint32_t, Reg> regs;
    enum { P_CLEARVAL, P_UNKNOWN, P_NODATA, P_UNBOUNDED, P_COPY, P_PROJW, P_LAYOUT, P_BADVS, P_N };
    static const char *pname[P_N] = { "clear value != 0", "unknown stencil write", "no vertex / constant data", "clip.w <= eps", "copy / upload into the image", "projection writes stencil", "layout not position-only", "VS does not read cb1 rows 0..3" };
    static long n_db, n_nodb, n_zlost, n_unb; static int n_dq, n_du, n_dw; static double zfrac_sum; static long n_poison[P_N], n_seq, n_vol, n_proj, n_proj_w, n_proj_lrz, n_proj_pass0, n_proj_full, n_scis, n_rep_vol, n_rep_proj, n_rep_proj_not, n_rep_clear, n_clearq, n_clearq_no, n_reset, n_partial;
    static double area_sum; static long area_hist[101], kind_hist[2][5]; static int n_logged;
    static Reg &reg(uint32_t res) { return regs[res]; }
    static void poison(uint32_t res, int why) { if (!res || !mode()) return; auto it = regs.find(res); if (it == regs.end()) return;
      Reg &g = it->second; if (g.full && g.cleared.empty() && g.r.empty()) return; g.full = true; g.cleared.clear(); g.r.clear(); n_poison[why]++; }
    static long n_zl[3]; static int n_dz;
    static void zpoison(uint32_t res, int why, const char *what) {   /* why: 0 depth write, 1 volume not z-fail counting / biased, 2 depth-only clear */
      if (!res) return; auto it = regs.find(res); if (it == regs.end()) return; bool any = false; for (auto &x : it->second.r) if (x.zok) { x.zok = false; any = true; }
      if (any) { n_zlost++; n_zl[why]++; if (n_dz < 8) { n_dz++; std::fprintf(stderr, "[b1sb] depth range lost %d: %s\n", n_dz, what); } } }   /* depth written: the volumes' depth ranges say nothing now */
    static void reset(uint32_t res) { Reg &g = reg(res); g.full = false; g.cleared.clear(); g.r.clear(); n_reset++; }
    static bool in_cleared(const Reg &g, const Rect &e) { for (auto &c : g.cleared) if (inside(e, c)) return true; return false; }
    static double zmax_vis; static int n_vis;   /* project(): max viewport depth over the vertices with w > eps, and their count (for the whole-rect fallback) */
    static void add_rect(uint32_t res, Rect b, double zlo, double zhi, bool zok);
    /* a volume with some vertices at / behind the eye: the whole rect; its depth range = [near plane, max depth of the visible vertices]: the clipped
       part lies on the near plane (z_ndc 0), z/w along every clipped edge / face is bounded by its end points (visible vertices or near / far crossings) */
    static void add_rect_unb(uint32_t res, const Rect &eo, const float *vp) {
      double lo = std::min(vp[4], vp[5]), hi = std::max(vp[4], vp[5]); if (n_vis > 0 && zmax_vis > -1e299) add_rect(res, eo, lo, std::max(lo, std::min(hi, zmax_vis)), true); else add_rect(res, eo, 0, 1, false); }
    static void add_rect(uint32_t res, Rect b, double zlo, double zhi, bool zok) { Reg &g = reg(res); if (b.empty()) return; if (g.r.empty()) n_seq++;
      b.zlo = zlo; b.zhi = zhi; b.zok = zok; g.r.push_back(b);
      if (g.r.size() > 16) { Rect u; for (auto &x : g.r) u = runion(u, x); g.r.clear(); g.r.push_back(u); } }
    static void shrink(uint32_t res, const Rect &cl) { Reg &g = reg(res); if (cl.empty()) return;
      if (g.full) { std::vector<Rect> k; for (auto &c : g.cleared) if (!inside(c, cl)) k.push_back(c); k.push_back(cl); if (k.size() > 8) k.erase(k.begin()); g.cleared.swap(k); }   /* rects (a union of two is no rect): the left and the right column's */
      std::vector<Rect> k; for (auto &x : g.r) if (!inside(x, cl)) k.push_back(x); g.r.swap(k); }
    /* the smallest rect outside which the quad with effective rect eff (viewport ∩ scissor) finds stencil 0; false = none (full) */
    static bool quad_rect(uint32_t res, const Rect &eff, Rect &out) { auto it = regs.find(res); if (it == regs.end()) return false; Reg &g = it->second;
      if (g.full && !in_cleared(g, eff)) return false;
      Rect u; bool first = true; for (auto &x : g.r) { Rect i = isect(x, eff); if (i.empty()) continue; i.zlo = x.zlo; i.zhi = x.zhi; i.zok = x.zok; u = first ? i : runion(u, i); first = false; }
      if (first) { u = Rect(); u.zok = true; u.zlo = 1.0; u.zhi = 0.0; }   /* nothing: an empty rect, an empty depth range */
      out = u; return true; }

    /* ---- vertex shaders: kind (the projection VS by hash), the cb1 registers its code reads (DXBC scan at creation) ---- */
    struct VsI { bool scanned = false, named = false; int kind = 0; uint8_t cb1 = 0; int pos_reg = -1; int decA = -1, decB = -1; int idx_reg = -1, w_reg = -1; bool skin = false; int nin = -1; };   /* nin: ISGN element count (b102: a projection quad VS must read only POSITION) */   /* b101 skin: UE3 GPU-skin VS (allow-list BSI_B1_SHBBOX_VS3; bones = cb3 rows 3i..3i+2, dp4 with (p, 1), then cb0 rows 0..3, cb1 rows 0..3) */   /* b99: decA/decB >= 0: a UE3 material VS whose clip position is ((v.xyz * cb0[decA].xyz + cb0[decB].xyz, v.w) * cb0 rows 0..3) * cb1 rows 0..3 and nothing else (DXBC pattern) */   /* pos_reg: input register of semantic POSITION (ISGN), -1 unknown */   /* kind 1 = projection VS, -1 other */
    static std::mutex smu; static std::unordered_map<uint32_t, VsI> vsi;
    static uint32_t r32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    static uint8_t scan_cb1(const void *code, uint32_t len) {   /* bit r: an operand cb1[r] (r < 8, immediate index) in an instruction (not the dcl) */
      const uint8_t *p = (const uint8_t *)code; uint8_t m = 0; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return m;
      uint32_t nch = r32(p + 32); if (nch > 64) return m;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 8 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x52444853u && tag != 0x58454853u) continue;   /* SHDR / SHEX */
        if ((uint64_t)off + 8 + sz > len || sz < 8) continue; const uint32_t *t = (const uint32_t *)(p + off + 8); uint32_t n = std::min<uint32_t>(sz / 4, t[1]);
        uint32_t i = 2; while (i < n) { uint32_t op = t[i] & 0x7ffu, l = (t[i] >> 24) & 0x7fu; if (op == 53u) l = i + 1 < n ? t[i + 1] : 0;   /* custom data: its own length */
          if (!l) break; uint32_t end = std::min<uint32_t>(i + l, n);
          if (op != 89u && op != 53u) for (uint32_t j = i + 1; j < end; j++) { uint32_t u = t[j];   /* 89 = dcl_constantbuffer */
            if (((u >> 12) & 0xffu) != 8u || ((u >> 20) & 3u) != 2u || ((u >> 22) & 0x3fu) != 0u) continue;   /* cb operand, 2 immediate indices */
            uint32_t k = j + 1; if (u & 0x80000000u) k++; if (k + 1 < end && t[k] == 1u && t[k + 1] < 8u) m |= (uint8_t)(1u << t[k + 1]); }
          i = end; } }
      return m; }
    static int scan_pos(const void *code, uint32_t len) {   /* ISGN: the register of semantic "POSITION" index 0 (or "SV_Position"), -1 if none */
      const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return -1;
      uint32_t nch = r32(p + 32); if (nch > 64) return -1;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 16 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x4e475349u && tag != 0x31475349u) continue;   /* ISGN / ISG1 */
        if ((uint64_t)off + 8 + sz > len) continue; const uint8_t *d = p + off + 8; uint32_t n = r32(d), es = tag == 0x31475349u ? 32u : 24u; if (n > 32 || 8 + (uint64_t)n * es > sz) continue;
        for (uint32_t i = 0; i < n; i++) { const uint8_t *e = d + 8 + i * es; uint32_t no = r32(e), si = r32(e + 4), rg = r32(e + 16); if (no >= sz || si != 0) continue;
          const char *nm = (const char *)d + no; size_t ml = strnlen(nm, sz - no); if ((ml == 8 && !strncasecmp(nm, "POSITION", 8)) || (ml == 11 && !strncasecmp(nm, "SV_Position", 11))) return (int)rg; }
        return -1; }
      return -1; }
    /* b99: subject markers. UE3 draws the shadow subject itself into the stencil (material VS + PS, REPLACE ref 1 on the depth test); its screen
       footprint bounds those writes. The material VS is accepted only when its SV_Position dataflow is exactly the fixed UE3 SM5 sequence:
         mad rD.xyz, v<pos>.xyzx, cb0[A].xyzx, cb0[B].xyzx          (MeshExtension / MeshOrigin decode)
         mul rE, rD.yyyy, cb0[1]; mad rE, cb0[0], rD.xxxx, rE; mad rF, cb0[2], rD.zzzz, rE; mad rF, cb0[3], v<pos>.wwww, rF   (LocalToWorld)
         mul rG, rF.yyyy, cb1[1]; mad rG, cb1[0], rF.xxxx, rG; mad rG, cb1[2], rF.zzzz, rG; mad rG, cb1[3], rF.wwww, rG       (ViewProjection)
         ... mov o<SV_Position>, rG  (rG not written in between)
       Anything else (world position offset, skinning, other decodes) is not matched and the region stays poisoned as before. */
    struct Opd { uint32_t type = 0xff, ncomp = 0, sel = 0, swz = 0, ndim = 0, idx[3] = { 0, 0, 0 }; };
    static bool opd(const uint32_t *t, uint32_t &j, uint32_t end, Opd &o) {
      if (j >= end) return false; uint32_t u = t[j++]; o.ncomp = u & 3u; o.sel = (u >> 2) & 3u; o.swz = (u >> 4) & 0xffu; o.type = (u >> 12) & 0xffu; o.ndim = (u >> 20) & 3u;
      uint32_t rep[3] = { (u >> 22) & 7u, (u >> 25) & 7u, (u >> 28) & 7u }; uint32_t ext = u >> 31;
      while (ext) { if (j >= end) return false; ext = t[j++] >> 31; }
      for (uint32_t k = 0; k < o.ndim; k++) { if (rep[k] == 0u) { if (j >= end) return false; o.idx[k] = t[j++]; } else if (rep[k] == 1u) { if (j + 1 >= end) return false; o.idx[k] = t[j]; j += 2; } else return false; }
      if (o.type == 4u) j += o.ncomp == 2u ? 4u : 1u; else if (o.type == 5u) j += o.ncomp == 2u ? 8u : 2u;   /* immediates */
      return j <= end; }
    struct Ins { uint32_t op = 0; std::vector<Opd> o; uint32_t siv = 0; };
    static bool is_cb(const Opd &o, uint32_t buf, uint32_t reg) { return o.type == 8u && o.ndim == 2u && o.idx[0] == buf && o.idx[1] == reg; }
    static bool is_tmp(const Opd &o, uint32_t r) { return o.type == 0u && o.ndim == 1u && o.idx[0] == r; }
    static bool is_in(const Opd &o, uint32_t r, uint32_t swz) { return o.type == 1u && o.ndim == 1u && o.idx[0] == r && o.sel == 1u && o.swz == swz; }
    static bool bcast(const Opd &o, uint32_t r, uint32_t c) { return is_tmp(o, r) && o.sel == 1u && o.swz == (c * 0x55u); }   /* rN.cccc */
    static bool scan_marker(const void *code, uint32_t len, int pos_reg, int &A, int &B) {
      A = B = -1; if (pos_reg < 0) return false;
      const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return false;
      uint32_t nch = r32(p + 32); if (nch > 64) return false; std::vector<Ins> ins; std::vector<uint32_t> sivs;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 8 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x52444853u && tag != 0x58454853u) continue;
        if ((uint64_t)off + 8 + sz > len || sz < 8) continue; const uint32_t *t = (const uint32_t *)(p + off + 8); uint32_t n = std::min<uint32_t>(sz / 4, t[1]);
        uint32_t i = 2; while (i < n) { uint32_t op = t[i] & 0x7ffu, l = (t[i] >> 24) & 0x7fu; if (op == 53u) l = i + 1 < n ? t[i + 1] : 0; if (!l) break; uint32_t end = std::min<uint32_t>(i + l, n);
          if (op == 50u || op == 56u || op == 54u || op == 103u) { Ins in; in.op = op; uint32_t j = i + 1; uint32_t tk = t[i]; while (tk >> 31) { if (j >= end) break; tk = t[j++]; }
            bool ok = true; uint32_t want = op == 50u ? 4u : op == 56u ? 3u : op == 54u ? 2u : 1u;
            for (uint32_t k = 0; k < want && ok; k++) { Opd o; ok = opd(t, j, end, o); in.o.push_back(o); }
            if (ok && op == 103u) { if (j < end) in.siv = t[j]; if (in.o[0].type == 2u && in.siv == 1u) sivs.push_back(in.o[0].idx[0]); }   /* dcl_output_siv oN, position */
            if (ok) ins.push_back(in); else ins.push_back(Ins()); }
          else ins.push_back(Ins());
          i = end; } break; }
      if (sivs.empty()) return false; uint32_t pr = (uint32_t)pos_reg;
      for (size_t k = 0; k + 9 < ins.size(); k++) { const Ins &d = ins[k];
        if (d.op != 50u || d.o.size() != 4 || !is_tmp(d.o[0], d.o[0].idx[0]) || !is_in(d.o[1], pr, 0x24u) || d.o[2].type != 8u || d.o[3].type != 8u || !is_cb(d.o[2], 0, d.o[2].idx[1]) || !is_cb(d.o[3], 0, d.o[3].idx[1])) continue;
        uint32_t rD = d.o[0].idx[0]; const Ins &i1 = ins[k + 1], &i2 = ins[k + 2], &i3 = ins[k + 3], &i4 = ins[k + 4], &i5 = ins[k + 5], &i6 = ins[k + 6], &i7 = ins[k + 7], &i8 = ins[k + 8];
        if (i1.op != 56u || i1.o.size() != 3 || !bcast(i1.o[1], rD, 1) || !is_cb(i1.o[2], 0, 1) || i1.o[0].type != 0u) continue; uint32_t rE = i1.o[0].idx[0];
        if (i2.op != 50u || i2.o.size() != 4 || !is_cb(i2.o[1], 0, 0) || !bcast(i2.o[2], rD, 0) || !is_tmp(i2.o[3], rE) || !is_tmp(i2.o[0], rE)) continue;
        if (i3.op != 50u || i3.o.size() != 4 || !is_cb(i3.o[1], 0, 2) || !bcast(i3.o[2], rD, 2) || !is_tmp(i3.o[3], rE) || i3.o[0].type != 0u) continue; uint32_t rF = i3.o[0].idx[0];
        if (i4.op != 50u || i4.o.size() != 4 || !is_cb(i4.o[1], 0, 3) || !is_in(i4.o[2], pr, 0xffu) || !is_tmp(i4.o[3], rF) || !is_tmp(i4.o[0], rF)) continue;
        if (i5.op != 56u || i5.o.size() != 3 || !bcast(i5.o[1], rF, 1) || !is_cb(i5.o[2], 1, 1) || i5.o[0].type != 0u) continue; uint32_t rG = i5.o[0].idx[0];
        if (i6.op != 50u || i6.o.size() != 4 || !is_cb(i6.o[1], 1, 0) || !bcast(i6.o[2], rF, 0) || !is_tmp(i6.o[3], rG) || !is_tmp(i6.o[0], rG)) continue;
        if (i7.op != 50u || i7.o.size() != 4 || !is_cb(i7.o[1], 1, 2) || !bcast(i7.o[2], rF, 2) || !is_tmp(i7.o[3], rG) || !is_tmp(i7.o[0], rG)) continue;
        if (i8.op != 50u || i8.o.size() != 4 || !is_cb(i8.o[1], 1, 3) || !bcast(i8.o[2], rF, 3) || !is_tmp(i8.o[3], rG) || !is_tmp(i8.o[0], rG)) continue;
        bool found = false; for (size_t m = k + 9; m < ins.size() && m < k + 24; m++) { const Ins &x = ins[m];
          if (x.op == 54u && x.o.size() == 2 && x.o[0].type == 2u && x.o[0].ndim == 1u && is_tmp(x.o[1], rG) && x.o[1].sel == 1u && x.o[1].swz == 0xe4u) { for (uint32_t sv : sivs) if (sv == x.o[0].idx[0]) found = true; if (found) break; }
          if (!x.o.empty() && x.o[0].type == 0u && x.o[0].idx[0] == rG) break;   /* rG overwritten first */
          if (x.op == 0u) break; }   /* an instruction we did not parse: give up */
        if (!found) continue;
        A = (int)d.o[2].idx[1]; B = (int)d.o[3].idx[1]; return A < 64 && B < 64; }
      return false; }
    static int scan_sem(const void *code, uint32_t len, const char *name) {   /* b101: ISGN register of semantic <name> index 0, -1 if none */
      const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return -1; size_t nl = std::strlen(name);
      uint32_t nch = r32(p + 32); if (nch > 64) return -1;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 16 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x4e475349u && tag != 0x31475349u) continue;
        if ((uint64_t)off + 8 + sz > len) continue; const uint8_t *d = p + off + 8; uint32_t n = r32(d), es = tag == 0x31475349u ? 32u : 24u; if (n > 32 || 8 + (uint64_t)n * es > sz) continue;
        for (uint32_t i = 0; i < n; i++) { const uint8_t *e = d + 8 + i * es; uint32_t no = r32(e), si = r32(e + 4), rg = r32(e + 16); if (no >= sz || si != 0) continue;
          const char *nm = (const char *)d + no; size_t ml = strnlen(nm, sz - no); if (ml == nl && !strncasecmp(nm, name, nl)) return (int)rg; }
        return -1; }
      return -1; }
    static bool scan_decode(const void *code, uint32_t len, int pos_reg, int &A, int &B) {   /* b101: the first "mad rD.xyz, v<pos>.xyzx, cb0[A], cb0[B]" (MeshExtension / MeshOrigin) */
      A = B = -1; if (pos_reg < 0) return false; const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return false;
      uint32_t nch = r32(p + 32); if (nch > 64) return false;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 8 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x52444853u && tag != 0x58454853u) continue;
        if ((uint64_t)off + 8 + sz > len || sz < 8) continue; const uint32_t *t = (const uint32_t *)(p + off + 8); uint32_t n = std::min<uint32_t>(sz / 4, t[1]);
        uint32_t i = 2; while (i < n) { uint32_t op = t[i] & 0x7ffu, l = (t[i] >> 24) & 0x7fu; if (op == 53u) l = i + 1 < n ? t[i + 1] : 0; if (!l) break; uint32_t end = std::min<uint32_t>(i + l, n);
          if (op == 50u) { uint32_t j = i + 1; uint32_t tk = t[i]; while (tk >> 31) { if (j >= end) break; tk = t[j++]; } Opd o[4]; bool ok = true; for (int k = 0; k < 4 && ok; k++) ok = opd(t, j, end, o[k]);
            if (ok && o[0].type == 0u && is_in(o[1], (uint32_t)pos_reg, 0x24u) && is_cb(o[2], 0, o[2].idx[1]) && is_cb(o[3], 0, o[3].idx[1]) && o[2].idx[1] < 64 && o[3].idx[1] < 64) { A = (int)o[2].idx[1]; B = (int)o[3].idx[1]; return true; } }
          i = end; } break; }
      return false; }
    static bool hash_listed3(const char *h) { static std::string l = [] { const char *e = std::getenv("BSI_B1_SHBBOX_VS3"); return std::string(e && *e ? e : "ff4b9172,cc3229bb,40c5f4d7"); }();   /* b101: UE3 GPU-skin VS seen as stencil subject markers at Rooftops */
      size_t p = 0; while (p < l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q - p == 8 && !l.compare(p, 8, h, 8)) return true; p = q + 1; } return false; }
    static int skins() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHSKIN", 0) > 0; return v; }   /* b107: default OFF */
    static int recv() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHRECV", 0) > 0; return v; }   /* b107: receiver-mesh bounds (b93-b95) default OFF: GPU gain ~0 measured, 1.3-2.8 ms of render thread (chat 5 b105 profile) */
    static int scan_nin(const void *code, uint32_t len) {   /* b102: number of ISGN elements, -1 unknown */
      const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return -1; uint32_t nch = r32(p + 32); if (nch > 64) return -1;
      for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off = r32(p + 36 + 4 * c); if ((uint64_t)off + 16 > len) continue;
        uint32_t tag = r32(p + off), sz = r32(p + off + 4); if (tag != 0x4e475349u && tag != 0x31475349u) continue; if ((uint64_t)off + 8 + sz > len) continue; return (int)r32(p + off + 8); }
      return -1; }
    static void vs_created(uint32_t id, const void *code, uint32_t len) { if (!mode()) return; VsI v; v.scanned = true; v.cb1 = scan_cb1(code, len); v.pos_reg = scan_pos(code, len); v.nin = scan_nin(code, len); if (markers()) scan_marker(code, len, v.pos_reg, v.decA, v.decB);
      if (skins() && v.decA < 0) { v.idx_reg = scan_sem(code, len, "BLENDINDICES"); v.w_reg = scan_sem(code, len, "BLENDWEIGHT"); if (v.idx_reg >= 0 && v.w_reg >= 0 && scan_decode(code, len, v.pos_reg, v.decA, v.decB)) v.skin = true; else v.decA = v.decB = -1; }   /* b101 */
      std::lock_guard<std::mutex> lk(smu); vsi[id] = v; }
    static void shader_released(uint32_t id) { std::lock_guard<std::mutex> lk(smu); if (!vsi.empty()) vsi.erase(id); }
    static bool hash_listed(const char *h) { static std::string l = [] { const char *e = std::getenv("BSI_B1_SHBBOX_VS"); return std::string(e && *e ? e : "3863988d"); }();
      size_t p = 0; while (p < l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q - p == 8 && !l.compare(p, 8, h, 8)) return true; p = q + 1; } return false; }
    static bool hash_listed2(const char *h) { static std::string l = [] { const char *e = std::getenv("BSI_B1_SHBBOX_VS2"); return std::string(e && *e ? e : "03ccec40"); }();   /* b93: receiver-mesh stencil VS (LocalToWorld in cb0, ViewProjection in cb1) */
      size_t p = 0; while (p < l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q - p == 8 && !l.compare(p, 8, h, 8)) return true; p = q + 1; } return false; }
    static VsI vs_info(uint32_t id) { std::lock_guard<std::mutex> lk(smu); VsI &v = vsi[id];
      if (!v.named) { v.named = true; char b[64]; v.kind = -1;
        if (b1dx_shader_name(id, b, sizeof b) && !std::strncmp(b, "vs.", 3) && std::strlen(b) >= 11) { char h[9]; std::memcpy(h, b + 3, 8); h[8] = 0; if (hash_listed(h)) v.kind = 1; else if (hash_listed2(h)) v.kind = 2; else if (v.skin) { if (hash_listed3(h)) v.kind = 5; } else if (v.decA >= 0 && v.decB >= 0) v.kind = 3; }
        if (v.kind == 5) std::fprintf(stderr, "[b1sb] skinned subject-marker VS %s (id %u): POSITION v%d, BLENDINDICES v%d, BLENDWEIGHT v%d, decode cb0[%d] * v + cb0[%d], bones cb3 rows 3i..3i+2 (b101: bbox from per-bone AABBs x bone matrices, no depth bounds)\n", b, id, v.pos_reg, v.idx_reg, v.w_reg, v.decA, v.decB);
        if (v.kind == 3) std::fprintf(stderr, "[b1sb] subject-marker material VS %s (id %u): POSITION at v%d, decode cb0[%d] * v + cb0[%d], clip = (p * cb0 rows 0..3) * cb1 rows 0..3 (b99: bbox from the projected vertices, no depth bounds)\n", b, id, v.pos_reg, v.decA, v.decB);
        if (v.kind == 2) std::fprintf(stderr, "[b1sb] receiver-mesh stencil VS %s (id %u): reads cb1 registers %02x, POSITION at v%d -> clip = (v * cb0 rows 0..3) * cb1 rows 0..3 (b93: bbox from the mesh AABB, no depth bounds)\n", b, id, v.cb1, v.pos_reg);
        if (v.kind == 1) std::fprintf(stderr, "[b1sb] projection VS %s (id %u): reads cb1 registers %02x%s, POSITION at v%d -> clip = %s * cb1 rows 0..3\n", b, id, v.cb1, v.scanned ? "" : " (not scanned)", v.pos_reg,
                                      pvt_env() == 1 || (pvt_env() < 0 && v.scanned && (v.cb1 & 0x10u)) ? "(v.xyz + cb1[4].xyz, v.w)" : "v"); }
      return v; }
    static bool pvt_of(const VsI &v) { int e = pvt_env(); if (e >= 0) return e == 1; return v.scanned && (v.cb1 & 0x10u) != 0; }

    /* ---- CPU copies: small dynamic vertex / index writes (ring of the latest), VS cb1 contents ---- */
    struct W { uint32_t id = 0, off = 0, size = 0; bool have = false; uint8_t b[4096]; };
    static W ring[256]; static unsigned ring_n;
    struct Cb { std::vector<uint8_t> b; uint32_t n = 0; };
    static std::unordered_map<uint32_t, Cb> cbs; static std::unordered_set<uint32_t> cbwant;
    static void vs_cb_bound(uint32_t slot, uint32_t id) { if (mode() && (slot == 1 || slot == 0 || slot == 3) && id) cbwant.insert(id); }   /* b101: cb3 = VSBoneConstants */   /* b1dx_c_cbs, stage 0 (b93: cb0 = LocalToWorld of the receiver-mesh VS too) */
    /* b93: static vertex / index buffers created with data (any size up to 1 MB, 128 MB in all): a CPU copy for the receiver-mesh AABBs; dropped on any later write / copy */
    struct Big { std::vector<uint8_t> b; std::vector<uint8_t> valid; /* one byte per 64-byte block: 1 = written by the CPU since the copy began */
      size_t size() const { return b.size(); } const uint8_t *data() const { return b.data(); } uint8_t *data() { return b.data(); }
      void mark(uint64_t off, uint64_t n) { for (uint64_t k = off >> 6, e = (off + n + 63) >> 6; k < e && k < valid.size(); k++) valid[k] = 1; }
      bool ok(uint64_t off, uint64_t n) const { if (off + n > b.size()) return false; for (uint64_t k = off >> 6, e = (off + n + 63) >> 6; k < e; k++) if (!valid[k]) return false; return true; } };
    static std::unordered_map<uint32_t, Big> bigbuf; static size_t big_total; static std::unordered_map<uint32_t, uint32_t> big_gen;
    static const size_t BIG_EACH = (size_t)8u << 20, BIG_ALL = (size_t)192u << 20;   /* b95: UE3's dynamic VB/IB pools are several MB */
    static Big *big_make(uint32_t id, size_t bsz) { if (bsz > BIG_EACH || big_total + bsz > BIG_ALL) return nullptr; Big &g = bigbuf[id]; g.b.assign(bsz, 0); g.valid.assign((bsz + 63) / 64, 0); big_total += bsz; big_gen[id]++; return &g; }
    static void big_drop(uint32_t id) { auto it = bigbuf.find(id); if (it != bigbuf.end()) { big_total -= it->second.size(); bigbuf.erase(it); big_gen[id]++; } }
    static void wrote(uint32_t id, Obj *o, uint32_t off, uint32_t size, const void *data) {   /* b1dx_c_update / unmap / bufwrite (g_rmu held) */
      if (!mode() || !o || o->kind != 1 || !data || !size) return;
      if (o->dxgi & 0x4u) { if (cbwant.empty() || !cbwant.count(id)) return; uint32_t bsz = (uint32_t)o->buf->info().size; if (bsz > 65536 || off >= bsz) return; if (off + size > bsz) size = bsz - off;
        Cb &c = cbs[id]; if (c.b.size() < bsz) c.b.resize(bsz); if (off > c.n) return; std::memcpy(c.b.data() + off, data, size); if (off + size > c.n) c.n = off + size; return; }
      if (!(o->dxgi & 0x3u)) return;   /* D3D11_BIND_VERTEX_BUFFER | INDEX_BUFFER */
      {   /* b94/b95: every CPU write into a VB / IB (Lock/Unlock uploads, UE3's dynamic pools written piecewise per draw) is mirrored into the store; blocks never written stay invalid */
        uint32_t bsz = (uint32_t)o->buf->info().size; auto it = bigbuf.find(id); Big *g = it != bigbuf.end() ? &it->second : nullptr;
        if (!g && mode() && (recv() || skins() || markers())) g = big_make(id, bsz);   /* b107: the static store only for the bounds that need it */
        if (g) { if ((uint64_t)off + size <= g->size()) { std::memcpy(g->data() + off, data, size); g->mark(off, size); big_gen[id]++; } else big_drop(id); } }
      if (size > sizeof ring[0].b) { W &w = ring[ring_n++ & 255u]; w.id = id; w.off = off; w.size = size; w.have = false; return; }
      W &w = ring[ring_n++ & 255u]; w.id = id; w.off = off; w.size = size; w.have = size <= sizeof w.b; if (w.have) std::memcpy(w.b, data, size); }
    void created(uint32_t id, uint32_t bind, uint32_t size, const void *data) {   /* b1dx_buffer_create2 with initial data: small static VB / IB (UE3's shared quad buffers); b93: big ones kept whole for the receiver-mesh AABBs */
      if (!mode() || !id || !data || !(bind & 0x3u)) return;
      if (recv() || skins() || markers()) if (Big *g = big_make(id, size)) { std::memcpy(g->data(), data, size); g->mark(0, size); }   /* b101: small ones too; b107: only when a bound needs the store */
      if (size > sizeof ring[0].b) return;
      W &w = ring[ring_n++ & 255u]; w.id = id; w.off = 0; w.size = size; w.have = true; std::memcpy(w.b, data, size); }
    static void buf_copied(uint32_t id) { if (!mode() || !id) return; W &w = ring[ring_n++ & 255u]; w.id = id; w.off = 0; w.size = 0xffffffffu; w.have = false; if (!cbs.empty()) cbs.erase(id); if (!bigbuf.empty()) big_drop(id); }   /* GPU copy into a buffer: unknown bytes */
    void released(uint32_t id, int kind) { if (!mode() || !id) return;
      if (kind == 2) { if (!regs.empty()) regs.erase(id); tile_released(id); return; }
      if (!bigbuf.empty()) big_drop(id);
      for (auto &w : ring) if (w.id == id) w.id = 0;
      if (!cbs.empty()) cbs.erase(id); if (!cbwant.empty()) cbwant.erase(id); }
    static const uint8_t *find(uint32_t id, uint32_t off, uint32_t size) {   /* the latest write covering [off, off + size) whole; null if unknown or partly overwritten later */
      if (!id || !size) return nullptr; uint64_t a = off, e = (uint64_t)off + size;
      for (unsigned i = 0; i < 256u && i < ring_n; i++) { const W &w = ring[(ring_n - 1 - i) & 255u]; if (w.id != id) continue;
        uint64_t wa = w.off, we = (uint64_t)w.off + w.size; if (we <= a || wa >= e) continue;
        return wa <= a && we >= e && w.have ? w.b + (a - wa) : nullptr; }
      return nullptr; }
    static const float *cb1_get(uint32_t need) { uint32_t id = g_bc.cb[0][1]; if (!id || id == BC_UNK) return nullptr; auto it = cbs.find(id); return it == cbs.end() || it->second.n < need ? nullptr : (const float *)it->second.b.data(); }
    static const float *cb_get(uint32_t slot, uint32_t need, uint32_t *have = nullptr) { uint32_t id = g_bc.cb[0][slot]; if (!id || id == BC_UNK) return nullptr; auto it = cbs.find(id); if (it == cbs.end() || it->second.n < need) return nullptr; if (have) *have = it->second.n; return (const float *)it->second.b.data(); }   /* b101 */
    static const float *cb0_get(uint32_t need) { uint32_t id = g_bc.cb[0][0]; if (!id || id == BC_UNK) return nullptr; auto it = cbs.find(id); return it == cbs.end() || it->second.n < need ? nullptr : (const float *)it->second.b.data(); }   /* b93 */

    /* ---- the current draw's geometry ---- */
    struct Cur { int kind = 0; /* 1 volume, 2 projection, 3 clear quad, 4 receiver mesh (b93) */ int pk = 0; uint32_t res = 0; bool have = false, pvt = false, cover = false; std::vector<double> v; /* x y z w per vertex, as in the buffer (kind 4: the 8 AABB corners) */ float l2w[16]; } cur;
    static long n_recv, n_recv_nodata, n_recv_unb; static long n_mark, n_mark_nodata, n_mark_unb;   /* b99: subject markers */ static std::unordered_map<uint64_t, std::array<double, 6>> aabbs;   /* b93: receiver meshes; AABB cache per (buffers, range) */
    /* b96 census (BSI_B1_SHCENSUS=1, diagnostic): an occlusion query around every projection quad = the pixels that pass its stencil (+ depth bounds / scissor)
       test = the pixels its kernel runs on. Ring of 8 frames x 96 queries, read 4 frames later; histogram per projection kind in the stats. */
    static int census() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_SHCENSUS", 0); if (v < 0) v = 0; } return v; }   /* 1 = shadow quads; 2 = b110 overdraw census of the scene-colour pass (RGBA16F target): every draw queried */
    static double c2_px[2]; static long c2_n[2], c2_frames;   /* b111: [0] opaque (blend off), [1] blended */
    struct CQ { Rc<DxvkQuery> q; int pk = 0; bool replay = false; bool scissored = false; double area = 0; };
    struct CFrame { std::vector<CQ> q; int n = 0; bool live = false; };
    static CFrame cring[8]; static int ccur; static bool c_open; static double c_px_sum[2][2], c_area_sum[2][2]; static long c_n[2][2], c_hist[2][6], c_skipped; static double c_eye_px;
    static void census_begin(int pk, bool replay, bool scissored, double area_frac) {
      if (!census() || c_open) return; CFrame &f = cring[ccur]; if (f.n >= (census() == 2 ? 2048 : 96)) return;
      if (f.n >= (int)f.q.size()) f.q.push_back(CQ());
      CQ &c = f.q[f.n]; if (c.q == nullptr) c.q = g_dev->createGpuQuery(VK_QUERY_TYPE_OCCLUSION, VK_QUERY_CONTROL_PRECISE_BIT, 0);
      c.pk = pk; c.replay = replay; c.scissored = scissored; c.area = area_frac; f.n++; c_open = true;
      emit([q = c.q] (DxvkContext* cc) { cc->beginQuery(q); }); }
    static void census_end() { if (!c_open) return; c_open = false; CFrame &f = cring[ccur]; CQ &c = f.q[f.n - 1]; emit([q = c.q] (DxvkContext* cc) { cc->endQuery(q); }); }
    static void census_frame() {   /* at present: close the frame's ring slot, read the slot from 4 frames ago */
      if (!census()) return; cring[ccur].live = true; ccur = (ccur + 1) & 7; if (cring[ccur].live) { c_skipped++; cring[ccur].live = false; } cring[ccur].n = 0;
      CFrame &o = cring[(ccur + 4) & 7]; if (!o.live) return; o.live = false; if (census() == 2) c2_frames++;
      for (int i = 0; i < o.n; i++) { DxvkQueryData d = { }; if (o.q[i].q->getData(d) != DxvkGpuQueryStatus::Available) { c_skipped++; continue; }
        double px = (double)d.occlusion.samplesPassed; if (o.q[i].pk < 0) { int b = o.q[i].pk == -2 ? 1 : 0; c2_px[b] += px; c2_n[b]++; continue; } int k = o.q[i].pk == b1vr::PK_SHPROJ_DIR ? 0 : 1, r = o.q[i].replay ? 1 : 0;
        c_px_sum[k][r] += px; c_area_sum[k][r] += o.q[i].area; c_n[k][r]++;
        double fr = c_eye_px > 0 ? px / c_eye_px : 0; c_hist[k][fr < 0.01 ? 0 : fr < 0.05 ? 1 : fr < 0.25 ? 2 : fr < 0.5 ? 3 : fr < 0.9 ? 4 : 5]++; } }
    static void compose(const float *a, const float *b, float *m) { for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { double s = 0; for (int j = 0; j < 4; j++) s += (double)a[i * 4 + j] * (double)b[j * 4 + k]; m[i * 4 + k] = (float)s; } }   /* row-vector: v * a * b */
    static bool armed; static Rect arm; static float rec_vp[6]; static VkRect2D rec_sc; static bool rec_valid;
    bool skip_draw; static long n_vol_skipped, n_proj_nost, n_proj_da; static bool st_override;   /* b92 mode 4 (NOT EXACT ceiling probe): volumes skipped, quads without stencil test */
    static void on_vp(DxvkViewport &d) {   /* apply_viewports, viewport 0 after es86's cap: record the effective rect; the armed bbox shrinks the scissor */
      std::memcpy(rec_vp, g_cs.vp[0], sizeof rec_vp); rec_sc = d.scissor; rec_valid = g_cs.nvp > 0;
      if (!armed) return;
      Rect s = { d.scissor.offset.x, d.scissor.offset.y, (int)std::min<int64_t>((int64_t)d.scissor.offset.x + d.scissor.extent.width, 1000000000), (int)std::min<int64_t>((int64_t)d.scissor.offset.y + d.scissor.extent.height, 1000000000) };
      if (s.empty()) return;
      Rect r = isect(s, arm); if (r.empty()) r = Rect { s.x0, s.y0, s.x0 + 1, s.y0 + 1 };   /* nothing to draw: one pixel (its stencil is 0) */
      d.scissor.offset = { r.x0, r.y0 }; d.scissor.extent = { (uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0) }; }
    static bool eff(Rect &out, Rect &in) {   /* the last emitted viewport 0 ∩ scissor: rounded outward (out) and inward (in) */
      if (!rec_valid) return false; double x = rec_vp[0], y = rec_vp[1], w = rec_vp[2], h = rec_vp[3]; if (!(w > 0) || !(h > 0)) return false;
      Rect s = { rec_sc.offset.x, rec_sc.offset.y, (int)std::min<int64_t>((int64_t)rec_sc.offset.x + rec_sc.extent.width, 1000000000), (int)std::min<int64_t>((int64_t)rec_sc.offset.y + rec_sc.extent.height, 1000000000) };
      Rect vo = { clampi(std::floor(x)), clampi(std::floor(y)), clampi(std::ceil(x + w)), clampi(std::ceil(y + h)) }, vi = { clampi(std::ceil(x)), clampi(std::ceil(y)), clampi(std::floor(x + w)), clampi(std::floor(y + h)) };
      out = isect(s, vo); in = isect(s, vi); return !out.empty(); }
    /* clip = v * M (M = cb[0..15], rows 0..3; the convention of vs_shadow_eye_consts), v.xyz += cb[16..18] first when pvt;
       pixel = D3D viewport transform (y down); bbox over the vertices + margin, rounded outward. 0 = some clip.w <= eps */
    static int project(const float *cb, bool pvt, const std::vector<double> &v, const float *vp, Rect &box, double *zlo = nullptr, double *zhi = nullptr) {
      double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300, mnz = 1e300, mxz = -1e300; bool unb = false; zmax_vis = -1e300; n_vis = 0;
      for (size_t i = 0; i + 3 < v.size(); i += 4) { double p[4] = { v[i], v[i + 1], v[i + 2], v[i + 3] }; if (pvt) for (int c = 0; c < 3; c++) p[c] += (double)cb[16 + c];
        double c[4]; for (int k = 0; k < 4; k++) c[k] = p[0] * (double)cb[0 * 4 + k] + p[1] * (double)cb[1 * 4 + k] + p[2] * (double)cb[2 * 4 + k] + p[3] * (double)cb[3 * 4 + k];
        if (!(c[3] > 1e-6)) { unb = true; continue; }
        n_vis++; double nx = c[0] / c[3], ny = c[1] / c[3]; double px = (double)vp[0] + (nx + 1.0) * 0.5 * (double)vp[2], py = (double)vp[1] + (1.0 - ny) * 0.5 * (double)vp[3];
        if (!(px == px) || !(py == py)) return 0;
        double nz = c[2] / c[3], dz = (double)vp[4] + nz * ((double)vp[5] - (double)vp[4]); if (!(dz == dz)) return 0;   /* D3D11 viewport depth mapping */
        mnx = std::min(mnx, px); mxx = std::max(mxx, px); mny = std::min(mny, py); mxy = std::max(mxy, py); mnz = std::min(mnz, dz); mxz = std::max(mxz, dz); }
      zmax_vis = mxz; if (unb || !(mnx <= mxx) || !(mny <= mxy) || !(mnz <= mxz)) return 0;
      if (zlo && zhi) { double lo = std::min(vp[4], vp[5]), hi = std::max(vp[4], vp[5]); *zlo = std::max(lo, mnz); *zhi = std::min(hi, mxz); if (*zlo > *zhi) { *zlo = lo; *zhi = hi; } }   /* clipped / clamped faces stay inside the viewport's depth range */
      double m = margin(); box = { clampi(std::floor(mnx - m)), clampi(std::floor(mny - m)), clampi(std::ceil(mxx + m) + 1), clampi(std::ceil(mxy + m) + 1) }; return 1; }
    /* b99: a convex set of vertices partly behind the eye (clip.w <= 0): the visible part's projection is the hull of the inside vertices plus the
       rays through the w = 0 crossings of every vertex pair (a superset of the hull's edges); each crossing sends the bbox to the viewport edge in its
       direction (both edges when the crossing is at the centre within tol). No depth range. 1 ok (box may be huge: clamp with the eye rect), 0 nothing visible / NaN */
    static int project_clip(const float *cb, const std::vector<double> &v, const float *vp, Rect &box) {
      size_t n = v.size() / 4; if (!n || n > 1024) return 0; std::vector<double> c(4 * n); double amax = 0;
      for (size_t i = 0; i < n; i++) { const double *p = &v[4 * i]; for (int k = 0; k < 4; k++) { double a = p[0] * (double)cb[0 * 4 + k] + p[1] * (double)cb[1 * 4 + k] + p[2] * (double)cb[2 * 4 + k] + p[3] * (double)cb[3 * 4 + k]; if (!(a == a)) return 0; c[4 * i + k] = a; if (k < 2) amax = std::max(amax, std::fabs(a)); } }
      double mnx = 1e300, mxx = -1e300, mny = 1e300, mxy = -1e300; int nin = 0; const double eps = 1e-6, tol = amax * 1e-5 + 1e-9;
      for (size_t i = 0; i < n; i++) { if (!(c[4 * i + 3] > eps)) continue; nin++; double nx = c[4 * i] / c[4 * i + 3], ny = c[4 * i + 1] / c[4 * i + 3];
        double px = (double)vp[0] + (nx + 1.0) * 0.5 * (double)vp[2], py = (double)vp[1] + (1.0 - ny) * 0.5 * (double)vp[3]; if (!(px == px) || !(py == py)) return 0;
        mnx = std::min(mnx, px); mxx = std::max(mxx, px); mny = std::min(mny, py); mxy = std::max(mxy, py); }
      if (!nin) return 0;
      for (size_t i = 0; i < n; i++) { double wi = c[4 * i + 3]; if (!(wi > eps)) continue;
        for (size_t j = 0; j < n; j++) { double wj = c[4 * j + 3]; if (!(wj <= eps)) continue; double t = wi / (wi - wj);   /* the w = 0 point of the segment i -> j */
          double x0 = c[4 * i] + t * (c[4 * j] - c[4 * i]), y0 = c[4 * i + 1] + t * (c[4 * j + 1] - c[4 * i + 1]);
          if (x0 > tol) mxx = 1e300; else if (x0 < -tol) mnx = -1e300; else { mxx = 1e300; mnx = -1e300; }
          if (y0 > tol) mny = -1e300; else if (y0 < -tol) mxy = 1e300; else { mny = -1e300; mxy = 1e300; } } }   /* ndc y up = screen y down */
      double m = margin(); box = { clampi(std::floor(mnx - m)), clampi(std::floor(mny - m)), clampi(std::ceil(mxx + m) + 1), clampi(std::ceil(mxy + m) + 1) }; return 1; }
    static long n_clip, n_clip_full;
    static void add_convex(uint32_t res, const float *m, const std::vector<double> &v, const Rect &eo, const float *vp, long &n_unbound) {   /* b99: kind 4 / markers: bbox, else the clipped bbox, else the whole rect */
      Rect box; double zlo, zhi;
      if (project(m, false, v, vp, box, &zlo, &zhi)) { add_rect(res, isect(box, eo), zlo, zhi, false); return; }
      if (project_clip(m, v, vp, box)) { n_clip++; Rect r = isect(box, eo); if (r.empty()) { n_clip_full++; add_rect_unb(res, eo, vp); return; } if (r.x0 <= eo.x0 && r.y0 <= eo.y0 && r.x1 >= eo.x1 && r.y1 >= eo.y1) n_clip_full++; add_rect(res, r, 0, 1, false); return; }
      n_unbound++; add_rect_unb(res, eo, vp); }
    /* the vertices this draw reads (position attribute of a position-only layout), in draw order. 1 ok; 0 not eligible (nodata: the bytes are unknown) */
    static const char *fetch_why;
    static int fetch(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, std::vector<double> &v, bool &nodata, int pos_reg) {
      v.clear(); nodata = false; fetch_why = "";
      uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) { fetch_why = "no input layout"; return 0; } IL *il = g_ils.get(ilid); if (!il || !il->na) { fetch_why = "no input layout"; return 0; }
      int ai = -1; if (pos_reg >= 0) { for (uint32_t k = 0; k < il->na; k++) if ((int)il->in[k].attribute().location == pos_reg) ai = (int)k; } else if (il->na == 1) ai = 0;
      if (ai < 0) { fetch_why = "position attribute not in the layout"; return 0; }
      fetch_why = "position format / rate / instancing"; DxvkVertexAttribute a = il->in[ai].attribute(); int nc = a.format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : a.format == VK_FORMAT_R32G32_SFLOAT ? 2 : 0; if (!nc) return 0;
      bool rate_ok = false; for (uint32_t k = 0; k < il->nb; k++) { DxvkVertexBinding b = il->in[il->na + k].binding(); if (b.binding == a.binding) rate_ok = b.inputRate == VK_VERTEX_INPUT_RATE_VERTEX; } if (!rate_ok) return 0;
      uint32_t slot = a.binding; if (slot >= 32) return 0; uint32_t vb = g_bc.vb[slot], st = g_bc.vbs[slot], bo = g_bc.vbo[slot]; if (!vb || vb == BC_UNK || st == BC_UNK || !st || bo == BC_UNK) { fetch_why = "no vertex buffer bound"; return 0; }
      if ((dc ? dc->instanceCount : dic->instanceCount) > 1) return 0;
      fetch_why = "vertex / index count"; std::vector<uint32_t> idx;
      if (dic) { uint32_t count = dic->indexCount; if (!count || count > 4096) return 0; uint32_t ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK) return 0;
        uint32_t isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; uint64_t io = (uint64_t)g_bc.ibo + (uint64_t)dic->firstIndex * isz; if (io + (uint64_t)count * isz > 0xffffffffu) return 0;
        const uint8_t *ip = find(ib, (uint32_t)io, count * isz); if (!ip) { nodata = true; fetch_why = "index bytes unknown"; return 0; }
        for (uint32_t i = 0; i < count; i++) { uint32_t x = isz == 4 ? r32(ip + 4 * i) : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8)); if (x == (isz == 4 ? 0xffffffffu : 0xffffu)) continue;   /* strip cut */
          int64_t vi = (int64_t)x + dic->vertexOffset; if (vi < 0 || vi > 0xffffffffll) return 0; idx.push_back((uint32_t)vi); }
        if (idx.empty()) return 0; }
      else { uint32_t count = dc->vertexCount; if (!count || count > 4096) return 0; for (uint32_t i = 0; i < count; i++) idx.push_back(dc->firstVertex + i); }
      uint32_t mn = idx[0], mx = idx[0]; for (uint32_t x : idx) { mn = std::min(mn, x); mx = std::max(mx, x); }
      uint64_t a0 = (uint64_t)bo + (uint64_t)mn * st + a.offset, a1 = (uint64_t)bo + (uint64_t)mx * st + a.offset + 4u * nc; if (a1 > 0xffffffffu || a0 >= a1) return 0;
      const uint8_t *p = find(vb, (uint32_t)a0, (uint32_t)(a1 - a0)); if (!p) { nodata = true; fetch_why = "vertex bytes unknown"; return 0; }
      fetch_why = "";
      v.reserve(4 * idx.size());
      for (uint32_t x : idx) { float f[4] = { 0.f, 0.f, 0.f, 1.f }; std::memcpy(f, p + (uint64_t)(x - mn) * st, 4u * nc); for (int c = 0; c < 4; c++) v.push_back((double)f[c]); }
      return 1; }
    /* b93: the object-space AABB of the vertices a draw reads, from the CPU copies (bigbuf for static buffers, the ring for small ones); cached per buffers + range */
    static const uint8_t *bytes(uint32_t id, uint64_t off, uint64_t size) { auto it = bigbuf.find(id); if (it != bigbuf.end() && it->second.ok(off, size)) return it->second.data() + off; return off + size <= 0xffffffffu ? find(id, (uint32_t)off, (uint32_t)size) : nullptr; }
    static int fetch_aabb(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, int pos_reg, std::array<double, 6> &out) {
      fetch_why = "";
      uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) { fetch_why = "no input layout"; return 0; } IL *il = g_ils.get(ilid); if (!il || !il->na) { fetch_why = "no input layout"; return 0; }
      int ai = -1; if (pos_reg >= 0) { for (uint32_t k = 0; k < il->na; k++) if ((int)il->in[k].attribute().location == pos_reg) ai = (int)k; } else if (il->na == 1) ai = 0;
      if (ai < 0) { fetch_why = "position attribute not in the layout"; return 0; }
      fetch_why = "position format / rate / instancing"; DxvkVertexAttribute a = il->in[ai].attribute(); int nc = a.format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 0; if (!nc) return 0;
      bool rate_ok = false; for (uint32_t k = 0; k < il->nb; k++) { DxvkVertexBinding b = il->in[il->na + k].binding(); if (b.binding == a.binding) rate_ok = b.inputRate == VK_VERTEX_INPUT_RATE_VERTEX; } if (!rate_ok) return 0;
      uint32_t slot = a.binding; if (slot >= 32) return 0; uint32_t vb = g_bc.vb[slot], st = g_bc.vbs[slot], bo = g_bc.vbo[slot]; if (!vb || vb == BC_UNK || st == BC_UNK || !st || bo == BC_UNK) { fetch_why = "no vertex buffer bound"; return 0; }
      if ((dc ? dc->instanceCount : dic->instanceCount) > 1) return 0;
      uint32_t ib = 0, ibo = 0, isz = 0, count = dc ? dc->vertexCount : dic->indexCount, first = dc ? dc->firstVertex : dic->firstIndex; int32_t voff = dic ? dic->vertexOffset : 0;
      if (dic) { ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK) return 0; ibo = g_bc.ibo; isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; }
      fetch_why = "vertex / index count"; if (!count || count > 65536) return 0;
      uint64_t key = 1469598103934665603ull; for (uint64_t x : { (uint64_t)vb, (uint64_t)bo, (uint64_t)st, (uint64_t)a.offset, (uint64_t)nc, (uint64_t)ib, (uint64_t)ibo, (uint64_t)isz, (uint64_t)count, (uint64_t)first, (uint64_t)(uint32_t)voff, (uint64_t)big_gen[vb], (uint64_t)(ib ? big_gen[ib] : 0u) }) key = (key ^ x) * 1099511628211ull;
      auto hit = aabbs.find(key); if (hit != aabbs.end()) { out = hit->second; return 1; }
      const uint8_t *ip = nullptr; if (dic) { uint64_t io = (uint64_t)ibo + (uint64_t)first * isz; ip = bytes(ib, io, (uint64_t)count * isz); if (!ip) { fetch_why = "index bytes unknown"; return 0; } }
      auto vit = bigbuf.find(vb); const Big *vg = vit != bigbuf.end() ? &vit->second : nullptr; const uint8_t *vbase = vg ? vg->data() : nullptr; size_t vsize = vg ? vg->size() : 0;
      if (!vbase) { fetch_why = "vertex bytes unknown"; return 0; }
      double mn[3] = { 1e300, 1e300, 1e300 }, mx[3] = { -1e300, -1e300, -1e300 }; size_t nv = 0;
      for (uint32_t i = 0; i < count; i++) { uint64_t x;
        if (dic) { x = isz == 4 ? r32(ip + 4 * i) : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8)); if (x == (isz == 4 ? 0xffffffffull : 0xffffull)) continue; int64_t v = (int64_t)x + voff; if (v < 0) return 0; x = (uint64_t)v; } else x = (uint64_t)first + i;
        uint64_t off = (uint64_t)bo + x * st + a.offset; if (off + 4u * nc > vsize || !vg->ok(off, 4u * nc)) { fetch_why = off + 4u * nc > vsize ? "vertex outside the buffer" : "vertex bytes never written"; return 0; }
        float f[3]; std::memcpy(f, vbase + off, 12); for (int c = 0; c < 3; c++) { if (!(f[c] == f[c])) return 0; mn[c] = std::min(mn[c], (double)f[c]); mx[c] = std::max(mx[c], (double)f[c]); } nv++; }
      if (!nv) return 0;
      out = { mn[0], mn[1], mn[2], mx[0], mx[1], mx[2] }; if (aabbs.size() < 65536) aabbs[key] = out; return 1; }
    /* b99: the decoded object-space vertices (x y z w) a subject-marker draw reads: p = raw.xyz * scale.xyz + bias.xyz (the VS's cb0[decA] / cb0[decB]), w = raw.w (1 for float3) */
    static int fetch_mark(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, int pos_reg, const float *scale, const float *bias, std::vector<double> &v) {
      v.clear(); fetch_why = "";
      uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) { fetch_why = "no input layout"; return 0; } IL *il = g_ils.get(ilid); if (!il || !il->na) { fetch_why = "no input layout"; return 0; }
      int ai = -1; if (pos_reg >= 0) for (uint32_t k = 0; k < il->na; k++) if ((int)il->in[k].attribute().location == pos_reg) ai = (int)k;
      if (ai < 0) { fetch_why = "position attribute not in the layout"; return 0; }
      DxvkVertexAttribute a = il->in[ai].attribute(); int fmt = a.format == VK_FORMAT_R32G32B32_SFLOAT ? 1 : a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 2 : a.format == VK_FORMAT_R16G16B16A16_SNORM ? 3 : 0; uint32_t esz = fmt == 1 ? 12u : fmt == 2 ? 16u : 8u;
      if (!fmt) { fetch_why = "position format"; return 0; }
      bool rate_ok = false; for (uint32_t k = 0; k < il->nb; k++) { DxvkVertexBinding b = il->in[il->na + k].binding(); if (b.binding == a.binding) rate_ok = b.inputRate == VK_VERTEX_INPUT_RATE_VERTEX; } if (!rate_ok) { fetch_why = "instance rate"; return 0; }
      uint32_t slot = a.binding; if (slot >= 32) return 0; uint32_t vb = g_bc.vb[slot], st = g_bc.vbs[slot], bo = g_bc.vbo[slot]; if (!vb || vb == BC_UNK || st == BC_UNK || !st || bo == BC_UNK) { fetch_why = "no vertex buffer bound"; return 0; }
      if ((dc ? dc->instanceCount : dic->instanceCount) > 1) { fetch_why = "instanced"; return 0; }
      fetch_why = "vertex / index count"; std::vector<uint32_t> idx;
      if (dic) { uint32_t count = dic->indexCount; if (!count || count > 8192) return 0; uint32_t ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK) return 0;
        uint32_t isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; uint64_t io = (uint64_t)g_bc.ibo + (uint64_t)dic->firstIndex * isz;
        const uint8_t *ip = bytes(ib, io, (uint64_t)count * isz); if (!ip) { fetch_why = "index bytes unknown"; return 0; }
        for (uint32_t i = 0; i < count; i++) { uint32_t x = isz == 4 ? r32(ip + 4 * i) : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8)); if (x == (isz == 4 ? 0xffffffffu : 0xffffu)) continue;
          int64_t vi = (int64_t)x + dic->vertexOffset; if (vi < 0 || vi > 0xffffffffll) return 0; idx.push_back((uint32_t)vi); }
        if (idx.empty()) return 0; }
      else { uint32_t count = dc->vertexCount; if (!count || count > 8192) return 0; for (uint32_t i = 0; i < count; i++) idx.push_back(dc->firstVertex + i); }
      uint32_t mn = idx[0], mx = idx[0]; for (uint32_t x : idx) { mn = std::min(mn, x); mx = std::max(mx, x); }
      uint64_t a0 = (uint64_t)bo + (uint64_t)mn * st + a.offset, a1 = (uint64_t)bo + (uint64_t)mx * st + a.offset + esz; if (a0 >= a1) return 0;
      const uint8_t *p = bytes(vb, a0, a1 - a0); if (!p) { fetch_why = "vertex bytes unknown"; return 0; }
      fetch_why = ""; v.reserve(4 * idx.size());
      for (uint32_t x : idx) { const uint8_t *q = p + (uint64_t)(x - mn) * st; double f[4] = { 0, 0, 0, 1 };
        if (fmt == 3) { for (int c = 0; c < 4; c++) { int16_t h; std::memcpy(&h, q + 2 * c, 2); f[c] = std::max(-1.0, (double)h / 32767.0); } }
        else { float g[4] = { 0.f, 0.f, 0.f, 1.f }; std::memcpy(g, q, esz); for (int c = 0; c < 4; c++) f[c] = (double)g[c]; }
        for (int c = 0; c < 3; c++) { double d = f[c] * (double)scale[c] + (double)bias[c]; if (!(d == d)) return 0; v.push_back(d); } v.push_back(f[3]); }
      return 1; }
    /* b101: skinned subject markers (UE3 GPU skin): per-bone AABBs of the RAW positions of the vertices each bone influences (weight > 0), cached per
       buffers + range; per draw the 8 corners of every used bone's box are decoded, skinned by that bone alone, taken to world space, and all of them
       bound the mesh (a skinned vertex is a convex combination of its per-bone images; a projective map keeps convex hulls). */
    struct SkinBoxes { std::vector<std::array<double, 6>> b; std::vector<uint8_t> used; };
    static std::unordered_map<uint64_t, SkinBoxes> skinboxes; static long n_skin, n_skin_nodata, n_skin_unb;
    static bool attr_of(IL *il, int reg, DxvkVertexAttribute &a, uint32_t &vb, uint32_t &st, uint32_t &bo) {
      int ai = -1; for (uint32_t k = 0; k < il->na; k++) if ((int)il->in[k].attribute().location == reg) ai = (int)k; if (ai < 0) return false; a = il->in[ai].attribute();
      bool rate_ok = false; for (uint32_t k = 0; k < il->nb; k++) { DxvkVertexBinding b = il->in[il->na + k].binding(); if (b.binding == a.binding) rate_ok = b.inputRate == VK_VERTEX_INPUT_RATE_VERTEX; } if (!rate_ok || a.binding >= 32) return false;
      vb = g_bc.vb[a.binding]; st = g_bc.vbs[a.binding]; bo = g_bc.vbo[a.binding]; return vb && vb != BC_UNK && st != BC_UNK && st && bo != BC_UNK; }
    static const SkinBoxes *fetch_skin(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, const VsI &vi) {
      fetch_why = ""; uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) { fetch_why = "no input layout"; return nullptr; } IL *il = g_ils.get(ilid); if (!il || !il->na) { fetch_why = "no input layout"; return nullptr; }
      DxvkVertexAttribute ap, ai, aw; uint32_t vbp, stp, bop, vbi, sti, boi, vbw, stw, bow;
      if (!attr_of(il, vi.pos_reg, ap, vbp, stp, bop) || !attr_of(il, vi.idx_reg, ai, vbi, sti, boi) || !attr_of(il, vi.w_reg, aw, vbw, stw, bow)) { fetch_why = "attribute / vertex buffer"; return nullptr; }
      int fmt = ap.format == VK_FORMAT_R32G32B32_SFLOAT ? 1 : ap.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 2 : ap.format == VK_FORMAT_R16G16B16A16_SNORM ? 3 : 0; uint32_t esz = fmt == 1 ? 12u : fmt == 2 ? 16u : 8u;
      if (!fmt || ai.format != VK_FORMAT_R8G8B8A8_UINT || aw.format != VK_FORMAT_R8G8B8A8_UNORM) { fetch_why = "position / blend formats"; return nullptr; }
      if ((dc ? dc->instanceCount : dic->instanceCount) > 1) { fetch_why = "instanced"; return nullptr; }
      uint32_t count = dc ? dc->vertexCount : dic->indexCount, first = dc ? dc->firstVertex : dic->firstIndex; int32_t voff = dic ? dic->vertexOffset : 0; uint32_t ib = 0, ibo = 0, isz = 0;
      if (dic) { ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK) { fetch_why = "no index buffer"; return nullptr; } ibo = g_bc.ibo; isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; }
      fetch_why = "vertex / index count"; if (!count || count > 262144) return nullptr;
      uint64_t key = 1469598103934665603ull; for (uint64_t x : { (uint64_t)vbp, (uint64_t)bop, (uint64_t)stp, (uint64_t)ap.offset, (uint64_t)fmt, (uint64_t)vbi, (uint64_t)boi, (uint64_t)sti, (uint64_t)ai.offset, (uint64_t)vbw, (uint64_t)bow, (uint64_t)stw, (uint64_t)aw.offset,
                                                        (uint64_t)ib, (uint64_t)ibo, (uint64_t)isz, (uint64_t)count, (uint64_t)first, (uint64_t)(uint32_t)voff, (uint64_t)big_gen[vbp], (uint64_t)big_gen[vbi], (uint64_t)big_gen[vbw], (uint64_t)(ib ? big_gen[ib] : 0u) }) key = (key ^ x) * 1099511628211ull;
      auto hit = skinboxes.find(key); if (hit != skinboxes.end()) return &hit->second;
      const uint8_t *ip = nullptr; if (dic) { ip = bytes(ib, (uint64_t)ibo + (uint64_t)first * isz, (uint64_t)count * isz); if (!ip) { fetch_why = "index bytes unknown"; return nullptr; } }
      SkinBoxes sb; sb.b.assign(70, { 1e300, 1e300, 1e300, -1e300, -1e300, -1e300 }); sb.used.assign(70, 0); size_t nv = 0;
      for (uint32_t i = 0; i < count; i++) { uint64_t x;
        if (dic) { x = isz == 4 ? r32(ip + 4 * i) : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8)); if (x == (isz == 4 ? 0xffffffffull : 0xffffull)) continue; int64_t v = (int64_t)x + voff; if (v < 0) { fetch_why = "negative vertex index"; return nullptr; } x = (uint64_t)v; } else x = (uint64_t)first + i;
        const uint8_t *pp = bytes(vbp, (uint64_t)bop + x * stp + ap.offset, esz), *pi = bytes(vbi, (uint64_t)boi + x * sti + ai.offset, 4), *pw = bytes(vbw, (uint64_t)bow + x * stw + aw.offset, 4);
        if (!pp || !pi || !pw) { fetch_why = "vertex bytes unknown"; return nullptr; }
        double f[3]; if (fmt == 3) { for (int c = 0; c < 3; c++) { int16_t h; std::memcpy(&h, pp + 2 * c, 2); f[c] = std::max(-1.0, (double)h / 32767.0); } } else { float g[3]; std::memcpy(g, pp, 12); for (int c = 0; c < 3; c++) f[c] = (double)g[c]; }
        for (int c = 0; c < 3; c++) if (!(f[c] == f[c])) { fetch_why = "NaN position"; return nullptr; }
        for (int k = 0; k < 4; k++) { if (!pw[k]) continue; uint32_t bone = pi[k]; if (bone >= 70) { fetch_why = "bone index >= 70"; return nullptr; } auto &bx = sb.b[bone]; sb.used[bone] = 1;
          for (int c = 0; c < 3; c++) { bx[c] = std::min(bx[c], f[c]); bx[3 + c] = std::max(bx[3 + c], f[c]); } }
        nv++; }
      if (!nv) { fetch_why = "no vertices"; return nullptr; }
      if (skinboxes.size() >= 4096) skinboxes.clear(); return &(skinboxes[key] = sb); }
    /* the world-space corners (x y z 1) of every used bone's decoded box, skinned by that bone: bones = cb3 rows (3i, 3i+1, 3i+2), world = q * cb0 rows 0..3 */
    static bool skin_points(const SkinBoxes &sb, const float *scale, const float *bias, const float *bones, uint32_t bones_have_regs, const float *l2w, std::vector<double> &out) {
      out.clear();
      for (uint32_t i = 0; i < 70; i++) { if (!sb.used[i]) continue; if (3 * i + 3 > bones_have_regs) return false; const float *R0 = bones + 12 * i, *R1 = R0 + 4, *R2 = R0 + 8; const auto &bx = sb.b[i];
        for (int c = 0; c < 8; c++) { double p[4] = { bx[c & 1 ? 3 : 0] * (double)scale[0] + (double)bias[0], bx[c & 2 ? 4 : 1] * (double)scale[1] + (double)bias[1], bx[c & 4 ? 5 : 2] * (double)scale[2] + (double)bias[2], 1.0 };
          double q[4] = { 0, 0, 0, 1 }; for (int k = 0; k < 4; k++) { q[0] += p[k] * (double)R0[k]; q[1] += p[k] * (double)R1[k]; q[2] += p[k] * (double)R2[k]; }
          for (int k = 0; k < 4; k++) { double w = q[0] * (double)l2w[0 * 4 + k] + q[1] * (double)l2w[1 * 4 + k] + q[2] * (double)l2w[2 * 4 + k] + q[3] * (double)l2w[3 * 4 + k]; if (!(w == w)) return false; out.push_back(w); } } }
      return !out.empty(); }
    /* a clear quad covers the whole viewport: every vertex (w = 1) a corner of the +-1 square and two triangles that share a diagonal */
    static const char *cover_why;
    static bool cover(const std::vector<double> &v, uint32_t topo, const RS *rs) {
      size_t n = v.size() / 4; cover_why = "fewer than 3 vertices"; if (n < 3) return false; std::vector<int> c(n);
      cover_why = "a vertex is not a corner of the +-1 square with w = 1";
      for (size_t i = 0; i < n; i++) { double x = v[4 * i], y = v[4 * i + 1], w = v[4 * i + 3]; if (std::fabs(std::fabs(x) - 1.0) > 1e-5 || std::fabs(std::fabs(y) - 1.0) > 1e-5 || std::fabs(w - 1.0) > 1e-6) return false; c[i] = (x > 0 ? 1 : 0) + (y < 0 ? 2 : 0); }
      VkCullModeFlags cull = rs ? rs->s.cullMode() : VK_CULL_MODE_BACK_BIT; bool ccw_front = rs ? rs->s.frontFace() == VK_FRONT_FACE_COUNTER_CLOCKWISE : false; bool culled = false;
      std::vector<int> tri; auto add = [&](size_t a, size_t b, size_t d) { int m = (1 << c[a]) | (1 << c[b]) | (1 << c[d]); if (c[a] == c[b] || c[b] == c[d] || c[a] == c[d]) return;
        if (cull != VK_CULL_MODE_NONE) {   /* orientation in pixel space (y down): the NDC signed area negated; front = CW unless FrontCounterClockwise */
          double ar = (v[4 * b] - v[4 * a]) * (v[4 * d + 1] - v[4 * a + 1]) - (v[4 * d] - v[4 * a]) * (v[4 * b + 1] - v[4 * a + 1]); if (topo == 5 && ((b - a) & 1)) ar = -ar;   /* strip: odd triangles flip */
          bool ccw_px = ar < 0, front = ccw_px == ccw_front; if ((cull & VK_CULL_MODE_FRONT_BIT) && front) { culled = true; return; } if ((cull & VK_CULL_MODE_BACK_BIT) && !front) { culled = true; return; } }
        tri.push_back(m); };
      cover_why = "topology is not a triangle list / strip";
      if (topo == 4) { for (size_t i = 0; i + 2 < n; i += 3) add(i, i + 1, i + 2); } else if (topo == 5) { for (size_t i = 0; i + 2 < n; i++) add(i, i + 1, i + 2); } else return false;
      cover_why = culled ? "a covering triangle is culled" : "no two triangles sharing a diagonal";
      for (size_t i = 0; i < tri.size(); i++) for (size_t j = i + 1; j < tri.size(); j++) { int s = tri[i] & tri[j]; if ((tri[i] | tri[j]) == 15 && (s == 9 || s == 6)) return true; }
      return false; }
    static bool st_true(const DxvkStencilOp &op, uint32_t ref, uint32_t val) {
      uint32_t m = op.compareMask(), r = ref & m, s = val & m;
      switch (op.compareOp()) { case VK_COMPARE_OP_ALWAYS: return true; case VK_COMPARE_OP_EQUAL: return r == s; case VK_COMPARE_OP_NOT_EQUAL: return r != s; case VK_COMPARE_OP_LESS: return r < s;
        case VK_COMPARE_OP_LESS_OR_EQUAL: return r <= s; case VK_COMPARE_OP_GREATER: return r > s; case VK_COMPARE_OP_GREATER_OR_EQUAL: return r >= s; default: return false; } }
    static bool op_zeroing(VkStencilOp o, uint32_t ref, uint32_t wm) { return o == VK_STENCIL_OP_ZERO || (o == VK_STENCIL_OP_REPLACE && (ref & wm) == 0); }   /* leaves a 0 at 0 */
    static bool face_grows(const DxvkStencilOp &f, uint32_t ref) { uint32_t wm = f.writeMask(); if (!wm) return false;
      VkStencilOp o[3] = { f.failOp(), f.depthFailOp(), f.passOp() }; for (VkStencilOp x : o) if (x != VK_STENCIL_OP_KEEP && !op_zeroing(x, ref, wm)) return true; return false; }
    /* z-fail counting on this face: stencil written on depth fail only (depth test on, less/greater compare, no depth write: checked by the caller) */
    static bool face_zfail(const DxvkStencilOp &f) { return f.writeMask() == 0 || (f.failOp() == VK_STENCIL_OP_KEEP && f.passOp() == VK_STENCIL_OP_KEEP && f.depthFailOp() != VK_STENCIL_OP_KEEP); }
    static bool cmp_ordered(VkCompareOp o) { return o == VK_COMPARE_OP_LESS || o == VK_COMPARE_OP_LESS_OR_EQUAL || o == VK_COMPARE_OP_GREATER || o == VK_COMPARE_OP_GREATER_OR_EQUAL; }
    /* every op that can fire writes 0 (func ALWAYS: no fail op; depth test off / ALWAYS: no depth-fail op) with the full write mask */
    static bool face_clears(const DxvkStencilOp &f, uint32_t ref, bool depth_can_fail) { if (f.writeMask() != 0xffu) return false;
      if (!op_zeroing(f.passOp(), ref, 0xffu)) return false; if (f.compareOp() != VK_COMPARE_OP_ALWAYS && !op_zeroing(f.failOp(), ref, 0xffu)) return false;
      if (depth_can_fail && !op_zeroing(f.depthFailOp(), ref, 0xffu)) return false; return true; }
    static void dss_str(char *b, size_t n, const DSS *d, uint32_t ref) {   /* diagnostics */
      if (!d) { std::snprintf(b, n, "default DSS"); return; } DxvkStencilOp f = d->s.stencilOpFront(), k = d->s.stencilOpBack();
      std::snprintf(b, n, "stencil %d front fail/zfail/pass %d/%d/%d func %d rm %02x wm %02x back %d/%d/%d func %d rm %02x wm %02x ref %u, depth test %d write %d cmp %d",
                    d->s.stencilTest(), (int)f.failOp(), (int)f.depthFailOp(), (int)f.passOp(), (int)f.compareOp(), f.compareMask(), f.writeMask(), (int)k.failOp(), (int)k.depthFailOp(), (int)k.passOp(), (int)k.compareOp(), k.compareMask(), k.writeMask(), ref,
                    d->s.depthTest(), d->s.depthWrite(), (int)d->s.depthCompareOp()); }
    static std::string shkey(uint32_t id) { char b[64]; if (!id || id == BC_UNK) return "-"; if (!b1dx_shader_name(id, b, sizeof b)) return "?" + std::to_string(id); return b; }

    /* ---- CHECK mode (BSI_B1_SHBBOX=2): the unscissored quad into a copy of the target, both read back and compared 2 frames later ---- */
    struct Chk { Rc<DxvkImage> tmp, rt; Rc<DxvkImageView> rtv; VkFormat fmt = VK_FORMAT_UNDEFINED; VkExtent3D ext = { }; Rc<DxvkBuffer> ra, rb; long frame = -1; uint32_t esz = 0; bool pending = false;
                 long n_runs, n_bad_runs, tex_bad, tex_tot, n_skip; } chk;
    static bool check_frame_now() { return mode() == 2 && check_every() > 0 && (g_frames % check_every()) == 0; }
    static Rc<DxvkBuffer> readback(const Rc<DxvkImage> &img, VkFormat fmt, uint32_t esz) {
      VkExtent3D e = img->mipLevelExtent(0); DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)e.width * e.height * esz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 sb87 check readback";
      Rc<DxvkBuffer> rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      emit([rb, fmt, img, sl, e] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, e); });
      return rb; }
    static void check_pre(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {   /* before the scissor is armed: the plain viewports are the emitted ones */
      if (chk.pending || chk.ra != nullptr) return;
      View *rv = nullptr; int nc = 0; for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2 && v->iv != nullptr) { nc++; if (!rv) rv = v; } }
      if (nc != 1 || !rv || rv->iv->info().mipIndex || rv->iv->info().layerIndex) { chk.n_skip++; return; }
      try {
        Rc<DxvkImage> rt = rv->iv->image(); VkExtent3D e = rt->mipLevelExtent(0); VkFormat fmt = rt->info().format;
        const DxvkFormatInfo *fi = lookupFormatInfo(fmt); if (!fi || !fi->elementSize || (fi->aspectMask & ~VK_IMAGE_ASPECT_COLOR_BIT)) { chk.n_skip++; return; }
        if (chk.tmp == nullptr || chk.fmt != fmt || chk.ext.width != e.width || chk.ext.height != e.height) {
          DxvkImageCreateInfo ii = rt->info(); ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
          ii.mipLevels = 1; ii.numLayers = 1; ii.flags = 0; ii.viewFormatCount = 0; ii.viewFormats = nullptr; ii.debugName = "B1 sb87 check copy";
          ii.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; ii.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
          ii.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; chk.tmp = g_dev->createImage(ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = fmt; k.aspects = VK_IMAGE_ASPECT_COLOR_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1;
          k.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; k.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; chk.rtv = chk.tmp->createView(k); chk.fmt = fmt; chk.ext = e; }
        VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        emit([di = chk.tmp, si = rt, sl, e] (DxvkContext* c) { c->copyImage(di, sl, VkOffset3D{ 0, 0, 0 }, si, sl, VkOffset3D{ 0, 0, 0 }, e); });
        DxvkRenderTargets real = { }; for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) real.color[i].view = v->iv; }
        { View *d = g_views.get(g_cs.dsv); if (d && d->type == 3) real.depth.view = d->iv; }
        DxvkRenderTargets alt = real; alt.color[0].view = chk.rtv;
        emit([alt] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(alt), 0u); });
        if (dc) { VkDrawIndirectCommand x = *dc; emit([x] (DxvkContext* c) { c->draw(1, &x); }); } else { VkDrawIndexedIndirectCommand x = *dic; emit([x] (DxvkContext* c) { c->drawIndexed(1, &x); }); }
        emit([real] (DxvkContext* c) mutable { c->bindRenderTargets(std::move(real), 0u); });
        chk.rt = rt; chk.esz = fi->elementSize; chk.pending = true;
      } catch (const DxvkError &e) { chk.n_skip++; std::fprintf(stderr, "[b1sb] check: setup failed: %s\n", e.message().c_str()); } }
    static void check_post() {   /* after the scissored draw: both images to host buffers */
      if (!chk.pending) return; chk.pending = false;
      try { chk.ra = readback(chk.tmp, chk.fmt, chk.esz); chk.rb = readback(chk.rt, chk.fmt, chk.esz); chk.frame = g_frames; }
      catch (const DxvkError &e) { chk.ra = chk.rb = nullptr; chk.n_skip++; std::fprintf(stderr, "[b1sb] check: readback failed: %s\n", e.message().c_str()); }
      chk.rt = nullptr; }
    static void check_frame() {
      if (chk.ra == nullptr || chk.rb == nullptr || g_frames < chk.frame + 2) return;
      ctx_flush(); cs_sync(); g_dev->waitForIdle();
      const uint8_t *a = (const uint8_t *)chk.ra->mapPtr(0), *b = (const uint8_t *)chk.rb->mapPtr(0); long n = (long)chk.ext.width * chk.ext.height, bad = 0;
      long bad1 = 0, badn = 0; int maxd = 0;
      if (a && b) for (long t = 0; t < n; t++) if (std::memcmp(a + t * chk.esz, b + t * chk.esz, chk.esz)) { bad++; int m = 0; for (uint32_t k = 0; k < chk.esz; k++) m = std::max(m, std::abs((int)a[t * chk.esz + k] - (int)b[t * chk.esz + k])); if (m <= 1) bad1++; else badn++; maxd = std::max(maxd, m); }
      chk.n_runs++; if (bad) chk.n_bad_runs++; chk.tex_bad += bad; chk.tex_tot += n;
      std::fprintf(stderr, "[b1sb] check: frame %ld: %ld texels differ (of %ld; by 1 level %ld, by more %ld, max %d); runs %ld, bad runs %ld, texels differing %ld of %ld\n", chk.frame, bad, n, bad1, badn, maxd, chk.n_runs, chk.n_bad_runs, chk.tex_bad, chk.tex_tot);
      chk.ra = chk.rb = nullptr; }

    /* ---- b102: TILED projection quads (BSI_B1_SHTILE=1, default 0; NOT bit-exact: the tiles' interpolated PS inputs differ from the full quad's at the
       ulp level = INVISIBLE tier, measured by the CHECK mode). Before each projection quad: a compute pass reads the stencil aspect of the bound
       depth-stencil image and, per tile of the eye rect, appends 6 indices for every tile where some pixel passes the quad's stencil test, plus the
       indirect draw's index count; the quad is then drawn with the game's VS / PS / state but our tile vertex buffer (the quad's z and w, the
       tile corners in NDC) and the GPU-built index list via drawIndexedIndirect. The render pass ends before the compute pass (measured b100:
       ~17 us each). b108: the UE3 projection draws are the light FRUSTUM (36 vertices, vs.3863988d: clip = v * VP, o1 = ScreenToTranslatedWorld(clip)), so
       the tiles use our own VS (tile.vert, same outputs from clip-space tile corners) with culling off; requirement: the projection VS is on
       BSI_B1_SHTILE_VS (default 3863988d), a sampled stencil aspect, position float2/3/4; otherwise the plain draw (counted). ---- */
#include "rhi_b1_sb87_spv.h"
    static int tile() { static int v = -1; if (v < 0) v = env_i("BSI_B1_SHTILE", 0) > 0; return v; }
    static int tile_px() { static int v = -1; if (v < 0) { v = env_i("BSI_B1_SHTILE_PX", 32); if (v < 8) v = 8; if (v > 128) v = 128; } return v; }
    static const uint32_t T_CBS = 5u * 16u + 13u, T_UAV = 128u * 6u + 64u * 2u + 61u /* kUavCompute + 61 (declared later in rhi_b1_dxvk.cpp) */, T_SRV = 5u * 128u + 126u, T_SLOT = 32768u, T_SLOTS = 256u, T_MAXT = 1360u;
    static Rc<DxvkShader> tcs, tvs; static Rc<DxvkBuffer> tring; static bool t_tried, t_ok; static uint32_t t_slot;
    static bool hash_listed_tile(const char *h) { static std::string l = [] { const char *e = std::getenv("BSI_B1_SHTILE_VS"); return std::string(e && *e ? e : "3863988d"); }();   /* b108: the projection VS the tile VS replicates */
      size_t p = 0; while (p < l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q - p == 8 && !l.compare(p, 8, h, 8)) return true; p = q + 1; } return false; }
    struct TVb { Rc<DxvkBuffer> vb; int tx = 0, ty = 0; };
    static std::unordered_map<uint64_t, TVb> tvbs; static std::unordered_map<uint32_t, Rc<DxvkImageView>> tstv;
    static long n_tile, n_tile_fb, n_tile_rep; static std::map<std::string, long> tile_fb_why;
    /* b110: lit-tile statistics: every 16th tiled draw copies its counter (lit tiles) to a host ring, read 4 frames later */
    struct TStat { Rc<DxvkBuffer> rb; uint32_t total = 0; long frame = -1; bool used = false; };
    static TStat tstat[32]; static uint32_t tstat_i; static long ts_n, ts_lit, ts_total; static long ts_hist[11]; static uint32_t t_n_emit;
    static void tstat_read() { for (auto &t : tstat) { if (!t.used || t.rb == nullptr || g_frames < t.frame + 4) continue; const uint32_t *p = (const uint32_t *)t.rb->mapPtr(0); if (p) { uint32_t lit = p[0]; if (lit > t.total) lit = t.total; ts_n++; ts_lit += lit; ts_total += t.total; int b = t.total ? (int)(lit * 10 / t.total) : 0; ts_hist[b < 10 ? b : 10]++; } t.used = false; } }
    static bool t_armed; static uint32_t t_params[16]; static uint32_t t_slotoff, t_vbslot, t_stride, t_ntiles; static Rc<DxvkBuffer> t_vb; static Rc<DxvkImageView> t_stv; static int t_tx, t_ty;
    static bool have_vp_ok() { return g_cs.nvp && g_cs.vp[0][2] > 0.f && g_cs.vp[0][3] > 0.f; }
    static bool tile_setup() {
      if (t_tried) return t_ok; t_tried = true;
      try {
        DxvkBindingInfo ub = { }; ub.set = 0; ub.binding = 0; ub.resourceIndex = T_CBS; ub.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ub.descriptorCount = 1; ub.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM; ub.access = VK_ACCESS_UNIFORM_READ_BIT; ub.flags.set(DxvkDescriptorFlag::UniformBuffer);
        DxvkBindingInfo sb = { }; sb.set = 0; sb.binding = 1; sb.resourceIndex = T_UAV; sb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; sb.descriptorCount = 1; sb.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM; sb.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        DxvkBindingInfo tb = { }; tb.set = 0; tb.binding = 2; tb.resourceIndex = T_SRV; tb.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; tb.descriptorCount = 1; tb.viewType = VK_IMAGE_VIEW_TYPE_2D; tb.access = VK_ACCESS_SHADER_READ_BIT;
        std::vector<DxvkBindingInfo> b = { ub, sb, tb };
        DxvkSpirvShaderCreateInfo ci = { }; ci.bindingCount = (uint32_t)b.size(); ci.bindings = b.data(); ci.debugName = "sb87 tile mask";
        tcs = new DxvkSpirvShader(ci, SpirvCodeBuffer(sizeof sb87_tile_comp / 4, sb87_tile_comp)); tcs->getShaderMetadata();
        DxvkBindingInfo vb0 = { }; vb0.set = 0; vb0.binding = 0; vb0.resourceIndex = 1u * 16u + 0u; vb0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; vb0.descriptorCount = 1; vb0.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM; vb0.access = VK_ACCESS_UNIFORM_READ_BIT; vb0.flags.set(DxvkDescriptorFlag::UniformBuffer);   /* the game's VS cb0 (DXVK slot: stage 1 (vertex) * 16 + 0) */
        std::vector<DxvkBindingInfo> vbs = { vb0 }; DxvkSpirvShaderCreateInfo vi = { }; vi.bindingCount = 1; vi.bindings = vbs.data(); vi.debugName = "sb87 tile vertex";
        tvs = new DxvkSpirvShader(vi, SpirvCodeBuffer(sizeof sb87_tile_vert / 4, sb87_tile_vert)); tvs->getShaderMetadata();
        DxvkBufferCreateInfo bi = { }; bi.size = (VkDeviceSize)T_SLOT * T_SLOTS;
        bi.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        bi.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "sb87 tile ring";
        tring = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        t_ok = true; std::fprintf(stderr, "[b1sb] SHTILE: tiled projection quads ON (NOT bit-exact: INVISIBLE tier), tile %d px, ring %u x %u B, shader %zu words\n", tile_px(), T_SLOTS, T_SLOT, sizeof sb87_tile_comp / 4);
      } catch (const DxvkError &e) { std::fprintf(stderr, "[b1sb] SHTILE setup failed: %s (plain quads)\n", e.message().c_str()); }
      return t_ok; }
    static void tile_fallback(const char *why) { n_tile_fb++; if (tile_fb_why.size() < 32 || tile_fb_why.count(why)) tile_fb_why[why]++; t_armed = false; }
    /* the tile vertex buffer for (eye rect, viewport, stride, position offset / components, z, w): 4 vertices per tile in the bound layout */
    static TVb *tile_vb(const Rect &eo, const float *vp, uint32_t stride, uint32_t aoff, int nc, float z, float w) {
      int T = tile_px(); int tx = (eo.x1 - eo.x0 + T - 1) / T, ty = (eo.y1 - eo.y0 + T - 1) / T; if (tx <= 0 || ty <= 0 || (uint32_t)(tx * ty) > T_MAXT) return nullptr;
      uint64_t key = 1469598103934665603ull; uint32_t zb, wb; std::memcpy(&zb, &z, 4); std::memcpy(&wb, &w, 4);
      for (uint64_t x : { (uint64_t)(uint32_t)eo.x0, (uint64_t)(uint32_t)eo.y0, (uint64_t)(uint32_t)eo.x1, (uint64_t)(uint32_t)eo.y1, (uint64_t)(uint32_t)vp[0], (uint64_t)(uint32_t)vp[1], (uint64_t)(uint32_t)vp[2], (uint64_t)(uint32_t)vp[3], (uint64_t)stride, (uint64_t)aoff, (uint64_t)nc, (uint64_t)zb, (uint64_t)wb, (uint64_t)T }) key = (key ^ x) * 1099511628211ull;
      auto it = tvbs.find(key); if (it != tvbs.end()) return &it->second;
      if (tvbs.size() >= 64) tvbs.clear();
      size_t nv = (size_t)tx * ty * 4, bytes = nv * stride; std::vector<uint8_t> data(bytes, 0);
      for (int j = 0; j < ty; j++) for (int i = 0; i < tx; i++) { double px0 = eo.x0 + (double)i * T, px1 = std::min<double>(px0 + T, eo.x1), py0 = eo.y0 + (double)j * T, py1 = std::min<double>(py0 + T, eo.y1);
        double cx[4] = { px0, px1, px1, px0 }, cy[4] = { py0, py0, py1, py1 };
        for (int k = 0; k < 4; k++) { float v[4] = { (float)((cx[k] - vp[0]) / vp[2] * 2.0 - 1.0), (float)(1.0 - (cy[k] - vp[1]) / vp[3] * 2.0), z, w }; std::memcpy(&data[((size_t)(j * tx + i) * 4 + k) * stride + aoff], v, 4u * nc); } }
      TVb t; t.tx = tx; t.ty = ty;
      DxvkBufferCreateInfo bi = { }; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "sb87 tile vertices";
      t.vb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      Rc<DxvkResourceAllocation> a = dyn_storage(*t.vb); std::memcpy(a->mapPtr(), data.data(), bytes);
      emit([b = t.vb, a] (DxvkContext* c) mutable { c->invalidateBuffer(b, std::move(a)); });
      return &(tvbs[key] = t); }
    /* arm the tiled draw for the current quad (left eye: draw_pre; right eye: replay_pre with the shifted viewport). cur.v = the quad's vertices */
    static void tile_arm(const Rect &eo, const DSS *d, uint32_t ref, bool replay) {
      t_armed = false; if (!tile() || !tile_setup() || !have_vp_ok()) return;
      float z = 0.5f, w = 1.0f;   /* b108: the tile quads are given in clip space to our own VS (tile.vert replicates vs.3863988d); the game's frustum geometry is not used */
      uint32_t vs = g_bc.sh[0] == BC_UNK ? 0 : g_bc.sh[0]; { char hb[64]; if (!vs || !b1dx_shader_name(vs, hb, sizeof hb) || std::strlen(hb) < 11 || !hash_listed_tile(hb + 3)) return tile_fallback("projection VS not on the tile list (BSI_B1_SHTILE_VS)"); }
      VsI vi = vs_info(vs); if (vi.pos_reg < 0) return tile_fallback("VS POSITION register unknown");
      DxvkStencilOp f = d->s.stencilOpFront(), b = d->s.stencilOpBack(); if (f.compareOp() != b.compareOp() || f.compareMask() != b.compareMask()) return tile_fallback("front / back stencil tests differ");
      uint32_t ilid = g_bc.il; if (!ilid || ilid == BC_UNK) return tile_fallback("no input layout"); IL *il = g_ils.get(ilid); if (!il || !il->na) return tile_fallback("no input layout");
      DxvkVertexAttribute a; uint32_t vb, st, bo; if (!attr_of(il, vi.pos_reg, a, vb, st, bo)) return tile_fallback("position attribute / buffer");
      int nc = a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : a.format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a.format == VK_FORMAT_R32G32_SFLOAT ? 2 : 0; if (!nc) return tile_fallback("position format");
      if (a.offset + 4u * nc > st) return tile_fallback("position outside the stride");
      View *dv = g_views.get(g_cs.dsv); Obj *o = dv && dv->type == 3 ? obj(dv->res) : nullptr; if (!o || o->kind != 2 || o->img == nullptr) return tile_fallback("no depth-stencil image");
      Rc<DxvkImageView> stv; { auto it = tstv.find(dv->res); if (it != tstv.end()) stv = it->second; else {
          const DxvkImageCreateInfo &ii = o->img->info(); if (!(ii.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(lookupFormatInfo(ii.format)->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)) return tile_fallback("depth-stencil image not sampleable / no stencil");
          try { DxvkImageViewKey k = { }; k.viewType = VK_IMAGE_VIEW_TYPE_2D; k.format = ii.format; k.aspects = VK_IMAGE_ASPECT_STENCIL_BIT; k.mipIndex = 0; k.mipCount = 1; k.layerIndex = 0; k.layerCount = 1; k.usage = VK_IMAGE_USAGE_SAMPLED_BIT; k.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            stv = o->img->createView(k); tstv[dv->res] = stv; } catch (const DxvkError &e) { return tile_fallback("stencil view creation failed"); } } }
      TVb *tv = tile_vb(eo, g_cs.vp[0], st, a.offset, nc, z, w); if (!tv) return tile_fallback("tile grid too large / empty");
      t_params[0] = (uint32_t)eo.x0; t_params[1] = (uint32_t)eo.y0; t_params[2] = (uint32_t)eo.x1; t_params[3] = (uint32_t)eo.y1; t_params[4] = (uint32_t)tv->tx; t_params[5] = (uint32_t)tv->ty; t_params[6] = (uint32_t)tile_px(); t_params[7] = 0;
      t_params[8] = (uint32_t)f.compareOp(); t_params[9] = ref; t_params[10] = f.compareMask(); t_params[11] = 0;
      { static int probe = -1; if (probe < 0) probe = env_i("BSI_B1_SHTILE_PROBE", 0); if (probe == 1) t_params[8] = 0u; /* b110 probe: no tile passes (NEVER): the overhead floor; the mask goes dark */ else if (probe == 2) t_params[8] = 7u; /* ALWAYS: every tile drawn */ } t_params[12] = t_params[13] = t_params[14] = t_params[15] = 0;
      t_vb = tv->vb; t_vbslot = a.binding; t_stride = st; t_tx = tv->tx; t_ty = tv->ty; t_ntiles = (uint32_t)(tv->tx * tv->ty); t_stv = stv; t_slotoff = (t_slot++ % T_SLOTS) * T_SLOT; t_armed = true; n_tile++; if (replay) n_tile_rep++; }
    /* the armed tiled draw instead of the game's draw (called from the draw paths and from pp76's replay); restores the game's VB / IB bindings */
    bool tile_pending() { return t_armed; }
    void tile_emit() {
      t_armed = false;
      struct Pk { uint32_t p[16]; uint32_t ca[8]; } pk; std::memcpy(pk.p, t_params, 64); pk.ca[0] = 0; pk.ca[1] = pk.ca[2] = pk.ca[3] = 0; pk.ca[4] = 0; pk.ca[5] = 1; pk.ca[6] = 0; pk.ca[7] = 0;   /* counter; args: indexCount 0, instanceCount 1, firstIndex 0 (vertexOffset / firstInstance = 0 are written below) */
      uint32_t slot = t_slotoff, ntiles = t_ntiles, vbslot = t_vbslot, stride = t_stride, tx = (uint32_t)t_tx, ty = (uint32_t)t_ty;
      DxvkBufferSlice gvb; uint32_t gst = 0; { uint32_t id = g_bc.vb[vbslot]; Obj *o = id && id != BC_UNK ? obj(id) : nullptr; if (o && o->kind == 1 && g_bc.vbo[vbslot] != BC_UNK && g_bc.vbo[vbslot] < o->buf->info().size) { gvb = DxvkBufferSlice(o->buf, g_bc.vbo[vbslot], o->buf->info().size - g_bc.vbo[vbslot]); gst = g_bc.vbs[vbslot]; } }
      DxvkBufferSlice gib; VkIndexType git = VK_INDEX_TYPE_UINT16; { uint32_t id = g_bc.ib; Obj *o = id && id != BC_UNK ? obj(id) : nullptr; if (o && o->kind == 1 && g_bc.ibo != BC_UNK && g_bc.ibo < o->buf->info().size) { gib = DxvkBufferSlice(o->buf, g_bc.ibo, o->buf->info().size - g_bc.ibo); git = (VkIndexType)g_bc.ibf; } }
      Rc<DxvkShader> gcs = g_cur_cs ? b1dx_shader_get(g_cur_cs) : nullptr; Rc<DxvkShader> gvs = (g_bc.sh[0] && g_bc.sh[0] != BC_UNK) ? b1dx_shader_get(g_bc.sh[0]) : nullptr;
      RS *rsp = g_rss.get(g_cs.rs); DxvkRasterizerState rs0 = rsp ? rsp->s : default_rs(), rs1 = rs0; rs1.setCullMode(VK_CULL_MODE_NONE);   /* b108: the tile quads have one face */
      DxvkBufferViewKey vk = { }; vk.format = VK_FORMAT_R32_UINT; vk.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; vk.offset = slot + 64u; vk.size = T_SLOT - 64u;
      Rc<DxvkBufferView> uv = tring->createView(vk);
      emit([pk, slot, ntiles, vbslot, stride, tx, ty, gvb = std::move(gvb), gst, gib = std::move(gib), git, gcs = std::move(gcs), gvs = std::move(gvs), uv = std::move(uv), stv = t_stv, vb = t_vb, cs = tcs, vs = tvs, rs0, rs1, ring = tring] (DxvkContext* c) mutable {
        uint32_t zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        c->updateBuffer(ring, slot, 64, pk.p); c->updateBuffer(ring, slot + 64, 32, pk.ca); c->updateBuffer(ring, slot + 96, 32, zero);   /* args firstInstance (slot + 96) = 0 */
        c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(cs));
        c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, T_CBS, DxvkBufferSlice(ring, slot, 64));
        c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, T_UAV, std::move(uv));
        c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, T_SRV, std::move(stv));
        c->dispatch(tx, ty, 1);
        c->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(std::move(gcs));
        c->bindUniformBuffer(VK_SHADER_STAGE_COMPUTE_BIT, T_CBS, DxvkBufferSlice()); c->bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT, T_UAV, nullptr); c->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, T_SRV, nullptr);
        c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(vs)); c->setRasterizerState(rs1);
        c->bindVertexBuffer(vbslot, DxvkBufferSlice(vb), stride);
        c->bindIndexBuffer(DxvkBufferSlice(ring, slot + 128u, (VkDeviceSize)ntiles * 24u), VK_INDEX_TYPE_UINT32);
        c->bindDrawBuffers(DxvkBufferSlice(ring, slot + 80u, 20u), DxvkBufferSlice());
        c->drawIndexedIndirect(0, 1, 20, false);
        c->bindDrawBuffers(DxvkBufferSlice(), DxvkBufferSlice());
        c->bindVertexBuffer(vbslot, std::move(gvb), gst); c->bindIndexBuffer(std::move(gib), git); c->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(std::move(gvs)); c->setRasterizerState(rs0); });
      if ((++t_n_emit & 15u) == 0u) { TStat &t = tstat[tstat_i++ & 31u]; if (!t.used) { if (t.rb == nullptr) { DxvkBufferCreateInfo bi = { }; bi.size = 16; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT; bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "sb87 tile stats";
            t.rb = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
          t.total = ntiles; t.frame = g_frames; t.used = true; emit([rb = t.rb, ring = tring, slot] (DxvkContext* c) { c->copyBuffer(rb, 0, ring, slot + 64u, 8); }); } } }
    static void tile_released(uint32_t res) { if (!tstv.empty()) tstv.erase(res); }
    static bool clr_pending; static VkOffset3D clr_off; static VkExtent3D clr_ext; static long n_clr, n_clr_rep, n_clr_no; static std::map<std::string, long> clr_why;
    static void clr_reject(const char *w) { n_clr_no++; if (clr_why.size() < 16 || clr_why.count(w)) clr_why[w]++; }
    /* b112: the quad only zeroes the stencil inside the effective rect: no colour write on any target, no depth write, the stencil ops leave 0
       everywhere (face_clears), the rect pixel-aligned (outward == inward), a stencil aspect on the bound depth-stencil view */
    static void clr_arm(const DSS *d, const Rect &eo, const Rect &ei) {
      clr_pending = false; if (!shclr()) return;
      if (d->s.depthWrite()) return clr_reject("depth write");
      BS *bs = g_bss.get(g_cs.bs); for (uint32_t i = 0; i < 8; i++) { View *v = g_views.get(g_cs.rtv[i]); if (!v || v->type != 2 || v->iv == nullptr) continue; if (!bs || bs->m[i].writeMask() != 0) return clr_reject("colour write"); }
      if (eo.x0 != ei.x0 || eo.y0 != ei.y0 || eo.x1 != ei.x1 || eo.y1 != ei.y1 || ei.empty()) return clr_reject("rect not pixel-aligned / empty");
      View *dv = g_views.get(g_cs.dsv); if (!dv || dv->type != 3 || dv->iv == nullptr || !(dv->iv->info().aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) return clr_reject("no stencil view");
      clr_off = { ei.x0, ei.y0, 0 }; clr_ext = { (uint32_t)(ei.x1 - ei.x0), (uint32_t)(ei.y1 - ei.y0), 1u }; clr_pending = true; }
    bool clr_take() { return clr_pending; }
    void clr_emit() { clr_pending = false; View *dv = g_views.get(g_cs.dsv); if (!dv || dv->iv == nullptr) return; VkClearValue cv = { }; cv.depthStencil.depth = 0.f; cv.depthStencil.stencil = 0;
      emit([iv = dv->iv, o = clr_off, e = clr_ext, cv] (DxvkContext* c) { c->clearImageView(iv, o, e, VK_IMAGE_ASPECT_STENCIL_BIT, cv); }); n_clr++; }
    /* ---- the draw hooks ---- */
    void apply_vp();   /* apply_viewports (rhi_b1_dxvk.cpp, after this file) */
    static bool db_armed;
    static bool db_feature() { static int v = -1; if (v < 0) { v = g_dev != nullptr && g_dev->features().core.features.depthBounds ? 1 : 0; if (!v && shdb()) std::fprintf(stderr, "[b1sb] depth bounds: the device does not report depthBounds: off\n"); } return v != 0; }
    static void db_set(float lo, float hi) { DxvkDepthBounds db = { lo, hi }; emit([db] (DxvkContext* c) { c->setDepthBounds(db); }); }
    static void db_arm(const Rect &g) {   /* the depth-bounds test of the projection quad from the depth range of the rects it covers */
      if (!shdb() || !db_feature()) return;
      if (!g.zok || !(g.zlo <= g.zhi)) { n_nodb++; return; }
      double lo = g.zlo - 1e-5 - std::fabs(g.zlo) * 4.8e-7, hi = g.zhi + 1e-5 + std::fabs(g.zhi) * 4.8e-7;   /* 1e-5 absolute + ~4 ulp relative */
      float flo = (float)std::max(0.0, std::min(1.0, lo)), fhi = (float)std::max(0.0, std::min(1.0, hi)); if (flo > (float)lo) flo = std::nextafter(flo, 0.f); if (fhi < (float)hi) fhi = std::nextafter(fhi, 1.f);
      if (flo < 0.f) flo = 0.f; if (fhi > 1.f) fhi = 1.f;
      if (!(flo > 0.f) && !(fhi < 1.f)) { n_nodb++; return; }   /* the whole range: nothing to test */
      db_set(flo, fhi); db_armed = true; n_db++; zfrac_sum += (double)(fhi - flo); }
    static void db_disarm() { if (db_armed) { db_armed = false; db_set(0.f, 1.f); } }
    static void arm_rect(uint32_t res, const Rect &eo, bool have_eff, const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic, bool replay) {
      Rect r;
      if (mode() == 3) {   /* NOT exact: measurement of the quads' raster cost */
        static bool said; if (!said) { said = true; std::fprintf(stderr, "[b1sb] BSI_B1_SHBBOX=3: NOT EXACT, every projection quad scissored to 2x2 px (measurement only)\n"); }
        if (!have_eff) return; r = Rect { eo.x0, eo.y0, eo.x0 + 2, eo.y0 + 2 }; }
      else if (mode() == 5) {   /* b98 NOT exact: every quad scissored to the centred quarter (25 % of the area): raster-proportional cost vs per-quad fixed cost */
        static bool said; if (!said) { said = true; std::fprintf(stderr, "[b1sb] BSI_B1_SHBBOX=5: NOT EXACT, every projection quad scissored to the centred 25 %% area (measurement only)\n"); }
        if (!have_eff) return; int w = eo.x1 - eo.x0, h = eo.y1 - eo.y0; r = Rect { eo.x0 + w / 4, eo.y0 + h / 4, eo.x0 + w / 4 + w / 2, eo.y0 + h / 4 + h / 2 }; }
      else if (!have_eff || !quad_rect(res, eo, r)) { n_proj_full++; if (replay) n_rep_proj_not++; if (have_eff) { c_eye_px = (double)eo.area(); census_begin(cur.pk, replay, false, 1.0); } return; }
      if (!replay && check_frame_now()) check_pre(dc, dic);
      { double fr0 = eo.area() > 0 ? isect(r, eo).area() / eo.area() : 1.0; c_eye_px = (double)eo.area(); census_begin(cur.pk, replay, true, fr0); }
      arm = r; armed = true; g_cs.vpdirty = true; apply_vp();
      if (mode() != 3 && mode() != 5) db_arm(r);
      n_scis++; if (replay) n_rep_proj++;
      double fr = eo.area() > 0 ? isect(r, eo).area() / eo.area() : 1.0; area_sum += fr; int bk = (int)(fr * 100.0 + 0.5); if (bk < 0) bk = 0; if (bk > 100) bk = 100; area_hist[bk]++;
      kind_hist[cur.pk == b1vr::PK_SHPROJ_DIR ? 0 : 1][fr < 0.10 ? 0 : fr < 0.25 ? 1 : fr < 0.50 ? 2 : fr < 0.90 ? 3 : 4]++;
      if (n_logged < 3) { n_logged++; std::fprintf(stderr, "[b1sb] bbox %d: %s projection scissor x %d..%d y %d..%d (%.1f%% of the %dx%d eye rect), %zu volume vertices\n", n_logged, replay ? "right-eye replay" : "game",
                                                   r.x0, r.x1, r.y0, r.y1, 100.0 * fr, eo.x1 - eo.x0, eo.y1 - eo.y0, cur.v.size() / 4); } }
    /* b100 probe (BSI_B1_SHBBOX=6, EXACT: no pixel changes): before every projection quad, the render pass is ended with a fragment -> vertex barrier,
       exactly what a GPU-computed tile mask read by the quad's vertex stage would cost (Vulkan allows no such barrier inside a pass). Measures the
       overhead of the tiled-quad design (~56 spills per frame at Rooftops). */
    static long n_spill; static std::map<std::pair<uint32_t, uint32_t>, long> unk_hist;   /* b100: unknown stencil writes per (vs, ps) */
    static void spill() { static bool said; if (!said) { said = true; std::fprintf(stderr, "[b1sb] BSI_B1_SHBBOX=6: render pass ended (fragment -> vertex barrier) before every projection quad (timing probe, exact)\n"); }
      n_spill++; emit([] (DxvkContext* c) { c->emitGraphicsBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); }); }
    static void draw_pre(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {   /* b1dx_c_draw*, after pre_draw (viewports emitted) */
      cur.kind = 0; cur.res = 0; cur.have = false; cur.cover = false; armed = false; skip_draw = false; clr_pending = false;   /* b112 */
      if (census() == 2 && !c_open) { View *rv = g_views.get(g_cs.rtv[0]); if (rv && rv->type == 2 && rv->iv != nullptr && rv->iv->image()->info().format == VK_FORMAT_R16G16B16A16_SFLOAT) { BS *bs = g_bss.get(g_cs.bs); census_begin(bs && bs->m[0].blendEnable() ? -2 : -1, false, false, 0); } }   /* b110 overdraw census; b111: split by blend */
      if (st_override) { st_override = false; DSS *d0 = g_dsss.get(g_cs.dss); if (d0) { DxvkDepthStencilState e = d0->s; emit([e] (DxvkContext* c) { c->setDepthStencilState(e); }); } }   /* b92 probe: the game's state back */
      if (!mode()) return;
      uint32_t res = 0; { View *v = g_views.get(g_cs.dsv); if (v && v->type == 3) res = v->res; } if (!res) return;
      DSS *d = g_dsss.get(g_cs.dss);
      if (!d || d->s.depthWrite()) { if (n_dz < 8) { char ds[256]; dss_str(ds, sizeof ds, d, g_cs.sref & 0xffu); char t[400]; std::snprintf(t, sizeof t, "depth write by vs %s ps %s | %s", shkey(g_bc.sh[0]).c_str(), shkey(g_bc.sh[1]).c_str(), ds); zpoison(res, 0, t); } else zpoison(res, 0, ""); }   /* no DSS = the default state: depth write on. b107: the diagnostic string only while it can still be printed (chat 5: ~1.8 ms of snprintf per frame on the render thread) */
      if (!d || !d->s.stencilTest()) return;
      DxvkStencilOp f = d->s.stencilOpFront(), b = d->s.stencilOpBack(); bool writes = f.writeMask() || b.writeMask(); uint32_t ref = g_cs.sref & 0xffu;
      uint32_t ps = g_bc.sh[1] == BC_UNK ? 0 : g_bc.sh[1]; int pk = ps ? b1vr::pk_of(ps) : b1vr::PK_NULL;
      cur.res = res; Rect eo, ei; bool have_eff = eff(eo, ei);
      if (pk == b1vr::PK_SHPROJ_DIR || pk == b1vr::PK_SHPROJ) { cur.kind = 2; cur.pk = pk; n_proj++;
        { BS *bs = g_bss.get(g_cs.bs); sheye::on_proj(have_eff && b1vr::eye_w > 0 && eo.x0 >= b1vr::eye_w, bs ? (uint32_t)bs->m[0].writeMask() : 0xfu); }   /* b123 sheye check */
        { static int pr = -1; if (pr < 0) { const char *e = std::getenv("BSI_B1_SHEYEPROBE"); pr = e && std::atoi(e) > 0; }   /* b122 NOT EXACT ceiling probe: right-eye shadow projections skipped (the right eye loses its dynamic shadows) */
          if (pr && have_eff && b1vr::eye_w > 0 && eo.x0 >= b1vr::eye_w) { skip_draw = true; static long n; if (!(n++ % 20000)) std::fprintf(stderr, "[b1sb] SHEYEPROBE: right-eye projection skipped (%ld so far)\n", n); return; } }
        if (mode() == 4) { DxvkDepthStencilState e = d->s; e.setStencilTest(false); e.normalize(); emit([e] (DxvkContext* c) { c->setDepthStencilState(e); }); st_override = true; n_proj_nost++; return; }   /* b92 probe */
        if (shda() && !d->s.depthTest() && !d->s.depthWrite()) {   /* b97 EXACT: a depth test that always passes changes nothing, but puts the stencil test before the shader
                                                                       (census b96: the kernels ran on every rasterised pixel; the quads' regions are 1-25 % of the eye) */
          DxvkDepthStencilState e = d->s; e.setDepthTest(true); e.setDepthWrite(false); e.setDepthCompareOp(VK_COMPARE_OP_ALWAYS); e.normalize();
          emit([e] (DxvkContext* c) { c->setDepthStencilState(e); }); st_override = true; n_proj_da++; }
        if (writes) { poison(res, P_PROJW); n_proj_w++; return; }
        if (sb87_lrz_mod()) { n_proj_lrz++; return; }
        if (st_true(f, ref, 0) || st_true(b, ref, 0)) { n_proj_pass0++; return; }   /* the quad also draws where the stencil is 0: no saving */
        if (mode() == 6) spill();   /* b100 probe */
        cur.have = true; arm_rect(res, eo, have_eff, dc, dic, false);
        if (tile() && have_eff) tile_arm(eo, d, ref, false);   /* b102/b108: the tiled draw is armed (geometry independent: our own VS draws the tiles) */
        return; }
      if (!writes) return;
      if (!face_grows(f, ref) && !face_grows(b, ref)) {   /* only zeros written: never grows the region (ignoring it is conservative); a clear quad proves a rect zero */
        if (pk != b1vr::PK_ONECOLOR || !clearquad()) return;
        RS *rs = g_rss.get(g_cs.rs); bool depth_can_fail = d->s.depthTest() && d->s.depthCompareOp() != VK_COMPARE_OP_ALWAYS;
        bool nodata = false; cur.kind = 3; n_clearq++; const char *why = nullptr;
        uint32_t vs = g_bc.sh[0] == BC_UNK ? 0 : g_bc.sh[0]; VsI vi = vs ? vs_info(vs) : VsI();
        if (!face_clears(f, ref, depth_can_fail) || !face_clears(b, ref, depth_can_fail)) why = "an op that can fire does not write 0 (or write mask != 0xff)";
        else if (!have_eff) why = "no viewport";
        else if (!fetch(dc, dic, cur.v, nodata, vi.pos_reg)) why = fetch_why;
        else if (!cover(cur.v, g_m79_topo, rs)) why = cover_why;
        if (!why) { cur.cover = true; shrink(res, ei); clr_arm(d, eo, ei); }
        else { n_clearq_no++;
          if (n_dq < 8) { n_dq++; char ds[256]; dss_str(ds, sizeof ds, d, ref); std::string vtx; char t[64];
            for (size_t i = 0; i + 3 < cur.v.size() && i < 24; i += 4) { std::snprintf(t, sizeof t, " (%.3g %.3g %.3g %.3g)", cur.v[i], cur.v[i + 1], cur.v[i + 2], cur.v[i + 3]); vtx += t; }
            BS *bs = g_bss.get(g_cs.bs); uint32_t ilid = g_bc.il == BC_UNK ? 0 : g_bc.il;
            std::fprintf(stderr, "[b1sb] clear quad %d NOT proven: %s | vs %s ps %s | %s | blend %s colour mask %x | cull %d front %d | %s%u verts topo %u layout %s (POSITION v%d) | vp %.0f,%.0f %.0fx%.0f sc %d,%d %ux%u | bytes %s%s\n",
                         n_dq, why, shkey(vs).c_str(), shkey(ps).c_str(), ds, bs && bs->m[0].blendEnable() ? "on" : "off", bs ? (unsigned)bs->m[0].writeMask() : 15u, rs ? (int)rs->s.cullMode() : -1, rs ? (int)rs->s.frontFace() : -1,
                         dic ? "indexed " : "", dic ? dic->indexCount : dc->vertexCount, g_m79_topo, m79::ilsig.count(ilid) ? m79::ilsig[ilid].c_str() : "?", vi.pos_reg, rec_vp[0], rec_vp[1], rec_vp[2], rec_vp[3],
                         rec_sc.offset.x, rec_sc.offset.y, rec_sc.extent.width, rec_sc.extent.height, cur.v.empty() ? (nodata ? "unknown" : "-") : "known:", vtx.c_str()); } }
        return; }
      /* a draw that can leave a non-zero stencil: the stencil volume (projection VS, no PS) or unknown */
      uint32_t vs = g_bc.sh[0] == BC_UNK ? 0 : g_bc.sh[0]; VsI vi = vs ? vs_info(vs) : VsI();
      if (pk == b1vr::PK_NULL && vi.kind == 2 && recv()) {   /* b93: receiver mesh marked in stencil (UE3 per-object shadows): its projected AABB bounds the stencil writes; depth range unknown (open meshes) */
        cur.kind = 4; n_recv++;
        std::array<double, 6> ab; const float *l2w = cb0_get(64), *vpm = cb1_get(64);
        if (!have_eff || !g_cs.nvp || !l2w || !vpm || !fetch_aabb(dc, dic, vi.pos_reg, ab)) { poison(res, P_NODATA); n_recv_nodata++;
          static int n_dr; if (n_dr < 8) { n_dr++; uint32_t ilid = g_bc.il == BC_UNK ? 0 : g_bc.il;
            std::fprintf(stderr, "[b1sb] receiver mesh %d NO DATA: eff %d nvp %u cb0 %s cb1 %s fetch: %s | %s%u verts topo %u layout %s | vb %u stride %u off %u ib %u copies %zu\n", n_dr, (int)have_eff, g_cs.nvp, l2w ? "ok" : "-", vpm ? "ok" : "-", fetch_why,
                         dic ? "indexed " : "", dic ? dic->indexCount : dc->vertexCount, g_m79_topo, m79::ilsig.count(ilid) ? m79::ilsig[ilid].c_str() : "?", g_bc.vb[0], g_bc.vbs[0], g_bc.vbo[0], g_bc.ib, bigbuf.size()); }
          return; }
        std::memcpy(cur.l2w, l2w, 64); cur.v.clear(); for (int c = 0; c < 8; c++) { cur.v.push_back(ab[c & 1 ? 3 : 0]); cur.v.push_back(ab[c & 2 ? 4 : 1]); cur.v.push_back(ab[c & 4 ? 5 : 2]); cur.v.push_back(1.0); }
        float m[16]; compose(cur.l2w, vpm, m); cur.have = true; add_convex(res, m, cur.v, eo, g_cs.vp[0], n_recv_unb); return; }
      if (vi.kind == 5 && skins()) {   /* b101: skinned subject (UE3 GPU skin, depth-only or material PS): per-bone boxes x bone matrices bound the stencil writes */
        cur.kind = 4; n_skin++; int need = 16 * (std::max(vi.decA, vi.decB) + 1); const float *g = cb0_get(need > 64 ? need : 64), *vpm = cb1_get(64); uint32_t bhave = 0; const float *bones = cb_get(3, 48, &bhave);
        const SkinBoxes *sb = (have_eff && g_cs.nvp && g && vpm && bones) ? fetch_skin(dc, dic, vi) : nullptr;
        static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        if (!sb || !skin_points(*sb, g + 4 * vi.decA, g + 4 * vi.decB, bones, bhave / 16u, g, cur.v)) { poison(res, P_NODATA); n_skin_nodata++;
          static int n_ds; if (n_ds < 8) { n_ds++; uint32_t ilid = g_bc.il == BC_UNK ? 0 : g_bc.il;
            std::fprintf(stderr, "[b1sb] skinned marker %d NO DATA: eff %d nvp %u cb0 %s cb1 %s cb3 %s (%u B) fetch: %s | vs %s ps %s | %s%u verts topo %u layout %s\n", n_ds, (int)have_eff, g_cs.nvp, g ? "ok" : "-", vpm ? "ok" : "-", bones ? "ok" : "-", bhave, sb ? "bones beyond cb3" : fetch_why, shkey(vs).c_str(), shkey(ps).c_str(),
                         dic ? "indexed " : "", dic ? dic->indexCount : dc->vertexCount, g_m79_topo, m79::ilsig.count(ilid) ? m79::ilsig[ilid].c_str() : "?"); }
          return; }
        std::memcpy(cur.l2w, ident, 64); cur.have = true; add_convex(res, vpm, cur.v, eo, g_cs.vp[0], n_skin_unb); return; }
      if (vi.kind == 3 && markers()) {   /* b99: the shadow subject itself, drawn with its material (depth test, stencil REPLACE): its projected vertices bound the stencil writes */
        cur.kind = 4; n_mark++; int need = 16 * (std::max(vi.decA, vi.decB) + 1); const float *g = cb0_get(need > 64 ? need : 64), *vpm = cb1_get(64);
        if (!have_eff || !g_cs.nvp || !g || !vpm || !fetch_mark(dc, dic, vi.pos_reg, g + 4 * vi.decA, g + 4 * vi.decB, cur.v)) { poison(res, P_NODATA); n_mark_nodata++;
          static int n_dm; if (n_dm < 8) { n_dm++; uint32_t ilid = g_bc.il == BC_UNK ? 0 : g_bc.il;
            std::fprintf(stderr, "[b1sb] subject marker %d NO DATA: eff %d nvp %u cb0 %s cb1 %s fetch: %s | vs %s ps %s | %s%u verts topo %u layout %s | vb %u stride %u off %u ib %u\n", n_dm, (int)have_eff, g_cs.nvp, g ? "ok" : "-", vpm ? "ok" : "-", fetch_why, shkey(vs).c_str(), shkey(ps).c_str(),
                         dic ? "indexed " : "", dic ? dic->indexCount : dc->vertexCount, g_m79_topo, m79::ilsig.count(ilid) ? m79::ilsig[ilid].c_str() : "?", g_bc.vb[0], g_bc.vbs[0], g_bc.vbo[0], g_bc.ib); }
          return; }
        std::memcpy(cur.l2w, g, 64); float m[16]; compose(cur.l2w, vpm, m); cur.have = true; add_convex(res, m, cur.v, eo, g_cs.vp[0], n_mark_unb); return; }
      if (pk != b1vr::PK_NULL || vi.kind != 1) { poison(res, P_UNKNOWN); if (unk_hist.size() < 64 || unk_hist.count({ vs, ps })) unk_hist[{ vs, ps }]++;
        if (n_du < 8) { n_du++; char ds[256]; dss_str(ds, sizeof ds, d, ref); uint32_t ilid = g_bc.il == BC_UNK ? 0 : g_bc.il;
          std::fprintf(stderr, "[b1sb] unknown stencil write %d: vs %s ps %s (kind %d) | %s | %s%u verts topo %u layout %s\n", n_du, shkey(vs).c_str(), shkey(ps).c_str(), pk, ds, dic ? "indexed " : "", dic ? dic->indexCount : dc->vertexCount, g_m79_topo, m79::ilsig.count(ilid) ? m79::ilsig[ilid].c_str() : "?"); }
        return; }
      if (vi.scanned && (vi.cb1 & 0x0fu) != 0x0fu) { poison(res, P_BADVS); return; }
      cur.kind = 1; n_vol++; cur.pvt = pvt_of(vi);
      if (mode() == 4) {   /* b92 NOT EXACT ceiling probe: the stencil volumes are not drawn at all (skip_draw) and the projection quads run with the stencil test off */
        static bool said; if (!said) { said = true; std::fprintf(stderr, "[b1sb] BSI_B1_SHBBOX=4: NOT EXACT, stencil volumes skipped + projection quads without stencil test (measurement only)\n"); }
        skip_draw = true; n_vol_skipped++; return; }
      { RS *rs = g_rss.get(g_cs.rs); bool bias = rs && (rs->b.depthBiasConstant != 0.f || rs->b.depthBiasSlope != 0.f);   /* a biased face depth is not the vertices' (no RS = default: no bias) */
        if (bias || !(d->s.depthTest() && !d->s.depthWrite() && cmp_ordered(d->s.depthCompareOp()) && face_zfail(f) && face_zfail(b))) { if (n_dz < 8) { char ds[256]; dss_str(ds, sizeof ds, d, ref); char t[400];
          std::snprintf(t, sizeof t, "volume %s | %s", bias ? "with depth bias" : "not z-fail counting", ds); zpoison(res, 1, t); } else zpoison(res, 1, ""); } }
      bool nodata; if (!fetch(dc, dic, cur.v, nodata, vi.pos_reg >= 0 ? vi.pos_reg : -1)) { poison(res, nodata ? P_NODATA : P_LAYOUT); return; }
      const float *cb = cb1_get(cur.pvt ? 80 : 64); if (!cb) { poison(res, P_NODATA); return; }
      Rect box; double zlo, zhi; if (!have_eff || !g_cs.nvp) { poison(res, P_NODATA); return; }
      sheye::on_vol(cb, cur.pvt, g_cs.vp[0], b1vr::eye_w > 0 && eo.x0 >= b1vr::eye_w);   /* b123 sheye check: this eye's view constants */
      if (!project(cb, cur.pvt, cur.v, g_cs.vp[0], box, &zlo, &zhi)) {   /* a vertex at / behind the eye: the whole rect, no depth range (the region stays known) */
        n_unb++; cur.have = true; add_rect_unb(res, eo, g_cs.vp[0]);
        if (n_dw < 4) { n_dw++; std::string ws; char t[32]; for (size_t i = 0; i + 3 < cur.v.size() && i < 96; i += 4) { double p[3] = { cur.v[i], cur.v[i + 1], cur.v[i + 2] }; if (cur.pvt) for (int c = 0; c < 3; c++) p[c] += cb[16 + c];
            double w = p[0] * cb[3] + p[1] * cb[7] + p[2] * cb[11] + cur.v[i + 3] * cb[15]; std::snprintf(t, sizeof t, " %.4g", w); ws += t; }
          std::fprintf(stderr, "[b1sb] volume %d with clip.w <= eps: %zu vertices, w =%s%s (whole rect, no depth bounds)\n", n_dw, cur.v.size() / 4, ws.c_str(), cur.v.size() > 96 ? " ..." : ""); }
        return; }
      cur.have = true; add_rect(res, isect(box, eo), zlo, zhi, true); }
    static void draw_post() {   /* right after the draw was emitted (before pp76's replay) */
      if (!mode()) return; census_end(); if (chk.pending) check_post();
      if (armed) { armed = false; g_cs.vpdirty = true; } db_disarm(); }
    void replay_pre(const float *vs1o) {   /* pp76 sh_post: the same draw again for the right eye (g_cs.vp / sc shifted; vs1o = its cb1 rows 0..3 + eye) */
      if (!mode() || !cur.res) return;
      apply_vp(); Rect eo, ei; bool have_eff = eff(eo, ei);
      switch (cur.kind) {
        case 1: { n_rep_vol++; if (cur.have && g_cs.nvp) sheye::on_vol(vs1o, cur.pvt, g_cs.vp[0], true);   /* b123 */ Rect box; double zlo, zhi; if (cur.have && have_eff && g_cs.nvp && project(vs1o, cur.pvt, cur.v, g_cs.vp[0], box, &zlo, &zhi)) add_rect(cur.res, isect(box, eo), zlo, zhi, true);
                  else if (have_eff && !cur.v.empty() && g_cs.nvp) { n_unb++; add_rect_unb(cur.res, eo, g_cs.vp[0]); } else poison(cur.res, P_NODATA); break; }
        case 2: { BS *bs = g_bss.get(g_cs.bs); sheye::on_proj(true, bs ? (uint32_t)bs->m[0].writeMask() : 0xfu); }   /* b123 */ if (mode() == 6) spill(); if (cur.have) arm_rect(cur.res, eo, have_eff, nullptr, nullptr, true); else n_rep_proj_not++;
                if (tile() && cur.have && have_eff) { DSS *d = g_dsss.get(g_cs.dss); if (d) tile_arm(eo, d, g_cs.sref & 0xffu, true); } break;
        case 4: { n_rep_vol++; if (!cur.have || !have_eff || !g_cs.nvp) { poison(cur.res, P_NODATA); break; } float m[16]; compose(cur.l2w, vs1o, m);   /* b93 receivers, b99 markers */
                  add_convex(cur.res, m, cur.v, eo, g_cs.vp[0], n_recv_unb); break; }
        case 3: n_rep_clear++; if (cur.cover && have_eff) { shrink(cur.res, ei); DSS *d = g_dsss.get(g_cs.dss); if (d) { clr_arm(d, eo, ei); if (clr_pending) n_clr_rep++; } } break;
        default: break; } }
    void replay_post() { census_end(); if (armed) { armed = false; g_cs.vpdirty = true; } db_disarm(); }
    static void clear_dsv(View *v, VkImageAspectFlags asp, uint32_t stencil, int es_cl) {   /* b1dx_c_clear_dsv, before the clear is emitted */
      if (!mode() || !v) return;
      if (!(asp & VK_IMAGE_ASPECT_STENCIL_BIT)) { zpoison(v->res, 2, "depth-only clear"); return; }   /* depth-only clear: the stencil keeps its value, the depth range is gone */
      Obj *o = obj(v->res); bool whole = o && o->kind == 2 && o->img->info().mipLevels == 1 && o->img->info().numLayers == 1 && v->iv != nullptr;
      if (!whole) { poison(v->res, P_COPY); return; }
      if (stencil & 0xffu) { poison(v->res, P_CLEARVAL); return; }
      if (es_cl > 0) { Reg &g = reg(v->res); g.full = true; g.r.clear(); g.cleared.assign(1, Rect { 0, 0, es_cl, (int)v->iv->mipLevelExtent(0).height }); n_partial++; return; }   /* es86 partial clear: the eye area is 0 */
      reset(v->res); }
    static long n_presents;
    static void stats() {
      long nh = 0; for (int i = 0; i <= 100; i++) nh += area_hist[i]; int med = 0; if (nh) { long acc = 0; for (int i = 0; i <= 100; i++) { acc += area_hist[i]; if (2 * acc >= nh) { med = i; break; } } }
      std::fprintf(stderr, "[b1sb] shadow bbox scissor (mode %d, margin %d): sequences %ld, volume draws %ld, projections %ld, scissored %ld (bbox area mean %.1f%% median %d%% of the eye rect), unscissored: region unknown %ld, writes stencil %ld, test dropped by s2 %ld, test true at 0 %ld\n",
                   mode(), margin(), n_seq, n_vol, n_proj, n_scis, nh ? 100.0 * area_sum / nh : 0.0, med, n_proj_full, n_proj_w, n_proj_lrz, n_proj_pass0);
      std::fprintf(stderr, "[b1sb]   bbox area of the eye rect, cascade (PK_SHPROJ_DIR) quads: <10%% %ld, 10-25%% %ld, 25-50%% %ld, 50-90%% %ld, >=90%% %ld; per-object / spot (PK_SHPROJ) quads: <10%% %ld, 10-25%% %ld, 25-50%% %ld, 50-90%% %ld, >=90%% %ld\n",
                   kind_hist[0][0], kind_hist[0][1], kind_hist[0][2], kind_hist[0][3], kind_hist[0][4], kind_hist[1][0], kind_hist[1][1], kind_hist[1][2], kind_hist[1][3], kind_hist[1][4]);
      if (mode() == 4) std::fprintf(stderr, "[b1sb]   MODE 4 (NOT EXACT): volume draws skipped %ld, projection quads drawn without stencil test %ld\n", n_vol_skipped, n_proj_nost);
      std::fprintf(stderr, "[b1sb]   depth-always (b97, %d): projection quads given depth test ALWAYS %ld\n", shda(), n_proj_da);
      std::fprintf(stderr, "[b1sb]   receiver meshes (b95): %ld (no data %ld, unbounded %ld), AABB cache %zu, static buffer copies %zu (%.1f MB)\n", n_recv, n_recv_nodata, n_recv_unb, aabbs.size(), bigbuf.size(), big_total / 1048576.0);
      std::fprintf(stderr, "[b1sb]   subject markers (b99, %d): %ld (no data %ld, unbounded %ld); near-plane clipped boxes %ld (%ld still the whole rect)\n", markers(), n_mark, n_mark_nodata, n_mark_unb, n_clip, n_clip_full);
      std::fprintf(stderr, "[b1sb]   skinned markers (b101, %d): %ld (no data %ld, unbounded %ld), box cache %zu\n", skins(), n_skin, n_skin_nodata, n_skin_unb, skinboxes.size());
      if (n_spill) std::fprintf(stderr, "[b1sb]   SHBBOX=6 probe: render pass ended before %ld projection quads\n", n_spill);
      { static uint64_t last[8]; static bool have; DxvkStatCounters sc = g_dev->getStatCounters();   /* b113: DXVK's own counters per frame (600-present windows) */
        uint64_t cur[8] = { sc.getCtr(DxvkStatCounter::CmdRenderPassCount), sc.getCtr(DxvkStatCounter::CmdBarrierCount), sc.getCtr(DxvkStatCounter::QueueSubmitCount), sc.getCtr(DxvkStatCounter::CmdDrawCalls),
                            sc.getCtr(DxvkStatCounter::CmdDispatchCalls), sc.getCtr(DxvkStatCounter::PipeCountGraphics), sc.getCtr(DxvkStatCounter::PipeCountLibrary), sc.getCtr(DxvkStatCounter::CmdDrawsMerged) };
        if (have) std::fprintf(stderr, "[b1dxs] per frame (600 presents): render passes %.1f, pipeline barriers %.1f, queue submits %.2f, draw calls %.0f, dispatches %.1f | graphics pipelines total %llu (libraries %llu), draws merged %.0f/frame\n",
                               (cur[0] - last[0]) / 600.0, (cur[1] - last[1]) / 600.0, (cur[2] - last[2]) / 600.0, (cur[3] - last[3]) / 600.0, (cur[4] - last[4]) / 600.0, (unsigned long long)cur[5], (unsigned long long)cur[6], (cur[7] - last[7]) / 600.0);
        std::memcpy(last, cur, sizeof last); have = true; }
      if (shclr()) { std::string w; for (auto &e : clr_why) { char b[96]; std::snprintf(b, sizeof b, " %ld x %s;", e.second, e.first.c_str()); w += b; }
        std::fprintf(stderr, "[b1sb]   SHCLR (b112, exact): stencil-clear quads replaced by stencil clears %ld (right-eye replays %ld), kept as draws %ld:%s\n", n_clr, n_clr_rep, n_clr_no, w.c_str()); }
      { char fb[256]; b1fp16_stats(fb, sizeof fb); std::fprintf(stderr, "[b1fp16] %s\n", fb); }
      if (tile()) { std::string w; for (auto &e : tile_fb_why) { char b[200]; std::snprintf(b, sizeof b, " %ld x %s;", e.second, e.first.c_str()); w += b; }
        std::fprintf(stderr, "[b1sb]   SHTILE (b102, %d px, NOT bit-exact): tiled quads %ld (right-eye replays %ld), plain fallbacks %ld:%s vertex grids %zu, stencil views %zu\n", tile_px(), n_tile, n_tile_rep, n_tile_fb, w.c_str(), tvbs.size(), tstv.size());
        tstat_read(); std::fprintf(stderr, "[b1sb]   SHTILE lit tiles (b110, %ld sampled draws): %.1f %% of the tiles lit on average; share of draws by lit fraction 0-10%%..90-100%%: %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld (100%%: %ld)\n", ts_n, ts_total ? 100.0 * ts_lit / ts_total : 0.0, ts_hist[0], ts_hist[1], ts_hist[2], ts_hist[3], ts_hist[4], ts_hist[5], ts_hist[6], ts_hist[7], ts_hist[8], ts_hist[9], ts_hist[10]); }
      { std::vector<std::pair<long, std::pair<uint32_t, uint32_t>>> top; for (auto &e : unk_hist) top.push_back({ e.second, e.first }); std::sort(top.rbegin(), top.rend());
        std::string t; for (size_t i = 0; i < top.size() && i < 6; i++) { char b[160]; std::snprintf(b, sizeof b, " %ld x (vs %s ps %s)", top[i].first, shkey(top[i].second.first).c_str(), shkey(top[i].second.second).c_str()); t += b; }
        if (!t.empty()) std::fprintf(stderr, "[b1sb]   unknown stencil writes by shader pair (b100):%s\n", t.c_str()); }
      if (census()) { const char *kn[2] = { "cascade", "per-object" };
        for (int k = 0; k < 2; k++) for (int r = 0; r < 2; r++) if (c_n[k][r]) std::fprintf(stderr, "[b1sb]   CENSUS %s%s quads: %ld, mean pixels passing stencil %.0f = %.1f%% of the eye (mean scissor area %.1f%%)\n", kn[k], r ? " (replay)" : "", c_n[k][r], c_px_sum[k][r] / c_n[k][r], c_eye_px > 0 ? 100.0 * c_px_sum[k][r] / c_n[k][r] / c_eye_px : 0.0, 100.0 * c_area_sum[k][r] / c_n[k][r]);
        if (census() == 2) for (int b = 0; b < 2; b++) std::fprintf(stderr, "[b1sb]   OVERDRAW census (b111) scene-colour pass (RGBA16F), %s draws: %ld draws over %ld frames, %.1f draws/frame, %.0f fragments passing depth/stencil per frame = %.2f per eye pixel (both eyes = %.0f px)\n", b ? "BLENDED" : "OPAQUE (blend off)", c2_n[b], c2_frames, c2_frames ? (double)c2_n[b] / c2_frames : 0.0, c2_frames ? c2_px[b] / c2_frames : 0.0, (c2_frames && c_eye_px > 0) ? c2_px[b] / c2_frames / (2.0 * c_eye_px) : 0.0, 2.0 * c_eye_px);
        for (int k = 0; k < 2; k++) std::fprintf(stderr, "[b1sb]   CENSUS %s quads by pixels passing / eye: <1%% %ld, 1-5%% %ld, 5-25%% %ld, 25-50%% %ld, 50-90%% %ld, >=90%% %ld (queries skipped %ld)\n", kn[k], c_hist[k][0], c_hist[k][1], c_hist[k][2], c_hist[k][3], c_hist[k][4], c_hist[k][5], c_skipped);
        std::memset(c_px_sum, 0, sizeof c_px_sum); std::memset(c_area_sum, 0, sizeof c_area_sum); std::memset(c_n, 0, sizeof c_n); std::memset(c_hist, 0, sizeof c_hist); }
      std::fprintf(stderr, "[b1sb]   replay: volumes %ld, quads scissored %ld / not %ld, clears %ld; clear quads %ld (not proven %ld), stencil clears %ld (partial %ld); volumes with clip.w <= eps (whole rect) %ld\n",
                   n_rep_vol, n_rep_proj, n_rep_proj_not, n_rep_clear, n_clearq, n_clearq_no, n_reset, n_partial, n_unb);
      std::fprintf(stderr, "[b1sb]   region lost to: %s %ld, %s %ld, %s %ld, %s %ld, %s %ld, %s %ld, %s %ld, %s %ld\n",
                   pname[0], n_poison[0], pname[1], n_poison[1], pname[2], n_poison[2], pname[3], n_poison[3], pname[4], n_poison[4], pname[5], n_poison[5], pname[6], n_poison[6], pname[7], n_poison[7]);
      std::fprintf(stderr, "[b1sb]   depth bounds (%d): quads with %ld (mean range %.3f), without %ld, ranges lost %ld (depth writes %ld, volume not z-fail / biased %ld, depth-only clears %ld)", shdb(), n_db, n_db ? zfrac_sum / n_db : 0.0, n_nodb, n_zlost, n_zl[0], n_zl[1], n_zl[2]);
      if (mode() == 2) std::fprintf(stderr, "; check: runs %ld, bad runs %ld, texels differing %ld of %ld, skipped %ld", chk.n_runs, chk.n_bad_runs, chk.tex_bad, chk.tex_tot, chk.n_skip);
      std::fprintf(stderr, "\n"); }
    void frame() {   /* every present */
      if (!mode()) return; n_presents++; census_frame();   /* b96 */
      if (mode() == 2) check_frame();
      if (n_presents % 600 == 0) stats(); }
  }
