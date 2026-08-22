#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Renderers/VolumetricCloudRenderer.h"
#include "Renderers/SceneViewState.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include <vulkan/vulkan.hpp>
#include "VulkanDynamicRHI.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace Durin
{
	namespace
	{
		constexpr uint32 WarmupFrames = 30;
		constexpr uint32 MeasuredFrames = 120;
		constexpr uint32 TierWarmupFrames = 12;
		constexpr uint32 TierMeasuredFrames = 48;

		struct FExtentFixture
		{
			uint32 Width;
			uint32 Height;
			uint32 ViewportX;
			uint32 ViewportY;
			uint32 ViewportWidth;
			uint32 ViewportHeight;
			ESceneDepthConvention DepthConvention =
				ESceneDepthConvention::ReversedZ;
		};

		constexpr std::array Extents{
			FExtentFixture{1280, 720, 0, 0, 1280, 720},
			FExtentFixture{1919, 1079, 137, 89, 1601, 901},
			FExtentFixture{1920, 1080, 0, 0, 1920, 1080},
			FExtentFixture{1279, 719, 73, 41, 1101, 623,
				ESceneDepthConvention::ForwardZ}
		};

		struct FTimingSummary
		{
			uint64 MedianNanoseconds = 0;
			uint64 P95Nanoseconds = 0;
		};

		struct FRouteProfile
		{
			std::vector<FGPUTimingQueryRHIRef> Queries;
			std::vector<uint8> Pixels;
			FVolumetricCloudRenderer::FExecutionCounters Counters;
			FTimingSummary Timing;
		};

		struct FExtentProfile
		{
			FExtentFixture Extent{};
			FRouteProfile Compute;
			FRouteProfile Fragment;
			uint64 RetainedBytes = 0;
			bool bComplete = false;
			bool bParity = false;
			bool bLowResolutionTemporalComplete = false;
		};

		struct FTierProfile
		{
			FVolumetricCloudRenderer::EQualityTier Tier{};
			FVolumetricCloudRenderer::FExecutionCounters Counters;
			FTimingSummary Timing;
			uint64 RetainedTargetBytes = 0;
			uint64 HistoryBytes = 0;
			uint64 TotalRetainedBytes = 0;
			uint32 HistoryAccepted = 0;
			uint32 HistoryRejected = 0;
			double MeanAbsoluteError = 0.0;
			float MaximumAbsoluteError = 0.0f;
			double OutlierComponentFraction = 0.0;
			bool bComplete = false;
		};

		constexpr std::array QualityTiers{
			FVolumetricCloudRenderer::EQualityTier::Reference,
			FVolumetricCloudRenderer::EQualityTier::Performance,
			FVolumetricCloudRenderer::EQualityTier::High,
			FVolumetricCloudRenderer::EQualityTier::Epic,
		};

		auto GetTierName(FVolumetricCloudRenderer::EQualityTier Tier)
			-> const char*
		{
			switch (Tier)
			{
			case FVolumetricCloudRenderer::EQualityTier::Performance:
				return "Performance";
			case FVolumetricCloudRenderer::EQualityTier::High:
				return "High";
			case FVolumetricCloudRenderer::EQualityTier::Epic:
				return "Epic";
			case FVolumetricCloudRenderer::EQualityTier::Reference:
				return "Reference";
			}
			return "Unknown";
		}

		struct FProfileVolumetricCloud
		{
			static constexpr auto GetName() -> const char*
			{
				return "ProfileVolumetricCloud";
			}
		};

		std::vector<FGPUTimingQueryRHIRef>* GCloudTimingQueries = nullptr;

		auto CaptureCloudTiming(const FGPUTimingQueryRHIRef& Query, FVolumetricCloudRenderer::ERoute Route) -> void
		{
			if (GCloudTimingQueries != nullptr
				&& Route != FVolumetricCloudRenderer::ERoute::Disabled)
			{
				GCloudTimingQueries->push_back(Query);
			}
		}

		auto BuildViewMatrix(const FVector3& Location, const FVector3& Forward)
			-> FMatrix
		{
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward)
			);
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			FMatrix View(1.0);
			View[0][0] = Forward.x;
			View[1][0] = Forward.y;
			View[2][0] = Forward.z;
			View[3][0] = -Math::Dot(Forward, Location);
			View[0][1] = Right.x;
			View[1][1] = Right.y;
			View[2][1] = Right.z;
			View[3][1] = -Math::Dot(Right, Location);
			View[0][2] = Up.x;
			View[1][2] = Up.y;
			View[2][2] = Up.z;
			View[3][2] = -Math::Dot(Up, Location);
			return View;
		}

		auto DecodeHalf(const uint8* Bytes) -> float
		{
			const uint16 Bits = static_cast<uint16>(Bytes[0])
				| (static_cast<uint16>(Bytes[1]) << 8u);
			const uint32 Sign = static_cast<uint32>(Bits & 0x8000u) << 16u;
			const uint32 Exponent = (Bits >> 10u) & 0x1fu;
			uint32 Mantissa = Bits & 0x03ffu;
			uint32 FloatBits = 0;
			if (Exponent == 0)
			{
				if (Mantissa == 0)
				{
					FloatBits = Sign;
				}
				else
				{
					int Shift = 0;
					while ((Mantissa & 0x0400u) == 0)
					{
						Mantissa <<= 1u;
						++Shift;
					}
					Mantissa &= 0x03ffu;
					FloatBits = Sign
						| (static_cast<uint32>(113 - Shift) << 23u)
						| (Mantissa << 13u);
				}
			}
			else if (Exponent == 31)
			{
				FloatBits = Sign | 0x7f800000u | (Mantissa << 13u);
			}
			else
			{
				FloatBits = Sign | ((Exponent + 112u) << 23u)
					| (Mantissa << 13u);
			}
			return std::bit_cast<float>(FloatBits);
		}

		auto MakeDeterministicBytes(uint32 Width, uint32 Height, uint32 Depth, uint8 Seed) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes(
				static_cast<size_t>(Width) * Height * Depth
			);
			for (uint32 Z = 0; Z < Depth; ++Z)
				for (uint32 Y = 0; Y < Height; ++Y)
					for (uint32 X = 0; X < Width; ++X)
					{
						Bytes[(static_cast<size_t>(Z) * Height + Y) * Width + X] = static_cast<uint8>(Seed + X * 3u + Y * 5u + Z * 7u);
					}
			return Bytes;
		}

		auto ResolveTiming(FRHICommandListImmediate& CommandList,
			std::vector<FGPUTimingQueryRHIRef>& Queries,
			uint32 WarmupCount = WarmupFrames,
			uint32 MeasuredCount = MeasuredFrames)
			-> FTimingSummary
		{
			for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
			{
				if (Queries.size() == WarmupCount + MeasuredCount
					&& std::ranges::all_of(Queries, [](const auto& Query) {
						return Query->GetResult().State
							== ERHIGPUTimingResultState::Ready;
					}))
				{
					break;
				}
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			std::vector<uint64> Durations;
			for (size_t Index = WarmupCount; Index < Queries.size(); ++Index)
			{
				const FRHIGPUTimingResult Result = Queries[Index]->GetResult();
				if (Result.State == ERHIGPUTimingResultState::Ready)
					Durations.push_back(Result.DurationNanoseconds);
			}
			std::ranges::sort(Durations);
			const size_t P95Index = MeasuredCount == 0 ? 0
				: std::min<size_t>(MeasuredCount - 1,
					(static_cast<size_t>(MeasuredCount) * 95 + 99) / 100 - 1);
			return Durations.size() == MeasuredCount
				? FTimingSummary{Durations[MeasuredCount / 2], Durations[P95Index]}
				: FTimingSummary{};
		}
	} // namespace

	TEST(FVolumetricCloudQualificationTests, ProfilesFrozenExtentAndRouteMatrix)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(FRHIInitializationContext::Headless());
		ASSERT_NE(GDynamicRHI, nullptr);
		auto* Vulkan = static_cast<VulkanRHI::IVulkanDynamicRHI*>(GDynamicRHI);
		vk::PhysicalDeviceProperties DeviceProperties{};
		Vulkan->RHIExecuteCommandBufferForBackendIntegration(
			[&Vulkan, &DeviceProperties](vk::CommandBuffer) {
				DeviceProperties = Vulkan->RHIGetVkPhysicalDevice().getProperties();
			}
		);
		InitRenderingThread();
		const std::string DeviceName = DeviceProperties.deviceName.data();
		const bool bNamedAdapter = DeviceName == "NVIDIA GeForce RTX 3090"
			&& vk::apiVersionMajor(DeviceProperties.apiVersion) == 1u
			&& vk::apiVersionMinor(DeviceProperties.apiVersion) == 4u
			&& vk::apiVersionPatch(DeviceProperties.apiVersion) == 325u;
		const char* Execution = std::getenv("DURIN_RHI_EXECUTION");
		const std::string ExecutionMode = Execution != nullptr ? Execution : "default";

		FRendererResourceCoordinator Coordinator;
		FFullscreenGeometryResources FullscreenGeometry;
		FVolumetricCloudRenderer Clouds(Coordinator, FullscreenGeometry);
		auto Profiles = std::make_shared<std::array<FExtentProfile, Extents.size()>>();
		auto TierProfiles =
			std::make_shared<std::array<FTierProfile, QualityTiers.size()>>();

		EnqueueRenderCommand<FProfileVolumetricCloud>(
			[&Clouds, &Coordinator, &FullscreenGeometry, Profiles, TierProfiles](
				FRHICommandListImmediate& CommandList
			) {
				auto MakeVolume = [&CommandList](const char* Name, uint32 Size, uint8 Seed) {
					const std::vector<uint8> Bytes =
						MakeDeterministicBytes(Size, Size, Size, Seed);
					FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create3D(Name)
										 .SetExtent(Size, Size)
										 .SetDepth(Size)
										 .SetFormat(EPixelFormat::R8_UNORM)
										 .SetFlags(ETextureCreateFlags::ShaderResource)
					);
					if (Texture)
					{
						GDynamicRHI->RHIUpdateTexture3D(CommandList, Texture, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, Size, Size, Size), Size, Size * Size, Bytes.data());
					}
					return Texture;
				};

				FTextureRHIRef Base = MakeVolume("CloudQualificationBase", 64, 11);
				FTextureRHIRef Detail = MakeVolume("CloudQualificationDetail", 32, 29);
				const std::vector<uint8> WeatherBytes =
					MakeDeterministicBytes(64, 64, 1, 47);
				FTextureRHIRef Weather = GDynamicRHI->RHICreateTexture(
					CommandList, FRHITextureCreateDesc::Create2D(
									 "CloudQualificationWeather", 64, 64,
									 EPixelFormat::R8_UNORM
								 )
									 .SetFlags(ETextureCreateFlags::ShaderResource)
				);
				if (Weather)
				{
					GDynamicRHI->RHIUpdateTexture2D(CommandList, Weather, 0, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 64, 64), 64, WeatherBytes.data());
				}
				FSamplerRHIRef Sampler = RHICreateSampler(FRHISamplerDesc::LinearRepeat());
				if (!Base || !Detail || !Weather || !Sampler) return;

				for (size_t ExtentIndex = 0; ExtentIndex < Extents.size(); ++ExtentIndex)
				{
					FExtentProfile& Profile = (*Profiles)[ExtentIndex];
					Profile.Extent = Extents[ExtentIndex];
					const FExtentFixture& Extent = Profile.Extent;
					auto* FragmentTargets = Clouds.EnsureTargets_RenderThread(
						Extent.Width, Extent.Height
					);
					auto* ComputeTargets = Clouds.EnsureComputeTargets_RenderThread(
						Extent.Width, Extent.Height
					);
					FTextureRHIRef Depth = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
										 "CloudQualificationDepth", Extent.Width, Extent.Height,
										 EPixelFormat::D32
									 )
										 .SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::ShaderResource)
					);
					if (!FragmentTargets || !ComputeTargets || !Depth) continue;

					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FRHIRenderPassInfo DepthPass{};
					DepthPass.RenderTargetLayout =
						RenderTargetLayouts::MakeDirectionalShadowDepth();
					DepthPass.DepthStencilRenderTarget = Depth;
					DepthPass.DepthStencilClearValue = FClearValueBinding(
						Extent.DepthConvention == ESceneDepthConvention::ReversedZ
							? 0.0f : 1.0f, 0u);
					CommandList.BeginRenderPass(
						DepthPass, "CloudQualificationDepthClear"
					);
					CommandList.EndRenderPass();
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);

					FSceneView View;
					View.ViewLocation = {0.0, 0.0, 2500.0};
					const FVector3 Forward =
						Math::Normalize(FVector3(1.0, 0.0, 0.1));
					View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
					if (!SceneViewProjection::BuildPerspectiveProjection(
							60.0, static_cast<double>(Extent.ViewportWidth) / Extent.ViewportHeight,
							0.1, 500000.0, Extent.DepthConvention,
							View.ProjectionMatrix
						))
					{
						continue;
					}
					View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
					View.ViewportX = Extent.ViewportX;
					View.ViewportY = Extent.ViewportY;
					View.ViewportWidth = Extent.ViewportWidth;
					View.ViewportHeight = Extent.ViewportHeight;
					View.DepthConvention = Extent.DepthConvention;
					const FVolumetricCloudRenderer::FRenderInput Input{
						.bRequested = true,
						.Textures = {
							.BaseDensity = Base,
							.DetailDensity = Detail,
							.Weather = Weather,
							.SceneDepth = Depth,
							.DensitySampler = Sampler
						},
						.View = &View,
						.QualityTier = FVolumetricCloudRenderer::EQualityTier::Reference,
						.Width = Extent.Width,
						.Height = Extent.Height
					};

					auto RunRoute = [&](FRouteProfile& Route,
										FVolumetricCloudRenderer::FComputeTargets* SelectedCompute) {
						GCloudTimingQueries = &Route.Queries;
						FVolumetricCloudRenderer::SetTimingQuerySink(CaptureCloudTiming);
						for (uint32 Frame = 0;
							Frame < WarmupFrames + MeasuredFrames; ++Frame)
						{
							++GRenderFrameCounterRenderThread;
							GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
							const auto Result = Clouds.Render_RenderThread(
								CommandList, FragmentTargets, SelectedCompute, Input
							);
							Route.Counters = Result.Counters;
							GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
						}
						FVolumetricCloudRenderer::SetTimingQuerySink(nullptr);
						GCloudTimingQueries = nullptr;
						Route.Timing = ResolveTiming(CommandList, Route.Queries);
						Route.Queries.clear();
						FRHITexture* Output = SelectedCompute != nullptr ? SelectedCompute->Cloud.GetReference() : FragmentTargets->Cloud.GetReference();
						GDynamicRHI->RHIReadTexture2D(
							CommandList, Output, 0, 0, Route.Pixels
						);
					};

					RunRoute(Profile.Fragment, nullptr);
					RunRoute(Profile.Compute, ComputeTargets);
					Profile.bParity = Profile.Compute.Pixels.size()
						== Profile.Fragment.Pixels.size()
						&& !Profile.Compute.Pixels.empty();
					for (size_t Offset = 0;
						Profile.bParity && Offset + 1 < Profile.Compute.Pixels.size();
						Offset += 2)
					{
						Profile.bParity = std::abs(
											  DecodeHalf(Profile.Compute.Pixels.data() + Offset)
											  - DecodeHalf(Profile.Fragment.Pixels.data() + Offset)
										  )
										  <= 2.0f / 1024.0f;
					}

					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FTextureRHIRef SceneColor = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
										 "CloudQualificationScene", Extent.Width, Extent.Height,
										 EPixelFormat::RGBA16_FLOAT
									 )
										 .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
					);
					if (SceneColor)
					{
						FRHIRenderPassInfo SceneClear{};
						SceneClear.RenderTargetLayout =
							RenderTargetLayouts::MakeVolumetricCloudOutput();
						SceneClear.ColorRenderTargets[0] = SceneColor;
						SceneClear.ColorClearValues[0] =
							FClearValueBinding(0.25f, 0.5f, 0.75f, 1.0f);
						CommandList.BeginRenderPass(
							SceneClear, "CloudQualificationSceneClear"
						);
						CommandList.EndRenderPass();
					}
					FRHITexture* Composite = SceneColor ? Clouds.Composite_RenderThread(CommandList, SceneColor, ComputeTargets->Cloud, Depth, View) : nullptr;
					if (ExtentIndex == 0 && SceneColor)
					{
						constexpr auto Tier =
							FVolumetricCloudRenderer::EQualityTier::High;
						const auto Policy =
							FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(Tier);
						const auto CloudExtent =
							FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
								Extent.Width, Extent.Height, Policy
							);
						auto* LowFragment = Clouds.EnsureTargets_RenderThread(
							CloudExtent.Width, CloudExtent.Height
						);
						auto* LowCompute = Clouds.EnsureComputeTargets_RenderThread(
							CloudExtent.Width, CloudExtent.Height
						);
						FSceneViewState ViewState;
						const auto Metadata = BuildSceneViewTemporalMetadata(
							View, nullptr, Extent.Width, Extent.Height
						);
						auto RunTemporalFrame = [&](uint64 Serial) {
							const auto Context = ViewState.Begin(
								Metadata, Serial, false
							);
							FVolumetricCloudRenderer::FRenderInput LowInput = Input;
							LowInput.QualityTier = Tier;
							LowInput.SuccessfulSequence = Context.SuccessfulSequence;
							LowInput.Width = CloudExtent.Width;
							LowInput.Height = CloudExtent.Height;
							LowInput.OutputWidth = Extent.Width;
							LowInput.OutputHeight = Extent.Height;
							const auto Spatial = Clouds.Render_RenderThread(
								CommandList, LowFragment, LowCompute, LowInput
							);
							const auto Temporal = Clouds.ReconstructTemporal_RenderThread(
								CommandList, {.CurrentCloud = Spatial.Cloud,
											  .View = &View,
											  .TemporalContext = &Context,
											  .ViewState = &ViewState,
											  .Parameters = LowInput.Parameters,
											  .QualityTier = Tier,
											  .CloudHistoryKey = 77}
							);
							FRHITexture* LowComposite =
								Clouds.Composite_RenderThread(CommandList, SceneColor, Temporal.Cloud, Depth, View);
							const bool bComplete = Spatial.Cloud != nullptr
												   && Temporal.Cloud != nullptr
												   && Temporal.bCandidatePublished
												   && LowComposite != nullptr
												   && (Serial == 1 || Temporal.bHistoryAccepted);
							ViewState.Commit();
							return bComplete;
						};
						Profile.bLowResolutionTemporalComplete =
							LowFragment != nullptr && LowCompute != nullptr
							&& RunTemporalFrame(1) && RunTemporalFrame(2);
					}
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					Profile.RetainedBytes =
						Clouds.GetRetainedTargetBytes_RenderThread();
					Profile.bComplete = Composite != nullptr
						&& Profile.Compute.Timing.MedianNanoseconds > 0u
						&& Profile.Fragment.Timing.MedianNanoseconds > 0u;
				}

				Clouds.ReleaseResources_RenderThread();

				// P3 A/B gate: time and compare the complete spatial + temporal +
				// full-resolution composite route at the frozen 4K output extent.
				constexpr uint32 OutputWidth = 3840;
				constexpr uint32 OutputHeight = 2160;
				FTextureRHIRef QualificationDepth = GDynamicRHI->RHICreateTexture(
					CommandList, FRHITextureCreateDesc::Create2D(
						"CloudTierQualificationDepth", OutputWidth, OutputHeight,
						EPixelFormat::D32)
						.SetFlags(ETextureCreateFlags::DepthStencilTargetable
							| ETextureCreateFlags::ShaderResource));
				std::vector<std::vector<uint8>> ReferenceFrames;
				ReferenceFrames.reserve(6);
				if (QualificationDepth)
				{
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FRHIRenderPassInfo DepthPass{};
					DepthPass.RenderTargetLayout =
						RenderTargetLayouts::MakeDirectionalShadowDepth();
					DepthPass.DepthStencilRenderTarget = QualificationDepth;
					DepthPass.DepthStencilClearValue = FClearValueBinding(0.0f, 0u);
					CommandList.BeginRenderPass(
						DepthPass, "CloudTierQualificationDepthClear");
					CommandList.EndRenderPass();
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}

				auto BuildQualificationView = [](uint32 Frame) {
					FSceneView View;
					View.ViewLocation = Frame == 3 ? FVector3(12.0, -4.0, 2500.0)
						: FVector3(0.0, 0.0, 2500.0);
					const FVector3 Forward = Math::Normalize(
						Frame == 4 ? FVector3(1.0, 0.06, 0.1)
							: FVector3(1.0, 0.0, 0.1));
					View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
					(void)SceneViewProjection::BuildPerspectiveProjection(
						60.0, static_cast<double>(OutputWidth) / OutputHeight,
						0.1, 500000.0, ESceneDepthConvention::ReversedZ,
						View.ProjectionMatrix);
					View.ViewProjectionMatrix =
						View.ProjectionMatrix * View.ViewMatrix;
					View.ViewportWidth = OutputWidth;
					View.ViewportHeight = OutputHeight;
					View.DepthConvention = ESceneDepthConvention::ReversedZ;
					return View;
				};

				for (size_t TierIndex = 0; TierIndex < QualityTiers.size();
					++TierIndex)
				{
					FTierProfile& Profile = (*TierProfiles)[TierIndex];
					Profile.Tier = QualityTiers[TierIndex];
					const auto Policy =
						FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(Profile.Tier);
					const auto CloudExtent =
						FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
							OutputWidth, OutputHeight, Policy);
					FVolumetricCloudRenderer TierClouds(
						Coordinator, FullscreenGeometry);
					auto* FragmentTargets = TierClouds.EnsureTargets_RenderThread(
						CloudExtent.Width, CloudExtent.Height);
					auto* ComputeTargets = TierClouds.EnsureComputeTargets_RenderThread(
						CloudExtent.Width, CloudExtent.Height);
					FTextureRHIRef SceneColor = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
							"CloudTierQualificationScene", OutputWidth, OutputHeight,
							EPixelFormat::RGBA16_FLOAT)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource));
					FSamplerRHIRef TierSampler = TierClouds.EnsureDensitySampler_RenderThread();
					Profile.bComplete = QualificationDepth && FragmentTargets
						&& ComputeTargets && SceneColor && TierSampler;

					auto ClearScene = [&]() {
						FRHIRenderPassInfo ScenePass{};
						ScenePass.RenderTargetLayout =
							RenderTargetLayouts::MakeVolumetricCloudOutput();
						ScenePass.ColorRenderTargets[0] = SceneColor;
						ScenePass.ColorClearValues[0] =
							FClearValueBinding(0.25f, 0.5f, 0.75f, 1.0f);
						CommandList.BeginRenderPass(
							ScenePass, "CloudTierQualificationSceneClear");
						CommandList.EndRenderPass();
					};

					auto RenderFrame = [&](FSceneViewState& ViewState,
						const FSceneView& View, uint64 Serial, bool bCameraCut,
						FGPUTimingQueryRHIRef TimingQuery)
						-> std::pair<FRHITexture*, FVolumetricCloudRenderer::FTemporalReconstructionResult>
					{
						ClearScene();
						const auto Metadata = BuildSceneViewTemporalMetadata(
							View, nullptr, OutputWidth, OutputHeight);
						const auto Context = ViewState.Begin(
							Metadata, Serial, bCameraCut);
						if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
						const auto Spatial = TierClouds.Render_RenderThread(
							CommandList, FragmentTargets, ComputeTargets,
							{.bRequested = true,
							 .Textures = {.BaseDensity = Base,
								.DetailDensity = Detail,
								.Weather = Weather,
								.SceneDepth = QualificationDepth,
								.DensitySampler = TierSampler},
							 .View = &View,
							 .QualityTier = Profile.Tier,
							 .SuccessfulSequence = Context.SuccessfulSequence,
							 .Width = CloudExtent.Width,
							 .Height = CloudExtent.Height,
							 .OutputWidth = OutputWidth,
							 .OutputHeight = OutputHeight});
						const auto Temporal = TierClouds.ReconstructTemporal_RenderThread(
							CommandList,
							{.CurrentCloud = Spatial.Cloud,
							 .View = &View,
							 .TemporalContext = &Context,
							 .ViewState = &ViewState,
							 .QualityTier = Profile.Tier,
							 .CloudHistoryKey = 0xC10D});
						FRHITexture* Composite = TierClouds.Composite_RenderThread(
							CommandList, SceneColor, Temporal.Cloud,
							QualificationDepth, View);
						if (TimingQuery) CommandList.EndGPUTimingQuery(TimingQuery);
						Profile.Counters = Spatial.Counters;
						if (Spatial.Cloud && Temporal.Cloud
							&& Temporal.bCandidatePublished && Composite)
							ViewState.Commit();
						else
						{
							ViewState.Abort();
							Profile.bComplete = false;
						}
						return {Composite, Temporal};
					};

					std::vector<FGPUTimingQueryRHIRef> TierQueries;
					TierQueries.reserve(TierWarmupFrames + TierMeasuredFrames);
					FSceneViewState TimingState;
					for (uint32 Frame = 0;
						Frame < TierWarmupFrames + TierMeasuredFrames; ++Frame)
					{
						++GRenderFrameCounterRenderThread;
						GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						FGPUTimingQueryRHIRef Query =
							GDynamicRHI->RHICreateGPUTimingQuery();
						if (!Query) Profile.bComplete = false;
						TierQueries.push_back(Query);
						(void)RenderFrame(TimingState, BuildQualificationView(0),
							Frame + 1, false, Query);
						GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
					Profile.Timing = ResolveTiming(CommandList, TierQueries,
						TierWarmupFrames, TierMeasuredFrames);

					FSceneViewState SequenceState;
					double AbsoluteError = 0.0;
					uint64 ComparedComponents = 0;
					uint64 OutlierComponents = 0;
					const float OutlierThreshold = Profile.Tier
						== FVolumetricCloudRenderer::EQualityTier::Performance
						? 0.35f : Profile.Tier
						== FVolumetricCloudRenderer::EQualityTier::High ? 0.30f : 0.25f;
					for (uint32 Frame = 0; Frame < 6; ++Frame)
					{
						++GRenderFrameCounterRenderThread;
						GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						auto [Composite, Temporal] = RenderFrame(
							SequenceState, BuildQualificationView(Frame),
							Frame + 1, Frame == 5, nullptr);
						if (Temporal.bHistoryAccepted)
							++Profile.HistoryAccepted;
						else
							++Profile.HistoryRejected;
						std::vector<uint8> Pixels;
						if (Composite)
							GDynamicRHI->RHIReadTexture2D(
								CommandList, Composite, 0, 0, Pixels);
						GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
						if (Profile.Tier
							== FVolumetricCloudRenderer::EQualityTier::Reference)
						{
							ReferenceFrames.push_back(std::move(Pixels));
						}
						else if (Frame < ReferenceFrames.size()
							&& Pixels.size() == ReferenceFrames[Frame].size())
						{
							for (size_t Pixel = 0; Pixel + 7 < Pixels.size(); Pixel += 8)
								for (size_t Channel = 0; Channel < 3; ++Channel)
								{
									const float Error = std::abs(
										DecodeHalf(Pixels.data() + Pixel + Channel * 2)
										- DecodeHalf(ReferenceFrames[Frame].data()
											+ Pixel + Channel * 2));
									AbsoluteError += Error;
									Profile.MaximumAbsoluteError = std::max(
										Profile.MaximumAbsoluteError, Error);
									if (Error > OutlierThreshold)
										++OutlierComponents;
									++ComparedComponents;
								}
						}
						else if (Profile.Tier
							!= FVolumetricCloudRenderer::EQualityTier::Reference)
						{
							Profile.bComplete = false;
						}
					}
					Profile.MeanAbsoluteError = ComparedComponents > 0
						? AbsoluteError / static_cast<double>(ComparedComponents)
						: 0.0;
					Profile.OutlierComponentFraction = ComparedComponents > 0
						? static_cast<double>(OutlierComponents)
							/ static_cast<double>(ComparedComponents)
						: 0.0;
					Profile.RetainedTargetBytes =
						TierClouds.GetRetainedTargetBytes_RenderThread();
					Profile.HistoryBytes =
						SequenceState.GetVolumetricCloudHistory().GetRetainedBytes();
					Profile.TotalRetainedBytes = Profile.RetainedTargetBytes
						+ Profile.HistoryBytes;
					Profile.bComplete = Profile.bComplete
						&& Profile.Timing.MedianNanoseconds > 0
						&& ReferenceFrames.size() == 6;
					TierClouds.ReleaseResources_RenderThread();
				}
				FullscreenGeometry.ReleaseResources_RenderThread();
			}
		);
		FlushRenderingCommands();

		for (FExtentProfile& Profile : *Profiles)
		{
			const FTimingSummary Compute = Profile.Compute.Timing;
			const FTimingSummary Fragment = Profile.Fragment.Timing;
			const bool bIsTimingGate = bNamedAdapter
				&& Profile.Extent.Width == 1920 && Profile.Extent.Height == 1080;
			std::cout << "VOLUMETRIC_CLOUD_QUALIFICATION"
				<< " status=" << (bNamedAdapter ? "named_gate" : "observation")
				<< ",gpu=\"" << DeviceName << "\""
				<< ",vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
				<< '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
				<< '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
				<< ",driver=" << DeviceProperties.driverVersion
				<< ",configuration=Win64-Debug-DurinEditor"
				<< ",execution=" << ExecutionMode
				<< ",extent=" << Profile.Extent.Width << 'x' << Profile.Extent.Height
				<< ",viewport=" << Profile.Extent.ViewportX << ':'
				<< Profile.Extent.ViewportY << ':'
				<< Profile.Extent.ViewportWidth << 'x'
				<< Profile.Extent.ViewportHeight
				<< ",depth=" << (Profile.Extent.DepthConvention
					== ESceneDepthConvention::ReversedZ ? "reversed" : "forward")
				<< ",warmup_frames=" << WarmupFrames
				<< ",measured_frames=" << MeasuredFrames
				<< ",compute_median_ns=" << Compute.MedianNanoseconds
				<< ",compute_p95_ns=" << Compute.P95Nanoseconds
				<< ",fragment_median_ns=" << Fragment.MedianNanoseconds
				<< ",fragment_p95_ns=" << Fragment.P95Nanoseconds
				<< ",retained_target_bytes=" << Profile.RetainedBytes
				<< ",parity=" << (Profile.bParity ? "pass" : "fail") << '\n';

			EXPECT_TRUE(Profile.bComplete);
			EXPECT_TRUE(Profile.bParity);
			if (Profile.Extent.Width == Extents[0].Width
				&& Profile.Extent.Height == Extents[0].Height)
			{
				EXPECT_TRUE(Profile.bLowResolutionTemporalComplete);
			}
			EXPECT_GT(Compute.MedianNanoseconds, 0u);
			EXPECT_GT(Fragment.MedianNanoseconds, 0u);
			EXPECT_EQ(Profile.Compute.Counters.Route, FVolumetricCloudRenderer::ERoute::Compute);
			EXPECT_EQ(Profile.Compute.Counters.Dispatches, 1u);
			EXPECT_EQ(Profile.Compute.Counters.Draws, 0u);
			EXPECT_EQ(Profile.Compute.Counters.Copies, 0u);
			EXPECT_EQ(Profile.Compute.Counters.GroupCountX, FVolumetricCloudSpatialRenderer::CalculateGroupCount(Profile.Extent.Width));
			EXPECT_EQ(Profile.Compute.Counters.GroupCountY, FVolumetricCloudSpatialRenderer::CalculateGroupCount(Profile.Extent.Height));
			EXPECT_EQ(Profile.Fragment.Counters.Route, FVolumetricCloudRenderer::ERoute::Fragment);
			EXPECT_EQ(Profile.Fragment.Counters.Dispatches, 0u);
			EXPECT_EQ(Profile.Fragment.Counters.Draws, 1u);
			EXPECT_EQ(Profile.Fragment.Counters.Copies, 0u);
			const uint64 ViewPixels =
				static_cast<uint64>(Profile.Extent.ViewportWidth)
					* Profile.Extent.ViewportHeight;
			const uint64 PrimarySamples = ViewPixels
				* FVolumetricCloudSpatialRenderer::MaximumPrimarySamples;
			const uint64 LightSamples = PrimarySamples
				* FVolumetricCloudSpatialRenderer::MaximumLightSamples;
			for (const FRouteProfile* Route :
				{&Profile.Compute, &Profile.Fragment})
			{
				EXPECT_EQ(Route->Counters.PrimarySamples, PrimarySamples);
				EXPECT_EQ(Route->Counters.LightSamples, LightSamples);
				EXPECT_EQ(Route->Counters.TargetBytes, FVolumetricCloudSpatialRenderer::CalculateTargetBytes(Profile.Extent.Width, Profile.Extent.Height));
			}
			EXPECT_LE(Profile.RetainedBytes, FVolumetricCloudSpatialRenderer::MaximumRetainedTargetBytes);
			if (bIsTimingGate)
			{
				EXPECT_LE(Compute.MedianNanoseconds, 12'000'000u);
				EXPECT_LE(Compute.P95Nanoseconds, 16'000'000u);
				EXPECT_LE(Fragment.MedianNanoseconds, Compute.MedianNanoseconds * 3u / 2u);
			}
		}

		const auto FindTier = [&](FVolumetricCloudRenderer::EQualityTier Tier)
			-> const FTierProfile& {
			return *std::ranges::find_if(*TierProfiles,
				[Tier](const FTierProfile& Profile) {
					return Profile.Tier == Tier;
				});
		};
		const FTierProfile& Reference = FindTier(
			FVolumetricCloudRenderer::EQualityTier::Reference);
		const FTierProfile& High = FindTier(
			FVolumetricCloudRenderer::EQualityTier::High);
		for (const FTierProfile& Profile : *TierProfiles)
		{
			const auto Policy =
				FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(Profile.Tier);
			const auto ExpectedExtent =
				FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
					3840, 2160, Policy);
			std::cout << "VOLUMETRIC_CLOUD_TEMPORAL_QUALIFICATION"
				<< " status=" << (bNamedAdapter ? "named_gate" : "observation")
				<< ",gpu=\"" << DeviceName << "\""
				<< ",vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
				<< '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
				<< '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
				<< ",execution=" << ExecutionMode
				<< ",tier=" << GetTierName(Profile.Tier)
				<< ",output=3840x2160"
				<< ",target=" << Profile.Counters.TargetWidth << 'x'
				<< Profile.Counters.TargetHeight
				<< ",primary_samples=" << Profile.Counters.PrimarySamples
				<< ",light_samples=" << Profile.Counters.LightSamples
				<< ",route_median_ns=" << Profile.Timing.MedianNanoseconds
				<< ",route_p95_ns=" << Profile.Timing.P95Nanoseconds
				<< ",target_bytes=" << Profile.Counters.TargetBytes
				<< ",history_bytes=" << Profile.HistoryBytes
				<< ",renderer_retained_bytes=" << Profile.RetainedTargetBytes
				<< ",total_retained_bytes=" << Profile.TotalRetainedBytes
				<< ",history_accepted=" << Profile.HistoryAccepted
				<< ",history_rejected=" << Profile.HistoryRejected
				<< ",mean_absolute_error=" << Profile.MeanAbsoluteError
				<< ",maximum_absolute_error=" << Profile.MaximumAbsoluteError
				<< ",outlier_component_fraction="
				<< Profile.OutlierComponentFraction
				<< '\n';

			EXPECT_TRUE(Profile.bComplete);
			EXPECT_EQ(Profile.Counters.Route,
				FVolumetricCloudRenderer::ERoute::Compute);
			EXPECT_EQ(Profile.Counters.TargetWidth, ExpectedExtent.Width);
			EXPECT_EQ(Profile.Counters.TargetHeight, ExpectedExtent.Height);
			EXPECT_EQ(Profile.Counters.OutputWidth, 3840u);
			EXPECT_EQ(Profile.Counters.OutputHeight, 2160u);
			EXPECT_EQ(Profile.Counters.PrimarySamples,
				static_cast<uint64>(ExpectedExtent.Width) * ExpectedExtent.Height
					* Policy.PrimarySampleCount);
			EXPECT_EQ(Profile.Counters.LightSamples,
				Profile.Counters.PrimarySamples * Policy.LightSampleCount);
			EXPECT_EQ(Profile.Counters.TargetBytes,
				FVolumetricCloudSpatialRenderer::CalculateTargetBytes(
					ExpectedExtent.Width, ExpectedExtent.Height));
			EXPECT_GT(Profile.Timing.MedianNanoseconds, 0u);
			EXPECT_GT(Profile.Timing.P95Nanoseconds, 0u);
			EXPECT_LE(Profile.TotalRetainedBytes,
				FVolumetricCloudSpatialRenderer::MaximumRetainedTargetBytes);

			if (Policy.IsFullResolution())
			{
				EXPECT_EQ(Profile.HistoryAccepted, 0u);
				EXPECT_EQ(Profile.HistoryBytes, 0u);
				EXPECT_DOUBLE_EQ(Profile.MeanAbsoluteError, 0.0);
				EXPECT_FLOAT_EQ(Profile.MaximumAbsoluteError, 0.0f);
				EXPECT_DOUBLE_EQ(Profile.OutlierComponentFraction, 0.0);
			}
			else
			{
				EXPECT_EQ(Profile.HistoryAccepted, 4u);
				EXPECT_EQ(Profile.HistoryRejected, 2u);
				EXPECT_LE(Profile.HistoryBytes, 32ull * 1024ull * 1024ull);
				const double MeanThreshold =
					Profile.Tier == FVolumetricCloudRenderer::EQualityTier::Performance
						? 0.08 : Profile.Tier
						== FVolumetricCloudRenderer::EQualityTier::High ? 0.06 : 0.04;
				EXPECT_LE(Profile.MeanAbsoluteError, MeanThreshold);
				const double OutlierFractionThreshold = Profile.Tier
					== FVolumetricCloudRenderer::EQualityTier::Performance
					? 0.05 : Profile.Tier
					== FVolumetricCloudRenderer::EQualityTier::High ? 0.01 : 0.005;
				EXPECT_LE(Profile.OutlierComponentFraction,
					OutlierFractionThreshold);
			}

			if (bNamedAdapter)
			{
				const uint64 MedianBudget = Policy.IsFullResolution()
					? 48'000'000u : Profile.Tier
					== FVolumetricCloudRenderer::EQualityTier::Performance
					? 16'000'000u : Profile.Tier
					== FVolumetricCloudRenderer::EQualityTier::High
					? 20'000'000u : 24'000'000u;
				EXPECT_LE(Profile.Timing.MedianNanoseconds, MedianBudget);
				EXPECT_LE(Profile.Timing.P95Nanoseconds,
					MedianBudget * 4u / 3u);
			}
		}
		if (bNamedAdapter)
		{
			EXPECT_LE(High.Timing.MedianNanoseconds,
				Reference.Timing.MedianNanoseconds * 4u / 5u);
		}

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}
} // namespace Durin
