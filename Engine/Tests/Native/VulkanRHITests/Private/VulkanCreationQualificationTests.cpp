#include "VulkanPipelinePrecacheTestSupport.h"
#include "VulkanTestSupport.h"
#include <gtest/gtest.h>
#include <barrier>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"

#include "PCH.VulkanRHI.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "SlangShaderCompiler.h"
#include "VulkanCreationQualificationSupport.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "Shader/Shader.h"

namespace Durin::VulkanRHI
{
	// Owns a headless device and restores execution configuration on every exit.
	class FVulkanCreationQualificationTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			ASSERT_TRUE(PrepareCreationQualificationValidationLayer());
			ASSERT_TRUE(ConfigureCreationQualificationAffinity());
			ASSERT_TRUE(ConfigureCreationQualificationHighQos());
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			ASSERT_TRUE(InitializeTaskScheduler(1));
			if (const char* Mode = std::getenv("DURIN_RHI_EXECUTION")) PreviousMode = Mode;
			_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		}
		auto TearDown() -> void override
		{
			if (GDynamicRHI) RHIExit();
			ShutdownTaskScheduler();
			_putenv_s("DURIN_RHI_EXECUTION", PreviousMode ? PreviousMode->c_str() : "");
		}
		std::optional<std::string> PreviousMode;
	};

	TEST_F(FVulkanCreationQualificationTests, BindingUpdateAndSubmission)
	{
		constexpr uint32 DrawCount = 1024;
		constexpr uint32 Warmup = 30;
		constexpr uint32 Samples = 120;
		constexpr uint32 Runs = 6; // Three A/A pairs before comparing a candidate.
		FShaderCompileOptions Options;
		Options.EntryPoints = {"ComputeMain", "VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Compute, EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		const auto Compiled = FSlangShaderCompiler().Compile(
			(std::filesystem::path(DURIN_TEST_DATA_DIR) / "DeferredBufferVersions.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderCompilerErrors(Compiled.Errors);
		ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
		ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
		auto& Commands = FRHICommandListImmediate::Get();
		const auto OutputRoot = Testing::CreateTestFixtureDirectory("BindingTiming");
		std::ofstream Raw(OutputRoot / "samples.csv");
		std::ofstream Hardware(OutputRoot / "device.txt");
		WriteCreationHost(Hardware);
		Hardware << GetVulkanDeviceDescriptionForTiming();
		Hardware << "draws=" << DrawCount << " warmup=" << Warmup << " samples=" << Samples
			<< " runs=" << Runs << "\nexecution=threaded; cache=warm; pipeline_reset=same PSO\n";
		Raw << "compute,indirect,scenario,run,sample,producer_update_ns,producer_submit_ns,queue_replay_ns,combined_ns\n";
		std::array<FShaderRHIRef, 3> Shaders;
		for (uint32 Index = 0; Index < Shaders.size(); ++Index)
		{
			const auto& Source = Compiled.CompiledShaders[Index];
			auto Desc = FRHIShaderCreateDesc::Create(Source.DebugName.c_str(), Source.Frequency, Source.Code, Source.CodeHash);
			Desc.SetEntryPoint(Source.BinaryEntryPoint.c_str());
			Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
			ASSERT_TRUE(Shaders[Index]);
		}
		std::array<FBufferRHIRef, 2> Uniforms;
		for (uint32 Index = 0; Index < Uniforms.size(); ++Index)
		{
			const std::array<uint32, 4> Value{10 + Index * 10, 0, 0, 0};
			auto Desc = FRHIBufferCreateDesc::Create("BindingTimingUniform", 16, 0,
				EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Static);
			Desc.InitialData = {Value.data(), sizeof(Value)};
			Uniforms[Index] = GDynamicRHI->RHICreateBuffer(Commands, Desc);
			ASSERT_TRUE(Uniforms[Index]);
		}
		const std::array<uint32, 4> Values{0, 30, 50, 0};
		auto StorageDesc = FRHIBufferCreateDesc::Create("BindingTimingStorage", 16, 16,
			EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource);
		StorageDesc.InitialData = {Values.data(), sizeof(Values)};
		auto Storage = GDynamicRHI->RHICreateBuffer(Commands, StorageDesc);
		ASSERT_TRUE(Storage);
		auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
		ASSERT_TRUE(Declaration);
		FRHIRenderTargetLayout TargetLayout;
		TargetLayout.NumColorRenderTargets = 1;
		auto& Color = TargetLayout.ColorAttachments[0].RenderTarget;
		Color.Format = EPixelFormat::RGBA8_UNORM;
		Color.LoadAction = ERHIRenderTargetLoadAction::Clear;
		Color.StoreAction = ERHIRenderTargetStoreAction::Store;
		Color.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		Color.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FComputePipelineStateInitializer ComputeInit;
		ComputeInit.ComputeShader = Shaders[0];
		ASSERT_TRUE(BuildPipelineLayoutFromReflection(std::array{Compiled.CompiledShaders[0].Reflection}, ComputeInit.PipelineLayout));
		FGraphicsPipelineStateInitializer GraphicsInit;
		GraphicsInit.BoundShaders = {Shaders[1], Shaders[2]};
		GraphicsInit.VertexDeclaration = Declaration;
		GraphicsInit.RenderTargetLayout = TargetLayout;
		ASSERT_TRUE(BuildPipelineLayoutFromReflection(std::array{Compiled.CompiledShaders[1].Reflection,
			Compiled.CompiledShaders[2].Reflection}, GraphicsInit.PipelineLayout));
		const auto PreparationStart = VulkanCreationTimestamp();
		auto Compute = GDynamicRHI->RHICreateComputePipelineState("BindingTimingCompute", ComputeInit);
		auto Graphics = GDynamicRHI->RHICreateGraphicsPipelineState("BindingTimingGraphics", GraphicsInit);
		Hardware << "pipeline_creation_ns=" << VulkanCreationTimestamp() - PreparationStart
			<< " cache_metadata_bytes=" << GetVulkanDeviceForTest(FVulkanDynamicRHI::Get())->GetCacheMetadataBytes() << '\n';
		ASSERT_TRUE(Compute && Graphics);
		for (const bool bCompute : {false, true})
		{
			const auto Access = bCompute ? ERHIAccess::ComputeShaderRead : ERHIAccess::GraphicsShaderRead;
			if (bCompute)
				Commands.TransitionBuffers(std::array{
					FRHIBufferTransition{Uniforms[0], 0, 16, ERHIAccess::GraphicsUniformRead, ERHIAccess::ComputeUniformRead},
					FRHIBufferTransition{Uniforms[1], 0, 16, ERHIAccess::GraphicsUniformRead, ERHIAccess::ComputeUniformRead},
					FRHIBufferTransition{Storage, 0, 16, ERHIAccess::GraphicsShaderRead, Access}});
			const std::array<uint32, 4> Arguments = bCompute
				? std::array<uint32, 4>{1, 1, 1, 0} : std::array<uint32, 4>{3, 1, 0, 0};
			auto ArgumentDesc = FRHIBufferCreateDesc::Create("BindingTimingIndirect", sizeof(Arguments), 0,
				EBufferUsageFlags::DrawIndirect | EBufferUsageFlags::Static);
			ArgumentDesc.InitialData = {Arguments.data(), sizeof(Arguments)};
			auto Indirect = GDynamicRHI->RHICreateBuffer(Commands, ArgumentDesc);
			ASSERT_TRUE(Indirect);
			for (const bool bIndirect : {false, true})
			for (const std::string_view Scenario : {"unchanged", "partial_change", "pipeline_reset"})
			{
				SCOPED_TRACE(std::format("compute={} indirect={} scenario={}", bCompute, bIndirect, Scenario));
				auto Output = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("BindingTimingPixels", DrawCount, 1, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback
							| (bCompute ? ETextureCreateFlags::Storage : ETextureCreateFlags::RenderTargetable)));
				ASSERT_TRUE(Output);
				Commands.SwitchPipeline(bCompute ? ERHIPipeline::Compute : ERHIPipeline::Graphics);
				if (bCompute) Commands.TransitionTextures(std::array{FRHITextureTransition{Output,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite}});
				const auto Bind = [&] {
					if (bCompute) Commands.SetComputePipelineState(*Compute);
					else Commands.SetGraphicsPipelineState(*Graphics);
					const std::array Parameters{
						FRHIShaderParameterResource{.Resource = Uniforms[0], .BindingIndex = 0, .Type = ERHIBindingType::UniformBuffer, .Size = 16},
						FRHIShaderParameterResource{.Resource = Storage, .BindingIndex = 1, .Type = ERHIBindingType::StorageBuffer, .Size = 16}};
					Commands.SetShaderParameters(Shaders[bCompute ? 0 : 2], Parameters);
					if (bCompute) Commands.SetShaderParameters(Shaders[0], std::array{FRHIShaderParameterResource{
						.Resource = Output, .BindingIndex = 2, .Type = ERHIBindingType::StorageImage}});
				};
				std::array<std::array<std::vector<uint64>, 4>, Runs> Timings;
				for (auto& Run : Timings) for (auto& Metric : Run) Metric.reserve(Samples);
				for (uint32 Run = 0; Run < Runs; ++Run)
				for (uint32 Sample = 0; Sample < Warmup + Samples; ++Sample)
				{
					const auto Started = VulkanCreationTimestamp();
					GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
					if (!bCompute)
					{
						FRHIRenderPassInfo Pass;
						Pass.RenderTargetLayout = TargetLayout;
						Pass.ColorRenderTargets[0] = Output;
						Commands.BeginRenderPass(Pass, "BindingTiming");
						Commands.SetViewport(0, 0, 0, DrawCount, 1, 1);
					}
					Bind();
					uint64 UpdateTime = 0, SubmitTime = 0;
					for (uint32 Pixel = 0; Pixel < DrawCount; ++Pixel)
					{
						const auto UpdateStart = VulkanCreationTimestamp();
						if (Scenario == "pipeline_reset") Bind();
						const uint32 UniformIndex = Scenario == "partial_change" ? Pixel % 2 : 0;
						Commands.SetShaderParameters(Shaders[bCompute ? 0 : 2], std::array{FRHIShaderParameterResource{
							.Resource = Uniforms[UniformIndex], .BindingIndex = 0, .Type = ERHIBindingType::UniformBuffer, .Size = 16}});
						const auto Updated = VulkanCreationTimestamp();
						if (bCompute)
						{
							Commands.PushConstants(EShaderStageFlags::Compute, 0, sizeof(Pixel), &Pixel);
							if (bIndirect) Commands.DispatchIndirect(Indirect, 0);
							else Commands.Dispatch(1, 1, 1);
						}
						else
						{
							Commands.SetScissor(static_cast<float>(Pixel), 0, 1, 1);
							if (bIndirect) Commands.DrawIndirect(Indirect, 0);
							else Commands.Draw({.VertexCount = 3});
						}
						const auto Encoded = VulkanCreationTimestamp();
						UpdateTime += Updated - UpdateStart;
						SubmitTime += Encoded - Updated;
					}
					if (!bCompute) Commands.EndRenderPass();
					GDynamicRHI->RHIEndFrame_RenderThread(Commands);
					const auto ReplayStart = VulkanCreationTimestamp();
					Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
					const auto Finished = VulkanCreationTimestamp();
					// Drain outside the CPU interval; each sample starts without GPU backlog.
					GDynamicRHI->RHIBlockUntilGPUIdle();
					if (Sample >= Warmup)
					{
						const std::array Values{UpdateTime, SubmitTime, Finished - ReplayStart, Finished - Started};
						for (uint32 Metric = 0; Metric < Values.size(); ++Metric) Timings[Run][Metric].push_back(Values[Metric]);
						Raw << bCompute << ',' << bIndirect << ',' << Scenario << ',' << Run << ','
							<< Sample - Warmup << ',' << UpdateTime << ',' << SubmitTime << ',' << Finished - ReplayStart << ',' << Finished - Started << '\n';
					}
				}
				constexpr std::array MetricNames{"producer_update", "producer_submit", "queue_replay", "combined"};
				for (uint32 Run = 0; Run < Runs; ++Run)
				for (uint32 Metric = 0; Metric < MetricNames.size(); ++Metric)
				{
					auto& Values = Timings[Run][Metric];
					std::ranges::sort(Values);
					const auto Median = (Values[Samples / 2 - 1] + Values[Samples / 2]) / 2;
					const auto P95 = Values[(Samples * 95 + 99) / 100 - 1];
					const auto Prefix = std::format("binding_c{}_i{}_{}_run{}_{}", bCompute, bIndirect, Scenario, Run, MetricNames[Metric]);
					RecordProperty(Prefix + "_median_ns", std::to_string(Median));
					RecordProperty(Prefix + "_p95_ns", std::to_string(P95));
					std::cout << "BindingTiming " << Prefix << " median_ns=" << Median << " p95_ns=" << P95 << '\n';
				}
				FByteBuffer Pixels;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, Pixels));
				ASSERT_EQ(Pixels.size(), DrawCount * 4);
				for (uint32 Pixel = 0; Pixel < DrawCount; ++Pixel)
				{
					const uint32 Red = Scenario == "partial_change" ? 10 + (Pixel % 2) * 10 : 10;
					EXPECT_NEAR(std::to_integer<uint8>(Pixels[Pixel * 4]), Red, 1);
					EXPECT_NEAR(std::to_integer<uint8>(Pixels[Pixel * 4 + 1]), 30, 1);
					EXPECT_NEAR(std::to_integer<uint8>(Pixels[Pixel * 4 + 2]), 50, 1);
					EXPECT_EQ(Pixels[Pixel * 4 + 3], std::byte{255});
				}
			}
		}
		ASSERT_TRUE(Raw.good() && Hardware.good());
		RecordProperty("warmup_count", Warmup);
		RecordProperty("sample_count", Samples);
		RecordProperty("run_count", Runs);
		RecordProperty("draw_count", DrawCount);
		RecordProperty("authority", "observation_requires_external_quiet_lane_and_noise_receipt");
		std::cout << "Binding timing output: " << OutputRoot << std::endl;
	}

	TEST_F(FVulkanCreationQualificationTests, ColdHotAndConcurrentRequests)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderCompilerErrors(Compiled.Errors);
		ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
		const auto Output = Testing::CreateTestFixtureDirectory("CreationTiming");
		for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Compiled.CompiledShaders[Index].Code,
				Output / std::format("Shader{}.spv", Index)));
		std::ofstream Rounds(Output / "rounds.csv");
		Rounds << "round,scenario,compute,total_ns,synchronous_operations,submission_serials,graphics_native,compute_native,queue_entries_peak,queue_payload_peak"
			<< CreationMemoryColumns << '\n';
		std::ofstream Hardware(Output / "device.txt");
		WriteCreationHost(Hardware);
		for (bool bCompute : {false, true})
		for (std::string_view Scenario : {"cold", "hot", "duplicate16", "distinct16", "batch64"})
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			const auto InitStart = VulkanCreationTimestamp();
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			Hardware << "compute=" << bCompute << " scenario=" << Scenario << " round=" << Round << " init_ns=" << VulkanCreationTimestamp() - InitStart << '\n';
			const auto& Props = GetVulkanDeviceForTest(FVulkanDynamicRHI::Get())->GetGpuProperties();
			if (Round == 0 && Scenario == "cold")
			{
				Hardware << GetVulkanDeviceDescriptionForTiming();
				Hardware << "device=" << Props.deviceName.data() << " vendor=" << Props.vendorID
					<< " device_id=" << Props.deviceID << " driver=" << Props.driverVersion
					<< " api=" << Props.apiVersion << "\napplication_cache=cold per lifetime; driver_private_cache=uncontrolled\n";
				for (const auto& Shader : Compiled.CompiledShaders)
					Hardware << "shader=" << Shader.DebugName << " bytes=" << Shader.Code.size() << '\n';
			}
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency,
					Shader.Code, Shader.CodeHash);
				Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
				Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
				ASSERT_TRUE(Shaders[Index]);
			}
			FVertexDeclarationRHIRef Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
			ASSERT_TRUE(Declaration);
			FGraphicsPipelineStateInitializer Graphics;
			Graphics.BoundShaders.VertexShader = Shaders[0];
			Graphics.BoundShaders.FragmentShader = Shaders[1];
			Graphics.VertexDeclaration = Declaration;
			Graphics.RenderTargetLayout.NumColorRenderTargets = 1;
			Graphics.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
			FComputePipelineStateInitializer Compute;
			Compute.ComputeShader = Shaders[2];
			std::vector<FGraphicsPipelineStateRHIRef> GraphicsResults(128);
			std::vector<FComputePipelineStateRHIRef> ComputeResults(128);
			auto Measure = [&](auto&& Body) {
				FCreationMemorySampler MemorySampler;
				const auto Before = GCommandListExecutor.GetStats();
				const auto Serial = GCommandListExecutor.GetLastSubmittedSerial();
				const auto Cache = GDynamicRHI->RHIGetPipelineCacheStatistics();
				const auto Started = VulkanCreationTimestamp();
				Body();
				const auto Duration = VulkanCreationTimestamp() - Started;
				const auto Memory = MemorySampler.Stop();
#ifdef _WIN32
				EXPECT_TRUE(Memory.bAvailable);
#endif

				const auto After = GDynamicRHI->RHIGetPipelineCacheStatistics();
				Rounds << Round << ',' << Scenario << ',' << bCompute << ',' << Duration << ','
					<< GCommandListExecutor.GetStats().SynchronousOperationCount - Before.SynchronousOperationCount << ','
					<< GCommandListExecutor.GetLastSubmittedSerial() - Serial << ','
					<< After.GraphicsPipelines.NativeCreations - Cache.GraphicsPipelines.NativeCreations << ','
					<< After.ComputePipelines.NativeCreations - Cache.ComputePipelines.NativeCreations << ','
					<< GCommandListExecutor.GetStats().PeakQueueEntryCount << ','
					<< GCommandListExecutor.GetStats().PeakQueuePayloadBytes;
				WriteCreationMemorySample(Rounds, Memory);
				Rounds << '\n';
				const uint64 ExpectedCreations = Scenario == "hot" ? 0 : Scenario == "distinct16" ? 16
					: Scenario == "batch64" ? 64 : 1;
				EXPECT_EQ(After.GraphicsPipelines.NativeCreations - Cache.GraphicsPipelines.NativeCreations,
					bCompute ? 0 : ExpectedCreations);
				EXPECT_EQ(After.ComputePipelines.NativeCreations - Cache.ComputePipelines.NativeCreations,
					bCompute ? ExpectedCreations : 0);
			};
			auto Create = [&](size_t Index, uint32 Variant) {
				auto G = Graphics;
				auto C = Compute;
				// Unused descriptor slots vary full semantic identity without changing shader bytes.
				if (Variant)
				{
					G.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
						EShaderStageFlags::Vertex, Variant, ERHIBindingType::Texture);
					C.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
						EShaderStageFlags::Compute, Variant, ERHIBindingType::Texture);
				}
				const FName Name(std::format("MeasuredPipeline{}", Index));
				if (bCompute)
					ComputeResults[Index] = GDynamicRHI->RHICreateComputePipelineState(Name, C);
				else GraphicsResults[Index] = GDynamicRHI->RHICreateGraphicsPipelineState(Name, G);
			};
			if (Scenario == "hot") Create(127, 0);
			Measure([&] {
				if (Scenario == "cold") Create(0, 0);
				else if (Scenario == "hot")
					for (uint32 Index = 0; Index < 100; ++Index) Create(Index, 0);
				else if (Scenario == "batch64")
					for (uint32 Index = 0; Index < 64; ++Index) Create(Index, Index + 1);
				else
				{
					std::barrier Start(16);
					std::vector<std::thread> Producers;
					for (uint32 Index = 0; Index < 16; ++Index)
						Producers.emplace_back([&, Index] {
							Start.arrive_and_wait();
							Create(Index, Scenario == "distinct16" ? Index + 1 : 0);
						});
					for (auto& Producer : Producers) Producer.join();
				}
			});
			if (Scenario == "hot" || Scenario == "duplicate16")
				for (uint32 Index = 1; Index < (Scenario == "hot" ? 100u : 16u); ++Index)
				{
					EXPECT_EQ(GraphicsResults[0], GraphicsResults[Index]);
					EXPECT_EQ(ComputeResults[0], ComputeResults[Index]);
				}
			GraphicsResults.clear();
			ComputeResults.clear();
			Declaration = nullptr;
			Shaders = {};
			RHIExit();
		}

		ASSERT_TRUE(Rounds.good());
		ASSERT_TRUE(Hardware.good());
		std::cout << "Creation timing output: " << Output << std::endl;
	}
	TEST_F(FVulkanCreationQualificationTests, ResourceBatches)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderCompilerErrors(Compiled.Errors);
		const auto Output = Testing::CreateTestFixtureDirectory("ResourceCreationTiming");
		FByteBuffer InitialData(1024);
		for (size_t Index = 0; Index < InitialData.size(); ++Index)
			InitialData[Index] = std::byte(Index % 256);
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(InitialData, Output / "initial-data.bin"));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Compiled.CompiledShaders[0].Code, Output / "Shader0.spv"));
		std::ofstream Rounds(Output / "rounds.csv");
		Rounds << "round,creation_ns,upload_record_ns,upload_wait_ns,synchronous_operations,queue_entries_peak,queue_payload_peak"
			<< CreationMemoryColumns << '\n';
		std::ofstream Host(Output / "device.txt");
		WriteCreationHost(Host);
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			if (Round == 0) Host << GetVulkanDeviceDescriptionForTiming();
			auto& Cmd = FRHICommandListImmediate::Get();
			std::array<FShaderRHIRef, 64> Shaders;
			std::array<TRefCountPtr<FRHISampler>, 64> Samplers;
			std::array<FBufferRHIRef, 64> Buffers;
			std::array<FTextureRHIRef, 64> Textures;
			std::array<FBufferViewRHIRef, 64> BufferViews;
			std::array<FTextureViewRHIRef, 64> TextureViews;
			std::array<FVertexDeclarationRHIRef, 64> Declarations;
			const auto Before = GCommandListExecutor.GetStats();
			FCreationMemorySampler MemorySampler;
			const auto Start = VulkanCreationTimestamp();
			const auto& Shader = Compiled.CompiledShaders[0];
			auto ShaderDesc = FRHIShaderCreateDesc::Create("ResourceBatchShader", Shader.Frequency,
				Shader.Code, Shader.CodeHash);
			ShaderDesc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
			for (auto& Result : Shaders) Result = GDynamicRHI->RHICreateShader(ShaderDesc);
			for (auto& Result : Samplers) Result = GDynamicRHI->RHICreateSampler({});
			auto BufferDesc = FRHIBufferCreateDesc::Create("ResourceBatchBuffer", 1024, 4,
				EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::FormattedBuffer);
			BufferDesc.InitialData = {InitialData.data(), static_cast<uint32>(InitialData.size())};
			for (auto& Result : Buffers) Result = GDynamicRHI->RHICreateBuffer(Cmd, BufferDesc);
			auto TextureDesc = FRHITextureCreateDesc::Create2D("ResourceBatchTexture", 16, 16,
				EPixelFormat::RGBA8_UNORM);
			TextureDesc.Flags = ETextureCreateFlags::ShaderResource | ETextureCreateFlags::DestinationCopy;
			for (auto& Result : Textures) Result = GDynamicRHI->RHICreateTexture(Cmd, TextureDesc);
			for (size_t Index = 0; Index < 64; ++Index)
			{
				if (Buffers[Index]) BufferViews[Index] = GDynamicRHI->RHICreateBufferView(Buffers[Index],
					MakeDefaultBufferViewDesc(*Buffers[Index], ERHIBufferViewType::Formatted, EPixelFormat::R32_UINT));
				if (Textures[Index]) TextureViews[Index] = GDynamicRHI->RHICreateTextureView(Textures[Index],
					MakeDefaultTextureViewDesc(*Textures[Index], ERHITextureViewUsage::Sampled));
			}
			for (auto& Result : Declarations) Result = GDynamicRHI->RHICreateVertexDeclaration({});
			const auto Created = VulkanCreationTimestamp();

			for (size_t Index = 0; Index < 64; ++Index)
			{
				ASSERT_TRUE(Shaders[Index] && Samplers[Index] && Buffers[Index] && Textures[Index]
					&& BufferViews[Index] && TextureViews[Index] && Declarations[Index]);
			}
			const auto UploadStart = VulkanCreationTimestamp();
			for (const auto& Texture : Textures)
			{
				ASSERT_TRUE(Texture);
				Cmd.UpdateTexture2D(Texture, 0, 0, {0, 0, 0, 0, 16, 16}, 64, InitialData);
			}
			const auto UploadRecorded = VulkanCreationTimestamp();
			Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
			GDynamicRHI->RHIBlockUntilGPUIdle();
			const auto UploadReady = VulkanCreationTimestamp();
			const auto Memory = MemorySampler.Stop();
#ifdef _WIN32
			EXPECT_TRUE(Memory.bAvailable);
#endif
			const auto After = GCommandListExecutor.GetStats();
			Rounds << Round << ',' << Created - Start << ',' << UploadRecorded - UploadStart << ','
				<< UploadReady - UploadRecorded << ',' << After.SynchronousOperationCount - Before.SynchronousOperationCount
				<< ',' << After.PeakQueueEntryCount << ',' << After.PeakQueuePayloadBytes;
			WriteCreationMemorySample(Rounds, Memory);
			Rounds << '\n';
			BufferViews = {}; TextureViews = {}; Shaders = {}; Samplers = {};
			Buffers = {}; Textures = {}; Declarations = {};
			RHIExit();
		}

		ASSERT_TRUE(Rounds.good());
		std::cout << "Resource creation timing output: " << Output << std::endl;
	}


	TEST_F(FVulkanCreationQualificationTests, AsyncRequestsOnSerialCreator)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR) / "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderCompilerErrors(Compiled.Errors);
		const auto Output = Testing::CreateTestFixtureDirectory("AsyncCreationTiming");
		std::ofstream Observers(Output / "observers.csv"), Rounds(Output / "rounds.csv"), Hardware(Output / "device.txt");
		Observers << "round,scenario,compute,index,key,entry_ns,request_returned_ns,ready_observed_ns,consumer_wait_ns\n";
		Rounds << "round,scenario,compute,observers,admission_ns,consumer_wait_ns,total_ns,native_creations,synchronous_operations,replay_serials,replay_marker_ns,metadata_bytes" << CreationMemoryColumns << '\n';
		WriteCreationHost(Hardware);
		Hardware << "core_workers=1\nnative_creators=1\napplication_cache=cold per lifetime\ndriver_private_cache=uncontrolled\n";
		for (bool ComputeKind : {false, true})
		for (std::string_view Scenario : {"cold", "hot", "duplicate16", "distinct16", "batch64"})
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			if (Round == 0 && Scenario == "cold") Hardware << GetVulkanDeviceDescriptionForTiming();
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, Shader.Code, Shader.CodeHash);
				Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
				Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
				ASSERT_TRUE(Shaders[Index]);
			}
			auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
			FGraphicsPipelineStateInitializer Graphics;
			Graphics.BoundShaders = {Shaders[0], Shaders[1]};
			Graphics.VertexDeclaration = Declaration;
			Graphics.RenderTargetLayout.NumColorRenderTargets = 1;
			Graphics.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
			FComputePipelineStateInitializer Compute;
			Compute.ComputeShader = Shaders[2];
			const uint32 Count = Scenario == "cold" ? 1 : Scenario == "hot" ? 100 : Scenario == "batch64" ? 64 : 16;
			struct FObservation { FPipelinePrecacheTestObservation Request; uint64 Key = 0, Entry = 0, Returned = 0, Ready = 0, Wait = 0; };
			std::vector<FObservation> Records(Count);
			auto Request = [&](uint32 Index) {
				auto G = Graphics; auto C = Compute;
				if (Scenario == "distinct16" || Scenario == "batch64")
				{
					G.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(EShaderStageFlags::Vertex, Index + 1, ERHIBindingType::Texture);
					C.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(EShaderStageFlags::Compute, Index + 1, ERHIBindingType::Texture);
				}
				if (ComputeKind) { auto Key = BuildComputePipelineStateKey(C, GDynamicRHI->RHIGetCapabilities()); ASSERT_TRUE(Key) << ToString(Key.error()); Records[Index].Key = FComputePipelineStateKeyHasher{}(*Key); }
				else { auto Key = BuildGraphicsPipelineStateKey(G, GDynamicRHI->RHIGetCapabilities()); ASSERT_TRUE(Key) << ToString(Key.error()); Records[Index].Key = FGraphicsPipelineStateKeyHasher{}(*Key); }
				Records[Index].Entry = VulkanCreationTimestamp();
				Records[Index].Request = ComputeKind ? PrecachePipelineForTest(C, "AsyncMeasured") : PrecachePipelineForTest(G, "AsyncMeasured");
				Records[Index].Returned = VulkanCreationTimestamp();
				EXPECT_TRUE(Records[Index].Request.IsAccepted());
			};
			if (Scenario == "hot") { Request(0); ASSERT_TRUE(Records[0].Request.Wait()); }
			FCreationMemorySampler Memory;
			const auto Before = GCommandListExecutor.GetStats();
			const auto BeforeCache = GDynamicRHI->RHIGetPipelineCacheStatistics();
			const auto Begin = VulkanCreationTimestamp();
			if (Scenario == "duplicate16" || Scenario == "distinct16")
			{
				std::barrier Start(16);
				std::vector<std::thread> Producers;
				for (uint32 Index = 0; Index < Count; ++Index) Producers.emplace_back([&, Index] { Start.arrive_and_wait(); Request(Index); });
				for (auto& Producer : Producers) Producer.join();
			}
			else for (uint32 Index = 0; Index < Count; ++Index) Request(Index);
			const auto Admitted = VulkanCreationTimestamp();
			std::promise<uint64> Marker;
			auto MarkerFuture = Marker.get_future();
			FRHICommandListImmediate::Get().EnqueueLambda([&] { Marker.set_value(VulkanCreationTimestamp()); });
			GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
			const auto WaitBegin = VulkanCreationTimestamp();
			for (auto& Record : Records)
			{
				const auto Start = VulkanCreationTimestamp();
				EXPECT_TRUE(Record.Request.Wait());
				Record.Ready = VulkanCreationTimestamp(); Record.Wait = Record.Ready - Start;
			}
			const auto Ready = VulkanCreationTimestamp();
			EXPECT_EQ(MarkerFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			const auto MarkerTime = MarkerFuture.get();
			const auto MemorySample = Memory.Stop();
			const auto AfterCache = GDynamicRHI->RHIGetPipelineCacheStatistics();
			const uint64 NativeCount = ComputeKind ? AfterCache.ComputePipelines.NativeCreations - BeforeCache.ComputePipelines.NativeCreations : AfterCache.GraphicsPipelines.NativeCreations - BeforeCache.GraphicsPipelines.NativeCreations;
			EXPECT_EQ(NativeCount, Scenario == "hot" ? 0u : Scenario == "distinct16" || Scenario == "batch64" ? Count : 1u);
			const auto After = GCommandListExecutor.GetStats();
			EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
			EXPECT_EQ(After.LastSubmittedSerial - Before.LastSubmittedSerial, 1u);
			Rounds << Round << ',' << Scenario << ',' << ComputeKind << ',' << Count << ',' << Admitted - Begin << ',' << Ready - WaitBegin << ',' << Ready - Begin << ',' << NativeCount << ',' << After.SynchronousOperationCount - Before.SynchronousOperationCount << ',' << After.LastSubmittedSerial - Before.LastSubmittedSerial << ',' << MarkerTime - Admitted << ',' << GetVulkanDeviceForTest(FVulkanDynamicRHI::Get())->GetCacheMetadataBytes();
			WriteCreationMemorySample(Rounds, MemorySample); Rounds << '\n';
			for (uint32 Index = 0; Index < Count; ++Index) { const auto& R = Records[Index]; Observers << Round << ',' << Scenario << ',' << ComputeKind << ',' << Index << ',' << R.Key << ',' << R.Entry << ',' << R.Returned << ',' << R.Ready << ',' << R.Wait << '\n'; }
			Records.clear(); Shaders = {}; Declaration = nullptr;
			RHIExit();
		}
		ASSERT_TRUE(Observers.good() && Rounds.good() && Hardware.good());
		std::cout << "Async creation timing output: " << Output << std::endl;
	}


}
