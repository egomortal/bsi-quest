/* rhi_b1_m_s2.inc: s2 copy aliasing / storage hand-over, b119 copy trim, s2 stencil, shsig diagnostics. Split out of rhi_b1_dxvk.cpp in build 120 (2026-11-02, chat 4) without any change: included at the same place, the preprocessed
   translation unit is the same as before. Only rhi_b1_dxvk.cpp includes this file. */
/* =====================================================================================================================
   S2 (14 Oct; Fable notes 1 and 6): exact savings in this immediate-mode backend. Every entry point below runs under g_rmu.
   (1) BSI_B1_COPYALIAS (copy aliasing / lazy copy). UE3 copies every render target into a separate "resolve" texture before
       sampling it (whole subresource, same format and size). Such a copy is not executed at once but recorded as pending
       (invariant: dst's content == src's current content). While pending, every SRV bind of a view of dst binds the same
       view of src (alias), so readers see the same bytes. The copy is executed for real at the last moment it is still
       exact, i.e. before anything could make the alias observable:
         - before src or dst is written: a draw with it bound as render/depth target, a clear, a copy into it, an upload,
           a UAV bind, a mip generation;
         - before dst is read other than through an SRV (copy source, mip generation, alias view not creatable);
         - before src is released;
       and then every slot holding an alias is rebound to dst's own view. A write that overwrites ALL of dst (whole copy,
       whole clear) instead drops the pending copy: it was never observable. Mode 2 adds the storage hand-over: when src is
       about to be overwritten completely (whole clear / whole copy into it), dst takes src's current backing storage
       (DxvkContext::invalidateImage) and src gets new storage, so the copy is never executed - dst then holds exactly the
       bytes the copy would have produced (only between images with the same create info, dst's usage/stages/access a
       subset of src's, so the release barrier DXVK recorded for src's last write covers dst's readers).
       Eligible: colour formats, 1 mip, 1 layer, same VkFormat and size, no box or a box covering the image, neither image
       bound as UAV, not the back buffer. A copy is never skipped on a guess: a dead copy (never read before its next
       overwrite) can only be dropped when that overwrite comes before any write to src - no look-ahead.
   (2) BSI_B1_LRZFIX (stencil test drop). Per depth-stencil image: the value of the whole stencil plane when known - set by a
       stencil clear of the whole image (1 mip, 1 layer), unknown after any draw with it bound whose state can write
       stencil (normalized write mask != 0), a partial clear, a copy or upload into it. A draw whose stencil test writes
       nothing and is true for that value on both faces gets the same state with the stencil test disabled: no fragment
       was rejected by that test and nothing was written, so the output is identical; Turnip keeps LRZ writes on for it
       (tu6_calculate_lrz_state turns LRZ writes off for any stencil compare other than ALWAYS/NEVER).
   (3) LOAD_OP_DONT_CARE: not implemented - see s2_fix/README.md (needs a coverage proof or look-ahead).
   ===================================================================================================================== */
namespace s2 {
  static int env_int(const char *n, int def) { const char *e = std::getenv(n); return e && *e ? std::atoi(e) : def; }
  static int ca_mode() { static int v = -1; if (v < 0) v = env_int("BSI_B1_COPYALIAS", 2); return v; }
  static int lrz_mode() { static int v = -1; if (v < 0) v = env_int("BSI_B1_LRZFIX", 1); return v; }
  static int ca_log() { static int v = -1; if (v < 0) v = env_int("BSI_B1_COPYALIAS_LOG", 0); return v; }
  /* BSI_B1_S2LOG=<frame> (or BSI_B1_LRZFIX_LOG): copy and stencil events of that frame and the next, repeats folded */
  static long lrz_logf() { static long v = -2; if (v == -2) v = env_int("BSI_B1_S2LOG", env_int("BSI_B1_LRZFIX_LOG", -1)); return v; }
  static std::string lg_last; static long lg_rep;
  static void lg(const char *t) {
    if (lg_last == t) { lg_rep++; return; }
    if (lg_rep) std::fprintf(stderr, "[b1s2log]   (x%ld)\n", lg_rep + 1);
    lg_rep = 0; lg_last = t; std::fprintf(stderr, "[b1s2log] f%ld %s\n", g_frames, t); }
  static inline bool lg_on() { long f = lrz_logf(); return f >= 0 && (g_frames == f || g_frames == f + 1); }
  struct Pend { uint32_t dst, src; Rc<DxvkImage> di, si; };
  static std::vector<Pend> pend;                 /* pending copies (at most one per dst) */
  static uint32_t al_dst[6][128];                /* stage/slot: dst id whose alias view is bound there (0 = none) */
  static Rc<DxvkImageView> al_real[6][128];      /* ... and the view the game bound there (rebound when the alias ends) */
  static long n_al;                              /* slots holding an alias */
  struct AV { Rc<DxvkImageView> orig, alias; };
  static std::unordered_map<uint32_t, AV> avc;   /* alias view (same key on src's image) by SRV view id */
  static uint32_t uav_res[64];                   /* resource of the UAV bound in each compute slot */
  static long c_def, c_alias, c_wr, c_rd, c_drop, c_swap, c_noswap, c_inel;
  /* stencil tracking */
  static std::vector<int16_t> stv;               /* by resource id: -1 unknown, else the value of the whole stencil plane */
  static DxvkDepthStencilState lrz_ds; static uint32_t lrz_ref; static bool lrz_have, lrz_mod;
  static long c_lrz_drop, c_lrz_sw, c_st_clear, c_st_inval;
  /* by resource id: a clear may still be queued inside DxvkContext (deferred clears are applied when the image is next
     used; a storage hand-over would move such a clear to the wrong storage). Set by clears, cleared by a draw with the
     image bound as target or a real copy from/into it (both flush DXVK's deferred clear). */
  static std::vector<uint8_t> clrp;
  static inline bool clrp_get(uint32_t id) { return id < clrp.size() && clrp[id]; }
  static inline void clrp_set(uint32_t id, bool v) { if (id >= clrp.size()) { if (!v) return; clrp.resize(id + 64, 0); } clrp[id] = v; }
  static inline int stv_get(uint32_t id) { return id && id < stv.size() ? stv[id] : -1; }
  static inline void stv_set(uint32_t id, int v) {
    if (!id) return;
    if (id >= stv.size()) { if (v < 0) return; stv.resize(id + 64, -1); }
    if (v < 0 && stv[id] >= 0) c_st_inval++;
    stv[id] = (int16_t)v; }
}
static int s2_find_dst(uint32_t id) { for (size_t i = 0; i < s2::pend.size(); i++) if (s2::pend[i].dst == id) return (int)i; return -1; }
static void s2_bind_slot(int st, uint32_t slot, Rc<DxvkImageView> iv) {
  emit([sb = sbit(st), s = sidx(st) * 128 + slot, iv = std::move(iv)] (DxvkContext* c) mutable { c->bindResourceImageView(sb, s, std::move(iv)); });
}
static void s2_restore_slots(uint32_t dst) {   /* the alias of dst ends: every slot holding it gets dst's own view back */
  using namespace s2;
  if (!n_al) return;
  for (int st = 0; st < 6; st++) for (uint32_t s = 0; s < 128; s++) if (al_dst[st][s] == dst) {
    Rc<DxvkImageView> iv = std::move(al_real[st][s]); al_real[st][s] = nullptr; al_dst[st][s] = 0; n_al--;
    s2_bind_slot(st, s, std::move(iv)); }
}
static bool s2_can_swap(const s2::Pend &p) {
  const DxvkImageCreateInfo &a = p.si->info(), &b = p.di->info();
  if (a.type != b.type || a.format != b.format || a.flags != b.flags || a.sampleCount != b.sampleCount || a.tiling != b.tiling
   || a.extent.width != b.extent.width || a.extent.height != b.extent.height || a.extent.depth != b.extent.depth
   || a.numLayers != b.numLayers || a.mipLevels != b.mipLevels || a.shared || b.shared || a.viewFormatCount != b.viewFormatCount) return false;
  for (uint32_t i = 0; i < a.viewFormatCount; i++) if (a.viewFormats[i] != b.viewFormats[i]) return false;
  if (s2::clrp_get(p.src) || s2::clrp_get(p.dst)) return false;   /* a deferred clear might still target the old storage */
  return !(b.usage & ~a.usage) && !(b.stages & ~a.stages) && !(b.access & ~a.access);
}
/* b119 copy trim (BSI_B1_COPYTRIM, default 0 for the A/B): an image copy between two stereo scene targets (back-buffer-sized, not the back
   buffer, eye layout on) only copies the columns [0, 2 eye_w + es86 margin): es86 already keeps every scene draw out of the UI column, so
   nothing displayed reads it (the same invariant as DEADCLEAR=2). The 2976x1040 RGBA16F scene-colour copies were 5 per frame at 0.72 ms each
   (Turnip u_trace, j0356). Applied to the game's direct copies and to s2's executed deferred copies; s2's storage hand-overs are unchanged. */
static int copytrim() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_COPYTRIM"); v = e && *e ? std::atoi(e) > 0 : 0; } return v; }
static long g_ncopytrim; static double g_copytrim_px;
static uint32_t copytrim_w(VkExtent3D se, VkExtent3D de, int32_t sx, uint32_t w) {   /* the width to copy from column sx (w = unchanged) */
  if (!copytrim() || !es86::on() || !b1vr::on || b1vr::eye_w <= 0 || b1vr::bb_w <= 0) return w;
  auto bbsz = [] (VkExtent3D e) { return (int)e.width >= b1vr::bb_w && (int)e.width <= b1vr::bb_w + 32 && (int)e.height >= b1vr::bb_h && (int)e.height <= b1vr::bb_h + 32; };
  int64_t l = 2 * (int64_t)b1vr::eye_w + es86::margin();
  if (!bbsz(se) || !bbsz(de) || l >= (int64_t)se.width || (int64_t)sx + w <= l) return w;
  uint32_t nw = sx < l ? (uint32_t)(l - sx) : 0u; g_ncopytrim++; g_copytrim_px += (double)(w - nw) * se.height; return nw; }
/* end pending copy i. how: 0 execute (a write follows), 1 execute (a read needs it), 2 drop (dst is overwritten
   completely), 3 hand src's storage over to dst (src is overwritten completely) */
static void s2_end(int i, int how) {
  using namespace s2;
  if (how != 2) { dc88::flush_res(pend[i].src, dc88::n_s2); dc88::flush_res(pend[i].dst, dc88::n_s2); }   /* dc88: the copy reads src / writes dst; the hand-over aliases their storage (only those two images) */
  Pend p = pend[i]; pend.erase(pend.begin() + i);
  if (how <= 1) {
    VkImageSubresourceLayers l = { lookupFormatInfo(p.si->info().format)->aspectMask, 0u, 0u, 1u }; VkExtent3D e = p.si->mipLevelExtent(0);
    if (p.si != p.di && p.di != nullptr) { uint32_t nw = copytrim_w(e, p.di->mipLevelExtent(0), 0, e.width); if (nw) e.width = nw; }   /* b119 (src == g_bb never pends) */
    emit([di = p.di, si = p.si, l, e] (DxvkContext* c) { c->copyImage(di, l, VkOffset3D{ 0, 0, 0 }, si, l, VkOffset3D{ 0, 0, 0 }, e); });
    (how ? c_rd : c_wr)++; clrp_set(p.src, false); clrp_set(p.dst, false);
  } else if (how == 2) c_drop++;
  else {
    emit([di = p.di, si = p.si] (DxvkContext* c) {
      Rc<DxvkResourceAllocation> cur = si->storage();
      c->invalidateImage(si, si->allocateStorage(), VK_IMAGE_LAYOUT_UNDEFINED);   /* src: new storage (about to be overwritten) */
      c->invalidateImage(di, std::move(cur), si->info().layout); });              /* dst: src's bytes, in src's default layout */
    c_swap++;
  }
  if (lg_on()) { char t[120]; VkExtent3D e = p.si->info().extent; std::snprintf(t, sizeof t, "copy %u -> %u (%ux%u fmt %d) %s", p.src, p.dst, e.width, e.height, (int)p.si->info().format,
      how == 0 ? "executed before a write" : how == 1 ? "executed for a read" : how == 2 ? "dropped (dst overwritten)" : "storage hand-over (not executed)"); lg(t); }
  if (ca_log() && (c_wr + c_rd + c_drop + c_swap) <= (long)ca_log())
    std::fprintf(stderr, "[s2] pending copy %u -> %u %s\n", p.src, p.dst, how == 0 ? "executed before a write" : how == 1 ? "executed for a read" : how == 2 ? "dropped (dst overwritten)" : "replaced by a storage hand-over");
  s2_restore_slots(p.dst);
}
/* image id is about to be written; full = every texel of every aspect is overwritten without being read */
static void s2_before_write(uint32_t id, bool full) {
  using namespace s2;
  if (pend.empty() || !id) return;
  int i = s2_find_dst(id); if (i >= 0) s2_end(i, full ? 2 : 0);
  bool src = false; for (const Pend &p : pend) if (p.src == id) { src = true; break; }
  if (!src) return;
  uint32_t sw = 0;
  if (full && ca_mode() >= 2) { for (const Pend &p : pend) if (p.src == id && s2_can_swap(p)) { sw = p.dst; break; } if (!sw) c_noswap++; }
  for (size_t k = 0; k < pend.size(); ) { if (pend[k].src == id && pend[k].dst != sw) s2_end((int)k, 0); else k++; }   /* these read src first */
  if (sw) s2_end(s2_find_dst(sw), 3);
}
static inline void s2_before_read(uint32_t id) { if (!s2::pend.empty()) { int i = s2_find_dst(id); if (i >= 0) s2_end(i, 1); } }
static void s2_written(uint32_t id) { s2::stv_set(id, -1); s2_before_write(id, false); sb87::poison(id, sb87::P_COPY); /* sb87 */ }
namespace { bool sb87_lrz_mod() { return s2::lrz_mod; } }   /* sb87: s2 dropped the stencil test of the bound state */
static void s2_flush_all() { dc88::flush_all(dc88::n_other); /* dc88 (legacy B1.5 entry points) */ while (!s2::pend.empty()) s2_end(0, 0); }
static void s2_on_release(uint32_t id) {
  using namespace s2;
  stv_set(id, -1); clrp_set(id, false);
  if (pend.empty()) return;
  int i = s2_find_dst(id); if (i >= 0) s2_end(i, 2);   /* dst gone: nobody can read the copy */
  for (size_t k = 0; k < pend.size(); ) { if (pend[k].src == id) s2_end((int)k, 0); else k++; }
}
static void s2_view_released(uint32_t id) { if (!s2::avc.empty()) s2::avc.erase(id); }
/* the view to bind for SRV view vid (v, real view iv) in stage st / slot s: an alias of src while v's resource is a pending dst */
static Rc<DxvkImageView> s2_alias_for(int st, uint32_t s, uint32_t vid, View *v, const Rc<DxvkImageView> &iv) {
  using namespace s2;
  if (pend.empty() || !v || v->type != 1) return nullptr;
  int i = s2_find_dst(v->res); if (i < 0) return nullptr;
  const Pend &p = pend[i];
  Rc<DxvkImageView> a;
  auto it = avc.find(vid);
  if (it != avc.end() && it->second.orig == iv && it->second.alias->image() == p.si.ptr()) a = it->second.alias;
  else {
    DxvkImageViewKey k = iv->info();
    if ((k.format == p.si->info().format || p.si->isViewCompatible(k.format)) && (p.si->info().usage & k.usage) == k.usage) {
      a = p.si->createView(k); avc[vid] = { iv, a }; }
  }
  if (a == nullptr) { s2_end(i, 1); return nullptr; }   /* cannot alias: execute the copy now */
  al_dst[st][s] = v->res; al_real[st][s] = iv; n_al++; c_alias++;
  return a;
}
/* hook in b1dx_c_srvs: the view actually bound for (st, slot) */
static Rc<DxvkImageView> s2_srv(int st, uint32_t s, uint32_t vid, View *v, Rc<DxvkImageView> iv) {
  using namespace s2;
  if (st < 0 || st > 5 || s >= 128) return iv;
  if (al_dst[st][s]) { al_dst[st][s] = 0; al_real[st][s] = nullptr; n_al--; }
  Rc<DxvkImageView> a = s2_alias_for(st, s, vid, v, iv);
  return a != nullptr ? a : iv;
}
/* hook in b1dx_c_copy_region (images): true = recorded as pending, the caller skips the copy */
static bool s2_copy(uint32_t dst, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint32_t src, uint32_t ssub, const uint32_t *box) {
  using namespace s2;
  stv_set(dst, -1);
  if (!ca_mode()) return false;
  Obj *d = robj(dst), *s = robj(src);
  s2_before_read(src);
  bool el = d && s && d->kind == 2 && s->kind == 2 && dst != src && dst != g_bb && src != g_bb && pend.size() < 64;
  if (el) {
    const DxvkImageCreateInfo &di = d->img->info(), &si = s->img->info();
    el = di.type == VK_IMAGE_TYPE_2D && si.type == VK_IMAGE_TYPE_2D && di.mipLevels == 1 && si.mipLevels == 1 && di.numLayers == 1 && si.numLayers == 1
      && d->fmt == s->fmt && di.format == si.format && di.sampleCount == si.sampleCount
      && !(lookupFormatInfo(d->fmt)->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
      && di.extent.width == si.extent.width && di.extent.height == si.extent.height
      && !dsub && !ssub && !x && !y && !z
      && (!box || (!box[0] && !box[1] && !box[2] && box[3] == si.extent.width && box[4] == si.extent.height && box[5] == 1u));
    for (int k = 0; k < 64 && el; k++) if (uav_res[k] && (uav_res[k] == dst || uav_res[k] == src)) el = false;
  }
  if (!el) { c_inel++; s2_before_write(dst, false);
    if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "copy %u -> %u not eligible (executed)", src, dst); lg(t); }
    return false; }
  s2_before_write(dst, true);                       /* a pending copy into dst is dropped; dst as a pending source hands over / executes */
  pend.push_back({ dst, src, d->img, s->img }); c_def++;
  if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "copy %u -> %u deferred", src, dst); lg(t); }
  if (ca_log() && c_def <= ca_log()) {
    const DxvkImageCreateInfo &di = d->img->info(), &si = s->img->info();
    std::fprintf(stderr, "[s2] copy %u -> %u (%ux%u fmt %d) deferred; src usage %x stages %x layout %d, dst usage %x stages %x layout %d, hand-over %s\n", src, dst,
      si.extent.width, si.extent.height, (int)si.format, si.usage, si.stages, (int)si.layout, di.usage, di.stages, (int)di.layout, s2_can_swap(pend.back()) ? "possible" : "not possible");
  }
  /* slots that hold a view of dst now read src */
  for (int st = 0; st < 6; st++) for (uint32_t k = 0; k < 128; k++) {
    uint32_t vid = g_bc.srv[st][k]; if (!vid || vid == BC_UNK) continue;
    View *v = g_views.get(vid); if (!v || v->type != 1 || v->res != dst) continue;
    if (al_dst[st][k]) { al_dst[st][k] = 0; al_real[st][k] = nullptr; n_al--; }
    Rc<DxvkImageView> a = s2_alias_for(st, k, vid, v, v->iv);
    if (a == nullptr) break;   /* the copy was executed instead */
    s2_bind_slot(st, k, std::move(a));
    if (s2_find_dst(dst) < 0) break;
  }
  return true;
}
static void s2_uav(uint32_t slot, View *v) {   /* hook in b1dx_c_uavs: compute may write the resource from now on */
  if (slot >= 64) return;
  s2::uav_res[slot] = v ? v->res : 0;
  if (v) s2_written(v->res);
}
static void s2_clear_rtv(View *v) {
  Obj *o = robj(v->res); bool whole = o && o->kind == 2 && o->img->info().mipLevels == 1 && o->img->info().numLayers == 1;
  s2_before_write(v->res, whole); if (s2::ca_mode() >= 2) s2::clrp_set(v->res, true);
}
static void s2_clear_dsv(View *v, VkImageAspectFlags asp, uint32_t stencil) {
  using namespace s2;
  s2_before_write(v->res, false); if (ca_mode() >= 2) clrp_set(v->res, true);
  if (!(asp & VK_IMAGE_ASPECT_STENCIL_BIT)) return;   /* depth-only clear: the stencil plane keeps its value */
  Obj *o = robj(v->res); bool whole = o && o->kind == 2 && o->img->info().mipLevels == 1 && o->img->info().numLayers == 1;
  if (lrz_mode() && whole) { stv_set(v->res, (int)(stencil & 0xffu)); c_st_clear++; } else stv_set(v->res, -1);
  if (lg_on()) { char t[96]; std::snprintf(t, sizeof t, "clear res %u aspects %x stencil %u whole %d", v->res, asp, stencil & 0xffu, whole); lg(t); }
}
/* hook in b1dx_c_dss, after the state was emitted */
static void s2_dss(const DxvkDepthStencilState &ds, uint32_t ref) { s2::lrz_ds = ds; s2::lrz_ref = ref & 0xffu; s2::lrz_have = true; s2::lrz_mod = false; }
static inline bool s2_stencil_true(const DxvkStencilOp &op, uint32_t ref, uint32_t val) {
  uint32_t m = op.compareMask(), r = ref & m, s = val & m;   /* Vulkan / D3D11: (ref & mask) OP (stored & mask) */
  switch (op.compareOp()) {
    case VK_COMPARE_OP_ALWAYS: return true;            case VK_COMPARE_OP_EQUAL: return r == s;
    case VK_COMPARE_OP_NOT_EQUAL: return r != s;       case VK_COMPARE_OP_LESS: return r < s;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return r <= s;   case VK_COMPARE_OP_GREATER: return r > s;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return r >= s; default: return false; }
}
/* hook in pre_draw: pending copies whose src/dst is a bound target are executed; stencil test drop */
static void s2_pre_draw() {
  using namespace s2;
  if (!pend.empty() || !clrp.empty()) {
    for (int i = 0; i < 8; i++) if (g_cs.rtv[i]) { View *v = g_views.get(g_cs.rtv[i]); if (v && v->type == 2) { s2_before_write(v->res, false); clrp_set(v->res, false); } }
    if (g_cs.dsv) { View *v = g_views.get(g_cs.dsv); if (v && v->type == 3) { s2_before_write(v->res, false); clrp_set(v->res, false); } }
  }
  if (!lrz_have || !lrz_ds.stencilTest()) return;   /* (a state without stencil test is never modified: lrz_mod is false) */
  uint32_t r = 0; if (g_cs.dsv) { View *v = g_views.get(g_cs.dsv); if (v && v->type == 3) r = v->res; }
  DxvkStencilOp f = lrz_ds.stencilOpFront(), b = lrz_ds.stencilOpBack();
  bool writes = f.writeMask() || b.writeMask();      /* normalized: write mask != 0 iff some op != KEEP */
  if (writes) stv_set(r, -1);
  int val = stv_get(r);
  bool triv = lrz_mode() && !writes && r && val >= 0 && s2_stencil_true(f, lrz_ref, (uint32_t)val) && s2_stencil_true(b, lrz_ref, (uint32_t)val);
  if (triv != lrz_mod) {
    DxvkDepthStencilState e = lrz_ds; if (triv) { e.setStencilTest(false); e.normalize(); }
    emit([e] (DxvkContext* c) { c->setDepthStencilState(e); }); lrz_mod = triv; c_lrz_sw++;
  }
  if (triv) c_lrz_drop++;
  if (lg_on()) { char t[200]; std::snprintf(t, sizeof t, "draw ds res %u known %d ref %u front op %d mask %x wm %x back op %d mask %x wm %x depth %d/%d -> %s",
      r, val, lrz_ref, (int)f.compareOp(), f.compareMask(), f.writeMask(), (int)b.compareOp(), b.compareMask(), b.writeMask(), lrz_ds.depthTest(), lrz_ds.depthWrite(),
      triv ? "stencil test dropped" : writes ? "writes stencil" : "kept"); lg(t); }
}
static void s2_stats() {   /* own log line (the [b1] COM: line is already at its buffer size in shadow mode) */
  using namespace s2;
  std::fprintf(stderr, "[b1s2] copyalias %d: deferred %ld (alias binds %ld), executed before a write %ld / for a read %ld, dropped %ld, storage hand-overs %ld (full overwrites without %ld), pending %zu, not eligible %ld | lrzfix %d: stencil test dropped on %ld draws (%ld state switches), whole stencil clears %ld, invalidations %ld\n",
    ca_mode(), c_def, c_alias, c_wr, c_rd, c_drop, c_swap, c_noswap, pend.size(), c_inel, lrz_mode(), c_lrz_drop, c_lrz_sw, c_st_clear, c_st_inval);
}

/* ---- perf73 research diagnostic (BSI_B1_SHADOWSIG=1, off by default; nothing runs without it): are the shadow-depth passes of
   a stereo frame rendered twice (once per eye view, identical) so that the second could be skipped? Every pass into a
   depth-only square target >= 512 gets a signature of everything that decides its result: per draw the shaders, input
   layout, vertex/index buffers + offsets, draw arguments, viewport, rasterizer/depth state ids and the CONTENT of the
   VS/PS constant buffers in slots 0-3 (hashed when written; buffer ids are not used, the game may rename them), plus its
   clears. Two passes with the same signature into the same depth target render the same image. One line per frame for
   the first 40 frames with shadow passes, then every 300 frames, plus a running summary. ---- */
namespace shsig {
  static int on = -1; static std::vector<uint64_t> cbh; static uint32_t cb[2][4], sh[2], il, vb[4], vbs[4], vbo[4], ib, ibo, ibf;
  static bool active; static uint64_t h; static uint32_t dsv, nd; static long frames, frames_sh, passes, dup_passes, dup_frames;
  struct P { uint32_t dsv, nd; uint64_t h; float vp[4]; }; static std::vector<P> fr;
  static inline bool enabled() { if (on < 0) { const char *e = std::getenv("BSI_B1_SHADOWSIG"); on = e && std::atoi(e) > 0; } return on > 0; }
  static inline uint64_t mix(uint64_t x, uint64_t v) { x ^= v + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2); return x * 1099511628211ull; }
  static uint64_t hdata(const void *d, size_t n) { uint64_t x = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)d; for (size_t i = 0; i < n; i++) x = (x ^ p[i]) * 1099511628211ull; return x; }
  static void wrote(uint32_t id, uint32_t off, uint32_t n, const void *d) { if (!enabled() || !id) return; if (cbh.size() <= id) cbh.resize(id + 1024, 0);
    cbh[id] = off == 0 ? hdata(d, n) : mix(cbh[id], mix(off, hdata(d, n))); }
  static void close() { if (active) { P p = { dsv, nd, h, { 0, 0, 0, 0 } }; if (g_cs.nvp) for (int i = 0; i < 4; i++) p.vp[i] = g_cs.vp[0][i]; fr.push_back(p); } active = false; }
  static void rts(uint32_t n, const uint32_t *rtvs, uint32_t d) {
    if (!enabled()) return; close();
    for (uint32_t i = 0; i < n && i < 8; i++) if (rtvs[i]) return;
    View *v = g_views.get(d); if (!v || v->type != 3) return;
    VkExtent3D e = v->iv->mipLevelExtent(0); if (e.width != e.height || e.width < 512) return;
    active = true; dsv = d; nd = 0; h = mix(1469598103934665603ull, d); }
  static void clear(uint32_t d, uint32_t fl, float depth, uint32_t st) { if (!enabled() || !active) return; uint32_t df; std::memcpy(&df, &depth, 4); h = mix(mix(mix(h, 0xC1EA4ull ^ d), fl), ((uint64_t)df << 32) | st); }
  static void draw(const uint32_t *args, int na) {
    if (!enabled() || !active) return;
    uint64_t x = mix(mix(mix(h, sh[0]), sh[1]), il);
    for (int i = 0; i < 4; i++) x = mix(mix(mix(x, vb[i]), vbs[i]), vbo[i]);
    x = mix(mix(mix(x, ib), ibo), ibf);
    for (int i = 0; i < na; i++) x = mix(x, args[i]);
    for (uint32_t i = 0; i < g_cs.nvp && i < 1; i++) for (int k = 0; k < 6; k++) { uint32_t f; std::memcpy(&f, &g_cs.vp[i][k], 4); x = mix(x, f); }
    x = mix(mix(x, g_cs.rs), g_cs.dss);
    for (int st = 0; st < 2; st++) for (int s2 = 0; s2 < 4; s2++) { uint32_t id = cb[st][s2]; x = mix(x, id && id < cbh.size() ? cbh[id] : 0); }
    h = x; nd++; }
  static void present() {
    if (!enabled()) return; close(); frames++;
    if (fr.empty()) return;
    frames_sh++; passes += (long)fr.size(); int dup = 0;
    for (size_t i = 0; i < fr.size(); i++) for (size_t j = 0; j < i; j++) if (fr[i].h == fr[j].h && fr[i].dsv == fr[j].dsv && fr[i].nd) { dup++; break; }
    dup_passes += dup; if (dup) dup_frames++;
    if (frames_sh <= 40 || frames_sh % 300 == 0) {
      std::string t; char b[160];
      for (size_t i = 0; i < fr.size(); i++) { std::snprintf(b, sizeof b, " | #%zu dsv %u vp %.0f,%.0f %.0fx%.0f draws %u sig %016llx", i, fr[i].dsv, fr[i].vp[0], fr[i].vp[1], fr[i].vp[2], fr[i].vp[3], fr[i].nd, (unsigned long long)fr[i].h); t += b; }
      std::fprintf(stderr, "[b1sig] frame %ld (stereo %d): %zu shadow-depth passes, %d identical to an earlier pass of the frame%s\n", frames, (int)b1vr::on, fr.size(), dup, t.c_str());
      std::fprintf(stderr, "[b1sig] so far: %ld frames with shadow passes, %ld passes, %ld duplicates in %ld frames\n", frames_sh, passes, dup_passes, dup_frames); }
    fr.clear(); }
}
static void shsig_frame() { if (shsig::enabled()) shsig::present(); }
