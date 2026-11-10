/* rhi_b1_skincensus.inc -- b137 BSI_B1_SKINCENSUS=1 (diagnostics only; no picture change; default off). #included at the end
   of rhi_b1_shadoweo.inc. Sizes the "skin once per frame" lever: every draw whose VS is a GPU-skin VS (rhi_b1_shader.cpp
   detects the 12-instruction cb3 bone blend on the game's code, b1dx_shader_is_skin) is keyed by what its skinned
   vertices depend on: vertex buffer 0 (+ offset), index buffer (+ offset), first index / count / base vertex and a hash
   of the bone constants it sees (VS cb3 content from sb87's CPU store of constant-buffer writes; 0 = not captured).
   Draws with the same key re-skin the same vertices (other passes, other VS, shadow cascades, the right-eye replay).
   Every 300 frames, per frame: skinned draws, vertices (or indices) skinned in total vs in the unique keys, distinct bone
   sets (= skinned mesh instances), pp76 right-eye replays, and the split by pass class (colour targets or depth-only, and
   the depth target's size). Ceiling of the lever (b136 SKINPROBE, blend made free): Fair -1.9 ms, PreLottery -1.6,
   TWN2 -4.1; skin-once can save at most the (total - unique) / total share of that. */
extern "C" int b1dx_shader_is_skin(uint32_t id);   /* rhi_b1_shader.cpp */
namespace skc {
  static int on() { static int v = -1; if (v < 0) { const char *e = std::getenv("BSI_B1_SKINCENSUS"); v = e && std::atoi(e) > 0; } return v; }
  struct Cls { double draws = 0, verts = 0; };
  static std::unordered_map<uint64_t, double> keys;        /* this frame: key -> vertices of that draw */
  static std::unordered_set<uint64_t> bones;               /* this frame: distinct bone-constant hashes */
  static std::map<std::string, Cls> cls;                    /* accumulated over the report window */
  static double a_draws, a_verts, a_uverts, a_ukeys, a_bones, a_replay, a_nobones, a_skinvs; static long n_fr, n_tot;
  static double f_draws, f_verts, f_replay, f_nobones;
  static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001b3ull; }
  static void post_draw(const VkDrawIndirectCommand *dc, const VkDrawIndexedIndirectCommand *dic) {
    if (!on() || !g_m79_vs || !b1dx_shader_is_skin(g_m79_vs)) return;
    double verts = dc ? (double)dc->vertexCount * (dc->instanceCount ? dc->instanceCount : 1) : dic ? (double)dic->indexCount * (dic->instanceCount ? dic->instanceCount : 1) : 0.0;
    uint64_t bh = 0; uint32_t cbid = g_bc.cb[0][3];
    if (cbid && cbid != BC_UNK) { sb87::cbwant.insert(cbid); auto it = sb87::cbs.find(cbid);
      if (it != sb87::cbs.end() && it->second.n) bh = b1eo::fnv64(it->second.b.data(), it->second.n) | 1ull; }
    if (!bh) f_nobones++; else bones.insert(bh);
    uint64_t k = mix(mix(mix(0, g_bc.vb[0]), g_bc.vbo[0]), bh);
    if (dic) k = mix(mix(mix(mix(mix(k, g_bc.ib), g_bc.ibo), dic->firstIndex), dic->indexCount), (uint64_t)(uint32_t)dic->vertexOffset);
    else if (dc) k = mix(mix(k, dc->firstVertex), dc->vertexCount);
    keys.emplace(k, verts);
    f_draws++; f_verts += verts;
    bool replay = b1vr::on && b1vr::pp_post_wanted(); if (replay) f_replay++;
    char nm[64]; View *rv = g_views.get(g_cs.rtv[0]); bool col = rv && rv->type == 2; uint32_t dw = 0, dh = 0;
    View *dv = g_views.get(g_cs.dsv); if (dv && dv->type == 3 && dv->iv != nullptr) { VkExtent3D e = dv->iv->image()->mipLevelExtent(0); dw = e.width; dh = e.height; }
    std::snprintf(nm, sizeof nm, "%s depth %ux%u", col ? "colour" : "depth-only", dw, dh);
    Cls &c = cls[nm]; c.draws += replay ? 2 : 1; c.verts += replay ? 2 * verts : verts; }
  static void frame() {
    if (!on()) return;
    n_tot++;
    if (n_tot > 300) {   /* after the warm-up, as the CHECKs */
      double uv = 0; for (auto &kv : keys) uv += kv.second;
      a_draws += f_draws; a_verts += f_verts; a_uverts += uv; a_ukeys += (double)keys.size(); a_bones += (double)bones.size(); a_replay += f_replay; a_nobones += f_nobones; n_fr++; }
    else cls.clear();
    keys.clear(); bones.clear(); f_draws = f_verts = f_replay = f_nobones = 0;
    if (n_fr && n_tot % 300 == 0) {
      double n = (double)n_fr;
      std::fprintf(stderr, "[b1skc] SKINCENSUS %ld frames (per frame): skinned draws %.1f (+ right-eye replays %.1f), vertices skinned %.0f, unique keys %.1f with %.0f vertices -> re-skinned share %.1f %% (draws without captured bones %.1f), distinct bone sets %.1f\n",
        n_fr, a_draws / n, a_replay / n, a_verts / n, a_ukeys / n, a_uverts / n, a_verts > 0 ? 100.0 * (1.0 - a_uverts / a_verts) : 0.0, a_nobones / n, a_bones / n);
      for (auto &kv : cls) std::fprintf(stderr, "[b1skc]   pass %s: draws %.1f, vertices %.0f per frame (replays counted twice)\n", kv.first.c_str(), kv.second.draws / n, kv.second.verts / n); } }
}
