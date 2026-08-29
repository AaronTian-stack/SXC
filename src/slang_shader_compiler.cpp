#include "slang_shader_compiler.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include <magic_enum/magic_enum.hpp>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#include <climits>
#endif

using namespace SXC;

namespace
{
constexpr std::array<const char*, 17> SPIRV_OPTIONS{
    "-fvk-use-dx-layout",
    "-fvk-use-dx-position-w",
    "-fspv-reflect",
    "-fvk-b-shift",
    "0",
    "all",
    "-fvk-s-shift",
    "0",
    "all",
    "-fvk-t-shift",
    "0",
    "all",
    "-fvk-u-shift",
    "0",
    "all",
    "-fvk-use-entrypoint-name",
    "-fvk-invert-y",
};

constexpr size_t SHADER_PROFILE_SIZE = sizeof("sm_X_Y");

std::array<char, SHADER_PROFILE_SIZE> shader_profile(const ShaderModel model)
{
    const auto enum_name = magic_enum::enum_name(model);
    assert(enum_name.size() + 1 == SHADER_PROFILE_SIZE);

    std::array<char, SHADER_PROFILE_SIZE> profile{};
    std::memcpy(profile.data(), enum_name.data(), enum_name.size());
    profile[0] = 's';
    profile[1] = 'm';
    return profile;
}

SlangStage shader_stage(const ShaderType type)
{
    switch (type)
    {
    case VERTEX_SHADER:
        return SLANG_STAGE_VERTEX;
    case PIXEL_SHADER:
        return SLANG_STAGE_FRAGMENT;
    case COMPUTE_SHADER:
        return SLANG_STAGE_COMPUTE;
    }
    // Library shaders don't have an entrypoint
    assert(false);
    return SLANG_STAGE_NONE;
}

SlangOptimizationLevel optimization_level(const CompilerInput::Optimization optimization)
{
    switch (optimization)
    {
    case CompilerInput::Optimization::O0:
        return SLANG_OPTIMIZATION_LEVEL_NONE;
    case CompilerInput::Optimization::O1:
        return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
    case CompilerInput::Optimization::O2:
        return SLANG_OPTIMIZATION_LEVEL_HIGH;
    case CompilerInput::Optimization::O3:
        return SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
    }
    return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
}

void set_diagnostics(SlangCompileRequest* request, CompilerOutput* output, const char* fallback)
{
    if (const auto diagnostics = spGetDiagnosticOutput(request); diagnostics && diagnostics[0] != '\0')
    {
        output->error_message = diagnostics;
    }
    else
    {
        output->error_message = fallback;
    }
}
} // namespace

SlangShaderCompiler::SlangShaderCompiler()
    : m_session(spCreateSession())
{
    if (!m_session)
    {
        fprintf(stderr, "SlangShaderCompiler: Failed to create Slang session\n");
        abort();
    }
}

SlangShaderCompiler::~SlangShaderCompiler()
{
    if (m_session)
    {
        spDestroySession(m_session);
    }
}

bool SlangShaderCompiler::get_compiler_path(char* buffer, const size_t length)
{
    if (!buffer || length == 0)
    {
        return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    HMODULE slang_module = nullptr;
    constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(&spCreateSession), &slang_module))
    {
        const auto result = GetModuleFileNameA(slang_module, buffer, static_cast<DWORD>(length));
        return result != 0 && result < length;
    }
    return false;
#elif defined(__APPLE__) || defined(__linux__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&spCreateSession), &info) == 0 || !info.dli_fname)
    {
        return false;
    }

    std::array<char, PATH_MAX> resolved_path{};
    if (!realpath(info.dli_fname, resolved_path.data()))
    {
        return false;
    }

    const auto path_length = std::strlen(resolved_path.data());
    if (path_length + 1 > length)
    {
        return false;
    }

    std::memcpy(buffer, resolved_path.data(), path_length + 1);
    return true;
#else
    return false;
#endif
}

bool SlangShaderCompiler::compile(const CompilerInput& input, CompilerOutput* output, const ShaderIR ir)
{
    assert(output);
    output->error_message.clear();
    output->blob.setNull();

    auto request = spCreateCompileRequest(m_session);
    if (!request)
    {
        output->error_message = "SlangShaderCompiler: Failed to create compile request";
        return false;
    }

    struct RequestGuard
    {
        SlangCompileRequest* request;
        ~RequestGuard()
        {
            spDestroyCompileRequest(request);
        }
    } request_guard{request};

    SlangCompileTarget target;
    switch (ir)
    {
    case ShaderIR::DXBC:
        target = SLANG_DXBC;
        break;
    case ShaderIR::DXIL:
        target = SLANG_DXIL;
        break;
    case ShaderIR::SPIRV:
        target = SLANG_SPIRV;
        break;
    default:
        output->error_message = "SlangShaderCompiler: Unsupported shader IR";
        return false;
    }
    const auto target_index = spAddCodeGenTarget(request, target);
    const auto profile_name = shader_profile(input.shader_model);
    if (profile_name[0] == '\0')
    {
        output->error_message = "SlangShaderCompiler: Unsupported shader model";
        return false;
    }

    const auto profile = spFindProfile(m_session, profile_name.data());
    if (profile == SLANG_PROFILE_UNKNOWN)
    {
        output->error_message = "SlangShaderCompiler: Slang does not support profile ";
        output->error_message += profile_name.data();
        return false;
    }
    spSetTargetProfile(request, target_index, profile);
    spSetMatrixLayoutMode(request, SLANG_MATRIX_LAYOUT_COLUMN_MAJOR);
    spSetOptimizationLevel(request, optimization_level(input.optimization));
    spSetDiagnosticFlags(request, spGetDiagnosticFlags(request) | SLANG_DIAGNOSTIC_FLAG_TREAT_WARNINGS_AS_ERRORS);

    if (input.flags & CompilerInput::DEBUG)
    {
        spSetDebugInfoLevel(request, SLANG_DEBUG_INFO_LEVEL_STANDARD);
        if (ir != ShaderIR::SPIRV)
        {
            spSetDebugInfoFormat(request, SLANG_DEBUG_INFO_FORMAT_C7);
        }
    }

    for (const auto& include : input.includes)
    {
        spAddSearchPath(request, include.c_str());
    }

    const auto input_path = std::filesystem::path(input.get_path());
    if (input_path.has_parent_path())
    {
        const auto input_directory = input_path.parent_path().string();
        spAddSearchPath(request, input_directory.c_str());
    }

    const auto defines = input.get_defines();
    for (const auto& define : defines)
    {
        const auto separator = define.find('=');
        if (separator == std::string::npos)
        {
            spAddPreprocessorDefine(request, define.c_str(), "1");
            continue;
        }

        const auto name = define.substr(0, separator);
        spAddPreprocessorDefine(request, name.c_str(), define.c_str() + separator + 1);
    }

    if (ir == ShaderIR::SPIRV)
    {
        const auto option_count = input.shader_type == VERTEX_SHADER ? static_cast<int>(SPIRV_OPTIONS.size())
                                                                     : static_cast<int>(SPIRV_OPTIONS.size() - 1);
        if (SLANG_FAILED(spProcessCommandLineArguments(request, SPIRV_OPTIONS.data(), option_count)))
        {
            set_diagnostics(request, output, "SlangShaderCompiler: Invalid SPIR-V compiler options");
            return false;
        }
    }

    const auto input_path_string = input_path.string();
    const auto source_language = input_path.extension() == ".slang" ? SLANG_SOURCE_LANGUAGE_SLANG
                                                                    : SLANG_SOURCE_LANGUAGE_HLSL;
    const auto translation_unit = spAddTranslationUnit(request, source_language, nullptr);
    spAddTranslationUnitSourceFile(request, translation_unit, input_path_string.c_str());

    const bool is_library = input.shader_type == LIBRARY_SHADER;
    int entry_point_index = -1;
    if (is_library)
    {
        spSetTargetFlags(request, target_index, SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM);
    }
    else
    {
        entry_point_index =
            spAddEntryPoint(request, translation_unit, input.entry_point.c_str(), shader_stage(input.shader_type));
    }

    if (SLANG_FAILED(spCompile(request)))
    {
        set_diagnostics(request, output, "SlangShaderCompiler: Compilation failed");
        return false;
    }

    const auto blob_result =
        is_library ? spGetTargetCodeBlob(request, target_index, output->blob.writeRef())
                   : spGetEntryPointCodeBlob(request, entry_point_index, target_index, output->blob.writeRef());
    if (SLANG_FAILED(blob_result) || !output->blob)
    {
        set_diagnostics(request, output, "SlangShaderCompiler: Failed to retrieve compiled bytecode");
        return false;
    }

    if (output->blob->getBufferSize() == 0 || !output->blob->getBufferPointer())
    {
        output->blob.setNull();
        set_diagnostics(request, output, "SlangShaderCompiler: Compiled bytecode is empty");
        return false;
    }
    return true;
}
