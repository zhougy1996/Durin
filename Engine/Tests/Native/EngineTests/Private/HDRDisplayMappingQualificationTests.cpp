#include <gtest/gtest.h>
#include "VulkanEngineTestSupport.h"

#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SceneRendererProfiling.h"
#include <vulkan/vulkan.hpp>
#include "VulkanDynamicRHI.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <ranges>
#include <thread>
#include <vector>

namespace Durin
{
	namespace
	{
		constexpr uint32 TimingWidth = 1920;
		constexpr uint32 TimingHeight = 1080;
		constexpr uint32 WarmupFrames = 30;
		constexpr uint32 MeasuredFrames = 120;

		std::vector<FGPUTimingQueryRHIRef>* GDisplayTimingQueries = nullptr;

		auto CaptureDisplayTiming(const FGPUTimingQueryRHIRef& Query) -> void
		{
			if (GDisplayTimingQueries != nullptr)
				GDisplayTimingQueries->push_back(Query);
		}

		struct FProfileHDRDisplayMapping
		{
			static constexpr auto GetName() -> const char*
			{
				return "ProfileHDRDisplayMapping";
			}
		};

		struct FDisplayTimingSummary
		{
			uint64 MedianNanoseconds = 0;
			uint64 P95Nanoseconds = 0;
		};
	}

	TEST(FHDRDisplayMappingQualificationTests, ProfilesCopyAndFXAAAt1080p)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		auto* Vulkan = static_cast<VulkanRHI::IVulkanDynamicRHI*>(GDynamicRHI);
		vk::PhysicalDeviceProperties DeviceProperties{};
		Vulkan->RHIExecuteCommandBufferForBackendIntegration(
			[&Vulkan, &DeviceProperties](vk::CommandBuffer) {
				DeviceProperties = Vulkan->RHIGetVkPhysicalDevice().getProperties();
			});
		const std::string DeviceName = DeviceProperties.deviceName.data();
		const bool bNamedAdapter = DeviceName == "NVIDIA GeForce RTX 3090"
			&& vk::apiVersionMajor(DeviceProperties.apiVersion) == 1u
			&& vk::apiVersionMinor(DeviceProperties.apiVersion) == 4u
			&& vk::apiVersionPatch(DeviceProperties.apiVersion) == 325u;
		InitRenderingThread();

		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("HDRDisplayMappingQualification");
		RendererLifecycle.Start(Renderer);

		auto ProfileRoute = [&Renderer](const char* TargetName, bool bEnableFXAA) {
			std::vector<FGPUTimingQueryRHIRef> Queries;
			GDisplayTimingQueries = &Queries;
			SetPostProcessTimingQuerySink(CaptureDisplayTiming);
			EnqueueRenderCommand<FProfileHDRDisplayMapping>(
				[&Renderer, TargetName, bEnableFXAA](
					FRHICommandListImmediate& CommandList) {
					const auto Desc = FRHITextureCreateDesc::Create2D(
						TargetName,
						TimingWidth,
						TimingHeight,
						EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource);
					FTextureRHIRef Target =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);
					FSceneView View;
					View.ViewportWidth = TimingWidth;
					View.ViewportHeight = TimingHeight;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.PostProcess.bEnableFXAA = bEnableFXAA;
					for (uint32 Frame = 0;
						Frame < WarmupFrames + MeasuredFrames;
						++Frame)
					{
						++GRenderFrameCounterRenderThread;
						GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						EXPECT_EQ(
							Renderer.RenderView(
								CommandList, nullptr, View, Target, false, {}),
							ERenderViewResult::Success);
						GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				});
			FlushRenderingCommands();
			SetPostProcessTimingQuerySink(nullptr);
			GDisplayTimingQueries = nullptr;

			for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
			{
				const bool bReady = Queries.size() == WarmupFrames + MeasuredFrames
					&& std::ranges::all_of(Queries, [](const auto& Query) {
						return Query->GetResult().State
							== ERHIGPUTimingResultState::Ready;
					});
				if (bReady) break;
				EnqueueRenderCommand<FProfileHDRDisplayMapping>(
					[](FRHICommandListImmediate& CommandList) {
						++GRenderFrameCounterRenderThread;
						GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					});
				FlushRenderingCommands();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			EXPECT_EQ(Queries.size(), WarmupFrames + MeasuredFrames);
			std::vector<uint64> Durations;
			for (size_t Index = WarmupFrames; Index < Queries.size(); ++Index)
			{
				const FRHIGPUTimingResult Result = Queries[Index]->GetResult();
				EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
				if (Result.State == ERHIGPUTimingResultState::Ready)
					Durations.push_back(Result.DurationNanoseconds);
			}
			std::ranges::sort(Durations);
			EXPECT_EQ(Durations.size(), MeasuredFrames);
			return Durations.size() == MeasuredFrames
				? FDisplayTimingSummary{
					.MedianNanoseconds = Durations[MeasuredFrames / 2],
					.P95Nanoseconds = Durations[113]}
				: FDisplayTimingSummary{};
		};

		const FDisplayTimingSummary Copy =
			ProfileRoute("HDRDisplayCopyQualification", false);
		const FDisplayTimingSummary FXAA =
			ProfileRoute("HDRDisplayFXAAQualification", true);
		constexpr uint64 SceneTargetBytes =
			FPostProcessRenderer::CalculateSceneTargetBytes(
				TimingWidth, TimingHeight);
		constexpr uint64 OutputBytes =
			static_cast<uint64>(TimingWidth) * TimingHeight * 4u;
		const uint64 FXAAP95Increment = FXAA.P95Nanoseconds > Copy.P95Nanoseconds
			? FXAA.P95Nanoseconds - Copy.P95Nanoseconds
			: 0u;
		std::cout
			<< "HDR_DISPLAY_QUALIFICATION status="
			<< (bNamedAdapter ? "named_gate" : "observation")
			<< ",gpu=\"" << DeviceName << "\""
			<< ",driver=" << DeviceProperties.driverVersion
			<< ",vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
			<< '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
			<< '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
			<< ",configuration=Win64-Debug-DurinEditor"
			<< ",warmup_frames=" << WarmupFrames
			<< ",measured_frames=" << MeasuredFrames
			<< ",copy_median_ns=" << Copy.MedianNanoseconds
			<< ",copy_p95_ns=" << Copy.P95Nanoseconds
			<< ",fxaa_median_ns=" << FXAA.MedianNanoseconds
			<< ",fxaa_p95_ns=" << FXAA.P95Nanoseconds
			<< ",fxaa_p95_increment_ns=" << FXAAP95Increment
			<< ",retained_target_bytes=" << SceneTargetBytes
			<< ",output_bytes=" << OutputBytes << '\n';

		EXPECT_GT(Copy.MedianNanoseconds, 0u);
		EXPECT_GT(FXAA.MedianNanoseconds, 0u);
		EXPECT_GE(FXAA.MedianNanoseconds, Copy.MedianNanoseconds);
		if (bNamedAdapter)
		{
			EXPECT_LE(Copy.MedianNanoseconds, 30'000u);
			EXPECT_LE(Copy.P95Nanoseconds, 40'000u);
			EXPECT_LE(FXAA.MedianNanoseconds, 100'000u);
			EXPECT_LE(FXAA.P95Nanoseconds, 120'000u);
			EXPECT_LE(FXAAP95Increment, 90'000u);
		}

		RendererLifecycle.Shutdown();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}
} // namespace Durin
