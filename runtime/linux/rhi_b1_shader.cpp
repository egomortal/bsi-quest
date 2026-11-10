/* rhi_b1_shader.cpp -- B1.4: the game's DXBC shaders compiled on our DXVK device through dxbc-spirv.
   The converter class is DXVK 3.1.1's D3D11ShaderConverter (src/d3d11/d3d11_shader.cpp, zlib licence), copied unchanged
   apart from the binding-mapping helper below, so b1 shaders use exactly D3D11's register -> binding layout
   (cbuffer b#, SRV t#, sampler s#, UAV u# per stage). Hidden symbols, like the rest of b1. */
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <array>
#include <map>
#include <sstream>
#include <unordered_map>
#include <sys/stat.h>

#include <dxbc/dxbc_container.h>
#include <dxbc/dxbc_interface.h>
#include <dxbc/dxbc_parser.h>
#include <dxbc/dxbc_converter.h>

#include "dxvk_device.h"
#include "dxvk_shader_ir.h"
#include "dxvk_shader_key.h"
#include "../util/util_small_vector.h"

#define D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT 14u

namespace dxvk {

  struct D3D11ShaderResourceMapping {
    static constexpr uint32_t StageCount        = 6u;
    static constexpr uint32_t CbvPerStage       = 16u;
    static constexpr uint32_t SamplersPerStage  = 16u;
    static constexpr uint32_t SrvPerStage       = 128u;
    static constexpr uint32_t SrvTotal          = SrvPerStage * StageCount;
    static constexpr uint32_t UavPerPipeline    = 64u;
    static constexpr uint32_t UavTotal          = UavPerPipeline * 4u;
    static constexpr uint32_t UavIndexGraphics  = SrvTotal;
    static constexpr uint32_t UavIndexCompute   = UavIndexGraphics + UavPerPipeline * 2u;
    static uint32_t computeCbvBinding(dxbc_spv::ir::ShaderStage stage, uint32_t index) { return computeStageIndex(stage) * CbvPerStage + index; }
    static uint32_t computeSamplerBinding(dxbc_spv::ir::ShaderStage stage, uint32_t index) { return computeStageIndex(stage) * SamplersPerStage + index; }
    static uint32_t computeSrvBinding(dxbc_spv::ir::ShaderStage stage, uint32_t index) { return computeStageIndex(stage) * SrvPerStage + index; }
    static uint32_t computeUavBinding(dxbc_spv::ir::ShaderStage stage, uint32_t index) {
      return (computeStageIndex(stage) == computeStageIndex(dxbc_spv::ir::ShaderStage::eCompute) ? UavIndexCompute : UavIndexGraphics) + index; }
    static uint32_t computeUavCounterBinding(dxbc_spv::ir::ShaderStage stage, uint32_t index) { return computeUavBinding(stage, index) + UavPerPipeline; }
    static constexpr uint32_t computeStageIndex(dxbc_spv::ir::ShaderStage stage) {
      switch (stage) {
        case dxbc_spv::ir::ShaderStage::ePixel:     return 0u;
        case dxbc_spv::ir::ShaderStage::eVertex:    return 1u;
        case dxbc_spv::ir::ShaderStage::eGeometry:  return 2u;
        case dxbc_spv::ir::ShaderStage::eHull:      return 3u;
        case dxbc_spv::ir::ShaderStage::eDomain:    return 4u;
        case dxbc_spv::ir::ShaderStage::eCompute:   return 5u;
        default:                                    return -1u;
      }
    }
  };

  class D3D11ShaderConverter : public DxvkIrShaderConverter {

  public:

    D3D11ShaderConverter(
      const DxvkShaderHash&         ShaderKey,
      const DxvkIrShaderCreateInfo& ModuleInfo,
      const void*                   pShaderBytecode,
            size_t                  BytecodeLength,
            bool                    LowerIcb)
    : m_key(ShaderKey), m_info(ModuleInfo), m_lowerIcb(LowerIcb) {
      m_dxbc.resize(BytecodeLength);
      std::memcpy(m_dxbc.data(), pShaderBytecode, BytecodeLength);
    }

    ~D3D11ShaderConverter() { }

    void convertShader(
            dxbc_spv::ir::Builder&    builder) {
      auto debugName = m_key.toString();

      dxbc_spv::dxbc::Converter::Options options = { };
      options.name = debugName.c_str();
      options.includeDebugNames = true;
      options.boundCheckShaderIo = true;
      options.lowerIcb = m_lowerIcb;
      options.icbRegisterSpace = 0u;
      options.icbRegisterIndex = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
      options.classInstanceRegisterSpace = 0u;
      options.classInstanceRegisterIndex = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + 1u;
      options.limitTessFactor = true;

      dxbc_spv::dxbc::Container container(m_dxbc.data(), m_dxbc.size());

      dxbc_spv::dxbc::ShaderInfo shaderInfo =
        dxbc_spv::dxbc::Parser(container.getCodeChunk()).getShaderInfo();

      dxbc_spv::dxbc::Converter converter(std::move(container), options);

      // Determine whether to create a regular shader or a pass-through GS
      auto dstIsGs = m_key.stage() == VK_SHADER_STAGE_GEOMETRY_BIT;
      auto srcIsGs = shaderInfo.getType() == dxbc_spv::dxbc::ShaderType::eGeometry;

      if (dstIsGs && !srcIsGs) {
        if (!converter.createPassthroughGs(builder))
          throw DxvkError(str::format("Failed to create pass-through geometry shader: ", m_key.toString()));
      } else {
        if (!converter.convertShader(builder))
          throw DxvkError(str::format("Failed to convert shader: ", m_key.toString()));

        lowerBuiltIns(builder);
      }
    }

    uint32_t determineResourceIndex(
            dxbc_spv::ir::ShaderStage stage,
            dxbc_spv::ir::ScalarType  type,
            uint32_t                  regSpace,
            uint32_t                  regIndex) const {
      switch (type) {
        case dxbc_spv::ir::ScalarType::eSampler:
          return D3D11ShaderResourceMapping::computeSamplerBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eCbv:
          return D3D11ShaderResourceMapping::computeCbvBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eSrv:
          return D3D11ShaderResourceMapping::computeSrvBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eUav:
          return D3D11ShaderResourceMapping::computeUavBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eUavCounter:
          return D3D11ShaderResourceMapping::computeUavCounterBinding(stage, regIndex);
        default:
          return -1u;
      }
    }

    void dumpSource(const std::string& path) const {
      std::ofstream file(str::topath(str::format(path, "/", m_key.toString(), ".dxbc").c_str()).c_str(), std::ios_base::trunc | std::ios_base::binary);
      file.write(reinterpret_cast<const char*>(m_dxbc.data()), m_dxbc.size());
    }

    std::string getDebugName() const {
      return m_key.toString();
    }

  private:

    std::vector<uint8_t> m_dxbc;

    DxvkShaderHash          m_key;
    DxvkIrShaderCreateInfo  m_info;

    bool                    m_lowerIcb = false;

    struct BuiltInInfo {
      dxbc_spv::ir::BuiltIn builtIn;
      dxbc_spv::ir::BasicType type;
      const char* name;
    };

    static void lowerBuiltIns(dxbc_spv::ir::Builder& builder) {
      using namespace dxbc_spv;

      // Nothing to do for compute, our speshul built-ins are
      // only used in graphics pipelines.
      auto [entryPoint, shaderStage] = findEntryPoint(builder);

      ir::SsaDef pushData = {};
      ir::SsaDef specSampleCount = {};

      switch (shaderStage) {
        case ir::ShaderStage::eHull: {
          pushData = builder.add(ir::Op::DclPushData(
            ir::ScalarType::eF32, entryPoint, 0u, shaderStage));
          builder.add(ir::Op::DebugName(pushData, "maxTessFactor"));
        } break;

        case ir::ShaderStage::ePixel: {
          specSampleCount = builder.add(ir::Op::DclSpecConstant(
            ir::ScalarType::eU32, entryPoint, 0u, 0u));
          builder.add(ir::Op::DebugName(specSampleCount, "vRasterizer"));
        } break;

        default:
          return;
      }

      // Gather built-in inputs
      small_vector<ir::SsaDef, 32u> inputs;

      for (auto iter = builder.getDeclarations().first;
                iter != builder.getDeclarations().second; iter++) {
        if (iter->getOpCode() == ir::OpCode::eDclInputBuiltIn)
          inputs.push_back(iter->getDef());
      }

      // Rewrite input loads as push data loads
      for (auto inputDef : inputs) {
        const auto& inputOp = builder.getOp(inputDef);

        auto builtIn = ir::BuiltIn(inputOp.getOperand(inputOp.getFirstLiteralOperandIndex()));

        switch (builtIn) {
          case ir::BuiltIn::eSampleCount: {
            dxbc_spv_assert(specSampleCount);
            rewriteBuiltIn(builder, inputDef, specSampleCount);
          } break;

          case ir::BuiltIn::eTessFactorLimit: {
            dxbc_spv_assert(pushData);
            rewriteBuiltIn(builder, inputDef, pushData);
          } break;

          default:
            break;
        }
      }
    }

    static void rewriteBuiltIn(dxbc_spv::ir::Builder& builder, dxbc_spv::ir::SsaDef oldDef, dxbc_spv::ir::SsaDef newDef) {
      using namespace dxbc_spv;

      small_vector<ir::SsaDef, 32u> uses;
      builder.getUses(oldDef, uses);

      const auto& newOp = builder.getOp(newDef);

      for (auto use : uses) {
        const auto& useOp = builder.getOp(use);

        if (useOp.getOpCode() == ir::OpCode::eInputLoad) {
          if (newOp.getOpCode() == ir::OpCode::eDclPushData) {
            auto loadType = useOp.getType().getBaseType(0u);
            builder.rewriteOp(use, ir::Op::PushDataLoad(loadType, newDef, ir::SsaDef()));
          } else {
            builder.rewriteDef(use, newDef);
          }
        }
      }
    }

    static std::pair<dxbc_spv::ir::SsaDef, dxbc_spv::ir::ShaderStage> findEntryPoint(dxbc_spv::ir::Builder& builder) {
      using namespace dxbc_spv;

      auto [a, b] = builder.getDeclarations();

      for (auto iter = a; iter != b; iter++) {
        if (iter->getOpCode() == ir::OpCode::eEntryPoint)
          return std::make_pair(iter->getDef(), ir::ShaderStage(iter->getOperand(iter->getFirstLiteralOperandIndex())));
      }

      return {};
    }

  };

}  // namespace dxvk

using namespace dxvk;

extern Rc<DxvkDevice> b1dx_device();   /* rhi_b1_dxvk.cpp */

namespace {
  std::mutex g_smu;
  std::vector<Rc<DxvkShader>> g_shaders;   /* index = id - 1 */
  std::vector<uint32_t> g_sfree;
  long g_nsh, g_nshfail, g_nshcompiled;
}

static void b1alt_setup(uint32_t id, const std::string &name, const Rc<DxvkShader> &sh);
static void b1alt_forget(uint32_t id);
void b1eo_ps_created(uint32_t id, const void *code, uint32_t len);   /* rhi_b1_dxvk.cpp (rhi_b1_shadoweo.inc) */
void b1ab_cs_created(uint32_t id, const void *code, uint32_t len);   /* rhi_b1_dxvk.cpp (rhi_b1_aoblur.inc) */
/* stage: 0 VS, 1 PS, 2 GS, 3 HS, 4 DS, 5 CS. compile_now: run the DXBC -> IR conversion immediately (validation). */
extern "C" uint32_t b1dx_shader_create(int stage, const void *code, uint32_t len, int compile_now) {
  static const VkShaderStageFlagBits st[6] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT };
  Rc<DxvkDevice> dev = b1dx_device();
  if (dev == nullptr || !code || !len || stage < 0 || stage > 5) return 0;
  try {
    dxbc_spv::dxbc::Container container(code, len);
    auto binHash = container.getHash();
    DxvkShaderHash key(st[stage], len, binHash.data.data(), binHash.data.size());
    DxvkIrShaderCreateInfo info = { };
    info.options = dev->getShaderCompileOptions();
    Rc<DxvkShader> sh = dev->createCachedShader(key.toString(), info, nullptr);
    if (sh == nullptr) {
      Rc<D3D11ShaderConverter> conv = new D3D11ShaderConverter(key, info, code, len, false);
      sh = dev->createCachedShader(key.toString(), info, std::move(conv));
    }
    if (sh == nullptr) { std::lock_guard<std::mutex> lk(g_smu); g_nshfail++; return 0; }
    { static const char *sd = std::getenv("BSI_B1_SHDUMP");   /* pp76 diagnostics (cloud): <dir>/<stage>_<container hash>.dxbc, the hash of the pass logs */
      if (sd && *sd) { std::string fn = std::string(sd) + "/" + key.toString() + ".dxbc";
        if (FILE *e = std::fopen(fn.c_str(), "rb")) std::fclose(e); else if (FILE *w = std::fopen(fn.c_str(), "wb")) { std::fwrite(code, 1, len, w); std::fclose(w); } } }
    if (compile_now) { sh->getShaderMetadata(); }
    std::unique_lock<std::mutex> lk(g_smu);
    g_nsh++; if (compile_now) g_nshcompiled++;
    uint32_t nid;
    if (!g_sfree.empty()) { nid = g_sfree.back(); g_sfree.pop_back(); g_shaders[nid - 1] = sh; }
    else { g_shaders.push_back(sh); nid = (uint32_t)g_shaders.size(); }
    if (stage == 5) { lk.unlock(); b1alt_setup(nid, key.toString(), sh); b1ab_cs_created(nid, code, len); }   /* V4 HDAO blur twin */
    if (stage == 1) { lk.unlock(); b1eo_ps_created(nid, code, len); }   /* S1 shadow early-out twin (rhi_b1_shadoweo.inc) */
    return nid;
  } catch (const DxvkError& e) {
    std::lock_guard<std::mutex> lk(g_smu);
    if (g_nshfail++ < 10) std::fprintf(stderr, "[b1] shader (stage %d, %u bytes) failed: %s\n", stage, len, e.message().c_str());
  } catch (...) {
    std::lock_guard<std::mutex> lk(g_smu);
    if (g_nshfail++ < 10) std::fprintf(stderr, "[b1] shader (stage %d, %u bytes) failed: unknown exception\n", stage, len);
  }
  return 0;
}

extern "C" void b1dx_shader_release_raw(uint32_t id) {   /* via b1dx_shader_release (rhi_b1_dxvk.cpp: bind cache, g_rmu) */
  std::lock_guard<std::mutex> lk(g_smu);
  if (!id || id > g_shaders.size() || g_shaders[id - 1] == nullptr) return;
  g_shaders[id - 1] = nullptr; g_sfree.push_back(id);
  b1alt_forget(id);
}

extern "C" void b1dx_shader_stats(char *out, int n) {
  std::lock_guard<std::mutex> lk(g_smu);
  std::snprintf(out, n, "shaders %ld (compiled %ld), shader failures %ld", g_nsh, g_nshcompiled, g_nshfail);
}


/* ===== AO switch (13 Oct, build 60): the bridge DXVK's BSI_SHADER_ALT / BSI_SHADER_SEL per-Dispatch compute replacement
   (dxvk_ao_android patches dxvk-bsi-shader-replace + -shader-alt + -ao-switch), rebuilt on our side because our DXVK
   core has no replacement patch. Same files (<dir>/<name>.txt "variant <hash|any> <file.spv> [dispatch_scale X Y Z]"),
   same selector file, same binding patch (descriptor and push-data names copied from the original's final SPIR-V).
   A compute shader with a valid variant in alt dir k gets a wrapper shader whose getCode() returns the patched
   replacement; b1dx_c_dispatch binds it for that Dispatch when the selector says k and scales the group counts. ===== */
namespace b1alt {
  using namespace dxvk;
  constexpr uint32_t OpName = 5u, OpMemberName = 6u, OpTypePointer = 32u, OpVariable = 59u, OpDecorate = 71u, OpMemberDecorate = 72u;
  constexpr uint32_t DecBinding = 33u, DecDescriptorSet = 34u, DecOffset = 35u, ScPushConstant = 9u;
  struct Io { std::unordered_map<uint32_t, std::string> names; std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
    std::unordered_map<uint32_t, size_t> setWord, bindingWord; std::map<std::pair<uint32_t, uint32_t>, size_t> offsetWord;
    std::unordered_map<uint32_t, uint32_t> pushPtrToStruct; std::vector<uint32_t> pushStructs; };
  static std::string rstr(const uint32_t *w, size_t n) { std::string s; for (size_t i = 0; i < n; i++) for (uint32_t b = 0; b < 4; b++) { char c = char((w[i] >> (8u * b)) & 0xffu); if (!c) return s; s.push_back(c); } return s; }
  static bool parse(const uint32_t *code, size_t dw, Io &io) {
    if (dw < 5u || code[0] != 0x07230203u) return false;
    std::vector<std::pair<uint32_t, uint32_t>> pv; size_t i = 5u;
    while (i < dw) { uint32_t len = code[i] >> 16, op = code[i] & 0xffffu; if (!len || i + len > dw) return false; const uint32_t *w = &code[i];
      switch (op) {
        case OpName: if (len >= 3u) io.names[w[1]] = rstr(&w[2], len - 2u); break;
        case OpMemberName: if (len >= 4u) io.memberNames[{ w[1], w[2] }] = rstr(&w[3], len - 3u); break;
        case OpDecorate: if (len >= 4u && w[2] == DecDescriptorSet) io.setWord[w[1]] = i + 3u; if (len >= 4u && w[2] == DecBinding) io.bindingWord[w[1]] = i + 3u; break;
        case OpMemberDecorate: if (len >= 5u && w[3] == DecOffset) io.offsetWord[{ w[1], w[2] }] = i + 4u; break;
        case OpTypePointer: if (len >= 4u && w[2] == ScPushConstant) io.pushPtrToStruct[w[1]] = w[3]; break;
        case OpVariable: if (len >= 4u && w[3] == ScPushConstant) pv.push_back({ w[1], w[2] }); break;
        default: break; }
      i += len; }
    for (auto &v : pv) { auto e = io.pushPtrToStruct.find(v.first); if (e != io.pushPtrToStruct.end()) io.pushStructs.push_back(e->second); }
    return true;
  }
  static bool patch(const std::string &name, const std::vector<uint32_t> &repl, const SpirvCodeBuffer &orig, SpirvCodeBuffer &out, bool quiet) {
    Io src, dst; std::vector<uint32_t> code = repl;
    if (!parse(orig.data(), orig.dwords(), src) || !parse(code.data(), code.size(), dst)) { if (!quiet) std::fprintf(stderr, "[b1ao] %s: SPIR-V parse failed\n", name.c_str()); return false; }
    std::unordered_map<std::string, uint32_t> byName;
    for (auto &e : src.setWord) { auto n = src.names.find(e.first); if (n != src.names.end() && !n->second.empty()) byName[n->second] = e.first; }
    for (auto &e : dst.setWord) { auto n = dst.names.find(e.first);
      if (n == dst.names.end() || !byName.count(n->second)) { if (!quiet) std::fprintf(stderr, "[b1ao] %s: descriptor '%s' not in the original\n", name.c_str(), n != dst.names.end() ? n->second.c_str() : "?"); return false; }
      uint32_t sid = byName[n->second]; auto b = dst.bindingWord.find(e.first); auto sb = src.bindingWord.find(sid);
      if (b == dst.bindingWord.end() || sb == src.bindingWord.end()) return false;
      code[e.second] = orig.data()[src.setWord[sid]]; code[b->second] = orig.data()[sb->second]; }
    if (!dst.pushStructs.empty()) { std::unordered_map<std::string, size_t> sm;
      for (uint32_t st : src.pushStructs) for (auto &m : src.memberNames) { if (m.first.first != st) continue; auto o = src.offsetWord.find(m.first); if (o != src.offsetWord.end()) sm[m.second] = o->second; }
      for (uint32_t st : dst.pushStructs) for (auto &o : dst.offsetWord) { if (o.first.first != st) continue; auto m = dst.memberNames.find(o.first);
        auto e = m != dst.memberNames.end() ? sm.find(m->second) : sm.end(); if (e == sm.end()) { if (!quiet) std::fprintf(stderr, "[b1ao] %s: push member not in the original\n", name.c_str()); return false; }
        code[o.second] = orig.data()[e->second]; } }
    out = SpirvCodeBuffer(std::move(code)); return true;
  }
  static uint64_t fnv(const uint32_t *c, size_t dw) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < dw; i++) for (uint32_t b = 0; b < 4u; b++) { h ^= (c[i] >> (8u * b)) & 0xffu; h *= 0x100000001b3ull; } return h; }
  struct Variant { uint64_t hash = 0; bool any = false; std::string file; std::array<uint32_t, 3> scale = {{ 1u, 1u, 1u }}; };
  /* the variant for this original in dir (exact hash first, then "any"), loaded; empty code = none */
  static bool find(const std::string &dir, const std::string &name, const SpirvCodeBuffer &unmapped, std::vector<uint32_t> &code, std::array<uint32_t, 3> &scale, std::string &file) {
    std::ifstream txt(dir + "/" + name + ".txt"); if (!txt) return false;
    std::vector<Variant> list; std::string line;
    while (std::getline(txt, line)) { std::istringstream ls(line); std::string k, h; ls >> k; if (k != "variant") continue; Variant v; ls >> h >> v.file; v.any = h == "any";
      v.hash = v.any ? 0 : std::strtoull(h.c_str(), nullptr, 16); std::string o; while (ls >> o) if (o == "dispatch_scale") for (int i = 0; i < 3; i++) { uint32_t n = 1; if (ls >> n) v.scale[i] = n ? n : 1; }
      if (!v.file.empty()) list.push_back(v); }
    uint64_t h = fnv(unmapped.data(), unmapped.dwords());
    for (int pass = 0; pass < 2; pass++) for (auto &v : list) {
      if (pass == 0 ? (v.any || v.hash != h) : !v.any) continue;
      std::ifstream f(dir + "/" + v.file, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (b.size() < 20 || (b.size() & 3)) continue;
      code.resize(b.size() / 4); std::memcpy(code.data(), b.data(), b.size()); scale = v.scale; file = v.file; return true; }
    return false;
  }
  class AltShader : public DxvkShader {
  public:
    AltShader(Rc<DxvkShader> orig, std::vector<uint32_t> repl, std::string name) : m_orig(std::move(orig)), m_repl(std::move(repl)), m_name(std::move(name)) { }
    DxvkPipelineLayoutBuilder getLayout() override { return m_orig->getLayout(); }
    DxvkShaderMetadata getShaderMetadata() override { return m_orig->getShaderMetadata(); }
    void compile() override { m_orig->compile(); }
    SpirvCodeBuffer getCode(const DxvkShaderBindingMap *b, const DxvkShaderLinkage *l) override {
      SpirvCodeBuffer o = m_orig->getCode(b, l), r;
      if (patch(m_name, m_repl, o, r, false)) { static std::atomic<int> nl{0}; if (nl++ < 8) std::fprintf(stderr, "[b1ao] %s: replacement in use\n", m_name.c_str()); return r; }
      return o; }
    void dump(std::ostream &os) override { m_orig->dump(os); }
    std::string debugName() override { return m_name + ".alt"; }
  private:
    Rc<DxvkShader> m_orig; std::vector<uint32_t> m_repl; std::string m_name;
  };
  struct Alt { Rc<DxvkShader> sh; std::array<uint32_t, 3> scale = {{ 1u, 1u, 1u }}; bool gtao = false; };   /* ao74: gtao = texelFetch-only GTAO */
  static std::mutex g_mu; static std::unordered_map<uint32_t, std::vector<Alt>> g_alts;   /* shader id -> alt 1..N */
  static std::vector<std::string> dirs() {
    static std::vector<std::string> v; static bool init;
    if (!init) { init = true; const char *e = std::getenv("BSI_SHADER_ALT"); std::string f = e ? e : ""; size_t p = 0;
      while (p < f.size()) { size_t q = f.find(':', p); if (q == std::string::npos) q = f.size(); if (q > p) v.push_back(f.substr(p, q - p)); p = q + 1; } }
    return v; }
  static void setup(uint32_t id, const std::string &name, const Rc<DxvkShader> &orig) {
    auto ds = dirs(); if (ds.empty()) return;
    std::vector<Alt> alts(ds.size()); bool any = false; bool haveUnmapped = false; SpirvCodeBuffer unmapped;
    for (size_t k = 0; k < ds.size(); k++) {
      { std::ifstream t(ds[k] + "/" + name + ".txt"); if (!t) continue; }
      try { if (!haveUnmapped) { unmapped = orig->getCode(nullptr, nullptr); haveUnmapped = true; } } catch (...) { return; }
      std::vector<uint32_t> code; std::array<uint32_t, 3> sc; std::string file;
      if (!find(ds[k], name, unmapped, code, sc, file)) { std::fprintf(stderr, "[b1ao] %s: no variant in %s for this code\n", name.c_str(), ds[k].c_str()); continue; }
      SpirvCodeBuffer test; if (!patch(name, code, unmapped, test, false)) continue;
      alts[k].sh = new AltShader(orig, std::move(code), name); alts[k].scale = sc; any = true;
      alts[k].gtao = file.find("gtao") != std::string::npos;   /* ao74: regress/gtao/gtao.comp: cb0 c4.zw only scale positions (no sampler) */
      std::fprintf(stderr, "[b1ao] %s alt %zu = %s/%s (dispatch scale %ux%ux%u)\n", name.c_str(), k + 1, ds[k].c_str(), file.c_str(), sc[0], sc[1], sc[2]);
    }
    if (any) { std::lock_guard<std::mutex> lk(g_mu); g_alts[id] = std::move(alts); }
  }
  static uint32_t select() {   /* BSI_SHADER_SEL: file with the active set number, re-read when its mtime changes */
    static std::string file; static bool init; static int64_t mt = -1; static uint32_t sel = 0; static std::mutex m;
    if (!init) { init = true; const char *e = std::getenv("BSI_SHADER_SEL"); file = e ? e : ""; }
    if (file.empty()) return 0;
    std::lock_guard<std::mutex> lk(m); struct stat st; if (::stat(file.c_str(), &st) != 0) return sel = 0;
    int64_t t = int64_t(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
    if (t != mt) { mt = t; std::ifstream f(file); uint32_t v = 0; if (f >> v) { if (v != sel) std::fprintf(stderr, "[b1ao] selector %u\n", v); sel = v; } }
    return sel;
  }
}
static void b1alt_setup(uint32_t id, const std::string &name, const Rc<DxvkShader> &sh) { try { b1alt::setup(id, name, sh); } catch (...) { } }
static void b1alt_forget(uint32_t id) { std::lock_guard<std::mutex> lk(b1alt::g_mu); b1alt::g_alts.erase(id); }
/* rhi_b1_dxvk.cpp, per Dispatch: the active alternate of compute shader id (nullptr = run the original) */
Rc<DxvkShader> b1dx_shader_alt2(uint32_t id, uint32_t scale[3], bool *gtao);
Rc<DxvkShader> b1dx_shader_alt(uint32_t id, uint32_t scale[3]) { return b1dx_shader_alt2(id, scale, nullptr); }
/* ao74: *gtao = the active alternate is a GTAO build (texelFetch only: the AO constant buffer's 1/size scales positions only) */
Rc<DxvkShader> b1dx_shader_alt2(uint32_t id, uint32_t scale[3], bool *gtao) {
  if (gtao) *gtao = false;
  if (!id) return nullptr;
  { std::lock_guard<std::mutex> lk(b1alt::g_mu); if (b1alt::g_alts.empty() || !b1alt::g_alts.count(id)) return nullptr; }
  uint32_t k = b1alt::select(); if (!k) return nullptr;
  std::lock_guard<std::mutex> lk(b1alt::g_mu); auto &v = b1alt::g_alts[id];
  if (k > v.size() || v[k - 1].sh == nullptr) return nullptr;
  for (int i = 0; i < 3; i++) scale[i] = v[k - 1].scale[i];
  if (gtao) *gtao = v[k - 1].gtao;
  return v[k - 1].sh;
}

/* rhi_b1_dxvk.cpp (COM seam): shader by id */
Rc<DxvkShader> b1dx_shader_get(uint32_t id) {
  std::lock_guard<std::mutex> lk(g_smu);
  return (id && id <= g_shaders.size()) ? g_shaders[id - 1] : nullptr;
}

/* input layout: semantic (name, index) -> input register of the VS signature, -1 if absent */
extern "C" int b1dx_isgn_lookup(const void *code, uint32_t len, const char *const *names, const uint32_t *idx, uint32_t n, int32_t *out) {
  try {
    dxbc_spv::dxbc::Container container(code, len);
    auto isgnChunk = container.getInputSignatureChunk();
    if (!isgnChunk) return 0;
    dxbc_spv::dxbc::Signature sig(std::move(isgnChunk));
    for (uint32_t i = 0; i < n; i++) {
      auto e = sig.findSemantic(0u, names[i], idx[i]);
      out[i] = e != sig.end() ? (int32_t)e->getRegisterIndex() : -1;
    }
    return 1;
  } catch (...) { return 0; }
}
