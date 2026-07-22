#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include "Misc/Paths.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		class FStaticVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FStaticVertexShader, FShader, "/Unit/StaticShader", EShaderFrequency::Vertex, "vertexMain");
		};

		class FStaticFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(FontTexture);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FStaticFragmentShader, FShader, "/Unit/StaticShader", EShaderFrequency::Fragment, "fragmentMain");
		};

		class FStorageFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStorageFragmentShader)
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(DataBuffer);
				DURIN_SHADER_PARAMETER_STORAGE_IMAGE(OutputImage);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FStorageFragmentShader, FShader, "/Unit/StorageShader", EShaderFrequency::Fragment, "fragmentMain");
		};

		class FIntermediateShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FIntermediateShader, FShader, "/Unit/InheritanceShader", EShaderFrequency::Vertex, "baseMain");
		};

		class FDerivedShaderNoParameters : public FIntermediateShader
		{
		public:
			DURIN_DECLARE_SHADER(FDerivedShaderNoParameters, FIntermediateShader, "/Unit/InheritanceShader", EShaderFrequency::Vertex, "derivedNoParamsMain");
		};

		class FDerivedShaderWithParameters : public FIntermediateShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FDerivedShaderWithParameters)
				DURIN_SHADER_PARAMETER_TEXTURE(FontTexture);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FDerivedShaderWithParameters, FIntermediateShader, "/Unit/InheritanceShader", EShaderFrequency::Fragment, "derivedWithParamsMain");
		};

		class FIncludedParametersShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FIncludedParametersShader)
				DURIN_SHADER_PARAMETER_TEXTURE(FontTexture);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FIncludedParametersShader, FShader, "/Unit/IncludeShader", EShaderFrequency::Fragment, "includeBaseMain");
		};

		class FExplicitIncludeOnlyShader : public FIntermediateShader
		{
		public:
			DURIN_INCLUDE_SHADER_PARAMETERS_FOR(FExplicitIncludeOnlyShader, FIncludedParametersShader);
			DURIN_DECLARE_SHADER(FExplicitIncludeOnlyShader, FIntermediateShader, "/Unit/IncludeShader", EShaderFrequency::Fragment, "includeOnlyMain");
		};

		class FExplicitIncludeWithOwnParametersShader : public FIntermediateShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FExplicitIncludeWithOwnParametersShader)
				DURIN_SHADER_PARAMETER_SAMPLER(FontSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_INCLUDE_SHADER_PARAMETERS(FIncludedParametersShader);

			DURIN_DECLARE_SHADER(FExplicitIncludeWithOwnParametersShader, FIntermediateShader, "/Unit/IncludeShader", EShaderFrequency::Fragment, "includeOwnMain");
		};

		class FNamedShaderAlias : public FShader
		{
		public:
			DURIN_DECLARE_SHADER_NAMED(FNamedShaderAlias, FShader, "ExplicitNamedShader", "/Unit/NamedShader", EShaderFrequency::Fragment, "namedMain");
		};

		template<typename ParameterStruct, size_t N>
		auto MakeTestParametersMetadata(const std::array<FShaderParameterMemberMetadata, N>& Members) -> FShaderParametersMetadata
		{
			return MakeInlineShaderParametersMetadata<ParameterStruct>("FParameters", Members);
		}

		auto MakeCode(uint8 Seed) -> std::shared_ptr<std::vector<std::byte>>
		{
			auto Code = std::make_shared<std::vector<std::byte>>();
			Code->resize(16);
			for (size_t Index = 0; Index < Code->size(); ++Index)
			{
				(*Code)[Index] = static_cast<std::byte>(Seed + static_cast<uint8>(Index));
			}
			return Code;
		}

		auto MakeCompiledShader(
			EShaderFrequency Frequency,
			std::string SourceEntryPoint,
			std::string DebugName,
			uint8 CodeSeed,
			FShaderReflectionData Reflection = {},
			std::string BinaryEntryPoint = "main"
		) -> FCompiledShader
		{
			FCompiledShader CompiledShader;
			CompiledShader.Frequency = Frequency;
			CompiledShader.SourceEntryPoint = std::move(SourceEntryPoint);
			CompiledShader.BinaryEntryPoint = std::move(BinaryEntryPoint);
			CompiledShader.DebugName = std::move(DebugName);
			CompiledShader.Code = MakeCode(CodeSeed);
			CompiledShader.Hash = FXxHash128::HashBuffer(*CompiledShader.Code);
			CompiledShader.Reflection = std::move(Reflection);
			return CompiledShader;
		}

		auto ExpectShaderEqual(const FCompiledShader& Actual, const FCompiledShader& Expected) -> void
		{
			ASSERT_EQ(Actual.Frequency, Expected.Frequency);
			EXPECT_EQ(Actual.SourceEntryPoint, Expected.SourceEntryPoint);
			EXPECT_EQ(Actual.BinaryEntryPoint, Expected.BinaryEntryPoint);
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
		FShaderType FragmentShaderType("UnitFragmentShader", "/Unit/TestShader", EShaderFrequency::Fragment, "fragmentMain");

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 1),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 21)
		};

		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;

		const uint32* VertexIndex = ShaderMap.FindShaderIndex(&VertexShaderType);
		const uint32* FragmentIndex = ShaderMap.FindShaderIndex(&FragmentShaderType);
		ASSERT_NE(VertexIndex, nullptr);
		ASSERT_NE(FragmentIndex, nullptr);
		EXPECT_EQ(*VertexIndex, 0u);
		EXPECT_EQ(*FragmentIndex, 1u);

		auto* FragmentShader = ShaderMap.GetShader(&FragmentShaderType);
		ASSERT_NE(FragmentShader, nullptr);
		TShaderRef<FShader> FragmentShaderRef(FragmentShader, &ShaderMap);
		ASSERT_TRUE(FragmentShaderRef);
		EXPECT_EQ(FragmentShaderRef.GetShader()->GetType(), &FragmentShaderType);
	}

	TEST(FShaderFoundationTests, ShaderMapInitializeReusesCachedResourcesForEquivalentIdentity)
	{
		FShaderType VertexShaderType("UnitVertexShader", "/Unit/TestShader", EShaderFrequency::Vertex, "vertexMain");
		FShaderType FragmentShaderType("UnitFragmentShader", "/Unit/TestShader", EShaderFrequency::Fragment, "fragmentMain");

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 1),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 21)
		};

		FShaderCompileOptions CompileOptions;
		CompileOptions.VirtualShaderPath = "/Unit/TestShader";
		CompileOptions.EntryPoints = {"vertexMain", "fragmentMain"};
		CompileOptions.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};

		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
		FShaderMapBase ShaderMapA;
		FShaderMapBase ShaderMapB;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMapA.Initialize(ShaderTypes, Output, CompileOptions, ErrorMessage)) << ErrorMessage;
		ASSERT_TRUE(ShaderMapB.Initialize(ShaderTypes, Output, CompileOptions, ErrorMessage)) << ErrorMessage;

		EXPECT_FALSE(ShaderMapA.GetCacheKey().IsZero());
		EXPECT_EQ(ShaderMapA.GetCacheKey(), ShaderMapB.GetCacheKey());
		EXPECT_EQ(ShaderMapA.GetCode(), ShaderMapB.GetCode());
		EXPECT_EQ(ShaderMapA.GetResource(), ShaderMapB.GetResource());
		ASSERT_EQ(ShaderMapA.GetMergedPipelineLayout().BindingLayouts.size(), ShaderMapB.GetMergedPipelineLayout().BindingLayouts.size());
		ASSERT_EQ(ShaderMapA.GetMergedPipelineLayout().PushConstantRanges.size(), ShaderMapB.GetMergedPipelineLayout().PushConstantRanges.size());
	}

	TEST(FShaderFoundationTests, ShaderMapResourceCacheReleasesExpiredEntries)
	{
		ClearShaderMapResourceCache();
		FShaderType ShaderType("ReclaimableShader", "/Unit/ReclaimableShader", EShaderFrequency::Vertex, "vertexMain");
		const std::array<const FShaderType*, 1> ShaderTypes = {&ShaderType};
		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "ReclaimableShader", 77)};
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Unit/ReclaimableShader";
		Options.EntryPoints = {"vertexMain"};
		Options.Frequencies = {EShaderFrequency::Vertex};

		{
			FShaderMapBase ShaderMap;
			std::string ErrorMessage;
			ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, Options, ErrorMessage)) << ErrorMessage;
			const FShaderMapResourceCacheStats Stats = GetShaderMapResourceCacheStats();
			EXPECT_EQ(Stats.EntryCount, 1u);
			EXPECT_EQ(Stats.LiveEntryCount, 1u);
		}

		const FShaderMapResourceCacheStats ReleasedStats = GetShaderMapResourceCacheStats();
		EXPECT_EQ(ReleasedStats.EntryCount, 0u);
		EXPECT_EQ(ReleasedStats.LiveEntryCount, 0u);
	}

	TEST(FShaderFoundationTests, ShaderMapInitializeSeparatesCachedResourcesForMacroOrBytecodeChanges)
	{
		FShaderType VertexShaderType("UnitVertexShader", "/Unit/TestShader", EShaderFrequency::Vertex, "vertexMain");
		FShaderType FragmentShaderType("UnitFragmentShader", "/Unit/TestShader", EShaderFrequency::Fragment, "fragmentMain");
		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};

		FShaderCompilerOutput OutputA;
		OutputA.bSucceeded = true;
		OutputA.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 2),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 22)
		};

		FShaderCompilerOutput OutputB = OutputA;
		OutputB.CompiledShaders[1] = MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShaderVariant", 23);

		FShaderCompileOptions BaseOptions;
		BaseOptions.VirtualShaderPath = "/Unit/TestShader";
		BaseOptions.EntryPoints = {"vertexMain", "fragmentMain"};
		BaseOptions.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};

		FShaderCompileOptions MacroOptions = BaseOptions;
		MacroOptions.Macros.emplace_back("USE_VARIANT");

		FShaderMapBase ShaderMapBaseIdentity;
		FShaderMapBase ShaderMapMacroVariant;
		FShaderMapBase ShaderMapBytecodeVariant;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMapBaseIdentity.Initialize(ShaderTypes, OutputA, BaseOptions, ErrorMessage)) << ErrorMessage;
		ASSERT_TRUE(ShaderMapMacroVariant.Initialize(ShaderTypes, OutputA, MacroOptions, ErrorMessage)) << ErrorMessage;
		ASSERT_TRUE(ShaderMapBytecodeVariant.Initialize(ShaderTypes, OutputB, BaseOptions, ErrorMessage)) << ErrorMessage;

		EXPECT_NE(ShaderMapBaseIdentity.GetCacheKey(), ShaderMapMacroVariant.GetCacheKey());
		EXPECT_NE(ShaderMapBaseIdentity.GetResource(), ShaderMapMacroVariant.GetResource());
		EXPECT_NE(ShaderMapBaseIdentity.GetCacheKey(), ShaderMapBytecodeVariant.GetCacheKey());
		EXPECT_NE(ShaderMapBaseIdentity.GetResource(), ShaderMapBytecodeVariant.GetResource());
	}

	TEST(FShaderFoundationTests, ShaderMacroDefinitionPreservesPresenceOnlyAndExplicitValueSemantics)
	{
		const FShaderMacroDefinition PresenceOnlyMacro("USE_VARIANT");
		EXPECT_EQ(PresenceOnlyMacro.Name, "USE_VARIANT");
		EXPECT_FALSE(PresenceOnlyMacro.HasValue());
		EXPECT_FALSE(PresenceOnlyMacro.Value.has_value());

		const std::string MacroName = "QUALITY_LEVEL";
		const std::string MacroValue = "2";
		const FShaderMacroDefinition ExplicitValueMacro(MacroName, MacroValue);
		EXPECT_EQ(ExplicitValueMacro.Name, MacroName);
		ASSERT_TRUE(ExplicitValueMacro.HasValue());
		EXPECT_EQ(*ExplicitValueMacro.Value, MacroValue);
		EXPECT_EQ(MacroName, "QUALITY_LEVEL");
		EXPECT_EQ(MacroValue, "2");
	}

	TEST(FShaderFoundationTests, ShaderMapCacheKeyDistinguishesPresenceOnlyMacroFromExplicitValue)
	{
		FShaderType VertexShaderType("UnitVertexShader", "/Unit/TestShader", EShaderFrequency::Vertex, "vertexMain");
		std::array<const FShaderType*, 1> ShaderTypes = {&VertexShaderType};

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 5)
		};

		FShaderCompileOptions PresenceOnlyOptions;
		PresenceOnlyOptions.VirtualShaderPath = "/Unit/TestShader";
		PresenceOnlyOptions.EntryPoints = {"vertexMain"};
		PresenceOnlyOptions.Frequencies = {EShaderFrequency::Vertex};
		PresenceOnlyOptions.Macros.emplace_back("USE_VARIANT");

		FShaderCompileOptions ExplicitValueOptions = PresenceOnlyOptions;
		ExplicitValueOptions.Macros.clear();
		ExplicitValueOptions.Macros.emplace_back("USE_VARIANT", "1");

		FShaderMapBase PresenceOnlyShaderMap;
		FShaderMapBase ExplicitValueShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(PresenceOnlyShaderMap.Initialize(ShaderTypes, Output, PresenceOnlyOptions, ErrorMessage)) << ErrorMessage;
		ASSERT_TRUE(ExplicitValueShaderMap.Initialize(ShaderTypes, Output, ExplicitValueOptions, ErrorMessage)) << ErrorMessage;

		EXPECT_NE(PresenceOnlyShaderMap.GetCacheKey(), ExplicitValueShaderMap.GetCacheKey());
	}

	TEST(FShaderFoundationTests, MakeShaderCreateDescPreservesFrequencyHashAndUsesBackendEntryPoint)
	{
		const FCompiledShader CompiledShader = MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 7);
		const FRHIShaderCreateDesc CreateDesc = MakeShaderCreateDesc(CompiledShader);

		EXPECT_EQ(CreateDesc.Frequency, EShaderFrequency::Fragment);
		EXPECT_EQ(CreateDesc.Hash, CompiledShader.Hash);
		EXPECT_STREQ(CreateDesc.EntryPoint, "main");
		EXPECT_STREQ(CreateDesc.DebugName, "UnitFragmentShader");
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

		FShaderReflectionData FragmentReflection;
		FragmentReflection.ResourceBindings.push_back({
			.Name = "SceneUniform",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});
		FragmentReflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 1,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});
		FragmentReflection.ResourceBindings.push_back({
			.Name = "FontSampler",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 2,
			.Type = ERHIBindingType::Sampler,
			.ArraySize = 1
		});

		std::vector<FCompiledShader> CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 3, VertexReflection),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 4, FragmentReflection)
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

		FShaderReflectionData FragmentReflection;
		FragmentReflection.ResourceBindings.push_back({
			.Name = "ResourceA",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});

		std::vector<FCompiledShader> CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitVertexShader", 8, VertexReflection),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 9, FragmentReflection)
		};

		FPipelineLayoutDesc PipelineLayout;
		std::string ErrorMessage;
		EXPECT_FALSE(BuildPipelineLayoutFromShaders(CompiledShaders, PipelineLayout, ErrorMessage));
		EXPECT_FALSE(ErrorMessage.empty());
	}

	TEST(FShaderFoundationTests, BuildShaderParameterBindingsResolvesReflectionSlots)
	{
		struct FParameters
		{
			FRHITexture* FontTexture = nullptr;
			FRHISampler* FontSampler = nullptr;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, decltype(FParameters::FontTexture)>("FontTexture", static_cast<uint32>(offsetof(FParameters, FontTexture))),
			MakeShaderParameterMemberMetadata<ERHIBindingType::Sampler, decltype(FParameters::FontSampler)>("FontSampler", static_cast<uint32>(offsetof(FParameters, FontSampler)))
		};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 2,
			.BindingIndex = 4,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});
		Reflection.ResourceBindings.push_back({
			.Name = "FontSampler",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 2,
			.BindingIndex = 5,
			.Type = ERHIBindingType::Sampler,
			.ArraySize = 1
		});

		std::vector<FShaderParameterBinding> Bindings;
		std::string ErrorMessage;
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		ASSERT_TRUE(BuildShaderParameterBindings(&ParametersMetadata, Reflection, Bindings, ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(Bindings.size(), 2u);
		EXPECT_STREQ(Bindings[0].Name, "FontTexture");
		EXPECT_EQ(Bindings[0].Offset, offsetof(FParameters, FontTexture));
		EXPECT_EQ(Bindings[0].SetIndex, 2u);
		EXPECT_EQ(Bindings[0].BindingIndex, 4u);
		EXPECT_EQ(Bindings[0].Type, ERHIBindingType::Texture);
		EXPECT_STREQ(Bindings[1].Name, "FontSampler");
		EXPECT_EQ(Bindings[1].SetIndex, 2u);
		EXPECT_EQ(Bindings[1].BindingIndex, 5u);
		EXPECT_EQ(Bindings[1].Type, ERHIBindingType::Sampler);
	}

	TEST(FShaderFoundationTests, BuildShaderParameterBindingsResolvesStorageResources)
	{
		struct FParameters
		{
			FRHIStorageBufferRange DataBuffer;
			FRHITexture* OutputImage = nullptr;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::StorageBuffer, decltype(FParameters::DataBuffer)>("DataBuffer", static_cast<uint32>(offsetof(FParameters, DataBuffer))),
			MakeShaderParameterMemberMetadata<ERHIBindingType::StorageImage, decltype(FParameters::OutputImage)>("OutputImage", static_cast<uint32>(offsetof(FParameters, OutputImage)))
		};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "DataBuffer",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 1,
			.BindingIndex = 2,
			.Type = ERHIBindingType::StorageBuffer,
			.ArraySize = 1
		});
		Reflection.ResourceBindings.push_back({
			.Name = "OutputImage",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 1,
			.BindingIndex = 3,
			.Type = ERHIBindingType::StorageImage,
			.ArraySize = 1
		});

		std::vector<FShaderParameterBinding> Bindings;
		std::string ErrorMessage;
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		ASSERT_TRUE(BuildShaderParameterBindings(&ParametersMetadata, Reflection, Bindings, ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(Bindings.size(), 2u);
		EXPECT_EQ(Bindings[0].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(Bindings[0].SetIndex, 1u);
		EXPECT_EQ(Bindings[0].BindingIndex, 2u);
		EXPECT_EQ(Bindings[1].Type, ERHIBindingType::StorageImage);
		EXPECT_EQ(Bindings[1].SetIndex, 1u);
		EXPECT_EQ(Bindings[1].BindingIndex, 3u);
	}

	TEST(FShaderFoundationTests, BuildShaderParameterBindingsAllowsDynamicUniformMetadataForConstantBufferReflection)
	{
		struct FParameters
		{
			FRHIUniformBufferRange SceneUniform;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::UniformBufferDynamic, decltype(FParameters::SceneUniform)>("SceneUniform", static_cast<uint32>(offsetof(FParameters, SceneUniform)))
		};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "SceneUniform",
			.StageFlags = EShaderStageFlags::Vertex,
			.SetIndex = 1,
			.BindingIndex = 2,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});

		std::vector<FShaderParameterBinding> Bindings;
		std::string ErrorMessage;
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		ASSERT_TRUE(BuildShaderParameterBindings(&ParametersMetadata, Reflection, Bindings, ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(Bindings.size(), 1u);
		EXPECT_EQ(Bindings[0].Type, ERHIBindingType::UniformBufferDynamic);
		EXPECT_EQ(Bindings[0].SetIndex, 1u);
		EXPECT_EQ(Bindings[0].BindingIndex, 2u);
		EXPECT_EQ(Bindings[0].Offset, offsetof(FParameters, SceneUniform));
	}

	TEST(FShaderFoundationTests, ShaderMapInitializeUsesDynamicUniformMetadataInPipelineLayout)
	{
		struct FParameters
		{
			FRHIUniformBufferRange SceneUniform;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::UniformBufferDynamic, decltype(FParameters::SceneUniform)>("SceneUniform", static_cast<uint32>(offsetof(FParameters, SceneUniform)))
		};
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		FShaderType VertexShaderType(
			"UnitDynamicUniformVertexShader",
			"/Unit/TestShader",
			EShaderFrequency::Vertex,
			"vertexMain",
			{},
			nullptr,
			nullptr,
			nullptr,
			&ParametersMetadata
		);
		std::array<const FShaderType*, 1> ShaderTypes = {&VertexShaderType};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "SceneUniform",
			.StageFlags = EShaderStageFlags::Vertex,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::UniformBuffer,
			.ArraySize = 1
		});

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "UnitDynamicUniformVertexShader", 14, Reflection)
		};

		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;
		ASSERT_EQ(ShaderMap.GetMergedPipelineLayout().BindingLayouts.size(), 1u);
		ASSERT_EQ(ShaderMap.GetMergedPipelineLayout().BindingLayouts[0].BindingLayouts.size(), 1u);
		EXPECT_EQ(ShaderMap.GetMergedPipelineLayout().BindingLayouts[0].BindingLayouts[0].Type, ERHIBindingType::UniformBufferDynamic);
	}

	TEST(FShaderFoundationTests, BuildShaderParameterBindingsRejectsMissingReflectionBinding)
	{
		struct FParameters
		{
			FRHITexture* MissingTexture = nullptr;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, decltype(FParameters::MissingTexture)>("MissingTexture", 0)
		};

		FShaderReflectionData Reflection;
		std::vector<FShaderParameterBinding> Bindings;
		std::string ErrorMessage;
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		EXPECT_FALSE(BuildShaderParameterBindings(&ParametersMetadata, Reflection, Bindings, ErrorMessage));
		EXPECT_FALSE(ErrorMessage.empty());
		EXPECT_TRUE(Bindings.empty());
	}

	TEST(FShaderFoundationTests, BuildShaderParameterBindingsRejectsTypeMismatch)
	{
		struct FParameters
		{
			FRHITexture* FontTexture = nullptr;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, decltype(FParameters::FontTexture)>("FontTexture", 0)
		};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 0,
			.Type = ERHIBindingType::Sampler,
			.ArraySize = 1
		});

		std::vector<FShaderParameterBinding> Bindings;
		std::string ErrorMessage;
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		EXPECT_FALSE(BuildShaderParameterBindings(&ParametersMetadata, Reflection, Bindings, ErrorMessage));
		EXPECT_FALSE(ErrorMessage.empty());
		EXPECT_TRUE(Bindings.empty());
	}

	TEST(FShaderFoundationTests, ShaderMapInitializeCachesParameterBindingsOnShaderInstance)
	{
		struct FParameters
		{
			FRHITexture* FontTexture = nullptr;
		};

		const std::array Metadata = {
			MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, decltype(FParameters::FontTexture)>("FontTexture", static_cast<uint32>(offsetof(FParameters, FontTexture)))
		};
		const FShaderParametersMetadata ParametersMetadata = MakeTestParametersMetadata<FParameters>(Metadata);
		FShaderType FragmentShaderType(
			"UnitFragmentShader",
			"/Unit/TestShader",
			EShaderFrequency::Fragment,
			"fragmentMain",
			{},
			nullptr,
			nullptr,
			nullptr,
			&ParametersMetadata
		);
		std::array<const FShaderType*, 1> ShaderTypes = {&FragmentShaderType};

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 3,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "UnitFragmentShader", 13, Reflection)
		};

		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;
		const FShader* Shader = ShaderMap.GetShader(&FragmentShaderType);
		ASSERT_NE(Shader, nullptr);

		const auto FirstBindings = Shader->GetParameterBindings();
		const auto SecondBindings = Shader->GetParameterBindings();
		ASSERT_EQ(FirstBindings.size(), 1u);
		EXPECT_EQ(FirstBindings.data(), SecondBindings.data());
		EXPECT_EQ(FirstBindings[0].BindingIndex, 3u);
		EXPECT_EQ(FirstBindings[0].Offset, offsetof(FParameters, FontTexture));
	}

	TEST(FShaderFoundationTests, ShaderDeclarationMacrosCreateStaticTypesAndParameterMetadata)
	{
		FShaderType& VertexShaderType = FStaticVertexShader::StaticType();
		FShaderType& FragmentShaderType = FStaticFragmentShader::StaticType();

		EXPECT_EQ(&FStaticVertexShader::StaticType(), &VertexShaderType);
		EXPECT_EQ(VertexShaderType.GetName(), "FStaticVertexShader");
		EXPECT_EQ(VertexShaderType.GetVirtualShaderPath(), "/Unit/StaticShader");
		EXPECT_EQ(VertexShaderType.GetEntryPoint(), "vertexMain");

		const auto ParameterMetadata = FragmentShaderType.GetParameterMetadata();
		ASSERT_EQ(ParameterMetadata.size(), 1u);
		EXPECT_STREQ(ParameterMetadata[0].Name, "FontTexture");
		EXPECT_EQ(ParameterMetadata[0].Kind, EShaderParameterMemberKind::Resource);
		EXPECT_EQ(ParameterMetadata[0].Size, sizeof(decltype(FStaticFragmentShader::FParameters::FontTexture)));
		EXPECT_EQ(FragmentShaderType.GetParametersMetadata()->StructSize, sizeof(FStaticFragmentShader::FParameters));

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 2,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "vertexMain", "StaticVertexShader", 3),
			MakeCompiledShader(EShaderFrequency::Fragment, "fragmentMain", "StaticFragmentShader", 7, Reflection)
		};

		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;

		const auto* FragmentShader = static_cast<const FStaticFragmentShader*>(ShaderMap.GetShader(&FragmentShaderType));
		ASSERT_NE(FragmentShader, nullptr);
		const auto Bindings = FragmentShader->GetParameterBindings();
		ASSERT_EQ(Bindings.size(), 1u);
		EXPECT_EQ(Bindings[0].BindingIndex, 2u);
		EXPECT_EQ(Bindings[0].Offset, offsetof(FStaticFragmentShader::FParameters, FontTexture));
	}

	TEST(FShaderFoundationTests, StorageShaderParameterMacrosCreateTypedMetadata)
	{
		FShaderType& ShaderType = FStorageFragmentShader::StaticType();
		const auto ParameterMetadata = ShaderType.GetParameterMetadata();
		ASSERT_EQ(ParameterMetadata.size(), 2u);
		EXPECT_STREQ(ParameterMetadata[0].Name, "DataBuffer");
		EXPECT_EQ(ParameterMetadata[0].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(ParameterMetadata[0].Size, sizeof(FRHIStorageBufferRange));
		EXPECT_STREQ(ParameterMetadata[1].Name, "OutputImage");
		EXPECT_EQ(ParameterMetadata[1].Type, ERHIBindingType::StorageImage);
		EXPECT_EQ(ParameterMetadata[1].Size, sizeof(FRHITexture*));
	}

	TEST(FShaderFoundationTests, ShaderDeclarationMacrosUseDefaultAndExplicitTypeNames)
	{
		FShaderType& DefaultNamedShaderType = FStaticVertexShader::StaticType();
		FShaderType& ExplicitNamedShaderType = FNamedShaderAlias::StaticType();

		EXPECT_EQ(DefaultNamedShaderType.GetName(), "FStaticVertexShader");
		EXPECT_EQ(ExplicitNamedShaderType.GetName(), "ExplicitNamedShader");
	}

	TEST(FShaderFoundationTests, DerivedShaderDeclarationUsesExplicitSuperAndDoesNotImplicitlyInheritParameters)
	{
		FShaderType& BaseShaderType = FIntermediateShader::StaticType();
		FShaderType& DerivedNoParametersType = FDerivedShaderNoParameters::StaticType();
		FShaderType& DerivedWithParametersType = FDerivedShaderWithParameters::StaticType();

		EXPECT_EQ(BaseShaderType.GetName(), "FIntermediateShader");
		EXPECT_EQ(DerivedNoParametersType.GetName(), "FDerivedShaderNoParameters");
		EXPECT_EQ(DerivedWithParametersType.GetName(), "FDerivedShaderWithParameters");
		EXPECT_TRUE(DerivedNoParametersType.GetParameterMetadata().empty());
		ASSERT_NE(DerivedWithParametersType.GetParametersMetadata(), nullptr);
		ASSERT_EQ(DerivedWithParametersType.GetParameterMetadata().size(), 1u);
		EXPECT_STREQ(DerivedWithParametersType.GetParameterMetadata()[0].Name, "FontTexture");

		FShaderReflectionData BaseReflection;
		FShaderReflectionData DerivedWithParametersReflection;
		DerivedWithParametersReflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 1,
			.BindingIndex = 7,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "baseMain", "FIntermediateShader", 31, BaseReflection),
			MakeCompiledShader(EShaderFrequency::Vertex, "derivedNoParamsMain", "FDerivedShaderNoParameters", 32, BaseReflection),
			MakeCompiledShader(EShaderFrequency::Fragment, "derivedWithParamsMain", "FDerivedShaderWithParameters", 33, DerivedWithParametersReflection)
		};

		std::array<const FShaderType*, 3> ShaderTypes = {&BaseShaderType, &DerivedNoParametersType, &DerivedWithParametersType};
		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;

		const auto* BaseShader = static_cast<const FIntermediateShader*>(ShaderMap.GetShader(&BaseShaderType));
		const auto* DerivedNoParametersShader = static_cast<const FDerivedShaderNoParameters*>(ShaderMap.GetShader(&DerivedNoParametersType));
		const auto* DerivedWithParametersShader = static_cast<const FDerivedShaderWithParameters*>(ShaderMap.GetShader(&DerivedWithParametersType));
		ASSERT_NE(BaseShader, nullptr);
		ASSERT_NE(DerivedNoParametersShader, nullptr);
		ASSERT_NE(DerivedWithParametersShader, nullptr);
		EXPECT_TRUE(BaseShader->GetParameterBindings().empty());
		EXPECT_TRUE(DerivedNoParametersShader->GetParameterBindings().empty());
		ASSERT_EQ(DerivedWithParametersShader->GetParameterBindings().size(), 1u);
		EXPECT_EQ(DerivedWithParametersShader->GetParameterBindings()[0].BindingIndex, 7u);
		EXPECT_EQ(DerivedWithParametersShader->GetParameterBindings()[0].Offset, offsetof(FDerivedShaderWithParameters::FParameters, FontTexture));
	}

	TEST(FShaderFoundationTests, ExplicitIncludedParametersAreFlattenedBeforeOwnParameters)
	{
		FShaderType& IncludedParametersType = FIncludedParametersShader::StaticType();
		FShaderType& IncludeOnlyType = FExplicitIncludeOnlyShader::StaticType();
		FShaderType& IncludeWithOwnType = FExplicitIncludeWithOwnParametersShader::StaticType();

		ASSERT_NE(IncludedParametersType.GetParametersMetadata(), nullptr);
		ASSERT_NE(IncludeOnlyType.GetParametersMetadata(), nullptr);
		ASSERT_NE(IncludeWithOwnType.GetParametersMetadata(), nullptr);
		ASSERT_EQ(IncludeOnlyType.GetParameterMetadata().size(), 1u);
		EXPECT_STREQ(IncludeOnlyType.GetParameterMetadata()[0].Name, "FontTexture");
		ASSERT_EQ(IncludeWithOwnType.GetParameterMetadata().size(), 2u);
		EXPECT_STREQ(IncludeWithOwnType.GetParameterMetadata()[0].Name, "FontTexture");
		EXPECT_STREQ(IncludeWithOwnType.GetParameterMetadata()[1].Name, "FontSampler");

		FShaderReflectionData Reflection;
		Reflection.ResourceBindings.push_back({
			.Name = "FontTexture",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 4,
			.Type = ERHIBindingType::Texture,
			.ArraySize = 1
		});
		Reflection.ResourceBindings.push_back({
			.Name = "FontSampler",
			.StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 0,
			.BindingIndex = 5,
			.Type = ERHIBindingType::Sampler,
			.ArraySize = 1
		});

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Fragment, "includeBaseMain", "FIncludedParametersShader", 41, Reflection),
			MakeCompiledShader(EShaderFrequency::Fragment, "includeOnlyMain", "FExplicitIncludeOnlyShader", 42, Reflection),
			MakeCompiledShader(EShaderFrequency::Fragment, "includeOwnMain", "FExplicitIncludeWithOwnParametersShader", 43, Reflection)
		};

		std::array<const FShaderType*, 3> ShaderTypes = {&IncludedParametersType, &IncludeOnlyType, &IncludeWithOwnType};
		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		ASSERT_TRUE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage)) << ErrorMessage;

		const auto* IncludeOnlyShader = static_cast<const FExplicitIncludeOnlyShader*>(ShaderMap.GetShader(&IncludeOnlyType));
		const auto* IncludeWithOwnShader = static_cast<const FExplicitIncludeWithOwnParametersShader*>(ShaderMap.GetShader(&IncludeWithOwnType));
		ASSERT_NE(IncludeOnlyShader, nullptr);
		ASSERT_NE(IncludeWithOwnShader, nullptr);
		ASSERT_EQ(IncludeOnlyShader->GetParameterBindings().size(), 1u);
		EXPECT_EQ(IncludeOnlyShader->GetParameterBindings()[0].BindingIndex, 4u);
		ASSERT_EQ(IncludeWithOwnShader->GetParameterBindings().size(), 2u);
		EXPECT_EQ(IncludeWithOwnShader->GetParameterBindings()[0].BindingIndex, 4u);
		EXPECT_EQ(IncludeWithOwnShader->GetParameterBindings()[1].BindingIndex, 5u);
	}

	TEST(FShaderFoundationTests, ShaderMapInitializeRejectsMismatchedSourceEntryPoint)
	{
		FShaderType VertexShaderType("UnitVertexShader", "/Unit/TestShader", EShaderFrequency::Vertex, "vertexMain");
		std::array<const FShaderType*, 1> ShaderTypes = {&VertexShaderType};

		FShaderCompilerOutput Output;
		Output.bSucceeded = true;
		Output.CompiledShaders = {
			MakeCompiledShader(EShaderFrequency::Vertex, "unexpectedMain", "UnitVertexShader", 12)
		};

		FShaderMapBase ShaderMap;
		std::string ErrorMessage;
		EXPECT_FALSE(ShaderMap.Initialize(ShaderTypes, Output, ErrorMessage));
		EXPECT_FALSE(ErrorMessage.empty());
	}

	TEST(FShaderFoundationTests, UnmountedShaderCacheFallsBackUnderEngineDirectory)
	{
		const std::string MetaPath = FShaderPaths::MetaPath("/Unit/TestShader", "00112233445566778899aabbccddeeff");
		EXPECT_TRUE(MetaPath.starts_with(FPaths::EngineDir() + "ShaderCache/SPIR-V/"));
	}

} // namespace Durin
