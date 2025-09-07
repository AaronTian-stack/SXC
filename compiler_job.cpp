#include "compiler_job.h"

#include <tbb/enumerable_thread_specific.h>
#include <tbb/concurrent_vector.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/parallel_pipeline.h>
#include <tbb/parallel_for_each.h>
#include <tsl/robin_set.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <regex>

#include <magic_enum/magic_enum.hpp>

#include <argparse/argparse.hpp>

#include "graphics/d3d12/d3d12_shader_compiler.h"
#include "qhenkiX/helper/d3d_helper.h"

#include "qhenkiX/helper/file_helper.h"

using namespace qhenki::sxc;

qhenki::gfx::ShaderType SXCJob::to_shader_type(const char* str)
{
	assert(strlen(str) == 2);

	auto hash = [](const char* s)->uint32_t
	{
		return s[0] << 16 | s[1] << 8 | s[2];
	};

	switch (hash(str))
	{
		case hash("vs"):
			return gfx::ShaderType::VERTEX_SHADER;
		case hash("ps"):
			return gfx::ShaderType::PIXEL_SHADER;
		case hash("cs"):
			return gfx::ShaderType::COMPUTE_SHADER;
		default:
			throw std::runtime_error("Unknown shader type: " + std::string(str));
	}
}

const char* SXCJob::shader_type_to_str(gfx::ShaderType type)
{
	switch (type)
	{
		case gfx::ShaderType::VERTEX_SHADER:
			return "_vs_";
		case gfx::ShaderType::PIXEL_SHADER:
			return "_ps_";
		case gfx::ShaderType::COMPUTE_SHADER:
			return "_cs_";
		default:
			throw std::runtime_error("Unknown shader type");
	}
}

int SXCJob::parse_config(const CLIInput& input, tbb::concurrent_vector<boost::container::small_vector<CompilerInput, 1>>* compiler_inputs)
{
	assert(compiler_inputs);
	std::ifstream config_file(input.config_path);
	if (!config_file.is_open()) 
	{
		std::cerr << "Failed to open config file: " << input.config_path << '\n';
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

	tbb::parallel_for(static_cast<size_t>(0), args.size(), [&](size_t i)
	{
			CompilerInput compiler_input
			{
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
			program.add_argument("-p", "--path")
				.nargs(1)
				.required();
			program.add_argument("-out", "--output-dir")
				.nargs(1);
			program.add_argument("-e", "--entry-point")
				.store_into(compiler_input.entry_point) // Store entry point directly
				.nargs(1)
				.required();
			program.add_argument("-d", "--define")
				.default_value(std::vector<std::string>{})
				.append();
			program.add_argument("-o", "--optimization")
				.default_value("O3")
				.choices("O0", "O1", "O2", "O3")
				.nargs(1);
			program.add_argument("-st", "--shader-type")
				.required();

			try
			{
				program.parse_args(arg);

				compiler_input.shader_type = to_shader_type(program.get<std::string>("--shader-type").c_str());

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
								comma = closing;

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
								break;
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
				auto generate_combinations = [&parsed_defines, &inputs, &compiler_input, &program](auto self,
					std::vector<std::string>& per_compile_defines)
				{
					if (per_compile_defines.size() == parsed_defines.size())
					{
						CompilerInput input_copy = compiler_input;
						input_copy.path_and_defines = Owning
						{
							.path = program.get<std::string>("--path"),
							.defines = per_compile_defines
						};
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
		}
	);

	for (const auto& error : parse_errors)
	{
		std::cerr << "Failed to parse config line: " << error.first << "\n\t" << error.second << '\n';
	}
	return parse_errors.empty() ? 0 : -1;
}

fs::file_time_type get_most_recent_time(const fs::path& file, tsl::robin_set<fs::path>& visited)
{
	if (!fs::exists(file))
	{
		printf("Include file not found: %s\n", file.string().c_str());
		return fs::file_time_type::min();
	}

	if (!visited.insert(file).second)
	{
		return fs::file_time_type::min(); // Already visited this file
	}

	std::ifstream in(file);
	if (!in.is_open())
		return fs::file_time_type::min();

	fs::file_time_type latest = fs::last_write_time(file); // This can technically throw an exception

	std::string line;
	std::regex include_regex(R"(^\s*#\s*include\s*["<](.*)[">])");
	while (std::getline(in, line))
	{
		std::smatch match;
		if (std::regex_search(line, match, include_regex))
		{
			// Look in include paths
			fs::path include_file = file.parent_path() / match[1].str();
			// Detect circular includes
			if (visited.find(include_file) != visited.end())
			{
				printf("Circular include detected: %s\n", include_file.string().c_str());
			}
			else
			{
				// Recursively get times of includes
				auto inc_time = get_most_recent_time(include_file, visited);
				if (inc_time > latest)
				{
					latest = inc_time;
				}
			}
		}
	}
	return latest;
}

bool needs_to_recompile_shader(const fs::path& input_path, const fs::path& output_path)
{
	if (!fs::exists(output_path))
	{
		return true;
	}

	// TODO: Need a hash based off defines or something. Otherwise, changes in config file do not trigger a recompilation.

	tsl::robin_set<fs::path> visited;
	const auto latest_input_time = get_most_recent_time(input_path, visited);
	const auto output_time = fs::last_write_time(output_path);

	if (latest_input_time > output_time)
		return true;

	return false;
}

fs::path SXCJob::get_resolved_output_name(const OutputInfo& info, const fs::path& input_path, const std::string& output_dir)
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
	filename += "_";
	filename += info.entry_point;

	// TODO: flag for Vulkan/SPIRV

	// TODO: If including multiple permutations, change file type to reflect that to differentiate between plain shaders
	if (info.sm > gfx::ShaderModel::SM_5_0)
	{
		filename += ".dxil";
	}
	else
	{
		filename += ".dxbc";
	}

	return fs::path(output_dir) / filename;
}

ShaderResultCount qhenki::sxc::execute_compilation_job(tbb::concurrent_vector<CompilerInputVector>* inputs, const std::string& output_dir)
{
	// Go through inputs and just return the same one
	const auto collect_inputs = tbb::make_filter<void, CompilerInputVector*>(
		tbb::filter_mode::serial_in_order,
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
		}
	);

	struct OutputPathAndCompilerInputVector
	{
		fs::path output_path;
		CompilerInputVector* input_vector = nullptr;
	};

	std::atomic_uint64_t skipped_count{ 0 };
	// Check if input needs to be compiled
	const auto filter_shaders = tbb::make_filter<CompilerInputVector*, OutputPathAndCompilerInputVector>(
		tbb::filter_mode::parallel,
		[&output_dir, &skipped_count](CompilerInputVector* input) -> OutputPathAndCompilerInputVector
		{
			assert(input); // nullptr should have stopped pipeline from last filter
			assert(!input->empty());

			// Assumes that all inputs are the same shader!

			// Check if the shader needs to be compiled
			// Since all inputs are the same shader just with different defines, we can cull entire groups
			// This is done by returning nullptr, which is then ignored by the next filter

			const auto& ci = input->at(0);

			const OutputInfo info
			{
				.sm = ci.shader_model,
				.st = ci.shader_type,
				.entry_point = ci.entry_point,
			};
			fs::path input_path = ci.get_path();
			fs::path output_path = SXCJob::get_resolved_output_name(info, input_path, output_dir);

			if (needs_to_recompile_shader(input_path, output_path))
			{
				return
				{
					.output_path = output_path,
					.input_vector = input
				};
			}

			skipped_count += input->size();

			return {};
		}
	);

	using CompilerOutputVector = oneapi::tbb::concurrent_vector<CompilerOutput>;

	struct PathAndOutputs
	{
		fs::path path;
		CompilerOutputVector* output;
	};

	tbb::enumerable_thread_specific<gfx::D3D12ShaderCompiler> compilers;
	// Compile shader
	auto compile_shaders = tbb::make_filter<OutputPathAndCompilerInputVector, PathAndOutputs>(
		tbb::filter_mode::parallel,
		[&compilers](const OutputPathAndCompilerInputVector& out_and_vector) -> PathAndOutputs
		{
			const auto& out_path = out_and_vector.output_path;
			const auto input_vector = out_and_vector.input_vector;

			if (!input_vector)
			{
				return
				{
					.path= "",
					.output= nullptr
				};
			}

			auto& compiler = compilers.local();

			using allocator = oneapi::tbb::tbb_allocator<CompilerOutputVector>;

			allocator alloc;
			auto output = alloc.allocate(1);
			std::allocator_traits<allocator>::construct(alloc, output);

			if (input_vector->size() > 1)
			{
				output->reserve(input_vector->size());
			}

			tbb::parallel_for(static_cast<size_t>(0), input_vector->size(), [&](size_t i)
			{
				auto& input = (*input_vector)[i];
				output->emplace_back();
				auto& out = output->back();
				const auto success = compiler.compile(input, out);

				const auto tm = gfx::D3DHelper::get_shader_model_char(input.shader_type, input.shader_model);

				if (input_vector->size() == 1)
				{
					if (success)
					{
						printf("Compiling shader: %s %s\n", input.get_path().data(), tm.data());
					}
					else
					{
						printf("Compiling shader: %s %s\n%s", input.get_path().data(), tm.data(), out.error_message.data());
					}
				}
				else
				{
					if (success)
					{
						printf("Permutation #%zu: Compiling shader: %s %s\n", i, input.get_path().data(), tm.data());
					}
					else
					{
						printf("Permutation #%zu: Compiling shader: %s %s\n%s", i, input.get_path().data(), tm.data(), out.error_message.data());
					}
				}
			});

			return
			{
				.path = out_path,
				.output = output
			};
		}
	);

	std::atomic_uint64_t succeeded_count{ 0 };
	std::atomic_uint64_t failed_count{ 0 };
	// Write out and collect results
	auto collect_compile_results = tbb::make_filter<PathAndOutputs, void>(
		tbb::filter_mode::parallel,
		[&failed_count, &succeeded_count](const PathAndOutputs& pa) -> void
		{
			if (pa.output)
			{
				for (const auto& co : *pa.output)
				{
					if (!co.error_message.empty())
					{
						++failed_count;
					}
					else
					{
						++succeeded_count;

						// TODO: permutations of the same shader need to be written to the same file
						// Needs to be a header with offsets

						if (!util::FileHelper::write_file(pa.path.c_str(), co.shader_data, co.shader_size))
						{
							printf("Failed to write shader to file: %s\n", pa.path.string().c_str());
							++failed_count;
						}
					}
				}
				oneapi::tbb::tbb_allocator<CompilerOutputVector>().deallocate(pa.output, 1);
			}
		}
	);

	constexpr auto max_tokens = 2;
	parallel_pipeline(max_tokens, collect_inputs, filter_shaders, compile_shaders, collect_compile_results);

	return
	{
		.succeeded_count = succeeded_count.load(),
		.failed_count = failed_count.load(),
		.skipped_count = skipped_count.load(),
	};
}
