# Regenerating the embedded SPIR-V (`fill_evenodd_spv.hpp`)

`fill_evenodd_spv.hpp` is a precompiled SPIR-V byte array (`const uint32_t[]`)
generated from `fill_evenodd.comp`. It is **embedded** so the committed build
needs **no runtime or build-time external shader compiler** (glslangValidator /
SPIRV-Tools are intentionally OFF in this tree).

To regenerate it after editing `fill_evenodd.comp`, compile and run the
dev-time generator below **in-process via the vendored glslang** (no external
binary). This tool is NOT part of the CMake build.

```cpp
// gen_spv.cpp — dev tool, compile against vendored glslang.
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
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
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
    return 0;
}
```

Build + run (paths relative to the repo root; the deps build dir must exist):

```sh
GLSLANG_INC=deps_src/glslang
GLSLANG_LIB=build/deps_src/glslang
RESLIM=deps_src/glslang/glslang/ResourceLimits/ResourceLimits.cpp
g++ -std=c++17 -O2 -I"$GLSLANG_INC" gen_spv.cpp "$RESLIM" \
  -Wl,--start-group \
  "$GLSLANG_LIB/glslang/Release/libglslang.a" \
  "$GLSLANG_LIB/SPIRV/Release/libSPIRV.a" \
  -Wl,--end-group -lpthread -o gen_spv
./gen_spv src/libslic3r/SLA/GPU/fill_evenodd.comp \
          src/libslic3r/SLA/GPU/fill_evenodd_spv.hpp fill_evenodd_spv
```

Verify the first word of the array is `0x07230203` (SPIR-V magic).
