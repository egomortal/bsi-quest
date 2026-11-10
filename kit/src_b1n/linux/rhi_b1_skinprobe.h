/* rhi_b1_skinprobe.h -- b134 BSI_B1_SKINPROBE: ceiling probe for "skin once per frame" (NOT exact; timing only; default off).
   Every UE3 GPU-skin vertex shader of BSI (225 in the Fair dump j0064) contains the same 13-instruction bone blend:
     imul rI, vBLENDINDICES, 3 ; then 3 rows x (mul, mad, mad, mad) reading cb3[k+rI.c] (VSBoneConstants, relative index).
   The probe replaces each of those 12 mul/mad with "mov <same dst>, cb3[row]" (row 0..2, immediate index) so the
   vertex shader no longer does the 12 relative constant loads + 48 FMA of the blend. Skinned meshes collapse to bone 0:
   the PICTURE CHANGES - only for sizing what a bit-exact skin-once build could save at most. Header-only, needs
   rhi_b1_shadoweo.h (b1eo token helpers / container). */
#pragma once
#include "rhi_b1_shadoweo.h"
namespace b1skp {
using namespace b1eo;
/* operand length (dwords) at p; 0 = malformed */
static inline uint32_t opd_len(const uint32_t *p, size_t n) {
  if (!n) return 0; uint32_t t = p[0], k = 1; uint32_t type = (t >> 12) & 0xff;
  bool ext = (t >> 31) & 1; while (ext) { if (k >= n) return 0; ext = (p[k] >> 31) & 1; k++; }
  if (type == RT_IMM32) { uint32_t nc = t & 3; return k + (nc == 1 ? 1 : nc == 2 ? 4 : 0); }
  if (type == 5 /* IMM64 */) { uint32_t nc = t & 3; return k + (nc == 1 ? 2 : nc == 2 ? 8 : 0); }
  uint32_t dims = (t >> 20) & 3;
  for (uint32_t d = 0; d < dims; d++) { uint32_t rep = (t >> (22 + 3 * d)) & 7;
    if (rep == 0) k += 1; else if (rep == 1) k += 2;
    else if (rep == 2 || rep == 3 || rep == 4) { k += (rep == 3 ? 1 : rep == 4 ? 2 : 0); if (k >= n) return 0; uint32_t l = opd_len(p + k, n - k); if (!l) return 0; k += l; }
    else return 0; }
  return k <= n ? k : 0; }
/* does operand at p reference cb3 with a relative index in dimension 1 (register)? */
static inline bool is_cb3_rel(const uint32_t *p, size_t n) {
  uint32_t t = p[0]; if (((t >> 12) & 0xff) != RT_CB || ((t >> 20) & 3) != 2) return false;
  uint32_t k = 1; bool ext = (t >> 31) & 1; while (ext) { ext = (p[k] >> 31) & 1; k++; }
  uint32_t rep0 = (t >> 22) & 7, rep1 = (t >> 25) & 7; if (rep0 != 0) return false;
  if (k >= n || p[k] != 3) return false; return rep1 == 2 || rep1 == 3; }
struct Result { std::vector<uint8_t> blob; int replaced = 0; std::string why; };
static inline Result patch_vs(const void *code, size_t len) {
  Result R; std::vector<Chunk> ch; if (!read_container((const uint8_t *)code, len, ch)) { R.why = "container"; return R; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0) { R.why = "no SHEX"; return R; }
  const std::vector<uint8_t> &sb = ch[si].data; if (sb.size() < 8 || sb.size() % 4) { R.why = "shex size"; return R; }
  std::vector<uint32_t> w(sb.size() / 4); memcpy(w.data(), sb.data(), sb.size());
  size_t total = w[1]; if (total > w.size()) { R.why = "len"; return R; }
  std::vector<uint32_t> out(w.begin(), w.begin() + 2); int hits = 0;
  for (size_t i = 2; i < total; ) {
    uint32_t L = ins_len(&w[i], total - i); if (!L) { R.why = "ins_len"; return R; }
    uint32_t op = w[i] & 0x7ff; bool rep = false;
    if ((op == OP_MUL || op == OP_MAD) && !((w[i] >> 31) & 1)) {
      size_t k = i + 1; uint32_t dl = opd_len(&w[k], i + L - k); size_t s = k + dl; bool cb3 = false;
      while (dl && s < i + L) { uint32_t sl = opd_len(&w[s], i + L - s); if (!sl) break; if (is_cb3_rel(&w[s], i + L - s)) cb3 = true; s += sl; }
      if (dl && cb3) { uint32_t row = (uint32_t)(hits / 4); if (row > 2) { R.why = "more than 12 blend instructions"; return R; }
        std::vector<uint32_t> ni; ni.push_back(0); ni.insert(ni.end(), w.begin() + k, w.begin() + k + dl);
        Opd c = cb(3, row, "xyzw"); ni.insert(ni.end(), c.t.begin(), c.t.end());
        ni[0] = OP_MOV | (uint32_t(ni.size()) << 24) | (w[i] & 0x00002000u /* saturate */);
        out.insert(out.end(), ni.begin(), ni.end()); hits++; rep = true; } }
    if (!rep) out.insert(out.end(), w.begin() + i, w.begin() + i + L);
    i += L; }
  if (hits != 12) { R.why = "blend pattern not found (" + std::to_string(hits) + " cb3 mul/mad)"; return R; }
  out[1] = (uint32_t)out.size();
  ch[si].data.resize(out.size() * 4); memcpy(ch[si].data.data(), out.data(), out.size() * 4);
  R.blob = write_container(ch); R.replaced = hits; return R; }
}
