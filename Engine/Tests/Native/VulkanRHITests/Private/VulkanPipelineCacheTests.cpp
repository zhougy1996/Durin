#include "VulkanIntegrationTestFixture.h"
#include "VulkanPipelinePrecacheTestSupport.h"
#include "VulkanTestSupport.h"
#include "VulkanRHITestEnvironment.h"
#include "Backend/RHIValidation.h"
#include "Backend/RHICreation.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHICommandList.h"
#include "SlangShaderCompiler.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanContext.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	class FVulkanPipelineCacheTests : public FVulkanIntegrationTest
	{
	};

	TEST_F(FVulkanPipelineCacheTests, FrameMaintenanceTrimsUnusedPipelinesAndPreservesPinnedEntries)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		ASSERT_EQ(Compiled.CompiledShaders.size(), 1u);
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		const auto& Input = Compiled.CompiledShaders.front();
		auto Desc = FRHIShaderCreateDesc::Create(Input.DebugName.c_str(), Input.Frequency, Input.Code, Input.CodeHash);
		Desc.SetEntryPoint(Input.BinaryEntryPoint.c_str());
		auto Shader = GDynamicRHI->RHICreateShader(Desc);
		ASSERT_TRUE(Shader);
		FComputePipelineStateInitializer Initializer;
		Initializer.ComputeShader = Shader;
		Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
			EShaderStageFlags::Compute, 0, ERHIBindingType::UniformBuffer);
		FComputePipelineStateRHIRef Pinned;
		for (uint32 Index = 0; Index < 70; ++Index)
		{
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = Index;
			auto Pipeline = GDynamicRHI->RHICreateComputePipelineState({}, Initializer);
			ASSERT_TRUE(Pipeline);
			if (Index == 0) Pinned = std::move(Pipeline);
		}
		EXPECT_EQ(GDynamicRHI->RHIGetPipelineCacheStatistics().ComputePipelines.Occupancy, 70u);
		auto* Device = GetVulkanDeviceForTest(FVulkanDynamicRHI::Get());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
			static_cast<IRHICommandContext*>(Device->GetImmediateContext())->RHIBeginFrame({});
		});
		const auto Stats = GDynamicRHI->RHIGetPipelineCacheStatistics().ComputePipelines;
		EXPECT_EQ(Stats.Occupancy, 65u);
		EXPECT_EQ(Stats.Evictions, 5u);
		Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts[0].Slot = 0;
		EXPECT_EQ(GDynamicRHI->RHICreateComputePipelineState({}, Initializer), Pinned);
	}

	TEST_F(FVulkanPipelineCacheTests, PublicAsyncPipelinesShareCreationAndRetainOwnedInputs)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FCoreGuard { ~FCoreGuard() { if (GDynamicRHI) RHIExit(); ShutdownTaskScheduler(); } } CoreGuard;
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		for (const char* Mode : {"threaded", "inline"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, Shader.Code, Shader.CodeHash);
				std::string TemporaryEntryPoint = Shader.BinaryEntryPoint;
				Desc.SetEntryPoint(TemporaryEntryPoint.c_str());
				Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
				TemporaryEntryPoint.assign(TemporaryEntryPoint.size(), 'x');
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
			FPipelinePrecacheTestObservation SurvivingRequest;
			for (bool IsCompute : {false, true})
			{
				SCOPED_TRACE(IsCompute);
				const auto Request = [&] { return IsCompute
					? PrecachePipelineForTest(Compute, "async")
					: PrecachePipelineForTest(Graphics, "async"); };
				auto Warm = Request();
				ASSERT_TRUE(Warm.Wait());
				const auto WarmResult = Warm.GetResult();
				auto& Layout = IsCompute ? Compute.PipelineLayout : Graphics.PipelineLayout;
				Layout.PushConstantRanges.push_back({
					IsCompute ? EShaderStageFlags::Compute : EShaderStageFlags::Vertex, 0, 4});
				const auto Before = GDynamicRHI->RHIGetPipelineCacheStatistics();
				std::vector<FPipelinePrecacheTestObservation> Requests;
				Requests.push_back(Request());
				std::vector<std::future<FPipelinePrecacheTestObservation>> Producers;
				for (uint32 Index = 1; Index < 16; ++Index)
					Producers.push_back(std::async(std::launch::async, Request));
				for (auto& Producer : Producers) Requests.push_back(Producer.get());
				Requests[0] = {}; // Releasing one precache owner does not cancel shared compilation.
				std::promise<void> Marker;
				auto MarkerFuture = Marker.get_future();
				auto& Immediate = FRHICommandListImmediate::Get();
				Immediate.EnqueueLambda([&] { Marker.set_value(); });
				Immediate.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
				EXPECT_EQ(MarkerFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				Layout.PushConstantRanges.clear();
				const auto Serial = GCommandListExecutor.GetLastSubmittedSerial();
				if (IsCompute) EXPECT_EQ(GDynamicRHI->RHICreateComputePipelineState("ready", Compute), WarmResult.Compute);
				else EXPECT_EQ(GDynamicRHI->RHICreateGraphicsPipelineState("ready", Graphics), WarmResult.Graphics);
				EXPECT_EQ(GCommandListExecutor.GetLastSubmittedSerial(), Serial);
				EXPECT_EQ(Requests[1].GetPipelineLayout()->PushConstantRanges.size(), 1u);
				for (size_t Index = 1; Index < Requests.size(); ++Index) EXPECT_TRUE(Requests[Index].Wait());
				const auto After = GDynamicRHI->RHIGetPipelineCacheStatistics();
				EXPECT_EQ((IsCompute ? After.ComputePipelines.NativeCreations : After.GraphicsPipelines.NativeCreations)
					- (IsCompute ? Before.ComputePipelines.NativeCreations : Before.GraphicsPipelines.NativeCreations), 1u);
				SurvivingRequest = Requests.back();
			}
			auto* Device = GetVulkanDeviceForTest(*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
			const auto Used = Device->GetCacheMetadataBytes();
			EXPECT_GT(Used, 0u);
			auto Reservation = Device->ReserveCacheMetadata(64ull * 1024 * 1024 - Used);
			EXPECT_THROW(Device->ReserveCacheMetadata(1), FRHIResourceCapacityError);
			Reservation.reset();
			EXPECT_EQ(Device->GetCacheMetadataBytes(), Used);
			Shaders = {};
			Declaration = nullptr;
			auto SurvivingLayout = SurvivingRequest.GetPipelineLayout();
			GDynamicRHI->RHIStopPipelineCreation();
			EXPECT_EQ(SurvivingRequest.GetState(), ERHIPipelineRequestState::Ready);
			RHIExit();
			EXPECT_EQ(SurvivingLayout->PushConstantRanges.size(), 1u);
			EXPECT_TRUE(SurvivingRequest.IsComplete());
			EXPECT_EQ(SurvivingRequest.GetState(), ERHIPipelineRequestState::Canceled);
			EXPECT_FALSE(SurvivingRequest.GetResult().Graphics);
			EXPECT_FALSE(SurvivingRequest.GetResult().Compute);
		}
	}

	TEST_F(FVulkanPipelineCacheTests,
		PipelineCapacityFailuresPreservePublishedEntries)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ASSERT_NE(GRHIThread, nullptr);
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
		auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
		ASSERT_TRUE(Declaration);
		auto* Device = GetVulkanDeviceForTest(*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
		FGraphicsPipelineStateInitializer Graphics;
		Graphics.BoundShaders.VertexShader = Shaders[0];
		Graphics.BoundShaders.FragmentShader = Shaders[1];
		Graphics.VertexDeclaration = Declaration;
		Graphics.RenderTargetLayout.NumColorRenderTargets = 1;
		Graphics.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
		FComputePipelineStateInitializer Compute;
		Compute.ComputeShader = Shaders[2];
		for (bool bCompute : {false, true})
		{
			SCOPED_TRACE(bCompute);
			{
				auto Access = Device->AccessPipelineCacheStatistics();
				(bCompute ? Access.Get().ComputePipelines : Access.Get().GraphicsPipelines).Capacity = 2;
			}
			const auto Create = [&](uint32 Variant) -> TRefCountPtr<FRHIResource> {
				auto G = Graphics;
				auto C = Compute;
				auto& Layout = bCompute ? C.PipelineLayout : G.PipelineLayout;
				Layout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
					bCompute ? EShaderStageFlags::Compute : EShaderStageFlags::Vertex,
					Variant, ERHIBindingType::UniformBuffer);
				TRefCountPtr<FRHIResource> Result;
				if (bCompute)
				{
					auto Key = BuildComputePipelineStateKey(C, GDynamicRHI->RHIGetCapabilities());
					if (!Key)
						throw std::runtime_error(std::string(ToString(Key.error())));
					Result = Device->GetPipelineManager().GetOrCreateComputePipelineState(C, std::move(*Key), "BackgroundCompute");
				}
				else
				{
					auto Key = BuildGraphicsPipelineStateKey(G, GDynamicRHI->RHIGetCapabilities());
					if (!Key)
						throw std::runtime_error(std::string(ToString(Key.error())));
					Result = Device->GetPipelineManager().GetOrCreateGraphicsPipelineState(G, std::move(*Key), "BackgroundGraphics");
				}
				return Result;
			};
			// PublicAsyncPipelinesShareCreationAndRetainOwnedInputs covers the
			// public creator's concurrency. Exercise cache admission serially here.
			auto Warm = Create(0);
			ASSERT_TRUE(Warm);
			auto Cold = Create(1);
			ASSERT_TRUE(Cold);
			EXPECT_EQ(Create(1), Cold);
			auto Hit = Create(0);
			auto& Commands = GCommandListExecutor.GetImmediateCommandList();
			EXPECT_EQ(Hit, Warm);
			EXPECT_FALSE(Create(2));
			const auto* ColdIdentity = Cold.GetReference();
			Cold = nullptr;
			{
				// Real budget pressure must restore the extracted cache-only victim.
				const auto Used = Device->GetCacheMetadataBytes();
				auto Pressure = Device->ReserveCacheMetadata(64ull * 1024 * 1024 - Used);
				EXPECT_FALSE(Create(2));
				auto Restored = Create(1);
				EXPECT_EQ(Restored.GetReference(), ColdIdentity);
				const auto PressureStats = Device->GetPipelineCacheStatistics();
				const auto& Cache = bCompute ? PressureStats.ComputePipelines : PressureStats.GraphicsPipelines;
				EXPECT_EQ(Cache.Occupancy, 2u);
				EXPECT_EQ(Cache.NativeCreations, 2u);
				EXPECT_EQ(Cache.Evictions, 0u);
			}
			auto Replacement = Create(2);
			EXPECT_TRUE(Replacement);
			EXPECT_EQ(Create(0), Warm);
			const auto Stats = Device->GetPipelineCacheStatistics();
			const auto& Cache = bCompute ? Stats.ComputePipelines : Stats.GraphicsPipelines;
			EXPECT_EQ(Cache.Occupancy, 2u);
			EXPECT_EQ(Cache.NativeCreations, 3u);
			EXPECT_EQ(Cache.FailedCandidates, 2u);
			EXPECT_EQ(Cache.Evictions, 1u);
			Warm = nullptr;
			Hit = nullptr;
			Replacement = nullptr;
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		}
	}
}
