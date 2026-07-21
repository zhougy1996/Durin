#include <gtest/gtest.h>

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
} // namespace Durin
