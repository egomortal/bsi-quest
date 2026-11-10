/* rhi_b1_fxsplit.h -- build 124 (chat 4, task #23): DXBC twin of the game's FXAA pixel shader for the exact edge/stencil split
   (BSI_B1_FXAASPLIT; runtime in rhi_b1_pp76.inc fx_draw, hook b1fx_ps_created in rhi_b1_dxvk.cpp).

   The game's FXAA 3.11 shader (fs.56e172d0 SM5 / fs.45fe1452 SM4) is, at the top level:
       <declarations> <preamble: centre sample_l -> r0, gather4 x2, min / max, mul, add, max, ge rX.c>
       if_nz rX.c  <edge search + resample into r0.xyz>  endif
       mov o0.xyzw, r0.xyzz
       ret
   The twin keeps every token of the declarations and the preamble unchanged and replaces the whole if_nz ... endif block by
       discard_nz rX.c        (same condition operand tokens)
   followed by the unchanged tail (mov o0, ret). So for a pixel whose edge test is false the twin executes exactly the
   original's instructions (the preamble, then the same mov) and writes the same value; a pixel whose test is true is
   discarded (no colour, no stencil write). The runtime draws the twin with stencil REPLACE 1 (marks the non-edge pixels),
   then the UNCHANGED game shader with stencil EQUAL 0 (only the edge pixels, packed into dense waves).
   Pure functions; no DXVK types. Structural checks below refuse anything that is not exactly this shape. */
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace b1fxs {
  enum : uint32_t { OP_DISCARD = 13, OP_ELSE = 18, OP_ENDIF = 21, OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_GE = 29, OP_IF = 31,
                    OP_LOOP = 48, OP_CUSTOMDATA = 53, OP_MOV = 54, OP_RET = 62, OP_SAMPLE_L = 72, OP_SWITCH = 76, OP_GATHER4 = 109,
                    OP_DCL_FIRST = 88, OP_DCL_LAST = 106 /* dcl_* opcodes of SM4 (88..106) */ };
  struct Res { bool ok = false; std::string why; std::vector<uint8_t> blob; uint32_t n_pre = 0, n_block = 0, n_gather = 0, n_sample = 0; };
  struct Chunk { uint32_t tag; std::vector<uint8_t> data; };
  static inline uint32_t fourcc(const char *s) { uint32_t v; std::memcpy(&v, s, 4); return v; }
  static inline uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; } return h; }
  static inline bool read_container(const uint8_t *b, size_t n, std::vector<Chunk> &out) {
    if (n < 32 || std::memcmp(b, "DXBC", 4)) return false;
    uint32_t nch; std::memcpy(&nch, b + 28, 4); if (nch > 64 || 32 + 4ull * nch > n) return false;
    for (uint32_t i = 0; i < nch; i++) { uint32_t off, tag, sz; std::memcpy(&off, b + 32 + 4 * i, 4); if (off + 8ull > n) return false;
      std::memcpy(&tag, b + off, 4); std::memcpy(&sz, b + off + 4, 4); if (off + 8ull + sz > n) return false;
      out.push_back({ tag, std::vector<uint8_t>(b + off + 8, b + off + 8 + sz) }); }
    return true; }
  /* the 16-byte digest is not validated by the converter but is DXVK's shader key (with the length): a new FNV of the body */
  static inline std::vector<uint8_t> write_container(const std::vector<Chunk> &ch) {
    std::vector<uint8_t> o(32 + 4 * ch.size());
    std::memcpy(&o[0], "DXBC", 4); uint32_t one = 1, nch = (uint32_t)ch.size(); std::memcpy(&o[20], &one, 4); std::memcpy(&o[28], &nch, 4);
    for (size_t i = 0; i < ch.size(); i++) { uint32_t off = (uint32_t)o.size(), sz = (uint32_t)ch[i].data.size(); std::memcpy(&o[32 + 4 * i], &off, 4);
      o.resize(o.size() + 8); std::memcpy(&o[off], &ch[i].tag, 4); std::memcpy(&o[off + 4], &sz, 4); o.insert(o.end(), ch[i].data.begin(), ch[i].data.end()); }
    uint32_t tot = (uint32_t)o.size(); std::memcpy(&o[24], &tot, 4);
    uint64_t h1 = fnv64(o.data() + 20, o.size() - 20, 0xcbf29ce484222325ull ^ 0xf1a5f1a5ull), h2 = fnv64(o.data() + 20, o.size() - 20, 0x6c62272e07bb0142ull ^ 0xf1a50124ull);
    std::memcpy(&o[4], &h1, 8); std::memcpy(&o[12], &h2, 8);
    return o; }
  static inline uint32_t ins_len(const uint32_t *p, size_t n) {
    if (!n) return 0; uint32_t op = p[0] & 0x7ffu, l = (p[0] >> 24) & 0x7fu;
    if (op == OP_CUSTOMDATA) { if (n < 2) return 0; l = p[1]; }
    return (l && l <= n) ? l : 0; }

  static inline Res make_twin(const void *code, size_t len) {
    Res r; const uint8_t *b = (const uint8_t *)code;
    std::vector<Chunk> ch; if (!read_container(b, len, ch)) { r.why = "container"; return r; }
    int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
    if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { r.why = "no program"; return r; }
    std::vector<uint32_t> w(ch[si].data.size() / 4); std::memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
    if ((w[0] >> 16) != 0) { r.why = "not a pixel shader"; return r; }
    if (w[1] != w.size()) { r.why = "program length"; return r; }
    std::vector<size_t> st; for (size_t i = 2; i < w.size();) { uint32_t l = ins_len(&w[i], w.size() - i); if (!l) { r.why = "parse"; return r; } st.push_back(i); i += l; }
    auto op = [&] (size_t k) { return w[st[k]] & 0x7ffu; };
    /* the first top-level if: everything before it is declarations + straight-line preamble */
    size_t kif = st.size();
    for (size_t k = 0; k < st.size(); k++) {
      uint32_t o = op(k);
      if (o == OP_IF) { kif = k; break; }
      if (o == OP_LOOP || o == OP_SWITCH || o == OP_ELSE || o == OP_ENDIF || o == OP_ENDLOOP || o == OP_ENDSWITCH || o == OP_DISCARD || o == OP_RET) { r.why = "control flow before the edge test"; return r; }
      if (o == OP_GATHER4) r.n_gather++;
      if (o == OP_SAMPLE_L) r.n_sample++;
      if (o < OP_DCL_FIRST || o > OP_DCL_LAST) r.n_pre++;
    }
    if (kif == st.size() || kif == 0) { r.why = "no top-level if"; return r; }
    if (!((w[st[kif]] >> 18) & 1u)) { r.why = "the edge test is if_z, not if_nz"; return r; }
    if (op(kif - 1) != OP_GE) { r.why = "the instruction before the if is not ge"; return r; }
    if (r.n_sample != 1 || r.n_gather != 2) { r.why = "preamble is not 1 sample_l + 2 gather4"; return r; }
    /* the condition operand of the if must be the destination register of the ge (same temp, one component) */
    uint32_t ifl = ins_len(&w[st[kif]], w.size() - st[kif]);
    if (ifl != 3) { r.why = "if operand is not a single temp component"; return r; }
    { uint32_t t = w[st[kif] + 1], idx = w[st[kif] + 2]; uint32_t ge0 = w[st[kif - 1] + 1], geidx = w[st[kif - 1] + 2];
      uint32_t comp = (t >> 4) & 3u, mask = (ge0 >> 4) & 0xfu;
      if (((t >> 12) & 0xffu) != 0 || ((t >> 2) & 3u) != 2 || ((ge0 >> 12) & 0xffu) != 0 || idx != geidx || mask != (1u << comp)) { r.why = "if operand is not the ge's result"; return r; } }
    /* matching endif, no else at depth 1 */
    size_t kend = 0; { int depth = 0;
      for (size_t k = kif; k < st.size(); k++) { uint32_t o = op(k);
        if (o == OP_IF || o == OP_LOOP || o == OP_SWITCH) depth++;
        if (o == OP_ELSE && depth == 1) { r.why = "the edge block has an else"; return r; }
        if (o == OP_ENDIF || o == OP_ENDLOOP || o == OP_ENDSWITCH) { if (--depth == 0) { if (o != OP_ENDIF) { r.why = "nesting"; return r; } kend = k; break; } } } }
    if (!kend) { r.why = "no matching endif"; return r; }
    r.n_block = (uint32_t)(kend - kif - 1);
    /* tail: exactly mov o0, r0 ; ret */
    if (kend + 3 != st.size() || op(kend + 1) != OP_MOV || op(kend + 2) != OP_RET) { r.why = "tail is not mov + ret"; return r; }
    { uint32_t d = w[st[kend + 1] + 1]; if (((d >> 12) & 0xffu) != 2u /* output */) { r.why = "tail mov does not write an output"; return r; } }
    for (size_t k = kif + 1; k < kend; k++) if (op(k) == OP_DISCARD || op(k) == OP_RET) { r.why = "discard / ret inside the edge block"; return r; }
    /* the twin */
    std::vector<uint32_t> o(w.begin(), w.begin() + st[kif]);
    o.push_back(OP_DISCARD | (1u << 18) | (3u << 24)); o.push_back(w[st[kif] + 1]); o.push_back(w[st[kif] + 2]);
    o.insert(o.end(), w.begin() + st[kend + 1], w.end());
    o[1] = (uint32_t)o.size();
    ch[si].data.resize(o.size() * 4); std::memcpy(ch[si].data.data(), o.data(), o.size() * 4);
    /* drop the statistics chunk (instruction counts no longer match; the converter does not need it) */
    std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(std::move(c));
    r.blob = write_container(keep); r.ok = true; return r; }
}
