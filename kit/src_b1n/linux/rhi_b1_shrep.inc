/* b115 shrep (b116: only the slots the bound shaders declare): SHADOW-MAP REPEAT COUNTER (diagnostic only; BSI_B1_SHREP=1, default 0 = no hooks run, nothing changes).
   The user's "browser" idea (28 Oct): avoid redoing work whose inputs did not change. Before any cache is built this
   counts how often a depth-only pass (UE3 shadow depth: 2048x2048 spot / point / cascade maps) is fed EXACTLY the same
   inputs as the same pass in the previous frame, so its output would be bit-identical.

   Every draw inside a depth-only pass (render-target set = a DSV and no colour target) gets a 64-bit hash of everything
   that decides its pixels: VS/PS/GS/HS/DS ids, input layout, topology, rasterizer / depth-stencil / blend state ids,
   stencil ref, sample mask, viewport 0 + scissor 0, the draw arguments, the vertex / index buffer ids + offsets + strides,
   and the CONTENTS of what the draw reads from buffers the CPU writes: every bound VS/PS constant buffer (a CPU shadow
   copy of every buffer written through update / unmap / bufwrite / buffer_write), the index range of a dynamic index
   buffer and the vertex range (scanned from the indices, or [first, first + count)) of a dynamic vertex buffer.
   Buffers / textures the CPU writes only rarely (static meshes, material textures) enter by id + a write generation that
   changes at every create / write / copy / release, so a reused id never matches. A draw is UNKNOWN (never counted as a
   repeat) when it samples an image the GPU writes (render target / depth / UAV), reads a dynamic buffer whose range is
   not known, or a bound buffer has no CPU copy (GPU-copied).
   The pass hash = the clears of its DSV inside the pass + the draw hashes in order. Passes are keyed (DSV resource,
   n-th use of that DSV this frame) and compared with the previous frame's pass of the same key:
     identical  = same pass hash (the whole map would come out bit-identical, the pass could be skipped / kept)
     draws kept = draws whose hash occurs in the previous frame's pass (depth LESS / LEQUAL with no colour is order-independent
                  per texel, so a static/dynamic split cache could keep these: shown as the upper bound for that idea)
   [b1rep] every 600 frames: depth-only passes and draws per frame, % identical, % draws kept, unknown reasons, and the top
   keys (size, draws, identical %, kept %; "#n" = the n-th pass into the same image this frame). */
namespace {
namespace shrep {
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHREP"); v = e && *e ? std::atoi(e) > 0 : 0; } return v; }
  static int mode() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SHREP"); v = e && *e ? std::atoi(e) : 0; } return v; }
  static inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
  static uint64_t hbytes(uint64_t h, const uint8_t *p, size_t n) {
    uint64_t a = h ^ (n * 0x100000001b3ull); size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); a = (a ^ w) * 0x9e3779b97f4a7c15ull; a ^= a >> 29; }
    uint64_t t = 0; if (i < n) std::memcpy(&t, p + i, n - i); a = (a ^ t ^ (uint64_t)n) * 0xc2b2ae3d27d4eb4full; return mix(h, a ^ (a >> 31)); }
  struct Buf { std::vector<uint8_t> cpu; bool have = false; uint64_t chash = 0; bool cdirty = true; };
  static std::unordered_map<uint32_t, Buf> bufs;          /* buffer id -> CPU shadow copy (buffers the CPU writes after creation) */
  static std::unordered_map<uint32_t, uint64_t> gen;      /* resource id -> write generation */
  static uint64_t g_gen = 1;
  static const size_t CAP = 16u << 20;                     /* no shadow copy above 16 MB (the draw then reads "no CPU copy") */
  static void bump(uint32_t id) { gen[id] = ++g_gen; }
  static void created(uint32_t id, uint32_t size, const void *data, bool hostvis) { if (!on() || !id) return; bump(id); bufs.erase(id);
    if (data && hostvis && size <= CAP) {   /* dynamic buffers with initial data; static ones enter by id + generation */ Buf &b = bufs[id]; b.cpu.assign((const uint8_t *)data, (const uint8_t *)data + size); b.have = true; b.cdirty = true; } }
  static void wrote(uint32_t id, uint32_t off, uint32_t size, const void *data, uint32_t bufsize) {
    if (!on() || !id || !data) return; bump(id); if (bufsize > CAP) { bufs.erase(id); return; }
    Buf &b = bufs[id]; if (b.cpu.size() != bufsize) { b.cpu.assign(bufsize, 0); b.have = off == 0 && size >= bufsize; }   /* a partial first write: bytes before it unknown */
    if (off < bufsize) { uint32_t n = std::min(size, bufsize - off); std::memcpy(b.cpu.data() + off, data, n); }
    if (off == 0 && size >= bufsize) b.have = true; b.cdirty = true; }
  static void gpu_wrote(uint32_t id) { if (!on() || !id) return; bump(id); auto it = bufs.find(id); if (it != bufs.end()) it->second.have = false; }   /* a GPU copy into a buffer */
  static void tex_written(uint32_t id) { if (on() && id) bump(id); }
  static void released(uint32_t id) { if (!on() || !id) return; bump(id); bufs.erase(id); }
  /* passes */
  struct Pass { uint32_t dsv = 0, res = 0, w = 0, h = 0; int nth = 0; uint64_t h64 = 0; std::vector<uint64_t> dr; long unk = 0; };
  static Pass cur; static bool open = false;
  static std::map<std::pair<uint32_t, int>, Pass> prev, now;   /* key (DSV resource, n-th use this frame) */
  static std::unordered_map<uint32_t, int> uses;               /* DSV resource -> passes into it this frame */
  struct KAcc { long seen = 0, ident = 0, draws = 0, kept = 0, unk = 0; uint32_t w = 0, h = 0; };
  static std::map<std::pair<uint32_t, int>, KAcc> acc;
  static long f_frames, f_passes, f_ident, f_draws, f_kept, f_unkdraws, f_first; static long n_noslots;   /* b116: draws with a shader whose declarations could not be read (all slots hashed) */
  enum { U_GPUTEX = 0, U_DYNRANGE, U_NOCPU, U_UNKSLOT, U_N }; static long ureason[U_N];
  static const char *const uname[U_N] = { "samples a GPU-written image", "dynamic VB/IB range unknown", "buffer without CPU copy", "unknown binding" };
  /* b121 OUTPUT mode (BSI_B1_SHREP=2): UE3 feeds camera-relative values into the shadow-depth vertex constants, so b116's INPUT hash differs every
     frame even when the light-space depth map that comes out is the same. Every 300 frames one 2048x2048 depth-only pass (key = DSV resource, n-th use)
     is copied to a host buffer at the end of the pass in two consecutive frames; three frames later (one GPU wait) both copies are compared byte for
     byte. [b1rep] out: per key, cycles compared, cycles identical, mean % of texels that differ. Keys rotate, so every map gets sampled. Diagnostic. */
  struct OutK { long cmp = 0, same = 0; double diffpct = 0; uint32_t w = 0, h = 0; };
  static std::map<std::pair<uint32_t, int>, OutK> outacc;
  static std::vector<std::pair<uint32_t, int>> outkeys; static size_t outrot;
  static std::pair<uint32_t, int> outkey(0u, -1); static Rc<DxvkBuffer> outbuf[2]; static bool outhave[2]; static long outphase = -1; static VkDeviceSize outsz;
  static void out_copy(const Pass &p) {   /* at close(): the pass's depth output -> outbuf[phase] */
    if (mode() != 2 || outphase < 0 || outphase > 1 || p.res != outkey.first || p.nth != outkey.second) return;
    Obj *o = robj(p.res); if (!o || o->kind != 2 || o->img == nullptr) return;
    VkExtent3D ext = o->img->mipLevelExtent(0); VkDeviceSize sz = (VkDeviceSize)ext.width * ext.height * 4u;
    if (outbuf[outphase] == nullptr || outsz != sz) { DxvkBufferCreateInfo bi = { }; bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      bi.access = VK_ACCESS_TRANSFER_WRITE_BIT; bi.debugName = "B1 shrep out"; outbuf[outphase] = g_dev->createBuffer(bi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); outsz = sz; }
    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
    emit([rb = outbuf[outphase], fmt = o->fmt, img = o->img, sl, ext] (DxvkContext* c) { c->copyImageToBuffer(rb, 0, 0, 0, fmt, img, sl, VkOffset3D{ 0, 0, 0 }, ext); });
    outhave[outphase] = true; }
  static void out_frame() {   /* frame end (after close): advance the cycle */
    if (mode() != 2) return;
    long ph = (long)(f_frames % 300);
    if (ph == 0) {   /* pick the next key among this frame's 2048x2048 passes */
      outkeys.clear(); for (auto &e : now) if (e.second.w == 2048 && e.second.h == 2048 && !e.second.dr.empty()) outkeys.push_back(e.first);
      if (outkeys.empty()) { outphase = -1; return; } outkey = outkeys[outrot++ % outkeys.size()]; outhave[0] = outhave[1] = false; outphase = 0; return; }
    if (ph == 1) { outphase = 1; return; }
    if (ph == 2) { outphase = 2; return; }
    if (ph == 4 && outphase == 2) {
      outphase = -1; if (!outhave[0] || !outhave[1]) return;
      ctx_flush(); cs_sync(); g_dev->waitForIdle();
      const uint32_t *a = (const uint32_t *)outbuf[0]->mapPtr(0), *b = (const uint32_t *)outbuf[1]->mapPtr(0); size_t n = outsz / 4, d = 0;
      for (size_t i = 0; i < n; i++) d += (a[i] != b[i]);
      OutK &k = outacc[outkey]; k.cmp++; if (!d) k.same++; k.diffpct += 100.0 * (double)d / (double)n; k.w = 2048; k.h = 2048; } }
  static void close() {
    if (!open) return; open = false;
    std::pair<uint32_t, int> k(cur.res, cur.nth); KAcc &a = acc[k]; a.w = cur.w; a.h = cur.h; a.seen++; a.draws += (long)cur.dr.size(); a.unk += cur.unk;
    f_passes++; f_draws += (long)cur.dr.size(); f_unkdraws += cur.unk;
    auto it = prev.find(k);
    if (it == prev.end()) f_first++;
    else { const Pass &p = it->second;
      if (!cur.unk && !p.unk && p.h64 == cur.h64 && p.dr.size() == cur.dr.size()) { a.ident++; f_ident++; }
      std::vector<uint64_t> x = p.dr, y = cur.dr; std::sort(x.begin(), x.end()); std::sort(y.begin(), y.end());
      size_t i = 0, j = 0; long m = 0; while (i < x.size() && j < y.size()) { if (x[i] == y[j]) { m++; i++; j++; } else if (x[i] < y[j]) i++; else j++; }   /* multiset intersection */
      a.kept += m; f_kept += m; }
    out_copy(cur);   /* b121 output mode */
    now[k] = std::move(cur); cur = Pass(); }
  /* b1dx_c_rts: the new target set */
  static void on_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv) {
    if (!on()) return; close();
    for (uint32_t i = 0; i < n; i++) if (rtvs && rtvs[i]) return;   /* colour bound: not a depth-only pass */
    View *v = g_views.get(dsv); if (!v || v->type != 3 || v->iv == nullptr) return;
    cur = Pass(); cur.dsv = dsv; cur.res = v->res; VkExtent3D e = v->iv->mipLevelExtent(0); cur.w = e.width; cur.h = e.height;
    cur.nth = uses[v->res]++; cur.h64 = mix(0x5eedull, ((uint64_t)e.width << 32) | e.height); cur.h64 = mix(cur.h64, v->iv->info().format); open = true; }
  static void on_clear_dsv(uint32_t res, uint32_t flags, float depth, uint32_t stencil) {
    if (!on() || !open || res != cur.res) return; uint32_t d; std::memcpy(&d, &depth, 4); cur.h64 = mix(mix(mix(cur.h64, 0xc1ea7ull), flags), ((uint64_t)d << 32) | (stencil & 0xffu)); }
  static bool cb_hash(uint32_t id, uint64_t &h, int &why) {
    if (!id || id == BC_UNK) { h = mix(h, id); return true; }
    Obj *o = robj(id); if (!o || o->kind != 1) { h = mix(h, 0xdeadull); return true; }
    auto it = bufs.find(id);
    if (it == bufs.end()) { auto g = gen.find(id); h = mix(mix(h, id), g == gen.end() ? 0 : g->second); return true; }   /* never written by the CPU after creation: id + generation */
    Buf &b = it->second; if (!b.have) { why = U_NOCPU; return false; }
    if (b.cdirty) { b.chash = hbytes(0x1234ull, b.cpu.data(), b.cpu.size()); b.cdirty = false; } h = mix(h, b.chash); return true; }
  static bool hostvis(Obj *o) { return (o->buf->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0; }
  /* a VB / IB slice: CPU-written (shadow copy) -> its bytes over [a, a + n); otherwise id + generation */
  static bool range_hash(uint32_t id, uint64_t a, uint64_t n, uint64_t &h, int &why) {
    Obj *o = robj(id); if (!o || o->kind != 1) { h = mix(h, 0xdead1ull); return true; }
    auto it = bufs.find(id);
    if (it == bufs.end() || !hostvis(o)) { if (it != bufs.end() && !it->second.have) { why = U_NOCPU; return false; }
      auto g = gen.find(id); h = mix(mix(h, id), g == gen.end() ? 0 : g->second); return true; }
    Buf &b = it->second; if (!b.have) { why = U_NOCPU; return false; }
    if (a + n > b.cpu.size()) { why = U_DYNRANGE; return false; }
    h = hbytes(h, b.cpu.data() + a, (size_t)n); return true; }
  static void on_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    if (!on() || !open) return;
    uint64_t h = 0x9a55ull; int why = -1;
    for (int st = 0; st < 6; st++) h = mix(h, dc88::sh[st]);
    h = mix(h, g_bc.il); h = mix(h, g_m79_topo); h = mix(h, g_cs.rs); h = mix(h, g_cs.dss); h = mix(h, g_cs.bs); h = mix(h, ((uint64_t)g_cs.sref << 32) | g_cs.mask);
    if (g_cs.nvp) h = hbytes(h, (const uint8_t *)g_cs.vp[0], sizeof g_cs.vp[0]); if (g_cs.nsc) h = hbytes(h, (const uint8_t *)g_cs.sc[0], sizeof g_cs.sc[0]);
    { RS *rs = g_rss.get(g_cs.rs); if (rs && rs->scissor) h = mix(h, 0x5c15ull); }
    if (dc) h = hbytes(h, (const uint8_t *)dc, sizeof *dc); else h = hbytes(h, (const uint8_t *)dic, sizeof *dic);
    /* b116: only the slots each bound shader DECLARES (UE3 leaves stale SRVs / constant buffers bound that the shader never reads) */
    uint64_t msrv[5]; uint32_t mcb[5], mss[5];
    for (int st = 0; st < 5; st++) { msrv[st] = 0; mcb[st] = 0; mss[st] = 0; if (dc88::sh[st]) { if (!b1dx_shader_slots(dc88::sh[st], &msrv[st], &mcb[st], &mss[st])) n_noslots++; } }
    /* constant buffers */
    for (int st = 0; st < 5 && why < 0; st++) for (int s = 0; s < 14 && why < 0; s++) if (mcb[st] >> s & 1u) cb_hash(g_bc.cb[st][s], h, why);
    /* images sampled (VS/PS: masked materials) */
    for (int st = 0; st < 2 && why < 0; st++) for (int s = 0; s < 64; s++) { if (!(msrv[st] >> s & 1u)) continue; uint32_t r = s < 128 ? dc88::srv_res[st][s] : 0; if (!r) continue;
      Obj *o = robj(r); if (o && o->kind == 2 && o->img != nullptr && (o->img->info().usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT))) { why = U_GPUTEX; break; }
      if (o && o->kind == 1) { if (!cb_hash(r, h, why)) break; continue; }   /* a buffer SRV */
      auto g = gen.find(r); h = mix(mix(h, ((uint64_t)st << 40) | ((uint64_t)s << 32) | r), g == gen.end() ? 0 : g->second); }
    for (int st = 0; st < 2 && why < 0; st++) for (int s = 0; s < 16; s++) if (mss[st] >> s & 1u) h = mix(h, g_bc.ss[st][s]);
    /* index range, then the vertex range of every binding the layout reads */
    uint64_t vmin = 0, vmax = 0; bool vr = false;   /* vertex index range [vmin, vmax] */
    if (why < 0 && dic) {
      uint32_t ib = g_bc.ib; if (!ib || ib == BC_UNK || g_bc.ibo == BC_UNK || g_bc.ibf == BC_UNK) why = U_UNKSLOT;
      else { uint32_t isz = g_bc.ibf == (uint32_t)VK_INDEX_TYPE_UINT32 ? 4 : 2; uint64_t a = (uint64_t)g_bc.ibo + (uint64_t)dic->firstIndex * isz, n = (uint64_t)dic->indexCount * isz;
        h = mix(h, ((uint64_t)isz << 32) | g_bc.ibo);
        if (range_hash(ib, a, n, h, why)) { Obj *o = robj(ib); auto it = bufs.find(ib);
          if (o && hostvis(o) && it != bufs.end() && it->second.have && a + n <= it->second.cpu.size()) {   /* dynamic IB: scan for the vertex range */
            const uint8_t *p = it->second.cpu.data() + a; uint64_t mn = ~0ull, mx = 0;
            for (uint32_t i = 0; i < dic->indexCount; i++) { uint32_t x = isz == 4 ? (uint32_t)(p[4 * i] | (p[4 * i + 1] << 8) | (p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24)) : (uint32_t)(p[2 * i] | (p[2 * i + 1] << 8)); mn = std::min<uint64_t>(mn, x); mx = std::max<uint64_t>(mx, x); }
            if (dic->indexCount) { int64_t b = dic->vertexOffset; if ((int64_t)mn + b >= 0) { vmin = mn + b; vmax = mx + b; vr = true; } } } } } }
    else if (why < 0 && dc && dc->vertexCount) { vmin = dc->firstVertex; vmax = (uint64_t)dc->firstVertex + dc->vertexCount - 1; vr = true; }
    if (why < 0) { IL *il = g_ils.get(g_bc.il == BC_UNK ? 0 : g_bc.il);
      if (!il) { if (g_bc.il == BC_UNK) why = U_UNKSLOT; }
      else for (uint32_t k = 0; k < il->nb && why < 0; k++) { DxvkVertexBinding bd = il->in[il->na + k].binding(); uint32_t s = bd.binding; if (s >= 32) continue;
        uint32_t vb = g_bc.vb[s], st = g_bc.vbs[s], bo = g_bc.vbo[s]; if (vb == BC_UNK || st == BC_UNK || bo == BC_UNK) { why = U_UNKSLOT; break; }
        h = mix(mix(h, ((uint64_t)s << 32) | st), bo); if (!vb) continue;
        Obj *o = robj(vb); auto it = bufs.find(vb);
        if (!o || o->kind != 1 || !hostvis(o) || it == bufs.end()) { range_hash(vb, 0, 0, h, why); continue; }   /* static: id + generation */
        uint64_t lo, n; uint32_t ninst = dc ? dc->instanceCount : dic->instanceCount, finst = dc ? dc->firstInstance : dic->firstInstance;
        if (bd.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) { lo = (uint64_t)bo + (uint64_t)finst * st; n = (uint64_t)std::max<uint32_t>(ninst, 1) * st; }
        else { if (!vr) { why = U_DYNRANGE; break; } lo = (uint64_t)bo + vmin * st; n = (vmax - vmin + 1) * st; }
        if (!st) n = it->second.cpu.size() > bo ? std::min<uint64_t>(64, it->second.cpu.size() - bo) : 0;   /* stride 0: one element */
        range_hash(vb, lo, n, h, why); } }
    if (why >= 0) { ureason[why]++; cur.unk++; h = mix(h, ++g_gen); }   /* never equal to anything */
    cur.dr.push_back(h); cur.h64 = mix(cur.h64, h); }
  static void frame() {
    if (!on()) return; close(); out_frame(); /* b121: needs this frame's passes in now */ prev.swap(now); now.clear(); uses.clear(); f_frames++;
    if (f_frames % 600) return;
    double F = (double)f_frames > 0 ? 600.0 : 1.0;
    std::string u; for (int k = 0; k < U_N; k++) if (ureason[k]) { char b[96]; std::snprintf(b, sizeof b, " %s %ld,", uname[k], ureason[k]); u += b; }
    std::fprintf(stderr, "[b1rep] shadow-map repeats (b116, last 600 frames, shader-declared slots only; %ld slot scans failed): depth-only passes %.1f/frame, draws %.1f/frame; passes IDENTICAL to the previous frame %.1f %% (%ld of %ld compared, %ld first-seen); draws kept (hash seen in the previous frame's pass) %.1f %%; unknown draws %.1f %%%s%s\n",
                 n_noslots, f_passes / F, f_draws / F, f_passes - f_first ? 100.0 * f_ident / (f_passes - f_first) : 0.0, f_ident, f_passes - f_first, f_first,
                 f_draws ? 100.0 * f_kept / f_draws : 0.0, f_draws ? 100.0 * f_unkdraws / f_draws : 0.0, u.empty() ? "" : " | unknown:", u.c_str());
    std::vector<std::pair<std::pair<uint32_t, int>, KAcc>> v(acc.begin(), acc.end());
    std::sort(v.begin(), v.end(), [] (const auto &a, const auto &b) { return a.second.draws > b.second.draws; });
    std::string o; for (size_t j = 0; j < v.size() && j < 16; j++) { const KAcc &a = v[j].second; char b[200];
      std::snprintf(b, sizeof b, " | res %u #%d %ux%u: %.2f/frame, %.1f draws, identical %.0f %%, kept %.0f %%, unknown %.0f %%", v[j].first.first, v[j].first.second, a.w, a.h, a.seen / F, a.seen ? (double)a.draws / a.seen : 0.0,
                    a.seen ? 100.0 * a.ident / a.seen : 0.0, a.draws ? 100.0 * a.kept / a.draws : 0.0, a.draws ? 100.0 * a.unk / a.draws : 0.0); o += b; }
    std::fprintf(stderr, "[b1rep] by pass (DSV resource, n-th use per frame; %zu keys)%s\n", v.size(), o.c_str());
    if (mode() == 2 && !outacc.empty()) { std::string q; for (auto &e : outacc) { char b[160]; std::snprintf(b, sizeof b, " | res %u #%d: %ld/%ld cycles identical, %.2f %% texels differ (mean)", e.first.first, e.first.second, e.second.same, e.second.cmp, e.second.cmp ? e.second.diffpct / e.second.cmp : 0.0); q += b; }
      std::fprintf(stderr, "[b1rep] out (b121, 2048x2048 depth OUTPUT compared frame to frame, one key per 300 frames)%s\n", q.c_str()); }
    acc.clear(); n_noslots = 0; f_passes = f_ident = f_draws = f_kept = f_unkdraws = f_first = 0; for (int k = 0; k < U_N; k++) ureason[k] = 0; }
}
}
