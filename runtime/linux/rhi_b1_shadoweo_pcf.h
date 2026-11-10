/* rhi_b1_shadoweo_pcf.h -- S1c "PCF from the search bits" (Fable note 11): a second DXBC pass over the S1 twin of the
   three contact-hardening shadow-projection pixel shaders. Header-only, no DXVK dependency; include after rhi_b1_shadoweo.h.

   What it does (see note 11 for the proof and the measurements behind it)
   - The game's PCF reads exactly the texels the blocker search already read: all 64 `gather4_c` taps sit on the SAME
     base coordinate as the search (r1.yz), with immediate texel offsets (-7..7 step 2)^2 = the search's 16x16 window,
     and compare against the SAME reference (r1.w; the search compares against r3.w = r1.w + r2.w with r2.w = 0). With a
     LESS_EQUAL comparison sampler a tap component is 1.0 exactly when the search's `ge texel, ref` was true. So:
       1. the blocker-search loop is unrolled (8 copies of its body, same instructions, same values; only `loop`,
          `ilt`/`breakc_nz` and `endloop` go), and after each of the 8 `ge` per copy the 4 compare bits are packed
          (bit 4g + c of word k, k = row pair 0..7, g = gather 0..7, c = component) into two new temps;
       2. every `gather4_c` of the PCF becomes `ubfe` + `utof` of those bits (0.0 / 1.0 exactly), so the PCF's 64
          texture fetches disappear while every arithmetic instruction of the shader stays as it was.
     Bit-exact by construction when (checked on the CPU before the twin is bound, see the .inc patch): s1 compares with
     LESS_OR_EQUAL (or LESS, see `strict`), s0 and s1 have the same address modes, and the atlas is a power-of-two size,
     AND, per pixel, the search's float-offset coordinates `r1.y + ox/size` (ox = -7..7) are exact in fp32 -- true when
     r1.y - 7/size, r1.y and r1.y + 7/size share the sign and exponent (same for v): then every sum is a multiple of the
     binade's ulp, so `floor(sum*size - 0.5)` is `floor(r1.y*size - 0.5) + ox` exactly, the texels the taps' integer
     offsets select. Pixels whose window straddles a power-of-two coordinate (about 8 % of a 2048^2 atlas: 7 texels
     below 1/2, 1/4, ... and the first 7) fail that test and keep their real gather4_c (per-tap branch on one flag).
   - Mode 2 (check): the taps keep their gather4_c and the shader counts, per tap, components that differ from the packed
     bits (u7[30] += mismatching taps, u7[31] += taps checked). Must stay 0 on the device before mode 1 is trusted.
   Register use: 4 new temps (scratch, bits[0..3], bits[4..7], flag/counters; check mode: +1 for the recorded coordinates);
   the PCF's own registers and arithmetic are untouched.

   s1c71 review (main lane, build 71) -- changes against Fable's version:
   - The comparison sampler is s2 in all three game shaders (dcl_sampler (comparison) s2; RenderDoc lists it as the second
     bound sampler, which is where "s1" came from). The patcher now takes the sampler register from the taps (all 64 must use
     the same one) and returns it (PcfResult::cmp_sampler); the run-time check reads that slot. The search gathers must use
     t1 with s0 (the sampler S1 already requires to wrap).
   - The per-pixel exactness flag also requires (all cheap, once per PCF pixel):
       * dims = exactly 1/size: dz and dw have a zero mantissa (powers of two) and dz * width == 1, dw * height == 1
         (resinfo of t1). Without it a cb0 dims value that is not the texture's own texel size (the search then steps by
         dims, the taps by whole texels) gives different texels; the offline A/B's "dims 1.4x / 0.5x" configs show it;
       * |u|, |v| small enough that every u + k*dz is exact AND u*size - 0.5 stays exact: exponent(u) <= exponent(dz) + 20
         (|u| < 2^(21-s) for size 2^s; 1024 atlas widths for 2048^2). Fable's flag only required u+-7dz, u in one binade,
         which is not enough for large coordinates (ulp(u) > dz);
       * the reference is bit-identical on both paths: r1.w + 0.0 == r1.w as bits (the search compares with r3.w = r1.w + 0).
   - All taps must be on one straight-line path (no control flow between the first and the last tap), so the flag computed
     before the first tap covers every tap and every PCF pixel runs all 64.
   - Mode 2 counts per pixel (one atomic per counter instead of two per tap): u7[28] safe pixels (each compares 64 taps),
     u7[29] unsafe pixels, u7[30] mismatching taps, u7[31] pixels with a mismatching tap; the first mismatching pixel stores
     u, v, ref and its mismatch count in u7[33..36] (u7[32] = how many tried).
   - `broken` (negative control only): 1 = the centre tap (-1,-1) gets inverted bits, 2 = x/y components swapped in every
     tap. */
#pragma once
#include <algorithm>
#include "rhi_b1_shadoweo.h"

namespace b1eo {

enum : uint32_t { OP_GATHER4_C = 126, OP_UBFE = 138, OP_IBFE = 137, OP_XOR = 87, OP_SAMPLE_CONTROLS_MASK = 0x3f };
enum PcfMode { P_OFF = 0, P_BITS = 1, P_CHECK = 2 };

struct PcfResult { bool ok = false; std::string why; std::vector<uint8_t> blob; uint32_t taps = 0, unrolled = 0; uint32_t cmp_sampler = ~0u; };   /* s1c71: cmp_sampler = the taps' sampler register */

/* operand decoding helpers (D3D10 tokenized format): operand token at p; returns its dword length (0 = unsupported) */
static inline uint32_t opd_len(const uint32_t *p, size_t n) {
  if (!n) return 0; uint32_t t = p[0], len = 1, i = 0;
  while (p[i] & 0x80000000u) { i++; if (i >= n) return 0; len++; }   /* extended operand tokens (modifiers) */
  uint32_t ncomp = t & 3u, dim = (t >> 20) & 3u;
  if (ncomp == 0 && ((t >> 12) & 0xffu) == RT_IMM32) return 0;       /* not produced by fxc */
  if (((t >> 12) & 0xffu) == RT_IMM32) return len + (ncomp == 1 ? 1u : 4u);
  for (uint32_t d = 0; d < dim; d++) {
    uint32_t rep = (t >> (22 + 3 * d)) & 7u;
    if (rep == 0) len += 1;                /* imm32 */
    else if (rep == 1) len += 2;           /* imm64 */
    else return 0;                         /* relative indexing: not expected in these shaders */
  }
  return len;
}
static inline uint32_t opd_type(uint32_t t) { return (t >> 12) & 0xffu; }
static inline uint32_t opd_reg(const uint32_t *p) { uint32_t i = 0; while (p[i] & 0x80000000u) i++; return p[i + 1]; }   /* first index (register number) */
static inline uint32_t opd_sel(uint32_t t) { return (t >> 2) & 3u; }     /* 0 mask, 1 swizzle, 2 select1 */
static inline uint32_t opd_mask(uint32_t t) { return (t >> 4) & 0xfu; }
static inline uint32_t opd_swz(uint32_t t) { return (t >> 4) & 0xffu; }

/* the operands of the instruction at w[a] (length l): start offsets within the instruction */
static inline bool opd_starts(const std::vector<uint32_t> &w, size_t a, uint32_t l, std::vector<size_t> &starts) {
  size_t i = a + 1; uint32_t t = w[a];
  while (t & 0x80000000u) { t = w[i]; i++; }            /* extended opcode tokens */
  while (i < a + l) { uint32_t ol = opd_len(&w[i], a + l - i); if (!ol) return false; starts.push_back(i); i += ol; }
  return true;
}
/* aoffimmi offsets of an extended-opcode instruction (sample controls token), sign-extended 4-bit */
static inline bool opd_offsets(const std::vector<uint32_t> &w, size_t a, int &ox, int &oy) {
  uint32_t t = w[a]; size_t i = a + 1; bool got = false;
  while (t & 0x80000000u) { uint32_t e = w[i]; i++;
    if ((e & 0x3fu) == 1u) { int u = (int)((e >> 9) & 0xfu), v = (int)((e >> 13) & 0xfu); if (u >= 8) u -= 16; if (v >= 8) v -= 16; ox = u; oy = v; got = true; }
    t = e; }
  return got;
}

/* strict: the comparison sampler at s1 uses LESS (ref < texel) instead of LESS_OR_EQUAL; the bits are then taken from an
   extra `lt` on the same operands (they differ from the search's `ge` only when a texel equals the reference exactly) */
static inline PcfResult patch_ps_pcf(const void *code, size_t len, int pmode, bool strict = false, int broken = 0) {
  PcfResult pr; const uint8_t *b = (const uint8_t *)code;
  if (pmode != P_BITS && pmode != P_CHECK) { pr.why = "mode"; return pr; }
  std::vector<Chunk> ch; if (!read_container(b, len, ch)) { pr.why = "container"; return pr; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { pr.why = "no program"; return pr; }
  std::vector<uint32_t> w(ch[si].data.size() / 4); memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
  if ((w[0] >> 16) != 0 || ((w[0] >> 4) & 15) != 5) { pr.why = "not ps_5_x"; return pr; }
  std::vector<size_t> st; if (!find_insn(w, st)) { pr.why = "parse"; return pr; }
  auto op = [&] (size_t k) { return w[st[k]] & 0x7ffu; };
  auto ilen = [&] (size_t k) { return ins_len(&w[st[k]], w.size() - st[k]); };
  /* the 8-gather search loop (the S1 patch keeps it as a loop) */
  size_t L = 0, E = 0; int ntemps_at = -1, last_res = -1; uint32_t ntemps = 0; bool have_u7 = false;
  for (size_t k = 0; k < st.size(); k++) {
    if (op(k) == OP_DCL_TEMPS) { ntemps_at = (int)k; ntemps = w[st[k] + 1]; }
    if (op(k) == OP_DCL_RESOURCE) last_res = (int)k;
    if (op(k) == OP_DCL_UAV_TYPED && w[st[k] + 2] == 7u) have_u7 = true;
    if (op(k) == OP_LOOP && !L) {
      size_t depth = 0, e = 0, ng = 0;
      for (size_t j = k; j < st.size(); j++) { if (op(j) == OP_LOOP) depth++; if (op(j) == OP_ENDLOOP && !--depth) { e = j; break; } if (op(j) == OP_GATHER4) ng++; }
      if (e && ng == 8) { L = k; E = e; }
    }
  }
  if (!L || ntemps_at < 0 || last_res < 0) { pr.why = "no 8-gather loop / dcl_temps / dcl_resource"; return pr; }
  /* body: ilt, breakc_nz, ... , iadd r1.x, 2 */
  if (op(L + 1) != OP_ILT || op(L + 2) != OP_BREAKC) { pr.why = "loop head differs"; return pr; }
  if (op(E - 1) != OP_IADD) { pr.why = "loop tail differs"; return pr; }
  for (size_t k = L + 3; k < E; k++) if (op(k) == OP_GATHER4) {   /* s1c71: the search reads t1 through s0 (S1 requires s0 to wrap) */
    std::vector<size_t> o; if (!opd_starts(w, st[k], ilen(k), o) || o.size() != 4) { pr.why = "search gather operands"; return pr; }
    if (opd_type(w[o[2]]) != RT_RESOURCE || opd_reg(&w[o[2]]) != 1 || opd_type(w[o[3]]) != RT_SAMPLER || opd_reg(&w[o[3]]) != 0) { pr.why = "search gather is not t1/s0"; return pr; }
  }
  /* the 8 `ge rX.xyzw, rY.xyzw, r3.wwww` of the body, each followed by `movc rX` */
  std::vector<size_t> ges;
  for (size_t k = L + 3; k < E; k++) if (op(k) == OP_GE) {
    std::vector<size_t> o; if (!opd_starts(w, st[k], ilen(k), o) || o.size() != 3) { pr.why = "ge operands"; return pr; }
    uint32_t d = w[o[0]], s2 = w[o[2]];
    if (opd_type(d) != RT_TEMP || opd_sel(d) != 0 || opd_mask(d) != 0xfu) { pr.why = "ge dst not a full temp"; return pr; }
    if (opd_type(s2) != RT_TEMP || opd_reg(&w[o[2]]) != 3 || opd_swz(s2) != swz_of("wwww")) { pr.why = "ge reference is not r3.w"; return pr; }
    if (k + 1 >= E || op(k + 1) != OP_MOVC) { pr.why = "ge not followed by movc"; return pr; }
    ges.push_back(k);
  }
  if (ges.size() != 8) { char t[64]; snprintf(t, sizeof t, "%zu ge in the loop (expected 8)", ges.size()); pr.why = t; return pr; }
  /* the PCF taps: 64 gather4_c, coordinate r1.yz.., resource t1, reference r1.w, offsets (-7..7 step 2)^2 each once */
  struct Tap { size_t k; int ox, oy; uint32_t dreg, dmask; };
  std::vector<Tap> taps; bool seen[8][8] = { };
  for (size_t k = E + 1; k < st.size(); k++) if (op(k) == OP_GATHER4_C) {
    std::vector<size_t> o; if (!opd_starts(w, st[k], ilen(k), o) || o.size() != 5) { pr.why = "gather4_c operands"; return pr; }
    int ox = 0, oy = 0; if (!opd_offsets(w, st[k], ox, oy)) { pr.why = "gather4_c without offsets"; return pr; }
    uint32_t d = w[o[0]], c = w[o[1]], r = w[o[2]], sm = w[o[3]], ref = w[o[4]];
    if (opd_type(sm) != RT_SAMPLER) { pr.why = "tap sampler operand"; return pr; }   /* s1c71: one comparison sampler for all taps */
    if (pr.cmp_sampler == ~0u) pr.cmp_sampler = opd_reg(&w[o[3]]); else if (pr.cmp_sampler != opd_reg(&w[o[3]])) { pr.why = "taps use different samplers"; return pr; }
    if (opd_type(d) != RT_TEMP || opd_sel(d) != 0 || opd_mask(d) != 0xfu) { pr.why = "tap dst not a full temp"; return pr; }
    if (opd_type(c) != RT_TEMP || opd_reg(&w[o[1]]) != 1 || (opd_swz(c) & 0xfu) != (swz_of("yzyy") & 0xfu)) { pr.why = "tap coordinate is not r1.yz"; return pr; }
    if (opd_type(r) != RT_RESOURCE || opd_reg(&w[o[2]]) != 1) { pr.why = "tap resource is not t1"; return pr; }
    if (opd_type(ref) != RT_TEMP || opd_reg(&w[o[4]]) != 1 || opd_sel(ref) != 2 || ((ref >> 4) & 3u) != 3u) { pr.why = "tap reference is not r1.w"; return pr; }
    if (ox < -7 || ox > 7 || oy < -7 || oy > 7 || ((ox + 7) & 1) || ((oy + 7) & 1)) { pr.why = "tap offset outside the window"; return pr; }
    int g = (ox + 7) / 2, kk = (oy + 7) / 2;
    if (seen[kk][g]) { pr.why = "tap offset repeated"; return pr; }
    seen[kk][g] = true;
    taps.push_back({ k, ox, oy, opd_reg(&w[o[0]]), opd_mask(d) });
  }
  if (taps.size() != 64) { char t[64]; snprintf(t, sizeof t, "%zu gather4_c taps (expected 64)", taps.size()); pr.why = t; return pr; }
  if (pr.cmp_sampler > 15u) { pr.why = "tap sampler register out of range"; return pr; }
  /* s1c71: the search's base coordinate and reference (r1.yzw) must reach the taps unchanged: no write to r1.y/z/w inside
     the loop or between the loop and the last tap (any mask-mode temp operand of r1 with y, z or w counts as a write; the
     last tap itself may write r1, as it does in d108/59c8: its sources are read first) */
  for (size_t k = L; k <= taps.back().k; k++) {
    std::vector<size_t> o; if (!opd_starts(w, st[k], ilen(k), o)) { pr.why = "operands between the search and the taps"; return pr; }
    if (k == taps.back().k) o.erase(o.begin());
    for (size_t q : o) { uint32_t t = w[q]; if (opd_type(t) == RT_TEMP && ((t >> 20) & 3u) == 1u && opd_sel(t) == 0 && (t & 3u) == 2u && opd_reg(&w[q]) == 1 && (opd_mask(t) & 0xeu))
      { char m[160]; snprintf(m, sizeof m, "r1.yzw written between the search and the taps (instruction %zu, opcode %u, operand %08x)", k, op(k), t); pr.why = m; return pr; } } }
  for (size_t k = taps.front().k; k <= taps.back().k; k++) {   /* s1c71: one straight-line path from the first to the last tap */
    uint32_t x = op(k);
    /* break, breakc, call, callc, case, continue, continuec, default, discard, else, endif, endloop, endswitch, if, label, loop, ret, retc, switch */
    if (x == 2 || x == 3 || (x >= 4 && x <= 8) || x == 10 || x == 13 || x == 18 || x == 21 || x == 22 || x == 23 || x == 31 || x == 44 || x == 48 || x == 62 || x == 63 || x == 76)
      { pr.why = "control flow between the taps"; return pr; } }
  /* the texel-size operand: the set-up's `mul r2.y, cbX[n].z, l(-7.0)` before the loop (the S1 patch checked its shape) */
  uint32_t cbslot = 0, cbreg = 0; bool have_dims = false;
  for (size_t k = L; k-- > 0;) if (op(k) == OP_MUL) { const uint32_t *p = &w[st[k]];
    if (p[1] == dst(RT_TEMP, 2, "y").t[0] && p[2] == 2 && ((p[3] >> 12) & 0xff) == RT_CB && ilen(k) >= 8 && p[6] == imm1f(-7.0f).t[0] && p[7] == f2u(-7.0f)) { cbslot = p[4]; cbreg = p[5]; have_dims = true; break; } }
  if (!have_dims) { pr.why = "no `mul r2.y, cb.z, -7` before the loop"; return pr; }
  /* the taps must not sit inside the loop (they do not) and every tap must come after the loop */
  const uint32_t T = ntemps, SC = T, P0 = T + 1, P1 = T + 2, SF = T + 3, SV = T + 4;   /* scratch, bits words, safety flag (.x; .yz scratch, .w mismatches); check mode: SV = r1.yzw at the first tap */
  auto pword = [&] (int kk) -> Opd { static const char *sw[4] = { "xxxx", "yyyy", "zzzz", "wwww" }; return s(kk < 4 ? P0 : P1, sw[kk & 3]); };
  auto pdst = [&] (int kk) -> Opd { static const char *m[4] = { "x", "y", "z", "w" }; return r(kk < 4 ? P0 : P1, m[kk & 3]); };
  /* assemble: everything before the loop, 8 unrolled copies with packing, everything after it with the taps replaced */
  std::vector<uint32_t> o; o.reserve(w.size() * 2);
  size_t endRes = st[last_res] + ilen(last_res);
  for (size_t i = 0; i < st[L]; i++) {
    if (i == endRes && pmode == P_CHECK && !have_u7) { Code d; d.dcl_uav_buf(7, RET_UINT); o.insert(o.end(), d.w.begin(), d.w.end()); }
    if (i == st[ntemps_at] + 1) { o.push_back(pmode == P_CHECK ? T + 5 : T + 4); continue; }
    o.push_back(w[i]); }
  for (int copy = 0; copy < 8; copy++) {
    for (size_t k = L + 3; k < E; k++) {      /* body without ilt/breakc; the iadd r1.x, 2 at the end is kept */
      size_t gi = std::find(ges.begin(), ges.end(), k) - ges.begin();
      std::vector<size_t> ops; uint32_t dreg = 0, treg = 0;
      if (gi < ges.size()) { opd_starts(w, st[k], ilen(k), ops); dreg = opd_reg(&w[ops[0]]); treg = opd_reg(&w[ops[1]]);
        if (strict) { Code c; c.ins(OP_LT, { r(SC, "xyzw"), s(3, "wwww"), s(treg, "xyzw") }); o.insert(o.end(), c.w.begin(), c.w.end()); } }   /* ref < texel, before the ge (the 8th ge overwrites r3) */
      o.insert(o.end(), w.begin() + st[k], w.begin() + st[k] + ilen(k));
      if (gi < ges.size()) {                  /* pack the 4 compare bits of gather gi of row pair `copy` */
        Code c;
        c.ins(OP_AND, { r(SC, "xyzw"), s(strict ? SC : dreg, "xyzw"), imm4u(1, 2, 4, 8) });
        c.ins(OP_OR, { r(SC, "xy"), s(SC, "xzxx"), s(SC, "ywyy") });
        c.ins(OP_OR, { r(SC, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
        if (gi == 0) c.ins(OP_MOV, { pdst(copy), s(SC, "xxxx") });
        else { c.ins(OP_ISHL, { r(SC, "x"), s(SC, "xxxx"), imm1u(4u * (uint32_t)gi) }); c.ins(OP_OR, { pdst(copy), pword(copy), s(SC, "xxxx") }); }
        o.insert(o.end(), c.w.begin(), c.w.end());
      }
    }
  }
  o.insert(o.end(), w.begin() + st[L + 1], w.begin() + st[L + 1] + ilen(L + 1));   /* the exit test `ilt r2.x, l(7), r1.x` the original runs last (r2.x = true) */
  for (size_t k = E + 1; k < st.size(); k++) {
    size_t a = st[k], l = ilen(k);
    const Tap *tp = nullptr; for (auto &t : taps) if (t.k == k) tp = &t;
    if (!tp) { o.insert(o.end(), w.begin() + a, w.begin() + a + l); continue; }
    int g = (tp->ox + 7) / 2, kk = (tp->oy + 7) / 2; uint32_t base = 4u * (uint32_t)g;
    const bool inv = broken == 1 && tp->ox == -1 && tp->oy == -1;   /* s1c71 negative controls (never in a real build) */
    const Opd boff = broken == 2 ? imm4u(base + 1, base, base + 2, base + 3) : imm4u(base, base + 1, base + 2, base + 3);
    Code c;
    if (tp == &taps[0]) {                     /* once, before the first tap: SF.x = the window's float offsets are exact (11.2) */
      c.ins(OP_MUL, { r(SC, "xyzw"), cb(cbslot, cbreg, "zzww"), imm4f(7.f, -7.f, 7.f, -7.f) });   /* +-7 texels, exact */
      c.ins(OP_ADD, { r(SC, "xyzw"), s(1, "yyzz"), s(SC, "xyzw") });                                /* u+7dz, u-7dz, v+7dw, v-7dw: as the loop computes them */
      c.ins(OP_AND, { r(SC, "xyzw"), s(SC, "xyzw"), imm4u(0xff800000u, 0xff800000u, 0xff800000u, 0xff800000u) });
      c.ins(OP_AND, { r(SF, "yz"), s(1, "yyzy"), imm4u(0, 0xff800000u, 0xff800000u, 0) });           /* sign+exponent of u, v */
      c.ins(OP_IEQ, { r(SC, "xyzw"), s(SC, "xyzw"), s(SF, "yyzz") });
      c.ins(OP_AND, { r(SC, "xy"), s(SC, "xyxx"), s(SC, "zwzz") });
      c.ins(OP_AND, { r(SF, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
      c.ins(OP_ULT, { r(SC, "xy"), s(SF, "yzyy"), imm4u(0x7f800000u, 0x7f800000u, 0, 0) });          /* finite, non-negative */
      c.ins(OP_AND, { r(SC, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "xxxx") });
      /* s1c71: |u|,|v| bound: sign+exponent(u) <= exponent(dz) + 20 (so ulp(u) <= dz/8 and u*size < 2^21: every u + k*dz and
         the texel-space value u*size - 0.5 are exact); same for v with dw. A negative u fails it (sign bit). */
      c.ins(OP_AND, { r(SC, "xy"), cb(cbslot, cbreg, "zwzz"), imm4u(0x7f800000u, 0x7f800000u, 0, 0) });
      c.ins(OP_IADD, { r(SC, "xy"), s(SC, "xyxx"), imm4u(20u << 23, 20u << 23, 0, 0) });
      c.ins(OP_UGE, { r(SC, "xy"), s(SC, "xyxx"), s(SF, "yzyy") });
      c.ins(OP_AND, { r(SC, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "xxxx") });
      /* s1c71: dims = exactly the texture's texel size: dz, dw powers of two (zero mantissa) and dz*width == dw*height == 1 */
      c.ins(OP_AND, { r(SC, "zw"), cb(cbslot, cbreg, "zzzw"), imm4u(0, 0, 0x007fffffu, 0x007fffffu) });
      c.ins(OP_IEQ, { r(SC, "zw"), s(SC, "zzzw"), imm4u(0, 0, 0, 0) });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "zzzz") });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "wwww") });
      c.ins(OP_RESINFO, { r(SC, "xy"), imm1u(0), res(1, "xyzw") }, 2u << 11);   /* resinfo_uint: width, height of t1 */
      c.ins(OP_UTOF, { r(SC, "xy"), s(SC, "xyxx") });
      c.ins(OP_MUL, { r(SC, "xy"), s(SC, "xyxx"), cb(cbslot, cbreg, "zwzz") });
      c.ins(OP_EQ, { r(SC, "xy"), s(SC, "xyxx"), imm4f(1.f, 1.f, 1.f, 1.f) });
      c.ins(OP_AND, { r(SC, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "xxxx") });
      /* s1c71: the taps' reference r1.w and the search's r3.w = r1.w + 0.0 are the same bits */
      c.ins(OP_ADD, { r(SC, "x"), s(1, "wwww"), imm1f(0.f) });
      c.ins(OP_IEQ, { r(SC, "x"), s(SC, "xxxx"), s(1, "wwww") });
      c.ins(OP_AND, { r(SF, "x"), s(SF, "xxxx"), s(SC, "xxxx") });
      if (pmode == P_CHECK) { c.ins(OP_MOV, { r(SF, "w"), imm1u(0) }); c.ins(OP_MOV, { r(SV, "yzw"), s(1, "xyzw") }); }   /* the last tap may overwrite r1 */
      if (pmode == P_CHECK) {                 /* u7[28] += safe pixels, u7[29] += unsafe pixels */
        c.ins(OP_MOVC, { r(SC, "x"), s(SF, "xxxx"), imm1u(28), imm1u(29) });
        c.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), s(SC, "xxxx"), imm1u(1) });
      }
    }
    if (pmode == P_CHECK) {                   /* the real tap stays; on safe pixels compare it with the bits */
      o.insert(o.end(), w.begin() + a, w.begin() + a + l);
      c.if_(s1(SF, 'x'), 1);
      c.ins(OP_UBFE, { r(SC, "xyzw"), imm4u(1, 1, 1, 1), boff, pword(kk) });
      if (inv) c.ins(OP_XOR, { r(SC, "xyzw"), s(SC, "xyzw"), imm4u(1, 1, 1, 1) });
      c.ins(OP_UTOF, { r(SC, "xyzw"), s(SC, "xyzw") });
      c.ins(OP_NE, { r(SC, "xyzw"), s(SC, "xyzw"), s(tp->dreg, "xyzw") });
      c.ins(OP_OR, { r(SC, "xy"), s(SC, "xzxx"), s(SC, "ywyy") });
      c.ins(OP_OR, { r(SC, "x"), s(SC, "xxxx"), s(SC, "yyyy") });
      c.ins(OP_AND, { r(SC, "x"), s(SC, "xxxx"), imm1u(1) });
      c.ins(OP_IADD, { r(SF, "w"), s(SF, "wwww"), s(SC, "xxxx") });   /* s1c71: per-pixel count, one atomic after the last tap */
      c.endif_();
      if (tp == &taps.back()) {               /* after the last tap: u7[30] += mismatching taps, u7[31] += mismatching pixels, first one recorded */
        c.if_(s1(SF, 'x'), 1);
        c.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), imm1u(30), s(SF, "wwww") });
        c.if_(s1(SF, 'w'), 1);
        c.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), imm1u(31), imm1u(1) });
        c.ins(OP_IMM_ATOMIC_IADD, { r(SC, "x"), uavd(7, "xyzw"), imm1u(32), imm1u(1) });
        c.if_(s1(SC, 'x'), 0);
        c.ins(OP_STORE_UAV_TYPED, { uavd(7, "xyzw"), imm1u(33), s(SV, "yyyy") });
        c.ins(OP_STORE_UAV_TYPED, { uavd(7, "xyzw"), imm1u(34), s(SV, "zzzz") });
        c.ins(OP_STORE_UAV_TYPED, { uavd(7, "xyzw"), imm1u(35), s(SV, "wwww") });
        c.ins(OP_STORE_UAV_TYPED, { uavd(7, "xyzw"), imm1u(36), s(SF, "wwww") });
        c.endif_();
        c.endif_();
        c.endif_();
      }
    } else {                                  /* safe pixel: the bits; otherwise the game's own tap */
      c.if_(s1(SF, 'x'), 1);
      c.ins(OP_UBFE, { r(tp->dreg, "xyzw"), imm4u(1, 1, 1, 1), boff, pword(kk) });
      if (inv) c.ins(OP_XOR, { r(tp->dreg, "xyzw"), s(tp->dreg, "xyzw"), imm4u(1, 1, 1, 1) });
      c.ins(OP_UTOF, { r(tp->dreg, "xyzw"), s(tp->dreg, "xyzw") });
      c.else_();
      c.w.insert(c.w.end(), w.begin() + a, w.begin() + a + l);
      c.endif_();
    }
    o.insert(o.end(), c.w.begin(), c.w.end());
  }
  o[1] = (uint32_t)o.size();
  ch[si].data.resize(o.size() * 4); memcpy(ch[si].data.data(), o.data(), o.size() * 4);
  std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(c);
  pr.blob = write_container(keep); pr.ok = true; pr.taps = 64; pr.unrolled = 8;
  return pr;
}

}  // namespace b1eo
