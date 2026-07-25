#include <gtest/gtest.h>

#include "Shader/Shader.h"
#include "Shader/SlangShaderCompiler.h"

namespace Durin
{
	TEST(FShaderReflectionTests, ReflectsStorageBuffersAndImages)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "StorageResources.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << Output.ErrorMessage;
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);

		const auto& Bindings = Output.CompiledShaders[0].Reflection.ResourceBindings;
		ASSERT_EQ(Bindings.size(), 3u);
		EXPECT_EQ(Bindings[0].Name, "ReadBuffer");
		EXPECT_EQ(Bindings[0].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(Bindings[1].Name, "WriteBuffer");
		EXPECT_EQ(Bindings[1].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(Bindings[2].Name, "OutputImage");
		EXPECT_EQ(Bindings[2].Type, ERHIBindingType::StorageImage);
	}

	TEST(FShaderReflectionTests, FragmentDepthOutputCompilesWithoutPipelineBindings)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "FragmentDepth.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << Output.ErrorMessage;
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);

		const FCompiledShader& Shader = Output.CompiledShaders[0];
		EXPECT_EQ(Shader.Frequency, EShaderFrequency::Fragment);
		EXPECT_FALSE(Shader.Code->empty());
		EXPECT_TRUE(Shader.Reflection.ResourceBindings.empty());
		EXPECT_TRUE(Shader.Reflection.PushConstantRanges.empty());

		FPipelineLayoutDesc PipelineLayout;
		std::string ErrorMessage;
		ASSERT_TRUE(BuildPipelineLayoutFromShaders(Output.CompiledShaders, PipelineLayout, ErrorMessage)) << ErrorMessage;
		EXPECT_TRUE(PipelineLayout.BindingLayouts.empty());
		EXPECT_TRUE(PipelineLayout.PushConstantRanges.empty());
	}
} // namespace Durin
