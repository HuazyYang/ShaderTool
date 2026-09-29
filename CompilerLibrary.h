#ifndef SHADER_TOOL_COMPILER_LIBRARY_H
#define SHADER_TOOL_COMPILER_LIBRARY_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* Frozen interface (contract section 8). One call compiles one resolved variant with
   one backend, fully in memory. 0 on success, -1 on failure; Diagnostics is filled
   either way when non-empty. No exceptions escape this API. */

enum class ShaderToolBackend { Dxbc, Dxil, Spirv, SlangDxbc, SlangDxil, SlangSpirv };

struct CompileVariantRequest {
    ShaderToolBackend Backend;
    std::string Source;                 // path as given on the command line
    std::string EntryPoint;             // default "main"
    std::string Profile;                // full, e.g. "ps_6_5"
    std::vector<std::pair<std::string, std::string>> Defines;  // resolved variant defines,
                                                               // no braces; valueless
                                                               // already normalized to "1"
    std::vector<std::string> IncludeDirs;
    std::vector<std::string> ExtraArgs; // canonical dxc-style tokens validated by the
                                        // options layer (joined forms intact, e.g.
                                        // "-fspv-target-env=vulkan1.2"; multi-token
                                        // shifts as consecutive tokens)
};

struct CompileVariantResult {
    std::vector<uint8_t> Object;        // DXBC/DXIL/SPIR-V bytes
    std::vector<std::string> Includes;  // resolved paths of every included file
                                        // (depfile input)
    std::string Diagnostics;            // UTF-8 warnings/errors
};

// 0 on success, -1 on failure (Diagnostics filled either way when non-empty).
int CompilerLibraryCompile(const CompileVariantRequest &request, CompileVariantResult &result);

#endif
