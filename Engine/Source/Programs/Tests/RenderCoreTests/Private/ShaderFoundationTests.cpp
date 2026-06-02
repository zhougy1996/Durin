#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "Shader/Shader.h"
#include "Shader/ShaderCacheStore.h"
#include "Shader/ShaderCompiler.h"

namespace Durin
{
	namespace
	{
		auto MakeCode(uint8 Seed) -> std::shared_ptr<FShaderCode>
		{
			auto Code = std::make_shared<FShaderCode>();
			Code->resize(16);
			for (size_t Index = 0; Index < Code->size(); ++Index)
			{
				(*Code)[Index] = static_cast<std::byte>(Seed + static_cast<uint8>(Index));
			}
			return Code;
		}

		auto MakeCompiledShader(
			EShaderFrequency Frequency,
			std::string EntryPoint,
			std::string DebugName,
			uint8 CodeSeed,
			FShaderReflectionData Reflection = {}
		) -> FCompiledShader
		{
			FCompiledShader CompiledShader;
			CompiledShader.Frequency = Frequency;
			CompiledShader.EntryPoint = std::move(EntryPoint);
			CompiledShader.DebugName = std::move(DebugName);
			CompiledShader.Code = MakeCode(CodeSeed);
			CompiledShader.Hash = FXxHash128::HashBuffer(*CompiledShader.Code);
			CompiledShader.Reflection = std::move(Reflection);
			return CompiledShader;
		}

		auto ExpectShaderEqual(const FCompiledShader& Actual, const FCompiledShader& Expected) -> void
		{
			ASSERT_EQ(Actual.Frequency, Expected.Frequency);
			EXPECT_EQ(Actual.EntryPoint, Expected.EntryPoint);
			EXPECT_EQ(Actual.DebugName, Expected.DebugName);
			EXPECT_EQ(Actual.Hash, Expected.Hash);
			ASSERT_TRUE(Actual.Code);
			ASSERT_TRUE(Expected.Code);
			ASSERT_EQ(Actual.Code->size(), Expected.Code->size());
			EXPECT_EQ(std::memcmp(Actual.Code->data(), Expected.Code->data(), Actual.Code->size()), 0);
			ASSERT_EQ(Actual.Reflection.ResourceBindings.size(), Expected.Reflection.ResourceBindings.size());
			for (size_t Index = 0; Index < Actual.Reflection.ResourceBindings.size(); ++Index)
			{
				EXPECT_EQ(Actual.Reflection.ResourceBindings[Index], Expected.Reflection.ResourceBindings[Index]);
			}
			ASSERT_EQ(Actual.Reflection.PushConstantRanges.size(), Expected.Reflection.PushConstantRanges.size());
			for (size_t Index = 0; Index < Actual.Reflection.PushConstantRanges.size(); ++Index)
			{
				EXPECT_EQ(Actual.Reflection.PushConstantRanges[Index].StageFlags, Expected.Reflection.PushConstantRanges[Index].StageFlags);
				EXPECT_EQ(Actual.Reflection.PushConstantRanges[Index].Offset, Expected.Reflection.PushConstantRanges[Index].Offset);
				EXPECT_EQ(Actual.Reflection.PushConstantRanges[Index].Size, Expected.Reflection.PushConstantRanges[Index].Size);
			}
		}

	}

	TEST(FShaderFoundationTests, ShaderMapLookupByTypeIsStable)
	{
		FShaderType VertexShaderType("UnitVertexShader", "/Unit/TestShader", EShaderFrequency::Vertex, "vertexMain");
		FShaderType PixelShaderType("UnitPixelShader", "/Unit/TestShader", EShaderFrequency::Pixel, "fragmentMain");

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 1),
			MakeCompiledShader(EShaderFrequency::Pixel, "fragmentMain", "UnitPixelShader", 21)
		};

		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &PixelShaderType};
		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;

		const uint32* VertexIndex = ShaderMap.FindShaderIndex(&VertexShaderType);
		const uint32* PixelIndex = ShaderMap.FindShaderIndex(&PixelShaderType);
		ASSERT_NE(VertexIndex, nullptr);
		ASSERT_NE(PixelIndex, nullptr);
		EXPECT_EQ(*VertexIndex, 0u);
		EXPECT_EQ(*PixelIndex, 1u);

		auto* PixelShader = ShaderMap.GetShader(&PixelShaderType);
		ASSERT_NE(PixelShader, nullptr);
		TShaderRef<FShader> PixelShaderRef(PixelShader, &ShaderMap);
		ASSERT_TRUE(PixelShaderRef);
		EXPECT_EQ(PixelShaderRef.GetShader()->GetShaderIndex(), 1u);
		EXPECT_EQ(PixelShaderRef.GetShader()->GetType(), &PixelShaderType);
	}

	TEST(FShaderFoundationTests, MakeShaderCreateDescPreservesFrequencyHashAndEntryPoint)
	{
		const FCompiledShader CompiledShader = MakeCompiledShader(EShaderFrequency::Pixel, "fragmentMain", "UnitPixelShader", 7);
		const FRHIShaderCreateDesc CreateDesc = MakeShaderCreateDesc(CompiledShader);

		EXPECT_EQ(CreateDesc.Frequency, EShaderFrequency::Pixel);
		EXPECT_EQ(CreateDesc.Hash, CompiledShader.Hash);
		EXPECT_STREQ(CreateDesc.EntryPoint, "fragmentMain");
		EXPECT_STREQ(CreateDesc.DebugName, "UnitPixelShader");
		ASSERT_EQ(CreateDesc.Code.size(), CompiledShader.Code->size());
		EXPECT_EQ(std::memcmp(CreateDesc.Code.data(), CompiledShader.Code->data(), CreateDesc.Code.size_bytes()), 0);
	}

	TEST(FShaderFoundationTests, BuildPipelineLayoutFromShadersMergesBindingsAndPushConstants)
	{
		FShaderReflectionData VertexReflection;
		VertexReflection.ResourceBindings.push_back({
			.Name = "SceneUniform",
			.StageFlags = EShaderStageFlags::Vertex,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});
		VertexReflection.PushConstantRanges.push_back({
			.StageFlags = EShaderStageFlags::Vertex,
			.Offset = 0,
			.Size = 16
		});

		FShaderReflectionData PixelReflection;
		PixelReflection.ResourceBindings.push_back({
			.Name = "SceneUniform",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});
		PixelReflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 1,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});
		PixelReflection.ResourceBindings.push_back({
			.Name = "FontSampler",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 2,
			.Type = ERHIBindingType::Sampler,
			.ArraySize = 1
		});

		std::vector<FCompiledShader> CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 3, VertexReflection),
			MakeCompiledShader(EShaderFrequency::Pixel, "fragmentMain", "UnitPixelShader", 4, PixelReflection)
		};

		FPipelineLayoutDesc PipelineLayout;
		std::string ErrorMessage;
		ASSERT_TRUE(BuildPipelineLayoutFromShaders(CompiledShaders, PipelineLayout, ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(PipelineLayout.BindingLayouts.size(), 1u);
		ASSERT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts.size(), 3u);

		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot, 0u);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[0].Type, ERHIBindingType::UniformBuffer);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[0].StageFlags, EShaderStageFlags::Vertex | EShaderStageFlags::Fragment);

		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[1].Slot, 1u);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[1].Type, ERHIBindingType::Texture);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[1].StageFlags, EShaderStageFlags::Fragment);

		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[2].Slot, 2u);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[2].Type, ERHIBindingType::Sampler);
		EXPECT_EQ(PipelineLayout.BindingLayouts[0].BindingLayouts[2].StageFlags, EShaderStageFlags::Fragment);

		ASSERT_EQ(PipelineLayout.PushConstantRanges.size(), 1u);
		EXPECT_EQ(PipelineLayout.PushConstantRanges[0].Offset, 0u);
		EXPECT_EQ(PipelineLayout.PushConstantRanges[0].Size, 16u);
		EXPECT_EQ(PipelineLayout.PushConstantRanges[0].StageFlags, EShaderStageFlags::Vertex);
	}

	TEST(FShaderFoundationTests, BuildPipelineLayoutFromShadersRejectsConflictingBindings)
	{
		FShaderReflectionData VertexReflection;
		VertexReflection.ResourceBindings.push_back({
			.Name = "ResourceA",
			.StageFlags = EShaderStageFlags::Vertex,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});

		FShaderReflectionData PixelReflection;
		PixelReflection.ResourceBindings.push_back({
			.Name = "ResourceA",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});

		std::vector<FCompiledShader> CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 8, VertexReflection),
			MakeCompiledShader(EShaderFrequency::Pixel, "fragmentMain", "UnitPixelShader", 9, PixelReflection)
		};

		FPipelineLayoutDesc PipelineLayout;
		std::string ErrorMessage;
		EXPECT_FALSE(BuildPipelineLayoutFromShaders(CompiledShaders, PipelineLayout, ErrorMessage));
		EXPECT_FALSE(ErrorMessage.empty());
	}

	TEST(FShaderFoundationTests, ShaderCacheRoundTripsReflectionSidecar)
	{
		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});
		Reflection.PushConstantRanges.push_back({
			.StageFlags = EShaderStageFlags::Vertex,
			.Offset = 0,
			.Size = 16
		});

		FShaderCompilerOutput SavedOutput;
		SavedOutput.bSucceeded = true;
		SavedOutput.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "CacheVertexShader", 12, Reflection)
		};

		FShaderCompileOptions Options;
		Options.EntryPoints = {"vertexMain"};
		Options.Frequencies = {EShaderFrequency::Vertex};

		FShaderVariantKey VariantKey;
		VariantKey.Hex = "unit-shader-cache";

		FShaderCacheStore CacheStore;
		ASSERT_TRUE(CacheStore.Save("/Unit/TestShader", Options, VariantKey, SavedOutput));

		FShaderCompilerOutput LoadedOutput;
		ASSERT_TRUE(CacheStore.TryLoad("/Unit/TestShader", Options, VariantKey, LoadedOutput));
		ASSERT_EQ(LoadedOutput.CompiledShaders.size(), 1u);
		ExpectShaderEqual(LoadedOutput.CompiledShaders[0], SavedOutput.CompiledShaders[0]);
	}
} // namespace Durin
