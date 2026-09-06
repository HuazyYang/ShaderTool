#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ShaderToolUtils.h"

// #include <vld.h>

enum {
    BackendFxc = 1 << 0,
    BackendDxc = 1 << 1,
    BackendSpirv = 1 << 2,
    BackendAll = BackendFxc | BackendDxc | BackendSpirv
};

typedef struct OptionSpec OptionSpec;

typedef struct ParsedOption {
    const OptionSpec *Spec;
    AStringView Spelling;
    AStringView Values[4];
} ParsedOption;

typedef struct ParsedOptionArray {
    union {
        DynamicArray Array;
        struct {
            ParsedOption *Buffer;
            size_t Size;
            size_t Capacity;
        };
    };
} ParsedOptionArray;

typedef struct Args {
    size_t Backend;
    AStringView CompilerProgram;
    ParsedOptionArray CompilerOptions;
    AStringView DepfilePath;
    AStringView SourceFilePath;
    AStringView OutputFilePath;
    AStringView EntryPointName;
    AStringView HeaderVariableName;
    bool HeaderOutput;
} Args;

typedef struct DependsParserContext {
    bool Enabled;
    bool StdoutOnly;
    size_t Backend;
    size_t SessionState;
    size_t LineIndex;
    ByteBuffer SrcContent;
    AString Target;
    ByteBuffer DependList;
} DependsParserContext;

#pragma region[ Args ]

void ArgsDeinit(Args *args) {
    DYNAMIC_ARRAY_DEINIT_TYPED(&args->CompilerOptions);
    memset(args, 0, sizeof(*args));
}

int ArgsAppendCompilerOption(Args *args, const ParsedOption *option) {
    return DYNAMIC_ARRAY_APPEND_TYPED(&args->CompilerOptions, ParsedOption, option, 1,
                                "compiler option storage");
}

bool ArgsHasDepfile(const Args *args) {
    return !AStringViewIsEmpty(&args->DepfilePath);
}

#pragma endregion[Args]

const char *ReadNextLine(const char *start, const char *end) {
    for (; start != end && *start != '\n'; ++start)
        ;
    if (start != end)
        ++start;
    return start;
}

#pragma region[ DependsParser ]

int DependsParserAppendDependList(DependsParserContext *ctx, const AStringView *depend) {
    const char *start = ctx->DependList.Buffer;
    const char *end = ctx->DependList.Buffer + ctx->DependList.Size;
    AString depend_path_buffer = {0};
    AStringView depend2;
    int ret = -1;

    if (NormalizeAPath(depend, &depend_path_buffer))
        goto final_cleanup;
    depend2 = AStringViewFromBuffer(depend_path_buffer.Buffer, depend_path_buffer.Length);

    for (; start != end;) {
        if (AStringViewEqual(&depend2, start))
            return 0;
        start += strlen(start) + 1;
    }

    if (ByteBufferWrite(&ctx->DependList, depend2.Buffer, depend2.Length))
        goto final_cleanup;

    if (ByteBufferWrite(&ctx->DependList, "", 1))
        goto final_cleanup;

    ret = 0;
final_cleanup:
    AStringDeinit(&depend_path_buffer);
    return ret;
}

int DependsParserInit(DependsParserContext *ctx, const Args *args) {
    AStringView path_view;

    ctx->Enabled = ArgsHasDepfile(args);
    ctx->StdoutOnly = AStringViewEqual(&args->DepfilePath, "-");
    ctx->Backend = args->Backend;

    if (ctx->Enabled) {
        path_view = AStringViewFromBuffer(args->OutputFilePath.Buffer,
                                          args->OutputFilePath.Length);
        if (NormalizeAPath(&path_view, &ctx->Target))
            return -1;

        if (DependsParserAppendDependList(ctx, &args->SourceFilePath))
            return -1;
    }

    return 0;
}

int DependsParserInput(DependsParserContext *ctx, const AStringView *content,
                       bool handle_remaining) {
    // Opening file [*], stack top [1]
    // Current working dir [*]
    // Resolved to [*]
    const AStringView FirstLinePrefix = A_STRING_VIEW_LITERAL("Opening file [");
    const AStringView SecondLinePrefix = A_STRING_VIEW_LITERAL("Current working dir [");
    const AStringView ThirdLinePrefix = A_STRING_VIEW_LITERAL("Resolved to [");
    const AStringView DxcLinePrefix = A_STRING_VIEW_LITERAL("; Opening file [");

    const char *start, *next, *last, *end;
    AStringView curr;
    int ret = -1;

    if (ctx->Enabled) {
        if (!AStringViewIsEmpty(content)) {
            if (ByteBufferCatAStringViews(&ctx->SrcContent, 1, content))
                goto final_cleanup;
        }

        start = ctx->SrcContent.Buffer;
        end = ctx->SrcContent.Buffer + ctx->SrcContent.Size;

        if (!handle_remaining && start != end) {
            last = end - 1;
            for (last = end - 1; last != start && *last != '\n'; --last)
                ;
            if (last == start) {
                ret = 0;
                goto final_cleanup;
            }
            end = last + 1;
        }

        while (start != end) {
            curr = AStringViewFromBuffer(start, end - start);

            if (ctx->Backend != BackendFxc &&
                AStringViewEqualAtLeast(&curr, &DxcLinePrefix)) {
                start += DxcLinePrefix.Length;
                for (next = start; next != end && *next != ']'; ++next)
                    ;
                curr = AStringViewFromBuffer(start, next - start);
                if (DependsParserAppendDependList(ctx, &curr))
                    goto relax_cleanup;
                start = ReadNextLine(next, end);
                ++ctx->LineIndex;
                continue;
            }

            if (AStringViewEqualAtLeast(&curr, &FirstLinePrefix)) {
                start += FirstLinePrefix.Length;
                ctx->SessionState = 1;
            } else {
                next = ReadNextLine(start, end);
                if (next != start && !ctx->StdoutOnly)
                    LOG_I("%.*s", (int)(next - start), start);
                start = next;
                ++ctx->LineIndex;
                continue;
            }

            if ((start = ReadNextLine(start, end)) == end)
                break;
            ++ctx->LineIndex;

            while (start != end) {
                curr = AStringViewFromBuffer(start, end - start);
                if (AStringViewEqualAtLeast(&curr, &SecondLinePrefix)) {
                    start += SecondLinePrefix.Length;
                    ctx->SessionState = 2;

                    if ((start = ReadNextLine(start, end)) == end)
                        break;
                    ++ctx->LineIndex;
                } else {
                    if (ctx->SessionState != 1 && ctx->SessionState != 2) {
                        LOG_W(
                            "Read inline stack log line (%zu) mismatched, must start with "
                            "'%s':\n%.*s\n",
                            ctx->LineIndex, SecondLinePrefix.Buffer, (int)curr.Length,
                            curr.Buffer);
                        goto relax_cleanup;
                    }
                    break;
                }
            }

            if (start != end) {
                curr = AStringViewFromBuffer(start, end - start);
                if (AStringViewEqualAtLeast(&curr, &ThirdLinePrefix)) {
                    start += ThirdLinePrefix.Length;
                    ctx->SessionState = 0;
                } else {
                    LOG_E(
                        "Read inline stack log line (%zu) mismatched, must start with "
                        "'%s':\n%.*s\n'",
                        ctx->LineIndex, ThirdLinePrefix.Buffer, (int)curr.Length,
                        curr.Buffer);
                    goto relax_cleanup;
                }
            }

            for (next = start; next != end && *next != ']'; ++next)
                ;

            curr = AStringViewFromBuffer(start, next - start);

            if (DependsParserAppendDependList(ctx, &curr))
                goto relax_cleanup;

            if ((start = ReadNextLine(next, end)) == end)
                break;

            ++ctx->LineIndex;
        }

    relax_cleanup:
        ByteBufferEraseRange(&ctx->SrcContent, 0, end - ctx->SrcContent.Buffer);
        ret = 0;
        goto final_cleanup;
    } else {
        if (!ctx->StdoutOnly)
            LOG_I("%.*s", (int)content->Length, content->Buffer);
        ret = 0;
    }

final_cleanup:
    return ret;
}

void DependsParserDeinit(DependsParserContext *ctx) {
    ByteBufferDeinit(&ctx->SrcContent);
    AStringDeinit(&ctx->Target);
    ByteBufferDeinit(&ctx->DependList);
}

#pragma endregion[DependsParser]

int WriteDepfile(const Args *args, const DependsParserContext *dep_ctx) {
    const AStringView target_postfix = A_STRING_VIEW_LITERAL(":");
    const AStringView depend_prefix = A_STRING_VIEW_LITERAL(" ");
    const AStringView line_end = A_STRING_VIEW_LITERAL("\n");
    ByteBuffer content = {0};
    AString depfile_path = {0};
    AStringView view;
    const char *start, *end;
    bool changed = false;
    int ret = -1;

    if (!ArgsHasDepfile(args))
        return 0;
    view = AStringViewFromBuffer(dep_ctx->Target.Buffer, dep_ctx->Target.Length);
    if (ByteBufferCatAStringViews(&content, 2, &view, &target_postfix))
        goto cleanup;
    start = dep_ctx->DependList.Buffer;
    end = start + dep_ctx->DependList.Size;
    while (start != end) {
        view = AStringViewFromCString(start);
        if (ByteBufferCatAStringViews(&content, 2, &depend_prefix, &view))
            goto cleanup;
        start += view.Length + 1;
    }
    if (ByteBufferCatAStringViews(&content, 1, &line_end))
        goto cleanup;
    if (AStringViewEqual(&args->DepfilePath, "-")) {
        if (fwrite(content.Buffer, 1, content.Size, stdout) != content.Size)
            goto cleanup;
    } else {
        if (NormalizeAPath(&args->DepfilePath, &depfile_path))
            goto cleanup;
        if (FileWriteIfChanged(depfile_path.Buffer, content.Buffer,
                                             content.Size, &changed))
            goto cleanup;
        if (changed)
            LOG_I("depfile save succeeded; see %s\n", depfile_path.Buffer);
    }
    ret = 0;
cleanup:
    ByteBufferDeinit(&content);
    AStringDeinit(&depfile_path);
    return ret;
}

#pragma region "Command Options"

enum { OptionClassGroup = 1 };

struct OptionSpec {
    uint32_t Classification;
    const char *Name;
    uint8_t NumValues;
    const char *ValueName;
    const char *Description;
};

typedef struct BackendSpec {
    const char *Name;
    uint8_t Backend;
    const char *Description;
} BackendSpec;

#define OPTION_GROUP_MASK UINT32_C(0x000000FF)
#define OPTION_BACKEND_MASK_VALUE UINT32_C(0x0000FF00)
#define OPTION_RESERVED_MASK UINT32_C(0xFFFF0000)
#define OPTION_CLASS(group, backends) ((uint32_t)(group) | ((uint32_t)(backends) << 8))
#define GROUP(backends, description) \
    {OPTION_CLASS(OptionClassGroup, backends), NULL, 0, NULL, description}
#define OPTION_FOR_BACKENDS(backends, name, values, value_name, description) \
    {OPTION_CLASS(0, backends), name, values, value_name, description}
#define OPTION(name, values, value_name, description) \
    {UINT32_C(0) | UINT32_C(0x0000FF00), name, values, value_name, description}

static const OptionSpec OptionSpecs[] = {
    GROUP(BackendFxc | BackendDxc | BackendSpirv, "ShaderTool control"),
    OPTION("-compiler", 1, "PATH", "compiler executable"),
    OPTION("-depfile", 1, "PATH|-", "write canonical dependencies"),
    OPTION("-h", 0, NULL, "show this help"),
    OPTION("--help", 0, NULL, "show this help"),
    OPTION("-spirv", 0, NULL, "reserved; select SPIR-V with the spirv subcommand"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Ignored compiler dependency options"),
    OPTION("-M", 0, NULL, "ignored compiler dependency generation option"),
    OPTION("-MD", 0, NULL, "ignored compiler dependency generation option"),
    OPTION("-MF", 1, "PATH", "ignored compiler depfile option"),
    OPTION("-Vi", 0, NULL, "ignored compiler include-trace option"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Preprocessor and include paths"),
    OPTION("-D", 1, "NAME[=VALUE]", "define a macro"),
    OPTION("-I", 1, "DIRECTORY", "add an include directory"),
    GROUP(BackendDxc | BackendSpirv, "DXC preprocessor behavior"),
    OPTION("-encoding", 1, "ENCODING", "select source encoding"),
    OPTION("-flegacy-macro-expansion", 0, NULL, "use legacy macro expansion"),
    OPTION("-ignore-line-directives", 0, NULL, "ignore line directives"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Entry point and shader profile"),
    OPTION("-E", 1, "ENTRY", "select the entry point"),
    OPTION("-T", 1, "PROFILE", "select the shader profile"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Compiler outputs"),
    OPTION("-Fc", 1, "FILE", "write assembly text"),
    OPTION("-Fd", 1, "FILE", "write debug information"),
    OPTION("-Fe", 1, "FILE", "write warnings and errors"),
    OPTION("-Fh", 1, "FILE", "write a hexadecimal header"),
    OPTION("-Fo", 1, "FILE", "write compiled shader binary"),
    OPTION("-Vn", 1, "NAME", "set header variable name"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Optimization and flow control"),
    OPTION("-Gfa", 0, NULL, "prefer flow-control constructs"),
    OPTION("-Gfp", 0, NULL, "prefer unrolled shaders"),
    OPTION("-Od", 0, NULL, "disable optimizations"),
    OPTION("-O0", 0, NULL, "optimization level 0"),
    OPTION("-O1", 0, NULL, "optimization level 1"),
    OPTION("-O2", 0, NULL, "optimization level 2"),
    OPTION("-O3", 0, NULL, "optimization level 3"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Validation and language behavior"),
    OPTION("-Gec", 0, NULL, "enable backward compatibility"),
    OPTION("-Ges", 0, NULL, "enable strict mode"),
    OPTION("-Gis", 0, NULL, "force IEEE strictness"),
    OPTION("-Vd", 0, NULL, "disable validation"),
    OPTION("-WX", 0, NULL, "treat warnings as errors"),
    OPTION("-Zpc", 0, NULL, "pack matrices in column-major order"),
    OPTION("-Zpr", 0, NULL, "pack matrices in row-major order"),
    GROUP(BackendDxc | BackendSpirv, "DXC validation and language behavior"),
    OPTION("-default-linkage", 1, "MODE", "set default linkage"),
    OPTION("-denorm", 1, "MODE", "select denormal handling"),
    OPTION("-disable-payload-qualifiers", 0, NULL, "disable payload access qualifiers"),
    OPTION("-enable-payload-qualifiers", 0, NULL, "enable payload access qualifiers"),
    OPTION("-enable-16bit-types", 0, NULL, "enable native 16-bit types"),
    OPTION("-enable-lifetime-markers", 0, NULL, "enable lifetime markers"),
    OPTION("-fdisable-loc-tracking", 0, NULL, "disable source-location tracking"),
    OPTION("-flegacy-resource-reservation", 0, NULL, "use legacy resource reservation"),
    OPTION("-fnew-inlining-behavior", 0, NULL, "use new inlining behavior"),
    OPTION("-HV", 1, "YEAR", "select HLSL language version"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Debug information"),
    OPTION("-Zi", 0, NULL, "enable debug information"),
    GROUP(BackendDxc | BackendSpirv, "DXC debug information"),
    OPTION("-Qembed_debug", 0, NULL, "embed debug information"),
    OPTION("-Qsource_in_debug_module", 0, NULL, "embed source in debug module"),
    OPTION("-Zsb", 0, NULL, "compute debug name from binary"),
    OPTION("-Zss", 0, NULL, "compute debug name from source"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Diagnostics and presentation"),
    OPTION("-Cc", 0, NULL, "color-code assembly output"),
    OPTION("-Lx", 0, NULL, "output hexadecimal literals"),
    OPTION("-Ni", 0, NULL, "number assembly instructions"),
    OPTION("-No", 0, NULL, "output instruction byte offsets"),
    OPTION("-nologo", 0, NULL, "suppress the compiler banner"),
    OPTION("-no-warnings", 0, NULL, "suppress warnings"),
    GROUP(BackendDxc | BackendSpirv, "DXC diagnostics and presentation"),
    OPTION_FOR_BACKENDS(BackendDxc | BackendSpirv, "-H", 0, NULL, "display include hierarchy"),
    OPTION("-Zs", 0, NULL, "perform syntax checking only"),
    OPTION("-fdiagnostics-show-option", 0, NULL, "show diagnostic option names"),
    OPTION("-fno-diagnostics-show-option", 0, NULL, "hide diagnostic option names"),
    OPTION("-ftime-report", 0, NULL, "print compilation timing"),
    OPTION("-ftime-trace", 0, NULL, "write a time trace"),
    OPTION("-verbose", 0, NULL, "enable verbose compiler output"),
    OPTION("-fdiagnostics-format=", 0, "FORMAT", "select diagnostic format"),
    OPTION("-ftime-trace=", 0, "FILE", "select time-trace output"),
    OPTION("-ftime-trace-granularity=", 0, "MICROSECONDS", "set time-trace granularity"),

    GROUP(BackendDxc | BackendSpirv, "Linking, exports, and root signatures"),
    OPTION("-auto-binding-space", 1, "SPACE", "set automatic binding space"),
    OPTION("-export-shaders-only", 0, NULL, "export shaders only"),
    OPTION("-exports", 1, "EXPORTS", "select exports"),
    OPTION("-force-rootsig-ver", 1, "VERSION", "force root-signature version"),
    OPTION("-Fre", 1, "FILE", "write reflection data"),
    OPTION("-Frs", 1, "FILE", "write root signature"),
    OPTION("-Fsh", 1, "FILE", "write shader hash"),
    OPTION("-pack-optimized", 0, NULL, "optimize signature packing"),
    OPTION("-pack-prefix-stable", 0, NULL, "keep signature prefixes stable"),
    OPTION("-rootsig-define", 1, "MACRO", "select root-signature macro"),

    GROUP(BackendFxc | BackendDxc | BackendSpirv, "Resource assumptions"),
    OPTION("-all-resources-bound", 0, NULL, "assume all resources are bound"),
    OPTION("-res-may-alias", 0, NULL, "allow resource aliasing"),

    GROUP(BackendSpirv, "SPIR-V binding remapping"),
    OPTION("-fvk-auto-shift-bindings", 0, NULL, "automatically shift Vulkan bindings"),
    OPTION("-fvk-b-shift", 2, "SHIFT SPACE", "shift constant-buffer bindings"),
    OPTION("-fvk-bind-counter-heap", 2, "BINDING SET", "bind counter heap"),
    OPTION("-fvk-bind-globals", 2, "BINDING SET", "bind globals"),
    OPTION("-fvk-bind-register", 4, "TYPE NUMBER SPACE SET", "remap a register"),
    OPTION("-fvk-bind-resource-heap", 2, "BINDING SET", "bind resource heap"),
    OPTION("-fvk-bind-sampler-heap", 2, "BINDING SET", "bind sampler heap"),
    OPTION("-fvk-s-shift", 2, "SHIFT SPACE", "shift sampler bindings"),
    OPTION("-fvk-t-shift", 2, "SHIFT SPACE", "shift texture bindings"),
    OPTION("-fvk-u-shift", 2, "SHIFT SPACE", "shift UAV bindings"),

    GROUP(BackendSpirv, "SPIR-V memory layout"),
    OPTION("-fspv-flatten-resource-arrays", 0, NULL, "flatten resource arrays"),
    OPTION("-fspv-use-legacy-buffer-matrix-order", 0, NULL,
           "use legacy buffer matrix order"),
    OPTION("-fspv-use-unknown-image-format", 0, NULL,
           "allow unknown storage image formats"),
    OPTION("-fspv-use-vulkan-memory-model", 0, NULL, "use Vulkan memory model"),
    OPTION("-fvk-invert-y", 0, NULL, "invert the Vulkan Y axis"),
    OPTION("-fvk-support-nonzero-base-instance", 0, NULL, "support nonzero base instance"),
    OPTION("-fvk-support-nonzero-base-vertex", 0, NULL, "support nonzero base vertex"),
    OPTION("-fvk-use-dx-layout", 0, NULL, "use DirectX memory layout"),
    OPTION("-fvk-use-dx-position-w", 0, NULL, "use DirectX position W"),
    OPTION("-fvk-use-gl-layout", 0, NULL, "use OpenGL memory layout"),
    OPTION("-fvk-use-scalar-layout", 0, NULL, "use scalar memory layout"),

    GROUP(BackendSpirv, "SPIR-V code generation, debug, and optimization"),
    OPTION("-fspv-enable-maximal-reconvergence", 0, NULL, "enable maximal reconvergence"),
    OPTION("-fspv-max-id", 2, "ID SHIFT", "set SPIR-V maximum ID"),
    OPTION("-fspv-preserve-bindings", 0, NULL, "preserve resource bindings"),
    OPTION("-fspv-preserve-interface", 0, NULL, "preserve entry-point interface"),
    OPTION("-fspv-print-all", 0, NULL, "print SPIR-V after every pass"),
    OPTION("-fspv-reduce-load-size", 0, NULL, "reduce SPIR-V load size"),
    OPTION("-fspv-reflect", 0, NULL, "emit reflection decorations"),
    OPTION("-fspv-debug=", 0, "MODE", "select SPIR-V debug information"),
    OPTION("-fspv-entrypoint-name=", 0, "NAME", "set SPIR-V entry-point name"),
    OPTION("-fspv-extension=", 0, "EXTENSION", "enable a SPIR-V extension"),
    OPTION("-fspv-target-env=", 0, "ENVIRONMENT", "select SPIR-V target environment"),
    OPTION("-Oconfig=", 0, "PASSES", "select SPIR-V optimization passes")};

static const BackendSpec BackendSpecs[] = {
    {"fxc", BackendFxc, "compile DXBC with FXC (Shader Model 5.0 or 5.1)"},
    {"dxc", BackendDxc, "compile DXIL with DXC (Shader Model 6.0 or newer)"},
    {"spirv", BackendSpirv, "compile SPIR-V with DXC (Shader Model 6.0 or newer)"}};

#undef GROUP
#undef OPTION
#undef OPTION_FOR_BACKENDS

#define countof(A) sizeof(A) / sizeof(A[0])

const BackendSpec *FindBackendSpec(const char *name) {
    size_t i;
    for (i = 0; i < sizeof(BackendSpecs) / sizeof(BackendSpecs[0]); ++i)
        if (!strcmp(name, BackendSpecs[i].Name))
            return &BackendSpecs[i];
    return NULL;
}

static uint8_t OptionBackendMask(const OptionSpec *spec) {
    return (uint8_t)((spec->Classification & OPTION_BACKEND_MASK_VALUE) >> 8);
}

static bool OptionSpecIsGroup(const OptionSpec *spec) {
    return (spec->Classification & OPTION_GROUP_MASK) != 0;
}

static bool OptionSpecSupportsBackends(const OptionSpec *group, const OptionSpec *spec,
                                       uint32_t target_backends) {
    return (target_backends & ((group->Classification & spec->Classification) >> 8)) != 0;
}

static bool OptionIsIgnoredDependencyControl(const OptionSpec *spec) {
    return !strcmp(spec->Name, "-M") || !strcmp(spec->Name, "-MD") ||
           !strcmp(spec->Name, "-MF") || !strcmp(spec->Name, "-Vi");
}

static void PrintBackends(void) {
    size_t i;
    LOG_I("\backend_count:\n");
    for (i = 0; i < sizeof(BackendSpecs) / sizeof(BackendSpecs[0]); ++i)
        LOG_I("  %-10s %s\n", BackendSpecs[i].Name, BackendSpecs[i].Description);
}

void Usage(const char *prog, const BackendSpec *backend) {
    const size_t count = sizeof(OptionSpecs) / sizeof(OptionSpecs[0]);
    const uint32_t target_backends = backend ? backend->Backend : 0;
    size_t i = 0;
    LOG_I("Usage: %s <fxc|dxc|spirv> -compiler <path> [options] <source>\n",
         prog ? prog : "ShaderTool");
    PrintBackends();
    if (!backend)
        return;
    while (i < count) {
        const OptionSpec *group = &OptionSpecs[i++];
        bool printed_group = false;
        while (i < count && !OptionSpecIsGroup(&OptionSpecs[i])) {
            const OptionSpec *spec = &OptionSpecs[i++];
            if (!OptionSpecSupportsBackends(group, spec, target_backends))
                continue;
            if (!printed_group) {
                LOG_I("\n%s:\n", group->Description);
                printed_group = true;
            }
            if (spec->ValueName) {
                LOG_I("  %-20s %-14s %s\n", spec->Name, spec->ValueName, spec->Description);
            } else {
                LOG_I("  %-35s %s\n", spec->Name, spec->Description);
            }
        }
    }
}

int ParseProfile(const AStringView *profile, uint32_t *major, uint32_t *minor) {
    char buffer[64], *first, *second, *end;
    unsigned long parsed_major, parsed_minor;
    if (profile->Length >= sizeof(buffer))
        return -1;
    memcpy(buffer, profile->Buffer, profile->Length);
    buffer[profile->Length] = 0;
    first = strchr(buffer, '_');
    second = first ? strchr(first + 1, '_') : NULL;
    if (!first || first == buffer || first - buffer > 15 || !second ||
        second == first + 1 || !second[1])
        return -1;
    *second = 0;
    errno = 0;
    parsed_major = strtoul(first + 1, &end, 10);
    if (errno == ERANGE || *end || parsed_major > UINT32_MAX)
        return -1;
    errno = 0;
    parsed_minor = strtoul(second + 1, &end, 10);
    if (errno == ERANGE || *end || parsed_minor > UINT32_MAX)
        return -1;
    *major = (uint32_t)parsed_major;
    *minor = (uint32_t)parsed_minor;
    return 0;
}

typedef struct OptionContext {
    int Argc;
    const char **Argv;
    size_t Index;
    const BackendSpec *Backend;
} OptionContext;

enum OptionFetchResult {
    OptionFetchError = -1,
    OptionFetchEnd = 0,
    OptionFetchOption = 1,
    OptionFetchOperand = 2
};

static void OptionContextInit(OptionContext *context, int argc, const char **argv,
                              const BackendSpec *backend) {
    context->Argc = argc;
    context->Argv = argv;
    context->Index = 2;
    context->Backend = backend;
}

static enum OptionFetchResult OptionContextFetch(OptionContext *context,
                                                 ParsedOption *option) {
    const OptionSpec *spec;
    const char *argument;
    size_t i, j;
    const size_t count = countof(OptionSpecs);

    if (context->Index >= (size_t)context->Argc)
        return OptionFetchEnd;
    argument = context->Argv[context->Index++];
    memset(option, 0, sizeof(*option));
    option->Spelling = AStringViewFromCString(argument);
    if (argument[0] != '-')
        return OptionFetchOperand;

    for(i = 0; i < count;) {
        const OptionSpec *group = &OptionSpecs[i++];
        while(i < count && !OptionSpecIsGroup(&OptionSpecs[i])) {
            const OptionSpec *spec = &OptionSpecs[i++];
            if(OptionSpecSupportsBackends(group, spec, context->Backend->Backend)) {
                if(spec->NumValues != 1) {
                    if(strcmp(spec->Name, argument) != 0)
                        continue;

                    if(spec->NumValues > (size_t)context->Argc - context->Index) {
                        LOG_E("option '%s' requires %u value(s)\n", argument,
                              spec->NumValues);
                        return OptionFetchError;
                    }
                    option->Spec = spec;
                    for (j = 0; j < spec->NumValues; ++j)
                        option->Values[j] =
                            AStringViewFromCString(context->Argv[context->Index++]);
                    return OptionFetchOption;
                } else {
                    /* Only one value */
                    size_t spec_len = strlen(spec->Name);
                    if(spec_len > 0 && spec->Name[spec_len - 1] == '=') {
                        if(strncmp(spec->Name, argument, spec_len) != 0)
                            continue;

                        if(argument[spec_len] == 0) {
                            LOG_E("option '%s' requires a value");
                            return OptionFetchError;
                        }
                        option->Spec = spec;
                        option->Values[0] = AStringViewFromCString(&argument[spec_len]);
                    } else {
                        if (strncmp(spec->Name, argument, spec_len) != 0)
                            continue;

                        if (argument[spec_len] == 0) {
                            if((size_t)context->Argc - context->Index == 0) {
                                LOG_E("option '%s' requires a value");
                                return OptionFetchError;
                            }
                            option->Spec = spec;
                            option->Values[0] = AStringViewFromCString(context->Argv[context->Index++]);
                        } else {
                            option->Spec = spec;
                            option->Values[0] = AStringViewFromCString(&argument[spec_len]);
                        }
                        return OptionFetchOption;
                    }
                }
            }
        }
    }

    LOG_E("unknown or non-compilation option '%s'\n", argument);
    return OptionFetchError;
}

int ParseCommandLine(int argc, const char **argv, Args *args) {
    OptionContext context;
    enum OptionFetchResult fetched;
    const BackendSpec *backend;
    const OptionSpec *spec;
    ParsedOption option;
    size_t compiler_count = 0, depfile_count = 0, profile_count = 0, source_count = 0;
    uint32_t major, minor;
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        Usage(argv[0], NULL);
        return 1;
    }
    if (argc < 2) {
        LOG_E("a backend subcommand is required\n");
        return -1;
    }
    backend = FindBackendSpec(argv[1]);
    if (!backend) {
        LOG_E("Unknown ShaderTool backend '%s'\n", argv[1]);
        return -1;
    }
    args->Backend = backend->Backend;
#if !defined(_WIN32) && !defined(SHADERTOOL_ALLOW_FXC)
    if (args->Backend == BackendFxc) {
        LOG_E("the fxc backend is unsupported on native Unix\n");
        return -1;
    }
#endif
    OptionContextInit(&context, argc, argv, backend);
    while ((fetched = OptionContextFetch(&context, &option)) != OptionFetchEnd) {
        if (fetched == OptionFetchError)
            return -1;
        if (fetched == OptionFetchOperand) {
            ++source_count;
            if (source_count != 1 || context.Index != (size_t)argc) {
                LOG_E("exactly one source file must be the final argument\n");
                return -1;
            }
            args->SourceFilePath = option.Spelling;
            continue;
        }
        spec = option.Spec;
        if (!strcmp(spec->Name, "-h") || !strcmp(spec->Name, "--help")) {
            Usage(argv[0], backend);
            return 1;
        }
        if (!strcmp(spec->Name, "-compiler")) {
            if (++compiler_count != 1) {
                LOG_E("-compiler may be specified exactly once\n");
                return -1;
            }
            args->CompilerProgram = option.Values[0];
            continue;
        }
        if (!strcmp(spec->Name, "-depfile")) {
            if (++depfile_count != 1) {
                LOG_E("-depfile may be specified at most once\n");
                return -1;
            }
            args->DepfilePath = option.Values[0];
            continue;
        }
        if (OptionIsIgnoredDependencyControl(spec))
            continue;
        if (!strcmp(spec->Name, "-spirv")) {
            if (args->Backend == BackendFxc)
                LOG_E("-spirv is not valid for FXC\n");
            else if (args->Backend == BackendSpirv)
                LOG_E("-spirv is implicit for the spirv subcommand\n");
            else
                LOG_E("-spirv is only selected by the spirv subcommand\n");
            return -1;
        }
        if (!strcmp(spec->Name, "-Fo") || !strcmp(spec->Name, "-Fh")) {
            args->OutputFilePath = option.Values[0];
            args->HeaderOutput = !strcmp(spec->Name, "-Fh");
        } else if (!strcmp(spec->Name, "-E"))
            args->EntryPointName = option.Values[0];
        else if (!strcmp(spec->Name, "-Vn"))
            args->HeaderVariableName = option.Values[0];
        else if (!strcmp(spec->Name, "-T")) {
            ++profile_count;
            if (ParseProfile(&option.Values[0], &major, &minor)) {
                LOG_E("invalid shader profile '%.*s'\n", (int)option.Values[0].Length,
                     option.Values[0].Buffer);
                return -1;
            }
            if (args->Backend == BackendFxc) {
                if (major != 5 || minor > 1) {
                    LOG_E("FXC requires shader profile 5_0 or 5_1\n");
                    return -1;
                }
            } else if (major < 6) {
                LOG_E("DXC requires shader profile 6_0 or newer\n");
                return -1;
            }
        }
        if (ArgsAppendCompilerOption(args, &option))
            return -1;
    }
    if (compiler_count != 1) {
        LOG_E("-compiler is required\n");
        return -1;
    }
    if (source_count != 1) {
        LOG_E("exactly one source file is required\n");
        return -1;
    }
    if (!profile_count) {
        LOG_E("-T <profile> is required\n");
        return -1;
    }
    if (ArgsHasDepfile(args) && AStringViewIsEmpty(&args->OutputFilePath)) {
        LOG_E("-depfile requires a compilation output selected by -Fo or -Fh\n");
        return -1;
    }
    return 0;
}
#pragma endregion "Command Options"

static int MarshalOutputHeader(const Args *args) {
    static const char fxc_type[] = "BYTE";
    static const char portable_type[] = "unsigned char";
    ByteBuffer source = {0}, output = {0}, suffix = {0};
    AString declaration = {0};
    AStringView suffix_toks[3], declaration_toks[2];
    AStringView variable, var_prefix;
    const char *type;
    int ret = -1;

    if (!args->HeaderOutput)
        return 0;

    if(!AStringViewIsEmpty(&args->HeaderVariableName)) {
        variable = args->HeaderVariableName;
        var_prefix = A_STRING_VIEW_LITERAL("");
    } else if(!AStringViewIsEmpty(&args->EntryPointName)) {
        variable = args->EntryPointName;
        var_prefix = A_STRING_VIEW_LITERAL("g_");
    } else {
        variable = A_STRING_VIEW_LITERAL("g_main");
        var_prefix = A_STRING_VIEW_LITERAL("");
    }

    suffix_toks[0] = A_STRING_VIEW_LITERAL("\nconst unsigned int ");
    suffix_toks[1] = A_STRING_VIEW_LITERAL("_size = sizeof(");
    suffix_toks[2] = A_STRING_VIEW_LITERAL(");\n");

    if (ByteBufferCatAStringViews(&suffix, 7, &suffix_toks[0], &var_prefix, &variable,
                                  &suffix_toks[1], &var_prefix, &variable, &suffix_toks[2]))
        goto cleanup;

    if (args->Backend != BackendFxc) {
        ret = FileWriteAll(args->OutputFilePath.Buffer, suffix.Buffer,
                                             suffix.Size, true);
        goto cleanup;
    }

    /* FXC backend */
    if (FileReadAll(args->OutputFilePath.Buffer, &source))
        goto cleanup;

    declaration_toks[0] = A_STRING_VIEW_LITERAL("const BYTE ");
    declaration_toks[1] = A_STRING_VIEW_LITERAL("[]");
    if (AStringCatN(&declaration, 4, &declaration_toks[0], &var_prefix, &variable,
                    &declaration_toks[1]))
        goto cleanup;
    type = strstr(source.Buffer, declaration.Buffer);
    if (!type) {
        LOG_E("ShaderTool: FXC header declaration '%s' was not found\n",
             declaration.Buffer);
        goto cleanup;
    }
    type += sizeof("const ") - 1;
    if (ByteBufferWrite(&output, source.Buffer, (size_t)(type - source.Buffer)) ||
        ByteBufferWrite(&output, portable_type, sizeof(portable_type) - 1) ||
        ByteBufferWrite(&output, type + sizeof(fxc_type) - 1,
                        source.Size - (size_t)(type - source.Buffer) -
                            (sizeof(fxc_type) - 1)))
        goto cleanup;
    if (ByteBufferWrite(&output, suffix.Buffer, suffix.Size))
        goto cleanup;
    if (FileWriteAll(args->OutputFilePath.Buffer, output.Buffer, output.Size,
                                    false))
        goto cleanup;
    ret = 0;
cleanup:
    AStringDeinit(&declaration);
    ByteBufferDeinit(&suffix);
    ByteBufferDeinit(&output);
    ByteBufferDeinit(&source);
    return ret;
}

typedef struct CompilerCommand {
    char **Arguments;
    char *RewrittenOptions;
} CompilerCommand;

static void CompilerCommandDeinit(CompilerCommand *command) {
    free(command->Arguments);
    memset(command, 0, sizeof(*command));
}

static const char *FxcRewriteName(const OptionSpec *spec) {
    if (!strcmp(spec->Name, "-all-resources-bound"))
        return "/all_resources_bound";
    if (!strcmp(spec->Name, "-res-may-alias"))
        return "/res_may_alias";
    return NULL;
}

static int ComposeCompilerCommand(const Args *args, CompilerCommand *command) {
    size_t argument_count = 2, rewrite_size = 0, pointer_bytes;
    size_t argument_index = 0, rewrite_offset = 0, i, j, option_size;
    bool has_include_trace = false;
    const ParsedOption *parsed;
    const char *rewrite_name;

    for (i = 0; i < args->CompilerOptions.Size; ++i) {
        parsed = &args->CompilerOptions.Buffer[i];
        if (args->Backend != BackendFxc && !strcmp(parsed->Spec->Name, "-H"))
            has_include_trace = true;
        argument_count += 1 + parsed->Spec->NumValues; /* option [values] */
        if (args->Backend == BackendFxc) {
            rewrite_name = FxcRewriteName(parsed->Spec);
            option_size = rewrite_name ? strlen(rewrite_name) : strlen(parsed->Spec->Name);
            rewrite_size += option_size + 1;
        }
    }
    if (ArgsHasDepfile(args) && !has_include_trace)
        ++argument_count; /* -MF equivallent */
    if (args->Backend == BackendSpirv)
        ++argument_count; /* -spirv */
    argument_count += 1; /* terminate NULL */
    pointer_bytes = argument_count * sizeof(char *);

    command->Arguments = (char **)calloc(1, pointer_bytes + rewrite_size);
    if (!command->Arguments) {
        LOG_E("Failed to allocate compiler command arguments\n");
        return -1;
    }
    if(rewrite_size)
        command->RewrittenOptions = (char *)command->Arguments + pointer_bytes;

    command->Arguments[argument_index++] = (char *)args->CompilerProgram.Buffer;
    if (ArgsHasDepfile(args) && !has_include_trace)
        command->Arguments[argument_index++] = args->Backend == BackendFxc ? "/Vi" : "-H";
    if (args->Backend == BackendSpirv)
        command->Arguments[argument_index++] = "-spirv";

    for (i = 0; i < args->CompilerOptions.Size; ++i) {
        parsed = &args->CompilerOptions.Buffer[i];
        if (args->Backend == BackendFxc) {
            char *rewritten = command->RewrittenOptions + rewrite_offset;
            rewrite_name = FxcRewriteName(parsed->Spec);
            if (rewrite_name) {
                option_size = strlen(rewrite_name);
                memcpy(rewritten, rewrite_name, option_size + 1);
            } else {
                option_size = strlen(parsed->Spec->Name);
                memcpy(rewritten, parsed->Spec->Name, option_size);
                rewritten[option_size] = 0;
                if (option_size && rewritten[0] == '-')
                    rewritten[0] = '/';
            }
            command->Arguments[argument_index++] = rewritten;
            rewrite_offset += option_size + 1;
        } else {
            command->Arguments[argument_index++] = (char *)parsed->Spec->Name;
        }
        for (j = 0; j < parsed->Spec->NumValues; ++j)
            command->Arguments[argument_index++] = (char *)parsed->Values[j].Buffer;
    }
    command->Arguments[argument_index++] = (char *)args->SourceFilePath.Buffer;
    command->Arguments[argument_index] = NULL;
    return 0;
}

static int CompilerOutput(void *context, const char *bytes, size_t size, bool final) {
    AStringView view;
    view = AStringViewFromBuffer(bytes, size);
    return DependsParserInput((DependsParserContext *)context, &view, final);
}

int RunCompiler(const Args *args, int *exit_code) {
    CompilerCommand command = {0};
    DependsParserContext parser = {0};
    int ret = -1;
    if (DependsParserInit(&parser, args))
        goto cleanup;
    if (ComposeCompilerCommand(args, &command))
        goto cleanup;
    if (ProgramRun(command.Arguments, ArgsHasDepfile(args),
                              CompilerOutput, &parser, exit_code))
        goto cleanup;
    if (*exit_code == 0 && MarshalOutputHeader(args))
        goto cleanup;
    if (WriteDepfile(args, &parser))
        goto cleanup;
    ret = 0;
cleanup:
    DependsParserDeinit(&parser);
    CompilerCommandDeinit(&command);
    return ret;
}

int main(int argc, char *argv[]) {
    Args args = {0};
    int compiler_exit_code;
    int rc;
    int ret;

    ret = 1;

    rc = ParseCommandLine(argc, (const char **)argv, &args);
    if (rc == 1) {
        ret = 0;
        goto final_cleanup;
    } else if (rc)
        goto final_cleanup;

    if (RunCompiler(&args, &compiler_exit_code))
        goto final_cleanup;

    ret = compiler_exit_code;

final_cleanup:
    ArgsDeinit(&args);

    return ret;
}
