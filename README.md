# SXC - Standalone Shader Compiler

SXC (Shader eXecution Compiler) is a command line tool for batch compilation of HLSL shaders built on top of the QhenkiX graphics library. It provides parallel compilation with support for shader permutations and multiple shader models. It is heavily inspired by [ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake) (MIT License), but with a key difference being that it does not create a individual subprocess for every shader compilation, which should result in lower overhead. 

## Features

- **Batch Compilation**: Compile multiple shaders from configuration file
- **Parallel Processing**: Uses Intel TBB for efficient multi-threaded compilation
- **Shader Permutations**: Support for generating multiple variants with different defines
- **Multiple Shader Models**: Support for SM 5.0 through 6.6
- **Debug Support**: Optional debug information generation

## Basic Usage

By default, SXC will use the system installed DXC or FXC compiler DLL depending on the shader model. To use a specific DLL, place it in the same directory as SXC.exe.

```bash
SXC.exe -c <config_file> -sm <shader_model> -out <output_dir> [options]
```

### Required Arguments

- `-c, --config-path`: Path to the configuration file
- `-sm, --shader-model`: Target shader model (5_0, 5_1, 6_0, 6_1, 6_2, 6_3, 6_4, 6_5, 6_6)
- `-out, --output`: Output directory for compiled shaders

### Optional Arguments

- `-pdb, --pdb-path`: Output directory for PDB debug symbols
- `-i, --include-path`: Additional include directories (can be specified multiple times)
- `-g, --global-defines`: Global preprocessor defines for all shaders (can be specified multiple times)
- `-dbg, --debug-flag`: Enable debug information for all shaders
- `-o, --optimization`: Default optimization level (O0, O1, O2, O3) [default: O3]

**Note**: Paths are resolved relative to the configuration file's directory location.

## Configuration File Format

The configuration file contains one shader compilation job per line. Each line specifies the shader file and compilation parameters:

```
-p <shader_path> -e <entry_point> -st <shader_type> [options]
```

### Configuration Parameters

- `-p, --path`: Path to the HLSL shader file
- `-e, --entry-point`: Shader entry point function name
- `-st, --shader-type`: Shader type (`vs` for vertex, `ps` for pixel, `cs` for compute)
- `-out, --output-dir`: Override global output directory for this shader
- `-d, --define`: Preprocessor defines (supports permutation syntax)
- `-o, --optimization`: Override global optimization level

### Shader Permutations (In Progress)

SXC supports generating multiple shader variants using define permutations:

```
-p shader.hlsl -e main -st vs -d FEATURE_A={0,1} -d FEATURE_B={0,1}
```

This will generate 4 shader variants:
- `FEATURE_A=0, FEATURE_B=0`
- `FEATURE_A=0, FEATURE_B=1`
- `FEATURE_A=1, FEATURE_B=0`
- `FEATURE_A=1, FEATURE_B=1`

However currently only the last permutation is written to disk. Soon I will add support for outputting permutations into a single binary file with a header for offset info.

## Example

### Configuration File (`shaders.config`)
```
-p basic_vs.hlsl -e main -st vs
-p basic_ps.hlsl -e main -st ps -d USE_TEXTURE={0,1}
-p compute.hlsl -e CSMain -st cs -o O2
```

### Command Line
```bash
SXC.exe -c shaders.config -sm 6_0 -out compiled_shaders -i include_dir -g GLOBAL_DEFINE=1
```

## Dependencies

- [QhenkiX](https://github.com/AaronTian-stack/QhenkiX) - MIT License
- [Intel TBB](https://github.com/uxlfoundation/oneTBB) - Apache 2.0 License
- DirectX Shader Compiler (DXC) or FXC
