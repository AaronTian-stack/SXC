#include "compiler_job.h"

#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_pipeline.h>
#include <tsl/robin_set.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>

#include <magic_enum/magic_enum.hpp>

#include <argparse/argparse.hpp>

#include "graphics/shared/slang_shader_compiler.h"

#include "qhenki/utility/shader_blob.h"
#include "qhenki/utility/shader_model_util.h"


using namespace qhenki::sxc;
using namespace qhenki::gfx;
using namespace qhenki::util;

namespace
{
std::string compute_defines_hash(const CompilerInputVector& inputs)
{
    constexpr std::hash<std::string> hasher;
    size_t combined_hash = 0;

    for (const auto& input : inputs)
    {
        const auto defines = input.get_defines();
        for (const auto& define : defines)
        {
            // Boost hash combine
            combined_hash ^= hasher(define) + 0x9e3779b9 + (combined_hash << 6) + (combined_hash >> 2);
        }
    }

    std::ostringstream oss;
    oss << std::hex << combined_hash;
    return oss.str();
}

bool write_meta_file(const fs::path& meta_path, const std::string& defines_hash)
{
    std::ofstream out(meta_path);
    if (!out.is_open())
    {
        return false;
    }
    out << defines_hash;
    return out.good();
}

bool check_meta_file(const fs::path& meta_path, const std::string& expected_hash)
{
    if (!fs::exists(meta_path))
    {
        return false;
    }

    std::ifstream in(meta_path);
    if (!in.is_open())
    {
        return false;
    }

    std::string stored_hash;
    if (!std::getline(in, stored_hash))
    {
        return false;
    }

    return stored_hash == expected_hash;
}

fs::file_time_type get_most_recent_time(const fs::path& file,
                                        tsl::robin_set<fs::path>& visited,
                                        std::span<const std::string> include_paths)
{
    if (!fs::exists(file))
    {
        printf("Include file not found: %s\n This may be a false positive if the include is in a disabled macro. \n",
               file.string().c_str());
        return fs::file_time_type::min();
    }

    if (!visited.insert(file).second)
    {
        return fs::file_time_type::min(); // Already visited this file
    }

    std::ifstream in(file);
    if (!in.is_open())
    {
        return fs::file_time_type::min();
    }

    fs::file_time_type latest = fs::last_write_time(file);

    std::string line;
    std::regex include_regex(R"(^\s*#\s*include\s*["<](.*)[">])");
    while (std::getline(in, line))
    {
        std::smatch match;
        if (std::regex_search(line, match, include_regex))
        {
            // Look in include paths
            fs::path include_file = file.parent_path() / match[1].str();

            // If not found relative to parent, search in include paths
            if (!fs::exists(include_file))
            {
                for (const auto& include_path : include_paths)
                {
                    fs::path candidate = fs::path(include_path) / match[1].str();
                    if (fs::exists(candidate))
                    {
                        include_file = candidate;
                        break;
                    }
                }
            }

            // Detect circular includes
            if (visited.find(include_file) != visited.end())
            {
                printf("Circular include detected: %s\n", include_file.string().c_str());
            }
            else
            {
                // Recursively get times of includes
                auto inc_time = get_most_recent_time(include_file, visited, include_paths);
                if (inc_time > latest)
                {
                    latest = inc_time;
                }
            }
        }
    }
    return latest;
}

bool needs_to_recompile_shader(const fs::path& input_path,
                               const fs::path& output_path,
                               const std::span<const std::string> include_paths,
                               const CompilerInputVector& inputs,
                               const bool force)
{
    if (force || !fs::exists(output_path))
    {
        return true;
    }

    // We only check file date and meta file. If something is wrong or does not match I expect user will force recompile

    // Check that the permutation hash file exists
    if (inputs.size() > 1)
    {
        fs::path meta_path = output_path.parent_path() / input_path.stem();
        meta_path += ".meta";

        const auto defines_hash = compute_defines_hash(inputs);
        if (!check_meta_file(meta_path, defines_hash))
        {
            return true; // Missing or hash mismatch
        }
    }
    tsl::robin_set<fs::path> visited;
    const auto latest_input_time = get_most_recent_time(input_path, visited, include_paths);
    const auto output_time = fs::last_write_time(output_path);

    if (latest_input_time > output_time)
    {
        return true;
    }

    return false;
}

bool write_shader_blob(const fs::path& output_path,
                       const std::vector<CompilerOutput>& outputs,
                       const CompilerInputVector& inputs)
{
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open())
    {
        return false;
    }

    const ShaderBlobHeader header{
        .magic = SHADER_BLOB_MAGIC,
        .version = SHADER_BLOB_VERSION,
        .shader_count = outputs.size(),
    };
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    uint64_t current_offset = sizeof(ShaderBlobHeader);

    for (size_t i = 0; i < outputs.size(); i++)
    {
        current_offset += sizeof(ShaderBlobEntry);

        const auto defines = inputs[i].get_defines();
        for (const auto& def : defines)
        {
            current_offset += def.size() + 1; // Include null terminator
        }
    }

    auto data_offset = current_offset;

    for (size_t i = 0; i < outputs.size(); i++)
    {
        const auto& co = outputs[i];
        const auto defines = inputs[i].get_defines();
        ShaderBlobEntry entry{
            .offset = data_offset,
            .size = co.blob->getBufferSize(),
            .define_count = static_cast<uint32_t>(defines.size()),
        };
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));

        for (const auto& def : defines)
        {
            assert(def.size() + 1 <= std::numeric_limits<long long>::max());
            out.write(def.c_str(), def.size() + 1); // Include null terminator
        }

        data_offset += co.blob->getBufferSize();
    }

    for (const auto& co : outputs)
    {
        out.write(static_cast<const char*>(co.blob->getBufferPointer()), co.blob->getBufferSize());
    }

    if (!out.good())
    {
        return false;
    }

    if (inputs.size() == 1)
    {
        return true;
    }

    // Write meta file containing the defines hash
    const fs::path input_path = inputs[0].get_path();
    fs::path meta_path = output_path.parent_path() / input_path.stem();
    meta_path += ".meta";
    const auto defines_hash = compute_defines_hash(inputs);
    return write_meta_file(meta_path, defines_hash);
}

} // namespace

ShaderType SXCJob::to_shader_type(const char* str)
{
    if (strcmp(str, "vs") == 0)
    {
        return VERTEX_SHADER;
    }
    if (strcmp(str, "ps") == 0)
    {
        return PIXEL_SHADER;
    }
    if (strcmp(str, "cs") == 0)
    {
        return COMPUTE_SHADER;
    }
    if (strcmp(str, "lib") == 0 || strcmp(str, "library") == 0)
    {
        return LIBRARY_SHADER;
    }
    throw std::runtime_error("Unknown shader type");
}

const char* SXCJob::shader_type_to_str(const ShaderType type)
{
    switch (type)
    {
    case VERTEX_SHADER:
        return "_vs_";
    case PIXEL_SHADER:
        return "_ps_";
    case COMPUTE_SHADER:
        return "_cs_";
    case LIBRARY_SHADER:
        return "_lib_";
    }
    throw std::runtime_error("Unknown shader type");
}

int SXCJob::parse_config(const CLIInput& input,
                         tbb::concurrent_vector<boost::container::small_vector<CompilerInput, 1>>* compiler_inputs)
{
    assert(compiler_inputs);
    std::ifstream config_file(input.config_path);
    if (!config_file.is_open())
    {
        fprintf(stderr, "Failed to open config file: %s\n", input.config_path.c_str());
        return -1;
    }

    std::vector<std::vector<std::string>> args;
    std::string line;
    while (std::getline(config_file, line))
    {
        args.emplace_back();
        auto& arg = args.back();
        arg.emplace_back("");

        std::istringstream iss(line);
        std::string token;
        while (iss >> token)
        {
            arg.push_back(token);
        }
    }

    tbb::concurrent_vector<std::pair<size_t, std::string>> parse_errors;

    tbb::parallel_for(
        static_cast<size_t>(0),
        args.size(),
        [&](size_t i)
        {
            CompilerInput compiler_input{
                .includes = input.include_paths,
                .shader_model = input.shader_model,
                // shader type determined below
                .flags = input.embed_debug ? CompilerInput::ShaderFlags::DEBUG : CompilerInput::ShaderFlags::NONE,
                .optimization = input.optimization, // May be overridden by config
            };

            const auto& arg = args[i];
            argparse::ArgumentParser program("config");
            program.set_prefix_chars("-+/");
            program.set_assign_chars("=:");
            program.add_argument("-p", "--path").nargs(1).required();
            program.add_argument("-out", "--output-dir").nargs(1);
            program.add_argument("-e", "--entry-point")
                .store_into(compiler_input.entry_point) // Store entry point directly
                .nargs(1);
            program.add_argument("-d", "--define").default_value(std::vector<std::string>{}).append();
            program.add_argument("-o", "--optimization").default_value("O3").choices("O0", "O1", "O2", "O3").nargs(1);
            program.add_argument("-st", "--shader-type").required();

            try
            {
                program.parse_args(arg);

                compiler_input.shader_type = to_shader_type(program.get<std::string>("--shader-type").c_str());

                // Enforce entry-point for non-library shaders
                const auto ep_present = program.present<std::string>("--entry-point");
                if (compiler_input.shader_type != LIBRARY_SHADER && !ep_present.has_value())
                {
                    throw std::runtime_error("-e: required.");
                }

                // Could be {1,2,3} format
                // Strings may get moved out so invalid after expand_defines routine
                auto defines = program.get<std::vector<std::string>>("--define");

                std::vector<boost::container::small_vector<std::string, 2>> parsed_defines;
                parsed_defines.reserve(defines.size() + input.global_defines.size());
                auto expand_defines = [&parsed_defines](auto self, std::string& d)
                {
                    auto& current_defines_list = parsed_defines.back();

                    const auto opening = d.find('{');

                    if (opening == std::string::npos)
                    {
                        current_defines_list.push_back(std::move(d));
                        return;
                    }

                    const auto closing = d.find('}', opening);
                    if (closing == std::string::npos)
                    {
                        // The entire line will fail which could cause multiple compiles to be missed
                        throw std::runtime_error("Missing '}' in define: " + d);
                    }

                    size_t current = opening + 1;
                    while (true)
                    {
                        size_t comma = d.find(',', current);
                        if (comma == std::string::npos || comma > closing)
                        {
                            comma = closing;
                        }
                        // Precompute the size for the new string to minimize allocations
                        const size_t prefix_len = opening;
                        const size_t middle_len = comma - current;
                        const size_t suffix_len = d.size() - (closing + 1);
                        std::string new_config;
                        new_config.reserve(prefix_len + middle_len + suffix_len);

                        new_config.append(d, 0, prefix_len);
                        new_config.append(d, current, middle_len);
                        new_config.append(d, closing + 1, suffix_len);
                        // Continue expanding other {}
                        self(self, new_config);

                        current = comma + 1;
                        if (comma >= closing)
                        {
                            break;
                        }
                    }
                };

                for (auto& d : input.global_defines)
                {
                    parsed_defines.emplace_back();
                    std::string copy = d;
                    expand_defines(expand_defines, copy);
                }

                for (auto& d : defines)
                {
                    parsed_defines.emplace_back();
                    expand_defines(expand_defines, d);
                }

                // Make compiler input for all combinations of defines
                CompilerInputVector inputs;
                auto generate_combinations =
                    [&parsed_defines, &inputs, &compiler_input, &program](auto self,
                                                                          std::vector<std::string>& per_compile_defines)
                {
                    if (per_compile_defines.size() == parsed_defines.size())
                    {
                        CompilerInput input_copy = compiler_input;
                        input_copy.path_and_defines = Owning{.path = program.get<std::string>("--path"),
                                                             .defines = per_compile_defines};
                        inputs.push_back(std::move(input_copy));
                        return;
                    }
                    for (const auto& define : parsed_defines[per_compile_defines.size()])
                    {
                        // Recursively generate combinations
                        per_compile_defines.push_back(define);
                        self(self, per_compile_defines);
                        // Backtrack
                        per_compile_defines.pop_back();
                    }
                };

                std::vector<std::string> per_compile_defines;
                generate_combinations(generate_combinations, per_compile_defines);

                compiler_inputs->push_back(std::move(inputs));
            }
            catch (const std::exception& err)
            {
                parse_errors.emplace_back(i, std::string(err.what()));
            }
        });

    for (const auto& error : parse_errors)
    {
        fprintf(stderr, "Failed to parse config line: %llu\n\t%s\n", error.first, error.second.c_str());
    }
    return parse_errors.empty() ? 0 : -1;
}

fs::path SXCJob::get_resolved_output_name(const OutputInfo& info,
                                          const fs::path& input_path,
                                          const std::string& output_dir,
                                          const ShaderIR ir_format)
{
    // SXC always writes a .slang_blob container even for single variant shaders
    fs::path filename = input_path.filename();
    if (filename.has_extension())
    {
        filename.replace_extension();
    }

    filename += shader_type_to_str(info.st); // _XY_

    const auto sm = magic_enum::enum_name(info.sm);
    assert(!sm.empty());

    filename += sm.substr(sm.find('_') + 1);

    // Library profiles contain multiple entry points so none is appended here.
    if (info.st != LIBRARY_SHADER)
    {
        filename += "_";
        filename += info.entry_point;
    }

    switch (ir_format)
    {
    case ShaderIR::DXBC:
        filename += ".dxbc_blob";
        break;
    case ShaderIR::DXIL:
        filename += ".dxil_blob";
        break;
    case ShaderIR::SPIRV:
        filename += ".spv_blob";
        break;
    default:
        assert(false);
    }

    return fs::path(output_dir) / filename;
}

ShaderResultCount qhenki::sxc::execute_compilation_job(tbb::concurrent_vector<CompilerInputVector>* inputs,
                                                       const std::string& output_dir,
                                                       bool force,
                                                       ShaderIR ir_format)
{
    // Go through inputs and just return the same one
    const auto collect_inputs =
        tbb::make_filter<void, CompilerInputVector*>(tbb::filter_mode::serial_in_order,
                                                     [&](tbb::flow_control& fc) -> CompilerInputVector*
                                                     {
                                                         static size_t index = 0;
                                                         if (inputs->empty() || index >= inputs->size())
                                                         {
                                                             fc.stop();
                                                             return nullptr;
                                                         }
                                                         const auto civ = &(*inputs)[index++];
                                                         if (civ->empty())
                                                         {
                                                             fc.stop();
                                                             return nullptr;
                                                         }
                                                         return civ;
                                                     });

    struct OutputPathAndCompilerInputVector
    {
        fs::path output_path;
        CompilerInputVector* input_vector = nullptr;
    };

    std::atomic_uint64_t skipped_count{0};
    // Check if input needs to be compiled
    const auto filter_shaders = tbb::make_filter<CompilerInputVector*, OutputPathAndCompilerInputVector>(
        tbb::filter_mode::parallel,
        [&output_dir, &skipped_count, force, ir_format](CompilerInputVector* input) -> OutputPathAndCompilerInputVector
        {
            assert(input); // nullptr should have stopped pipeline from last filter
            assert(!input->empty());

            // Assumes that all inputs are the same shader

            // Check if the shader needs to be compiled
            // Since all inputs are the same shader just with different defines, we can cull entire groups
            // This is done by returning nullptr, which is then ignored by the next filter

            const auto& ci = input->at(0);

            const OutputInfo info{
                .sm = ci.shader_model,
                .st = ci.shader_type,
                .entry_point = ci.entry_point,
            };
            const fs::path input_path = ci.get_path();
            const fs::path output_path = SXCJob::get_resolved_output_name(info, input_path, output_dir, ir_format);

            if (needs_to_recompile_shader(input_path, output_path, ci.includes, *input, force))
            {
                return {.output_path = output_path, .input_vector = input};
            }

            skipped_count += input->size();

            return {};
        });

    struct PathAndOutputs
    {
        fs::path path;
        std::vector<CompilerOutput> output;
        CompilerInputVector* input_vector;
    };

    tbb::enumerable_thread_specific<SlangShaderCompiler> slang_compilers;
    // Compile shader
    auto compile_shaders = tbb::make_filter<OutputPathAndCompilerInputVector, PathAndOutputs>(
        tbb::filter_mode::parallel,
        [&slang_compilers, ir_format](const OutputPathAndCompilerInputVector& out_and_vector) -> PathAndOutputs
        {
            const auto& out_path = out_and_vector.output_path;
            const auto input_vector = out_and_vector.input_vector;

            if (!input_vector)
            {
                return {};
            }

            std::vector<CompilerOutput> output(input_vector->size());

            tbb::parallel_for(static_cast<size_t>(0),
                              input_vector->size(),
                              [&output, &slang_compilers, input_vector, ir_format](const size_t i)
                              {
                                  const auto& input = (*input_vector)[i];
                                  auto& out = output[i];
                                  const auto success = slang_compilers.local().compile(input, out, ir_format);

                                  const auto tm = shader_model_char(input.shader_type, input.shader_model);

                                  if (success)
                                  {
                                      printf("Permutation #%zu: Compiling shader: %s %s\n",
                                             i,
                                             input.get_path().data(),
                                             tm.data());
                                  }
                                  else
                                  {
                                      printf("Permutation #%zu: Compiling shader: %s %s %s\n",
                                             i,
                                             input.get_path().data(),
                                             tm.data(),
                                             out.error_message.data());
                                  }
                              });

            return {.path = out_path, .output = std::move(output), .input_vector = input_vector};
        });

    std::atomic_uint64_t succeeded_count{0};
    std::atomic_uint64_t failed_count{0};
    auto collect_compile_results =
        tbb::make_filter<PathAndOutputs, void>(tbb::filter_mode::parallel,
                                               [&failed_count, &succeeded_count](const PathAndOutputs& pa)
                                               {
                                                   if (pa.input_vector)
                                                   {
                                                       bool any_failed = false;
                                                       for (const auto& co : pa.output)
                                                       {
                                                           if (!co.error_message.empty())
                                                           {
                                                               ++failed_count;
                                                               any_failed = true;
                                                           }
                                                           else
                                                           {
                                                               ++succeeded_count;
                                                           }
                                                       }

                                                       if (!any_failed)
                                                       {
                                                           if (!write_shader_blob(pa.path, pa.output, *pa.input_vector))
                                                           {
                                                               printf("Failed to write shader blob to file: %s\n",
                                                                      pa.path.string().c_str());
                                                               failed_count += pa.output.size();
                                                               succeeded_count -= pa.output.size();
                                                           }
                                                       }
                                                   }
                                               });

    constexpr auto max_tokens = 2;
    parallel_pipeline(max_tokens, collect_inputs, filter_shaders, compile_shaders, collect_compile_results);

    return {
        .succeeded_count = succeeded_count.load(),
        .failed_count = failed_count.load(),
        .skipped_count = skipped_count.load(),
    };
}
