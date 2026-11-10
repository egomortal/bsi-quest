/* rhi_b1_sunnl.h -- build 129: sun N.L PCSS skip, DXBC side (BSI_B1_SUNNL; runtime in rhi_b1_sunnl.inc).

   Proof (light pre-pass sun light fs.5dc7e57b, FNV key d0e72728ca566e90): o0.xyz = att * cb0[2] * sat(d), o1.xyz =
   exp(w * log(sat(dot(R, -V)))) * o0.xyz with d = dp3(cb0[1].xyz, normalize(2 * N - 1)), N = t0 (G-buffer normal),
   att = dp4(t1 sample, cb0[0]) (UNORM8 source: finite, >= 0), w = gloss^2 * 512 + 3 >= 3. For d <= 0: sat(d) = +0, so
   o0 = sign(cb0[2]) * 0 and o1 = (finite >= 0) * that = the same signed zero, WHATEVER att is. So the sun shadow value
   of a pixel whose d <= 0 never reaches the picture through that pixel's own light. It can still reach a NEIGHBOUR's
   light through the attenuation copy pass fs.c675b895 (sample_l, filter unknown) - the end-to-end CHECK covers that.

   Patch of the cascade projection twins (c29c9e4e S1 twin and its S1c twins): after the shader's first two
   instructions (div r0.xy, v0.xy, v0.w ; mad r0.xy, r0.xy, cbX[n].xy, cbX[n].wz = the screen UV of the scene depth),
   decode the G-buffer normal at that pixel (PS t125, ld at floor(uv * size), clamped) EXACTLY as the light does
   (mad x2-1, dp3, rsq, mul) and test d = dp3(L, n) <= -1e-3 with L = PS t124[0].xyz (the sun light's cb0[1] of the
   previous frame, uploaded by the runtime; t124[0].w != 0 = enabled). The margin 1e-3 covers the light's own filtered
   sample / UV ulps (b127: 0.0 % of the sun pixels have -1e-3 < d <= 0).
     variant V_SKIP:   discard_nz F ; retc_nz F            (pixel keeps the previous attenuation, its work is skipped)
     variant V_SKIP3:  the same, F = AND over the 3x3 neighbourhood of the pixel (safe for a filtering copy pass)
     variant V_MASK / V_MASK3 (CHECK, b132: used BY the real draw): if F: store u5[pixel] = 1 ; then the original code (same picture)
     b131: the mask variants also count into PS u6 (R32 uint buffer): [0] pixels tested, [1] test enabled, [2] would skip */
#pragma once
#include "rhi_b1_shadoweo.h"

namespace b1eo {
enum : uint32_t { OP_DISCARD = 13, OP_DP3 = 16, OP_IMAX = 36, OP_RSQ = 68, RET_UNORM = 1 };
enum SunVariant { V_SKIP = 1, V_SKIP3 = 3, V_MASK = 2, V_MASK3 = 4 };
struct SunResult { bool ok = false; std::string why; std::vector<uint8_t> blob; };

static inline SunResult patch_sunnl(const void *code, size_t len, int variant) {
  SunResult r_; const uint8_t *b = (const uint8_t *)code;
  std::vector<Chunk> ch; if (!read_container(b, len, ch)) { r_.why = "container"; return r_; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { r_.why = "no program"; return r_; }
  std::vector<uint32_t> w(ch[si].data.size() / 4); memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
  if ((w[0] >> 16) != 0 || ((w[0] >> 4) & 15) != 5) { r_.why = "not ps_5_x"; return r_; }
  std::vector<size_t> st; if (!find_insn(w, st)) { r_.why = "parse"; return r_; }
  auto op = [&] (size_t k) { return w[st[k]] & 0x7ffu; };
  int ntemps_at = -1, last_res = -1, first = -1; uint32_t ntemps = 0;
  for (size_t k = 0; k < st.size(); k++) {
    uint32_t o = op(k);
    if (o == OP_DCL_TEMPS) { ntemps_at = (int)k; ntemps = w[st[k] + 1]; }
    if (o == OP_DCL_RESOURCE) last_res = (int)k;
    bool dcl = (o >= 88 && o <= 106) || (o >= 143 && o <= 163) || o == OP_CUSTOMDATA;
    if (!dcl && first < 0) first = (int)k; }
  if (ntemps_at < 0 || last_res < 0 || first < 0 || (size_t)first + 2 >= st.size()) { r_.why = "no dcl_temps / dcl_resource / code"; return r_; }
  /* the first two instructions must be div r0.xy, v0.., v0.. and mad r0.xy, r0.., cb.., cb.. (dest token: temp, mask xy, index 0) */
  auto dest_is_r0xy = [&] (size_t k) { uint32_t t = w[st[k] + 1]; return ((t >> 12) & 0xffu) == RT_TEMP && ((t >> 4) & 15u) == 3u && w[st[k] + 2] == 0u; };
  if (op(first) != OP_DIV || !dest_is_r0xy(first)) { r_.why = "first instruction is not div r0.xy"; return r_; }
  if (op(first + 1) != OP_MAD || !dest_is_r0xy(first + 1)) { r_.why = "second instruction is not mad r0.xy"; return r_; }
  const uint32_t T = ntemps, A = T, B = T + 1, C = T + 2, D = T + 3, E = T + 4, F = T + 5;
  Code p;
  p.ins(OP_RESINFO, { r(A, "xy"), imm1u(0), res(125, "xyzw") }, 2u << 11);     /* resinfo_uint: normal texture size */
  p.ins(OP_UTOF, { r(B, "xy"), s(A, "xyxx") });
  p.ins(OP_MUL, { r(B, "xy"), s(0, "xyxx"), s(B, "xyxx") });                    /* uv * size */
  p.ins(OP_FTOI, { r(B, "xy"), s(B, "xyxx") });                                 /* pixel (truncation; uv >= 0) */
  p.ins(OP_IADD, { r(A, "xy"), s(A, "xyxx"), imm4u(0xffffffffu, 0xffffffffu, 0, 0) });   /* size - 1 */
  p.ins(OP_LD, { r(E, "xyzw"), imm4u(0, 0, 0, 0), res(124, "xyzw") });         /* L.xyz, enable flag w */
  p.ins(OP_NE, { r(F, "x"), s(E, "wwww"), imm1f(0.f) });
  const bool mk = variant == V_MASK || variant == V_MASK3;
  if (mk) {   /* b131 CHECK counters (PS u6, R32 uint): [0] pixels that reach the test, [1] of them with the test enabled */
    p.ins(OP_ATOMIC_IADD, { uavd(6, "xyzw"), imm1u(0), imm1u(1) });
    p.if_(s1(F, 'x'), 1); p.ins(OP_ATOMIC_IADD, { uavd(6, "xyzw"), imm1u(1), imm1u(1) }); p.endif_(); }
  const bool nb = variant == V_SKIP3 || variant == V_MASK3;
  for (int dy = nb ? -1 : 0; dy <= (nb ? 1 : 0); dy++) for (int dx = nb ? -1 : 0; dx <= (nb ? 1 : 0); dx++) {
    p.ins(OP_IADD, { r(C, "xy"), s(B, "xyxx"), imm4u((uint32_t)dx, (uint32_t)dy, 0, 0) });
    p.ins(OP_IMAX, { r(C, "xy"), s(C, "xyxx"), imm4u(0, 0, 0, 0) });
    p.ins(OP_IMIN, { r(C, "xy"), s(C, "xyxx"), s(A, "xyxx") });
    p.ins(OP_MOV, { r(C, "zw"), imm4u(0, 0, 0, 0) });
    p.ins(OP_LD, { r(D, "xyzw"), s(C, "xyzw"), res(125, "xyzw") });
    p.ins(OP_MAD, { r(D, "xyz"), s(D, "xyzx"), imm4f(2.f, 2.f, 2.f, 0.f), imm4f(-1.f, -1.f, -1.f, 0.f) });   /* as the light: mad x2-1 */
    p.ins(OP_DP3, { r(D, "w"), s(D, "xyzx"), s(D, "xyzx") });
    p.ins(OP_RSQ, { r(D, "w"), s(D, "wwww") });
    p.ins(OP_MUL, { r(D, "xyz"), s(D, "wwww"), s(D, "xyzx") });
    p.ins(OP_DP3, { r(D, "w"), s(E, "xyzx"), s(D, "xyzx") });                   /* d (NaN -> ge false -> no skip) */
    p.ins(OP_GE, { r(D, "w"), imm1f(-1e-3f), s(D, "wwww") });
    p.ins(OP_AND, { r(F, "x"), s(F, "xxxx"), s(D, "wwww") }); }
  if (variant == V_SKIP || variant == V_SKIP3) {
    p.ins(OP_DISCARD, { s1(F, 'x') }, 1u << 18);
    p.retc(s1(F, 'x'), 1);
  } else {   /* CHECK mask (b132: inline in the REAL draw): u5[pixel] = 1 where the skip variant would skip, then the original code runs unchanged */
    p.if_(s1(F, 'x'), 1);
    p.ins(OP_STORE_UAV_TYPED, { uavd(5, "xyzw"), s(B, "xyyy"), imm4f(1.f, 1.f, 1.f, 1.f) });
    p.ins(OP_ATOMIC_IADD, { uavd(6, "xyzw"), imm1u(2), imm1u(1) });   /* [2] pixels the skip variant would skip */
    p.endif_();
  }
  Code dcl; dcl.dcl_tex2d(125, RET_FLOAT); dcl.dcl_buf(124, RET_FLOAT); if (mk) { dcl.dcl_uav_tex2d(5, RET_UNORM); dcl.dcl_uav_buf(6, RET_UINT); }
  size_t endRes = st[last_res] + ins_len(&w[st[last_res]], w.size() - st[last_res]);
  size_t at = st[first + 2];
  std::vector<uint32_t> o; o.reserve(w.size() + p.w.size() + dcl.w.size());
  for (size_t i = 0; i < w.size(); i++) {
    if (i == endRes) o.insert(o.end(), dcl.w.begin(), dcl.w.end());
    if (i == at) o.insert(o.end(), p.w.begin(), p.w.end());
    if (i == st[ntemps_at] + 1) { o.push_back(T + 6); continue; }
    o.push_back(w[i]); }
  o[1] = (uint32_t)o.size();
  ch[si].data.resize(o.size() * 4); memcpy(ch[si].data.data(), o.data(), o.size() * 4);
  std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(c);
  r_.blob = write_container(keep); r_.ok = true; return r_;
}

/* b136 CHECK compare split by the skip mask (CS, 8x8): u61[1] += texels of t127 / t125 that differ where the mask t123 is 0,
   u61[2] += where it is set (cb13[0].xy = width - 1, height - 1; cb13[0].z != 0: mask has the same size, else all go to [1]) */
static inline std::vector<uint8_t> build_cs_compare_masked() {
  Code c;
  c.dcl_cb(13, 1);
  c.dcl_tex2d(127, RET_FLOAT);
  c.dcl_tex2d(125, RET_FLOAT);
  c.dcl_tex2d(123, RET_FLOAT);
  c.dcl_uav_buf(61, RET_UINT);
  c.dcl_tid("xy");
  c.dcl_temps(5);
  c.dcl_tg(8, 8, 1);
  c.ins(OP_MOV, { r(0, "xy"), tid("xyxx") });
  c.ins(OP_ILT, { r(4, "x"), cb(13, 0, "xxxx"), s(0, "xxxx") });
  c.ins(OP_ILT, { r(4, "y"), cb(13, 0, "yyyy"), s(0, "yyyy") });
  c.ins(OP_OR, { r(0, "z"), s(4, "xxxx"), s(4, "yyyy") });
  c.retc(s1(0, 'z'), 1);
  c.ins(OP_MOV, { r(0, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_LD, { r(1, "xyzw"), s(0, "xyzw"), res(127, "xyzw") });
  c.ins(OP_LD, { r(2, "xyzw"), s(0, "xyzw"), res(125, "xyzw") });
  c.ins(OP_INE, { r(3, "xyzw"), s(1, "xyzw"), s(2, "xyzw") });              /* bit patterns */
  c.ins(OP_OR, { r(3, "xy"), s(3, "xyxx"), s(3, "zwzz") });
  c.ins(OP_OR, { r(3, "x"), s(3, "xxxx"), s(3, "yyyy") });
  c.retc(s1(3, 'x'), 0);
  c.ins(OP_LD, { r(4, "xyzw"), s(0, "xyzw"), res(123, "xyzw") });
  c.ins(OP_NE, { r(4, "x"), s(4, "xxxx"), imm1f(0.f) });
  c.ins(OP_AND, { r(4, "x"), s(4, "xxxx"), cb(13, 0, "zzzz") });
  c.ins(OP_MOVC, { r(4, "y"), s(4, "xxxx"), imm1u(2), imm1u(1) });
  c.ins(OP_ATOMIC_IADD, { uavd(61, "xyzw"), s(4, "yyyy"), imm1u(1) });
  c.ret();
  return cs_container(c);
}

/* CHECK merge (CS, 8x8): alt (u60, RGBA8 UNORM storage) = real attenuation (t127), with ALL channels of every texel whose
   mask (t125) is non-zero replaced by 0.5 (garbage: any value must give the same sun-light bytes; the sun light reads only
   its own channel); u61[2] += masked texels */
static inline std::vector<uint8_t> build_cs_sunnl_merge(bool all = false) {   /* all (b131 positive control): every texel counts as masked */
  Code c;   /* b135: only the channels in cb13[0] (uint ~0 = the cascade projection writes that channel) get the garbage */
  c.dcl_cb(13, 1);
  c.dcl_tex2d(127, RET_FLOAT);
  c.dcl_tex2d(125, RET_FLOAT);
  c.dcl_uav_tex2d(60, RET_UNORM);
  c.dcl_uav_buf(61, RET_UINT);
  c.dcl_tid("xy");
  c.dcl_temps(5);
  c.dcl_tg(8, 8, 1);
  c.ins(OP_RESINFO, { r(0, "zw"), imm1u(0), res(127, "xxxy") }, 2u << 11);
  c.ins(OP_ULT, { r(1, "xy"), tid("xyxx"), s(0, "zwzz") });
  c.ins(OP_AND, { r(1, "x"), s(1, "xxxx"), s(1, "yyyy") });
  c.retc(s1(1, 'x'), 0);
  c.ins(OP_MOV, { r(0, "xy"), tid("xyxx") });
  c.ins(OP_MOV, { r(0, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_LD, { r(1, "xyzw"), s(0, "xyzw"), res(127, "xyzw") });
  c.ins(OP_LD, { r(2, "xyzw"), s(0, "xyzw"), res(125, "xyzw") });
  if (all) c.ins(OP_MOV, { r(3, "x"), imm1u(0xffffffffu) }); else c.ins(OP_NE, { r(3, "x"), s(2, "xxxx"), imm1f(0.f) });
  c.ins(OP_AND, { r(4, "xyzw"), s(3, "xxxx"), cb(13, 0, "xyzw") });
  c.ins(OP_MOVC, { r(1, "xyzw"), s(4, "xyzw"), imm4f(0.5f, 0.5f, 0.5f, 0.5f), s(1, "xyzw") });
  c.ins(OP_STORE_UAV_TYPED, { uavd(60, "xyzw"), s(0, "xyyy"), s(1, "xyzw") });
  c.if_(s1(3, 'x'), 1);
  c.ins(OP_ATOMIC_IADD, { uavd(61, "xyzw"), imm1u(2), imm1u(1) });
  c.endif_();
  c.ret();
  return cs_container(c);
}
/* b137 BSI_B1_SUNNL=5: IN-SHADER exactness check of the skip (S1c pattern, no redraw). Twin of the sun light PS
   fs.5dc7e57b (key d0e72728ca566e90) used BY the real draw: the original code runs unchanged (same picture); two movs save
   the attenuation sample (S) and sat(d) (G.x); before the ret, at pixels where the cascade's skip test F holds (computed
   exactly as patch_sunnl does: t125 G-buffer normal at the pixel, t124[0] = previous-frame L + enable flag, margin 1e-3),
   the light result is recomputed with the attenuation's R channel replaced by the garbage values t124[1].xyz (0, 1, 0.5:
   runtime values, so the compiler cannot fold them) through the same instruction sequence, and o0.xyz / o1.xyz are compared
   bit for bit with the real results. ctl_all (BSI_B1_SUNNL_CTL=1, positive control): F = true everywhere (must mismatch).
   PS u6 (R32 uint) counters: [0] light pixels, [1] pixels with F, [2] F pixels whose output DEPENDS on the attenuation
   (must be 0), [3] pixels whose t1 sample is not bit-equal to t1's own texel (ld; must be 0 for "no neighbour reaches a
   pixel"), [4] pixels whose v0 * size(t1) is not at a texel centre (frac != 0.5), [5] F pixels with the light's own
   sat(d) > 0 (previous-frame L vs this frame's; diagnostic). */
enum : uint32_t { OP_DP4_ = 17, OP_SAMPLE_ = 69, OP_LT_ = 49, OP_RNI_ = 65, SAT_BIT = 1u << 13 };
static inline SunResult patch_sunlight_check(const void *code, size_t len, bool ctl_all) {
  SunResult r_; const uint8_t *b = (const uint8_t *)code;
  std::vector<Chunk> ch; if (!read_container(b, len, ch)) { r_.why = "container"; return r_; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { r_.why = "no program"; return r_; }
  std::vector<uint32_t> w(ch[si].data.size() / 4); memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
  if ((w[0] >> 16) != 0 || ((w[0] >> 4) & 15) != 5) { r_.why = "not ps_5_x"; return r_; }
  std::vector<size_t> st; if (!find_insn(w, st)) { r_.why = "parse"; return r_; }
  auto op = [&] (size_t k) { return w[st[k]] & 0x7ffu; };
  int ntemps_at = -1, last_res = -1, first = -1; uint32_t ntemps = 0;
  for (size_t k = 0; k < st.size(); k++) {
    uint32_t o = op(k);
    if (o == OP_DCL_TEMPS) { ntemps_at = (int)k; ntemps = w[st[k] + 1]; }
    if (o == OP_DCL_RESOURCE) last_res = (int)k;
    bool dcl = (o >= 88 && o <= 106) || (o >= 143 && o <= 163) || o == OP_CUSTOMDATA;
    if (!dcl && first < 0) first = (int)k; }
  if (ntemps_at < 0 || last_res < 0 || first < 0 || (size_t)first + 12 >= st.size()) { r_.why = "no dcl_temps / dcl_resource / code"; return r_; }
  /* the expected shape (j0064 dump): 0 sample r0 <- t1 ; 1 dp4 r0.x ; ... 11 mov_sat r2.x, r1.w ; ... last = ret */
  auto dtemp = [&] (size_t k, uint32_t reg) { size_t q = st[k]; while (w[q] >> 31) q++; uint32_t t = w[q + 1]; return ((t >> 12) & 0xffu) == RT_TEMP && w[q + 2] == reg; };   /* skips extended opcode tokens (fxc's sample has them) */
  if (op(first) != OP_SAMPLE_ || !dtemp(first, 0)) { r_.why = "instruction 0 is not sample r0"; return r_; }
  if (op(first + 1) != OP_DP4_) { r_.why = "instruction 1 is not dp4"; return r_; }
  if (op(first + 11) != OP_MOV || !(w[st[first + 11]] & SAT_BIT) || !dtemp(first + 11, 2)) { r_.why = "instruction 11 is not mov_sat r2"; return r_; }
  if (op(st.size() - 1) != OP_RET) { r_.why = "last instruction is not ret"; return r_; }
  for (size_t k = first; k + 1 < st.size(); k++) { uint32_t o = op(k); if (o == OP_RET || o == OP_RETC || o == OP_DISCARD || o == OP_IF || o == OP_LOOP) { r_.why = "control flow inside"; return r_; } }
  const uint32_t T = ntemps, A = T, B = T + 1, C = T + 2, D = T + 3, E = T + 4, F = T + 5, S = T + 6, G = T + 7, H = T + 8, K = T + 9, M = T + 10;
  auto cnt = [&] (Code &c, uint32_t i) { c.ins(OP_ATOMIC_IADD, { uavd(6, "xyzw"), imm1u(i), imm1u(1) }); };
  Code p0; p0.ins(OP_MOV, { r(S, "xyzw"), s(0, "xyzw") });                    /* after the attenuation sample */
  Code p1; p1.ins(OP_MOV, { r(G, "x"), s(2, "xxxx") });                       /* after mov_sat r2.x (= sat(d)) */
  Code p; const Opd uv = src(RT_INPUT, 0, "xyxx");
  cnt(p, 0);
  p.ins(OP_RESINFO, { r(A, "xy"), imm1u(0), res(125, "xyzw") }, 2u << 11);
  p.ins(OP_UTOF, { r(B, "xy"), s(A, "xyxx") });
  p.ins(OP_MUL, { r(B, "xy"), uv, s(B, "xyxx") });
  p.ins(OP_FTOI, { r(B, "xy"), s(B, "xyxx") });
  p.ins(OP_IADD, { r(A, "xy"), s(A, "xyxx"), imm4u(0xffffffffu, 0xffffffffu, 0, 0) });
  p.ins(OP_LD, { r(E, "xyzw"), imm4u(0, 0, 0, 0), res(124, "xyzw") });
  p.ins(OP_NE, { r(F, "x"), s(E, "wwww"), imm1f(0.f) });
  p.ins(OP_IADD, { r(C, "xy"), s(B, "xyxx"), imm4u(0, 0, 0, 0) });            /* the cascade test, centre pixel, verbatim */
  p.ins(OP_IMAX, { r(C, "xy"), s(C, "xyxx"), imm4u(0, 0, 0, 0) });
  p.ins(OP_IMIN, { r(C, "xy"), s(C, "xyxx"), s(A, "xyxx") });
  p.ins(OP_MOV, { r(C, "zw"), imm4u(0, 0, 0, 0) });
  p.ins(OP_LD, { r(D, "xyzw"), s(C, "xyzw"), res(125, "xyzw") });
  p.ins(OP_MAD, { r(D, "xyz"), s(D, "xyzx"), imm4f(2.f, 2.f, 2.f, 0.f), imm4f(-1.f, -1.f, -1.f, 0.f) });
  p.ins(OP_DP3, { r(D, "w"), s(D, "xyzx"), s(D, "xyzx") });
  p.ins(OP_RSQ, { r(D, "w"), s(D, "wwww") });
  p.ins(OP_MUL, { r(D, "xyz"), s(D, "wwww"), s(D, "xyzx") });
  p.ins(OP_DP3, { r(D, "w"), s(E, "xyzx"), s(D, "xyzx") });
  p.ins(OP_GE, { r(D, "w"), imm1f(-1e-3f), s(D, "wwww") });
  p.ins(OP_AND, { r(F, "x"), s(F, "xxxx"), s(D, "wwww") });
  if (ctl_all) p.ins(OP_MOV, { r(F, "x"), imm1u(0xffffffffu) });
  p.if_(s1(F, 'x'), 1);
  cnt(p, 1);
  p.ins(OP_LD, { r(H, "xyzw"), imm4u(1, 0, 0, 0), res(124, "xyzw") });       /* garbage values (runtime) */
  p.ins(OP_MUL, { r(K, "xyz"), s(0, "wwww"), s(0, "xyzx") });                /* the real o1.xyz (as the shader computes it) */
  p.ins(OP_MOV, { r(M, "x"), imm1u(0) });
  for (int g = 0; g < 3; g++) {
    static const char *gs[3] = { "xxxx", "yyyy", "zzzz" };
    p.ins(OP_MOV, { r(D, "xyzw"), s(S, "xyzw") });
    p.ins(OP_MOV, { r(D, "x"), s(H, gs[g]) });
    p.ins(OP_DP4_, { r(D, "x"), s(D, "xyzw"), cb(0, 0, "xyzw") });           /* = the shader's 3 instructions with att' */
    p.ins(OP_MUL, { r(D, "xyz"), s(D, "xxxx"), cb(0, 2, "xyzx") });
    p.ins(OP_MUL, { r(D, "xyz"), s(D, "xyzx"), s(G, "xxxx") });
    p.ins(OP_MUL, { r(C, "xyz"), s(0, "wwww"), s(D, "xyzx") });
    p.ins(OP_INE, { r(D, "xyz"), s(D, "xyzx"), s(0, "xyzx") });
    p.ins(OP_INE, { r(C, "xyz"), s(C, "xyzx"), s(K, "xyzx") });
    p.ins(OP_OR, { r(D, "xyz"), s(D, "xyzx"), s(C, "xyzx") });
    p.ins(OP_OR, { r(D, "x"), s(D, "xxxx"), s(D, "yyyy") });
    p.ins(OP_OR, { r(D, "x"), s(D, "xxxx"), s(D, "zzzz") });
    p.ins(OP_OR, { r(M, "x"), s(M, "xxxx"), s(D, "xxxx") }); }
  p.if_(s1(M, 'x'), 1); cnt(p, 2); p.endif_();
  p.ins(OP_LT_, { r(D, "x"), imm1f(0.f), s(G, "xxxx") });
  p.if_(s1(D, 'x'), 1); cnt(p, 5); p.endif_();
  p.endif_();
  /* does the attenuation sample read only the pixel's own texel? */
  p.ins(OP_RESINFO, { r(A, "xy"), imm1u(0), res(1, "xyzw") }, 2u << 11);
  p.ins(OP_UTOF, { r(C, "xy"), s(A, "xyxx") });
  p.ins(OP_MUL, { r(C, "xy"), uv, s(C, "xyxx") });
  p.ins(OP_RNI_, { r(D, "xy"), s(C, "xyxx") });
  p.ins(OP_ADD, { r(D, "xy"), s(C, "xyxx"), s(D, "xyxx", true) });
  p.ins(OP_NE, { r(D, "xy"), s(D, "xyxx"), imm4f(0.5f, 0.5f, 0.f, 0.f) });
  p.ins(OP_OR, { r(D, "x"), s(D, "xxxx"), s(D, "yyyy") });
  p.if_(s1(D, 'x'), 1); cnt(p, 4); p.endif_();
  p.ins(OP_FTOI, { r(C, "xy"), s(C, "xyxx") });
  p.ins(OP_MOV, { r(C, "zw"), imm4u(0, 0, 0, 0) });
  p.ins(OP_LD, { r(D, "xyzw"), s(C, "xyzw"), res(1, "xyzw") });
  p.ins(OP_INE, { r(D, "xyzw"), s(D, "xyzw"), s(S, "xyzw") });
  p.ins(OP_OR, { r(D, "xy"), s(D, "xyxx"), s(D, "zwzz") });
  p.ins(OP_OR, { r(D, "x"), s(D, "xxxx"), s(D, "yyyy") });
  p.if_(s1(D, 'x'), 1); cnt(p, 3); p.endif_();
  Code dcl; dcl.dcl_tex2d(125, RET_FLOAT); dcl.dcl_buf(124, RET_FLOAT); dcl.dcl_uav_buf(6, RET_UINT);
  size_t endRes = st[last_res] + ins_len(&w[st[last_res]], w.size() - st[last_res]);
  size_t at0 = st[first + 1], at1 = st[first + 12], atr = st[st.size() - 1];
  std::vector<uint32_t> o; o.reserve(w.size() + p.w.size() + dcl.w.size() + 16);
  for (size_t i = 0; i < w.size(); i++) {
    if (i == endRes) o.insert(o.end(), dcl.w.begin(), dcl.w.end());
    if (i == at0) o.insert(o.end(), p0.w.begin(), p0.w.end());
    if (i == at1) o.insert(o.end(), p1.w.begin(), p1.w.end());
    if (i == atr) o.insert(o.end(), p.w.begin(), p.w.end());
    if (i == st[ntemps_at] + 1) { o.push_back(T + 11); continue; }
    o.push_back(w[i]); }
  o[1] = (uint32_t)o.size();
  ch[si].data.resize(o.size() * 4); memcpy(ch[si].data.data(), o.data(), o.size() * 4);
  std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(c);
  r_.blob = write_container(keep); r_.ok = true; return r_;
}
}  // namespace b1eo

