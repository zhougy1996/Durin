#include <gtest/gtest.h>

#include "Shader/Shader.h"
#include "Shader/SlangShaderCompiler.h"

#include <cstring>
#include <set>
#include <unordered_map>

namespace Durin
{
	namespace
	{
		auto FindBinding(
			const FCompiledShader& Shader,
			std::string_view Name) -> const FShaderResourceBinding*
		{
			const auto It = std::ranges::find(
				Shader.Reflection.ResourceBindings,
				Name,
				&FShaderResourceBinding::Name);
			return It == Shader.Reflection.ResourceBindings.end()
				? nullptr
				: &*It;
		}

		auto GetSpirvInputLocations(
			const FCompiledShader& Shader) -> std::set<uint32>
		{
			constexpr uint16 OpDecorate = 71;
			constexpr uint16 OpVariable = 59;
			constexpr uint32 DecorationLocation = 30;
			constexpr uint32 StorageClassInput = 1;
			std::vector<uint32> Words(
				Shader.Code->size() / sizeof(uint32));
			std::memcpy(
				Words.data(),
				Shader.Code->data(),
				Shader.Code->size());

			std::unordered_map<uint32, uint32> LocationsById;
			std::set<uint32> InputIds;
			for (size_t Offset = 5; Offset < Words.size();)
			{
				const uint32 Instruction = Words[Offset];
				const uint16 WordCount =
					static_cast<uint16>(Instruction >> 16);
				const uint16 OpCode =
					static_cast<uint16>(Instruction & 0xffffu);
				if (WordCount == 0
					|| Offset + WordCount > Words.size())
				{
					return {};
				}
				if (OpCode == OpDecorate && WordCount >= 4
					&& Words[Offset + 2] == DecorationLocation)
				{
					LocationsById.emplace(
						Words[Offset + 1],
						Words[Offset + 3]);
				}
				else if (OpCode == OpVariable && WordCount >= 4
					&& Words[Offset + 3] == StorageClassInput)
				{
					InputIds.insert(Words[Offset + 2]);
				}
				Offset += WordCount;
			}

			std::set<uint32> InputLocations;
			for (const uint32 InputId : InputIds)
			{
				if (const auto It = LocationsById.find(InputId);
					It != LocationsById.end())
				{
					InputLocations.insert(It->second);
				}
			}
			return InputLocations;
		}

		auto ExpectBinding(
			const FCompiledShader& Shader,
			std::string_view Name,
			uint32 BindingIndex,
			ERHIBindingType Type,
			EShaderStageFlags StageFlags) -> void
		{
			const FShaderResourceBinding* Binding =
				FindBinding(Shader, Name);
			ASSERT_NE(Binding, nullptr) << Name;
			EXPECT_EQ(Binding->SetIndex, 0u) << Name;
			EXPECT_EQ(Binding->BindingIndex, BindingIndex) << Name;
			EXPECT_EQ(Binding->Type, Type) << Name;
			EXPECT_EQ(Binding->ArraySize, 1u) << Name;
			EXPECT_EQ(Binding->StageFlags, StageFlags) << Name;
		}
	}

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

	TEST(FShaderReflectionTests,
		StaticMeshBasePassModuleExtractionPreservesShaderAbi)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex,
			EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << Output.ErrorMessage;
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FCompiledShader& VertexShader =
			Output.CompiledShaders[0];
		const FCompiledShader& FragmentShader =
			Output.CompiledShaders[1];

		EXPECT_EQ(VertexShader.SourceEntryPoint, "VertexMain");
		EXPECT_EQ(FragmentShader.SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(VertexShader.BinaryEntryPoint, "main");
		EXPECT_EQ(FragmentShader.BinaryEntryPoint, "main");
		EXPECT_EQ(
			GetSpirvInputLocations(VertexShader),
			(std::set<uint32>{0, 1, 2, 3, 4, 5, 6, 7}));

		ASSERT_EQ(
			VertexShader.Reflection.ResourceBindings.size(), 1u);
		ExpectBinding(
			VertexShader,
			"Transform",
			0,
			ERHIBindingType::UniformBuffer,
			EShaderStageFlags::Vertex);
		ASSERT_EQ(
			FragmentShader.Reflection.ResourceBindings.size(), 22u);
		ExpectBinding(
			FragmentShader,
			"Lighting",
			1,
			ERHIBindingType::UniformBuffer,
			EShaderStageFlags::Fragment);
		ExpectBinding(
			FragmentShader,
			"Material",
			2,
			ERHIBindingType::UniformBuffer,
			EShaderStageFlags::Fragment);
		ExpectBinding(
			FragmentShader,
			"BaseColorTexture",
			3,
			ERHIBindingType::Texture,
			EShaderStageFlags::Fragment);
		ExpectBinding(
			FragmentShader,
			"NormalTexture",
			4,
			ERHIBindingType::Texture,
			EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "MetallicTexture", 5,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "RoughnessTexture", 6,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "AmbientOcclusionTexture", 7,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EmissiveTexture", 8,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "OpacityTexture", 9,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "OpacityMaskTexture", 10,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(
			FragmentShader,
			"BaseColorSampler",
			11,
			ERHIBindingType::Sampler,
			EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "NormalSampler", 12,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "MetallicSampler", 13,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "RoughnessSampler", 14,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "AmbientOcclusionSampler", 15,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EmissiveSampler", 16,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "OpacitySampler", 17,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "OpacityMaskSampler", 18,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EnvironmentIrradiance", 19,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EnvironmentPrefiltered", 20,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EnvironmentBrdfLut", 21,
			ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(FragmentShader, "EnvironmentSampler", 22,
			ERHIBindingType::Sampler, EShaderStageFlags::Fragment);

		FPipelineLayoutDesc PipelineLayout;
		std::string ErrorMessage;
		ASSERT_TRUE(BuildPipelineLayoutFromShaders(
			Output.CompiledShaders,
			PipelineLayout,
			ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(PipelineLayout.BindingLayouts.size(), 1u);
		const auto& SetLayout =
			PipelineLayout.BindingLayouts[0].BindingLayouts;
		ASSERT_EQ(SetLayout.size(), 23u);
		for (uint32 BindingIndex = 0;
			BindingIndex < SetLayout.size();
			++BindingIndex)
		{
			EXPECT_EQ(
				SetLayout[BindingIndex].Slot,
				BindingIndex);
		}
		EXPECT_EQ(
			SetLayout[0].Type,
			ERHIBindingType::UniformBuffer);
		EXPECT_EQ(
			SetLayout[0].StageFlags,
			EShaderStageFlags::Vertex);
		EXPECT_EQ(
			SetLayout[1].Type,
			ERHIBindingType::UniformBuffer);
		EXPECT_EQ(
			SetLayout[1].StageFlags,
			EShaderStageFlags::Fragment);
		EXPECT_EQ(
			SetLayout[2].Type,
			ERHIBindingType::UniformBuffer);
		EXPECT_EQ(
			SetLayout[2].StageFlags,
			EShaderStageFlags::Fragment);
		for (uint32 BindingIndex = 3; BindingIndex <= 10; ++BindingIndex)
		{
			EXPECT_EQ(SetLayout[BindingIndex].Type, ERHIBindingType::Texture);
			EXPECT_EQ(
				SetLayout[BindingIndex].StageFlags,
				EShaderStageFlags::Fragment);
		}
		for (uint32 BindingIndex = 11; BindingIndex <= 18; ++BindingIndex)
		{
			EXPECT_EQ(SetLayout[BindingIndex].Type, ERHIBindingType::Sampler);
			EXPECT_EQ(SetLayout[BindingIndex].StageFlags, EShaderStageFlags::Fragment);
		}
		for (uint32 BindingIndex = 19; BindingIndex <= 21; ++BindingIndex)
		{
			EXPECT_EQ(SetLayout[BindingIndex].Type, ERHIBindingType::Texture);
			EXPECT_EQ(SetLayout[BindingIndex].StageFlags, EShaderStageFlags::Fragment);
		}
		EXPECT_EQ(SetLayout[22].Type, ERHIBindingType::Sampler);
		EXPECT_EQ(SetLayout[22].StageFlags, EShaderStageFlags::Fragment);
		EXPECT_TRUE(PipelineLayout.PushConstantRanges.empty());
	}

	TEST(FShaderReflectionTests, TerrainBasePassUsesExactIntegerHeightBinding)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		Options.Macros.emplace_back("DURIN_TERRAIN", "1");
		Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "0");
		Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "1");
		Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", "1056964608");
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << Output.ErrorMessage;
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FCompiledShader& Vertex = Output.CompiledShaders[0];
		EXPECT_EQ(GetSpirvInputLocations(Vertex), (std::set<uint32>{0}));
		ASSERT_EQ(Vertex.Reflection.ResourceBindings.size(), 3u);
		ExpectBinding(Vertex, "Transform", 0, ERHIBindingType::UniformBuffer, EShaderStageFlags::Vertex);
		ExpectBinding(Vertex, "HeightTexture", 23, ERHIBindingType::Texture, EShaderStageFlags::Vertex);
		ExpectBinding(Vertex, "Terrain", 24, ERHIBindingType::UniformBuffer, EShaderStageFlags::Vertex);
		EXPECT_EQ(Output.CompiledShaders[1].Reflection.ResourceBindings.size(), 22u);
	}
} // namespace Durin
