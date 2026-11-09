// vk_glsl.cpp — Vulkan GLSL (from dxbc_to_vk.cpp) to SPIR-V, on the phone, with glslang.
//
// glslang is the copy the NDK ships for shaderc (sources/third_party/shaderc/third_party/glslang, 11.11.0). That copy
// leaves out StandAlone/ResourceLimits.cpp, so the built-in limits are set here, to glslang's own defaults: they size
// built-in arrays (gl_ClipDistance, ...) and bound what a shader may declare; the GPU's real limits apply at pipeline
// creation.
#include "vk_backend.h"

#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace bo2vk {
namespace {

TBuiltInResource default_resources() {
    TBuiltInResource r{};
    r.maxLights = 32; r.maxClipPlanes = 6; r.maxTextureUnits = 32; r.maxTextureCoords = 32;
    r.maxVertexAttribs = 64; r.maxVertexUniformComponents = 4096; r.maxVaryingFloats = 64;
    r.maxVertexTextureImageUnits = 32; r.maxCombinedTextureImageUnits = 80; r.maxTextureImageUnits = 32;
    r.maxFragmentUniformComponents = 4096; r.maxDrawBuffers = 32; r.maxVertexUniformVectors = 128;
    r.maxVaryingVectors = 8; r.maxFragmentUniformVectors = 16; r.maxVertexOutputVectors = 16;
    r.maxFragmentInputVectors = 15; r.minProgramTexelOffset = -8; r.maxProgramTexelOffset = 7;
    r.maxClipDistances = 8; r.maxComputeWorkGroupCountX = 65535; r.maxComputeWorkGroupCountY = 65535;
    r.maxComputeWorkGroupCountZ = 65535; r.maxComputeWorkGroupSizeX = 1024; r.maxComputeWorkGroupSizeY = 1024;
    r.maxComputeWorkGroupSizeZ = 64; r.maxComputeUniformComponents = 1024; r.maxComputeTextureImageUnits = 16;
    r.maxComputeImageUniforms = 8; r.maxComputeAtomicCounters = 8; r.maxComputeAtomicCounterBuffers = 1;
    r.maxVaryingComponents = 60; r.maxVertexOutputComponents = 64; r.maxGeometryInputComponents = 64;
    r.maxGeometryOutputComponents = 128; r.maxFragmentInputComponents = 128; r.maxImageUnits = 8;
    r.maxCombinedImageUnitsAndFragmentOutputs = 8; r.maxCombinedShaderOutputResources = 8; r.maxImageSamples = 0;
    r.maxVertexImageUniforms = 0; r.maxTessControlImageUniforms = 0; r.maxTessEvaluationImageUniforms = 0;
    r.maxGeometryImageUniforms = 0; r.maxFragmentImageUniforms = 8; r.maxCombinedImageUniforms = 8;
    r.maxGeometryTextureImageUnits = 16; r.maxGeometryOutputVertices = 256; r.maxGeometryTotalOutputComponents = 1024;
    r.maxGeometryUniformComponents = 1024; r.maxGeometryVaryingComponents = 64; r.maxTessControlInputComponents = 128;
    r.maxTessControlOutputComponents = 128; r.maxTessControlTextureImageUnits = 16;
    r.maxTessControlUniformComponents = 1024; r.maxTessControlTotalOutputComponents = 4096;
    r.maxTessEvaluationInputComponents = 128; r.maxTessEvaluationOutputComponents = 128;
    r.maxTessEvaluationTextureImageUnits = 16; r.maxTessEvaluationUniformComponents = 1024;
    r.maxTessPatchComponents = 120; r.maxPatchVertices = 32; r.maxTessGenLevel = 64; r.maxViewports = 16;
    r.maxVertexAtomicCounters = 0; r.maxTessControlAtomicCounters = 0; r.maxTessEvaluationAtomicCounters = 0;
    r.maxGeometryAtomicCounters = 0; r.maxFragmentAtomicCounters = 8; r.maxCombinedAtomicCounters = 8;
    r.maxAtomicCounterBindings = 1; r.maxVertexAtomicCounterBuffers = 0; r.maxTessControlAtomicCounterBuffers = 0;
    r.maxTessEvaluationAtomicCounterBuffers = 0; r.maxGeometryAtomicCounterBuffers = 0;
    r.maxFragmentAtomicCounterBuffers = 1; r.maxCombinedAtomicCounterBuffers = 1; r.maxAtomicCounterBufferSize = 16384;
    r.maxTransformFeedbackBuffers = 4; r.maxTransformFeedbackInterleavedComponents = 64; r.maxCullDistances = 8;
    r.maxCombinedClipAndCullDistances = 8; r.maxSamples = 4;
    r.maxMeshOutputVerticesNV = 256; r.maxMeshOutputPrimitivesNV = 512; r.maxMeshWorkGroupSizeX_NV = 32;
    r.maxMeshWorkGroupSizeY_NV = 1; r.maxMeshWorkGroupSizeZ_NV = 1; r.maxTaskWorkGroupSizeX_NV = 32;
    r.maxTaskWorkGroupSizeY_NV = 1; r.maxTaskWorkGroupSizeZ_NV = 1; r.maxMeshViewCountNV = 4;
    r.maxMeshOutputVerticesEXT = 256; r.maxMeshOutputPrimitivesEXT = 256; r.maxMeshWorkGroupSizeX_EXT = 128;
    r.maxMeshWorkGroupSizeY_EXT = 128; r.maxMeshWorkGroupSizeZ_EXT = 128; r.maxTaskWorkGroupSizeX_EXT = 128;
    r.maxTaskWorkGroupSizeY_EXT = 128; r.maxTaskWorkGroupSizeZ_EXT = 128; r.maxMeshViewCountEXT = 4;
    r.maxDualSourceDrawBuffersEXT = 1;
    r.limits.nonInductiveForLoops = true; r.limits.whileLoops = true; r.limits.doWhileLoops = true;
    r.limits.generalUniformIndexing = true; r.limits.generalAttributeMatrixVectorIndexing = true;
    r.limits.generalVaryingIndexing = true; r.limits.generalSamplerIndexing = true;
    r.limits.generalVariableIndexing = true; r.limits.generalConstantMatrixVectorIndexing = true;
    return r;
}

std::once_flag g_init;
const TBuiltInResource& resources() {
    static const TBuiltInResource r = default_resources();
    return r;
}

bool compile(bool pixel, const std::string& src, std::vector<uint32_t>& spirv, std::string& log);

// The SPIR-V cache: <BO2_SHADER_CACHE_DIR>/vk_spv_<fnv64 of the stage, the glslang settings below and the GLSL>.spv.
// Without it every shader went through glslang again in every session, on the CPU, partly in the frame that first
// draws it. The key covers everything compile() depends on; bump kSpvCacheTag when compile() changes. A file that is not
// whole SPIR-V (wrong size, wrong magic) is ignored and rewritten.
constexpr const char* kSpvCacheTag = "spv1 glslang-11.11 vk1.1 spv1.3 440 core noopt";

std::string spv_cache_path(bool pixel, const std::string& src) {
    const char* dir = getenv("BO2_SHADER_CACHE_DIR");
    if (!dir || !*dir) return std::string();
    static const bool on = [] { const char* e = getenv("BO2_SPV_CACHE"); return !e || *e != '0'; }();
    if (!on) return std::string();
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const char* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ull; } };
    mix(kSpvCacheTag, strlen(kSpvCacheTag));
    mix(pixel ? "ps" : "vs", 2);
    mix(src.data(), src.size());
    char nm[48];
    snprintf(nm, sizeof nm, "/vk_spv_%016llx.spv", (unsigned long long)h);
    return std::string(dir) + nm;
}

bool spv_read(const std::string& path, std::vector<uint32_t>& spirv) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n >= 20 && n % 4 == 0;
    if (ok) {
        spirv.resize((size_t)n / 4);
        ok = fread(spirv.data(), 1, (size_t)n, f) == (size_t)n && spirv[0] == 0x07230203u;
    }
    fclose(f);
    if (!ok) spirv.clear();
    return ok;
}

void spv_write(const std::string& path, const std::vector<uint32_t>& spirv) {
    const std::string tmp = path + ".tmp" + std::to_string((unsigned long long)(uintptr_t)&spirv);
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(spirv.data(), 4, spirv.size(), f) == spirv.size();
    fclose(f);
    if (ok) rename(tmp.c_str(), path.c_str());
    else remove(tmp.c_str());
}

}  // namespace

bool glsl_to_spirv(bool pixel, const std::string& src, std::vector<uint32_t>& spirv, std::string& log) {
    const std::string path = spv_cache_path(pixel, src);
    if (!path.empty() && spv_read(path, spirv)) return true;
    if (!compile(pixel, src, spirv, log)) return false;
    if (!path.empty()) spv_write(path, spirv);
    return true;
}

namespace {
bool compile(bool pixel, const std::string& src, std::vector<uint32_t>& spirv, std::string& log) {
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    const EShLanguage stage = pixel ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    const char* text = src.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const EShMessages msg = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(&resources(), 440, ECoreProfile, false, false, msg)) {
        log = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msg)) {
        log = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.generateDebugInfo = false;
    opt.disableOptimizer = true;   // SPIRV-Tools is not linked; the driver optimises
    opt.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &opt);
    if (spirv.empty()) {
        log = "GlslangToSpv produced nothing";
        return false;
    }
    return true;
}
}  // namespace

}  // namespace bo2vk
