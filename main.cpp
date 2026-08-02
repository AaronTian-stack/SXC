#include <qhenki/rhi/shader_compiler.h>
#include <qhenki/utility/string_util.h>
#include <argparse/argparse.hpp>
#include <cinttypes>
#include <filesystem>
#include <magic_enum/magic_enum.hpp>
#include "compiler_job.h"
#include "graphics/shared/slang_shader_compiler.h"

int main(int argc, char* argv[])
{
    argparse::ArgumentParser program("SXC", "0.5.0");
    program.add_description("Slang batch shader compiler.");
#if defined(_WIN32) || defined(_WIN64)
    program.set_prefix_chars("-+/");
#else
    program.set_prefix_chars("-+");
#endif
    program.set_assign_chars("=:");

    { // OPTIONAL ARGUMENTS
        program.add_argument("-i", "--include-path").append().help("include path");

        program.add_argument("-g", "--global-defines").append().help("defines to be used for all shaders");

        program.add_argument("-dbg", "--embed-debug").flag().help("embed debug information in all shaders");

        program.add_argument("-f", "--force").flag().help("force recompilation of all shaders");

        program.add_argument("-o", "--optimization")
            .choices("O0", "O1", "O2", "O3")
            .nargs(1)
            .default_value("O3")
            .help("default optimization level for shaders");
    }

    std::string config_file_path;

    { // REQUIRED ARGUMENTS
        program.add_group("Required arguments");

        program.add_argument("-c", "--config-path")
            .help("config file path")
            .nargs(1)
            .store_into(config_file_path)
            .required();

        program.add_argument("-sm", "--shader-model")
            .help("shader model [X_Y]")
            .choices("5_0", "6_0", "6_1", "6_2", "6_3", "6_4", "6_5", "6_6", "6_7", "6_8", "6_9")
            .nargs(1)
            .required();

        program.add_argument("-out", "--output").nargs(1).help("output directory").required();

        program.add_argument("-ir", "--output-IR")
            .choices("DXBC", "DXIL", "SPIRV")
            .nargs(1)
            .help("shader intermediate representation")
            .required();
    }

    try
    {
        program.parse_args(argc, argv);

        if (!std::filesystem::exists(config_file_path))
        {
            throw std::runtime_error("Config file does not exist: " + program.get<std::string>("--config-path"));
        }

        const auto config_dir = std::filesystem::path(config_file_path).parent_path();
        if (!config_dir.empty())
        {
            std::filesystem::current_path(config_dir);
        }

        const auto includes = program.present<std::vector<std::string>>("--include-path");

        { // Verify optional arguments

            if (includes.has_value())
            {
                for (const auto& include_paths = includes.value(); const auto& include_path : include_paths)
                {
                    if (!std::filesystem::exists(include_path))
                    {
                        throw std::runtime_error("Specified include path does not exist: " + include_path);
                    }
                }
            }
        }

        const auto sm_str = qhenki::util::format_string<7>("SM_%s", program.get<std::string>("--shader-model").c_str());

        const auto sm = magic_enum::enum_cast<qhenki::gfx::ShaderModel>(sm_str.buffer.data());
        if (!sm.has_value())
        {
            throw std::runtime_error("Failed to reflect shader model: " + std::string(sm_str.buffer.data()));
        }

        const auto output_IR = magic_enum::enum_cast<ShaderIR>(program.get<std::string>("--output-IR"));
        if (!output_IR.has_value())
        {
            throw std::runtime_error("Failed to reflect output type");
        }

        if (output_IR.value() == ShaderIR::DXBC && sm.value() >= qhenki::gfx::ShaderModel::SM_6_0)
        {
            throw std::runtime_error("DXBC does not support SM >= 6.0");
        }
        if (output_IR.value() != ShaderIR::DXBC && sm.value() < qhenki::gfx::ShaderModel::SM_6_0)
        {
            throw std::runtime_error("DXIL and SPIR-V require SM >= 6.0");
        }

        const auto optimization = magic_enum::enum_cast<CompilerInput::Optimization>(
            program.get<std::string>("--optimization"));
        if (!optimization.has_value())
        {
            throw std::runtime_error("Failed to reflect optimization");
        }

        auto global_defines = program.present<std::vector<std::string>>("--global-defines");
        qhenki::sxc::CLIInput input{.config_path = std::move(config_file_path),
                                    .output_dir = program.get<std::string>("--output"),
                                    .global_defines = global_defines.has_value() ? global_defines.value()
                                                                                 : std::span<const std::string>{},
                                    .include_paths = includes.has_value() ? includes.value()
                                                                          : std::span<const std::string>{},
                                    .shader_model = sm.value(),
                                    .optimization = optimization.value(),
                                    .embed_debug = program.get<bool>("--embed-debug"),
                                    .force_recompile = program.get<bool>("--force"),
                                    .output_IR = output_IR.value()};

        const auto start = std::chrono::steady_clock::now();

        tbb::concurrent_vector<qhenki::sxc::CompilerInputVector> inputs;
        const auto num_lines = qhenki::sxc::SXCJob::parse_config(input, &inputs);
        if (num_lines < 0)
        {
            fprintf(stderr, "Failed to parse config file: %s\n", input.config_path.c_str());
            return 1;
        }

        std::array<char, 4096> name_buffer;
#ifdef __linux__
        static_assert(name_buffer.size() >= PATH_MAX);
#endif
        if (qhenki::gfx::SlangShaderCompiler::get_compiler_path(name_buffer.data(), name_buffer.size()))
        {
            printf("Using shader compiler library:\nSlang: %s\n", name_buffer.data());
        }
        else
        {
            printf("Using shader compiler library: Slang (path unavailable)\n");
        }

        const auto result_count =
            qhenki::sxc::execute_compilation_job(&inputs, input.output_dir, input.force_recompile, input.output_IR);

        const auto end = std::chrono::steady_clock::now();

        printf("========== Build: %" PRIu64 " succeeded, %" PRIu64 " failed, %" PRIu64 " up-to-date ==========\n",
               result_count.succeeded_count,
               result_count.failed_count,
               result_count.skipped_count);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        double seconds = static_cast<double>(ms) / 1000.0;

        printf("========== Build completed and took %.3f seconds ==========\n", seconds);

        if (result_count.failed_count > 0)
        {
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& err)
    {
        fprintf(stderr, "%s\n", err.what());
        fprintf(stderr, "%s", program.help().str().c_str());
        return EXIT_FAILURE;
    }
}
