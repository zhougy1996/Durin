#include "MetalShaderFormat.h"
#include <gtest/gtest.h>
#include "NativeRHIBackendTestSupport.h"

#include "Shader/Shader.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include <barrier>
#include <exception>


namespace Durin
{
	namespace
	{
		auto FindBinding(
			const FShaderCode& Shader,
			std::string_view Name
		) -> const FShaderResourceBinding*
		{
			const auto It = std::ranges::find(
				Shader.Reflection.ResourceBindings,
				Name,
				&FShaderResourceBinding::Name
			);
			return It == Shader.Reflection.ResourceBindings.end() ? nullptr : &*It;
		}

		auto GetSpirvInterfaceLocations(
			const FShaderCode& Shader,
			uint32 StorageClass
		) -> std::set<uint32>
		{
			constexpr uint16 OpDecorate = 71;
			constexpr uint16 OpVariable = 59;
			constexpr uint32 DecorationLocation = 30;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

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
						Words[Offset + 3]
					);
				}
				else if (OpCode == OpVariable && WordCount >= 4
						 && Words[Offset + 3] == StorageClass)
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

		auto GetSpirvInputLocations(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint32 StorageClassInput = 1;
			return GetSpirvInterfaceLocations(Shader, StorageClassInput);
		}

		auto GetSpirvOutputLocations(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint32 StorageClassOutput = 3;
			return GetSpirvInterfaceLocations(Shader, StorageClassOutput);
		}

		auto GetSpirvImageFormats(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint16 OpTypeImage = 25;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

			std::set<uint32> Formats;
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
				if (OpCode == OpTypeImage && WordCount >= 9)
				{
					Formats.insert(Words[Offset + 8]);
				}
				Offset += WordCount;
			}
			return Formats;
		}

		auto HasSpirvInputBuiltIn(
			const FShaderCode& Shader,
			uint32 BuiltIn
		) -> bool
		{
			constexpr uint16 OpDecorate = 71;
			constexpr uint16 OpVariable = 59;
			constexpr uint32 DecorationBuiltIn = 11;
			constexpr uint32 StorageClassInput = 1;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

			std::set<uint32> BuiltInIds;
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
					return false;
				}
				if (OpCode == OpDecorate && WordCount >= 4
					&& Words[Offset + 2] == DecorationBuiltIn
					&& Words[Offset + 3] == BuiltIn)
				{
					BuiltInIds.insert(Words[Offset + 1]);
				}
				else if (OpCode == OpVariable && WordCount >= 4
						 && Words[Offset + 3] == StorageClassInput)
				{
					InputIds.insert(Words[Offset + 2]);
				}
				Offset += WordCount;
			}
			return std::ranges::any_of(
				BuiltInIds,
				[&InputIds](uint32 Id) { return InputIds.contains(Id); }
			);
		}

		auto ExpectBinding(
			const FShaderCode& Shader,
			std::string_view Name,
			uint32 BindingIndex,
			ERHIBindingType Type,
			EShaderStageFlags StageFlags, uint32 SetIndex = 0
		) -> void
		{
			const FShaderResourceBinding* Binding =
				FindBinding(Shader, Name);
			ASSERT_NE(Binding, nullptr) << Name;
			EXPECT_EQ(Binding->SetIndex, SetIndex) << Name;
			EXPECT_EQ(Binding->BindingIndex, BindingIndex) << Name;
			EXPECT_EQ(Binding->Type, Type) << Name;
			EXPECT_EQ(Binding->ArraySize, 1u) << Name;
			EXPECT_EQ(Binding->StageFlags, StageFlags) << Name;
		}
	} // namespace

	class FShaderReflectionTests : public ::testing::Test
	{
	protected:
		Testing::FScopedRHIBackendOverride Backend{"vulkan"};
	};

	TEST_F(FShaderReflectionTests, PooledSessionsDoNotCrossThreadLifetimes)
	{
		FSlangGlobalSessionPool Pool;
		slang::IGlobalSession* FirstSession = nullptr;
		slang::IGlobalSession* NextSession = nullptr;
		std::thread First([&] {
			auto Lease = Pool.Acquire();
			FirstSession = Lease.operator->();
		});
		First.join();
		std::thread Next([&] {
			auto Lease = Pool.Acquire();
			NextSession = Lease.operator->();
		});
		Next.join();
		// std::thread::id can be recycled after join; this is still a new thread.
		EXPECT_NE(FirstSession, NextSession);
	}

	TEST_F(FShaderReflectionTests, PooledSessionsSurviveResolverChurnAndConcurrentReflection)
	{
		constexpr std::string_view Source = R"(
[[vk::binding(0, 0)]] Texture2D<float4> Inputs[2];
[[vk::binding(1, 0)]] SamplerState InputSamplers[2];
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> Output;
[shader("compute")]
[numthreads(2, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(Inputs[id.x].SampleLevel(InputSamplers[id.x], float2(0.5, 0.5), 0).r * 255);
}
)";
		FSlangShaderCompiler Compiler;
		std::barrier Start(6);
		std::vector<std::future<std::string>> Workers;
		for (uint32 Worker = 0; Worker < 6; ++Worker)
			Workers.push_back(std::async(std::launch::async, [&] {
				FShaderCompileOptions Options;
				Options.Target = VulkanShaderTarget;
				Options.VirtualShaderPath = "/Unit/SessionLifetime";
				Options.EntryPoints = {"ComputeMain"};
				Options.Frequencies = {EShaderFrequency::Compute};
				Start.arrive_and_wait();
				for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
				{
					{
						FSlangShaderDependencyResolver Resolver;
						std::vector<std::string> Dependencies;
						const auto Resolved = Resolver.ResolveSource("SessionLifetime", "/Unit/SessionLifetime.slang", Source, Options, Dependencies);
						if (!Resolved) return std::string(Resolved.error());
					}
					const auto Result = Compiler.CompileSource("SessionLifetime", "/Unit/SessionLifetime.slang", Source, Options);
					if (!Result) return FormatShaderCompilerErrors(Result.Errors);
					if (Result.CompiledShaders.size() != 1 || Result.CompiledShaders.front().Reflection.ResourceBindings.size() != 3)
						return std::string("Reflection lost resource bindings after resolver destruction.");
				}
				return std::string{};
			}));
		for (auto& Worker : Workers) EXPECT_EQ(Worker.get(), "");
	}

	TEST_F(FShaderReflectionTests, CompilerSessionsSurviveSequentialWorkerLifetimes)
	{
		FSlangShaderCompiler Compiler;
		const auto Path = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "StorageResources.slang").string();
		FShaderCompileOptions Options;
		Options.Target = VulkanShaderTarget;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};
		for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
		{
			SCOPED_TRACE(Iteration);
			FShaderCompilerOutput Output;
			std::exception_ptr Failure;
			std::thread Worker([&] {
				try { Output = Compiler.Compile(Path, Options); }
				catch (...) { Failure = std::current_exception(); }
			});
			Worker.join();
			if (Failure) std::rethrow_exception(Failure);
			ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
			ASSERT_EQ(Output.CompiledShaders.size(), 1u);
			EXPECT_FALSE(Output.CompiledShaders.front().Reflection.ResourceBindings.empty());
		}
	}

	TEST_F(FShaderReflectionTests, ReflectsStorageBuffersAndImages)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "StorageResources.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
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

	TEST_F(FShaderReflectionTests, TexturePreviewCompilesWithMaterialNormalDecoder)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "TexturePreview.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/TexturePreview";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const auto& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 3u);
		ExpectBinding(Fragment, "PreviewTexture", 0, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "PreviewSampler", 1, ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "PreviewSettings", 2, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

#if defined(__APPLE__)
	TEST_F(FShaderReflectionTests, MetalTexturePreviewCarriesCheckedNativeBindings)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "TexturePreview.slang";
		FShaderCompileOptions Options;
		Options.Target = MetalShaderTarget;
		Options.VirtualShaderPath = "/Engine/TexturePreview";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		const auto Output = FSlangShaderCompiler().Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		for (const auto& Shader : Output.CompiledShaders)
		{
			EXPECT_EQ(Shader.Target, MetalShaderTarget);
			EXPECT_EQ(Shader.CodeFormat, EShaderCodeFormat::Msl20Source);
			EXPECT_EQ(Shader.BinaryEntryPoint, "main0");
			ASSERT_TRUE(!Shader.Code.IsEmpty());
			const std::string_view Source(reinterpret_cast<const char*>(Shader.Code.data()),
				Shader.Code.size());
			EXPECT_TRUE(Source.contains("#include <metal_stdlib>"));
			EXPECT_NE(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Identity, FXxHash128{});
		}
		const auto& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->Bindings.size(), 3u);
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->Bindings[0], (FMetalShaderBinding{0, 0, ERHIBindingType::Texture, 0, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->Bindings[1], (FMetalShaderBinding{0, 1, ERHIBindingType::Sampler, 0, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->Bindings[2], (FMetalShaderBinding{0, 2, ERHIBindingType::UniformBuffer, 0, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->PushConstantBufferSlot, UINT32_MAX);
	}

	TEST_F(FShaderReflectionTests, MetalComputeReservesArrayAndPushConstantSlots)
	{
		constexpr std::string_view Source = R"(
[[vk::binding(0, 0)]] Texture2D<float4> Inputs[2];
[[vk::binding(1, 0)]] SamplerState InputSamplers[2];
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> Output;
struct FPush { uint Offset; };
[[vk::push_constant]] ConstantBuffer<FPush> Push;
[shader("compute")]
[numthreads(2, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(Inputs[id.x].SampleLevel(InputSamplers[id.x], float2(0.5, 0.5), 0).r * 255) + Push.Offset;
}
)";
		FShaderCompileOptions Options;
		Options.Target = MetalShaderTarget;
		Options.VirtualShaderPath = "/Unit/MetalBindingArrays";
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		const auto Output = FSlangShaderCompiler().CompileSource(
			"MetalBindingArrays", "/Unit/MetalBindingArrays.slang", Source, Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);
		const auto& Shader = Output.CompiledShaders[0];
		ASSERT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings.size(), 3u);
		EXPECT_EQ(Shader.ComputeThreadGroupSize,
			(std::array<uint32, 3>{2, 1, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[0], (FMetalShaderBinding{0, 0, ERHIBindingType::Texture, 0, 2}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[1], (FMetalShaderBinding{0, 1, ERHIBindingType::Sampler, 0, 2}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[2], (FMetalShaderBinding{0, 2, ERHIBindingType::StorageBuffer, 0, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->PushConstantBufferSlot, 1u);
	}
#endif

	TEST_F(FShaderReflectionTests, VolumetricCloudPublishesMatchedSpatialBindingsAndRgba16Output)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloud.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloud";
		Options.EntryPoints = {"VertexMain", "CloudFragmentMain", "CloudComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 3u);
		const FShaderCode& Vertex = Output.CompiledShaders[0];
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		const FShaderCode& Compute = Output.CompiledShaders[2];
		EXPECT_TRUE(Vertex.Reflection.ResourceBindings.empty());
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ASSERT_EQ(Compute.Reflection.ResourceBindings.size(), 7u);

		for (const FShaderCode* Shader : {&Fragment, &Compute})
		{
			const EShaderStageFlags Stage = Shader == &Fragment ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
			ExpectBinding(*Shader, "BaseDensity", 0, ERHIBindingType::Texture, Stage);
			ExpectBinding(*Shader, "DetailDensity", 1, ERHIBindingType::Texture, Stage);
			ExpectBinding(*Shader, "WeatherTexture", 2, ERHIBindingType::Texture, Stage);
			ExpectBinding(*Shader, "SceneDepth", 3, ERHIBindingType::Texture, Stage);
			ExpectBinding(*Shader, "DensitySampler", 4, ERHIBindingType::Sampler, Stage);
			ExpectBinding(*Shader, "Params", 5, ERHIBindingType::UniformBuffer, Stage);
		}
		ExpectBinding(Compute, "CloudOutput", 6, ERHIBindingType::StorageImage, EShaderStageFlags::Compute);
		constexpr uint32 SpirvImageFormatRgba16f = 2;
		EXPECT_TRUE(GetSpirvImageFormats(Compute).contains(
			SpirvImageFormatRgba16f
		));
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudShadowPublishesMatchedR8VisibilityBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudShadow.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudShadow";
		Options.EntryPoints = {"VertexMain", "CloudVisibilityFragmentMain", "CloudVisibilityComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(
			ShaderPath.string(), Options
		);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 3u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		const FShaderCode& Compute = Output.CompiledShaders[2];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ASSERT_EQ(Compute.Reflection.ResourceBindings.size(), 7u);
		for (const FShaderCode* Shader : {&Fragment, &Compute})
		{
			const EShaderStageFlags Stage = Shader == &Fragment ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
			const std::array<const char*, 4> Textures{
				"BaseDensity", "DetailDensity", "WeatherTexture", "SceneDepth"
			};
			for (uint32 Index = 0; Index < Textures.size(); ++Index)
				ExpectBinding(*Shader, Textures[Index], Index, ERHIBindingType::Texture, Stage);
			ExpectBinding(*Shader, "DensitySampler", 4, ERHIBindingType::Sampler, Stage);
			ExpectBinding(*Shader, "Params", 5, ERHIBindingType::UniformBuffer, Stage);
		}
		ExpectBinding(Compute, "CloudVisibilityOutput", 6, ERHIBindingType::StorageImage, EShaderStageFlags::Compute);
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudCompositePublishesDepthAwareReconstructionBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudComposite.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudComposite";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 5u);
		ExpectBinding(Fragment, "SceneColorTexture", 0, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "CloudTexture", 1, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "SceneDepthTexture", 2, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "DebugTexture", 3, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "Params", 4, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudTemporalPublishesTypedHistoryBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudTemporal.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudTemporal";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 4u);
		ExpectBinding(Fragment, "CurrentCloud", 0, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "PreviousCloud", 1, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "HistorySampler", 2, ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "Params", 3, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, FragmentDepthOutputCompilesWithoutPipelineBindings)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "FragmentDepth.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);

		const FShaderCode& Shader = Output.CompiledShaders[0];
		EXPECT_EQ(Shader.Frequency, EShaderFrequency::Fragment);
		EXPECT_FALSE(Shader.Code.IsEmpty());
		EXPECT_TRUE(Shader.Reflection.ResourceBindings.empty());
		EXPECT_TRUE(Shader.Reflection.PushConstantRanges.empty());

		FPipelineLayoutDesc PipelineLayout;
		FShaderOperationResult ErrorMessage;
		ASSERT_TRUE((ErrorMessage = BuildPipelineLayoutFromShaders(Output.CompiledShaders, PipelineLayout))) << std::string(ErrorMessage.error());
		EXPECT_TRUE(PipelineLayout.BindingLayouts.empty());
		EXPECT_TRUE(PipelineLayout.PushConstantRanges.empty());
	}

	TEST_F(FShaderReflectionTests, ErrorTerminalPreservesVertexAbiWithoutFragmentDescriptors)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex,
			EShaderFrequency::Fragment
		};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& VertexShader =
			Output.CompiledShaders[0];
		const FShaderCode& FragmentShader =
			Output.CompiledShaders[1];

		EXPECT_EQ(VertexShader.SourceEntryPoint, "VertexMain");
		EXPECT_EQ(FragmentShader.SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(VertexShader.BinaryEntryPoint, "main");
		EXPECT_EQ(FragmentShader.BinaryEntryPoint, "main");
		// Constant error color does not consume the FrontFacing built-in.
		EXPECT_FALSE(HasSpirvInputBuiltIn(FragmentShader, 17u));
		EXPECT_EQ(
			GetSpirvInputLocations(VertexShader),
			(std::set<uint32>{0, 1, 2, 3, 4, 5, 6, 7})
		);

		ASSERT_EQ(
			VertexShader.Reflection.ResourceBindings.size(), 2u
		);
		ExpectBinding(VertexShader, "MaterialView", 0,
			ERHIBindingType::UniformBuffer, EShaderStageFlags::Vertex, 0);
		ExpectBinding(
			VertexShader,
			"MaterialPrimitive",
			0,
			ERHIBindingType::UniformBuffer,
			EShaderStageFlags::Vertex, 1
		);
		EXPECT_TRUE(FragmentShader.Reflection.ResourceBindings.empty());
		EXPECT_TRUE(FragmentShader.Reflection.PushConstantRanges.empty());
		FPipelineLayoutDesc PipelineLayout;
		FShaderOperationResult ErrorMessage;
		ASSERT_TRUE((ErrorMessage = BuildPipelineLayoutFromShaders(Output.CompiledShaders, PipelineLayout))) << std::string(ErrorMessage.error());
		ASSERT_EQ(PipelineLayout.BindingLayouts.size(), 2u);
		const auto& ViewLayout = PipelineLayout.BindingLayouts[0].BindingLayouts;
		ASSERT_EQ(ViewLayout.size(), 1u);
		EXPECT_EQ(ViewLayout[0].Slot, 0u);
		EXPECT_EQ(ViewLayout[0].Type, ERHIBindingType::UniformBuffer);
		EXPECT_EQ(ViewLayout[0].StageFlags, EShaderStageFlags::Vertex);
		const auto& SetLayout = PipelineLayout.BindingLayouts[1].BindingLayouts;
		ASSERT_EQ(SetLayout.size(), 1u);
		EXPECT_EQ(SetLayout[0].Slot, 0u);
		EXPECT_EQ(SetLayout[0].Type, ERHIBindingType::UniformBuffer);
		EXPECT_EQ(SetLayout[0].StageFlags, EShaderStageFlags::Vertex);
	}

	TEST_F(FShaderReflectionTests, GeometryPassPublishesFourTargetsForEveryVertexFactoryDomain)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		const std::array<std::pair<const char*, const char*>, 2> Domains{{
			{"Local", nullptr},
			{"Spline", "DURIN_SPLINE_MESH"},
		}};
		FSlangShaderCompiler Compiler;
		for (const auto& [Name, DomainMacro] : Domains)
		{
			FShaderCompileOptions Options;
			Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
			Options.EntryPoints = {"VertexMain", "GeometryFragmentMain"};
			Options.Frequencies = {
				EShaderFrequency::Vertex,
				EShaderFrequency::Fragment
			};
			Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "1");
			Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "1");
			Options.Macros.emplace_back(
				"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
				"1056964608"
			);
			if (DomainMacro != nullptr)
				Options.Macros.emplace_back(DomainMacro, "1");

			const FShaderCompilerOutput Output =
				Compiler.Compile(ShaderPath.string(), Options);
			ASSERT_TRUE(Output) << Name << ": " << FormatShaderCompilerErrors(Output.Errors);
			ASSERT_EQ(Output.CompiledShaders.size(), 2u) << Name;
			const FShaderCode& Fragment = Output.CompiledShaders[1];
			EXPECT_EQ(Fragment.SourceEntryPoint, "GeometryFragmentMain")
				<< Name;
			EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0, 1, 2, 3})) << Name;
			EXPECT_TRUE(HasSpirvInputBuiltIn(Fragment, 17u)) << Name;
			ASSERT_TRUE(Fragment.Reflection.ResourceBindings.empty())
				<< Name;
			EXPECT_EQ(FindBinding(Fragment, "Lighting"), nullptr) << Name;
			EXPECT_EQ(FindBinding(Fragment, "EnvironmentIrradiance"), nullptr)
				<< Name;
			EXPECT_EQ(FindBinding(Fragment, "DirectionalShadowTexture"), nullptr)
				<< Name;
		}
	}

	TEST_F(FShaderReflectionTests, GBufferDebugDecodesAllAttachmentsAndDepth)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "GBufferDebug.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/GBufferDebug";
		Options.EntryPoints = {"VertexMain", "DebugFragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0}));
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ExpectBinding(Fragment, "GBufferMaterial", 0, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GBufferNormals", 1, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GBufferSurface", 2, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GBufferEmissive", 3, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "SceneDepth", 4, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "Params", 5, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, ContactVisibilityFreezesGBufferInputAbi)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "ContactShadow.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/ContactShadow";
		Options.EntryPoints = {"VertexMain", "ContactVisibilityFragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0}));
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			const std::array<const char*, 5> Names{
				"GBufferMaterial", "GBufferNormals", "GBufferSurface",
				"GBufferEmissive", "SceneDepth"
			};
			ExpectBinding(Fragment, Names[Index], Index, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		}
		ExpectBinding(Fragment, "Params", 5, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, ContactVisibilityComputeFreezesR8StorageAbi)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "ContactShadow.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/ContactShadow";
		Options.EntryPoints = {"ContactVisibilityComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		const FShaderCompilerOutput Output =
			FSlangShaderCompiler().Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);
		const FShaderCode& Compute = Output.CompiledShaders[0];
		ASSERT_EQ(Compute.Reflection.ResourceBindings.size(), 7u);
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			const std::array<const char*, 5> Names{
				"GBufferMaterial", "GBufferNormals", "GBufferSurface",
				"GBufferEmissive", "SceneDepth"
			};
			ExpectBinding(Compute, Names[Index], Index, ERHIBindingType::Texture, EShaderStageFlags::Compute);
		}
		ExpectBinding(Compute, "Params", 5, ERHIBindingType::UniformBuffer, EShaderStageFlags::Compute);
		ExpectBinding(Compute, "ContactVisibilityOutput", 6, ERHIBindingType::StorageImage, EShaderStageFlags::Compute);
	}

	TEST_F(FShaderReflectionTests, DeferredDirectionalLightingFreezesPublishedInputAbi)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "DeferredDirectionalLighting.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/DeferredDirectionalLighting";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0}));
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 19u);
		for (uint32 Index = 0; Index <= 7; ++Index)
		{
			const std::array<const char*, 8> Names{
				"GBufferMaterial", "GBufferNormals", "GBufferSurface",
				"GBufferEmissive", "SceneDepth", "EnvironmentIrradiance",
				"EnvironmentPrefiltered", "EnvironmentBrdfLut"
			};
			ExpectBinding(Fragment, Names[Index], Index, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		}
		ExpectBinding(Fragment, "EnvironmentSampler", 8, ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "DirectionalShadowTexture", 9, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "DirectionalShadowSampler", 10, ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "View", 11, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "Lighting", 12, ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GroundTruthAmbientOcclusionRaw", 13, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GroundTruthAmbientOcclusionFiltered", 14, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "ContactVisibility", 15, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GroundTruthAmbientOcclusionResolved", 16, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "GroundTruthAmbientOcclusionSelector", 17, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectBinding(Fragment, "VolumetricCloudVisibility", 18, ERHIBindingType::Texture, EShaderStageFlags::Fragment);
	}
} // namespace Durin
