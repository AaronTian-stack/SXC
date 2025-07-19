#include <iostream>
#include <argparse/argparse.hpp>
#include <qhenkiX/RHI/shader_compiler.h>
#include <magic_enum/magic_enum.hpp>

int main(int argc, char* argv[])
{
	argparse::ArgumentParser program("SXC");
	program.add_description("FXC/DXC shader compiler.");

	program
		.add_argument("-p", "--path")
		.default_value<std::vector<std::wstring>>({})
		.help("shader file path")
		.required();

	program
		.add_argument("-pdb", "--pdb-path")
		.help("(optional) pdb file path")
		.default_value(std::wstring(L""));

	program
		.add_argument("-e", "--entry-point")
		.help("entry point function name")
		.default_value(std::wstring(L"main"))
		.nargs(1)
		.required();

	program
		.add_argument("-i", "--include-path")
		.default_value<std::vector<std::string>>({})
		.help("include path");

	program
		.add_argument("-d", "--define")
		.default_value<std::vector<std::wstring>>({})
		.help("defines");

	program
		.add_argument("-sm", "--shader-model")
		.help("hlsl shader model")
		.choices("SM_5_0", "SM_5_1", "SM_6_0", "SM_6_1", "SM_6_2", "SM_6_3", "SM_6_4", "SM_6_5", "SM_6_6")
		.nargs(1)
		.required();

	program
		.add_argument("-st", "--shader-type")
		.help("shader type")
		.nargs(1)
		.required();

	// Flags
	program.add_argument("-d", "--debug-flag")
		.flag()
		.help("enable debug flag");

	program
		.add_argument("-o", "--optimization")
		.default_value("O3")
		.choices("O0", "O1", "O2", "O3")
		.help("optimization level");

	try
	{
		program.parse_args(argc, argv);

		// TODO: check all shader paths exist
		// TODO: check pdb, include paths exist
		
		auto sm = magic_enum::enum_cast<qhenki::gfx::ShaderModel>(program.get<std::string>("--shader-model"));
		if (!sm.has_value())
		{
			throw std::runtime_error("Invalid shader model specified");
		}

		auto st = magic_enum::enum_cast<qhenki::gfx::ShaderType>(program.get<std::string>("--shader-type"));
		if (!st.has_value())
		{
			throw std::runtime_error("Invalid shader type specified");
		}

		auto opt = magic_enum::enum_cast<CompilerInput::Optimization>(program.get<std::string>("--optimization"));
		if (!opt.has_value())
		{
			throw std::runtime_error("Invalid optimization level specified");
		}
		
		// Pass values to job factory
	}
	catch (const std::exception& err)
	{
		std::cerr << err.what() << std::endl;
		std::cerr << program;
		return 1;
	}
}
