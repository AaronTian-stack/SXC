#include <iostream>
#include <argparse/argparse.hpp>
#include <qhenkiX/RHI/shader_compiler.h>
#include <magic_enum/magic_enum.hpp>
#include <filesystem>
#include "compiler_job.h"

int main(int argc, char* argv[])
{
	argparse::ArgumentParser program("SXC", "0.1.0");
	program.add_description("FXC/DXC batch shader compiler.");
	program.set_prefix_chars("-+/");
	program.set_assign_chars("=:");

	{ // OPTIONAL ARGUMENTS
		program
			.add_argument("-pdb", "--pdb-path")
			.help("pdb output path")
			.nargs(1);

		program
			.add_argument("-i", "--include-path")
			.append()
			.help("include path");

		program
			.add_argument("-g", "--global-defines")
			.append()
			.help("defines to be used for all shaders");

		program.add_argument("-dbg", "--debug-flag")
			.flag()
			.help("enable debug flag for all shaders");

		program.add_argument("-o", "--optimization")
			.choices("O0", "O1", "O2", "O3")
			.nargs(1)
			.default_value("O3")
			.help("default optimization level for shaders");
	}

	std::string config_file_path;

	{ // REQUIRED ARGUMENTS
		program.add_group("Required arguments");

		program
			.add_argument("-c", "--config-path")
			.help("config file path")
			.nargs(1)
			.store_into(config_file_path)
			.required();

		program
			.add_argument("-sm", "--shader-model")
			.help("HLSL shader model [X_Y]")
			.choices("5_0", "5_1", "6_0", "6_1", "6_2", "6_3", "6_4", "6_5", "6_6")
			.nargs(1)
			.required();

		program
			.add_argument("-out", "--output")
			.nargs(1)
			.help("output directory")
			.required();
	}

	try
	{
		program.parse_args(argc, argv);

		// Verify config file path
		if (!std::filesystem::exists(config_file_path))
		{
			throw std::runtime_error("Config file does not exist: " + program.get<std::string>("--config-path"));
		}

		// Set working directory to config file location
		const auto config_dir = std::filesystem::path(config_file_path).parent_path();
		if (!config_dir.empty())
		{
			std::filesystem::current_path(config_dir);
		}

		const auto pdb = program.present<std::string>("--pdb-path");
		const auto includes = program.present<std::vector<std::string>>("--include-path");

		{ // Verify optional arguments

			// Check PDB path exists if specified
			if (pdb.has_value())
			{
				if (const auto& pdb_path = pdb.value(); !std::filesystem::exists(pdb_path))
				{
					throw std::runtime_error("Specified PDB path does not exist: " + std::string(pdb_path.begin(), pdb_path.end()));
				}
			}

			// Check if all include paths exist if specified
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

		std::array<char, 7> sm_str;
		if (snprintf(sm_str.data(), sm_str.size(), "SM_%s", program.get<std::string>("--shader-model").c_str()) < 0)
		{
			throw std::runtime_error("Failed to extract SM string");
		}

		const auto sm = magic_enum::enum_cast<qhenki::gfx::ShaderModel>(sm_str.data());
		if (!sm.has_value())
		{
			throw std::runtime_error("Failed to reflect shader model: " + std::string(sm_str.data()));
		}

		const auto optimization = magic_enum::enum_cast<CompilerInput::Optimization>(
			program.get<std::string>("--optimization"));
		if (!optimization.has_value())
		{
			throw std::runtime_error("Failed to reflect optimization");
		}

		auto global_defines = program.present<std::vector<std::string>>("--global-defines");
		qhenki::sxc::CLIInput input
		{
			.config_path = std::move(config_file_path),
			.output_dir = program.get<std::string>("--output"),
			.pdb_dir = pdb.has_value() ? pdb.value() : std::string_view{},
			.global_defines = global_defines.has_value() ? global_defines.value() : std::span<const std::string>{},
			.include_paths = includes.has_value() ? includes.value() : std::span<const std::string>{},
			.shader_model = sm.value(),
			.optimization = optimization.value(),
			.debug_flag = program.get<bool>("--debug-flag")
		};

		const auto start = std::chrono::steady_clock::now();
		
		tbb::concurrent_vector<qhenki::sxc::CompilerInputVector> inputs;
		const auto num_lines = qhenki::sxc::SXCJob::parse_config(input, &inputs);
		if (num_lines < 0)
		{
			std::cerr << "Failed to parse config file: " << input.config_path << '\n';
			return 1;
		}

		const auto result_count = qhenki::sxc::execute_compilation_job(&inputs, input.output_dir);

		const auto end = std::chrono::steady_clock::now();

		std::cout << "========== Build: "
			  << result_count.succeeded_count	<< " succeeded, "
			  << result_count.failed_count		<< " failed, "
			  << result_count.skipped_count		<< " up-to-date ==========\n";

		// Print duration in seconds with milliseconds
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		double seconds = ms / 1000.0;

		std::cout << "========== Build completed and took " << std::fixed << std::setprecision(3)
			  << seconds << " seconds ==========\n";

		return 0;
	}
	catch (const std::exception& err)
	{
		std::cerr << err.what() << '\n';
		std::cerr << program;
		return 1;
	}
}
