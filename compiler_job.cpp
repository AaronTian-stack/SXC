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

#include "graphics/d3d12/dxc_shader_compiler.h"
#if defined(_WIN32) || defined(_WIN64)
#include "graphics/d3d11/fxc_shader_compiler.h"
#endif
#include "utility/d3d_util.h"

#include "qhenki/utility/file_util.h"
#include "qhenki/utility/shader_blob.h"


using namespace qhenki::sxc;
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
                               bool force)
{
    if (force || !fs::exists(output_path))
    {
        return true;
    }

    // Check meta file for permutated shaders
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
                       const tbb::concurrent_vector<CompilerOutput>& outputs,
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
            .size = co.blob->GetBufferSize(),
            .define_count = static_cast<uint32_t>(defines.size()),
        };
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));

        for (const auto& def : defines)
        {
            assert(def.size() + 1 <= std::numeric_limits<long long>::max());
            out.write(def.c_str(), def.size() + 1); // Include null terminator
        }

        data_offset += co.blob->GetBufferSize();
    }

    for (const auto& co : outputs)
    {
        assert(co.blob->GetBufferSize() <= std::numeric_limits<std::streamsize>::max());
        out.write(static_cast<const char*>(co.blob->GetBufferPointer()), co.blob->GetBufferSize());
    }

    if (!out.good())
    {
        return false;
    }

    // Write meta file containing the defines hash
    const fs::path input_path = inputs[0].get_path();
    fs::path meta_path = output_path.parent_path() / input_path.stem();
    meta_path += ".meta";
    const auto defines_hash = compute_defines_hash(inputs);
    return write_meta_file(meta_path, defines_hash);
}

} // namespace

qhenki::gfx::ShaderType SXCJob::to_shader_type(const char* str)
{
    // Support traditional stages and DXIL library (lib)
    if (strcmp(str, "vs") == 0)
    {
        return gfx::ShaderType::VERTEX_SHADER;
    }
    if (strcmp(str, "ps") == 0)
    {
        return gfx::ShaderType::PIXEL_SHADER;
    }
    if (strcmp(str, "cs") == 0)
    {
        return gfx::ShaderType::COMPUTE_SHADER;
    }
    if (strcmp(str, "lib") == 0 || strcmp(str, "library") == 0)
    {
        return gfx::ShaderType::LIBRARY_SHADER;
    }
    throw std::runtime_error("Unknown shader type");
}

const char* SXCJob::shader_type_to_str(const gfx::ShaderType type)
{
    switch (type)
    {
    case gfx::ShaderType::VERTEX_SHADER:
        return "_vs_";
    case gfx::ShaderType::PIXEL_SHADER:
        return "_ps_";
    case gfx::ShaderType::COMPUTE_SHADER:
        return "_cs_";
    case gfx::ShaderType::LIBRARY_SHADER:
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

    tbb::concurrent_vector<std::pair<int, std::string>> parse_errors;

    tbb::parallel_for(
        static_cast<size_t>(0),
        args.size(),
        [&](size_t i)
        {
            CompilerInput compiler_input{
                .pdb_path = input.pdb_dir,
                .includes = input.include_paths,
                .shader_model = input.shader_model,
                // shader type determined below
                .flags = input.debug_flag ? CompilerInput::ShaderFlags::DEBUG : CompilerInput::ShaderFlags::NONE,
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
                if (compiler_input.shader_type != gfx::ShaderType::LIBRARY_SHADER && !ep_present.has_value())
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
        fprintf(stderr, "Failed to parse config line: %d\n\t%s\n", error.first, error.second.c_str());
    }
    return parse_errors.empty() ? 0 : -1;
}

fs::path SXCJob::get_resolved_output_name(const OutputInfo& info,
                                          const fs::path& input_path,
                                          const std::string& output_dir,
                                          const size_t permutation_count)
{
    // Appends something like _vs_5_0_main.dxil
    fs::path filename = input_path.filename();
    if (filename.has_extension())
    {
        filename.replace_extension();
    }

    filename += shader_type_to_str(info.st); // _XY_

    const auto sm = magic_enum::enum_name(info.sm);
    assert(!sm.empty());

    filename += sm.substr(sm.find('_') + 1);
    // For DXIL libraries, skip appending an entry point name
    if (info.st != gfx::ShaderType::LIBRARY_SHADER)
    {
        filename += "_";
        filename += info.entry_point;
    }

    if (info.output_spirv)
    {
        if (permutation_count > 1)
        {
            filename += ".spv_blob";
        }
        else
        {
            filename += ".spv";
        }
        return fs::path(output_dir) / filename;
    }

    if (permutation_count > 1)
    {
        if (info.sm > gfx::ShaderModel::SM_5_0)
        {
            filename += ".dxil_blob";
        }
        else
        {
            filename += ".dxbc_blob";
        }
    }
    else
    {
        if (info.sm > gfx::ShaderModel::SM_5_0)
        {
            filename += ".dxil";
        }
        else
        {
            filename += ".dxbc";
        }
    }

    return fs::path(output_dir) / filename;
}

ShaderResultCount qhenki::sxc::execute_compilation_job(tbb::concurrent_vector<CompilerInputVector>* inputs,
                                                       const std::string& output_dir,
                                                       bool force,
                                                       bool output_spirv)
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
        [&output_dir, &skipped_count, force, output_spirv](
            CompilerInputVector* input) -> OutputPathAndCompilerInputVector
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
                .output_spirv = output_spirv,
            };
            const fs::path input_path = ci.get_path();
            const fs::path output_path = SXCJob::get_resolved_output_name(info, input_path, output_dir, input->size());

            if (needs_to_recompile_shader(input_path, output_path, ci.includes, *input, force))
            {
                return {.output_path = output_path, .input_vector = input};
            }

            skipped_count += input->size();

            return {};
        });

    using CompilerOutputVector = oneapi::tbb::concurrent_vector<CompilerOutput>;

    struct PathAndOutputs
    {
        fs::path path;
        CompilerOutputVector* output;
        CompilerInputVector* input_vector;
    };

    tbb::enumerable_thread_specific<gfx::DXCShaderCompiler> d3d12_compilers;
#if defined(_WIN32) || defined(_WIN64)
    tbb::enumerable_thread_specific<gfx::FXCShaderCompiler> d3d11_compilers;
#endif
    // Compile shader
    auto compile_shaders = tbb::make_filter<OutputPathAndCompilerInputVector, PathAndOutputs>(
        tbb::filter_mode::parallel,
        [&d3d12_compilers
#if defined(_WIN32) || defined(_WIN64)
         ,
         &d3d11_compilers
#endif
         ,
         output_spirv](const OutputPathAndCompilerInputVector& out_and_vector) -> PathAndOutputs
        {
            const auto& out_path = out_and_vector.output_path;
            const auto input_vector = out_and_vector.input_vector;

            if (!input_vector)
            {
                return {.path = "", .output = nullptr, .input_vector = nullptr};
            }

            // Select compiler based on shader model
            ShaderCompiler* compiler;
            const auto& first_input = input_vector->at(0);

            assert(!(output_spirv && first_input.shader_model < gfx::ShaderModel::SM_6_0));
            if (!output_spirv && first_input.shader_model < gfx::ShaderModel::SM_6_0)
            {
#if defined(_WIN32) || defined(_WIN64)
                compiler = &d3d11_compilers.local();
#else
                compiler = &d3d12_compilers.local();
#endif
            }
            else
            {
                compiler = &d3d12_compilers.local();
            }

            using allocator = oneapi::tbb::tbb_allocator<CompilerOutputVector>;

            allocator alloc;
            const auto output = alloc.allocate(1);
            std::allocator_traits<allocator>::construct(alloc, output);

            if (input_vector->size() > 1)
            {
                output->reserve(input_vector->size());
            }

            tbb::parallel_for(static_cast<size_t>(0),
                              input_vector->size(),
                              [&output, compiler, input_vector, output_spirv](const size_t i)
                              {
                                  const auto& input = (*input_vector)[i];
                                  output->emplace_back();
                                  auto& out = output->back();
                                  const auto success = compiler->compile(input, out, output_spirv);

                                  const auto tm = gfx::shader_model_char(input.shader_type, input.shader_model);

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

            return {.path = out_path, .output = output, .input_vector = input_vector};
        });

    std::atomic_uint64_t succeeded_count{0};
    std::atomic_uint64_t failed_count{0};
    auto collect_compile_results = tbb::make_filter<PathAndOutputs, void>(
        tbb::filter_mode::parallel,
        [&failed_count, &succeeded_count](const PathAndOutputs& pa)
        {
            if (pa.output)
            {
                bool any_failed = false;
                for (const auto& co : *pa.output)
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
                    if (pa.output->size() == 1)
                    {
                        const auto& co = pa.output->at(0);
                        if (!co.blob || co.blob->GetBufferSize() == 0 || co.blob->GetBufferPointer() == nullptr)
                        {
                            printf("0 byte shader output: %s\n", pa.path.string().c_str());
                            ++failed_count;
                            --succeeded_count;
                        }
                        else if (!write_file(pa.path.c_str(), co.blob->GetBufferPointer(), co.blob->GetBufferSize()))
                        {
                            printf("Failed to write shader to file: %s\n", pa.path.string().c_str());
                            ++failed_count;
                            --succeeded_count;
                        }
                    }
                    else
                    {
                        if (!write_shader_blob(pa.path, *pa.output, *pa.input_vector))
                        {
                            printf("Failed to write shader blob to file: %s\n", pa.path.string().c_str());
                            failed_count += pa.output->size();
                            succeeded_count -= pa.output->size();
                        }
                    }
                    for (const auto& co : *pa.output)
                    {
                        if (co.blob)
                        {
                            co.blob->Release();
                        }
                    }
                }

                oneapi::tbb::tbb_allocator<CompilerOutputVector>().deallocate(pa.output, 1);
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
