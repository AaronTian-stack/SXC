#pragma once

#include <span>
#include <string>
#include <variant>
#include <vector>

#include <slang-com-ptr.h>

namespace SXC
{
enum ShaderType : uint8_t
{
    VERTEX_SHADER,
    PIXEL_SHADER,
    COMPUTE_SHADER,
    LIBRARY_SHADER,
};

enum class ShaderModel
{
    SM_5_0,
    SM_6_0,
    SM_6_1,
    SM_6_2,
    SM_6_3,
    SM_6_4,
    SM_6_5,
    SM_6_6,
    SM_6_7,
    SM_6_8,
    SM_6_9,
};

struct NonOwning
{
    const std::string_view path;
    std::span<const std::string> defines;
};

struct Owning
{
    std::string path;
    std::vector<std::string> defines;
};

struct CompilerInput
{
    std::variant<NonOwning, Owning> path_and_defines;
    std::string entry_point = "main";
    std::span<const std::string> includes;
    ShaderModel shader_model;
    ShaderType shader_type;
    enum ShaderFlags : uint8_t
    {
        NONE = 0,
        DEBUG = 1,
    } flags = NONE;
    enum Optimization : uint8_t
    {
        O0,
        O1,
        O2,
        O3
    } optimization = O3;

    std::string_view get_path() const
    {
        switch (path_and_defines.index())
        {
        case 0: // NonOwning
            return std::get<NonOwning>(path_and_defines).path;
        case 1: // Owning
            return std::string_view{std::get<Owning>(path_and_defines).path};
        default:
            return {};
        }
    }

    std::span<const std::string> get_defines() const
    {
        switch (path_and_defines.index())
        {
        case 0: // NonOwning
            return std::get<NonOwning>(path_and_defines).defines;
        case 1: // Owning
        {
            const auto& defs = std::get<Owning>(path_and_defines).defines;
            return std::span(defs.data(), defs.size());
        }
        default:
            return {};
        }
    }
};

enum class ShaderIR : uint8_t
{
    DXBC,
    DXIL,
    SPIRV,
};

struct CompilerOutput
{
    std::string error_message;
    // TODO: Use own blob type?
    Slang::ComPtr<ISlangBlob> blob;
};

class ShaderCompiler
{
public:
    // Creates a source blob (DXIL, DXBC, or SPIR-V) from the input
    virtual bool compile(const CompilerInput& input, CompilerOutput* output, ShaderIR ir) = 0;
    virtual ~ShaderCompiler() = default;
};
} // namespace SXC
