/* rhi_b1_shadoweo.h -- S1 "shadow early-out" (Fable note 02 S1 / 05 5.1): DXBC-level tools, no DXVK dependency.
   Header-only so it is compiled inside rhi_b1_dxvk.cpp (via rhi_b1_shadoweo.inc) and by the offline test tools alike.

   What it does
   - Patches the game's two contact-hardening shadow-projection pixel shaders (UE3 SM5, ShadowFilterQualityBias 3):
       ps c29c9e4e876e162b (cascades)  and  ps d108de4649b4e7c0 (per-object / spot)   (bridge.c dxbc_key names)
     Each runs a 16x16-texel blocker search (loop: 8 iterations x 8 gather4 of ShadowDepthTexture t1, counting texels
     whose depth is not >= the receiver depth) and then: count == 0 -> fully lit, count >= 256 -> fully shadowed, else
     PCF. The patch inserts, before that loop, a test against a conservative min/max of the search window read from a
     block map (t127, built by the compute shaders below) and skips the loop when the window is provably all
     non-blockers (count would be 0) or all blockers (count would be 256), setting the count register to exactly that
     value. The code after the loop is unchanged, so the output is bit-identical by construction (see README).
     Receivers whose depth reconstruction degenerates (the sky: recv = -inf with NaN coordinates, or recv = NaN) are
     decided without a window where IEEE semantics fix the count whatever the gathers return (see patch_ps).
   - Builds the compute shaders of the block map (geometry: struct Geo, default L1 block B = 4 texels, span S = 6):
       CS1: one thread per BxB texel block of the atlas -> L1 (R32G32_UINT: ~min, max of the raw depth bits)
       CS2: L2[i,j] = combine(L1[i..i+S-1, j..j+S-1])   (one L2 texel covers any window touching <= S blocks per axis)
       CS3: whole-atlas "no invalid block" flag (t126 of the PS), from the L2 at stride S
     Min/max are taken on the raw 32-bit patterns: for non-negative non-NaN floats the unsigned order is the float order,
     and the result is always one of the texels' own bit patterns. A block holding any negative or NaN pattern (sign bit
     or exponent all-ones with mantissa != 0: unsigned > 0x7f800000) is marked invalid (min = max = 0xffffffff) and
     disables the early-out for every window touching it. The min is stored inverted (~min), so a map that is not
     bound (reads 0) decodes as invalid and can never cause an early-out.
   Register/slot choices (never used by the UE3 RHI): PS t127 = L2 map, t126 = atlas flag; CS t127/t125 = sources,
   u63/u62/u61 = destinations, cb13 = parameters; stats/check variants: PS u7 = counters (R32_UINT typed buffer). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <string>

namespace b1eo {

/* ---------------- token encoding (D3D10/11 tokenized program format) ---------------- */
enum : uint32_t { RT_TEMP = 0, RT_INPUT = 1, RT_OUTPUT = 2, RT_IMM32 = 4, RT_SAMPLER = 6, RT_RESOURCE = 7, RT_CB = 8, RT_UAV = 30, RT_TGSM = 31, RT_TID = 32, RT_TGID = 33, RT_TIG = 34 };
enum : uint32_t { OP_ADD = 0, OP_AND = 1, OP_BREAKC = 3, OP_DIV = 14, OP_ELSE = 18, OP_ENDIF = 21, OP_ENDLOOP = 22, OP_EQ = 24, OP_FTOI = 27, OP_GE = 29, OP_IEQ = 32,
  OP_IADD = 30, OP_IF = 31, OP_IGE = 33, OP_ILT = 34, OP_IMIN = 37, OP_INE = 39, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43, OP_LD = 45, OP_LOOP = 48, OP_LT = 49,
  OP_MAD = 50, OP_MIN = 51, OP_CUSTOMDATA = 53, OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56, OP_NE = 57, OP_NOT = 59, OP_OR = 60, OP_RESINFO = 61, OP_RET = 62,
  OP_RETC = 63, OP_ROUND_NI = 65, OP_ULT = 79, OP_UGE = 80, OP_UDIV = 78, OP_UMAX = 83, OP_UMIN = 84, OP_UTOF = 86, OP_IMAD = 35, OP_BREAK = 2, OP_DCL_RESOURCE = 88, OP_DCL_CB = 89, OP_DCL_INPUT = 95,
  OP_DCL_TEMPS = 104, OP_GATHER4 = 109, OP_DCL_SAMPLER = 90, OP_DCL_TGSM_STRUCTURED = 160, OP_LD_STRUCTURED = 167, OP_STORE_STRUCTURED = 168, OP_SYNC = 190, OP_USHR = 85, OP_DCL_THREAD_GROUP = 155, OP_DCL_UAV_TYPED = 156, OP_STORE_UAV_TYPED = 164, OP_ATOMIC_AND = 169, OP_ATOMIC_IADD = 173, OP_IMM_ATOMIC_IADD = 180 };
enum : uint32_t { DIM_BUFFER = 1, DIM_TEX2D = 3 };
enum : uint32_t { RET_UINT = 4, RET_FLOAT = 5 };

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
/* "xyzw"-style string -> write mask bits / swizzle bits */
static inline uint32_t cidx(char c) { return c == 'x' ? 0u : c == 'y' ? 1u : c == 'z' ? 2u : 3u; }
static inline uint32_t mask_of(const char *m) { uint32_t r = 0; for (; *m; m++) r |= 1u << cidx(*m); return r; }
static inline uint32_t swz_of(const char *s) { uint32_t r = 0; size_t n = strlen(s); for (uint32_t i = 0; i < 4; i++) r |= cidx(s[i < n ? i : n - 1]) << (2 * i); return r; }

struct Opd { std::vector<uint32_t> t; };
static inline Opd dst(uint32_t type, uint32_t idx, const char *mask) { return { { 2u | (0u << 2) | (mask_of(mask) << 4) | (type << 12) | (1u << 20), idx } }; }
static inline Opd src(uint32_t type, uint32_t idx, const char *swz, bool neg = false, bool abs = false) {
  Opd o { { 2u | (1u << 2) | (swz_of(swz) << 4) | (type << 12) | (1u << 20) | ((neg || abs) ? 0x80000000u : 0u), idx } };
  if (neg || abs) o.t.insert(o.t.begin() + 1, 1u | (neg ? 1u << 6 : 0u) | (abs ? 1u << 7 : 0u));
  return o; }
static inline Opd sel1(uint32_t type, uint32_t idx, char c) { return { { 2u | (2u << 2) | (cidx(c) << 4) | (type << 12) | (1u << 20), idx } }; }
static inline Opd r(uint32_t i, const char *m) { return dst(RT_TEMP, i, m); }            /* temp as destination */
static inline Opd s(uint32_t i, const char *z, bool neg = false, bool abs = false) { return src(RT_TEMP, i, z, neg, abs); }
static inline Opd s1(uint32_t i, char c) { return sel1(RT_TEMP, i, c); }
static inline Opd imm4u(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return { { 2u | (RT_IMM32 << 12), a, b, c, d } }; }
static inline Opd imm4f(float a, float b, float c, float d) { return imm4u(f2u(a), f2u(b), f2u(c), f2u(d)); }
static inline Opd imm1u(uint32_t a) { return { { 1u | (RT_IMM32 << 12), a } }; }
static inline Opd imm1f(float a) { return imm1u(f2u(a)); }
static inline Opd cb(uint32_t slot, uint32_t reg, const char *swz) { return { { 2u | (1u << 2) | (swz_of(swz) << 4) | (RT_CB << 12) | (2u << 20), slot, reg } }; }
static inline Opd res(uint32_t idx, const char *swz) { return src(RT_RESOURCE, idx, swz); }
static inline Opd uavd(uint32_t idx, const char *mask) { return dst(RT_UAV, idx, mask); }
static inline Opd tid(const char *swz) { return { { 2u | (1u << 2) | (swz_of(swz) << 4) | (RT_TID << 12) | (0u << 20) } }; }
/* s1b: thread-group id / thread id in group (0-dim inputs), groupshared g# (1-dim), sampler as gather4's channel select (s#.x, as fxc) */
static inline Opd tgid(const char *swz) { return { { 2u | (1u << 2) | (swz_of(swz) << 4) | (RT_TGID << 12) } }; }
static inline Opd tig(const char *swz) { return { { 2u | (1u << 2) | (swz_of(swz) << 4) | (RT_TIG << 12) } }; }
static inline Opd gd(uint32_t idx, const char *mask) { return dst(RT_TGSM, idx, mask); }
static inline Opd gs(uint32_t idx, const char *swz) { return src(RT_TGSM, idx, swz); }
static inline Opd smp(uint32_t idx) { return sel1(RT_SAMPLER, idx, 'x'); }

struct Code {
  std::vector<uint32_t> w;
  void ins(uint32_t op, std::initializer_list<Opd> ops, uint32_t extra = 0) {
    size_t at = w.size(); w.push_back(0); for (auto &o : ops) w.insert(w.end(), o.t.begin(), o.t.end());
    w[at] = op | extra | (uint32_t(w.size() - at) << 24); }
  void raw(std::initializer_list<uint32_t> v) { w.insert(w.end(), v.begin(), v.end()); }
  void append(const uint32_t *p, size_t n) { w.insert(w.end(), p, p + n); }
  /* control flow; test: 1 = nonzero, 0 = zero */
  void if_(Opd c, int nz) { ins(OP_IF, { c }, uint32_t(nz ? 1 : 0) << 18); }
  void else_() { ins(OP_ELSE, {}); }
  void endif_() { ins(OP_ENDIF, {}); }
  void retc(Opd c, int nz) { ins(OP_RETC, { c }, uint32_t(nz ? 1 : 0) << 18); }
  void ret() { ins(OP_RET, {}); }
  /* declarations */
  void dcl_temps(uint32_t n) { raw({ OP_DCL_TEMPS | (2u << 24), n }); }
  void dcl_tex2d(uint32_t t, uint32_t rt) { raw({ OP_DCL_RESOURCE | (DIM_TEX2D << 11) | (4u << 24), (RT_RESOURCE << 12) | (1u << 20), t, rt * 0x1111u }); }
  void dcl_buf(uint32_t t, uint32_t rt) { raw({ OP_DCL_RESOURCE | (DIM_BUFFER << 11) | (4u << 24), (RT_RESOURCE << 12) | (1u << 20), t, rt * 0x1111u }); }
  void dcl_uav_tex2d(uint32_t u, uint32_t rt) { raw({ OP_DCL_UAV_TYPED | (DIM_TEX2D << 11) | (4u << 24), (RT_UAV << 12) | (1u << 20), u, rt * 0x1111u }); }
  void dcl_uav_buf(uint32_t u, uint32_t rt) { raw({ OP_DCL_UAV_TYPED | (DIM_BUFFER << 11) | (4u << 24), (RT_UAV << 12) | (1u << 20), u, rt * 0x1111u }); }
  void dcl_cb(uint32_t slot, uint32_t n) { raw({ OP_DCL_CB | (4u << 24), 2u | (1u << 2) | (swz_of("xyzw") << 4) | (RT_CB << 12) | (2u << 20), slot, n }); }
  void dcl_tid(const char *mask) { raw({ OP_DCL_INPUT | (2u << 24), 2u | (mask_of(mask) << 4) | (RT_TID << 12) }); }
  void dcl_tg(uint32_t x, uint32_t y, uint32_t z) { raw({ OP_DCL_THREAD_GROUP | (4u << 24), x, y, z }); }
  /* s1b */
  void dcl_sampler(uint32_t sl) { raw({ OP_DCL_SAMPLER | (3u << 24), (RT_SAMPLER << 12) | (1u << 20), sl }); }
  void dcl_tgsm(uint32_t g, uint32_t stride, uint32_t count) { raw({ OP_DCL_TGSM_STRUCTURED | (5u << 24), (RT_TGSM << 12) | (1u << 20), g, stride, count }); }
  void dcl_in(uint32_t rt, const char *mask) { raw({ OP_DCL_INPUT | (2u << 24), 2u | (mask_of(mask) << 4) | (rt << 12) }); }
  void sync_gt() { raw({ OP_SYNC | (1u << 24) | (3u << 11) }); }            /* sync_g_t: groupshared memory + thread barrier */
  /* gather4 with fxc's extended tokens (resource dimension texture2d, return type float) */
  void gather4(Opd d, Opd coord, uint32_t t, uint32_t sl) {
    size_t at = w.size(); w.push_back(0); w.push_back(0x800000c2u); w.push_back(0x155543u);
    for (const Opd *o : { &d, &coord }) w.insert(w.end(), o->t.begin(), o->t.end());
    Opd rs = res(t, "xyzw"), ss = smp(sl); w.insert(w.end(), rs.t.begin(), rs.t.end()); w.insert(w.end(), ss.t.begin(), ss.t.end());
    w[at] = OP_GATHER4 | 0x80000000u | (uint32_t(w.size() - at) << 24); }
};

/* instruction length in dwords at p (n = dwords left); 0 = malformed */
static inline uint32_t ins_len(const uint32_t *p, size_t n) {
  if (!n) return 0; uint32_t op = p[0] & 0x7ffu, l = (p[0] >> 24) & 0x7fu;
  if (op == OP_CUSTOMDATA) { if (n < 2) return 0; l = p[1]; }
  return (l && l <= n) ? l : 0; }

static inline uint64_t fnv64(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
/* bridge.c dxbc_key (dxbc_info.h): FNV-1a 64 over the blob, then the length folded in */
static inline uint64_t dxbc_key(const void *p, size_t n) { uint64_t h = fnv64(p, n); h ^= (uint64_t)n; h *= 0x100000001b3ull; return h; }

/* ---------------- DXBC container ---------------- */
struct Chunk { uint32_t tag; std::vector<uint8_t> data; };
static inline bool read_container(const uint8_t *b, size_t n, std::vector<Chunk> &out) {
  if (n < 32 || memcmp(b, "DXBC", 4)) return false;
  uint32_t nch; memcpy(&nch, b + 28, 4); if (nch > 64 || 32 + 4ull * nch > n) return false;
  for (uint32_t i = 0; i < nch; i++) { uint32_t off, tag, sz; memcpy(&off, b + 32 + 4 * i, 4); if (off + 8ull > n) return false;
    memcpy(&tag, b + off, 4); memcpy(&sz, b + off + 4, 4); if (off + 8ull + sz > n) return false;
    out.push_back({ tag, std::vector<uint8_t>(b + off + 8, b + off + 8 + sz) }); }
  return true; }
/* the 16-byte digest is not validated by the converter; it is DXVK's shader key (with the length), so it must differ
   from the original's: we store a 128-bit FNV of the new body */
static inline std::vector<uint8_t> write_container(const std::vector<Chunk> &ch) {
  std::vector<uint8_t> o(32 + 4 * ch.size());
  memcpy(&o[0], "DXBC", 4); uint32_t one = 1, nch = (uint32_t)ch.size(); memcpy(&o[20], &one, 4); memcpy(&o[28], &nch, 4);
  for (size_t i = 0; i < ch.size(); i++) { uint32_t off = (uint32_t)o.size(), sz = (uint32_t)ch[i].data.size(); memcpy(&o[32 + 4 * i], &off, 4);
    o.resize(o.size() + 8); memcpy(&o[off], &ch[i].tag, 4); memcpy(&o[off + 4], &sz, 4); o.insert(o.end(), ch[i].data.begin(), ch[i].data.end()); }
  uint32_t tot = (uint32_t)o.size(); memcpy(&o[24], &tot, 4);
  uint64_t h1 = fnv64(o.data() + 20, o.size() - 20), h2 = fnv64(o.data() + 20, o.size() - 20, 0x6c62272e07bb0142ull ^ 0xb1e0b1e0ull);
  memcpy(&o[4], &h1, 8); memcpy(&o[12], &h2, 8);
  return o; }
static inline uint32_t fourcc(const char *s) { uint32_t v; memcpy(&v, s, 4); return v; }
static inline std::vector<uint8_t> shex_chunk(const Code &c, uint32_t type) {   /* program: version token, length, code */
  std::vector<uint32_t> w; w.push_back((type << 16) | (5u << 4) | 0u); w.push_back((uint32_t)c.w.size() + 2); w.insert(w.end(), c.w.begin(), c.w.end());
  std::vector<uint8_t> b(w.size() * 4); memcpy(b.data(), w.data(), b.size()); return b; }
static inline std::vector<uint8_t> empty_sig() { std::vector<uint8_t> b(8, 0); b[4] = 8; return b; }   /* count 0, header 8 */
static inline std::vector<uint8_t> cs_container(const Code &c) {
  std::vector<Chunk> ch; ch.push_back({ fourcc("ISGN"), empty_sig() }); ch.push_back({ fourcc("OSGN"), empty_sig() }); ch.push_back({ fourcc("SHEX"), shex_chunk(c, 5) });
  return write_container(ch); }

/* ---------------- compute shaders of the block map ----------------
   cb13[0] = (origin x, origin y, last x, last y) of the dispatch region in destination texels (inclusive bounds)
   cb13[1] = (source width - 1, source height - 1, -, -)   (clamp for the reads) */
/* Geometry: L1 block = (1 << shift) texels square; one L2 texel combines span x span L1 blocks starting at its own index.
   The search window plus margins is at most 19 texels wide (README), which touches at most ceil(19 / B) + 1 blocks, so
   span = that number guarantees one L2 texel covers it (the PS still checks hi - lo <= span - 1). Default B = 4: the
   cell covers 24x24 texels for a 16x16 search (B = 8 would cover 32x32 and prove far fewer pixels). */
struct Geo { uint32_t shift = 2, span = 6; uint32_t block() const { return 1u << shift; } };
static inline Geo geo_for(uint32_t shift) { Geo g; g.shift = shift; g.span = (19u + (1u << shift) - 1u) / (1u << shift) + 1u; return g; }
static inline std::vector<uint8_t> build_cs_reduce(int pass, Geo geo) {
  Code c;
  c.dcl_cb(13, 2);
  c.dcl_tex2d(127, pass == 1 ? RET_FLOAT : RET_UINT);
  c.dcl_uav_tex2d(63, RET_UINT);
  c.dcl_tid("xy");
  c.dcl_temps(5);
  c.dcl_tg(8, 8, 1);
  c.ins(OP_IADD, { r(0, "xy"), tid("xyxx"), cb(13, 0, "xyxx") });            /* destination texel */
  c.ins(OP_ILT, { r(0, "zw"), cb(13, 0, "zzzw"), s(0, "xxxy") });            /* last < dst: outside */
  c.ins(OP_OR, { r(0, "z"), s(0, "zzzz"), s(0, "wwww") });
  c.retc(s1(0, 'z'), 1);
  uint32_t n = pass == 1 ? geo.block() : geo.span;
  if (pass == 1) c.ins(OP_ISHL, { r(1, "xy"), s(0, "xyxx"), imm4u(geo.shift, geo.shift, 0, 0) });   /* first source texel of the block */
  else c.ins(OP_MOV, { r(1, "xy"), s(0, "xyxx") });
  c.ins(OP_MOV, { r(2, "xy"), imm4u(0xffffffffu, 0, 0, 0) });                 /* min, max (unsigned) */
  c.ins(OP_MOV, { r(3, "zw"), imm4u(0, 0, 0, 0) });                           /* ld: mip 0 */
  for (uint32_t dy = 0; dy < n; dy++) for (uint32_t dx = 0; dx < n; dx++) {
    c.ins(OP_IADD, { r(3, "xy"), s(1, "xyxx"), imm4u(dx, dy, 0, 0) });
    c.ins(OP_IMIN, { r(3, "xy"), s(3, "xyxx"), cb(13, 1, "xyxx") });          /* clamp: duplicates a real texel/block */
    if (pass == 1) {
      c.ins(OP_LD, { r(4, "x"), s(3, "xyzw"), res(127, "xxxx") });
      c.ins(OP_UMIN, { r(2, "x"), s(2, "xxxx"), s(4, "xxxx") });
      c.ins(OP_UMAX, { r(2, "y"), s(2, "yyyy"), s(4, "xxxx") });
    } else {
      c.ins(OP_LD, { r(4, "xy"), s(3, "xyzw"), res(127, "xyxx") });
      c.ins(OP_NOT, { r(4, "x"), s(4, "xxxx") });                               /* stored as ~min */
      c.ins(OP_UMIN, { r(2, "x"), s(2, "xxxx"), s(4, "xxxx") });
      c.ins(OP_UMAX, { r(2, "y"), s(2, "yyyy"), s(4, "yyyy") });
    }
  }
  /* any pattern above +inf (negative, -0, NaN) -> invalid (min = max = 0xffffffff) */
  c.ins(OP_ULT, { r(0, "z"), imm4u(0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u), s(2, "yyyy") });
  c.ins(OP_MOVC, { r(2, "xy"), s(0, "zzzz"), imm4u(0xffffffffu, 0xffffffffu, 0, 0), s(2, "xyxx") });
  c.ins(OP_NOT, { r(2, "x"), s(2, "xxxx") });   /* store ~min: a null/unbound map reads 0 -> min 0xffffffff = invalid, never an early-out */
  c.ins(OP_MOV, { r(2, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_STORE_UAV_TYPED, { uavd(63, "xyzw"), s(0, "xyyy"), s(2, "xyzw") });
  c.ret();
  return cs_container(c);
}

/* CS3 (one group of 256 threads): t126 source of the PS "no NaN in the atlas" flag. u62[0] is set to 1 before the dispatch;
   every thread reads the L2 texels at stride span (which together cover every L1 block) and clears it (atomic and) if one
   of them is invalid. cb13[0] = (number of strided texels, texels per row, span, -), cb13[1] = (L2 width - 1, height - 1). */
static inline std::vector<uint8_t> build_cs_nanfree() {
  Code c;
  c.dcl_cb(13, 2);
  c.dcl_tex2d(127, RET_UINT);
  c.dcl_uav_buf(62, RET_UINT);
  c.dcl_tid("x");
  c.dcl_temps(3);
  c.dcl_tg(256, 1, 1);
  c.ins(OP_MOV, { r(0, "x"), tid("xxxx") });
  c.ins(OP_MOV, { r(0, "y"), imm4u(1, 1, 1, 1) });
  c.ins(OP_MOV, { r(1, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_LOOP, {});
  c.ins(OP_UGE, { r(0, "z"), s(0, "xxxx"), cb(13, 0, "xxxx") });
  c.ins(OP_BREAKC, { s1(0, 'z') }, 1u << 18);
  c.ins(OP_UDIV, { r(1, "y"), r(1, "x"), s(0, "xxxx"), cb(13, 0, "yyyy") });  /* row, column */
  c.ins(OP_IMAD, { r(1, "xy"), s(1, "xyxx"), cb(13, 0, "zzzz"), imm4u(0, 0, 0, 0) });
  c.ins(OP_IMIN, { r(1, "xy"), s(1, "xyxx"), cb(13, 1, "xyxx") });
  c.ins(OP_LD, { r(2, "y"), s(1, "xyzw"), res(127, "yyyy") });
  c.ins(OP_ULT, { r(2, "z"), imm4u(0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u), s(2, "yyyy") });
  c.ins(OP_MOVC, { r(0, "y"), s(2, "zzzz"), imm4u(0, 0, 0, 0), s(0, "yyyy") });
  c.ins(OP_IADD, { r(0, "x"), s(0, "xxxx"), imm4u(256, 256, 256, 256) });
  c.ins(OP_ENDLOOP, {});
  c.ins(OP_ATOMIC_AND, { uavd(62, "xyzw"), imm1u(0), s(0, "yyyy") });
  c.ret();
  return cs_container(c);
}

/* ---------------- s1b: block-map kernels v2 (BSI_B1_SHADOWEO_MAPV=2, default) ----------------
   Same map (same L1/L2 values, same flag) as v1, fewer texture operations:
   - CS1g: L1 from (B/2)^2 gather4 per BxB block instead of B^2 ld (4 instead of 16 for B = 4). The gathers sit exactly on
     the corner of 2x2 texels (u = (x + 1) / w), so each returns texels x, x+1 of rows y, y+1; sampler s15 = nearest,
     clamp to edge, bound by the runtime (clamping duplicates edge texels, like v1's imin). Same fetch path as the PS's
     own blocker search (gather4 of t1).
     cb13[0] = (bx0, by0, bx1, by1), cb13[1] = (w - 1, h - 1, bits(1/w), bits(1/h))
   - CS2s: L2 through groupshared memory: an 8x8 group loads the (8+S-1)^2 L1 blocks it needs once (2.6 loads per thread
     instead of S^2 = 36), combines S along x into a second array, then S along y. Thread (0,0) of group (0,0) sets the
     atlas flag u62[0] = 1 (replaces v1's clearBuffer before CS3). cb13[0] = (cx0, cy0, last x, last y), cb13[1] = (bw-1, bh-1)
   - CS3w: the flag pass with one thread per strided L2 texel (v1: one group of 256 threads looping over all of them) and an
     atomic only for an invalid texel. cb13[0] = (nx, ny, span, -), cb13[1] = (bw - 1, bh - 1) */
static inline std::vector<uint8_t> build_cs_l1_gather(Geo geo) {
  Code c;
  c.dcl_cb(13, 2); c.dcl_sampler(15); c.dcl_tex2d(127, RET_FLOAT); c.dcl_uav_tex2d(63, RET_UINT); c.dcl_tid("xy"); c.dcl_temps(6); c.dcl_tg(8, 8, 1);
  c.ins(OP_IADD, { r(0, "xy"), tid("xyxx"), cb(13, 0, "xyxx") });            /* L1 block */
  c.ins(OP_ILT, { r(0, "zw"), cb(13, 0, "zzzw"), s(0, "xxxy") });
  c.ins(OP_OR, { r(0, "z"), s(0, "zzzz"), s(0, "wwww") });
  c.retc(s1(0, 'z'), 1);
  c.ins(OP_ISHL, { r(1, "xy"), s(0, "xyxx"), imm4u(geo.shift, geo.shift, 0, 0) });   /* first texel */
  c.ins(OP_ITOF, { r(1, "xy"), s(1, "xyxx") });
  c.ins(OP_MOV, { r(2, "xy"), imm4u(0xffffffffu, 0, 0, 0) });
  const uint32_t n = geo.block() / 2;
  for (uint32_t dy = 0; dy < n; dy++) for (uint32_t dx = 0; dx < n; dx++) {
    c.ins(OP_ADD, { r(3, "xy"), s(1, "xyxx"), imm4f(1.f + 2.f * dx, 1.f + 2.f * dy, 0.f, 0.f) });   /* corner of texels x+2dx.. +1 */
    c.ins(OP_MUL, { r(3, "xy"), s(3, "xyxx"), cb(13, 1, "zwzz") });
    c.gather4(r(4, "xyzw"), s(3, "xyxx"), 127, 15);
    c.ins(OP_UMIN, { r(5, "xy"), s(4, "xyxx"), s(4, "zwzz") });
    c.ins(OP_UMAX, { r(5, "zw"), s(4, "xxxy"), s(4, "xxzw") });
    c.ins(OP_UMIN, { r(5, "x"), s(5, "xxxx"), s(5, "yyyy") });
    c.ins(OP_UMAX, { r(5, "y"), s(5, "zzzz"), s(5, "wwww") });
    c.ins(OP_UMIN, { r(2, "x"), s(2, "xxxx"), s(5, "xxxx") });
    c.ins(OP_UMAX, { r(2, "y"), s(2, "yyyy"), s(5, "yyyy") });
  }
  c.ins(OP_ULT, { r(0, "z"), imm4u(0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u), s(2, "yyyy") });
  c.ins(OP_MOVC, { r(2, "xy"), s(0, "zzzz"), imm4u(0xffffffffu, 0xffffffffu, 0, 0), s(2, "xyxx") });
  c.ins(OP_NOT, { r(2, "x"), s(2, "xxxx") });
  c.ins(OP_MOV, { r(2, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_STORE_UAV_TYPED, { uavd(63, "xyzw"), s(0, "xyyy"), s(2, "xyzw") });
  c.ret();
  return cs_container(c);
}
static inline std::vector<uint8_t> build_cs_l2_lds(Geo geo) {
  const uint32_t T = 8, S = geo.span, AW = T + S - 1, NA = AW * AW, NH = AW * T;
  Code c;
  c.dcl_cb(13, 2); c.dcl_tex2d(127, RET_UINT); c.dcl_uav_tex2d(63, RET_UINT); c.dcl_uav_buf(62, RET_UINT);
  c.dcl_in(RT_TGID, "xy"); c.dcl_in(RT_TIG, "xy"); c.dcl_temps(6);
  c.dcl_tgsm(0, 8, NA); c.dcl_tgsm(1, 8, NH); c.dcl_tg(T, T, 1);
  c.ins(OP_IMAD, { r(0, "xy"), tgid("xyxx"), imm4u(T, T, 0, 0), cb(13, 0, "xyxx") });   /* tile origin (L2 = first L1 block of its cell) */
  c.ins(OP_IMAD, { r(0, "z"), tig("yyyy"), imm4u(T, T, T, T), tig("xxxx") });            /* flat thread index */
  c.ins(OP_OR, { r(0, "w"), tgid("xxxx"), tgid("yyyy") });
  c.ins(OP_OR, { r(0, "w"), s(0, "wwww"), s(0, "zzzz") });
  c.if_(s1(0, 'w'), 0);
  c.ins(OP_STORE_UAV_TYPED, { uavd(62, "xyzw"), imm4u(0, 0, 0, 0), imm4u(1, 1, 1, 1) });   /* atlas flag: 1 until CS3w finds an invalid block */
  c.endif_();
  c.ins(OP_MOV, { r(3, "zw"), imm4u(0, 0, 0, 0) });
  for (uint32_t k = 0; k * T * T < NA; k++) {                                       /* load the L1 tile: A[iy][ix] = (min, max) */
    c.ins(OP_IADD, { r(1, "x"), s(0, "zzzz"), imm1u(k * T * T) });
    bool guard = (k + 1) * T * T > NA;
    if (guard) { c.ins(OP_ULT, { r(1, "w"), s(1, "xxxx"), imm1u(NA) }); c.if_(s1(1, 'w'), 1); }
    c.ins(OP_UDIV, { r(1, "z"), r(1, "y"), s(1, "xxxx"), imm1u(AW) });               /* row (z), column (y) */
    c.ins(OP_IADD, { r(3, "xy"), s(0, "xyxx"), s(1, "yzyy") });
    c.ins(OP_IMIN, { r(3, "xy"), s(3, "xyxx"), cb(13, 1, "xyxx") });                  /* clamp: duplicates a real block, as v1 */
    c.ins(OP_LD, { r(2, "xy"), s(3, "xyzw"), res(127, "xyxx") });
    c.ins(OP_NOT, { r(2, "x"), s(2, "xxxx") });
    c.ins(OP_STORE_STRUCTURED, { gd(0, "xy"), s(1, "xxxx"), imm1u(0), s(2, "xyxx") });
    if (guard) c.endif_();
  }
  c.sync_gt();
  for (uint32_t k = 0; k * T * T < NH; k++) {                                       /* H[row][col] = combine A[row][col .. col+S-1] */
    c.ins(OP_IADD, { r(1, "x"), s(0, "zzzz"), imm1u(k * T * T) });
    bool guard = (k + 1) * T * T > NH;
    if (guard) { c.ins(OP_ULT, { r(1, "w"), s(1, "xxxx"), imm1u(NH) }); c.if_(s1(1, 'w'), 1); }
    c.ins(OP_USHR, { r(1, "z"), s(1, "xxxx"), imm1u(3) });
    c.ins(OP_AND, { r(1, "y"), s(1, "xxxx"), imm1u(7) });
    c.ins(OP_IMAD, { r(1, "w"), s(1, "zzzz"), imm1u(AW), s(1, "yyyy") });
    c.ins(OP_MOV, { r(2, "xy"), imm4u(0xffffffffu, 0, 0, 0) });
    for (uint32_t d = 0; d < S; d++) {
      c.ins(OP_IADD, { r(4, "x"), s(1, "wwww"), imm1u(d) });
      c.ins(OP_LD_STRUCTURED, { r(5, "xy"), s(4, "xxxx"), imm1u(0), gs(0, "xyxx") });
      c.ins(OP_UMIN, { r(2, "x"), s(2, "xxxx"), s(5, "xxxx") });
      c.ins(OP_UMAX, { r(2, "y"), s(2, "yyyy"), s(5, "yyyy") });
    }
    c.ins(OP_STORE_STRUCTURED, { gd(1, "xy"), s(1, "xxxx"), imm1u(0), s(2, "xyxx") });
    if (guard) c.endif_();
  }
  c.sync_gt();
  c.ins(OP_MOV, { r(2, "xy"), imm4u(0xffffffffu, 0, 0, 0) });                     /* combine H[ty .. ty+S-1][tx] */
  for (uint32_t d = 0; d < S; d++) {
    c.ins(OP_IADD, { r(4, "y"), tig("yyyy"), imm1u(d) });
    c.ins(OP_IMAD, { r(4, "x"), s(4, "yyyy"), imm1u(T), tig("xxxx") });
    c.ins(OP_LD_STRUCTURED, { r(5, "xy"), s(4, "xxxx"), imm1u(0), gs(1, "xyxx") });
    c.ins(OP_UMIN, { r(2, "x"), s(2, "xxxx"), s(5, "xxxx") });
    c.ins(OP_UMAX, { r(2, "y"), s(2, "yyyy"), s(5, "yyyy") });
  }
  c.ins(OP_ULT, { r(1, "x"), imm4u(0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u), s(2, "yyyy") });
  c.ins(OP_MOVC, { r(2, "xy"), s(1, "xxxx"), imm4u(0xffffffffu, 0xffffffffu, 0, 0), s(2, "xyxx") });
  c.ins(OP_NOT, { r(2, "x"), s(2, "xxxx") });
  c.ins(OP_MOV, { r(2, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_IADD, { r(0, "xy"), s(0, "xyxx"), tig("xyxx") });                       /* destination L2 texel */
  c.ins(OP_ILT, { r(1, "xy"), cb(13, 0, "zwzz"), s(0, "xyxx") });
  c.ins(OP_OR, { r(1, "x"), s(1, "xxxx"), s(1, "yyyy") });
  c.if_(s1(1, 'x'), 0);
  c.ins(OP_STORE_UAV_TYPED, { uavd(63, "xyzw"), s(0, "xyyy"), s(2, "xyzw") });
  c.endif_();
  c.ret();
  return cs_container(c);
}
static inline std::vector<uint8_t> build_cs_nanfree_wide() {
  Code c;
  c.dcl_cb(13, 2); c.dcl_tex2d(127, RET_UINT); c.dcl_uav_buf(62, RET_UINT); c.dcl_tid("xy"); c.dcl_temps(3); c.dcl_tg(16, 16, 1);
  c.ins(OP_IGE, { r(0, "zw"), tid("xxxy"), cb(13, 0, "xxxy") });
  c.ins(OP_OR, { r(0, "z"), s(0, "zzzz"), s(0, "wwww") });
  c.retc(s1(0, 'z'), 1);
  c.ins(OP_IMAD, { r(1, "xy"), tid("xyxx"), cb(13, 0, "zzzz"), imm4u(0, 0, 0, 0) });
  c.ins(OP_IMIN, { r(1, "xy"), s(1, "xyxx"), cb(13, 1, "xyxx") });
  c.ins(OP_MOV, { r(1, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_LD, { r(2, "y"), s(1, "xyzw"), res(127, "yyyy") });
  c.ins(OP_ULT, { r(2, "z"), imm4u(0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u), s(2, "yyyy") });
  c.if_(s1(2, 'z'), 1);
  c.ins(OP_ATOMIC_AND, { uavd(62, "xyzw"), imm1u(0), imm1u(0) });
  c.endif_();
  c.ret();
  return cs_container(c);
}

/* CSC (verification mode 4): counts the texels that differ between two images of the same size (t127, t125), u61[1]++.
   cb13[0] = (width - 1, height - 1, -, -) */
static inline std::vector<uint8_t> build_cs_compare() {
  Code c;
  c.dcl_cb(13, 1);
  c.dcl_tex2d(127, RET_FLOAT);
  c.dcl_tex2d(125, RET_FLOAT);
  c.dcl_uav_buf(61, RET_UINT);
  c.dcl_tid("xy");
  c.dcl_temps(4);
  c.dcl_tg(8, 8, 1);
  c.ins(OP_MOV, { r(0, "xy"), tid("xyxx") });
  c.ins(OP_ILT, { r(0, "zw"), cb(13, 0, "zzxy"), s(0, "zzxy") });
  c.ins(OP_OR, { r(0, "z"), s(0, "zzzz"), s(0, "wwww") });
  c.retc(s1(0, 'z'), 1);
  c.ins(OP_MOV, { r(0, "zw"), imm4u(0, 0, 0, 0) });
  c.ins(OP_LD, { r(1, "xyzw"), s(0, "xyzw"), res(127, "xyzw") });
  c.ins(OP_LD, { r(2, "xyzw"), s(0, "xyzw"), res(125, "xyzw") });
  c.ins(OP_INE, { r(3, "xyzw"), s(1, "xyzw"), s(2, "xyzw") });              /* bit patterns */
  c.ins(OP_OR, { r(3, "xy"), s(3, "xyxx"), s(3, "zwzz") });
  c.ins(OP_OR, { r(3, "x"), s(3, "xxxx"), s(3, "yyyy") });
  c.if_(s1(3, 'x'), 1);
  c.ins(OP_ATOMIC_IADD, { uavd(61, "xyzw"), imm1u(1), imm1u(1) });
  c.endif_();
  c.ret();
  return cs_container(c);
}

/* ---------------- the pixel shader patch ----------------
   Allowlist: the shaders whose code after the search loop was checked by hand (README): on the paths taken when the
   count is 0 or >= 256 nothing the loop writes except r0.y (the count) is read before it is overwritten. */
struct Target { uint64_t key; const char *name; };
static const Target kTargets[] = {
  { 0xc29c9e4e876e162bull, "c29c9e4e (cascade shadow projection)" },
  { 0xd108de4649b4e7c0ull, "d108de46 (per-object shadow projection)" },
  /* s1b: per-object shadow from a point light (radial falloff, no cone). From the `if` on its code is d108's with every
     cb0 index >= 7 lowered by one; proofs: tools/livecheck.py (post-loop liveness), tools/eoab.cpp (offline A/B) */
  { 0x59c8d61ba14cdddcull, "59c8d61b (per-object point-light shadow projection)" },
};
/* s1b: the blocker-search loop of all three, hashed with the constant-buffer indices masked (loop_hash); patch_ps is
   called with it at run time, so a shader whose loop differs in anything else is not patched */
static const uint64_t kLoopHash = 0xd3ee128b5517cf3aull;
enum Mode { M_EO = 1, M_STATS = 2, M_CHECK = 3 };   /* M_STATS: early-out + per-pixel counters; M_CHECK: never skips, counts mismatches */
struct PatchResult { bool ok = false; bool candidate = false; std::string why; std::vector<uint8_t> blob; uint32_t loop_at = 0; };

/* expected structure around the blocker-search loop, as decoded from the two allowlisted shaders:
     mul r2.y, <cb dims>.z, l(-7.0) ; mov r2.w, l(0) ; mov r0.xy, l(0,0,0,0) ; mov r1.x, l(-7) ; loop
     loop body: 70 instructions, 8 gather4 of t1 with sampler s0, compare against r3.w, accumulate into r0.xy, iadd r1.x, 2
   The loop's own hash with the constant-buffer register index masked is checked (kLoopHash); the dims operand used by
   the patch is copied from the loop (first mul with a cb operand). */
struct Shex { std::vector<uint32_t> w; };
static inline bool find_insn(const std::vector<uint32_t> &w, std::vector<size_t> &starts) {
  if (w.size() < 2 || w[1] != w.size()) return false;
  for (size_t i = 2; i < w.size();) { uint32_t l = ins_len(&w[i], w.size() - i); if (!l) return false; starts.push_back(i); i += l; }
  return true; }
/* masked hash of a token range: every operand token of type CB has its two index dwords hashed as 0 */
static inline uint64_t loop_hash(const std::vector<uint32_t> &w, size_t a, size_t b) {
  std::vector<uint32_t> v(w.begin() + a, w.begin() + b);
  /* walk instructions, operands: we mask by pattern: an operand token with type CB, 2 index dims, imm32 indices is followed by 2 index dwords */
  size_t i = 0;
  while (i < v.size()) { uint32_t l = ins_len(&v[i], v.size() - i); if (!l) break;
    for (size_t k = i + 1; k + 2 < i + l; k++) { uint32_t t = v[k]; if (((t >> 12) & 0xffu) == RT_CB && ((t >> 20) & 3u) == 2u && ((t >> 22) & 0x1ffu) == 0u && (t & 3u) == 2u) { v[k + 1] = 0; v[k + 2] = 0; k += 2; } }
    i += l; }
  return fnv64(v.data(), v.size() * 4); }

static inline PatchResult patch_ps(const void *code, size_t len, int mode, Geo geo, uint64_t expect_loop_hash = 0, bool allowlist_only = true, bool broken = false) {
  PatchResult pr; const uint8_t *b = (const uint8_t *)code;
  uint64_t key = dxbc_key(code, len); bool listed = false;
  for (auto &t : kTargets) listed |= t.key == key;
  std::vector<Chunk> ch; if (!read_container(b, len, ch)) { pr.why = "container"; return pr; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { pr.why = "no program"; return pr; }
  std::vector<uint32_t> w(ch[si].data.size() / 4); memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
  if ((w[0] >> 16) != 0 || ((w[0] >> 4) & 15) != 5) { pr.why = "not ps_5_x"; return pr; }
  std::vector<size_t> st; if (!find_insn(w, st)) { pr.why = "parse"; return pr; }
  /* candidate loop: preceded by the 4-instruction setup, containing 8 gather4 */
  auto op = [&] (size_t k) { return w[st[k]] & 0x7ffu; };
  size_t L = 0, E = 0; int ntemps_at = -1, last_res = -1; uint32_t ntemps = 0;
  for (size_t k = 0; k < st.size(); k++) {
    if (op(k) == OP_DCL_TEMPS) { ntemps_at = (int)k; ntemps = w[st[k] + 1]; }
    if (op(k) == OP_DCL_RESOURCE) last_res = (int)k;
    if (op(k) == OP_LOOP && !L && k >= 4) {
      size_t depth = 0, e = 0, ng = 0;
      for (size_t j = k; j < st.size(); j++) { if (op(j) == OP_LOOP) depth++; if (op(j) == OP_ENDLOOP && !--depth) { e = j; break; } if (op(j) == OP_GATHER4) ng++; }
      if (e && ng == 8) { L = k; E = e; }
    }
  }
  if (!L) { pr.why = "no 8-gather loop"; return pr; }
  pr.candidate = true;
  if (ntemps_at < 0 || last_res < 0) { pr.why = "no dcl_temps / dcl_resource"; return pr; }
  /* setup: mul r2.y, cb.z, l(-7.0) ; mov r2.w, l(0) ; mov r0.xy, l(0,0,0,0) ; mov r1.x, l(-7) */
  { const uint32_t *p = &w[st[L - 3]];   /* mov r2.w, l(0) */
    bool ok = (p[0] & 0x7ff) == OP_MOV && p[1] == dst(RT_TEMP, 2, "w").t[0] && p[2] == 2 && p[3] == imm1u(0).t[0] && p[4] == 0;
    p = &w[st[L - 2]];                    /* mov r0.xy, l(0,0,0,0) */
    ok = ok && (p[0] & 0x7ff) == OP_MOV && p[1] == dst(RT_TEMP, 0, "xy").t[0] && p[2] == 0 && p[3] == imm4u(0, 0, 0, 0).t[0] && !p[4] && !p[5] && !p[6] && !p[7];
    p = &w[st[L - 1]];                    /* mov r1.x, l(-7) */
    ok = ok && (p[0] & 0x7ff) == OP_MOV && p[1] == dst(RT_TEMP, 1, "x").t[0] && p[2] == 1 && p[3] == imm1u(0).t[0] && p[4] == (uint32_t)-7;
    p = &w[st[L - 4]];                    /* mul r2.y, cbX[n].z, l(-7.0) */
    ok = ok && (p[0] & 0x7ff) == OP_MUL && p[1] == dst(RT_TEMP, 2, "y").t[0] && p[2] == 2 && ((p[3] >> 12) & 0xff) == RT_CB && p[6] == imm1f(-7.0f).t[0] && p[7] == f2u(-7.0f);
    if (!ok) { pr.why = "setup before the loop differs"; return pr; } }
  uint64_t lh = loop_hash(w, st[L], st[E] + ins_len(&w[st[E]], w.size() - st[E]));
  if (expect_loop_hash && lh != expect_loop_hash) { char t[64]; snprintf(t, sizeof t, "loop hash %016llx differs", (unsigned long long)lh); pr.why = t; return pr; }
  if (allowlist_only && !listed) { char t[96]; snprintf(t, sizeof t, "same search loop but not in the allowlist (key %016llx)", (unsigned long long)key); pr.why = t; return pr; }
  /* dims operand: the cb operand of the mul at L-4 (cbX[n], 4 dwords: token, slot, reg), re-swizzled */
  uint32_t cbslot = w[st[L - 4] + 4], cbreg = w[st[L - 4] + 5];
  const uint32_t T = ntemps, A = T, B = T + 1, C = T + 2, D = T + 3, X = T + 4;   /* new temps */
  Code pre;
  /* atlas size (texels) */
  pre.ins(OP_RESINFO, { r(A, "xy"), imm1u(0), res(1, "xyzw") }, 2u << 11);  /* resinfo_uint */
  pre.ins(OP_UTOF, { r(A, "zw"), s(A, "xxxy") });
  /* window of the gathers in texel space: u = r1.y + {-7..7} dz, v = r1.z + {-7..7} dw; footprint floor(p*size - 0.5) .. +1;
     plus one texel of margin on each side for the rounding of the coordinates (the gathers sit exactly on texel centres) */
  pre.ins(OP_MAD, { r(B, "xy"), cb(cbslot, cbreg, "zwzz"), imm4f(-7.f, -7.f, 0.f, 0.f), s(1, "yzyy") });
  pre.ins(OP_MAD, { r(B, "zw"), cb(cbslot, cbreg, "zzzw"), imm4f(0.f, 0.f, 7.f, 7.f), s(1, "yyyz") });
  pre.ins(OP_MAD, { r(B, "xyzw"), s(B, "xyzw"), s(A, "zwzw"), imm4f(-.5f, -.5f, -.5f, -.5f) });
  pre.ins(OP_ROUND_NI, { r(B, "xyzw"), s(B, "xyzw") });
  pre.ins(OP_ADD, { r(B, "xyzw"), s(B, "xyzw"), imm4f(-1.f, -1.f, 2.f, 2.f) });
  /* s0 (g_SamplePoint) wraps (REPEAT on U and V, checked on the CPU before the twin is bound): the texels fetched for the
     integer footprint index i are imod(i, size). Shift the window by k = floor(lo / size) atlas sizes and require it to lie
     in [0, size) after the shift, i.e. not to straddle the seam; every step is exact on these integer values (or makes the
     test fail). Windows more than 64 atlas sizes away and NaN coordinates (which fail every ordered compare) are rejected. */
  pre.ins(OP_DIV, { r(X, "xy"), s(B, "xyxx"), s(A, "zwzz") });
  pre.ins(OP_ROUND_NI, { r(X, "xy"), s(X, "xyxx") });
  pre.ins(OP_LT, { r(X, "zw"), s(X, "xxxy", false, true), imm4f(64.f, 64.f, 64.f, 64.f) });   /* |k| < 64 */
  pre.ins(OP_MAD, { r(B, "xyzw"), s(X, "xyxy", true), s(A, "zwzw"), s(B, "xyzw") });
  pre.ins(OP_GE, { r(C, "xy"), s(B, "xyxx"), imm4f(0.f, 0.f, 0.f, 0.f) });
  pre.ins(OP_LT, { r(C, "zw"), s(B, "zzzw"), s(A, "zzzw") });
  pre.ins(OP_AND, { r(C, "xy"), s(C, "xyxx"), s(C, "zwzz") });
  pre.ins(OP_AND, { r(C, "xy"), s(C, "xyxx"), s(X, "zwzz") });
  pre.ins(OP_AND, { r(C, "x"), s(C, "xxxx"), s(C, "yyyy") });
  if (mode != M_EO) {                                                       /* stats: inside the atlas; off it with NaN coordinates */
    pre.ins(OP_MOV, { r(D, "y"), s(C, "xxxx") });
    pre.ins(OP_NE, { r(D, "w"), s(B, "xxxx"), s(B, "xxxx") });
    pre.ins(OP_MOVC, { r(D, "w"), s(D, "wwww"), imm4u(25, 25, 25, 25), imm4u(26, 26, 26, 26) });
    pre.ins(OP_MOVC, { r(D, "w"), s(C, "xxxx"), imm4u(27, 27, 27, 27), s(D, "wwww") });
    pre.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), s(D, "wwww"), imm1u(1) });
  }
  pre.ins(OP_MOVC, { r(B, "xyzw"), s(C, "xxxx"), s(B, "xyzw"), imm4f(0.f, 0.f, 0.f, 0.f) });
  pre.ins(OP_FTOI, { r(B, "xyzw"), s(B, "xyzw") });
  pre.ins(OP_ISHR, { r(B, "xyzw"), s(B, "xyzw"), imm4u(geo.shift, geo.shift, geo.shift, geo.shift) });   /* L1 block indices */
  pre.ins(OP_IADD, { r(C, "yz"), s(B, "xzwx"), s(B, "xxyx", true) });       /* hi - lo (blocks) */
  pre.ins(OP_IGE, { r(C, "yz"), imm4u(0, geo.span - 1, geo.span - 1, 0), s(C, "xyzx") });   /* one L2 texel covers it */
  pre.ins(OP_AND, { r(C, "x"), s(C, "xxxx"), s(C, "yyyy") });
  pre.ins(OP_AND, { r(C, "x"), s(C, "xxxx"), s(C, "zzzz") });
  if (mode != M_EO) pre.ins(OP_MOV, { r(D, "z"), s(C, "xxxx") });           /* stats: inside and covered */
  pre.ins(OP_MOV, { r(B, "zw"), imm4u(0, 0, 0, 0) });
  pre.ins(OP_LD, { r(A, "xy"), s(B, "xyzw"), res(127, "xyxx") });           /* (~min bits, max bits) of the window superset */
  pre.ins(OP_NOT, { r(A, "x"), s(A, "xxxx") });
  pre.ins(OP_ADD, { r(C, "w"), s(1, "wwww"), s(2, "wwww") });               /* the loop's compare value r3.w = r1.w + r2.w */
  pre.ins(OP_INE, { r(C, "y"), s(A, "xxxx"), imm4u(0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu) });
  pre.ins(OP_AND, { r(C, "x"), s(C, "xxxx"), s(C, "yyyy") });               /* inside, covered and valid */
  if (mode != M_EO) pre.ins(OP_MOV, { r(X, "w"), s(C, "xxxx") });
  pre.ins(OP_GE, { r(C, "y"), s(A, "xxxx"), s(C, "wwww") });                /* every texel >= recv: no blocker (count 0) */
  pre.ins(OP_GE, { r(C, "z"), s(A, "yyyy"), s(C, "wwww") });
  pre.ins(OP_NOT, { r(C, "z"), s(C, "zzzz") });                             /* no texel >= recv: all blockers (count 256) */
  pre.ins(OP_AND, { r(C, "yz"), s(C, "xxxx"), s(C, "xyzw") });
  /* receivers that need no window (the sky: the depth reconstruction gives recv = -inf and NaN coordinates):
     recv NaN  -> ge(t, NaN) is false for every texel: count 256, whatever the gathers return;
     recv -inf -> ge(t, -inf) is true for every non-NaN value: count 0 if the gathers cannot return a NaN, i.e. the atlas
                  holds no NaN (t126[0] == 1, written by the third compute pass over the whole map) and the sampler has
                  no NaN border colour (checked on the CPU before the twin is bound). */
  pre.ins(OP_NE, { r(X, "x"), s(C, "wwww"), s(C, "wwww") });
  pre.ins(OP_EQ, { r(X, "y"), s(C, "wwww"), imm4u(0xff800000u, 0xff800000u, 0xff800000u, 0xff800000u) });
  pre.ins(OP_LD, { r(X, "z"), imm4u(0, 0, 0, 0), res(126, "xxxx") });
  pre.ins(OP_IEQ, { r(X, "z"), s(X, "zzzz"), imm4u(1, 1, 1, 1) });
  pre.ins(OP_AND, { r(X, "y"), s(X, "yyyy"), s(X, "zzzz") });
  pre.ins(OP_OR, { r(C, "yz"), s(C, "xyzx"), s(X, "xyxx") });
  pre.ins(OP_OR, { r(C, "x"), s(C, "yyyy"), s(C, "zzzz") });
  if (broken) pre.ins(OP_MOV, { r(C, "xy"), imm4u(~0u, ~0u, 0, 0) });       /* negative control for the A/B test only: always "lit" */
  if (mode == M_STATS || mode == M_CHECK) {   /* u7: [0] lit, [1] shadowed, full search because: [2] undecided by the map,
                                                  [5] window leaves the atlas, [6] window too wide, [7] invalid block in it;
                                                  [21] of the lit ones: recv -inf, [22] of the shadowed ones: recv NaN */
    pre.ins(OP_MOVC, { r(D, "x"), s(X, "wwww"), imm4u(2, 2, 2, 2), imm4u(7, 7, 7, 7) });
    pre.ins(OP_MOVC, { r(D, "x"), s(D, "zzzz"), s(D, "xxxx"), imm4u(6, 6, 6, 6) });
    pre.ins(OP_MOVC, { r(D, "x"), s(D, "yyyy"), s(D, "xxxx"), imm4u(5, 5, 5, 5) });
    pre.ins(OP_MOVC, { r(D, "x"), s(C, "zzzz"), imm4u(1, 1, 1, 1), s(D, "xxxx") });
    pre.ins(OP_MOVC, { r(D, "x"), s(C, "yyyy"), imm4u(0, 0, 0, 0), s(D, "xxxx") });
    pre.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), s(D, "xxxx"), imm1u(1) });
    pre.ins(OP_MOVC, { r(D, "x"), s(X, "xxxx"), imm4u(22, 22, 22, 22), imm4u(23, 23, 23, 23) });
    pre.ins(OP_MOVC, { r(D, "x"), s(X, "yyyy"), imm4u(21, 21, 21, 21), s(D, "xxxx") });
    pre.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), s(D, "xxxx"), imm1u(1) });
  }
  Code post;
  if (mode == M_CHECK) {   /* the loop always ran: an early-out decision that disagrees with its count is a mismatch (u7[3]) */
    post.ins(OP_NE, { r(D, "x"), s(0, "yyyy"), imm1f(0.f) });
    post.ins(OP_NE, { r(D, "y"), s(0, "yyyy"), imm1f(256.f) });
    post.ins(OP_AND, { r(D, "xy"), s(D, "xyxx"), s(C, "yzyy") });
    post.ins(OP_OR, { r(D, "x"), s(D, "xxxx"), s(D, "yyyy") });
    post.if_(s1(D, 'x'), 1);
    post.ins(OP_ATOMIC_IADD, { uavd(7, "xyzw"), imm1u(3), imm1u(1) });
    /* the first mismatching pixel of the run records its inputs in u7[8..19] (diagnostics) */
    post.ins(OP_IMM_ATOMIC_IADD, { r(D, "z"), uavd(7, "xyzw"), imm1u(4), imm1u(1) });
    post.if_(s1(D, 'z'), 0);
    const struct { uint32_t reg; char c; } rec[12] = { { 1, 'y' }, { 1, 'z' }, { 1, 'w' }, { 2, 'w' }, { C, 'w' }, { A, 'x' }, { A, 'y' }, { A, 'z' }, { A, 'w' }, { B, 'x' }, { B, 'y' }, { 0, 'y' } };
    for (uint32_t i = 0; i < 12; i++) { char sw[5] = { rec[i].c, rec[i].c, rec[i].c, rec[i].c, 0 }; post.ins(OP_STORE_UAV_TYPED, { uavd(7, "xyzw"), imm1u(8 + i), s(rec[i].reg, sw) }); }
    post.endif_();
    post.endif_();
  } else {
    pre.if_(s1(C, 'x'), 0);
    post.else_();
    post.ins(OP_MOVC, { r(0, "y"), s(C, "yyyy"), imm1f(0.f), imm1f(256.f) });   /* exactly the count the loop would produce */
    post.endif_();
  }
  Code dcl; dcl.dcl_tex2d(127, RET_UINT); dcl.dcl_buf(126, RET_UINT); if (mode != M_EO) dcl.dcl_uav_buf(7, RET_UINT);
  /* assemble */
  std::vector<uint32_t> o; o.reserve(w.size() + pre.w.size() + post.w.size() + dcl.w.size());
  size_t endE = st[E] + ins_len(&w[st[E]], w.size() - st[E]);
  size_t endRes = st[last_res] + ins_len(&w[st[last_res]], w.size() - st[last_res]);
  for (size_t i = 0; i < w.size(); i++) {
    if (i == endRes) o.insert(o.end(), dcl.w.begin(), dcl.w.end());
    if (i == st[L]) o.insert(o.end(), pre.w.begin(), pre.w.end());
    if (i == endE) o.insert(o.end(), post.w.begin(), post.w.end());
    if (i == st[ntemps_at] + 1) { o.push_back(T + 5); continue; }
    o.push_back(w[i]);
  }
  if (endE == w.size()) o.insert(o.end(), post.w.begin(), post.w.end());
  o[1] = (uint32_t)o.size();
  ch[si].data.resize(o.size() * 4); memcpy(ch[si].data.data(), o.data(), o.size() * 4);
  /* drop chunks the converter does not read and that would no longer describe the code (statistics) */
  std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(c);
  pr.blob = write_container(keep); pr.ok = true; pr.loop_at = (uint32_t)L;
  return pr;
}

}  // namespace b1eo
