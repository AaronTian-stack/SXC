# SXC - Standalone Shader Compiler

SXC (Shader eXecution Compiler) is a command line tool for batch compilation of Slang HLSL shaders, acting as a frontend for [Slang](https://github.com/shader-slang/slang) to produce DXBC, DXIL, and SPIR-V. Although SXC uses the [QhenkiX](../QhenkiX) library, it can be used on its own as a separate tool.

SXC is heavily inspired by [ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake) (MIT License), but with a key difference being that it does not create a individual subprocess for every shader compilation, which should result in lower overhead. 

## Features

- **Batch Compilation**: Compile multiple shaders from configuration file
- **Parallel Processing**: Uses Intel TBB for efficient multi-threaded compilation
- **Shader Permutations**: Support for generating multiple variants with different defines
- **Multiple Shader Models**: Support for SM 5.0 and SM 6.0 through 6.6
- **Library Profile Support**: Compile DXIL libraries (`lib` / `library`)
- **Debug Support**: Optional debug information generation
- **Incremental Rebuilds**: Skips up-to-date shaders by checking file timestamps and include dependencies

## Basic Usage

```bash
SXC.exe -c <config_file> -sm <shader_model> -ir <DXBC|DXIL|SPIRV> -out <output_dir> [options]
```

The output consists of one or more `.slang_blob` files, which is a container holding one or more native shader bytecode variants. 

### Required Arguments

- `-c, --config-path`: Path to the configuration file
- `-sm, --shader-model`: Target shader model (`5_0` through `6_9`)
- `-ir, --output-IR`: Output representation (`DXBC`, `DXIL`, or `SPIRV`)
- `-out, --output`: Output directory for compiled shaders

### Optional Arguments

- `-i, --include-path`: Additional include directories (can be specified multiple times)
- `-g, --global-defines`: Global preprocessor defines for all shaders (can be specified multiple times)
- `-dbg, --embed-debug`: Embed target-native debug information in all shaders
- `-f, --force`: Force recompilation of shaders even if up-to-date
- `-o, --optimization`: Default optimization level (O0, O1, O2, O3) [default: O3]

**Note**: Paths are resolved relative to the configuration file's directory location.

## Configuration File Format

The configuration file contains one shader compilation job per line. Each line specifies the shader file and compilation parameters:

```
-p <shader_path> -st <shader_type> [-e <entry_point>] [options]
```

### Configuration Parameters

- `-p, --path`: Path to a native `.slang` or compatible `.hlsl` shader file
- `-e, --entry-point`: Shader entry point function name (required for non-library shader types)
- `-st, --shader-type`: Shader type (`vs`, `ps`, `cs`, `lib`)
- `-out, --output-dir`: Override global output directory for this shader
- `-d, --define`: Preprocessor defines (supports permutation syntax)
- `-o, --optimization`: Override global optimization level

### Shader Permutations

SXC supports generating multiple shader variants using define permutations:

```
-p shader.slang -e main -st vs -d FEATURE_A={0,1} -d FEATURE_B={0,1}
```

This will generate 4 shader variants:
- `FEATURE_A=0, FEATURE_B=0`
- `FEATURE_A=0, FEATURE_B=1`
- `FEATURE_A=1, FEATURE_B=0`
- `FEATURE_A=1, FEATURE_B=1`

All shader permutations are compiled and written to a single binary file. There is an additional `.meta` file generated to track permutation changes for incremental rebuild decisions.

To select a permutation at runtime, call `ShaderBlob::find_shader()` with its requested defines. `shader_blob.h` is part of QhenkiX but can be copied and used as a standalone header.

## Example

### Configuration File (`shaders.config`)
```
-p basic_vs.slang -e main -st vs
-p basic_ps.slang -e main -st ps -d USE_TEXTURE={0,1}
-p compute.slang -e CSMain -st cs -o O2
-p raytracing_lib.slang -st lib
```

### Command Line
```bash
SXC.exe -c shaders.config -sm 6_0 -ir DXIL -out compiled_shaders -i include_dir -g GLOBAL_DEFINE=1
```

## Dependencies

- [QhenkiX](https://github.com/AaronTian-stack/QhenkiX) - MIT License
- [Slang](https://github.com/shader-slang/slang) - Apache 2.0 with LLVM Exception
- [oneTBB](https://github.com/uxlfoundation/oneTBB) - Apache 2.0 License
- [argparse](https://github.com/p-ranav/argparse) - MIT License
