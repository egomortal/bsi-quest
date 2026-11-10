/* rhi_b1_aoblur.h -- "V4" HDAO blur rewrite (Fable note 02 S6 / 08 8.1): DXBC-level tools, no DXVK dependency.
   Header-only: compiled inside rhi_b1_dxvk.cpp (via rhi_b1_aoblur.inc) and by the offline tool (aoblur_fix/tools/abtest.cpp).

   The two HDAO bilateral blur compute shaders (UE3 SM5 HDAO, Ultra AO; bridge.c dxbc_key names)
       cs 1da468b6b32aff04 (blur 1, horizontal)   and   cs cfaa37dda9ade3f6 (blur 2, vertical)
   run 32x2 groups; every thread stages 5 records of 20 bytes in groupshared memory (160 per row: the AO texel pair
   sampled bilinearly at the pair boundary (.xyz), a weight, and the bilinearly sampled linear depth), then writes 4
   outputs, each a 17-tap kernel: 16 taps over every second record (skipping the record at the kernel centre) plus the
   centre texel (point-sampled AO and depth):
       w_t   = (tapDepth < centreDepth ? storedWeight : 0.125) * k_t          (storedWeight is the constant 0.125)
       wsum  = 0.049868 + sum w_t ;  sum = ao_centre * 0.049868 + sum w_t * ao_t ;  out.x = sum / wsum
       out.yzw = 8-bit split of the centre pixel's linear depth
   The "bilateral" select has both arms equal to 0.125, so the tap depths, the stored weight and ao.y/.z never matter.

   The rewrite keeps every arithmetic instruction that contributes to an output, verbatim and in the original order
   (same operand order, same immediates, same float -> int and coordinate maths), and changes only where values come from:
     - groupshared records hold the AO .x alone (4 instead of 20 bytes: 1,280 B per group instead of 6,400), filled by
       the same 5 sample instructions per thread at the same coordinates; the depth samples, their linearisation (a
       divide each) and the other 4 stores per record are gone;
     - each tap's select `movc w, lt(tapDepth, centreDepth), storedWeight, 0.125` becomes `mov w, cb13[t]`, where
       cb13[t] = 0.125 (bit pattern 0x3e000000), the value the select returns on both arms. The weight stays a run-time
       value the driver cannot see through (a compile-time 0.125 would let the compiler fold the whole weight chain and
       turn the final divide into a multiply by a CPU-rounded reciprocal, which need not equal the GPU's rcp);
     - the tap-depth loads, compares and record-address arithmetic die and are removed (liveness, per component).
   By default (share = false) every select gets its own cb value (64), so the twin's data-flow graph is the original's
   exactly: only the leaves (groupshared loads, weights) differ. share = true gives one value per tap position (16), shared
   by the thread's 4 outputs, whose identical weight chains then fold into one (BSI_B1_AOBLUR_SHARE=1; ir3 computes the
   uniform weight chain in the shader preamble either way, so the main body costs the same).

   Everything is checked on the original's tokens before a twin is made: the LDS accesses are classified by simulating the
   integer address arithmetic for all 64 threads of two groups (record index = 5*tid + k on stores / 4*tid + m on loads,
   field 0/4/8/12/16, row = ty), the weight field's every store must be the immediate 0.125, every select must have that
   shape (then-arm = a weight load of record m, else-arm = immediate 0.125, condition = a compare that reads a depth load
   of the same record), select k of a thread must read record (k % 4) + 2 * (k / 4) (+1 from tap position 8 on), and
   after the rewrite every AO load must remain and no load of another field. Any mismatch: no twin (the game's shader runs).

   BSI_B1_AOBLUR_BREAK=1 (negative control of the A/B mode only) makes a deliberately wrong twin: the first groupshared AO
   load of record 16 reads record 17 (one tap of one output off by one texel pair). */
#pragma once
#include "rhi_b1_shadoweo.h"
#include <array>
#include <optional>

namespace b1ab {
using namespace b1eo;

enum : uint32_t { OP_DP2 = 15, OP_DP3 = 16, OP_DP4 = 17, OP_FTOU = 28, OP_IMAX = 36, OP_IMUL = 38, OP_INEG = 40, OP_LABEL = 44, OP_LD_MS = 46,
  OP_ENDLOOP_ = 22, OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71, OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74, OP_USHR = 85, OP_XOR = 87,
  OP_UMUL = 81, OP_DCL_GLOBAL_FLAGS = 106, OP_DCL_TGSM_RAW = 159, OP_STORE_UAV = 164, OP_LD_RAW = 165, OP_STORE_RAW = 166, OP_SYNC = 190,
  RT_NULL = 13, RT_TGSM = 31, RT_TGID = 33, RT_TID_IN_GROUP = 34 };

struct Opnd {
  size_t at = 0, ntok = 0; uint32_t type = 0, ncm = 0, sel = 0, bits = 0, dim = 0; uint32_t idx[3] = { 0, 0, 0 }; bool rel = false;
  uint32_t imm[4] = { 0, 0, 0, 0 }; bool neg = false, abs = false;
  bool temp() const { return type == RT_TEMP && dim == 1 && !rel; }
  /* source components read for destination mask dm (per-component op) */
  uint32_t reads(uint32_t dm) const {
    if (ncm == 1) return 1u;
    if (ncm != 2) return 0u;
    if (sel == 2) return 1u << (bits & 3u);
    if (sel == 0) return bits & 15u;
    uint32_t r = 0; for (uint32_t c = 0; c < 4; c++) if (dm >> c & 1u) r |= 1u << ((bits >> (2 * c)) & 3u); return r; }
  uint32_t all4() const { return reads(15u); }
  uint32_t comp(uint32_t c) const { return ncm == 2 ? (sel == 1 ? (bits >> (2 * c)) & 3u : sel == 2 ? bits & 3u : c) : 0u; }
  uint32_t mask() const { return ncm == 2 && sel == 0 ? bits & 15u : ncm == 1 ? 1u : 0u; }   /* destination mask */
};
struct Ins { size_t at = 0; uint32_t len = 0, op = 0; std::vector<Opnd> o; uint32_t ndst = 0; bool decl = false; };

static inline bool parse_opnd(const std::vector<uint32_t> &w, size_t &p, size_t end, Opnd &o) {
  if (p >= end) return false;
  o.at = p; uint32_t t = w[p++];
  o.ncm = t & 3u; o.sel = (t >> 2) & 3u; o.bits = (t >> 4) & 0xffu; o.type = (t >> 12) & 0xffu; o.dim = (t >> 20) & 3u;
  bool ext = t >> 31;
  while (ext) { if (p >= end) return false; uint32_t e = w[p++]; if ((e & 0x3fu) == 1u) { uint32_t m = (e >> 6) & 0xffu; o.neg = m & 1u; o.abs = m & 2u; } ext = e >> 31; }
  if (o.type == RT_IMM32) { uint32_t n = o.ncm == 1 ? 1u : 4u; if (p + n > end) return false; for (uint32_t i = 0; i < n; i++) o.imm[i] = w[p++]; o.ntok = p - o.at; return true; }
  for (uint32_t i = 0; i < o.dim; i++) {
    uint32_t rep = (t >> (22 + 3 * i)) & 7u;
    if (rep == 0) { if (p >= end) return false; o.idx[i] = w[p++]; }
    else if (rep == 1) { if (p + 2 > end) return false; o.idx[i] = w[p]; p += 2; }
    else if (rep == 2 || rep == 3) { o.rel = true; if (rep == 3) { if (p >= end) return false; o.idx[i] = w[p++]; } Opnd sub; if (!parse_opnd(w, p, end, sub)) return false; }
    else return false;
  }
  o.ntok = p - o.at; return true;
}
static inline uint32_t num_dst(uint32_t op) {
  switch (op) {
    case OP_IMUL: case OP_UMUL: case OP_UDIV: return 2;
    case OP_IF: case OP_ELSE: case OP_ENDIF: case OP_LOOP: case OP_ENDLOOP: case OP_BREAK: case OP_BREAKC: case OP_RET: case OP_RETC: case OP_SYNC: case 58 /* nop */: case OP_LABEL: return 0;
    case OP_STORE_RAW: case OP_STORE_UAV: return 1;   /* the memory operand */
    default: return 1;
  }
}
static inline bool is_decl(uint32_t op) { return (op >= 88 && op <= 107) || (op >= 143 && op <= 163); }
/* the program (version, length, instructions) -> instruction list */
static inline bool decode(const std::vector<uint32_t> &w, std::vector<Ins> &out, std::string &why) {
  if (w.size() < 2 || w[1] != w.size()) { why = "length"; return false; }
  for (size_t i = 2; i < w.size();) {
    uint32_t l = ins_len(&w[i], w.size() - i); if (!l) { why = "malformed instruction"; return false; }
    Ins in; in.at = i; in.len = l; in.op = w[i] & 0x7ffu; in.decl = is_decl(in.op) || in.op == OP_CUSTOMDATA;
    if (!in.decl) {
      size_t p = i + 1, end = i + l; bool ext = w[i] >> 31;
      while (ext && p < end) { ext = w[p] >> 31; p++; }   /* extended opcode tokens (resource dim / return type / offsets) */
      while (p < end) { Opnd o; if (!parse_opnd(w, p, end, o)) { why = "operand"; return false; } in.o.push_back(o); }
      in.ndst = std::min<uint32_t>(num_dst(in.op), (uint32_t)in.o.size());
    }
    out.push_back(std::move(in)); i += l;
  }
  return true;
}

/* ---------------- integer address simulation (one thread) ---------------- */
struct Sim {
  std::vector<std::array<std::optional<uint32_t>, 4>> r; uint32_t tid[2], gid[2];
  std::optional<uint32_t> get(const Opnd &o, uint32_t c) const {
    uint32_t k = o.comp(c);
    std::optional<uint32_t> v;
    if (o.type == RT_IMM32) v = o.imm[o.ncm == 1 ? 0 : k];
    else if (o.temp() && o.idx[0] < r.size()) v = r[o.idx[0]][k];
    else if (o.type == RT_TID_IN_GROUP) v = k < 2 ? std::optional<uint32_t>(tid[k]) : std::optional<uint32_t>(0u);
    else if (o.type == RT_TGID) v = k < 2 ? std::optional<uint32_t>(gid[k]) : std::optional<uint32_t>(0u);
    if (v && o.abs) return std::nullopt;
    if (v && o.neg) v = 0u - *v;
    return v; }
  void step(const Ins &in) {
    if (in.decl || in.ndst == 0) return;
    for (uint32_t d = 0; d < in.ndst; d++) {
      const Opnd &D = in.o[d]; if (!D.temp() || D.idx[0] >= r.size()) continue;
      uint32_t m = D.mask();
      for (uint32_t c = 0; c < 4; c++) if (m >> c & 1u) {
        std::optional<uint32_t> v;
        auto a = [&] (size_t i) { return i < in.o.size() ? get(in.o[i], c) : std::nullopt; };
        size_t s = in.ndst;   /* first source */
        switch (in.op) {
          case OP_MOV: v = a(s); break;
          case OP_IADD: if (a(s) && a(s + 1)) v = *a(s) + *a(s + 1); break;
          case OP_IMUL: if (d == 1 && a(s) && a(s + 1)) v = (uint32_t)((int32_t)*a(s) * (int64_t)(int32_t)*a(s + 1)); break;
          case OP_IMAD: if (a(s) && a(s + 1) && a(s + 2)) v = *a(s) * *a(s + 1) + *a(s + 2); break;
          case OP_ISHL: if (a(s) && a(s + 1)) v = *a(s) << (*a(s + 1) & 31u); break;
          case OP_USHR: if (a(s) && a(s + 1)) v = *a(s) >> (*a(s + 1) & 31u); break;
          case OP_ISHR: if (a(s) && a(s + 1)) v = (uint32_t)((int32_t)*a(s) >> (*a(s + 1) & 31u)); break;
          case OP_AND: if (a(s) && a(s + 1)) v = *a(s) & *a(s + 1); break;
          case OP_OR: if (a(s) && a(s + 1)) v = *a(s) | *a(s + 1); break;
          case OP_INEG: if (a(s)) v = 0u - *a(s); break;
          default: break;
        }
        r[D.idx[0]][c] = v;
      }
    }
  }
};

/* ---------------- per-component liveness and dead-code removal (straight line + if/else/endif) ---------------- */
static inline bool side_effect(const Ins &in) {
  switch (in.op) {
    case OP_STORE_RAW: case OP_STORE_UAV: case OP_SYNC: case OP_RET: case OP_RETC: case OP_IF: case OP_ELSE: case OP_ENDIF: return true;
    default: break;
  }
  if (in.decl) return true;
  for (uint32_t d = 0; d < in.ndst; d++) if (!in.o[d].temp() && in.o[d].type != RT_NULL) return true;   /* outputs, memory */
  switch (in.op) {   /* pure ops this pass understands */
    case OP_ADD: case OP_AND: case OP_DIV: case OP_DP2: case OP_DP3: case OP_DP4: case OP_EQ: case 25: case 26: case OP_FTOI: case OP_FTOU: case OP_GE: case OP_IADD:
    case OP_IEQ: case OP_IGE: case OP_ILT: case OP_IMAD: case OP_IMAX: case OP_IMIN: case OP_IMUL: case OP_INE: case OP_INEG: case OP_ISHL: case OP_ISHR: case OP_ITOF:
    case OP_LD: case 47: case OP_LT: case OP_MAD: case OP_MIN: case 52: case OP_MOV: case OP_MOVC: case OP_MUL: case OP_NE: case OP_NOT: case OP_OR:
    case 64: case OP_ROUND_NI: case 66: case 67: case 68: case OP_SAMPLE_L: case 75: case OP_ULT: case OP_UGE: case OP_UMAX: case OP_UMIN: case OP_USHR: case OP_UTOF: case OP_XOR:
    case OP_LD_RAW:
      return false;
    default: return true;
  }
}
/* components of temp register `reg` that source operand i of `in` reads */
static inline uint32_t src_reads(const Ins &in, size_t i) {
  const Opnd &s = in.o[i];
  uint32_t dm = 0; for (uint32_t d = 0; d < in.ndst; d++) dm |= in.o[d].mask();
  switch (in.op) {
    case OP_DP2: return s.reads(3u); case OP_DP3: return s.reads(7u); case OP_DP4: return s.reads(15u);
    case OP_SAMPLE_L: case OP_LD: case OP_LD_RAW: case OP_STORE_UAV: case OP_STORE_RAW: case OP_IF: case OP_RETC:
      return s.all4();
    default: break;
  }
  if (side_effect(in) && in.op != OP_STORE_RAW) return s.all4();
  return s.reads(dm ? dm : 15u);
}
/* removes dead pure instructions (in place); returns the number removed */
static inline size_t dce(std::vector<Ins> &v, uint32_t ntemps) {
  size_t removed = 0;
  for (;;) {
    std::vector<uint32_t> live(ntemps, 0u);
    std::vector<std::vector<uint32_t>> stack;   /* saved live-after-endif sets */
    std::vector<bool> dead(v.size(), false); bool any = false;
    for (size_t k = v.size(); k-- > 0;) {
      Ins &in = v[k];
      if (in.decl) continue;
      if (in.op == OP_RET) { std::fill(live.begin(), live.end(), 0u); continue; }
      if (in.op == OP_ENDIF) { stack.push_back(live); continue; }
      if (in.op == OP_ELSE) { if (stack.empty()) return removed; std::vector<uint32_t> after = stack.back(); stack.back() = live; live = after; continue; }   /* else-body start: live of the then-body begins from after-endif */
      if (in.op == OP_IF) {
        if (stack.empty()) return removed;
        for (uint32_t r = 0; r < ntemps; r++) live[r] |= stack.back()[r];   /* the body may be skipped */
        stack.pop_back();
        for (size_t i = 0; i < in.o.size(); i++) if (in.o[i].temp() && in.o[i].idx[0] < ntemps) live[in.o[i].idx[0]] |= src_reads(in, i);
        continue; }
      bool se = side_effect(in);
      if (!se) {
        bool needed = false;
        for (uint32_t d = 0; d < in.ndst; d++) if (in.o[d].temp() && in.o[d].idx[0] < ntemps && (live[in.o[d].idx[0]] & in.o[d].mask())) needed = true;
        if (!needed) { dead[k] = true; any = true; continue; }
      }
      for (uint32_t d = 0; d < in.ndst; d++) if (in.o[d].temp() && in.o[d].idx[0] < ntemps && !se) live[in.o[d].idx[0]] &= ~in.o[d].mask();
      for (size_t i = in.ndst; i < in.o.size(); i++) if (in.o[i].temp() && in.o[i].idx[0] < ntemps) live[in.o[i].idx[0]] |= src_reads(in, i);
      if (in.op == OP_STORE_RAW || in.op == OP_STORE_UAV) { /* memory operand 0 is not a temp */ }
    }
    if (!any) return removed;
    std::vector<Ins> keep; for (size_t k = 0; k < v.size(); k++) if (!dead[k]) keep.push_back(std::move(v[k])); else removed++;
    v.swap(keep);
  }
}

/* ---------------- the rewrite ---------------- */
struct Target { uint64_t key; const char *name; };
static const Target kTargets[] = {
  { 0x1da468b6b32aff04ull, "1da468b6 (HDAO blur 1, horizontal)" },
  { 0xcfaa37dda9ade3f6ull, "cfaa37dd (HDAO blur 2, vertical)" },
};
struct Result {
  bool ok = false, candidate = false; std::string why; std::vector<uint8_t> blob;
  uint32_t nweights = 0;           /* cb13 floats the twin reads (all 0.125) */
  uint32_t ntaps = 0, nloads0 = 0, nstores0 = 0, removed = 0, n_in = 0, n_out = 0, lds_in = 0, lds_out = 0;
};
static constexpr uint32_t kRec = 20, kRow = 3200, kRecs = 160;   /* original record layout */

static inline Result rewrite(const void *code, size_t len, bool share = false, bool broken = false, bool allowlist_only = true) {
  Result R; const uint8_t *b = (const uint8_t *)code;
  uint64_t key = dxbc_key(code, len); bool listed = false; for (auto &t : kTargets) listed |= t.key == key;
  std::vector<Chunk> ch; if (!read_container(b, len, ch)) { R.why = "container"; return R; }
  int si = -1; for (size_t i = 0; i < ch.size(); i++) if (ch[i].tag == fourcc("SHEX") || ch[i].tag == fourcc("SHDR")) si = (int)i;
  if (si < 0 || ch[si].data.size() < 8 || (ch[si].data.size() & 3)) { R.why = "no program"; return R; }
  std::vector<uint32_t> w(ch[si].data.size() / 4); memcpy(w.data(), ch[si].data.data(), ch[si].data.size());
  if ((w[0] >> 16) != 5 || ((w[0] >> 4) & 15) != 5) { R.why = "not cs_5_x"; return R; }
  std::vector<Ins> v; if (!decode(w, v, R.why)) return R;
  /* candidate: 32x2 groups, one 6,400-byte raw TGSM, one sync */
  uint32_t ntemps = 0; int tg = -1, tgsm = -1, sync = -1, nsync = 0, cbdecl = -1;
  for (size_t k = 0; k < v.size(); k++) {
    const uint32_t *p = &w[v[k].at];
    if (v[k].op == OP_DCL_TEMPS) ntemps = p[1];
    if (v[k].op == OP_DCL_THREAD_GROUP && p[1] == 32 && p[2] == 2 && p[3] == 1) tg = (int)k;
    if (v[k].op == OP_DCL_TGSM_RAW && v[k].len == 4 && p[3] == kRow * 2) tgsm = (int)k;
    if (v[k].op == OP_DCL_CB) cbdecl = (int)k;
    if (v[k].op == OP_SYNC) { sync = (int)k; nsync++; }
  }
  if (tg < 0 || tgsm < 0 || nsync != 1) return R;
  R.candidate = true; R.n_in = (uint32_t)v.size();
  if (allowlist_only && !listed) { char t[96]; snprintf(t, sizeof t, "same blur structure but not in the allowlist (key %016llx)", (unsigned long long)key); R.why = t; return R; }
  if (!ntemps || ntemps > 60 || cbdecl < 0) { R.why = "temps / cb declaration"; return R; }
  /* 1. classify every LDS access by simulating the address arithmetic of all 64 threads of two groups */
  struct Acc { size_t k; bool store; int field = -1; int rec = -1; };   /* rec relative to 5*tid (stores) or 4*tid (loads) */
  std::vector<Acc> acc;
  for (size_t k = 0; k < v.size(); k++) if (v[k].op == OP_LD_RAW || v[k].op == OP_STORE_RAW) acc.push_back({ k, v[k].op == OP_STORE_RAW });
  for (int g = 0; g < 2; g++) for (uint32_t ty = 0; ty < 2; ty++) for (uint32_t tx = 0; tx < 32; tx++) {
    Sim s; s.r.assign(ntemps, {}); s.tid[0] = tx; s.tid[1] = ty; s.gid[0] = g ? 3u : 0u; s.gid[1] = g ? 7u : 0u;
    size_t ai = 0;
    for (size_t k = 0; k < v.size(); k++) {
      if (ai < acc.size() && acc[ai].k == k) {
        const Ins &in = v[k]; const Opnd &A = in.o[acc[ai].store ? 1 : 1];   /* ld_raw dst, addr, g0 | store_raw g0, addr, value */
        auto a = s.get(A, 0);
        if (!a) { R.why = "LDS address not computable"; return R; }
        uint32_t row = *a / kRow, rem = *a % kRow, rec = rem / kRec, f = rem % kRec;
        int rel = (int)rec - (int)(acc[ai].store ? 5 * tx : 4 * tx);
        if (row != ty || (f & 3)) { R.why = "LDS address shape"; return R; }
        if (acc[ai].field < 0) { acc[ai].field = (int)f; acc[ai].rec = rel; }
        else if (acc[ai].field != (int)f || acc[ai].rec != rel) { R.why = "LDS access differs between threads"; return R; }
        ai++;
      }
      s.step(v[k]);
    }
  }
  /* stores: records 5*tid + 0..4 (every record of the row once per field), the weight field always the immediate 0.125 */
  uint32_t seen[5][5] = { };
  for (auto &a : acc) if (a.store) {
    if (a.rec < 0 || a.rec > 4 || a.field > 16) { R.why = "store record"; return R; }
    seen[a.rec][a.field / 4]++;
    const Opnd &val = v[a.k].o[2];
    if (a.field == 12 && !(val.type == RT_IMM32 && val.imm[0] == f2u(0.125f))) { R.why = "weight store is not the immediate 0.125"; return R; }
    if (v[a.k].o[0].mask() != 1u) { R.why = "store mask"; return R; }
  }
  for (auto &r : seen) for (auto n : r) if (n != 1) { R.why = "store coverage"; return R; }
  for (auto &a : acc) if (!a.store && (a.rec < 0 || a.rec > 35)) { R.why = "load record range"; return R; }
  for (auto &a : acc) if ((a.store && a.k > (size_t)sync) || (!a.store && a.k < (size_t)sync)) { R.why = "LDS access on the wrong side of the barrier"; return R; }
  /* 2. the selects: then-arm = weight load of record m, else-arm = immediate 0.125, condition = compare reading a depth load of record m */
  struct Tag { int field = -1, rec = -1; };
  std::vector<std::array<Tag, 4>> tag(ntemps);
  std::vector<std::array<int, 4>> cmpRec(ntemps);   /* component written by an lt whose first source is a depth load: record */
  for (auto &r : cmpRec) r.fill(-1);
  struct Sel { size_t k; int rec; };
  std::vector<Sel> sels; size_t ai = 0;
  for (size_t k = 0; k < v.size(); k++) {
    const Ins &in = v[k];
    bool isacc = ai < acc.size() && acc[ai].k == k; const Acc *A = isacc ? &acc[ai++] : nullptr;
    if (in.op == OP_MOVC && k > (size_t)sync) {
      const Opnd &d = in.o[0], &c = in.o[1], &t = in.o[2], &e = in.o[3];
      uint32_t m = d.mask();
      if (!d.temp() || __builtin_popcount(m) != 1) { R.why = "select shape"; return R; }
      uint32_t dc = __builtin_ctz(m);
      bool ok = c.temp() && t.temp() && e.type == RT_IMM32 && !e.neg && !e.abs && !t.neg && !t.abs;
      int crec = ok ? cmpRec[c.idx[0]][c.comp(dc)] : -1; Tag tt = ok ? tag[t.idx[0]][t.comp(dc)] : Tag();
      ok = ok && e.imm[e.ncm == 1 ? 0 : dc] == f2u(0.125f) && tt.field == 12 && crec == tt.rec && crec >= 0;
      if (!ok) { R.why = "a select is not the no-op bilateral weight"; return R; }
      sels.push_back({ k, tt.rec });
    }
    /* update tags / compare records for the written components */
    for (uint32_t di = 0; di < in.ndst; di++) {
      const Opnd &d = in.o[di]; if (!d.temp() || d.idx[0] >= ntemps) continue; uint32_t m = d.mask();
      for (uint32_t cc = 0; cc < 4; cc++) if (m >> cc & 1u) {
        Tag nt; int nr = -1;
        if (A && !A->store) { nt.field = A->field; nt.rec = A->rec; }
        if (in.op == OP_LT && in.o[1].temp()) { Tag s0t = tag[in.o[1].idx[0]][in.o[1].comp(cc)]; if (s0t.field == 16) nr = s0t.rec; }
        tag[d.idx[0]][cc] = nt; cmpRec[d.idx[0]][cc] = nr; }
    }
  }
  /* tap k of the thread's 4 outputs: output k % 4, tap position k / 4, record (k % 4) + 2 * (k / 4) */
  if (getenv("AB_DEBUG")) { fprintf(stderr, "sels %zu:", sels.size()); for (auto &q : sels) fprintf(stderr, " %d", q.rec); fprintf(stderr, "\n"); }
  bool regular = sels.size() == 64;   /* 16 positions; the kernel centre (record i + 16) is the separate point sample */
  for (size_t k = 0; regular && k < sels.size(); k++) { uint32_t t = (uint32_t)(k / 4); regular = sels[k].rec == (int)(k % 4 + 2 * t + (t >= 8 ? 1 : 0)); }
  if (!regular) { R.why = "taps are not the 4 x 16 pattern"; return R; }
  R.ntaps = (uint32_t)sels.size();
  /* 3. rewrite: new temps A (store base), B (load base), C (scratch address) */
  const uint32_t TA = ntemps, TB = ntemps + 1, TC = ntemps + 2;
  std::vector<std::vector<uint32_t>> tok(v.size());   /* replacement tokens per instruction (empty: original) */
  std::vector<bool> drop(v.size(), false);
  uint32_t nw = share ? 16u : 64u; R.nweights = nw;
  for (size_t k = 0; k < sels.size(); k++) {
    uint32_t slot = share ? (uint32_t)(k / 4) : (uint32_t)k;
    const Opnd &d = v[sels[k].k].o[0];
    Code c; char m[2] = { "xyzw"[__builtin_ctz(d.mask())], 0 }, sw[5];
    char cc = "xyzw"[slot & 3u]; sw[0] = sw[1] = sw[2] = sw[3] = cc; sw[4] = 0;
    c.ins(OP_MOV, { r(d.idx[0], m), cb(13, slot / 4, sw) });
    tok[sels[k].k] = c.w;
  }
  ai = 0;
  for (auto &a : acc) {
    const Ins &in = v[a.k];
    if (a.store) {
      if (a.field != 0) { drop[a.k] = true; continue; }
      Code c;   /* store_raw g0.x, TC.x, <value> with TC = TA + 4 * rec */
      c.ins(OP_IADD, { r(TC, "x"), s(TA, "xxxx"), imm1u(4u * (uint32_t)a.rec) });
      std::vector<uint32_t> st(w.begin() + in.at, w.begin() + in.at + in.len);
      const Opnd &ad = in.o[1]; size_t off = ad.at - in.at;
      std::vector<uint32_t> na = s1(TC, 'x').t;
      st.erase(st.begin() + off, st.begin() + off + ad.ntok); st.insert(st.begin() + off, na.begin(), na.end());
      st[0] = (st[0] & ~(0x7fu << 24)) | ((uint32_t)st.size() << 24);
      c.w.insert(c.w.end(), st.begin(), st.end());
      tok[a.k] = c.w; R.nstores0++;
    } else {
      if (a.field != 0) continue;   /* weight / depth / ao.yz loads: dead after the select rewrite (removed below) */
      int rec = a.rec;
      Code c;
      c.ins(OP_IADD, { r(TC, "x"), s(TB, "xxxx"), imm1u(4u * (uint32_t)rec) });
      std::vector<uint32_t> st(w.begin() + in.at, w.begin() + in.at + in.len);
      const Opnd &ad = in.o[1]; size_t off = ad.at - in.at;
      std::vector<uint32_t> na = s1(TC, 'x').t;
      st.erase(st.begin() + off, st.begin() + off + ad.ntok); st.insert(st.begin() + off, na.begin(), na.end());
      st[0] = (st[0] & ~(0x7fu << 24)) | ((uint32_t)st.size() << 24);
      c.w.insert(c.w.end(), st.begin(), st.end());
      tok[a.k] = c.w; R.nloads0++;
    }
  }
  if (broken) {   /* negative control: the centre-position tap (k / 4 == 8) of output 0 reads the next record */
    for (auto &a : acc) if (!a.store && a.field == 0 && a.rec == 16) { Code c;
      c.ins(OP_IADD, { r(TC, "x"), s(TB, "xxxx"), imm1u(4u * 17u) });
      const Ins &in = v[a.k]; std::vector<uint32_t> st(w.begin() + in.at, w.begin() + in.at + in.len);
      const Opnd &ad = in.o[1]; size_t off = ad.at - in.at; std::vector<uint32_t> na = s1(TC, 'x').t;
      st.erase(st.begin() + off, st.begin() + off + ad.ntok); st.insert(st.begin() + off, na.begin(), na.end());
      st[0] = (st[0] & ~(0x7fu << 24)) | ((uint32_t)st.size() << 24); c.w.insert(c.w.end(), st.begin(), st.end()); tok[a.k] = c.w; break; }
  }
  /* rebuild the instruction stream with the replacements, then decode it again and remove dead code */
  std::vector<uint32_t> o; o.push_back(w[0]); o.push_back(0);
  /* TA = (5 * tid.x + ty * 160) * 4 just before the first groupshared store, TB = (4 * tid.x + ty * 160) * 4 just before the
     first load: placed late on purpose, so the builtin loads keep their order of first use and dxbc-spirv numbers (and
     orders the operands of) every original value exactly as in the original */
  Code pre, preB;
  { /* imul null, TA.x, vThreadIDInGroup.x, l(20) */
    std::vector<uint32_t> t; t.push_back(0); t.push_back(0u | (RT_NULL << 12)); auto d = r(TA, "x").t; t.insert(t.end(), d.begin(), d.end());
    uint32_t ttok = 2u | (1u << 2) | (swz_of("xxxx") << 4) | (RT_TID_IN_GROUP << 12); t.push_back(ttok);
    auto im = imm1u(20u).t; t.insert(t.end(), im.begin(), im.end());
    t[0] = OP_IMUL | ((uint32_t)t.size() << 24); pre.w.insert(pre.w.end(), t.begin(), t.end());
    std::vector<uint32_t> u; u.push_back(0); auto d2 = r(TA, "x").t; u.insert(u.end(), d2.begin(), d2.end());
    u.push_back(2u | (1u << 2) | (swz_of("yyyy") << 4) | (RT_TID_IN_GROUP << 12)); auto i2 = imm1u(640u).t; u.insert(u.end(), i2.begin(), i2.end());
    auto s2 = s(TA, "xxxx").t; u.insert(u.end(), s2.begin(), s2.end()); u[0] = OP_IMAD | ((uint32_t)u.size() << 24); pre.w.insert(pre.w.end(), u.begin(), u.end());
    std::vector<uint32_t> q; q.push_back(0); auto d3 = r(TB, "x").t; q.insert(q.end(), d3.begin(), d3.end());
    q.push_back(2u | (1u << 2) | (swz_of("xxxx") << 4) | (RT_TID_IN_GROUP << 12)); auto i3 = imm1u(4u).t; q.insert(q.end(), i3.begin(), i3.end());
    q[0] = OP_ISHL | ((uint32_t)q.size() << 24); preB.w.insert(preB.w.end(), q.begin(), q.end());
    std::vector<uint32_t> y; y.push_back(0); auto d4 = r(TB, "x").t; y.insert(y.end(), d4.begin(), d4.end());
    y.push_back(2u | (1u << 2) | (swz_of("yyyy") << 4) | (RT_TID_IN_GROUP << 12)); auto i4 = imm1u(640u).t; y.insert(y.end(), i4.begin(), i4.end());
    auto s4 = s(TB, "xxxx").t; y.insert(y.end(), s4.begin(), s4.end()); y[0] = OP_IMAD | ((uint32_t)y.size() << 24); preB.w.insert(preB.w.end(), y.begin(), y.end());
  }
  bool pre_done = false, preB_done = false;
  for (size_t k = 0; k < v.size(); k++) {
    const Ins &in = v[k];
    if (in.op == OP_STORE_RAW && !pre_done) { o.insert(o.end(), pre.w.begin(), pre.w.end()); pre_done = true; }
    if (in.op == OP_LD_RAW && !preB_done) { o.insert(o.end(), preB.w.begin(), preB.w.end()); preB_done = true; }
    if (drop[k]) continue;
    if (in.op == OP_DCL_TEMPS) { o.push_back(w[in.at]); o.push_back(ntemps + 3); continue; }
    if ((int)k == tgsm) { o.insert(o.end(), w.begin() + in.at, w.begin() + in.at + 3); o.push_back(kRecs * 4u * 2u); continue; }
    if ((int)k == cbdecl) { o.insert(o.end(), w.begin() + in.at, w.begin() + in.at + in.len); Code d; d.dcl_cb(13, (nw + 3) / 4); o.insert(o.end(), d.w.begin(), d.w.end()); continue; }
    if (!tok[k].empty()) { o.insert(o.end(), tok[k].begin(), tok[k].end()); continue; }
    o.insert(o.end(), w.begin() + in.at, w.begin() + in.at + in.len);
  }
  o[1] = (uint32_t)o.size();
  std::vector<Ins> nv; if (!decode(o, nv, R.why)) { R.why = "re-decode: " + R.why; return R; }
  R.removed = (uint32_t)dce(nv, ntemps + 3);
  /* checks on the result: no load other than field 0 left, the right number of loads/stores, no depth sample left */
  uint32_t nld = 0, nst = 0;
  for (auto &in : nv) { if (in.op == OP_LD_RAW) nld++; if (in.op == OP_STORE_RAW) nst++; }
  if (nld != R.nloads0 || nst != 5) { char t[96]; snprintf(t, sizeof t, "after the rewrite %u loads / %u stores remain (want %u / 5)", nld, nst, R.nloads0); R.why = t; return R; }
  std::vector<uint32_t> fin; fin.push_back(o[0]); fin.push_back(0);
  for (auto &in : nv) fin.insert(fin.end(), o.begin() + in.at, o.begin() + in.at + in.len);
  fin[1] = (uint32_t)fin.size();
  R.n_out = (uint32_t)nv.size(); R.lds_in = kRow * 2; R.lds_out = kRecs * 4 * 2;
  ch[si].data.resize(fin.size() * 4); memcpy(ch[si].data.data(), fin.data(), fin.size() * 4);
  std::vector<Chunk> keep; for (auto &c : ch) if (c.tag != fourcc("STAT")) keep.push_back(c);
  R.blob = write_container(keep); R.ok = true;
  return R;
}

}  // namespace b1ab
