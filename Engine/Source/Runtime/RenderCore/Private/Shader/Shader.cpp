#include "Shader/Shader.h"

#include "RHIResources.h"
#include "Shader/ShaderCompiler.h"

namespace Durin
{
	auto FShaderMapResourceCode::AddCompiledShader(const FCompiledShader& CompiledShader) -> void
	{
		const FShaderCodeResource NewCodeResource(CompiledShader.Code, CompiledShader.Frequency);
		ShaderCodeResources.push_back(NewCodeResource);
	}

	auto FShaderMapResource::AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void
	{
		for (const FCompiledShader& CompiledShader : Output.CompiledShaders)
		{
			Code->AddCompiledShader(CompiledShader);
		}
	}

	auto FShaderMapResource::CreateRHIShader(uint32 ShaderIndex, bool bRequired) -> FRHIShader*
	{
		const FShaderCodeView CodeView = Code->GetCodeView(ShaderIndex);

		FShaderRHIRef RHIShader;

		const FRHIShaderCreateDesc VertexShaderCreateDesc = FRHIShaderCreateDesc::CreateVertex("ImGuiVertexShader", CodeView, {});

		if (bRequired)
		{
			checkf(RHIShader, "Failed to create required shader");
		}

		Shaders.resize(ShaderIndex + 1);
		Shaders[ShaderIndex] = RHIShader;
		return RHIShader;
	}
} // namespace Durin