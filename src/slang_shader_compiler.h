#pragma once

#include <slang.h>

#include <shader_compiler.h>

namespace SXC
{
class SlangShaderCompiler final : public ShaderCompiler
{
    SlangSession* m_session = nullptr;

public:
    SlangShaderCompiler();
    SlangShaderCompiler(const SlangShaderCompiler&) = delete;
    SlangShaderCompiler& operator=(const SlangShaderCompiler&) = delete;

    static bool get_compiler_path(char* buffer, size_t length);

    bool compile(const CompilerInput& input, CompilerOutput* output, ShaderIR ir) override;

    ~SlangShaderCompiler() override;
};
} // namespace SXC
