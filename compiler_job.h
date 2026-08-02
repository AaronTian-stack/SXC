#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/container/small_vector.hpp>

#include <oneapi/tbb/concurrent_vector.h>

#include <qhenki/rhi/shader.h>
#include <qhenki/rhi/shader_compiler.h>

namespace qhenki::sxc
{
namespace fs = std::filesystem;

struct CLIInput
{
    std::string config_path;
    std::string output_dir;
    std::span<const std::string> global_defines;
    std::span<const std::string> include_paths;
    gfx::ShaderModel shader_model;
    CompilerInput::Optimization optimization;
    bool embed_debug;
    bool force_recompile;
    ShaderIR output_IR;
};

struct CompilerInputFile
{
    CompilerInput input;
    std::filesystem::path output_path;
};

struct OutputInfo
{
    gfx::ShaderModel sm;
    gfx::ShaderType st;
    std::string_view entry_point;
};

using CompilerInputVector = boost::container::small_vector<CompilerInput, 1>;

class SXCJob
{
    struct ConfigLineInput
    {
        std::optional<std::string> output_dir; // Overrides CLIInput output directory
        std::string entry_point;
        std::optional<std::vector<std::string>> defines;         // List of range of possible range of values
        std::optional<CompilerInput::Optimization> optimization; // Overrides CLIInput optimization
        gfx::ShaderType shader_type;
    };

    static gfx::ShaderType to_shader_type(const char* str);
    static const char* shader_type_to_str(gfx::ShaderType type);

public:
    static fs::path get_resolved_output_name(const OutputInfo& info,
                                             const fs::path& input_path,
                                             const std::string& output_dir,
                                             ShaderIR ir_format);
    static int parse_config(const CLIInput& input, tbb::concurrent_vector<CompilerInputVector>* compiler_inputs);
};

struct ShaderResultCount
{
    uint64_t succeeded_count;
    uint64_t failed_count;
    uint64_t skipped_count;
};

ShaderResultCount execute_compilation_job(tbb::concurrent_vector<CompilerInputVector>* inputs,
                                          const std::string& output_dir,
                                          bool force,
                                          ShaderIR ir_format);
} // namespace qhenki::sxc
