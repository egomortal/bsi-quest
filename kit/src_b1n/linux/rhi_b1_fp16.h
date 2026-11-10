/* rhi_b1_fp16.h -- b103: RelaxedPrecision pass over a fragment shader's SPIR-V (opt-in BSI_B1_FP16; NOT bit-exact: INVISIBLE tier).
   Every float-typed value an instruction produces gets "OpDecorate %id RelaxedPrecision" (Turnip's SPIR-V -> NIR honours it per instruction:
   mediump_16bit_alu = true, the ALU runs in fp16 with half registers), EXCEPT the backward dataflow slice of every image-sampling coordinate
   (texture addressing stays fp32: a 2048-texel UV in fp16 would be off by texels), of fragment-depth / sample-mask outputs and of OpKill
   conditions (alpha-test edges stay put). Input / uniform variables are never decorated (interpolants and constants are loaded as fp32 and
   converted at use). Host-testable: spv_relax() is pure. */
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace b1fp16 {
  struct Stats { uint32_t floatvals = 0, relaxed = 0, kept = 0, images = 0; };
  enum : uint32_t { OpExtInst = 12, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypePointer = 32, OpTypeFunction = 33, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
                    OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75,
                    OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82, OpCopyObject = 83,
                    OpImageSampleImplicitLod = 87, OpImageSampleProjDrefExplicitLod = 94, OpImageFetch = 95, OpImageGather = 96, OpImageDrefGather = 97, OpImageQueryLod = 106,
                    OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112, OpFConvert = 115, OpBitcast = 124,
                    OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143,
                    OpVectorTimesMatrix = 144, OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146, OpOuterProduct = 147, OpDot = 148,
                    OpSelect = 169, OpDPdx = 207, OpDPdy = 208, OpFwidth = 209, OpPhi = 245, OpBranchConditional = 250, OpKill = 252, OpFunction = 54, OpLabel = 248,
                    OpTypeVoid = 19, OpTypeForwardPointer = 39, OpConstantTrue = 41, OpSpecConstantOp = 52, OpExecutionMode = 16, OpExtInstImport = 11,
                    OpImageSparseSampleImplicitLod = 305 };
  static const uint32_t DecRelaxedPrecision = 0u, DecBuiltIn = 11u, BuiltInFragDepth = 22u, BuiltInSampleMask = 20u;

  /* DXBC: count of shadow-compare texture ops (sample_c 70, sample_c_lz 71, gather4_c 127, gather4_po_c 128) and of all instructions */
  static void dxbc_census(const void *code, uint32_t len, uint32_t &ninstr, uint32_t &ncmp) {
    ninstr = ncmp = 0; const uint8_t *p = (const uint8_t *)code; if (!p || len < 40 || std::memcmp(p, "DXBC", 4)) return; uint32_t nch; std::memcpy(&nch, p + 32, 4); if (nch > 64) return;
    for (uint32_t c = 0; c < nch; c++) { if (36 + 4 * c + 4 > len) break; uint32_t off; std::memcpy(&off, p + 36 + 4 * c, 4); if ((uint64_t)off + 8 > len) continue; uint32_t tag, sz; std::memcpy(&tag, p + off, 4); std::memcpy(&sz, p + off + 4, 4);
      if (tag != 0x52444853u && tag != 0x58454853u) continue; if ((uint64_t)off + 8 + sz > len || sz < 8) continue; const uint32_t *t = (const uint32_t *)(p + off + 8); uint32_t n = sz / 4 < t[1] ? sz / 4 : t[1];
      uint32_t i = 2; while (i < n) { uint32_t op = t[i] & 0x7ffu, l = (t[i] >> 24) & 0x7fu; if (op == 53u) l = i + 1 < n ? t[i + 1] : 0; if (!l) break; if (op < 89u || op > 111u) ninstr++; if (op == 70u || op == 71u || op == 126u || op == 128u) ncmp++;   /* sample_c, sample_c_lz, gather4_c (126), gather4_po_c (128) */ i += l; } return; } }
  static bool relax(const uint32_t *w, size_t n, std::vector<uint32_t> &out, Stats &st, std::string *err = nullptr) {
    auto fail = [&] (const char *m) { if (err) *err = m; return false; };
    if (n < 5 || w[0] != 0x07230203u) return fail("not SPIR-V");
    std::vector<size_t> ins; for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16; if (!len || i + len > n) return fail("bad instruction length"); ins.push_back(i); i += len; }
    auto op = [&] (size_t k) { return w[ins[k]] & 0xffffu; }; auto len = [&] (size_t k) { return w[ins[k]] >> 16; }; auto word = [&] (size_t k, uint32_t j) { return w[ins[k] + j]; };
    std::unordered_set<uint32_t> ftypes;                 /* float32 scalar / vector / matrix type ids */
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;  /* pointer type -> pointee type */
    std::unordered_map<uint32_t, uint32_t> ptr_class;
    std::unordered_map<uint32_t, size_t> def;            /* result id -> instruction index (typed instructions) */
    std::unordered_set<uint32_t> builtin_keep;           /* variables decorated BuiltIn FragDepth / SampleMask */
    for (size_t k = 0; k < ins.size(); k++) { uint32_t o = op(k);
      if (o == OpTypeFloat && len(k) >= 3 && word(k, 2) == 32u) ftypes.insert(word(k, 1));
      else if (o == OpTypeVector && len(k) >= 4 && ftypes.count(word(k, 2))) ftypes.insert(word(k, 1));
      else if (o == OpTypeMatrix && len(k) >= 4 && ftypes.count(word(k, 2))) ftypes.insert(word(k, 1));
      else if (o == OpTypePointer && len(k) >= 4) { ptr_pointee[word(k, 1)] = word(k, 3); ptr_class[word(k, 1)] = word(k, 2); }
      else if (o == OpDecorate && len(k) >= 4 && word(k, 2) == DecBuiltIn && (word(k, 3) == BuiltInFragDepth || word(k, 3) == BuiltInSampleMask)) builtin_keep.insert(word(k, 1)); }
    /* typed instructions: result type at word 1, result id at word 2 (every value-producing op used here) */
    auto typed = [&] (uint32_t o) { return o == OpExtInst || o == OpLoad || o == OpAccessChain || (o >= OpVectorShuffle && o <= OpCopyObject) || (o >= OpImageSampleImplicitLod && o <= OpImageDrefGather) || o == OpImageQueryLod ||
                                     (o >= OpConvertFToU && o <= OpFConvert) || o == OpBitcast || (o >= OpFNegate && o <= OpDot) || o == OpSelect || (o >= OpDPdx && o <= OpFwidth) || o == OpPhi || o == OpVariable || o == OpImageSparseSampleImplicitLod || (o >= 164u && o <= 168u) /* OpLogical* */ || (o >= 170u && o <= 191u) /* OpIEqual .. OpFUnordGreaterThanEqual */; };
    for (size_t k = 0; k < ins.size(); k++) if (typed(op(k)) && len(k) >= 3) def[word(k, 2)] = k;
    /* candidates: float-valued results of relaxable instructions */
    auto relaxable = [&] (uint32_t o) { return o == OpExtInst || o == OpLoad || (o >= OpVectorShuffle && o <= OpCopyObject) || (o >= OpImageSampleImplicitLod && o <= OpImageDrefGather) || o == OpConvertSToF || o == OpConvertUToF ||
                                         (o >= OpFNegate && o <= OpDot) || o == OpSelect || o == OpPhi || o == OpImageSparseSampleImplicitLod; };
    std::vector<uint32_t> cand; std::unordered_set<uint32_t> candset;
    for (size_t k = 0; k < ins.size(); k++) { uint32_t o = op(k); if (!relaxable(o) || len(k) < 3) continue; if (!ftypes.count(word(k, 1))) continue; cand.push_back(word(k, 2)); candset.insert(word(k, 2)); }
    /* function-local float variables too (OpVariable with a Function-class pointer to a float type) */
    std::vector<uint32_t> vars; for (size_t k = 0; k < ins.size(); k++) if (op(k) == OpVariable && len(k) >= 4 && word(k, 3) == 7u /* Function */) { auto p = ptr_pointee.find(word(k, 1)); if (p != ptr_pointee.end() && ftypes.count(p->second)) vars.push_back(word(k, 2)); }
    st.floatvals = (uint32_t)(cand.size() + vars.size());
    /* keep set: backward slices of image coordinates, OpKill conditions, FragDepth / SampleMask stores */
    std::vector<uint32_t> work; std::unordered_set<uint32_t> keep;
    auto push = [&] (uint32_t id) { if (id && !keep.count(id)) { keep.insert(id); work.push_back(id); } };
    for (size_t k = 0; k < ins.size(); k++) { uint32_t o = op(k);
      if (((o >= OpImageSampleImplicitLod && o <= OpImageDrefGather) || o == OpImageQueryLod || o == OpImageSparseSampleImplicitLod) && len(k) >= 5) { st.images++; push(word(k, 4));   /* coordinate */
        bool dref = o == 89u || o == 90u || o == 93u || o == 94u || o == OpImageDrefGather; uint32_t m = dref ? 6u : 5u; if (dref && len(k) >= 6) push(word(k, 5));   /* Dref; then the image-operands mask (literal) and its ids (lod / grad / bias / offsets) */
        for (uint32_t j = m + 1; j < len(k); j++) push(word(k, j)); }
      else if ((o == OpConvertFToU || o == OpConvertFToS || o == OpBitcast) && len(k) >= 4) push(word(k, 3));   /* integer results of float maths (indices, bit tricks) stay exact */
      else if ((o == OpDPdx || o == OpDPdy || o == OpFwidth) && len(k) >= 4) push(word(k, 3));
      else if (o == OpStore && len(k) >= 3 && builtin_keep.count(word(k, 1))) push(word(k, 2));
      else if (o == OpBranchConditional && len(k) >= 4) { /* a conditional branch whose true or false block starts with OpKill: keep its condition */
        for (uint32_t t = 2; t <= 3; t++) { uint32_t lbl = word(k, t); for (size_t m = 0; m < ins.size(); m++) if (op(m) == OpLabel && len(m) >= 2 && word(m, 1) == lbl) { if (m + 1 < ins.size() && (op(m + 1) == OpKill || op(m + 1) == 5380u /* OpDemoteToHelperInvocation */ || op(m + 1) == 4416u /* OpTerminateInvocation */)) push(word(k, 1)); break; } } } }
    while (!work.empty()) { uint32_t id = work.back(); work.pop_back(); auto d = def.find(id); if (d == def.end()) continue; size_t k = d->second; uint32_t o = op(k);
      switch (o) {
        case OpVariable: break;
        case OpLoad: if (len(k) >= 4) push(word(k, 3)); break;
        case OpAccessChain: for (uint32_t j = 3; j < len(k); j++) push(word(k, j)); break;
        case OpExtInst: for (uint32_t j = 5; j < len(k); j++) push(word(k, j)); break;
        case OpCompositeExtract: if (len(k) >= 4) push(word(k, 3)); break;
        case OpCompositeInsert: if (len(k) >= 5) { push(word(k, 3)); push(word(k, 4)); } break;
        case OpVectorShuffle: if (len(k) >= 5) { push(word(k, 3)); push(word(k, 4)); } break;
        case OpPhi: for (uint32_t j = 3; j < len(k); j += 2) push(word(k, j)); break;
        case OpImageSampleImplicitLod: case OpImageFetch: case OpImageGather: case OpImageDrefGather: case OpImageSparseSampleImplicitLod: break;   /* a sampled value feeding a coordinate: the fetch itself stays fp32 */
        default: if (o >= 88 && o <= 94) break; for (uint32_t j = 3; j < len(k); j++) push(word(k, j)); break; } }
    /* OpStore into a kept function variable: its stored value is part of the slice too (the variable is loaded fp32 later) */
    for (size_t k = 0; k < ins.size(); k++) if (op(k) == OpStore && len(k) >= 3 && keep.count(word(k, 1))) { uint32_t v = word(k, 2); if (!keep.count(v)) { push(v);
        while (!work.empty()) { uint32_t id = work.back(); work.pop_back(); auto d = def.find(id); if (d == def.end()) continue; size_t m = d->second; uint32_t o = op(m);
          if (o == OpVariable || (o >= OpImageSampleImplicitLod && o <= OpImageDrefGather)) continue; if (o == OpLoad) { if (len(m) >= 4) push(word(m, 3)); continue; }
          if (o == OpPhi) { for (uint32_t j = 3; j < len(m); j += 2) push(word(m, j)); continue; } if (o == OpExtInst) { for (uint32_t j = 5; j < len(m); j++) push(word(m, j)); continue; }
          if (o == OpCompositeExtract) { if (len(m) >= 4) push(word(m, 3)); continue; } for (uint32_t j = 3; j < len(m); j++) push(word(m, j)); } } }
    std::vector<uint32_t> dec; for (uint32_t id : cand) { if (keep.count(id)) { st.kept++; continue; } dec.push_back(id); }
    for (uint32_t id : vars) { if (keep.count(id)) { st.kept++; continue; } dec.push_back(id); }
    st.relaxed = (uint32_t)dec.size();
    /* insertion point: before the first annotation, else before the first type / constant / global declaration */
    size_t at = ins.size(); for (size_t k = 0; k < ins.size(); k++) { uint32_t o = op(k); if ((o >= OpDecorate && o <= OpGroupMemberDecorate) || (o >= OpTypeVoid && o <= OpTypeForwardPointer) || (o >= OpConstantTrue && o <= OpSpecConstantOp) || o == OpVariable || o == OpFunction) { at = k; break; } }
    if (at == ins.size()) return fail("no insertion point");
    size_t pos = ins[at]; out.clear(); out.reserve(n + 3 * dec.size()); out.insert(out.end(), w, w + pos);
    for (uint32_t id : dec) { out.push_back((3u << 16) | OpDecorate); out.push_back(id); out.push_back(DecRelaxedPrecision); }
    out.insert(out.end(), w + pos, w + n); return true; }
}
