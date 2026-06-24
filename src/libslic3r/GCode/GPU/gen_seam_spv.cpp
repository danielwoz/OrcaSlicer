// gen_spv.cpp — dev tool, compile against vendored glslang. Targets Vulkan 1.2 /
// SPIR-V 1.4 so GL_EXT_ray_query is accepted.
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s in.comp out.hpp symbol\n", argv[0]); return 2; }
    std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str(); const char* srcs[] = { src.c_str() };
    glslang::InitializeProcess();
    glslang::TShader shader(EShLangCompute);
    shader.setStrings(srcs, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_4);
    const TBuiltInResource* res = GetDefaultResources();
    EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(res, 100, false, msgs)) { std::fprintf(stderr, "PARSE FAIL:\n%s\n", shader.getInfoLog()); return 1; }
    glslang::TProgram program; program.addShader(&shader);
    if (!program.link(msgs)) { std::fprintf(stderr, "LINK FAIL:\n%s\n", program.getInfoLog()); return 1; }
    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv);
    glslang::FinalizeProcess();
    std::ofstream out(argv[2]);
    out << "#pragma once\n#include <cstdint>\n#include <cstddef>\n\n";
    out << "static const uint32_t " << argv[3] << "[] = {\n";
    for (size_t i = 0; i < spirv.size(); ++i) { char b[16]; std::snprintf(b, sizeof(b), "0x%08x,", spirv[i]); if (i%8==0) out << "    "; out << b << ((i%8==7)?"\n":" "); }
    out << "\n};\nstatic const size_t " << argv[3] << "_size = " << spirv.size() << ";\n";
    std::fprintf(stderr, "OK: %zu words, magic=0x%08x\n", spirv.size(), spirv.empty()?0:spirv[0]);
    return 0;
}
