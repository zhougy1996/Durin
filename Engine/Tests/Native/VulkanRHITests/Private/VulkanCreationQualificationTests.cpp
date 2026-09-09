#include <gtest/gtest.h>
#include <barrier>
#include <fstream>
#include <future>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"

#include "PCH.VulkanRHI.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "SlangShaderCompiler.h"
#include "VulkanCreationTiming.h"
#include "VulkanCreationQualificationSupport.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"

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
			if (const char* Mode = std::getenv("DURIN_RHI_EXECUTION")) PreviousMode = Mode;
			_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		}
		auto TearDown() -> void override
		{
			if (GDynamicRHI) RHIExit();
			SetVulkanPipelineCachePathForTest({});
			_putenv_s("DURIN_RHI_EXECUTION", PreviousMode ? PreviousMode->c_str() : "");
		}
		std::optional<std::string> PreviousMode;
	};

	TEST_F(FVulkanCreationQualificationTests, ColdHotAndConcurrentRequests)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << Compiled.ErrorMessage;
		ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
		const auto Output = Testing::CreateTestFixtureDirectory("CreationTiming");
		for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Compiled.CompiledShaders[Index].Code,
				Output / std::format("Shader{}.spv", Index)));
		// Later comparison runs restore these exact bytes, never a newly generated seed.
		const char* SeedDirectory = std::getenv("DURIN_VULKAN_CREATION_SEED_DIRECTORY");
		if (SeedDirectory)
			for (const char* Name : {"graphics-seed.bin", "compute-seed.bin"})
				std::filesystem::copy_file(std::filesystem::path(SeedDirectory) / Name, Output / Name);
		std::ofstream Csv(Output / "requests.csv");
		Csv << "round,scenario," << CreationRequestColumns << '\n';
		std::ofstream Rounds(Output / "rounds.csv");
		Rounds << "round,scenario,compute,synchronous_operations,submission_serials,graphics_native,compute_native,queue_entries_peak,queue_payload_peak"
			<< CreationMemoryColumns << '\n';
		std::ofstream Hardware(Output / "device.txt");
		WriteCreationHost(Hardware);
		const auto CachePath = Output / "PipelineCache-v1.bin";
		SetVulkanPipelineCachePathForTest(CachePath);
		for (bool bCompute : {false, true})
		for (std::string_view Scenario : {"cold", "hot", "duplicate16", "distinct16", "batch64"})
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			std::filesystem::remove(CachePath);
			const auto SeedPath = Output / (bCompute ? "compute-seed.bin" : "graphics-seed.bin");
			if (Scenario == "hot")
				std::filesystem::copy_file(SeedPath, CachePath);
			const auto InitStart = VulkanCreationTimestamp();
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			Hardware << "compute=" << bCompute << " scenario=" << Scenario << " round=" << Round << " init_ns=" << VulkanCreationTimestamp() - InitStart << '\n';
			const auto& Props = FVulkanDynamicRHI::Get().GetDeviceForTesting()->GetGpuProperties();
			if (Round == 0 && Scenario == "cold")
			{
				Hardware << GetVulkanDeviceDescriptionForTiming();
				Hardware << "device=" << Props.deviceName.data() << " vendor=" << Props.vendorID
					<< " device_id=" << Props.deviceID << " driver=" << Props.driverVersion
					<< " api=" << Props.apiVersion << "\napplication_cache=cold per lifetime; driver_private_cache=uncontrolled\n";
				for (const auto& Shader : Compiled.CompiledShaders)
					Hardware << "shader=" << Shader.DebugName << " bytes=" << Shader.Code->size() << '\n';
			}
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency,
					*Shader.Code, Shader.Hash);
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
				const auto Before = GCommandListExecutor.GetStats();
				const auto Serial = GCommandListExecutor.GetLastSubmittedSerial();
				const auto Cache = GDynamicRHI->RHIGetPipelineCacheStatistics();
				FCreationMemorySampler MemorySampler;
				BeginVulkanCreationTimingCapture(8192);
				Body();
				uint64 Dropped = 0;
				const auto Samples = EndVulkanCreationTimingCapture(Dropped);
				const auto Memory = MemorySampler.Stop();
#ifdef _WIN32
				EXPECT_TRUE(Memory.bAvailable);
#endif
				EXPECT_EQ(Dropped, 0u);
				const auto After = GDynamicRHI->RHIGetPipelineCacheStatistics();
				Rounds << Round << ',' << Scenario << ',' << bCompute << ','
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
				EXPECT_EQ(Samples.size(), Scenario == "hot" ? 100u : Scenario == "batch64" ? 64u
					: Scenario == "cold" ? 1u : 16u);
				for (const auto& S : Samples)
				{
					EXPECT_TRUE(S.bSucceeded);
					Csv << Round << ',' << Scenario << ',';
					WriteCreationRequest(Csv, S);
					Csv << '\n';
				}
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
			if (Scenario == "cold" && Round == 0 && !SeedDirectory)
				std::filesystem::copy_file(CachePath, SeedPath);
		}
		ASSERT_TRUE(Csv.good());
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
		ASSERT_TRUE(Compiled) << Compiled.ErrorMessage;
		const auto Output = Testing::CreateTestFixtureDirectory("ResourceCreationTiming");
		const auto CachePath = Output / "PipelineCache-v1.bin";
		SetVulkanPipelineCachePathForTest(CachePath);
		FByteBuffer InitialData(1024);
		for (size_t Index = 0; Index < InitialData.size(); ++Index)
			InitialData[Index] = std::byte(Index % 256);
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(InitialData, Output / "initial-data.bin"));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Compiled.CompiledShaders[0].Code, Output / "Shader0.spv"));
		std::ofstream Csv(Output / "requests.csv");
		Csv << "round," << CreationRequestColumns << '\n';
		std::ofstream Rounds(Output / "rounds.csv");
		Rounds << "round,creation_ns,upload_record_ns,upload_wait_ns,synchronous_operations,queue_entries_peak,queue_payload_peak"
			<< CreationMemoryColumns << '\n';
		std::ofstream Host(Output / "device.txt");
		WriteCreationHost(Host);
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			std::filesystem::remove(CachePath);
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
			BeginVulkanCreationTimingCapture(1024);
			const auto Start = VulkanCreationTimestamp();
			const auto& Shader = Compiled.CompiledShaders[0];
			auto ShaderDesc = FRHIShaderCreateDesc::Create("ResourceBatchShader", Shader.Frequency,
				*Shader.Code, Shader.Hash);
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
			uint64 Dropped = 0;
			const auto Samples = EndVulkanCreationTimingCapture(Dropped);
			ASSERT_EQ(Dropped, 0u);
			ASSERT_EQ(Samples.size(), 7u * 64);
			for (const auto& S : Samples) ASSERT_TRUE(S.bSucceeded) << uint32(S.Kind);
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
			for (const auto& Sample : Samples)
			{
				Csv << Round << ',';
				WriteCreationRequest(Csv, Sample);
				Csv << '\n';
			}
			BufferViews = {}; TextureViews = {}; Shaders = {}; Samplers = {};
			Buffers = {}; Textures = {}; Declarations = {};
			RHIExit();
		}
		ASSERT_TRUE(Csv.good());
		ASSERT_TRUE(Rounds.good());
		std::cout << "Resource creation timing output: " << Output << std::endl;
	}

	TEST_F(FVulkanCreationQualificationTests, TimingInstrumentationOverhead)
	{
		const auto Output = Testing::CreateTestFixtureDirectory("CreationInstrumentationOverhead");
		std::ofstream Csv(Output / "overhead.csv");
		Csv << "round,enabled,sample,elapsed_ns\n";
		for (uint32 Round = 0; Round < 30; ++Round)
		for (bool bEnabled : {false, true})
		{
			std::array<uint64, 1000> Durations;
			if (bEnabled) BeginVulkanCreationTimingCapture(Durations.size());
			for (auto& Duration : Durations)
			{
				const auto Start = VulkanCreationTimestamp();
				{
					FVulkanCreationTimingScope Request(false);
					if (auto* Timing = Request.Get()) Timing->Scheduled = VulkanCreationTimestamp();
					FVulkanCreationBodyTimingScope Body(Request.Get());
					{ FVulkanCreationDependencyTimingScope Dependency; }
					{ FVulkanNativeCreationTimingScope Native; }
				}
				Duration = VulkanCreationTimestamp() - Start;
			}
			if (bEnabled)
			{
				uint64 Dropped = 0;
				EXPECT_EQ(EndVulkanCreationTimingCapture(Dropped).size(), Durations.size());
				EXPECT_EQ(Dropped, 0u);
			}
			for (size_t Index = 0; Index < Durations.size(); ++Index)
				Csv << Round << ',' << bEnabled << ',' << Index << ',' << Durations[Index] << '\n';
		}
		ASSERT_TRUE(Csv.good());
		std::cout << "Creation instrumentation overhead: " << Output << std::endl;
	}

	TEST_F(FVulkanCreationQualificationTests, AsyncRequestsOnSerialCreator)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FCoreGuard { ~FCoreGuard() { if (GDynamicRHI) RHIExit(); ShutdownTaskScheduler(); } } CoreGuard;
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR) / "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << Compiled.ErrorMessage;
		const auto Output = Testing::CreateTestFixtureDirectory("AsyncCreationTiming");
		std::ofstream Native(Output / "native.csv"), Observers(Output / "observers.csv"), Rounds(Output / "rounds.csv"), Hardware(Output / "device.txt");
		Native << "round,scenario," << CreationRequestColumns << '\n';
		Observers << "round,scenario,compute,index,key,entry_ns,request_returned_ns,ready_observed_ns,consumer_wait_ns\n";
		Rounds << "round,scenario,compute,observers,admission_ns,consumer_wait_ns,total_ns,native_creations,synchronous_operations,replay_serials,replay_marker_ns,metadata_bytes" << CreationMemoryColumns << '\n';
		WriteCreationHost(Hardware);
		Hardware << "core_workers=1\nnative_creators=1\napplication_cache=cold per lifetime\ndriver_private_cache=uncontrolled\n";
		const auto CachePath = Output / "PipelineCache-v1.bin";
		SetVulkanPipelineCachePathForTest(CachePath);
		for (bool ComputeKind : {false, true})
		for (std::string_view Scenario : {"cold", "hot", "duplicate16", "distinct16", "batch64"})
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			std::filesystem::remove(CachePath);
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			if (Round == 0 && Scenario == "cold") Hardware << GetVulkanDeviceDescriptionForTiming();
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
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
			struct FObservation { FRHIPipelineCreationRequest Request; uint64 Key = 0, Entry = 0, Returned = 0, Ready = 0, Wait = 0; };
			std::vector<FObservation> Records(Count);
			auto Request = [&](uint32 Index) {
				auto G = Graphics; auto C = Compute;
				if (Scenario == "distinct16" || Scenario == "batch64")
				{
					G.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(EShaderStageFlags::Vertex, Index + 1, ERHIBindingType::Texture);
					C.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(EShaderStageFlags::Compute, Index + 1, ERHIBindingType::Texture);
				}
				std::string Error;
				if (ComputeKind) { FComputePipelineStateKey Key; EXPECT_TRUE(BuildComputePipelineStateKey(C, GDynamicRHI->RHIGetCapabilities(), Key, Error)); Records[Index].Key = FComputePipelineStateKeyHasher{}(Key); }
				else { FGraphicsPipelineStateKey Key; EXPECT_TRUE(BuildGraphicsPipelineStateKey(G, GDynamicRHI->RHIGetCapabilities(), Key, Error)); Records[Index].Key = FGraphicsPipelineStateKeyHasher{}(Key); }
				Records[Index].Entry = VulkanCreationTimestamp();
				Records[Index].Request = ComputeKind ? GDynamicRHI->RHIRequestComputePipelineState(C, "AsyncMeasured") : GDynamicRHI->RHIRequestGraphicsPipelineState(G, "AsyncMeasured");
				Records[Index].Returned = VulkanCreationTimestamp();
				EXPECT_TRUE(Records[Index].Request.IsAccepted());
			};
			if (Scenario == "hot") { Request(0); ASSERT_TRUE(Records[0].Request.Wait()); }
			const auto Before = GCommandListExecutor.GetStats();
			const auto BeforeCache = GDynamicRHI->RHIGetPipelineCacheStatistics();
			FCreationMemorySampler Memory;
			BeginVulkanCreationTimingCapture(256);
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
			uint64 Dropped = 0;
			const auto Samples = EndVulkanCreationTimingCapture(Dropped);
			const auto MemorySample = Memory.Stop();
			const auto AfterCache = GDynamicRHI->RHIGetPipelineCacheStatistics();
			const uint64 NativeCount = ComputeKind ? AfterCache.ComputePipelines.NativeCreations - BeforeCache.ComputePipelines.NativeCreations : AfterCache.GraphicsPipelines.NativeCreations - BeforeCache.GraphicsPipelines.NativeCreations;
			EXPECT_EQ(NativeCount, Scenario == "hot" ? 0u : Scenario == "distinct16" || Scenario == "batch64" ? Count : 1u);
			EXPECT_EQ(Dropped, 0u); EXPECT_EQ(Samples.size(), NativeCount);
			const auto After = GCommandListExecutor.GetStats();
			EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
			EXPECT_EQ(After.LastSubmittedSerial - Before.LastSubmittedSerial, 1u);
			Rounds << Round << ',' << Scenario << ',' << ComputeKind << ',' << Count << ',' << Admitted - Begin << ',' << Ready - WaitBegin << ',' << Ready - Begin << ',' << NativeCount << ',' << After.SynchronousOperationCount - Before.SynchronousOperationCount << ',' << After.LastSubmittedSerial - Before.LastSubmittedSerial << ',' << MarkerTime - Admitted << ',' << FVulkanDynamicRHI::Get().GetDeviceForTesting()->GetCacheMetadataBytes();
			WriteCreationMemorySample(Rounds, MemorySample); Rounds << '\n';
			for (const auto& Sample : Samples) { EXPECT_TRUE(Sample.bBackground && Sample.bSucceeded); Native << Round << ',' << Scenario << ','; WriteCreationRequest(Native, Sample); Native << '\n'; }
			for (uint32 Index = 0; Index < Count; ++Index) { const auto& R = Records[Index]; Observers << Round << ',' << Scenario << ',' << ComputeKind << ',' << Index << ',' << R.Key << ',' << R.Entry << ',' << R.Returned << ',' << R.Ready << ',' << R.Wait << '\n'; }
			Records.clear(); Shaders = {}; Declaration = nullptr;
			RHIExit();
		}
		ASSERT_TRUE(Native.good() && Observers.good() && Rounds.good() && Hardware.good());
		std::cout << "Async creation timing output: " << Output << std::endl;
	}


}
